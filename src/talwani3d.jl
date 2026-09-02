# talwani3d.jl — GMT menu > "talwani3d": free-air, geoid or vertical-gravity-gradient anomalies over
# 3-D bodies given as stacked HORIZONTAL CONTOURS, by the method of Talwani & Ewing.
#
# The C++ dialog is Talwani3DDialog (70_window.cpp, loads deps/ui/talwani3d_dialog.ui).
#
# Like its 2-D twin this runs in MONOLITHIC mode (GMT.jl has no `talwani3d` wrapper) and keeps the
# model as a FILE, because the module takes each slice's DEPTH and DENSITY out of that slice's
# segment header. `_tal_scan_model`, `_tal_F`, `_tal_M`, `_tal_D` and `_gmt_quote_path` are shared with
# talwani2d.jl — same options, one implementation.
#
# ONE DIFFERENCE THAT IS NOT COSMETIC: the module's output goes to a REAL FILE, never to a virtual
# one. talwani3d declares its output as "G?}" — a family the API only resolves when -N is given — so
# asking for a grid back through memory is not something GMT can encode. The result is therefore
# written where the dialog says, or to a temporary file that is read back and then deleted.

# Where the anomaly is evaluated. These are the module's own three and they are exclusive: -Z<grid>
# goes with neither -R -I -r nor -N, which is what the radios enforce.
const _TAL3D_MODES = ("grid", "track", "obsgrid")

# -R and -I: the lattice a grid run is computed on. Same check as the xyz2grd dialog's, for the same
# reason — a half-given region is the one mistake that produces a plausible wrong grid.
function _tal3d_geometry(d::Dict{String,String})
	reg = _get(d, "region")
	(isempty(reg) || occursin("//", reg)) &&
		error("give the full region (xmin, xmax, ymin, ymax) to compute over")
	inc = _get(d, "inc")
	isempty(inc) && error("give the grid increment")
	return reg, inc
end

# The model file talwani3d will accept. The DEPTH always comes from the slice's segment header (-D
# only ever replaces the density), so a header carrying no number at all is fatal whatever -D says.
function _tal3d_check_model(file::AbstractString, haveD::Bool)
	isfile(file) || error("model file not found: $file")
	nseg, nnum, sawdata = _tal_scan_model(file)
	sawdata || error("the model file has no contour coordinates in it: $file")
	(nseg == 0) &&
		error("the model file has no segment header (>) — each slice needs one carrying its depth" *
		      (haveD ? "" : " and density"))
	(nnum < 1) && error("a segment header of the model file carries no depth")
	(!haveD && nnum < 2) &&
		error("a segment header of the model file has a depth but no density — give a fixed density contrast instead")
	return nothing
end

# -Z: either a constant observation level or the name of a grid OF levels, which then also sets the
# region of the result. Same option, and the two never travel together.
function _tal3d_Z(d::Dict{String,String}, mode::AbstractString)::String
	if mode == "obsgrid"
		g = _get(d, "zgrid")
		isempty(g) && error("choose the grid of observation levels")
		isfile(g) || error("observation-level grid not found: $g")
		return _gmt_quote_path(g)
	end
	lvl = _get(d, "level")
	isempty(lvl) && return ""
	(tryparse(Float64, lvl) === nothing) && error("the observation level must be a number, not '$lvl'")
	return lvl
end

# With -N the module writes x, y, the observation level it used, and the anomaly.
function _tal3d_colnames(field::AbstractString, ncol::Int)::Vector{String}
	ncol <= 0 && return String[]
	names = ["x", "y", "Observation level", _tal_fieldlabel(field)]
	ncol <= 4 && return names[1:ncol]
	append!(names, ["column $k" for k in 5:ncol])
	return names
end

# The whole -option string, built once so the same list can be logged as the recipe of the grid.
function _tal3d_opts(d::Dict{String,String}, model::AbstractString, dens::AbstractString,
                     mode::AbstractString)::Vector{String}
	opts = String[_gmt_quote_path(model), "-F" * _tal_F(d)]
	isempty(dens) || push!(opts, "-D" * dens)
	_on(d, "zup") && push!(opts, "-A")               # the z-axis of the model points UP
	M = _tal_M(d);  isempty(M) || push!(opts, "-M" * M)
	_on(d, "geog") && push!(opts, "-fg")             # lon/lat, converted to km by a flat-Earth rule

	if mode == "grid"
		reg, inc = _tal3d_geometry(d)
		push!(opts, "-R" * reg, "-I" * inc)
		_on(d, "pixel") && push!(opts, "-r")
	elseif mode == "track"
		trk = _get(d, "trackfile")
		isempty(trk) && error("choose the file with the output locations")
		_tal_check_track(trk, !isempty(_get(d, "level")))   # THE track check (talwani2d.jl)
		push!(opts, "-N" * _gmt_quote_path(trk))
	elseif mode == "obsgrid"
		# The observation grid states both the levels AND the region, so -R/-I have nothing to add.
	else
		error("unknown output mode '$mode'")
	end
	Z = _tal3d_Z(d, mode);  isempty(Z) || push!(opts, "-Z" * Z)
	return opts
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaTalwani3DFn. Returns Cint 1 on success, 0 on failure.
function _on_talwani3d(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	tmpout = ""
	try
		d = _nswing_parse(unsafe_string(cparams))
		model = _get(d, "infile")
		isempty(model) && error("no model file")
		dens = _tal_D(d)
		_tal3d_check_model(model, !isempty(dens))

		mode = _get(d, "mode", "grid")
		(mode in _TAL3D_MODES) || error("unknown output mode '$mode'")
		field = _get(d, "field", "f")
		opts = _tal3d_opts(d, model, dens, mode)

		# -G is the module's ONLY way out and it wants a real name (see the note at the top). The
		# user's chosen file IS that name when there is one; otherwise a temporary stands in and is
		# removed again once the result is in memory.
		out = _get(d, "outfile")
		dest = out
		if isempty(dest)
			tmpout = tempname() * (mode == "track" ? ".txt" : ".grd")
			dest = tmpout
		end
		recipe = "talwani3d " * join(opts, ' ')   # without -G: a temporary name is nobody's recipe
		push!(opts, "-G" * _gmt_quote_path(dest))

		GMT.gmt("talwani3d " * join(opts, ' '))
		isfile(dest) || error("talwani3d wrote no output")

		if mode == "track"
			R = GMT.gmtread(String(dest); dataset = true)
			R === nothing && error("talwani3d returned nothing")
			Ds = isa(R, Vector) ? R : [R]
			isempty(Ds) && error("talwani3d returned no rows")
			all(x -> isa(x, GMTdataset), Ds) || error("got a $(typeof(R)), not a table")
			for D in Ds
				D.colnames = _tal3d_colnames(field, size(D.data, 2))
			end
			show_table(scene, length(Ds) == 1 ? Ds[1] : Ds;
			           name = "talwani3d (" * _tal_fieldname(field) * ")")
			# The track's x,y ARE map coordinates, so the computed values can sit on the map at the
			# points they belong to, each carrying its own row as a tooltip.
			if _on(d, "plotpts")
				for D in Ds
					m = D.data
					(size(m, 1) >= 1 && size(m, 2) >= 2) || continue
					names = D.colnames
					infos = [join(("$(names[c]) = $(m[k, c])" for c in 1:size(m, 2)), '\n')
					         for k in 1:size(m, 1)]
					add_symbols!(scene, view(m, :, 1), view(m, :, 2); symbol = :circle, size = 6,
					             fill = :cyan, edge = :black, edgewidth = 0.5,
					             name = "talwani3d (" * basename(String(model)) * ")", info = infos)
				end
			end
			return Cint(1)
		end

		G = GMT.gmtread(String(dest); grd = true)
		isa(G, GMTgrid) || error("got a $(typeof(G)), not a grid")
		# The file was written by GMT itself, so `_gm3d_deliver` is told to save nothing: it would
		# only rewrite what is already on disk.
		return _gm3d_deliver(scene, G, "talwani3d (" * _tal_fieldname(field) * ")", "", false, recipe;
		                     geographic = _on(d, "geog") ? true : nothing)
	catch e
		_tool_failed(scene, "talwani3d", e)
		return Cint(0)
	finally
		if !isempty(tmpout)
			try; rm(tmpout; force = true); catch; end
		end
	end
end

function _register_talwani3d()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_talwani3d, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_talwani3d_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
