# movieanno.jl — movie frame LABELS (GMT movie -L) and PROGRESS INDICATORS (-P).
#
# Both are first-class scene elements with their own Scene Objects handle (checkbox, properties,
# Remove) — see 58_movieanno.cpp. `movie()` never creates a private overlay of its own: it pushes each
# frame's string and progress fraction into the annotations the window already carries, so what a
# frame shows is exactly what the user placed and can see live before spending a render on it.
#
# THE STRING IS COMPUTED HERE, not in C++. Only this side knows the frame table, the elapsed-time
# scale (-L+s) and the C-format (+t); deriving any of it a second time in the viewer would be the same
# quantity computed by two functions. C++ owns placement and drawing, nothing else.
#
# One parser serves both option surfaces: the keyword form (`kind=:frame, justify=:TL`) and the raw
# GMT spec (`"f+jTL+gwhite"`) end at the same `_AnnoSpec`, so the two can never read a modifier
# differently.

# GMT's `+t` is a C-format supplied at RUN time, so `@sprintf` (which needs a literal) cannot apply it
# — `Printf.format(Printf.Format(fmt), v)` can. Printf is not a direct dependency of this package;
# reached through GMT, the same route empilhador.jl takes for `@sprintf` and `Dates`.
using GMT: Printf

# GMT's +j reference points, in the order the C side indexes them (0..8 = TL TC TR ML MC MR BL BC BR).
const _ANNO_JUST = Dict{String,Int}(
	"TL" => 0, "TC" => 1, "TR" => 2,
	"ML" => 3, "MC" => 4, "MR" => 5,
	"BL" => 6, "BC" => 7, "BR" => 8)

# The label sources GMT's -L accepts, as the integer the C side stores for its properties dialog.
const _ANNO_SRC = Dict{Symbol,Int}(
	:elapsed => 0, :frame => 1, :percent => 2, :string => 3, :column => 4, :word => 5)

"""One movie annotation, host side: its viewer id plus everything needed to render its text."""
mutable struct _AnnoSpec
	id::Int                 # the viewer's own handle id (gmtvtk_anno_add_h)
	progress::Bool          # false = -L label, true = -P indicator
	source::Int             # 0 elapsed, 1 frame, 2 percent, 3 fixed string, 4 column, 5 word
	fixed::String           # the -Ls<string> text
	format::String          # the -L+t / -P+t C-format ("" = this source's own default)
	scale::Float64          # -L+s / -P+s seconds per frame (NaN = 1/frame_rate, resolved at run time)
	col::Int                # 0-based column (-Lc) or word (-Lt) index
	owned::Bool             # created by a movie() call, so that call removes it again
end

# Per-window annotation registry. Keyed by the viewer handle, like every other per-window store here.
const _ANNOS = Dict{Ptr{Cvoid}, Vector{_AnnoSpec}}()

_anno_specs(h::Ptr{Cvoid}) = get!(() -> _AnnoSpec[], _ANNOS, h)

# Colour in the ONE form the C API takes: 3 doubles in 0..1. Every accepted spelling — a Symbol, a
# GMT colour string ("#ff0000", "255/0/0", "lightred"), an RGB tuple — goes through `_parse_gmt_color`
# (colors.jl), which is this package's only GMT-colour reader.
function _anno_rgb(c)::Vector{Float64}
	if c isa Tuple || c isa AbstractVector
		v = collect(Float64, c)
		length(v) == 3 || throw(ArgumentError("movie annotation: a colour tuple needs 3 components, got $(length(v))"))
		return [x <= 1 ? x : x / 255 for x in v]
	end
	u = _parse_gmt_color(c isa Symbol ? String(c) : String(c))
	return [u[1] / 255, u[2] / 255, u[3] / 255]
end

function _anno_just(j)::Int
	s = uppercase(strip(j isa Symbol ? String(j) : String(j)))
	if !haskey(_ANNO_JUST, s)
		known = join(sort(collect(keys(_ANNO_JUST))), " ")
		throw(ArgumentError("movie annotation: `justify` must be one of $known — got '$s'"))
	end
	return _ANNO_JUST[s]
end

# GMT font spec "size[,family][,colour]" (e.g. "12p,Helvetica,black"). The family is dropped: the
# viewer draws every annotation with one font, so honouring a family here would promise something the
# renderer does not deliver. Returns (sizeInPixels, rgb) with 0.0 meaning "auto".
function _anno_font(spec::String)
	parts = String.(split(spec, ','))
	sz = 0.0
	rgb = [0.0, 0.0, 0.0]
	if !isempty(parts) && !isempty(strip(parts[1]))
		t = strip(parts[1])
		endswith(t, "p") && (t = t[1:prevind(t, lastindex(t))])
		v = tryparse(Float64, t)
		v === nothing || (sz = v)
	end
	length(parts) >= 3 && !isempty(strip(parts[3])) && (rgb = _anno_rgb(String(strip(parts[3]))))
	return sz, rgb
end

# GMT pen spec "width[,colour][,style]" (e.g. "1p,black"). Style is dropped for the same reason the
# font family is. Returns (widthInPixels, rgb).
function _anno_pen(spec::String)
	parts = String.(split(spec, ','))
	w = 1.0
	rgb = [0.0, 0.0, 0.0]
	if !isempty(parts) && !isempty(strip(parts[1]))
		t = strip(parts[1])
		endswith(t, "p") && (t = t[1:prevind(t, lastindex(t))])
		v = tryparse(Float64, t)
		v === nothing || (w = v)
	end
	length(parts) >= 2 && !isempty(strip(parts[2])) && (rgb = _anno_rgb(String(strip(parts[2]))))
	return w, rgb
end

# Split a GMT option string into its leading argument and its `+x<value>` modifiers.
#
# A naive split on '+' would cut a fixed label string containing one ("-Ls50+ years") in half, which
# is why only a '+' FOLLOWED BY ONE OF THIS OPTION'S OWN modifier letters starts a modifier. `valid`
# is that option's letter set, so -L and -P each get their own reading of the same character.
function _anno_split_mods(spec::String, valid::String)
	mods = Dict{Char,String}()
	i = firstindex(spec)
	cut = lastindex(spec) + 1
	while i <= lastindex(spec)
		c = spec[i]
		if c == '+'
			j = nextind(spec, i)
			if j <= lastindex(spec) && occursin(spec[j], valid)
				cut = min(cut, i)
				k = nextind(spec, j)
				e = k
				while e <= lastindex(spec)
					if spec[e] == '+' && nextind(spec, e) <= lastindex(spec) && occursin(spec[nextind(spec, e)], valid)
						break
					end
					e = nextind(spec, e)
				end
				mods[spec[j]] = String(spec[k:prevind(spec, e)])
				i = e
				continue
			end
		end
		i = nextind(spec, i)
	end
	main = cut > lastindex(spec) ? spec : String(spec[firstindex(spec):prevind(spec, cut)])
	return main, mods
end

# THE renderer of an annotation's text for one frame. Every source and both option surfaces come
# through here, so a label and an indicator's +a annotation of the same kind can never print
# differently. `rate` supplies GMT's default elapsed scale of 1/frame_rate.
function _anno_text(a::_AnnoSpec, f::MovieFrame, rate::Float64)::String
	a.source == 3 && return a.fixed
	fmt = a.format
	if a.source == 0                                    # elapsed time
		scl = isnan(a.scale) ? (rate > 0 ? 1 / rate : 1.0) : a.scale
		return _anno_fmt(isempty(fmt) ? "%g" : fmt, f.frame * scl)
	elseif a.source == 1                                # running frame number (GMT counts from 0)
		return _anno_fmt(isempty(fmt) ? "%d" : fmt, f.frame)
	elseif a.source == 2                                # percent complete
		# Whole percent by default. "%g" over 100*(61/97) prints "62.8866", which is not a progress
		# reading — and inside a circular indicator, whose annotation GMT sizes from the INDICATOR
		# rather than from the string, six digits also overflow the ring they sit in.
		return _anno_fmt(isempty(fmt) ? "%.0f" : fmt, 100 * f.progress)
	elseif a.source == 4                                # value of column `col` (0-based, like GMT)
		(0 <= a.col < length(f.cols)) ||
			throw(ArgumentError("movie label: column $(a.col) is out of range for a frame with $(length(f.cols)) column(s)"))
		return _anno_fmt(fmt, f.cols[a.col + 1])
	end
	(0 <= a.col < length(f.words)) ||                   # word `col` of the trailing text
		throw(ArgumentError("movie label: word $(a.col) is out of range for a frame with $(length(f.words)) word(s)"))
	return _anno_fmt(fmt, f.words[a.col + 1])
end

# Apply a C-format at run time. An empty format prints the value as it stands, which is the only
# sensible default for a table column that may hold text.
function _anno_fmt(fmt::String, v)::String
	isempty(fmt) && return string(v)
	# "%d" over a Float64 (or "%g" over an Int) throws in Printf, and a frame table read as numbers
	# hands over Float64 for what the user thinks of as an integer column. Coerce to what the format
	# asks for rather than making the user match Julia's types to their format string.
	if v isa Real
		if occursin(r"%[-+ #0-9.]*[dic]", fmt)
			return Printf.format(Printf.Format(fmt), round(Int, v))
		elseif occursin(r"%[-+ #0-9.]*[eEfgG]", fmt)
			return Printf.format(Printf.Format(fmt), Float64(v))
		end
	end
	return Printf.format(Printf.Format(fmt), v)
end

# Everything the C API needs for one annotation, defaulted. Keyword handlers and the GMT-spec parser
# both fill this in, so `_anno_push!` is the ONE place that talks to the viewer.
Base.@kwdef mutable struct _AnnoOpts
	progress::Bool  = false
	source::Int     = 1
	fixed::String   = ""
	format::String  = ""
	scale::Float64  = NaN
	col::Int        = 0
	style::Int      = 0
	annot::Bool     = false
	just::Int       = 0
	offx::Float64   = 0.0
	offy::Float64   = 0.0
	width::Float64  = 0.0
	fontsize::Float64 = 0.0
	fontrgb::Vector{Float64} = [0.0, 0.0, 0.0]
	hasfill::Bool   = false
	fillrgb::Vector{Float64} = [1.0, 1.0, 1.0]
	haspen::Bool    = false
	penrgb::Vector{Float64}  = [0.0, 0.0, 0.0]
	penwidth::Float64 = 1.0
	clearance::Float64 = 0.0
	rounded::Bool   = false
	hasfg::Bool     = false
	fgrgb::Vector{Float64} = [1.0, 0.0, 0.0]
	fgwidth::Float64 = 0.0
	hasbg::Bool     = false
	bgrgb::Vector{Float64} = [0.0, 0.0, 0.0]
	bgwidth::Float64 = 0.0
	name::String    = ""
end

# Create the element in the viewer and register its host-side spec. The single door: both public
# constructors and the movie() keyword path end here.
function _anno_push!(fig::_MovieFigure, o::_AnnoOpts; owned::Bool = false)::Int
	isalive(fig) || error("movie annotation: the viewer window is closed")
	h = _fig_handle(fig)
	nm = isempty(o.name) ? _anno_default_name(o) : o.name
	id = ccall(_fn(:gmtvtk_anno_add_h), Cint,
	           (Ptr{Cvoid}, Cstring, Cint, Cint, Cint, Cint, Cint,
	            Cdouble, Cdouble, Cdouble, Cdouble, Ptr{Cdouble},
	            Cint, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Cdouble,
	            Cdouble, Cint, Cint, Ptr{Cdouble}, Cdouble, Cint, Ptr{Cdouble}, Cdouble),
	           h, nm, Cint(o.progress), Cint(o.source), Cint(o.style), Cint(o.annot), Cint(o.just),
	           o.offx, o.offy, o.width, o.fontsize, o.fontrgb,
	           Cint(o.hasfill), o.fillrgb, Cint(o.haspen), o.penrgb, o.penwidth,
	           o.clearance, Cint(o.rounded),
	           Cint(o.hasfg), o.fgrgb, o.fgwidth, Cint(o.hasbg), o.bgrgb, o.bgwidth)
	id > 0 || error("movie annotation: the viewer refused to create it (window closed?)")
	push!(_anno_specs(h), _AnnoSpec(Int(id), o.progress, o.source, o.fixed, o.format, o.scale, o.col, owned))
	return Int(id)
end

function _anno_default_name(o::_AnnoOpts)::String
	o.progress && return "Progress " * string(Char('a' + o.style))
	o.source == 0 && return "Elapsed time"
	o.source == 1 && return "Frame number"
	o.source == 2 && return "Percent"
	o.source == 3 && return isempty(o.fixed) ? "Label" : o.fixed
	o.source == 4 && return "Column $(o.col)"
	return "Word $(o.col)"
end

# The shared placement/box keywords, applied to whichever kind is being built. One function so a
# label and an indicator can never disagree about what `justify` or `offset` mean.
function _anno_common!(o::_AnnoOpts; justify = nothing, offset = nothing, font = nothing,
                       fontsize = nothing, color = nothing, format = nothing, scale = nothing,
                       name = nothing)
	justify  === nothing || (o.just = _anno_just(justify))
	if offset !== nothing
		v = offset isa Number ? (Float64(offset), Float64(offset)) :
		    (Float64(offset[1]), Float64(length(offset) > 1 ? offset[2] : offset[1]))
		o.offx, o.offy = v
	end
	if font !== nothing
		sz, rgb = _anno_font(font isa Symbol ? String(font) : String(font))
		sz > 0 && (o.fontsize = sz)
		o.fontrgb = rgb
	end
	fontsize === nothing || (o.fontsize = Float64(fontsize))
	color    === nothing || (o.fontrgb  = _anno_rgb(color))
	format   === nothing || (o.format   = String(format))
	scale    === nothing || (o.scale    = Float64(scale))
	name     === nothing || (o.name     = String(name))
	return o
end

"""
	add_label!(fig; kind=:frame, …) -> Int

Add a movie frame label (GMT `movie -L`) to a live window and return its id. The label is a real
scene element: it gets its own row under "Movie annotations" in Scene Objects, with a checkbox, a
properties menu and Remove, and it is visible immediately — you can place it before spending a render
on it. `movie()` fills in its text frame by frame.

`kind` picks what the label says, matching GMT's `-L`:

| `kind` | GMT | shows |
|---|---|---|
| `:elapsed` | `e` | elapsed time, `scale` seconds per frame [`1/frame_rate`] |
| `:frame` | `f` | the running frame number (0-based, like GMT) |
| `:percent` | `p` | percent of the sequence completed |
| `:string` | `s` | the fixed `text` |
| `:column` | `c` | `frames` column `column` (0-based) |
| `:word` | `t` | word `column` of the frame's trailing text |

Keywords: `text` (for `:string`), `column`, `format` (a C-format applied to the value, GMT `+t`),
`scale` (`+s`), `justify` (`+j`, one of `TL TC TR ML MC MR BL BC BR`), `offset` (`+o`, pixels, a
number or a pair), `font` (a GMT font spec, `+f`), `fontsize`/`color` (the same in pieces), `fill`
(`+g`), `pen`/`penwidth` (`+p`), `clearance` (`+c`), `rounded` (`+r`), and `name` for the Scene
Objects row.

A raw GMT spec works too and parses to exactly the same element: `add_label!(fig, "f+jTL+gwhite")`.
"""
function add_label!(fig::_MovieFigure; kind = :frame, text = "", column::Integer = 0,
                    format = nothing, scale = nothing, justify = nothing, offset = nothing,
                    font = nothing, fontsize = nothing, color = nothing,
                    fill = nothing, pen = nothing, penwidth = nothing,
                    clearance = nothing, rounded::Bool = false, name = nothing)
	k = Symbol(kind)
	haskey(_ANNO_SRC, k) ||
		throw(ArgumentError("add_label!: `kind` must be one of :elapsed, :frame, :percent, :string, :column, :word — got :$k"))
	o = _AnnoOpts(progress = false, source = _ANNO_SRC[k], fixed = String(text), col = Int(column),
	              rounded = rounded)
	_anno_common!(o; justify = justify, offset = offset, font = font, fontsize = fontsize,
	              color = color, format = format, scale = scale, name = name)
	if fill !== nothing
		o.hasfill = true;  o.fillrgb = _anno_rgb(fill)
	end
	if pen !== nothing
		w, rgb = _anno_pen(pen isa Symbol ? String(pen) : String(pen))
		o.haspen = true;  o.penrgb = rgb;  o.penwidth = w
	end
	penwidth  === nothing || (o.haspen = true; o.penwidth = Float64(penwidth))
	clearance === nothing || (o.clearance = Float64(clearance))
	return _anno_push!(fig, o)
end

add_label!(fig::_MovieFigure, spec::String; name = nothing) =
	_anno_push!(fig, _anno_parse_label(spec, name))

"""
	add_progress!(fig; style=:a, …) -> Int

Add a movie progress indicator (GMT `movie -P`) to a live window and return its id. Like `add_label!`
it is a real scene element with its own Scene Objects row, properties and Remove.

`style` selects one of GMT's six indicators; `a`–`c` are circular, `d`–`f` are axis-like:

| `style` | look | static (`static`) | moving (`moving`) |
|---|---|---|---|
| `:a` | filled disc with a growing wedge | fill, lightgreen | fill, lightred |
| `:b` | ring with a growing arc | pen, lightblue | pen, blue |
| `:c` | circular arrow | dashed pen, darkred | pen + arrow head, red |
| `:d` | rounded line with a cross-mark | pen, black | pen, yellow |
| `:e` | plain axis | pen, red | pen, lightgreen |
| `:f` | axis with a moving triangle | pen, black | fill, red |

All of them grow clockwise from 12 o'clock (circular) or left to right (linear). Circular indicators
default to `justify=:TR` at 5 % of the larger window dimension; linear ones to `justify=:BC` at 60 %
of the window width — pass `justify` and `width` (pixels, GMT `+w`) to override.

Set `annot=true` (GMT `+a`) to give the indicator a label of its own, driven by the same `kind` /
`column` / `format` / `scale` keywords `add_label!` takes. Style `a` has no label in GMT and gets none
here. `moving`/`static` set the two colours (`+g`/`+G` or `+p`/`+P`), `movingwidth`/`staticwidth`
their pen widths.

A raw GMT spec works too: `add_progress!(fig, "b+w200+jBL")`.
"""
function add_progress!(fig::_MovieFigure; style = :a, annot::Bool = false, kind = :percent,
                       column::Integer = 0, format = nothing, scale = nothing,
                       justify = nothing, offset = nothing, width = nothing,
                       moving = nothing, static = nothing,
                       movingwidth = nothing, staticwidth = nothing,
                       font = nothing, fontsize = nothing, color = nothing, name = nothing)
	st = _anno_style(style)
	k = Symbol(kind)
	haskey(_ANNO_SRC, k) ||
		throw(ArgumentError("add_progress!: `kind` must be one of :elapsed, :frame, :percent, :column, :word — got :$k"))
	o = _AnnoOpts(progress = true, style = st, annot = annot, source = _ANNO_SRC[k], col = Int(column))
	# GMT's own default reference points: circles top-right, axes bottom-centre.
	o.just = st <= 2 ? _ANNO_JUST["TR"] : _ANNO_JUST["BC"]
	_anno_common!(o; justify = justify, offset = offset, font = font, fontsize = fontsize,
	              color = color, format = format, scale = scale, name = name)
	width       === nothing || (o.width = Float64(width))
	if moving !== nothing
		o.hasfg = true;  o.fgrgb = _anno_rgb(moving)
	end
	if static !== nothing
		o.hasbg = true;  o.bgrgb = _anno_rgb(static)
	end
	movingwidth === nothing || (o.fgwidth = Float64(movingwidth))
	staticwidth === nothing || (o.bgwidth = Float64(staticwidth))
	return _anno_push!(fig, o)
end

add_progress!(fig::_MovieFigure, spec::String; name = nothing) =
	_anno_push!(fig, _anno_parse_progress(spec, name))

function _anno_style(style)::Int
	s = style isa Integer ? Int(style) : Int(only(lowercase(strip(String(style isa Symbol ? String(style) : style)))) - 'a')
	(0 <= s <= 5) ||
		throw(ArgumentError("movie progress: `style` must be one of :a … :f, got $(repr(style))"))
	return s
end

"""
	remove_annotation!(fig, id) -> Bool

Remove one movie label or progress indicator by the id its constructor returned. Removing it from
Scene Objects (its row's Remove) does the same thing; both go through the viewer's own remove, so the
host registry cannot outlive the element.
"""
function remove_annotation!(fig::_MovieFigure, id::Integer)::Bool
	isalive(fig) || return false
	h = _fig_handle(fig)
	ok = ccall(_fn(:gmtvtk_anno_remove_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(id)) != 0
	v = get(_ANNOS, h, nothing)
	v === nothing || filter!(a -> a.id != Int(id), v)
	return ok
end

"""
	movie_annotations(fig) -> Vector{Int}

The ids of the movie labels and progress indicators this window carries, oldest first.
"""
function movie_annotations(fig::_MovieFigure)::Vector{Int}
	isalive(fig) || return Int[]
	_anno_sync!(_fig_handle(fig))
	return [a.id for a in _anno_specs(_fig_handle(fig))]
end

# Drop host specs the viewer no longer has — a row removed through Scene Objects goes straight to
# `movieAnnoRemove` without passing through Julia, so the registry has to be reconciled against the
# viewer's own count rather than trusted. Cheap, and it runs once per movie, not per frame.
function _anno_sync!(h::Ptr{Cvoid})
	v = get(_ANNOS, h, nothing)
	v === nothing && return
	n = ccall(_fn(:gmtvtk_anno_count_h), Cint, (Ptr{Cvoid},), h)
	if n < 0
		delete!(_ANNOS, h)                              # the window is gone
	elseif Int(n) != length(v)
		# The viewer is the authority on which ids exist; ask it about each one by trying to address it.
		# A NULL text with haveFrac=0 changes nothing — it is a pure "do you still have this id?".
		# Declared Ptr{Cchar}, not Cstring: only a pointer type takes C_NULL.
		filter!(a -> ccall(_fn(:gmtvtk_anno_set_h), Cint,
		                   (Ptr{Cvoid}, Cint, Ptr{Cchar}, Cdouble, Cint),
		                   h, Cint(a.id), C_NULL, 0.0, Cint(0)) != 0, v)
	end
	return
end

# Push one frame into every annotation of a window. Called once per movie frame, before the capture.
function _anno_frame!(h::Ptr{Cvoid}, f::MovieFrame, rate::Float64)
	v = get(_ANNOS, h, nothing)
	(v === nothing || isempty(v)) && return
	for a in v
		txt = _anno_text(a, f, rate)
		ccall(_fn(:gmtvtk_anno_set_h), Cint, (Ptr{Cvoid}, Cint, Cstring, Cdouble, Cint),
		      h, Cint(a.id), txt, f.progress, Cint(1))
	end
	return
end

# What `movie(...; label=…, progress=…)` accepts, per element: a raw GMT spec string ("f+jTL"), a
# NamedTuple of the constructor's own keywords, a Symbol (the label kind / the indicator style),
# `true` for that kind's defaults, or a vector of any of those — GMT's -L and -P are both repeatable,
# so a vector is the natural spelling of "two of them".
#
# It builds through the SAME public constructors a user calls by hand, so an annotation made by movie()
# and one placed beforehand are the same element with the same defaults, differing only in who removes
# it. Returns the ids this call owns.
function _movie_make_annos!(fig::_MovieFigure, label, progress)::Vector{Int}
	ids = Int[]
	for (spec, isprog) in ((label, false), (progress, true))
		spec === nothing && continue
		items = (spec isa AbstractVector || spec isa Tuple) && !(spec isa NamedTuple) ? collect(spec) : Any[spec]
		for it in items
			it === nothing && continue
			it === false && continue
			o = if it isa AbstractString
				isprog ? _anno_parse_progress(String(it)) : _anno_parse_label(String(it))
			elseif it isa NamedTuple
				_movie_anno_from_kw(isprog, it)
			elseif it isa Symbol
				isprog ? _movie_anno_from_kw(true, (style = it,)) : _movie_anno_from_kw(false, (kind = it,))
			elseif it === true
				_movie_anno_from_kw(isprog, NamedTuple())
			else
				throw(ArgumentError("movie: a `$(isprog ? "progress" : "label")` entry must be a GMT spec string, " *
				                    "a NamedTuple of keywords, a Symbol or `true` — got $(typeof(it))"))
			end
			push!(ids, _anno_push!(fig, o; owned = true))
		end
	end
	return ids
end

# A NamedTuple of the public constructor's keywords -> the resolved options, without going through the
# constructor itself (which would create the element before we can mark it owned). Keeping this a thin
# forward of the same keyword names is what stops the two paths from drifting.
function _movie_anno_from_kw(isprog::Bool, kw::NamedTuple)::_AnnoOpts
	if isprog
		o = _AnnoOpts(progress = true, source = _ANNO_SRC[:percent],
		              style = haskey(kw, :style) ? _anno_style(kw.style) : 0,
		              annot = get(kw, :annot, false),
		              col   = Int(get(kw, :column, 0)))
		haskey(kw, :kind) && (o.source = _anno_src(kw.kind, "progress"))
		o.just = o.style <= 2 ? _ANNO_JUST["TR"] : _ANNO_JUST["BC"]
		_anno_common!(o; justify = get(kw, :justify, nothing), offset = get(kw, :offset, nothing),
		              font = get(kw, :font, nothing), fontsize = get(kw, :fontsize, nothing),
		              color = get(kw, :color, nothing), format = get(kw, :format, nothing),
		              scale = get(kw, :scale, nothing), name = get(kw, :name, nothing))
		haskey(kw, :width)  && (o.width = Float64(kw.width))
		haskey(kw, :moving) && (o.hasfg = true; o.fgrgb = _anno_rgb(kw.moving))
		haskey(kw, :static) && (o.hasbg = true; o.bgrgb = _anno_rgb(kw.static))
		haskey(kw, :movingwidth) && (o.fgwidth = Float64(kw.movingwidth))
		haskey(kw, :staticwidth) && (o.bgwidth = Float64(kw.staticwidth))
		return o
	end
	o = _AnnoOpts(progress = false,
	              source = haskey(kw, :kind) ? _anno_src(kw.kind, "label") : _ANNO_SRC[:frame],
	              fixed  = String(get(kw, :text, "")),
	              col    = Int(get(kw, :column, 0)),
	              rounded = get(kw, :rounded, false))
	_anno_common!(o; justify = get(kw, :justify, nothing), offset = get(kw, :offset, nothing),
	              font = get(kw, :font, nothing), fontsize = get(kw, :fontsize, nothing),
	              color = get(kw, :color, nothing), format = get(kw, :format, nothing),
	              scale = get(kw, :scale, nothing), name = get(kw, :name, nothing))
	haskey(kw, :fill) && (o.hasfill = true; o.fillrgb = _anno_rgb(kw.fill))
	if haskey(kw, :pen)
		w, rgb = _anno_pen(kw.pen isa Symbol ? String(kw.pen) : String(kw.pen))
		o.haspen = true;  o.penrgb = rgb;  o.penwidth = w
	end
	haskey(kw, :penwidth)  && (o.haspen = true; o.penwidth = Float64(kw.penwidth))
	haskey(kw, :clearance) && (o.clearance = Float64(kw.clearance))
	return o
end

function _anno_src(kind, who::String)::Int
	k = Symbol(kind)
	haskey(_ANNO_SRC, k) ||
		throw(ArgumentError("movie $who: `kind` must be one of :elapsed, :frame, :percent, :string, :column, :word — got :$k"))
	return _ANNO_SRC[k]
end

# ---- GMT spec parsing -------------------------------------------------------------------------
# `-L<labelinfo>`: e | f | p | s<string> | c<col> | t<col>, then +c +f +g +h +j +o +p +r +s +t.
# `+h` (the textbox drop shadow) is accepted and ignored — the viewer draws no shadow, and silently
# dropping a modifier the user typed is better than refusing the whole spec over decoration.
function _anno_parse_label(spec::String, name = nothing)::_AnnoOpts
	main, mods = _anno_split_mods(String(strip(spec)), "cfghjoprst")
	isempty(main) && throw(ArgumentError("movie label: empty spec — expected e, f, p, s<string>, c<col> or t<col>"))
	o = _AnnoOpts(progress = false)
	c = main[1]
	rest = String(main[nextind(main, 1):end])
	if c == 'e'
		o.source = 0
	elseif c == 'f'
		o.source = 1
	elseif c == 'p'
		o.source = 2
	elseif c == 's'
		o.source = 3;  o.fixed = rest
	elseif c == 'c' || c == 't'
		o.source = (c == 'c') ? 4 : 5
		v = tryparse(Int, strip(rest))
		v === nothing && throw(ArgumentError("movie label: '$c' needs a column number, got \"$rest\""))
		o.col = v
	else
		throw(ArgumentError("movie label: unknown kind '$c' — expected e, f, p, s, c or t"))
	end
	_anno_apply_mods!(o, mods)
	name === nothing || (o.name = String(name))
	return o
end

# `-P<progressinfo>`: a..f, then +a +f +G +g +j +o +P +p +s +t +w. Note the CASE split GMT uses —
# lower case is the MOVING look, upper case the STATIC one.
function _anno_parse_progress(spec::String, name = nothing)::_AnnoOpts
	main, mods = _anno_split_mods(String(strip(spec)), "afGgjoPpstw")
	o = _AnnoOpts(progress = true, source = _ANNO_SRC[:percent])
	o.style = isempty(main) ? 0 : _anno_style(String(main[1:1]))
	o.just  = o.style <= 2 ? _ANNO_JUST["TR"] : _ANNO_JUST["BC"]
	if haskey(mods, 'a')                                # +a[e|f|p|c<col>]: annotate, and with what
		o.annot = true
		av = strip(mods['a'])
		if !isempty(av)
			ac = av[1]
			ac == 'e' && (o.source = 0)
			ac == 'f' && (o.source = 1)
			ac == 'p' && (o.source = 2)
			if ac == 'c'
				o.source = 4
				v = tryparse(Int, String(av[nextind(av, 1):end]))
				v === nothing && throw(ArgumentError("movie progress: +ac needs a column number, got \"$av\""))
				o.col = v
			end
		end
	end
	haskey(mods, 'w') && (o.width = _anno_len(mods['w']))
	haskey(mods, 'g') && (o.hasfg = true; o.fgrgb = _anno_rgb(String(mods['g'])))
	haskey(mods, 'G') && (o.hasbg = true; o.bgrgb = _anno_rgb(String(mods['G'])))
	if haskey(mods, 'p')
		w, rgb = _anno_pen(String(mods['p']));  o.hasfg = true;  o.fgrgb = rgb;  o.fgwidth = w
	end
	if haskey(mods, 'P')
		w, rgb = _anno_pen(String(mods['P']));  o.hasbg = true;  o.bgrgb = rgb;  o.bgwidth = w
	end
	_anno_apply_mods!(o, mods; skip = "aGgPpw")
	name === nothing || (o.name = String(name))
	return o
end

# The modifiers -L and -P spell the same way, applied once so the two options cannot drift.
function _anno_apply_mods!(o::_AnnoOpts, mods::Dict{Char,String}; skip::String = "")
	for (k, v) in mods
		occursin(k, skip) && continue
		if k == 'j'
			o.just = _anno_just(String(v))
		elseif k == 'o'
			p = String.(split(v, '/'))
			o.offx = _anno_len(p[1])
			o.offy = length(p) > 1 ? _anno_len(p[2]) : o.offx
		elseif k == 'f'
			sz, rgb = _anno_font(String(v))
			sz > 0 && (o.fontsize = sz)
			o.fontrgb = rgb
		elseif k == 'g'
			o.hasfill = true;  o.fillrgb = _anno_rgb(String(v))
		elseif k == 'p'
			w, rgb = _anno_pen(String(v));  o.haspen = true;  o.penrgb = rgb;  o.penwidth = w
		elseif k == 'c'
			p = String.(split(v, '/'))
			o.clearance = _anno_len(p[1])
		elseif k == 'r'
			o.rounded = true
		elseif k == 't'
			o.format = String(v)
		elseif k == 's'
			x = tryparse(Float64, strip(v))
			x === nothing || (o.scale = x)
		end
		# 'h' (drop shadow) falls through deliberately — see _anno_parse_label.
	end
	return o
end

# A GMT length ("20", "20p", "0.5i", "1c") in the PIXELS this viewer places things with. Points are
# pixels here; inches and centimetres convert at 72 points to the inch, which is what GMT's own
# point-based lengths mean.
function _anno_len(s::String)::Float64
	t = strip(s)
	isempty(t) && return 0.0
	u = t[end]
	if u == 'p' || u == 'i' || u == 'c'
		v = tryparse(Float64, String(t[1:prevind(t, lastindex(t))]))
		v === nothing && throw(ArgumentError("movie annotation: cannot read the length \"$s\""))
		u == 'i' && return v * 72
		u == 'c' && return v * 72 / 2.54
		return v
	end
	v = tryparse(Float64, t)
	v === nothing && throw(ArgumentError("movie annotation: cannot read the length \"$s\""))
	return v
end
