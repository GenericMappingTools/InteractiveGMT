# gmtsyntax.jl — ONE NAME PER JOB. Where GMT.jl already has a function for a job, iGMT does that job
# under GMT.jl's NAME and with GMT.jl's SYNTAX, never under a second name of its own:
#
#   plot / plot!   the X,Y plot tool          (iGMT's own: shadows GMT.plot / GMT.plot!)
#   feather        stick (vector) diagrams    (iGMT's own: shadows GMT.feather)
#   viz            the 3-D viewer front door  (iGMT's own: shadows GMT.viz; `iview` is its alias)
#   scatter!       symbols on an open window  (a METHOD of GMT.scatter!, on iGMT's window types)
#   orbits!        a satellite ground track   (a METHOD of GMT.orbits!, on iGMT's Satellite)
#   bar!           bars on an X,Y plot window (a METHOD of GMT.bar!, on QtXYPlot)
#
# The front doors only TRANSLATE GMT.jl's keywords and units and call the implementation (_xyplot,
# _xyadd!, _stickplot, _view_*, _add_symbols!, _plot_groundtrack!), which every menu and tool calls too.
# A GMT.jl keyword the interactive window cannot honour is an ERROR that names it, never silently
# dropped. The iGMT-only keywords (xtime, track, trackgeog, scale, z, srs, info, ...) are the agreed
# exceptions: GMT.jl has no keyword for those jobs.

_gmt_kw_error(fname, k) =
	error("$fname: keyword `$k` is not supported by iGMT's interactive $fname; use GMT.$fname for a static figure")

# The keywords the implementation `f` accepts for argument types `T`; any other one is an error.
function _kwcheck(fname::Symbol, f, T::Type, kw)
	ok = Base.kwarg_decl(which(f, T))
	any(s -> endswith(String(s), "..."), ok) && return nothing     # a slurping method decides itself
	for k in keys(kw)
		k in ok || _gmt_kw_error(fname, k)
	end
	return nothing
end

# GMT.jl lengths: a number is in the keyword's default unit (pen widths in points, symbol sizes in cm),
# a string carries its own unit ("1p", "0.25c", "0.1i"). Screen px at 96 dpi, the viewer's own unit.
_unit_px(x::Float64, u::Char) = u == 'p' ? x * 96 / 72 : u == 'c' ? x * 96 / 2.54 : x * 96
function _gmt_len_px(fname, k, v, defunit::Char)
	v isa Real && return _unit_px(Float64(v), defunit)
	s = strip(string(v))
	u = isempty(s) ? defunit : last(s)
	x = tryparse(Float64, u in ('p', 'c', 'i') ? s[1:end-1] : s)
	x === nothing && error("$fname: cannot read `$k=$(repr(v))` as a length")
	return _unit_px(x, u in ('p', 'c', 'i') ? u : defunit)
end

const _GMT_LS = Dict("-" => :solid, "solid" => :solid, "--" => :dash, "dash" => :dash, "dashed" => :dash,
                     "." => :dot, ":" => :dot, "dot" => :dot, "dotted" => :dot,
                     "-." => :dashdot, ".-" => :dashdot, "dashdot" => :dashdot,
                     "-.." => :dashdotdot, "dashdotdot" => :dashdotdot, "none" => :none)
_gmt_ls(fname, v) = get(_GMT_LS, lowercase(string(v))) do
	error("$fname: line style $(repr(v)) is not supported here (have $(join(sort(collect(keys(_GMT_LS))), ", ")))")
end

const _GMT_MARKER = Dict("c" => :circle, "circ" => :circle, "circle" => :circle,
                         "s" => :square, "square" => :square, "d" => :diamond, "diamond" => :diamond,
                         "x" => :cross, "cross" => :cross, "xcross" => :cross, "+" => :plus, "plus" => :plus)
_gmt_marker(fname, v) = get(_GMT_MARKER, lowercase(string(v))) do
	error("$fname: marker $(repr(v)) is not supported in the X,Y plot (have circle, square, diamond, cross, plus)")
end

# pen=w | (w,) | (w, color) | (w, color, style) | "w,color,style" -> the parts that are given.
function _gmt_pen(fname, v)
	p = v isa Tuple ? collect(Any, v) : v isa AbstractString ? Any[strip(s) for s in split(v, ',')] : Any[v]
	length(p) > 3 && error("$fname: pen $(repr(v)) has more than width, colour and style")
	w = (length(p) >= 1 && p[1] != "") ? _gmt_len_px(fname, :pen, p[1], 'p') : nothing
	c = (length(p) >= 2 && p[2] != "") ? p[2] : nothing
	s = (length(p) >= 3 && p[3] != "") ? _gmt_ls(fname, p[3]) : nothing
	return w, c, s
end

# GMT.jl's plot keywords -> the X,Y tool's. `adding` = plot!: the window's title/labels/axes stay.
function _gmt_xy_kw(fname::Symbol, kw; adding::Bool=false)
	o = Dict{Symbol,Any}()
	lc = mc = nothing
	for (k, v) in pairs(kw)
		if k in (:legend, :l)
			o[:name] = string(v)
		elseif k in (:lc, :linecolor)
			lc = v
		elseif k in (:lw, :lt, :linewidth, :linethick, :linethickness)
			o[:linewidth] = _gmt_len_px(fname, k, v, 'p')
		elseif k in (:ls, :linestyle)
			o[:linestyle] = _gmt_ls(fname, v)
		elseif k === :pen
			w, c, s = _gmt_pen(fname, v)
			w === nothing || (o[:linewidth] = w)
			c === nothing || (lc = c)
			s === nothing || (o[:linestyle] = s)
		elseif k in (:marker, :Marker, :shape)
			o[:marker] = _gmt_marker(fname, v)
		elseif k in (:ms, :markersize, :MarkerSize, :size)
			o[:markersize] = _gmt_len_px(fname, k, v, 'c')
		elseif k in (:mc, :markercolor, :markerfacecolor, :MarkerFaceColor)
			mc = v
		elseif k in (:multi, :multicol, :multicols)
			o[:multicol] = v === true || v == 1
		elseif !adding && k in (:title, :xlabel, :ylabel)
			o[k] = string(v)
		elseif !adding && k in (:proj, :J)
			s = lowercase(string(v))
			s in ("logx", "loglog", "logxy") && (o[:xscale] = :log)
			s in ("logy", "loglog", "logxy") && (o[:yscale] = :log)
			s in ("logx", "logy", "loglog", "logxy", "linear", "x") || _gmt_kw_error(fname, "$k=$(repr(v))")
		elseif k in (:xtime, :track, :trackgeog)       # iGMT-only: no GMT.jl keyword for these jobs
			o[k] = v
		elseif k === :show                              # GMT.jl's "display it": a live window always is
		else
			_gmt_kw_error(fname, k)
		end
	end
	(lc !== nothing && mc !== nothing && lc != mc) &&
		error("$fname: a series has ONE colour in the X,Y plot; lc=$(repr(lc)) and mc=$(repr(mc)) differ")
	c = lc !== nothing ? lc : mc
	c === nothing || (o[:color] = c)
	return o
end

const _XY_SERIES_KW = (:color, :linewidth, :linestyle, :marker, :markersize)

# What plot/plot! draw, in GMT.jl's reading: column 2 against column 1, and with `multicol=true` every
# further column its own line. -> segments of (x, [y columns], [names], xlabel).
_xy_cols(n::Int, multi::Bool) = multi ? (2:n) : (2:min(n, 2))
_gmt_xy_segs(y::AbstractVector{<:Real}, multi::Bool) = [(collect(1.0:length(y)), [collect(Float64, y)], [""], "X")]
_gmt_xy_segs(x::AbstractVector, y::AbstractVector, multi::Bool) = [(collect(Float64, x), [collect(Float64, y)], [""], "X")]
function _gmt_xy_segs(x::AbstractVector, Y::AbstractMatrix, multi::Bool)
	cs = multi ? axes(Y, 2) : (1:1)
	return [(collect(Float64, x), [collect(Float64, @view Y[:, c]) for c in cs],
	         [multi ? "Line $c" : "" for c in cs], "X")]
end
function _gmt_xy_segs(M::AbstractMatrix{<:Real}, multi::Bool)
	size(M, 2) < 2 && error("plot: a matrix needs at least 2 columns (x, y)")
	return _gmt_xy_segs(@view(M[:, 1]), @view(M[:, 2:end]), multi)
end
function _gmt_xy_segs(D::GMTdataset{<:Real,2}, multi::Bool)
	n = size(D.data, 2)
	n < 2 && error("plot: a dataset needs at least 2 columns (x, y)")
	cn = _ds_colnames(D, n)
	cs = _xy_cols(n, multi)
	return [(collect(Float64, @view D.data[:, 1]), [collect(Float64, @view D.data[:, c]) for c in cs], cn[cs], cn[1])]
end
function _gmt_xy_segs(D::AbstractVector{<:GMTdataset}, multi::Bool)
	segs = [s for s in D if size(s.data, 2) >= 2]
	isempty(segs) && error("plot: no plottable (>= 2 column) segments")
	out = reduce(vcat, [_gmt_xy_segs(s, multi) for s in segs])
	length(out) > 1 && for (i, s) in enumerate(out)
		s[3] .= [isempty(nm) ? "seg $i" : "$nm (seg $i)" for nm in s[3]]
	end
	return out
end
_gmt_xy_segs(args...) = error("plot: cannot draw $(join(map(a -> string(typeof(a)), args[1:end-1]), ", "))")

# Push every series of `segs` into `p` (opening the window on the first one when `p` is nothing).
function _gmt_xy_draw(p, segs, o::Dict{Symbol,Any})
	legend = pop!(o, :name, nothing)
	xlab = pop!(o, :xlabel, nothing)
	track = pop!(o, :track, nothing)
	sk = (; (k => o[k] for k in _XY_SERIES_KW if haskey(o, k))...)
	nser = sum(s -> length(s[2]), segs)
	k = 0
	for (x, ys, nms, xl) in segs, (y, nm) in zip(ys, nms)
		k += 1
		name = legend === nothing ? nm : nser == 1 ? legend : "$legend $k"
		tk = k == 1 ? (; track, trackgeog = get(o, :trackgeog, true)) : (;)   # a track belongs to ONE series
		if p === nothing
			rest = (; (kk => v for (kk, v) in o if !(kk in _XY_SERIES_KW) && kk !== :trackgeog)...)
			p = _xyplot(x, y; name, xlabel = xlab === nothing ? xl : xlab, sk..., rest..., tk...)
		else
			_xyadd!(p, x, y; name, sk..., tk...)
		end
	end
	return p
end

"""
    plot(x, y; kw...)  /  plot(y; kw...)  /  plot(M; kw...)  /  plot(D; kw...) -> QtXYPlot

GMT.jl's `plot`, drawn in iGMT's interactive **X,Y plot** window (a new page of the open one, if
there is one). Same arguments and keywords as GMT.jl: column 2 against column 1 of a matrix or
`GMTdataset`, every further column too with `multicol=true`; `lc`/`linecolor`, `lw`/`linewidth`
(points), `ls`/`linestyle`, `pen=(w, color, style)`, `marker`, `ms`/`markersize` (cm), `mc`,
`legend`, `title`, `xlabel`, `ylabel`, `proj=:logx`/`:logy`/`:loglog`. iGMT-only: `xtime` (X is Unix
epoch seconds: `:date`, `:date_ymd`, `:time`, `:decyear`, `:doy`), `track`, `trackgeog`.

A GMT.jl keyword the window cannot honour (`region`, `frame`, `savefig`, ...) is an error; use
`GMT.plot` for a static figure. Add more lines with [`plot!`](@ref).

```julia
t = range(0, 4π; length=400)
p = plot(t, sin.(t); legend="sin", title="demo", ylabel="amplitude")
plot!(p, t, cos.(t); legend="cos", lc=:blue, ls=:dash)
```
"""
function plot(args...; kw...)
	o = _gmt_xy_kw(:plot, kw)
	return _gmt_xy_draw(nothing, _gmt_xy_segs(args..., pop!(o, :multicol, false)), o)
end

"""
    plot!(p::QtXYPlot, args...; kw...)  /  plot!(args...; kw...) -> QtXYPlot

Add lines to an open X,Y plot window: `p`, or without it the current one (the one `plot` last drew
in). Same arguments and line/marker keywords as [`plot`](@ref).
"""
function plot!(p::QtXYPlot, args...; kw...)
	isalive(p) || error("plot!: the X,Y plot window is closed")
	o = _gmt_xy_kw(:plot!, kw; adding = true)
	return _gmt_xy_draw(p, _gmt_xy_segs(args..., pop!(o, :multicol, false)), o)
end
plot!(args...; kw...) = plot!(_xy_current(:plot!), args...; kw...)

"""
    bar!(p::QtXYPlot, x, y; width=0, kw...) -> QtXYPlot

GMT.jl's `bar!`, onto an open X,Y plot window `p`: the `(x, y)` series drawn as a BAR graph.
`width` is the bar width in **x data units** (0 = 90 % of the median x spacing); `lc`, `lw` (the
bars' outline, points) and `legend` as in [`plot`](@ref).
"""
function bar!(p::QtXYPlot, x::AbstractVector, y::AbstractVector; width::Real = 0, kw...)
	isalive(p) || error("bar!: the X,Y plot window is closed")
	o = _gmt_xy_kw(:bar!, kw; adding = true)
	for k in keys(o)
		k in (:name, :color, :linewidth) || _gmt_kw_error(:bar!, k)
	end
	return _xyadd!(p, x, y; name = get(o, :name, ""), color = get(o, :color, nothing),
	               linewidth = get(o, :linewidth, 0), kind = :bar, barwidth = width)
end

function _xy_current(fname)
	h = _XY_CURRENT[]
	(h != C_NULL && ccall(_fn(:gmtvtk_xyplot_is_alive), Cint, (Ptr{Cvoid},), h) != 0) ||
		error("$fname: no X,Y plot window is open; draw one with plot first")
	return _FIGREG[h]::QtXYPlot
end

"""
    feather(M; kw...)  /  feather(t, UV; kw...) -> QtXYPlot

GMT.jl's `feather` — a **stick (vector) diagram** — in iGMT's interactive X,Y plot window. As in
GMT.jl the columns are `(x, u, v)` (or `(x, y, u, v)` with every `y` 0), and `rtheta=true` reads the
last two as `(θ, r)`: angle in degrees counter-clockwise from +x, and length. Line keywords as
[`plot`](@ref); iGMT-only: `scale` (`:auto` ≈ 5 % of the time span per full-length stick).
"""
function feather(args...; rtheta = nothing, scale = :auto, kw...)
	M = length(args) == 1 ? (args[1] isa GMTdataset ? args[1].data : args[1]) : hcat(args...)
	(M isa AbstractMatrix{<:Real}) || error("feather: give a matrix, a GMTdataset or (t, [u v])")
	n = size(M, 2)
	n in (3, 4) || error("feather: need 3 columns (x, u, v) or 4 (x, y, u, v); got $n")
	(n == 4 && any(!iszero, @view M[:, 2])) && error("feather: sticks start on the y = 0 baseline here; column 2 (y) must be 0")
	a, b = collect(Float64, @view M[:, n-1]), collect(Float64, @view M[:, n])
	u, v = (rtheta === nothing || rtheta === false) ? (a, b) : (b .* cosd.(a), b .* sind.(a))
	o = _gmt_xy_kw(:feather, kw)
	pop!(o, :multicol, nothing)
	return _stickplot(collect(Float64, @view M[:, 1]), u, v; scale, (; (kk => vv for (kk, vv) in o)...)...)
end

"Alias of [`viz`](@ref)."
const iview = viz

# The window types symbols can be stamped on.
const _QtWin = Union{QtFigure, QtImage, QtEmpty, QtPoints, QtFV}

"""
    scatter!(fig, x, y; kw...)  /  scatter!(fig, D::GMTdataset; kw...) -> fig

GMT.jl's `scatter!`, onto an open iGMT window `fig`: a screen-constant symbol at every point (true
data coordinates). GMT.jl keywords: `marker`/`shape` (`:circle`, `:square`, `:triangle`,
`:diamond`, `:star`, `:cross`, `:plus`, ... or a 1-char GMT code), `ms`/`markersize` (cm, or a
string with its unit), `mc`/`fill` (fill colour), `ml`/`markerline` (outline `w`, `color` or
`(w, color)`), `legend` (the Scene Objects name). iGMT-only: `z`, `srs` (the points' referencing
system; a dataset's own CRS is used by default), `info`.
"""
function scatter!(fig::_QtWin, x::AbstractVector, y::AbstractVector; kw...)
	o = Dict{Symbol,Any}(:sizeunit => :pt)
	for (k, v) in pairs(kw)
		if k in (:marker, :Marker, :shape)
			o[:symbol] = v
		elseif k in (:ms, :markersize, :MarkerSize, :size)
			o[:size] = v isa AbstractVector ? [_gmt_len_px(:scatter!, k, e, 'c') * 72 / 96 for e in v] :
			                                  _gmt_len_px(:scatter!, k, v, 'c') * 72 / 96
		elseif k in (:mc, :markercolor, :markerfacecolor, :MarkerFaceColor, :fill, :G)
			o[:fill] = v
		elseif k in (:ml, :markerline, :MarkerLine)
			w, c = v isa Tuple ? (v[1], length(v) > 1 ? v[2] : nothing) :
			       (v isa Real || (v isa AbstractString && tryparse(Float64, rstrip(v, ['p', 'c', 'i'])) !== nothing)) ?
			       (v, nothing) : (nothing, v)
			w === nothing || (o[:edgewidth] = _gmt_len_px(:scatter!, k, w, 'p'))
			c === nothing || (o[:edge] = c)
		elseif k in (:legend, :l)
			o[:name] = string(v)
		elseif k in (:z, :srs, :info)
			o[k] = v
		elseif k === :show
		else
			_gmt_kw_error(:scatter!, k)
		end
	end
	_add_symbols!(fig, x, y; (; (k => v for (k, v) in o)...)...)
	return fig
end
function scatter!(fig::_QtWin, D::GMTdataset; kw...)
	size(D.data, 2) < 2 && error("scatter!: a dataset needs at least 2 columns (x, y)")
	srs = !isempty(D.proj4) ? D.proj4 : !isempty(D.wkt) ? D.wkt : ""
	kwd = Dict{Symbol,Any}(pairs(kw))
	isempty(srs) || haskey(kwd, :srs) || (kwd[:srs] = srs)
	return scatter!(fig, view(D.data, :, 1), view(D.data, :, 2); (; (k => v for (k, v) in kwd)...)...)
end

"""
    orbits!(fig, sat::Satellite; kw...) -> Bool

GMT.jl's `orbits!` for a [`Satellite`](@ref): draw its ground track into the open iGMT window `fig`
as a vector overlay with its own Scene Objects row. `lc`/`linecolor` (or the colour of `pen`) sets
the line colour, `legend` the row name; `start`, `stop`, `step` and `revolutions` choose the stretch
of orbit (see [`groundtrack`](@ref)).
"""
function orbits!(fig::Union{_QtWin, Ptr{Cvoid}}, s::Satellite; kw...)
	o = Dict{Symbol,Any}()
	for (k, v) in pairs(kw)
		if k in (:lc, :linecolor)
			o[:color] = v
		elseif k === :pen
			_, c, _ = _gmt_pen(:orbits!, v)
			c === nothing || (o[:color] = c)
		elseif k in (:legend, :l)
			o[:name] = string(v)
		elseif k === :show
		else
			o[k] = v
		end
	end
	gk = (; (k => v for (k, v) in o if !(k in (:color, :name)))...)
	_kwcheck(:orbits!, groundtrack, Tuple{Satellite}, gk)
	return _plot_groundtrack!(_sym_ptr(fig), s; (; (k => v for (k, v) in o)...)...)
end
