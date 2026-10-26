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
		r = (mode == "gdalinfo") ? _gdalinfo(src) : GMT.grdinfo(src)
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
# The grid is THE grid every Grid Tool works on — `_active_grid`, the layer ON DISPLAY — never the
# figure's own `fig.G`. A window whose grid did not come in through `_view_grid` (an Aquamoto tsunami
# above all: its figure carries no grid, the slice on screen is registered under the window's
# surface name) answered "No grid" here while every tool found the layer. The figure's own object is
# only the fallback for a window with no grid layer at all (an image).
function _info_text(fig, mode::AbstractString)::Nothing
	h = try _fig_handle(fig) catch; C_NULL end
	name = h == C_NULL ? "" : _active_grid_name(h)
	G = h == C_NULL ? nothing : _active_grid(h, name)
	obj = G === nothing ? _info_object(fig) : G
	if obj === nothing
		print("No grid or image in this window.")
		return nothing
	end
	G === nothing && return _info_report(obj, _info_source_file(h), mode)
	return _info_report(_info_grid(h, name, G), _info_source(h, name), mode)
end

# What a named grid element is described from. An Aquamoto tank's element is ONE LAYER of a cube, and
# no file spec describes one: grdinfo on "file?var[k]" prints the header's `actual_range`, which is
# the WHOLE variable's (all timesteps), and GDAL has no layer syntax at all. So the layer is described
# as the object it is (`_info_grid`). Anything else: the plain file-or-nothing rule of
# `_info_source_file`.
_info_is_tank(h::Ptr{Cvoid}, name::AbstractString) =
	h != C_NULL && (st = get(_AQUA, h, nothing); st !== nothing && name == basename(st.path))
function _info_source(h::Ptr{Cvoid}, name::AbstractString)::String
	_info_is_tank(h, name) && return ""
	return _info_source_file(h, name)
end

# The grid a named element's report describes. A tsunami layer is the WATER: its `z` stores the land
# elevation on every dry node, so the raw layer's range is the land's (v_max = the highest hill), not
# the sea's. It is described as `_aqua_split` cuts it — THE water half, dry nodes NaN, exactly the
# cells the layer colours as water and its colour bar spans (whole layer when Split Dry/Wet is off).
function _info_grid(h::Ptr{Cvoid}, name::AbstractString, G::GMTgrid)::GMTgrid
	_info_is_tank(h, name) || return G
	st = _AQUA[h]
	return first(_aqua_split(st, G, st.bat.z, G.z, st.split))
end
_info_grid(h::Ptr{Cvoid}, name::AbstractString, G) = G

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
	return _info_report(_info_grid(scene, name, G), _info_source(scene, name), mode)
end
