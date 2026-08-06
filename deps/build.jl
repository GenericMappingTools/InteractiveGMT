# deps/build.jl -- Pkg build hook: fetch prebuilt gmtvtk binaries from GitHub Releases.
#
# Two binaries, two release cadences:
#
#   * FULL runtime zip (gmtvtk.dll + bundled VTK/Qt/TBB + Qt plugins) -- changes rarely,
#     only when the VTK/Qt/TBB module set changes. Pinned by deps/RUNTIME_VERSION (a git
#     tag, e.g. "runtime-0.1"). Fetched on first install, and again whenever that pin MOVES.
#
#   * DLL-ONLY zip (just gmtvtk.dll) -- can change daily as the C++ side is edited. Lives at
#     a FIXED, reused release tag (DLL_TAG below); its one asset gets overwritten in place
#     (`gh release upload dll-latest gmtvtk-win64.zip --clobber`) -- no new tag per day.
#     Re-downloaded on every `Pkg.build("InteractiveGMT")`.
#
# THE CONTRACT BETWEEN THEM. The small zip's gmtvtk.dll must be loadable against the runtime the
# big zip installed. Break that -- link one VTK module the pinned bundle predates -- and every
# existing install gets a DLL that Windows cannot load at all, reported by Julia as a missing
# gmtvtk_* symbol that says nothing about the real cause. Three guards, none sufficient alone:
#
#   1. deps/check_dll_deps.ps1 -- run BEFORE uploading. Walks the dll's transitive imports and
#      subtracts the published bundle's file list; non-empty remainder = do not publish.
#   2. .full_runtime_installed stores the installed TAG (not a bare sentinel), so bumping
#      deps/RUNTIME_VERSION actually re-fetches the bundle on machines that already have one.
#      That is the fix for the real design hole: the marker used to be empty, making the pin
#      unenforceable and forcing new modules to hitch a ride in the dll-only zip.
#   3. .dll_requires -- the manifest generated at package time (deps/CMakeLists.txt), checked
#      after extraction here and again by src/libgmtvtk.jl when dlopen fails, so a shortfall is
#      reported as the missing MODULE names.
#
# A regular `Pkg.add`-installed (non-dev) package lives in a content-hashed folder
# (~/.julia/packages/InteractiveGMT/<hash>/) that gets a BRAND NEW <hash> on every single
# Pkg.update, even for a one-line .jl change unrelated to the C++ side. If the ~200 MB
# VTK/Qt/TBB runtime were extracted INTO that folder (as an earlier version of this file did),
# every update would silently re-download and re-extract the entire runtime again --
# unacceptable. Fix: extract the runtime into SHARED_ROOT, a location keyed off the Julia
# DEPOT itself (~/.julia), not off this ephemeral package folder -- the same physical spot
# survives every Pkg.update, `Pkg.add` or `Pkg.develop` alike, so the runtime is fetched once,
# ever, no matter how many times the package updates. src/libgmtvtk.jl looks in this same
# SHARED_ROOT (falling back to it only when there's no LOCAL deps/build/gmtvtk.dll -- i.e. a
# developer's own `deps/build.bat` build always wins first).
using Downloads

const REPO    = "GenericMappingTools/InteractiveGMT"
const DLL_TAG = "dll-latest"   # fixed tag; its one asset is re-uploaded in place, never retagged

const DEPS_DIR     = @__DIR__
const SHARED_ROOT  = joinpath(first(Base.DEPOT_PATH), "gmtvtk_runtime")   # survives every Pkg.update; zip paths (deps/build/...) are relative to here
# Holds the runtime TAG that is installed, not merely the fact that something is. It used to be an
# empty sentinel file, which made deps/RUNTIME_VERSION unenforceable: the full bundle was fetched
# once ever, so bumping the tag reached no existing install, and a gmtvtk.dll linking a VTK module
# newer than the installed bundle simply failed to load (LoadLibrary can't resolve it -> Julia
# reports a missing gmtvtk_* symbol, naming nothing useful). With the tag stored, a bump is
# detected and the newer bundle is fetched. Legacy empty marker = the tag that was pinned when it
# was written, i.e. treat as current and let the .dll_requires check below catch a real shortfall.
const MARKER       = joinpath(SHARED_ROOT, "deps", "build", ".full_runtime_installed")


function runtime_tag()
    f = joinpath(DEPS_DIR, "RUNTIME_VERSION")
    isfile(f) || error("deps/RUNTIME_VERSION missing -- can't tell which runtime release to fetch")
    String(strip(read(f, String)))
end

release_url(tag::String, asset::String) =
    "https://github.com/$REPO/releases/download/$tag/$asset"

# Windows 10 1803+/11 ships a real bsdtar (understands .zip) at System32\tar.exe. Called by
# FULL PATH, never bare `tar` -- a bare `tar` can resolve to Git/MSYS's GNU tar instead (whichever
# comes first on PATH), which cannot read ZIP at all and fails with a cryptic
# "does not look like a tar archive" / "Error exit delayed from previous errors".
const TAR = Sys.iswindows() ? joinpath(get(ENV, "SystemRoot", "C:\\Windows"), "System32", "tar.exe") : something(Sys.which("tar"), "tar")

# The full zip also contains Project.toml/data/src (for the standalone zip/NSIS user who isn't
# going through Julia Pkg at all) -- irrelevant here since Pkg already gave us those via git, and
# SHARED_ROOT only ever needs the binaries. Restrict extraction to deps/build/ so SHARED_ROOT
# doesn't waste disk space on a redundant copy of data/ and src/.
#
# Help > Check for Updates runs update!() -> Pkg.build IN the same running process that has
# gmtvtk.dll dlopen'd (in-process viewer, see CLAUDE.md). Windows won't let you overwrite the
# CONTENT of a DLL file that's currently mapped for execution -- but it WILL let you rename or
# delete that same file (the loader opens image files with FILE_SHARE_DELETE), which is the
# standard Windows self-update trick: displace the locked file, then create the new one fresh
# under the original name. The already-running process keeps using the orphaned old file quite
# happily; a future dlopen (next Julia session) picks up the new one.
#
# Every DLL the zip is about to overwrite gets this treatment, not just gmtvtk.dll: the dll-only
# zip also carries any VTK module that the pinned full runtime bundle predates (see the
# _GMTVTK_DLL_EXTRA_MODULES block in deps/CMakeLists.txt), and those are mapped for execution in
# this process exactly like gmtvtk.dll is. The list comes from the downloaded zip itself, so
# nothing else in deps/build/ (the ~200 MB bundle) is touched.
function _displace_locked_dll(dest::String, zip::String)
    names = try
        readlines(`$TAR -tf $zip`)
    catch e
        @warn "InteractiveGMT: couldn't list the zip -- falling back to displacing gmtvtk.dll only" exception=e
        ["deps/build/gmtvtk.dll"]
    end
    stamp = "$(getpid())-$(round(Int, time()))"
    for n in names
        endswith(lowercase(n), ".dll") || continue
        dll = joinpath(dest, replace(strip(n), '/' => Base.Filesystem.path_separator))
        isfile(dll) || continue
        try
            mv(dll, dll * ".old-$stamp"; force=true)
        catch e
            @warn "InteractiveGMT: couldn't displace an in-use dll -- update may fail" file=dll exception=e
        end
    end
end

# Sweep of orphaned .old-* files left behind by _displace_locked_dll in a PREVIOUS update (that
# process is gone by now, so these are almost always removable). A failure here is NOT silently
# swallowed -- if a stale file can't be removed, we @warn with the exact path and reason so a
# growing trail is visible instead of assumed away.
function _sweep_stale_dlls(dest::String)
    dir = joinpath(dest, "deps", "build")
    isdir(dir) || return
    for f in readdir(dir; join=true)
        if occursin(".old-", f)
            try
                rm(f; force=true)
            catch e
                @warn "InteractiveGMT: couldn't remove stale gmtvtk dll -- it will linger" file=f exception=e
            end
        end
    end
end

# GitHub API JSON is small/predictable enough here to scrape with a scoped regex, rather than
# add a JSON dependency (Project.toml deps are off-limits without explicit authorization). Returns
# the asset's "updated_at" as a freshness signature, or nothing if the API call fails (caller then
# falls back to fetching -- staying correct is more important than staying fast).
function _dll_asset_signature(tag::String, asset::String)
    api_url = "https://api.github.com/repos/$REPO/releases/tags/$tag"
    io = IOBuffer()
    try
        Downloads.download(api_url, io; headers=["User-Agent" => "InteractiveGMT.jl", "Accept" => "application/vnd.github+json"])
    catch
        return nothing
    end
    body = String(take!(io))
    m = match(Regex("\"name\"\\s*:\\s*\"" * asset * "\".*?\"updated_at\"\\s*:\\s*\"([^\"]+)\"", "s"), body)
    m === nothing ? nothing : m.captures[1]
end

function fetch_and_extract(url::String, dest::String)
    isfile(TAR) || error("$TAR not found -- cannot extract InteractiveGMT binaries")
    archive = joinpath(tempdir(), basename(url))
    @info "InteractiveGMT: downloading gmtvtk binaries" url
    try
        Downloads.download(url, archive)
    catch e
        error("failed to download $url -- has this asset been uploaded yet? ($e)")
    end
    mkpath(dest)
    if Sys.iswindows()
        _sweep_stale_dlls(dest)
        _displace_locked_dll(dest, archive)
        run(`$TAR -xf $archive -C $dest deps/build`)
    else
        run(`$TAR -xzf $archive -C $dest deps/build`)
    end
    rm(archive; force=true)
    return nothing
end

# The Desktop shortcut is NOT created here. `] dev` never runs this build hook, so it can't be the
# thing that makes the icon for a dev install -- InteractiveGMT's __init__ (_ensure_desktop_shortcut)
# owns that instead, firing on the first `using` for dev and add alike.

# The dll-only zip carries `.dll_requires`: every non-system DLL gmtvtk.dll imports, transitively
# (written at package time by deps/CMakeLists.txt). Returns the ones that are NOT on disk.
function _missing_modules()
    dir = joinpath(SHARED_ROOT, "deps", "build")
    man = joinpath(dir, ".dll_requires")
    isfile(man) || return String[]
    return filter(n -> !isfile(joinpath(dir, n)),
                  String.(filter(n -> !isempty(n) && !startswith(n, "#"), strip.(readlines(man)))))
end

# An install with NO manifest cannot be checked, so it must never be treated as proven-good: it
# predates .dll_requires, which is precisely the population that can be missing modules. Saying
# "nothing missing" there is how a broken machine skips its own repair -- the manifest arrives WITH
# the dll zip, so the cure is to fetch that zip, which is what returning false here triggers.
function _runtime_verified()
    isfile(joinpath(SHARED_ROOT, "deps", "build", ".dll_requires")) || return false
    return isempty(_missing_modules())
end

# SELF-HEAL, not merely diagnose. This is the last thing every build path runs, and it is what
# makes Help > Check for Updates enough on its own: whatever state the machine got into -- a
# marker that lies, a runtime bundle older than the dll, an interrupted earlier update -- if a
# module the dll needs is absent, the full runtime bundle is fetched right here and the viewer
# works after a restart. No manual download, no instructions to follow. A warning that told the
# user to go fix it themselves is not a fix; it is the same broken machine with better prose.
function _ensure_runtime_complete()
    miss = _missing_modules()
    isempty(miss) && return
    @warn "InteractiveGMT: the installed runtime is missing modules gmtvtk.dll needs -- repairing by re-fetching the full runtime bundle (~53 MB)." missing=join(miss, ", ")
    try
        fetch_and_extract(release_url(runtime_tag(), "iGMT-win64-full.zip"), SHARED_ROOT)
        write(MARKER, runtime_tag())
    catch e
        @error "InteractiveGMT: could not fetch the runtime bundle -- the viewer will not load." exception=(e,)
        return
    end
    still = _missing_modules()
    if isempty(still)
        @info "InteractiveGMT: runtime repaired -- all modules present." SHARED_ROOT
    else
        @error """InteractiveGMT: still missing modules after re-fetching the runtime bundle: $(join(still, ", ")).
                  The published $(runtime_tag()) bundle does not contain what this gmtvtk.dll needs -- please report it.""" SHARED_ROOT
    end
end

# "libstdc++.so.6.0.34" -> v"6.0.34". Anything that isn't a fully versioned real file (the bare
# SONAME symlink, a stray .igmt-backup) returns nothing, so callers can't compare junk.
function _libstdcxx_version(path::String)
    m = match(r"^libstdc\+\+\.so\.(\d+)\.(\d+)\.(\d+)$", basename(path))
    m === nothing ? nothing : VersionNumber(parse(Int, m[1]), parse(Int, m[2]), parse(Int, m[3]))
end

# The newest fully versioned libstdc++ real file in `dir`, or nothing when the directory has none.
function _newest_libstdcxx(dir::String)
    isdir(dir) || return nothing
    best, bestv = nothing, nothing
    for f in readdir(dir)
        v = _libstdcxx_version(f)
        v === nothing && continue
        if bestv === nothing || v > bestv
            best, bestv = joinpath(dir, f), v
        end
    end
    return best
end

_ld_preload_hint(bundled::String) =
    "Workaround without touching Julia:  LD_PRELOAD=$bundled julia"

# Conda's Qt 6.11 / VTK 9.6 / icu need GLIBCXX_3.4.34 + CXXABI_1.3.15 (GCC 14). Julia ships its OWN
# libstdc++ in lib/julia and its loader dlopens that copy at startup -- so by the time __init__
# reaches dlopen(libgmtvtk.so), the SONAME libstdc++.so.6 is already taken by Julia's older one, and
# the newer copy sitting right beside libgmtvtk.so (RUNPATH $ORIGIN) is never even looked at: the
# dynamic linker resolves a SONAME once per process. NOTHING done at runtime can undo that -- not
# dlopen order, not RTLD_GLOBAL, not LD_LIBRARY_PATH set from inside Julia; all three were measured
# to still fail with "version `CXXABI_1.3.15' not found".
#
# So fix it at build time, in the one place that can: install the bundle's libstdc++ INTO Julia's
# private lib dir. The direction is the safe one -- libstdc++ is backward compatible, so Julia's own
# LLVM (built against GLIBCXX_3.4.30) runs unchanged against 6.0.34, while the viewer gets the
# symbols it needs. The displaced original is kept as *.igmt-backup, never deleted. Idempotent: a
# private lib dir that is already >= the bundle's is left completely alone, which is also what makes
# a re-run after a juliaup upgrade (fresh Julia, old libstdc++ again) do the right thing.
function _fix_julia_libstdcxx()
    bundled = _newest_libstdcxx(joinpath(SHARED_ROOT, "deps", "build"))
    bundled === nothing && return nothing            # nothing to install from
    jdir = joinpath(Sys.BINDIR, Base.PRIVATE_LIBDIR)
    have = _newest_libstdcxx(jdir)
    if have === nothing
        # A distro/system Julia with no private copy: it uses the system libstdc++, which we must
        # not touch. Say what to do instead of silently leaving a viewer that cannot load.
        @info """InteractiveGMT: this Julia has no private libstdc++ -- the system one must provide
                 GLIBCXX_3.4.34. If `using InteractiveGMT` reports a missing CXXABI/GLIBCXX version,
                 $(_ld_preload_hint(bundled))"""
        return nothing
    end
    _libstdcxx_version(have) >= _libstdcxx_version(bundled) && return nothing   # already good
    dst = joinpath(jdir, basename(bundled))
    try
        backup = have * ".igmt-backup"
        isfile(backup) ? rm(have; force=true) : mv(have, backup)
        cp(bundled, dst; force=true, follow_symlinks=true)
        chmod(dst, 0o755)
        for link in ("libstdc++.so.6", "libstdc++.so")   # SONAME + linker name, both repointed
            p = joinpath(jdir, link)
            (islink(p) || isfile(p)) && rm(p; force=true)
            symlink(basename(dst), p)
        end
        @info """InteractiveGMT: replaced Julia's private libstdc++ ($(basename(have))) with the
                 viewer's newer $(basename(dst)); the original is kept beside it as
                 $(basename(have)).igmt-backup.""" jdir
    catch e
        @error """InteractiveGMT: could not install $(basename(bundled)) into $jdir -- the viewer
                  will fail to load with "version `CXXABI_1.3.15' not found".
                  $(_ld_preload_hint(bundled))""" exception=e
    end
    return nothing
end

function main_linux()
    want  = runtime_tag()
    asset = "iGMT-linux-x86_64-full.tar.gz"
    lib   = joinpath(SHARED_ROOT, "deps", "build", "libgmtvtk.so")
    sigf  = joinpath(SHARED_ROOT, "deps", "build", ".linux_release_sig")
    installed = isfile(MARKER) ? String(strip(read(MARKER, String))) : ""
    # There is ONE Linux asset and it is re-uploaded in place under the same tag (the tarball is the
    # whole runtime -- there is no separate rolling .so zip as on Windows). So the tag alone cannot
    # tell a machine that the bundle changed: matching MARKER used to mean "skip", and a re-upload
    # then reached nobody who already had it. Compare the release asset's updated_at as well, the
    # same freshness signal main() uses for the Windows dll zip.
    sig = _dll_asset_signature(want, asset)
    stale = !isfile(lib) || installed != want ||
            (sig !== nothing && (!isfile(sigf) || read(sigf, String) != sig))
    if stale
        # Extraction MERGES into whatever is already there, so a library the bundle policy dropped
        # (the GLVND set, a retired Qt plugin) would survive forever and keep being loaded in
        # preference to the host's -- the exact bug the policy exists to prevent. Clear the tree
        # first, guarded on the path so a wrong SHARED_ROOT can never delete anything else.
        # Unlinking a mapped .so is safe on Linux; a running viewer keeps the file it already has.
        dir = joinpath(SHARED_ROOT, "deps", "build")
        endswith(dir, joinpath("gmtvtk_runtime", "deps", "build")) && rm(dir; recursive=true, force=true)
        fetch_and_extract(release_url(want, asset), SHARED_ROOT)
        mkpath(dirname(MARKER))
        write(MARKER, want)
        sig !== nothing && write(sigf, sig)
    end
    _fix_julia_libstdcxx()
    @info "InteractiveGMT: Linux gmtvtk runtime installed" SHARED_ROOT
    return nothing
end

function main()
    installed = isfile(MARKER) ? String(strip(read(MARKER, String))) : ""
    want      = runtime_tag()
    if !isfile(MARKER)
        # First install EVER on this machine: full runtime bundle, pinned to a coarse,
        # rarely-bumped tag.
        fetch_and_extract(release_url(want, "iGMT-win64-full.zip"), SHARED_ROOT)
        write(MARKER, want)
    elseif !isempty(installed) && installed != want
        # The pinned runtime moved (a new VTK/Qt module set). This is the ONLY path that can
        # refresh the bundled VTK/Qt on a machine that already has one, so it must not be skipped
        # -- a stale bundle means a gmtvtk.dll that cannot load at all.
        @warn "InteractiveGMT: bundled VTK/Qt runtime is out of date ($installed -> $want) -- downloading the new bundle (~53 MB)."
        fetch_and_extract(release_url(want, "iGMT-win64-full.zip"), SHARED_ROOT)
        write(MARKER, want)
        # Fall through: the full bundle carries a gmtvtk.dll, but the rolling dll-latest asset is
        # normally newer, so still sync it below.
        rm(joinpath(SHARED_ROOT, "deps", "build", ".dll_release_sig"); force=true)
    end
    if isempty(installed) && isfile(MARKER)
        # Legacy empty sentinel from before the marker carried a tag: the bundle on disk is of
        # UNKNOWN vintage. Stamping it "current" and moving on was WRONG -- the full bundle is the
        # only place newer VTK modules ever come from, so skipping it left exactly these machines
        # broken however many times the dll-only zip was re-published. Unknown now counts as
        # stale: fetch the pinned bundle once, and the marker is honest from then on.
        @warn "InteractiveGMT: runtime bundle of unknown version -- fetching the pinned $want bundle once so it is known-complete."
        fetch_and_extract(release_url(want, "iGMT-win64-full.zip"), SHARED_ROOT)
        write(MARKER, want)
        rm(joinpath(SHARED_ROOT, "deps", "build", ".dll_release_sig"); force=true)
    end
    let
        # Every subsequent build: DLL only (~1 MB), always the same rolling tag/asset -- but only
        # actually re-downloaded when the release asset is newer than what we last synced.
        asset  = "gmtvtk-win64.zip"
        dll    = joinpath(SHARED_ROOT, "deps", "build", "gmtvtk.dll")
        sigf   = joinpath(SHARED_ROOT, "deps", "build", ".dll_release_sig")
        sig    = _dll_asset_signature(DLL_TAG, asset)
        # The signature short-circuit is a speed optimisation, never a reason to leave a broken
        # install broken: if a module is missing, re-fetch the dll zip regardless of what the
        # stored signature says, because that zip is what carries the manifest and the modules.
        if sig !== nothing && isfile(dll) && isfile(sigf) && read(sigf, String) == sig && _runtime_verified()
            @info "InteractiveGMT: gmtvtk.dll already up to date" SHARED_ROOT
            return nothing
        end
        fetch_and_extract(release_url(DLL_TAG, asset), SHARED_ROOT)
        sig !== nothing && write(sigf, sig)
    end
    _ensure_runtime_complete()
    @info "InteractiveGMT: gmtvtk binaries installed" SHARED_ROOT
end

if Sys.iswindows()
    main()
elseif Sys.islinux() && Sys.ARCH === :x86_64
    main_linux()
else
    @warn "InteractiveGMT binaries are not published for this platform" kernel=Sys.KERNEL arch=Sys.ARCH
end
