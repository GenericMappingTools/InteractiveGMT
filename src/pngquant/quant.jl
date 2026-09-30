# quant.jl — quant.rs: palette search (median cut in a feedback loop with K-Means), refinement,
# sorting, and the result object that remaps images.

"""
    LiqResult

Result of `quantize` (libimagequant's `QuantizationResult`): the palette, and the settings used to
remap images to it. Set `set_dithering_level!` / `set_output_gamma!`, then call `remapped`.
"""
mutable struct LiqResult
	palette::PalF
	int_palette::Vector{RGBA}
	remapped_palette::Union{Nothing,Vector{RGBA}}
	remapped_error::Union{Nothing,Float64}
	dither_level::Float32
	gamma::Float64
	palette_error::Union{Nothing,Float64}
	min_posterization_output::Int
	use_dither_map::Int
end

"""
    quantize(attr, img) -> LiqResult

Makes the palette for one image.
"""
function quantize(attr::LiqAttr, img::LiqImage)::LiqResult
	hist = LiqHistogram(attr)
	add_image!(hist, attr, img)
	return quantize_internal(hist, attr, false)
end

"""
    quantize(hist, attr) -> LiqResult

Makes one palette for everything added to the histogram. Its colours are frozen: remapping does
not refine them.
"""
quantize(hist::LiqHistogram, attr::LiqAttr)::LiqResult = quantize_internal(hist, attr, true)

function quantize_internal(hist::LiqHistogram, attr::LiqAttr, freeze_result_colors::Bool)::LiqResult
	(isempty(hist.hashmap) && isempty(hist.fixed_colors)) && throw(LiqError(:Unsupported, "empty histogram"))
	gamma = something(hist.gamma, 0.45455)
	h = finalize_builder(hist, gamma)
	verbose_print(attr, "  made histogram...$(length(h.items)) colors found")
	return LiqResult(attr, h, freeze_result_colors, gamma)
end

function LiqResult(attr::LiqAttr, hist::HistogramInternal, freeze_result_colors::Bool, gamma::Float64)
	max_mse, tmse, target_mse_is_zero = target_mse(attr, length(hist.items))
	palette, palette_error = find_best_palette(attr, tmse, target_mse_is_zero, max_mse, hist)
	if freeze_result_colors
		palette.pops .= pop_to_fixed.(palette.pops)
	end
	if palette_error !== nothing && max_mse !== nothing && palette_error > max_mse
		verbose_print(attr, "  image degradation MSE=$(_f3(internal_mse_to_standard_mse(palette_error))) " *
		                    "(Q=$(mse_to_quality(palette_error))) exceeded limit of " *
		                    "$(_f3(internal_mse_to_standard_mse(max_mse))) ($(mse_to_quality(max_mse)))")
		throw(LiqError(:QualityTooLow, "quality below the minimum set by set_quality!"))
	end
	sort_palette!(attr, palette)
	return LiqResult(palette, RGBA[], nothing, nothing, 1f0, gamma, palette_error, attr.min_posterization_output,
	                 attr.use_dither_map)
end

_f3(x::Float64) = string(round(x; digits=3))

"""
    set_dithering_level!(res, level)

0 (none) … 1 (full Floyd-Steinberg, the default).
"""
function set_dithering_level!(res::LiqResult, value::Real)
	0 <= value <= 1 || throw(LiqError(:ValueOutOfRange, "dithering level must be 0-1"))
	res.remapped_palette = nothing
	res.dither_level = Float32(value)
	return res
end

"""
    set_output_gamma!(res, gamma)

Gamma of the output palette (default: the input's, sRGB ~1/2.2).
"""
function set_output_gamma!(res::LiqResult, value::Real)
	0 < value < 1 || throw(LiqError(:ValueOutOfRange, "gamma must be 0-1"))
	res.remapped_palette = nothing
	res.gamma = Float64(value)
	return res
end

"Number 0-100 guessing how nice the input image will look remapped to this palette."
quantization_quality(res::LiqResult) = res.palette_error === nothing ? nothing : mse_to_quality(res.palette_error)
"Approximate mean square error of the palette (0..255 scale)."
quantization_error(res::LiqResult) = res.palette_error === nothing ? nothing : internal_mse_to_standard_mse(res.palette_error)
"Mean square error of the most recent remapping (0..255 scale)."
remapping_error(res::LiqResult) = (e = something(res.remapped_error, res.palette_error, NaN); isnan(e) ? nothing : internal_mse_to_standard_mse(e))
"Quality 0-100 of the most recent remapping."
remapping_quality(res::LiqResult) = (e = something(res.remapped_error, res.palette_error, NaN); isnan(e) ? nothing : mse_to_quality(e))

"""
    palette(res) -> Vector{RGBA}

The final palette. Take it AFTER `remapped`: remapping refines it.
"""
function palette(res::LiqResult)::Vector{RGBA}
	res.remapped_palette === nothing || return res.remapped_palette
	if isempty(res.int_palette)
		res.int_palette = init_int_palette!(res.palette, res.gamma, res.min_posterization_output)
	end
	return res.int_palette
end

"""
    remapped(res, img) -> (palette::Vector{RGBA}, indices::Vector{UInt8})

Remaps the image to the palette. `indices` are 0-based palette indices, row-major, top row first.
"""
function remapped(res::LiqResult, img::LiqImage)
	out = Vector{UInt8}(undef, img.width * img.height)
	pal = copy(res.palette)
	if res.dither_level == 0f0
		res.remapped_palette = init_int_palette!(pal, res.gamma, res.min_posterization_output)
		res.remapped_error = remap_to_palette!(img, out, pal)
	else
		dither_map_error = optionally_generate_dither_map!(res.use_dither_map, img, out, pal)
		output_image_is_remapped = dither_map_error !== nothing
		palette_error = dither_map_error === nothing ? res.palette_error : dither_map_error
		# the remapping above was the last chance of a K-Means iteration, so the final palette is set now
		res.remapped_palette = init_int_palette!(pal, res.gamma, res.min_posterization_output)
		res.remapped_error = palette_error
		max_dither_error = Float32(max(something(palette_error, quality_to_mse(80)) * 2.4, quality_to_mse(35)))
		remap_to_palette_floyd!(img, out, pal, res, max_dither_error, output_image_is_remapped)
	end
	return res.remapped_palette::Vector{RGBA}, out
end

function optionally_generate_dither_map!(use_dither_map::Int, img::LiqImage, out::Vector{UInt8}, palette::PalF)
	is_image_huge = img.width * img.height > 2000 * 2000
	allow = use_dither_map == DITHERMAP_ALWAYS || (!is_image_huge && use_dither_map != DITHERMAP_NONE)
	(allow && img.dither_map === nothing) || return nothing
	# with a dither map, this remap finds the areas that need dithering
	palette_error = remap_to_palette!(img, out, palette)
	update_dither_map!(img, out, palette)
	return palette_error
end

function sort_palette!(attr::LiqAttr, palette::PalF)
	lit = attr.last_index_transparent
	perm = sortperm(1:length(palette); by=i -> (!is_fully_opaque(palette.colors[i]) == lit, -popularity(palette.pops[i])),
	                alg=MergeSort)
	palette.colors .= palette.colors[perm]
	palette.pops .= palette.pops[perm]
	if lit
		alpha_index = 0
		for i in 1:length(palette)
			c = palette.colors[i]
			if !is_fully_opaque(c) && (alpha_index == 0 || c.a < palette.colors[alpha_index].a)
				alpha_index = i
			end
		end
		alpha_index > 0 && pal_swap!(palette, length(palette), alpha_index)
	else
		num_transparent = findlast(c -> !is_fully_opaque(c), palette.colors)
		if num_transparent !== nothing
			verbose_print(attr, "  eliminated opaque tRNS-chunk entries...$num_transparent entr$(num_transparent == 1 ? "y" : "ies") transparent")
		end
	end
end

# Repeats mediancut with different histogram weights to find the palette with minimum error.
# `feedback_loop_trials` controls how long the search takes.
function find_best_palette(attr::LiqAttr, target_mse::Float64, target_mse_is_zero::Bool, max_mse::Union{Nothing,Float64},
                           hist::HistogramInternal)
	# hist.items includes the fixed colours already
	few_input_colors = length(hist.items) <= attr.max_colors
	# the target_mse passed in has an extra diff from posterization
	if few_input_colors && target_mse_is_zero
		return palette_from_histogram(hist, attr.max_colors)
	end

	max_colors = attr.max_colors
	total_trials = feedback_loop_trials(attr, length(hist.items))
	trials_left = total_trials
	best_palette = nothing
	target_mse_overshoot = total_trials > 0 ? 1.05 : 1.0
	fails_in_a_row = 0
	palette_error = nothing
	local pal::PalF
	while true
		max_mse_per_color = max(target_mse, something(palette_error, quality_to_mse(1)), quality_to_mse(51)) * 1.2
		new_palette = with_fixed_colors(mediancut(hist, max_colors, target_mse * target_mse_overshoot, max_mse_per_color),
		                                attr.max_colors, hist.fixed_colors)
		stage_done = 1.0 - (max(trials_left, 0) / (total_trials + 1))^2
		verbose_print(attr, "  selecting colors...$(floor(Int, 100 * stage_done))%")

		if trials_left <= 0
			pal = new_palette
			break
		end

		first_run_of_target_mse = best_palette === nothing && target_mse > 0
		total_error = kmeans_iteration!(hist, new_palette, !first_run_of_target_mse)
		if best_palette === nothing || total_error < something(palette_error, floatmax(Float64)) ||
		   (total_error <= target_mse && length(new_palette) < max_colors)
			if total_error < target_mse && total_error > 0
				# if the number of colours could be reduced, try to keep it that way
				target_mse_overshoot = min(target_mse_overshoot * 1.25, target_mse / total_error)
			end
			palette_error = total_error
			max_colors = min(max_colors, length(new_palette) + 1)
			trials_left -= 1
			fails_in_a_row = 0
			best_palette = new_palette
		else
			fails_in_a_row += 1
			target_mse_overshoot = 1.0
			trials_left -= 5 + fails_in_a_row
		end
		if trials_left <= 0
			pal = best_palette::PalF
			break
		end
	end

	palette_error = refine_palette!(pal, attr, hist, max_mse, palette_error)
	return pal, palette_error
end

function refine_palette!(palette::PalF, attr::LiqAttr, hist::HistogramInternal, max_mse::Union{Nothing,Float64},
                         palette_error::Union{Nothing,Float64})
	iterations, iteration_limit = kmeans_iterations(attr, length(hist.items), palette_error !== nothing)
	if iterations > 0
		verbose_print(attr, "  moving colormap towards local minimum")
		i = 0
		while i < iterations
			pal_err = kmeans_iteration!(hist, palette, false)
			previous_palette_error = palette_error
			palette_error = pal_err
			if previous_palette_error !== nothing && abs(previous_palette_error - pal_err) < iteration_limit
				break
			end
			i += pal_err > something(max_mse, 1e20) * 1.5 ? 2 : 1
		end
	end
	return palette_error
end

function palette_from_histogram(hist::HistogramInternal, max_colors::Int)
	p = PalF()
	for item in hist.items
		pal_push!(p, item.color, item.perceptual_weight)
	end
	return with_fixed_colors(p, max_colors, hist.fixed_colors), 0.0
end
