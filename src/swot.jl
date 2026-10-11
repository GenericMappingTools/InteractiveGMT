# swot.jl — Satellite > SWOT: the Surface Water and Ocean Topography mission's Level-2 products from
# NASA's PO.DAAC, found over a region and a period, downloaded with the Earthdata login, and shown in
# the window. One more tool of the Remote sensing dialog family (RemoteSDialog, 70_window.cpp, over
# deps/ui/remotes_swot.ui): same wire protocol, same CMR search (`_rs_cmr_names`), same Earthdata
# download (`_rs_download`), same .netrc account (`_rs_netrc_*`), same delivery (`_rs_deliver`).
#
# Two kinds of product:
#   * LR (KaRIn low rate, the ocean): one file per half-orbit PASS, pole to pole, 2 km pixels on two
#     60 km swaths. Gridded over the region with GMT nearneighbor.
#   * HR Raster (KaRIn high rate, inland and coastal water): a regular UTM grid per 128 km tile, read
#     as it is.
#
# Why the footprints are not taken from CMR alone: CMR gives every pass that crosses the antimeridian a
# WHOLE-GLOBE bounding box (-180/180/-90/90; checked 2026-10-11 on SWOT_L2_LR_SSH_BASIC_D), so a box
# search returns those passes for ANY region. A month over Iberia came back with 21 LR granules of which
# several never come near it (pass 424: longitudes 20..188). The real footprint comes from the orbit:
# SWOT flies a 21-day repeat (292 revolutions = 584 passes), so a pass is the SAME ground track every
# cycle, and pass p+2 is pass p shifted by -360*21/292 degrees in longitude. Verified on CMR's own
# polygons: passes 432 -> 460 -> 488 (14 revolutions apart) step by exactly -2.4657534 degrees, which is
# 14 * -360*21/292 mod 360. Two reference outlines (one descending, one ascending pass, CMR polygons,
# lat lon pairs) therefore give the outline of every pass.

const _SWOT_DLON = -360.0 * 21 / 292       # longitude shift of a pass per revolution (two passes)

# (pass number, CMR polygon "lat lon lat lon ...") — even passes descend, odd passes ascend.
const _SWOT_REF_DESC = (432, "78.2362414 -82.7309064 77.0897237 -82.7309064 75.8399841 -58.4875236 72.6501699 -40.831175 68.360759 -29.3798741 63.4990184 -21.9050367 58.3258903 -16.7961837 52.9701746 -13.1286881 47.4995245 -10.3785989 41.9521127 -8.2374154 36.3509928 -6.5144293 30.711003 -5.0858977 25.0422986 -3.8677129 19.3522669 -2.8000358 13.6466193 -1.8382303 7.9300442 -0.9472289 2.206617 -0.0978077 -0.6564905 0.3195147 -6.3832336 1.1550125 -12.1076672 2.0127463 -17.8260267 2.9199637 -23.5341698 3.9083043 -29.22726 5.0170075 -34.8992765 6.2976543 -40.5422062 7.8217026 -46.1446391 9.6931978 -51.6891906 12.0714225 -57.147472 15.2135051 -62.4695682 19.5588282 -67.5602878 25.9020398 -72.2220488 35.7312547 -76.0213815 51.6177497 -78.1032616 75.7506048 -76.9685868 76.5130571 -75.0461911 54.0332254 -71.4496393 38.4507961 -66.9484816 28.4114289 -61.973793 21.7786734 -56.735845 17.1770745 -51.3400685 13.8284256 -45.8430433 11.2877755 -40.2773669 9.2893731 -34.6631124 7.6665012 -29.0134402 6.3094463 -23.3375234 5.142609 -17.6421572 4.1114512 -11.9326915 3.1746792 -6.2135954 2.2993013 -0.4888186 1.4572892 2.3745449 1.0407764 8.1009719 0.2011518 13.8239582 -0.6687443 19.5396512 -1.5971941 25.2437382 -2.6177194 30.9310886 -3.7726566 36.5951883 -5.1185666 42.2271916 -6.7349954 47.8142468 -8.7395001 53.3363685 -11.3148617 58.7602129 -14.7611054 64.0257579 -19.5999414 69.0155715 -26.7888641 73.4803021 -38.1145143 76.8751738 -56.3960771 78.2362414 -82.7309064")
const _SWOT_REF_ASC = (447, "-77.0895982 -96.9089886 -78.2361159 -96.9089886 -76.8763531 -70.5875554 -73.4819884 -52.3026189 -69.0172888 -40.975686 -64.0273134 -33.786857 -58.7615395 -28.9487634 -53.3374523 -25.503535 -47.8150981 -22.929276 -42.2278323 -20.9258626 -36.5956463 -19.3104573 -30.9313944 -17.9654671 -25.2439229 -16.811322 -19.5397455 -15.7914443 -13.8239917 -14.8634857 -8.1009727 -13.9939163 -2.37454 -13.1544488 0.4888167 -12.7379502 6.2135609 -11.8958375 11.9325997 -11.020189 17.6419835 -10.0829809 23.3372433 -9.0512292 29.0130292 -7.8836514 34.6625461 -6.5257255 40.2766224 -4.9018746 45.8421001 -2.9024183 51.3389117 -0.3606904 56.7344701 2.9889757 61.9722164 7.59138 66.9467631 14.224444 71.4479357 24.2631052 75.0448655 39.8432424 76.9682762 62.3204503 78.1029102 61.5567152 76.019976 37.4274721 72.2203201 21.5437382 67.5586067 11.715293 62.4680776 5.3717356 57.1462182 1.0255603 51.6881784 -2.1175781 46.1438537 -4.4969098 40.5416234 -6.3694807 34.8988674 -7.8945245 29.2269939 -9.1760549 23.5340154 -10.2855083 17.8259537 -11.2744507 12.1076464 -12.1821107 6.3832372 -13.0401206 0.6564921 -13.8757242 -2.2066261 -14.293035 -7.9300933 -15.1423043 -13.6467331 -16.0329848 -19.3524699 -16.9943059 -25.0426154 -18.0613437 -30.7114582 -19.2787469 -36.3516102 -20.7063723 -41.9529148 -22.4283529 -47.5005305 -24.5684689 -52.971397 -27.3174874 -58.3273289 -30.9840091 -63.5006469 -36.0921671 -68.3624962 -43.5669337 -72.6518113 -55.0193771 -75.84108 -72.67843 -77.0895982 -96.9089886")

# The products of the Product combo: (combo text, CMR collection, PO.DAAC archive folder, kind,
# the arrays offered before the file is on disk — the file's own list replaces them once it is).
# Collections verified in CMR 2026-10-11 (version D is the current processing).
const _SWOT_PRODUCTS = Tuple{String,String,String,Symbol,Vector{String}}[
	("LR sea surface height, Basic (2 km)",   "SWOT_L2_LR_SSH_BASIC_D",    "SWOT_L2_LR_SSH_D",    :lr,
	 ["ssha_karin", "ssha_karin_2", "ssh_karin", "ssh_karin_2", "mean_sea_surface_cnescls", "geoid"]),
	("LR sea surface height, Expert (2 km)",  "SWOT_L2_LR_SSH_EXPERT_D",   "SWOT_L2_LR_SSH_D",    :lr,
	 ["ssha_karin", "ssha_karin_2", "ssh_karin", "ssh_karin_2", "sig0_karin", "swh_karin", "wind_speed_karin"]),
	# AVISO+ (CNES) Level-3: calibrated, edited and denoised LR passes. Not in CMR: the second and third
	# fields are the THREDDS variant folder and the product version (_SWOT_L3_TDS).
	("LR sea surface height L3, Basic (AVISO, 2 km)",  "Basic",  "v3_0", :l3,
	 ["ssha_filtered", "ssha_unfiltered", "mdt"]),
	("LR sea surface height L3, Expert (AVISO, 2 km)", "Expert", "v3_0", :l3,
	 ["ssha_filtered", "ssha_unfiltered", "ssha_unedited", "mdt", "ugos_filtered", "vgos_filtered",
	  "ugosa_filtered", "vgosa_filtered", "sigma0", "mss"]),
	("HR water surface, Raster 100 m",       "SWOT_L2_HR_Raster_100m_D",  "SWOT_L2_HR_Raster_D", :raster,
	 ["wse", "water_area", "water_frac", "sig0"]),
	("HR water surface, Raster 250 m",        "SWOT_L2_HR_Raster_250m_D",  "SWOT_L2_HR_Raster_D", :raster,
	 ["wse", "water_area", "water_frac", "sig0"]),
	# River and lake single-pass products: zipped shapefiles, one per pass and continent ("_EU_").
	("HR rivers, reaches (shapefile)",        "SWOT_L2_HR_RiverSP_reach_D", "SWOT_L2_HR_RiverSP_D", :reach,
	 ["wse", "width", "slope", "dschg_c", "area_total"]),
	("HR rivers, nodes (shapefile)",          "SWOT_L2_HR_RiverSP_node_D",  "SWOT_L2_HR_RiverSP_D", :node,
	 ["wse", "width", "area_total", "wse_u"]),
	("HR lakes, prior lakes (shapefile)",     "SWOT_L2_HR_LakeSP_prior_D",  "SWOT_L2_HR_LakeSP_D",  :lake,
	 ["wse", "area_total", "wse_std", "wse_u"]),
]
const _SWOT_ARCHIVE = "https://archive.swot.podaac.earthdata.nasa.gov/podaac-swot-ops-cumulus-protected/"
# The shapefile kinds, and each one's own quality attribute (0 good, 1 suspect, 2 degraded, 3 bad).
const _SWOT_SHP_QUAL = Dict(:reach => "reach_q", :node => "node_q", :lake => "quality_f")
_swot_isshp(prod::Int)::Bool = haskey(_SWOT_SHP_QUAL, _SWOT_PRODUCTS[prod][4])
_swot_ext(prod::Int)::String = _swot_isshp(prod) ? ".zip" : ".nc"

struct SwotGranule
	name::String          # producer granule id (the file name without its extension)
	cycle::Int
	pass::Int
	t0::DateTime
	prod::Int             # index into _SWOT_PRODUCTS
	url::String           # where the file is downloaded from
end

# The last search of each window's dialog: the File combo lists these, in this order.
const _SWOT_FOUND = Dict{Ptr{Cvoid},Vector{SwotGranule}}()

_swot_dir()::String = get(ENV, "IGMT_SWOT_DIR", joinpath(homedir(), "Downloads", "SWOT"))
_swot_url(g::SwotGranule)::String = g.url
_swot_file(g::SwotGranule, dir::String)::String = joinpath(dir, g.name * _swot_ext(g.prod))
_swot_isl3(prod::Int)::Bool = _SWOT_PRODUCTS[prod][4] === :l3

# Cycle, pass and start time out of any SWOT name: "..._055_432_20260905T074243_..." (LR, L2 and L3),
# "..._055_432_040F_20260905T075445_..." (Raster: the tile between pass and time),
# "..._055_320_EU_20260901T074809_..." (river and lake shapefiles: the continent). `url` defaults to
# the file's place in PO.DAAC's archive; an AVISO file passes its own.
function _swot_parse(name::String, prod::Int; url::String = "")::Union{SwotGranule,Nothing}
	m = match(r"_(\d{3})_(\d{3})_(?:\d{3}[A-Z]_|[A-Z]{2}_)?(\d{8}T\d{6})_", name)
	m === nothing && return nothing
	isempty(url) && (url = _SWOT_ARCHIVE * _SWOT_PRODUCTS[prod][3] * "/" * name * _swot_ext(prod))
	return SwotGranule(name, parse(Int, m[1]), parse(Int, m[2]), DateTime(m[3], GMT.Dates.DateFormat("yyyymmddTHHMMSS")),
	                   prod, url)
end

# --- the footprint of a pass ---------------------------------------------------------------------

# The vertices of pass `p`'s CMR outline (both swaths, pole to pole), lon, lat.
function _swot_pass_vertices(p::Int)::Tuple{Vector{Float64},Vector{Float64}}
	pref, txt = iseven(p) ? _SWOT_REF_DESC : _SWOT_REF_ASC
	v = parse.(Float64, split(txt))
	# Reduced to -180..180: passes far apart in number are many whole turns apart (126 vs 432: 3961 deg).
	shift = mod(((p - pref) ÷ 2) * _SWOT_DLON + 180, 360) - 180
	return v[2:2:end] .+ shift, v[1:2:end]
end

# Outline of pass `p` as lon, lat, its edges followed as GREAT CIRCLES (what a CMR polygon's edges are)
# every 0.25 degree of arc: near the poles the vertices are 25 degrees of longitude apart, and a
# straight lon/lat chord there misses the swath by more than its own width. Longitudes continuous (no
# ±360 jumps along it).
function _swot_pass_outline(p::Int)::Tuple{Vector{Float64},Vector{Float64}}
	vlon, vlat = _swot_pass_vertices(p)
	xyz(lo, la) = (cosd(la) * cosd(lo), cosd(la) * sind(lo), sind(la))
	lon = Float64[vlon[1]];  lat = Float64[vlat[1]]
	for i in 1:length(vlon)-1
		a, b = xyz(vlon[i], vlat[i]), xyz(vlon[i+1], vlat[i+1])
		w = acosd(clamp(sum(a .* b), -1.0, 1.0))
		m = max(1, ceil(Int, w / 0.25))
		for t in 1:m
			f = t / m
			if w < 1e-9
				q = b
			else
				s1, s2 = sind((1 - f) * w) / sind(w), sind(f * w) / sind(w)
				q = s1 .* a .+ s2 .* b
			end
			push!(lon, atand(q[2], q[1]));  push!(lat, asind(clamp(q[3], -1.0, 1.0)))
		end
	end
	for k in 2:length(lon)
		while lon[k] - lon[k-1] > 180  lon[k] -= 360  end
		while lon[k] - lon[k-1] < -180 lon[k] += 360  end
	end
	return lon, lat
end

# Do segments (a,b) and (c,d) cross?
function _swot_seg_cross(ax, ay, bx, by, cx, cy, dx, dy)::Bool
	o(px, py, qx, qy, rx, ry) = sign((qx - px) * (ry - py) - (qy - py) * (rx - px))
	o1, o2 = o(ax, ay, bx, by, cx, cy), o(ax, ay, bx, by, dx, dy)
	o3, o4 = o(cx, cy, dx, dy, ax, ay), o(cx, cy, dx, dy, bx, by)
	return o1 * o2 < 0 && o3 * o4 < 0
end

function _swot_inside(px::Float64, py::Float64, lon::Vector{Float64}, lat::Vector{Float64})::Bool
	c = false;  n = length(lon)
	for i in 1:n
		j = i == 1 ? n : i - 1
		((lat[i] > py) != (lat[j] > py)) &&
			px < (lon[j] - lon[i]) * (py - lat[i]) / (lat[j] - lat[i]) + lon[i] && (c = !c)
	end
	return c
end

# Does pass `p` cross the box? Measured against a real pass (432, cycle 55): the outermost pixels lie up
# to 15 km outside the CMR outline up to 76 degrees of latitude, and up to 200 km at the polar turn,
# where the outline's cap cuts the swath short. So the box is padded by 20 km, and a box that reaches
# past 75 degrees is never filtered (every pass CMR returns is kept). Tried at every 360-degree turn of
# the outline's longitudes.
function _swot_pass_hits(p::Int, W::Float64, E::Float64, S::Float64, N::Float64)::Bool
	(N > 75 || S < -75) && return true
	lon, lat = _swot_pass_outline(p)
	pad = 0.2                                       # 20 km of latitude
	padx = pad / cosd(max(abs(S), abs(N)) + pad)    # and of longitude at the box's poleward side
	s, n = S - pad, N + pad
	for k in -2:2
		w, e = W - padx + 360k, E + padx + 360k
		any(i -> w <= lon[i] <= e && s <= lat[i] <= n, eachindex(lon)) && return true
		any(c -> _swot_inside(c[1], c[2], lon, lat), ((w, s), (w, n), (e, s), (e, n))) && return true
		box = ((w, s, e, s), (e, s, e, n), (e, n, w, n), (w, n, w, s))
		for i in 1:length(lon)-1, b in box
			_swot_seg_cross(lon[i], lat[i], lon[i+1], lat[i+1], b...) && return true
		end
	end
	return false
end

# A Raster tile's UTM zone must be one the box covers (a tile never leaves its zone). The antimeridian
# tiles (UTM01, UTM60) carry the same whole-globe box in CMR as the LR passes.
function _swot_zone_ok(name::String, W::Float64, E::Float64)::Bool
	m = match(r"_UTM(\d{2})[A-Z]_", name)
	m === nothing && return true
	z = parse(Int, m[1])
	return any(lon -> mod(floor(Int, mod(lon + 180, 360) / 6), 60) + 1 == z, range(W - 1, E + 1; step = 0.5))
end

# The two swath edges of each pass, cut to the box padded by 2 degrees: one line per stretch inside.
function _swot_pass_lines(p::Int, W::Float64, E::Float64, S::Float64, N::Float64)::Vector{Matrix{Float64}}
	lon, lat = _swot_pass_outline(p)
	# Turn the whole outline by 360 so that it sits nearest the box.
	c = (W + E) / 2
	k = round(Int, (c - sum(lon) / length(lon)) / 360)
	lon .+= 360k
	w, e, s, n = W - 2, E + 2, S - 2, N + 2
	out = Matrix{Float64}[];  run = Tuple{Float64,Float64}[]
	flush!() = (length(run) > 1 && push!(out, [first.(run) last.(run)]); empty!(run))
	for i in 1:length(lon)-1
		m = max(2, ceil(Int, max(abs(lon[i+1] - lon[i]), abs(lat[i+1] - lat[i])) / 0.05))
		for t in 0:m-1
			x = lon[i] + (lon[i+1] - lon[i]) * t / m
			y = lat[i] + (lat[i+1] - lat[i]) * t / m
			(w <= x <= e && s <= y <= n) ? push!(run, (x, y)) : flush!()
		end
	end
	flush!()
	return out
end

# --- the search ------------------------------------------------------------------------------------

function _swot_search(prod::Int, W::Float64, E::Float64, S::Float64, N::Float64,
                      d0::GMT.Dates.Date, d1::GMT.Dates.Date)::Tuple{Vector{SwotGranule},Int}
	coll = _SWOT_PRODUCTS[prod][2]
	# CMR's box is in -180..180; E past 180 is written the CMR way for a box across the antimeridian (W > E).
	cw, ce = W >= 180 ? (W - 360, E - 360) : (W, E)
	ce > 180 && (ce -= 360)
	names = _rs_cmr_names(coll, cw, ce, S, N, "$(d0)T00:00:00Z", "$(d1)T23:59:59Z", "")
	kind = _SWOT_PRODUCTS[prod][4]
	found = SwotGranule[];  ndrop = 0
	hit = Dict{Int,Bool}()
	for nm in names
		g = _swot_parse(nm, prod)
		g === nothing && continue
		ok = get!(() -> _swot_pass_hits(g.pass, W, E, S, N), hit, g.pass) &&
		     (kind !== :raster || _swot_zone_ok(nm, W, E))
		ok ? push!(found, g) : (ndrop += 1)
	end
	sort!(found, by = g -> (g.t0, g.name))
	return found, ndrop
end

# --- AVISO+ Level-3: the THREDDS catalogue -----------------------------------------------------------
# AVISO's L3 is not in CMR. Its THREDDS server lists every file, publicly (checked 2026-10-11:
# catalog.xml answers without a login; the files themselves and OPeNDAP need the AVISO+ login, HTTP
# Basic, realm "Ldap Authentification"). Layout: <version>/<Basic|Expert>/<reproc|forward>/cycle_CCC/
# SWOT_L3_LR_SSH_<Variant>_<CCC>_<PPP>_<begin>_<end>_v<version>.nc — "reproc" is the reprocessed
# archive, "forward" the recent cycles; a cycle in both is taken from reproc.
const _SWOT_L3_TDS = "https://tds-odatis.aviso.altimetry.fr/thredds/"
const _SWOT_L3_DS = "dataset-l3-swot-karin-nadir-validated/l3_lr_ssh/"
const _SWOT_AVISO = "tds-odatis.aviso.altimetry.fr"        # the .netrc machine of the AVISO+ login
# Science-orbit cycles, 20.8646 days each (cycle 55 pass 432 -> cycle 56 pass 432, PO.DAAC names:
# 2026-09-05T07:42:43 -> 2026-09-26T04:27:41). Cycle 55 pass 1 began 2026-08-20T22:08:02 (AVISO name).
# The science orbit began 2023-07-26 (cycle 1, pass 149, AVISO L3 handbook); before that SWOT flew its
# 1-day cal/val orbit, whose passes are not this orbit's and are not searched here.
const _SWOT_CYCLE_DAYS = 20.8646
const _SWOT_C55 = DateTime(2026, 8, 20, 22, 8, 2)
const _SWOT_SCIENCE0 = DateTime(2023, 7, 26)

_swot_cycle_at(t::DateTime)::Int =
	55 + floor(Int, GMT.Dates.value(t - _SWOT_C55) / (_SWOT_CYCLE_DAYS * 86_400_000))

# The (name, urlPath) of every .nc of one catalog page; empty when the folder does not exist.
function _swot_tds_files(cat::String)::Vector{Tuple{String,String}}
	out = IOBuffer()
	r = try
		Downloads.request(cat; output = out, throw = false)
	catch e
		error("AVISO's catalogue could not be reached: " * sprint(showerror, e))
	end
	r isa Downloads.Response && r.status == 200 || return Tuple{String,String}[]
	txt = String(take!(out))
	return [(String(m[1]), String(m[2])) for m in
	        eachmatch(r"name=\"(SWOT_L3_LR_SSH_[^\"]+?)\.nc\"[^>]*?urlPath=\"([^\"]+)\"", txt)]
end

function _swot_l3_search(prod::Int, W::Float64, E::Float64, S::Float64, N::Float64,
                         d0::GMT.Dates.Date, d1::GMT.Dates.Date)::Tuple{Vector{SwotGranule},Int}
	_, variant, ver, _, _ = _SWOT_PRODUCTS[prod]
	t0, t1 = DateTime(d0), DateTime(d1) + GMT.Dates.Day(1)
	t1 <= _SWOT_SCIENCE0 &&
		error("Before 26 July 2023 SWOT flew its 1-day cal/val orbit: the L3 search covers the science orbit only.")
	c0 = max(1, _swot_cycle_at(max(t0, _SWOT_SCIENCE0)) - 1)
	c1 = _swot_cycle_at(t1) + 1
	found = SwotGranule[];  ndrop = 0
	hit = Dict{Int,Bool}()
	# Every (cycle, stream) page asked at once: a page is 1-4 s (300 kB, 584 files) and a cold server
	# took over a minute for a month's four cycles one after the other (measured 2026-10-11).
	cat(c, stream) = _SWOT_L3_TDS * "catalog/" * _SWOT_L3_DS * "$(ver)/$(variant)/$(stream)/cycle_$(lpad(c, 3, '0'))/catalog.xml"
	jobs = [(c, s) for c in c0:c1 for s in ("reproc", "forward")]
	pages = Dict(zip(jobs, asyncmap(j -> _swot_tds_files(cat(j...)), jobs; ntasks = 8)))
	for c in c0:c1
		have = Set{Int}()
		for stream in ("reproc", "forward")             # reproc first: it wins where both have a pass
			for (nm, path) in pages[(c, stream)]
				g = _swot_parse(nm, prod; url = _SWOT_L3_TDS * "fileServer/" * path)
				(g === nothing || g.pass in have) && continue
				t0 <= g.t0 < t1 || continue
				push!(have, g.pass)
				get!(() -> _swot_pass_hits(g.pass, W, E, S, N), hit, g.pass) ? push!(found, g) : (ndrop += 1)
			end
		end
	end
	sort!(found, by = g -> (g.t0, g.name))
	return found, ndrop
end

# --- reading ---------------------------------------------------------------------------------------

# Quality choice of the dialog: 0 = good only, 1 = good and suspect, 2 = every valid value.
#
# LR: `<var>_qual` is a bit field; its flag_meanings say which bits are "suspect_*" (the rest are
# degraded/bad). Good = no bit set; suspect = only suspect bits set. The fill value has all bits set.
function _swot_lr_suspect_mask(sds_q::String)::UInt32
	info = GMT.gdalinfo(sds_q)
	info isa AbstractString || return 0x00000000
	mm = match(r"flag_masks=\{([0-9,\s]+)\}", info)
	mn = match(r"flag_meanings=([^\n]+)", info)
	(mm === nothing || mn === nothing) && return 0x00000000
	masks = parse.(UInt32, split(mm[1], ','))
	means = split(strip(mn[1]))
	length(masks) == length(means) || return 0x00000000
	s = 0x00000000
	for (b, w) in zip(masks, means)
		startswith(w, "suspect") && (s |= b)
	end
	return s
end

_swot_sds(file::String, var::String)::String = "NETCDF:\"$(file)\":$(var)"

# The arrays of a file the Variable combo offers: no coordinates, no quality fields. For a zipped
# shapefile, its numeric attributes (read once per file: a node file is 20 000 features).
const _SWOT_SHP_ATTRS = Dict{String,Vector{String}}()
function _swot_vars(file::String)::Vector{String}
	if lowercase(splitext(file)[2]) in (".zip", ".shp")
		return get!(_SWOT_SHP_ATTRS, abspath(file)) do
			D = GMT.gmtread(file)
			a = (D isa AbstractVector ? D[1] : D).attrib
			sort!([String(k) for (k, v) in a if tryparse(Float64, strip(v)) !== nothing && !endswith(k, "_id") &&
			       k != "Feature_ID" && !endswith(k, "_q") && !endswith(k, "_q_b") && k != "quality_f"])
		end
	end
	v = RS.sds_names(file)
	return filter(n -> !(n in ("latitude", "longitude", "quality_flag", "i_num_line", "i_num_pixel")) && !endswith(n, "_qual") && !endswith(n, "_qual_bitwise"), v)
end

# A river/lake shapefile -> the features over the box, ready to draw: each keeps its geometry, its
# value of `var` and its id in its header (the overlay's hover text). Quality is the feature's own
# attribute (_SWOT_SHP_QUAL): qmode 0 keeps 0 only, 1 keeps 0 and 1, 2 keeps all. The nodes (points,
# one feature each) are merged into ONE point table with a text per point, so 20 000 nodes are one
# overlay and not 20 000.
function _swot_shp_features(file::String, var::String, kind::Symbol, W::Float64, E::Float64,
                            S::Float64, N::Float64, qmode::Int)::Vector{GMTdataset{Float64,2}}
	D = GMT.gmtread(file)
	segs = D isa AbstractVector ? D : [D]
	qn = _SWOT_SHP_QUAL[kind]
	idn = kind === :reach ? "reach_id" : kind === :node ? "node_id" : "lake_id"
	units = Dict("wse" => " m", "width" => " m", "area_total" => (kind === :lake ? " km²" : " m²"),   # files: lakes km², reaches/nodes m²
	              "slope" => " m/m", "dschg_c" => " m³/s",
	             "wse_u" => " m", "wse_std" => " m")
	fmt(s) = (v = tryparse(Float64, strip(s)); v === nothing ? strip(s) :
	          v <= -1e11 ? "no value" : string(round(v, sigdigits = 6)) * get(units, var, ""))
	inbox(M) = any(i -> W <= W + mod(M[i, 1] - W, 360) <= E && S <= M[i, 2] <= N, axes(M, 1))
	out = GMTdataset{Float64,2}[]
	px = Float64[];  py = Float64[];  pt = String[]
	for s in segs
		a = s.attrib
		if qmode < 2
			q = tryparse(Float64, strip(get(a, qn, "")))
			(q === nothing || q > qmode) && continue
		end
		inbox(s.data) || continue
		txt = "$(idn) $(strip(get(a, idn, "?")))  $(var) = $(fmt(get(a, var, "")))"
		if kind === :node
			for i in axes(s.data, 1)
				push!(px, s.data[i, 1]);  push!(py, s.data[i, 2]);  push!(pt, txt)
			end
		else
			Dk = GMT.mat2ds(Matrix{Float64}(s.data[:, 1:2]); hdr = txt)
			Dk.proj4 = GMT.prj4WGS84
			push!(out, Dk)
		end
	end
	if kind === :node && !isempty(px)
		Dk = GMT.mat2ds([px py], pt)
		Dk.proj4 = GMT.prj4WGS84
		push!(out, Dk)
	end
	isempty(out) && error("No $(kind === :lake ? "lake" : kind === :node ? "river node" : "river reach") of that quality inside the region in this file.")
	return out
end

# LR pass -> grid over the box. The pass's 2 km pixels (lon in 0..360) are taken into the box's own
# longitudes, filtered by quality, cut to the box, and gridded with GMT nearneighbor, search radius two
# increments (what grid_at_sensor uses: inc_x + inc_y). Not grid_at_sensor itself: it knows MODIS's
# "qual_<var>" fields only, and takes the swath's longitudes as they come, so a pass in 0..360 never
# lands in a box written in -180..180.
#
# `xover`: add the crossover calibration `height_cor_xover` to the L2 heights (ssh_karin, ssha_karin
# and their _2 twins). The L2 heights do NOT include it, and without it a pass carries the KaRIn
# roll/phase error as a tilt ACROSS the swath: measured on pass 432 (cycle 55, Bay of Biscay) the
# per-pixel mean of ssha_karin ran from -0.96 m on one edge to +1.02 m on the other, and the scatter
# over the swath dropped from 0.63 m to 0.05 m once it was added. L3 heights are already calibrated.
const _SWOT_XOVER_VARS = ("ssh_karin", "ssha_karin", "ssh_karin_2", "ssha_karin_2")

function _swot_lr_grid(file::String, var::String, W::Float64, E::Float64, S::Float64, N::Float64,
                       inc::Float64, qmode::Int; xover::Bool = true)::GMTgrid
	Z = gd2gmt(_swot_sds(file, var))
	Lo = gd2gmt(_swot_sds(file, "longitude"))
	La = gd2gmt(_swot_sds(file, "latitude"))
	z = Z.z;  lo = Lo.z;  la = La.z
	(size(z) == size(lo) == size(la)) || error("$(var) is not on the swath's (lines, pixels) grid.")
	if xover && var in _SWOT_XOVER_VARS && "height_cor_xover" in RS.sds_names(file)
		xc = gd2gmt(_swot_sds(file, "height_cor_xover")).z     # same reader, same order as z
		size(xc) == size(z) || error("height_cor_xover and $(var) differ in size.")
		z = Float32[Float32(z[k]) + Float32(xc[k]) for k in eachindex(z)]   # NaN where either is missing
		z = reshape(z, size(xc))
	end
	keep = [!isnan(Float64(v)) for v in z]
	qn = var * "_qual"
	if qmode < 2 && qn in RS.sds_names(file)
		sq = _swot_sds(file, qn)
		q = gd2gmt(sq).z
		sus = qmode == 1 ? _swot_lr_suspect_mask(sq) : 0x00000000
		@inbounds for k in eachindex(q)
			keep[k] && (keep[k] = (UInt32(q[k]) & ~sus) == 0)
		end
	elseif qmode < 2 && "quality_flag" in RS.sds_names(file)
		# AVISO L3: ONE flag per pixel, a code not bits — 0 good; 3 eclipse, 5 local outliers, 10 bad
		# quality coast, 18/19/20 ocean/ice unsure, ice, 30 soft outliers, 50 extremes, 70 mission event,
		# 100+ ... Its handbook's own advice for a looser cut ("apply flags higher than 30", §5.5) is the
		# "Good and suspect" choice. (The ssha_filtered/_unfiltered arrays already have the flags applied.)
		Q = gd2gmt(_swot_sds(file, "quality_flag"))
		q = Q isa GMTimage ? Q.image : Q.z
		length(q) == length(z) || error("quality_flag and $(var) differ in size.")
		qmax = qmode == 0 ? 0 : 30
		@inbounds for k in eachindex(q)
			keep[k] && (keep[k] = Int(q[k]) <= qmax)
		end
	end
	pad = 2inc
	x = Float64[];  y = Float64[];  v = Float64[]
	@inbounds for k in eachindex(z)
		keep[k] || continue
		X = W + mod(Float64(lo[k]) - W, 360.0)        # into the box's longitudes
		Y = Float64(la[k])
		(W - pad <= X <= E + pad && S - pad <= Y <= N + pad) || continue
		push!(x, X);  push!(y, Y);  push!(v, Float64(z[k]))
	end
	isempty(v) && error("This pass has no $(var) value of that quality inside the region.")
	w, e = floor(W / inc) * inc, ceil(E / inc) * inc
	s, n = floor(S / inc) * inc, ceil(N / inc) * inc
	G = GMT.nearneighbor([x y v], I = inc, R = (w, e, s, n), S = 2inc)
	G.proj4 = GMT.prj4WGS84
	return G
end

# HR Raster tile -> its grid, as it lies (UTM). Quality `<var>_qual`: 0 good, 1 suspect, 2 degraded,
# 3 bad. The two arrays come off the same reader, so the mask is applied element by element.
function _swot_raster_grid(file::String, var::String, qmode::Int)::GMTgrid
	G = _gmtread_trb(file * "?" * var)
	G isa GMTgrid || error("$(var) of $(basename(file)) is not a grid.")
	qn = var * "_qual"
	if qmode < 2 && qn in RS.sds_names(file)
		Q = _gmtread_trb(file * "?" * qn)
		size(Q.z) == size(G.z) || error("$(qn) and $(var) differ in size.")
		qmax = qmode == 0 ? 0.0 : 1.0
		@inbounds for k in eachindex(G.z)
			(isnan(Q.z[k]) || Q.z[k] > qmax) && (G.z[k] = NaN)
		end
		any(!isnan, G.z) || error("$(var) has no value of that quality in this tile.")
		G.range[5] = GMT.minimum_nan(G.z);  G.range[6] = GMT.maximum_nan(G.z)
	end
	return G
end

# --- the dialog --------------------------------------------------------------------------------------

function _swot_selected(scene::Ptr{Cvoid}, d::Dict{String,String})::SwotGranule
	found = get(_SWOT_FOUND, scene, SwotGranule[])
	isempty(found) && error("Find the passes first.")
	k = _rs_idx(d, "p_granule") + 1
	(1 <= k <= length(found)) || error("Choose a file in the list.")
	return found[k]
end

function _swot_dir_of(d::Dict{String,String})::String
	dir = _rs_get(d, "p_dir")
	return isempty(dir) ? _swot_dir() : dir
end

function _swot_box(d::Dict{String,String})::NTuple{4,Float64}
	W, E, S, N = (_rs_num(d, k, NaN) for k in ("p_rg_w", "p_rg_e", "p_rg_s", "p_rg_n"))
	any(isnan, (W, E, S, N)) && error("Give the four limits of the region.")
	(W < E && S < N) || error("The region must have W < E and S < N.")
	(E - W <= 360 && -90 <= S && N <= 90) || error("The region is not on the globe.")
	return W, E, S, N
end

# The Variable combo: the file's own arrays once it is on disk, the product's usual ones before.
function _swot_fill_vars!(io::IOBuffer, d::Dict{String,String}, g::Union{SwotGranule,Nothing}, prod::Int)
	f = g === nothing ? "" : _swot_file(g, _swot_dir_of(d))
	vars = isfile(f) ? (try _swot_vars(f) catch; String[] end) : String[]
	usual = _SWOT_PRODUCTS[prod][5]
	# The product's usual ones first, in their order, then the rest of the file's.
	vars = isempty(vars) ? usual : vcat(filter(in(vars), usual), filter(!in(usual), vars))
	cur = _rs_get(d, "p_var")
	_rs_items(io, "p_var", vars)
	k = findfirst(==(cur), vars)
	_rs_set(io, "p_var", k === nothing ? 0 : k - 1)
	lr = _SWOT_PRODUCTS[prod][4] in (:lr, :l3)
	_rs_enable(io, "p_inc", lr)
	_rs_enable(io, "p_xover", _SWOT_PRODUCTS[prod][4] === :lr)   # L2 only: L3 is already calibrated
	return isfile(f)
end

function _swot_label(g::SwotGranule, dir::String)::String
	tile = (m = match(r"_(UTM\d{2}[A-Z])_.*_(\d{3}F)_", g.name)) !== nothing ? "  $(m[1]) $(m[2])" :
	       (m = match(r"_\d{3}_\d{3}_([A-Z]{2})_", g.name)) !== nothing ? "  $(m[1])" : ""
	have = isfile(_swot_file(g, dir)) ? "  (on disk)" : ""
	return "$(GMT.Dates.format(g.t0, "yyyy-mm-dd HH:MM"))  cycle $(g.cycle)  pass $(g.pass)" * tile * have
end

# An empty window gets a geographic map of the region (padded 2 degrees) with its coastline, through
# the Geography menu's own door; a window with content keeps its frame.
function _swot_map_if_empty(scene::Ptr{Cvoid}, W::Float64, E::Float64, S::Float64, N::Float64, title::String)
	_rs_empty(scene) || return nothing
	w, e, s, n = max(W - 2, -180.0), min(E + 2, 180.0), max(S - 2, -90.0), min(N + 2, 90.0)
	_blank_canvas(scene, w, e, s, n, true, title)
	_on_geography(scene, "coast/i/$(w)/$(e)/$(s)/$(n)")
	return nothing
end

# The swaths of the passes found, drawn by every search: ONE line element per pass (its two swath
# edges), all under one group named for the search, so a click on a line names its pass. Each is a
# GUIDE line of this dialog (gmtvtk_overlay_set_tool_pick_h): its menu's "Show this pass (SWOT)" does
# what the dialog's "Pick a pass" button does (show that pass), and it has no clamp/fill/names, which mean
# nothing here. A new search REPLACES the window's previous swaths (one set per window, by its group
# tag). Returns the number of passes drawn.
const _SWOT_SWATHS = Dict{Ptr{Cvoid},String}()     # window -> the group tag of its swaths

function _swot_plot_swaths!(scene::Ptr{Cvoid}, found::Vector{SwotGranule},
                            W::Float64, E::Float64, S::Float64, N::Float64)::Int
	old = pop!(_SWOT_SWATHS, scene, "")
	isempty(old) || ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, old)
	isempty(found) && return 0
	bypass = Dict{Int,Vector{Int}}()
	for g in found
		push!(get!(bypass, g.pass, Int[]), g.cycle)
	end
	perpass = Tuple{String,Vector{GMTdataset{Float64,2}}}[]
	for p in sort!(collect(keys(bypass)))
		cyc = sort!(unique(bypass[p]))
		lab = "Pass $(p) (cycle$(length(cyc) > 1 ? "s" : "") $(join(string.(cyc), ", ")))"
		D = Vector{GMTdataset{Float64,2}}()
		for M in _swot_pass_lines(p, W, E, S, N)
			Dk = GMT.mat2ds(M; hdr = lab)
			Dk.proj4 = GMT.prj4WGS84
			push!(D, Dk)
		end
		isempty(D) || push!(perpass, (lab, D))
	end
	isempty(perpass) && return 0
	_swot_map_if_empty(scene, W, E, S, N, "SWOT passes")
	nm = "SWOT swaths $(GMT.Dates.format(minimum(g.t0 for g in found), "yyyy-mm-dd")) to $(GMT.Dates.format(maximum(g.t0 for g in found), "yyyy-mm-dd"))"
	for (lab, D) in perpass
		_add_dataset_to_scene(scene, D, lab; groupName = nm, noConvertToPoints = true, forceMode = :lines) || continue
		ccall(_fn(:gmtvtk_overlay_set_tool_pick_h), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring, Cstring),
		      scene, lab, "swot", "pickpass", "Show this pass (SWOT)")
	end
	_SWOT_SWATHS[scene] = nm
	return length(perpass)
end

function _swot_fetch(g::SwotGranule, dir::String)::String
	f = _swot_file(g, dir)
	isfile(f) && filesize(f) > 0 && return f
	mkpath(dir)
	_rs_prog_show("Downloading $(basename(f))…")
	try
		if _swot_isl3(g.prod)
			try
				_rs_download(_swot_url(g), f; machine = _SWOT_AVISO, who = "AVISO+")
			catch e
				occursin("401", sprint(showerror, e)) &&
					error("AVISO refused the login (HTTP 401): check the AVISO+ login in the Accounts tab.")
				rethrow()
			end
		else
			_rs_download(_swot_url(g), f; kind = _swot_isshp(g.prod) ? :zip : :netcdf)
		end
	finally
		_rs_prog_close()
	end
	return f
end

function _rs_swot(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	prod = clamp(_rs_idx(d, "p_product") + 1, 1, length(_SWOT_PRODUCTS))
	# The two accounts of the Accounts tab, each one entry of ~/.netrc: (widget prefix, label, machine, name).
	accts = (("p_ed", "lbl_ed_where", _RS_URS, "Earthdata"), ("p_av", "lbl_av_where", _SWOT_AVISO, "AVISO+"))
	if what == "savelogin" || what == "saveaviso"
		pre, lbl, mach, nm = accts[what == "savelogin" ? 1 : 2]
		f = _rs_netrc_path()
		_rs_netrc_set!(f, mach, _rs_get(d, pre * "_user"), _rs_get(d, pre * "_pass"))
		_rs_label(io, lbl, "Saved in $(f) as machine $(mach)")
		_rs_status(io, "$(nm) login saved in $(f) (machine $(mach)).")
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
		_rs_items(io, "p_product", String[p[1] for p in _SWOT_PRODUCTS])
		_rs_set(io, "p_product", 0)
		today = GMT.Dates.Date(RS.now(RS.UTC))
		_rs_set(io, "p_from", "$(today - GMT.Dates.Day(30))T00:00:00")
		_rs_set(io, "p_to", "$(today)T00:00:00")
		_rs_set(io, "p_dir", _swot_dir())
		f = _rs_netrc_path()
		u = ""
		for (k, (pre, lbl, mach, _)) in enumerate(accts)
			uk, pk, why = try
				(_rs_netrc_get(f, mach)..., isfile(f) ? "" : "  (the file does not exist yet)")
			catch e
				("", "", "  — cannot read it: " * sprint(showerror, e))
			end
			k == 1 && (u = uk)
			_rs_set(io, pre * "_user", uk);  _rs_set(io, pre * "_pass", pk)
			_rs_label(io, lbl, "Saved in $(f) as machine $(mach)$(why)")
		end
		# A dialog (re)opened on a window that already has a search — its swaths are on screen and can
		# be picked from their own menus — shows THAT search, never an empty list the swaths cannot match.
		found = get(_SWOT_FOUND, scene, SwotGranule[])
		if isempty(found)
			_rs_items(io, "p_granule", String[])
			_swot_fill_vars!(io, d, nothing, 1)
		else
			_rs_set(io, "p_product", found[1].prod - 1)
			_rs_items(io, "p_granule", String[_swot_label(g, _swot_dir()) for g in found])
			_rs_set(io, "p_granule", 0)
			_swot_fill_vars!(io, d, found[1], found[1].prod)
		end
		_rs_status(io, isempty(u) ? "Downloading needs your NASA Earthdata login: save it in the Accounts tab." :
		                            "Give the region and the period, then Find the passes.")
		return true
	end
	if what == "refresh"
		found = get(_SWOT_FOUND, scene, SwotGranule[])
		k = _rs_idx(d, "p_granule") + 1
		g = 1 <= k <= length(found) ? found[k] : nothing
		# A different product makes the last search's list stale.
		if g !== nothing && g.prod != prod
			delete!(_SWOT_FOUND, scene);  _rs_items(io, "p_granule", String[]);  g = nothing
			_rs_status(io, "Product changed: Find the passes again.")
		end
		_swot_fill_vars!(io, d, g, prod)
		return true
	end
	if what == "search"
		W, E, S, N = _swot_box(d)
		d0 = GMT.Dates.Date(DateTime(_rs_get(d, "p_from")))
		d1 = GMT.Dates.Date(DateTime(_rs_get(d, "p_to")))
		d0 <= d1 || error("The period ends before it starts.")
		l3 = _swot_isl3(prod)
		_rs_prog_show(l3 ? "Reading AVISO's catalogue of the SWOT L3 passes…" : "Asking NASA's CMR for the SWOT files over the region…")
		found, ndrop = try
			l3 ? _swot_l3_search(prod, W, E, S, N, d0, d1) : _swot_search(prod, W, E, S, N, d0, d1)
		finally
			_rs_prog_close()
		end
		_SWOT_FOUND[scene] = found
		dir = _swot_dir_of(d)
		_rs_items(io, "p_granule", String[_swot_label(g, dir) for g in found])
		_rs_set(io, "p_granule", 0)
		_swot_fill_vars!(io, d, isempty(found) ? nothing : found[1], prod)
		npass = _swot_plot_swaths!(scene, found, W, E, S, N)
		_rs_status(io, (isempty(found) ? "No $(_SWOT_PRODUCTS[prod][1]) file over the region in that period." :
		                                 "$(length(found)) file(s), $(npass) pass(es) drawn. To show one: Pick a pass, then click its swath.") *
		           (ndrop > 0 ? " $(ndrop) more $(l3 ? "of those cycles" : "that NASA's search returned") are from passes whose swaths do not cross the region." : ""))
		return true
	end
	if what == "pickpass"
		# A line of the view was pointed at: its Scene Objects name ("Pass 432 (cycles 55, 56)").
		line = _rs_get(d, "picked")
		m = match(r"^Pass (\d+) \(", line)
		m === nothing && error("\"$(line)\" is not a SWOT swath: Find the passes, then click one of the swaths it draws.")
		p = parse(Int, m[1])
		found = get(_SWOT_FOUND, scene, SwotGranule[])
		isempty(found) && error("Find the passes first: the list of files is empty.")
		ks = findall(g -> g.pass == p, found)
		isempty(ks) && error("Pass $(p) has no file in the current list (another product or period): Find the passes again.")
		# Picking a pass SHOWS it: the same as choosing it in the Pass list and pressing "Download and
		# show" (its oldest file when the period holds several cycles; the list has the others).
		picked = found[ks[1]]
	else
		picked = nothing
	end
	what == "download" || what == "open" || what == "pickpass" || error("unknown request: $what")
	g = picked === nothing ? _swot_selected(scene, d) : picked
	dir = _swot_dir_of(d)
	f = _swot_fetch(g, dir)
	found = _SWOT_FOUND[scene]
	_rs_items(io, "p_granule", String[_swot_label(x, dir) for x in found])
	_rs_set(io, "p_granule", findfirst(==(g), found) - 1)
	_swot_fill_vars!(io, d, g, g.prod)
	if what == "download"
		_rs_status(io, "$(f) ($(round(filesize(f) / 2^20, digits = 1)) MB).")
		return true
	end
	var = _rs_get(d, "p_var")
	vars = _swot_vars(f)
	var in vars || error("$(basename(f)) has no \"$(var)\". It has: $(join(vars, ", ")).")
	qmode = clamp(_rs_idx(d, "p_quality"), 0, 2)
	day = GMT.Dates.format(g.t0, "yyyy-mm-dd")
	kind = _SWOT_PRODUCTS[g.prod][4]
	if _swot_isshp(g.prod)
		# Vector data: an overlay on what the window shows, in its frame — never new axes (SACRED_LAW.md,
		# vector import). An empty window gets the region's map first, as for the swaths.
		W, E, S, N = _swot_box(d)
		F = _swot_shp_features(f, var, kind, W, E, S, N, qmode)
		_swot_map_if_empty(scene, W, E, S, N, "SWOT rivers and lakes")
		what_ = kind === :reach ? "river reaches" : kind === :node ? "river nodes" : "lakes"
		cont = (m = match(r"_\d{3}_\d{3}_([A-Z]{2})_", g.name)) === nothing ? "" : " $(m[1])"
		name = "$(var) — SWOT $(what_) cycle $(g.cycle) pass $(g.pass)$(cont) $(day)"
		_add_dataset_to_scene(scene, F, name; forceMode = kind === :node ? :points : :lines)
		n = kind === :node ? size(F[1].data, 1) : length(F)
		_rs_status(io, "$(name): $(n) $(kind === :node ? "nodes" : "features") over the region. Hover one for its $(var).")
		return true
	end
	if kind in (:lr, :l3)
		W, E, S, N = _swot_box(d)
		inc = _rs_num(d, "p_inc", 0.02)
		inc > 0 || error("The increment must be positive.")
		xo = kind === :lr && _rs_get(d, "p_xover") != "0"     # the box is on unless unticked
		_rs_prog_show("Gridding $(var) of pass $(g.pass)…")
		G = try
			_swot_lr_grid(f, var, W, E, S, N, inc, qmode; xover = xo)
		finally
			_rs_prog_close()
		end
		name = "$(var)$(xo && kind === :lr && var in _SWOT_XOVER_VARS ? "" : kind === :lr && var in _SWOT_XOVER_VARS ? " (no xover)" : "") — SWOT$(kind === :l3 ? " L3" : "") cycle $(g.cycle) pass $(g.pass) $(day)"
		recipe = "SWOT $(basename(f)): $(var)$(xo && var in _SWOT_XOVER_VARS ? " + height_cor_xover" : "") gridded at $(inc) over $(W)/$(E)/$(S)/$(N) (nearneighbor)"
	else
		G = _swot_raster_grid(f, var, qmode)
		tile = (m = match(r"_(UTM\d{2}[A-Z])_.*_(\d{3}F)_", g.name)) === nothing ? "" : " $(m[1]) $(m[2])"
		name = "$(var) — SWOT Raster cycle $(g.cycle) pass $(g.pass)$(tile) $(day)"
		recipe = "SWOT $(basename(f)): $(var)"
	end
	_rs_deliver(scene, G, name, recipe)
	_rs_status(io, "$(name) added.")
	return true
end
