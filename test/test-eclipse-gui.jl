# T & L > "Earth-Moon system" (deps/src/74_eclipse.cpp): the eclipse search against NASA's eclipse
# catalogues (Espenak & Meeus, Five Millennium Canons; eclipse.gsfc.nasa.gov), and the demo itself.

@testitem "eclipses: search against NASA (dates, kinds, gamma, magnitudes, century counts)" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	using Dates
	search(a, b; max = 600) = (out = zeros(5 * max);
		n = ccall(IG._fn(:gmtvtk_eclipse_search_test), Cint, (Cdouble, Cdouble, Ptr{Cdouble}, Cint),
		          datetime2julian(a), datetime2julian(b), out, max);
		[out[5i+1:5i+5] for i in 0:min(n, max)-1])
	# NASA: greatest eclipse (UT), solar?, kind, gamma, (lunar) umbral magnitude.
	ref = [(DateTime(2017, 8, 21, 18, 25, 31), 1, 2, 0.4367, NaN),
	       (DateTime(2024, 4, 8, 18, 17, 20), 1, 2, 0.3431, NaN),
	       (DateTime(2023, 10, 14, 17, 59, 32), 1, 1, 0.3753, NaN),
	       (DateTime(1999, 8, 11, 11, 3, 4), 1, 2, 0.5062, NaN),
	       (DateTime(2018, 7, 27, 20, 21, 44), 0, 2, 0.1168, 1.6087),
	       (DateTime(2022, 11, 8, 10, 59, 11), 0, 2, 0.2570, 1.3589),
	       (DateTime(2025, 3, 14, 6, 58, 43), 0, 2, 0.3484, 1.1781)]
	for (t, solar, kind, gamma, mag) in ref
		ev = search(t - Day(2), t + Day(2))
		@test length(ev) == 1
		isempty(ev) && continue
		e = ev[1]
		@test e[2] == solar && e[3] == kind
		@test abs(e[1] - datetime2julian(t)) * 1440 < 2.0        # greatest eclipse within 2 minutes
		@test abs(e[4] - gamma) < 0.012
		isnan(mag) || @test abs(e[5] - mag) < 0.02
	end
	# 2024: penumbral (Mar 25), total solar (Apr 8), partial lunar (Sep 18), annular (Oct 2).
	ev = search(DateTime(2024, 1, 1), DateTime(2025, 1, 1))
	@test [(Int(e[2]), Int(e[3]), Dates.month(julian2datetime(e[1]))) for e in ev] ==
	      [(0, 0, 3), (1, 2, 4), (0, 1, 9), (1, 1, 10)]
	# The 21st century (2001-2100): NASA lists 224 solar (77 partial; the rest central: annular, total
	# and hybrid, which this demo files as one or the other) and 228 lunar (86 penumbral, 57 partial,
	# 85 total). Borderline grazing cases may fall either side of a definition: a few allowed.
	ev = search(DateTime(2001, 1, 1), DateTime(2101, 1, 1))
	sol = filter(e -> e[2] == 1, ev); lun = filter(e -> e[2] == 0, ev)
	cnt(v, k) = count(e -> e[3] == k, v)
	@info "2001-2100: solar $(length(sol)) (partial $(cnt(sol, 0)), annular $(cnt(sol, 1)), total $(cnt(sol, 2))); " *
	      "lunar $(length(lun)) (penumbral $(cnt(lun, 0)), partial $(cnt(lun, 1)), total $(cnt(lun, 2)))"
	@test abs(length(sol) - 224) <= 2 && abs(cnt(sol, 0) - 77) <= 2
	@test abs(length(lun) - 228) <= 2
	@test abs(cnt(lun, 0) - 86) <= 3 && abs(cnt(lun, 1) - 57) <= 3 && abs(cnt(lun, 2) - 85) <= 3
end

@testitem "Earth-Moon system: menu opens it; day/night on the Moon; both eclipses drawn; list, buttons, panel" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	using Dates
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	txt = Vector{UInt8}(undef, 4096)
	state(fig, action = "") = (out = zeros(12);
		ok = ccall(IG._fn(:gmtvtk_eclipse_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Ptr{UInt8}, Cint),
		           fig.h, action, out, txt, length(txt));
		ok == 1 ? out : nothing)
	info() = unsafe_string(pointer(txt))
	G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
	fig = IG.viz(G; title = "grid")
	pump()
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), fig.h, "Earth-Moon system") == 1
		pump()
		s = state(fig)
		@test s !== nothing && s[2] == 1                       # on screen
		@test 900 < s[5] < 1000                                # ~ 228 x 2 + 224 x 2 eclipses in 1900-2100
		@test s[8] == 1                                        # the Moon's night half is darkened, all of it
		s = state(fig, "jd:" * string(datetime2julian(DateTime(2018, 7, 27, 20, 22))))
		@test s[6] > 0.99                                      # total lunar eclipse: the Moon's day half all in the umbra
		@test occursin("Lunar total", info())
		@test s[7] == 0                                        # ...and no shadow on the Earth
		s = state(fig, "jd:" * string(datetime2julian(DateTime(2024, 4, 8, 18, 17))))
		@test s[7] > 0.99 && s[6] == 0                         # total solar: the Sun fully hidden somewhere on the Earth
		@test occursin("Solar total", info())
		s = state(fig, "jd:" * string(datetime2julian(DateTime(2024, 4, 15))))
		@test s[6] == 0 && s[7] == 0 && occursin("no eclipse", info())
		s = state(fig, "next")                                 # the next eclipse: 2024-09-18, partial lunar
		@test Date(julian2datetime(s[3])) == Date(2024, 9, 18) && s[9] == 2   # camera on the Moon
		s = state(fig, "next")                                 # 2024-10-02, annular
		@test Date(julian2datetime(s[3])) == Date(2024, 10, 2) && s[9] == 1   # camera on the Earth from the Sun
		@test 0 < s[7] < 1                                     # annular: a ring of Sun is left
		s = state(fig, "prev")
		@test Date(julian2datetime(s[3])) == Date(2024, 9, 18)
		n = s[4]
		s = state(fig, "pen:1")
		@test s[4] > n                                         # penumbral eclipses join the list
		s = state(fig, "solar:0")
		s2 = state(fig, "lunar:0")
		@test s2[4] == 0 && s[4] > 0
		state(fig, "solar:1"); state(fig, "lunar:1")
		s = state(fig, "row:0")
		@test s[12] == 0 && year(julian2datetime(s[3])) == 1900
		s = state(fig, "panel:0")
		@test s[11] == 0
		s = state(fig, "panel:1")
		@test s[11] == 1
		state(fig, "close")
		pump()
		s = state(fig)
		@test s[2] == 0 && s[10] == 1                          # closed = parked in the window's Scene Objects
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		pump()
	end
end
