# File > Open xy(z) — Mirone-compatible specialised table imports.
#
# Reading is centralized on `_import_xy_read`: ONE GMT.gmtselect call that reads the table AND clips
# it to the region ON SCREEN. 
#
# The region always exists: the C++ side (70_window.cpp's Open xy(z) actions) calls sceneEnsureBase
# first, which loads the whole-world Base Map into an empty launcher, then hands over
# sceneVisibleRegion's W/E/S/N in the request envelope. So an import is always clipped to the raster
# on display, never to the file's own extent.
#
# Rendering delegates to existing overlay, symbol and text builders; these vector imports never
# create or reframe axes (SACRED_LAW.md), and every element they add carries a name so it gets its
# own Scene Objects handle (batched text hangs under one master group named for the file).

_xy_segments(D::GMTdataset)::Vector{GMTdataset} = GMTdataset[D]
_xy_segments(D::Vector{GMTdataset})::Vector{GMTdataset} = D
_xy_matrix(D::GMTdataset)::Matrix{Float64} = Float64.(D.data)
_xy_matrix(D::Vector{GMTdataset})::Matrix{Float64} = reduce(vcat, (Float64.(s.data) for s in D))

function _import_xy_points(scene::Ptr{Cvoid}, D::GMTdataset, name::String)::Nothing
	_add_dataset_to_scene(scene, D, name; forceMode=:points)
	return nothing
end
function _import_xy_points(scene::Ptr{Cvoid}, D::Vector{GMTdataset}, name::String)::Nothing
	_add_dataset_to_scene(scene, D, name; forceMode=:points)
	return nothing
end

# Build auto-scaled arrow geometry, then send it through the same overlay builder used by every
# imported/drawn line. Each arrow is a shaft and two head strokes; no parallel actor type exists.
function _import_xy_arrows(scene::Ptr{Cvoid}, D::GMTdataset, name::String)::Nothing
	return _import_xy_arrows(scene, GMTdataset[D], name)
end
function _import_xy_arrows(scene::Ptr{Cvoid}, D::Vector{GMTdataset}, name::String)::Nothing
	m = _xy_matrix(D)
	size(m, 2) >= 4 || error("Import Arrow field requires four columns: x y u v")
	good = [all(isfinite, Float64.(m[k, 1:4])) for k in axes(m, 1)]
	any(good) || error("Import Arrow field contains no finite x y u v rows")
	x = @view m[good, 1]; y = @view m[good, 2]
	u = @view m[good, 3]; v = @view m[good, 4]
	mag = hypot.(u, v); mm = maximum(mag)
	mm > 0 || error("Import Arrow field contains only zero-length vectors")
	span = max(maximum(x)-minimum(x), maximum(y)-minimum(y))
	span > 0 || (span = 1.0)
	scale = 0.08 * span / mm
	segs = Matrix{Float64}[]
	for k in eachindex(x)
		dx = u[k] * scale; dy = v[k] * scale
		tx = x[k] + dx; ty = y[k] + dy
		push!(segs, [x[k] y[k]; tx ty])
		len = hypot(dx, dy); len == 0 && continue
		ux = dx / len; uy = dy / len; h = 0.28len; w = 0.13len
		push!(segs, [tx ty; tx-h*ux+w*uy ty-h*uy-w*ux])
		push!(segs, [tx ty; tx-h*ux-w*uy ty-h*uy+w*ux])
	end
	_add_dataset_to_scene(scene, segs, name; forceMode=:lines, noConvertToPoints=true)
	return nothing
end

function _import_xy_scaled(scene::Ptr{Cvoid}, D::GMTdataset, name::String)::Nothing
	return _import_xy_scaled(scene, GMTdataset[D], name)
end
function _import_xy_scaled(scene::Ptr{Cvoid}, D::Vector{GMTdataset}, name::String)::Nothing
	m = _xy_matrix(D)
	size(m, 2) >= 3 || error("Import scaled symbols requires at least three columns: x y z")
	good = [all(isfinite, Float64.(m[k, 1:3])) for k in axes(m, 1)]
	any(good) || error("Import scaled symbols contains no finite x y z rows")
	m = m[good, :]; z = Float64.(m[:, 3]); z0, z1 = extrema(z)
	colors = if size(m, 2) >= 7
		c = Float64.(m[:, 5:7])
		maximum(c) > 1 && (c ./= 255)
		clamp.(c, 0, 1)
	else
		t = z1 == z0 ? fill(0.5, length(z)) : (z .- z0) ./ (z1-z0)
		hcat(clamp.(1.5 .- abs.(4t .- 3), 0, 1),
		     clamp.(1.5 .- abs.(4t .- 2), 0, 1),
		     clamp.(1.5 .- abs.(4t .- 1), 0, 1))
	end
	sizes = size(m, 2) >= 4 ? max.(1.0, Float64.(m[:, 4])) : fill(7.0, size(m, 1))
	# ONE layer for the whole table: per-point size and colour travel INSIDE it (add_symbols! passes
	# them to gmtvtk_add_symbols_ex_h). One call, one actor, ONE Scene Objects handle named for the
	# file — a call per row gave a row per point, which is the same flooding the text import had.
	add_symbols!(scene, Float64.(m[:, 1]), Float64.(m[:, 2]); z=Float64.(m[:, 3]),
	             symbol=:circle, size=sizes, sizeunit=:pt, fill=colors, edge=:black, name=name)
	return nothing
end

function _import_xy_text(scene::Ptr{Cvoid}, D::GMTdataset, name::String)::Nothing
	return _import_xy_text(scene, GMTdataset[D], name)
end
function _import_xy_text(scene::Ptr{Cvoid}, D::Vector{GMTdataset}, name::String)::Nothing
	xy = Float64[]; labels = String[]
	for s in _xy_segments(D)
		m = s isa GMTdataset ? s.data : s
		t = s isa GMTdataset ? (try s.text catch; nothing end) : nothing
		for k in axes(m, 1)
			size(m, 2) >= 2 || continue
			x, y = Float64(m[k,1]), Float64(m[k,2])
			isfinite(x) && isfinite(y) || continue
			label = t !== nothing && k <= length(t) ? strip(String(t[k])) : ""
			isempty(label) && continue
			push!(xy, x, y); push!(labels, label)
		end
	end
	isempty(labels) && error("Import text requires rows of x y text")
	blob = join(labels, '\x1e'); n = length(labels)
	ccall(_fn(:gmtvtk_add_texts_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint, Cdouble, Cdouble, Cdouble, Cint,
	       Cstring, Cint, Cint, Cstring, Ptr{Cint}),
	      scene, xy, blob, Cint(n), 0.0, 0.0, 0.0, 10, "Arial", Cint(0), Cint(0), name, C_NULL)
	return nothing
end

# Read `path` clipped to the displayed region. `f=:g` only when that region is geographic (the same
# crude lon/lat range test used elsewhere), so a cartesian table is not pushed through a geographic
# region test. Returns nothing when the file has no row inside the view.
function _import_xy_read(path::String, region::NTuple{4,Float64})
	W, E, S, N = region
	geog = (W >= -360.0 && E <= 360.0 && S >= -90.0 && N <= 90.0)
	D = geog ? GMT.gmtselect(path, R=(W, E, S, N), f=:g) : GMT.gmtselect(path, R=(W, E, S, N))
	D === nothing && return nothing
	segs = D isa AbstractVector ? D : [D]
	any(s -> s.data !== nothing && !isempty(s.data), segs) || return nothing
	return D
end

function _on_import_xy(scene::Ptr{Cvoid}, path::String, mode::Symbol, region::NTuple{4,Float64})::Cvoid
	try
		D = _import_xy_read(path, region)
		D === nothing &&
			(_viewer_log_error(scene, "Open xy(z): no data inside the displayed region " *
			                          "$(region[1])/$(region[2])/$(region[3])/$(region[4])"); return)
		name = splitext(basename(path))[1]
		mode === :points ? _import_xy_points(scene, D, name) :
		mode === :arrows ? _import_xy_arrows(scene, D, name) :
		mode === :scaled ? _import_xy_scaled(scene, D, name) :
		mode === :text ? _import_xy_text(scene, D, name) :
		error("unknown Open xy(z) mode: $mode")
	catch e
		_viewer_log_error(scene, "Open xy(z) FAILED: $(sprint(showerror, e))")
	end
	return
end
