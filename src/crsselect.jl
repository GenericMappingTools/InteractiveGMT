# crsselect.jl -- the data behind the Coordinate Reference System selector (deps/src/73_crsselect.cpp),
# all of it from PROJ's own database through GMT.jl's libproj -- the same catalogue QGIS's selector is
# generated from. One callback, the two-phase buffer protocol: C asks with out == NULL for the length
# of an answer, then again with a buffer to fill. Requests:
#   "list"       one line per CRS: id \t name \t type \t deprecated \t bbox \t w \t s \t e \t n \t
#                method \t area \t body          (type = PROJ's PJ_TYPE number)
#   "coast"      "lon lat" lines, segments separated by ">" (GMT low-resolution shoreline)
#   "info <id>"  WKT2, a line holding only \x1f, then the PROJ string
#   "check\t<scene hex>\t<grid>\t<id>"   can that window be reprojected into <id>? (_crs_check)

# PROJ_CRS_INFO (proj.h), field for field.
struct _PJCrsInfo
	auth_name::Cstring
	code::Cstring
	name::Cstring
	type::Cint
	deprecated::Cint
	bbox_valid::Cint
	west::Cdouble
	south::Cdouble
	east::Cdouble
	north::Cdouble
	area_name::Cstring
	projection_method_name::Cstring
	celestial_body_name::Cstring
end

# A field as one tab-free line of text.
_crs_txt(c::Cstring)::String = c == C_NULL ? "" : replace(unsafe_string(c), r"[\t\r\n]" => " ")

function _crs_list()::String
	n = Ref{Cint}(0)
	p = ccall((:proj_get_crs_info_list_from_database, GMT.libproj), Ptr{Ptr{_PJCrsInfo}},
	          (Ptr{Cvoid}, Cstring, Ptr{Cvoid}, Ptr{Cint}), C_NULL, C_NULL, C_NULL, n)
	p == C_NULL && error("PROJ handed back no CRS catalogue")
	io = IOBuffer()
	try
		for i in 1:n[]
			r = unsafe_load(unsafe_load(p, i))
			print(io, _crs_txt(r.auth_name), ':', _crs_txt(r.code), '\t', _crs_txt(r.name), '\t', r.type, '\t',
			      r.deprecated, '\t', r.bbox_valid, '\t', r.west, '\t', r.south, '\t', r.east, '\t', r.north, '\t',
			      _crs_txt(r.projection_method_name), '\t', _crs_txt(r.area_name), '\t',
			      _crs_txt(r.celestial_body_name), '\n')
		end
	finally
		ccall((:proj_crs_info_list_destroy, GMT.libproj), Cvoid, (Ptr{Ptr{_PJCrsInfo}},), p)
	end
	return String(take!(io))
end

function _crs_coast()::String
	D = GMT.coast(R = :d, D = :l, M = true, W = 1, A = "0/0/1")   # the land/ocean shoreline only: no lakes, islands in lakes, ...
	io = IOBuffer()
	for s in (D isa GMTdataset ? [D] : D)
		println(io, '>')
		for k in 1:size(s.data, 1)
			println(io, s.data[k, 1], ' ', s.data[k, 2])
		end
	end
	return String(take!(io))
end

# WKT2 (2019) and the PROJ string of one system, by authority ID ("EPSG:3763", "ESRI:102837", ...).
function _crs_info(id::String)::String
	pj = ccall((:proj_create, GMT.libproj), Ptr{Cvoid}, (Ptr{Cvoid}, Cstring), C_NULL, id)
	pj == C_NULL && error("PROJ does not know '$id'")
	try
		w = ccall((:proj_as_wkt, GMT.libproj), Cstring, (Ptr{Cvoid}, Ptr{Cvoid}, Cint, Ptr{Cstring}),
		          C_NULL, pj, Cint(2), C_NULL)                                   # PJ_WKT2_2019
		p = ccall((:proj_as_proj_string, GMT.libproj), Cstring, (Ptr{Cvoid}, Ptr{Cvoid}, Cint, Ptr{Cstring}),
		          C_NULL, pj, Cint(0), C_NULL)                                   # PJ_PROJ_5
		return (w == C_NULL ? "" : unsafe_string(w)) * "\n\x1f\n" * (p == C_NULL ? "" : unsafe_string(p))
	finally
		ccall((:proj_destroy, GMT.libproj), Ptr{Cvoid}, (Ptr{Cvoid},), pj)
	end
end

# The area where system `id` is defined (lon/lat degrees, east < west across the antimeridian), or
# nothing when PROJ does not know one.
function _crs_area(id::String)
	pj = ccall((:proj_create, GMT.libproj), Ptr{Cvoid}, (Ptr{Cvoid}, Cstring), C_NULL, id)
	pj == C_NULL && return nothing
	w, s, e, n = Ref{Cdouble}(-1000), Ref{Cdouble}(-1000), Ref{Cdouble}(-1000), Ref{Cdouble}(-1000)
	nm = Ref{Cstring}(C_NULL)
	ok = ccall((:proj_get_area_of_use, GMT.libproj), Cint,
	           (Ptr{Cvoid}, Ptr{Cvoid}, Ref{Cdouble}, Ref{Cdouble}, Ref{Cdouble}, Ref{Cdouble}, Ref{Cstring}),
	           C_NULL, pj, w, s, e, n, nm)
	name = (ok == 1 && nm[] != C_NULL) ? unsafe_string(nm[]) : ""
	ccall((:proj_destroy, GMT.libproj), Ptr{Cvoid}, (Ptr{Cvoid},), pj)
	(ok == 1 && w[] > -1000) || return nothing        # -1000 = PROJ's "unknown"
	return (w = w[], s = s[], e = e[], n = n[], name = name)
end

# Does the lon/lat box L = (W, E, S, N) touch the area `a`? Longitudes are tried a turn either way,
# since data and area may count them -180..180 or 0..360.
function _crs_overlap(L, a)::Bool
	(L[4] < a.s || L[3] > a.n) && return false
	ae = a.e < a.w ? a.e + 360 : a.e
	return any(k -> L[1] + k <= ae && L[2] + k >= a.w, (-360.0, 0.0, 360.0))
end

_crs_box_txt(b) = join((round(v; sigdigits = 6) for v in b), " / ")

# Can the window's raster be reprojected into `id`? "OK\t<the window's own srs>", or "NO\t<why>" in
# words, for the warning box. Possible = the window has a raster and a CRS, PROJ can carry the
# raster's footprint into `id`, and that footprint touches the area where `id` is defined (a UTM zone
# on the other side of the world transforms to finite numbers too, all of them meaningless).
function _crs_check(scene::Ptr{Cvoid}, grid::String, id::String)::String
	O = _find_object(scene, :grid, grid)
	O === nothing && (O = _find_object_named(scene, :image)[2])
	O === nothing && return "NO\tThis window has no grid or image to reproject."
	src = _window_srs(scene)
	isempty(src) && return "NO\tThe data in this window has no coordinate reference system, " *
	                       "so it cannot be reprojected into $id."
	W, E, S, N = Float64.(O.range[1:4])
	B = try
		_box_to(src, id, W, E, S, N)
	catch e
		return "NO\tPROJ has no transformation from this window's system into $id.\n\n" * sprint(showerror, e)
	end
	(all(isfinite, B) && B[1] < B[2] && B[3] < B[4]) ||
		return "NO\tThe data of this window does not land anywhere in $id."
	a = _crs_area(id)
	if a !== nothing
		L = _project_is_geog(src) ? (W, E, S, N) : (try _box_to(src, _LONLAT, W, E, S, N) catch; nothing end)
		(L === nothing || _crs_overlap(L, a)) ||
			return "NO\tThe data of this window (lon/lat $(_crs_box_txt(L))) lies outside the area where " *
			       "$id is defined" * (isempty(a.name) ? "" : ", $(a.name)") *
			       " (lon/lat $(_crs_box_txt((a.w, a.e, a.s, a.n))))."
	end
	return "OK\t" * src
end

function _crs_answer(req::String)::String
	req == "list"  && return _crs_list()
	req == "coast" && return _crs_coast()
	startswith(req, "info ") && return _crs_info(String(strip(req[6:end])))
	if startswith(req, "check\t")
		f = split(req, '\t')
		length(f) == 4 || error("CRS selector: malformed request '$req'")
		return _crs_check(Ptr{Cvoid}(parse(UInt, f[2]; base = 16)), String(f[3]), String(f[4]))
	end
	error("CRS selector: unknown request '$req'")
end

const _CRS_LAST = Ref(("", ""))      # (request, answer): computed on the size query, handed on the fill

function _on_crs(req::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		r = unsafe_string(req)
		(out == C_NULL || _CRS_LAST[][1] != r) && (_CRS_LAST[] = (r, _crs_answer(r)))
		a = _CRS_LAST[][2]
		n = ncodeunits(a)
		out == C_NULL && return Cint(n)
		cap > n || return Cint(-1)
		GC.@preserve a unsafe_copyto!(out, pointer(a), n)
		unsafe_store!(out, 0x00, n + 1)
		return Cint(n)
	catch e
		@tool_error "CRS selector" exception=(e, catch_backtrace())
		return Cint(-1)
	end
end

# Wired the first time the selector is asked for, never at start-up (see wcdviewer.jl's note).
const _CRS_WIRED = Ref(false)
function _register_crs_select()
	_CRS_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_crs_select_callback) || return   # a DLL built without the selector
	f = @cfunction((q, o, c) -> Base.invokelatest(_on_crs, q, o, c), Cint, (Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_crs_select_callback), Cvoid, (Ptr{Cvoid},), f)
	_CRS_WIRED[] = true
	return
end
warm_register("crs", _register_crs_select)

"""
    crs_select(fig=nothing; initial="") -> String

Open the Coordinate Reference System selector (modal, over the window of `fig` when given) and return
the authority ID of the system picked, e.g. `"EPSG:3763"`, or `""` when it was cancelled. `initial`
preselects a system. The list is PROJ's own catalogue (EPSG, ESRI, IAU, IGNF, OGC, ...), grouped by
kind, with each system's WKT, PROJ string and area of use; the last ten picks are kept as "Recently
used" in `~/.gmt/iGMT.ini`.
"""
function crs_select(fig = nothing; initial::String = "")::String
	_register_crs_select()
	h = fig === nothing ? C_NULL : _fig_handle(fig)
	buf = zeros(UInt8, 256)
	ok = ccall(_fn(:gmtvtk_crs_select), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, initial, buf, length(buf))
	ok == 1 || return ""
	return GC.@preserve buf unsafe_string(pointer(buf))
end
