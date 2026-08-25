# grdvolume.jl — GMT menu > "grdvolume": the area, volume and mean height of the window's grid above
# (or below, or between) contour levels, through GMT.jl's own `grdvolume`.
#
# The C++ dialog is GrdVolumeDialog (70_window.cpp, loads deps/ui/grdvolume_dialog.ui).
#
# The module answers with a TABLE — one row per contour tried — so the result goes to the window's
# Data Viewer, and when a RANGE of contours was asked for it also goes to the X,Y plot tool: the
# mean-height-versus-contour curve is the whole point of scanning a range (the Optimal Robust
# Separator of Wessel [1998, 2016]), and reading it off a spreadsheet is not the same thing.

# -C in the five shapes the dialog offers, which are the module's own four plus "no -C at all"
# (the whole grid). Everything is checked here because a malformed -C is the one mistake that makes
# grdvolume report something plausible for a question nobody asked.
function _gvol_C(d::Dict{String,String})::String
	mode = _get(d, "cmode", "all")
	num(key, what) = begin
		v = _get(d, key)
		isempty(v) && error("give $what")
		(tryparse(Float64, v) === nothing) && error("$what must be a number, not '$v'")
		v
	end
	mode == "all" && return ""                       # the whole grid: report and no -C
	if mode == "above"
		return num("cval", "the contour level")
	elseif mode == "below"
		return "r" * num("cval", "the contour level")
	elseif mode == "between"
		lo, hi = num("clow", "the lower contour"), num("chigh", "the upper contour")
		(parse(Float64, lo) < parse(Float64, hi)) || error("the lower contour must be below the upper one")
		return "r" * lo * "/" * hi
	elseif mode == "range"
		lo, hi = num("clow", "the first contour"), num("chigh", "the last contour")
		dz = num("cdelta", "the contour step")
		(parse(Float64, lo) < parse(Float64, hi)) || error("the first contour must be below the last one")
		(parse(Float64, dz) > 0) || error("the contour step must be positive")
		return lo * "/" * hi * "/" * dz
	end
	error("unknown contour mode '$mode'")
end

# What the module reports, column by column. With -D (slices) the last column is the slice thickness
# rather than a mean height, so it is named for what it holds.
_gvol_colnames(slices::Bool, ncol::Int) =
	["Contour", "Area", "Volume", slices ? "Slice thickness" : "Mean height"][1:min(ncol, 4)]

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdVolumeFn. Returns Cint 1 on success, 0 on failure.
function _on_grdvolume(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The grid measured is the one ON DISPLAY (the dialog sends the active layer's label).
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		mode = _get(d, "cmode", "all")
		kw = Dict{Symbol,Any}()
		C = _gvol_C(d);  isempty(C) || (kw[:C] = C)

		slices = _on(d, "slices")
		if slices
			# -D measures each horizontal slice, so it only means something for a range of contours —
			# the module requires exactly that.
			(mode == "range") || error("slice volumes need a RANGE of contours (first, last, step)")
			kw[:D] = true
		end
		tmax = _get(d, "tmax")
		if !isempty(tmax)
			(tmax in ("h", "c")) || error("the 'best contour' rule is by height or by curvature")
			# -Tc picks the contour of maximum CURVATURE of height vs contour, which needs contours to
			# have been tried at all.
			(tmax == "c" && mode == "all") &&
				error("the maximum-curvature rule needs a range of contours to look at")
			kw[:T] = tmax
		end
		b = _get(d, "base")
		if !isempty(b)
			(tryparse(Float64, b) === nothing) && error("the base level must be a number, not '$b'")
			kw[:L] = b
		end
		u = _get(d, "unit")
		if !isempty(u)
			(length(u) == 1 && u[1] in ('e', 'f', 'k', 'M', 'n', 'u')) ||
				error("the distance unit is one of e f k M n u, not '$u'")
			kw[:S] = u
		end
		zf, zs = _get(d, "zfact"), _get(d, "zshift")
		if !isempty(zf) || !isempty(zs)
			isempty(zf) && error("a shift needs the scale factor it goes with (give 1 for no scaling)")
			for (v, what) in ((zf, "scale factor"), (zs, "shift"))
				(isempty(v) || tryparse(Float64, v) !== nothing) || error("the $what must be a number, not '$v'")
			end
			kw[:Z] = isempty(zs) ? zf : zf * "/" * zs
		end
		reg = _get(d, "region")
		(isempty(reg) || occursin("//", reg)) || (kw[:region] = reg)

		R = GMT.grdvolume(G; kw...)
		(R === nothing || isempty(R)) && error("grdvolume reported nothing for this grid")
		D = isa(R, Vector) ? R[1] : R
		isa(D, GMTdataset) || error("got a $(typeof(D)), not a table")
		m = D.data
		D.colnames = _gvol_colnames(slices, size(m, 2))

		out = _get(d, "outfile");  isempty(out) || GMT.gmtwrite(String(out), D)
		show_table(scene, D; name = mode == "all" ? "Volume (whole grid)" : "Volume by contour")

		# One contour is one number — a plot of it would be a single dot. A RANGE is a curve, and the
		# mean-height one is what the range was scanned for; the area and volume ride along with it.
		if _on(d, "plot") && size(m, 1) > 1 && size(m, 2) >= 3
			x = Float64.(@view m[:, 1])
			names = D.colnames
			p = xyplot(x, Float64.(@view m[:, size(m, 2) >= 4 ? 4 : 3]); name = names[end],
			           title = "grdvolume", xlabel = names[1], ylabel = names[end])
			add!(p, x, Float64.(@view m[:, 3]); name = names[3])
			add!(p, x, Float64.(@view m[:, 2]); name = names[2], linestyle = :dash)
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "grdvolume FAILED: $(sprint(showerror, e))")
		@warn "grdvolume FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_grdvolume()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdvolume, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdvolume_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
