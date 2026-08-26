# CI-safe unit tests for the Geography > "Sun and terminators" dialog (GMT's solar/pssolar).
# Everything here is checked WITHOUT a window and WITHOUT running GMT: the option string the dialog's
# settings turn into, the seam-splitting of a terminator circle, the night-side polygon built from
# it, the report formatting, and the refusals — every one of which fires before the module is ever
# called. A refusal never reaches the C side (it is logged through _viewer_log_error, which swallows
# a dead/absent handle), so no DLL is needed. Actually drawing a terminator needs a live window and
# live GMT: that is a :gui matter.

@testitem "solar: callback is wired" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	for s in (:_on_solar, :_register_solar, :_solar_mods, :_solar_pos, :_solar_split_seam,
	          :_solar_wraps, :_solar_night_polys, :_solar_paint!, :_solar_rgb, :_solar_report_text)
		@test isdefined(IG, s)
	end
	# Every registration must have its export in the DLL symbol list, or the feature silently stays
	# "not wired" at runtime (new-c-export-needs-lib-symbols).
	for sym in (:gmtvtk_set_solar_callback, :gmtvtk_solar_report, :gmtvtk_remove_symbols_h,
	            :gmtvtk_remove_polys_h, :gmtvtk_add_poly_full)
		@test sym in IG._LIB_SYMBOLS
	end
end

@testitem "solar: the +d / +z tail and the observer position" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	@test IG._solar_mods(Dict{String,String}()) == ""                       # "now" = say nothing
	@test IG._solar_mods(Dict("date" => "2000-04-25T12:15:00")) == "+d2000-04-25T12:15:00"
	@test IG._solar_mods(Dict("date" => "2000-04-25T12:15:00", "tz" => "-03:00")) ==
	      "+d2000-04-25T12:15:00+z-03:00"
	@test IG._solar_mods(Dict("tz" => "02")) == "+z02"
	@test_throws ErrorException IG._solar_mods(Dict("tz" => "Lisbon"))      # not an offset from UTC

	@test IG._solar_pos(Dict{String,String}()) == ""                        # no position is legal
	@test IG._solar_pos(Dict("lon" => "-9.14", "lat" => "38.7")) == "-9.14/38.7"
	@test_throws ErrorException IG._solar_pos(Dict("lon" => "-9.14"))       # half a position is a typo
	@test_throws ErrorException IG._solar_pos(Dict("lon" => "west", "lat" => "38.7"))
end

@testitem "solar: the paint colour from the dialog's button" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	d = (0.5, 0.5, 0.5)
	@test all(IG._solar_rgb("255/0/128", d) .≈ (1.0, 0.0, 128 / 255))
	@test IG._solar_rgb("", d) == d                                        # no key -> the default
	@test IG._solar_rgb("26/33", d) == d                                   # malformed -> the default
	@test IG._solar_rgb("nope/0/0", d) == d
	@test all(0 .<= IG._solar_rgb("999/-5/0", d) .<= 1)                     # clamped, never out of gamut
end

@testitem "solar: a terminator is cut at the map seam" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	# A small circle centred on the dateline: as one polyline it would streak straight across the map.
	lons = [170.0, 175.0, 179.0, -179.0, -175.0, -170.0]
	circle = [lons  fill(10.0, length(lons))]
	pieces = IG._solar_split_seam([circle], -180.0)
	@test length(pieces) == 2                                    # cut where it jumps the seam
	# Every original point is still there, plus the crossing point itself, inserted on BOTH sides so
	# each piece really reaches the edge of the map.
	@test sum(size(p, 1) for p in pieces) == length(lons) + 2
	@test all(all(-180.0 .<= p[:, 1] .<= 180.0) for p in pieces)
	@test any(p -> any(≈(180.0), p[:, 1]), pieces)                # one piece ends ON the eastern edge
	@test any(p -> any(≈(-180.0), p[:, 1]), pieces)               # the other starts on the western one

	# The SAME circle on a 0..360 map is continuous, so it must come back in one piece — normalized
	# into that frame, not left with negative longitudes hanging outside the map.
	one = IG._solar_split_seam([circle], 0.0)
	@test length(one) == 1
	@test all(0.0 .<= one[1][:, 1] .<= 360.0)
	# A 1-point "segment" draws nothing and is dropped rather than handed on as a degenerate line.
	@test isempty(IG._solar_split_seam([[10.0 20.0]], -180.0))
end

@testitem "solar: the night side is a closed polygon" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	# A ring that goes all the way round the world (what the day/night terminator does whenever the
	# two poles are not in the same state): a wave in latitude, one full turn in longitude.
	lon = collect(-180.0:10.0:180.0)
	ring = [lon  20.0 .* sind.(lon)]
	@test IG._solar_wraps(ring)
	# Sun in the SOUTH -> the north pole is the dark one, so the region closes along +90.
	polys = IG._solar_night_polys(ring, -180.0, -15.0)
	@test length(polys) == 1
	P = polys[1]
	@test P[1, :] == P[end, :]                                    # closed
	@test maximum(P[:, 2]) == 90.0                                # …around the north pole
	@test minimum(P[:, 2]) > -90.0                                # and NOT around the south one
	@test extrema(P[:, 1]) == (-180.0, 180.0)                     # spanning the whole frame
	# Sun in the NORTH -> the other pole, same polygon otherwise.
	Q = IG._solar_night_polys(ring, -180.0, 15.0)[1]
	@test minimum(Q[:, 2]) == -90.0
	@test maximum(Q[:, 2]) < 90.0

	# A ring that does NOT wrap (a deep-twilight cap that never reaches a pole) already bounds its own
	# region: it is closed as it stands, with no pole edge invented for it.
	th = collect(0.0:15.0:360.0)
	cap = [(20.0 .+ 30.0 .* cosd.(th))  (10.0 .+ 30.0 .* sind.(th))]
	@test !IG._solar_wraps(cap)
	C = IG._solar_night_polys(cap, -180.0, -15.0)
	@test length(C) == 1
	@test C[1][1, :] == C[1][end, :]
	@test maximum(abs.(C[1][:, 2])) < 90.0                        # no pole closure anywhere in it
end

@testitem "solar: the sun report" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	# The ten numbers -C reports: sun lon, sun lat, azimuth, elevation, sunrise, sunset, noon,
	# day length (min), elevation corrected, equation of time (min).
	v = [12.5, 13.25, 234.5, 45.0, 0.25, 0.875, 0.5625, 900.0, 45.1, -3.5]
	full = IG._solar_report_text(v, "-9.14/38.7", "2000-04-25T12:15:00", "")
	@test occursin("Longitude = 12.5", full)
	@test occursin("Azimuth   = 234.5", full)
	@test occursin("refraction-corrected 45.1", full)
	@test occursin("Sunrise = 06:00", full)      # 0.25 of a day
	@test occursin("Sunset = 21:00", full)       # 0.875
	@test occursin("Noon = 13:30", full)         # 0.5625
	@test occursin("Day length = 15:00", full)   # 900 minutes
	@test occursin("2000-04-25T12:15:00", full)

	# Without a position the module still fills those fields in — for the point (0,0) — so the report
	# must NOT show them as if they belonged to the user's place.
	bare = IG._solar_report_text(v, "", "", "")
	@test !occursin("Sunrise", bare)
	@test occursin("now", bare)

	# Polar day/night: there is no sunrise at all, and that is said in words rather than as "--:--".
	polar = copy(v);  polar[5] = NaN;  polar[6] = NaN
	@test occursin("neither rises nor sets", IG._solar_report_text(polar, "0/85", "", ""))
	@test IG._solar_hhmm(NaN) == "--:--"
	@test IG._solar_hhmm_min(NaN) == "--:--"
end

@testitem "solar: refuses what the module cannot do" tags=[:unit, :fast, :solar] begin
	IG = InteractiveGMT
	scene = Ptr{Cvoid}(UInt(0x50142201))
	call(kv) = IG._on_solar(scene, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))
	# Nothing ticked at all — no terminator, no report, no marker.
	@test call(["terms=", "sun=0", "marksun=0"]) == 0
	# A time zone that is not an offset from UTC.
	@test call(["terms=d", "tz=Lisbon", "sun=0"]) == 0
	# Half an observer position (checked before the module is called, since no terminator was asked
	# for — so this refusal costs no GMT run).
	@test call(["terms=", "sun=1", "lon=-9.14"]) == 0
end
