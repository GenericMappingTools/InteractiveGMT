# CI-safe unit tests for the NOAA historical tsunami catalog (tsunamicatalog.jl): the shipped
# .dat's layout, the NCEI code books, the partial-date builder, the cell formatter and the reader's
# region clip / coordinate-less-record drop. These never open a Qt+VTK window and never touch the
# network. The layer itself (add_symbols!) is exercised by the :gui scenarios.

@testitem "noaa tsunami helpers present" tags=[:unit, :fast] begin
	for s in (:_noaa_tsunami_data, :_noaa_tsu_info, :_noaa_tsu_row, :_noaa_tsu_cell,
	          :_noaa_tsu_date, :_plot_noaa_tsunami, :_NOAA_TSU_COLS, :_NOAA_TSU_CAUSE,
	          :_NOAA_TSU_VALIDITY, :_NOAA_TSU_TABLE_COLS, :_geo_select)
		@test isdefined(InteractiveGMT, s)
	end
end

# The .dat is data, and a silently re-ordered column would mis-label every tooltip in the tool
# without failing anything else. So the file's own `#` header is checked against the indices
# tsunamicatalog.jl reads by.
@testitem "noaa tsunami: the shipped .dat matches the column indices the code reads by" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	path = joinpath(IG._PKGROOT, "data", "noaa_historical_tsunami_events.dat")
	@test isfile(path)
	# Collect first, pick after: `hdr = ln` INSIDE the loop is an assignment to a global from soft
	# scope, so Julia made a fresh local each iteration and `hdr` stayed "" — every assertion below
	# then ran on an empty header and "failed" for a reason that had nothing to do with the .dat.
	comments = String[]
	for ln in eachline(path)
		startswith(ln, "#") || break
		push!(comments, ln)                     # push! mutates; no rebinding, no soft-scope trap
	end
	i = findlast(l -> occursin("lon", l) && occursin("lat", l), comments)
	hdr = i === nothing ? "" : comments[i]
	@test !isempty(hdr)
	names = split(strip(lstrip(hdr, ['#', ' '])), '\t')
	@test names[1] == "lon" && names[2] == "lat"
	@test names[end] == "country"                       # the trailing text, and the ONLY text field
	@test names[IG._NOAA_TSU_YEAR]  == "year"
	@test names[IG._NOAA_TSU_MONTH] == "month"
	@test names[IG._NOAA_TSU_DAY]   == "day"
	@test names[IG._NOAA_TSU_MAG]   == "eqMagnitude"
	# deaths / housesDestroyed are the TSUNAMI's own toll: NCEI's `*Total` columns count the whole
	# source event (Haiti 2010: 7 vs 316000) and must never be the ones shipped here.
	@test "deaths" in names && !("deathsTotal" in names)
	@test "housesDestroyed" in names && !("housesDestroyedTotal" in names)
	# every numeric column the code names must exist, and be a numeric column (not the text tail)
	for (ci, _, _) in IG._NOAA_TSU_COLS
		@test ci <= length(names) - 1
	end
end

@testitem "noaa tsunami: NCEI cause codes" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	cause(v) = IG._noaa_tsu_cell(Float64(v), -2)
	@test cause(0)  == "Unknown"
	@test cause(1)  == "Earthquake"
	@test cause(2)  == "Questionable Earthquake"
	@test cause(3)  == "Earthquake and Landslide"
	@test cause(4)  == "Volcano and Earthquake"
	@test cause(5)  == "Volcano, Earthquake and Landslide"
	@test cause(6)  == "Volcano"
	@test cause(7)  == "Volcano and Landslide"
	@test cause(8)  == "Landslide"
	@test cause(9)  == "Meteorological"
	@test cause(10) == "Explosion"
	# NCEI publishes no label for 11 (2 events, neither with coordinates) — it stays a NUMBER, and
	# so does anything else unlabelled. A made-up meaning here would be worse than the bare code.
	@test cause(11) == "11"
	@test cause(99) == "99"
	@test cause(-5) == "-5"
	@test IG._noaa_tsu_cell(NaN, -2) == ""
end

@testitem "noaa tsunami: NCEI event-validity codes" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	val(v) = IG._noaa_tsu_cell(Float64(v), -3)
	# The scale starts at -1, so the table is indexed by code+2 — an off-by-one here would shift
	# every label by one step of confidence, which no other test would notice.
	@test val(-1) == "Erroneous entry"
	@test val(0)  == "Event that only caused a seiche"
	@test val(1)  == "Very Doubtful Tsunami"
	@test val(2)  == "Questionable Tsunami"
	@test val(3)  == "Probable Tsunami"
	@test val(4)  == "Definite Tsunami"
	@test val(5)  == "5"                                 # unlabelled -> the number
	@test val(-2) == "-2"
	@test IG._noaa_tsu_cell(NaN, -3) == ""
end

@testitem "noaa tsunami: cell formatting" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._noaa_tsu_cell(NaN, 0) == ""                # missing is blank, never the number zero
	@test IG._noaa_tsu_cell(NaN, 2) == ""
	@test IG._noaa_tsu_cell(0.0, 0) == "0"               # …but a real zero still prints
	@test IG._noaa_tsu_cell(1279.0, 0) == "1279"
	@test IG._noaa_tsu_cell(9.5, 1) == "9.5"
	@test IG._noaa_tsu_cell(25.0, 2) == "25.0"
	@test IG._noaa_tsu_cell(1.0, -1) == "True"           # the 0/1 made out of the source True/False
	@test IG._noaa_tsu_cell(0.0, -1) == "False"
end

@testitem "noaa tsunami: a date carries only what the event has" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	d(y, m, dy) = IG._noaa_tsu_date(Float64(y), Float64(m), Float64(dy))
	@test d(2011, 3, 11) == "2011-03-11"
	@test d(1960, 5, 22) == "1960-05-22"
	@test IG._noaa_tsu_date(1650.0, 4.0, NaN) == "1650-04"      # no day -> no fabricated 1st
	@test IG._noaa_tsu_date(1650.0, NaN, NaN) == "1650"         # no month either
	@test IG._noaa_tsu_date(NaN, NaN, NaN) == ""                # no date at all
	# The catalog reaches back to 2100 BC: a negative year is a real year, not an error.
	@test IG._noaa_tsu_date(-2000.0, NaN, NaN) == "-2000"
	@test IG._noaa_tsu_date(-1610.0, NaN, NaN) == "-1610"
	# An impossible month/day is dropped, never rolled over into the next month.
	@test d(1900, 13, 1) == "1900"
	@test d(1900, 0, 1)  == "1900"
	@test d(1900, 2, 30) == "1900-02"
	@test d(1900, 2, 28) == "1900-02-28"
end

@testitem "noaa tsunami: reader clips to the view and drops the position-less records" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	xs, ys, data, country = IG._noaa_tsunami_data(-180.0, 180.0, -90.0, 90.0)
	@test length(xs) > 2000
	@test length(xs) == length(ys) == length(country) == size(data, 1)
	@test size(data, 2) == length(IG._NOAA_TSU_COLS) + 2        # lon/lat + every named column
	# NCEI leaves 464 of the 3130 records without a lat/lon. gmtselect passes those through
	# UNTESTED (there is nothing to test them against), so they arrive whatever region was asked
	# for; stamped as-is they would be symbols at NaN,NaN. The reader must drop them itself.
	@test !any(isnan, xs)
	@test !any(isnan, ys)
	# …and the clip is real: a small box holds far fewer events, all of them inside it.
	xj, yj, dj, cj = IG._noaa_tsunami_data(120.0, 150.0, 25.0, 50.0)
	@test 0 < length(xj) < length(xs)
	@test all(120 .<= xj .<= 150)
	@test all(25 .<= yj .<= 50)
	@test length(xj) == size(dj, 1) == length(cj)
	# A box with no events at all comes back empty, not broken.
	xe, ye, de, ce = IG._noaa_tsunami_data(-5.0, -4.0, -80.0, -79.0)
	@test isempty(xe) && isempty(ye) && isempty(ce) && size(de, 1) == 0
end

@testitem "noaa tsunami: the hover block and the data table" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	xs, ys, data, country = IG._noaa_tsunami_data(-180.0, 180.0, -90.0, 90.0)
	# The largest earthquake in the catalog: 1960 Chile, the reference record for this tool.
	k = argmax([isnan(data[i, IG._NOAA_TSU_MAG]) ? -Inf : data[i, IG._NOAA_TSU_MAG] for i in eachindex(xs)])
	info = IG._noaa_tsu_info(view(data, k, :), country[k])
	@test occursin("1960-05-22", info)                  # the date, built from year/month/day
	@test occursin("CHILE", info)
	@test occursin("Cause: Earthquake", info)           # the code SPELLED OUT, not the number
	@test !occursin("Cause: 1", info)
	@test occursin("Event validity: Definite Tsunami", info)
	@test occursin("Eq magnitude: 9.5", info)
	@test occursin("Deaths: 227899", info) || occursin("Deaths: 2226", info)
	# A column the event does not carry is left OUT of the block, not shown blank.
	@test !occursin(": \n", info) && !endswith(info, ": ")
	# The oldest event carries almost nothing — it must still produce a usable block, not a wall
	# of empty labels.
	o = argmin([isnan(data[i, IG._NOAA_TSU_YEAR]) ? Inf : data[i, IG._NOAA_TSU_YEAR] for i in eachindex(xs)])
	oinfo = IG._noaa_tsu_info(view(data, o, :), country[o])
	@test !isempty(oinfo)
	@test count(==('\n'), oinfo) < count(==('\n'), info)
	# The table: one cell per declared column, on every row, and the codes spelled out there too.
	row = IG._noaa_tsu_row(xs[k], ys[k], view(data, k, :), country[k])
	@test length(row) == length(IG._NOAA_TSU_TABLE_COLS)
	@test row[findfirst(==("Cause"), IG._NOAA_TSU_TABLE_COLS)] == "Earthquake"
	@test row[findfirst(==("Date"), IG._NOAA_TSU_TABLE_COLS)] == "1960-05-22"
	for i in (1, 17, 512, length(xs))
		@test length(IG._noaa_tsu_row(xs[i], ys[i], view(data, i, :), country[i])) ==
		      length(IG._NOAA_TSU_TABLE_COLS)
	end
	# How big the circle is drawn is a property of the picture, not of the event: it is NOT data.
	@test !any(c -> occursin("size", lowercase(c)), IG._NOAA_TSU_TABLE_COLS)
end

# SACRED_LAW: same quantity, same function. The circle size comes from the app's ONE
# magnitude->diameter scale (the USGS one Seismicity draws with), never a second copy of it here.
@testitem "noaa tsunami: circle size is the shared USGS magnitude scale" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	xs, ys, data, country = IG._noaa_tsunami_data(-180.0, 180.0, -90.0, 90.0)
	sizes = [IG._seis_mag_size(Float64(data[k, IG._NOAA_TSU_MAG])) for k in eachindex(xs)]
	@test length(sizes) == length(xs)
	@test all(>(0), sizes)
	@test all(isfinite, sizes)
	# a bigger magnitude is never a smaller circle, and the two ends are the scale's own clamps
	@test IG._seis_mag_size(7.0) > IG._seis_mag_size(5.0) > IG._seis_mag_size(3.0)
	@test IG._seis_mag_size(NaN) == IG._seis_mag_size(IG._SEIS_MAG_LO)   # no magnitude -> smallest
	@test maximum(sizes) <= IG._seis_mag_size(IG._SEIS_MAG_HI)           # …and M9.5 cannot blot the map
end

# The catalog is reached through the SAME door the Geography point datasets use, and that door's
# shared reader was split out of `_geo_points` to serve it. Both halves must keep working.
@testitem "noaa tsunami: the shared _geo_select still serves the Geography datasets" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._geo_layer_name("noaa_tsunami") == "NOAA Historical Tsunamis"
	for (file, latlon) in (("volcanoes.dat", true), ("meteoritos.dat", false),
	                       ("hydrothermal_vents.dat", false), ("mareg_online.dat", false),
	                       ("wcity_major.dat", false))
		x, y, t = IG._geo_points(file, -180.0, 180.0, -90.0, 90.0; latlon=latlon)
		@test length(x) > 0
		@test length(x) == length(y) == length(t)
	end
	# _geo_select hands back the WHOLE dataset (what a multi-column catalog needs) plus the same
	# normalized lon _geo_points returns, and says "nothing" the same way for a missing file.
	Sel, xs = IG._geo_select("noaa_historical_tsunami_events.dat", -180.0, 180.0, -90.0, 90.0)
	@test Sel !== nothing
	@test size(Sel.data, 1) == length(xs)
	@test size(Sel.data, 2) == length(IG._NOAA_TSU_COLS) + 2
	bad, badxs = IG._geo_select("no_such_file_here.dat", -180.0, 180.0, -90.0, 90.0)
	@test bad === nothing && isempty(badxs)
	# ...and it SAID so. The missing file is an error: this claims it (so the end-of-run "no
	# unclaimed errors" check stays meaningful) and asserts it names the file it could not find.
	@test any(m -> occursin("no_such_file_here.dat", m), IG._take_tool_errors!())
end
