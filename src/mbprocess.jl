# mbprocess.jl — Geophysics > MB-System > "Process swath data (mbprocess)" (deps/src/72_mbprocess.cpp).
#
# The dialog's host side. Everything is MB-System's own: mbset writes a file's parameter file
# (file.par), mbprocess reads it and writes the processed file, both run IN THIS PROCESS through GMT's
# MB-System supplement, and a datalist is expanded by MBIO's own datalist reader. Nothing here
# re-implements any of it; this file only carries requests across and brings the modules' report back.
#
# REQUESTS: a newline-separated "key=value" block, `what` naming the request.
#   what=files   input, format      -> one "path\tformat" line per swath file (a datalist expanded)
#   what=par     file               -> exists=0|1, editmode=0|1, editfile=...  (what the .par says;
#                                      with no .par, what mbprocess's own look-around would find)
#   what=setpar  file, format, ifmissing[, editmode[, editfile]], set.KEY=value ...  -> mbset's report
#   what=process file, format, force, strip, verbose             -> mbprocess's report
#   what=again                      -> the last reply again, whole (the reply did not fit the buffer)
# The callback returns the reply's FULL byte length, negative for a failure; it writes what fits.

# The MBIO GMT loaded with its MB-System supplement (mbedit.jl's _mbio_from_gmt), opened once.
const _MBP_MBIO = Ref{Ptr{Cvoid}}(C_NULL)
function _mbp_mbio()::Ptr{Cvoid}
	_MBP_MBIO[] != C_NULL && return _MBP_MBIO[]
	lib = isdefined(@__MODULE__, :_mbio_from_gmt) ? _mbio_from_gmt() : ""
	isempty(lib) && error("GMT has no MB-System supplement loaded, so there is no MBIO to read swath data with.")
	_MBP_MBIO[] = Libdl.dlopen(lib)
	return _MBP_MBIO[]
end
_mbp_sym(name::Symbol)::Ptr{Cvoid} = Libdl.dlsym(_mbp_mbio(), name)

# MBIO builds a datalist entry's path off the datalist's own by looking for '/', so a Windows path is
# handed over with forward slashes (which every Windows file API takes as they are).
_mbp_slash(p::String)::String = replace(p, '\\' => '/')

const _MBP_PATHLEN = 4096         # >= MB_PATH_MAXLINE (1024)

# MBIO's format id from a file name (mb_get_format); 0 when it cannot tell.
function _mbp_guess_format(path::String)::Int
	root = zeros(UInt8, _MBP_PATHLEN)
	fmt, err = Ref{Cint}(0), Ref{Cint}(0)
	ccall(_mbp_sym(:mb_get_format), Cint, (Cint, Cstring, Ptr{UInt8}, Ref{Cint}, Ref{Cint}),
	      0, path, root, fmt, err)
	return Int(fmt[])
end

# The swath files `input` stands for: itself, or every file of a datalist (format < 0), recursively,
# as MBIO reads it (mb_datalist_open / mb_datalist_read, raw files: MB_DATALIST_LOOK_NO).
function _mbp_files(input::String, format::Int)::Vector{Tuple{String,Int}}
	isfile(input) || error("No such file: $input")
	path = _mbp_slash(input)
	fmt = format != 0 ? format : _mbp_guess_format(path)
	fmt == 0 && error("Cannot tell the MBIO format of $(basename(input)) from its name: give it in Format.")
	fmt > 0 && return [(path, fmt)]
	dl, err = Ref{Ptr{Cvoid}}(C_NULL), Ref{Cint}(0)
	ccall(_mbp_sym(:mb_datalist_open), Cint, (Cint, Ref{Ptr{Cvoid}}, Cstring, Cint, Ref{Cint}),
	      0, dl, path, 1, err) == 1 || error("MBIO could not open the datalist $input (error $(err[])).")
	out = Tuple{String,Int}[]
	try
		file, dpath = zeros(UInt8, _MBP_PATHLEN), zeros(UInt8, _MBP_PATHLEN)
		f, w = Ref{Cint}(0), Ref{Cdouble}(0.0)
		while ccall(_mbp_sym(:mb_datalist_read), Cint,
		            (Cint, Ptr{Cvoid}, Ptr{UInt8}, Ptr{UInt8}, Ref{Cint}, Ref{Cdouble}, Ref{Cint}),
		            0, dl[], file, dpath, f, w, err) == 1
			push!(out, (unsafe_string(pointer(file)), Int(f[])))
		end
	finally
		ccall(_mbp_sym(:mb_datalist_close), Cint, (Cint, Ref{Ptr{Cvoid}}, Ref{Cint}), 0, dl, err)
	end
	return out
end

# A parameter file as written by mb_pr_writepar: "KEYWORD value" lines, '#' comments.
function _mbp_read_par(par::String)::Dict{String,String}
	d = Dict{String,String}()
	for line in eachline(par)
		t = strip(line)
		(isempty(t) || startswith(t, '#')) && continue
		k, v = occursin(' ', t) ? (split(t, ' '; limit = 2)...,) : (t, "")
		d[String(k)] = String(strip(v))
	end
	return d
end

# What the selected file's .par says about the bathymetry edits. With no .par, what mbprocess's
# look-around (mbset -L, which the dialog runs before the first processing) would find: file.esf.
#
# Every key of the .par also goes out as "par.KEY=value": the dialog's other tabs are filled from
# them by key (the "parKey" of each widget in mbprocess.ui).
function _mbp_par_info(file::String)::String
	par = file * ".par"
	if isfile(par)
		d = _mbp_read_par(par)
		ef = get(d, "EDITSAVEFILE", "")
		isempty(ef) && (ef = file * ".esf")
		keys_ = join(("par.$k=$v\n" for (k, v) in d if !occursin('\n', v)))
		return "exists=1\neditmode=$(get(d, "EDITSAVEMODE", "0") == "1" ? 1 : 0)\neditfile=$ef\n" * keys_
	end
	esf = file * ".esf"
	return "exists=0\neditmode=$(isfile(esf) ? 1 : 0)\neditfile=$esf\n"
end

# One GMT option token, quoted when it holds a space (GMT's own command-string tokenizer keeps a
# quoted token whole).
_mbp_opt(flag::String, val::String)::String = occursin(' ', val) ? "-$flag\"$val\"" : "-$flag$val"

# Run MB-System module `mod` with `args` through GMT's own entry point (GMT.jl's gmt() appends output
# options the MB modules reject — see mbplugin.jl), capturing what it writes to stdout and stderr:
# mbset and mbprocess report with GMT_Report and plain fprintf alike. Returns (status, report).
function _mbp_module(mod::String, args::Vector{String})::Tuple{Int,String}
	api = GMT.G_API[]
	api == C_NULL && error("GMT has no session.")
	GMT.GMT_Call_Module(api, mod, GMT.GMT_MODULE_EXIST, C_NULL) == 0 ||
		error("GMT has no $mod module: MB-System's GMT supplement is not loaded (Geophysics > MB-System > Install as plugin).")
	cmd = join(args, ' ')
	out, onote = _console_capture_file(1)      # the console's own capture (console.jl), as it is
	err, enote = _console_capture_file(2)
	st, etxt, otxt = Cint(-1), "", ""
	try
		st = GC.@preserve cmd GMT.GMT_Call_Module(api, mod, GMT.GMT_MODULE_CMD, pointer(cmd))
	finally
		etxt = err !== nothing ? _console_capture_file_end(err) : enote
		otxt = out !== nothing ? _console_capture_file_end(out) : onote
	end
	return Int(st), String(strip(etxt * otxt, '\n'))
end

_mbp_int(d::Dict{String,String}, k::String, def::Int = 0)::Int =
	(v = strip(get(d, k, "")); isempty(v) ? def : parse(Int, v))

# mbset on one file. `ifmissing`: only make a .par that does not exist yet (the dialog's parameters
# were not changed), so an up-to-date file is not made out of date by a rewritten .par.
function _mbp_setpar(d::Dict{String,String})::Tuple{Bool,String}
	file = get(d, "file", "")
	par = file * ".par"
	exists = isfile(par)
	(_mbp_int(d, "ifmissing") == 1 && exists) && return true, ""
	fmt = _mbp_int(d, "format")
	# mbset writes a .par only when it is given at least one -P; FORMAT (the value it already has)
	# is that one, so a missing .par is made even when nothing was changed.
	args = [_mbp_opt("I", file), "-F$fmt", "-PFORMAT:$fmt"]
	exists || push!(args, "-L")                          # first .par: mbprocess's own look-around
	if haskey(d, "editmode")                             # the Edits tab, when it was changed
		mode = _mbp_int(d, "editmode")
		push!(args, "-PEDITSAVEMODE:$mode")
		if mode == 1
			ef = strip(get(d, "editfile", ""))
			push!(args, "-PEDITSAVEFILE:" * _mbp_slash(isempty(ef) ? file * ".esf" : String(ef)))
		end
	end
	# The other tabs: "set.KEY=value", one per key the user changed, as mbset's own -PKEY:value.
	for (k, v) in sort!(collect(d))
		startswith(k, "set.") || continue
		key = k[5:end]
		occursin(r"^[A-Z0-9]+$", key) || error("mbprocess: \"$key\" is not an mbset parameter name")
		val = String(strip(v))
		endswith(key, "FILE") && (val = _mbp_slash(val))
		push!(args, _mbp_opt("P", "$key:$val"))
	end
	st, txt = _mbp_module("mbset", args)
	st == 0 || return false, (isempty(txt) ? "mbset failed (GMT status $st)." : txt * "\nmbset failed (GMT status $st).")
	return true, txt
end

function _mbp_process(d::Dict{String,String})::Tuple{Bool,String}
	file = get(d, "file", "")
	args = [_mbp_opt("I", file), "-F$(_mbp_int(d, "format"))"]
	_mbp_int(d, "force") == 1   && push!(args, "-P")
	_mbp_int(d, "strip") == 1   && push!(args, "-N")
	_mbp_int(d, "verbose") == 1 && push!(args, "-V")
	st, txt = _mbp_module("mbprocess", args)
	st == 0 || return false, (isempty(txt) ? "mbprocess failed (GMT status $st)." : txt * "\nmbprocess failed (GMT status $st).")
	return true, (isempty(txt) ? "mbprocess: nothing reported." : txt)
end

_mbp_kv(s::String)::Dict{String,String} =
	Dict{String,String}(String(strip(k)) => String(v) for (k, v) in
	                    (split(l, '='; limit = 2) for l in split(s, '\n') if occursin('=', l)))

# One request -> (ok, reply). Throws on a broken request; the callback turns that into a failure.
function _mbp_request(d::Dict{String,String})::Tuple{Bool,String}
	what = get(d, "what", "")
	if what == "files"
		fs = _mbp_files(String(strip(get(d, "input", ""))), _mbp_int(d, "format"))
		return true, join(("$p\t$f" for (p, f) in fs), '\n')
	end
	what == "par"     && return true, _mbp_par_info(get(d, "file", ""))
	what == "setpar"  && return _mbp_setpar(d)
	what == "process" && return _mbp_process(d)
	error("mbprocess: unknown request \"$what\"")
end

const _MBP_LAST = Ref((true, ""))    # the last reply, for what=again

function _mbp_reply(out::Ptr{UInt8}, cap::Cint, ok::Bool, s::String)::Cint
	_MBP_LAST[] = (ok, s)
	n = ncodeunits(s)
	if cap > 0
		k = min(n, Int(cap) - 1)
		k > 0 && GC.@preserve s unsafe_copyto!(out, pointer(s), k)
		unsafe_store!(out, 0x00, k + 1)
	end
	return Cint(ok ? n : -n)
end

# THE C ENTRY POINT NEVER THROWS (an exception through the Qt pump stops the window's event loop).
function _on_mbprocess(scene::Ptr{Cvoid}, params::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		d = _mbp_kv(unsafe_string(params))
		get(d, "what", "") == "again" && return _mbp_reply(out, cap, _MBP_LAST[]...)
		ok, s = _mbp_request(d)
		# A failure always says something: a zero length could not be told from a success.
		(!ok && isempty(s)) && (s = "mbprocess: failed.")
		return _mbp_reply(out, cap, ok, s)
	catch e
		return _mbp_reply(out, cap, false, sprint(showerror, e))
	end
end

# NOTHING OF THIS TOOL RUNS AT START-UP: the menu entry fires warmupTool("mbprocess") and waits for the
# callback; mbprocess_dialog() wires it itself. The warm_register entry is a Dict insertion only.
const _MBP_WIRED = Ref(false)
function _register_mbprocess()
	_MBP_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_mbprocess_callback) || return   # a DLL built before this tool
	f = @cfunction((s, p, o, n) -> Base.invokelatest(_on_mbprocess, s, p, o, n)::Cint,
	               Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_mbprocess_callback), Cvoid, (Ptr{Cvoid},), f)
	_MBP_WIRED[] = true
	return
end
warm_register("mbprocess", _register_mbprocess)

"""
    mbprocess_dialog(file="") -> Bool

Open Geophysics > MB-System > "Process swath data (mbprocess)": MB-System's `mbset` and `mbprocess`
behind a dialog, on a swath file or a datalist (`file`, optional). The bathymetry edits saved by
mbedit / mbeditviz / CUBE (the edit save file) are switched on or off in each file's parameter
file (`file.par`), and Process runs `mbprocess` on every file, its report shown in the dialog's log.
Needs MB-System's GMT supplement (on Linux and macOS: Geophysics > MB-System > Install as plugin).

With `fig` (a viewer window) the dialog belongs to that window, as when it is opened from its menu:
closing or minimising it parks it in the window's Scene Objects.
"""
function mbprocess_dialog(file::String = ""; fig = nothing)::Bool
	_register_mbprocess()
	ok = fig === nothing ? ccall(_fn(:gmtvtk_mbprocess_open), Cint, (Cstring,), file) :
	                       ccall(_fn(:gmtvtk_mbprocess_open_h), Cint, (Ptr{Cvoid}, Cstring), fig.h, file)
	_start_pump()
	return ok == 1
end
export mbprocess_dialog

# The open dialog, for tests: its log; press Process (1) / Save .par (2); Edits on (3) / off (4); close (5).
function _mbprocess_log()::String
	n = ccall(_fn(:gmtvtk_mbprocess_test), Cint, (Cint, Ptr{UInt8}, Cint), 0, C_NULL, 0)
	n < 0 && return ""
	buf = zeros(UInt8, n + 1)
	ccall(_fn(:gmtvtk_mbprocess_test), Cint, (Cint, Ptr{UInt8}, Cint), 0, buf, n + 1)
	return unsafe_string(pointer(buf))
end
_mbprocess_parked(fig)::Int = Int(ccall(_fn(:gmtvtk_mbprocess_parked_test), Cint, (Ptr{Cvoid},), fig.h))
_mbprocess_set_par(key::String, value::String)::Bool =
	ccall(_fn(:gmtvtk_mbprocess_set_par), Cint, (Cstring, Cstring), key, value) == 1
_mbprocess_do(action::Int)::Int = Int(ccall(_fn(:gmtvtk_mbprocess_test), Cint, (Cint, Ptr{UInt8}, Cint), action, C_NULL, 0))
