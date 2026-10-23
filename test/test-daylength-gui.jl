# T & L > "Earth orbit & day length" (deps/src/74_daylength.cpp): Earth round the Sun, drawn natively.

@testitem "earth orbit: menu opens the orbit demo; day, latitude and tilt drive it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	state(fig, action = "") = (out = zeros(8);
		ok = ccall(IG._fn(:gmtvtk_daylength_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), fig.h, action, out);
		ok == 1 ? out : nothing)
	G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
	fig = IG.view_grid(G; title = "grid")
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
