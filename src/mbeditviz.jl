# mbeditviz.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — the geographic bathymetry editor and
# patch test tool: MB-System's mbeditviz, ported into the viewer (deps/src/mbeditviz/: the engine mbeditviz.c, its file
# list window and the 3-D sounding editor). Everything happens on the C side; this file only opens it, as
# `mbeditviz -I file -F format` would, and drives / reads it back for tests.

export mbeditviz

"""
	mbeditviz(file=""; format=0, browse=false) -> Bool

Open the bathymetry editor and patch test tool, a port of MB-System's `mbeditviz`. Swath files
are listed, loaded and gridded together; the grid and every file's navigation are shown on a
survey map (an InteractiveGMT window); the soundings of a shape drawn on that map (a Region, an
Area along a line, or the pings of the Navigation inside it) open in a 3-D editor where they are
flagged and unflagged (Toggle, Pick, Erase, Restore, Grab, Info, and mbedit's key macros), and
where roll, pitch, heading, time lag and sound speed corrections are tried (patch test) and
optimized. Edits go to each file's edit save file (`file.esf`) and its mbprocess parameter file,
as mbeditviz does.

- `file`: a swath file, or a datalist with `format=-1`. Each file needs its `.inf` (mbinfo) file.
- `format`: the MB-System format id; `0` guesses it from the file name.
- `browse`: look only (mbeditviz's Browse Only mode): nothing is written.

mbeditviz needs MB-System 5.8.x's MBIO library, found as the swath editor finds it
(`ENV["INTERACTIVEGMT_MBIO"]`, iGMT.ini, the PATH, or asked). It is also on the Tools menu, as
"Bathymetry editor and patch test (mbeditviz)".

```julia
mbeditviz("survey.mb-1"; format=-1)
```
"""
function mbeditviz(file::String=""; format::Int=0, browse::Bool=false)
	isdefined(@__MODULE__, :_push_mbio_hint) && _push_mbio_hint()   # the MBIO GMT loads (mbedit.jl)
	ok = ccall(_fn(:gmtvtk_mbeditviz_open), Cint, (Cstring, Cint, Cint), file, Cint(format), Cint(browse ? 1 : 0))
	_start_pump()
	return ok == 1
end

# The tool's state, for tests: (open, numfiles, numloaded, gridstatus, nx, ny, nselected, nflagged, editor)
# gridstatus: 0 no grid, 1 grid not viewed, 2 grid on the survey map.
function _mbeditviz_state()
	v = zeros(Cint, 9)
	ccall(_fn(:gmtvtk_mbeditviz_state), Cint, (Ptr{Cint}, Cint), v, Cint(9))
	return (open = v[1] == 1, numfiles = Int(v[2]), numloaded = Int(v[3]), gridstatus = Int(v[4]), nx = Int(v[5]),
	        ny = Int(v[6]), nselected = Int(v[7]), nflagged = Int(v[8]), editor = v[9] == 1)
end

_mbeditviz_view_all(cellsize::Real=0.0) = ccall(_fn(:gmtvtk_mbeditviz_view_all), Cint, (Cdouble,), cellsize) == 1
_mbeditviz_select(what::Int, shape::String) = ccall(_fn(:gmtvtk_mbeditviz_select), Cint, (Cint, Cstring), what, shape) == 1
_mbeditviz_select_box(x0::Real, x1::Real, y0::Real, y1::Real) =
	ccall(_fn(:gmtvtk_mbeditviz_select_box), Cint, (Cdouble, Cdouble, Cdouble, Cdouble), x0, x1, y0, y1) == 1
_mbeditviz_editor_key(c::Char) = ccall(_fn(:gmtvtk_mbeditviz_editor_key), Cint, (Cint,), Cint(c)) == 1
_mbeditviz_editor_mode(mode::Int) = ccall(_fn(:gmtvtk_mbeditviz_editor_mode), Cint, (Cint,), mode) == 1
_mbeditviz_editor_click(i::Int) = ccall(_fn(:gmtvtk_mbeditviz_editor_click), Cint, (Cint,), i) == 1
_mbeditviz_editor_save_png(path::String) = ccall(_fn(:gmtvtk_mbeditviz_editor_save_png), Cint, (Cstring,), path) == 1
_mbeditviz_close_editor() = ccall(_fn(:gmtvtk_mbeditviz_close_editor), Cint, ()) == 1
_mbeditviz_close_map() = ccall(_fn(:gmtvtk_mbeditviz_close_map), Cint, ()) == 1
_mbeditviz_close() = ccall(_fn(:gmtvtk_mbeditviz_close), Cint, ()) == 1
