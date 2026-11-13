# The WORKSPACE dock (View > Workspace Panel, 77_workspace.cpp): MATLAB's workspace browser for this
# Julia session. It lists the variables of `Main` — the module the REPL and the in-window console both
# evaluate in — and shows one as a table (THE shared table dialog, `show_table`) or as a picture,
# through the same `_drop_into` + `_adopt_new_element` door a dropped file goes through.
#
# One C callback serves the whole panel (`_on_workspace`); its answer travels in its own buffer, never
# as captured console output.

const _WS_OUT  = Ref{String}("")        # the answer of the last request (two-phase buffer)
const _WS_LIST = Ref{String}("")        # the last listing, reused while the bindings look unchanged
const _WS_SIG  = Ref{UInt}(0)
# The names a SCRIPT host (the desktop launcher, iview_app.jl, or any `julia script.jl`) had put in
# Main before its first window opened: the host's own plumbing (flag paths, timers), not variables the
# user made. Filled when the callbacks register (first window); a REPL host leaves it empty.
const _WS_BASE = Set{Symbol}()

# Not variables: modules, functions and types live in Main too, and a workspace does not list them.
_ws_hidden(v) = v isa Module || v isa Function || v isa Type

# THE WINDOW'S RASTERS AS VARIABLES. Every grid and image a window lists in Scene Objects is bound to a
# variable of Main, so the console can compute with it by name (`mask = layer0 .> 100`) and the panel
# lists it under "This window". The data bound is the object the window remembers for that row
# (`_SCENE_OBJS`, the registry Save / the Grid Calculator read) -- an image at full precision (its
# 16-bit original when it has one) -- never a copy.
#   sym -> (scene, Scene Objects label, the bound object, made by this panel)
# The last flag is false for a variable the USER had made and then put into the window ("Show as image
# in this window" on `mask`): it is listed under the window like the others, but when its row goes it
# is only let go of, never cleared -- it was the user's before the window had it.
const _WS_BOUND = Dict{Symbol,Tuple{Ptr{Cvoid},String,Any,Bool}}()
# Names this panel bound and later released (row removed, window closed): Julia cannot delete a global,
# so the name is left holding `nothing`, and it is not listed while it does.
const _WS_DEAD = Set{Symbol}()

# The grids and images `scene` shows in Scene Objects, as [(label, data)] in Scene Objects order. A
# registered object whose row is gone is not one: the base counts while the window has a surface, an
# extra while the window still lists a grid/image row of that name.
function _ws_scene_objects(scene::Ptr{Cvoid})
	out = Tuple{String,Any}[]
	(scene == C_NULL || !haskey(_SCENE_OBJS, scene)) && return out
	st = try _scene_state(scene) catch; return out end
	# The rows, as Scene Objects lists them: the base (under the window's own label for it), then the
	# grid/image extras.
	surf = String(get(st, "surf_name", ""))
	rows = Tuple{String,Vector{Symbol}}[]
	# (an unnamed base is "Surface", as the panel and the Grid calculator's baseName() call it)
	get(st, "has_surface", 0) == 1 && push!(rows, (isempty(surf) ? "Surface" : surf, [:grid, :image]))
	for (k, n) in st["extras"]
		k == "grid"  && push!(rows, (n, [:grid]))
		k == "image" && push!(rows, (n, [:image]))
	end
	origs = get(_IMG_ORIG, scene, Dict{String,GMTimage}())
	for (label, kinds) in rows
		any(t -> t[1] == label, out) && continue
		for kind in kinds
			# THE name lookup (savefile.jl): it already resolves the base whether it was registered
			# under "" or under its own name -- never a second resolver here.
			# An unnamed base has no label to look up: "" is the shared key for the window's primary.
			d = isempty(surf) && label == "Surface" ? _find_object(scene, kind, "") :
			                                          _find_object_exact(scene, kind, label)
			d === nothing && continue
			if d isa GMTimage                     # its full-precision original, when it has one
				d = get(origs, label, label == surf ? get(origs, "", d) : d)
			end
			push!(out, (label, d))
			break
		end
	end
	return out
end

# A Scene Objects label as a Julia name: a file extension dropped ("layer0.grd" -> layer0), every run of
# other characters made one '_' ("Base image (global)" -> Base_image_global), a leading digit prefixed.
function _ws_ident(label::AbstractString)::String
	b, e = splitext(label)
	s = (!isempty(b) && occursin(r"^\.[A-Za-z][A-Za-z0-9]{0,4}$", e)) ? b : String(label)
	s = strip(replace(s, r"[^A-Za-z0-9_]+" => "_"), '_')
	isempty(s) && (s = "obj")
	isdigit(s[1]) && (s = "x" * s)
	(try Meta.parse(s) catch; nothing end) === Symbol(s) || (s *= "_")      # `end`, `for`, `true`...
	return s
end

# Is `sym` free for this panel to bind? Not defined in Main, or one of the names it released earlier
# and still holding `nothing`.
_ws_free(sym::Symbol) = !isdefined(Main, sym) || (sym in _WS_DEAD && getfield(Main, sym) === nothing)

function _ws_setglobal(sym::Symbol, v)
	Core.eval(Main, Expr(:(=), sym, QuoteNode(v)))
	return
end

# Bring Main's bindings in line with what the windows show: bind every raster `scene` lists that has no
# variable yet, re-point a variable whose row now holds a different object (a recompute under the same
# name), and release the variables of rows that are gone -- this window's, and those of any window
# that has closed. A variable the user has since assigned something else to is THEIRS: it is left alone
# and the panel stops managing it.
function _ws_sync_scene!(scene::Ptr{Cvoid})
	objs = _ws_scene_objects(scene)
	for (sym, (sc, label, data, made)) in collect(_WS_BOUND)
		cur = isdefined(Main, sym) ? getfield(Main, sym) : nothing
		if cur !== data                                # reassigned by the user: no longer ours
			delete!(_WS_BOUND, sym)
			continue
		end
		gone = !haskey(_SCENE_OBJS, sc)                # its window has closed
		if sc == scene
			i = findfirst(t -> t[1] == label, objs)
			if i === nothing
				gone = true
			elseif objs[i][2] !== data                 # same row, new object: follow it
				_ws_setglobal(sym, objs[i][2])
				_WS_BOUND[sym] = (sc, label, objs[i][2], made)
			end
		end
		if gone
			delete!(_WS_BOUND, sym)
			made || continue                           # the user's own variable: let go, untouched
			_ws_setglobal(sym, nothing)
			push!(_WS_DEAD, sym)
		end
	end
	for (label, data) in objs
		any(t -> t[1] == scene && t[2] == label, values(_WS_BOUND)) && continue
		base = Symbol(_ws_ident(label))
		# the user's own variable put into the window under its name: it IS that row, adopt it
		if !haskey(_WS_BOUND, base) && isdefined(Main, base) && getfield(Main, base) === data
			_WS_BOUND[base] = (scene, label, data, false)
			continue
		end
		sym, k = base, 1
		while !_ws_free(sym) || haskey(_WS_BOUND, sym)
			k += 1
			sym = Symbol(base, '_', k)
		end
		_ws_setglobal(sym, data)
		_WS_BOUND[sym] = (scene, label, data, true)
		delete!(_WS_DEAD, sym)
	end
	return
end

# The variables of Main as (name, value), sorted by name. The window rasters bound above are not
# among them: each is listed by its own window's panel, under "This window".
function _ws_vars()
	out = Tuple{Symbol,Any}[]
	for n in names(Main; all=true)
		startswith(String(n), '#') && continue       # compiler-generated names
		n in _WS_BASE && continue                    # the script host's own globals
		haskey(_WS_BOUND, n) && continue             # a window's raster
		isdefined(Main, n) || continue
		v = getfield(Main, n)
		(n in _WS_DEAD && v === nothing) && continue  # a released raster name
		_ws_hidden(v) || push!(out, (n, v))
	end
	return sort!(out; by=first)
end

# This window's bound rasters as (name, value, Scene Objects label), sorted by name.
_ws_scene_vars(scene::Ptr{Cvoid}) =
	sort!([(sym, t[3], t[2]) for (sym, t) in _WS_BOUND if t[1] == scene]; by=first)

# What the panel can do with a value: "grid", "image", "dataset", "dataframe", "matrix", "vector", "fv"
# or "other".
function _ws_kind(v)::String
	v isa GMTgrid  && return ndims(v.z) == 2 ? "grid" : "other"     # a cube has no single table
	v isa GMTimage && return "image"
	v isa GMTfv    && return "fv"
	(v isa GMTdataset || v isa AbstractVector{<:GMTdataset}) && return "dataset"
	GMT.isdataframe(v) && return "dataframe"                        # a table (show_table reads it)
	v isa AbstractMatrix{<:Real} && return "matrix"
	v isa AbstractVector{<:Real} && return "vector"
	return "other"
end

_ws_nbands(v) = v isa GMTimage && ndims(v.image) == 3 ? size(v.image, 3) : 1

function _ws_size(v)::String
	v isa GMTgrid && ndims(v.z) == 2 && return join(_grid_dims(v), '×')
	if v isa GMTimage
		rowmajor = length(v.layout) >= 2 && v.layout[2] == 'R'
		s1, s2 = size(v.image, 1), size(v.image, 2)
		nx, ny = rowmajor ? (s1, s2) : (s2, s1)
		nb = _ws_nbands(v)
		return nb > 1 ? "$(nx)×$(ny)×$(nb)" : "$(nx)×$(ny)"
	end
	v isa GMTdataset && return join(size(v.data), '×')
	GMT.isdataframe(v) && return "$(length(names(v)) == 0 ? 0 : length(v[!, names(v)[1]]))×$(length(names(v)))"
	v isa AbstractVector{<:GMTdataset} && return "$(length(v)) segment" * (length(v) == 1 ? "" : "s")
	v isa AbstractArray && return join(size(v), '×')
	v isa AbstractString && return "$(length(v)) char" * (length(v) == 1 ? "" : "s")
	return "1×1"
end

# The short value MATLAB shows beside a name: numbers, strings, symbols and small numeric arrays only.
function _ws_value(v)::String
	shown = v isa Union{Number,AbstractString,Symbol,Char,Nothing,Missing} ||
	        (v isa AbstractArray{<:Number} && length(v) <= 8) ||
	        (v isa Tuple && length(v) <= 8)
	shown || return ""
	s = sprint(show, v; context=(:compact => true, :limit => true))
	s = replace(s, '\t' => ' ', '\n' => ' ', '\r' => ' ')
	return length(s) > 60 ? first(s, 59) * "…" : s
end

function _ws_type(v)::String
	s = replace(string(typeof(v)), "GMT." => "")
	return length(s) > 48 ? first(s, 47) * "…" : s
end

# A cheap fingerprint of the bindings: names, identities and array shapes. When it has not moved, the
# last listing is answered again without re-measuring every variable.
function _ws_signature(vars)::UInt
	h = hash(length(vars))
	for (n, v) in vars
		h = hash(n, hash(objectid(v), h))
		v isa AbstractArray && (h = hash(size(v), h))
		v isa GMTdataset    && (h = hash(size(v.data), h))
	end
	return h
end

# One line per variable: name \t value \t size \t type \t bytes \t kind \t bands \t nbytes \t origin
# (77_workspace.cpp). `origin` is the Scene Objects label of a window raster ("" for a plain variable):
# those rows come first and the panel groups them under "This window".
function _ws_listing(scene::Ptr{Cvoid}, force::Bool)::String
	_ws_sync_scene!(scene)
	svars = _ws_scene_vars(scene)
	vars  = _ws_vars()
	sig   = hash(scene, hash([(n, l) for (n, _, l) in svars],
	             _ws_signature(vcat([(n, v) for (n, v, _) in svars], vars))))
	(!force && sig == _WS_SIG[]) && return _WS_LIST[]
	io = IOBuffer()
	for (n, v, origin) in vcat(svars, [(n, v, "") for (n, v) in vars])
		nbytes = try Base.summarysize(v) catch; 0 end
		println(io, n, '\t', _ws_value(v), '\t', _ws_size(v), '\t', _ws_type(v), '\t',
		        Base.format_bytes(nbytes), '\t', _ws_kind(v), '\t', _ws_nbands(v), '\t', nbytes, '\t',
		        replace(origin, '\t' => ' ', '\n' => ' '))
	end
	_WS_SIG[]  = sig
	_WS_LIST[] = String(take!(io))
	return _WS_LIST[]
end

function _ws_get(name::AbstractString)
	s = Symbol(name)
	isdefined(Main, s) || error("there is no variable `$name` any more")
	return getfield(Main, s)
end

# A variable INTO window `scene`, exactly as a dropped file lands (`_open_spec_into`): the window
# promoted if it is still an empty launcher, a raster re-framed onto its own axes, a table overlaid
# on what is there. A plain matrix becomes a grid first, as `iview(M)` makes it.
function _ws_into_window(scene::Ptr{Cvoid}, name::String, v)
	# A logical result (`mask = G .> 100` -> a Bool grid, or a Bool image) is a MASK IMAGE, delivered
	# by THE mask door (savefile.jl `_commit_mask!`), the one every mask-making tool uses.
	v isa GMTgrid{Bool,2} && (v = _gc_mask_image(v))
	if v isa GMTimage{Bool}
		_commit_mask!(scene, v, name) == 1 || error("could not put the mask `$name` in the window")
		return
	end
	data  = v isa AbstractMatrix{<:Real} ? GMT.mat2grid(v isa Array ? v : Matrix(v)) : v
	empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
	ok = _drop_into(scene, data, name; promote=empty)
	is_table = data isa GMTdataset || data isa AbstractVector{<:GMTdataset}
	(ok === false || is_table) || _adopt_new_element(scene, name, data)
	empty && ccall(_fn(:gmtvtk_set_title_h), Cvoid, (Ptr{Cvoid}, Cstring), scene, "i'GMT -- $name")
	return
end

function _ws_request(scene::Ptr{Cvoid}, req::String)::String
	req == "list"  && return _ws_listing(scene, false)
	req == "list!" && return _ws_listing(scene, true)
	f = split(req, '\t')
	length(f) >= 2 || error("bad Workspace request \"$req\"")
	op, name = String(f[1]), String(f[2])
	arg = length(f) >= 3 ? String(f[3]) : ""
	v = _ws_get(name)
	if op == "table"
		if v isa GMTimage
			b  = isempty(arg) ? 1 : parse(Int, arg)
			nb = _ws_nbands(v)
			show_table(scene, _ImageBand(v, b); name = nb > 1 ? "$name (band $b of $nb)" : name)
		else
			show_table(scene, v; name = name)
		end
	elseif op == "here"
		_ws_into_window(scene, name, v)
	elseif op == "plot"
		# "Plot it": a vector (against its sample number) or a dataset (column 1 = X) in the X,Y tool
		v isa AbstractVector{<:Real} ? _xyplot(v; name = name) : _xyplot(v)
	elseif op == "new"
		# a solid has a viewer of its own; everything else goes into a fresh empty window by the same
		# door as "this window"
		v isa GMTfv ? viz(v) : _ws_into_window(getfield(iview(), :h), name, v)
	elseif op == "summary"
		# GMT.jl prints its own types' text (a grid's header, a dataset's bounding box) straight to
		# stdout, so that is captured with the console's own capture (console.jl), and it comes first,
		# as it would in the REPL. A grid or image is described by its header alone (GMT.info without
		# the data): its numbers are what the table is for.
		st, note = _console_capture_file(1)
		s, printed = "", ""
		try
			v isa Union{GMTgrid,GMTimage} ? GMT.info(v, false) :
				(s = sprint(show, MIME"text/plain"(), v; context=(:limit => true, :displaysize => (60, 120))))
		finally
			printed = _console_capture_file_end(st)
		end
		s = note * printed * s
		return length(s) > 20000 ? first(s, 20000) * "\n…" : s
	else
		error("unknown Workspace action \"$op\"")
	end
	return ""
end

# The C callback (JuliaWorkspaceFn, 77_workspace.cpp). Two-phase: out == NULL runs the request and
# returns the byte length of its answer; the second call copies it. -1 = failure, reason in `err`.
# Never throws (an exception escaping a callback wedges the pump).
function _on_workspace(scene::Ptr{Cvoid}, creq::Cstring, out::Ptr{UInt8}, cap::Cint,
                       err::Ptr{UInt8}, errcap::Cint)::Cint
	req = ""
	try
		req = unsafe_string(creq)
		# In the LATEST world, whoever calls: since Julia 1.12 a variable the user has just created is
		# invisible to code still running in an older world (isdefined says false), and a workspace
		# must see what was typed a moment ago.
		out == C_NULL && (_WS_OUT[] = Base.invokelatest(_ws_request, scene, req))
		s = _WS_OUT[]
		n = sizeof(s)
		out == C_NULL && return Cint(n)
		cap >= n || error("Workspace buffer too small ($n bytes needed)")
		GC.@preserve s unsafe_copyto!(out, pointer(s), n)
		return Cint(n)
	catch e
		startswith(req, "list") || _tool_failed(scene, "Workspace", e)
		nb = _console_write(err, errcap, sprint(showerror, e))
		errcap > 0 && unsafe_store!(err, 0x00, Int(nb) + 1)
		return Cint(-1)
	end
end

function _register_workspace()
	haskey(_LIB_FNS, :gmtvtk_set_workspace_callback) || return
	isempty(PROGRAM_FILE) || union!(_WS_BASE, Base.invokelatest(names, Main; all=true))
	fptr = @cfunction((s, r, o, c, e, ec) -> Base.invokelatest(_on_workspace, s, r, o, c, e, ec),
		Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_workspace_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
