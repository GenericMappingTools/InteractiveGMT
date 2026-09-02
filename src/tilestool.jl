# tilestool.jl — Tools > Tiles Tool. Port of Mirone's tiles_tool.m (src_figs/tiles_tool.m) MINUS the
# download/mosaic machinery (url2image), which is replaced by GMT.jl's `mosaic`. The C++ picker
# (TilesPicker in 70_window.cpp) is an interactive equirectangular world map under a refinable web-tile
# mesh; the user brackets an area with two diagonal tiles and hits GO, which sends a request string
#   "go;W/E/S/N;zoom;provider;cache;merc"
# here. We build the final mosaic two zoom levels coarser (the tool's design — a quick, lighter image)
# and open it in a fresh viewer.
#
# Like every C->Julia callback (console / drop / basemap) the @cfunction and its registration are
# RUNTIME values, created lazily at the first window open (eventloop.jl `_ensure_callbacks`) via a thin
# invokelatest trampoline — never at top level (a precompiled @cfunction is invalid and would bake GMT
# into the pkgimage). The asset-path push is GMT-free, so it is installed eagerly in __init__.

# C callback: `params` = "op;...". op "go" -> "go;W/E/S/N;zoom;provider;cache;merc". `dlg` is the
# TilesPicker* (reserved for the Phase-2 coarser-background push back into the open picker). `scene` is
# the viewer the tool was opened from — used only to surface errors in its Errors tab.
# Path of the last background PNG written, so we can delete it before writing the next (the dialog has
# already loaded the previous one by the time we replace it). The newest one lingers in TMP — harmless.
const _LAST_BG = Ref{String}("")

# ONE GMT call at a time, from this tool. The prefill runs as a background task (it must — it takes
# minutes and would otherwise freeze the viewer), so its downloads yield, and while they are yielded
# the user can pan the map and start a foreground fetch. Two GMT.mosaic calls then run interleaved on
# one thread, and GMT is a C library that is not reentrant: that is a crash, and a random one, since
# it depends on where the yield landed. Every mosaic this file makes is taken under this lock.
const _TILE_LOCK = ReentrantLock()

# Footprint providers: "what tiles does the data under this region come in?", one entry per tool that
# has an answer. The map tool is generic and owns none of this — a provider is registered BY the tool
# that knows (dgtlidar.jl registers "dgt"), so a future one is added there and nothing here changes.
# A provider takes (W, E, S, N, arg) and returns (rects, names): `rects` is a flat Vector{Float64} of
# W,E,S,N per tile — the layout gmtvtk_tiles_set_footprints reads — and `names` one string per tile.
const _FP_PROVIDERS = Dict{String, Function}()

"""
    _fp_register(name::String, fn::Function)

Declare how to answer "how is this ground tiled?" for `name`. Called from the owning tool's
`_register_*` function.
"""
_fp_register(name::String, fn::Function) = (_FP_PROVIDERS[name] = fn; nothing)

# Append one line to the picker's collapsible "Downloads info" console (gmtvtk_tiles_log). Best-effort.
_tiles_log(dlg::Ptr{Cvoid}, msg::AbstractString) = (try
	ccall(_fn(:gmtvtk_tiles_log), Cvoid, (Ptr{Cvoid}, Cstring), dlg, String(msg))
catch; end; nothing)

# Run GMT.mosaic and report progress to the picker's "Downloads info" console (the user watches tiles
# download there, not the iGMT viewer's Errors tab). We do NOT redirect stdout/stderr around the
# download — an fd-level redirect corrupts GMT's tile fetch (it throws, the error gets swallowed into
# the log, and no image comes back). Instead we count the tiles up front with a cheap `quadonly` pass
# (pure quadtree math, no download) and bracket the real fetch with a "downloading N tile(s)…" /
# "mosaic ready WxH" pair, and route GMT.mosaic's per-tile notes via TILE_LOGGER. Returns the GMTimage.
function _mosaic_logged(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, lon, lat, kw::Dict{Symbol,Any})
	z = get(kw, :zoom, 0)
	n = try
		lock(_TILE_LOCK) do                       # GMT is not reentrant: this counts through it too
			q  = GMT.mosaic(lon, lat; quadonly=true, kw...)
			qt = q isa Tuple ? q[1] : q
			qt isa AbstractString ? 1 : length(qt)
		end
	catch
		0
	end
	# "needs N tile(s)", NOT "downloading N": this count is pure quadtree math and knows nothing about
	# what is already on disk. Saying "downloading" here is what made a fully cached view announce a
	# download that never happened. Only GMT's own per-tile "Downloading file …" notes, routed below,
	# mean bytes are actually crossing the network.
	_tiles_log(dlg, n > 0 ?
		"needs $n tile(s) at zoom $z for $(lon[1])/$(lon[2])/$(lat[1])/$(lat[2])" :
		"fetching $(lon[1])/$(lon[2])/$(lat[1])/$(lat[2]) at zoom $z")
	# Route mosaic's per-tile fetch notes (download URL / cache hit) into the picker console — the
	# verbose=2 effect, via GMT's TILE_LOGGER hook (no fd-level stdout redirect, which corrupts the
	# fetch). Reset in a finally so a thrown fetch never leaves a stale logger installed.
	# Under the lock: the fetch AND the global TILE_LOGGER it installs. Two mosaics interleaving would
	# also trample that Ref, each resetting the other's logger in its own `finally`.
	I = lock(_TILE_LOCK) do
		GMT.TILE_LOGGER[] = m -> _tiles_log(dlg, m)
		try
			GMT.mosaic(lon, lat; kw...)
		finally
			GMT.TILE_LOGGER[] = nothing
		end
	end
	_tiles_log(dlg, "mosaic ready ($(size(I.image, 2))×$(size(I.image, 1)) px)")
	return I
end

# Partial bbox overlap (ranges are [xmin,xmax,ymin,ymax,…]). True if the rectangles intersect.
_bbox_overlap(a, b) = (min(a[2], b[2]) > max(a[1], b[1])) && (min(a[4], b[4]) > max(a[3], b[3]))

# Same referencing system? Prefer EPSG when both carry one, else compare PROJ4 token-sets, else
# treat two unreferenced objects as "compatible" (both plain, no CRS). crs_from already normalises a
# plain geographic object to WGS84 (epsg 4326), so a lon/lat grid and a geographic mosaic both land
# on 4326 and match here.
function _crs_compatible(a::CRS, b::CRS)
	(a.epsg != 0 && b.epsg != 0) && return a.epsg == b.epsg
	(!isempty(a.proj4) && !isempty(b.proj4)) && return Set(split(a.proj4)) == Set(split(b.proj4))
	return !hascrs(a) && !hascrs(b)
end

# Decide where the freshly built mosaic `I` should go relative to the calling window `scene`:
#   true  -> add it IN PLACE (scene is an empty launcher, OR holds a grid whose CRS matches the
#            mosaic AND whose bbox at least partially overlaps it)
#   false -> open it in a NEW window (image-only window, incompatible grid, or no registry entry)
# `geog` is the mosaic's geographic flag (false for a Mercator mosaic).
function _tiles_inplace(scene::Ptr{Cvoid}, I::GMTimage, geog::Bool)::Bool
	scene == C_NULL && return false
	ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0 && return true   # empty launcher
	fig = get(_FIGREG, scene, nothing)
	fig isa QtFigure || return false                                                  # only a grid qualifies
	gcrs = crs_from(fig.G; geographic = (_isgeog(fig.G) == 1))
	icrs = crs_from(I;     geographic = geog)
	return _crs_compatible(gcrs, icrs) && _bbox_overlap(fig.G.range, I.range)
end

function _on_tiles(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		# The warm-up runs the SAME GMT.mosaic path in a background task, started the moment the picker
		# opens — which is a breath before this first fetch. GMT is a C library and is not reentrant, so
		# two mosaics interleaving on one thread (each yields while its downloads wait) is a crash, not
		# a slowdown. warmup.jl's own contract is that the tool waits for its warm-up before running.
		warm_wait("tiles")
		parts = split(unsafe_string(cparams), ';')
		op = parts[1]
		W, E, S, N = parse.(Float64, split(parts[2], '/'))
		# Only the first two fields are common to every op; the rest are the op's own and a caller that
		# has nothing to say about one simply stops writing. Reading them positionally and blind is what
		# made a "footprints" request (5 fields) crash on parts[6].
		fld(i) = (length(parts) >= i) ? String(parts[i]) : ""
		prov  = fld(4)
		cache = fld(5)
		merc  = fld(6) == "1"
		# Field 3 is ONE zoom for "go"/"bg"; a list for "prefill" and a provider name for "footprints",
		# neither of which is a number.
		zoom  = (op == "go" || op == "bg") ? parse(Int, fld(3)) : 0
		if op == "go"
			fz = min(22, zoom + 2)                         # final mosaic is two zoom levels FINER than the picker mesh
			kw = Dict{Symbol,Any}(:zoom => fz)
			isempty(prov)  || (kw[:provider] = prov)
			isempty(cache) || (kw[:cache]    = cache)
			merc && (kw[:merc] = true)
			I = _mosaic_logged(scene, dlg, [W, E], [S, N], kw)
			name = "Tiles ($(round(W;digits=2))/$(round(E;digits=2))/$(round(S;digits=2))/$(round(N;digits=2)))"
			# Put the mosaic in the CALLING window when it makes sense — an empty launcher, or a grid with
			# the same CRS and an overlapping bbox; otherwise open a fresh window. Either way the tile is a
			# managed ExtraObj image (NOT iview(I), which makes an imageOnly surface with no properties row
			# and a bare red backing plane).
			if _tiles_inplace(scene, I, !merc)
				_place_image_in_window(scene, I, name; geographic = !merc)
				try ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), scene) catch end
			else
				fig = iview_image_obj(I, name)             # framed new window, tile as ExtraObj image
				# We're nested inside the picker's GO-click handler; the fresh window can open BEHIND the
				# picker, so the user thinks "nothing happened". Raise it to the front.
				try
					h = _fig_handle(fig)
					h == C_NULL || ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), h)
				catch
				end
			end
		elseif op == "bg"
			# Coarser background for the picker's current view (Mirone's bgZoomLevel = zoom-3). The offset
			# is the caller's: the Tiles Tool wants Mirone's 3 (a light backdrop under its mesh), the
			# region picker 1 (streets are what it is picked on). Always geographic (no merc) so it aligns
			# with the equirectangular picker display. Write a PNG and hand its path + ACTUAL extent
			# (I.range, which loose tile bounds may widen) back to the dialog.
			off = length(parts) >= 7 ? something(tryparse(Int, parts[7]), 3) : 3
			bz = max(1, zoom - off)
			kw = Dict{Symbol,Any}(:zoom => bz)
			isempty(prov)  || (kw[:provider] = prov)
			isempty(cache) || (kw[:cache]    = cache)
			I = _mosaic_logged(scene, dlg, [W, E], [S, N], kw)
			png = joinpath(tempdir(), "igmt_tiles_bg_$(time_ns()).png")   # unique name -> never a stale reload
			GMT.gmtwrite(png, I)
			isempty(_LAST_BG[]) || rm(_LAST_BG[]; force=true)
			_LAST_BG[] = png
			r = I.range
			ccall(_fn(:gmtvtk_tiles_set_bg), Cvoid,
			      (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble),
			      dlg, png, Float64(r[1]), Float64(r[2]), Float64(r[3]), Float64(r[4]))
		elseif op == "footprints"
			# "how is the data under this region tiled?" — the map itself has no idea, and must not:
			# it is meant to be reused by other tools. Field 3 is "<provider>:<argument>", and the
			# provider is looked up in the registry a tool fills in for itself (_fp_register).
			kind = fld(3)
			k    = findfirst(==(':'), kind)
			name = k === nothing ? kind : kind[1:k-1]
			arg  = k === nothing ? ""   : kind[k+1:end]
			fn   = get(_FP_PROVIDERS, name, nothing)
			rects, names = fn === nothing ? (Float64[], String[]) : fn(W, E, S, N, arg)
			ccall(_fn(:gmtvtk_tiles_set_footprints), Cvoid,
			      (Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint),
			      dlg, rects, join(names, '\n'), Cint(length(names)))
		elseif op == "prefill"
			# Fill the tile cache for a whole area at the given zoom levels, ONCE, so the picker is not
			# downloading while the user is trying to aim it. Field 3 is the list ("8,9,10"). Nothing is
			# displayed: the point is purely what lands in ~/.gmt/cache_tileserver, which every later
			# mosaic over that ground then reads from disk.
			# ONE mosaic per zoom level, which is what this cost when measured: over mainland Portugal a
			# fully cached level 10 assembles a 2970x4434 image and reprojects it in 1.3 s, and skipping
			# the reprojection (merc=true) saves 0.04 s of that — nothing. Cutting the box into 4x4-tile
			# chunks to avoid the big assembly was measured too, and is 3.4x SLOWER (6.1 s vs 1.8 s):
			# per-call overhead dwarfs what it saves. So: no chunking, no merc, one call per level.
			#
			# The whole cost of a first run is DOWNLOADING the tiles, so the only lever that matters is
			# how many levels are asked for — see the caller, which asks for the two the picker reads.
			zooms = filter(z -> z !== nothing, tryparse.(Int, split(fld(3), ',')))
			# IN THE BACKGROUND, and this is the whole point of the op: hundreds of tiles take MINUTES,
			# and every other request here runs synchronously on the UI thread — which is the thread the
			# Qt loop and the whole viewer live on. Run this one there and iGMT is frozen solid until the
			# last tile lands. `@async` returns at once; the download yields to the scheduler during each
			# network wait (the same cooperative trick GMT.mosaic uses internally), so the Julia pump
			# timer keeps running and the window stays alive. The dialog is told it is over by the final
			# note, not by this call returning.
			@async begin
				for (k, z) in enumerate(zooms)
					# In SMALL PIECES, and here that is not about speed (one call per level is faster,
					# measured). It is the lock: every mosaic runs under _TILE_LOCK, and a whole level
					# is minutes of downloading, so a single call would hold it for that long and every
					# pan the user makes meanwhile would sit behind it — the freeze, moved rather than
					# fixed. A 4x4-tile piece holds it for well under a second.
					n = 1 << z
					tx0 = floor(Int, (W + 180) / 360 * n);  tx1 = floor(Int, (E + 180) / 360 * n)
					ty(l) = floor(Int, (1 - asinh(tand(l)) / pi) / 2 * n)
					ty0, ty1 = ty(N), ty(S)                   # north is the smaller tile row
					x2lon(x) = x / n * 360 - 180
					y2lat(y) = atand(sinh(pi * (1 - 2 * y / n)))
					CH = 4
					pieces = [(cx, min(cx + CH - 1, tx1), cy, min(cy + CH - 1, ty1))
					          for cx in tx0:CH:tx1 for cy in ty0:CH:ty1]
					_tiles_log(dlg, "prefill $k/$(length(zooms)): zoom level $z " *
					                "($((tx1-tx0+1)*(ty1-ty0+1)) tiles)")
					for (a, b, c, d) in pieces
						kw = Dict{Symbol,Any}(:zoom => z)
						isempty(prov)  || (kw[:provider] = prov)
						isempty(cache) || (kw[:cache]    = cache)
						try
							_mosaic_logged(scene, dlg, [x2lon(a), x2lon(b + 1)], [y2lat(d + 1), y2lat(c)], kw)
						catch e                               # one bad piece must not lose the rest
							_tiles_log(dlg, "prefill zoom $z failed: $(sprint(showerror, e))")
						end
						yield()                               # let the viewer breathe between pieces
					end
				end
				_tiles_log(dlg, "prefill done")
			end
		end
	catch e
		# The dialog that asked, and nowhere else: its Messages pane is where this tool's errors land.
		_tiles_log(dlg, "ERROR: $(sprint(showerror, e))")
		@tool_error "tilestool: request failed" exception=(e,)
	end
	return
end

# Build the C-callable pointer. Lazy (first window) via _ensure_callbacks — the @cfunction is a thin
# invokelatest trampoline so it drags no GMT into compile.
function _register_tiles()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_tiles, s, d, c),
	                  Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_tiles_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("tiles", _tiles_warm)     # C++ fires this when a map picker opens (70_window.cpp)
	return
end

# Warm-up (see warmup.jl): the first background fetch pays for compiling GMT.mosaic, its quadtree, the
# GDAL warp and gmtwrite — several seconds with nothing on screen to explain them. Run the SAME path
# here, on a tiny box at a coarse zoom, while the user is still looking at the map. Nothing is
# displayed and the live scene is never touched.
function _tiles_warm()
	try
		# Under the same lock as every other mosaic here: this runs as a background task, so without it
		# the warm-up would be the SECOND concurrent GMT call it is meant to spare the user.
		lock(_TILE_LOCK) do
			GMT.mosaic([-9.2, -9.1], [38.7, 38.8]; quadonly=true, zoom=3)   # quadtree only, no network
			I = GMT.mosaic([-9.2, -9.1], [38.7, 38.8]; zoom=3, provider="OSM", cache="gmt")
			png = joinpath(tempdir(), "igmt_tiles_warm.png")   # the same write the bg op does
			GMT.gmtwrite(png, I)
			rm(png; force=true)
		end
	catch
	end
	return nothing
end

# The equirectangular world image the picker crops/zooms as its base (the same bundled data/etopo4.jpg
# the Base Map picker uses; `_etopo4_path` lives in basemap.jl). A static path push (no GMT inference) ->
# installed eagerly in __init__, before the first window builds.
function _install_tiles_assets()
	ccall(_fn(:gmtvtk_set_tiles_world), Cvoid, (Cstring,), _etopo4_path())
	return
end
