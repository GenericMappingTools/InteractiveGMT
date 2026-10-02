# mbgrdviz.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — the survey planning and display
# tool: MB-System's mbgrdviz, ported onto InteractiveGMT windows (deps/src/mbgrdviz/). Everything happens on the C
# side; this file only opens it, as `mbgrdviz -I grid` would, and drives / reads it back for tests.

export mbgrdviz

"""
	mbgrdviz(file=""; fig=nothing) -> Bool

Open the survey planning and display tool, a port of MB-System's `mbgrdviz`, on InteractiveGMT
windows. A grid shown in an InteractiveGMT window is its view: site, route, vector, navigation
and swath files open into that window as its own elements (each route a polyline, each site a
symbol, each navigation line an overlay), with their Scene Objects rows; routes and sites drawn
in the window with its Polyline and Symbols tools are mbgrdviz routes and sites. Sites and routes
are saved in mbgrdviz's formats (route, reversed route, Risi scripts, degrees and decimal minutes,
Hypack LNW, Greensea YML, TECDIS LST, Kongsberg DP, SIS ASCIIPlan), a profile along a route is
saved, survey lines are generated over an area (a two-point line drawn in the window plus a width),
a region (a drawn rectangle) is opened as a new window, and the checked navigation is opened in the
ported swath editors (mbedit, mbeditviz, mbvelocitytool).

- `file`: a grid to open, as `mbgrdviz -I` does: into `fig`'s window when it is empty, else a new window.
- `fig`: the window to bind (a figure returned by `view_grid` and friends).

It reads swath files with MB-System's MBIO library, found as the swath editor finds it
(`ENV["INTERACTIVEGMT_MBIO"]`, iGMT.ini, the PATH, or asked). It is also on the Tools menu of every
window, as "Survey planning (mbgrdviz)".

```julia
fig = view_grid(G);  mbgrdviz(; fig)
```
"""
function mbgrdviz(file::String=""; fig=nothing)
	h = fig === nothing ? C_NULL : fig.h
	isdefined(@__MODULE__, :_push_mbio_hint) && _push_mbio_hint()   # the MBIO GMT loads (mbedit.jl)
	ok = ccall(_fn(:gmtvtk_mbgrdviz_open), Cint, (Ptr{Cvoid}, Cstring), h, file)
	_start_pump()
	return ok == 1
end

# what the engine opens / writes (deps/src/mbgrdviz/mbgrdviz.h)
const _MBGRDVIZ_OPEN = Dict(:site => 2, :route => 3, :vector => 4, :nav => 5, :swath => 6)
const _MBGRDVIZ_SAVE = Dict(:route => 7, :route_reversed => 8, :risi_heading => 9, :risi_noheading => 10,
                            :risi2_heading => 11, :risi2_noheading => 12, :degdecmin => 13, :lnw => 14, :greensea_yml => 15,
                            :tecdis_lst => 16, :kongsberg_dp => 17, :sis_asciiplan1 => 18, :sis_asciiplan2 => 19,
                            :site => 20, :profile => 21)

# The tool's state, for tests: (open, nviews, ready, nroute, nsite, nnav, nvector, working_route)
function _mbgrdviz_state()
	v = zeros(Cint, 8)
	ccall(_fn(:gmtvtk_mbgrdviz_state), Cint, (Ptr{Cint}, Cint), v, Cint(8))
	return (open = v[1] == 1, nviews = Int(v[2]), ready = v[3] == 1, nroute = Int(v[4]), nsite = Int(v[5]),
	        nnav = Int(v[6]), nvector = Int(v[7]), working_route = Int(v[8]))
end

_mbgrdviz_open(what::Symbol, path::String) =
	ccall(_fn(:gmtvtk_mbgrdviz_open_file), Cint, (Cint, Cstring), _MBGRDVIZ_OPEN[what], path) == 1
_mbgrdviz_save(what::Symbol, path::String) =
	ccall(_fn(:gmtvtk_mbgrdviz_save), Cint, (Cint, Cstring), _MBGRDVIZ_SAVE[what], path) == 1
_mbgrdviz_select_route(name::String="") = ccall(_fn(:gmtvtk_mbgrdviz_select_route), Cint, (Cstring,), name) == 1
_mbgrdviz_set_area(line::String, width::Real=0.0) =
	ccall(_fn(:gmtvtk_mbgrdviz_set_area), Cint, (Cstring, Cdouble), line, width) == 1
_mbgrdviz_set_region(rect::String) = ccall(_fn(:gmtvtk_mbgrdviz_set_region), Cint, (Cstring,), rect) == 1

# the survey dialog's values, then Generate Route; the route's name ("" when none was made)
function _mbgrdviz_generate_survey(; mode::Int=0, platform::Int=1, direction::Int=0, crosslines::Int=0,
                                   crosslines_last::Bool=false, linespacing::Int=200, swathwidth::Int=120,
                                   altitude::Int=150, depth::Int=1, interleaving::Int=1, color::Int=0,
                                   name::String="Survey")
	p = Cint[mode, platform, direction, crosslines, crosslines_last ? 1 : 0, linespacing, swathwidth, altitude, depth,
	         interleaving, color]
	buf = zeros(UInt8, 1024)
	ok = ccall(_fn(:gmtvtk_mbgrdviz_generate_survey), Cint, (Ptr{Cint}, Cstring, Ptr{UInt8}, Cint), p, name, buf,
	           Cint(length(buf)))
	return ok == 1 ? unsafe_string(pointer(buf)) : ""
end

_mbgrdviz_survey_dismiss() = ccall(_fn(:gmtvtk_mbgrdviz_survey_dismiss), Cint, ()) == 1
_mbgrdviz_open_region() = ccall(_fn(:gmtvtk_mbgrdviz_open_region), Cint, ()) == 1
_mbgrdviz_select_nav(nav::Int, selected::Bool=true) =
	ccall(_fn(:gmtvtk_mbgrdviz_select_nav), Cint, (Cint, Cint), nav, selected ? 1 : 0) == 1
_mbgrdviz_close() = ccall(_fn(:gmtvtk_mbgrdviz_close), Cint, ()) == 1
