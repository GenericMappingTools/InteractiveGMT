# bgregion.jl — File > Background region (port of Mirone's empty-figure-with-limits). The C++ dialog
# (compass W/E/S/N + "Is Geographic?" checkbox, in 70_window.cpp) calls `_on_bgregion` with the
# limits string; we frame a blank WHITE 2-D map over [W,E]x[S,N] — axes only, nothing plotted yet —
# ready to drop coastlines / overlays onto.
#
# The window the menu was clicked in is the FIRST CHOICE: if it is an EMPTY launcher (no data yet),
# the new axes are applied IN PLACE — no spurious new window. Only when that window already holds
# data do we open a fresh one (can't repurpose a populated window as a blank canvas).
#
# Implementation reuses the proven empty-launcher promote path (the same one basemap.jl uses):
# `gmtvtk_promote_surface_h` lays down a flat z=0 plane over the region with real framed axes + a
# top-down flat-2-D view. We colour that plane WHITE via a 2-node white CPT (crgb is 0..1 RGB,
# row-major — see cpt.jl) so the map area reads as a white "paper" canvas.
#
# As with the console / drop / basemap callbacks, the @cfunction + its registration are RUNTIME
# values created in __init__ (a precompiled @cfunction is invalid), never at module top level.

# C callback: copt = "W/E/S/N/geographic" (geographic = "1"/"0"). Frame a blank white 2-D map over
# those limits — in the calling window if it is empty, else in a fresh one; tag WGS84 when geographic.
function _on_bgregion(scene::Ptr{Cvoid}, copt::Cstring)::Cvoid
	try
		p = split(unsafe_string(copt), '/')
		W, E, S, N = parse.(Float64, p[1:4])
		geog = length(p) >= 5 && strip(p[5]) == "1"
		E > W || error("Background region: need E > W (got W=$W E=$E)")
		N > S || error("Background region: need N > S (got S=$S N=$N)")

		# Reuse the calling window when it's an empty launcher (no surface yet); otherwise open a new
		# one. promote_surface_h only repurposes an emptyStart scene — on a populated one it would add
		# the white plane as an extra object, so gate explicitly on has_surface.
		reuse = scene != C_NULL &&
		        ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		h = reuse ? scene :
		    ccall(_fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "i'GMT  —  background region")
		h == C_NULL && error("Background region: could not open a window")

		# Flat z=0 plane over the region, coloured white by a 2-node CPT (white at both ends).
		zblank = zeros(Float32, 2, 2)
		cz   = Float64[0.0, 1.0]
		crgb = Float64[1.0, 1.0, 1.0, 1.0, 1.0, 1.0]   # white, white (row-major RGB, 0..1)
		ccall(_fn(:gmtvtk_promote_surface_h), Cint,
		      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		      h, zblank, Cint(2), Cint(2), W, E, S, N, Cint(geog ? 1 : 0),
		      cz, crgb, Cint(2), C_NULL, Cint(0), Cint(0), Cint(0), Cint(1), "Background region")

		if geog
			ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
			      h, "+proj=longlat +datum=WGS84 +no_defs", "", Cint(4326))   # reveals Geography menu
		end
		reuse || _register_fig!(QtEmpty(h))   # a reused window is already in the registry
		_start_pump()
	catch e
		_viewer_log_error(scene, "Background region FAILED: $(sprint(showerror, e))")
		@warn "bgregion: could not open the region" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__.
function _register_bgregion()
	fptr = @cfunction(_on_bgregion, Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_bgregion_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
