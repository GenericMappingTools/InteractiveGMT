# aquamoto.jl — Geophysics > Tsunamis > "Aquamoto viewer…" (port of Mirone's aquamoto.m + its
# netCDF support half aqua_suppfuns.m — NETCDF TAB ONLY, the first tab of aquamoto.ui). The target
# file class is NSWING's own single 3-D netCDF output (`-G<stem>,<int>`, no `+m`): a static 2-D
# `bathymetry` variable + a time-varying 3-D quantity variable ("stage" by default), read the SAME
# way any other 3-D netCDF cube is read in this app (`file.nc?var[i]`, see drop.jl).
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
# .ui itself ships Show-mesh disabled and Mirone's own set_common() disables the Primary-quantities
# radios + derived-var checkbox for this exact (COARDS/NSWING) file class, so leaving them inert
# here is a faithful port, not a scope cut. Shading/illumination is the separate "Shading OR Image"
# tab (also out of scope): this pass paints flat, unshaded colour.
#
# Every call comes from the C++ AquamotoWindow (75_aquamoto.cpp) through the generic console-eval
# bridge (g_juliaEval / juliaEvalCall — the SAME synchronous round-trip NswingDialog already uses
# for its own small queries), keyed to the caller's own live viewer window (`scene`). No new
# @cfunction/registration is needed for that: only the composited-texture push
# (gmtvtk_show_layer_rgba_h) is a new C export (see 90_c_api.cpp).

mutable struct _AquaState
	path::String                            # the netCDF file
	varname::String                         # time-varying quantity variable name ("stage", …)
	bat::GMTgrid{Float32,2}                 # bathymetry grid, read once
	nsteps::Int
	geog::Bool
	imgbat::Array{UInt8,3}                  # cached land RGB (ny,nx,3); EMPTY (size 0) = not built yet
	first::Bool                             # true until the first slice has been shown (Save/Session bookkeeping)
	wetlo::Vector{Float64}                  # per-layer WET-cell min (Inf if a layer is entirely dry)
	wethi::Vector{Float64}                  # per-layer WET-cell max (-Inf if a layer is entirely dry)
	alllo::Vector{Float64}                  # per-layer min over EVERY cell (land included)
	allhi::Vector{Float64}                  # per-layer max over EVERY cell (land included)
end

const _AQUA = Dict{Ptr{Cvoid}, _AquaState}()

# The time-varying variable name NSWING/Mirone actually write. "stage" is the default (surface
# elevation); try the handful of other names before giving up.
const _AQUA_VARNAMES = ("stage", "depth", "amp", "z")

# Pick the time-varying 3-D quantity variable in `path`, skipping `skip` (the bathymetry variable).
# Prefers the netCDF subdataset introspection this app already has (drop.jl's `_netcdf_subdatasets`,
# GDAL's Subdatasets report); falls back to probing the handful of known NSWING/Mirone names
# directly for a plain 2-variable file GDAL doesn't report as multi-subdataset. "" if none found.
function _aqua_find_varname(path::String, skip::String)
	for v in _netcdf_subdatasets(path)
		lowercase(v.name) == lowercase(skip) && continue
		length(v.dims) >= 3 && return v.name
	end
	for nm in _AQUA_VARNAMES
		try
			GMT.grdinfo("$(path)?$(nm)", C = true, Q = true)
			return nm
		catch
		end
	end
	return ""
end

# z -> RGB (UInt8, ny x nx x 3) via a LINEAR cpt built fresh over [zlo,zhi] with `cmap` (any GMT
# colormap name, e.g. :geo, :polar). Nearest-bin lookup against the cpt's own discrete nodes (same
# convention as cpt.jl's `_z_to_hex`, generalized to a whole array). NaN -> mid-grey. Returns a
# flat greyed-out array if the cpt itself fails to build (`_cpt_nodes_range` returned nothing usable).
function _aqua_colorize(Z::Matrix{T}, zlo::Float64, zhi::Float64, cmap::Symbol)::Array{UInt8,3} where {T<:Real}
	ny, nx = size(Z)
	rgb = Array{UInt8}(undef, ny, nx, 3)
	cz, crgb, n = _cpt_nodes_range(zlo, zhi, cmap)
	if (n < 2)
		fill!(rgb, UInt8(160))
		return rgb
	end
	# 256-entry LUT (`_cpt_nodes_range` resamples any master CPT to 256 continuous nodes): index each
	# pixel into it. As long as the [zlo,zhi] range is matched to the data, the full 256-colour palette
	# is spanned -- the banding earlier came from a MIS-matched range (e.g. :geo over the full bathymetry
	# left land in only ~16 of the 256 nodes), not from too few palette entries.
	span = zhi > zlo ? (zhi - zlo) : 1.0
	invspan = (n - 1) / span
	@inbounds for j in 1:nx, i in 1:ny
		v = Float64(Z[i, j])
		if isnan(v)
			rgb[i, j, 1] = rgb[i, j, 2] = rgb[i, j, 3] = UInt8(160)
			continue
		end
		idx = clamp(round(Int, (v - zlo) * invspan) + 1, 1, n)
		b = 3 * (idx - 1)
		rgb[i, j, 1] = round(UInt8, clamp(crgb[b+1] * 255, 0, 255))
		rgb[i, j, 2] = round(UInt8, clamp(crgb[b+2] * 255, 0, 255))
		rgb[i, j, 3] = round(UInt8, clamp(crgb[b+3] * 255, 0, 255))
	end
	return rgb
end

# Pack an (ny,nx,3) RGB array (GMT's native column-major layout, row 1 = y_min = SOUTH, row ny =
# NORTH, column 1 = x_min = WEST — see grid.jl/drape.jl's own "y ascending" convention) into the
# row-major, row-0-=-south, west->east, opaque RGBA byte buffer `gmtvtk_show_layer_rgba_h` expects
# (the SAME convention bakeLayerRGBA's own output uses, 40_shading.cpp) — no vertical flip needed
# since GMT's row-ascending-with-y already puts the south row first in memory.
function _aqua_pack_rgba(rgb::Array{UInt8,3})::Vector{UInt8}
	ny, nx, _ = size(rgb)
	buf = Vector{UInt8}(undef, ny * nx * 4)
	k = 1
	@inbounds for i in 1:ny
		for j in 1:nx
			buf[k]   = rgb[i, j, 1]
			buf[k+1] = rgb[i, j, 2]
			buf[k+2] = rgb[i, j, 3]
			buf[k+3] = 0xff
			k += 4
		end
	end
	return buf
end

# Open a netCDF file, cache its header (bathymetry grid, time-varying var name, step count) per
# window, and immediately scan every layer ONCE for its own min/max (both the WET-cell-only range
# and the whole-cell range) -- so navigating slices and toggling "Scale colour to global min/max"
# are instant lookups afterwards, never a fresh rescan. Prints the step count (parsed by the C++
# dialog to fill "Time steps = N" + the slider range) on success; throws (shown as an error dialog
# by the console-eval bridge) on anything it can't make sense of.
function _aquamoto_open(scene::Ptr{Cvoid}, path::String)
	isfile(path) || error("Aquamoto: file not found: $path")
	varname = _aqua_find_varname(path, "bathymetry")
	isempty(varname) && error("Aquamoto: could not find a time-varying quantity variable in $path " *
	                          "(expected alongside a 'bathymetry' variable — NSWING's own single 3-D netCDF output)")
	bat = try
		GMT.gmtread("$(path)?bathymetry")
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
	wetlo = fill(Inf, nsteps); wethi = fill(-Inf, nsteps)
	alllo = fill(Inf, nsteps); allhi = fill(-Inf, nsteps)
	_progress_show_async(nsteps, "Aquamoto — scanning layers…")
	for k in 0:nsteps-1
		Z = GMT.gmtread("$(path)?$(varname)[$(k)]").z
		lo_a, hi_a, lo_w, hi_w = Inf, -Inf, Inf, -Inf
		@inbounds for i in eachindex(Z)
			z = Z[i]
			isnan(z) && continue
			z < lo_a && (lo_a = z); z > hi_a && (hi_a = z)
			abs(batz[i] - z) < 1e-2 && continue   # dry cell -> excluded from the wet-only range
			z < lo_w && (lo_w = z); z > hi_w && (hi_w = z)
		end
		alllo[k+1], allhi[k+1] = lo_a, hi_a
		wetlo[k+1], wethi[k+1] = lo_w, hi_w
		# `value` here is the RAW step count, not a percentage: gmtvtk_progress_show_async set the
		# dialog's range to 0..nsteps, so Qt computes the percentage itself from value/range. Passing
		# an already-computed 0..100 percentage against that range made the bar finish at
		# (100/nsteps)*100% instead of 100% -- e.g. ~56% at completion for a 180-step file.
		_progress_status(k + 1, "Aquamoto — scanning layers… ($(k + 1)/$(nsteps))")
	end
	_progress_close()

	# Push the LAND colorbar ONCE (static for the whole file, unlike the per-slice water bar). LAND is
	# elevation >= 0, so the bar MUST start at sea level (0), never at the ocean-floor depth. Build the
	# :geo ramp over the FULL bathymetry range (so the colours match what _aqua_composite_rgb's imgbat
	# actually paints on land — makecpt stretches :geo to the range), then keep only the z >= 0 nodes and
	# anchor the first exactly at 0. Result: same land colours, colorbar minimum = 0.
	blo, bhi = extrema(filter(isfinite, batz))
	blo == bhi && (bhi = blo + 0.1)
	lcz, lcrgb, ln = _cpt_nodes_range(blo, bhi, :geo)
	if ln >= 2
		keep = [i for i in 1:ln if lcz[i] >= 0.0]
		if length(keep) >= 2
			flat = Float64[]
			for i in keep; append!(flat, lcrgb[3(i-1)+1:3(i-1)+3]); end
			lcz = Float64.(lcz[keep]); lcrgb = flat; ln = length(keep)
			lcz[1] = 0.0                       # anchor the bar's minimum exactly at sea level
		end
	end
	ln < 2 && (lcz = [0.0, max(bhi, 0.1)]; lcrgb = [0.0, 0.4, 0.0, 0.6, 0.3, 0.1]; ln = 2)
	lbarlo = 0.0                                # displayed LAND range: [0, max land elevation]
	lbarhi = max(bhi, lbarlo + 0.1)
	ccall(_fn(:gmtvtk_aqua_set_land_cpt_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cdouble, Cdouble),
		scene, lcz, lcrgb, Cint(ln), Cdouble(lbarlo), Cdouble(lbarhi))

	# Hand the viewer the static bathymetry = the LAND surface for hillshading. Column-major Float32,
	# the SAME layout as the per-slice stage (zhover) the viewer already receives. The viewer then
	# shades LAND from this and WATER from the live stage through the ONE shared applyReliefShade
	# (bakeAquaShade) -- so the Shading dock's Hillshade drives the tsunami like any other layer.
	bz = eltype(batz) === Float32 ? batz : Float32.(batz)
	bny, bnx = size(bz)
	ccall(_fn(:gmtvtk_aqua_set_bathy_h), Cint, (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint),
		scene, bz, Cint(bnx), Cint(bny))

	_AQUA[scene] = _AquaState(String(path), varname, bat, nsteps, geog, Array{UInt8}(undef, 0, 0, 0), true, wetlo, wethi, alllo, allhi)
	print(nsteps)
	return nothing
end

# Prior-session state for `scene` (if any), so a freshly (re)opened Aquamoto panel on a window that
# already had a file loaded restores that state instead of starting blank. Prints "path|nsteps", or
# nothing (empty) if this scene has no cached session.
function _aquamoto_state(scene::Ptr{Cvoid})
	st = get(_AQUA, scene, nothing)
	st === nothing && return nothing
	print(st.path, "|", st.nsteps)
	return nothing
end

# The whole-cube WET-cell min/max, derived from the per-layer arrays `_aquamoto_open` already
# scanned up front (layers that were entirely dry contribute Inf/-Inf and drop out of the filter).
function _aqua_global_minmax(st::_AquaState)
	lo = filter(isfinite, st.wetlo); hi = filter(isfinite, st.wethi)
	(isempty(lo) || isempty(hi)) && return (0.0, 1.0)
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
# CACHED real `imgbat` -- so re-enabling a toggle never needs a bathymetry recolour.
function _aqua_composite_rgb(bat::Matrix{T}, Z::Matrix{S}, splitDryWet::Bool,
                             waterlo::Float64, waterhi::Float64, transparency::Float64,
                             imgbat::Array{UInt8,3},
                             shadeWater::Bool=true, shadeLand::Bool=true) where {T<:Real, S<:Real}
	ny, nx = size(Z)
	if !splitDryWet
		return _aqua_colorize(Z, waterlo, waterhi, :polar), imgbat
	end
	indland = (abs.(bat .- Z) .< 1e-2) .| isnan.(Z)
	Zc = copy(Z)
	Zc[indland] .= 0.0
	if isempty(imgbat)                                     # cache: only depends on the (static) bathymetry
		blo, bhi = Float64.(extrema(filter(isfinite, bat)))
		blo == bhi && (bhi = blo + 0.1)
		imgbat = _aqua_colorize(bat, blo, bhi, :geo)        # :geo already has its own land/sea break
	end
	# ALWAYS colour BOTH sides -- land from the cached bathymetry, water from the wet stage. NEVER grey a
	# side out (that made land show up grey when Water was the selected radio). Both images are always
	# shown; the Shade Water/Shade Land radio only selects which side's LIGHT the Shading dock edits
	# (aquaShowWater, applied per-side by bakeAquaShade in the viewer), it does not hide either colour.
	imgwater = _aqua_colorize(Zc, waterlo, waterhi, :polar)                  # diverging: trough/calm/crest
	landrgb  = imgbat
	alfa = clamp(transparency, 0.0, 1.0)
	rgb = similar(imgwater)
	if alfa > 0.01                                          # mixe_images' addweighted cross-blend
		@inbounds for idx in eachindex(rgb)
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

# Compute + display slice `k` (0-based). `splitDryWet` toggles the dry/wet composite; `globalMM`
# picks the whole-cube min/max over the slice's own; `transparency` (0..1) is the Water-
# transparency slider (mixe_images' cross-blend fraction — land pixels are always hard-overwritten
# with the land colour regardless of this value, matching Mirone). `shadeWater`/`shadeLand` are the
# "Shade Water"/"Shade Land" toggle buttons — see `_aqua_composite_rgb`.
function _aquamoto_slice(scene::Ptr{Cvoid}, k::Int, splitDryWet::Bool, globalMM::Bool, transparency::Float64,
                         shadeWater::Bool=true, shadeLand::Bool=true)
	st = get(_AQUA, scene, nothing)
	st === nothing && error("Aquamoto: no file open in this window")
	(0 <= k < st.nsteps) || error("Aquamoto: slice $k out of range (0..$(st.nsteps - 1))")
	G = GMT.gmtread("$(st.path)?$(st.varname)[$(k)]")
	#Z = eltype(G.z) === Float64 ? G.z : Float64.(G.z)		# WTF is that Float64 doing here?
	Z = G.z
	bat = st.bat.z
	ny, nx = size(Z)
	(size(bat) == size(Z)) || error("Aquamoto: '$(st.varname)' ($(size(Z))) and bathymetry ($(size(bat))) sizes differ")

	# Colourbar min/max = the real min/max of the WATER being displayed, i.e. the actual data range of
	# exactly the cells this slice colours as water. In Split Dry/Wet that is the WET cells only (the dry
	# land cells store the land elevation, up to +200 m in `z` here -- they are painted as land, never on
	# the water scale, so they must NOT enter the water colourbar). No borrowed global, no nudge. The
	# "Scale colour to global min/max" checkbox is the only override. Colouring uses this SAME range.
	# Every range below is a plain lookup into the per-layer arrays `_aquamoto_open` already scanned
	# up front -- no rescan of `Z` needed here.
	if globalMM
		waterlo, waterhi = _aqua_global_minmax(st)
	elseif splitDryWet
		waterlo, waterhi = st.wetlo[k+1], st.wethi[k+1]
		isfinite(waterlo) || ((waterlo, waterhi) = (0.0, 1.0))   # this layer is entirely dry
	else
		waterlo, waterhi = st.alllo[k+1], st.allhi[k+1]
	end
	waterhi > waterlo || (waterhi = waterlo + 1.0)     # guard an exactly-flat layer (div-by-zero only)
	rgb, st.imgbat = _aqua_composite_rgb(bat, Z, splitDryWet, waterlo, waterhi, transparency, st.imgbat,
	                                     shadeWater, shadeLand)

	rgba = _aqua_pack_rgba(rgb)
	zhover = G.z                         # native GMT column-major layout -- passed as-is
	cz, crgb, n = _cpt_nodes_range(waterlo, waterhi, :polar)   # colourbar legend = the water scale
	n < 2 && (cz = [waterlo, waterhi]; crgb = [0.0, 0.0, 1.0, 1.0, 0.0, 0.0]; n = 2)
	r = st.bat.range
	name = basename(st.path)                                   # handle named after the file, like every other layer
	ok = ccall(_fn(:gmtvtk_show_layer_rgba_h), Cint,
		(Ptr{Cvoid}, Ptr{Cuchar}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cfloat}, Cstring),
		scene, rgba, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(st.geog), cz, crgb, Cint(n), zhover, name)
	(ok == 0) && error("Aquamoto: the viewer rejected the update (window closed?)")
	if st.first
		_remember_object!(scene, :grid, name, st.bat)
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
	bat = st.bat.z
	everwet = falses(size(bat))
	_progress_show_async(st.nsteps, "Aquamoto — computing inundation…")
	for k in 0:st.nsteps-1
		Z = GMT.gmtread("$(st.path)?$(st.varname)[$(k)]").z
		@inbounds for i in eachindex(Z)
			everwet[i] |= !(isnan(Z[i]) || abs(bat[i] - Z[i]) < 1e-2)
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
