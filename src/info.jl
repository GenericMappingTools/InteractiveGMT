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

# The window's image AS IT IS, not as it is drawn: the object its row holds (a Bool mask stays Bool),
# and for a 16-bit band the full-precision original the image add stashed beside its 8-bit display
# copy -- the same (key, original) pair Auto histogram stretch reads (savefile.jl `_on_img_stretch`).
function _info_image(h::Ptr{Cvoid}, fig, name::AbstractString = "")
	h == C_NULL && return _info_object(fig)
	if isempty(name)
		key, I = _find_object_named(h, :image)
		I isa GMTimage || return _info_object(fig)
	else                                           # a NAMED image: that one, or nothing (no fallback)
		key, I = String(name), _find_object_exact(h, :image, name)
		I isa GMTimage || return nothing
	end
	d = get(_IMG_ORIG, h, nothing)
	return d === nothing ? I : get(d, key, I)
end

# The viewer's answer to "what is this window showing": (1, grid name) / (2, image name) / (0, "").
function _active_element(h::Ptr{Cvoid})
	(h == C_NULL || !haskey(_LIB_FNS, :gmtvtk_active_element_h)) && return 0, ""
	buf = zeros(UInt8, 4096)
	k = ccall(_fn(:gmtvtk_active_element_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(length(buf)))
	return Int(k), GC.@preserve buf unsafe_string(pointer(buf))
end

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
	# AN IMAGE IS NOT A GRID. grdinfo reads any raster it is pointed at as a grid -- a UInt16 Landsat
	# band came back as a Float grid report, and the in-memory image object it cannot read at all ("not
	# a GRID container type"). The grid reporter's slot therefore describes an image as the image it
	# is: GMT's own image header (GMTimage, its element type -- UInt16 for a band --, bands, georef).
	if obj isa GMTimage && mode != "gdalinfo"
		try
			GMT.info(obj, false)                      # prints; the console door captures stdout
		catch e
			print("info failed: ", sprint(showerror, e))
		end
		return nothing
	end
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
	# WHAT IS ON SCREEN, asked of the viewer (gmtvtk_active_element_h): the visible grid, else the
	# topmost visible image. Asking for "the grid" first answered with a HIDDEN grid whenever the window
	# showed only an image -- a mask just computed from a grid unchecks that grid, and Info went on
	# describing it.
	kind, ename = _active_element(h)
	if kind == 2
		I = _info_image(h, fig, ename)
		I === nothing && (print("No image named \"$ename\" found."); return nothing)
		return _info_report(I, _info_source_file(h, ename), mode)
	end
	name = kind == 1 ? ename : (h == C_NULL ? "" : _active_grid_name(h))
	G = h == C_NULL ? nothing : _active_grid(h, name)
	obj = G === nothing ? _info_image(h, fig) : G
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
