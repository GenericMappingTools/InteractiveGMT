# remotes_gui.jl — the Satellite > Remote sensing dialogs, over InteractiveGMT.RemoteS.
#
# ONE callback for all of them (RemoteSDialog, 70_window.cpp; typedef JuliaRemoteSFn in 30_app.cpp).
# The C++ side is generic: it sends every `p_*` widget of the tool's .ui as key=value and applies the
# directives this file answers with. So each tool's behaviour lives HERE, in one function per tool,
# and its .ui is only its face. Wire format, newline-separated both ways:
#
#   request:  tool=<indices|truecolor|calibration|classify|cutcube|modisl2|modisscenes>
#             what=init|refresh|<the run_ button's suffix>
#             <p_widget>=<value>        (combos also send <p_widget>_idx=<0-based index>)
#   answer:   items:<widget>=a\tb\tc    (fill a combo / list)
#             set:<widget>=<value>      (combo: index; check/radio: 0|1; text/plain-text: the text, tabs = new lines;
#                                        rich text (QTextBrowser): HTML, links open in the browser)
#             label:<widget>=<text>     enable:<widget>=0|1
#             status=<one line for the dialog's status label>
#
# Everything computed comes back into the SAME window through the one derived-variable transition
# (`_adopt_derived!`, SACRED_LAW.md); scene footprints are a vector overlay and never reframe.

const RS = RemoteS

# --- small plumbing -----------------------------------------------------------------------------

_rs_get(d::Dict{String,String}, k::String, dflt::String = "")::String = strip(get(d, k, dflt))
_rs_idx(d::Dict{String,String}, k::String)::Int = something(tryparse(Int, _rs_get(d, k * "_idx", "0")), 0)
_rs_on(d::Dict{String,String}, k::String)::Bool = _rs_get(d, k) == "1"
function _rs_num(d::Dict{String,String}, k::String, dflt::Float64)::Float64
	v = tryparse(Float64, _rs_get(d, k))
	return v === nothing ? dflt : v
end

# The file the window is showing, when it came from one: a multi-band open (bandslist.jl) or a cube.
function _rs_window_file(scene::Ptr{Cvoid})::String
	haskey(_BANDS_INFO, scene) && return String(_BANDS_INFO[scene].path)
	haskey(_CUBE_INFO, scene) && return String(_CUBE_INFO[scene].path)
	return ""
end

# One label per band of a raster file: its description, or "Layer k" where it has none.
function _rs_bands(file::String)::Vector{String}
	(isempty(file) || !isfile(file)) && return String[]
	desc = RS._band_descriptions(file)
	return String[isempty(strip(s)) ? "Layer $k" : String(s) for (k, s) in enumerate(desc)]
end

# 0-based index of the first band whose name contains `want` (case-insensitive), else `dflt`.
function _rs_match(names::Vector{String}, want::String, dflt::Int)::Int
	w = lowercase(want)
	k = findfirst(s -> occursin(w, lowercase(s)), names)
	return k === nothing ? dflt : k - 1
end

_rs_items(io::IOBuffer, w::String, v::Vector{String}) = println(io, "items:", w, "=", join(v, '\t'))
_rs_set(io::IOBuffer, w::String, v) = println(io, "set:", w, "=", string(v))
_rs_label(io::IOBuffer, w::String, v::String) = println(io, "label:", w, "=", v)
_rs_enable(io::IOBuffer, w::String, on::Bool) = println(io, "enable:", w, "=", on ? 1 : 0)
_rs_status(io::IOBuffer, v::String) = println(io, "status=", replace(v, '\n' => "  "))

# The source file box: prefilled from the window on `init`, and it must name a file that exists.
function _rs_file!(io::IOBuffer, d::Dict{String,String}, scene::Ptr{Cvoid}, w::String = "p_file")::String
	f = _rs_get(d, w)
	if isempty(f) && _rs_get(d, "what") == "init"
		f = _rs_window_file(scene)
		isempty(f) || _rs_set(io, w, f)
	end
	return f
end

function _rs_need_file(f::String, what::String)::String
	isempty(f) && error("Choose the $what first.")
	isfile(f) || error("No such file: $f")
	return f
end

# --- delivery: into the SAME window ------------------------------------------------------------

_rs_empty(scene::Ptr{Cvoid})::Bool = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0

# A grid: the canonical grid tail (replace a same-named result, promote an empty window, adopt).
_rs_deliver(scene::Ptr{Cvoid}, G::GMTgrid, name::String, recipe::String)::Bool =
	_gm3d_deliver(scene, G, name, "", false, recipe) != 0

# An image: replace a same-named result, add, then the one derived-variable transition.
function _rs_deliver(scene::Ptr{Cvoid}, I::GMTimage, name::String, recipe::String)::Bool
	ccall(_fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
	_forget_object!(scene, :image, name)
	_add_image_to_scene(scene, I, name; promote = _rs_empty(scene))
	_adopt_derived!(scene, name, I)
	return true
end

# --- Spectral indices --------------------------------------------------------------------------

# name, what it measures, the bands it takes (in the order its RemoteS function takes them)
const _RS_INDICES = Tuple{String,String,Vector{String}}[
	("NDVI",   "Normalized difference vegetation",       ["red", "nir"]),
	("EVI",    "Enhanced vegetation",                     ["blue", "red", "nir"]),
	("EVI2",   "Two-band enhanced vegetation",            ["red", "nir"]),
	("SAVI",   "Soil adjusted vegetation",                ["red", "nir"]),
	("MSAVI",  "Modified soil adjusted vegetation",       ["red", "nir"]),
	("SATVI",  "Soil adjusted total vegetation",          ["red", "swir1", "swir2"]),
	("SLAVI",  "Specific leaf area vegetation",           ["red", "nir", "swir2"]),
	("GNDVI",  "Green normalized difference vegetation",  ["green", "nir"]),
	("GLI",    "Green leaf",                              ["red", "green", "blue"]),
	("TGI",    "Triangular greenness",                    ["red", "green", "blue"]),
	("VARI",   "Visible atmospherically resistant",       ["red", "green", "blue"]),
	("CLG",    "Green chlorophyll",                       ["green", "redEdge3"]),
	("CLRE",   "Red-edge chlorophyll",                    ["redEdge1", "redEdge3"]),
	("MTCI",   "MERIS terrestrial chlorophyll",           ["red", "redEdge1", "redEdge2"]),
	("MCARI",  "Modified chlorophyll absorption ratio",   ["green", "red", "redEdge1"]),
	("NDREI1", "Normalized difference red edge",          ["redEdge1", "redEdge2"]),
	("NDREI2", "Normalized difference red edge 2",        ["redEdge1", "redEdge3"]),
	("NDWI",   "Normalized difference water (McFeeters)", ["green", "nir"]),
	("NDWI2",  "Normalized difference water (Gao)",       ["nir", "swir2"]),
	("MNDWI",  "Modified normalized difference water",    ["green", "swir2"]),
	("NBRI",   "Normalized burn ratio",                   ["nir", "swir2"]),
]

function _rs_indices(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	file = _rs_file!(io, d, scene)
	k = clamp(_rs_idx(d, "p_index") + 1, 1, length(_RS_INDICES))
	ix, _, want = _RS_INDICES[k]
	if what == "init"
		_rs_items(io, "p_index", String["$(n) — $(t)" for (n, t, _) in _RS_INDICES])
		_rs_set(io, "p_index", 0)
	end
	if what == "init" || what == "refresh"
		names = _rs_bands(file)
		for j in 1:3
			w = "p_b$j"
			on = j <= length(want)
			_rs_label(io, "lbl_b$j", on ? want[j] : "—")
			_rs_enable(io, w, on && !isempty(names))
			_rs_items(io, w, names)
			on && !isempty(names) && _rs_set(io, w, _rs_match(names, want[j], min(j, length(names)) - 1))
		end
		_rs_status(io, isempty(names) ? "Choose a multi-band file (a cube made by Band cube, or any band stack)." :
		                                "$(length(names)) bands. $(ix) takes: $(join(want, ", ")).")
		return true
	end
	what == "compute" || error("unknown request: $what")
	_rs_need_file(file, "multi-band file")
	layers = Int[_rs_idx(d, "p_b$j") + 1 for j in 1:length(want)]
	mode = _rs_get(d, "p_output")
	kw = Pair{Symbol,Any}[]
	if !startswith(mode, "Index")
		if startswith(mode, "Classes")
			cls = Float64[parse(Float64, s) for s in split(_rs_get(d, "p_classes"), ',') if !isempty(strip(s))]
			isempty(cls) && error("Give the class separators, e.g. 0.2, 0.4, 0.6")
			push!(kw, :classes => cls)
		else
			thr = tryparse(Float64, _rs_get(d, "p_threshold"))
			thr === nothing && error("Give a threshold value.")
			push!(kw, :threshold => thr)
			startswith(mode, "Mask (≥") && push!(kw, :mask => 1)
			startswith(mode, "Mask (<") && push!(kw, :mask => -1)
		end
	end
	sc = RS.subcube(file; layers = layers)
	b3 = length(layers) == 3 ? view(sc, :, :, 3) : nothing
	O = RS.sp_indices(view(sc, :, :, 1), view(sc, :, :, 2), b3; index = ix, kw...)
	# Each output kind is its own result: a mask must never replace the index grid it came from.
	what_out = isempty(kw) ? "" : haskey(Dict(kw), :classes) ? " classes" :
	           haskey(Dict(kw), :mask) ? " mask $(Dict(kw)[:mask] > 0 ? "≥" : "<") $(Dict(kw)[:threshold])" :
	                                     " ≥ $(Dict(kw)[:threshold])"
	name = "$(ix)$(what_out) — $(splitext(basename(file))[1])"
	_rs_deliver(scene, O, name, "RemoteS.$(lowercase(ix))(\"$(basename(file))\", layers=$(layers))")
	_rs_status(io, "$(name) added.")
	return true
end

# --- True color ----------------------------------------------------------------------------------

function _rs_truecolor(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	file = _rs_file!(io, d, scene)
	if what == "init" || what == "refresh"
		names = _rs_bands(file)
		for (j, (w, c)) in enumerate((("p_r", "red"), ("p_g", "green"), ("p_bl", "blue")))
			_rs_items(io, w, names)
			isempty(names) || _rs_set(io, w, _rs_match(names, c, min(j, length(names)) - 1))
		end
		_rs_status(io, isempty(names) ? "Choose a multi-band file." : "$(length(names)) bands.")
		return true
	end
	what == "compute" || error("unknown request: $what")
	_rs_need_file(file, "multi-band file")
	layers = Int[_rs_idx(d, w) + 1 for w in ("p_r", "p_g", "p_bl")]
	I = RS.truecolor(file; layers = layers, stretch = _rs_on(d, "p_stretch"))
	I isa GMTimage || error("truecolor returned a $(typeof(I)), not an image")
	name = "True color $(layers) — $(splitext(basename(file))[1])"
	_rs_deliver(scene, I, name, "RemoteS.truecolor(\"$(basename(file))\", layers=$(layers))")
	_rs_status(io, "$(name) added.")
	return true
end

# --- Landsat calibration -----------------------------------------------------------------------

function _rs_calibration(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	file = _rs_file!(io, d, scene)
	if what == "init" || what == "refresh"
		names = _rs_bands(file)
		if length(names) > 1
			_rs_items(io, "p_band", names)
			_rs_enable(io, "p_band", true)
			_rs_status(io, "A cube: pick the band. Its MTL is read from the cube's own metadata.")
		else
			_rs_items(io, "p_band", String["(the band this file holds)"])
			_rs_enable(io, "p_band", false)
			_rs_status(io, isempty(names) ? "Choose a Landsat 8/9 band file (…_B5.TIF) or a Band cube." :
			                                "A single Landsat band: its number and MTL come from the file name.")
		end
		return true
	end
	what == "compute" || error("unknown request: $what")
	_rs_need_file(file, "Landsat band or cube")
	names = _rs_bands(file)
	band = 0
	if length(names) > 1
		lab = names[clamp(_rs_idx(d, "p_band") + 1, 1, length(names))]
		m = match(r"Band\s*(\d+)", lab)
		m === nothing && error("Band \"$lab\" has no Landsat band number in its description.")
		band = parse(Int, m.captures[1])
	end
	mtl = _rs_get(d, "p_mtl")
	q = _rs_get(d, "p_quantity")
	G, tag = startswith(q, "Radiance")    ? (RS.dn2radiance(file; band = band, mtl = mtl), "Radiance TOA") :
	         startswith(q, "Reflectance") ? (RS.dn2reflectance(file; band = band, mtl = mtl), "Reflectance TOA") :
	         startswith(q, "Surface")     ? (RS.reflectance_surf(file; band = band, mtl = mtl), "Surface reflectance") :
	                                        (RS.dn2temperature(file; band = band, mtl = mtl), "Brightness temperature")
	G isa GMTgrid || error("got a $(typeof(G)), not a grid")
	name = "$(tag)$(band > 0 ? " B$(band)" : "") — $(splitext(basename(file))[1])"
	_rs_deliver(scene, G, name, "RemoteS $(tag) of \"$(basename(file))\"")
	_rs_status(io, "$(name) added.")
	return true
end

# --- Supervised classification ----------------------------------------------------------------

function _rs_classify(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	file = _rs_file!(io, d, scene)
	if what == "init" || what == "refresh"
		n = length(_rs_bands(file))
		_rs_status(io, n == 0 ? "Choose the band cube to classify." :
		           "$(n) bands. Training polygons need a \"class\" attribute (e.g. > Attrib(class=water)).")
		return true
	end
	what == "compute" || error("unknown request: $what")
	_rs_need_file(file, "band cube")
	train = _rs_need_file(_rs_get(d, "p_train"), "training polygons file")
	cube = GMT.gmtread(file)
	cube isa GMT.GItype && ndims(cube) == 3 || error("$(basename(file)) is not a multi-band cube")
	depth = round(Int, _rs_num(d, "p_depth", 3.0))
	dens = _rs_num(d, "p_density", 0.1)
	model, classes = RS.train_raster(cube, train; density = dens, max_depth = depth)
	I = RS.classify(cube, model; class_names = classes)
	name = "Classes — $(splitext(basename(file))[1])"
	_rs_deliver(scene, I, name, "RemoteS.classify(\"$(basename(file))\", \"$(basename(train))\")")
	_rs_status(io, "$(name): $(classes).")
	return true
end

# --- Band cube (cut) -----------------------------------------------------------------------------

function _rs_cutcube(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	if what == "init" || what == "refresh"
		n = count(!isempty, strip.(split(_rs_get(d, "p_names"), ';')))
		_rs_status(io, n == 0 ? "Choose the band files of one scene." : "$(n) band file(s).")
		return true
	end
	what == "compute" || error("unknown request: $what")
	names = String[String(strip(s)) for s in split(_rs_get(d, "p_names"), ';') if !isempty(strip(s))]
	isempty(names) && error("Choose the band files of one scene.")
	save = _rs_get(d, "p_save")
	isempty(save) && error("Choose the output file.")
	reg = _rs_get(d, "p_region")
	s2 = _rs_get(d, "p_sentinel")
	sent = startswith(s2, "10") ? 10 : startswith(s2, "20") ? 20 : startswith(s2, "60") ? 60 : 0
	RS.cutcube(names = names, region = isempty(reg) ? nothing : reg, sentinel2 = sent, save = save)
	isfile(save) || error("cutcube wrote nothing to $save")
	_on_drop(scene, save)			# the ONE open-a-file-into-a-window path
	_rs_status(io, "$(basename(save)) written ($(length(names)) bands) and opened.")
	return true
end

# --- MODIS L2 swath to grid ---------------------------------------------------------------------

# Is `path` a Level-2 swath (what "MODIS L2 swath to grid" grids)? By CONTENT, not name: an OB.DAAC
# L2 file says so itself (global processing_level = L2) and carries the swath's navigation_data
# longitude/latitude arrays. A plain netCDF grid, a Level-3 map or an Aquamoto file answers false.
# The variable picked in the netCDF picker for a dropped L2 swath (drop.jl), keyed by the file's
# abspath; the tool's next init/refresh on that file selects it in p_var and forgets it.
const _RS_L2_PICKED = Dict{String,String}()

function _rs_is_l2_swath(path::String)::Bool
	lowercase(splitext(path)[2]) in (".nc", ".nc4", ".h5", ".hdf", ".he5") || return false
	isfile(path) || return false
	s = try GMT.gdalinfo(path) catch; return false end
	s isa AbstractString || return false
	return occursin(r"NC_GLOBAL#processing_level=L2\b", s) &&
	       occursin("navigation_data/longitude", s) && occursin("navigation_data/latitude", s)
end

function _rs_modisl2(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	file = _rs_get(d, "p_file")
	if what == "init" || what == "refresh"
		# A new file fills the arrays AND the increment + region "Grid it" would use on its own
		# (RS.sensor_limits: grid_at_sensor's own estimate). Nothing is gridded until "Grid it".
		vars = (isempty(file) || !isfile(file)) ? String[] : RS.sds_names(file)
		_rs_items(io, "p_var", vars)
		if isempty(vars)
			_rs_status(io, "Choose a MODIS (or other swath) L2 netCDF file.")
			return true
		end
		picked = isempty(file) ? nothing : pop!(_RS_L2_PICKED, abspath(file), nothing)
		k = something((picked === nothing ? nothing : findfirst(==(picked), vars)),
		              findfirst(v -> v in ("sst", "chlor_a"), vars), 1)
		_rs_set(io, "p_var", k - 1)
		note = try
			# ONE increment for x and y, 0.02 degrees as the first choice (the swath's own spacing gives
			# unequal dx/dy); the region is rounded on it, as grid_at_sensor rounds on the increment.
			inc, (W, E, S, N) = RS.sensor_limits(file, vars[k]; quality = _rs_idx(d, "p_quality"), inc = [0.02, 0.02])
			_rs_set(io, "p_inc", string(inc[1]))
			for (w, v) in (("p_rg_w", W), ("p_rg_e", E), ("p_rg_s", S), ("p_rg_n", N))
				_rs_set(io, w, string(round(v, sigdigits = 10)))   # W + n*inc: no -0.21999999999999886 in a box
			end
			" Increment and region are those of the swath of \"$(vars[k])\"; edit them, then press Grid it."
		catch e
			" (Could not estimate the increment and region: $(sprint(showerror, e)))"
		end
		_rs_status(io, "$(length(vars)) arrays in the file." * note)
		return true
	end
	what == "compute" || error("unknown request: $what")
	_rs_need_file(file, "L2 file")
	var = _rs_get(d, "p_var")
	isempty(var) && error("Pick the array to grid.")
	kw = Pair{Symbol,Any}[]
	inc = _rs_get(d, "p_inc")
	isempty(inc) || push!(kw, :inc => RS._inc_arg(String(inc)))
	lims = [_rs_get(d, w) for w in ("p_rg_w", "p_rg_e", "p_rg_s", "p_rg_n")]
	if all(!isempty, lims)
		W, E, S, N = (parse(Float64, x) for x in lims)
		(W < E && S < N) || error("The region must have W < E and S < N.")
		push!(kw, :region => join(lims, '/'))
	elseif any(!isempty, lims)
		error("Give all four limits of the region, or none (then the swath's extent is used).")
	end
	G = RS.grid_at_sensor(file, var; quality = _rs_idx(d, "p_quality"), kw...)
	G isa GMTgrid || error("got a $(typeof(G)), not a grid")
	name = "$(var) — $(splitext(basename(file))[1])"
	_rs_deliver(scene, G, name, "RemoteS.grid_at_sensor(\"$(basename(file))\", \"$(var)\")")
	_rs_status(io, "$(name) gridded and added.")
	return true
end

# --- MODIS scenes (Terra / Aqua) ---------------------------------------------------------------
# The RemoteS gallery's last example, for any date: the MODIS scene footprints a satellite paints in a
# time window, and the scenes that cover a point. The orbit is iGMT's own SGP4 (RS.sat_tracks).

const _RS_NORAD = Dict{String,String}("AQUA" => "27424", "TERRA" => "25994")
# Celestrak's Earth-resources group: checked to carry both TERRA and AQUA (the same URL the Satellite
# orbits dialog lists as "Earth observation — all").
const _RS_CELESTRAK = "https://celestrak.org/NORAD/elements/gp.php?GROUP=resource&FORMAT=tle"

_rs_norad(t::TLE)::String = strip(t.line1[3:7])

# The epoch (JD) as line 1 writes it: cols 19-20 the year (57-99 = 19xx), 21-32 the day of the year
# with its fraction. Read off the text — a satellite's whole history can be thousands of sets.
function _rs_tle_epoch(t::TLE)::Float64
	yy = parse(Int, t.line1[19:20])
	return jd(DateTime(yy < 57 ? 2000 + yy : 1900 + yy)) + parse(Float64, t.line1[21:32]) - 1
end

# --- Space-Track history: the element sets of past dates ----------------------------------------
# Celestrak serves only the CURRENT elements; Space-Track keeps every set ever published (class
# gp_history). Asked by calendar MONTH, one satellite at a time, and every answer is KEPT: one file per
# satellite (<NORAD>.tle, all its sets) plus the months already complete (<NORAD>.months). A month on
# disk is never asked again — Space-Track limits automated use and asks that history be fetched once.
# The current month is not complete: it is asked again at most every 2 h.

const _RS_ST = "www.space-track.org"
const _RS_ST_LOGIN = "https://www.space-track.org/ajaxauth/login"
const _RS_ST_RECENT = Dict{String,Float64}()          # "<NORAD> yyyy-mm" of an incomplete month => time asked

_rs_st_dir()::String = get(ENV, "IGMT_TLE_HISTORY", joinpath(homedir(), ".gmt", "iGMT", "tle_history"))

# application/x-www-form-urlencoded value.
_rs_form(s::String)::String = join((isletter(Char(b)) && b < 0x80) || isdigit(Char(b)) || b in UInt8.(('-', '_', '.', '~')) ?
                                   string(Char(b)) : "%" * uppercase(string(b, base = 16, pad = 2)) for b in codeunits(s))

# Space-Track's session: log in (POST identity/password -> a session cookie), ask, log out. The
# one-request login+query form is deprecated ("Single command deprecated. See API help for cookie use").
function _rs_st_login(user::String, pass::String)::String
	out = IOBuffer()
	r = Downloads.request(_RS_ST_LOGIN; method = "POST", output = out,
	                      input = IOBuffer("identity=$(_rs_form(user))&password=$(_rs_form(pass))"),
	                      headers = ["Content-Type" => "application/x-www-form-urlencoded"])
	txt = String(take!(out))
	(r.status != 200 || occursin("\"Login\":\"Failed\"", txt)) &&
		error("Space-Track refused the login (HTTP $(r.status)): check it in the Accounts tab.")
	ck = String[first(split(v, ';')) for (k, v) in r.headers if lowercase(k) == "set-cookie"]
	isempty(ck) && error("Space-Track accepted the login but gave no session cookie.")
	return join(ck, "; ")
end

function _rs_st_get(url::String, cookie::String)::Tuple{Int,String}
	out = IOBuffer()
	r = Downloads.request(url; output = out, headers = ["Cookie" => cookie])
	return r.status, String(take!(out))
end

# The gp_history sets of `norad` with epochs in each [d0, d1] of `spans`, as TLE text, in ONE session.
function _rs_st_query(norad::String, spans::Vector{Tuple{GMT.Dates.Date,GMT.Dates.Date}},
                      user::String, pass::String)::Vector{String}
	cookie = _rs_st_login(user, pass)
	try
		out = String[]
		for (d0, d1) in spans
			q = "https://www.space-track.org/basicspacedata/query/class/gp_history/NORAD_CAT_ID/$(norad)/EPOCH/" *
			    "$(d0)--$(d1 + GMT.Dates.Day(1))/orderby/EPOCH%20asc/format/tle"
			st, txt = _rs_st_get(q, cookie)
			st == 200 || error("Space-Track answered HTTP $(st): " * first(strip(txt), 200))
			(isempty(strip(txt)) || occursin(r"(?m)^1 ", txt)) ||
				error("Space-Track answered something that is not TLE: " * first(strip(txt), 200))
			push!(out, txt)
		end
		return out
	finally
		try _rs_st_get("https://www.space-track.org/ajaxauth/logout", cookie) catch end
	end
end

# (line1, line2) pairs of a TLE text (2- or 3-line sets).
function _rs_tle_pairs(txt::String)::Vector{Tuple{String,String}}
	ls = [rstrip(l) for l in split(txt, '\n')]
	out = Tuple{String,String}[]
	for i in 1:length(ls) - 1
		startswith(ls[i], "1 ") && startswith(ls[i + 1], "2 ") && push!(out, (String(ls[i]), String(ls[i + 1])))
	end
	return out
end

# Every set on disk for `norad`, after making sure the months around `when` (±3 days) are there.
_rs_st_history(norad::String, when::DateTime, user::String, pass::String)::String =
	_rs_st_history(norad, when - GMT.Dates.Day(3), when + GMT.Dates.Day(3), user, pass)

# ...and for a whole period: the months covering [t0, t1].
function _rs_st_history(norad::String, t0::DateTime, t1::DateTime, user::String, pass::String)::String
	dir = _rs_st_dir();  mkpath(dir)
	ftle, fmon = joinpath(dir, "$(norad).tle"), joinpath(dir, "$(norad).months")
	done = isfile(fmon) ? Set(String.(split(read(fmon, String)))) : Set{String}()
	have = isfile(ftle) ? _rs_tle_pairs(read(ftle, String)) : Tuple{String,String}[]
	seen = Set(p[1] for p in have)
	today = GMT.Dates.Date(RS.now(RS.UTC))
	# The months of [t0, t1] still to ask for: not complete on disk, not in the future, and (the
	# current month) not asked in the last 2 h.
	need = Tuple{GMT.Dates.Date,GMT.Dates.Date}[]
	d = GMT.Dates.firstdayofmonth(GMT.Dates.Date(t0))
	while d <= GMT.Dates.Date(t1)
		d1 = GMT.Dates.lastdayofmonth(d)
		key = GMT.Dates.format(d, "yyyy-mm")
		!(key in done) && d <= today && time() - get(_RS_ST_RECENT, "$(norad) $(key)", -Inf) > 7200.0 &&
			push!(need, (d, d1))
		d = d1 + GMT.Dates.Day(1)
	end
	if !isempty(need)
		(isempty(user) || isempty(pass)) &&
			error("Space-Track history needs your Space-Track login: save it in the Accounts tab.")
		for ((d0, d1), txt) in zip(need, _rs_st_query(norad, need, user, pass))
			for p in _rs_tle_pairs(txt)
				p[1] in seen || (push!(have, p); push!(seen, p[1]))
			end
			key = GMT.Dates.format(d0, "yyyy-mm")
			# Complete = the month ended more than a day ago (late sets of its last day are in).
			d1 < today - GMT.Dates.Day(1) ? push!(done, key) : (_RS_ST_RECENT["$(norad) $(key)"] = time())
		end
		sort!(have, by = p -> _rs_tle_epoch(TLE("", p[1], p[2])))     # by epoch (TERRA's history starts in 1999)
		write(ftle, join(("$(a)\n$(b)" for (a, b) in have), "\n") * "\n")
		write(fmon, join(sort!(collect(done)), "\n") * "\n")
	end
	return isfile(ftle) ? read(ftle, String) : ""
end

# The TLE for `sat` nearest to `when` (JD), from the chosen source, and its epoch.
function _rs_modis_tle(d::Dict{String,String}, sat::String, when::Float64)::Tuple{TLE, Float64}
	t = datetime(when)
	tles, eps = _rs_tle_sets(d, sat, t - GMT.Dates.Day(3), t + GMT.Dates.Day(3))
	k = argmin(abs.(eps .- when))
	return tles[k], eps[k]
end

# Every element set of `sat` the chosen source has for [t0, t1], and their epochs (JD).
function _rs_tle_sets(d::Dict{String,String}, sat::String, t0::DateTime, t1::DateTime)::Tuple{Vector{TLE},Vector{Float64}}
	src = _rs_get(d, "p_tlesrc")
	txt = if startswith(src, "TLE file")
		_rs_need_file(_rs_get(d, "p_tlefile"), "TLE file")
	elseif startswith(src, "Space-Track")
		u, p = _rs_netrc_get(_rs_netrc_path(), _RS_ST)
		_rs_st_history(_RS_NORAD[sat], t0, t1, u, p)
	else
		_tle_fetch(_RS_CELESTRAK)
	end
	tles = filter(t -> _rs_norad(t) == _RS_NORAD[sat], read_tle(txt))
	isempty(tles) && error("No $(sat) (NORAD $(_RS_NORAD[sat])) element set in that source.")
	return tles, Float64[_rs_tle_epoch(t) for t in tles]
end

# The scene files as clickable download links: OB.DAAC's getfile + the file name, the address its own
# Level-2 listing links every file to.
const _RS_OBDAAC = "https://oceandata.sci.gsfc.nasa.gov/getfile/"
# OB.DAAC's file search: the files matching a name pattern, one per line ("No Results Found" when
# none). Asked per granule — a few seconds each for a recent date — where a day's directory page takes
# ~11 s just to be generated. Answers are kept 10 min: a granule not yet processed may appear later.
const _RS_FILESEARCH = "https://oceandata.sci.gsfc.nasa.gov/api/file_search"
const _RS_FOUND = Dict{String,Tuple{Float64,Vector{String}}}()     # search pattern => (time asked, names)
const _RS_FOUND_SECS = 600.0

function _rs_file_search(pattern::String, sensor::String)::Vector{String}
	c = get(_RS_FOUND, pattern, nothing)
	(c !== nothing && time() - c[1] < _RS_FOUND_SECS) && return c[2]
	io = IOBuffer()
	Downloads.download("$(_RS_FILESEARCH)?search=$(pattern)&dtype=L2&sensor=$(sensor)&results_as_file=1", io)
	names = String[strip(l) for l in split(String(take!(io)), '\n') if endswith(strip(l), ".nc")]
	_RS_FOUND[pattern] = (time(), names)
	return names
end

# A computed name -> the name OB.DAAC really gave that granule. The minute is the orbit's; the seconds
# (:00 or :01, the time of the first scan) and NRT vs refined are only known from OB.DAAC itself.
# The refined file is preferred when both exist. Returns (name, how): how = :found, :missing (OB.DAAC
# answered and has no such granule) or :unread (it could not be asked; the name is a guess).
function _rs_resolve_scene(n::String)::Tuple{String,Symbol}
	m = match(r"^(AQUA|TERRA)_MODIS\.(\d{8})T(\d{4})\d\d\.L2\.([A-Z0-9]+)\.", n)
	m === nothing && return n, :unread
	sat, ymd, hm, prod = m[1], m[2], m[3], m[4]
	got = try
		_rs_file_search("$(sat)_MODIS.$(ymd)T$(hm)*.L2.$(prod).*", lowercase(sat))
	catch
		return n, :unread
	end
	hits = filter(h -> occursin(Regex("^$(sat)_MODIS\\.$(ymd)T$(hm)\\d\\d\\.L2\\.$(prod)\\.(NRT\\.)?nc\$"), h), got)
	isempty(hits) && return n, :missing
	k = findfirst(h -> !occursin(".NRT.", h), hits)
	return hits[k === nothing ? 1 : k], :found
end

# The MODIS scenes runs' progress: the app's determinate progress dialog (message in its body, not in
# its title bar), 0..100. It replaces the dialog's generic busy notice as soon as Julia has the call.
_rs_prog_show(label::String) = ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), Cint(100), label)
_rs_prog(v::Int, label::String) = ccall(_fn(:gmtvtk_progress_status), Cvoid, (Cint, Cstring), Cint(v), label)
_rs_prog_close() = ccall(_fn(:gmtvtk_progress_close), Cvoid, ())

# The found panel, right-justified: a link per granule OB.DAAC has; one it lacks is left out (counted
# for the status line). An unchecked guess (OB.DAAC unreachable) is shown, marked. `p0` = the progress
# already spent; the answers fill the rest of the bar, one step per granule, with the seconds waited
# ticking in the message so a slow answer never looks like a hang.
function _rs_scene_links(names::Vector{String}, p0::Int = -1)::Tuple{String,Int,Int}
	rows = String[];  nmiss = 0;  nunread = 0
	n = length(names);  done = Ref(0);  t0 = time();  finished = Ref(false)
	msg() = "Asking OB.DAAC for the file names: $(done[]) of $(n) answered ($(round(Int, time() - t0)) s)"
	if p0 >= 0 && n > 0
		_rs_prog(p0, msg())
		@async while !finished[]
			sleep(0.25)
			finished[] || _rs_prog(-1, msg())
		end
	end
	one(x) = (r = _rs_resolve_scene(x); done[] += 1;
	          p0 >= 0 && _rs_prog(p0 + round(Int, (100 - p0) * done[] / n), msg()); r)
	# All granules asked at once: the wait is the slowest answer, not the sum of them.
	res = try
		asyncmap(one, names; ntasks = 8)
	finally
		finished[] = true
	end
	for (r, how) in res
		if how === :found
			push!(rows, "<a href=\"$(_RS_OBDAAC)$(r)\">$(_RS_OBDAAC)$(r)</a>")
		elseif how === :missing
			nmiss += 1
		else
			nunread += 1;  push!(rows, "(unchecked) <a href=\"$(_RS_OBDAAC)$(r)\">$(_RS_OBDAAC)$(r)</a>")
		end
	end
	return isempty(rows) ? "" : "<div align=\"right\">" * join(rows, "<br>") * "</div>", nmiss, nunread
end

# --- Region tab: every scene over a rectangle in a period, as a download script ------------------
# NASA's CMR granule search does the spatial and day/night selection on its side, from each file's own
# footprint: only the region's files come back (a month of Terra SST over Iberia: 0.7 s), exact names,
# no orbit and no orbital elements involved. OB.DAAC files each product in two collections, the
# reprocessed one and the near-real-time one; both are asked and the reprocessed file wins.

const _RS_CMR = "https://cmr.earthdata.nasa.gov/search/granules.json"
# The Region tab's sensors — those of the Oceancolor dialog — as (Satellite combo text, CMR collection
# stem, products). Collection = stem * "_" * product [* "_NRT"]; verified in CMR 2026-09-28. OLCI has
# no thermal bands (no SST) and is taken at its reduced resolution (ERR, 1.2 km), as Oceancolor does.
const _RS_CMR_SENSORS = Tuple{String,String,Vector{String}}[
	("Aqua (MODIS)",       "MODISA_L2",       ["SST", "OC"]),
	("Terra (MODIS)",      "MODIST_L2",       ["SST", "OC"]),
	("Suomi-NPP (VIIRS)",  "VIIRSN_L2",       ["SST", "OC"]),
	("NOAA-20 (VIIRS)",    "VIIRSJ1_L2",      ["SST", "OC"]),
	("Sentinel-3A (OLCI)", "OLCIS3A_L2_ERR",  ["OC"]),
	("Sentinel-3B (OLCI)", "OLCIS3B_L2_ERR",  ["OC"]),
]
_rs_cmr_collection(stem::String, prod::String, nrt::Bool)::String = stem * "_" * prod * (nrt ? "_NRT" : "")

# Every granule name of `coll` over the box in [t0, t1], paged 2000 at a time (CMR-Search-After).
function _rs_cmr_names(coll::String, W::Float64, E::Float64, S::Float64, N::Float64,
                       t0::String, t1::String, dn::String)::Vector{String}
	url = "$(_RS_CMR)?short_name=$(coll)&bounding_box=$(W),$(S),$(E),$(N)&temporal=$(t0),$(t1)&page_size=2000" *
	      (isempty(dn) ? "" : "&day_night_flag=$(dn)")
	names = String[];  after = ""
	while true
		out = IOBuffer()
		r = Downloads.request(url; output = out, headers = isempty(after) ? Pair{String,String}[] : ["CMR-Search-After" => after])
		r.status == 200 || error("NASA's CMR answered HTTP $(r.status) for $(coll).")
		page = String[m.captures[1] for m in eachmatch(r"\"producer_granule_id\":\"([^\"]+)\"", String(take!(out)))]
		append!(names, page)
		nxt = [v for (k, v) in r.headers if lowercase(k) == "cmr-search-after"]
		(length(page) < 2000 || isempty(nxt)) && break
		after = nxt[1]
	end
	return names
end

# One OB.DAAC file to `path`, logged in with the Earthdata entry of .netrc: getfile redirects to
# urs.earthdata.nasa.gov, and curl answers it from the netrc there (what wget --auth-no-challenge
# does in the scripts), with the session cookie kept along the redirect chain. Written to a ".part"
# file and checked to be netCDF/HDF5 before it takes the name: a refused login sends back a web page.
function _rs_download(url::String, path::String)::Nothing
	nrc = _rs_netrc_path()
	u, p = _rs_netrc_get(nrc, _RS_URS)
	(isempty(u) || isempty(p)) && error("Downloading needs your Earthdata login: save it in the Accounts tab.")
	C = Downloads.Curl
	dl = Downloads.Downloader()
	dl.easy_hook = (easy, info) -> begin
		C.setopt(easy, C.CURLOPT_NETRC, C.CURL_NETRC_OPTIONAL)
		C.setopt(easy, C.CURLOPT_NETRC_FILE, nrc)
		C.setopt(easy, C.CURLOPT_COOKIEFILE, "")
		C.setopt(easy, C.CURLOPT_UNRESTRICTED_AUTH, 1)
	end
	part = path * ".part";  tl = Ref(0.0)
	prog(total, now) = (time() - tl[] > 0.25 && total > 0) &&
		(tl[] = time(); _rs_prog(round(Int, 100 * now / total),
		                         "Downloading $(basename(path)): $(round(now / 2^20, digits = 1)) of $(round(total / 2^20, digits = 1)) MB"))
	try
		Downloads.download(url, part; downloader = dl, progress = prog)
		head = open(io -> read(io, 4), part)
		(head == UInt8[0x89, 0x48, 0x44, 0x46] || head[1:3] == UInt8[0x43, 0x44, 0x46]) ||
			error("OB.DAAC did not send a netCDF file (a refused Earthdata login sends a web page): check the login in the Accounts tab.")
		mv(part, path; force = true)
	finally
		isfile(part) && rm(part; force = true)
	end
	return nothing
end

# The script's download line, in the form of the user's own geta.bat.
function _rs_wget_line(name::String, user::String, pass::String)::String
	if Sys.iswindows()
		q(s) = "\"" * replace(s, "%" => "%%") * "\""
		return "wget -nc --waitretry=5 --retry-on-http-error=429 --user=$(q(user)) --password=$(q(pass)) --auth-no-challenge=on $(_RS_OBDAAC)$(name)"
	end
	sq(s) = "'" * replace(s, "'" => "'\\''") * "'"
	return "wget -nc --waitretry=5 --retry-on-http-error=429 --user=$(sq(user)) --password=$(sq(pass)) --auth-no-challenge=on $(_RS_OBDAAC)$(name)"
end

function _rs_region_run(d::Dict{String,String}, io::IOBuffer)::Bool
	W, E, S, N = (_rs_num(d, k, NaN) for k in ("p_rg_w", "p_rg_e", "p_rg_s", "p_rg_n"))
	any(isnan, (W, E, S, N)) && error("Give the four limits of the region.")
	(W < E && S < N) || error("The region must have W < E and S < N.")
	day0 = GMT.Dates.Date(DateTime(_rs_get(d, "p_rg_from")))
	day1 = GMT.Dates.Date(DateTime(_rs_get(d, "p_rg_to")))
	day0 <= day1 || error("The period ends before it starts.")
	script = _rs_get(d, "p_rg_script")          # empty: the script opens in a text window instead
	eu, ep = _rs_netrc_get(_rs_netrc_path(), _RS_URS)
	(isempty(eu) || isempty(ep)) && error("The script needs your Earthdata login: save it in the Accounts tab.")
	prod = startswith(_rs_get(d, "p_rg_product"), "Chlor") ? "OC" : "SST"
	lab = _rs_get(d, "p_rg_sat", "Aqua (MODIS)")
	k = findfirst(t -> t[1] == lab, _RS_CMR_SENSORS)
	k === nothing && error("Unknown satellite \"$(lab)\".")
	sat, stem, prods = _RS_CMR_SENSORS[k]
	prod in prods || error("$(sat) has no $(prod) product (OLCI has no thermal bands): choose Chlorophyll-a.")
	w = _rs_get(d, "p_rg_daynight")
	dn = startswith(w, "Day") ? "day" : startswith(w, "Night") ? "night" : ""
	t0, t1 = "$(day0)T00:00:00Z", "$(day1)T23:59:59Z"

	_rs_prog(20, "Asking NASA's CMR for the $(sat) $(prod) scenes over the region…")
	got = asyncmap(nrt -> _rs_cmr_names(_rs_cmr_collection(stem, prod, nrt), W, E, S, N, t0, t1, dn), (false, true))
	_rs_prog(90, "Writing the script…")
	# One file per scene: the reprocessed one when both exist (the NRT name is the same with ".NRT").
	byscene = Dict{String,String}()
	for n in Iterators.flatten(got)
		key = replace(n, ".NRT.nc" => ".nc")
		old = get(byscene, key, "")
		(isempty(old) || (occursin(".NRT.", old) && !occursin(".NRT.", n))) && (byscene[key] = n)
	end
	names = sort!(collect(values(byscene)), rev = true)

	head = Sys.iswindows() ? "@echo off" : "#!/bin/sh"
	lines = vcat(head, String[_rs_wget_line(n, eu, ep) for n in names])
	if isempty(script)
		# No file named: the whole script in a text window (tabs = new lines on the wire), saved from there.
		println(io, "editor:", Sys.iswindows() ? "getmodis.bat" : "getmodis.sh", "=", join(lines, '\t'))
		where = "shown in the text window: save it from there"
	else
		eol = Sys.iswindows() ? "\r\n" : "\n"
		write(script, join(lines, eol) * eol)
		Sys.iswindows() || chmod(script, 0o755)
		where = "written to $(script)"
	end
	_rs_status(io, "$(length(names)) $(sat) $(prod) scene(s) over $(W)/$(E)/$(S)/$(N), $(day0) to $(day1), $(where).")
	return true
end


function _rs_links_note(nmiss::Int, nunread::Int)::String
	s = nmiss > 0 ? " $(nmiss) not (yet) at OB.DAAC: not processed yet, or no ocean in it." : ""
	nunread > 0 && (s *= " $(nunread) unchecked: OB.DAAC could not be reached, so their seconds (:00/:01) are a guess.")
	return s
end

function _rs_modis_when(d::Dict{String,String})::DateTime
	s = _rs_get(d, "p_start")
	isempty(s) && return RS.now(RS.UTC)
	return DateTime(s)
end

# --- the NASA Earthdata account (OB.DAAC downloads) ---------------------------------------------
# Kept where curl, wget and GDAL look for it: the "machine urs.earthdata.nasa.gov" entry of ~/.netrc
# ($NETRC names another file). Every other entry of the file is kept as it is.

const _RS_URS = "urs.earthdata.nasa.gov"

_rs_netrc_path()::String = get(ENV, "NETRC", joinpath(homedir(), ".netrc"))

# The file as entries: (machine, [key => value, ...]); "default" is the machine name of a default entry.
function _rs_netrc_read(path::String)::Vector{Tuple{String, Vector{Pair{String,String}}}}
	out = Tuple{String, Vector{Pair{String,String}}}[]
	isfile(path) || return out
	tok = split(read(path, String))
	i = 1
	while i <= length(tok)
		t = tok[i]
		if t == "macdef"
			error("$(path) holds a macdef, which this dialog cannot rewrite safely; edit the file by hand.")
		elseif t == "machine" && i < length(tok)
			push!(out, (String(tok[i + 1]), Pair{String,String}[]));  i += 2
		elseif t == "default"
			push!(out, ("default", Pair{String,String}[]));  i += 1
		elseif !isempty(out) && i < length(tok)
			push!(out[end][2], String(t) => String(tok[i + 1]));  i += 2
		else
			i += 1
		end
	end
	return out
end

function _rs_netrc_get(path::String, machine::String)::Tuple{String,String}
	for (m, kv) in _rs_netrc_read(path)
		m == machine || continue
		d = Dict{String,String}(kv)
		return get(d, "login", ""), get(d, "password", "")
	end
	return "", ""
end

function _rs_netrc_set!(path::String, machine::String, login::String, pass::String)
	(isempty(login) || isempty(pass)) && error("Give both the login and the password.")
	any(isspace, login * pass) && error("A .netrc login or password cannot contain spaces.")
	ents = _rs_netrc_read(path)
	k = findfirst(e -> e[1] == machine, ents)
	if k === nothing
		# Before a "default" entry: it matches any machine, so one after it would never be reached.
		kd = findfirst(e -> e[1] == "default", ents)
		insert!(ents, kd === nothing ? length(ents) + 1 : kd, (machine, Pair{String,String}[]))
		k = kd === nothing ? length(ents) : kd
	end
	kv = filter(p -> !(p.first in ("login", "password")), ents[k][2])
	ents[k] = (machine, vcat(["login" => login, "password" => pass], kv))
	io = IOBuffer()
	for (m, kv) in ents
		println(io, m == "default" ? "default" : "machine $m")
		for (a, b) in kv
			println(io, "    ", a, " ", b)
		end
	end
	write(path, take!(io))
	Sys.iswindows() || chmod(path, 0o600)      # curl refuses a netrc others can read
	return nothing
end

function _rs_modisscenes(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	sat = uppercase(_rs_get(d, "p_sat", "AQUA"))
	haskey(_RS_NORAD, sat) || (sat = "AQUA")
	src = _rs_get(d, "p_tlesrc")
	byfile = startswith(src, "TLE file")
	# The two accounts of the Accounts tab, each one entry of ~/.netrc: (widget prefix, machine, name).
	accts = (("p_ed", "lbl_ed_where", _RS_URS, "Earthdata"), ("p_st", "lbl_st_where", _RS_ST, "Space-Track"))
	if what == "savelogin" || what == "savest"
		pre, _, mach, nm = accts[what == "savelogin" ? 1 : 2]
		f = _rs_netrc_path()
		_rs_netrc_set!(f, mach, _rs_get(d, pre * "_user"), _rs_get(d, pre * "_pass"))
		_rs_status(io, "$(nm) login saved in $(f) (machine $(mach)).")
		return true
	end
	if what == "download"
		url, path = _rs_get(d, "dl_url"), _rs_get(d, "dl_path")
		(isempty(url) || isempty(path)) && error("No file to download.")
		_rs_prog_show("Downloading $(basename(path))…")
		try
			_rs_download(url, path)
		finally
			_rs_prog_close()
		end
		_rs_status(io, "Downloaded $(path) ($(round(filesize(path) / 2^20, digits = 1)) MB).")
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
		# The Region tab opens on the last complete month. Its script box stays as the .ui has it: empty
		# means "show the script in a text window".
		m1 = GMT.Dates.firstdayofmonth(GMT.Dates.Date(RS.now(RS.UTC))) - GMT.Dates.Day(1)
		_rs_set(io, "p_rg_from", "$(GMT.Dates.firstdayofmonth(m1))T00:00:00")
		_rs_set(io, "p_rg_to", "$(m1)T00:00:00")
		f = _rs_netrc_path()
		for (pre, lbl, mach, _) in accts
			u, p, why = try
				(_rs_netrc_get(f, mach)..., isfile(f) ? "" : "  (the file does not exist yet)")
			catch e
				("", "", "  — cannot read it: " * sprint(showerror, e))
			end
			_rs_set(io, pre * "_user", u);  _rs_set(io, pre * "_pass", p)
			_rs_label(io, lbl, "Saved in $(f) as machine $(mach)$(why)")
		end
	end
	if what == "init" || what == "refresh"
		_rs_enable(io, "p_tlefile", byfile);  _rs_enable(io, "browse_p_tlefile", byfile)
		what == "init" && _rs_set(io, "p_start", GMT.Dates.format(floor(RS.now(RS.UTC), Minute(5)), "yyyy-mm-ddTHH:MM:SS"))
		what == "init" && _rs_status(io, "Elements are good for a few days around their epoch: for a past date use \"Space-Track history\" (login in the Accounts tab) or a TLE file; the set nearest the date is taken.")
		return true
	end
	what == "region" || what == "scenes" || what == "find" || error("unknown request: $what")
	_rs_prog_show(what == "region" ? "Asking NASA's CMR for the scenes over the region…" :
	              byfile ? "Reading the TLE file…" : startswith(src, "Space-Track") ?
	              "Reading the $(sat) element history (Space-Track, or the copy already on disk)…" :
	              "Reading the orbital elements from Celestrak…")
	try
		return _rs_modis_run(scene, d, io, what, sat)
	finally
		_rs_prog_close()
	end
end

# "scenes" and "find", under the progress dialog _rs_modisscenes put up: elements 0-10, orbit 10-30,
# the OB.DAAC names 30-100.
function _rs_modis_run(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer, what::String, sat::String)::Bool
	what == "region" && return _rs_region_run(d, io)   # its own Satellite list
	when = _rs_modis_when(d)
	tle, ep = _rs_modis_tle(d, sat, jd(when))
	_rs_prog(10, "Propagating the $(sat) orbit…")
	age = round(abs(jd(when) - ep), digits = 1)
	agemsg = "TLE epoch $(GMT.Dates.format(datetime(ep), "yyyy-mm-dd HH:MM")) UTC, $(age) days from the requested time" *
	         (age > 15 ? " — too far: positions may be off by tens of km." : ".")
	if what == "scenes"
		mins = _rs_num(d, "p_minutes", 10.0)
		mins < 5 && error("The window must be at least 5 minutes (one MODIS scene).")
		D = RS.sat_tracks(tle_obj = tle, start = when, stop = when + Minute(round(Int, mins)), tiles = true, sat = sat)
		isempty(D) && error("No complete scene in that time window.")
		# An empty window gets a map to draw on: a blank canvas over the scenes, and its coastline
		# through the Geography menu's own door. A window with content keeps its frame (vector law).
		if _rs_empty(scene)
			W, E = minimum(minimum(view(s.data, :, 1)) for s in D), maximum(maximum(view(s.data, :, 1)) for s in D)
			S, N = minimum(minimum(view(s.data, :, 2)) for s in D), maximum(maximum(view(s.data, :, 2)) for s in D)
			W, E, S, N = max(W - 5, -180.0), min(E + 5, 180.0), max(S - 5, -90.0), min(N + 5, 90.0)
			_blank_canvas(scene, W, E, S, N, true, "$(sat) MODIS scenes")
			_on_geography(scene, "coast/i/$W/$E/$S/$N")
		end
		nm = "$(sat) scenes $(GMT.Dates.format(when, "yyyy-mm-dd HH:MM"))+$(round(Int, mins))m"
		_add_dataset_to_scene(scene, D, nm; noConvertToPoints = true, forceMode = :lines)
		html, nmiss, nunread = _rs_scene_links(String[s.header for s in D], 30)
		_rs_set(io, "p_found", html)
		_rs_status(io, "$(length(D)) scene(s) drawn.$(_rs_links_note(nmiss, nunread)) $(agemsg)")
		return true
	end
	what == "find" || error("unknown request: $what")
	lon, lat = _rs_num(d, "p_lon", NaN), _rs_num(d, "p_lat", NaN)
	(isnan(lon) || isnan(lat)) && error("Give the point's longitude and latitude.")
	days = round(Int, _rs_num(d, "p_days", -2.0))
	dn = _rs_get(d, "p_daynight")
	kw = Pair{Symbol,Any}[]
	startswith(dn, "Day") && push!(kw, :day => true)
	startswith(dn, "Night") && push!(kw, :night => true)
	push!(kw, startswith(_rs_get(d, "p_product"), "Chlor") ? (:oc => true) : (:sst => true))
	_rs_prog(10, "Propagating the $(sat) orbit over $(abs(days)) day(s) and finding the passes over the point…")
	sc = RS.findscenes(lon, lat; start = when, sat = sat, duration = days, tle = tle, kw...)
	names = sc === nothing ? String[] : sc
	html, nmiss, nunread = _rs_scene_links(names, 30)
	_rs_set(io, "p_found", html)
	_rs_status(io, isempty(names) ? "No $(sat) scene covers ($(lon), $(lat)) in that period. $(agemsg)" :
	                                "$(length(names)) scene(s) cover ($(lon), $(lat)).$(_rs_links_note(nmiss, nunread)) $(agemsg)")
	return true
end

# --- the callback ------------------------------------------------------------------------------

const _RS_TOOLS = Dict{String,Function}(
	"indices" => _rs_indices, "truecolor" => _rs_truecolor, "calibration" => _rs_calibration,
	"classify" => _rs_classify, "cutcube" => _rs_cutcube, "modisl2" => _rs_modisl2,
	"modisscenes" => _rs_modisscenes)

function _on_remotes(scene::Ptr{Cvoid}, params::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	tool = ""
	try
		d = _sat_kv(unsafe_string(params))
		tool = _rs_get(d, "tool")
		f = get(_RS_TOOLS, tool, nothing)
		f === nothing && error("unknown Remote sensing tool: \"$tool\"")
		_rs_get(d, "what") in ("init", "refresh") || warm_wait("remotes")
		io = IOBuffer()
		f(scene, d, io)
		ans = String(take!(io))
		# Never a silently cut answer (a download script missing its last lines looks complete).
		sizeof(ans) < cap || error("The answer ($(sizeof(ans)) bytes) does not fit the dialog's $(cap)-byte buffer.")
		_sat_reply(out, cap, ans)
		return Cint(1)
	catch e
		try; _tool_failed(scene, "Remote sensing" * (isempty(tool) ? "" : " ($tool)"), e); catch; end
		_sat_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# JIT warm-up while a dialog is being filled in: the index engine, the tree and the orbit path.
function _remotes_warm()
	a = rand(Float32, 8, 8);  b = rand(Float32, 8, 8)
	RS.ndvi(a, b);  RS.evi(a, b, a)
	RS.predict(RS.fit_tree([0.1 0.2; 0.8 0.9], UInt8[1, 2]), [0.5 0.5])
	precompile(_on_remotes, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	return nothing
end

function _register_remotes()
	fptr = @cfunction((s, p, o, n) -> Base.invokelatest(_on_remotes, s, p, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_remotes_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("remotes", _remotes_warm)
end
