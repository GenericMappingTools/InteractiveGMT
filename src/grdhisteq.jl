# grdhisteq.jl — GMT menu > "grdhisteq": histogram equalization of the window's grid, through
# GMT.jl's own `grdhisteq`. No maths lives here — the module does all of it.
#
# The C++ dialog is GrdHistEqDialog (70_window.cpp, loads deps/ui/grdhisteq_dialog.ui). Its two
# radios are the module's own two outputs, which GMT itself insists on ("Either -D or -G is required
# for output"):
#   grid   the equalized grid — either the equal-area CELL INDEX (-C cells, optionally quadratic -Q)
#          or standard NORMAL SCORES (-N[norm], which the module forbids combining with -C or -Q)
#   table  the data values that divide the grid into n_cells patches of equal area (-D)

# The module may hand back a grid, a table, or (when the API registered both of its outputs) a tuple
# holding whatever it produced. Pick the kind THIS run asked for rather than assuming the position.
function _ghq_pick(R, want::Type)
	R === nothing && error("grdhisteq returned nothing")
	isa(R, want) && return R
	if isa(R, Tuple) || isa(R, Vector)
		for r in R
			isa(r, want) && return r
		end
		# A Vector{GMTdataset} IS the table, in its multi-segment form.
		(want === GMTdataset && !isempty(R) && isa(R[1], GMTdataset)) && return R[1]
	end
	error("grdhisteq returned a $(typeof(R)), not a $(want === GMTgrid ? "grid" : "table")")
end

# -C: how many equal-area cells. Positive integer, which is the module's own check.
function _ghq_ncells(d::Dict{String,String})::Int
	v = _get(d, "ncells", "16")
	n = tryparse(Int, v)
	(n === nothing || n <= 0) && error("the number of cells must be a positive integer, not '$v'")
	return n
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdHistEqFn. Returns Cint 1 on success, 0 on failure.
function _on_grdhisteq(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The grid to equalize is the one ON DISPLAY (the dialog sends the active layer's label).
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		kw = Dict{Symbol,Any}()
		reg = _get(d, "region")
		(isempty(reg) || occursin("//", reg)) || (kw[:region] = reg)   # -R selects a subsection

		mode = _get(d, "mode", "grid")
		gaussian = (mode == "grid") && (_get(d, "flavour", "cells") == "gaussian")
		quad = _on(d, "quadratic")
		if gaussian
			# The module refuses these combinations itself; saying so here costs nothing and explains
			# WHY, which "Option -N: Cannot be combined with -C" on a hidden stream does not.
			quad && error("normal scores (N) cannot be combined with the quadratic distribution (Q)")
			norm = _get(d, "norm")
			if isempty(norm)
				kw[:N] = true
			else
				v = tryparse(Float64, norm)
				(v === nothing || v <= 0) && error("the score limit must be a positive number, not '$norm'")
				kw[:N] = norm                      # scores forced into the ±norm range
			end
		else
			kw[:C] = _ghq_ncells(d)
			quad && (kw[:Q] = true)
		end

		if mode == "table"
			kw[:D] = true
			D = _ghq_pick(GMT.grdhisteq(G; kw...), GMTdataset)
			# The three columns the module dumps: each cell's z range and the cell's own index.
			D.colnames = ["Start", "Stop", "Cell"][1:min(size(D.data, 2), 3)]
			out = _get(d, "outfile");  isempty(out) || GMT.gmtwrite(String(out), D)
			show_table(scene, D; name = "Equal-area levels ($(kw[:C]) cells)")
			return Cint(1)
		elseif mode != "grid"
			error("unknown grdhisteq mode '$mode'")
		end

		R = _ghq_pick(GMT.grdhisteq(G; kw...), GMTgrid)
		title = gaussian ? "Normal scores" :
		        quad     ? "Equalized ($(kw[:C]) cells, quadratic)" : "Equalized ($(kw[:C]) cells)"
		return _gm3d_deliver(scene, R, title, _get(d, "outfile"), false,
		                     "grdhisteq " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdhisteq", e)
		return Cint(0)
	end
end

function _register_grdhisteq()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdhisteq, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdhisteq_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
