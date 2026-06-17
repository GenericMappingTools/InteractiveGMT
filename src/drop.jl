# File drag-and-drop. The C side calls `_on_drop` with the receiving window's Scene* handle and
# each dropped file's path; we read it (GMT.gmtread, which auto-detects a large range of formats)
# and add it INTO that window — a new surface/image actor or a line/point overlay, listed in the
# window's "Scene Objects" panel.
#
# The @cfunction pointer + its registration are RUNTIME values, created in __init__ (NOT at top
# level — a precompiled @cfunction is invalid), exactly like the in-window console callback.

# Called on the UI thread from inside the Qt pump when a file is dropped on a viewer window.
# An EMPTY launcher window (no primary surface) is PROMOTED: the file opens a full viewer window
# (real axes + coordinate readout + Scene Objects panel) and the bare launcher is retired. A
# populated window gets the file ADDED into it (extra surface/image/overlay).
function _on_drop(scene::Ptr{Cvoid}, cpath::Cstring)::Cvoid
	path = unsafe_string(cpath)
	try
		data = GMT.gmtread(path)
		if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) != 0
			_drop_into(scene, data, basename(path))            # add into the populated window
		else
			iview(data)                                        # promote: full window with axes/readout
			ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), scene)   # retire the empty launcher
		end
	catch e
		@warn "drop: could not read/open file" path exception=e
	end
	return
end

# Dispatch the dropped object by type into the window `scene`.
_drop_into(scene::Ptr{Cvoid}, G::GMTgrid,  name) = _add_grid_to_scene(scene, G, name)
_drop_into(scene::Ptr{Cvoid}, I::GMTimage, name) = _add_image_to_scene(scene, I, name)
_drop_into(scene::Ptr{Cvoid}, D::GMTdataset, name) = _add_dataset_to_scene(scene, D, name)
_drop_into(scene::Ptr{Cvoid}, D::Vector{<:GMTdataset}, name) = _add_dataset_to_scene(scene, D, name)
_drop_into(scene::Ptr{Cvoid}, x, name) = @warn "drop: unsupported data type" type=typeof(x)

# Add a dropped grid as a CPT-coloured surface actor in the window.
function _add_grid_to_scene(scene::Ptr{Cvoid}, G::GMTgrid, name; cmap=:geo)
	z = eltype(G.z) === Float32 ? G.z : Float32.(G.z); ny, nx = size(z); r = G.range
	cz, crgb, ncolor = _cpt_nodes(G, cmap)
	ok = ccall(_fn(:gmtvtk_add_surface_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		  scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4],
		  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(name))
	ok == 0 && @warn "drop: window is closed; grid not added"
	return ok != 0
end

# Add a dropped image as a flat textured plane in the window.
function _add_image_to_scene(scene::Ptr{Cvoid}, I::GMTimage, name)
	ir = I.range
	z = zeros(Float32, 2, 2)
	fillu = (UInt8(200), UInt8(200), UInt8(200))
	img, iw, ih, ibands = _drape_to_bbox(I, ir[1], ir[2], ir[3], ir[4]; outside=:transparent, fill=fillu)
	ok = ccall(_fn(:gmtvtk_add_surface_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		  scene, z, Cint(2), Cint(2), ir[1], ir[2], ir[3], ir[4],
		  C_NULL, C_NULL, Cint(0), img, Cint(iw), Cint(ih), Cint(ibands), Cint(1), String(name))
	ok == 0 && @warn "drop: window is closed; image not added"
	return ok != 0
end

# Add a dropped dataset as a line/point overlay in the window. z comes from column 3 if present,
# else 0 (there is no host grid to drape on for an arbitrary dropped-on window).
function _add_dataset_to_scene(scene::Ptr{Cvoid}, D, name)
	mode = _ds_kind(D) === :points ? :points : :lines
	xyz, segoff, nseg, npts = _pack_dataset_flat(D)
	modei = mode === :lines ? Cint(1) : Cint(0)
	cr, cg, cb = _ovl_color(nothing, mode)
	lw = mode === :lines ? 2.0 : 0.0
	ps = mode === :points ? 6.0 : 0.0
	ok = ccall(_fn(:gmtvtk_add_overlay_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
		  scene, xyz, Cint(npts), segoff, Cint(nseg), modei, cr, cg, cb, lw, ps)
	ok == 0 && @warn "drop: window is closed; dataset not added"
	return ok != 0
end

# Pack a GMTdataset (single or multi-segment) into the C overlay layout, taking z from column 3
# when present else 0 (no host grid to sample). Mirrors grid.jl `_pack_dataset` but grid-free.
function _pack_dataset_flat(D)
	segs = (D isa GMTdataset || D isa AbstractMatrix) ? (D,) : collect(D)
	xyz = Float64[]; segoff = Cint[0]; off = 0
	for seg in segs
		m = seg isa GMTdataset ? seg.data : seg
		n, ncol = size(m, 1), size(m, 2)
		for k in 1:n
			x = Float64(m[k, 1]); y = Float64(m[k, 2])
			z = ncol >= 3 ? Float64(m[k, 3]) : 0.0
			push!(xyz, x, y, z)
		end
		off += n; push!(segoff, Cint(off))
	end
	return xyz, segoff, length(segs), off
end

# Build the C-callable pointer and install it in the DLL. Called once from __init__.
function _register_drop_callback()
	fptr = @cfunction(_on_drop, Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_drop_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

"""
	iview() -> QtEmpty

Open an empty viewer window that acts as a drag-and-drop launcher: drop a grid, image, or table
file (anything `GMT.gmtread` understands) onto it — or onto any open viewer window — and it is
added to that window and listed in its "Scene Objects" panel. Returns a `QtEmpty` handle.
"""
function iview()
	h = ccall(_fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "GMT 3-D Viewer  —  drop a file")
	h == C_NULL && error("iview: could not open the empty window")
	fig = _register_fig!(QtEmpty(h))
	_start_pump()
	return fig
end
