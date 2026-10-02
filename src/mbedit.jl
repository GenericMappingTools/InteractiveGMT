# mbedit.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — the swath bathymetry editor: MB-System's mbedit, ported into the viewer
# (deps/src/mbedit/: the engine mbedit.c and its Qt window). Everything happens on the C side;
# this file only opens it, as `mbedit -I file -F format [-S]` would, and reads it back for tests.

export mbedit

"""
	mbedit(file=""; format=0, esf=:ask) -> Bool

Open the swath bathymetry editor, a port of MB-System's `mbedit`: the pings of a swath sonar
file drawn as a waterfall of across-track profiles, whose soundings are flagged and unflagged
with the mouse (Toggle, Pick, Erase, Restore, Grab, Info modes) and the same key macros, filters
and views as mbedit. Edits go to the file's edit save file (`file.esf`) and are recorded for
`mbprocess`, exactly as mbedit does.

- `file`: a swath file, or a datalist with `format=-1`. Empty opens the editor with File > Open.
- `format`: the MB-System format id; `0` guesses it from the file name.
- `esf`: what to do with an edit save file left by an earlier session: `:ask` (mbedit's dialog),
  `true` (apply it, mbedit `-S`) or `false` (start from the file's own flags).

The editor reads swath data through MB-System's MBIO library, loaded at run time: the one GMT loads
with the MB-System supplement it is configured with (`GMT_CUSTOM_LIBS`). To pick another, set
`ENV["INTERACTIVEGMT_MBIO"]` to its path, or point to it when the editor asks (the answer is kept
in iGMT.ini). It is also on the Tools menu, as "Swath editor (mbedit)".

Returns `true` when the editor is open, with the file loaded if one was given.

```julia
mbedit("TN136HS.309.snipped.mb11")
mbedit("survey.mb-1"; format=-1)        # a datalist
```
"""
function mbedit(file::String=""; format::Int=0, esf::Union{Symbol,Bool}=:ask)
	(esf isa Symbol && esf !== :ask) && throw(ArgumentError("mbedit: esf must be :ask, true or false"))
	use = esf === :ask ? -1 : (esf ? 1 : 0)
	_push_mbio_hint()                    # the MBIO GMT loads with its MB-System supplement
	ok = ccall(_fn(:gmtvtk_mbedit_open), Cint, (Cstring, Cint, Cint), file, Cint(format), Cint(use))
	_start_pump()
	return ok == 1
end

# The editor's state, for tests: (open, numfiles, currentfile, nbuffer, ngood, icurrent, nplot,
# nflagged, nunflagged) — nflagged/nunflagged count the soundings in the buffer.
function _mbedit_state()
	v = zeros(Cint, 9)
	ccall(_fn(:gmtvtk_mbedit_state), Cint, (Ptr{Cint}, Cint), v, Cint(9))
	return (open = v[1] == 1, numfiles = Int(v[2]), currentfile = Int(v[3]), nbuffer = Int(v[4]),
	        ngood = Int(v[5]), icurrent = Int(v[6]), nplot = Int(v[7]), nflagged = Int(v[8]),
	        nunflagged = Int(v[9]))
end

_mbedit_key(c::Char) = ccall(_fn(:gmtvtk_mbedit_key), Cint, (Cint,), Cint(c)) == 1
_mbedit_click(x::Int, y::Int) = ccall(_fn(:gmtvtk_mbedit_click), Cint, (Cint, Cint), Cint(x), Cint(y)) == 1
_mbedit_save_png(path::String) = ccall(_fn(:gmtvtk_mbedit_save_png), Cint, (Cstring,), path) == 1
_mbedit_close() = ccall(_fn(:gmtvtk_mbedit_close), Cint, ()) == 1

# THE MBIO IS THE ONE GMT LOADS. Only GMT finds MB-System: it loads the MB-System supplement it is
# configured with (GMT_CUSTOM_LIBS, or its own plugins), and that supplement brings its own MBIO into
# the process. So GMT is asked for an MB-System module (GMT_MODULE_EXIST loads the supplement that
# holds it), and the MBIO is then the loaded library exporting MBIO's own `mb_read_init` -- no library
# name of ours anywhere. "" = GMT has no MB-System.
const _MB_GMT_MODULES = ("mbswath", "mbcontour", "mbgrdtiff")   # the MB-System supplement's GMT modules

function _mbio_from_gmt()::String
	api = GMT.G_API[]
	api == C_NULL && return ""
	any(m -> GMT.GMT_Call_Module(api, m, GMT.GMT_MODULE_EXIST, C_NULL) == 0, _MB_GMT_MODULES) || return ""
	for lib in Libdl.dllist()
		h = Libdl.dlopen(lib; throw_error = false)          # already in the process: only a refcount
		h === nothing && continue
		has = Libdl.dlsym(h, :mb_read_init; throw_error = false) !== nothing
		Libdl.dlclose(h)
		has && return lib
	end
	return ""
end

# Hand it to the viewer's ONE MBIO loader (mbedit_window.cpp), once, at the first window.
function _push_mbio_hint()
	haskey(_LIB_FNS, :gmtvtk_mbedit_set_mbio_hint) || return nothing   # a viewer built without the tools
	ccall(_fn(:gmtvtk_mbedit_set_mbio_hint), Cvoid, (Cstring,), _mbio_from_gmt())
	return nothing
end