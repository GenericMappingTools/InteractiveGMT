# frontend.jl — pngquant.c: the command-line tool's per-file flow, as a function. Option names follow
# the CLI (--quality, --speed, --nofs, --floyd, --posterize, --ext, --output, --force,
# --skip-if-larger, --verbose). Not ported: stdin/stdout, --map (fixed palette from another image),
# --iebug (deprecated upstream), metadata copying (output is always as with --strip).

"""
    parse_quality(q) -> (min, max)

pngquant's `--quality` syntax: `"N"` (automatic: min N*9/10, max N), `"-N"` (0-N), `"N-"` (N-100),
`"N-M"`. A `(min, max)` tuple passes through.
"""
function parse_quality(q::String)::Tuple{Int,Int}
	m = match(r"^\s*(-?\d+)(?:(-)(\d*))?\s*$", q)
	m === nothing && throw(ArgumentError("Quality should be in format min-max where min and max are numbers in range 0-100."))
	t1 = parse(Int, m.captures[1])
	if m.captures[2] === nothing
		return t1 < 0 ? (0, -t1) : (t1 * 9 ÷ 10, t1)       # "-N" / "N"
	end
	return isempty(m.captures[3]) ? (t1, 100) : (t1, parse(Int, m.captures[3]))
end
parse_quality(q::Tuple{Int,Int}) = q

# Settings shared by the in-memory and the file versions, applied exactly as main() does.
function _pq_attr(ncolors::Int, quality, speed::Int, posterize::Int, last_index_transparent::Bool, verbose::Bool)
	liq = LiqAttr()
	verbose && (liq.log = msg -> println(stderr, msg))
	min_quality_limit = false
	if quality !== nothing
		qmin, qmax = parse_quality(quality)
		set_quality!(liq, qmin, qmax)
		min_quality_limit = qmin > 0
	end
	set_last_index_transparent!(liq, last_index_transparent)
	1 <= speed <= 11 || throw(ArgumentError("Speed should be between 1 (slow) and 11 (fast)."))
	set_speed!(liq, min(speed, 10))
	set_max_colors!(liq, ncolors)
	set_min_posterization!(liq, posterize)
	return liq, min_quality_limit
end

# --speed 11 = speed 10 without dithering; --nofs = --floyd=0
_pq_floyd(floyd::Real, nofs::Bool, speed::Int) = (nofs || speed == 11) ? 0.0 : Float64(floyd)

"""
    pngquant(A; ncolors=256, quality=nothing, speed=4, floyd=1.0, nofs=false, posterize=0,
             gamma=0.0, last_index_transparent=false, verbose=false) -> (indices, palette, quality)

Quantizes an image held in memory. `A` is a (height, width, nbands) `Array{UInt8,3}` (1 grey,
2 grey+alpha, 3 RGB, 4 RGBA), `A[1,1,:]` = top-left, or a `Vector{RGBA}` with `width`/`height`
given as keywords. Returns the (height, width) `Matrix{UInt8}` of 0-based palette indices, the
`Vector{RGBA}` palette and the achieved quality (0-100). Throws `LiqError(:QualityTooLow)` when
the `quality` minimum can't be met.
"""
function pngquant(A::Array{UInt8,3}; ncolors::Int=256, quality=nothing, speed::Int=4, floyd::Real=1.0,
                  nofs::Bool=false, posterize::Int=0, gamma::Real=0.0, last_index_transparent::Bool=false,
                  verbose::Bool=false)
	liq, _ = _pq_attr(ncolors, quality, speed, posterize, last_index_transparent, verbose)
	img = LiqImage(liq, A; gamma=gamma)
	pal, idx, q = _pq_remap(liq, img, _pq_floyd(floyd, nofs, speed), verbose)
	return permutedims(reshape(idx, img.width, img.height)), pal, q
end

function _pq_remap(liq::LiqAttr, img::LiqImage, floyd::Float64, verbose::Bool)
	res = quantize(liq, img)
	# fixed gamma ~2.2 for the web. PNG can't store exact 1/2.2
	set_output_gamma!(res, 0.45455)
	set_dithering_level!(res, floyd)
	pal, idx = remapped(res, img)
	q = something(quantization_quality(res), 90)
	if res.palette_error !== nothing
		verbose_print(liq, "  mapped image to new colors...MSE=$(_f3(quantization_error(res))) (Q=$q)")
	end
	return pal, idx, q
end

"""
    pngquant(filename; ncolors=256, quality=nothing, speed=4, floyd=1.0, nofs=false, posterize=0,
             output=nothing, ext=nothing, force=false, skip_if_larger=false,
             last_index_transparent=false, verbose=false) -> String

Does what `pngquant [options] [ncolors] -- filename` does: writes the 8-bit indexed version of a
PNG and returns its path. Default output name: `<name>-fs8.png` (`-or8.png` without dithering), or
`ext` in place of that suffix, or `output`. An existing output is an error unless `force`.
Throws `LiqError(:QualityTooLow)` (exit code 99 of the CLI) when the `quality` minimum can't be
met, and `LiqError(:TooLargeFile)` (code 98) when `skip_if_larger` and the result is not smaller
enough to justify the quality loss — in both cases nothing is written.
"""
function pngquant(filename::String; ncolors::Int=256, quality=nothing, speed::Int=4, floyd::Real=1.0,
                  nofs::Bool=false, posterize::Int=0, output::Union{Nothing,String}=nothing,
                  ext::Union{Nothing,String}=nothing, force::Bool=false, skip_if_larger::Bool=false,
                  last_index_transparent::Bool=false, verbose::Bool=false)::String
	(ext !== nothing && output !== nothing) && throw(ArgumentError("--ext and --output options can't be used at the same time"))
	liq, _ = _pq_attr(ncolors, quality, speed, posterize, last_index_transparent, verbose)
	fl = _pq_floyd(floyd, nofs, speed)
	# the new filename's extension depends on the options used. Typically basename-fs8.png
	if output === nothing
		e = ext === nothing ? (fl > 0 ? "-fs8.png" : "-or8.png") : ext
		output = (length(filename) > 4 && lowercase(filename[end-3:end]) == ".png") ? filename[1:end-4] * e : filename * e
	end
	(!force && isfile(output)) && throw(ArgumentError("$output exists; not overwriting (use force=true)"))

	verbose_print(liq, "$filename:")
	png = read_png(filename)
	verbose_print(liq, "  read $(cld(png.file_size, 1024))KB file")
	png.iccp && verbose_print(liq, "  warning: embedded ICC profile ignored (not converted to sRGB)")
	if png.srgb
		verbose_print(liq, "  passing sRGB tag from the input")
	elseif png.gamma != 0.45455
		verbose_print(liq, "  converted image from gamma $(round(1 / png.gamma; digits=1)) to gamma 2.2")
	end
	img = LiqImage(liq, png.rgba, png.width, png.height; gamma=png.gamma)
	pal, idx, q = _pq_remap(liq, img, fl, verbose)

	io = IOBuffer()
	nbytes = write_png8(io, idx, png.width, png.height, pal; srgb=png.srgb, fast_compression=speed >= 10)
	if skip_if_larger
		# very rough approximation, but generally avoids losing more quality than is gained in file
		# size. Quality is raised to 1.5, because bigger savings are needed to justify a big quality
		# loss; but >50% savings are always worthwhile, so low-quality conversions work at all
		expected_reduced_size = (q / 100)^1.5
		maximum_file_size = (png.file_size - 1) * max(expected_reduced_size, 0.5)
		if nbytes > maximum_file_size
			verbose_print(liq, "  file exceeded expected size of $(floor(Int, maximum_file_size / 1024))KB")
			throw(LiqError(:TooLargeFile, "$output: result would not be smaller enough"))
		end
	end
	write(output, take!(io))
	verbose_print(liq, "  writing $(length(pal))-color image as $(basename(output))")
	return output
end
