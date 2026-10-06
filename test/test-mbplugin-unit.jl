# Unit tests for the MB-System GMT plugin installer (src/mbplugin.jl): which release asset a system
# gets, where it goes, and how GMT_CUSTOM_LIBS is extended. The download itself is a :net item.

@testitem "mbplugin: asset and paths for this system" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	a = IG._mbplugin_asset()
	if Sys.islinux() && Sys.ARCH === :x86_64
		@test a == "mbsystem-linux-x86_64.tar.gz"
	elseif Sys.isapple()
		@test a in ("mbsystem-macos-arm64.tar.gz", "mbsystem-macos-x86_64.tar.gz")
	elseif IG.GMT.isJLL
		@test a == "mbsystem-windows-x86_64.tar.gz"      # Windows with GMT_jll's GMT
		@test IG._mbplugin_applies()
	else
		@test a == ""                                    # Windows GMT: the plugin comes with GMT
		@test !IG._mbplugin_applies()
	end
	@test IG._mbplugin_dir() == joinpath(IG._PKGROOT, "mbsystem")
	@test basename(IG._mbplugin_lib()) ==
	      (Sys.isapple() ? "mbsystem.dylib" : Sys.iswindows() ? "mbsystem.dll" : "mbsystem.so")
	@test basename(dirname(IG._mbplugin_lib())) == "lib"
end

@testitem "mbplugin: update check -- the asset's time in the release JSON, the install stamp" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	# the shape of GitHub's release JSON: an uploader object (with no updated_at) inside each asset
	json = """{"assets":[{"name":"mbsystem-linux-x86_64.tar.gz","uploader":{"login":"x"},
	           "updated_at":"2026-10-05T11:23:41Z"},
	          {"name": "mbsystem-windows-x86_64.tar.gz", "uploader": {"login": "x"},
	           "updated_at": "2026-10-07T08:00:00Z"}]}"""
	@test IG._mbplugin_asset_time(json, "mbsystem-linux-x86_64.tar.gz") == D.DateTime(2026, 10, 5, 11, 23, 41)
	@test IG._mbplugin_asset_time(json, "mbsystem-windows-x86_64.tar.gz") == D.DateTime(2026, 10, 7, 8)
	@test IG._mbplugin_asset_time(json, "mbsystem-macos-arm64.tar.gz") === nothing
	d = mktempdir()
	@test IG._mbplugin_local_time(d) === nothing              # no stamp: an older install
	write(IG._mbplugin_stamp(d), "2026-10-06T10:00:00")
	@test IG._mbplugin_local_time(d) == D.DateTime(2026, 10, 6, 10)
	rm(d; recursive = true)
end

@testitem "mbplugin: clearing the install dir keeps nothing of the old copy" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	d = mktempdir()
	mkpath(joinpath(d, "lib")); mkpath(joinpath(d, "share", "mbsystem"))
	write(joinpath(d, "lib", "libmbio.x"), "a"); write(joinpath(d, "share", "mbsystem", "x.dat"), "b")
	write(joinpath(d, "lib", "old.x.old-1-2"), "c")             # a previous update's leftover
	IG._mbplugin_clear(d)
	@test isempty([f for (_, _, fs) in walkdir(d) for f in fs])
	rm(d; recursive = true)
end

@testitem "mbplugin: GMT_CUSTOM_LIBS keeps every entry, adds the plugin once" tags=[:unit, :fast] begin
	f = InteractiveGMT._mbplugin_custom_libs
	@test f("", "/a/mbsystem.so") == "/a/mbsystem.so"
	@test f("/x/gmtsar.so", "/a/mbsystem.so") == "/x/gmtsar.so,/a/mbsystem.so"
	@test f("/x/gmtsar.so,/a/mbsystem.so", "/a/mbsystem.so") == "/x/gmtsar.so,/a/mbsystem.so"
	@test f(" /x/gmtsar.so , ", "/a/mbsystem.so") == "/x/gmtsar.so,/a/mbsystem.so"
end

@testitem "mbplugin: the Linux archive unpacks to mbsystem/lib/mbsystem.so" tags=[:net] begin
	IG = InteractiveGMT
	dest = joinpath(mktempdir(), "mbsystem")
	lib = IG._mbplugin_fetch("mbsystem-linux-x86_64.tar.gz", dest)
	@test lib == joinpath(dest, "lib", "mbsystem.so")
	@test isfile(lib)
	@test isfile(joinpath(dest, "lib", "libmbio.so")) || isfile(joinpath(dest, "lib", "libmbio.so.0"))
	@test isfile(joinpath(dest, "share", "mbsystem", "LevitusAnnual82.dat"))
	# the install is stamped with its download time, which the release asset is older than
	t = IG._mbplugin_local_time(dest)
	@test t !== nothing
	r = IG._mbplugin_remote_time("mbsystem-linux-x86_64.tar.gz")
	@test r !== nothing && r < t
	rm(dirname(dest); recursive = true, force = true)
end
