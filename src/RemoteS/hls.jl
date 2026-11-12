# hls.jl — NASA HLS (Harmonized Landsat Sentinel-2) time series over an area of interest.
#
# HLS v2.0 is two collections on ONE 30 m grid, the Sentinel-2 MGRS tiling: HLSL30 (Landsat 8/9 OLI)
# and HLSS30 (Sentinel-2 MSI), atmospherically corrected, BRDF-normalised and band-adjusted so the two
# mix in one series. Files are Cloud-Optimized GeoTIFFs, one per band, served by LP DAAC behind NASA
# Earthdata Login.
#
# What this does, in order:
#   1. search CMR-STAC (no login) for the items over the box and period, both products;
#   2. keep ONE MGRS tile (the one most items share) so every date lands on the same pixel grid;
#   3. read the Fmask of each item over the box ONLY (a COG window through /vsicurl/) and drop the
#      dates whose box is not clear enough — a tile's cloud cover says little about one field;
#   4. read the bands the index needs, box only, as reflectance (DN * 0.0001, -9999 = NaN);
#   5. compute the index with THE RemoteS engine (`sp_indices`), never a formula of its own.
#
# The Earthdata login is the one the Satellite dialogs already keep: the urs.earthdata.nasa.gov entry of
# the .netrc file (`_rs_netrc_path`, remotes_gui.jl). GDAL's /vsicurl/ follows LP DAAC's redirect to URS
# and answers it from that file; the URS session cookie is kept in ~/.urs_cookies, the file NASA's own
# wget/curl instructions use.
#
# Collection IDs are the CMR-STAC ones since Sept 2024 ("HLSL30_2.0"; the old "HLSL30.v2.0" form is
# retired).

const _HLS_STAC = "https://cmr.earthdata.nasa.gov/stac/LPCLOUD/search"
const _HLS_COLL = Dict{String,String}("L30" => "HLSL30_2.0", "S30" => "HLSS30_2.0")

# Band role -> file band name, per product. The same band NUMBER is not the same band in L30 and S30
# (NIR is B05 in L30, B8A in S30), so callers ask by role, never by number.
const _HLS_BANDS = Dict{String,Dict{String,String}}(
	"L30" => Dict("blue" => "B02", "green" => "B03", "red" => "B04", "nir" => "B05",
	              "swir1" => "B06", "swir2" => "B07"),
	"S30" => Dict("blue" => "B02", "green" => "B03", "red" => "B04", "redEdge1" => "B05",
	              "redEdge2" => "B06", "redEdge3" => "B07", "nir" => "B8A", "swir1" => "B11",
	              "swir2" => "B12"),
)

# Index -> the band roles it takes, in the order its RemoteS method takes them (spectral_indices.jl).
const _HLS_INDEX = Dict{String,Vector{String}}(
	"NDVI"   => ["red", "nir"],
	"EVI"    => ["blue", "red", "nir"],
	"SAVI"   => ["red", "nir"],
	"MSAVI"  => ["red", "nir"],
	"NDWI"   => ["green", "nir"],
	"MNDWI"  => ["green", "swir2"],
	"NBRI"   => ["nir", "swir2"],
	"NDMI"   => ["nir", "swir1"],
	"NDREI1" => ["redEdge1", "redEdge2"],      # S30 only: Landsat has no red-edge bands
	"NDREI2" => ["redEdge1", "redEdge3"],      # S30 only
)

# Fmask bits that make a pixel unusable: 0 cirrus, 1 cloud, 2 adjacent to cloud/shadow, 3 cloud shadow,
# 4 snow/ice. Bit 5 (water) and 6-7 (aerosol level) do not.
const _HLS_FMASK_BAD = 0x1f

# One scene of either source: an HLS granule or a Sentinel-2 L2A scene of Earth Search (s2_search).
struct HLSItem
	id::String
	product::String          # "L30" or "S30" (HLS), "S2A"/"S2B"/"S2C" (Sentinel-2 L2A)
	tile::String             # MGRS tile, e.g. "T29SNB"
	time::DateTime
	cloud::Float64           # whole-tile cloud cover, %
	assets::Dict{String,String}    # band name ("B04", "Fmask", "red", "scl", ...) -> https URL
	scale::Float64           # reflectance = DN * scale + offset
	offset::Float64
	fill::Float32            # the DN of a pixel with no data
end

# --- Sentinel-2 L2A at its own 10 m, from Element84's Earth Search (AWS): COGs, no login ----------
const _ES_STAC = "https://earth-search.aws.element84.com/v1/search"
const _ES_COLL = "sentinel-2-l2a"
# Role -> Earth Search asset. B02/B03/B04/B08 are 10 m; red-edge, B8A and SWIR are 20 m. An index on
# 10 m bands only is made at 10 m with NIR = B08; any other is made at 20 m with NIR = B8A (as in S30).
const _ES_BANDS10 = Dict("blue" => "blue", "green" => "green", "red" => "red", "nir" => "nir")
const _ES_BANDS20 = Dict("blue" => "blue", "green" => "green", "red" => "red", "nir" => "nir08",
                         "redEdge1" => "rededge1", "redEdge2" => "rededge2", "redEdge3" => "rededge3",
                         "swir1" => "swir16", "swir2" => "swir22")
# Scene classification (SCL) classes that make a pixel unusable: 0 no data, 1 saturated/defective,
# 3 cloud shadow, 8 cloud (medium), 9 cloud (high), 10 thin cirrus, 11 snow/ice.
const _ES_SCL_BAD = (0, 1, 3, 8, 9, 10, 11)

"""
    items = hls_search(region; from, to, products=["L30","S30"], maxcloud=100)

Search NASA's CMR-STAC for HLS v2.0 items over `region = [W, E, S, N]` (degrees) between the dates
`from` and `to` (`"yyyy-mm-dd"` or a `DateTime`). `maxcloud` drops items whose WHOLE-TILE cloud cover
(%) is above it. No login is needed to search. Returns a `Vector{HLSItem}` sorted by time.
"""
function hls_search(region::Vector{Float64}; from::Union{String,DateTime}, to::Union{String,DateTime},
                    products::Vector{String}=["L30", "S30"], maxcloud::Float64=100.0)::Vector{HLSItem}
	colls = String[]
	for p in products
		haskey(_HLS_COLL, p) || error("unknown HLS product \"$p\": use \"L30\" and/or \"S30\"")
		push!(colls, _HLS_COLL[p])
	end
	# limit=50: a FULL page of 100 got a 502 every time (2026-11-11)
	return _stac_search(_HLS_STAC, join(colls, ','), region, from, to, 50, _hls_item, maxcloud)
end

"""
    items = s2_search(region; from, to, maxcloud=100)

Search Element84's Earth Search for Sentinel-2 L2A scenes (collection `sentinel-2-l2a`, Cloud-Optimized
GeoTIFFs on AWS) over `region = [W, E, S, N]` (degrees) between the dates `from` and `to`. No account
is needed, neither to search nor to read. `maxcloud` drops scenes whose WHOLE-TILE cloud cover (%) is
above it. A scene reprocessed by ESA is listed once, its latest processing. Returns a `Vector{HLSItem}`
sorted by time, `product` = "S2A"/"S2B"/"S2C".
"""
function s2_search(region::Vector{Float64}; from::Union{String,DateTime}, to::Union{String,DateTime},
                   maxcloud::Float64=100.0)::Vector{HLSItem}
	items = _stac_search(_ES_STAC, _ES_COLL, region, from, to, 100, _es_item, maxcloud)
	# The same take (satellite, tile, time) can be listed twice, "_0" and a later "_1" processing: keep
	# the latest, so a date is never counted (and read) twice.
	best = Dict{Tuple{String,String,DateTime},HLSItem}()
	for it in items
		k = (it.product, it.tile, it.time)
		(!haskey(best, k) || _es_seq(it.id) > _es_seq(best[k].id)) && (best[k] = it)
	end
	return sort!(collect(values(best)), by = it -> it.time)
end

_es_seq(id::String)::Int = (m = match(r"_(\d+)_L2A$", id); m === nothing ? 0 : parse(Int, m[1]))

# Both catalogues are STAC: one GET per page, the next page's address in links[rel=next]. `item`
# turns a feature into an HLSItem (or nothing).
function _stac_search(stac::String, colls::String, region::Vector{Float64}, from::Union{String,DateTime},
                      to::Union{String,DateTime}, limit::Int, item::F, maxcloud::Float64)::Vector{HLSItem} where {F}
	length(region) == 4 || error("region must be [W, E, S, N]")
	W, E, S, N = region
	(W < E && S < N) || error("region must have W < E and S < N")
	t0 = isa(from, DateTime) ? from : DateTime(from)
	t1 = isa(to, DateTime) ? to : DateTime(to) + Day(1) - Second(1)
	t0 <= t1 || error("the period ends before it starts")
	url = "$(stac)?collections=$(colls)&bbox=$(W),$(S),$(E),$(N)" *
	      "&datetime=$(_shub_esc(string(t0, "Z/", t1, "Z")))&limit=$(limit)"
	items = HLSItem[]
	while !isempty(url)
		js = _shub_json(_hls_get(url))
		for f in get(js, "features", Any[])
			it = item(f)
			(it !== nothing && it.cloud <= maxcloud) && push!(items, it)
		end
		url = ""
		for l in get(js, "links", Any[])
			(isa(l, Dict) && get(l, "rel", "") == "next") && (url = _shub_str(l, "href"); break)
		end
	end
	return sort!(items, by = it -> it.time)
end

# One page of the search. CMR-STAC answers an occasional 502/503 on a page that works a second later
# (seen on a year-long search, page 2): such an answer is asked again, twice, before it is an error.
function _hls_get(url::String)::String
	for k in 1:3
		try
			return _shub_http_get(url; timeout = 120)
		catch e
			(k < 3 && occursin(r"answered 5\d\d", sprint(showerror, e))) || rethrow()
			sleep(2.0 * k)
		end
	end
	return ""
end

# One STAC feature -> HLSItem, or nothing when it is not an HLS granule this file understands.
function _hls_item(f)::Union{HLSItem,Nothing}
	isa(f, Dict) || return nothing
	id = _shub_str(f, "id")
	m = match(r"^HLS\.(L30|S30)\.(T\w{5})\.", id)
	m === nothing && return nothing
	props = get(f, "properties", Dict{String,Any}())
	ts = _shub_str(props, "datetime")
	isempty(ts) && return nothing
	t = DateTime(first(replace(ts, "Z" => ""), 23))
	cc = get(props, "eo:cloud_cover", nothing)
	assets = Dict{String,String}()
	for (k, a) in get(f, "assets", Dict{String,Any}())
		href = _shub_str(a, "href")
		startswith(href, "https://") && endswith(href, ".tif") && (assets[k] = href)
	end
	return HLSItem(id, String(m[1]), String(m[2]), t, isa(cc, Real) ? Float64(cc) : NaN, assets, 0.0001, 0.0, -9999f0)
end

# One Earth Search feature ("S2B_29SNC_20261030_0_L2A") -> HLSItem. The scale is the scene's own
# (raster:bands of its red band). The OFFSET is not taken from there blindly: since processing baseline
# 04.00 (2022) L2A DNs carry a +1000 shift, and Earth Search removes it from the pixels itself when it
# says `earthsearch:boa_offset_applied = true` — while raster:bands still lists offset -0.1. Applying it
# again sent red to -0.02 and NDVI to 1.18 on a field where HLS S30 of the same take gives 0.525; with
# offset 0 Sentinel-2 gives 0.520 (Faro, 2024-04-13). So: flag true -> 0, else raster:bands.
function _es_item(f)::Union{HLSItem,Nothing}
	isa(f, Dict) || return nothing
	id = _shub_str(f, "id")
	m = match(r"^(S2[A-D])_(\d{1,2}[A-Z]{3})_\d{8}_\d+_L2A$", id)
	m === nothing && return nothing
	props = get(f, "properties", Dict{String,Any}())
	ts = _shub_str(props, "datetime")
	isempty(ts) && return nothing
	t = DateTime(first(replace(ts, "Z" => ""), 23))
	cc = get(props, "eo:cloud_cover", nothing)
	assets = Dict{String,String}()
	scale, offset = 0.0001, 0.0
	for (k, a) in get(f, "assets", Dict{String,Any}())
		href = _shub_str(a, "href")
		(startswith(href, "https://") && endswith(href, ".tif")) || continue
		assets[k] = href
		if k == "red"
			rb = get(a, "raster:bands", nothing)
			if isa(rb, Vector) && !isempty(rb) && isa(rb[1], Dict)
				s = get(rb[1], "scale", nothing);   isa(s, Real) && (scale = Float64(s))
				o = get(rb[1], "offset", nothing);  isa(o, Real) && (offset = Float64(o))
			end
		end
	end
	get(props, "earthsearch:boa_offset_applied", false) === true && (offset = 0.0)
	tile = "T" * lpad(String(m[2]), 5, '0')
	return HLSItem(id, String(m[1]), tile, t, isa(cc, Real) ? Float64(cc) : NaN, assets, scale, offset, 0f0)
end

# Run `f()` with the GDAL settings an Earthdata COG read needs, set on THIS thread only so no other
# /vsicurl/ user of the session (EMODnet's zips, IPMA's radar) is touched.
function _hls_with_gdal(f::F) where {F}
	opts =["GDAL_HTTP_NETRC" => "YES",
	        "GDAL_HTTP_NETRC_FILE" => InteractiveGMT._rs_netrc_path(),
	        "GDAL_HTTP_COOKIEFILE" => joinpath(homedir(), ".urs_cookies"),
	        "GDAL_HTTP_COOKIEJAR" => joinpath(homedir(), ".urs_cookies"),
	        "GDAL_DISABLE_READDIR_ON_OPEN" => "EMPTY_DIR",
	        "GDAL_HTTP_MAX_RETRY" => "3", "GDAL_HTTP_RETRY_DELAY" => "2"]
	for (k, v) in opts
		ccall((:CPLSetThreadLocalConfigOption, GMT.Gdal.libgdal), Cvoid, (Cstring, Cstring), k, v)
	end
	try
		return f()
	finally
		for (k, _) in opts
			ccall((:CPLSetThreadLocalConfigOption, GMT.Gdal.libgdal), Cvoid, (Cstring, Ptr{Cchar}), k, C_NULL)
		end
	end
end

# One band of one item over the box, as Float32 (raw DNs; scaling is the caller's). The reads run in
# parallel, so this is NOT GMT.gdaltranslate (whose wrapper shares a /vsimem/ destination and global
# state between calls) but the GDAL calls under it, as emodnet.jl makes them on a worker thread:
# GDALOpenEx + GDALTranslate into an in-memory (MEM) dataset + gd2gmt.
function _hls_read(href::String, W::Float64, E::Float64, S::Float64, N::Float64)::GMTgrid
	opts = ["-of", "MEM", "-projwin", string(W), string(N), string(E), string(S),
	        "-projwin_srs", "EPSG:4326", "-ot", "Float32"]
	return _hls_translate(href, opts)
end

# The same band read on the pixels of `R` (an earlier read of the same tile): its window in the tile's
# own coordinates and its size, so a 20 m band (or the 20 m SCL) lands cell for cell on a 10 m grid.
function _hls_read_on(href::String, R::GMTgrid)::GMTgrid
	h = R.registration == 0 ? 0.5 : 0.0
	x0, x1 = R.range[1] - h * R.inc[1], R.range[2] + h * R.inc[1]
	y0, y1 = R.range[3] - h * R.inc[2], R.range[4] + h * R.inc[2]
	nx, ny = round(Int, (x1 - x0) / R.inc[1]), round(Int, (y1 - y0) / R.inc[2])
	opts = ["-of", "MEM", "-projwin", string(x0), string(y1), string(x1), string(y0),
	        "-outsize", string(nx), string(ny), "-r", "nearest", "-ot", "Float32"]
	return _hls_translate(href, opts)
end

function _hls_translate(href::String, opts::Vector{String})::GMTgrid
	G = GMT.Gdal
	return _hls_with_gdal() do
		ds = G.GDALOpenEx("/vsicurl/" * href, G.GDAL_OF_READONLY | G.GDAL_OF_RASTER, C_NULL, C_NULL, C_NULL)
		ds == C_NULL && error("GDAL could not open $href (is the Earthdata login right?)")
		try
			o = G.GDALTranslateOptionsNew(opts, C_NULL)
			r = G.GDALTranslate("", ds, o, Ref{Cint}(0))
			G.GDALTranslateOptionsFree(o)
			r == C_NULL && error("gdal_translate failed on $href")
			try
				return GMT.gd2gmt(G.Dataset(r))
			finally
				G.GDALClose(r)
			end
		finally
			G.GDALClose(ds)
		end
	end
end

# Most of a read's time is waiting on the network (the URS redirect, then the ranges), so the reads run
# concurrently: one Julia thread task each, at most this many at once. Serial on a 1-thread session.
# `tick()` runs after each read, on the calling thread (asyncmap's tasks), so it may report progress.
const _HLS_NTASKS = 8
function _hls_pmap(f::F, xs::Vector{String}, tick::T = () -> nothing) where {F,T}
	Threads.nthreads() == 1 && return [(r = f(x); tick(); r) for x in xs]
	return asyncmap(x -> (r = fetch(Threads.@spawn f(x)); tick(); r), xs; ntasks = _HLS_NTASKS)
end

"""
    S = hls_series(region; from, to, index="NDVI", products=["L30","S30"], maxcloud=80,
                   minclear=0.8, tile="", outdir="", prefix="hls")

Time series of a spectral index from NASA HLS v2.0 over `region = [W, E, S, N]` (degrees), between the
dates `from` and `to`. Only the region's pixels are read (COG windows), never whole tiles.

- `index`: one of NDVI, EVI, SAVI, MSAVI, NDWI, MNDWI, NBRI, NDMI (both products), NDREI1, NDREI2
  (Sentinel-2 only: Landsat has no red-edge bands, so L30 items are skipped for those).
- `maxcloud`: drop items whose whole-tile cloud cover (%) is above this, before reading anything.
- `minclear`: keep a date only if at least this fraction of the region's pixels is clear in its Fmask
  (not cirrus, cloud, cloud-adjacent, shadow or snow).
- `tile`: MGRS tile to use (e.g. "T29SNB"). Empty: the tile most items over the region share, so every
  date is on the same pixel grid.
- `outdir`: when given, each date is written there as `<prefix>_<yyyymmdd>_<L30|S30>.tif`, the stack as
  `<prefix>_cube.nc` and the region means as `<prefix>_series.csv`.
- `progress`: a function `(percent::Int, message::String)` called as the work advances.

Needs a NASA Earthdata login in the .netrc file (`urs.earthdata.nasa.gov` entry), the one the Account
tab of the HLS time series dialog (and of MODIS scenes) saves.

Returns a NamedTuple `(time, product, grids, mean, clear, tile)`: the dates, "L30"/"S30" of each, the
index grids (masked pixels NaN), the region mean of each, the clear fraction of each, and the MGRS tile.
"""
function hls_series(region::Vector{Float64}; from::Union{String,DateTime}, to::Union{String,DateTime},
                    index::String="NDVI", products::Vector{String}=["L30", "S30"], maxcloud::Float64=80.0,
                    minclear::Float64=0.8, tile::String="", outdir::String="", prefix::String="hls",
                    progress = (p, m) -> nothing)
	idx = uppercase(index)
	haskey(_HLS_INDEX, idx) || error("unknown index \"$index\": use one of $(join(sort!(collect(keys(_HLS_INDEX))), ", "))")
	roles = _HLS_INDEX[idx]
	prods = filter(p -> all(r -> haskey(_HLS_BANDS[p], r), roles), products)
	isempty(prods) && error("$idx needs bands ($(join(roles, ", "))) that $(join(products, "/")) does not have")
	u, p = InteractiveGMT._rs_netrc_get(InteractiveGMT._rs_netrc_path(), InteractiveGMT._RS_URS)
	(isempty(u) || isempty(p)) &&
		error("HLS data need a NASA Earthdata login: save it in the Account tab of the HLS time series dialog " *
		      "(or add a 'machine urs.earthdata.nasa.gov' entry to $(InteractiveGMT._rs_netrc_path())).")
	isempty(outdir) || isdir(outdir) || error("output directory \"$outdir\" does not exist")
	progress(2, "Searching NASA's CMR-STAC for HLS $(join(prods, " + ")) items over the region…")
	items = hls_search(region; from = from, to = to, products = prods, maxcloud = maxcloud)
	isempty(items) && error("no HLS item over that region and period (with whole-tile cloud <= $(maxcloud) %)")
	key(it::HLSItem, r::String)::String = _HLS_BANDS[it.product][r]
	return _index_series(items, region, idx, roles, key, "Fmask", _fmask_bad, minclear, tile, outdir, prefix, progress)
end

"""
    S = s2_series(region; from, to, index="NDVI", maxcloud=80, minclear=0.8, tile="", outdir="",
                  prefix="s2", progress)

The same time series as `hls_series`, from Sentinel-2 L2A at its own resolution (Element84's Earth
Search, AWS, no account): **10 m** for the indices made of blue, green, red and NIR (NDVI, EVI, SAVI,
MSAVI, NDWI; NIR = B08), **20 m** for those that need a red-edge or SWIR band (NDMI, MNDWI, NBRI,
NDREI1, NDREI2; NIR = B8A). Clouds come from the scene classification (SCL: no data, saturated, cloud
shadow, cloud, cirrus and snow are masked). Landsat is not mixed in: its grid and bands differ.

Returns the same NamedTuple as `hls_series`; `product` is "S2A", "S2B" or "S2C".
"""
function s2_series(region::Vector{Float64}; from::Union{String,DateTime}, to::Union{String,DateTime},
                   index::String="NDVI", maxcloud::Float64=80.0, minclear::Float64=0.8, tile::String="",
                   outdir::String="", prefix::String="s2", progress = (p, m) -> nothing)
	idx = uppercase(index)
	haskey(_HLS_INDEX, idx) || error("unknown index \"$index\": use one of $(join(sort!(collect(keys(_HLS_INDEX))), ", "))")
	roles = _HLS_INDEX[idx]
	bands = all(r -> haskey(_ES_BANDS10, r), roles) ? _ES_BANDS10 : _ES_BANDS20
	isempty(outdir) || isdir(outdir) || error("output directory \"$outdir\" does not exist")
	progress(2, "Searching Earth Search for Sentinel-2 L2A scenes over the region…")
	items = s2_search(region; from = from, to = to, maxcloud = maxcloud)
	isempty(items) && error("no Sentinel-2 scene over that region and period (with whole-tile cloud <= $(maxcloud) %)")
	key(it::HLSItem, r::String)::String = bands[r]
	return _index_series(items, region, idx, roles, key, "scl", _scl_bad, minclear, tile, outdir, prefix, progress)
end

# The two cloud masks, as "is this pixel unusable": HLS Fmask bits, Sentinel-2 SCL classes.
_fmask_bad(v::Float32)::Bool = !(v >= 0 && v <= 255) || (UInt8(v) & _HLS_FMASK_BAD) != 0
_scl_bad(v::Float32)::Bool = !(v >= 0 && v <= 255) || (round(Int, v) in _ES_SCL_BAD)

# THE series, whatever the source: one MGRS tile, every date's cloud mask over the region first (pass 1),
# then the bands of the dates clear enough (pass 2), all on ONE pixel grid (the first date's first
# band), scaled to reflectance, masked, and the index made by the RemoteS engine. `key(item, role)` is
# the asset holding that band role; `maskkey` the mask asset; `isbad` reads a mask value.
function _index_series(items::Vector{HLSItem}, region::Vector{Float64}, idx::String, roles::Vector{String},
                       key::FK, maskkey::String, isbad::FB, minclear::Float64, tile::String, outdir::String,
                       prefix::String, progress) where {FK,FB}
	if isempty(tile)
		cnt = Dict{String,Int}()
		for it in items  cnt[it.tile] = get(cnt, it.tile, 0) + 1  end
		tile = first(sort!(collect(cnt), by = x -> -x[2]))[1]
	end
	filter!(it -> it.tile == tile, items)
	filter!(it -> haskey(it.assets, maskkey) && all(r -> haskey(it.assets, key(it, r)), roles), items)
	isempty(items) && error("no scene on tile $tile has the bands $(join(roles, ", "))")

	W, E, S, N = region
	times = DateTime[];  prodv = String[];  grids = GMTgrid[];  means = Float64[];  clears = Float64[]
	# The pixel grid every read lands on: the region's window of the first date's first band.
	progress(4, "Tile $(tile): setting up the pixel grid…")
	R = _hls_read(items[1].assets[key(items[1], roles[1])], W, E, S, N)
	rd(h::String) = _hls_read_on(h, R)

	# Pass 1: every item's mask over the box, all at once; keep the dates clear enough.
	n1 = length(items);  d1 = Ref(0)
	progress(5, "Tile $(tile): reading the cloud masks of $(n1) dates…")
	fms = _hls_pmap(rd, String[it.assets[maskkey] for it in items],
	                () -> (d1[] += 1; progress(5 + 30 * d1[] ÷ n1, "Tile $(tile): cloud mask $(d1[]) of $(n1) read")))
	keep = Int[];  bads = BitMatrix[];  clr = Float64[]
	for (k, Fm) in enumerate(fms)
		fm = Fm.z
		bad = falses(size(fm))
		@inbounds for j in eachindex(fm)
			bad[j] = isbad(fm[j])
		end
		clear = 1.0 - count(bad) / length(bad)
		clear < minclear && continue
		push!(keep, k);  push!(bads, bad);  push!(clr, clear)
	end

	# Pass 2: the bands of the kept dates, every (date, band) window at once.
	nb = length(roles)
	hrefs = String[items[k].assets[key(items[k], r)] for k in keep for r in roles]
	n2 = length(hrefs);  d2 = Ref(0)
	isempty(keep) || progress(35, "$(length(keep)) of $(n1) dates clear enough: reading their $(join(roles, ", ")) bands…")
	bandv = _hls_pmap(rd, hrefs,
	                  () -> (d2[] += 1; progress(35 + 60 * d2[] ÷ n2, "Band $(d2[]) of $(n2) read ($(length(keep)) dates)")))
	for (i, k) in enumerate(keep)
		it, bad, clear = items[k], bads[i], clr[i]
		bands = GMTgrid[bandv[(i - 1) * nb + j] for j in 1:nb]
		sc, of, fv = Float32(it.scale), Float32(it.offset), it.fill
		for (j, B) in enumerate(bands)
			size(B.z) == size(bad) || error("$(it.id): band $(roles[j]) and mask windows differ in size")
			z = B.z
			@inbounds for q in eachindex(z)
				z[q] = (z[q] == fv || bad[q]) ? NaN32 : z[q] * sc + of
			end
		end
		v = nb == 3 ? sp_indices(bands[1].z, bands[2].z, bands[3].z; index = idx) :
		              sp_indices(bands[1].z, bands[2].z; index = idx)
		# The engine writes 0 where it clips; the masked pixels go back to NaN.
		G = bands[1]
		z = G.z
		@inbounds for q in eachindex(z)
			z[q] = isnan(z[q]) ? NaN32 : v[q]
		end
		for b in bands[2:end]
			@inbounds for q in eachindex(z)  isnan(b.z[q]) && (z[q] = NaN32)  end
		end
		ok = filter(!isnan, vec(z))
		isempty(ok) && continue
		G.range[5], G.range[6] = minimum(ok), maximum(ok)
		G.names = ["$idx $(it.product) $(Date(it.time))"]
		push!(times, it.time);  push!(prodv, it.product);  push!(grids, G)
		push!(means, sum(Float64, ok) / length(ok));  push!(clears, clear)
	end
	isempty(grids) && error("no date over that region is at least $(round(Int, 100 * minclear)) % clear")

	if !isempty(outdir)
		progress(96, "Writing the grids, the cube and the series to $(outdir)…")
		files = String[]
		for (t, p, G) in zip(times, prodv, grids)
			f = joinpath(outdir, "$(prefix)_$(GMT.Dates.format(t, "yyyymmdd"))_$(p).tif")
			gmtwrite(f, G)
			push!(files, f)
		end
		stackgrids(files, times; z_unit = "decimalyear", save = joinpath(outdir, "$(prefix)_cube.nc"))
		open(joinpath(outdir, "$(prefix)_series.csv"), "w") do io
			println(io, "date,product,$(lowercase(idx))_mean,clear_fraction")
			for (t, p, m, c) in zip(times, prodv, means, clears)
				println(io, Date(t), ",", p, ",", @sprintf("%.4f", m), ",", @sprintf("%.3f", c))
			end
		end
	end
	return (time = times, product = prodv, grids = grids, mean = means, clear = clears, tile = tile)
end
