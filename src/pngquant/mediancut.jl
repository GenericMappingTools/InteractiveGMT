# mediancut.jl — mediancut.rs: splits the histogram into boxes, box with the largest weighted
# variance first, until there are enough colours or the error is low enough.
# A box is a 1-based index range lo:hi into `hist.items`; splitting partitions it in place.

mutable struct MBox
	lo::Int
	hi::Int
	avg_color::FPixel
	variance::FPixel                  # per channel, weighted with adjusted_weight
	adjusted_weight_sum::Float64
	total_error::Float64              # NaN = not computed yet
	max_error::Float32                # max colour difference between avg_color and any entry
end
Base.length(b::MBox) = b.hi - b.lo + 1

function weighed_average_color(items::Vector{HistItem}, lo::Int, hi::Int)::FPixel
	t = FPixel()
	s = 0f0
	@inbounds for k in lo:hi
		c = items[k]
		s += c.adjusted_weight
		t += c.color * c.adjusted_weight
	end
	s != 0f0 && (t = t / s)
	return t
end

function MBox(items::Vector{HistItem}, lo::Int, hi::Int, adjusted_weight_sum::Float64)
	avg = weighed_average_color(items, lo, hi)
	variance = FPixel()
	max_error = 0f0
	@inbounds for k in lo:hi
		it = items[k]
		d = avg - it.color
		variance += FPixel(d.a * d.a, d.r * d.r, d.g * d.g, d.b * d.b) * it.adjusted_weight
		diff = pxdiff(avg, it.color)
		diff > max_error && (max_error = diff)
	end
	return MBox(lo, hi, avg, variance, adjusted_weight_sum, NaN, max_error)
end

function MBox(items::Vector{HistItem}, lo::Int, hi::Int)
	ws = 0.0
	@inbounds for k in lo:hi
		ws += Float64(items[k].adjusted_weight)
	end
	return MBox(items, lo, hi, ws)
end

function compute_total_error!(b::MBox, items::Vector{HistItem})::Float64
	e = 0.0
	@inbounds for k in b.lo:b.hi
		e += Float64(pxdiff(b.avg_color, items[k].color)) * Float64(items[k].perceptual_weight)
	end
	b.total_error = e
	return e
end

@inline _chan(p::FPixel, c::Int) = c == 1 ? p.a : c == 2 ? p.r : c == 3 ? p.g : p.b

function prepare_sort!(b::MBox, items::Vector{HistItem})
	# sort dimensions by their variance, then colours first by the dimension with the highest one
	vars = (b.variance.a, b.variance.r, b.variance.g, b.variance.b)
	ch = sort!([1, 2, 3, 4]; by=c -> vars[c], rev=true)
	@inbounds for k in b.lo:b.hi
		c = items[k].color
		# only the first channel really matters; the others keep sort randomness from influencing
		# the outcome when median cut is repeated with different weights
		v = UInt32(_sat_u16(_chan(c, ch[1]) * 65535f0)) << 16 |
		    UInt32(_sat_u16((_chan(c, ch[3]) + _chan(c, ch[2]) / 2f0 + _chan(c, ch[4]) / 4f0) * 65535f0))
		items[k] = with_sortval(items[k], v)
	end
end

function median_color!(b::MBox, items::Vector{HistItem})::FPixel
	v = view(items, b.lo:b.hi)
	mid = length(v) ÷ 2 + 1
	partialsort!(v, mid; by=h -> h.sortval)
	return v[mid].color
end

function prepare_color_weight_total!(b::MBox, items::Vector{HistItem})::Float64
	median = median_color!(b, items)
	s = 0.0
	@inbounds for k in b.lo:b.hi
		a = items[k]
		w = sqrt(sqrt(pxdiff(median, a.color)) * (2f0 + a.adjusted_weight))
		items[k] = with_mc_weight(a, w)
		s += Float64(w)
	end
	return s
end

function split_box(b::MBox, items::Vector{HistItem})
	prepare_sort!(b, items)
	half_weight = prepare_color_weight_total!(b, items) / 2.0
	# yeah, there's some off-by-one error in there (upstream's words)
	break_at = max(hist_item_sort_half!(items, b.lo, b.hi, half_weight), 1)
	left_sum = 0.0
	@inbounds for k in b.lo:b.lo+break_at-1
		left_sum += Float64(items[k].adjusted_weight)
	end
	right_sum = b.adjusted_weight_sum - left_sum
	return MBox(items, b.lo, b.lo + break_at - 1, left_sum), MBox(items, b.lo + break_at, b.hi, right_sum)
end

# qsort helpers over items[lo:hi]; positions p are 0-based within that range, as in the Rust
@inline _sv(items, lo, p) = @inbounds items[lo+p].sortval
@inline function _swap!(items, lo, p, q)
	@inbounds items[lo+p], items[lo+q] = items[lo+q], items[lo+p]
end

function qsort_pivot(items::Vector{HistItem}, lo::Int, len::Int)::Int
	len < 32 && return len ÷ 2
	pv = sort!([8, len ÷ 2, len - 1]; by=p -> _sv(items, lo, p))
	return pv[2]
end

# Partitions DESCENDING around a pivot; returns the pivot's final position.
function qsort_partition!(items::Vector{HistItem}, lo::Int, len::Int)::Int
	r = len
	_swap!(items, lo, qsort_pivot(items, lo, len), 0)
	pivot_value = _sv(items, lo, 0)
	l = 1
	while l < r
		if _sv(items, lo, l) >= pivot_value
			l += 1
		else
			r -= 1
			while l < r && _sv(items, lo, r) <= pivot_value
				r -= 1
			end
			_swap!(items, lo, l, r)
		end
	end
	l -= 1
	_swap!(items, lo, l, 0)
	return l
end

# Sorts items[lo:hi] so the sum of mc_color_weight on one side is below `weight_half_sum`; returns
# the count of items on that side.
function hist_item_sort_half!(items::Vector{HistItem}, lo::Int, hi::Int, weight_half_sum::Float64)::Int
	base_index = 0
	len = hi - lo + 1
	len == 0 && return 0
	while true
		partition = qsort_partition!(items, lo, len)
		nleft = partition + 1              # pivot stays on the left side
		left_sum = 0.0
		@inbounds for k in lo:lo+nleft-1
			left_sum += Float64(items[k].mc_color_weight)
		end
		if left_sum >= weight_half_sum
			partition > 0 || return base_index
			len = partition                 # trim the pivot point
			continue
		end
		weight_half_sum -= left_sum
		base_index += nleft
		nleft < len || return base_index
		lo += nleft
		len -= nleft
	end
end

function total_box_error_below_target!(boxes::Vector{MBox}, items::Vector{HistItem}, target_mse::Float64, total_pw::Float64)::Bool
	target_mse *= total_pw
	total_error = 0.0
	for b in boxes
		isnan(b.total_error) || (total_error += b.total_error)
	end
	total_error > target_mse && return false
	for b in boxes
		if isnan(b.total_error)
			total_error += compute_total_error!(b, items)
			total_error > target_mse && return false
		end
	end
	return true
end

function take_best_splittable_box!(boxes::Vector{MBox}, max_mse::Float64)::Union{Nothing,MBox}
	best = 0
	best_sum = -Inf
	for (i, b) in enumerate(boxes)
		length(b) > 1 || continue
		thissum = b.adjusted_weight_sum * (Float64(b.variance.a) + Float64(b.variance.r) + Float64(b.variance.g) + Float64(b.variance.b))
		if Float64(b.max_error) > max_mse
			thissum = thissum * Float64(b.max_error) / max_mse
		end
		if thissum >= best_sum            # max_by_key: the LAST of equal maxima
			best_sum = thissum; best = i
		end
	end
	best == 0 && return nothing
	b = boxes[best]
	boxes[best] = boxes[end]                # swap_remove
	pop!(boxes)
	return b
end

function mediancut(hist::HistogramInternal, target_colors::Int, target_mse::Float64, max_mse_per_color::Float64)::PalF
	items = hist.items
	boxes = MBox[]
	sizehint!(boxes, target_colors)
	used = filter(!isempty, hist.clusters)
	if length(used) <= target_colors ÷ 3
		for c in used
			push!(boxes, MBox(items, first(c), last(c)))
		end
	else
		push!(boxes, MBox(items, 1, length(items)))
	end

	max_mse = max(max_mse_per_color, quality_to_mse(20))
	while length(boxes) < target_colors
		# first split boxes that exceed the quality limit (colours for things like an odd green pixel),
		# later raise the limit so large smooth areas/gradients get colours
		fraction_done = length(boxes) / target_colors
		current_max_mse = fma(fraction_done * 16.0, max_mse, max_mse)
		bi = take_best_splittable_box!(boxes, current_max_mse)
		bi === nothing && break
		l, r = split_box(bi, items)
		push!(boxes, l, r)
		total_box_error_below_target!(boxes, items, target_mse, hist.total_perceptual_weight) && break
	end

	palette = PalF()
	for (pal_index, b) in enumerate(boxes)
		pop = 0.0
		@inbounds for k in b.lo:b.hi
			items[k] = with_palette_index(items[k], pal_index)
			pop += Float64(items[k].perceptual_weight)
		end
		# store total colour popularity (perceptual_weight is an approximation of it)
		rep = b.avg_color
		if length(b) > 2
			best = Inf32; bc = FPixel()
			@inbounds for k in b.lo:b.hi
				d = pxdiff(rep, items[k].color)
				if d < best
					best = d; bc = items[k].color
				end
			end
			rep = bc
		end
		pal_push!(palette, rep, Float32(pop))
	end
	return palette
end
