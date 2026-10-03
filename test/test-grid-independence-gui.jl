# :gui -- THE TWO LAWS OF A WINDOW WITH SEVERAL GRIDS, checked on screen, not in the scene graph.
#
#   1. Separate grids do not influence each other: a second grid (a derived one, as mbeditviz's map is)
#      arriving, or changing ITS vertical exaggeration, leaves the first grid's VE alone, and the other
#      way round.
#   2. A line laid on the second grid belongs to that grid: it rides THAT grid's VE and nobody else's,
#      and it is VISIBLE -- counted in the rendered pixels, because "its Scene Objects row is checked"
#      is exactly what stayed true while nothing was on screen (2026-10-03: an MB-System track at
#      z = 0 over mbeditviz's bathymetry, the base grid unchecked, cut away by a depth range fitted
#      to the bathymetry alone).
#
# The scenario is the MB-System one, through the SAME doors: the base grid, then the second grid as
# mbeditviz's map opens it (gmtvtk_promote_surface_h + gmtvtk_show_new_element_h, which unchecks the
# base), then the track as mbHostAddTrack lays it (gmtvtk_add_overlay_ex4_h, z = 0 a placeholder, NOT
# clamped). No MBIO needed: it runs everywhere the GUI tier runs.

@testitem "two grids: the second one and its track never touch the first; the track is on screen" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n=25) = for _ in 1:n; IG._pump_once(); sleep(0.02); end
	px(h) = ccall(_test_fn(:gmtvtk_pixel_count_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble),
	              h, 1.0, 0.0, 1.0, 0.08)                     # the track is pure magenta, nothing else is
	probe(h) = (b = zeros(UInt8, 2048);
	            ccall(_test_fn(:gmtvtk_overlay_probe_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, "track", b, 2048);
	            Dict(split(kv, '=')[1] => parse(Float64, split(kv, '=')[2]) for kv in split(unsafe_string(pointer(b)), ';')))
	state(h) = IG._parse_scene_state(IG._scene_state_full_raw(h))
	vekey(st, k) = Float64(st[k])
	setve(h, v) = ccall(IG._fn(:gmtvtk_set_view_azel_h), Cint,
	                    (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble, Cdouble, Cdouble),
	                    h, 135.0, 35.0, -1.0, v, Cint(0), 0.0, 0.0, 0.0)   # the ACTIVE layer's VE (the gizmo's door)
	setvis(h, name, on) = ccall(IG._fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), h, name, Cint(on))
	viewmode(h, m) = ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(m))

	# a deep sloping sea floor, and the second grid over a part of it (mbeditviz grids the swath's footprint)
	x = collect(range(-12.0, -11.0, length=201)); y = collect(range(36.5, 37.5, length=201))
	G = IG.GMT.mat2grid(Float32[-5300 + 3800 * (xi + 12.0) * (yi - 36.5) for yi in y, xi in x], x=x, y=y)
	xs = collect(range(-11.70, -11.48, length=120)); ys = collect(range(36.71, 36.88, length=110))
	Zs = Float32[-5300 + 3800 * (xi + 12.0) * (yi - 36.5) + 40 * sin(30xi) for yi in ys, xi in xs]

	function second!(h)
		ok = ccall(IG._fn(:gmtvtk_promote_surface_h), Cint,
		           (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}, Ptr{Cdouble},
		            Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring, Cint),
		           h, Float32.(permutedims(Zs)[:]), Cint(length(xs)), Cint(length(ys)), xs[1], xs[end], ys[1], ys[end],
		           Cint(1), C_NULL, C_NULL, Cint(0), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), "second", Cint(1))
		ccall(IG._fn(:gmtvtk_show_new_element_h), Cvoid,
		      (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cint),
		      h, "second", xs[1], xs[end], ys[1], ys[end], Float64(minimum(Zs)), Float64(maximum(Zs)), Cint(1), Cint(0))
		pump()
		ok == 1
	end
	function track!(h)
		n = 200
		xyz = vec(permutedims(hcat(collect(range(-11.68, -11.50, length=n)), collect(range(36.79, 36.77, length=n)), zeros(n))))
		r = ccall(IG._fn(:gmtvtk_add_overlay_ex4_h), Cint,
		          (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble,
		           Cstring, Cstring, Cstring, Cint, Cint, Cint, Cstring),
		          h, xyz, Cint(n), Cint[0, n], Cint(1), Cint(1), 1.0, 0.0, 1.0, 2.0, 0.0, "track", "Tracks", "",
		          Cint(1), Cint(1), Cint(0), "")
		pump()
		r == 1
	end

	# LAW 2, on screen: the track over the second grid is drawn, in the map view and in 3-D, whatever
	# either grid's VE was before or becomes after
	for mode in (1, 0), (veBase, veSecond, veAfter) in ((1.0, 1.0, 1.0), (5.0, 1.0, 1.0), (1.0, 5.0, 1.0),
	                                                     (1.0, 1.0, 5.0), (5.0, 1.0, 0.3))
		f = IG.view_grid(G; geographic=true, title="base"); pump()
		h = f.h
		try
			mode == 0 && @test viewmode(h, 0) == 1
			pump()
			veBase != 1.0 && setve(h, veBase)
			pump()
			@test second!(h)
			veSecond != 1.0 && setve(h, veSecond)
			pump()
			@test track!(h)
			veAfter != 1.0 && setve(h, veAfter)
			pump()
			p = probe(h)
			@test p["vis"] == 1
			# no z of its own: it lies on top of ITS grid (the second one's highest z), at ITS grid's scale
			@test p["bz0"] ≈ Float64(maximum(Zs)) * p["sz"] rtol=1e-5
			@test p["bz1"] ≈ p["bz0"]
			n = px(h)
			@test n > 300                                     # the track IS on screen (a 2 px line, ~900 px long)
			n > 300 || @info "track not drawn" mode veBase veSecond veAfter n p
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h); pump()
		end
	end

	# LAW 1 and the track's ownership, on the VE numbers
	f = IG.view_grid(G; geographic=true, title="base"); pump()
	h = f.h
	try
		setve(h, 3.0); pump()
		st = state(h)
		@test vekey(st, "ve") ≈ 3.0
		@test second!(h)
		st = state(h)
		tag = only([k for k in keys(st) if startswith(k, "ve_")])
		@test vekey(st, "ve") ≈ 3.0                        # the second grid's arrival leaves the base's VE alone
		@test vekey(st, tag) ≈ 1.0                         # ...and takes none of it for itself
		@test track!(h)
		sz1 = probe(h)["sz"]
		setve(h, 4.0); pump()                              # the SECOND grid's VE (it is the active one)
		st = state(h)
		@test vekey(st, "ve") ≈ 3.0                        # the base did not move
		@test vekey(st, tag) ≈ 4.0
		@test probe(h)["sz"] ≈ 4.0 * sz1 rtol=1e-6         # the track rides ITS grid's VE
		@test px(h) > 300
		# the base back on top, its VE changed: neither the second grid nor its track follow
		@test setvis(h, "second", 0) == 1
		@test setvis(h, "", 1) == 1
		pump()
		setve(h, 2.0); pump()
		st = state(h)
		@test vekey(st, "ve") ≈ 2.0
		@test vekey(st, tag) ≈ 4.0
		@test probe(h)["sz"] ≈ 4.0 * sz1 rtol=1e-6
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h); pump()
	end
end
