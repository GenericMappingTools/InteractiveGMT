# CI-safe unit tests for the grid-transplant math (transplant.jl). These call the pure helpers
# directly — no Qt+VTK window — so they run anywhere `using InteractiveGMT` succeeds. The GUI wiring
# (context menu, base-vs-extra replace, Ctrl+Z) is C++ and exercised by the :gui scenarios.
#
# What they lock down:
#   * `_transplant_grid` mutates the host z IN PLACE (no whole-grid copy) yet the undo snapshot
#     `blk.oz` still holds the ORIGINAL pre-paste values, so an undo restores the host exactly. This
#     guards the "why copy?" fix: the block must be snapshotted BEFORE the in-place paste, and the
#     `-R$(tx[end])` interpolation must be well-formed (a malformed region string makes GMT surface
#     throw, so a passing run is itself the regression guard).
#   * `_nested_fill!` samples the implant onto the blank grid's own nodes and writes them in place,
#     leaving uncovered nodes at their blank value — no copy, no fresh GMTgrid.

@testitem "transplant helpers present" tags=[:unit, :fast] begin
	for s in (:_transplant_grid, :_nested_fill!, :_grid_xy, :_on_transplant,
	          :_on_nested_transplant, :_on_transplant_undo)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "transplant: keepres mutates host in place, undo snapshot is original" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# Host: 11x11 gridline grid over 0..10 with structure so `surface` has something to fit.
	xs = collect(0.0:1.0:10.0)
	H  = GMT.mat2grid(Float32[ x + 2y for y in xs, x in xs ]; x = xs, y = xs)
	# Implant: constant 100 over the 3..7 subregion at finer 0.5 spacing.
	xi = collect(3.0:0.5:7.0)
	I  = GMT.mat2grid(fill(100.0f0, length(xi), length(xi)); x = xi, y = xi)

	Horig = copy(H.z)
	Gout, blk = IG._transplant_grid(H, I; keepres = true)

	@test blk.samegeom == true              # host geometry untouched -> block-level undo
	@test blk.oz !== nothing
	@test size(blk.oz) == (blk.r1 - blk.r0 + 1, blk.c1 - blk.c0 + 1)
	# Snapshot holds the ORIGINAL values (taken before the in-place paste), not the transplanted ones.
	@test blk.oz == Horig[blk.r0:blk.r1, blk.c0:blk.c1]
	# Host z really was mutated in place over the block (implant ~100 differs from x+2y there).
	@test H.z[blk.r0:blk.r1, blk.c0:blk.c1] != Horig[blk.r0:blk.r1, blk.c0:blk.c1]
	# The window still shows the mutated host (Gout wraps the same modified z).
	@test Gout.z[blk.r0:blk.r1, blk.c0:blk.c1] == H.z[blk.r0:blk.r1, blk.c0:blk.c1]
	# END-TO-END undo: paste the snapshot back over the changed block -> host identical to original.
	Z = copy(H.z);  Z[blk.r0:blk.r1, blk.c0:blk.c1] .= blk.oz
	@test Z == Horig
end

@testitem "transplant: adopt-implant-resolution leaves host untouched (whole-grid undo)" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:1.0:10.0)
	H  = GMT.mat2grid(Float32[ x + 2y for y in xs, x in xs ]; x = xs, y = xs)
	xi = collect(3.0:0.5:7.0)
	I  = GMT.mat2grid(fill(100.0f0, length(xi), length(xi)); x = xi, y = xi)

	Horig = copy(H.z)
	Gout, blk = IG._transplant_grid(H, I; keepres = false)

	@test blk.samegeom == false             # host was resampled -> whole-grid undo, no block snapshot
	@test blk.oz === nothing
	@test H.z == Horig                       # original host object is left pristine
	@test isapprox(Gout.inc[1], I.inc[1]; atol = 1e-9)   # output adopted the implant increment
	@test isapprox(Gout.inc[2], I.inc[2]; atol = 1e-9)
end

@testitem "nested fill: samples implant in place, blank nodes preserved" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:1.0:10.0)
	# Blank ("layerN") host: every node NaN, gridline, Float32.
	Gb = GMT.mat2grid(fill(NaN32, length(xs), length(xs)); x = xs, y = xs)
	xi = collect(3.0:0.5:7.0)
	I  = GMT.mat2grid(fill(100.0f0, length(xi), length(xi)); x = xi, y = xi)

	zref = Gb.z                              # same object -> proves in-place (no fresh array)
	blk  = IG._nested_fill!(Gb, I)
	@test Gb.z === zref                      # filled in place, not replaced (already Float32)
	# Covered block took the implant's constant 100; outside nodes kept their blank NaN.
	@test all(≈(100.0f0), Gb.z[blk.r0:blk.r1, blk.c0:blk.c1])
	@test isnan(Gb.z[1, 1])                  # a corner well outside 3..7 stays blank
	@test count(isnan, Gb.z) > 0             # blank ring survives

	# No overlap -> a clear error (the GUI catch turns this into a viewer log line).
	Ifar = GMT.mat2grid(fill(1.0f0, 3, 3); x = collect(50.0:1.0:52.0), y = collect(50.0:1.0:52.0))
	@test_throws ErrorException IG._nested_fill!(Gb, Ifar)
end
