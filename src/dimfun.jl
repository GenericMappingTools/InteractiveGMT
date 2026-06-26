# Region-box cross-field recompute for the grdsample dialog — a Julia port of Mirone's
# utils/dim_funs.m (+ its helpers ivan_the_terrible.m, test_dms.m, dec2deg.m). The C++ dialog calls
# `_on_dimfun(which, state)` whenever one of the 8 Griding Line Geometry boxes is edited; we return
# the 8 recomputed fields and the dialog writes them back.
#
#   which  = "xMin|xMax|yMin|yMax|xInc|yInc|nCols|nRows"
#   state  = "xMin/xMax/yMin/yMax/xInc/yInc/nCols/nRows/oneOrZero/xMinOr/xMaxOr/yMinOr/yMaxOr/dms"
#   return = "xMin/xMax/yMin/yMax/xInc/yInc/nCols/nRows"
#
# `*Or` are the source-grid caps (a sampled grid can't exceed the input extent); empty = no cap.
# `oneOrZero` is 1 for gridline, 0 for pixel registration.

# ── small helpers (ports of test_dms / cellDMS2double / ivan_the_terrible / dec2deg) ─────────────

# Parse a decimal or dd:mm[:ss] string -> token vector; empty vector if empty/invalid.
function _test_dms(s::String)::Vector{String}
	s = strip(s)
	isempty(s) && return String[]
	(startswith(s, ":") || endswith(s, ":") || occursin("::", s)) && return String[]
	toks = split(s, ':')
	for t in toks
		(isempty(t) || tryparse(Float64, t) === nothing) && return String[]
	end
	return String.(toks)
end

# Combine dd:mm:ss tokens into a decimal value; sign taken from the first token (Mirone convention).
function _cellDMS2double(toks)
	x = 0.0
	if parse(Float64, toks[1]) > 0
		for i in eachindex(toks);  x += parse(Float64, toks[i]) / 60.0^(i - 1);  end
	else
		for i in eachindex(toks);  x -= abs(parse(Float64, toks[i])) / 60.0^(i - 1);  end
	end
	return x
end

# Box string -> decimal value, or `NaN` when empty/invalid.
_box_num(s::AbstractString)::Float64 = (v = _test_dms(s); isempty(v) ? NaN : _cellDMS2double(v))

# GMT_grd_RI_verify-compatible increment. mode 1: n = #lines -> inc; mode 2: n = candidate inc.
function _ivan(dx::Float64, n::Real, mode::Int, oz::Int)
	if mode == 1
		return dx / (n - oz)
	else                       # mode 2
		t_inc = Float64(n)
		(dx / t_inc) < 2 && return dx / 2          # "you must be joking" -> least-idiot (3 lines)
		nl = round(dx / t_inc) + oz
		return dx / (nl - oz)
	end
end

# Significant-digit string wide enough to show `x` without losing precision (Mirone get_format_str:
# integer-part digit count + N significant digits, mimicking sprintf('%.Ng')). Base-only (no Printf).
function _fmtg(x::Float64, N::Int = 8)
	nDigit = length(string(trunc(Int, abs(x)))) + (x < 0 ? 1 : 0)   # integer-part digits (incl. sign)
	s = string(round(x, sigdigits = nDigit + N))
	endswith(s, ".0") ? s[1:end-2] : s                              # trim trailing ".0" like %g
end

# Decimal degrees -> "d:m:s.sss" (Mirone dec2deg string form; magnitudes only, as in the original).
function _dec2deg(x::Float64)
	deg = trunc(Int, x)
	minutes = trunc(Int, 60 * (x - deg))
	seconds = 3600 * (x - deg) - 60 * minutes
	if seconds > 60;  seconds -= 60;  minutes += 1;  end
	if 60 - seconds < 1e-10;  seconds = 0.0;  minutes += 1;  end
	return "$(abs(deg)):$(abs(minutes)):$(round(abs(seconds), digits = 3))"
end

# ── state struct + the 8 edit handlers ──────────────────────────────────────────────────────────

mutable struct _DimState
	xMin::String; xMax::String; yMin::String; yMax::String
	xInc::String; yInc::String; nCols::String; nRows::String
	oz::Int
	xMinOr::Float64; xMaxOr::Float64   # NaN = no cap
	yMinOr::Float64; yMaxOr::Float64
end

# One generic min/max handler (x and y differ only in which boxes they touch). `lo` = this is the
# MIN box (cap = *Or lower bound); otherwise it's the MAX box (cap = *Or upper bound).
function _edit_limit!(st::_DimState, axis::Symbol, lo::Bool)
	box, other, incf, nf, cap, otherval = if axis === :x
		lo ? (:xMin, :xMax, :xInc, :nCols, st.xMinOr, _box_num(st.xMax)) :
		     (:xMax, :xMin, :xInc, :nCols, st.xMaxOr, _box_num(st.xMin))
	else
		lo ? (:yMin, :yMax, :yInc, :nRows, st.yMinOr, _box_num(st.yMax)) :
		     (:yMax, :yMin, :yInc, :nRows, st.yMaxOr, _box_num(st.yMin))
	end
	xx  = getfield(st, box)
	val = _test_dms(xx)
	if isempty(val)                                 # box emptied -> clear inc + count too
		setfield!(st, box, ""); setfield!(st, incf, ""); setfield!(st, nf, "")
		return
	end
	v = _cellDMS2double(val)
	if !isnan(cap) && ((lo && v < cap) || (!lo && v > cap))       # clamp to the source extent
		setfield!(st, box, _fmtg(cap)); return
	end
	# West>=East / South>=North check (other box filled).
	if !isnan(otherval) && ((lo && v >= otherval) || (!lo && v <= otherval))
		setfield!(st, box, ""); return
	end
	if !isnan(otherval)
		span = lo ? (otherval - v) : (v - otherval)
		nc   = getfield(st, nf)
		if !isempty(nc) && tryparse(Float64, nc) !== nothing
			inc = _ivan(span, round(abs(parse(Float64, nc))), 1, st.oz)
			setfield!(st, incf, _fmtg(inc))
		else                                       # no count yet -> default to 100
			inc = _ivan(span, 100, 1, st.oz)
			setfield!(st, incf, _fmtg(inc)); setfield!(st, nf, "100")
		end
	end
	return
end

# One generic increment handler.
function _edit_inc!(st::_DimState, axis::Symbol)
	box, minf, maxf, nf, otherinc = axis === :x ?
		(:xInc, :xMin, :xMax, :nCols, :yInc) : (:yInc, :yMin, :yMax, :nRows, :xInc)
	xx  = getfield(st, box)
	val = _test_dms(xx)
	if isempty(val)
		setfield!(st, box, ""); return
	end
	dms   = length(val) > 1
	inc   = _cellDMS2double(val)
	lo    = _box_num(getfield(st, minf))
	hi    = _box_num(getfield(st, maxf))
	if !isnan(lo) && !isnan(hi)
		inc = _ivan(hi - lo, inc, 2, st.oz)        # snap to a GMT-legal increment
		setfield!(st, box, dms ? _dec2deg(inc) : _fmtg(inc, 10))
		n = round(Int, (hi - lo) / inc) + st.oz
		setfield!(st, nf, string(n))
	end
	# Mirone mirrors x_inc into an empty y_inc (only for the X handler).
	axis === :x && isempty(getfield(st, otherinc)) && setfield!(st, otherinc, xx)
	return
end

# One generic count (Ncols/Nrows) handler.
function _edit_count!(st::_DimState, axis::Symbol)
	cf, minf, maxf, incf = axis === :x ?
		(:nCols, :xMin, :xMax, :xInc) : (:nRows, :yMin, :yMax, :yInc)
	xx = getfield(st, cf)
	lo = _box_num(getfield(st, minf)); hi = _box_num(getfield(st, maxf))
	if !isnan(lo) && !isnan(hi) && !isempty(getfield(st, incf)) &&
	   !isempty(xx) && tryparse(Float64, xx) !== nothing
		inc = _ivan(hi - lo, round(abs(parse(Float64, xx))), 1, st.oz)
		dms = occursin(":", getfield(st, incf))    # keep the inc in the format it was typed
		setfield!(st, incf, dms ? _dec2deg(inc) : _fmtg(inc, 10))
	end
	return
end

# ── C entry point ───────────────────────────────────────────────────────────────────────────────

const _DIMFUN_BUF = Ref{Vector{UInt8}}(UInt8[0])

function _on_dimfun(cwhich::Cstring, cstate::Cstring)::Cstring
	out = ""
	try
		which = unsafe_string(cwhich)
		f = split(unsafe_string(cstate), '/')
		getf(i) = length(f) >= i ? String(f[i]) : ""
		capnum(s) = (s = strip(s); isempty(s) ? NaN : something(tryparse(Float64, s), NaN))
		st = _DimState(getf(1), getf(2), getf(3), getf(4), getf(5), getf(6), getf(7), getf(8),
		               tryparse(Int, getf(9)) === nothing ? 1 : parse(Int, getf(9)),
		               capnum(getf(10)), capnum(getf(11)), capnum(getf(12)), capnum(getf(13)))
		if     which == "xMin";  _edit_limit!(st, :x, true)
		elseif which == "xMax";  _edit_limit!(st, :x, false)
		elseif which == "yMin";  _edit_limit!(st, :y, true)
		elseif which == "yMax";  _edit_limit!(st, :y, false)
		elseif which == "xInc";  _edit_inc!(st, :x)
		elseif which == "yInc";  _edit_inc!(st, :y)
		elseif which == "nCols"; _edit_count!(st, :x)
		elseif which == "nRows"; _edit_count!(st, :y)
		end
		out = join((st.xMin, st.xMax, st.yMin, st.yMax, st.xInc, st.yInc, st.nCols, st.nRows), '/')
	catch e
		@warn "dim_fun FAILED" exception=(e,)
	end
	_DIMFUN_BUF[] = Vector{UInt8}(codeunits(out * "\0"))
	return Cstring(pointer(_DIMFUN_BUF[]))
end

function _register_dimfun()
	fptr = @cfunction((w, s) -> Base.invokelatest(_on_dimfun, w, s), Cstring, (Cstring, Cstring))
	ccall(_fn(:gmtvtk_set_dimfun_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
