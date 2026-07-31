# plates.jl — Plates menu > "Euler rotations": port of Mirone's src_figs/euler_stuff.m (Do Rotations /
# Add poles / Interpolate poles).
#
# When the Mirone code was written GMT had no plate-rotation modules, so it carried its own
# (rot_euler.m, add_poles.m, finite2stages.m, telha_m). GMT grew the `spotter` supplement since, so
# NOTHING of that maths is re-implemented here: every rotation and every pole composition is a GMT
# call.
#
#   line rotation, one finite pole   backtracker -E<lon>/<lat>/<ang> -Db -Q1
#   line rotation at N ages          backtracker -E<rotfile> -Db   (input rows are lon lat age)
#   flow line from one point         backtracker -E<rotfile> -Db -Lf<step>
#   revert the sense of rotation     -Df instead of -Db
#   add two poles                    rotconverter <p1> + <p2>       (verified == Mirone's add_poles)
#   finite poles -> stage poles      rotconverter <file> -Fs        (== Mirone's finite2stages)
#   interpolate a model at age t     rotconverter <stage*frac> + <P(t_k)>   (== Mirone's cumpute_interp,
#                                    every composition done by rotconverter, only the scalar fraction
#                                    of the stage angle is arithmetic)
#   geodetic latitudes               mapproject -Ng   (and -Ng -I on the way back)
#
# The C++ dialog is EulerDialog (70_window.cpp, loads deps/ui/euler_stuff.ui); the "key=value" block
# it sends is described in 30_app.cpp (JuliaEulerFn).

# The rotated lines land as ONE new Scene Objects group of named lines and the SOURCE LINE STAYS
# VISIBLE: a reconstruction is read against the thing it was reconstructed from, so hiding the source
# (the derived-variable display law's usual "uncheck the source") would defeat the tool's purpose.

# ---------------------------------------------------------------------------------------------
# The target line's vertices, straight out of the window: gmtvtk_serialize_vector_h answers for a
# drawn polygon and for an imported line overlay alike, so this side never has to care which kind of
# element the user pointed at. One segment per line of the blob, "x,y" joined by '|'. Two-pass buffer.
function _euler_segments(scene::Ptr{Cvoid}, name::AbstractString)::Vector{Matrix{Float64}}
	segs = Matrix{Float64}[]
	isempty(name) && return segs
	n = ccall(_fn(:gmtvtk_serialize_vector_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
	          scene, String(name), C_NULL, Cint(0))
	n <= 0 && return segs
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_serialize_vector_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
	      scene, String(name), buf, Cint(n + 1))
	for ln in split(unsafe_string(pointer(buf)), '\n'; keepempty = false)
		pts = split(ln, '|'; keepempty = false)
		isempty(pts) && continue
		m = Matrix{Float64}(undef, length(pts), 2)
		ok = true
		for (i, p) in enumerate(pts)
			c = split(p, ',')
			if length(c) < 2;  ok = false; break;  end
			m[i, 1] = parse(Float64, c[1]);  m[i, 2] = parse(Float64, c[2])
		end
		ok && push!(segs, m)
	end
	return segs
end

# Hand the dialog whatever the tab it asked for has to say (the summed pole, the interpolated table,
# the GMT command). Read by EulerDialog::run the moment the callback returns.
# The last answer, kept Julia-side too: the C channel is write-only from here, and the tests (and any
# console user) need to see what a tab just produced.
const _euler_last_result = Ref{String}("")

function _euler_result(txt::AbstractString)
	_euler_last_result[] = String(txt)
	# Best-effort: the pole algebra is worth running even with no viewer library around (the unit
	# tests drive _euler_add/_euler_interp directly), and a window closed under us must not turn a
	# finished computation into an exception.
	try
		ccall(_fn(:gmtvtk_euler_result), Cvoid, (Cstring,), String(txt))
	catch
	end
	return
end

# "Geodetic Lats": the rotation is a rigid rotation of the SPHERE, so geodetic latitudes have to
# become geocentric before it and go back after. GMT does that conversion (mapproject -N[g], -I for
# the inverse) — the ellipsoid formula is never rewritten here.
function _euler_lat_convert(m::Matrix{Float64}, inverse::Bool)::Matrix{Float64}
	isempty(m) && return m
	R = GMT.gmt(inverse ? "mapproject -Ng -I" : "mapproject -Ng", m[:, 1:2])
	out = copy(m)
	D = isa(R, Vector) ? R[1].data : R.data
	out[:, 1] = D[:, 1];  out[:, 2] = D[:, 2]
	return out
end

_euler_num(d, k) = (v = _get(d, k); isempty(v) ? nothing : parse(Float64, v))

# The dialog sends one `target<i>=` line per selected line (the same `file<i>=` convention the other
# multi-input dialogs use). `target=` is still read, so a single-target caller keeps working.
function _euler_targets(d::Dict{String,String})::Vector{String}
	t = String[]
	one = _get(d, "target")
	isempty(one) || push!(t, one)
	i = 1
	while true
		v = _get(d, "target$i")
		isempty(v) && break
		push!(t, v)
		i += 1
	end
	return t
end

# Several lines: rotate each one exactly as a single one is rotated — the SAME function, once per
# line — so a batch can never drift from what one line alone would do.
function _euler_rotate_many(scene::Ptr{Cvoid}, d::Dict{String,String}, targets::Vector{String})::Cint
	ok = 0
	msgs = String[]
	for t in targets
		one = copy(d)
		one["target"] = t
		for i in 1:length(targets);  delete!(one, "target$i");  end
		try
			_euler_rotate(scene, one) == 1 && (ok += 1)
		catch e
			push!(msgs, "$t: $(sprint(showerror, e))")
		end
	end
	ok == 0 && error(isempty(msgs) ? "nothing was rotated" : join(msgs, "\n"))
	_euler_result(isempty(msgs) ? "$ok line(s) rotated" :
	              "$ok of $(length(targets)) line(s) rotated\n" * join(msgs, "\n"))
	return Cint(1)
end

function _euler_ages(d)::Vector{Float64}
	s = _get(d, "ages")
	isempty(s) && return Float64[]
	return [parse(Float64, t) for t in split(s, ','; keepempty = false)]
end

function _euler_age_labels(d, n::Int)::Vector{String}
	s = _get(d, "agelabels")
	l = isempty(s) ? String[] : String.(split(s, ','; keepempty = false))
	return length(l) == n ? l : String[]
end

# ---------------------------------------------------------------------------------------------
# Do Rotations. Three shapes, all of them one backtracker call:
#   * "Use this Pole" — the whole line rotated by one finite pole (Mirone's rot_euler branch);
#   * a rotation file + N ages — the whole line rotated to each age (Mirone's telha_m time slices);
#   * a rotation file + a ONE-POINT target — the point's flow line (Mirone's flow-line branch).
function _euler_rotate(scene::Ptr{Cvoid}, d::Dict{String,String})::Cint
	# One or many lines (the dialog's list is multi-select, Mirone's "GOT n LINE(S) TO WORK WITH"):
	# every one is rotated by the same pole/model, each into its own Scene Objects group.
	targets = _euler_targets(d)
	isempty(targets) && error("no line selected")
	length(targets) > 1 && return _euler_rotate_many(scene, d, targets)
	target = targets[1]
	segs   = _euler_segments(scene, target)
	isempty(segs) && error("no line named \"$target\" in this window")

	usepole  = _on(d, "usepole")
	revert   = _on(d, "revert")
	geodetic = _on(d, "geodetic")
	showcmd  = _on(d, "showcmd")
	dopt     = revert ? "-Df" : "-Db"
	npts     = sum(size(s, 1) for s in segs)

	local eopt::String, model::String
	if usepole
		plon = _euler_num(d, "polelon");  plat = _euler_num(d, "polelat");  pang = _euler_num(d, "poleang")
		(plon === nothing || plat === nothing || pang === nothing) && error("the pole needs Lon, Lat and Angle")
		eopt  = "-E$(plon)/$(plat)/$(pang)"
		model = "$(plon)/$(plat)/$(pang)"
	else
		pf = _get(d, "polesfile")
		isempty(pf) && error("no rotation poles file given")
		isfile(pf) || error("rotation poles file not found: $pf")
		eopt  = "-E$pf"
		model = basename(pf)
	end

	ages   = _euler_ages(d)
	labels = _euler_age_labels(d, length(ages))
	# A single point with a rotation model is a FLOW LINE, not a rotated line — Mirone's own split.
	flowline = !usepole && npts == 1
	if !usepole && !flowline && isempty(ages)
		error("no ages given — the rotations are computed at those ages")
	end

	cmd = if usepole
		"backtracker $eopt $dopt -Q1"
	elseif flowline
		# -Lf<step> samples the flow line every <step> km; bare -Lf would give one vertex per stage
		# boundary (6 for a 5-stage model), which draws as a chain of straight chords. 25 km reads as a
		# smooth curve at map scale. The oldest age asked for is how far back the line goes.
		amax = isempty(ages) ? 100.0 : maximum(ages)
		"backtracker $eopt $dopt -Lf25 -Q$amax"
	else
		"backtracker $eopt $dopt"
	end
	if showcmd
		_euler_result(cmd * "\n\ninput: the line's lon lat" * (usepole || flowline ? "" : " age") *
		              (geodetic ? "\ngeodetic lats: mapproject -Ng before, -Ng -I after" : ""))
		return Cint(1)
	end

	# One call for everything: the segments (and, for the multi-age case, one copy of them per age)
	# go in as a single matrix, and the answer comes back row-for-row in the same order.
	work = geodetic ? [_euler_lat_convert(s, false) for s in segs] : segs
	local mat::Matrix{Float64}
	if usepole || flowline
		mat = vcat(work...)
	else
		mat = vcat([hcat(s, fill(a, size(s, 1))) for a in ages for s in work]...)
	end
	R = GMT.gmt(cmd, mat)
	D = isa(R, Vector) ? vcat([r.data for r in R]...) : R.data
	size(D, 1) == 0 && error("backtracker returned nothing")

	# The group carries what was done, so two runs on the same line (say with and without geodetic
	# latitudes) never end up as two identically-labelled rows.
	geotag = geodetic ? ", geodetic" : ""
	grp = usepole ? "$target rotated ($model$geotag)" : "$target reconstructed ($model$geotag)"
	added = 0
	if flowline
		# The path comes back as (lon, lat, age); it is ONE line whatever its length.
		out = geodetic ? _euler_lat_convert(Matrix{Float64}(D[:, 1:2]), true) : Matrix{Float64}(D[:, 1:2])
		_add_dataset_to_scene(scene, GMT.mat2ds(out), "$target flow line"; groupName = grp) && (added += 1)
	elseif usepole
		off = 0
		for (k, s) in enumerate(work)
			n = size(s, 1)
			out = Matrix{Float64}(D[off+1:off+n, 1:2])
			geodetic && (out = _euler_lat_convert(out, true))
			nm = length(work) == 1 ? "$target rot $model$geotag" : "$target rot $model$geotag [$k]"
			_add_dataset_to_scene(scene, GMT.mat2ds(out), nm; groupName = grp) && (added += 1)
			off += n
		end
	else
		off = 0
		for (ai, a) in enumerate(ages)
			lab = isempty(labels) ? "$a Ma" : labels[ai] * " Ma"
			for (k, s) in enumerate(work)
				n = size(s, 1)
				out = Matrix{Float64}(D[off+1:off+n, 1:2])
				geodetic && (out = _euler_lat_convert(out, true))
				nm = length(work) == 1 ? "$target @ $lab" : "$target @ $lab [$k]"
				_add_dataset_to_scene(scene, GMT.mat2ds(out), nm; groupName = grp) && (added += 1)
				off += n
			end
		end
	end
	added == 0 && error("nothing was added to the window")
	# The new group must be VISIBLE as a group, not hidden inside a folded row.
	ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
	_euler_result(cmd)
	return Cint(1)
end

# ---------------------------------------------------------------------------------------------
# Add poles: rotconverter's own composition, which reproduces Mirone's add_poles(p1, p2) exactly
# (checked against its T = R2*R1 for the same inputs). -N brings the answer to the northern
# hemisphere, which is the flip Mirone did by hand afterwards.
function _euler_add(d::Dict{String,String})::Cint
	g(k) = (v = _euler_num(d, k); v === nothing ? error("both poles need Lon, Lat and Angle") : v)
	cmd = "rotconverter $(g("p1lon"))/$(g("p1lat"))/$(g("p1ang")) + " *
	      "$(g("p2lon"))/$(g("p2lat"))/$(g("p2ang")) -N"
	if _on(d, "showcmd")
		_euler_result(cmd)
		return Cint(1)
	end
	R = GMT.gmt(cmd)
	D = isa(R, Vector) ? R[1].data : R.data
	size(D, 1) == 0 && error("rotconverter returned nothing")
	_euler_result(join((_ffmt(D[1, 1], 6), _ffmt(D[1, 2], 6), _ffmt(D[1, 3], 6)), ' '))
	return Cint(1)
end

# ---------------------------------------------------------------------------------------------
# Read a table of finite (total reconstruction) poles. GMT writes them "lon lat age angle", Mirone
# "lon lat angle age" — both are in circulation (the shipped catalogue is Mirone's), so the age is
# taken to be whichever of the two columns is non-negative AND sorted, GMT's order winning a tie.
function _euler_read_poles(path::AbstractString)::Matrix{Float64}
	rows = Vector{Float64}[]
	for ln in eachline(path)
		t = strip(ln)
		(isempty(t) || startswith(t, '#')) && continue
		t = split(t, '!')[1]
		c = split(t, r"[\s,]+"; keepempty = false)
		length(c) < 4 && continue
		v = tryparse.(Float64, c[1:4])
		any(x -> x === nothing, v) && continue
		push!(rows, Float64[v[1], v[2], v[3], v[4]])
	end
	isempty(rows) && error("no poles read from $path")
	m = reduce(vcat, (permutedims(r) for r in rows))
	return _euler_poles_gmt_order(m)
end

function _euler_poles_gmt_order(m::Matrix{Float64})::Matrix{Float64}
	sorted(v) = all(diff(v) .>= 0) && all(v .>= 0)
	sorted(m[:, 3]) && return m                       # already lon lat age angle
	sorted(m[:, 4]) && return m[:, [1, 2, 4, 3]]      # Mirone's lon lat angle age
	error("cannot tell which column holds the age (need lon lat age angle, or lon lat angle age)")
end

# Interpolate poles: Mirone's cumpute_interp, with rotconverter doing every rotation.
#   age <= the youngest pole's age   ->  that pole with its angle scaled by age/t1
#   age inside the model             ->  the stage that spans it, its angle scaled by the fraction of
#                                        the interval, composed with the bracketing younger pole
# Ages past the oldest pole are dropped (no extrapolation), exactly as Mirone does.
function _euler_interp(d::Dict{String,String})::Cint
	inline = _get(d, "poles")
	local P::Matrix{Float64}
	if !isempty(inline)
		rows = Vector{Float64}[]
		for rec in split(inline, ';'; keepempty = false)
			c = split(strip(rec), r"\s+"; keepempty = false)
			length(c) < 4 && continue
			push!(rows, [parse(Float64, c[1]), parse(Float64, c[2]), parse(Float64, c[3]), parse(Float64, c[4])])
		end
		isempty(rows) && error("no usable poles picked from the catalogue")
		P = _euler_poles_gmt_order(reduce(vcat, (permutedims(r) for r in rows)))
	else
		pf = _get(d, "polesfile")
		isempty(pf) && error("no finite poles given")
		isfile(pf) || error("poles file not found: $pf")
		P = _euler_read_poles(pf)
	end
	P = P[sortperm(P[:, 3]), :]                        # youngest first, like Mirone's sortrows
	size(P, 1) < 1 && error("need at least one pole")

	ages = _euler_ages(d)
	isempty(ages) && error("no ages to interpolate at")

	# rotconverter needs the model on disk to give the stage poles (== Mirone's finite2stages).
	tmp = joinpath(tempdir(), "igmt_euler_$(time_ns()).rot")
	stages = try
		open(tmp, "w") do io
			for r in eachrow(P);  println(io, join(r, '\t'));  end
		end
		R = GMT.gmt("rotconverter $tmp -Fs")
		isa(R, Vector) ? R[1].data : R.data
	finally
		isfile(tmp) && rm(tmp, force = true)
	end

	if _on(d, "showcmd")
		_euler_result("rotconverter <poles> -Fs\nrotconverter <stage, angle x fraction> + <bracketing pole> -N")
		return Cint(1)
	end

	out = Vector{NTuple{4,Float64}}()
	tmax = P[end, 3]
	for a in ages
		a > tmax && continue                            # no extrapolation, same as Mirone
		if a <= P[1, 3]                                 # younger than the first pole: scale its angle
			f = P[1, 3] == 0 ? 0.0 : a / P[1, 3]
			push!(out, (P[1, 1], P[1, 2], P[1, 4] * f, a))
			continue
		end
		k = findlast(t -> t < a, P[:, 3])                # bracketing pair k (younger) .. k+1 (older)
		(k === nothing || k >= size(P, 1)) && continue
		t0 = P[k, 3];  t1 = P[k+1, 3]
		frac = (a - t0) / (t1 - t0)
		if frac == 0
			push!(out, (P[k, 1], P[k, 2], P[k, 4], a))
			continue
		end
		# The stage row that spans [t0, t1] (rotconverter writes them "lon lat tstart tend angle").
		si = findfirst(r -> (max(stages[r, 3], stages[r, 4]) ≈ t1) && (min(stages[r, 3], stages[r, 4]) ≈ t0),
		               1:size(stages, 1))
		si === nothing && error("no stage pole spans $t0-$t1 Ma")
		cmd = "rotconverter $(stages[si,1])/$(stages[si,2])/$(frac * stages[si,5]) + " *
		      "$(P[k,1])/$(P[k,2])/$(P[k,4]) -N"
		R = GMT.gmt(cmd)
		D = isa(R, Vector) ? R[1].data : R.data
		size(D, 1) == 0 && error("rotconverter returned nothing for age $a")
		push!(out, (D[1, 1], D[1, 2], D[1, 3], a))
	end
	isempty(out) && error("every age falls outside the model's range")

	# Same four columns, same widths as Mirone's cumpute_interp writes (_ffmt: no Printf dependency).
	txt = IOBuffer()
	println(txt, "#longitude\tlatitude\tangle(deg)\tage(Ma)")
	for p in out
		println(txt, lpad(_ffmt(p[1], 5), 9), '\t', lpad(_ffmt(p[2], 5), 9), '\t',
		             lpad(_ffmt(p[3], 4), 7), '\t', lpad(_ffmt(p[4], 4), 8))
	end
	blob = String(take!(txt))
	outfile = _get(d, "outfile")
	isempty(outfile) || write(outfile, blob)
	_euler_result(blob)
	return Cint(1)
end

# ---------------------------------------------------------------------------------------------
# The Euler poles a line carries in its OWN header. A Mirone isochron's segment header is
#   > 5 EURASIA/NORTH AMERICA FIN"136.53 63.63 -3.951 16.37" STG0"147.63 51.43 19.60 16.37 0.3362"
# — the same `KEY"v1 v2 …"` syntax `_isoc_parse_header` (isocs.jl) already reads, so THAT parser is
# what runs here; there is no second one. Reported as catalogue lines ("lon lat ang age !LABEL"), so
# the Poles selector can list them above lista_polos.dat exactly as Mirone's push_polesList_CB does.
#   FIN  = lon lat ang age                     (a finite pole, verbatim)
#   STG* = lon lat tstart tend ang [residue]   (a stage pole; offered as lon lat ANG TEND, Mirone's
#          own choice in push_polesList_CB — the pole that took the line to its own age)
function _euler_header_poles(scene::Ptr{Cvoid}, d::Dict{String,String})::Cint
	targets = _euler_targets(d)
	isempty(targets) && error("no line selected")
	name = targets[1]
	n = ccall(_fn(:gmtvtk_vector_info_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
	          scene, name, C_NULL, Cint(0))
	if n <= 0
		_euler_result("")
		return Cint(1)                      # no header on this line is normal, not a failure
	end
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_vector_info_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
	      scene, name, buf, Cint(n + 1))
	seen = Set{String}()
	out  = String[]
	for ln in split(unsafe_string(pointer(buf)), '\n'; keepempty = false)
		occursin('"', ln) || continue
		_, attrs = _isoc_parse_header(String(ln))
		for (key, v) in attrs
			local lon, lat, ang, age
			if startswith(key, "FIN") && length(v) >= 4
				lon, lat, ang, age = v[1], v[2], v[3], v[4]
			elseif startswith(key, "STG") && length(v) >= 5
				lon, lat, ang, age = v[1], v[2], v[5], v[4]
			else
				continue
			end
			line = join((_ffmt(lon, 2), _ffmt(lat, 2), _ffmt(ang, 4), _ffmt(age, 2)), "  ") *
			       "  !$key - from this line's header"
			line in seen && continue
			push!(seen, line);  push!(out, line)
		end
	end
	_euler_result(join(out, '\n'))
	return Cint(1)
end

# ---------------------------------------------------------------------------------------------
# Finite poles -> a stage-pole FILE, and that file becomes the rotation model (Mirone's choosebox
# stage path — `finite2stages(finite, half, side)`, whose own file-writing branch was commented out).
# GMT does the conversion:
#   half     -> rotconverter -M0.5   (HALF=2: half opening angles, the single-plate flow-line case)
#   side     -> -N (northern hemisphere) / -S (southern) / nothing (positive angles)
#   inverse  -> a_STAGE_b instead of b_STAGE_a: the REVERSE rotation, i.e. the same pole with the
#               opposite opening angle (a definition, not re-derived rotation maths)
#   geodetic -> the picked poles' latitudes are geodetic: mapproject -Ng before converting, the same
#               conversion the rotations themselves use (choosebox did this unconditionally; here it
#               follows the dialog's Geodetic Lats checkbox, so ONE control governs it everywhere)
function _euler_stages(d::Dict{String,String})::Cint
	rows = Vector{Float64}[]
	for rec in split(_get(d, "poles"), ';'; keepempty = false)
		c = split(strip(rec), r"\s+"; keepempty = false)
		length(c) < 4 && continue
		push!(rows, [parse(Float64, c[1]), parse(Float64, c[2]), parse(Float64, c[3]), parse(Float64, c[4])])
	end
	length(rows) < 2 && error("need at least two finite poles to build stages")
	P = _euler_poles_gmt_order(reduce(vcat, (permutedims(r) for r in rows)))
	P = P[sortperm(P[:, 3]), :]
	if _on(d, "geodetic")
		P[:, 1:2] = _euler_lat_convert(Matrix{Float64}(P[:, 1:2]), false)
	end

	opt = "-Fs"
	_on(d, "half") && (opt *= " -M0.5")
	side = parse(Int, _get(d, "side", "1"))
	side ==  1 && (opt *= " -N")
	side == -1 && (opt *= " -S")

	tmp = joinpath(tempdir(), "igmt_finite_$(time_ns()).rot")
	S = try
		open(tmp, "w") do io
			for r in eachrow(P);  println(io, join(r, '\t'));  end
		end
		R = GMT.gmt("rotconverter $tmp $opt")
		isa(R, Vector) ? R[1].data : R.data
	finally
		isfile(tmp) && rm(tmp, force = true)
	end
	size(S, 1) == 0 && error("rotconverter returned no stages")
	S = copy(S)
	_on(d, "inverse") && (S[:, 5] .= -S[:, 5])          # a_STAGE_b: the reverse rotation

	if _on(d, "showcmd")
		_euler_result("rotconverter <picked poles> $opt")
		return Cint(1)
	end

	# GMT's own 5-column stage format (lon lat tstart tend angle) — the very thing backtracker's -E
	# reads, so the file can be handed straight back to the Do Rotations tab.
	path = joinpath(tempdir(), "igmt_stages_$(time_ns()).stg")
	txt = IOBuffer()
	println(txt, "#longitude\tlatitude\ttstart(Ma)\ttend(Ma)\tangle(deg)")
	for r in eachrow(S)
		println(txt, lpad(_ffmt(r[1], 5), 9), '\t', lpad(_ffmt(r[2], 5), 9), '\t',
		             lpad(_ffmt(r[3], 4), 8), '\t', lpad(_ffmt(r[4], 4), 8), '\t', lpad(_ffmt(r[5], 4), 7))
	end
	table = String(take!(txt))
	write(path, table)
	_euler_result(path * "\n" * table)
	return Cint(1)
end

# Plate calculator — port of Mirone's src_figs/plate_calculator.m. Same seven models, same three
# answers (relative Euler pole, speed, azimuth), with GMT doing all the sphere work:
#
#   relative pole   gmtvector  (the poles of a model are ANGULAR VELOCITY vectors, so the motion of
#                   plate b relative to plate a is the vector difference w_b - w_a — Mirone's
#                   calculate_pole. gmtvector converts to and from Cartesian; only the scalar scaling
#                   by the rate and the subtraction are arithmetic, and rotconverter is deliberately
#                   NOT used: it composes FINITE rotations, which is not the same operation.)
#   speed/azimuth   gmtpmodeler -E<lon>/<lat>/<rate> -T1 -Sar   (a single rotation is taken as one
#                   1-Ma stage, so the opening angle IS the rate in deg/Ma; -Sa is the plate motion
#                   azimuth and -Sr the rate in mm/yr. It converts the point's geodetic latitude to
#                   geocentric itself — verified to reproduce Mirone's own ellipsoid conversion to
#                   four decimals — so the point goes in as the plain map coordinate.)

const _PLATE_MODELS = Dict{String,String}(
	"Nuvel1A" => "Nuvel1A_poles.dat",
	"MORVEL"  => "MORVEL_poles.dat",
	"PB"      => "PB_poles.dat",
	"GEODVEL" => "GEODVEL_poles.dat",
	"NNR"     => "Nuvel1A_NNR_poles.dat",
	"DEOS2K"  => "DEOS2K_poles.dat",
	"REVEL"   => "REVEL_poles.dat",
)

# One row of a model's poles table: "AB Name lat lon rate" (Mirone's own column order).
struct PlatePole
	abbrev::String
	name::String
	lat::Float64
	lon::Float64
	rate::Float64
end

const _PLATE_CACHE = Dict{String,Vector{PlatePole}}()

function _plate_poles(model::AbstractString)::Vector{PlatePole}
	key = String(model)
	haskey(_PLATE_CACHE, key) && return _PLATE_CACHE[key]
	file = get(_PLATE_MODELS, key, "")
	isempty(file) && error("unknown plate motion model \"$model\"")
	path = joinpath(_PKGROOT, "data", "plates", file)
	isfile(path) || error("plate poles file not found: $path")
	out = PlatePole[]
	for ln in eachline(path)
		t = strip(ln)
		(isempty(t) || startswith(t, '#')) && continue
		c = split(t, r"\s+"; keepempty = false)
		length(c) < 5 && continue
		lat  = tryparse(Float64, c[3]);  lon = tryparse(Float64, c[4])
		rate = tryparse(Float64, c[5])
		(lat === nothing || lon === nothing || rate === nothing) && continue
		push!(out, PlatePole(String(c[1]), String(c[2]), lat, lon, rate))
	end
	isempty(out) && error("no plates read from $path")
	_PLATE_CACHE[key] = out
	return out
end

_plate_find(v::Vector{PlatePole}, abbrev::AbstractString) =
	(i = findfirst(p -> p.abbrev == abbrev, v); i === nothing ? 0 : i)

# The angular velocity vectors of the given plates — one gmtvector call for the whole batch.
function _plate_cart(v::Vector{PlatePole}, idx::Vector{Int})::Matrix{Float64}
	m = Matrix{Float64}(undef, length(idx), 2)
	for (k, i) in enumerate(idx);  m[k, 1] = v[i].lon;  m[k, 2] = v[i].lat;  end
	R = GMT.gmt("gmtvector -Co -fg", m)
	D = isa(R, Vector) ? vcat([r.data for r in R]...) : R.data
	out = Matrix{Float64}(D[:, 1:3])
	for (k, i) in enumerate(idx);  out[k, :] .*= v[i].rate;  end
	return out
end

# Cartesian angular velocity vector -> (lon, lat, rate).
function _plate_spherical(w::Vector{Float64})::Tuple{Float64,Float64,Float64}
	rate = sqrt(sum(w .^ 2))
	rate < 1e-12 && return (0.0, 0.0, 0.0)
	R = GMT.gmt("gmtvector -A$(w[1])/$(w[2])/$(w[3]) -Ci -fg")
	D = isa(R, Vector) ? R[1].data : R.data
	return (D[1, 1], D[1, 2], rate)
end

# op=plates — the plate list of a model, one "AB<tab>Name" line per plate, in file order.
function _plate_list(d::Dict{String,String})::Cint
	v = _plate_poles(_get(d, "model", "Nuvel1A"))
	_euler_result(join(("$(p.abbrev)\t$(p.name)" for p in v), '\n'))
	return Cint(1)
end

# op=platepole — the Euler pole of `mov` relative to `fix`. An EMPTY `fix` means an absolute model:
# the plate's own pole IS the answer (Mirone's absolute_motion branch). Answers "" when there is no
# motion (the same plate on both sides), which is how the dialog knows to clear its boxes.
function _plate_pole(d::Dict{String,String})::Cint
	model = _get(d, "model", "Nuvel1A")
	v  = _plate_poles(model)
	im = _plate_find(v, _get(d, "mov"))
	im == 0 && error("no pole for plate \"$(_get(d, "mov"))\" in $model")
	local lon::Float64, lat::Float64, rate::Float64
	fixab = _get(d, "fix")
	if isempty(fixab)
		lon, lat, rate = v[im].lon, v[im].lat, v[im].rate
	else
		fi = _plate_find(v, fixab)
		fi == 0 && error("no pole for plate \"$fixab\" in $model")
		W = _plate_cart(v, [fi, im])
		lon, lat, rate = _plate_spherical(vec(W[2, :]) .- vec(W[1, :]))
	end
	if rate < 1e-12
		_euler_result("")
		return Cint(1)
	end
	# Two decimals on the pole, four on the rate — Mirone's own widths, and what the boxes show IS
	# what Calculate then uses, so the printed pole and the printed velocity always agree.
	_euler_result(join((_ffmt(lon, 2), _ffmt(lat, 2), _ffmt(rate, 4)), ' '))
	return Cint(1)
end

# op=platevel — speed (mm/yr) and azimuth (degrees cw from N) of the plate at one point.
function _plate_velocity(d::Dict{String,String})::Cint
	g(k) = (v = _euler_num(d, k); v === nothing ? error("Euler pole parameters are wrong") : v)
	plon, plat, rate = g("polelon"), g("polelat"), g("polerate")
	alon = _euler_num(d, "lon");  alat = _euler_num(d, "lat")
	(alon === nothing || alat === nothing) && error("calculate the velocity where?")
	cmd = "gmtpmodeler -E$plon/$plat/$rate -T1 -Sar -fg"
	if _on(d, "showcmd")
		_euler_result(cmd)
		return Cint(1)
	end
	R = GMT.gmt(cmd, [alon alat])
	D = isa(R, Vector) ? R[1].data : R.data
	size(D, 1) == 0 && error("gmtpmodeler returned nothing")
	azim = D[1, 4];  azim < 0 && (azim += 360.0)     # always report in 0-360, like Mirone
	_euler_result(join((_ffmt(D[1, 5], 1), _ffmt(azim, 1)), ' '))
	return Cint(1)
end

# ---------------------------------------------------------------------------------------------
# C callback (Compute / Show GMT command): `cparams` is the newline-separated "key=value" block
# described in 30_app.cpp's JuliaEulerFn. Returns Cint 1 on success, 0 on failure.
function _on_euler(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	# The Plates menu's two dialogs share this one door, so the failure message names the right tool.
	tool = "Euler rotations"
	try
		d  = _nswing_parse(unsafe_string(cparams))
		op = _get(d, "op")
		startswith(op, "plate") && (tool = "Plate calculator")
		op == "rotate"      && return _euler_rotate(scene, d)
		op == "add"         && return _euler_add(d)
		op == "interp"      && return _euler_interp(d)
		op == "headerpoles" && return _euler_header_poles(scene, d)
		op == "stages"      && return _euler_stages(d)
		op == "plates"      && return _plate_list(d)
		op == "platepole"   && return _plate_pole(d)
		op == "platevel"    && return _plate_velocity(d)
		error("unknown Plates operation \"$op\"")
	catch e
		msg = sprint(showerror, e)
		_viewer_log_error(scene, "$tool FAILED: $msg")
		_euler_result("$tool failed: $msg")
		@warn "$tool FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_euler()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_euler, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_euler_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
