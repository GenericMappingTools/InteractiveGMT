# Helpers for test-ve-rules-gui.jl. Kept in one file so every item measures the SAME way -- a
# regression suite whose items each compute the expected number differently is a suite that can
# agree with a bug.
#
# The rule these encode, once:  drawn height of z  =  z * zfac * ve
#   zfac = kVEReference * H_displayed / (that layer's own z range)      [kVEReference = 0.1]
#   H_displayed = max(|x1-x0| * xfac, |y1-y0|)                          [xfac = cos(midlat), or 1]
# Both are PLOT dimensions. Nothing here reconciles z's unit with x,y's -- that is the point.

const VE_REFERENCE = 0.1

ve_pump(n::Int=25) = for _ in 1:n; InteractiveGMT._pump_once(); sleep(0.02); end
ve_close(h) = (ccall(InteractiveGMT._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h); ve_pump(5))

ve_state(h)      = InteractiveGMT._scene_state(h)
ve_state_full(h) = InteractiveGMT._parse_scene_state(InteractiveGMT._scene_state_full_raw(h))

# A synthetic geographic grid over a FIXED footprint with a chosen z span, so a test can vary the one
# quantity it is about. Smooth, so the relief shades like terrain rather than like noise.
function ve_grid(; zspan::Float64, n::Int = 48, x = (-15.0, -10.0), y = (35.0, 40.0))
	xs = collect(range(x[1], x[2], length = n))
	ys = collect(range(y[1], y[2], length = n))
	z = [Float32(zspan * (0.5 + 0.5 * sin(3 * (xi - x[1]) / (x[2] - x[1])) *
	                            cos(3 * (yi - y[1]) / (y[2] - y[1]))))
	     for yi in ys, xi in xs]
	InteractiveGMT.GMT.mat2grid(z, x = xs, y = ys)
end

# The DISPLAYED horizontal size of the map this grid makes, in world units: the same quantity
# sceneZRefFor uses (lon span times the drawn x aspect, or the lat span, whichever is larger).
function ve_H_displayed(G, geographic::Bool = true)
	x0, x1, y0, y1 = Float64.(G.range[1:4])
	xfac = geographic ? max(1e-6, cos(0.5 * (y0 + y1) * pi / 180)) : 1.0
	max(abs(x1 - x0) * xfac, abs(y1 - y0))
end

ve_zspan(G) = Float64(G.range[6] - G.range[5])
# The axis mapping the rule requires for THIS layer, from displayed dimensions alone.
ve_zfac_expected(G, geographic::Bool = true) = VE_REFERENCE * ve_H_displayed(G, geographic) / ve_zspan(G)
# ...and the height that layer must actually be DRAWN at, at a given ve.
ve_drawn_span_expected(G, ve::Real, geographic::Bool = true) =
	ve_zspan(G) * ve_zfac_expected(G, geographic) * float(ve)

# The ACTIVE raster's axes box height in WORLD units -- the drawn geometry, after every scale the
# viewer applies. NaN when the window reports no box (no active raster).
function ve_active_drawn_span(h)
	st = ve_state(h)
	(haskey(st, "axZ0") && haskey(st, "axZ1")) || return NaN
	parse(Float64, string(st["axZ1"])) - parse(Float64, string(st["axZ0"]))
end

# What the VE handle's label is showing right now (0 = no handle on this window).
ve_gizmo_shows(h) = parse(Float64, string(get(ve_state(h), "gizve", "0")))

# Every ve in the window, keyed as the session records them: "ve" = the base relief's own,
# "ve_<tag>" = each dropped layer's own.
function ve_all_ves(h)
	st = ve_state_full(h)
	Dict(k => parse(Float64, string(v)) for (k, v) in st if k == "ve" || startswith(k, "ve_"))
end

# Add a grid as a NEW LAYER exactly as a drop/derive does: gmtvtk_add_surface_h then the ONE adopt
# transition (_adopt_derived!), so these tests exercise the real path and not a test-only shortcut.
function ve_add_layer(h, G, name::AbstractString)
	z = Float32.(G.z'[:]);  ny, nx = size(G.z)
	ok = ccall(InteractiveGMT._fn(:gmtvtk_add_surface_h), Cint,
	     (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
	      Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring, Cint),
	     h, z, Cint(nx), Cint(ny), G.range[1], G.range[2], G.range[3], G.range[4], Cint(1),
	     C_NULL, C_NULL, Cint(0), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(name), Cint(1))
	ve_pump()
	InteractiveGMT._adopt_derived!(h, String(name), G)
	ve_pump()
	ok
end

# Set the ACTIVE layer's ve through gmtvtk_set_view_azel_h -- the same activeVEPtr door the gizmo
# drag and the VE dialog use, so a test cannot pass by writing a field no user can reach.
ve_set_active(h, ve::Real) = ccall(InteractiveGMT._fn(:gmtvtk_set_view_azel_h), Cint,
	(Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble, Cdouble, Cdouble),
	h, 135.0, 35.0, -1.0, float(ve), Cint(0), 0.0, 0.0, 0.0)

# Show/hide a whole layer by name; "" is the base relief (the convention gmtvtk_set_object_visible
# uses). Hiding the layer above it is how a test makes another layer the active one.
function ve_show(h, name::AbstractString, on::Bool)
	r = ccall(InteractiveGMT._fn(:gmtvtk_set_object_visible), Cint,
	          (Ptr{Cvoid}, Cstring, Cint), h, String(name), Cint(on ? 1 : 0))
	ve_pump(); r
end

# ---- pixel measures (illumination) -------------------------------------------------------------
# Read a rendered PNG back through GMT (no extra dependency) as a luminance matrix.
function ve_luma(path::AbstractString)
	I = InteractiveGMT.GMT.gmtread(path)
	A = Array(I.image)
	nd = ndims(A)
	if nd == 3 && size(A, 3) >= 3
		return 0.299 .* Float64.(A[:, :, 1]) .+ 0.587 .* Float64.(A[:, :, 2]) .+ 0.114 .* Float64.(A[:, :, 3])
	end
	Float64.(nd == 3 ? A[:, :, 1] : A)
end

# How much the luminance VARIES across the frame. Lit relief spreads it; a flat plate under one sun
# is a single tone. Measured on the middle half of the frame so the background and the colour bar
# cannot carry the number on their own.
function ve_luminance_spread(path::AbstractString)
	L = ve_luma(path)
	ny, nx = size(L)
	sub = L[max(1, ny ÷ 4):min(ny, 3 * ny ÷ 4), max(1, nx ÷ 4):min(nx, 3 * nx ÷ 4)]
	isempty(sub) ? 0.0 : Float64(sqrt(sum(abs2, sub .- (sum(sub) / length(sub))) / length(sub)))
end

# Two frames identical to within encoder noise: the geometry and the lighting did not change.
function ve_frames_match(p1::AbstractString, p2::AbstractString; tol = 0.75)
	A = ve_luma(p1);  B = ve_luma(p2)
	size(A) == size(B) || return false
	sum(abs.(A .- B)) / length(A) <= tol
end
