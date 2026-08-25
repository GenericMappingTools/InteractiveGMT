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
function _on_image_histo(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, px::Ptr{UInt8}, npix::Cint, nb::Cint)::Cint
	try
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
		_viewer_log_error(scene, "Image histogram FAILED: $(sprint(showerror, err))")
		return Cint(0)
	end
end

# The trampoline goes through `invokelatest`, like every other registration in _ensure_callbacks,
# and that is not a style detail: a DIRECT `@cfunction(_on_image_histo, …)` makes the compiler infer
# and emit the WHOLE handler body — GMT.histogray and all — at REGISTRATION time, which is the first
# window open. Measured: 134 ms here against ~14 ms for the invokelatest form, the single most
# expensive of the ~79 registrations. Behind invokelatest the body compiles on the first actual
# histogram instead, where the user is already waiting for a computation.
function _register_image_histo()
	cb = @cfunction((s, d, p, n, b) -> Base.invokelatest(_on_image_histo, s, d, p, n, b)::Cint,
	                Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{UInt8}, Cint, Cint))
	ccall(_fn(:gmtvtk_set_image_histo_callback), Cvoid, (Ptr{Cvoid},), cb)
	return nothing
end
