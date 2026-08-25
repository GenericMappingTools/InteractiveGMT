# gravprisms.jl — GMT menu > "gravprisms": the free-air anomaly, the geoid or the vertical gravity
# gradient of a body built out of vertically oriented RECTANGULAR PRISMS, through GMT.jl's own
# `gravprisms`.
#
# The C++ dialog is GravPrismsDialog (70_window.cpp, loads deps/ui/gravprisms_dialog.ui).
#
# The prisms come from one of two places: a TABLE that already lists them (x y z_low z_high [dx dy]
# [density]), or the module itself (-C), asked to approximate a seamount given by its heights (-S)
# or the layer between a base (-L) and a top (-T). Their density is either one number (or one grid)
# for all of them (-D) or the ad-hoc radial model of grdseamount (-H), which is what makes a prism
# a STACK of sub-prisms of different densities.
#
# Three things are shared with the talwani dialogs, because they are the same options: `_tal_F`
# (-F, the three field components and the geoid's reference latitude), `_tal_M` (-M, which distances
# are in km) and `_gmt_quote_path` (talwani2d.jl). The field NAMES come from there too.
#
# ONE THING TO KNOW ABOUT PATHS: GMT.jl's `gravprisms` wrapper builds -D, -S, -L, -T, -Z, -N and -W
# by CONCATENATING the value onto the option letter, so those paths end up inside GMT's command
# string. They are therefore double-quoted here (`_gmt_quote_path`), which is what makes a path with
# a space in it survive GMT_Create_Options. The prism TABLE is not among them: it is handed to the
# wrapper as its first argument and read on the Julia side.
#
# The output file is NOT passed as -G either. Like every other dialog here, the grid comes back into
# the window and `_gm3d_deliver` writes it — one place that knows how a result is saved.

# The seven columns the module writes when it is asked to save the prisms it built (+w).
const _GPR_PRISM_COLS = ["x", "y", "z low", "z high", "dx", "dy", "Density"]

# -D: one density contrast for every prism. A number is a number; anything else is the name of a
# grid of vertically-averaged densities — the same reading GMT does. Returns the option value and
# whether it turned out to be a grid, because -H cannot live with the grid form.
function _gpr_D(d::Dict{String,String})
	v = _get(d, "density")
	isempty(v) && return "", false
	isgrid = tryparse(Float64, v) === nothing
	isgrid && !isfile(v) && error("density grid not found: $v")
	s = isgrid ? _gmt_quote_path(v) : v
	# +c SUBTRACTS this density from each prism's own instead of replacing it — the seawater case.
	_on(d, "contrast") && (s *= "+c")
	return s, isgrid
end

# -H<H>/<rho_l>/<rho_h>[+b<boost>][+d<densify>][+p<power>]: the ad-hoc radial density model, the one
# grdseamount documents. Every piece is checked here because a malformed -H is accepted by nobody
# and reported by GMT as a parse error with no hint of which of the three numbers was wrong.
function _gpr_H(d::Dict{String,String})::String
	_on(d, "radial") || return ""
	H, lo, hi = _get(d, "href"), _get(d, "rholo"), _get(d, "rhohi")
	(isempty(H) || isempty(lo) || isempty(hi)) &&
		error("the radial density model needs its reference height and its low and high densities")
	for (v, what) in ((H, "reference height"), (lo, "low density"), (hi, "high density"))
		(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
	end
	s = H * "/" * lo * "/" * hi
	for (key, mod, what) in (("boost", "+b", "height boost"), ("densify", "+d", "densify gradient"),
	                         ("power", "+p", "profile exponent"))
		v = _get(d, key)
		isempty(v) && continue
		(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
		s *= mod * v
	end
	return s
end

# -C's modifiers: +z<dz> (the sub-prism height), +w<file> (save the prisms that were built) and +q
# (stop right after that). The module's own three rules about them are enforced here in its words.
function _gpr_C(d::Dict{String,String}, haveH::Bool)::String
	s = ""
	dz = _get(d, "dz")
	if !isempty(dz)
		haveH || error("a sub-prism height only means something with the radial density model")
		(tryparse(Float64, dz) === nothing || parse(Float64, dz) <= 0) &&
			error("the sub-prism height must be a positive number, not '$dz'")
		s *= "+z" * dz
	elseif haveH
		error("the radial density model needs a positive sub-prism height — it stacks the prisms")
	end
	f = _get(d, "saveprisms")
	isempty(f) || (s *= "+w" * _gmt_quote_path(f))
	if _on(d, "quit")
		isempty(f) && error("stopping after saving the prisms needs a file to save them to")
		s *= "+q"
	end
	return s
end

# What the module writes at the track locations: the point, the level it used, and the anomaly.
function _gpr_colnames(field::AbstractString, geog::Bool, ncol::Int)::Vector{String}
	ncol <= 0 && return String[]
	names = [geog ? "lon" : "x", geog ? "lat" : "y", "Observation level", _tal_fieldlabel(field)]
	ncol <= 4 && return names[1:ncol]
	append!(names, ["column $k" for k in 5:ncol])
	return names
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGravPrismsFn. Returns Cint 1 on success, 0 on failure.
function _on_gravprisms(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	tmpG = ""
	try
		d = _nswing_parse(unsafe_string(cparams))
		source = _get(d, "source", "table")
		(source in ("table", "create")) || error("unknown prism source '$source'")
		mode = _get(d, "mode", "grid")
		(mode in ("grid", "track", "obsgrid")) || error("unknown output mode '$mode'")
		field = _get(d, "field", "f")
		geog  = _on(d, "geog")

		kw = Dict{Symbol,Any}(:component => _tal_F(d))
		_on(d, "zup") && (kw[:zup] = true)
		M = _tal_M(d);  isempty(M) || (kw[:units] = M)
		geog && (kw[:f] = :g)

		Dstr, dens_is_grid = _gpr_D(d)
		H = _gpr_H(d)
		isempty(Dstr) || (kw[:density] = Dstr)
		isempty(H)    || (kw[:radial_rho] = H)
		if !isempty(H)
			# The module's own two refusals about mixing -H with -D, in its own words.
			dens_is_grid && error("the radial density model cannot be combined with a density GRID")
			(!isempty(Dstr) && !_on(d, "contrast")) &&
				error("with the radial density model a fixed density must be SUBTRACTED (+c), not substituted")
		end

		infile = ""
		quitting = false
		if source == "create"
			(isempty(Dstr) && isempty(H)) &&
				error("creating prisms needs a density: a fixed one, a density grid, or the radial model")
			shape = _get(d, "shape")
			if !isempty(shape)
				isfile(shape) || error("heights grid not found: $shape")
				kw[:topography] = _gmt_quote_path(shape)
			end
			# -L and -T each take either a grid or a plain z-level, and the module says so.
			base, top = _get(d, "base"), _get(d, "top")
			for (v, sym, what) in ((base, :base, "base"), (top, :top, "top"))
				isempty(v) && continue
				if tryparse(Float64, v) === nothing
					isfile(v) || error("$what surface grid not found: $v")
					kw[sym] = _gmt_quote_path(v)
				else
					kw[sym] = v
				end
			end
			(isempty(shape) && isempty(top)) &&
				error("give the grid of heights, or the top of the layer to approximate")
			(!isempty(H) && isempty(shape)) &&
				error("the radial density model needs the grid of heights to know the seamount's full height")
			C = _gpr_C(d, !isempty(H))
			kw[:prisms] = isempty(C) ? true : C
			quitting = _on(d, "quit")
		else
			infile = _get(d, "infile")
			isempty(infile) && error("no prism table")
			isfile(infile) || error("prism table not found: $infile")
			e = _get(d, "dxdy")
			if !isempty(e)
				all(x -> tryparse(Float64, x) !== nothing, split(e, '/')) ||
					error("the prism size is dx or dx/dy, not '$e'")
				kw[:dxdy] = e
			end
		end

		W = _get(d, "avedens")
		if !isempty(W)
			# -W holds the densities the radial model worked out for the prisms it created; without
			# both of those there is nothing for it to hold.
			(source == "create" && !isempty(H)) ||
				error("the mean-density grid is only produced when the prisms are CREATED with the radial density model")
			kw[:avedens] = _gmt_quote_path(W)
		end

		# WHERE to evaluate — but a +q run evaluates nothing, so it is not asked.
		if !quitting
			if mode == "grid"
				reg = _get(d, "region")
				(isempty(reg) || occursin("//", reg)) &&
					error("give the full region (xmin, xmax, ymin, ymax) to compute over")
				inc = _get(d, "inc")
				isempty(inc) && error("give the grid increment")
				kw[:region] = reg
				kw[:inc] = inc
				_on(d, "pixel") && (kw[:registration] = :pixel)
			elseif mode == "track"
				trk = _get(d, "trackfile")
				isempty(trk) && error("choose the file with the output locations")
				_tal_check_track(trk, !isempty(_get(d, "level")))   # THE track check (talwani2d.jl)
				kw[:track] = _gmt_quote_path(trk)
			else
				zg = _get(d, "zgrid")
				isempty(zg) && error("choose the grid of observation levels")
				isfile(zg) || error("observation-level grid not found: $zg")
				kw[:level] = _gmt_quote_path(zg)      # -Z<grid>: it sets the output region as well
			end
			if mode != "obsgrid"
				lvl = _get(d, "level")
				if !isempty(lvl)
					(tryparse(Float64, lvl) === nothing) &&
						error("the observation level must be a number, not '$lvl'")
					kw[:level] = lvl
				end
			end
		else
			# GMT.jl's wrapper appends a bare -G whenever -N is absent, and a bare -G asks the API to
			# hand a grid back through memory. With +q the module returns BEFORE making any grid, so
			# that memory would be read back empty. Giving -G a real name — one this run never gets
			# far enough to write — stops the API from registering an output at all.
			tmpG = tempname() * ".grd"
			kw[:save] = _gmt_quote_path(tmpG)   # a temp dir can sit under a user name with a space
		end

		R = isempty(infile) ? GMT.gravprisms(; kw...) : GMT.gravprisms(String(infile); kw...)

		if quitting
			# The prisms WERE the answer. Show them, in the module's own seven columns.
			pf = _get(d, "saveprisms")
			isfile(pf) || error("gravprisms wrote no prism table")
			P = GMT.gmtread(String(pf); dataset = true)
			P === nothing && error("could not read back the prisms that were written")
			Ps = isa(P, Vector) ? P : [P]
			isempty(Ps) && error("the prism table came back empty")
			for p in Ps
				n = min(size(p.data, 2), length(_GPR_PRISM_COLS))
				p.colnames = _GPR_PRISM_COLS[1:n]
			end
			show_table(scene, length(Ps) == 1 ? Ps[1] : Ps; name = "Prisms")
			return Cint(1)
		end

		R === nothing && error("gravprisms returned nothing")
		out = _get(d, "outfile")
		label = _tal_fieldname(field)
		if mode == "track"
			Ds = isa(R, Vector) ? R : [R]
			all(x -> isa(x, GMTdataset), Ds) || error("got a $(typeof(R)), not a table")
			for Dd in Ds
				Dd.colnames = _gpr_colnames(field, geog, size(Dd.data, 2))
			end
			isempty(out) || GMT.gmtwrite(String(out), R)
			show_table(scene, length(Ds) == 1 ? Ds[1] : Ds; name = "gravprisms (" * label * ")")
			# The track's x,y ARE map coordinates, so the values can sit on the map where they belong.
			if _on(d, "plotpts")
				for Dd in Ds
					m = Dd.data
					(size(m, 1) >= 1 && size(m, 2) >= 2) || continue
					names = Dd.colnames
					infos = [join(("$(names[c]) = $(m[k, c])" for c in 1:size(m, 2)), '\n')
					         for k in 1:size(m, 1)]
					add_symbols!(scene, view(m, :, 1), view(m, :, 2); symbol = :circle, size = 6,
					             fill = :cyan, edge = :black, edgewidth = 0.5,
					             name = "gravprisms (" * label * ")", info = infos)
				end
			end
			return Cint(1)
		end

		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")
		return _gm3d_deliver(scene, R, "gravprisms (" * label * ")", out, false,
		                     "gravprisms " * join(("$k=$v" for (k, v) in kw), ' ');
		                     geographic = geog ? true : nothing)
	catch e
		_viewer_log_error(scene, "gravprisms FAILED: $(sprint(showerror, e))")
		@warn "gravprisms FAILED" exception=(e,)
		return Cint(0)
	finally
		if !isempty(tmpG)
			try; rm(tmpG; force = true); catch; end
		end
	end
end

function _register_gravprisms()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_gravprisms, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gravprisms_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
