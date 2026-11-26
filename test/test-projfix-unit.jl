# TEMPORARY with src/projfix.jl: the repair of the out-of-date proj.db that iGMT Windows installs made
# before 2026-11-26 carry. A fake install (this machine's GMT, a share folder whose proj.db PROJ
# rejects) must be repaired from deps/proj_data_fix.zip, refuse quietly while proj.db is held open, and
# then never be tested again.

@testitem "projfix: not an iGMT Windows install -> nothing to do" tags=[:unit, :fast] begin
	@test InteractiveGMT._projfix_inst() == ""          # the test julia has no igmt_bundle.ini beside it
	@test InteractiveGMT._proj_db_repair("no-such.zip")  # and the repair says there is nothing to do
end

@testitem "projfix: a rejected proj.db is replaced, a locked one waits, then the stamp ends it" tags=[:unit] begin
	Sys.iswindows() || return @test_skip "the iGMT installer is Windows-only"
	PF  = InteractiveGMT
	zip = joinpath(InteractiveGMT._PKGROOT, "deps", "proj_data_fix.zip")
	@test isfile(zip)
	gmtexe = joinpath(dirname(InteractiveGMT.GMT.libgmt), "gmt.exe")
	@test isfile(gmtexe)
	share = normpath(strip(read(`$gmtexe --show-sharedir`, String)))   # GMT prints '/': mklink refuses those
	inst  = mktempdir()
	links = String[]
	junction(link, target) = (run(`cmd /c mklink /J $link $target`, devnull, devnull, devnull); push!(links, link))
	try
		mkpath(joinpath(inst, "gmt", "share", "proj"))
		junction(joinpath(inst, "gmt", "bin"), dirname(gmtexe))
		for e in readdir(share)
			src = joinpath(share, e)
			e == "proj" && continue
			isdir(src) ? junction(joinpath(inst, "gmt", "share", e), src) : cp(src, joinpath(inst, "gmt", "share", e))
		end
		proj = joinpath(inst, "gmt", "share", "proj")
		db   = joinpath(proj, "proj.db")
		write(db, "not a PROJ database")                 # what PROJ refuses, like the 2021 one
		@test !PF._projfix_db_ok(inst)

		# held open the way SQLite holds it (read/write sharing, no delete): nothing may change
		h = ccall((:CreateFileW, "kernel32"), stdcall, Ptr{Cvoid},
		          (Cwstring, UInt32, UInt32, Ptr{Cvoid}, UInt32, UInt32, Ptr{Cvoid}),
		          db, 0x80000000, 0x00000003, C_NULL, 3, 0x80, C_NULL)
		@test h != Ptr{Cvoid}(-1)
		@test !PF._proj_db_repair(zip; inst)
		ccall((:CloseHandle, "kernel32"), stdcall, Cint, (Ptr{Cvoid},), h)
		@test read(db, String) == "not a PROJ database"
		@test !isfile(joinpath(proj, PF._PROJFIX_STAMP))

		# free: replaced from the zip, accepted by PROJ, stamped, no displaced files left behind
		@test PF._proj_db_repair(zip; inst)
		@test filesize(db) > 1_000_000
		@test PF._projfix_db_ok(inst)
		@test isfile(joinpath(proj, PF._PROJFIX_STAMP))
		@test isempty(filter(f -> occursin(".old-", f), readdir(proj)))

		# stamped: not tested again, even with a broken database put back
		write(db, "not a PROJ database")
		@test PF._proj_db_repair("no-such.zip"; inst)
		@test read(db, String) == "not a PROJ database"
	finally
		foreach(l -> run(`cmd /c rmdir $l`, devnull, devnull, devnull), links)   # the junctions, never their targets
		rm(inst; recursive = true, force = true)
	end
end
