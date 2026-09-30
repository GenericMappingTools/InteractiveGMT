# LOAD ORDER MAY NOT CHANGE WHAT A GRID LOOKS LIKE — the regression suite for 2026-09-26.
#
# What broke: the first grid opened into a window is its BASE (a zoom pyramid of makeGridTile tiles, a
# NaN backdrop); every later grid was an EXTRA built by gmtvtk_add_surface_h with a different mesh
# builder, no NaN backdrop and no zoom refinement. So the same SST file opened after layer0.grd lost
# data at its cloud edges, showed the window background through its holes, was lit differently and
# stayed blurred on zoom — and, opened FIRST, vanished when layer0 was unchecked (the depth range was
# still fitted to layer0's flatter relief).
#
# The rule under test, measured on the PIXELS the window shows (never the shape of the code):
#   * a grid with NaNs looks the same opened alone, opened second, and checked back on after the grid
#     opened after it is unchecked — at the full view AND zoomed in (the pyramid refines);
#   * its holes wear the NaN fill colour in every case.
# "The same" allows one-pixel seams: the base draws one actor per tile, so its tile edges rasterise a
# hair differently from a layer drawn as one mesh. Anything that changes the shading, the mesh or the
# resolution moves far more pixels than that.

@testmodule GridOrderFixture begin
	using InteractiveGMT
	const IG = InteractiveGMT
	const GMT = IG.GMT

	pump(n::Int = 25) = for _ in 1:n; IG._pump_once(); sleep(0.02); end

	# A grid with holes and relief, like a daily SST: smooth field, ~40% NaN in blobs, geographic.
	function holey(; nx = 240, ny = 120, x = (100.0, 130.0), y = (-10.0, 5.0))
		xs = collect(range(x[1], x[2], length = nx)); ys = collect(range(y[1], y[2], length = ny))
		z = Matrix{Float32}(undef, ny, nx)
		for ix in 1:nx, iy in 1:ny
			u = xs[ix]; v = ys[iy]
			f = 20 + 6 * sin(0.9 * u) * cos(1.3 * v) + 2 * sin(3.1 * u + 2.3 * v)
			hole = sin(0.7 * u + 0.4 * v) * cos(0.5 * u - 0.9 * v) > 0.35
			z[iy, ix] = hole ? NaN32 : Float32(f)
		end
		GMT.mat2grid(z, x = xs, y = ys)
	end
	# A grid WITHOUT holes and with a z range in the thousands (metres), like layer0.grd: its drawn
	# relief is far flatter than the SST's, which is what left the depth range too narrow.
	function relief(; n = 80, x = (110.0, 118.0), y = (-3.0, 3.0))
		xs = collect(range(x[1], x[2], length = n)); ys = collect(range(y[1], y[2], length = n))
		z = [Float32(-5000 + 8000 * (0.5 + 0.5 * sin(0.8 * (xi - x[1])) * cos(0.9 * (yi - y[1]))))
		     for yi in ys, xi in xs]
		GMT.mat2grid(z, x = xs, y = ys)
	end

	# The File > Open door: add the grid (promoting an empty window) and adopt it as what is shown.
	function open!(h, G, name; promote::Bool)
		IG._drop_into(h, G, name; promote = promote, source = "")
		IG._adopt_new_element(h, name, G)
		pump(30)
	end

	reframe!(h, x0, x1, y0, y1) = (ccall(IG._fn(:gmtvtk_reframe_h), Cvoid,
	                                     (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cint),
	                                     h, x0, x1, y0, y1, Cint(0)); pump(40))

	# The window as displayed, (band, col, row), RGBA.
	function capture(h)
		p = Ref{Ptr{UInt8}}(C_NULL); w = Ref{Cint}(0); hh = Ref{Cint}(0)
		ok = ccall(IG._fn(:gmtvtk_capture_view_rgba), Cint,
		           (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}), h, p, w, hh)
		ok == 1 || error("view capture failed")
		A = copy(unsafe_wrap(Array, p[], (4, Int(w[]), Int(hh[]))))
		ccall(IG._fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), p[])
		A
	end
	# Share of the map area (the colour bar strip at the right left out) where two captures differ.
	function diff_frac(A, B; tol = 24)
		size(A) == size(B) || return 1.0
		n = 0; t = 0
		for y in 1:size(A, 3), x in 1:floor(Int, 0.9 * size(A, 2))
			t += 1
			d = max(abs(Int(A[1, x, y]) - Int(B[1, x, y])), abs(Int(A[2, x, y]) - Int(B[2, x, y])),
			        abs(Int(A[3, x, y]) - Int(B[3, x, y])))
			d > tol && (n += 1)
		end
		n / max(t, 1)
	end
	# Share of the map area in the NaN fill colour (near-white: the Preferences default).
	function white_frac(A)
		n = 0; t = 0
		for y in 1:size(A, 3), x in 1:floor(Int, 0.9 * size(A, 2))
			t += 1
			(A[1, x, y] > 245 && A[2, x, y] > 245 && A[3, x, y] > 245) && (n += 1)
		end
		n / max(t, 1)
	end
	# Share of the map area that is coloured data (neither near-white nor the dark window background).
	function data_frac(A)
		n = 0; t = 0
		for y in 1:size(A, 3), x in 1:floor(Int, 0.9 * size(A, 2))
			t += 1
			r, g, b = Int(A[1, x, y]), Int(A[2, x, y]), Int(A[3, x, y])
			white = r > 245 && g > 245 && b > 245
			(!white && max(r, g, b) - min(r, g, b) > 30) && (n += 1)
		end
		n / max(t, 1)
	end

	empty_window() = (h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "grid order"); pump(10); h)
	close!(h) = (ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h); pump(5))
end

@testitem "Grid order: a grid looks the same opened alone, opened second, and re-checked" tags=[:gui, :nan] setup=[GmtvtkTest, GridOrderFixture] begin
	F = GridOrderFixture
	click(h, path) = ccall(GmtvtkTest._test_fn(:gmtvtk_objrow_click_test), Cint, (Ptr{Cvoid}, Cstring), h, path)
	S = F.holey(); L = F.relief()
	full = (100.0, 130.0, -10.0, 5.0)          # the holey grid's own frame
	zoom = (112.0, 114.0, -2.0, 0.0)           # deep enough that the pyramid has to refine

	hR = F.empty_window(); hA = F.empty_window(); hB = F.empty_window()
	# Same view size in all three, or the captures are not comparable (a window cascaded near the edge
	# of the screen opens smaller than the others).
	pin(h) = ccall(GmtvtkTest._test_fn(:gmtvtk_view_fixed_size_test), Cint, (Ptr{Cvoid}, Cint, Cint), h, 800, 600)
	for h in (hR, hA, hB); @test pin(h) == 1; end
	try
		# R: the holey grid ALONE — the reference.
		F.open!(hR, S, "sst"; promote = true)
		# A: the relief grid FIRST, the holey grid SECOND (it is an extra).
		F.open!(hA, L, "layer0"; promote = true)
		F.open!(hA, S, "sst"; promote = false)
		# B: the holey grid FIRST, the relief grid second; then check the holey grid back on and uncheck
		# the relief grid — the user's sequence that left a blank window.
		F.open!(hB, S, "sst"; promote = true)
		F.open!(hB, L, "layer0"; promote = false)
		@test click(hB, "sst") == 1
		@test click(hB, "layer0") == 0
		F.pump(30)

		for (tag, box) in (("full", full), ("zoom", zoom))
			for h in (hR, hA, hB); F.reframe!(h, box...); end
			R = F.capture(hR); A = F.capture(hA); B = F.capture(hB)
			dA = F.diff_frac(A, R); dB = F.diff_frac(B, R)
			wR = F.white_frac(R); wA = F.white_frac(A); wB = F.white_frac(B)
			pR = F.data_frac(R); pA = F.data_frac(A); pB = F.data_frac(B)
			@info "grid order ($tag)" dA dB wR wA wB pR pA pB
			@test pR > 0.2                          # the reference really shows data…
			@test wR > 0.05                         # …and its holes in the NaN colour
			@test dA < 0.05                         # opened second: the same picture
			@test dB < 0.05                         # re-checked after the other is unchecked: the same
			@test isapprox(pA, pR; atol = 0.02)     # no data lost or invented
			@test isapprox(pB, pR; atol = 0.02)
			@test isapprox(wA, wR; atol = 0.02)     # holes wear the NaN colour whatever the order
			@test isapprox(wB, wR; atol = 0.02)
		end
	finally
		F.close!(hR); F.close!(hA); F.close!(hB)
	end
end
