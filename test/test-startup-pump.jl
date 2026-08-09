# Regression guard for the "busy cursor for ~1 s over a freshly opened window" bug.
#
# WHAT WENT WRONG (2026-07-28). `_ensure_callbacks` (eventloop.jl) registers ~50 host callbacks, each
# an `@cfunction` costing a few ms. It runs AFTER `gmtvtk_open_empty` has returned — the window is
# already on screen — and BEFORE the pump Timer's first tick. With no `gmtvtk_process_events` inside
# the loop the window sat visible but UNPUMPED for the whole block (~0.86 s measured), and Windows
# paints a busy cursor over an app that is not servicing its message queue. No exception, nothing in
# the Errors console — which is exactly why it read as "it silently errors".
#
# WHAT THESE TESTS PIN. Not a wall-clock budget (the block is allowed to take as long as it takes),
# but the two properties that make the freeze impossible:
#   1. the loop pumps ONCE PER REGISTRATION — moving the pump out of the loop (or dropping it) sends
#      the count to 0 or 1 and fails here;
#   2. no single unpumped stretch is long enough to show a busy cursor.
# Both come from instrumentation the loop itself keeps (`_CB_PUMPS`, `_CB_MAXGAP`), so the test
# measures the real loop rather than re-reading the source.
#
# CI-safe: with no DLL every registration fails fast inside its own try/catch and the loop still runs,
# so the pump count is still asserted. The gap assertion is only meaningful when the DLL is there (the
# registrations then do real work), and is a valid upper bound either way.
@testitem "startup: callback registration never leaves the window unpumped" tags=[:unit] begin
	const IG = InteractiveGMT

	# `_ensure_callbacks` is one-shot (guarded by _CB_DONE). Re-arm it so this measures a real run;
	# registration is idempotent — each `_register_*` just re-installs its own callback pointer.
	IG._CB_DONE[] = false
	IG._ensure_callbacks()

	nreg = IG._CB_PUMPS[]
	@test IG._CB_DONE[]                      # the one-shot guard is still set on the way out

	# 1. One pump per registration. The loop registers dozens of callbacks; a pump hoisted out of it
	#    would leave this at 0 or 1, which is the bug verbatim.
	@test nreg >= 40

	# 2. No unpumped stretch long enough to freeze the window. The gap that matters is the FIRST
	#    registration's, which pays this session's compilation of the registration path — a machine-
	#    and load-dependent number (measured 0.11 s when this was written, 0.36-0.65 s on a busier
	#    box), NOT something the loop controls. The failure mode being guarded against is the WHOLE
	#    block unpumped, which measured ~0.86 s, so the bound sits below that and above the compile
	#    cost. The structural guard is assertion 1 (one pump per registration); this one only has to
	#    catch "nothing was pumped at all".
	@test IG._CB_MAXGAP[] < 0.8

	# The instrumentation must reset per run, or a second call would report a stale maximum.
	IG._CB_DONE[] = false
	IG._ensure_callbacks()
	@test IG._CB_PUMPS[] == nreg             # same set of registrations, freshly counted
	@test IG._CB_MAXGAP[] < 0.8
end
