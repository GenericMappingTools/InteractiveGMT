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
	I::GMTimage                     # the source image at FULL precision (a 16-bit band's original)
	Ig::GMTimage                    # grey 2-D working image, in the source's own type (UInt8 or UInt16)
	counts::Vector{Float64}         # `_image_hist`'s histogram of Ig: 256 grey levels, or DN bins
	lo::Float64                     # the value of bin 1 (0 for 8-bit)
	binw::Float64                   # the bin width in data values (1 for 8-bit and for DN bins)
	mask::GMTimage                  # current mask, UInt8 0/255, georef of Ig
	backup::GMTimage                # one-level undo
	labels::Matrix{Int32}           # last "Label matrix" run (empty until it is run)
end

const _BINSTATE = Dict{Ptr{Cvoid}, _BinState}()

# ---------------------------------------------------------------------------------------------
# Threshold estimators ported from thresholdit.m (the ones Mirone implements there itself). All
# take the image's histogram — 256 grey levels for 8-bit, DN bins for 16-bit (`_image_hist`)
# — and return a BIN POSITION, 0-based (= the grey level for 8-bit). `_bin_method_level` turns it
# into a data value. The arithmetic is thresholdit's with its 256 replaced by the bin count, so on
# an 8-bit image every level is what it always was.
# ---------------------------------------------------------------------------------------------

# graythresh (img_fun.m): Otsu's method — the threshold that maximizes the between-class variance.
# MATLAB returns a normalized [0 1] level and im2bw multiplies it by 255 for a uint8 image, which
# is exactly `idx - 1` here.
function _bin_otsu(counts::Vector{Float64})::Float64
	n   = length(counts)
	tot = sum(counts)
	tot <= 0 && return (n - 1) / 2
	p     = counts ./ tot
	omega = cumsum(p)
	mu    = cumsum(p .* collect(1.0:Float64(n)))
	mu_t  = mu[end]
	best  = -Inf
	nbest = 0
	sbest = 0.0                                    # sum of the tied bin positions (MATLAB's mean())
	for k in 1:n
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
# thresholdit's two inner sums are run as running sums — the SAME quantities, since
# -sum(h/c*log(h/c)) over a class = log(c) - sum(h*log h)/c — because its double loop is n^2 and a
# 16-bit band has tens of thousands of bins.
function _bin_maxentropy(counts::Vector{Float64})::Float64
	n    = length(counts)
	npix = sum(counts)
	npix <= 0 && return (n - 1) / 2
	hn = counts ./ npix
	hlh = [h > 0 ? h * log(h) : 0.0 for h in hn]
	tot = sum(hlh)
	ent = zeros(n)
	cl = 0.0;  sl = 0.0
	for t in 1:n
		cl += hn[t];  sl += hlh[t]
		hl = cl > 0 ? log(cl) - sl / cl : 0.0
		ch = 1.0 - cl                              # constraint cl + ch = 1
		hh = ch > 0 ? log(ch) - (tot - sl) / ch : 0.0
		ent[t] = hl + hh
	end
	h_max = ent[1];  threshold = 0.0
	for t in 2:n
		if ent[t] > h_max
			h_max = ent[t];  threshold = Float64(t - 1)
		end
	end
	return threshold
end

# minCE (thresholdit.m): the threshold that minimizes the cross entropy between the image and its
# two-level (class-mean) reconstruction. Its class sums run as running sums, as in maxentropy.
function _bin_mince(counts::Vector{Float64})::Float64
	n    = length(counts)
	npix = sum(counts)
	npix <= 0 && return (n - 1) / 2
	hn = counts ./ npix
	imEntropy = 0.0
	for i in 1:n
		imEntropy += i * hn[i] * log(i)
	end
	ihTot = sum(i * hn[i] for i in 1:n);  hTot = sum(hn)
	CE = zeros(n)
	lowV = 0.0;  lowSum = 0.0
	for t in 1:n
		lowV += t * hn[t];  lowSum += hn[t]
		highV = ihTot - lowV;  highSum = hTot - lowSum
		lowValue  = lowSum  > 0 ? lowV  / lowSum  : 1.0
		highValue = highSum > 0 ? highV / highSum : 1.0
		CE[t] = imEntropy - lowV * log(lowValue) - highV * log(highValue)
	end
	D_min = CE[1];  threshold = 0.0
	for t in 2:n
		if CE[t] < D_min
			D_min = CE[t];  threshold = Float64(t - 1)
		end
	end
	return threshold
end

# Isodata — GMT.jl's `isodata` (img_funs.jl), which only takes an 8-bit image, run on the histogram
# itself so a 16-bit band's DN bins go through the same iteration. Its edges are 0..n-1 and its
# result is normalised the way GMT's is ((T-1)/(edges[end]-1) of the top), as a bin position.
function _bin_isodata(counts::Vector{Float64})::Float64
	n = length(counts)
	edges = collect(0.0:Float64(n - 1))
	tot = sum(counts)
	tot <= 0 && return (n - 1) / 2
	Tp = round(Int, sum(edges .* counts) / tot)
	mean_of(r) = (s = sum(view(counts, r)); s > 0 ? sum(view(edges, r) .* view(counts, r)) / s : 0.0)
	clampT(T) = clamp(T, 1, n)
	T = round(Int, (mean_of(clampT(Tp):n) + mean_of(1:clampT(Tp))) / 2)
	while abs(T - Tp) >= 1
		Tp = T
		T = round(Int, (mean_of(clampT(Tp):n) + mean_of(1:clampT(Tp))) / 2)
	end
	return Float64(round(Int, (T - 1) / (edges[end] - 1) * (n - 1)))
end

# triangle_th (thresholdit.m, Zack et al. 1977): the level whose histogram value lies farthest from
# the line joining the histogram peak to the far end of its longer tail. The `a + mean(position)`
# arithmetic below is Mirone's own (position is 1-based, so the level sits one bin above the
# geometric answer) — ported as-is, on purpose: this is THE algorithm, not a lookalike of it.
function _bin_triangle(counts::Vector{Float64})::Float64
	num_bins = length(counts)
	h, xmax = findmax(counts)
	h <= 0 && return (num_bins - 1) / 2
	indi = findall(>(h / 10000), counts)
	isempty(indi) && return (num_bins - 1) / 2
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
	return (level / num_bins) * (num_bins - 1)
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

# img_fun.m im2bw: `A = (A >= top*level)`, level already in data units here (grey levels for 8-bit,
# DNs for 16-bit), rounded into the image's own type.
function _bin_im2bw(Ig::GMTimage, level::Float64)
	T = eltype(Ig.image)
	return _bin_mask(Ig, Ig.image .>= round(T, clamp(level, 0.0, Float64(typemax(T)))))
end

# thresholdit.m binarize_it: `I(img < threshold) = 0; I(img > threshold) = 1` — a pixel EQUAL to the
# threshold stays 0, in both polarities (`negative` only swaps which side gets the 1).
function _bin_binarize_it(Ig::GMTimage, level::Float64, negative::Bool)
	thr = clamp(level, 0.0, Float64(typemax(eltype(Ig.image))))
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
	# The displayed object of a 16-bit image is its 8-bit display copy; the image IS the original,
	# and it is thresholded in its own DNs.
	O = get(get(_IMG_ORIG, scene, Dict{String,GMTimage}()), String(srcname), nothing)
	O === nothing || (I = O)
	(eltype(I.image) == UInt8 || eltype(I.image) == UInt16) ||
		error("Binarize works on 8- or 16-bit images (this one is $(eltype(I.image)))")
	# EXACTLY thresholdit.m's opening: `if (ndims(handles.img) == 3), handles.img = rgb2gray(...)`.
	# Nothing else is done to the data — no palette expansion, no re-scaling.
	Ig = ndims(I.image) == 3 && size(I.image, 3) >= 3 ? _bin_rgb2gray(I) : _bin_band1(I)
	H  = _image_hist(Ig)                                 # THE image histogram (imagehisto.jl)
	cnt = H.counts[1]
	mask0 = _bin_im2bw(Ig, 0.0)
	st = _BinState(scene, String(srcname), I, Ig, cnt, H.lo, H.binw, mask0, mask0, Matrix{Int32}(undef, 0, 0))
	level = _bin_method_level(st, "otsu")
	st.mask = st.backup = _bin_im2bw(Ig, level)          # thresholdit.m opens with img_fun('im2bw')
	_BINSTATE[dlg] = st
	# Which image, and all five levels on ITS histogram in one line — so the numbers can be compared
	# against Mirone's thresholdit on the same image without clicking through the buttons one by one.
	nlev = count(>(0), cnt)                              # distinct levels actually present
	kind = ndims(I.image) == 3 ? "RGB->grey" : (I.n_colors > 0 ? "indexed($(I.n_colors))->grey" : "grey")
	_viewer_log_info(scene, "Binarize: image \"$(isempty(srcname) ? "(primary)" : srcname)\", " *
		"$(size(Ig.image, 1))x$(size(Ig.image, 2)) px, $(eltype(Ig.image)) $kind, $nlev distinct levels")
	lv(nm) = round(Int, _bin_method_level(st, nm))
	_viewer_log_info(scene, "Binarize levels — Otsu=$(round(Int, level)) MaxEnt=$(lv("maxent")) " *
		"MinCE=$(lv("mince")) Isodata=$(lv("isodata")) Triangle=$(lv("triangle"))")
	H.data && ccall(_fn(:gmtvtk_binarize_set_data_range), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble), dlg, H.lo, H.hi)
	ccall(_fn(:gmtvtk_binarize_set_histogram), Cvoid, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), dlg, cnt, Cint(length(cnt)))
	_bin_push!(st, dlg; level=level)
	return
end

# The grey working image. An 8-bit RGB goes through GMT.jl's `rgb2gray` (it reads band-planar and
# 'P' alike); GMT's is 8-bit only, so a 16-bit RGB takes the same luminance (0.299, 0.587, 0.114)
# here, in UInt16.
function _bin_rgb2gray(I::GMTimage)::GMTimage
	eltype(I.image) == UInt8 && return GMT.rgb2gray(I)
	Ib = _to_band_planar(I)
	S  = Ib.image
	g  = Matrix{UInt16}(undef, size(S, 1), size(S, 2))
	@inbounds for j in eachindex(g)
		g[j] = round(UInt16, 0.299 * S[j] + 0.587 * S[j + length(g)] + 0.114 * S[j + 2length(g)])
	end
	return GMT.mat2img(g, Ib)
end

# A one-band image as the 2-D working image it already is (a 1-band 3-D array is its first band).
_bin_band1(I::GMTimage)::GMTimage =
	ndims(I.image) == 2 ? I : GMT.mat2img(I.image[:, :, 1], I)

# The five automatic methods, as a DATA value: the estimators give a bin position on the image's
# histogram, bin i being `lo + (i-1)*binw` (`_image_hist`). For 8-bit that is the grey level itself.
function _bin_method_level(st::_BinState, name::AbstractString)::Float64
	pos = name == "otsu"     ? _bin_otsu(st.counts)       :
	      name == "maxent"   ? _bin_maxentropy(st.counts) :
	      name == "mince"    ? _bin_mince(st.counts)      :
	      name == "isodata"  ? _bin_isodata(st.counts)    :
	      name == "triangle" ? _bin_triangle(st.counts)   : error("Binarize: unknown method '$name'")
	return st.lo + pos * st.binw
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
		_viewer_log_info(st.scene, "Binarize: $ncomp connected components added as \"$name\"")
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
		_tool_failed(scene, "Binarize ($op)", e)
		return Cint(0)
	end
end

function _register_binarize()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_binarize, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_binarize_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
