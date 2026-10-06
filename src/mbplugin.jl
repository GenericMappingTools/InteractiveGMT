# mbplugin.jl — Geophysics > MB-System > Install as plugin (Linux and macOS; Windows when GMT.jl runs
# GMT_jll's GMT -- an installed Windows GMT has it already). Once installed, every start looks for a
# newer one on the release (INSTALLED_AT vs the asset's updated_at) and the menu then offers
# "Update the plugin".
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

# Does this system get the plugin from here at all? Linux and macOS always. Windows only when GMT.jl
# runs GMT_jll's GMT (GMT.isJLL): an installed Windows GMT brings its own MB-System supplement.
_mbplugin_applies()::Bool = !Sys.iswindows() || GMT.isJLL

# The release asset for this machine, "" where there is none.
function _mbplugin_asset()::String
	Sys.islinux() && Sys.ARCH === :x86_64 && return "mbsystem-linux-x86_64.tar.gz"
	Sys.isapple() && return Sys.ARCH === :aarch64 ? "mbsystem-macos-arm64.tar.gz" : "mbsystem-macos-x86_64.tar.gz"
	Sys.iswindows() && GMT.isJLL && Sys.ARCH === :x86_64 && return "mbsystem-windows-x86_64.tar.gz"
	return ""
end

_mbplugin_dir()::String = joinpath(_PKGROOT, "mbsystem")
_mbplugin_libname()::String = Sys.isapple() ? "mbsystem.dylib" : Sys.iswindows() ? "mbsystem.dll" : "mbsystem.so"
_mbplugin_lib(dir::String = _mbplugin_dir())::String = joinpath(dir, "lib", _mbplugin_libname())
# The library an ARCHIVE carries, by its asset name (it need not be this system's).
_mbplugin_libname(asset::String)::String =
	occursin("macos", asset) ? "mbsystem.dylib" : occursin("windows", asset) ? "mbsystem.dll" : "mbsystem.so"

# WINDOWS FINDS A PLUGIN'S OWN DLLS ONLY THROUGH PATH. GMT loads the plugin with a plain LoadLibrary
# (gmt_sharedlibs.c), and Windows then looks for mbsystem.dll's dependencies (libmbio.dll, ...) in the
# executable's directory, the system directories and PATH -- never beside mbsystem.dll. So its lib/
# goes on this process's PATH before GMT is asked to load it. (Linux and macOS: $ORIGIN /
# @loader_path in the libraries themselves.)
function _mbplugin_path_dir(dir::String)
	Sys.iswindows() || return nothing
	parts = split(get(ENV, "PATH", ""), ';')
	any(p -> !isempty(p) && normpath(p) == normpath(dir), parts) || (ENV["PATH"] = dir * ";" * get(ENV, "PATH", ""))
	return nothing
end

# WHEN WAS THE INSTALLED COPY DOWNLOADED. Written at install time (the download moment, UTC), and
# compared with the release asset's own updated_at: an asset uploaded after it is a newer plugin.
# The libraries' own file times cannot serve -- tar restores the times they had on the build machine.
_mbplugin_stamp(dir::String = _mbplugin_dir())::String = joinpath(dir, "INSTALLED_AT")

function _mbplugin_local_time(dir::String = _mbplugin_dir())::Union{Nothing,GMT.Dates.DateTime}
	f = _mbplugin_stamp(dir)
	isfile(f) || return nothing
	return tryparse(GMT.Dates.DateTime, strip(read(f, String)), GMT.Dates.dateformat"yyyy-mm-ddTHH:MM:SS")
end

# The release asset's updated_at (UTC), from GitHub's API; nothing when it cannot be had.
function _mbplugin_remote_time(asset::String)::Union{Nothing,GMT.Dates.DateTime}
	io = IOBuffer()
	GMT.Downloads.download("https://api.github.com/repos/$_MBPLUGIN_REPO/releases/tags/$_MBPLUGIN_TAG", io;
	                       headers = ["Accept" => "application/vnd.github+json"], timeout = 20)
	return _mbplugin_asset_time(String(take!(io)), asset)
end

# The updated_at of asset `asset` in a GitHub release JSON: the first one after its "name".
function _mbplugin_asset_time(json::String, asset::String)::Union{Nothing,GMT.Dates.DateTime}
	m = findfirst(Regex("\"name\"\\s*:\\s*\"" * replace(asset, "." => "\\.") * "\""), json)
	m === nothing && return nothing
	u = match(r"\"updated_at\"\s*:\s*\"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)Z\"", json, last(m))
	u === nothing && return nothing
	return GMT.Dates.DateTime(u.captures[1])
end

# Is a newer plugin than the installed one on the release? Set by the check below, pushed with the
# ready state.
const _MBPLUGIN_UPDATE = Ref(false)

function _mbplugin_check_update()
	asset = _mbplugin_asset()
	(isempty(asset) || !isfile(_mbplugin_lib())) && return nothing
	remote = _mbplugin_remote_time(asset)
	remote === nothing && return nothing
	loc = _mbplugin_local_time()
	_MBPLUGIN_UPDATE[] = loc === nothing || remote > loc   # no stamp (an older install): offer it
	_mbplugin_push_ready()
	return nothing
end

# At start, once the first window is up: look for a newer plugin, in the background. Nothing at all
# when the plugin is not installed. A failure (no network) is silent: the menu then just says installed.
function _mbplugin_schedule_update_check()
	(_mbplugin_applies() && isfile(_mbplugin_lib())) || return nothing
	Timer(1.0; interval = 1.0) do t
		_PUMP[] === nothing && return                   # no window on screen yet
		close(t)
		@async try
			_mbplugin_check_update()
		catch e
			@debug "InteractiveGMT: MB-System plugin update check failed (harmless)" exception = (e,)
		end
	end
	return nothing
end

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

# Tell the viewer (when it is loaded) whether the menu offers the plugin here, and its state:
# 0 = "Install as plugin", 1 = installed and current, 2 = installed, "Update the plugin".
function _mbplugin_push_ready()
	haskey(_LIB_FNS, :gmtvtk_set_mbplugin_ready) || return nothing   # viewer not loaded / not rebuilt yet
	haskey(_LIB_FNS, :gmtvtk_set_mbplugin_offered) &&
		ccall(_fn(:gmtvtk_set_mbplugin_offered), Cvoid, (Cint,), _mbplugin_applies() ? 1 : 0)
	state = !_MBPLUGIN_READY[] ? 0 : _MBPLUGIN_UPDATE[] ? 2 : 1
	ccall(_fn(:gmtvtk_set_mbplugin_ready), Cvoid, (Cint,), state)
	return nothing
end

# Clear `dest` for a new copy. A library THIS process has loaded (the plugin being updated) cannot be
# deleted on Windows, but it can be renamed: it is moved aside as name.old-<pid>-<time> and removed
# by the next update (selfupdate's _displace_locked_dll, same Windows fact). Elsewhere it just goes.
function _mbplugin_clear(dest::String)
	isdir(dest) || return nothing
	for (root, _, files) in walkdir(dest; topdown = false), f in files
		p = joinpath(root, f)
		occursin(".old-", f) && (try rm(p; force = true) catch end; continue)   # a previous update's leftovers
		try
			rm(p; force = true)
		catch
			mv(p, p * ".old-$(getpid())-$(round(Int, time()))")
		end
	end
	return nothing
end

# Download `asset` from the release and unpack its mbsystem/ directory to `dest`, replacing what is
# there, and stamp the install time (INSTALLED_AT, UTC). Returns the plugin library's path.
function _mbplugin_fetch(asset::String, dest::String)::String
	url = "https://github.com/$_MBPLUGIN_REPO/releases/download/$_MBPLUGIN_TAG/$asset"
	tmp = mktempdir()
	try
		archive = joinpath(tmp, asset)
		println("Downloading $url")
		when = GMT.Dates.now(GMT.Dates.UTC)
		GMT.Downloads.download(url, archive)
		run(`tar -xzf $archive -C $tmp`)
		src = joinpath(tmp, "mbsystem")
		libname = _mbplugin_libname(asset)
		isfile(joinpath(src, "lib", libname)) || error("the archive has no mbsystem/lib/$libname")
		_mbplugin_clear(dest)
		for (root, _, files) in walkdir(src), f in files
			to = joinpath(dest, relpath(joinpath(root, f), src))
			mkpath(dirname(to))
			cp(joinpath(root, f), to; force = true, follow_symlinks = false)   # libmbio.so -> libmbio.so.0 stays a link
		end
		write(_mbplugin_stamp(dest), GMT.Dates.format(when, GMT.Dates.dateformat"yyyy-mm-ddTHH:MM:SS"))
	finally
		rm(tmp; recursive = true, force = true)
	end
	return joinpath(dest, "lib", _mbplugin_libname(asset))
end

# Make GMT.jl's session load `lib` (see ACTIVATION above). Returns true when GMT has the MB-System
# modules afterwards.
function _mbplugin_activate(lib::String)::Bool
	api = GMT.G_API[]
	api == C_NULL && return false
	_mbplugin_has_module(api) && return true
	_mbplugin_path_dir(dirname(lib))                   # Windows: its own DLLs, through PATH
	# Forward slashes on Windows: the value is written into gmt.conf, where a backslash is no path separator.
	value = _mbplugin_custom_libs(GMT.gmtlib_getparameter(api, "GMT_CUSTOM_LIBS"), replace(lib, '\\' => '/'))
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
	isempty(asset) && error("There is no MB-System plugin download for this system ($(Sys.KERNEL) $(Sys.ARCH)" *
	                        (Sys.iswindows() ? ": on Windows the plugin comes with GMT, unless GMT.jl uses GMT_jll" : "") * ").")
	# An UPDATE: this process already has the old copy loaded, and keeps it until it ends (GMT loads a
	# plugin once per session, and Windows will not unload it from under GMT). The new copy is the one
	# loaded from the next start.
	updating = _mbplugin_has_module(GMT.G_API[]) && _mbplugin_loaded("mbsystem")
	lib = _mbplugin_fetch(asset, _mbplugin_dir())
	println("Installed in $(_mbplugin_dir())")
	_MBPLUGIN_UPDATE[] = false
	if updating
		println("The plugin was updated. This session keeps the version it started with; the new one is loaded the next time iGMT starts.")
		_mbplugin_push_ready()
		return nothing
	end
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
