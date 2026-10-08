# transplant.jl — Grid Tools > "Transplant 2nd grid": a PORT of Mirone's utils/transplants.m,
# IMPLANTGRID mode. Insert an external ("implant") grid into the window's host grid; the two
# resolutions need not match. Mirone's own dispatch and its three jobs are reproduced as they are:
#
#   res == false         -> the host is first resampled to the implant's increment (c_grdsample)
#   no NaNs anywhere     -> job_no_nans          : host skirt (pad 6, inner ring 1) + implant, regridded
#   NaNs in host only    -> job_NanHost_noNanImp : the implant is sampled (grdtrack) at the host's NaN
#                                                  nodes, joined with the host's non-NaN nodes, and the
#                                                  WHOLE host is regridded
#   NaNs in implant only -> job_noNanHost_NanImp : as job_no_nans, but the implant's holes let the host
#                                                  survive there, and gmtmbgrid's -C clip is applied
#   NaNs in both         -> Mirone warns "not yet implemented" and runs job_NanHost_noNanImp
#
# The regridder is Mirone's gmtmbgrid_m, i.e. MB-System's MBGRID as adapted by J. Luis — the SAME
# code `mbgrid` (mbgrid.jl / deps/src/mbgrid.c, ported from gmtmbgrid.c) runs, with the same
# defaults (-W1, clip ALL, no -E) and the options each job passes (-T, -Mz = :zgrid, -C).
#
# Wired from two places (both via g_juliaEval, like Extract profile):
#   • Grid Tools menu  -> whole host grid, no rectangle needed.
#   • a rectangle's context menu -> the rectangle W/E/S/N clips the implant first (the "connection
#     to rectangle handles"); a drawn rectangle is NOT required for the menu path.
#
# Every z is read through `_zmat` — (ny,nx), row 1 = south — whatever layout the grid arrived in
# (SACRED_LAW.md, grid memory-layout law).

# Node coordinate vectors GUARANTEED to match the grid's node counts. GMT.jl usually keeps `G.x`/`G.y`
# consistent with `z`, but some file formats/headers come back with an axis length that disagrees
# with the z array (which is authoritative for the data) — indexing a meshgrid built from the stored
# axes by a z-derived mask then throws BoundsError. So rebuild from range+inc when lengths disagree,
# honouring pixel registration (centres offset half a cell).
function _grid_xy(G::GMTgrid)
	nx, ny = _grid_dims(G)        # from the coordinates, never size(G.z) (grid memory-layout law)
	reg    = G.registration       # 0 = gridline, 1 = pixel
	xv = (length(G.x) == nx) ? collect(Float64, G.x) :
	     collect(G.range[1] + (reg == 1 ? G.inc[1] / 2 : 0.0) .+ (0:nx-1) .* G.inc[1])
	yv = (length(G.y) == ny) ? collect(Float64, G.y) :
	     collect(G.range[3] + (reg == 1 ? G.inc[2] / 2 : 0.0) .+ (0:ny-1) .* G.inc[2])
	return xv, yv, ny, nx
end

# img_fun('bwmorph', M, 'dilate'): grow a node mask by `k` nodes in every direction (square window).
# Separable running max through prefix sums, so it costs O(n) whatever `k` is.
function _mask_dilate(M::AbstractMatrix{Bool}, k::Int)::BitMatrix
	ny, nx = size(M)
	k <= 0 && return BitMatrix(M)
	A  = falses(ny, nx)
	cs = zeros(Int, nx + 1)
	@inbounds for r in 1:ny
		for c in 1:nx;  cs[c+1] = cs[c] + M[r, c];  end
		for c in 1:nx;  A[r, c] = cs[min(nx, c + k) + 1] - cs[max(1, c - k)] > 0;  end
	end
	B  = falses(ny, nx)
	cr = zeros(Int, ny + 1)
	@inbounds for c in 1:nx
		for r in 1:ny;  cr[r+1] = cr[r] + A[r, c];  end
		for r in 1:ny;  B[r, c] = cr[min(ny, r + k) + 1] - cr[max(1, r - k)] > 0;  end
	end
	return B
end

# cropimg(..., rect, 'out_grid'): the host node block (r_c, 1-based, row 1 = south) inside w/e/s/n.
function _tp_crop_rc(xv::Vector{Float64}, yv::Vector{Float64}, dx, dy, w, e, s, n)
	tx = 1e-6 * dx;  ty = 1e-6 * dy
	c0 = findfirst(x -> x >= w - tx, xv);  c1 = findlast(x -> x <= e + tx, xv)
	r0 = findfirst(y -> y >= s - ty, yv);  r1 = findlast(y -> y <= n + ty, yv)
	(c0 === nothing || c1 === nothing || r0 === nothing || r1 === nothing || c0 > c1 || r0 > r1) &&
		error("The implant region holds no host node.")
	return r0, r1, c0, c1
end

# Progress reporting. The core takes `progress(step, text)`; the pure/test path passes nothing.
# Steps (of _TP_NSTEPS): 1 read, 2 resample (adopt-res only), 3 collect/sample, 4 regrid, 5 add.
const _TP_NSTEPS = 5
_tp_noprog(::Int, ::AbstractString) = nothing
# The window's progress dialog (SACRED_LAW.md no-dead-time law): the app's own gmtvtk_progress_*
# pair, the same one the cube scan uses. Best-effort — a missing dialog never fails the transplant.
_tp_dialog(k::Int, s::AbstractString) = (try
	ccall(_fn(:gmtvtk_progress_status), Cvoid, (Cint, Cstring), Cint(k), String(s))
catch; end; nothing)

# gmtmbgrid_m(XX,YY,ZZ, opt_R, opt_I, '-T…', '-Mz', opt_C): the SAME MBGRID engine, zgrid solver.
# The result must land node for node on the block it replaces, so its size is checked.
function _tp_mbgrid(XX, YY, ZZ, region, inc, tension, reg, nx, ny; clipmode = :all, clip = 0,
                    progress = _tp_noprog)
	(length(ZZ) < 4) && error("Not enough valid nodes to regrid.")
	progress(4, "Regridding $(length(ZZ)) points onto $(nx) x $(ny) nodes (gmtmbgrid)…")
	G = mbgrid(XX, YY, ZZ; region = region, inc = inc, tension = tension, solver = :zgrid,
	           clipmode = clipmode, clip = clip, registration = (reg == 0 ? :gridline : :pixel))
	_grid_dims(G) == (nx, ny) || error("regridded block is $(_grid_dims(G)) nodes, expected $((nx, ny))")
	return _zmat(G)
end

# Host nodes of block (r0, c0)+mask selected by `mask` (same shape as the block), NaNs dropped.
function _tp_host_pts!(XX, YY, ZZ, Zh, hxv, hyv, r0, c0, mask)
	@inbounds for ij in findall(mask)
		r, c = ij[1] + r0 - 1, ij[2] + c0 - 1
		z = Zh[r, c];  isnan(z) && continue
		push!(XX, hxv[c]);  push!(YY, hyv[r]);  push!(ZZ, z)
	end
end

# job_no_nans. Host block = implant BB + `pad` host cells; skirt = block nodes OUTSIDE the implant BB
# + 1 host cell; skirt + every implant node regridded. Mirone passes '-T0.25' then '-T100' — the
# second one is what gmtmbgrid_m keeps, so tension 100.
function _tp_job_no_nans(Zh, hxv, hyv, hdx, hdy, reg, I, ix, iy, pad; progress = _tp_noprog)
	progress(3, "Collecting the host skirt and the implant nodes…")
	x_min = max(ix[1] - pad * hdx, hxv[1]);  x_max = min(ix[2] + pad * hdx, hxv[end])
	y_min = max(iy[1] - pad * hdy, hyv[1]);  y_max = min(iy[2] + pad * hdy, hyv[end])
	r0, r1, c0, c1 = _tp_crop_rc(hxv, hyv, hdx, hdy, x_min, x_max, y_min, y_max)
	x1 = max(ix[1] - hdx, hxv[1]);  x2 = min(ix[2] + hdx, hxv[end])
	y1 = max(iy[1] - hdy, hyv[1]);  y2 = min(iy[2] + hdy, hyv[end])
	skirt = BitMatrix([!(x1 <= hxv[c] <= x2 && y1 <= hyv[r] <= y2) for r in r0:r1, c in c0:c1])
	XX = Float64[];  YY = Float64[];  ZZ = Float64[]
	_tp_host_pts!(XX, YY, ZZ, Zh, hxv, hyv, r0, c0, skirt)
	ixv, iyv, nyi, nxi = _grid_xy(I);  Zi = _zmat(I)
	@inbounds for ci in 1:nxi, ri in 1:nyi
		push!(XX, ixv[ci]);  push!(YY, iyv[ri]);  push!(ZZ, Zi[ri, ci])
	end
	Zr = _tp_mbgrid(XX, YY, ZZ, (hxv[c0], hxv[c1], hyv[r0], hyv[r1]), (hdx, hdy), 100.0, 0,
	                c1 - c0 + 1, r1 - r0 + 1; progress = progress)
	return Zr, (r0, r1, c0, c1)
end

# job_NanHost_noNanImp. The host's NaN nodes (grd2xyz -s+r) get the implant sampled there (grdtrack;
# rows that come back NaN are dropped, as grdtrack_m's -S does), joined with the host's non-NaN nodes
# (grd2xyz -s), and the WHOLE host region is regridded at the host increment, -T0.25 -Mz.
function _tp_job_nanhost(Zh, hxv, hyv, hdx, hdy, reg, range4, I; progress = _tp_noprog)
	hny, hnx = length(hyv), length(hxv)
	nanidx = findall(isnan, Zh)
	P = Matrix{Float64}(undef, length(nanidx), 2)
	@inbounds for (k, ij) in enumerate(nanidx)
		P[k, 1] = hxv[ij[2]];  P[k, 2] = hyv[ij[1]]
	end
	ilay = I.layout
	progress(3, "Sampling the implant at $(length(nanidx)) empty host nodes…")
	T = GMT.grdtrack(I, P)
	I.layout = ilay                # GMT.jl may re-label a module's input grid; put the truth back
	TD = T isa GMTdataset ? T.data : (T isa AbstractVector ? T[1].data : T)
	XX = Float64[];  YY = Float64[];  ZZ = Float64[]
	@inbounds for k in 1:size(TD, 1)
		isnan(TD[k, 3]) && continue
		push!(XX, TD[k, 1]);  push!(YY, TD[k, 2]);  push!(ZZ, TD[k, 3])
	end
	_tp_host_pts!(XX, YY, ZZ, Zh, hxv, hyv, 1, 1, trues(hny, hnx))
	Zr = _tp_mbgrid(XX, YY, ZZ, range4, (hdx, hdy), 0.25, reg, hnx, hny; progress = progress)
	return Zr, (1, hny, 1, hnx)
end

# job_noNanHost_NanImp. Block = implant BB + `pad` host cells. Skirt = block nodes outside the polygon
# around the implant's non-NaN nodes dilated by one cell (helper2: bwmorph dilate + bwboundaries), and
# inside that polygon's holes, so the host survives under the implant's holes. Skirt + the implant's
# non-NaN nodes regridded, -T0.25 -Mz, -C5 — or -C<2*round(imp_inc/host_inc)> when the host is the
# finer grid (gmtmbgrid's -C<n> without a suffix = clip mode "gap").
function _tp_job_nanimp(Zh, hxv, hyv, hdx, hdy, reg, I, ix, iy, pad; progress = _tp_noprog)
	progress(3, "Collecting the host skirt and the implant's non-NaN nodes…")
	x_min = max(ix[1] - pad * hdx, hxv[1]);  x_max = min(ix[2] + pad * hdx, hxv[end])
	y_min = max(iy[1] - pad * hdy, hyv[1]);  y_max = min(iy[2] + pad * hdy, hyv[end])
	r0, r1, c0, c1 = _tp_crop_rc(hxv, hyv, hdx, hdy, x_min, x_max, y_min, y_max)
	ixv, iyv, nyi, nxi = _grid_xy(I);  Zi = _zmat(I)
	notnan = BitMatrix(.!isnan.(Zi))
	D = _mask_dilate(notnan, 1)        # inside the outer polygon and not in one of its holes
	idx, idy = I.inc[1], I.inc[2]
	function inD(x, y)
		ci = round(Int, (x - ixv[1]) / idx) + 1;  ri = round(Int, (y - iyv[1]) / idy) + 1
		return 1 <= ci <= nxi && 1 <= ri <= nyi && D[ri, ci]
	end
	skirt = BitMatrix([!inD(hxv[c], hyv[r]) for r in r0:r1, c in c0:c1])
	XX = Float64[];  YY = Float64[];  ZZ = Float64[]
	_tp_host_pts!(XX, YY, ZZ, Zh, hxv, hyv, r0, c0, skirt)
	@inbounds for ci in 1:nxi, ri in 1:nyi
		notnan[ri, ci] || continue
		push!(XX, ixv[ci]);  push!(YY, iyv[ri]);  push!(ZZ, Zi[ri, ci])
	end
	clipn = hdx < idx ? 2 * round(Int, idx / hdx) : 5
	Zr = _tp_mbgrid(XX, YY, ZZ, (hxv[c0], hxv[c1], hyv[r0], hyv[r1]), (hdx, hdy), 0.25, 0,
	                c1 - c0 + 1, r1 - r0 + 1; clipmode = :gap, clip = clipn, progress = progress)
	return Zr, (r0, r1, c0, c1)
end

# The IMPLANTGRID dispatch. `keepres` true keeps the host resolution, false adopts the implant's.
# Returns the new host grid and the replaced block (+ undo snapshot, + Mirone's warning if any).
function _transplant_grid(H::GMTgrid, I::GMTgrid; keepres::Bool=true, pad::Int=6, progress = _tp_noprog)
	hx0, hx1, hy0, hy1 = H.range[1], H.range[2], H.range[3], H.range[4]
	ix0, ix1, iy0, iy1 = I.range[1], I.range[2], I.range[3], I.range[4]

	# Both grids must share a region, else there is nothing to implant.
	(ix1 <= hx0 || ix0 >= hx1 || iy1 <= hy0 || iy0 >= hy1) &&
		error("The to-be-transplanted grid does not have any region in common with the base grid.")

	host_has_nans       = any(isnan, H.z)       # handles.have_nans, taken BEFORE any resample
	implanting_has_nans = any(isnan, I.z)

	# ~res: resample the host to the implant's increment over the host's own region.
	Hwork = if keepres
		deepcopy(H)                    # the result is a NEW grid: the host is never written (derived-variable law)
	else
		lay = H.layout
		progress(2, "Resampling the host to the implant's increment…")
		S = GMT.grdsample(H; region = (hx0, hx1, hy0, hy1), inc = (I.inc[1], I.inc[2]))
		H.layout = lay                 # GMT.grdsample re-labels its INPUT's layout; put the truth back
		S
	end
	hdx, hdy = Hwork.inc[1], Hwork.inc[2]
	hxv, hyv, hny, hnx = _grid_xy(Hwork)
	Zh = _zmat(Hwork)              # a view over Hwork.z's OWN memory, so the write lands in its layout
	ix = (ix0, ix1);  iy = (iy0, iy1)

	warn = ""
	if !host_has_nans && !implanting_has_nans
		Zr, rc = _tp_job_no_nans(Zh, hxv, hyv, hdx, hdy, Hwork.registration, I, ix, iy, pad; progress = progress)
	elseif host_has_nans && !implanting_has_nans
		Zr, rc = _tp_job_nanhost(Zh, hxv, hyv, hdx, hdy, Hwork.registration, Tuple(Hwork.range[1:4]), I; progress = progress)
	elseif !host_has_nans && implanting_has_nans
		Zr, rc = _tp_job_nanimp(Zh, hxv, hyv, hdx, hdy, Hwork.registration, I, ix, iy, pad; progress = progress)
	else
		warn = "NaNs in both Host and Implanting grid is not yet implemented. Uknown result."
		Zr, rc = _tp_job_nanhost(Zh, hxv, hyv, hdx, hdy, Hwork.registration, Tuple(Hwork.range[1:4]), I; progress = progress)
	end
	r0, r1, c0, c1 = rc

	# get_the_output: Z(r_c) = Z_rect — into the WORK copy; the host the window shows stays as it was.
	@views Zh[r0:r1, c0:c1] .= Zr

	# The new grid wraps Hwork's buffer AS IT LIES, so it carries that buffer's layout label too.
	hreg = Hwork.registration      # mat2grid wants nx+1 cell EDGES for a pixel grid, nx nodes otherwise
	Gout = hreg == 0 ? mat2grid(Hwork.z; x=hxv, y=hyv) :
	       mat2grid(Hwork.z; x=collect(range(hxv[1] - hdx / 2; step=hdx, length=hnx + 1)),
	                         y=collect(range(hyv[1] - hdy / 2; step=hdy, length=hny + 1)), reg=1)
	Gout.layout = Hwork.layout
	_grid_command!(Gout, "iGMT Transplant 2nd grid (Mirone transplants.m IMPLANTGRID, gmtmbgrid)")
	isdefined(H, :proj4)   && !isempty(H.proj4)   && (Gout.proj4   = H.proj4)
	isdefined(H, :wkt)     && !isempty(H.wkt)     && (Gout.wkt     = H.wkt)
	Gout.registration = Hwork.registration
	# `block` = the node rectangle Mirone's job rewrote (r_c); `warn` = its warndlg text, if any.
	return Gout, (r0 = r0, r1 = r1, c0 = c0, c1 = c1, warn = warn)
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
			if (k === :grid && (isempty(name) || n == name))
				v[i] = (:grid, n, G);  break
			end
		end
	end
	get(_FIGREG, scene, nothing) isa QtFigure && (_FIGREG[scene] = QtFigure(scene, G))
	return
end

# Replace the window's base grid surface IN PLACE (data + render), keeping camera/VE/name. Reuses the
# same CPT build as a normal grid add so the colours follow the new data range. `zrange` (zmn,zmx)
# overrides the CPT's own autoscale -- used by the cube layer slider's "global min/max" checkbox.
function _apply_host_grid!(scene::Ptr{Cvoid}, G::GMTgrid, name::String; zrange=nothing)
	cmap             = _default_cmap(G)
	cz, crgb, ncolor = zrange !== nothing ? _cpt_nodes_range(zrange[1], zrange[2], cmap) : _cpt_nodes(G, cmap)
	z, nx, ny, zlay = _grid_zbuf(G)   # buffer as it lies + layout code -- NO transposition
	r    = G.range
	geog = _isgeographic(G)
	ok = ccall(_fn(:gmtvtk_replace_base_grid_h), Cint,
		(Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring, Cint),
		scene, z, nx, ny, r[1], r[2], r[3], r[4], Cint(geog), cz, crgb, Cint(ncolor), name, zlay)
	ok == 0 && error("the viewer rejected the base-grid replace (window closed?)")
	_sync_host_grid!(scene, name, G)
	return ok
end

# g_juliaEval entry point. `scene` = the window; `implant_path` = grid to implant; `res` = 1 keep
# host resolution / 0 adopt implant resolution; `rectstr` = "W/E/S/N" from a rectangle context menu
# (empty for the menu path) that clips the implant before implanting; `gname` = the Scene Objects
# name of the grid the window is SHOWING ("" = the base grid) — the host, as every grid tool takes it.
#
# The result is a NEW derived grid (SACRED_LAW.md derived-variable display law): a descriptive name,
# through the ONE transition `_adopt_derived!` — checked, the host unchecked, Scene Objects unfolded.
# The host itself is never written, so there is nothing to undo: the source is still there, and the
# result has its own row with its own Remove.
function _on_transplant(scene::Ptr{Cvoid}, implant_path::String, res::Int=1, rectstr::String="",
                        gname::String="")
	# No-dead-time law: the run shows what it is doing, step by step, from the first read to the add.
	# Raised HERE — the one door both the Grid Tools menu and the rectangle menu come through.
	try ccall(_fn(:gmtvtk_progress_show), Cint, (Cint, Cstring), Cint(_TP_NSTEPS), "Transplant 2nd grid")
	catch end
	try
		H = _find_object(scene, :grid, gname)
		(H === nothing) && error("No host grid in this window to transplant into.")

		_tp_dialog(1, "Reading $(basename(String(implant_path)))…")
		I = _gmtread_trb(implant_path)      # grids are READ in "TRB" — THE reader
		I isa GMTgrid || error("The chosen file is not a grid: $(basename(String(implant_path)))")

		# Optional rectangle clip (the rectangle-handle connection). Intersect the rect with the
		# implant's own range so grdcut never gets an out-of-range region.
		rf = split(rectstr, '/')
		if (length(rf) == 4 && !any(isempty, rf))
			w, e, s, n = parse.(Float64, rf)
			w = max(w, I.range[1]);  e = min(e, I.range[2])
			s = max(s, I.range[3]);  n = min(n, I.range[4])
			(e > w && n > s) && (I = GMT.grdcut(I; region=(w, e, s, n)))
		end

		Gout, blk = _transplant_grid(H, I; keepres=(res != 0), progress=_tp_dialog)
		isempty(blk.warn) || _viewer_log_error(scene, "Transplant: " * blk.warn)   # Mirone's warndlg

		# The base grid is registered unnamed; its Scene Objects label is the live scene's surf_name.
		host = gname
		isempty(host) && (host = try String(get(_scene_state(scene), "surf_name", "")) catch; "" end)
		isempty(host) && (host = "Grid")
		title = "$host + $(basename(String(implant_path))) (transplant, " *
		        (res != 0 ? "host" : "implant") * " res)"
		_tp_dialog(5, "Adding \"$title\" to the window…")
		ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, title)   # re-run replaces
		_forget_object!(scene, :grid, title)
		has_surface = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
		_add_grid_to_scene(scene, Gout, title; promote = (has_surface == 0)) ||
			error("window closed, grid not added")
		_adopt_derived!(scene, title, Gout)        # the ONE derived-variable transition (grid.jl)
		_viewer_log_info(scene, "Transplant: \"$title\" added; \"$host\" is unchanged.")
	catch e
		try ccall(_fn(:gmtvtk_progress_close), Cvoid, ()) catch end   # never leave it over the error
		_tool_failed(scene, "Transplant", e)
	finally
		try ccall(_fn(:gmtvtk_progress_close), Cvoid, ()) catch end
	end
	return nothing
end

# "Transplant 2nd grid" on a NESTING LEVEL's grid — the hollow "layerN" the Nested-grids tool makes
# for a rectangle, or the "layerN.grd" NSWING wrote for that same level and the user opened again.
# Unlike _on_transplant (which blends an implant into a real HOST grid over a smooth seam), a nesting
# level is simply SAMPLED from the implant onto the level's own nodes; nodes the implant doesn't cover
# keep the blank value. `gname` = the level's Scene Objects name; `implant_path` = grid to sample from.
# The old grid is dropped (viewer + registry) and the filled one re-added under the SAME name (visible).
#
# The NODES come from the "Nested rectangle N" polygon, not from the grid on screen: the rectangle is
# what defines a level, it re-quantizes on every drag/edit (nestReflow), and a REfill after a resize
# must produce the level at its new size. So the caller passes the rectangle's current W/E/S/N + cell
# size and a level whose grid no longer matches is REBUILT at those limits before being sampled into.

# Pure core (no scene/registry/ccall — unit-tested): SAMPLE implant `I` onto blank grid `G`'s own node
# spacing+registration over their overlap and write it INTO `G.z` IN PLACE. The blank grid is discarded
# regardless, so we fill its OWN z — no copy, no fresh GMTgrid (G already carries the right
# proj4/wkt/registration/axes). Nodes the implant doesn't cover keep their blank value. grdsample ALWAYS
# returns a Float32 grid, so Isamp.z pastes straight in; only convert G.z if it somehow isn't Float32.
# Returns the (r0,r1,c0,c1) node block filled (1-based). Throws if the grids don't overlap.
function _nested_fill!(G::GMTgrid, I::GMTgrid)
	x0, x1, y0, y1 = G.range[1], G.range[2], G.range[3], G.range[4]
	dx, dy = G.inc[1], G.inc[2]
	# SACRED_LAW.md, grid memory-layout law: dims come from the COORDINATE VECTORS and the paste goes
	# through `_zmat`, never `size(G.z)` / `G.z[iy,ix]`. A freshly created blank layer is column-major
	# ("BCB") and the raw indexing happened to work; the SAME layer read back from a saved session is
	# row-major ("TRB"), so writing grdsample's block straight into `G.z` filed a column-major buffer
	# under a row-major label. Verified live (2026-09-06) on a reloaded tsunami session: the refilled
	# layer's raw buffer matched the implant exactly while the grid it described correlated -0.19 with
	# it — the data was there, scrambled, and every consumer that honours the layout read garbage.
	nx, ny = _grid_dims(G)

	# Region shared by the blank grid and the implant (grdsample can't leave the implant's extent),
	# SNAPPED to the blank grid's own nodes so the sampled block lands on integer node indices.
	w  = max(x0, I.range[1]);  e  = min(x1, I.range[2])
	so = max(y0, I.range[3]);  no = min(y1, I.range[4])
	(e > w && no > so) || error("The implant grid does not overlap the nested grid region.")
	ci0 = max(0, ceil(Int, (w  - x0) / dx - 1e-6));  ci1 = min(nx - 1, floor(Int, (e  - x0) / dx + 1e-6))
	ri0 = max(0, ceil(Int, (so - y0) / dy - 1e-6));  ri1 = min(ny - 1, floor(Int, (no - y0) / dy + 1e-6))
	(ci1 >= ci0 && ri1 >= ri0) || error("The implant grid does not overlap the nested grid region.")
	ws = x0 + ci0 * dx;  es = x0 + ci1 * dx;  sos = y0 + ri0 * dy;  nos = y0 + ri1 * dy

	# Match registration: the blank grid is gridline-registered but the implant (e.g. earth_relief) is
	# often pixel-registered — grdsample would then keep pixel nodes (one fewer per axis), leaving the
	# top row and right column blank.
	Isamp = GMT.grdsample(I; region=(ws, es, sos, nos), inc=(dx, dy),
	                      registration = (G.registration == 0 ? "g" : "p"))
	c0 = ci0 + 1;  r0 = ri0 + 1
	sx, sy = _grid_dims(Isamp)
	r1 = min(r0 + sy - 1, ny);  c1 = min(c0 + sx - 1, nx)
	# Both sides as (ny,nx) south-first views over their OWN memory — the write lands in G's buffer in
	# whatever order that buffer is really in, so `G.layout` stays true after the paste.
	Zg = _zmat(G);  Zs = _zmat(Isamp)
	@views Zg[r0:r1, c0:c1] .= Zs[1:(r1 - r0 + 1), 1:(c1 - c0 + 1)]
	# G.z was pasted by hand (not through a GMT call that recomputes the header) — G.range[5:6] (z_min,
	# z_max) is still the blank grid's stale 0/0 unless refreshed here. This is the bug behind NSWING
	# reading a "blank" nested grid despite real data in G.z: grdinfo (and anything trusting the header
	# instead of scanning z) sees z_min=z_max=0.
	zmn, zmx = extrema(G.z);  G.range[5] = zmn;  G.range[6] = zmx
	return (r0 = r0, r1 = r1, c0 = c0, c1 = c1)
end

# True when `G` already IS the level the rectangle currently describes (same limits, same cell size),
# so it can be filled where it lies. A NaN geometry means the caller had no rectangle to ask.
function _nested_geom_matches(G::GMTgrid, x0, x1, y0, y1, xi, yi)
	all(isfinite, (x0, x1, y0, y1, xi, yi)) || return true
	tol(v) = 1e-6 * max(abs(v), 1.0)
	return abs(G.range[1] - x0) <= tol(x0) && abs(G.range[2] - x1) <= tol(x1) &&
	       abs(G.range[3] - y0) <= tol(y0) && abs(G.range[4] - y1) <= tol(y1) &&
	       abs(G.inc[1]   - xi) <= tol(xi) && abs(G.inc[2]   - yi) <= tol(yi)
end

function _on_nested_transplant(scene::Ptr{Cvoid}, gname::AbstractString, implant_src::String,
                               x0 = NaN, x1 = NaN, y0 = NaN, y1 = NaN, xi = NaN, yi = NaN)
	try
		name = String(gname)
		G = _find_object(scene, :grid, name)
		(G isa GMTgrid) || error("Nested grid '$name' not found in this window.")
		# The rectangle has been resized/re-quantized since this level was materialised: the level is
		# REBUILT at the rectangle's current nodes (the one blank-grid constructor, nested.jl) and the
		# implant sampled into that. Filling the old grid would refill the OLD size, which is the whole
		# thing this option exists to avoid.
		if !_nested_geom_matches(G, x0, x1, y0, y1, xi, yi)
			G = _nested_blank(x0, x1, y0, y1, xi, yi, _isgeographic(G), name)
		end

		# `implant_src` is EITHER the name of a grid already in this window (the chooser's first
		# option: layer0, or any loaded grid in the same coordinate kind whose region totally covers
		# this one) OR a path to an external file. One entry point for both, so the fill below is the
		# same operation whichever the user picked — the choice only decides where the grid comes from.
		local I, srclabel
		Iw = _find_object_exact(scene, :grid, String(implant_src))
		if Iw isa GMTgrid
			I = Iw;  srclabel = String(implant_src)
		else
			I = _gmtread_trb(String(implant_src))   # grids are READ in "TRB" — THE reader
			srclabel = basename(String(implant_src))
		end
		I isa GMTgrid || error("The chosen source is not a grid: $srclabel")

		_nested_fill!(G, I)      # fill the blank grid IN PLACE from the implant
		G.title = name

		# Show the now-FILLED grid (real CPT, visible). WHERE the blank grid lives decides HOW: an EXTRA
		# grid (its own Scene Objects row, exact name match in the registry) is dropped + re-added; but
		# after "Move to new window" the SAME nested grid is the window's BASE surface (registered under
		# name "") — there is no extra to remove, so replace the base in place (gmtvtk_replace_base_grid_h).
		# Keeps the option working identically wherever the grid was moved.
		v = get(_SCENE_OBJS, scene, nothing)
		is_extra = v !== nothing && any(t -> t[1] === :grid && t[2] == name, v)
		if is_extra
			ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
			v !== nothing && filter!(t -> !(t[1] === :grid && t[2] == name), v)
			_add_grid_to_scene(scene, G, name)
		else
			_apply_host_grid!(scene, G, "")   # base surface: replace data in place, keep its surfName
		end
		# SACRED_LAW.md: a grid that has just appeared IS what the window shows — through the ONE
		# transition, so it is checked and EVERY other layer is unchecked whatever its kind. Adding it
		# and stopping there is what left the previous grids standing with their boxes still ticked.
		#
		# Passed WITHOUT an extent (the transition's own no-bbox half, `_NO_BBOX`): filling a nested
		# layer is not a new quantity arriving, it is the level the user is already looking at getting
		# its depths, so the view stays exactly where they put it instead of snapping to the little
		# layer. Show/hide only — not a second transition, the same one asked for half its work.
		_adopt_new_element(scene, name, nothing)

		_viewer_log_info(scene, "Nested grid '$name' filled from $srclabel.")
	catch e
		_tool_failed(scene, "Nested transplant", e)
	end
	return nothing
end

# Undo the transplant on this window: put the kept original subregion back, so the grid is identical
# to what it was before the transplant.
function _on_transplant_undo(scene::Ptr{Cvoid})
	try
		u = get(_TRANSPLANT_ORIG, scene, nothing)
		if (u === nothing)
			_viewer_log_error(scene, "Transplant: nothing to undo.")
			return nothing
		end
		name = _host_grid_name(scene)
		if (u isa GMTgrid)                    # whole-grid case (host geometry had changed)
			_apply_host_grid!(scene, u, name)
		else                                  # paste the kept original subregion back into the grid
			G = _find_object(scene, :grid, "")
			(G === nothing) && error("No host grid in this window to undo into.")
			# Restore the changed block IN PLACE — no copy, no fresh GMTgrid. G (the transplanted host)
			# is replaced by _apply_host_grid! right after, and already carries proj4/wkt/registration.
			@views _zmat(G)[u.r0:u.r1, u.c0:u.c1] .= u.z   # same (ny,nx) south-first view it was taken from
			_apply_host_grid!(scene, G, name)
		end
		delete!(_TRANSPLANT_ORIG, scene)
		_set_transplant_undo(scene, false)    # nothing left to undo -> hide the rectangle-menu entry
		_viewer_log_info(scene, "Transplant: undone (original restored).")
	catch e
		_tool_failed(scene, "Transplant undo", e)
	end
	return nothing
end
