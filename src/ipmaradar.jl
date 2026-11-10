# ipmaradar.jl — Tools > PT Tools > "IPMA radar rain": IPMA's radar rain-rate images of mainland Portugal.
#
# The C++ side is IpmaRadarDialog (70_window.cpp): Start/End times, Fetch, a frame slider, Play, Save,
# and a zoomable map (IpmaMapView) that draws the frame over a background — web tiles (OpenStreetMap by
# default) or a grid/image file — all in Web Mercator.
#
# Where the images come from. IPMA's page (https://www.ipma.pt/pt/otempo/obs.remote/) is a Leaflet map:
# imgs-radar.json lists the last 3 h of frames, one every 5 minutes, newest first, and each frame is a
# palette PNG at <base>/por/pcr-YYYY-MM-DDTHHMM.png that mapbuilder-pt.js lays over `boundspor`
# (_IPMA_BOUNDS here). The server keeps frames older than its index lists — found 2026-11-08: one week
# back still answered, one month back was 404 — so a frame is asked for by its NAME, never looked up in
# the index. The index is read only to know the newest frame.
#
# Frames are kept in memory as the PNG bytes IPMA sent (_IPMA_PNG, keyed by frame time), once per
# session, and handed to the dialog as they are: it decodes them itself.
#
# "Save" writes the frames on the dialog as ONE netCDF-4 cube (time, y, x), deflated, with ONE palette.
# IPMA paints every frame with the same colour scale, but each PNG file numbers its colours its own way:
# its index table holds only the colours that frame uses, in its own order (158 to 179 entries over the
# 37 frames of 2026-11-08 16:00-19:00), so the same pixel value is a different colour from one file to
# the next. The save puts every frame on one common table — the colours of all the frames saved, sorted —
# so a value in the cube means the same colour at every time step. Index 0 is "no rain" (transparent).
#
# Georeference. Leaflet stretches an ImageOverlay over its bounds in Web Mercator, so x/y are written in
# EPSG:3857 metres. The image's height/width (2331/1500 = 1.554) is close to the Mercator ratio of those
# bounds (1.52) and far from the plain lon/lat one (1.21) — measured on the aspect only, not yet checked
# against the coastline drawn in the image.
#
# The callback is wired on first use (warm_register "ipma"), never at start-up.

const _IPMA_BASE   = "https://www.ipma.pt/resources.www/transf/radar/"
const _IPMA_INDEX  = _IPMA_BASE * "imgs-radar.json"
const _IPMA_BOUNDS = (-12.454795, -4.345465, 34.011513, 43.792862)   # W, E, S, N (mapbuilder-pt.js `boundspor`)
const _IPMA_PNG    = Dict{String,Vector{UInt8}}()                    # "yyyy-mm-ddTHHMM" -> PNG bytes
const _IPMA_WIRED  = Ref(false)
const _IPMA_STEP   = GMT.Dates.Minute(5)

_ipma_key(t::GMT.Dates.DateTime)::String   = GMT.Dates.format(t, "yyyy-mm-ddTHHMM")
_ipma_label(t::GMT.Dates.DateTime)::String = GMT.Dates.format(t, "yyyy-mm-dd HH:MM")
_ipma_time(s::String)::GMT.Dates.DateTime  = GMT.Dates.DateTime(strip(s), GMT.Dates.dateformat"yyyy-mm-dd HH:MM")

# One GET. Returns (HTTP status, body); a network failure (no answer at all) is an error.
function _ipma_get(url::String)::Tuple{Int,Vector{UInt8}}
	io = IOBuffer()
	r = GMT.Downloads.request(url; output = io, throw = false, timeout = 60)
	r isa GMT.Downloads.Response || error("IPMA: no answer from $url ($(r.message))")
	return (r.status, take!(io))
end

# The newest frame IPMA lists, as "yyyy-mm-dd HH:MM". The index is flat JSON; the dates are read with a
# regex rather than pulling in a JSON package for one field.
function _ipma_latest()::String
	st, b = _ipma_get(_IPMA_INDEX)
	st == 200 || error("IPMA: the frame index answered HTTP $st")
	ds = [String(m.captures[1]) for m in eachmatch(r"\"date\":\s*\"(\d{4}-\d\d-\d\d \d\d:\d\d)\"", String(b))]
	isempty(ds) && error("IPMA: the frame index lists no frame")
	return maximum(ds)
end

# IPMA's newest frame as last read off its index. Re-read only when a span reaches past it — the index
# is not asked again for a span that is all in the past.
const _IPMA_NEWEST = Ref{Union{GMT.Dates.DateTime,Nothing}}(nothing)
# An End this close to the clock means "the latest". It is IPMA's own "current" span, the 3 h its index
# lists. 30 min was too short: the End box is SET to IPMA's newest frame, already ~15 min behind the clock,
# so an End left there fell out a few minutes later and Fetch stopped updating. An End further back than
# this is a past span and is never moved.
const _IPMA_HEAD   = GMT.Dates.Hour(3)

# What became of one 5-minute step of a fetch (the `status` of gmtvtk_ipma_add_frame).
const _IPMA_ST_MISSING    = 0   # IPMA has no such frame (404: too old, or not made yet)
const _IPMA_ST_KEPT       = 1   # the dialog already holds it: nothing is sent
const _IPMA_ST_MEMORY     = 2   # sent from this session's memory
const _IPMA_ST_DOWNLOADED = 3   # downloaded now

# Every 5-minute step from t0 to t1, reported to the dialog one by one (gmtvtk_ipma_add_frame) in time
# order, so the dialog rebuilds its frame list from this ONE walk of the span. Only what the dialog does
# not already hold (`have`, its frame labels) is sent, and only what this session does not already hold
# is downloaded. Returns (frames in the span, frames missing on IPMA's server, frames downloaded, the End
# actually used when it was cut to IPMA's newest frame, else "").
function _ipma_fetch(dlg::Ptr{Cvoid}, t0::GMT.Dates.DateTime, t1::GMT.Dates.DateTime,
                     have::Set{String})::Tuple{Int,Int,Int,String}
	t0 = floor(t0, _IPMA_STEP);  t1 = floor(t1, _IPMA_STEP)
	# AN END AT "NOW" MEANS "THE LATEST", and Fetch is then an UPDATE: an End within _IPMA_HEAD (3 h) of the
	# clock (IPMA publishes each frame ~10-15 min late) or in the future is moved to IPMA's newest frame,
	# read fresh off the index — so pressing Fetch again later brings in what IPMA has made since. An
	# End further back is the user's own and is only cut if it passes the newest frame known. Either
	# way the dialog is told the End it really got.
	head = t1 >= floor(GMT.Dates.now(GMT.Dates.UTC), _IPMA_STEP) - _IPMA_HEAD
	(head || _IPMA_NEWEST[] === nothing || t1 > _IPMA_NEWEST[]) && (_IPMA_NEWEST[] = _ipma_time(_ipma_latest()))
	newest = _IPMA_NEWEST[]
	moved = ""
	if head || t1 > newest
		t1 == newest || (moved = _ipma_label(newest))
		t1 = newest
		t0 > t1 && error("the Start time is after IPMA's newest frame ($(_ipma_label(newest)))")
	end
	t1 < t0 && error("the End time is before the Start time")
	ts = collect(t0:_IPMA_STEP:t1)
	got, miss, down = 0, 0, 0
	for (k, t) in enumerate(ts)
		key, label = _ipma_key(t), _ipma_label(t)
		b = UInt8[]
		status = _IPMA_ST_KEPT
		if !(label in have)
			b = get(_IPMA_PNG, key, UInt8[])
			status = _IPMA_ST_MEMORY
			if isempty(b)
				st, b = _ipma_get(_IPMA_BASE * "por/pcr-$key.png")
				if st == 404
					status, b = _IPMA_ST_MISSING, UInt8[]
				elseif st != 200
					error("IPMA: frame $key answered HTTP $st")
				else
					_IPMA_PNG[key] = b
					status = _IPMA_ST_DOWNLOADED
					down += 1
				end
			end
		end
		ccall(_fn(:gmtvtk_ipma_add_frame), Cvoid, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint, Cint, Cint, Cint),
		      dlg, label, b, Cint(length(b)), Cint(k), Cint(length(ts)), Cint(status))
		status == _IPMA_ST_MISSING ? (miss += 1) : (got += 1)
	end
	return (got, miss, down, moved)
end

# One PNG, decoded by GDAL straight out of memory (/vsimem/, no file on disk): the palette indices, row
# 0 = north and x fastest, and the palette as 256 RGBA entries (unused entries 0).
function _ipma_decode(b::Vector{UInt8}, key::String)::Tuple{Vector{UInt8},Int,Int,Vector{UInt8}}
	ccall((:GDALAllRegister, GMT.libgdal), Cvoid, ())
	path = "/vsimem/ipma_$key.png"
	GC.@preserve b begin
		fp = ccall((:VSIFileFromMemBuffer, GMT.libgdal), Ptr{Cvoid}, (Cstring, Ptr{UInt8}, UInt64, Cint),
		           path, b, UInt64(length(b)), Cint(0))
		fp == C_NULL && error("IPMA: GDAL could not take frame $key into memory")
		ccall((:VSIFCloseL, GMT.libgdal), Cint, (Ptr{Cvoid},), fp)
		ds = ccall((:GDALOpen, GMT.libgdal), Ptr{Cvoid}, (Cstring, Cint), path, Cint(0))
		try
			ds == C_NULL && error("IPMA: GDAL could not read frame $key as a PNG")
			nx = Int(ccall((:GDALGetRasterXSize, GMT.libgdal), Cint, (Ptr{Cvoid},), ds))
			ny = Int(ccall((:GDALGetRasterYSize, GMT.libgdal), Cint, (Ptr{Cvoid},), ds))
			band = ccall((:GDALGetRasterBand, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cint), ds, Cint(1))
			idx = Vector{UInt8}(undef, nx * ny)
			r = ccall((:GDALRasterIO, GMT.libgdal), Cint,
			          (Ptr{Cvoid}, Cint, Cint, Cint, Cint, Cint, Ptr{UInt8}, Cint, Cint, Cint, Cint, Cint),
			          band, Cint(0), Cint(0), Cint(0), Cint(nx), Cint(ny), idx, Cint(nx), Cint(ny), Cint(1), Cint(0), Cint(0))
			r == 0 || error("IPMA: GDAL failed reading the pixels of frame $key")
			ct = ccall((:GDALGetRasterColorTable, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), band)
			ct == C_NULL && error("IPMA: frame $key is not a palette image")
			nc = min(256, Int(ccall((:GDALGetColorEntryCount, GMT.libgdal), Cint, (Ptr{Cvoid},), ct)))
			pal = zeros(UInt8, 4 * 256)
			ce = zeros(Int16, 4)                                # GDALColorEntry: c1..c4 = R, G, B, alpha
			for i in 0:nc-1
				ccall((:GDALGetColorEntryAsRGB, GMT.libgdal), Cint, (Ptr{Cvoid}, Cint, Ptr{Int16}), ct, Cint(i), ce)
				for c in 1:4
					pal[4i + c] = UInt8(clamp(ce[c], 0, 255))
				end
			end
			return (idx, nx, ny, pal)
		finally
			ds == C_NULL || ccall((:GDALClose, GMT.libgdal), Cint, (Ptr{Cvoid},), ds)
			ccall((:VSIUnlink, GMT.libgdal), Cint, (Cstring,), path)
		end
	end
end

# Web Mercator (EPSG:3857) metres of a lon/lat, the projection Leaflet lays the image in.
_ipma_merc_x(lon::Float64)::Float64 = 6378137.0 * deg2rad(lon)
_ipma_merc_y(lat::Float64)::Float64 = 6378137.0 * log(tan(pi / 4 + deg2rad(lat) / 2))

# The frames named by `labels` ("yyyy-mm-dd HH:MM", oldest first), written as one netCDF-4 cube.
function _ipma_save(labels::Vector{String}, fname::String)::String
	isempty(labels) && error("no frames to save — Fetch first")
	ts = sort!([_ipma_time(l) for l in labels])
	frames = Vector{Vector{UInt8}}(undef, length(ts))
	for (k, t) in enumerate(ts)
		b = get(_IPMA_PNG, _ipma_key(t), UInt8[])
		isempty(b) && error("frame $(_ipma_label(t)) is not in memory — Fetch again")
		frames[k] = b
	end
	nt = length(ts)
	# Pass 1: the common table. Every opaque colour any saved frame's PNG lists, sorted so the same set of
	# frames always gives the same table; a transparent entry (alpha 0) is "no rain" = 0, the fill value.
	nx, ny = 0, 0
	cols = Set{NTuple{4,UInt8}}()
	for k in 1:nt
		_, fx, fy, pal = _ipma_decode(frames[k], _ipma_key(ts[k]))
		k == 1 && ((nx, ny) = (fx, fy))
		(fx, fy) == (nx, ny) || error("frame $(_ipma_label(ts[k])) is $(fx)x$(fy), the first one $(nx)x$(ny)")
		for i in 0:255
			pal[4i + 4] == 0x00 || push!(cols, (pal[4i + 1], pal[4i + 2], pal[4i + 3], pal[4i + 4]))
		end
	end
	table = sort!(collect(cols))
	gidx = Dict{NTuple{4,UInt8},Int}(c => i for (i, c) in enumerate(table))   # colour -> 1-based common index
	# 8 bits while the colours fit (they did over 3 h of 2026-11-08: 228), 16 when a longer span brings more.
	T = length(table) <= 255 ? UInt8 : UInt16
	X0, X1, Y0, Y1 = _ipma_merc_box()
	dx, dy = (X1 - X0) / nx, (Y1 - Y0) / ny
	xs = [X0 + (i - 0.5) * dx for i in 1:nx]                  # pixel centres; the image's edges ARE the bounds
	ys = [Y1 - (j - 0.5) * dy for j in 1:ny]                  # row 1 = north, as the PNG stores it
	epoch = GMT.Dates.DateTime(1970, 1, 1)
	tm = [GMT.Dates.value(t - epoch) / 60000.0 for t in ts]

	ds = _shnc_create_multidim(fname)
	try
		root = _shnc_root(ds)
		dimT = _shnc_create_dim(root, "time", nt)
		dimY = _shnc_create_dim(root, "y", ny)
		dimX = _shnc_create_dim(root, "x", nx)
		dimC = _shnc_create_dim(root, "color", length(table) + 1)
		dimR = _shnc_create_dim(root, "rgba", 4)
		edt64, edtS = _shnc_edt_f64(), _shnc_edt_str()
		edt8 = _shnc_ck(ccall((:GDALExtendedDataTypeCreate, GMT.libgdal), _SHNC_H, (UInt32,), UInt32(1)), "create Byte type")
		edtI = T === UInt8 ? edt8 :
		       _shnc_ck(ccall((:GDALExtendedDataTypeCreate, GMT.libgdal), _SHNC_H, (UInt32,), UInt32(2)), "create UInt16 type")

		# x/y named and unitised the CF way GDAL checks for a projected grid ("m", "x coordinate of projection").
		for (dim, nm, vals, un, long) in ((dimX, "x", xs, "m", "x coordinate of projection"),
		                                  (dimY, "y", ys, "m", "y coordinate of projection"),
		                                  (dimT, "time", tm, "minutes since 1970-01-01 00:00:00", "frame time (IPMA file name)"))
			a = _shnc_create_array(root, nm, dim, edt64; deflate = 0)
			_shnc_array_write!(a, edt64, vals)
			_shnc_set_str_attr!(a, false, "units", edtS, un)
			_shnc_set_str_attr!(a, false, "long_name", edtS, long)
			_shnc_release_array(a)
		end

		mk = (nm::String, dims::Vector{Ptr{Cvoid}}, edt::Ptr{Cvoid}, opts::Vector{String}) -> _shnc_ck(
			ccall((:GDALGroupCreateMDArray, GMT.libgdal), _SHNC_H,
			      (_SHNC_H, Cstring, Csize_t, Ptr{_SHNC_H}, _SHNC_H, Ptr{Ptr{UInt8}}),
			      root, nm, Csize_t(length(dims)), dims, edt, isempty(opts) ? Ptr{Ptr{UInt8}}(C_NULL) : _shnc_csl(opts)),
			"create array $nm")
		put = (arr::Ptr{Cvoid}, edt::Ptr{Cvoid}, start::Vector{UInt64}, cnt::Vector{UInt64}, buf::Vector) -> begin
			step = fill(Int64(1), length(start))
			r = ccall((:GDALMDArrayWrite, GMT.libgdal), Cint,
			          (_SHNC_H, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, _SHNC_H, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
			          arr, start, cnt, step, C_NULL, edt, buf, C_NULL, 0)
			r == 0 && error("IPMA: netCDF write failed" * ((e = _shnc_lasterr()) == "" ? "" : ": $e"))
		end

		# One chunk per frame: a frame is what is read back at a time, and deflate works on whole chunks.
		aI = mk("rain_index", Ptr{Cvoid}[dimT, dimY, dimX], edtI, ["COMPRESS=DEFLATE", "ZLEVEL=5", "BLOCKSIZE=1,$ny,$nx"])
		aP = mk("palette", Ptr{Cvoid}[dimC, dimR], edt8, String[])
		# Everything that DEFINES a variable (fill value, CRS, attributes) before its first write: netCDF
		# refuses a fill value once data exists.
		_shnc_set_str_attr!(aI, false, "long_name", edtS, "IPMA radar rain-rate image, palette index")
		_shnc_set_str_attr!(aI, false, "units", edtS, "index into palette(color,:); 0 = no rain")
		ccall((:GDALMDArraySetNoDataValueAsDouble, GMT.libgdal), Cint, (_SHNC_H, Cdouble), aI, 0.0)
		srs = ccall((:OSRNewSpatialReference, GMT.libgdal), Ptr{Cvoid}, (Ptr{UInt8},), C_NULL)
		ccall((:OSRImportFromEPSG, GMT.libgdal), Cint, (Ptr{Cvoid}, Cint), srs, Cint(3857))
		ccall((:GDALMDArraySetSpatialRef, GMT.libgdal), Cint, (_SHNC_H, Ptr{Cvoid}), aI, srs)
		ccall((:OSRDestroySpatialReference, GMT.libgdal), Cvoid, (Ptr{Cvoid},), srs)
		_shnc_set_str_attr!(aP, false, "long_name", edtS, "the colour of each rain_index value, R G B alpha (0-255); row 0 = no rain")

		ptab = zeros(UInt8, 4 * (length(table) + 1))          # row 0 stays (0,0,0,0): no rain
		for (i, c) in enumerate(table), j in 1:4
			ptab[4i + j] = c[j]
		end
		put(aP, edt8, UInt64[0, 0], UInt64[length(table) + 1, 4], ptab)

		# Pass 2: each frame's pixels through its own file-index -> common-index table.
		lut = zeros(T, 256)
		buf = Vector{T}(undef, nx * ny)
		for k in 1:nt
			idx, _, _, pal = _ipma_decode(frames[k], _ipma_key(ts[k]))
			for i in 0:255
				lut[i + 1] = pal[4i + 4] == 0x00 ? zero(T) : T(gidx[(pal[4i + 1], pal[4i + 2], pal[4i + 3], pal[4i + 4])])
			end
			@inbounds for p in eachindex(idx)
				buf[p] = lut[idx[p] + 1]
			end
			put(aI, edtI, UInt64[k - 1, 0, 0], UInt64[1, ny, nx], buf)
		end
		_shnc_release_array(aI)
		_shnc_release_array(aP)

		_shnc_set_str_attr!(root, true, "Conventions", edtS, "CF-1.8")
		_shnc_set_str_attr!(root, true, "title", edtS, "IPMA radar rain-rate images, mainland Portugal")
		_shnc_set_str_attr!(root, true, "institution", edtS, "IPMA - Instituto Portugues do Mar e da Atmosfera")
		_shnc_set_str_attr!(root, true, "source", edtS, _IPMA_BASE * "por/pcr-<yyyy-mm-dd>T<HHMM>.png")
		_shnc_set_str_attr!(root, true, "legend", edtS, _IPMA_BASE * "png-geo/legenda-radar.png (rain rate, mm/h)")
		_shnc_set_str_attr!(root, true, "lonlat_bounds_WESN", edtS, join(string.(_IPMA_BOUNDS), '/'))
		for h in (dimT, dimY, dimX, dimC, dimR); _shnc_release_dim(h); end
		for h in (edt64, edtS, edt8); _shnc_release_edt(h); end
		T === UInt8 || _shnc_release_edt(edtI)
		ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (Ptr{Cvoid},), root)   # before the close, or the file is left unflushed
	finally
		ccall((:GDALClose, GMT.libgdal), Cint, (Ptr{Cvoid},), ds)
	end
	return fname
end

# ------------------------------------------------------------------------------------------------
# BACKGROUNDS. The dialog draws in Web Mercator metres — the radar's own frame — so every background is
# brought there: web tiles come in it already (GMT.mosaic merc=true), a file is warped to it. Display
# only; nothing here is saved.

# The radar's Mercator box, the one the dialog frames and the file backgrounds are warped onto.
_ipma_merc_box()::NTuple{4,Float64} = (_ipma_merc_x(_IPMA_BOUNDS[1]), _ipma_merc_x(_IPMA_BOUNDS[2]),
                                       _ipma_merc_y(_IPMA_BOUNDS[3]), _ipma_merc_y(_IPMA_BOUNDS[4]))
_ipma_merc_lon(x::Float64)::Float64 = rad2deg(x / 6378137.0)
_ipma_merc_lat(y::Float64)::Float64 = rad2deg(atan(sinh(y / 6378137.0)))

# The image's outer EDGES: a grid-registered range names the centres of the edge pixels.
function _ipma_edges(I)::NTuple{4,Float64}
	r = I.range
	I.registration == 1 && return (Float64(r[1]), Float64(r[2]), Float64(r[3]), Float64(r[4]))
	hx, hy = I.inc[1] / 2, I.inc[2] / 2
	return (r[1] - hx, r[2] + hx, r[3] - hy, r[4] + hy)
end

# Any GMTimage as RGBA bytes, row 1 = north, x fastest (what a QImage RGBA8888 is), read through the
# ONE palette/layout-aware accessor (drape.jl). `alpha(r, c)`, when given, overrides the opacity.
function _ipma_pack_rgba(I::GMTimage; alpha::Union{Function,Nothing} = nothing)::Tuple{Vector{UInt8},Int,Int}
	pix, nb, w, h, rowmajor = _pixaccess_img(I)
	north = _north_first(I.layout, rowmajor)
	out = Vector{UInt8}(undef, 4 * w * h)
	@inbounds for r in 1:h
		lat = north ? r : (h - r + 1)
		for c in 1:w
			o = 4 * ((r - 1) * w + (c - 1))
			for b in 1:3
				out[o + b] = UInt8(pix(lat, c, b <= nb ? b : nb))
			end
			out[o + 4] = alpha !== nothing ? alpha(r, c) : nb >= 4 ? UInt8(pix(lat, c, 4)) : 0xff
		end
	end
	return (out, w, h)
end

_ipma_push_bg(dlg::Ptr{Cvoid}, which::Int, rgba::Vector{UInt8}, w::Int, h::Int, e::NTuple{4,Float64}) =
	ccall(_fn(:gmtvtk_ipma_set_bg), Cvoid,
	      (Ptr{Cvoid}, Cint, Ptr{UInt8}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble),
	      dlg, Cint(which), rgba, Cint(w), Cint(h), e[1], e[2], e[3], e[4])

# Web tiles over a Mercator box at one zoom level, straight from GMT.mosaic (and its tile cache).
# `which` 0 = the base layer under the whole radar, 1 = the sharper patch over what is on screen.
function _ipma_bg_tiles(dlg::Ptr{Cvoid}, which::Int, prov::String, variant::String,
                        box::NTuple{4,Float64}, zoom::Int)::String
	lon = [_ipma_merc_lon(box[1]), _ipma_merc_lon(box[2])]
	lat = [_ipma_merc_lat(box[3]), _ipma_merc_lat(box[4])]
	pv = isempty(variant) ? prov : (prov, variant)
	I = lock(_TILE_LOCK) do                    # one lock for every tile fetch (GMT is not reentrant)
		GMT.mosaic(lon, lat; zoom = zoom, provider = pv, cache = "gmt", merc = true)
	end
	isa(I, GMTimage) || error("the $prov mosaic came back as a $(typeof(I)), not an image")
	rgba, w, h = _ipma_pack_rgba(I)
	_ipma_push_bg(dlg, which, rgba, w, h, _ipma_edges(I))
	return "$prov zoom $zoom, $(w)x$(h) px"
end

# A grid or an image from disk, warped onto the radar's Mercator box (2x the radar's own pixel size, an
# alpha band marking where the file has no data). A grid is coloured with iGMT's default palette for it
# (`_default_cmap`: geo for topography, else turbo) AFTER the warp, so its NaN nodes — outside the
# file, or holes in it — stay see-through.
function _ipma_bg_file(dlg::Ptr{Cvoid}, path::String)::String
	isfile(path) || error("no file \"$path\"")
	D = _gmtread_trb(path)
	(D isa GMTgrid || D isa GMTimage) || error("\"$(basename(path))\" is not a grid or an image (read as $(typeof(D)))")
	c = crs_from(D; geographic = (_isgeog(D) == 1))
	hascrs(c) || error("\"$(basename(path))\" carries no coordinate system and is not lon/lat, so it cannot " *
	                   "be placed under the radar")
	X0, X1, Y0, Y1 = _ipma_merc_box()
	W, H = 3000, 4662                          # 2 x the radar frames' 1500 x 2331
	opts = ["-s_srs", _crs_srs(c), "-t_srs", "EPSG:3857", "-te", string(X0), string(Y0), string(X1), string(Y1),
	        "-ts", string(W), string(H), "-r", "bilinear"]
	if D isa GMTgrid
		Gw = GMT.gdalwarp(D, opts)
		isa(Gw, GMTgrid) || error("warping \"$(basename(path))\" did not give a grid")
		# The palette is NAMED and spread over this grid's own range: a bare grdimage takes GMT.jl's
		# current CPT, i.e. whatever palette the session made last (measured: a 2..40 one from another
		# grid painted this relief black below 2 m and white above 40 m).
		zf = filter(isfinite, Gw.z)
		isempty(zf) && error("\"$(basename(path))\" does not reach the radar's area")
		zmn, zmx = Float64(minimum(zf)), Float64(maximum(zf))
		zmx > zmn || (zmx = zmn + 1)
		C = GMT.makecpt(cmap = _default_cmap(D), range = (zmn, zmx), continuous = true)
		I = GMT.grdimage(Gw, C = C, A = true)
		Z = _zmat(Gw)                          # row 1 = SOUTH
		ny = size(Z, 1)
		rgba, w, h = _ipma_pack_rgba(I; alpha = (r, cc) -> isnan(Z[ny - r + 1, cc]) ? 0x00 : 0xff)
		(w, h) == (size(Z, 2), ny) || error("the coloured grid is $(w)x$(h), the grid $(size(Z, 2))x$(ny)")
		e = _ipma_edges(Gw)
	else
		Iw = GMT.gdalwarp(_to_band_planar(D), vcat(opts, ["-dstalpha"]))
		isa(Iw, GMTimage) || error("warping \"$(basename(path))\" did not give an image")
		rgba, w, h = _ipma_pack_rgba(Iw)
		e = _ipma_edges(Iw)
	end
	_ipma_push_bg(dlg, 0, rgba, w, h, e)
	_IPMA_BGFILE[] = (path, D isa GMTgrid ? String(_default_cmap(D)) : "")
	return "$(basename(path)), warped to Web Mercator for display"
end

# The file background last loaded and the palette it was coloured with ("" = an image), for the script.
const _IPMA_BGFILE = Ref{Tuple{String,String}}(("", ""))

# ------------------------------------------------------------------------------------------------
# PLOT WITH GMT.jl. The dialog's view as a GMT.jl script, shown in iGMT's ONE script editor (the
# dialog owns it, as a window or an X,Y plot does). The frame is IPMA's own PNG, georeferenced exactly
# as the dialog lays it (Web Mercator corners, `_ipma_merc_box`), put on lon/lat; the background is the
# same source the dialog shows; the map is a Mercator one of the area that was on screen. The radar is
# drawn RGBA, its no-rain pixels see-through, at the 0.8 opacity IPMA's page and the dialog use.
function _ipma_script_text(label::String, box::NTuple{4,Float64}, bg::String, prov::String,
                           variant::String, zoom::Int)::String
	t = _ipma_time(label)
	X0, X1, Y0, Y1 = _ipma_merc_box()
	f2(v) = GMT.@sprintf("%.2f", v)             # plain decimals: `string` would write -1.38646144e6
	f4(v) = GMT.@sprintf("%.4f", v)
	W, E = _ipma_merc_lon(box[1]), _ipma_merc_lon(box[2])
	S, N = _ipma_merc_lat(box[3]), _ipma_merc_lat(box[4])
	what = bg == "tiles" ? (isempty(variant) ? prov : "$prov $variant") :
	       bg == "file"  ? basename(_IPMA_BGFILE[][1]) : "no background"
	title = "IPMA radar rain rate  $label"
	frm = "frame = (axes = :WSen, annot = :auto), title = \"$title\""
	io = IOBuffer()
	println(io, "using GMT\n")
	println(io, "# IPMA radar rain rate, $label (the time in IPMA's file name), over $what.")
	println(io, "# From iGMT: Tools > PT Tools > IPMA radar rain > Plot with GMT.jl.")
	println(io, "# IPMA's frame is a palette PNG that its own map lays over the corners below in Web Mercator")
	println(io, "# (EPSG:3857); here it is georeferenced the same way and put on lon/lat. Legend (mm/h):")
	println(io, "# $(_IPMA_BASE)png-geo/legenda-radar.png\n")
	println(io, "frame  = \"$(_IPMA_BASE)por/pcr-$(_ipma_key(t)).png\"")
	println(io, "corner = [\"-a_ullr\", \"$(f2(X0))\", \"$(f2(Y1))\", \"$(f2(X1))\", \"$(f2(Y0))\"]   # the frame's corners, metres")
	println(io, "region = ($(f4(W)), $(f4(E)), $(f4(S)), $(f4(N)))                       # the area that was on screen\n")
	println(io, "rain = gdaltranslate(\"/vsicurl/\" * frame, vcat([\"-a_srs\", \"EPSG:3857\", \"-expand\", \"rgba\"], corner))")
	println(io, "rain = gdalwarp(rain, [\"-t_srs\", \"EPSG:4326\", \"-r\", \"near\"])\n")
	if bg == "tiles"
		pv = isempty(variant) ? "\"$prov\"" : "(\"$prov\", \"$variant\")"
		println(io, "bg = mosaic([region[1], region[2]], [region[3], region[4]]; provider = $pv, zoom = $zoom, cache = \"gmt\")")
		println(io, "grdimage(bg, region = region, proj = :merc, figsize = 15, $frm)")
	elseif bg == "file"
		path, cmap = _IPMA_BGFILE[]
		println(io, "bg = gmtread(raw\"$path\")")
		println(io, "GMT.isgeog(bg) || (bg = gdalwarp(bg, [\"-t_srs\", \"EPSG:4326\"]))    # the map is drawn on lon/lat")
		if isempty(cmap)
			println(io, "grdimage(bg, region = region, proj = :merc, figsize = 15, $frm)")
		else
			println(io, "C = makecpt(cmap = :$cmap, range = (bg.range[5], bg.range[6]), continuous = true)")
			println(io, "grdimage(bg, region = region, proj = :merc, figsize = 15, cmap = C, $frm)")
		end
	else
		println(io, "basemap(region = region, proj = :merc, figsize = 15, $frm)")
	end
	println(io, "grdimage!(rain, transparency = 20)")
	println(io, "showfig()")
	return String(take!(io))
end

# ------------------------------------------------------------------------------------------------
# SAVE MOVIE. The frames are the dialog's own map — each one shown in turn and grabbed exactly as it is
# on screen (gmtvtk_ipma_grab_frame), background and zoom included. Everything else is movie.jl's: the
# output/frame-folder naming (`_movie_paths`, `_moviedlg_stem`), the frame numbering (`_movie_frames`),
# ffmpeg and its arguments (`_movie_ffmpeg_path`, `_movie_ffmpeg_args`) and the shared progress bar —
# the same steps Tools > Make movie's `movie(...)` takes, so a film made here is encoded the same way.
# Only the capture differs: `movie` grabs an iGMT window's VTK view, and this dialog is not one.
function _ipma_movie(dlg::Ptr{Cvoid}, file::String, nframes::Int, rate::Float64)::String
	nframes >= 2 || error("a movie needs at least 2 frames — Fetch a longer span")
	ext = lowercase(lstrip(splitext(file)[2], '.'))
	format = _movie_format(isempty(ext) ? "mp4" : ext)
	format === :png && error("save as .mp4, .webm or .gif")
	stem, frame_dir, output = _movie_paths(_moviedlg_stem(file, String(format)), nothing, format)
	seq = _movie_frames(nframes)
	width = length(seq[1].tag)
	mkpath(frame_dir)
	created = String[]
	# ONE bar for both halves: frames grabbed (1..N), then frames encoded (N+1..2N).
	_progress_show_async(2nframes, "Save movie — rendering…")
	tgrab = tenc = 0.0
	try
		tgrab = @elapsed for f in seq
			path = joinpath(frame_dir, stem * "_" * f.tag * ".png")
			ccall(_fn(:gmtvtk_ipma_grab_frame), Cint, (Ptr{Cvoid}, Cint, Cstring), dlg, Cint(f.index - 1), path) == 1 ||
				error("could not grab frame $(f.index) to \"$path\"")
			push!(created, path)
			_progress_status(f.index, "Save movie — frame $(f.index)/$nframes")
		end
		isfile(output) && rm(output; force = true)
		exe = _movie_ffmpeg_path(nothing)
		args = _ipma_mp4_small(_movie_ffmpeg_args(exe, joinpath(frame_dir, stem * "_%0$(width)d.png"), output, format, rate))
		tenc = @elapsed _ipma_encode(args, nframes)
		isfile(output) || error("ffmpeg returned without creating \"$output\"")
	finally
		_progress_close()
		foreach(p -> rm(p; force = true), created)            # the frames were only the film's input
		try isempty(readdir(frame_dir)) && rm(frame_dir) catch end
	end
	_IPMA_MOVIE_TIMES[] = (tgrab, tenc)
	return output
end

const _IPMA_MOVIE_TIMES = Ref((0.0, 0.0))   # the last Save movie: (s grabbing, s encoding), for its report

# SMALLER MP4s, for this dialog only (movie.jl's arguments are Tools > Make movie's too, and stay as they
# are). movie.jl encodes at CRF 18, near lossless, from frames grabbed at the map's full screen size: here
# CRF 26 (x264's rule of thumb: +6 = about half the size), tuned for flat-coloured animation, and the
# frames scaled down to at most _IPMA_MOVIE_W pixels wide (height follows, kept even for yuv420p) before
# movie.jl's own even-size pad. WebM and GIF go through unchanged.
const _IPMA_MOVIE_W = 1280

function _ipma_mp4_small(args::Vector{String})::Vector{String}
	"libx264" in args || return args
	a = copy(args)
	k = findfirst(==("-crf"), a)
	k === nothing || (a[k + 1] = "26")
	k = findfirst(==("-vf"), a)
	k === nothing || (a[k + 1] = "scale='min($(_IPMA_MOVIE_W),iw)':-2," * a[k + 1] * ",setsar=1")   # square pixels: the even-height rounding left SAR 2261:2260
	k = findfirst(==("-preset"), a)
	k === nothing || splice!(a, k + 2:k + 1, ["-tune", "animation"])
	return a
end

# NO DEAD TIME WHILE FFMPEG ENCODES. A plain `run` blocks this thread — the UI thread — until ffmpeg
# exits, so iGMT stopped repainting and looked dead for the whole encode. ffmpeg is started WITHOUT
# waiting, asked to report its progress on stdout (`-progress pipe:1`, a "frame=<n>" line per update),
# and while it runs the window is pumped and the bar moves frame by frame (steps N+1..2N of the bar).
# The arguments are movie.jl's (`_movie_ffmpeg_args`); only the progress report is added in front.
function _ipma_encode(args::Vector{String}, nframes::Int)::Nothing
	cmdv = vcat(args[1:1], ["-progress", "pipe:1", "-nostats"], args[2:end])
	out, err = Pipe(), Pipe()
	proc = run(pipeline(Cmd(cmdv); stdout = out, stderr = err); wait = false)
	close(out.in);  close(err.in)
	done = Ref(0)
	errtxt = Ref("")
	reader = @async for ln in eachline(out)
		startswith(ln, "frame=") && (done[] = something(tryparse(Int, strip(ln[7:end])), done[]))
	end
	ereader = @async (errtxt[] = read(err, String))
	shown = -1
	while process_running(proc)
		if done[] != shown
			shown = done[]
			_progress_status(nframes + min(shown, nframes), "Save movie — encoding frame $shown/$nframes")
		end
		ccall(_fn(:gmtvtk_process_events), Cint, ())   # keep the window alive while ffmpeg works
		sleep(0.05)                                     # yields: the readers above get the pipe output
	end
	wait(proc);  wait(reader);  wait(ereader)
	proc.exitcode == 0 || error("ffmpeg failed (exit $(proc.exitcode)): " * strip(errtxt[]))
	return nothing
end

_ipma_reply(out::Ptr{UInt8}, cap::Cint, txt::String)::Nothing = _sat_reply(out, cap, txt)

# C callback (IpmaRadarDialog). `cparams` is a newline-separated "key=value" block:
#   mode=init                             answer "extent=X0 X1 Y0 Y1" (the radar's Mercator box) and
#                                         "latest=<t>" (IPMA's newest frame; empty + "err=" when unreachable)
#   mode=fetch  start=<t>  end=<t>  have=<t>,<t>,…   walk every step between them (gmtvtk_ipma_add_frame),
#                                         sending only frames not in `have`; answer a report,
#                                         plus "end=<t>" when End was past IPMA's newest frame and was cut there
#   mode=save   frames=<t>,<t>,…  file=<path>   write those frames as one netCDF cube; answer a report
#   mode=bg     which=0|1 provider= variant= box=X0 X1 Y0 Y1 zoom=   web tiles (gmtvtk_ipma_set_bg)
#   mode=bgfile file=<path>               a grid or image as the background (gmtvtk_ipma_set_bg)
#   mode=movie  file=<path.mp4|webm|gif> nframes=N rate=<fps>   film the dialog's map, frame by frame
#   mode=script frame=<t> box=X0 X1 Y0 Y1 bg=tiles|file|none provider= variant= zoom=   answer the GMT.jl script
# Times are "yyyy-mm-dd HH:MM". The answer (or the error text) goes back in `out`. Returns 1/0.
function _on_ipma(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode")
		if mode == "init"
			X0, X1, Y0, Y1 = _ipma_merc_box()
			latest, err = try (_ipma_latest(), "") catch e; ("", sprint(showerror, e)) end
			_ipma_reply(out, cap, "extent=$X0 $X1 $Y0 $Y1\nlatest=$latest\nerr=$err\n")
		elseif mode == "bg"
			b = parse.(Float64, split(_get(d, "box")))
			_ipma_reply(out, cap, _ipma_bg_tiles(dlg, parse(Int, _get(d, "which", "0")), String(_get(d, "provider", "OSM")),
			                                     String(_get(d, "variant")), (b[1], b[2], b[3], b[4]), parse(Int, _get(d, "zoom", "7"))))
		elseif mode == "bgfile"
			_ipma_reply(out, cap, _ipma_bg_file(dlg, String(_get(d, "file"))))
		elseif mode == "script"
			b = parse.(Float64, split(_get(d, "box")))
			_ipma_reply(out, cap, _ipma_script_text(String(_get(d, "frame")), (b[1], b[2], b[3], b[4]), String(_get(d, "bg")),
			                                        String(_get(d, "provider")), String(_get(d, "variant")),
			                                        parse(Int, _get(d, "zoom", "8"))))
		elseif mode == "movie"
			f = _ipma_movie(dlg, String(_get(d, "file")), parse(Int, _get(d, "nframes", "0")),
			                parse(Float64, _get(d, "rate", "3")))
			tg, te = _IPMA_MOVIE_TIMES[]
			_ipma_reply(out, cap, "movie written to $f ($(round(filesize(f) / 2^20; digits = 1)) MB; " *
			                      "frames $(round(tg; digits = 1)) s, encoding $(round(te; digits = 1)) s)")
		elseif mode == "fetch"
			have = Set{String}(String(strip(s)) for s in split(_get(d, "have"), ',') if !isempty(strip(s)))
			got, miss, down, moved = _ipma_fetch(dlg, _ipma_time(_get(d, "start")), _ipma_time(_get(d, "end")), have)
			_ipma_reply(out, cap, "$got frame(s), $down downloaded" * (miss > 0 ? ", $miss not on IPMA's server" : "") *
			                      (isempty(moved) ? "" : ", up to IPMA's newest ($moved)\nend=$moved"))
		elseif mode == "save"
			labels = String[strip(s) for s in split(_get(d, "frames"), ',') if !isempty(strip(s))]
			f = _ipma_save(labels, String(_get(d, "file")))
			_ipma_reply(out, cap, "$(length(labels)) frame(s) written to $f ($(round(filesize(f) / 2^20; digits = 1)) MB)")
		else
			error("unknown request mode=\"$mode\"")
		end
		return Cint(1)
	catch e
		_ipma_reply(out, cap, sprint(showerror, e))
		_tool_failed(scene, "IPMA radar", e)
		return Cint(0)
	end
end

function _register_ipma()
	_IPMA_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_ipma_callback) || return   # a DLL built without the tool
	fptr = @cfunction((s, w, c, o, n) -> Base.invokelatest(_on_ipma, s, w, c, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_ipma_callback), Cvoid, (Ptr{Cvoid},), fptr)
	_IPMA_WIRED[] = true
	return
end
warm_register("ipma", _register_ipma)
