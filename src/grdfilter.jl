# grdfilter.jl — GMT menu > "grdfilter": filter the window's grid in the space domain, through
# GMT.jl's own `grdfilter`.
#
# The C++ dialog is GrdFilterDialog (70_window.cpp, loads deps/ui/grdfilter_dialog.ui), whose layout
# is Mirone's Grdfilter window (the shared Griding Line Geometry block, Filter type + width, Distance
# flag) plus everything the module offers today: the custom/operator weight-grid filters, the
# histogram mode with its bin width, +h high-pass, +q quantile, +c/+l/+u, a rectangular second width,
# the -N NaN policy and the -T registration toggle.
#
# The whole -F string is assembled by the DIALOG (it is the side that knows which knob belongs to
# which filter) and travels verbatim; this file does not second-guess it.

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdFilterFn. Returns Cint 1 on success, 0 on failure.
function _on_grdfilter(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		G = _find_object(scene, :grid, _get(d, "grid"))     # the grid ON DISPLAY, not the base one
		G === nothing && error("no grid in this window")

		F = _get(d, "filter")
		isempty(F) && error("no filter given")
		kw = Dict{Symbol,Any}(:filter => F)
		# -D is REQUIRED by the module; the dialog always sends it (defaulted from the grid's own
		# geographic flag), so an empty one here is a bug worth reporting rather than silently guessing.
		dist = _get(d, "distance")
		isempty(dist) && error("no distance flag given")
		kw[:distance] = dist
		# Distance flags 1-5 mean "the grid is in DEGREES". A grid handed over IN MEMORY does not carry
		# that to the module — even one built with a lon/lat proj4 (its geog flag is 1) is seen as
		# Cartesian, and grdfilter refuses outright: "Option -D: Input grid is Cartesian but your
		# distance mode is set for geographic distances". -fg is what actually tells it. Verified: D=4
		# fails alone and succeeds with f=:g. Flags p and 0 ARE Cartesian, so they must not get it.
		(dist in ("1", "2", "3", "4", "5")) && (kw[:f] = :g)

		# -R / -I: the output can be a sub-region and/or a coarser grid than the input. Both come from
		# the geometry block, which starts filled with the input's own values — so passing them is
		# harmless when untouched, and is the whole point when narrowed.
		reg = _get(d, "region")
		(isempty(reg) || occursin("//", reg)) || (kw[:region] = reg)
		inc = _get(d, "inc");  isempty(inc) || (kw[:inc] = inc)
		nan = _get(d, "nans");  isempty(nan) || (kw[:nans] = nan)
		_on(d, "toggle") && (kw[:toggle] = true)

		R = GMT.grdfilter(G; kw...)
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")

		# A descriptive name: the -F string IS the recipe ("g100+h", "m600+q0.25", …).
		return _gm3d_deliver(scene, R, "Filtered ($F)", _get(d, "outfile"), false,
		                     "grdfilter " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdfilter", e)
		return Cint(0)
	end
end

function _register_grdfilter()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdfilter, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdfilter_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
