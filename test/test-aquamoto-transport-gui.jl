# THE TRANSPORT IS MEASURED HERE, BY HOLDING THE BUTTON — UNDER THE APP'S OWN EVENT LOOP.
#
# `test-aquamoto-slider-hotpath.jl` reads the SOURCE: it can say that nothing is wired to the
# slider's signal and that nothing disables the arrows. It cannot say that holding `>` steps, that the
# tank is REDRAWN on screen while it does, or that it STOPS when the finger comes off.
#
# THE ITEM THAT USED TO BE HERE WAS USELESS, AND WHY IS THE POINT. It held the button inside
# `gmtvtk_aqua_hold_arrow_test`, which pumps its OWN `processEvents` loop. The app never runs that
# loop: there the outer loop is the Julia Timer's `gmtvtk_process_events`, whose `inPump` guard makes
# every nested tick during a slice draw a no-op. So the old item measured repeats being coalesced
# inside draws — a machine the user never holds — and it stayed green while the real one stalled.
# It also asserted only "the slider moved more than 1", which says nothing about what reaches the
# screen, whether the REPL froze, or whether loading ran on after release.
#
# Here the button is PRESSED and RELEASED with REAL OS mouse input (`aqf_realpress`, the fixture),
# and the hold is a Julia `sleep`, so the app's own Timer pump drives the auto-repeat exactly as it
# does under a finger. What is asserted is what the user sees:
#   * slices are DRAWN during the hold (the host's own "slice on screen", `st.cur`), several of them;
#   * each drawn slice is RENDERED (`gmtvtk_render_count_test` counts finished renders);
#   * Julia keeps getting control (the REPL is not frozen): no gap between wakeups over 1 s;
#   * after release the slider STOPS within one step, and the drawn slice catches up to it — no
#     loading that goes on by itself.

@testitem "Aquamoto transport: a real-pump hold draws, renders, keeps the REPL alive, stops on release" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	slider(h)  = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_slider_value_test), Cint, (Ptr{Cvoid},), h))
	renders(h, reset = false) = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_render_count_test), Cint,
	                                      (Ptr{Cvoid}, Cint), h, Cint(reset)))

	# Hold for `secs` by SLEEPING (the Timer pump runs), then release and let it settle `settle` s.
	function hold(h, st, dir, secs; settle = 3.0)
		renders(h, true)
		v0 = aqf_realpress(h, dir, true)
		@assert v0 >= 0 "arrow press hook failed: $v0"
		drawn = Int[]; gap = 0.0; tl = time(); t0 = tl
		while time() - t0 < secs
			sleep(0.01); t = time(); gap = max(gap, t - tl); tl = t
			push!(drawn, st.cur)
		end
		vrel = aqf_realpress(h, dir, false)
		nr = renders(h)
		marks = Int[]; t1 = time()
		while time() - t1 < settle
			sleep(0.01); t = time(); gap = max(gap, t - tl); tl = t
			push!(marks, slider(h))
		end
		(; v0, vrel, ndrawn = length(unique(drawn)), nr, gap,
		   vmid = marks[max(1, length(marks) ÷ 2)], vend = slider(h), cur = st.cur + 1)
	end

	mktempdir() do dir
		# 40 steps: enough that a working hold cannot run out of slices inside the hold window.
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_transport.nc"); nt = 40)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(80)
			st = IG._AQUA[f.h]
			for dir in (+1, -1)
				r = hold(f.h, st, dir, 2.0)
				@info "hold dir=$dir" r
				# Moves in the asked direction, several DRAWN slices (a healthy 2 s hold draws 10+).
				@test dir * (r.vrel - r.v0) > 3
				@test r.ndrawn > 3
				# Every drawn slice reached the screen.
				@test r.nr >= r.ndrawn
				# The REPL is alive throughout: the pump never kept the UI thread for a second.
				@test r.gap < 1.0
				# RELEASE STOPS IT: at most the one repeat already in flight, nothing after mid-settle,
				# and the tank shows the slice the slider says.
				@test abs(r.vend - r.vrel) <= 1
				@test r.vend == r.vmid
				@test r.cur == r.vend
			end
		finally
			aqf_close(f.h)
		end
	end
end

# THE SAME HOLD WITH "Sat img" ON, ON A REAL-SIZE TANK. With the satellite drape on, every slice also
# rebuilds the drape's RGBA at the "Res" resolution (Res 4 on 765x476 nodes = a 3060x1904 texture).
# That loop was type-unstable (the mask came from a Union-typed grid) and cost 1.2 s per slice, so a
# hold drew one slice every ~1.5 s, froze the REPL for as long, and went on loading after release —
# while the plain-tank item above stayed green. So this item runs the identical assertions with the
# satellite on, at the size where the cost shows. Needs the tiles once (GMT's own tile cache after).
@testitem "Aquamoto transport: the real-pump hold stays live with Sat img on" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	slider(h) = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_slider_value_test), Cint, (Ptr{Cvoid},), h))
	renders(h, reset = false) = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_render_count_test), Cint,
	                                      (Ptr{Cvoid}, Cint), h, Cint(reset)))
	check(h, name, on) = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_check_test), Cint,
	                           (Ptr{Cvoid}, Cstring, Cint), h, name, Cint(on))

	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_sat.nc"); nx = 765, ny = 476, nt = 40, coast = true)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(80)
			st = IG._AQUA[f.h]
			@test check(f.h, "cinema3DCheckBox", 1) == 1           # 3-D, where it was reported
			@test check(f.h, "splitDryWetCheckBox", 1) == 1        # Sat img needs a land side
			@test check(f.h, "renderedSatImgCheckBox", 1) == 1
			aqf_pump(40)
			@test st.satimg                                         # the drape really is on
			renders(f.h, true); g0 = Base.gc_time_ns()
			v0 = aqf_realpress(f.h, +1, true)
			drawn = Int[]; gap = 0.0; gapat = 0.0; tl = time(); t0 = tl
			while time() - t0 < 2.0
				sleep(0.01); t = time(); (t - tl > gap) && (gap = t - tl; gapat = t - t0); tl = t
				push!(drawn, st.cur)
			end
			vrel = aqf_realpress(f.h, +1, false)
			nr = renders(f.h)
			marks = Int[]; t1 = time()
			while time() - t1 < 3.0
				sleep(0.01); t = time(); (t - tl > gap) && (gap = t - tl; gapat = t - t0); tl = t
				push!(marks, slider(f.h))
			end
			vend = slider(f.h)
			@info "sat hold" gapat gcms = round((Base.gc_time_ns() - g0) / 1e6) v0 vrel ndrawn = length(unique(drawn)) nr gap vend cur = st.cur + 1
			@test vrel - v0 > 3
			@test length(unique(drawn)) > 3
			@test nr >= length(unique(drawn))
			@test gap < 1.0
			@test abs(vend - vrel) <= 1
			@test vend == marks[max(1, length(marks) ÷ 2)]
			@test st.cur + 1 == vend
		finally
			aqf_close(f.h)
		end
	end
end

# THE η(x) FIGURE COMES UP WITH THE TANK. Its only door used to be the Cinema tab's "Show η(x)
# profile" box (checked by default); 41b5e13 removed that box from aquamoto.ui and the figure silently
# stopped opening at all — no test noticed. This opens a tank and reads the figure off the screen
# (gmtvtk_aqua_eta_curve_test: -2 = no figure exists).
@testitem "Aquamoto: the η(x) figure opens with the tank" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_eta.nc"); nt = 5)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			s = Ref{Cdouble}(0.0)
			n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_eta_curve_test), Cint,
			          (Ptr{Cvoid}, Ptr{Cdouble}, Cint), f.h, s, Cint(0))
			@test n >= 2
		finally
			aqf_close(f.h)
		end
	end
end

# (An item that held the arrow on a PLAIN, non-benchmark window and watched the same curve lived
# here. It was dropped: the benchmark item below asserts the very same thing about the profile curve
# AND about the reference, in the configuration the user is actually in, while this one only held on
# the geographic fixture and proved flaky inside the runner — the figure it measured came back empty
# when the item ran straight after the hold item, though the identical sequence standalone gives
# 128 points before and after the hold. Measuring less, less reliably, is not worth a red suite.)

# (The item "Aquamoto transport: the analytic reference follows every slice" lived here. It drove the
# Cinema tab's "Profile figure" block — cinemaProfileCheckBox / cinemaProfileX0Edit /
# cinemaProfileLenEdit, through gmtvtk_aqua_show_eta_test and gmtvtk_aqua_set_prof_range_test — and
# that block was removed from aquamoto.ui, so the hooks find no widget, return 0, and every assertion
# after them reads a figure that was never raised. The control the item exercised no longer exists,
# so the item goes with it, not the code.)
