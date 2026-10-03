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
# Resume a run: only items whose test FILE sorts at or after this name (files run alphabetically), so
# the ones a stopped run already passed are not run again. e.g. INTERACTIVEGMT_TEST_FROM=test-sacred-law
const _FROMFILE = strip(get(ENV, "INTERACTIVEGMT_TEST_FROM", ""), [' ', '"', '\x27'])

# macOS/arm64: GMT picks Accelerate's vDSP for its FFT and SEGFAULTS (signal 11) inside
# vDSP_fft2d_zip — vDSP_fft2d_zip <- gmtfft_2d_vDSP <- GMT_FFT_2D, all inside libgmt — which kills
# the whole test process, not just the item that asked. A bug in GMT's C library that no test can
# survive, so the portable KissFFT backend is selected instead; it gives the same answers.
#
# GMT must WRITE the defaults file itself (a hand-written one is refused as "may not be GMT 6
# compatible", and a refused file changes nothing). `gmtset` writes gmt.conf into the CURRENT
# directory, which is where GMT looks first — so this holds for the test process wherever it runs,
# with no dependence on a CI step. Remove once GMT's vDSP path is fixed upstream.
#
# The setting is then READ BACK and reported, because asking is not the same as it having taken: a
# ~/.gmt/gmt.conf carrying GMT_FFT = kissfft was NOT honoured (CI job 100087642537 printed the file
# and the run segfaulted in vDSP regardless), and WHY it was not honoured is still unknown. Only
# `gmtget` answers that, and it answers it from inside the very process that is about to crash. If
# the value did not take, say so LOUDLY here, where the line lands right above the crash it
# explains, instead of leaving a bare `signal (11)` to be re-diagnosed from scratch. The FFT items
# are NOT skipped on that account: a test that cannot run is a defect to fix, not one to hide.
#
# ...and that is exactly what it turned out to be: `gmtset`/`gmtget` run a MODULE, which reads and
# writes the defaults FILE; the long-lived API session this process does its FFTs through
# (GMT.G_API[]) was created by `using GMT` before any of it and keeps its own copy of the settings.
# So the file said kissfft, `gmtget` said kissfft, and the session that actually called GMT_FFT_1D
# still dispatched to vDSP and died (run 33583990097, GMT_FFT_1D <- libgmt.6.7.0.dylib). The fix is
# to set the value ON THAT SESSION, through the API, which is what GMT_Set_Default does.
# ...and the backend picked here is FFTW, not KissFFT, which is the correction to the above. Forcing
# kissfft did stop the vDSP segfault and the run then died differently: `signal (6): Abort trap` in
# the X,Y op-dispatch item (run 33657361347), no message, a one-frame backtrace. GMT's C source
# contains no abort() at all and the GMT_jll aarch64 build log settles what is left:
#     -- Using CFLAGS = '-std=gnu99 -fopenmp=libomp -O3 -DNDEBUG'
#     *  OpenMP support             : enabled
#     *  FFTW library               : .../libfftw3f.dylib
# NDEBUG means GMT's asserts are compiled out, so no assert raised it either. What IS live is
# OpenMP: kiss_fft.c's kf_work opens a `#pragma omp parallel for` region, so selecting kissfft --
# and ONLY selecting kissfft, since GMT reaches for vDSP/FFTW otherwise -- runs GMT's libomp inside
# a Julia process that already carries one. Two OpenMP runtimes in one process abort, which is the
# signal 6 exactly. FFTW3f is linked into the same jll, handles any length (vDSP is radix-2 only,
# which is why auto-selection fell into it for N=1024 and crashed), and stays out of OpenMP.
if Sys.isapple()
	try
		try		# the LIVE session — the one whose GMT_FFT_1D crashes; a file cannot reach it
			r = ccall((:GMT_Set_Default, InteractiveGMT.GMT.libgmt), Cint,
			          (Ptr{Cvoid}, Cstring, Cstring), InteractiveGMT.GMT.G_API[], "GMT_FFT", "fftw")
			(r == 0) || @error "tests: GMT_Set_Default(GMT_FFT, fftw) returned $r on the live session"
		catch e
			@warn "tests: could not set GMT_FFT on the live API session" exception=(e,)
		end
		InteractiveGMT.GMT.gmt("gmtset GMT_FFT fftw")
		# Read it back FROM THE LIVE SESSION (GMT_Get_Default), not from the file: the file was never
		# the thing that was wrong. `gmtget` is only the fallback when that call is unavailable.
		eff = try
			b = Vector{UInt8}(undef, 256)
			ok = ccall((:GMT_Get_Default, InteractiveGMT.GMT.libgmt), Cint,
			           (Ptr{Cvoid}, Cstring, Ptr{UInt8}), InteractiveGMT.GMT.G_API[], "GMT_FFT", b)
			(ok == 0) ? strip(unsafe_string(pointer(b))) : error("GMT_Get_Default returned $ok")
		catch
			try
				d = InteractiveGMT.GMT.gmt("gmtget GMT_FFT")
				t = d isa AbstractString ? d : (hasproperty(d, :text) && !isempty(d.text) ? d.text[1] : string(d))
				strip(String(t))
			catch
				"<unreadable>"
			end
		end
		if occursin("fftw", lowercase(eff))
			@info "tests: GMT_FFT=$eff (vDSP segfaults here, KissFFT drags in a second OpenMP runtime)"
		else
			@error "tests: GMT_FFT is '$eff', NOT fftw— GMT's vDSP path will SEGFAULT the whole " *
			       "test process (vDSP_fft2d_zop <- gmtfft_2d_vDSP <- GMT_FFT_2D). cwd=$(pwd())"
		end
	catch e
		@warn "tests: could not select GMT's FFTW backend; FFT items may crash" exception=(e,)
	end
end

using Test

# For the length of this run, a failure is RECORDED but not printed as it happens: the negative items
# provoke ~200 refusals on purpose, and each one screaming a block of @error + backtrace buries the
# terminal. Nothing is lost — every error still lands in the sink, a test may only claim its own by
# name, and the two verdict testsets at the bottom PRINT whatever nobody claimed and fail on it.
InteractiveGMT._TEST_MODE[] = true

# THE GUI TIER RUNS OFF THE USER'S SCREEN. Every window the run opens is parked beyond the edge of
# the desktop as it appears (gmtvtk_hide_windows_test, the test DLL), where it still renders, so the
# pixel checks read the same frames and nobody's screen flickers for eight minutes. The only items
# that bring their window back are the two that press the REAL mouse (test-aquamoto-transport-gui.jl),
# for as long as they press. INTERACTIVEGMT_TEST_SHOW=1 leaves every window on screen, to watch a run.
const _SHOW_GUI = lowercase(strip(get(ENV, "INTERACTIVEGMT_TEST_SHOW", "0"), [' ', '"', '\''])) in ("1", "true", "yes", "on")
const _TEST_LIB = Ref{Ptr{Cvoid}}(C_NULL)        # the test DLL, for the dialog verdict at the end
if _RUN_GUI && !_SHOW_GUI
	let name = Sys.iswindows() ? "gmtvtk_test.dll" : Sys.isapple() ? "libgmtvtk_test.dylib" : "libgmtvtk_test.so",
	    libs = filter(isfile, [joinpath(InteractiveGMT._PKGROOT, "deps", "build", name),
	                           joinpath(first(Base.DEPOT_PATH), "gmtvtk_runtime", "deps", "build", name)])
		ok = try
			ccall(InteractiveGMT._fn(:gmtvtk_app_init), Cint, ()) == 1 && !isempty(libs) &&
				(_TEST_LIB[] = InteractiveGMT.Libdl.dlopen(first(libs)); true) &&
				ccall(InteractiveGMT.Libdl.dlsym(_TEST_LIB[], :gmtvtk_hide_windows_test),
				      Cint, (Cint,), 1) >= 0
		catch e
			@warn "tests: could not park the GUI windows off screen" exception = (e,)
			false
		end
		ok || @warn "tests: the GUI windows will be ON SCREEN this run"
	end
end

# PROGRESS. TestItemRunner says nothing while it runs, and a full run takes many minutes. The filter
# below sees every item before any runs, so it counts the ones that will run (N); the testset it is
# given is made once per file and once per item as each STARTS, so each start is reported here:
# a file as "── name", an item as "[k/N] elapsed  name". Printed to stdout and flushed at once, so a
# log being tailed shows where the run is.
const _PROGRESS_ITEMS = Set{String}()
const _PROGRESS_DONE = Ref(0)
const _PROGRESS_T0 = Ref(time())
function _progress_testset(name::AbstractString; verbose::Bool = false)
	if name in _PROGRESS_ITEMS
		_PROGRESS_DONE[] += 1
		t = round(Int, time() - _PROGRESS_T0[])
		println("[", _PROGRESS_DONE[], "/", length(_PROGRESS_ITEMS), "] ", t ÷ 60, "m", lpad(t % 60, 2, '0'), "s  ", name)
	else
		println("── ", name)
	end
	flush(stdout)
	return Test.DefaultTestSet(name; verbose = verbose)
end
function _progress_filter(ti)
	# DROPPED FOR NOW (user, 2026-10-03): the PngQuant module is not included in the package
	# (src/InteractiveGMT.jl, its include line commented out), so its unit tests cannot run.
	occursin("test-pngquant", ti.filename) && return false
	run = (_RUN_GUI || !(:gui in ti.tags)) && (_RUN_NET || !(:net in ti.tags)) &&
	      (isempty(_ONLY) || occursin(_ONLY, ti.name)) &&
	      (isempty(_ONLYFILE) || occursin(_ONLYFILE, ti.filename)) &&
	      (isempty(_ONLYTAG) || Symbol(_ONLYTAG) in ti.tags) &&
	      (isempty(_FROMFILE) || basename(ti.filename) >= _FROMFILE)
	run && push!(_PROGRESS_ITEMS, ti.name)
	return run
end

@run_package_tests verbose=true filter = _progress_filter testset = _progress_testset

# THE VERDICT ON THE WARNINGS. Every tool callback catches, logs "X FAILED: …" and returns 0, which
# is right for the GUI and blind for a test: an item that asserts `call(kv) == 0` for a refusal it
# WANTED cannot tell that refusal from the tool blowing up on something else entirely. That is how
# `Earth regions FAILED: Something went wrong when calling the module. GMT error number = 72` and
# `Interpolate FAILED: … error number = 72` rode along in green CI runs as ordinary warnings.
#
# InteractiveGMT._viewer_log_error is the ONE funnel every one of those messages passes through, so
# it sorts them (src/console.jl): a sentence the user can act on is the tool WORKING; a GMT C-level
# error or a raw Julia MethodError/BoundsError/… is a BUG. The second list is asserted here, over
# the whole run, so a disguised error can never be green again.
@testset "no internal tool failures (disguised errors)" begin
	bad = InteractiveGMT._internal_tool_errors()
	isempty(bad) || @error "Tools failed with internal errors, not with a refusal a user could " *
	                       "act on. Each of these is a bug:\n  " * join(bad, "\n  ")
	@test isempty(bad)
end

# NO MESSAGE BOX CAME UP. With the windows parked off screen a modal box can be neither seen nor
# pressed; the test DLL dismisses each one so the run cannot hang on it (gmtvtk_dismissed_dialogs_test)
# -- and every one it had to dismiss is a failure here, in the box's own words.
@testset "no dialog was raised that no one could see" begin
	p = _TEST_LIB[] == C_NULL ? C_NULL : InteractiveGMT.Libdl.dlsym(_TEST_LIB[], :gmtvtk_dismissed_dialogs_test; throw_error = false)
	if p !== nothing && p != C_NULL
		buf = zeros(UInt8, 1 << 16)
		n = ccall(p, Cint, (Ptr{UInt8}, Cint), buf, Cint(length(buf)))
		n == 0 || @error "Message boxes came up during the run (dismissed so it could not hang):\n  " *
		                 replace(String(buf[1:something(findfirst(==(0x00), buf), 1)-1]), "\n" => "\n  ")
		@test n == 0
	end
end

# ...and the wider rule, which is the one that stops a failure hiding: EVERY message in the sink got
# there because an exception was raised and a `catch` swallowed it. Each one is an ERROR. A test that
# provokes one CLAIMS it (IG._errored, src/console.jl) — takes it out of the sink and asserts the
# shape of the failure. What is left at the end of the run is an error nobody expected: it happened,
# the suite would otherwise have gone green over it, and it fails here instead.
# THE TIER ITSELF IS AN ASSERTION. A skipped tier is not a passing tier, and treating it as one is
# how a broken guarantee goes green: the "Nested grids" items are :gui (nestReflow lives in the DLL
# and needs a real Scene), so a plain `julia test/runtests.jl` ran ZERO of them and exited 0 — with
# the quantization rule provably broken, verified by mutating nestNearest to the historical outward
# rounding and watching the default run report `InteractiveGMT | None`.
#
# So: on a machine that CAN run the GUI tier — the library is built and present — skipping it fails
# here. A DLL-less checkout (CI, a fresh clone) still passes, which is the case the opt-in existed
# for; what is no longer possible is a developer with a working build getting a green suite that
# never touched the DLL. `INTERACTIVEGMT_TEST_NO_GUI=1` is the deliberate override for the rare case
# of wanting the unit tier alone on a build machine; it must be asked for, in writing.
@testset "the GUI tier was not silently skipped" begin
	waived = lowercase(strip(get(ENV, "INTERACTIVEGMT_TEST_NO_GUI", "0"), [' ', '"', '\''])) in
	         ("1", "true", "yes", "on") ||
	# ...and ALWAYS on CI, which decides tier per workflow and must not be second-guessed here.
	# ci.yml opts the tier in explicitly (Xvfb + INTERACTIVEGMT_TEST_GUI=1); ReusableTest.yml
	# deliberately runs the unit tier alone, on runners that DO have the rolling runtime in the
	# depot — so keying off "the library is present" fails those jobs for doing exactly what they
	# were written to do. The hole this guard exists to close is a DEVELOPER with a working local
	# build getting a green suite that never entered the DLL; that is not a CI situation.
	         get(ENV, "CI", "") != ""
	haslib = isfile(InteractiveGMT._LOCAL_LIB) || isfile(InteractiveGMT._SHARED_LIB)
	if !_RUN_GUI && haslib && !waived
		@error "The :gui tier was SKIPPED although gmtvtk is built and present. Those items are " *
		       "the only cover for everything that lives in the DLL (the nested-grid quantization " *
		       "rule among them), so this run proves nothing about it. Re-run with:\n" *
		       "    julia test/runtests.jl gui\n" *
		       "or set INTERACTIVEGMT_TEST_NO_GUI=1 to state that the unit tier alone was intended."
	end
	@test _RUN_GUI || !haslib || waived
end

@testset "no unclaimed errors" begin
	left = InteractiveGMT._tool_errors()
	isempty(left) || @error "Errors were raised, caught and never claimed by any test. Each of " *
	                        "these is a failure that would have passed unnoticed:\n  " * join(left, "\n  ")
	@test isempty(left)
end
