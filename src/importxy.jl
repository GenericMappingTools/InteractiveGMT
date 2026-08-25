# File > Open xy(z) - Mirone-compatible specialised table imports.
#
# Reading is centralized on `_import_xy_read`: GMT.gmtread reads the table, then the existing
# `_clip_to_display` function clips it to what is on display — one reader, one clipper, and the
# region is read inside that clipper (from the display bounds), never shipped alongside it.
#
# A window that is still a bare launcher has nothing to clip to, so it is PROMOTED first, by
# `_promote_for_vector` (drop.jl) — the same promotion an ordinary dropped table goes through —
# framed on the imported data's own extent with a Base Map under it. Whether the window is bare is
# decided ONCE, C++-side by sceneNeedsBase (the predicate sceneEnsureBase asks), and carried in the
# request envelope.
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
	# Mirone draw_funs.m:loc_quiver autoscaling, exactly. Estimate an effective square grid,
	# then make the largest vector 0.9 of that grid spacing. U and V receive the SAME scale.
	nside = sqrt(length(x))
	delx = (maximum(x) - minimum(x)) / nside
	dely = (maximum(y) - minimum(y)) / nside
	del2 = delx * delx + dely * dely
	maxlen = del2 > 0 ? sqrt(maximum((u .* u .+ v .* v) ./ del2)) : 0.0
	scale = maxlen > 0 ? 0.9 / maxlen : 0.9
	alpha = 0.33
	beta = 0.33
	# THE arrow shape is `_gv_arrow!` (grdvector.jl) — the same one the grdvector dialog draws, with
	# Mirone's head arms in it. Two callers, one function: this import used to spell the shaft and the
	# two arms out again here, which is exactly the duplicated-geometry SACRED_LAW.md forbids. The
	# head at the tip only ("e"), no +n taper (shrink = 1) is what loc_quiver does.
	segs = Matrix{Float64}[]
	for k in eachindex(x)
		_gv_arrow!(segs, x[k], y[k], u[k] * scale, v[k] * scale, "e", alpha, beta, 1.0)
	end
	Dsegs = GMTdataset[GMT.mat2ds(seg) for seg in segs]
	_add_dataset_to_scene(scene, Dsegs, name; forceMode=:lines, noConvertToPoints=true)
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
	# file. A call per row gave a row per point, which is the same flooding the text import had.
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

# Read through the ONE file reader, then use the SAME display clipping path as every other imported
# vector. Never ask gmtselect to read the file and never duplicate its result-shape assumptions.
function _import_xy_read(scene::Ptr{Cvoid}, path::String, empty::Bool)
	D = GMT.gmtread(path; table=true)
	return empty ? D : _clip_to_display(scene, D)
end

# `empty` is the C++ side's own sceneNeedsBase answer, carried in the request envelope — never a
# second "is this window empty" test here (SACRED_LAW.md: one operation, one function).
function _on_import_xy(scene::Ptr{Cvoid}, path::String, mode::Symbol, empty::Bool)::Cvoid
	try
		D = _import_xy_read(scene, path, empty)
		D === nothing &&
			(_viewer_log_error(scene, "Open xy(z): no data inside the displayed region"); return)
		name = splitext(basename(path))[1]
		# A bare launcher is promoted by THE vector promotion (drop.jl), the same one an ordinary
		# dropped table goes through; `backdrop` asks it for a Base Map under the vectors instead of
		# a blank scaffold. The bbox is derived there, once, for both callers.
		empty && _promote_for_vector(scene, D; backdrop=true)
		mode === :points ? _import_xy_points(scene, D, name) :
		mode === :arrows ? _import_xy_arrows(scene, D, name) :
		mode === :scaled ? _import_xy_scaled(scene, D, name) :
		mode === :text ? _import_xy_text(scene, D, name) :
		error("unknown Open xy(z) mode: $mode")
	catch e
		bt = catch_backtrace()
		detail = sprint(showerror, e, bt)
		_viewer_log_error(scene, "Open xy(z) FAILED:\n$(first(detail, min(length(detail), 3000)))")
	end
	return
end
