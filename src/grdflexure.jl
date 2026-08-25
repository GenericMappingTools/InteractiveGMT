# grdflexure.jl — GMT menu > "grdflexure": the flexural deformation of a 3-D surface under a
# topographic load, in the wavenumber domain, for the module's five rheologies — an elastic plate
# over an inviscid half-space, over a viscous one, over a viscous layer on a viscous half-space, a
# viscoelastic (Maxwell) plate, and the general linear model with an initial and a final thickness.
#
# The C++ dialog is GrdFlexureDialog (70_window.cpp, loads deps/ui/grdflexure_dialog.ui).
#
# MONOLITHIC mode, like its 2-D twin gmtflexure: GMT.jl wraps neither module. Paths inside the
# command string are double-quoted (`_gmt_quote_path`, talwani2d.jl) and the FFT settings are built
# by `_grdfft_N` (grdfft.jl) — it is the same -N option, so it is the same builder, not a copy.
#
# A RUN HAS THREE SHAPES, and they are the module's, not this file's invention:
#
#   no -T          one instantaneous (elastic) solution. -G comes back through memory and the grid
#                  lands in the window.
#   with -T        the module writes ONE GRID PER EVALUATION TIME to a filename TEMPLATE — it
#                  refuses a -G without a % in it — so nothing can come back through memory. -L is
#                  passed alongside to learn exactly which files were written; the last time's grid
#                  is loaded into the window and the rest are named in the Errors console. Note that
#                  -F (firmoviscous) and -M (viscoelastic) BOTH require -T, so every time-dependent
#                  rheology takes this path.
#   -Q             no flexure at all: the chosen transfer function is written for seven elastic
#                  thicknesses, to seven fixed file names in the working directory. See below.

# What -Q writes: one file per elastic thickness (0 km when the response is purely viscous), each
# with wavelength (km), wavenumber (1/m) and then the transfer function — once if the response is
# elastic, or at these twelve times if -F or -M made it time-dependent.
const _GFLX_TRANSFER_TE = (0, 1, 2, 5, 10, 20, 50, 100)
const _GFLX_TRANSFER_T = ("1 kyr", "2 kyr", "5 kyr", "10 kyr", "20 kyr", "50 kyr", "100 kyr",
                          "200 kyr", "500 kyr", "1 Myr", "2 Myr", "5 Myr")

_gflx_transfer_name(te::Integer) = "grdflexure_transfer_function_te_" * lpad(string(te), 3, '0') * "_km.txt"

# A time: a number with an optional k (kyr) or M (Myr). Years otherwise, which is the module's default.
function _flex_time(v::AbstractString)::Bool
	s = (!isempty(v) && last(v) in ('k', 'M')) ? chop(v) : v
	return !isempty(s) && tryparse(Float64, s) !== nothing
end

# -D<rm>/<rl>[/<ri>]/<rw>[+r<rr>]. With a variable load-density grid (-H) the module insists the
# fixed load density be given as a dash, so the dialog greys that box out and this puts the dash in.
function _gflx_D(d::Dict{String,String}, haveH::Bool)::String
	m, i, w = _get(d, "rhom"), _get(d, "rhoi"), _get(d, "rhow")
	(isempty(m) || isempty(w)) && error("give the mantle and water densities")
	l = "-"
	if !haveH
		l = _get(d, "rhol")
		isempty(l) && error("give the load density, or a grid of load densities")
		(tryparse(Float64, l) === nothing) && error("the load density must be a number, not '$l'")
	end
	for (v, what) in ((m, "mantle density"), (w, "water density"))
		(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
	end
	s = m * "/" * l
	if !isempty(i)
		(tryparse(Float64, i) === nothing) && error("the infill density must be a number, not '$i'")
		s *= "/" * i
	end
	s *= "/" * w
	r = _get(d, "rhoroot")
	if !isempty(r)
		(tryparse(Float64, r) === nothing) && error("the root density must be a number, not '$r'")
		s *= "+r" * r
	end
	return s
end

# -E[<Te>[k][/<Te2>[k]]]: one thickness, two for the general linear viscoelastic model, or none at
# all — which means no plate and a purely viscous response, and only makes sense with -F.
# Returns the option value and whether two thicknesses were given.
function _gflx_E(d::Dict{String,String})
	te, te2 = _get(d, "te"), _get(d, "te2")
	if isempty(te)
		isempty(te2) ||
			error("a general linear model needs BOTH the initial and the final plate thickness")
		return "", false
	end
	_flex_num_k(te) ||
		error("the elastic thickness is a number, optionally with k for km, not '$te'")
	isempty(te2) && return te, false
	_flex_num_k(te2) ||
		error("the final elastic thickness is a number, optionally with k for km, not '$te2'")
	return te * "/" * te2, true
end

# -F<nu_a>[/<h_a>[k]/<nu_m>]: one viscosity for an elastic plate over a viscous half-space, or all
# three for the two-layer version. Two of the three is not a model the module knows.
function _gflx_F(d::Dict{String,String})::String
	na = _get(d, "nua")
	isempty(na) && return ""
	(tryparse(Float64, na) === nothing) &&
		error("the asthenosphere viscosity must be a number in Pa s, not '$na'")
	ha, nm = _get(d, "ha"), _get(d, "num")
	(isempty(ha) != isempty(nm)) &&
		error("a two-layer firmoviscous model needs BOTH the asthenosphere thickness and the lower-mantle viscosity")
	isempty(ha) && return na
	_flex_num_k(ha) ||
		error("the asthenosphere thickness is a number, optionally with k for km, not '$ha'")
	(tryparse(Float64, nm) === nothing) &&
		error("the lower-mantle viscosity must be a number in Pa s, not '$nm'")
	return na * "/" * ha * "/" * nm
end

# -A<Nx>/<Ny>/<Nxy>: the in-plane force triple, all three or none — compression negative.
function _gflx_A(d::Dict{String,String})::String
	nx, ny, nxy = _get(d, "nx"), _get(d, "ny"), _get(d, "nxy")
	(isempty(nx) && isempty(ny) && isempty(nxy)) && return ""
	(isempty(nx) || isempty(ny) || isempty(nxy)) &&
		error("in-plane forces are given as all three of Nx, Ny and Nxy")
	for (v, what) in ((nx, "Nx"), (ny, "Ny"), (nxy, "Nxy"))
		(tryparse(Float64, v) === nothing) && error("$what must be a number in Pa m, not '$v'")
	end
	return nx * "/" * ny * "/" * nxy
end

# -T<t0>[/<t1>/<dt>[+l]] or a file of times.
function _gflx_T(d::Dict{String,String})::String
	f = _get(d, "tfile")
	if !isempty(f)
		isfile(f) || error("times file not found: $f")
		return _gmt_quote_path(f)
	end
	t0 = _get(d, "t0")
	isempty(t0) && return ""
	_flex_time(t0) || error("a time is a number, optionally with k for kyr or M for Myr, not '$t0'")
	t1, dt = _get(d, "t1"), _get(d, "dt")
	(isempty(t1) != isempty(dt)) &&
		error("a sequence of times needs both the last time and the step (or the number of steps)")
	isempty(t1) && return t0
	_flex_time(t1) || error("a time is a number, optionally with k for kyr or M for Myr, not '$t1'")
	# The step carries the same k/M units the two times do (and with +l it is a COUNT of steps, a
	# plain number) — so it is checked by the same reader, not by a bare parse that would reject "1M".
	_flex_time(dt) ||
		error("the time step is a number, optionally with k for kyr or M for Myr, not '$dt'")
	return t0 * "/" * t1 * "/" * dt * (_on(d, "tlog") ? "+l" : "")
end

# The -L list the module writes when -T is used: one record of "time filename timetag" per grid.
# Parsed here rather than through gmtread because the useful column is the FILE NAME, which is text.
function _gflx_read_list(path::AbstractString)
	out = Tuple{Float64,String,String}[]
	for line in eachline(String(path))
		s = strip(line)
		(isempty(s) || s[1] == '#' || s[1] == '>') && continue
		f = split(s)
		length(f) >= 2 || continue
		t = tryparse(Float64, f[1])
		t === nothing && continue
		# "time file timetag": the tag is one word at the end, so everything between it and the time
		# is the file name — which is how a name with a space in it survives being read back.
		name = length(f) >= 3 ? join(f[2:end-1], ' ') : String(f[2])
		push!(out, (t, String(name), length(f) >= 3 ? String(f[end]) : ""))
	end
	return out
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdFlexureFn. Returns Cint 1 on success, 0 on failure.
function _on_grdflexure(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	tmpG, tmpL = "", ""
	try
		d = _nswing_parse(unsafe_string(cparams))
		transfer = _on(d, "transfer")
		hgrid = _get(d, "rhogrid")
		haveH = !isempty(hgrid)

		opts = String[]
		E, twoTe = _gflx_E(d)
		F = _gflx_F(d)
		Mt = _get(d, "maxwell")
		if !isempty(Mt)
			_flex_time(Mt) ||
				error("the Maxwell time is a number, optionally with k for kyr or M for Myr, not '$Mt'")
		end
		A = _gflx_A(d)
		T = _gflx_T(d)

		# The module's own incompatibilities, refused here before anything runs.
		(!isempty(F) && !isempty(Mt)) &&
			error("a firmoviscous model (-F) and a viscoelastic one (-M) are alternatives, not a pair")
		(!isempty(A) && !isempty(F)) &&
			error("in-plane forces are not known to work with a firmoviscous model")
		(isempty(E) && isempty(F)) &&
			error("with no plate thickness there is no plate: that only means something with a firmoviscous model")
		if !transfer
			(!isempty(F) && isempty(T)) && error("a firmoviscous model needs the times to evaluate at")
			(!isempty(Mt) && isempty(T)) && error("a viscoelastic model needs the times to evaluate at")
			(twoTe && isempty(Mt)) && error("a general linear model needs a Maxwell time")
		end

		push!(opts, "-D" * _gflx_D(d, haveH), "-E" * E)
		isempty(F)  || push!(opts, "-F" * F)
		isempty(Mt) || push!(opts, "-M" * Mt)
		isempty(A)  || push!(opts, "-A" * A)
		for (key, letter, what) in (("poisson", "p", "Poisson's ratio"), ("young", "y", "Young's modulus"))
			v = _get(d, key)
			isempty(v) && continue
			(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
			push!(opts, "-C" * letter * v)
		end
		beta = _get(d, "beta")
		if !isempty(beta)
			b = tryparse(Float64, beta)
			(b === nothing || !(0 <= b <= 1)) &&
				error("the starved-moat fraction is between 0 and 1, not '$beta'")
			push!(opts, "-S" * beta)
		end
		for (key, letter, what) in (("water", "W", "water depth"), ("zobs", "Z", "observation level"))
			v = _get(d, key)
			isempty(v) && continue
			_flex_num_k(v) || error("the $what is a number, optionally with k for km, not '$v'")
			(parse(Float64, endswith(v, "k") ? chop(v) : v) > 0) ||
				error("the $what must be positive, not '$v'")
			push!(opts, "-" * letter * v)
		end
		nfft = _grdfft_N(d);  isempty(nfft) || push!(opts, "-N" * nfft)   # THE -N builder (grdfft.jl)
		_on(d, "geog") && push!(opts, "-fg")

		# ---- Transfer functions only: no load, no output grid, seven files in the working directory.
		if transfer
			push!(opts, "-Q")
			# The API insists on the primary input grid this module declares even though -Q reads
			# none, and on an output grid it would never write; a throwaway grid satisfies the first
			# and a real (never-written) name satisfies the second.
			tmpG = tempname() * ".grd"
			push!(opts, "-G" * _gmt_quote_path(tmpG))
			dummy = GMT.mat2grid(zeros(Float32, 2, 2); x = [0.0, 1.0], y = [0.0, 1.0])
			t_run = time()
			GMT.gmt("grdflexure " * join(opts, ' '), dummy)

			made = String[]
			p = nothing
			for te in _GFLX_TRANSFER_TE
				f = joinpath(pwd(), _gflx_transfer_name(te))
				(isfile(f) && mtime(f) >= t_run - 5) || continue    # not a leftover from last time
				push!(made, f)
				Dt = GMT.gmtread(f; dataset = true)
				Dt = isa(Dt, Vector) ? (isempty(Dt) ? nothing : Dt[1]) : Dt
				(Dt === nothing || size(Dt.data, 2) < 3) && continue
				m = Dt.data
				# Column 3 is the first (or only) response; with -F or -M there are twelve, one per
				# time, and the curve says which one it is rather than pretending there is only one.
				nm = "Te = $te km" * (size(m, 2) > 3 ? " (t = " * _GFLX_TRANSFER_T[1] * ")" : "")
				x, y = Float64.(@view m[:, 1]), Float64.(@view m[:, 3])
				if p === nothing
					p = xyplot(x, y; name = nm, title = "grdflexure transfer function",
					           xlabel = "Wavelength (km)", ylabel = "Transfer function")
				else
					add!(p, x, y; name = nm)
				end
			end
			isempty(made) && error("grdflexure wrote no transfer-function files")
			_viewer_log_error(scene, "grdflexure -Q wrote " * string(length(made)) *
			                  " transfer-function file(s):\n" * join(made, "\n") *
			                  "\n(a time-dependent response carries twelve columns after the " *
			                  "wavenumber, for " * join(_GFLX_TRANSFER_T, ", ") * "; the plot shows the first.)")
			return Cint(1)
		end

		# ---- Everything else needs the load grid.
		load = _get(d, "loadgrid")
		isempty(load) && error("choose the grid of the topographic load")
		isfile(load) || error("load grid not found: $load")
		loadarg = _gmt_quote_path(_on(d, "loadkm") ? load * "+uk" : load)
		if haveH
			isfile(hgrid) || error("load-density grid not found: $hgrid")
			push!(opts, "-H" * _gmt_quote_path(hgrid))
		end
		out = _get(d, "outfile")

		# ---- A time (or a sequence of them): the module writes FILES, one per evaluation time.
		if !isempty(T)
			isempty(out) &&
				error("with a time the output name is a TEMPLATE the module fills in, e.g. flex_%s.nc — give one")
			occursin('%', out) ||
				error("with a time the output name must be a template containing % (e.g. flex_%s.nc)")
			tmpL = tempname() * ".lis"
			push!(opts, "-T" * T, "-G" * _gmt_quote_path(out), "-L" * _gmt_quote_path(tmpL))
			GMT.gmt("grdflexure " * loadarg * " " * join(opts, ' '))
			isfile(tmpL) || error("grdflexure wrote no grids for those times")
			made = _gflx_read_list(tmpL)
			isempty(made) && error("grdflexure wrote no grids for those times")
			# Informational, through the same Errors console grdseamount's time-step runs report to:
			# it is the one place a window has for saying what a run put on disk.
			_viewer_log_error(scene, "grdflexure wrote " * string(length(made)) * " grid(s):\n" *
			                  join(("  " * m[3] * "  " * m[2] for m in made), "\n"))
			# The LAST evaluation time is the one that lands in the window; the others are on disk
			# and named above. Its own time tag is in the title, so a second time is a second layer
			# rather than a silent overwrite.
			# Picked by the TIME, not by the row: the module writes its -L list newest-first
			# (2M, 1M, 0M for -T0/2M/1M), so `made[end]` would hand over t=0 — the un-relaxed
			# instantaneous response — under a comment promising the last one.
			t, file, tag = made[argmax(first.(made))]
			isfile(file) || (file = joinpath(pwd(), file))
			isfile(file) || error("grdflexure named a grid that is not there: $file")
			G = GMT.gmtread(String(file); grd = true)
			isa(G, GMTgrid) || error("got a $(typeof(G)), not a grid")
			return _gm3d_deliver(scene, G, "Flexure (" * (isempty(tag) ? string(t) : tag) * ")", "",
			                     false, "grdflexure " * join(opts, ' ');
			                     geographic = _on(d, "geog") ? true : nothing)
		end

		# ---- One instantaneous solution: the grid comes back through memory.
		push!(opts, "-G")
		G = GMT.gmt("grdflexure " * loadarg * " " * join(opts, ' '))
		G === nothing && error("grdflexure returned nothing")
		isa(G, GMTgrid) || error("got a $(typeof(G)), not a grid")
		return _gm3d_deliver(scene, G, "Flexure", out, false,
		                     "grdflexure " * join(opts, ' ');
		                     geographic = _on(d, "geog") ? true : nothing)
	catch e
		_viewer_log_error(scene, "grdflexure FAILED: $(sprint(showerror, e))")
		@warn "grdflexure FAILED" exception=(e,)
		return Cint(0)
	finally
		for f in (tmpG, tmpL)
			isempty(f) || (try; rm(f; force = true); catch; end)
		end
	end
end

function _register_grdflexure()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdflexure, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdflexure_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
