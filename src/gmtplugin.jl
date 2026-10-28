# gmtplugin.jl — Geophysics > MB-System / GMTSAR > Install as plugin: a GMT supplement from GitHub,
# loaded into GMT.jl's GMT. ONE mechanism for every plugin (the SACRED LAW: same operation, same
# function); a plugin is only its description, a GmtPlugin below. Linux and macOS; Windows when GMT.jl
# runs GMT_jll's GMT (the archives are built against GMT_jll). Once installed, every start looks for a
# newer one on the release (INSTALLED_AT vs the asset's updated_at) and the menu then offers
# "Update the plugin".
#
# Each plugin is fetched from its repository's rolling `<name>-latest` release and unpacked to
# <iGMT root>/<name>:
#   MB-System  joa-quim/MB-System  mbsystem-latest  mbsystem.so + the MB libraries it links (libmbio, ...)
#   GMTSAR     joa-quim/gmtsar     gmtsar-latest    gmtsar.so (+ share/gmtsar: filters, PRM templates)
# The archives are built by those repos' CI (`plugin` / `plugin-windows` jobs of
# .github/workflows/build.yml) against GMT_jll, i.e. against the very libgmt/GDAL/PROJ/netCDF GMT.jl
# has loaded into this process, and their load is tested there through GMT.jl before they are published.
#
# ACTIVATION. GMT builds its plugin list once, in GMT_Create_Session (gmt_api.c,
# gmtapi_init_sharedlibs), from GMT_CUSTOM_LIBS as read from a gmt.conf; gmtlib_setparameter on a
# live session only stores the string and loads nothing. So a plugin is activated by restarting
# GMT.jl's session (GMT.gmt_restart) from a THROWAWAY directory whose gmt.conf GMT itself has just
# written with `gmtset` -- the current session's settings plus GMT_CUSTOM_LIBS -- and the directory is
# removed again. No gmt.conf is left anywhere: the user's ~/.gmt/gmt.conf is never touched.
# __init__ repeats this at every start for every installed plugin.
#
# Once GMT has MB-System's, MBIO is the libmbio the plugin brought in -- found by _mbio_from_gmt
# (mbedit.jl) like any other MB-System supplement.

struct GmtPlugin
	id::Cint                     # the viewer's index (GMTVTK_PLUGIN_* in deps/src/30_app.cpp)
	title::String                # "MB-System": what the menu and the messages call it
	name::String                 # "mbsystem": the release asset prefix, <iGMT>/<name>, lib/<name>.so
	repo::String                 # GitHub repository of the `<name>-latest` release
	winext::String               # ".zip" or ".tar.gz": the extension of the Windows archive
	probe::String                # a module GMT has once the plugin is loaded
	companions::Vector{String}   # other libraries of the archive that must be loaded from it
	run::Function                # (api) -> "" or why it failed: an actual module run through GMT
	ran::String                  # what `run` did, for the success message
	installed::Function          # () -> nothing, after a fresh install and activation
	ready::Base.RefValue{Bool}   # installed, loaded and tested
	update::Base.RefValue{Bool}  # a newer one is on the release
end

_plugin_tag(p::GmtPlugin)::String = p.name * "-latest"

# Run module `m` with the command line `a`, straight through GMT_Call_Module: GMT.jl's gmt() appends
# its own output options (->@G...), which plugin modules reject.
_plugin_call(api::Ptr{Cvoid}, m::String, a::String) = GC.@preserve a GMT.GMT_Call_Module(api, m, GMT.GMT_MODULE_CMD, pointer(a))

# MB-System: mbdefaults needs no data and writes no file, and it goes through MBIO (mb_defaults).
function _mbsystem_run(api::Ptr{Cvoid})::String
	st = _plugin_call(api, "mbdefaults", "-V")
	return st == 0 ? "" : "Running mbdefaults through GMT failed (status $st)."
end

# GMTSAR: nearest_grid fills the NaN node of a small grid GMT writes, through GMT's own grid I/O.
function _gmtsar_run(api::Ptr{Cvoid})::String
	tmp = mktempdir()
	try
		return cd(tmp) do
			st = _plugin_call(api, "grdmath", "-R0/4/0/4 -I1 X Y ADD X 2 SUB ABS Y 2 SUB ABS ADD 0 NAN 0 MUL ADD = in.grd")
			st == 0 || return "GMT could not write the test grid (grdmath status $st)."
			st = _plugin_call(api, "nearest_grid", "in.grd -Gout.grd")
			(st == 0 && isfile("out.grd")) || return "Running nearest_grid through GMT failed (status $st)."
			return ""
		end
	finally
		rm(tmp; recursive = true, force = true)
	end
end

function _mbsystem_installed()
	if isdefined(@__MODULE__, :_mbio_from_gmt)        # mbedit.jl, an optional include
		mbio = _mbio_from_gmt()
		println(isempty(mbio) ? "MBIO was not found in the process." : "MBIO: $mbio")
		_push_mbio_hint()          # the viewer may have been told "no MBIO" before the plugin existed
	end
	return nothing
end

const MBSYSTEM_PLUGIN = GmtPlugin(0, "MB-System", "mbsystem", "joa-quim/MB-System", ".zip", "mbdefaults", ["libmbio"],
                                  _mbsystem_run, "ran mbdefaults through MBIO", _mbsystem_installed, Ref(false), Ref(false))
const GMTSAR_PLUGIN   = GmtPlugin(1, "GMTSAR", "gmtsar", "joa-quim/gmtsar", ".tar.gz", "nearest_grid", String[],
                                  _gmtsar_run, "ran nearest_grid on a grid", () -> nothing, Ref(false), Ref(false))
const _PLUGINS = (MBSYSTEM_PLUGIN, GMTSAR_PLUGIN)

# Does this system get the plugins from here at all? Linux and macOS always. Windows only when GMT.jl
# runs GMT_jll's GMT (GMT.isJLL): the archives are built against it.
_plugin_applies()::Bool = !Sys.iswindows() || GMT.isJLL

# The release asset of `p` for this machine, "" where there is none.
function _plugin_asset(p::GmtPlugin)::String
	Sys.islinux() && Sys.ARCH === :x86_64 && return "$(p.name)-linux-x86_64.tar.gz"
	Sys.isapple() && return Sys.ARCH === :aarch64 ? "$(p.name)-macos-arm64.tar.gz" : "$(p.name)-macos-x86_64.tar.gz"
	Sys.iswindows() && GMT.isJLL && Sys.ARCH === :x86_64 && return "$(p.name)-windows-x86_64$(p.winext)"
	return ""
end

_plugin_dir(p::GmtPlugin)::String = joinpath(_PKGROOT, p.name)
# The plugin library: <name>.so / .dylib / .dll, never lib<name>.* -- GMT takes the plugin's tag (and
# so its <tag>_module_* functions) from the file name.
_plugin_libname(p::GmtPlugin)::String = p.name * (Sys.isapple() ? ".dylib" : Sys.iswindows() ? ".dll" : ".so")
_plugin_lib(p::GmtPlugin, dir::String = _plugin_dir(p))::String = joinpath(dir, "lib", _plugin_libname(p))
# The library an ARCHIVE carries, by its asset name (it need not be this system's).
_plugin_libname(p::GmtPlugin, asset::String)::String =
	p.name * (occursin("macos", asset) ? ".dylib" : occursin("windows", asset) ? ".dll" : ".so")

# WINDOWS FINDS A PLUGIN'S OWN DLLS ONLY THROUGH PATH. GMT loads the plugin with a plain LoadLibrary
# (gmt_sharedlibs.c), and Windows then looks for its dependencies (libmbio.dll, ...) in the
# executable's directory, the system directories and PATH -- never beside the plugin. So its lib/
# goes on this process's PATH before GMT is asked to load it. (Linux and macOS: $ORIGIN /
# @loader_path in the libraries themselves.)
function _plugin_path_dir(dir::String)
	Sys.iswindows() || return nothing
	parts = split(get(ENV, "PATH", ""), ';')
	any(p -> !isempty(p) && normpath(p) == normpath(dir), parts) || (ENV["PATH"] = dir * ";" * get(ENV, "PATH", ""))
	return nothing
end

# WHEN WAS THE INSTALLED COPY DOWNLOADED. Written at install time (the download moment, UTC), and
# compared with the release asset's own updated_at: an asset uploaded after it is a newer plugin.
# The libraries' own file times cannot serve -- tar restores the times they had on the build machine.
_plugin_stamp(dir::String)::String = joinpath(dir, "INSTALLED_AT")

function _plugin_local_time(dir::String)::Union{Nothing,GMT.Dates.DateTime}
	f = _plugin_stamp(dir)
	isfile(f) || return nothing
	return tryparse(GMT.Dates.DateTime, strip(read(f, String)), GMT.Dates.dateformat"yyyy-mm-ddTHH:MM:SS")
end

# The start-up update checks (plugins here, VectorCraft/PhotoCraft in craftapps.jl) download on a
# background task, and Julia 1.10's Downloads CRASHES the process at exit (ACCESS VIOLATION in
# curl_multi_cleanup) when a transfer is still in flight then. So the atexit hook WAITS for a check
# still running (each download has a 20 s timeout) before Downloads' own cleanup runs. Nothing else:
# while the session runs, a check is the same plain download it always was -- an `interrupt` Event
# on it made Aquamoto's < > hold freeze on one layer and jump ten (2026-10-26).
const _BG_DL_TASKS = Task[]

function _bg_check(f::Function)
	isempty(_BG_DL_TASKS) && atexit(_bg_stop_checks)
	push!(_BG_DL_TASKS, @async f())
	return nothing
end

function _bg_stop_checks()
	foreach(t -> timedwait(() -> istaskdone(t), 25.0), _BG_DL_TASKS)
	return nothing
end

# The release asset's updated_at (UTC), from GitHub's API; nothing when it cannot be had.
function _plugin_remote_time(p::GmtPlugin, asset::String)::Union{Nothing,GMT.Dates.DateTime}
	io = IOBuffer()
	GMT.Downloads.download("https://api.github.com/repos/$(p.repo)/releases/tags/$(_plugin_tag(p))", io;
	                       headers = ["Accept" => "application/vnd.github+json"], timeout = 20)
	return _plugin_asset_time(String(take!(io)), asset)
end

# The updated_at of asset `asset` in a GitHub release JSON: the first one after its "name".
function _plugin_asset_time(json::String, asset::String)::Union{Nothing,GMT.Dates.DateTime}
	m = findfirst(Regex("\"name\"\\s*:\\s*\"" * replace(asset, "." => "\\.") * "\""), json)
	m === nothing && return nothing
	u = match(r"\"updated_at\"\s*:\s*\"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)Z\"", json, last(m))
	u === nothing && return nothing
	return GMT.Dates.DateTime(u.captures[1])
end

# Is a newer `p` than the installed one on the release? Sets p.update, pushed with the ready state.
function _plugin_check_update(p::GmtPlugin)
	asset = _plugin_asset(p)
	(isempty(asset) || !isfile(_plugin_lib(p))) && return nothing
	remote = _plugin_remote_time(p, asset)
	remote === nothing && return nothing
	loc = _plugin_local_time(_plugin_dir(p))
	p.update[] = loc === nothing || remote > loc   # no stamp (an older install): offer it
	_plugin_push_ready(p)
	return nothing
end

# At start, once the first window is up: look for newer plugins, in the background. Nothing at all
# when none is installed. A failure (no network) is silent: the menu then just says installed.
function _plugin_schedule_update_check()
	installed = filter(p -> isfile(_plugin_lib(p)), collect(_PLUGINS))
	(_plugin_applies() && !isempty(installed)) || return nothing
	Timer(1.0; interval = 1.0) do t
		_PUMP[] === nothing && return                   # no window on screen yet
		close(t)
		_bg_check() do
			for p in installed
				try
					_plugin_check_update(p)
				catch e
					@debug "InteractiveGMT: $(p.title) plugin update check failed (harmless)" exception = (e,)
				end
			end
		end
	end
	return nothing
end

# GMT_CUSTOM_LIBS is a comma-separated list: add `lib` to what is there, never drop an entry.
function _plugin_custom_libs(current::String, lib::String)::String
	entries = filter(!isempty, strip.(split(current, ',')))
	lib in entries && return join(entries, ',')
	return join(vcat(entries, lib), ',')
end

_plugin_has_module(p::GmtPlugin, api::Ptr{Cvoid})::Bool =
	api != C_NULL && GMT.GMT_Call_Module(api, p.probe, GMT.GMT_MODULE_EXIST, C_NULL) == 0

# Has this process loaded library `name` from directory `dir`? (dllist may report the resolved path,
# so the directory is matched both as given and resolved.)
function _plugin_loaded(name::String, dir::String)::Bool
	real = isdir(dir) ? realpath(dir) : dir
	return any(l -> (startswith(l, dir) || startswith(l, real)) && occursin(name, basename(l)), Libdl.dllist())
end

# Silent check (every start): GMT has the modules, and the plugin and its companions are the installed ones.
_plugin_loaded_ok(p::GmtPlugin)::Bool =
	_plugin_has_module(p, GMT.G_API[]) && all(n -> _plugin_loaded(n, _plugin_dir(p)), vcat(p.name, p.companions))

# Full self-test (after an install), the same one the CI plugin jobs run: the silent check, plus an
# actual module run through GMT.
function _plugin_selftest(p::GmtPlugin)::String
	_plugin_has_module(p, GMT.G_API[]) || return "GMT does not have the $(p.title) modules."
	_plugin_loaded(p.name, _plugin_dir(p)) || return "The $(p.title) plugin loaded is not the one in $(_plugin_dir(p))."
	for n in p.companions
		_plugin_loaded(n, _plugin_dir(p)) || return "$n was not loaded from $(_plugin_dir(p))."
	end
	return p.run(GMT.G_API[])
end

# Tell the viewer (when it is loaded) whether the menu offers `p` here, and its state:
# 0 = "Install as plugin", 1 = installed and current, 2 = installed, "Update the plugin",
# 3 = not offered here, but the GMT in use already has the modules (e.g. an installed Windows GMT
# with the plugin in its own plugin directory).
function _plugin_push_ready(p::GmtPlugin)
	haskey(_LIB_FNS, :gmtvtk_set_plugin_state) || return nothing   # viewer not loaded / not rebuilt yet
	haskey(_LIB_FNS, :gmtvtk_set_plugin_offered) &&
		ccall(_fn(:gmtvtk_set_plugin_offered), Cvoid, (Cint, Cint), p.id, _plugin_applies() ? 1 : 0)
	state = !_plugin_applies() ? (_plugin_has_module(p, GMT.G_API[]) ? 3 : 0) :
	        !p.ready[] ? 0 : p.update[] ? 2 : 1
	ccall(_fn(:gmtvtk_set_plugin_state), Cvoid, (Cint, Cint), p.id, state)
	return nothing
end

_plugin_push_ready() = foreach(_plugin_push_ready, _PLUGINS)

# Clear `dest` for a new copy. A library THIS process has loaded (the plugin being updated) cannot be
# deleted on Windows, but it can be renamed: it is moved aside as name.old-<pid>-<time> and removed
# by the next update (selfupdate's _displace_locked_dll, same Windows fact). Elsewhere it just goes.
function _plugin_clear(dest::String)
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

# Download `asset` of `p` from its release and unpack its <name>/ directory to `dest`, replacing what
# is there, and stamp the install time (INSTALLED_AT, UTC). Returns the plugin library's path.
function _plugin_fetch(p::GmtPlugin, asset::String, dest::String)::String
	url = "https://github.com/$(p.repo)/releases/download/$(_plugin_tag(p))/$asset"
	tmp = mktempdir()
	try
		archive = joinpath(tmp, asset)
		println("Downloading $url")
		when = GMT.Dates.now(GMT.Dates.UTC)
		GMT.Downloads.download(url, archive)
		# Relative name, run in the directory: a GNU tar on Windows reads "C:..." as a remote host.
		# On Windows _craft_tar() is Windows' own tar.exe (bsdtar), which reads .zip and .tar.gz alike.
		cd(() -> run(`$(_craft_tar()) -xf $asset`), tmp)
		src = joinpath(tmp, p.name)
		libname = _plugin_libname(p, asset)
		isfile(joinpath(src, "lib", libname)) || error("the archive has no $(p.name)/lib/$libname")
		_plugin_clear(dest)
		for (root, _, files) in walkdir(src), f in files
			to = joinpath(dest, relpath(joinpath(root, f), src))
			mkpath(dirname(to))
			cp(joinpath(root, f), to; force = true, follow_symlinks = false)   # libmbio.so -> libmbio.so.0 stays a link
		end
		write(_plugin_stamp(dest), GMT.Dates.format(when, GMT.Dates.dateformat"yyyy-mm-ddTHH:MM:SS"))
	finally
		rm(tmp; recursive = true, force = true)
	end
	return joinpath(dest, "lib", _plugin_libname(p, asset))
end

# Make GMT.jl's session load `lib` of `p` (see ACTIVATION above). Returns true when GMT has the
# plugin's modules afterwards. Plugins already in GMT_CUSTOM_LIBS stay there.
function _plugin_activate(p::GmtPlugin, lib::String)::Bool
	api = GMT.G_API[]
	api == C_NULL && return false
	_plugin_has_module(p, api) && return true
	_plugin_path_dir(dirname(lib))                     # Windows: its own DLLs, through PATH
	# Forward slashes on Windows: the value is written into gmt.conf, where a backslash is no path separator.
	value = _plugin_custom_libs(GMT.gmtlib_getparameter(api, "GMT_CUSTOM_LIBS"), replace(lib, '\\' => '/'))
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
	return _plugin_has_module(p, GMT.G_API[])
end

# __init__: activate every installed plugin. Nothing at all to do for one that is not installed.
function _plugin_activate_installed()
	for p in _PLUGINS
		lib = _plugin_lib(p)
		isfile(lib) || continue
		try
			_plugin_activate(p, lib) ||
				@tool_error "InteractiveGMT: the $(p.title) plugin in $(dirname(dirname(lib))) did not load into GMT."
			p.ready[] = _plugin_loaded_ok(p)     # pushed to the viewer once it is loaded (__init__)
		catch e
			@tool_error "InteractiveGMT: could not load the $(p.title) plugin." exception=(e,)
		end
	end
	return nothing
end

"""
    install_plugin(p::GmtPlugin)

Download plugin `p` (`MBSYSTEM_PLUGIN`, `GMTSAR_PLUGIN`) from its repository's `<name>-latest`
release into `<InteractiveGMT>/<name>` and load it into GMT. Re-running it replaces the installed
copy. Geophysics > <discipline> > Install as plugin calls this.
"""
function install_plugin(p::GmtPlugin)
	asset = _plugin_asset(p)
	isempty(asset) && error("There is no $(p.title) plugin download for this system ($(Sys.KERNEL) $(Sys.ARCH)" *
	                        (Sys.iswindows() ? ": on Windows the plugins are built for GMT.jl running GMT_jll" : "") * ").")
	# An UPDATE: this process already has the old copy loaded, and keeps it until it ends (GMT loads a
	# plugin once per session, and Windows will not unload it from under GMT). The new copy is the one
	# loaded from the next start.
	updating = _plugin_has_module(p, GMT.G_API[]) && _plugin_loaded(p.name, _plugin_dir(p))
	lib = _plugin_fetch(p, asset, _plugin_dir(p))
	println("Installed in $(_plugin_dir(p))")
	p.update[] = false
	if updating
		println("The plugin was updated. This session keeps the version it started with; the new one is loaded the next time iGMT starts.")
		_plugin_push_ready(p)
		return nothing
	end
	p.ready[] = false
	_plugin_push_ready(p)
	_plugin_activate(p, lib) || error("The $(p.title) plugin was installed but GMT did not load it ($lib).")
	println("GMT has loaded the $(p.title) modules.")
	p.installed()
	why = _plugin_selftest(p)
	isempty(why) || error("The $(p.title) plugin was installed but failed its test: $why")
	p.ready[] = true
	_plugin_push_ready(p)
	println("$(p.title) plugin installed and tested: GMT loaded it and $(p.ran).")
	return nothing
end

"""
    install_mbsystem_plugin()

Download MB-System's GMT supplement from the `mbsystem-latest` release of joa-quim/MB-System into
`<InteractiveGMT>/mbsystem` and load it into GMT. Geophysics > MB-System > Install as plugin.
"""
install_mbsystem_plugin() = install_plugin(MBSYSTEM_PLUGIN)

"""
    install_gmtsar_plugin()

Download GMTSAR's GMT supplement (its InSAR processing programs as GMT modules) from the
`gmtsar-latest` release of joa-quim/gmtsar into `<InteractiveGMT>/gmtsar` and load it into GMT.
Geophysics > GMTSAR > Install as plugin.
"""
install_gmtsar_plugin() = install_plugin(GMTSAR_PLUGIN)
