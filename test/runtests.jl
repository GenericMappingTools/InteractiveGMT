using InteractiveGMT
using TestItemRunner

# Two tiers of tests:
#   * :unit / :fast  — pure-Julia helpers (colour, CPT, packing, basemap pixel math, the scene-state
#                      parser). No DLL, run anywhere `using InteractiveGMT` works. Always run.
#   * :gui           — scenario tests that open REAL Qt+VTK windows through the built gmtvtk.dll and
#                      assert scene state via gmtvtk_scene_state. Opt in with INTERACTIVEGMT_TEST_GUI=1
#                      (add QT_QPA_PLATFORM=offscreen for a headless attempt). Skipped by default so
#                      CI / a DLL-less checkout still passes.
# Tolerant of how cmd/PowerShell mangle the value: strip surrounding quotes and whitespace
# (`set VAR="1"` keeps the quotes; `set VAR=1 && ...` keeps a trailing space). Also honour
# `Pkg.test(test_args=["gui"])`, which forwards "gui" into ARGS reliably across the subprocess.
const _RUN_GUI = ("gui" in ARGS) ||
	lowercase(strip(get(ENV, "INTERACTIVEGMT_TEST_GUI", "0"), [' ', '"', '\''])) in ("1", "true", "yes", "on")
# :net testitems hit live web services (e.g. the USGS seismicity query) — opt in the same way.
const _RUN_NET = ("net" in ARGS) ||
	lowercase(strip(get(ENV, "INTERACTIVEGMT_TEST_NET", "0"), [' ', '"', '\''])) in ("1", "true", "yes", "on")

# Run only the testitems whose NAME contains this text (INTERACTIVEGMT_TEST_NAME="Plate calculator").
# Empty = everything, i.e. what CI does. Purely a development convenience: it keeps a change to one
# tool from costing a full-suite run on every iteration.
const _ONLY = strip(get(ENV, "INTERACTIVEGMT_TEST_NAME", ""), [' ', '"', '\''])

# Same convenience, one level up: run only the testitems whose FILE matches
# (INTERACTIVEGMT_TEST_FILE="scene-gui"). A name filter cannot express "this whole file", and the
# GUI tier is where that hurts — chasing one failure in test-scene-gui.jl through the full suite
# costs minutes of unrelated windows opening and closing, and some GUI faults only show up with the
# rest of THAT file's items ahead of them, so they cannot be narrowed by name either.
const _ONLYFILE = strip(get(ENV, "INTERACTIVEGMT_TEST_FILE", ""), [' ', '"', '\''])

# …and by TAG (INTERACTIVEGMT_TEST_TAG="xyplot"), the third axis: a fault that only shows up in one
# subsystem is reproduced by that subsystem's items, not by its file's other four dozen.
const _ONLYTAG = strip(get(ENV, "INTERACTIVEGMT_TEST_TAG", ""), [' ', '"', '\''])

# macOS/arm64: GMT picks Accelerate's vDSP for its FFT and SEGFAULTS (signal 11) inside
# vDSP_fft2d_zip — vDSP_fft2d_zip <- gmtfft_2d_vDSP <- GMT_FFT_2D, all inside libgmt — which kills
# the whole test process, not just the item that asked. A bug in GMT's C library that no test can
# survive, so the portable KissFFT backend is selected instead; it gives the same answers.
#
# GMT must WRITE the defaults file itself (a hand-written one is refused as "may not be GMT 6
# compatible", and a refused file changes nothing). `gmtset` writes gmt.conf into the CURRENT
# directory, which is where GMT looks first — so this holds for the test process wherever it runs,
# with no dependence on a CI step. Remove once GMT's vDSP path is fixed upstream.
if Sys.isapple()
	try
		InteractiveGMT.GMT.gmt("gmtset GMT_FFT kissfft")
		@info "tests: GMT_FFT=kissfft (GMT's vDSP FFT segfaults on this platform)"
	catch e
		@warn "tests: could not select GMT's KissFFT backend; FFT items may crash" exception=(e,)
	end
end

@run_package_tests verbose=true filter = ti ->
	(_RUN_GUI || !(:gui in ti.tags)) && (_RUN_NET || !(:net in ti.tags)) &&
	(isempty(_ONLY) || occursin(_ONLY, ti.name)) &&
	(isempty(_ONLYFILE) || occursin(_ONLYFILE, ti.filename)) &&
	(isempty(_ONLYTAG) || Symbol(_ONLYTAG) in ti.tags)
