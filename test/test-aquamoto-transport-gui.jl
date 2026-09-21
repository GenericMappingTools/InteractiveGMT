# THE TRANSPORT IS MEASURED HERE, BY HOLDING THE BUTTON.
#
# `test-aquamoto-slider-hotpath.jl` reads the SOURCE: it can say that nothing is wired to the
# slider's signal and that nothing disables the arrows. It cannot say that holding `>` steps — and
# that is the failure the user has hit three times in a row:
#
#   * the arrows were disabled for the duration of each slice draw, and Qt CANCELS a pressed button's
#     auto-repeat when the button is disabled (the press is dropped, the grab goes, and the repeat
#     does not resume when it is re-enabled) — so a hold gave exactly ONE step, for ever;
#   * a blocking host call taken between two repeats ate the gap the next repeat needed.
#
# Both are invisible to source reading and to every Julia-level call. So this item presses the real
# QToolButton with a real QMouseEvent, holds it while the event loop runs, releases it, and asserts
# the slider actually travelled (`gmtvtk_aqua_hold_arrow_test`, 90_c_api.cpp). Re-enable the arrows
# in `transportEnable` and this goes red; that is what it is for.

@testitem "Aquamoto transport: holding > steps through slices, holding < comes back" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))

	# Hold an arrow for `ms` and report (slices advanced, slice landed on).
	hold(h, dir, ms) = begin
		out = Ref{Cint}(0)
		n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_hold_arrow_test), Cint,
		          (Ptr{Cvoid}, Cint, Cint, Ptr{Cint}, Ptr{Cint}, Ptr{Cint}), h, Cint(dir), Cint(ms),
		          out, C_NULL, C_NULL)
		(Int(n), Int(out[]))
	end

	mktempdir() do dir
		# 40 steps: enough that a working hold cannot run out of slices inside the hold window.
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_transport.nc"); nt = 40)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)

			# THE HOLD. Qt's auto-repeat starts after ~300 ms and then fires every ~100 ms, so 2 s of
			# holding is a dozen repeats on an idle machine. The assertion is deliberately loose — how
			# many slices a hold gets through depends on how fast each one draws — but it is FAR above
			# what the defect produces, which is exactly 1, however long the button is held.
			adv, land = hold(f.h, +1, 2000)
			@test adv > 1
			@test land > 1

			# …AND BACK. The left arrow must do the same in the other direction (its own button, its
			# own repeat), and it must not be the slider simply running to an end stop.
			back, home = hold(f.h, -1, 2000)
			@test back > 1
			@test home < land
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

# THE ANALYTIC REFERENCE MUST FOLLOW THE SLICE TOO — and this is the item that runs in the
# configuration the user's window is actually in: a BENCHMARK window. Membership of `_BM1_SCENES` is
# what makes the host answer with a reference curve at all (benchmark1.jl `_aqua_eta_curves`), and it
# is also what sets `etaHostCurves_` on the dialog — the flag that used to suppress the per-slice
# repaint. The plain-cube item above cannot see any of that: it never enters the host-owned path, and
# it stayed green through every version of this defect.
#
# What is measured is the SECOND curve on the panel (`which = 1`): the analytic solution at the
# slice's model time. It is a different curve at every timestep, so two different slices that come
# back with the same reference mean the figure is comparing the wave against a solution from another
# instant — the bug, reported as "you are not updating the analytical solution".
@testitem "Aquamoto transport: the analytic reference follows every slice" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))

	curve(h, which) = begin
		s = Ref{Cdouble}(0.0)
		n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_eta_curve_test), Cint,
		          (Ptr{Cvoid}, Ptr{Cdouble}, Cint), h, s, Cint(which))
		(Int(n), Float64(s[]))
	end
	# Hold the arrow and report (slices advanced, DIFFERENT analytic curves, DIFFERENT figure curves)
	# — all three counted WHILE the button is down.
	hold(h, dir, ms) = begin
		d = Ref{Cint}(0);  dc = Ref{Cint}(0)
		n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_hold_arrow_test), Cint,
		          (Ptr{Cvoid}, Cint, Cint, Ptr{Cint}, Ptr{Cint}, Ptr{Cint}), h, Cint(dir), Cint(ms),
		          C_NULL, d, dc)
		(Int(n), Int(d[]), Int(dc[]))
	end

	mktempdir() do dir
		# A BENCHMARK-SHAPED WINDOW: Cartesian, metres, a flume 20 km long — the span the analytic
		# solution actually covers. On the default geographic patch (half a degree wide) the reference
		# comes back empty for real reasons, and the item would pass while measuring nothing.
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_analytic.nc"); nt = 40, geog = false,
		                         xrange = (-200.0, 20000.0), yrange = (0.0, 5000.0))
		# The finer NEST a real run writes beside its cube. Registering it is not decoration: it is what
		# makes the host answer with a stitched MODEL curve, and therefore what sets `etaHostCurves_` on
		# the dialog — the flag whose repaint gate was this defect. Without it the flag never goes true
		# and the item measures a path the user is not on.
		lev = aqf_make_tsunami_nc(joinpath(dir, "tsu_analytic_lev1.nc"); nt = 40, geog = false,
		                          xrange = (-200.0, 1000.0), yrange = (0.0, 5000.0))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			# …and it is a benchmark window, declared the one way benchmark1.jl declares one: that is
			# what makes the host answer with a reference at all, AND what sets `etaHostCurves_` on the
			# dialog — the flag whose per-slice-repaint gate was this whole defect.
			# …through the REAL door, the one benchmark1.jl uses: it registers the window AND locks the
			# controls that do not apply to a benchmark.
			IG._bm1_mark_scene!(f.h)
			IG._BM1_LEVELS[f.h] = [lev]
			aqf_pump(5)
			# THE "Rendered image" BLOCK IS LOCKED. On a benchmark it would spend a full two-sided
			# render per slice on a flume whose answer is the η(x) curve — useless and slow, so the
			# block states that by being disabled, not by quietly doing nothing.
			enabled(n) = ccall(GmtvtkTest._test_fn(:gmtvtk_widget_enabled_test), Cint, (Cstring,), n)
			@test enabled("renderedImageGroupBox") == 0
			@test enabled("renderedImageCheck")    == 0
			@test enabled("cinemaPlaybackGroupBox") == 1    # …and nothing else was locked with it
			@test ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_show_eta_test), Cint,
			            (Ptr{Cvoid}, Cint), f.h, Cint(1)) == 1
			@test ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_set_prof_range_test), Cint,
			            (Ptr{Cvoid}, Cdouble, Cdouble), f.h, -200.0, 20200.0) == 1
			aqf_pump(30)

			# THE HOLD, AND WHAT HAPPENED *DURING* IT. `distinct` is how many different analytic curves
			# were on the figure while the button was still down — sampled at every slice the hold
			# passed through, before the release. 1 means it stood still under the finger and only
			# caught up afterwards; 0 means it was not there at all. Either is the defect.
			adv1, distinct1, dcurve1 = hold(f.h, +1, 2500)
			@test adv1 > 1
			@test distinct1 > 1                # the ANALYTIC curve moved under the finger…
			@test dcurve1 > 1                  # …and so did the figure's own curve (the track/row)
			aqf_pump(40)
			nm1, m1 = curve(f.h, 0)
			nr1, r1 = curve(f.h, 1)
			@test nm1 > 2                      # the slice profile is on the figure…
			@test nr1 > 2                      # …and so is the ANALYTIC SOLUTION

			adv2, distinct2, dcurve2 = hold(f.h, -1, 2500)   # …and the same going back
			@test adv2 > 1
			@test distinct2 > 1
			@test dcurve2 > 1
			aqf_pump(40)
			nm2, m2 = curve(f.h, 0)
			nr2, r2 = curve(f.h, 1)
			@test nm2 > 2
			@test nr2 > 2
			@test m2 != m1                     # the wave moved…
			@test r2 != r1                     # …AND THE ANALYTIC CURVE MOVED WITH IT
		finally
			delete!(IG._BM1_LEVELS, f.h)
			delete!(IG._BM1_SCENES, f.h)
			aqf_close(f.h)
		end
	end
end
