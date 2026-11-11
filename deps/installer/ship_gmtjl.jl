# ship_gmtjl.jl -- run by make_installer.ps1 on the BUILD machine, after make_env.jl:
#
#   julia --startup-file=no ship_gmtjl.jl <stage>
#
# JULIA_DEPOT_PATH points at the build depot (deps\build\installer_depot), where make_env.jl cloned
# GMT.jl into dev\GMT (shallow, Project.toml without the JLLs). Copies that clone, .git included, to
#
#   <stage>\depot\dev\GMT
#
# which the installer unpacks as <INSTDIR>\depot\dev\GMT: the path the shipped Manifest gives GMT.jl
# (../../dev/GMT). So on the user's machine GMT.jl is never downloaded: setup.jl only builds it
# (deps.jl from the bundled GMT) and precompiles it into the install's own depot, and update!()
# fast-forwards the clone later. The build machine's deps\deps.jl is NOT shipped: it names this
# machine's GMT.

using TOML, LibGit2

include(joinpath(@__DIR__, "..", "..", "src", "gmtjl_strip.jl"))   # _GMTJL_DROP

const STAGE = abspath(ARGS[1])
const src   = joinpath(first(DEPOT_PATH), "dev", "GMT")
const want  = strip(read(joinpath(STAGE, "env", "gmt_commit.txt"), String))
isdir(joinpath(src, ".git")) || error("no GMT.jl clone at $src: run make_installer.ps1 without -SkipEnv")
let got = string(LibGit2.head_oid(LibGit2.GitRepo(src)))
	got == want || error("the GMT.jl clone is at $got, the Manifest was made with $want")
end
let p = TOML.parsefile(joinpath(src, "Project.toml"))
	for n in _GMTJL_DROP
		haskey(p["deps"], n) && error("$n is in $src\\Project.toml")
	end
end

const OUT = joinpath(STAGE, "depot")
if isdir(OUT)
	chmod(OUT, 0o777; recursive=true)       # git and Pkg leave files read-only
	rm(OUT; recursive=true)
end
dst = joinpath(OUT, "dev", "GMT")
mkpath(dirname(dst))
cp(src, dst)
chmod(dst, 0o777; recursive=true)           # setup.jl's GMT build writes deps\deps.jl in there
rm(joinpath(dst, "deps", "deps.jl"); force=true)
println("shipped GMT.jl ", want, " -> ", dst)
