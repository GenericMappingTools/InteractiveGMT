# Colour parsing shared by the overlay / point / fv viewers.

# A few named colours (GMT/CSS basics) for `color=:red` etc.
const _NAMED_COLORS = Dict{Symbol,NTuple{3,Float64}}(
	:red=>(1.0,0.0,0.0), :green=>(0.0,0.6,0.0), :blue=>(0.0,0.0,1.0),
	:black=>(0.0,0.0,0.0), :white=>(1.0,1.0,1.0), :yellow=>(1.0,1.0,0.0),
	:cyan=>(0.0,1.0,1.0), :magenta=>(1.0,0.0,1.0), :orange=>(1.0,0.5,0.0),
	:purple=>(0.6,0.0,0.8), :brown=>(0.55,0.27,0.07),
	:gray=>(0.5,0.5,0.5), :grey=>(0.5,0.5,0.5))

# data_color -> (r,g,b) in 0..1. nothing -> per-mode default; a colour NAME (Symbol or
# String); a 0-255 int / (r,g,b) of ints or 0-1 floats all accepted.
function _ovl_color(data_color, mode::Symbol)
	data_color === nothing && return mode === :lines ? (0.0, 0.0, 0.0) : (1.0, 0.0, 0.0)
	if data_color isa Symbol || data_color isa AbstractString
		key = Symbol(lowercase(String(data_color)))
		haskey(_NAMED_COLORS, key) || error("unknown colour name :$data_color (have $(sort(collect(keys(_NAMED_COLORS)))))")
		return _NAMED_COLORS[key]
	end
	col = data_color isa Number ? (data_color, data_color, data_color) : data_color
	c = map(v -> v <= 1 ? Float64(v) : Float64(v) / 255, col)
	return (c[1], c[2], c[3])
end

# "-G#rrggbb" / "-Gr/g/b" / "-G<gray>" / "-G<name>" -> (r,g,b) UInt8. Mirrors GMTF3D parse_gmt_color.
function _parse_gmt_color(s::AbstractString)::NTuple{3,UInt8}
	c = strip(s)
	startswith(c, "-G") && (c = c[3:end])
	isempty(c) && return (0x80, 0x80, 0x80)
	if startswith(c, "#")                                   # #rrggbb
		h = c[2:end]
		return (parse(UInt8, h[1:2], base=16), parse(UInt8, h[3:4], base=16), parse(UInt8, h[5:6], base=16))
	elseif occursin('/', c)                                 # r/g/b
		p = split(c, '/')
		return (UInt8(clamp(parse(Int, p[1]), 0, 255)), UInt8(clamp(parse(Int, p[2]), 0, 255)), UInt8(clamp(parse(Int, p[3]), 0, 255)))
	elseif all(isdigit, c)                                  # single number => gray
		g = UInt8(clamp(parse(Int, c), 0, 255));  return (g, g, g)
	else                                                    # named (reuse the overlay palette)
		t = get(_NAMED_COLORS, Symbol(lowercase(c)), (0.5, 0.5, 0.5))
		return (round(UInt8, 255t[1]), round(UInt8, 255t[2]), round(UInt8, 255t[3]))
	end
end
