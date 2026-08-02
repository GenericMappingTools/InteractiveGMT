# Image Enhance -> "1 - Indexed and RGB" — port of Mirone's src_figs/image_enhance.m
# ("Adjust Contrast"). The dialog (deps/ui/image_enhance.ui, ImageEnhanceDialog in 70_window.cpp)
# owns the contrast window; every pixel operation is here.
#
# Nothing is re-implemented that already exists:
#   * the histograms are `GMT.histogray` — the same single function Binarize and Show Histogram use;
#   * imadjust_j (clamp to [lo,hi], then map linearly onto 0..255) is `GMT.rescale`;
#   * the eigendecomposition decorrstretch needs is LinearAlgebra's, reached through GMT.
# What IS ported, because Mirone/MATLAB ships it and nothing here does it: localStretchlim (the
# cdf-based outlier limits) and decorrstretch/fitdecorrtrans (the PCA-style band decorrelation).

mutable struct _EnhState
	scene::Ptr{Cvoid}
	srcname::String
	I::GMTimage                       # the source image, UInt8, band-planar
	counts::Vector{Vector{Float64}}   # one 256-bin histogram per band
	nbands::Int
end

const _ENHSTATE = Dict{Ptr{Cvoid}, _EnhState}()

# ---------------------------------------------------------------------------------------------
# The two algorithms Mirone ships and Julia/GMT do not
# ---------------------------------------------------------------------------------------------

# image_enhance.m localStretchlim / img_fun.m stretchlim_j, for a uint8 band: walk the cdf of the
# 256-bin histogram and take the first bins crossing tol_low and tol_high. `tol` is the HALF
# fraction (Mirone passes outlierPct/2), so the window is [tol, 1-tol]. Returns grey levels.
function _enh_stretchlim(counts::Vector{Float64}, tol::Float64)::Tuple{Float64,Float64}
	tol_low, tol_high = tol, 1.0 - tol
	(tol_low >= tol_high) && return (0.0, 255.0)        # "this tolerance does not make sense" -> [0 1]
	tot = sum(counts)
	tot <= 0 && return (0.0, 255.0)
	cdf = cumsum(counts) ./ tot
	ilow  = findfirst(>(tol_low), cdf)
	ihigh = findfirst(>=(tol_high), cdf)
	(ilow === nothing || ihigh === nothing) && return (0.0, 255.0)
	ilow == ihigh && return (0.0, 255.0)                # limits are same, use default
	return (Float64(ilow - 1), Float64(ihigh - 1))      # bin index -> grey level
end

# MATLAB's im2uint8: round HALF AWAY FROM ZERO. Julia's plain `round` is RoundNearest (ties to
# even), which is what made 16.5 come out 16 where MATLAB gives 17 — a visible one-level shift
# through a whole image. Every uint8 quantisation in this file goes through here.
_enh_u8(x::Float64)::UInt8 = round(UInt8, clamp(x, 0.0, 1.0) * 255, RoundNearestTiesAway)

# img_fun.m imadjust_j with gamma=1 and [low_out high_out]=[0 1], uint8 branch = AdjustWithLUT:
# build a 256-entry lookup table per band (clamp to [low_in,high_in], map onto [0,1], im2uint8) and
# push the band through it. Kept as the LUT Mirone uses, not as a generic rescale: the two differ in
# their rounding, and this is the one the numbers must match.
function _enh_imadjust(I::GMTimage, lo::Vector{Float64}, hi::Vector{Float64})::GMTimage
	S  = I.image
	nb = ndims(S) == 3 ? size(S, 3) : 1
	out = similar(S)
	lut = Vector{UInt8}(undef, 256)
	for k = 1:nb
		src = nb == 1 ? view(S, :, :) : view(S, :, :, k)
		dst = nb == 1 ? view(out, :, :) : view(out, :, :, k)
		if hi[k] <= lo[k]                               # a degenerate window would divide by zero
			dst .= src
			continue
		end
		# The arithmetic is done in MATLAB's NORMALISED domain — lut = linspace(0,1,256) against
		# low_in/high_in = the window / 255 — not in grey levels. The two differ in the last bits,
		# and that is enough to flip a value sitting exactly on a .5 tie (65 -> 37 vs 38).
		lo_in = lo[k] / 255.0
		hi_in = hi[k] / 255.0
		for v = 0:255
			lut[v + 1] = _enh_u8((clamp(v / 255.0, lo_in, hi_in) - lo_in) / (hi_in - lo_in))
		end
		@inbounds for j in eachindex(src)  dst[j] = lut[src[j] + 1]  end
	end
	return GMT.mat2img(out, I)                          # mat2img carries the georeferencing over
end

# img_fun.m fitdecorrtrans + decorr, 'correlation' mode (decorrstretch's default), targetMean and
# targetSigma empty = restore the original sample means/variances.
# `pinv` there is MATLAB's own diagonal pseudo-inverse with a loose tolerance — kept, because it is
# what makes a zero-variance or perfectly correlated band survive instead of blowing up.
function _enh_pinv_diag(d::Vector{Float64})::Vector{Float64}
	tol = length(d) * maximum(d) * sqrt(eps())
	return [x > tol ? 1.0 / x : 0.0 for x in d]
end

function _enh_decorrstretch(I::GMTimage, tol::Float64)::GMTimage
	S = I.image
	(ndims(S) == 3 && size(S, 3) >= 3) || error("Decorrelation stretch needs an RGB image")
	nb = size(S, 3)
	npix = size(S, 1) * size(S, 2)
	A = Matrix{Float64}(undef, npix, nb)                # im2double: uint8 -> [0 1]
	@inbounds for k = 1:nb
		v = view(S, :, :, k)
		for j = 1:npix  A[j, k] = v[j] / 255.0  end
	end
	meanA = vec(sum(A, dims=1) ./ npix)
	Cov = (A' * A .- npix .* (meanA * meanA')) ./ (npix - 1)

	sd = sqrt.(max.(diag_of(Cov), 0.0))                 # S = diag(sqrt(diag(Cov)))
	targetSigma = GMT.diagm(sd)                         # targetSigma empty -> restore sample sigmas
	invS = GMT.diagm(_enh_pinv_diag(sd))
	Corr = invS * Cov * invS
	@inbounds for k = 1:nb  Corr[k, k] = 1.0  end       # Corr(logical(eye)) = 1
	F = GMT.eigen(GMT.Symmetric(Corr))
	W = GMT.diagm(sqrt.(_enh_pinv_diag(max.(F.values, 0.0))))   # decorrWeight(D) = sqrt(pinv(D))
	T = invS * F.vectors * W * F.vectors' * targetSigma
	# "Get the output variances right even for correlated bands" — T * pinv(diag(sqrt(diag(T'CovT))))
	T = T * GMT.diagm(_enh_pinv_diag(sqrt.(max.(diag_of(T' * Cov * T), 0.0)))) * targetSigma
	offset = meanA .- vec(meanA' * T)

	B = A * T .+ offset'
	out = similar(S)
	if tol > 0                                          # the optional contrast stretch, on [0 1] data
		for k = 1:nb
			cnt = _enh_hist01(view(B, :, k))
			lo, hi = _enh_stretchlim(cnt, tol)
			lo /= 255.0;  hi /= 255.0
			v = view(out, :, :, k)
			@inbounds for j = 1:npix
				t = hi > lo ? (clamp(B[j, k], lo, hi) - lo) / (hi - lo) : clamp(B[j, k], 0.0, 1.0)
				v[j] = _enh_u8(t)
			end
		end
	else
		for k = 1:nb
			v = view(out, :, :, k)
			@inbounds for j = 1:npix  v[j] = _enh_u8(B[j, k])  end
		end
	end
	return GMT.mat2img(out, I)
end

diag_of(M::AbstractMatrix{Float64}) = [M[k, k] for k = 1:size(M, 1)]

# 256-bin histogram of [0,1] doubles — stretchlim_j's imhist_j for the decorrelated (double) bands.
# The uint8 path never comes here: it uses GMT.histogray.
function _enh_hist01(v::AbstractVector{Float64})::Vector{Float64}
	c = zeros(Float64, 256)
	@inbounds for x in v  c[clamp(round(Int, x * 255), 0, 255) + 1] += 1.0  end
	return c
end

# ---------------------------------------------------------------------------------------------
# The ops the dialog sends
# ---------------------------------------------------------------------------------------------

function _enh_init(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, wanted::AbstractString="")
	srcname, I = isempty(wanted) ? _find_object_named(scene, :image) :
	                               (String(wanted), _find_object(scene, :image, wanted))
	(I isa GMTimage) || error("No image in this window to enhance")
	Iu = eltype(I.image) == UInt8 ? I : _stretch_to_u8(I)
	Iu = _to_band_planar(Iu)                       # band indexing below must not meet a 'P' image
	nb = ndims(Iu.image) == 3 ? min(size(Iu.image, 3), 3) : 1
	cnts = Vector{Vector{Float64}}(undef, nb)
	for k = 1:nb
		band = nb == 1 ? view(Iu.image, :, :) : view(Iu.image, :, :, k)
		counts, _ = GMT.histogray(band)
		cnts[k] = Float64.(counts)
		lo, hi = Float64(minimum(band)), Float64(maximum(band))
		ccall(_fn(:gmtvtk_enhance_set_band), Cvoid,
		      (Ptr{Cvoid}, Cint, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cint),
		      dlg, Cint(k - 1), cnts[k], Cint(256), lo, hi, Cint(nb))
	end
	_ENHSTATE[dlg] = _EnhState(scene, String(srcname), Iu, cnts, nb)
	return
end

# The result of a stretch is a NEW named element, checked, with the source unchecked and Scene
# Objects unfolded — SACRED_LAW.md's derived-variable display law, same path Binarize commits by.
function _enh_commit!(st::_EnhState, Iout::GMTimage, what::AbstractString)
	_commit_derived_image!(st.scene, Iout,
	                       isempty(st.srcname) ? "Image ($what)" : "$(st.srcname) ($what)")
	return
end

# push_contStrectch_CB with its default 1% tolerance, as ONE callable: per band, stretchlim on that
# band's own 256-bin histogram, then the imadjust LUT. This IS the auto contrast stretch of an 8-bit
# image, so the "Auto histogram stretch (new image)" image handle calls exactly this (savefile.jl
# `_on_img_stretch`) rather than carrying a second stretch of its own — same operation, same function.
# Bands past the third (e.g. an alpha channel) keep [0,255], which makes their LUT the identity.
function _enh_auto_stretch(I::GMTimage, tol::Float64=0.01)::GMTimage
	S  = I.image
	nb = ndims(S) == 3 ? size(S, 3) : 1
	lo, hi = zeros(Float64, nb), fill(255.0, nb)
	for k = 1:min(nb, 3)
		cnt, = GMT.histogray(nb == 1 ? view(S, :, :) : view(S, :, :, k))
		lo[k], hi[k] = _enh_stretchlim(Float64.(cnt), tol)
	end
	return _enh_imadjust(I, lo, hi)
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
function _on_rgb_scatter(scene::Ptr{Cvoid}, px::Ptr{UInt8}, npix::Cint, nb::Cint,
                         clabel::Cstring)::Cint
	try
		(px == C_NULL || npix <= 0 || nb < 3) && return Cint(0)
		A = unsafe_wrap(Array, px, (Int(nb), Int(npix)))       # pixel-interleaved, no copy
		step = max(1, ceil(Int, Int(npix) / (256 * 256)))
		idx = 1:step:Int(npix)
		P = Matrix{Float64}(undef, length(idx), 3)
		@inbounds for (k, j) in enumerate(idx)
			P[k, 1] = A[1, j];  P[k, 2] = A[2, j];  P[k, 3] = A[3, j]
		end
		label = unsafe_string(clabel)
		# A real 3-D iGMT window instead of MATLAB's plot3 figure — Red/Green/Blue ARE x/y/z.
		view_points(P; geographic=false,
		            title="RGB scatter — $label  (x = Red, y = Green, z = Blue)")
		return Cint(1)
	catch err
		_viewer_log_error(scene, "ScaterPlot FAILED: $(sprint(showerror, err))")
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
			_enh_init(scene, dlg, length(p) >= 2 ? strip(p[2]) : "")
			return Cint(1)
		end
		st = get(_ENHSTATE, dlg, nothing)
		(st isa _EnhState) || error("Image Enhance: this dialog has no image loaded")
		if op == "stretchlim"
			band = something(tryparse(Int, strip(p[2])), 0)
			pct  = something(tryparse(Float64, strip(p[3])), 2.0)
			lo, hi = _enh_stretchlim(st.counts[clamp(band, 0, st.nbands - 1) + 1], pct / 100 / 2)
			ccall(_fn(:gmtvtk_enhance_set_window), Cvoid, (Ptr{Cvoid}, Cint, Cdouble, Cdouble),
			      dlg, Cint(band), lo, hi)
		elseif op == "contrast"
			lo = [parse(Float64, p[2]), parse(Float64, p[4]), parse(Float64, p[6])]
			hi = [parse(Float64, p[3]), parse(Float64, p[5]), parse(Float64, p[7])]
			_enh_commit!(st, _enh_imadjust(st.I, lo, hi), "contrast stretch")
		elseif op == "decorr"
			pct = something(tryparse(Float64, strip(p[2])), 2.0)
			_enh_commit!(st, _enh_decorrstretch(st.I, pct / 100 / 2), "decorr stretch")
		else
			error("Image Enhance: unknown op '$op'")
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Image Enhance ($op) FAILED: $(sprint(showerror, e))")
		@warn "Image Enhance FAILED" op exception=(e,)
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
