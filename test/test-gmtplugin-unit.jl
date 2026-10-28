# Unit tests for the GMT plugin installer (src/gmtplugin.jl), for both plugins it serves (MB-System,
# GMTSAR): which release asset a system gets, where it goes, and how GMT_CUSTOM_LIBS is extended. The
# downloads themselves are :net items.

@testitem "gmtplugin: asset and paths for this system" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for p in (IG.MBSYSTEM_PLUGIN, IG.GMTSAR_PLUGIN)
		a = IG._plugin_asset(p)
		if Sys.islinux() && Sys.ARCH === :x86_64
			@test a == "$(p.name)-linux-x86_64.tar.gz"
		elseif Sys.isapple()
			@test a in ("$(p.name)-macos-arm64.tar.gz", "$(p.name)-macos-x86_64.tar.gz")
		elseif IG.GMT.isJLL
			@test a == "$(p.name)-windows-x86_64$(p.winext)"  # Windows with GMT_jll's GMT: .zip (MB-System) or .tar.gz
			@test IG._plugin_applies()
		else
			@test a == ""                                     # Windows GMT: not built for it
			@test !IG._plugin_applies()
		end
		@test IG._plugin_dir(p) == joinpath(IG._PKGROOT, p.name)
		# never lib<name>.*: GMT takes the plugin's tag from the file name
		@test basename(IG._plugin_lib(p)) ==
		      p.name * (Sys.isapple() ? ".dylib" : Sys.iswindows() ? ".dll" : ".so")
		@test basename(dirname(IG._plugin_lib(p))) == "lib"
		@test IG._plugin_tag(p) == p.name * "-latest"
	end
	@test IG.MBSYSTEM_PLUGIN.repo == "joa-quim/MB-System"
	@test IG.GMTSAR_PLUGIN.repo == "joa-quim/gmtsar"
	@test IG.MBSYSTEM_PLUGIN.id != IG.GMTSAR_PLUGIN.id
end

@testitem "gmtplugin: update check -- the asset's time in the release JSON, the install stamp" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	# the shape of GitHub's release JSON: an uploader object (with no updated_at) inside each asset
	json = """{"assets":[{"name":"mbsystem-linux-x86_64.tar.gz","uploader":{"login":"x"},
	           "updated_at":"2026-10-05T11:23:41Z"},
	          {"name": "mbsystem-windows-x86_64.zip", "uploader": {"login": "x"},
	           "updated_at": "2026-10-07T08:00:00Z"}]}"""
	@test IG._plugin_asset_time(json, "mbsystem-linux-x86_64.tar.gz") == D.DateTime(2026, 10, 5, 11, 23, 41)
	@test IG._plugin_asset_time(json, "mbsystem-windows-x86_64.zip") == D.DateTime(2026, 10, 7, 8)
	@test IG._plugin_asset_time(json, "mbsystem-macos-arm64.tar.gz") === nothing
	d = mktempdir()
	@test IG._plugin_local_time(d) === nothing              # no stamp: an older install
	write(IG._plugin_stamp(d), "2026-10-06T10:00:00")
	@test IG._plugin_local_time(d) == D.DateTime(2026, 10, 6, 10)
	rm(d; recursive = true)
end

@testitem "gmtplugin: clearing the install dir keeps nothing of the old copy" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	d = mktempdir()
	mkpath(joinpath(d, "lib")); mkpath(joinpath(d, "share", "mbsystem"))
	write(joinpath(d, "lib", "libmbio.x"), "a"); write(joinpath(d, "share", "mbsystem", "x.dat"), "b")
	write(joinpath(d, "lib", "old.x.old-1-2"), "c")             # a previous update's leftover
	IG._plugin_clear(d)
	@test isempty([f for (_, _, fs) in walkdir(d) for f in fs])
	rm(d; recursive = true)
end

@testitem "gmtplugin: GMT_CUSTOM_LIBS keeps every entry, adds the plugin once" tags=[:unit, :fast] begin
	f = InteractiveGMT._plugin_custom_libs
	@test f("", "/a/mbsystem.so") == "/a/mbsystem.so"
	@test f("/x/gmtsar.so", "/a/mbsystem.so") == "/x/gmtsar.so,/a/mbsystem.so"
	@test f("/x/gmtsar.so,/a/mbsystem.so", "/a/mbsystem.so") == "/x/gmtsar.so,/a/mbsystem.so"
	@test f(" /x/gmtsar.so , ", "/a/mbsystem.so") == "/x/gmtsar.so,/a/mbsystem.so"
end

@testitem "gmtplugin: the MB-System Linux archive unpacks to mbsystem/lib/mbsystem.so" tags=[:net] begin
	IG = InteractiveGMT
	p = IG.MBSYSTEM_PLUGIN
	dest = joinpath(mktempdir(), "mbsystem")
	lib = IG._plugin_fetch(p, "mbsystem-linux-x86_64.tar.gz", dest)
	@test lib == joinpath(dest, "lib", "mbsystem.so")
	@test isfile(lib)
	@test isfile(joinpath(dest, "lib", "libmbio.so")) || isfile(joinpath(dest, "lib", "libmbio.so.0"))
	@test isfile(joinpath(dest, "share", "mbsystem", "LevitusAnnual82.dat"))
	# the install is stamped with its download time, which the release asset is older than
	t = IG._plugin_local_time(dest)
	@test t !== nothing
	r = IG._plugin_remote_time(p, "mbsystem-linux-x86_64.tar.gz")
	@test r !== nothing && r < t
	rm(dirname(dest); recursive = true, force = true)
end

@testitem "gmtplugin: the GMTSAR Linux archive unpacks to gmtsar/lib/gmtsar.so" tags=[:net] begin
	IG = InteractiveGMT
	p = IG.GMTSAR_PLUGIN
	dest = joinpath(mktempdir(), "gmtsar")
	lib = IG._plugin_fetch(p, "gmtsar-linux-x86_64.tar.gz", dest)
	@test lib == joinpath(dest, "lib", "gmtsar.so")
	@test isfile(lib)
	@test isdir(joinpath(dest, "share", "gmtsar", "filters"))
	t = IG._plugin_local_time(dest)
	@test t !== nothing
	r = IG._plugin_remote_time(p, "gmtsar-linux-x86_64.tar.gz")
	@test r !== nothing && r < t
	rm(dirname(dest); recursive = true, force = true)
end

@testitem "update check: the process can exit while it is still downloading" tags=[:unit, :net] begin
	# The start-up update checks fetch GitHub's release JSON on a background task. Julia 1.10's
	# Downloads crashed the process at exit (ACCESS VIOLATION in curl_multi_cleanup) whenever that
	# transfer was still in flight, e.g. a short test run, or a session closed right after start.
	# _bg_check waits for it at exit. In a CHILD, exiting a moment after the check starts.
	script = """
	using InteractiveGMT
	IG = InteractiveGMT
	IG._bg_check(() -> try IG.GMT.Downloads.download("https://api.github.com/repos/GenericMappingTools/GMT.jl/releases/latest", IOBuffer(); timeout = 20) catch end)
	sleep(0.05)
	print(stderr, "EXITING")
	"""
	err = IOBuffer()
	p = run(pipeline(ignorestatus(`$(Base.julia_cmd()) --startup-file=no -e $script`); stdout = devnull, stderr = err))
	out = String(take!(err))
	@test occursin("EXITING", out)
	@test !occursin("ACCESS_VIOLATION", out)
	@test success(p)
end
