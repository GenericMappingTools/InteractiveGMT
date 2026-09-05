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
	cur::Int                                # 0-based index of the slice on screen (the time slider)
	illum::Dict{String,String}              # View > "Illumination (Hillshade)" params, EMPTY = none
	                                        # loaded. Kept because the WATER side stands on a surface
	                                        # that changes at every timestep, so its reflectance has to
	                                        # be recomputed per slice (_aqua_relight_water!).
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
end

const _AQUA = Dict{Ptr{Cvoid}, _AquaState}()

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
			ccall((:GDALClose, GMT.libgdal), Cvoid, (Ptr{Cvoid},), ds)
		end
	catch
		return Float64[]
	end
	return out
end

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
function _aqua_colorize(Z::Matrix{Float32}, zlo::Float64, zhi::Float64, cmap::Symbol)::Array{UInt8,3}
	ny, nx = size(Z)
	rgb = Array{UInt8}(undef, ny, nx, 3)
	cz, crgb, n = _cpt_nodes_range(zlo, zhi, cmap)
	# 256-entry LUT (`_cpt_nodes_range` resamples any master CPT to 256 continuous nodes): index each
	# pixel into it. As long as the [zlo,zhi] range is matched to the data, the full 256-colour palette
	# is spanned -- the banding earlier came from a MIS-matched range (e.g. :geo over the full bathymetry
	# left land in only ~16 of the 256 nodes), not from too few palette entries.
	span = (zhi > zlo) ? (zhi - zlo) : 1.0
	invspan = (n - 1) / span
	@inbounds for j in 1:nx, i in 1:ny
		v = Float64(Z[i, j])
		idx = clamp(round(Int, (v - zlo) * invspan) + 1, 1, n)
		b = 3 * (idx - 1)
		rgb[i, j, 1] = round(UInt8, clamp(crgb[b+1] * 255, 0, 255))
		rgb[i, j, 2] = round(UInt8, clamp(crgb[b+2] * 255, 0, 255))
		rgb[i, j, 3] = round(UInt8, clamp(crgb[b+3] * 255, 0, 255))
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
function _aqua_push_land_cpt!(scene::Ptr{Cvoid}, bat::GMTgrid, cmap::Symbol)
	lbarlo = 0.0                                # displayed LAND range: [0, max land elevation]
	lbarhi = max(bat.range[6], lbarlo + 0.1)    # falls back to lbarlo+0.1 when the whole area is ocean
	# Build the CPT DIRECTLY over the land-only span so all 256 nodes land on [0,lbarhi] -- building
	# over the full bathymetry range (as before) and then keeping only the z>=0 nodes wasted almost
	# the whole ramp on ocean-floor depths, leaving land with only a handful of distinct colours.
	lcz, lcrgb, ln = _cpt_nodes_range(lbarlo, lbarhi, cmap)
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
	if side == 0
		st.watercmap = sym
	elseif side == 1
		st.landcmap = sym
		st.imgbat = Array{UInt8}(undef, 0, 0, 0)   # cached bathymetry colourisation used the OLD cmap
		_aqua_push_land_cpt!(scene, st.bat, sym)
	else
		error("Aquamoto: unknown colorbar side $side (0=water, 1=land)")
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
function _aquamoto_open(scene::Ptr{Cvoid}, path::String)
	isfile(path) || error("Aquamoto: file not found: $path")
	varnames = _aqua_find_all_varnames(path, "bathymetry")
	isempty(varnames) && error("Aquamoto: could not find a time-varying quantity variable in $path " *
	                          "(expected alongside a 'bathymetry' variable — NSWING's own single 3-D netCDF output)")
	varname = varnames[1]   # no name-based preference -- whichever time-varying quantity var was found first
	bat = try
		_gmtread_trb("$(path)?bathymetry")
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
			Gk = _read_cube_layer("$(path)?$(vn)", k + 1)
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
	_add_grid_to_scene(scene, bat, "bathymetry"; promote = false, source = "$(path)?bathymetry")
	ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, "bathymetry", Cint(0))
	skipvars = Set(lowercase.(varnames)); push!(skipvars, "bathymetry")
	for v in _netcdf_subdatasets(path)
		lowercase(v.name) in skipvars && continue
		try
			G = _gmtread_trb("$(path)?$(v.name)")
			_add_grid_to_scene(scene, G, v.name; promote = false, source = "$(path)?$(v.name)")
			ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, v.name, Cint(0))
		catch e
			@tool_error "Aquamoto: could not load variable '$(v.name)'" exception=e
		end
	end

	# The trailing display options mirror the dialog's OWN startup state ("Split Dry/Wet" checked,
	# global scaling off, no transparency, both sides shaded). They are overwritten by the first
	# `_aquamoto_slice`, i.e. before anything is on screen, so they only ever matter to a caller that
	# asks for a slice before the dialog has drawn one.
	_AQUA[scene] = _AquaState(String(path), varname, varnames, scans, bat, nsteps, geog, Array{UInt8}(undef, 0, 0, 0), true, :polar, :geo, 0, Dict{String,String}(),
	                          true, false, 0.0, true, true, _aqua_read_times(String(path), nsteps),
	                          Dict{String,GMTgrid}())
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
                             watercmap::Symbol=:polar, landcmap::Symbol=:geo)
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
		blo = 0.0
		bhi = max(landhi, blo + 0.1)
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
function _aqua_illuminate!(scene::Ptr{Cvoid}, model::Int, d::Dict{String,String})
	st = get(_AQUA, scene, nothing)
	(st === nothing) && error("Aquamoto: no file open in this window")
	st.illum = copy(d)                        # remembered: the water side is re-lit at every timestep
	st.illum["model"] = string(model)
	_hs_push_grid(scene, st.bat, model, st.illum, 1)    # LAND  <- the static bathymetry
	_aqua_relight_water!(scene, st)                     # WATER <- the CURRENT slice's own stage
	# The method is DECLARED here, by the act that chose it — never by the pushes above, which are
	# data and run again at every slice. Both sides get it (sceneSetReliefLook copies the look to both).
	_hs_declare_look(scene)
	return nothing
end

# The WATER side's reflectance, computed from the stage it actually stands on. Called by the tool and
# AGAIN by every slice change: the stage is a DIFFERENT surface at every timestep, so a reflectance
# computed once would light slice 40's wave with slice 3's relief. `G` is the slice the caller has
# already read (never re-read it); without one, the slice on screen is read here.
function _aqua_relight_water!(scene::Ptr{Cvoid}, st::_AquaState, G::Union{GMTgrid,Nothing}=nothing)
	isempty(st.illum) && return nothing
	# …AND ONLY WHILE THAT MODEL IS STILL THE WINDOW'S LIGHT. Picking a relief look (VTK PBR, grdimage,
	# Lambert) REPLACES a loaded Illumination model — sceneSetReliefLook drops it — so re-pushing the
	# remembered one here would put the window straight back on a hillshade method (every push force-
	# sets useHillshade/hillGrd), i.e. the method the user just chose silently reverting at the next
	# slice. Ask the viewer whether the model is still loaded; when it is not, forget it.
	if ccall(_fn(:gmtvtk_has_extern_shade_h), Cint, (Ptr{Cvoid},), scene) == 0
		empty!(st.illum)
		return nothing
	end
	model = parse(Int, st.illum["model"])
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
	R = _hs_reflectance(W, model, st.illum)
	# ...and back in as NEUTRAL. Covers the dropped cells and the one-cell NaN fringe GMT leaves
	# around them: every node the light has nothing to say about says exactly nothing.
	@inbounds for k in eachindex(R)
		isfinite(R[k]) || (R[k] = 0.0f0)
	end
	return R
end

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
	return _read_cube_layer("$(st.path)?$(st.varname)", k + 1)
end

# "Load all in RAM" for the ACTIVE variable — the option the other netCDF cubes get from the cube
# dock's own button (`_on_cube_load_all`, drop.jl), which an Aquamoto file never reaches because it
# opens through this dialog instead. Same reader, same RAM test, same return codes:
# 0 = loaded (or already resident), 1 = would not fit in free RAM, 2 = error.
function _aqua_load_all(scene::Ptr{Cvoid})::Cint
	try
		st = get(_AQUA, scene, nothing)
		st === nothing && return Cint(2)
		haskey(st.ram, st.varname) && return Cint(0)         # already resident
		g1 = _read_cube_layer("$(st.path)?$(st.varname)", 1)
		g1 === nothing && return Cint(2)
		_cube_fits_ram(length(g1.z) * sizeof(eltype(g1.z)), st.nsteps) || return Cint(1)
		C = _read_whole_cube("$(st.path)?$(st.varname)", st.nsteps)
		(C isa GMTgrid && ndims(C.z) == 3) || return Cint(2)
		st.ram[st.varname] = C
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
	st.cur = k                                         # the slice on screen, for _aqua_relight_water!
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
	amp = max(abs(waterlo), abs(waterhi))
	amp > 0 || (amp = 1.0)
	waterlo, waterhi = -amp, amp
	landhi = st.bat.range[6]                           # max land elevation, straight from the grid's OWN known range

	# THE COMPOSITE IS THE TSUNAMI. Land is coloured from the bathymetry with the LAND colormap, water
	# from the stage with the WATER one, and each side is lit from its own surface — that separation IS
	# Aquamoto (SACRED_LAW.md's two-surface law), and it is not something a display mode may drop. A
	# 3-D pass that pushed the bare stage grid with the water CPT painted the land on the water scale
	# (all red at the top of :polar) and threw the per-side illumination away; it is gone.
	#
	# What the geometry question is still good for is the FAST PATH: the same-size push repaints the
	# texture instead of rebuilding the scene (showLayerImageTail).
	cz, crgb, n = _cpt_nodes_range(waterlo, waterhi, st.watercmap)   # the water scale = the colourbar
	zhover, znx, zny, zlay = _grid_zbuf(G)             # stage buffer + the layout code the VIEWER reads it with
	r = st.bat.range
	name = basename(st.path)                           # handle named after the file, like every other layer
	rgb, st.imgbat = _aqua_composite_rgb(bat, Z, splitDryWet, waterlo, waterhi, transparency, st.imgbat, landhi,
	                                     shadeWater, shadeLand, st.watercmap, st.landcmap)

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
	precompile(_aqua_relight_water!, (Ptr{Cvoid}, _AquaState, Nothing))
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
