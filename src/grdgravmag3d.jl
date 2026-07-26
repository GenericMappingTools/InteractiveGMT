# grdgravmag3d.jl — Geophysics > Magnetics > "grdgravmag3d": the gravity/magnetic anomaly of a body
# described by ONE grid (a constant-thickness layer under it, or the volume closed at its bottom/top)
# or by TWO grids (the volume between them). Same Okabe (1979) engine as gravmag3d.jl, different body
# description — so only the input side differs here; the result is delivered by the SHARED
# `_gm3d_deliver` (gravmag3d.jl), never a second copy of that tail.
#
# The C++ dialog is GrdGravMag3DDialog (70_window.cpp, loads deps/ui/grdgravmag3d_dialog.ui).

# "selected" = the grid already loaded in the window (the dialog's default top surface), same sentinel
# and same _FIGREG resolution as grdsample.jl/rtp3d.jl use.
function _grdgm3d_grid(scene::Ptr{Cvoid}, spec::AbstractString)::GMTgrid
	if spec == "selected"
		fig = get(_FIGREG, scene, nothing)
		(fig isa QtFigure) || error("no grid loaded in this window")
		return fig.G
	end
	isfile(spec) || error("grid file not found: $spec")
	return GMT.gmtread(String(spec))
end

# Assemble the -H value(s). GMT accepts SEVERAL -H at once and GMT.jl appends " -H" + the string it is
# given, so the module's own documented idiom ("z -H+n -H+mmag.grd") is what a multi-part request has
# to become: the parts are joined by " -H". Order follows the docs — angles/component first, then the
# +i|+n ambient-field modifier, then the +m intensity grid.
function _grdgm3d_magparams(d::Dict{String,String})::String
	parts = String[]
	for key in ("component", "magparams", "igrf")
		v = _get(d, key);  isempty(v) || push!(parts, v)
	end
	mgrd = _get(d, "maggrid");  isempty(mgrd) || push!(parts, "+m" * mgrd)
	isempty(parts) && error("a magnetic anomaly needs the five mag_params, a component, an intensity grid or IGRF")
	return join(parts, " -H")
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdGravMag3DFn. Absent key = don't pass that option, so the module's own defaults
# (thickness 500 m, radius 30 km, the input grid's region/increment) stay in charge.
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_grdgravmag3d(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		kw = Dict{Symbol,Any}()

		Gtop = _grdgm3d_grid(scene, _get(d, "top", "selected"))
		bot  = _get(d, "bottom")
		Gbot = isempty(bot) ? nothing : _grdgm3d_grid(scene, bot)

		title = ""
		if _get(d, "mode", "grav") == "grav"
			dens = _get(d, "density")
			isempty(dens) && error("a gravity anomaly needs a density")
			# Either a constant (SI) or a variable-density grid: the module takes both, so only the
			# number is converted and anything else travels as the path it is.
			kw[:density] = something(tryparse(Float64, dens), dens)
			title = "Gravity anomaly"
		else
			kw[:mag_params] = _grdgm3d_magparams(d)
			title = "Magnetic anomaly"
		end

		zl = _get(d, "zlevel")
		isempty(zl) || (kw[:level] = Symbol(zl))            # :bottom -> -Zb, :top -> -Zt
		for (key, sym) in (("region", :region), ("inc", :inc), ("pad", :pad), ("track", :track))
			v = _get(d, key);  isempty(v) || (kw[sym] = v)
		end
		for (key, sym) in (("thickness", :thickness), ("zobs", :z_obs), ("radius", :radius))
			v = _get(d, key);  isempty(v) || (kw[sym] = parse(Float64, v))
		end
		v = _get(d, "threads");  isempty(v) || (kw[:x] = parse(Int, v))
		_on(d, "geog") && (kw[:f] = :g)		# lon/lat -> meters via the module's Flat Earth approximation

		R = Gbot === nothing ? GMT.grdgravmag3d(Gtop; kw...) : GMT.grdgravmag3d(Gtop, Gbot; kw...)
		return _gm3d_deliver(scene, R, title, _get(d, "outfile"), haskey(kw, :track),
		                     "grdgravmag3d " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_viewer_log_error(scene, "grdgravmag3d FAILED: $(sprint(showerror, e))")
		@warn "grdgravmag3d FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_grdgravmag3d()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdgravmag3d, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdgravmag3d_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
