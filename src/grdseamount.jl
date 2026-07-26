# grdseamount.jl — GMT menu > "grdseamount": build synthetic seamounts (Gaussian, parabolic,
# polynomial, cone or disc; circular or elliptical) from a table of seamount parameters.
#
# The C++ dialog is GrdSeamountDialog (70_window.cpp, loads deps/ui/grdseamount_dialog.ui).
#
# The keyword names below come from GMT.jl's own wrapper (GMT/src/potential/grdseamount.jl), NOT from
# the module's .qmd page: that page carries a "manual translate, needs revision" warning and its
# names disagree with the wrapper on nearly every option — shape/shapefun, elliptical/elliptic,
# normalize/norm, build_mode/bmode, time/timeinc, list_stats/list, list/listfiles, densities/rhofun,
# density_grid/rhomodel, density_output/averho. The wrapper is what actually runs.

# The dialog's typed single seamount arrives as "lon/lat/[azimuth/semimajor/semiminor|radius]/height".
_seamount_record(s::AbstractString) = GMT.mat2ds(reshape(parse.(Float64, split(s, '/')), 1, :))

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdSeamountFn. Absent key = don't pass that option.
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_grdseamount(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))

		tbl = _get(d, "table")
		D = if !isempty(tbl)
			isfile(tbl) || error("seamount table not found: $tbl")
			GMT.gmtread(String(tbl))
		else
			rec = _get(d, "record")
			isempty(rec) && error("no seamount given")
			_seamount_record(rec)
		end

		kw = Dict{Symbol,Any}()
		kw[:region] = _get(d, "region")
		kw[:inc]    = _get(d, "inc")
		# -C. The wrapper spells the flat-topped one "disk"; the module's own docs say "disc".
		shp = _get(d, "shape", "gaussian")
		kw[:shapefun] = Symbol(shp == "disc" ? "disk" : shp)
		_on(d, "elliptical") && (kw[:elliptic] = true)
		# -F with no value = read the flattening from the table's last column; with a value = that value.
		if _on(d, "flatcol")
			kw[:flattening] = true
		else
			v = _get(d, "flattening");  isempty(v) || (kw[:flattening] = parse(Float64, v))
		end
		u = _get(d, "unit");  isempty(u) || (kw[:unit] = Symbol(u))

		# -Z: a background depth, or NaN to flag the nodes no seamount touched.
		if _on(d, "levelnan")
			kw[:level] = NaN
		else
			v = _get(d, "level");  isempty(v) || (kw[:level] = parse(Float64, v))
		end
		v = _get(d, "normalize");  isempty(v) || (kw[:norm] = parse(Float64, v))

		# -A: a mask grid instead of relief, optionally with its own outside/inside values and a
		# radius scale factor.
		if _on(d, "mask")
			out, inn = _get(d, "maskout"), _get(d, "maskin")
			sc = _get(d, "maskscale")
			m = (isempty(out) && isempty(inn)) ? "" : (isempty(out) ? "1" : out) * "/" * (isempty(inn) ? "NaN" : inn)
			isempty(sc) || (m *= "+s" * sc)
			kw[:mask] = isempty(m) ? true : m
		end
		_on(d, "liststats") && (kw[:list] = true)      # -L: area/volume/mean height, no grid at all

		# -T (+ -Q, -M): one grid PER TIME STEP, written to the -G template rather than returned.
		tspec = _get(d, "time")
		if !isempty(tspec)
			kw[:timeinc] = tspec
			b = _get(d, "buildmode");  isempty(b) || (kw[:bmode] = b)
			l = _get(d, "list");       isempty(l) || (kw[:listfiles] = l)
		end

		# -H (+ -K, -W): the ad-hoc variable radial density model.
		dens = _get(d, "densities")
		if !isempty(dens)
			h = dens
			p = _get(d, "densify");    isempty(p) || (h *= "+d" * p)
			q = _get(d, "denspower");  isempty(q) || (h *= "+p" * q)
			kw[:rhofun] = h
			k = _get(d, "densitygrid");  isempty(k) || (kw[:rhomodel] = k)
			w = _get(d, "densityout");   isempty(w) || (kw[:averho] = w)
		end

		out = _get(d, "outgrid")
		# With -T or -L the module writes files / prints a table and returns no grid to show, so the
		# output name goes to GMT and there is nothing to add to the window.
		if !isempty(tspec) || _on(d, "liststats")
			isempty(out) || (kw[:save] = out)
			R = GMT.grdseamount(D; kw...)
			if _on(d, "liststats")
				R === nothing && error("grdseamount returned no statistics")
				show_table(scene, isa(R, Vector) ? R : [R]; name = "Seamount statistics")
			else
				_viewer_log_error(scene, "grdseamount: wrote the time-step grids to $out")
			end
			return Cint(1)
		end

		R = GMT.grdseamount(D; kw...)
		return _gm3d_deliver(scene, R, "Seamounts", out, false,
		                     "grdseamount " * join(("$k=$v" for (k, v) in kw), ' '))
	catch e
		_viewer_log_error(scene, "grdseamount FAILED: $(sprint(showerror, e))")
		@warn "grdseamount FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_grdseamount()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdseamount, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdseamount_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
