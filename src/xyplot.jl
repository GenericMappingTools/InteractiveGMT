# xyplot.jl — the standalone X,Y plot tool (the evolution of the Profile). Opens a SEPARATE Qt
# window (vtkChartXY plot + Object Manager + foldable Data Viewer + menubar/toolbar); the C side
# (deps/src/65_xyplot.cpp) owns the GUI, Julia owns the data. Non-blocking: the shared Qt pump
# (eventloop.jl `_start_pump`) keeps the REPL alive — `gmtvtk_process_events` counts these windows
# too. File > Open/Save and the menu route back here through the `_on_xy` callback.

"""
    xyplot(x, y; name="", color=nothing, linewidth=0, title="", xlabel="X", ylabel="Y") -> QtXYPlot

Open a standalone **X,Y plot** window and draw the `(x, y)` series. `y` may be a matrix whose
columns are separate lines sharing the same `x`. `color` accepts a name (`:red`, `"blue"`), a
0–255 / 0–1 triple, or `nothing` for the default. `xlabel`/`ylabel` set the axis titles. Returns
a live [`QtXYPlot`](@ref) handle; add more lines with [`add!`](@ref). Non-blocking.

```julia
t = range(0, 4π; length=400) |> collect
p = xyplot(t, sin.(t); name="sin", title="demo", ylabel="amplitude")
add!(p, t, cos.(t); name="cos", color=:blue)
```
"""
# X-axis time modes -> the C `gmtvtk_xyplot_set_xtime` codes. X must be Unix epoch seconds.
const _XTIME = Dict(:linear => 0, :date => 1, :date_ymd => 2, :time => 3, :decyear => 4, :doy => 5)

# line style / marker keyword -> vtkPen / vtkPlotPoints codes (-1 / -1 = "keep the default").
const _LINESTYLE = Dict(:solid => 1, :dash => 2, :dashed => 2, :dot => 3, :dotted => 3,
                        :dashdot => 4, :dashdotdot => 5, :none => 0)
const _MARKER    = Dict(:none => 0, :cross => 1, :x => 1, :plus => 2, :square => 3,
                        :circle => 4, :o => 4, :diamond => 5)
_style_code(d, v, what) = v === nothing ? -1 :
	get(d, v, nothing) === nothing ? error("xyplot: unknown $what :$v (have $(sort(collect(keys(d)))))") : d[v]

function xyplot(x::AbstractVector, y::AbstractVecOrMat; name::AbstractString="", color=nothing,
                linewidth::Real=0, linestyle=nothing, marker=nothing, markersize::Real=0,
                title::AbstractString="",
                xlabel::AbstractString="X", ylabel::AbstractString="Y", xtime::Symbol=:linear,
                xscale::Symbol=:linear, yscale::Symbol=:linear)
	h = ccall(_fn(:gmtvtk_xyplot_open), Ptr{Cvoid}, (Cstring,), String(title))
	h == C_NULL && error("xyplot: failed to open the plot window")
	p = _register_fig!(QtXYPlot(h))
	_start_pump()
	_xy_set_labels(p, xlabel, ylabel)
	xtime === :linear || xtime!(p, xtime)
	_xy_add(p, x, y; name, color, linewidth, linestyle, marker, markersize)
	xscale === :log && logscale!(p; x=true)
	yscale === :log && logscale!(p; y=true)
	return p
end

"""
    logscale!(p::QtXYPlot; x=nothing, y=nothing) -> QtXYPlot

Set base-10 **log scaling** on the X and/or Y axis (`true` = log, `false` = linear, `nothing` =
leave unchanged). Data on a log axis must be positive.
"""
function logscale!(p::QtXYPlot; x::Union{Nothing,Bool}=nothing, y::Union{Nothing,Bool}=nothing)
	isalive(p) || return p
	x === nothing || ccall(_fn(:gmtvtk_xyplot_set_logscale), Cvoid, (Ptr{Cvoid}, Cint, Cint), p.h, Cint(0), Cint(x))
	y === nothing || ccall(_fn(:gmtvtk_xyplot_set_logscale), Cvoid, (Ptr{Cvoid}, Cint, Cint), p.h, Cint(1), Cint(y))
	return p
end

"""
    xtime!(p::QtXYPlot, mode) -> QtXYPlot

Format the X axis as time, reading X as **Unix epoch seconds**. `mode` is `:linear` (plain numbers),
`:date` (auto by span), `:date_ymd` (yyyy-mm-dd), `:time` (HH:MM), `:decyear` (decimal year) or
`:doy` (decimal day-of-year). Ticks auto-update on zoom/pan.
"""
function xtime!(p::QtXYPlot, mode::Symbol)
	code = get(_XTIME, mode, nothing)
	code === nothing && error("xtime!: unknown mode :$mode (have $(sort(collect(keys(_XTIME)))))")
	isalive(p) && ccall(_fn(:gmtvtk_xyplot_set_xtime), Cvoid, (Ptr{Cvoid}, Cint), p.h, Cint(code))
	return p
end

"""
    xyplot(D::GMTdataset; kwargs...) -> QtXYPlot
    xyplot(D::Vector{<:GMTdataset}; kwargs...) -> QtXYPlot

Open a `GMTdataset` (or a multi-segment vector) in the X,Y plot tool: column 1 is X, columns 2…n
are Y series; each segment of a vector becomes its own line. Same keywords as the vector form.
"""
function xyplot(D::GMTdataset; kwargs...)
	m = D.data
	size(m, 2) < 2 && error("xyplot: dataset needs at least 2 columns (x, y)")
	return xyplot(Float64.(@view m[:, 1]), Float64.(m[:, 2:end]); kwargs...)
end

function xyplot(D::AbstractVector{<:GMTdataset}; kwargs...)
	segs = [s for s in D if size(s.data, 2) >= 2]
	isempty(segs) && error("xyplot: no plottable (>= 2 column) segments")
	m1 = segs[1].data
	multi = length(segs) > 1
	p = xyplot(Float64.(@view m1[:, 1]), Float64.(m1[:, 2:end]);
	           (multi ? (; name="seg 1") : (;))..., kwargs...)
	for i in 2:length(segs)
		m = segs[i].data
		add!(p, Float64.(@view m[:, 1]), Float64.(m[:, 2:end]); name="seg $i")
	end
	return p
end

"""
    add!(p::QtXYPlot, x, y; name="", color=nothing, linewidth=0) -> QtXYPlot

Add another `(x, y)` line to an open X,Y plot window. Same keyword semantics as [`xyplot`](@ref).
"""
function add!(p::QtXYPlot, x::AbstractVector, y::AbstractVecOrMat; name::AbstractString="",
              color=nothing, linewidth::Real=0, linestyle=nothing, marker=nothing, markersize::Real=0)
	isalive(p) || error("add!: the X,Y plot window is closed")
	_xy_add(p, x, y; name, color, linewidth, linestyle, marker, markersize)
	return p
end

"""
    clear!(p::QtXYPlot) -> QtXYPlot

Remove every series from an open X,Y plot window.
"""
function clear!(p::QtXYPlot)
	isalive(p) && ccall(_fn(:gmtvtk_xyplot_clear), Cvoid, (Ptr{Cvoid},), p.h)
	empty!(p.series)
	return p
end

# Set the axis titles.
_xy_set_labels(p::QtXYPlot, xl, yl) =
	ccall(_fn(:gmtvtk_xyplot_set_labels), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), p.h, String(xl), String(yl))

# Push one (vector) or several (matrix-column) series to the window + the Julia-side copy.
function _xy_add(p::QtXYPlot, x::AbstractVector, y::AbstractVecOrMat;
                 name, color, linewidth, linestyle=nothing, marker=nothing, markersize::Real=0)
	xv = collect(Float64, x)
	(r, g, b) = color === nothing ? (-1.0, 0.0, 0.0) : _ovl_color(color, :lines)
	lt = _style_code(_LINESTYLE, linestyle, "linestyle")
	mk = _style_code(_MARKER, marker, "marker")
	Y = y isa AbstractVector ? reshape(collect(Float64, y), :, 1) : Matrix{Float64}(y)
	ncol = size(Y, 2)
	for c in 1:ncol
		yv = Vector{Float64}(@view Y[:, c])
		length(yv) == length(xv) ||
			error("xyplot: x and y must have equal length (got $(length(xv)) and $(length(yv)))")
		nm = ncol == 1 ? String(name) : (isempty(name) ? "Line $(length(p.series)+1)" : "$(name) $c")
		ccall(_fn(:gmtvtk_xyplot_add_series), Cint,
		      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cstring,
		       Float64, Float64, Float64, Float64, Cint, Cint, Float64),
		      p.h, xv, yv, length(xv), nm, Float64(r), Float64(g), Float64(b), Float64(linewidth),
		      Cint(lt), Cint(mk), Float64(markersize))
		push!(p.series, (xv, yv, nm))
	end
	return p
end

"""
    profile_to_xyplot(fig) -> QtXYPlot

Open the **current Profile** of a 3-D viewer window `fig` (a Ctrl+left-drag elevation profile, or a
downloaded tide series — whatever its bottom-dock Profile panel shows) in a standalone **X,Y plot
tool** window, with its Object Manager, Analysis menu and save. The programmatic twin of the
panel's right-click "Open in X,Y plot tool". `fig` is any 3-D viewer handle (`QtFigure`/…).
"""
function profile_to_xyplot(fig)
	# The C call spawns the window and (synchronously) calls `_on_xy_seed` back, which registers the
	# QtXYPlot mirror + adds the series. So after it returns the mirror is already in _FIGREG.
	h = ccall(_fn(:gmtvtk_open_profile_in_xyplot), Ptr{Cvoid}, (Ptr{Cvoid},), _fig_handle(fig))
	h == C_NULL && error("profile_to_xyplot: no profile to send (draw one with Ctrl+left-drag) or the window is closed")
	p = get(_FIGREG, h, nothing)
	p isa QtXYPlot && return p
	q = _register_fig!(QtXYPlot(h)); _start_pump(); return q   # fallback (seed cb not attached)
end

# Seed callback (C++ Profile -> X,Y tool spawn -> here): register a QtXYPlot mirror for the new
# window and add the handed (x,y) series, so File>Save / Analysis (Julia-side) work on it.
function _on_xy_seed(plot::Ptr{Cvoid}, cx::Ptr{Float64}, cy::Ptr{Float64}, n::Cint, cname::Cstring)::Cvoid
	try
		N = Int(n)
		x = copy(unsafe_wrap(Array, cx, N))
		y = copy(unsafe_wrap(Array, cy, N))
		nm = unsafe_string(cname)
		p = get(_FIGREG, plot, nothing)
		if !(p isa QtXYPlot)
			p = _register_fig!(QtXYPlot(plot))
			_start_pump()
		end
		add!(p, x, y; name=nm)
	catch e
		_dbg("xy-seed", "ERR", sprint(showerror, e))
		@warn "xyplot seed failed" exception=e
	end
	return
end

# Install the seed callback. Called once from __init__.
function _register_xy_seed()
	fptr = @cfunction(_on_xy_seed, Cvoid, (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cstring))
	ccall(_fn(:gmtvtk_xyplot_set_seed_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# "New blank X,Y window" callback (3-D viewer Tools > X,Y plot opened an empty window in C++):
# register a Julia mirror so its File>Open / Save / Analysis work.
function _on_xy_new(plot::Ptr{Cvoid})::Cvoid
	try
		get(_FIGREG, plot, nothing) isa QtXYPlot && return
		_register_fig!(QtXYPlot(plot))
		_start_pump()
	catch e
		_dbg("xy-new", "ERR", sprint(showerror, e))
		@warn "xyplot new-window registration failed" exception=e
	end
	return
end

function _register_xy_new()
	fptr = @cfunction(_on_xy_new, Cvoid, (Ptr{Cvoid},))
	ccall(_fn(:gmtvtk_xyplot_set_new_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ---- File menu callback (C++ File>Open/Save/New -> here) --------------------------------------

# Invoked on the UI thread from inside the Qt pump when a File-menu item is chosen. `action` is
# "open" | "save" | "new"; `sel` = selected series index (0-based, Save only; -1 = none); `path`
# = the file picked in the native dialog. Reentrant on the one Julia thread (Timer -> ccall -> Qt
# -> cfunction -> here -> ccall back), exactly like the console / drop callbacks.
function _on_xy(plot::Ptr{Cvoid}, caction::Cstring, sel::Cint, cpath::Cstring)::Cvoid
	action = unsafe_string(caction)
	path   = unsafe_string(cpath)
	try
		if action == "new"
			_xy_open_blank()
		elseif action == "open"
			p = get(_FIGREG, plot, nothing)
			(p isa QtXYPlot) && _xy_open_file(p, path)
		elseif action == "save"
			p = get(_FIGREG, plot, nothing)
			(p isa QtXYPlot) && _xy_save(p, Int(sel), path)
		end
	catch e
		_dbg("xy", action, "ERR", sprint(showerror, e))
		_xy_log(plot, "File $action FAILED: $(sprint(showerror, e))"; err=true)   # show IN the window
		@warn "xyplot $action failed" path exception=e
	end
	return
end

# File > New: open a fresh empty X,Y plot window.
function _xy_open_blank()
	h = ccall(_fn(:gmtvtk_xyplot_open), Ptr{Cvoid}, (Cstring,), "i'GMT  —  X,Y plot")
	h == C_NULL && return
	_register_fig!(QtXYPlot(h))
	_start_pump()
	return
end

# File > Open: read a data file (anything GMT.gmtread understands) and add its columns as series.
function _xy_open_file(p::QtXYPlot, path)
	D = GMT.gmtread(path)
	segs = (D isa GMTdataset || D isa AbstractMatrix) ? (D,) : collect(D)
	nseg = length(segs)
	for (i, seg) in enumerate(segs)
		m = seg isa GMTdataset ? seg.data : seg
		size(m, 2) < 2 && continue                       # need at least x,y
		x = Float64.(@view m[:, 1])
		Y = Float64.(m[:, 2:end])
		nm = nseg > 1 ? "$(basename(path)) seg $i" : basename(path)
		add!(p, x, Y; name=nm)
	end
	return
end

# Pull series `sel` (0-based) of the window's CURRENT page straight from the C side (it owns the
# vtkTables). Returns (x, y) Float64 vectors, or nothing if the handle/index is bad. With pages the
# Julia mirror no longer tracks per-page series, so Analysis / Save read the live page from here.
function _xy_get_series(plot::Ptr{Cvoid}, sel::Integer)
	n = ccall(_fn(:gmtvtk_xyplot_series_npoints), Cint, (Ptr{Cvoid}, Cint), plot, Cint(sel))
	n <= 0 && return nothing
	x = Vector{Float64}(undef, n); y = Vector{Float64}(undef, n)
	got = ccall(_fn(:gmtvtk_xyplot_get_series), Cint,
	            (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint), plot, Cint(sel), x, y, Cint(n))
	got <= 0 && return nothing
	return (resize!(x, got), resize!(y, got))
end

# Name of series `sel` (0-based) on the current page (C-owned). "" on a bad handle/index.
function _xy_series_name(plot::Ptr{Cvoid}, sel::Integer)
	buf = Vector{UInt8}(undef, 256)
	n = ccall(_fn(:gmtvtk_xyplot_series_name), Cint,
	          (Ptr{Cvoid}, Cint, Ptr{UInt8}, Cint), plot, Cint(sel), buf, Cint(length(buf)))
	n <= 0 && return ""
	return String(@view buf[1:n])
end

# File > Save: write the selected series (or all, if sel < 0) of the CURRENT page to `path`. The
# format follows the extension (GMT.gmtwrite: .dat/.txt plain, .csv, OGR vector formats, …). Data is
# pulled from the C side (the page actually shown), so Save targets the right page once tabs exist.
function _xy_save(p::QtXYPlot, sel::Int, path)
	cnt = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), p.h)
	cnt <= 0 && return
	idxs = (0 <= sel < cnt) ? (sel:sel) : (0:cnt-1)
	mats = GMTdataset[]
	for i in idxs
		xy = _xy_get_series(p.h, i)
		xy === nothing && continue
		push!(mats, GMT.mat2ds(hcat(xy[1], xy[2])))
	end
	isempty(mats) && return
	GMT.gmtwrite(String(path), length(mats) == 1 ? mats[1] : mats)
	return
end

# Build the C-callable pointer and install it in the DLL. Called once from __init__.
function _register_xy_callback()
	fptr = @cfunction(_on_xy, Cvoid, (Ptr{Cvoid}, Cstring, Cint, Cstring))
	ccall(_fn(:gmtvtk_xyplot_set_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
