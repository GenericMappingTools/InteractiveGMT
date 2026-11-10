# selfupdate.jl — update!() pulls the latest InteractiveGMT source in place and rebuilds the
# binaries, for a `] dev`-installed checkout ONLY.
#
# Why this exists: `] dev https://github.com/GenericMappingTools/InteractiveGMT` clones ONCE to
# a fixed, permanent directory (~/.julia/dev/InteractiveGMT by default) that never moves again --
# unlike a plain `Pkg.add`, which re-checks-out into a brand NEW content-hashed folder on every
# single `Pkg.update`. A fixed directory means a Desktop shortcut never goes stale. The one thing
# `dev` doesn't give you for free is an update mechanism: Pkg.update() deliberately skips dev'd
# packages (you're expected to manage their git state yourself). update!() is that missing piece
# — using Julia's BUNDLED LibGit2, not a system `git.exe`, so end users never need git installed.

using LibGit2
# (no `import Pkg`: the binary sync runs deps/build.jl directly, see below. Pkg appears in this
# file only as advice in the error text for a non-dev install.)

"""
    InteractiveGMT.update!()

Pull the latest InteractiveGMT source in place (fast-forward only) and rebuild the binaries.
Only works for a `] dev`-installed checkout — a plain `Pkg.add` install should use
`Pkg.update("InteractiveGMT")` instead.
"""
function update!()
	isdir(joinpath(_PKGROOT, ".git")) || error(
		"InteractiveGMT at $_PKGROOT isn't a git checkout -- update! only works for a " *
		"`] dev`-installed copy. For a plain `Pkg.add` install, use " *
		"Pkg.update(\"InteractiveGMT\") instead.")

	# A source pull that cannot fast-forward MUST NOT stop the binary sync. These are two
	# independent things: the .jl source comes from git, gmtvtk.dll + its VTK/Qt runtime come from
	# release assets. Letting a dirty working tree throw here used to skip Pkg.build entirely, so
	# "Check for updates" silently did NOTHING on exactly the machines that most needed the new
	# binaries -- the failure looked like the download being broken when it had never been tried.
	repo = LibGit2.GitRepo(_PKGROOT)
	try
		println("InteractiveGMT: fetching latest changes... ($_PKGROOT)")
		LibGit2.fetch(repo)
		before = LibGit2.head_oid(repo)
		if LibGit2.merge!(repo; fastforward=true)
			after = LibGit2.head_oid(repo)
			println(before == after ? "InteractiveGMT: source already up to date." :
			        "InteractiveGMT: source updated $(string(before)[1:8]) -> $(string(after)[1:8]).")
		else
			@warn "InteractiveGMT: couldn't fast-forward the source (local changes or diverged " *
			      "history) at $_PKGROOT -- keeping the current source and syncing the binaries anyway."
		end
	catch e
		@tool_error "InteractiveGMT: source update failed -- syncing the binaries anyway." exception=(e,)
	finally
		close(repo)
	end

	# gmtvtk.dll is a separately-rolling release asset (dll-latest, re-uploaded in place on its own
	# cadence) -- it can be newer than the last source commit, so its refresh is NOT gated behind
	# a source-diff check above. deps/build.jl always re-fetches it unconditionally; run it every
	# time update! runs, source-changed or not.
	#
	# RUN THE SCRIPT DIRECTLY, not through Pkg.build. Measured 2026-09-01: deps/build.jl does its
	# whole job (two GitHub API calls, and a ~1.3 MB download only when the asset actually changed)
	# in 1.4 s, while Pkg.build("InteractiveGMT") took 258 s for the same result -- it resolves the
	# environment and then AUTO-PRECOMPILES every package in it. That precompile is not work saved:
	# Julia invalidates the caches the git pull above just touched and rebuilds them at the next
	# `using` regardless, which is what the "restart Julia" line below already asks for -- and only
	# for what changed, instead of the whole environment.
	#
	# A subprocess rather than include(): build.jl is a script full of top-level consts (SHARED_ROOT,
	# LIB_NAME, main, ...) and include() evaluates at MODULE scope, so pulling it in here would
	# scatter those names through InteractiveGMT. It needs no environment resolution -- Downloads and
	# tar only -- so a bare julia is enough.
	println("InteractiveGMT: syncing gmtvtk binaries...")
	let script = joinpath(_PKGROOT, "deps", "build.jl")
		if isfile(script)
			try
				run(`$(Base.julia_cmd()) --startup-file=no --color=no $script`)
			catch e
				@tool_error "InteractiveGMT: the binary sync failed -- the source is updated, the \
				       binaries are unchanged." exception=(e,)
			end
		else
			@tool_error "InteractiveGMT: deps/build.jl not found at $script -- binaries not synced."
		end
	end
	println("InteractiveGMT: update complete. Restart i'GMT to use the new version.")
	return nothing
end

"""
    InteractiveGMT.update_gmtjl!()

Update GMT.jl (Help > Update GMT.jl). Two cases:

  * an iGMT Windows install (its depot holds the installer's marker, gmtjl_strip.jl): fast-forward
    the install's GMT.jl clone and take the JLLs its bundled GMT replaces out of it again;
  * anywhere else: `Pkg.update("GMT")` -- the registry GMT.jl, exactly as any Julia user updates it.
    A `] dev` GMT.jl checkout is the user's own and is not touched.
"""
function update_gmtjl!()
	if isfile(joinpath(first(DEPOT_PATH), _GMTJL_MARKER))
		_update_gmtjl()
	else
		f = Base.locate_package(Base.PkgId(Base.UUID("5752ebe1-31b9-557e-87aa-f909b540aa54"), "GMT"))
		f === nothing && (println("GMT.jl: not installed in this environment."); return nothing)
		dir = dirname(dirname(f))
		if !any(d -> startswith(lowercase(abspath(dir)), lowercase(abspath(joinpath(d, "packages")))), DEPOT_PATH)
			println("GMT.jl at $dir is a `] dev` checkout -- update it with git, not from here.")
			return nothing
		end
		println("GMT.jl: Pkg.update(\"GMT\")...")
		buf = IOBuffer()
		try
			run(pipeline(`$(Base.julia_cmd()) --startup-file=no --color=no -e "import Pkg; Pkg.update(\"GMT\")"`;
			             stdout=buf, stderr=buf))
			print(String(take!(buf)))
		catch e
			print(String(take!(buf)))
			@tool_error "GMT.jl: Pkg.update failed." exception=(e,)
			return nothing
		end
	end
	println("Restart i'GMT to use an updated GMT.jl.")
	return nothing
end

# GMT.jl of an iGMT Windows install (gmtjl_strip.jl): only in a depot that holds the installer's
# marker. A registry GMT.jl or a user's own `] dev` GMT.jl is never touched.
function _update_gmtjl()
	marker = joinpath(first(DEPOT_PATH), _GMTJL_MARKER)
	isfile(marker) || return nothing
	dir = strip(read(marker, String))
	pf  = joinpath(dir, "Project.toml")
	(isdir(joinpath(dir, ".git")) && isfile(pf)) ||
		(@tool_error "GMT.jl: $dir (named in $marker) is not a git checkout -- not updated."; return nothing)

	olddeps = Set(_gmtjl_project_deps(pf))
	repo = LibGit2.GitRepo(dir)
	before = after = LibGit2.head_oid(repo)
	try
		# The stripped Project.toml is a local edit: back to git's own, or the pull cannot fast-forward
		write(pf, LibGit2.rawcontent(LibGit2.GitBlob(repo, "HEAD:Project.toml")))
		println("GMT.jl: fetching latest changes... ($dir)")
		LibGit2.fetch(repo)
		if LibGit2.merge!(repo; fastforward=true)
			after = LibGit2.head_oid(repo)
			println(before == after ? "GMT.jl: already up to date." :
			        "GMT.jl: updated $(string(before)[1:8]) -> $(string(after)[1:8]).")
		else
			@warn "GMT.jl: couldn't fast-forward at $dir -- keeping the current version."
		end
	catch e
		@tool_error "GMT.jl: update failed -- keeping the current version." exception=(e,)
	finally
		close(repo)
		_strip_gmtjl_project!(pf)      # always, a failed pull included: the JLLs never stay in
	end
	before == after && return nothing

	# deps\deps.jl again, with the new build.jl, from the same GMT the old one names. build.jl runs
	# the first `gmt` on PATH, so that GMT's bin dir goes first -- for that process only.
	depsjl = joinpath(dir, "deps", "deps.jl")
	try
		m = Module()
		Base.include(m, depsjl)
		gmtbin = dirname(replace(Base.invokelatest(getproperty, m, :_libgmt), '/' => '\\'))
		bf = joinpath(dir, "deps", "build.jl")
		withenv("PATH" => gmtbin * ";" * get(ENV, "PATH", "")) do
			cd(() -> run(`$(Base.julia_cmd()) --startup-file=no --color=no $bf`), dirname(bf))
		end
		_backslash_depsjl!(depsjl)
	catch e
		@tool_error "GMT.jl: rebuilding deps.jl failed." exception=(e,)
	end

	# New dependencies in the new GMT.jl: the environment's Manifest must get them (the JLLs stay out,
	# they are no longer in GMT.jl's Project.toml).
	if Set(_gmtjl_project_deps(pf)) != olddeps
		println("GMT.jl: its dependencies changed, resolving the environment...")
		try
			run(`$(Base.julia_cmd()) --startup-file=no --color=no -e "import Pkg; Pkg.resolve()"`)
		catch e
			@tool_error "GMT.jl: resolving the environment failed." exception=(e,)
		end
	end
	return nothing
end
