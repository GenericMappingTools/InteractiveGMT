# grdblend.jl — GMT menu > "grdblend": blend several grids into one, through GMT.jl's own `grdblend`.
#
# The C++ dialog is GrdBlendDialog (70_window.cpp, loads deps/ui/grdblend_dialog.ui). Its grids come
# either from a blend file or from the dialog's list, which the user fills by picking, on a map, the
# rectangles of the grids found in a directory. Two callbacks (contract in 30_app.cpp):
#   _on_grdblend_headers  the header of every path, for the map (one grdinfo -C call for all of them)
#   _on_grdblend          the Compute
# Wired on first use (warm_register), never at start-up.

const _GRDBLEND_BUF = Ref{Vector{UInt8}}(UInt8[0])
const _GRDBLEND_WIRED = Ref(false)

_gb_quote(p::AbstractString) = "\"" * p * "\""

# "w e s n dx dy nx ny reg geog" per path, "" for a path that is not a readable grid. One grdinfo
# for the whole set; when that fails (one bad file sinks it), each path on its own.
function _grdblend_headers(paths::Vector{String})::Vector{String}
	row(v) = join((v[1], v[2], v[3], v[4], v[7], v[8], Int(v[9]), Int(v[10]), Int(v[11]), Int(v[12])), ' ')
	out = fill("", length(paths))
	ks = findall(isfile, paths)                  # a missing path never reaches GMT
	isempty(ks) && return out
	try
		I = GMT.grdinfo(join(_gb_quote.(paths[ks]), ' '); C = :n)
		if size(I.data, 1) == length(ks) && size(I.data, 2) >= 12
			for (j, k) in enumerate(ks)
				out[k] = row(I.data[j, :])
			end
			return out
		end
	catch
	end
	for k in ks
		p = paths[k]
		try
			I = GMT.grdinfo(_gb_quote(p); C = :n)
			size(I.data, 2) >= 12 && (out[k] = row(I.data[1, :]))
		catch                                    # not a grid: no box for it, and no error either
		end
	end
	return out
end

function _on_grdblend_headers(cpaths::Cstring)::Cstring
	s = ""
	try
		paths = String.(filter(!isempty, split(unsafe_string(cpaths), '\n')))
		s = join(_grdblend_headers(paths), '\n')
	catch e
		@tool_error "grdblend: reading the grid headers FAILED" exception = (e,)
	end
	_GRDBLEND_BUF[] = Vector{UInt8}(codeunits(s * "\0"))
	return Cstring(pointer(_GRDBLEND_BUF[]))
end

# A grid GMT's grdblend reads straight from disk (netCDF or native binary). Anything else it first
# reformats through a temp file, which is broken for more than one such input (see _on_grdblend).
_grdblend_byname(f::AbstractString)::Bool = lowercase(splitext(f)[2]) in (".nc", ".grd", ".nc4", ".cdf")

# The output spacing as numbers: "dx" or "dx/dy", with GMT's arc-minute/second suffixes.
function _grdblend_inc(inc::AbstractString)::NTuple{2,Float64}
	one(s) = begin
		s = strip(s)
		m = endswith(s, 'm') ? 60.0 : endswith(s, 's') ? 3600.0 : 1.0
		v = tryparse(Float64, m == 1.0 ? s : s[1:end-1])
		v === nothing && error("give the grid spacing in the grids' own units (not \"$inc\")")
		v / m
	end
	p = split(inc, '/')
	dx = one(p[1])
	return (dx, length(p) > 1 ? one(p[2]) : dx)
end

# Does this input already sit on the output lattice: same registration, same spacing, in phase?
# Only then can grdblend take it as it is — its OWN resampling of the others is broken (see below).
function _grdblend_on_lattice(w, s, dx, dy, pix::Bool, W, S, DX, DY, PIX::Bool)::Bool
	inphase(v, o, d) = (r = (v - o) / d; abs(r - round(r)) < 1e-6)
	pix == PIX && isapprox(dx, DX; rtol = 1e-9) && isapprox(dy, DY; rtol = 1e-9) &&
		inphase(w, W, DX) && inphase(s, S, DY)
end

# Every input read and, when it is not on the output lattice, resampled to it with grdsample over
# its own extent snapped to that lattice — what grdblend means to do itself. GMT's grdblend does that
# step (and the netCDF reformat of a TIFF) through temp files that come back garbled on Windows, so
# its inputs are handed over in memory and already on the lattice: it then resamples nothing. A grid
# with no lattice node inside the region is left out.
function _grdblend_prepare(files::Vector{String}, region::AbstractString, inc::AbstractString,
                           pixel::Bool, interp::AbstractString)::Vector{GMTgrid}
	W, E, S, N = parse.(Float64, split(region, '/')[1:4])
	DX, DY = _grdblend_inc(inc)
	Gs = GMTgrid[]
	for f in files
		G = GMT.gmtread(f; grd = true)
		gw, ge, gs, gn = G.range[1:4]
		if !_grdblend_on_lattice(gw, gs, G.inc[1], G.inc[2], G.registration == 1, W, S, DX, DY, pixel)
			w = W + ceil((max(gw, W) - W) / DX - 1e-6) * DX;  e = W + floor((min(ge, E) - W) / DX + 1e-6) * DX
			s = S + ceil((max(gs, S) - S) / DY - 1e-6) * DY;  n = S + floor((min(gn, N) - S) / DY + 1e-6) * DY
			(e > w && n > s) || continue
			# registration ALWAYS stated: grdsample keeps the input's own otherwise
			kw = Dict{Symbol,Any}(:R => (w, e, s, n), :I => (DX, DY), :r => (pixel ? "p" : "g"))
			isempty(interp) || (kw[:n] = interp)
			srs = G.proj4;  wkt = G.wkt
			G = GMT.grdsample(G; kw...)
			isempty(G.proj4) && (G.proj4 = srs);  isempty(G.wkt) && (G.wkt = wkt)
		end
		push!(Gs, G)
	end
	return Gs
end

# All inputs readable by name AND already on the output lattice: grdblend can be given the files.
function _grdblend_direct(files::Vector{String}, region::AbstractString, inc::AbstractString, pixel::Bool)::Bool
	all(_grdblend_byname, files) || return false
	W, _, S, _ = parse.(Float64, split(region, '/')[1:4])
	DX, DY = _grdblend_inc(inc)
	for h in _grdblend_headers(files)
		v = tryparse.(Float64, split(h))
		(length(v) < 9 || any(isnothing, v)) && return false
		_grdblend_on_lattice(v[1], v[3], v[5], v[6], v[9] == 1, W, S, DX, DY, pixel) || return false
	end
	return true
end

# The options of a grdblend run as a command string, for the in-memory call (the same keys
# _on_grdblend gives GMT.grdblend).
function _grdblend_opts(kw::Dict{Symbol,Any})::String
	o = String[]
	for (k, f) in ((:R, "-R"), (:I, "-I"), (:C, "-C"), (:Z, "-Z"), (:N, "-N"), (:n, "-n"), (:f, "-f"))
		haskey(kw, k) && push!(o, f * string(kw[k]))
	end
	haskey(kw, :r) && push!(o, "-r")
	haskey(kw, :W) && push!(o, kw[:W] === true ? "-W" : "-W" * string(kw[:W]))
	haskey(kw, :V) && push!(o, "-V")
	return join(o, ' ')
end

# The grids a blend file names: the first field of every record ("file [-R|-] [weight]").
function _grdblend_job_files(bf::AbstractString)::Vector{String}
	fs = String[]
	for ln in eachline(bf)
		s = strip(ln)
		(isempty(s) || startswith(s, '#')) && continue
		push!(fs, String(first(split(s))))
	end
	return fs
end

# The referencing system of the blend's inputs, read off their headers (no data read). GMT's grdblend
# hands back a grid with NONE (its inputs go through grdconvert/grdsample to netCDF first), so the
# result is stamped with this. Referenced inputs must all share one: a blend across two systems
# would put the same numbers on different ground. An input with NO referencing is taken along only
# when the others are lon/lat and its own limits look like lon/lat (GMT.guessgeog's range test, on
# the header's limits); next to projected grids nothing tells what system it is in, so it is refused.
function _grdblend_srs(files::Vector{String})::String
	srs = "";  from = "";  bare = String[]
	for f in files
		s = try GMT.getproj(f) catch; "" end
		if isempty(s)
			push!(bare, f)
		elseif isempty(srs)
			srs = s;  from = f
		elseif !_same_srs(srs, s)
			error("the grids are in different referencing systems ($(basename(from)) and $(basename(f))): " *
			      "bring them to one system before blending")
		end
	end
	(isempty(srs) || isempty(bare)) && return srs
	_project_is_geog(srs) || error("$(basename(bare[1])) carries no referencing system and the others " *
	                               "are projected ($(basename(from))): give it one before blending")
	for (f, h) in zip(bare, _grdblend_headers(bare))
		v = tryparse.(Float64, split(h))
		lonlat = length(v) >= 4 && !any(isnothing, v[1:4]) &&
		         v[1] >= -180 && v[2] <= 360 && v[3] >= -90 && v[4] <= 90   # GMT.guessgeog's test
		lonlat || error("$(basename(f)) carries no referencing system and its limits are not lon/lat, " *
		                "while the others are geographic ($(basename(from)))")
	end
	return srs
end

function _on_grdblend(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The input: the blend file when one is given, else the list (grdblend wants two or more).
		bf = _get(d, "blendfile")
		files = String.(filter(!isempty, strip.(split(_get(d, "files"), '\t'))))
		if !isempty(bf)
			isfile(bf) || error("blend file not found: $bf")
			src = _gb_quote(bf)
		else
			length(files) < 2 && error("put at least two grids on the list (or give a blend file)")
			for f in files
				isfile(f) || error("grid not found: $f")
			end
			src = join(_gb_quote.(files), ' ')
		end

		kw = Dict{Symbol,Any}()
		reg = _get(d, "region");  inc = _get(d, "inc")
		(isempty(reg) || occursin("//", reg) || endswith(reg, "/") || startswith(reg, "/")) &&
			error("give a region (Min/Max in both directions)")
		isempty(inc) && error("give a grid spacing")
		kw[:R] = reg
		kw[:I] = inc
		_on(d, "pixel") && (kw[:r] = true)
		mode = _get(d, "mode")
		if !isempty(mode)
			mode in ("f", "l", "o", "u") || error("unknown clobber mode \"$mode\"")
			sg = _get(d, "sign")
			sg in ("", "n", "p") || error("unknown clobber modifier \"$sg\"")
			kw[:C] = isempty(sg) ? mode : mode * "+" * sg
		end
		w = _get(d, "weights")
		if !isempty(w)
			w in ("w", "z") || error("unknown weights output \"$w\"")
			kw[:W] = w == "z" ? "z" : true
		end
		sc = _get(d, "scale")
		if !isempty(sc)
			v = tryparse(Float64, sc)
			v === nothing && error("the scale must be a number, not \"$sc\"")
			kw[:Z] = v
		end
		nd = _get(d, "nodata")
		if !isempty(nd)
			tryparse(Float64, nd) === nothing && error("the no-data value must be a number, not \"$nd\"")
			kw[:N] = nd                          # -N<val> is grdblend's own name for -di<val>
		end
		ip = _get(d, "interp")
		if !isempty(ip)
			ip in ("b", "c", "l", "n") || error("unknown interpolation \"$ip\"")
			kw[:n] = ip
		end
		geog = _on(d, "geog")
		geog && (kw[:f] = "g")
		_on(d, "verbose") && (kw[:V] = true)

		srs = _grdblend_srs(isempty(bf) ? files : _grdblend_job_files(bf))

		R = if isempty(bf) && !_grdblend_direct(files, reg, inc, _on(d, "pixel"))
			# GMT's grdblend reformats a non-netCDF input, and resamples one off the output lattice,
			# through temp files that come back garbled on Windows (the reformat uses ONE fixed temp
			# name for every input). So the inputs go over in memory, in the list's order, already on
			# the lattice: grdblend then has nothing to reformat or resample.
			Gs = _grdblend_prepare(files, reg, inc, _on(d, "pixel"), ip)
			length(Gs) < 2 && error("fewer than two of the grids reach into the region")
			GMT.gmt("grdblend " * _grdblend_opts(kw), Gs...)
		else
			# a blend file (its inner regions and weights exist only in that form) must not lead
			# grdblend into those broken steps: refuse rather than return a garbled grid
			if !isempty(bf)
				jf = _grdblend_job_files(bf)
				_grdblend_direct(jf, reg, inc, _on(d, "pixel")) ||
					error("the blend file names grids that are not netCDF, or not on the output grid's " *
					      "spacing/registration: GMT's grdblend garbles those. Use the list of grids " *
					      "instead (it handles them), or match them to the output first")
			end
			GMT.grdblend(src; kw...)
		end
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")
		# put back the inputs' referencing system, which GMT drops (before the save and the display)
		if !isempty(srs) && isempty(R.proj4) && isempty(R.wkt) && R.epsg == 0
			if startswith(srs, '+')
				R.proj4 = srs
			else
				R.wkt = srs
				R.proj4 = try GMT.wkt2proj(srs) catch; "" end
			end
		end

		title = isempty(w) ? "Blended grid" : "Blend weights"
		return _gm3d_deliver(scene, R, title, _get(d, "outfile"), false,
		                     "grdblend " * src * " " * join(("$k=$v" for (k, v) in kw), ' ');
		                     geographic = geog ? true : nothing)
	catch e
		_tool_failed(scene, "grdblend", e)
		return Cint(0)
	end
end

function _register_grdblend()
	_GRDBLEND_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_grdblend_callbacks) || return   # a DLL built without the dialog
	h = @cfunction((c) -> Base.invokelatest(_on_grdblend_headers, c), Cstring, (Cstring,))
	r = @cfunction((s, c) -> Base.invokelatest(_on_grdblend, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdblend_callbacks), Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}), h, r)
	_GRDBLEND_WIRED[] = true
	return
end
warm_register("grdblend", _register_grdblend)
