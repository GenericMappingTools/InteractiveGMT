# grdredpol.jl — Geophysics > Magnetics > "grdredpol": Continuous Reduction To the Pole, a.k.a.
# differential RTP (Luis & Miranda 2008, JGR 113 B10105). Unlike the plain RTP (rtp3d.jl, one
# direction for the whole grid), both the ambient field and the magnetization may vary across the
# area: the grid is decomposed into moving windows taken as locally constant and the per-point filter
# is rebuilt by a Taylor expansion.
#
# GMT.jl has no verbose-syntax wrapper for this supplement, so it runs in MONOLITHIC mode — `gmt(...)`
# is still one IN-PROCESS call into the GMT library, never an OS subprocess.
#
# The C++ dialog is GrdRedPolDialog (70_window.cpp, loads deps/ui/grdredpol_dialog.ui).

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdRedPolFn. Absent key = don't pass that option, so the module's own defaults
# (F 25/25, W 5, T 2000, zero-padded edges, Taylor expansion on) stay in charge.
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_grdredpol(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))

		# "selected" = the grid already loaded in this window (same sentinel/resolution as the other
		# dialogs); anything else is a path to read.
		spec = _get(d, "input", "selected")
		G = if spec == "selected"
			fig = get(_FIGREG, scene, nothing)
			(fig isa QtFigure) ? fig.G : error("no grid loaded in this window")
		else
			isfile(spec) || error("grid file not found: $spec")
			_gmtread_trb(String(spec))      # grids are READ in "TRB" — THE reader
		end

		# -G with no name = give the result back instead of writing it (the dialog's own Save-as box is
		# honoured by _gm3d_deliver, so there is one write path, not two).
		opts = String["-G"]
		cdec, cdip = _get(d, "constdec"), _get(d, "constdip")
		if !isempty(cdec) && !isempty(cdip)
			push!(opts, "-C$cdec/$cdip")				# classical RTP: one direction for the whole grid
		else
			y = _get(d, "year");  isempty(y) || push!(opts, "-T$y")
		end
		# Ei/Ed are independent of the mode above: whatever is not given as a grid comes from IGRF.
		ig = _get(d, "incgrid");  isempty(ig) || push!(opts, "-Ei$ig")
		dg = _get(d, "decgrid");  isempty(dg) || push!(opts, "-Ed$dg")
		flt = _get(d, "filter");  isempty(flt) || push!(opts, "-F$flt")
		win = _get(d, "window");  isempty(win) || push!(opts, "-W$win")
		bnd = _get(d, "boundary"); isempty(bnd) || push!(opts, "-M$bnd")
		_on(d, "notaylor") && push!(opts, "-N")
		reg = _get(d, "region");  isempty(reg) || push!(opts, "-R$reg")
		zf  = _get(d, "filterfile");  isempty(zf) || push!(opts, "-Z$zf")

		cmd = "grdredpol " * join(opts, ' ')
		R = gmt(cmd, G)
		return _gm3d_deliver(scene, R, "RTP continuous", _get(d, "outfile"), false, cmd)
	catch e
		_viewer_log_error(scene, "grdredpol FAILED: $(sprint(showerror, e))")
		@warn "grdredpol FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_grdredpol()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdredpol, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdredpol_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
