# grdtrend.jl — GMT menu > "grdtrend": fit a low-order polynomial trend to the window's grid and
# return the trend surface, the residuals or the robust-fit weights, through GMT.jl's own `grdtrend`.
#
# The C++ dialog is GrdTrendDialog (70_window.cpp, loads deps/ui/grdtrend_dialog.ui), whose layout is
# Mirone's grdtrend window (src_figs/grdtrend_mir.m: the What-to-compute radios, the model-parameter
# combo, Robust Fit and Protect NaNs) plus the options GMT grew since: the 1-D +x/+y fits, -R, and the
# -W input weight grid with its +s (one-sigma) modifier.

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdTrendFn. Returns Cint 1 on success, 0 on failure.
function _on_grdtrend(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	wtmp = ""
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The grid to fit is the one ON DISPLAY (the dialog sends the active layer's label), not
		# whatever happens to be the window's base grid.
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		want = _get(d, "what")                       # "trend" | "diff" | "weights"
		n    = parse(Int, _get(d, "model", "3"))
		robust = _on(d, "robust")
		axis   = _get(d, "axis")                     # "" (surface) | "x" | "y"
		# -N: n_model plus its modifiers. 1-D fits (+x/+y) accept 1-4, the surface 1-10 — the dialog
		# already limits the combo, this is the backstop.
		nmax = isempty(axis) ? 10 : 4
		(1 <= n <= nmax) || error("number of model parameters must be 1-$nmax for this fit")
		nt = Dict{Symbol,Any}(:n => n)
		robust      && (nt[:robust] = true)
		axis == "x" && (nt[:xonly]  = true)
		axis == "y" && (nt[:yonly]  = true)

		kw = Dict{Symbol,Any}(:model => NamedTuple(nt))
		reg = _get(d, "region");  isempty(reg) || (kw[:region] = reg)

		# -W: a grid of weights for a weighted least-squares fit (+s = the values are one-sigma
		# uncertainties, so the weights become 1/sigma^2).
		# The dialog appends the "+s" modifier (values are one-sigma) to the path, GMT's own spelling —
		# so the existence check has to look past it.
		wfile = _get(d, "wfile")
		wpath = endswith(wfile, "+s") ? wfile[1:end-2] : wfile
		(isempty(wpath) || isfile(wpath)) || error("weight grid not found: $wpath")

		local R
		if want == "weights"
			# GMT has no "give me the weights" output: -W names a file that the ROBUST fit writes the
			# weights it used into (reading it first when it already exists). So run the fit with -W
			# pointed at a scratch copy and read that back. Same file semantics as the command line —
			# no second, private way of computing weights.
			robust || error("the weights are produced by the ROBUST fit — tick Robust Fit")
			wtmp = joinpath(tempdir(), "igmt_grdtrend_w_$(time_ns()).nc")
			isempty(wpath) || cp(wpath, wtmp; force = true)   # user weights in, robust weights out
			GMT.grdtrend(G; trend = true, weights = wtmp, kw...)
			isfile(wtmp) || error("grdtrend wrote no weight grid")
			R = _gmtread_trb(wtmp)      # grids are READ in "TRB" — THE reader
		else
			want == "diff" ? (kw[:diff] = true) : (kw[:trend] = true)
			isempty(wfile) || (kw[:weights] = wfile)
			R = GMT.grdtrend(G; kw...)
		end
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")

		# "Protect NaNs" (Mirone's check_NaNs): the trend is a polynomial defined EVERYWHERE, so it
		# also covers the holes the input has. Put those holes back. Only meaningful for the trend
		# surface — the residuals and the weights already follow the input's own NaNs.
		if want == "trend" && _on(d, "protectnans") && size(R.z) == size(G.z)
			holes = false
			@inbounds for k in eachindex(G.z)
				if isnan(G.z[k]);  R.z[k] = NaN32;  holes = true;  end
			end
			holes && (R.hasnans = 2)
		end

		title = want == "trend"   ? "Trend (n=$n$(robust ? ", robust" : ""))" :
		        want == "diff"    ? "Residuals (n=$n$(robust ? ", robust" : ""))" :
		                            "Weights (n=$n)"
		return _gm3d_deliver(scene, R, title, _get(d, "outfile"), false,
		                     "grdtrend " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdtrend", e)
		return Cint(0)
	finally
		(!isempty(wtmp) && isfile(wtmp)) && rm(wtmp, force = true)
	end
end

function _register_grdtrend()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdtrend, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdtrend_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
