# nearest.jl — nearest.rs: vantage-point tree over the palette, for nearest-colour search.
# Palette indices are 1-based here.

const LEAF_MAX_SIZE = 6

struct VPNode
	vantage_point::FPixel
	idx::Int
	# inner node (near/far !== nothing) …
	radius::Float32
	radius_squared::Float32
	near::Union{Nothing,VPNode}
	far::Union{Nothing,VPNode}
	# … or leaf
	leaf_idxs::Vector{Int}
	leaf_colors::Vector{FPixel}
end

struct Nearest
	root::VPNode
	colors::Vector{FPixel}
	nearest_other_color_dist::Vector{Float32}
end

mutable struct Visitor
	distance::Float32
	distance_squared::Float32
	idx::Int
	exclude::Int                  # 0 = none
end

@inline function visit!(v::Visitor, distance::Float32, distance_squared::Float32, idx::Int)
	if distance_squared < v.distance_squared && v.exclude != idx
		v.distance = distance
		v.distance_squared = distance_squared
		v.idx = idx
	end
end

function Nearest(palette::PalF)
	n = length(palette)
	(n == 0 || n > MAX_COLORS) && throw(LiqError(:Unsupported, "palette size $n"))
	root = vp_create_node(collect(1:n), palette)
	nocd = Vector{Float32}(undef, n)
	for i in 1:n
		best = Visitor(floatmax(Float32), floatmax(Float32), 1, i)
		vp_search_node(root, palette.colors[i], best)
		nocd[i] = best.distance_squared / 4f0
	end
	return Nearest(root, palette.colors, nocd)
end

# -> (index, squared distance)
@inline function nearest_search(n::Nearest, px::FPixel, likely::Int)
	if 1 <= likely <= length(n.colors)
		guess_diff = pxdiff(px, @inbounds n.colors[likely])
		guess_diff < @inbounds(n.nearest_other_color_dist[likely]) && return likely, guess_diff
		best = Visitor(sqrt(guess_diff), guess_diff, likely, 0)
	else
		best = Visitor(Inf32, Inf32, 1, 0)
	end
	vp_search_node(n.root, px, best)
	return best.idx, best.distance_squared
end

function vp_create_node(indexes::Vector{Int}, palette::PalF)::VPNode
	colors = palette.colors
	if length(indexes) <= 1
		idx = isempty(indexes) ? 1 : indexes[1]
		return VPNode(colors[idx], idx, 0f0, 0f0, nothing, nothing, Int[], FPixel[])
	end
	# most popular item (max_by_key: the LAST of equal maxima)
	mp = 1
	for k in 2:length(indexes)
		popularity(palette.pops[indexes[k]]) >= popularity(palette.pops[indexes[mp]]) && (mp = k)
	end
	indexes[1], indexes[mp] = indexes[mp], indexes[1]
	ref = indexes[1]
	rest = indexes[2:end]
	vp = colors[ref]
	sort!(rest; by=i -> pxdiff(vp, colors[i]), alg=MergeSort)     # stable, like sort_by_cached_key
	num = length(rest)
	if num <= LEAF_MAX_SIZE
		return VPNode(vp, ref, 0f0, 0f0, nothing, nothing, rest, FPixel[colors[i] for i in rest])
	end
	half = num ÷ 2
	near = rest[1:half]
	far = rest[half+1:end]
	radius_squared = pxdiff(vp, colors[far[1]])
	return VPNode(vp, ref, sqrt(radius_squared), radius_squared, vp_create_node(near, palette),
	              vp_create_node(far, palette), Int[], FPixel[])
end

function vp_search_node(node::VPNode, needle::FPixel, best::Visitor)
	while true
		distance_squared = pxdiff(node.vantage_point, needle)
		distance = sqrt(distance_squared)
		visit!(best, distance, distance_squared, node.idx)
		if node.near !== nothing
			# recurse towards the most likely candidate first, to narrow best.distance soon
			if distance_squared < node.radius_squared
				vp_search_node(node.near::VPNode, needle, best)
				# the answer may be just outside the radius, but no farther than the best distance so far
				if distance >= node.radius - best.distance
					node = node.far::VPNode
					continue
				end
			else
				vp_search_node(node.far::VPNode, needle, best)
				if distance <= node.radius + best.distance
					node = node.near::VPNode
					continue
				end
			end
			break
		else
			@inbounds for k in eachindex(node.leaf_idxs)
				ds = pxdiff(node.leaf_colors[k], needle)
				visit!(best, sqrt(ds), ds, node.leaf_idxs[k])
			end
			break
		end
	end
end
