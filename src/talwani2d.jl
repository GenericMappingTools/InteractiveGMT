# talwani2d.jl — GMT menu > "talwani2d": free-air, geoid or vertical-gravity-gradient anomalies over
# 2-D (or 2.5-D) bodies given as CROSS-SECTION polygons, by the method of Talwani.
#
# The C++ dialog is Talwani2DDialog (70_window.cpp, loads deps/ui/talwani2d_dialog.ui).
#
# GMT.jl has NO `talwani2d` wrapper (nor a `talwani3d` one), so this goes through MONOLITHIC mode —
# `GMT.gmt("talwani2d …")` — exactly like the gravfft dialog's -C branch and the mgd77list calls in
# gmtedit.jl. That is not a workaround: the module is a normal GMT module, monolithic mode is the
# documented way to reach one that has no Julia wrapper, and the option string built here is the one
# a user would type.
#
# The model is a FILE and stays a file: talwani2d reads cross-sections one segment per body and
# takes each body's DENSITY out of that segment's header, which is precisely the part a re-read on
# our side would be most likely to get wrong. What this file does check, before GMT is called at
# all, is that those headers actually carry a density — because the module's own answer to a model
# without one is to quit after the dialog has already put up a busy cursor.

# A file path that travels inside a GMT command STRING is double-quoted. GMT_Create_Options masks
# spaces inside quotes before splitting on space and strips the quotes afterwards, which is what
# lets a path like C:/My Documents/body2d.txt survive the trip. Shared by every dialog that puts a
# path in a command string rather than reading the file itself: talwani2d, talwani3d, gravprisms.
_gmt_quote_path(path::AbstractString) = '"' * String(path) * '"'

# The three geopotential fields the two talwani modules share (-F), with the unit each is reported in.
const _TAL_FIELDS = Dict("f" => ("Free-air anomaly", "mGal"),
                         "n" => ("Geoid", "m"),
                         "v" => ("Vertical gravity gradient", "Eotvos"))

_tal_fieldname(f::AbstractString) = get(_TAL_FIELDS, string(first(f)), ("Anomaly", ""))[1]
_tal_fieldunit(f::AbstractString) = get(_TAL_FIELDS, string(first(f)), ("Anomaly", ""))[2]
_tal_fieldlabel(f::AbstractString) = _tal_fieldname(f) * " (" * _tal_fieldunit(f) * ")"

# -F: which field, and for the geoid the latitude at which normal gravity is evaluated. Shared by
# both dialogs — the option is identical in talwani2d and talwani3d.
function _tal_F(d::Dict{String,String})::String
	f = _get(d, "field", "f")
	(f in ("f", "n", "v")) ||
		error("the field is the free-air anomaly (f), the geoid (n) or the VGG (v), not '$f'")
	f == "n" || return f
	lat = _get(d, "lat")
	isempty(lat) && return "n"                       # the module's own default reference latitude
	v = tryparse(Float64, lat)
	(v === nothing || !(-90 <= v <= 90)) &&
		error("the geoid reference latitude is between -90 and 90, not '$lat'")
	return "n" * lat
end

# -M[h][v]: which distances are in km rather than metres. Shared by both dialogs.
_tal_M(d::Dict{String,String}) = (_on(d, "hkm") ? "h" : "") * (_on(d, "vkm") ? "v" : "")

# -D: one density contrast for every body, overriding the segment headers. GMT reads |rho| < 10 as
# g/cm^3 and anything larger as kg/m^3, so both spellings are legal and neither is corrected here.
function _tal_D(d::Dict{String,String})::String
	v = _get(d, "density")
	isempty(v) && return ""
	(tryparse(Float64, v) === nothing) && error("the fixed density contrast must be a number, not '$v'")
	return v
end

# What the segment headers of a model file carry. The modules pull the density (2-D) or the depth
# and density (3-D) out of the header text with a plain `sscanf`, so this counts exactly what that
# sscanf would consume: the run of leading NUMERIC tokens.
#
# Returns (number of segment headers, the smallest such count over all of them, saw any data at all).
# Headers written by `grdcontour -D` say "contour -Z…" and are read by talwani3d along its own path,
# so they are counted but not judged.
function _tal_scan_model(file::AbstractString)
	nseg, worst, sawdata = 0, typemax(Int), false
	for line in eachline(String(file))
		s = lstrip(line)
		isempty(s) && continue
		if s[1] == '>'
			nseg += 1
			occursin("contour -Z", s) && continue
			k = 0
			for tok in split(SubString(s, 2))
				(tryparse(Float64, tok) === nothing) && break
				k += 1
			end
			worst = min(worst, k)
		elseif s[1] != '#'
			sawdata = true
		end
	end
	return nseg, (nseg == 0 ? 0 : worst), sawdata
end

# A -N track file for the THREE-DIMENSIONAL modules (talwani3d, gravprisms) must carry the
# observation level as a third column unless a constant one was given with -Z. GMT does not merely
# refuse a shorter file: it warns "Mismatch between actual (2) and expected (3) fields" and then dies
# inside gmtlib_read_table with an ACCESS VIOLATION, taking the whole Julia session with it — a crash
# no try/catch in the callback can survive. Verified 2026-08-25 on GMT 6.7 for both modules, and that
# the same 2-column file is perfectly legal once -Z supplies the level.
# talwani2d is NOT in this: its -N file is a list of x alone, and one column is what it wants.
# One check, one function, both callers (SACRED_LAW): never re-spell it at a call site.
function _tal_check_track(file::AbstractString, haveLevel::Bool)
	isfile(file) || error("track file not found: $file")
	haveLevel && return nothing                      # -Z states the level, so 2 columns is enough
	# `open ... do` and not `eachline(path)`: the first data record answers the question, and leaving
	# that loop early over a PATH leaves the stream open — which on Windows LOCKS the user's own track
	# file for the rest of the session. The `do` block closes it whichever way this leaves.
	ncol = -1                                        # -1: not one data record in the file
	open(String(file), "r") do io
		for line in eachline(io)
			s = lstrip(line)
			(isempty(s) || s[1] == '#' || s[1] == '>') && continue
			k = 0
			for tok in split(s)
				(tryparse(Float64, tok) === nothing) && break
				k += 1
			end
			ncol = k
			break
		end
	end
	ncol < 0 && error("the track file has no locations in it: $file")
	ncol >= 3 && return nothing
	error("the track file gives $ncol column(s) per point: add the observation level as a third " *
	      "column, or set a constant one")
end

# The model file talwani2d will accept: coordinates in it, and a density for every body — either in
# each segment header or, once and for all, in -D. Refusing here rather than letting the module quit
# is the difference between a sentence and a busy cursor followed by an empty result.
function _tal2d_check_model(file::AbstractString, haveD::Bool)
	isfile(file) || error("model file not found: $file")
	nseg, nnum, sawdata = _tal_scan_model(file)
	sawdata || error("the model file has no cross-section coordinates in it: $file")
	haveD && return nothing                          # -D answers the density question for every body
	(nseg == 0) &&
		error("the model file has no segment header (>) to take a density from — give a fixed density contrast instead")
	(nnum < 1) &&
		error("a segment header of the model file carries no density — give a fixed density contrast instead")
	return nothing
end

# -T<min>/<max>/<inc>[+n]: the equidistant profile the anomaly is computed on. With "+n" the third
# box is the NUMBER of points instead of the step, which is the module's own array syntax.
function _tal2d_T(d::Dict{String,String})::String
	lo, hi, inc = _get(d, "tmin"), _get(d, "tmax"), _get(d, "tinc")
	(isempty(lo) || isempty(hi) || isempty(inc)) &&
		error("the profile needs its first x, its last x and the step (or the number of points)")
	for (v, what) in ((lo, "first x"), (hi, "last x"), (inc, "step"))
		(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
	end
	(parse(Float64, lo) < parse(Float64, hi)) || error("the first x must be smaller than the last one")
	if _on(d, "tnum")
		(parse(Float64, inc) >= 2) || error("as a NUMBER of points that must be at least 2, not '$inc'")
		return lo * "/" * hi * "/" * inc * "+n"
	end
	(parse(Float64, inc) > 0) || error("the step must be positive")
	return lo * "/" * hi * "/" * inc
end

# -Z<level>[/<ymin>/<ymax>]: the observation level, and — free-air only — the finite extent of the
# body along strike, which is what turns a 2-D body into a 2.5-D one (Rasmussen & Pedersen, 1979).
function _tal2d_Z(d::Dict{String,String}, field::AbstractString)::String
	lvl = _get(d, "level")
	y0, y1 = _get(d, "y25min"), _get(d, "y25max")
	if isempty(y0) && isempty(y1)
		isempty(lvl) && return ""
		(tryparse(Float64, lvl) === nothing) && error("the observation level must be a number, not '$lvl'")
		return lvl
	end
	(isempty(y0) || isempty(y1)) && error("a 2.5-D body needs BOTH ends of its extent along strike")
	(field == "f") || error("a finite extent along strike (2.5-D) is for free-air anomalies only")
	for (v, what) in ((y0, "near end"), (y1, "far end"))
		(tryparse(Float64, v) === nothing) && error("the $what of the strike extent must be a number, not '$v'")
	end
	(parse(Float64, y0) < parse(Float64, y1)) ||
		error("the near end of the strike extent must be smaller than the far end")
	isempty(lvl) && (lvl = "0")                      # -Z needs its level before the two limits
	return lvl * "/" * y0 * "/" * y1
end

# What comes back: x, then the anomaly in the second column. With -N the module overwrites the
# track's second column with the answer and leaves any further columns as they were read.
function _tal2d_colnames(field::AbstractString, ncol::Int)::Vector{String}
	ncol <= 0 && return String[]
	names = ["x", _tal_fieldlabel(field)]
	ncol <= 2 && return names[1:ncol]
	append!(names, ["column $k" for k in 3:ncol])
	return names
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaTalwani2DFn. Returns Cint 1 on success, 0 on failure.
function _on_talwani2d(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		model = _get(d, "infile")
		isempty(model) && error("no model file")
		dens = _tal_D(d)
		_tal2d_check_model(model, !isempty(dens))

		field = _get(d, "field", "f")
		opts = String[_gmt_quote_path(model), "-F" * _tal_F(d)]
		isempty(dens) || push!(opts, "-D" * dens)
		_on(d, "zup") && push!(opts, "-A")            # the z-axis of the model points UP
		M = _tal_M(d);  isempty(M) || push!(opts, "-M" * M)

		mode = _get(d, "mode", "lattice")
		if mode == "track"
			# -N and -T are the module's two mutually exclusive ways of saying WHERE to compute, so
			# the dialog's radios send one or the other and never both.
			trk = _get(d, "trackfile")
			isempty(trk) && error("choose the file with the output locations")
			isfile(trk) || error("track file not found: $trk")
			push!(opts, "-N" * _gmt_quote_path(trk))
		elseif mode == "lattice"
			push!(opts, "-T" * _tal2d_T(d))
		else
			error("unknown output mode '$mode'")
		end
		Z = _tal2d_Z(d, field);  isempty(Z) || push!(opts, "-Z" * Z)

		# MONOLITHIC mode: GMT.jl has no talwani2d wrapper. The module's keys are "<D{,ND(,>D}", so
		# the model and track FILES satisfy the two inputs and the table comes back through the
		# implicit virtual output file — no temporary file anywhere.
		R = GMT.gmt("talwani2d " * join(opts, ' '))
		R === nothing && error("talwani2d returned nothing")
		Ds = isa(R, Vector) ? R : [R]
		isempty(Ds) && error("talwani2d returned no rows")
		all(x -> isa(x, GMTdataset), Ds) || error("got a $(typeof(R)), not a table")
		for D in Ds
			D.colnames = _tal2d_colnames(field, size(D.data, 2))
		end

		out = _get(d, "outfile");  isempty(out) || GMT.gmtwrite(String(out), R)
		label = _tal_fieldname(field)
		show_table(scene, length(Ds) == 1 ? Ds[1] : Ds; name = "talwani2d (" * label * ")")

		# The answer IS a profile, so it goes to the X,Y plot tool — reading a modelled anomaly off a
		# spreadsheet is not the same thing as seeing its shape against the body that made it.
		if _on(d, "plot")
			p = nothing
			for (k, D) in enumerate(Ds)
				m = D.data
				(size(m, 1) > 1 && size(m, 2) >= 2) || continue
				nm = length(Ds) == 1 ? label : label * " #" * string(k)
				x = Float64.(@view m[:, 1])
				y = Float64.(@view m[:, 2])
				if p === nothing
					p = xyplot(x, y; name = nm, title = "talwani2d", xlabel = "x",
					           ylabel = _tal_fieldlabel(field))
				else
					add!(p, x, y; name = nm)
				end
			end
		end
		return Cint(1)
	catch e
		_tool_failed(scene, "talwani2d", e)
		return Cint(0)
	end
end

function _register_talwani2d()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_talwani2d, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_talwani2d_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
