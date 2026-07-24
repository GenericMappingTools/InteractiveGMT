# mgd77tracks.jl — Geophysics > Magnetics > Import *.gmt/*.nc file(s): plot cruise-track
# navigation, port of Mirone's GeophysicsImportGmtFile_CB (mirone.m ~3757). Mirone reads the
# legacy pre-MGD77 *.gmt binary format with its own compiled gmtlist_m MEX and MGD77+ netCDF with
# nc_funs. GMT itself (>= 5) only understands MGD77 ASCII / MGD77T / MGD77+ netCDF -- the old
# *.gmt binary was dropped from the mgd77 suite years ago (confirmed live: `gmt mgd77list` on a
# real Mirone *.gmt sample answers "Hard path without extension given" -- it doesn't recognise the
# extension at all). So *.nc goes through GMT.jl's own `mgd77list` (no new dependency, no
# reimplemented reader); a *.gmt file is reported as unsupported per-file rather than guessing at
# its byte layout.
#
# Scope is deliberately just the track (lon/lat), lightly Douglas-Peucker simplified -- not the
# magnetics/gravity/topography columns Mirone's importer also has string plumbing for.

# Read a list file: one path per line, "#"-prefixed / blank lines skipped. A bare filename (no
# directory component) is resolved against the list file's OWN directory, matching Mirone's
# aux_funs('get_mgg', ...) PathName-prepending behaviour.
function _import_gmt_list(listpath::AbstractString)::Vector{String}
	dir = dirname(listpath)
	out = String[]
	isfile(listpath) || return out
	for line in eachline(listpath)
		t = strip(first(split(line)))
		(isempty(t) || startswith(t, "#")) && continue
		push!(out, isabspath(t) ? t : joinpath(dir, t))
	end
	return out
end

# WGS84 lon/lat, geographic (all MGD77+ navigation is). The literal string crs_from's own plain
# lon/lat fallback resolves to -- setting it explicitly on the built GMTdataset lets `_isgeog`
# (drop.jl) detect it the SAME way any other referenced dataset is detected, no special-casing.
const _MGD77_PROJ4 = "+proj=longlat +datum=WGS84 +no_defs"

# Extract (lon,lat) navigation from one MGD77+ netCDF cruise file via GMT's mgd77list module,
# lightly Douglas-Peucker simplified (gmtsimplify, ~0.001 degree tolerance -- a few hundred
# metres, well under typical MGD77 1-minute sampling spacing), packed as a proper GMTdataset (line
# geometry, WGS84) so it flows through the SAME overlay machinery (`_pack_dataset_flat`,
# `_add_dataset_to_scene`, `_promote_dataset`'s `_dataset_bbox`/`_isgeog`) every other dropped
# table already uses -- SACRED_LAW.md: adding a line overlay to a scene is already ONE function,
# never a second hand-rolled ccall. Returns `nothing` on nothing usable.
#
# mgd77list.jl/mgd77info.jl/etc (GMT/src/mgd77/) exist on disk but are NOT `include`d in GMT.jl
# (only magref.jl is -- see GMT.jl:398) -- `GMT.mgd77list` is simply undefined. Call the module in
# MONOLITHIC mode instead (GMT.gmt("mgd77list <path> -F...")), the same low-level path every
# wrapped module funnels through anyway -- confirmed live (2026-07-24) after `GMT.mgd77list`
# threw UndefVarError on a real cruise file.
function _mgd77_track(path::AbstractString)
	D = GMT.gmt("mgd77list " * path * " -Flon,lat")
	d1 = D isa AbstractVector ? (isempty(D) ? nothing : first(D)) : D
	d1 === nothing && return nothing
	m = d1.data
	(m === nothing || size(m, 1) < 2) && return nothing
	simplified = try
		Ds = GMT.gmtsimplify(d1; tol=0.001)
		ms = (Ds isa AbstractVector ? first(Ds) : Ds).data
		(ms === nothing || size(ms, 1) < 2) ? m : ms
	catch
		m                                         # simplification is a nicety, never fatal
	end
	return GMT.mat2ds(simplified; proj4=_MGD77_PROJ4, geom=GMT.wkbLineString)
end

# Read one file's navigation. Returns (name, GMTdataset) or `nothing` (logging why, into the
# scene's Errors console — never a silent skip).
function _import_gmt_read(scene::Ptr{Cvoid}, path::AbstractString)
	isfile(path) || (_viewer_log_error(scene, "Import *.gmt/*.nc: file not found: $path"); return nothing)
	ext = lowercase(splitext(path)[2])
	if ext == ".gmt"
		_viewer_log_error(scene, "Import *.gmt/*.nc: \"$(basename(path))\" is the legacy pre-MGD77 " *
		                          "*.gmt binary format, which GMT (>= 5) no longer reads -- convert " *
		                          "it to MGD77+ netCDF (e.g. via mgd77manage) and import the .nc file instead.")
		return nothing
	elseif ext != ".nc"
		_viewer_log_error(scene, "Import *.gmt/*.nc: unsupported extension \"$ext\" ($(basename(path)))")
		return nothing
	end
	D = try
		_mgd77_track(path)
	catch e
		_viewer_log_error(scene, "Import *.gmt/*.nc: failed to read \"$(basename(path))\": $(sprint(showerror, e))")
		return nothing
	end
	D === nothing && (_viewer_log_error(scene, "Import *.gmt/*.nc: \"$(basename(path))\" has no usable navigation"); return nothing)
	return splitext(basename(path))[1], D
end

# C callback (gmtvtk_set_import_gmt_callback): `cpath` is a single file (isList == 0) or a list
# file (isList != 0). Each track is read then handed to `_drop_into` (drop.jl) — the SAME
# dispatcher `_on_drop` itself calls for a dropped/opened GMTdataset, and the SAME convention its
# own multi-item case (a netCDF file with several variables) already uses for "several new named
# things from one import, the first one promotes an empty launcher, the rest are added as extras"
# (`isbase = empty && i == 1`, `_open_spec_into`'s caller in `_on_drop`). No bespoke second
# promote/add orchestration here (SACRED_LAW.md).
function _on_import_gmt(scene::Ptr{Cvoid}, cpath::Cstring, isList::Cint)::Cvoid
	try
		path = unsafe_string(cpath)
		files = isList != 0 ? _import_gmt_list(path) : [path]
		if isempty(files)
			_viewer_log_error(scene, "Import *.gmt/*.nc: no files found in list \"$(basename(path))\"")
			return
		end
		empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		any_added = false
		for f in files
			r = _import_gmt_read(scene, f)
			r === nothing && continue
			name, D = r
			_drop_into(scene, D, name; promote=(empty && !any_added), source=f)
			any_added = true
		end
		any_added || _viewer_log_error(scene, "Import *.gmt/*.nc: nothing plotted")
	catch e
		_viewer_log_error(scene, "Import *.gmt/*.nc FAILED: $(sprint(showerror, e))")
		@warn "import gmt/nc: request failed" exception=(e,)
	end
	return
end

# Lazy registration (first window open, via eventloop.jl `_ensure_callbacks`) -- thin invokelatest
# trampoline, same convention as every other C->Julia callback.
function _register_import_gmt()
	fptr = @cfunction((s, p, l) -> Base.invokelatest(_on_import_gmt, s, p, l),
	                  Cvoid, (Ptr{Cvoid}, Cstring, Cint))
	ccall(_fn(:gmtvtk_set_import_gmt_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
