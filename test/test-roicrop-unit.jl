# CI-safe unit tests for "Roi Crop Tools" (grid.jl `_roi_crop_grid`/`_roi_crop_image`, the rectangle
# context-menu's Crop Grid / Crop Image / Crop Image (with coords) — port of Mirone's mirone.m
# ImageCrop_CB CropaGrid_pure / plain-image-crop). Pure math, no DLL — call the helpers directly.
#
# What these lock down: GMT.jl's `grdcut`, for a plain in-memory region-only cut with no clip
# polygon / file / GDAL request, dispatches to the SAME in-memory `crop()` our helpers call
# (grdcut.jl:81, `do_crop_here` branch) — for BOTH grids and images. So the result of "Crop Grid" /
# "Crop Image" must be BIT-IDENTICAL to `GMT.grdcut(G; region=...)` — not just "close" or
# "plausible". A future GMT.jl change that makes grdcut diverge from crop() for this in-memory case
# would break this test, which is the point.

@testitem "roi crop helpers present" tags=[:unit, :fast] begin
	for s in (:_roi_crop_grid, :_roi_crop_image, :_on_roi_crop)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "roi crop grid == grdcut, interior region" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.peaks()
	w, e, s, n = -1.0, 1.5, -2.0, 0.75          # asymmetric, off-centre, off-grid-line box

	Gc   = IG._roi_crop_grid(G, w, e, s, n)
	Gref = GMT.grdcut(G; region=(w, e, s, n))

	@test size(Gc.z) == size(Gref.z)
	@test Gc.range == Gref.range
	@test Gc.z == Gref.z                        # bit-identical, not just isapprox
end

@testitem "roi crop grid == grdcut, several regions incl. whole-grid and near-corner" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.peaks()                             # range [-3,3,-3,3]
	for (w, e, s, n) in ((-3.0, 3.0, -3.0, 3.0), # whole grid
	                     (-2.9, -1.0, -2.9, -1.0), # near a corner
	                     (-0.125, 0.125, -0.125, 0.125), # tiny box, sub-cell
	                     (0.0, 3.0, -3.0, 0.0))   # a quadrant
		Gc   = IG._roi_crop_grid(G, w, e, s, n)
		Gref = GMT.grdcut(G; region=(w, e, s, n))
		@test size(Gc.z) == size(Gref.z)
		@test Gc.range == Gref.range
		@test Gc.z == Gref.z
	end
end

@testitem "roi crop image == grdcut, interior region" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	I = GMT.mat2img(rand(UInt8, 40, 60, 3); x = [0.0, 12.0], y = [0.0, 8.0])
	w, e, s, n = 2.0, 9.0, 1.0, 6.0

	Ic   = IG._roi_crop_image(I, w, e, s, n)
	Iref = GMT.grdcut(I; region=(w, e, s, n))

	@test size(Ic.image) == size(Iref.image)
	@test Ic.range == Iref.range
	@test Ic.image == Iref.image                # bit-identical pixels
end

@testitem "_on_roi_crop: malformed rect / unknown kind never throw (best-effort, logs instead)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# scene = C_NULL: _viewer_log_error swallows the ccall (no DLL loaded in CI), so a bad
	# request must return normally instead of throwing all the way out to the caller.
	@test IG._on_roi_crop(C_NULL, "grid", "not/a/valid/rect") === nothing
	@test IG._on_roi_crop(C_NULL, "bogus_kind", "-1.0/1.0/-1.0/1.0") === nothing
end
