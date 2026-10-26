# Plot with GMT.jl > Run and Edit in VectorCraft / PhotoCraft (src/craftapps.jl, deps/src/70_window.cpp).

@testitem "craftapps: the handoff rewrites the last showfig, keeps the user's keywords" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	parse_stmts(src) = (s = Tuple{LineNumberNode,Any}[]; loc = LineNumberNode(1);
	                    for a in Meta.parseall(src).args; a isa LineNumberNode ? (loc = a) : push!(s, (loc, a)); end; s)
	kws(ex) = Dict(k.args[1] => k.args[2] for k in ex.args[2].args)
	# the last showfig is the one replaced; its other keywords survive, savefig/show are ours
	s = parse_stmts("using GMT\nshowfig()\nx = 1\nshowfig(frame=:a, savefig=\"mine.pdf\", show=true)\n")
	IG._script_handoff!(s, "out.png", 150)
	@test s[2][2] == :(showfig())
	k = kws(s[4][2])
	@test k[:frame] == QuoteNode(:a) && k[:savefig] == "out.png" && k[:show] == false && k[:dpi] == 150
	# a dpi means nothing to EPS/PS: the user's is dropped, none is added
	s = parse_stmts("showfig(dpi=100)\n")
	IG._script_handoff!(s, "out.eps", 0)
	@test !haskey(kws(s[1][2]), :dpi) && kws(s[1][2])[:savefig] == "out.eps"
	# no showfig at all: one is appended
	s = parse_stmts("x = 1\n")
	IG._script_handoff!(s, "out.eps", 0)
	@test length(s) == 2 && kws(s[2][2])[:show] == false
	@test_throws ErrorException IG._script_handoff!(Tuple{LineNumberNode,Any}[], "out.eps", 0)
end

@testitem "Plot with GMT.jl: Run and Edit in VectorCraft / PhotoCraft asks nothing and hands over a cropped figure" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	btn(name, click) = (b = zeros(UInt8, 256);
	                    r = ccall(GmtvtkTest._test_fn(:gmtvtk_dialog_button_test), Cint,
	                              (Cstring, Cstring, Cint, Ptr{UInt8}, Cint), "Plot with GMT.jl", name, click, b, 256);
	                    (r, unsafe_string(pointer(b))))
	# Stand-in for both programs: julia itself, which exits at once on a file it cannot run. What is
	# tested is what the button does before the launch -- no question asked, a cropped figure written.
	keys = ("INTERACTIVEGMT_VECTORCRAFT", "INTERACTIVEGMT_PHOTOCRAFT")
	old = Dict(k => get(ENV, k, nothing) for k in keys)
	# A REAL install on this machine (~/.gmt/craftapps) plus the start-up update check, which runs in
	# the background and may land at any moment, turns the button into "Update …" and the click into an
	# "Update it now?" box. That is the release check's business, not this item's: for the length of the
	# item the home folder is an empty one, so no installed copy exists to be offered an update.
	homes = ("USERPROFILE", "HOME")
	oldhome = Dict(k => get(ENV, k, nothing) for k in homes)
	tmphome = mktempdir()
	foreach(k -> ENV[k] = tmphome, homes)
	G = GMT.mat2grid(Float32[sin(ix / 3) * cos(iy / 4) for iy in 0:29, ix in 0:29]; x=[0.0, 29.0], y=[0.0, 29.0])
	f = InteractiveGMT.viz(G)
	made = String[]
	try
		for k in keys; ENV[k] = first(Base.julia_cmd().exec); end
		IG._craft_push_state()
		IG._pump_once()
		IG._script_open_editor(f.h)
		IG._pump_once()
		# the editor reports what the script cannot reproduce (a lit surface is captured as an image)
		@test all(e -> startswith(e, "script: "), IG._take_tool_errors!())
		@test btn("craftHelp", 0)[1] == 1                 # the "?" beside them (not clicked: it is a modal box)
		for (id, name, ext) in ((0, "VectorCraft", ".eps"), (1, "PhotoCraft", ".png"))
			r, label = btn("craftBtn$id", 0)
			@test r == 1 && label == "Run and Edit in $name"
			IG._CRAFT_LAST[] = ""
			@test btn("craftBtn$id", 1)[1] == 1          # returns only if no dialog was put up in its way
			out = IG._CRAFT_LAST[]
			isempty(out) || push!(made, out)
			@test isfile(out) && endswith(out, ext) && startswith(basename(out), "igmt_figure_")
		end
		# cropped to the figure: GMT.jl's raw PS page is 32767 pt square
		bb = [parse.(Float64, split(l)[2:5]) for l in eachline(made[1]) if startswith(l, "%%BoundingBox:")][1]
		@test 0 < bb[3] - bb[1] < 2000 && 0 < bb[4] - bb[2] < 2000
		I = GMT.gmtread(made[2])
		@test 0 < size(I, 1) < 10000 && 0 < size(I, 2) < 10000
	finally
		for k in keys
			old[k] === nothing ? delete!(ENV, k) : (ENV[k] = old[k])
		end
		for k in homes
			oldhome[k] === nothing ? delete!(ENV, k) : (ENV[k] = oldhome[k])
		end
		IG._craft_push_state()
		foreach(p -> rm(p; force = true), made)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "PhotoCraft round trip: MCP replies are read, the edited picture keeps the original's limits" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	ok = """{"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"{\\"path\\":\\"a.png\\",\\"warnings\\":[]}"}],"isError":false}}"""
	@test IG._pc_text(ok, "doc_save") == """{"path":"a.png","warnings":[]}"""
	bad = """{"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"no document open"}],"isError":true}}"""
	@test_throws ErrorException IG._pc_text(bad, "doc_save")
	@test IG._pc_stem("my image (1).tif") == "my_image__1_.tif"
	# a resampled picture (other size) over the SAME limits; gridline originals are not widened
	R = GMT.mat2img(zeros(UInt8, 20, 30, 3); x = [-10.0, 20.0], y = [30.0, 50.0])
	J = IG._pc_georef!(GMT.mat2img(zeros(UInt8, 45, 70, 3)), R)
	@test J.range[1:4] == R.range[1:4] && J.registration == 1 && length(J.x) == 71 && length(J.y) == 46
	G = GMT.mat2grid(zeros(Float32, 5, 5); x = [0.0, 4.0], y = [0.0, 4.0])      # gridline
	@test IG._pc_georef!(GMT.mat2img(zeros(UInt8, 9, 9, 3)), G).range[1:4] == [0.0, 4.0, 0.0, 4.0]
end

@testitem "craftapps: every system's release unpacks to the program, its CLI and (Linux) its icon" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	mktempdir() do t
		# a release archive laid out like the Linux tarball: one top folder, bin/ + share/icons
		top = joinpath(t, "src", "photocraft-9.9.9-linux-x86_64")
		for f in ("bin/photocraft", "bin/photocraft-cli", "share/icons/hicolor/16x16/apps/p.png",
		          "share/icons/hicolor/256x256/apps/p.png")
			mkpath(dirname(joinpath(top, f))); write(joinpath(top, f), f)
		end
		cd(() -> run(`$(IG._craft_tar()) -czf ../rel.tar.gz photocraft-9.9.9-linux-x86_64`), joinpath(t, "src"))
		dest = joinpath(t, "dest")
		IG._craft_unpack(joinpath(t, "rel.tar.gz"), dest, mktempdir(t))
		@test sort(readdir(joinpath(dest, "bin"))) == ["photocraft", "photocraft-cli"]
		IG._craft_linux_icon(IG.PHOTOCRAFT, dest)
		@test read(joinpath(dest, "photocraft.png"), String) == "share/icons/hicolor/256x256/apps/p.png"   # the largest
		# macOS: the program to start is the one inside the .app
		app = joinpath(t, "PhotoCraft.app")
		mkpath(joinpath(app, "Contents", "MacOS")); write(joinpath(app, "Contents", "MacOS", "photocraft"), "")
		@test IG._craft_binary(app) == joinpath(app, "Contents", "MacOS", "photocraft")
		@test IG._craft_binary("/usr/bin/x") == "/usr/bin/x"
	end
end

@testitem "PhotoCraft round trip: get back replaces the image in place, drapes on the grid" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# no PhotoCraft here: the edited picture is handed straight to the replacement step
	A = zeros(UInt8, 60, 90, 3); A[:, :, 1] .= 200
	f = iview(GMT.mat2img(A; x = [-10.0, 20.0], y = [30.0, 50.0]))
	g = InteractiveGMT.viz(GMT.mat2grid(Float32[ix * iy for iy in 0:20, ix in 0:30]; x = [0.0, 30.0], y = [0.0, 20.0]))
	try
		IG._pump_once()
		key, I0 = IG._find_object_named(f.h, :image)
		E = GMT.mat2img(fill(UInt8(77), 120, 180, 3))                    # edited, resampled x2
		IG._pc_put_back!(f.h, "image", "", String(key), I0, E)
		_, I1 = IG._find_object_named(f.h, :image)
		@test I1 === I0                                                  # the same image, in place
		@test I1.range[1:4] == [-10.0, 20.0, 30.0, 50.0] && all(==(0x4d), I1.image)
		@test IG._scene_state(f.h)["alive"] == 1
		# a grid: the picture comes back DRAPED on it, and a second get-back replaces that drape
		G = IG._find_object(g.h, :grid, "")
		IG._pc_put_back!(g.h, "grid", "base", "base", G, GMT.mat2img(fill(UInt8(10), 40, 60, 3)))
		IG._pump_once()
		D = IG._find_object_exact(g.h, :image, "base (PhotoCraft)")
		@test D isa GMT.GMTimage && D.range[1:4] == G.range[1:4]
		IG._pc_put_back!(g.h, "grid", "base", "base", G, GMT.mat2img(fill(UInt8(20), 40, 60, 3)))
		IG._pump_once()
		st = IG._scene_state(g.h)
		@test count(e -> e[2] == "base (PhotoCraft)", st["extras"]) == 1
		@test all(==(0x14), IG._find_object_exact(g.h, :image, "base (PhotoCraft)").image)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), g.h)
	end
end
