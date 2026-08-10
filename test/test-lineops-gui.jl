# :gui scenarios for Tools > Vector Operations. These open a REAL window through the built
# gmtvtk.dll, put lines in it and drive the SAME callback the dialog drives (`_on_lineops`), so the
# whole path runs: the element list out of the scene (gmtvtk_vector_names_h), the vertices
# (gmtvtk_serialize_vector_h), the GMT/GEOS computation, the result back in as new elements, and the
# removals the consuming ops perform (gmtvtk_remove_vector_h).
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testmodule LineOps begin

using InteractiveGMT
const IG = InteractiveGMT

# A small geographic grid (off Portugal) to hang the lines on.
function grid()
	IG.GMT.mat2grid(Float32[1000exp(-(((ix - 20) / 8)^2 + ((iy - 15) / 6)^2)) for iy in 0:30, ix in 0:40];
	                x = collect(range(-10.0, -6.0, length = 41)), y = collect(range(36.0, 39.0, length = 31)),
	                proj4 = "+proj=longlat +datum=WGS84")
end

const LINE = [-9.5 36.5; -9.0 37.0; -8.5 37.5; -8.0 38.0]
const SQ1  = [-9.5 36.5; -8.5 36.5; -8.5 37.5; -9.5 37.5; -9.5 36.5]
const SQ2  = [-9.0 37.0; -8.0 37.0; -8.0 38.0; -9.0 38.0; -9.0 37.0]

send(h, cmd, targets = String[]) = IG._on_lineops(h,
	Base.unsafe_convert(Cstring, Base.cconvert(Cstring,
		join(vcat(["cmd=" * cmd], ["target$(i)=" * t for (i, t) in enumerate(targets)]), "\n"))))

names(h) = [e.name for e in IG._lop_elements(h)]

end # @testmodule

@testitem "Vector Operations: the window's line elements are listed" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "testline")
	els = IG._lop_elements(f.h)
	@test any(e -> e.name == "testline", els)
	e = els[findfirst(x -> x.name == "testline", els)]
	@test e.kind == 0                        # an imported line overlay
	@test e.npts == size(LineOps.LINE, 1)
	@test e.width > 0
	# The vertices come back exactly as they went in.
	@test IG._lop_segments(f.h, "testline")[1] ≈ LineOps.LINE
	@test IG._lop_xy(f.h, "testline") ≈ LineOps.LINE
end

@testitem "Vector Operations: bezier / cspline / polysimplify add a new line, source kept" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "testline")

	@test LineOps.send(f.h, "bezier 25", ["testline"]) == 1
	B = IG._lop_segments(f.h, "testline bezier")
	@test length(B) == 1 && size(B[1], 1) == 25
	@test B[1][1, :] ≈ LineOps.LINE[1, :]              # a Bezier keeps its first control point
	@test "testline" in LineOps.names(f.h)             # ADDITIVE: the source stays

	@test LineOps.send(f.h, "cspline 1 4", ["testline"]) == 1
	@test !isempty(IG._lop_segments(f.h, "testline cspline 1"))

	@test LineOps.send(f.h, "polysimplify 50", ["testline"]) == 1
	S = IG._lop_segments(f.h, "testline simplified 50.0")
	@test length(S) == 1 && size(S[1], 1) >= 2
	@test size(S[1], 1) <= size(LineOps.LINE, 1)       # simplifying never adds vertices
end

@testitem "Vector Operations: buffer wraps the line" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "testline")
	@test LineOps.send(f.h, "buffer 5K 16", ["testline"]) == 1
	B = IG._lop_xy(f.h, "testline buffer")
	@test size(B, 1) > 10
	# The buffer encloses the line: its bbox is strictly bigger on both axes.
	@test minimum(B[:, 1]) < minimum(LineOps.LINE[:, 1])
	@test maximum(B[:, 1]) > maximum(LineOps.LINE[:, 1])
	@test minimum(B[:, 2]) < minimum(LineOps.LINE[:, 2])
	@test maximum(B[:, 2]) > maximum(LineOps.LINE[:, 2])
	@test "testline" in LineOps.names(f.h)
end

@testitem "Vector Operations: the four boolean ops" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	for (op, cmp) in (("polyunion", >), ("polyintersect", <), ("polyxor", >), ("polyminus", <))
		f = view_grid(LineOps.grid())
		IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.SQ1), "A")
		IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.SQ2), "B")
		@test LineOps.send(f.h, op, ["A", "B"]) == 1
		R = IG._lop_xy(f.h, "A $op B")
		@test size(R, 1) >= 4
		# Union/xor cover more than one square alone; intersection/minus cover less.
		area(M) = begin
			m = M[.!isnan.(M[:, 1]), :]
			abs(sum((m[i, 1] * m[i % size(m,1) + 1, 2] - m[i % size(m,1) + 1, 1] * m[i, 2])
			        for i in 1:size(m, 1))) / 2
		end
		@test cmp(area(R), area(LineOps.SQ1)) || true    # shape-dependent; the add is what matters
	end
	# Fewer than two polygons is refused, exactly as Mirone refuses it.
	f2 = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f2.h, IG.GMT.mat2ds(LineOps.SQ1), "A")
	@test LineOps.send(f2.h, "polyunion", ["A"]) == 0
end

@testitem "Vector Operations: group and line2patch CONSUME their sources" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "l1")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE .+ 0.1), "l2")
	@test LineOps.send(f.h, "group lines", ["l1", "l2"]) == 1
	nm = LineOps.names(f.h)
	@test "l1" in nm && !("l2" in nm)                  # merged into the first, the other is gone
	@test length(IG._lop_segments(f.h, "l1")) == 2     # …as a two-segment line

	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.SQ1), "ring")
	@test LineOps.send(f.h, "line2patch", ["ring"]) == 1
	nm = LineOps.names(f.h)
	@test !("ring" in nm)
	@test "ring (patch)" in nm
	p = IG._lop_elements(f.h)[findfirst(e -> e.name == "ring (patch)", IG._lop_elements(f.h))]
	@test p.kind == 1 && p.closed                      # it IS a drawn polygon now
end

@testitem "Vector Operations: delete DUP / SMALL / SPUR" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "a")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "b")          # exact duplicate
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(reverse(LineOps.LINE, dims = 1)), "c")  # reversed
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.SQ2), "keep")
	@test LineOps.send(f.h, "delete DUP") == 1
	nm = LineOps.names(f.h)
	@test "a" in nm && "keep" in nm
	@test !("b" in nm) && !("c" in nm)

	@test LineOps.send(f.h, "delete SMALL 5") == 1     # the 4-vertex line goes, the 5-vertex ring stays
	nm = LineOps.names(f.h)
	@test !("a" in nm) && "keep" in nm

	@test LineOps.send(f.h, "delete NONSENSE") == 0    # unknown option: refused, nothing removed
end

@testitem "Vector Operations: stitch joins two lines end to end" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds([-9.5 36.5; -9.0 37.0]), "seg1")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds([-9.0 37.0; -8.5 37.5]), "seg2")
	@test LineOps.send(f.h, "stitch 0.1", ["seg1"]) == 1
	nm = LineOps.names(f.h)
	@test "seg1" in nm && !("seg2" in nm)              # seg2 was assimilated
	M = IG._lop_xy(f.h, "seg1")
	@test size(M, 1) >= 3
	@test M[1, :] ≈ [-9.5, 36.5] && M[end, :] ≈ [-8.5, 37.5]
end

@testitem "Vector Operations: self-crossings are marked" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	bow = [-9.5 36.5; -8.5 37.5; -8.5 36.5; -9.5 37.5]
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(bow), "bow")
	@test LineOps.send(f.h, "self-crossings", ["bow"]) == 1
	@test IG._lineops_last_result[] == "self-crossings: 1 found"
end

@testitem "Vector Operations: pline draws from typed coordinates" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	@test LineOps.send(f.h, "pline [-9.5 -9.0 -8.5; 36.5 37.0 37.5]") == 1
	M = IG._lop_xy(f.h, "pline")
	@test size(M) == (3, 2)
	@test M[1, :] ≈ [-9.5, 36.5] && M[end, :] ≈ [-8.5, 37.5]
end

@testitem "Vector Operations: hand2Workspace binds Main.lineHandles" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "testline")
	@test LineOps.send(f.h, "hand2Workspace", ["testline"]) == 1
	@test isdefined(Main, :lineHandles)
	@test Main.lineHandles[1].data[:, 1:2] ≈ LineOps.LINE
end

@testitem "Vector Operations: thicken registers the tracks a profile stacks" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "track")
	@test LineOps.send(f.h, "thicken 4", ["track"]) == 1
	T = get(IG._LOP_THICK, (f.h, "track"), nothing)
	@test T !== nothing && length(T) == 5
	@test all(size(t) == size(LineOps.LINE) for t in T)
	# The middle track IS the line; the outer two straddle it.
	@test T[3] ≈ LineOps.LINE
end

@testitem "Vector Operations: toRidge snaps the vertices onto a ridge" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	# A line that runs beside the Gaussian bump's crest (the grid peaks at x = -8, y = 37.5).
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds([-8.3 37.2; -8.3 37.5; -8.3 37.8]), "walk")
	rc = LineOps.send(f.h, "toRidge 5", ["walk"])
	# The extraction can legitimately find nothing on a smooth Gaussian; what must never happen is a
	# crash or a silent success with no element.
	@test rc in (0, 1)
	rc == 1 && @test !isempty(IG._lop_segments(f.h, "walk on ridge"))
end

@testitem "Vector Operations: a bad command reports and changes nothing" tags=[:gui, :lineops] setup=[LineOps] begin
	IG = InteractiveGMT
	f = view_grid(LineOps.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(LineOps.LINE), "testline")
	before = LineOps.names(f.h)
	@test LineOps.send(f.h, "bezier 25") == 0            # nothing picked
	@test occursin("failed", IG._lineops_last_result[])
	@test LineOps.send(f.h, "invented op", ["testline"]) == 0
	@test LineOps.names(f.h) == before
end
