# THE SLICE SLIDER'S HOT PATH IS PROTECTED HERE.
#
# The `<` `>` arrow buttons beside the Aquamoto slice slider AUTO-REPEAT: holding one down fires
# `sliceSlider::valueChanged` as fast as Qt can deliver it, and every one of those ticks ends in
# `fireSlice`, which BLOCKS on the host while the slice is read, composited and pushed. Anything else
# wired to that same signal therefore runs once per repeat, on the thread that is already blocking —
# and the update stalls while the button is held.
#
# That has been broken repeatedly, most recently (2026-09-19) by mirroring the Debug tab's duplicate
# slice row from `sliceSlider::valueChanged`: two extra widget updates per repeat, and holding an
# arrow stalled the slice update. The fix is not a tuning: NOTHING may be connected to that signal.
# The Debug copy DRIVES `sliceSlider` (one direction, cheap) and is refreshed FROM it only in
# `afterSliceShown`, which runs once per slice ACTUALLY DRAWN.
#
# These are SOURCE tests, deliberately. The defect is a wiring decision in C++ that no Julia-level
# call can observe, and reproducing an auto-repeat stall in CI would need a Qt window, a held button
# and a wall-clock threshold — a slow, flaky test for a fault that is exactly one `connect` line.
# Reading that line is precise, instant and runs anywhere.

@testitem "aquamoto: only the slice itself is wired to the slider's valueChanged" tags=[:unit, :fast] begin
	src = read(joinpath(dirname(dirname(pathof(InteractiveGMT))), "deps", "src", "75_aquamoto.cpp"),
	           String)
	# EXACTLY ONE connection may have `sliceSlider::valueChanged` as its source: the handler that
	# mirrors the number box and calls `fireSlice`. That one IS the slice path. Every further
	# connection runs once per auto-repeat tick, on the thread already blocking on the host — which is
	# the stall. So the test is on the COUNT, not on the presence.
	ms = collect(eachmatch(r"connect\s*\(\s*sliceSlider\s*,\s*&QScrollBar::valueChanged", src))
	@test length(ms) == 1
	# …and that one is the slice path: `fireSlice` appears inside its lambda.
	body = src[ms[1].offset:min(lastindex(src), ms[1].offset + 400)]
	@test occursin("fireSlice()", body)
end

@testitem "aquamoto: the Debug slice row mirrors only after a slice is drawn" tags=[:unit, :fast] begin
	src = read(joinpath(dirname(dirname(pathof(InteractiveGMT))), "deps", "src", "75_aquamoto.cpp"),
	           String)
	# The Debug copy exists…
	@test occursin("dbgSliceSlider", src)
	# …it drives the real slider (one direction, no host work of its own)…
	@test occursin(r"connect\s*\(\s*dbgSlider\s*,\s*&QScrollBar::valueChanged", src)
	# …and the refresh FROM the real slider lives in afterSliceShown, which runs once per drawn slice.
	i = findfirst("void afterSliceShown()", src)
	@test i !== nothing
	tail = src[first(i):min(lastindex(src), first(i) + 1200)]
	@test occursin("dbgSlider_", tail)
	# The mirror must not be able to drive back while it is being refreshed.
	@test occursin("QSignalBlocker", tail)
end

@testitem "aquamoto: the Debug tab's widgets are in the .ui, not built in code" tags=[:unit, :fast] begin
	root = dirname(dirname(pathof(InteractiveGMT)))
	ui  = read(joinpath(root, "deps", "ui", "aquamoto.ui"), String)
	src = read(joinpath(root, "deps", "src", "75_aquamoto.cpp"), String)
	# Every widget this session added belongs to the .ui — the dialog is loaded from it at runtime and
	# hand-building widgets in code is the "modify the .ui under the hood" this project forbids.
	for name in ("combinedImageButton", "dbgSliceSlider", "dbgSliceNSpinBox", "debugTab", "anugaTab")
		@test occursin(name, ui)
	end
	# …and the code only WIRES them: no `new QPushButton` for the combined-image button.
	@test !occursin(r"new\s+QPushButton\s*\(\s*\"Combined image\"", src)
end

# (An item here asserted that the η(x) reference ask DEFERRED itself while a transport button was
# down. That requirement is dead: deferring it is exactly what left the analytic curve standing still
# under the finger, and the order now is that both curves follow every slice DURING the hold. The ask
# is made inline per slice, and what it does is measured on the figure itself in
# test-aquamoto-transport-gui.jl, "the analytic reference follows every slice" — which counts how many
# different analytic curves appear while the button is held, and goes red the moment one does not.)

# HOLDING `<` / `>` MUST KEEP STEPPING. The two arrows beside the slice slider AUTO-REPEAT, and Qt
# cancels a pressed button's repeat the moment that button is DISABLED: the press is dropped, the
# grab goes, and the repeat does not resume when it is re-enabled — it needs a fresh press. So a
# `transportEnable(false)` that reaches those two arrows gives exactly ONE step per hold and then
# silence, which is what "pushing > < continuously does nothing" was (2026-09-21, reported three
# times). They need no disabling: a press landing during a draw is COALESCED by `busy_`/`sliceDirty_`
# and caught up once at the end — a design that only works if the presses actually arrive.
@testitem "aquamoto: the auto-repeating slider arrows are never disabled" tags=[:unit, :fast] begin
	src = read(joinpath(dirname(dirname(pathof(InteractiveGMT))), "deps", "src", "75_aquamoto.cpp"),
	           String)
	# The arrows exist and auto-repeat…
	@test occursin(r"leftBtn->setAutoRepeat\(true\)", src)
	@test occursin(r"rightBtn->setAutoRepeat\(true\)", src)
	# …and NOTHING sets their enabled state, anywhere.
	for w in ("sliceArrowL_", "sliceArrowR_")
		@test !occursin(Regex(w * raw"\s*->\s*setEnabled"), src)
	end
	i = findfirst("void transportEnable(bool on)", src)
	@test i !== nothing
	body = src[first(i):min(lastindex(src), first(i) + 400)]
	@test occursin("setEnabled(on)", body)          # it still gates the single-shot Cinema buttons…
	@test !occursin("sliceArrowL_", body)           # …and never the auto-repeating arrows
	@test !occursin("sliceArrowR_", body)
	@test !occursin("QToolButton", body)
end
