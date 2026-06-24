# solids.jl — the toolbar "3-D Bodies" flyout. The C++ flyout (70_window.cpp) hands a GMT solid
# NAME ("cube"/"sphere"/"torus"/"cylinder"/"tetrahedron"/… — the SOLIDS catalogue keys in fv.jl) to
# `_on_solid`, which builds the named GMTfv and shows it.
#
# Placement rule (per request): if the calling window is EMPTY (a launcher) or already holds a
# body-button solid, build the body INTO that window IN PLACE — REPLACING the existing body if any
# (gmtvtk_promote_fv_h). A window showing real data (grid/image/points/poly-mesh) is left untouched;
# the body then opens in its OWN new FV window (view_fv). The C function decides which case applies
# and returns 1 (reused) / 0 (declined -> open new window).
#
# As with the other menu callbacks, the @cfunction and its registration are RUNTIME values created
# lazily on first window open (eventloop.jl `_ensure_callbacks`) via a thin invokelatest trampoline —
# never at module top level (a precompiled @cfunction is invalid and would drag GMT into the image).

# Try to build the solid INTO an existing window (empty launcher / prior body) IN PLACE. Returns the
# C result: 1 = reused this window, 0 = declined (window has real data -> caller opens a new window).
function _promote_fv(scene::Ptr{Cvoid}, xyz::Vector{Float64}, sides::Vector{Cint}, indices::Vector{Cint},
					 facergb::Vector{UInt8}, facez::Vector{Float64}, cz::Vector{Float64},
					 crgb::Vector{Float64}, ncolor::Int, bb::Vector{Float64},
					 geographic::Bool, zscale::Float64, edges::Bool, objname::String)
	nv     = length(xyz) ÷ 3
	nfaces = length(sides)
	fc  = isempty(facergb) ? Ptr{Cuchar}(C_NULL)  : pointer(facergb)
	fzp = isempty(facez)   ? Ptr{Cdouble}(C_NULL) : pointer(facez)
	czp = isempty(cz)      ? Ptr{Cdouble}(C_NULL) : pointer(cz)
	cgp = isempty(crgb)    ? Ptr{Cdouble}(C_NULL) : pointer(crgb)
	r = GC.@preserve xyz sides indices facergb facez cz crgb ccall(_fn(:gmtvtk_promote_fv_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Ptr{Cint}, Ptr{Cuchar}, Ptr{Cdouble},
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint,
		 Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble,
		 Cint, Cdouble, Cint, Cstring),
		scene, xyz, Cint(nv), sides, Cint(nfaces), indices, fc, fzp,
		czp, cgp, Cint(ncolor),
		bb[1], bb[2], bb[3], bb[4], bb[5], bb[6],
		Cint(geographic), zscale, Cint(edges), objname)
	return Int(r)
end

# C callback: cname = a SOLIDS key. Build the solid, then try IN-PLACE into the calling window; if the
# window declined (it shows real data), open the body in its own FV window. The body is a faceted GMT
# solid coloured by mean-z (the view_fv default); it carries full Scene Objects properties either way.
function _on_solid(scene::Ptr{Cvoid}, cname::Cstring)::Cvoid
	name = ""
	try
		name = String(strip(unsafe_string(cname)))
		isempty(name) && return
		lname   = lowercase(name)
		builder = get(SOLIDS, lname, nothing)
		builder === nothing && error("unknown solid '$name'")
		fv      = builder()                              # the GMT generator's own defaults (demo curves where needed)
		objname = uppercasefirst(lname)                  # Scene Objects row label = the solid name
		xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale =
			_fv_resolve(fv; cmap=:turbo, color=true, geographic=nothing)
		r = _promote_fv(scene, xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale, false, objname)
		if r == 0                                        # window holds real data -> own new window
			_view_fv(fv, xyz, sides, indices, facergb, facez, cz, crgb, ncolor,
					 bb[1], bb[2], bb[3], bb[4], bb[5], bb[6], geog, zscale, false,
					 "GMT $lname  (Qt + VTK)", objname)
		end
	catch e
		_viewer_log_error(scene, "3-D body '$name' FAILED: $(sprint(showerror, e))")
		@warn "solids: could not build/show '$name'" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_solid()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_solid, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_solid_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
