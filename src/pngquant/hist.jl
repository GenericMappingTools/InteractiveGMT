# hist.jl — hist.rs: counting the colours of the image(s), then turning the counts into the
# weighted, cluster-grouped item list that median cut works on.

"""
    LiqHistogram(attr)

Collects colour statistics from one or more images (`add_image!`, `add_colors!`,
`add_fixed_color!`) to make ONE palette shared by all of them (`quantize`).
"""
mutable struct LiqHistogram
	gamma::Union{Nothing,Float64}
	fixed_colors::Vector{RGBA}                   # insertion order == the Rust set's `index`
	# key: the RGBA as a little-endian u32 (posterized); value: (boosted count, first colour seen)
	hashmap::Dict{UInt32,Tuple{UInt32,RGBA}}
	posterize_bits::Int
	max_histogram_entries::Int
end

LiqHistogram(attr::LiqAttr) = LiqHistogram(nothing, RGBA[], Dict{UInt32,Tuple{UInt32,RGBA}}(), posterize_bits(attr),
                                           attr.max_histogram_entries)

# One histogram entry. `sortval` is the Rust union: the median-cut sort key during median cut,
# the likely palette index (low byte, 0-based) afterwards.
struct HistItem
	color::FPixel
	adjusted_weight::Float32
	perceptual_weight::Float32
	mc_color_weight::Float32
	sortval::UInt32
end
@inline likely_palette_index(h::HistItem) = Int(h.sortval & 0xff) + 1          # 1-based
@inline with_palette_index(h::HistItem, i::Int) = HistItem(h.color, h.adjusted_weight, h.perceptual_weight, h.mc_color_weight, UInt32(i - 1))
@inline with_sortval(h::HistItem, v::UInt32) = HistItem(h.color, h.adjusted_weight, h.perceptual_weight, h.mc_color_weight, v)
@inline with_mc_weight(h::HistItem, w::Float32) = HistItem(h.color, h.adjusted_weight, h.perceptual_weight, w, h.sortval)
@inline with_adjusted_weight(h::HistItem, w::Float32) = HistItem(h.color, w, h.perceptual_weight, h.mc_color_weight, h.sortval)

# Clusters form the initial boxes for quantization, so that extreme colours are better represented
const LIQ_MAXCLUSTER = 16

mutable struct HistogramInternal
	items::Vector{HistItem}
	total_perceptual_weight::Float64
	clusters::Vector{UnitRange{Int}}             # 1-based item ranges, one per cluster
	fixed_colors::Vector{FPixel}
end

@inline rgba_int(c::RGBA) = UInt32(c.r) | UInt32(c.g) << 8 | UInt32(c.b) << 16 | UInt32(c.a) << 24

@inline function posterize_mask(bits::Int)::UInt32
	m = UInt32((255 << bits) & 0xff)
	return m | m << 8 | m << 16 | m << 24
end

"""
    add_image!(hist, attr, img)

"Learns" the colours of `img`. Its fixed colours are added to the histogram too.
"""
function add_image!(hist::LiqHistogram, attr::LiqAttr, img::LiqImage)
	if img.importance_map === nothing && attr.use_contrast_maps
		contrast_maps!(img)
	end
	hist.gamma = img.gamma
	append!(hist.fixed_colors, img.fixed_colors)
	pb = posterize_bits(attr)
	surface_area = img.height * img.width
	estimated_colors = min(surface_area ÷ (pb + (surface_area > 512 * 512 ? 7 : 5)), 250_000)
	sizehint!(hist.hashmap, length(hist.hashmap) + max(estimated_colors - length(hist.hashmap) ÷ 3, 0))
	add_pixel_rows!(hist, img.px, img.importance_map, pb)
	return hist
end

"""
    add_colors!(hist, colors::Vector{RGBA}, counts::Vector{<:Integer}; gamma=0.0)

Alternative to `add_image!` when the image's histogram is already known.
"""
function add_colors!(hist::LiqHistogram, colors::Vector{RGBA}, counts::Vector{<:Integer}; gamma::Real=0.0)
	(isempty(colors) || length(colors) > 1 << 24 || length(counts) != length(colors)) && throw(LiqError(:ValueOutOfRange, "bad color list"))
	0.0 <= gamma < 1.0 || throw(LiqError(:ValueOutOfRange, "gamma must be 0-1"))
	(hist.gamma === nothing && gamma > 0) && (hist.gamma = Float64(gamma))
	for k in eachindex(colors)
		add_color!(hist, colors[k], UInt32(counts[k]))
	end
	return hist
end

"""
    add_fixed_color!(hist, c::RGBA; gamma=0.0)

A colour guaranteed to be in the final palette.
"""
function add_fixed_color!(hist::LiqHistogram, c::RGBA; gamma::Real=0.0)
	length(hist.fixed_colors) >= MAX_COLORS && throw(LiqError(:Unsupported, "too many fixed colors"))
	(hist.gamma === nothing && gamma > 0) && (hist.gamma = Float64(gamma))
	push!(hist.fixed_colors, c)
	return hist
end

@inline function add_color!(hist::LiqHistogram, c::RGBA, boost::UInt32)
	boost == 0 && return
	k = c.a != 0 ? posterize_mask(hist.posterize_bits) & rgba_int(c) : UInt32(0)
	e = get(hist.hashmap, k, nothing)
	if e === nothing
		hist.hashmap[k] = (boost, c)
	else
		s = e[1] + boost
		hist.hashmap[k] = (s < e[1] ? typemax(UInt32) : s, e[2])      # saturating: images over 2^24 px
	end
	return
end

function init_posterize_bits!(hist::LiqHistogram, bits::Int)
	hist.posterize_bits >= bits && return
	hist.posterize_bits = bits
	m = posterize_mask(bits)
	old = hist.hashmap
	hist.hashmap = Dict{UInt32,Tuple{UInt32,RGBA}}()
	sizehint!(hist.hashmap, length(old) ÷ 3)
	for (k, v) in old
		hist.hashmap[k & m] = v
	end
end

function add_pixel_rows!(hist::LiqHistogram, px::Vector{RGBA}, importance_map::Union{Nothing,Vector{UInt8}}, pb::Int)
	if importance_map === nothing
		@inbounds for p in px
			add_color!(hist, p, UInt32(255))
		end
	else
		@inbounds for i in eachindex(px)
			add_color!(hist, px[i], UInt32(importance_map[i]))
		end
	end
	init_posterize_bits!(hist, pb)
	if length(hist.hashmap) > hist.max_histogram_entries && hist.posterize_bits < 3
		init_posterize_bits!(hist, hist.posterize_bits + 1)
	end
end

function finalize_builder(hist::LiqHistogram, gamma::Float64)::HistogramInternal
	# fixed colours go into the normal hashmap, with a temporary 0 meaning "fixed max weight"
	for c in hist.fixed_colors
		hist.hashmap[c.a != 0 ? rgba_int(c) : UInt32(0)] = (UInt32(0), c)
	end

	n = length(hist.hashmap)
	temp_color = Vector{RGBA}(undef, n)
	temp_weight = Vector{Float32}(undef, n)
	temp_cluster = Vector{Int}(undef, n)
	counts = zeros(Int, LIQ_MAXCLUSTER)
	k = 0
	for (boost, c) in values(hist.hashmap)
		k += 1
		ci = ((c.r >> 7) << 3) | ((c.g >> 7) << 2) | ((c.b >> 7) << 1) | (c.a >> 7)
		counts[ci+1] += 1
		temp_color[k] = c
		temp_weight[k] = Float32(boost)            # fixed colours give weight 0
		temp_cluster[k] = ci + 1
	end

	next_free = zeros(Int, LIQ_MAXCLUSTER)
	clusters = Vector{UnitRange{Int}}(undef, LIQ_MAXCLUSTER)
	b = 1
	for ci in 1:LIQ_MAXCLUSTER
		clusters[ci] = b:b+counts[ci]-1
		next_free[ci] = b
		b += counts[ci]
	end

	items = Vector{HistItem}(undef, n)
	# Limit perceptual weight to 1/10th of the image surface area, so no single colour dominates
	max_perceptual_weight = Float32((0.1 / 255.0) * sum(Float64, temp_weight; init=0.0))

	lut = gamma_lut(gamma)
	total_perceptual_weight = 0.0
	for k in 1:n
		ci = temp_cluster[k]
		idx = next_free[ci]
		next_free[ci] += 1
		w = temp_weight[k] > 0f0 ? min(temp_weight[k] * (1f0 / 255f0), max_perceptual_weight) : max_perceptual_weight * 16f0
		total_perceptual_weight += Float64(w)
		items[idx] = HistItem(from_rgba(lut, temp_color[k]), w, w, 0f0, UInt32(0))
	end

	fixed = FPixel[from_rgba(lut, c) for c in hist.fixed_colors]
	return HistogramInternal(items, total_perceptual_weight, clusters, fixed)
end
