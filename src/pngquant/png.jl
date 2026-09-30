# png.jl — what pngquant's rwpng.c does with libpng: read any PNG as 8-bit RGBA, write an indexed
# (PLTE + tRNS) PNG. zlib is the copy every Julia install ships (Julia itself links it), reached by
# ccall — no package dependency. Read side: all colour types and bit depths, tRNS, Adam7
# interlacing; 16-bit samples keep their high byte (png_set_strip_16). Colour management: gAMA is
# passed on as the image gamma and an sRGB chunk is carried to the output, like a pngquant built
# without lcms2; iCCP/cHRM are not applied. Metadata chunks are not copied (as with --strip).

const _LIBZ = Ref{Ptr{Cvoid}}(C_NULL)

function _libz_sym(name::Symbol)::Ptr{Cvoid}
	if _LIBZ[] == C_NULL
		for nm in ("libz", "libz.so.1", "libz.1.dylib", "zlib1")
			h = Base.Libc.Libdl.dlopen(nm; throw_error=false)
			if h !== nothing
				_LIBZ[] = h
				break
			end
		end
		_LIBZ[] == C_NULL && error("pngquant: cannot load zlib")
	end
	return Base.Libc.Libdl.dlsym(_LIBZ[], name)
end

function _zcompress(src::Vector{UInt8}, level::Int)::Vector{UInt8}
	bound = ccall(_libz_sym(:compressBound), Culong, (Culong,), length(src))
	dst = Vector{UInt8}(undef, bound)
	dl = Ref{Culong}(bound)
	r = ccall(_libz_sym(:compress2), Cint, (Ptr{UInt8}, Ref{Culong}, Ptr{UInt8}, Culong, Cint), dst, dl, src, length(src), level)
	r == 0 || error("pngquant: zlib compress2 failed ($r)")
	return resize!(dst, dl[])
end

function _zuncompress(src::Vector{UInt8}, n::Int)::Vector{UInt8}
	dst = Vector{UInt8}(undef, n)
	dl = Ref{Culong}(n)
	r = ccall(_libz_sym(:uncompress), Cint, (Ptr{UInt8}, Ref{Culong}, Ptr{UInt8}, Culong), dst, dl, src, length(src))
	# Z_BUF_ERROR (-5) with a full buffer = trailing garbage after the image data; libpng ignores it too
	(r == 0 || (r == -5 && dl[] == n)) || error("pngquant: corrupt PNG image data (zlib $r)")
	dl[] == n || error("pngquant: truncated PNG image data")
	return dst
end

_crc32(crc::UInt32, data::Vector{UInt8}) = ccall(_libz_sym(:crc32), Culong, (Culong, Ptr{UInt8}, Cuint), crc, data, length(data)) % UInt32

const PNG_SIGNATURE = UInt8[0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]

_be32(b::Vector{UInt8}, i::Int) = UInt32(b[i]) << 24 | UInt32(b[i+1]) << 16 | UInt32(b[i+2]) << 8 | UInt32(b[i+3])
_be16(b::Vector{UInt8}, i::Int) = UInt16(b[i]) << 8 | UInt16(b[i+1])

"""
    PNGImage

A PNG read by `read_png`: `rgba` (row-major, top row first), `gamma` (0.45455 when the file does
not say), `srgb` (the file had an sRGB chunk), `file_size`, `iccp` (the file had an ICC profile,
which is NOT applied).
"""
struct PNGImage
	width::Int
	height::Int
	rgba::Vector{RGBA}
	gamma::Float64
	srgb::Bool
	iccp::Bool
	file_size::Int
end

"""
    read_png(filename) -> PNGImage
    read_png(bytes::Vector{UInt8}) -> PNGImage
"""
read_png(filename::String) = read_png(read(filename))

function read_png(buf::Vector{UInt8})::PNGImage
	(length(buf) >= 8 && buf[1:8] == PNG_SIGNATURE) || error("pngquant: not a PNG file")
	pos = 9
	width = height = 0
	bit_depth = color_type = interlace = 0
	plte = RGBA[]
	trns = UInt8[]
	gamma = 0.0
	srgb = iccp = false
	idat = UInt8[]
	while pos + 7 <= length(buf)
		len = Int(_be32(buf, pos))
		typ = String(buf[pos+4:pos+7])
		d = pos + 8
		d + len - 1 <= length(buf) || error("pngquant: truncated PNG chunk $typ")
		if typ == "IHDR"
			width = Int(_be32(buf, d)); height = Int(_be32(buf, d + 4))
			bit_depth = Int(buf[d+8]); color_type = Int(buf[d+9]); interlace = Int(buf[d+12])
		elseif typ == "PLTE"
			plte = [RGBA(buf[d+3k], buf[d+3k+1], buf[d+3k+2], 0xff) for k in 0:len÷3-1]
		elseif typ == "tRNS"
			trns = buf[d:d+len-1]
		elseif typ == "gAMA"
			gamma = _be32(buf, d) / 100000
		elseif typ == "sRGB"
			srgb = true
		elseif typ == "iCCP"
			iccp = true
		elseif typ == "IDAT"
			append!(idat, view(buf, d:d+len-1))
		elseif typ == "IEND"
			break
		end
		pos = d + len + 4
	end
	(width > 0 && height > 0) || error("pngquant: PNG has no IHDR")
	channels = (1, 0, 3, 1, 2, 0, 4)[color_type+1]
	channels > 0 || error("pngquant: bad PNG colour type $color_type")
	bpp_bits = channels * bit_depth
	bpp = max(1, bpp_bits ÷ 8)

	# the Adam7 passes (or the one full image): (x0, y0, dx, dy)
	passes = interlace == 1 ?
	         ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)) :
	         ((0, 0, 1, 1),)
	pdims = [(cld(max(width - p[1], 0), p[3]), cld(max(height - p[2], 0), p[4])) for p in passes]
	total = sum(((pw, ph),) -> pw == 0 ? 0 : ph * (1 + cld(pw * bpp_bits, 8)), pdims)
	raw = _zuncompress(idat, total)

	# samples, 16-bit kept whole for the tRNS key comparison
	samples = Matrix{UInt16}(undef, channels, width * height)
	off = 0
	for (p, (pw, ph)) in zip(passes, pdims)
		(pw == 0 || ph == 0) && continue
		rowbytes = cld(pw * bpp_bits, 8)
		prev = zeros(UInt8, rowbytes)
		cur = Vector{UInt8}(undef, rowbytes)
		for y in 0:ph-1
			ft = raw[off+1]
			copyto!(cur, 1, raw, off + 2, rowbytes)
			off += 1 + rowbytes
			_unfilter!(cur, prev, ft, bpp)
			py = p[2] + y * p[4]
			for x in 0:pw-1
				px = p[1] + x * p[3]
				k = py * width + px + 1
				for c in 1:channels
					samples[c, k] = _sample(cur, x * channels + c - 1, bit_depth)
				end
			end
			prev, cur = cur, prev
		end
	end

	rgba = Vector{RGBA}(undef, width * height)
	maxv = (1 << bit_depth) - 1
	s8(v) = bit_depth == 16 ? UInt8(v >> 8) : bit_depth == 8 ? UInt8(v) : UInt8(div(Int(v) * 255, maxv))
	@inbounds for k in 1:width*height
		if color_type == 3
			i = Int(samples[1, k]) + 1
			c = i <= length(plte) ? plte[i] : RGBA(0, 0, 0, 255)
			rgba[k] = RGBA(c.r, c.g, c.b, i <= length(trns) ? trns[i] : 0xff)
		elseif color_type == 0
			g = samples[1, k]
			a = (length(trns) >= 2 && g == _be16(trns, 1)) ? 0x00 : 0xff
			v = s8(g)
			rgba[k] = RGBA(v, v, v, a)
		elseif color_type == 2
			r, g, b = samples[1, k], samples[2, k], samples[3, k]
			a = (length(trns) >= 6 && r == _be16(trns, 1) && g == _be16(trns, 3) && b == _be16(trns, 5)) ? 0x00 : 0xff
			rgba[k] = RGBA(s8(r), s8(g), s8(b), a)
		elseif color_type == 4
			v = s8(samples[1, k])
			rgba[k] = RGBA(v, v, v, s8(samples[2, k]))
		else
			rgba[k] = RGBA(s8(samples[1, k]), s8(samples[2, k]), s8(samples[3, k]), s8(samples[4, k]))
		end
	end
	if srgb
		gamma = 0.45455
	elseif !(0 < gamma <= 1)
		gamma == 0 || @warn "pngquant readpng:  ignored out-of-range gamma $gamma"
		gamma = 0.45455
	end
	return PNGImage(width, height, rgba, gamma, srgb, iccp, length(buf))
end

@inline function _sample(row::Vector{UInt8}, i::Int, bit_depth::Int)::UInt16
	bit_depth == 8 && return UInt16(row[i+1])
	bit_depth == 16 && return UInt16(row[2i+1]) << 8 | UInt16(row[2i+2])
	per = 8 ÷ bit_depth
	byte = row[i÷per+1]
	shift = 8 - bit_depth * (i % per + 1)
	return UInt16((byte >> shift) & ((1 << bit_depth) - 1))
end

function _unfilter!(cur::Vector{UInt8}, prev::Vector{UInt8}, ft::UInt8, bpp::Int)
	n = length(cur)
	@inbounds if ft == 1
		for i in bpp+1:n
			cur[i] += cur[i-bpp]
		end
	elseif ft == 2
		for i in 1:n
			cur[i] += prev[i]
		end
	elseif ft == 3
		for i in 1:n
			a = i > bpp ? Int(cur[i-bpp]) : 0
			cur[i] += UInt8((a + Int(prev[i])) >> 1)
		end
	elseif ft == 4
		for i in 1:n
			a = i > bpp ? Int(cur[i-bpp]) : 0
			b = Int(prev[i])
			c = i > bpp ? Int(prev[i-bpp]) : 0
			p = a + b - c
			pa = abs(p - a); pb = abs(p - b); pc = abs(p - c)
			cur[i] += UInt8(pa <= pb && pa <= pc ? a : pb <= pc ? b : c)
		end
	elseif ft != 0
		error("pngquant: bad PNG filter type $ft")
	end
	return cur
end

function _write_chunk(io::IO, typ::String, data::Vector{UInt8})
	write(io, hton(UInt32(length(data))))
	td = vcat(Vector{UInt8}(typ), data)
	write(io, td)
	write(io, hton(_crc32(UInt32(0), td)))
end

"""
    write_png8(io_or_filename, indices, width, height, palette; srgb=false, gamma=0.45455, fast_compression=false)

Writes an indexed PNG: `indices` are 0-based, row-major, top row first. Bit depth 1/2/4/8 by palette
size, no filtering (palette images gain nothing from it), tRNS trimmed after the last non-opaque
entry — as rwpng_write_image8. The gAMA+sRGB pair is written only when `srgb` (the input had it).
Returns the number of bytes written.
"""
function write_png8(io::IO, indices::Vector{UInt8}, width::Int, height::Int, palette::Vector{RGBA};
                    srgb::Bool=false, gamma::Float64=0.45455, fast_compression::Bool=false)::Int
	np = length(palette)
	np <= 256 || error("pngquant: palette larger than 256")
	depth = np <= 2 ? 1 : np <= 4 ? 2 : np <= 16 ? 4 : 8
	rowbytes = cld(width * depth, 8)
	raw = zeros(UInt8, height * (rowbytes + 1))
	per = 8 ÷ depth
	@inbounds for y in 0:height-1
		ro = y * (rowbytes + 1) + 1          # raw[ro] = filter byte 0
		for x in 0:width-1
			v = indices[y*width+x+1]
			if depth == 8
				raw[ro+x+1] = v
			else
				raw[ro+x÷per+1] |= v << (8 - depth * (x % per + 1))
			end
		end
	end
	start = position(io)
	write(io, PNG_SIGNATURE)
	ihdr = vcat(reinterpret(UInt8, [hton(UInt32(width)), hton(UInt32(height))]), UInt8[depth, 3, 0, 0, 0])
	_write_chunk(io, "IHDR", ihdr)
	if srgb
		_write_chunk(io, "gAMA", collect(reinterpret(UInt8, [hton(UInt32(round(gamma * 100000)))])))
		_write_chunk(io, "sRGB", UInt8[0])                   # 0 = perceptual
	end
	_write_chunk(io, "PLTE", UInt8[b for c in palette for b in (c.r, c.g, c.b)])
	num_trans = something(findlast(c -> c.a < 255, palette), 0)
	num_trans > 0 && _write_chunk(io, "tRNS", UInt8[palette[i].a for i in 1:num_trans])
	_write_chunk(io, "IDAT", _zcompress(raw, fast_compression ? 1 : 9))
	_write_chunk(io, "IEND", UInt8[])
	return position(io) - start
end

function write_png8(filename::String, args...; kw...)::Int
	return open(io -> write_png8(io, args...; kw...), filename, "w")
end
