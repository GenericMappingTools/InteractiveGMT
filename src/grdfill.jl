# grdfill.jl — GMT menu > "grdfill": fill the holes in the window's grid, or just report where they
# are, through GMT.jl's own `grdfill`. A "hole" is a run of NaNs by default, or of any other value
# the dialog names (-N).
#
# The C++ dialog is GrdFillDialog (70_window.cpp, loads deps/ui/grdfill_dialog.ui). Its two radios
# are the module's own two outcomes:
#   fill   the grid with its holes filled by one of the four algorithms (-A)
#   list   the holes themselves (-L): their bounding boxes, or closed polygons (-Lp) — no fill takes
#          place, and the polygons can also be DRAWN on the map, which is the fastest way to see
#          where the gaps in a grid actually are

# The four fill algorithms, each with the argument it takes appended (or nothing at all when the
# module's own default is wanted). Letters are GMT's; the dialog offers them by name.
function _gfill_A(d::Dict{String,String})::String
	algo = _get(d, "algo")
	isempty(algo) && error("choose a fill algorithm")
	if algo == "c"
		v = _get(d, "value")
		isempty(v) && error("a constant fill needs the value to fill with")
		(tryparse(Float64, v) === nothing) && error("the fill value must be a number, not '$v'")
		return "c" * v
	elseif algo == "n"
		r = _get(d, "radius")
		isempty(r) && return "n"                    # the module's own radius
		(tryparse(Float64, r) === nothing || parse(Float64, r) <= 0) &&
			error("the search radius is a positive number of pixels, not '$r'")
		return "n" * r
	elseif algo == "s"
		t = _get(d, "tension")
		isempty(t) && return "s"                    # bicubic spline, no tension
		tv = tryparse(Float64, t)
		(tv === nothing || !(0 <= tv < 1)) && error("the spline tension is a number in [0,1), not '$t'")
		return "s" * t
	elseif algo == "g"
		# Sample a (possibly coarser) grid at the hole nodes. GMT reads that grid itself, so its path
		# travels as written — but a path that is not there is worth catching here, where it can be
		# said plainly.
		f = _get(d, "gridfile")
		isempty(f) && error("sampling another grid needs that grid")
		isfile(f) || error("grid not found: $f")
		return "g" * f
	end
	error("unknown fill algorithm '$algo'")
end

# Column names for what -L reports: one row per hole (its bounding box), or the closed polygons of
# those boxes with -Lp.
_gfill_colnames(polygons::Bool, ncol::Int) =
	(polygons ? ["x", "y"] : ["West", "East", "South", "North"])[1:min(ncol, polygons ? 2 : 4)]

const _GFILL_HOLES_LAYER = "Hole outlines"

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdFillFn. Returns Cint 1 on success, 0 on failure.
function _on_grdfill(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The grid to fill is the one ON DISPLAY (the dialog sends the active layer's label).
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		kw = Dict{Symbol,Any}()
		reg = _get(d, "region")
		(isempty(reg) || occursin("//", reg)) || (kw[:region] = reg)
		nod = _get(d, "nodata")
		if !isempty(nod)
			(tryparse(Float64, nod) === nothing) &&
				error("the hole value must be a number (leave it empty for NaN), not '$nod'")
			kw[:N] = nod
		end

		if _get(d, "mode", "fill") == "list"
			polygons = _on(d, "polygons")
			kw[:L] = polygons ? "p" : true
			R = GMT.grdfill(G; kw...)
			(R === nothing || isempty(R)) && error("this grid has no holes to report")
			D = isa(R, Vector) ? R : [R]
			ncol = size(D[1].data, 2)
			names = _gfill_colnames(polygons, ncol)
			for seg in D
				seg.colnames = names
			end
			out = _get(d, "outfile");  isempty(out) || GMT.gmtwrite(String(out), length(D) == 1 ? D[1] : D)
			show_table(scene, length(D) == 1 ? D[1] : D;
			           name = polygons ? "Hole polygons" : "Hole bounding boxes")
			# The polygons are also worth SEEING: they go on the map as a line layer, replacing the
			# previous one, so re-running after a fill shows what is left rather than piling up.
			if polygons && _on(d, "draw")
				ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring),
				      scene, _GFILL_HOLES_LAYER)
				_add_geo_overlay(scene, D; color = (0.9, 0.1, 0.1), linewidth = 1.5,
				                 name = _GFILL_HOLES_LAYER) ||
					error("could not draw the hole outlines in this window")
			end
			return Cint(1)
		end

		A = _gfill_A(d)
		kw[:A] = A
		R = GMT.grdfill(G; kw...)
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")
		how = startswith(A, "c") ? "constant $(A[2:end])" :
		      startswith(A, "n") ? "nearest neighbour" :
		      startswith(A, "s") ? "spline" : "sampled grid"
		return _gm3d_deliver(scene, R, "Filled ($how)", _get(d, "outfile"), false,
		                     "grdfill " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdfill", e)
		return Cint(0)
	end
end

function _register_grdfill()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdfill, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdfill_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
