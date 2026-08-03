# CI-safe unit tests for Tools > Ocean Color Data Browser (oceancolor.jl). Everything here is pure
# arithmetic and string building — the composite calendar and the OB.DAAC file naming. The one test
# that talks to the servers is tagged :net and is skipped unless INTERACTIVEGMT_TEST_NET=1.

@testitem "oceancolor: catalogue rows match the .ui combos" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for s in (:_OC_INSTRUMENTS, :_OC_PRODUCTS, :_OC_PERIODS, :_oc_period_span, :_oc_stamp,
	          :_oc_basenames, :_oc_png_urls, :oc_latest, :oc_fetch_png, :oc_png_georef, :oc_data_url)
		@test isdefined(IG, s)
	end
	# Order is load-bearing: the dialog sends a combo INDEX, never a label.
	@test [i.label for i in IG._OC_INSTRUMENTS] ==
	      ["Aqua-MODIS", "Terra-MODIS", "SNPP-VIIRS", "NOAA20-VIIRS"]
	@test [p[2] for p in IG._OC_PERIODS] == ["DAY", "8D", "MO", "YR"]
	@test length(IG._OC_PRODUCTS) == 2

	# Mission start days, and the annual-file ranges produced so far. NOAA-20 has no annual product
	# at all, which the Period combo needs in order to grey "Annual" out.
	D = IG.Dates.Date
	@test [i.first for i in IG._OC_INSTRUMENTS] ==
	      [D(2002, 7, 4), D(2000, 2, 24), D(2012, 1, 2), D(2017, 12, 13)]
	@test [(i.yr_first, i.yr_last) for i in IG._OC_INSTRUMENTS] ==
	      [(2002, 2025), (2000, 2024), (2012, 2024), (0, 0)]
end

@testitem "oceancolor: composite calendar" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	D = IG.Dates.Date

	@test IG._oc_period_span("DAY", D(2026, 8, 1)) == (D(2026, 8, 1), D(2026, 8, 1))
	@test IG._oc_period_span("MO",  D(2026, 6, 17)) == (D(2026, 6, 1), D(2026, 6, 30))
	@test IG._oc_period_span("MO",  D(2024, 2, 9))  == (D(2024, 2, 1), D(2024, 2, 29))  # leap
	@test IG._oc_period_span("YR",  D(2026, 6, 17)) == (D(2026, 1, 1), D(2026, 12, 31))

	# 8-day buckets restart at day-of-year 1 every year: 1, 9, 17, … 361.
	@test IG._oc_period_span("8D", D(2026, 1, 1))  == (D(2026, 1, 1), D(2026, 1, 8))
	@test IG._oc_period_span("8D", D(2026, 7, 21)) == (D(2026, 7, 20), D(2026, 7, 27))
	@test IG._oc_period_span("8D", D(2020, 7, 21)) == (D(2020, 7, 19), D(2020, 7, 26))  # leap shift
	# The last bucket of a year is short and never crosses New Year (2026: DOY 361 = 27 Dec).
	@test IG._oc_period_span("8D", D(2026, 12, 31)) == (D(2026, 12, 27), D(2026, 12, 31))
	@test IG._oc_period_prev("8D", D(2026, 1, 1))   == D(2025, 12, 27)
	@test IG._oc_period_prev("MO", D(2026, 1, 1))   == D(2025, 12, 1)
end

@testitem "oceancolor: URL construction" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	D = IG.Dates.Date
	aqua, terra, snpp, n20 = IG._OC_INSTRUMENTS
	sst, chl = IG._OC_PRODUCTS

	# The exact daily address the browser was specified against.
	u = IG._oc_png_urls(aqua, sst, "DAY", D(2026, 7, 31))
	@test u[1] == "https://oceandata.sci.gsfc.nasa.gov/showimages/MODISA/L3BRS/2026/0731/" *
	              "AQUA_MODIS.20260731.L3m.DAY.NSST.sst.4km.NRT.nc.png"
	# NRT first, reprocessed second — an old date answers on the second try.
	@test endswith(u[2], "AQUA_MODIS.20260731.L3m.DAY.NSST.sst.4km.nc.png")

	# Per-instrument directory and file prefix.
	@test occursin("/MODIST/", IG._oc_png_urls(terra, sst, "DAY", D(2026, 7, 31))[1])
	@test occursin("TERRA_MODIS.", IG._oc_png_urls(terra, sst, "DAY", D(2026, 7, 31))[1])
	@test occursin("/VIIRS/L3BRS/", IG._oc_png_urls(snpp, sst, "DAY", D(2026, 7, 31))[1])
	@test occursin("SNPP_VIIRS.", IG._oc_png_urls(snpp, sst, "DAY", D(2026, 7, 31))[1])
	@test occursin("/VIIRSJ1/L3BRS/", IG._oc_png_urls(n20, sst, "DAY", D(2026, 7, 31))[1])
	@test occursin("JPSS1_VIIRS.", IG._oc_png_urls(n20, sst, "DAY", D(2026, 7, 31))[1])

	# Composites carry a start_end stamp and live under the START day's directory.
	@test IG._oc_basenames(aqua, chl, "8D", D(2026, 7, 21))[1] ==
	      "AQUA_MODIS.20260720_20260727.L3m.8D.CHL.chlor_a.4km.NRT.nc"
	@test occursin("/2026/0601/", IG._oc_png_urls(aqua, sst, "MO", D(2026, 6, 30))[1])
	@test occursin("/2025/0101/", IG._oc_png_urls(aqua, sst, "YR", D(2025, 8, 9))[1])
	# Annual products are never near-real-time, so there is only one spelling to try.
	@test IG._oc_basenames(aqua, sst, "YR", D(2025, 8, 9)) ==
	      ["AQUA_MODIS.20250101_20251231.L3m.YR.NSST.sst.4km.nc"]

	# The L3 grid is the L4 image's name minus ".png" — a pure rename, no second catalogue.
	@test IG.oc_data_url("AQUA_MODIS.20260731.L3m.DAY.NSST.sst.4km.NRT.nc") ==
	      "https://oceandata.sci.gsfc.nasa.gov/cgi/getfile/AQUA_MODIS.20260731.L3m.DAY.NSST.sst.4km.NRT.nc"
	@test IG.oc_data_url_of_png(u[1]) == IG.oc_data_url("AQUA_MODIS.20260731.L3m.DAY.NSST.sst.4km.NRT.nc")
	@test IG._oc_basename_of(u[1]) == "AQUA_MODIS.20260731.L3m.DAY.NSST.sst.4km.NRT.nc"
end

@testitem "oceancolor: browse-image georeferencing" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	g = IG.oc_png_georef(IG._OC_PRODUCTS[1])			# 4 km
	@test g.region == (-180.0, 180.0, -90.0, 90.0)
	@test (g.nx, g.ny) == (8640, 4320)
	@test g.inc ≈ 360.0 / 8640
	@test g.registration == 1 && g.geographic
	@test IG._oc_png_size("9km") == (4320, 2160)
end

@testitem "oceancolor: the server really answers where we look" tags=[:net] begin
	IG = InteractiveGMT
	# Every instrument must resolve a recent DAILY image of both products, and report the two newest
	# in descending date order. This is the check that catches a server-side rename.
	for i in 1:length(IG._OC_INSTRUMENTS), ip in 1:length(IG._OC_PRODUCTS)
		r = IG.oc_latest(i, ip, 1; n = 2)
		@test length(r) == 2
		@test r[1].start > r[2].start
		@test occursin(IG._OC_INSTRUMENTS[i].prefix, r[1].url)
		@test occursin(IG._OC_PRODUCTS[ip].suite, r[1].url)
	end
	# And the composite periods, on the instrument with the longest record.
	for ip in 2:4
		@test length(IG.oc_latest(1, 1, ip; n = 2)) == 2
	end
end
