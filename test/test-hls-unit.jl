# HLS (Harmonized Landsat Sentinel-2) time series: src/RemoteS/hls.jl.

@testitem "HLS: a STAC feature becomes an HLSItem, anything else is skipped" tags=[:unit, :remotes] begin
	using InteractiveGMT
	import InteractiveGMT.GMT.Dates: DateTime
	const R = InteractiveGMT.RemoteS

	js = InteractiveGMT._shub_json("""{"id":"HLS.S30.T29SNB.2024104T112119.v2.0",
		"properties":{"datetime":"2024-04-13T11:31:10.123Z","eo:cloud_cover":7},
		"assets":{"B04":{"href":"https://data.lpdaac.earthdatacloud.nasa.gov/x/HLS.S30.T29SNB.2024104T112119.v2.0.B04.tif"},
		          "Fmask":{"href":"https://data.lpdaac.earthdatacloud.nasa.gov/x/HLS.S30.T29SNB.2024104T112119.v2.0.Fmask.tif"},
		          "thumbnail":{"href":"https://data.lpdaac.earthdatacloud.nasa.gov/x/HLS.S30.T29SNB.jpg"},
		          "s3":{"href":"s3://lp-prod-protected/x/HLS.S30.T29SNB.2024104T112119.v2.0.B8A.tif"}}}""")
	it = R._hls_item(js)
	@test it isa R.HLSItem
	@test it.product == "S30" && it.tile == "T29SNB"
	@test it.time == DateTime(2024, 4, 13, 11, 31, 10, 123)
	@test it.cloud == 7.0
	@test sort(collect(keys(it.assets))) == ["B04", "Fmask"]      # https .tif only: no jpg, no s3://
	@test R._hls_item(InteractiveGMT._shub_json("""{"id":"OPERA_L3_DSWx","properties":{"datetime":"2024-04-13T11:31:10Z"}}""")) === nothing
	@test R._hls_item(InteractiveGMT._shub_json("""{"id":"HLS.L30.T29SNB.2024107T110743.v2.0","properties":{}}""")) === nothing
end

@testitem "Sentinel-2 (Earth Search): a feature becomes an item, and the offset is never applied twice" tags=[:unit, :remotes] begin
	using InteractiveGMT
	import InteractiveGMT.GMT.Dates: DateTime
	const R = InteractiveGMT.RemoteS

	feat(applied) = InteractiveGMT._shub_json("""{"id":"S2B_29SNC_20261030_1_L2A",
		"properties":{"datetime":"2026-10-30T11:30:44.549000Z","eo:cloud_cover":12.5,"earthsearch:boa_offset_applied":$(applied)},
		"assets":{"red":{"href":"https://sentinel-cogs.s3.us-west-2.amazonaws.com/x/B04.tif",
		                 "raster:bands":[{"scale":0.0001,"offset":-0.1,"nodata":0}]},
		          "scl":{"href":"https://sentinel-cogs.s3.us-west-2.amazonaws.com/x/SCL.tif"},
		          "red-jp2":{"href":"s3://sentinel-s2-l2a/x/B04.jp2"}}}""")
	it = R._es_item(feat(true))
	@test it.product == "S2B" && it.tile == "T29SNC" && it.cloud == 12.5
	@test it.time == DateTime(2026, 10, 30, 11, 30, 44, 549)
	@test sort(collect(keys(it.assets))) == ["red", "scl"]
	@test it.scale == 0.0001 && it.offset == 0.0 && it.fill == 0f0       # the pixels already carry the offset
	@test R._es_item(feat(false)).offset == -0.1                         # not applied: raster:bands says how
	@test R._es_seq("S2B_29SNC_20261030_1_L2A") == 1 && R._es_seq("S2B_29SNC_20261030_0_L2A") == 0
	@test R._es_item(InteractiveGMT._shub_json("""{"id":"HLS.S30.T29SNB.2024104T112119.v2.0","properties":{}}""")) === nothing
	# SCL: no data, saturated, cloud shadow, clouds, cirrus and snow are masked; land, water, dark areas kept
	@test all(R._scl_bad, Float32[0, 1, 3, 8, 9, 10, 11])
	@test !any(R._scl_bad, Float32[2, 4, 5, 6, 7])
	@test R._fmask_bad(Float32(0b10)) && !R._fmask_bad(Float32(0b100000))   # cloud bit vs water bit
end

@testitem "HLS: band roles differ per product, and bad requests fail before any download" tags=[:unit, :remotes] begin
	using InteractiveGMT
	const R = InteractiveGMT.RemoteS

	@test R._HLS_BANDS["L30"]["nir"] == "B05" && R._HLS_BANDS["S30"]["nir"] == "B8A"
	@test R._HLS_BANDS["L30"]["swir1"] == "B06" && R._HLS_BANDS["S30"]["swir1"] == "B11"
	@test !haskey(R._HLS_BANDS["L30"], "redEdge1")
	@test R._HLS_INDEX["NDMI"] == ["nir", "swir1"]
	box = [-7.98, -7.95, 37.08, 37.10]
	@test_throws "unknown index" R.hls_series(box; from = "2024-04-01", to = "2024-04-30", index = "XYZ")
	@test_throws "does not have" R.hls_series(box; from = "2024-04-01", to = "2024-04-30", index = "NDREI1", products = ["L30"])
	@test_throws "W < E" R.hls_search([1.0, 0.0, 37.0, 38.0]; from = "2024-04-01", to = "2024-04-30")
	@test_throws "ends before" R.hls_search(box; from = "2024-05-01", to = "2024-04-01")
end

@testitem "HLS: the parallel reads keep their order and tick once each" tags=[:unit, :remotes] begin
	using InteractiveGMT
	const R = InteractiveGMT.RemoteS

	n = Ref(0)
	xs = string.(1:20)
	out = R._hls_pmap(x -> (sleep(0.01 * (21 - parse(Int, x))); parse(Int, x)^2), xs, () -> (n[] += 1))
	@test out == (1:20) .^ 2
	@test n[] == 20
end

@testitem "HLS: a real NDMI date over a Faro field (needs the Earthdata login)" tags=[:net, :remotes] begin
	using InteractiveGMT
	import InteractiveGMT.GMT.Dates: Date
	const R = InteractiveGMT.RemoteS

	box = [-7.98, -7.95, 37.08, 37.10]
	its = R.hls_search(box; from = "2024-04-01", to = "2024-04-30")
	@test !isempty(its)
	@test issorted(its, by = it -> it.time)
	@test "T29SNB" in [it.tile for it in its]
	u, p = InteractiveGMT._rs_netrc_get(InteractiveGMT._rs_netrc_path(), InteractiveGMT._RS_URS)
	if isempty(u) || isempty(p)
		@info "HLS: no urs.earthdata.nasa.gov entry in .netrc, the read part is skipped"
	else
		pr = Int[]
		S = R.hls_series(box; from = "2024-04-13", to = "2024-04-13", index = "NDMI", progress = (pc, m) -> push!(pr, pc))
		@test S.tile == "T29SNB"
		@test length(S.grids) == 1 && Date(S.time[1]) == Date(2024, 4, 13) && S.product == ["S30"]
		@test S.clear[1] == 1.0
		@test isapprox(S.mean[1], 0.1197; atol = 5e-4)          # measured 2026-11-11
		@test !any(isnan, S.grids[1].z)
		@test issorted(pr) && first(pr) == 2 && last(pr) == 95
	end
end

@testitem "Sentinel-2 (Earth Search): 10 m NDVI and 20 m NDMI over a Faro field, matching HLS" tags=[:net, :remotes] begin
	using InteractiveGMT
	import InteractiveGMT.GMT.Dates: Date
	const R = InteractiveGMT.RemoteS

	box = [-7.98, -7.95, 37.08, 37.10]
	V = R.s2_series(box; from = "2024-04-13", to = "2024-04-13", index = "NDVI")
	@test V.tile == "T29SNB" && V.product == ["S2B"] && Date(V.time[1]) == Date(2024, 4, 13)
	@test V.grids[1].inc[1] == 10.0                                  # red + NIR: the 10 m bands
	@test isapprox(V.mean[1], 0.5198; atol = 5e-4)                   # HLS S30 of the same take: 0.5252
	M = R.s2_series(box; from = "2024-04-13", to = "2024-04-13", index = "NDMI")
	@test M.grids[1].inc[1] == 20.0                                  # SWIR1 is 20 m, so NIR = B8A
	@test isapprox(M.mean[1], 0.1091; atol = 5e-4)                   # HLS S30: 0.1197
	@test size(M.grids[1].z) == (136, 114) && size(V.grids[1].z) == (270, 226)
end
