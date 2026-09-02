# gravfft.jl — GMT menu > "gravfft": spectral computation of the geopotential (gravity, geoid, VGG,
# deflections) of a surface, of the isostatic response of an elastic plate, and of the admittance |
# coherence between two grids — all through GMT.jl's own `gravfft`. No maths lives here: the module
# does every FFT; this file only turns the dialog's key=value block into gravfft keywords and puts
# the result back where it belongs (a grid goes on the window, a spectrum goes to the Data Viewer
# AND the X,Y plot tool).
#
# The C++ dialog is GravFFTDialog (70_window.cpp, loads deps/ui/gravfft_dialog.ui). Its four modes
# are the module's own three "main modes" plus the theoretical-curve-only one:
#   surface  the geopotential of the window's grid, given a density contrast     (-D [-W -E -F])
#   flexure  the isostatic response of an elastic plate under that load          (-T [-Z -Q -S +m])
#   admitt   the admittance | coherence between the window's grid and a 2nd one  (-I, 2 grids)
#   theo     a theoretical admittance curve alone, no grid at all                (-C -T -Z)

# The five -T boxes -> the module's own slash-separated list, te/rl/rm/rw[/ri][+m]. GMT itself
# rejects an incomplete list, so the four required values are demanded here rather than letting a
# half-written "7000//3300/1035" reach it.
function _gravfft_T(d::Dict{String,String}; moho::Bool = false)::String
	te, rhol = _get(d, "te"), _get(d, "rhol")
	rhom, rhow = _get(d, "rhom"), _get(d, "rhow")
	(isempty(te) || isempty(rhol) || isempty(rhom) || isempty(rhow)) &&
		error("the elastic plate model needs Te and the load, mantle and water densities")
	s = te * "/" * rhol * "/" * rhom * "/" * rhow
	rhoi = _get(d, "rhoi");  isempty(rhoi) || (s *= "/" * rhoi)     # infill density [defaults to rl]
	return moho ? s * "+m" : s
end

# -Z zm[/zl]. `needzl` is the "loading from below" model, which also needs the sub-surface load depth
# — exactly the two conditions gravfft's own parser checks.
function _gravfft_Z(d::Dict{String,String}; needzl::Bool = false)::String
	zm = _get(d, "zm")
	isempty(zm) && error("this model needs the Moho average compensation depth (Z)")
	zl = _get(d, "zl")
	needzl && isempty(zl) && error("the \"loading from below\" model also needs the load depth (Z zm/zl)")
	return isempty(zl) ? zm : zm * "/" * zl
end

# -N: the FFT dimension directive plus the detrend / extension / taper modifiers, in the order the
# manual lists them. An EMPTY string means "don't pass -N at all", so the module's own defaults
# (dimensions chosen for speed+accuracy, Parker's +h mid-value removal, +e edge-point symmetry)
# stay in charge — never re-stated here.
function _gravfft_N(d::Dict{String,String})::String
	s = _get(d, "fftdim") * _get(d, "detrend") * _get(d, "extend")
	tw = _get(d, "taper");  isempty(tw) || (s *= "+t" * tw)
	_on(d, "fftverbose") && (s *= "+v")
	return s
end

# A theoretical admittance is only defined for the free-air anomaly and the geoid (gravfft refuses
# any other -F together with the "from top"/"from below" models). Same check, one place.
function _gravfft_check_field(field::AbstractString)
	(isempty(field) || field[1] == 'f' || field == "g") ||
		error("a theoretical admittance is only defined for the free-air anomaly or the geoid (F)")
	return nothing
end

# Scene Objects name of a grid result: ONE fixed name per field kind, so recomputing REPLACES the
# previous result instead of piling copies up (the law _gm3d_deliver implements).
function _gravfft_title(field::AbstractString)::String
	isempty(field) && return "Free-air anomaly"
	field[1] == 'f' && return "Free-air anomaly"
	field == "b" && return "Bouguer anomaly"
	field == "g" && return "Geoid anomaly"
	field == "v" && return "VGG"
	field == "e" && return "East deflection"
	field == "n" && return "North deflection"
	return "Geopotential"
end

# A spectrum is not a surface: it goes to the window's Data Viewer tab (the numbers, error bar and
# all) AND to the X,Y plot tool (the curve one actually reads). `flags` is the -I|-C flag string, the
# only thing that says what the first column holds and whether column 2 is coherence or admittance;
# `freqname` is what the manual calls that first column for the option in hand (-I says wavenumber,
# -C says frequency — the same 1/m either way).
#   3 columns: frequency|wavelength, admittance|coherence, one-sigma error
#   4 columns: + the theoretical admittance asked for with "t" (from top) or "b" (from below)
function _gravfft_curve(scene::Ptr{Cvoid}, R, title::String, outfile::AbstractString,
                        flags::AbstractString, field::AbstractString;
                        freqname::AbstractString = "Wavenumber")::Cint
	D = isa(R, Vector) ? (isempty(R) ? error("gravfft returned no spectral estimates") : R[1]) : R
	isa(D, GMTdataset) || error("got a $(typeof(D)), not a table")
	m = D.data
	size(m, 2) < 2 && error("gravfft returned $(size(m, 2)) column(s), expected at least 2")

	coh  = occursin('c', flags)
	xlab = occursin('w', flags) ? (occursin('k', flags) ? "Wavelength (km)" : "Wavelength (m)") :
	                              "$freqname (1/m)"
	ylab = coh ? "Coherence" : field == "g" ? "Admittance (m/m)" : "Admittance (mGal/m)"
	D.colnames = [xlab, ylab, "1-sigma", "Theoretical"][1:min(size(m, 2), 4)]
	isempty(outfile) || GMT.gmtwrite(String(outfile), D)
	show_table(scene, D; name = title)

	x = Float64.(@view m[:, 1])
	p = xyplot(x, Float64.(@view m[:, 2]); name = ylab, title = title, xlabel = xlab, ylabel = ylab)
	# The one-sigma error bar has no primitive of its own in the X,Y tool, so it travels as the two
	# dashed envelopes around the estimate — which is what it means anyway.
	if size(m, 2) >= 3
		e = Float64.(@view m[:, 3])
		y = Float64.(@view m[:, 2])
		add!(p, x, y .+ e; name = "+1 sigma", linestyle = :dash)
		add!(p, x, y .- e; name = "-1 sigma", linestyle = :dash)
	end
	size(m, 2) >= 4 && add!(p, x, Float64.(@view m[:, 4]); name = "theoretical", linewidth = 2)
	return Cint(1)
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGravFFTFn. An absent key = don't pass that option, so the module's own defaults
# (3 Parker terms, free-air anomaly, no observation level) stay in charge.
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_gravfft(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode", "surface")
		field = _get(d, "field")
		kw = Dict{Symbol,Any}()
		isempty(field) || (kw[:F] = field)

		nterms = _get(d, "terms")
		if !isempty(nterms)
			n = parse(Int, nterms)
			(1 <= n <= 10) || error("the Parker expansion takes 1 to 10 terms, not $n")
			kw[:E] = n
		end
		lev = _get(d, "level");  isempty(lev) || (kw[:W] = lev)     # -W<wd>[k], relative to the topography
		nfft = _gravfft_N(d);    isempty(nfft) || (kw[:N] = nfft)
		_on(d, "geog") && (kw[:f] = :g)     # lon/lat -> meters via the module's Flat Earth approximation

		# ---- Theoretical curve only (-C): no grid is read at all. GMT.jl's `gravfft` cannot serve this
		# one — it refuses to run without input data ("Missing input data to run this module") — so this
		# mode goes through GMT.jl's monolithic entry, i.e. the very command line the manual documents.
		if mode == "theo"
			np, lam = _get(d, "cn"), _get(d, "clambda")
			dep, model = _get(d, "cdepth"), _get(d, "cmodel")
			(isempty(np) || isempty(lam) || isempty(dep)) &&
				error("the theoretical curve needs the number of points, the wavelength and the mean depth")
			(model == "t" || model == "b") ||
				error("choose the \"loading from top\" or the \"loading from below\" model")
			_gravfft_check_field(field)
			flags = model * (_on(d, "cwave") ? "w" : "")
			cmd = "gravfft -C$np/$lam/$dep/$flags -T" * _gravfft_T(d) *
			      " -Z" * _gravfft_Z(d; needzl = (model == "b"))
			isempty(field) || (cmd *= " -F" * field)
			return _gravfft_curve(scene, GMT.gmt(cmd), "Theoretical admittance", _get(d, "outfile"),
			                      flags, field; freqname = "Frequency")
		end

		# ---- Every other mode operates on the grid ON DISPLAY (the dialog sends the active layer's
		# label), not on whatever happens to be the window's base grid.
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		title = _gravfft_title(field)
		if mode == "surface"
			dens = _get(d, "density")
			isempty(dens) && error("the geopotential of a surface needs a density contrast (D)")
			# Either a constant (SI) or a co-registered grid of variable density contrasts: the module
			# takes both, so only the number is converted and anything else travels as the path it is.
			kw[:density] = something(tryparse(Float64, dens), dens)
		elseif mode == "flexure"
			moho, flextopo, subplate = _on(d, "moho"), _on(d, "flextopo"), _on(d, "subplate")
			(flextopo && subplate) &&
				error("the flexural topography (Q) and the subplate load (S) are two different results")
			kw[:T] = _gravfft_T(d; moho = moho)
			flextopo && (kw[:Q] = true)
			subplate && (kw[:S] = true)
			# -Z is REQUIRED by the module for +m, and both models need their compensation depth(s);
			# outside those cases it is passed only when the dialog filled it in.
			if moho || flextopo || subplate || !isempty(_get(d, "zm"))
				kw[:Z] = _gravfft_Z(d; needzl = subplate)
			end
			title = flextopo ? "Flexural topography" :
			        subplate ? "Subplate load ($(_gravfft_title(field)))" :
			        moho     ? "Moho effect ($(_gravfft_title(field)))" :
			                   "Isostatic response ($(_gravfft_title(field)))"
		elseif mode == "admitt"
			p2 = _get(d, "grid2")
			isfile(p2) || error("the admittance needs a gravity or geoid grid: '$p2' not found")
			flags = _get(d, "iflags")
			from_top, from_below = occursin('t', flags), occursin('b', flags)
			(from_top && from_below) && error("choose only one theoretical model, from top OR from below")
			if from_top || from_below
				# A theoretical column is the plate model's, so it needs the whole model.
				_gravfft_check_field(field)
				kw[:T] = _gravfft_T(d)
				kw[:Z] = _gravfft_Z(d; needzl = from_below)
			end
			kw[:I] = flags                          # "" is legal: plain admittance, wavenumber in m
			G2 = _gmtread_trb(p2)                   # grids are READ in "TRB" — THE reader
			R = GMT.gravfft(G, G2; kw...)
			return _gravfft_curve(scene, R, occursin('c', flags) ? "Coherence" : "Admittance",
			                      _get(d, "outfile"), flags, field)
		else
			error("unknown gravfft mode '$mode'")
		end

		R = GMT.gravfft(G; kw...)
		return _gm3d_deliver(scene, R, title, _get(d, "outfile"), false,
		                     "gravfft " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "gravfft", e)
		return Cint(0)
	end
end

function _register_gravfft()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_gravfft, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gravfft_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
