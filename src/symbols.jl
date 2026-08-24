# symbols.jl — generic screen-constant SYMBOL layers (volcanoes, seismicity, cities, hotspots, …).
#
# One shape (a GMT symbol code) stamped at N points, drawn at a CONSTANT pixel size at any zoom
# (the C side rescales the glyph each frame from the camera, like the gizmo). This is the reusable
# primitive every "plot a point dataset" feature should call — Geography volcanoes is the first user.
#
# The C export is gmtvtk_add_symbols_h(handle, xyz, npts, sym, sizePx, filled, fr,fg,fb, er,eg,eb,
# edgeWidth) in 90_c_api.cpp; this file is the Julia-friendly front (named symbols/colours, px|pt).

# Friendly symbol name -> GMT 1-char code. Raw 1-char codes ("c","t","+",…) are also accepted.
const _SYMBOL_CODES = Dict{Symbol,String}(
	:circle=>"c", :c=>"c",          :square=>"s", :s=>"s",
	:triangle=>"t", :t=>"t",        :itriangle=>"i", :invtriangle=>"i", :i=>"i",
	:diamond=>"d", :d=>"d",         :hexagon=>"h", :h=>"h",
	:pentagon=>"n", :n=>"n",        :octagon=>"g", :g=>"g",
	:star=>"a", :a=>"a",            :cross=>"x", :x=>"x",
	:plus=>"+",                     :dash=>"-", :minus=>"-",
	:sphere=>"o", :ball=>"o", :o=>"o",
	:cube=>"u", :box=>"u", :u=>"u")

# Resolve a user `symbol` (Symbol / Char / String) to a GMT code. A length-1 string/char passes
# through verbatim (so "+", "-", "x" work); otherwise it's looked up by friendly name.
function _symbol_code(symbol)::String
	(symbol isa AbstractChar) && return string(symbol)
	(symbol isa AbstractString && length(symbol) == 1) && return String(symbol)
	key = Symbol(lowercase(String(symbol)))
	return get(_SYMBOL_CODES, key) do
		error("unknown symbol $(repr(symbol)); names: $(sort(unique(string.(keys(_SYMBOL_CODES))))) or a 1-char GMT code")
	end
end

_sym_ptr(h::Ptr{Cvoid}) = h
_sym_ptr(f) = f.h                          # QtFigure (and friends) carry the Scene* in `.h`

"""
    add_symbols!(handle, x, y; z=0, symbol=:c, size=8, sizeunit=:px,
                 fill=:yellow, edge=:black, edgewidth=1.0, filled=true)

Stamp a screen-constant symbol layer at points `(x, y[, z])` (TRUE data coords) on an existing
viewer `handle` (a `QtFigure` or a raw `Scene*` `Ptr`). `symbol` is a friendly name (`:circle`,
`:square`, `:triangle`, `:itriangle`, `:diamond`, `:hexagon`, `:pentagon`, `:octagon`, `:star`,
`:cross`, `:plus`, `:dash`, `:sphere`, `:cube`) or a 1-char GMT code. `:sphere`/`:cube` are true
3-D, lit volumes (visible from any angle, e.g. edge-on in perspective) — every other shape is a
flat, unlit XY glyph. `size` is on screen, in `:px` or `:pt`. `fill` and `edge` accept any colour
`_ovl_color` understands (name Symbol/String, 0-1 or 0-255 tuple). Symbols stay the same pixel
size at any zoom. Returns `true` if the layer was added.
"""
function add_symbols!(handle, x, y;
                      z=0.0, symbol=:c, size=8, sizeunit::Symbol=:px,
                      fill=:yellow, edge=:black, edgewidth=1.0, filled::Bool=true,
                      name::AbstractString="", info=nothing,
                      datanames::Vector{String}=String[],
                      datarows::Vector{Vector{String}}=Vector{Vector{String}}())
	p = _sym_ptr(handle)
	xv = collect(Float64, x); yv = collect(Float64, y)
	n = length(xv)
	n == length(yv) || error("add_symbols!: x ($(length(xv))) and y ($(length(yv))) length mismatch")
	n == 0 && return false
	zv = z isa AbstractVector ? collect(Float64, z) : [Float64(z) for _ in 1:n]
	length(zv) == n || error("add_symbols!: z length must be 1 or match x/y")
	xyz = Vector{Float64}(undef, 3n)
	@inbounds for i in 1:n
		xyz[3i-2] = xv[i]; xyz[3i-1] = yv[i]; xyz[3i] = zv[i]
	end
	code = _symbol_code(symbol)
	# points -> pixels @96dpi. A VECTOR `size` (per-point, see below) has no single value here; its
	# base is picked from the vector further down, so stand in with a placeholder until then.
	spx  = size isa AbstractVector ? 0.0 :
	       (sizeunit === :pt ? Float64(size) * 96 / 72 : Float64(size))
	fr, fg, fb = _ovl_color(fill, :points)
	er, eg, eb = _ovl_color(edge, :lines)
	# Optional per-point hover text -> one packed string: n records joined by RS ('\x1e'), each a
	# ready-to-show multi-line block (newlines kept). The C side splits on RS and shows the matching
	# block as a tooltip when the cursor is over a symbol. Must align 1:1 with the points or it's dropped.
	packed = ""
	if info !== nothing
		iv = collect(String, info)
		length(iv) == n || error("add_symbols!: info length ($(length(iv))) must match x/y ($n)")
		packed = join(iv, '\x1e')
	end
	# Per-point size / fill (a "scaled symbols" table): ONE layer carrying its own factors, never one
	# layer per row — that would be one actor and one Scene Objects row per point. `size` may be a
	# vector (the biggest becomes the layer's base size, the rest ride as factors of it) and `fill` a
	# matrix/vector of per-point colours.
	sizev = size isa AbstractVector ? collect(Float64, size) : nothing
	fillm = _sym_point_rgb(fill, n)
	if (sizev === nothing && fillm === nothing)
		ok = ccall(_fn(:gmtvtk_add_symbols_h), Cint,
		      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cstring, Cdouble, Cint,
		       Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
		      p, xyz, Cint(n), code, spx, Cint(filled ? 1 : 0),
		      fr, fg, fb, er, eg, eb, Float64(edgewidth), name, packed)
		ok != 0 && _sym_attach_table(p, String(name), datanames, datarows, n)
		return ok != 0
	end
	if (sizev !== nothing)
		length(sizev) == n || error("add_symbols!: size vector ($(length(sizev))) must match x/y ($n)")
		base = maximum(sizev)
		base > 0 || error("add_symbols!: size vector must hold a positive value")
		spx   = sizeunit === :pt ? base * 96 / 72 : base
		scale = sizev ./ base
	else
		scale = C_NULL
	end
	ok = ccall(_fn(:gmtvtk_add_symbols_ex_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cstring, Cdouble, Cint,
	       Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring,
	       Ptr{Cdouble}, Ptr{Cdouble}),
	      p, xyz, Cint(n), code, spx, Cint(filled ? 1 : 0),
	      fr, fg, fb, er, eg, eb, Float64(edgewidth), name, packed,
	      scale, fillm === nothing ? C_NULL : fillm)
	ok != 0 && _sym_attach_table(p, String(name), datanames, datarows, n)
	return ok != 0
end

# The layer's OWN DATA (a catalog's lon/lat/depth/magnitude/date) -> the "Show data table" window.
# Already FORMATTED by the caller, because only the caller knows what its columns mean: a date is a
# date, a magnitude carries one decimal. Graphical properties (the on-screen symbol size) are NOT
# data and never go in here. Silently skipped unless the rows align 1:1 with the points.
function _sym_attach_table(p::Ptr{Cvoid}, name::String, names::Vector{String},
                           rows::Vector{Vector{String}}, n::Int)
	(isempty(names) || length(rows) != n) && return false
	hdr  = join(names, '\x1f')
	body = join((join(r, '\x1f') for r in rows), '\x1e')
	ok = ccall(_fn(:gmtvtk_symbol_set_table_h), Cint,
	           (Ptr{Cvoid}, Cstring, Cstring, Cstring), p, name, hdr, body)
	return ok != 0
end

# Per-point fill colours -> a flat 3n vector of 0..1 RGB, or nothing when `fill` is one colour for the
# whole layer (the ordinary case, handled by the plain export). Accepts an n x 3 matrix or a vector of
# n colours; 0..255 input is detected and normalised, like every other colour path here.
function _sym_point_rgb(fill, n::Int)
	M = fill isa AbstractMatrix ? Float64.(fill) :
	    (fill isa AbstractVector && length(fill) == n && n != 3 && eltype(fill) <: Union{Tuple,AbstractVector}) ?
	        reduce(vcat, (reshape(Float64.(collect(c))[1:3], 1, 3) for c in fill)) : nothing
	M === nothing && return nothing
	size(M, 1) == n && size(M, 2) >= 3 || error("add_symbols!: fill matrix must be $(n)x3")
	M = M[:, 1:3]
	maximum(M) > 1 && (M = M ./ 255)
	return vec(permutedims(clamp.(M, 0, 1)))
end
