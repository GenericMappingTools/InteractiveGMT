# projfix.jl -- TEMPORARY (2026-11-26). Remove once every iGMT Windows install has been repaired.
#
# Installers built before 2026-11-26 shipped <INSTDIR>\gmt\share\proj from the GMT source tree's
# untracked share\proj (a 2021 proj.db) beside a current proj_w64.dll. That PROJ rejects the old
# database ("DATABASE.LAYOUT.VERSION.MINOR = 1 whereas a number >= 7 is expected"), so every EPSG
# lookup fails -- and iGMT crashed on a UTM GeoTIFF. make_installer.ps1 now ships the matching data;
# this repairs installs that already exist, without a reinstall, from deps/proj_data_fix.zip (the
# share\proj of the PROJ that proj_w64.dll is).
#
# Stdlib only: deps/build.jl includes this file too. It runs (1) from build.jl, which Check for
# Updates runs from the freshly pulled tree, and (2) from __init__, for the case where the running
# iGMT had proj.db open during the update (Windows will not replace it then). A stamp in share\proj
# makes it a one-time check per install.

const _PROJFIX_STAMP = ".igmt_proj_checked"

# <INSTDIR> of an iGMT Windows install (julia is <INSTDIR>\julia\bin\julia.exe beside
# igmt_bundle.ini), "" anywhere else.
function _projfix_inst()::String
	Sys.iswindows() || return ""
	r = dirname(dirname(Sys.BINDIR))
	return (isfile(joinpath(r, "igmt_bundle.ini")) && isdir(joinpath(r, "gmt", "share", "proj"))) ? r : ""
end

# Does the bundled GMT's PROJ accept the bundled proj.db? -9 is UTM zone 29's central meridian, so
# EPSG:32629 must give exactly the 500000 m false easting -- the same test make_installer.ps1 runs.
function _projfix_db_ok(inst::String)::Bool
	gmt = joinpath(inst, "gmt", "bin", "gmt.exe")
	isfile(gmt) || return true                      # no bundled GMT to test: nothing of ours to fix
	pt  = tempname() * ".txt"
	write(pt, "-9 40\n")
	out, err = IOBuffer(), IOBuffer()
	cmd = addenv(`$gmt mapproject $pt -JEPSG:32629 -R-10/-8/39/41`,
	             "GMT_SHAREDIR" => joinpath(inst, "gmt", "share"), "PROJ_DATA" => joinpath(inst, "gmt", "share", "proj"))
	p = try
		run(pipeline(ignorestatus(cmd); stdout=out, stderr=err))
	finally
		rm(pt; force=true)
	end
	o, e = String(take!(out)), String(take!(err))
	return p.exitcode == 0 && occursin(r"^500000\s"m, o) && !occursin("PROJ", e)
end

"""
	_proj_db_repair(zip) -> Bool

TEMPORARY. On an iGMT Windows install whose proj.db its own PROJ rejects, replace
<INSTDIR>\\gmt\\share\\proj with the contents of `zip`. True when nothing is left to do.
"""
function _proj_db_repair(zip::String)::Bool
	inst = _projfix_inst()
	isempty(inst) && return true
	dir   = joinpath(inst, "gmt", "share", "proj")
	stamp = joinpath(dir, _PROJFIX_STAMP)
	isfile(stamp) && return true
	for f in readdir(dir; join=true)                # a previous repair's displaced files
		occursin(".old-", f) && try rm(f; force=true) catch end
	end
	if _projfix_db_ok(inst)
		touch(stamp)
		return true
	end
	if !isfile(zip)
		@warn "iGMT: the bundled PROJ database is out of date and the repair archive is missing" zip
		return false
	end
	println("iGMT: replacing the out-of-date PROJ database in $dir ...")
	tar = joinpath(get(ENV, "SystemRoot", "C:\\Windows"), "System32", "tar.exe")
	tmp = mktempdir()
	try
		run(`$tar -xf $zip -C $tmp`)
		isfile(joinpath(tmp, "proj.db")) || error("$zip has no proj.db")
		stampid = "$(getpid())-$(round(Int, time()))"
		# proj.db FIRST: it is the one a running PROJ may hold, and if it cannot be moved nothing else
		# has been touched yet
		for f in sort(readdir(tmp); by = f -> f != "proj.db")
			dst = joinpath(dir, f)
			isfile(dst) && mv(dst, dst * ".old-$stampid"; force=true)   # fails while PROJ has it open
			cp(joinpath(tmp, f), dst; force=true)
		end
		for f in readdir(dir; join=true)            # nothing holds the displaced files: drop them now
			occursin(".old-", f) && try rm(f; force=true) catch end
		end
	catch e
		@warn "iGMT: could not replace the PROJ database now (in use?) -- it is retried at the next start of iGMT." exception=e
		return false
	finally
		rm(tmp; recursive=true, force=true)
	end
	ok = _projfix_db_ok(inst)
	ok ? (touch(stamp); println("iGMT: PROJ database replaced.")) :
	     @warn "iGMT: the PROJ database was replaced but the bundled GMT still rejects it." dir
	return ok
end
