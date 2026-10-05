# mbplugin.jl — Geophysics > MB-System > Install as plugin (Linux and macOS; Windows has it with GMT).
#
# Fetches MB-System's GMT supplement (mbsystem.so + the MB libraries it links: libmbio, libmbaux, ...)
# from the joa-quim/MB-System `mbsystem-latest` release and unpacks it to <iGMT root>/mbsystem. The
# archive is built by that repo's CI (`plugin` job in .github/workflows/build.yml) against GMT_jll,
# i.e. against the very libgmt/GDAL/PROJ/netCDF GMT.jl has loaded into this process, and its load is
# tested there through GMT.jl before it is published.
#
# ACTIVATION. GMT builds its plugin list once, in GMT_Create_Session (gmt_api.c,
# gmtapi_init_sharedlibs), from GMT_CUSTOM_LIBS as read from a gmt.conf; gmtlib_setparameter on a
# live session only stores the string and loads nothing. So the plugin is activated by restarting
# GMT.jl's session (GMT.gmt_restart) from a THROWAWAY directory whose gmt.conf GMT itself has just
# written with `gmtset` -- the current session's settings plus GMT_CUSTOM_LIBS -- and the directory is
# removed again. No gmt.conf is left anywhere: the user's ~/.gmt/gmt.conf is never touched.
# __init__ repeats this at every start while the plugin is installed.
#
# Once GMT has it, MBIO is the libmbio the plugin brought in -- found by _mbio_from_gmt (mbedit.jl)
# like any other MB-System supplement.

const _MBPLUGIN_REPO = "joa-quim/MB-System"
const _MBPLUGIN_TAG  = "mbsystem-latest"

# The release asset for this machine, "" where there is none (Windows: the plugin comes with GMT).
function _mbplugin_asset()::String
	Sys.islinux() && Sys.ARCH === :x86_64 && return "mbsystem-linux-x86_64.tar.gz"
	Sys.isapple() && return Sys.ARCH === :aarch64 ? "mbsystem-macos-arm64.tar.gz" : "mbsystem-macos-x86_64.tar.gz"
	return ""
end

_mbplugin_dir()::String = joinpath(_PKGROOT, "mbsystem")
_mbplugin_libname()::String = Sys.isapple() ? "mbsystem.dylib" : "mbsystem.so"
_mbplugin_lib(dir::String = _mbplugin_dir())::String = joinpath(dir, "lib", _mbplugin_libname())

# GMT_CUSTOM_LIBS is a comma-separated list: add `lib` to what is there, never drop an entry.
function _mbplugin_custom_libs(current::String, lib::String)::String
	entries = filter(!isempty, strip.(split(current, ',')))
	lib in entries && return join(entries, ',')
	return join(vcat(entries, lib), ',')
end

_mbplugin_has_module(api::Ptr{Cvoid})::Bool =
	api != C_NULL && GMT.GMT_Call_Module(api, "mbdefaults", GMT.GMT_MODULE_EXIST, C_NULL) == 0

# True once the plugin is installed, loaded and tested; the viewer hides "Install as plugin" then.
const _MBPLUGIN_READY = Ref(false)

# Has this process loaded library `name` from the installed plugin's directory? (dllist may report
# the resolved path, so the directory is matched both as given and resolved.)
function _mbplugin_loaded(name::String, dir::String = _mbplugin_dir())::Bool
	real = isdir(dir) ? realpath(dir) : dir
	return any(l -> (startswith(l, dir) || startswith(l, real)) && occursin(name, basename(l)), Libdl.dllist())
end

# Silent check (every start): GMT has the modules, and the plugin and its libmbio are the installed ones.
_mbplugin_loaded_ok()::Bool =
	_mbplugin_has_module(GMT.G_API[]) && _mbplugin_loaded("mbsystem") && _mbplugin_loaded("libmbio")

# Full self-test (after an install), the same one the CI plugin job runs: the silent check, plus an
# actual MB-System module run through GMT. mbdefaults needs no data and writes no file, and it goes
# through MBIO (mb_defaults). Straight through GMT_Call_Module: GMT.jl's gmt() appends its own
# output options (->@G...), which the MB modules reject.
function _mbplugin_selftest()::String
	_mbplugin_has_module(GMT.G_API[]) || return "GMT does not have the MB-System modules."
	_mbplugin_loaded("mbsystem") || return "The MB-System plugin loaded is not the one in $(_mbplugin_dir())."
	_mbplugin_loaded("libmbio") || return "MBIO (libmbio) was not loaded from $(_mbplugin_dir())."
	a = "-V"
	st = GC.@preserve a GMT.GMT_Call_Module(GMT.G_API[], "mbdefaults", GMT.GMT_MODULE_CMD, pointer(a))
	st == 0 || return "Running mbdefaults through GMT failed (status $st)."
	return ""
end

# Tell the viewer (when it is loaded) whether the plugin is ready.
function _mbplugin_push_ready()
	haskey(_LIB_FNS, :gmtvtk_set_mbplugin_ready) || return nothing   # viewer not loaded / not rebuilt yet
	ccall(_fn(:gmtvtk_set_mbplugin_ready), Cvoid, (Cint,), _MBPLUGIN_READY[] ? 1 : 0)
	return nothing
end

# Download `asset` from the release and unpack its mbsystem/ directory to `dest`, replacing what is
# there. Returns the plugin library's path.
function _mbplugin_fetch(asset::String, dest::String)::String
	url = "https://github.com/$_MBPLUGIN_REPO/releases/download/$_MBPLUGIN_TAG/$asset"
	tmp = mktempdir()
	try
		archive = joinpath(tmp, asset)
		println("Downloading $url")
		GMT.Downloads.download(url, archive)
		run(`tar -xzf $archive -C $tmp`)
		lib = _mbplugin_lib(joinpath(tmp, "mbsystem"))
		isfile(lib) || error("the archive has no mbsystem/lib/$(basename(lib))")
		rm(dest; recursive = true, force = true)
		mkpath(dirname(dest))
		mv(joinpath(tmp, "mbsystem"), dest)
	finally
		rm(tmp; recursive = true, force = true)
	end
	return _mbplugin_lib(dest)
end

# Make GMT.jl's session load `lib` (see ACTIVATION above). Returns true when GMT has the MB-System
# modules afterwards.
function _mbplugin_activate(lib::String)::Bool
	api = GMT.G_API[]
	api == C_NULL && return false
	_mbplugin_has_module(api) && return true
	value = _mbplugin_custom_libs(GMT.gmtlib_getparameter(api, "GMT_CUSTOM_LIBS"), lib)
	tmp = mktempdir()
	try
		cd(tmp) do
			arg = "GMT_CUSTOM_LIBS \"$value\""
			st = GC.@preserve arg GMT.GMT_Call_Module(api, "gmtset", GMT.GMT_MODULE_CMD, pointer(arg))
			(st == 0 && isfile("gmt.conf")) || error("GMT could not write the session settings (gmtset status $st)")
			GMT.gmt_restart()                  # the new session reads ./gmt.conf
		end
	finally
		rm(tmp; recursive = true, force = true)
	end
	return _mbplugin_has_module(GMT.G_API[])
end

# __init__: activate an installed plugin. Nothing at all to do when it is not installed.
function _mbplugin_activate_installed()
	lib = _mbplugin_lib()
	isfile(lib) || return nothing
	_mbplugin_activate(lib) ||
		@tool_error "InteractiveGMT: the MB-System plugin in $(dirname(dirname(lib))) did not load into GMT."
	_MBPLUGIN_READY[] = _mbplugin_loaded_ok()     # pushed to the viewer once it is loaded (__init__)
	return nothing
end

"""
    install_mbsystem_plugin()

Download MB-System's GMT supplement from the `mbsystem-latest` release of joa-quim/MB-System into
`<InteractiveGMT>/mbsystem` and load it into GMT. Re-running it replaces the installed copy.
Geophysics > MB-System > Install as plugin calls this.
"""
function install_mbsystem_plugin()
	asset = _mbplugin_asset()
	isempty(asset) && error("There is no MB-System plugin download for this system ($(Sys.KERNEL) $(Sys.ARCH)).")
	lib = _mbplugin_fetch(asset, _mbplugin_dir())
	println("Installed in $(_mbplugin_dir())")
	_MBPLUGIN_READY[] = false
	_mbplugin_push_ready()
	_mbplugin_activate(lib) || error("The MB-System plugin was installed but GMT did not load it ($lib).")
	println("GMT has loaded the MB-System modules.")
	if isdefined(@__MODULE__, :_mbio_from_gmt)        # mbedit.jl, an optional include
		mbio = _mbio_from_gmt()
		println(isempty(mbio) ? "MBIO was not found in the process." : "MBIO: $mbio")
		_push_mbio_hint()          # the viewer may have been told "no MBIO" before the plugin existed
	end
	why = _mbplugin_selftest()
	isempty(why) || error("The MB-System plugin was installed but failed its test: $why")
	_MBPLUGIN_READY[] = true
	_mbplugin_push_ready()
	println("MB-System plugin installed and tested: GMT loaded it and ran mbdefaults through MBIO.")
	return nothing
end
