# gmtedit.jl — Geophysics > Magnetics > "gmtedit" : the MGD77 track editor.
# Port of Mirone's src_figs/gmtedit.m ("Revival of the ancient Sunview gmtedit").
#
# Three stacked panels (by default Gravity / Magnetics / Bathymetry) plot one cruise's channels
# against along-track distance (or record number). Clicking a point flags it BAD (green square ->
# red square); clicking it again unflags it. A rubber-band rectangle flags every point inside it.
# Shift-click "despikes" an isolated point (relocates it onto the value a cubic spline through its
# neighbours predicts). Saving writes the surviving values back into the MGD77+ netCDF file, with
# flagged points replaced by the variable's own `missing_value`.
#
# SPLIT (the project's standard C++-GUI / Julia-data division, see .wolf/knowledge/mirone-port.md):
# the window, the 3 charts, the point picking, the rubber bands, the scroll/zoom and the segment
# dragging all live in deps/src/67_gmtedit.cpp. THIS file owns every byte of data: reading the
# cruise, computing the outlier/nav-filter/despike numbers, and writing the file back. They talk
# through the `gmtvtk_gmtedit_*` C API + the one `_on_gmtedit` callback (action + ';'-joined
# argument string, the same convention tilestool.jl/xyplot.jl already use).
#
# READING — no new reader was invented (see .wolf/cerebrum.md 2026-07-24):
#   • lon, lat, rtime, dist, vel and every STANDARD MGD77 column (faa, mtf1, mag, depth, gobs,
#     twt, diur, …) come from `GMT.gmt("mgd77list <file> -F…")` — the SAME monolithic-mode call
#     mgd77tracks.jl already uses (`GMT.mgd77list` itself is unwrapped, GMT.jl only includes
#     magref.jl). `dist` is GMT's own along-track distance in km (-Ndk) and `vel` its own ship
#     speed in knots (-Nsn), so neither is re-derived here.
#   • MGD77+ EXTENDED columns (the user-added ones: anom_cm4, best_cm4, …) are rejected by
#     mgd77list in this GMT build — it prints them in its own "valid column" listing and then
#     answers `"anom_cm4" is not a valid column name` (confirmed live 2026-07-26 on
#     C:\a1\mgd77\01010003.nc, with and without -T/-Tm/-Te and with the bare cruise ID). Those are
#     read straight from the netCDF variable instead, through the GDAL MDArray API shapenc.jl
#     already wraps — exactly the variables (and the scale_factor/missing_value handling)
#     Mirone's own `nc_funs('varget', …)` reads.
#   mgd77list emits one row per netCDF record (verified on 01010003 / 01010007 / 01010043: 340 /
#   11318 / 13068 rows vs identical `lon` array lengths), which is what makes the two sources
#   index-aligned; `_ge_read` asserts it rather than assuming it.
#
#   Some paths defeat mgd77list entirely — a long, deeply-nested one answers "No such file or
#   directory" followed by a wall of "NetCDF: Not a valid ID" (confirmed live 2026-07-26; the same
#   bytes read fine from a short path), because the mgd77 suite resolves its argument as a cruise
#   ID against MGD77_HOME rather than opening it as a plain file. `_ge_read` then falls back to
#   reading every column from the netCDF variables and deriving the abscissa with `_seg_dist_azim`
#   (measure.jl — the project's ONE geodesic distance function, the one the "Line length…" menu
#   uses), and says so in the window's message panel. The two distances differ only by the earth
#   model (1342.9 vs 1342.0 km over a 340-record cruise, 0.07%).
#
# WRITING — GDAL MDArray in update mode (GDALOpenEx with GDAL_OF_UPDATE|GDAL_OF_MULTIDIM_RASTER,
# proven live on a real MGD77+ file: read raw int32, poke `missing_value`, close, and `mgd77list`
# then reports NaN for that record). Raw storage values are written, so the caller must divide by
# `scale_factor` first — same arithmetic Mirone's `y_g / handles.gravScaleF` does.

# ---------------------------------------------------------------------------------------------
#  the per-window track
# ---------------------------------------------------------------------------------------------

# One loaded cruise. `y[k]`/`vars[k]` are the channel plotted in panel k (1..3). `velSlot` is the
# panel showing ship speed instead of a file variable (Mirone's handles.got_vel) — "vel" is not an
# MGD77+ variable, so that panel is never saved.
mutable struct GmtEditTrack
	path::String
	n::Int
	lon::Vector{Float64}
	lat::Vector{Float64}
	time::Vector{Float64}          # rtime, seconds since the GMT epoch (empty when the file has none)
	dist::Vector{Float64}          # along-track distance, km (mgd77list -Ndk)
	vel::Vector{Float64}           # ship speed, knots (mgd77list -Nsn)
	xdata::Vector{Float64}         # the abscissa actually plotted (dist, or 1:n when xISdist=false)
	vars::Vector{String}
	labels::Vector{String}         # the Y-axis title of each panel
	y::Vector{Vector{Float64}}
	year::Int
	month::Int
	agency::String
	xISdist::Bool
	velSlot::Int
	navEdited::Bool                # a despikeNav recomputed lon/lat -> they must be saved too
	distFromGMT::Bool              # false = mgd77list was unusable, distance came from _seg_dist_azim
	islegacy::Bool                 # true = the pre-MGD77 *.gmt binary, not an MGD77+ netCDF
end

# Live editor windows: the C `GmtEdit *` handle -> its loaded track. Dropped when the window
# reports "closed".
const _GE_REG = Dict{Ptr{Cvoid},GmtEditTrack}()

# The three default channels, in panel order (gmtedit.m: 'Gravity anomaly (mGal)' /
# 'Magnetic field (nT)' / 'Bathymetry (m)').
const _GE_DEFAULT_VARS = ["faa", "mtf1", "depth"]

# Axis labels for the variables gmtedit.m names explicitly; anything else is labelled by its own
# netCDF `long_name` + `units` (falling back to the bare variable name).
const _GE_LABELS = Dict{String,String}("faa"   => "Gravity anomaly (mGal)",
                                        "mtf1"  => "Magnetic field (nT)",
                                        "depth" => "Bathymetry (m)",
                                        "vel"   => "Speed (knots)")

# Columns mgd77list understands (its own -F listing). Anything else is an MGD77+ extended column
# and gets read from the netCDF variable directly.
const _GE_MGD77_COLS = Set{String}(["drt", "tz", "ptc", "twt", "depth", "bcc", "btc", "mtf1",
	"mtf2", "mag", "msens", "diur", "msd", "gobs", "eot", "faa", "nqc", "carter", "igrf",
	"ceot", "ngrav", "weight"])

# ---------------------------------------------------------------------------------------------
#  netCDF (GDAL MDArray) access — read/write ONE variable of an MGD77+ file
# ---------------------------------------------------------------------------------------------

# Open an MGD77+ file for reading (`update=false`) or in-place editing (`update=true`). The driver
# is forced to netCDF for the same reason shapenc.jl forces it (`_shnc_open_multidim_update`):
# GDALOpenEx's probe otherwise leans on the extension.
function _ge_nc_open(path::String, update::Bool)
	ccall((:GDALAllRegister, GMT.libgdal), Cvoid, ())
	flags = UInt32(update ? (0x01 | 0x10 | 0x40) : (0x10 | 0x40))   # [UPDATE|]MULTIDIM_RASTER|VERBOSE_ERROR
	ds = ccall((:GDALOpenEx, GMT.libgdal), Ptr{Cvoid},
	           (Cstring, UInt32, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}, Ptr{Ptr{UInt8}}),
	           path, flags, _shnc_csl(["netCDF"]), C_NULL, C_NULL)
	ds == C_NULL && error("gmtedit: GDAL could not open \"$path\" as netCDF")
	return ds
end

_ge_nc_close(ds) = ccall((:GDALClose, GMT.libgdal), Cvoid, (Ptr{Cvoid},), ds)

_ge_nc_array(root, name::String) =
	ccall((:GDALGroupOpenMDArray, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cstring, Ptr{Ptr{UInt8}}), root, name, C_NULL)

# Names of every 1-D array in the file's root group.
function _ge_nc_varnames(root)::Vector{String}
	lst = ccall((:GDALGroupGetMDArrayNames, GMT.libgdal), Ptr{Ptr{UInt8}},
	            (Ptr{Cvoid}, Ptr{Ptr{UInt8}}), root, C_NULL)
	out = String[]
	lst == C_NULL && return out
	k = 0
	while true                                       # the CSL is NULL-terminated
		p = unsafe_load(lst, k + 1)
		p == C_NULL && break
		push!(out, unsafe_string(p)); k += 1
	end
	return out
end

# Length of a 1-D MDArray (0 when it is not 1-D).
function _ge_nc_len(arr)::Int
	nref = Ref{Csize_t}(0)
	d = ccall((:GDALMDArrayGetDimensions, GMT.libgdal), Ptr{Ptr{Cvoid}}, (Ptr{Cvoid}, Ref{Csize_t}), arr, nref)
	(d == C_NULL || Int(nref[]) != 1) && return 0
	return Int(ccall((:GDALDimensionGetSize, GMT.libgdal), UInt64, (Ptr{Cvoid},), unsafe_load(d, 1)))
end

# One numeric attribute of an MDArray (`nothing` when absent). GDAL reports a string attribute as
# 0.0 through ReadAsDouble, so only attributes known to be numeric go through here.
function _ge_nc_attr(arr, name::String)
	a = ccall((:GDALMDArrayGetAttribute, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cstring), arr, name)
	a == C_NULL && return nothing
	v = ccall((:GDALAttributeReadAsDouble, GMT.libgdal), Cdouble, (Ptr{Cvoid},), a)
	_shnc_release_attr(a)
	return v
end

# One string attribute of an MDArray ("" when absent).
function _ge_nc_attr_str(arr, name::String)::String
	a = ccall((:GDALMDArrayGetAttribute, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cstring), arr, name)
	a == C_NULL && return ""
	p = ccall((:GDALAttributeReadAsString, GMT.libgdal), Cstring, (Ptr{Cvoid},), a)
	s = p == C_NULL ? "" : unsafe_string(p)
	_shnc_release_attr(a)
	return s
end

# Every global (root-group) attribute of the file, as name => text. This is the MGD77+ header —
# the same place Mirone's read_mgd77_plus fishes Survey_Departure_Year / Source_Institution out of.
function _ge_nc_global_attrs(root)::Dict{String,String}
	nref = Ref{Csize_t}(0)
	lst = ccall((:GDALGroupGetAttributes, GMT.libgdal), Ptr{Ptr{Cvoid}},
	            (Ptr{Cvoid}, Ref{Csize_t}, Ptr{Ptr{UInt8}}), root, nref, C_NULL)
	out = Dict{String,String}()
	(lst == C_NULL) && return out
	for k in 1:Int(nref[])                            # counted, NOT NULL-terminated
		a = unsafe_load(lst, k)
		a == C_NULL && continue
		np = ccall((:GDALAttributeGetName, GMT.libgdal), Cstring, (Ptr{Cvoid},), a)
		vp = ccall((:GDALAttributeReadAsString, GMT.libgdal), Cstring, (Ptr{Cvoid},), a)
		np == C_NULL && continue
		out[unsafe_string(np)] = (vp == C_NULL ? "" : unsafe_string(vp))
	end
	return out
end

# Read a 1-D variable's RAW storage values (no scale_factor applied — GDAL's MDArray read hands
# back the stored integers, verified live). Returns an empty vector when the variable is missing.
function _ge_nc_read_raw(root, name::String)::Vector{Float64}
	arr = _ge_nc_array(root, name)
	arr == C_NULL && return Float64[]
	n = _ge_nc_len(arr)
	if n == 0
		_shnc_release_array(arr); return Float64[]
	end
	edt = _shnc_edt_f64()
	buf = Vector{Float64}(undef, n)
	r = ccall((:GDALMDArrayRead, GMT.libgdal), Cint,
	          (Ptr{Cvoid}, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
	          arr, UInt64[0], UInt64[n], Int64[1], Int64[1], edt, buf, C_NULL, 0)
	_shnc_release_edt(edt); _shnc_release_array(arr)
	r == 0 && error("gmtedit: failed reading netCDF variable \"$name\"")
	return buf
end

# Read a variable the way `nc_funs('varget', …)` does: raw values, `missing_value`/`_FillValue`
# turned into NaN, then scale_factor/add_offset applied.
function _ge_nc_read_scaled(root, name::String)::Vector{Float64}
	arr = _ge_nc_array(root, name)
	arr == C_NULL && return Float64[]
	miss  = _ge_nc_attr(arr, "missing_value")
	miss === nothing && (miss = _ge_nc_attr(arr, "_FillValue"))
	scale = _ge_nc_attr(arr, "scale_factor")
	off   = _ge_nc_attr(arr, "add_offset")
	_shnc_release_array(arr)
	v = _ge_nc_read_raw(root, name)
	isempty(v) && return v
	if miss !== nothing
		m = miss::Float64
		@inbounds for i in eachindex(v)
			(v[i] == m) && (v[i] = NaN)
		end
	end
	(scale !== nothing) && (v .*= scale::Float64)
	(off   !== nothing) && (v .+= off::Float64)
	return v
end

# GDAL numeric type codes that mean "this variable is stored as integers", so a value being written
# back must be rounded onto one. (GDALDataType: Byte 1, UInt16 2, Int16 3, UInt32 4, Int32 5,
# Float32 6, Float64 7, …, Int64 12, UInt64 13, Int8 14.)
const _GE_INT_GDT = Set{Int}([1, 2, 3, 4, 5, 12, 13, 14])

# `scale_factor` (1.0 when absent), `missing_value` (NaN when absent) and whether the variable is
# stored as an integer — what the save path needs to turn display values back into stored ones.
function _ge_nc_scaling(root, name::String)::Tuple{Float64,Float64,Bool}
	arr = _ge_nc_array(root, name)
	arr == C_NULL && return (1.0, NaN, false)
	scale = _ge_nc_attr(arr, "scale_factor")
	miss  = _ge_nc_attr(arr, "missing_value")
	miss === nothing && (miss = _ge_nc_attr(arr, "_FillValue"))
	isint = false
	dt = ccall((:GDALMDArrayGetDataType, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), arr)
	if dt != C_NULL
		cls = ccall((:GDALExtendedDataTypeGetClass, GMT.libgdal), Cint, (Ptr{Cvoid},), dt)
		if cls == 0                                    # GEDTC_NUMERIC
			gdt = ccall((:GDALExtendedDataTypeGetNumericDataType, GMT.libgdal), Cint, (Ptr{Cvoid},), dt)
			isint = Int(gdt) in _GE_INT_GDT
		end
		_shnc_release_edt(dt)
	end
	_shnc_release_array(arr)
	return (scale === nothing ? 1.0 : scale::Float64, miss === nothing ? NaN : miss::Float64, isint)
end

# Write RAW storage values back into an existing 1-D variable (the file must have been opened with
# update=true). Length must match the variable's own.
function _ge_nc_write_raw!(root, name::String, v::Vector{Float64})
	arr = _ge_nc_array(root, name)
	arr == C_NULL && error("gmtedit: variable \"$name\" not found for writing")
	n = _ge_nc_len(arr)
	if n != length(v)
		_shnc_release_array(arr)
		error("gmtedit: \"$name\" has $n records but $(length(v)) values were given")
	end
	edt = _shnc_edt_f64()
	r = ccall((:GDALMDArrayWrite, GMT.libgdal), Cint,
	          (Ptr{Cvoid}, Ptr{UInt64}, Ptr{UInt64}, Ptr{Int64}, Ptr{Int64}, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}, Csize_t),
	          arr, UInt64[0], UInt64[n], Int64[1], Int64[1], edt, v, C_NULL, 0)
	_shnc_release_edt(edt); _shnc_release_array(arr)
	r == 0 && error("gmtedit: failed writing netCDF variable \"$name\"")
	return
end

# ---------------------------------------------------------------------------------------------
#  the legacy pre-MGD77 *.gmt binary format
# ---------------------------------------------------------------------------------------------
#
# GMT >= 5's mgd77 suite dropped this format entirely, and Mirone reads it with a compiled MEX. The
# layout below is NOT reverse-engineered: it is transcribed from that MEX's own C source,
# `C:\SVN\mironeWC64\mex\gmtlist_m.c` (the classic GMT `gmt_mgg` structures), cross-checked against
# the writer in gmtedit.m's own save_clickedCB.
#
#   header, 18 bytes:  int32 year | int32 n_records | char[10] agency
#   record, 18 bytes:  int32 time | int32 lat | int32 lon | int16 grav | int16 mag | int16 topo
#
#   time  seconds since Jan 1 00:00 of `year`          (gmtlist_m.c:110-113, :464 reads 18 bytes)
#   lat   micro-degrees, x 1e-6                        (gmtlist_m.c:524-525, MDEG2DEG)
#   lon   micro-degrees, x 1e-6
#   grav  0.1 mGal  -> value = stored * 0.1            (gmtlist_m.c:550)
#   mag   nT, stored as-is                             (gmtlist_m.c:551)
#   topo  m, stored as-is                              (gmtlist_m.c:552)
#   -32000 in any of the three = no data               (GMTMGG_NODATA, gmtlist_m.c:83)
#
# Big-endian files are detected exactly as the MEX does — both header int32s coming out negative
# (gmtlist_m.c:424) — and byte-swapped.
const _GE_GMT_NODATA = Int16(-32000)
const _GE_GMT_RECBYTES = 18

# Cumulative days before each month over the 5 years a cruise may span, i.e. gmtmgg_init's
# `daymon` table (gmtlist_m.c:712-744). Reproduced EXACTLY, including its leap rule
# `y % 4 == 0 && !(y % 400 == 0)` — which calls 2000 a common year and 1900 a leap one. That rule
# is wrong as a calendar, but it is what every existing *.gmt file was written with and what
# gmtlist reads them back with, so "fixing" it here would put this reader out of step with both.
function _ge_gmt_daymon(year1::Int)::Vector{Int}
	dm = [0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30]     # dm[1] is January's slot, = 0 at first
	daymon = zeros(Int, 61)
	month = 0
	for y in 0:4
		thisyear = year1 + y
		(thisyear % 4 == 0 && !(thisyear % 400 == 0)) && (dm[3] = 29)
		for m in 1:12
			month += 1
			daymon[month + 1] = daymon[month] + dm[m]
		end
		dm[3] = 28
		dm[1] = 31                                            # December, for the next year's pass
	end
	return daymon
end

# Seconds-since-Jan-1-of-`year1` -> (year, month, day, hour, minute, second), the inverse of
# gmtmgg_time. Used only for the Cruise Info report.
function _ge_gmt_date(t::Int, year1::Int)
	daymon = _ge_gmt_daymon(year1)
	nday = div(t, 86400)
	rem  = t - nday * 86400
	mon  = 1
	while mon < 60 && daymon[mon + 2] <= nday
		mon += 1
	end
	day   = nday - daymon[mon + 1] + 1
	year  = year1 + div(mon - 1, 12)
	month = mod(mon - 1, 12) + 1
	return (year, month, day, div(rem, 3600), div(mod(rem, 3600), 60), mod(rem, 60))
end

# One legacy cruise, in the units gmtlist_m hands back.
struct GmtLegacyTrack
	year::Int
	n::Int
	agency::String
	time::Vector{Float64}          # seconds since Jan 1 of `year`
	lon::Vector{Float64}
	lat::Vector{Float64}
	grav::Vector{Float64}          # mGal
	mag::Vector{Float64}           # nT
	topo::Vector{Float64}          # m
end

_ge_bswap(v::Int32) = bswap(v)
_ge_bswap(v::Int16) = bswap(v)

"""
    _ge_gmt_read(path; geodetic=false) -> GmtLegacyTrack

Read a legacy pre-MGD77 `*.gmt` binary cruise. `geodetic=true` keeps longitudes in [0,360) (the
module default); `false` is gmtedit's own `-G`, which wraps them to [-180,180] (gmtlist_m.c:562).
No-data (-32000) becomes NaN.
"""
function _ge_gmt_read(path::String; geodetic::Bool=false)::GmtLegacyTrack
	isfile(path) || error("gmtedit: file not found: $path")
	raw = read(path)
	length(raw) >= _GE_GMT_RECBYTES || error("gmtedit: \"$path\" is too short to be a *.gmt file")
	io   = IOBuffer(raw)
	year = read(io, Int32)
	nrec = read(io, Int32)
	swap = (year < 0 && nrec < 0)                             # gmtlist_m.c:424
	swap && (year = bswap(year); nrec = bswap(nrec))
	# The 10-byte agency field is free-form and real files do carry binary junk in it (Mirone's own
	# data/tests/so_lucky.gmt has bytes 255,217,205,… there), so keep only printable ASCII rather
	# than let that reach a window title or the Info report.
	agency = String(strip(String(UInt8[b for b in read(io, 10) if 0x20 <= b <= 0x7e])))
	n = Int(nrec)
	(0 < n) || error("gmtedit: \"$path\" declares $n records — not a *.gmt file?")
	avail = div(length(raw) - 18, _GE_GMT_RECBYTES)
	if n > avail
		@warn "gmtedit: *.gmt header claims $n records but only $avail fit in the file — reading $avail" path
		n = avail
	end
	tim = Vector{Float64}(undef, n); lon = Vector{Float64}(undef, n); lat = Vector{Float64}(undef, n)
	grv = Vector{Float64}(undef, n); mag = Vector{Float64}(undef, n); top = Vector{Float64}(undef, n)
	for i in 1:n
		t  = read(io, Int32); la = read(io, Int32); lo = read(io, Int32)
		g  = read(io, Int16); m  = read(io, Int16); d  = read(io, Int16)
		if swap
			t = bswap(t); la = bswap(la); lo = bswap(lo)
			g = bswap(g); m = bswap(m); d = bswap(d)
		end
		tim[i] = Float64(t)
		lat[i] = Float64(la) * 1e-6
		x      = Float64(lo) * 1e-6
		(!geodetic && x > 180.0) && (x -= 360.0)               # gmtlist_m.c:562, gmtedit's -G
		lon[i] = x
		grv[i] = (g == _GE_GMT_NODATA) ? NaN : Float64(g) * 0.1
		mag[i] = (m == _GE_GMT_NODATA) ? NaN : Float64(m)
		top[i] = (d == _GE_GMT_NODATA) ? NaN : Float64(d)
	end
	return GmtLegacyTrack(Int(year), n, agency, tim, lon, lat, grv, mag, top)
end

"""
    _ge_gmt_write(path, year, agency, time, lon, lat, grav, mag, topo)

Write a legacy `*.gmt` binary cruise — the inverse of `_ge_gmt_read`, and the format gmtedit.m's
`save_clickedCB` writes with its `fwrite` loop. `grav` is in mGal, `mag` in nT, `topo` in m; NaN
(and non-finite) values become -32000.

NOTE — one deliberate correction to gmtedit.m. Its writer stores gravity as `int16(y_g)` while the
reader it pairs with returns `record.gmt[0] * 0.1` (gmtlist_m.c:550), so a Mirone save/reload cycle
silently divides gravity by ten. This writer stores `round(grav * 10)`, which is what makes the
round trip an identity. Everything else matches gmtedit.m byte for byte.
"""
function _ge_gmt_write(path::String, year::Int, agency::String, time::Vector{Float64},
                       lon::Vector{Float64}, lat::Vector{Float64}, grav::Vector{Float64},
                       mag::Vector{Float64}, topo::Vector{Float64})
	n = length(lon)
	(length(lat) == n && length(time) == n) || error("gmtedit: *.gmt write: lon/lat/time length mismatch")
	# The agency field is exactly 10 BYTES (gmtedit.m pads it to 10 chars, but that is the same
	# thing only for ASCII — a non-ASCII byte in there would otherwise push every record out of
	# alignment and silently corrupt the whole file).
	ab = fill(UInt8(' '), 10)
	cu = codeunits(agency)
	nb = min(10, length(cu))
	(nb > 0) && (ab[1:nb] = cu[1:nb])
	i16(v, scale) = (isfinite(v) ? Int16(clamp(round(v * scale), -32767, 32767)) : _GE_GMT_NODATA)
	open(path, "w") do io
		write(io, Int32(year)); write(io, Int32(n))
		write(io, ab)
		for i in 1:n
			write(io, Int32(round(time[i])))
			write(io, Int32(round(lat[i] * 1e6)))
			write(io, Int32(round(lon[i] * 1e6)))
			write(io, i <= length(grav) ? i16(grav[i], 10.0) : _GE_GMT_NODATA)   # 0.1 mGal units
			write(io, i <= length(mag)  ? i16(mag[i],   1.0) : _GE_GMT_NODATA)
			write(io, i <= length(topo) ? i16(topo[i],  1.0) : _GE_GMT_NODATA)
		end
	end
	return path
end

# ---------------------------------------------------------------------------------------------
#  "is this file a cruise?" — the test File > Open / drag-and-drop routes on
# ---------------------------------------------------------------------------------------------

# A legacy *.gmt binary? Sniffed from the 18-byte header rather than trusted to the extension: the
# same ".gmt" suffix is used for plain ASCII multi-segment tables (Mirone ships several, e.g.
# data/isocs/outros/GSFML.*.picks.gmt), which must NOT be dragged into the track editor. The test is
# exact — a file whose declared record count reproduces its own byte length is not a coincidence.
function _ge_is_legacy_gmt(path::String)::Bool
	try
		sz = filesize(path)
		sz >= 18 + _GE_GMT_RECBYTES || return false
		yr, nr = open(path, "r") do io
			(read(io, Int32), read(io, Int32))
		end
		(yr < 0 && nr < 0) && (yr = bswap(yr); nr = bswap(nr))      # big-endian file
		(1939 <= yr <= 2100) || return false                         # NGDC_OLDEST_YY (gmtlist_m.c:85)
		nr > 0 || return false
		return sz == 18 + _GE_GMT_RECBYTES * Int(nr)
	catch
		return false
	end
end

# An MGD77+ netCDF? Recognised by the MGD77 header attributes `mgd77manage` writes into the root
# group ("Format_Acronym" = MGD77 / MGD77T, "Data_Center_File_Number" = the NGDC ID) — the same kind
# of global-attribute sniff `_shnc_is_shapenc` uses to spot a SHAPENC file, and for the same reason:
# a cruise is not a grid, and the netCDF extension alone says nothing.
function _ge_is_mgd77plus(path::String)::Bool
	try
		ds = _ge_nc_open(path, false)
		try
			a = _ge_nc_global_attrs(_shnc_root(ds))
			return haskey(a, "Format_Acronym") || haskey(a, "Data_Center_File_Number")
		finally
			_ge_nc_close(ds)
		end
	catch
		return false
	end
end

"""
    _ge_is_mgd77(path) -> Bool

Is `path` an MGD77 cruise — either the legacy pre-MGD77 `*.gmt` binary or an MGD77+ netCDF? Both
are checked by CONTENT, never by extension alone.
"""
function _ge_is_mgd77(path::String)::Bool
	isfile(path) || return false
	ext = lowercase(splitext(path)[2])
	ext == ".gmt" && return _ge_is_legacy_gmt(path)
	ext == ".nc"  && return _ge_is_mgd77plus(path)
	return false
end

# ---------------------------------------------------------------------------------------------
#  OPTcontrol's -V switch (gmtedit.m parse_optV)
# ---------------------------------------------------------------------------------------------

# gmtedit.m reads Mirone's data/OPTcontrol.txt for a `MIR_GMTEDIT -V…` line that picks WHICH three
# MGD77+ variables go in the three panels, which extra ones get overlaid, and whether the abscissa
# is distance or record number. iGMT has no OPTcontrol.txt — its equivalent user-settings file is
# ~/.gmt/iGMT.ini (see .wolf memory prefs-storage-ini-never-registry), so the identical switch is
# read from a `[gmtedit] V=` key there. The SYNTAX is gmtedit.m's, unchanged:
#
#     -Vvar1,var2,var3[:varI+slotI[/varI+slotI[/varI+slotI]]][|]
#
# `varI+slotI` overlays variable varI on panel slotI (1..3); a trailing '|' makes the abscissa the
# record number instead of distance in km.
#
# Returns (vars, overlays, xISdist). `vars` is empty when no -V was given (=> the gmtedit.m
# defaults faa/mtf1/depth); `overlays` is a vector of (varname, slot).
function _ge_parse_optv(optV::String)::Tuple{Vector{String},Vector{Tuple{String,Int}},Bool}
	vars     = String[]
	overlays = Tuple{String,Int}[]
	xISdist  = true
	s = strip(optV)
	isempty(s) && return (vars, overlays, xISdist)
	startswith(s, "-V") && (s = s[3:end])
	if endswith(s, "|")                              # abscissa = the "fiducial" record numbers
		xISdist = false
		s = s[1:prevind(s, lastindex(s))]
	end
	icol  = findfirst(':', s)
	head  = icol === nothing ? s : s[1:prevind(s, icol)]
	tail  = icol === nothing ? "" : s[nextind(s, icol):end]
	parts = split(head, ',')
	if length(parts) == 3                            # gmtedit.m only accepts exactly three
		vars = String[strip(String(p)) for p in parts]
	elseif !isempty(strip(head))
		vars = copy(_GE_DEFAULT_VARS)                 # the '-V:anom+2' form keeps the defaults
	end
	for chunk in split(tail, '/'; keepempty=false)
		ip = findfirst('+', chunk)
		ip === nothing && continue
		nm = strip(String(chunk[1:prevind(chunk, ip)]))
		sl = tryparse(Int, strip(String(chunk[nextind(chunk, ip):end])))
		(isempty(nm) || sl === nothing || !(1 <= sl <= 3)) && continue
		push!(overlays, (nm, sl))
	end
	return (vars, overlays, xISdist)
end

# EVERY key=value of `[<section>]`, in file order (empty when the file or the section is absent).
# THE Julia-side reader of ~/.gmt/iGMT.ini — the C++ side owns the file through QSettings, Julia only
# ever reads it. It lives here because gmtedit was the first caller, but it is section/key agnostic:
# callers wanting one key pick it out (`_ini_get`, `_ge_ini_optv`), callers wanting a whole section
# (palettes.jl's `[ColorPalettes]` custom CPT list) take it whole. Never a second hand-rolled parser.
function _ini_section(section::String)::Vector{Pair{String,String}}
	out = Pair{String,String}[]
	f = joinpath(homedir(), ".gmt", "iGMT.ini")
	isfile(f) || return out
	sec, cur = lowercase(section), ""
	for line in eachline(f)
		t = strip(line)
		(isempty(t) || startswith(t, ';') || startswith(t, '#')) && continue
		if startswith(t, '[') && endswith(t, ']')
			cur = lowercase(t[2:prevind(t, lastindex(t))]); continue
		end
		cur == sec || continue
		ie = findfirst('=', t)
		ie === nothing && continue
		push!(out, String(strip(String(t[1:prevind(t, ie)]))) => String(strip(String(t[nextind(t, ie):end]))))
	end
	return out
end

# The value of `<key>=` under `[<section>]`, "" when the file, the section or the key is absent.
function _ini_get(section::String, key::String)::String
	want = lowercase(key)
	for (k, v) in _ini_section(section)
		lowercase(k) == want && return v
	end
	return ""
end

# The `[gmtedit] V=` line of ~/.gmt/iGMT.ini ("" when the file/key is absent).
_ge_ini_optv()::String = _ini_get("gmtedit", "V")

# ---------------------------------------------------------------------------------------------
#  reading a cruise
# ---------------------------------------------------------------------------------------------

# Split one mgd77list run's output into per-column vectors. Returns `nothing` when the module gave
# nothing usable, so the caller can fall back to the netCDF variables.
function _ge_mgd77list(path::String, cols::Vector{String})
	isempty(cols) && return nothing
	D = GMT.gmt("mgd77list " * path * " -F" * join(cols, ',') * " -Ndk -Nsn")
	d1 = D isa AbstractVector ? (isempty(D) ? nothing : first(D)) : D
	d1 === nothing && return nothing
	m = d1.data
	(m === nothing || size(m, 1) < 1 || size(m, 2) != length(cols)) && return nothing
	return Dict{String,Vector{Float64}}(cols[k] => Vector{Float64}(@view m[:, k]) for k in 1:length(cols))
end

# The axis label for one channel: gmtedit.m's own three strings where it names them, else the
# variable's own netCDF long_name + units, so an MGD77+ extended column is still self-describing.
function _ge_label(root, name::String)::String
	haskey(_GE_LABELS, name) && return _GE_LABELS[name]
	arr = _ge_nc_array(root, name)
	arr == C_NULL && return name
	ln = _ge_nc_attr_str(arr, "long_name")
	un = _ge_nc_attr_str(arr, "units")
	_shnc_release_array(arr)
	isempty(ln) && (ln = name)
	return isempty(un) ? ln : "$ln ($un)"
end

# Read one cruise into a GmtEditTrack. `vars` are the three channel names (empty -> the gmtedit.m
# defaults). `extra` names additional variables the caller wants back (the -V overlays).
function _ge_read(path::String, vars::Vector{String}, xISdist::Bool,
                  extra::Vector{String}=String[])::Tuple{GmtEditTrack,Dict{String,Vector{Float64}},Dict{String,String},Vector{String}}
	isfile(path) || error("gmtedit: file not found: $path")
	lowercase(splitext(path)[2]) == ".gmt" && return _ge_read_legacy(path, xISdist)
	isempty(vars) && (vars = copy(_GE_DEFAULT_VARS))
	length(vars) == 3 || error("gmtedit: need exactly 3 channel names, got $(length(vars))")

	ds   = _ge_nc_open(path, false)
	root = _shnc_root(ds)
	local track, extraVals, hdr, varlist
	try
		hdr     = _ge_nc_global_attrs(root)
		allvars = _ge_nc_varnames(root)
		nrec    = 0
		for nm in ("lon", "lat")
			a = _ge_nc_array(root, nm)
			a == C_NULL && continue
			nrec = _ge_nc_len(a); _shnc_release_array(a)
			nrec > 0 && break
		end
		nrec > 0 || error("gmtedit: \"$path\" has no lon/lat records")

		# Plottable variables = every 1-D numeric array as long as the record dimension, minus the
		# navigation/text ones (Mirone's variable picker offers exactly the data columns).
		varlist = String[]
		for nm in allvars
			(nm in ("lon", "lat", "time", "id", "sln", "sspn")) && continue
			a = _ge_nc_array(root, nm); a == C_NULL && continue
			ok = (_ge_nc_len(a) == nrec); _shnc_release_array(a)
			ok && push!(varlist, nm)
		end
		push!(varlist, "vel")                          # not a file variable, but a pickable channel

		# --- navigation + the GMT-computed derived columns, through mgd77list -------------------
		std  = String[v for v in unique(vcat(vars, extra)) if v in _GE_MGD77_COLS]
		cols = vcat(["lon", "lat", "rtime", "dist", "vel"], std)
		got  = try
			_ge_mgd77list(path, cols)
		catch e
			@tool_error "gmtedit: mgd77list failed, falling back to the netCDF variables" path exception=(e,)
			nothing
		end
		if got !== nothing && length(got["lon"]) != nrec
			# The two sources must be index-aligned for a save to land on the right records.
			@warn "gmtedit: mgd77list returned $(length(got["lon"])) rows for $nrec netCDF records — using the netCDF variables only" path
			got = nothing
		end

		lon = got === nothing ? _ge_nc_read_scaled(root, "lon") : got["lon"]
		lat = got === nothing ? _ge_nc_read_scaled(root, "lat") : got["lat"]
		tim = got === nothing ? _ge_nc_read_scaled(root, "time") : got["rtime"]
		if got === nothing
			# No mgd77list -> derive the abscissa with the project's ONE geodesic distance function
			# (measure.jl `_seg_dist_azim`, the same one the "Line length…" menu uses), never a
			# hand-rolled sphere formula (.wolf memory use-gmt-geodesic-not-handrolled).
			inc, _ = _seg_dist_azim(lon, lat, true)
			inc[1] = 0.0
			dist = cumsum(inc)
			vel  = _ge_speed_knots(dist, tim)
		else
			dist = got["dist"]
			vel  = got["vel"]
		end

		# --- the three channels ----------------------------------------------------------------
		ych     = Vector{Vector{Float64}}(undef, 3)
		velSlot = 0
		for k in 1:3
			nm = vars[k]
			if nm == "vel"
				ych[k] = copy(vel); velSlot = k
			elseif got !== nothing && haskey(got, nm)
				ych[k] = got[nm]
			else
				v = _ge_nc_read_scaled(root, nm)
				ych[k] = isempty(v) ? fill(NaN, nrec) : v
			end
			length(ych[k]) == nrec || (ych[k] = _ge_fit_length(ych[k], nrec))
		end

		extraVals = Dict{String,Vector{Float64}}()
		for nm in extra
			v = (nm == "vel")                    ? copy(vel) :
			    (got !== nothing && haskey(got, nm)) ? got[nm] : _ge_nc_read_scaled(root, nm)
			isempty(v) || (extraVals[nm] = _ge_fit_length(v, nrec))
		end

		labels = String[_ge_label(root, v) for v in vars]
		xdata  = xISdist ? copy(dist) : collect(Float64, 1:nrec)
		track = GmtEditTrack(path, nrec, lon, lat, tim, dist, vel, xdata, copy(vars), labels, ych,
		                      _ge_hdr_int(hdr, "Survey_Departure_Year"),
		                      _ge_hdr_int(hdr, "Survey_Departure_Month"),
		                      get(hdr, "Source_Institution", ""), xISdist, velSlot, false,
		                      got !== nothing, false)
	finally
		_ge_nc_close(ds)
	end
	return (track, extraVals, hdr, varlist)
end

# The legacy *.gmt branch of `_ge_read`. The format carries exactly three channels, so there is no
# variable list to choose from and no -V to honour; the distance still comes from the project's ONE
# geodesic (`_seg_dist_azim`, measure.jl) rather than gmtlist's own Flat-Earth accumulation, so a
# legacy and an MGD77+ cruise are measured the same way.
function _ge_read_legacy(path::String, xISdist::Bool)
	L = _ge_gmt_read(path)                                 # gmtedit's own -G default: [-180,180]
	inc, _ = _seg_dist_azim(L.lon, L.lat, true)
	inc[1] = 0.0
	dist = cumsum(inc)
	vel  = _ge_speed_knots(dist, L.time)
	vars   = ["gravity", "magnetics", "topography"]
	labels = ["Gravity anomaly (mGal)", "Magnetic field (nT)", "Bathymetry (m)"]
	ych    = Vector{Vector{Float64}}([L.grav, L.mag, L.topo])
	xdata  = xISdist ? copy(dist) : collect(Float64, 1:L.n)
	# The header only carries the start YEAR; month/day come from the first record's own clock.
	_, mo, dy = _ge_gmt_date(Int(round(L.time[1])), L.year)
	hdr = Dict{String,String}("Source_Institution"      => L.agency,
	                           "Survey_Departure_Year"  => string(L.year),
	                           "Survey_Departure_Month" => string(mo),
	                           "Survey_Departure_Day"   => string(dy))
	if L.n > 1
		ey, em, ed = _ge_gmt_date(Int(round(L.time[end])), L.year)
		hdr["Survey_Arrival_Year"]  = string(ey)
		hdr["Survey_Arrival_Month"] = string(em)
		hdr["Survey_Arrival_Day"]   = string(ed)
	end
	tr = GmtEditTrack(path, L.n, L.lon, L.lat, L.time, dist, vel, xdata, vars, labels, ych,
	                   L.year, mo, L.agency, xISdist, 0, false, false, true)
	return (tr, Dict{String,Vector{Float64}}(), hdr, copy(vars))
end

# Pad/truncate a variable to the record count (a malformed file must not take the window down).
function _ge_fit_length(v::Vector{Float64}, n::Int)::Vector{Float64}
	length(v) == n && return v
	out = fill(NaN, n)
	m = min(n, length(v))
	@inbounds out[1:m] = v[1:m]
	return out
end

_ge_hdr_int(hdr::Dict{String,String}, key::String)::Int =
	(v = tryparse(Int, strip(get(hdr, key, ""))); v === nothing ? 0 : v)

# Ship speed in knots from along-track km + time in seconds — gmtedit.m's own conversion
# (`* 1e3 / 1852 * 3600`), used ONLY on the fallback path where mgd77list (and therefore its own
# `vel` column) was unavailable.
function _ge_speed_knots(dist::Vector{Float64}, tim::Vector{Float64})::Vector{Float64}
	n = length(dist)
	(n < 2 || length(tim) != n) && return fill(NaN, n)
	v = fill(NaN, n)
	@inbounds for i in 2:n
		dt = tim[i] - tim[i-1]
		v[i] = dt == 0 ? NaN : (dist[i] - dist[i-1]) / dt * 1e3 / 1852 * 3600
	end
	(n >= 2) && (v[1] = v[2])
	return v
end

# ---------------------------------------------------------------------------------------------
#  cubic smoothing spline (the outlier detector's engine)
# ---------------------------------------------------------------------------------------------
#
# gmtedit_outliersdetect compares the data against `csaps(x, y, p, x)` and flags every point whose
# residual reaches the threshold. `csaps` is MATLAB's cubic SMOOTHING spline: it minimises
#   p * Σ w_i (y_i - f(x_i))^2  +  (1-p) * ∫ f''(t)^2 dt
# and is implemented (de Boor, "A Practical Guide to Splines", ch. XIV) by solving
#   (6(1-p) Q'W⁻¹Q + p R) u = Q'y      then     f = y - 6(1-p) W⁻¹ Q u
# with R the (n-2)x(n-2) tridiagonal 2(dx_i+dx_{i+1}) matrix and Q the second-difference operator.
# That is what the two functions below do, with unit weights. No smoothing-spline routine exists in
# GMT.jl or in this package, and Project.toml may not gain a dependency, so it is written out here
# rather than approximated with a different filter (the p / threshold numbers gmtedit.m ships are
# calibrated for THIS spline).

# In-place Cholesky A = U'U of a symmetric positive-definite matrix with half-bandwidth 2, stored
# as its main / 1st-super / 2nd-super diagonals. Returns false if A is not positive definite.
function _ge_band_chol!(b0::Vector{Float64}, b1::Vector{Float64}, b2::Vector{Float64})::Bool
	m = length(b0)
	@inbounds for i in 1:m
		s = b0[i]
		(i >= 2) && (s -= b1[i-1]^2)
		(i >= 3) && (s -= b2[i-2]^2)
		s <= 0 && return false
		b0[i] = sqrt(s)
		if i + 1 <= m
			t = b1[i]
			(i >= 2) && (t -= b1[i-1] * b2[i-1])
			b1[i] = t / b0[i]
		end
		(i + 2 <= m) && (b2[i] = b2[i] / b0[i])
	end
	return true
end

# Solve U'U x = rhs for the factor produced by _ge_band_chol! (forward then back substitution).
function _ge_band_solve(b0::Vector{Float64}, b1::Vector{Float64}, b2::Vector{Float64},
                        rhs::Vector{Float64})::Vector{Float64}
	m = length(b0)
	z = Vector{Float64}(undef, m)
	@inbounds for i in 1:m
		s = rhs[i]
		(i >= 2) && (s -= b1[i-1] * z[i-1])
		(i >= 3) && (s -= b2[i-2] * z[i-2])
		z[i] = s / b0[i]
	end
	x = Vector{Float64}(undef, m)
	@inbounds for i in m:-1:1
		s = z[i]
		(i + 1 <= m) && (s -= b1[i] * x[i+1])
		(i + 2 <= m) && (s -= b2[i] * x[i+2])
		x[i] = s / b0[i]
	end
	return x
end

# Build R (tridiagonal) and Q'Q (pentadiagonal, half-bandwidth 2) for knots `x`, returned as their
# diagonal triples, together with Q'y.
function _ge_csaps_system(x::Vector{Float64}, y::Vector{Float64})
	n  = length(x)
	m  = n - 2
	dx = diff(x)
	odx = 1.0 ./ dx
	r0 = Vector{Float64}(undef, m); r1 = zeros(m); r2 = zeros(m)
	q0 = Vector{Float64}(undef, m); q1 = zeros(m); q2 = zeros(m)
	qty = Vector{Float64}(undef, m)
	# Row i of Q' (i = 1..m) has entries  odx[i], -(odx[i]+odx[i+1]), odx[i+1]  at columns i,i+1,i+2.
	a = Vector{Float64}(undef, m); b = Vector{Float64}(undef, m); c = Vector{Float64}(undef, m)
	@inbounds for i in 1:m
		a[i] = odx[i]
		b[i] = -(odx[i] + odx[i+1])
		c[i] = odx[i+1]
		qty[i] = a[i] * y[i] + b[i] * y[i+1] + c[i] * y[i+2]
		r0[i]  = 2.0 * (dx[i] + dx[i+1])
	end
	@inbounds for i in 1:m-1
		r1[i] = dx[i+1]
	end
	@inbounds for i in 1:m                              # (Q'Q)[i,i], [i,i+1], [i,i+2]
		q0[i] = a[i]^2 + b[i]^2 + c[i]^2
		(i + 1 <= m) && (q1[i] = b[i] * a[i+1] + c[i] * b[i+1])
		(i + 2 <= m) && (q2[i] = c[i] * a[i+2])
	end
	return (r0, r1, r2, q0, q1, q2, qty, a, b, c)
end

"""
    _ge_csaps(x, y, p) -> Vector{Float64}

Cubic **smoothing spline** evaluated at the data abscissae — the `csaps(x, y, p, x)` of MATLAB's
Curve Fitting toolbox, which gmtedit.m's outlier detector compares the data against. `p` runs from
0 (least-squares straight line) to 1 (interpolation). `x` must be strictly increasing and neither
vector may contain NaN.
"""
function _ge_csaps(x::Vector{Float64}, y::Vector{Float64}, p::Float64)::Vector{Float64}
	n = length(x)
	n < 4 && return copy(y)
	p = clamp(p, 0.0, 1.0)
	p >= 1.0 && return copy(y)
	r0, r1, r2, q0, q1, q2, qty, a, b, c = _ge_csaps_system(x, y)
	m  = n - 2
	w1 = 6.0 * (1.0 - p)
	d0 = Vector{Float64}(undef, m); d1 = Vector{Float64}(undef, m); d2 = Vector{Float64}(undef, m)
	@inbounds for i in 1:m
		d0[i] = w1 * q0[i] + p * r0[i]
		d1[i] = w1 * q1[i] + p * r1[i]
		d2[i] = w1 * q2[i] + p * r2[i]
	end
	_ge_band_chol!(d0, d1, d2) || return copy(y)
	u = _ge_band_solve(d0, d1, d2, qty)
	f = copy(y)                                        # f = y - 6(1-p) Q u
	@inbounds for i in 1:m
		f[i]   -= w1 * a[i] * u[i]
		f[i+1] -= w1 * b[i] * u[i]
		f[i+2] -= w1 * c[i] * u[i]
	end
	return f
end

"""
    _ge_csaps_autop(x, y) -> Float64

The smoothing parameter `csaps` picks when none is given, `p = 1/(1 + trace(R)/(6 trace(Q'WQ)))`
(de Boor's balance between the roughness and the residual term). gmtedit.m prefills the "Smoothing
parameter (p)" box with it, per channel.
"""
function _ge_csaps_autop(x::Vector{Float64}, y::Vector{Float64})::Float64
	length(x) < 4 && return 1.0
	r0, _, _, q0, _, _, _, _, _, _ = _ge_csaps_system(x, y)
	tq = sum(q0)
	tq <= 0 && return 1.0
	return 1.0 / (1.0 + sum(r0) / (6.0 * tq))
end

# The finite, strictly-increasing subset of (x,y) a spline can be fitted to, plus the original
# indices it came from.
function _ge_finite_pairs(x::Vector{Float64}, y::Vector{Float64})
	idx = Int[]
	last = -Inf
	@inbounds for i in eachindex(x)
		(isfinite(x[i]) && isfinite(y[i]) && x[i] > last) || continue
		push!(idx, i); last = x[i]
	end
	return (idx, x[idx], y[idx])
end

# ---------------------------------------------------------------------------------------------
#  not-a-knot cubic spline through a few points, evaluated at one abscissa (despika's interp1)
# ---------------------------------------------------------------------------------------------

"""
    _ge_spline_at(x, y, xq) -> Float64

Value at `xq` of the **not-a-knot cubic spline** through `(x, y)` — the `interp1(Xs, Ys, x(n),
'spline')` gmtedit.m's `despika` uses to relocate a spike onto the position its neighbours imply.
`x` must be strictly increasing; returns NaN with fewer than 4 points.
"""
function _ge_spline_at(x::Vector{Float64}, y::Vector{Float64}, xq::Float64)::Float64
	n = length(x)
	n < 4 && return NaN
	h = diff(x)
	# Second derivatives M from the standard tridiagonal system, closed with the not-a-knot end
	# conditions (third derivative continuous across the 2nd and the (n-1)th knot). n is small
	# (gmtedit.m feeds at most 8 neighbours), so a plain dense elimination is enough.
	A = zeros(n, n); r = zeros(n)
	A[1, 1] = h[2];  A[1, 2] = -(h[1] + h[2]);  A[1, 3] = h[1]
	A[n, n-2] = h[n-1];  A[n, n-1] = -(h[n-2] + h[n-1]);  A[n, n] = h[n-2]
	for i in 2:n-1
		A[i, i-1] = h[i-1]
		A[i, i]   = 2 * (h[i-1] + h[i])
		A[i, i+1] = h[i]
		r[i]      = 6 * ((y[i+1] - y[i]) / h[i] - (y[i] - y[i-1]) / h[i-1])
	end
	M = try
		A \ r
	catch
		return NaN
	end
	i = searchsortedlast(x, xq)
	i = clamp(i, 1, n - 1)
	t = xq - x[i]
	return y[i] + t * ((y[i+1] - y[i]) / h[i] - h[i] * (2 * M[i] + M[i+1]) / 6) +
	       t^2 * M[i] / 2 + t^3 * (M[i+1] - M[i]) / (6 * h[i])
end

# ---------------------------------------------------------------------------------------------
#  the analyses the toolbar buttons run
# ---------------------------------------------------------------------------------------------

"""
    _ge_outliers(x, y, p, thresh) -> Vector{Bool}

gmtedit_outliersdetect's `push_Apply`: smooth `y` with a cubic smoothing spline of parameter `p`
and flag every point whose `|y - smooth|` reaches `thresh`. NaN points are never flagged.
"""
function _ge_outliers(x::Vector{Float64}, y::Vector{Float64}, p::Float64, thresh::Float64)::Vector{Bool}
	flags = falses(length(y))
	idx, xf, yf = _ge_finite_pairs(x, y)
	length(idx) < 4 && return collect(flags)
	sm = _ge_csaps(xf, yf, p)
	@inbounds for k in eachindex(idx)
		(abs(yf[k] - sm[k]) >= thresh) && (flags[idx[k]] = true)
	end
	return collect(flags)
end

"""
    _ge_navfilter(x, y, tim, minSpeed, maxSpeed, maxSlope) -> Vector{Bool}

gmtedit_NavFilters' `push_navFiltApply`: flag records whose implied ship speed falls outside
`[minSpeed, maxSpeed]` knots **while real data exists there**, plus records where the along-track
gradient `|dy/dx|` exceeds `maxSlope` (nT/km). `x` is the abscissa in km, `tim` the record times in
seconds. As in gmtedit.m the flag lands on the LATER record of each pair.
"""
function _ge_navfilter(x::Vector{Float64}, y::Vector{Float64}, tim::Vector{Float64},
                       minSpeed::Float64, maxSpeed::Float64, maxSlope::Float64)::Vector{Bool}
	n = length(x)
	flags = falses(n)
	(n < 2 || length(y) != n) && return collect(flags)
	hasT = (length(tim) == n)
	@inbounds for i in 2:n
		bad = false
		if hasT
			dt = tim[i] - tim[i-1]
			if dt != 0 && isfinite(dt)
				vel = (x[i] - x[i-1]) / dt * (1000 / 1852 * 3600)   # km & s -> knots (gmtedit.m)
				(isfinite(y[i]) && (vel < minSpeed || vel > maxSpeed)) && (bad = true)
			end
		end
		dxx = x[i] - x[i-1]
		if !bad && dxx != 0 && isfinite(y[i]) && isfinite(y[i-1])
			(abs((y[i] - y[i-1]) / dxx) > maxSlope) && (bad = true)
		end
		flags[i] = bad
	end
	return collect(flags)
end

"""
    _ge_despike(x, y, n) -> Float64

gmtedit.m's `despika`: the value a not-a-knot cubic spline through the (up to) eight neighbours of
record `n` predicts at `x[n]`, with the spiky point itself and any NaNs left out. NaN when there
are too few usable neighbours.
"""
function _ge_despike(x::Vector{Float64}, y::Vector{Float64}, n::Int)::Float64
	(n < 1 || n > length(x)) && return NaN
	i1 = max(n - 4, 1); i2 = min(n + 4, length(x))
	xs = Float64[]; ys = Float64[]
	for i in i1:i2
		(i == n) && continue
		(isfinite(x[i]) && isfinite(y[i])) || continue
		(!isempty(xs) && x[i] <= xs[end]) && continue
		push!(xs, x[i]); push!(ys, y[i])
	end
	length(xs) < 4 && return NaN
	return _ge_spline_at(xs, ys, x[n])
end

"""
    _ge_despike_nav!(tr, n) -> Union{Nothing,Tuple{Float64,Float64}}

gmtedit.m's `despikeNav`: replace record `n`'s coordinates with the midpoint of its neighbours,
when the surrounding nav is regular enough for that to mean anything. Mutates `tr.lon`/`tr.lat`
(and sets `navEdited`, so a save also rewrites them) and returns the new `(lon, lat)`; returns
`nothing` when the edit is refused, exactly where gmtedit.m puts up its warndlg.
"""
function _ge_despike_nav!(tr::GmtEditTrack, n::Int)
	(n <= 1 || n >= tr.n) && return nothing            # first/last cannot be recalculated
	i1 = max(n - 3, 1); i2 = min(n + 3, tr.n)
	lon = @view tr.lon[i1:i2]; lat = @view tr.lat[i1:i2]
	meanDLon = abs(lon[end] - lon[1]) / (length(lon) - 1)
	meanDLat = abs(lat[end] - lat[1]) / (length(lat) - 1)
	lonInt = tr.lon[n+1] - tr.lon[n-1]
	latInt = tr.lat[n+1] - tr.lat[n-1]
	(lonInt > 4 * meanDLon || latInt > 4 * meanDLat) && return nothing
	tr.lon[n] = tr.lon[n-1] + lonInt / 2
	tr.lat[n] = tr.lat[n-1] + latInt / 2
	tr.navEdited = true
	return (tr.lon[n], tr.lat[n])
end

# ---------------------------------------------------------------------------------------------
#  saving
# ---------------------------------------------------------------------------------------------

"""
    _ge_save(tr, flags, yvals; path="") -> String

Write the edited channels back into the MGD77+ netCDF file (gmtedit.m's `save_clickedCB`, netCDF
branch). `flags[k]` marks the points the user painted red in panel k and `yvals[k]` holds that
panel's CURRENT values (which differ from the file's when a segment was dragged or a spike was
relocated). A flagged point is stored as the variable's own `missing_value`; every other value is
divided by its `scale_factor` first, since GDAL writes RAW storage values.

A channel is written only when it actually changed — gmtedit.m's `saveGRAV`/`saveMAG`/`saveTOPO`
guard, so opening a cruise and saving it untouched cannot rewrite (or round-trip) data. `path`
saves to a copy instead of in place. Returns the path written; throws on failure.
"""
function _ge_save(tr::GmtEditTrack, flags::Vector{Vector{Bool}}, yvals::Vector{Vector{Float64}};
                  path::String="")::String
	tr.islegacy && return _ge_save_legacy(tr, flags, yvals, isempty(path) ? tr.path : path)
	out = isempty(path) ? tr.path : path
	if out != tr.path
		cp(tr.path, out; force=true)
	end
	ds   = _ge_nc_open(out, true)
	root = _shnc_root(ds)
	nwritten = 0
	try
		for k in 1:3
			nm = tr.vars[k]
			(k == tr.velSlot || nm == "vel") && continue          # speed is not a file variable
			fl = k <= length(flags) ? flags[k] : Bool[]
			yv = k <= length(yvals) ? yvals[k] : Float64[]
			length(yv) == tr.n || continue
			changed = any(fl) || any(i -> !_ge_same(yv[i], tr.y[k][i]), 1:tr.n)
			changed || continue
			scale, miss, isint = _ge_nc_scaling(root, nm)
			isnan(miss) && error("gmtedit: \"$nm\" has no missing_value attribute — refusing to guess one")
			raw = Vector{Float64}(undef, tr.n)
			@inbounds for i in 1:tr.n
				v = yv[i]
				bad = (i <= length(fl) && fl[i]) || !isfinite(v)
				raw[i] = bad ? miss : (isint ? round(v / scale) : v / scale)
			end
			_ge_nc_write_raw!(root, nm, raw)
			tr.y[k] = copy(yv)                                     # the file now matches the window
			nwritten += 1
		end
		if tr.navEdited                                            # despikeNav moved coordinates
			for (nm, v) in (("lon", tr.lon), ("lat", tr.lat))
				scale, _, isint = _ge_nc_scaling(root, nm)
				_ge_nc_write_raw!(root, nm, [isint ? round(x / scale) : x / scale for x in v])
			end
			tr.navEdited = false
			nwritten += 1
		end
	finally
		_ge_nc_close(ds)
	end
	nwritten == 0 && error("gmtedit: nothing to save — no channel was edited")
	return out
end

_ge_same(a::Float64, b::Float64) = (isnan(a) && isnan(b)) || a == b

# Apply the window's red flags to a channel: a flagged (or already non-finite) record becomes NaN,
# which every writer here then turns into that format's own no-data value. gmtedit.m does this with
# its `y_g((x_g - x_gn(k)) == 0) = NaN` loops.
function _ge_blank_flagged(y::Vector{Float64}, fl::Vector{Bool})::Vector{Float64}
	out = copy(y)
	@inbounds for i in eachindex(out)
		((i <= length(fl) && fl[i]) || !isfinite(out[i])) && (out[i] = NaN)
	end
	return out
end

# Legacy *.gmt save. The format has exactly three channels in a fixed order, so unlike the MGD77+
# branch there is no per-variable "was it edited?" gate — the whole file is rewritten (as
# gmtedit.m's own `fwrite` loop does).
function _ge_save_legacy(tr::GmtEditTrack, flags::Vector{Vector{Bool}},
                         yvals::Vector{Vector{Float64}}, out::String)::String
	ch = [k <= length(yvals) && length(yvals[k]) == tr.n ? yvals[k] : tr.y[k] for k in 1:3]
	fl = [k <= length(flags) && length(flags[k]) == tr.n ? flags[k] : falses(tr.n) for k in 1:3]
	g, m, t = (_ge_blank_flagged(ch[k], collect(fl[k])) for k in 1:3)
	_ge_gmt_write(out, tr.year, tr.agency, tr.time, tr.lon, tr.lat, g, m, t)
	for k in 1:3
		tr.y[k] = copy(ch[k])
	end
	tr.navEdited = false
	return out
end

"""
    _ge_save_as_gmt(tr, flags, yvals, out) -> String

gmtedit.m's `force_gmt` branch of `save_clickedCB`: write an MGD77+ cruise out as a legacy `*.gmt`
binary. Two conversions come straight from that branch:

  * magnetics `y_m = y_m - 40000` — an MGD77+ `mtf1` is a TOTAL field of ~50000 nT, which does not
    fit an `int16`; subtracting 40000 puts it back in the anomaly-sized range the old format holds.
  * time — the netCDF `time` variable is seconds since 1970, the `*.gmt` record time is seconds
    since Jan 1 of the cruise year, so the epoch difference is removed.

The channels are taken from whichever panels currently hold `faa` / `mtf1` / `depth`; a panel
showing anything else (an extended column, ship speed) has no slot in this format and is skipped,
with its channel written as all-no-data.
"""
function _ge_save_as_gmt(tr::GmtEditTrack, flags::Vector{Vector{Bool}},
                         yvals::Vector{Vector{Float64}}, out::String)::String
	tr.islegacy && return _ge_save_legacy(tr, flags, yvals, out)
	nan3 = fill(NaN, tr.n)
	pick = Dict{String,Vector{Float64}}()
	for k in 1:3
		y  = (k <= length(yvals) && length(yvals[k]) == tr.n) ? yvals[k] : tr.y[k]
		f  = (k <= length(flags) && length(flags[k]) == tr.n) ? collect(flags[k]) : falses(tr.n)
		pick[tr.vars[k]] = _ge_blank_flagged(y, f)
	end
	g = get(pick, "faa",   nan3)
	m = get(pick, "mtf1",  nan3)
	t = get(pick, "depth", nan3)
	haskey(pick, "mtf1") && (m = m .- 40000.0)             # gmtedit.m: total field -> int16 range
	# The MGD77+ `time` variable is seconds since 1970 (gmtedit.m assumes exactly this in its own
	# `tempo - (date2jd(year) - date2jd(1970)) * 86400`); the *.gmt record time is seconds since
	# Jan 1 of the cruise year.
	yr   = tr.year > 0 ? tr.year : 1970
	base = GMT.Dates.datetime2unix(GMT.Dates.DateTime(yr, 1, 1))
	tim  = (length(tr.time) == tr.n) ? (tr.time .- base) : collect(Float64, 0:tr.n-1)
	_ge_gmt_write(out, yr, tr.agency, tim, tr.lon, tr.lat, g, m, t)
	return out
end

# ---------------------------------------------------------------------------------------------
#  the Cruise Info report
# ---------------------------------------------------------------------------------------------

# gmtedit.m's info_clickedCB shows N_recs / N_grav / N_mag / N_topo, the W/E/S/N box and the start
# and end dates + the agency. Those counts and limits come from the data itself; the dates and the
# agency from the MGD77+ header. The full header follows, so an MGD77+ file's own metadata (which
# Mirone could not show at all) is one scroll away.
function _ge_info_text(tr::GmtEditTrack, hdr::Dict{String,String})::String
	cnt(v) = count(isfinite, v)
	iv(k)  = _ge_hdr_int(hdr, k)
	lonf = filter(isfinite, tr.lon); latf = filter(isfinite, tr.lat)
	io = IOBuffer()
	println(io, "N_recs = ", tr.n,
	            ", N_", tr.vars[1], " = ", cnt(tr.y[1]),
	            ", N_", tr.vars[2], " = ", cnt(tr.y[2]),
	            ", N_", tr.vars[3], " = ", cnt(tr.y[3]))
	if !isempty(lonf)
		println(io, "W: = ", minimum(lonf), "  E: = ", maximum(lonf))
		println(io, "S: = ", minimum(latf), "  N: = ", maximum(latf))
	end
	println(io, "Start day,month,year: = ", iv("Survey_Departure_Day"), "  ",
	            iv("Survey_Departure_Month"), "  ", iv("Survey_Departure_Year"))
	println(io, "End   day,month,year: = ", iv("Survey_Arrival_Day"), "  ",
	            iv("Survey_Arrival_Month"), "  ", iv("Survey_Arrival_Year"))
	println(io)
	println(io, get(hdr, "Source_Institution", ""))
	println(io, "\n--- MGD77+ header ---")
	for k in sort(collect(keys(hdr)))
		k == "history" && continue                    # a long conversion log, not cruise metadata
		v = strip(hdr[k])
		isempty(v) && continue
		println(io, rpad(k, 44), " = ", v)
	end
	return String(take!(io))
end

# ---------------------------------------------------------------------------------------------
#  pushing a loaded track into the window
# ---------------------------------------------------------------------------------------------

_ge_flag_i32(f::Vector{Bool}) = Cint[b ? 1 : 0 for b in f]

# Hand the window everything it needs to draw: abscissa, the three channels with their labels, the
# variable list its per-panel pull-downs offer, and the window title.
function _ge_push_track(edit::Ptr{Cvoid}, tr::GmtEditTrack, varlist::Vector{String})
	ccall(_fn(:gmtvtk_gmtedit_set_title), Cvoid, (Ptr{Cvoid}, Cstring), edit,
	      "gmtedit  " * basename(tr.path))
	ccall(_fn(:gmtvtk_gmtedit_set_varlist), Cvoid, (Ptr{Cvoid}, Cstring), edit, join(varlist, ','))
	ccall(_fn(:gmtvtk_gmtedit_set_x), Cvoid, (Ptr{Cvoid}, Ptr{Float64}, Cint, Cint),
	      edit, tr.xdata, Cint(tr.n), Cint(tr.xISdist ? 1 : 0))
	for k in 1:3
		ccall(_fn(:gmtvtk_gmtedit_set_channel), Cvoid,
		      (Ptr{Cvoid}, Cint, Cstring, Cstring, Ptr{Float64}, Cint),
		      edit, Cint(k - 1), tr.vars[k], tr.labels[k], tr.y[k], Cint(tr.n))
	end
	return
end

# Pull one panel's CURRENT state back out of the window: the red flags and the (possibly dragged or
# despiked) values, in record order.
function _ge_pull_channel(edit::Ptr{Cvoid}, slot::Int, n::Int)::Tuple{Vector{Bool},Vector{Float64}}
	fl = Vector{Cint}(undef, n)
	got = ccall(_fn(:gmtvtk_gmtedit_get_flags), Cint, (Ptr{Cvoid}, Cint, Ptr{Cint}, Cint),
	            edit, Cint(slot - 1), fl, Cint(n))
	flags = got == n ? Bool[f != 0 for f in fl] : falses(n)
	yv = Vector{Float64}(undef, n)
	gy = ccall(_fn(:gmtvtk_gmtedit_get_channel), Cint, (Ptr{Cvoid}, Cint, Ptr{Float64}, Cint),
	           edit, Cint(slot - 1), yv, Cint(n))
	return (collect(flags), gy == n ? yv : fill(NaN, n))
end

function _ge_log(edit::Ptr{Cvoid}, msg::AbstractString; err::Bool=false)
	err && _record_tool_error(msg)      # same failure sink _viewer_log_error feeds
	ccall(_fn(:gmtvtk_gmtedit_log), Cvoid, (Ptr{Cvoid}, Cstring, Cint), edit, String(msg), Cint(err))
end

_ge_message(edit::Ptr{Cvoid}, title::AbstractString, text::AbstractString) =
	ccall(_fn(:gmtvtk_gmtedit_message), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), edit, String(title), String(text))

# ---------------------------------------------------------------------------------------------
#  the C -> Julia callback
# ---------------------------------------------------------------------------------------------

# Per-window scratch the actions need but the track itself does not own: the file header (Info) and
# the variable list (the pull-downs).
const _GE_HDR  = Dict{Ptr{Cvoid},Dict{String,String}}()
const _GE_VARS = Dict{Ptr{Cvoid},Vector{String}}()
# The 3-D viewer window an editor was opened FROM, if any — gmtedit.m's `hMirAxes`, which it gets
# when draw_funs launches it off a plotted cruise track. Only the link tool needs it.
const _GE_PARENT = Dict{Ptr{Cvoid},Ptr{Cvoid}}()

# Load `path` into the window `edit`, honouring the OPTcontrol-style -V switch.
function _ge_load!(edit::Ptr{Cvoid}, path::String)
	vars, overlays, xISdist = _ge_parse_optv(_ge_ini_optv())
	extra = String[nm for (nm, _) in overlays]
	tr, extraVals, hdr, varlist = _ge_read(path, vars, xISdist, extra)
	_GE_REG[edit]  = tr
	_GE_HDR[edit]  = hdr
	_GE_VARS[edit] = varlist
	_ge_push_track(edit, tr, varlist)
	for (nm, slot) in overlays                        # -V's varI+slotI extra curves
		v = get(extraVals, nm, Float64[])
		length(v) == tr.n || continue
		ccall(_fn(:gmtvtk_gmtedit_add_overlay), Cvoid,
		      (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint, Cstring, Cdouble, Cdouble, Cdouble, Cdouble),
		      edit, Cint(slot - 1), tr.xdata, v, Cint(tr.n), nm, 0.0, 0.0, 0.9, 1.5)
	end
	_ge_log(edit, "Loaded $(basename(path)): $(tr.n) records  [$(join(tr.vars, ", "))]")
	tr.distFromGMT || _ge_log(edit, "mgd77list could not read this path (GMT's mgd77 suite rejects " *
	                                "long/UNC cruise paths); data and distances came from the netCDF " *
	                                "variables + the geodesic used by the Line length tool.")
	return
end

# Replace ONE panel's channel with another variable of the same file (the panel pull-down, and the
# "Overlay another variable" context item when `overlay` is true).
function _ge_setvar!(edit::Ptr{Cvoid}, slot::Int, name::String, overlay::Bool)
	tr = get(_GE_REG, edit, nothing)
	tr === nothing && return
	local v::Vector{Float64}, lab::String
	if name == "vel"
		v = copy(tr.vel); lab = _GE_LABELS["vel"]
	else
		ds = _ge_nc_open(tr.path, false); root = _shnc_root(ds)
		try
			v   = _ge_nc_read_scaled(root, name)
			lab = _ge_label(root, name)
			isempty(v) || (v = _ge_fit_length(v, tr.n))
		finally
			_ge_nc_close(ds)
		end
	end
	isempty(v) && (_ge_log(edit, "Variable \"$name\" not found in $(basename(tr.path))"; err=true); return)
	if overlay
		ccall(_fn(:gmtvtk_gmtedit_add_overlay), Cvoid,
		      (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint, Cstring, Cdouble, Cdouble, Cdouble, Cdouble),
		      edit, Cint(slot - 1), tr.xdata, v, Cint(tr.n), name, 0.0, 0.0, 0.9, 1.5)
	else
		tr.vars[slot]   = name
		tr.labels[slot] = lab
		tr.y[slot]      = v
		tr.velSlot      = (name == "vel") ? slot : (tr.velSlot == slot ? 0 : tr.velSlot)
		ccall(_fn(:gmtvtk_gmtedit_set_channel), Cvoid,
		      (Ptr{Cvoid}, Cint, Cstring, Cstring, Ptr{Float64}, Cint),
		      edit, Cint(slot - 1), name, lab, v, Cint(tr.n))
	end
	return
end

# Sample a grid along the cruise and overlay it on one panel (gmtedit_track's push_OK_CB:
# grdtrack + an optional IGRF). The IGRF comes from GMT.magref, the SAME source igrf.jl uses.
function _ge_interp_grid(edit::Ptr{Cvoid}, slot::Int, gridfile::String, addIGRF::Bool)
	tr = get(_GE_REG, edit, nothing)
	tr === nothing && return
	isfile(gridfile) || (_ge_log(edit, "Grid not found: $gridfile"; err=true); return)
	G  = _gmtread_trb(gridfile)      # grids are READ in "TRB" — THE reader
	D  = GMT.grdtrack(G, hcat(tr.lon, tr.lat))
	zz = Vector{Float64}(@view (D isa AbstractVector ? first(D) : D).data[:, end])
	if addIGRF
		yr = tr.year > 0 ? tr.year + 0.5 : 2020.0     # gmtedit_track: "Date doesn't need to be very accurate here"
		# Same call igrf.jl's file path uses: last output column = Total Field.
		F  = GMT.magref(hcat(tr.lon, tr.lat); alt=0.0, onetime=yr, T=true)
		Fd = F isa AbstractVector ? first(F) : F
		fv = Vector{Float64}(@view Fd.data[:, end])
		(length(fv) == length(zz)) && (zz .+= fv)
	end
	ccall(_fn(:gmtvtk_gmtedit_add_overlay), Cvoid,
	      (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint, Cstring, Cdouble, Cdouble, Cdouble, Cdouble),
	      edit, Cint(slot - 1), tr.xdata, zz, Cint(tr.n), basename(gridfile), 0.0, 0.0, 1.0, 2.0)
	_ge_log(edit, "Overlaid $(basename(gridfile))" * (addIGRF ? " + IGRF" : "") * " on panel $slot")
	return
end

# gmtedit.m's "Show in XY grapher": send one panel's data to the X,Y plot tool, either whole or
# only what the window currently shows. Bathymetry goes over with its sign flipped, as in
# move_to_ecran. The X,Y tool is this project's `ecran` (xyplot.jl) — one tool, a new page.
function _ge_to_xyplot(edit::Ptr{Cvoid}, slot::Int, whole::Bool, xlo::Float64, xhi::Float64)
	tr = get(_GE_REG, edit, nothing)
	tr === nothing && return
	_, yv = _ge_pull_channel(edit, slot, tr.n)
	nm = tr.vars[slot]
	(nm == "depth") && (yv = -yv)
	keep = whole ? collect(1:tr.n) : findall(i -> xlo <= tr.xdata[i] <= xhi, 1:tr.n)
	isempty(keep) && (_ge_log(edit, "Nothing in this window to send to the X,Y plot tool"; err=true); return)
	ttl = "$(nm)_from_$(splitext(basename(tr.path))[1])"
	xyplot(tr.xdata[keep], yv[keep]; name=nm, title=ttl,
	       xlabel=(tr.xISdist ? "Distance (km)" : "Record #"), ylabel=tr.labels[slot])
	return
end

# gmtedit.m's despikeNav: fix a bad fix by interpolating its coordinates, then rebuild everything
# that hangs off them — the along-track distance (hence the abscissa) and the ship speed.
function _ge_despike_nav_action(edit::Ptr{Cvoid}, tr::GmtEditTrack, slot::Int, idx::Int)
	if !tr.xISdist
		_ge_message(edit, "Warning",
		    "Cannot recompute velocity when the X axis holds record numbers instead of distances.")
		return
	end
	if _ge_despike_nav!(tr, idx) === nothing
		_ge_message(edit, "Warning",
		    "Coordinate data around the clicked point does not allow a reasonable estimate of a new " *
		    "Lon/Lat (likely close to a data gap), or it is the first/last record.")
		return
	end
	inc, _ = _seg_dist_azim(tr.lon, tr.lat, true)
	inc[1] = 0.0
	tr.dist  = cumsum(inc)
	tr.vel   = _ge_speed_knots(tr.dist, tr.time)
	tr.xdata = copy(tr.dist)
	ccall(_fn(:gmtvtk_gmtedit_set_x), Cvoid, (Ptr{Cvoid}, Ptr{Float64}, Cint, Cint),
	      edit, tr.xdata, Cint(tr.n), Cint(1))
	for k in 1:3                                       # set_x reset the channels' own abscissae
		ccall(_fn(:gmtvtk_gmtedit_set_channel), Cvoid,
		      (Ptr{Cvoid}, Cint, Cstring, Cstring, Ptr{Float64}, Cint),
		      edit, Cint(k - 1), tr.vars[k], tr.labels[k],
		      (k == tr.velSlot ? tr.vel : tr.y[k]), Cint(tr.n))
	end
	(tr.velSlot > 0) && (tr.y[tr.velSlot] = copy(tr.vel))
	_ge_log(edit, "Recomputed nav at record $idx (panel $slot)")
	return
end

"""
    _ge_open_dropped(scene, path)

Open an MGD77 cruise that arrived through the ONE open-a-file path (`_on_drop`, drop.jl: window
drop, File > Open, File > Recent Files, desktop-icon drop). Two things happen, in this order:

  1. the cruise's NAVIGATION is plotted into the receiving window, through the very same
     `_mgd77_track` + `_add_dataset_to_scene` pair Geophysics > Magnetics > Import *.gmt/*.nc uses
     (a bare launcher is promoted to a blank map framed on the track first, exactly as that
     importer does it) — so the file is still "opened" in the window it was dropped on;
  2. the track editor opens on it, with that window as its parent, so its link tool can send a
     clicked record straight back to the map.

Plotting the track is best-effort: a cruise whose navigation cannot be read still gets its editor.
"""
function _ge_open_dropped(scene::Ptr{Cvoid}, path::String)
	parent = C_NULL
	try
		D = _mgd77_track(path)
		if D !== nothing
			if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
				W, E, S, N = _padded_bbox(_dataset_bbox([D])...)
				_promote_blank_scaffold(scene, W, E, S, N, _isgeog(D) == 1; crsobj=D)
				ccall(_fn(:gmtvtk_set_title_h), Cvoid, (Ptr{Cvoid}, Cstring), scene,
				      "i'GMT -- $(basename(path))")
			end
			_add_dataset_to_scene(scene, D, splitext(basename(path))[1];
			                       color=(rand(), rand(), rand()), noConvertToPoints=true, noDataTable=true)
			_mark_file_open(path, scene, splitext(basename(path))[1])   # the name _add_dataset_to_scene gave the track
			parent = scene
		end
	catch e
		_viewer_log_error(scene, "gmtedit: could not plot the track of \"$(basename(path))\": " *
		                          "$(sprint(showerror, e))  (opening the editor anyway)")
	end
	h = gmtedit(path; parent=(parent == C_NULL ? nothing : parent))
	return h
end

# The one callback the window talks back through. `action` names the operation and `arg` carries a
# ';'-joined payload, exactly the convention `_on_tiles`/`_on_xy` already use.
function _on_gmtedit(edit::Ptr{Cvoid}, caction::Cstring, carg::Cstring)::Cvoid
	action = unsafe_string(caction)
	arg    = unsafe_string(carg)
	try
		f = split(arg, ';')
		gets(i)  = i <= length(f) ? String(f[i]) : ""
		getf(i, d) = (v = tryparse(Float64, gets(i)); v === nothing ? d : v)
		geti(i, d) = (v = tryparse(Int, gets(i)); v === nothing ? d : v)

		if action == "open"
			_ge_load!(edit, gets(1))

		elseif action == "closed"
			delete!(_GE_REG, edit); delete!(_GE_HDR, edit); delete!(_GE_VARS, edit)
			delete!(_GE_PARENT, edit)

		elseif action == "save"
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			flags = Vector{Vector{Bool}}(undef, 3); yv = Vector{Vector{Float64}}(undef, 3)
			for k in 1:3
				flags[k], yv[k] = _ge_pull_channel(edit, k, tr.n)
			end
			out = _ge_save(tr, flags, yv; path=gets(1))
			_ge_log(edit, "Saved $(basename(out))")

		elseif action == "saveas_gmt"                  # gmtedit.m's force_gmt branch
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			out = gets(1)
			isempty(out) && return
			flags = Vector{Vector{Bool}}(undef, 3); yv = Vector{Vector{Float64}}(undef, 3)
			for k in 1:3
				flags[k], yv[k] = _ge_pull_channel(edit, k, tr.n)
			end
			_ge_save_as_gmt(tr, flags, yv, out)
			_ge_log(edit, "Saved $(basename(out)) in the old *.gmt format" *
			              (tr.islegacy ? "" : "  (mtf1 - 40000 nT, time re-based to $(tr.year))"))

		elseif action == "setparent"
			# The C side hands back the parent Scene* as "0x…" (or "0" to detach) — the same opaque
			# handle `_FIGREG` is already keyed on.
			p = tryparse(UInt, gets(1))
			if p === nothing || p == 0
				delete!(_GE_PARENT, edit)
			else
				_GE_PARENT[edit] = Ptr{Cvoid}(p)
			end

		elseif action == "pickpt"                      # the link tool (gmtedit.m ptcoords/bdn_ptcoords)
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			idx = geti(2, 0) + 1
			(1 <= idx <= tr.n) || return
			scene = get(_GE_PARENT, edit, C_NULL)
			if scene == C_NULL
				_ge_log(edit, "Link: this editor has no parent 3-D window"; err=true); return
			end
			if ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), scene) == 0
				delete!(_GE_PARENT, edit)
				_ge_log(edit, "Link: the parent 3-D window was closed"; err=true); return
			end
			# gmtedit.m plots a black filled circle, MarkerSize 6, tagged 'LinkedSymb'. `add_symbols!`
			# (symbols.jl) is this project's ONE "stamp a symbol layer on a scene" primitive.
			add_symbols!(scene, [tr.lon[idx]], [tr.lat[idx]]; symbol=:circle, size=9,
			             fill=:black, edge=:white, edgewidth=1.0,
			             name="$(splitext(basename(tr.path))[1])  rec $idx")
			ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), scene)
			_ge_log(edit, "Link: record $idx  ->  lon $(round(tr.lon[idx], digits=5)), " *
			              "lat $(round(tr.lat[idx], digits=5))")

		elseif action == "info"
			tr  = get(_GE_REG, edit, nothing)
			tr === nothing && return
			hdr = get(_GE_HDR, edit, Dict{String,String}())
			_ge_message(edit, "Cruise Info", _ge_info_text(tr, hdr))

		elseif action == "autop"                       # prefill the outlier dialog's p box
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			slot = clamp(geti(1, 1), 1, 3)
			_, yv = _ge_pull_channel(edit, slot, tr.n)
			_, xf, yf = _ge_finite_pairs(tr.xdata, yv)
			p = length(xf) >= 4 ? _ge_csaps_autop(xf, yf) : 1.0
			ccall(_fn(:gmtvtk_gmtedit_set_autop), Cvoid, (Ptr{Cvoid}, Cint, Cdouble), edit, Cint(slot - 1), p)

		elseif action == "outliers"
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			slot = clamp(geti(1, 1), 1, 3)
			_, yv = _ge_pull_channel(edit, slot, tr.n)
			fl = _ge_outliers(tr.xdata, yv, getf(2, 1.0), getf(3, 4.0))
			ccall(_fn(:gmtvtk_gmtedit_set_flags), Cvoid, (Ptr{Cvoid}, Cint, Ptr{Cint}, Cint),
			      edit, Cint(slot - 1), _ge_flag_i32(fl), Cint(tr.n))
			_ge_log(edit, "Outliers on panel $slot: found $(count(fl))")

		elseif action == "navfilter"
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			slot = clamp(geti(1, 2), 1, 3)
			_, yv = _ge_pull_channel(edit, slot, tr.n)
			fl = _ge_navfilter(tr.xdata, yv, tr.time, getf(2, 1.0), getf(3, 15.0), getf(4, 250.0))
			ccall(_fn(:gmtvtk_gmtedit_set_flags), Cvoid, (Ptr{Cvoid}, Cint, Ptr{Cint}, Cint),
			      edit, Cint(slot - 1), _ge_flag_i32(fl), Cint(tr.n))
			ccall(_fn(:gmtvtk_gmtedit_set_navfound), Cvoid, (Ptr{Cvoid}, Cint), edit, Cint(count(fl)))

		elseif action == "despike" || action == "despikenav"
			tr = get(_GE_REG, edit, nothing)
			tr === nothing && return
			slot = clamp(geti(1, 2), 1, 3); idx = geti(2, 0) + 1
			# gmtedit.m's add_MarkColor routes a Shift-click on the SPEED panel to despikeNav
			# (recompute the coordinates) and every other panel to despika (relocate the value).
			if action == "despikenav" || slot == tr.velSlot
				_ge_despike_nav_action(edit, tr, slot, idx)
				return
			end
			_, yv = _ge_pull_channel(edit, slot, tr.n)
			ny = _ge_despike(tr.xdata, yv, idx)
			isnan(ny) && (_ge_log(edit, "Despike: not enough clean neighbours around record $idx"; err=true); return)
			ccall(_fn(:gmtvtk_gmtedit_set_point), Cvoid, (Ptr{Cvoid}, Cint, Cint, Cdouble),
			      edit, Cint(slot - 1), Cint(idx - 1), ny)

		elseif action == "setvar"
			_ge_setvar!(edit, clamp(geti(1, 1), 1, 3), gets(2), false)

		elseif action == "overlayvar"
			_ge_setvar!(edit, clamp(geti(1, 1), 1, 3), gets(2), true)

		elseif action == "interp"
			_ge_interp_grid(edit, clamp(geti(1, 1), 1, 3), gets(2), gets(3) == "1")

		elseif action == "toxy"
			_ge_to_xyplot(edit, clamp(geti(1, 1), 1, 3), gets(2) != "window", getf(3, -Inf), getf(4, Inf))
		end
	catch e
		_ge_log(edit, "$action FAILED: $(sprint(showerror, e))"; err=true)
		@tool_error "gmtedit: $action failed" arg exception=(e,)
	end
	return
end

function _register_gmtedit()
	fptr = @cfunction((e, a, g) -> Base.invokelatest(_on_gmtedit, e, a, g), Cvoid,
	                  (Ptr{Cvoid}, Cstring, Cstring))
	ccall(_fn(:gmtvtk_set_gmtedit_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ---------------------------------------------------------------------------------------------
#  public entry point
# ---------------------------------------------------------------------------------------------

"""
    gmtedit([file]; width=200.0, center=nothing)

Open the **MGD77 track editor** — a port of Mirone's `gmtedit`. Three stacked panels plot a
cruise's gravity, magnetics and bathymetry (or whatever three variables the `[gmtedit] V=` line of
`~/.gmt/iGMT.ini` names) against along-track distance in km.

Click a point to flag it bad (green → red), click it again to unflag it; drag a rectangle to flag
many at once. Shift-click relocates an isolated spike onto the value a cubic spline through its
neighbours predicts. The toolbar adds an automatic outlier detector (cubic smoothing spline
residuals), a speed/gradient nav filter, and cruise info. Saving writes the surviving values back
into the MGD77+ netCDF file, flagged points becoming that variable's `missing_value`.

`file` is an MGD77+ netCDF cruise (`.nc`) or a legacy pre-MGD77 `*.gmt` binary; with no argument the
window opens empty and the file is picked from its toolbar. `width` is the displayed window width
in km (gmtedit's `-L`); `center` is a `(lon, lat)` the display should start centred on (gmtedit's
`-P`). `parent` is a 3-D viewer handle (a `QtFigure`, or the window this cruise track is plotted
in): it enables the toolbar's **link** tool, which sends the record you click to that window as a
marker at its own lon/lat — gmtedit.m's `hMirAxes` / `ptcoords`. Non-blocking — returns the opaque
window handle.

Saving writes back in the format the cruise came from. "Save as old *.gmt" always writes the legacy
binary, converting an MGD77+ cruise on the way (gmtedit.m's `force_gmt`).

```julia
gmtedit("C:/data/mgd77/01010003.nc")
gmtedit("so_lucky.gmt"; width=300.0, center=(-70.0, 31.0))

fig = iview("C:/data/mgd77/01010003.nc")      # the cruise track, plotted
gmtedit("C:/data/mgd77/01010003.nc"; parent=fig)
```
"""
function gmtedit(file::AbstractString=""; width::Real=200.0, center=nothing, parent=nothing)
	_ensure_callbacks()
	h = ccall(_fn(:gmtvtk_gmtedit_open), Ptr{Cvoid}, (Cstring, Cdouble), "gmtedit", Float64(width))
	h == C_NULL && error("gmtedit: failed to open the editor window")
	_start_pump()
	if parent !== nothing
		p = parent isa Ptr{Cvoid} ? parent : _fig_handle(parent)
		_GE_PARENT[h] = p
		ccall(_fn(:gmtvtk_gmtedit_set_parent), Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}), h, p)
	end
	if !isempty(file)
		path = String(file)
		isempty(splitext(path)[2]) && (path *= ".nc")   # gmtedit.m fills in the missing extension
		_ge_load!(h, path)
		if center !== nothing && haskey(_GE_REG, h)
			tr = _GE_REG[h]
			# gmtedit.m's -P: start the display centred on the record closest to (lon,lat).
			d  = @. (tr.lon - Float64(center[1]))^2 + (tr.lat - Float64(center[2]))^2
			id = argmin(d)
			ccall(_fn(:gmtvtk_gmtedit_set_mark), Cvoid, (Ptr{Cvoid}, Cdouble), h, tr.xdata[id])
		end
	end
	return h
end
