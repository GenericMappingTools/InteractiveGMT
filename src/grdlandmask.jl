# grdlandmask.jl — GMT menu > "grdlandmask": a wet/dry mask grid from the shoreline database, through
# GMT.jl's own `grdlandmask`.
#
# The C++ dialog is GrdLandmaskDialog (70_window.cpp, loads deps/ui/grdlandmask_dialog.ui), whose
# layout is Mirone's grdlandmask window (the shared Griding Line Geometry block, coastline resolution,
# Min area, registration, the five node values, Boundary) plus what the module grew since: the "auto"
# resolution, the -E border VALUES, and the second method — hand grdlandmask a GRID and it masks THAT
# grid instead of returning a bare mask.

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdLandmaskFn. Returns Cint 1 on success, 0 on failure.
function _on_grdlandmask(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d  = _nswing_parse(unsafe_string(cparams))
		kw = Dict{Symbol,Any}()

		# Second method: mask the grid this window is DISPLAYING. Its own region and spacing then define
		# the mask (GMT.jl reads them off the grid), so the geometry boxes are not sent at all.
		clip = _on(d, "clip")
		local G = nothing
		if clip
			G = _find_object(scene, :grid, _get(d, "grid"))
			G === nothing && error("no grid in this window to mask")
		else
			reg = _get(d, "region");  inc = _get(d, "inc")
			(isempty(reg) || occursin("//", reg)) && error("give a region (Min/Max in both directions)")
			isempty(inc) && error("give a grid spacing")
			kw[:region] = reg
			kw[:inc]    = inc
			_on(d, "pixel") && (kw[:registration] = :pixel)
		end

		res = _get(d, "res");  isempty(res) || (kw[:res] = Symbol(res))
		ar  = _get(d, "area"); isempty(ar)  || (kw[:area] = ar)
		# -N: "wet/dry" or "ocean/land/lake/island/pond". NaN is a legal value here (that is how a mask
		# punches holes), so the strings travel as typed.
		mv = _get(d, "maskvalues")
		isempty(mv) || (kw[:maskvalues] = mv)
		# -E: bare (nodes on a boundary count as outside) or with line-tracing values.
		if haskey(d, "border")
			bv = _get(d, "border")
			kw[:border] = isempty(bv) ? true : bv
		end
		_on(d, "verbose") && (kw[:verbose] = true)

		R = clip ? GMT.grdlandmask(G; kw...) : GMT.grdlandmask(; kw...)
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")

		outfile = _get(d, "outfile")
		title = clip ? "Masked grid" : "Land mask"
		return _gm3d_deliver(scene, R, title, outfile, false,
		                     "grdlandmask " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdlandmask", e)
		return Cint(0)
	end
end

function _register_grdlandmask()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdlandmask, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdlandmask_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
