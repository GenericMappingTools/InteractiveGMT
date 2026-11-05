# s1products.jl — Satellite > Copernicus > "Sentinel-1 SAR products…": the Sentinel-1 product FILES
# (the .SAFE archives ESA distributes: GRD, SLC, OCN, RAW) found in the Copernicus Data Space catalogue
# over a region and a period, downloaded, and — for a GRD — shown in the window as a grid. One more
# tool of the Remote sensing dialog family (RemoteSDialog, 70_window.cpp, over deps/ui/remotes_s1.ui):
# same wire protocol, same .netrc account (`_rs_netrc_*`), same progress dialog (`_rs_prog_*`), same
# delivery (`_rs_deliver`), same "map for an empty window" (`_swot_map_if_empty`).
#
# Not "Sentinel Hub imagery" (sentinelhub.jl): that one asks the Process API for a picture RESAMPLED
# to a region, with an OAuth CLIENT (client_credentials). The product files are a different door with
# a different login, both checked live 2026-11-05:
#   * search: the OData catalogue, no login at all;
#   * download: download.dataspace.copernicus.eu, Bearer token from the SAME Keycloak endpoint as
#     Sentinel Hub (`_SHUB_OAUTH_CDSE`) but with the USER's e-mail and password (client_id cdse-public,
#     grant_type password, plus `totp` on an account with two-factor login). Without a token it is a 401.
#     (zipper.dataspace… answers 401 as well; catalogue…/$value is a 301 to download…, which would lose
#     the Authorization header on the way — so download… is asked directly.)
#
# The filters were checked against the live catalogue: `contains(Name, …)` selects mode and product
# (a name is S1C_IW_GRDH_1SDV_…, a COG GRD ends in _COG.SAFE and its productType is "IW_GRDH_1S-COG"),
# and the orbit direction / relative orbit are the StringAttribute "orbitDirection" and the
# IntegerAttribute "relativeOrbitNumber".

const _S1_ODATA    = "https://catalogue.dataspace.copernicus.eu/odata/v1/Products"
const _S1_DOWNLOAD = "https://download.dataspace.copernicus.eu/odata/v1/Products"
const _S1_MACHINE  = "identity.dataspace.copernicus.eu"     # the .netrc entry of the CDSE login
const _S1_TOP      = 1000                                    # the catalogue's largest page

# The Product combo: (text, the piece of the name after the mode, kind).
const _S1_PRODUCTS = Tuple{String,String,Symbol}[
	("GRD: ground range, detected (Level-1)",              "GRD",  :grd),
	("GRD COG: the same as cloud-optimised GeoTIFFs",      "GRD",  :cog),
	("SLC: single look complex (Level-1, for InSAR)",      "SLC_", :slc),
	("OCN: ocean wind, waves and currents (Level-2)",      "OCN_", :ocn),
	("RAW: the raw signal (Level-0)",                      "RAW_", :raw),
]
const _S1_MODES = ["IW", "EW", "SM", "WV"]
const _S1_POLS  = ["VV", "VH", "HH", "HV"]

struct S1Product
	id::String                       # the catalogue's product id (the download address)
	name::String                     # S1C_IW_GRDH_1SDV_…_35DA.SAFE
	t0::DateTime
	bytes::Float64
	orbit::String                    # "ASCENDING" | "DESCENDING" ("" when the catalogue says nothing)
	track::Int                       # relative orbit number (0 when unknown)
	rings::Vector{Matrix{Float64}}   # the footprint, lon lat, one matrix per ring
end

const _S1_FOUND  = Dict{Ptr{Cvoid},Vector{S1Product}}()           # window -> its last search
const _S1_FRAMES = Dict{Ptr{Cvoid},Dict{String,Vector{Int}}}()    # window -> footprint label -> products
const _S1_GROUP  = Dict{Ptr{Cvoid},String}()                      # window -> group tag of its footprints

_s1_dir()::String = get(ENV, "IGMT_S1_DIR", joinpath(homedir(), "Downloads", "Sentinel-1"))
_s1_file(p::S1Product, dir::String)::String = joinpath(dir, replace(p.name, r"\.SAFE$" => "") * ".zip")
_s1_kind(p::S1Product)::Symbol =
	occursin("_GRD", p.name) ? (occursin("_COG", p.name) ? :cog : :grd) :
	occursin("_SLC_", p.name) ? :slc : occursin("_OCN_", p.name) ? :ocn : :raw

# --- the search ------------------------------------------------------------------------------------

function _s1_filter(prod::Int, mode::String, W::Float64, E::Float64, S::Float64, N::Float64,
                    d0::GMT.Dates.Date, d1::GMT.Dates.Date, orbit::String, track::Int)::String
	f = ["Collection/Name eq 'SENTINEL-1'",
	     "OData.CSC.Intersects(area=geography'SRID=4326;POLYGON(($W $S,$E $S,$E $N,$W $N,$W $S))')",
	     "ContentDate/Start ge $(d0)T00:00:00.000Z",
	     "ContentDate/Start le $(d1)T23:59:59.999Z",
	     "contains(Name,'_$(mode)_$(_S1_PRODUCTS[prod][2])')"]
	kind = _S1_PRODUCTS[prod][3]
	kind === :cog && push!(f, "contains(Name,'_COG')")
	kind === :grd && push!(f, "not contains(Name,'_COG')")
	isempty(orbit) || push!(f, "Attributes/OData.CSC.StringAttribute/any(att:att/Name eq 'orbitDirection' and " *
	                           "att/OData.CSC.StringAttribute/Value eq '$(orbit)')")
	track > 0 && push!(f, "Attributes/OData.CSC.IntegerAttribute/any(att:att/Name eq 'relativeOrbitNumber' and " *
	                      "att/OData.CSC.IntegerAttribute/Value eq $(track))")
	return join(f, " and ")
end

# The footprint of one catalogue entry: GeoJSON Polygon or MultiPolygon (a scene over the antimeridian).
function _s1_rings(g)::Vector{Matrix{Float64}}
	out = Matrix{Float64}[]
	isa(g, Dict) || return out
	t = _shub_str(g, "type");  c = get(g, "coordinates", nothing)
	isa(c, Vector) || return out
	polys = t == "MultiPolygon" ? c : t == "Polygon" ? Any[c] : Any[]
	for P in polys
		(isa(P, Vector) && !isempty(P) && isa(P[1], Vector)) || continue
		ring = P[1]
		M = Matrix{Float64}(undef, length(ring), 2)
		for (k, v) in enumerate(ring)
			M[k, 1] = Float64(v[1]);  M[k, 2] = Float64(v[2])
		end
		push!(out, M)
	end
	return out
end

function _s1_attr(o, name::String)
	a = get(o, "Attributes", nothing)
	isa(a, Vector) || return nothing
	for x in a
		isa(x, Dict) && _shub_str(x, "Name") == name && return get(x, "Value", nothing)
	end
	return nothing
end

function _s1_parse(o)::Union{S1Product,Nothing}
	isa(o, Dict) || return nothing
	id, name = _shub_str(o, "Id"), _shub_str(o, "Name")
	(isempty(id) || isempty(name)) && return nothing
	cd = get(o, "ContentDate", nothing)
	ts = isa(cd, Dict) ? _shub_str(cd, "Start") : ""
	length(ts) >= 19 || return nothing
	bytes = get(o, "ContentLength", 0.0)
	orb = _s1_attr(o, "orbitDirection")
	trk = _s1_attr(o, "relativeOrbitNumber")
	return S1Product(id, name, DateTime(ts[1:19]), isa(bytes, Real) ? Float64(bytes) : 0.0,
	                 isa(orb, String) ? orb : "", isa(trk, Real) ? round(Int, trk) : 0,
	                 _s1_rings(get(o, "GeoFootprint", nothing)))
end

# Every product over the box in the period, newest first, and how many the catalogue counted.
function _s1_search(prod::Int, mode::String, W::Float64, E::Float64, S::Float64, N::Float64,
                    d0::GMT.Dates.Date, d1::GMT.Dates.Date, orbit::String, track::Int)::Tuple{Vector{S1Product},Int}
	url = _shub_url(_S1_ODATA, ["\$filter" => _s1_filter(prod, mode, W, E, S, N, d0, d1, orbit, track),
	                            "\$top" => string(_S1_TOP), "\$orderby" => "ContentDate/Start desc",
	                            "\$expand" => "Attributes", "\$count" => "True"])
	js = _shub_json(_shub_http_get(url; timeout = 180))
	isa(js, Dict) || error("The Copernicus catalogue did not answer with a product list.")
	v = get(js, "value", Any[])
	found = S1Product[]
	for o in (isa(v, Vector) ? v : Any[])
		p = _s1_parse(o)
		p === nothing || push!(found, p)
	end
	n = get(js, "@odata.count", length(found))
	return found, isa(n, Real) ? round(Int, n) : length(found)
end

# --- the login -------------------------------------------------------------------------------------
# One token per session, refreshed with its refresh token while that lives (so a two-factor account is
# not asked for a new code at every download), and a new password login after that.
const _S1_TOK    = Ref{String}("")
const _S1_TOKEXP = Ref{Float64}(0.0)
const _S1_REF    = Ref{String}("")
const _S1_REFEXP = Ref{Float64}(0.0)
const _S1_TOKWHO = Ref{String}("")          # the user the token belongs to

function _s1_login(d::Dict{String,String})::Tuple{String,String}
	u, p = _rs_get(d, "p_cd_user"), _rs_get(d, "p_cd_pass")
	(isempty(u) || isempty(p)) && ((u, p) = _rs_netrc_get(_rs_netrc_path(), _S1_MACHINE))
	(isempty(u) || isempty(p)) &&
		error("Downloading needs your Copernicus Data Space login (e-mail and password): give it in the Account tab.")
	return u, p
end

function _s1_token_from(form::String)::String
	js = _shub_json(_shub_http_post(_SHUB_OAUTH_CDSE, form))
	tok = _shub_str(js, "access_token")
	isempty(tok) && error("The Copernicus Data Space returned no access token.")
	num(k, dflt) = (isa(js, Dict) && isa(get(js, k, nothing), Real)) ? Float64(js[k]) : dflt
	_S1_TOK[] = tok;  _S1_TOKEXP[] = time() + num("expires_in", 600.0)
	_S1_REF[] = _shub_str(js, "refresh_token");  _S1_REFEXP[] = time() + num("refresh_expires_in", 0.0)
	return tok
end

function _s1_token(d::Dict{String,String})::String
	u, p = _s1_login(d)
	u == _S1_TOKWHO[] && time() < _S1_TOKEXP[] - 60 && return _S1_TOK[]
	if u == _S1_TOKWHO[] && !isempty(_S1_REF[]) && time() < _S1_REFEXP[] - 60
		tok = try
			_s1_token_from("grant_type=refresh_token&client_id=cdse-public&refresh_token=$(_shub_esc(_S1_REF[]))")
		catch
			""
		end
		isempty(tok) || return tok
	end
	totp = _rs_get(d, "p_cd_totp")
	form = "grant_type=password&client_id=cdse-public&username=$(_shub_esc(u))&password=$(_shub_esc(p))" *
	       (isempty(totp) ? "" : "&totp=$(_shub_esc(totp))")
	tok = try
		_s1_token_from(form)
	catch e
		msg = sprint(showerror, e)
		occursin("invalid_grant", msg) &&
			error("The Copernicus Data Space refused the login: check the e-mail and password in the Account tab" *
			      (isempty(totp) ? " (an account with two-factor login also needs the code of the moment)." : " and the two-factor code."))
		rethrow()
	end
	_S1_TOKWHO[] = u
	return tok
end

# --- the download ----------------------------------------------------------------------------------
# One product to `<folder>/<name>.zip`, through a ".part" file checked to be a zip before it takes the
# name (a refused token sends JSON). A file already there is not downloaded again.
function _s1_fetch(p::S1Product, dir::String, d::Dict{String,String})::String
	f = _s1_file(p, dir)
	isfile(f) && filesize(f) > 0 && return f
	mkpath(dir)
	tok = _s1_token(d)
	part = f * ".part";  tl = Ref(0.0)
	# Downloads.request calls it with (dl_total, dl_now, ul_total, ul_now).
	prog(total, now, _...) = (time() - tl[] > 0.25 && total > 0) &&
		(tl[] = time(); _rs_prog(round(Int, 100 * now / total),
		                         "Downloading $(basename(f)): $(round(now / 2^20, digits = 1)) of $(round(total / 2^20, digits = 1)) MB"))
	_rs_prog_show("Downloading $(basename(f))…")
	try
		r = open(part, "w") do io
			Downloads.request("$(_S1_DOWNLOAD)($(p.id))/\$value"; output = io, progress = prog, throw = false,
			                  headers = ["Authorization" => "Bearer " * tok])
		end
		isa(r, Downloads.Response) || throw(r)
		if !(200 <= r.status < 300)
			txt = try first(read(part, String), 400) catch; "" end
			error("The Copernicus Data Space answered $(r.status) for $(p.name)\n\n$(txt)")
		end
		head = open(io -> read(io, 4), part)
		head == UInt8[0x50, 0x4b, 0x03, 0x04] ||
			error("The Copernicus Data Space did not send a zip file for $(p.name): $(first(read(part, String), 300))")
		mv(part, f; force = true)
	finally
		_rs_prog_close()
		isfile(part) && rm(part; force = true)
	end
	return f
end

# --- showing a GRD ---------------------------------------------------------------------------------
# GDAL's SAFE driver reads the product inside its zip and gives the geolocation grid as GCPs; the
# calibrated subdatasets (SENTINEL1_CALIB:SIGMA0:…, GDAL >= 3.4) carry sigma nought. The warp to
# longitude/latitude follows the GCPs with a thin-plate spline, averaging the 10 m pixels into the
# spacing asked for.

function _s1_manifest(f::String, p::S1Product)::String
	base = replace(p.name, r"\.SAFE$" => "")
	for c in ("/vsizip/$(f)/$(base).SAFE/manifest.safe", "/vsizip/$(f)/$(base)/manifest.safe", "/vsizip/$(f)/manifest.safe")
		ok = try
			s = GMT.gdalinfo(c);  s isa AbstractString && occursin("SUBDATASET", s)
		catch
			false
		end
		ok && return c
	end
	error("GDAL could not open the Sentinel-1 product inside $(f).")
end

_s1_subdatasets(man::String)::Vector{String} =
	[String(m[1]) for m in eachmatch(r"SUBDATASET_\d+_NAME=([^\r\n]+)", string(GMT.gdalinfo(man)))]

# (subdataset, dB factor): 10 for an intensity, 20 for an amplitude, 0 = raw DN, no logarithm.
function _s1_pick_sds(sds::Vector{String}, pol::String, sigma::Bool)::Tuple{String,Float64}
	has(s) = occursin(Regex(":[A-Z0-9]+_$(pol)(:|\$)"), s)
	if sigma
		c = filter(s -> occursin("SENTINEL1_CALIB:SIGMA0:", s) && has(s), sds)
		k = findfirst(s -> endswith(s, ":INTENSITY"), c)
		k !== nothing && return (c[k], 10.0)
		k = findfirst(s -> endswith(s, ":AMPLITUDE"), c)
		k !== nothing && return (c[k], 20.0)
		isempty(c) || return (c[1], 10.0)
	end
	c = filter(s -> (occursin("SENTINEL1_CALIB:UNCALIB:", s) || occursin("SENTINEL1_DS:", s)) && has(s), sds)
	isempty(c) && error("This product has no $(pol) band. It has: " *
	                    join(unique(String(m[1]) for s in sds for m in eachmatch(r":[A-Z0-9]+_([VH][VH])(?::|$)", s)), ", "))
	k = findfirst(s -> endswith(s, ":AMPLITUDE"), c)
	return (c[k === nothing ? 1 : k], 0.0)
end

function _s1_grd_grid(f::String, p::S1Product, pol::String, sigma::Bool, inc::Float64,
                      box::Union{Nothing,NTuple{4,Float64}})::Tuple{GMTgrid,Bool}
	sds, fac = _s1_pick_sds(_s1_subdatasets(_s1_manifest(f, p)), pol, sigma)
	opts = ["-t_srs", "EPSG:4326", "-tr", string(inc), string(inc), "-r", "average", "-tps",
	        "-ot", "Float32", "-srcnodata", "0", "-dstnodata", "nan"]
	if box !== nothing
		W, E, S, N = box
		fw = minimum(minimum(view(M, :, 1)) for M in p.rings; init = Inf)
		fe = maximum(maximum(view(M, :, 1)) for M in p.rings; init = -Inf)
		fs = minimum(minimum(view(M, :, 2)) for M in p.rings; init = Inf)
		fn = maximum(maximum(view(M, :, 2)) for M in p.rings; init = -Inf)
		w, e, s, n = isfinite(fw) ? (max(W, fw), min(E, fe), max(S, fs), min(N, fn)) : (W, E, S, N)
		(w < e && s < n) || error("This scene does not reach the region.")
		append!(opts, ["-te", string(w), string(s), string(e), string(n)])
	end
	G = GMT.gdalwarp(sds, opts)
	G isa GMTgrid || error("GDAL did not return a grid for $(sds).")
	if fac > 0
		@inbounds for k in eachindex(G.z)
			v = G.z[k]
			G.z[k] = v > 0 ? Float32(fac * log10(v)) : NaN32
		end
	end
	any(!isnan, G.z) || error("The scene has no $(pol) value inside the region.")
	G.range[5] = GMT.minimum_nan(G.z);  G.range[6] = GMT.maximum_nan(G.z)
	isempty(G.proj4) && (G.proj4 = GMT.prj4WGS84)
	return G, fac > 0
end

# --- the dialog ------------------------------------------------------------------------------------

function _s1_box(d::Dict{String,String})::NTuple{4,Float64}
	W, E, S, N = (_rs_num(d, k, NaN) for k in ("p_rg_w", "p_rg_e", "p_rg_s", "p_rg_n"))
	any(isnan, (W, E, S, N)) && error("Give the four limits of the region.")
	(W < E && S < N) || error("The region must have W < E and S < N.")
	(-180 <= W && E <= 180 && -90 <= S && N <= 90) || error("The region must lie within -180/180/-90/90.")
	return W, E, S, N
end

function _s1_folder(d::Dict{String,String})::String
	dir = _rs_get(d, "p_folder")
	return isempty(dir) ? _s1_dir() : dir
end

_s1_sat(p::S1Product)::String = first(p.name, 3)
_s1_orb(p::S1Product)::String = p.orbit == "ASCENDING" ? "asc" : p.orbit == "DESCENDING" ? "desc" : "?"

function _s1_label(p::S1Product, dir::String)::String
	sz = p.bytes >= 2^30 ? "$(round(p.bytes / 2^30, digits = 1)) GB" : "$(round(Int, p.bytes / 2^20)) MB"
	have = isfile(_s1_file(p, dir)) ? "  (on disk)" : ""
	return "$(GMT.Dates.format(p.t0, "yyyy-mm-dd HH:MM"))  $(_s1_sat(p))  $(_s1_orb(p))  track $(p.track)  $(sz)$(have)"
end

function _s1_selected(scene::Ptr{Cvoid}, d::Dict{String,String})::S1Product
	found = get(_S1_FOUND, scene, S1Product[])
	isempty(found) && error("Find the scenes first.")
	k = _rs_idx(d, "p_granule") + 1
	(1 <= k <= length(found)) || error("Choose a scene in the list.")
	return found[k]
end

# The footprints of the scenes found: ONE line element per FRAME (the same piece of the same track,
# whatever the date — a period of weeks repeats each frame every 6 or 12 days), all under one group
# named for the search. Each is a guide line of this dialog: its menu's "Choose this scene (Sentinel-1)"
# selects the newest scene of that frame in the list. A new search replaces the previous footprints.
function _s1_plot_footprints!(scene::Ptr{Cvoid}, found::Vector{S1Product},
                              W::Float64, E::Float64, S::Float64, N::Float64)::Int
	old = pop!(_S1_GROUP, scene, "")
	isempty(old) || ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, old)
	delete!(_S1_FRAMES, scene)
	isempty(found) && return 0
	# Frames: same track, same direction, footprint centres less than half a degree apart in latitude.
	# (the closing vertex repeats the first one and is not counted twice)
	cen(p) = isempty(p.rings) ? NaN : (M = p.rings[1]; n = size(M, 1) - (size(M, 1) > 1 && M[1, :] == M[end, :]);
	                                   sum(view(M, 1:n, 2)) / n)
	frames = Tuple{Int,String,Float64,Vector{Int}}[]
	for (k, p) in enumerate(found)
		isempty(p.rings) && continue
		c = cen(p)
		j = findfirst(f -> f[1] == p.track && f[2] == p.orbit && abs(f[3] - c) < 0.5, frames)
		j === nothing ? push!(frames, (p.track, p.orbit, c, [k])) : push!(frames[j][4], k)
	end
	isempty(frames) && return 0
	_swot_map_if_empty(scene, W, E, S, N, "Sentinel-1 scenes")
	t0 = minimum(p.t0 for p in found);  t1 = maximum(p.t0 for p in found)
	nm = "Sentinel-1 scenes $(GMT.Dates.format(t0, "yyyy-mm-dd")) to $(GMT.Dates.format(t1, "yyyy-mm-dd"))"
	labels = Dict{String,Vector{Int}}()
	for (trk, orb, c, ks) in frames
		p = found[ks[1]]
		lab = "Track $(trk) $(_s1_orb(p)) at $(round(abs(c), digits = 1))$(c < 0 ? "S" : "N") ($(length(ks)) scene$(length(ks) > 1 ? "s" : ""))"
		haskey(labels, lab) && (lab *= " #$(length(labels) + 1)")
		D = Vector{GMTdataset{Float64,2}}()
		for M in p.rings
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
		      scene, lab, "s1", "pickscene", "Choose this scene (Sentinel-1)")
		labels[lab] = ks
	end
	_S1_GROUP[scene] = nm
	_S1_FRAMES[scene] = labels
	return length(labels)
end

function _rs_s1(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	prod = clamp(_rs_idx(d, "p_product") + 1, 1, length(_S1_PRODUCTS))
	if what == "savelogin"
		f = _rs_netrc_path()
		_rs_netrc_set!(f, _S1_MACHINE, _rs_get(d, "p_cd_user"), _rs_get(d, "p_cd_pass"))
		_rs_label(io, "lbl_cd_where", "Saved in $(f) as machine $(_S1_MACHINE)")
		_rs_status(io, "Copernicus Data Space login saved in $(f) (machine $(_S1_MACHINE)).")
		return true
	end
	if what == "rgwindow" || (what == "init" && !_rs_empty(scene))
		st = _scene_state(scene)
		if haskey(st, "x0") && st["x1"] > st["x0"] && st["y1"] > st["y0"]
			for (w, k) in (("p_rg_w", "x0"), ("p_rg_e", "x1"), ("p_rg_s", "y0"), ("p_rg_n", "y1"))
				_rs_set(io, w, string(round(Float64(st[k]), digits = 4)))
			end
		elseif what == "rgwindow"
			error("The window shows nothing yet: type the limits.")
		end
		what == "rgwindow" && return true
	end
	if what == "init"
		_rs_items(io, "p_product", String[p[1] for p in _S1_PRODUCTS])
		_rs_set(io, "p_product", 0)
		_rs_enable(io, "gb_show", true)                 # the first product is a GRD
		today = GMT.Dates.Date(RS.now(RS.UTC))
		_rs_set(io, "p_from", "$(today - GMT.Dates.Day(30))T00:00:00")
		_rs_set(io, "p_to", "$(today)T00:00:00")
		_rs_set(io, "p_folder", _s1_dir())
		f = _rs_netrc_path()
		u, p, why = try
			(_rs_netrc_get(f, _S1_MACHINE)..., isfile(f) ? "" : "  (the file does not exist yet)")
		catch e
			("", "", "  — cannot read it: " * sprint(showerror, e))
		end
		_rs_set(io, "p_cd_user", u);  _rs_set(io, "p_cd_pass", p)
		_rs_label(io, "lbl_cd_where", "Saved in $(f) as machine $(_S1_MACHINE)$(why)")
		found = get(_S1_FOUND, scene, S1Product[])
		_rs_items(io, "p_granule", String[_s1_label(x, _s1_dir()) for x in found])
		isempty(found) || _rs_set(io, "p_granule", 0)
		_rs_status(io, isempty(u) ? "Searching needs no account; downloading needs your Copernicus Data Space login (Account tab)." :
		                            "Give the region and the period, then Find the scenes.")
		return true
	end
	if what == "refresh"
		_rs_enable(io, "gb_show", _S1_PRODUCTS[prod][3] in (:grd, :cog))
		return true
	end
	if what == "search"
		W, E, S, N = _s1_box(d)
		d0 = GMT.Dates.Date(DateTime(_rs_get(d, "p_from")))
		d1 = GMT.Dates.Date(DateTime(_rs_get(d, "p_to")))
		d0 <= d1 || error("The period ends before it starts.")
		mode = _S1_MODES[clamp(_rs_idx(d, "p_mode") + 1, 1, length(_S1_MODES))]
		orbit = ("", "ASCENDING", "DESCENDING")[clamp(_rs_idx(d, "p_orbit") + 1, 1, 3)]
		ts = _rs_get(d, "p_track")
		track = isempty(ts) ? 0 : something(tryparse(Int, ts), -1)
		(0 <= track <= 175) || error("The track (relative orbit) is a number from 1 to 175, or empty for any.")
		_rs_prog_show("Asking the Copernicus catalogue for the Sentinel-1 scenes over the region…")
		found, total = try
			_s1_search(prod, mode, W, E, S, N, d0, d1, orbit, track)
		finally
			_rs_prog_close()
		end
		_S1_FOUND[scene] = found
		dir = _s1_folder(d)
		_rs_items(io, "p_granule", String[_s1_label(x, dir) for x in found])
		isempty(found) || _rs_set(io, "p_granule", 0)
		nfr = _s1_plot_footprints!(scene, found, W, E, S, N)
		_rs_status(io, isempty(found) ? "No $(mode) $(_S1_PRODUCTS[prod][1]) scene over the region in that period." :
		               "$(length(found)) scene(s) in $(nfr) frame(s), newest first." *
		               (total > length(found) ? " The catalogue has $(total): shorten the period to see the rest." : "") *
		               " Choose one in the list, or Pick a frame and click its footprint.")
		return true
	end
	if what == "pickscene"
		line = _rs_get(d, "picked")
		ks = get(get(_S1_FRAMES, scene, Dict{String,Vector{Int}}()), line, Int[])
		isempty(ks) && error("\"$(line)\" is not a Sentinel-1 footprint: Find the scenes, then click one of the footprints it draws.")
		found = _S1_FOUND[scene]
		_rs_set(io, "p_granule", ks[1] - 1)
		p = found[ks[1]]
		_rs_status(io, "$(line): chose the newest, $(GMT.Dates.format(p.t0, "yyyy-mm-dd HH:MM"))" *
		               (length(ks) > 1 ? " (the list has the other $(length(ks) - 1))." : "."))
		return true
	end
	what == "download" || what == "open" || error("unknown request: $what")
	p = _s1_selected(scene, d)
	dir = _s1_folder(d)
	f = _s1_fetch(p, dir, d)
	found = _S1_FOUND[scene]
	_rs_items(io, "p_granule", String[_s1_label(x, dir) for x in found])
	_rs_set(io, "p_granule", findfirst(==(p), found) - 1)
	if what == "download"
		_rs_status(io, "$(f) ($(round(filesize(f) / 2^20, digits = 1)) MB).")
		return true
	end
	_s1_kind(p) in (:grd, :cog) ||
		error("Only a GRD can be shown here (an SLC, OCN or RAW product is downloaded, not displayed): $(f)")
	pol = _S1_POLS[clamp(_rs_idx(d, "p_pol") + 1, 1, length(_S1_POLS))]
	sigma = _rs_idx(d, "p_scale") == 0
	inc = _rs_num(d, "p_res", 0.001)
	inc > 0 || error("The spacing must be positive.")
	box = _rs_on(d, "p_cut") ? _s1_box(d) : nothing
	_rs_prog_show("Resampling the $(pol) band of $(p.name) to longitude/latitude…")
	G, db = try
		_s1_grd_grid(f, p, pol, sigma, inc, box)
	finally
		_rs_prog_close()
	end
	what_ = !sigma ? "amplitude $(pol) (DN)" : db ? "sigma0 $(pol) (dB)" : "sigma0 $(pol)"
	name = "$(what_) — $(_s1_sat(p)) $(GMT.Dates.format(p.t0, "yyyy-mm-dd HH:MM")) track $(p.track) $(_s1_orb(p))"
	recipe = "Sentinel-1 $(basename(f)): $(what_) warped to $(inc) deg (gdalwarp -tps -r average)"
	_rs_deliver(scene, G, name, recipe)
	_rs_status(io, "$(name) added.")
	return true
end
