# empilhador.jl — Tools > "Empilhador" (port of Mirone's src_figs/empilhador.m). Stack a list of 2-D
# grids, or of MODIS/VIIRS/SeaWiFS L2 scenes, into ONE 3-D netCDF (or VTK, or a multi-band TIFF, or a
# VRT list). The C++ dialog (EmpilhadorDialog, 70_window.cpp) loads deps/ui/empilhador.ui at runtime
# and drives everything through the two callbacks below.
#
# NONE of the stacking lives here: the whole engine is GMT.jl's `empilhador` (GMT/src/empilhador.jl),
# so the command line and this dialog run the SAME code — there is no GUI-only fork of the maths.
# This file only turns the dialog's "key=value" block into that function's keywords.

# Split the dialog's newline-separated "key=value" block into a Dict. Values may contain '=' (paths
# rarely do, but a wildcard request can), so only the FIRST '=' separates.
function _emp_params(s::AbstractString)
	d = Dict{String,String}()
	for ln in split(s, '\n')
		ln = strip(ln)
		isempty(ln) && continue
		i = findfirst('=', ln)
		i === nothing && continue
		d[String(ln[1:i-1])] = String(strip(ln[i+1:end]))
	end
	return d
end

# The files picked one by one, in the order the dialog listed them (file0=, file1=, ...).
function _emp_file_list(d::Dict{String,String})
	names = String[]
	k = 0
	while haskey(d, "file$k")
		push!(names, d["file$k"]);		k += 1
	end
	return names
end

"""
    _emp_config_str() -> nothing

Print `"w/e/s/n;inc;ncells;quality"` from ~/.gmt/L2config.txt, for the dialog to fill its boxes with
when "Use config file" is ticked. The FILE is parsed by GMT.jl (`_emp_sniff_config`) — the one and
only reader of that format — never re-parsed here.
"""
function _emp_config_str()
	L2 = GMT.EmpL2()
	GMT._emp_sniff_config(GMT.l2config_file(true), L2)		# create a default one if absent
	R = L2.got_R ? string(L2.region[1], "/", L2.region[2], "/", L2.region[3], "/", L2.region[4]) : ""
	print(R, ";", L2.inc, ";", L2.ncells, ";", L2.quality)
	return nothing
end

"""
    _on_empilhador(scene, cparams) -> Cint

C callback (Compute button). `cparams` is the "key=value" block documented in 30_app.cpp. Returns 1
on success, 0 on failure — same contract as `_on_clipgrid`: the dialog reports it, because the
Errors console may be on a window the user is not looking at.
"""
function _on_empilhador(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		warm_wait("empilhador")			# never enter GMT twice at once (src/warmup.jl)
		p = _emp_params(unsafe_string(cparams))
		out = get(p, "out", "")
		isempty(out) && error("Empilhador: no output file name")

		kw = Dict{Symbol,Any}(:outfile => out, :format => Symbol(get(p, "fmt", "netcdf")))
		haskey(p, "region") && (kw[:region] = Tuple(parse.(Float64, split(p["region"], '/'))))
		(get(p, "l2", "0") == "1")     && (kw[:l2] = true)
		(get(p, "config", "0") == "1") && (kw[:config] = true)
		(get(p, "bitflags", "0") == "1") && (kw[:bitflags] = true)
		haskey(p, "quality") && (kw[:quality] = parse(Int, p["quality"]))
		if haskey(p, "inc")
			v = tryparse(Float64, p["inc"])
			(v !== nothing && v > 0) && (kw[:inc] = v)
		end
		if haskey(p, "ncells")
			v = tryparse(Int, p["ncells"])
			(v !== nothing && v >= 0) && (kw[:ncells] = v)
		end
		# Walk the list in the console, so a long L2 job shows it is alive (the UI thread is blocked
		# by the compute, exactly as for every other tool here).
		kw[:progress] = (k, n, nm) -> println("Empilhador: [$k/$n] ", basename(nm))

		files = _emp_file_list(p)
		if !isempty(files)
			GMT.empilhador(files; kw...)
		else
			GMT.empilhador(get(p, "list", ""); kw...)
		end
		println("Empilhador: wrote ", out)
		return Cint(1)
	catch err
		@error "Empilhador failed" exception = (err, catch_backtrace())
		return Cint(0)
	end
end

# Warm-up body (src/warmup.jl): the dialog opening fires this, so the first Compute does not pay the
# JIT for the whole GMT.jl `empilhador` chain. Pure compute on throw-away data — nothing here touches
# the scene, so no `precompile`-only entries are needed (see warmup.jl's body rule).
function _emp_warm()
	GMT._emp_time_from_name("A2012024021000.L2_LAC_SST4");	yield()
	GMT._emp_flagbits(GMT.L2_DEFAULT_FLAGS)
	GMT._emp_akima([1.0, 5, 9, 13], [0.0, 2, 1, 3], collect(1.0:13.0));		yield()
	L2 = GMT.EmpL2(active = true, inc = 0.5, ncells = 1, region = (0.0, 4.0, 0.0, 4.0), got_R = true)
	x = repeat(collect(0.0:0.5:4.0), 9);	y = repeat(collect(0.0:0.5:4.0), inner = 9)
	GMT._emp_smart_grid(x, y, x .+ y, L2, 0.0, 4.0, 0.0, 4.0, 9, 9);		yield()
	precompile(_on_empilhador, (Ptr{Cvoid}, Cstring))
	precompile(_emp_config_str, ())
	return nothing
end

function _register_empilhador()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_empilhador, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_empilhador_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("empilhador", _emp_warm)		# C++ fires this when the dialog opens (70_window.cpp)
	return
end
