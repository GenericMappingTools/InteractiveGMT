# Image Enhance -> "1 - Indexed and RGB" — port of Mirone's src_figs/image_enhance.m
# ("Adjust Contrast"). The dialog (deps/ui/image_enhance.ui, ImageEnhanceDialog in 70_window.cpp)
# owns the contrast window; every pixel operation is here.
#
# Nothing is re-implemented that already exists:
#   * the histograms are `_image_hist` (imagehisto.jl) — the same single function Binarize and Show
#     Histogram use: 256 grey levels for 8-bit, DN bins for a 16-bit image's original;
#   * imadjust_j (clamp to [lo,hi], then map linearly onto 0..255) is `GMT.rescale`;
#   * the eigendecomposition decorrstretch needs is LinearAlgebra's, reached through GMT.
# What IS ported, because Mirone/MATLAB ships it and nothing here does it: localStretchlim (the
# cdf-based outlier limits) and decorrstretch/fitdecorrtrans (the PCA-style band decorrelation).

mutable struct _EnhState
	scene::Ptr{Cvoid}
	srcname::String
	I::GMTimage                       # the source image at FULL precision (a 16-bit band's original), band-planar
	counts::Vector{Vector{Float32}}   # one histogram per band — `_image_hist`'s (256 grey levels, or DN bins)
	nbands::Int
	lo::Float64                       # the value of bin 1 (0 for 8-bit)
	binw::Float64                     # the bin width in data values (1 for 8-bit and for DN bins)
end

const _ENHSTATE = Dict{Ptr{Cvoid}, _EnhState}()

# ---------------------------------------------------------------------------------------------
# The two algorithms Mirone ships and Julia/GMT do not
# ---------------------------------------------------------------------------------------------

# image_enhance.m localStretchlim / img_fun.m stretchlim_j: walk the cdf of the band's histogram and
# take the first bins crossing tol_low and tol_high. `tol` is the HALF fraction (Mirone passes
# outlierPct/2), so the window is [tol, 1-tol]. Returns DATA values: bin i is `lo + (i-1)*binw` —
# grey levels for an 8-bit band (lo 0, binw 1), DNs for a 16-bit one. A senseless tolerance or
# degenerate limits give the band's whole bin range ([0 1] in Mirone's normalised terms).
function _enh_stretchlim(counts::Vector{Float32}, tol::Float32, lo::Float64 = 0.0, binw::Float64 = 1.0)::Tuple{Float32,Float32}
	full = (Float32(lo), Float32(lo + (length(counts) - 1) * binw))
	tol_low, tol_high = tol, 1.0f0 - tol
	(tol_low >= tol_high) && return full                # "this tolerance does not make sense" -> [0 1]
	tot = sum(counts)
	tot <= 0 && return full
	cdf = cumsum(counts) ./ tot
	ilow  = findfirst(>(tol_low), cdf)
	ihigh = findfirst(>=(tol_high), cdf)
	(ilow === nothing || ihigh === nothing) && return full
	ilow == ihigh && return full                        # limits are same, use default
	return (Float32(lo + (ilow - 1) * binw), Float32(lo + (ihigh - 1) * binw))   # bin index -> value
end

# MATLAB's im2uint8: round HALF AWAY FROM ZERO. Julia's plain `round` is RoundNearest (ties to
# even), which is what made 16.5 come out 16 where MATLAB gives 17 — a visible one-level shift
# through a whole image. Every uint8 quantisation in this file goes through here.
_enh_u8(x::Float32)::UInt8 = round(UInt8, clamp(x, 0f0, 1f0) * 255, RoundNearestTiesAway)

# img_fun.m imadjust_j with gamma=1 and [low_out high_out]=[0 1], uint8 branch = AdjustWithLUT:
# build a 256-entry lookup table per band (clamp to [low_in,high_in], map onto [0,1], im2uint8) and
# push the band through it. Kept as the LUT Mirone uses, not as a generic rescale: the two differ in
# their rounding, and this is the one the numbers must match.
#
# A 16-bit band goes through the SAME LUT, one entry per DN (65536), in MATLAB's normalised domain
# for uint16 (v/65535 against the window/65535), and comes out 8-bit: imadjust's [0 1] output put on
# screen. The window it gets is in DNs, read off that band's own DN histogram.
function _enh_imadjust(I::GMTimage, lo::Vector{Float32}, hi::Vector{Float32})::GMTimage
	S  = I.image
	T  = eltype(S)
	(T == UInt8 || T == UInt16) || error("Contrast Stretch works on 8- or 16-bit images (this one is $T)")
	nb = ndims(S) == 3 ? size(S, 3) : 1
	out = Array{UInt8}(undef, size(S))
	top = Float32(typemax(T))
	lut = Vector{UInt8}(undef, Int(typemax(T)) + 1)
	for k = 1:nb
		src = nb == 1 ? view(S, :, :) : view(S, :, :, k)
		dst = nb == 1 ? view(out, :, :) : view(out, :, :, k)
		if hi[k] <= lo[k]                               # a degenerate window would divide by zero
			for v = 0:Int(typemax(T))  lut[v + 1] = _enh_u8(Float32(v) / top)  end
			@inbounds for j in eachindex(src)  dst[j] = lut[src[j] + 1]  end
			continue
		end
		# The arithmetic is done in MATLAB's NORMALISED domain — lut = linspace(0,1,256) against
		# low_in/high_in = the window / 255 — not in grey levels. The two differ in the last bits,
		# and that is enough to flip a value sitting exactly on a .5 tie (65 -> 37 vs 38).
		lo_in = lo[k] / top
		hi_in = hi[k] / top
		for v = 0:Int(typemax(T))
			lut[v + 1] = _enh_u8((clamp(Float32(v) / top, lo_in, hi_in) - lo_in) / (hi_in - lo_in))
		end
		@inbounds for j in eachindex(src)  dst[j] = lut[src[j] + 1]  end
	end
	# mat2img carries the georeferencing over — and any colormap, which these adjusted pixel VALUES
	# must not be looked up in (drape.jl `_img_drop_palette!`).
	return _img_drop_palette!(GMT.mat2img(out, I))
end

# img_fun.m fitdecorrtrans + decorr, 'correlation' mode (decorrstretch's default), targetMean and
# targetSigma empty = restore the original sample means/variances.
# `pinv` there is MATLAB's own diagonal pseudo-inverse with a loose tolerance — kept, because it is
# what makes a zero-variance or perfectly correlated band survive instead of blowing up.
function _enh_pinv_diag(d::Vector{Float32})::Vector{Float32}
	tol = length(d) * maximum(d) * sqrt(eps(Float32))
	return [x > tol ? 1f0 / x : 0f0 for x in d]
end

function _enh_decorrstretch(I::GMTimage, tol::Float32)::GMTimage
	S = I.image
	(ndims(S) == 3 && size(S, 3) >= 3) || error("Decorrelation stretch needs an RGB image")
	nb = size(S, 3)
	npix = size(S, 1) * size(S, 2)
	A = Matrix{Float32}(undef, npix, nb)                # im2double: uint8 /255, uint16 /65535 -> [0 1]
	top = Float32(typemax(eltype(S)))
	@inbounds for k = 1:nb
		v = view(S, :, :, k)
		for j = 1:npix  A[j, k] = v[j] / top  end
	end
	meanA = vec(sum(A, dims=1) ./ npix)
	Cov = (A' * A .- npix .* (meanA * meanA')) ./ (npix - 1)

	sd = sqrt.(max.(diag_of(Cov), 0f0))                 # S = diag(sqrt(diag(Cov)))
	targetSigma = GMT.diagm(sd)                         # targetSigma empty -> restore sample sigmas
	invS = GMT.diagm(_enh_pinv_diag(sd))
	Corr = invS * Cov * invS
	@inbounds for k = 1:nb  Corr[k, k] = 1f0  end       # Corr(logical(eye)) = 1
	F = GMT.eigen(GMT.Symmetric(Corr))
	W = GMT.diagm(sqrt.(_enh_pinv_diag(max.(F.values, 0f0))))   # decorrWeight(D) = sqrt(pinv(D))
	T = invS * F.vectors * W * F.vectors' * targetSigma
	# "Get the output variances right even for correlated bands" — T * pinv(diag(sqrt(diag(T'CovT))))
	T = T * GMT.diagm(_enh_pinv_diag(sqrt.(max.(diag_of(T' * Cov * T), 0f0)))) * targetSigma
	offset = meanA .- vec(meanA' * T)

	B = A * T .+ offset'
	out = similar(S)
	if tol > 0                                          # the optional contrast stretch, on [0 1] data
		for k = 1:nb
			cnt = _enh_hist01(view(B, :, k))
			lo, hi = _enh_stretchlim(cnt, tol)
			lo /= 255f0;  hi /= 255f0
			v = view(out, :, :, k)
			@inbounds for j = 1:npix
				t = hi > lo ? (clamp(B[j, k], lo, hi) - lo) / (hi - lo) : clamp(B[j, k], 0f0, 1f0)
				v[j] = _enh_u8(t)
			end
		end
	else
		for k = 1:nb
			v = view(out, :, :, k)
			@inbounds for j = 1:npix  v[j] = _enh_u8(B[j, k])  end
		end
	end
	return _img_drop_palette!(GMT.mat2img(out, I))
end

diag_of(M::AbstractMatrix{Float32}) = [M[k, k] for k = 1:size(M, 1)]

# 256-bin histogram of [0,1] floats — stretchlim_j's imhist_j for the decorrelated bands (an
# intermediate, not an image). An image's own bands never come here: they use `_image_hist`.
function _enh_hist01(v::AbstractVector{Float32})::Vector{Float32}
	c = zeros(Float32, 256)
	@inbounds for x in v  c[clamp(round(Int, x * 255), 0, 255) + 1] += 1f0  end
	return c
end

# ---------------------------------------------------------------------------------------------
# The ops the dialog sends
# ---------------------------------------------------------------------------------------------

function _enh_init(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, wanted::String="")
	srcname, I = isempty(wanted) ? _find_object_named(scene, :image) :
	                               (String(wanted), _find_object(scene, :image, wanted))
	(I isa GMTimage) || error("No image in this window to enhance")
	# The displayed object of a 16-bit image is its 8-bit display copy; the image IS the original.
	O = get(get(_IMG_ORIG, scene, Dict{String,GMTimage}()), String(srcname), nothing)
	O === nothing || (I = O)
	Iu = _to_band_planar(I)                        # band indexing below must not meet a 'P' image
	H  = _image_hist(Iu)                           # THE image histogram (imagehisto.jl), as Show Histogram's
	nb = length(H.counts)
	# A data histogram (DN bins) — the dialog's axes then speak data values, not grey levels.
	H.data && ccall(_fn(:gmtvtk_enhance_set_data_range), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble), dlg, H.lo, H.hi)
	cnts = Vector{Vector{Float32}}(undef, nb)
	for k = 1:nb
		cnts[k] = Float32.(H.counts[k])
		lo, hi = Float64.(extrema(view(H.M, :, k)))
		# The C API speaks double (gmtvtk_enhance_set_band) — the bins widened at the boundary only;
		# every computation on this side stays Float32.
		ccall(_fn(:gmtvtk_enhance_set_band), Cvoid,
		      (Ptr{Cvoid}, Cint, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cint),
		      dlg, Cint(k - 1), H.counts[k], Cint(length(H.counts[k])), lo, hi, Cint(nb))
	end
	# The limits "Auto histogram stretch" would use, for the "Show auto stretch limits" lines — from `I`
	# itself, the very image that stretch reads (the stashed original, not the band-planar copy).
	for (k, (lo, hi)) in enumerate(_enh_auto_limits(I, cnts, H.lo, H.binw))
		k > 3 && break
		ccall(_fn(:gmtvtk_enhance_set_auto_limits), Cvoid, (Ptr{Cvoid}, Cint, Cdouble, Cdouble),
		      dlg, Cint(k - 1), lo, hi)
	end
	_ENHSTATE[dlg] = _EnhState(scene, String(srcname), Iu, cnts, nb, H.lo, H.binw)
	return
end

# The result of a stretch is a NEW named element, checked, with the source unchecked and Scene
# Objects unfolded — SACRED_LAW.md's derived-variable display law, same path Binarize commits by.
function _enh_commit!(st::_EnhState, Iout::GMTimage, what::String)
	_commit_derived_image!(st.scene, Iout, isempty(st.srcname) ? "Image ($what)" : "$(st.srcname) ($what)")
	return
end

# push_contStrectch_CB with its default 1% tolerance, as ONE callable: per band, stretchlim on that
# band's own 256-bin histogram, then the imadjust LUT. This IS the auto contrast stretch of an 8-bit
# image, so the "Auto histogram stretch (new image)" image handle calls exactly this (savefile.jl
# `_on_img_stretch`) rather than carrying a second stretch of its own — same operation, same function.
# Bands past the third (e.g. an alpha channel) keep [0,255], which makes their LUT the identity.
function _enh_auto_stretch(I::GMTimage, tol::Float32=0.01f0)::GMTimage
	S  = I.image
	nb = ndims(S) == 3 ? size(S, 3) : 1
	lo, hi = zeros(Float32, nb), fill(255f0, nb)
	H = _image_hist(I)                                  # THE image histogram (imagehisto.jl)
	for k = 1:length(H.counts)
		lo[k], hi[k] = _enh_stretchlim(Float32.(H.counts[k]), tol, H.lo, H.binw)
	end
	return _enh_imadjust(I, lo, hi)
end

# The per-band limits "Auto histogram stretch (new image)" (savefile.jl `_on_img_stretch`) stretches
# `I` between, in the image's own data values — shown, never applied, by Adjust Contrast's "Show auto
# stretch limits". The same two sources that stretch has: a 16-bit image goes through
# `mat2img(I; stretch=true)`, which takes GMT's `find_histo_limits(I.image)`; an 8-bit one through
# `_enh_auto_stretch`, i.e. `_enh_stretchlim` at its 1% default on THE image histogram (`counts`,
# `_image_hist`'s, with its first-bin value `lo` and bin width `binw`). Any other type has no auto
# stretch, hence no limits.
function _enh_auto_limits(I::GMTimage, counts::Vector{Vector{Float32}}, lo::Float64, binw::Float64)::Vector{Tuple{Float64,Float64}}
	T = eltype(I.image)
	if T == UInt16
		# Where find_histo_limits throws (a band it cannot read a histogram off), the stretch itself
		# would throw too: no limits to show, and the dialog greys the box out instead of failing to open.
		L = try GMT.find_histo_limits(I.image) catch; return Tuple{Float64,Float64}[] end
		return [(Float64(L[2k - 1]), Float64(L[2k])) for k = 1:length(L) ÷ 2]
	elseif T == UInt8
		return [Float64.(_enh_stretchlim(c, 0.01f0, lo, binw)) for c in counts]
	end
	return Tuple{Float64,Float64}[]
end

# push_scaterPlot_CB: plot3(r,g,b,'.') of the three bands.
#
# The pixels arrive from C++ (sceneDisplayedRGB) and are the ones the window is DISPLAYING at the
# moment the button was pressed — so after a Decorrelation Stretch it is the decorrelated image
# that gets plotted. Nothing here remembers "which image is current": there is exactly one answer
# to that, the display itself, and this reads it (SACRED_LAW — one source of truth, no second copy
# to fall out of step, which is precisely what a remembered `work` image did).
#
# Mirone decimates to at most 256x256 points first (localImresize) — kept, a full image would be
# millions of markers.
function _on_rgb_scatter(scene::Ptr{Cvoid}, px::Ptr{UInt8}, npix::Cint, nb::Cint, clabel::Cstring)::Cint
	try
		(px == C_NULL || npix <= 0 || nb < 3) && return Cint(0)
		A = unsafe_wrap(Array, px, (Int(nb), Int(npix)))       # pixel-interleaved, no copy
		step = max(1, ceil(Int, Int(npix) / (256 * 256)))
		idx = 1:step:Int(npix)
		P = Matrix{Float32}(undef, length(idx), 3)
		@inbounds for (k, j) in enumerate(idx)
			P[k, 1] = A[1, j];  P[k, 2] = A[2, j];  P[k, 3] = A[3, j]
		end
		label = unsafe_string(clabel)
		# A real 3-D iGMT window instead of MATLAB's plot3 figure — Red/Green/Blue ARE x/y/z.
		_view_points(P; geographic=false,
		            title="RGB scatter — $label  (x = Red, y = Green, z = Blue)")
		return Cint(1)
	catch err
		_tool_failed(scene, "ScaterPlot", err)
		return Cint(0)
	end
end

function _on_image_enhance(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		p  = split(unsafe_string(cparams), ';')
		op = p[1]
		if op == "close"
			delete!(_ENHSTATE, dlg)
			return Cint(1)
		end
		if op == "init"
			_enh_init(scene, dlg, length(p) >= 2 ? String(strip(p[2])) : "")
			return Cint(1)
		end
		st = get(_ENHSTATE, dlg, nothing)
		(st isa _EnhState) || error("Image Enhance: this dialog has no image loaded")
		if op == "contrast"
			lo = [parse(Float32, p[2]), parse(Float32, p[4]), parse(Float32, p[6])]
			hi = [parse(Float32, p[3]), parse(Float32, p[5]), parse(Float32, p[7])]
			_enh_commit!(st, _enh_imadjust(st.I, lo, hi), "contrast stretch")
		elseif op == "decorr"
			# Mirone's default 2% outliers; an explicit "decorr;<pct>" still overrides it.
			pct = length(p) >= 2 ? something(tryparse(Float32, strip(p[2])), 2f0) : 2f0
			_enh_commit!(st, _enh_decorrstretch(st.I, pct / 100 / 2), "decorr stretch")
		else
			error("Image Enhance: unknown op '$op'")
		end
		return Cint(1)
	catch e
		_tool_failed(scene, "Image Enhance ($op)", e)
		return Cint(0)
	end
end

function _register_image_enhance()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_image_enhance, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_image_enhance_callback), Cvoid, (Ptr{Cvoid},), fptr)
	sptr = @cfunction((s, p, n, b, l) -> Base.invokelatest(_on_rgb_scatter, s, p, n, b, l)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{UInt8}, Cint, Cint, Cstring))
	ccall(_fn(:gmtvtk_set_rgb_scatter_callback), Cvoid, (Ptr{Cvoid},), sptr)
	return
end
