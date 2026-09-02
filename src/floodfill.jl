# Image > Shape detector — the compute half of Mirone's src_figs/floodfill.m ("color segmentation or
# painting like the magick wand").
#
# Mirone grows the region with cvlib_mex('floodfill') = OpenCV cvFloodFill. Reading mex/cvlib_mex.c
# settles what that means: `ffill_case` is hard-wired to 1, so the flags always carry
# CV_FLOODFILL_FIXED_RANGE, and `up_diff = lo_diff = Tolerance` ("Don't see any reason to have them
# different"). FIXED range = every candidate is compared with the SEED, not with the neighbour that
# reached it, and for a 3-band image EVERY channel must be inside seed +/- tol. That is what
# `_ff_flood_mask` implements — the algorithm itself, not a lookalike filter.
#
# VTK's vtkImageThresholdConnectivity would cover the 1-band case, but it thresholds a single scalar
# component; the RGB wand needs the per-channel test, and floodfill.m converts indexed to RGB on
# purpose ("the floodfill here works bad with indexed"). So the growth is ours; the two places GDAL
# already does the job better — turning a mask into polygons and simplifying them — are GDAL's
# (GMT.polygonize = GDALPolygonize, GMT.simplify = GEOS).

# ---------------------------------------------------------------------------------------------
# Pixel access. A GMTimage may be stored row-major ("TRB…", dim 1 = column) or column-major
# ("TCB…", dim 1 = row); everything below works on a normalised (ny, nx, nbands) array and converts
# back on the way out, so no algorithm has to care.
# ---------------------------------------------------------------------------------------------
_ff_rowmajor(I::GMTimage) = length(I.layout) >= 2 && I.layout[2] == 'R'

function _ff_pixels(I::GMTimage)::Array{UInt8,3}
	# An INDEXED image is expanded to its palette colours for the working array only — the image
	# itself stays indexed. floodfill.m does exactly this before the wand runs ("the floodfill here
	# works bad with indexed", so `img_fun('ind2rgb'...)`): every algorithm below compares COLOURS,
	# and an index number is not a colour. The palette is read by the shared `_img_palette`.
	if _img_is_indexed(I)
		S  = I.image
		L  = _img_palette(I)
		nL, nc = size(L, 1), min(size(L, 2), 3)
		rowmajor = _ff_rowmajor(I)
		ny, nx = rowmajor ? (size(S, 2), size(S, 1)) : (size(S, 1), size(S, 2))
		out = Array{UInt8,3}(undef, ny, nx, 3)
		@inbounds for c = 1:nx, r = 1:ny
			v = clamp(Int(rowmajor ? S[c, r] : S[r, c]) + 1, 1, nL)
			for b = 1:3  out[r, c, b] = L[v, b <= nc ? b : nc]  end
		end
		return out
	end
	A = I.image
	B = ndims(A) == 3 ? A : reshape(A, size(A, 1), size(A, 2), 1)
	return _ff_rowmajor(I) ? permutedims(B, (2, 1, 3)) : Array{UInt8,3}(B)
end

# The inverse: a normalised (ny, nx, nb) array back into an image that carries `I`'s georef+layout.
# The result holds real pixel VALUES (a mask, a segmentation), never indices, so the parent's palette
# — which `mat2img(mat, I)` copies along with the georef — is dropped.
function _ff_image(P::Array{UInt8,3}, I::GMTimage)::GMTimage
	B = _ff_rowmajor(I) ? permutedims(P, (2, 1, 3)) : P
	mat = size(B, 3) == 1 ? B[:, :, 1] : B
	J = GMT.mat2img(mat, I)
	J.layout = I.layout
	return _img_drop_palette!(J)
end

# ---------------------------------------------------------------------------------------------
# The wand itself
# ---------------------------------------------------------------------------------------------

# cvFloodFill with CV_FLOODFILL_FIXED_RANGE: grow from (r0,c0) over every connected pixel whose
# EVERY band lies in [seed-tol, seed+tol]. `conn` is 4 or 8, Mirone's radio_fourConn/radio_eightConn.
# Returns the mask in normalised (ny, nx) orientation.
function _ff_flood_mask(P::Array{UInt8,3}, r0::Int, c0::Int, tol::Int, conn::Int)::BitMatrix
	ny, nx, nb = size(P)
	(r0 < 1 || r0 > ny || c0 < 1 || c0 > nx) && error("Flood fill: the seed point is outside the image")
	lo = [UInt8(clamp(Int(P[r0, c0, k]) - tol, 0, 255)) for k = 1:nb]
	hi = [UInt8(clamp(Int(P[r0, c0, k]) + tol, 0, 255)) for k = 1:nb]
	mask = falses(ny, nx)
	inrange(r, c) = begin                        # every band inside the seed's window
		ok = true
		@inbounds for k = 1:nb
			v = P[r, c, k]
			(v < lo[k] || v > hi[k]) && (ok = false; break)
		end
		ok
	end
	inrange(r0, c0) || return mask               # the seed itself fails: nothing to grow
	# Stack of linear (r,c) indices. A plain flood is O(npixels) and needs no scanline trickery to
	# stay correct for both connectivities — the 8-connected diagonals are just two more offsets.
	stack = [(r0 - 1) * nx + c0]
	mask[r0, c0] = true
	dr = conn == 8 ? (-1, -1, -1, 0, 0, 1, 1, 1) : (-1, 0, 0, 1)
	dc = conn == 8 ? (-1, 0, 1, -1, 1, -1, 0, 1) : (0, -1, 1, 0)
	@inbounds while !isempty(stack)
		p = pop!(stack)
		r, c = fld(p - 1, nx) + 1, mod(p - 1, nx) + 1
		for k in eachindex(dr)
			rr, cc = r + dr[k], c + dc[k]
			(rr < 1 || rr > ny || cc < 1 || cc > nx) && continue
			mask[rr, cc] && continue
			inrange(rr, cc) || continue
			mask[rr, cc] = true
			push!(stack, (rr - 1) * nx + cc)
		end
	end
	return mask
end

# img_fun('bwmorph', mask, 'clean'): drop pixels with no 8-connected neighbour ("isolated pixels").
function _ff_clean(mask::BitMatrix)::BitMatrix
	ny, nx = size(mask)
	out = copy(mask)
	@inbounds for c = 1:nx, r = 1:ny
		mask[r, c] || continue
		keep = false
		for cc = max(c - 1, 1):min(c + 1, nx), rr = max(r - 1, 1):min(r + 1, ny)
			(rr == r && cc == c) && continue
			mask[rr, cc] && (keep = true; break)
		end
		keep || (out[r, c] = false)
	end
	return out
end

# img_fun('bwmorph', mask, 'dilate'): a 3x3 (ones) structuring element, Mirone's checkbox_useDilation
# ("find better limits between neighboring shapes").
function _ff_dilate(mask::BitMatrix)::BitMatrix
	ny, nx = size(mask)
	out = falses(ny, nx)
	@inbounds for c = 1:nx, r = 1:ny
		mask[r, c] || continue
		for cc = max(c - 1, 1):min(c + 1, nx), rr = max(r - 1, 1):min(r + 1, ny)
			out[rr, cc] = true
		end
	end
	return out
end

# ---------------------------------------------------------------------------------------------
# The Mahalanobis alternative (check_mahal): floodfill.m's colorseg + covmatrix + mahalanobis,
# the minimalist DIPUM version. The class is sampled from a 7x7 patch around the seed, and every
# pixel whose Mahalanobis distance to that class mean is <= Tolerance joins the mask. RGB only,
# which is why Mirone disables "Pick multiple shapes" while it is ticked.
# ---------------------------------------------------------------------------------------------
function _ff_mahal_mask(P::Array{UInt8,3}, r0::Int, c0::Int, T::Float32)::BitMatrix
	ny, nx, nb = size(P)
	nb == 3 || error("Mahalanobis segmentation needs an RGB image")
	r1, r2 = max(r0 - 3, 1), min(r0 + 3, ny)
	c1, c2 = max(c0 - 3, 1), min(c0 + 3, nx)
	X = Float32.(reshape(P[r1:r2, c1:c2, :], (r2 - r1 + 1) * (c2 - c1 + 1), 3))   # covmatrix's rows
	K = size(X, 1)
	m = vec(sum(X, dims = 1) ./ K)
	Xc = X .- m'
	C = (Xc' * Xc) ./ (K - 1)                     # unbiased estimate, exactly covmatrix.m
	# mahalanobis.m writes `Yc / Cx`. A patch of FLAT colour — a solid blob, the very thing one is
	# most likely to click — has no colour spread at all, so Cx is singular: MATLAB warns and hands
	# back Inf, which makes `D <= T` false everywhere = an empty mask with no explanation. Say why
	# instead. NOT the pseudo-inverse: on a rank-deficient Cx that collapses every distance to zero
	# and would select the WHOLE image, which is worse than saying nothing.
	Ci = try
		inv(C)
	catch
		error("Mahalanobis: the colours around that point are too uniform to estimate a covariance " *
		      "(a flat patch). Untick Mahalanobis and use the tolerance instead.")
	end
	all(isfinite, Ci) ||
		error("Mahalanobis: the colours around that point are too uniform to estimate a covariance " *
		      "(a flat patch). Untick Mahalanobis and use the tolerance instead.")
	F = Float32.(reshape(P, ny * nx, 3)) .- m'    # mahalanobis: subtract the mean from every vector
	d = vec(sum((F * Ci) .* F, dims = 2))         # real(sum(Yc / Cx .* conj(Yc), 2))
	return BitMatrix(reshape(d .<= T, ny, nx))
end

# ---------------------------------------------------------------------------------------------
# What Mirone does with the mask: radio_colorSegment (1) / radio_mask (2) / radio_digitize (0)
# ---------------------------------------------------------------------------------------------

# colorSegment == 1: keep the masked pixels, everything else goes to the background colour.
function _ff_segment_image(P::Array{UInt8,3}, mask::BitMatrix, bg::UInt8, I::GMTimage)::GMTimage
	out = fill(bg, size(P))
	@inbounds for k = 1:size(P, 3), c = 1:size(P, 2), r = 1:size(P, 1)
		mask[r, c] && (out[r, c, k] = P[r, c, k])
	end
	return _ff_image(out, I)
end

# colorSegment == 2: the mask itself as an image (0 / 255 so it is visible as a plain grey image).
function _ff_mask_image(mask::BitMatrix, I::GMTimage)::GMTimage
	out = Array{UInt8,3}(undef, size(mask, 1), size(mask, 2), 1)
	@inbounds for c = 1:size(mask, 2), r = 1:size(mask, 1)
		out[r, c, 1] = mask[r, c] ? 0xff : 0x00
	end
	return _ff_image(out, I)
end

# colorSegment == 0 (digitize): the mask's outer boundaries as polygons. floodfill.m runs
# bwboundaries + cvlib_mex('dp', …, 0.5) (Douglas-Peucker); GDAL already does both, better —
# GMT.polygonize is GDALPolygonize and GMT.simplify is GEOS SimplifyPreserveTopology. The mask
# carries the source georef, so the polygons come out in WORLD coordinates with no conversion of
# ours (Mirone has to multiply by head(8)/head(9) by hand). `minpts` is Mirone's edit_minPts.
function _ff_digitize(mask::BitMatrix, I::GMTimage, minpts::Int)
	M = _ff_mask_image(mask, I)
	D = GMT.polygonize(M)
	(D === nothing || isempty(D)) && return nothing
	segs = D isa GMTdataset ? [D] : collect(D)
	# THE size filter. `minpts` is edit_minPts, "shapes with less than this number of vertex won't be
	# ploted" — in Mirone that count is of BOUNDARY PIXELS, because bwboundaries returns every pixel
	# of the outline. GDALPolygonize hands back an already-rectilinear outline (a rectangle is 5
	# points however big it is), so counting ITS vertices would throw away everything. The equivalent
	# measure is the outline's LENGTH in pixels, which is what a boundary-pixel count really was.
	# NOTE this used to pass `minpts=` to polygonize, which is not one of its keywords (`min` /
	# `min_area` are) — so it was silently dropped and every single-pixel speck came back as a
	# 5-point polygon. That is the "bunch of shit".
	px = max(abs(I.inc[1]), abs(I.inc[2]))
	keep = GMTdataset[]
	for d in segs
		m = d.data
		size(m, 1) < 2 && continue
		per = 0.0
		for k = 2:size(m, 1)
			per += hypot(m[k, 1] - m[k-1, 1], m[k, 2] - m[k-1, 2])
		end
		(per / px) >= minpts && push!(keep, d)
	end
	isempty(keep) && return nothing
	# cvlib_mex('dp', boundary, 0.5) = Douglas-Peucker at half a pixel, here in world units. Applied
	# ONE SEGMENT AT A TIME: handed the whole vector, GMT.simplify comes back with a single dataset
	# and the other shapes are simply gone (two blobs in, one out).
	tol = 0.5 * abs(I.inc[1])
	tol <= 0 && return keep
	out = GMTdataset[]
	for d in keep
		s = try GMT.simplify(d, tol) catch; d end
		push!(out, s isa GMTdataset ? s : first(s))
	end
	return out
end

# push_pickMultiple_CB, RGB branch (the only one still reachable from the GUI — the Lab/YCrCb code
# is commented out in floodfill.m). Each seed's flood mask is used ONLY to measure that colour: the
# per-band MEAN over the mask becomes a class, and the whole image is then re-thresholded to
# mean +/- tol. So this is not a union of the flood masks — a shape of the same colour elsewhere in
# the image joins in even though no one clicked it. Returns one mask per seed, in seed order.
function _ff_multi_masks(P::Array{UInt8,3}, seeds::Vector{Tuple{Int,Int}}, tol::Int, conn::Int,
                         dodil::Bool)::Vector{BitMatrix}
	ny, nx, nb = size(P)
	out = BitMatrix[]
	for (r, c) in seeds
		m0 = _ff_flood_mask(P, r, c, tol, conn)
		any(m0) || continue
		lo = zeros(Int, nb);  hi = fill(255, nb)
		for k = 1:nb
			s, n = 0f0, 0
			@inbounds for cc = 1:nx, rr = 1:ny
				m0[rr, cc] && (s += P[rr, cc, k]; n += 1)
			end
			mu = s / max(n, 1)                    # mean2(a(mask)), floodfill.m line 627
			lo[k] = Int(max(round(mu) - tol, 0));  hi[k] = Int(min(round(mu) + tol, 255))
		end
		m = falses(ny, nx)
		@inbounds for cc = 1:nx, rr = 1:ny
			ok = true
			for k = 1:nb
				v = Int(P[rr, cc, k])
				(v < lo[k] || v > hi[k]) && (ok = false; break)
			end
			ok && (m[rr, cc] = true)
		end
		m = _ff_clean(m)                          # 'clean' FIRST here, then dilate (line 635-637)
		dodil && (m = _ff_dilate(m))
		push!(out, m)
	end
	return out
end

# Draw digitized shapes through the existing line-overlay export, driven exactly as `add!` and
# Aquamoto's Run-in drive it — no new drawing mechanism for this tool. Packed here rather than
# through `_pack_dataset`, which samples a GRID for the missing z: this window may hold nothing but
# the image, and the shapes are outlines OF that image, so they lie in its plane, z = 0.
function _ff_draw_shapes(scene::Ptr{Cvoid}, D, name::String, group::String)
	xyz, segoff = Float64[], Cint[0]
	for seg in (D isa GMTdataset ? (D,) : collect(D))
		m = seg isa GMTdataset ? seg.data : seg
		for k = 1:size(m, 1)
			push!(xyz, Float64(m[k, 1]), Float64(m[k, 2]), 0.0)
		end
		push!(segoff, Cint(length(xyz) ÷ 3))
	end
	nseg, npts = length(segoff) - 1, length(xyz) ÷ 3
	cr, cg, cb = _ovl_color(nothing, :lines)
	# `gmtvtk_add_overlay_ex_h`, not the plain one, for its `groupName`: every colour's shapes fold
	# under ONE collapsible master row in Scene Objects, so the whole detection shows, hides or dies
	# in a single click — the same grouping Geography > Plate boundaries and the slip-model patches
	# use, never a private one for this tool. Its `linewidth` is in POINTS (0 = the preference).
	ok = ccall(_fn(:gmtvtk_add_overlay_ex_h), Cint,
	           (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble,
	            Cdouble, Cdouble, Cdouble, Cstring, Cstring, Cstring),
	           scene, xyz, Cint(npts), segoff, Cint(nseg), Cint(1), cr, cg, cb, 0.0, 0.0,
	           String(name), String(group), C_NULL)
	ok == 0 && error("the viewer rejected the shapes (window closed?)")
	ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
	return
end

# ---------------------------------------------------------------------------------------------
# The C callback. params = "op;args":
#   "<op>;<name>;<tol>;<conn>;<dilate>;<mahal>;<mode>;<minpts>;<bg>;x1,y1[;x2,y2…]"
# op = "seed" (push_pickSingle, one point) or "multi" (push_pickMultiple, as many as were clicked);
# mode = 0 digitize / 1 colour segmentation / 2 mask, floodfill.m's three radios.
# ---------------------------------------------------------------------------------------------
function _ff_world_to_pixel(I::GMTimage, x::Float64, y::Float64)
	ny = length(I.y) > 1 ? length(I.y) - Int(I.registration == 0) : size(I.image, 1)
	nx = length(I.x) > 1 ? length(I.x) - Int(I.registration == 0) : size(I.image, 2)
	x0, x1, y0, y1 = I.range[1], I.range[2], I.range[3], I.range[4]
	c = Int(round((x - x0) / max(x1 - x0, eps()) * (nx - 1))) + 1
	# Row 1 is the TOP of the image (every GMT image layout starts with 'T'), so y runs downward.
	r = Int(round((y1 - y) / max(y1 - y0, eps()) * (ny - 1))) + 1
	return clamp(r, 1, ny), clamp(c, 1, nx)
end

function _on_floodfill(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		p = split(unsafe_string(cparams), ';')
		op = p[1]
		(op == "seed" || op == "multi") || error("Shape detector: unknown op '$op'")
		name = String(strip(p[2]))
		I = isempty(name) ? _find_object_named(scene, :image)[2] : _find_object(scene, :image, name)
		(I isa GMTimage) || error("Shape detector: this window has no image named '$name'")
		tol    = parse(Int,     strip(p[3]));  conn  = parse(Int,     strip(p[4]))
		dodil  = strip(p[5]) == "1";           domah = strip(p[6]) == "1"
		mode   = parse(Int,     strip(p[7]));  minpts = parse(Int,    strip(p[8]))
		bg     = UInt8(clamp(parse(Int, strip(p[9])), 0, 255))
		# Every remaining field is one seed, "x,y" in WORLD coordinates (the viewer knows nothing of
		# pixel indices). "seed" sends exactly one; "multi" sends as many as were clicked.
		seeds = Tuple{Int,Int}[]
		P = _ff_pixels(I)
		for f in p[10:end]
			isempty(strip(f)) && continue
			xy = split(f, ',')
			length(xy) == 2 || continue
			push!(seeds, _ff_world_to_pixel(I, parse(Float64, xy[1]), parse(Float64, xy[2])))
		end
		isempty(seeds) && error("Shape detector: no seed point")
		src = isempty(name) ? "Image" : name

		if op == "multi"
			masks = _ff_multi_masks(P, seeds, tol, conn, dodil)
			isempty(masks) && (_viewer_log_error(scene, "Shape detector: nothing selected"); return Cint(0))
			if mode == 0                                 # digitize: one set of polygons per colour
				grp, drew = "$src (shapes)", 0
				for (i, m) in enumerate(masks)
					D = _ff_digitize(m, I, minpts)
					D === nothing && continue
					_ff_draw_shapes(scene, D, "Colour $i", grp)      # …all under ONE master row
					drew += 1
				end
				drew == 0 && (_viewer_log_error(scene,
					"Shape detector: no shape's outline reached $minpts pixels — lower Min"); return Cint(0))
			elseif mode == 2                             # mask: the OR of every colour (line 606-610)
				acc = masks[1]
				for k = 2:length(masks)  acc = acc .| masks[k]  end
				_commit_derived_image!(scene, _ff_mask_image(_ff_clean(acc), I), "$src (segmentation mask)")
			else                                         # ONE image, each colour painted back into it
				out = fill(bg, size(P))
				for m in masks, k = 1:size(P, 3), c = 1:size(P, 2), r = 1:size(P, 1)
					m[r, c] && (out[r, c, k] = P[r, c, k])
				end
				_commit_derived_image!(scene, _ff_image(out, I), "$src (color segmentation)")
			end
			return Cint(1)
		end

		r, c = seeds[1]
		mask = domah ? _ff_mahal_mask(P, r, c, Float32(tol)) : _ff_flood_mask(P, r, c, tol, conn)
		dodil && (mask = _ff_dilate(mask))
		any(mask) || (_viewer_log_error(scene, "Shape detector: nothing selected at that point"); return Cint(0))
		if mode == 0                                     # radio_digitize
			D = _ff_digitize(_ff_clean(mask), I, minpts)
			D === nothing && (_viewer_log_error(scene,
				"Shape detector: no shape's outline reached $minpts pixels — lower Min"); return Cint(0))
			# Grouped even when there is one colour: several disjoint bodies can share it, and they
			# must still show/hide/die together under one master row.
			_ff_draw_shapes(scene, D, "Colour 1", "$src (shapes)")
		elseif mode == 2                                 # radio_mask
			_commit_derived_image!(scene, _ff_mask_image(_ff_clean(mask), I), "$src (segmentation mask)")
		else                                             # radio_colorSegment
			_commit_derived_image!(scene, _ff_segment_image(P, _ff_clean(mask), bg, I),
			                       "$src (color segmentation)")
		end
		return Cint(1)
	catch e
		_tool_failed(scene, "Shape detector ($op)", e)
		return Cint(0)
	end
end

function _register_floodfill()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_floodfill, s, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_floodfill_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
