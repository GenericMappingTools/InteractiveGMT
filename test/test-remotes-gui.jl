# :gui scenarios for Satellite > Remote sensing (src/RemoteS/remotes_gui.jl). A REAL window through the
# built gmtvtk.dll, and the ONE callback driven with exactly the key=value block RemoteSDialog sends,
# so the whole path runs: parameters -> InteractiveGMT.RemoteS -> delivery into the same window.
# Self-contained: the band cube is synthetic and written to a temporary directory, the TLE is inline.

@testmodule RemoteSGui begin

using InteractiveGMT
const IG = InteractiveGMT

# Drive the callback; returns (ok, answer text).
function rs(h::Ptr{Cvoid}, kv::Vector{String})
	buf = zeros(UInt8, 1 << 18)
	s = join(kv, "\n")
	ok = GC.@preserve buf s IG._on_remotes(h, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, s)),
	                                        pointer(buf), Cint(length(buf)))
	return ok, unsafe_string(pointer(buf))
end

# The AQUA elements of September 2021 the RemoteS gallery uses, as a TLE file (the dialog's "TLE file"
# source), with a TERRA set beside it so the NORAD filter is exercised too.
function tle_kv(dir::String)::Vector{String}
	f = joinpath(dir, "aqua_2021.tle")
	write(f, """
	AQUA
	1 27424U 02022A   21245.83760660  .00000135  00000-0  39999-4 0  9997
	2 27424  98.2123 186.0654 0002229  67.6025 313.3829 14.57107527 28342
	TERRA
	1 25994U 99068A   26250.50000000  .00000521  00000+0  11826-3 0  9993
	2 25994  98.1234 250.1234 0001234  90.0000 270.0000 14.57109999999999
	""")
	return ["p_tlesrc=TLE file (the set nearest the date)", "p_tlefile=$f"]
end

# OB.DAAC's file-search answers for the gallery's granules, as they really are (asked 2026-09-28), put
# in the answer cache so the name resolution runs offline. Note T125500 vs T134001: the seconds differ.
function obdaac_pages()
	IG._RS_FOUND["AQUA_MODIS.20210902T1330*.L2.SST.*"] = (time(), ["AQUA_MODIS.20210902T133001.L2.SST.NRT.nc",
		"AQUA_MODIS.20210902T133001.L2.SST.nc"])
	IG._RS_FOUND["AQUA_MODIS.20210902T1335*.L2.SST.*"] = (time(), ["AQUA_MODIS.20210902T133501.L2.SST.NRT.nc",
		"AQUA_MODIS.20210902T133501.L2.SST.nc"])
	IG._RS_FOUND["AQUA_MODIS.20210906T1310*.L2.OC.*"] = (time(), ["AQUA_MODIS.20210906T131001.L2.OC.nc"])
	IG._RS_FOUND["AQUA_MODIS.20210907T1350*.L2.OC.*"] = (time(), ["AQUA_MODIS.20210907T135001.L2.OC.nc"])
	IG._RS_FOUND["AQUA_MODIS.20210908T1255*.L2.OC.*"] = (time(), ["AQUA_MODIS.20210908T125500.L2.OC.nc"])
	# Not a real query: an answer with no granule, for the "not at OB.DAAC" branch.
	IG._RS_FOUND["AQUA_MODIS.20210909T0000*.L2.OC.*"] = (time(), String[])
	return nothing
end

open_empty(title::String) = (h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), title);
                             IG._register_fig!(IG.QtEmpty(h)); IG._start_pump(); IG._ensure_callbacks(); h)
close_win(h::Ptr{Cvoid}) = ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)

# A 7-band UInt16 "Landsat" cube with the band descriptions cutcube gives a Landsat scene.
function cube_file(dir::String)::String
	names = [IG.RemoteS.Lsat8_desc[k] for k = 1:7]
	A = rand(UInt16, 60, 50, 7) .÷ UInt16(4) .+ UInt16(2000)
	A[1:30, :, 5] .+= UInt16(9000)                  # a "vegetated" half: high NIR
	I = IG.GMT.mat2img(A, names = names, noconv = 1)
	f = joinpath(dir, "cube.tif")
	IG.GMT.gdaltranslate(I, dest = f)
	return f
end

extras(h::Ptr{Cvoid}) = (IG._pump_once(); [t[2] for t in IG._scene_state(h)["extras"]])

end # @testmodule

@testitem "Remote sensing: every submenu entry builds its dialog" tags=[:gui, :remotes] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(IG.GMT.peaks())
	try
		trig(p) = ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, p)
		# Firing an entry BUILDS its dialog from deps/ui/remotes_<tool>.ui, so a missing or broken .ui
		# fails here and not on the user's screen.
		for entry in ("Spectral indices", "True color", "Landsat calibration", "Supervised classification",
		              "Band cube (cut)", "MODIS L2 swath to grid", "MODIS scenes (Terra")
			@test trig(entry) == 1
			IG._pump_once()
		end
		# A dialog comes up IN FRONT of the iGMT window, never behind it: the last one opened is active...
		@test ccall(_test_fn(:gmtvtk_window_active_test), Cint, (Cstring,), "RemoteSModisScenes") == 1
		# ...and every one is OWNED by the iGMT window, so Windows can never put it behind that window.
		for nm in ("RemoteSIndices", "RemoteSTruecolor", "RemoteSCalibration", "RemoteSClassify",
		           "RemoteSCutcube", "RemoteSModisL2", "RemoteSModisScenes")
			@test ccall(_test_fn(:gmtvtk_window_owned_test), Cint, (Cstring,), nm) == 1
		end
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "Remote sensing: Enter in a text box never runs the tool (MODIS L2 swath to grid)" tags=[:gui, :remotes] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(IG.GMT.peaks())
	real = IG._RS_TOOLS["modisl2"]
	asked = String[]
	IG._RS_TOOLS["modisl2"] = (s, d, io) -> (push!(asked, get(d, "what", "")); true)
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "MODIS L2 swath to grid") == 1
		for _ in 1:5; IG._pump_once(); end
		@test "init" in asked                                   # the dialog is up and talking
		for w in ("p_file", "p_inc", "p_rg_w", "p_rg_n")
			@test ccall(_test_fn(:gmtvtk_press_return_test), Cint, (Cstring, Cstring), "RemoteSModisL2", w) == 1
			IG._pump_once()
		end
		@test !("compute" in asked)                             # only the Grid it button grids
	finally
		IG._RS_TOOLS["modisl2"] = real
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "Remote sensing: MODIS scene footprints reproduce the RemoteS gallery" tags=[:gui, :remotes] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	h = RemoteSGui.open_empty("remotes footprints")
	dir = mktempdir()
	try
		RemoteSGui.obdaac_pages()
		ok, r = RemoteSGui.rs(h, vcat(["tool=modisscenes", "what=scenes", "p_sat=AQUA",
		                               "p_start=2021-09-02T13:30:00", "p_minutes=10"], RemoteSGui.tle_kv(dir)))
		@test ok == 1
		# The two scenes the gallery prints (Dsc[1].header, Dsc[2].header), as OB.DAAC names them now:
		# the refined file wins over the NRT one, and SST4 is not SST. Clickable, one per scene.
		u = "https://oceandata.sci.gsfc.nasa.gov/getfile/"
		@test occursin("set:p_found=<div align=\"right\"><a href=\"$(u)AQUA_MODIS.20210902T133001.L2.SST.nc\">", r)
		@test occursin("<br><a href=\"$(u)AQUA_MODIS.20210902T133501.L2.SST.nc\">", r)
		IG._pump_once()
		st = IG._scene_state(h)
		# An empty window got a geographic map framed round the scenes, plus coastline + footprints.
		@test st["has_surface"] == 1 && st["flat2d"] == 1 && st["crs"] == 1
		@test st["n_overlays"] == 2
		@test st["x0"] < -15 && st["x1"] > 5 && st["y0"] < 20 && st["y1"] > 50
	finally
		RemoteSGui.close_win(h)
		try rm(dir; recursive = true, force = true) catch end
	end
end

@testitem "Remote sensing: find the scenes over a point (the gallery's answer)" tags=[:gui, :remotes] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	h = RemoteSGui.open_empty("remotes find")
	dir = mktempdir()
	try
		RemoteSGui.obdaac_pages()
		ok, r = RemoteSGui.rs(h, vcat(["tool=modisscenes", "what=find", "p_sat=AQUA", "p_start=2021-09-07T17:00:00",
		                               "p_lon=-8", "p_lat=36", "p_days=-2", "p_daynight=Day passes",
		                               "p_product=Chlorophyll-a (L2 OC)"], RemoteSGui.tle_kv(dir)))
		@test ok == 1
		u = "https://oceandata.sci.gsfc.nasa.gov/getfile/"
		n1, n2 = "AQUA_MODIS.20210906T131001.L2.OC.nc", "AQUA_MODIS.20210907T135001.L2.OC.nc"
		@test occursin("set:p_found=<div align=\"right\"><a href=\"$(u)$(n1)\">$(u)$(n1)</a><br><a href=\"$(u)$(n2)\">", r)
		# OB.DAAC's :00 wins over the computed :01.
		@test IG._rs_resolve_scene("AQUA_MODIS.20210908T125501.L2.OC.NRT.nc") == ("AQUA_MODIS.20210908T125500.L2.OC.nc", :found)
		# OB.DAAC answered, without the granule: left out of the panel, only counted in the status.
		@test IG._rs_scene_links(["AQUA_MODIS.20210909T000001.L2.OC.NRT.nc"]) == ("", 1, 0)
		@test occursin("1 not (yet) at OB.DAAC", IG._rs_links_note(1, 0))
	finally
		RemoteSGui.close_win(h)
		try rm(dir; recursive = true, force = true) catch end
	end
end

@testitem "Remote sensing: the Earthdata login goes to .netrc and comes back" tags=[:gui, :remotes] setup=[RemoteSGui] begin
	h = RemoteSGui.open_empty("remotes netrc")
	dir = mktempdir()
	old = get(ENV, "NETRC", nothing)
	try
		f = joinpath(dir, ".netrc")
		write(f, "machine example.org\n    login other\n    password keepme\n")
		ENV["NETRC"] = f
		ok, r = RemoteSGui.rs(h, ["tool=modisscenes", "what=savelogin", "p_ed_user=someone", "p_ed_pass=s3cret"])
		@test ok == 1
		txt = read(f, String)
		@test occursin("machine example.org\n    login other\n    password keepme", txt)       # kept
		@test occursin("machine urs.earthdata.nasa.gov\n    login someone\n    password s3cret", txt)
		# Saving again replaces the entry; it does not add a second one.
		@test RemoteSGui.rs(h, ["tool=modisscenes", "what=savelogin", "p_ed_user=someone", "p_ed_pass=n3w"])[1] == 1
		@test count("urs.earthdata.nasa.gov", read(f, String)) == 1 && occursin("password n3w", read(f, String))
		ok, r = RemoteSGui.rs(h, ["tool=modisscenes", "what=init"])
		@test ok == 1 && occursin("set:p_ed_user=someone", r) && occursin("set:p_ed_pass=n3w", r)
		ok, r = RemoteSGui.rs(h, ["tool=modisscenes", "what=savelogin", "p_ed_user=someone", "p_ed_pass="])
		@test InteractiveGMT._errored(ok, "Remote sensing") == 0 && occursin("Give both the login and the password", r)
		@test occursin("password n3w", read(f, String))                               # left untouched
	finally
		old === nothing ? delete!(ENV, "NETRC") : (ENV["NETRC"] = old)
		RemoteSGui.close_win(h)
		try rm(dir; recursive = true, force = true) catch end
	end
end

@testitem "Remote sensing: indices, true color and classification into the window" tags=[:gui, :remotes] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	dir = mktempdir()
	h = RemoteSGui.open_empty("remotes cube")
	try
		F = RemoteSGui.cube_file(dir)
		# The file's band descriptions become the pickers, preselected by name.
		ok, r = RemoteSGui.rs(h, ["tool=indices", "what=refresh", "p_file=$F", "p_index_idx=0"])
		@test ok == 1
		@test occursin("label:lbl_b1=red", r) && occursin("set:p_b1=3", r) && occursin("set:p_b2=4", r)
		call(extra) = RemoteSGui.rs(h, vcat(["tool=indices", "what=compute", "p_file=$F", "p_index_idx=0",
		                                     "p_b1_idx=3", "p_b2_idx=4"], extra))
		@test call(["p_output=Index values (grid)"])[1] == 1
		@test call(["p_output=Mask (≥ threshold)", "p_threshold=0.3"])[1] == 1
		@test call(["p_output=Classes (up to 3 separators)", "p_classes=0, 0.2, 0.4"])[1] == 1
		G = IG._find_object(h, :grid, "NDVI — cube")
		@test G !== nothing && maximum(filter(isfinite, G.z)) > 0.3      # the high-NIR half
		@test IG._find_object(h, :image, "NDVI mask ≥ 0.3 — cube") !== nothing
		@test IG._find_object(h, :image, "NDVI classes — cube") !== nothing

		ok, r = RemoteSGui.rs(h, ["tool=truecolor", "what=compute", "p_file=$F", "p_r_idx=3", "p_g_idx=2",
		                          "p_bl_idx=1", "p_stretch=1"])
		@test ok == 1
		I = IG._find_object(h, :image, "True color [4, 3, 2] — cube")
		@test I !== nothing && size(I, 3) == 3

		# Supervised classification: two classes, one polygon each, over the two halves of the cube.
		C = IG.GMT.gmtread(F); x0, x1, y0, y1 = C.range[1:4]; xm = (x0 + x1) / 2
		tr = joinpath(dir, "train.txt")
		open(tr, "w") do io
			for (cl, a, b) in (("veg", x0, xm), ("bare", xm, x1))
				println(io, "> Attrib(class=$cl)")
				for (x, y) in ((a + 1, y0 + 1), (b - 1, y0 + 1), (b - 1, y1 - 1), (a + 1, y1 - 1), (a + 1, y0 + 1))
					println(io, x, " ", y)
				end
			end
		end
		ok, r = RemoteSGui.rs(h, ["tool=classify", "what=compute", "p_file=$F", "p_train=$tr", "p_depth=3",
		                          "p_density=0.5"])
		@test ok == 1 && occursin("veg,bare", r)
		@test IG._find_object(h, :image, "Classes — cube") !== nothing
	finally
		RemoteSGui.close_win(h)
		# GMT.jl's gmtread keeps a GeoTIFF open on Windows (docs/GMTJL_GRID_ISSUES.md, issue 6), so the
		# cube may still be locked here; the temporary directory then goes with the OS's temp cleanup.
		try rm(dir; recursive = true, force = true) catch end
	end
end
