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
	G = GMT.mat2grid(Float32[sin(ix / 3) * cos(iy / 4) for iy in 0:29, ix in 0:29]; x=[0.0, 29.0], y=[0.0, 29.0])
	f = view_grid(G)
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
		IG._craft_push_state()
		foreach(p -> rm(p; force = true), made)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end
