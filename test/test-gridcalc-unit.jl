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

# One-band images are operands (a Landsat band, any 16-bit image) — at FULL precision, the stashed
# original, never the 8-bit display copy. An RGB picture is not one value per node: not offered.
@testitem "gridcalc: one-band images are operands, at full precision" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	nx, ny = 4, 3
	img(A) = GMT.mat2img(A; x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true)
	B4 = img(UInt16[1000 * j + i for i in 1:ny, j in 1:nx])
	B5 = img(UInt16[3000 + 10 * (j + i) for i in 1:ny, j in 1:nx])
	shown4 = IG._stretch_to_u8(B4);  shown5 = IG._stretch_to_u8(B5)   # what the window displays
	rgb = img(rand(UInt8, ny, nx, 3))
	scene = Ptr{Cvoid}(UInt(0x51EDCA1D))
	IG._SCENE_OBJS[scene] = Tuple{Symbol,String,Any}[(:image, "B4", shown4), (:image, "B5", shown5), (:image, "rgb", rgb)]
	IG._IMG_ORIG[scene] = Dict("B4" => B4, "B5" => B5)
	try
		objs = IG._gridcalc_objects(scene, "Surface")
		@test first.(objs) == ["B4", "B5"]                               # the RGB picture is not offered
		z = IG._gridcalc_eval(objs, Dict{String,String}(), "(B5 - B4) / (B5 + B4)")
		@test isapprox(z, (Float32.(B5.image) .- Float32.(B4.image)) ./ (Float32.(B5.image) .+ Float32.(B4.image)))
		@test maximum(IG._gridcalc_eval(objs, Dict{String,String}(), "B4")) == 1000 * nx + ny   # DNs, not bytes
	finally
		delete!(IG._SCENE_OBJS, scene);  delete!(IG._IMG_ORIG, scene)
	end
end

# A window with no grid at all still computes: a "Load Grid" file (a band .TIF) is the reference.
@testitem "gridcalc: computes with no grid in the window" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	nx, ny = 4, 3
	# Values under 256: gmtwrite stores this small image as 8-bit.
	B = GMT.mat2img(UInt8[10 * j + i for i in 1:ny, j in 1:nx]; x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true)
	tmp = joinpath(mktempdir(), "LC09_B4.tif")
	GMT.gmtwrite(tmp, B)
	try
		files = Dict("LC09_B4.tif" => tmp)
		objs = IG._gc_with_reference(Tuple{String,IG.GMTgrid}[], files, "LC09_B4.tif * 2")
		@test first.(objs) == ["LC09_B4.tif"]
		z = IG._gridcalc_eval(Tuple{String,IG.GMTgrid}[], files, "LC09_B4.tif * 2")
		@test sort(vec(z)) == sort(vec(Float32.(B.image) .* 2))
		@test_throws Exception IG._gc_with_reference(Tuple{String,IG.GMTgrid}[], Dict{String,String}(), "1")
	finally
		try rm(tmp, force = true) catch end     # GDAL may still hold the .tif open (Windows)
	end
end

# A band image read from DISK (the reader every open/drop uses) lands in the calculator the right way
# up: node for node the same as GDAL's own grid read of that file. It used to come out flipped.
@testitem "gridcalc: a band read from disk keeps its orientation" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	nx, ny = 5, 4
	B = GMT.mat2img(UInt8[10 * j + i for i in 1:ny, j in 1:nx]; x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true)
	tmp = joinpath(mktempdir(), "LC09_Bx.tif")
	GMT.gmtwrite(tmp, B)
	try
		I = IG._gmtread_trb(tmp)                     # an image: the plain reader, as on open/drop
		@test I isa GMT.GMTimage
		G = IG._gc_image_grid(I)
		R = GMT.gmtread(tmp, grd = true)              # GDAL's own grid of the same file
		@test IG._zmat(G) == Float32.(IG._zmat(R))
		# …and through the whole calculator
		z = IG._gridcalc_eval(Tuple{String,IG.GMTgrid}[("b", G)], Dict{String,String}(), "b * 1")
		@test IG._zmat(GMT.mat2grid(Float32.(z), G)) == Float32.(IG._zmat(R))
	finally
		try rm(tmp, force = true) catch end     # GDAL may still hold the .tif open (Windows)
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

# Comparisons and logic: `> >= < <= == !=` on grids give a MASK; `& | ! xor` combine MASKS; a mask in
# the window (a Bool image) is a mask operand, never a 0/1 grid. Which operator takes which operand is
# CHECKED before anything is evaluated: the wrong kind is a `_GcRejected` with a readable message,
# never a MethodError from the middle of the broadcast.
@testitem "gridcalc: comparisons and logic on grids and masks" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = 3)), y = collect(range(0, 2, length = 3)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9]);  B = mk([10.0 10 10; 20 20 20; 30 30 30])
	M = IG._gc_image_grid(IG._gc_mask_image(Bool[1 0 1; 0 1 0; 1 1 0], A))
	@test M isa IG.GMTgrid{Bool,2}                        # a mask operand stays a mask
	objs = Tuple{String,IG.GMTgrid}[("topo", A), ("layer B", B), ("msk", M)]
	ev(e) = IG._gridcalc_eval(objs, Dict{String,String}(), e)

	@test ev("topo > 4") == (A.z .> 4)
	@test eltype(ev("topo > 4")) == Bool                  # -> a mask result
	@test ev("(topo > 4) & (layer B < 25)") == ((A.z .> 4) .& (B.z .< 25))
	@test ev("topo > 4 && layer B < 25")    == ((A.z .> 4) .& (B.z .< 25))
	@test ev("!(topo >= 5) | msk") == (.!(A.z .>= 5) .| M.z)
	@test ev("2 < topo <= 6") == (2 .< A.z .<= 6)
	@test ev("msk == (topo > 4)") == (M.z .== (A.z .> 4))
	@test ev("xor(msk, topo > 3)") == xor.(M.z, A.z .> 3)
	@test ev("isnan(topo) | msk") == M.z
	@test ev("topo * msk") == A.z .* M.z                  # a mask in arithmetic is 0/1
	@test isequal(ev("ifelse(msk, topo, NaN)"), ifelse.(M.z, A.z, NaN))
	# `&` after an operand is AND; where an operand is expected it is still Mirone's name prefix
	@test ev("(topo > 1) &msk") == ((A.z .> 1) .& M.z)
	@test ev("&topo + &{layer B}") == A.z .+ B.z

	rejected(e) = try ev(e); false catch x; x isa IG._GcRejected end
	for e in ("topo & msk", "topo > 1 & msk", "msk > 0", "msk == topo", "!topo", "foo + topo",
	          "bar(topo)", "ifelse(topo, 1, 2)", "topo +", "topo; msk", "topo[1]", "isnan(msk)")
		@test rejected(e)
	end
end

# ifelse(mask, A, B) with an RGB image: B (or A) a whole number 0-255, a colour (r, g, b) or another
# RGB image (Mirone run_cmd's mask(Z) and mix(Z, img2, mask)). An RGB image anywhere else is refused
# BEFORE computing. The source image is never painted in place.
@testitem "gridcalc: ifelse paints RGB images" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mk(z) = GMT.mat2grid(Float32.(z); x = collect(range(0, 2, length = 3)), y = collect(range(0, 2, length = 3)))
	A = mk([1.0 2 3; 4 5 6; 7 8 9])
	arr  = UInt8[10i + j + 100b for i in 1:3, j in 1:3, b in 0:2]
	arr2 = UInt8[200 + i + j + b for i in 1:3, j in 1:3, b in 0:2]
	I = GMT.mat2img(arr, A);  I2 = GMT.mat2img(arr2, A)
	@test IG._gc_same_geom(I, A)
	objs = Tuple{String,IG.GMTgrid}[("topo", A)]
	rgbs = Tuple{String,IG.GMTimage}[("img", I), ("img2", I2)]
	ev(e) = IG._gridcalc_eval(objs, Dict{String,String}(), e; rgbs = rgbs)
	m = A.z .> 4
	expect(col) = UInt8[m[i, j] ? arr[i, j, b] : col[b] for i in 1:3, j in 1:3, b in 1:3]
	@test ev("ifelse(topo > 4, img, 255)").image == expect((255, 255, 255))
	@test ev("ifelse(topo > 4, img, (255, 0, 7))").image == expect((255, 0, 7))
	@test ev("ifelse(topo <= 4, (1, 2, 3), img)").image == expect((1, 2, 3))      # the colour as A
	@test ev("ifelse(topo > 4, img, img2)").image ==
	      UInt8[m[i, j] ? arr[i, j, b] : arr2[i, j, b] for i in 1:3, j in 1:3, b in 1:3]
	@test I.image == arr                                                            # source untouched

	rejected(e) = try ev(e); false catch x; x isa IG._GcRejected end
	for e in ("img + 1", "img > 3", "!img", "img", "ifelse(img, topo, 1)", "(1,2,3) + topo",
	          "ifelse(topo > 4, img, 300)", "ifelse(topo > 4, img, 2.5)", "ifelse(topo > 4, img, (1, 2))",
	          "ifelse(topo > 4, img, topo)", "ifelse(topo > 4, topo, (1,2,3))")
		@test rejected(e)
	end
end

# An RGB image as every open reads it (row-major, pixel-interleaved "BRPa") against a mask made from
# GDAL's own grid of its band 1 (column-major "BCB"): the two layouts are matched node for node, so
# band 1 of the result is that grid, masked — not a flipped or sheared copy of it.
@testitem "gridcalc: ifelse on an RGB image read from disk keeps its orientation" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	nx, ny = 5, 4
	rgb = UInt8[(10 * j + i + 60b) % 256 for i in 1:ny, j in 1:nx, b in 0:2]
	f3 = joinpath(mktempdir(), "gc_rgb.tif")
	GMT.gmtwrite(f3, GMT.mat2img(rgb; x = [0.0, nx], y = [0.0, ny], noconv = true))
	try
		Ir = IG._gmtread_trb(f3)
		Gb = GMT.gmtread(f3, grd = true)                 # band 1, as GDAL itself reads it
		@test IG._gc_same_geom(Ir, Gb)
		R = IG._gridcalc_eval(Tuple{String,IG.GMTgrid}[("b1", Gb)], Dict{String,String}(),
		                      "ifelse(b1 > 30, rgb, 0)"; rgbs = Tuple{String,IG.GMTimage}[("rgb", Ir)])
		idx, npl, _ = IG._gc_rgb_index(R)
		Z = IG._zmat(Gb)
		@test all(R.image[idx(ix, iy)] == (Z[iy, ix] > 30 ? UInt8(Z[iy, ix]) : 0x00) for ix in 1:nx, iy in 1:ny)
		@test count(Z .> 30) > 0 && count(Z .<= 30) > 0     # both branches really taken
	finally
		try rm(f3, force = true) catch end     # GDAL may still hold the .tif open (Windows)
	end
end
