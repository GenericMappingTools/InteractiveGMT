# make_env.jl -- run by make_installer.ps1 on the BUILD machine, with the julia that gets bundled:
#
#   julia --startup-file=no make_env.jl <stage>
#
# JULIA_DEPOT_PATH points at a private build depot (deps\build\installer_depot, kept between builds so
# the downloads happen once) and PATH starts with the staged <stage>\gmt\bin (make_installer.ps1 sets
# both). Puts GMT.jl master and InteractiveGMT master, both as `] dev` git clones in <depot>\dev, into a
# fresh environment, and writes to <stage>\env:
#
#   Project.toml, Manifest.toml   the environment setup.jl instantiates at install time
#   igmt_commit.txt               the InteractiveGMT commit the Manifest was resolved against
#   gmt_commit.txt                the GMT.jl commit (the clone itself ships in the installer, ship_gmtjl.jl)
#
# GMT.jl lists GMT_jll, GDAL_jll, PROJ_jll, Leptonica_jll and LASzip_jll in [deps], but with a system
# GMT (the staged one) its deps/deps.jl takes those libraries from the GMT bin dir and never loads the
# JLLs. The clone's Project.toml has them taken out (src/gmtjl_strip.jl), so the resolve never adds
# them, and on the user's machine neither instantiate nor a later `] up` does: Pkg reads a dev'd
# package's dependencies from its own Project.toml. Only the installer's clone is edited.

using Pkg, TOML, LibGit2

include(joinpath(@__DIR__, "..", "..", "src", "gmtjl_strip.jl"))   # _GMTJL_DROP, _strip_gmtjl_project!

const STAGE  = abspath(ARGS[1])
const GMTBIN = joinpath(STAGE, "gmt", "bin")
const OUT    = joinpath(STAGE, "env")

const GMTJL_URL = "https://github.com/GenericMappingTools/GMT.jl"   # master: iGMT is developed against it
const IGMT_URL  = "https://github.com/GenericMappingTools/InteractiveGMT"

step(msg) = (println("\n==> ", msg); flush(stdout))

# GMT.jl's deps/build.jl takes the first `gmt` on PATH; when none runs it downloads and runs the
# official GMT installer instead. Make sure it is the staged one.
gmt = Sys.which("gmt")
(gmt !== nothing && normpath(dirname(gmt)) == normpath(GMTBIN)) ||
	error("`gmt` on PATH is $(repr(gmt)), not the staged $(GMTBIN)")

# Leptonica_jll and LASzip_jll are dropped too: the staged GMT must have its own copies
for lib in ("leptonica_w64.dll", "laszip_w64.dll")
	isfile(joinpath(GMTBIN, lib)) || error("no $lib in $GMTBIN: GMT.jl would need its JLL")
end

const ENVDIR = joinpath(first(DEPOT_PATH), "environments", "installer")
const DEVDIR = joinpath(first(DEPOT_PATH), "dev", "InteractiveGMT")
const GMTDEV = joinpath(first(DEPOT_PATH), "dev", "GMT")
for d in (ENVDIR, DEVDIR, GMTDEV)            # Pkg.develop(url=...) reuses an existing clone unchanged
	ispath(d) && chmod(d, 0o777; recursive=true)   # git leaves pack files read-only
	rm(d; force=true, recursive=true)
end
mkpath(ENVDIR)
Pkg.activate(ENVDIR)

step("Cloning GMT.jl (master) into $GMTDEV")
# Shallow: the clone ships in the installer, and update!() only ever fast-forwards it
const GMT_COMMIT = string(LibGit2.head_oid(LibGit2.clone(GMTJL_URL, GMTDEV; depth=1)))
println("GMT.jl ", GMT_COMMIT)
println("removed from GMT.jl's Project.toml: ", join(_strip_gmtjl_project!(joinpath(GMTDEV, "Project.toml")), ", "))
let p = TOML.parsefile(joinpath(GMTDEV, "Project.toml"))
	for n in _GMTJL_DROP
		(haskey(p["deps"], n) || haskey(get(p, "compat", Dict()), n)) && error("$n still in GMT.jl's Project.toml")
	end
end

step("Resolving GMT.jl + InteractiveGMT (master)")
Pkg.develop(path=GMTDEV)
withenv("JULIA_PKG_DEVDIR" => dirname(DEVDIR)) do
	Pkg.develop(url=IGMT_URL)
end
isdir(joinpath(DEVDIR, ".git")) || error("InteractiveGMT was not cloned into $DEVDIR")
const COMMIT = string(LibGit2.head_oid(LibGit2.GitRepo(DEVDIR)))
println("InteractiveGMT ", COMMIT)

# Make the dev paths relative. Here the env is <depot>\environments\installer, at install time it is
# <depot>\environments\v1.x: both see the clones at ../../dev/<name>. So the shipped files hold no
# build-machine path and setup.jl needs not rewrite them -- which it cannot do anyway: since Julia
# 1.13.1 the Manifest's project_hash covers the Project's [sources], so an edited path invalidates it.
const RELDEV = Dict("InteractiveGMT" => "../../dev/InteractiveGMT", "GMT" => "../../dev/GMT")
for (name, rel) in RELDEV
	normpath(joinpath(ENVDIR, rel)) == normpath(joinpath(first(DEPOT_PATH), "dev", name)) ||
		error("$rel does not lead to the $name clone")
end
let pf = joinpath(ENVDIR, "Project.toml"), p = TOML.parsefile(pf)
	src = get!(p, "sources", Dict{String,Any}())
	for (name, rel) in RELDEV
		src[name] = Dict("path" => rel)
	end
	open(io -> TOML.print(io, p; sorted=true), pf, "w")
end
Pkg.resolve()                      # rewrites the Manifest (paths, project_hash) from the edited Project

man = TOML.parsefile(joinpath(ENVDIR, "Manifest.toml"))
for n in _GMTJL_DROP
	haskey(man["deps"], n) && error("$n is in the Manifest: something besides GMT.jl needs it")
end
for (name, rel) in RELDEV
	mpath = get(man["deps"][name][1], "path", "")
	normpath(mpath) == normpath(rel) || error("Manifest path of $name is $(repr(mpath)), not $rel")
end

rm(OUT; force=true, recursive=true)
mkpath(OUT)
cp(joinpath(ENVDIR, "Project.toml"),  joinpath(OUT, "Project.toml"))
cp(joinpath(ENVDIR, "Manifest.toml"), joinpath(OUT, "Manifest.toml"))
write(joinpath(OUT, "igmt_commit.txt"), COMMIT * "\n")
write(joinpath(OUT, "gmt_commit.txt"), GMT_COMMIT * "\n")
println("\nwrote ", OUT, " (", length(man["deps"]), " packages)")
