# add_isosurface!: the surface where a 3-D cube crosses a value, added as a MESH layer (it goes
# through gmtvtk_add_mesh_h, the one mesh door) — asserted from the window's own scene state.

@testitem "iso-surface: a cube's level surface becomes a mesh layer on display" tags=[:gui, :isosurface] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	G = ve_grid(zspan = 1000.0)
	# A ball: distance from the box centre, in units of the box half-size, on a 21x21x11 cube.
	xs = collect(range(-15.0, -10.0, length = 21)); ys = collect(range(35.0, 40.0, length = 21))
	zs = collect(range(-5000.0, 0.0, length = 11))
	d  = Float32[sqrt(((x + 12.5) / 2.5)^2 + ((y - 37.5) / 2.5)^2 + ((z + 2500) / 2500)^2)
	             for y in ys, x in xs, z in zs]
	C = GMT.mat2grid(d, x = xs, y = ys, v = zs)
	fig = IG.view_grid(G); ve_pump()
	h = getfield(fig, :h)
	try
		IG.add_isosurface!(fig, C; level = 0.6, color = :red, name = "Ball")
		ve_pump()
		st = ve_state(h)
		k = findfirst(==(("mesh", "Ball")), st["extras"])
		@test k !== nothing
		@test k !== nothing && get(st, "extravis$(k - 1)", 0) == 1
		# a level the cube never reaches is an error, not a silent empty layer
		@test_throws ErrorException IG.add_isosurface!(fig, C; level = 50.0)
		# a 2-D grid is not a cube
		@test_throws ErrorException IG.add_isosurface!(fig, G; level = 0.6)
	finally
		ve_close(h)
	end
end

# The view frame a raster is adopted with (sceneReframeSet's pin) is what every camera placement frames
# against. It used to be a snapshot at the VE of the moment, so after a VE change the camera framed a
# box the geometry no longer occupied: a deep model at a small VE got its parallel camera parked ~20x
# too far out, where the SSAO pass quantised every flat surface into stripes (2026-09-30).
@testitem "view frame follows the VE of the layer it was pinned to" tags=[:gui, :isosurface, :ve] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	G = ve_grid(zspan = 1000.0)
	xs = collect(range(-15.0, -10.0, length = 21)); ys = collect(range(35.0, 40.0, length = 21))
	zs = collect(range(-500000.0, 0.0, length = 11))
	d  = Float32[sqrt(((x + 12.5) / 2.5)^2 + ((y - 37.5) / 2.5)^2 + ((z + 250000) / 250000)^2)
	             for y in ys, x in xs, z in zs]
	C = GMT.mat2grid(d, x = xs, y = ys, v = zs)
	fig = IG.view_grid(G); ve_pump()
	h = getfield(fig, :h)
	try
		IG.add_isosurface!(fig, C; level = 0.6, name = "Deep ball")
		ve_pump()
		s1 = ve_state(h)
		z1 = s1["sb_z0"]
		@test z1 < 0
		# the new layer is the active one: its own VE, through the scene state door
		tag = only(k for k in keys(ve_state_full(h)) if startswith(k, "ve_"))
		ccall(IG._fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, "$tag=0.05;")
		ve_pump()
		s2 = ve_state(h)
		@test isapprox(s2["sb_z0"], 0.05 * z1; rtol = 1e-6)
	finally
		ve_close(h)
	end
end
