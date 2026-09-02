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

@run_package_tests verbose=true filter = ti ->
	(_RUN_GUI || !(:gui in ti.tags)) && (_RUN_NET || !(:net in ti.tags)) &&
	(isempty(_ONLY) || occursin(_ONLY, ti.name)) &&
	(isempty(_ONLYFILE) || occursin(_ONLYFILE, ti.filename)) &&
	(isempty(_ONLYTAG) || Symbol(_ONLYTAG) in ti.tags)

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

# ...and the wider rule, which is the one that stops a failure hiding: EVERY message in the sink got
# there because an exception was raised and a `catch` swallowed it. Each one is an ERROR. A test that
# provokes one CLAIMS it (IG._errored, src/console.jl) — takes it out of the sink and asserts the
# shape of the failure. What is left at the end of the run is an error nobody expected: it happened,
# the suite would otherwise have gone green over it, and it fails here instead.
@testset "no unclaimed errors" begin
	left = InteractiveGMT._tool_errors()
	isempty(left) || @error "Errors were raised, caught and never claimed by any test. Each of " *
	                        "these is a failure that would have passed unnoticed:\n  " * join(left, "\n  ")
	@test isempty(left)
end
