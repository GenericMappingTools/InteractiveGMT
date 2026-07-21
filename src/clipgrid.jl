# clipgrid.jl — Grid Tools > "Clip Grid" (port of Mirone's src_figs/ml_clip.m). Replace grid nodes
# below/above user thresholds (or the whole in-between band) with given values, or auto-derive those
# thresholds from the data statistics ("Statistical Hammering": % end-members, n·STD, n·MAD). The
# result is a NEW derived grid added to the SAME window (SACRED_LAW.md derived-variable display law),
# exactly like rtp3d.jl. The C++ dialog (ClipGridDialog, 70_window.cpp) loads deps/ui/clipp_grid.ui
# at runtime and drives everything through the two callbacks below.

# Parse a Value edit-box: empty -> nothing (that side is NOT clipped), "NaN"/"nan" -> NaN (a valid
# replacement — sets those nodes to NaN, exactly like ml_clip.m accepts the literal 'NaN'), else the
# number.
function _clip_parse_val(s::AbstractString)
	t = strip(s)
	isempty(t) && return nothing
	lowercase(t) == "nan" && return NaN
	return parse(Float64, t)
end

# Parse a stat box: empty OR NaN -> nothing (not provided). ml_clip.m's push_okUP_CB skips a box
# whenever str2double gives NaN (which covers both the empty and the non-numeric case).
function _clip_parse_stat(s::AbstractString)
	t = strip(s)
	isempty(t) && return nothing
	x = tryparse(Float64, t)
	(x === nothing || isnan(x)) && return nothing
	return x
end

# Faithful port of ml_clip.m's push_OK_CB clipping. Mutates `z` in place. `below_val`/`above_val`
# are `nothing` when that side is left blank. Both masks are read from the ORIGINAL z (a node can't
# be > above AND < below, so they're disjoint) — this reproduces ml_clip.m's careful "special case"
# branch for every input, without the sequential double-clip its plain branch suffers when e.g.
# above_val < below. NaN nodes compare false against everything, so they are never touched (unless
# explicitly caught by a band that a value falls in — same as MATLAB).
function _clip_apply!(z::Matrix{Float32}, below::Float64, above::Float64,
                      below_val::Union{Nothing,Float64}, above_val::Union{Nothing,Float64},
                      in_between::Bool)
	if in_between
		below_val === nothing && error("Clip in between needs a Value (the replacement for the in-between band).")
		bv = Float32(below_val)
		@inbounds for i in eachindex(z)
			v = z[i]
			(v >= below && v <= above) && (z[i] = bv)
		end
	else
		(below_val === nothing && above_val === nothing) &&
			error("Nothing to do: give a Below Value and/or an Above Value.")
		av = above_val === nothing ? nothing : Float32(above_val)
		bv = below_val === nothing ? nothing : Float32(below_val)
		@inbounds for i in eachindex(z)
			v = z[i]
			if av !== nothing && v > above
				z[i] = av
			elseif bv !== nothing && v < below
				z[i] = bv
			end
		end
	end
	return z
end

# --- small NaN-aware statistics (kept in-house so Clip Grid pulls in no new dependency) ----------
function _clip_median(v::Vector{Float64})
	n = length(v)
	n == 0 && return NaN
	s = sort(v)
	isodd(n) ? s[(n + 1) ÷ 2] : 0.5 * (s[n ÷ 2] + s[n ÷ 2 + 1])
end

function _clip_mean_std(v::Vector{Float64})
	n = length(v)
	n == 0 && return (NaN, NaN)
	m = sum(v) / n
	n == 1 && return (m, 0.0)
	sd = sqrt(sum(x -> (x - m)^2, v) / (n - 1))
	return (m, sd)
end

# Port of ml_clip.m's push_okUP_CB: translate whichever of the three stat inputs is provided into a
# (low, up) clip pair. The three are applied in the .m's own order (% end-members, then n·STD, then
# n·MAD); a later one overrides an earlier one — but the C++ dialog keeps only one box filled at a
# time, so in practice exactly one runs. Returns nothing when none is provided.
function _clip_stats(zmat::AbstractMatrix, pct::Union{Nothing,Float64},
                     nstd::Union{Nothing,Float64}, nmad::Union{Nothing,Float64})
	fin = Float64[]
	@inbounds for v in zmat
		isnan(v) || push!(fin, Float64(v))
	end
	isempty(fin) && error("Grid has no finite values to compute statistics from.")

	low = nothing;  up = nothing
	if pct !== nothing
		xx = abs(pct) * 0.01
		s = sort(fin);  n = length(s)
		n_out = clamp(round(Int, n * xx / 2), 1, n)
		low = s[n_out];  up = s[clamp(n - n_out, 1, n)]      # ml_clip.m: s(n_out) / s(numel-n_out)
	end
	if nstd !== nothing
		xx = abs(nstd)
		m, sd = _clip_mean_std(fin)
		low = m - xx * sd;  up = m + xx * sd
	end
	if nmad !== nothing
		xx = abs(nmad)
		med = _clip_median(fin)
		mad = 1.4826 * _clip_median(abs.(fin .- med))
		low = med - xx * mad;  up = med + xx * mad
	end
	(low === nothing) && return nothing
	return (low, up)
end

# g_juliaEval round-trip for the dialog's "UP" button: prints "low/up" (or nothing when no stat box
# is filled). `params` = "pct;nstd;nmad". The grid is this window's loaded grid (resolved via _FIGREG,
# same "the window already holds it" convention rtp3d.jl uses).
function _clip_stats_str(scene::Ptr{Cvoid}, params::AbstractString)
	p = split(params, ';')
	pct  = length(p) >= 1 ? _clip_parse_stat(p[1]) : nothing
	nstd = length(p) >= 2 ? _clip_parse_stat(p[2]) : nothing
	nmad = length(p) >= 3 ? _clip_parse_stat(p[3]) : nothing
	fig = get(_FIGREG, scene, nothing)
	(fig isa QtFigure) || error("No grid loaded in this window")
	r = _clip_stats(fig.G.z, pct, nstd, nmad)
	r === nothing && return nothing
	print(r[1], "/", r[2])
	return nothing
end

# NaN-aware min/max for the clipped grid's refreshed header range.
function _clip_extrema(z::AbstractMatrix)
	mn = Inf;  mx = -Inf
	@inbounds for v in z
		isnan(v) && continue
		v < mn && (mn = v)
		v > mx && (mx = v)
	end
	return (isfinite(mn) ? Float64(mn) : 0.0, isfinite(mx) ? Float64(mx) : 1.0)
end

# C callback (Apply button): params = "below;above;belowVal;aboveVal;inBetween;stretch". Clips this
# window's grid and adds the result as a NEW derived grid. Returns 1 on success, 0 on failure — same
# contract as _on_rtp3d, and the add/promote + derived-variable-display tail is deliberately identical.
function _on_clipgrid(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		p = split(unsafe_string(cparams), ';')
		below = parse(Float64, p[1]);  above = parse(Float64, p[2])
		below_val = _clip_parse_val(p[3])
		above_val = _clip_parse_val(p[4])
		in_between = p[5] == "1"
		stretch    = p[6] == "1"

		fig = get(_FIGREG, scene, nothing)
		(fig isa QtFigure) || error("No grid loaded in this window")
		G = fig.G

		# Stretch histogram (Mirone scaleto8(Z,[below above])): a DISPLAY contrast remap, NOT a data
		# change. scaleto8 maps [below,above] linearly onto the colour range, clamping outside — so the
		# faithful iGMT equivalent is to recolour the EXISTING surface across [below,above] and leave
		# the grid untouched (making a "new grid" here was wrong: with Below/Above prefilled to the
		# grid's own min/max nothing lay outside them, so the clip was a no-op and the result equalled
		# the original). Uses the SAME primitive the colorbar chooser uses (gmtvtk_set_cpt via
		# _cpt_nodes_range), with the grid's current default palette so only the contrast changes.
		if stretch
			(above > below) || error("Stretch histogram needs Above > Below.")
			cmap = _default_cmap(G)
			cz, crgb, n = _cpt_nodes_range(below, above, cmap)
			n < 2 && error("Stretch: makecpt failed for colormap $cmap")
			ccall(_fn(:gmtvtk_set_cpt), Cvoid, (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint),
			      scene, cz, crgb, Cint(n))
			return Cint(1)
		end

		G2 = deepcopy(G)
		z  = Matrix{Float32}(G2.z)                         # independent, guaranteed Float32
		_clip_apply!(z, below, above, below_val, above_val, in_between)
		G2.z = z
		zmn, zmx = _clip_extrema(z)
		if length(G2.range) >= 6                            # refresh header z range (ml_clip.m head(5:6))
			G2.range[5] = zmn;  G2.range[6] = zmx
		end
		G2.hasnans = 0                                      # unknown -> let downstream recheck

		title = stretch ? "Stretched" : "Clipped"
		# REPLACE, never pile up: recomputing under the SAME title trashes the previous result (both
		# the C++ actor and the stale Julia reference) before adding the new one — same as _on_rtp3d.
		ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, title)
		_forget_object!(scene, :grid, title)

		_grid_command!(G2, "InteractiveGMT clip below=$below above=$above belowVal=$(below_val) " *
		                   "aboveVal=$(above_val) inBetween=$in_between stretch=$stretch")
		r = G2.range
		nx = size(z, 2);  ny = size(z, 1)
		geog = _isgeog(G2)
		cz, crgb, ncolor = _cpt_nodes(G2, :turbo)
		has_surface = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
		promote = (has_surface == 0)
		fn = promote ? :gmtvtk_promote_surface_h : :gmtvtk_add_surface_h
		ok = promote ?
			ccall(_fn(fn), Cint,
			  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
			   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			  scene, G2.z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(geog),
			  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(title)) :
			ccall(_fn(fn), Cint,
			  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
			   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			  scene, G2.z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4],
			  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(title))
		if ok == 0
			_viewer_log_error(scene, "Clip Grid: window closed, grid not added")
			return Cint(0)
		end
		_remember_object!(scene, :grid, title, G2)
		# SACRED_LAW.md derived-variable display law (same tail as _on_rtp3d): the clipped grid starts
		# CHECKED, every OTHER grid in the window is UNCHECKED, and Scene Objects unfolds to reveal it.
		_show_object!(scene, title)
		_hide_other_objects!(scene, :grid, title)
		ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Clip Grid FAILED: $(sprint(showerror, e))")
		@warn "Clip Grid FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_clipgrid()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_clipgrid, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_clipgrid_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
