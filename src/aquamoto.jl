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
end

const _AQUA = Dict{Ptr{Cvoid}, _AquaState}()

# Every time-varying (>=3-D) quantity variable in `path`, skipping `skip` (the bathymetry
# variable) — NOT just the first match: the caller must load and offer ALL of them, never silently
# pick one and discard the rest. Pure netCDF subdataset introspection (drop.jl's
# `_netcdf_subdatasets`, GDAL's Subdatasets report) -- no guessed/hard-coded variable names. A
# tsunami netCDF of this file class always carries >1 variable (bathymetry + the time-varying
# quantity, at minimum), so it always shows up in GDAL's Subdatasets report; there is no
# single-variable case to fall back for. Empty if none found.
function _aqua_find_all_varnames(path::String, skip::String)
	found = String[]
	for v in _netcdf_subdatasets(path)
		lowercase(v.name) == lowercase(skip) && continue
		length(v.dims) >= 3 && push!(found, v.name)
	end
	return found
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
			Gk = GMT.gmtread("$(path)?$(vn)[$(k)]")
			Z = Gk.z
			sc.alllo[k+1], sc.allhi[k+1] = Gk.range[5], Gk.range[6]
			lo_w, hi_w = Inf, -Inf
			@inbounds for i in eachindex(Z)
				z = Z[i]
				abs(batz[i] - z) < 1e-2 && continue   # dry cell -> excluded from the wet-only range
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

	# Hand the viewer the static bathymetry = the LAND surface for hillshading. Column-major Float32,
	# the SAME layout as the per-slice stage (zhover) the viewer already receives. The viewer then
	# shades LAND from this and WATER from the live stage through the ONE shared applyReliefShade
	# (bakeAquaShade) -- so the Shading dock's Hillshade drives the tsunami like any other layer.
	bz = eltype(batz) === Float32 ? batz : Float32.(batz)
	bny, bnx = size(bz)
	ccall(_fn(:gmtvtk_aqua_set_bathy_h), Cint, (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint), scene, bz, Cint(bnx), Cint(bny))

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
			G = GMT.gmtread("$(path)?$(v.name)")
			_add_grid_to_scene(scene, G, v.name; promote = false, source = "$(path)?$(v.name)")
			ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, v.name, Cint(0))
		catch e
			@warn "Aquamoto: could not load variable '$(v.name)'" exception=e
		end
	end

	_AQUA[scene] = _AquaState(String(path), varname, varnames, scans, bat, nsteps, geog, Array{UInt8}(undef, 0, 0, 0), true, :polar, :geo)
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
function _aqua_composite_rgb(bat::Matrix{Float32}, Z::Matrix{Float32}, splitDryWet::Bool,
                             waterlo::Float64, waterhi::Float64, transparency::Float64,
                             imgbat::Array{UInt8,3}, landhi::Float64,
                             shadeWater::Bool=true, shadeLand::Bool=true,
                             watercmap::Symbol=:polar, landcmap::Symbol=:geo)
	ny, nx = size(Z)
	if !splitDryWet
		return _aqua_colorize(Z, waterlo, waterhi, watercmap), imgbat
	end
	indland = abs.(bat .- Z) .< 1e-2
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
	landhi = st.bat.range[6]                           # max land elevation, straight from the grid's OWN known range
	rgb, st.imgbat = _aqua_composite_rgb(bat, Z, splitDryWet, waterlo, waterhi, transparency, st.imgbat, landhi,
	                                     shadeWater, shadeLand, st.watercmap, st.landcmap)

	rgba = _aqua_pack_rgba(rgb)
	zhover = G.z                         # native GMT column-major layout -- passed as-is
	cz, crgb, n = _cpt_nodes_range(waterlo, waterhi, st.watercmap)   # colourbar legend = the water scale
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
			everwet[i] |= abs(bat[i] - Z[i]) >= 1e-2
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
