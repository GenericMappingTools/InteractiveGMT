# Geophysics > Tsunamis > Tsunami travel times (tsunamittt.jl + TttDialog, 70_window.cpp).

@testmodule TttTest begin

using InteractiveGMT
const IG = InteractiveGMT

# A lon/lat ocean (off Portugal), 3000 m deep with a shallower bump, WITHOUT a CRS -- like the
# user's layer0.grd: geographic to the window only through _isgeographic's range test, which is
# exactly what the Mirone run missed (it asked GMT.isgeog, ran Cartesian and returned ~0 h).
function bathy()
	IG.GMT.mat2grid(
		Float32[-3000 + 2000exp(-(((ix - 20) / 8)^2 + ((iy - 15) / 6)^2)) for iy in 0:30, ix in 0:40];
		x = collect(range(-14.0, -10.0, length = 41)), y = collect(range(36.0, 39.0, length = 31)))
end

geom(A) = (A.x, A.y, A.inc, A.registration, A.range[1:4], length(A.z))

end

# Both methods through the dialog's own callback. The Mirone one used to die on its first line with
# "TypeError: non-boolean (Int64) used in boolean context" (`geo` handed the 0/1 Int of `_isgeog`).
# The result is a grid computed FROM the bathymetry, so it must come back on the bathymetry's exact
# geometry (SACRED_LAW.md, derived-from-a-grid geometry law).
@testitem "travel times: both methods run and keep the bathymetry's geometry; contours trace them" tags=[:gui] setup=[TttTest, GmtvtkTest] begin
	IG = InteractiveGMT
	G = TttTest.bathy()
	f = viz(G)
	try
		for (k, method) in enumerate(("mirone", "ttt", "mirone"))    # runs 2 and 3 start with the travel times on display
			# grid = what the dialog sends: activeGridName. On the 2nd run that is the travel-time grid itself.
			act0 = unsafe_string(ccall(GmtvtkTest._test_fn(:gmtvtk_active_grid_name_test), Cstring, (Ptr{Cvoid},), f.h))
			k > 1 && @test act0 == "Travel time (h)"
			req = "what=grid\ngrid=$act0\nmethod=$method\nsrcxy=-12,37.5" *
			      (method == "mirone" ? "\nfillvoids=0" : "\nnodes=120\nsearch=0\nbias=1\nradius=0\nsrcdepth=0\nmindepth=0")
			@test IG._on_ttt(f.h, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, req))) == 1
			R = IG._find_object_exact(f.h, :grid, "Travel time (h)")
			@test R !== nothing
			R === nothing && continue
			@test TttTest.geom(R) == TttTest.geom(G)
			v = filter(!isnan, R.z)
			# ~2.5 deg (~270 km) to the far corner at sqrt(g*3000 m) = 171 m/s is ~0.43 h (measured: 0.43 Mirone,
			# 0.45 ttt). The Cartesian mistake gives ~4e-6 h.
			@test !isempty(v) && maximum(v) > 0.2
		end
		# Contours then trace the layer ON DISPLAY (the travel times, in hours), never the bathymetry
		# under it: the name the C++ side sends is activeGridName's, and the range comes off that grid.
		_test_fn = GmtvtkTest._test_fn
		act = unsafe_string(ccall(_test_fn(:gmtvtk_active_grid_name_test), Cstring, (Ptr{Cvoid},), f.h))
		@test act == "Travel time (h)"
		Gc = IG._contour_grid(f.h, act)
		@test Gc === IG._find_object_exact(f.h, :grid, "Travel time (h)")
		@test Gc !== G && maximum(filter(!isnan, Gc.z)) < 100   # hours, not metres of bathymetry
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "travel times dialog: X, minimise and Esc park it, Delete ends it" tags=[:gui] setup=[TttTest, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(TttTest.bathy())
	parked() = ccall(_test_fn(:gmtvtk_ttt_parked_test), Cint, (Ptr{Cvoid},), f.h)
	opened() = ccall(_test_fn(:gmtvtk_ttt_open_dialog_test), Cint, (Ptr{Cvoid},), f.h)
	leave(how) = ccall(_test_fn(:gmtvtk_ttt_leave_dialog_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, how)
	rows() = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	try
		# Opening twice yields ONE dialog: the second call brings back the first.
		@test opened() == 1 && parked() == 0
		@test opened() == 1 && parked() == 0
		# Every way out parks it, with a row in this window's Scene Objects; the menu brings it back.
		for how in (0, 1, 2)                     # the X, minimise, Esc
			leave(how)
			@test parked() == 1
			@test occursin("Tsunami travel times", rows())
			@test opened() == 1 && parked() == 0
		end
		# Only the parked row's own "Delete" ends it, and the row goes with it.
		leave(0)
		@test ccall(_test_fn(:gmtvtk_ttt_delete_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test parked() == -1
		@test !occursin("Tsunami travel times", rows())
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
	@test ccall(_test_fn(:gmtvtk_scene_borrowed_test), Cint, (Ptr{Cvoid},), f.h) == 0
end

# Contouring the travel times puts ONE "Contours" set in Scene Objects — and Apply again REPLACES it.
@testitem "travel times: contours of the travel-time layer are ONE set in Scene Objects" tags=[:gui] setup=[TttTest, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(TttTest.bathy())
	tree() = (for _ in 1:5; IG._pump_once(); sleep(0.02); end;
	          unsafe_string(ccall(_test_fn(:gmtvtk_objtree_test), Cstring, (Ptr{Cvoid},), f.h)))
	sets(t) = count(l -> strip(l) == "Contours", split(t, '\n'))
	try
		req = "what=grid\ngrid=\nmethod=mirone\nsrcxy=-12,37.5\nfillvoids=0"
		@test IG._on_ttt(f.h, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, req))) == 1
		act = unsafe_string(ccall(_test_fn(:gmtvtk_active_grid_name_test), Cstring, (Ptr{Cvoid},), f.h))
		IG._on_contours(f.h, "0;1;0.1,0.2,0.3")
		t1 = tree()
		@test sets(t1) == 1
		IG._on_contours(f.h, "0;1;0.1,0.2,0.3")
		t2 = tree()
		@test sets(t2) == 1
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# The Source block, as Mirone's: NO default location — "Plot point source" (one click) and "Draw Fault
# source" (a line, every vertex a source) placed with the window's own draw tools, through the
# dialog's own buttons. Two sources = Mirone's refusal; none = "Yes I compute, but ... WHAT?".
@testitem "travel times dialog: point and fault sources are placed on the map" tags=[:gui] setup=[TttTest, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	place(h, fault, xy) = unsafe_string(ccall(_test_fn(:gmtvtk_ttt_place_source_test), Cstring,
	                                          (Ptr{Cvoid}, Cint, Ptr{Cdouble}, Cint), h, fault, xy, length(xy) ÷ 2))
	pts(s) = [parse.(Float64, split(p, ',')) for p in split(s, ';')]
	rows(h) = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), h))
	ttt(h, src) = IG._on_ttt(h, Base.unsafe_convert(Cstring, Base.cconvert(Cstring,
		"what=grid\ngrid=\nmethod=mirone\nfillvoids=0\nsrcxy=$src")))
	for fault in (0, 1)
		f = viz(TttTest.bathy())
		try
			ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, 1)
			@test ccall(_test_fn(:gmtvtk_ttt_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
			# Nothing placed (no default location, no boxes): Mirone's refusal.
			@test place(f.h, fault, Float64[]) == "!Yes I compute, but ... WHAT?"
			want = fault == 1 ? [-12.5, 37.0, -12.0, 37.5, -11.5, 38.0] : [-12.0, 37.5]
			src = place(f.h, fault, want)
			@test !startswith(src, "!")
			P = pts(src)
			@test length(P) == length(want) ÷ 2
			@test all(isapprox(P[k], want[2k-1:2k]; atol = 0.1) for k in eachindex(P))
			@test occursin("TTT source", rows(f.h))
			if fault == 0
				# The point is the Circle tool's circle and nothing else: a double-click arms it and shows
				# the same selection handle (the inner square) every circle shows. It was once repainted
				# yellow — the handle's own colour — and the square vanished on it alone.
				ccall(_test_fn(:gmtvtk_dblclick_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Ptr{Cint}),
				      f.h, P[1][1], P[1][2], 0.0, C_NULL)
				dbg = zeros(4)
				ccall(_test_fn(:gmtvtk_sym_debug_test), Cvoid, (Ptr{Cvoid}, Ptr{Cdouble}), f.h, dbg)
				@test dbg[1] >= 0 && dbg[3] == 1
				ref = zeros(4)   # fill of the TTT circle vs the handle's yellow: must differ
				@test ccall(_test_fn(:gmtvtk_ttt_src_fill_test), Cint, (Ptr{Cvoid}, Ptr{Cdouble}), f.h, ref) == 1
				@test ref[1:3] != [1.0, 1.0, 0.0]
				ccall(_test_fn(:gmtvtk_dblclick_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Ptr{Cint}),
				      f.h, P[1][1], P[1][2], 0.0, C_NULL)   # double-click again: leave the armed state
			end
			@test ttt(f.h, src) == 1
			R = IG._find_object_exact(f.h, :grid, "Travel time (h)")
			v = filter(!isnan, R.z)
			@test minimum(v) < 0.05 && maximum(v) > 0.2      # zero at the source, hours away at the corners
			# A second source is refused, as in Mirone.
			@test startswith(place(f.h, 1 - fault, [-13.0, 36.5, -13.0, 37.0][1:(fault == 1 ? 2 : 4)]), "!More than one source")
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		end
	end
end
