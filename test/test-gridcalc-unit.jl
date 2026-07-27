# CI-safe unit tests for the Grid calculator (gridcalc.jl, Grid Tools > "Grid calculator", port of
# Mirone's src_figs/grid_calculator.m). These never open a Qt+VTK window: the geometry filter, the
# name substitution and the expression evaluation are pure Julia, so they run anywhere
# `using InteractiveGMT` succeeds. The dialog itself (keypads, double-click insertion) is C++.

@testitem "gridcalc helpers present" tags=[:unit, :fast] begin
	for s in (:_gc_same_geom, :_gc_token_name, :_gc_replace_bare, :_gridcalc_objects,
	          :_gridcalc_names, :_gridcalc_eval, :_on_gridcalc, :_register_gridcalc)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "gridcalc: only same-geometry grids are combinable" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z, x, y) = GMT.mat2grid(Float32.(z); x = collect(x), y = collect(y))
	A = mk([1.0 2 3; 4 5 6; 7 8 9], range(0, 2, length = 3), range(0, 2, length = 3))
	B = mk([9.0 8 7; 6 5 4; 3 2 1], range(0, 2, length = 3), range(0, 2, length = 3))
	@test IG._gc_same_geom(B, A)                       # same size, limits, inc, registration
	@test IG._gc_same_geom(A, A)
	# different node count
	@test !IG._gc_same_geom(mk(ones(5, 5), range(0, 2, length = 5), range(0, 2, length = 5)), A)
	# same count, different limits (=> different inc)
	@test !IG._gc_same_geom(mk(ones(3, 3), range(0, 4, length = 3), range(0, 2, length = 3)), A)
	# a float-noise difference is still the SAME grid (file round-trips lose the last bit)
	C = deepcopy(A);  C.range[2] += 1e-12
	@test IG._gc_same_geom(C, A)
end

@testitem "gridcalc: the list offers only what can be combined" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z, n) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = n)), y = collect(range(0, 2, length = n)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9], 3);  B = mk(ones(3, 3), 3);  C = mk(ones(5, 5), 5)
	scene = Ptr{Cvoid}(UInt(0x51EDCA1C))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:grid, "", A), (:grid, "layer B", B), (:grid, "other", C)]
	try
		# The base grid is stored under the empty name and takes the label the dialog passes in.
		objs = IG._gridcalc_objects(scene, "topo.grd")
		@test first.(objs) == ["topo.grd", "layer B", "other"]
		# _gridcalc_names PRINTS the list (that is how the dialog reads it back over the eval bridge),
		# so capture stdout. redirect_stdout needs a real stream, not an IOBuffer.
		out = mktemp() do path, io
			redirect_stdout(() -> IG._gridcalc_names(scene, "topo.grd"), io)
			flush(io);  read(path, String)
		end
		@test split(strip(out), '\n') == ["topo.grd", "layer B"]     # `other` has other limits
	finally
		delete!(IG._SCENE_OBJS, scene)
	end
end

@testitem "gridcalc: expressions, bare names and &-tokens" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = 3)), y = collect(range(0, 2, length = 3)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9]);  B = mk([10.0 10 10; 20 20 20; 30 30 30])
	objs = Tuple{String,IG.GMTgrid}[("topo.grd", A), ("layer B", B)]
	files = Dict{String,String}()
	ev(e) = IG._gridcalc_eval(objs, files, e)

	@test ev("&topo.grd + &{layer B}") == A.z .+ B.z          # Mirone's own spelling
	@test ev("topo.grd + layer B")     == A.z .+ B.z          # bare, blanks and all
	@test ev("topo.grd - &{layer B}")  == A.z .- B.z          # mixed
	@test isapprox(ev("&{layer B} / &topo.grd - 1"), B.z ./ A.z .- 1)
	@test isapprox(ev(" sqrt( topo.grd) * log10(100)"), sqrt.(A.z) .* 2)
	@test ev("topo.grd * topo.grd") == A.z .^ 2               # repeated name = one variable
end

@testitem "gridcalc: bare names never eat a function call" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = 3)), y = collect(range(0, 2, length = 3)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9]);  B = mk(fill(2.0, 3, 3))
	# A grid called `a` must not match the `a` inside `atan`; `ab` (longer) wins over `a`.
	objs = Tuple{String,IG.GMTgrid}[("a", A), ("ab", B)]
	z = IG._gridcalc_eval(objs, Dict{String,String}(), "atan(a) + abs(ab) * 0")
	@test isapprox(z, atan.(A.z))
	# A grid whose label IS a function name can only be reached with the '&' spelling.
	objs2 = Tuple{String,IG.GMTgrid}[("abs", A)]
	@test IG._gridcalc_eval(objs2, Dict{String,String}(), "&abs * 2") == A.z .* 2
	@test_throws Exception IG._gridcalc_eval(objs2, Dict{String,String}(), "abs * 2")
end

@testitem "gridcalc: refusals" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z, n) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = n)), y = collect(range(0, 2, length = n)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9], 3);  C = mk(ones(5, 5), 5)
	objs = Tuple{String,IG.GMTgrid}[("topo", A), ("other", C)]
	nofiles = Dict{String,String}()
	@test_throws Exception IG._gridcalc_eval(objs, nofiles, "topo + other")   # geometry differs
	@test_throws Exception IG._gridcalc_eval(objs, nofiles, "&nope + 1")      # unknown name
	@test_throws Exception IG._gridcalc_eval(objs, nofiles, "1 + 2")          # uses no grid at all
	@test_throws Exception IG._gridcalc_eval(Tuple{String,IG.GMTgrid}[], nofiles, "topo")
end

@testitem "gridcalc: a grid loaded from disk joins the expression" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = 3)), y = collect(range(0, 2, length = 3)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9]);  B = mk([10.0 10 10; 20 20 20; 30 30 30])
	tmp = joinpath(mktempdir(), "gc_disk.grd")
	GMT.gmtwrite(tmp, B)
	try
		objs = Tuple{String,IG.GMTgrid}[("topo", A)]
		# "Load Grid" only stores the name; the file is read HERE, and geometry-checked like any other.
		z = IG._gridcalc_eval(objs, Dict("gc_disk.grd" => tmp), "topo + gc_disk.grd")
		@test isapprox(z, A.z .+ B.z)
		@test_throws Exception IG._gridcalc_eval(objs, Dict("gc_disk.grd" => tmp), "topo + missing.grd")
	finally
		rm(tmp, force = true)
	end
end

@testitem "gridcalc: token spelling" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._gc_token_name("&topo.grd")   == "topo.grd"
	@test IG._gc_token_name("&{layer B}")  == "layer B"
	@test IG._gc_token_name("&{}")         == ""
	# _gc_replace_bare only ever touches labels it was given, longest first.
	seen = String[]
	out = IG._gc_replace_bare("aa + a + bb", ["a", "aa"], nm -> (push!(seen, nm); "V"))
	@test out == "V + V + bb"
	@test sort(unique(seen)) == ["a", "aa"]
end
