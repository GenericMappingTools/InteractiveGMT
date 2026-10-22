# craftapps.jl — Plot with GMT.jl > "Edit in VectorCraft" / "Edit in PhotoCraft": the figure the editor's
# script makes, handed to an external editor. ONE mechanism for both programs (the SACRED LAW: same
# operation, same function); a program is only its description, a CraftApp below.
#
#   VectorCraft  storytold/vectorcraft  vector illustration  gets the figure as EPS (.eps)
#   PhotoCraft   storytold/photocraft   photo editor         gets the figure as a PNG (.png)
#
# INSTALL. Both publish portable builds on their GitHub releases, so nothing is built: the latest
# release's file for this machine (`<name>-<ver>-windows-<arch>-portable.zip`, `-linux-<arch>.tar.gz`,
# `-macos-universal.dmg` plus `<name>-cli-<ver>-macos-universal.zip`: every system gets the program AND
# its <name>-cli, which the PhotoCraft round trip needs) is downloaded, checked against the release's
# SHA256SUMS.txt and unpacked to
# ~/.gmt/craftapps/<name>. NOT the package root: an `] add` install moves to a new folder on every iGMT
# update, which would strand the program and break its desktop icon. INSTALLED_TAG records the
# release; once the first window is up, an installed copy is compared with the latest release and the
# button then offers the update. After a first install the user is offered a desktop icon, made by
# iGMT's own launcher (`igmt --shortcut`, deps/src/launcher.c), so the program also starts outside iGMT.
#
# FINDING THE PROGRAM, in order: the INTERACTIVEGMT_<NAME> environment variable, `[<name>] path=` in
# ~/.gmt/iGMT.ini, the install folder, the PATH.
#
# HANDOFF. "Run and Edit in X" runs the editor's CURRENT text through `_script_run` — the very Run — with
# its closing `showfig()` asked to write a figure file instead of showing it (GMT.jl's own
# `showfig(show=false, savefig=…)`, which crops with psconvert -A). The text in the box is not changed.
# No question is asked: the viewer names the file, igmt_figure_<date_time>.eps / .png in the default
# directory (or ~/.gmt/craftapps/figures). It is a kept file, not a temporary one, and GMT.jl's PS in its
# temp dir is never pointed at. EPS, not PS, for VectorCraft: GMT.jl's .ps is the raw PostScript on its
# oversized page (a 32767 pt bounding box, all white around the figure); .eps goes through psconvert -A.
# The program is then started on the file as a separate process.

struct CraftApp
	id::Cint                          # the viewer's index (GMTVTK_CRAFT_* in deps/src/30_app.cpp)
	title::String                     # "VectorCraft": what the button and the messages call it
	name::String                      # "vectorcraft": the release asset prefix, the install folder, the executable
	repo::String                      # GitHub repository of the releases
	latest::Base.RefValue{String}     # the latest release's tag, "" until looked up
end

const VECTORCRAFT = CraftApp(0, "VectorCraft", "vectorcraft", "storytold/vectorcraft", Ref(""))
const PHOTOCRAFT  = CraftApp(1, "PhotoCraft",  "photocraft",  "storytold/photocraft",  Ref(""))
const _CRAFT_APPS = (VECTORCRAFT, PHOTOCRAFT)

_craft_app(id::Integer)::CraftApp = _CRAFT_APPS[id + 1]

# The figure file the last "Run and Edit" wrote (the viewer names it; the test suite reads it here).
const _CRAFT_LAST = Ref("")

_craft_dir(a::CraftApp)::String = joinpath(homedir(), ".gmt", "craftapps", a.name)
_craft_tagfile(a::CraftApp, dir::String = _craft_dir(a))::String = joinpath(dir, "INSTALLED_TAG")

# The tail of the release asset for this machine, "" where there is none.
function _craft_asset_suffix()::String
	if Sys.iswindows()
		Sys.ARCH === :x86_64  && return "windows-x64-portable.zip"
		Sys.ARCH === :aarch64 && return "windows-arm64-portable.zip"
		Sys.ARCH === :i686    && return "windows-x86-portable.zip"
	elseif Sys.islinux()
		# The tarball, not the AppImage: it carries <name>-cli beside the program (bin/), and the CLI is
		# what iGMT drives the program through (the PhotoCraft round trip below).
		Sys.ARCH === :x86_64  && return "linux-x86_64.tar.gz"
		Sys.ARCH === :aarch64 && return "linux-aarch64.tar.gz"
	elseif Sys.isapple()
		return "macos-universal.dmg"              # the .app; its CLI is a separate zip (_craft_install)
	end
	return ""
end

# The program as the install put it in its folder: <name>.exe (Windows), bin/<name> (Linux), the .app
# bundle (macOS).
function _craft_installed_exe(a::CraftApp, d::String = _craft_dir(a))::String
	isdir(d) || return ""
	if Sys.isapple()
		for f in readdir(d)
			endswith(f, ".app") && return joinpath(d, f)
		end
		return ""
	end
	f = Sys.iswindows() ? joinpath(d, a.name * ".exe") : joinpath(d, "bin", a.name)
	return isfile(f) ? f : ""
end

# Its command-line twin, <name>-cli: beside the program on Windows and Linux, in cli/ on macOS (the
# separate zip), else on the PATH. "" when there is none.
function _craft_cli(a::CraftApp)::String
	cli = a.name * "-cli" * (Sys.iswindows() ? ".exe" : "")
	exe = _craft_exe(a)
	for f in (isempty(exe) || endswith(exe, ".app") ? "" : joinpath(dirname(exe), cli), joinpath(_craft_dir(a), "cli", cli))
		(!isempty(f) && isfile(f)) && return f
	end
	w = Sys.which(cli)
	return w === nothing ? "" : w
end

# The executable to start. A macOS .app is a folder: its program is the one file in Contents/MacOS
# (started directly, not through `open`, so iGMT holds the process -- the round trip needs it alive).
function _craft_binary(exe::String)::String
	endswith(exe, ".app") || return exe
	d = joinpath(exe, "Contents", "MacOS")
	fs = isdir(d) ? filter(f -> isfile(joinpath(d, f)), readdir(d)) : String[]
	isempty(fs) && error("$exe has no program in Contents/MacOS.")
	return joinpath(d, first(fs))
end

# The program to launch (see FINDING THE PROGRAM above), "" when there is none.
function _craft_exe(a::CraftApp)::String
	for p in (get(ENV, "INTERACTIVEGMT_" * uppercase(a.name), ""), _ini_get(a.name, "path"))
		(!isempty(p) && ispath(p)) && return p
	end
	f = _craft_installed_exe(a)
	isempty(f) || return f
	w = Sys.which(a.name)
	return w === nothing ? "" : w
end

# The latest release: (tag, asset name for this machine, its size in bytes, the release JSON). Records
# the tag in a.latest.
function _craft_release(a::CraftApp)::Tuple{String,String,Int,String}
	suf = _craft_asset_suffix()
	isempty(suf) && error("$(a.title) has no download for this system ($(Sys.KERNEL) $(Sys.ARCH)).")
	io = IOBuffer()
	GMT.Downloads.download("https://api.github.com/repos/$(a.repo)/releases/latest", io;
	                       headers = ["Accept" => "application/vnd.github+json"], timeout = 20)
	json = String(take!(io))
	m = match(r"\"tag_name\"\s*:\s*\"([^\"]+)\"", json)
	m === nothing && error("GitHub did not report a release of $(a.title) (github.com/$(a.repo)).")
	tag = String(m.captures[1])
	asset = "$(a.name)-$(lstrip(tag, 'v'))-$suf"
	k = findfirst(Regex("\"name\"\\s*:\\s*\"" * replace(asset, "." => "\\.") * "\""), json)
	k === nothing && error("The $(a.title) $tag release has no $asset.")
	s = match(r"\"size\"\s*:\s*(\d+)", json, last(k))
	a.latest[] = tag
	return tag, asset, s === nothing ? 0 : parse(Int, s.captures[1]), json
end

# Is a newer release than the one installed in ~/.gmt/craftapps/<name> known? (A program found elsewhere —
# environment, iGMT.ini, PATH — is the user's own and never offered an update.)
function _craft_update_available(a::CraftApp)::Bool
	(isempty(a.latest[]) || isempty(_craft_installed_exe(a)) || !isfile(_craft_tagfile(a))) && return false
	return strip(read(_craft_tagfile(a), String)) != a.latest[]
end

# Tell the viewer what the editor's button for `a` offers: 0 = "Install", 1 = "Edit in", 2 = "Update"
# (and edit), 3 = nothing (no download for this system, and none found).
function _craft_push_state(a::CraftApp)
	haskey(_LIB_FNS, :gmtvtk_set_craft_state) || return nothing   # viewer not loaded / not rebuilt yet
	state = !isempty(_craft_exe(a)) ? (_craft_update_available(a) ? 2 : 1) :
	        isempty(_craft_asset_suffix()) ? 3 : 0
	ccall(_fn(:gmtvtk_set_craft_state), Cvoid, (Cint, Cint), a.id, Cint(state))
	return nothing
end

_craft_push_state() = foreach(_craft_push_state, _CRAFT_APPS)

# At start, once the first window is up: look for newer releases of the installed programs, in the
# background. Nothing at all when none is installed. A failure (no network) is silent.
function _craft_schedule_update_check()
	installed = filter(a -> !isempty(_craft_installed_exe(a)), collect(_CRAFT_APPS))
	isempty(installed) && return nothing
	Timer(1.0; interval = 1.0) do t
		_PUMP[] === nothing && return                   # no window on screen yet
		close(t)
		@async for a in installed
			try
				_craft_release(a)
				_craft_push_state(a)
			catch e
				@debug "InteractiveGMT: $(a.title) update check failed (harmless)" exception = (e,)
			end
		end
	end
	return nothing
end

# SHA-256 of a file, by the system's own tool (no hashing package is a dependency).
function _craft_sha256(f::String)::String
	out = Sys.iswindows() ? read(`certutil -hashfile $f SHA256`, String) :
	      Sys.isapple()   ? read(`shasum -a 256 $f`, String) : read(`sha256sum $f`, String)
	for l in split(out, '\n')
		w = split(l)
		isempty(w) && continue
		# certutil prints the hash alone on its line (older Windows: as space-separated bytes);
		# sha256sum / shasum print "<hash>  <file>".
		t = Sys.iswindows() ? join(w) : String(w[1])
		occursin(r"^[0-9a-fA-F]{64}$", t) && return lowercase(t)
	end
	error("could not compute the SHA-256 of $f")
end

# Windows: the system's own tar (bsdtar, which reads zip). A Git/MSYS GNU tar earlier on the PATH
# cannot, and reads "C:..." as a remote host.
_craft_tar()::String = Sys.iswindows() ? joinpath(get(ENV, "SystemRoot", "C:\\Windows"), "System32", "tar.exe") : "tar"

# Clear `dest` for a new copy, KEEPING the program's portable data folder: with portable.txt beside the
# executable, it keeps its preferences, presets and crash-recovery autosaves in <Title>Data there
# (PhotoCraftData, VectorCraftData), and an update must not wipe them. Files in use are moved aside
# (`_plugin_clear`, the same Windows fact), and an earlier update's leftovers are removed.
function _craft_clear(a::CraftApp, dest::String)
	isdir(dest) || return nothing
	for e in readdir(dest)
		lowercase(e) == lowercase(a.title * "Data") && continue
		p = joinpath(dest, e)
		if isdir(p)
			_plugin_clear(p)
		elseif occursin(".old-", e)
			try rm(p; force = true) catch end
		else
			try
				rm(p; force = true)
			catch
				mv(p, p * ".old-$(getpid())-$(round(Int, time()))")
			end
		end
	end
	return nothing
end

# Download the latest release of `a` for this machine, check it, and put the program in `dest`
# (~/.gmt/craftapps/<name>), replacing what is there. Returns the program's path.
function _craft_install(a::CraftApp, dest::String = _craft_dir(a))::String
	tag, asset, _, json = _craft_release(a)
	base = "https://github.com/$(a.repo)/releases/download/$tag/"
	tmp = mktempdir()
	try
		io = IOBuffer()
		GMT.Downloads.download(base * "SHA256SUMS.txt", io)
		sums = String(take!(io))
		# One release file into tmp, checked against SHA256SUMS.txt.
		function fetch(name)
			file = joinpath(tmp, name)
			println("Downloading $(base)$(name)")
			GMT.Downloads.download(base * name, file)
			m = match(Regex("([0-9a-fA-F]{64})\\s+\\*?" * replace(name, "." => "\\.")), sums)
			m === nothing && error("SHA256SUMS.txt of $(a.title) $tag does not list $name.")
			_craft_sha256(file) == lowercase(m.captures[1]) || error("The download of $name is corrupt (SHA-256 mismatch).")
			return file
		end
		file = fetch(asset)
		_craft_clear(a, dest)
		mkpath(dest)
		if endswith(asset, ".zip") || endswith(asset, ".tar.gz")
			# One folder, <name>-<ver>-<system>…/: its contents go to dest (Windows: <name>.exe beside
			# <name>-cli.exe; Linux: bin/<name>, bin/<name>-cli and share/).
			_craft_unpack(file, dest, tmp)
			Sys.islinux() && _craft_linux_icon(a, dest)
		else                        # .dmg: mount it, copy the .app out
			mnt = joinpath(tmp, "mnt")
			mkpath(mnt)
			run(`hdiutil attach -nobrowse -readonly -mountpoint $mnt $file`)
			try
				apps = filter(f -> endswith(f, ".app"), readdir(mnt))
				isempty(apps) && error("$asset holds no .app.")
				cp(joinpath(mnt, apps[1]), joinpath(dest, apps[1]); force = true, follow_symlinks = false)
			finally
				run(ignorestatus(`hdiutil detach -quiet $mnt`))
			end
			# The .app has no <name>-cli; the release ships it as its own zip, into dest/cli.
			cli = "$(a.name)-cli-$(lstrip(tag, 'v'))-macos-universal.zip"
			occursin("\"name\": \"$cli\"", json) || occursin("\"name\":\"$cli\"", json) ?
				_craft_unpack(fetch(cli), joinpath(dest, "cli"), tmp) :
				println("The $(a.title) $tag release has no $cli: the PhotoCraft round trip will need one on the PATH.")
		end
		write(_craft_tagfile(a, dest), tag)
	finally
		rm(tmp; recursive = true, force = true)
	end
	exe = _craft_installed_exe(a, dest)
	isempty(exe) && error("$(a.title) $tag was unpacked into $dest, but its program is not there.")
	return exe
end

# ── what the editor's button calls (deps/src/70_window.cpp, craftClicked) ─────────────────────────

# Before an install: what would be downloaded, for the confirmation box.
function _craft_offer(id::Integer)::Nothing
	a = _craft_app(id)
	tag, asset, sz = _craft_release(a)
	println("$(a.title) $tag: $asset, $(round(Int, sz / 2^20)) MB, from github.com/$(a.repo).\n",
	        "It goes into $(_craft_dir(a)).")
	return nothing
end

"""
    install_craft_app(id)

Download the latest portable release of VectorCraft (`id = 0`) or PhotoCraft (`id = 1`) into
`~/.gmt/craftapps/<name>`. Re-running it replaces the installed copy. The Plot with GMT.jl editor's
"Install" / "Update" button calls this.
"""
function install_craft_app(id::Integer)::Nothing
	a = _craft_app(id)
	exe = _craft_install(a)
	_craft_push_state(a)
	println("$(a.title) $(a.latest[]) installed: $exe")
	return nothing
end

# Unpack archive `file` (.zip or .tar.gz) and put the contents of its single top folder in `dest`.
# Links stay links and the executables keep their mode (cp copies the file mode).
function _craft_unpack(file::String, dest::String, tmp::String)
	x = mktempdir(tmp)
	# Relative names, run in the directory: a GNU tar on Windows reads "C:..." as a remote host.
	cd(() -> run(`$(_craft_tar()) -xf $(relpath(file, x)) -C .`), x)
	tops = readdir(x)
	src = length(tops) == 1 && isdir(joinpath(x, tops[1])) ? joinpath(x, tops[1]) : x
	for (root, _, files) in walkdir(src), f in files
		to = joinpath(dest, relpath(joinpath(root, f), src))
		mkpath(dirname(to))
		cp(joinpath(root, f), to; force = true, follow_symlinks = false)
	end
	return nothing
end

# Linux: the program's own icon for its desktop entry, the largest PNG of share/icons/hicolor, copied
# as <name>.png beside bin/. Best effort: without it the entry gets a generic theme icon.
function _craft_linux_icon(a::CraftApp, dest::String)
	best, px = "", 0
	for (root, _, files) in walkdir(joinpath(dest, "share", "icons")), f in files
		endswith(f, ".png") || continue
		m = match(r"(\d+)x\d+", root)
		n = m === nothing ? 0 : parse(Int, m.captures[1])
		n > px && ((best, px) = (joinpath(root, f), n))
	end
	isempty(best) || cp(best, joinpath(dest, a.name * ".png"); force = true)
	return nothing
end

"""
    _craft_shortcut(id)

Put an icon for the installed VectorCraft (`id = 0`) / PhotoCraft (`id = 1`) on the desktop, so it starts
outside iGMT too. Made by iGMT's own launcher (`igmt --shortcut`, deps/src/launcher.c), the same binary
and the same desktop folder iGMT's own icon uses.
"""
function _craft_shortcut(id::Integer)::Nothing
	a = _craft_app(id)
	exe = _craft_installed_exe(a)
	isempty(exe) && error("$(a.title) is not installed in $(_craft_dir(a)).")
	launcher = joinpath(_BIN_DIR, Sys.iswindows() ? "igmt.exe" : "igmt")
	isfile(launcher) || error("iGMT's launcher ($launcher) is not there, so no desktop icon can be made.")
	png = joinpath(_craft_dir(a), a.name * ".png")      # Linux: taken from share/icons at install
	icon = (Sys.islinux() && isfile(png)) ? ["--icon=$png"] : String[]
	comment = a.title * (a.id == PHOTOCRAFT.id ? " photo editor" : " vector illustration")
	success(`$launcher --shortcut --name=$(a.title) --target=$exe $icon --comment=$comment`) ||
		error("iGMT's launcher could not create the desktop icon for $(a.title).")
	println("A $(a.title) icon is on the desktop.")
	return nothing
end

# Start `exe` on `file` as a separate program; returns the process at once.
function _craft_launch(exe::String, file::String)::Base.Process
	cmd = (Sys.isapple() && endswith(exe, ".app")) ? `open -a $exe $file` :
	      Cmd(`$exe $file`; dir = dirname(exe), detach = true)
	return run(cmd; wait = false)
end

# NO DEAD TIME while the program starts (a 100+ MB app takes seconds to put its window up): the busy
# notice stays until the program is ready for input. Windows only, by WaitForInputIdle in short slices
# so the notice keeps being painted; capped at 30 s, and left at once when the process ends or is not a
# GUI program (WaitForInputIdle then fails). Elsewhere the program is a child that starts on its own
# (macOS `open`) and there is no portable "window is up" to wait for.
function _craft_wait_ready(p::Base.Process)
	Sys.iswindows() || return nothing
	h = ccall((:OpenProcess, "kernel32"), Ptr{Cvoid}, (UInt32, Cint, UInt32), 0x00100400, 0, getpid(p))
	h == C_NULL && return nothing          # SYNCHRONIZE | PROCESS_QUERY_INFORMATION refused
	try
		t0 = time()
		while time() - t0 < 30 && process_running(p)
			ccall((:WaitForInputIdle, "user32"), UInt32, (Ptr{Cvoid}, UInt32), h, 100) == 0x00000102 || break
			ccall(_fn(:gmtvtk_progress_status), Cvoid, (Cint, Cstring), -1, C_NULL)   # repaint the notice
		end
	finally
		ccall((:CloseHandle, "kernel32"), Cint, (Ptr{Cvoid},), h)
	end
	return nothing
end

"""
    _craft_edit(scene, id, dest, dpi, transparent) -> nothing

"Edit in VectorCraft / PhotoCraft": run the editor's script with its closing `showfig` writing `dest`
(EPS for VectorCraft; a PNG at `dpi`, optionally with a transparent background, for PhotoCraft),
then open `dest` in the program.
"""
function _craft_edit(scene::Ptr{Cvoid}, id::Integer, dest::String, dpi::Integer, transparent::Bool)::Nothing
	a = _craft_app(id)
	exe = _craft_exe(a)
	isempty(exe) && error("$(a.title) is not installed.")
	isfile(dest) && rm(dest)              # a stale copy must not pass for the new figure
	# GMT.jl's savefig picks the format by extension; ".PNG" (upper case) is its transparent PNG (-TG),
	# written as .png.
	fig = (transparent && endswith(lowercase(dest), ".png")) ? splitext(dest)[1] * ".PNG" : dest
	# One notice from click to program window: the script's status window ("Creating the figure for X")
	# while the figure is made, then "Starting X" until the program is up.
	_script_run(scene; savefig = fig, dpi = Int(dpi), what = "Creating the figure for $(a.title)...")
	isfile(dest) || error("The script ran but did not write $dest.")
	_CRAFT_LAST[] = dest
	ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), 0, "Starting $(a.title)...")
	try
		_craft_wait_ready(_craft_launch(exe, dest))
	finally
		ccall(_fn(:gmtvtk_progress_close), Cvoid, ())
	end
	println("Opened $dest in $(a.title).")
	return nothing
end

# ── Round trip with PhotoCraft: a displayed raster out to PhotoCraft, and the edited picture back ──
#
# Scene Objects > an image or the base grid > "Edit in PhotoCraft" / "Get back from PhotoCraft".
#
# OUT: the picture is written to ~/.gmt/craftapps/exchange (a kept folder, not a temporary one) and
# opened in a PhotoCraft that iGMT started with its CONTROL channel on (`--control <port>`, a token file
# beside the install, file access limited to that folder). An IMAGE goes out as the stored image itself,
# at full resolution; a GRID as the picture the window shows of it (`_display_image`, the one function
# that turns a grid's displayed pixels into an image — gmtscript.jl says why a grid has no other honest
# picture). A second "Edit" while that PhotoCraft is open opens the file in it rather than starting
# another.
#
# BACK: iGMT asks PhotoCraft to save its current document into the exchange folder, through
# `photocraft-cli mcp --bridge` (PhotoCraft's own MCP bridge, spoken over the child's stdin/stdout: no
# socket library needed in iGMT), then reads it. The edited picture ALWAYS REPLACES ITS ORIGINAL, by
# the user's order (2026-10-22), and keeps the original's EXTENT: a resampled image of another size
# over the same limits is not wrong, so only the pixel size follows the new image.
#   * an image: the same object, rewritten in place (like Image > Flip), and redrawn under its own row;
#   * a grid: its heights cannot come back from colours, so the edited picture is DRAPED on it, as an
#     image named "<grid> (PhotoCraft)"; a later get-back replaces that drape. Draping exists for the
#     window's BASE grid only (imageRebuildActor drapes onto the base's nodes), so that is the grid
#     offered.
# There is no "user saved" event in PhotoCraft's protocol, so getting back is always asked for.

const _PC_SENT = Dict{Tuple{Ptr{Cvoid},String,String},String}()   # (scene, kind, name) -> file sent
const _PC_PROC = Ref{Union{Nothing,Base.Process}}(nothing)        # the PhotoCraft iGMT controls

_pc_dir()::String = mkpath(joinpath(homedir(), ".gmt", "craftapps", "exchange"))
_pc_token_file()::String = joinpath(homedir(), ".gmt", "craftapps", "photocraft-control.token")
function _pc_port()::Int
	p = tryparse(Int, _ini_get("photocraft", "control_port"))
	return p === nothing ? 47874 : p
end

# photocraft-cli, which speaks the MCP bridge (see _craft_cli for where each system's install has it).
function _pc_cli()::String
	isempty(_craft_exe(PHOTOCRAFT)) &&
		error("PhotoCraft is not installed. Install it from File > Plot with GMT.jl > Install PhotoCraft.")
	cli = _craft_cli(PHOTOCRAFT)
	isempty(cli) && error("photocraft-cli, which iGMT talks to PhotoCraft through, was not found. Reinstall " *
	                      "PhotoCraft from File > Plot with GMT.jl (Update / Install) to get it.")
	return cli
end

_pc_running()::Bool = _PC_PROC[] !== nothing && process_running(_PC_PROC[])

# One reply of the MCP conversation: the line carrying `id`. Waits at most 90 s.
function _pc_rpc(p, id::Int, method::String, params::String)::String
	write(p, "{\"jsonrpc\":\"2.0\",\"id\":$id,\"method\":\"$method\",\"params\":$params}\n")
	flush(p)
	t = @async begin
		while !eof(p)
			l = readline(p)
			occursin("\"id\":$id,", l) && return l
		end
		return ""
	end
	timedwait(() -> istaskdone(t), 90.0) === :ok || error("PhotoCraft did not answer within 90 s.")
	l = fetch(t)
	isempty(l) && error("The connection to PhotoCraft ended without an answer. Is the PhotoCraft iGMT opened still running?")
	return l
end

# The text a tool call answered, or its error.
function _pc_text(l::String, tool::String)::String
	m = match(r"\"text\":\"((?:[^\"\\]|\\.)*)\"", l)
	txt = m === nothing ? "" : unescape_string(m.captures[1])
	if occursin("\"isError\":true", l) || (m === nothing && occursin("\"error\":", l))
		e = match(r"\"message\":\"((?:[^\"\\]|\\.)*)\"", l)
		error("PhotoCraft refused $tool: " * (isempty(txt) ? (e === nothing ? l : unescape_string(e.captures[1])) : txt))
	end
	return txt
end

# A sequence of MCP tool calls (name => JSON arguments) on the running PhotoCraft; their texts.
function _pc_tools(calls::Vector{Pair{String,String}})::Vector{String}
	cmd = `$(_pc_cli()) mcp --bridge 127.0.0.1:$(_pc_port()) --control-token-file $(_pc_token_file())`
	p = open(pipeline(Cmd(cmd; windows_hide = true); stderr = devnull), "r+")
	try
		_pc_rpc(p, 1, "initialize",
		        "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"iGMT\",\"version\":\"1\"}}")
		write(p, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
		return [_pc_text(_pc_rpc(p, k + 1, "tools/call", "{\"name\":\"$t\",\"arguments\":$a}"), t)
		        for (k, (t, a)) in enumerate(calls)]
	finally
		close(p)
	end
end

# A file name from an element name: letters, digits, '-', '_' and '.', the rest '_'.
_pc_stem(name::AbstractString) = (s = replace(String(name), r"[^A-Za-z0-9_.-]" => "_"); isempty(s) ? "image" : s)

# The raster `kind` ("image" | "grid") called `name` ("" = the window's own), as (key, object).
function _pc_raster(scene::Ptr{Cvoid}, kind::String, name::String)
	if kind == "image"
		key, I = isempty(name) ? _find_object_named(scene, :image) : (name, _find_object(scene, :image, name))
		(I isa GMTimage) || error("This window has no image named '$name'.")
		return String(key), I
	end
	kind == "grid" || error("PhotoCraft: unknown kind '$kind'")
	# The BASE grid, by name "": the row this comes from is the base surface's own, and the base is the
	# only grid a picture drapes onto. Through _active_grid, the one grid resolver.
	G = _active_grid(scene, "")
	(G isa GMTgrid) || error("This window has no base grid.")
	return String(name), G
end

"""
    _pc_send(scene, kind, name)

Scene Objects > "Edit in PhotoCraft": write the raster's picture to the exchange folder and open it in
the PhotoCraft iGMT controls (started now, with its control channel, if it is not running).
"""
function _pc_send(scene::Ptr{Cvoid}, kind::String, name::String)::Nothing
	exe = _craft_exe(PHOTOCRAFT)
	isempty(exe) && error("PhotoCraft is not installed. Install it from File > Plot with GMT.jl > Install PhotoCraft.")
	_pc_cli()                                       # fail before anything is written, not at get-back
	key, R = _pc_raster(scene, kind, name)
	file = _pc_stem(isempty(key) ? kind : key) * "_" * Libc.strftime("%Y%m%d_%H%M%S", time()) * ".png"
	path = joinpath(_pc_dir(), file)
	ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), 0, "Sending the picture to PhotoCraft...")
	try
		pic = R isa GMTimage ? R : _display_image(scene, R.range[1], R.range[2], R.range[3], R.range[4])
		GMT.gmtwrite(path, pic)
		isfile(path) || error("Could not write $path.")
		if _pc_running()
			_pc_tools(["doc_open" => "{\"path\":\"$file\"}"])
		else
			x = _pc_dir()
			bin = _craft_binary(exe)                # macOS: the program inside the .app, so iGMT holds it
			cmd = `$bin --control $(_pc_port()) --control-token-file $(_pc_token_file()) --automation-read-root $x --automation-write-root $x $path`
			_PC_PROC[] = run(Cmd(cmd; dir = dirname(bin), detach = true); wait = false)
			ccall(_fn(:gmtvtk_progress_status), Cvoid, (Cint, Cstring), -1, "Starting PhotoCraft...")
			_craft_wait_ready(_PC_PROC[])
		end
	finally
		ccall(_fn(:gmtvtk_progress_close), Cvoid, ())
	end
	_PC_SENT[(scene, kind, name)] = file
	println("Sent to PhotoCraft as $file. Edit it there, then use \"Get back from PhotoCraft\" on the same row.")
	return nothing
end

# The edited picture onto the original's extent: the SAME limits and CRS; only the pixel size is the
# new image's. The limits are the original's `range` as it stands, because that is what the picture
# covered: the viewer lays an image over its range (`_drape_to_bbox(I, range...)`), and a grid's picture
# is captured over its range (`_pc_send`). Widening a gridline raster by half a cell would move it.
function _pc_georef!(J::GMTimage, R)
	r = R.range
	J.proj4, J.wkt, J.epsg, J.geog = R.proj4, R.wkt, R.epsg, R.geog
	return _georef_image!(J, r[1], r[2], r[3], r[4])
end

"""
    _pc_back(scene, kind, name)

Scene Objects > "Get back from PhotoCraft": have PhotoCraft save the document iGMT sent it, read it, and
put it in place of the original (an image: the same image rewritten; a grid: draped on it).
"""
function _pc_back(scene::Ptr{Cvoid}, kind::String, name::String)::Nothing
	file = get(_PC_SENT, (scene, kind, name), "")
	isempty(file) && error("This $kind was not sent to PhotoCraft. Use \"Edit in PhotoCraft\" first.")
	_pc_running() || error("The PhotoCraft iGMT opened is no longer running, so the edits cannot be fetched. " *
	                       "Use \"Edit in PhotoCraft\" again.")
	key, R = _pc_raster(scene, kind, name)
	back = splitext(file)[1] * "_edited.png"
	ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), 0, "Getting the picture back from PhotoCraft...")
	try
		insp = _pc_tools(["doc_inspect" => "{}"])[1]
		occursin("\"name\":\"$file\"", insp) ||
			error("PhotoCraft is showing another document. Bring '$file' to the front in PhotoCraft and try again.")
		_pc_tools(["doc_save" => "{\"path\":\"$back\"}"])
		path = joinpath(_pc_dir(), back)
		isfile(path) || error("PhotoCraft did not write $path.")
		_pc_put_back!(scene, kind, name, key, R, GMT.gmtread(path))
	finally
		ccall(_fn(:gmtvtk_progress_close), Cvoid, ())
	end
	println("Got the edited picture back from PhotoCraft.")
	return nothing
end

# The edited picture `J` in place of raster `R` (see BACK above). Separate from the fetch so a test can
# drive the replacement without a PhotoCraft.
function _pc_put_back!(scene::Ptr{Cvoid}, kind::String, name::String, key::String, R, J::GMTimage)
	_pc_georef!(J, R)
	if R isa GMTimage
		# In place, like Image > Flip: the viewer's registry, Save… and Save Session hold this object.
		for f in fieldnames(GMTimage)
			setfield!(R, f, getfield(J, f))
		end
		# The 16-bit source a stretch re-derives from is the OLD picture now: drop it and its menu entry.
		d = get(_IMG_ORIG, scene, nothing)
		d === nothing || delete!(d, key)
		ccall(_fn(:gmtvtk_image_set_has_orig_h), Cvoid, (Ptr{Cvoid}, Cstring, Cint), scene, key, Cint(0))
		# A file-loaded image would be re-read from its file by a saved session: the edit lives in memory.
		for r in get(_SESSION_LOG, scene, ElementRecipe[])
			(r.name == key && r.kind in (:image, :dropimage)) && (r.origin = :generated; r.source = "")
		end
		_update_image_pixels!(scene, name, R) || error("The viewer has no image named '$name' to redraw.")
		return nothing
	end
	nm = (isempty(key) ? "grid" : key) * " (PhotoCraft)"
	if _update_image_pixels!(scene, nm, J)
		_forget_object!(scene, :image, nm)              # the previous get-back's picture, replaced
		_remember_object!(scene, :image, nm, J)
	else
		_add_image_to_scene(scene, J, nm; promote = false)
		ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, nm, Cint(1))
		ccall(_fn(:gmtvtk_image_set_draped_h), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, nm, Cint(1))
	end
	return nothing
end
