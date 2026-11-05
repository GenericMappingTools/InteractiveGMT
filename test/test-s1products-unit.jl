# Satellite > Copernicus > Sentinel-1 SAR products (src/s1products.jl): the catalogue filter, the
# parser of a catalogue answer, the subdataset choice, and (tag :net) one live search.

@testitem "Sentinel-1: catalogue filter and answer" tags=[:unit, :s1] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	f = IG._s1_filter(1, "IW", -10.0, -8.0, 38.0, 40.0, D.Date(2026, 9, 1), D.Date(2026, 10, 15), "DESCENDING", 125)
	@test occursin("Collection/Name eq 'SENTINEL-1'", f)
	@test occursin("POLYGON((-10.0 38.0,-8.0 38.0,-8.0 40.0,-10.0 40.0,-10.0 38.0))", f)
	@test occursin("contains(Name,'_IW_GRD')", f) && occursin("not contains(Name,'_COG')", f)
	@test occursin("att/OData.CSC.StringAttribute/Value eq 'DESCENDING'", f)
	@test occursin("att/OData.CSC.IntegerAttribute/Value eq 125", f)
	f = IG._s1_filter(3, "EW", -10.0, -8.0, 38.0, 40.0, D.Date(2026, 9, 1), D.Date(2026, 9, 2), "", 0)
	@test occursin("contains(Name,'_EW_SLC_')", f) && !occursin("COG", f) && !occursin("orbitDirection", f)
	@test occursin("contains(Name,'_COG')", IG._s1_filter(2, "IW", 0.0, 1.0, 0.0, 1.0, D.Date(2026, 1, 1), D.Date(2026, 1, 2), "", 0))

	# One entry of a real catalogue answer (2026-11-05), trimmed to what the parser reads.
	js = """{"value":[{"Id":"b770306b-8415-4bed-9cfe-b1734737c25a",
	  "Name":"S1C_IW_GRDH_1SDV_20261013T182658_20261013T182723_009870_013AD6_35DA.SAFE","ContentLength":1749208612,
	  "ContentDate":{"Start":"2026-10-13T18:26:58.197346Z","End":"2026-10-13T18:27:23.196516Z"},
	  "GeoFootprint":{"type":"Polygon","coordinates":[[[-9.603388,41.462151],[-9.215427,39.961109],[-6.199116,40.365456],[-6.518022,41.865513],[-9.603388,41.462151]]]},
	  "Attributes":[{"Name":"orbitDirection","Value":"ASCENDING"},{"Name":"relativeOrbitNumber","Value":147}]}]}"""
	p = IG._s1_parse(IG._shub_json(js)["value"][1])
	@test p.id == "b770306b-8415-4bed-9cfe-b1734737c25a"
	@test p.t0 == IG.DateTime(2026, 10, 13, 18, 26, 58)
	@test (p.orbit, p.track, length(p.rings), size(p.rings[1])) == ("ASCENDING", 147, 1, (5, 2))
	@test IG._s1_kind(p) === :grd
	@test basename(IG._s1_file(p, "d")) == "S1C_IW_GRDH_1SDV_20261013T182658_20261013T182723_009870_013AD6_35DA.zip"
	@test startswith(IG._s1_label(p, mktempdir()), "2026-10-13 18:26  S1C  asc  track 147  1.6 GB")
	# A footprint over the antimeridian is a MultiPolygon: one ring each.
	mp = IG._shub_json("""{"type":"MultiPolygon","coordinates":[[[[179,1],[180,1],[180,2],[179,1]]],[[[-180,1],[-179,1],[-180,2],[-180,1]]]]}""")
	@test length(IG._s1_rings(mp)) == 2
end

@testitem "Sentinel-1: the subdataset of a polarisation" tags=[:unit, :s1] begin
	IG = InteractiveGMT
	m = "/vsizip/x.zip/S1.SAFE/manifest.safe"
	sds = ["SENTINEL1_CALIB:SIGMA0:$m:IW_VV:AMPLITUDE", "SENTINEL1_CALIB:SIGMA0:$m:IW_VH:AMPLITUDE",
	       "SENTINEL1_CALIB:UNCALIB:$m:IW_VV:AMPLITUDE", "SENTINEL1_CALIB:UNCALIB:$m:IW_VH:AMPLITUDE"]
	@test IG._s1_pick_sds(sds, "VH", true) == ("SENTINEL1_CALIB:SIGMA0:$m:IW_VH:AMPLITUDE", 20.0)
	@test IG._s1_pick_sds(vcat(sds, "SENTINEL1_CALIB:SIGMA0:$m:IW_VV:INTENSITY"), "VV", true)[2] == 10.0
	@test IG._s1_pick_sds(sds, "VV", false) == ("SENTINEL1_CALIB:UNCALIB:$m:IW_VV:AMPLITUDE", 0.0)
	# No calibration subdatasets (an older GDAL): the plain measurement, no logarithm.
	@test IG._s1_pick_sds(["SENTINEL1_DS:$m:IW_VV", "SENTINEL1_DS:$m:IW"], "VV", true) == ("SENTINEL1_DS:$m:IW_VV", 0.0)
	e = try IG._s1_pick_sds(sds, "HH", true); "" catch x; sprint(showerror, x) end
	@test occursin("no HH band", e) && occursin("VV", e) && occursin("VH", e)
end

@testitem "Sentinel-1: a download without a login says where to give it" tags=[:unit, :s1] begin
	IG = InteractiveGMT
	withenv("NETRC" => joinpath(mktempdir(), "none")) do
		e = try IG._s1_token(Dict{String,String}()); "" catch x; sprint(showerror, x) end
		@test occursin("Copernicus Data Space login", e) && occursin("Account tab", e)
	end
end

@testitem "Sentinel-1: catalogue search (live)" tags=[:net, :s1] begin
	IG = InteractiveGMT
	D = IG.GMT.Dates
	found, total = IG._s1_search(1, "IW", -10.0, -8.0, 38.0, 40.0, D.Date(2026, 10, 1), D.Date(2026, 10, 15), "", 0)
	@test !isempty(found) && total >= length(found)
	@test all(p -> occursin("_IW_GRD", p.name) && !occursin("_COG", p.name), found)
	@test issorted(found, by = p -> p.t0, rev = true)
	@test all(p -> p.track > 0 && p.orbit in ("ASCENDING", "DESCENDING") && !isempty(p.rings), found)
end
