# :gui scenarios for Geophysics > MB-System > 3-D mesh (mbmesh) (src/mbmesh_dialog.jl,
# deps/ui/remotes_mbmesh.ui): the menu entry builds the dialog, and a run meshes point data with
# MB-System's mbmesh and shows the mesh in the window, read back through the plugin's
# mbmesh_read_glb_geodetic. Both need MB-System's GMT supplement; without it they are skipped, visibly.

@testitem "mbmesh: the MB-System menu entry builds its dialog" tags=[:gui, :mbmesh] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	if isempty(IG._gmt_plugin_file("mbsystem"))
		@test_skip "MB-System's GMT supplement is not loaded: the menu has no 3-D mesh entry"
	else
		f = viz(IG.GMT.peaks())
		real = IG._RS_TOOLS["mbmesh"]
		asked = String[]
		IG._RS_TOOLS["mbmesh"] = (s, d, io) -> (push!(asked, get(d, "what", "")); true)
		try
			@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "MB-System/3-D mesh (mbmesh)") == 2
			# The MB-System page re-pops the Geophysics menu and openRemoteSDialog waits ~1 s for the popup.
			t0 = time()
			while !("init" in asked) && time() - t0 < 4
				IG._pump_once()
				sleep(0.02)
			end
			@test "init" in asked
			@test ccall(_test_fn(:gmtvtk_window_owned_test), Cint, (Cstring,), "RemoteSMbmesh") == 1
		finally
			IG._RS_TOOLS["mbmesh"] = real
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		end
	end
end

@testitem "mbmesh: a run meshes point data and shows the mesh in the window" tags=[:gui, :mbmesh] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	lib = IG._gmt_plugin_file("mbsystem")
	reader = isempty(lib) ? nothing : (h = IG.Libdl.dlopen(lib); p = IG.Libdl.dlsym(h, :mbmesh_read_glb_geodetic; throw_error = false); IG.Libdl.dlclose(h); p)
	if reader === nothing
		@test_skip "MB-System's GMT supplement with mbmesh_read_glb_geodetic is not loaded"
	else
		h = RemoteSGui.open_empty("mbmesh run")
		dir = mktempdir()
		try
			# a sloping sea floor of 25 x 25 soundings, ~10 m apart, in lon/lat
			pts = joinpath(dir, "slope.xyz")
			open(pts, "w") do io
				for i in 0:24, j in 0:24
					println(io, -124.503 + i * 1.2e-4, ' ', 40.837 + j * 0.9e-4, ' ', -460.0 - 0.4 * i - 0.2 * j)
				end
			end
			ok, r = RemoteSGui.rs(h, ["tool=mbmesh", "what=init", "p_input=$pts", "p_outdir="])
			@test ok == 1 && occursin("set:p_outdir=$(joinpath(dir, "slope_mbmesh"))", r)
			kv = ["tool=mbmesh", "what=mesh", "p_input=$pts", "p_outdir=", "p_srs=", "p_lod=", "p_decimate=",
			      "p_method=mbmesh: Poisson surface (slow)", "p_geoorigin=1", "p_save=0",
			      "p_tileset=0", "p_html=0", "p_cesium=0", "p_track=0"]
			ok, r = RemoteSGui.rs(h, kv)
			@test ok == 1 && occursin(r"\"slope mesh\": \d+ triangles\.", r)
			# the mesh comes back in memory: nothing written, not even the output folder
			@test readdir(dir) == ["slope.xyz"]
			@test ccall(IG._fn(:gmtvtk_has_element_h), Cint, (Ptr{Cvoid}, Cstring), h, "slope mesh") == 1
			# the mesh handed back sits where the soundings are, in lon/lat and height
			P = IG._mbmesh_table_points(pts)
			fv, _ = IG._mbmesh_run_in_memory(["mbmesh", "-I" * pts, "-O" * joinpath(dir, "out"), "--geoorigin"], false, P)
			@test -124.504 < fv.bbox[1] < fv.bbox[2] < -124.499 && 40.836 < fv.bbox[3] < fv.bbox[4] < 40.840
			# the soundings span -474.4 to -460 m; the Poisson surface flares a few metres at the edges
			@test -490 < fv.bbox[5] < fv.bbox[6] < -445
			@test !isdir(joinpath(dir, "out"))
			# Save: the files as well, and the same mesh in them
			fv2, _ = IG._mbmesh_run_in_memory(["mbmesh", "-I" * pts, "-O" * joinpath(dir, "out"), "--geoorigin"], true, P)
			@test isfile(joinpath(dir, "out", "mesh.glb")) && fv2.verts == fv.verts
			g = IG._mbmesh_glb_geodetic(joinpath(dir, "out", "mesh.glb"))
			@test size(g.faces[1], 1) == size(fv.faces[1], 1)
			@test maximum(abs.(extrema(g.verts[:, 3]) .- extrema(fv.verts[:, 3]))) < 0.01
			# a second run adds a second element, never a silently ignored one
			ok, r = RemoteSGui.rs(h, kv)
			@test ok == 1 && occursin("\"slope mesh (2)\":", r)
			# the tool's own refusals
			ok, r = RemoteSGui.rs(h, ["tool=mbmesh", "what=mesh", "p_input=$pts", "p_lod=-1"])
			@test ok == 0 && occursin("Level of detail is a size in meters", r)
			ok, r = RemoteSGui.rs(h, ["tool=mbmesh", "what=mesh", "p_input="])
			@test ok == 0 && occursin("Choose the point data", r)
			IG._take_tool_errors!()
		finally
			RemoteSGui.close_win(h)
			try rm(dir; recursive = true, force = true) catch end
		end
	end
end
