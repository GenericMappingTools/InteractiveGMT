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
import Pkg

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

	repo = LibGit2.GitRepo(_PKGROOT)
	try
		println("InteractiveGMT: fetching latest changes... ($_PKGROOT)")
		LibGit2.fetch(repo)
		before = LibGit2.head_oid(repo)
		ok = LibGit2.merge!(repo; fastforward=true)
		ok || error("InteractiveGMT: local changes or diverged history -- couldn't fast-forward. " *
		            "This checkout is a normal git repo at $_PKGROOT; resolve manually (e.g. `git status`).")
		after = LibGit2.head_oid(repo)
		if before == after
			println("InteractiveGMT: already up to date.")
			return nothing
		end
		println("InteractiveGMT: updated $(string(before)[1:8]) -> $(string(after)[1:8]).")
	finally
		close(repo)
	end

	println("InteractiveGMT: rebuilding binaries...")
	Pkg.build("InteractiveGMT")
	println("InteractiveGMT: update complete. Restart Julia to use the new version.")
	return nothing
end
