# gmtscript.jl unit tier — the PURE parts of the GMT.jl script export: the literal renderer, the
# blob parsers, the camera -> -p conversion, the -JZ height, the colour/pen spellings and the
# recorded-command guard. No DLL, no window: everything here is string/number work that the emitter
# builds its calls out of, so a regression in any of it changes what an exported script says.
#
# The end-to-end legs (a real window -> a script that runs) live in the :gui tier, for the same reason
# every other scene test does: they need a live Scene.

@testitem "gmtscript: literal rendering" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	@test M._lit("a/b\\c") == "\"a/b\\\\c\""          # a Windows path reads back identical
	@test M._lit(:merc) == ":merc"
	@test M._lit(true) == "true"
	@test M._lit(nothing) == "nothing"
	@test M._lit(15.0) == "15"                        # a whole float prints as an integer
	@test M._lit(1.5) == "1.5"
	@test M._lit((1.0, 2.0)) == "(1, 2)"
	@test M._lit((7.0,)) == "(7,)"                    # 1-tuples keep the trailing comma
	@test M._lit((annot=:auto, axes=:WSen)) == "(annot=:auto, axes=:WSen)"
	@test M._lit((annot=:auto,)) == "(annot=:auto,)"  # …and so do 1-element NamedTuples
	# A ScriptVar renders as its NAME (the script's variable), never as its value.
	@test M._lit(M.ScriptVar(:REG, (1.0, 2.0, 3.0, 4.0))) == "REG"
	# …while the live sink resolves the same thing to the value.
	@test M._script_resolve(M.ScriptVar(:G1, 42), Dict{Symbol,Any}()) == 42
	@test M._script_resolve(nothing, Dict{Symbol,Any}()) === nothing
	# A SubString (everything `split` produces) must still render as a quoted literal, not bare text.
	@test M._lit(split("a,b", ',')[1]) == "\"a\""
end

@testitem "gmtscript: colour and pen spellings" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	@test M._script_rgb(1.0, 0.0, 0.0) == "255/0/0"
	@test M._script_rgb(0.0, 0.6, 0.0) == "0/153/0"
	@test M._script_rgb(0.0, 0.0, 0.0) == "0/0/0"
	@test M._script_linestyle(0) === nothing          # solid: no `ls` kwarg at all
	@test M._script_linestyle(1) === :dash
	@test M._script_linestyle(2) === :dot
	@test M._script_proj(true)  === :merc             # geographic -> Mercator
	@test M._script_proj(false) === :X                # anything else is plotted linearly
end

@testitem "gmtscript: vertex blob parsers" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	V = M._script_verts("1,2,3|4,5,6")
	@test size(V) == (2, 3)
	@test V[2, :] == [4.0, 5.0, 6.0]
	V2 = M._script_verts2("1,2|3,4|5,6")
	@test size(V2) == (3, 2)
	@test V2[3, :] == [5.0, 6.0]
	# Malformed vertices are DROPPED, never parsed into garbage or thrown at the caller: an export
	# must not fail wholesale because one row of one layer is odd.
	@test size(M._script_verts("1,2,3|bogus|7,8,9")) == (2, 3)
	@test size(M._script_verts2("1,2|x|5,6")) == (2, 2)
	@test isempty(M._script_verts(""))
	@test isempty(M._script_verts2(""))
	# z presence is what decides plot vs plot3 (§4 of the plan) — all-zero z is NOT 3-D.
	@test !M._script_has_z([M._script_verts("1,2,0|3,4,0")])
	@test  M._script_has_z([M._script_verts("1,2,0|3,4,7")])
end

@testitem "session: the vector snapshot parsers (overlays, symbols, rulers)" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	# One blob, two consumers (Save Session + the GMT.jl script export) — these are the shared readers.
	ov = M._parse_overlays_blob("1;1;0;0;9;6;2;3;1;myImport;trackA;-2,-2,0|0,1,0>5,5,0|6,6,0\n")
	@test length(ov) == 1
	@test ov[1].mode == 1 && ov[1].lw == 9.0 && ov[1].lstyle == 2 && ov[1].stack == 3
	@test ov[1].visible && ov[1].group == "myImport" && ov[1].name == "trackA"
	@test length(ov[1].segs) == 2 && size(ov[1].segs[1]) == (2, 3)   # '>' splits SEGMENTS
	@test isempty(M._parse_overlays_blob("1;1;0;0;9;6;0;1;1;g;n\n"))  # a line short of its vertices
	# An unchecked row is parsed, not dropped: the consumer decides (the script skips it, the session
	# restores it hidden).
	@test !M._parse_overlays_blob("1;1;0;0;9;6;0;1;0;;n;1,2,3|4,5,6\n")[1].visible

	# Symbols: the per-point size scale and colour are what make a seismicity layer scale with magnitude
	# and colour with depth. Token width is declared by the hasScale/hasRGB flags.
	sl = M._parse_symbols_blob(
		"a;12;1;1;1;0;0;0;0;1;0;2;1;0;1;1;quakes;-1,-1,0,0.5,255,0,0|0,0,0,1,0,255,0\n")
	@test length(sl) == 1 && sl[1].sym == "a" && sl[1].sizePx == 12.0
	@test sl[1].stack == 2 && sl[1].visible && !sl[1].oneShot
	@test size(sl[1].xyz) == (2, 3) && sl[1].scale == [0.5, 1.0]
	@test sl[1].rgb[1, :] == [1.0, 0.0, 0.0] && sl[1].rgb[2, :] == [0.0, 1.0, 0.0]
	# No per-point arrays -> plain "x,y,z" tokens and empty scale/rgb.
	plain = M._parse_symbols_blob("c;8;1;1;0;0;0;0;0;1;1;0;1;1;0;0;dots;1,2,3|4,5,6\n")
	@test isempty(plain[1].scale) && isempty(plain[1].rgb) && plain[1].oneShot
	# A token that lies about its width is dropped, not read as garbage.
	@test isempty(M._parse_symbols_blob("c;8;1;1;0;0;0;0;0;1;1;0;1;0;1;1;bad;1,2,3\n"))

	# Rulers travel as their clicked vertices only — the legs are re-measured on rebuild.
	rl = M._parse_rulers_blob("7;-2,2,0|0,2,0|2,0,0\n")
	@test length(rl) == 1 && rl[1].id == 7 && size(rl[1].verts) == (3, 3)
	@test isempty(M._parse_rulers_blob("3;1,2,3\n"))     # one vertex is not a measurement
	@test isempty(M._parse_rulers_blob(""))
end

@testitem "gmtscript: camera -> GMT -p azimuth/elevation" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	# A top-down window takes no -p at all, however its camera happens to sit.
	@test M._script_view(Dict("flat2d" => 1), Dict("cam_px" => 1.0)) === nothing
	# azim is a BEARING (clockwise from north), elev the angle above the horizontal.
	mk(px, py, pz) = Dict{String,Any}("cam_px" => px, "cam_py" => py, "cam_pz" => pz,
	                                  "cam_fx" => 0.0, "cam_fy" => 0.0, "cam_fz" => 0.0)
	st = Dict("flat2d" => 0)
	@test M._script_view(st, mk(0.0, -10.0, 0.0)) == (180.0, 0.0)     # due south, on the horizon
	@test M._script_view(st, mk(10.0, 0.0, 0.0))  == (90.0, 0.0)      # due east
	@test M._script_view(st, mk(0.0, -10.0, 10.0)) == (180.0, 45.0)   # south, 45° up
	# The round trip the emitter actually relies on: az=135, el=30 must come back exactly.
	horiz = 20 * cosd(30.0)
	v = M._script_view(st, mk(horiz * sind(135.0), horiz * cosd(135.0), 20 * sind(30.0)))
	@test v == (135.0, 30.0)
	# Straight overhead is degenerate in azimuth; it must not produce a NaN bearing.
	@test M._script_view(st, mk(0.0, 0.0, 10.0)) == (0.0, 90.0)
	# No camera in the dump (an older viewer build) -> no -p, not an error.
	@test M._script_view(st, Dict{String,Any}()) === nothing
end

@testitem "gmtscript: -JZ height from the viewer's own scale factors" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	ctx() = M.ScriptCtx(C_NULL, (-3.0, 3.0, -3.0, 3.0), true, :merc, 15.0, (135.0, 30.0), nothing,
	                    "script_data", false, Set{String}(), Dict{String,Int}(), String[],
	                    false, 0, 96.0)
	st  = Dict{String,Any}("zmin" => -6.0, "zmax" => 6.0)          # 12 units of z over 6 of x
	stf = Dict{String,Any}("zfac" => 0.5, "xfac" => 1.0, "ve" => 1.0)
	# figsize * (zext*zfac*ve) / (xext*xfac) = 15 * (12*0.5) / 6 = 15
	@test M._script_zsize(ctx(), st, stf) == 15.0
	# VE multiplies it, exactly as it does on screen.
	@test M._script_zsize(ctx(), st, merge(stf, Dict("ve" => 2.0))) == 30.0
	# A top-down figure gets no -JZ.
	c = ctx(); c.view = nothing
	@test M._script_zsize(c, st, stf) === nothing
	# A viewer build without the factors must REFUSE to guess, and say so in the notes.
	c2 = ctx()
	@test M._script_zsize(c2, st, Dict{String,Any}("ve" => 1.0)) === nothing
	@test any(n -> occursin("zsize", n), c2.notes)
	# A flat scene has no vertical extent to scale.
	@test M._script_zsize(ctx(), Dict{String,Any}("zmin" => 0.0, "zmax" => 0.0), stf) === nothing
end

@testitem "gmtscript: sizes and thicknesses in POINTS, as they were set" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	# Two DIFFERENT pt<->px relations live in the app and must not be conflated: symbols are sized at
	# a fixed 96 dpi (symbols.jl), line widths at the render window's own dpi (which the viewer
	# reports; 162 on the machine this was measured on). A pen SET to 4 pt must come back as 4 pt.
	@test M._script_pt_sym(10.0 * 96 / 72) == 10.0
	@test M._script_ptstr_sym(8.0 * 96 / 72) == "8.0p"
	ctx = M.ScriptCtx(C_NULL, (-3.0, 3.0, -3.0, 3.0), true, :merc, 15.0, nothing, nothing,
	                  "script_data", false, Set{String}(), Dict{String,Int}(), String[],
	                  false, 0, 162.0)
	@test M._script_pt_w(9.0, ctx) == 4.0                 # 4 pt at 162 dpi is stored as 9 px
	@test M._script_ptstr_w(9.0, ctx) == "4.0p"
	# The factor is the WINDOW's, not the figure's: a different figsize must not change a thickness.
	ctx.figsize = 30.0
	@test M._script_ptstr_w(9.0, ctx) == "4.0p"
end

@testitem "gmtscript: text blob parser (one function, groups included)" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	blob = "0;0;0;0;0;24;;Single\n-2;-2;1;0;0;12;cityLabels;Batch1\n1.5;2;1;0;0;12;cityLabels;Batch2\n"
	rows = M._parse_texts_blob(blob)
	@test length(rows) == 3
	@test rows[1].text == "Single" && rows[1].group == "" && rows[1].size == 24
	@test rows[2].group == "cityLabels" && rows[2].x == -2.0 && rows[2].text == "Batch1"
	# A 7-field line is a blob written before the group field existed (an older session zip).
	@test M._parse_texts_blob("1;2;0;0;0;18;Old\n")[1] == (x=1.0, y=2.0, r=0.0, g=0.0, b=0.0,
	                                                       size=18, group="", text="Old")
	# A label's string may itself contain ';' — it is the LAST field, so it is never split.
	@test M._parse_texts_blob("0;0;0;0;0;9;;a;b;c\n")[1].text == "a;b;c"
	@test isempty(M._parse_texts_blob(""))
	@test isempty(M._parse_texts_blob("garbage\n0;0;0;0;0;x;;bad\n"))
end

@testitem "gmtscript: recorded-command guard (tier T1)" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	@test !M._script_command_usable("")                       # nothing recorded
	# A command whose input file is gone reproduces nothing — it must not reach the script.
	@test !M._script_command_usable("grdsample C:/nope/gone.grd -I1 -Gout.nc")
	# Options alone are fine (no path to check).
	@test M._script_command_usable("grdmath -R0/1/0/1 -I0.1 X Y MUL = out.nc") ==
	      M._script_command_usable("grdmath -R0/1/0/1 -I0.1 X Y MUL = out.nc")   # deterministic
	# A command naming a file that DOES exist is usable.
	tmp = tempname() * ".nc"
	write(tmp, "x")
	try
		@test M._script_command_usable("grdinfo $tmp")
	finally
		rm(tmp; force=true)
	end
end

@testitem "gmtscript: sidecar ids stay unique and legal" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	used = Set{String}()
	# Shared with Save Session (_session_sidecar_id): spaces become underscores, a raster extension is
	# replaced rather than appended, and a repeated name is suffixed instead of overwriting.
	@test M._session_sidecar_id("Line 1", ".dat", used) == "Line_1.dat"
	@test M._session_sidecar_id("Line 1", ".dat", used) == "Line_1_2.dat"
	@test M._session_sidecar_id("long_beach.grd", ".nc", used) == "long_beach.nc"
	@test M._session_sidecar_id("a/b:c", ".nc", used) == "a_b_c.nc"
	@test M._session_sidecar_id("", ".nc", used) == "element.nc"
end

@testitem "gmtscript: colormap name capture" tags=[:unit, :fast, :gmtscript] begin
	M = InteractiveGMT
	# The one record of which CPT a layer wears (the C side keeps only resolved LUT nodes).
	@test M._cmap_tag(:geo) == "geo"
	@test M._cmap_tag("turbo") == "turbo"
	@test M._cmap_tag(nothing) == ""            # the viewer's built-in ramp has no name
	# A CPT OBJECT has no name GMT can look up again -> "" so the caller serializes the palette.
	@test M._cmap_tag(M.GMT.makecpt(range=(0, 1, 0.1))) == ""
end
