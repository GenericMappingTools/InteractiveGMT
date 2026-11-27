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

@testitem "HLS field polygon: rings, the working region, and the pixels inside it" tags=[:unit, :remotes] begin
	using InteractiveGMT
	const R = InteractiveGMT.RemoteS
	const GMT = InteractiveGMT.GMT

	# Rings: an open lon/lat ring is closed; fewer than 3 vertices, or not lon/lat, is refused.
	r = R._hls_rings([0.0 0.0; 1.0 0.0; 1.0 1.0])
	@test length(r) == 1 && size(r[1]) == (4, 2) && r[1][1, :] == r[1][end, :]
	@test_throws ErrorException R._hls_rings([0.0 0.0; 1.0 0.0])
	@test_throws ErrorException R._hls_rings([500000.0 4.2e6; 500100.0 4.2e6; 500100.0 4.2001e6])
	@test R._hls_rings(nothing) == Matrix{Float64}[]
	@test R._hls_work_region([1.0, 2.0, 3.0, 4.0], Matrix{Float64}[]) == [1.0, 2.0, 3.0, 4.0]
	@test R._hls_work_region(Float64[], r) == [0.0, 1.0, 0.0, 1.0]
	@test_throws ErrorException R._hls_work_region(Float64[], Matrix{Float64}[])

	# A square field in UTM 29N, given in lon/lat, placed back on a 30 m UTM grid: the pixels whose
	# centre is inside it, and no other — for both memory layouts and both registrations.
	utm = "+proj=utm +zone=29 +datum=WGS84 +units=m +no_defs"
	sq = [500045.0 4200075.0; 500165.0 4200075.0; 500165.0 4200225.0; 500045.0 4200225.0; 500045.0 4200075.0]
	ll = InteractiveGMT._vector_xy_project(sq, utm, InteractiveGMT._LONLAT)
	# The same field as a UTM GMTdataset: converted with its own CRS.
	D = GMT.mat2ds(sq); D.proj4 = utm
	@test isapprox(R._hls_rings(D)[1], ll; atol = 1e-9)
	rings = R._hls_rings(ll)
	for reg in (0, 1)
		# node / cell-edge origin and extent: 11 x 11 centres at 500000 + 30k, 4200000 + 30k either way
		x0, y0, w = 500000.0 - 15reg, 4200000.0 - 15reg, 300.0 + 30reg
		G = GMT.mat2grid(zeros(Float32, 11, 11); x = [x0, x0 + w], y = [y0, y0 + w], reg = reg, proj4 = utm)
		@test G.inc == [30.0, 30.0]
		nx, ny = InteractiveGMT._grid_dims(G)
		want = falses(ny, nx)                       # (iy, ix), row 1 = south
		h = reg == 1 ? 15.0 : 0.0
		for ix in 1:nx, iy in 1:ny
			x, y = G.range[1] + h + 30(ix - 1), G.range[3] + h + 30(iy - 1)
			want[iy, ix] = !(500045 <= x <= 500165 && 4200075 <= y <= 4200225)
		end
		@test count(!, want) == 20                  # 4 columns x 5 rows of centres inside the square
		for lay in ("BCB", "TRB")
			T = deepcopy(G)
			if lay == "TRB"                         # the same grid, row-major and north-first
				T.z = reshape(vec(permutedims(reverse(G.z, dims = 1))), size(G.z))
				T.layout = "TRB"
			end
			out = R._hls_outside(T, rings)
			@test size(out) == size(T.z)
			C = deepcopy(T);  C.z = Float32.(out)
			@test Bool.(InteractiveGMT._zmat(C)) == want
		end
	end
end
