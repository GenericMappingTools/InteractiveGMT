# gravmag3d.jl — Geophysics > Magnetics > "Anomaly of a 3-D body": host glue for GMT's
# gmtgravmag3d (Okabe 1979 polyhedral bodies), driven through GMT.jl's own `gravmag3d`. No maths
# lives here — the module does all of it; this file only turns the dialog's key=value block into
# gravmag3d keywords and puts the result back in the window.
#
# The C++ dialog is GravMag3DDialog (70_window.cpp, loads deps/ui/gravmag3d_dialog.ui).

# One "shape,params" list entry -> the NamedTuple `gravmag3d`'s `body` keyword wants. Params are
# passed on verbatim (slash-separated, as the module documents them); the shape name is whatever
# the dialog's combo offered, so no validation is duplicated here.
function _gm3d_bodies(s::AbstractString)
	bods = NamedTuple{(:shape, :params),Tuple{Symbol,String}}[]
	for item in split(s, '|')
		isempty(strip(item)) && continue
		i = findfirst(',', item)
		i === nothing && error("malformed body '$item' (expected \"shape,params\")")
		push!(bods, (shape = Symbol(strip(item[1:i-1])), params = String(strip(item[i+1:end]))))
	end
	return bods
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGravMag3DFn. Absent key = don't pass that option to GMT, so the module's own
# defaults stay in charge (never re-stated here).
#
# Grid result: added to `scene` under one fixed name per anomaly kind, obeying SACRED_LAW.md's
# derived-variable display law exactly like _on_rtp3d/_on_clipgrid — through the SHARED
# `_add_grid_to_scene` (drop.jl), never a private copy of the add/promote ccall pair — plus the
# derived-variable AXES law (`gmtvtk_reframe_h` with the new grid's own limits, since the anomaly
# region is typically NOT the parent grid's).
# Track result: no grid exists at all, so it goes to the window's Data Viewer tab (`show_table`).
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_gravmag3d(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		kw = Dict{Symbol,Any}()
		arg1 = nothing

		bodykind = _get(d, "bodykind", "geom")
		if bodykind == "geom"
			bods = _gm3d_bodies(_get(d, "bodies"))
			isempty(bods) && error("no body was defined")
			# A single body goes as the bare NamedTuple; several as a Tuple of them (gravmag3d emits
			# one -M+s per element). They all share one density / one set of magnetic parameters —
			# the module has no syntax for per-body values.
			kw[:body] = length(bods) == 1 ? bods[1] : Tuple(bods)
		elseif bodykind == "file"
			file = _get(d, "file")
			isfile(file) || error("body file not found: $file")
			fk = _get(d, "filekind", "raw")
			kw[fk == "index" ? :index : fk == "stl" ? :stl : :raw_triang] = file
		elseif bodykind == "memfv"
			fig = get(_FIGREG, scene, nothing)
			(fig isa QtFV) || error("this window holds no 3-D solid (GMTfv) to compute the anomaly of")
			arg1 = fig.fv			# gravmag3d itself rejects a non-triangulated solid (e.g. a cube)
		else
			error("unknown body kind '$bodykind'")
		end
		_on(d, "onebased") && (kw[:onebased] = true)
		_on(d, "noswap")   && (kw[:noswap] = true)

		title = ""
		if _get(d, "mode", "grav") == "grav"
			dens = _get(d, "density")
			isempty(dens) && error("a gravity anomaly needs a density")
			# Either a constant (SI) or the name of a variable-density grid — the module takes both,
			# so only the number is converted and anything else travels as the path it is.
			kw[:density] = something(tryparse(Float64, dens), dens)
			title = "Gravity anomaly"
		else
			mp = _get(d, "magparams")
			isempty(mp) && error("a magnetic anomaly needs the five mag_params")
			kw[:mag_params] = mp			# f_dec/f_dip/m_int/m_dec/m_dip, as the module documents it
			title = "Magnetic anomaly"
		end

		for (key, sym) in (("region", :region), ("inc", :inc), ("track", :track))
			v = _get(d, key);  isempty(v) || (kw[sym] = v)
		end
		for (key, sym) in (("zobs", :z_obs), ("level", :level), ("thickness", :thickness), ("radius", :radius))
			v = _get(d, key);  isempty(v) || (kw[sym] = parse(Float64, v))
		end
		_on(d, "geog") && (kw[:f] = :g)		# lon/lat -> meters via the module's Flat Earth approximation

		R = arg1 === nothing ? GMT.gravmag3d(; kw...) : GMT.gravmag3d(arg1; kw...)

		# --- Track mode: a table of anomaly values at the given x,y — nothing to put on a surface.
		if haskey(kw, :track)
			D = isa(R, Vector) ? R : [R]
			isempty(D) && error("gravmag3d returned no data for the track locations")
			outfile = _get(d, "outfile")
			isempty(outfile) || GMT.gmtwrite(outfile, R)
			show_table(scene, D; name = title)
			return Cint(1)
		end

		isa(R, GMTgrid) || error("gravmag3d returned a $(typeof(R)), not a grid")
		G = R
		outfile = _get(d, "outfile")
		isempty(outfile) || GMT.gmtwrite(outfile, G)

		# REPLACE, never pile up: recomputing the same anomaly kind trashes the previous result (the
		# C++ actor AND the stale Julia reference) before adding the new one — same as _on_rtp3d.
		ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, title)
		_forget_object!(scene, :grid, title)

		_grid_command!(G, "InteractiveGMT gravmag3d " * join(("$k=$v" for (k, v) in kw), ' '))
		has_surface = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
		if !_add_grid_to_scene(scene, G, title; promote = (has_surface == 0))
			_viewer_log_error(scene, "gravmag3d: window closed, grid not added")
			return Cint(0)
		end
		# SACRED_LAW.md derived-variable display law: the anomaly starts CHECKED, every other grid in
		# the window is UNCHECKED, and Scene Objects unfolds to reveal it.
		_show_object!(scene, title)
		_hide_other_objects!(scene, :grid, title)
		ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
		# ...and the derived-variable AXES law: the anomaly's region is the one the user typed, not
		# the parent grid's, so the axes cube + camera must re-fit to it (keepMargin=0: grids fill
		# edge-to-edge, the existing grid convention).
		r = G.range
		ccall(_fn(:gmtvtk_reframe_h), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cint),
		      scene, r[1], r[2], r[3], r[4], Cint(0))
		return Cint(1)
	catch e
		_viewer_log_error(scene, "gravmag3d FAILED: $(sprint(showerror, e))")
		@warn "gravmag3d FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_gravmag3d()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_gravmag3d, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gravmag3d_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
