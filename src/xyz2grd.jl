# xyz2grd.jl — GMT menu > "xyz2grd": turn an x,y,z table into a grid, through GMT.jl's own
# `xyz2grd`. This is NOT gridding: xyz2grd assumes the data already SIT on the nodes of the
# region/increment it is given (that is what makes it exact and fast, and why it can also read a
# bare one-column table with -Z). To interpolate scattered data onto a grid, the Interpolate dialog
# (surface, nearneighbor, …) is the tool.
#
# The C++ dialog is Xyz2GrdDialog (70_window.cpp, loads deps/ui/xyz2grd_dialog.ui).
#
# The FILE is handed to the module, not read here: xyz2grd is the side that knows how to apply -h,
# -i, -: and the whole -Z one-column convention (including its binary forms), and reading it twice —
# once our way, once its way — is exactly how the two would drift apart.

# What to do when several records land on the same node (-A). The letters are the module's own; the
# dialog offers them by name, so an unknown one here means the two lists have drifted apart.
const _X2G_MULTI = ("d", "f", "l", "m", "n", "r", "S", "s", "u", "z")

# -D: the header fields the new grid carries. Built from whichever boxes were filled, in the order
# the manual lists them; empty = don't pass -D at all.
function _x2g_header(d::Dict{String,String})::String
	s = ""
	for (key, mod) in (("dxname", "+x"), ("dyname", "+y"), ("dzname", "+z"),
	                   ("dtitle", "+t"), ("dremark", "+r"))
		v = _get(d, key);  isempty(v) || (s *= mod * v)
	end
	return s
end

# -R and -I are what xyz2grd needs to know WHERE the nodes are; without them it has no grid to fill.
function _x2g_geometry(d::Dict{String,String})
	reg = _get(d, "region")
	(isempty(reg) || occursin("//", reg)) &&
		error("give the full region (xmin, xmax, ymin, ymax) the table covers")
	inc = _get(d, "inc")
	isempty(inc) && error("give the grid increment")
	return reg, inc
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaXyz2GrdFn. Returns Cint 1 on success, 0 on failure.
function _on_xyz2grd(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		infile = _get(d, "infile")
		isempty(infile) && error("no input table")
		isfile(infile) || error("input table not found: $infile")

		reg, inc = _x2g_geometry(d)
		kw = Dict{Symbol,Any}(:region => reg, :inc => inc)
		_on(d, "pixel") && (kw[:registration] = :pixel)
		_on(d, "geog")  && (kw[:f] = :g)          # the nodes are lon/lat, so say so on the grid too

		am = _get(d, "amode")
		if !isempty(am)
			(am in _X2G_MULTI) ||
				error("unknown rule '$am' for several values on one node (see the manual's -A)")
			kw[:A] = am
		end
		z = _get(d, "zflags");  isempty(z) || (kw[:Z] = z)   # one-column table, the module's own -Z
		h = _get(d, "headers")
		if !isempty(h)
			(tryparse(Int, h) === nothing || parse(Int, h) < 0) &&
				error("the number of header lines must be a non-negative integer, not '$h'")
			kw[:h] = h
		end
		ic = _get(d, "incols");  isempty(ic) || (kw[:i] = ic)   # -i: which columns are x, y and z
		_on(d, "toggle") && (kw[:yx] = true)                    # the file's first two columns are y,x
		hdr = _x2g_header(d);  isempty(hdr) || (kw[:D] = hdr)

		G = GMT.xyz2grd(String(infile); kw...)
		isa(G, GMTgrid) || error("got a $(typeof(G)), not a grid")

		return _gm3d_deliver(scene, G, "Grid (" * basename(String(infile)) * ")",
		                     _get(d, "outfile"), false,
		                     "xyz2grd " * join(("$k=$v" for (k, v) in kw), ' ');
		                     geographic = _on(d, "geog") ? true : nothing)
	catch e
		_tool_failed(scene, "xyz2grd", e)
		return Cint(0)
	end
end

function _register_xyz2grd()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_xyz2grd, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_xyz2grd_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
