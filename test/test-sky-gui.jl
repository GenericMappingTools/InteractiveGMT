# T & L > "Solar system" (deps/src/74_solarsys.cpp) and "Sky viewer" (deps/src/74_skyview.cpp), over the
# one ephemeris both draw from (deps/src/74_ephem.cpp). Reference values: Meeus, "Astronomical
# Algorithms" 2nd ed., worked examples 12.a, 25.a, 33.a and 47.a.

@testitem "ephemeris: sidereal time, Sun, Moon, Venus against Meeus' worked examples" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	eph(what, jd) = (out = zeros(3);
		@test ccall(IG._fn(:gmtvtk_ephem_test), Cint, (Cstring, Cdouble, Ptr{Cdouble}), what, jd, out) == 1; out)
	@test isapprox(eph("gmst", 2446895.5)[1], 197.693195; atol=1e-5)    # 12.a: 1987 Apr 10, 0h UT
	@test isapprox(eph("deltat", 2000.0)[1], 63.86; atol=0.01)
	m = eph("moonecl", 2448724.5)                                        # 47.a: 1992 Apr 12, 0h TD
	@test isapprox(m[1], 133.162655; atol=0.01)
	@test isapprox(m[2], -3.229126; atol=0.01)
	@test isapprox(m[3], 368409.7; atol=50)
	s = eph("sun", 2448908.5)                                            # 25.a: 1992 Oct 13, 0h TD
	@test isapprox(s[1], 198.38083; atol=0.02) && isapprox(s[2], -7.78507; atol=0.02)
	@test isapprox(s[3], 0.99766; atol=0.0005)
	v = eph("venus", 2448976.5)                                          # 33.a: 1992 Dec 20, 0h TD
	@test isapprox(v[1], 316.17291; atol=0.05) && isapprox(v[2], -18.88801; atol=0.05)
	@test isapprox(v[3], 0.910947; atol=0.002)
	c = eph("stars", 0.0)
	@test c[1] > 9000 && c[2] > 300 && c[3] == 88                       # BSC5, names, 88 constellations
end

# Pluto, Ceres and Eris: mean elements fitted to JPL Horizons over 1800-2200 (74_ephem.cpp). Reference:
# Horizons heliocentric vectors (J2000 ecliptic, AU) at 2000-01-01 12:00, 2026-01-01 and 2100-01-01 TDB.
# Tolerances = the fit's own worst error over the span (0.05 / 0.6 / 0.18 deg) with a little margin.
@testitem "ephemeris: Pluto, Ceres and Eris against JPL Horizons" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	using LinearAlgebra
	helio(b, jd) = (out = zeros(3);
		@test ccall(IG._fn(:gmtvtk_ephem_test), Cint, (Cstring, Cdouble, Ptr{Cdouble}), "helio:" * b, jd, out) == 1; out)
	sep(a, b) = acosd(clamp(dot(a, b) / (norm(a) * norm(b)), -1, 1))
	ref = Dict(
		"pluto" => [(2451545.0, [-9.875347258580963, -27.95878609745811, 5.850454132132248]),
		            (2461041.5, [19.22791145673266, -29.65238047604593, -2.387513202954491]),
		            (2488069.5, [39.67530922819456, 24.90476330864163, -14.14321151643269])],
		"ceres" => [(2451545.0, [-2.379327705915647, 0.7954860388931395, 0.4630055715902157]),
		            (2461041.5, [2.547982816654031, 1.287973113680162, -0.4286307219181571]),
		            (2488069.5, [1.716868948812041, 2.199900564300181, -0.2369326860286655])],
		"eris"  => [(2451545.0, [88.39334192774233, 30.76524538911426, -26.0943902751538]),
		            (2461041.5, [85.21409498695202, 39.46350543472784, -17.4997018198493]),
		            (2488069.5, [61.65916153652115, 56.2427415628848, 8.968264551268833])])
	tol = Dict("pluto" => (0.07, 0.1), "ceres" => (0.7, 0.03), "eris" => (0.25, 0.3))   # deg, AU
	for (b, rows) in ref, (jd, x) in rows
		v = helio(b, jd)
		@test sep(v, x) < tol[b][1]
		@test abs(norm(v) - norm(x)) < tol[b][2]
	end
end

# Apophis: Horizons osculating-element table (74_apophis.cpp), built to 1e-4 AU over 1900-2100. Reference:
# Horizons heliocentric VECTORS (J2000 ecliptic, AU, TDB), including the 2029-04-13 21:46 closest approach.
@testitem "ephemeris: Apophis against JPL Horizons, through the 2029 Earth pass" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	using LinearAlgebra
	helio(jd) = (out = zeros(3);
		ok = ccall(IG._fn(:gmtvtk_ephem_test), Cint, (Cstring, Cdouble, Ptr{Cdouble}), "helio:apophis", jd, out); (ok, out))
	ref = [(2451545.0, [-1.037925696095382, -0.1457977684770640, -0.01749114057492798]),
	       (2461041.5, [0.6337626700399294, -0.3921765622518124, 0.03591964281079789]),
	       (2462240.406944444, [-0.9175077190175104, -0.4050855957678985, 6.983171695791036e-5]),
	       (2462250.5, [-0.8039343531432555, -0.5490172448864571, 0.007044317892253287]),
	       (2473000.5, [0.9540766176798963, 0.6977701826482935, -0.01038787015725944]),
	       (2488069.5, [-1.021376131679670, -0.2029056978569125, -0.007966915741052339])]
	for (jd, x) in ref
		ok, v = helio(jd)
		@test ok == 1 && norm(v - x) < 1.2e-4
	end
	@test helio(2415000.0)[1] == 0 && helio(2488100.0)[1] == 0    # no position outside 1900-2100
end

# Halley: same kind of table (74_halley.cpp, Horizons record 90000030 = JPL#75), built to 1e-4 AU over
# 1900-2100. Reference: Horizons heliocentric VECTORS (J2000 ecliptic, AU, TDB), the three perihelia among them.
@testitem "ephemeris: Halley against JPL Horizons, through its perihelia" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	using LinearAlgebra
	helio(jd) = (out = zeros(3);
		ok = ccall(IG._fn(:gmtvtk_ephem_test), Cint, (Cstring, Cdouble, Ptr{Cdouble}), "helio:halley", jd, out); (ok, out))
	ref = [(2415100.5, [-8.338478172092175, 19.02542503672929, -5.459886838870164]),
	       (2418781.5, [0.3341335455808441, -0.4530215647803934, 0.1672046425366221]),
	       (2446470.5, [0.3423337950487022, -0.4465925417210364, 0.1677969814871451]),
	       (2451545.0, [-17.38599346385816, 16.97917611082230, -7.577986613535686]),
	       (2461351.5, [-19.33546098851591, 27.47269271480358, -9.849070571158840]),
	       (2474034.5, [0.3294658268970470, -0.4629216774978387, 0.1690680985684523]),
	       (2488000.5, [-19.09155096417895, 26.66263866973291, -9.666249373791478])]
	for (jd, x) in ref
		ok, v = helio(jd)
		@test ok == 1 && norm(v - x) < 1.2e-4
	end
	@test helio(2415000.0)[1] == 0 && helio(2488100.0)[1] == 0    # no position outside 1900-2100
end

@testitem "solar system: menu opens the demo; time and the distance scale drive it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	state(fig, action = "") = (out = zeros(16);
		ok = ccall(IG._fn(:gmtvtk_solarsys_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), fig.h, action, out);
		ok == 1 ? out : nothing)
	G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
	fig = IG.viz(G; title = "grid")
	pump()
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), fig.h, "Solar system") == 1
		pump()
		s = state(fig)
		@test s !== nothing && s[2] == 1                     # on screen
		s = state(fig, "jd:2451545.0")                       # J2000.0: the Earth at heliocentric longitude ~100.5
		@test isapprox(s[4], -0.178; atol=0.01) && isapprox(s[5], 0.967; atol=0.01)
		@test 1.38 < s[6] < 1.67                             # Mars between perihelion and aphelion
		@test s[9] == 1 && isapprox(s[7], 10 * sqrt(hypot(s[4], s[5])); atol=0.01)   # square-root distances
		s = state(fig, "scale:0")
		@test isapprox(s[7], 10 * hypot(s[4], s[5]); atol=0.01)                     # true distances
		s = state(fig, "scale:3")                            # Titius-Bode: Neptune moved out to 38.8 AU's slot
		@test isapprox(s[15], 10 * sqrt(s[16] * 38.8 / 30.06952752); atol=0.01)
		@test isapprox(s[7], 10 * sqrt(hypot(s[4], s[5]) / 1.00000018); atol=0.01)  # ...the Earth stays at 1
		@test s[11] == 14 && s[12] == 31                     # every name (13 bodies + the Sun); dwarfs, Apophis, Halley drawn
		s = state(fig, "halley:0")
		@test s[12] == 15 && s[11] == 13                     # Halley and its name hidden by its box
		s = state(fig, "halley:1")
		@test s[12] == 31
		s = state(fig, "names:0")
		@test s[11] == 0                                     # names off...
		s = state(fig, "hover:4")
		@test s[14] == 4 && s[11] == 1                       # ...but Jupiter's shows under the cursor
		s = state(fig, "hover:-1")
		@test s[14] == -1 && s[11] == 0
		s = state(fig, "dwarfs:0")
		@test s[12] == 28                                    # Ceres and Eris hidden, Pluto, Apophis and Halley stay
		s = state(fig, "apophis:0")
		@test s[12] == 20                                    # Apophis hidden by its box
		state(fig, "apophis:1")
		s = state(fig, "jd:2415000.0")                       # 1899: outside Apophis' and Halley's tables
		@test s[12] == 4                                     # ...no position, not drawn
		s = state(fig, "jd:2451545.0")
		@test s[12] == 28                                    # back inside: drawn again
		s = state(fig, "panel:0")
		@test s[13] == 0                                     # the right panel folds away...
		s = state(fig, "panel:1")
		@test s[13] == 1                                     # ...and comes back
		s = state(fig, "play")
		@test isapprox(s[3], 2451546.0; atol=1e-6)           # one Play frame = one day
		state(fig, "close")
		pump()
		s = state(fig)
		@test s[2] == 0 && s[8] == 1                         # closed = parked in the window's Scene Objects
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		pump()
	end
end

@testitem "sky viewer: menu opens it; the Sun culminates where it must; daylight hides the stars; a click names the Sun" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	pick = Vector{UInt8}(undef, 2048)
	state(fig, action = "") = (out = zeros(13);
		ok = ccall(IG._fn(:gmtvtk_sky_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Ptr{UInt8}, Cint),
		           fig.h, action, out, pick, length(pick));
		ok == 1 ? out : nothing)
	picked() = unsafe_string(pointer(pick))
	G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
	fig = IG.viz(G; title = "grid")
	pump()
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), fig.h, "Sky viewer") == 1
		pump()
		s = state(fig)
		@test s !== nothing && s[2] == 1                     # on screen
		s = state(fig, "place:Lisbon")
		@test isapprox(s[12], 38.72; atol=0.01)
		state(fig, "mag:6.5")
		# 2026-06-21 12:38 UTC, Lisbon: local apparent noon at the June solstice. The Sun stands due south
		# at 90 - 38.72 + 23.44 = 74.7 deg.
		s = state(fig, "jd:2461213.0264")
		@test isapprox(s[6], 180.0; atol=1.0) && isapprox(s[5], 74.7; atol=0.3)
		state(fig, "day:1")
		s = state(fig)
		@test s[4] == 0                                      # broad daylight: no star drawn
		s = state(fig, "day:0")
		@test s[4] > 8000                                    # ...all of them without the daylight
		s = state(fig, "look:0")
		@test isapprox(s[10], s[5]; atol=0.6) && isapprox(s[11], s[6]; atol=0.6)
		state(fig, "click:3,3")                              # a corner: not the Sun
		@test !occursin("<b>Sun</b>", picked())
		state(fig, "click:center")
		@test occursin("<b>Sun</b>", picked())               # a click on the Sun names it
		s = state(fig, "jd:2461213.5")                       # 00:00 UTC: astronomical night in Lisbon
		@test s[5] < -18
		state(fig, "day:1")
		s = state(fig)
		@test s[4] > 8000                                    # the night shows them all again
		@test 0 <= s[13] <= 1
		state(fig, "close")
		pump()
		s = state(fig)
		@test s[2] == 0 && s[9] == 1                         # closed = parked in the window's Scene Objects
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		pump()
	end
end
