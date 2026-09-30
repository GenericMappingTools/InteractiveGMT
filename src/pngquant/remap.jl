# remap.jl — remap.rs: nearest-colour remapping (which also does one last K-Means pass on the
# palette), and Floyd-Steinberg dithering steered by the dither map.
# `out` holds 0-based palette indices, row-major, top row first. Not ported: background images.
# Dithering runs as ONE chunk (the Rust's single-threaded path), so there are no chunk seams.

# -> mean remapping error (internal MSE)
function remap_to_palette!(img::LiqImage, out::Vector{UInt8}, palette::PalF)::Float64
	n = Nearest(palette)
	length(palette) > 256 && throw(LiqError(:Unsupported, "palette larger than 256"))
	km = Kmeans(length(palette))
	f = fpixels(img)
	imp = img.importance_map
	width = img.width
	remapping_error = 0.0
	@inbounds for r in 0:img.height-1
		last_match = 1
		for c in 1:width
			k = r * width + c
			inp = f[k]
			matched, diff = nearest_search(n, inp, last_match)
			last_match = matched
			remapping_error += Float64(diff)
			out[k] = UInt8(matched - 1)
			importance = imp === nothing ? 1f0 : Float32(imp[k])
			update_color!(km, inp, importance, matched)
		end
	end
	finalize!(km, palette)
	return remapping_error / (img.width * img.height)
end

function get_dithered_pixel(dither_level::Float32, max_dither_error::Float32, thiserr::FPixel, px::FPixel)::FPixel
	s = thiserr * dither_level
	# this prevents gaudy green pixels popping out of the blue (or red or black! ;)
	dither_error = fma(s.r, s.r, s.g * s.g) + fma(s.b, s.b, s.a * s.a)
	# don't dither areas that don't have noticeable error — makes the file smaller
	dither_error < 2f0 / 256f0 / 256f0 && return px

	ratio = 1f0
	MAX_OVERFLOW = 1.1f0
	MAX_UNDERFLOW = -0.1f0
	# allowing some overflow prevents undithered bands caused by clamping of all channels
	if px.r + s.r > MAX_OVERFLOW
		ratio = min(ratio, (MAX_OVERFLOW - px.r) / s.r)
	elseif px.r + s.r < MAX_UNDERFLOW
		ratio = min(ratio, (MAX_UNDERFLOW - px.r) / s.r)
	end
	if px.g + s.g > MAX_OVERFLOW
		ratio = min(ratio, (MAX_OVERFLOW - px.g) / s.g)
	elseif px.g + s.g < MAX_UNDERFLOW
		ratio = min(ratio, (MAX_UNDERFLOW - px.g) / s.g)
	end
	if px.b + s.b > MAX_OVERFLOW
		ratio = min(ratio, (MAX_OVERFLOW - px.b) / s.b)
	elseif px.b + s.b < MAX_UNDERFLOW
		ratio = min(ratio, (MAX_UNDERFLOW - px.b) / s.b)
	end
	dither_error > max_dither_error && (ratio *= 0.8f0)
	return FPixel(clamp(px.a + s.a, 0f0, 1f0), fma(s.r, ratio, px.r), fma(s.g, ratio, px.g), fma(s.b, ratio, px.b))
end

# Uses the edge/noise map to dither only flat areas: dithering on edges makes jagged lines, and noisy
# areas are "naturally" dithered. With `output_image_is_remapped`, `out` already holds an undithered
# remap, used as the search guess.
function remap_to_palette_floyd!(img::LiqImage, out::Vector{UInt8}, palette::PalF, res::LiqResult,
                                 max_dither_error::Float32, output_image_is_remapped::Bool)
	width, height = img.width, img.height
	dither_map = res.use_dither_map != DITHERMAP_NONE ?
	             something(img.dither_map, img.edges, UInt8[]) : UInt8[]
	n = Nearest(palette)
	colors = palette.colors
	# response to this value is non-linear: without it any value < 0.8 would give almost no dithering
	base_dithering_level = fma(1f0 - res.dither_level, -(1f0 - res.dither_level), 1f0) * (15f0 / 16f0)
	isempty(dither_map) || (base_dithering_level *= 1f0 / 255f0)       # dither_map is 0-255
	f = fpixels(img)
	errwidth = width + 2                 # +2 saves checking out-of-bounds access
	diffusion = fill(FPixel(), errwidth * 2)
	for r in 0:height-1
		dither_row!(f, r * width, out, width, dither_map, base_dithering_level, max_dither_error, n, colors,
		            output_image_is_remapped, diffusion, r & 1 == 0)
	end
end

function dither_row!(f::Vector{FPixel}, ro::Int, out::Vector{UInt8}, width::Int, dither_map::Vector{UInt8},
                     base_dithering_level::Float32, max_dither_error::Float32, n::Nearest, colors::Vector{FPixel},
                     guess_from_remapped_pixels::Bool, diffusion::Vector{FPixel}, even_row::Bool)
	# the two halves of `diffusion` swap roles every row; offsets are 0-based
	this0, next0 = even_row ? (0, width + 2) : (width + 2, 0)
	@inbounds for k in 1:width+2
		diffusion[next0+k] = FPixel()
	end
	has_map = !isempty(dither_map)
	last_match = 1
	@inbounds for x in 0:width-1
		col = even_row ? x : width - 1 - x
		t = this0 + col                  # thiserr[0] == diffusion[t+1]
		nx = next0 + col
		input_px = f[ro+col+1]
		dither_level = base_dithering_level
		has_map && (dither_level *= Float32(dither_map[ro+col+1]))
		spx = get_dithered_pixel(dither_level, max_dither_error, diffusion[t+2], input_px)
		guessed_match = guess_from_remapped_pixels ? Int(out[ro+col+1]) + 1 : last_match
		matched, _ = nearest_search(n, spx, guessed_match)
		last_match = matched
		output_px = colors[matched]
		out[ro+col+1] = UInt8(matched - 1)
		err = spx - output_px
		# this prevents weird green pixels popping out of the blue (or red or black! ;)
		if fma(err.r, err.r, err.g * err.g) + fma(err.b, err.b, err.a * err.a) > max_dither_error
			err = err * 0.75f0
		end
		if even_row
			diffusion[t+3]  += err * (7f0 / 16f0)
			diffusion[nx+1] += err * (3f0 / 16f0)
			diffusion[nx+2] += err * (5f0 / 16f0)
			diffusion[nx+3]  = err * (1f0 / 16f0)
		else
			diffusion[t+1]  += err * (7f0 / 16f0)
			diffusion[nx+1]  = err * (1f0 / 16f0)
			diffusion[nx+2] += err * (5f0 / 16f0)
			diffusion[nx+3] += err * (3f0 / 16f0)
		end
	end
end
