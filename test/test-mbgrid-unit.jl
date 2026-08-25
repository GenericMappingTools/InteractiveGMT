# Unit tests for `mbgrid` (src/mbgrid.jl), the Julia side of deps/src/mbgrid.c. The C NUMERICS have
# their own standalone suite (deps/src/test_mbgrid.c, built as deps/build/test_mbgrid) — what is
# checked here is the part that suite cannot see: the argument coercions, the ABI mirror, and the
# two ccall paths actually returning a GMTgrid registered where it says it is.
#
# Everything that touches the DLL is gated on the library having loaded, so a DLL-less checkout
# still passes — same shape as the rest of the :unit tier.

@testitem "mbgrid: exports and DLL symbols" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test isdefined(IG, :mbgrid)
	@test mbgrid === IG.mbgrid                       # it is exported, not just internal
	# Resolved through the ONE symbol table, like every other export of this library — a build too
	# old to carry them is stale for exactly the same reason it would be missing a gmtvtk_* export.
	for s in (:mbgrid_dims, :mbgrid_work_dims, :mbgrid_work_origin, :mbgrid_bin, :mbgrid_zgrid,
	          :mbgrid_nodes, :mbgrid_fill, :mbgrid_extract, :mbgrid_run, :mbgrid_strerror)
		@test s in IG._LIB_SYMBOLS
	end
end

@testitem "mbgrid: the ABI mirror matches deps/src/mbgrid.h" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# 9 doubles then 6 int32 = 96, no tail padding; 4 pointers + 2 int32 = 40; 2 doubles + 4
	# int64 = 48. A silent change here is a struct read as garbage across the DLL boundary.
	@test sizeof(IG._MBParams) == 96
	@test sizeof(IG._MBBreakline) == 40
	@test sizeof(IG._MBStats) == 48
	@test fieldnames(IG._MBParams)[1:6] == (:west, :east, :south, :north, :xinc, :yinc)
	@test fieldnames(IG._MBParams)[10:15] == (:nx, :ny, :clipmode, :clip, :verbose, :registration)
	@test all(t -> t === Cdouble, fieldtypes(IG._MBParams)[1:9])
	@test all(t -> t === Cint, fieldtypes(IG._MBParams)[10:15])
	# The clip modes are the C's MBGRID_INTERP_* values, in the C's order.
	@test IG._MB_INTERP == Dict(:none => Cint(0), :gap => Cint(1), :near => Cint(2), :all => Cint(3))
	@test IG._MB_LAY_BCB == Cint(0)
	@test IG._MB_SG_COLMAJOR == Cint(2)              # MBGRID_SG_COLMAJOR: a "BCB" grid, untransposed
end

@testitem "mbgrid: region, inc and knobs take the dialog's strings" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# The Interpolate dialog sends everything as text; the REPL sends numbers and Symbols. Both
	# land on the SAME function, so the coercions live there and not in a dialog-only wrapper.
	@test IG._mb_region("0/10/30/45") == (0.0, 10.0, 30.0, 45.0)
	@test IG._mb_region("0,10,30,45") == (0.0, 10.0, 30.0, 45.0)
	@test IG._mb_region((0, 10, 30, 45)) == (0.0, 10.0, 30.0, 45.0)
	@test IG._mb_region([0.0, 10.0, 30.0, 45.0]) == (0.0, 10.0, 30.0, 45.0)
	@test_throws Exception IG._mb_region("0/10/30")
	@test IG._mb_inc("0.5") == (0.5, 0.5)
	@test IG._mb_inc("0.5/0.25") == (0.5, 0.25)
	@test IG._mb_inc(0.5) == (0.5, 0.5)
	@test IG._mb_inc((0.5, 0.25)) == (0.5, 0.25)

	@test IG._mb_num("1.5") == 1.5
	@test IG._mb_num("") == 0.0                      # an empty box means "the default", i.e. 0
	@test IG._mb_int("3") === Cint(3)
	@test IG._mb_bool("true") && IG._mb_bool("1") && !IG._mb_bool("0") && !IG._mb_bool("")
	@test IG._mb_sym("Near") === :near               # the combo's data string, whatever its case
	@test IG._mb_reg(:pixel) === Cint(1)
	@test IG._mb_reg("gridline") === Cint(0)
	@test IG._mb_reg(1) === Cint(1)
	@test_throws Exception IG._mb_reg(:sideways)
end

@testitem "mbgrid: x,y,z and breaklines out of every container" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mat = [0.0 1.0 10.0; 2.0 3.0 20.0]
	@test IG._mb_xyz(mat) == ([0.0, 2.0], [1.0, 3.0], [10.0, 20.0])
	@test IG._mb_xyz(GMT.mat2ds(mat)) == ([0.0, 2.0], [1.0, 3.0], [10.0, 20.0])
	@test IG._mb_xyz([0.0, 2.0], [1.0, 3.0], [10.0, 20.0]) == ([0.0, 2.0], [1.0, 3.0], [10.0, 20.0])
	@test_throws Exception IG._mb_xyz([0.0 1.0; 2.0 3.0])          # only two columns

	@test IG._mb_break(nothing) == (Float64[], Float64[], Float64[], Cint[])
	# Multi-segment stays multi-segment: the densifier must NOT draw a line from the end of one
	# segment to the start of the next.
	bx, by, bz, len = IG._mb_break([GMT.mat2ds(mat), GMT.mat2ds([5.0 5.0 1.0; 6.0 6.0 2.0; 7.0 7.0 3.0])])
	@test len == Cint[2, 3]
	@test length(bx) == length(by) == length(bz) == 5
	@test IG._mb_break(mat)[4] == Cint[2]
end

@testitem "mbgrid: grids a plane, both solvers, both registrations" tags=[:unit] begin
	IG = InteractiveGMT; GMT = IG.GMT
	if IG._DLL[] == C_NULL
		@test_skip "viewer library not loaded"
	else
		plane(x, y) = 3.0 + 2.0 * x - 1.5 * y
		pts = [(x, y) for x in 0:0.5:10 for y in 0:0.5:10]
		X = Float64[p[1] for p in pts]
		Y = Float64[p[2] for p in pts]
		Z = Float64[plane(p[1], p[2]) for p in pts]

		G = mbgrid(X, Y, Z; region = (0, 10, 0, 10), inc = 0.5)
		@test isa(G, GMT.GMTgrid)
		@test size(G.z) == (21, 21)                  # (ny, nx), column-major = the C's BCB
		@test G.x[1] == 0.0 && G.x[end] == 10.0
		@test G.y[1] == 0.0 && G.y[end] == 10.0
		@test G.registration == 0
		# z[iy, ix] with row 1 = SOUTH is what BCB means; the interior is the plane exactly.
		@test G.z[11, 11] ≈ plane(5.0, 5.0) atol = 1e-3
		@test G.z[3, 17] ≈ plane(8.0, 1.0) atol = 1e-3
		@test occursin("mbgrid", G.command)          # provenance is stamped, as for any derived grid
		@test occursin("nodes from data", G.remark)

		# A hole in the middle must be filled by the spline, not left NaN.
		keep = [!(4.0 <= X[i] <= 6.0 && 4.0 <= Y[i] <= 6.0) for i in eachindex(X)]
		Gh = mbgrid(X[keep], Y[keep], Z[keep]; region = (0, 10, 0, 10), inc = 0.5)
		@test !isnan(Gh.z[11, 11])
		@test Gh.z[11, 11] ≈ plane(5.0, 5.0) atol = 0.1
		# ...and :none must leave it alone, which is the same call with one knob moved.
		Gn = mbgrid(X[keep], Y[keep], Z[keep]; region = (0, 10, 0, 10), inc = 0.5, clipmode = :none)
		@test isnan(Gn.z[11, 11])

		# The GMT.surface solver goes down the other branch of the same function and must land on
		# the same grid geometry. Its answer is merged UNTRANSPOSED (MBGRID_SG_COLMAJOR).
		Gs = mbgrid(X[keep], Y[keep], Z[keep]; region = (0, 10, 0, 10), inc = 0.5, solver = :surface)
		@test size(Gs.z) == (21, 21)
		@test Gs.z[11, 11] ≈ plane(5.0, 5.0) atol = 0.1
		@test occursin("solver=surface", Gs.command)

		# Pixel registration: 20 CELLS, first node half a cell in. The dialog's registration
		# checkbox is shared by every gridding method, so mbgrid has to answer it too.
		Gp = mbgrid(X, Y, Z; region = (0, 10, 0, 10), inc = 0.5, registration = :pixel)
		@test size(Gp.z) == (20, 20)
		@test Gp.registration == 1
		# A pixel grid's x/y are the CELL BOUNDARIES (nx+1 of them), GMT's own convention — the
		# node centres are half a cell in from the first one.
		@test length(Gp.x) == 21 && length(Gp.y) == 21
		@test Gp.x[1] ≈ 0.0 && Gp.x[end] ≈ 10.0
		@test Gp.z[11, 11] ≈ plane(0.25 + 10 * 0.5, 0.25 + 10 * 0.5) atol = 1e-3
	end
end

@testitem "mbgrid: a breakline pins the nodes it crosses" tags=[:unit] begin
	IG = InteractiveGMT; GMT = IG.GMT
	if IG._DLL[] == C_NULL
		@test_skip "viewer library not loaded"
	else
		X = Float64[4, 6, 4, 6];  Y = Float64[4, 4, 6, 6];  Z = zeros(4)
		bl = GMT.mat2ds([3.0 5.0 100.0; 7.0 5.0 100.0])
		G = mbgrid(X, Y, Z; region = (0, 10, 0, 10), inc = 0.5, breakline = bl, clipmode = :none)
		@test G.z[11, 11] ≈ 100.0 atol = 1e-3        # (x, y) = (5, 5) sits on the line
		@test isnan(G.z[2, 2])                        # nowhere near either, and no spline ran
	end
end

@testitem "mbgrid: refuses what it cannot grid" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	X = Float64[0, 1, 2];  Y = Float64[0, 1, 2];  Z = Float64[1, 2, 3]
	@test_throws Exception mbgrid(X, Y, Z; inc = 1)                       # no region
	@test_throws Exception mbgrid(X, Y, Z; region = (0, 10, 0, 10))       # no inc
	@test_throws Exception mbgrid(X, Y, [1.0, 2.0]; region = (0, 10, 0, 10), inc = 1)
	@test_throws Exception mbgrid(X, Y, Z; region = (0, 10, 0, 10), inc = 1, clipmode = :sometimes)
	@test_throws Exception mbgrid(X, Y, Z; region = (0, 10, 0, 10), inc = 1, solver = :magic)
end
