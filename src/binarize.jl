# binarize.jl — Image > "Binarize Image" (port of Mirone's src_figs/thresholdit.m). Threshold this
# window's image into a black-and-white mask: five automatic methods (Otsu, Maximum Entropy, Minimum
# Cross-Entropy, Isodata, Triangle) or a hand-dragged level / [lo,hi] band on the grey histogram,
# then clean the mask up (dust, holes, connected-component labels) with a one-level undo.
#
# ALL the maths lives here, in ONE place (SACRED_LAW.md): what the dialog previews IS what "Good, I
# like it" commits — the C++ side (BinarizeDialog, 70_window.cpp) only paints the mask this file
# pushes back and sends "op;args" through g_juliaBinarize. Nothing is re-thresholded C++-side.
#
# Nothing is re-implemented that GMT.jl/GDAL already do: `rgb2gray`, `histogray`, `binarize`,
# `isodata`, `imcomplement`, `imfill`, `bwdist` and `bwconncomp` come from GMT.jl, and
# `bwareaopen` is GDAL's GDALSieveFilter. Only the four threshold estimators Mirone carries in
# thresholdit.m itself (Otsu/graythresh, Maximum Entropy, Minimum Cross-Entropy, Triangle) are
# ported below, from that file.
#
# A REFERENCED image keeps its coordinates in the mask: every mask is built with `mat2img(mat, I)`,
# which copies range/inc/x/y/registration/proj4/wkt/epsg from the source image, and it enters the
# scene through the same `_add_image_to_scene` every other georeferenced image uses.

# Per-dialog state: the source image, the grey working copy (thresholdit's handles.img), its
# histogram, the CURRENT mask, the one-level undo copy (handles.mask_backup) and the last label
# matrix. Keyed by the C++ dialog pointer, dropped on op "close".
mutable struct _BinState
	scene::Ptr{Cvoid}
	srcname::String                 # Scene Objects name of the source image ("" = window primary)
	I::GMTimage                     # the source image, as displayed
	Ig::GMTimage                    # grey UInt8 2-D working image
	counts::Vector{Float64}         # 256-bin grey-level histogram
	mask::GMTimage                  # current mask, UInt8 0/255, georef of Ig
	backup::GMTimage                # one-level undo
	labels::Matrix{Int32}           # last "Label matrix" run (empty until it is run)
end

const _BINSTATE = Dict{Ptr{Cvoid}, _BinState}()

# ---------------------------------------------------------------------------------------------
# Threshold estimators ported from thresholdit.m (the ones Mirone implements there itself). All
# take the 256-bin grey histogram and return a level in [0 255].
# ---------------------------------------------------------------------------------------------

# graythresh (img_fun.m): Otsu's method — the threshold that maximizes the between-class variance.
# MATLAB returns a normalized [0 1] level and im2bw multiplies it by 255 for a uint8 image, which
# is exactly `idx - 1` here.
function _bin_otsu(counts::Vector{Float64})::Float64
	tot = sum(counts)
	tot <= 0 && return 128.0
	p     = counts ./ tot
	omega = cumsum(p)
	mu    = cumsum(p .* collect(1.0:256.0))
	mu_t  = mu[end]
	best  = -Inf
	nbest = 0
	sbest = 0.0                                    # sum of the tied bin positions (MATLAB's mean())
	for k in 1:256
		den = omega[k] * (1.0 - omega[k])
		den <= 0 && continue
		s = (mu_t * omega[k] - mu[k])^2 / den
		if s > best
			best = s;  nbest = 1;  sbest = Float64(k)
		elseif s == best
			nbest += 1;  sbest += Float64(k)
		end
	end
	nbest == 0 && return 0.0
	return sbest / nbest - 1.0
end

# maxentropy (thresholdit.m): the threshold that maximizes the sum of the two classes' entropies.
function _bin_maxentropy(counts::Vector{Float64})::Float64
	npix = sum(counts)
	npix <= 0 && return 128.0
	hn = counts ./ npix
	c  = cumsum(hn)
	hl = zeros(256);  hh = zeros(256)
	for t in 1:256
		cl = c[t]
		if cl > 0
			for i in 1:t
				hn[i] > 0 && (hl[t] -= (hn[i] / cl) * log(hn[i] / cl))
			end
		end
		ch = 1.0 - cl                              # constraint cl + ch = 1
		if ch > 0
			for i in (t+1):256
				hn[i] > 0 && (hh[t] -= (hn[i] / ch) * log(hn[i] / ch))
			end
		end
	end
	ent = hl .+ hh
	h_max = ent[1];  threshold = 0.0
	for t in 2:256
		if ent[t] > h_max
			h_max = ent[t];  threshold = Float64(t - 1)
		end
	end
	return threshold
end

# minCE (thresholdit.m): the threshold that minimizes the cross entropy between the image and its
# two-level (class-mean) reconstruction.
function _bin_mince(counts::Vector{Float64})::Float64
	npix = sum(counts)
	npix <= 0 && return 128.0
	hn = counts ./ npix
	imEntropy = 0.0
	for i in 1:256
		imEntropy += i * hn[i] * log(i)
	end
	CE = zeros(256)
	for t in 1:256
		lowValue = 0.0;  lowSum = 0.0
		for i in 1:t
			lowValue += i * hn[i];  lowSum += hn[i]
		end
		lowValue = lowSum > 0 ? lowValue / lowSum : 1.0
		highValue = 0.0;  highSum = 0.0
		for i in (t+1):256
			highValue += i * hn[i];  highSum += hn[i]
		end
		highValue = highSum > 0 ? highValue / highSum : 1.0
		lowEntropy = 0.0;   logLowVal  = log(lowValue)
		for i in 1:t
			lowEntropy += i * hn[i] * logLowVal
		end
		highEntropy = 0.0;  logHighVal = log(highValue)
		for i in (t+1):256
			highEntropy += i * hn[i] * logHighVal
		end
		CE[t] = imEntropy - lowEntropy - highEntropy
	end
	D_min = CE[1];  threshold = 0.0
	for t in 2:256
		if CE[t] < D_min
			D_min = CE[t];  threshold = Float64(t - 1)
		end
	end
	return threshold
end

# triangle_th (thresholdit.m, Zack et al. 1977): the level whose histogram value lies farthest from
# the line joining the histogram peak to the far end of its longer tail. The `a + mean(position)`
# arithmetic below is Mirone's own (position is 1-based, so the level sits one bin above the
# geometric answer) — ported as-is, on purpose: this is THE algorithm, not a lookalike of it.
function _bin_triangle(counts::Vector{Float64})::Float64
	num_bins = 256
	h, xmax = findmax(counts)
	h <= 0 && return 128.0
	indi = findall(>(h / 10000), counts)
	isempty(indi) && return 128.0
	fnz = indi[1];  lnz = indi[end]
	lspan = xmax - fnz;  rspan = lnz - xmax
	local leh::Vector{Float64}, a::Int, b::Int, isflip::Bool
	if rspan > lspan                               # flip so the long tail is on the right of the peak
		leh = reverse(counts);  a = num_bins - lnz + 1;  b = num_bins - xmax + 1;  isflip = true
	else
		leh = counts;           a = fnz;                b = xmax;                 isflip = false
	end
	b <= a && return Float64(xmax - 1)             # degenerate (peak at the first non-zero bin)
	m  = h / (b - a)                               # the straight line, x shifted by a
	x1 = collect(0.0:Float64(b - a))
	y1 = leh[a:b]
	beta = y1 .+ x1 ./ m
	x2 = beta ./ (m + 1 / m)
	y2 = m .* x2
	L  = sqrt.((y2 .- y1).^2 .+ (x2 .- x1).^2)
	Lmax = maximum(L)
	pos  = findall(==(Lmax), L)
	level = a + sum(pos) / length(pos)
	isflip && (level = num_bins - level + 1)
	return (level / num_bins) * 255.0
end

# ---------------------------------------------------------------------------------------------
# Mask <-> dialog plumbing
# ---------------------------------------------------------------------------------------------

# Decimated top-down grey buffer of `I` for the dialog's preview label. The row order and the
# band/interleave handling come from the SAME `_pixaccess`/`_north_first` pair the drape/texture
# path uses (drape.jl), so a preview can never disagree with how the window paints the image.
# Returns (buf, w, h) with buf row-major, TOP row first — what QImage::Format_Grayscale8 wants.
function _bin_preview_buf(I::GMTimage)
	S   = I.image
	lay = I.layout
	d3  = ndims(S) == 3
	nb  = d3 ? size(S, 3) : 1
	rowmajor    = length(lay) >= 2 && lay[2] == 'R'
	north_first = _north_first(lay, rowmajor)
	nlon, nlat  = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix  = _pixaccess(S, lay, d3, nb, rowmajor, nlon, nlat)
	step = max(1, ceil(Int, max(nlon, nlat) / 1024))          # a 511x407 label needs no more
	cols = collect(1:step:nlon);  rows = collect(1:step:nlat)
	w = length(cols);  h = length(rows)
	buf = Vector{UInt8}(undef, w * h)
	k = 1
	@inbounds for r in rows                                   # r counts from the TOP
		lat = north_first ? r : (nlat - r + 1)
		for c in cols
			buf[k] = pix(lat, c, 1);  k += 1
		end
	end
	return buf, w, h
end

# Push the current mask (and, when a method/drag decided them, the histogram handles) into the
# dialog. A negative level/lo/hi leaves that handle where the user put it.
function _bin_push!(st::_BinState, dlg::Ptr{Cvoid}; level::Float64=-1.0, lo::Float64=-1.0, hi::Float64=-1.0)
	buf, w, h = _bin_preview_buf(st.mask)
	ccall(_fn(:gmtvtk_binarize_set_preview), Cvoid,
	      (Ptr{Cvoid}, Cint, Cint, Ptr{UInt8}, Cdouble, Cdouble, Cdouble),
	      dlg, Cint(w), Cint(h), buf, level, lo, hi)
	return
end

# Replace the current mask, keeping ONE level of undo (thresholdit's update_bw).
function _bin_setmask!(st::_BinState, m::GMTimage)
	st.backup = st.mask
	st.mask   = m
	return
end

# Bool matrix -> the 0/255 UInt8 mask image, with the working image's georef (mat2img(mat, I)).
# 0/255 are grey VALUES, not palette indices, so an indexed source's colormap — which mat2img copies
# with the georef — is dropped (_img_drop_palette!, drape.jl); left on, the display would look them
# up in the palette.
_bin_mask(Ig::GMTimage, t::AbstractMatrix{Bool}) =
	_img_drop_palette!(GMT.mat2img(map(v -> v ? 0xff : 0x00, t), Ig))
_bin_mask_img(st::_BinState, t::AbstractMatrix{Bool}) = _bin_mask(st.Ig, t)

# --- the four mask rules thresholdit.m uses, each kept exactly as it is written there ------------
# They are NOT interchangeable and they are NOT GMT.binarize (which is `>=` everywhere and refuses a
# band touching 0 or 255): the Otsu button goes through img_fun('im2bw'), the other four method
# buttons through binarize_it, and the two histogram drags have their own comparisons again.

# img_fun.m im2bw for a uint8 image: `A = (A >= 255*level)`, level already in grey units here.
_bin_im2bw(Ig::GMTimage, level::Float64) =
	_bin_mask(Ig, Ig.image .>= round(UInt8, clamp(level, 0.0, 255.0)))

# thresholdit.m binarize_it: `I(img < threshold) = 0; I(img > threshold) = 1` — a pixel EQUAL to the
# threshold stays 0, in both polarities (`negative` only swaps which side gets the 1).
function _bin_binarize_it(Ig::GMTimage, level::Float64, negative::Bool)
	thr = clamp(level, 0.0, 255.0)
	t = negative ? (Ig.image .< thr) : (Ig.image .> thr)
	return _bin_mask(Ig, t)
end

# VLUpFcn (the dragged single line): `bw = (img > level)`, or `(img <= level)` when Revert is on.
_bin_vline_mask(Ig::GMTimage, level::Float64, revert::Bool) =
	_bin_mask(Ig, revert ? (Ig.image .<= level) : (Ig.image .> level))

# BoxUpFcn (the dragged window): `bw = (img >= x_min & img <= x_max)`, or the complement of that
# band (`img < x_min | img > x_max`) when Revert is on.
_bin_box_mask(Ig::GMTimage, lo::Float64, hi::Float64, revert::Bool) =
	_bin_mask(Ig, revert ? ((Ig.image .< lo) .| (Ig.image .> hi)) :
	                       ((Ig.image .>= lo) .& (Ig.image .<= hi)))

# ---------------------------------------------------------------------------------------------
# The ops the dialog sends
# ---------------------------------------------------------------------------------------------

# op "init": grab the window's image, grey it, histogram it and hand back the first mask — Otsu,
# thresholdit.m's own opening move (img_fun('im2bw') with no level = graythresh).
function _bin_init(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, wanted::AbstractString="")
	# `wanted` = the Scene Objects name the caller pointed at (the image handle's "Binarize Image…"
	# sends it). Empty -> the window's primary image. Without this a window holding several images
	# (a basemap tile + a dropped picture, say) would always threshold whichever came first, i.e.
	# not necessarily the one on screen — and then every level would look "wrong" next to Mirone's.
	srcname, I = isempty(wanted) ? _find_object_named(scene, :image) :
	                               (String(wanted), _find_object(scene, :image, wanted))
	(I isa GMTimage) || error("No image in this window to binarize")
	Iu = eltype(I.image) == UInt8 ? I : _stretch_to_u8(I)      # 16-bit band -> 8-bit grey levels
	# EXACTLY thresholdit.m's opening: `if (ndims(handles.img) == 3), handles.img = rgb2gray(...)`.
	# Nothing else is done to the data — no palette expansion, no re-scaling.
	Ig = ndims(Iu.image) == 3 ? GMT.rgb2gray(Iu) : Iu          # rgb2gray reads both band-planar and 'P'
	counts, _ = GMT.histogray(Ig; band=1)
	cnt = Float64.(counts)
	level = _bin_otsu(cnt)
	mask  = _bin_im2bw(Ig, level)                        # thresholdit.m opens with img_fun('im2bw')
	st = _BinState(scene, String(srcname), I, Ig, cnt, mask, mask, Matrix{Int32}(undef, 0, 0))
	_BINSTATE[dlg] = st
	# Which image, and all five levels on ITS histogram in one line — so the numbers can be compared
	# against Mirone's thresholdit on the same image without clicking through the buttons one by one.
	nlev = count(>(0), cnt)                              # distinct grey levels actually present
	kind = ndims(I.image) == 3 ? "RGB->grey" : (I.n_colors > 0 ? "indexed($(I.n_colors))->grey" : "grey")
	_viewer_log_error(scene, "Binarize: image \"$(isempty(srcname) ? "(primary)" : srcname)\", " *
		"$(size(Ig.image, 1))x$(size(Ig.image, 2)) px, $kind, $nlev distinct levels")
	_viewer_log_error(scene, "Binarize levels — Otsu=$(round(Int, level)) " *
		"MaxEnt=$(round(Int, _bin_maxentropy(cnt))) MinCE=$(round(Int, _bin_mince(cnt))) " *
		"Isodata=$(GMT.isodata(Ig; band=1)) Triangle=$(round(Int, _bin_triangle(cnt)))")
	ccall(_fn(:gmtvtk_binarize_set_histogram), Cvoid, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), dlg, cnt, Cint(256))
	_bin_push!(st, dlg; level=level)
	return
end

# The five automatic methods. Isodata is GMT.jl's own `isodata`; the other four are the ports above.
function _bin_method_level(st::_BinState, name::AbstractString)::Float64
	name == "otsu"     && return _bin_otsu(st.counts)
	name == "maxent"   && return _bin_maxentropy(st.counts)
	name == "mince"    && return _bin_mince(st.counts)
	name == "isodata"  && return Float64(GMT.isodata(st.Ig; band=1))
	name == "triangle" && return _bin_triangle(st.counts)
	error("Binarize: unknown method '$name'")
end

# A method button. Mirone routes them DIFFERENTLY and that difference is kept: push_Otsu_CB goes
# through img_fun('im2bw') (`>=`) and then complements the whole mask when Revert is ticked, while
# push_maxEntropy/minCrossEntropy/isodata/triang_CB all go through helper_thresh -> binarize_it
# (strict `>`, ties to 0, `negative` swapping the sides).
function _bin_method!(st::_BinState, name::AbstractString, level::Float64, revert::Bool)
	if name == "otsu"
		bw = _bin_im2bw(st.Ig, level)
		revert && (bw = GMT.imcomplement(bw))
		_bin_setmask!(st, bw)
	else
		_bin_setmask!(st, _bin_binarize_it(st.Ig, level, revert))
	end
	return
end

# The dragged single line (VLUpFcn).
_bin_vline!(st::_BinState, level::Float64, revert::Bool) =
	_bin_setmask!(st, _bin_vline_mask(st.Ig, level, revert))

# The dragged window (BoxUpFcn): inside [lo,hi] is white, outside black.
_bin_window!(st::_BinState, lo::Float64, hi::Float64, revert::Bool) =
	_bin_setmask!(st, _bin_box_mask(st.Ig, lo, hi, revert))

# "Clean white dust" (push_cleanDust_CB): drop the white connected components smaller than `ds`
# pixels, then put back the removed pixels that sit within 2 px of what survived — so thinning a
# real object's fringe doesn't eat it.
#
# The drop is done on GMT.jl's `bwconncomp` (Leptonica) component list, NOT on GMT.jl's
# `bwareaopen`: that one is GDAL's GDALSieveFilter, which (a) MERGES a small blob into its largest
# neighbour instead of removing it — not what MATLAB's bwareaopen, hence thresholdit.m, means —
# and (b) fails intermittently here ("Access in DatasetRasterIO failed", ~40 % of calls on a
# 2579x3602 mask, same mask succeeding on the next attempt).
function _bin_cleandust!(st::_BinState, ds::Int)
	cc = GMT.bwconncomp(st.mask; conn=8)               # MATLAB bwareaopen's default connectivity
	cur  = st.mask.image .!= 0
	kept = falses(size(cur))
	@inbounds for k in 1:cc.num_objects
		length(cc.pixel_list[k]) < ds && continue
		for idx in cc.pixel_list[k]
			kept[idx] = true
		end
	end
	removed = cur .⊻ kept
	D = GMT.bwdist(kept)
	out = kept .| (removed .& (D .<= 2))
	_bin_setmask!(st, _bin_mask_img(st, out))
	return
end

# "Label matrix" (push_labelimg_CB). n == 0: paint every connected component in its own colour and
# add THAT as a new scene element (Mirone opened a second figure; the derived-variable display law
# says same window, new named row). n > 0: the mask becomes that one component.
function _bin_label!(st::_BinState, n::Int)
	cc = GMT.bwconncomp(st.mask; conn=8)
	ncomp = cc.num_objects
	(n > ncomp) && error("Label $n is higher than the number of components in the image ($ncomp)")
	if isempty(st.labels) || size(st.labels) != size(st.mask.image)
		st.labels = zeros(Int32, size(st.mask.image))
	else
		fill!(st.labels, Int32(0))
	end
	@inbounds for k in 1:ncomp
		for idx in cc.pixel_list[k]
			st.labels[idx] = Int32(k)
		end
	end
	if n == 0
		ncomp < 1 && error("The mask has no connected components to label")
		C = GMT.makecpt(cmap=:jet, range=(1, ncomp + 1, 1))
		cm = C.colormap                                          # ncomp x 3 (or more), 0..1
		rgb = zeros(UInt8, size(st.labels, 1), size(st.labels, 2), 3)
		@inbounds for k in eachindex(st.labels)
			lb = st.labels[k]
			lb == 0 && continue                                  # background stays black
			row = min(Int(lb), size(cm, 1))
			rgb[k]                       = round(UInt8, 255 * cm[row, 1])
			rgb[k +   length(st.labels)] = round(UInt8, 255 * cm[row, 2])
			rgb[k + 2length(st.labels)]  = round(UInt8, 255 * cm[row, 3])
		end
		Ilab = GMT.mat2img(rgb, st.Ig)
		name = _bin_name(st, "labels")
		_add_image_to_scene(st.scene, Ilab, name; promote=false)
		_adopt_derived!(st.scene, name, Ilab)     # the ONE derived-variable transition (grid.jl)
		_viewer_log_error(st.scene, "Binarize: $ncomp connected components added as \"$name\"")
	else
		_bin_setmask!(st, _bin_mask_img(st, st.labels .== Int32(n)))
	end
	return
end

# Name of a derived product: "<source image> (mask)" etc., or a bare name when the source is the
# window's unnamed primary image.
_bin_name(st::_BinState, what::AbstractString) =
	isempty(st.srcname) ? "Image ($what)" : "$(st.srcname) ($what)"

# "Good, I like it" (push_OK_CB). Without "Apply to original" the MASK itself becomes a new scene
# element; with it, the SOURCE image is masked — its false pixels either made transparent (Alpha)
# or painted with the background colour. Either way the result is a NEW named, checked element and
# the source is unchecked (SACRED_LAW.md derived-variable display law), and the georeferencing
# rides along because every image here was built with mat2img(mat, I).
function _bin_commit!(st::_BinState, applyOrig::Bool, useAlpha::Bool)
	if !applyOrig
		Iout = st.mask
		name = _bin_name(st, "mask")
	else
		Iout = _bin_masked_image(st, useAlpha)
		name = _bin_name(st, useAlpha ? "masked, alpha" : "masked")
	end
	# REPLACE, never pile up: committing twice under the same name trashes the previous result first.
	ccall(_fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), st.scene, name)
	_forget_object!(st.scene, :image, name)
	_add_image_to_scene(st.scene, Iout, name; promote=false)
	_adopt_derived!(st.scene, name, Iout)         # the ONE derived-variable transition (grid.jl)
	return
end

# The SOURCE image masked by the current mask (push_OK_CB's "Apply to original" branch): where the
# mask is false the pixels either go transparent (an alpha band, `useAlpha`) or take the background
# colour. Band indexing is raw (S[k], S[k+npix], …), so the source is made band-planar first — a
# 'P' (pixel-interleaved) image would otherwise scramble into colour noise.
function _bin_masked_image(st::_BinState, useAlpha::Bool)::GMTimage
	Isrc = _to_band_planar(eltype(st.I.image) == UInt8 ? st.I : _stretch_to_u8(st.I))
	S    = Isrc.image
	keep = st.mask.image .!= 0
	nb   = ndims(S) == 3 ? size(S, 3) : 1
	npix = length(keep)
	if useAlpha                                            # RGB + an alpha band from the mask
		out = zeros(UInt8, size(keep, 1), size(keep, 2), 4)
		@inbounds for k in 1:npix                          # a 1-band source greys into all three
			out[k]         = S[k]
			out[k + npix]  = nb >= 3 ? S[k + npix]  : S[k]
			out[k + 2npix] = nb >= 3 ? S[k + 2npix] : S[k]
			out[k + 3npix] = keep[k] ? 0xff : 0x00
		end
		return GMT.mat2img(out, Isrc)
	end
	bg  = _bin_bg_color()                                  # false pixels painted the background colour
	out = copy(S)
	@inbounds for k in 1:npix
		keep[k] && continue
		if nb >= 3
			out[k] = bg[1];  out[k + npix] = bg[2];  out[k + 2npix] = bg[3]
		else
			out[k] = bg[1]
		end
	end
	return GMT.mat2img(out, Isrc)
end

# The colour masked-out pixels get when "Apply to original" is used without Alpha — Mirone's
# handMir.bg_color, which for a plain image window is white.
_bin_bg_color()::NTuple{3,UInt8} = (0xff, 0xff, 0xff)

# ---------------------------------------------------------------------------------------------
# The C callback: params = "op;arg;arg" (see JuliaBinarizeFn, 30_app.cpp). 1 on success, 0 on
# failure — same yes/no contract as _on_clipgrid.
# ---------------------------------------------------------------------------------------------
function _on_binarize(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		p  = split(unsafe_string(cparams), ';')
		op = p[1]
		if op == "close"
			delete!(_BINSTATE, dlg)
			return Cint(1)
		end
		if op == "init"
			_bin_init(scene, dlg, length(p) >= 2 ? strip(p[2]) : "")
			return Cint(1)
		end
		st = get(_BINSTATE, dlg, nothing)
		(st isa _BinState) || error("Binarize: this dialog has no image loaded")
		level = -1.0;  lo = -1.0;  hi = -1.0
		if op == "method"
			revert = length(p) >= 3 && p[3] == "1"
			level  = _bin_method_level(st, p[2])
			_bin_method!(st, p[2], level, revert)
			# NO logging here: _viewer_log_error pops the viewer's Errors tab to the front, which pulls
			# the keyboard focus/activation off the dialog — i.e. it visibly kicked the highlight off
			# the button that was just pressed. The levels are logged ONCE, at init, instead.
		elseif op == "level"
			revert = length(p) >= 3 && p[3] == "1"
			level  = parse(Float64, p[2])
			_bin_vline!(st, level, revert)
		elseif op == "window"
			revert = length(p) >= 4 && p[4] == "1"
			lo = parse(Float64, p[2]);  hi = parse(Float64, p[3])
			_bin_window!(st, lo, hi, revert)
			lo = -1.0;  hi = -1.0                        # the handles are already where the user left them
		elseif op == "revert"
			_bin_setmask!(st, GMT.imcomplement(st.mask))  # complement the CURRENT mask, not a re-threshold
		elseif op == "dust"
			ds = something(tryparse(Int, strip(p[2])), 15)
			_bin_cleandust!(st, max(ds, 1))
		elseif op == "fill"
			_bin_setmask!(st, GMT.imfill(st.mask; conn=4))
		elseif op == "label"
			n = something(tryparse(Int, strip(p[2])), 0)
			_bin_label!(st, max(n, 0))
		elseif op == "undo"
			st.mask, st.backup = st.backup, st.mask
		elseif op == "ok"
			_bin_commit!(st, p[2] == "1", length(p) >= 3 && p[3] == "1")
		else
			error("Binarize: unknown op '$op'")
		end
		_bin_push!(st, dlg; level=level, lo=lo, hi=hi)
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Binarize ($op) FAILED: $(sprint(showerror, e))")
		@warn "Binarize FAILED" op exception=(e,)
		return Cint(0)
	end
end

function _register_binarize()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_binarize, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_binarize_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
