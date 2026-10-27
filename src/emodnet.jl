# emodnet.jl — Tools > "EMODNET": the EMODnet Bathymetry DTM over a region drawn on a map.
#
# The C++ side is EmodnetDialog (70_window.cpp): the etopo world image bounded to the tiles' ground,
# the tiles drawn on it, and a rectangle dragged over it.
#
# EMODnet publishes its DTM as a fixed set of tiles (A1 … H8 plus "caribbean"), each a zipped 32-bit
# float GeoTIFF. The tile list, footprints and URLs are NOT restated here: they come from EMODnet's
# own WFS layer `emodnet:download_tiles` (the one their geoviewer draws), read once per session, so a
# new DTM release is picked up without touching this file.
#
# The data are never fetched by us. GDAL reads each tile straight off the server through
# /vsizip//vsicurl/, and `gdal_translate -projwin` takes only the part of the tile inside the region.
# A region that crosses several tiles gets one such piece per tile, and the pieces are mosaicked by a
# VRT over them — GDAL again. The tiles overlap by two cells on every side; each piece is cut to its
# tile's NOMINAL box, so the overlap is never read twice and the pieces meet edge to edge.
#
# The callback is wired on first use (warm_register "emodnet"), never at start-up.

const _EMOD_WFS = "https://ows.emodnet-bathymetry.eu/wfs?service=WFS&version=2.0.0&request=GetFeature" *
                  "&typeNames=emodnet:download_tiles&outputFormat=application/json"
const _EMOD_FORMAT = "32 bit float GeoTiff"     # the one product GDAL can subset

struct _EmodTile
	name::String
	W::Float64; E::Float64; S::Float64; N::Float64
	url::String
	release::String
end

const _EMOD_TILES = Ref{Vector{_EmodTile}}(_EmodTile[])
const _EMODNET_WIRED = Ref(false)

_emod_log(dlg::Ptr{Cvoid}, msg::AbstractString) = (try
	ccall(_fn(:gmtvtk_emodnet_log), Cvoid, (Ptr{Cvoid}, Cstring), dlg, String(msg))
catch; end; nothing)

# The GeoTIFF tiles of the WFS answer. The GeoJSON is flat and regular, so each feature is read with a
# regex per field rather than pulling in a JSON package for five strings and a bbox.
function _emod_parse_tiles(txt::AbstractString)::Vector{_EmodTile}
	out = _EmodTile[]
	for f in split(txt, "{\"type\":\"Feature\"")[2:end]
		fmt = match(r"\"product_format\":\"([^\"]*)\"", f)
		(fmt === nothing || fmt[1] != _EMOD_FORMAT) && continue
		nm  = match(r"\"dtm_tile\":\"([^\"]*)\"", f)
		url = match(r"\"download_url\":\"([^\"]*)\"", f)
		rel = match(r"\"dtm_release\":\"([^\"]*)\"", f)
		bb  = match(r"\"bbox\":\[([^\]]*)\]", f)
		(nm === nothing || url === nothing || bb === nothing) && continue
		b = tryparse.(Float64, split(bb[1], ','))
		(length(b) == 4 && all(!isnothing, b)) || continue
		# GeoJSON bbox is [minx, miny, maxx, maxy]; ours is W, E, S, N.
		push!(out, _EmodTile(nm[1], b[1], b[3], b[2], b[4], url[1], rel === nothing ? "" : rel[1]))
	end
	return sort!(out; by = t -> t.name)
end

# The tile table, read from the WFS the first time it is asked for in a session.
function _emod_tiles()::Vector{_EmodTile}
	isempty(_EMOD_TILES[]) || return _EMOD_TILES[]
	io = IOBuffer()
	GMT.Downloads.download(_EMOD_WFS, io; timeout = 60)
	t = _emod_parse_tiles(String(take!(io)))
	isempty(t) && error("the EMODnet WFS answered, but listed no \"$_EMOD_FORMAT\" tiles")
	_EMOD_TILES[] = t
	return t
end

# The GDAL path of a tile's GeoTIFF: the file inside the zip carries the zip's own name minus ".zip".
_emod_vsi(t::_EmodTile) = "/vsizip//vsicurl/" * t.url * "/" * replace(basename(t.url), r"\.zip$" => "")

# Snap a region OUTWARDS to the DTM's cell edges (1/16 arc-minute = 1/960 degree), so every -projwin
# lands on whole cells and the pieces of neighbouring tiles butt exactly.
const _EMOD_CELLS_PER_DEG = 960
_emod_snap(W, E, S, N) = (floor(W * _EMOD_CELLS_PER_DEG) / _EMOD_CELLS_PER_DEG,
                          ceil(E * _EMOD_CELLS_PER_DEG) / _EMOD_CELLS_PER_DEG,
                          floor(S * _EMOD_CELLS_PER_DEG) / _EMOD_CELLS_PER_DEG,
                          ceil(N * _EMOD_CELLS_PER_DEG) / _EMOD_CELLS_PER_DEG)

# --- What GDAL is fetching, while it fetches -----------------------------------------------------
# GDAL's /vsicurl/ announces every HTTP range it requests as a CPL_DEBUG line
# ("VSICURL: Downloading <first>-<last> (<url>)..."). Those lines are the progress report: bytes of
# the zip fetched so far, and how far into the zip GDAL has got. They are caught by a THREAD-LOCAL
# error handler on the worker thread that runs the open + translate, so nothing else in the session
# is touched (GMT.gdaltranslate cannot be used for this: it runs under GDAL's quiet handler, which
# sends debug lines straight to stderr).
const _EMOD_BYTES  = Threads.Atomic{Int}(0)   # sum of the ranges GDAL has requested
const _EMOD_MAXOFF = Threads.Atomic{Int}(0)   # end of the range GDAL requested last
const _EMOD_OPENED = Threads.Atomic{Bool}(false)
const _EMOD_CACHE_BYTES = 512 * 1024^2          # > the largest tile zip (~300 MB)

function _emod_cpl_handler(cls::UInt32, no::Cint, msg::Cstring)::Cvoid
	s = unsafe_string(msg)
	m = match(r"VSICURL: Downloading (\d+)-(\d+)", s)
	if m !== nothing
		a, b = parse(Int, m[1]), parse(Int, m[2])
		Threads.atomic_add!(_EMOD_BYTES, b - a + 1)
		_EMOD_MAXOFF[] = b + 1
	elseif occursin("succeeds as", s)
		_EMOD_OPENED[] = true
	end
	return nothing
end

# Size of a tile's zip, for "x of y MB". 0 when the server does not say.
function _emod_zip_size(url::String)::Int
	try
		r = GMT.Downloads.request(url; method = "HEAD", throw = false, timeout = 20)
		for (k, v) in r.headers
			lowercase(k) == "content-length" && return something(tryparse(Int, v), 0)
		end
	catch
	end
	return 0
end

# GDAL open + gdal_translate of one tile's piece into `dest`, on a worker thread, reporting what GDAL
# fetches once a second through `log`. Same GDAL calls GMT.gdaltranslate makes (GDALOpenEx,
# GDALTranslate); only the handler around them differs. With `whole=true` there is no translate:
# the opened tile itself is read into a GMTgrid (gd2gmt) and returned — straight into memory.
function _emod_read_piece(t::_EmodTile, opts::Vector{String}, dest::String, log::Function;
                          whole::Bool = false)
	G   = GMT.Gdal
	src = _emod_vsi(t)
	tot = _emod_zip_size(t.url)
	_EMOD_BYTES[] = 0;  _EMOD_MAXOFF[] = 0;  _EMOD_OPENED[] = false
	# GDAL's /vsicurl/ keeps what it downloaded in an in-memory cache, 16 MB by default. Opening the
	# zipped GeoTIFF has already pulled the zip through, and reading the rows afterwards then fetched it
	# all over again (C6: 17 s to read the rows; 2.3 s with the zip still cached). RAM only — no file.
	G.setconfigoption("CPL_VSIL_CURL_CACHE_SIZE", string(_EMOD_CACHE_BYTES))
	err = Ref("")
	out = Ref{Any}(nothing)
	work = function ()
		h = @cfunction(_emod_cpl_handler, Cvoid, (UInt32, Cint, Cstring))
		ccall((:CPLSetThreadLocalConfigOption, GMT.Gdal.libgdal), Cvoid, (Cstring, Cstring), "CPL_DEBUG", "ON")
		ccall((:CPLPushErrorHandler, GMT.Gdal.libgdal), Cvoid, (Ptr{Cvoid},), h)
		try
			ds = G.GDALOpenEx(src, G.GDAL_OF_READONLY | G.GDAL_OF_RASTER | G.GDAL_OF_VERBOSE_ERROR,
			                  C_NULL, C_NULL, C_NULL)
			ds == C_NULL && error("GDAL could not open $src")
			try
				if whole
					out[] = GMT.gd2gmt(G.Dataset(ds))
					return nothing
				end
				o = G.GDALTranslateOptionsNew(opts, C_NULL)
				r = G.GDALTranslate(dest, ds, o, Ref{Cint}(0))
				G.GDALTranslateOptionsFree(o)
				r == C_NULL && error("gdal_translate failed on tile $(t.name)")
				G.GDALClose(r)
			finally
				G.GDALClose(ds)
			end
		catch e
			err[] = sprint(showerror, e)
		finally
			ccall((:CPLPopErrorHandler, GMT.Gdal.libgdal), Cvoid, ())
			ccall((:CPLSetThreadLocalConfigOption, GMT.Gdal.libgdal), Cvoid, (Cstring, Ptr{Cchar}), "CPL_DEBUG", C_NULL)
		end
		return nothing
	end
	MB(b) = round(b / 1048576; digits = 1)
	secs(x) = string(round(Int, x), " s")
	t0 = time()
	if Threads.nthreads() > 1
		task = Threads.@spawn work()
		# One line every 2 s while bytes arrive (amount, %, CURRENT rate, time left), one line when the
		# transfer stops, then silence until the grid is ready — never the same line repeated.
		lastT, lastB, said_end = t0, 0, false
		while !istaskdone(task)
			Libc.systemsleep(0.25)         # no yield: this runs inside a Qt callback
			now, b = time(), _EMOD_BYTES[]
			now - lastT < 2.0 && continue
			if b > lastB
				rate = (b - lastB) / (now - lastT)
				left = (tot > b && rate > 0) ? ", ~$(secs((tot - b) / rate)) left" : ""
				pct  = tot > 0 ? " ($(round(Int, 100 * min(b, tot) / tot))%)" : ""
				log("   $(t.name): downloaded $(MB(b))$(tot > 0 ? " of $(MB(tot))" : "") MB$pct, " *
				    "$(MB(rate)) MB/s$left")
				said_end = false
			elseif !said_end && b > 0
				log("   $(t.name): transfer finished ($(MB(b)) MB in $(secs(now - t0))) — decoding the grid …")
				said_end = true
			end
			lastT, lastB = now, b
		end
	else
		work()
	end
	G.VSICurlClearCache()                  # the zip held in RAM for this read is not needed any more
	isempty(err[]) || error(err[])
	log("   $(t.name): done in $(secs(time() - t0)), $(MB(_EMOD_BYTES[])) MB transferred")
	return out[]
end

"""
    emodnet_dtm(W, E, S, N; log=println) -> GMTgrid

The EMODnet Bathymetry DTM over `W/E/S/N` (degrees), read by GDAL straight off EMODnet's tiles: one
`-projwin` piece per tile the region touches, mosaicked through a VRT.
"""
function emodnet_dtm(W::Float64, E::Float64, S::Float64, N::Float64; log::Function = println)
	(W < E && S < N) || error("the region must have West < East and South < North")
	W, E, S, N = _emod_snap(W, E, S, N)
	hits = [t for t in _emod_tiles() if t.W < E && t.E > W && t.S < N && t.N > S]
	isempty(hits) && error("the region $W/$E/$S/$N does not touch any EMODnet DTM tile")
	log("region $W/$E/$S/$N crosses $(length(hits)) tile(s): $(join((t.name for t in hits), ", "))")
	pieces = String[]
	vrt = "/vsimem/emodnet_mosaic.vrt"
	try
		for (k, t) in enumerate(hits)
			w, e, s, n = max(W, t.W), min(E, t.E), max(S, t.S), min(N, t.N)
			piece = "/vsimem/emodnet_$(t.name).tif"
			log("[$k/$(length(hits))] tile $(t.name): GDAL is reading $w/$e/$s/$n …")
			_emod_read_piece(t, ["-projwin", string(w), string(n), string(e), string(s), "-of", "GTiff"],
			                 piece, log)
			push!(pieces, piece)
		end
		log("mosaicking $(length(pieces)) piece(s) …")
		# Named, not GDAL's default "/vsimem/tmp": that is also where gdaltranslate puts its own
		# result, and GDAL refuses a translate whose source and destination are the same dataset.
		GMT.gdalbuildvrt(pieces, ["-srcnodata", "nan", "-vrtnodata", "nan"]; dest = vrt)
		G = GMT.gdaltranslate(vrt)
		isa(G, GMTgrid) || error("GDAL did not return a grid (got $(typeof(G)))")
		return G
	finally
		for p in pieces
			GMT.Gdal.VSIUnlink(p)
		end
		GMT.Gdal.VSIUnlink(vrt)
	end
end

# C callback (EmodnetDialog): `cparams` is a newline-separated "key=value" block.
#   mode=tiles     push the tile table back into the dialog (gmtvtk_emodnet_set_tiles)
#   mode=download  region=W/E/S/N, optional outfile=<path>: read the DTM, add it to the window
# Returns Cint 1 on success, 0 on failure.
function _on_emodnet(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d    = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode", "download")
		if mode == "tiles"
			rects, names = Float64[], String[]
			rel = ""
			for t in _emod_tiles()
				append!(rects, (t.W, t.E, t.S, t.N))
				push!(names, t.name)
				rel = t.release
			end
			ccall(_fn(:gmtvtk_emodnet_set_tiles), Cvoid,
			      (Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint, Cstring),
			      dlg, rects, join(names, '\n'), Cint(length(names)), rel)
			return Cint(1)
		end
		if mode == "tile"
			name = _get(d, "tile")
			k = findfirst(t -> t.name == name, _emod_tiles())
			k === nothing && error("no EMODnet tile called \"$name\"")
			t = _emod_tiles()[k]
			_emod_log(dlg, "tile $name: GDAL is reading the whole tile …")
			G = _emod_read_piece(t, String[], "", m -> _emod_log(dlg, m); whole = true)
			isa(G, GMTgrid) || error("GDAL did not return a grid for tile $name (got $(typeof(G)))")
			title = "EMODnet DTM $(t.release) tile $name"
			ok = _gm3d_deliver(scene, G, title, "", false, "emodnet_tile $name"; geographic = true)
			nx, ny = GMT.getsize(G)
			ok == Cint(1) && _emod_log(dlg, "\"$title\" added to the window ($(nx)×$(ny) nodes)")
			return ok
		end
		bbox = _dgt_bbox(_get(d, "region"))
		G = emodnet_dtm(bbox[1], bbox[2], bbox[3], bbox[4]; log = m -> _emod_log(dlg, m))
		outfile = strip(_get(d, "outfile"))
		if !isempty(outfile)
			GMT.gmtwrite(String(outfile), G)
			_emod_log(dlg, "written to $outfile")
		end
		r = G.range
		rel = isempty(_EMOD_TILES[]) ? "" : " " * _EMOD_TILES[][1].release
		title = "EMODnet DTM$rel ($(round(r[1]; digits=4))/$(round(r[2]; digits=4))/" *
		        "$(round(r[3]; digits=4))/$(round(r[4]; digits=4)))"
		recipe = "emodnet_dtm $(bbox[1])/$(bbox[2])/$(bbox[3])/$(bbox[4])"
		ok = _gm3d_deliver(scene, G, title, "", false, recipe; geographic = true)
		nx, ny = GMT.getsize(G)
		ok == Cint(1) && _emod_log(dlg, "\"$title\" added to the window ($(nx)×$(ny) nodes)")
		return ok
	catch e
		_emod_log(dlg, "FAILED: $(sprint(showerror, e))")
		_tool_failed(scene, "EMODNET", e)
		return Cint(0)
	end
end

function _register_emodnet()
	_EMODNET_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_emodnet_callback) || return   # a DLL built without the tool
	fptr = @cfunction((s, w, c) -> Base.invokelatest(_on_emodnet, s, w, c)::Cint,
	                  Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_emodnet_callback), Cvoid, (Ptr{Cvoid},), fptr)
	_EMODNET_WIRED[] = true
	return
end
warm_register("emodnet", _register_emodnet)
