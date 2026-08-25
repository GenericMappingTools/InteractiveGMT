# cptbuild.jl — GMT menu > "Make CPT (makecpt / grd2cpt)": build a colour palette table and apply it
# to the window's grid and/or write it to a .cpt file, through GMT.jl's own `makecpt` and `grd2cpt`.
#
# The C++ dialog is CptBuildDialog (70_window.cpp, loads deps/ui/cpt_build_dialog.ui).
#
# ONE dialog for TWO modules because they are one question asked two ways: "which colours, over which
# z range?". makecpt takes the range from the boxes (or leaves the master's own), grd2cpt takes it
# from the DATA — the window's grid — and can equalize the histogram. Everything after that decision
# (continuity, inversion, log, truncation, colour model, transparency, the BFN colours) is the same
# option on both sides, so it lives in one shared group and is sent once.
#
# This is NOT a second Color Palettes tool (Image > Color Palettes, palettes.jl): that one is a
# palette CHOOSER working in RGB rows, Mirone's. This one drives the two GMT modules and hands the
# result to the SAME per-layer recolour road the colour-bar chooser uses (`_recolor_grid`, cpt.jl),
# so a CPT built here reaches the surface, its LOD tiles and the colour bar exactly like any other.

# The options both modules share, verbatim. Each is passed only when the dialog actually set it, so
# an untouched control leaves the module's own default in charge.
function _cptb_common!(kw::Dict{Symbol,Any}, d::Dict{String,String})
	_on(d, "continuous") && (kw[:Z] = true)
	_on(d, "log")        && (kw[:Q] = true)
	inv = _get(d, "invert")
	if !isempty(inv)
		(inv in ("c", "z", "cz")) || error("invert takes c (colours), z (z-values) or both, not '$inv'")
		kw[:I] = inv
	end
	cm = _get(d, "colormodel");  isempty(cm) || (kw[:F] = cm)
	# -G truncates the master before anything else is done to it. One end alone is legal: the other
	# is written as NaN, which is how GMT is told to leave that end where it is.
	glo, ghi = _get(d, "glo"), _get(d, "ghi")
	if !(isempty(glo) && isempty(ghi))
		for (v, what) in ((glo, "lower"), (ghi, "upper"))
			(isempty(v) || tryparse(Float64, v) !== nothing) ||
				error("the $what truncation limit must be a number, not '$v'")
		end
		kw[:G] = (isempty(glo) ? "NaN" : glo) * "/" * (isempty(ghi) ? "NaN" : ghi)
	end
	a = _get(d, "alpha")
	if !isempty(a)
		av = tryparse(Float64, a)
		(av === nothing || !(0 <= av <= 100)) && error("transparency is a percentage 0-100, not '$a'")
		kw[:A] = a * (_on(d, "alphaall") ? "+a" : "")
	end
	bfn = _get(d, "bfn")                      # "" | D | Di | M | N — the four ways to set BFN
	if !isempty(bfn)
		bfn == "D"  ? (kw[:D] = true) :
		bfn == "Di" ? (kw[:D] = "i")  :
		bfn == "M"  ? (kw[:M] = true) :
		bfn == "N"  ? (kw[:N] = true) :
		error("unknown background/foreground mode '$bfn'")
	end
	_on(d, "categorical") && (kw[:W] = _on(d, "wrap") ? "w" : true)
	return kw
end

# makecpt's -T: min/max[/inc][+n|+l|+i|+b]. Empty = no -T at all, which keeps the master CPT's own
# range — a legitimate and common request, so it is not an error.
function _cptb_T(d::Dict{String,String})::String
	tmin, tmax, tinc = _get(d, "tmin"), _get(d, "tmax"), _get(d, "tinc")
	if isempty(tmin) && isempty(tmax)
		isempty(tinc) || error("an interval needs the range it divides — give the min and max too")
		return ""
	end
	(isempty(tmin) || isempty(tmax)) && error("give BOTH ends of the range, or neither")
	for v in (tmin, tmax, tinc)
		(isempty(v) || tryparse(Float64, v) !== nothing) || error("the range takes numbers, not '$v'")
	end
	s = tmin * "/" * tmax
	isempty(tinc) && return s
	return s * "/" * tinc * _get(d, "tmod")
end

# grd2cpt's data-side options: -E [nlevels][+c], -L min/max, -S h|l|m|u.
function _cptb_grd!(kw::Dict{Symbol,Any}, d::Dict{String,String})
	nl = _get(d, "nlevels")
	if !isempty(nl) || _on(d, "cdf")
		if !isempty(nl)
			(tryparse(Int, nl) === nothing || parse(Int, nl) < 2) &&
				error("the number of levels must be an integer >= 2, not '$nl'")
		end
		kw[:E] = nl * (_on(d, "cdf") ? "+c" : "")
	end
	lmin, lmax = _get(d, "lmin"), _get(d, "lmax")
	if !(isempty(lmin) && isempty(lmax))
		for v in (lmin, lmax)
			(isempty(v) || tryparse(Float64, v) !== nothing) ||
				error("the data limits take numbers, not '$v'")
		end
		# GMT's own spelling for "only one limit": a hyphen stands for the end left alone.
		kw[:L] = (isempty(lmin) ? "-" : lmin) * "/" * (isempty(lmax) ? "-" : lmax)
	end
	sym = _get(d, "symmetric")
	if !isempty(sym)
		(sym in ("h", "l", "m", "u")) ||
			error("the symmetric mode is h, l, m or u (see the manual), not '$sym'")
		kw[:S] = sym
	end
	return kw
end

# C callback (Make button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaCptBuildFn. Returns Cint 1 on success, 0 on failure.
function _on_cptbuild(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode", "make")
		out  = _get(d, "outfile")
		apply = _on(d, "apply")
		(apply || !isempty(out)) ||
			error("nothing to do with the CPT — apply it to the window, save it, or both")

		kw = Dict{Symbol,Any}()
		master = _get(d, "master");  isempty(master) || (kw[:cmap] = master)
		_cptb_common!(kw, d)

		local C
		if mode == "grd"
			# The palette is derived from the DATA, so it needs the grid the window is showing.
			G = _find_object(scene, :grid, _get(d, "grid"))
			G === nothing && error("no grid in this window to build a palette from")
			_cptb_grd!(kw, d)
			C = GMT.grd2cpt(G; kw...)
		elseif mode == "make"
			T = _cptb_T(d);  isempty(T) || (kw[:T] = T)
			C = GMT.makecpt(; kw...)
		else
			error("unknown mode '$mode'")
		end
		# grd2cpt hands back a tuple when it was also asked for the CDF; the palette is the first half.
		isa(C, Tuple) && (C = C[1])
		(C isa GMTcpt && !isempty(C.colormap)) || error("the module returned no palette")

		isempty(out) || GMT.gmtwrite(String(out), C)
		if apply
			# The SAME road the colour-bar's own colormap chooser takes: the layer is addressed by its
			# tag (gridSel) and coloured over its own z range, so this never recolours "the first grid".
			zmn = tryparse(Float64, _get(d, "zmin"))
			zmx = tryparse(Float64, _get(d, "zmax"))
			(zmn === nothing || zmx === nothing || !(zmx > zmn)) &&
				error("this window has no grid to apply the palette to")
			sel = something(tryparse(Int, _get(d, "gridsel")), -1)
			# `_cpt_nodes_range` + `gmtvtk_set_cpt_grid` (cpt.jl) is the pair `_recolor_grid` is made
			# of, and the node builder keeps a CPT's OWN z boundaries when it has them — which is the
			# whole point of a palette built with an explicit -T. The wrapper itself is not used only
			# because its return message interpolates the colormap into a string, and here that
			# colormap is a whole GMTcpt object rather than a name.
			cz, crgb, n = _cpt_nodes_range(zmn, zmx, C)
			n < 2 && error("the palette could not be turned into colour nodes")
			ccall(_fn(:gmtvtk_set_cpt_grid), Cvoid,
			      (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint),
			      scene, Cint(sel), cz, crgb, Cint(n))
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Make CPT FAILED: $(sprint(showerror, e))")
		@warn "Make CPT FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_cptbuild()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_cptbuild, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_cptbuild_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
