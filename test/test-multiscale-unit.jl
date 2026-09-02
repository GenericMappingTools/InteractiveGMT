# CI-safe unit tests for Grid Tools > Terrain Modeling (multiscale.jl, port of Mirone's mirblock.c
# MEX behind src_figs/multiscale.m). Pure Julia, no Qt window: the dialog is C++, everything the
# dialog drives is here.
#
# The window statistics are checked on grids whose windows can be written out by hand, and the
# plane-fit family on planes (where the fit is exact, so Slope/Aspect/Trend/Residue all have closed
# forms). Borders are compared only where the mirrored padding does not change the answer — the
# padding itself is checked separately.

@testitem "multiscale: helpers present, method list is mirblock's -A order" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for s in (:_mb_reflect, :_mb_pad, :_mb_stats, :_mb_solve3, :_mb_surface_fit, :_mb_cap,
	          :_mb_maxfinite, :mirblock, :MIRBLOCK_METHODS, :_on_multiscale)
		@test isdefined(IG, s)
	end
	# multiscale.m warns in capitals that the popup order must match the MEX. deps/ui/multiscale.ui
	# repeats this list, so only the index travels over the eval bridge.
	@test length(IG.MIRBLOCK_METHODS) == 14
	@test IG.MIRBLOCK_METHODS[1]  == "Terrain Ruggedness Index"    # -A0
	@test IG.MIRBLOCK_METHODS[7]  == "Slope"                       # -A6
	@test IG.MIRBLOCK_METHODS[8]  == "Aspect"                      # -A7
	@test IG.MIRBLOCK_METHODS[14] == "AGC (Local Amp)"             # -A13
end

@testitem "multiscale: mirrored padding does not repeat the edge" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test [IG._mb_reflect(k, 5) for k in -1:7] == [3, 2, 1, 2, 3, 4, 5, 4, 3]
	z = Float32[10 20 30; 40 50 60; 70 80 90]
	P = IG._mb_pad(z, 1)
	@test size(P) == (5, 5)
	@test P[2:4, 2:4] == z                                   # the original sits in the middle
	@test P[1, 2:4] == z[2, :]                               # row 0 mirrors row 2, not row 1
	@test P[5, 2:4] == z[2, :]
	@test P[2:4, 1] == z[:, 2]
	@test P[1, 1] == z[2, 2]                                 # the corner follows from both
	@test_throws ErrorException IG._mb_pad(z, 3)             # window wider than the grid
end

@testitem "multiscale: bad method / window are refused" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[i + j for i = 1:5, j = 1:5]; x = collect(1.0:5), y = collect(1.0:5))
	@test_throws ErrorException IG.mirblock(G; method = 14)
	@test_throws ErrorException IG.mirblock(G; method = -1)
	@test_throws ErrorException IG.mirblock(G; win = 4)      # must be odd
	@test_throws ErrorException IG.mirblock(G; win = 1)      # and at least 3
end

@testitem "multiscale: the window statistics on a hand-computable grid" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# z = the row number, so every interior 3x3 window holds {c-1, c, c+1}, three of each.
	x = collect(1.0:7);  y = collect(1.0:7)
	G = GMT.mat2grid(Float32[i for i = 1:7, _ = 1:7]; x = x, y = y)
	k = 2:6                                                  # interior: no mirrored cell in the window
	m(id) = IG.mirblock(G; method = id).z[k, k]
	@test all(≈(6 / 9; atol = 1e-6), m(0))                   # TRI  = mean|c-v| = (3*1 + 3*0 + 3*1)/9
	@test all(≈(0.0;   atol = 1e-6), m(1))                   # TPI  = c - mean(window) = 0
	@test m(2) == fill(2.0f0, 5, 5)                          # Roughness = max - min
	@test m(3) ≈ Float32[i for i = 2:6, _ = 2:6]             # Mean
	@test m(4) ≈ Float32[i - 1 for i = 2:6, _ = 2:6]         # Min
	@test m(5) ≈ Float32[i + 1 for i = 2:6, _ = 2:6]         # Max
	@test all(≈(sqrt(6 / 9); atol = 1e-6), m(8))             # RMS = sqrt(<z^2> - <z>^2)
end

@testitem "multiscale: a constant grid is flat by every measure" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(fill(7.5f0, 9, 9); x = collect(1.0:9), y = collect(1.0:9))
	for id in (0, 1, 2, 6, 8, 10, 11)                        # TRI TPI Rough Slope RMS Residue ResRMS
		@test all(≈(0.0; atol = 1e-5), IG.mirblock(G; method = id).z)
	end
	for id in (3, 4, 5, 9)                                   # Mean Min Max Trend
		@test all(≈(7.5; atol = 1e-5), IG.mirblock(G; method = id).z)
	end
end

@testitem "multiscale: the plane-fit family is exact on a plane" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# A Cartesian plane z = 3x + 2y on a grid whose increments are NOT 1, so the physical scaling of
	# the gradient is actually exercised.
	x = collect(0.0:0.5:6.0);  y = collect(0.0:0.25:3.0)
	Z = Float32[3 * xx + 2 * yy for yy in y, xx in x]
	G = GMT.mat2grid(Z; x = x, y = y)
	ki = 3:(length(y) - 2);  kj = 3:(length(x) - 2)          # interior (win = 5 below needs 2)
	@test IG.mirblock(G; method = 9).z[ki, kj] ≈ Z[ki, kj]  atol=1e-3     # Trend = the plane itself
	@test all(≈(0.0; atol = 1e-3), IG.mirblock(G; method = 10).z[ki, kj]) # Residue
	@test all(≈(0.0; atol = 1e-3), IG.mirblock(G; method = 11).z[ki, kj]) # RMS of Residue
	# Slope of that plane: atan(|grad|) in degrees, whatever the window size.
	want = atand(hypot(3.0, 2.0))
	for w in (3, 5)
		S = IG.mirblock(G; method = 6, win = w).z
		@test all(≈(want; atol = 1e-3), S[3:(end - 2), 3:(end - 2)])
	end
end

@testitem "multiscale: Aspect points down the slope, mirblock's convention" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:1.0:8.0);  y = collect(0.0:1.0:8.0)
	asp(f) = IG.mirblock(GMT.mat2grid(Float32[f(xx, yy) for yy in y, xx in x]; x = x, y = y);
	                     method = 7).z[3:(end - 2), 3:(end - 2)]
	# aspect = -(90 + atan2d(dz/dy, dz/dx)), wrapped into [0,360). Ground truth, worked through for
	# the four cardinal ramps:
	@test all(≈(270.0; atol = 1e-3), asp((a, b) ->  a))      # rising east  -> 270
	@test all(≈(90.0;  atol = 1e-3), asp((a, b) -> -a))      # rising west  -> 90
	@test all(≈(180.0; atol = 1e-3), asp((a, b) ->  b))      # rising north -> 180
	@test all(≈(0.0;   atol = 1e-3), asp((a, b) -> -b))      # rising south -> 0
end

@testitem "multiscale: Slope converts degrees to metres for a geographic grid" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# 1000 m of rise per degree of longitude, at latitude 60 where a degree of longitude is half as
	# long. Cartesian reading: atan(1000) ~ 89.94 deg. Geographic: the run becomes
	# 111195.01524 * cos(60) metres per degree, so the gradient is 1000/(111195.01524*0.5).
	x = collect(0.0:0.25:4.0);  y = collect(59.0:0.25:61.0)
	G = GMT.mat2grid(Float32[1000 * xx for _ in y, xx in x]; x = x, y = y)
	Sc = IG.mirblock(G; method = 6, geog = false).z
	Sg = IG.mirblock(G; method = 6, geog = true).z
	@test all(≈(atand(1000.0); atol = 1e-3), Sc[3:(end - 2), 3:(end - 2)])
	# Every row must match its OWN latitude — the .c builds a cos(lat) table per window-centre row.
	for lat in (59.5, 60.0, 60.5)
		i = findfirst(≈(lat), y)
		@test Sg[i, 5] ≈ atand(1000 / (IG._MB_M_PER_DEG * cosd(lat)))  atol=1e-3
	end
end

@testitem "multiscale: NaNs are skipped inside a window, fatal at its centre" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	Z = Float32[i for i = 1:7, _ = 1:7]
	Z[4, 4] = NaN32                                          # one hole
	G = GMT.mat2grid(Z; x = collect(1.0:7), y = collect(1.0:7))
	M = IG.mirblock(G; method = 3).z                         # Mean
	@test isnan(M[4, 4])                                     # NaN centre -> NaN out, window unread
	# Its neighbour's window holds 8 good cells: rows 2,3,4 (3+3+2 cells) around centre row 3.
	@test M[3, 4] ≈ Float32((3 * 2 + 3 * 3 + 2 * 4) / 8)     # = 23/8, the hole simply not counted
	@test count(isnan, M) == 1                               # ...and no NaN spreading anywhere else
	@test all(isfinite, IG.mirblock(G; method = 4).z[[1, 7], :])   # Min: borders stay finite
end

@testitem "multiscale: AGC pushes every node's local RMS towards the largest one" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:1.0:15.0);  y = collect(0.0:1.0:15.0)
	# Amplitude grows with x, so the local RMS does too and the gain must fall towards 1 eastwards.
	Z = Float32[(1 + xx) * sinpi(yy / 2) for yy in y, xx in x]
	G = GMT.mat2grid(Z; x = x, y = y)
	A = IG.mirblock(G; method = 12).z                        # AGC (Full Amp)
	rms = IG.mirblock(G; method = 8).z
	gain = abs.(A) ./ max.(abs.(Z), 1.0f-6)
	ok = abs.(Z) .> 1.0f-3
	@test all(gain[ok] .<= 10.0f0 + 1.0f-4)                  # the .c caps amplification at 10
	@test all(gain[ok] .>= 1.0f0 - 1.0f-4)                   # ...and never attenuates
	# At the node with the largest local RMS the factor is rmsMax/rms = 1, so it is passed through
	# untouched (checked on the values, not the ratio: that node's own z may be ~0).
	@test A[argmax(rms)] ≈ Z[argmax(rms)]  atol=1e-4
	@test IG.mirblock(G; method = 13) isa GMT.GMTgrid        # AGC (Local Amp) runs the same path
end

@testitem "multiscale: the result keeps the grid's geometry" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.5:6.0);  y = collect(0.0:0.25:3.0)
	G = GMT.mat2grid(Float32[xx * yy for yy in y, xx in x]; x = x, y = y)
	for id in 0:13
		R = IG.mirblock(G; method = id)
		@test R isa GMT.GMTgrid
		@test size(R.z) == size(G.z)
		@test R.range[1:4] ≈ G.range[1:4]
		@test R.inc ≈ G.inc
		@test !any(isinf, R.z)                               # mirone.m turns every Inf into a NaN
	end
end
