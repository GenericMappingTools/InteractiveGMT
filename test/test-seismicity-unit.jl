# CI-safe unit tests for the Seismicity port (seismicity.jl): the key=value parsing, the
# plain-column catalog readers, the region/date/magnitude/depth filter and the interval
# bucketing. These never open a Qt+VTK window and never touch the network — the USGS web-query
# reader has its own opt-in :net testitem (INTERACTIVEGMT_TEST_NET=1 or test_args=["net"]).
# The dialog itself (PlotSeismicityDialog) is C++ and exercised by the :gui scenarios.

@testitem "seismicity helpers present" tags=[:unit, :fast] begin
	for s in (:_on_seismicity, :_register_seismicity, :_seis_table, :_seis_posit, :_seis_usgs,
	          :_seis_isf, :_seis_filter, :_seis_bucket, :_seis_region, :_seis_bound,
	          :_seis_datestr, :_seis_unix, :_seis_info, :_seis_plot, :_seis_layer)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "seismicity: interval buckets" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# magnitude: <3, 3-5, 5-6, 6-7, 7-8, ≥8
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, 2.0)  == 1
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, 4.5)  == 2
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, 5.0)  == 3
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, 6.9)  == 4
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, 7.0)  == 5
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, 8.6)  == 6
	@test IG._seis_bucket(IG._SEIS_MAG_EDGES, NaN)  == 1     # unknown mag -> smallest bucket
	# depth: <33, 33-70, 70-150, 150-300, ≥300 km
	@test IG._seis_bucket(IG._SEIS_DEP_EDGES, 10.0)  == 1
	@test IG._seis_bucket(IG._SEIS_DEP_EDGES, 33.0)  == 2
	@test IG._seis_bucket(IG._SEIS_DEP_EDGES, 149.9) == 3
	@test IG._seis_bucket(IG._SEIS_DEP_EDGES, 350.0) == 5
end

@testitem "seismicity: dialog-field helpers (region, dates)" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	d = IG._nswing_parse("syear=2000\neyear=2025\nregion=-12.000000/-6.000000/35.000000/39.000000")
	@test IG._seis_region(d) == (-12.0, -6.0, 35.0, 39.0)
	@test IG._seis_region(IG._nswing_parse("region=")) == (-180.0, 180.0, -90.0, 90.0)  # fallback
	# empty month/day default to the interval-appropriate end (Jan 1 / Dec 31)
	@test IG._seis_datestr(d, "s") == "2000-01-01"
	@test IG._seis_datestr(d, "e") == "2025-12-31"
	@test IG._seis_datestr(IG._nswing_parse("syear="), "s") == ""                        # no year -> no bound
	@test IG._seis_bound(d, "s", true) == GMT.Dates.datetime2unix(GMT.Dates.DateTime(2000, 1, 1))
	# end bound covers the WHOLE end day (+86399.999 s)
	@test IG._seis_bound(d, "e", false) > GMT.Dates.datetime2unix(GMT.Dates.DateTime(2025, 12, 31))
	@test IG._seis_bound(IG._nswing_parse(""), "s", true)  == -Inf
	@test IG._seis_bound(IG._nswing_parse(""), "e", false) ==  Inf
end

@testitem "seismicity: plain-column readers" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	mktempdir() do dir
		# format 4: lon,lat,dep,mag,yy,mm,dd (one NaN mag/dep row)
		f4 = joinpath(dir, "quakes4.dat")
		open(f4, "w") do io
			println(io, "-10.0 37.0  10  4.5 2004 12 26")
			println(io, "-8.0  38.0 120  7.3 2020 07 15")
			println(io, "-9.0  37.5 NaN  NaN 2018 06 01")
		end
		lon, lat, dep, mag, t = IG._seis_table(f4, false)
		@test length(lon) == 3 && lon[1] == -10.0 && lat[1] == 37.0
		@test dep[2] == 120.0 && mag[2] == 7.3                       # dep-before-mag layout honoured
		@test isnan(mag[3]) && isnan(dep[3])
		@test GMT.Dates.unix2datetime(t[1]) == GMT.Dates.DateTime(2004, 12, 26)
		# format 3: lon,lat,mag,dep,yy,mm,dd,hh,mm,ss
		f3 = joinpath(dir, "quakes3.dat")
		open(f3, "w") do io
			println(io, "-10.0 37.0 4.5 10 2004 12 26 3 30 15")
		end
		lon3, lat3, dep3, mag3, t3 = IG._seis_table(f3, true)
		@test mag3[1] == 4.5 && dep3[1] == 10.0                      # mag-before-dep layout honoured
		@test GMT.Dates.unix2datetime(t3[1]) == GMT.Dates.DateTime(2004, 12, 26, 3, 30, 15)
	end
end

@testitem "seismicity: filter (region ∩ date ∩ mag ∩ depth, NaN re-admission)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	lon = [-10.0, -9.5, -8.0, -7.5, 50.0, -9.0]
	lat = [ 37.0, 36.5, 38.0, 36.0, 10.0, 37.5]
	dep = [ 10.0, 40.0, 120.0, 350.0, 10.0, NaN]
	mag = [  4.5,  6.1,   7.3,   8.6,  5.0, NaN]
	t   = IG._seis_unix.([2004, 2010, 2020, 1969, 2015, 2018], 6.0, 15.0)
	kv  = "syear=2000\neyear=2025\nmagmin=1\nmagmax=10\ndepmin=0\ndepmax=900\n" *
	      "region=-12.000000/-6.000000/35.000000/39.000000"
	# NaN mag/dep row excluded by default…
	keep = IG._seis_filter(IG._nswing_parse(kv), lon, lat, dep, mag, t)
	@test collect(keep) == [true, true, true, false, false, false]   # 1969 out of date, 50E out of map
	# …and re-admitted by "All magnitudes" + "All depths"
	keep = IG._seis_filter(IG._nswing_parse(kv * "\nallmags=1\nalldeps=1"), lon, lat, dep, mag, t)
	@test collect(keep) == [true, true, true, false, false, true]
	# undated events pass the date filter (the file simply carried no time)
	keep = IG._seis_filter(IG._nswing_parse(kv), lon, lat, dep, mag, fill(NaN, 6))
	@test keep[4] == true
end

@testitem "seismicity: unix-time helper rejects invalid dates" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	@test IG._seis_unix(2004.0, 12.0, 26.0, 3.0, 30.0, 15.0) ==
	      GMT.Dates.datetime2unix(GMT.Dates.DateTime(2004, 12, 26, 3, 30, 15))
	@test isnan(IG._seis_unix(NaN, 1.0, 1.0))
	@test isnan(IG._seis_unix(2004.0, 13.0, 1.0))      # bad month
	@test isnan(IG._seis_unix(2023.0, 2.0, 29.0))      # Feb 29 of a non-leap year
	@test !isnan(IG._seis_unix(2024.0, 2.0, 29.0))     # …but fine on a leap year
end

@testitem "seismicity: hover tooltip" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	t = IG._seis_unix(2004.0, 12.0, 26.0, 3.0, 30.0)
	@test IG._seis_info(5.34, 33.0, t) == "M 5.3\nDepth: 33.0 km\n2004-12-26 03:30"
	@test IG._seis_info(NaN, NaN, NaN) == "earthquake"
	@test IG._seis_info(4.0, NaN, NaN) == "M 4.0"
end

@testitem "seismicity: built-in global catalog (data/quakes.dat)" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	lon, lat, dep, mag, t = IG._seis_default()
	@test length(lon) > 30000                                # the shipped 1990-2009 catalog
	@test all(x -> -180.0 <= x <= 180.0, lon) && all(y -> -90.0 <= y <= 90.0, lat)
	@test all(m -> isnan(m) || 0.0 <= m <= 10.0, mag)
	y0 = GMT.Dates.year(GMT.Dates.unix2datetime(minimum(t)))
	y1 = GMT.Dates.year(GMT.Dates.unix2datetime(maximum(t)))
	@test y0 == 1990 && y1 == 2009
	# format 6 keeps everything with the default dialog fields (whole-world region, no bounds)
	keep = IG._seis_filter(IG._nswing_parse("format=6"), lon, lat, dep, mag, t)
	@test count(keep) == length(lon)
end

# Live USGS web query (network). Opt in with INTERACTIVEGMT_TEST_NET=1 or Pkg.test(test_args=["net"]).
@testitem "seismicity: USGS web query" tags=[:net] begin
	IG = InteractiveGMT; GMT = IG.GMT
	d = IG._nswing_parse("syear=2024\nsmonth=1\nsday=1\neyear=2024\nemonth=1\neday=15\nmagmin=4")
	lon, lat, dep, mag, t = IG._seis_usgs(d, -180.0, 180.0, -90.0, 90.0)
	@test length(lon) > 100                                  # world, M≥4, two weeks: hundreds
	@test all(m -> m >= 4, mag)
	@test all(ti -> GMT.Dates.DateTime(2023, 12, 31) <= GMT.Dates.unix2datetime(ti) <=
	                GMT.Dates.DateTime(2024, 1, 16), t)
end
