# :unit -- SACRED_LAW.md, ENFORCED. Each law in that file that names the code shape betraying it ("grep for
# X: any hit is this bug coming back") is turned here into a check over the source tree, so a violation
# fails the suite the moment it is written instead of being found by the user. Every failure prints the
# offending file:line. The laws whose violation only shows at run time (grid independence, a line on a
# second grid, the VE laws) are GUI items: test-grid-independence-gui.jl, test-ve-rules-gui.jl.

@testitem "SACRED LAW: one law, one source scan" tags=[:unit, :law] begin
	root = pkgdir(InteractiveGMT)
	cpp  = [joinpath(dp, f) for (dp, _, fs) in walkdir(joinpath(root, "deps", "src")) for f in fs
	        if endswith(f, ".cpp") && !occursin("third_party", dp)]
	jl   = [joinpath(dp, f) for (dp, _, fs) in walkdir(joinpath(root, "src")) for f in fs if endswith(f, ".jl")]
	rel(p) = relpath(p, root)

	# lines of code only: C/C++ `//` comments and Julia `#` comments stripped (a gravestone comment
	# naming a deleted helper is documentation, not a violation)
	code_c(l)  = (i = findfirst("//", l); i === nothing ? l : l[1:first(i)-1])
	code_jl(l) = (i = findfirst('#', l); i === nothing ? l : l[1:i-1])

	# every hit of `pat` in code: (file:line, the line, the enclosing top-level function)
	function hits(files, pat; strip = code_c)
		out = Tuple{String,String,String}[]
		for f in files
			fn = ""
			for (k, l) in enumerate(eachline(f))
				# a C++ top-level definition starts in column 0 with a type and has `(`; Julia's `function`
				if occursin(r"^(static |GMTVTK_API )?[A-Za-z_][\w:<>,\* ]*\s+\**[A-Za-z_]\w*\(", l) ||
				   occursin(r"^function |^[A-Za-z_]\w*\(.*\)\s*=", l)
					fn = l
				end
				occursin(pat, strip(l)) && push!(out, ("$(rel(f)):$k", strip(l), fn))
			end
		end
		out
	end
	report(name, hs) = isempty(hs) || @error "SACRED LAW VIOLATED: $name" hits = join(["$(h[1])  $(strip(h[2]))" for h in hs], "\n")

	# --- Preference-application law: a grid colour transfer function is built ONLY by makeGridCTF, which
	#     applies the NaN fill colour by construction. Any other construction forgets it.
	#     buildSceneContent's no-CPT fallback ramp is exempt: hand-built HSV, NaN colour applied right after it.
	h = filter(x -> !occursin("makeGridCTF", x[3]) && !occursin("buildSceneContent(", x[3]),
	           hits(cpp, r"vtkNew<vtkColorTransferFunction>|vtkSmartPointer<vtkColorTransferFunction>::New|vtkColorTransferFunction::New"))
	report("Preference-application law (a CTF built outside makeGridCTF)", h)
	@test isempty(h)

	# --- Derived-variable display law: the per-kind show/hide helpers are DELETED; _adopt_derived! is the
	#     one transition.
	h = hits(jl, r"_show_object!|_hide_other_objects!|_hide_all_grids!"; strip = code_jl)
	report("Derived-variable display law (a deleted per-kind show/hide helper is called)", h)
	@test isempty(h)

	# --- Derived-variable display law: a computed result is a NEW handle, never written over its source.
	#     `_apply_host_grid!` replaces a window grid's data IN PLACE; it is legitimate only where the SAME
	#     element takes new data of its own (a cube's layer slider, a movie frame, a nested level being
	#     filled, an undo). Any other caller is a derive tool overwriting its source — 2026-10-08, Transplant
	#     2nd grid did exactly that and no check caught it.
	ok_inplace = r"^function (_cube_write_surface!|replace_grid!|_on_nested_transplant|_on_transplant_undo|_apply_host_grid!)\("
	h = filter(x -> !occursin(ok_inplace, x[3]), hits(jl, r"_apply_host_grid!\("; strip = code_jl))
	report("Derived-variable display law (a result written over its source grid with _apply_host_grid!)", h)
	@test isempty(h)

	# --- No fallback to someone else's axes: an axes resolver never ends in an UNCONDITIONAL
	#     `return &s->baseAxes;` (a conditioned one, naming the base by its own name, is fine).
	h = filter(x -> !occursin(r"\bif\b", x[2]), hits(cpp, r"return\s*&\s*s->baseAxes\s*;"))
	report("No-fallback axes law (an unconditional `return &s->baseAxes`)", h)
	@test isempty(h)

	# --- Raster-own-axes law: gmtvtk_grow_frame_h stays deleted, and no adopt/reframe call of a raster is
	#     gated on the window being empty / being a promote.
	h = vcat(hits(cpp, r"grow_frame"), hits(jl, r"grow_frame"; strip = code_jl))
	report("Raster-own-axes law (grow_frame is back)", h)
	@test isempty(h)
	h = vcat(filter(x -> occursin(r"promote|was_empty|isempty|wasEmpty|emptyStart", x[2]),
	                hits(jl, r"_adopt_new_element\("; strip = code_jl)),
	         filter(x -> occursin(r"promote|wasEmpty|emptyStart|isEmpty", x[2]), hits(cpp, r"gmtvtk_show_new_element_h\(")))
	report("Raster-own-axes law (a raster's reframe gated on the window being empty)", h)
	@test isempty(h)

	# --- Tsunami one-bake law / the SACRED LAW's shading engine: the relief and PBR shades are called
	#     only from bakeLayerRGBA and hillshadeMapper.
	h = filter(x -> !occursin(r"\b(bakeLayerRGBA|hillshadeMapper|applyReliefShade|applyPBRShade)\(", x[3]),
	           hits(cpp, r"\b(applyPBRShade|applyReliefShade)\("))
	report("One-bake law (a relief/PBR shade called outside bakeLayerRGBA / hillshadeMapper)", h)
	@test isempty(h)

	# --- Grid memory-layout law: no 2-D indexing of a grid's raw `z` and no nx/ny taken from size(z) in
	#     the tools -- the buffer is TRB, reached through _zmat / _grid_dims.
	#     `_grid_dims` IS that accessor: its size(G.z) is the law's own fallback, not a violation of it.
	#     transplant.jl and empilhador.jl (tsunami code) are EXEMPT: user-protected, never edited to satisfy a scan.
	h = filter(x -> !occursin(r"^function _grid_dims\(", x[3]) &&
	                !startswith(x[1], joinpath("src", "transplant.jl")) && !startswith(x[1], joinpath("src", "empilhador.jl")),
	           vcat(hits(jl, r"\bG\.z\[[^\]]*,[^\]]*\]"; strip = code_jl),
	                hits(jl, r"\b(ny|nx)\s*,\s*(nx|ny)\s*=\s*size\(G\.z\)"; strip = code_jl)))
	report("Grid memory-layout law (2-D indexing of G.z / nx,ny from size(G.z))", h)
	@test isempty(h)

	# --- One quantity, one function: "is this grid/image geographic?" is `_isgeographic` (cpt.jl), the
	#     test the window's own adders use. The CRS-only `_isgeog` / `GMT.isgeog` on a grid or image
	#     disagrees on every lon/lat file without a CRS (Tsunami travel times' Mirone run came back ~0 h).
	#     Exempt: the two definitions themselves, and `_is_referenced` (a different question: "does it
	#     carry ANY georeference", where only a real CRS counts).
	h = filter(x -> !occursin(r"^(function _isgeog\(|_isgeographic\(|function _is_referenced\()", x[3]) &&
	                !startswith(x[2], "_isgeographic("),
	           hits(jl, r"\b(_isgeog|GMT\.isgeog)\((G\w*|I\w*|bat\w*)\)"; strip = code_jl))
	report("Geographic test: _isgeog / GMT.isgeog on a grid or image (use _isgeographic)", h)
	@test isempty(h)

	# --- One question, one function: "which grid does this tool work on?" is `_active_grid` (savefile.jl)
	#     -- the layer ON DISPLAY, through the C++ activeGridName the colour bar, Z axis and readout use.
	#     `_find_object(scene, :grid, "")` is the BASE grid: Contours traced the bathymetry under a
	#     travel-time layer, and Multiscale / Spline Smooth / SDG / Thicken did the same (2026-10-13).
	#     Exempt, because there the base IS what is meant: _active_grid itself (its "" = base branch);
	#     NSWING and Nested grids (layer0 = the base bathymetry by definition); drop.jl's z-unit share
	#     with the base; transplant.jl (user-protected tsunami code, never edited to satisfy a scan).
	ok_base = r"^function (_active_grid|_on_nswing_run|_nswing_\w+|_on_nested\w*|_nested_\w+|_add_grid_to_scene)\("
	h = filter(x -> !occursin(ok_base, x[3]) &&
	                !startswith(x[1], joinpath("src", "transplant.jl")) &&
	                !startswith(x[1], joinpath("src", "nswing.jl")) && !startswith(x[1], joinpath("src", "nested.jl")),
	           hits(jl, r"_find_object\(\s*\w+\s*,\s*:grid\s*,\s*\"\"\s*\)"; strip = code_jl))
	report("Grid Tool on the BASE grid: _find_object(scene, :grid, \"\") (use _active_grid)", h)
	@test isempty(h)

	#     The same question asked two more ways, both answering with the base / first-registered grid:
	#     the window's own `fig.G` (_FIGREG — the "selected" sentinel of the FFT, grdsample, RTP 3D,
	#     grdgravmag3d, grdredpol dialogs) and `_find_object_named(scene, :grid)` (the FFT tool, ROI crop,
	#     the vector wizard). Found 2026-10-21: every one of them transformed the bathymetry under a
	#     displayed derived layer. Exempt where the base IS what is meant: the registry internals
	#     (savefile.jl), Okada's sampling grid (SACRED_LAW.md: the window's own grid object, the
	#     bathymetry it is added to), hillshade's documented fallback to the primary, and the Julia
	#     API that takes a FIGURE and so means that figure's own surface (recolor!, add_curtain!,
	#     replace_grid!, an overlay's z source, its info, the grid calculator's object list, the Tiles
	#     tool's in-place frame test) — no layer is "displayed" there, the caller named the figure.
	ok_fig = r"^(function (_elastic_sample_grid|_hs_restore_original|_recolor|add_curtain!|_densify_xy|_add_overlay!|_gridcalc_objects|replace_grid!|_tiles_inplace)\(|_info_object\()"
	h = filter(x -> !occursin(ok_fig, x[3]) && !startswith(x[1], joinpath("src", "savefile.jl")) &&
	                !startswith(x[1], joinpath("src", "transplant.jl")),
	           vcat(hits(jl, r"\bfig\.G\b"; strip = code_jl),
	                hits(jl, r"_find_object_named\(\s*\w+\s*,\s*:grid\s*\)"; strip = code_jl)))
	report("Grid Tool on the base / first grid: fig.G or _find_object_named(scene, :grid) (use _active_grid)", h)
	@test isempty(h)

	# --- A symbol's look is the symbol code's (50_scene.cpp: addSymbols, its kind rebuild, its own
	#     properties menu). A tool that places a symbol through a shared draw tool and then repaints it
	#     makes that one symbol behave differently: the TTT source painted yellow hid the yellow
	#     selection handle a double-click shows on every other circle (2026-10-13).
	h = filter(x -> !startswith(x[1], joinpath("deps", "src", "50_scene.cpp")),
	           hits(cpp, r"(\.|->)fillRGB\s*\[\s*\d\s*\]\s*="))   # a WRITE through an object, not the struct default
	report("Symbol repainted outside the symbol code (fillRGB written outside 50_scene.cpp)", h)
	@test isempty(h)
end
