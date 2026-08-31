# lidarpt.jl — Tools > LIDAR2011 PT. Port of Mirone's `cartas_militares.m` in its second ("nikles")
# mode: the menu entry `Tools -> Misc Tools -> LIDAR2011 PT` calls `cartas_militares(handles,'nikles')`,
# whose two-argument branch hands the figure to the local `lidarPT` function and returns — so none of
# the 1:25000 "Cartas Militares" machinery runs. What is left is a mosaic builder over the LIDAR2011 PT
# survey: pick cells of the survey's 1600 x 1000 m tile matrix in the C++ picker (LidarPicker,
# 70_window.cpp), hit "Faz Mosaico", and the tiles inside the selection's bounding box are read,
# decimated to the chosen resolution and assembled into one grid, opened in a fresh viewer.
#
# The tile table is `data/lidarPT.dat` (read here with `gmtread`), the text twin of Mirone's
# `data/lidarPT.mat`: row 1 is the survey's global bounding box, rows 2..N one tile each, four columns
# `x0 x1 y0 y1` (metres, the survey's ETRS89/PT-TM06 frame) plus the tile name as trailing text. The
# cell index arithmetic (`ind = (row-1)*nColsQ + col`) is Mirone's verbatim, and is duplicated in the
# picker, so both halves address the same cell by the same number.
#
# Like every C->Julia callback (console / drop / basemap / tiles) the @cfunction and its registration
# are RUNTIME values, built lazily at the first window open (eventloop.jl `_ensure_callbacks`); the
# background-image path push is GMT-free and so is installed eagerly in __init__.

const _LIDAR_XINC = 1600.0        # LIDAR2011 tile size in metres (lidarPT.m x_inc / y_inc)
const _LIDAR_YINC = 1000.0
const _LIDAR_NODATA = -999.0      # readLidarTile: z(z == -999) = NaN
const _LIDAR_TILE_NX = 801        # a tile's native 2 m node counts, readLidarTile's reshape(z,801,501)
const _LIDAR_TILE_NY = 501

# ETRS89_Portugal_TM06, verbatim from push_lidarMosaico_CB's meta.srsWKT.
const _LIDAR_WKT = "PROJCS[\"ETRS89_Portugal_TM06\",GEOGCS[\"GCS_ETRS89\",DATUM[\"D_ETRS_1989\"," *
	"SPHEROID[\"GRS_1980\",6378137,298.257222101]],PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.017453292519943295]]," *
	"PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"latitude_of_origin\",39.66825833333333]," *
	"PARAMETER[\"central_meridian\",-8.133108333333334],PARAMETER[\"scale_factor\",1]," *
	"PARAMETER[\"false_easting\",0],PARAMETER[\"false_northing\",0],UNIT[\"Meter\",1]]"

_lidar_dat_path()   = joinpath(_PKGROOT, "data", "lidarPT.dat")
_lidar_image_path() = joinpath(_PKGROOT, "data", "PTimg_lidar.jpg")

# The parsed tile table, read once per session. `rects` is 4 x N (column-major, so each column is one
# row's x0,x1,y0,y1 — the layout gmtvtk_lidar_set_tiles expects), `names` the parallel names, `index`
# maps a cell's linear index to its tile name (Mirone's infoIND/infoNOME pair), and the bbox fields are
# row 1 (Mirone's infoBB).
struct LidarTable
	rects::Matrix{Float64}
	names::Vector{String}
	index::Dict{Int,String}
	x0::Float64
	x1::Float64
	y0::Float64
	y1::Float64
	nColsQ::Int
end

const _LIDAR_TABLE = Ref{Union{LidarTable,Nothing}}(nothing)

function _lidar_table()::LidarTable
	t = _LIDAR_TABLE[]
	t === nothing || return t
	path = _lidar_dat_path()
	isfile(path) || error("LIDAR2011 tile table not found: $path")
	D = GMT.gmtread(path)
	D isa Vector && (D = D[1])
	size(D.data, 2) < 4 && error("$path: expected 4 numeric columns (x0 x1 y0 y1), got $(size(D.data, 2))")
	nrows = size(D.data, 1)
	names = length(D.text) == nrows ? String.(D.text) : fill("", nrows)
	x0, x1, y0, y1 = D.data[1, 1], D.data[1, 2], D.data[1, 3], D.data[1, 4]
	nColsQ = round(Int, (x1 - x0) / _LIDAR_XINC) + 1        # lidarPT(): number of cells along X, +1 as in Mirone
	index = Dict{Int,String}()
	for k in 2:nrows                                        # row 1 is the global bbox, it is not a tile
		col = round(Int, (D.data[k, 1] - x0) / _LIDAR_XINC) + 1
		row = round(Int, (D.data[k, 3] - y0) / _LIDAR_YINC) + 1
		index[(row - 1) * nColsQ + col] = names[k]
	end
	t = LidarTable(permutedims(Float64.(D.data[:, 1:4])), names, index, x0, x1, y0, y1, nColsQ)
	_LIDAR_TABLE[] = t
	return t
end

# One-line progress text in the picker (Mirone's aguentabar). Best-effort.
_lidar_status(dlg::Ptr{Cvoid}, msg::AbstractString) = (try
	ccall(_fn(:gmtvtk_lidar_status), Cvoid, (Ptr{Cvoid}, Cstring), dlg, String(msg))
catch; end; nothing)

# The files that could hold a given tile, in the order push_lidarMosaico_CB tries them (LASzip first,
# then the XYZ and ESRI-ASCII exports of the same tile). All existing candidates are returned rather
# than just the first, so a corrupt/short file falls through to its twin instead of losing the cell.
function _lidar_tile_files(dir::AbstractString, nome::AbstractString)::Vector{String}
	out = String[]
	for ext in (".laz", ".xyz", ".asc")
		f = joinpath(dir, nome * "-mis_orto" * ext)
		isfile(f) && push!(out, f)
	end
	return out
end

# readLidarTile's LASzip branch. Mirone: `xyz = laszreader_mex(fname); z = single(xyz(3,:));
# z = reshape(z,801,501)'` — the tile IS a regular 2 m grid stored as a point cloud, x varying
# fastest, 801 columns x 501 rows, written north-first (hence the flipud that follows in Mirone).
# GMT.jl ships the same laszip reader (GMT.Laszip.lazread), so this is the MEX call one-for-one:
# `out="z"` returns just the elevation column.
function _read_laz_tile(fname::AbstractString)::Matrix{Float32}
	o = GMT.lazread(fname; out = "z", type = Float32)
	v = getproperty(o, Symbol(o.stored))
	z = v isa GMTdataset ? vec(v.data) : vec(v)
	length(z) == _LIDAR_TILE_NX * _LIDAR_TILE_NY ||
		error("$(basename(fname)): $(length(z)) points, expected $(_LIDAR_TILE_NX*_LIDAR_TILE_NY) " *
		      "($(_LIDAR_TILE_NX)x$(_LIDAR_TILE_NY) nodes)")
	m = permutedims(reshape(Float32.(z), _LIDAR_TILE_NX, _LIDAR_TILE_NY))   # reshape(z,801,501)'
	return reverse(m; dims = 1)                                             # flipud -> row 1 = south
end

# readLidarTile: read one 1600 x 1000 m tile, blank its no-data nodes and decimate it. `factor` is
# Mirone's decimation factor (1 keeps the native 2 m nodes, 5 -> 10 m, 10 -> 20 m, 25 -> 50 m).
# The raster (.xyz/.asc) branch needs NO flip: Mirone flips because MATLAB's gdalread hands it a
# top-down raster, while GMT/GDAL here already return the grid south-up. The LASzip branch reshapes
# a north-first point cloud, so it does flip (inside _read_laz_tile) exactly as Mirone does.
function _read_lidar_tile(fname::AbstractString, factor::Int)::Matrix{Float32}
	local z::Matrix{Float32}
	if lowercase(splitext(fname)[2]) in (".laz", ".las")
		z = _read_laz_tile(fname)
	else
		G  = GMT.gdalread(fname)
		zz = G isa GMTgrid ? G.z : G
		z  = zz isa Matrix{Float32} ? copy(zz) : Float32.(zz)
	end
	@inbounds for k in eachindex(z)
		z[k] == Float32(_LIDAR_NODATA) && (z[k] = NaN32)
	end
	factor == 1 && return z
	return z[1:factor:end, 1:factor:end]
end

# push_lidarMosaico_CB: fill a grid spanning the bounding box of the selected cells — inner cells that
# were NOT selected are read too, and cells with no tile (or no file on disk) simply stay NaN.
# `rMin..cMax` are 1-based data-matrix addresses of the picker's selection, `res` the target resolution
# in metres (2/10/20/50) and `dir` the directory holding the *-mis_orto.* files.
function _lidar_mosaic(dlg::Ptr{Cvoid}, rMin::Int, rMax::Int, cMin::Int, cMax::Int,
                       res::Float64, dir::AbstractString)
	t = _lidar_table()
	factor = max(1, round(Int, res / 2))                     # str2double(resolution)/2, as in Mirone
	nCol = 800 ÷ factor                                      # columns kept from a tile (1600/2/factor)
	nRow = 500 ÷ factor                                      # rows kept    (1000/2/factor)
	nx = (cMax - cMin + 1) * nCol + 1                        # +1: grid registration, tiles share an edge
	ny = (rMax - rMin + 1) * nRow + 1
	Z  = fill(NaN32, ny, nx)
	X0 = t.x0 + (cMin - 1) * _LIDAR_XINC
	Y0 = t.y0 + (rMin - 1) * _LIDAR_YINC
	X1 = t.x0 + cMax * _LIDAR_XINC
	Y1 = t.y0 + rMax * _LIDAR_YINC
	total = (rMax - rMin + 1) * (cMax - cMin + 1)
	got = 0
	missed = String[]                                        # named tiles whose file we could not read
	last = ""
	done = 0
	for m in 1:(rMax - rMin + 1), n in 1:(cMax - cMin + 1)
		done += 1
		ind  = (rMin + m - 2) * t.nColsQ + (cMin + n - 1)     # (row-1)*nColsQ + col, row = rMin+m-1
		nome = get(t.index, ind, "")
		isempty(nome) && continue
		cands = _lidar_tile_files(dir, nome)
		if isempty(cands)
			push!(missed, nome)
			continue
		end
		z = nothing; why = ""; used = ""
		for fname in cands                                   # first candidate that actually reads wins
			total > 9 && _lidar_status(dlg, "A ler os fiches…  $done/$total   ($(basename(fname)))")
			zz = try
				_read_lidar_tile(fname, factor)
			catch e
				why = sprint(showerror, e)
				continue
			end
			if size(zz) != (nRow + 1, nCol + 1)
				why = "$(size(zz, 1))x$(size(zz, 2)) nodes, expected $(nRow+1)x$(nCol+1)"
				continue
			end
			z = zz; used = fname
			break
		end
		if z === nothing
			push!(missed, nome * " [" * why * "]")
			continue
		end
		Z[(m-1)*nRow+1 : m*nRow+1, (n-1)*nCol+1 : n*nCol+1] = z
		got += 1
		last = used
	end
	got == 0 && error("Nickles de LIDAR grelhas no directório indicado ($dir). Bye Bye" *
	                  (isempty(missed) ? "" : "\nNão encontrei: " * join(first(missed, 12), ", ")))
	name = (total == 1 && !isempty(last)) ? basename(last) : "Mosaico LIDAR2011"
	G = GMT.mat2grid(Z; x = collect(range(X0, X1; length = nx)), y = collect(range(Y0, Y1; length = ny)))
	G.wkt = _LIDAR_WKT
	G.command = "LIDAR2011 PT mosaic: rows $rMin-$rMax, cols $cMin-$cMax at $(2*factor) m"
	return G, name, got, missed
end

# C callback. `params` = "op;..." — "init" (hand the tile table to the picker) or
# "go;rMin/rMax/cMin/cMax;res;dir" (build the mosaic). `dlg` is the live LidarPicker*, `scene` the
# viewer the tool was opened from (used for its Errors tab).
function _on_lidar(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		raw = unsafe_string(cparams)
		# The Cartas Militares picker is the SAME dialog class in its other mode, so it arrives through
		# this same callback; its ops are the "cm" ones and cartasmil.jl answers them.
		startswith(raw, "cm") && return _on_cartas(scene, dlg, raw)
		parts = split(raw, ';'; limit = 4)                      # a directory may contain '/' but not ';'
		op = parts[1]
		if op == "init"
			t = _lidar_table()
			ccall(_fn(:gmtvtk_lidar_set_tiles), Cvoid,
			      (Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint),
			      dlg, t.rects, join(t.names, '\n'), Cint(size(t.rects, 2)))
		elseif op == "go"
			rMin, rMax, cMin, cMax = parse.(Int, split(parts[2], '/'))
			res = parse(Float64, parts[3])
			dir = String(parts[4])
			G, name, got, missed = _lidar_mosaic(dlg, rMin, rMax, cMin, cMax, res, dir)
			isempty(missed) || _viewer_log_error(scene,
				"LIDAR2011: $(length(missed)) tile(s) skipped — " * join(first(missed, 12), ", "))
			_lidar_status(dlg, "$got quadrado(s) lido(s) — a abrir $(size(G.z, 2))x$(size(G.z, 1))…")
			fig = iview(G; title = name)
			# We are nested inside the picker's button handler, so the fresh window can open behind it.
			try
				h = _fig_handle(fig)
				h == C_NULL || ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), h)
			catch
			end
			_lidar_status(dlg, "")
		end
	catch e
		_lidar_status(dlg, "")
		_viewer_log_error(scene, "LIDAR2011 PT FAILED: $(sprint(showerror, e))")
		@warn "lidarpt: request failed" exception=(e,)
	end
	return
end

# Build the C-callable pointer. Lazy (first window) via _ensure_callbacks — the @cfunction is a thin
# invokelatest trampoline so it drags no GMT into compile.
function _register_lidar()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_lidar, s, d, c),
	                  Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_lidar_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("lidarpt", _lidar_warm)   # C++ fires this when the dialog opens (70_window.cpp)
	warm_register("cartasmil", _cm_warm)    # its twin, the Cartas Militares mode of the same picker
	return
end

# JIT warm-up while the user is picking cells: the table read and one gdalread are what "Faz Mosaico"
# then pays for. Best-effort — a failure here must never keep the dialog from opening.
function _lidar_warm()
	try _lidar_table() catch; end
	try GMT.mat2grid(rand(Float32, 8, 8); x = collect(0.0:7.0), y = collect(0.0:7.0)) catch; end
	return
end

# The picker's background image (data/PTimg_lidar.jpg). A static path push (no GMT inference) ->
# installed eagerly in __init__, before the first window builds.
function _install_lidar_assets()
	ccall(_fn(:gmtvtk_set_lidar_image), Cvoid, (Cstring,), _lidar_image_path())
	return
end
