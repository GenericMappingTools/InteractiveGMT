# pointcloudeditor.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — MB-System's
# pointCloudEditor, ported (deps/src/pointcloudeditor/). Everything happens on the C side; this file only opens it,
# as `pointCloudEditor [-elev] file` would, and drives / reads it back for tests.

export pointcloudeditor

"""
	pointcloudeditor(file=""; fig=nothing, elev=false) -> Bool

Open the point cloud editor, a port of MB-System's `pointCloudEditor`: a grid (or a swath file,
gridded as mbeditviz grids it) shown as a surface whose points are coloured by their quality,
green good and red bad. Press `r` for the rubber band and drag over the points to ERASE (mark bad)
or RESTORE (mark good) them, as the ERASE / RESTORE buttons say; the slider sets the vertical
exaggeration. With `elev=true` (the original's `-elev`) the rubber band draws an elevation profile
across the data, plotted below. Geographic grids are shown in UTM. Like the original, it writes
nothing.

- `file`: a grid (opened in a window of its own) or a swath file (`.mb*`, with its `.inf`).
- `fig`: edit the grid of this window (a figure returned by `view_grid` and friends).

It reads swath files and projects with MB-System's MBIO library, found as the swath editor finds
it (`ENV["INTERACTIVEGMT_MBIO"]`, iGMT.ini, the PATH, or asked); swath files need MB-System 5.8.x.
It is also on the Tools menu of every window, as "Point cloud editor (pointCloudEditor)".

```julia
fig = view_grid(G);  pointcloudeditor(; fig)
```
"""
function pointcloudeditor(file::String=""; fig=nothing, elev::Bool=false)
	h = fig === nothing ? C_NULL : fig.h
	isdefined(@__MODULE__, :_push_mbio_hint) && _push_mbio_hint()   # the MBIO GMT loads (mbedit.jl)
	ok = ccall(_fn(:gmtvtk_pce_open), Cint, (Ptr{Cvoid}, Cstring, Cint), h, file, Cint(elev ? 1 : 0))
	_start_pump()
	return ok == 1
end

# The editor's state, for tests: (open, npoints, ncells, nbad, editmode, selecting, elevprofile, nprofile)
function _pce_state()
	v = zeros(Cint, 8)
	ccall(_fn(:gmtvtk_pce_state), Cint, (Ptr{Cint}, Cint), v, Cint(8))
	return (open = v[1] == 1, npoints = Int(v[2]), ncells = Int(v[3]), nbad = Int(v[4]), editmode = Int(v[5]),
	        selecting = v[6] == 1, elevprofile = v[7] == 1, nprofile = Int(v[8]))
end

_pce_set_edit_mode(mode::Int) = ccall(_fn(:gmtvtk_pce_set_edit_mode), Cint, (Cint,), mode) == 1
_pce_set_vertical_exagg(v::Real) = ccall(_fn(:gmtvtk_pce_set_vertical_exagg), Cint, (Cdouble,), v) == 1
_pce_rubber_band(x0::Int, y0::Int, x1::Int, y1::Int) =
	ccall(_fn(:gmtvtk_pce_rubber_band), Cint, (Cint, Cint, Cint, Cint), x0, y0, x1, y1) == 1
_pce_set_elev_profile(on::Bool) = ccall(_fn(:gmtvtk_pce_set_elev_profile), Cint, (Cint,), on ? 1 : 0) == 1
function _pce_canvas_size()
	v = zeros(Cint, 2)
	ccall(_fn(:gmtvtk_pce_canvas_size), Cint, (Ptr{Cint},), v)
	return (Int(v[1]), Int(v[2]))
end
_pce_save_png(path::String) = ccall(_fn(:gmtvtk_pce_save_png), Cint, (Cstring,), path) == 1
_pce_close() = ccall(_fn(:gmtvtk_pce_close), Cint, ()) == 1
