# grdrotater.jl — GMT menu > "grdrotater": reconstruct the window's grid by an Euler rotation (the
# spotter supplement), through GMT.jl's own `grdrotater`. The grid must be geographic — the module
# rotates lon/lat about a pole on the sphere.
#
# The C++ dialog is GrdRotaterDialog (70_window.cpp, loads deps/ui/grdrotater_dialog.ui).
#
# Two things come out of a run: the rotated GRID and the rotated OUTLINE of the area that was
# rotated. The outline is what tells you where the grid went, so when it is asked for it is also
# drawn on the map as its own line layer — replaced on every re-run, since the outline of 10 Ma and
# the outline of 20 Ma are the same thing at two times, not two things.

const _GROT_OUTLINE_LAYER = "Rotated outline"

# -E in the three ways the module takes it, plus the +i that inverts any of them.
function _grot_E(d::Dict{String,String})::String
	mode = _get(d, "emode", "pole")
	s = if mode == "pole"
		lon, lat, ang = _get(d, "elon"), _get(d, "elat"), _get(d, "eangle")
		(isempty(lon) || isempty(lat) || isempty(ang)) &&
			error("a rotation pole needs its longitude, latitude and opening angle")
		for (v, what) in ((lon, "pole longitude"), (lat, "pole latitude"), (ang, "opening angle"))
			(tryparse(Float64, v) === nothing) && error("the $what must be a number, not '$v'")
		end
		lon * "/" * lat * "/" * ang
	elseif mode == "file"
		f = _get(d, "efile")
		isempty(f) && error("choose the rotation file")
		isfile(f) || error("rotation file not found: $f")
		f
	elseif mode == "plates"
		p = _get(d, "eplates")
		occursin(r"^[A-Za-z0-9]+-[A-Za-z0-9]+$", p) ||
			error("a plate pair looks like PAC-MBL (two GPlates IDs joined by a hyphen), not '$p'")
		p
	else
		error("unknown rotation mode '$mode'")
	end
	return _on(d, "invert") ? s * "+i" : s
end

# What came back: the rotated grid, the rotated outline, or both (GMT.jl hands over a tuple when the
# outline was asked for). Pick by KIND, never by position.
function _grot_pick(R, want::Type)
	R === nothing && return nothing
	isa(R, want) && return R
	if isa(R, Tuple) || isa(R, Vector)
		for r in R
			isa(r, want) && return r
		end
		(want === GMTdataset && !isempty(R) && isa(R[1], GMTdataset)) && return R
	end
	return nothing
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdRotaterFn. Returns Cint 1 on success, 0 on failure.
function _on_grdrotater(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The grid rotated is the one ON DISPLAY (the dialog sends the active layer's label).
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		kw = Dict{Symbol,Any}(:rotation => _grot_E(d))
		# The outline is the only product of an outline-only run, so that choice implies asking for it.
		outline_only = _on(d, "outlineonly")
		want_outline = outline_only || _on(d, "outline")
		outline_only && (kw[:rot_outline_only] = true)
		want_outline && (kw[:rot_outline] = true)     # GMT.jl passes -N when this is absent

		pf = _get(d, "polyfile")
		if !isempty(pf)
			isfile(pf) || error("polygon file not found: $pf")
			kw[:rot_polyg] = pf                        # -F: rotate only what is inside this polygon
		end
		t = _get(d, "time")
		if !isempty(t)
			# One time per run: with a RANGE the module writes one file per reconstruction time (its
			# -G needs a C-format specifier), which is a file-naming job this dialog does not do.
			(tryparse(Float64, t) === nothing) &&
				error("give ONE reconstruction time in Ma (a range writes one file per time, which this dialog does not do)")
			kw[:T] = t
		end
		reg = _get(d, "region")
		(isempty(reg) || occursin("//", reg)) || (kw[:A] = reg)   # -A: crop/extend the rotated region

		R = GMT.grdrotater(G; kw...)
		Grot = outline_only ? nothing : _grot_pick(R, GMTgrid)
		Drot = want_outline ? _grot_pick(R, GMTdataset) : nothing
		(Grot === nothing && Drot === nothing) && error("grdrotater returned nothing usable")

		# The outline first: it is what says WHERE the grid went, and it is also what an outline-only
		# run is for. REPLACE, never pile up.
		if Drot !== nothing
			out2 = _get(d, "outoutline")
			isempty(out2) || GMT.gmtwrite(String(out2), Drot)
			if _on(d, "drawoutline")
				ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring),
				      scene, _GROT_OUTLINE_LAYER)
				_add_geo_overlay(scene, Drot; color = (0.85, 0.2, 0.0), linewidth = 2.0,
				                 name = _GROT_OUTLINE_LAYER) ||
					error("could not draw the rotated outline in this window")
			end
		end

		if Grot === nothing
			outline_only || error("grdrotater returned no grid")
			return Cint(1)                             # outline-only run: the outline WAS the answer
		end
		isa(Grot, GMTgrid) || error("got a $(typeof(Grot)), not a grid")
		title = isempty(_get(d, "time")) ? "Rotated" : "Rotated to $(_get(d, "time")) Ma"
		return _gm3d_deliver(scene, Grot, title, _get(d, "outfile"), false,
		                     "grdrotater " * join(("$k=$v" for (k, v) in kw), ' ');
		                     geographic = true)        # the module only ever works in lon/lat
	catch e
		_tool_failed(scene, "grdrotater", e)
		return Cint(0)
	end
end

function _register_grdrotater()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdrotater, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdrotater_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
