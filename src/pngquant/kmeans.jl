# kmeans.jl — kmeans.rs: one K-Means iteration — each palette colour becomes the weighted average
# of the histogram colours that map best to it. (Rust splits the work over threads and merges the
# per-thread sums; here it is one accumulator.)

struct Kmeans
	sum_a::Vector{Float64}
	sum_r::Vector{Float64}
	sum_g::Vector{Float64}
	sum_b::Vector{Float64}
	total::Vector{Float64}
	weighed_diff_sum::Base.RefValue{Float64}
end
Kmeans(n::Int) = Kmeans(zeros(n), zeros(n), zeros(n), zeros(n), zeros(n), Ref(0.0))

@inline function update_color!(km::Kmeans, px::FPixel, value::Float32, matched::Int)
	p = px * value
	@inbounds begin
		km.sum_a[matched] += Float64(p.a)
		km.sum_r[matched] += Float64(p.r)
		km.sum_g[matched] += Float64(p.g)
		km.sum_b[matched] += Float64(p.b)
		km.total[matched] += Float64(value)
	end
end

function finalize!(km::Kmeans, palette::PalF)::Float64
	for i in 1:length(palette)
		pop_is_fixed(palette.pops[i]) && continue
		total = km.total[i]
		palette.pops[i] = Float32(total)
		if total > 0 && palette.colors[i].a != 0f0
			palette.colors[i] = FPixel(Float32(km.sum_a[i] / total), Float32(km.sum_r[i] / total),
			                           Float32(km.sum_g[i] / total), Float32(km.sum_b[i] / total))
		end
	end
	return km.weighed_diff_sum[]
end

function kmeans_iteration!(hist::HistogramInternal, palette::PalF, adjust_weight::Bool)::Float64
	isempty(hist.items) && return 0.0
	n = Nearest(palette)
	colors = palette.colors
	km = Kmeans(length(palette))
	items = hist.items
	s = 0.0
	@inbounds for k in eachindex(items)
		item = items[k]
		px = item.color
		matched, diff = nearest_search(n, px, likely_palette_index(item))
		item = with_palette_index(item, matched)
		if adjust_weight
			remapped = colors[matched]
			_, diff = nearest_search(n, px + px - remapped, matched)
			item = with_adjusted_weight(item, fma(2f0, item.adjusted_weight, item.perceptual_weight) * (0.5f0 + diff))
		end
		items[k] = item
		update_color!(km, px, item.adjusted_weight, matched)
		s += Float64(diff * item.perceptual_weight)
	end
	km.weighed_diff_sum[] = s
	d = finalize!(km, palette) / hist.total_perceptual_weight
	replace_unused_colors!(palette, hist)
	return d
end

# K-Means may have merged or obsoleted some palette entries: replace them with the histogram colours
# that currently fit the palette worst.
function replace_unused_colors!(palette::PalF, hist::HistogramInternal)
	for pal_idx in 1:length(palette)
		pop = palette.pops[pal_idx]
		if popularity(pop) == 0f0 && !pop_is_fixed(pop)
			n = Nearest(palette)
			worst = 0
			worst_diff = 0f0
			colors = palette.colors
			# diff only, ignoring adjusted_weight: the palette already optimizes for the max weight,
			# so it would likely find another redundant entry
			@inbounds for k in eachindex(hist.items)
				item = hist.items[k]
				li = likely_palette_index(item)
				# the early reject avoids a full palette search for every entry
				may_be_worst = li > length(colors) || pxdiff(colors[li], item.color) > worst_diff
				if may_be_worst
					diff = nearest_search(n, item.color, li)[2]
					if diff > worst_diff
						worst_diff = diff; worst = k
					end
				end
			end
			if worst > 0
				pal_set!(palette, pal_idx, hist.items[worst].color, hist.items[worst].adjusted_weight)
			end
		end
	end
end
