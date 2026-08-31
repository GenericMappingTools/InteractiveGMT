# info.jl — the toolbar "i" (Info) button: a grdinfo / gdalinfo report on the active grid/image.
#
# The C++ flyout (70_window.cpp) has a stylish 'i' slot with a 'v' dropdown picking the reporter
# (grdinfo = default, or gdalinfo). On click the viewer runs `_info_text(fig, mode)` through the
# console eval bridge (g_juliaEval binds `fig` to the clicked window's figure first), captures the
# printed report, and shows it in a read-only text popup. So this file only has to PRINT the report.
#
# Both reporters are the GMT.jl functions of the same name: grdinfo for the grid header/range,
# gdalinfo for the full GDAL dataset report. grdinfo needs a grid; gdalinfo takes a grid OR image.

# The primary grid/image a window reports on: QtFigure -> its grid, QtImage -> its image.
_info_object(fig::QtFigure) = fig.G
_info_object(fig::QtImage)  = fig.I
_info_object(::Any)         = nothing

# THE REPORT IS ABOUT THE DATA FILE, when there is one — and about nothing else. What sits in memory
# is not what the file holds: GMT.jl reads every integer raster into a Float32 grid, so reporting the
# in-memory object says "Float32" about an Int16 GeoTIFF, which is the one thing an Info button must
# not get wrong. The file is what is described; what it was turned into on the way in is not part of
# the answer. A grid with no file behind it — a crop, an anomaly, anything derived — has nothing else
# to describe and is reported as it is.
#
# `name`, when given, is a Scene Objects element: the file is only its own if that element came out
# of it (_OPEN_FILES tracks which rows each open file produced), so a derived grid living in a
# window opened from a file is NOT described by that file.
function _info_source_file(handle::Ptr{Cvoid}, name::AbstractString = "")::String
	handle == C_NULL && return ""
	for (spec, v) in _OPEN_FILES
		v[1] === handle || continue
		(isempty(name) || name in v[2]) || continue
		p = String(first(split(spec, '?')))      # "file?var" (a netCDF subdataset) -> the file itself
		isfile(p) && return p
	end
	return ""
end

# One report, whichever reporter and whichever source — the ONE place that decides file-or-object, so
# the toolbar button and a Scene Objects row can never answer differently.
function _info_report(obj, path::AbstractString, mode::AbstractString)::Nothing
	src = isempty(path) ? obj : path
	local r
	try
		r = (mode == "gdalinfo") ? GMT.gdalinfo(src) : GMT.grdinfo(src)
	catch e
		print("$mode failed: ", sprint(showerror, e))
		return nothing
	end
	print(r === nothing ? "($mode returned nothing)" : _info_to_string(r))
	return nothing
end

# Normalize a reporter's return value to plain text. grdinfo's default report is a GMTdataset whose
# `.text` holds the lines; gdalinfo returns a String. Fall back to the value's pretty repr.
_info_to_string(s::AbstractString) = String(s)
function _info_to_string(D::GMT.GMTdataset)
	isdefined(D, :text) && !isempty(D.text) && return join(D.text, "\n")
	return sprint(show, MIME("text/plain"), D)
end
_info_to_string(x) = sprint(show, MIME("text/plain"), x)

# Print the grdinfo / gdalinfo report for the window's active grid/image. `mode` is "grdinfo"
# (default) or "gdalinfo". Returns nothing — the printed text is what the viewer captures & shows.
function _info_text(fig, mode::AbstractString)::Nothing
	obj = _info_object(fig)
	if obj === nothing
		print("No grid or image in this window.")
		return nothing
	end
	h = try _fig_handle(fig) catch; C_NULL end
	return _info_report(obj, _info_source_file(h), mode)
end

# Same report, but for a NAMED Scene Objects grid handle (base surface or an extra/nested grid) rather
# than "the window's active fig" — the grid handle's own "Info (grdinfo)…" menu entry (50_scene.cpp
# surfaceObjectMenu/gridObjectMenu), reached via the _SCENE_OBJS registry (_find_object, savefile.jl)
# so it works for extras that a QtFigure/QtImage alone can't reach.
function _info_text_named(scene::Ptr{Cvoid}, name::AbstractString, mode::AbstractString)::Nothing
	G = _find_object(scene, :grid, name)
	if G === nothing
		print("No grid named \"$name\" found.")
		return nothing
	end
	# The file only when THIS element came out of it — a derived grid sharing the window with a
	# file-opened one is described by itself, not by that file.
	return _info_report(G, _info_source_file(scene, name), mode)
end
