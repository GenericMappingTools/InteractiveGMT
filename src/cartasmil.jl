# cartasmil.jl — Tools > PT tools > "Cartas Militares". Port of Mirone's `cartas_militares.m` in its
# FIRST mode (the one `lidarpt.jl` skips): the 1:25000 military map sheets of mainland Portugal.
#
# The C++ dialog is the SAME picker the LIDAR2011 tool uses (LidarPicker in PT_Cartas mode,
# 70_window.cpp) — in Mirone the two tools are literally one figure, and here they are one class: the
# background image, the pan/zoom map, the tile mesh, the right-click selection and its bounding box
# exist once. What differs is the TABLE pushed into it and what the action button does with the picks.
#
# THE SHEET GRID. `cartas_militares.m` builds it in the "Coordenadas Militares" frame (Datum Lisboa):
# 19 columns of 16000 m by 58 rows of 10000 m starting at (72000, 0), minus the cells that fall in the
# sea — `naos{n}` lists, per column, the rows that do NOT exist. Sheets are numbered 1..N running from
# the TOP row down, west to east; a handful carry a letter instead ('401A' — Mirone's "exceptions
# TYPE I", which fill cells the `naos` table left out), and three are not whole rectangles at all
# ('325B', '325C', '611' — "exceptions TYPE II"). All of that is transcribed below, and nothing else
# about the grid is invented: the numbering falls out of the same loop Mirone runs.
#
# THE BACKGROUND. The picker paints data/PTimg_lidar.jpg, whose hard-coded extent is in the LIDAR
# survey's ETRS89/PT-TM06 metres — another CRS. Its four corners are therefore converted into the
# military frame here (GMT's own transform, never a hand-rolled datum shift) and pushed in with
# gmtvtk_lidar_set_bg before the tiles.
#
# GEOREFERENCING A SHEET is Mirone's third method, the one its own header calls the best: the sheet's
# corner coordinates are KNOWN — they are the cell it occupies in this grid — so the image read from
# disk (or from the web) is stamped with them and with the Datum Lisboa projection. The Ozi `.map`
# and world-file branches are not carried over: they exist for sheets scanned by someone else, and
# what this window has is exactly the internal geometry.

# The military ("Coordenadas Militares", Datum Lisboa) frame every sheet is referenced in, verbatim
# from bdnTile's `proj` string.
const _CM_PROJ4 = "+proj=tmerc +lat_0=39.66666666666666 +lon_0=-8.131906111111111 +k=1.0 " *
                  "+x_0=200000 +y_0=300000 +ellps=intl +towgs84=-304.046,-60.576,103.640,0,0,0,0"
# The frame the background image's hard-coded extent is in (the LIDAR survey's, _LIDAR_WKT's
# parameters as PROJ4) — only ever used to convert those four corners into the frame above.
const _CM_BG_PROJ4 = "+proj=tmerc +lat_0=39.66825833333333 +lon_0=-8.133108333333334 +k=1 " *
                     "+x_0=0 +y_0=0 +ellps=GRS80 +units=m +no_defs"

const _CM_X0, _CM_Y0 = 72000.0, 0.0        # xMap_min / yMap_min
const _CM_XINC, _CM_YINC = 16000.0, 10000.0
const _CM_NCOLS, _CM_NROWS = 19, 58

# `naos{n}`: per column, the rows that have NO sheet (sea, or Spain). Transcribed literally.
const _CM_NAOS = Vector{Int}[
	[1:19; 22:58], [1:19; 28:58], [1:16; 30:58], [4:16; 35:58], [1; 43:47; 57:58],
	[1, 58], [1, 58], [1], [56, 58], collect(56:58),
	collect(56:58), [1, 2, 56, 57, 58], [1:9; 29:30; 56:58], [1:11; 16:20; 26:30; 57:58],
	[1:12; 15:22; 25:31; 36; 57:58], [1:46; 57:58], [1:47; 57:58], [1:49; 54:58], collect(1:58)]

# Exceptions TYPE I: cells the `naos` table skipped that DO carry a sheet, under a lettered name.
# col => (rows, names) — Mirone's ABs, same pairing order.
const _CM_ABS = Dict{Int,Tuple{Vector{Int},Vector{String}}}(
	1  => ([22],             ["401A"]),
	2  => ([19],             ["441B"]),
	3  => ([30],             ["306B"]),
	4  => ([4, 12, 35, 36, 37], ["583A", "515A", "248B", "238A", "227B"]),
	5  => ([43],             ["162A"]),
	13 => ([29, 30, 56],     ["325A", "315A", "9A"]),
	14 => ([16, 20, 30],     ["483A", "441A", "315B"]),
	15 => ([12, 31, 36],     ["525A", "306A", "248A"]),
	16 => ([38, 40, 46],     ["227A", "205A", "142A"]),
	18 => ([49],             ["108A"]),
	19 => ([52],             ["67A"]))

# Exceptions TYPE II: sheets that are not a whole 16x10 km cell. (x0, x1, y0, y1, name).
const _CM_ODD = [(76000.0,  83659.0, 270000.0, 280125.0, "325B"),
                 (96344.0, 104000.0, 270000.0, 280000.0, "325C"),
                 (_CM_X0 + 9 * _CM_XINC, _CM_X0 + 10 * _CM_XINC, -617.0, 10000.0, "611")]

# The sheet table, built once per session: `rects` is 4 x N (row 1 = the grid's global bounding box,
# the same convention lidarPT.dat uses — the extreme tile ORIGINS, not the outer edges), `names` the
# parallel names, and `bynames` each sheet's own rectangle so a pick can be turned back into corner
# coordinates without re-deriving anything.
struct CartasTable
	rects::Matrix{Float64}
	names::Vector{String}
	bynames::Dict{String,NTuple{4,Float64}}
end

const _CM_TABLE = Ref{Union{CartasTable,Nothing}}(nothing)

function _cm_table()::CartasTable
	t = _CM_TABLE[]
	t === nothing || return t
	rects = Vector{NTuple{4,Float64}}()
	names = String[]
	push!(rects, (_CM_X0, _CM_X0 + (_CM_NCOLS - 1) * _CM_XINC, _CM_Y0, _CM_Y0 + (_CM_NROWS - 1) * _CM_YINC))
	push!(names, "")                                  # row 1 is the bbox; its name is unused
	cell(m, n) = (_CM_X0 + (n - 1) * _CM_XINC, _CM_X0 + n * _CM_XINC,
	              _CM_Y0 + (m - 1) * _CM_YINC, _CM_Y0 + m * _CM_YINC)
	# The numbering loop, Mirone's verbatim: rows from the top down, columns west to east, the counter
	# advancing only over cells that exist.
	nr = 1
	for m in _CM_NROWS:-1:1, n in 1:_CM_NCOLS
		(m in _CM_NAOS[n]) && continue
		push!(rects, cell(m, n))
		push!(names, string(nr, pad = 3))             # sprintf('%.3d', nr)
		nr += 1
	end
	for (n, (rows, nms)) in _CM_ABS, k in eachindex(rows)
		push!(rects, cell(rows[k], n))
		push!(names, nms[k])
	end
	for (x0, x1, y0, y1, nm) in _CM_ODD
		push!(rects, (x0, x1, y0, y1))
		push!(names, nm)
	end
	m = Matrix{Float64}(undef, 4, length(rects))
	for k in eachindex(rects)
		m[1, k], m[2, k], m[3, k], m[4, k] = rects[k]
	end
	by = Dict{String,NTuple{4,Float64}}()
	for k in 2:length(names)
		by[names[k]] = rects[k]
	end
	t = CartasTable(m, names, by)
	_CM_TABLE[] = t
	return t
end

# The background image's extent in THIS map's coordinates: the LIDAR picker's hard-coded corners
# (which are PT-TM06 metres) converted to the military frame. Returns nothing if the conversion is
# not available, in which case the picker keeps its own default and simply shows the image where
# LIDAR2011 shows it.
function _cm_bg_extent()
	# The same four numbers LidarArea carries as BGX0..BGY1 (lidarPT(): "Hard code this limits").
	x = [-123787.423169315, 92138.1835464904]
	y = [-303489.079090607, 261783.41534662]
	corners = [x[1] y[1]; x[2] y[1]; x[2] y[2]; x[1] y[2]]
	try
		xy = GMT.lonlat2xy(corners; s_srs = _CM_BG_PROJ4, t_srs = _CM_PROJ4)
		return (minimum(xy[:, 1]), maximum(xy[:, 1]), minimum(xy[:, 2]), maximum(xy[:, 2]))
	catch e
		@warn "Cartas Militares: could not place the background image" exception=(e,)
		return nothing
	end
end

# The file holding a sheet, in the order bdnTile tries them: <name>.gif, then the Mr. Sid version.
# Both cases of the extension are tried — a directory of sheets copied off a CD is as likely to shout
# them in capitals.
function _cm_sheet_path(dir::AbstractString, name::AbstractString)::String
	for ext in (".gif", ".GIF", ".sid", ".SID", ".tif", ".TIF")
		p = joinpath(String(dir), String(name) * ext)
		isfile(p) && return p
	end
	return ""
end

# bdnTile's web branch: the sheet is <url>/<NAME>.GIF. The address is the user's own — nothing is
# ever filled in for them, here or in the dialog.
function _cm_sheet_url(url::AbstractString, name::AbstractString)::String
	u = strip(String(url))
	endswith(u, '/') || (u *= "/")
	startswith(u, "http://") || startswith(u, "https://") || (u = "http://" * u)
	return u * String(name) * ".GIF"
end

# One sheet, read and referenced. `rect` is its own (x0,x1,y0,y1) out of the table, which IS the
# georeferencing (bdnTile's third and best method), and the projection is the military one. The
# georeference is set in full — range, x, y, inc, registration and the CRS — never `range` alone.
function _cm_read_sheet(src::AbstractString, rect::NTuple{4,Float64})::GMTimage
	I = GMT.gmtread(String(src))
	isa(I, GMTimage) || error("$(basename(String(src))) did not come back as an image (got a $(typeof(I)))")
	ny, nx = size(I.image, 1), size(I.image, 2)
	(nx < 2 || ny < 2) && error("$(basename(String(src))) is empty")
	x0, x1, y0, y1 = rect
	# The sheet fills its cell exactly, and its pixels are cells: a pixel-registered grid of nx by ny
	# over [x0,x1] x [y0,y1].
	I.inc = [(x1 - x0) / nx, (y1 - y0) / ny]
	I.registration = 1
	I.x = collect(range(x0, x1, length = nx + 1))
	I.y = collect(range(y0, y1, length = ny + 1))
	I.range = [x0, x1, y0, y1, 0.0, 255.0]
	I.proj4 = _CM_PROJ4
	I.wkt = ""
	I.epsg = 0                               # I.layout is left exactly as the reader set it
	return I
end

# Several sheets -> one image. Each referenced sheet is written to a temp GeoTIFF and GDAL does the
# mosaic (buildvrt + translate over the bounding box), so no pixel pasting is written here. Sheets
# with a palette are expanded to RGB first: two sheets can carry two different colour tables, and a
# mosaic has one.
function _cm_mosaic(imgs::Vector{GMTimage}, bbox::NTuple{4,Float64})::GMTimage
	length(imgs) == 1 && return imgs[1]
	files = String[]
	for I in imgs
		J = (I.n_colors > 0) ? GMT.ind2rgb(I) : I
		f = tempname() * ".tif"
		GMT.gmtwrite(f, J)
		push!(files, replace(f, '\\' => '/'))
	end
	vrt = GMT.gdalbuildvrt(files)
	x0, x1, y0, y1 = bbox
	I = GMT.gdaltranslate(vrt, ["-projwin", string(x0), string(y1), string(x1), string(y0)])
	for f in files
		rm(f; force = true)
	end
	isa(I, GMTimage) || error("the mosaic came back as a $(typeof(I)), not an image")
	return I
end

# C callback, the "cm*" half of the PT picker's protocol (the other half is _on_lidar):
#   "cminit"                                   push the sheet grid (and the background's placement)
#   "cmgo;<names>;dir|web;<src>;W/E/S/N"       read the picked sheets and open them
function _on_cartas(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, raw::AbstractString)::Cvoid
	try
		parts = split(raw, ';'; limit = 5)          # a directory or URL may hold '/' but not ';'
		op = parts[1]
		if op == "cminit"
			bg = _cm_bg_extent()
			bg === nothing || ccall(_fn(:gmtvtk_lidar_set_bg), Cvoid,
			                        (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble),
			                        dlg, bg[1], bg[2], bg[3], bg[4])
			t = _cm_table()
			ccall(_fn(:gmtvtk_lidar_set_tiles), Cvoid, (Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint),
			      dlg, t.rects, join(t.names, '\n'), Cint(size(t.rects, 2)))
			return
		end
		(op == "cmgo") || error("unknown request '$op'")
		names = String.(filter(!isempty, split(String(parts[2]), ',')))
		isempty(names) && error("no sheet was picked")
		web = parts[3] == "web"
		src = String(parts[4])
		bb  = parse.(Float64, split(String(parts[5]), '/'))
		t = _cm_table()
		imgs = GMTimage[]
		missed = String[]
		for (k, nm) in enumerate(names)
			rect = get(t.bynames, nm, nothing)
			rect === nothing && (push!(missed, nm); continue)
			_lidar_status(dlg, "Reading $nm …  ($k/$(length(names)))")
			path = web ? _cm_sheet_url(src, nm) : _cm_sheet_path(src, nm)
			if isempty(path)
				push!(missed, nm)                    # bdnTile's "FILE ... NOT FOUND IN THERE"
				continue
			end
			try
				push!(imgs, _cm_read_sheet(path, rect))
			catch e
				push!(missed, nm)
				_viewer_log_error(scene, "Cartas Militares: $nm could not be read ($(sprint(showerror, e)))")
			end
		end
		isempty(missed) || _viewer_log_error(scene,
			"Cartas Militares: $(length(missed)) sheet(s) not found/read — " * join(first(missed, 12), ", "))
		isempty(imgs) && error("none of the picked sheets could be read from " *
		                       (web ? "that address" : "'$src'"))
		_lidar_status(dlg, "Assembling $(length(imgs)) sheet(s)…")
		I = _cm_mosaic(imgs, (bb[1], bb[2], bb[3], bb[4]))
		isempty(I.proj4) && (I.proj4 = _CM_PROJ4)
		name = length(imgs) == 1 ? "Carta $(names[1])" : "Cartas $(names[1])…$(names[end])"
		# A new raster in its own metric CRS: it goes in by the image door, in a fresh window (the
		# picker is not tied to the viewer it was opened from), and that door frames the axes to it.
		fig = iview_image_obj(I, name)
		try                                          # opened behind the picker otherwise
			h = _fig_handle(fig)
			h == C_NULL || ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), h)
		catch
		end
		_lidar_status(dlg, "")
	catch e
		_lidar_status(dlg, "")
		_viewer_log_error(scene, "Cartas Militares FAILED: $(sprint(showerror, e))")
		@warn "cartasmil: request failed" exception=(e,)
	end
	return
end

# Warm-up (the C++ menu action fires warmupTool("cartasmil") when the dialog opens): build the sheet
# table and the background transform, which is all the tool does before the user clicks anything.
function _cm_warm()
	_cm_table()
	yield()
	_cm_bg_extent()
	return nothing
end
