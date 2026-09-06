# "Nested grids" (tsunami) rectangle chain — the COMCOT/NSWING quantization rule.
#
# THE LAW THIS FILE EXISTS TO ENFORCE: AN INNER NESTED GRID NEVER CHANGES ITS PARENTS, ONLY ITS
# CHILDREN. Editing, adding or restoring a rectangle re-quantizes the chain (nestReflow,
# deps/src/85_polygon.cpp, port of Mirone's resize2nesting_size in utils/nesting_sizes.m), and every
# rectangle ABOVE the one being touched must come out of it standing exactly where it stood — to the
# last bit, not "about the same".
#
# How it was broken, so the shape of the regression is on record: the port snapped each edge with a
# floor/ceil ("round OUTWARD so the rect always encloses the drawn box") instead of Mirone's
# find_nearest. Outward rounding is NOT idempotent — a rect whose edge already sits half a parent
# cell outside a node rounds out to the NEXT node — so every reflow grew every rectangle in the
# chain by one parent cell, parents included. There is no rounding in the nesting rule. Nearest-node
# snapping is idempotent, which is precisely what lets the whole chain be walked on any edit without
# anything but the edited rect and its descendants moving.
#
# Tier: :gui — nestReflow lives in the DLL and needs a real Scene with a base grid, so these open a
# window (INTERACTIVEGMT_TEST_GUI=1, QT_QPA_PLATFORM=offscreen for headless).

# Base grid for the chain: 101x101 nodes over [0,10]x[0,10] -> inc 0.1, gridline registration.
# Every rectangle below is drawn deliberately OFF the parent's nodes, so the snap has work to do and
# a change in snapping rule cannot pass unnoticed.
@testitem "nested grids: adding a child never moves its parents" tags=[:gui, :nested] begin
	IG = InteractiveGMT; GMT = IG.GMT

	# Read every nestKind==1 rectangle back out of the scene as (name, xi, yi, W, E, S, N). The "N;"
	# records of gmtvtk_serialize_faults are the same ones a session round-trips through, so this
	# asserts on the rectangles the viewer really holds, not on a Julia-side copy of them.
	nestrects(h) = begin
		out = NamedTuple[]
		for line in split(IG._serialize_faults_raw(h), '\n'; keepempty=false)
			startswith(line, "N;") || continue
			p = split(line, ';'; limit=6)
			length(p) < 6 && continue
			xy, nv = IG._parse_xy2(p[6])
			nv < 2 && continue
			xs = @view xy[1:2:end];  ys = @view xy[2:2:end]
			push!(out, (name = String(p[5]), xi = parse(Float64, p[2]), yi = parse(Float64, p[3]),
			            w = minimum(xs), e = maximum(xs), s = minimum(ys), n = maximum(ys)))
		end
		return out
	end

	addrect(h, x0, x1, y0, y1, xi, yi, name) = begin
		xy = [x0, y0,  x1, y0,  x1, y1,  x0, y1,  x0, y0]
		ccall(IG._fn(:gmtvtk_add_nested_rect), Cint,
		      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cint, Cstring),
		      h, xy, Cint(5), xi, yi, Cint(0), name)
	end

	# Each daughter edge must sit at parent_node ∓ parent_inc/2 ± child_inc/2 — the rule the whole
	# tool exists to enforce (nswing.jl's _nest_binning checks the same thing on the grids).
	on_parent_node(edge, halfshift, p0, pinc) = begin
		k = (edge + halfshift - p0) / pinc
		abs(k - round(k)) < 1e-9
	end

	G = GMT.mat2grid(zeros(Float32, 101, 101); x = [0.0, 10.0], y = [0.0, 10.0])
	f = view_grid(G)
	try
		IG._pump_once()
		bx0, binc = 0.0, 0.1                      # base grid origin + increment (the level-1 parent)

		@test addrect(f.h, 2.34, 7.71, 2.16, 7.63, 0.02, 0.02, "Nested rectangle 1") >= 0
		IG._pump_once()
		r = nestrects(f.h)
		@test length(r) == 1
		lvl1 = r[1]

		# Level 1 obeys the rule against the BASE grid.
		@test on_parent_node(lvl1.w,  binc / 2 - lvl1.xi / 2, bx0, binc)
		@test on_parent_node(lvl1.e, -binc / 2 + lvl1.xi / 2, bx0, binc)
		@test on_parent_node(lvl1.s,  binc / 2 - lvl1.yi / 2, bx0, binc)
		@test on_parent_node(lvl1.n, -binc / 2 + lvl1.yi / 2, bx0, binc)

		# ── the law ── add a CHILD. Level 1 is now an ancestor and must not have moved at all.
		@test addrect(f.h, 3.53, 6.42, 3.61, 6.38, 0.004, 0.004, "Nested rectangle 2") >= 0
		IG._pump_once()
		r = nestrects(f.h)
		@test length(r) == 2
		@test r[1].w == lvl1.w
		@test r[1].e == lvl1.e
		@test r[1].s == lvl1.s
		@test r[1].n == lvl1.n
		lvl2 = r[2]

		# The child obeys the rule against its parent, and lies strictly INSIDE it.
		@test on_parent_node(lvl2.w,  lvl1.xi / 2 - lvl2.xi / 2, lvl1.w, lvl1.xi)
		@test on_parent_node(lvl2.e, -lvl1.xi / 2 + lvl2.xi / 2, lvl1.w, lvl1.xi)
		@test on_parent_node(lvl2.s,  lvl1.yi / 2 - lvl2.yi / 2, lvl1.s, lvl1.yi)
		@test on_parent_node(lvl2.n, -lvl1.yi / 2 + lvl2.yi / 2, lvl1.s, lvl1.yi)
		@test lvl1.w < lvl2.w && lvl2.e < lvl1.e && lvl1.s < lvl2.s && lvl2.n < lvl1.n

		# ── and again ── a GRANDCHILD moves neither of the two rectangles above it.
		@test addrect(f.h, 4.47, 5.53, 4.41, 5.62, 0.001, 0.001, "Nested rectangle 3") >= 0
		IG._pump_once()
		r = nestrects(f.h)
		@test length(r) == 3
		@test (r[1].w, r[1].e, r[1].s, r[1].n) == (lvl1.w, lvl1.e, lvl1.s, lvl1.n)
		@test (r[2].w, r[2].e, r[2].s, r[2].n) == (lvl2.w, lvl2.e, lvl2.s, lvl2.n)
		lvl3 = r[3]
		@test lvl2.w < lvl3.w && lvl3.e < lvl2.e && lvl2.s < lvl3.s && lvl3.n < lvl2.n

		# ── the growth signature ── a fourth level reflows the chain a fourth time. With outward
		# rounding every rectangle above would by now have grown one parent cell PER reflow; nothing
		# above the new one may have moved by so much as a float bit.
		@test addrect(f.h, 4.87, 5.13, 4.88, 5.11, 0.0002, 0.0002, "Nested rectangle 4") >= 0
		IG._pump_once()
		r = nestrects(f.h)
		@test length(r) == 4
		@test (r[1].w, r[1].e, r[1].s, r[1].n) == (lvl1.w, lvl1.e, lvl1.s, lvl1.n)
		@test (r[2].w, r[2].e, r[2].s, r[2].n) == (lvl2.w, lvl2.e, lvl2.s, lvl2.n)
		@test (r[3].w, r[3].e, r[3].s, r[3].n) == (lvl3.w, lvl3.e, lvl3.s, lvl3.n)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# ABSOLUTE VALUES. Everything above is self-consistent: limits are compared against other limits read
# back from the same scene, so a rule that is uniformly WRONG but consistently so still satisfies it.
# This item pins four hand-computed numbers instead, derived from the rule on paper:
#
#   base grid x0 = 0, inc = 0.1 (101 nodes).  Rect A requested 2.34 … 7.71 with child inc 0.02.
#     nearest node to 2.34 -> (2.34-0)/0.1 = 23.4 -> node 23 = 2.30
#     west = 2.30 - 0.1/2 + 0.02/2 = 2.26        (half a PARENT cell out, half a CHILD cell in)
#     nearest to 7.71 -> 77.1 -> node 77 = 7.70;  east = 7.70 + 0.05 - 0.01 = 7.74
#     nearest to 2.16 -> 21.6 -> node 22 = 2.20;  south = 2.20 - 0.05 + 0.01 = 2.16
#     nearest to 7.63 -> 76.3 -> node 76 = 7.60;  north = 7.60 + 0.05 - 0.01 = 7.64
#
#   Rect B, child of A, requested 3.534 … 6.417 with child inc 0.004, parent inc 0.02 from 2.26:
#     (3.534-2.26)/0.02 = 63.70 -> node 64 = 3.54;  west  = 3.54 - 0.01 + 0.002 = 3.532
#     (6.417-2.26)/0.02 = 207.85 -> node 208 = 6.42; east = 6.42 + 0.01 - 0.002 = 6.428
#     (3.616-2.16)/0.02 = 72.80 -> node 73 = 3.62;  south = 3.62 - 0.01 + 0.002 = 3.612
#     (6.383-2.16)/0.02 = 211.15 -> node 211 = 6.38; north = 6.38 + 0.01 - 0.002 = 6.388
#
# Every requested edge above is deliberately clear of a half-node (….4, ….1, ….6, ….3, ….70, ….85,
# ….80, ….15), so no assertion here rests on how a tie rounds — a tie is a separate question from
# the rule, and mixing the two would make this item flaky rather than strict.
@testitem "nested grids: the snapped limits are the hand-computed ones" tags=[:gui, :nested] begin
	IG = InteractiveGMT; GMT = IG.GMT

	nestrects(h) = begin
		out = NamedTuple[]
		for line in split(IG._serialize_faults_raw(h), '\n'; keepempty=false)
			startswith(line, "N;") || continue
			p = split(line, ';'; limit=6)
			length(p) < 6 && continue
			xy, nv = IG._parse_xy2(p[6])
			nv < 2 && continue
			xs = @view xy[1:2:end];  ys = @view xy[2:2:end]
			push!(out, (w = minimum(xs), e = maximum(xs), s = minimum(ys), n = maximum(ys)))
		end
		return out
	end

	addrect(h, x0, x1, y0, y1, xi, yi, name) = begin
		xy = [x0, y0,  x1, y0,  x1, y1,  x0, y1,  x0, y0]
		ccall(IG._fn(:gmtvtk_add_nested_rect), Cint,
		      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cint, Cstring),
		      h, xy, Cint(5), xi, yi, Cint(0), name)
	end

	G = GMT.mat2grid(zeros(Float32, 101, 101); x = [0.0, 10.0], y = [0.0, 10.0])
	f = view_grid(G)
	try
		IG._pump_once()
		@test addrect(f.h, 2.34, 7.71, 2.16, 7.63, 0.02, 0.02, "Nested rectangle 1") >= 0
		IG._pump_once()
		r = nestrects(f.h)
		@test length(r) == 1
		@test r[1].w ≈ 2.26 atol=1e-9
		@test r[1].e ≈ 7.74 atol=1e-9
		@test r[1].s ≈ 2.16 atol=1e-9
		@test r[1].n ≈ 7.64 atol=1e-9

		@test addrect(f.h, 3.534, 6.417, 3.616, 6.383, 0.004, 0.004, "Nested rectangle 2") >= 0
		IG._pump_once()
		r = nestrects(f.h)
		@test length(r) == 2
		# the parent is still exactly where the paper says, after a second reflow
		@test r[1].w ≈ 2.26 atol=1e-9
		@test r[1].e ≈ 7.74 atol=1e-9
		@test r[1].s ≈ 2.16 atol=1e-9
		@test r[1].n ≈ 7.64 atol=1e-9
		@test r[2].w ≈ 3.532 atol=1e-9
		@test r[2].e ≈ 6.428 atol=1e-9
		@test r[2].s ≈ 3.612 atol=1e-9
		@test r[2].n ≈ 6.388 atol=1e-9
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# KNOWN GAP, stated rather than faked: the law this file enforces names "Editing, adding or
# restoring". Adding is covered by the first item, restoring by the third. EDITING — dragging a
# rectangle's vertex or moving it, which reaches nestReflow through 55_lineprops.cpp's drag-release
# — is NOT covered, because no exported entry point moves an existing nested rect; only a synthetic
# mouse drag or a new test hook can reach it. Do not paper this over with a re-add: re-adding is a
# different code path and would report cover this file does not have.

# The same law along the OTHER axis this bug travelled: a rectangle that already obeys the rule must
# re-derive to ITSELF. This is what a session restore does (gmtvtk_add_nested_rect replays saved,
# already-snapped verts and reflows), and it is the property that makes walking the whole chain safe.
# It had been papered over with a nestReflow(snap=false) mode that skipped the snap on restore — a
# second implementation of the same operation, which is exactly what let the real defect survive.
@testitem "nested grids: re-snapping a settled rectangle is a no-op" tags=[:gui, :nested] begin
	IG = InteractiveGMT; GMT = IG.GMT

	nestrects(h) = begin
		out = NamedTuple[]
		for line in split(IG._serialize_faults_raw(h), '\n'; keepempty=false)
			startswith(line, "N;") || continue
			p = split(line, ';'; limit=6)
			length(p) < 6 && continue
			xy, nv = IG._parse_xy2(p[6])
			nv < 2 && continue
			xs = @view xy[1:2:end];  ys = @view xy[2:2:end]
			push!(out, (xi = parse(Float64, p[2]), yi = parse(Float64, p[3]),
			            w = minimum(xs), e = maximum(xs), s = minimum(ys), n = maximum(ys)))
		end
		return out
	end

	addrect(h, x0, x1, y0, y1, xi, yi, name) = begin
		xy = [x0, y0,  x1, y0,  x1, y1,  x0, y1,  x0, y0]
		ccall(IG._fn(:gmtvtk_add_nested_rect), Cint,
		      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cint, Cstring),
		      h, xy, Cint(5), xi, yi, Cint(0), name)
	end

	G = GMT.mat2grid(zeros(Float32, 101, 101); x = [0.0, 10.0], y = [0.0, 10.0])

	# Window A: draw the chain from raw, off-node boxes and let it settle.
	fa = view_grid(G)
	settled = try
		IG._pump_once()
		addrect(fa.h, 2.34, 7.71, 2.16, 7.63, 0.02,  0.02,  "Nested rectangle 1")
		addrect(fa.h, 3.53, 6.42, 3.61, 6.38, 0.004, 0.004, "Nested rectangle 2")
		IG._pump_once()
		nestrects(fa.h)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fa.h)
	end
	@test length(settled) == 2

	# Window B: replay the SETTLED limits, the way a session restore does. They must come back
	# unchanged — each rect's edges are less than half a parent cell off a parent node, so nearest
	# lands on the same node it came from.
	fb = view_grid(G)
	try
		IG._pump_once()
		for (i, q) in enumerate(settled)
			addrect(fb.h, q.w, q.e, q.s, q.n, q.xi, q.yi, "Nested rectangle $i")
		end
		IG._pump_once()
		replayed = nestrects(fb.h)
		@test length(replayed) == 2
		for (q, p) in zip(settled, replayed)
			@test p.w == q.w
			@test p.e == q.e
			@test p.s == q.s
			@test p.n == q.n
		end
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fb.h)
	end
end
