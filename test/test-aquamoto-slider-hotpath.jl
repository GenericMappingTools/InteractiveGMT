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
