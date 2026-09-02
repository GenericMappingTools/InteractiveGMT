# Ctrl+V — paste the clipboard into a window. The paste twin of drop.jl: the C side
# (`scenePasteClipboard`, 30_app.cpp) reads the clipboard and calls `_on_paste` with EITHER an
# image byte buffer OR text; nothing here builds a display of its own — an image goes through the
# SAME `_drop_into` a dropped .png takes, a numeric table through the SAME `_add_dataset_to_scene`
# a dropped .dat takes, and an X,Y-bound table through the SAME `_xy_add_dataset` File > Open uses
# (SACRED_LAW.md: one operation, one function). Copied FILES never reach here at all — the C side
# hands those to the drop callback, so pasting a copied file IS dropping it.
#
# Routing of a pasted numeric table, in this order:
#   1. window is the bare launcher (no surface)      -> promote it over the data, as a dropped table does
#   2. its bbox overlaps the displayed grid/image    -> line / polygon overlay in THIS window
#   3. an X,Y plot window is open                    -> a NEW PAGE there (X,Y law: one window, many pages)
#   4. none of the above                             -> say so in the window's Errors tab

# Serial number for pasted objects, so successive pastes get distinct Scene Objects names.
const _PASTE_N = Ref(0)

# Called on the UI thread from inside the Qt pump when Ctrl+V (or File > Paste) fires on a viewer
# window. Exactly one of `rgb` (w*h*nb packed bytes, top row first, borrowed for this call only)
# and `ctext` carries the payload.
function _on_paste(scene::Ptr{Cvoid}, ctext::Cstring, rgb::Ptr{UInt8}, w::Cint, h::Cint, nb::Cint)::Cvoid
	try
		if rgb != C_NULL && w > 0 && h > 0 && nb > 0
			_paste_image(scene, rgb, Int(w), Int(h), Int(nb))
		else
			_paste_text(scene, unsafe_string(ctext))
		end
	catch e
		_tool_failed(scene, "Paste", e)
	end
	return
end

# A pasted image has no georeference, so it lands in PIXEL coordinates (x = 1…w, y = 1…h) — exactly
# where a dropped unreferenced .png lands, and through exactly the same builder: the empty launcher
# is promoted in place, a populated window gets a managed image object in its Scene Objects panel.
function _paste_image(scene::Ptr{Cvoid}, rgb::Ptr{UInt8}, w::Int, h::Int, nb::Int)
	view = unsafe_wrap(Array, rgb, (nb, w, h))     # (band, col, row), C memory, borrowed
	mat  = permutedims(view, (3, 2, 1))            # (row, col, band), owned copy
	I    = GMT.mat2img(mat)
	_PASTE_N[] += 1
	name  = "Clipboard image $(_PASTE_N[])"
	empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
	_drop_into(scene, I, name; promote=empty)
	empty && ccall(_fn(:gmtvtk_set_title_h), Cvoid, (Ptr{Cvoid}, Cstring), scene, "i'GMT -- $name")
	# A row the user cannot see is the same as no row at all (SACRED_LAW.md, derived-variable law).
	ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
	_viewer_log_info(scene, "Pasted a $(w)x$(h) image -> \"$name\"")
	return
end

# Clipboard TEXT. A lone file path is opened as if the file had been dropped; anything else must
# parse as a numeric table, which is then routed by the rules at the top of this file.
function _paste_text(scene::Ptr{Cvoid}, txt::AbstractString)
	p = strip(strip(txt), '"')
	if !occursin('\n', p) && !isempty(p) && isfile(p)
		_on_drop(scene, p)                       # a copied path == that file dropped here
		return
	end
	segs, cn = _paste_parse_table(txt)
	if isempty(segs)
		_viewer_log_error(scene, "Paste: the clipboard holds no image and no numeric table")
		return
	end
	_PASTE_N[] += 1
	name = "Pasted data $(_PASTE_N[])"
	ncol = size(segs[1], 2)
	if ncol >= 2
		D = _paste_to_dataset(segs, cn)
		if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
			_drop_into(scene, D, name; promote=true)          # bare launcher -> frame it over the data
			ccall(_fn(:gmtvtk_set_title_h), Cvoid, (Ptr{Cvoid}, Cstring), scene, "i'GMT -- $name")
			ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
			_viewer_log_info(scene, "Pasted $(sum(size.(segs, 1))) points -> \"$name\"")
			return
		end
		if _paste_fits_window(scene, D)
			_add_dataset_to_scene(scene, D, name)             # line / polygon overlay in THIS window
			ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
			_viewer_log_info(scene, "Pasted $(sum(size.(segs, 1))) points -> \"$name\"")
			return
		end
	end
	# Outside the displayed region (or a single column, which is no map object at all): the X,Y tool
	# is where such numbers belong — if one is open.
	if _paste_to_xyplot(_paste_to_dataset(segs, cn), name)
		_viewer_log_info(scene, "Pasted \"$name\" into the X,Y plot window (new page)")
		return
	end
	W, E, S, N = _dataset_bbox(_paste_to_dataset(segs, cn))
	_viewer_log_error(scene, ncol >= 2 ?
		"Paste: the data ($(W)/$(E)/$(S)/$(N)) lies outside this window, and no X,Y plot window is open" :
		"Paste: single-column data needs an X,Y plot window (Tools > X,Y plot), none is open")
	return
end

# GMTdataset (one segment) or Vector{GMTdataset} (several) from the parsed segments. A SINGLE-column
# paste is not a map object: it gets a synthetic 1…n index as X so the X,Y tool can plot it.
function _paste_to_dataset(segs::Vector{Matrix{Float64}}, cn::Vector{String})
	if size(segs[1], 2) == 1
		segs = [hcat(collect(1.0:size(m, 1)), m) for m in segs]
		cn   = length(cn) == 1 ? ["index", cn[1]] : String[]
	end
	names = length(cn) == size(segs[1], 2) ? cn : String[]
	mk(m) = isempty(names) ? GMT.mat2ds(m) : GMT.mat2ds(m; colnames=names)
	return length(segs) == 1 ? mk(segs[1]) : [mk(m) for m in segs]
end

# Does the pasted data land where the window is looking? True when its bbox OVERLAPS the displayed
# grid/image extent (`x0..x1`,`y0..y1` of the window's own scene state) — a line that crosses the
# map only partly is still worth drawing; one entirely elsewhere is not.
function _paste_fits_window(scene::Ptr{Cvoid}, D)::Bool
	st = try _scene_state(scene) catch; return false end
	_num(k) = (v = get(st, k, nothing); v isa Real ? Float64(v) : NaN)
	get(st, "has_surface", 0) == 1 || return false
	x0, x1, y0, y1 = _num("x0"), _num("x1"), _num("y0"), _num("y1")
	any(isnan, (x0, x1, y0, y1)) && return false
	W, E, S, N = _dataset_bbox(D)
	return !(E < x0 || W > x1 || N < y0 || S > y1)
end

# Hand the data to the X,Y plot tool as a NEW PAGE of the window that is already open (the X,Y law:
# one tool window, many pages — never a second instance). False when no X,Y window is alive.
function _paste_to_xyplot(D, name::AbstractString)::Bool
	h = _XY_CURRENT[]
	h == C_NULL && return false
	ccall(_fn(:gmtvtk_xyplot_is_alive), Cint, (Ptr{Cvoid},), h) == 0 && return false
	p = get(_FIGREG, h, nothing)
	p isa QtXYPlot || return false
	ccall(_fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), h, String(name))
	_xy_add_dataset(p, D)
	ccall(_fn(:gmtvtk_xyplot_raise), Cvoid, (Ptr{Cvoid},), h)
	return true
end

# Parse clipboard text into numeric segments. THE ONE clipboard-table reader (the 3-D viewer's paste
# and the X,Y tool's both call it). Tolerant of what a real clipboard carries: whitespace, tab,
# comma or semicolon separators; '#' comments; a leading non-numeric line taken as column NAMES (an
# Excel/CSV header); GMT '>' segment headers and blank lines both break the segment; ragged rows are
# clipped to the column count of the first data row. Returns (segments, colnames) — an empty segment
# vector means "this is not a numeric table".
function _paste_parse_table(txt::AbstractString)
	segs = Matrix{Float64}[]
	rows = Vector{Float64}[]
	cn   = String[]
	ncol = 0
	sawnum = false
	function flush!()
		isempty(rows) && return
		m = Matrix{Float64}(undef, length(rows), ncol)
		for (i, r) in enumerate(rows), j in 1:ncol
			m[i, j] = r[j]
		end
		push!(segs, m)
		empty!(rows)
		return
	end
	for ln in split(String(txt), r"\r\n|\n|\r")
		t = strip(ln)
		if isempty(t) || startswith(t, '>')
			flush!()                                  # blank line / GMT header -> next segment
			continue
		end
		startswith(t, '#') && continue
		toks = split(t, r"[\s,;]+"; keepempty=false)
		vals = Vector{Float64}(undef, length(toks))
		ok = true
		for (i, tk) in enumerate(toks)
			v = tryparse(Float64, tk)
			if v === nothing
				ok = false
				break
			end
			vals[i] = v
		end
		if !ok
			# A header line before any numbers names the columns; a non-numeric line further down is
			# just noise (a units row, a legend) and is skipped.
			(!sawnum && isempty(cn)) && (cn = String.(toks))
			continue
		end
		sawnum = true
		if ncol == 0
			ncol = length(vals)
		elseif length(vals) < ncol
			continue                                   # short row -> drop it
		elseif length(vals) > ncol
			resize!(vals, ncol)                        # long row -> keep the common columns
		end
		push!(rows, vals)
	end
	flush!()
	return segs, cn
end

# Parse clipboard text straight into a plottable dataset (X,Y tool side). `nothing` when the text
# is not a numeric table.
function _paste_dataset(txt::AbstractString)
	segs, cn = _paste_parse_table(txt)
	isempty(segs) && return nothing
	return _paste_to_dataset(segs, cn)
end

# Build the C-callable pointer and install it in the DLL. Lazy (first window) via _ensure_callbacks.
function _register_paste_callback()
	fptr = @cfunction((s,t,p,w,h,n)->Base.invokelatest(_on_paste,s,t,p,w,h,n),
	                  Cvoid, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint, Cint, Cint))
	ccall(_fn(:gmtvtk_set_paste_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
