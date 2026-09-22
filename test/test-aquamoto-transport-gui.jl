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

# (The item "Aquamoto transport: the analytic reference follows every slice" lived here. It drove the
# Cinema tab's "Profile figure" block — cinemaProfileCheckBox / cinemaProfileX0Edit /
# cinemaProfileLenEdit, through gmtvtk_aqua_show_eta_test and gmtvtk_aqua_set_prof_range_test — and
# that block was removed from aquamoto.ui, so the hooks find no widget, return 0, and every assertion
# after them reads a figure that was never raised. The control the item exercised no longer exists,
# so the item goes with it, not the code.)
