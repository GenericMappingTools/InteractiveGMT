# Unit tests for the MB-System GMT plugin installer (src/mbplugin.jl): which release asset a system
# gets, where it goes, and how GMT_CUSTOM_LIBS is extended. The download itself is a :net item.

@testitem "mbplugin: asset and paths for this system" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	a = IG._mbplugin_asset()
	if Sys.islinux() && Sys.ARCH === :x86_64
		@test a == "mbsystem-linux-x86_64.tar.gz"
	elseif Sys.isapple()
		@test a in ("mbsystem-macos-arm64.tar.gz", "mbsystem-macos-x86_64.tar.gz")
	else
		@test a == ""                                    # Windows: the plugin comes with GMT
	end
	@test IG._mbplugin_dir() == joinpath(IG._PKGROOT, "mbsystem")
	@test basename(IG._mbplugin_lib()) == (Sys.isapple() ? "mbsystem.dylib" : "mbsystem.so")
	@test basename(dirname(IG._mbplugin_lib())) == "lib"
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
	rm(dirname(dest); recursive = true, force = true)
end
