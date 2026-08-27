# earthregions.jl — Tools > "Earth regions": pick a named geographic region out of GMT.jl's own
# collections and either bring its topography/imagery into the window or just learn where it is.
#
# The C++ dialog is EarthRegionsDialog (70_window.cpp, loads deps/ui/earthregions_dialog.ui).
#
# EVERYTHING about what a region IS comes from `GMT.earthregions` (GMT.jl, src/pscoast.jl): the
# collection tables, which codes exist, the rounded-versus-exact boundaries, the dataset/resolution
# rules. None of that is restated here — this file only chooses which of the function's three
# outcomes to ask for and puts the answer where this window can show it:
#
#   list    `earthregions(<collection>)` PRINTS its table. There is no dataset to return (the
#           function's own answer is the printout), so the printout is captured and written to the
#           window's message pane — the same text a REPL user reads, in the one place a window has
#           for text.
#   raster  `earthregions(code, dataset=…, res=…, registration=…)` returns a GMTgrid or a GMTimage.
#           That is a new raster, so it goes in through the SAME doors a dropped file uses:
#           `_gm3d_deliver` for a grid, `_place_image_in_window` for an image — both of which honour
#           the raster-own-axes law. Never a private display path.
#   limits  a code -> its four boundaries, so the dialog can show them in its Region boxes. The
#           function's other outcome is a `coast` call, and what this window wants out of it is the
#           REGION it resolved, so the run is made with `Vd=2` (the function's own dry run, which
#           hands back the coast command it would have issued) and the -R is read out of that.
#           Deliberate: it keeps the collection lookup, the rounding and the exact/rounded choice in
#           GMT.jl, where they live.
#
# THE REGION IS THE REQUEST. The collections, the codes and the listing exist only to fill the four
# Region boxes; whatever stands in them is what the data are fetched over. The country tick is the
# one thing that is neither raster nor number, and it is NOT a plotting problem: `coast` with -M
# (`dump=true`) hands the same shoreline/border polygons back as VECTORS — only its default branch
# draws PostScript — so they go on as an ordinary line overlay over what was fetched.

# The collections `earthregions` knows, in the order its own no-argument call prints them.
const _ER_COLLECTIONS = ("DCW", "NatEarth", "UN", "Mainlands", "IHO", "Wiki", "Lakes")

# The remote datasets and resolutions, from the function's own two lists. Kept here only so the
# dialog can OFFER them; every rule about which pairs are legal stays in GMT.jl, which refuses the
# impossible ones with its own message.
const _ER_DATASETS = ("earth_relief", "earth_synbath", "earth_gebco", "earth_mask", "earth_day",
                      "earth_night", "earth_geoid", "earth_faa", "earth_dist", "earth_mss",
                      "earth_vgg", "earth_wdmam", "earth_age", "mars_relief", "moon_relief",
                      "mercury_relief", "venus_relief", "pluto_relief")
const _ER_RESOLUTIONS = ("01d", "30m", "20m", "15m", "10m", "06m", "05m", "04m", "03m", "02m",
                         "01m", "30s", "15s", "03s", "01s")

# `round` travels as text: a bare number, a slash-separated list, or one of GMT's own +r/+R/+e
# strings. The function itself validates the string form; what is checked here is only that a
# non-string is really a number, so a typo does not reach GMT as a silent 0.
function _er_round(s::AbstractString)
	t = strip(String(s))
	isempty(t) && return 0
	startswith(t, "+") && return t                    # +r / +R / +e — GMT's own syntax, passed whole
	for p in split(t, '/')
		(tryparse(Float64, p) === nothing) &&
			error("the rounding step is a number, inc/inc, or four incs — or a +r/+R/+e string, not '$s'")
	end
	return t
end

# Where GMT.jl keeps the collection tables. This mirrors `_earthregions`'s own `pato`, and it is the
# ONE thing about the collections known on this side — because the function itself only ever PRINTS
# them, through GMT.jl's pretty-dataset `show`, and that printer throws on the two biggest:
#     The number of columns in the header (7) must be equal to that of the table (5).
# for DCW (255 regions) and IHO (104), while the five smaller collections print fine. Those two are
# exactly the ones a user most needs listed, so the rows are read and laid out here instead. That is
# FORMATTING, which is this window's job; nothing about which regions exist is restated.
_er_dir() = joinpath(dirname(pathof(GMT))[1:end-4], "share", "named_regions", "")

# One collection as a plain aligned listing: code, name, then the four boundaries. The file's text
# column is "CODE,Name,Reference"; the reference is the collection itself and adds nothing here.
function _er_listing(coll::AbstractString)::String
	f = _er_dir() * String(coll) * "_collection.txt"
	isfile(f) || error("the '$coll' collection table is not there: $f")
	D = GMT.gmtread(f)
	isa(D, GMTdataset) || error("the '$coll' collection came back as a $(typeof(D))")
	m = D.data
	(size(m, 1) == 0 || size(m, 2) < 4) && error("the '$coll' collection has no regions in it")
	io = IOBuffer()
	println(io, rpad("Code", 10), rpad("Region", 40),
	            lpad("West", 11), lpad("East", 11), lpad("South", 11), lpad("North", 11))
	for k in 1:size(m, 1)
		t = k <= length(D.text) ? String(D.text[k]) : ""
		p = split(t, ',')
		code = isempty(p) ? "" : String(p[1])
		name = length(p) >= 3 ? join(p[2:end-1], ',') : (length(p) == 2 ? String(p[2]) : "")
		println(io, rpad(code, 10), rpad(name, 40),
		            lpad(string(round(m[k, 1]; digits = 4)), 11),
		            lpad(string(round(m[k, 2]; digits = 4)), 11),
		            lpad(string(round(m[k, 3]; digits = 4)), 11),
		            lpad(string(round(m[k, 4]; digits = 4)), 11))
	end
	return String(take!(io))
end

# The Region block's four boxes -> "W/E/S/N". Empty is empty (the code is used instead); anything
# else must be four numbers that make a box, because a region typed by hand is the one input nobody
# else checks — the collections' own limits are already known good.
function _er_region(s::AbstractString)::String
	t = strip(String(s))
	isempty(t) && return ""
	p = split(t, '/')
	length(p) == 4 || error("the region takes four numbers, West/East/South/North, not '$s'")
	v = tryparse.(Float64, p)
	any(x -> x === nothing, v) && error("the region takes four NUMBERS, not '$s'")
	(v[1] < v[2]) || error("West must be smaller than East")
	(v[3] < v[4]) || error("South must be smaller than North")
	(-90 <= v[3] && v[4] <= 90) || error("South and North are latitudes, between -90 and 90")
	return t
end

# The W/E/S/N `earthregions` resolved for a code, taken from its OWN dry run: with Vd=2 the `coast`
# call at the end of the map branch returns the command string instead of drawing, and that string
# carries the -R the whole collection lookup just produced.
function _er_limits(code::AbstractString, country::Bool, rnd, exact::Bool)::String
	cmd = GMT.earthregions(String(code); country = country, round = rnd, exact = exact,
	                       show = false, Vd = 2)
	isa(cmd, AbstractString) || error("could not resolve the region '$code'")
	m = match(r"-R(\S+)", String(cmd))
	m === nothing && error("no region came back for '$code'")
	return String(m.captures[1])
end

# The country / region outline, as VECTORS — the one thing about a region that is neither a raster
# nor a number. `coast`'s -M dump gives the polygons the map version would have drawn in PostScript,
# and they go on as an ordinary line overlay, on top of whatever the window already shows.
# Only the DCW collection has border polygons, and only a CODE names one: four coordinates are a box,
# and a box has no country in it.
function _er_draw_border(scene::Ptr{Cvoid}, code::AbstractString, name::AbstractString)
	isempty(code) &&
		error("the border lines need a country code (a DCW code such as PT, or PT,ES), not just a region")
	# The pen goes to GMT in the SAME points the line is drawn with here (_GEO_LINE_PT, geography.jl),
	# so the outline is the thickness a coastline is — one number, not two that drift apart.
	D = GMT.coast(DCW = String(code) * "+p" * string(_GEO_LINE_PT), dump = true)
	(D === nothing || (isa(D, AbstractVector) && isempty(D))) &&
		error("no border came back for '$code' — only the DCW collection has country polygons")
	# A border is a LINE: no "Convert to points" on its handle.
	_add_geo_overlay(scene, D; color = (0.0, 0.0, 0.0), name = String(name) * " (border)",
	                 noConvertToPoints = true) ||
		error("could not draw the border of '$code' in this window")
	return nothing
end

# C callback (Get button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaEarthRegionsFn. Returns Cint 1 on success, 0 on failure.
function _on_earthregions(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode", "raster")

		# ---- List a collection, laid out for the message pane (see `_er_listing`).
		if mode == "list"
			coll = _get(d, "collection")
			(coll in _ER_COLLECTIONS) || error("unknown collection '$coll'")
			txt = _er_listing(coll)
			isempty(strip(txt)) && error("the '$coll' collection came back empty")
			# Back to the DIALOG that asked, not to the Errors console: this is output to read and
			# CHOOSE from — the popup it opens fills the code box on a double-click — and the console
			# is where failures land, which would bury a 255-row listing and be buried by it.
			ccall(_fn(:gmtvtk_earthregions_set_listing), Cvoid, (Ptr{Cvoid}, Cstring, Cstring),
			      dlg, "Earth regions — the $coll collection", txt)
			return Cint(1)
		end

		region = _er_region(_get(d, "region"))
		# ---- Just resolve a code to its boundaries, so the dialog can SHOW them in its Region boxes.
		# Nothing is fetched and nothing is added to the window: this is the lookup a user would
		# otherwise do by reading the listing, done for them the moment they finish typing a code.
		if mode == "limits"
			c = strip(_get(d, "code"))
			isempty(c) && error("no code to look up")
			lim = _er_limits(c, _on(d, "country"), _er_round(_get(d, "round")), _on(d, "exact"))
			p = split(lim, '/')
			v = length(p) == 4 ? tryparse.(Float64, p) : nothing
			# A composite code, or a +r rounding form, is not four plain numbers — then there is
			# nothing to put in four boxes, and saying so beats filling them with something else.
			(v === nothing || any(x -> x === nothing, v)) &&
				error("'$c' resolves to '$lim', which is not a plain West/East/South/North box")
			ccall(_fn(:gmtvtk_earthregions_set_region), Cvoid,
			      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), dlg, v[1], v[2], v[3], v[4])
			return Cint(1)
		end

		# THE REGION IS THE REQUEST. The collections, the codes and the listing exist only to fill the
		# four Region boxes; whatever stands in them is what the data are fetched over. A code with
		# the boxes still empty (nothing has looked it up yet) is resolved to its four numbers right
		# here — `exact` and `round` belong to THAT resolution, because turning a code into numbers
		# is the only thing they do.
		rnd = _er_round(_get(d, "round"))
		exact = _on(d, "exact")
		country = _on(d, "country")
		code = strip(_get(d, "code"))
		if isempty(region)
			isempty(code) &&
				error("give the four Region boxes, or a code to fill them — press \"List its regions\" to see the codes")
			region = _er_limits(code, country, rnd, exact)
		end
		# The layer's name: the code when there is one, since "-10/-6/36/39" names nothing.
		name = strip(_get(d, "name"))
		isempty(name) && (name = isempty(code) ? "Region" : String(code))

		(mode == "raster") || error("unknown mode '$mode'")

		# ---- The region's grid or image. Every rule about which dataset/resolution pairs exist is
		# GMT.jl's; an impossible one comes back as its own error, which lands in the same console.
		dataset = _get(d, "dataset", "earth_relief")
		(dataset in _ER_DATASETS) || error("unknown dataset '$dataset'")
		res = _get(d, "res")
		reg = _get(d, "registration")
		(!isempty(reg) && isempty(res)) &&
			error("a registration can only be asked for together with a resolution")
		# The layer's identity is what was ASKED FOR — the region's name, the dataset and the
		# resolution — so the same request twice is recognisable BEFORE anything is downloaded.
		title = name * " (" * dataset * (isempty(res) ? "" : " " * res) * ")"
		if _find_object_exact(scene, :grid, title) !== nothing ||
		   _find_object_exact(scene, :image, title) !== nothing
			# Already here. Downloading it again would cost the transfer and then either pile up a
			# duplicate layer or silently replace an identical one.
			_viewer_log_error(scene, "Earth regions — \"$title\" is already in this window; " *
			                  "nothing was downloaded.")
			country && _er_draw_border(scene, code, name)   # the border was still asked for
			return Cint(1)
		end

		# `region` is already the four numbers, so it goes to the module as the region it is: `exact`
		# stops it being looked up in a collection, and `round` is NOT passed again — it was applied
		# when the code became these numbers, and would otherwise be applied a second time.
		R = GMT.earthregions(String(region); dataset = dataset, res = res, registration = reg,
		                     exact = true, show = false)
		R === nothing && error("no data came back for $region")

		if isa(R, GMTimage)
			# An image is not a grid: it goes in by the image door, which is the same one a dropped
			# .tif uses (and which reframes the axes to it, per the raster-own-axes law).
			_place_image_in_window(scene, R, title; geographic = true)
			country && _er_draw_border(scene, code, name)
			return Cint(1)
		end
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid or an image")
		ok = _gm3d_deliver(scene, R, title, "", false,
		                   "earthregions $code dataset=$dataset res=$res"; geographic = true)
		# The border goes on AFTER the raster: a vector overlay drawn first would be the only thing
		# left if the download failed, and it belongs on top of what was fetched anyway.
		(ok == Cint(1) && country) && _er_draw_border(scene, code, name)
		return ok
	catch e
		_viewer_log_error(scene, "Earth regions FAILED: $(sprint(showerror, e))")
		@warn "Earth regions FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_earthregions()
	fptr = @cfunction((s, w, c) -> Base.invokelatest(_on_earthregions, s, w, c)::Cint,
	                  Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_earthregions_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
