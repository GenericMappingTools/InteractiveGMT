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
	local r
	try
		r = (mode == "gdalinfo") ? GMT.gdalinfo(obj) : GMT.grdinfo(obj)
	catch e
		print("$mode failed: ", sprint(showerror, e))
		return nothing
	end
	print(r === nothing ? "($mode returned nothing)" : _info_to_string(r))
	return nothing
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
	local r
	try
		r = (mode == "gdalinfo") ? GMT.gdalinfo(G) : GMT.grdinfo(G)
	catch e
		print("$mode failed: ", sprint(showerror, e))
		return nothing
	end
	print(r === nothing ? "($mode returned nothing)" : _info_to_string(r))
	return nothing
end
