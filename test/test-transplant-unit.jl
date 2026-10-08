# CI-safe unit tests for the grid-transplant math (transplant.jl). These call the pure helpers
# directly — no Qt+VTK window — so they run anywhere `using InteractiveGMT` succeeds. The GUI wiring
# (context menu, base-vs-extra replace, Ctrl+Z) is C++ and exercised by the :gui scenarios.
#
# What they lock down:
#   * `_transplant_grid` (Mirone transplants.m IMPLANTGRID) returns a NEW grid and never writes the
#     host (SACRED_LAW.md derived-variable display law): the host z is identical afterwards. This
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

# `_transplant_grid` is a port of Mirone's transplants.m IMPLANTGRID mode (2026-10-08, user order:
# "do exactly what Mirone does"). Each test pins one branch of Mirone's dispatch.

# NaNs in the host, none in the implant -> job_NanHost_noNanImp: the implant is sampled at the host's
# NaN nodes and the WHOLE host is regridded (r_c = the whole grid).
@testitem "transplant: host NaNs -> implant sampled into them, whole host regridded" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:1.0:10.0)
	H  = GMT.mat2grid(Float32[ (4 <= x <= 6 && 4 <= y <= 6) ? NaN32 : x + 2y for y in xs, x in xs ]; x = xs, y = xs)
	xi = collect(-1.0:0.5:11.0)
	I  = GMT.mat2grid(Float32[ x + 2y for y in xi, x in xi ]; x = xi, y = xi)

	Horig = copy(H.z)
	Gout, blk = IG._transplant_grid(H, I; keepres = true)

	@test isequal(H.z, Horig)                                        # host never written
	@test (blk.r0, blk.r1, blk.c0, blk.c1) == (1, 11, 1, 11)        # Mirone: r_c = whole host
	@test isempty(blk.warn)
	@test !any(isnan, Gout.z)                                        # the hole is filled
	@test isapprox(Gout.z[6, 6], 15.0; atol = 0.5)                   # (5,5) on the shared plane x+2y
	@test Gout.z !== H.z
end

# No NaNs anywhere -> job_no_nans: host block = implant BB + 6 cells, regridded from the host skirt and
# the implant nodes.
@testitem "transplant: no NaNs -> implant BB + pad block regridded, host untouched" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:1.0:20.0)
	H  = GMT.mat2grid(Float32[ x + 2y for y in xs, x in xs ]; x = xs, y = xs)
	xi = collect(8.0:0.5:12.0)
	I  = GMT.mat2grid(fill(100.0f0, length(xi), length(xi)); x = xi, y = xi)

	Horig = copy(H.z)
	Gout, blk = IG._transplant_grid(H, I; keepres = true)

	@test H.z == Horig                                               # host never written
	@test (blk.r0, blk.r1, blk.c0, blk.c1) == (3, 19, 3, 19)        # 8-6 .. 12+6 -> nodes 2..18
	@test Gout.z[blk.r0:blk.r1, blk.c0:blk.c1] != Horig[blk.r0:blk.r1, blk.c0:blk.c1]
	@test Gout.z[1, 1] == Horig[1, 1]                                # outside the block: untouched
	@test isapprox(Gout.z[11, 11], 100.0; atol = 1.0)                # implant centre (10,10)
end

# NaNs in both -> Mirone's warning, then job_NanHost_noNanImp.
@testitem "transplant: NaNs in both -> Mirone's warning" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:1.0:10.0)
	H  = GMT.mat2grid(Float32[ (4 <= x <= 6 && 4 <= y <= 6) ? NaN32 : x + 2y for y in xs, x in xs ]; x = xs, y = xs)
	xi = collect(-1.0:0.5:11.0)
	I  = GMT.mat2grid(Float32[ (x == 11 && y == 11) ? NaN32 : x + 2y for y in xi, x in xi ]; x = xi, y = xi)
	Gout, blk = IG._transplant_grid(H, I; keepres = true)
	@test occursin("not yet implemented", blk.warn)
end

@testitem "transplant: adopt-implant-resolution resamples the host, host untouched" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:0.5:10.0)
	H  = GMT.mat2grid(Float32[ (4 <= x <= 6 && 4 <= y <= 6) ? NaN32 : x + 2y for y in xs, x in xs ]; x = xs, y = xs)
	xi = collect(-1.0:1.0:11.0)
	I  = GMT.mat2grid(Float32[ x + 2y for y in xi, x in xi ]; x = xi, y = xi)

	Horig = copy(H.z)
	Gout, blk = IG._transplant_grid(H, I; keepres = false)

	@test (blk.r0, blk.c0) == (1, 1)
	@test isequal(H.z, Horig)                # original host object is left pristine
	@test isapprox(Gout.inc[1], I.inc[1]; atol = 1e-9)   # output adopted the implant increment
	@test isapprox(Gout.inc[2], I.inc[2]; atol = 1e-9)
	@test !any(isnan, Gout.z)
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

# 2026-10-08: a host + implant read the way the app reads them ("TRB", row-major, north first) were
# indexed as column-major south-first — each implant z went to another node's x/y and the result was
# written into the wrong nodes. The SAME files read "BCB" and "TRB" must give the SAME transplant.
@testitem "transplant: TRB grids give the same result as BCB" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	xs = collect(0.0:1.0:12.0);  ys = collect(0.0:1.0:8.0)
	Hm = GMT.mat2grid(Float32[ (5 <= x <= 8 && 3 <= y <= 4) ? NaN32 : x + 10y for y in ys, x in xs ]; x = xs, y = ys)
	xi = collect(-1.0:0.5:13.0);  yi = collect(-1.0:0.5:9.0)
	Im = GMT.mat2grid(Float32[ 100 + x - 3y for y in yi, x in xi ]; x = xi, y = yi)
	fh = tempname() * ".grd";  fi = tempname() * ".grd"
	try
		GMT.gmtwrite(fh, Hm);  GMT.gmtwrite(fi, Im)
		Hb = GMT.gmtread(fh);  Ib = GMT.gmtread(fi)
		Ht = IG._gmtread_trb(fh);  It = IG._gmtread_trb(fi)
		@test startswith(Ht.layout, "TR")
		Horig = copy(IG._zmat(Ht));  ilay = It.layout
		Gb, _   = IG._transplant_grid(Hb, Ib; keepres = true)
		Gt, blk = IG._transplant_grid(Ht, It; keepres = true)
		@test Gt.layout == Ht.layout
		@test It.layout == ilay                 # grdtrack must not leave the implant re-labelled
		@test IG._zmat(Gt) == IG._zmat(Gb)
		@test isequal(IG._zmat(Ht), Horig)       # host never written
		# adopt mode must not re-label the source grid (GMT.grdsample does, to its input)
		Ht = IG._gmtread_trb(fh);  lay = Ht.layout     # fresh: the keepres run above wrote Ht in place
		IG._transplant_grid(Ht, It; keepres = false)
		@test Ht.layout == lay
	finally
		rm(fh; force = true);  rm(fi; force = true)
	end
end
