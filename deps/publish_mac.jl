# publish_mac.jl -- put the macOS binaries on the GitHub releases page.
#
# WHY THIS EXISTS. Windows and Linux are built on a machine that exists (deps/build.bat,
# deps/build.sh) and published from it (deps/publish_linux.sh). There is no Mac on that desk, so the
# CI runner IS the build machine: .github/workflows/MacBinaries.yml builds both architectures and
# leaves the two archives as WORKFLOW ARTIFACTS. An artifact lives at a per-run URL that changes with
# every build and expires, so it is not something a user can be pointed at -- it has to be moved to
# the two fixed release tags the installer reads. That move is what this script does.
#
# The workflow can also upload them itself (a manual `workflow_dispatch` with publish=rolling|full,
# i.e. `gh workflow run MacBinaries.yml -f publish=rolling`), but only for a run you start by hand
# with that choice made up front. This script needs none of that: it takes the artifacts of a run
# that ALREADY happened -- the automatic push build -- and publishes those.
#
# THE TWO STREAMS, same contract as every other platform (deps/PACKAGING.md, top of deps/build.jl):
#   * gmtvtk-macos-<arch>.tar.gz     -> the fixed `dll-latest` tag. libgmtvtk.dylib + its
#     `.dylib_requires` manifest; overwritten in place on every rebuild (the light update path).
#   * iGMT-macos-<arch>-full.tar.gz  -> the tag in deps/RUNTIME_VERSION. The whole VTK/Qt/TBB
#     bundle; re-uploaded only when the module set changes.
#
# USE (from the repo root, `gh` authenticated with write access to the repo):
#   julia deps/publish_mac.jl                 # rolling library, both architectures
#   julia deps/publish_mac.jl --full          # the runtime bundle too (the rare one)
#   julia deps/publish_mac.jl --arch arm64    # one architecture only
#   julia deps/publish_mac.jl --run 33347252202   # a specific run instead of the newest green one
#   julia deps/publish_mac.jl --dry           # download + verify, upload nothing
#
# In a session: `include("deps/publish_mac.jl"); publish_mac(full=true)`.

module PublishMac

const REPO     = "GenericMappingTools/InteractiveGMT"
const WORKFLOW = "MacBinaries.yml"
const DLL_TAG  = "dll-latest"          # fixed tag; must match DLL_TAG in deps/build.jl
const ARCHES   = ("arm64", "x86_64")   # the two matrix jobs of MacBinaries.yml
const DEPS_DIR = @__DIR__

function gh()
	p = something(Sys.which("gh"), Sys.which("gh.exe"), Sys.which("gh.cmd"), "")
	isempty(p) && error("`gh` (the GitHub CLI) is not on PATH -- it is what talks to the releases page")
	return p
end

# Run gh and give back its stdout. gh's own message is the useful one when it fails, so it is let
# through to the console instead of being swallowed into an exception.
function _gh(args::Vector{String})
	out = IOBuffer()
	p = run(pipeline(Cmd([gh(), args...]); stdout=out), wait=false)
	wait(p)
	success(p) || error("gh $(join(args, ' ')) failed")
	return String(take!(out))
end

# The newest SUCCESSFUL MacBinaries run on the default branch. This is the whole point of the
# script: the run id (and with it the artifact URL) is different every build, so it is looked up,
# never written down.
function latest_run(; branch::String="master")::Int
	# The successful runs are picked out with gh's own jq rather than `--status success`, which older
	# gh releases (2.23, the one on this desk) do not have on `run list`.
	jq = raw"""[.[] | select(.conclusion=="success")][0] | "\(.databaseId) \(.headSha[0:7]) \(.createdAt)" """
	out = strip(_gh(["run", "list", "--repo", REPO, "--workflow", WORKFLOW, "--branch", branch,
	                 "--limit", "30", "--json", "databaseId,conclusion,headSha,createdAt", "--jq", jq]))
	(isempty(out) || startswith(out, "null")) &&
		error("no successful $WORKFLOW run found on '$branch' -- has the build run yet?")
	f = split(out)
	println("  run $(f[1])  commit $(length(f) > 1 ? f[2] : "?")  $(length(f) > 2 ? f[3] : "")")
	return parse(Int, f[1])
end

# The tag the full bundle belongs to. Same file the installer reads, so the two can never disagree.
function runtime_tag()::String
	f = joinpath(DEPS_DIR, "RUNTIME_VERSION")
	isfile(f) || error("deps/RUNTIME_VERSION missing -- can't tell which runtime release to upload to")
	t = strip(read(f, String))
	isempty(t) && error("deps/RUNTIME_VERSION is empty")
	return t
end

# One architecture's artifact, unpacked into `dir`. `gh run download` unzips it for us, so what
# lands there are the two .tar.gz files the workflow staged.
function fetch_artifact(runid::Int, arch::String, dir::String)::String
	out = joinpath(dir, arch)
	mkpath(out)
	println("  downloading artifact gmtvtk-macos-$arch ...")
	_gh(["run", "download", string(runid), "--repo", REPO, "--name", "gmtvtk-macos-$arch", "--dir", out])
	return out
end

# NEVER upload an archive without looking inside it first. The installer extracts `deps/build` out of
# these, so an archive whose members do not live under that prefix installs nothing while reporting
# success. Returns the member list so the caller can also say how big the thing is.
function verify(archive::String)
	isfile(archive) || error("$(basename(archive)) is not in the artifact -- did the workflow change?")
	names = readlines(Cmd([something(Sys.which("tar"), "tar"), "-tzf", archive]))
	isempty(names) && error("$(basename(archive)) is empty")
	bad = filter(n -> !startswith(n, "deps/build"), names)
	isempty(bad) || error("$(basename(archive)) has members outside deps/build: $(first(bad, 3))")
	any(n -> occursin("libgmtvtk.dylib", n), names) ||
		error("$(basename(archive)) carries no libgmtvtk.dylib")
	println("    $(basename(archive)): $(length(names)) members, $(round(filesize(archive)/2^20, digits=1)) MB  OK")
	return names
end

# Upload one asset to one tag, creating the release the first time (same shape as publish_linux.sh).
function publish_asset(tag::String, archive::String, title::String, notes::String; dry::Bool=false)
	if dry
		println("    DRY: would upload $(basename(archive)) -> $tag")
		return nothing
	end
	exists = success(run(pipeline(Cmd([gh(), "release", "view", tag, "--repo", REPO]);
	                              stdout=devnull, stderr=devnull), wait=true))
	if exists
		_gh(["release", "upload", tag, archive, "--repo", REPO, "--clobber"])
	else
		_gh(["release", "create", tag, archive, "--repo", REPO, "--title", title, "--notes", notes])
	end
	println("    uploaded $(basename(archive)) -> $tag")
	return nothing
end

"""
    publish_mac(; run=0, arches=("arm64","x86_64"), rolling=true, full=false, dry=false, branch="master")

Fetch the macOS archives built by CI and put them on the releases page.

`run` is a MacBinaries run id; `0` (the default) looks up the newest successful one on `branch`.
`rolling` uploads `gmtvtk-macos-<arch>.tar.gz` to the fixed `dll-latest` tag, `full` uploads
`iGMT-macos-<arch>-full.tar.gz` to the tag in `deps/RUNTIME_VERSION` (the ~200 MB bundle -- only
when the VTK/Qt module set changed). Both archives are verified before anything is uploaded, and
`dry=true` stops right after that.
"""
function publish_mac(; run::Int=0, arches=ARCHES, rolling::Bool=true, full::Bool=false,
                     dry::Bool=false, branch::String="master")
	(rolling || full) || error("nothing asked for: set rolling=true and/or full=true")
	println("macOS binaries -> $REPO")
	runid = run == 0 ? latest_run(; branch=branch) : run
	tag   = full ? runtime_tag() : ""
	tmp   = mktempdir()
	try
		for arch in arches
			println("  --- $arch ---")
			dir = fetch_artifact(runid, String(arch), tmp)
			lib  = joinpath(dir, "gmtvtk-macos-$arch.tar.gz")
			bund = joinpath(dir, "iGMT-macos-$arch-full.tar.gz")
			rolling && verify(lib)
			full    && verify(bund)
			rolling && publish_asset(DLL_TAG, lib, "gmtvtk_rolling_library",
			                         "Always_the_latest_gmtvtk_build"; dry=dry)
			full    && publish_asset(tag, bund, "gmtvtk_runtime_$(replace(tag, "runtime-" => ""))",
			                         "VTK_Qt_TBB_runtime_bundles"; dry=dry)
		end
	finally
		rm(tmp; recursive=true, force=true)
	end
	println(dry ? "dry run -- nothing was uploaded" : "done")
	return nothing
end

end # module

using .PublishMac: publish_mac

# Command line: only when this file is RUN, not when it is included for its function.
if abspath(PROGRAM_FILE) == @__FILE__
	local kw = Dict{Symbol,Any}(:rolling => true, :full => false, :dry => false)
	local i = 1
	while i <= length(ARGS)
		a = ARGS[i]
		if a == "--full"
			kw[:full] = true
		elseif a == "--only-full"
			kw[:full] = true;  kw[:rolling] = false
		elseif a == "--dry"
			kw[:dry] = true
		elseif a == "--arch"
			kw[:arches] = (ARGS[i+1],);  i += 1
		elseif a == "--run"
			kw[:run] = parse(Int, ARGS[i+1]);  i += 1
		elseif a == "--branch"
			kw[:branch] = ARGS[i+1];  i += 1
		else
			error("unknown option '$a' -- see the header of this file for the usage")
		end
		i += 1
	end
	publish_mac(; kw...)
end
