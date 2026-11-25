# landsat.jl — Satellite > "Landsat scenes…": Landsat scenes found over a region, a period and a cloud-cover
# limit, each with its browse picture, then downloaded WHOLE or band by band, the bands cut to the region
# into one band cube shown in the window. One more tool of the Remote sensing dialog family
# (RemoteSDialog, 70_window.cpp, over deps/ui/remotes_landsat.ui). Same shape as Sentinel-2 MSI products
# (s2products.jl), whose band-cut (`_s2_cut_box`) it calls as it is.
#
# Two sources, the Source combo (checked live 2026-11-24):
#   * Microsoft Planetary Computer, the DEFAULT: USGS's own Collection 2 Level-2 files (Landsat 4-5, 7,
#     8-9; same file names) with NO account at all. Search: its public STAC API. Preview: its public
#     rendered_preview PNG. Files: an asset's blob URL signed by the public `sas/v1/sign` call. (Its
#     Level-1 collection holds only the old MSS scenes, so Level-1 of TM/ETM+/OLI is USGS only.) There is
#     no archive of a whole scene there: "whole scene" = every file of the item, one by one.
#   * USGS, through the Machine-to-Machine API EarthExplorer itself runs on: every Landsat (4-5 TM, 7 ETM+,
#     8-9 OLI/TIRS), Collection 2 Level-1 and Level-2, the whole globe. `login-token` with the user's ERS
#     user name and APPLICATION TOKEN (.netrc machine m2m.cr.usgs.gov) gives the API key, sent as the
#     X-Auth-Token header. `scene-search` answers entityId, displayId, cloudCover, spatialCoverage (GeoJSON)
#     and `browse` (browsePath, a ~80 kB JPEG served WITHOUT a login). DOWNLOADING (download-options,
#     download-request) needs M2M download access granted by USGS on request: without it download-options
#     is an HTTP 403, said so in plain words by `_ls_m2m`.
# Not a source: the Copernicus Data Space (it lists ESA's Landsat 8-9 Level-1 but refuses every download,
# HTTP 403 DAT-ZIP-608: ESA keeps Landsat downloads for Copernicus Services users), and AWS (USGS's
# usgs-landsat bucket is requester-pays: no anonymous access).

const _LS_M2M     = "https://m2m.cr.usgs.gov/api/api/json/stable/"
const _LS_MACHINE = "m2m.cr.usgs.gov"                 # the .netrc entry: ERS user name + application token
const _LS_PC      = "https://planetarycomputer.microsoft.com/api/"
const _LS_SRCKEYS = (:pc, :usgs)                      # the Source combo, in its order
const _LS_SOURCES = ["Microsoft Planetary Computer (no account)", "USGS EarthExplorer (M2M account)"]

# The Product combo of each source: (text, M2M dataset or Planetary Computer platforms, sensor, level).
const _LS_PRODUCTS = Dict{Symbol,Vector{Tuple{String,String,Symbol,Int}}}(
	:pc   => [("Landsat 8-9 OLI/TIRS, Level-2 surface reflectance", "landsat-8 landsat-9", :ot, 2),
	          ("Landsat 7 ETM+, Level-2 surface reflectance",       "landsat-7", :etm, 2),
	          ("Landsat 4-5 TM, Level-2 surface reflectance",       "landsat-4 landsat-5", :tm, 2)],
	:usgs => [("Landsat 8-9 OLI/TIRS, Level-2 surface reflectance", "landsat_ot_c2_l2", :ot, 2),
	          ("Landsat 8-9 OLI/TIRS, Level-1",                     "landsat_ot_c2_l1", :ot, 1),
	          ("Landsat 7 ETM+, Level-2 surface reflectance",       "landsat_etm_c2_l2", :etm, 2),
	          ("Landsat 7 ETM+, Level-1",                           "landsat_etm_c2_l1", :etm, 1),
	          ("Landsat 4-5 TM, Level-2 surface reflectance",       "landsat_tm_c2_l2", :tm, 2),
	          ("Landsat 4-5 TM, Level-1",                           "landsat_tm_c2_l1", :tm, 1)],
)

# The band files of each (sensor, level): (file suffix, check box text, description, role). Role :cube =
# a 30 m 16-bit layer of the band cube; :own = shown as a grid of its own (the 15 m panchromatic band, and
# the 8-bit / signed quality layers, which a UInt16 cube would mangle). Collection 2 names (USGS's Landsat
# Collection 2 product guides; the 8-9 Level-1 list checked on a CDSE product's file tree, the 8-9 Level-2
# list on USGS's STAC items, 2026-11-24).
const _LS_BANDS = Dict{Tuple{Symbol,Int},Vector{Tuple{String,String,String,Symbol}}}(
	(:ot, 2) => [("SR_B1", "SR B1", "B1 coastal aerosol 443 nm", :cube), ("SR_B2", "SR B2", "B2 blue 482 nm", :cube),
	             ("SR_B3", "SR B3", "B3 green 562 nm", :cube), ("SR_B4", "SR B4", "B4 red 655 nm", :cube),
	             ("SR_B5", "SR B5", "B5 near infrared 865 nm", :cube), ("SR_B6", "SR B6", "B6 SWIR 1.61 um", :cube),
	             ("SR_B7", "SR B7", "B7 SWIR 2.2 um", :cube), ("ST_B10", "ST B10", "B10 surface temperature", :cube),
	             ("QA_PIXEL", "QA PIXEL", "pixel quality (clouds, shadow, water, snow)", :cube),
	             ("QA_RADSAT", "QA RADSAT", "radiometric saturation", :cube),
	             ("SR_QA_AEROSOL", "QA AEROSOL", "aerosol quality", :own)],
	(:ot, 1) => [("B1", "B1", "B1 coastal aerosol 443 nm", :cube), ("B2", "B2", "B2 blue 482 nm", :cube),
	             ("B3", "B3", "B3 green 562 nm", :cube), ("B4", "B4", "B4 red 655 nm", :cube),
	             ("B5", "B5", "B5 near infrared 865 nm", :cube), ("B6", "B6", "B6 SWIR 1.61 um", :cube),
	             ("B7", "B7", "B7 SWIR 2.2 um", :cube), ("B8", "B8 (15 m)", "B8 panchromatic, 15 m", :own),
	             ("B9", "B9", "B9 cirrus 1.37 um", :cube), ("B10", "B10", "B10 thermal 10.9 um", :cube),
	             ("B11", "B11", "B11 thermal 12.0 um", :cube),
	             ("QA_PIXEL", "QA PIXEL", "pixel quality (clouds, shadow, water, snow)", :cube),
	             ("QA_RADSAT", "QA RADSAT", "radiometric saturation", :cube)],
	(:etm, 2) => [("SR_B1", "SR B1", "B1 blue 485 nm", :cube), ("SR_B2", "SR B2", "B2 green 560 nm", :cube),
	              ("SR_B3", "SR B3", "B3 red 660 nm", :cube), ("SR_B4", "SR B4", "B4 near infrared 835 nm", :cube),
	              ("SR_B5", "SR B5", "B5 SWIR 1.65 um", :cube), ("SR_B7", "SR B7", "B7 SWIR 2.22 um", :cube),
	              ("ST_B6", "ST B6", "B6 surface temperature", :cube),
	              ("QA_PIXEL", "QA PIXEL", "pixel quality (clouds, shadow, water, snow)", :cube),
	              ("QA_RADSAT", "QA RADSAT", "radiometric saturation", :cube),
	              ("SR_CLOUD_QA", "CLOUD QA", "cloud quality (8 bits)", :own),
	              ("SR_ATMOS_OPACITY", "OPACITY", "atmospheric opacity", :own)],
	(:etm, 1) => [("B1", "B1", "B1 blue 485 nm", :cube), ("B2", "B2", "B2 green 560 nm", :cube),
	              ("B3", "B3", "B3 red 660 nm", :cube), ("B4", "B4", "B4 near infrared 835 nm", :cube),
	              ("B5", "B5", "B5 SWIR 1.65 um", :cube), ("B6_VCID_1", "B6 low", "B6 thermal, low gain", :cube),
	              ("B6_VCID_2", "B6 high", "B6 thermal, high gain", :cube), ("B7", "B7", "B7 SWIR 2.22 um", :cube),
	              ("B8", "B8 (15 m)", "B8 panchromatic, 15 m", :own),
	              ("QA_PIXEL", "QA PIXEL", "pixel quality (clouds, shadow, water, snow)", :cube),
	              ("QA_RADSAT", "QA RADSAT", "radiometric saturation", :cube)],
	(:tm, 2) => [("SR_B1", "SR B1", "B1 blue 485 nm", :cube), ("SR_B2", "SR B2", "B2 green 560 nm", :cube),
	             ("SR_B3", "SR B3", "B3 red 660 nm", :cube), ("SR_B4", "SR B4", "B4 near infrared 830 nm", :cube),
	             ("SR_B5", "SR B5", "B5 SWIR 1.65 um", :cube), ("SR_B7", "SR B7", "B7 SWIR 2.22 um", :cube),
	             ("ST_B6", "ST B6", "B6 surface temperature", :cube),
	             ("QA_PIXEL", "QA PIXEL", "pixel quality (clouds, shadow, water, snow)", :cube),
	             ("QA_RADSAT", "QA RADSAT", "radiometric saturation", :cube),
	             ("SR_CLOUD_QA", "CLOUD QA", "cloud quality (8 bits)", :own),
	             ("SR_ATMOS_OPACITY", "OPACITY", "atmospheric opacity", :own)],
	(:tm, 1) => [("B1", "B1", "B1 blue 485 nm", :cube), ("B2", "B2", "B2 green 560 nm", :cube),
	             ("B3", "B3", "B3 red 660 nm", :cube), ("B4", "B4", "B4 near infrared 830 nm", :cube),
	             ("B5", "B5", "B5 SWIR 1.65 um", :cube), ("B6", "B6", "B6 thermal 11.45 um", :cube),
	             ("B7", "B7", "B7 SWIR 2.22 um", :cube),
	             ("QA_PIXEL", "QA PIXEL", "pixel quality (clouds, shadow, water, snow)", :cube),
	             ("QA_RADSAT", "QA RADSAT", "radiometric saturation", :cube)],
)
const _LS_NSLOT = 15                                 # p_band_01 .. p_band_15 in the .ui
# The bands ticked when a product is chosen: true colour plus near infrared.
const _LS_DEFAULT = Dict{Tuple{Symbol,Int},Vector{String}}(
	(:ot, 2) => ["SR_B2", "SR_B3", "SR_B4", "SR_B5"], (:ot, 1) => ["B2", "B3", "B4", "B5"],
	(:etm, 2) => ["SR_B1", "SR_B2", "SR_B3", "SR_B4"], (:etm, 1) => ["B1", "B2", "B3", "B4"],
	(:tm, 2) => ["SR_B1", "SR_B2", "SR_B3", "SR_B4"], (:tm, 1) => ["B1", "B2", "B3", "B4"])

struct LsScene
	src::Symbol                      # :pc | :usgs
	prod::Int                        # index into _LS_PRODUCTS[src]
	id::String                       # USGS entityId | Planetary Computer item id
	name::String                     # the product name, as in its files (LC09_L2SP_203034_20261031_20261101_02_T1)
	t0::DateTime
	cloud::Float64                   # percent, NaN when unknown
	bytes::Float64                   # whole product; 0 when the source does not say
	browse::String                   # the browse picture's address
	rings::Vector{Matrix{Float64}}   # footprint, lon lat
	path::Int
	row::Int
	files::Dict{String,String}       # Planetary Computer: file name -> its (unsigned) address; USGS: empty
	# The scene's image GRID (what its browse picture covers): (EPSG, x0, x1, y0, y1) in its UTM system, as
	# the Planetary Computer item states it (proj:epsg, proj:shape, proj:transform); EPSG 0 = not stated
	# (USGS), then _ls_grid takes the UTM box of the footprint.
	grid::NTuple{5,Float64}
end
LsScene(src, prod, id, name, t0, cloud, bytes, browse, rings, path, row, files) =
	LsScene(src, prod, id, name, t0, cloud, bytes, browse, rings, path, row, files, (0.0, 0.0, 0.0, 0.0, 0.0))

const _LS_FOUND    = Dict{Ptr{Cvoid},Vector{LsScene}}()             # window -> its last search
const _LS_FRAMES   = Dict{Ptr{Cvoid},Dict{String,Vector{Int}}}()    # window -> footprint label -> scenes
const _LS_GROUP    = Dict{Ptr{Cvoid},String}()                      # window -> group tag of its footprints
const _LS_PENS     = Dict{Ptr{Cvoid},Dict{String,Vector{Float64}}}() # window -> footprint label -> own pen
const _LS_LIT      = Dict{Ptr{Cvoid},String}()                      # window -> the highlighted footprint
const _LS_LASTPROD = Dict{Ptr{Cvoid},Tuple{Int,Int}}()              # window -> (source, product) last shown

_ls_dir()::String = get(ENV, "IGMT_LANDSAT_DIR", joinpath(homedir(), "Downloads", "Landsat"))
_ls_kind(s::LsScene)::Tuple{Symbol,Int} = (_LS_PRODUCTS[s.src][s.prod][3], _LS_PRODUCTS[s.src][s.prod][4])
_ls_bands(s::LsScene) = _LS_BANDS[_ls_kind(s)]
_ls_sat(s::LsScene)::String = first(s.name, 4)
_ls_band_dir(s::LsScene, dir::String)::String = joinpath(dir, s.name)
_ls_band_file(s::LsScene, suffix::String, dir::String)::String = joinpath(_ls_band_dir(s, dir), "$(s.name)_$(suffix).TIF")
_ls_whole_file(s::LsScene, dir::String)::String = joinpath(dir, s.name * ".tar")      # USGS's bundle
_ls_srcidx(s::LsScene)::Int = findfirst(==(s.src), _LS_SRCKEYS)
# Is the whole scene on disk? USGS: its bundle; Planetary Computer: every file of the item.
_ls_have_whole(s::LsScene, dir::String)::Bool = s.src === :usgs ? isfile(_ls_whole_file(s, dir)) :
	!isempty(s.files) && all(f -> isfile(joinpath(_ls_band_dir(s, dir), f)), keys(s.files))

# Path and row out of a Collection 2 name: LC09_L2SP_203034_20261031_20261101_02_T1 -> (203, 34).
function _ls_pathrow(name::AbstractString)::Tuple{Int,Int}
	m = match(r"^L[COTEM]\d{2}_L[12][A-Z]{2}_(\d{3})(\d{3})_", name)
	return m === nothing ? (0, 0) : (parse(Int, m[1]), parse(Int, m[2]))
end

# Quote a string for the JSON bodies M2M is sent.
_ls_jstr(s::AbstractString)::String = "\"" * replace(String(s), "\\" => "\\\\", "\"" => "\\\"") * "\""

# --- USGS M2M ----------------------------------------------------------------------------------------

const _LS_KEY    = Ref{String}("")
const _LS_KEYEXP = Ref{Float64}(0.0)
const _LS_KEYWHO = Ref{String}("")

# One M2M call: POST a JSON body, answer its `data`. An M2M error (errorCode) or an HTTP error is raised
# in words; a 403 is the account missing M2M download access, which only USGS can grant.
function _ls_m2m(ep::String, body::String; key::String = "", timeout::Real = 120)
	io = IOBuffer()
	hdrs = ["Content-Type" => "application/json", "Content-Length" => string(sizeof(body))]
	isempty(key) || push!(hdrs, "X-Auth-Token" => key)
	r = Downloads.request(_LS_M2M * ep; method = "POST", input = IOBuffer(body), output = io, headers = hdrs,
	                      timeout = timeout, throw = false)
	isa(r, Downloads.Response) || throw(r)
	txt = String(take!(io))
	r.status == 403 && error("USGS refused \"$(ep)\" (HTTP 403): this USGS account has no M2M download access yet. " *
	                         "Request it on your ERS profile (ers.cr.usgs.gov, \"Request Access\", M2M API); USGS reviews it in 1-2 working days.")
	(200 <= r.status < 300) || error("USGS M2M answered $(r.status) to \"$(ep)\".\n\n$(first(txt, 300))")
	js = _shub_json(txt)
	isa(js, Dict) || error("USGS M2M did not answer \"$(ep)\" with JSON.")
	ec = get(js, "errorCode", nothing)
	isa(ec, String) && !isempty(ec) && error("USGS M2M, \"$(ep)\": $(ec): $(_shub_str(js, "errorMessage"))")
	return get(js, "data", nothing)
end

function _ls_login(d::Dict{String,String})::String
	u, t = _rs_get(d, "p_us_user"), _rs_get(d, "p_us_token")
	(isempty(u) || isempty(t)) && ((u, t) = _rs_netrc_get(_rs_netrc_path(), _LS_MACHINE))
	(isempty(u) || isempty(t)) &&
		error("USGS needs your ERS user name and M2M application token: give them in the Account tab.")
	u == _LS_KEYWHO[] && time() < _LS_KEYEXP[] && return _LS_KEY[]
	k = _ls_m2m("login-token", "{\"username\":$(_ls_jstr(u)),\"token\":$(_ls_jstr(t))}")
	(isa(k, String) && !isempty(k)) || error("USGS M2M returned no API key for $(u).")
	_LS_KEY[] = k;  _LS_KEYEXP[] = time() + 1.5 * 3600;  _LS_KEYWHO[] = u     # a key lives 2 hours
	return k
end

function _ls_usgs_search(prod::Int, W::Float64, E::Float64, S::Float64, N::Float64, d0::GMT.Dates.Date,
                         d1::GMT.Dates.Date, cloud::Int, d::Dict{String,String})::Tuple{Vector{LsScene},Int}
	key = _ls_login(d)
	ds = _LS_PRODUCTS[:usgs][prod][2]
	body = "{\"datasetName\":$(_ls_jstr(ds)),\"maxResults\":1000,\"metadataType\":\"summary\",\"sceneFilter\":{" *
	       "\"spatialFilter\":{\"filterType\":\"mbr\",\"lowerLeft\":{\"latitude\":$(S),\"longitude\":$(W)}," *
	       "\"upperRight\":{\"latitude\":$(N),\"longitude\":$(E)}}," *
	       "\"acquisitionFilter\":{\"start\":\"$(d0)\",\"end\":\"$(d1)\"}," *
	       "\"cloudCoverFilter\":{\"min\":0,\"max\":$(cloud),\"includeUnknown\":false}}}"
	data = _ls_m2m("scene-search", body; key = key, timeout = 180)
	res = isa(data, Dict) ? get(data, "results", Any[]) : Any[]
	found = LsScene[]
	for o in (isa(res, Vector) ? res : Any[])
		isa(o, Dict) || continue
		nm = _shub_str(o, "displayId");  id = _shub_str(o, "entityId")
		(isempty(nm) || isempty(id)) && continue
		tc = get(o, "temporalCoverage", nothing)
		ts = isa(tc, Dict) ? _shub_str(tc, "startDate") : ""
		length(ts) >= 10 || continue
		t0 = length(ts) >= 19 ? DateTime(replace(ts[1:19], ' ' => 'T')) : DateTime(GMT.Dates.Date(ts[1:10]))
		cc = get(o, "cloudCover", nothing)
		cv = isa(cc, Real) ? Float64(cc) : isa(cc, String) ? something(tryparse(Float64, cc), NaN) : NaN
		br = get(o, "browse", nothing)
		bp = ""
		if isa(br, Vector)
			for b in br
				isa(b, Dict) && (bp = _shub_str(b, "browsePath"); isempty(bp) || break)
			end
		end
		p, r = _ls_pathrow(nm)
		push!(found, LsScene(:usgs, prod, id, nm, t0, cv, 0.0, bp, _s1_rings(get(o, "spatialCoverage", nothing)), p, r,
		                     Dict{String,String}()))
	end
	sort!(found, by = s -> (s.t0, s.name), rev = true)
	tot = isa(data, Dict) ? get(data, "totalHits", length(found)) : length(found)
	return found, isa(tot, Real) ? round(Int, tot) : length(found)
end

# The download addresses of one scene: the whole product (the bundle) and every band file, from
# download-options. NOT YET CHECKED LIVE: the test account had no M2M download access (HTTP 403) on
# 2026-11-24. Field names: products carry id, productName, available, secondaryDownloads; a secondary
# download carries id, entityId, displayId (the file name), available.
function _ls_usgs_options(s::LsScene, key::String)::Tuple{Union{Nothing,Tuple{String,String}},Dict{String,Tuple{String,String}}}
	ds = _LS_PRODUCTS[:usgs][s.prod][2]
	data = _ls_m2m("download-options", "{\"datasetName\":$(_ls_jstr(ds)),\"entityIds\":[$(_ls_jstr(s.id))]," *
	                                   "\"includeSecondaryFileGroups\":true}"; key = key)
	whole = nothing
	files = Dict{String,Tuple{String,String}}()                # file name -> (entityId, productId)
	for p in (isa(data, Vector) ? data : Any[])
		isa(p, Dict) || continue
		get(p, "available", false) == true || continue
		pid = string(get(p, "id", ""));  pn = _shub_str(p, "productName")
		(whole === nothing || occursin("Bundle", pn)) && !isempty(pid) && (whole = (_shub_str(p, "entityId"), pid))
		sec = get(p, "secondaryDownloads", nothing)
		for x in (isa(sec, Vector) ? sec : Any[])
			isa(x, Dict) && get(x, "available", false) == true || continue
			dn = _shub_str(x, "displayId")
			isempty(dn) || (files[dn] = (_shub_str(x, "entityId"), string(get(x, "id", ""))))
		end
	end
	return whole, files
end

# The URL of one download: download-request, then download-retrieve while USGS stages it.
function _ls_usgs_url(entity::String, product::String, key::String)::String
	lab = "igmt_" * string(round(Int, time() * 1000))
	data = _ls_m2m("download-request", "{\"downloads\":[{\"entityId\":$(_ls_jstr(entity)),\"productId\":$(_ls_jstr(product))}]," *
	                                   "\"label\":$(_ls_jstr(lab))}"; key = key)
	urls(v) = [_shub_str(x, "url") for x in (isa(v, Vector) ? v : Any[]) if isa(x, Dict) && !isempty(_shub_str(x, "url"))]
	u = isa(data, Dict) ? urls(get(data, "availableDownloads", nothing)) : String[]
	isempty(u) || return u[1]
	t0 = time()
	while time() - t0 < 300                                      # staged: asked again every 5 s, 5 minutes at most
		sleep(5)
		r = _ls_m2m("download-retrieve", "{\"label\":$(_ls_jstr(lab))}"; key = key)
		u = isa(r, Dict) ? urls(get(r, "available", nothing)) : String[]
		isempty(u) || return u[1]
	end
	error("USGS did not make the file ready within 5 minutes: try again later.")
end

# --- Microsoft Planetary Computer ------------------------------------------------------------------------

function _ls_post_json(url::String, body::String; timeout::Real = 180)
	io = IOBuffer()
	r = Downloads.request(url; method = "POST", input = IOBuffer(body), output = io, timeout = timeout, throw = false,
	                      headers = ["Content-Type" => "application/json", "Content-Length" => string(sizeof(body))])
	isa(r, Downloads.Response) || throw(r)
	txt = String(take!(io))
	(200 <= r.status < 300) || error("The Planetary Computer answered $(r.status).\n\n$(first(txt, 300))")
	return _shub_json(txt)
end

# Its STAC search, one page of up to 1000 items (newest first after sorting here), and whether there
# were more (a "next" link).
function _ls_pc_search(prod::Int, W::Float64, E::Float64, S::Float64, N::Float64, d0::GMT.Dates.Date,
                       d1::GMT.Dates.Date, cloud::Int)::Tuple{Vector{LsScene},Bool}
	plats = join(("\"$(p)\"" for p in split(_LS_PRODUCTS[:pc][prod][2])), ",")
	body = "{\"collections\":[\"landsat-c2-l2\"],\"bbox\":[$(W),$(S),$(E),$(N)]," *
	       "\"datetime\":\"$(d0)T00:00:00Z/$(d1)T23:59:59Z\",\"limit\":1000," *
	       "\"query\":{\"platform\":{\"in\":[$(plats)]},\"eo:cloud_cover\":{\"lte\":$(cloud)}}}"
	js = _ls_post_json(_LS_PC * "stac/v1/search", body)
	isa(js, Dict) || error("The Planetary Computer did not answer with a list of scenes.")
	feats = get(js, "features", Any[])
	found = LsScene[]
	for o in (isa(feats, Vector) ? feats : Any[])
		isa(o, Dict) || continue
		pr = get(o, "properties", nothing);  as = get(o, "assets", nothing)
		(isa(pr, Dict) && isa(as, Dict)) || continue
		ts = _shub_str(pr, "datetime")
		length(ts) >= 19 || continue
		files = Dict{String,String}()
		for (_, a) in as
			isa(a, Dict) || continue
			h = _shub_str(a, "href")
			startswith(h, "https://") && occursin(".blob.core.windows.net/", h) && (files[basename(h)] = h)
		end
		isempty(files) && continue
		# The product name is the folder its files sit in (the item id drops the processing date).
		nm = basename(dirname(first(values(files))))
		cc = get(pr, "eo:cloud_cover", nothing)
		pv = ""
		for (k, a) in as
			k == "rendered_preview" && isa(a, Dict) && (pv = _shub_str(a, "href"))
		end
		pa, ro = _ls_pathrow(nm)
		# The image grid: proj:shape = (rows, columns), proj:transform = (dx, 0, x0, 0, -dy, y1).
		ep = get(pr, "proj:epsg", nothing);  sh = get(pr, "proj:shape", nothing);  tr = get(pr, "proj:transform", nothing)
		grid = (0.0, 0.0, 0.0, 0.0, 0.0)
		if isa(ep, Real) && isa(sh, Vector) && length(sh) >= 2 && isa(tr, Vector) && length(tr) >= 6 &&
		   all(x -> isa(x, Real), sh) && all(x -> isa(x, Real), tr)
			x0 = Float64(tr[3]);  y1 = Float64(tr[6])
			grid = (Float64(ep), x0, x0 + sh[2] * Float64(tr[1]), y1 + sh[1] * Float64(tr[5]), y1)
		end
		push!(found, LsScene(:pc, prod, _shub_str(o, "id"), nm, DateTime(ts[1:19]), isa(cc, Real) ? Float64(cc) : NaN,
		                     0.0, pv, _s1_rings(get(o, "geometry", nothing)), pa, ro, files, grid))
	end
	sort!(found, by = s -> (s.t0, s.name), rev = true)
	links = get(js, "links", Any[])
	more = isa(links, Vector) && any(l -> isa(l, Dict) && _shub_str(l, "rel") == "next", links)
	return found, more
end

# A blob address signed for reading: the public sas/v1/sign call, no account.
function _ls_pc_sign(href::String)::String
	js = _shub_json(_shub_http_get(_LS_PC * "sas/v1/sign?href=" * _shub_esc(href); timeout = 60))
	s = _shub_str(js, "href")
	isempty(s) && error("The Planetary Computer did not sign $(basename(href)).")
	return s
end

_ls_s1product(s::LsScene)::S1Product = S1Product(s.id, s.name, s.t0, s.bytes, "", 0, s.rings)

# --- files -----------------------------------------------------------------------------------------------

const _LS_TIF_MAGIC = (UInt8[0x49, 0x49, 0x2a, 0x00], UInt8[0x49, 0x49, 0x2b, 0x00], UInt8[0x4d, 0x4d, 0x00, 0x2a])

# One file to `f` through a ".part" file whose first bytes must be `ok(head)` (a refused request sends an
# error page). `prog(now)` follows the bytes.
function _ls_get(url::String, f::String, hdrs::Vector{Pair{String,String}}, ok::Function, prog::Function)::Nothing
	mkpath(dirname(f))
	part = f * ".part"
	try
		r = open(part, "w") do io
			Downloads.request(url; output = io, throw = false, headers = hdrs, progress = (total, now, _...) -> prog(now))
		end
		isa(r, Downloads.Response) || throw(r)
		if !(200 <= r.status < 300)
			txt = try first(read(part, String), 300) catch; "" end
			error("The server answered $(r.status) for $(basename(f))\n\n$(txt)")
		end
		ok(open(io -> read(io, 4), part)) || error("The server did not send the file $(basename(f)): $(first(read(part, String), 300))")
		mv(part, f; force = true)
	finally
		isfile(part) && rm(part; force = true)
	end
	return nothing
end

# The browse picture, public on both sources: USGS's JPEG, the Planetary Computer's rendered PNG.
function _ls_quicklook(s::LsScene, dir::String, d::Dict{String,String})::String
	isempty(s.browse) && return ""
	f = joinpath(dir, "quicklooks", s.name * (s.src === :pc ? ".png" : ".jpg"))
	isfile(f) && filesize(f) > 0 && return f
	pic(h) = length(h) >= 2 && ((h[1] == 0xff && h[2] == 0xd8) || (h[1] == 0x89 && h[2] == UInt8('P')))
	try
		_ls_get(s.browse, f, Pair{String,String}[], pic, _ -> nothing)
	catch
		return ""
	end
	return f
end

function _ls_show_thumb!(io::IOBuffer, s::Union{LsScene,Nothing}, dir::String, d::Dict{String,String})
	if s === nothing
		_rs_label(io, "lbl_thumb", "No scene chosen.");  _rs_label(io, "lbl_thumbinfo", "")
		return nothing
	end
	f = _ls_quicklook(s, dir, d)
	_rs_label(io, "lbl_thumb", !isempty(f) ? "<img src=\"file:///$(replace(f, '\\' => '/'))\" width=\"300\" height=\"300\">" :
	                           "The source has no browse picture for this scene.")
	cc = isnan(s.cloud) ? "cloud ?" : "cloud $(round(s.cloud, digits = 1)) %"
	sz = s.bytes > 0 ? "  $(_s2_mb(s.bytes))" : ""
	_rs_label(io, "lbl_thumbinfo", "$(GMT.Dates.format(s.t0, "yyyy-mm-dd"))  $(_ls_sat(s))  path $(s.path) row $(lpad(s.row, 3, '0'))  $(cc)$(sz)")
	return nothing
end

# --- the band panel --------------------------------------------------------------------------------------

_ls_ticked(d::Dict{String,String}, bands)::Vector{Int} =
	[k for k in eachindex(bands) if k <= _LS_NSLOT && _rs_on(d, "p_band_" * lpad(k, 2, '0'))]

# The slots take the bands of the product: text, tooltip-free, enabled; the slots past them are emptied
# and locked. When the product changes the default bands are ticked (never on a mere refresh: the user's
# ticks stay).
function _ls_fill_bands!(io::IOBuffer, scene::Ptr{Cvoid}, d::Dict{String,String}, src::Int, prod::Int,
                         s::Union{LsScene,Nothing}, dir::String)
	sk = _LS_SRCKEYS[src]
	P = _LS_PRODUCTS[sk][clamp(prod, 1, length(_LS_PRODUCTS[sk]))]
	kind = (P[3], P[4])
	bands = _LS_BANDS[kind]
	fresh = get(_LS_LASTPROD, scene, (0, 0)) != (src, prod)
	_LS_LASTPROD[scene] = (src, prod)
	def = _LS_DEFAULT[kind]
	for k in 1:_LS_NSLOT
		w = "p_band_" * lpad(k, 2, '0')
		if k <= length(bands)
			_rs_label(io, w, bands[k][2]);  _rs_enable(io, w, s !== nothing)
			fresh && _rs_set(io, w, bands[k][1] in def ? 1 : 0)
		else
			_rs_label(io, w, "");  _rs_enable(io, w, false);  _rs_set(io, w, 0)
		end
	end
	_rs_enable(io, "run_bands", s !== nothing)
	if s === nothing
		_rs_label(io, "lbl_bsize", "")
		return nothing
	end
	ks = fresh ? [k for k in eachindex(bands) if bands[k][1] in def] : _ls_ticked(d, bands)
	box = _ls_box(d)
	todo = [k for k in ks if !isfile(_ls_band_target(s, bands[k][1], dir, box))]
	_rs_label(io, "lbl_bsize", isempty(ks) ? "no band ticked" :
	                           "$(length(ks)) band(s)" * (box === nothing ? ", whole scene: " : ", region only: ") *
	                           (isempty(todo) ? "on disk" : "$(length(todo)) new"))
	return nothing
end

# The region the bands are cut to: the Search tab's box when "Cut to the region" is ticked, else nothing.
function _ls_box(d::Dict{String,String})::Union{Nothing,NTuple{4,Float64}}
	_rs_on(d, "p_cut") || return nothing
	return try _s1_box(d) catch; nothing end
end

# Where a band lands: the whole file under its own name, or the region's window under a name that says
# which region (a window of another region is another file).
function _ls_band_target(s::LsScene, suffix::String, dir::String, box::Union{Nothing,NTuple{4,Float64}})::String
	box === nothing && return _ls_band_file(s, suffix, dir)
	tag = join((replace(string(round(v, digits = 4)), "-" => "m", "." => "p") for v in box), "_")
	return joinpath(_ls_band_dir(s, dir), "$(s.name)_$(suffix)_cut_$(tag).TIF")
end

# A remote GeoTIFF's extent and system, from its header only (GDAL reads a few kB over HTTP).
function _ls_remote_box(vsi::String)::Tuple{NTuple{4,Float64},String}
	info = GMT.gdalinfo(vsi)
	(info isa AbstractString && occursin("Size is", info)) || error("GDAL could not read the header of the remote file.")
	ms = match(r"Size is (\d+),\s*(\d+)", info)
	mo = match(r"Origin = \(([-\d.eE+]+),([-\d.eE+]+)\)", info)
	mp = match(r"Pixel Size = \(([-\d.eE+]+),([-\d.eE+]+)\)", info)
	ids = collect(eachmatch(r"ID\[\"EPSG\",(\d+)\]", info))
	(ms === nothing || mo === nothing || mp === nothing || isempty(ids)) &&
		error("The remote file's header has no georeferencing GDAL can read.")
	nx, ny = parse(Int, ms[1]), parse(Int, ms[2])
	x0, y0 = parse(Float64, mo[1]), parse(Float64, mo[2])
	dx, dy = parse(Float64, mp[1]), parse(Float64, mp[2])
	xs, ys = (x0, x0 + nx * dx), (y0, y0 + ny * dy)
	return (minimum(xs), maximum(xs), minimum(ys), maximum(ys)), "EPSG:" * ids[end][1]
end

# ONLY the region's window of a remote cloud-optimised GeoTIFF, read by GDAL over HTTP (range requests:
# the bytes of the tiles covering the window, never the whole file), written to `f`.
function _ls_window(url::String, f::String, box::NTuple{4,Float64}; extra::Vector{String} = String[])::Nothing
	vsi = "/vsicurl/" * url
	b, srs = _ls_remote_box(vsi)
	w, e, s, n = _box_to(_LONLAT, srs, box...)
	w, e, s, n = max(w, b[1]), min(e, b[2]), max(s, b[3]), min(n, b[4])
	(w < e && s < n) || error("The region does not reach $(basename(f)).")
	mkpath(dirname(f))
	part = f * ".part.tif"
	try
		GMT.gdaltranslate(vsi, vcat(["-projwin", string(w), string(n), string(e), string(s), "-co", "COMPRESS=DEFLATE"], extra);
		                  dest = part)
		isfile(part) || error("GDAL wrote nothing for $(basename(f)).")
		mv(part, f; force = true)
	finally
		isfile(part) && rm(part; force = true)
	end
	return nothing
end

# The ticked bands: the whole files, or with `box` ONLY the region's window of each (a file already there
# is not fetched again). (suffix, path) in panel order.
# The scene's metadata file <name>_MTL.txt (a few tens of kB) into its folder, beside the bands: the
# calibration coefficients (radiance and TOA reflectance gains and offsets, the thermal K1/K2, the sun
# elevation, the Level-2 surface-reflectance and temperature scales) that turn the bands' digital numbers
# into physical quantities. The cube writer (RS.cutcube) finds it there by the band's name, for whole
# bands and region-only ones alike, and stores it in the cube. Already there = not fetched again.
function _ls_fetch_mtl(s::LsScene, dir::String, d::Dict{String,String})::String
	fn = "$(s.name)_MTL.txt"
	f = joinpath(_ls_band_dir(s, dir), fn)
	isfile(f) && filesize(f) > 0 && return f
	text(h) = h[1] != UInt8('<') && h[1] != UInt8('{')       # an MTL, not an error page
	if s.src === :pc
		haskey(s.files, fn) || error("The Planetary Computer has no $(fn) for $(s.name).")
		_ls_get(_ls_pc_sign(s.files[fn]), f, Pair{String,String}[], text, _ -> nothing)
	else
		key = _ls_login(d)
		_, files = _ls_usgs_options(s, key)
		haskey(files, fn) || error("USGS offers no $(fn) for $(s.name). It offers: $(join(sort!(collect(keys(files))), ", "))")
		ent, pid = files[fn]
		_ls_get(_ls_usgs_url(ent, pid, key), f, Pair{String,String}[], text, _ -> nothing)
	end
	return f
end

function _ls_fetch_bands(s::LsScene, ks::Vector{Int}, dir::String, d::Dict{String,String},
                         box::Union{Nothing,NTuple{4,Float64}} = nothing)::Vector{Tuple{String,String}}
	bands = _ls_bands(s)
	out = [(bands[k][1], _ls_band_target(s, bands[k][1], dir, box)) for k in ks]
	todo = [(b, f) for (b, f) in out if !(isfile(f) && filesize(f) > 0)]
	_ls_fetch_mtl(s, dir, d)                    # the scene's metadata always goes beside its bands
	isempty(todo) && return out
	tif(h) = any(==(h), _LS_TIF_MAGIC)
	tl = Ref(0.0)
	_rs_prog_show((box === nothing ? "Downloading " : "Reading the region of ") * "$(length(todo)) band(s) of $(s.name)…")
	# One file: its whole bytes, or the window only.
	function one(i::Int, b::String, f::String, url::String)
		_rs_prog(round(Int, 100 * (i - 1) / length(todo)), (box === nothing ? "Downloading " : "Reading the region of ") *
		         "$(b) ($(i) of $(length(todo)))")
		if box === nothing
			prog(now) = time() - tl[] > 0.25 &&
				(tl[] = time(); _rs_prog(round(Int, 100 * (i - 1) / length(todo)), "Downloading $(b) ($(i) of $(length(todo))): $(_s2_mb(Float64(now)))"))
			_ls_get(url, f, Pair{String,String}[], tif, prog)
		else
			_ls_window(url, f, box)
		end
	end
	try
		if s.src === :pc
			for (i, (b, f)) in enumerate(todo)
				fn = "$(s.name)_$(b).TIF"
				haskey(s.files, fn) || error("The Planetary Computer has no file $(fn) for $(s.name).")
				one(i, b, f, _ls_pc_sign(s.files[fn]))
			end
		else
			key = _ls_login(d)
			_, files = _ls_usgs_options(s, key)
			for (i, (b, f)) in enumerate(todo)
				fn = "$(s.name)_$(b).TIF"
				haskey(files, fn) ||
					error("USGS offers no file $(fn) for $(s.name). It offers: $(join(sort!(collect(keys(files))), ", "))")
				ent, pid = files[fn]
				one(i, b, f, _ls_usgs_url(ent, pid, key))
			end
		end
	finally
		_rs_prog_close()
	end
	return out
end

# The whole scene: USGS's bundle (.tar); on the Planetary Computer every file of the item, one by one,
# into the scene's folder (the folder is answered).
function _ls_fetch_whole(s::LsScene, dir::String, d::Dict{String,String})::String
	if s.src === :pc
		bd = _ls_band_dir(s, dir)
		todo = sort!([n for n in keys(s.files) if !(isfile(joinpath(bd, n)) && filesize(joinpath(bd, n)) > 0)])
		tl = Ref(0.0)
		_rs_prog_show("Downloading the $(length(todo)) files of $(s.name)…")
		try
			for (i, n) in enumerate(todo)
				prog(now) = time() - tl[] > 0.25 &&
					(tl[] = time(); _rs_prog(round(Int, 100 * (i - 1) / length(todo)), "Downloading $(n) ($(i) of $(length(todo))): $(_s2_mb(Float64(now)))"))
				# a TIFF must be one; a text or XML file (MTL, ANG) only must not be an error page
				ok(h) = endswith(n, ".TIF") ? any(==(h), _LS_TIF_MAGIC) : !(h[1] == UInt8('<') && !endswith(n, ".xml"))
				_ls_get(_ls_pc_sign(s.files[n]), joinpath(bd, n), Pair{String,String}[], ok, prog)
			end
		finally
			_rs_prog_close()
		end
		return bd
	end
	f = _ls_whole_file(s, dir)
	isfile(f) && filesize(f) > 0 && return f
	key = _ls_login(d)
	whole, _ = _ls_usgs_options(s, key)
	whole === nothing && error("USGS offers no product bundle for $(s.name).")
	url = _ls_usgs_url(whole[1], whole[2], key)
	tl = Ref(0.0)
	_rs_prog_show("Downloading $(basename(f))…")
	try
		# a tar has no magic in its first bytes ("ustar" sits at 257): an error page starts with '<' or '{'
		_ls_get(url, f, Pair{String,String}[], h -> !(h[1] in (UInt8('<'), UInt8('{'))),
		        now -> time() - tl[] > 0.25 && (tl[] = time(); _rs_prog(0, "Downloading $(basename(f)): $(_s2_mb(Float64(now)))")))
	finally
		_rs_prog_close()
	end
	return f
end

# --- footprints ------------------------------------------------------------------------------------------
# ONE line element per SCENE, its own footprint as the source gives it (never one scene's outline standing
# for others of the same path/row), under one group named for the search; each is a guide line of this
# dialog ("Choose this scene (Landsat)" chooses that scene).
function _ls_plot_footprints!(scene::Ptr{Cvoid}, found::Vector{LsScene}, W::Float64, E::Float64, S::Float64, N::Float64)::Int
	_ls_remove_stack!(scene)                      # the browse stack belonged to the previous search
	old = pop!(_LS_GROUP, scene, "")
	isempty(old) || ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, old)
	delete!(_LS_FRAMES, scene);  delete!(_LS_PENS, scene);  delete!(_LS_LIT, scene)
	ccall(_fn(:gmtvtk_set_click_tool_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "landsat", "")
	isempty(found) && return 0
	frames = [(k, s) for (k, s) in enumerate(found) if !isempty(s.rings)]
	isempty(frames) && return 0
	_swot_map_if_empty(scene, W, E, S, N, "Landsat scenes")
	t0 = minimum(s.t0 for s in found);  t1 = maximum(s.t0 for s in found)
	nm = "Landsat scenes $(GMT.Dates.format(t0, "yyyy-mm-dd")) to $(GMT.Dates.format(t1, "yyyy-mm-dd"))"
	labels = Dict{String,Vector{Int}}();  pens = Dict{String,Vector{Float64}}()
	for (k, s) in frames
		lab = s.name                               # the scene's own name: one footprint, one scene
		ks = [k]
		D = Vector{GMTdataset{Float64,2}}()
		for M in s.rings
			L = copy(M)
			for i in 2:size(L, 1)
				while L[i, 1] - L[i-1, 1] > 180  L[i, 1] -= 360  end
				while L[i, 1] - L[i-1, 1] < -180 L[i, 1] += 360  end
			end
			Dk = GMT.mat2ds(L; hdr = lab);  Dk.proj4 = GMT.prj4WGS84
			push!(D, Dk)
		end
		_add_dataset_to_scene(scene, D, lab; groupName = nm, noConvertToPoints = true, forceMode = :lines) || continue
		ccall(_fn(:gmtvtk_overlay_set_tool_pick_h), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring, Cstring),
		      scene, lab, "landsat", "pickscene", "Choose this scene (Landsat)")
		labels[lab] = ks
		pen = zeros(6)
		ccall(_fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), scene, lab, pen) != 0 && (pens[lab] = pen)
	end
	_LS_GROUP[scene] = nm;  _LS_FRAMES[scene] = labels;  _LS_PENS[scene] = pens
	# A plain click inside a footprint now chooses its scene (GLView::mouseReleaseEvent -> "clickscene").
	ccall(_fn(:gmtvtk_set_click_tool_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "landsat", "clickscene")
	return length(labels)
end

function _ls_highlight!(scene::Ptr{Cvoid}, k::Int)
	lab = ""
	for (l, ks) in get(_LS_FRAMES, scene, Dict{String,Vector{Int}}())
		k in ks && (lab = l; break)
	end
	_rs_light_footprint!(scene, _LS_LIT, get(_LS_PENS, scene, Dict{String,Vector{Float64}}()), lab)
	# ...and in the browse stack, a thin red outline round the chosen scene's picture
	haskey(_LS_STACK, scene) &&
		ccall(_fn(:gmtvtk_sheet_select_h), Cint, (Ptr{Cvoid}, Cstring), scene, isempty(lab) ? "" : lab * " (quicklook)")
	return nothing
end

# --- the browse-picture stack ------------------------------------------------------------------------------
# THE stack of every scene tool (_rs_plot_stack!, s1products.jl). What is Landsat's own: a browse picture
# covers the scene's whole image GRID (the Planetary Computer item states it); the rows of one path on one
# day are one pass, side by side on one level.
const _LS_STACK = Dict{Ptr{Cvoid},String}()           # window -> group name of its browse stack

_ls_remove_stack!(scene::Ptr{Cvoid}) = _rs_remove_stack!(scene, _LS_STACK)

# The scene's image grid (EPSG, x0, x1, y0, y1): as stated, else the UTM box of its footprint in the zone of
# the footprint's centre (USGS gives no grid; a Landsat grid IS the UTM box of the imaged area).
function _ls_grid(s::LsScene)::NTuple{5,Float64}
	s.grid[1] > 0 && return s.grid
	isempty(s.rings) && return s.grid
	R = reduce(vcat, s.rings)
	lon = (minimum(R[:, 1]) + maximum(R[:, 1])) / 2;  lat = (minimum(R[:, 2]) + maximum(R[:, 2])) / 2
	epsg = (lat >= 0 ? 32600 : 32700) + clamp(floor(Int, (lon + 180) / 6) + 1, 1, 60)
	U = _srs_xy(R[:, 1:2], _LONLAT, "EPSG:$(epsg)")
	return (Float64(epsg), minimum(U[:, 1]), maximum(U[:, 1]), minimum(U[:, 2]), maximum(U[:, 2]))
end

function _ls_plot_stack!(scene::Ptr{Cvoid}, found::Vector{LsScene}, dir::String, d::Dict{String,String})::Int
	_ls_remove_stack!(scene)
	t0 = minimum(s.t0 for s in found);  t1 = maximum(s.t0 for s in found)
	g = "Landsat browse pictures $(GMT.Dates.format(t0, "yyyy-mm-dd")) to $(GMT.Dates.format(t1, "yyyy-mm-dd"))"
	sheets = RsSheet[]
	_rs_prog_show("Fetching $(length(found)) browse pictures…")
	try
		for (i, s) in enumerate(found)
			_rs_prog(round(Int, 100 * (i - 1) / length(found)), "Browse picture $(i) of $(length(found))")
			f = try _ls_quicklook(s, dir, d) catch; "" end
			gr = _ls_grid(s)
			gr[1] > 0 || continue
			day = GMT.Dates.format(s.t0, "yyyy-mm-dd")
			push!(sheets, RsSheet(s.name, s.t0, "$(first(s.name, 4))_$(s.path)_$(day)", "$(day)  path $(s.path)",
			                      Int(gr[1]), gr[2], gr[3], gr[4], gr[5], f))
		end
		return _rs_plot_stack!(scene, _LS_STACK, sheets, g, "landsat", get(_LS_LIT, scene, ""))
	finally
		_rs_prog_close()                            # only once the pictures are in: no dead time
	end
end

# --- the dialog --------------------------------------------------------------------------------------------

_ls_src(d::Dict{String,String})::Int = clamp(_rs_idx(d, "p_source") + 1, 1, 2)
_ls_prod(d::Dict{String,String}, src::Int)::Int = clamp(_rs_idx(d, "p_product") + 1, 1, length(_LS_PRODUCTS[_LS_SRCKEYS[src]]))

function _ls_folder(d::Dict{String,String})::String
	dir = _rs_get(d, "p_folder")
	return isempty(dir) ? _ls_dir() : dir
end

function _ls_label(s::LsScene, dir::String)::String
	cc = isnan(s.cloud) ? "?" : "$(round(Int, s.cloud))"
	sz = s.bytes > 0 ? "  $(_s2_mb(s.bytes))" : ""
	have = _ls_have_whole(s, dir) ? "  (on disk)" : ""
	return "$(GMT.Dates.format(s.t0, "yyyy-mm-dd"))  $(_ls_sat(s))  $(s.path)/$(lpad(s.row, 3, '0'))  cloud $(cc) %$(sz)$(have)"
end

function _ls_selected(scene::Ptr{Cvoid}, d::Dict{String,String})::LsScene
	found = get(_LS_FOUND, scene, LsScene[])
	isempty(found) && error("Find the scenes first.")
	k = _rs_idx(d, "p_granule") + 1
	(1 <= k <= length(found)) || error("Choose a scene in the list.")
	return found[k]
end

function _ls_choose!(io::IOBuffer, scene::Ptr{Cvoid}, d::Dict{String,String}, k::Int)
	found = get(_LS_FOUND, scene, LsScene[])
	(1 <= k <= length(found)) || return nothing
	s = found[k]
	_rs_set(io, "p_granule", k - 1)
	_ls_highlight!(scene, k)
	dir = _ls_folder(d)
	_ls_show_thumb!(io, s, dir, d)
	_ls_fill_bands!(io, scene, d, _ls_srcidx(s), s.prod, s, dir)
	return nothing
end

function _ls_forget!(scene::Ptr{Cvoid})
	old = pop!(_LS_GROUP, scene, "")
	isempty(old) || ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, old)
	_ls_remove_stack!(scene)
	delete!(_LS_FOUND, scene);  delete!(_LS_FRAMES, scene);  delete!(_LS_PENS, scene);  delete!(_LS_LIT, scene)
	ccall(_fn(:gmtvtk_set_click_tool_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "landsat", "")
	return nothing
end

function _ls_account_init!(io::IOBuffer)
	f = _rs_netrc_path()
	u, p, why = try
		(_rs_netrc_get(f, _LS_MACHINE)..., isfile(f) ? "" : "  (the file does not exist yet)")
	catch e
		("", "", "  — cannot read it: " * sprint(showerror, e))
	end
	_rs_set(io, "p_us_user", u);  _rs_set(io, "p_us_token", p)
	_rs_label(io, "lbl_us_where", "Saved in $(f) as machine $(_LS_MACHINE)$(why)")
	return nothing
end

function _rs_landsat(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	src = _ls_src(d)
	if what == "saveusgs"
		f = _rs_netrc_path()
		_rs_netrc_set!(f, _LS_MACHINE, _rs_get(d, "p_us_user"), _rs_get(d, "p_us_token"))
		_rs_label(io, "lbl_us_where", "Saved in $(f) as machine $(_LS_MACHINE)")
		_rs_status(io, "USGS login saved in $(f) (machine $(_LS_MACHINE)).")
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
		found = get(_LS_FOUND, scene, LsScene[])
		src = isempty(found) ? 1 : _ls_srcidx(found[1])
		prod = isempty(found) ? 1 : found[1].prod
		_rs_items(io, "p_source", _LS_SOURCES);  _rs_set(io, "p_source", src - 1)
		_rs_items(io, "p_product", String[p[1] for p in _LS_PRODUCTS[_LS_SRCKEYS[src]]]);  _rs_set(io, "p_product", prod - 1)
		_s1_lock_rgwindow(io, scene)
		today = GMT.Dates.Date(RS.now(RS.UTC))
		_rs_set(io, "p_from", "$(today - GMT.Dates.Day(30))T00:00:00")
		_rs_set(io, "p_to", "$(today)T00:00:00")
		_rs_set(io, "p_folder", _ls_dir())
		_ls_account_init!(io)
		delete!(_LS_LASTPROD, scene)
		_rs_items(io, "p_granule", String[_ls_label(x, _ls_dir()) for x in found])
		if isempty(found)
			_ls_show_thumb!(io, nothing, _ls_dir(), d)
			_ls_fill_bands!(io, scene, d, src, prod, nothing, _ls_dir())
		else
			_ls_choose!(io, scene, d, 1)
		end
		_rs_status(io, "Give the region, the period and the cloud limit, then Find the scenes. The Planetary Computer needs no " *
		               "account; USGS needs your ERS user name and M2M application token (Account tab).")
		return true
	end
	if what == "refresh"
		_s1_lock_rgwindow(io, scene)
		prod = _ls_prod(d, src)
		# A new source: its own product list (the list of scenes found is then stale).
		if get(_LS_LASTPROD, scene, (src, 0))[1] != src
			_rs_items(io, "p_product", String[p[1] for p in _LS_PRODUCTS[_LS_SRCKEYS[src]]])
			_rs_set(io, "p_product", 0)
			prod = 1
		end
		found = get(_LS_FOUND, scene, LsScene[])
		k = _rs_idx(d, "p_granule") + 1
		s = 1 <= k <= length(found) ? found[k] : nothing
		if s !== nothing && ((_ls_srcidx(s)) != src || s.prod != prod)
			_ls_forget!(scene)
			_rs_items(io, "p_granule", String[])
			_ls_show_thumb!(io, nothing, _ls_folder(d), d)
			_ls_fill_bands!(io, scene, d, src, prod, nothing, _ls_folder(d))
			_rs_status(io, "Source or product changed: Find the scenes again.")
			return true
		end
		s === nothing ? _ls_fill_bands!(io, scene, d, src, prod, nothing, _ls_folder(d)) : _ls_choose!(io, scene, d, k)
		return true
	end
	if what == "search"
		W, E, S, N = _s1_box(d)
		d0 = GMT.Dates.Date(DateTime(_rs_get(d, "p_from")))
		d1 = GMT.Dates.Date(DateTime(_rs_get(d, "p_to")))
		d0 <= d1 || error("The period ends before it starts.")
		cloud = clamp(round(Int, _rs_num(d, "p_cloud", 100.0)), 0, 100)
		prod = _ls_prod(d, src)
		pc = _LS_SRCKEYS[src] === :pc
		_rs_prog_show(pc ? "Asking the Planetary Computer for the Landsat scenes over the region…" :
		                   "Asking USGS for the Landsat scenes over the region…")
		found, more = try
			if pc
				_ls_pc_search(prod, W, E, S, N, d0, d1, cloud)
			else
				f, t = _ls_usgs_search(prod, W, E, S, N, d0, d1, cloud, d)
				(f, t > length(f))
			end
		finally
			_rs_prog_close()
		end
		_LS_FOUND[scene] = found
		dir = _ls_folder(d)
		_rs_items(io, "p_granule", String[_ls_label(x, dir) for x in found])
		nfr = _ls_plot_footprints!(scene, found, W, E, S, N)
		_s1_lock_rgwindow(io, scene)
		isempty(found) ? (_ls_show_thumb!(io, nothing, dir, d); _ls_fill_bands!(io, scene, d, src, prod, nothing, dir)) :
		                 _ls_choose!(io, scene, d, 1)
		_rs_status(io, isempty(found) ? "No $(_LS_PRODUCTS[_LS_SRCKEYS[src]][prod][1]) scene over the region in that period with at most $(cloud) % cloud." :
		               "$(length(found)) scene(s) on $(nfr) path/row(s), newest first." *
		               (more ? " The source has more: shorten the period to see the rest." : "") *
		               " Choose one in the list, or click inside its footprint in the view (or Pick a scene).")
		return true
	end
	# "pickscene" with a point: the armed "Pick a scene" button. "clickscene": a plain click in the view
	# (GLView), where a click outside every footprint is an ordinary click and is answered with nothing.
	if (what == "pickscene" || what == "clickscene") && !isempty(_rs_get(d, "px"))
		quiet = what == "clickscene"
		found = get(_LS_FOUND, scene, LsScene[])
		if isempty(found)
			quiet && return true
			error("Find the scenes first, then click inside one of the footprints it draws.")
		end
		x, y = _rs_num(d, "px", NaN), _rs_num(d, "py", NaN)
		srs = _window_srs(scene)
		if !isempty(srs) && !_project_is_geog(srs)
			Q = _srs_xy([x y], srs, _LONLAT);  x, y = Q[1, 1], Q[1, 2]
		end
		ks = _s1_covering(S1Product[_ls_s1product(s) for s in found], x, y)
		if isempty(ks)
			quiet && return true
			error("No scene found covers $(round(x, digits = 3)), $(round(y, digits = 3)): click inside a footprint.")
		end
		cur = _rs_idx(d, "p_granule") + 1
		j = findfirst(==(cur), ks)
		k = ks[j === nothing ? 1 : mod1(j + 1, length(ks))]
		_ls_choose!(io, scene, d, k)
		s = found[k]
		_rs_status(io, "Chose $(GMT.Dates.format(s.t0, "yyyy-mm-dd")) path $(s.path) row $(s.row)" *
		               (length(ks) > 1 ? ": $(length(ks)) scenes cover that point, click it again for the next." : "."))
		return true
	end
	if what == "pickscene"
		line = replace(_rs_get(d, "picked"), r" \(quicklook\)$" => "")     # a browse-picture sheet: its scene
		ks = get(get(_LS_FRAMES, scene, Dict{String,Vector{Int}}()), line, Int[])
		isempty(ks) && error("\"$(line)\" is not a Landsat footprint: Find the scenes, then click one of the footprints it draws.")
		_ls_choose!(io, scene, d, ks[1])
		s = _LS_FOUND[scene][ks[1]]
		_rs_status(io, "Chose $(s.name).")
		return true
	end
	if what == "stack"
		found = get(_LS_FOUND, scene, LsScene[])
		if _rs_toggle_stack!(scene, _LS_STACK)        # pressed again: the 3-D stack goes away
			_rs_status(io, "Browse-picture stack removed, the view put back as it was.")
			return true
		end
		isempty(found) && error("Find the scenes first.")
		n = _ls_plot_stack!(scene, found, _ls_folder(d), d)
		_rs_status(io, "$(n) browse pictures stacked one level per pass, oldest at the bottom. Click one to choose its scene.")
		return true
	end
	if what == "download"
		s = _ls_selected(scene, d)
		dir = _ls_folder(d)
		f = _ls_fetch_whole(s, dir, d)
		found = _LS_FOUND[scene]
		_rs_items(io, "p_granule", String[_ls_label(x, dir) for x in found])
		_ls_choose!(io, scene, d, findfirst(==(s), found))
		_rs_status(io, isdir(f) ? "$(length(s.files)) files of $(s.name) in $(f)." : "$(f) ($(_s2_mb(Float64(filesize(f))))).")
		return true
	end
	what == "bands" || error("unknown request: $what")
	s = _ls_selected(scene, d)
	dir = _ls_folder(d)
	bands = _ls_bands(s)
	ks = _ls_ticked(d, bands)
	isempty(ks) && error("Tick at least one band.")
	# With "Cut to the region" only the region's window of each band is fetched (GDAL range reads of the
	# cloud-optimised GeoTIFFs), never the whole scene; the files are then already the region.
	box = _ls_box(d)
	_rs_on(d, "p_cut") && box === nothing && error("Give the four limits of the region (W < E, S < N), or untick \"Cut to the region\".")
	files = _ls_fetch_bands(s, ks, dir, d, box)
	role = Dict(b[1] => b[4] for b in bands);  desc = Dict(b[1] => b[3] for b in bands)
	_ls_remove_stack!(scene)                       # the data takes the window: the 3-D browse stack goes, the view comes back
	shown = String[]
	# A band that cannot be a layer of the 30 m 16-bit cube is a grid of its own.
	for (b, f) in files
		role[b] === :own || continue
		_on_drop(scene, f)                         # the ONE open-a-file-into-a-window path
		push!(shown, basename(f))
	end
	cube = [(b, f) for (b, f) in files if role[b] === :cube]
	if !isempty(cube)
		tag = box === nothing ? "" : "_" * replace(basename(cube[1][2]), r"^.*_cut_" => "cut_", r"\.TIF$" => "")
		out = joinpath(_ls_band_dir(s, dir), "$(s.name)_$(join(first.(cube), '-'))$(tag).tiff")
		_rs_prog_show("Stacking $(length(cube)) band(s) into one cube…")
		try
			# cutcube reads each band with GMT's grdcut: a GMT door, so each name goes through it safe.
			RS.cutcube(names = String[_gmt_safe_path(f; quoted = false) for (_, f) in cube], region = nothing,
			           description = String[desc[b] for (b, _) in cube], save = out)
		finally
			_rs_prog_close()
		end
		isfile(out) || error("cutcube wrote nothing to $(out)")
		_on_drop(scene, out)                       # the ONE open-a-file-into-a-window path
		push!(shown, basename(out))
	end
	found = _LS_FOUND[scene]
	_rs_items(io, "p_granule", String[_ls_label(x, dir) for x in found])
	_ls_choose!(io, scene, d, findfirst(==(s), found))
	_rs_release_clicks!(scene, "landsat")
	_rs_status(io, "$(join(shown, " and ")) written in $(_ls_band_dir(s, dir)) and opened.")
	return true
end
