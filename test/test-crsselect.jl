# The Coordinate Reference System selector (deps/src/73_crsselect.cpp + src/crsselect.jl).

@testitem "crs selector: PROJ catalogue, details and the two-phase protocol" begin
	IG = InteractiveGMT
	L = IG._crs_list()
	rows = split(L, '\n'; keepempty = false)
	@test length(rows) > 10000
	f4326 = split(rows[findfirst(r -> startswith(r, "EPSG:4326\t"), rows)], '\t')
	@test length(f4326) == 12
	@test f4326[2] == "WGS 84" && f4326[3] == "12" && f4326[5] == "1"      # Geographic 2D, has a bbox
	f3763 = split(rows[findfirst(r -> startswith(r, "EPSG:3763\t"), rows)], '\t')
	@test f3763[3] == "15" && f3763[10] == "Transverse Mercator"           # Projected, by its method
	@test all(r -> count(==('\t'), r) == 11, rows)                          # no field leaks a tab
	info = IG._crs_info("EPSG:3763")
	@test startswith(info, "PROJCRS[") && occursin("Portugal TM06", info)
	@test occursin("\n\x1f\n", info) && occursin("+proj=tmerc", info)
	# the protocol as C uses it: the size, then the fill, the same answer
	n = IG._on_crs(Base.unsafe_convert(Cstring, "info EPSG:3763"), Ptr{UInt8}(C_NULL), Cint(0))
	@test n == ncodeunits(info)
	buf = zeros(UInt8, n + 1)
	@test GC.@preserve buf IG._on_crs(Base.unsafe_convert(Cstring, "info EPSG:3763"), pointer(buf), Cint(n + 1)) == n
	@test String(buf[1:n]) == info
	@test GC.@preserve buf IG._on_crs(Base.unsafe_convert(Cstring, "info EPSG:3763"), pointer(buf), Cint(n)) == -1
	C = IG._crs_coast()
	@test count(==('>'), C) > 1000
end

@testitem "crs selector: filter, pick with the mouse, OK; fits a small screen" tags=[:gui] setup=[GmtvtkTest] begin
	register_crs_select_test()
	out = zeros(UInt8, 128)
	flags = ccall(_test_fn(:gmtvtk_crs_select_test), Cint, (Cstring, Cstring, Cstring, Cint, Ptr{UInt8}, Cint),
	              "EPSG:4326", "Portugal TM06", "EPSG:3763", 1, out, length(out))
	@test flags & 1  != 0      # on screen and in front
	@test flags & 2  != 0      # no taller than the screen
	@test flags & 4  != 0      # squeezed to 560x480, every pane still inside and usable
	@test flags & 8  != 0      # the filter left the wanted row visible
	@test flags & 16 != 0      # the preselected system was the current row at open
	@test flags & 32 != 0      # clicking it put its details up
	@test flags & 64 != 0      # OK returned it
	@test GC.@preserve out unsafe_string(pointer(out)) == "EPSG:3763"
end

@testitem "crs selector: Tools > Project > Select CRS... fills the Destination" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	IG._register_crs_select()                  # the window's own DLL asks for the catalogue
	f = view_grid(IG.GMT.peaks(N = 41))
	try
		out = zeros(UInt8, 128)
		flags = ccall(_test_fn(:gmtvtk_project_crs_test), Cint, (Ptr{Cvoid}, Cstring, Cstring, Ptr{UInt8}, Cint),
		              f.h, "Portugal TM06", "EPSG:3763", out, length(out))
		@test flags & 1  != 0      # Tools > Project... opened the Project dialog
		@test flags & 2  != 0      # its "Select CRS..." button is visible
		@test flags & 4  != 0      # the button brought the selector up
		@test flags & 8  != 0      # the pick landed in the Destination box
		@test flags & 16 != 0      # and in the Projections combo
		@test GC.@preserve out unsafe_string(pointer(out)) == "EPSG:3763"
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "crs selector: the status-corner EPSG chip opens it on the window's own system" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	IG._register_crs_select()
	G = IG.GMT.peaks(N = 41)
	G.proj4 = "+proj=utm +zone=29 +datum=WGS84 +units=m +no_defs"
	f = view_grid(G)
	try
		out = zeros(UInt8, 128)
		@test ccall(_test_fn(:gmtvtk_crs_chip_test), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), f.h, out, length(out)) == 1
		@test GC.@preserve out unsafe_string(pointer(out)) == "EPSG:32629"
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end
