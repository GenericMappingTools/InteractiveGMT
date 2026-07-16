# deps/build.jl — Pkg build hook: fetch prebuilt gmtvtk binaries from GitHub Releases.
#
# Two binaries, two release cadences:
#
#   * FULL runtime zip (gmtvtk.dll + bundled VTK/Qt/TBB + Qt plugins) — changes rarely,
#     only when the VTK/Qt/TBB module set changes. Pinned by deps/RUNTIME_VERSION (a git
#     tag, e.g. "runtime-0.1"). Downloaded ONCE, on first install (no marker in deps/build/).
#
#   * DLL-ONLY zip (just gmtvtk.dll) — can change daily as the C++ side is edited. Lives at
#     a FIXED, reused release tag (DLL_TAG below); its one asset gets overwritten in place
#     (`gh release upload dll-latest gmtvtk-win64.zip --clobber`) — no new tag per day.
#     Re-downloaded on every `Pkg.build("InteractiveGMT")`.

using Downloads

const REPO    = "GenericMappingTools/InteractiveGMT"
const DLL_TAG = "dll-latest"   # fixed tag; its one asset is re-uploaded in place, never retagged

const DEPS_DIR = @__DIR__
const PKG_ROOT = normpath(joinpath(DEPS_DIR, ".."))   # zip paths are relative to here (deps/build/..., src/..., data/...)
const MARKER   = joinpath(DEPS_DIR, "build", ".full_runtime_installed")

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
    run(`$TAR -xf $zip -C $dest`)
    rm(zip; force=true)
end

function main()
    if !isfile(MARKER)
        # First install: full runtime bundle, pinned to a coarse, rarely-bumped tag.
        fetch_and_extract(release_url(runtime_tag(), "iGMT-win64-full.zip"), PKG_ROOT)
        touch(MARKER)
    else
        # Update: DLL only, always the same rolling tag/asset.
        fetch_and_extract(release_url(DLL_TAG, "gmtvtk-win64.zip"), PKG_ROOT)
    end
    @info "InteractiveGMT: gmtvtk binaries installed"
end

main()
