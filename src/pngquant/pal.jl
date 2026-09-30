# pal.jl — pal.rs: the colour types, the internal premultiplied-gamma pixel, palettes, gamma LUT,
# posterization and the MSE scale conversions.

const INTERNAL_GAMMA = 0.57
const LIQ_WEIGHT_A = 0.625f0
const LIQ_WEIGHT_R = 0.5f0
const LIQ_WEIGHT_G = 1.0f0
const LIQ_WEIGHT_B = 0.45f0
# fudge factor — reminder that colours are not in 0..1 range any more
const LIQ_WEIGHT_MSE = 0.45

# Palettes are at most this long (the crate's default, non-"large_palettes" build).
const MAX_COLORS = 256

"""
    RGBA(r, g, b, a)

8-bit sRGB colour with straight (non-premultiplied) alpha — the only colour format the library
takes in and hands back.
"""
struct RGBA
	r::UInt8
	g::UInt8
	b::UInt8
	a::UInt8
end
RGBA(r::Integer, g::Integer, b::Integer, a::Integer) = RGBA(UInt8(r), UInt8(g), UInt8(b), UInt8(a))
RGBA() = RGBA(0x00, 0x00, 0x00, 0x00)

# Rust's `f as u8`: truncation toward zero, saturating at both ends, NaN -> 0.
@inline _sat_u8(x::Float32)::UInt8 = !(x > 0f0) ? 0x00 : x >= 255f0 ? 0xff : unsafe_trunc(UInt8, x)
@inline _sat_u16(x::Float32)::UInt16 = !(x > 0f0) ? 0x0000 : x >= 65535f0 ? 0xffff : unsafe_trunc(UInt16, x)

# ---------------------------------------------------------------------------------------------
# f_pixel: 4xf32 ARGB, premultiplied, in the internal gamma, each channel weighted

struct FPixel
	a::Float32
	r::Float32
	g::Float32
	b::Float32
end
FPixel() = FPixel(0f0, 0f0, 0f0, 0f0)

Base.:+(x::FPixel, y::FPixel) = FPixel(x.a + y.a, x.r + y.r, x.g + y.g, x.b + y.b)
Base.:-(x::FPixel, y::FPixel) = FPixel(x.a - y.a, x.r - y.r, x.g - y.g, x.b - y.b)
Base.:*(x::FPixel, s::Float32) = FPixel(x.a * s, x.r * s, x.g * s, x.b * s)
Base.:/(x::FPixel, s::Float32) = FPixel(x.a / s, x.r / s, x.g / s, x.b / s)

# Colour difference: the larger of the difference seen on a black and on a white background,
# summed over r,g,b (alpha shows up through the white-background term).
@inline function pxdiff(x::FPixel, y::FPixel)::Float32
	alphas = y.a - x.a
	br = x.r - y.r; bg = x.g - y.g; bb = x.b - y.b
	wr = br + alphas; wg = bg + alphas; wb = bb + alphas
	return max(br * br, wr * wr) + max(bg * bg, wg * wg) + max(bb * bb, wb * wb)
end

@inline is_fully_transparent(p::FPixel) = p.a < Float32(1 / 255 * Float64(LIQ_WEIGHT_A))
@inline is_fully_opaque(p::FPixel) = p.a >= Float32(255 / 256 * Float64(LIQ_WEIGHT_A))

function to_rgb(p::FPixel, gamma::Float64)::RGBA
	is_fully_transparent(p) && return RGBA()
	r = Float32(Float64(LIQ_WEIGHT_A) / Float64(LIQ_WEIGHT_R)) * p.r / p.a
	g = Float32(Float64(LIQ_WEIGHT_A) / Float64(LIQ_WEIGHT_G)) * p.g / p.a
	b = Float32(Float64(LIQ_WEIGHT_A) / Float64(LIQ_WEIGHT_B)) * p.b / p.a
	gm = Float32(gamma / INTERNAL_GAMMA)
	# 256, because numbers are in range 1..255.9999… rounded down
	return RGBA(_sat_u8(max(r, 0f0)^gm * 256f0), _sat_u8(max(g, 0f0)^gm * 256f0),
	            _sat_u8(max(b, 0f0)^gm * 256f0), _sat_u8(p.a * Float32(256 / Float64(LIQ_WEIGHT_A))))
end

@inline function from_rgba(lut::Vector{Float32}, px::RGBA)::FPixel
	a = Float32(px.a) / 255f0
	@inbounds FPixel(a * LIQ_WEIGHT_A, lut[px.r+1] * LIQ_WEIGHT_R * a, lut[px.g+1] * LIQ_WEIGHT_G * a,
	                 lut[px.b+1] * LIQ_WEIGHT_B * a)
end

function gamma_lut(gamma::Float64)::Vector{Float32}
	e = Float32(INTERNAL_GAMMA / gamma)
	return Float32[(Float32(i) / 255f0)^e for i in 0:255]
end

# ---------------------------------------------------------------------------------------------
# PalPop: popularity, with `is_fixed` stuffed into the sign bit

@inline pop_is_fixed(p::Float32) = p < 0f0
@inline pop_to_fixed(p::Float32) = p < 0f0 ? p : (p > 0f0 ? -p : -1f0)
@inline popularity(p::Float32) = abs(p)

# ---------------------------------------------------------------------------------------------
# PalF: palette of premultiplied ARGB colours in internal gamma, plus their popularities

struct PalF
	colors::Vector{FPixel}
	pops::Vector{Float32}
end
PalF() = PalF(sizehint!(FPixel[], MAX_COLORS), sizehint!(Float32[], MAX_COLORS))
Base.copy(p::PalF) = PalF(copy(p.colors), copy(p.pops))
Base.length(p::PalF) = length(p.colors)

function pal_push!(p::PalF, color::FPixel, pop::Float32)
	push!(p.pops, pop)
	push!(p.colors, color)
	return p
end

function pal_set!(p::PalF, i::Int, color::FPixel, pop::Float32)
	p.pops[i] = pop
	p.colors[i] = color
	return p
end

function pal_swap!(p::PalF, a::Int, b::Int)
	p.colors[a], p.colors[b] = p.colors[b], p.colors[a]
	p.pops[a], p.pops[b] = p.pops[b], p.pops[a]
	return p
end

# `max_colors` is the user's limit, not just the size of the current candidate palette.
function with_fixed_colors(p::PalF, max_colors::Int, fixed::Vector{FPixel})::PalF
	isempty(fixed) && return p
	# with low quality, mediancut may not have made enough colours
	max_fixed = min(length(fixed), max_colors)
	if length(p) < max_fixed
		needs_extra = max_fixed - length(p)
		append!(p.colors, fixed[1:needs_extra])
		append!(p.pops, fill(0f0, needs_extra))
	end
	# the fixed colours were in the histogram, so expect them in the palette: the closest existing
	# entry is changed into the exact fixed one
	for i in 1:min(length(fixed), length(p))
		fc = fixed[i]
		best_idx = i
		best = pxdiff(p.colors[i], fc)
		for j in i+1:length(p)
			d = pxdiff(p.colors[j], fc)
			if d < best
				best = d; best_idx = j
			end
		end
		pal_swap!(p, i, best_idx)
		pal_set!(p, i, fc, pop_to_fixed(p.pops[i]))
	end
	return p
end

@inline posterize_channel(c::UInt8, bits::Int) = bits == 0 ? c : (c & ~UInt8((1 << bits) - 1)) | (c >> (8 - bits))

# Makes the 8-bit palette, and rounds the float palette to exactly what the 8-bit one holds.
function init_int_palette!(p::PalF, gamma::Float64, posterize::Int)::Vector{RGBA}
	lut = gamma_lut(gamma)
	out = Vector{RGBA}(undef, length(p))
	for i in 1:length(p)
		c = to_rgb(p.colors[i], gamma)
		px = RGBA(posterize_channel(c.r, posterize), posterize_channel(c.g, posterize),
		          posterize_channel(c.b, posterize), posterize_channel(c.a, posterize))
		p.colors[i] = from_rgba(lut, px)
		if px.a == 0 && !pop_is_fixed(p.pops[i])
			px = RGBA(71, 112, 76, 0)
		end
		out[i] = px
	end
	return out
end

# MSE that assumes 0..1 channels, scaled to the MSE met in practice
unit_mse_to_internal_mse(mse::Float64) = LIQ_WEIGHT_MSE * mse
# internal MSE scaled to the equivalent in 0..255 pixels
internal_mse_to_standard_mse(mse::Float64) = (mse * 65536.0 / 6.0) / LIQ_WEIGHT_MSE
