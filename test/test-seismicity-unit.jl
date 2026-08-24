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

@testitem "seismicity: magnitude -> symbol size (USGS scheme)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	pt(m) = IG._seis_mag_size(m) * 72 / 96          # back to POINTS, the scheme's own unit
	@test pt(5.0) ≈ 8.0                              # the anchor: M5 is 8 points
	@test pt(6.0) / pt(5.0) ≈ IG._SEIS_MAG_BASE      # geometric in magnitude
	@test pt(3.0) / pt(2.0) ≈ IG._SEIS_MAG_BASE
	@test pt(IG._SEIS_MAG_HI) / pt(IG._SEIS_MAG_LO) ≈    # the legend's own smallest … biggest span
	      IG._SEIS_MAG_BASE^(IG._SEIS_MAG_HI - IG._SEIS_MAG_LO)
	@test pt(IG._SEIS_MAG_HI + 2.5) == pt(IG._SEIS_MAG_HI)   # the top end saturates ("8+")
	@test pt(IG._SEIS_MAG_LO - 1.0) == pt(IG._SEIS_MAG_LO)   # … and so does the small end
	@test pt(NaN) == pt(IG._SEIS_MAG_LO)             # no magnitude -> smallest
	@test issorted([pt(m) for m in 0:0.5:9])
end

@testitem "seismicity: a collapsed region is repaired, never queried" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test  IG._seis_box_ok(-12.0, -6.0, 35.0, 39.0)
	@test !IG._seis_box_ok(0.0, 0.0, 0.0, 0.0)          # camera gave nothing
	@test !IG._seis_box_ok(-12.0, -12.0, 35.0, 39.0)    # zero width
	@test !IG._seis_box_ok(-12.0, -6.0, 35.0, 35.0)     # zero height
	@test !IG._seis_box_ok(NaN, 1.0, 0.0, 1.0)
	# A usable box passes through untouched …
	@test IG._seis_usable_region(-12.0, -6.0, 35.0, 39.0, nothing) == (-12.0, -6.0, 35.0, 39.0)
	# … a collapsed one falls back to the raster ON DISPLAY …
	@test IG._seis_usable_region(0.0, 0.0, 0.0, 0.0, (-40.0, 0.0, 25.0, 50.0)) == (-40.0, 0.0, 25.0, 50.0)
	# … and to the whole world when there is no raster (or its frame is degenerate too).
	@test IG._seis_usable_region(0.0, 0.0, 0.0, 0.0, nothing) == (-180.0, 180.0, -90.0, 90.0)
	@test IG._seis_usable_region(0.0, 0.0, 0.0, 0.0, (1.0, 1.0, 2.0, 2.0)) == (-180.0, 180.0, -90.0, 90.0)
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

@testitem "seismicity: camera region -> legal query box" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# A fitted world view is WIDER than the map; a full turn (or more) collapses to the whole world.
	@test IG._seis_norm_region(-180.5, 180.5, -90.5, 90.5) == (-180.0, 180.0, -90.0, 90.0)
	@test IG._seis_norm_region(-400.0, 400.0, -10.0, 10.0) == (-180.0, 180.0, -10.0, 10.0)
	# A Pacific window past +180 shifts WHOLE into the negative half (never folded to the Atlantic).
	@test IG._seis_norm_region(150.0, 210.0, -60.0, 60.0) == (-210.0, -150.0, -60.0, 60.0)
	@test IG._seis_norm_region(-12.0, -6.0, 35.0, 39.0)   == (-12.0, -6.0, 35.0, 39.0)   # untouched
	# …and the filter then matches the catalog's own -180…180 longitudes against that shifted box.
	@test IG._seis_inlon(175.0, -210.0, -150.0)            # 175E  == -185, inside
	@test IG._seis_inlon(-170.0, -210.0, -150.0)
	@test !IG._seis_inlon(0.0, -210.0, -150.0)
	@test !IG._seis_inlon(NaN, -180.0, 180.0)
	@test IG._seis_region(IG._nswing_parse("region=150.0/210.0/-60.0/60.0")) == (-210.0, -150.0, -60.0, 60.0)
end

@testitem "seismicity: USGS query URL" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	u = IG._seis_usgs_url(IG._nswing_parse(""), -180.0, 180.0, -90.0, 90.0)
	@test occursin("orderby=time&", u) && !occursin("time-asc", u)   # newest FIRST: the cap drops old
	@test occursin("limit=$(IG._SEIS_USGS_LIMIT)", u)                # explicit, or a big query 400s
	@test occursin("minmagnitude=3.0", u)                            # "Current seismicity" default
	@test !occursin("starttime", u) && !occursin("endtime", u)       # no bounds -> service's last 30 days
	d = IG._nswing_parse("syear=2024\nsmonth=1\nsday=1\neyear=2024\nemonth=1\neday=15\nmagmin=4\nmagmax=8\ndepmin=10\ndepmax=700")
	u2 = IG._seis_usgs_url(d, -12.0, -6.0, 35.0, 39.0)
	@test occursin("starttime=2024-01-01", u2)
	@test occursin("endtime=2024-01-15T23:59:59", u2)                # the WHOLE end day, not midnight
	@test occursin("minmagnitude=4.0", u2) && occursin("maxmagnitude=8.0", u2)
	@test occursin("mindepth=10.0", u2) && occursin("maxdepth=700.0", u2)
	@test occursin("minlongitude=-12.0", u2) && occursin("maxlatitude=39.0", u2)
end

@testitem "seismicity: USGS csv field parsing (no file, no CSV reader)" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	@test IG._seis_isotime("2026-08-24T00:00:48.085Z") ==
	      GMT.Dates.datetime2unix(GMT.Dates.DateTime(2026, 8, 24, 0, 0, 48, 85))
	@test IG._seis_isotime("2026-08-24T00:00:48") ==
	      GMT.Dates.datetime2unix(GMT.Dates.DateTime(2026, 8, 24, 0, 0, 48))
	@test isnan(IG._seis_isotime(""))            # a row that carries no time never kills the catalog
	@test isnan(IG._seis_isotime("not a date"))
	@test IG._seis_num(" 4.5 ") == 4.5 && IG._seis_num("-2.84") == -2.84
	@test isnan(IG._seis_num("")) && isnan(IG._seis_num("null"))
	# The five wanted columns all come BEFORE the service's one quoted field, so a plain split is exact.
	rec = "2026-08-23T23:20:35.049Z,52.0776,176.4797,7.956,5,mb,62,109,1.298,0.97,us,us6000tn19," *
	      "2026-08-24T00:00:48.085Z,\"239 km ESE of Attu Station, Alaska\",earthquake"
	f = split(rec, ','; limit=6)
	@test IG._seis_num(f[3]) == 176.4797 && IG._seis_num(f[4]) == 7.956 && IG._seis_num(f[5]) == 5.0
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
