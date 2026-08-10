# :unit tests for Tools > Vector Operations (src/lineops.jl, port of Mirone's line_operations.m).
# Everything here is pure Julia: the ported algorithms (Bernstein Bezier, the cardinal spline, the
# self-intersection search, the PPA ridge extractor, the stitching rules) and the command parser.
# No DLL, no window — the scene half is exercised by test-lineops-gui.jl.

@testitem "lineops: the command parser takes Mirone's own spellings" tags=[:unit, :lineops] begin
	IG = InteractiveGMT

	# DIST with a unit suffix is metres; a bare number in a geographic window is degrees of arc.
	@test IG._lop_dist("10K", true)  == (10000.0, true)
	@test IG._lop_dist("250M", true) == (250.0, true)
	@test IG._lop_dist("3N", true)   == (5556.0, true)
	@test IG._lop_dist("0.5", true)  == (0.5, false)
	# Cartesian: Mirone only looks for a unit suffix when the map is geographic, so "10K" is not a
	# number there — and a plain number is taken in map units.
	@test IG._lop_dist("10", false) == (10.0, false)
	@test_throws ErrorException IG._lop_dist("10K", false)
	@test_throws ErrorException IG._lop_dist("nonsense", true)

	o = IG._lop_buffer_args("10K 24 in SIDE=right BASE=0 TOP=0", true, "both")
	@test o.dist == 10000.0 && o.dir == "in" && o.npts == 24
	@test o.side == 'r' && o.base == false && o.top == false
	@test IG._lop_buffer_args("10K NPTS=33", true, "both").npts == 33     # spelled-out form too
	@test IG._lop_buffer_args("1", true, "out").dir == "out"              # 'closing' default
	@test IG._lop_buffer_args("1", true, "both").npts == 13               # Mirone's default
	@test_throws ErrorException IG._lop_buffer_args("DIST", true, "both") # the template, unedited
	@test_throws ErrorException IG._lop_buffer_args("", true, "both")

	# Degrees <-> metres, on the authalic radius Mirone uses.
	@test IG._lop_m2deg(IG._lop_deg2m(1.7)) ≈ 1.7
end

@testitem "lineops: bezier is the GLOBAL Bernstein curve" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	P = [0.0 0.0; 1.0 2.0; 2.0 0.0]
	B = IG._lop_bezier(P, 11)
	@test size(B) == (11, 2)
	# A Bezier interpolates its first and last control point and NOT the middle ones.
	@test B[1, :]   ≈ P[1, :]
	@test B[end, :] ≈ P[end, :]
	@test B[6, :]   ≈ [1.0, 1.0]            # quadratic at t = 1/2: (P1 + 2P2 + P3)/4
	@test !(B[6, :] ≈ P[2, :])
	# The binomial helper is nchoosek.
	@test IG._lop_nchoosek(5, 2) ≈ 10.0
	@test IG._lop_nchoosek(7, 0) == 1.0
end

@testitem "lineops: the cardinal spline passes through its knots" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	x = [0.0, 1.0, 2.0, 3.0];  y = [0.0, 1.0, 0.0, 1.0]
	xs, ys = IG._lop_spline_interp(x, y, 5)
	@test length(xs) == length(ys)
	@test xs[1] ≈ x[1] && ys[1] ≈ y[1]
	@test xs[end] ≈ x[end] && ys[end] ≈ y[end]
	# Every knot is reproduced somewhere along the resampled curve.
	for k in eachindex(x)
		@test minimum(abs.(xs .- x[k]) .+ abs.(ys .- y[k])) < 1e-9
	end
	# A straight line stays straight.
	xl, yl = IG._lop_spline_interp([0.0, 1.0, 2.0], [0.0, 1.0, 2.0], 4)
	@test all(abs.(yl .- xl) .< 1e-9)
	# Fewer than three points, or no subdivision asked for, gives the input back.
	@test IG._lop_spline_interp([0.0, 1.0], [0.0, 1.0], 5)[1] == [0.0, 1.0]
	@test IG._lop_spline_interp(x, y, 1)[1] == x

	# The tridiagonal solve it rests on: check it against the system it claims to solve.
	sol = IG._lop_thomas([0.0, 1, 1], [4.0, 4, 4], [1.0, 1, 0], [6.0, 6, 6])
	@test 4sol[1] + sol[2] ≈ 6
	@test sol[1] + 4sol[2] + sol[3] ≈ 6
	@test sol[2] + 4sol[3] ≈ 6
end

@testitem "lineops: self-crossings" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	# A bow tie crosses itself exactly once, in the middle.
	xc, yc, ic = IG._lop_intersections([0.0, 2, 2, 0, 0], [0.0, 2, 0, 2, 0])
	@test length(xc) == 1
	@test xc[1] ≈ 1.0 && yc[1] ≈ 1.0
	@test 1 <= ic[1] <= 2
	# A simple, non-crossing ring reports nothing (its shared first/last vertex is not a crossing).
	x2, _, _ = IG._lop_intersections([0.0, 1, 1, 0, 0], [0.0, 0, 1, 1, 0])
	@test isempty(x2)
	# A straight line has nothing to cross.
	x3, _, _ = IG._lop_intersections([0.0, 1, 2, 3], [0.0, 1, 2, 3])
	@test isempty(x3)
end

@testitem "lineops: stitch — closest end, join sense, cleanup, sort" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	me = [0.0 0.0; 1.0 0.0]
	# endType 3: `me` ends near the START of the other line.
	k, et = IG._lop_closest(me, [[1.01 0.0; 2.0 0.0]], 0.1)
	@test k == 1 && et == 3
	# endType 2: the other line ENDS near me's start.
	k, et = IG._lop_closest(me, [[-1.0 0.0; -0.01 0.0]], 0.1)
	@test k == 1 && et == 2
	# Out of tolerance: nothing found.
	@test IG._lop_closest(me, [[5.0 0.0; 6.0 0.0]], 0.1)[1] == 0
	# A closed polygon is never a stitching candidate.
	@test IG._lop_closest(me, [[1.0 0.0; 2.0 0.0; 2.0 1.0; 1.0 0.0]], 0.5)[1] == 0

	a = [0.0 0.0; 1.0 0.0];  b = [2.0 0.0; 1.01 0.0]
	@test IG._lop_join(a, b, 3) == [0.0 0.0; 1.0 0.0; 2.0 0.0; 1.01 0.0]
	@test IG._lop_join(a, b, 4) == [0.0 0.0; 1.0 0.0; 1.01 0.0; 2.0 0.0]

	# Cleanup: repeated points go, and ends within TOL close the ring.
	C = IG._lop_stitch_cleanup([0.0 0.0; 1.0 0.0; 1.0 0.0; 1.0 1.0; 0.001 0.0], 0.01)
	@test size(C, 1) == 5                       # 4 unique + the closing duplicate
	@test C[1, :] == C[end, :]

	# sortline re-orders by proximity.
	S = IG._lop_sortline([0.0 0.0; 5.0 5.0; 0.1 0.1; 5.1 5.1])
	@test size(S) == (4, 2)
	@test S[1, :] == [0.0, 0.0] && S[2, :] == [0.1, 0.1]
end

@testitem "lineops: delete SPUR keeps Mirone's own (conservative) rule" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	# A single out-and-back is NOT a spur for line_operations.m (it needs the surrounding dif
	# entries to vanish too) — the port must not be more eager than the original.
	M = [0.0 0; 1 0; 2 0; 1 0; 0 0]
	@test IG._lop_despur(M) == M
	# A line with no mirrored pair at all is returned untouched.
	@test IG._lop_despur([0.0 0; 1 0; 2 0; 3 0]) == [0.0 0; 1 0; 2 0; 3 0]
end

@testitem "lineops: the PPA ridge extractor finds a ridge" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	# A N-S ridge down column 3 of a 7x5 grid (row 1 = south, the orientation the port assumes).
	Z = zeros(Float64, 7, 5)
	for r in 1:7;  Z[r, 3] = 10.0;  Z[r, 2] = 3.0;  Z[r, 4] = 3.0;  end
	R = IG._ppa_ridges(Z, 0.0, 0.0, 1.0, 1.0)
	@test size(R, 2) == 2
	@test size(R, 1) > 0
	pts = R[.!isnan.(R[:, 1]), :]
	@test size(pts, 1) > 0
	# Every extracted point sits on (or right beside) the crest, x = 2 in these coordinates.
	@test all(abs.(pts[:, 1] .- 2.0) .< 1.0)
	# A grid too small to hold the padded stencil answers with nothing rather than throwing.
	@test size(IG._ppa_ridges(zeros(2, 2), 0.0, 0.0, 1.0, 1.0), 1) == 0
	# The neighbour-order helper cycles 1..8.
	@test IG._ppa_neb(9) == 1 && IG._ppa_neb(0) == 8 && IG._ppa_neb(5) == 5
end

@testitem "lineops: NaN-broken matrices split into segments" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	M = [0.0 0; 1 1; NaN NaN; 5.0 5; 6 6; 7 7]
	segs = IG._lop_split_nan(M)
	@test length(segs) == 2
	@test size(segs[1], 1) == 2 && size(segs[2], 1) == 3
	D = IG._lop_mat2ds(M)
	@test isa(D, Vector) && length(D) == 2
	@test isa(IG._lop_mat2ds([0.0 0; 1 1]), InteractiveGMT.GMT.GMTdataset)
	@test IG._lop_mat2ds(zeros(0, 2)) === nothing
	# A one-point stub is not a line and is dropped.
	@test IG._lop_mat2ds([1.0 1]) === nothing

	# …and back again.
	@test IG._lop_ds2mat(InteractiveGMT.GMT.mat2ds([0.0 0; 1 1])) == [0.0 0; 1 1]
	@test size(IG._lop_ds2mat(D), 1) == 6                    # 2 + separator + 3
end

@testitem "lineops: GMT_DB orientation and the near-vertex search" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	# check_bombordo: a CW ring is reversed, a CCW one is left alone.
	ccw = [0.0 0; 2 0; 2 2; 0 2; 0 0]
	@test IG._lop_ccw(ccw) == ccw || IG._lop_ccw(ccw) == reverse(ccw, dims = 1)
	@test size(IG._lop_ccw(ccw)) == size(ccw)

	M = [0.0 0; 1 0; 2 0; 3 0]
	@test IG._lop_near_index(M, 2.005, 0.0, 0.02) == 3
	@test IG._lop_near_index(M, 2.5, 0.0, 0.02) == 0          # nothing inside the box
end

@testitem "lineops: thicken builds N+1 parallel tracks" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	# A due-east cartesian line: the offsets are purely in y, and symmetric about the line.
	xy = [0.0 0.0; 1.0 0.0; 2.0 0.0]
	T = IG._lop_thicken_lines(xy, 4, 1.0, false)
	@test length(T) == 5
	@test all(size(t) == size(xy) for t in T)
	offs = [t[1, 2] - xy[1, 2] for t in T]
	@test offs ≈ [0.5, 0.25, 0.0, -0.25, -0.5]
	@test all(t[:, 1] ≈ xy[:, 1] for t in T)                 # x untouched for an E-W line
	@test_throws ErrorException IG._lop_thicken_lines([0.0 0.0], 4, 1.0, false)
end

@testitem "lineops: unknown / incomplete commands are refused" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	@test length(IG._LOP_OPS) == 21
	@test "toRidge" in IG._LOP_OPS && "GMT_DB" in IG._LOP_OPS
	# Every op that needs no pick is a real op.
	@test all(o in IG._LOP_OPS for o in IG._LOP_NOPICK)
	@test_throws ErrorException IG._lop_apply(C_NULL, "", String[])
	@test_throws ErrorException IG._lop_apply(C_NULL, "invented", String[])
	# A real op with nothing picked: "Apply WHERE????"
	@test_throws ErrorException IG._lop_apply(C_NULL, "bezier 50", String[])

	# pline needs no pick and no scene beyond the add — its parser is checked for its own errors.
	@test_throws ErrorException IG._lop_apply(C_NULL, "pline 1 2 3", String[])
	@test_throws ErrorException IG._lop_apply(C_NULL, "pline [1 2 3]", String[])
	@test_throws ErrorException IG._lop_apply(C_NULL, "pline [1 2 3; 4 5]", String[])
end

@testitem "lineops: the targets of a params block" tags=[:unit, :lineops] begin
	IG = InteractiveGMT
	d = IG._nswing_parse("cmd=buffer 10K\ntarget1=line A\ntarget2=line B\n")
	@test IG._get(d, "cmd") == "buffer 10K"
	@test IG._lop_targets(d) == ["line A", "line B"]
	@test isempty(IG._lop_targets(IG._nswing_parse("cmd=scale\n")))
end
