# mbvelocitytool.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — the water sound velocity profile
# editor: MB-System's mbvelocitytool, ported into the viewer (deps/src/mbvelocitytool/: the engine
# mbvelocity.c and its Qt window). Everything happens on the C side; this file only opens it, as
# `mbvelocitytool -I swath -F format -W editsvp -S displaysvp` would, and reads it back for tests.

export mbvelocitytool

"""
	mbvelocitytool(swath=""; format=0, edit_svp="", display_svp="") -> Bool

Open the sound velocity profile editor, a port of MB-System's `mbvelocitytool`: water sound
velocity profiles (SVPs) drawn as depth against velocity, an editable profile whose nodes are
dragged (left button), added (middle) and deleted (right) with the mouse, and the pings of a swath
file raytraced through it. The tool shows the rays of the first ping and the mean bathymetry
residual of every beam: a profile that is right leaves the residuals flat.

- `swath`: a swath sonar file (`-I`). Its pings are raytraced through the editable profile; if none
  is loaded yet, the default one is made for it. The Levitus annual mean profile at the file's first
  position, and any `<swath>_NNN.svp` written by mbsvplist, are added as display profiles.
- `format`: its MB-System format id (`-F`); `0` guesses it from the file name.
- `edit_svp`: a profile to edit (`-W`), as lines of `depth velocity` (`#` lines are comments).
- `display_svp`: a profile to show for comparison (`-S`).

The File menu saves the edited profile, `<swath>.svp` and the beam residuals as static offsets
(`<swath>.sbo`, `.sbao`), and sets them in the swath file's mbprocess parameter file, exactly as
mbvelocitytool does. It is also on the Tools menu, as "Sound velocity editor (mbvelocitytool)".

The swath file is read through MB-System's MBIO library, the same one the swath editor (`mbedit`)
uses and found the same way (`ENV["INTERACTIVEGMT_MBIO"]`, iGMT.ini, the PATH, or asked).

Returns `true` when the tool is open with every given file loaded.

```julia
mbvelocitytool("TN136HS.309.snipped.mb11")
mbvelocitytool("survey.mb58"; edit_svp="ctd_cast.svp")
```
"""
function mbvelocitytool(swath::String=""; format::Int=0, edit_svp::String="", display_svp::String="")
	isdefined(@__MODULE__, :_push_mbio_hint) && _push_mbio_hint()   # the MBIO GMT loads (mbedit.jl)
	ok = ccall(_fn(:gmtvtk_mbvelocity_open), Cint, (Cstring, Cint, Cstring, Cstring), swath, Cint(format), edit_svp, display_svp)
	_start_pump()
	return ok == 1
end

# The tool's state, for tests: (open, edit, nedit, ndisplay, nbuffer, nbeams, width, height) — nbeams
# counts the beams that have a residual; width x height is the canvas, in pixels.
function _mbvelocity_state()
	v = zeros(Cint, 8)
	ccall(_fn(:gmtvtk_mbvelocity_state), Cint, (Ptr{Cint}, Cint), v, Cint(8))
	return (open = v[1] == 1, edit = v[2] == 1, nedit = Int(v[3]), ndisplay = Int(v[4]), nbuffer = Int(v[5]),
	        nbeams = Int(v[6]), width = Int(v[7]), height = Int(v[8]))
end

# The profile plot's box on the canvas, as mbvt_plot lays it out: (xmin, xmax, ymin, ymax)
function _mbvelocity_profile_box()
	s = _mbvelocity_state()
	w, h = s.width - 1, s.height - 1                  # borders[1] - borders[0], borders[3] - borders[2]
	margin = h ÷ 15
	return (trunc(Int, 2.25 * margin), trunc(Int, 0.5 * w - margin), margin, trunc(Int, 0.5 * h))
end

# (depth, velocity) of node i (0-based) of the editable profile, or nothing
function _mbvelocity_edit_node(i::Int)
	v = zeros(Cdouble, 2)
	ok = ccall(_fn(:gmtvtk_mbvelocity_edit_node), Cint, (Cint, Ptr{Cdouble}), Cint(i), v)
	return ok == 1 ? (v[1], v[2]) : nothing
end

_mbvelocity_mouse(button::Int, x0::Int, y0::Int, x1::Int=x0, y1::Int=y0) =
	ccall(_fn(:gmtvtk_mbvelocity_mouse), Cint, (Cint, Cint, Cint, Cint, Cint), button, x0, y0, x1, y1) == 1
_mbvelocity_reprocess() = ccall(_fn(:gmtvtk_mbvelocity_reprocess), Cint, ()) == 1
_mbvelocity_save_swath_svp() = ccall(_fn(:gmtvtk_mbvelocity_save_swath_svp), Cint, ()) == 1
_mbvelocity_save_residuals() = ccall(_fn(:gmtvtk_mbvelocity_save_residuals), Cint, ()) == 1
_mbvelocity_save_png(path::String) = ccall(_fn(:gmtvtk_mbvelocity_save_png), Cint, (Cstring,), path) == 1
_mbvelocity_close() = ccall(_fn(:gmtvtk_mbvelocity_close), Cint, ()) == 1
