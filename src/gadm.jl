# gadm.jl — Geography > "Administrative units (GADM)…": a country's administrative boundaries from
# the gadm.org database, through GMT.jl's `gadm`.
#
# The C++ dialog is GadmDialog (70_window.cpp, loads deps/ui/gadm_dialog.ui).
#
# EVERYTHING about the database is `GMT.gadm`: which countries exist, the download into ~/.gmt/cache
# (the first use of a country fetches its file and says so), the administrative hierarchy and the
# names of each level. None of that is restated here — this file only turns the dialog's block into
# one call and puts the answer where the window can show it:
#
#   names   `gadm(country, subs...; names=true)` returns the names of the children of what was asked
#           for. There is nothing to draw, so the list goes back to the DIALOG (it is what its
#           Subregions box is filled from), not to the window at large.
#   plot    `gadm(country, subs...; children=…)` returns the boundary polygons. That is VECTOR data
#           landing on whatever the window already shows, so it goes on as an ordinary line overlay:
#           no axes of its own, no re-framing (SACRED_LAW.md, vector-import-onto-existing-display).

# The country list the dialog offers: "Name (ISO3)", sorted by name. Built from GMT.jl's OWN two
# tables and nothing else — `iso3to2_world()` (250 ISO 3166 alpha-3 -> alpha-2 pairs, choropleth_utils.jl)
# for the codes GADM speaks, and the DCW collection file for the names that go with the alpha-2 ones.
# A code with no name in DCW is offered by its code alone, never by a name invented here.
const _GADM_COUNTRIES = Ref{Vector{String}}(String[])

function _gadm_countries()::Vector{String}
	isempty(_GADM_COUNTRIES[]) || return _GADM_COUNTRIES[]
	name2 = Dict{String,String}()                     # alpha-2 -> country name
	f = _er_dir() * "DCW_collection.txt"              # the same table Earth regions lists
	if isfile(f)
		D = GMT.gmtread(f)
		D isa Vector && (D = D[1])
		for t in D.text
			p = split(String(t), ',')
			length(p) >= 2 && (name2[strip(p[1])] = strip(p[2]))
		end
	end
	rows = String[]
	for (a3, a2) in GMT.iso3to2_world()
		nm = get(name2, a2, "")
		push!(rows, isempty(nm) ? String(a3) : "$nm ($a3)")
	end
	sort!(rows; by = lowercase)
	_GADM_COUNTRIES[] = rows
	return rows
end

# The frame a blank window gets for these boundaries: their own bounding box grown by 10% on each
# side, clamped to what a geographic map can hold — longitudes inside [-180, 360] (the two conventions
# GMT accepts, so a box that legitimately runs past 180 is not cut back), latitudes inside [-90, 90].
# A degenerate box (a single point) is given a small span rather than a zero-width one.
function _gadm_frame(D)::NTuple{4,Float64}
	segs = D isa AbstractVector ? D : [D]
	W, E, S, N = Inf, -Inf, Inf, -Inf
	for s in segs
		m = s.data
		size(m, 1) == 0 && continue
		W = min(W, minimum(view(m, :, 1)));  E = max(E, maximum(view(m, :, 1)))
		S = min(S, minimum(view(m, :, 2)));  N = max(N, maximum(view(m, :, 2)))
	end
	isfinite(W) && isfinite(S) || error("the boundaries carry no coordinates")
	dx, dy = E - W, N - S
	dx <= 0 && (dx = 0.1);  dy <= 0 && (dy = 0.1)
	W -= 0.1dx;  E += 0.1dx;  S -= 0.1dy;  N += 0.1dy
	W = max(W, -180.0);  E = min(E, 360.0)
	S = max(S,  -90.0);  N = min(N,  90.0)
	return (W, E, S, N)
end

# The white canvas these boundaries stand on, and how big it is right now — one entry per window we
# made one for. Only a canvas OF OURS is ever resized: a window showing somebody's grid or image
# keeps its frame, because a vector import does not reframe what was already there.
const _GADM_CANVAS = Dict{Ptr{Cvoid},NTuple{4,Float64}}()

# Make the window able to HOLD `D`: a blank white canvas framed to it when the window is empty, and
# a canvas GROWN to cover it when one of ours is already there and the new boundaries fall outside.
# Growing is the same white 2x2 plane re-laid over the union of the two extents, through
# `gmtvtk_replace_base_grid_h`, which keeps every overlay already on it — the boundaries plotted
# before do not have to be re-fetched to fit beside the new ones.
function _gadm_canvas!(scene::Ptr{Cvoid}, D, name::AbstractString)::Nothing
	W, E, S, N = _gadm_frame(D)
	if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		h = _blank_canvas(scene, W, E, S, N, true, name)
		_GADM_CANVAS[h] = (W, E, S, N)
		return nothing
	end
	cur = get(_GADM_CANVAS, scene, nothing)
	cur === nothing && return nothing               # not our canvas: never touch its frame
	(W >= cur[1] && E <= cur[2] && S >= cur[3] && N <= cur[4]) && return nothing   # already covered
	u = (min(W, cur[1]), max(E, cur[2]), min(S, cur[3]), max(N, cur[4]))
	zblank = zeros(Float32, 2, 2)
	cz   = Float64[0.0, 1.0]
	crgb = Float64[1.0, 1.0, 1.0, 1.0, 1.0, 1.0]    # white, white — the same paper as _blank_canvas
	ok = ccall(_fn(:gmtvtk_replace_base_grid_h), Cint,
	           (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
	            Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring, Cint),
	           scene, zblank, Cint(2), Cint(2), u[1], u[2], u[3], u[4], Cint(1),
	           cz, crgb, Cint(2), "", Cint(0))
	ok == 0 && return nothing
	# ...and the axes/camera with it, so what was just made room for is actually on screen.
	ccall(_fn(:gmtvtk_reframe_h), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cint),
	      scene, u[1], u[2], u[3], u[4], Cint(0))
	_GADM_CANVAS[scene] = u
	return nothing
end

# The SECOND level down: the children of each child (a country's districts rather than its
# provinces). `gadm` hands back one level at a time, so this is that same call once per child —
# never a private reader of the .gpkg, and never a guess about how many levels a country has: a
# child with no children of its own contributes its own outline instead, so nothing is lost.
function _gadm_grandchildren(country::AbstractString, subs::Vector{String})
	kids = GMT.gadm(country, subs...; names = true)
	(kids === nothing || isempty(kids)) &&
		error("$(_gadm_name(country, subs, false)) has no administrative children")
	out = GMTdataset[]
	for k in String.(kids)
		D = try
			GMT.gadm(country, subs..., k; children = true)
		catch
			try GMT.gadm(country, subs..., k) catch; nothing end   # a leaf level: its own outline
		end
		D === nothing && continue
		isa(D, GMTdataset) ? push!(out, D) : append!(out, D)
	end
	isempty(out) && error("nothing came back for the level below $(_gadm_name(country, subs, false))")
	return out
end


# EVERY LEVEL BELOW THE CHOSEN UNIT: `gadm`'s own `alllevels`, which reads the country file ONCE
# instead of once per unit (asking level by level with `children` re-opens and re-scans that file
# for every province, district and parish — hundreds of full reads for a country like Portugal).
# The walk belongs there, with the open dataset, not here.
_gadm_levels_below(country::AbstractString, subs::Vector{String}) =
	GMT.gadm(country, subs...; alllevels = true)

# What each polygon IS, one name per segment: `gadm` hangs the GADM record on every dataset it
# returns (`.attrib`, filled by GMT.jl's helper_get_attrib), so the name is READ from there — the
# deepest administrative level the record actually names, which is the unit that polygon draws.
# These become the overlay's per-segment info: the hover text, and what "Show names" plots.
const _GADM_NAME_KEYS = ("NAME_5", "NAME_4", "NAME_3", "NAME_2", "NAME_1", "NAME_0", "COUNTRY")

function _gadm_seg_names(D)::Vector{String}
	segs = D isa AbstractVector ? D : [D]
	out = String[]
	for s in segs
		a = try s.attrib catch; nothing end
		nm = ""
		if a !== nothing
			for k in _GADM_NAME_KEYS
				v = get(a, k, "")
				if !isempty(strip(String(v)))
					nm = strip(String(v));  break
				end
			end
		end
		push!(out, nm)
	end
	return all(isempty, out) ? String[] : out
end

# The subregions box: "Cabo Delgado, Pemba" -> the positional arguments `gadm` takes, in order. Empty
# entries are dropped, so a trailing comma while typing is not an error.
_gadm_subs(s::AbstractString)::Vector{String} =
	String[strip(p) for p in split(String(s), ',') if !isempty(strip(p))]

# What this overlay is called when the user did not name it: the code, then the subregions, which is
# exactly how the request reads.
function _gadm_name(country::AbstractString, subs::Vector{String}, children::Bool)::String
	nm = isempty(subs) ? String(country) : String(country) * " / " * join(subs, " / ")
	return children ? nm * " (children)" : nm
end

# The name for a run that went one level deeper than `children`.
function _gadm_name(country::AbstractString, subs::Vector{String}, children::Bool, grand::Bool)::String
	grand || return _gadm_name(country, subs, children)
	nm = isempty(subs) ? String(country) : String(country) * " / " * join(subs, " / ")
	return nm * " (2nd level)"
end

# C callback (List subregions / Plot): `cparams` is the newline-separated "key=value" block described
# in 30_app.cpp's JuliaGadmFn. Returns Cint 1 on success, 0 on failure.
function _on_gadm(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode", "plot")
		# ---- The dialog is filling its country combo (asked for as it opens).
		if mode == "countries"
			ccall(_fn(:gmtvtk_gadm_set_countries), Cvoid, (Ptr{Cvoid}, Cstring),
			      dlg, join(_gadm_countries(), '\n'))
			return Cint(1)
		end
		country = uppercase(strip(_get(d, "country")))
		isempty(country) && error("no country code given")
		length(country) == 3 ||
			error("'$country' is not an ISO 3166 alpha-3 code — three letters, e.g. PRT, MOZ, IND")
		subs = _gadm_subs(_get(d, "subs"))
		children = _on(d, "children")

		# ---- The names of the children of what was asked for. The listing is the step that TELLS you
		# what can be typed in the Subregions box, so it goes straight back to the dialog.
		if mode == "names"
			nms = GMT.gadm(country, subs...; names = true)
			(nms === nothing || isempty(nms)) &&
				error("no administrative children under " * _gadm_name(country, subs, false))
			txt = join(String.(nms), "\n")
			ccall(_fn(:gmtvtk_gadm_set_listing), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), dlg,
			      "GADM — inside " * _gadm_name(country, subs, false), txt)
			return Cint(1)
		end
		(mode == "plot") || error("unknown mode '$mode'")

		alllev = _on(d, "all")                          # every level: it takes the two below it over
		grand  = !alllev && _on(d, "grand") && children
		D = alllev ? _gadm_levels_below(country, subs) :
		    grand  ? _gadm_grandchildren(country, subs) :
		             GMT.gadm(country, subs...; children = children)
		(D === nothing || (isa(D, AbstractVector) && isempty(D))) &&
			error("no boundaries came back for " * _gadm_name(country, subs, children, grand))
		name = strip(_get(d, "name"))
		isempty(name) && (name = alllev ? _gadm_name(country, subs, false) * " (levels below)" :
		                                  _gadm_name(country, subs, children, grand))
		# An EMPTY window has nothing for a vector to land on, and this tool deliberately does not pull
		# a world basemap in. So it gets a blank WHITE canvas framed to these boundaries — the SAME
		# builder File > Background region uses (`_blank_canvas`), never a second promote call here.
		_gadm_canvas!(scene, D, String(name))
		# A boundary is a LINE: no "Convert to points" on its handle. Same black the other geographic
		# line overlays are drawn in (_add_geo_overlay's own default).
		_add_geo_overlay(scene, D; name = String(name), noConvertToPoints = true,
		                 info = _gadm_seg_names(D)) ||
			error("could not draw $name in this window")
		return Cint(1)
	catch e
		_tool_failed(scene, "GADM", e)
		return Cint(0)
	end
end

function _register_gadm()
	fptr = @cfunction((s, w, c) -> Base.invokelatest(_on_gadm, s, w, c)::Cint,
	                  Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gadm_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
