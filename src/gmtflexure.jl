# gmtflexure.jl — GMT menu > "gmtflexure": the flexural response of a 2-D (profile) plate to a load,
# by finite differences [Bodine, 1980] — variable rigidity, a choice of boundary conditions at each
# end, a pre-existing deformation, and a restoring force that may depend on the sign of the flexure.
#
# The C++ dialog is GmtFlexureDialog (70_window.cpp, loads deps/ui/gmtflexure_dialog.ui).
#
# GMT.jl has no `gmtflexure` wrapper (nor a `grdflexure` one), so this runs in MONOLITHIC mode —
# `GMT.gmt("gmtflexure …")` — exactly as the talwani dialogs do, and file paths travel inside that
# command string double-quoted (`_gmt_quote_path`, talwani2d.jl). The module's keys are
# "ED(,QD(,TD(,>D}": every input it takes is named by an option, there is no primary input at all,
# and the profile comes back through the implicit virtual output file. No temporary file anywhere.
#
# The 3-D twin of this is grdflexure, which flexes a GRID in the wavenumber domain; this one solves
# a profile in the space domain and is the only one of the two that offers boundary conditions.

# The four boundary conditions the module knows, in its own numbering.
const _GF2_BC = ("0", "1", "2", "3")     # infinity, periodic, clamped, free

# "10", "10k", "1e10" — a plain number with an optional trailing k (kilometres). The module's -E, -W
# and -Z all read their argument that way, and say so: -M does not apply to them.
function _flex_num_k(v::AbstractString)::Bool
	s = endswith(v, "k") ? chop(v) : v
	return !isempty(s) && tryparse(Float64, s) !== nothing
end

# -D<rm>/<rl>[/<ri>]/<rw>: mantle, load, infill and water densities. The infill defaults to the load
# density, which is why it is the one box that may be left empty.
function _gf2_D(d::Dict{String,String})::String
	m, l, i, w = _get(d, "rhom"), _get(d, "rhol"), _get(d, "rhoi"), _get(d, "rhow")
	(isempty(m) || isempty(l) || isempty(w)) &&
		error("give the mantle, load and water densities")
	for (v, what) in ((m, "mantle density"), (l, "load density"), (w, "water density"))
		(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
	end
	isempty(i) && return m * "/" * l * "/" * w
	(tryparse(Float64, i) === nothing) && error("the infill density must be a number, not '$i'")
	return m * "/" * l * "/" * i * "/" * w
end

# -E: an elastic thickness in metres (append k for km), a flexural rigidity (anything above 1e10 is
# read as one), or a FILE of variable thickness/rigidity — which must be co-registered with -Q's.
# Returns the option value and whether it turned out to be a file, because -Qn leans on that.
function _gf2_E(d::Dict{String,String})
	v = _get(d, "te")
	isempty(v) && error("give the elastic plate thickness")
	_flex_num_k(v) && return v, false
	isfile(v) || error("elastic thickness file not found: $v")
	return _gmt_quote_path(v), true
end

# -Q: where the load comes from, in the module's own three forms — no load at all (n), loads in Pa
# (q) or topography (t). With -Qn and no variable-rigidity file there is nothing that says WHERE to
# compute, so the module wants an array specification instead; that is the one case the range boxes
# are for.
function _gf2_Q(d::Dict{String,String}, teIsFile::Bool)::String
	mode = _get(d, "qmode", "t")
	if mode == "n"
		lo, hi, inc = _get(d, "qmin"), _get(d, "qmax"), _get(d, "qinc")
		if isempty(lo) && isempty(hi) && isempty(inc)
			teIsFile ||
				error("with no load and no variable-rigidity file, give the x range and step to compute on")
			return "n"
		end
		(isempty(lo) || isempty(hi) || isempty(inc)) &&
			error("the x range needs its first x, its last x and the step")
		for (v, what) in ((lo, "first x"), (hi, "last x"), (inc, "step"))
			(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
		end
		(parse(Float64, lo) < parse(Float64, hi)) || error("the first x must be smaller than the last one")
		(parse(Float64, inc) > 0) || error("the step must be positive")
		return "n" * lo * "/" * hi * "/" * inc
	end
	(mode in ("q", "t")) || error("unknown load kind '$mode'")
	f = _get(d, "loadfile")
	isempty(f) && error(mode == "q" ? "choose the file of loads (x, load in Pa)" :
	                                  "choose the file of topography (x, height)")
	isfile(f) || error("load file not found: $f")
	return mode * _gmt_quote_path(f)
end

# -A[l|r]<bc>[/<args>]: one boundary condition per end. Only two of the four carry an argument — the
# clamped one takes a deflection, the free one a moment/force pair — and giving one to the other two
# is the mistake this refuses.
function _gf2_A(d::Dict{String,String}, side::AbstractString)::String
	bc = _get(d, side * "bc")
	isempty(bc) && return ""
	(bc in _GF2_BC) || error("a boundary condition is 0, 1, 2 or 3, not '$bc'")
	a = _get(d, side * "args")
	isempty(a) && return side * bc
	if bc == "2"
		(tryparse(Float64, a) === nothing) &&
			error("a clamped boundary takes one deflection value, not '$a'")
	elseif bc == "3"
		p = split(a, '/')
		(length(p) == 2 && all(x -> tryparse(Float64, x) !== nothing, p)) ||
			error("a free boundary takes moment/force, not '$a'")
	else
		error("the infinity and periodic boundary conditions take no value")
	end
	return side * bc * "/" * a
end

# What comes back: the profile, its deflection, and — only when -S was asked for — the curvature.
function _gf2_colnames(vkm::Bool, ncol::Int)::Vector{String}
	ncol <= 0 && return String[]
	names = ["x", "Deflection (" * (vkm ? "km" : "m") * ")", "Curvature"]
	ncol <= 3 && return names[1:ncol]
	append!(names, ["column $k" for k in 4:ncol])
	return names
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGmtFlexureFn. Returns Cint 1 on success, 0 on failure.
function _on_gmtflexure(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	tmpout = ""
	try
		d = _nswing_parse(unsafe_string(cparams))
		E, teIsFile = _gf2_E(d)
		opts = String["-D" * _gf2_D(d), "-E" * E, "-Q" * _gf2_Q(d, teIsFile)]

		for side in ("l", "r")
			s = _gf2_A(d, side)
			isempty(s) || push!(opts, "-A" * s)
		end
		# -C is given TWICE when both constants are changed: -Cp<nu> and -Cy<E>, never "-Cpy".
		for (key, letter, what) in (("poisson", "p", "Poisson's ratio"), ("young", "y", "Young's modulus"))
			v = _get(d, key)
			isempty(v) && continue
			(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
			push!(opts, "-C" * letter * v)
		end
		fo = _get(d, "force")
		if !isempty(fo)
			(tryparse(Float64, fo) === nothing) &&
				error("the in-plane force must be a number (Pa m), not '$fo'")
			push!(opts, "-F" * fo)
		end
		_on(d, "varrestore") && push!(opts, "-L")     # restoring force follows the sign of the flexure
		M = _tal_M(d);  isempty(M) || push!(opts, "-M" * M)
		_on(d, "curvature") && push!(opts, "-S")      # curvature in a third column
		wf = _get(d, "wfile")
		if !isempty(wf)
			isfile(wf) || error("pre-existing deformation file not found: $wf")
			push!(opts, "-T" * _gmt_quote_path(wf))
		end
		# -W and -Z are distances in metres unless they carry a k, and both must be positive: the
		# module says so and refuses otherwise, so the dialog does it first.
		for (key, letter, what) in (("water", "W", "water depth"), ("zobs", "Z", "observation level"))
			v = _get(d, key)
			isempty(v) && continue
			_flex_num_k(v) || error("the $what is a number, optionally with k for km, not '$v'")
			(parse(Float64, endswith(v, "k") ? chop(v) : v) > 0) ||
				error("the $what must be positive, not '$v'")
			push!(opts, "-" * letter * v)
		end

		# The table CANNOT come back through memory: asking GMT.jl's monolithic entry for this module's
		# output kills the process with an access violation inside its own `get_dataset` (verified on
		# GMT 6.7, with and without a dummy primary input to satisfy the module's declared input key).
		# The module writes its profile to stdout, so the run is REDIRECTED to a real file and the file
		# is read back — the same answer talwani3d's -G already needs, for the same kind of reason. When
		# the dialog named an output file that IS the destination, so nothing is written twice.
		out = _get(d, "outfile")
		dest = out
		if isempty(dest)
			tmpout = tempname() * ".txt"
			dest = tmpout
		end
		GMT.gmt("gmtflexure " * join(opts, ' ') * " > " * _gmt_quote_path(dest))
		isfile(dest) || error("gmtflexure wrote no output")
		R = GMT.gmtread(String(dest); dataset = true)
		R === nothing && error("gmtflexure returned nothing")
		Ds = isa(R, Vector) ? R : [R]
		isempty(Ds) && error("gmtflexure returned no rows")
		all(x -> isa(x, GMTdataset), Ds) || error("got a $(typeof(R)), not a table")
		vkm = _on(d, "vkm")
		for Dd in Ds
			Dd.colnames = _gf2_colnames(vkm, size(Dd.data, 2))
		end

		show_table(scene, length(Ds) == 1 ? Ds[1] : Ds; name = "gmtflexure")

		# A flexure profile is a shape — a downward moat and its flanking bulge — so it goes to the
		# X,Y plot tool, with the curvature on the same axes when it was asked for.
		if _on(d, "plot")
			p = nothing
			for (k, Dd) in enumerate(Ds)
				m = Dd.data
				(size(m, 1) > 1 && size(m, 2) >= 2) || continue
				names = Dd.colnames
				x = Float64.(@view m[:, 1])
				nm = length(Ds) == 1 ? names[2] : names[2] * " #" * string(k)
				if p === nothing
					p = xyplot(x, Float64.(@view m[:, 2]); name = nm, title = "gmtflexure",
					           xlabel = names[1], ylabel = names[2])
				else
					add!(p, x, Float64.(@view m[:, 2]); name = nm)
				end
				if size(m, 2) >= 3
					add!(p, x, Float64.(@view m[:, 3]);
					     name = length(Ds) == 1 ? names[3] : names[3] * " #" * string(k),
					     linestyle = :dash)
				end
			end
		end
		return Cint(1)
	catch e
		_tool_failed(scene, "gmtflexure", e)
		return Cint(0)
	finally
		if !isempty(tmpout)
			try; rm(tmpout; force = true); catch; end
		end
	end
end

function _register_gmtflexure()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_gmtflexure, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gmtflexure_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
