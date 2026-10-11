# Satellite > SWOT (src/swot.jl): the file-name parser and the pass geometry that filters NASA's CMR
# answers. No network, no data files: the reference values are CMR's own outlines (asked 2026-10-11).

@testitem "SWOT: granule names" tags=[:unit, :swot] begin
	using InteractiveGMT
	const IG = InteractiveGMT
	g = IG._swot_parse("SWOT_L2_LR_SSH_Basic_055_432_20260905T074243_20260905T083312_PID0_01", 1)
	@test (g.cycle, g.pass, g.t0) == (55, 432, IG.DateTime(2026, 9, 5, 7, 42, 43))
	r = IG._swot_parse("SWOT_L2_HR_Raster_250m_UTM29T_N_x_x_x_055_432_040F_20260905T075445_20260905T075506_PID0_01", findfirst(p -> p[1] == "HR water surface, Raster 250 m", IG._SWOT_PRODUCTS))
	@test (r.cycle, r.pass, r.t0) == (55, 432, IG.DateTime(2026, 9, 5, 7, 54, 45))
	@test IG._swot_url(g) == "https://archive.swot.podaac.earthdata.nasa.gov/podaac-swot-ops-cumulus-protected/" *
	                         "SWOT_L2_LR_SSH_D/SWOT_L2_LR_SSH_Basic_055_432_20260905T074243_20260905T083312_PID0_01.nc"
	@test IG._swot_parse("not a swot file", 1) === nothing
end

@testitem "SWOT: every pass from two reference outlines" tags=[:unit, :swot] begin
	using InteractiveGMT
	const IG = InteractiveGMT
	# First vertex longitude of CMR's outline of each pass (same in every cycle).
	for (p, lon1) in ((432, -82.7309064), (460, -85.1966598), (488, -87.6624132), (126, -81.4980296),
	                  (447, -96.9089886), (475, -99.374742), (141, -95.6761119))
		vlon, _ = IG._swot_pass_vertices(p)
		@test abs(mod(vlon[1] - lon1 + 180, 360) - 180) < 1e-6
	end
	# Iberia, September 2026: 432 crosses it; 424 (longitudes 20..188 in its file) does not, although
	# CMR's whole-globe footprint of it says it does.
	@test IG._swot_pass_hits(432, -10.0, -6.0, 36.0, 42.0)
	@test IG._swot_pass_hits(447, -10.0, -6.0, 36.0, 42.0)
	@test !IG._swot_pass_hits(424, -10.0, -6.0, 36.0, 42.0)
	@test IG._swot_pass_hits(424, 0.0, 10.0, 76.0, 80.0)          # past 75 degrees: never filtered
	# Raster tiles: the box's own UTM zones only.
	@test IG._swot_zone_ok("SWOT_L2_HR_Raster_250m_UTM29T_N_x_x_x_055_432_040F_2026", -10.0, -6.0)
	@test !IG._swot_zone_ok("SWOT_L2_HR_Raster_250m_UTM60W_N_x_x_x_055_323_138F_2026", -10.0, -6.0)
	# The swath edges drawn for a pass over the box: inside the box padded by 2 degrees.
	L = IG._swot_pass_lines(432, -10.0, -6.0, 36.0, 42.0)
	@test !isempty(L)
	@test all(M -> all(-12 .<= M[:, 1] .<= -4) && all(34 .<= M[:, 2] .<= 44), L)
end

@testitem "SWOT: river and lake shapefiles" tags=[:unit, :swot] begin
	using InteractiveGMT, InteractiveGMT.GMT
	const IG = InteractiveGMT
	r = IG._swot_parse("SWOT_L2_HR_RiverSP_Reach_055_320_EU_20260901T074809_20260901T075356_PID0_01", findfirst(p -> p[4] === :reach, IG._SWOT_PRODUCTS))
	@test (r.cycle, r.pass, r.t0) == (55, 320, IG.DateTime(2026, 9, 1, 7, 48, 9))
	@test endswith(IG._swot_url(r), "/SWOT_L2_HR_RiverSP_D/SWOT_L2_HR_RiverSP_Reach_055_320_EU_20260901T074809_20260901T075356_PID0_01.zip")
	# A synthetic reach file: one good reach in the box, one suspect with the fill value, one far away.
	td = mktempdir()
	mk(M, a, g) = (d = mat2ds(M); d.attrib = a; d.geom = g; d.proj4 = GMT.prj4WGS84; d)
	L = GMT.wkbLineString
	f = joinpath(td, "r.shp")
	gmtwrite(f, [mk([-8.0 40; -7.5 40.5], Dict("reach_id" => "11", "wse" => "123.25", "reach_q" => "0"), L),
	             mk([-7.0 41; -6.8 41.2], Dict("reach_id" => "12", "wse" => "-999999999999.0", "reach_q" => "1"), L),
	             mk([20.0 50; 21 51],     Dict("reach_id" => "13", "wse" => "5", "reach_q" => "0"), L)])
	F = IG._swot_shp_features(f, "wse", :reach, -10.0, -6.0, 38.0, 43.0, 0)
	@test [s.header for s in F] == ["reach_id 11  wse = 123.25 m"]
	F = IG._swot_shp_features(f, "wse", :reach, -10.0, -6.0, 38.0, 43.0, 1)
	@test [s.header for s in F] == ["reach_id 11  wse = 123.25 m", "reach_id 12  wse = no value"]
	@test IG._swot_vars(f) == ["wse"]                  # numeric, not an id, not a quality flag
	# Nodes: every node in the box in ONE point table, a text per point.
	P = GMT.wkbPoint
	fn = joinpath(td, "n.shp")
	gmtwrite(fn, [mk([-8.0 40.0], Dict("node_id" => "1", "wse" => "10", "node_q" => "0"), P),
	              mk([-7.0 41.0], Dict("node_id" => "2", "wse" => "20", "node_q" => "0"), P)])
	F = IG._swot_shp_features(fn, "wse", :node, -10.0, -6.0, 38.0, 43.0, 2)
	@test length(F) == 1 && size(F[1].data, 1) == 2
	@test F[1].text == ["node_id 1  wse = 10.0 m", "node_id 2  wse = 20.0 m"]
	rm(td; recursive = true, force = true)
end

@testitem "SWOT: AVISO Level-3 names, cycles and login" tags=[:unit, :swot] begin
	using InteractiveGMT
	const IG = InteractiveGMT
	k = findfirst(p -> p[4] === :l3 && p[2] == "Basic", IG._SWOT_PRODUCTS)
	u = "https://tds-odatis.aviso.altimetry.fr/thredds/fileServer/dataset-l3-swot-karin-nadir-validated/l3_lr_ssh/" *
	    "v3_0/Basic/forward/cycle_055/SWOT_L3_LR_SSH_Basic_055_376_20260903T074042_20260903T083208_v3.0.nc"
	g = IG._swot_parse("SWOT_L3_LR_SSH_Basic_055_376_20260903T074042_20260903T083208_v3.0", k; url = u)
	@test (g.cycle, g.pass, g.t0, IG._swot_url(g)) == (55, 376, IG.DateTime(2026, 9, 3, 7, 40, 42), u)
	@test endswith(IG._swot_file(g, "d"), "SWOT_L3_LR_SSH_Basic_055_376_20260903T074042_20260903T083208_v3.0.nc")
	# Science-orbit cycles: start times of cycles 55 and 56 (PO.DAAC and AVISO names), and cycle 1.
	@test IG._swot_cycle_at(IG.DateTime(2026, 9, 5, 7, 42, 43)) == 55
	@test IG._swot_cycle_at(IG.DateTime(2026, 9, 26, 4, 27, 41)) == 56
	@test IG._swot_cycle_at(IG.DateTime(2023, 7, 27)) == 1
	@test_throws ErrorException IG._swot_l3_search(k, -10.0, -6.0, 36.0, 42.0, IG.GMT.Dates.Date(2023, 5, 1), IG.GMT.Dates.Date(2023, 5, 2))
	# No AVISO+ entry in .netrc: refused before anything is asked of the server, and nothing is written.
	td = mktempdir();  nrc = joinpath(td, "netrc")
	write(nrc, "machine urs.earthdata.nasa.gov\n    login x\n    password y\n")
	err = withenv("NETRC" => nrc) do
		try IG._swot_fetch(g, td); "" catch e; sprint(showerror, e) end
	end
	@test err == "Downloading needs your AVISO+ login: save it in the Accounts tab."
	@test readdir(td) == ["netrc"]
	rm(td; recursive = true, force = true)
end

@testitem "SWOT: AVISO Level-3 catalogue search (live)" tags=[:net, :swot] begin
	using InteractiveGMT
	const IG = InteractiveGMT
	k = findfirst(p -> p[4] === :l3 && p[2] == "Expert", IG._SWOT_PRODUCTS)
	D = IG.GMT.Dates
	found, ndrop = IG._swot_l3_search(k, -14.0, -6.0, 36.0, 48.0, D.Date(2024, 1, 1), D.Date(2024, 1, 10))
	@test !isempty(found) && ndrop > 0
	@test all(g -> D.Date(2024, 1, 1) <= D.Date(g.t0) <= D.Date(2024, 1, 10), found)
	@test all(g -> startswith(g.url, "https://tds-odatis.aviso.altimetry.fr/thredds/fileServer/") &&
	               occursin("/v3_0/Expert/", g.url), found)
	@test all(g -> IG._swot_pass_hits(g.pass, -14.0, -6.0, 36.0, 48.0), found)
end
