# Geophysics > MB-System > 3-D mesh (mbmesh) (src/mbmesh_dialog.jl): what the tool takes as input, the
# output folder it proposes, and the points it hands mbmesh in memory, taken to lon/lat.

@testitem "mbmesh: input kinds and the default output folder" tags=[:unit, :mbmesh] begin
	IG = InteractiveGMT
	@test IG._mbmesh_is_swath("a/b/TN136HS.309.mb11") && IG._mbmesh_is_swath("x.MB-1") && IG._mbmesh_is_swath("f.mb261")
	@test !IG._mbmesh_is_swath("f.mb") && !IG._mbmesh_is_swath("f.las")
	@test all(IG._mbmesh_is_points, ["p.las", "p.LAZ", "p.xyz", "p.txt", "p.csv", "s.mb57"])
	@test !IG._mbmesh_is_points("g.grd") && !IG._mbmesh_is_points("i.tif")
	@test IG._mbmesh_default_outdir(joinpath("d", "survey.laz")) == joinpath("d", "survey_mbmesh")
	@test occursin("Choose the point data", IG._mbmesh_describe(""))
	@test occursin("No such file", IG._mbmesh_describe(joinpath(mktempdir(), "nope.las")))
end

@testitem "mbmesh: point data becomes lon/lat points for mbmesh, in memory" tags=[:unit, :mbmesh] begin
	IG = InteractiveGMT
	dir = mktempdir()
	try
		# lon/lat by its range: as it is, z kept as a height; the row with no z is left out
		ll = joinpath(dir, "pts.xyz")
		write(ll, "-124.5027 40.8379 -461.0
-124.5020 40.8382 -462.5
-124.5010 40.8388 -463.0
-124.5015 40.8380 NaN
")
		M = IG._mbmesh_table_points(ll)
		@test M ≈ [-124.5027 40.8379 -461.0; -124.5020 40.8382 -462.5; -124.5010 40.8388 -463.0]

		# projected (PT-TM06 / ETRS89, EPSG:3763): refused without a system, taken to lon/lat with one
		pr = joinpath(dir, "pt.xyz")
		write(pr, "5334.407 -271630.531 160.3
5354.593 -271613.251 169.2
5340.0 -271620.0 165.0
")
		@test_throws "no coordinate system" IG._mbmesh_table_points(pr)
		M = IG._mbmesh_table_points(pr, "EPSG:3763")
		# ~10 m: the place, not the datum shift the transform chooses between ETRS89 and WGS 84
		@test size(M, 1) == 3 && isapprox(M[1, 1], -8.072978; atol = 1e-4) && isapprox(M[1, 2], 37.221233; atol = 1e-4)
		@test M[:, 3] == [160.3, 169.2, 165.0]

		# a LAZ file, read without its system: the CRS box supplies it
		lz = joinpath(dir, "pt.laz")
		IG.GMT.lazwrite(lz, [5334.407 -271630.531 160.3; 5354.593 -271613.251 169.2; 5340.0 -271620.0 165.0])
		M = IG._mbmesh_table_points(lz, "EPSG:3763")
		@test size(M, 1) == 3 && isapprox(M[1, 1], -8.072978; atol = 1e-4) && isapprox(M[1, 2], 37.221233; atol = 1e-4)

		two = joinpath(dir, "two.xyz");  write(two, "1 2
3 4
5 6
")
		@test_throws "third column" IG._mbmesh_table_points(two)
		@test_throws "not found" IG._mbmesh_table_points(joinpath(dir, "nope.xyz"))
		@test readdir(dir) == ["pt.laz", "pt.xyz", "pts.xyz", "two.xyz"]   # nothing written beside them
	finally
		rm(dir; recursive = true, force = true)
	end
end
