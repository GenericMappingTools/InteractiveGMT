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

# Run GMT.mosaic and report progress to the viewer's Errors console (the user watches tiles download
# there). We do NOT redirect stdout/stderr around the download — an fd-level redirect corrupts GMT's
# tile fetch (it throws, the error gets swallowed into the log, and no image comes back). Instead we
# count the tiles up front with a cheap `quadonly` pass (pure quadtree math, no download) and bracket
# the real fetch with a "downloading N tile(s)…" / "mosaic ready WxH" pair. Returns the GMTimage.
function _mosaic_logged(scene::Ptr{Cvoid}, lon, lat, kw::Dict{Symbol,Any})
	z = get(kw, :zoom, 0)
	n = try
		q  = GMT.mosaic(lon, lat; quadonly=true, kw...)
		qt = q isa Tuple ? q[1] : q
		qt isa AbstractString ? 1 : length(qt)
	catch
		0
	end
	_viewer_log_error(scene, n > 0 ?
		"Tiles: downloading $n tile(s) at zoom $z for $(lon[1])/$(lon[2])/$(lat[1])/$(lat[2])…" :
		"Tiles: fetching $(lon[1])/$(lon[2])/$(lat[1])/$(lat[2]) at zoom $z…")
	I = GMT.mosaic(lon, lat; kw...)
	_viewer_log_error(scene, "Tiles: mosaic ready ($(size(I.image, 2))×$(size(I.image, 1)) px)")
	return I
end

function _on_tiles(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		parts = split(unsafe_string(cparams), ';')
		op = parts[1]
		W, E, S, N = parse.(Float64, split(parts[2], '/'))
		zoom  = parse(Int, parts[3])
		prov  = String(parts[4])
		cache = String(parts[5])
		merc  = parts[6] == "1"
		if op == "go"
			fz = min(22, zoom + 2)                         # final mosaic is two zoom levels FINER than the picker mesh
			kw = Dict{Symbol,Any}(:zoom => fz)
			isempty(prov)  || (kw[:provider] = prov)
			isempty(cache) || (kw[:cache]    = cache)
			merc && (kw[:merc] = true)
			I = _mosaic_logged(scene, [W, E], [S, N], kw)
			fig = iview(I)                                 # bare image -> new flat top-down viewer window
			# We're nested inside the picker's GO-click handler; the fresh window can open BEHIND the
			# picker, so the user thinks "nothing happened". Raise it to the front.
			try
				h = _fig_handle(fig)
				h == C_NULL || ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), h)
			catch
			end
		elseif op == "bg"
			# Coarser background for the picker's current view at high zoom (Mirone's bgZoomLevel = zoom-3).
			# Always geographic (no merc) so it aligns with the equirectangular picker display. Write a PNG
			# and hand its path + ACTUAL extent (I.range, which loose tile bounds may widen) back to the dialog.
			bz = max(1, zoom - 3)
			kw = Dict{Symbol,Any}(:zoom => bz)
			isempty(prov)  || (kw[:provider] = prov)
			isempty(cache) || (kw[:cache]    = cache)
			I = _mosaic_logged(scene, [W, E], [S, N], kw)
			png = joinpath(tempdir(), "igmt_tiles_bg_$(time_ns()).png")   # unique name -> never a stale reload
			GMT.gmtwrite(png, I)
			isempty(_LAST_BG[]) || rm(_LAST_BG[]; force=true)
			_LAST_BG[] = png
			r = I.range
			ccall(_fn(:gmtvtk_tiles_set_bg), Cvoid,
			      (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble),
			      dlg, png, Float64(r[1]), Float64(r[2]), Float64(r[3]), Float64(r[4]))
		end
	catch e
		_viewer_log_error(scene, "Tiles Tool FAILED: $(sprint(showerror, e))")
		@warn "tilestool: request failed" exception=(e,)
	end
	return
end

# Build the C-callable pointer. Lazy (first window) via _ensure_callbacks — the @cfunction is a thin
# invokelatest trampoline so it drags no GMT into compile.
function _register_tiles()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_tiles, s, d, c),
	                  Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_tiles_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# The equirectangular world image the picker crops/zooms as its base (the same bundled data/etopo4.jpg
# the Base Map picker uses; `_etopo4_path` lives in basemap.jl). A static path push (no GMT inference) ->
# installed eagerly in __init__, before the first window builds.
function _install_tiles_assets()
	ccall(_fn(:gmtvtk_set_tiles_world), Cvoid, (Cstring,), _etopo4_path())
	return
end
