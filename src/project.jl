# project.jl — Tools > "Project": reproject the window's raster into another coordinate reference
# system. Port of Mirone's src_figs/gdal_project.m ("Projections > GDAL project"), with the GDAL
# dropped from the name; it is, exactly as the .m is, an interface to gdalwarp — here GMT.jl's own
# `gdalwarp`, which takes and returns GMTgrid/GMTimage in memory.
#
# The C++ dialog is ProjectDialog (70_window.cpp, loads deps/ui/project_dialog.ui). It owns the
# projection list and the Source box; this file only turns its key=value block into gdalwarp options.
#
# What comes back is a NEW derived variable in the window (SACRED_LAW.md derived-variable display +
# axes laws) through the SAME transitions every other derive tool uses — `_gm3d_deliver` for a grid,
# `_commit_derived_image!` for an image — never a hand-rolled add/hide/reframe sequence. The window's
# CRS store is then re-stamped to the TARGET system, so the status-corner EPSG chip, the Geography
# menu gate and any later GMT call all read the reprojected truth from the one place they already do.

# The gdalwarp option vector for one run. Mirone's own rules, kept verbatim: a resolution beats a
# row/column count, and a row count alone (without columns) means nothing.
function _project_opts(d::Dict{String,String}, isimage::Bool)
	t_srs = _get(d, "t_srs")
	isempty(t_srs) && error("no destination referencing system given")
	opts = String["-t_srs", t_srs]
	s_srs = _get(d, "s_srs")
	# "+proj=latlong" alone is what a plain lon/lat window carries; it is also gdalwarp's own default
	# reading of an ungeoreferenced raster, so passing it changes nothing — the .m skips it too.
	(isempty(s_srs) || s_srs == "+proj=latlong") || append!(opts, ["-s_srs", s_srs])
	r = _get(d, "resample")
	isempty(r) || append!(opts, ["-r", r])
	xinc, yinc = _get(d, "xinc"), _get(d, "yinc")
	rows, cols = _get(d, "rows"), _get(d, "cols")
	if !isempty(xinc)
		append!(opts, ["-tr", xinc, isempty(yinc) ? xinc : yinc])
	elseif !isempty(rows) && !isempty(cols)
		append!(opts, ["-ts", cols, rows])
	end
	# An image has no NaN to mark the empties a rotated/curved warp leaves in the corners; Mirone
	# fills them with 255 (white) by setting hdr.nodata, so do the same. Grids get GMT.jl's own
	# "-dstnodata NaN" automatically (gdal_tools.jl helper_run_GDAL_fun).
	isimage && append!(opts, ["-dstnodata", "255"])
	return opts
end

# The destination EPSG code for this run, or 0 when the run genuinely has none (a bare projection
# with no registered code — Mollweide, Robinson, a hand-written PROJ4). Two sources, both explicit:
# `t_epsg` (the dialog's picked entry, sent only while the box still holds that entry's own string)
# and an "EPSG:nnnn" typed into the Destination box itself.
function _project_epsg(d::Dict{String,String})
	e = tryparse(Int, _get(d, "t_epsg"))
	(e !== nothing && e > 0) && return e
	m = match(r"^\s*(?:EPSG|epsg)\s*:\s*(\d+)\s*$", _get(d, "t_srs"))
	m === nothing && return 0
	n = tryparse(Int, m.captures[1])
	return (n === nothing || n <= 0) ? 0 : n
end

# True when this PROJ4/WKT/EPSG string names a geographic (lon/lat) system. AUTHORITATIVE for the
# result's axes: a projected grid in metres whose extent happens to be small would otherwise be
# guessed geographic by the range heuristic and get "lon"/"lat" axis labels (see _add_grid_to_scene).
function _project_is_geog(t_srs::AbstractString)
	s = lowercase(t_srs)
	(occursin("latlong", s) || occursin("longlat", s)) && return true
	(occursin("geogcs", s) && !occursin("projcs", s))  && return true
	return s == "epsg:4326" || s == "4326"
end

# --- the VECTOR half (Mirone do_project's tail) -------------------------------------------------
# gdal_project.m warps the raster and then re-projects the figure's lines, patches and text into the
# result. The result lands in THIS window here (derived-variable display law), so those elements are
# moved IN PLACE: read every vector point out of the scene, transform it, write it back — the C side
# walks one list in one order for both halves (sceneVisitVectorXY, 90_c_api.cpp).

# Every vector point in the window as an n×2 matrix (empty when there are none).
function _vector_xy_get(scene::Ptr{Cvoid})
	n = ccall(_fn(:gmtvtk_vector_points_count_h), Cint, (Ptr{Cvoid},), scene)
	n <= 0 && return zeros(Float64, 0, 2)
	buf = zeros(Float64, 2 * Int(n))
	got = ccall(_fn(:gmtvtk_vector_points_get_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint),
	            scene, buf, Cint(length(buf)))
	got == n || error("vector readout returned $got points, expected $n")
	return permutedims(reshape(buf, 2, Int(n)))
end

# Put them back. The count must match what was read, or the C side refuses (a partial write would
# scramble the scene) — so a projection that DROPPED points is caught here, before the call.
function _vector_xy_set(scene::Ptr{Cvoid}, M::Matrix{Float64})
	n = size(M, 1)
	n == 0 && return 0
	buf = vec(permutedims(M[:, 1:2]))
	return Int(ccall(_fn(:gmtvtk_vector_points_set_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint),
	                 scene, buf, Cint(n)))
end

# Transform them from `s_srs` to `t_srs`. `s_srs` empty = plain lon/lat (GMT.lonlat2xy's own WGS84
# default), which is what an unreferenced geographic window carries. ONE ogr2ogr call for the whole
# window — the same one-shot `ogrproj` Mirone's do_project uses. A projection that dropped points
# (anything outside the target's domain) is an error here rather than a scrambled scene: the write
# side matches strictly on count.
function _vector_xy_project(M::Matrix{Float64}, s_srs::AbstractString, t_srs::AbstractString)
	size(M, 1) == 0 && return M
	P = isempty(s_srs) ? GMT.lonlat2xy(M; t_srs = String(t_srs)) :
	                     GMT.lonlat2xy(M; s_srs = String(s_srs), t_srs = String(t_srs))
	Q = P isa Matrix ? Float64.(P) : Float64.(P.data)
	size(Q, 1) == size(M, 1) ||
		error("projection returned $(size(Q,1)) of $(size(M,1)) vector points — nothing moved")
	return Q
end

# C callback (OK button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaProjectFn. Returns Cint 1 on success, 0 on failure.
function _on_project(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The DISPLAYED raster, grid first (a window can hold both). Same resolution order the rest of
		# the derive tools use: the dialog names the active grid layer, images answer by kind.
		O = _find_object(scene, :grid, _get(d, "grid"))
		if O === nothing
			O = _find_object_named(scene, :image)[2]
		end
		O === nothing && error("this window has no grid or image to project")
		isimage = O isa GMTimage

		t_srs = _get(d, "t_srs")
		# The destination EPSG, when the run HAS one: the code the dialog's projection entry carries
		# (sent only while its own string is still in the box), or one typed straight into it. A PROJ4
		# string can NOT be turned back into a code — GDAL's identify says "Failed to identify EPSG
		# code" for every one of these — so it has to come from the choice, and it is worth having:
		# warping to "EPSG:<code>" makes GDAL write authority nodes into the result's WKT, so the
		# window's EPSG chip shows the real code instead of 000.
		t_epsg = _project_epsg(d)
		t_epsg > 0 && (t_srs = "EPSG:$t_epsg")
		d["t_srs"] = t_srs
		opts  = _project_opts(d, isimage)
		R = GMT.gdalwarp(O, opts)
		(R isa GMTgrid || R isa GMTimage) || error("gdalwarp returned a $(typeof(R)), not a grid or image")

		# Mirone's own name for the result: "Reprojected (<projection>) grid|image". With no projection
		# picked from the combo (a hand-typed PROJ4) the target string itself is the description.
		prj = _get(d, "projname")
		isempty(prj) && (prj = length(t_srs) > 48 ? t_srs[1:48] * "…" : t_srs)
		title = "Reprojected ($prj) " * (isimage ? "image" : "grid")

		# Warp the grid, display the grid. The result opens in ITS OWN window, through the SAME
		# `view_grid` / `iview_image_obj` a file does — the path that is known good for a projected
		# raster, and Mirone's own behaviour too (do_project ends in `mirone(ras,tmp)`, a new figure).
		# Nothing is adopted into the source window, so nothing has to be re-scaled, re-framed or
		# re-shaded around it.
		if isimage
			iview_image_obj(R, title; title = title)
		else
			view_grid(R; title = title)
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Project FAILED: $(sprint(showerror, e))")
		@warn "Project FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_project()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_project, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_project_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
