# Image histogram — port of Mirone's src_figs/image_histo.m ("Image -> Show Histogram").
#
# Mirone histograms `get(hMirHand.hImg,'CData')`: the image ON DISPLAY. For a grid window that is
# the grid's RENDERED colour image, not the grid's z values — so the pixels can only come from the
# viewer, which is what the C++ side hands us here (deps/src/70_window.cpp, sceneDisplayedRGB).
#
# The counting itself is NOT re-implemented: it is `GMT.histogray`, the same single function the
# Binarize dialog uses (SACRED_LAW: one quantity, one function). histogray is exactly Mirone's
# imhistc for uint8 with n=256/isScaled=1/top=255 — bin k counts the pixels whose value is k.

# `px` is pixel-interleaved: nb bytes per pixel, npix pixels. Wrapping it as an (nb x npix) matrix
# makes band k the row k view that histogray consumes, with no copy of the buffer.
#
# When the displayed element is an IMAGE the window holds, its histogram is counted from that image's
# own data, not from the bytes on screen, which lie twice: a 16-bit image is DISPLAYED as an 8-bit
# copy (`_add_image_to_scene` stretches it; the original is stashed in `_IMG_ORIG`), and a one-band
# picture is drawn as an RGBA texture, so its bytes read as three identical "R/G/B" bands.
function _on_image_histo(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, px::Ptr{UInt8}, npix::Cint, nb::Cint,
                         cname::Cstring)::Cint
	try
		name = cname == C_NULL ? "" : unsafe_string(cname)
		I = get(get(_IMG_ORIG, scene, Dict{String,GMTimage}()), name, nothing)
		I === nothing && (I = _find_object_exact(scene, :image, name))
		I isa GMTimage && return _image_histo_data(dlg, I)
		(px == C_NULL || npix <= 0 || nb <= 0) && return Cint(0)
		A = unsafe_wrap(Array, px, (Int(nb), Int(npix)))
		nband = min(Int(nb), 3)              # RGB(A): the alpha band is not part of Mirone's display
		for k = 1:nband
			counts, _ = GMT.histogray(view(A, k:k, :))
			cnt = Float64.(counts)
			ccall(_fn(:gmtvtk_histo_set_counts), Cvoid, (Ptr{Cvoid}, Cint, Ptr{Cdouble}, Cint),
			      dlg, Cint(k - 1), cnt, Cint(length(cnt)))
		end
		return Cint(1)
	catch err
		_tool_failed(scene, "Image histogram", err)
		return Cint(0)
	end
end

# The bin width, in DNs, of an integer (16-bit) image's histogram.
const _IMG_HIST_DNBIN = 2

# THE IMAGE HISTOGRAM — every tool that shows or uses an image's histogram (Show Histogram, Image
# Enhance, Binarize) gets it from here and nowhere else (SACRED_LAW: one quantity, one function).
#
# One band at a time (an alpha band is not one: 1-2 bands make ONE histogram, 3-4 make R/G/B). A band
# is taken where it lies: band-planar memory is one column per band, pixel-interleaved one row per
# band — a histogram does not care where a pixel sits, only what it holds.
#   8-bit:     256 grey levels, `GMT.histogray`. lo = 0, binw = 1, data = false.
#   integer:   bins _IMG_HIST_DNBIN (2) DN wide (a 16-bit band: up to 32768), `_hist_dn_count` —
#              bin i counts the values lo+(i-1)*binw ... lo+i*binw-1. A whole number of DNs, so no
#              bin holds more values than its neighbour. data = true.
#   float:     no DN — 256 bins over [lo, hi]. data = true.
# A black or white background spike at either END is dropped, for every type (`_hist_drop_end_spikes!`).
# Returns the counts per band, the value of bin 1 (`lo`), the last bin's value (`hi`), the bin width,
# whether the X axis is in data values, and the (pixels x bands) view the bands were read from.
function _image_hist(I::GMTimage)
	A  = I.image
	nb = ndims(A) == 3 ? size(A, 3) : 1
	M  = (nb > 1 && length(I.layout) > 2 && I.layout[3] == 'P') ? transpose(reshape(A, nb, :)) : reshape(A, :, nb)
	nshow = nb >= 3 ? 3 : 1
	cnts = Vector{Vector{Float64}}(undef, nshow)
	if eltype(A) == UInt8
		for k = 1:nshow
			c, _ = GMT.histogray(view(M, :, k:k))
			cnts[k] = _hist_drop_end_spikes!(Float64.(c))
		end
		return (counts = cnts, lo = 0.0, hi = 255.0, binw = 1.0, data = false, M = M)
	end
	lo, hi = Float64(I.range[5]), Float64(I.range[6])
	hi > lo || ((lo, hi) = Float64.(extrema(A)))
	hi > lo || (hi = lo + 1.0)
	isint = eltype(A) <: Integer
	isint && ((lo, hi) = (floor(lo), floor(hi)))
	binw  = isint ? Float64(_IMG_HIST_DNBIN) : (hi - lo) / 256
	nbin  = isint ? fld(Int(hi - lo), _IMG_HIST_DNBIN) + 1 : 256
	isint && (hi = lo + (nbin - 1) * binw)                  # the value bin `nbin` starts at
	for k = 1:nshow
		if isint
			cnt = _hist_dn_count(view(M, :, k), Int(lo), _IMG_HIST_DNBIN, nbin)
		else
			hst = GMT.hst_floats(view(M, :, k), string(binw); min_max = (lo, hi))[1]
			cnt = zeros(Float64, nbin)
			for i in 1:size(hst, 1)
				cnt[min(i, nbin)] += hst[i, 2]   # a value exactly at `hi` lands one bin past
			end
		end
		cnts[k] = _hist_drop_end_spikes!(cnt)
	end
	return (counts = cnts, lo = lo, hi = hi, binw = binw, data = true, M = M)
end

# Integer data into DN bins EXACTLY: bin i holds lo+(i-1)*w ... lo+i*w-1, in integer arithmetic.
# NOT GMT.hst_floats: it divides by the width plus an epsilon (its float edges' guard), so a value
# sitting on a bin edge — every lo+k*w — fell into the bin BELOW (a 1-DN histogram was shifted by
# one DN throughout; with 2-DN bins the peak of a Landsat band was over by 498 counts).
function _hist_dn_count(v::AbstractVector{<:Integer}, lo::Int, w::Int, nbin::Int)::Vector{Float64}
	c = zeros(Int, nbin)
	@inbounds for x in v
		i = (Int(x) - lo) ÷ w + 1
		(1 <= i <= nbin) && (c[i] += 1)
	end
	return Float64.(c)
end

# A black or white BACKGROUND — the fill around a rotated satellite swath (DN 0, a third of a Landsat
# scene), a white page around a scan, a saturated sky — is one bin at an END of the histogram holding
# millions of pixels, and it flattens the real distribution to nothing. It is dropped, at BOTH ends,
# by GMT's own two tests for exactly this (`find_histo_limits`, histo_funs.jl): the end bin's next
# four neighbours are empty (an isolated peak), or the end bin holds more than 100 times the mean of
# its next nine (a spike on top of the tail). Never when it would leave nothing: a pure two-level
# mask IS its two end bins.
function _hist_drop_end_spikes!(cnt::Vector{Float64})::Vector{Float64}
	n = length(cnt)
	n > 10 || return cnt
	spike(e, nb) = cnt[e] > 0 && (all(iszero, view(cnt, nb[1:4])) || cnt[e] > 100 * sum(view(cnt, nb)) / 9)
	lo_sp = spike(1, 2:10)
	hi_sp = spike(n, (n-1):-1:(n-9))
	rest = sum(view(cnt, 2:n-1))
	rest > 0 || return cnt
	lo_sp && (cnt[1] = 0.0)
	hi_sp && (cnt[n] = 0.0)
	return cnt
end

# Show Histogram's half: `_image_hist`, pushed into the dialog.
function _image_histo_data(dlg::Ptr{Cvoid}, I::GMTimage)::Cint
	H = _image_hist(I)
	ccall(_fn(:gmtvtk_histo_set_bands), Cvoid, (Ptr{Cvoid}, Cint, Cdouble, Cdouble),
	      dlg, Cint(length(H.counts)), H.data ? H.lo : 0.0, H.data ? H.hi : 0.0)
	for (k, cnt) in enumerate(H.counts)
		ccall(_fn(:gmtvtk_histo_set_counts), Cvoid, (Ptr{Cvoid}, Cint, Ptr{Cdouble}, Cint),
		      dlg, Cint(k - 1), cnt, Cint(length(cnt)))
	end
	return Cint(1)
end

# The trampoline goes through `invokelatest`, like every other registration in _ensure_callbacks,
# and that is not a style detail: a DIRECT `@cfunction(_on_image_histo, …)` makes the compiler infer
# and emit the WHOLE handler body — GMT.histogray and all — at REGISTRATION time, which is the first
# window open. Measured: 134 ms here against ~14 ms for the invokelatest form, the single most
# expensive of the ~79 registrations. Behind invokelatest the body compiles on the first actual
# histogram instead, where the user is already waiting for a computation.
function _register_image_histo()
	cb = @cfunction((s, d, p, n, b, nm) -> Base.invokelatest(_on_image_histo, s, d, p, n, b, nm)::Cint,
	                Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{UInt8}, Cint, Cint, Cstring))
	ccall(_fn(:gmtvtk_set_image_histo_callback), Cvoid, (Ptr{Cvoid},), cb)
	return nothing
end
