# grdfft.jl — GMT menu > "grdfft": take the 2-D FFT of the window's grid, do one or more operations
# in the frequency domain and transform back — or, instead of a grid, estimate the power spectrum.
# All of it through GMT.jl's own `grdfft`; no maths lives here.
#
# The C++ dialog is GrdFFTDialog (70_window.cpp, loads deps/ui/grdfft_dialog.ui). Its two modes are
# the module's own two outcomes:
#   grid      the transformed grid   (-A -C -D -I -S -Q, a filter -F, and the -N FFT block)
#   spectrum  a 1-D power spectrum   (-E; with a second grid the 17-column cross-spectrum)
# The whole -F string is assembled by the DIALOG (it is the side that knows which knob belongs to
# which filter, exactly as in grdfilter) and travels verbatim; this file does not second-guess it.

# -N: the FFT dimension directive plus the detrend / extension / taper modifiers, in the order the
# manual lists them. EMPTY = don't pass -N at all, so the module's own defaults stay in charge.
function _grdfft_N(d::Dict{String,String})::String
	s = _get(d, "fftdim") * _get(d, "detrend") * _get(d, "extend")
	tw = _get(d, "taper");  isempty(tw) || (s *= "+t" * tw)
	_on(d, "fftverbose") && (s *= "+v")
	return s
end

# -D and -I are flags that OPTIONALLY carry a scale (or the letter g: geoid <-> gravity). The dialog
# sends the tick and the box separately, because "no scale" and "not asked for" are different things
# and one string cannot say both.
function _grdfft_flagval(d::Dict{String,String}, key::AbstractString)
	_on(d, key) || return nothing
	v = _get(d, key * "_val")
	isempty(v) && return true                       # the bare -D / -I
	(v == "g" || tryparse(Float64, v) !== nothing) ||
		error("$key takes a scale or the letter g (geoid <-> gravity), not '$v'")
	return v
end

# Column names of the spectrum table, so the Data Viewer and the plot read as physics instead of
# "col 7". One grid gives 3 columns, two grids the 17-column cross-spectrum — the module's own
# order: frequency, then 8 quantities each followed by its 1-sigma error.
function _grdfft_spec_colnames(ncol::Int, xlab::AbstractString)::Vector{String}
	ncol <= 3 && return [xlab, "Power", "1-sigma"][1:ncol]
	names = [xlab]
	for q in ("X power", "Y power", "Coherent power", "Noise power", "Phase",
	          "Admittance", "Gain", "Coherency")
		push!(names, q);  push!(names, q * " error")
	end
	return names[1:min(ncol, length(names))]
end

# A spectrum is not a surface: it goes to the window's Data Viewer tab (all of it, errors included)
# AND to the X,Y plot tool, which is where a spectrum is actually read. The plotted quantity is the
# power for a single grid and the coherency for a cross-spectrum — the one everybody looks at first;
# every other column is one click away in the Data Viewer.
function _grdfft_curve(scene::Ptr{Cvoid}, R, title::String, outfile::AbstractString,
                       espec::AbstractString)::Cint
	D = isa(R, Vector) ? (isempty(R) ? error("grdfft returned no spectrum") : R[1]) : R
	isa(D, GMTdataset) || error("got a $(typeof(D)), not a table")
	m = D.data
	size(m, 2) < 2 && error("grdfft returned $(size(m, 2)) column(s), expected at least 2")

	xlab = occursin("+w", espec) ? (occursin("+wk", espec) ? "Wavelength (km)" : "Wavelength (m)") :
	                               "Frequency (1/m)"
	names = _grdfft_spec_colnames(size(m, 2), xlab)
	D.colnames = names
	isempty(outfile) || GMT.gmtwrite(String(outfile), D)
	show_table(scene, D; name = title)

	# 3 columns: power + its error. 17: the cross-spectrum, whose headline curve is the coherency
	# (column 16, its error in 17).
	col = size(m, 2) >= 16 ? 16 : 2
	ylab = names[col]
	x = Float64.(@view m[:, 1])
	y = Float64.(@view m[:, col])
	p = xyplot(x, y; name = ylab, title = title, xlabel = xlab, ylabel = ylab)
	if size(m, 2) > col
		e = Float64.(@view m[:, col + 1])
		add!(p, x, y .+ e; name = "+1 sigma", linestyle = :dash)
		add!(p, x, y .- e; name = "-1 sigma", linestyle = :dash)
	end
	return Cint(1)
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdFFTFn. An absent key = don't pass that option, so the module's own defaults
# stay in charge. Returns Cint 1 on success, 0 on failure.
function _on_grdfft(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The grid to transform is the one ON DISPLAY (the dialog sends the active layer's label),
		# not whatever happens to be the window's base grid.
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		kw = Dict{Symbol,Any}()
		nfft = _grdfft_N(d);  isempty(nfft) || (kw[:N] = nfft)
		_on(d, "geog") && (kw[:f] = :g)      # degrees -> meters, the module's Flat Earth approximation
		mg = _get(d, "mgal45");  isempty(mg) || (kw[:M] = mg)

		if _get(d, "mode", "grid") == "spectrum"
			espec = _get(d, "espec", "r")
			kw[:E] = espec
			p2 = _get(d, "grid2")
			R = if isempty(p2)
				GMT.grdfft(G; kw...)
			else
				isfile(p2) || error("the cross-spectrum needs a second grid: '$p2' not found")
				GMT.grdfft(G, _gmtread_trb(p2); kw...)   # grids are READ in "TRB" — THE reader
			end
			return _grdfft_curve(scene, R, isempty(p2) ? "Power spectrum" : "Cross-spectrum",
			                     _get(d, "outfile"), espec)
		end

		# ---- Grid mode: any combination of the frequency-domain operators, plus a filter.
		what = String[]
		az = _get(d, "azim")
		if !isempty(az)
			kw[:A] = az;  push!(what, "d/d($(az)°)")
		end
		up = _get(d, "upward")
		if !isempty(up)
			z = tryparse(Float64, up)
			z === nothing && error("the continuation level must be a number of meters, not '$up'")
			kw[:C] = up;  push!(what, z >= 0 ? "up $(up) m" : "down $(abs(z)) m")
		end
		dv = _grdfft_flagval(d, "dfdz")
		dv === nothing || (kw[:D] = dv;  push!(what, "d/dz"))
		iv = _grdfft_flagval(d, "integrate")
		iv === nothing || (kw[:I] = iv;  push!(what, "integral dz"))
		filt = _get(d, "filter")
		isempty(filt) || (kw[:F] = filt;  push!(what, "filter $filt"))
		sc = _get(d, "scale")
		isempty(sc) || (kw[:S] = sc)
		if _on(d, "noop")
			# -Q is the "do NOTHING in the frequency domain" switch — it exists to write out the
			# intermediate products -N asks for, so it cannot be combined with an operator.
			isempty(what) || error("-Q (no wavenumber operation) cannot be combined with an operation")
			kw[:Q] = true;  push!(what, "no operation")
		end
		isempty(what) && error("nothing to do — choose an operation, a filter, or -Q")

		R = GMT.grdfft(G; kw...)
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")
		return _gm3d_deliver(scene, R, "grdfft (" * join(what, ", ") * ")", _get(d, "outfile"), false,
		                     "grdfft " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_tool_failed(scene, "grdfft", e)
		return Cint(0)
	end
end

function _register_grdfft()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdfft, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdfft_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
