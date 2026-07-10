# transplant.jl — Grid Tools > "Transplant 2nd grid" (port of Mirone's utils/transplants.m,
# IMPLANTGRID mode). Insert an external ("implant") grid into the window's host grid. The two
# resolutions need not match: over the seam we reinterpolate so host and implant join smoothly.
#
# Mirone leaves a 1-cell padding ring between the datasets and regrids a skirt of host nodes
# together with the implant nodes through gmtmbgrid (-T0.25). GMT.jl has no gmtmbgrid, so we use
# `GMT.surface` with the SAME tension (0.25) as the minimum-curvature substitute — it takes the
# scattered (skirt host + implant) points and rebuilds the seam tile.
#
# Wired from two places (both via g_juliaEval, like Extract profile):
#   • Grid Tools menu  -> whole host grid, no rectangle needed.
#   • a rectangle's context menu -> the rectangle W/E/S/N clips the implant first (the "connection
#     to rectangle handles"); a drawn rectangle is NOT required for the menu path.

# Node coordinate vectors GUARANTEED to match `size(G.z)` (ny,nx). GMT.jl usually keeps `G.x`/`G.y`
# consistent with `z`, but some file formats/headers come back with an axis length that disagrees
# with the z array (which is authoritative for the data) — indexing a meshgrid built from the stored
# axes by a z-derived mask then throws BoundsError. So rebuild from range+inc when lengths disagree,
# honouring pixel registration (centres offset half a cell).
function _grid_xy(G::GMTgrid)
	ny, nx = size(G.z)
	reg    = G.registration       # 0 = gridline, 1 = pixel
	xv = length(G.x) == nx ? collect(Float64, G.x) :
	     collect(G.range[1] + (reg == 1 ? G.inc[1] / 2 : 0.0) .+ (0:nx-1) .* G.inc[1])
	yv = length(G.y) == ny ? collect(Float64, G.y) :
	     collect(G.range[3] + (reg == 1 ? G.inc[2] / 2 : 0.0) .+ (0:ny-1) .* G.inc[2])
	return xv, yv, ny, nx
end

# Do the actual implant. `keepres` true keeps the host resolution, false adopts the implant's.
# `pad` (host cells) is the seam width regridded for a smooth transition (Mirone's pad = 6).
function _transplant_grid(H::GMTgrid, I::GMTgrid; keepres::Bool=true, pad::Int=6)
	hx0, hx1, hy0, hy1 = H.range[1], H.range[2], H.range[3], H.range[4]
	ix0, ix1, iy0, iy1 = I.range[1], I.range[2], I.range[3], I.range[4]

	# Both grids must share a region, else there is nothing to implant.
	(ix1 <= hx0 || ix0 >= hx1 || iy1 <= hy0 || iy0 >= hy1) &&
		error("The implant grid has no region in common with the host grid.")

	# res == false -> resample the host to the implant increment first (Mirone lines 134-138), so
	# everything downstream lives on one grid. res == true -> host stays as is.
	Hwork = keepres ? H : GMT.grdsample(H; region=(hx0, hx1, hy0, hy1), inc=(I.inc[1], I.inc[2]))
	hdx, hdy = Hwork.inc[1], Hwork.inc[2]

	# Seam tile = implant bounding box + `pad` host cells, clamped to the host — cropped on host nodes.
	x_min = max(ix0 - pad * hdx, hx0);  x_max = min(ix1 + pad * hdx, hx1)
	y_min = max(iy0 - pad * hdy, hy0);  y_max = min(iy1 + pad * hdy, hy1)
	tile  = GMT.grdcut(Hwork; region=(x_min, x_max, y_min, y_max))
	tx, ty, nyt, nxt = _grid_xy(tile)

	# Inner rect (implant BB + 1 host cell): host nodes OUTSIDE it form the skirt that is regridded
	# with the implant so the seam is smooth; the implant owns everything inside.
	x1 = max(ix0 - hdx, hx0);  x2 = min(ix1 + hdx, hx1)
	y1 = max(iy0 - hdy, hy0);  y2 = min(iy1 + hdy, hy1)

	# Host skirt points (tile nodes outside the inner rect). z is (ny,nx) row=y, col=x.
	TX   = repeat(reshape(tx, 1, :), nyt, 1)
	TY   = repeat(reshape(ty, :, 1), 1, nxt)
	skirt = .!((TX .>= x1) .& (TX .<= x2) .& (TY .>= y1) .& (TY .<= y2))
	XXs, YYs, ZZs = TX[skirt], TY[skirt], Float64.(tile.z[skirt])
	gs   = .!isnan.(ZZs)                                   # host may carry NaNs under the skirt

	# Implant points (all nodes; only the outside NaNs are dropped — inner holes are ignored, and
	# surface fills them from the surrounding data, matching Mirone's intent).
	ixv, iyv, nyi, nxi = _grid_xy(I)
	IX   = repeat(reshape(ixv, 1, :), nyi, 1)
	IY   = repeat(reshape(iyv, :, 1), 1, nxi)
	XXi, YYi, ZZi = IX[:], IY[:], Float64.(I.z[:])
	gi   = .!isnan.(ZZi)

	XX = vcat(XXs[gs], XXi[gi]);  YY = vcat(YYs[gs], YYi[gi]);  ZZ = vcat(ZZs[gs], ZZi[gi])
	length(ZZ) < 4 && error("Not enough valid nodes to interpolate the seam.")

	# gmtmbgrid substitute: minimum-curvature surface with the same tension Mirone uses (-T0.25).
	Gtile = GMT.surface([XX YY ZZ]; region=(tx[1], tx[end], ty[1], ty[end]),
	                    inc=(hdx, hdy), tension=0.25)

	# Paste the seam tile back into a copy of the (working) host, addressed by host node index.
	hxv, hyv, _, _ = _grid_xy(Hwork)
	Zout = copy(Hwork.z)
	r0 = round(Int, (ty[1] - hyv[1]) / hdy) + 1
	c0 = round(Int, (tx[1] - hxv[1]) / hdx) + 1
	gny, gnx = size(Gtile.z)
	r1 = min(r0 + gny - 1, size(Zout, 1));  c1 = min(c0 + gnx - 1, size(Zout, 2))
	Zout[r0:r1, c0:c1] .= Float32.(Gtile.z[1:(r1 - r0 + 1), 1:(c1 - c0 + 1)])

	Gout = mat2grid(Zout; x=hxv, y=hyv)
	isdefined(H, :proj4)   && !isempty(H.proj4)   && (Gout.proj4   = H.proj4)
	isdefined(H, :wkt)     && !isempty(H.wkt)     && (Gout.wkt     = H.wkt)
	Gout.registration = Hwork.registration
	# `block` = the ONLY node rectangle that changed (r0:r1, c0:c1). `samegeom` is true when the host
	# geometry was untouched (keepres) so those indices also address the ORIGINAL host — the undo then
	# needs to keep just this block; otherwise (resampled host) the whole grid changed.
	return Gout, (r0 = r0, r1 = r1, c0 = c0, c1 = c1, samegeom = (Hwork === H))
end

# ── modify-in-place + undo ────────────────────────────────────────────────────────────────────
# Per window, the ORIGINAL subregion that the last transplant overwrote — just the changed node
# rectangle and its pre-transplant z values, so undo puts it back and the grid is identical to the
# original (no full-grid copy). Value = (r0,r1,c0,c1, z) for the common in-place case, or a whole
# GMTgrid when the host geometry itself changed (adopt-implant-resolution). C++ stays history-free.
const _TRANSPLANT_ORIG = Dict{Ptr{Cvoid}, Any}()

# Tell the viewer whether an undo is currently available, so the rectangle context menu can show/hide
# its "Undo transplant" entry. Guarded: a DLL missing the export must not break the transplant itself.
function _set_transplant_undo(scene::Ptr{Cvoid}, on::Bool)
	try
		ccall(_fn(:gmtvtk_set_transplant_undo), Cvoid, (Ptr{Cvoid}, Cint), scene, Cint(on))
	catch
	end
	return
end

# The Scene Objects label of the window's base grid (first :grid entry). "" when unknown -> the C++
# side then keeps whatever name the surface already has.
function _host_grid_name(scene::Ptr{Cvoid})
	v = get(_SCENE_OBJS, scene, nothing)
	v !== nothing && for (k, n, _) in v
		k === :grid && return n
	end
	return ""
end

# Point the registries at the new base-grid data (same window, same name) so Save / Info / a further
# transplant all see the modified grid, not the stale original.
function _sync_host_grid!(scene::Ptr{Cvoid}, name::AbstractString, G::GMTgrid)
	v = get(_SCENE_OBJS, scene, nothing)
	if v !== nothing
		for i in eachindex(v)
			k, n, _ = v[i]
			if k === :grid && (isempty(name) || n == name)
				v[i] = (:grid, n, G);  break
			end
		end
	end
	get(_FIGREG, scene, nothing) isa QtFigure && (_FIGREG[scene] = QtFigure(scene, G))
	return
end

# Replace the window's base grid surface IN PLACE (data + render), keeping camera/VE/name. Reuses the
# same CPT build as a normal grid add so the colours follow the new data range.
function _apply_host_grid!(scene::Ptr{Cvoid}, G::GMTgrid, name::AbstractString)
	cmap             = _default_cmap(G)
	cz, crgb, ncolor = _cpt_nodes(G, cmap)
	z    = eltype(G.z) === Float32 ? G.z : Float32.(G.z)
	ny, nx = size(z);  r = G.range
	geog = _isgeographic(G)
	ok = ccall(_fn(:gmtvtk_replace_base_grid_h), Cint,
		(Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring),
		scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(geog),
		cz, crgb, Cint(ncolor), String(name))
	ok == 0 && error("the viewer rejected the base-grid replace (window closed?)")
	_sync_host_grid!(scene, name, G)
	return ok
end

# g_juliaEval entry point. `scene` = the window; `implant_path` = grid to implant; `res` = 1 keep
# host resolution / 0 adopt implant resolution; `rectstr` = "W/E/S/N" from a rectangle context menu
# (empty for the menu path) that clips the implant before implanting. The host grid is modified IN
# PLACE (no new layer); the ORIGINAL host is kept so the operation can be undone (Ctrl+Z or the
# rectangle's context menu).
function _on_transplant(scene::Ptr{Cvoid}, implant_path::AbstractString,
                        res::Integer=1, rectstr::AbstractString="")
	try
		H = _find_object(scene, :grid, "")
		H === nothing && error("No host grid in this window to transplant into.")

		I = GMT.gmtread(String(implant_path))
		I isa GMTgrid || error("The chosen file is not a grid: $(basename(String(implant_path)))")

		# Optional rectangle clip (the rectangle-handle connection). Intersect the rect with the
		# implant's own range so grdcut never gets an out-of-range region.
		rf = split(String(rectstr), '/')
		if length(rf) == 4 && !any(isempty, rf)
			w, e, s, n = parse.(Float64, rf)
			w = max(w, I.range[1]);  e = min(e, I.range[2])
			s = max(s, I.range[3]);  n = min(n, I.range[4])
			(e > w && n > s) && (I = GMT.grdcut(I; region=(w, e, s, n)))
		end

		Gout, blk = _transplant_grid(H, I; keepres=(res != 0))
		name = _host_grid_name(scene)

		# Keep ONLY the original subregion that changed (undo restores it exactly). If the host
		# geometry itself changed (resample), keep the whole original grid instead.
		_TRANSPLANT_ORIG[scene] = blk.samegeom ?
			(r0 = blk.r0, r1 = blk.r1, c0 = blk.c0, c1 = blk.c1,
			 z = copy(H.z[blk.r0:blk.r1, blk.c0:blk.c1])) : H
		_apply_host_grid!(scene, Gout, name)
		_set_transplant_undo(scene, true)     # an undo is now available (Ctrl+Z / rectangle menu)

		_viewer_log_error(scene, "Transplant: host grid modified in place with " *
		                         "$(basename(String(implant_path))) ($(res != 0 ? "host" : "implant") " *
		                         "resolution). Undo with Ctrl+Z or the rectangle menu.")
	catch e
		_viewer_log_error(scene, "Transplant FAILED: $(sprint(showerror, e))")
		@warn "Transplant FAILED" exception=(e,)
	end
	return nothing
end

# "Transplant 2nd grid" on a "Nested grid N" BLANK grid (the hollow grid the Nested-grids tool makes
# for a rectangle). Unlike _on_transplant (which blends an implant into a real HOST grid over a smooth
# seam), the nested grid has no data — every node is blank — so we simply SAMPLE the implant onto this
# grid's own nodes and REPLACE the blank values. Nodes the implant doesn't cover keep their blank value.
# `gname` = the blank grid's Scene Objects name ("Nested grid N"); `implant_path` = grid to sample from.
# The blank grid is dropped (viewer + registry) and a FILLED grid re-added under the SAME name (visible).
function _on_nested_transplant(scene::Ptr{Cvoid}, gname::AbstractString, implant_path::AbstractString)
	try
		name = String(gname)
		G = _find_object(scene, :grid, name)
		(G isa GMTgrid) || error("Nested blank grid '$name' not found in this window.")

		I = GMT.gmtread(String(implant_path))
		I isa GMTgrid || error("The chosen file is not a grid: $(basename(String(implant_path)))")

		x0, x1, y0, y1 = G.range[1], G.range[2], G.range[3], G.range[4]
		dx, dy = G.inc[1], G.inc[2]

		# Region shared by the blank grid and the implant (grdsample can't leave the implant's extent).
		w  = max(x0, I.range[1]);  e  = min(x1, I.range[2])
		so = max(y0, I.range[3]);  no = min(y1, I.range[4])
		(e > w && no > so) || error("The implant grid does not overlap the nested grid region.")

		# Sample the implant onto THIS grid's node spacing over the overlap, then paste it into a copy of
		# the blank z (nodes outside the implant footprint keep their blank value).
		Isamp = GMT.grdsample(I; region=(w, e, so, no), inc=(dx, dy))
		xv, yv, ny, nx = _grid_xy(G)
		Z  = eltype(G.z) === Float32 ? copy(G.z) : Float32.(G.z)
		c0 = round(Int, (Isamp.range[1] - x0) / dx) + 1
		r0 = round(Int, (Isamp.range[3] - y0) / dy) + 1
		sy, sx = size(Isamp.z)
		r1 = min(r0 + sy - 1, size(Z, 1));  c1 = min(c0 + sx - 1, size(Z, 2))
		Z[r0:r1, c0:c1] .= Float32.(Isamp.z[1:(r1 - r0 + 1), 1:(c1 - c0 + 1)])

		Gout = mat2grid(Z; x=xv, y=yv)
		isdefined(G, :proj4) && !isempty(G.proj4) && (Gout.proj4 = G.proj4)
		isdefined(G, :wkt)   && !isempty(G.wkt)   && (Gout.wkt   = G.wkt)
		Gout.registration = G.registration
		Gout.title = name

		# Replace the blank grid IN PLACE: drop it (viewer + registry) and re-add the FILLED grid under
		# the same name. The filled grid builds a real CPT (no solid-colour override) and is visible.
		ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
		v = get(_SCENE_OBJS, scene, nothing)
		v !== nothing && filter!(t -> !(t[1] === :grid && t[2] == name), v)
		_add_grid_to_scene(scene, Gout, name)

		_viewer_log_error(scene, "Nested grid '$name' filled from $(basename(String(implant_path))).")
	catch e
		_viewer_log_error(scene, "Nested transplant FAILED: $(sprint(showerror, e))")
		@warn "Nested transplant FAILED" exception=(e,)
	end
	return nothing
end

# Undo the transplant on this window: put the kept original subregion back, so the grid is identical
# to what it was before the transplant.
function _on_transplant_undo(scene::Ptr{Cvoid})
	try
		u = get(_TRANSPLANT_ORIG, scene, nothing)
		if u === nothing
			_viewer_log_error(scene, "Transplant: nothing to undo.")
			return nothing
		end
		name = _host_grid_name(scene)
		if u isa GMTgrid                      # whole-grid case (host geometry had changed)
			_apply_host_grid!(scene, u, name)
		else                                  # paste the kept original subregion back into the grid
			G = _find_object(scene, :grid, "")
			G === nothing && error("No host grid in this window to undo into.")
			Z = copy(G.z)
			Z[u.r0:u.r1, u.c0:u.c1] .= u.z
			G0 = mat2grid(Z; x=collect(G.x), y=collect(G.y))
			isdefined(G, :proj4) && !isempty(G.proj4) && (G0.proj4 = G.proj4)
			isdefined(G, :wkt)   && !isempty(G.wkt)   && (G0.wkt   = G.wkt)
			G0.registration = G.registration
			_apply_host_grid!(scene, G0, name)
		end
		delete!(_TRANSPLANT_ORIG, scene)
		_set_transplant_undo(scene, false)    # nothing left to undo -> hide the rectangle-menu entry
		_viewer_log_error(scene, "Transplant: undone (original restored).")
	catch e
		_viewer_log_error(scene, "Transplant undo FAILED: $(sprint(showerror, e))")
		@warn "Transplant undo FAILED" exception=(e,)
	end
	return nothing
end
