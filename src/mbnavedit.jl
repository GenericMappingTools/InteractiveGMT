# mbnavedit.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — the interactive navigation
# editor: MB-System's mbnavedit, ported into the viewer (deps/src/mbnavedit/: the engine mbnavedit.c and its
# Qt window). Everything happens on the C side; this file only opens it, as
# `mbnavedit -I file -F format [-D] [-X] [-P] [-N]` would, and reads it back for tests.

export mbnavedit

"""
	mbnavedit(files...; format=0, browse=false, run_mbprocess=false, use_ping_data=false,
	          strip_comments=false) -> Bool

Open the navigation editor, a port of MB-System's `mbnavedit`: the navigation of a swath file
shown as time series — time between fixes, longitude, latitude, speed, heading and sonar depth —
where bad fixes are selected with the mouse and repaired by interpolation, by the speed or course
made good, or by a navigation model (Gaussian mean, dead reckoning, smooth inversion).

- `files`: swath files or datalists (`-I`). Every file joins the editor's file list and the first
  one is loaded; **Done** moves on to the next.
- `format`: their MB-System format id (`-F`), one for all or a vector, one per file; `0` guesses it
  from the file name, `-1` reads a datalist.
- `browse`: look only, nothing is written (`-D`). Otherwise the edited navigation goes to
  `<file>.nve` and is set in the file's mbprocess parameter file.
- `run_mbprocess`: run mbprocess on a file when it is finished (`-X`); `strip_comments` passes it
  `-N` (`-N`).
- `use_ping_data`: take the navigation from the survey pings instead of the navigation records (`-P`).

It is also on Geophysics > MB-System, as "MBnavedit", and on each MBgrdviz ship track's right-click
menu. The files are read through MB-System's MBIO library, the same one the swath editor (`mbedit`)
uses and found the same way (`ENV["INTERACTIVEGMT_MBIO"]`, iGMT.ini, the PATH, or asked).

Returns `true` when the editor is open with the first file loaded (or open and empty, with no file).

```julia
mbnavedit("TN136HS.309.snipped.mb57")
mbnavedit("survey.mb-1"; format=-1, run_mbprocess=true)
```
"""
function mbnavedit(files::String...; format::Union{Int,Vector{Int}}=0, browse::Bool=false, run_mbprocess::Bool=false,
                   use_ping_data::Bool=false, strip_comments::Bool=false, use_previous::Int=-1)
	isdefined(@__MODULE__, :_push_mbio_hint) && _push_mbio_hint()   # the MBIO GMT loads (mbedit.jl)
	forms = Cint[format isa Int ? format : format[min(i, length(format))] for i in eachindex(files)]
	ok = ccall(_fn(:gmtvtk_mbnavedit_open), Cint, (Cstring, Ptr{Cint}, Cint, Cint, Cint, Cint, Cint, Cint),
	           join(files, '\n'), forms, Cint(length(forms)), Cint(browse), Cint(run_mbprocess), Cint(use_ping_data),
	           Cint(strip_comments), Cint(use_previous))
	_start_pump()
	return ok == 1
end

# The editor's state, for tests
function _mbnavedit_state()
	v = zeros(Cint, 14)
	ccall(_fn(:gmtvtk_mbnavedit_state), Cint, (Ptr{Cint}, Cint), v, Cint(14))
	return (open = v[1] == 1, file_open = v[2] == 1, numfiles = Int(v[3]), currentfile = Int(v[4]), nbuff = Int(v[5]),
	        current_id = Int(v[6]), nplot = Int(v[7]), nload_total = Int(v[8]), ndump_total = Int(v[9]),
	        number_plots = Int(v[10]), model_mode = Int(v[11]), mode_pick = Int(v[12]), width = Int(v[13]),
	        height = Int(v[14]))
end

# record i (0-based) of the buffer: (time, lon, lat, speed, heading, draft, selected bits), or nothing
function _mbnavedit_record(i::Int)
	v = zeros(Cdouble, 6)
	sel = Ref{Cint}(0)
	ok = ccall(_fn(:gmtvtk_mbnavedit_record), Cint, (Cint, Ptr{Cdouble}, Ref{Cint}), Cint(i), v, sel)
	return ok == 1 ? (time = v[1], lon = v[2], lat = v[3], speed = v[4], heading = v[5], draft = v[6],
	                  selected = Int(sel[])) : nothing
end

# plot iplot's (0-based) box on the canvas: (ixmin, ixmax, iymin, iymax, type), or nothing
function _mbnavedit_plot_box(iplot::Int)
	v = zeros(Cint, 5)
	ok = ccall(_fn(:gmtvtk_mbnavedit_plot_box), Cint, (Cint, Ptr{Cint}), Cint(iplot), v)
	return ok == 1 ? (ixmin = Int(v[1]), ixmax = Int(v[2]), iymin = Int(v[3]), iymax = Int(v[4]), type = Int(v[5])) : nothing
end

# the canvas pixel record i is drawn at in plot iplot, or nothing
function _mbnavedit_record_xy(iplot::Int, i::Int)
	v = zeros(Cint, 2)
	ok = ccall(_fn(:gmtvtk_mbnavedit_record_xy), Cint, (Cint, Cint, Ptr{Cint}), Cint(iplot), Cint(i), v)
	return ok == 1 ? (Int(v[1]), Int(v[2])) : nothing
end

_mbnavedit_mouse(button::Int, x0::Int, y0::Int, x1::Int=x0, y1::Int=y0) =
	ccall(_fn(:gmtvtk_mbnavedit_mouse), Cint, (Cint, Cint, Cint, Cint, Cint), button, x0, y0, x1, y1) == 1
_mbnavedit_key(ch::Char) = ccall(_fn(:gmtvtk_mbnavedit_key), Cint, (Cint,), Cint(ch)) == 1
_mbnavedit_press(name::String) = ccall(_fn(:gmtvtk_mbnavedit_press), Cint, (Cstring,), name) == 1
_mbnavedit_save_png(path::String) = ccall(_fn(:gmtvtk_mbnavedit_save_png), Cint, (Cstring,), path) == 1
_mbnavedit_close() = ccall(_fn(:gmtvtk_mbnavedit_close), Cint, ()) == 1
