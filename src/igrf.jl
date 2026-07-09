# igrf.jl — Geophysics > Magnetics > IGRF (port of Mirone's igrf_options.m,
# src_figs/igrf_options.m). Both paths (single-point readout + the grid Compute button) go
# through GMT.jl's `magref` (mgd77magref wrapper) instead of Mirone's igrf_m MEX.
#
# Point compute: state = "lon/lat/elev_m/date_dec" -> "F/H/X/Y/Z/D/I" (nT x5, deg x2).
# magref with no :F kwarg and a >=3-column input defaults to "-Frthxyzdi/0" (echo input columns,
# then Total/Horizontal/X/Y/Z/Declination/Inclination in that order) — the LAST 7 output columns
# are always [F H X Y Z D I] regardless of how many input columns got echoed, so we just slice
# `end-6:end` rather than depend on the echo width.
#
# Grid compute: magref's OWN grid mode (no arg1, :R/:I kwargs) builds the mesh and calls
# xyz2grd for us — no need to hand-loop linspace/reshape like Mirone's push_ComputeGrid_CB.
# Grid mode wants exactly ONE field-component kwarg (:T/:H/:X/:Y/:Z/:dec/:inc); `elev_m` comes in
# via :alt (magref wants kilometres, same convention Mirone used: `elev/1000`).

const _IGRF_BUF = Ref{Vector{UInt8}}(UInt8[0])

# "IGRF Total field" combo/field-code -> (magref kwarg symbol, display name)
function _igrf_field(code::AbstractString)
	code == "H" ? (:H, "H component")   :
	code == "X" ? (:X, "X component")   :
	code == "Y" ? (:Y, "Y component")   :
	code == "Z" ? (:Z, "Z component")   :
	code == "D" ? (:dec, "Declination") :
	code == "I" ? (:inc, "Inclination") : (:T, "Total field")
end

# C callback: state = "lon/lat/elev_m/date_dec" -> "F/H/X/Y/Z/D/I", or "" on failure. The
# returned pointer is Julia-owned (module-global buffer, single UI thread -> no reentrancy); C++
# copies it immediately and never frees it (same convention as `_on_dimfun`/`_on_gridmeta`).
function _on_igrf_point(cstate::Cstring)::Cstring
	out = ""
	try
		p = split(unsafe_string(cstate), '/')
		lon, lat, elev_m, date = parse.(Float64, p[1:4])
		D = GMT.magref([lon lat (elev_m / 1000) date])
		Dd = D isa AbstractVector ? D[1] : D
		v = Dd.data[1, end-6:end]           # [F H X Y Z D I]
		out = join(round.(v; digits=3), '/')
	catch e
		@warn "IGRF point FAILED" exception=(e,)
	end
	_IGRF_BUF[] = Vector{UInt8}(codeunits(out * "\0"))
	return Cstring(pointer(_IGRF_BUF[]))
end

function _register_igrf_point()
	fptr = @cfunction((c) -> Base.invokelatest(_on_igrf_point, c), Cstring, (Cstring,))
	ccall(_fn(:gmtvtk_set_igrf_point_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# C callback: params = "W/E/S/N/xinc/yinc/elev_m/date_dec/fieldcode" (fieldcode one of
# T|H|X|Y|Z|D|I). Adds the grid to the dialog's parent window (the passed scene), with the
# fieldCombo selection as the grid name/title.
function _on_igrf_grid(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		p = split(unsafe_string(cparams), '/')
		W, E, S, N, xinc, yinc, elev_m, date = parse.(Float64, p[1:8])
		fieldcode = length(p) >= 9 ? String(p[9]) : "T"
		fieldsym, fieldname = _igrf_field(fieldcode)
		kw = Dict{Symbol,Any}(:R => "$W/$E/$S/$N", :I => "$xinc/$yinc",
		                       :alt => elev_m / 1000, :onetime => date, fieldsym => true)
		G = GMT.magref(; kw...)
		# Add to the existing scene (dialog's parent window), named after the field selection.
		z = eltype(G.z) === Float32 ? G.z : Float32.(G.z); ny, nx = size(z); r = G.range
		geog = _isgeog(G)
		cmap = :turbo
		cz, crgb, ncolor = _cpt_nodes(G, cmap)
		title = "IGRF $fieldname"
		# Use promote if the scene is an empty launcher (no surface yet), otherwise add as extra.
		has_surface = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
		promote = (has_surface == 0)
		fn = promote ? :gmtvtk_promote_surface_h : :gmtvtk_add_surface_h
		ok = promote ?
			ccall(_fn(fn), Cint,
			  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
			   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			  scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(geog),
			  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(title)) :
			ccall(_fn(fn), Cint,
			  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
			   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			  scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4],
			  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(title))
		ok == 0 && @warn "IGRF grid: window closed, grid not added"
		ok != 0 && _remember_object!(scene, :grid, title, G)
	catch e
		_viewer_log_error(scene, "IGRF grid FAILED: $(sprint(showerror, e))")
		@warn "IGRF grid FAILED" exception=(e,)
	end
	return
end

function _register_igrf_grid()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_igrf_grid, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_igrf_grid_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# C callback: params = "infile;outfile;nHeaders;elev_m;date_dec" (Input Mag File "Compute").
# MVP scope, matching the dialog's own help text that 2 columns (lon,lat) is the MINIMUM it
# needs: read the first two whitespace-separated numeric tokens of every non-header line, compute
# Total Field for all of them at the shared Elevation/Date box values, write "lon\tlat\tfield" to
# outfile. Mirone's fuller version (per-row elevation/date columns via an interactive column
# selector, optional anomaly column) is not ported — see docs/GRDSAMPLE_TODO.md for the project's
# convention of flagging a scoped-down port instead of overclaiming completeness.
function _on_igrf_file(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		p = split(unsafe_string(cparams), ';')
		infile  = String(p[1]); outfile = String(p[2])
		nskip   = (length(p) >= 3 && !isempty(p[3])) ? parse(Int, p[3]) : 0
		elev_m  = parse(Float64, p[4]); date = parse(Float64, p[5])
		lons = Float64[]; lats = Float64[]
		for (i, line) in enumerate(eachline(infile))
			i <= nskip && continue
			toks = split(strip(line))
			length(toks) < 2 && continue
			lon = tryparse(Float64, toks[1]); lat = tryparse(Float64, toks[2])
			(lon === nothing || lat === nothing) && continue
			push!(lons, lon); push!(lats, lat)
		end
		isempty(lons) && error("No numeric lon/lat rows found (check the header-line count)")
		D = GMT.magref([lons lats]; alt = elev_m / 1000, onetime = date, T = true)
		Dd = D isa AbstractVector ? D[1] : D
		f = Dd.data[:, end]                     # last column = Total Field
		open(outfile, "w") do io
			for k in eachindex(lons)
				println(io, "$(lons[k])\t$(lats[k])\t$(round(f[k]; digits=1))")
			end
		end
	catch e
		_viewer_log_error(scene, "IGRF file compute FAILED: $(sprint(showerror, e))")
		@warn "IGRF file compute FAILED" exception=(e,)
	end
	return
end

function _register_igrf_file()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_igrf_file, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_igrf_file_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
