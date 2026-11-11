# setup.jl -- run ONCE by the Windows installer (igmt.nsi), with the bundled julia:
#
#   julia --startup-file=no setup.jl <INSTDIR>
#
# JULIA_DEPOT_PATH already points at <INSTDIR>\depot (the installer sets it). This sets the rest of
# the environment, installs GMT.jl + InteractiveGMT in the private depot, precompiles them at their
# final path, and writes <INSTDIR>\igmt_bundle.ini so the launcher hands every later julia the
# same environment (launcher.c, bundle_load).
#
# The environment comes ready-made in <INSTDIR>\env (make_env.jl, on the build machine): a Manifest
# without the JLLs the bundled GMT replaces, so they are never downloaded nor precompiled here.
# InteractiveGMT is a git clone in <depot>\dev\InteractiveGMT (a `] dev` checkout, which is what
# selfupdate.jl works on), at the commit that Manifest was resolved against.

using Pkg, TOML, LibGit2

include(joinpath(@__DIR__, "gmtjl_strip.jl"))   # _GMTJL_DROP, _GMTJL_MARKER, _backslash_depsjl!

const INST     = abspath(ARGS[1])

# Everything this script and the programs it runs print goes to the installer's details pane AND to
# <INSTDIR>\setup.log, a file whose text can be read and copied after the installer has closed.
# stdout/stderr are redirected at the OS level, so the julia/gmt/build processes run() starts write
# into the same pipe; a task copies the pipe to both places, and atexit drains it -- after an
# uncaught error too, whose message Julia prints before running the atexit hooks.
const SETUP_LOG   = open(joinpath(INST, "setup.log"), "w")
const CONSOLE_OUT = stdout
const CONSOLE_ERR = stderr
const TEE         = Pipe()
Base.link_pipe!(TEE; reader_supports_async=true, writer_supports_async=true)
redirect_stdout(TEE.in)
redirect_stderr(TEE.in)
const TEE_TASK = @async while !eof(TEE.out)
	b = readavailable(TEE.out)
	write(CONSOLE_OUT, b); flush(CONSOLE_OUT)
	write(SETUP_LOG, b);   flush(SETUP_LOG)
end
atexit() do
	redirect_stdout(CONSOLE_OUT)
	redirect_stderr(CONSOLE_ERR)
	close(TEE.in)
	wait(TEE_TASK)
	close(SETUP_LOG)
end

const GMTBIN   = joinpath(INST, "gmt", "bin")
const GMTSHARE = joinpath(INST, "gmt", "share")
const JULIA    = joinpath(Sys.BINDIR, "julia.exe")

const IGMT_URL  = "https://github.com/GenericMappingTools/InteractiveGMT"   # GMT.jl: pinned in env\Manifest.toml

const BUNDLE_ENV = [
	"GMT_SHAREDIR" => GMTSHARE,
	"GDAL_DATA"    => joinpath(GMTSHARE, "GDAL_DATA"),
	"PROJ_DATA"    => joinpath(GMTSHARE, "proj"),
	"PROJ_LIB"     => joinpath(GMTSHARE, "proj"),    # PROJ < 9.1 reads this name
]

step(msg) = (println("\n==> ", msg); flush(stdout))

# ~/.gmt/igmt_launcher.ini is shared by every iGMT on this machine and names the julia a launcher
# without a bundle beside it uses. Left as an older install wrote it (julia=...\Julia-1.10\...,
# plus the root= that launchers built before 2026-11-10 still read), any launcher that is not
# this bundle's own -- e.g. one a later precompile's `igmt --install-shortcut` puts the Desktop
# icon on -- would start that other julia on that other iGMT. So it is written UP FRONT, before
# anything here can start a launcher, and again at the end. No root=: a launcher finds its own.
function write_launcher_ini()
	d = joinpath(get(ENV, "USERPROFILE", homedir()), ".gmt")     # the launcher's home_dir()
	mkpath(d)
	open(joinpath(d, "igmt_launcher.ini"), "w") do io
		println(io, "# Written by the iGMT installer (setup.jl). Rewritten by `igmt --install-shortcut`")
		println(io, "# on every precompile of InteractiveGMT.")
		println(io, "julia=", JULIA)
	end
end

for (k, v) in BUNDLE_ENV
	ENV[k] = v
end
ENV["PATH"] = GMTBIN * ";" * ENV["PATH"]
ENV["JULIA_PKG_PRECOMPILE_AUTO"] = "0"    # one precompile, at the end, with the launcher held back

# GMT.jl's deps/build.jl takes the first `gmt` on PATH; when none runs it downloads and runs the
# official GMT installer instead. Make sure it is ours before it gets the chance.
step("Checking the bundled GMT")
gmt = joinpath(GMTBIN, "gmt.exe")
isfile(gmt) || error("the bundled GMT is missing: $gmt")
println("GMT ", readchomp(`$gmt --version`))

step("Pointing ~/.gmt/igmt_launcher.ini at this bundle's julia")
write_launcher_ini()

const DEPOT  = first(DEPOT_PATH)
const ENVDIR = joinpath(DEPOT, "environments", "v$(VERSION.major).$(VERSION.minor)")   # the default env
const ROOT   = joinpath(DEPOT, "dev", "InteractiveGMT")
const COMMIT = strip(read(joinpath(INST, "env", "igmt_commit.txt"), String))

# <INSTDIR>\igmt_bundle.ini is what makes <INSTDIR>\igmt.exe run THIS install -- its julia, its
# depot, its iGMT, its GMT -- instead of whatever julia is first on PATH and ~/.julia. Everything in
# it is known now, from INSTDIR alone, so it is written NOW, before any step below that can fail:
# written last, a failed setup left igmt.exe running another installation entirely.
step("Writing igmt_bundle.ini")
open(joinpath(INST, "igmt_bundle.ini"), "w") do io
	println(io, "# Written by the iGMT installer (setup.jl), read by igmt.exe beside it.")
	println(io, "julia=", JULIA)
	println(io, "root=", ROOT)
	println(io, "env.JULIA_DEPOT_PATH=", ENV["JULIA_DEPOT_PATH"])
	for (k, v) in BUNDLE_ENV
		println(io, "env.", k, "=", v)
	end
end

# GMT loads its plugins (gmt\bin\gmt_plugins\mbsystem.dll, ...) by full path, and Windows looks for
# a plugin's own DLLs (mbio.dll, mbaux.dll, ... in gmt\bin) only in the standard places -- not in
# gmt\bin. Rather than putting gmt\bin on PATH, every julia on this depot (igmt.exe's, julia.bat's)
# adds it as the process's DLL directory at startup. That process only; PATH is not touched.
step("Writing the depot's startup.jl")
mkpath(joinpath(DEPOT, "config"))
open(joinpath(DEPOT, "config", "startup.jl"), "w") do io
	println(io, "# Written by the iGMT installer (setup.jl): GMT's plugins find their DLLs in gmt\\bin.")
	println(io, "Sys.iswindows() && ccall((:SetDllDirectoryW, \"kernel32\"), stdcall, Cint, (Cwstring,), ", repr(GMTBIN), ")")
end

step("Cloning InteractiveGMT into $ROOT")
# Installing over an earlier install: its clone is replaced. ROOT is inside this install's own
# depot, so nothing outside the install is touched. chmod first: git leaves its pack files
# read-only, which a plain rm cannot delete on Windows.
if ispath(ROOT)
	println("replacing the existing ", ROOT)
	chmod(ROOT, 0o777; recursive=true)
	rm(ROOT; force=true, recursive=true)
end
repo = LibGit2.clone(IGMT_URL, ROOT)
# master is put at the commit the Manifest was made for; a later selfupdate fast-forwards it.
LibGit2.reset!(repo, LibGit2.GitHash(COMMIT), LibGit2.Consts.RESET_HARD)
println("InteractiveGMT ", COMMIT)

step("Instantiating the environment")
mkpath(ENVDIR)
# Copied as they are: the Manifest's InteractiveGMT path is relative (../../dev/InteractiveGMT, i.e.
# ROOT) and its project_hash covers the Project's [sources], so neither file may be edited here.
for f in ("Project.toml", "Manifest.toml")
	cp(joinpath(INST, "env", f), joinpath(ENVDIR, f); force=true)
end
man = TOML.parsefile(joinpath(ENVDIR, "Manifest.toml"))
normpath(joinpath(ENVDIR, man["deps"]["InteractiveGMT"][1]["path"])) == normpath(ROOT) ||
	error("the Manifest does not place InteractiveGMT at $ROOT")
Pkg.activate(ENVDIR)
# No Pkg builds: Pkg runs a deps/build.jl in a sandbox made from that package's OWN Project.toml, and
# GMT.jl's lists the JLLs -- the sandbox would download them all (~1.5 GB) right after instantiate
# left them out. Both build scripts only need stdlib, so they are run directly, the way Pkg runs them
# (from their deps dir) but in this environment.
Pkg.instantiate(allow_build=false)
const GMT_UUID  = Base.UUID("5752ebe1-31b9-557e-87aa-f909b540aa54")
const GMT_DIR   = dirname(dirname(Base.locate_package(Base.PkgId(GMT_UUID, "GMT"))))
# GMT.jl MUST be the clone the installer shipped (depot\dev\GMT, ship_gmtjl.jl), without the JLLs
let shipped = joinpath(DEPOT, "dev", "GMT")
	isdir(joinpath(shipped, ".git")) || error("the installer's GMT.jl is missing: $shipped")
	normpath(GMT_DIR) == normpath(shipped) || error("GMT.jl loads from $GMT_DIR, not the shipped $shipped")
	p = TOML.parsefile(joinpath(shipped, "Project.toml"))
	for n in _GMTJL_DROP
		haskey(p["deps"], n) && error("$n is in the shipped GMT.jl's Project.toml")
	end
	println("GMT.jl: ", GMT_DIR, " (shipped)")
	# Tells update!() (InteractiveGMT, selfupdate.jl) that this depot's GMT.jl is this clone: it
	# fast-forwards it and strips its Project.toml again. Without this file it never touches GMT.jl.
	write(joinpath(DEPOT, _GMTJL_MARKER), shipped * "\n")
end
for (name, dir) in ("GMT" => GMT_DIR, "InteractiveGMT" => ROOT)   # GMT first: deps.jl from the bundled `gmt`
	step("Building $name")
	bf = joinpath(dir, "deps", "build.jl")
	# GMT.jl's build.jl finds GMT by running `gmt` from PATH: only this step sees the bundled one first
	cd(() -> run(`$JULIA --startup-file=no --history-file=no $bf`), dirname(bf))
end
# deps.jl must name the libraries with '\' (see _backslash_depsjl!)
let f = joinpath(GMT_DIR, "deps", "deps.jl")
	_backslash_depsjl!(f)
	println(read(f, String))
end
# A re-resolve would have put back the JLLs the shipped Manifest leaves out
sort(collect(keys(TOML.parsefile(joinpath(ENVDIR, "Manifest.toml"))["deps"]))) == sort(collect(keys(man["deps"]))) ||
	error("Pkg changed the shipped Manifest: the packages are not the ones make_env.jl resolved")
let got = filter(d -> endswith(d, "_jll") && !haskey(man["deps"], d), readdir(joinpath(DEPOT, "packages")))
	isempty(got) || error("packages left out of the Manifest were installed anyway: ", join(got, ", "))
end

gmtdeps = joinpath(GMT_DIR, "deps", "deps.jl")
depstxt = lowercase(replace(read(gmtdeps, String), "\\\\" => "/"))
occursin(lowercase(replace(GMTBIN, '\\' => '/')), depstxt) ||
	error("GMT.jl's deps.jl does not point at the bundled GMT:\n" * read(gmtdeps, String))
# A JLL taken out of the Manifest cannot be loaded: its library must then come from the bundled GMT
for (jll, lib) in ("Leptonica_jll" => "leptonica_w64.dll", "LASzip_jll" => "laszip_w64.dll")
	(haskey(man["deps"], jll) || occursin(lib, depstxt)) ||
		error("$jll is not installed and GMT.jl's deps.jl has no $lib:\n" * read(gmtdeps, String))
end

# InteractiveGMT's precompile runs `igmt --install-shortcut`, which makes an "iGMT" desktop icon and
# rewrites ~/.gmt/igmt_launcher.ini for a launcher that knows nothing of this bundle. The installer
# makes its own icon, so the copies Pkg.build put in the depot are held back while precompiling.
held = String[]
for (dir, _, files) in walkdir(joinpath(INST, "depot")), f in files
	f == "igmt.exe" || continue
	p = joinpath(dir, f)
	mv(p, p * ".hold"; force=true)
	push!(held, p)
end

step("Precompiling (several minutes)")
try
	Pkg.precompile()
	# Loaded the way the launcher starts it (`julia -t auto iview_app.jl`: no --startup-file=no, no
	# --project, since the clone has no Manifest), still with the launchers held back: if this
	# julia finds anything left to compile, it compiles it HERE, not on the user's first launch --
	# where the precompile's `igmt --install-shortcut` would re-point the Desktop icon.
	step("Loading InteractiveGMT")
	run(`$JULIA -t auto -e "using InteractiveGMT"`)
finally
	# ONE launcher per install: <INSTDIR>\igmt.exe. The copies the runtime release unpacked into the
	# depot are deleted, not restored.
	for p in held
		rm(p * ".hold"; force=true)
	end
end

write_launcher_ini()             # again: nothing run above may leave another julia in it
println("\niGMT installed: ", ROOT)
