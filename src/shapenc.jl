"""
shapenc.jl — Julia port of Mirone's `utils/shapenc.m`: write a point swarm / polygon / polyline
(optionally with an outer boundary + inner holes) into a "SHAPENC" netCDF4 container file — one
combined Mx2 `"lonLat..."`/`"XY..."` coordinate-pair variable per ensemble (2-D dims: point count x
2), plus a small zero-dimensional "container" variable that carries the ensemble's BoundingBox (and
optional name/tag/SegmentBoundaries) as attributes. (A `z`/`Z...` variable stays a separate 1-D
array for 3-D ensembles — only lon/lat, or X/Y, are paired up.)

This differs from Mirone's ORIGINAL `shapenc.m`, which always wrote lon and lat as two separate
1-D variables — the read side (`_shnc_read`/`_shnc_read_bounded`, via `_shnc_read_coordpair`) still
tries the combined array FIRST and falls back to the old separate pair, so both a genuine
Mirone-written file and any file this port wrote before the combined format existed keep reading
correctly; only the WRITE side changed.

`shapenc.m` reached the real netCDF library through Mirone's private `nc_funs` MEX wrapper. This
port has no MEX layer to reuse, so it talks to netCDF directly through **GDAL's multidimensional-
array C API** — GDAL already ships with GMT.jl (`GMT.libgdal`), so this needs no new dependency.
(GDAL's ordinary raster/vector helpers, incl. `GMT.gdalwrite`, don't fit this file's ad hoc
per-ensemble variable/attribute layout — the low-level MDArray API is the piece of GDAL that maps
onto it directly.) Verified round-trip (write via GDAL MDArray, read back with an independent
netCDF reader) before writing this port.

## What is ported (everything below is fully implemented, not a stub)

- **DATA / OUTER / INNER input forms** — a file name (read via `GMT.gmtread`, so ASCII tables,
  multi-segment files and OGR-readable shapefiles all work), a `GMTdataset`, a
  `Vector{<:GMTdataset}` (one ensemble per element — the `Vector` original called a "cell array"),
  a plain `Matrix{<:Real}`, or a `Vector{<:Matrix{<:Real}}`.
- **Point (2-D/3-D) and Polygon/PolyLine (2-D/3-D) ensembles** — geometry is auto-detected from a
  `GMTdataset`'s `.geom` field (`wkbPoint`/`wkbMultiPoint` → point, `wkbLineString`/
  `wkbMultiLineString` → polyline, `wkbPolygon`/`wkbMultiPolygon` → polygon) or, when that's
  unavailable (plain matrices), guessed from the column count the same way the M-file's own
  default does (2 or 3 columns ⇒ point) unless overridden with `point2D`/`polygon2D`/`polygon3D`/
  `polyline2D`/`polyline3D`. 3-D vs 2-D is read straight off the column count (2 vs 3).
- **`outer`/`inner` boundary polygons** — `outer` (a single polygon) and `inner` (one polygon, or
  several holes passed as a `Vector`) are written as combined `lonLatPolyOUT_k`/`XYPolyOUT_k` and
  `lonLatPolyIN_1_k`/`XYPolyIN_1_k` Mx2 variables next to the point ensemble they belong to — same
  coordinate-pair convention as the main ensemble coords above — functionally like the M-file's
  `'outer'`/`'inner'` name-value pairs. Only meaningful for — and only ever
  attached to — Point ensembles, same as the original. Limitation: only a SINGLE data ensemble may
  carry outer/inner polygons in this port (the M-file's per-swarm "cell of cells" case — several
  swarms, each with its own outer/inner set — is not implemented; passing multi-swarm `data`
  together with `outer`/`inner` raises an error rather than silently dropping the polygons).
- **`append`** — `append=true` re-opens an existing `fname` in GDAL update mode and adds the new
  ensemble(s) after whatever is already in the file (matching numbering, running global
  BoundingBox and `Number_of_main_ensembles` all carried forward), same as the M-file's
  `'append'` option. If `fname` doesn't exist yet, `append=true` just creates it (same fallback
  the M-file has).
- **`tag`** — either a `String` (written as the ensemble's container-variable `name` attribute) or
  an iterable of `(key, value)` pairs (each written as its own container-variable attribute), same
  as the M-file's `'tag'` option — extended here (the M-file only ever wrote string attributes)
  since real tag values are often numeric: `value` may be a `String` (written as a netCDF string
  attribute) OR a `Vector{Float64}` (written as a proper netCDF `double[]` attribute, same storage
  as `BoundingBox`) — pick whichever matches what the value actually is; a quoted space-separated
  number list like a magnetic-isochron pole (`"136.53 63.63 -3.951 16.37"`) should be
  `parse.(Float64, split(...))`-ed into the latter, not kept as opaque text. Only ever attached to
  the FIRST ensemble of a multi-swarm `data`, matching the original's documented restriction.
- **`ids`** — a `Vector{String}` (one entry per ensemble in `data`, not an M-file option) that
  replaces the auto-incrementing integer normally used as the ensemble's "kk" in every variable
  name with a caller-chosen identifier — AND drops the `Point_`/`Polygon_`/`PolyLine_` type prefix
  entirely, so the variable is named from the id alone (`lon` * id / `lat` * id, e.g. `lonc4a`, not
  `lonPolyLine_c4a`) — for isocs.jl, the isochron's own short file-stem id, so the netCDF VARIABLE
  ITSELF carries exactly that name, not just an attribute. Every id ever used in the file (across
  `append` calls too) is tracked in the global `Ensemble_IDs` attribute, which `_shnc_read` uses to
  enumerate ensembles by name instead of assuming a 1..N integer range; since the bare name alone
  can't encode Point/Polygon/PolyLine or 2-D/3-D anymore, a bare ensemble's kind/dimensionality is
  read back from the file's own global `SHAPENC_type` attribute instead.
- **`desc`** — global `Description` attribute (M-file's `'desc'`).
- **`version`** — global `File Version` attribute (M-file's `'version'`).
- **`geog`/`srs`** — `geog=true` (default) writes `lon`/`lat` coordinate variable names with
  degrees units and `spatial_ref="+proj=longlat"`; `geog=false` writes `X`/`Y` with meters units
  and `spatial_ref="+proj=xy"`. Pass `srs` to override `spatial_ref` with any PROJ4/WKT string
  regardless of `geog` (M-file's `'geog'`/`'srs'` options).
- **`multiseg`** — when `data` normalizes to several segments (a `Vector{<:GMTdataset}` or
  `Vector{<:Matrix}`) and `multiseg=true`, they are NaN-packed into ONE ensemble (row-separated by
  a NaN row, like a GMT multi-segment file) instead of becoming separate ensembles, and the packed
  row positions are recorded as the first ensemble's `SegmentBoundaries` attribute — the M-file's
  `cell2multiseg`/`'multiseg'` behaviour.
- **`f64`** — not an M-file option. Coordinate variables (`lon`/`lat`/`z`, PolyOUT/PolyIN) are
  written as netCDF `Float32` by default (halves the on-disk size of the bulk per-point payload);
  pass `f64=true` to write them as `Float64` instead. Global/container-variable attributes
  (`BoundingBox`, numeric `tag` values, ...) are always `Float64` regardless of `f64` — they're a
  handful of numbers per ensemble, not the bulk data this option is meant to control. `_shnc_read`
  always reads back into `Float64` `GMTdataset`s either way (GDAL widens `Float32` on read), so a
  file written with `f64=false` round-trips losslessly at `Float32` precision (~7 significant
  digits), not exactly bit-for-bit.

## NOT ported

- The Douglas-Peucker `dp` line-simplification option.
- Raw GMT-style binary (`-b`) input (`data` as a path to a headerless binary dump) — pass an
  already-read `Matrix`/`GMTdataset` instead.

## Examples

```julia
# Plain 2-D point swarm from a Matrix
shapenc("acores.nc", [lon lat]; desc="Small ensemble isolated from main dataset")

# 3-D point swarm (3 columns ⇒ auto-detected as PointZ) from a GMTdataset
D = GMT.mat2ds([lon lat depth])
shapenc("acores.nc", D)

# Point swarm + an outer boundary polygon
shapenc("acores.nc", swarm_xy; outer=poly_xy, desc="EMEPC bathymetry")

# Point swarm + one outer boundary + several inner holes
shapenc("acores.nc", swarm_xy; outer=poly_xy, inner=[hole1_xy, hole2_xy])

# Append a second, related swarm (with its own tag) to that same file
shapenc("acores.nc", swarm2_xy; append=true, tag="secondary cluster")

# Several unrelated file names, one call, each becomes its own ensemble
shapenc("many.nc", ["a.dat", "b.dat", "c.dat"])

# Several segments packed into ONE multi-segment ensemble (NaN-separated)
shapenc("track.nc", [seg1_xyz, seg2_xyz, seg3_xyz]; multiseg=true)

# Non-geographic (projected) data, explicit CRS
shapenc("survey.nc", xy_meters; geog=false, srs="+proj=utm +zone=29 +datum=WGS84")

# Force interpretation as a 2-D polyline instead of the column-count-guessed default
shapenc("track.nc", xy; polyline2D=true)
```
"""

# ---- thin wrappers over GDAL's multidimensional-array C API (GMT.libgdal, no new dependency) --
const _SHNC_H = Ptr{Cvoid}

_shnc_lasterr() = (m = ccall((:CPLGetLastErrorMsg, GMT.libgdal), Cstring, ()); m == C_NULL ? "" : unsafe_string(m))
function _shnc_ck(h::Ptr{Cvoid}, what::String)
	(h == C_NULL) && error("shapenc: GDAL failed to $what" * ((e = _shnc_lasterr()) == "" ? "" : ": $e"))
	h
end

function _shnc_csl(strs::Vector{String})
	ptr = Ptr{Ptr{UInt8}}(C_NULL)
	for s in strs
		ptr = ccall((:CSLAddString, GMT.libgdal), Ptr{Ptr{UInt8}}, (Ptr{Ptr{UInt8}}, Cstring), ptr, s)
	end
	ptr
end

function _shnc_create_multidim(fname::String)
	ccall((:GDALAllRegister, GMT.libgdal), Cvoid, ())
	drv = _shnc_ck(ccall((:GDALGetDriverByName, GMT.libgdal), _SHNC_H, (Cstring,), "netCDF"), "find the netCDF driver")
	ds = ccall((:GDALCreateMultiDimensional, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}),
	           drv, fname, _shnc_csl(["FORMAT=NC4"]), C_NULL)
	_shnc_ck(ds, "create $fname")
end

function _shnc_open_multidim_update(fname::String)
	ccall((:GDALAllRegister, GMT.libgdal), Cvoid, ())
	flags = UInt32(0x01 | 0x10 | 0x40)		# GDAL_OF_UPDATE | GDAL_OF_MULTIDIM_RASTER | GDAL_OF_VERBOSE_ERROR
	# Without an explicit allowed-drivers list, GDALOpenEx's driver probe leans on the file
	# EXTENSION -- proven live: reopening a real SHAPENC file for append/update fails with
	# "CreateDimension() not implemented" whenever it's saved under a non-".nc" extension (e.g.
	# ".dat", isocs.jl's grouped output), even though the exact same bytes reopen fine as ".nc".
	# Forcing the driver here fixes it regardless of what extension the caller chose.
	ds = ccall((:GDALOpenEx, GMT.libgdal), _SHNC_H, (Cstring, UInt32, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}),
	           fname, flags, _shnc_csl(["netCDF"]), C_NULL, C_NULL)
	_shnc_ck(ds, "reopen $fname for append")
end

_shnc_root(ds) = _shnc_ck(ccall((:GDALDatasetGetRootGroup, GMT.libgdal), _SHNC_H, (_SHNC_H,), ds), "get root group")

_shnc_create_dim(grp, name::String, n::Int) =
	_shnc_ck(ccall((:GDALGroupCreateDimension, GMT.libgdal), _SHNC_H,
	               (_SHNC_H, Cstring, Cstring, Cstring, UInt64, Ptr{Ptr{UInt8}}),
	               grp, name, "", "", UInt64(n), C_NULL), "create dimension $name")

_shnc_edt_f64() = _shnc_ck(ccall((:GDALExtendedDataTypeCreate, GMT.libgdal), _SHNC_H, (UInt32,), UInt32(7)), "create Float64 type")
_shnc_edt_f32() = _shnc_ck(ccall((:GDALExtendedDataTypeCreate, GMT.libgdal), _SHNC_H, (UInt32,), UInt32(6)), "create Float32 type")
_shnc_edt_i32() = _shnc_ck(ccall((:GDALExtendedDataTypeCreate, GMT.libgdal), _SHNC_H, (UInt32,), UInt32(5)), "create Int32 type")
_shnc_edt_str() = _shnc_ck(ccall((:GDALExtendedDataTypeCreateString, GMT.libgdal), _SHNC_H, (Csize_t,), Csize_t(0)), "create String type")

function _shnc_create_array(grp, name::String, dim, edt; deflate::Int=5)
	dimv  = (dim === nothing) ? Ptr{_SHNC_H}(C_NULL) : [dim]
	ndims = (dim === nothing) ? 0 : 1
	opts  = (dim === nothing || deflate <= 0) ? Ptr{Ptr{UInt8}}(C_NULL) : _shnc_csl(["COMPRESS=DEFLATE", "ZLEVEL=$(deflate)"])
	_shnc_ck(ccall((:GDALGroupCreateMDArray, GMT.libgdal), _SHNC_H,
	               (_SHNC_H, Cstring, Csize_t, Ptr{_SHNC_H}, _SHNC_H, Ptr{Ptr{UInt8}}),
	               grp, name, ndims, dimv, edt, opts), "create array $name")
end

function _shnc_array_write!(arr, edt, data::AbstractVector{T}) where T <: Union{Float64, Float32}
	n = length(data)
	start = UInt64[0]; cnt = UInt64[n]; step = Int64[1]; stride = Int64[1]
	r = ccall((:GDALMDArrayWrite, GMT.libgdal), Cint,
	          (_SHNC_H, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, _SHNC_H, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
	          arr, start, cnt, step, stride, edt, data, C_NULL, 0)
	(r == 0) && error("shapenc: failed writing array data" * ((e = _shnc_lasterr()) == "" ? "" : ": $e"))
end

# `owner` is a group or an array handle -- same entry points, Group->Array swapped. (ccall needs
# the (symbol, lib) pair to be literal, so the two owner kinds get separate methods, not a runtime
# symbol switch.)
function _shnc_attr_create(owner, isgroup::Bool, name::String, edt, n::Int=0)
	dimsz = (n == 0) ? Ptr{UInt64}(C_NULL) : UInt64[n]
	nd = (n == 0) ? 0 : 1
	h = isgroup ?
		ccall((:GDALGroupCreateAttribute, GMT.libgdal), _SHNC_H,
		      (_SHNC_H, Cstring, Csize_t, Ptr{UInt64}, _SHNC_H, Ptr{Ptr{UInt8}}), owner, name, nd, dimsz, edt, C_NULL) :
		ccall((:GDALMDArrayCreateAttribute, GMT.libgdal), _SHNC_H,
		      (_SHNC_H, Cstring, Csize_t, Ptr{UInt64}, _SHNC_H, Ptr{Ptr{UInt8}}), owner, name, nd, dimsz, edt, C_NULL)
	_shnc_ck(h, "create attribute $name")
end
function _shnc_attr_write_str!(attr, s::String)
	(ccall((:GDALAttributeWriteString, GMT.libgdal), Cint, (_SHNC_H, Cstring), attr, s) == 0) &&
		error("shapenc: failed writing string attribute")
end
function _shnc_attr_write_f64v!(attr, v::Vector{Float64})
	(ccall((:GDALAttributeWriteDoubleArray, GMT.libgdal), Cint, (_SHNC_H, Ptr{Cdouble}, Csize_t), attr, v, length(v)) == 0) &&
		error("shapenc: failed writing double[] attribute")
end
function _shnc_attr_write_i32v!(attr, v::Vector{Int32})
	(ccall((:GDALAttributeWriteIntArray, GMT.libgdal), Cint, (_SHNC_H, Ptr{Cint}, Csize_t), attr, v, length(v)) == 0) &&
		error("shapenc: failed writing int[] attribute")
end
function _shnc_attr_write_i32!(attr, v::Int)
	(ccall((:GDALAttributeWriteInt, GMT.libgdal), Cint, (_SHNC_H, Cint), attr, Int32(v)) == 0) &&
		error("shapenc: failed writing int attribute")
end

_shnc_release_attr(h)  = ccall((:GDALAttributeRelease, GMT.libgdal), Cvoid, (_SHNC_H,), h)
_shnc_release_array(h) = ccall((:GDALMDArrayRelease, GMT.libgdal), Cvoid, (_SHNC_H,), h)
_shnc_release_dim(h)   = ccall((:GDALDimensionRelease, GMT.libgdal), Cvoid, (_SHNC_H,), h)
_shnc_release_edt(h)   = ccall((:GDALExtendedDataTypeRelease, GMT.libgdal), Cvoid, (_SHNC_H,), h)

# Every array/dim/attribute handle GDAL hands back is a *local* reference that must be released
# once we're done with it -- the owning group keeps the real one. Skipping this (found live: every
# handle left open until the final GDALClose) leaves the netCDF4/HDF5 file unflushed/corrupt, so
# the combined set!-helpers below always create -> write -> release in one shot.
function _shnc_set_str_attr!(owner, isgroup::Bool, name::String, edt_str, value::String)
	a = _shnc_attr_create(owner, isgroup, name, edt_str)
	_shnc_attr_write_str!(a, value)
	_shnc_release_attr(a)
end
function _shnc_set_f64v_attr!(owner, isgroup::Bool, name::String, edt_f64, v::Vector{Float64})
	a = _shnc_attr_create(owner, isgroup, name, edt_f64, length(v))
	_shnc_attr_write_f64v!(a, v)
	_shnc_release_attr(a)
end
# One ensemble's `tag` (or `tags[k]`) applied to its container variable -- a String (-> "name"
# attribute) or an iterable of (key,value) pairs, each value a String or Vector{Float64}. The ONE
# place tag values get written, whether from `tag` (first-ensemble-only) or `tags` (per-ensemble).
function _shnc_write_tag_attrs!(cvar, edt_f64, edt_str, tagval)
	if tagval isa String
		_shnc_set_str_attr!(cvar, false, "name", edt_str, tagval)
	else
		for (pk, pv) in tagval
			if pv isa String
				_shnc_set_str_attr!(cvar, false, pk, edt_str, pv)
			elseif pv isa Vector{Float64}
				_shnc_set_f64v_attr!(cvar, false, pk, edt_f64, pv)
			else
				error("shapenc: tag value for \"$pk\" must be a String or a numeric vector, got $(typeof(pv))")
			end
		end
	end
end

function _shnc_set_i32_attr!(owner, isgroup::Bool, name::String, edt_i32, v::Int)
	a = _shnc_attr_create(owner, isgroup, name, edt_i32)
	_shnc_attr_write_i32!(a, v)
	_shnc_release_attr(a)
end

# Below this many elements, DEFLATE makes the file BIGGER, not smaller -- HDF5's chunked+filtered
# storage layout (chunk B-tree/index, filter pipeline message, gzip header) costs ~1-2 KB of fixed
# structural overhead per array, which swamps any saving on a small payload. Proven live: a 100-
# double ramp (800 raw bytes, near-maximally compressible) came out at 8353 bytes WITH
# COMPRESS=DEFLATE vs 6944 bytes with no compression option at all -- most isochron ensembles
# average ~100 points, so requesting compression unconditionally (the original default) was
# actively bloating every real-world SHAPENC file, not shrinking it.
const _SHNC_COMPRESS_MIN_ELEMS = 512

# Create a 1-D array on `dim`, write `data`, tag long_name/units, then release the array handle.
# `edt_val` must be the GDAL extended-type handle matching `data`'s own eltype (Float64<->edt_f64,
# Float32<->edt_f32) -- picked by the caller from the `f64::Bool` kwarg on `shapenc`.
function _shnc_write_coord_var!(root, name::String, dim, edt_val, edt_str, data::AbstractVector{T}, long_name::String, units::String) where T <: Union{Float64, Float32}
	deflate = length(data) >= _SHNC_COMPRESS_MIN_ELEMS ? 5 : 0
	arr = _shnc_create_array(root, name, dim, edt_val; deflate)
	_shnc_array_write!(arr, edt_val, data)
	_shnc_set_str_attr!(arr, false, "long_name", edt_str, long_name)
	_shnc_set_str_attr!(arr, false, "units", edt_str, units)
	_shnc_release_array(arr)
end

# 2-D sibling of _shnc_create_array/_shnc_array_write! -- used ONLY for the combined lon/lat (or
# X/Y) coordinate-pair array below (`dims` is always `[dim_n, dim_2]`, never more).
function _shnc_create_array2(grp, name::String, dims::Vector, edt; deflate::Int=5)
	opts = deflate <= 0 ? Ptr{Ptr{UInt8}}(C_NULL) : _shnc_csl(["COMPRESS=DEFLATE", "ZLEVEL=$(deflate)"])
	_shnc_ck(ccall((:GDALGroupCreateMDArray, GMT.libgdal), _SHNC_H,
	               (_SHNC_H, Cstring, Csize_t, Ptr{_SHNC_H}, _SHNC_H, Ptr{Ptr{UInt8}}),
	               grp, name, Csize_t(2), dims, edt, opts), "create array $name")
end
function _shnc_array_write2!(arr, edt, data::AbstractVector{T}, n::Int) where T <: Union{Float64, Float32}
	# bufferStride is in ELEMENTS of the linear `data` buffer, not array-index steps -- for a
	# row-major (dim0=n, dim1=2) interleaved buffer, incrementing the n-index skips a whole
	# 2-element row (stride 2) while incrementing the xy-index skips 1 element (stride 1).
	# [1,1] here (the naive guess) silently reads/writes the wrong elements, proven live: rows
	# past the first came back as leftover `undef` garbage (denormals/NaN), not zeros or an error.
	start = UInt64[0, 0]; cnt = UInt64[n, 2]; step = Int64[1, 1]; stride = Int64[2, 1]
	r = ccall((:GDALMDArrayWrite, GMT.libgdal), Cint,
	          (_SHNC_H, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, _SHNC_H, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
	          arr, start, cnt, step, stride, edt, data, C_NULL, 0)
	(r == 0) && error("shapenc: failed writing array data" * ((e = _shnc_lasterr()) == "" ? "" : ": $e"))
end

# Combined lon/lat ("lonLat...") or X/Y coordinate-PAIR array: ONE Mx2 array instead of two
# separate M-length 1-D arrays -- the single write path every lon/lat pair in this file goes
# through (main ensemble coords, PolyOUT, PolyIN alike), never reimplemented per call site.
#
# `dim2` (the always-size-2 second dimension) is a handle the CALLER creates once and owns --
# `shapenc()` creates it ONCE per call and reuses it for every ensemble/PolyOUT/PolyIN written by
# that call, released once at the end. A version of this that created (or worse, group-searched
# for) a fresh dim2 on every single call -- i.e. once per array -- was measured live: reused
# per-call it stays as fast/small as the original two-separate-1D-arrays format; searched or
# recreated per array it either duplicates a size-2 dim per array (1368x on the Seton isochron
# file, ~51KB of pure waste) or, if made a truly FILE-WIDE shared dim looked up across 1368
# separate file-reopen sessions, hits HDF5's per-attach dimension-scale reference-list rewrite cost
# and gets WORSE on both axes (150s->236s, 15.8MB->23.2MB). Per-CALL reuse (this version) avoids
# both failure modes because a single call means a single continuous GDAL/HDF5 session -- no
# reopen-driven reattach cost, and no wasteful one-off dims either.
function _shnc_write_coordpair_var!(root, name::String, dim_n, dim2, edt_val, edt_str, x::AbstractVector{T}, y::AbstractVector{T}, long_name::NTuple{2,String}, units::NTuple{2,String}) where T <: Union{Float64, Float32}
	n = length(x)
	deflate = n >= _SHNC_COMPRESS_MIN_ELEMS ? 5 : 0
	arr = _shnc_create_array2(root, name, [dim_n, dim2], edt_val; deflate)
	buf = Vector{T}(undef, 2n)
	@inbounds for i in 1:n
		buf[2i - 1] = x[i]; buf[2i] = y[i]
	end
	_shnc_array_write2!(arr, edt_val, buf, n)
	_shnc_set_str_attr!(arr, false, "long_name", edt_str, long_name[1] * "/" * long_name[2])
	_shnc_set_str_attr!(arr, false, "units", edt_str, units[1])
	_shnc_release_array(arr)
end

# "lonLat"/"XY" -- the combined coordinate-pair variable's name stem, geographic vs projected.
_shnc_coordpair_name(geog::Bool)::String = geog ? "lonLat" : "XY"

function _shnc_attr_read_i32(grp, name::String)::Union{Nothing, Int}
	attr = ccall((:GDALGroupGetAttribute, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring), grp, name)
	(attr == C_NULL) && return nothing
	v = ccall((:GDALAttributeReadAsInt, GMT.libgdal), Cint, (_SHNC_H,), attr)
	ccall((:GDALAttributeRelease, GMT.libgdal), Cvoid, (_SHNC_H,), attr)
	Int(v)
end

function _shnc_attr_read_f64v(grp, name::String)::Union{Nothing, Vector{Float64}}
	attr = ccall((:GDALGroupGetAttribute, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring), grp, name)
	(attr == C_NULL) && return nothing
	pn = Ref{Csize_t}(0)
	p = ccall((:GDALAttributeReadAsDoubleArray, GMT.libgdal), Ptr{Cdouble}, (_SHNC_H, Ptr{Csize_t}), attr, pn)
	v = (p == C_NULL) ? Float64[] : copy(unsafe_wrap(Array, p, Int(pn[])))
	ccall((:GDALAttributeRelease, GMT.libgdal), Cvoid, (_SHNC_H,), attr)
	v
end

function _shnc_attr_read_str(grp, name::String)::Union{Nothing, String}
	attr = ccall((:GDALGroupGetAttribute, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring), grp, name)
	(attr == C_NULL) && return nothing
	s = _shnc_attr_str(attr)
	ccall((:GDALAttributeRelease, GMT.libgdal), Cvoid, (_SHNC_H,), attr)
	s
end

# ---- DATA/OUTER/INNER normalization -------------------------------------------------------
# Reduce any accepted input form to a Vector of Float64 Mx2/Mx3 matrices ("ensembles"/swarms)
# plus a GDAL/OGR geometry code (0 = unknown) taken from the first ensemble when available.
_shnc_normalize(data::String) = _shnc_normalize(GMT.gmtread(data))

_shnc_normalize(data::GMT.GMTdataset) = ([Float64.(data.data)], Int(data.geom))
function _shnc_normalize(data::Vector{<:GMT.GMTdataset})
	isempty(data) && error("shapenc: empty dataset")
	([Float64.(d.data) for d in data], Int(first(data).geom))
end
_shnc_normalize(data::Matrix{<:Real}) = ([Float64.(data)], 0)
function _shnc_normalize(data::Vector{<:Matrix{<:Real}})
	isempty(data) && error("shapenc: empty data")
	([Float64.(m) for m in data], 0)
end
# Several file names in one call -- each expands to its own ensemble(s) (a file that is itself
# multi-segment/a shapefile contributes more than one), all concatenated in argument order.
function _shnc_normalize(data::Vector{String})
	isempty(data) && error("shapenc: empty file list")
	mats = Matrix{Float64}[]
	geom = 0
	for (i, fn) in enumerate(data)
		m, g = _shnc_normalize(fn)
		append!(mats, m)
		(i == 1) && (geom = g)
	end
	(mats, geom)
end

# NaN-pack several segments into ONE ensemble (port of Mirone's cell2multiseg). Returns the
# packed Mx2/Mx3 matrix and the 1-based row positions of the NaN separators.
function _shnc_cell2multiseg(mats::Vector{Matrix{Float64}})
	ncol = size(mats[1], 2)
	any(size(m, 2) != ncol for m in mats) && error("shapenc: multiseg segments have inconsistent column counts")
	nseg = length(mats)
	ntot = sum(size(m, 1) for m in mats) + nseg - 1
	out = fill(NaN, ntot, ncol)
	pos = Int[]
	row = 1
	for (k, m) in enumerate(mats)
		n = size(m, 1)
		out[row:row+n-1, :] .= m
		row += n
		if (k < nseg)
			push!(pos, row)			# 1-based row that holds the NaN separator
			row += 1
		end
	end
	(out, pos)
end

function _nanextrema(v::AbstractVector{Float64})
	mn, mx = Inf, -Inf
	for x in v
		isnan(x) && continue
		(x < mn) && (mn = x)
		(x > mx) && (mx = x)
	end
	(mn, mx)
end

_shnc_geomkind(geom::Int, ncol::Int) =
	geom in (1, 4) ? :point : geom in (2, 5) ? :polyline : geom in (3, 6) ? :polygon :
	(ncol in (2, 3)) ? :point : error("shapenc: cannot determine geometry type (got $ncol data columns)")

"""
    shapenc(fname::String, data; outer=nothing, inner=nothing, geog::Bool=true, srs::String="",
            desc::String="", version::String="", append::Bool=false, tag=nothing, tags=nothing,
            multiseg::Bool=false, point2D::Bool=false, polygon2D::Bool=false,
            polygon3D::Bool=false, polyline2D::Bool=false, polyline3D::Bool=false, f64::Bool=false)

Write `data` (a point swarm / polygon / polyline ensemble, or several of them) into `fname` as a
SHAPENC netCDF4 file — a Julia port of Mirone's `utils/shapenc.m`. See this file's module
docstring for the full write-up of what each option does and its exact scope; short form:

- `data`/`outer`/`inner`: file name, `GMTdataset`, `Vector{<:GMTdataset}`, `Matrix{<:Real}`, or
  `Vector{<:Matrix{<:Real}}`. `outer`/`inner` only apply to a single Point ensemble.
- `geog`/`srs`: geographic (`lon`/`lat`, default) vs projected (`X`/`Y`) coordinate names/units;
  `srs` overrides the written `spatial_ref` PROJ4/WKT string outright.
- `desc`/`version`: global `Description`/`File Version` attributes.
- `append`: add ensemble(s) to an existing SHAPENC file instead of overwriting it.
- `tag`: a `String` (ensemble `name` attribute) or `(key,value)` pairs (per-attribute), first
  ensemble only.
- `tags`: like `tag`, but a `Vector` with one entry PER ensemble (`length(tags) == length(data)`),
  applied to every ensemble instead of only the first — for a single batched call writing many
  independently-named ensembles at once (e.g. isocs.jl's shapefile batch, one `name`/`fromage` tag
  per feature). Takes priority over `tag` when both are given.
- `multiseg`: NaN-pack several segments into ONE ensemble instead of separate ones.
- `point2D`/`polygon2D`/`polygon3D`/`polyline2D`/`polyline3D`: force the geometry kind instead of
  auto-detecting it from a `GMTdataset`'s `.geom` or the column count.
- `f64`: coordinate variables (lon/lat/z, PolyOUT/PolyIN) are written as netCDF `Float32` by
  default; pass `f64=true` to write them as `Float64` instead. Global/ensemble attributes
  (`BoundingBox`, `tag` numeric values, ...) are always `Float64` regardless of `f64`.
- NOT ported: Douglas-Peucker `dp` simplification, raw GMT `-b` binary input, shapefile `maxpoly`
  chunking (see module docstring for why).

### Examples
```julia
shapenc("acores.nc", [lon lat]; desc="Small ensemble isolated from main dataset")
shapenc("acores.nc", GMT.mat2ds([lon lat depth]))                      # 3-D, auto-detected
shapenc("acores.nc", swarm_xy; outer=poly_xy, inner=[hole1_xy, hole2_xy])
shapenc("acores.nc", swarm2_xy; append=true, tag="secondary cluster")
shapenc("many.nc", ["a.dat", "b.dat", "c.dat"])                        # 3 ensembles, 1 call
shapenc("track.nc", [seg1_xyz, seg2_xyz, seg3_xyz]; multiseg=true)     # 1 NaN-packed ensemble
shapenc("survey.nc", xy_meters; geog=false, srs="+proj=utm +zone=29 +datum=WGS84")
shapenc("track.nc", xy; polyline2D=true)                               # force geometry kind
```
"""
function shapenc(fname::String, data; outer=nothing, inner=nothing, geog::Bool=true, srs::String="",
                  desc::String="", version::String="", append::Bool=false, tag=nothing, tags=nothing,
                  multiseg::Bool=false, point2D::Bool=false, polygon2D::Bool=false,
                  polygon3D::Bool=false, polyline2D::Bool=false, polyline3D::Bool=false,
                  ids::Union{Nothing, Vector{String}}=nothing, f64::Bool=false)

	mats, geom = _shnc_normalize(data)
	multiseg_pos = Int[]
	if multiseg && length(mats) > 1
		packed, multiseg_pos = _shnc_cell2multiseg(mats)
		mats = [packed]
	end
	n_swarms = length(mats)
	(n_swarms > 1 && (outer !== nothing || inner !== nothing)) &&
		error("shapenc: multi-swarm DATA together with OUTER/INNER polygons is not supported by this first-cut port")
	(ids !== nothing && length(ids) != n_swarms) &&
		error("shapenc: ids has $(length(ids)) entries but DATA has $n_swarms ensemble(s)")
	(tags !== nothing && length(tags) != n_swarms) &&
		error("shapenc: tags has $(length(tags)) entries but DATA has $n_swarms ensemble(s)")

	ncol = size(mats[1], 2)
	(ncol != 2 && ncol != 3) && error("shapenc: DATA must have 2 or 3 columns (got $ncol)")
	is_3D = (ncol == 3)

	kind = point2D || polygon2D || polygon3D || polyline2D || polyline3D ?
		(point2D ? :point : polygon2D || polygon3D ? :polygon : :polyline) : _shnc_geomkind(geom, ncol)

	# _shnc_normalize always returns a Vector of ensembles, even for a single GMTdataset/Matrix/file
	# name (a length-1 Vector) -- so OUTER takes the first (only) ensemble, and INNER's holes are
	# exactly that Vector, whatever form the caller passed it in.
	outer_mat = (outer === nothing) ? nothing : first(first(_shnc_normalize(outer)))
	inner_mats = (inner === nothing) ? Matrix{Float64}[] : first(_shnc_normalize(inner))

	Xname, Yname = geog ? ("lon", "lat") : ("X", "Y")
	long_name = geog ? ("Longitude", "Latitude") : ("X", "Y")
	units = geog ? ("degrees_east", "degrees_north") : ("meters", "meters")
	spatial_ref = !isempty(srs) ? srs : (geog ? "+proj=longlat" : "+proj=xy")
	coordpair_name = _shnc_coordpair_name(geog)

	type_attr = kind == :point ? (is_3D ? "PointZ" : "Point") :
	            kind == :polygon ? (is_3D ? "PolygonZ" : "Polygon") : (is_3D ? "PolyLineZ" : "PolyLine")
	# A caller-chosen `ids` identifier IS the variable name -- no "PolyLine_"/"Point_"/... prefix
	# glued in front of it (user explicit: "I DON'T WANT any Polyline_xxx in variable names").
	# Without `ids`, the type prefix + auto-incrementing integer is still how a plain shapenc file
	# tells ensembles apart, unchanged.
	prefix = ids !== nothing ? "" :
	         kind == :point ? (is_3D ? "PointZ_" : "Point_") :
	         kind == :polygon ? (is_3D ? "PolygonZ_" : "Polygon_") : (is_3D ? "PolyLineZ_" : "PolyLine_")

	is_new = append ? !isfile(fname) : true
	ds = is_new ? _shnc_create_multidim(fname) : _shnc_open_multidim_update(fname)
	root = _shnc_root(ds)

	ultimo = is_new ? 0 : something(_shnc_attr_read_i32(root, "Number_of_main_ensembles"), 0)
	global_bb = is_new ? [Inf, -Inf, Inf, -Inf] : something(_shnc_attr_read_f64v(root, "BoundingBox"), [Inf, -Inf, Inf, -Inf])
	# Every ensemble's own "kk" (plain integer counter OR a custom `ids` string) is recorded here so
	# `_shnc_read` can enumerate ensembles by NAME instead of assuming they're numbered 1..N --
	# required once `ids` lets a caller (isocs.jl) use a non-numeric identifier as the actual
	# variable name, per user requirement: the netCDF variable name itself must carry the isochron's
	# own short id, not just an attribute.
	id_list = is_new ? String[] : split(something(_shnc_attr_read_str(root, "Ensemble_IDs"), ""), ";"; keepempty=false) .|> String

	edt_f64, edt_i32, edt_str = _shnc_edt_f64(), _shnc_edt_i32(), _shnc_edt_str()
	edt_coord = f64 ? edt_f64 : _shnc_edt_f32()
	# Coordinate arrays (lon/lat/z, PolyOUT/PolyIN) go out as Float32 by default; BoundingBox/tag
	# attributes stay Float64 always -- they're small (a handful of numbers per ensemble), not the
	# bulk per-point payload `f64` is meant to control.
	_shnc_coord(v::AbstractVector{Float64}) = f64 ? v : Float32.(v)

	if is_new
		_shnc_set_str_attr!(root, true, "title", edt_str, "SHAPENC: A netCDF extended storage version of some types of shapefiles")
		!isempty(desc) && _shnc_set_str_attr!(root, true, "Description", edt_str, desc)
		_shnc_set_str_attr!(root, true, "version", edt_str, "1.0")
		_shnc_set_str_attr!(root, true, "SHAPENC_type", edt_str, type_attr)
		_shnc_set_str_attr!(root, true, "spatial_ref", edt_str, spatial_ref)
	elseif !isempty(desc)
		_shnc_set_str_attr!(root, true, "Description", edt_str, desc)
	end
	!isempty(version) && _shnc_set_str_attr!(root, true, "File Version", edt_str, version)

	# ONE shared "xy" dim for every ensemble/PolyOUT/PolyIN this CALL writes (see
	# _shnc_write_coordpair_var!'s docstring comment for why per-call, not per-array or
	# file-wide-across-calls). Named uniquely per call (via `ultimo`, the ensemble count already in
	# the file before this call) so appending into a file that already has ensembles -- possibly
	# from an earlier call's own "xy_<n>" -- never collides.
	dim2 = _shnc_create_dim(root, "xy_$(ultimo+1)", 2)

	for (k, m) in enumerate(mats)
		kk = ids === nothing ? k + ultimo : ids[k]
		push!(id_list, string(kk))
		n_pts = size(m, 1)
		x, y = view(m, :, 1), view(m, :, 2)
		xmn, xmx = _nanextrema(x)
		ymn, ymx = _nanextrema(y)
		bb = is_3D ? Float64[xmn, xmx, ymn, ymx, 0.0, 0.0] : Float64[xmn, xmx, ymn, ymx]

		dim = _shnc_create_dim(root, "dimpts_$kk", n_pts)
		_shnc_write_coordpair_var!(root, "$(coordpair_name)$(prefix)$kk", dim, dim2, edt_coord, edt_str, _shnc_coord(x), _shnc_coord(y), long_name, units)
		if is_3D
			z = view(m, :, 3)
			bb[5], bb[6] = _nanextrema(z)
			zname = (prefix == "" || kind == :point) ? "z_$kk" : "Z$(prefix)$kk"
			_shnc_write_coord_var!(root, zname, dim, edt_coord, edt_str, _shnc_coord(z), "z", "meters")
		end
		_shnc_release_dim(dim)

		cvar = _shnc_create_array(root, "$(prefix)$kk", nothing, edt_f64)
		_shnc_set_f64v_attr!(cvar, false, "BoundingBox", edt_f64, bb)
		(k == 1 && !isempty(multiseg_pos)) &&
			_shnc_set_f64v_attr!(cvar, false, "SegmentBoundaries", edt_f64, Float64.(multiseg_pos))
		# `tags` (one entry per ensemble) takes priority when given; otherwise `tag` applies to the
		# first ensemble only, same restriction the M-file's own 'tag' option always had.
		tagval = tags !== nothing ? tags[k] : (k == 1 ? tag : nothing)
		tagval === nothing || _shnc_write_tag_attrs!(cvar, edt_f64, edt_str, tagval)
		_shnc_release_array(cvar)

		global_bb[1] = min(global_bb[1], bb[1]); global_bb[2] = max(global_bb[2], bb[2])
		global_bb[3] = min(global_bb[3], bb[3]); global_bb[4] = max(global_bb[4], bb[4])

		# Outer/inner boundary polygons -- only meaningful (and only ever passed) for point ensembles.
		if kind == :point && outer_mat !== nothing
			dimo = _shnc_create_dim(root, "dimpolyOUT_$kk", size(outer_mat, 1))
			_shnc_write_coordpair_var!(root, "$(coordpair_name)PolyOUT_$kk", dimo, dim2, edt_coord, edt_str, _shnc_coord(view(outer_mat, :, 1)), _shnc_coord(view(outer_mat, :, 2)), long_name, units)
			_shnc_release_dim(dimo)
		end
		if kind == :point && !isempty(inner_mats)
			for (j, hole) in enumerate(inner_mats)
				jj = j + ultimo
				dimi = _shnc_create_dim(root, "dimpolyIN_$(ultimo+1)_$jj", size(hole, 1))
				_shnc_write_coordpair_var!(root, "$(coordpair_name)PolyIN_$(ultimo+1)_$jj", dimi, dim2, edt_coord, edt_str, _shnc_coord(view(hole, :, 1)), _shnc_coord(view(hole, :, 2)), long_name, units)
				_shnc_release_dim(dimi)
			end
		end
	end
	_shnc_release_dim(dim2)

	_shnc_set_f64v_attr!(root, true, "BoundingBox", edt_f64, global_bb)
	_shnc_set_i32_attr!(root, true, "Number_of_main_ensembles", edt_i32, ultimo + n_swarms)
	_shnc_set_str_attr!(root, true, "Ensemble_IDs", edt_str, join(id_list, ";"))

	_shnc_release_edt(edt_f64); _shnc_release_edt(edt_i32); _shnc_release_edt(edt_str)
	ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (_SHNC_H,), root)
	(ccall((:GDALClose, GMT.libgdal), Cint, (_SHNC_H,), ds) != 0) && error("shapenc: GDAL failed to close $fname")
	fname
end

# ============================================================================================
# ---- READ side: recognize a SHAPENC .nc file and load it back as vector data (GMTdataset) ----
# Lets a SHAPENC file drop/open through the SAME vector-overlay pipeline any other multi-segment
# file (a shapefile, a multi-segment ASCII table) already uses -- `_drop_into(scene, ::Vector{<:GMTdataset}, ...)`
# in drop.jl -- never a separate/parallel display path (SACRED_LAW.md).

function _shnc_open_multidim_read(fname::String)
	ccall((:GDALAllRegister, GMT.libgdal), Cvoid, ())
	flags = UInt32(0x10 | 0x40)		# GDAL_OF_MULTIDIM_RASTER | GDAL_OF_VERBOSE_ERROR (read-only)
	# Force the driver (see _shnc_open_multidim_update for why) -- a SHAPENC file need not have a
	# ".nc" extension (isocs.jl's grouped output uses ".dat"), and GDAL's extension-based driver
	# probe is unreliable for those.
	ds = ccall((:GDALOpenEx, GMT.libgdal), _SHNC_H, (Cstring, UInt32, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}),
	           fname, flags, _shnc_csl(["netCDF"]), C_NULL, C_NULL)
	_shnc_ck(ds, "open $fname for reading")
end

_shnc_group_open_array(grp, name::String) =
	ccall((:GDALGroupOpenMDArray, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring, Ptr{Ptr{UInt8}}), grp, name, C_NULL)

_shnc_group_attr(grp, name::String) = ccall((:GDALGroupGetAttribute, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring), grp, name)
_shnc_array_attr(arr, name::String) = ccall((:GDALMDArrayGetAttribute, GMT.libgdal), _SHNC_H, (_SHNC_H, Cstring), arr, name)

function _shnc_attr_str(attr)::String
	p = ccall((:GDALAttributeReadAsString, GMT.libgdal), Cstring, (_SHNC_H,), attr)
	p == C_NULL ? "" : unsafe_string(p)
end
function _shnc_attr_f64v(attr)::Vector{Float64}
	pn = Ref{Csize_t}(0)
	p = ccall((:GDALAttributeReadAsDoubleArray, GMT.libgdal), Ptr{Cdouble}, (_SHNC_H, Ptr{Csize_t}), attr, pn)
	p == C_NULL ? Float64[] : copy(unsafe_wrap(Array, p, Int(pn[])))
end
# GEDTC_STRING=1 (data), GEDTC_NUMERIC=0 -- which reader (_shnc_attr_str vs _shnc_attr_f64v) applies.
function _shnc_attr_is_string(attr)::Bool
	edt = ccall((:GDALAttributeGetDataType, GMT.libgdal), _SHNC_H, (_SHNC_H,), attr)
	cls = ccall((:GDALExtendedDataTypeGetClass, GMT.libgdal), UInt32, (_SHNC_H,), edt)
	_shnc_release_edt(edt)
	cls == 1
end

# Every attribute NAME on an array owner (BoundingBox, name, and whatever `tag` keys were written --
# FIN/STG0/STG3_5c-6/... are NOT known ahead of time, so they must be enumerated, not guessed).
function _shnc_array_attr_names(arr)::Vector{String}
	pn = Ref{Csize_t}(0)
	p = ccall((:GDALMDArrayGetAttributes, GMT.libgdal), Ptr{_SHNC_H}, (_SHNC_H, Ptr{Csize_t}, Ptr{Ptr{UInt8}}), arr, pn, C_NULL)
	n = Int(pn[])
	n == 0 && return String[]
	names = String[]
	for h in unsafe_wrap(Array, p, n)
		nm = ccall((:GDALAttributeGetName, GMT.libgdal), Cstring, (_SHNC_H,), h)
		push!(names, nm == C_NULL ? "" : unsafe_string(nm))
		_shnc_release_attr(h)
	end
	names
end

_shnc_array_count(arr)::Int = Int(ccall((:GDALMDArrayGetTotalElementsCount, GMT.libgdal), UInt64, (_SHNC_H,), arr))

function _shnc_array_read_f64(arr, edt_f64, n::Int)::Vector{Float64}
	out = Vector{Float64}(undef, n)
	start = UInt64[0]; cnt = UInt64[n]; step = Int64[1]; stride = Int64[1]
	r = ccall((:GDALMDArrayRead, GMT.libgdal), Cint,
	          (_SHNC_H, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, _SHNC_H, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
	          arr, start, cnt, step, stride, edt_f64, out, C_NULL, 0)
	(r == 0) && error("shapenc: failed reading array data" * ((e = _shnc_lasterr()) == "" ? "" : ": $e"))
	out
end

# Read a combined Mx2 coordinate-pair array (the "lonLat"/"XY" `_shnc_write_coordpair_var!` writes)
# back into separate x,y Vectors -- the read-side counterpart, kept next to the 1-D reader above.
function _shnc_array_read_pairs_f64(arr, edt_f64, n::Int)::Tuple{Vector{Float64}, Vector{Float64}}
	buf = Vector{Float64}(undef, 2n)
	# Same row-major bufferStride fix as _shnc_array_write2! (dim0=n -> stride 2, dim1=xy -> stride 1).
	start = UInt64[0, 0]; cnt = UInt64[n, 2]; step = Int64[1, 1]; stride = Int64[2, 1]
	r = ccall((:GDALMDArrayRead, GMT.libgdal), Cint,
	          (_SHNC_H, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, _SHNC_H, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
	          arr, start, cnt, step, stride, edt_f64, buf, C_NULL, 0)
	(r == 0) && error("shapenc: failed reading array data" * ((e = _shnc_lasterr()) == "" ? "" : ": $e"))
	(buf[1:2:end], buf[2:2:end])
end

# Open+read a lon/lat (or X/Y) coordinate pair, trying the combined "lonLat"/"XY" Mx2 array
# (current `shapenc` write format) FIRST, then falling back to the old separate lon/lat 1-D arrays
# -- required to keep reading both genuine Mirone-original SHAPENC files (always separate arrays)
# and every SHAPENC file this port itself wrote before the combined format existed.
function _shnc_read_coordpair(root, pairname::String, xname::String, yname::String, edt_f64)::Union{Nothing, Tuple{Vector{Float64}, Vector{Float64}}}
	parr = _shnc_group_open_array(root, pairname)
	if parr != C_NULL
		n = _shnc_array_count(parr) ÷ 2
		xy = _shnc_array_read_pairs_f64(parr, edt_f64, n)
		_shnc_release_array(parr)
		return xy
	end
	xarr = _shnc_group_open_array(root, xname)
	xarr == C_NULL && return nothing
	yarr = _shnc_group_open_array(root, yname)
	if yarr == C_NULL
		_shnc_release_array(xarr)
		return nothing
	end
	n = _shnc_array_count(xarr)
	x = _shnc_array_read_f64(xarr, edt_f64, n)
	y = _shnc_array_read_f64(yarr, edt_f64, n)
	_shnc_release_array(xarr); _shnc_release_array(yarr)
	(x, y)
end

# Cheap probe: is `path` a SHAPENC file? (extension + the "SHAPENC_type" global attribute we always
# write). Returns the type string ("Point"/"PolygonZ"/...) or `nothing`.
function _shnc_probe_type(path::String)::Union{Nothing, String}
	lowercase(splitext(first(split(path, '?')))[2]) == ".nc" || return nothing
	local ds
	try
		ds = _shnc_open_multidim_read(path)
	catch
		return nothing
	end
	root = _shnc_root(ds)
	a = _shnc_group_attr(root, "SHAPENC_type")
	t = a == C_NULL ? nothing : (s = _shnc_attr_str(a); isempty(s) ? nothing : s)
	a == C_NULL || _shnc_release_attr(a)
	ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (_SHNC_H,), root)
	ccall((:GDALClose, GMT.libgdal), Cint, (_SHNC_H,), ds)
	t
end
_shnc_is_shapenc(path::String)::Bool = _shnc_probe_type(path) !== nothing

# Split a Mx2/Mx3 matrix that may contain NaN separator rows (a `multiseg=true`-packed ensemble,
# see `_shnc_cell2multiseg`) back into its individual segments. A matrix with no NaN rows returns
# itself as the only segment.
function _shnc_split_nan_segments(mat::Matrix{Float64})::Vector{Matrix{Float64}}
	nanrows = findall(isnan, view(mat, :, 1))
	isempty(nanrows) && return [mat]
	segs = Matrix{Float64}[]
	start = 1
	for r in nanrows
		(r > start) && push!(segs, mat[start:r-1, :])
		start = r + 1
	end
	(start <= size(mat, 1)) && push!(segs, mat[start:end, :])
	segs
end

# Container-variable prefixes tried, in order, for each ensemble index -- a file can in principle
# mix geometry kinds across ensembles (each `shapenc`/`append` call picks its own `kind`), so every
# ensemble is probed independently rather than assuming one kind file-wide. `""` is a
# `shapenc(...; ids=...)`-written ensemble (isocs.jl): no type prefix at all, just the bare id --
# is_3D/geom for that case can't come from the (empty) prefix text, see the read loop below.
const _SHNC_PREFIXES = ("PointZ_", "Point_", "PolygonZ_", "Polygon_", "PolyLineZ_", "PolyLine_", "")

"""
    _shnc_read(path::String) -> Vector{GMT.GMTdataset}

Read a SHAPENC netCDF file (written by `shapenc`) back into a `Vector{GMTdataset}`, one entry per
segment (an ensemble packed via `multiseg=true` at write time is split back into its own segments
by `_shnc_split_nan_segments`). Each `GMTdataset`'s `.geom` is set from the ensemble's Point/
Polygon/PolyLine kind, `.proj4` from the file's `spatial_ref`, and `.attrib` carries every
container-variable attribute EXCEPT `BoundingBox` (each `GMTdataset` computes its own) --
`name` (the `tag` string, when present) and any numeric `tag` attribute (FIN/STG0/... for
isochrons) joined into a plain space-separated string (exact attribute display is not decided
yet -- this just gets the data and its labels through, unrounded).
"""
function _shnc_read(path::String)::Vector{GMT.GMTdataset}
	ds = _shnc_open_multidim_read(path)
	root = _shnc_root(ds)
	sr_attr = _shnc_group_attr(root, "spatial_ref")
	spatial_ref = sr_attr == C_NULL ? "+proj=longlat" : _shnc_attr_str(sr_attr)
	sr_attr == C_NULL || _shnc_release_attr(sr_attr)   # leaked handle here left a LATER reopen-for-update failing (proven live)
	geog = occursin("longlat", spatial_ref)
	# Fallback kind/dimensionality for a bare (`ids=`, no type prefix) ensemble -- the file-wide
	# "SHAPENC_type" global attribute (always written) is the best guess available since the empty
	# prefix itself carries no type information per-ensemble.
	type_attr = something(_shnc_attr_read_str(root, "SHAPENC_type"), "PolyLine")
	bare_is_3D = endswith(type_attr, "Z")
	bare_geom = startswith(type_attr, "Point") ? GMT.wkbPoint :
	            startswith(type_attr, "Polygon") ? GMT.wkbPolygon : GMT.wkbLineString
	Xname, Yname = geog ? ("lon", "lat") : ("X", "Y")
	coordpair_name = _shnc_coordpair_name(geog)
	edt_f64 = _shnc_edt_f64()
	# Ensemble "kk" identities are looked up by NAME (the "Ensemble_IDs" attribute, semicolon-
	# joined -- written by every `shapenc` call, see there) rather than assumed to be 1..N: a
	# `shapenc(...; ids=[...])` caller (isocs.jl) can use an arbitrary string as the actual variable
	# name, not just a sequential integer. A file written before "Ensemble_IDs" existed falls back
	# to the old 1..N integer assumption.
	ids_attr = _shnc_attr_read_str(root, "Ensemble_IDs")
	kks = if ids_attr === nothing || isempty(ids_attr)
		n_ens = something(_shnc_attr_read_i32(root, "Number_of_main_ensembles"), 0)
		string.(1:n_ens)
	else
		String.(split(ids_attr, ";"; keepempty=false))
	end

	out = GMT.GMTdataset[]
	for kk in kks
		for prefix in _SHNC_PREFIXES
			xy = _shnc_read_coordpair(root, "$(coordpair_name)$(prefix)$kk", "$(Xname)$(prefix)$kk", "$(Yname)$(prefix)$kk", edt_f64)
			xy === nothing && continue
			x, y = xy

			is_3D = prefix == "" ? bare_is_3D : endswith(prefix, "Z_")
			z = Float64[]
			if is_3D
				zname = (prefix == "" || startswith(prefix, "Point")) ? "z_$kk" : "Z$(prefix)$kk"
				zarr = _shnc_group_open_array(root, zname)
				if zarr != C_NULL
					z = _shnc_array_read_f64(zarr, edt_f64, n)
					_shnc_release_array(zarr)
				end
			end
			mat = (is_3D && length(z) == n) ? [x y z] : [x y]

			geom = prefix == "" ? bare_geom :
			       startswith(prefix, "Point") ? GMT.wkbPoint :
			       startswith(prefix, "Polygon") ? GMT.wkbPolygon : GMT.wkbLineString

			attrib = Dict{String, Union{String, Vector{String}}}()
			cvar = _shnc_group_open_array(root, "$(prefix)$kk")
			if cvar != C_NULL
				for aname in _shnc_array_attr_names(cvar)
					aname == "BoundingBox" && continue
					a = _shnc_array_attr(cvar, aname)
					a == C_NULL && continue
					attrib[aname] = _shnc_attr_is_string(a) ? _shnc_attr_str(a) : join(_shnc_attr_f64v(a), " ")
					_shnc_release_attr(a)
				end
				_shnc_release_array(cvar)
			end

			# Canonical proj4 from `geog` (already correctly detected via `occursin("longlat", ...)`
			# above), NOT the file's raw `spatial_ref` text verbatim: a Mirone-ORIGINAL SHAPENC file
			# (not written by this port) can store it in shorthand form ("+longlat", no "proj="), and
			# `GMT.mat2ds` double-prefixes an unrecognized string into "+proj=+longlat" -- malformed,
			# so `GMT.isgeog` on the resulting GMTdataset silently returns false downstream. That
			# broke geographic actor scaling on drop (xfac/zfac fell back to 1/1 cartesian), which put
			# real elevation/depth values on the SAME scale as raw lon/lat degrees and pushed every
			# point far outside the camera's clip range -- invisible, no error. Proven live on a real
			# Mirone file (CARTA604_IH.nc, spatial_ref="+longlat").
			proj4 = geog ? "+proj=longlat" : "+proj=xy"
			for seg in _shnc_split_nan_segments(mat)
				push!(out, GMT.mat2ds(seg; attrib=attrib, proj4=proj4, geom=geom))
			end
			break                    # this ensemble matched -- don't also try the other prefixes
		end
	end

	_shnc_release_edt(edt_f64)
	ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (_SHNC_H,), root)
	ccall((:GDALClose, GMT.libgdal), Cint, (_SHNC_H,), ds)
	out
end

# ---- OUT/IN boundary polygons (Mirone-style "bounded" point ensembles) --------------------

# Every root-group MDArray name -- used to discover PolyIN_x_y variables, whose two-number suffix
# is NOT a fixed function of `kk` alone (matches whatever the WRITER's own bookkeeping produced --
# our own shapenc()'s `ultimo+1`/`j+ultimo`, or Mirone's original shapenc.m convention), so they
# must be found by scanning, not guessed from kk directly.
function _shnc_array_names(root)::Vector{String}
	p = ccall((:GDALGroupGetMDArrayNames, GMT.libgdal), Ptr{Ptr{UInt8}}, (_SHNC_H, Ptr{Ptr{UInt8}}), root, C_NULL)
	p == C_NULL && return String[]
	names = String[]
	i = 0
	while true
		sp = unsafe_load(p, i + 1)
		sp == C_NULL && break
		push!(names, unsafe_string(sp))
		i += 1
	end
	ccall((:CSLDestroy, GMT.libgdal), Cvoid, (Ptr{Ptr{UInt8}},), p)
	names
end

const _SHNC_POLYIN_RE = r"^lonPolyIN_(\d+)_(\d+)$"
const _SHNC_POLYIN_RE_PAIR = r"^lonLatPolyIN_(\d+)_(\d+)$"

# Read one lon/lat coordinate pair into an Nx2 Matrix, or `nothing` if neither the combined
# "lonLat..." array nor the old separate lon/lat arrays are present (see _shnc_read_coordpair).
function _shnc_read_xyarr(root, pairname::String, xname::String, yname::String, edt_f64)::Union{Nothing, Matrix{Float64}}
	xy = _shnc_read_coordpair(root, pairname, xname, yname, edt_f64)
	xy === nothing && return nothing
	x, y = xy
	[x y]
end

"""
    _shnc_read_bounded(path::String) -> Vector{@NamedTuple{kk::String, outp, ins, ds}}

Read a SHAPENC file's ensembles alongside their OUTER/INNER boundary polygons (the SHAPENC
convention `shapenc()`'s `outer`/`inner` kwargs write): a combined `lonLatPolyOUT_kk` (or a
Mirone-original file's separate `lonPolyOUT_kk`/`latPolyOUT_kk`, e.g. `enxertos_pascal.nc`) and one
or more `lonLatPolyIN_x_y`/`lonPolyIN_x_y`+`latPolyIN_x_y` per ensemble. One `NamedTuple` per ensemble --
`outp::Union{Nothing,Matrix{Float64}}` (lon/lat ring, `nothing` when this ensemble has no OUTER
boundary), `ins::Vector{Matrix{Float64}}` (0 or more lon/lat hole rings), and `ds::GMTdataset` (the
SAME per-ensemble result `_shnc_read` already produces, x,y[,z] + attrib/proj4 -- reused, not
re-derived, so this is a thin second pass over `_shnc_read`, not a parallel reader). Callers (e.g.
`drop.jl`) use `outp !== nothing` to decide Mirone's own display convention: plot ONLY the OUT/IN
boundary, keep the raw point swarm (`ds.data`) hidden until asked for.
"""
function _shnc_read_bounded(path::String)
	ds_list = _shnc_read(path)
	dsr = _shnc_open_multidim_read(path)
	root = _shnc_root(dsr)
	edt_f64 = _shnc_edt_f64()
	names = _shnc_array_names(root)
	ids_attr = _shnc_attr_read_str(root, "Ensemble_IDs")
	kks = if ids_attr === nothing || isempty(ids_attr)
		n_ens = something(_shnc_attr_read_i32(root, "Number_of_main_ensembles"), 0)
		string.(1:n_ens)
	else
		String.(split(ids_attr, ";"; keepempty=false))
	end

	out = NamedTuple{(:kk, :outp, :ins, :ds), Tuple{String, Union{Nothing,Matrix{Float64}}, Vector{Matrix{Float64}}, GMT.GMTdataset}}[]
	for (i, kk) in enumerate(kks)
		i > length(ds_list) && break   # a multiseg-split ensemble would desync the 1:1 kk<->ds_list
		                                # assumption below -- bounded ensembles are always single-segment
		                                # Point swarms in practice, so this just stops safely rather than
		                                # mismatching kk against the wrong dataset.
		outp = _shnc_read_xyarr(root, "lonLatPolyOUT_$kk", "lonPolyOUT_$kk", "latPolyOUT_$kk", edt_f64)
		ins = Matrix{Float64}[]
		seen_suf = Set{String}()
		for nm in names
			mm = match(_SHNC_POLYIN_RE, nm)
			mm === nothing && (mm = match(_SHNC_POLYIN_RE_PAIR, nm))
			mm === nothing && continue
			(mm.captures[1] == kk || mm.captures[2] == kk) || continue
			suf = "$(mm.captures[1])_$(mm.captures[2])"
			suf in seen_suf && continue
			push!(seen_suf, suf)
			m = _shnc_read_xyarr(root, "lonLatPolyIN_$suf", "lonPolyIN_$suf", "latPolyIN_$suf", edt_f64)
			m === nothing || push!(ins, m)
		end
		push!(out, (kk=kk, outp=outp, ins=ins, ds=ds_list[i]))
	end

	_shnc_release_edt(edt_f64)
	ccall((:GDALGroupRelease, GMT.libgdal), Cvoid, (_SHNC_H,), root)
	ccall((:GDALClose, GMT.libgdal), Cint, (_SHNC_H,), dsr)
	out
end
