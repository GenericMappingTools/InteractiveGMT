# attr.jl — attr.rs + error.rs: the configuration handle, and the library's error type.

"""
    LiqError(code, msg)

Thrown by the library. `code` is one of `:QualityTooLow`, `:ValueOutOfRange`, `:Unsupported`,
`:BufferTooSmall` — the same variants as libimagequant's `Error`.
"""
struct LiqError <: Exception
	code::Symbol
	msg::String
end
Base.showerror(io::IO, e::LiqError) = print(io, "libimagequant: ", e.code, isempty(e.msg) ? "" : ": " * e.msg)

# remap.rs DitherMapMode
const DITHERMAP_NONE    = 0
const DITHERMAP_ENABLED = 1
const DITHERMAP_ALWAYS  = 2

const LIQ_HIGH_MEMORY_LIMIT = 1 << 26

"""
    LiqAttr()

Settings of the quantization (libimagequant's `Attributes`). Defaults: speed 4, 256 colours,
quality 0-100 (best effort, never fails). Change them with `set_speed!`, `set_quality!`,
`set_max_colors!`, `set_min_posterization!`, `set_last_index_transparent!`. `log` is a
`String -> Any` function receiving the verbose messages, or `nothing`.
"""
mutable struct LiqAttr
	max_colors::Int
	target_mse::Float64
	max_mse::Union{Nothing,Float64}
	kmeans_iteration_limit::Float64
	kmeans_iterations::Int
	feedback_loop_trials::Int
	max_histogram_entries::Int
	min_posterization_output::Int
	min_posterization_input::Int
	last_index_transparent::Bool
	use_contrast_maps::Bool
	single_threaded_dithering::Bool
	use_dither_map::Int
	speed::Int
	log::Union{Nothing,Function}
end

function LiqAttr()
	attr = LiqAttr(MAX_COLORS, 0.0, nothing, 0.0, 0, 0, 0, 0, 0, false, false, false, DITHERMAP_NONE, 0, nothing)
	set_speed!(attr, 4)
	return attr
end

verbose_print(attr::LiqAttr, msg::String) = (attr.log === nothing || attr.log(msg); nothing)

"""
    set_max_colors!(attr, n)

2..256. It is better to use `set_quality!`.
"""
function set_max_colors!(attr::LiqAttr, colors::Integer)
	2 <= colors <= 256 || throw(LiqError(:ValueOutOfRange, "colors must be 2-256"))
	attr.max_colors = Int(colors)
	return attr
end

"""
    set_quality!(attr, minimum, target)

Range 0-100, roughly like JPEG. If the minimum can't be met the quantization throws
`LiqError(:QualityTooLow)`. With target < 100 the library tries to use fewer colours.
"""
function set_quality!(attr::LiqAttr, minimum::Integer, target::Integer)
	(0 <= target <= 100 && target >= minimum) || throw(LiqError(:ValueOutOfRange, "quality must be 0 <= min <= max <= 100"))
	target < 30 && verbose_print(attr, "  warning: quality set too low")
	attr.target_mse = quality_to_mse(Int(target))
	attr.max_mse = quality_to_mse(Int(minimum))
	return attr
end

"""
    set_speed!(attr, s)

1 (slow, best) … 10 (fast, rough). Default 4.
"""
function set_speed!(attr::LiqAttr, value::Integer)
	1 <= value <= 10 || throw(LiqError(:ValueOutOfRange, "speed must be 1-10"))
	iterations = max(8 - value, 0)
	iterations += iterations * iterations ÷ 2
	attr.kmeans_iterations = iterations
	attr.kmeans_iteration_limit = 1.0 / Float64(1 << (23 - value))
	attr.feedback_loop_trials = max(56 - 9 * value, 0)
	attr.max_histogram_entries = (1 << 17) + (1 << 18) * (10 - value)
	attr.min_posterization_input = value >= 8 ? 1 : 0
	attr.use_dither_map = value <= 6 ? DITHERMAP_ENABLED : DITHERMAP_NONE
	if attr.use_dither_map != DITHERMAP_NONE && value < 3
		attr.use_dither_map = DITHERMAP_ALWAYS
	end
	attr.use_contrast_maps = (value <= 7) || attr.use_dither_map != DITHERMAP_NONE
	attr.single_threaded_dithering = value == 1
	attr.speed = Int(value)
	return attr
end

"""
    set_min_posterization!(attr, bits)

0-4: number of least significant bits to drop (for 15-bit, ARGB4444 … output).
"""
function set_min_posterization!(attr::LiqAttr, value::Integer)
	0 <= value <= 4 || throw(LiqError(:ValueOutOfRange, "posterization must be 0-4"))
	attr.min_posterization_output = Int(value)
	return attr
end

"""
    set_last_index_transparent!(attr, tf)

Move the transparent colour to the LAST palette entry (less efficient for PNG, required by some
broken software).
"""
set_last_index_transparent!(attr::LiqAttr, tf::Bool) = (attr.last_index_transparent = tf; attr)

function feedback_loop_trials(attr::LiqAttr, hist_items::Int)::Int
	t = attr.feedback_loop_trials
	hist_items > 5000    && (t = (t * 3 + 3) ÷ 4)
	hist_items > 25000   && (t = (t * 3 + 3) ÷ 4)
	hist_items > 50000   && (t = (t * 3 + 3) ÷ 4)
	hist_items > 100_000 && (t = (t * 3 + 3) ÷ 4)
	return t
end

# (max_mse, target_mse, user asked for perfect quality)
function target_mse(attr::LiqAttr, hist_items_len::Int)
	max_mse = attr.max_mse === nothing ? nothing : attr.max_mse * (hist_items_len <= MAX_COLORS ? 0.33 : 1.0)
	aim_for_perfect_quality = attr.target_mse == 0.0
	t = max(attr.target_mse, (Float64(1 << attr.min_posterization_output) / 1024.0)^2)
	max_mse === nothing || (t = min(t, max_mse))
	return max_mse, t, aim_for_perfect_quality
end

# (iterations, iteration_limit)
function kmeans_iterations(attr::LiqAttr, hist_items_len::Int, palette_error_is_known::Bool)
	limit = attr.kmeans_iteration_limit
	it = attr.kmeans_iterations
	hist_items_len > 5000  && (it = (it * 3 + 3) ÷ 4)
	hist_items_len > 25000 && (it = (it * 3 + 3) ÷ 4)
	hist_items_len > 50000 && (it = (it * 3 + 3) ÷ 4)
	if hist_items_len > 100_000
		it = (it * 3 + 3) ÷ 4
		limit *= 2.0
	end
	if it == 0 && !palette_error_is_known && attr.max_mse !== nothing
		it = 1
	end
	return it, limit
end

posterize_bits(attr::LiqAttr) = max(attr.min_posterization_output, attr.min_posterization_input)

# quant.rs: the 0-100 quality scale <-> internal MSE
function quality_to_mse(quality::Int)::Float64
	quality == 0 && return 1e20       # + epsilon for floating point errors
	quality >= 100 && return 0.0
	q = Float64(quality)
	extra_low_quality_fudge = max(0.016 / (0.001 + q) - 0.001, 0.0)
	return unit_mse_to_internal_mse(extra_low_quality_fudge + 2.5 / (210.0 + q)^1.2 * (100.1 - q) / 100.0)
end

function mse_to_quality(mse::Float64)::Int
	for i in 100:-1:1
		mse <= quality_to_mse(i) + 0.000001 && return i
	end
	return 0
end
