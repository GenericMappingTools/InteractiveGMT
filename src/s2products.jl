# s2products.jl — Satellite > Copernicus > "Sentinel-2 MSI products…": the Sentinel-2 optical products
# (the .SAFE archives ESA distributes, Level-2A surface reflectance and Level-1C top of atmosphere)
# found in the Copernicus Data Space catalogue over a region, a period and a cloud-cover limit, each with
# its quicklook, then downloaded WHOLE (the .zip) or band by band, the bands cut to the region into one
# band cube shown in the window. One more tool of the Remote sensing dialog family (RemoteSDialog,
# 70_window.cpp, over deps/ui/remotes_s2.ui).
#
# It is the Sentinel-1 products tool's twin and uses ITS machinery as it is (s1products.jl): the same
# catalogue entry (`S1Product`, `_s1_parse`, `_s1_rings`, `_s1_attr`), the same CDSE login and token
# (`_s1_token`, the Account tab's p_cd_* boxes), the same whole-product download (`_s1_fetch`), the same
# region checks (`_s1_box`, `_s1_window_box`) and point pick (`_s1_covering`). What is new is only what
# Sentinel-2 has and Sentinel-1 has not. All checked live 2026-11-24:
#   * the cloud filter: the DoubleAttribute "cloudCover" (percent of the tile);
#   * the quicklook: every product carries an asset of Type QUICKLOOK, a 343x343 JPEG served by the
#     catalogue with NO login (`$expand=Assets`, repeated beside `$expand=Attributes`: a comma-joined
#     expand is a 400);
#   * the bands: the product's file tree is the OData `Nodes` of download.dataspace…, LISTED without a
#     login (each node with its ContentLength), DOWNLOADED (`…/Nodes(file)/$value`) with the CDSE token.
#     L2A: GRANULE/<granule>/IMG_DATA/R10m|R20m|R60m/T29SPA_<time>_B04_10m.jp2 (each folder has the
#     bands at that spacing, resampled where coarser than the band's own); L1C: GRANULE/<granule>/
#     IMG_DATA/T29SPA_<time>_B04.jp2, every band at its own spacing only.
# The catalogue host answers a Nodes request with a 301 to download…, which would lose the
# Authorization header on the way, so download… is asked directly (the same reason as s1products.jl).

# The Product combo: (text, the piece of the name that selects it).
const _S2_LEVELS = Tuple{String,String}[
	("L2A: surface reflectance (bottom of atmosphere)", "MSIL2A"),
	("L1C: top-of-atmosphere reflectance",              "MSIL1C"),
]
const _S2_RES = [10, 20, 60]                      # the Resolution combo, metres

# Every band file a product can hold: (key in the file name, description). The check box of a key is
# p_b_<lowercase key> in the .ui.
const _S2_BANDS = Tuple{String,String}[
	("B01", "B01 coastal aerosol 443 nm"), ("B02", "B02 blue 490 nm"), ("B03", "B03 green 560 nm"),
	("B04", "B04 red 665 nm"), ("B05", "B05 red edge 705 nm"), ("B06", "B06 red edge 740 nm"),
	("B07", "B07 red edge 783 nm"), ("B08", "B08 near infrared 842 nm"), ("B8A", "B8A narrow near infrared 865 nm"),
	("B09", "B09 water vapour 945 nm"), ("B10", "B10 SWIR cirrus 1375 nm"), ("B11", "B11 SWIR 1610 nm"),
	("B12", "B12 SWIR 2190 nm"), ("SCL", "SCL scene classification"), ("AOT", "AOT aerosol optical thickness"),
	("WVP", "WVP water vapour"), ("TCI", "TCI true colour"),
]
# L1C keeps every band at its own spacing only (ESA's MSI table).
const _S2_L1C_RES = Dict("B01" => 60, "B02" => 10, "B03" => 10, "B04" => 10, "B05" => 20, "B06" => 20,
                         "B07" => 20, "B08" => 10, "B8A" => 20, "B09" => 60, "B10" => 60, "B11" => 20,
                         "B12" => 20, "TCI" => 10)

struct S2Scene
	p::S1Product          # the catalogue entry: id, name, time, size, footprint (s1products.jl)
	cloud::Float64        # cloudCover, percent (NaN when the catalogue says nothing)
	ql::String            # the quicklook's address ("" when the product has none)
	tile::String          # MGRS tile, "29SPA"
	orbit::Int            # relative orbit, from the name's R037
end

const _S2_FOUND  = Dict{Ptr{Cvoid},Vector{S2Scene}}()             # window -> its last search
const _S2_FRAMES = Dict{Ptr{Cvoid},Dict{String,Vector{Int}}}()    # window -> footprint label -> scenes
const _S2_GROUP  = Dict{Ptr{Cvoid},String}()                      # window -> group tag of its footprints
const _S2_PENS   = Dict{Ptr{Cvoid},Dict{String,Vector{Float64}}}() # window -> footprint label -> its own pen
const _S2_LIT    = Dict{Ptr{Cvoid},String}()                      # window -> the highlighted footprint
const _S2_SEARCHED = Dict{Ptr{Cvoid},NTuple{4,Float64}}()         # window -> the region its list was found for
# product id -> (granule folder, every band file: (IMG_DATA sub-folder, file name, bytes)). The tree of a
# product never changes, so it is listed once per session.
const _S2_NODES  = Dict{String,Tuple{String,Vector{Tuple{String,String,Float64}}}}()

_s2_dir()::String = get(ENV, "IGMT_S2_DIR", joinpath(homedir(), "Downloads", "Sentinel-2"))
_s2_base(s::S2Scene)::String = replace(s.p.name, r"\.SAFE$" => "")
_s2_isl1c(s::S2Scene)::Bool = occursin("_MSIL1C_", s.p.name)
_s2_sat(s::S2Scene)::String = first(s.p.name, 3)
_s2_mb(b::Float64)::String = b >= 2^30 ? "$(round(b / 2^30, digits = 1)) GB" :
                             b >= 2^20 ? "$(round(b / 2^20, digits = 1)) MB" : "$(round(Int, b / 2^10)) kB"

# The band key of a band file name: "T29SPA_20261114T112321_B04_10m.jp2" -> "B04" ("" for anything else).
function _s2_key(file::String)::String
	m = match(r"_(B\d{2}|B8A|SCL|AOT|WVP|TCI)(?:_\d+m)?\.jp2$", file)
	return m === nothing ? "" : String(m[1])
end

# --- the search ------------------------------------------------------------------------------------

function _s2_filter(lev::String, W::Float64, E::Float64, S::Float64, N::Float64,
                    d0::GMT.Dates.Date, d1::GMT.Dates.Date, cloud::Int)::String
	f = ["Collection/Name eq 'SENTINEL-2'",
	     "OData.CSC.Intersects(area=geography'SRID=4326;POLYGON(($W $S,$E $S,$E $N,$W $N,$W $S))')",
	     "ContentDate/Start ge $(d0)T00:00:00.000Z",
	     "ContentDate/Start le $(d1)T23:59:59.999Z",
	     "contains(Name,'_$(lev)_')"]
	cloud < 100 && push!(f, "Attributes/OData.CSC.DoubleAttribute/any(att:att/Name eq 'cloudCover' and " *
	                        "att/OData.CSC.DoubleAttribute/Value le $(cloud))")
	return join(f, " and ")
end

function _s2_parse(o)::Union{S2Scene,Nothing}
	p = _s1_parse(o)
	p === nothing && return nothing
	cc = _s1_attr(o, "cloudCover")
	ql = ""
	a = get(o, "Assets", nothing)
	if isa(a, Vector)
		for x in a
			isa(x, Dict) && _shub_str(x, "Type") == "QUICKLOOK" && (ql = _shub_str(x, "DownloadLink"); break)
		end
	end
	mt = match(r"_T(\d{2}[A-Z]{3})_", p.name)
	mo = match(r"_R(\d{3})_", p.name)
	return S2Scene(p, isa(cc, Real) ? Float64(cc) : NaN, ql, mt === nothing ? "" : String(mt[1]),
	               mo === nothing ? 0 : parse(Int, mo[1]))
end

# Every product over the box in the period under the cloud limit, newest first, and how many the
# catalogue counted.
function _s2_search(lev::String, W::Float64, E::Float64, S::Float64, N::Float64,
                    d0::GMT.Dates.Date, d1::GMT.Dates.Date, cloud::Int)::Tuple{Vector{S2Scene},Int}
	url = _shub_url(_S1_ODATA, ["\$filter" => _s2_filter(lev, W, E, S, N, d0, d1, cloud),
	                            "\$top" => string(_S1_TOP), "\$orderby" => "ContentDate/Start desc",
	                            "\$expand" => "Attributes", "\$expand" => "Assets", "\$count" => "True"])
	js = _shub_json(_shub_http_get(url; timeout = 180))
	isa(js, Dict) || error("The Copernicus catalogue did not answer with a product list.")
	v = get(js, "value", Any[])
	found = S2Scene[]
	for o in (isa(v, Vector) ? v : Any[])
		s = _s2_parse(o)
		s === nothing || push!(found, s)
	end
	n = get(js, "@odata.count", length(found))
	return found, isa(n, Real) ? round(Int, n) : length(found)
end

# --- the quicklook ---------------------------------------------------------------------------------
# Fetched when a scene is chosen (16 kB, public), kept in <folder>/quicklooks under the product's name:
# choosing it again, or reopening the dialog, reads it from there.
function _s2_quicklook(s::S2Scene, dir::String)::String
	isempty(s.ql) && return ""
	f = joinpath(dir, "quicklooks", _s2_base(s) * ".jpg")
	isfile(f) && filesize(f) > 0 && return f
	mkpath(dirname(f))
	part = f * ".part"
	try
		r = open(part, "w") do io
			Downloads.request(s.ql; output = io, timeout = 60, throw = false)
		end
		isa(r, Downloads.Response) && 200 <= r.status < 300 || return ""
		open(io -> read(io, 2), part) == UInt8[0xff, 0xd8] || return ""     # a JPEG, not an error page
		mv(part, f; force = true)
	catch
		return ""
	finally
		isfile(part) && rm(part; force = true)
	end
	return f
end

# The quicklook pane: the picture (rich text <img>, which a QLabel loads from the file itself) and the
# scene's line under it.
function _s2_show_thumb!(io::IOBuffer, s::Union{S2Scene,Nothing}, dir::String)
	if s === nothing
		_rs_label(io, "lbl_thumb", "No scene chosen.")
		_rs_label(io, "lbl_thumbinfo", "")
		return nothing
	end
	f = _s2_quicklook(s, dir)
	src = replace(f, '\\' => '/')
	_rs_label(io, "lbl_thumb", isempty(f) ? "The catalogue has no quicklook for this scene." :
	                           "<img src=\"file:///$(src)\" width=\"300\" height=\"300\">")
	cc = isnan(s.cloud) ? "cloud ?" : "cloud $(round(s.cloud, digits = 1)) %"
	_rs_label(io, "lbl_thumbinfo", "$(GMT.Dates.format(s.p.t0, "yyyy-mm-dd HH:MM"))  $(_s2_sat(s))  tile $(s.tile)  " *
	                               "orbit R$(lpad(s.orbit, 3, '0'))  $(cc)  $(_s2_mb(s.p.bytes))")
	return nothing
end

# --- the product's band files ----------------------------------------------------------------------

_s2_node_url(s::S2Scene, parts::Vector{String})::String =
	"$(_S1_DOWNLOAD)($(s.p.id))/Nodes($(s.p.name))" * join("/Nodes($(x))" for x in parts)

function _s2_node_list(url::String)::Vector{Tuple{String,Float64}}
	js = _shub_json(_shub_http_get(url * "/Nodes"; timeout = 60))
	r = isa(js, Dict) ? get(js, "result", Any[]) : Any[]
	out = Tuple{String,Float64}[]
	for x in (isa(r, Vector) ? r : Any[])
		isa(x, Dict) || continue
		nm = _shub_str(x, "Name");  b = get(x, "ContentLength", 0.0)
		isempty(nm) || push!(out, (nm, isa(b, Real) ? Float64(b) : 0.0))
	end
	return out
end

function _s2_nodes(s::S2Scene)::Tuple{String,Vector{Tuple{String,String,Float64}}}
	haskey(_S2_NODES, s.p.id) && return _S2_NODES[s.p.id]
	g = _s2_node_list(_s2_node_url(s, ["GRANULE"]))
	isempty(g) && error("The Copernicus Data Space lists no granule inside $(s.p.name).")
	gran = g[1][1]
	files = Tuple{String,String,Float64}[]
	if _s2_isl1c(s)
		for (nm, b) in _s2_node_list(_s2_node_url(s, ["GRANULE", gran, "IMG_DATA"]))
			isempty(_s2_key(nm)) || push!(files, ("", nm, b))
		end
	else
		for r in _S2_RES
			sub = "R$(r)m"
			for (nm, b) in _s2_node_list(_s2_node_url(s, ["GRANULE", gran, "IMG_DATA", sub]))
				isempty(_s2_key(nm)) || push!(files, (sub, nm, b))
			end
		end
	end
	isempty(files) && error("The Copernicus Data Space lists no band file inside $(s.p.name).")
	return _S2_NODES[s.p.id] = (gran, files)
end

# The band files of the scene at spacing `res`: key -> (sub-folder, file, bytes). L2A: its R<res>m folder;
# L1C: the bands whose own spacing is `res`.
function _s2_files_at(s::S2Scene, res::Int)::Dict{String,Tuple{String,String,Float64}}
	_, files = _s2_nodes(s)
	out = Dict{String,Tuple{String,String,Float64}}()
	for (sub, nm, b) in files
		k = _s2_key(nm)
		ok = _s2_isl1c(s) ? get(_S2_L1C_RES, k, 0) == res : sub == "R$(res)m"
		ok && (out[k] = (sub, nm, b))
	end
	return out
end

_s2_res(d::Dict{String,String})::Int = _S2_RES[clamp(_rs_idx(d, "p_bres") + 1, 1, length(_S2_RES))]
_s2_band_dir(s::S2Scene, dir::String)::String = joinpath(dir, _s2_base(s))

# The ticked bands the scene has at that spacing, in the panel's order.
function _s2_ticked(d::Dict{String,String}, have::Dict{String,Tuple{String,String,Float64}})::Vector{String}
	return [k for (k, _) in _S2_BANDS if haskey(have, k) && _rs_on(d, "p_b_" * lowercase(k))]
end

# The band panel follows the chosen scene and spacing: a box is enabled only for a band the product
# really has there, and the line beside the spacing says what the ticked ones cost.
function _s2_fill_bands!(io::IOBuffer, d::Dict{String,String}, s::Union{S2Scene,Nothing}, dir::String)
	have = s === nothing ? Dict{String,Tuple{String,String,Float64}}() : _s2_files_at(s, _s2_res(d))
	for (k, _) in _S2_BANDS
		_rs_enable(io, "p_b_" * lowercase(k), haskey(have, k))
	end
	_rs_enable(io, "run_bands", !isempty(have))
	if s === nothing
		_rs_label(io, "lbl_bsize", "")
		return nothing
	end
	ks = _s2_ticked(d, have)
	res = _s2_res(d)
	if isempty(ks)
		_rs_label(io, "lbl_bsize", "no band ticked at $(res) m")
		return nothing
	end
	box = _ls_box(d)
	# A found scene always reaches the region it was found for; one that does not means the region was
	# changed after the search (the refresh then clears the list, "Region changed: Find the scenes again").
	# Never an error thrown out of a panel refresh, never a button that would fetch nothing.
	if box !== nothing && (try _s2_window(s, box, res); false catch; true end)
		_rs_enable(io, "run_bands", false)
		_rs_label(io, "lbl_bsize", "Region changed: Find the scenes again")
		return nothing
	end
	ondisk = all(((_, o),) -> isfile(o), _s2_outs(s, ks, res, dir, box))
	if box === nothing
		n = round(Int, 109800 / res)                   # the tile, in cells of the chosen spacing
		txt = ondisk ? "on disk" : "whole tile, $(n)x$(n) px"
	else
		_, _, _, _, nx, ny = _s2_window(s, box, res)
		txt = ondisk ? "on disk" : "region only, $(nx)x$(ny) px"
	end
	_rs_label(io, "lbl_bsize", "$(length(ks)) at $(res) m: $(txt)")
	return nothing
end

# The files one "Get the bands" makes, (true-colour?, path): ONE per request, the true-colour picture
# (3 bands of 8 bits, an image of its own, never a cube layer) and the cube of the other ticked bands,
# named for the region when only the region is asked.
function _s2_outs(s::S2Scene, ks::Vector{String}, res::Int, dir::String,
                  box::Union{Nothing,NTuple{4,Float64}})::Vector{Tuple{Bool,String}}
	day = GMT.Dates.format(s.p.t0, "yyyy-mm-dd")
	lvl = _s2_isl1c(s) ? "L1C" : "L2A"
	bd = _s2_band_dir(s, dir)
	# Named for the window REALLY cut (the region cut to this scene, in the tile's metres), never for the
	# region as typed: two regions that reach the same part of the scene are the same file.
	tag = box === nothing ? "" :
	      (wb = _s2_window(s, box, res); "_cut_" * join((string(round(Int, v)) for v in wb[1:4]), "_"))
	cks = [k for k in ks if k != "TCI"]
	outs = Tuple{Bool,String}[]
	"TCI" in ks && push!(outs, (true, joinpath(bd, "$(s.tile)_$(day)_TCI_$(res)m$(tag).tiff")))
	isempty(cks) || push!(outs, (false, joinpath(bd, "$(s.tile)_$(day)_$(lvl)_$(res)m_$(join(cks, '-'))$(tag).tiff")))
	return outs
end

# One band file to `f`, through a ".part" file checked to be a JPEG 2000 before it takes the name (a
# refused token sends JSON). `prog(now)` follows the bytes.
const _S2_JP2_MAGIC = UInt8[0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20]

function _s2_fetch_node(url::String, f::String, tok::String, prog::Function)::Nothing
	mkpath(dirname(f))
	part = f * ".part"
	try
		r = open(part, "w") do io
			Downloads.request(url; output = io, throw = false, headers = ["Authorization" => "Bearer " * tok],
			                  progress = (total, now, _...) -> prog(now))
		end
		isa(r, Downloads.Response) || throw(r)
		if !(200 <= r.status < 300)
			txt = try first(read(part, String), 400) catch; "" end
			error("The Copernicus Data Space answered $(r.status) for $(basename(f))\n\n$(txt)")
		end
		open(io -> read(io, 8), part) == _S2_JP2_MAGIC ||
			error("The Copernicus Data Space did not send a JPEG 2000 file for $(basename(f)): $(first(read(part, String), 300))")
		mv(part, f; force = true)
	finally
		isfile(part) && rm(part; force = true)
	end
	return nothing
end

# The ticked bands of `s` at `res`, downloaded (a file already there is not downloaded again), as
# (key, path) in the panel's order. One progress line for them all.
function _s2_fetch_bands(s::S2Scene, ks::Vector{String}, res::Int, dir::String,
                         d::Dict{String,String})::Vector{Tuple{String,String}}
	have = _s2_files_at(s, res)
	gran, _ = _s2_nodes(s)
	bd = _s2_band_dir(s, dir)
	out = [(k, joinpath(bd, have[k][2])) for k in ks]
	todo = [(k, f) for (k, f) in out if !(isfile(f) && filesize(f) > 0)]
	isempty(todo) && return out
	tok = _s1_token(d)
	total = sum(have[k][3] for (k, _) in todo)
	done = Ref(0.0);  tl = Ref(0.0)
	_rs_prog_show("Downloading $(length(todo)) band(s) of $(_s2_base(s))…")
	try
		for (k, f) in todo
			sub, nm, b = have[k]
			parts = isempty(sub) ? ["GRANULE", gran, "IMG_DATA", nm] : ["GRANULE", gran, "IMG_DATA", sub, nm]
			prog(now) = (time() - tl[] > 0.25 && total > 0) &&
				(tl[] = time(); _rs_prog(round(Int, 100 * (done[] + now) / total),
				                         "Downloading $(k): $(_s2_mb(done[] + now)) of $(_s2_mb(total))"))
			_s2_fetch_node(_s2_node_url(s, parts) * "/\$value", f, tok, prog)
			done[] += b
		end
	finally
		_rs_prog_close()
	end
	return out
end

# The region in the tile's own system, cut to the tile: `nothing` = the whole tile.
function _s2_cut_box(f::String, box::Union{Nothing,NTuple{4,Float64}})::Union{Nothing,Vector{Float64}}
	box === nothing && return nothing
	b, srs = _rs_file_box(f)
	isempty(srs) && error("$(basename(f)) carries no coordinate system: it cannot be cut to a lon/lat region.")
	w, e, s, n = _box_to(_LONLAT, srs, box...)
	w, e, s, n = max(w, b[1]), min(e, b[2]), max(s, b[3]), min(n, b[4])
	(w < e && s < n) || error("The region does not reach $(basename(f)).")
	return [w, e, s, n]
end

# --- the region only: Copernicus' own window service ---------------------------------------------
# "Cut to the region" never fetches a band file. JPEG 2000 itself CAN be read by window over the net (ESA's
# band files are tiled; a JP2 reader fetches only the tiles a window needs), but GMT's GDAL has no JP2 driver
# iGMT may use: only JP2ECW, which must never read a .jp2, and no JP2OpenJPEG. The Copernicus Data Space's Sentinel Hub Process API (sh.dataspace…)
# cuts the window on the server and sends ONLY it, as a GeoTIFF, on the tile's own UTM grid at the chosen
# spacing, with the same CDSE login token as the band downloads (checked live 2026-11-24: 1.8 s for one
# 10 m band over 9x11 km). The bands come as ESA's raw digital numbers (`units:"DN"`,
# `harmonizeValues:false`: the processing-baseline offset is kept, as in ESA's files); 0 = no data.
# Checked 2026-11-25: a 907x1127 B04 window of tile 29SPB (2026-11-23) is IDENTICAL, all 1 022 189 pixels,
# to the same window decoded from ESA's own band JP2 with OpenJPEG (opj_decompress -d).
# One request answers at most 2500x2500 pixels, so a larger region is asked in pieces, stitched by a VRT
# that GDAL writes into ONE GeoTIFF (the band descriptions with it); the pieces are removed.
const _S2_SH_MAX = 2500
# When a block is asked again. Measured: a block's first byte after ~9 s (Copernicus computing), the
# whole block in 11-20 s with 4 at once. Seen live 2026-11-25: one request of 9 got NO byte at all while
# the same block asked alone answered in 4.3 s -- it stalls on our side, and only asking again gets it.
const _S2_FIRST_BYTE = 25.0                      # s without a first byte
const _S2_STALL      = 15.0                      # s without a new byte, once bytes flow

# The tile's UTM system: "29SNB" -> EPSG:32629 (latitude band N..X = north).
function _s2_epsg(s::S2Scene)::Int
	m = match(r"^(\d{2})([C-X])", s.tile)
	m === nothing && error("Cannot tell the UTM zone of tile \"$(s.tile)\".")
	return (m[2][1] >= 'N' ? 32600 : 32700) + parse(Int, m[1])
end

# The lon/lat region in the tile's system, CUT TO THE CHOSEN SCENE -- its tile and its own data footprint
# (a pass can cover only part of a tile) -- then widened to whole cells of `res`: (w, e, s, n, nx, ny).
# Never the region as typed: the window service mosaics every tile of the pass inside the box it is asked
# for, so an uncut box came back as the whole region, filled from tiles that are not the scene.
function _s2_window(s::S2Scene, box::NTuple{4,Float64}, res::Int)::Tuple{Float64,Float64,Float64,Float64,Int,Int}
	srs = "EPSG:$(_s2_epsg(s))"
	w, e, so, n = _box_to(_LONLAT, srs, box...)
	_, tx0, tx1, ty0, ty1 = _s2_tile_box(s.tile)
	w, e, so, n = max(w, tx0), min(e, tx1), max(so, ty0), min(n, ty1)
	if !isempty(s.p.rings)                        # the footprint's box, in the tile's system
		F = _srs_xy(reduce(vcat, (M[:, 1:2] for M in s.p.rings)), _LONLAT, srs)
		w, e = max(w, minimum(F[:, 1])), min(e, maximum(F[:, 1]))
		so, n = max(so, minimum(F[:, 2])), min(n, maximum(F[:, 2]))
	end
	(w < e && so < n) || error("The region does not reach this scene ($(s.tile), $(GMT.Dates.format(s.p.t0, "yyyy-mm-dd HH:MM"))): " *
	                           "choose a scene whose footprint covers part of it.")
	r = Float64(res)
	w, e, so, n = floor(w / r) * r, ceil(e / r) * r, floor(so / r) * r, ceil(n / r) * r
	return w, e, so, n, round(Int, (e - w) / r), round(Int, (n - so) / r)
end

# The evalscript: the bands as raw DN (UInt16); or the true-colour picture coded as ESA codes its TCI
# (reflectance 0..0.2 -> 1..255, 0 = no data, UInt8).
function _s2_evalscript(ks::Vector{String}, tci::Bool)::String
	if tci
		return "//VERSION=3\nfunction setup(){return {input:[{bands:[\"B04\",\"B03\",\"B02\",\"dataMask\"],units:\"DN\"}]," *
		       "output:{bands:3,sampleType:\"UINT8\"}};}\n" *
		       "function t(v,m){return m==0?0:Math.min(255,Math.max(1,Math.round(1+v*254/2000)));}\n" *
		       "function evaluatePixel(p){return [t(p.B04,p.dataMask),t(p.B03,p.dataMask),t(p.B02,p.dataMask)];}"
	end
	q = join(("\"$(k)\"" for k in ks), ",")
	return "//VERSION=3\nfunction setup(){return {input:[{bands:[$(q)],units:\"DN\"}]," *
	       "output:{bands:$(length(ks)),sampleType:\"UINT16\"}};}\n" *
	       "function evaluatePixel(p){return [$(join(("p.$(k)" for k in ks), ","))];}"
end

function _s2_sh_body(s::S2Scene, ks::Vector{String}, tci::Bool, w::Float64, so::Float64, e::Float64, n::Float64,
                     nx::Int, ny::Int)::String
	D = GMT.Dates
	iso(t) = D.format(t, "yyyy-mm-ddTHH:MM:SS") * "Z"
	coll = _s2_isl1c(s) ? "sentinel-2-l1c" : "sentinel-2-l2a"
	# The scene's own pass: its sensing start, give or take a few minutes (one satellite, one datatake).
	tr = "{\"from\":$(_ls_jstr(iso(s.p.t0 - D.Minute(5)))),\"to\":$(_ls_jstr(iso(s.p.t0 + D.Minute(10))))}"
	return "{\"input\":{\"bounds\":{\"bbox\":[$(w),$(so),$(e),$(n)],\"properties\":{\"crs\":" *
	       "\"http://www.opengis.net/def/crs/EPSG/0/$(_s2_epsg(s))\"}}," *
	       "\"data\":[{\"type\":\"$(coll)\",\"dataFilter\":{\"timeRange\":$(tr)}," *
	       "\"processing\":{\"harmonizeValues\":$(tci ? "true" : "false")}}]}," *
	       "\"output\":{\"width\":$(nx),\"height\":$(ny),\"responses\":[{\"identifier\":\"default\"," *
	       "\"format\":{\"type\":\"image/tiff\"}}]},\"evalscript\":$(replace(_ls_jstr(_s2_evalscript(ks, tci)), '\n' => "\\n"))}"
end

_s2_xml(t::String)::String = replace(t, '&' => "&amp;", '<' => "&lt;", '>' => "&gt;", '"' => "&quot;")

# The window of the ticked bands (or the true-colour picture) over the lon/lat `box`, written to `out`,
# ONE GeoTIFF.
function _s2_fetch_window(s::S2Scene, ks::Vector{String}, tci::Bool, res::Int, box::NTuple{4,Float64},
                          out::String, desc::Vector{String}, d::Dict{String,String}; scene::Ptr{Cvoid}=C_NULL)::Nothing
	w, e, so, n, _, _ = _s2_window(s, box, res)
	return _s2_fetch_utm(s, ks, tci, res, w, e, so, n, out, desc, d; scene = scene)
end

# The WHOLE tile the same way (the window service, in pieces): iGMT never reads ESA's JPEG 2000 band
# files -- GMT's GDAL has only the ECW driver for them, which must never be used to read a JP2.
function _s2_fetch_tile(s::S2Scene, ks::Vector{String}, tci::Bool, res::Int, out::String,
                        desc::Vector{String}, d::Dict{String,String}; scene::Ptr{Cvoid}=C_NULL)::Nothing
	_, x0, x1, y0, y1 = _s2_tile_box(s.tile)
	return _s2_fetch_utm(s, ks, tci, res, x0, x1, y0, y1, out, desc, d; scene = scene)
end

# The window w, e, s, n (the tile's UTM system, whole cells of `res`) of the ticked bands, ONE GeoTIFF.
function _s2_fetch_utm(s::S2Scene, ks::Vector{String}, tci::Bool, res::Int, w::Float64, e::Float64, so::Float64,
                       n::Float64, out::String, desc::Vector{String}, d::Dict{String,String};
                       scene::Ptr{Cvoid}=C_NULL)::Nothing
	r = Float64(res)
	nx = round(Int, (e - w) / r);  ny = round(Int, (n - so) / r)
	(nx > 0 && ny > 0) || error("The region is empty.")
	tok = _s1_token(d)
	url = _shub_process_url(_SHUB_CDSE)
	mkpath(dirname(out))
	pieces = Tuple{String,Int,Int,Int,Int}[]          # (file, x offset, y offset, columns, rows)
	for j0 in 0:_S2_SH_MAX:ny-1, i0 in 0:_S2_SH_MAX:nx-1
		push!(pieces, (out * ".piece$(length(pieces) + 1).tif", i0, j0, min(_S2_SH_MAX, nx - i0), min(_S2_SH_MAX, ny - j0)))
	end
	vrt = out * ".vrt"
	nb = tci ? 3 : length(ks)
	what = tci ? "the true-colour picture" : "$(length(ks)) band(s)"
	_rs_prog_show("Asking Copernicus for $(what) over the region…")
	try
		# A BLOCK = one request of at most 2500x2500 pixels, the most the service answers at once; a larger
		# window is several. FOUR asked at a time (measured 2026-11-25: 4 full 4-band blocks in 20 s at
		# once, ~55 s one after the other; ~9 s of each is Copernicus computing, with no traffic at all).
		# The download tasks do network work ONLY; the progress dialog is told from THIS task, the dialog's
		# own, never from inside a download task.
		# The login token is asked for PER BLOCK: a CDSE token lives ~10 minutes, and a long job that took
		# one at the start had its later blocks refused (HTTP 401). _s1_token hands back the cached one
		# while it is still good and renews it when it is not.
		nextk = Ref(0);  done = Ref(0);  err = Ref{Any}(nothing)
		# ONE block, with a watchdog: a block answers in 11-20 s (measured), but Copernicus can leave a
		# request hanging -- the bar froze at 67 % for minutes, the request's own limit being 600 s. Each
		# try writes its own file; one not answered in _S2_BLOCK_WAIT s is ABANDONED (it ends by itself,
		# unseen) and the block asked again, at most 3 tries.
		function fetch_block(k::Int)
			f, i0, j0, cx, cy = pieces[k]
			pw = w + i0 * r;  pn = n - j0 * r
			body = _s2_sh_body(s, ks, tci, pw, pn - cy * r, pw + cx * r, pn, cx, cy)
			last = nothing
			for attempt in 1:3
				ft = "$(f).try$(attempt)"
				tk = @async try
					_shub_process_to_file(url, body, _s1_token(d), ft);  nothing
				catch e
					e
				end
				# A stalled request is told by its FILE: no first byte in _S2_FIRST_BYTE s (a healthy block's
				# first byte comes after ~9 s of Copernicus computing), or no growth for _S2_STALL s once
				# bytes flow. Then it is re-asked at once, never waited out.
				t1 = time();  sz = 0;  tgrow = t1
				while !istaskdone(tk) && err[] === nothing
					sleep(0.25)
					s2 = isfile(ft) ? filesize(ft) : 0
					s2 > sz && (sz = s2; tgrow = time())
					(sz == 0 ? time() - t1 > _S2_FIRST_BYTE : time() - tgrow > _S2_STALL) && break
				end
				err[] === nothing || return
				if !istaskdone(tk)
					last = ErrorException(sz == 0 ? "no answer in $(_S2_FIRST_BYTE) s" : "the transfer stopped for $(_S2_STALL) s")
					continue
				end
				last = fetch(tk)
				if last === nothing
					open(io -> read(io, 4), ft) in (UInt8[0x49, 0x49, 0x2a, 0x00], UInt8[0x4d, 0x4d, 0x00, 0x2a]) ||
						error("Copernicus did not send a GeoTIFF for $(what):\n\n$(first(read(ft, String), 400))")
					mv(ft, f; force = true)
					return
				end
			end
			error("Copernicus did not deliver block $(k) of $(length(pieces)) of $(what) in 3 tries: " *
			      sprint(showerror, last))
		end
		function worker()
			while err[] === nothing
				k = (nextk[] += 1)
				k > length(pieces) && return
				try
					fetch_block(k)
					err[] === nothing && (done[] += 1)
				catch e
					err[] === nothing && (err[] = e)
				end
			end
		end
		tasks = [@async(worker()) for _ in 1:min(4, length(pieces))]
		t0 = time()
		# The window can be closed while this waits: then STOP -- no new block is asked for, and the
		# progress dialog is never touched again (each update re-shows it: it kept popping up after
		# iGMT's window was gone). Blocks already in flight end on their own, unseen.
		alive() = scene == C_NULL || ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), scene) == 1
		while !all(istaskdone, tasks)
			if !alive()
				err[] === nothing && (err[] = ErrorException("The window was closed: the download of $(what) was stopped."))
				break
			end
			_rs_prog(round(Int, 100 * done[] / length(pieces)), "$(what), $(nx)x$(ny) pixels: " *
			         "$(done[]) of $(length(pieces)) block(s) received, $(round(Int, time() - t0)) s " *
			         "(each block up to $(_S2_SH_MAX)x$(_S2_SH_MAX) pixels; Copernicus computes it before sending)")
			sleep(0.5)
		end
		err[] === nothing || throw(err[])
		_rs_prog(100, "Writing $(basename(out))…")
		x = IOBuffer()
		print(x, "<VRTDataset rasterXSize=\"$(nx)\" rasterYSize=\"$(ny)\">\n<SRS>EPSG:$(_s2_epsg(s))</SRS>\n",
		      "<GeoTransform>$(w), $(r), 0, $(n), 0, $(-r)</GeoTransform>\n")
		for b in 1:nb
			print(x, "<VRTRasterBand dataType=\"$(tci ? "Byte" : "UInt16")\" band=\"$(b)\">\n",
			      "<Description>$(_s2_xml(b <= length(desc) ? desc[b] : ""))</Description><NoDataValue>0</NoDataValue>\n")
			tci && print(x, "<ColorInterp>$(("Red", "Green", "Blue")[b])</ColorInterp>\n")
			for (f, i0, j0, cx, cy) in pieces
				print(x, "<SimpleSource><SourceFilename relativeToVRT=\"0\">$(_s2_xml(f))</SourceFilename>",
				      "<SourceBand>$(b)</SourceBand><SrcRect xOff=\"0\" yOff=\"0\" xSize=\"$(cx)\" ySize=\"$(cy)\"/>",
				      "<DstRect xOff=\"$(i0)\" yOff=\"$(j0)\" xSize=\"$(cx)\" ySize=\"$(cy)\"/></SimpleSource>\n")
			end
			print(x, "</VRTRasterBand>\n")
		end
		print(x, "</VRTDataset>\n")
		write(vrt, take!(x))
		isfile(out) && rm(out; force = true)
		GMT.gdaltranslate(vrt, ["-co", "COMPRESS=DEFLATE"]; dest = out)
		isfile(out) || error("GDAL wrote nothing to $(out)")
	finally
		_rs_prog_close()
		# every block file and every try of one; a try still in flight (abandoned, window closed) holds its file
		for p in pieces, g in (p[1], ("$(p[1]).try$(a)" for a in 1:3)...)
			try; isfile(g) && rm(g; force = true); catch; end
		end
		try; isfile(vrt) && rm(vrt; force = true); catch; end
	end
	return nothing
end

# --- the footprints --------------------------------------------------------------------------------
# ONE line element per SCENE, its own footprint as the catalogue gives it (never one scene's outline
# standing for others: two passes over a tile can cover different parts of it). All under one group
# named for the search; each is a guide line of this dialog, whose menu item "Choose this scene
# (Sentinel-2)" chooses that scene. A new search replaces the previous ones.
function _s2_plot_footprints!(scene::Ptr{Cvoid}, found::Vector{S2Scene},
                              W::Float64, E::Float64, S::Float64, N::Float64)::Int
	_s2_remove_stack!(scene)                      # the quicklook stack belonged to the previous search
	old = pop!(_S2_GROUP, scene, "")
	isempty(old) || ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, old)
	delete!(_S2_FRAMES, scene);  delete!(_S2_PENS, scene);  delete!(_S2_LIT, scene)
	ccall(_fn(:gmtvtk_set_click_tool_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "s2", "")
	isempty(found) && return 0
	# EVERY scene draws ITS OWN footprint, exactly as the catalogue gives it: two passes of the same orbit
	# over the same tile do not cover the same part of it (one datatake can end inside the tile).
	frames = [(k, s) for (k, s) in enumerate(found) if !isempty(s.p.rings)]
	isempty(frames) && return 0
	_swot_map_if_empty(scene, W, E, S, N, "Sentinel-2 scenes")
	t0 = minimum(s.p.t0 for s in found);  t1 = maximum(s.p.t0 for s in found)
	nm = "Sentinel-2 scenes $(GMT.Dates.format(t0, "yyyy-mm-dd")) to $(GMT.Dates.format(t1, "yyyy-mm-dd"))"
	labels = Dict{String,Vector{Int}}()
	pens = Dict{String,Vector{Float64}}()
	for (k, s) in frames
		lab = _s2_base(s)                          # the scene's own name: one footprint, one scene
		ks = [k]
		D = Vector{GMTdataset{Float64,2}}()
		for M in s.p.rings
			L = copy(M)
			for i in 2:size(L, 1)                       # continuous longitudes across the antimeridian
				while L[i, 1] - L[i-1, 1] > 180  L[i, 1] -= 360  end
				while L[i, 1] - L[i-1, 1] < -180 L[i, 1] += 360  end
			end
			Dk = GMT.mat2ds(L; hdr = lab)
			Dk.proj4 = GMT.prj4WGS84
			push!(D, Dk)
		end
		_add_dataset_to_scene(scene, D, lab; groupName = nm, noConvertToPoints = true, forceMode = :lines) || continue
		ccall(_fn(:gmtvtk_overlay_set_tool_pick_h), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring, Cstring),
		      scene, lab, "s2", "pickscene", "Choose this scene (Sentinel-2)")
		labels[lab] = ks
		pen = zeros(6)
		ccall(_fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), scene, lab, pen) != 0 &&
			(pens[lab] = pen)
	end
	_S2_GROUP[scene] = nm
	_S2_FRAMES[scene] = labels
	_S2_PENS[scene] = pens
	# A plain click inside a footprint now chooses its scene (GLView::mouseReleaseEvent -> "clickscene").
	ccall(_fn(:gmtvtk_set_click_tool_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "s2", "clickscene")
	return length(labels)
end

# Highlight the footprint of scene `k` (red, 3 px) and give the one lit before back its own pen.
function _s2_highlight!(scene::Ptr{Cvoid}, k::Int)
	lab = ""
	for (l, ks) in get(_S2_FRAMES, scene, Dict{String,Vector{Int}}())
		k in ks && (lab = l; break)
	end
	_rs_light_footprint!(scene, _S2_LIT, get(_S2_PENS, scene, Dict{String,Vector{Float64}}()), lab)
	# ...and in the quicklook stack, a thin red outline round the chosen scene's picture
	haskey(_S2_STACK, scene) &&
		ccall(_fn(:gmtvtk_sheet_select_h), Cint, (Ptr{Cvoid}, Cstring), scene, isempty(lab) ? "" : lab * " (quicklook)")
	return nothing
end

# --- the quicklook stack ---------------------------------------------------------------------------
# THE stack of every scene tool (_rs_plot_stack!, s1products.jl): one level per pass, the slider, the
# red outline, the click. What is Sentinel-2's own is only WHERE a quicklook lies: it covers the WHOLE
# 109.8 km MGRS tile, not only the data footprint, so it is laid on the tile's corners, worked out from
# the tile's name.
const _S2_STACK = Dict{Ptr{Cvoid},String}()           # window -> group name of its quicklook stack

# The MGRS tile "29SPB" as its UTM system and box (x0, x1, y0, y1): the 100 km square's lower-left
# corner from its two letters (column sets by zone mod 3, row letters shifted by 5 in even zones,
# repeating every 2000 km, resolved with the latitude band), then ESA's tile = 109.8 km from the square's
# upper-left corner going east and south.
const _S2_TILE_BOX = Dict{String,Tuple{Int,Float64,Float64,Float64,Float64}}()   # a tile's box never changes
_s2_tile_box(tile::String)::Tuple{Int,Float64,Float64,Float64,Float64} =
	get!(() -> _s2_tile_box_calc(tile), _S2_TILE_BOX, tile)
function _s2_tile_box_calc(tile::String)::Tuple{Int,Float64,Float64,Float64,Float64}
	m = match(r"^(\d{2})([C-X])([A-Z])([A-V])$", tile)
	m === nothing && error("Not an MGRS tile: \"$(tile)\".")
	zone = parse(Int, m[1]);  band = m[2][1];  cl = m[3][1];  rl = m[4][1]
	cols = ("STUVWXYZ", "ABCDEFGH", "JKLMNPQR")[mod(zone, 3) + 1]
	ic = findfirst(==(cl), cols);  ic === nothing && error("Bad column letter in tile \"$(tile)\".")
	rows = "ABCDEFGHJKLMNPQRSTUV"
	ir = findfirst(==(rl), rows);  ir === nothing && error("Bad row letter in tile \"$(tile)\".")
	e0 = ic * 100000.0
	n0 = mod(ir - 1 - (iseven(zone) ? 5 : 0), 20) * 100000.0
	bands = "CDEFGHJKLMNPQRSTUVWX"
	latS = -80.0 + 8.0 * (findfirst(==(band), bands) - 1)
	north = band >= 'N'
	epsg = (north ? 32600 : 32700) + zone
	cm = -183.0 + 6.0 * zone
	nb = _srs_xy([cm latS], _LONLAT, "EPSG:$(epsg)")[1, 2]     # northing of the band's south edge
	k = max(0, ceil(Int, (nb - 100000.0 - n0) / 2.0e6 - 1e-9))
	nll = n0 + 2.0e6 * k
	return epsg, e0, e0 + 109800.0, nll + 100000.0 - 109800.0, nll + 100000.0
end

_s2_remove_stack!(scene::Ptr{Cvoid}) = _rs_remove_stack!(scene, _S2_STACK)

# The pass a scene belongs to: its sensing start ("20261123T111311"), shared by every tile of one
# acquisition. Tiles of the same pass lie side by side on ONE level of the stack.
_s2_pass(s::S2Scene)::String = (m = match(r"_MSIL\w{2}_(\d{8}T\d{6})_", s.p.name); m === nothing ? s.p.name : String(m[1]))

# Every scene's quicklook (fetched once, kept in <folder>/quicklooks) laid on its tile: THE stack
# (_rs_plot_stack!, s1products.jl).
function _s2_plot_stack!(scene::Ptr{Cvoid}, found::Vector{S2Scene}, dir::String)::Int
	_s2_remove_stack!(scene)
	t0 = minimum(s.p.t0 for s in found);  t1 = maximum(s.p.t0 for s in found)
	g = "Sentinel-2 quicklooks $(GMT.Dates.format(t0, "yyyy-mm-dd")) to $(GMT.Dates.format(t1, "yyyy-mm-dd"))"
	sheets = RsSheet[]
	_rs_prog_show("Fetching $(length(found)) quicklooks…")
	try
		files = String[]
		for (i, s) in enumerate(found)              # a cached one is read from disk
			_rs_prog(round(Int, 100 * (i - 1) / length(found)), "Quicklook $(i) of $(length(found))")
			push!(files, _s2_quicklook(s, dir))
		end
		for (s, f) in zip(found, files)
			p = _s2_pass(s)
			t = try GMT.Dates.DateTime(p, "yyyymmddTHHMMSS") catch; nothing end
			epsg, x0, x1, y0, y1 = _s2_tile_box(s.tile)
			push!(sheets, RsSheet(_s2_base(s), s.p.t0, p, t === nothing ? p : GMT.Dates.format(t, "yyyy-mm-dd HH:MM"),
			                      epsg, x0, x1, y0, y1, f))
		end
		return _rs_plot_stack!(scene, _S2_STACK, sheets, g, "s2", get(_S2_LIT, scene, ""))
	finally
		_rs_prog_close()
	end
end

# --- the dialog ------------------------------------------------------------------------------------

function _s2_folder(d::Dict{String,String})::String
	dir = _rs_get(d, "p_folder")
	return isempty(dir) ? _s2_dir() : dir
end

function _s2_label(s::S2Scene, dir::String)::String
	cc = isnan(s.cloud) ? "?" : "$(round(Int, s.cloud))"
	have = isfile(_s1_file(s.p, dir)) ? "  (on disk)" : ""
	return "$(GMT.Dates.format(s.p.t0, "yyyy-mm-dd HH:MM"))  $(_s2_sat(s))  $(s.tile)  cloud $(cc) %  $(_s2_mb(s.p.bytes))$(have)"
end

function _s2_selected(scene::Ptr{Cvoid}, d::Dict{String,String})::S2Scene
	found = get(_S2_FOUND, scene, S2Scene[])
	isempty(found) && error("Find the scenes first.")
	k = _rs_idx(d, "p_granule") + 1
	(1 <= k <= length(found)) || error("Choose a scene in the list.")
	return found[k]
end

# Choose scene `k`: the list, its footprint, its quicklook and its band panel.
function _s2_choose!(io::IOBuffer, scene::Ptr{Cvoid}, d::Dict{String,String}, k::Int)
	found = get(_S2_FOUND, scene, S2Scene[])
	(1 <= k <= length(found)) || return nothing
	_rs_set(io, "p_granule", k - 1)
	_s2_highlight!(scene, k)
	dir = _s2_folder(d)
	_s2_show_thumb!(io, found[k], dir)
	_s2_fill_bands!(io, d, found[k], dir)
	return nothing
end

function _rs_s2(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	lev = _S2_LEVELS[clamp(_rs_idx(d, "p_product") + 1, 1, length(_S2_LEVELS))][2]
	if what == "savelogin"
		f = _rs_netrc_path()
		_rs_netrc_set!(f, _S1_MACHINE, _rs_get(d, "p_cd_user"), _rs_get(d, "p_cd_pass"))
		_rs_label(io, "lbl_cd_where", "Saved in $(f) as machine $(_S1_MACHINE)")
		_rs_status(io, "Copernicus Data Space login saved in $(f) (machine $(_S1_MACHINE)).")
		return true
	end
	if what == "rgwindow" || (what == "init" && !_rs_empty(scene))
		b = _s1_window_box(scene)
		if b !== nothing
			for (w, v) in zip(("p_rg_w", "p_rg_e", "p_rg_s", "p_rg_n"), b)
				_rs_set(io, w, string(round(v, digits = 4)))
			end
		elseif what == "rgwindow"
			error(_rs_empty(scene) ? "The window shows nothing yet: type the limits." :
			      "The window is neither geographic nor in a known coordinate system: type the limits.")
		end
		what == "rgwindow" && return true
	end
	if what == "init"
		_rs_items(io, "p_product", String[x[1] for x in _S2_LEVELS])
		_rs_set(io, "p_product", 0)
		_rs_items(io, "p_bres", String["$(r) m" for r in _S2_RES])
		_rs_set(io, "p_bres", 0)
		_s1_lock_rgwindow(io, scene)
		today = GMT.Dates.Date(RS.now(RS.UTC))
		_rs_set(io, "p_from", "$(today - GMT.Dates.Day(30))T00:00:00")
		_rs_set(io, "p_to", "$(today)T00:00:00")
		_rs_set(io, "p_folder", _s2_dir())
		f = _rs_netrc_path()
		u, p, why = try
			(_rs_netrc_get(f, _S1_MACHINE)..., isfile(f) ? "" : "  (the file does not exist yet)")
		catch e
			("", "", "  — cannot read it: " * sprint(showerror, e))
		end
		_rs_set(io, "p_cd_user", u);  _rs_set(io, "p_cd_pass", p)
		_rs_label(io, "lbl_cd_where", "Saved in $(f) as machine $(_S1_MACHINE)$(why)")
		# A dialog (re)opened on a window that already has a search shows THAT search.
		found = get(_S2_FOUND, scene, S2Scene[])
		_rs_items(io, "p_granule", String[_s2_label(x, _s2_dir()) for x in found])
		if isempty(found)
			_s2_show_thumb!(io, nothing, _s2_dir())
			_s2_fill_bands!(io, d, nothing, _s2_dir())
		else
			_rs_set(io, "p_product", _s2_isl1c(found[1]) ? 1 : 0)
			_s2_choose!(io, scene, d, 1)
		end
		_rs_status(io, isempty(u) ? "Searching needs no account; downloading needs your Copernicus Data Space login (Account tab)." :
		                            "Give the region, the period and the cloud limit, then Find the scenes.")
		return true
	end
	if what == "refresh"
		_s1_lock_rgwindow(io, scene)
		found = get(_S2_FOUND, scene, S2Scene[])
		k = _rs_idx(d, "p_granule") + 1
		s = 1 <= k <= length(found) ? found[k] : nothing
		# Another level, or ANOTHER REGION, makes the last search's list stale: every scene in it was found
		# for the old region, and one that does not reach the new region must never stay choosable.
		rg = try _s1_box(d) catch; nothing end
		moved = s !== nothing && rg !== nothing && get(_S2_SEARCHED, scene, rg) != rg
		if s !== nothing && (moved || _s2_isl1c(s) != (lev == "MSIL1C"))
			old = pop!(_S2_GROUP, scene, "")
			isempty(old) || ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, old)
			_s2_remove_stack!(scene)
			delete!(_S2_FOUND, scene);  delete!(_S2_FRAMES, scene);  delete!(_S2_PENS, scene);  delete!(_S2_LIT, scene)
			ccall(_fn(:gmtvtk_set_click_tool_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "s2", "")
			_rs_items(io, "p_granule", String[])
			_s2_show_thumb!(io, nothing, _s2_folder(d))
			_s2_fill_bands!(io, d, nothing, _s2_folder(d))
			delete!(_S2_SEARCHED, scene)
			_rs_status(io, (moved ? "Region" : "Product") * " changed: Find the scenes again.")
			return true
		end
		s === nothing ? _s2_fill_bands!(io, d, nothing, _s2_folder(d)) : _s2_choose!(io, scene, d, k)
		return true
	end
	if what == "search"
		W, E, S, N = _s1_box(d)
		d0 = GMT.Dates.Date(DateTime(_rs_get(d, "p_from")))
		d1 = GMT.Dates.Date(DateTime(_rs_get(d, "p_to")))
		d0 <= d1 || error("The period ends before it starts.")
		cloud = clamp(round(Int, _rs_num(d, "p_cloud", 100.0)), 0, 100)
		_rs_prog_show("Asking the Copernicus catalogue for the Sentinel-2 scenes over the region…")
		found, total = try
			_s2_search(lev, W, E, S, N, d0, d1, cloud)
		finally
			_rs_prog_close()
		end
		_S2_FOUND[scene] = found
		_S2_SEARCHED[scene] = (W, E, S, N)            # the region this list belongs to
		dir = _s2_folder(d)
		_rs_items(io, "p_granule", String[_s2_label(x, dir) for x in found])
		nfr = _s2_plot_footprints!(scene, found, W, E, S, N)
		_s1_lock_rgwindow(io, scene)                   # the search may have made a map in an empty window
		isempty(found) ? (_s2_show_thumb!(io, nothing, dir); _s2_fill_bands!(io, d, nothing, dir)) :
		                 _s2_choose!(io, scene, d, 1)
		lvl = lev == "MSIL1C" ? "L1C" : "L2A"
		_rs_status(io, isempty(found) ? "No Sentinel-2 $(lvl) scene over the region in that period with at most $(cloud) % cloud." :
		               "$(length(found)) scene(s) on $(nfr) footprint(s), newest first." *
		               (total > length(found) ? " The catalogue has $(total): shorten the period to see the rest." : "") *
		               " Choose one in the list, or click inside its footprint in the view (or Pick a scene).")
		return true
	end
	# "pickscene" with a point: the armed "Pick a scene" button. "clickscene": a plain click in the view
	# (GLView), where a click outside every footprint is an ordinary click and is answered with nothing.
	if (what == "pickscene" || what == "clickscene") && !isempty(_rs_get(d, "px"))
		quiet = what == "clickscene"
		found = get(_S2_FOUND, scene, S2Scene[])
		if isempty(found)
			quiet && return true
			error("Find the scenes first, then click inside one of the footprints it draws.")
		end
		x, y = _rs_num(d, "px", NaN), _rs_num(d, "py", NaN)
		src = _window_srs(scene)
		if !isempty(src) && !_project_is_geog(src)
			Q = _srs_xy([x y], src, _LONLAT);  x, y = Q[1, 1], Q[1, 2]
		end
		ks = _s1_covering(S1Product[s.p for s in found], x, y)
		if isempty(ks)
			quiet && return true
			error("No scene found covers $(round(x, digits = 3)), $(round(y, digits = 3)): click inside a footprint.")
		end
		# Clicking the same place again steps to the next scene that covers it.
		cur = _rs_idx(d, "p_granule") + 1
		j = findfirst(==(cur), ks)
		k = ks[j === nothing ? 1 : mod1(j + 1, length(ks))]
		_s2_choose!(io, scene, d, k)
		s = found[k]
		_rs_status(io, "Chose $(GMT.Dates.format(s.p.t0, "yyyy-mm-dd HH:MM")) tile $(s.tile)" *
		               (length(ks) > 1 ? ": $(length(ks)) scenes cover that point, click it again for the next." : "."))
		return true
	end
	if what == "pickscene"
		line = replace(_rs_get(d, "picked"), r" \(quicklook\)$" => "")     # a quicklook sheet: its scene
		ks = get(get(_S2_FRAMES, scene, Dict{String,Vector{Int}}()), line, Int[])
		isempty(ks) && error("\"$(line)\" is not a Sentinel-2 footprint: Find the scenes, then click one of the footprints it draws.")
		_s2_choose!(io, scene, d, ks[1])
		s = _S2_FOUND[scene][ks[1]]
		_rs_status(io, "Chose $(GMT.Dates.format(s.p.t0, "yyyy-mm-dd HH:MM")) tile $(s.tile).")
		return true
	end
	if what == "stack"
		found = get(_S2_FOUND, scene, S2Scene[])
		if _rs_toggle_stack!(scene, _S2_STACK)        # pressed again: the 3-D stack goes away
			_rs_status(io, "Quicklook stack removed, the view put back as it was.")
			return true
		end
		isempty(found) && error("Find the scenes first.")
		n = _s2_plot_stack!(scene, found, _s2_folder(d))
		_rs_status(io, "$(n) quicklooks stacked one level per pass, oldest at the bottom. Click one to choose its scene.")
		return true
	end
	if what == "download"
		s = _s2_selected(scene, d)
		dir = _s2_folder(d)
		f = _s1_fetch(s.p, dir, d)
		found = _S2_FOUND[scene]
		_rs_items(io, "p_granule", String[_s2_label(x, dir) for x in found])
		_s2_choose!(io, scene, d, findfirst(==(s), found))
		_rs_status(io, "$(f) ($(_s2_mb(Float64(filesize(f))))).")
		return true
	end
	what == "bands" || error("unknown request: $what")
	s = _s2_selected(scene, d)
	dir = _s2_folder(d)
	res = _s2_res(d)
	have = _s2_files_at(s, res)
	ks = _s2_ticked(d, have)
	isempty(ks) && error("Tick at least one band that this scene has at $(res) m.")
	box = _ls_box(d)
	_rs_on(d, "p_cut") && box === nothing && error("Give the four limits of the region (W < E, S < N), or untick \"Cut to the region\".")
	cks = [k for k in ks if k != "TCI"]
	desc = String[last(_S2_BANDS[findfirst(b -> b[1] == k, _S2_BANDS)]) for k in cks]
	outs = _s2_outs(s, ks, res, dir, box)
	todo = [(t, o) for (t, o) in outs if !(isfile(o) && filesize(o) > 0)]
	if box !== nothing
		# Only the region's window, cut by Copernicus on its server: no band file is downloaded.
		for (t, o) in todo
			_s2_fetch_window(s, t ? ["TCI"] : cks, t, res, box, o, t ? String[] : desc, d; scene = scene)
		end
	else
		# The whole tile, by the same window service in pieces: no JPEG 2000 band file is ever read.
		for (t, o) in todo
			_s2_fetch_tile(s, t ? ["TCI"] : cks, t, res, o, t ? String[] : desc, d; scene = scene)
		end
	end
	# The data takes the window: the quicklook stack (a 3-D picture of the SEARCH) goes, and the window
	# gets back the view it had before it (the 2-D map) -- the same removal as pressing Quicklooks 3-D
	# again. Left there, the cube opened in 3-D under floating quicklooks.
	_s2_remove_stack!(scene)
	shown = String[]
	for (_, o) in outs
		_on_drop(scene, o)                         # the ONE open-a-file-into-a-window path
		push!(shown, basename(o))
	end
	found = _S2_FOUND[scene]
	_rs_items(io, "p_granule", String[_s2_label(x, dir) for x in found])
	_s2_choose!(io, scene, d, findfirst(==(s), found))
	_rs_release_clicks!(scene, "s2")
	_rs_status(io, "$(join(shown, " and ")) written in $(_s2_band_dir(s, dir)) and opened.")
	return true
end
