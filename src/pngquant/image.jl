# image.jl — image.rs, rows.rs (the pixel rows and their float conversion) and blur.rs.
#
# Pixels are held row-major, top row first: pixel (row, col) — both 1-based — is px[(row-1)*width + col].
# rows.rs streams rows through a callback to save memory on huge images; here the float copy is
# always made (`fpixels`), which is what rows.rs does for anything under LIQ_HIGH_MEMORY_LIMIT.
# Not ported: `set_background` (GIF "keep" frames) and row callbacks.

"""
    LiqImage(attr, pixels::Vector{RGBA}, width, height; gamma=0.0)
    LiqImage(attr, A::Array{UInt8,3}; gamma=0.0)

An image to quantize. `pixels` is row-major, top row first. `A` is (height, width, nbands) with
nbands 1 (grey), 2 (grey+alpha), 3 (RGB) or 4 (RGBA), A[1,1,:] = top-left. `gamma` 0 means sRGB.
"""
mutable struct LiqImage
	width::Int
	height::Int
	px::Vector{RGBA}
	gamma::Float64
	fpx::Union{Nothing,Vector{FPixel}}
	importance_map::Union{Nothing,Vector{UInt8}}
	edges::Union{Nothing,Vector{UInt8}}
	dither_map::Union{Nothing,Vector{UInt8}}
	fixed_colors::Vector{RGBA}
end

function LiqImage(attr::LiqAttr, pixels::Vector{RGBA}, width::Integer, height::Integer; gamma::Real=0.0)
	(width > 0 && height > 0) || throw(LiqError(:ValueOutOfRange, "empty image"))
	length(pixels) >= width * height || throw(LiqError(:BufferTooSmall, "need $(width)x$(height) pixels, got $(length(pixels))"))
	if !(0.0 <= gamma <= 1.0)
		verbose_print(attr, "  error: gamma must be >= 0 and <= 1 (try 1/gamma instead)")
		throw(LiqError(:ValueOutOfRange, "gamma must be 0-1"))
	end
	return LiqImage(Int(width), Int(height), pixels, gamma > 0 ? Float64(gamma) : 0.45455, nothing, nothing,
	                nothing, nothing, RGBA[])
end

function LiqImage(attr::LiqAttr, A::Array{UInt8,3}; gamma::Real=0.0)
	h, w, nb = size(A)
	px = Vector{RGBA}(undef, w * h)
	@inbounds for r in 1:h, c in 1:w
		px[(r-1)*w+c] = nb == 1 ? RGBA(A[r,c,1], A[r,c,1], A[r,c,1], 0xff) :
		                nb == 2 ? RGBA(A[r,c,1], A[r,c,1], A[r,c,1], A[r,c,2]) :
		                nb == 3 ? RGBA(A[r,c,1], A[r,c,2], A[r,c,3], 0xff) :
		                          RGBA(A[r,c,1], A[r,c,2], A[r,c,3], A[r,c,4])
	end
	return LiqImage(attr, px, w, h; gamma=gamma)
end
LiqImage(attr::LiqAttr, A::Matrix{UInt8}; gamma::Real=0.0) = LiqImage(attr, reshape(A, size(A)..., 1); gamma=gamma)

function fpixels(img::LiqImage)::Vector{FPixel}
	img.fpx === nothing || return img.fpx
	lut = gamma_lut(img.gamma)
	img.fpx = FPixel[from_rgba(lut, p) for p in img.px]
	return img.fpx
end

"""
    set_importance_map!(img, map::Vector{UInt8})

Which pixels matter more (higher = more likely to get a palette entry). Row-major, width*height.
"""
function set_importance_map!(img::LiqImage, map::Vector{UInt8})
	length(map) == img.width * img.height || throw(LiqError(:BufferTooSmall, "importance map must be width*height"))
	img.importance_map = map
	return img
end

"""
    add_fixed_color!(img, c::RGBA)

Reserve a colour in the output palette, as if it were used in the image and very important.
"""
function add_fixed_color!(img::LiqImage, c::RGBA)
	length(img.fixed_colors) >= MAX_COLORS && throw(LiqError(:Unsupported, "too many fixed colors"))
	push!(img.fixed_colors, c)
	return img
end

# Builds two maps:
#   importance_map — approximation of areas with high-frequency noise, except straight edges. 1=flat, 0=noisy.
#   edges          — noise map including all edges
function contrast_maps!(img::LiqImage)
	width, height = img.width, img.height
	(width < 4 || height < 4 || 3 * width * height > LIQ_HIGH_MEMORY_LIMIT) && return   # shrug
	img.importance_map === nothing && (img.importance_map = zeros(UInt8, width * height))
	img.edges === nothing && (img.edges = zeros(UInt8, width * height))
	noise = img.importance_map::Vector{UInt8}
	edges = img.edges::Vector{UInt8}
	f = fpixels(img)
	chmax(p::FPixel) = max(max(p.a, p.r), max(p.g, p.b))
	fabs(p::FPixel) = FPixel(abs(p.a), abs(p.r), abs(p.g), abs(p.b))
	next_row = 0; curr_row = 0          # 0-based row starts
	@inbounds for j in 0:height-1
		prev_row = curr_row
		curr_row = next_row
		next_row = j + 1 < height ? j + 1 : next_row
		po = prev_row * width; co = curr_row * width; no = next_row * width
		curr = f[co+1]
		nxt = curr
		for i in 0:width-1
			prv = curr
			curr = nxt
			nxt = f[co+min(i + 1, width - 1)+1]
			# contrast is the difference between neighbours horizontally and vertically
			horiz = chmax(fabs(prv + nxt - curr * 2f0))     # noise is amplified
			vert  = chmax(fabs(f[po+i+1] + f[no+i+1] - curr * 2f0))
			edge = max(horiz, vert)
			z = fma(abs(horiz - vert), -0.5f0, edge)
			z = 1f0 - max(z, min(horiz, vert))
			z *= z
			z *= z
			# 85 is about 1/3rd of weight (not 0: noisy pixels still count, just not as precisely)
			noise[j*width+i+1] = _sat_u8(fma(z, 176f0, 80f0))
			edges[j*width+i+1] = _sat_u8((1f0 - edge) * 256f0)
		end
	end
	# noise areas are shrunk and then expanded to remove thin edges from the map
	tmp = zeros(UInt8, width * height)
	liq_max3(noise, tmp, width, height)
	liq_max3(tmp, noise, width, height)
	liq_blur(noise, tmp, width, height, 3)
	liq_max3(noise, tmp, width, height)
	liq_min3(tmp, noise, width, height)
	liq_min3(noise, tmp, width, height)
	liq_min3(tmp, noise, width, height)
	liq_min3(edges, tmp, width, height)
	liq_max3(tmp, edges, width, height)
	@inbounds for k in eachindex(edges)
		edges[k] = min(noise[k], edges[k])
	end
	return
end

# Turns `edges` into the dither map: pixels in runs of one palette index that also continue above
# and below are flat areas, and get dithered.
function update_dither_map!(img::LiqImage, remapped::Vector{UInt8}, palette::PalF)
	img.edges === nothing && contrast_maps!(img)
	img.edges === nothing && return
	edges = img.edges::Vector{UInt8}
	img.edges = nothing
	width, height = img.width, img.height
	@inbounds for r in 0:height-1
		ro = r * width
		lastpixel = remapped[ro+1]
		lastcol = 0
		for col in 1:width-1
			px = remapped[ro+col+1]
			if px != lastpixel || col == width - 1
				neighbor_count = 10 * (col - lastcol)
				for i in lastcol:col-1
					r > 0 && remapped[ro-width+i+1] == lastpixel && (neighbor_count += 15)
					r < height - 1 && remapped[ro+width+i+1] == lastpixel && (neighbor_count += 15)
				end
				while lastcol <= col
					e = Float32(UInt16(edges[ro+lastcol+1]) + 128) * (255f0 / Float32(255 + 128)) *
					    (1f0 - 20f0 / Float32(20 + neighbor_count))
					edges[ro+lastcol+1] = _sat_u8(e)
					lastcol += 1
				end
				lastpixel = px
			end
		end
	end
	img.dither_map = edges
	return
end

# ---------------------------------------------------------------------------------------------
# blur.rs

# Blurs horizontally (width 2*size+1) and writes the result transposed to dst (twice = 2-D blur)
function transposing_1d_blur(src::Vector{UInt8}, dst::Vector{UInt8}, width::Int, height::Int, size::Int)
	(width < 2 * size + 1 || height < 2 * size + 1) && return
	@inbounds for j in 0:height-1
		ro = j * width
		row(k) = Int(src[ro+k+1])
		s = row(0) * size
		for k in 0:size-1
			s += row(k)
		end
		for i in 0:size-1
			s -= row(0)
			s += row(i + size)
			dst[i*height+j+1] = (s ÷ (size * 2)) % UInt8
		end
		for i in size:width-size-1
			s -= row(i - size)
			s += row(i + size)
			dst[i*height+j+1] = (s ÷ (size * 2)) % UInt8
		end
		for i in width-size:width-1
			s -= row(i - size)
			s += row(width - 1)
			dst[i*height+j+1] = (s ÷ (size * 2)) % UInt8
		end
	end
end

function liq_op3(op::F, src::Vector{UInt8}, dst::Vector{UInt8}, width::Int, height::Int) where {F}
	@inbounds for j in 0:height-1
		ro = j * width
		po = max(j - 1, 0) * width
		no = min(j + 1, height - 1) * width
		curr = src[ro+1]
		nxt = src[ro+1]
		for i in 0:width-2
			prv = curr
			curr = nxt
			nxt = src[ro+i+2]
			t1 = op(prv, nxt)
			t2 = op(src[no+i+1], src[po+i+1])
			dst[ro+i+1] = op(curr, op(t1, t2))
		end
		t1 = op(curr, nxt)
		t2 = op(src[no+width], src[po+width])
		dst[ro+width] = op(curr, op(t1, t2))
	end
end

# maximum of neighbouring pixels (blur + lighten)
liq_max3(src, dst, width, height) = liq_op3(max, src, dst, width, height)
# minimum of neighbouring pixels (blur + darken)
liq_min3(src, dst, width, height) = liq_op3(min, src, dst, width, height)

# Box blur of radius `size`, result back in src_dst; tmp is overwritten.
function liq_blur(src_dst::Vector{UInt8}, tmp::Vector{UInt8}, width::Int, height::Int, size::Int)
	transposing_1d_blur(src_dst, tmp, width, height, size)
	transposing_1d_blur(tmp, src_dst, height, width, size)
end
