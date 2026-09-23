# aquamoto.jl — Geophysics > Tsunamis > "Aquamoto viewer…" (port of Mirone's aquamoto.m + its
# netCDF support half aqua_suppfuns.m — NETCDF TAB ONLY, the first tab of aquamoto.ui). The target
# file class is NSWING's own single 3-D netCDF output (`-G<stem>,<int>`, no `+m`): a static 2-D
# `bathymetry` variable + a time-varying 3-D quantity variable (usually "z"), read the SAME way
# any other 3-D netCDF cube is read in this app (`file.nc?var[i]`, see drop.jl). Only "bathymetry"
# is a known/guaranteed name -- the time-varying variable's own name is never assumed.
#
# The whole point of the tool (aqua_suppfuns.m's IamTSU branch, coards_sliceShow/do_imgWater/
# do_imgBat/mixe_images): ocean wave height and dry-land elevation live on very different scales,
# so they are coloured SEPARATELY and blended only at render time, never on one shared colour scale.
# Per requested slice k: indLand = abs(bathymetry - stage) < tol (cells where the water level equals
# the sea floor -> no water on top -> dry); the land side is coloured from the (cached) bathymetry,
# the wet side from the (possibly clamped) stage, cross-blended by the Water-transparency slider and
# then the land pixels are HARD-overwritten with the land colour (mixe_images) so land always reads
# as land regardless of the transparency slider.
#
# ANUGA's .sww triangulated-mesh path (Show mesh, the 14-formula get_derivedVar, vector/momentum
# plotting) has no VTK/GMT triangulated-mesh equivalent on hand and is OUT OF SCOPE this pass — the
# .ui ships Show-mesh + Derived var disabled and they stay that way. The Primary-quantities picker
# itself (Stage/Xmoment/Ymoment/Or…) IS in scope: every time-varying quantity variable found in the
# opened nc file is loadable and switchable as the ACTIVE one (`_aqua_find_all_varnames`,
# `_aquamoto_set_var`) — loading only the first match and silently discarding the rest was a bug,
# not the intended scope cut. Shading/illumination is the separate "Shading OR Image" tab (also out
# of scope): this pass paints flat, unshaded colour.
#
# Every call comes from the C++ AquamotoWindow (75_aquamoto.cpp) through the generic console-eval
# bridge (g_juliaEval / juliaEvalCall — the SAME synchronous round-trip NswingDialog already uses
# for its own small queries), keyed to the caller's own live viewer window (`scene`). No new
# @cfunction/registration is needed for that: only the composited-texture push
# (gmtvtk_show_layer_rgba_h) is a new C export (see 90_c_api.cpp).

# Per-variable min/max scan result -- one of these per varname, ALL built up front at open time so
# every variable in the file is actually loaded, not just the active one. `alllo`/`allhi` are NOT
# scanned -- they come straight from the per-layer GMTgrid's own `.range` (GMT already computes a
# grid's z min/max when it reads it; recomputing that with a manual Julia loop is pure waste).
# `wetlo`/`wethi` genuinely need a scan (the wet/dry split is a per-cell comparison against
# bathymetry that no metadata carries); `wetany` flags a layer that had at least one wet cell.
struct _AquaVarScan
	wetlo::Vector{Float64}                  # per-layer WET-cell min (meaningless where wetany is false)
	wethi::Vector{Float64}                  # per-layer WET-cell max (meaningless where wetany is false)
	wetany::Vector{Bool}                    # true iff that layer had at least one wet cell
	alllo::Vector{Float64}                  # per-layer min over EVERY cell (land included) -- from .range
	allhi::Vector{Float64}                  # per-layer max over EVERY cell (land included) -- from .range
end

mutable struct _AquaState
	path::String                            # the netCDF file
	varname::String                         # ACTIVE time-varying quantity variable name (whatever the file calls it)
	varnames::Vector{String}                # EVERY time-varying quantity variable found in the file
	scans::Dict{String,_AquaVarScan}        # ALL varnames' scans, built at open time -- every
	                                        # variable is loaded up front, switching is a lookup
	bat::GMTgrid{Float32,2}                 # bathymetry grid, read once
	nsteps::Int
	geog::Bool
	imgbat::Array{UInt8,3}                  # cached land RGB (ny,nx,3); EMPTY (size 0) = not built yet
	first::Bool                             # true until the first slice has been shown (Save/Session bookkeeping)
	watercmap::Symbol                       # user-selectable via "Color Bar water" (default :polar)
	landcmap::Symbol                        # user-selectable via "Color Bar Land" (default :geo)
	# …and the SAME row's "Color Palettes…" editor, which hands over an ARBITRARY palette rather than
	# the name of one: its rows plus the z each row sits at (log spacing included). EMPTY = that side
	# is still on its named colormap. TWO INSTANCES OF ONE THING, indexed by the side code the viewer
	# speaks (0 water / 1 land, +1 for Julia), exactly like `illum` below.
	cpt::NTuple{2,Tuple{Vector{Float64},Vector{Float64}}}   # (cz, crgb) per side; crgb is flat, 3 per node
	cur::Int                                # 0-based index of the slice on screen (the time slider)
	illum::NTuple{2,Dict{String,String}}    # the Illumination dialog's params, ONE PER SIDE:
	                                        # illum[1] = WATER (the live stage), illum[2] = LAND (the
	                                        # static bathymetry). EMPTY = that side has no model loaded.
	                                        # TWO INSTANCES OF ONE THING, never two differently-named
	                                        # fields (SACRED_LAW.md, two-surface illumination law): the
	                                        # sides are indexed by the very `side` code the viewer and
	                                        # `_hs_push` speak (0 water / 1 land, +1 for Julia).
	                                        # Kept because the WATER side stands on a surface that
	                                        # changes at every timestep, so its reflectance has to be
	                                        # rebuilt per slice by `_aqua_shaded_rgb`, the land half with
	                                        # it — the bathymetry is static but the SHORELINE is not, so
	                                        # the half it is masked down to changes at every timestep.
	# The display options the DIALOG last drew a slice with, recorded by `_aquamoto_slice` itself.
	# A programmatic slice change (`set_layer!`, movie.jl) reads them back and calls THE SAME slice
	# function with them, so an animation looks exactly like the slice the user is looking at instead
	# of inventing a second set of defaults. Same operation, same function -- see SACRED_LAW.md.
	split::Bool                             # "Split Dry/Wet"
	globalmm::Bool                          # "Scale colour to global min/max"
	transp::Float64                         # water transparency, 0..1
	shadewater::Bool                        # the two shading radio buttons
	shadeland::Bool
	times::Vector{Float64}                  # the cube's own `time` coordinate, one per step (EMPTY when
	                                        # the file has none) -- shown in the viewer window's title
	ram::Dict{String,GMTgrid}               # varname -> the WHOLE cube in memory ("Load all in RAM").
	                                        # Per VARIABLE, because the picker switches between them and
	                                        # each is its own cube; absent = that variable is read off
	                                        # disk one layer at a time, as before.
	xwin::Tuple{Float64,Float64}            # the DISPLAY WINDOW in x (NaNs = the whole file) -- see below
	                                        # OFF (user order, 2026-09-19): the tsunami is shown as THE
	                                        # COMPOSITE IMAGE OF THE TWO HALVES — one picture, painted
	                                        # from the water half and the land half — never as two
	                                        # surfaces each blanked to NaN outside its own nodes.
	twosurf::Bool                           # show the layer as TWO SURFACES (water + land), the way the
	                                        # two halves are shown on their own, instead of one composited
	                                        # texture. The composite cannot carry a per-side VTK (PBR)
	                                        # material; two actors can.
	satimg::Bool                            # "Sat img": the LAND side wears a downloaded satellite
	                                        # mosaic instead of a colourmap of its relief. Albedo only —
	                                        # the land is still lit by `_hs_reflectance` over the
	                                        # bathymetry, per side, exactly as before (SACRED_LAW.md's
	                                        # two-surface illumination law is untouched by this). The
	                                        # mosaic itself is cached in aquasat.jl; `imgbat` above is
	                                        # this side's per-node albedo whichever source made it, so
	                                        # toggling this empties it like a colormap change does.
end

const _AQUA = Dict{Ptr{Cvoid}, _AquaState}()

# ── the DISPLAY WINDOW in x ────────────────────────────────────────────────────────────────────
# A cube can be SHOWN over a sub-range of its own x extent: Catalina benchmark 1 simulates the whole
# 50 km flume and is shown over the first 20 km of it. NOTHING IS WRITTEN FOR THAT. The window is
# applied where a grid is READ, so no cropped copy of anybody's simulation is left on disk beside it
# (it used to write a `…_display20km.nc` next to the run — 16.8 MB, silent, and rebuilt every time it
# was deleted).
#
# Per scene, in the file's own x units, set BEFORE the open (the dialog calls `_aquamoto_open` with a
# path and nothing else) and carried in the state from there on. Absent = the whole file, which is
# every ordinary cube.
const _AQUA_XWIN  = Dict{Ptr{Cvoid},Tuple{Float64,Float64}}()
const _AQUA_NOWIN = (NaN, NaN)

_aqua_xwin(scene::Ptr{Cvoid}) = get(_AQUA_XWIN, scene, _AQUA_NOWIN)

# A TSUNAMI WINDOW KEEPS THE NaN-HOLE RIM in its grid meshes (Scene::aquaWindow, 10_geometry.cpp): the
# water and land halves are cut from one field by one mask, and the rim is what closes the one-cell
# slot between them along the coast. Every other window meshes a hole exactly. The Aquamoto viewer's
# own window is marked by the C side (AquamotoWindow::openFor); this marks the windows THIS file opens
# itself, right after each exists and before its halves are added. Optional export: with a library
# that predates it the call is skipped, and that library meshes every window with the rim anyway.
function _aqua_mark_window(h::Ptr{Cvoid})
	(h == C_NULL || !haskey(_LIB_FNS, :gmtvtk_set_aqua_window_h)) && return nothing
	ccall(_fn(:gmtvtk_set_aqua_window_h), Cvoid, (Ptr{Cvoid}, Cint), h, Cint(1))
	return nothing
end

"""
    _aqua_clipx(G, win) -> GMTgrid

THE display-window clip. EVERY grid this file class hands over goes through it — the bathymetry,
each layer of the open-time scan, the slice on screen, a whole cube pulled into RAM, the file's extra
static variables — so no read path can describe a different extent than another (SACRED_LAW.md). A
grid whose window is absent (or which the window covers whole) comes back UNTOUCHED: same buffer,
same layout, no copy.
"""
_aqua_clipx(::Nothing, ::Tuple{Float64,Float64}) = nothing    # a failed read stays a failed read

function _aqua_clipx(G::GMTgrid, win::Tuple{Float64,Float64})::GMTgrid
	(isfinite(win[1]) && isfinite(win[2])) || return G
	x  = Float64.(G.x)
	c0 = something(findfirst(>=(win[1]), x), 1)
	c1 = something(findlast(<=(win[2]), x), length(x))
	c1 > c0 || error("Aquamoto: the display window [$(win[1]), $(win[2])] holds no columns")
	(c0 == 1 && c1 == length(x)) && return G
	xs  = x[c0:c1]
	# A pixel-registered grid's `range` is its outer EDGES, a gridline-registered one's is its end
	# nodes — half a cell apart, and getting that wrong shifts the whole tank by half a column.
	half = (G.registration == 1) ? Float64(G.inc[1]) / 2 : 0.0
	rng  = copy(Float64.(G.range))
	rng[1], rng[2] = xs[1] - half, xs[end] + half
	local Z, lay
	if ndims(G.z) == 3
		# A CUBE IS CUT LAYER BY LAYER, THROUGH THE SAME ACCESSOR A SINGLE LAYER IS. `G.z[:, c0:c1, :]`
		# looks like the obvious thing and is wrong: a whole-cube read comes back with the file's own
		# memory order (GMT.jl honours "TRB" for cubes since 2026-08-08, so it is ROW-major), and the
		# Julia dims of such an array do not line up with (row, column) at all — slicing dim 2 keeps a
		# scrambled subset. Live proof of the bug it caused: a RAM-loaded cube's layer differed from the
		# same layer read off disk by up to 31 m while both had identical extrema, i.e. the same values
		# in the wrong places — on screen, a tank that no longer moved.
		nt  = size(G.z, 3)
		Z1  = _zmat(_cube_layer_view(G, 1))[:, c0:c1]
		Z   = Array{eltype(G.z),3}(undef, size(Z1, 1), size(Z1, 2), nt)
		Z[:, :, 1] = Z1
		for k in 2:nt
			Z[:, :, k] = _zmat(_cube_layer_view(G, k))[:, c0:c1]
		end
		lay = "BCB"                                # rebuilt column-major, row 1 = south (as `_zmat` gives)
	else
		# `_zmat` is THE accessor for z[iy,ix] with row 1 = south, whatever the source buffer's layout.
		# What comes out of it here is a NEW column-major matrix, so the grid built from it says "BCB"
		# — it is not the source's buffer any more (grid memory-layout law, SACRED_LAW.md).
		Z, lay = Matrix(_zmat(G)[:, c0:c1]), "BCB"
		rng[5], rng[6] = _finite_extrema(Z)
	end
	return GMT.GMTgrid(; proj4=G.proj4, wkt=G.wkt, epsg=G.epsg, geog=G.geog, range=rng,
	                   inc=copy(G.inc), registration=G.registration, nodata=G.nodata,
	                   x=xs, y=copy(G.y), z=Z, layout=lay, cpt=G.cpt)
end

# Every time-varying (>=3-D) quantity variable in `path`, skipping `skip` (the bathymetry
# variable) — NOT just the first match: the caller must load and offer ALL of them, never silently
# pick one and discard the rest. Pure netCDF subdataset introspection (drop.jl's
# `_netcdf_subdatasets`, GDAL's Subdatasets report) -- no guessed/hard-coded variable names. A
# tsunami netCDF of this file class always carries >1 variable (bathymetry + the time-varying
# quantity, at minimum), so it always shows up in GDAL's Subdatasets report; there is no
# single-variable case to fall back for. Empty if none found.  (`_aqua_find_all_varnames`, below.)

# The cube's own `time` coordinate — the model time of every step, for the window title. A 1-D
# variable is not a grid, so GMT cannot read it ("Named variable is not 2-, 3-, 4- or 5-D"); it comes
# out through the netCDF reader this package already owns for non-raster variables, shapenc.jl's
# GDAL MDArray helpers. A file without a usable `time` simply has no times, and the title then shows
# the slice number alone.
function _aqua_read_times(path::String, nsteps::Int)::Vector{Float64}
	out = Float64[]
	try
		ds = _shnc_open_multidim_read(path)
		root = C_NULL
		try
			root = _shnc_root(ds)
			arr = _shnc_group_open_array(root, "time")
			arr == C_NULL && return out
			n = min(_shnc_array_count(arr), nsteps)
			if n > 0
				edt = _shnc_edt_f64()
				out = _shnc_array_read_f64(arr, edt, n)
				_shnc_release_edt(edt)
				# A `time` variable that was never written comes back as netCDF's own fill (~9.97e36),
				# which is not a time — treat it as no time axis at all rather than putting 2.8e33
				# hours in the title (and, before this check, throwing an InexactError mid-slice).
				all(t -> isfinite(t) && abs(t) < 1e30, out) || (out = Float64[])
			end
			_shnc_release_array(arr)
		finally
			# THE ROOT GROUP IS RELEASED BEFORE THE CLOSE, on every path (the no-`time` return included).
			# A live group handle keeps the netCDF file open after GDALClose for the rest of the
			# process: on Windows the tsunami file then cannot be deleted or overwritten
			# (the test suite's "mktempdir cleanup ... EBUSY" on every Aquamoto fixture).
			root == C_NULL || ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (Ptr{Cvoid},), root)
			ccall((:GDALClose, GMT.libgdal), Cvoid, (Ptr{Cvoid},), ds)
		end
	catch
		return Float64[]
	end
	return out
end

# A cube's static 2-D companions include MASKS — a byte array, one flag per node (an inundation
# footprint, a beach). A mask is a BLACK-AND-WHITE PICTURE, not a field of numbers, so it is read and
# shown as one: `GMT.gdalread` on the NETCDF subdataset hands back the bytes as they are, a
# `GMTimage{UInt8}` with its full georef (range, inc, x, y, registration). `gmtread("file?var")`
# would convert the same variable to a Float32 GRID — a mask dressed up as data, with a colour scale
# and a z range it has no business having.
#
# Recognised by the on-disk TYPE (8-bit), never by the variable's name: those names belong to whoever
# built the model, and a hard-coded name list is the bug this package already has a law about.
#
# THE BYTES ARE NOT TOUCHED. The mask is an INDEXED image: its values are 0 and 1, meant to be shown
# as an entry into a black/white palette. VERIFIED against GMT.jl's own gdalread (gdal_utils.jl:67-76):
# `n_colors`/`colormap` are populated ONLY from a real GDAL raster colour table, and confirmed on this
# project's own byte masks (long_beach.grd / short_beach.grd, `gdalinfo` → "ColorInterp=Undefined", no
# colour table) that a netCDF byte variable never carries one. So `_img_is_indexed` was always false
# here, `_pixaccess_img` fell back to the raw 0/1 values as literal RGB, and the mask rendered as
# near-black on near-black -- present in the scene, invisible on screen. Rescaling those values to
# 0/255 "so black is black" would be wrong the same way: an 8-bit index of 255 means something only
# once a palette says so. So the palette is supplied here, explicitly, through the SAME setter every
# other indexed image in this codebase uses (`_img_set_palette!`, drape.jl) — never a bespoke path.
function _aqua_mask_image(path::String, varname::String)::GMTimage
	I = GMT.gdalread("NETCDF:\"$(path)\":$(varname)")
	I isa GMTimage || error("Aquamoto: '$varname' did not read back as an image")
	_img_is_indexed(I) || _img_set_palette!(I, UInt8[0 0 0; 255 255 255])
	eltype(I.image) == UInt8 || error("Aquamoto: '$varname' is not an 8-bit mask")
	return I
end

# Which DOOR a companion variable comes in by -- the image reader above for an 8-bit flag array, the
# grid reader for everything else. Nothing else in the viewer knows or cares that it is a mask: it is
# an image, and it behaves exactly like every other image in the window.
_aqua_is_byte_raster(v) = length(v.dims) == 2 && v.typ == "UInt8"

function _aqua_find_all_varnames(path::String, skip::String)
	found = String[]
	for v in _netcdf_subdatasets(path)
		lowercase(v.name) == lowercase(skip) && continue
		length(v.dims) >= 3 && push!(found, v.name)
	end
	return found
end

# Does `vars` (drop.jl's `_netcdf_subdatasets` report for a netCDF file) describe NSWING's own
# tsunami output -- a STATIC 2-D "bathymetry" grid alongside at least one time-varying (>=3-D)
# quantity variable? Such a file is not a pile of unrelated variables to choose between: it is ONE
# tsunami dataset whose two halves only mean anything TOGETHER (dry land coloured from the
# bathymetry, water from the stage -- see this file's header), and the only thing in the app that
# can display it is the Aquamoto viewer. So the file-open path routes it straight there instead of
# popping the generic multi-variable picker (`_on_drop`, drop.jl). Same shape as the test
# `_aqua_find_all_varnames` already applies, asked of a variable list the caller has in hand.
function _is_aquamoto_file(vars::Vector{@NamedTuple{name::String, dims::Vector{Int}, typ::String}})::Bool
	any(v -> lowercase(v.name) == "bathymetry" && length(v.dims) == 2, vars) &&
		any(v -> lowercase(v.name) != "bathymetry" && length(v.dims) >= 3, vars)
end

# z -> RGB (UInt8, ny x nx x 3) via a LINEAR cpt built fresh over [zlo,zhi] with `cmap` (any GMT
# colormap name, e.g. :geo, :polar). Nearest-bin lookup against the cpt's own discrete nodes (same
# convention as cpt.jl's `_z_to_hex`, generalized to a whole array). No NaN handling -- this file
# class is guaranteed clean Float32 data. Returns a flat greyed-out array if the cpt itself fails to
# build (`_cpt_nodes_range` returned nothing usable).
# WHAT COLOURS A SIDE: the name of a colormap (the quick list), or an explicit palette the "Color
# Palettes…" editor built (its rows + the z each row sits at, log spacing included). One alias, so
# every function that colours a side takes both forms and none of them has to know which arrived —
# the editor is not a second colouring path (SACRED_LAW.md), only another way to say what the palette
# is.
const _AquaPal = Union{Symbol,Tuple{Vector{Float64},Vector{Float64}}}

# THE PALETTE OF ONE SIDE (`side` 0 water / 1 land): the editor's, when it has been used on that
# side, else the named colormap. Every builder below asks HERE and nowhere else.
_aqua_side_pal(st::_AquaState, side::Int)::_AquaPal =
	isempty(st.cpt[side + 1][1]) ? (side == 1 ? st.landcmap : st.watercmap) : st.cpt[side + 1]

# …and its NODES over [lo, hi]. A named colormap is built fresh over that span; an editor palette
# carries its own z (that is the whole point of its Min Z / Max Z boxes) and is handed back as it is.
function _aqua_side_nodes(st::_AquaState, side::Int, lo::Float64, hi::Float64)
	p = _aqua_side_pal(st, side)
	p isa Symbol && return _cpt_nodes_range(lo, hi, p)
	return (p[1], p[2], length(p[1]))
end

function _aqua_colorize(Z::Matrix{Float32}, zlo::Float64, zhi::Float64, cmap::Symbol)::Array{UInt8,3}
	cz, crgb, n = _cpt_nodes_range(zlo, zhi, cmap)
	return _aqua_colorize(Z, zlo, zhi, cz, crgb, n)
end

# An EDITOR palette colours by ITS OWN node z values: they can be log-spaced, and they can span a
# range the user typed rather than the data's. So the lookup is a search in `cz`, not the even-step
# index the named path can afford. Same interpolation between neighbours either way, so a palette
# picked from the quick list and the same palette applied from the editor paint identically.
function _aqua_colorize(Z::Matrix{Float32}, cz::Vector{Float64}, crgb::Vector{Float64})::Array{UInt8,3}
	ny, nx = size(Z)
	rgb = Array{UInt8}(undef, ny, nx, 3)
	n = length(cz)
	(n < 2) && (fill!(rgb, 0xa0); return rgb)
	@inbounds for j in 1:nx, i in 1:ny
		v = Float64(Z[i, j])
		k = searchsortedlast(cz, v)
		if k < 1
			b = 0
			w = 0.0
		elseif k >= n
			b = 3 * (n - 1)
			w = 0.0
		else
			b = 3 * (k - 1)
			w = (cz[k+1] > cz[k]) ? (v - cz[k]) / (cz[k+1] - cz[k]) : 0.0
		end
		b1 = (w > 0.0) ? b + 3 : b
		for c in 1:3
			rgb[i, j, c] = round(UInt8, clamp(((1 - w) * crgb[b+c] + w * crgb[b1+c]) * 255, 0, 255))
		end
	end
	return rgb
end

# The palette in either form, over [zlo, zhi]: the ONE door the composite colours a side through.
_aqua_colorize(Z::Matrix{Float32}, zlo::Float64, zhi::Float64, pal::Tuple{Vector{Float64},Vector{Float64}})::Array{UInt8,3} =
	_aqua_colorize(Z, pal[1], pal[2])

function _aqua_colorize(Z::Matrix{Float32}, zlo::Float64, zhi::Float64,
                        cz::Vector{Float64}, crgb::Vector{Float64}, n::Int)::Array{UInt8,3}
	ny, nx = size(Z)
	rgb = Array{UInt8}(undef, ny, nx, 3)
	# 256-entry LUT (`_cpt_nodes_range` resamples any master CPT to 256 continuous nodes): index each
	# pixel into it. As long as the [zlo,zhi] range is matched to the data, the full 256-colour palette
	# is spanned -- the banding earlier came from a MIS-matched range (e.g. :geo over the full bathymetry
	# left land in only ~16 of the 256 nodes), not from too few palette entries.
	span = (zhi > zlo) ? (zhi - zlo) : 1.0
	invspan = (n - 1) / span
	# INTERPOLATED, like the viewer's own colour transfer function. Snapping to the nearest of the 256
	# nodes is a 256-step staircase, and the SAME half shown as a plain grid goes through VTK's
	# continuous CTF — so the composite and the standalone half were two different pictures of one
	# colour scale. Same z, same palette, same colour now.
	@inbounds for j in 1:nx, i in 1:ny
		v = Float64(Z[i, j])
		t = clamp((v - zlo) * invspan, 0.0, Float64(n - 1))
		i0 = floor(Int, t)
		i1 = min(i0 + 1, n - 1)
		w  = t - i0
		b0 = 3 * i0
		b1 = 3 * i1
		rgb[i, j, 1] = round(UInt8, clamp(((1 - w) * crgb[b0+1] + w * crgb[b1+1]) * 255, 0, 255))
		rgb[i, j, 2] = round(UInt8, clamp(((1 - w) * crgb[b0+2] + w * crgb[b1+2]) * 255, 0, 255))
		rgb[i, j, 3] = round(UInt8, clamp(((1 - w) * crgb[b0+3] + w * crgb[b1+3]) * 255, 0, 255))
	end
	return rgb
end

# Pack the composited RGB planes into the row-major, row-0-=-south, west->east, opaque RGBA byte
# buffer `gmtvtk_show_layer_rgba_h` expects (the SAME convention bakeLayerRGBA's own output uses,
# 40_shading.cpp).
#
# The colouring above is ELEMENT-WISE, so `rgb` sits in the grid's OWN element order, whatever layout
# the slice was read in — this is the one step that has to know which. `zlayout` is the grid's layout
# code (`_grid_layout_code`, drop.jl); `nx`/`ny` are its true dimensions (`_grid_dims`), which for a
# row-major grid are NOT `size(rgb)[1:2]`. Both branches are a plain gather, no transposition, no
# intermediate matrix:
#   "BCB" (0) — plane element (ix,iy) at ix*ny+iy   -> walk rows south->north, striding by ny
#   "TRB" (3) — plane element (ix,iy) at (ny-1-iy)*nx+ix -> the source rows ARE output rows, read
#               back to front (row 0 of the source is the NORTH one)
function _aqua_pack_rgba(rgb::Array{UInt8,3}, zlayout::Integer, nx::Int, ny::Int)::Vector{UInt8}
	npix = nx * ny
	length(rgb) == 3 * npix ||
		error("Aquamoto: RGB planes ($(length(rgb)) bytes) do not match the grid ($(nx)x$(ny))")
	buf = Vector{UInt8}(undef, npix * 4)
	rowmajor = (zlayout & 1) != 0
	northfirst = (zlayout & 2) != 0
	k = 1
	@inbounds for iy in 0:ny-1
		for ix in 0:nx-1
			m = rowmajor ? (northfirst ? (ny - 1 - iy) * nx + ix : iy * nx + ix) :
			               (northfirst ? ix * ny + (ny - 1 - iy) : ix * ny + iy)
			buf[k]   = rgb[m + 1]
			buf[k+1] = rgb[m + 1 + npix]
			buf[k+2] = rgb[m + 1 + 2npix]
			buf[k+3] = 0xff
			k += 4
		end
	end
	return buf
end

# Build + push the STATIC land colorbar legend for `cmap`. LAND is elevation >= 0, so the bar MUST
# start at sea level (0), never at the ocean-floor depth, AND the ramp is built over the LAND-ONLY
# span so it matches what _aqua_composite_rgb's imgbat cache actually paints -- both spend the full
# 256-node ramp on land only. `bat.range[6]` IS the max land elevation whenever any land exists (the
# overall max of a bathymetry grid always lands on a land cell, since land is defined as z>=0 and
# the sea floor is negative) -- no scan. Called once at file-open (_aquamoto_open) and again whenever
# the user picks a different land colormap (_aquamoto_set_cmap, side=1).
function _aqua_push_land_cpt!(scene::Ptr{Cvoid}, bat::GMTgrid, cmap::_AquaPal)
	lbarlo = 0.0                                # displayed LAND range: [0, max land elevation]
	lbarhi = max(bat.range[6], lbarlo + 0.1)    # falls back to lbarlo+0.1 when the whole area is ocean
	# Build the CPT DIRECTLY over the land-only span so all 256 nodes land on [0,lbarhi] -- building
	# over the full bathymetry range (as before) and then keeping only the z>=0 nodes wasted almost
	# the whole ramp on ocean-floor depths, leaving land with only a handful of distinct colours.
	# …an EDITOR palette carries its own nodes and its own z span, so the legend shows the span the
	# user typed in the editor — the bar and the pixels are then annotated by one and the same thing.
	local lcz::Vector{Float64}, lcrgb::Vector{Float64}, ln::Int
	if cmap isa Symbol
		lcz, lcrgb, ln = _cpt_nodes_range(lbarlo, lbarhi, cmap)
	else
		lcz, lcrgb = cmap[1], cmap[2]
		ln = length(lcz)
		if ln >= 2
			lbarlo = lcz[1]
			lbarhi = max(lcz[end], lbarlo + 0.1)
		end
	end
	ln < 2 && error("Aquamoto: colormap '$cmap' failed (makecpt)")
	ccall(_fn(:gmtvtk_aqua_set_land_cpt_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cdouble, Cdouble),
	      scene, lcz, lcrgb, Cint(ln), Cdouble(lbarlo), Cdouble(lbarhi))
	return nothing
end

# Change the WATER (side=0) or LAND (side=1) colormap for an already-open Aquamoto file (the
# "Color Bar water"/"Color Bar Land" colormap chooser, 50_scene.cpp aquaWaterColorbarRow/
# aquaLandColorbarRow). Water needs nothing else here -- _aquamoto_slice already recomputes the
# composite AND the legend from st.watercmap on every call. Land is CACHED (st.imgbat, built once
# from the static bathymetry -- see _aqua_composite_rgb) so the cache must be invalidated, and its
# own STATIC legend (built once at open) re-pushed. The caller (C++) re-renders the current slice
# right after this returns, same contract as _aquamoto_set_var.
function _aquamoto_set_cmap(scene::Ptr{Cvoid}, side::Int, cmap::String)
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	sym = Symbol(cmap)
	(side == 0 || side == 1) || error("Aquamoto: unknown colorbar side $side (0=water, 1=land)")
	# A NAME REPLACES WHATEVER THAT SIDE WORE, editor palette included: the quick list and the editor
	# are two ways of saying the same thing, so the last one used is the side's palette, full stop.
	empty!(st.cpt[side + 1][1]);  empty!(st.cpt[side + 1][2])
	if side == 0
		st.watercmap = sym
	else
		st.landcmap = sym
		st.imgbat = Array{UInt8}(undef, 0, 0, 0)   # cached bathymetry colourisation used the OLD cmap
		_aqua_push_land_cpt!(scene, st.bat, sym)
	end
	return nothing
end

# ── THE LAND SIDE'S ALBEDO ───────────────────────────────────────────────────────────────────────
# ONE function for "what colour is the land?", asked by every land painter there is: the flat
# composite (`_aqua_composite_rgb`), the lit combine (`aqua_shade_image` -> `_aqua_side_picture`) and
# the two-surface land actor (`_aqua_push_two_surfaces`). With "Sat img" off it answers EMPTY, which
# means "colour it from the land colormap as you always did"; with it on it answers the satellite
# mosaic on the bathymetry's own nodes. A painter that decided this for itself would be the fork
# SACRED_LAW.md forbids — and would be visible immediately, as one side of the window wearing tiles
# and another wearing :geo.
#
# The answer is CACHED in `st.imgbat`, the same field the colourmapped albedo has always lived in, so
# it is invalidated by the same one line every other land-colour change already uses.
function _aqua_land_albedo(st::_AquaState)::Array{UInt8,3}
	isempty(st.imgbat) || return st.imgbat
	st.satimg || return st.imgbat          # empty -> the painter colourises from st.landcmap, as before
	st.imgbat = _aqua_sat_rgb(st.bat)
	return st.imgbat
end

# The same answer as a georeferenced IMAGE, for the painters that take a picture rather than a
# per-node array (`grdimage`'s `-I` modulation, the PBR render's texture). `nothing` = "Sat img" is
# off and that painter keeps its colormap. One source, two shapes of the same thing — never a second
# fetch and never a second resample.
function _aqua_land_image(st::_AquaState)::Union{Nothing,GMTimage}
	st.satimg || return nothing
	A = _aqua_land_albedo(st)
	# MEMOISED ON THE ALBEDO ITSELF. The lit land half is cached on this image's identity
	# (`aqua_shade_image`'s `lkey`), so handing back a freshly built image every slice would make that
	# key change every slice and re-render the static land half at every timestep — the exact cost
	# that cache exists to avoid. One image per albedo array; a new albedo (a "Sat img" toggle, a new
	# region) is a new array and therefore a new image.
	get!(_AQUA_LAND_IMG, objectid(A)) do
		_aqua_sat_image(A, st.bat)
	end
end

const _AQUA_LAND_IMG = Dict{UInt,GMTimage}()

# "Sat img" (the checkbox under the "Water side" button). Drops the cached land albedo so the next
# paint asks `_aqua_land_albedo` again, and drops the LIT land half too — that cache is keyed on the
# land colormap, which is exactly the thing this replaces, so a key that cannot see the change would
# keep serving the picture made before it. The caller (C++) re-renders the current slice.
#
# IT FETCHES HERE, NOT LATER. The albedo used to be built lazily, at the next paint: a download that
# failed then surfaced as a broken SLICE, seconds after the click, with nothing tying the two
# together — and a download that SUCCEEDED said nothing at all, so a box that had done its whole job
# looked identical to one that was not wired. Both are the same defect: a control the user cannot
# tell the outcome of. The work happens inside the click, behind the click's own busy notice, and
# this prints ONE line saying what the land side now wears — which the caller shows.
function _aquamoto_sat_img(scene::Ptr{Cvoid}, on::Bool)
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	st.satimg = on
	st.imgbat = Array{UInt8}(undef, 0, 0, 0)
	empty!(_AQUA_LAND_CACHE)
	empty!(_AQUA_LAND_IMG)
	if !on
		print("Land side: colour map ($(st.landcmap))")
		return nothing
	end
	A = _aqua_land_albedo(st)                      # downloads + warps NOW, so a failure throws HERE
	_aqua_sat_hires(st.bat)                        # …and the 3-D drape's finer texture, same click
	nx, ny = _grid_dims(st.bat)
	print("Land side: satellite imagery, $(nx)x$(ny) nodes at zoom $(_aqua_sat_zoom(st.bat))")
	return nothing
end

# The SAME row's "Color Palettes…" editor (50_scene.cpp's aqua colour-bar rows -> ColorPalettesWindow
# -> g_aquamotoSetCPT), which hands over an ARBITRARY palette instead of the name of one: `cz` is the
# z each palette row sits at (log-spaced when the editor's Logaritmize is on) and `crgb` its colours,
# flat, three per node in 0..1. Everything else is `_aquamoto_set_cmap`'s contract verbatim -- the
# land cache is dropped, the land legend re-pushed, and the caller re-renders the current slice.
#
# WHY IT EXISTS: a grid's Color Bar row has always offered this editor (colorbarRow -> showColorPalettes),
# and Aquamoto's two rows offered only the quick list, because the editor applies through
# `gmtvtk_set_cpt_grid`, which recolours a scalar+LUT surface -- and a tsunami layer is a picture the
# host composites. Same control, less function for one element type: SACRED_LAW.md, verbatim. The
# editor now reaches the composite through the door the composite is painted behind.
function _aquamoto_set_cpt(scene::Ptr{Cvoid}, side::Int, cz::Vector{Float64}, crgb::Vector{Float64})
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	(side == 0 || side == 1) || error("Aquamoto: unknown colorbar side $side (0=water, 1=land)")
	(length(cz) >= 2 && length(crgb) == 3 * length(cz)) ||
		error("Aquamoto: palette has $(length(cz)) nodes and $(length(crgb)) colour components")
	p = st.cpt[side + 1]                       # the tuple is fixed; the vectors in it are the state
	empty!(p[1]);  append!(p[1], cz)
	empty!(p[2]);  append!(p[2], crgb)
	if side == 1
		st.imgbat = Array{UInt8}(undef, 0, 0, 0)   # cached bathymetry colourisation used the OLD palette
		_aqua_push_land_cpt!(scene, st.bat, (p[1], p[2]))
	end
	return nothing
end

# Open a netCDF file, cache its header (bathymetry grid, EVERY time-varying quantity var name,
# step count) per window, and immediately scan the ACTIVE variable's layers ONCE for its own
# min/max (both the WET-cell-only range and the whole-cell range) -- so navigating slices and
# toggling "Scale colour to global min/max" are instant lookups afterwards, never a fresh rescan.
# Prints "nsteps|activevar|var1,var2,…" (parsed by the C++ dialog to fill "Time steps = N" + the
# slider range + the Stage/Xmoment/Ymoment/Or… quantity picker) on success; throws (shown as an
# error dialog by the console-eval bridge) on anything it can't make sense of.
# Drop the per-variable Scene Objects rows a PREVIOUS open of this window left behind — the ones
# `_aquamoto_open` adds below, named after the variable. Called by the open itself, so no caller has
# to remember to clean up, and so a second file opened into the same window replaces the first file's
# rows instead of stacking beside them. Removing a name that is not there is a no-op.
function _aqua_drop_var_rows(scene::Ptr{Cvoid}, path::String, varnames::Vector{String})
	old = get(_AQUA, scene, nothing)
	names = Set{String}(["bathymetry"])
	# The variables of the file being opened AND of the one this window had before it: either set can
	# carry a name the other does not.
	for v in _netcdf_subdatasets(path);  push!(names, v.name);  end
	if old !== nothing
		push!(names, "bathymetry")
		try
			for v in _netcdf_subdatasets(old.path);  push!(names, v.name);  end
		catch                                   # the previous file may be gone by now — its rows are not
		end
		union!(names, old.varnames)
	end
	union!(names, varnames)
	# A companion variable is dropped BY NAME, whatever kind it came in as: the quantity variables and
	# the bathymetry are grids, an 8-bit mask is an image (`_aqua_mask_image`). Dropping only the grid
	# kind left every mask actor and handle standing, so a second open piled a new one on top of a
	# stale one under the same name.
	for nm in names
		ccall(_fn(:gmtvtk_remove_grid_h),  Cint, (Ptr{Cvoid}, Cstring), scene, nm)
		ccall(_fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), scene, nm)
		_forget_object!(scene, :grid,  nm)
		_forget_object!(scene, :image, nm)
	end
	return nothing
end

function _aquamoto_open(scene::Ptr{Cvoid}, path::String)
	isfile(path) || error("Aquamoto: file not found: $path")
	varnames = _aqua_find_all_varnames(path, "bathymetry")
	isempty(varnames) && error("Aquamoto: could not find a time-varying quantity variable in $path " *
	                          "(expected alongside a 'bathymetry' variable — NSWING's own single 3-D netCDF output)")
	varname = varnames[1]   # no name-based preference -- whichever time-varying quantity var was found first
	# The window this file is to be SHOWN over, if the caller asked for one before the open. From here
	# on it travels in the state, and EVERY read below goes through `_aqua_clipx`.
	xwin = _aqua_xwin(scene)
	bat = try
		_aqua_clipx(_gmtread_trb("$(path)?bathymetry"), xwin)
	catch e
		error("Aquamoto: could not read 'bathymetry' from $path ($(sprint(showerror, e)))")
	end
	info = try
		GMT.grdinfo("$(path)?$(varname)", C = true, Q = true)
	catch e
		error("Aquamoto: could not read '$varname' header from $path ($(sprint(showerror, e)))")
	end
	inl = findfirst(==("n_layers"), info.colnames)
	nsteps = inl === nothing ? 1 : max(1, Int(info.data[inl]))
	geog = _isgeographic(bat)

	batz = bat.z
	# Scan EVERY discovered variable now, not just the active one -- the whole point of this fix is
	# that no variable in the file is silently left unread. One shared progress bar spans all of them.
	# `alllo`/`allhi` are read straight off each layer's OWN GMTgrid `.range` -- GMT already computes a
	# grid's z min/max when it reads it, so recomputing that by hand would be pure waste. `wetlo`/
	# `wethi` genuinely need a per-cell scan (wet/dry is a comparison against bathymetry, not something
	# any header/metadata carries) -- this file class is guaranteed clean Float32 with no NaNs, so no
	# isnan guard either.
	scans = Dict{String,_AquaVarScan}()
	_progress_show_async(nsteps * length(varnames), "Aquamoto — scanning layers…")
	for (vi, vn) in enumerate(varnames)
		sc = get!(scans, vn) do
			_AquaVarScan(fill(NaN, nsteps), fill(NaN, nsteps), falses(nsteps), fill(NaN, nsteps), fill(NaN, nsteps))
		end
		for k in 0:nsteps-1
			# `_read_cube_layer` (drop.jl) is THE read of one layer out of a cube; it takes a 1-based
			# index and applies the same "TRB" reader the bathymetry above went through, so the two
			# share an element order. Spelling the subdataset index out here instead would be the same
			# operation written twice — with the two spellings disagreeing on the index base, which is
			# exactly how that drifts.
			Gk = _aqua_clipx(_read_cube_layer("$(path)?$(vn)", k + 1), xwin)
			Z = Gk.z
			sc.alllo[k+1], sc.allhi[k+1] = Gk.range[5], Gk.range[6]
			lo_w, hi_w = Inf, -Inf
			dry = _aqua_isdry.(batz, Z)              # THE dry/wet test (_aqua_indland's own element rule)
			@inbounds for i in eachindex(Z)
				dry[i] && continue                    # dry cell -> excluded from the wet-only range
				z = Z[i]
				z < lo_w && (lo_w = z); z > hi_w && (hi_w = z)
			end
			sc.wetany[k+1] = lo_w <= hi_w
			sc.wetlo[k+1], sc.wethi[k+1] = lo_w, hi_w
			_progress_status((vi - 1) * nsteps + k + 1, "Aquamoto — scanning layers… ($(vn) $(k + 1)/$(nsteps))")
		end
	end
	_progress_close()

	# Push the LAND colorbar ONCE here (static for the whole file, unlike the per-slice water bar) --
	# re-pushed later only if the user picks a different land colormap (_aquamoto_set_cmap).
	_aqua_push_land_cpt!(scene, bat, :geo)

	# Hand the viewer the static bathymetry = the LAND surface for hillshading. The buffer goes over as
	# it lies with its layout code (the viewer stores it column-major itself, gridCopyToCM) -- the SAME
	# handoff as the per-slice stage (zhover). The viewer then shades LAND from this and WATER from the
	# live stage through the ONE shared applyReliefShade (bakeAquaShade) -- so the Shading dock's
	# Hillshade drives the tsunami like any other layer.
	bz, bnx, bny, blay = _grid_zbuf(bat)
	ccall(_fn(:gmtvtk_aqua_set_bathy_h), Cint, (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cint), scene, bz, bnx, bny, blay)

	# Label the composited water/land surface's OWN Scene Objects group with the active variable's
	# real name (whatever the file itself calls it -- no assumed naming).
	ccall(_fn(:gmtvtk_aqua_set_var_label_h), Cint, (Ptr{Cvoid}, Cstring), scene, varname)

	# Load EVERY variable this file actually carries as its OWN Scene Objects group (nested, in the
	# viewer, under the file's group): bathymetry itself, plus any other static 2-D grid the file
	# happens to have alongside bathymetry/the time-varying quantity var(s) -- pure enumeration off
	# the file's real Subdatasets report, no guessed/hard-coded variable names beyond the
	# already-established "bathymetry" convention this file class uses. Only the ACTIVE quantity
	# variable ('z'/the composited water surface) starts visible -- every other loaded group
	# (bathymetry, any extra static grid) starts UNCHECKED (gmtvtk_set_object_visible, the same
	# "add hidden" call nested.jl's blank-grid path already uses).
	# A RE-OPEN REPLACES THESE ROWS, it never stacks a second set. This window may already be showing
	# another file of this class (open a tsunami, then open a second one into the same window — which is
	# what a benchmark does when its run finishes), and the variable groups added below are named after
	# the VARIABLE, not the file: adding again gave two "bathymetry" handles, of which only the first
	# was hidden by the call below — so the second stood there CHECKED with nothing on screen, which is
	# a checkbox lying about what it controls. Removing first is idempotent: a name that is not there
	# removes nothing.
	_aqua_drop_var_rows(scene, path, varnames)
	_add_grid_to_scene(scene, bat, "bathymetry"; promote = false, source = "$(path)?bathymetry")
	ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, "bathymetry", Cint(0))
	skipvars = Set(lowercase.(varnames)); push!(skipvars, "bathymetry")
	for v in _netcdf_subdatasets(path)
		lowercase(v.name) in skipvars && continue
		try
			if _aqua_is_byte_raster(v)    # an 8-bit flag array: a B&W MASK IMAGE, never a grid
				# Added EXACTLY like every other companion of the cube, image or grid: through the normal
				# add, with its OWN axes set built and framed to its own extent at birth (the image branch
				# of gmtvtk_add_surface_h), then left unchecked. No adopt here and none for the grid
				# companions either -- an adopt is the "this is what the window shows now" transition, and
				# what this window shows is the simulation being opened.
				_add_image_to_scene(scene, _aqua_mask_image(String(path), v.name), v.name;
				                    promote = false, source = "$(path)?$(v.name)")
			else
				G = _aqua_clipx(_gmtread_trb("$(path)?$(v.name)"), xwin)
				_add_grid_to_scene(scene, G, v.name; promote = false, source = "$(path)?$(v.name)")
			end
			ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, v.name, Cint(0))
		catch e
			@tool_error "Aquamoto: could not load variable '$(v.name)'" exception=e
		end
	end

	# The trailing display options mirror the dialog's OWN startup state ("Split Dry/Wet" checked,
	# global scaling off, no transparency, both sides shaded). They are overwritten by the first
	# `_aquamoto_slice`, i.e. before anything is on screen, so they only ever matter to a caller that
	# asks for a slice before the dialog has drawn one.
	_AQUA[scene] = _AquaState(String(path), varname, varnames, scans, bat, nsteps, geog, Array{UInt8}(undef, 0, 0, 0), true, :polar, :geo,
	                          ((Float64[], Float64[]), (Float64[], Float64[])),   # no palette editor applied yet
	                          0,
		                          (Dict{String,String}(), Dict{String,String}()),
	                          true, false, 0.0, true, true, _aqua_read_times(String(path), nsteps),
	                          Dict{String,GMTgrid}(), xwin, false, false)   # twosurf: see `_aqua_push_two_surfaces`; satimg: "Sat img"
	print(nsteps, "|", varname, "|", join(varnames, ","))
	return nothing
end

# Switch the ACTIVE quantity variable for an already-open file (the Stage/Xmoment/Ymoment/Or…
# picker). Every variable was already scanned up front in `_aquamoto_open` (`st.scans`), so this
# is a plain lookup, never a rescan. The caller (C++) re-renders the current slice right after this
# returns. Throws if `varname` was not among the ones `_aquamoto_open` already found in the file.
function _aquamoto_set_var(scene::Ptr{Cvoid}, varname::String)
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	(varname == st.varname) && return nothing   # no-op: already active
	haskey(st.scans, varname) || error("Aquamoto: '$varname' is not one of this file's quantity variables")
	st.varname = varname
	# st.imgbat (the cached land colourisation) depends only on the static bathymetry, never on the
	# active quantity variable -- left untouched here on purpose.
	ccall(_fn(:gmtvtk_aqua_set_var_label_h), Cint, (Ptr{Cvoid}, Cstring), scene, varname)
	return nothing
end

# Prior-session state for `scene` (if any), so a freshly (re)opened Aquamoto panel on a window that
# already had a file loaded restores that state instead of starting blank. Prints
# "path|nsteps|activevar|var1,var2,…", or nothing (empty) if this scene has no cached session.
function _aquamoto_state(scene::Ptr{Cvoid})
	st = get(_AQUA, scene, nothing)
	(st === nothing) && return nothing
	print(st.path, "|", st.nsteps, "|", st.varname, "|", join(st.varnames, ","))
	return nothing
end

# The whole-cube WET-cell min/max, derived from the per-layer arrays `_aquamoto_open` already
# scanned up front (an entirely-dry layer is flagged in `wetany` and excluded here).
"""
    _aqua_global_mm(scene) -> Cint

The whole-cube min/max of the variable on screen, printed as "var,lo,hi" for the dialog's
"Scale colour to global min/max" box to show on hover. Read through `_aqua_global_minmax`, the one
place that number comes from, so the hint and the colour scale can never disagree.
"""
function _aqua_global_mm(scene::Ptr{Cvoid})::Cint
	st = get(_AQUA, scene, nothing)
	(st === nothing) && return Cint(0)
	haskey(st.scans, st.varname) || return Cint(0)
	lo, hi = _aqua_global_minmax(st)
	print(st.varname, ',', lo, ',', hi)
	return Cint(1)
end

function _aqua_global_minmax(st::_AquaState)
	sc = st.scans[st.varname]
	any(sc.wetany) || return (0.0, 1.0)
	lo = sc.wetlo[sc.wetany]; hi = sc.wethi[sc.wetany]
	return (minimum(lo), maximum(hi))
end

# The colour-scale range for a slice: the whole-cube global min/max when `useglobal`, else the
# extrema of `vals` (already the wet-only values when splitDryWet, the whole slice otherwise). A
# degenerate (all-equal) range is nudged so `_cpt_nodes_range` never sees zlo==zhi.
function _aqua_range(vals::Vector{Float64}, useglobal::Bool, globalmin::Float64, globalmax::Float64)
	if useglobal
		return globalmin, globalmax
	end
	isempty(vals) && return (0.0, 1.0)
	lo, hi = extrema(vals)
	# A near-zero-width span (not just an EXACT lo==hi) must be caught too -- a tsunami's very first
	# timestep is essentially all-zero water, so the wet-cell extrema can come out as floating-point
	# noise like (-1e-14, 2e-15): that passed the old `lo == hi` check untouched and left the
	# colourbar showing "-0 / 0 / 0" (its tick formatter rounding both ends to zero). Reset to a
	# clean, symmetric fallback span whenever the real span is negligible, rather than nudging just
	# one end (which would keep the confusing near-zero OTHER end as-is).
	if (hi - lo) < 1e-6
		mid = (lo + hi) / 2
		lo, hi = mid - 0.1, mid + 0.1
	end
	return (lo, hi)
end

# The pure compositing step (aqua_suppfuns.m coards_sliceShow's IamTSU branch, do_imgWater/
# do_imgBat/mixe_images): given the bathymetry + this slice's quantity (both ny x nx, same shape),
# returns `(rgb, imgbat)` — `imgbat` is the land colourisation, passed back so the caller can cache
# it (only depends on the static bathymetry, never the slice). No I/O, no scene state -- everything
# a caller needs is an argument, so this is exactly what the unit tests exercise directly.
# `shadeWater`/`shadeLand` (only meaningful when `splitDryWet`) let the "Shade Water"/"Shade Land"
# toggles hide one side's colour scale at a time (flat mid-grey instead) without touching the
# CACHED real `imgbat` -- so re-enabling a toggle never needs a bathymetry recolour. `landhi` is the
# LAND-ONLY colour-scale top (max land elevation) -- the CALLER already knows this from the
# bathymetry grid's own `.range` (see _aquamoto_open/_aquamoto_slice), so this pure helper is not
# asked to rediscover it by filtering + scanning `bat` on every first call.
# THE dry/wet test. A cell is DRY LAND when the water level sits ON the sea floor — the stage equals
# the bathymetry there. Every consumer of that fact goes through this ONE function: the composite
# below, the run-in scan, the per-layer wet-range scan, and — via the mask pushed with the texture —
# the viewer's own per-side relight. It used to be spelled out separately in each of those places,
# including once in C++ (`bakeAquaShade`), and the moment two of those spellings disagreed the
# composite painted a cell as land while the relight lit it as water: the dry/wet split vanished on
# screen with nothing to show for it. SACRED_LAW.md: same operation, same function.
_aqua_isdry(b::Real, z::Real)::Bool = abs(b - z) < 1f-2          # the rule, per cell
_aqua_indland(bat::AbstractMatrix, Z::AbstractMatrix) = _aqua_isdry.(bat, Z)   # …and over a whole slice

# The same mask as the viewer must read it: one byte per node, row-major with row 0 = SOUTH, i.e. the
# exact convention `_aqua_pack_rgba` uses for the composite it accompanies (and the same `zlayout`
# code, since the mask is built from the grids in THEIR order).
function _aqua_pack_landmask(indland::AbstractMatrix{Bool}, zlayout::Integer, nx::Int, ny::Int)::Vector{UInt8}
	npix = nx * ny
	length(indland) == npix ||
		error("Aquamoto: land mask ($(length(indland))) does not match the grid ($(nx)x$(ny))")
	buf = Vector{UInt8}(undef, npix)
	rowmajor = (zlayout & 1) != 0
	northfirst = (zlayout & 2) != 0
	k = 1
	@inbounds for iy in 0:ny-1
		for ix in 0:nx-1
			m = rowmajor ? (northfirst ? (ny - 1 - iy) * nx + ix : iy * nx + ix) :
			               (northfirst ? ix * ny + (ny - 1 - iy) : ix * ny + iy)
			buf[k] = indland[m + 1] ? 0x01 : 0x00
			k += 1
		end
	end
	return buf
end

function _aqua_composite_rgb(bat::Matrix{Float32}, Z::Matrix{Float32}, splitDryWet::Bool,
                             waterlo::Float64, waterhi::Float64, transparency::Float64,
                             imgbat::Array{UInt8,3}, landhi::Float64,
                             shadeWater::Bool=true, shadeLand::Bool=true,
                             watercmap::_AquaPal=:polar, landcmap::_AquaPal=:geo)
	ny, nx = size(Z)
	if !splitDryWet
		return _aqua_colorize(Z, waterlo, waterhi, watercmap), imgbat
	end
	indland = _aqua_indland(bat, Z)
	Zc = copy(Z)
	Zc[indland] .= 0.0
	if isempty(imgbat)                                     # cache: only depends on the (static) bathymetry
		                                                    # AND landcmap -- caller invalidates (empties
		                                                    # imgbat) whenever the land colormap changes.
		# LAND-ONLY range, same as the colorbar (_aquamoto_open) -- colorizing over the FULL bathymetry
		# range (incl. ocean depths) wasted most of the 256-node ramp on sea floor, leaving land itself
		# with only a handful of distinct colours (blocky look, and mismatched vs the legend).
		# …and the land's own extrema, exactly as the land half shown alone is coloured: the palette
		# spans the DRY NODES' min..max, not 0..max and not the whole bathymetry (which buries the
		# coast in the top fraction of the ramp). Same rule as the water above — each side coloured as
		# a grid of that side alone.
		dz  = view(bat, indland)
		blo = isempty(dz) ? 0.0 : Float64(minimum(dz))
		bhi = isempty(dz) ? 1.0 : Float64(maximum(dz))
		(bhi > blo) || (bhi = blo + 0.1)
		imgbat = _aqua_colorize(bat, blo, bhi, landcmap)    # :geo default already has its own land/sea break
	end
	# ALWAYS colour BOTH sides -- land from the cached bathymetry, water from the wet stage. NEVER grey a
	# side out (that made land show up grey when Water was the selected radio). Both images are always
	# shown; the Shade Water/Shade Land radio only selects which side's LIGHT the Shading dock edits
	# (aquaShowWater, applied per-side by bakeAquaShade in the viewer), it does not hide either colour.
	imgwater = _aqua_colorize(Zc, waterlo, waterhi, watercmap)               # diverging: trough/calm/crest
	landrgb  = imgbat
	alfa = clamp(transparency, 0.0, 1.0)
	rgb = similar(imgwater)
	if alfa > 0.01                                          # mixe_images' addweighted cross-blend
		for idx in eachindex(rgb)
			rgb[idx] = round(UInt8, clamp((1 - alfa) * imgwater[idx] + alfa * landrgb[idx], 0, 255))
		end
	else
		rgb = imgwater
	end
	@inbounds for j in 1:nx, i in 1:ny                      # hard land overwrite (mixe_images)
		indland[i, j] || continue
		rgb[i, j, 1] = landrgb[i, j, 1]
		rgb[i, j, 2] = landrgb[i, j, 2]
		rgb[i, j, 3] = landrgb[i, j, 3]
	end
	return rgb, imgbat
end

# ---------------------------------------------------------------------------------------------
# View > "Illumination (Hillshade)…" ON A TSUNAMI LAYER. What this window shows is a COMPOSITE of two
# images standing on two DIFFERENT surfaces -- water on the live stage, land on the static bathymetry
# -- and the shading engine already lights the two separately (bakeAquaShade, 40_shading.cpp). So the
# tool illuminates EACH SIDE FROM ITS OWN SURFACE, through the same `_hs_reflectance` a plain grid
# goes through (SACRED_LAW: same operation, same function), and pushes each with its own `side`.
# Illuminating "the grid this window shows" instead resolved to the bathymetry and then lit the SEA
# with the sea FLOOR's relief -- the dry/wet split gone, which is precisely what the dock exists to
# keep. Models 8/9 build a new variable rather than modulating and never come here (hillshade.jl).
#
# `side` says WHICH surface the user aimed the dialog at: -1 (the toolbar button) lights both, 0 the
# water alone, 1 the land alone. The two palette buttons beside Shade Water / Shade Land send 0 and 1,
# and a method chosen for one side leaves the other's exactly as it was — which is the same law read
# one level up: the two sides are two surfaces, so the METHOD is a per-side choice too.
"""
    _aqua_forget_illum!(st, side)

Forget the Illumination model of ONE side (`side` 0 water / 1 land), or of BOTH when `side < 0`.
Called wherever a light is replaced by a look or taken off: the remembered params are what make the
water re-light itself at every timestep, so a side that no longer has a model must no longer have
params either.
"""
function _aqua_forget_illum!(st::_AquaState, side::Int)
	for sd in (side < 0 ? (0, 1) : (side,))
		empty!(st.illum[sd + 1])
	end
	# Method 1 renders each half in an off-screen window kept between frames (`_PBR_WIN`,
	# aquashade.jl). With no model loaded on either side nothing will photograph them again, so they
	# go now rather than idling with a GL context for the rest of the session.
	all(isempty, st.illum) && aqua_pbr_release!()
	return nothing
end

"""
    _aqua_illum_models(scene) -> prints "mw,ml"

The model number each side is CURRENTLY lit with — water first, land second, 0 when that side has
no model stored. Read straight off `st.illum`, the one place a side's method lives, so the Debug
tab's two boxes state what the layer actually wears instead of a default of their own. Printed for
the C++ side (75_aquamoto.cpp) to parse.
"""
function _aqua_illum_models(scene::Ptr{Cvoid})::Cint
	st = get(_AQUA, scene, nothing)
	(st === nothing) && (print("0,0"); return Cint(0))
	m(p) = isempty(p) ? 0 : parse(Int, p["model"])
	print(m(st.illum[1]), ',', m(st.illum[2]))
	return Cint(1)
end

"""
    _aqua_set_illum_model(scene, side, model) -> Cint

The METHOD of one side, set from the dialog's model spin box. The side keeps the light it already
carries (azimuth, elevation, material); only the model number changes, and it is applied through
`_aqua_illuminate!` — the one setter the Illumination dialog itself goes through, so a box and the
dialog cannot mean two different things.
"""
function _aqua_set_illum_model(scene::Ptr{Cvoid}, side::Int, model::Int)::Cint
	st = get(_AQUA, scene, nothing)
	(st === nothing) && return Cint(0)
	(0 <= side <= 1) || return Cint(0)
	d = copy(st.illum[side+1])
	d["model"] = string(model)
	_aqua_illuminate!(scene, model, d, side)
	return Cint(1)
end

function _aqua_illuminate!(scene::Ptr{Cvoid}, model::Int, d::Dict{String,String}, side::Int = -1)
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	# Remembered PER SIDE: each side keeps its own method and its own light, and a run aimed at one
	# leaves the other's exactly as it was.
	for sd in (side < 0 ? (0, 1) : (side,))
		p = st.illum[sd + 1]                  # the tuple is fixed; the dictionaries in it are the state
		empty!(p); merge!(p, d)
		p["model"] = string(model)
	end
	# THE LIGHT IS A REFLECTANCE PUSHED TO THE VIEWER, one per side, each computed from ITS OWN
	# surface: the LAND from the static bathymetry, the WATER from the stage of the slice on screen.
	# The viewer then modulates the composite it already paints (bakeAquaShade / the 3-D surface's
	# baked colours) with them, which is what every other grid in this app does with an Illumination
	# model — same operation, same function (`_hs_reflectance`), no second picture.
	(side < 0 || side == 1) && _hs_push_grid(scene, st.bat, model, st.illum[2], 1)   # LAND
	(side < 0 || side == 0) && _aqua_relight_water!(scene, st)                        # WATER
	# The method is DECLARED here, by the act that chose it — never by the pushes above, which are
	# data and run again at every slice.
	_hs_declare_look(scene, side)
	return nothing
end

# The WATER side's reflectance, computed from the stage it actually stands on. Called by the tool and
# AGAIN by every slice change: the stage is a DIFFERENT surface at every timestep, so a reflectance
# computed once would light slice 40's wave with slice 3's relief. `G` is the slice the caller has
# already read (never re-read it); without one, the slice on screen is read here.
function _aqua_relight_water!(scene::Ptr{Cvoid}, st::_AquaState, G::Union{GMTgrid,Nothing}=nothing)
	p = st.illum[1]
	isempty(p) && return nothing
	# …AND ONLY WHILE THAT MODEL IS STILL THE WINDOW'S LIGHT. Picking a relief look (VTK PBR, grdimage,
	# Lambert) REPLACES a loaded Illumination model — sceneSetReliefLook drops it — so re-pushing the
	# remembered one here would put the window straight back on a hillshade method (every push force-
	# sets useHillshade/hillGrd), i.e. the method the user just chose silently reverting at the next
	# slice. Ask the viewer whether the model is still loaded; when it is not, forget it.
	if ccall(_fn(:gmtvtk_has_extern_shade_h), Cint, (Ptr{Cvoid},), scene) == 0
		empty!(p)
		return nothing
	end
	model = parse(Int, p["model"])
	Gw = G === nothing ? _aqua_layer(st, st.cur) : G     # st.cur is 0-based; THE layer read (RAM or disk)
	Gw === nothing && error("Aquamoto: could not read layer $(st.cur + 1) of '$(st.varname)' from $(st.path)")
	R = _aqua_water_reflectance(st, Gw, model)
	R === nothing && return nothing            # an entirely dry step has no water to light
	_hs_push(scene, R, Float64(Gw.range[1]), Float64(Gw.range[2]),
	         Float64(Gw.range[3]), Float64(Gw.range[4]), model, 0)
	return nothing
end

# THE WATER SIDE's reflectance. Two things, and both matter:
#
# 1. THE FIELD IS THE WET STAGE. A tsunami stage stores the LAND ELEVATION on its dry cells, so the
#    raw array is not a water surface: it is a water surface with the coastline welded into it. Every
#    model scales its intensity from the data's OWN range, so those cliffs, not the sea, decided the
#    light. The dry cells are dropped with `_aqua_indland`, THE dry/wet test the composite itself
#    paints with, so the stretch sees the wave field and nothing else.
#
# 2. WHAT GOES DOWN IS NEVER NaN — the dropped cells are pushed as intensity ZERO. `gmtIlluminate`
#    returns immediately on 0, so those texels keep the composite's colour byte for byte. A NaN there
#    instead makes `externShadeAt` return NaN, which sends the pixel down bakeAquaShade's OTHER branch
#    (normal-derived shading) — a different code path, and therefore a different colour, on pixels
#    this tool was never asked to touch.
function _aqua_water_reflectance(st::_AquaState, G::GMTgrid, model::Int)
	(size(st.bat.z) == size(G.z)) ||
		error("Aquamoto: '$(st.varname)' ($(size(G.z))) and bathymetry ($(size(st.bat.z))) sizes differ")
	(_grid_layout_code(G) == _grid_layout_code(st.bat)) ||
		error("Aquamoto: '$(st.varname)' ($(G.layout)) and bathymetry ($(st.bat.layout)) have different memory layouts")
	dry = _aqua_indland(st.bat.z, G.z)         # element-wise, both buffers as they lie -- no layout
	all(dry) && return nothing
	W = deepcopy(G)
	W.z[dry] .= NaN32                          # out of the FIELD, so they are out of the STRETCH
	wet = view(W.z, .!dry)
	W.range[5], W.range[6] = Float64(minimum(wet)), Float64(maximum(wet))
	R = _hs_reflectance(W, model, st.illum[1])
	# ...and back in as NEUTRAL. Covers the dropped cells and the one-cell NaN fringe GMT leaves
	# around them: every node the light has nothing to say about says exactly nothing.
	@inbounds for k in eachindex(R)
		isfinite(R[k]) || (R[k] = 0.0f0)
	end
	return R
end

# THE ILLUMINATED PICTURE for the slice on screen, band-planar in THE GRID'S OWN element order —
# which is what `_aqua_pack_rgba` consumes, exactly like the plain composite it stands in for.
# `nothing` when no side carries a method, so the plain composite is used unchanged.
#
# `aqua_shade_image` hands back a plain (row, col, band) picture — it de-interleaves each half itself
# (`_aqua_rgb_plane`) so a grdimage half and a VTK-rendered one compose in the same combine — and it
# came from THIS grid, so its pixel order IS the grid's element order (row-major, north first).
#
# `model_water` / `model_land` OVERRIDE the model that side's stored illumination carries (0 = use
# the stored one, which is what every per-slice call passes). The Debug tab's two 1..7 boxes come in
# through here, so the picture the button builds is made by THIS function, with the same halves, the
# same ranges and the same combine as the one the layer wears — never a second composition path
# (SACRED_LAW.md).
# WHICH MODEL EACH SIDE IS LIT WITH — the ONE place that decides, for every caller.
#
# `wbox` / `lbox` are the Debug tab's two spin boxes as the dialog sends them: a box the user has
# typed in this session sends its 1..7 value, an untouched one sends 0 meaning "the model this side
# already carries". With nothing carried either, the fallback is 2 — and that number is what the
# Debug boxes display for an unset side, so what the box says and what the picture uses are the same
# statement (75_aquamoto.cpp `syncIllumModelSpins`).
#
# Records the resolved pair in `_AQUA_LAST_MODELS`, so the message line reports what the picture was
# ACTUALLY built with rather than what was asked for.
function _aqua_resolve_models(scene::Ptr{Cvoid}, st::_AquaState, wbox::Int, lbox::Int)
	mdl(p, dflt) = isempty(p) ? dflt : parse(Int, p["model"])
	mw = wbox > 0 ? wbox : mdl(st.illum[1], 2)
	ml = lbox > 0 ? lbox : mdl(st.illum[2], 2)
	_AQUA_LAST_MODELS[scene] = (mw, ml)
	return mw, ml
end

function _aqua_shaded_rgb(scene::Ptr{Cvoid}, st::_AquaState, G::GMTgrid, splitDryWet::Bool;
                          model_water::Int = 0, model_land::Int = 0)::Union{Array{UInt8,3},Nothing}
	splitDryWet || return nothing                     # no dry/wet split -> no two halves to combine
	# EVERY LAYER, NO GATE ON A STORED MODEL. The combined image is what this window shows, so it is
	# built at every timestep whether or not the Illumination dialog has been through here: with no
	# model stored the sides fall back to the defaults below (2, the classic reflectance).
	pw, pl = st.illum[1], st.illum[2]
	num(p, key, dflt) = (isempty(p) && return dflt; v = _get(p, key); isempty(v) ? dflt : parse(Float64, v))
	mw, ml = _aqua_resolve_models(scene, st, model_water, model_land)
	img, iw, il = aqua_shade_image(st.bat, G;
	                        method_water = mw, method_land = ml,
	                        azim_water = num(pw, "azim", 45.0), elev_water = num(pw, "elev", 30.0),
	                        azim_land  = num(pl, "azim", 45.0), elev_land  = num(pl, "elev", 30.0),
	                        cmap_water = st.watercmap, cmap_land = st.landcmap,
	                        # "Sat img": the LAND half's colours come from the satellite mosaic instead
	                        # of cmap_land. Asked at the same one door the flat composite asks
	                        # (`_aqua_land_albedo`), so the lit picture and the flat one can never be
	                        # showing different land. Nothing = that side keeps its colormap.
	                        albedo_land = _aqua_land_image(st),
	                        scene = scene)   # render the halves in THIS window — never open one
	# HANDED OVER AS IT LIES. It is a PICTURE — one pixel per node, row 1 = NORTH, col 1 = WEST — and
	# it is packed as one: the caller passes layout code 2 for it, which is exactly what a Julia
	# (ny, nx) column-major array with its first row in the north IS to `_aqua_pack_rgba`. Re-ordering
	# it into the grid's element order here was a second place that had to agree about layout, and it
	# is the place the water half was lost.
	A = img.image
	nx, ny = _grid_dims(G)
	(size(A, 1) == ny && size(A, 2) == nx) ||
		error("Aquamoto: the illuminated picture is $(size(A,1))x$(size(A,2)) for a $(ny)x$(nx) grid")
	_AQUA_LAST_COMBINED[scene] = img                  # the button shows THIS, never a second build
	_AQUA_LAST_HALVES[scene] = (iw, il)               # …and the Debug tab's two side buttons show THESE
	return A
end

# The two halves of the last build, exactly as the combine took them: water first, land second. The
# Debug tab's "Water side" / "Land side" buttons show these — never a half built a second time for
# looking at, which would be a second composition path (SACRED_LAW.md).
const _AQUA_LAST_HALVES = Dict{Ptr{Cvoid},Tuple{Array{UInt8,3},Array{UInt8,3}}}()

# The last combined image this window drew, kept so the "Combined image" button can show exactly what
# is on the layer without paying for a rebuild — the picture is the product of the slice that just ran.
const _AQUA_LAST_COMBINED = Dict{Ptr{Cvoid},Any}()

# The model number each side was ACTUALLY lit with on the last build — the resolved pair, after the
# Debug boxes' override and the stored-model fallback have both been applied. Reported by the
# "Combined image" button, so the message line states what made the picture rather than what was
# asked for.
const _AQUA_LAST_MODELS = Dict{Ptr{Cvoid},Tuple{Int,Int}}()

# The one window the "Combined image" button owns. Closed and replaced at every press — see
# `_aqua_combined_popup`. Still used by `_aqua_side_popup`; the combined image itself now lands in
# the Aquamoto window as a handle of its own.
const _AQUA_POPUP_WIN = Ref{Ptr{Cvoid}}(C_NULL)

# The Scene Objects row the rendered image lands in. ONE name for both doors — the Debug tab's
# "Combined image" button and the netCDF tab's "Rendered image" box — so a second build replaces the
# row instead of stacking a second handle under it.
const AQUA_COMBINED_NAME = "Rendered image"

# The OFF-SCREEN staging window the method-1 render is drawn in and photographed from. One per
# session, kept and reused — see `_aqua_capture_combined`.
const _AQUA_STAGE_WIN = Ref{Ptr{Cvoid}}(C_NULL)

# The two half grids of the slice the staging window last built, and which slice they are: the split
# depends on the layer and on nothing else, so a second press on the same slice reuses them.
const _AQUA_STAGE_HALVES = Ref{Any}(nothing)

# …and which slice's SURFACES are currently standing in that window, so they are rebuilt only when
# the slice changes. Only the light differs between presses.
const _AQUA_STAGE_LOADED = Ref{Any}(nothing)

# …and what the staging LAND surface was last built wearing: (state, ("Sat img", method, azim, elev)).
const _AQUA_STAGE_LANDKEY = Ref{Any}(nothing)

# The LAND grid, built once per file: the bathymetry never changes with the timestep.
const _AQUA_STAGE_LAND = Ref{Any}(nothing)

# …and its rendered PICTURE, cached against the only three things it can depend on — the file, the
# land method and the land sun. A press that only moved the wave reuses it and skips a whole
# render + capture.
const _AQUA_LAND_SHOT = Ref{Any}(nothing)

# …AND IT IS KEPT ONLY WHILE IT IS BEING USED. An off-screen window is still a whole viewer — a GL
# context, a render window, two surfaces and their textures, plus the grids and pictures cached below
# it — and holding that for the rest of the session so that a press five minutes from now saves a
# quarter of a second is the wrong trade: measured, opening it costs 0.145 s and closing it 0.107 s,
# once, against a window's worth of memory held for ever. It is therefore torn down after
# `_AQUA_STAGE_IDLE_S` with no build, and rebuilt by the next one — a run of slices (where the saving
# actually matters, one press after another) never reaches the timeout.
const _AQUA_STAGE_IDLE_S = 60.0
const _AQUA_STAGE_WATCH  = Ref{Union{Nothing,Timer}}(nothing)   # the idle watchdog, one per session
const _AQUA_STAGE_USED   = Ref{Float64}(0.0)                    # time() of the last build
const _AQUA_STAGE_BUSY   = Ref{Bool}(false)                     # a build is in flight: never close under it

# The staging window and everything cached about what stood in it. Called by the watchdog, and safe
# to call at any time: the next build simply opens a fresh one.
function _aqua_stage_close!()
	h = _AQUA_STAGE_WIN[]
	_aqua_stage_forget!()             # cleared FIRST: the close fires the forget callback, which
	h == C_NULL || ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)   # comes back through here
	return
end

# Everything this session remembers ABOUT the staging window, dropped — with no call on the window
# itself. This is the half `_forget_window!` (savefile.jl) needs when the window is already dying of
# its own accord, and `_aqua_stage_close!` above is that same half plus the close: one place that
# knows what the stage owns, never two lists to keep in step.
function _aqua_stage_forget!()
	_AQUA_STAGE_WIN[]    = C_NULL
	_AQUA_STAGE_LOADED[] = nothing
	_AQUA_STAGE_HALVES[] = nothing
	_AQUA_STAGE_LAND[]   = nothing
	_AQUA_LAND_SHOT[]    = nothing
	_AQUA_STAGE_LANDKEY[] = nothing
	if _AQUA_STAGE_WATCH[] !== nothing
		close(_AQUA_STAGE_WATCH[])
		_AQUA_STAGE_WATCH[] = nothing
	end
	return
end

# Mark the staging window used, and arm the watchdog if it is not already running. The watchdog ticks
# on a Julia `Timer`, i.e. on this same thread between tasks — never inside a build, which is what
# `_AQUA_STAGE_BUSY` states for the case where a tick lands on a pump inside one.
function _aqua_stage_touch!()
	_AQUA_STAGE_USED[] = time()
	_AQUA_STAGE_WATCH[] === nothing || return
	_AQUA_STAGE_WATCH[] = Timer(_AQUA_STAGE_IDLE_S / 4, interval = _AQUA_STAGE_IDLE_S / 4) do _
		try
			(_AQUA_STAGE_BUSY[] || _AQUA_STAGE_WIN[] == C_NULL) && return
			time() - _AQUA_STAGE_USED[] < _AQUA_STAGE_IDLE_S && return
			_aqua_stage_close!()
		catch
		end
	end
	return
end

# THE SEA BED's own staging window and its own picture — the REAL bathymetry, unclamped, which the
# water is blended over when "Water transparency" is not zero. A window of its own so that its z
# span (down to the abyssal plain) cannot reach the water's pass and flatten its relief.
const _AQUA_BATHY_WIN  = Ref{Ptr{Cvoid}}(C_NULL)
const _AQUA_BATHY_SHOT = Ref{Any}(nothing)

# A mask grown by ONE node in every direction (8-connected), in GRID SPACE.
#
# Needed by `_aqua_capture_combined` below to close the shoreline. It is a NEIGHBOUR test, which is
# the one thing a raw grid buffer cannot answer: these grids are read "TRB" (row-major), so `M[i±1,j]`
# on `G.z` walks the WRONG axis and the ring smears into slabs across the sea (SACRED_LAW.md, grid
# memory-layout law — the caller hands this `_zmat`'s view, never `G.z`).
function _aqua_dilate1(M::AbstractMatrix{Bool})::Matrix{Bool}
	ny, nx = size(M)
	D = Matrix{Bool}(M)
	@inbounds for j in 1:nx, i in 1:ny
		M[i, j] || continue
		for jj in max(1, j - 1):min(nx, j + 1), ii in max(1, i - 1):min(ny, i + 1)
			D[ii, jj] = true
		end
	end
	return D
end

# THE COMBINED IMAGE, MADE THE ONLY WAY METHOD 1 CAN EXIST: RENDERED, THEN PHOTOGRAPHED.
#
# Method 1 is VTK'S OWN PBR RENDER. It happens on a SURFACE, on the GPU. Two routes were tried and
# neither can produce it, for the same reason:
#   * `aqua_shade_image` composites two half PICTURES on the CPU — a picture has no surface to
#     render, so method 1 silently degrades to a flat shaded look;
#   * capturing the AQUAMOTO LAYER gives the same thing, because that layer IS that composite,
#     draped as a flat texture. Photographing it returns the CPU composite, method 1 or not.
# So the two halves are put up AS TWO SURFACES, each lit by name through the Illumination tool's own
# door, the renderer draws them, and THAT is photographed. The staging window is closed before this
# returns — what comes back is an IMAGE, nothing is left on screen.
#
# THE TWO RINGS. A surface is built CELL by cell and `makeGridFromArray` DROPS any cell with a NaN
# corner, so two halves cut on the same shoreline both drop the cells straddling it — a one-node gap
# belonging to neither, through which the window shows its NaN plane (white by default). Each half
# is therefore extended ONE NODE into the other:
#   * the LAND takes its wet neighbours' ground, set a hair BELOW the stage so the water covers it
#     and its colour is never seen — it is there for geometry only;
#   * the WATER takes the mean of its OWN wet neighbours, never the dry node's own stage (at a dry
#     node the stage IS the ground, tens of metres up, which falls off the water palette and paints
#     the coast in the palette's extreme colour instead).
# The water ends up δ above the land ring, so the waterline is drawn by the water.
#
# Returns the captured GMTimage, or `nothing` if the window could not be built.
# THE SEA BED, RENDERED ONCE, FOR "WATER TRANSPARENCY" TO SHOW THROUGH.
#
# The land surface the combine uses is FLATTENED under the shore (see `_aqua_land_grid`), because its
# z span feeds the window's relief normalisation and a surface running to -4000 m flattens the
# water's own relief to nothing. That flat plateau is exactly the wrong thing to look at through
# transparent water, so the sea bed gets a picture of its OWN: the real bathymetry, unclamped,
# rendered in a window of its OWN so its span cannot reach the water's pass.
#
# Built on the FIRST request that needs it and kept for the session — it is the same bathymetry at
# every timestep and under every water setting, so it depends only on the file, the land palette and
# this side's light.
function _aqua_bathy_shot(st::_AquaState, model::Int, az::Float64, el::Float64,
                          w::Float64, e::Float64, s::Float64, n::Float64)
	hit = _AQUA_BATHY_SHOT[]
	(hit !== nothing && hit[1] === st && hit[2] == model && hit[3] == az && hit[4] == el) && return hit[5]
	# IT IS ABOUT TO BUILD, SO IT SAYS SO (SACRED_LAW.md, no-dead-time law). The FIRST press with the
	# transparency slider off zero pays for a second staging window and a render of the whole
	# bathymetry — measured 1.65 s against 1.18 s for every press after it — and a dialog that sits
	# there doing nothing for a second and a half cannot be told from one that has hung. Raised
	# through the app's ONE busy notice, never a second one of this tool's own, and taken down in a
	# `finally` so no path can leave it up.
	ccall(_fn(:gmtvtk_busy_show), Cvoid, (Cstring,), "Rendering the sea bed…")
	try
	h = _AQUA_BATHY_WIN[]
	if h == C_NULL
		h = ccall(_fn(:gmtvtk_open_empty_offscreen), Ptr{Cvoid}, (Cstring,), "Sea bed staging")
		h == C_NULL && return nothing
		_register_fig!(QtEmpty(h))
		_aqua_mark_window(h)        # a tsunami window: its halves keep the NaN-hole rim
		_AQUA_BATHY_WIN[] = h
		_pump_once()
		_add_grid_to_scene(h, st.bat, AQUA_LAND; cmap = st.landcmap, promote = true, record = false)
		ccall(_fn(:gmtvtk_set_surface_name_h), Cvoid, (Ptr{Cvoid}, Cstring), h, AQUA_LAND)
		_remember_object!(h, :grid, AQUA_LAND, st.bat)
		ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, "flat2d=1;")
		ccall(_fn(:gmtvtk_set_capture_scale_h), Cvoid, (Ptr{Cvoid}, Cint), h, Cint(1))
		for _ in 1:20; _pump_once(); end
	end
	_on_hillshade(h, "model=$model\ngrid=$AQUA_LAND\nazim=$az\nelev=$el\n")
	img = _display_image(h, w, e, s, n)   # no pumps: see `shoot` in `_aqua_capture_combined`
	img === nothing || (_AQUA_BATHY_SHOT[] = (st, model, az, el, img))
	return img
	finally
		ccall(_fn(:gmtvtk_busy_close), Cvoid, ())
	end
end

# THE LAND SURFACE, BUILT ONCE PER FILE. The bathymetry as it is — no NaN, no ring, no dependence on
# which timestep is showing. Its palette spans the LAND (nodes at or above sea level), so it is the
# same set of colours at every slice; the sea bed it also covers is never read, because the merge
# takes a land pixel only where the dry/wet mask says dry.
function _aqua_land_grid(st::_AquaState)
	hit = _AQUA_STAGE_LAND[]
	(hit !== nothing && hit[1] === st) && return hit[2]
	Gl = deepcopy(st.bat)
	land = filter(v -> isfinite(v) && v >= 0, Gl.z)
	fin  = filter(isfinite, Gl.z)
	isempty(fin) && return Gl
	lo, hi = isempty(land) ? (Float64(minimum(fin)), Float64(maximum(fin))) :
	                         (Float64(minimum(land)), Float64(maximum(land)))
	# …AND DOWN TO THE LOWEST STAGE THE WAVE EVER REACHES, over the WHOLE cube.
	#
	# The clamp level is what the wet part of this surface is flattened to, so it has to stay UNDER
	# the water at every timestep: set at the shore's own level it sits ABOVE a deep trough, and the
	# sea bed would surface through the wave — the land showing where it is supposed to be covered.
	# The cube's own per-layer wet minima are already scanned at open time, so the global floor costs
	# a `minimum` over a vector, and it is the same number at every slice.
	sc = get(st.scans, st.varname, nothing)
	if sc !== nothing && !isempty(sc.wetlo)
		wet = [sc.wetlo[k] for k in eachindex(sc.wetlo) if sc.wetany[k] && isfinite(sc.wetlo[k])]
		isempty(wet) || (lo = min(lo, minimum(wet)))
	end
	# FLATTENED AT SEA LEVEL BELOW THE SHORE, not carried down to the sea bed.
	#
	# The surface has to cover every node so there are no dropped cells at the coast, but its Z SPAN
	# is not free: it feeds the window's relief normalisation, and a land grid running to -4000 m
	# made the water's own relief negligible beside it — the PBR pass then lit the sea almost
	# uniformly and the picture came back as flat palette colour, a pure white sea with a blown-out
	# wave. Clamping the wet part to the shore's own level keeps the span exactly what the old
	# dry-only half had, and those nodes are under the water and never read by the merge anyway.
	lof = Float32(lo)
	Gl.z .= max.(Gl.z, lof)
	Gl.range[5], Gl.range[6] = lo, hi
	_AQUA_STAGE_LAND[] = (st, Gl)
	return Gl
end

function _aqua_capture_combined(st::_AquaState, G::GMTgrid, mw::Int, ml::Int,
                                azw::Float64, elw::Float64, azl::Float64, ell::Float64)
	# THE SPLIT IS CACHED PER SLICE. It depends on the layer and nothing else — not on the methods,
	# not on the sun — so pressing the button again on the same slice must not pay for it twice
	# (0.195 s of a ~1 s press, measured). Keyed by the state object and the slice index, so a new
	# slice, a new file or another window all miss it and rebuild.
	local Gw, Gl, drym, ny_, nx_
	hit = _AQUA_STAGE_HALVES[]
	if hit !== nothing && hit[1] === st && hit[2] == st.cur
		Gw, Gl, drym = hit[3], hit[4], hit[5]
		ny_, nx_ = size(drym)
		@goto staged
	end
	bat = st.bat
	dry = _aqua_indland(bat.z, G.z)                  # element-wise, both buffers as they lie
	any(dry) || return nothing                       # no dry/wet split -> nothing to combine
	Gw = deepcopy(G);  Gw.z[dry] .= NaN32            # WATER: the wave, nothing else

	# THE LAND HALF IS THE WHOLE BATHYMETRY, AND IT NEVER CHANGES.
	#
	# It used to be the slice's DRY nodes plus a one-node ring, which made it depend on the timestep
	# and cost a rebuild, a re-add and a render on every press. But the sea floor does not move: what
	# moves is the SHORELINE, and that is the water's business — the merge below reads a land pixel
	# only where the mask says dry, so a land surface that also covers the sea bed costs nothing and
	# is the same surface at every timestep. It also retires the land's coastal ring: a surface with
	# no NaN has no dropped cells to fill.
	#
	# Its palette spans the land, i.e. the nodes at or above sea level — a fixed set, so the colours
	# do not shift from slice to slice either. (The previous per-slice dry range came out
	# -0.084 .. 216.8 on this file, which is that same set.)
	Gl = _aqua_land_grid(st)

	drym = _aqua_indland(_zmat(bat), _zmat(G))       # the SAME test, in grid space
	ny_, nx_ = size(drym)
	Zw, Zm = _zmat(Gw), _zmat(G)
	wring = _aqua_dilate1(.!drym) .& drym
	@inbounds for j in 1:nx_, i in 1:ny_
		wring[i, j] || continue
		acc = 0.0f0; cnt = 0
		for jj in max(1, j - 1):min(nx_, j + 1), ii in max(1, i - 1):min(ny_, i + 1)
			(drym[ii, jj] || !isfinite(Zm[ii, jj])) && continue
			acc += Float32(Zm[ii, jj]); cnt += 1
		end
		cnt > 0 && (Zw[i, j] = acc / cnt)
	end
	# The water's palette spans ITS OWN data, exactly as a plain grid's does. (The land's is fixed —
	# see `_aqua_land_grid`.)
	let v = view(Gw.z, .!dry)
		Gw.range[5], Gw.range[6] = Float64(minimum(v)), Float64(maximum(v))
	end
	_AQUA_STAGE_HALVES[] = (st, st.cur, Gw, Gl, drym)
	@label staged

	# THE STAGING WINDOW IS OFF-SCREEN AND IT IS KEPT.
	#
	# It used to be a normal window opened and closed on every press: a white, empty window flashing
	# up for the half-second the build took (measured: 0.145 s to open + 0.107 s to close, plus the
	# flash itself). `gmtvtk_open_empty_offscreen` builds it exactly as `gmtvtk_open_empty` does —
	# one builder — and then moves it off every screen at zero opacity, so it renders (the capture
	# reads the BACK buffer, which does not care where the window is) and is never seen.
	#
	# Kept for the session and reused, so that cost is paid once instead of on every press. Its
	# contents are replaced each time: the two surfaces are removed by name before the new pair goes
	# in, the same remove-then-add `_aqua_drop_var_rows` uses against stale same-named handles.
	h = _AQUA_STAGE_WIN[]
	if h == C_NULL
		h = ccall(_fn(:gmtvtk_open_empty_offscreen), Ptr{Cvoid}, (Cstring,), "Combined image staging")
		h == C_NULL && return nothing
		_register_fig!(QtEmpty(h))
		_aqua_mark_window(h)        # a tsunami window: its halves keep the NaN-hole rim
		_AQUA_STAGE_WIN[] = h
		_pump_once()
	end
	_aqua_stage_touch!()          # used now: the idle watchdog restarts its count (and is armed once)
	_AQUA_STAGE_BUSY[] = true
	try
		# THE SURFACES ARE REBUILT ONLY WHEN THE SLICE CHANGES. They are the same two grids at every
		# press on the same layer; only the LIGHT differs, and that is pushed onto surfaces already
		# standing. Removing and re-adding them was costing 0.22 s a press for an identical result.
		loaded = _AQUA_STAGE_LOADED[]
		needW = loaded === nothing || loaded[1] !== st || loaded[2] != st.cur
		# THE LAND SURFACE AND WHAT IT WEARS. With "Sat img" it wears the satellite picture, LIT with the
		# land's method and sun — the light is baked into that drape — so it is added again whenever any
		# of those changes; otherwise once per file, as before.
		lkey  = st.satimg ? (true, ml, azl, ell) : (false, 0, 0.0, 0.0)
		lk    = _AQUA_STAGE_LANDKEY[]
		needL = lk === nothing || lk[1] !== st || lk[2] != lkey
		if needW
			# ONLY THE WATER SURFACE IS REPLACED per slice. The land's is the same grid at every timestep
			# (see `_aqua_land_grid`).
			if loaded !== nothing
				ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), h, AQUA_WATER)
				_forget_object!(h, :grid, AQUA_WATER)
			end
			# The water's palette over the SYMMETRIC scale `_aqua_water_range` gives, so a diverging
			# palette's centre sits on the calm sea instead of wherever the slice's extremes leave it.
			_add_grid_to_scene(h, Gw, AQUA_WATER; cmap = st.watercmap, promote = true,
			                   zrange = _aqua_water_range(Gw), record = false)
			ccall(_fn(:gmtvtk_set_surface_name_h), Cvoid, (Ptr{Cvoid}, Cstring), h, AQUA_WATER)
			_remember_object!(h, :grid, AQUA_WATER, Gw)
		end
		if needL
			if lk !== nothing
				ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), h, AQUA_LAND)
				_forget_object!(h, :grid, AQUA_LAND)
			end
			_add_grid_to_scene(h, Gl, AQUA_LAND; cmap = st.landcmap, promote = false, record = false,
			                   drape = st.satimg ? _aqua_sat_lit(st, Gl, ml, st.illum[2]) : nothing)
			# …AND SHOWN: a grid added to a window that already has one is registered hidden, and
			# this one is not an alternative view of the layer, it is HALF OF IT.
			ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), h, AQUA_LAND, Cint(1))
			_AQUA_STAGE_LANDKEY[] = (st, lkey)
		end
		if needW || needL
			# STRAIGHT DOWN, through the ONE 2D/3D door, and captured at SCALE 1: `SetScale(n>1)` builds
			# the frame from n x n TILES with the camera window shifted per tile, and every view-dependent
			# term — which is all of PBR with image-based lighting — then differs between them, so the
			# tile seams show as a cross through the picture. More pixels come from a bigger window.
			ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, "flat2d=1;")
			ccall(_fn(:gmtvtk_set_capture_scale_h), Cvoid, (Ptr{Cvoid}, Cint), h, Cint(1))
			_AQUA_STAGE_LOADED[] = (st, st.cur)
			for _ in 1:20; _pump_once(); end
		end
		w, e, s, n = Float64(Gw.range[1]), Float64(Gw.range[2]), Float64(Gw.range[3]), Float64(Gw.range[4])
		# ONE PASS: only the side being lit is on screen.
		#
		# A method is applied to the window's ACTIVE layer — `sceneSetReliefLook` writes
		# `activeLook(s)` and `_on_hillshade` drops the grid name entirely for the look models
		# (1/5/6/7). With both surfaces up, which of the two received the method was decided by
		# `resolveActiveGrid`, not by the name sent: pressing with water=2, land=1 left the water on
		# the PREVIOUS press's PBR look, so it rendered as method 1 whatever the box said. Hiding the
		# other side makes the active layer the one being lit, by construction. Its pixels are the
		# only ones this pass contributes, so what the hidden side would have shown does not matter.
		vis(nm, on) = ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint),
		                    h, nm, Cint(on))
		shoot(model, gname, az, el) = begin
			vis(gname, 1)
			vis(gname == AQUA_WATER ? AQUA_LAND : AQUA_WATER, 0)
			_on_hillshade(h, "model=$model\ngrid=$gname\nazim=$az\nelev=$el\n")
			# NO PUMPS AT ALL BEFORE THE CAPTURE. Every call above applies itself synchronously on the
			# C++ side, and the capture then forces its OWN deterministic re-render
			# (`ShouldRerenderOn`), so the event loop has nothing to contribute. And it is not free:
			# after a light push each pump does the re-shading work over again — measured 12 ms each,
			# so the six that used to sit here cost 71 ms PER PASS, 142 ms of a 230 ms build. Checked
			# against six pumps on the same request: the captured image is identical, sample for
			# sample. If a future change ever needs one here, it needs a reason and a measurement.
			_display_image(h, w, e, s, n)
		end

		# ANY COMBINATION OF METHODS, BY RENDERING ONCE PER METHOD.
		#
		# A method is not per grid. Models 1/5/6/7 are LOOKS and `_on_hillshade` (hillshade.jl) sends
		# them straight to the look setter, which DROPS the grid name and writes the window's active
		# layer; models 2/3/4 are reflectances and are pushed into the window's one `ExternShade`
		# (or, on a real Aquamoto layer, into one of its two SIDE slots — slots this staging window
		# does not have). Either way, two grids in one window cannot each hold their own method: the
		# second request overwrote the first, so the LAND always won and the water's box did nothing.
		# Measured: changing the water model moved exactly 0 pixels, changing the land model moved
		# 2 470 126.
		#
		# So the window is rendered ONCE PER METHOD and each side's pixels are taken from the pass
		# that was lit its way. Same surfaces, same camera, same size — the two captures are aligned
		# by construction, so this is a SELECTION, not a blend: no pixel is ever mixed or resampled.
		# Equal methods need one pass, which is the common case and the fast one.
		# ALWAYS TWO PASSES, equal methods included: each pass is defined by its side being the only
		# thing on screen, so there is no one-pass form of it. The cost is one extra render + capture
		# (~0.1 s), and it buys a method that is applied to the layer it names, every time.
		imgW = shoot(mw, AQUA_WATER, azw, elw)
		# THE LAND PASS IS SKIPPED WHILE ITS ANSWER CANNOT HAVE CHANGED. The land surface is the same
		# at every timestep, so its picture depends only on its METHOD and its SUN — not on the slice.
		# Cached against exactly those three, it is rendered once and reused, which takes a whole
		# render + capture out of every press that only moved the wave.
		# "Sat img" is in the key: it changes what the land wears. A satellite land is never shot with
		# method 1 (its PBR render washes a photograph grey) — lit as 2, anonymously, as everywhere.
		lhit = _AQUA_LAND_SHOT[]
		imgL = (lhit !== nothing && lhit[1] === st && lhit[2] == ml && lhit[3] == azl && lhit[4] == ell &&
		        lhit[5] == st.satimg) ? lhit[6] : nothing
		if imgL === nothing
			imgL = shoot((st.satimg && ml == 1) ? 2 : ml, AQUA_LAND, azl, ell)
			imgL === nothing || (_AQUA_LAND_SHOT[] = (st, ml, azl, ell, st.satimg, imgL))
		end
		(imgW === nothing || imgL === nothing) && return imgW

		# THE PICK, BY THE SAME DRY/WET MASK THE HALVES WERE CUT BY — never a test made again here.
		# The picture is "TRBa": (col, row, band), row 1 = NORTH; `drym` is (iy, ix) with row 1 = SOUTH.
		A = imgW.image;  B = imgL.image
		nxp, nyp = size(A, 1), size(A, 2)
		# The pixel -> node maps are COLUMN-wise and ROW-wise separable, so they are built once as two
		# small vectors instead of a `round`+`clamp` per pixel (2.1 M of them, ~0.1 s a press).
		ixs = [clamp(round(Int, (c - 1) / max(nxp - 1, 1) * (nx_ - 1)) + 1, 1, nx_) for c in 1:nxp]
		iys = [clamp(ny_ - round(Int, (r - 1) / max(nyp - 1, 1) * (ny_ - 1)), 1, ny_)  for r in 1:nyp]
		# WATER TRANSPARENCY, at last honoured. `st.transp` (0 = opaque, 1 = clear) is the dialog's
		# slider, and it was read and then ignored: the composite had nothing to show underneath. Here
		# the sea bed exists as its own picture (`_aqua_bathy_shot`), so a wet pixel is the water over
		# it: out = (1-t)*water + t*seabed. t = 0 leaves the water untouched and costs nothing.
		t = clamp(Float64(st.transp), 0.0, 1.0)
		Sb = nothing
		if t > 0
			imgS = _aqua_bathy_shot(st, ml, azl, ell, w, e, s, n)
			imgS === nothing || (Sb = imgS.image)
		end
		# TWO LOOPS, NOT ONE WITH A BRANCH. Opaque water is the normal case and it is a pure SELECTION
		# — the wet pixels are already right and must not be touched at all. Carrying the blend's test
		# into that loop cost ~0.05 s a press for a branch that never fired.
		if Sb === nothing
			@inbounds for r in 1:nyp
				iy = iys[r]
				for c in 1:nxp
					drym[iy, ixs[c]] || continue         # wet: the water pass already holds it
					A[c, r, 1] = B[c, r, 1];  A[c, r, 2] = B[c, r, 2];  A[c, r, 3] = B[c, r, 3]
				end
			end
		else
			tf, tc = Float32(t), Float32(1 - t)
			@inbounds for r in 1:nyp
				iy = iys[r]
				for c in 1:nxp
					if drym[iy, ixs[c]]
						A[c, r, 1] = B[c, r, 1];  A[c, r, 2] = B[c, r, 2];  A[c, r, 3] = B[c, r, 3]
					else                                 # wet AND see-through: water over the sea bed
						for bb in 1:3
							A[c, r, bb] = round(UInt8, clamp(tc * A[c, r, bb] + tf * Sb[c, r, bb], 0f0, 255f0))
						end
					end
				end
			end
		end
		return imgW
	finally
		_AQUA_STAGE_BUSY[] = false
		_AQUA_STAGE_USED[] = time()   # the idle clock runs from the END of the build, not its start
		_pump_once()      # the staging window is kept (off-screen) until it goes idle — see above
	end
end

# THE COMBINED IMAGE, ON DEMAND, IN A DISPLAY OF ITS OWN.
#
# Called by the Aquamoto dialog's "Combined image" button (75_aquamoto.cpp), never on its own: it
# builds the picture for the layer currently on screen through `aqua_shade_image` — the SAME function
# the drape uses, so what pops up is what the layer wears — and opens it in a new window.
#
# `model_water` / `model_land` are the Debug tab's two 1..7 boxes (0 = that side's stored model), and
# `t0` is the UNIX EPOCH SECOND THE BUTTON WAS PRESSED, read on the C++ side. The elapsed time is
# taken right before the picture is handed to a new iGMT window and PRINTED, which is how it reaches
# the dialog's message text window: what this function prints is what the caller shows.
#
# TEMPORARY, and here to be looked at.
"""
    _aqua_render_image_show(scene, on) -> Cint

The dialog's box and the "Rendered image" row in Scene Objects are one state: unchecking the box
UNCHECKS the row (the handle stays, with its picture), checking it checks the row again. Nothing is
removed — the row is the user's to Remove.
"""
function _aqua_render_image_show(scene::Ptr{Cvoid}, on::Bool)::Cint
	r = ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint),
	          scene, AQUA_COMBINED_NAME, Cint(on))
	_pump_once()
	return r
end

function _aqua_combined_popup(scene::Ptr{Cvoid}, model_water::Int = 0, model_land::Int = 0,
                              t0::Float64 = 0.0)::Cint
	st = get(_AQUA, scene, nothing)
	# A REFUSAL SAYS WHY. These three used to return a bare 0, so a press (or a slice change) that
	# built nothing looked like a control doing nothing at all.
	(st === nothing) && (print("no tsunami file open in this window"); return Cint(0))
	G = _aqua_layer(st, st.cur)
	G === nothing && (print("layer $(st.cur + 1) could not be read"); return Cint(0))
	st.split || (print("needs Split Dry/Wet — without it there are no two halves to combine");
	             return Cint(0))
	# NOTHING IS COMPOSITED FOR THIS BUTTON ANY MORE.
	#
	# `_aqua_shaded_rgb` used to run here: the CPU composite of two half PICTURES, 0.32 s, and its
	# result was then thrown away because what pops up is the RENDER, captured. It is gone from this
	# path. The two things it also did are done directly instead — the model pair is resolved by
	# `_aqua_resolve_models` (the same rule, in one place now), and the halves the Debug tab's two
	# side buttons show are built by `_aqua_side_popup` itself, which calls `_aqua_shaded_rgb` on its
	# own and always did.
	mw, ml = _aqua_resolve_models(scene, st, model_water, model_land)
	pw, pl = st.illum[1], st.illum[2]
	numv(p, key, dflt) = (isempty(p) && return dflt; v = _get(p, key); isempty(v) ? dflt : parse(Float64, v))
	# WHERE THE TIME GOES, SPLIT. `t0` is stamped in C++ at the CLICK, so the single number this used
	# to print covered the `runBlocking` bridge crossing (Qt -> the Julia console -> back) as well as
	# the work. They are very different things to look at: the build is what this code controls, the
	# bridge is what the door costs. Both are reported.
	tin = time()
	img = _aqua_capture_combined(st, G, mw, ml,
	                             numv(pw, "azim", 45.0), numv(pw, "elev", 30.0),
	                             numv(pl, "azim", 45.0), numv(pl, "elev", 30.0))
	(img === nothing) && (print("the render could not be captured"); return Cint(0))
	tbuilt = time()
	# EACH PRESS UNDOES THE LAST. The previous popup is closed before a new one opens, so this button
	# can never leave a pile of windows behind — every one of them is a live scene the event loop
	# carries for the rest of the session, which is exactly how a "look at this" button ends up
	# slowing the tool it belongs to. One window, replaced, and nothing else kept.
	if _AQUA_POPUP_WIN[] != C_NULL
		ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), _AQUA_POPUP_WIN[])
		_AQUA_POPUP_WIN[] = C_NULL
		_pump_once()
	end
	# THE CLOCK STOPS HERE — the last instant before the image leaves for a window of its own, which is
	# what the button was asked to time. The line also STATES WHAT MADE THE PICTURE: the models the
	# build resolved to (not the ones asked for), the dry/wet split it was combined by and the water
	# span it was coloured over — the three inputs that decide what the water half looks like, so a
	# picture that comes out wrong says why on the spot instead of having to be guessed at.
	mw, ml = get(_AQUA_LAST_MODELS, scene, (0, 0))
	dry = _aqua_indland(st.bat.z, G.z)
	nd, nw = count(dry), count(.!dry)
	ws = _aqua_water_span(G.z, .!dry)
	print("layer $(st.cur + 1): water model $(mw), land model $(ml), dry $(nd) / wet $(nw), ",
	      "water span ", round(ws[1]; digits = 3), " .. ", round(ws[2]; digits = 3))
	# INTO THIS WINDOW, AS ITS OWN HANDLE — no second iGMT window.
	#
	# A new window costs ~0.34 s of the press (measured) and leaves a whole live scene behind for the
	# event loop to carry. The picture is a raster over the layer's OWN bbox, so it belongs in the
	# window the layer is in, as a row the user can toggle, inspect and Remove like any other.
	#
	# REMOVE-THEN-ADD, BY NAME: a second press REPLACES the row instead of stacking another one under
	# the same name (the pile `_aqua_drop_var_rows` exists to prevent — a checkbox that controls only
	# the first of two identically-named handles is a checkbox lying about what it controls).
	#
	# NOT RE-LIT, AND NOTHING WINDOW-WIDE IS TOUCHED. The picture already carries its light: each half
	# was lit as it was made. In its own window that was ensured with RL_None, but that call is
	# WINDOW-WIDE and here it would change the tsunami layer's look too. It is not needed: image
	# extras are left alone by the shading pass on purpose (40_shading.cpp — "applyShading
	# deliberately leaves image extras alone — they are pictures, not shaded surfaces"), so an image
	# added here is shown exactly as it was handed over.
	nm = AQUA_COMBINED_NAME
	# THE ROW IS REPAINTED, NOT REPLACED. Remove-then-add rebuilt the whole Scene Objects tree three
	# times per build (the remove, the add and the show each call `rebuildSceneObjects`) — the tsunami
	# group blinking at every slice. The handle, its plane and its bbox are the same thing at every
	# slice; only the pixels differ, so only the pixels are sent (`_update_image_pixels!`). The add is
	# what runs the first time, when there is no row yet.
	if !_update_image_pixels!(scene, nm, img)
		ccall(_fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), scene, nm)
		_forget_object!(scene, :image, nm)
		_add_image_to_scene(scene, img, nm; promote = false, record = false)
		ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, nm, Cint(1))
	end
	_pump_once()
	# THE CLOCK, SPLIT, AND STOPPED AT THE END. `t0` is stamped in C++ at the CLICK, so a single
	# number covered the `runBlocking` bridge crossing (Qt -> the Julia console -> back) as well as
	# the work, and reading it as "the build" was wrong by however long the door takes. `build` is
	# the render + capture + merge, `show` is putting the picture into the window as a handle, and
	# `bridge` is everything before this function was entered. Printed LAST, so `total` covers the
	# whole press instead of stopping before the image was on screen.
	(t0 > 0) && print(" — build ", round(tbuilt - tin; digits = 3),
	                  " s, show ",   round(time() - tbuilt; digits = 3),
	                  " s, bridge ", round(tin - t0; digits = 3),
	                  " s, TOTAL ",  round(time() - t0; digits = 3), " s")
	return Cint(1)
end

# ONE SIDE ON ITS OWN, in the same window the combined image uses. `side` 0 = water, 1 = land.
#
# What it shows is THE HALF THE COMBINE TOOK — `_AQUA_LAST_HALVES`, filled by the one build in
# `_aqua_shaded_rgb` — so the side seen alone and the side seen inside the composite are the same
# pixels, never two pictures of the same thing (SACRED_LAW.md).
function _aqua_side_popup(scene::Ptr{Cvoid}, side::Int, model_water::Int = 0, model_land::Int = 0,
                          t0::Float64 = 0.0)::Cint
	st = get(_AQUA, scene, nothing)
	(st === nothing) && return Cint(0)
	G = _aqua_layer(st, st.cur)
	G === nothing && return Cint(0)
	rgb = _aqua_shaded_rgb(scene, st, G, st.split; model_water = model_water, model_land = model_land)
	(rgb === nothing) && return Cint(0)
	nm = side == 0 ? AQUA_WATER : AQUA_LAND
	H  = get(_AQUA_HALF_GRID, nm, nothing)
	(H === nothing) && return Cint(0)
	# SHOWN THE WAY TsuIllum SHOWS IT: the half is a PLAIN GRID, so it goes into a plain grid window
	# with its own palette, and is lit by the app's illumination push with that side's own model —
	# `illuminate!`'s two steps (`_hs_push_grid` + `_hs_declare_look`), which is what TsuIllum copied
	# from here in the first place. No image is composed, nothing is drawn for it: it is the grid.
	mw, ml = get(_AQUA_LAST_MODELS, scene, (0, 0))
	model  = side == 0 ? mw : ml
	p      = st.illum[side + 1]
	cmap   = side == 0 ? st.watercmap : st.landcmap
	if _AQUA_POPUP_WIN[] != C_NULL                    # one window, replaced — same rule as the combined
		ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), _AQUA_POPUP_WIN[])
		_AQUA_POPUP_WIN[] = C_NULL
		_pump_once()
	end
	ttl = (side == 0 ? "Water side" : "Land side") * " — layer $(st.cur + 1)"
	print(ttl, ": model ", model, ", range ", round(H.range[5]; digits = 3), " .. ",
	      round(H.range[6]; digits = 3))
	(t0 > 0) && print(" — built in ", round(time() - t0; digits = 3), " s")
	# "Sat img": the land half wears the satellite picture, draped on its own surface.
	landimg = _aqua_land_drape(scene, st)
	Hw = get(_AQUA_HALF_GRID, AQUA_WATER, nothing)
	Hl = get(_AQUA_HALF_GRID, AQUA_LAND, nothing)
	(Hw === nothing) && return Cint(0)
	# ONE SCENE FOR BOTH BUTTONS: the WATER is always the window's base and the LAND is always added
	# beside it. The two buttons differ only in which half is lit, framed and carries the colour bar.
	# Built the other way round (land as base, water added) the water — an added grid, stretched by its
	# own 2 cm z range — floated over the land with a gap all along the coast.
	fig = view_grid(Hw; cmap = st.watercmap, title = ttl)
	# A tsunami window: its halves keep the NaN-hole rim. view_grid has already built the water's tile
	# pyramid, so the setter re-meshes it (sceneSetAquaWindow) before the land half is added beside it.
	_aqua_mark_window(fig.h)
	ccall(_fn(:gmtvtk_set_surface_name_h), Cvoid, (Ptr{Cvoid}, Cstring), fig.h, AQUA_WATER)
	_AQUA_POPUP_WIN[] = fig.h
	# THE LIGHT, on the window's ACTIVE layer — which is why the water is lit BEFORE the land is added
	# and the land AFTER (a last-added raster is the active one). EACH HALF WITH ITS OWN model and sun,
	# in BOTH popups: the scene is one and the same, only the side being looked at differs. (Leaving the
	# water unlit on the Land side is what left it floating as a slab above the land's zero.)
	light!(G, model, p) = begin
		az = _get(p, "azim") == "" ? "45" : _get(p, "azim")
		el = _get(p, "elev") == "" ? "30" : _get(p, "elev")
		if model == 1
			# MODEL 1 IS THE RENDER. It is not a reflectance to push: it is the window's own PBR look with
			# this side's sun — `gmtvtk_set_relief_look_h` with RL_PBR, the same call the Illumination
			# dialog makes for a plain grid.
			ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), fig.h,
			      "sunaz=$(az);sunel=$(el);")
			ccall(_fn(:gmtvtk_set_relief_look_h), Cvoid, (Ptr{Cvoid}, Cint, Cint), fig.h, Cint(1), Cint(0))
		else
			_hs_push_grid(fig.h, G, model, Dict{String,String}("azim" => az, "elev" => el), -1)
			_hs_declare_look(fig.h, -1)
		end
		_pump_once()
	end
	light!(Hw, mw, st.illum[1])
	# THE LAND STANDS BESIDE IT, so neither side is a picture with a hole in it: the nodes one half left
	# NaN are the other half's. Through `_add_grid_to_scene` + `gmtvtk_set_object_visible`, the SAME two
	# steps `_aqua_push_two_surfaces` uses (SACRED_LAW.md: same operation, same function).
	if Hl !== nothing
		_add_grid_to_scene(fig.h, Hl, AQUA_LAND; cmap = st.landcmap, promote = false,
		                   source = "$(st.path)?bathymetry", drape = landimg)
		ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint),
		      fig.h, AQUA_LAND, Cint(1))
		# THE LAND'S LIGHT — unless it wears the satellite picture: that drape IS the lit half the combine
		# took (`_hs_reflectance`, or method 1's render), and a second light pushed on the land re-shades
		# its colour-mapped surface over the drape.
		landimg === nothing && light!(Hl, ml, st.illum[2])
		# ONE SET OF AXES: the other half's is hidden. These two describe THE SAME ground, node for node,
		# so a second box only doubles the frame (the programmatic form of that raster's Axes checkbox).
		ccall(_fn(:gmtvtk_set_axes_shown_h), Cvoid, (Ptr{Cvoid}, Cstring, Cint),
		      fig.h, side == 0 ? AQUA_LAND : AQUA_WATER, Cint(0))
		# …AND THE CAMERA RE-FIT TO WHAT THE WINDOW NOW HOLDS. Flat 2-D parks the camera just above the
		# z-max of whatever raster set it — the water half, 3 m — so a companion 2 km tall sat behind it
		# and the land was simply missing until the user toggled 3-D and back.
		ccall(_fn(:gmtvtk_refit_view_h), Cvoid, (Ptr{Cvoid},), fig.h)
		# THE PILE IS LEFT ALONE. The companion arrives on top, which is where a last-added raster
		# belongs, and the colour bar follows it (resolveActiveGrid reads the pile's top). Sending it
		# to the bottom to keep the bar on the water was tried and REPAINTS THE LAND IN THE WATER'S
		# PALETTE — the half this window is about ends up lending its colours to the other half, which
		# is a worse lie than a bar naming the wrong side. Measured, not assumed: the land came back
		# white-on-white at sea level under the tsunami's diverging ramp.
		_pump_once()
	end
	return Cint(1)
end

# THE REFLECTANCE-PUSH PATH IS GONE (2026-09-19). The tsunami used to be lit by pushing a per-side
# reflectance down to the viewer, which then modulated the composite's colours. It is now lit where
# its colours are MADE: `aqua_shade_image` (aquashade.jl) splits the slice, lights each half as a
# picture of its own and combines the two by the dry/wet mask, and that single image IS the layer.
# One act, not two -- so there is no reflectance to pair back against the grid's nodes, which is
# where every layout bug in this file came from.
# WHAT MASKING A HALF BEFORE LIGHTING IT USED TO BUY — kept as the measurement, NOT as instructions:
# masking is banned (user order, 2026-09-19), so `aqua_shade_image` lights each half over its whole
# field — scoped by its RANGE, not by a blanked copy — and the consequence recorded in 1. is the
# accepted one. Read this before proposing any change to how a side's contrast is set:
#
# 1. THE FIELD IS THE WET STAGE. A tsunami stage stores the LAND ELEVATION on its dry cells (365 m in
#    aiai.nc — see this file's header and `_aquamoto_slice`'s colourbar note), so the raw array is not
#    a water surface: it is a water surface with the coastline welded into it. Every model scales its
#    intensity from the data's OWN range — model 1 on the grid's mean/sigma (-Nt), the -E models by
#    stretching the reflectance to the full [-0.95, 0.95] (grdgradient_m.c: "data must be scaled to
#    the [-1,1] interval") — so those cliffs, not the sea, decided the light. Measured on layer 66:
#      * the sea's intensity collapsed to two piles, 8.7% pinned at the -0.95 floor and ~70% inside
#        +0.24..+0.27, with nothing in between (the two tones on screen);
#      * the ELEVATION ran BACKWARDS on the water — method 2's spread GREW with sun elevation
#        (sd 0.451 at 5 deg -> 0.587 at 85) while on the bathymetry it correctly fell (0.639 -> 0.166).
#    The dry cells are dropped with `_aqua_indland`, THE dry/wet test the composite itself paints
#    with, so the stretch sees the wave field and nothing else.
#
# 2. WHAT GOES DOWN IS NEVER NaN — the dropped cells are pushed as intensity ZERO. `gmtIlluminate`
#    returns immediately on 0, so those texels keep the composite's colour byte for byte. A NaN there
#    instead makes `externShadeAt` return NaN, which sends the pixel down bakeAquaShade's OTHER branch
#    (normal-derived shading) — a different code path, and therefore a different colour, on pixels
#    this tool was never asked to touch. That is what wrecked the colours the first time.
#
# 3. THE LAND SIDE IS MASKED FOR THE SAME REASON. Every model scales from the data's OWN spread, so a
#    stretch that includes 4 km of sea floor is a different light from one over the coastal strip the
#    user is looking at — which is why the two halves shown in SEPARATE windows (TsuIllum.jl, each a
#    plain grid of its own half) looked right and the composite looked wrong: the standalone land
#    window is the dry-masked bathymetry, the composite's land was not.

# THE read of one Aquamoto cube layer (`k` 0-based): out of memory when this variable's cube has been
# pulled in by "Load all in RAM", off disk one layer at a time otherwise. Every slice path goes
# through here — the display, the movie writer, the Cinema tab — so a RAM-resident cube cannot be
# bypassed by one of them and end up reading the disk anyway (SACRED_LAW.md). The open-time per-
# variable scan is the one caller that does NOT come here, and cannot: it runs before any cube can be
# resident, and it is what decides the ranges the cache is later described by.
function _aqua_layer(st::_AquaState, k::Int)::Union{GMTgrid,Nothing}
	C = get(st.ram, st.varname, nothing)
	if C !== nothing && 0 <= k < size(C.z, 3)
		G = _cube_layer_view(C, k + 1)          # zero-copy view into the cube, same as the cube dock's
		# A view cannot inherit a z-range nobody computed. Consumers read that range off the layer
		# (the scan's `.range[5:6]`, the hillshade), so fill it here — a RAM layer and a disk layer
		# must be the same object to everything downstream.
		G.range[5], G.range[6] = _finite_extrema(G.z)
		return G
	end
	# Off disk: the display window is applied HERE, at the read, so the slice on screen has exactly
	# the extent the bathymetry and the scan were given (a RAM-resident cube was already clipped when
	# it was loaded, so the view above needs nothing).
	return _aqua_clipx(_read_cube_layer("$(st.path)?$(st.varname)", k + 1), st.xwin)
end

# "Load all in RAM" for the ACTIVE variable — the option the other netCDF cubes get from the cube
# dock's own button (`_on_cube_load_all`, drop.jl), which an Aquamoto file never reaches because it
# opens through this dialog instead. Same reader, same RAM test, same return codes:
# 0 = loaded (or already resident), 1 = would not fit in free RAM, 2 = error.
function _aqua_load_all(scene::Ptr{Cvoid})::Cint
	try
		st = get(_AQUA, scene, nothing)
		st === nothing && return Cint(2)
		# Already resident: still SAY so — a caller may be asking on a path where the dialog was never
		# told (a task started before the file was even open).
		if haskey(st.ram, st.varname)
			_aqua_report_ram(scene, true)
			return Cint(0)
		end
		# The probe is clipped too: what has to fit in RAM is what is DISPLAYED, and on a windowed cube
		# (benchmark 1) that is a fraction of the file's own columns.
		g1 = _aqua_clipx(_read_cube_layer("$(st.path)?$(st.varname)", 1), st.xwin)
		g1 === nothing && return Cint(2)
		_cube_fits_ram(length(g1.z) * sizeof(eltype(g1.z)), st.nsteps) || return Cint(1)
		C = _read_whole_cube("$(st.path)?$(st.varname)", st.nsteps)
		(C isa GMTgrid && ndims(C.z) == 3) || return Cint(2)
		C = _aqua_clipx(C, st.xwin)          # clipped ONCE, here -- every layer view then comes out right
		st.ram[st.varname] = C
		# THE CONTROLS THAT DESCRIBE RESIDENCY LEARN IT HERE, at the one place that makes it true — so
		# "Load all in RAM" freezes at "In RAM ✓" whichever route asked for the load: the button itself,
		# the Benchs checkbox, or a run/load carrying the flag.
		_aqua_report_ram(scene, true)
		return Cint(0)
	catch e
		@error "Aquamoto load-all failed" exception=(e, catch_backtrace())
		return Cint(2)
	end
end

# Is the ACTIVE variable's cube in memory? The dialog asks on re-open so its button shows the state
# the data is actually in, rather than offering a load that already happened.
function _aqua_in_ram(scene::Ptr{Cvoid})::Cint
	st = get(_AQUA, scene, nothing)
	return Cint((st !== nothing && haskey(st.ram, st.varname)) ? 1 : 0)
end

# The title suffix for slice `k` (0-based): the step's model time when the cube carries one, else the
# step number. Seconds up to an hour, then h:mm:ss — a 4400-cycle benchmark run and a multi-hour ocean
# crossing both have to read naturally.
function _aqua_title_time(st::_AquaState, k::Int)::String
	(0 <= k < st.nsteps) || return ""
	if k + 1 > length(st.times)
		return "step $(k + 1)/$(st.nsteps)"
	end
	t = st.times[k+1]
	isfinite(t) || return "step $(k + 1)/$(st.nsteps)"
	if abs(t) < 3600
		return "t = " * (isinteger(t) ? string(Int(t)) : string(round(t, digits = 2))) * " s"
	end
	h = floor(Int, t / 3600);  m = floor(Int, (t - 3600h) / 60);  s = t - 3600h - 60m
	return "t = $(h)h" * lpad(m, 2, '0') * "m" * lpad(string(round(Int, s)), 2, '0') * "s"
end

function _aqua_set_title_time(scene::Ptr{Cvoid}, st::_AquaState, k::Int)
	ccall(_fn(:gmtvtk_set_title_extra_h), Cvoid, (Ptr{Cvoid}, Cstring), scene, _aqua_title_time(st, k))
	return nothing
end

# Compute + display slice `k` (0-based). `splitDryWet` toggles the dry/wet composite; `globalMM`
# picks the whole-cube min/max over the slice's own; `transparency` (0..1) is the Water-
# transparency slider (mixe_images' cross-blend fraction — land pixels are always hard-overwritten
# with the land colour regardless of this value, matching Mirone). `shadeWater`/`shadeLand` are the
# "Shade Water"/"Shade Land" toggle buttons — see `_aqua_composite_rgb`.
function _aquamoto_slice(scene::Ptr{Cvoid}, k::Int, splitDryWet::Bool, globalMM::Bool, transparency::Float64,
                         shadeWater::Bool=true, shadeLand::Bool=true)
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	(0 <= k < st.nsteps) || error("Aquamoto: slice $k out of range (0..$(st.nsteps - 1))")
	st.cur = k                                         # the slice on screen, for the illuminated redraw
	# Record the look this slice is being drawn with, so a programmatic slice change reproduces it
	# rather than picking its own options (see `_AquaState`, and `set_layer!` in movie.jl).
	st.split, st.globalmm, st.transp = splitDryWet, globalMM, transparency
	st.shadewater, st.shadeland = shadeWater, shadeLand
	G = _aqua_layer(st, k)                                    # THE cube-layer read; k is 0-based here
	# `_aqua_layer` is declared to be able to return nothing, so say what happened here rather
	# than letting the union reach `G.z` below (and so the code after this line sees a concrete grid).
	G === nothing && error("Aquamoto: could not read layer $(k + 1) of '$(st.varname)' from $(st.path)")
	# Read in "TRB" like every other grid, and composited WHERE IT LIES: the colouring below is
	# element-wise, and the stage and the bathymetry come from the SAME file through the SAME reader,
	# so they share an element order. Only the RGBA pack (and the viewer's zhover) needs the layout.
	Z = G.z
	bat = st.bat.z
	nx, ny = _grid_dims(G)
	(size(bat) == size(Z)) || error("Aquamoto: '$(st.varname)' ($(size(Z))) and bathymetry ($(size(bat))) sizes differ")
	(_grid_layout_code(G) == _grid_layout_code(st.bat)) ||
		error("Aquamoto: '$(st.varname)' ($(G.layout)) and bathymetry ($(st.bat.layout)) have different memory layouts")

	# Colourbar min/max = the real min/max of the WATER being displayed, i.e. the actual data range of
	# exactly the cells this slice colours as water. In Split Dry/Wet that is the WET cells only (the dry
	# land cells store the land elevation, up to +200 m in `z` here -- they are painted as land, never on
	# the water scale, so they must NOT enter the water colourbar). No borrowed global, no nudge. The
	# "Scale colour to global min/max" checkbox is the only override. Colouring uses this SAME range.
	# Every range below is a plain lookup into the per-layer arrays `_aquamoto_open` already scanned
	# up front -- no rescan of `Z` needed here.
	sc = st.scans[st.varname]
	if globalMM
		waterlo, waterhi = _aqua_global_minmax(st)
	elseif splitDryWet
		if sc.wetany[k+1]
			waterlo, waterhi = sc.wetlo[k+1], sc.wethi[k+1]
		else
			waterlo, waterhi = 0.0, 1.0                    # this layer is entirely dry
		end
	else
		waterlo, waterhi = sc.alllo[k+1], sc.allhi[k+1]
	end
	(waterhi > waterlo) || (waterhi = waterlo + 1.0)     # guard an exactly-flat layer (div-by-zero only)
	# The water quantity is a DEVIATION from the rest state and its palette is DIVERGING (:polar =
	# trough / calm / crest). A range that never crosses zero — which is what a slice whose water is
	# entirely above (or below) the rest level gives, e.g. [0.006, 0.397] as the wave arrives — puts
	# every wet cell on ONE side of the ramp: the map goes flat white and the land overwrite vanishes
	# with it. Centre the scale on zero and let the amplitude set its half-width, so calm water is the
	# palette's middle at every slice and the two sides mean what they say. The "global min/max"
	# checkbox lands here too — one rule for both, no per-branch special case.
	# …THAT IS NOT WHAT A HALF SHOWN ON ITS OWN DOES, AND THE TWO MUST BE THE SAME PICTURE. Split the
	# tsunami into its water half and its land half, open each as a plain grid, and each one's palette
	# spans ITS OWN DATA EXTREMA (`_cpt_nodes`, the one scale-builder every grid in this app goes
	# through). Centring the water on zero made the composite a different picture from that half —
	# same data, same palette, different colours — so the centring is gone and the composite now
	# colours each side exactly as a grid of that side alone is coloured. (TsuIllum.jl is that
	# reference, side by side.)
	(waterhi > waterlo) || (waterhi = waterlo + 1.0)
	landhi = st.bat.range[6]                           # max land elevation, straight from the grid's OWN known range

	# THE COMPOSITE IS THE TSUNAMI. Land is coloured from the bathymetry with the LAND colormap, water
	# from the stage with the WATER one, and each side is lit from its own surface — that separation IS
	# Aquamoto (SACRED_LAW.md's two-surface law), and it is not something a display mode may drop. A
	# 3-D pass that pushed the bare stage grid with the water CPT painted the land on the water scale
	# (all red at the top of :polar) and threw the per-side illumination away; it is gone.
	#
	# What the geometry question is still good for is the FAST PATH: the same-size push repaints the
	# texture instead of rebuilding the scene (showLayerImageTail).
	# TWO SURFACES, NOT ONE COMPOSITE. The tsunami is shown the way the two halves are shown on their
	# own — water on its surface, land on its surface, in this one window — because that is the only
	# shape in which every illumination method means what it says per side. VTK (PBR) above all: it is
	# VTK's own render path and the material belongs to an ACTOR, so a single composited actor can only
	# ever carry one, which is how method 1 silently became method 7's CPU bake. Two actors, two
	# materials, two lights, two palettes, each spanning its own data — nothing shared, nothing baked
	# together (SACRED_LAW.md's two-surface law, taken all the way).
	if st.twosurf
		_aqua_push_two_surfaces(scene, st, G, bat, Z, splitDryWet, k)
		return nothing
	end
	cz, crgb, n = _aqua_side_nodes(st, 0, waterlo, waterhi)          # the water scale = the colourbar
	zhover, znx, zny, zlay = _grid_zbuf(G)             # stage buffer + the layout code the VIEWER reads it with
	r = st.bat.range
	name = basename(st.path)                           # handle named after the file, like every other layer
	# THE land albedo, asked at the one door (`_aqua_land_albedo`): the satellite mosaic when "Sat img"
	# is on, else empty, which is `_aqua_composite_rgb`'s own signal to colourise from st.landcmap the
	# way it always has. Either way it goes into the SAME cache field and is used the SAME way below —
	# the composite has no idea which source painted it, and must not.
	rgb, st.imgbat = _aqua_composite_rgb(bat, Z, splitDryWet, waterlo, waterhi, transparency, _aqua_land_albedo(st), landhi,
	                                     shadeWater, shadeLand, _aqua_side_pal(st, 0), _aqua_side_pal(st, 1))
	# THE LIGHT IS NOT IN THIS PICTURE. The composite carries the COLOURS; the Illumination tool's
	# light is a reflectance the viewer modulates them with, pushed per side (`_aqua_illuminate!`,
	# `_aqua_relight_water!` below) — one operation, one function, the same one every grid uses.
	# `aqua_shade_image` still builds the lit picture on demand for the "Combined image" button.

	# The composite was coloured element-wise off `G` itself, so the pack must follow THE GRID's own
	# layout, not the one `_grid_zbuf` hands the viewer -- those differ only in the degraded case
	# (a library too old to be told a layout, see `_grid_zbuf`), and mixing them up would shear the
	# texture exactly the way that case exists to prevent.
	rgba = _aqua_pack_rgba(rgb, _grid_layout_code(G), Int(nx), Int(ny))
	# THE dry/wet mask goes over WITH the composite it belongs to: the viewer relights water and land
	# from different sources with different lights, and it must split them exactly where this composite
	# painted them (`_aqua_indland`), never by a test of its own. A non-split slice is all water.
	lmask = splitDryWet ? _aqua_pack_landmask(_aqua_indland(bat, Z), _grid_layout_code(G), Int(nx), Int(ny)) :
	                      zeros(UInt8, Int(nx) * Int(ny))
	ok = ccall(_fn(:gmtvtk_show_layer_rgba_h), Cint,
		(Ptr{Cvoid}, Ptr{Cuchar}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cfloat}, Cstring, Cint, Ptr{Cuchar}),
		scene, rgba, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(st.geog), cz, crgb, Cint(n), zhover, name, zlay, lmask)
	(ok == 0) && error("Aquamoto: the viewer rejected the update (window closed?)")
	# The water now stands on a NEW surface, so its reflectance is recomputed from THIS slice's stage.
	# No-op unless the Illumination tool has a model loaded; the LAND side needs nothing here, its
	# surface (the bathymetry) is the same one at every timestep.
	_aqua_relight_water!(scene, st, G)
	# THE GRID THIS WINDOW IS SHOWING is the stage of the slice just drawn — not the bathymetry.
	# Everything that asks Julia for "the grid on display" by the window's Scene Objects name reaches
	# this entry: `_find_object`, and through it `_extract_profile` (a drawn line's "Extract profile"),
	# Save, and every grid tool. It used to hold `st.bat`, registered once on the first slice, so a
	# profile taken along a drawn line came back as the SEA FLOOR while the Ctrl-drag profile — which
	# ray-samples the surface actually on screen — correctly followed the water. One window, one
	# answer to "what is displayed" (SACRED_LAW.md); the bathymetry is reachable as `st.bat` by the
	# two-surface code that genuinely means the land side.
	_forget_object!(scene, :grid, name)
	_remember_object!(scene, :grid, name, G)
	# WHEN this slice is, in the viewer window's title, right after the zoom percentage. The cube's own
	# `time` coordinate; a file without one shows the step number alone.
	_aqua_set_title_time(scene, st, k)
	if st.first
		_session_record!(scene, :basegrid, :file, st.path; name = name)
		st.first = false
	end
	return nothing
end

# THE DEFAULT η(x) TRACK, AS A LINE IN THE WINDOW. The figure samples the grid row the dialog's
# "x from"/"length" boxes describe; this is that row, drawn, so the user can see where the curve comes
# from. Its own Scene Objects row, like every other vector element, and its own name so showing it
# again replaces it instead of stacking a second one.
#
# HELD AT A FIXED HEIGHT ABOVE THE WATER (`AQUA_TRACK_Z`), not on the surface: a line lying on the
# stage covers the very wave the figure is measuring.
#
# "Add Track" replaces all of this — a drawn track is its own element, already visible, and the
# figure follows it instead (see `trackSeries`, 75_aquamoto.cpp).
const AQUA_TRACK_NAME = "Wave track (default)"
const AQUA_TRACK_Z = 15.0               # metres above the datum

"""
    _aqua_track_end(st, x0, x1, y) -> x

Where the default track STOPS: the first node along the row `y`, walking from `x0` toward `x1`, whose
BATHYMETRY has risen to `AQUA_TRACK_Z`. The line then ends on the beach slope at its own height
instead of running up over the land, where it lies across the wave the figure is measuring. `x1`
itself when the row never gets that high (an all-wet tank).

Read off `st.bat` through `_zmat` — the one accessor for `z[iy,ix]`, row 1 = south (SACRED_LAW.md's
grid memory-layout law); never off `bat.z` directly.
"""
function _aqua_track_end(st::_AquaState, x0::Float64, x1::Float64, y::Float64)::Float64
	B = st.bat
	xv, yv = vec(Float64.(B.x)), vec(Float64.(B.y))
	(length(xv) < 2 || length(yv) < 2) && return x1
	M = _zmat(B)
	iy = clamp(argmin(abs.(yv .- y)), 1, size(M, 1))
	step = x1 >= x0 ? 1 : -1
	i0 = clamp(argmin(abs.(xv .- x0)), 1, length(xv))
	i1 = clamp(argmin(abs.(xv .- x1)), 1, length(xv))
	@inbounds for ix in i0:step:i1
		z = Float64(M[iy, min(ix, size(M, 2))])
		(isfinite(z) && z >= AQUA_TRACK_Z) && return xv[ix]
	end
	return x1
end

function _aqua_show_track(scene::Ptr{Cvoid}, on::Bool, x0::Float64, x1::Float64, y::Float64)::Cint
	ccall(_fn(:gmtvtk_remove_overlay_named_h), Cint, (Ptr{Cvoid}, Cstring), scene, AQUA_TRACK_NAME)
	on || return Cint(1)
	(isfinite(x0) && isfinite(x1) && isfinite(y) && x1 != x0) || return Cint(0)
	st = get(_AQUA, scene, nothing)
	xe = st === nothing ? x1 : _aqua_track_end(st, x0, x1, y)
	(isfinite(xe) && xe != x0) || return Cint(0)
	n = 64                              # enough vertices to follow a curved (geographic) window
	xs = collect(range(x0, xe; length = n))
	D = GMT.mat2ds([xs fill(y, n) fill(AQUA_TRACK_Z, n)])
	ok = _add_dataset_to_scene(scene, D, AQUA_TRACK_NAME; color = :yellow, forceMode = :lines,
	                           noConvertToPoints = true)
	return Cint(ok ? 1 : 0)
end

const AQUA_WATER = "WATER stage"        # the two surfaces' Scene Objects names — what the Illumination
const AQUA_LAND  = "LAND bathymetry"    # dialog aims at, one per side

"""
    _aqua_water_range(Gw) -> (lo, hi)

The water half's colour span: SYMMETRIC about zero, `amp = max(|min|, |max|)` over its own wet nodes,
so calm water sits at the centre of the diverging palette and trough and crest read as the two sides
they are.
"""
function _aqua_water_range(Gw::GMTgrid)
	lo, hi = Float64(Gw.range[5]), Float64(Gw.range[6])
	amp = max(abs(lo), abs(hi))
	amp > 0 || (amp = 1.0)
	return (-amp, amp)
end

"""
    _aqua_split(st, G, bat, Z, splitDryWet) -> (Gwater, Gland)

THE SPLIT, and the only place it happens: the slice's stage with every DRY node NaN, and the
bathymetry with every WET node NaN. Each comes back as a first-class grid whose z-range is its OWN
data's — which is what makes it colour and light exactly like that half shown in a window of its own.
`splitDryWet` off means the whole layer is water, so there is no land grid.
"""
function _aqua_split(st::_AquaState, G::GMTgrid, bat::Matrix{Float32}, Z::Matrix{Float32},
                     splitDryWet::Bool)
	if !splitDryWet
		return deepcopy(G), nothing
	end
	dry = _aqua_indland(bat, Z)
	wet = .!dry
	Gw = deepcopy(G);      Gw.z[dry] .= NaN32
	Gl = deepcopy(st.bat); Gl.z[wet] .= NaN32
	for (H, m) in ((Gw, wet), (Gl, dry))
		if any(m)
			v = view(H.z, m)
			H.range[5], H.range[6] = Float64(minimum(v)), Float64(maximum(v))
		end
	end
	return Gw, (any(dry) ? Gl : nothing)
end

# The two surfaces pushed into ONE window. The water is the window's base (it is the quantity the
# slider drives); the land is a second surface beside it, built once — the bathymetry does not change
# with time, only which of its nodes are dry does, and that is the water's business.
function _aqua_push_two_surfaces(scene::Ptr{Cvoid}, st::_AquaState, G::GMTgrid,
                                 bat::Matrix{Float32}, Z::Matrix{Float32}, splitDryWet::Bool, k::Int)
	Gw, Gl = _aqua_split(st, G, bat, Z, splitDryWet)
	first = st.first
	if first
		# The water half OPENS the window, exactly as a grid does — on the tsunami's own colour scale:
		# SYMMETRIC about zero, so calm water is the diverging palette's centre and trough/crest read as
		# the two sides they are. A raw min/max span puts the resting sea wherever the slice's extremes
		# happen to leave it (white, at this one), which is not a picture of a wave.
		_add_grid_to_scene(scene, Gw, AQUA_WATER; cmap = st.watercmap, promote = true,
		                   zrange = _aqua_water_range(Gw), source = "$(st.path)?$(st.varname)")
		if Gl !== nothing
			# NOT TOUCHED BY "Sat img". Draping the land actor here would mean changing
			# `_add_grid_to_scene`, which every grid add in this program goes through — a new feature
			# does not get to alter that. The two-surface mode therefore keeps its colourmapped land
			# for now; the composite and the lit combine carry the satellite albedo, and this mode
			# gets it when it can be done without editing a shared door.
			_add_grid_to_scene(scene, Gl, AQUA_LAND; cmap = st.landcmap, promote = false,
			                   source = "$(st.path)?bathymetry")
			# …AND SHOWN. A grid added to a window that already has one is registered hidden (the file's
			# other variables — bathymetry, the beach masks — are meant to arrive unchecked). This one is
			# not an alternative view of the layer, it is HALF OF IT.
			ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint),
			      scene, AQUA_LAND, Cint(1))
		end
		st.first = false
		# REAL SURFACES, NOT A FLAT IMAGE. The flat-image form is one textured quad covering the whole
		# bbox, so the side that owns it hides the other behind its own NaN texels — and a texture has
		# no material, which is the other half of why VTK (PBR) could not be per side. Two surfaces
		# only make sense as surfaces.
		#
		# …AND THE WINDOW MUST BE IN 3-D TO SHOW THAT. `imgmode=0` only leaves LAYER-IMAGE mode; it says
		# nothing about flat 2-D, and `_add_grid_to_scene(promote=true)` opens the window flat top-down
		# the way a plain grid does (`gmtvtk_promote_surface_h`). The per-slice `replace_base_grid_h`
		# then preserves the camera on purpose, so a window that started flat STAYS flat for every
		# timestep: the stage height was on the surface all along (the viewer's zmin/zmax track each
		# slice) and was being drawn as a picture. A TSUNAMI IS ITS SEA HEIGHT — the quantity the slider
		# drives is the water's z, so this window opens in 3-D. Through `flat2d`, i.e. the ONE
		# `sceneSetFlat2D` the 2D/3D toolbar button goes through (apply_scene_state, 90_c_api.cpp) —
		# never a camera set of our own.
		ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), scene, "imgmode=0;flat2d=0;")
		_session_record!(scene, :basegrid, :file, st.path; name = AQUA_WATER)
	else
		# A NEW TIMESTEP IS A NEW Z FOR THE WATER SURFACE, nothing else: the land surface is untouched,
		# so its light, its palette and its material survive every slice change by construction.
		z, nx, ny, zlay = _grid_zbuf(Gw)
		wlo, whi = _aqua_water_range(Gw)              # the same symmetric scale at every timestep
		cz, crgb, n = _cpt_nodes_range(wlo, whi, st.watercmap)
		ok = ccall(_fn(:gmtvtk_replace_base_grid_h), Cint,
		           (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		            Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring, Cint),
		           scene, z, Cint(nx), Cint(ny), Gw.range[1], Gw.range[2], Gw.range[3], Gw.range[4],
		           Cint(st.geog), cz, crgb, Cint(n), AQUA_WATER, zlay)
		(ok == 0) && error("Aquamoto: the viewer rejected the new water surface (window closed?)")
		_forget_object!(scene, :grid, AQUA_WATER)
		_remember_object!(scene, :grid, AQUA_WATER, Gw)
	end
	# The water stands on a NEW surface at every step, so its own reflectance is recomputed from it —
	# from the HALF GRID itself, through the plain `_hs_reflectance` a grid window uses, because in
	# this mode the water IS a plain grid. A C++ LOOK (1 VTK PBR, 5, 6, 7) carries no reflectance and
	# needs nothing here: it lives on the actor and survives the z replacement.
	let p = st.illum[1]
		if !isempty(p)
			m = parse(Int, p["model"])
			if m in (2, 3, 4)
				R = _hs_reflectance(Gw, m, p)
				@inbounds for i in eachindex(R); isfinite(R[i]) || (R[i] = 0.0f0); end
				_hs_push(scene, R, Float64(Gw.range[1]), Float64(Gw.range[2]),
				         Float64(Gw.range[3]), Float64(Gw.range[4]), m, 0)
			end
		end
	end
	_aqua_set_title_time(scene, st, k)
	return nothing
end

# "Plot Run In": scan every timestep once (progress bar) building the "ever wet" mask (any step
# where a cell wasn't dry), intersect with dry land (bathymetry >= 0) for the inundation zone, then
# contour its boundary and draw it as a line overlay — the existing overlay C export
# (gmtvtk_add_overlay_h, driven the same way grid.jl's `_add_overlay!`/`add!` do), no new drawing
# mechanism needed.
function _aquamoto_runin(scene::Ptr{Cvoid})
	st = get(_AQUA, scene, nothing)
	st === nothing && error("Aquamoto: no file open in this window")
	# The mask this builds becomes a GRID (mat2grid below, column-major south-first by construction),
	# so this scan works on `_zmat` views -- (ny,nx) with row 1 = south for any layout the file was
	# read in. Index arithmetic only, no matrix copies.
	bat = _zmat(st.bat)
	everwet = falses(size(bat))
	_progress_show_async(st.nsteps, "Aquamoto — computing inundation…")
	for k in 0:st.nsteps-1
		Gk = _aqua_layer(st, k)
		Gk === nothing && error("Aquamoto: could not read layer $(k + 1) of '$(st.varname)' from $(st.path)")
		Z = _zmat(Gk)
		@inbounds for i in eachindex(Z)
			everwet[i] |= !_aqua_isdry(bat[i], Z[i])   # wet == not dry, by THE dry/wet test
		end
		_progress_status(k + 1, "Aquamoto — computing inundation… ($(k + 1)/$(st.nsteps))")   # raw count, see _aquamoto_open
	end
	_progress_close()
	inund = everwet .& (bat .>= 0)
	any(inund) || error("Aquamoto: no inundation zone found (nothing was ever both dry land and wet at some step)")
	G = GMT.mat2grid(Float64.(inund); x = st.bat.x, y = st.bat.y)
	D = try
		GMT.grdcontour(G, cont = 0.5, dump = true)
	catch e
		error("Aquamoto: could not contour the inundation mask ($(sprint(showerror, e)))")
	end
	(D === nothing) && error("Aquamoto: the inundation zone has no traceable boundary")
	xyz, segoff, nseg, npts = _pack_dataset(D, st.bat)
	cr, cg, cb = _ovl_color(nothing, :lines)
	ok = ccall(_fn(:gmtvtk_add_overlay_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring),
		scene, xyz, Cint(npts), segoff, Cint(nseg), Cint(1), cr, cg, cb, 0.0, 0.0, "Run-in")
	(ok == 0) && error("Aquamoto: could not draw the inundation boundary (window closed?)")
	return nothing
end

# ---------------------------------------------------------------------------------------------
# JIT WARM-UP. A slice is not a cheap call: it reads a layer, runs the dry/wet composite, packs an
# RGBA texture and a land mask, and (when a light is loaded) computes a reflectance. All of that is
# compiled the FIRST time the user moves the slider, which is exactly the "the slider does nothing
# for a long time, then starts working" the tool was reported with. The window's own open is the dead
# time to spend on it (warmupTool("aquamoto"), 75_aquamoto.cpp), so the first drag is already warm.
#
# Tiny throw-away data, never the live scene (warmup.jl's rule): the point is the CODE, not the grid.
function _aqua_warm()
	# NOTHING HERE MAY CALL GMT. warmup.jl runs this body as a TASK, and on a multi-threaded session
	# (20 threads on this machine) that task runs on ANOTHER THREAD — while the window that just
	# opened is still reading its cube through GMT. GMT is not thread-safe, and two threads inside it
	# is an access violation that takes the whole application down: exactly the crash this warm-up
	# caused on its first outing, through _aqua_composite_rgb -> _aqua_colorize -> _cpt_nodes_range
	# -> makecpt. So every GMT-touching method is COMPILED, never RUN — `precompile` infers and
	# generates code without executing a line of it, which is what warmup.jl prescribes for anything
	# that must not actually happen. Only pure-Julia array work runs here.
	ny, nx = 24, 32
	bat = Float32.(repeat(range(-50, 20; length = nx)', ny, 1))       # a beach: deep -> dry land
	Z   = copy(bat);  Z[:, 1:(nx ÷ 2)] .= 0.5f0                       # wet offshore half, dry ashore
	dry = _aqua_indland(bat, Z)                                       # THE dry/wet test
	yield()
	rgb = Array{UInt8}(undef, ny, nx, 3);  fill!(rgb, 0x80)
	_aqua_pack_rgba(rgb, 0, nx, ny)                                   # the texture pack…
	_aqua_pack_landmask(dry, 0, nx, ny)                               # …and its dry/wet mask
	yield()
	precompile(_aqua_composite_rgb, (Matrix{Float32}, Matrix{Float32}, Bool, Float64, Float64, Float64,
	                                 Array{UInt8,3}, Float64, Bool, Bool, Symbol, Symbol))
	precompile(_aqua_colorize,   (Matrix{Float32}, Float64, Float64, Symbol))
	precompile(_cpt_nodes_range, (Float64, Float64, Symbol))
	precompile(_aquamoto_slice,  (Ptr{Cvoid}, Int, Bool, Bool, Float64, Bool, Bool))
	precompile(_aqua_shaded_rgb, (Ptr{Cvoid}, _AquaState, GMTgrid{Float32,2}, Bool))
	precompile(_aqua_set_title_time, (Ptr{Cvoid}, _AquaState, Int))
	precompile(_read_cube_layer, (String, Int))
	precompile(_aqua_read_times, (String, Int))
	return nothing
end

# The Aquamoto window has no callback of its own (every call arrives through the console-eval
# bridge), so this registers only the warm-up body — fired by warmupTool("aquamoto") when the window
# opens (75_aquamoto.cpp), exactly like every other tool's dialog.
function _register_aquamoto()
	warm_register("aquamoto", _aqua_warm)
	return
end

# Tell the Aquamoto dialog whether this window's cube is now held whole in memory. Called by
# `_aqua_load_all` itself, so no caller has to remember to report — and the file-open path does the
# reset in the other direction (a new file is on disk until this says otherwise).
_aqua_report_ram(scene::Ptr{Cvoid}, on::Bool) =
	ccall(_fn(:gmtvtk_aqua_set_ram_loaded_h), Cvoid, (Ptr{Cvoid}, Cint), scene, Cint(on))
