# xysession.jl — the X,Y tool's File > Session (Save / Open), ecran.m's FileSaveSession_CB /
# FileOpenSession_CB. One session format for the whole app: the SAME `.igmtz` container the 3-D
# viewer's Save Session writes (session.jl's store-only zip), whose `session.manifest` here says
# `[session] kind=xyplot`. The manifest is written by the C side (gmtvtk_xyplot_session_state: window,
# pages, series styles, axes, magnetic modelling); beside it, one binary file per series holds its x
# then its y (little-endian Float64). Opening it — from the X,Y File menu, the 3-D File > Load Session
# or a dropped file (both reach `_on_load_session`) — rebuilds everything in a fresh X,Y window.

# Undo the C side's one-line escaping (xyEsc: \\ \n \r).
function _xy_unesc(v::AbstractString)
	io = IOBuffer(); it = iterate(v)
	while it !== nothing
		c, st = it
		if c == '\\'
			nx = iterate(v, st)
			if nx !== nothing
				d, st = nx
				write(io, d == 'n' ? '\n' : d == 'r' ? '\r' : d)
				it = iterate(v, st)
				continue
			end
		end
		write(io, c)
		it = iterate(v, st)
	end
	return String(take!(io))
end

# The manifest's sections, in order: [(name, Dict(key => value))]. Keys split on the FIRST '='.
function _xy_manifest_sections(str::AbstractString)
	secs = Tuple{String,Dict{String,String}}[]
	for raw in split(str, '\n')
		line = rstrip(raw, '\r')
		isempty(line) && continue
		if startswith(line, "[") && endswith(line, "]")
			push!(secs, (String(line[2:end-1]), Dict{String,String}()))
			continue
		end
		(isempty(secs) || (eq = findfirst('=', line)) === nothing) && continue
		secs[end][2][String(line[1:prevind(line, eq)])] = String(line[nextind(line, eq):end])
	end
	return secs
end

_xy_nums(v::AbstractString) = [x == "nan" ? NaN : parse(Float64, x) for x in split(v)]

# The window's state text (gmtvtk_xyplot_session_state).
function _xy_session_state(h::Ptr{Cvoid})
	n = ccall(_fn(:gmtvtk_xyplot_session_state), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, 0)
	n < 0 && error("the X,Y window is closed")
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_xyplot_session_state), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, n + 1)
	return String(buf[1:n])
end

_xy_series_file(page, idx) = "data/p$(page)_s$(idx).bin"

"""
    _xy_save_session(h, path) -> path

Write X,Y window `h` to the `.igmtz` session `path`: the C side's manifest plus each series' data.
"""
function _xy_save_session(h::Ptr{Cvoid}, path::AbstractString)
	str = _xy_session_state(h)
	files = Tuple{String,Vector{UInt8}}[("session.manifest", Vector{UInt8}(codeunits(str)))]
	for (sec, d) in _xy_manifest_sections(str)
		sec == "series" || continue
		pg = parse(Int, d["page"]); i = parse(Int, d["idx"])
		n = ccall(_fn(:gmtvtk_xyplot_page_series_npoints), Cint, (Ptr{Cvoid}, Cint, Cint), h, pg, i)
		n <= 0 && continue
		x = Vector{Float64}(undef, n); y = Vector{Float64}(undef, n)
		ccall(_fn(:gmtvtk_xyplot_page_get_series), Cint,
		      (Ptr{Cvoid}, Cint, Cint, Ptr{Float64}, Ptr{Float64}, Cint), h, pg, i, x, y, n)
		push!(files, (_xy_series_file(pg, i), Vector{UInt8}(reinterpret(UInt8, htol.(vcat(x, y))))))
	end
	_zip_write(String(path), files)
	ccall(_fn(:gmtvtk_add_recent), Cvoid, (Cstring, Cint), abspath(String(path)), Cint(3))
	return path
end

"""
    _xy_load_session(path, entries) -> QtXYPlot

Rebuild a saved X,Y window (`entries` = the session zip's members) in a fresh X,Y window: its pages
and series first, then the rest of the state (gmtvtk_xyplot_session_apply).
"""
function _xy_load_session(path::AbstractString, entries::Dict{String,Vector{UInt8}})
	str = String(copy(entries["session.manifest"]))
	secs = _xy_manifest_sections(str)
	win = Dict{String,String}()
	for (sec, d) in secs
		sec == "window" && (win = d; break)
	end
	title = _xy_unesc(get(win, "title", "i'GMT  —  X,Y plot"))
	h = ccall(_fn(:gmtvtk_xyplot_open), Ptr{Cvoid}, (Cstring,), title)
	h == C_NULL && error("could not open an X,Y window")
	p = _register_fig!(QtXYPlot(h))
	_start_pump()
	_XY_CURRENT[] = h
	npage = 0
	for (sec, d) in secs
		if sec == "page"
			npage += 1
			npage > 1 && ccall(_fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), h, _xy_unesc(get(d, "name", "")))
		elseif sec == "series"
			bytes = get(entries, _xy_series_file(d["page"], d["idx"]), UInt8[])
			v = ltoh.(collect(reinterpret(Float64, bytes)))
			n = length(v) ÷ 2
			x = v[1:n]; y = v[n+1:2n]
			name = _xy_unesc(get(d, "name", ""))
			r, g, b = _xy_nums(get(d, "rgb", "0 0 0"))
			width = parse(Float64, get(d, "width", "1.5"))
			if get(d, "nowcross", "0") == "1"
				cx, cy, csz = _xy_nums(get(d, "cross", "0 0 24"))
				ccall(_fn(:gmtvtk_xyplot_add_now_cross), Cint,
				      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring),
				      h, cx, cy, r, g, b, csz, width, name)
				push!(p.series, (Float64[cx], Float64[cy], name))
				continue
			end
			n < 1 && continue
			if get(d, "kind", "0") == "1"
				ccall(_fn(:gmtvtk_xyplot_add_bars), Cint,
				      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cstring, Float64, Float64, Float64, Float64, Float64),
				      h, x, y, n, name, r, g, b, width, parse(Float64, get(d, "barwidth", "0")))
			else
				ccall(_fn(:gmtvtk_xyplot_add_series), Cint,
				      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cstring,
				       Float64, Float64, Float64, Float64, Cint, Cint, Float64),
				      h, x, y, n, name, r, g, b, width, parse(Cint, get(d, "ltype", "1")),
				      parse(Cint, get(d, "marker", "0")), parse(Float64, get(d, "msize", "7")))
			end
			push!(p.series, (x, y, name))
		end
	end
	ccall(_fn(:gmtvtk_xyplot_session_apply), Cvoid, (Ptr{Cvoid}, Cstring), h, str)
	ccall(_fn(:gmtvtk_add_recent), Cvoid, (Cstring, Cint), abspath(String(path)), Cint(3))
	return p
end

# File > Session > Open of an X,Y window: a session file of either kind goes through the ONE loader
# (`_on_load_session`, with no invoking 3-D window), which hands an X,Y one to `_xy_load_session` and
# opens a 3-D one in a new viewer window.
_xy_open_session(path::AbstractString) = _on_load_session(C_NULL, String(path))
