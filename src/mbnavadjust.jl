# mbnavadjust.jl — EXPERIMENTAL (switched by its include line in InteractiveGMT.jl) — the navigation
# adjustment tool: MB-System's mbnavadjust, ported into the viewer (deps/src/mbnavadjust/: MB-System's own
# GUI-free core, the engine and the Motif callbacks in C, their Qt window). This file opens it, and is the
# host for the two things upstream did outside its own code:
#
#   RUN   upstream runs MB-System programs through system(): "cd <dir> ; mbdatalist ...",
#         "cd <dir> ; mbprocess -C4", "cd <dir> ; ./mbgrid_adj.cmd" (a script of mbgrid lines it wrote)
#         and "chmod +x <script>". Each program is run here IN THIS PROCESS, as the MB-System GMT module of
#         the same name, in the project's folder, through the ONE MB module runner (mbprocess.jl's
#         _mbp_module); chmod has nothing to do on a file the runner reads.
#   GRID  upstream reads its grids with GMT_Read_Data; the port's mb_read_gmt_grd asks for the header and
#         the values here: the viewer's grid reader (_gmtread_trb), handed over north row first.

export mbnavadjust

# --- RUN ------------------------------------------------------------------------------------------
# One script line or command: the program and its arguments
function _mbna_program(line::AbstractString)::Int
	s = strip(line)
	(isempty(s) || startswith(s, '#')) && return 0
	prog, args = let i = findfirst(isspace, s)
		i === nothing ? (String(s), "") : (String(s[1:prevind(s, i)]), String(strip(s[i:end])))
	end
	prog == "chmod" && return 0                     # the script is read here, never executed by a shell
	if startswith(prog, "./") || startswith(prog, ".\\")
		script = prog[3:end]
		isfile(script) || error("No such script: $(joinpath(pwd(), script))")
		# a script upstream wrote: lines continued with a backslash, one program per line
		text = replace(read(script, String), r"\\\r?\n" => " ")
		st = 0
		for l in split(text, '\n')
			st = _mbna_program(l)
			st != 0 && return st
		end
		return st
	end
	# upstream's programs parse their options with getopt, which takes "-I file"; a GMT module wants
	# "-Ifile": a lone option letter takes the token after it as its value
	toks = split(args)
	opts = String[]
	i = 1
	while i <= length(toks)
		t = String(toks[i])
		if length(t) == 2 && t[1] == '-' && isletter(t[2]) && i < length(toks) && !startswith(toks[i + 1], '-')
			t *= toks[i + 1]
			i += 1
		end
		push!(opts, t)
		i += 1
	end
	println("MBnavadjust: ", prog, " ", join(opts, ' '))
	st, txt = _mbp_module(prog, opts)
	isempty(txt) || println(txt)
	return st
end

# upstream's system(command): "cd <dir> ; <program> ...", or a lone program
function _mbna_run(command::String)::Int
	parts = strip.(split(command, ';'))
	dir = ""
	if !isempty(parts) && startswith(parts[1], "cd ")
		dir = String(strip(parts[1][4:end]))
		parts = parts[2:end]
	end
	run1() = (st = 0; for p in parts; st = _mbna_program(p); st != 0 && break; end; st)
	return isempty(dir) ? run1() : cd(run1, dir)
end

# THE C ENTRY POINTS NEVER THROW (an exception through the Qt pump stops the window's event loop).
function _on_mbna_run(cmd::Cstring)::Cint
	try
		return Cint(_mbna_run(unsafe_string(cmd)))
	catch e
		@tool_error "MBnavadjust: $(sprint(showerror, e))"
		return Cint(-1)
	end
end

# --- GRID -----------------------------------------------------------------------------------------
# the grid of the last request: the header is asked for first, the values right after
const _MBNA_GRID = Ref{Any}(nothing)
const _MBNA_GRID_KEY = Ref{Tuple{String,Float64}}(("", 0.0))

function _mbna_grid(path::String)::GMTgrid
	key = (path, mtime(path))
	if _MBNA_GRID_KEY[] != key || _MBNA_GRID[] === nothing
		G = _gmtread_trb(path)
		G isa GMTgrid || error("$path is not a grid.")
		_MBNA_GRID[] = G
		_MBNA_GRID_KEY[] = key
	end
	return _MBNA_GRID[]::GMTgrid
end

function _on_mbna_grid(cpath::Cstring, idims::Ptr{Cint}, dvals::Ptr{Cdouble}, remark::Ptr{UInt8}, data::Ptr{Cfloat},
                       n::Clonglong)::Cint
	try
		path = unsafe_string(cpath)
		G = _mbna_grid(path)
		z, nx, ny, code = _grid_zbuf(G)
		unsafe_store!(idims, Cint(nx), 1)
		unsafe_store!(idims, Cint(ny), 2)
		unsafe_store!(idims, Cint(G.registration), 3)
		for i in 1:6
			unsafe_store!(dvals, Cdouble(G.range[i]), i <= 4 ? i : i + 2)   # wesn, then z_min z_max
		end
		unsafe_store!(dvals, Cdouble(G.inc[1]), 5)
		unsafe_store!(dvals, Cdouble(G.inc[2]), 6)
		rem = codeunits(G.remark)
		k = min(length(rem), 159)
		for i in 1:k
			unsafe_store!(remark, rem[i], i)
		end
		unsafe_store!(remark, 0x00, k + 1)
		if data != C_NULL
			n == nx * ny || error("$path: $(nx)x$(ny) nodes, $n asked for")
			out = unsafe_wrap(Array, data, Int(n))
			if code == 3                                # "TRB": already row-major, north row first
				copyto!(out, vec(z))
			else                                        # any other layout, through the ONE accessor
				zm = _zmat(G)                           # zm[iy, ix], row 1 = south
				for iy in 1:ny, ix in 1:nx
					out[(ny - iy) * nx + ix] = zm[iy, ix]
				end
			end
			_MBNA_GRID[] = nothing                      # the values went over: let the grid go
		end
		return Cint(1)
	catch e
		@tool_error "MBnavadjust: $(sprint(showerror, e))"
		return Cint(0)
	end
end

# NOTHING OF THIS TOOL RUNS AT START-UP: the menu entry fires warmupTool("mbnavadjust") and waits for the
# callbacks; mbnavadjust() wires them itself. The warm_register entry is a Dict insertion only.
const _MBNA_WIRED = Ref(false)
function _register_mbnavadjust()
	_MBNA_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_mbnavadjust_callbacks) || return   # a DLL built without this tool
	run = @cfunction(c -> Base.invokelatest(_on_mbna_run, c)::Cint, Cint, (Cstring,))
	grid = @cfunction((p, i, d, r, z, n) -> Base.invokelatest(_on_mbna_grid, p, i, d, r, z, n)::Cint,
	                  Cint, (Cstring, Ptr{Cint}, Ptr{Cdouble}, Ptr{UInt8}, Ptr{Cfloat}, Clonglong))
	ccall(_fn(:gmtvtk_set_mbnavadjust_callbacks), Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}), run, grid)
	_MBNA_WIRED[] = true
	return
end
warm_register("mbnavadjust", _register_mbnavadjust)

"""
	mbnavadjust(project=""; fig=nothing) -> Bool

Open the navigation adjustment tool, a port of MB-System's `mbnavadjust`: the bathymetry of overlapping
and crossing swath lines is made to agree by adjusting their navigation. The files of a project are cut
into sections; where two sections overlap they form a crossing, at which an offset — a tie — is set by
hand (dragging one section's contours over the other's) or by correlation (Auto-Pick). An inversion turns
the ties into a smooth navigation adjustment for every file, which mbprocess then applies.

- `project`: an mbnavadjust project (`.nvh`) to open, as `mbnavadjust -I project`; empty: File > New or
  File > Open in the tool.
- `fig`: the viewer window the tool belongs to (it parks in its Scene Objects when closed or minimised).

The project's programs (mbdatalist, mbprocess, mbgrid) run as MB-System's GMT modules, in this process;
View > Visualize Survey opens the project grid in a viewer window with the navigation and the ties on it.
It is also on Geophysics > MB-System, as "MBnavadjust". The swath files are read through MB-System's MBIO
library, the same one the swath editor (`mbedit`) uses and found the same way.

Returns `true` when the tool is open (and the project, when one is given).

```julia
mbnavadjust("C:/surveys/adjust/Project.nvh")
```
"""
function mbnavadjust(project::String = ""; fig = nothing)
	isdefined(@__MODULE__, :_push_mbio_hint) && _push_mbio_hint()   # the MBIO GMT loads (mbedit.jl)
	_register_mbnavadjust()
	h = fig === nothing ? C_NULL : fig.h
	ok = ccall(_fn(:gmtvtk_mbnavadjust_open), Cint, (Ptr{Cvoid}, Cstring), h, project)
	_start_pump()
	return ok == 1
end

# --- for tests ------------------------------------------------------------------------------------
function _mbnavadjust_state()
	v = zeros(Cint, 18)
	ccall(_fn(:gmtvtk_mbnavadjust_state), Cint, (Ptr{Cint}, Cint), v, Cint(18))
	return (open = v[1] == 1, project_open = v[2] == 1, num_files = Int(v[3]), num_surveys = Int(v[4]),
	        num_crossings = Int(v[5]), num_crossings_analyzed = Int(v[6]), num_truecrossings = Int(v[7]),
	        num_ties = Int(v[8]), num_globalties = Int(v[9]), inversion_status = Int(v[10]), grid_status = Int(v[11]),
	        naverr_mode = Int(v[12]), view_list = Int(v[13]), crossing_select = Int(v[14]), tie_select = Int(v[15]),
	        modelplot = v[16] == 1, visualization = v[17] == 1, list_count = Int(v[18]))
end

_mbnavadjust_press(name::String)::Bool = ccall(_fn(:gmtvtk_mbnavadjust_press), Cint, (Cstring,), name) == 1

# File > New (1) / Open (2) / Import Swath Data (3) / as One Survey (4) / Reference Grid (5), no dialog
_mbnavadjust_file(mode::Int, path::String, format::Int = 0)::Bool =
	ccall(_fn(:gmtvtk_mbnavadjust_file_select), Cint, (Cint, Cstring, Cint), Cint(mode), path, Cint(format)) == 1

_mbnavadjust_list_select(row::Int)::Bool = ccall(_fn(:gmtvtk_mbnavadjust_list_select), Cint, (Cint,), Cint(row)) == 1

function _mbnavadjust_list_item(row::Int)::String
	n = ccall(_fn(:gmtvtk_mbnavadjust_list_item), Cint, (Cint, Ptr{UInt8}, Cint), Cint(row), C_NULL, Cint(0))
	n < 0 && return ""
	buf = zeros(UInt8, n + 1)
	ccall(_fn(:gmtvtk_mbnavadjust_list_item), Cint, (Cint, Ptr{UInt8}, Cint), Cint(row), buf, Cint(n + 1))
	return String(buf[1:n])
end

_mbnavadjust_mouse(canvas::Int, button::Int, x0::Int, y0::Int, x1::Int = x0, y1::Int = y0)::Bool =
	ccall(_fn(:gmtvtk_mbnavadjust_mouse), Cint, (Cint, Cint, Cint, Cint, Cint, Cint),
	      Cint(canvas), Cint(button), Cint(x0), Cint(y0), Cint(x1), Cint(y1)) == 1

_mbnavadjust_save_png(canvas::Int, path::String)::Bool =
	ccall(_fn(:gmtvtk_mbnavadjust_save_png), Cint, (Cint, Cstring), Cint(canvas), path) == 1

function _mbnavadjust_messages()::String
	n = ccall(_fn(:gmtvtk_mbnavadjust_messages), Cint, (Ptr{UInt8}, Cint), C_NULL, Cint(0))
	buf = zeros(UInt8, n + 1)
	ccall(_fn(:gmtvtk_mbnavadjust_messages), Cint, (Ptr{UInt8}, Cint), buf, Cint(n + 1))
	return String(buf[1:n])
end

_mbnavadjust_close()::Bool = ccall(_fn(:gmtvtk_mbnavadjust_close), Cint, ()) == 1
