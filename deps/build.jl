# deps/build.jl — Pkg build hook: fetch prebuilt gmtvtk binaries from GitHub Releases.
#
# Two binaries, two release cadences:
#
#   * FULL runtime zip (gmtvtk.dll + bundled VTK/Qt/TBB + Qt plugins) — changes rarely,
#     only when the VTK/Qt/TBB module set changes. Pinned by deps/RUNTIME_VERSION (a git
#     tag, e.g. "runtime-0.1"). Downloaded ONCE EVER (see SHARED_ROOT below).
#
#   * DLL-ONLY zip (just gmtvtk.dll) — can change daily as the C++ side is edited. Lives at
#     a FIXED, reused release tag (DLL_TAG below); its one asset gets overwritten in place
#     (`gh release upload dll-latest gmtvtk-win64.zip --clobber`) — no new tag per day.
#     Re-downloaded on every `Pkg.build("InteractiveGMT")`.
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
const MARKER       = joinpath(SHARED_ROOT, "deps", "build", ".full_runtime_installed")

function runtime_tag()
    f = joinpath(DEPS_DIR, "RUNTIME_VERSION")
    isfile(f) || error("deps/RUNTIME_VERSION missing — can't tell which runtime release to fetch")
    String(strip(read(f, String)))
end

release_url(tag::String, asset::String) =
    "https://github.com/$REPO/releases/download/$tag/$asset"

# Windows 10 1803+/11 ships a real bsdtar (understands .zip) at System32\tar.exe. Called by
# FULL PATH, never bare `tar` — a bare `tar` can resolve to Git/MSYS's GNU tar instead (whichever
# comes first on PATH), which cannot read ZIP at all and fails with a cryptic
# "does not look like a tar archive" / "Error exit delayed from previous errors".
const TAR = joinpath(get(ENV, "SystemRoot", "C:\\Windows"), "System32", "tar.exe")

# The full zip also contains Project.toml/data/src (for the standalone zip/NSIS user who isn't
# going through Julia Pkg at all) — irrelevant here since Pkg already gave us those via git, and
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
function _displace_locked_dll(dest::String)
    dll = joinpath(dest, "deps", "build", "gmtvtk.dll")
    isfile(dll) || return
    stale = dll * ".old-$(getpid())-$(round(Int, time()))"
    try
        mv(dll, stale; force=true)
    catch e
        @warn "InteractiveGMT: couldn't displace the in-use gmtvtk.dll -- update may fail" exception=e
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
    isfile(TAR) || error("$TAR not found — need Windows 10 1803+ (bsdtar) to unzip gmtvtk binaries")
    zip = joinpath(tempdir(), basename(url))
    @info "InteractiveGMT: downloading gmtvtk binaries" url
    try
        Downloads.download(url, zip)
    catch e
        error("failed to download $url — has this asset been uploaded yet? ($e)")
    end
    mkpath(dest)
    _sweep_stale_dlls(dest)
    _displace_locked_dll(dest)
    run(`$TAR -xf $zip -C $dest deps/build`)
    rm(zip; force=true)
end

# The Desktop shortcut is NOT created here. `] dev` never runs this build hook, so it can't be the
# thing that makes the icon for a dev install -- InteractiveGMT's __init__ (_ensure_desktop_shortcut)
# owns that instead, firing on the first `using` for dev and add alike.

function main()
    if !isfile(MARKER)
        # First install EVER on this machine: full runtime bundle, pinned to a coarse,
        # rarely-bumped tag. Never repeated after this, even across many future updates.
        fetch_and_extract(release_url(runtime_tag(), "iGMT-win64-full.zip"), SHARED_ROOT)
        touch(MARKER)
    else
        # Every subsequent build: DLL only (~1 MB), always the same rolling tag/asset -- but only
        # actually re-downloaded when the release asset is newer than what we last synced.
        asset  = "gmtvtk-win64.zip"
        dll    = joinpath(SHARED_ROOT, "deps", "build", "gmtvtk.dll")
        sigf   = joinpath(SHARED_ROOT, "deps", "build", ".dll_release_sig")
        sig    = _dll_asset_signature(DLL_TAG, asset)
        if sig !== nothing && isfile(dll) && isfile(sigf) && read(sigf, String) == sig
            @info "InteractiveGMT: gmtvtk.dll already up to date" SHARED_ROOT
            return nothing
        end
        fetch_and_extract(release_url(DLL_TAG, asset), SHARED_ROOT)
        sig !== nothing && write(sigf, sig)
    end
    @info "InteractiveGMT: gmtvtk binaries installed" SHARED_ROOT
end

main()
