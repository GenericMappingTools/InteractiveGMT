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
	:plus=>"+",                     :dash=>"-", :minus=>"-")

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
`:cross`, `:plus`, `:dash`) or a 1-char GMT code. `size` is on screen, in `:px` or `:pt`. `fill`
and `edge` accept any colour `_ovl_color` understands (name Symbol/String, 0-1 or 0-255 tuple).
Symbols stay the same pixel size at any zoom. Returns `true` if the layer was added.
"""
function add_symbols!(handle, x, y;
                      z=0.0, symbol=:c, size=8, sizeunit::Symbol=:px,
                      fill=:yellow, edge=:black, edgewidth=1.0, filled::Bool=true,
                      name::AbstractString="", info=nothing)
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
	spx  = sizeunit === :pt ? Float64(size) * 96 / 72 : Float64(size)   # points -> pixels @96dpi
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
	ok = ccall(_fn(:gmtvtk_add_symbols_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cstring, Cdouble, Cint,
	       Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
	      p, xyz, Cint(n), code, spx, Cint(filled ? 1 : 0),
	      fr, fg, fb, er, eg, eb, Float64(edgewidth), name, packed)
	return ok != 0
end
