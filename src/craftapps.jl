# craftapps.jl — Plot with GMT.jl > "Edit in VectorCraft" / "Edit in PhotoCraft": the figure the editor's
# script makes, handed to an external editor. ONE mechanism for both programs (the SACRED LAW: same
# operation, same function); a program is only its description, a CraftApp below.
#
#   VectorCraft  storytold/vectorcraft  vector illustration  gets the figure's PostScript (.ps)
#   PhotoCraft   storytold/photocraft   photo editor         gets the figure as a PNG (.png)
#
# INSTALL. Both publish portable builds on their GitHub releases, so nothing is built: the latest
# release's file for this machine (`<name>-<ver>-windows-<arch>-portable.zip`, `-linux-<arch>.AppImage`,
# `-macos-universal.dmg`) is downloaded, checked against the release's SHA256SUMS.txt and unpacked to
# ~/.gmt/craftapps/<name>. NOT the package root: an `] add` install moves to a new folder on every iGMT
# update, which would strand the program and break its desktop icon. INSTALLED_TAG records the
# release; once the first window is up, an installed copy is compared with the latest release and the
# button then offers the update. After a first install the user is offered a desktop icon, made by
# iGMT's own launcher (`igmt --shortcut`, deps/src/launcher.c), so the program also starts outside iGMT.
#
# FINDING THE PROGRAM, in order: the INTERACTIVEGMT_<NAME> environment variable, `[<name>] path=` in
# ~/.gmt/iGMT.ini, the install folder, the PATH.
#
# HANDOFF. "Edit in X" runs the editor's CURRENT text through `_script_run` — the very Run — with its
# closing `showfig()` asked to write the file the user picked instead of showing it (GMT.jl's own
# `showfig(show=false, savefig=…)`). The text in the box is not changed. The file is the user's, chosen
# in a Save dialog: nothing temporary, and GMT.jl's PS in its temp dir is never pointed at. The program
# is then started on it as a separate process.

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

_craft_dir(a::CraftApp)::String = joinpath(homedir(), ".gmt", "craftapps", a.name)
_craft_tagfile(a::CraftApp, dir::String = _craft_dir(a))::String = joinpath(dir, "INSTALLED_TAG")

# The tail of the release asset for this machine, "" where there is none.
function _craft_asset_suffix()::String
	if Sys.iswindows()
		Sys.ARCH === :x86_64  && return "windows-x64-portable.zip"
		Sys.ARCH === :aarch64 && return "windows-arm64-portable.zip"
		Sys.ARCH === :i686    && return "windows-x86-portable.zip"
	elseif Sys.islinux()
		Sys.ARCH === :x86_64  && return "linux-x86_64.AppImage"
		Sys.ARCH === :aarch64 && return "linux-aarch64.AppImage"
	elseif Sys.isapple()
		return "macos-universal.dmg"
	end
	return ""
end

# The program as the install put it in its folder: <name>.exe, <name>.AppImage, or the .app bundle.
function _craft_installed_exe(a::CraftApp, d::String = _craft_dir(a))::String
	isdir(d) || return ""
	if Sys.isapple()
		for f in readdir(d)
			endswith(f, ".app") && return joinpath(d, f)
		end
		return ""
	end
	f = joinpath(d, a.name * (Sys.iswindows() ? ".exe" : ".AppImage"))
	return isfile(f) ? f : ""
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

# The latest release: (tag, asset name for this machine, its size in bytes). Records the tag in a.latest.
function _craft_release(a::CraftApp)::Tuple{String,String,Int}
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
	return tag, asset, s === nothing ? 0 : parse(Int, s.captures[1])
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
	tag, asset, _ = _craft_release(a)
	base = "https://github.com/$(a.repo)/releases/download/$tag/"
	tmp = mktempdir()
	try
		file = joinpath(tmp, asset)
		println("Downloading $(base)$(asset)")
		GMT.Downloads.download(base * asset, file)
		io = IOBuffer()
		GMT.Downloads.download(base * "SHA256SUMS.txt", io)
		m = match(Regex("([0-9a-fA-F]{64})\\s+\\*?" * replace(asset, "." => "\\.")), String(take!(io)))
		m === nothing && error("SHA256SUMS.txt of $(a.title) $tag does not list $asset.")
		_craft_sha256(file) == lowercase(m.captures[1]) || error("The download of $asset is corrupt (SHA-256 mismatch).")
		_craft_clear(a, dest)
		mkpath(dest)
		if endswith(asset, ".zip")
			# The zip holds one folder, <name>-<ver>-windows-<arch>-portable/: its contents go to dest.
			mkpath(joinpath(tmp, "x"))
			cd(() -> run(`$(_craft_tar()) -xf $asset -C x`), tmp)
			tops = readdir(joinpath(tmp, "x"))
			src = length(tops) == 1 && isdir(joinpath(tmp, "x", tops[1])) ? joinpath(tmp, "x", tops[1]) : joinpath(tmp, "x")
			for (root, _, files) in walkdir(src), f in files
				to = joinpath(dest, relpath(joinpath(root, f), src))
				mkpath(dirname(to))
				cp(joinpath(root, f), to; force = true)
			end
		elseif endswith(asset, ".AppImage")
			to = joinpath(dest, a.name * ".AppImage")
			cp(file, to; force = true)
			chmod(to, 0o755)
			_craft_appimage_icon(a, to, dest)
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

# Linux: the AppImage's own icon (its .DirIcon, often a symlink to the real PNG inside), taken out once
# at install as <name>.png beside it, for the desktop entry. Best effort: without it the entry gets a
# generic theme icon.
function _craft_appimage_icon(a::CraftApp, appimage::String, dest::String)
	sq = joinpath(dest, "squashfs-root")
	try
		cd(dest) do
			ex(p) = run(pipeline(`$appimage --appimage-extract $p`; stdout = devnull, stderr = devnull))
			ex(".DirIcon")
			ic = joinpath(sq, ".DirIcon")
			islink(ic) && ex(readlink(ic))
			isfile(ic) && cp(realpath(ic), joinpath(dest, a.name * ".png"); force = true)
		end
	catch e
		@debug "InteractiveGMT: no icon taken from $appimage (harmless)" exception = (e,)
	finally
		rm(sq; recursive = true, force = true)
	end
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
	png = joinpath(dirname(exe), a.name * ".png")
	icon = (Sys.islinux() && isfile(png)) ? ["--icon=$png"] : String[]
	comment = a.title * (a.id == PHOTOCRAFT.id ? " photo editor" : " vector illustration")
	success(`$launcher --shortcut --name=$(a.title) --target=$exe $icon --comment=$comment`) ||
		error("iGMT's launcher could not create the desktop icon for $(a.title).")
	println("A $(a.title) icon is on the desktop.")
	return nothing
end

# Start `exe` on `file` as a separate program; returns at once.
function _craft_launch(exe::String, file::String)
	cmd = (Sys.isapple() && endswith(exe, ".app")) ? `open -a $exe $file` :
	      Cmd(`$exe $file`; dir = dirname(exe), detach = true)
	run(cmd; wait = false)
	return nothing
end

"""
    _craft_edit(scene, id, dest, dpi, transparent) -> nothing

"Edit in VectorCraft / PhotoCraft": run the editor's script with its closing `showfig` writing `dest`
(PostScript for VectorCraft; a PNG at `dpi`, optionally with a transparent background, for PhotoCraft),
then open `dest` in the program.
"""
function _craft_edit(scene::Ptr{Cvoid}, id::Integer, dest::String, dpi::Integer, transparent::Bool)::Nothing
	a = _craft_app(id)
	exe = _craft_exe(a)
	isempty(exe) && error("$(a.title) is not installed.")
	isfile(dest) && rm(dest)              # the Save dialog asked; a stale copy must not pass for the new figure
	# GMT.jl's savefig picks the format by extension; ".PNG" (upper case) is its transparent PNG (-TG),
	# written as .png.
	fig = (transparent && endswith(lowercase(dest), ".png")) ? splitext(dest)[1] * ".PNG" : dest
	_script_run(scene; savefig = fig, dpi = Int(dpi))
	isfile(dest) || error("The script ran but did not write $dest.")
	_craft_launch(exe, dest)
	println("Opened $dest in $(a.title).")
	return nothing
end
