# ------------------------------------------------------------------------------------------------
# Geophysics > Copernicus > "Sentinel Hub imagery…" — a port of the QGIS SentinelHub plugin
# (github.com/sentinel-hub/sentinelhub-qgis-plugin, Sinergise) onto the iGMT dialog machinery.
# The C++ half is SentinelHubDialog (70_window.cpp), which loads deps/ui/sentinelhub_dialog.ui at
# runtime and talks to this file through gmtvtk_set_sentinelhub_callback with the newline-separated
# "key=value" block documented at JuliaSentinelHubFn (30_app.cpp).
#
# What the plugin does that QGIS gives it for free and we must do ourselves:
#   * QGIS adds the service as a LIVE WMS layer. iGMT has no streaming raster layer, so every
#     request here is a ONE-SHOT GetMap/GetCoverage that lands as a file and goes through the SHARED
#     file door (`_on_drop`) — the same door a drag-and-drop uses, so the raster laws in
#     SACRED_LAW.md (own axes, own Z, unconditional reframe) are satisfied by construction.
#   * The plugin keeps its credentials in the QGIS settings store. We keep them where the DGT LIDAR
#     tool keeps its own (`~/.dgt`): a plain-text file, `~/.sentinelhub`, read when the dialog's
#     boxes are empty.
#
# WMS version 1.1.1 is used deliberately, not the plugin's 1.3.0: 1.3.0 flips the axis order of
# EPSG:4326 to lat,lon, which is a bug farm for a bbox that everything else in iGMT writes as
# W/E/S/N. 1.1.1 takes `srs=` and minx,miny,maxx,maxy in every CRS.
# ------------------------------------------------------------------------------------------------

const _SHUB_OAUTH_CDSE = "https://identity.dataspace.copernicus.eu/auth/realms/CDSE/protocol/openid-connect/token"
const _SHUB_CDSE       = "https://sh.dataspace.copernicus.eu"
const _SHUB_START      = "1985-01-01"      # the plugin's DEFAULT_START_TIME: "since there was data"

# The live token: {access_token, expires_at}, plus the base URL it belongs to (a change of service
# invalidates it). Kept for the session only — never written anywhere.
const _SHUB_TOKEN  = Ref{String}("")
const _SHUB_EXPIRY = Ref{Float64}(0.0)
const _SHUB_BASE   = Ref{String}("")

# ── the credentials file ─────────────────────────────────────────────────────────────────────────
# Same shape and same contract as DGT's `~/.dgt`: two named lines, comments with '#', and the boxes
# in the dialog override it when they are filled.
_shub_credfile()::String = joinpath(homedir(), ".sentinelhub")

function _shub_read_credfile()::Tuple{String,String}
	f = _shub_credfile()
	isfile(f) || error("no credentials: type a client id and secret, or write them to $f as\n\n" *
	                   "client_id YOUR_ID\nclient_secret YOUR_SECRET")
	id, secret = "", ""
	for ln in eachline(f)
		s = strip(ln)
		(isempty(s) || startswith(s, '#')) && continue
		parts = split(s, r"[ \t=]+", limit = 2)
		length(parts) == 2 || continue
		k = lowercase(String(parts[1]))
		(k == "client_id" || k == "id") && (id = String(strip(parts[2])))
		(k == "client_secret" || k == "secret") && (secret = String(strip(parts[2])))
	end
	(isempty(id) || isempty(secret)) &&
		error("$f carries no 'client_id' and 'client_secret' pair")
	return (id, secret)
end

function _shub_write_credfile(id::String, secret::String)::String
	f = _shub_credfile()
	open(f, "w") do io
		println(io, "# Sentinel Hub / Copernicus Data Space OAuth client, used by InteractiveGMT")
		println(io, "client_id $id")
		println(io, "client_secret $secret")
	end
	return f
end

# The id/secret a request runs with: what the dialog typed, else the file. One function, so no
# caller can end up reading only half of the pair from each place.
function _shub_creds(d::Dict{String,String})::Tuple{String,String}
	id, secret = String(strip(_get(d, "id"))), String(strip(_get(d, "secret")))
	(isempty(id) || isempty(secret)) && return _shub_read_credfile()
	return (id, secret)
end

_shub_base(d::Dict{String,String})::String =
	let u = String(strip(_get(d, "url", _SHUB_CDSE))); rstrip(isempty(u) ? _SHUB_CDSE : u, '/') end

# The Copernicus deployment authenticates against Keycloak, every other one against the service's
# own /oauth/token. The plugin's `select_oauth_url`, verbatim.
_shub_oauth_url(base::String)::String = (base == _SHUB_CDSE) ? _SHUB_OAUTH_CDSE : base * "/oauth/token"

# ── HTTP ─────────────────────────────────────────────────────────────────────────────────────────
# Everything this tool fetches goes through these two, so a timeout, a proxy or an error message has
# ONE shape. `Downloads` is a stdlib and already a dependency (oceancolor.jl leans on it too).

function _shub_http_get(url::String; token::String = "", timeout::Real = 60)::String
	io = IOBuffer()
	hdrs = isempty(token) ? Pair{String,String}[] : ["Authorization" => "Bearer " * token]
	r = Downloads.request(url; output = io, headers = hdrs, timeout = timeout, throw = false)
	isa(r, Downloads.Response) || throw(r)
	body = String(take!(io))
	(200 <= r.status < 300) ||
		error("the service answered $(r.status) $(r.message) for\n$url\n\n$(first(body, 500))")
	return body
end

function _shub_http_post(url::String, form::String; timeout::Real = 60)::String
	io = IOBuffer()
	# Content-Length is written by hand: without it curl uploads the form chunked, which some OAuth
	# front ends refuse with a bare 411.
	hdrs = ["Content-Type" => "application/x-www-form-urlencoded",
	        "Content-Length" => string(sizeof(form))]
	r = Downloads.request(url; method = "POST", input = IOBuffer(form), output = io,
	                      headers = hdrs, timeout = timeout, throw = false)
	isa(r, Downloads.Response) || throw(r)
	body = String(take!(io))
	(200 <= r.status < 300) ||
		error("authentication failed: $(r.status) $(r.message)\n\n$(first(body, 500))")
	return body
end

# Percent-encode one query value. `Downloads` has no URL builder and the project has no HTTP.jl.
function _shub_esc(s::AbstractString)::String
	out = IOBuffer()
	for b in codeunits(String(s))
		c = Char(b)
		if (('A' <= c <= 'Z') || ('a' <= c <= 'z') || ('0' <= c <= '9') || c in ('-', '_', '.', '~'))
			write(out, c)
		else
			write(out, '%', uppercase(string(b, base = 16, pad = 2)))
		end
	end
	return String(take!(out))
end

_shub_url(base::String, params::Vector{Pair{String,String}})::String =
	base * "?" * join(("$k=$(_shub_esc(v))" for (k, v) in params), "&")

# ── the smallest JSON reader that answers the two questions this tool asks ───────────────────────
# The configuration API replies with an array of objects, and all this tool wants out of them is a
# couple of string fields. A real parser is 60 lines and cannot be fooled by a brace inside a name,
# which a regexp scan can — so it is a real parser.
function _shub_json(s::AbstractString)
	i = Ref(1)
	v = _shub_json_value(s, i)
	return v
end

function _shub_json_ws(s::AbstractString, i::Ref{Int})
	while i[] <= lastindex(s) && isspace(s[i[]]);  i[] = nextind(s, i[]);  end
end

function _shub_json_value(s::AbstractString, i::Ref{Int})
	_shub_json_ws(s, i)
	i[] > lastindex(s) && error("truncated JSON")
	c = s[i[]]
	c == '{' && return _shub_json_object(s, i)
	c == '[' && return _shub_json_array(s, i)
	c == '"' && return _shub_json_string(s, i)
	if startswith(SubString(s, i[]), "true");   i[] += 4;  return true;   end
	if startswith(SubString(s, i[]), "false");  i[] += 5;  return false;  end
	if startswith(SubString(s, i[]), "null");   i[] += 4;  return nothing; end
	j = i[]
	while j <= lastindex(s) && (isdigit(s[j]) || s[j] in ('-', '+', '.', 'e', 'E'));  j = nextind(s, j);  end
	num = tryparse(Float64, SubString(s, i[], prevind(s, j)))
	num === nothing && error("bad JSON value at byte $(i[])")
	i[] = j
	return num
end

function _shub_json_string(s::AbstractString, i::Ref{Int})::String
	i[] = nextind(s, i[])                      # the opening quote
	out = IOBuffer()
	while i[] <= lastindex(s)
		c = s[i[]]
		if c == '"'
			i[] = nextind(s, i[])
			return String(take!(out))
		elseif c == '\\'
			i[] = nextind(s, i[])
			e = s[i[]]
			if e == 'u'
				hex = SubString(s, nextind(s, i[]), i[] + 4)
				write(out, Char(parse(UInt16, hex, base = 16)))
				i[] += 4
			else
				write(out, e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' :
				           e == 'b' ? '\b' : e == 'f' ? '\f' : e)
			end
			i[] = nextind(s, i[])
		else
			write(out, c)
			i[] = nextind(s, i[])
		end
	end
	error("unterminated JSON string")
end

function _shub_json_object(s::AbstractString, i::Ref{Int})::Dict{String,Any}
	d = Dict{String,Any}()
	i[] = nextind(s, i[])                      # '{'
	_shub_json_ws(s, i)
	if i[] <= lastindex(s) && s[i[]] == '}';  i[] = nextind(s, i[]);  return d;  end
	while true
		_shub_json_ws(s, i)
		k = _shub_json_string(s, i)
		_shub_json_ws(s, i)
		s[i[]] == ':' || error("expected ':' in JSON object")
		i[] = nextind(s, i[])
		d[k] = _shub_json_value(s, i)
		_shub_json_ws(s, i)
		i[] > lastindex(s) && error("truncated JSON object")
		if s[i[]] == ','
			i[] = nextind(s, i[])
		else
			s[i[]] == '}' || error("expected ',' or '}' in JSON object")
			i[] = nextind(s, i[])
			return d
		end
	end
end

function _shub_json_array(s::AbstractString, i::Ref{Int})::Vector{Any}
	v = Any[]
	i[] = nextind(s, i[])                      # '['
	_shub_json_ws(s, i)
	if i[] <= lastindex(s) && s[i[]] == ']';  i[] = nextind(s, i[]);  return v;  end
	while true
		push!(v, _shub_json_value(s, i))
		_shub_json_ws(s, i)
		i[] > lastindex(s) && error("truncated JSON array")
		if s[i[]] == ','
			i[] = nextind(s, i[])
		else
			s[i[]] == ']' || error("expected ',' or ']' in JSON array")
			i[] = nextind(s, i[])
			return v
		end
	end
end

_shub_str(o, k::String, default::String = "")::String =
	(isa(o, Dict) && haskey(o, k) && isa(o[k], String)) ? o[k]::String : default

# ── the built-in collections: the Process API, for an account with no configuration ──────────────
# The QGIS plugin can only ask the OGC endpoints, and those need a CONFIGURATION INSTANCE made by
# hand in the Copernicus dashboard — an account that has none (which is what a fresh OAuth client
# has) gets an empty layer list and nothing to press. The Process API needs no such thing: the
# request carries its own evalscript, so an account with nothing but its OAuth client can ask for an
# image. These entries are therefore offered ALONGSIDE whatever configurations the account has, and
# the `proc:` prefix on a configuration id is what tells the request builder which door to use.
#
# Only collections whose band names are taken from the Copernicus documentation are listed. A sensor
# whose bands would have to be guessed is not here — it is reachable through a configuration of your
# own, which is exactly what configurations are for.
const _SHUB_RENDER = Dict{String,Vector{Tuple{String,String,Int,String}}}(
	# collection => (rendering id, nice name, band count, evalscript)
	"sentinel-2-l2a" => [
		("true",  "True colour (B04 B03 B02)", 3, """//VERSION=3
function setup(){return {input:["B04","B03","B02"],output:{bands:3}};}
function evaluatePixel(s){return [2.5*s.B04,2.5*s.B03,2.5*s.B02];}"""),
		("false", "False colour (B08 B04 B03)", 3, """//VERSION=3
function setup(){return {input:["B08","B04","B03"],output:{bands:3}};}
function evaluatePixel(s){return [2.5*s.B08,2.5*s.B04,2.5*s.B03];}"""),
		("swir",  "SWIR (B12 B8A B04)", 3, """//VERSION=3
function setup(){return {input:["B12","B8A","B04"],output:{bands:3}};}
function evaluatePixel(s){return [2.5*s.B12,2.5*s.B8A,2.5*s.B04];}"""),
		("ndvi",  "NDVI (a grid, not a picture)", 1, """//VERSION=3
function setup(){return {input:["B08","B04"],output:{bands:1,sampleType:"FLOAT32"}};}
function evaluatePixel(s){return [(s.B08-s.B04)/(s.B08+s.B04)];}"""),
	],
	"sentinel-2-l1c" => [
		("true",  "True colour (B04 B03 B02)", 3, """//VERSION=3
function setup(){return {input:["B04","B03","B02"],output:{bands:3}};}
function evaluatePixel(s){return [2.5*s.B04,2.5*s.B03,2.5*s.B02];}"""),
		("false", "False colour (B08 B04 B03)", 3, """//VERSION=3
function setup(){return {input:["B08","B04","B03"],output:{bands:3}};}
function evaluatePixel(s){return [2.5*s.B08,2.5*s.B04,2.5*s.B03];}"""),
	],
	"sentinel-1-grd" => [
		("vv",    "VV backscatter (a grid)", 1, """//VERSION=3
function setup(){return {input:["VV"],output:{bands:1,sampleType:"FLOAT32"}};}
function evaluatePixel(s){return [s.VV];}"""),
		("vvvh",  "VV / VH false colour", 3, """//VERSION=3
function setup(){return {input:["VV","VH"],output:{bands:3}};}
function evaluatePixel(s){return [2.5*s.VV,2.5*s.VH,0.1*s.VV/Math.max(s.VH,1e-6)];}"""),
	],
	"dem" => [
		("height", "Height (a grid)", 1, """//VERSION=3
function setup(){return {input:["DEM"],output:{bands:1,sampleType:"FLOAT32"}};}
function evaluatePixel(s){return [s.DEM];}"""),
	],
)

# Shown in the Configuration combo above whatever the account owns, in this order.
const _SHUB_COLLECTIONS = [("sentinel-2-l2a", "Sentinel-2 L2A (built in)"),
                           ("sentinel-2-l1c", "Sentinel-2 L1C (built in)"),
                           ("sentinel-1-grd", "Sentinel-1 GRD (built in)"),
                           ("dem",            "Copernicus DEM (built in)")]

_shub_is_proc(config::AbstractString)::Bool = startswith(config, "proc:")
_shub_collection(config::AbstractString)::String = String(chopprefix(String(config), "proc:"))

function _shub_renderings(collection::String)::String
	r = get(_SHUB_RENDER, collection, nothing)
	r === nothing && error("no built-in renderings for '$collection'")
	return join(("$(x[1])\t$(x[2])" for x in r), '\n')
end

function _shub_evalscript(collection::String, rendering::String)::String
	for x in get(_SHUB_RENDER, collection, Tuple{String,String,Int,String}[])
		x[1] == rendering && return x[4]
	end
	error("'$rendering' is not one of the built-in renderings of $collection")
end

# ── the session ──────────────────────────────────────────────────────────────────────────────────
# The plugin's `Session`: one token, refreshed a minute before it expires. Same rule here, and the
# base URL is part of the identity — pointing the dialog at the other deployment must not keep using
# the token the first one handed out.
function _shub_token(base::String, id::String, secret::String)::String
	if (!isempty(_SHUB_TOKEN[]) && _SHUB_BASE[] == base && _SHUB_EXPIRY[] > time() + 60)
		return _SHUB_TOKEN[]
	end
	form = "grant_type=client_credentials&client_id=$(_shub_esc(id))&client_secret=$(_shub_esc(secret))"
	js = _shub_json(_shub_http_post(_shub_oauth_url(base), form))
	tok = _shub_str(js, "access_token")
	isempty(tok) && error("the service returned no access token")
	life = (isa(js, Dict) && isa(get(js, "expires_in", nothing), Float64)) ? js["expires_in"]::Float64 : 600.0
	_SHUB_TOKEN[] = tok;  _SHUB_EXPIRY[] = time() + life;  _SHUB_BASE[] = base
	return tok
end

# The two configuration-API endpoints. Copernicus moved them to /api/v2/configuration; the classic
# Sentinel Hub deployments still answer on /configuration/v1/wms. The plugin carries the same fork.
_shub_conf_url(base::String)::String =
	(base == _SHUB_CDSE) ? base * "/api/v2/configuration/instances" : base * "/configuration/v1/wms/instances"

_shub_layers_url(base::String, instance::String)::String =
	(base == _SHUB_CDSE) ? base * "/api/v2/configuration/instances/$instance/layers" :
	                       base * "/configuration/v1/wms/instances/$instance/layers"

# "id\tname" lines, sorted by name, which is what both combos in the dialog are filled from.
function _shub_catalog(url::String, token::String)::String
	js = _shub_json(_shub_http_get(url; token = token))
	isa(js, Vector) || error("the configuration API did not answer with a list")
	rows = Tuple{String,String}[]
	for o in js
		id = _shub_str(o, "id")
		isempty(id) && continue
		nm = _shub_str(o, "name", _shub_str(o, "title", id))
		push!(rows, (id, nm))
	end
	sort!(rows, by = r -> lowercase(r[2]))
	return join(("$(r[1])\t$(r[2])" for r in rows), '\n')
end

# ── the request ──────────────────────────────────────────────────────────────────────────────────
# The plugin's `_build_time`: an empty range means "no time filter at all", an exact date means a
# one-day window, and a half-open range is closed with the default start or with today.
function _shub_time(d::Dict{String,String})::String
	t0, t1 = String(strip(_get(d, "t0"))), String(strip(_get(d, "t1")))
	exact  = _on(d, "exact")
	(exact && isempty(t0)) && return ""
	(isempty(t0) && isempty(t1)) && return ""
	exact && (t1 = t0)
	isempty(t0) && (t0 = _SHUB_START)
	isempty(t1) && (t1 = string(GMT.Dates.today()))
	return "$t0/$t1/P1D"
end

# W/E/S/N as the dialog holds it, in the CRS the request will be made in. iGMT's boxes are always
# degrees (the standing rule that goes with addRefGridRow), so a projected CRS is reached through
# GMT's own transform — never through a Mercator formula written out here.
function _shub_bbox(d::Dict{String,String}, crs::String)::NTuple{4,Float64}
	num(k) = let s = String(strip(_get(d, k)))
		v = tryparse(Float64, s)
		v === nothing && error("the region box '$k' does not hold a number: '$s'")
		v
	end
	w, e, s, n = num("x0"), num("x1"), num("y0"), num("y1")
	(e > w && n > s) || error("the region is empty — west < east and south < north, please")
	(crs == "EPSG:4326" || isempty(crs)) && return (w, s, e, n)
	xy = GMT.lonlat2xy([w s; e n], t_srs = crs)
	return (xy[1, 1], xy[1, 2], xy[2, 1], xy[2, 2])
end

# The pixel count of a GetMap. The service caps a single request at 2500x2500, so the long side is
# clamped there and the short one follows the bbox's own aspect: an image that comes back stretched
# is worse than one that comes back smaller.
function _shub_size(d::Dict{String,String}, bb::NTuple{4,Float64})::Tuple{Int,Int}
	want = something(tryparse(Int, String(strip(_get(d, "size", "1024")))), 1024)
	want = clamp(want, 64, 2500)
	dx, dy = bb[3] - bb[1], bb[4] - bb[2]
	(dx > 0 && dy > 0) || error("the region is empty")
	return dx >= dy ? (want, max(1, round(Int, want * dy / dx))) :
	                  (max(1, round(Int, want * dx / dy)), want)
end

_shub_ogc(base::String, service::String, instance::String)::String =
	base * "/ogc/" * lowercase(service) * "/" * instance

const _SHUB_MIME = Dict("PNG" => "image/png", "JPEG" => "image/jpeg", "TIFF" => "image/tiff")
const _SHUB_EXT  = Dict("PNG" => ".png",      "JPEG" => ".jpg",       "TIFF" => ".tif")

# A GetMap, WMS 1.1.1 (see the header comment on why not 1.3.0).
function _shub_getmap_url(d::Dict{String,String}, base::String, instance::String, layer::String,
                          crs::String, bb::NTuple{4,Float64}, wid::Int, hei::Int, mime::String)::String
	p = ["service" => "WMS", "request" => "GetMap", "version" => "1.1.1",
	     "layers" => layer, "styles" => "", "srs" => crs,
	     "bbox" => join((bb[1], bb[2], bb[3], bb[4]), ','),
	     "width" => string(wid), "height" => string(hei),
	     "format" => mime, "transparent" => "false",
	     "priority" => _get(d, "priority", "mostRecent"),
	     "maxcc" => _get(d, "maxcc", "100"),
	     "showLogo" => _on(d, "logo") ? "true" : "false"]
	t = _shub_time(d)
	isempty(t) || push!(p, "time" => t)
	return _shub_url(_shub_ogc(base, "wms", instance), p)
end

# The plugin's `get_wcs_url`: the download path, where the user asks for a ground resolution instead
# of a pixel count.
function _shub_wcs_url(d::Dict{String,String}, base::String, instance::String, layer::String,
                       crs::String, bb::NTuple{4,Float64}, mime::String)::String
	resx = String(strip(_get(d, "resx")));  resy = String(strip(_get(d, "resy")))
	(isempty(resx) || isempty(resy)) && error("give me both resolutions (X and Y, in metres)")
	p = ["service" => "wcs", "request" => "GetCoverage", "version" => "1.1.2",
	     "coverage" => layer, "bbox" => join((bb[1], bb[2], bb[3], bb[4]), ','), "crs" => crs,
	     "format" => mime, "resx" => resx * "m", "resy" => resy * "m",
	     "priority" => _get(d, "priority", "mostRecent"),
	     "maxcc" => _get(d, "maxcc", "100"),
	     "showLogo" => _on(d, "logo") ? "true" : "false", "transparent" => "false"]
	t = _shub_time(d)
	isempty(t) || push!(p, "time" => t)
	return _shub_url(_shub_ogc(base, "wcs", instance), p)
end

# ── the Process API ──────────────────────────────────────────────────────────────────────────────
# One POST, one image back. The request carries the evalscript, so it needs no configuration
# instance — which is the whole reason this path exists (see _SHUB_RENDER above).

# The time range as the Process API wants it: two instants, not the OGC "from/to/P1D" string.
function _shub_range(d::Dict{String,String})
	t0, t1 = String(strip(_get(d, "t0"))), String(strip(_get(d, "t1")))
	exact = _on(d, "exact")
	exact && !isempty(t0) && (t1 = t0)
	(isempty(t0) && isempty(t1)) && return nothing
	isempty(t0) && (t0 = _SHUB_START)
	isempty(t1) && (t1 = string(GMT.Dates.today()))
	return (t0 * "T00:00:00Z", t1 * "T23:59:59Z")
end

# Escape one string into a JSON literal (the evalscripts are multi-line, so this is not optional).
function _shub_jstr(s::AbstractString)::String
	out = IOBuffer()
	write(out, '"')
	for c in s
		c == '"'  ? write(out, "\\\"") :
		c == '\\' ? write(out, "\\\\") :
		c == '\n' ? write(out, "\\n")  :
		c == '\r' ? write(out, "\\r")  :
		c == '\t' ? write(out, "\\t")  :
		c < ' '   ? write(out, "\\u", string(UInt16(c), base = 16, pad = 4)) : write(out, c)
	end
	write(out, '"')
	return String(take!(out))
end

_shub_crs_uri(crs::String)::String =
	"http://www.opengis.net/def/crs/EPSG/0/" * (isempty(crs) ? "4326" : String(split(crs, ':')[end]))

function _shub_process_body(d::Dict{String,String}, collection::String, evalscript::String,
                            crs::String, bb::NTuple{4,Float64}, mime::String,
                            wid::Int, hei::Int, resx::String, resy::String)::String
	tr = _shub_range(d)
	filt = String[]
	# The DEM is not a time series and carries no cloud cover: asking it for either is an error, not
	# a harmless extra.
	if collection != "dem"
		tr === nothing || push!(filt, "\"timeRange\":{\"from\":$(_shub_jstr(tr[1])),\"to\":$(_shub_jstr(tr[2]))}")
		push!(filt, "\"maxCloudCoverage\":$(_get(d, "maxcc", "100"))")
		push!(filt, "\"mosaickingOrder\":$(_shub_jstr(_get(d, "priority", "mostRecent")))")
	end
	data = "{\"type\":$(_shub_jstr(collection))" *
	       (isempty(filt) ? "" : ",\"dataFilter\":{" * join(filt, ',') * "}") * "}"
	# Either a pixel count (the Image tab) or a ground resolution (the Download tab) — never both,
	# which the service rejects.
	sz = isempty(resx) ? "\"width\":$wid,\"height\":$hei" : "\"resx\":$resx,\"resy\":$resy"
	return "{\"input\":{\"bounds\":{\"bbox\":[$(bb[1]),$(bb[2]),$(bb[3]),$(bb[4])]," *
	       "\"properties\":{\"crs\":$(_shub_jstr(_shub_crs_uri(crs)))}},\"data\":[$data]}," *
	       "\"output\":{$sz,\"responses\":[{\"identifier\":\"default\",\"format\":{\"type\":$(_shub_jstr(mime))}}]}," *
	       "\"evalscript\":$(_shub_evalscript_json(evalscript))}"
end

_shub_evalscript_json(s::String)::String = _shub_jstr(s)

_shub_process_url(base::String)::String = base * "/api/v1/process"

# POST the request and write the image the service answers with straight to `target`.
function _shub_process_to_file(url::String, body::String, token::String, target::String)::Nothing
	io = open(target, "w")
	r = try
		Downloads.request(url; method = "POST", input = IOBuffer(body), output = io, timeout = 600,
		                  throw = false,
		                  headers = ["Authorization" => "Bearer " * token,
		                             "Content-Type" => "application/json",
		                             "Accept" => "*/*",
		                             "Content-Length" => string(sizeof(body))])
	finally
		close(io)
	end
	isa(r, Downloads.Response) || (rm(target, force = true); throw(r))
	if !(200 <= r.status < 300)
		txt = try first(read(target, String), 800) catch; "" end
		rm(target, force = true)
		error("the Process API answered $(r.status) $(r.message)\n\n$txt")
	end
	(filesize(target) > 0) || (rm(target, force = true); error("the service sent an empty image"))
	return nothing
end

# PNG and JPEG come back as bare pixels — no CRS, no corners — and a raster with no georeference is
# not a map. Rather than write a world file (which only half the readers honour, and which says
# nothing about the CRS), the corners are stamped into a GeoTIFF copy with GDAL, and THAT is what
# the window is given. A TIFF from the service is already a GeoTIFF and is left alone.
function _shub_georef(path::String, crs::String, bb::NTuple{4,Float64})::String
	endswith(lowercase(path), ".tif") && return path
	out = first(path, length(path) - length(splitext(path)[2])) * ".tif"
	GMT.gdaltranslate(path, ["-a_srs", isempty(crs) ? "EPSG:4326" : crs,
	                         "-a_ullr", string(bb[1]), string(bb[4]), string(bb[3]), string(bb[2])];
	                  dest = out)
	return out
end

# Where an unnamed image lands, and under what name. The name is a FUNCTION OF THE REQUEST (layer,
# dates, bbox), so asking twice for the same thing finds the first answer on disk — same contract as
# `_ecmwf_autoname`.
function _shub_destdir(folder::String)::String
	isempty(folder) || return folder
	dir = joinpath(tempdir(), "iGMT_sentinelhub")
	isdir(dir) || mkpath(dir)
	return dir
end

function _shub_autoname(d::Dict{String,String}, layer::String, bb::NTuple{4,Float64}, fmt::String)::String
	clean(s) = replace(String(s), r"[^A-Za-z0-9_.-]" => "_")
	t = replace(_shub_time(d), "/" => "_")
	isempty(t) && (t = "anytime")
	tag = string(hash((bb, _get(d, "crs"), _get(d, "maxcc"), _get(d, "priority"))), base = 16)
	return clean(layer) * "_" * clean(t) * "_" * tag * get(_SHUB_EXT, fmt, ".tif")
end

# ── the C callback ───────────────────────────────────────────────────────────────────────────────
# Same split as `_on_ecmwf`, for the same reason: the light branches (a login, the two catalogues)
# must not drag the image path — `_on_drop`, and with it every grid reader and the whole file door —
# into inference. The heavy branch is reached through `invokelatest`, where inference stops.
function _on_sentinelhub(scene::Ptr{Cvoid}, cparams::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		what = _get(d, "what", "getimage")
		if what == "login"
			id, secret = "", ""
			typed_id, typed_secret = String(strip(_get(d, "id"))), String(strip(_get(d, "secret")))
			msg = IOBuffer()
			if (!isempty(typed_id) && !isempty(typed_secret))
				id, secret = typed_id, typed_secret
				# The file is written BEFORE the login is tried, and the result of the try is reported
				# separately: a user whose credentials are right but whose network is down must not have
				# to type them again next time.
				_on(d, "savecred") && println(msg, "wrote ", _shub_write_credfile(id, secret))
			else
				id, secret = _shub_read_credfile()
				println(msg, "read ", _shub_credfile(), " (client id ", id, ")")
			end
			base = _shub_base(d)
			_shub_token(base, id, secret)
			println(msg, "logged in at ", base, " — the configuration list is available")
			_shub_reply(out, cap, String(take!(msg)))
			return Cint(1)
		end
		# What the dialog shows GREYED in its two empty credential boxes: the client id already on
		# disk, or nothing at all. Asked for rather than rebuilt on the C++ side, so the file's name
		# and format stay known in ONE place. No network, so the dialog can ask it while opening.
		if what == "credinfo"
			id = try first(_shub_read_credfile()) catch; "" end
			_shub_reply(out, cap, id)
			return Cint(1)
		end
		# The built-in collections FIRST, then whatever configurations the account owns. A fresh OAuth
		# client owns none — the dashboard is where those are made — and that must leave a usable
		# dialog, not an empty combo, which is what the Process API entries are for.
		if what == "configs"
			base = _shub_base(d);  id, secret = _shub_creds(d)
			rows = ["proc:$(c[1])\t$(c[2])" for c in _SHUB_COLLECTIONS]
			own = try
				_shub_catalog(_shub_conf_url(base), _shub_token(base, id, secret))
			catch e
				# The account's own list is a bonus here, not the point: a service that refuses it must
				# not take the built-in collections down with it.
				""
			end
			isempty(strip(own)) || append!(rows, split(own, '\n'))
			_shub_reply(out, cap, join(rows, '\n'))
			return Cint(1)
		end
		if what == "layers"
			inst = String(strip(_get(d, "config")))
			isempty(inst) && error("no configuration chosen")
			_shub_is_proc(inst) && (_shub_reply(out, cap, _shub_renderings(_shub_collection(inst))); return Cint(1))
			base = _shub_base(d);  id, secret = _shub_creds(d)
			_shub_reply(out, cap, _shub_catalog(_shub_layers_url(base, inst), _shub_token(base, id, secret)))
			return Cint(1)
		end
		return Base.invokelatest(_shub_fetch, scene, d, out, cap)::Cint
	catch e
		_tool_failed(scene, "Sentinel Hub", e)
		_shub_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# The two buttons that bring an image back — "Get image" (a GetMap sized in pixels) and "Download"
# (a GetCoverage sized in metres). ONE function: they differ in the URL builder and in where the
# file is kept, never in how the request is assembled or how the result reaches the window.
function _shub_fetch(scene::Ptr{Cvoid}, d::Dict{String,String}, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		what   = _get(d, "what", "getimage")
		base   = _shub_base(d)
		inst   = String(strip(_get(d, "config")))
		layer  = String(strip(_get(d, "layer")))
		isempty(inst)  && error("no configuration chosen — press Login first, then pick one")
		isempty(layer) && error("no layer chosen")
		crs    = String(strip(_get(d, "crs", "EPSG:4326")))
		fmt    = uppercase(String(strip(_get(d, "fmt", "TIFF"))))
		haskey(_SHUB_MIME, fmt) || error("unknown image format '$fmt'")
		bb     = _shub_bbox(d, crs)
		folder = String(strip(_get(d, "folder")))
		dir    = _shub_destdir(folder)
		isdir(dir) || mkpath(dir)

		# "Get image" is a look at the data, so it comes back as a GeoTIFF whatever the Download tab's
		# format says — a PNG would have to be re-stamped with its corners anyway.
		(what == "getimage") && (fmt = "TIFF")
		target = joinpath(dir, _shub_autoname(d, inst * "_" * layer, bb, fmt))

		# Asked for before and still on disk: the service is not asked again.
		if !isfile(target)
			id, secret = _shub_creds(d)
			tok = _shub_token(base, id, secret)
			if _shub_is_proc(inst)
				# A built-in collection: the Process API, which needs no configuration instance.
				coll = _shub_collection(inst)
				wid, hei = _shub_size(d, bb)
				resx = (what == "download") ? String(strip(_get(d, "resx"))) : ""
				resy = (what == "download") ? String(strip(_get(d, "resy"))) : ""
				(what == "download" && (isempty(resx) || isempty(resy))) &&
					error("give me both resolutions (X and Y, in metres)")
				body = _shub_process_body(d, coll, _shub_evalscript(coll, layer), crs, bb,
				                          _SHUB_MIME[fmt], wid, hei, resx, resy)
				_shub_process_to_file(_shub_process_url(base), body, tok, target)
			else
				# A configuration of the account's own: the OGC endpoints, exactly as the QGIS plugin
				# asks them.
				url = if what == "download"
					_shub_wcs_url(d, base, inst, layer, crs, bb, _SHUB_MIME[fmt])
				else
					wid, hei = _shub_size(d, bb)
					_shub_getmap_url(d, base, inst, layer, crs, bb, wid, hei, _SHUB_MIME[fmt])
				end
				io = open(target, "w")
				r = try
					Downloads.request(url; output = io, timeout = 600, throw = false,
					                  headers = ["Authorization" => "Bearer " * tok])
				finally
					close(io)
				end
				isa(r, Downloads.Response) || (rm(target, force = true); throw(r))
				if !(200 <= r.status < 300)
					txt = try first(read(target, String), 500) catch; "" end
					rm(target, force = true)
					error("the service answered $(r.status) $(r.message)\n\n$txt")
				end
				(filesize(target) > 0) || (rm(target, force = true); error("the service sent an empty image"))
			end
		end
		target = _shub_georef(target, crs, bb)

		# THE shared file door, the same one a drag-and-drop goes through — which is where the raster
		# laws (own axes, own Z, unconditional reframe) are applied (SACRED_LAW.md).
		_on(d, "load") && _on_drop(scene, target)
		_shub_reply(out, cap, target)
		return Cint(1)
	catch e
		_tool_failed(scene, "Sentinel Hub", e)
		_shub_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# Copy `txt` into the C buffer, NUL-terminated and never past `cap`.
function _shub_reply(out::Ptr{UInt8}, cap::Cint, txt::String)::Nothing
	(out == C_NULL || cap <= 0) && return nothing
	b = codeunits(txt)
	n = min(length(b), Int(cap) - 1)
	GC.@preserve b unsafe_copyto!(out, pointer(b), n)
	unsafe_store!(out, UInt8(0), n + 1)
	return nothing
end

function _register_sentinelhub()
	fptr = @cfunction((s, c, o, n) -> Base.invokelatest(_on_sentinelhub, s, c, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_sentinelhub_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
