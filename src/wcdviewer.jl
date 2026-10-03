# Geophysics > MB-System > Water column viewer: a port of kmwcd_viewer.py (Christian dos Santos
# Ferreira, MARUM / MB-System, github.com/cdsferreira/wcd_viewer). This file is everything that is not
# the window: the Kongsberg KMALL datagrams the viewer reads (ported from the KMALL.py reader it ships
# with: index_file and read_EMdgm{Header,MWC,SPO,SKM,MRZ}), the ping geometry, the nearest-time
# navigation and the pick. The window (deps/src/wcdviewer/) asks for them through the callbacks below.

# ---- the KMALL datagrams --------------------------------------------------------------------------
# A datagram = [numBytesDgm u32][type 4 chars][version u8][systemID u8][echoSounderID u16]
#              [time_sec u32][time_nanosec u32] ... [numBytesDgm u32]. Little-endian throughout.
struct _KmDg
	type::String
	off::Int
	size::Int
	time::Float64        # header UTC time, s since 1970
end

_wcd_f(v::Real, d::Int) = GMT.Printf.format(GMT.Printf.Format("%.$(d)f"), v)
_wcd_utc(t::Float64) = GMT.Dates.format(GMT.Dates.unix2datetime(t), "yyyy-mm-dd HH:MM:SS.sss") * " UTC"

_km(::Type{T}, b::Vector{UInt8}, p::Int) where {T} = ltoh(GC.@preserve b unsafe_load(Ptr{T}(pointer(b, p + 1))))

# index_file: type, byte offset, size and header time of every datagram. A bad size ends the scan
# (KMALL.py re-syncs on the next '#'; a truncated tail is all that ever leaves it there in practice).
function _km_index(path::String)::Vector{_KmDg}
	idx = _KmDg[]
	n = filesize(path)
	open(path) do io
		hdr = Vector{UInt8}(undef, 20)
		while position(io) + 20 <= n
			off = position(io)
			readbytes!(io, hdr, 20) == 20 || break
			sz = Int(_km(UInt32, hdr, 0))
			(sz < 20 || off + sz > n) && break
			push!(idx, _KmDg(String(hdr[5:8]), off, sz, _km(UInt32, hdr, 12) + _km(UInt32, hdr, 16) / 1e9))
			seek(io, off + sz)
		end
	end
	return idx
end

_km_read(path::String, d::_KmDg) = open(io -> (seek(io, d.off); read(io, d.size)), path)

# read_EMdgmMWC: one ping's water column. amp = beams x samples (0.5 dB units, raw, as the viewer
# plots it; NaN past a beam's own sample count), the beam pointing angles re vertical, and dr, the
# slant range of one sample, 0.5 c / fs.
function _km_mwc(b::Vector{UInt8})
	p = 24                                            # header (20) + partition (4)
	p += _km(UInt16, b, p)                            # cmnPart (EMdgmMbody), its own size
	ntx, ntxb = _km(UInt16, b, p + 2), _km(UInt16, b, p + 4)
	p += _km(UInt16, b, p) + ntx * ntxb               # txInfo + its sector blocks
	# rxInfo "2H3B1b2f": size, numBeams, numBytesPerBeamEntry, phaseFlag, TVG X, TVG offset, fs, c
	nrx, nbeam_b, phase = Int(_km(UInt16, b, p + 2)), Int(b[p + 5]), Int(b[p + 6])
	fs, c = _km(Float32, b, p + 8), _km(Float32, b, p + 12)
	p += _km(UInt16, b, p)                            # rxInfo, its own size
	beams = Vector{Tuple{Float64,Int,Int}}(undef, nrx)   # angle, first sample byte, count
	for i in 1:nrx
		ns = Int(_km(UInt16, b, p + 10))
		beams[i] = (Float64(_km(Float32, b, p)), p + nbeam_b, ns)
		p += nbeam_b + ns * (phase == 0 ? 1 : phase == 1 ? 2 : 3)
	end
	maxns = isempty(beams) ? 0 : maximum(t -> t[3], beams)
	amp = fill(NaN32, nrx, maxns)
	for (i, (_, q, ns)) in enumerate(beams), j in 1:ns
		amp[i, j] = reinterpret(Int8, b[q + j])
	end
	ang = Float64[isfinite(a) ? a : 0.0 for (a, _, _) in beams]   # compute_amplitude_geometry
	return amp, ang, 0.5 * Float64(c) / Float64(fs)
end

# read_EMdgmSPO: (sensor time, lat, lon, cog, sog m/s)
function _km_spo(b::Vector{UInt8})
	p = 20 + _km(UInt16, b, 20)
	t = _km(UInt32, b, p) + _km(UInt32, b, p + 4) / 1e9
	return (t, _km(Float64, b, p + 12), _km(Float64, b, p + 20), Float64(_km(Float32, b, p + 32)),
	        Float64(_km(Float32, b, p + 28)))
end

# read_EMdgmSKM: the mean heading of the datagram's KM binary samples (what the viewer takes from it).
# Each sample is a #KMB block (120 bytes, heading at +48) followed by its delayed heave (12 bytes).
function _km_skm_heading(b::Vector{UInt8})
	p = 20
	nsamp = Int(_km(UInt16, b, p + 6))
	p += _km(UInt16, b, p)
	h = Float64[Float64(_km(Float32, b, p + 132 * (k - 1) + 48)) for k in 1:nsamp if p + 132 * (k - 1) + 52 <= length(b)]
	isempty(h) && return NaN
	h = filter(isfinite, h)
	return isempty(h) ? NaN : sum(h) / length(h)
end

# read_EMdgmMRZ: the soundings the "Bottom" overlay draws: (y, z, detectionType, detectionClass,
# detectionMethod), main detections and extra detections alike.
function _km_mrz(b::Vector{UInt8})
	p = 24
	p += _km(UInt16, b, p)                            # cmnPart
	ntx, ntxb = _km(UInt16, b, p + 84), _km(UInt16, b, p + 86)
	p += _km(UInt16, b, p) + ntx * ntxb               # pingInfo + txSectorInfo blocks
	nmain, nbs = _km(UInt16, b, p + 2), _km(UInt16, b, p + 6)
	nextra, ncls, nclsb = _km(UInt16, b, p + 26), _km(UInt16, b, p + 28), _km(UInt16, b, p + 30)
	p += _km(UInt16, b, p) + ncls * nclsb             # rxInfo + extraDetClassInfo blocks
	n = Int(nmain) + Int(nextra)
	y = Float64[]; z = Float64[]; dt = Int[]; dc = Int[]; dm = Int[]
	for k in 0:n-1
		q = p + k * nbs
		q + 100 > length(b) && break
		push!(dt, b[q + 3]); push!(dm, b[q + 4]); push!(dc, b[q + 8])
		push!(z, _km(Float32, b, q + 92)); push!(y, _km(Float32, b, q + 96))
	end
	return y, z, dt, dc, dm
end

# ---- the open file -------------------------------------------------------------------------------
mutable struct _WcdFile
	path::String
	pings::Vector{_KmDg}                 # the #MWC datagrams, in file order
	mrz::Vector{_KmDg}; mrzpath::String  # #MRZ: this file's, or the companion .kmall's for a .kmwcd
	nav::Vector{NTuple{5,Float64}}       # #SPO: (t, lat, lon, cog, sog m/s), time-sorted
	hdg::Vector{NTuple{2,Float64}}       # #SKM: (t, mean heading), time-sorted
	cache::Dict{Int,Any}
end
const _WCD = Ref{Union{Nothing,_WcdFile}}(nothing)

_wcd_nearest(v, t) = isempty(v) ? 0 : argmin(k -> abs(v[k][1] - t), eachindex(v))

# open_file: index, navigation, heading, the bottom detections' datagrams (from the companion .kmall
# when a .kmwcd has none of its own) and the pre-scan of every ping for the global amplitude and
# depth ranges. Returns (npings, ampmin, ampmax, depthmax) or throws.
function _wcd_open(path::String; progress::Function = (pct -> nothing))
	idx = _km_index(path)
	pings = filter(d -> d.type == "#MWC", idx)
	isempty(pings) && error("No #MWC data found.")
	base, ext = splitext(path)
	sib = base * ".kmall"
	navidx = idx
	mrz, mrzpath = filter(d -> d.type == "#MRZ", idx), path
	if lowercase(ext) == ".kmwcd" && isfile(sib)
		sidx = _km_index(sib)
		isempty(mrz) && ((mrz, mrzpath) = (filter(d -> d.type == "#MRZ", sidx), sib))
		any(d -> d.type in ("#SPO", "#SKM"), idx) || (navidx = sidx)
	end
	navpath = navidx === idx ? path : sib
	nav = sort!([_km_spo(_km_read(navpath, d)) for d in navidx if d.type == "#SPO"]; by = first)
	hdg = sort!([(d.time, _km_skm_heading(_km_read(navpath, d))) for d in navidx if d.type == "#SKM"]; by = first)
	F = _WcdFile(path, pings, mrz, mrzpath, nav, hdg, Dict{Int,Any}())
	amin, amax, dmax = Inf, -Inf, 0.0
	last = -1
	for (i, d) in enumerate(pings)
		pct = round(Int, 100i / length(pings))
		pct != last && (progress(pct); last = pct)
		amp, ang, dr = _km_mwc(_km_read(path, d))
		for v in amp
			isfinite(v) && (v < amin && (amin = v); v > amax && (amax = v))
		end
		# the deepest sample that HOLDS data (kmwcd_viewer.py took the whole padded grid, i.e. the longest
		# beam's range straight down -- a plot twice as deep as the water column, half of it empty)
		for b in axes(amp, 1)
			s = findlast(isfinite, view(amp, b, :))
			s === nothing || (dmax = max(dmax, cosd(ang[b]) * (s - 1) * dr))
		end
	end
	_WCD[] = F
	return length(pings), amin, amax, dmax
end

function _wcd_ping(i::Int)
	F = _WCD[]
	return get!(F.cache, i) do
		length(F.cache) > 64 && empty!(F.cache)
		d = F.pings[i+1]
		amp, ang, dr = _km_mwc(_km_read(F.path, d))
		(amp, ang, dr, d.time)
	end
end

_wcd_nav(t) = (F = _WCD[]; k = _wcd_nearest(F.nav, t); k == 0 ? (NaN, NaN, NaN, NaN) : F.nav[k][2:5])

# the redraw's header line: "Ping i: DateTime=..., SOG=x.xx kn, Pos=lat,lon"
function _wcd_meta(i::Int)
	_, _, _, t = _wcd_ping(i)
	lat, lon, _, sog = _wcd_nav(t)
	ts = _wcd_utc(t)
	sogs = isfinite(sog) ? _wcd_f(sog * 1.9438445, 2) * " kn" : "N/A"
	pos = (isfinite(lat) ? _wcd_f(lat, 6) : "N/A") * "," * (isfinite(lon) ? _wcd_f(lon, 6) : "N/A")
	return "Ping $i: DateTime=$ts, SOG=$sogs, Pos=$pos"
end

# The MRZ bottom detections nearest in time to ping i, as the viewer draws them: (x, z, colour code,
# hollow). Colour: 1 red (amplitude detection), 2 dark blue (phase), 0 grey (none), 3 green (rejected).
# Hollow = an extra detection (detectionClass != 0). x follows kmwcd_viewer.py's own flips verbatim:
# every detection is flipped once to the water column's orientation and the solid ones once more.
function _wcd_bottom(i::Int)
	F = _WCD[]
	isempty(F.mrz) && return NTuple{4,Float64}[]
	_, _, _, t = _wcd_ping(i)
	k = argmin(j -> abs(F.mrz[j].time - t), eachindex(F.mrz))
	y, z, dt, dc, dm = _km_mrz(_km_read(F.mrzpath, F.mrz[k]))
	out = NTuple{4,Float64}[]
	for j in eachindex(y)
		(isfinite(y[j]) && isfinite(z[j])) || continue
		col = dt[j] == 2 ? 3.0 : dm[j] == 1 ? 1.0 : dm[j] == 2 ? 2.0 : 0.0
		hollow = dc[j] != 0
		push!(out, (hollow ? -y[j] : y[j], z[j], col, hollow ? 1.0 : 0.0))
	end
	return out
end

# _on_plot_click: the grid cell nearest the click (x across-track as drawn, d depth), the vessel at the
# ping's time and the point's own position. Azimuth = the nearest SKM heading, else the SPO course.
# Returns the picked-positions row: Ping, UTC, AcrossTrack_m, Depth_m, ShipLat, ShipLon, PointLat,
# PointLon, Azimuth.
function _wcd_pick(i::Int, x::Float64, d::Float64)::Vector{String}
	F = _WCD[]
	amp, ang, dr, t = _wcd_ping(i)
	nb, ns = size(amp)
	best, across, depth = Inf, NaN, NaN
	for bi in 1:nb, s in 1:ns
		r = (s - 1) * dr
		ya, da = -sind(ang[bi]) * r, cosd(ang[bi]) * r
		e = (ya - x)^2 + (da - d)^2
		e < best && ((best, across, depth) = (e, ya, da))
	end
	lat, lon, cog, _ = _wcd_nav(t)
	k = _wcd_nearest(F.hdg, t)
	heading = k == 0 ? NaN : F.hdg[k][2]
	az = isfinite(heading) ? heading : cog
	status = isfinite(heading) ? "Heading" : isfinite(cog) ? "COG" : "None"
	east, north = isfinite(az) ? (across * cosd(az), -across * sind(az)) : (across, 0.0)
	plat, plon = NaN, NaN
	if isfinite(lat) && isfinite(lon)
		plat = lat + north / 111320.0
		plon = lon + east / (111320.0 * max(1e-6, cosd(lat)))
	end
	f6(v) = isfinite(v) ? _wcd_f(v, 6) : ""
	ts = _wcd_utc(t)
	return [string(i), ts, _wcd_f(across, 2), _wcd_f(depth, 2), f6(lat), f6(lon), f6(plat), f6(plon), status]
end

# ---- the callbacks the window calls (deps/src/wcdviewer/) ----------------------------------------
_wcd_put!(buf::Ptr{UInt8}, cap::Cint, s::String) = (cap > 0 && (n = min(ncodeunits(s), cap - 1);
	unsafe_copyto!(buf, pointer(s), n); unsafe_store!(buf, 0x00, n + 1)); nothing)

# open: info = [npings, ampmin, ampmax, depthmax, ncolour, cz(n), rgb(3n)] (the viridis CPT over 0..1)
function _on_wcd_open(path::Cstring, info::Ptr{Cdouble}, cap::Cint, err::Ptr{UInt8}, ecap::Cint)::Cint
	try
		ccall(_fn(:gmtvtk_busy_show), Cvoid, (Cstring,), "Indexing…")
		n, amin, amax, dmax = try
			_wcd_open(unsafe_string(path); progress = pct -> ccall(_fn(:gmtvtk_busy_show), Cvoid, (Cstring,), "Loading pings… $pct%"))
		finally
			ccall(_fn(:gmtvtk_busy_close), Cvoid, ())
		end
		cz, crgb, nc = _cpt_nodes_range(0.0, 1.0, :viridis)
		v = vcat(Float64[n, amin, amax, dmax, nc], cz, crgb)
		length(v) <= cap || error("info buffer too small")
		unsafe_copyto!(info, pointer(v), length(v))
		return Cint(1)
	catch e
		_wcd_put!(err, ecap, sprint(showerror, e))
		return Cint(0)
	end
end

# ping i: dims = [nbeams, nsamples]; with a big enough `amp` (beam-major) and `ang`, fills them too;
# xtra = [dr, xmin, xmax]; meta = the header line. Returns 1, or 0 with no file / bad index.
function _on_wcd_ping(i::Cint, dims::Ptr{Cint}, amp::Ptr{Cfloat}, acap::Cint, ang::Ptr{Cdouble}, gcap::Cint,
                      xtra::Ptr{Cdouble}, meta::Ptr{UInt8}, mcap::Cint)::Cint
	try
		F = _WCD[]
		(F === nothing || !(0 <= i < length(F.pings))) && return Cint(0)
		A, a, dr, _ = _wcd_ping(Int(i))
		nb, ns = size(A)
		unsafe_store!(dims, Cint(nb), 1); unsafe_store!(dims, Cint(ns), 2)
		if acap >= nb * ns && gcap >= nb
			At = permutedims(A)                       # beam-major: sample index fastest
			unsafe_copyto!(amp, pointer(At), nb * ns)
			unsafe_copyto!(ang, pointer(a), nb)
			rmax = max(ns - 1, 0) * dr
			xs = [-sind(x) * rmax for x in a]
			unsafe_store!(xtra, dr, 1); unsafe_store!(xtra, min(0.0, minimum(xs; init = 0.0)), 2)
			unsafe_store!(xtra, max(0.0, maximum(xs; init = 0.0)), 3)
			_wcd_put!(meta, mcap, _wcd_meta(Int(i)))
		end
		return Cint(1)
	catch e
		@tool_error "Water column viewer: ping $i" exception=(e, catch_backtrace())
		return Cint(0)
	end
end

# bottom detections of ping i, 4 doubles each (see _wcd_bottom); returns how many (all, when they fit)
function _on_wcd_bottom(i::Cint, out::Ptr{Cdouble}, cap::Cint)::Cint
	try
		_WCD[] === nothing && return Cint(0)
		B = _wcd_bottom(Int(i))
		if 4length(B) <= cap
			for (k, t) in enumerate(B), j in 1:4
				unsafe_store!(out, t[j], 4(k - 1) + j)
			end
		end
		return Cint(length(B))
	catch e
		@tool_error "Water column viewer: bottom detections" exception=(e, catch_backtrace())
		return Cint(0)
	end
end

# a pick at (x, d) on ping i: the row, tab-separated
function _on_wcd_pick(i::Cint, x::Cdouble, d::Cdouble, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		_WCD[] === nothing && return Cint(0)
		_wcd_put!(out, cap, join(_wcd_pick(Int(i), Float64(x), Float64(d)), '\t'))
		return Cint(1)
	catch e
		@tool_error "Water column viewer: pick" exception=(e, catch_backtrace())
		return Cint(0)
	end
end

# NOTHING OF THIS TOOL RUNS AT START-UP. Its callbacks are wired the first time the viewer is opened:
# the menu entry fires warmupTool("wcd") (the same hook every tool dialog fires when it opens) and waits
# for them, and `wcdviewer()` wires them itself. The entry below is a plain Dict insertion made at
# package load, i.e. baked into the precompiled image -- no @cfunction is built until then.
const _WCD_WIRED = Ref(false)
function _register_wcd()
	_WCD_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_wcd_callbacks) || return   # a DLL built without the viewer
	o = @cfunction((p, a, c, e, ec) -> Base.invokelatest(_on_wcd_open, p, a, c, e, ec),
	               Cint, (Cstring, Ptr{Cdouble}, Cint, Ptr{UInt8}, Cint))
	g = @cfunction((i, d, a, ac, an, gc, x, m, mc) -> Base.invokelatest(_on_wcd_ping, i, d, a, ac, an, gc, x, m, mc),
	               Cint, (Cint, Ptr{Cint}, Ptr{Cfloat}, Cint, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Ptr{UInt8}, Cint))
	b = @cfunction((i, o, c) -> Base.invokelatest(_on_wcd_bottom, i, o, c), Cint, (Cint, Ptr{Cdouble}, Cint))
	k = @cfunction((i, x, d, o, c) -> Base.invokelatest(_on_wcd_pick, i, x, d, o, c),
	               Cint, (Cint, Cdouble, Cdouble, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_wcd_callbacks), Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}), o, g, b, k)
	_WCD_WIRED[] = true
	return
end
warm_register("wcd", _register_wcd)

"""
    wcdviewer(file="")

Open the water column viewer (Geophysics > MB-System > Water column viewer), with a Kongsberg
`.kmwcd`/`.kmall` file loaded when one is given.
"""
function wcdviewer(file::String = "")::Bool
	_register_wcd()
	ok = ccall(_fn(:gmtvtk_wcd_open), Cint, (Cstring,), file)
	_start_pump()
	return ok == 1
end
export wcdviewer

# The open viewer's state, for tests: (open, npings, ping, npicks, nbottom)
function _wcd_state()
	v = zeros(Cint, 5)
	ccall(_fn(:gmtvtk_wcd_state), Cint, (Ptr{Cint}, Cint), v, Cint(5))
	return (open = v[1] == 1, npings = Int(v[2]), ping = Int(v[3]), npicks = Int(v[4]), nbottom = Int(v[5]))
end
_wcd_set_ping(i::Int) = ccall(_fn(:gmtvtk_wcd_set_ping), Cint, (Cint,), i) == 1
_wcd_pick_at(x::Real, d::Real) = ccall(_fn(:gmtvtk_wcd_pick_at), Cint, (Cdouble, Cdouble), x, d) == 1
_wcd_save_png(path::String) = ccall(_fn(:gmtvtk_wcd_save_png), Cint, (Cstring,), path) == 1
_wcd_grab_window(path::String) = ccall(_fn(:gmtvtk_wcd_grab_window), Cint, (Cstring,), path) == 1
_wcd_close() =ccall(_fn(:gmtvtk_wcd_close), Cint, ()) == 1
