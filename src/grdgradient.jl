# grdgradient.jl — GMT menu > "grdgradient": the directional derivative, the slope magnitude or the
# gradient direction (aspect) of the window's grid, through GMT.jl's own `grdgradient`.
#
# The C++ dialog is GrdGradientDialog (70_window.cpp, loads deps/ui/grdgradient_dialog.ui), whose
# layout is Mirone's grdgradient window.

# C callback (OK button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdGradientFn. Absent key = don't pass that option, so the module's own defaults
# stay in charge (no normalization, its own boundary handling).
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_grdgradient(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		fig = get(_FIGREG, scene, nothing)
		(fig isa QtFigure) || error("no grid loaded in this window")
		G = fig.G

		kw = Dict{Symbol,Any}()
		# Mirone adds -M whenever the grid is geographic (grdgradient_mir.m: `if (handles.geog)`), which
		# is GMT6's f=:g — distances converted to meters via the Flat Earth approximation. Automatic
		# there, automatic here: it is a property of the grid, not a choice.
		# Mirone passes -M whenever `handles.geog` is set (grdgradient_mir.m), and that flag is its own
		# lon/lat RANGE GUESS — not a CRS lookup. `_isgeographic` (GMT.guessgeog) is the same guess, so
		# it is the one to use here; `_isgeog` (the real CRS) is wrong for this because the common case,
		# a plain lon/lat DEM with no proj4, reports 0 and would then be differentiated in per-DEGREE
		# units: a 1-degree, ±500 m test DEM yields a slope of 149994 % without -M and 1.7 % with it.
		# (_isgeographic returns a Bool; the similarly-named _isgeog returns 1/0 for ccalls.)
		_isgeographic(G) && (kw[:f] = :g)
		title = "Gradient"
		# -E, the Lambertian radiance family. GMT states it overrides -A/-D/-S, and the dialog grays
		# those out accordingly, so this branch is exclusive. Peucker and manip hard-wire the light to
		# 315/45 (the module ignores any view given), which is why they send none.
		lamb = _get(d, "lambert")
		if !isempty(lamb)
			nt = Dict{Symbol,Any}()
			lamb == "simple"  && (nt[:simple]  = true)
			lamb == "peucker" && (nt[:peucker] = true)
			lamb == "manip"   && (nt[:manip]   = true)
			az = _get(d, "lambazim");  el = _get(d, "lambelev")
			(isempty(az) || isempty(el)) || (nt[:view] = (parse(Float64, az), parse(Float64, el)))
			for (key, sym) in (("ambient", :ambient), ("difuse", :difuse),
			                   ("specular", :specular), ("shine", :shine))
				v = _get(d, key);  isempty(v) || (nt[sym] = parse(Float64, v))
			end
			kw[:lambert] = NamedTuple(nt)
			title = "Lambertian radiance"
		end

		# -A can also be a GRID of azimuths, one per node, instead of one or two constants.
		azgrid = _get(d, "azimgrid")
		if !isempty(azgrid)
			isfile(azgrid) || error("azimuth grid not found: $azgrid")
			kw[:azim] = azgrid
			title = "Directional derivative"
		end
		# -A: one azimuth, or two (the module keeps whichever gradient is larger in magnitude).
		if haskey(d, "azim")
			a1 = _get(d, "azim");  a2 = _get(d, "azim2")
			kw[:azim] = isempty(a2) ? parse(Float64, a1) : (parse(Float64, a1), parse(Float64, a2))
			title = "Directional derivative"
		end
		if haskey(d, "finddir")
			# -D: the flag letters travel verbatim (c = trigonometric angles, o = 0-180 orientations,
			# n = +90). An empty string is the bare -D — which is also what Mirone's Slope sends, since
			# there -Sp always comes with -D.
			flags = _get(d, "finddir")
			kw[:find_dir] = isempty(flags) ? true : Symbol(flags)
			if _on(d, "slope")
				kw[:slopegrid] = true             # -S, an EXTRA output grid alongside the -D one
				title = "Slope"
			else
				title = occursin('a', flags) ? "Aspect" : "Gradient direction"
			end
		end

		# -Q: reuse one offset/sigma across grids or tiles ('c' save, 'r' read, 'R' read then delete).
		q = _get(d, "savestats");  isempty(q) || (kw[:save_stats] = q)
		# -R: the output region (the dialog fills it from the input grid, but it can be narrowed).
		reg = _get(d, "region");  isempty(reg) || (kw[:region] = reg)

		# Boundary condition. Mirone sends GMT4's -Lx / -Ly / -Lxy / -Lg; GMT6 spells the same thing as
		# the -n option's +b modifier, where PERIODIC is the letter `p` with an optional axis suffix —
		# "+bx" alone is rejected outright ("Cannot parse boundary condition x"), so the periodic cases
		# must become +bpx / +bpy / +bp. Geographic stays +bg.
		bc = _get(d, "boundary")
		if !isempty(bc)
			kw[:interp] = bc == "g" ? "+bg" : bc == "xy" ? "+bp" : "+bp" * bc
		end

		# -N: amp/sigma/offset are the module's own modifiers of whichever flavour was picked.
		nrm = _get(d, "norm")
		if !isempty(nrm)
			nt = Dict{Symbol,Any}()
			nrm == "laplace" && (nt[:laplace] = true)
			nrm == "cauchy"  && (nt[:cauchy]  = true)
			for (key, sym) in (("amp", :amp), ("sigma", :sigma), ("offset", :offset))
				v = _get(d, key);  isempty(v) || (nt[sym] = parse(Float64, v))
			end
			# Mirone's "Simple" with no modifier at all is the bare -N.
			kw[:norm] = (nrm == "simple" && isempty(nt)) ? true : NamedTuple(nt)
		end

		R = GMT.grdgradient(G; kw...)
		# FLAT ground has a perfectly good slope — zero — so its direction grid must read 0, not a
		# hole. GMT leaves NaN wherever the gradient vector is exactly zero (verified: the NaNs of a
		# seamount on a flat floor coincide one-for-one with the |grad| == 0 nodes, none anywhere the
		# surface slopes), which peppers any synthetic surface with NaN. Fill those back in — but only
		# where the INPUT had data, so genuine no-data holes stay holes.
		if haskey(kw, :find_dir) && !haskey(kw, :slopegrid) && isa(R, GMTgrid) && size(R.z) == size(G.z)
			@inbounds for k in eachindex(R.z)
				(isnan(R.z[k]) && !isnan(G.z[k])) && (R.z[k] = 0f0)
			end
			R.hasnans = 0                                  # unknown -> let downstream recheck
		end
		# -S is an EXTRA output, not a replacement: grdgradient.c writes the -G grid (the directions)
		# first and the -S grid (the slope magnitudes) second, so asking for a slope hands back BOTH.
		# Keep whichever the user actually asked for.
		if isa(R, Tuple)
			R = haskey(kw, :slopegrid) ? R[2] : R[1]
			# The checkbox promises PERCENT. -S returns the magnitude of the gradient vector, which is
			# the tangent of the slope angle, so percent is 100x that (Mirone's own definition).
			if haskey(kw, :slopegrid)
				R = deepcopy(R)
				R.z = R.z .* 100f0
				R.range[5] *= 100;  R.range[6] *= 100
			end
		end
		return _gm3d_deliver(scene, R, title, _get(d, "outfile"), false,
		                     "grdgradient " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdgradient", e)
		return Cint(0)
	end
end

function _register_grdgradient()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdgradient, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdgradient_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
