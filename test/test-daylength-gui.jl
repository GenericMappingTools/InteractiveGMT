# T & L > "Earth orbit & day length" (deps/src/74_daylength.cpp): Earth round the Sun, drawn natively.

@testitem "earth orbit: menu opens the orbit demo; day, latitude and tilt drive it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	state(fig, action = "") = (out = zeros(8);
		ok = ccall(IG._fn(:gmtvtk_daylength_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), fig.h, action, out);
		ok == 1 ? out : nothing)
	G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
	fig = IG.viz(G; title = "grid")
	pump()
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), fig.h,
		            "Earth orbit") == 1       # T & L > Earth orbit & day length…
		pump()
		s = state(fig)
		@test s !== nothing && s[2] == 1      # on screen
		@test s[3] == 172                     # opens on the June solstice...
		@test isapprox(s[4], 23.44; atol=0.1) # ...where the Sun stands over the Tropic of Cancer
		@test s[5] == 40.0 && isapprox(s[6], 14.84; atol=0.05)   # cos H = -tan 40 tan 23.44
		s = state(fig, "day:80")              # March equinox: 12 h everywhere
		@test abs(s[4]) < 0.6 && abs(s[6] - 12.0) < 0.1
		@test s[7] < 0                        # Sun at ecliptic longitude 0: the Earth opposite, at 180
		s = state(fig, "lat:80")
		s = state(fig, "day:172")
		@test s[6] == 24.0                    # midnight sun
		s = state(fig, "tilt:0")
		@test s[4] == 0.0 && s[6] == 12.0     # no tilt, no seasons
		s = state(fig, "play")
		@test s[3] == 173
		state(fig, "close")
		pump()
		s = state(fig)
		@test s[2] == 0 && s[8] == 1          # closed = parked in the window's Scene Objects
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		pump()
	end
end

# T & L > "Sun-synchronous orbit" (deps/src/74_sunsync.cpp): J2 nodal precession against the Sun's year.
@testitem "sun-synchronous: menu opens the demo; the node keeps its local time only at the right inclination" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	state(fig, action = "") = (out = zeros(10);
		ok = ccall(IG._fn(:gmtvtk_sunsync_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), fig.h, action, out);
		ok == 1 ? out : nothing)
	G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
	fig = IG.viz(G; title = "grid")
	pump()
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), fig.h,
		            "Sun-synchronous orbit") == 1
		pump()
		s = state(fig)
		@test s !== nothing && s[2] == 1             # on screen
		@test s[3] == 1                              # opens on 1 January...
		@test isapprox(s[4], 98.19; atol=0.02)       # ...700 km, sun-synchronous: i = 98.19 deg
		@test isapprox(s[5], 360 / 365.2422; atol=1e-3)   # the node turns with the Sun
		@test isapprox(s[6], 22.5; atol=0.02)        # LTAN as set
		s = state(fig, "day:180")
		@test isapprox(s[6], 22.5; atol=0.03)        # ...and still the same half a year later
		s = state(fig, "incl:90")                    # polar: no drift, the Sun walks round the plane
		@test abs(s[5]) < 1e-12                      # cos(90 deg) in floating point
		s = state(fig, "day:92")
		@test isapprox(s[6], 22.5 - 91 * 0.98565 / 15; atol=0.03)   # six hours earlier after a quarter year
		s = state(fig, "sso")
		@test isapprox(s[4], 98.19; atol=0.02)
		s = state(fig, "ltan:12"); s = state(fig, "day:80")
		@test s[7] > 0.3                             # noon-midnight: a third of each revolution in shadow
		s = state(fig, "ltan:18")
		@test s[7] == 0.0 && abs(s[8]) > 64          # dawn-dusk at the equinox: always in sunlight
		@test s[10] == 2                             # Earth on screen, plane disc not
		s = state(fig, "plane:1")
		@test s[10] == 5                             # "Orbit plane only": the disc and both arrows, no Earth
		s = state(fig, "plane:0")
		@test s[10] == 2
		s = state(fig, "ortho:1")
		@test s[10] == 10                            # both cameras orthographic
		s = state(fig, "ortho:0")
		@test s[10] == 2                             # ...and back to perspective
		s = state(fig, "play")
		@test s[3] == 82                             # one Play frame = 2 days
		state(fig, "close")
		pump()
		s = state(fig)
		@test s[2] == 0 && s[9] == 1                 # closed = parked in the window's Scene Objects
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		pump()
	end
end

# T & L > "Day length & axial tilt (GMT figures)" (deps/src/74_daylength_gmt.cpp, src/daylength.jl).
@testitem "daylength: the sunrise equation" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG.day_length(0.0, 23.44) ≈ 12.0 atol=0.5          # the equator stays near 12 h
	@test IG.day_length(0.0, 0.0) == 12.0
	@test IG.day_length(45.0, 0.0) ≈ 12.0                    # equinox: 12 h everywhere
	@test IG.day_length(80.0, 23.44) == 24.0                 # midnight sun
	@test IG.day_length(-80.0, 23.44) == 0.0                 # polar night
	@test IG.day_length(40.0, 10.0) ≈ 24 - IG.day_length(-40.0, 10.0)
end
