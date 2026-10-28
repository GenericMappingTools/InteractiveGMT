# gmtscript.jl — reproduce what an iGMT window displays as GMT.jl calls.
#
# Two products, ONE emitter (docs/GMTSCRIPT_PLAN.md):
#   * `gmtreplay(fig)`  — run the calls right now, in this session, against the LIVE grid/image
#     objects. No files are written at all.
#   * `gmtscript(fig; path=…)` — the same call list rendered as a standalone `.jl` script, with any
#     in-memory layer materialized once into `<script>_data/`.
#
# The element inventory is NOT re-derived here: it is `_SESSION_LOG` (session.jl), the provenance
# registry Save Session already fills at every add site. Save Session and this file are two backends
# on that one inventory — SACRED_LAW.md, same operation = same function. A second walk of the Scene
# would drift away from the recipes exactly the way the twelve derived-variable sites drifted.
#
# GMT.jl is ALWAYS GMT: there is no 2-D emitter and no 3-D emitter. A tilted window contributes
# `view=`/`zsize=` KWARGS to the same calls, and the module for each layer follows what that element
# IS and whether it carries z (grdview for a relief surface, grdimage for a flat raster, plot/plot3
# for vectors) — never a global mode switch.
#
# P1 scope: rasters (base + dropped grids/images), region/projection/frame/colorbar, the view/zsize
# kwargs, and the T1→T2→T3 data ladder. Vector layers, faults, curtains and the C++-drawn elements
# are LISTED in the script header as not-yet-exported rather than silently dropped (P2/P3).

# ── the two things a rendered call is made of ────────────────────────────────────────────────
# A value that is a NAMED VARIABLE in the script and a live object in this session: `cmap=C1` in the
# text, the actual GMTcpt when the call is run here. Both sinks read the same kwarg list this way,
# so neither sink owns a translation table of its own.
struct ScriptVar
	name::Symbol
	value::Any
end

# How a layer's data reaches the script — one rung of the T1→T2→T3 ladder. `expr` is the text the
# script uses to obtain it. NOTHING is ever written to disk for it: a layer that exists only in memory
# is fetched from the LIVE WINDOW by the script (`:live`/`:capture`). There used to be a `sidecar`
# field here and a `script_data/` directory written beside the script — a temporary-file bridge, which
# is banned outright. Do not bring it back.
struct DataBind
	var::ScriptVar
	tier::Symbol            # :file | :command | :live | :capture | :cpt
	expr::String
	mk::Any                 # :cpt only — the makecpt kwargs, so the live sink builds the very same
	                        # palette by CALLING makecpt, never by eval'ing the text it rendered
end
DataBind(var, tier, expr) = DataBind(var, tier, expr, nothing)

# One GMT.jl call plus whatever has to be bound before it.
struct ScriptStep
	comment::String
	binds::Vector{DataBind}
	mod::Symbol                          # :grdimage, :grdview, :colorbar, …
	pos::Any                             # positional argument (a ScriptVar) or nothing
	kw::Vector{Pair{Symbol,Any}}
	bang::Bool                           # append to the current figure (`grdimage!`)
end

# Everything the emitter resolves once for the whole figure.
mutable struct ScriptCtx
	scene::Ptr{Cvoid}
	region::NTuple{4,Float64}
	geog::Bool
	proj::Any
	figsize::Float64
	view::Union{Nothing,NTuple{2,Float64}}
	zsize::Union{Nothing,Float64}
	recompute::Bool
	nvar::Dict{String,Int}               # per-prefix counters for generated variable names
	notes::Vector{String}                # "not exported" lines for the script header
	needs_base64::Bool                   # a displayed image is fetched -> the script needs InteractiveGMT
	ncurtain::Int                        # curtains emitted so far (their fetch key is this index)
	dpi::Float64                         # render-window DPI: the pt<->px factor for LINE WIDTHS
end

# Screen PIXELS -> the POINTS the element actually HAS. This program has TWO pt<->px relations and
# they are not interchangeable; using one for both is why a set thickness came back as a different
# number:
#
#  * SYMBOL SIZES — `_add_symbols!` (symbols.jl) stores `pt * 96/72`, a FIXED 96 dpi. So a layer made
#    with `sizeunit=:pt, size=8` must read back as exactly 8p.
#  * LINE WIDTHS — `gmtvtk_add_overlay_ex_h` (90_c_api.cpp) and the Line Properties dialog take
#    POINTS and store `pt * dpi/72` using the RENDER WINDOW's own DPI. A width set to 4 pt must read
#    back as 4p, which only happens if the same dpi divides it again.
#
# (An even earlier version scaled by plot width over window pixel width. That is not a relation this
# program has at all: it made the same layer a different size at every `figsize`.)
const _SCRIPT_SYMBOL_DPI = 96.0
_script_pt_sym(px)  = round(Float64(px) * 72 / _SCRIPT_SYMBOL_DPI; sigdigits=3)
_script_ptstr_sym(px) = string(_script_pt_sym(px), "p")
_script_pt_w(px, ctx::ScriptCtx)  = round(Float64(px) * 72 / ctx.dpi; sigdigits=3)
_script_ptstr_w(px, ctx::ScriptCtx) = string(_script_pt_w(px, ctx), "p")

# The window's OWN points->pixels factor for line widths, cached per window.
#
# Guessing it is not good enough and a hard-coded 96 is simply wrong: on this display a pen set to
# 4 pt through `gmtvtk_add_overlay_ex_h` lands as 9.0 px, i.e. 162 dpi, so a 96 assumption reported
# 6.75p for a width the user had set to 4. So the app is ASKED: the scene state's `dpi` when the build
# reports it, else measured by adding one throwaway overlay of a known POINT width, reading the pixel
# width back off its actor, and removing it again — the app converting for us, which cannot be wrong.
const _SCRIPT_DPI = Dict{Ptr{Cvoid},Float64}()
function _script_dpi(h::Ptr{Cvoid}, stf)::Float64
	d = Float64(get(stf, "dpi", 0))
	d > 0 && return d
	haskey(_SCRIPT_DPI, h) && return _SCRIPT_DPI[h]
	dpi = 96.0
	try
		probe_pt = 10.0
		probe = "__igmt_dpi_probe"
		xyz = Float64[0, 0, 0, 0, 0, 0]; segoff = Cint[0, 2]
		ok = ccall(_fn(:gmtvtk_add_overlay_ex_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
			 Cdouble, Cdouble, Cstring, Cstring, Cstring),
			h, xyz, Cint(2), segoff, Cint(1), Cint(1), 0.0, 0.0, 0.0, probe_pt, 0.0, probe, probe, "")
		if ok != 0
			out = zeros(Float64, 6)
			if ccall(_fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), h, probe, out) != 0
				out[4] > 0 && (dpi = out[4] / probe_pt * 72)
			end
			ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), h, probe)
		end
	catch
	end
	_SCRIPT_DPI[h] = dpi
	return dpi
end

# G1, G2, I1, C1, … — numbered PER PREFIX, so a script's grids and palettes each read 1, 2, 3.
function _script_var!(ctx::ScriptCtx, prefix::String, value)
	n = get(ctx.nvar, prefix, 0) + 1
	ctx.nvar[prefix] = n
	return ScriptVar(Symbol(prefix, n), value)
end

# ── literal rendering (text sink) ────────────────────────────────────────────────────────────
# Julia source text for a kwarg value. `repr` handles string escaping, so a Windows path with
# backslashes comes out as a literal that reads back identical.
_lit(v::ScriptVar)      = String(v.name)
_lit(s::String) = repr(s)
_lit(s::SubString{String}) = repr(String(s))
_lit(s::Symbol)         = ":" * String(s)
_lit(b::Bool)           = b ? "true" : "false"
_lit(x::Real)           = (isinteger(x) && abs(x) < 1e15) ? string(Int(round(x))) : string(Float64(x))
_lit(::Nothing)         = "nothing"
_lit(t::Tuple)          = length(t) == 1 ? "(" * _lit(t[1]) * ",)" : "(" * join(map(_lit, t), ", ") * ")"
function _lit(nt::NamedTuple)
	ks = keys(nt)
	body = join(["$(k)=$(_lit(nt[k]))" for k in ks], ", ")
	return length(ks) == 1 ? "($body,)" : "($body)"
end
_lit(x) = string(x)

# The live value behind a kwarg (live sink): a ScriptVar hands over its object, everything else is
# already the value. NamedTuples/Tuples pass through untouched — GMT.jl reads them directly.
_val(v::ScriptVar) = v.value
_val(x) = x

# ── window state -> figure-wide settings ─────────────────────────────────────────────────────
# Both scene-state dumps are the SAME "k=v;" format, so introspect.jl's parser reads both: the
# read-only snapshot (region, flat2d, colorbar visibility) and the restorable one (ve, camera).
_script_state(h::Ptr{Cvoid}) = (_scene_state(h), _parse_scene_state(_scene_state_full_raw(h)))

# GMT's -p azimuth/elevation from the VTK camera. `azim` is measured CLOCKWISE FROM NORTH, so it is
# atand(dx, dy) — the viewpoint direction's bearing; `elev` is its angle above the horizontal plane.
# The camera offset is read in the window's SCALED space (z already carries the vertical
# exaggeration), which is the space GMT's own -JZ height describes, so the two agree. Returns
# `nothing` for a top-down window: a flat-2D map takes no -p at all.
function _script_view(st, stf)
	get(st, "flat2d", 0) == 1 && return nothing
	haskey(stf, "cam_px") || return nothing
	dx = Float64(stf["cam_px"]) - Float64(stf["cam_fx"])
	dy = Float64(stf["cam_py"]) - Float64(stf["cam_fy"])
	dz = Float64(stf["cam_pz"]) - Float64(stf["cam_fz"])
	horiz = hypot(dx, dy)
	horiz < 1e-9 && return (0.0, 90.0)                    # exactly overhead
	az = mod(atand(dx, dy), 360.0)
	el = clamp(atand(dz, horiz), -90.0, 90.0)
	return (round(az; digits=1), round(el; digits=1))
end

# GMT's -JZ height, in the same units as `figsize`, for a 3-D view.
#
# The viewer scales its geometry by (xfac, 1, zfac*ve) — `applyVE`, 10_geometry.cpp — where `zfac`
# is re-derived from the drawn geometry (`sceneZRef`), never from a unit assumption. So the DISPLAYED
# z-to-x ratio is (zextent * zfac * ve) / (xextent * xfac), and the plot height that reproduces it is
# that ratio times the plot width. Those two factors are read straight out of the scene state: an
# earlier version re-derived the geographic case here from _DEG2M, which was a second copy of a
# scaling the viewer already computes — precisely the fork SACRED_LAW.md forbids. A DLL that predates
# the keys yields no zsize at all (and says so) rather than a guess.
function _script_zsize(ctx::ScriptCtx, st, stf)
	ctx.view === nothing && return nothing
	zext = Float64(get(st, "zmax", 0)) - Float64(get(st, "zmin", 0))
	xext = ctx.region[2] - ctx.region[1]
	(zext > 0 && xext > 0) || return nothing
	if !haskey(stf, "zfac") || !haskey(stf, "xfac")
		push!(ctx.notes, "zsize (-JZ) NOT exported: this viewer build predates the zfac/xfac scene-state keys")
		return nothing
	end
	zfac = Float64(stf["zfac"]); xfac = Float64(stf["xfac"])
	xfac > 0 || return nothing
	h = ctx.figsize * (zext * zfac * Float64(get(stf, "ve", 1.0))) / (xext * xfac)
	return (isfinite(h) && h > 0) ? round(h; sigdigits=4) : nothing
end

# The projection kwarg. A geographic window gets Mercator (GMT's default map projection, and what a
# lon/lat grid is normally shown in); anything else is plotted LINEARLY — data already in projected
# metres must not be re-projected, and an unreferenced grid has no projection to speak of. `-JX`
# with a width alone preserves the region's aspect ratio.
_script_proj(geog::Bool) = geog ? :merc : :X

# ── the T1 -> T2 -> T3 data ladder ───────────────────────────────────────────────────────────
# The GMT command GMT itself recorded for a computed grid (`_grid_command!`, stamped at every
# grid-generating site and persisted into the netCDF header). Empty for anything not made by one
# GMT call.
_script_command(obj)::String =
	(obj isa GMTgrid && hasproperty(obj, :command) && obj.command isa String) ? String(strip(obj.command)) : ""

# Is a recorded command safe to put in a script? Only if every path-looking token in it still exists.
# A GMT command that was run on a temporary file reproduces NOTHING once that file is gone — it would
# turn into a script that errors on someone else's machine, which is worse than shipping the data. So
# T1 is offered only when the command can actually be re-run.
function _script_command_usable(cmd::String)
	isempty(cmd) && return false
	for tok in split(cmd)
		startswith(tok, "-") && continue                      # an option, not a path
		occursin(r"[\\/]|\.(nc|grd|tif|tiff|img|xyz|dat|txt)$"i, tok) || continue
		isfile(tok) || return false
	end
	return true
end

# Bind one layer's data, best rung first:
#   T1 :file    — the layer came off disk and the file is still there: the script reads that file.
#                 The best reuse available, and no bytes are copied anywhere.
#   T1 :command — `recompute=true` and GMT recorded the command that produced this grid: the script
#                 RE-RUNS it, which is what makes the script editable rather than a data loader.
#                 Off by default: a recorded command can reference temporary paths that outlive
#                 nothing, so promoting it is the caller's decision, not a silent default.
#   T2 :live    — the layer only exists in memory: the script FETCHES that object from the live window
#                 (`_find_object`, the same lookup the emitter itself used). Nothing is written.
#   T3 :capture — no data object at all: the script asks the live window for its displayed pixels
#                 (`_display_image`, through `_script_capture_call`). Nothing is written.
function _script_bind!(ctx::ScriptCtx, r::ElementRecipe, obj)
	kind = _script_obj_kind(r)
	prefix = kind === :image ? "I" : "G"
	if r.origin === :file && !isempty(r.source) && isfile(r.source)
		var = _script_var!(ctx, prefix, obj)
		# The FULL path of the file the window loaded, so the script (and its standalone copy, which
		# keeps this line as it is) reads that very file from wherever it is run.
		return DataBind(var, :file, "gmtread($(_lit(abspath(r.source))))")
	end
	if ctx.recompute
		cmd = _script_command(obj)
		if _script_command_usable(cmd)
			var = _script_var!(ctx, prefix, obj)
			return DataBind(var, :command, "gmt($(_lit(cmd)))")
		elseif !isempty(cmd)
			push!(ctx.notes, "recompute skipped for '$(isempty(r.name) ? "(base)" : r.name)': its command references files that are gone")
		end
	end
	ctx.needs_base64 = true          # both remaining rungs reach into the window -> the script needs InteractiveGMT
	if obj !== nothing
		var = _script_var!(ctx, prefix, obj)
		expr = "InteractiveGMT._find_object(Ptr{Nothing}(UInt(" * string(UInt(ctx.scene)) * ")), " *
		       "$(_lit(kind)), $(_lit(r.name)))"
		return DataBind(var, :live, expr)
	end
	x0, x1, y0, y1 = ctx.region
	I = _capture_rect_image(ctx.scene, x0, x1, y0, y1; coords=true)
	var = _script_var!(ctx, "I", I)
	push!(ctx.notes, "layer '$(isempty(r.name) ? "(base)" : r.name)' had no data object — RASTERIZED (a picture, not a reproduction)")
	return DataBind(var, :capture, _script_capture_call(ctx.scene, x0, x1, y0, y1))
end

# The registry kind a raster recipe's object is stored under — ONE answer for the emitter's lookup and
# for the script's live fetch of the same object.
_script_obj_kind(r::ElementRecipe)::Symbol = r.kind in (:image, :dropimage, :basemap) ? :image : :grid

# ── per-element emitters ─────────────────────────────────────────────────────────────────────
# The kinds that put a raster on screen. Same list Save Session replays rasters-first with, for the
# same reason: rasters go down before anything that draws on top of them.
_script_israster(r::ElementRecipe) = r.kind in (:basegrid, :image, :dropgrid, :dropimage, :basemap)

# A grid's z range, for the CPT the layer is drawn with. `range` carries zmin/zmax in slots 5-6
# (GMT fills them on read/compute); a non-finite pair falls back to scanning the buffer, which is
# what the viewer's own `_cpt_nodes` does.
function _script_zrange(G::GMTgrid)
	r = G.range
	(length(r) >= 6 && isfinite(r[5]) && isfinite(r[6]) && r[6] > r[5]) && return (Float64(r[5]), Float64(r[6]))
	fin = filter(isfinite, G.z)
	isempty(fin) && return nothing
	zmn, zmx = extrema(fin)
	return zmx > zmn ? (Float64(zmn), Float64(zmx)) : nothing
end

# The CPT a grid layer wears, as a `makecpt` binding — the SAME call the viewer builds its LUT with
# (`_cpt_nodes_range`: makecpt over the data range, continuous), so the script's colours and the
# window's colours come from one source. Emitted as its own variable because the colorbar needs the
# very same object; a name-only `cmap=:geo` would leave the bar without a range.
function _script_cpt!(ctx::ScriptCtx, r::ElementRecipe, G::GMTgrid)
	aq = _script_aqua_water_cpt!(ctx, r)
	aq === nothing || return aq
	zr = _script_zrange(G)
	zr === nothing && return nothing
	tag = String(get(r.params, "cmap", ""))
	isempty(tag) && (tag = String(_default_cmap(G)))
	var = _script_var!(ctx, "C", nothing)
	mk = (cmap=Symbol(tag), range=zr, continuous=true)
	expr = "makecpt(cmap=$(_lit(mk.cmap)), range=$(_lit(mk.range)), continuous=true)"
	return DataBind(var, :cpt, expr, mk)
end

# A TSUNAMI layer's palette is the "Color Bar water" one: the water side's palette over the water span
# of the slice on screen — the SAME `_aqua_side_pal` / `_aqua_layer_water_range` the layer is coloured
# with and its bar is built from. The layer's grid (the stage, dry cells holding land elevation) and a
# default colormap describe neither. `nothing` for any layer that is not this window's tsunami.
function _script_aqua_water_cpt!(ctx::ScriptCtx, r::ElementRecipe)
	st = get(_AQUA, ctx.scene, nothing)
	(st === nothing || r.name != basename(st.path)) && return nothing
	lo, hi = _aqua_layer_water_range(st, st.cur, st.split, st.globalmm)
	p = _aqua_side_pal(st, 0)
	if p isa Symbol
		var = _script_var!(ctx, "C", nothing)
		mk = (cmap=p, range=(lo, hi), continuous=true)
		expr = "makecpt(cmap=$(_lit(mk.cmap)), range=$(_lit(mk.range)), continuous=true)"
		return DataBind(var, :cpt, expr, mk)
	end
	# A "Color Palettes…" editor palette carries its own node z (log spacing included), which makecpt
	# over a range cannot express: the CPT is written out from those nodes, slice by slice.
	cz, crgb = p
	n = length(cz)
	n < 2 && return nothing
	rgb = permutedims(reshape(Float64.(crgb), 3, n))
	m = n - 1
	col, far = rgb[1:m, :], rgb[2:n, :]
	rg = hcat(Float64.(cz[1:m]), Float64.(cz[2:n]))
	bfn = [1.0 1.0 1.0; 0.0 0.0 0.0; 0.5 0.5 0.5]
	C = GMTcpt(col, zeros(m), rg, [Float64(cz[1]), Float64(cz[n])], bfn, Cint(24), NaN, hcat(col, far),
	           0, String[], String[], "rgb", String[])
	var = _script_var!(ctx, "C", C)
	expr = "GMTcpt($(col), $(zeros(m)), $(rg), $(C.minmax), $(bfn), Cint(24), NaN, $(hcat(col, far)), " *
	       "0, String[], String[], \"rgb\", String[])"
	return DataBind(var, :cpt, expr)
end

# What the window is DOING to this layer's colours, and where the layer sits: `repro` says whether GMT
# can redraw it from data + CPT, `why` names the look when it cannot, and x0..y1 is the layer's own
# footprint (gmtvtk_layer_display, 90_c_api.cpp).
function _script_layer_display(h::Ptr{Cvoid}, name::String)
	n = ccall(_fn(:gmtvtk_layer_display), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, name, C_NULL, Cint(0))
	n <= 0 && return Dict{String,Any}()
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_layer_display), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, name, buf, Cint(n + 1))
	return _parse_scene_state(unsafe_string(pointer(buf)))
end

# Does this layer have to be drawn from the pixels on screen?
#
# A GRID SURFACE: ALWAYS. The viewer renders a grid as a lit 3-D surface — PBR key + fill light, SSAO,
# tone mapping — or with a baked hillshade, a drape, an illumination grid, a composite. GMT draws flat
# CPT colours. Those are never the same picture, so the grid's displayed pixels are what goes to GMT.
# There is no "but the simple case is fine" exception here: emitting `grdimage(G, cmap=C)` for a grid
# is precisely the output that was reported as ignoring the instruction, and no reading of the scene
# state is allowed to talk this back into that shape.
#
# An IMAGE: only when the window is compositing it into something else (draped, Aquamoto blend). A
# plain image actor is rendered flat-albedo, so GMT drawing the same image gives the same picture —
# and passing the image through is better than a screenshot of it (full resolution, re-projectable).
_script_needs_pixels(disp, isgrid::Bool)::Bool = isgrid || get(disp, "repro", 0) == 0

# THE rule for a look GMT has no equivalent for: hand GMT the PIXELS THAT ARE ON SCREEN.
#
# A shaded, draped or host-composited raster cannot be redrawn by grdimage from its data and CPT. The
# answer is NOT to emit a plain unshaded grdimage and mention the shading in a comment — that produces
# a figure which does not look like the window, and "disabling a control to prevent breakage IS the
# violation" (SACRED_LAW.md). Instead the layer's displayed colours are captured into a GMTimage, in
# memory, and given to GMT:
#   * a 3-D grid keeps its GEOMETRY from the grid and takes its COLOURS from the image —
#     `grdview(G, drape=I)`, which is exactly what GMT's drape is for;
#   * anything else is drawn as the image itself, `grdimage(I)`.
# The capture must be the DATA-SPACE bake (`baked_only=true`): it is georeferenced and camera-
# independent, so it can serve as a texture. A perspective screen grab cannot, and is only accepted for
# a top-down figure, where it is already a map.
function _script_rasterize!(ctx::ScriptCtx, r::ElementRecipe, disp, isgrid::Bool)
	x0 = Float64(get(disp, "x0", ctx.region[1])); x1 = Float64(get(disp, "x1", ctx.region[2]))
	y0 = Float64(get(disp, "y0", ctx.region[3])); y1 = Float64(get(disp, "y1", ctx.region[4]))
	# A degenerate layer bbox is no reason to give up on the picture: fall back to the window's own
	# extent. NEVER return without an image — "GMT cannot draw this look" means USE THE DISPLAYED
	# IMAGE, so there is no path from here back to a plain CPT render.
	(x1 > x0 && y1 > y0) || ((x0, x1, y0, y1) = ctx.region)
	# The data-space bake is preferred (georeferenced, camera-independent, so it can also serve as a
	# 3-D drape texture); when there is none, the screen grab is used — which for a top-down display is
	# already a map, and is in every case what is on screen.
	# Four attempts before this function is allowed to come back empty-handed: the layer's own bbox
	# baked, the layer's own bbox grabbed, then the same two over the WHOLE WINDOW. The order is
	# "use the displayed image" and it is not conditional, so every way of getting that image is tried.
	I = nothing
	for (bx0, bx1, by0, by1) in ((x0, x1, y0, y1), ctx.region)
		for baked in (true, false)
			I = try
				_capture_rect_image(ctx.scene, bx0, bx1, by0, by1; coords=true, baked_only=baked)
			catch
				nothing
			end
			I === nothing || break
		end
		I === nothing || break
	end
	if I === nothing
		# The ONLY way out of here, and it is a failure, not a design choice: say so loudly instead of
		# quietly handing back a plain CPT render that does not look like the window.
		push!(ctx.notes, "COULD NOT CAPTURE the displayed image for " *
		                 "'$(isempty(r.name) ? "(base)" : r.name)' — it is drawn from its data instead, " *
		                 "which will NOT match the window")
		return nothing
	end
	label = isempty(r.name) ? (isgrid ? "base grid" : "base image") : r.name
	why = get(disp, "why", "")
	isempty(why) && (why = isgrid ? "lit 3-D surface" : "viewer look")
	push!(ctx.notes, "'$label': its displayed colours ($why) are not expressible in GMT, " *
	                 "so the pixels on screen were captured and are drawn as an image")
	# The displayed pixels are ALREADY IN MEMORY. They fill a GMTimage and that object is handed to GMT
	# directly — never written to disk and read back.
	#
	# The script does NOT carry pixels and does NOT read a file. It asks the LIVE WINDOW for its
	# displayed image, by the address the window already is: `_capture_rect_image` wraps the viewer's
	# own RGB buffer and returns a georeferenced GMTimage. One short line, no data, no disk.
	var = _script_var!(ctx, "I", I)
	ctx.needs_base64 = true          # the script needs InteractiveGMT to reach the window
	return DataBind(var, :capture, _script_capture_call(ctx.scene, x0, x1, y0, y1))
end

"""
	_display_image(scene, w, e, s, n) -> GMTimage

The window's displayed pixels as a GMTimage GMT CAN PLOT. `_capture_rect_image` (grid.jl) hands back
the buffer in the layout Roi Crop wants — (row, col, band), COLUMN-major band-planar, "TCBa" — and
`grdimage` will not draw that: it comes out tiled and colour-scrambled. What GMT plots is band-planar
ROW-major, so the array is transposed to (col, row, band) and labelled "TRBa", then given a COMPLETE
georeference (`_georef_image!`: range + x + y + inc + registration, never range alone).

TWO layouts were wrong here before, and the second only showed up on a NON-SQUARE image:
  * "TCBa" as captured -> tiled 3x3, colours scrambled;
  * pixel-interleaved "TRPa" -> looked perfect on a square capture and SHEARED the picture into a
    diagonal smear as soon as the region was not square (rows read with the wrong stride). A square
    test image cannot tell nx from ny — never verify an image layout with one.

This is the ONE place that turns displayed pixels into a plottable GMTimage. Both the in-session
replay and the generated script call it, so neither can drift into a broken layout again.
"""
function _display_image(scene::Ptr{Cvoid}, w, e, s, n)
	# THE DISPLAYED pixels: the screen grab, lighting and all — the same source File > Save Screenshot
	# GeoTIFF writes, except nothing is written; the image is used.
	I = _capture_rect_image(scene, w, e, s, n; coords=true, prefer_screen=true)
	J = GMT.mat2img(permutedims(I.image, (2, 1, 3));          # (row,col,band) -> (col,row,band)
	                x=[Float64(w), Float64(e)], y=[Float64(s), Float64(n)],
	                proj4=I.proj4, wkt=I.wkt)
	J.layout = "TRBa"                                          # band-planar, ROW-major, north-first
	return _georef_image!(J, w, e, s, n)
end

# The call that builds that image in the generated script: the Scene address as an explicit pointer
# literal plus the rectangle to take. The SAME function the live path runs — one way of getting the
# picture, not two.
_script_capture_call(scene::Ptr{Cvoid}, x0, x1, y0, y1)::String =
	"InteractiveGMT._display_image(Ptr{Nothing}(UInt(" * string(UInt(scene)) * ")), " *
	"$(_lit(Float64(x0))), $(_lit(Float64(x1))), $(_lit(Float64(y0))), $(_lit(Float64(y1))))"

# ── the 3-D BODY view modes: spherical (globe) and cubified (QSC cube) ───────────────────────
# GMT has NO projection for either of these. They are not maps: the globe wraps lon/lat/z onto a
# sphere and the cube onto PROJ's quadrilateralized spherical cube (`globeXf`, 10_geometry.cpp), and
# what is on screen is that BODY seen through the window's camera. `grdview` draws a z(x,y) surface
# under -p and has nothing to draw for a body — pointing it at one is what produced a figure that had
# no relation to the window at all.
#
# So the rule this file already applies per layer to any look GMT cannot reproduce applies to the
# WHOLE WINDOW here: the pixels on screen are captured and handed to GMT. Not a special case — the
# same "hand GMT the displayed pixels" answer, at window scope because the thing GMT cannot draw is
# the window's whole geometry rather than one layer's colours.
#
# `-JX` on a PIXEL region, with no frame: the capture is a picture and its axes are its own pixel
# rows and columns; a lon/lat frame drawn around a globe would be a lie, and `figsize=(w, 0)` lets
# GMT derive the height so the body is not stretched.

"""
	_view_image(scene) -> GMTimage

The window's WHOLE rendered view as a GMTimage `grdimage` can plot, at the current camera. Used by
the globe/cube export, where the per-layer `_display_image` cannot be used: its capture is cut out
of the screen by projecting a WORLD bbox through the camera, and in these two modes the data
coordinates are not the world coordinates (they are wrapped onto the body), so that rectangle lands
nowhere near the picture.

Georeferenced in PIXELS (0..nx, 0..ny), because a perspective view of a 3-D body has no honest map
georeference — the same reason the script emits it with `-JX` and no frame. Layout is the plottable
band-planar row-major "TRBa", the one `_display_image` documents.
"""
function _view_image(scene::Ptr{Cvoid})
	haskey(_LIB_FNS, :gmtvtk_capture_view_rgba) ||
		error("gmtscript: this viewer library has no gmtvtk_capture_view_rgba, so a globe/cube window " *
		      "cannot be captured — rebuild deps/build/gmtvtk.dll")
	pRgb = Ref{Ptr{UInt8}}(C_NULL); pW = Ref{Cint}(0); pH = Ref{Cint}(0)
	ok = ccall(_fn(:gmtvtk_capture_view_rgba), Cint,
	           (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}), scene, pRgb, pW, pH)
	ok == 0 && error("gmtscript: could not capture this window's view")
	nx, ny = Int(pW[]), Int(pH[])
	try
		v = unsafe_wrap(Array, pRgb[], (4, nx, ny))    # (band, col, row) RGBA, C memory, borrowed
		p = permutedims(v, (2, 3, 1))                  # -> (col, row, band), owned: what grdimage plots
		I = GMT.mat2img(p[:, :, 1:3]; x=[0.0, Float64(nx)], y=[0.0, Float64(ny)])
		# The FOURTH band is the window background's transparency, not a colour: it rides in the
		# image's own `alpha` (a GMTimage field, not a mat2img kwarg) so the planet lands on the
		# figure with nothing painted around it.
		I.alpha = p[:, :, 4]
		I.layout = "TRBa"                              # band-planar, ROW-major, north-first
		return _georef_image!(I, 0.0, Float64(nx), 0.0, Float64(ny))
	finally
		ccall(_fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), pRgb[])
	end
end

# The call that fetches that picture in the generated script — the SAME function the live sink runs,
# addressed by the window's own pointer, exactly like `_script_capture_call` above.
_script_view_capture_call(scene::Ptr{Cvoid})::String =
	"InteractiveGMT._view_image(Ptr{Nothing}(UInt(" * string(UInt(scene)) * ")))"

# The whole export for a globe/cube window: one capture, one `grdimage`, no frame. The figure-wide
# context is rewritten to match what is actually emitted (a pixel region on a linear projection, and
# NO -p / -JZ: the camera is already inside the picture), so the hoisted REG/PROJ consts in the
# script header describe the call below instead of a map that is not being drawn.
function _script_body_view!(ctx::ScriptCtx, cube::Bool)
	I = _view_image(ctx.scene)
	nx, ny = size(I.image, 1), size(I.image, 2)        # "TRBa": (col, row, band)
	ctx.region = (0.0, Float64(nx), 0.0, Float64(ny))
	ctx.proj   = :X
	ctx.view   = nothing
	ctx.zsize  = nothing
	body = cube ? "cubified (QSC cube)" : "spherical (globe)"
	push!(ctx.notes, "the window is in the $body view mode, which GMT has no projection for: the " *
	                 "WHOLE VIEW was captured and is drawn as one image — a picture of the window, " *
	                 "so nothing in it is editable or re-projectable")
	var = _script_var!(ctx, "I", I)
	ctx.needs_base64 = true                            # the script asks the live window for the pixels
	bind = DataBind(var, :capture, _script_view_capture_call(ctx.scene))
	# `alpha_color` (-Q) is what makes GMT honour the capture's alpha band: without it the image is
	# plotted opaque and the viewer's background comes back as a rectangle around the body.
	kw = Pair{Symbol,Any}[:region  => ScriptVar(:REG, ctx.region),
	                      :proj    => ScriptVar(:PROJ, ctx.proj),
	                      :figsize => (ScriptVar(:FIGSIZE, ctx.figsize), 0),
	                      :frame   => :none,
	                      :alpha_color => true]
	return ScriptStep[ScriptStep("whole-view capture — $body", DataBind[bind], :grdimage, var, kw, false)]
end

# One raster layer -> one grdimage/grdview call. `first` carries the figure-wide settings (region,
# projection, size, frame, and the view/zsize kwargs when the window is tilted); later layers append
# with `!` and inherit them, exactly as a hand-written GMT.jl script would.
function _script_emit_raster!(ctx::ScriptCtx, r::ElementRecipe, obj, first::Bool)
	kw = Pair{Symbol,Any}[]
	isgrid = obj isa GMTgrid
	# A relief surface in a tilted window is grdview; a flat raster (image, or any raster in a
	# top-down window) is grdimage under the same -p. Element type + whether it carries z, never a
	# global 2-D/3-D switch.
	mod = (isgrid && ctx.view !== nothing) ? :grdview : :grdimage
	# Does GMT own this layer's look, or does the viewer? Asked FIRST, before any data binding, because
	# when the answer is "the viewer" a top-down layer needs no grid data at all — the captured image is
	# the layer (see _script_rasterize!).
	disp = _script_layer_display(ctx.scene, r.name)
	shot = _script_needs_pixels(disp, isgrid) ? _script_rasterize!(ctx, r, disp, isgrid) : nothing
	if shot !== nothing
		# The image carries the layer's colours, so no `cmap` goes on the call — but the window's COLOUR
		# BAR is still that layer's CPT, and losing it would be its own fidelity hole. So the palette is
		# still built and bound; only the kwarg is dropped.
		cptbind = isgrid ? _script_cpt!(ctx, r, obj) : nothing
		if isgrid && ctx.view !== nothing
			# Geometry from the grid, colours from the screen — GMT's own drape.
			gbind = _script_bind!(ctx, r, obj)
			push!(kw, :drape => shot.var)
			push!(kw, :surftype => :image)
			bl = cptbind === nothing ? DataBind[gbind, shot] : DataBind[gbind, shot, cptbind]
			return _script_emit_raster_finish!(ctx, r, bl, :grdview, gbind.var, kw, first, isgrid, disp)
		end
		# Top-down (or an image): the capture IS the layer, so no grid/image data is bound at all.
		bl = cptbind === nothing ? DataBind[shot] : DataBind[shot, cptbind]
		return _script_emit_raster_finish!(ctx, r, bl, :grdimage, shot.var, kw, first, isgrid, disp)
	end
	bind = _script_bind!(ctx, r, obj)
	binds = DataBind[bind]
	if isgrid
		cptbind = _script_cpt!(ctx, r, obj)
		if cptbind !== nothing
			push!(binds, cptbind)
			push!(kw, :cmap => cptbind.var)
		end
		mod === :grdview && push!(kw, :surftype => :image)     # -Qi: a draped image, not a mesh
	end
	return _script_emit_raster_finish!(ctx, r, binds, mod, bind.var, kw, first, isgrid, disp)
end

# THE FRAME: what the raster's own Axes properties dialog says (gmtvtk_axes_props_h), as GMT.jl's
# frame / xaxis / yaxis / zaxis / par kwargs. A set never edited draws the app's mapping convention:
# W and S annotated, ticks on all four sides. One function, so the backdrop export frames the same way.
function _script_frame_kw(h::Ptr{Cvoid}, name::String, view3d::Bool)::Vector{Pair{Symbol,Any}}
	default = Pair{Symbol,Any}[:frame => (annot=:auto, ticks=:auto, axes=:WSen)]
	n = ccall(_fn(:gmtvtk_axes_props_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, name, C_NULL, Cint(0))
	n <= 0 && return default
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_axes_props_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, name, buf, Cint(n + 1))
	p = Dict{String,String}()
	for ln in split(unsafe_string(pointer(buf)), '\n')
		i = findfirst('=', ln)
		i === nothing || (p[String(ln[1:i-1])] = String(ln[i+1:end]))
	end
	get(p, "-B.+n", "0") == "1" && return Pair{Symbol,Any}[:frame => :none]
	on(k) = get(p, k, "0") == "1"
	# Sides: annotations + ticks = W, line + ticks = w, line only = l/r/b/t, not drawn = left out.
	axes = ""
	for (k, up, lo, line) in (("W", "W", "w", "l"), ("E", "E", "e", "r"), ("S", "S", "s", "b"), ("N", "N", "n", "t"))
		m = get(p, "MAP_FRAME_AXES." * k, "")
		axes *= m == "Annot. + ticks" ? up : m == "Line + ticks" ? lo : m == "Line only" ? line : ""
	end
	if view3d
		m = get(p, "MAP_FRAME_AXES.Z", "")
		z = m == "Annot. + ticks" ? "Z" : m == "Line + ticks" ? "z" : ""
		isempty(z) || (axes *= z * join(c for c in "1234" if on("-B.Z$c")))
	end
	fr = Pair{Symbol,Any}[:axes => (isempty(axes) ? :none : Symbol(axes))]
	t = strip(get(p, "-B.+t", "")); isempty(t) || push!(fr, :title => String(t))
	t = strip(get(p, "-B.+s", "")); isempty(t) || push!(fr, :subtitle => String(t))
	view3d && on("-B.+b") && push!(fr, :cube => true)
	on("-B.+g") && !isempty(get(p, "-B.+g.color", "")) && push!(fr, :fill => p["-B.+g.color"])
	# One -B stride: auto / empty (not drawn) / stride[+-phase][unit].
	function stride(pre, item)
		v = strip(get(p, pre * item, ""))
		isempty(v) && return nothing
		lowercase(v) == "auto" && return :auto
		ph = strip(get(p, pre * item * ".phase", ""))
		(isempty(ph) || ph[1] in "+-") || (ph = "+" * ph)
		return String(v) * ph * get(p, pre * item * ".unit", "")
	end
	gridOn = (on("MAP_FRAME_AXES.Sg"), on("MAP_FRAME_AXES.Wg"), on("MAP_FRAME_AXES.Sg") || on("MAP_FRAME_AXES.Wg"))
	ints = [Pair{Symbol,Any}[] for _ in 1:3]          # per axis: annot / ticks / grid
	rest = [Pair{Symbol,Any}[] for _ in 1:3]          # per axis: label / prefix / suffix
	for (k, ax) in ((1, "x"), (2, "y"), (3, "z"))
		(k == 3 && !view3d) && continue
		pre = "-Bp" * ax * "."
		(s = stride(pre, "a")) === nothing || push!(ints[k], :annot => s)
		(s = stride(pre, "f")) === nothing || push!(ints[k], :ticks => s)
		if gridOn[k]
			s = stride(pre, "g")
			push!(ints[k], :grid => s === nothing ? :auto : s)
		end
		for (key, field) in (("label", :label), ("prefix", :prefix), ("unit", :suffix))
			t = strip(get(p, "-B" * ax * "." * key, ""))
			isempty(t) || push!(rest[k], field => String(t))
		end
	end
	# X and Y spaced alike: ONE frame=(annot=…, ticks=…, grid=…), as a hand-written script says it.
	if ints[1] == ints[2]
		append!(fr, ints[1])
		empty!(ints[1]); empty!(ints[2])
	end
	kw = Pair{Symbol,Any}[:frame => (; fr...)]
	for (k, sym) in ((1, :xaxis), (2, :yaxis), (3, :zaxis))
		a = vcat(ints[k], rest[k])
		isempty(a) || push!(kw, sym => (; a...))
	end
	# Fonts and pens: ONLY what differs from GMT's own defaults (gmtdefaults -D, classic theme — what a
	# GMT.jl session draws with). Compared per component, so an unchanged size or face is left out
	# and GMT's positional syntax keeps the rest ("12p,,red" = only the colour, ",,red" style).
	part(k, f) = strip(get(p, k * "." * f, ""))
	pts(s) = (m = match(r"^([0-9.]+)\s*([pci]?)$", s); m === nothing ? NaN :
	          parse(Float64, m[1]) * (m[2] == "c" ? 72 / 2.54 : m[2] == "i" ? 72.0 : 1.0))
	samelen(a, b) = isempty(a) || (x = pts(a); isfinite(x) && abs(x - pts(b)) < 1e-3)
	samecol(c) = isempty(c) || lowercase(c) in ("black", "#000000", "0/0/0")
	join_pos(bits) = rstrip(join(bits, ","), ',')
	par = Pair{Symbol,Any}[]
	fontdef = Dict("FONT_ANNOT_PRIMARY" => ("12p", "Helvetica"), "FONT_LABEL" => ("16p", "Helvetica"),
	               "FONT_TITLE" => ("24p", "Helvetica"))
	for k in ("FONT_ANNOT_PRIMARY", "FONT_LABEL", "FONT_TITLE")
		sz, nm, c = part(k, "size"), part(k, "name"), part(k, "color")
		bits = [samelen(sz, fontdef[k][1]) ? "" : sz, (isempty(nm) || nm == fontdef[k][2]) ? "" : nm,
		        samecol(c) ? "" : c]
		v = join_pos(bits)
		isempty(v) || push!(par, Symbol(k) => v)
	end
	pendef = Dict("MAP_FRAME_PEN" => "1.5p", "MAP_TICK_PEN_PRIMARY" => "0.25p", "MAP_GRID_PEN_PRIMARY" => "0.25p")
	for k in ("MAP_FRAME_PEN", "MAP_TICK_PEN_PRIMARY", "MAP_GRID_PEN_PRIMARY")
		w, c, st = part(k, "width"), part(k, "color"), part(k, "style")
		bits = [samelen(w, pendef[k]) ? "" : w, samecol(c) ? "" : c, st in ("", "solid") ? "" : st]
		v = join_pos(bits)
		isempty(v) || push!(par, Symbol(k) => v)
	end
	ma, mi = part("MAP_TICK_LENGTH_PRIMARY", "major"), part("MAP_TICK_LENGTH_PRIMARY", "minor")
	if !(samelen(ma, "5p") && samelen(mi, "2.5p"))
		push!(par, :MAP_TICK_LENGTH_PRIMARY => (isempty(ma) ? "5p" : ma) * (isempty(mi) ? "" : "/" * mi))
	end
	isempty(par) || push!(kw, :par => (; par...))
	return kw
end

# The half every raster path shares: the figure-wide kwargs (first layer only) and the ScriptStep. Both
# the ordinary CPT path and the rasterized-look path above end here, so neither can drift from the
# other on region/projection/size/frame or on how the step is labelled.
function _script_emit_raster_finish!(ctx::ScriptCtx, r::ElementRecipe, binds::Vector{DataBind}, mod::Symbol,
                                    pos::ScriptVar, kw::Vector{Pair{Symbol,Any}}, first::Bool,
                                    isgrid::Bool, disp)
	if first
		push!(kw, :region => ScriptVar(:REG, ctx.region))
		push!(kw, :proj => ScriptVar(:PROJ, ctx.proj))
		push!(kw, :figsize => ScriptVar(:FIGSIZE, ctx.figsize))
		ctx.view  !== nothing && push!(kw, :view => ctx.view)
		ctx.zsize !== nothing && push!(kw, :zsize => ctx.zsize)
		# The frame of THIS raster's own axes, as its Axes properties say (default: W/S annotated,
		# ticks on all four sides — the mapping convention this app holds to).
		append!(kw, _script_frame_kw(ctx.scene, r.name, ctx.view !== nothing))
	end
	label = isempty(r.name) ? (isgrid ? "base grid" : "base image") : r.name
	tier = isempty(binds) ? "" : String(binds[1].tier)   # the DATA bind; a trailing :cpt is not the tier
	why = get(disp, "why", "")
	extra = (get(disp, "repro", 1) == 0 && !isempty(why)) ? ", displayed pixels: $why" : ""
	return ScriptStep("$(label)  ($(r.kind), $(tier)$(extra))", binds, mod, pos, kw, !first)
end

# The colour bar, when the window is showing one. Positioned explicitly: GMT's automatic placement
# is unreliable under -p, and the window's own bar coordinates are VTK viewport fractions, not a
# GMT anchor — so this is a sane fixed placement, flagged in the fidelity notes rather than
# pretended to be exact.
function _script_emit_colorbar(ctx::ScriptCtx, cptvar::ScriptVar)
	kw = Pair{Symbol,Any}[:pos => (anchor=:MR, offset=(1.0, 0.0)), :frame => (annot=:auto,)]
	return ScriptStep("colour bar", DataBind[], :colorbar, cptvar, kw, true)
end

# ── vector layers (P2) ───────────────────────────────────────────────────────────────────────
# The C++-owned vector elements have no add-time recipe, so — exactly like Save Session's polygons,
# faults and texts — they are SNAPSHOTTED at export time through the `gmtvtk_serialize_*` blobs and
# turned into calls here. Two of those serializers are new (overlays, symbols); the polygon and text
# ones are the very same the session already uses, read through the same two-pass helpers.

# Two-pass string fetch, the shape every serializer in this package uses.
function _script_blob(sym::Symbol, h::Ptr{Cvoid})::String
	n = ccall(_fn(sym), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
	n <= 0 && return ""
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(sym), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
	return unsafe_string(pointer(buf))
end

# GMT's "r/g/b" (0-255) from the viewer's 0-1 triple — the colour form every GMT.jl pen/fill kwarg
# takes, so no call site has to decide how to spell a colour.
_script_rgb(r, g, b) = string(round(Int, 255r), "/", round(Int, 255g), "/", round(Int, 255b))

# "x,y,z|x,y,z" -> an n×3 Matrix{Float64} (an empty matrix when the blob is empty/malformed).
function _script_verts(blob::String)
	rows = split(blob, '|'; keepempty=false)
	M = Matrix{Float64}(undef, length(rows), 3)
	k = 0
	for row in rows
		c = split(row, ',')
		length(c) < 3 && continue
		x = tryparse(Float64, c[1]); y = tryparse(Float64, c[2]); z = tryparse(Float64, c[3])
		(x === nothing || y === nothing || z === nothing) && continue
		k += 1
		M[k, 1] = x; M[k, 2] = y; M[k, 3] = z
	end
	return k == 0 ? Matrix{Float64}(undef, 0, 3) : M[1:k, :]
end

# A vector element's data becomes a GMTdataset (one per segment). Whether the script gets 3 columns
# or 2 is decided HERE, once, by the same rule §4 of the plan states: z travels only when the layer
# actually has depth AND the figure is tilted — a flat table under a top-down view is a 2-column
# table, and passing it a constant z column would only invite GMT to draw it in a perspective that
# does not exist.
_script_dataset(segs::Vector{Matrix{Float64}}, want3d::Bool) =
	[GMT.mat2ds(want3d ? M : M[:, 1:2]) for M in segs if !isempty(M)]
_script_has_z(segs::Vector{Matrix{Float64}}) = any(M -> !isempty(M) && any(!iszero, view(M, :, 3)), segs)

# A FILLED overlay's rings for GMT: the viewer fills even-odd (a ring inside a ring is a hole), GMT
# fills each polygon and cuts only the segments headed " -Ph" out of the perimeter they FOLLOW. So
# each ring's nesting depth (how many other rings contain it) decides: even = a perimeter, odd = a
# hole, emitted right after the ring that immediately contains it.
function _script_fill_rings(ds::Vector{<:GMTdataset})
	n = length(ds)
	inside(x, y, M) = begin                          # even-odd point in polygon
		c = false;  m = size(M, 1)
		j = m
		@inbounds for i in 1:m
			xi, yi, xj, yj = M[i, 1], M[i, 2], M[j, 1], M[j, 2]
			((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi) && (c = !c)
			j = i
		end
		c
	end
	bb = [(extrema(view(D.data, :, 1)), extrema(view(D.data, :, 2))) for D in ds]
	area(M) = abs(sum(M[i, 1] * M[mod1(i + 1, size(M, 1)), 2] - M[mod1(i + 1, size(M, 1)), 1] * M[i, 2] for i in 1:size(M, 1))) / 2
	ar = [area(D.data) for D in ds]
	depth = zeros(Int, n);  parent = zeros(Int, n)
	for i in 1:n
		x, y = ds[i].data[1, 1], ds[i].data[1, 2]
		for j in 1:n
			(i == j || ar[j] <= ar[i]) && continue
			(bb[j][1][1] <= x <= bb[j][1][2] && bb[j][2][1] <= y <= bb[j][2][2]) || continue
			inside(x, y, ds[j].data) || continue
			depth[i] += 1
			(parent[i] == 0 || ar[j] < ar[parent[i]]) && (parent[i] = j)
		end
	end
	out = eltype(ds)[]
	for i in 1:n
		iseven(depth[i]) || continue
		ds[i].header = "";  push!(out, ds[i])
		for k in 1:n
			(parent[k] == i && isodd(depth[k])) || continue
			ds[k].header = " -Ph";  push!(out, ds[k])
		end
	end
	return out
end

# ── plotted vectors come from the LIVE SCENE, never from disk ─────────────────────────────────
# A plotted symbol layer / line overlay exists only in the viewer's memory — it was never a file, so
# there is nothing to read back and nothing may be written. These two fetch it straight out of the
# scene, by the window's address, and are what the generated script calls: exactly the same route the
# in-session replay takes, so script and replay cannot disagree about the data.

"""
	_symbol_layer(scene, name, want3d=false) -> Vector{GMTdataset}

One symbol layer's points in TRUE data coordinates, by its Scene Objects name. Empty if the layer is
gone.

A symbol layer stores x MULTIPLIED BY `xfac` — `addSymbols` (50_scene.cpp) bakes it in, because the
symbol actor scales only z ("x already baked into the points", applyVE). Handing those numbers over
raw puts every symbol at the wrong longitude: at 36°N, xfac ≈ 0.81, so -12° lands near -9.7°. So x is
divided back out HERE, in the one place that reads the layer, using the viewer's own `gmtvtk_get_xfac`
— never a second copy of the factor.
"""
function _symbol_layer(scene::Ptr{Cvoid}, name::String, want3d::Bool=false)
	for sl in _parse_symbols_blob(_script_blob(:gmtvtk_serialize_symbols, scene))
		sl.name == name || continue
		M = copy(sl.xyz)
		isempty(M) && break
		xf = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), scene)
		(isfinite(xf) && xf != 0.0) && (M[:, 1] ./= xf)     # un-bake x -> true longitude
		return [GMT.mat2ds(want3d ? M : M[:, 1:2])]
	end
	return GMTdataset[]
end

"""
	_arrow_layer(scene, name, cm_per_unit, want3d=false) -> Vector{GMTdataset}

An ARROW layer (a vector field drawn as solid arrows) as GMT vectors: one segment per colour, rows
`x y [z] direction length` for `plot(…, S="v…")`, direction in degrees counter-clockwise from +x on
the map, length in cm on paper (`cm_per_unit` = paper cm per WORLD unit of the window, which is what
the layer's arrow length is measured in). A layer coloured by magnitude is split into up to 24
magnitude classes, each wearing the mean colour of its arrows (a segment header carries one -G/-W).
x is un-baked exactly as in `_symbol_layer`.
"""
function _arrow_layer(scene::Ptr{Cvoid}, name::String, cm_per_unit::Float64, want3d::Bool=false)
	for sl in _parse_symbols_blob(_script_blob(:gmtvtk_serialize_symbols, scene))
		(sl.name == name && !isempty(sl.uv)) || continue
		n = size(sl.xyz, 1)
		xf = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), scene)
		(isfinite(xf) && xf != 0.0) || (xf = 1.0)
		mag = [hypot(sl.uv[i, 1], sl.uv[i, 2]) for i in 1:n]
		mmax = maximum(mag)
		mmax > 0 || return GMTdataset[]
		ncls = isempty(sl.rgb) ? 1 : min(24, n)
		cls = [ncls == 1 ? 1 : clamp(floor(Int, mag[i] / mmax * ncls) + 1, 1, ncls) for i in 1:n]
		out = GMTdataset[]
		for c in 1:ncls
			idx = findall(==(c), cls)
			isempty(idx) && continue
			rgb = isempty(sl.rgb) ? (sl.r, sl.g, sl.b) :
			      Tuple(sum(sl.rgb[i, k] for i in idx) / length(idx) for k in 1:3)
			col = string(round(Int, 255rgb[1]), "/", round(Int, 255rgb[2]), "/", round(Int, 255rgb[3]))
			M = Matrix{Float64}(undef, length(idx), want3d ? 5 : 4)
			for (r, i) in enumerate(idx)
				M[r, 1] = sl.xyz[i, 1] / xf;  M[r, 2] = sl.xyz[i, 2]
				want3d && (M[r, 3] = sl.xyz[i, 3])
				M[r, end-1] = atand(sl.uv[i, 2], sl.uv[i, 1])
				M[r, end]   = sl.worldSize * mag[i] / mmax * cm_per_unit
			end
			D = GMT.mat2ds(M)
			D.header = " -G$col -W0.5p,$col"
			push!(out, D)
		end
		return out
	end
	return GMTdataset[]
end

"One line/point overlay's segments, by its Scene Objects name, as a multisegment GMTdataset."
function _overlay_layer(scene::Ptr{Cvoid}, name::String, want3d::Bool=false)
	for ov in _parse_overlays_blob(_script_blob(:gmtvtk_serialize_overlays, scene))
		ov.name == name || continue
		ds = _script_dataset(ov.segs, want3d)
		return ov.filled ? _script_fill_rings(ds) : ds
	end
	return GMTdataset[]
end

# Bind a plotted vector layer: the live dataset for `gmtreplay`, and for the script a CALL that fetches
# the same thing from the window. No file, ever. The emitted call is ALWAYS `fn(scene, name, want3d)`,
# so every fetcher bound here takes that third argument, even one whose geometry has no 2-D/3-D choice.
function _script_bind_live_vector!(ctx::ScriptCtx, fn::String, name::String, want3d::Bool,
                                   ds::Vector{<:GMTdataset})
	var = _script_var!(ctx, "D", length(ds) == 1 ? ds[1] : ds)
	ctx.needs_base64 = true          # the script reaches into the window -> it needs InteractiveGMT
	expr = "InteractiveGMT.$fn(Ptr{Nothing}(UInt(" * string(UInt(ctx.scene)) * ")), " *
	       "$(_lit(name)), $(_lit(want3d)))"
	return DataBind(var, :live, expr)
end

# Same, for the two fetches whose key is not (name, want3d): text labels are keyed by their STYLE and
# a curtain by its index in the window's provenance log.
function _script_bind_live_text!(ctx::ScriptCtx, szpx::Int, rgb::String, ds::Vector{<:GMTdataset},
                                 skip::String = "")
	var = _script_var!(ctx, "D", length(ds) == 1 ? ds[1] : ds)
	ctx.needs_base64 = true
	expr = "InteractiveGMT._text_labels(Ptr{Nothing}(UInt(" * string(UInt(ctx.scene)) * ")), " *
	       "$(szpx), $(_lit(rgb))" * (isempty(skip) ? "" : ", $(_lit(skip))") * ")"
	return DataBind(var, :live, expr)
end
function _script_bind_live_curtain!(ctx::ScriptCtx, which::Int, ds::Vector{<:GMTdataset})
	var = _script_var!(ctx, "D", length(ds) == 1 ? ds[1] : ds)
	ctx.needs_base64 = true
	expr = "InteractiveGMT._curtain_outline(Ptr{Nothing}(UInt(" * string(UInt(ctx.scene)) * ")), $(which))"
	return DataBind(var, :live, expr)
end

"One user-drawn polygon/polyline/rect/circle, by name, in true coordinates."
function _poly_layer(scene::Ptr{Cvoid}, name::String, want3d::Bool=false)
	for line in split(_serialize_polys_raw(scene), '\n'; keepempty=false)
		p = split(line, ';'; limit=13)
		length(p) < 13 && continue
		String(p[12]) == name || continue
		M = _script_verts(String(p[13]))
		isempty(M) && break
		return [GMT.mat2ds(want3d ? M : M[:, 1:2])]
	end
	return GMTdataset[]
end

"""
	_text_labels(scene, szpt, rgb, skip="")

The window's text labels of ONE style (`szpt` = the size as SET, in points, + `r/g/b` colour), with
their strings — read live out of the window, never off disk. BATCH labels are included (city names,
focal-mechanism dates): they are grouped by style like any other label, and they have to be here
because `gmtvtk_capture_rect_rgb` hides text before grabbing the pixels, so a label this fetch drops
is a label the figure loses. `skip` names a group whose labels some other call draws itself — the
contour annotations, which `grdcontour` places (see `_script_emit_contours!`).
"""
function _text_labels(scene::Ptr{Cvoid}, szpt::Int, rgb::String, skip::String = "")
	xs = Float64[]; ys = Float64[]; txt = String[]
	for t in _parse_texts_blob(_serialize_texts_raw(scene; groups=true))
		(isempty(skip) || t.group != skip) || continue
		(t.size == szpt && _script_rgb(t.r, t.g, t.b) == rgb) || continue
		push!(xs, t.x); push!(ys, t.y); push!(txt, t.text)
	end
	isempty(xs) && return GMTdataset[]
	return [GMT.mat2ds(hcat(xs, ys); text=txt)]
end

"One fault's TRACE, by name."
function _fault_layer(scene::Ptr{Cvoid}, name::String, want3d::Bool=false)
	for line in split(_serialize_faults_raw(scene), '\n'; keepempty=false)
		startswith(line, "F;") || continue
		p = split(line, ';'; limit=10)
		length(p) < 10 && continue
		String(p[9]) == name || continue
		M = _script_verts2(String(p[10]))
		size(M, 1) < 2 && break
		return [GMT.mat2ds(M)]
	end
	return GMTdataset[]
end

"One fault's 3-D dipping PLANE, by name (the `P` record — geometry read off the actor)."
function _fault_plane(scene::Ptr{Cvoid}, name::String, want3d::Bool=false)
	for line in split(_serialize_faults_raw(scene), '\n'; keepempty=false)
		startswith(line, "P;") || continue
		p = split(line, ';'; limit=3)
		length(p) < 3 && continue
		String(p[2]) == name || continue
		M = _script_verts(String(p[3]))
		size(M, 1) < 3 && break
		return [GMT.mat2ds(M)]
	end
	return GMTdataset[]
end

"One slip MODEL's patches, by group name — each patch a segment carrying its own -G fill header."
function _slip_layer(scene::Ptr{Cvoid}, group::String, want3d::Bool=false)
	ds = GMTdataset[]
	for line in split(_serialize_faults_raw(scene), '\n'; keepempty=false)
		startswith(line, "S;") || continue
		p = split(line, ';'; limit=12)
		length(p) < 12 && continue
		String(p[2]) == group || continue
		M = _script_verts2(String(p[12]))
		size(M, 1) < 3 && continue
		fc = split(p[11], ',')
		length(fc) < 3 && continue
		d = GMT.mat2ds(M)
		d.header = " -G" * _script_rgb(parse(Float64, fc[1]), parse(Float64, fc[2]), parse(Float64, fc[3]))
		push!(ds, d)
	end
	return ds
end

"One curtain's 3-D outline, by its index in this window's provenance log."
function _curtain_outline(scene::Ptr{Cvoid}, which::Int)
	k = 0
	for r in get(_SESSION_LOG, scene, ElementRecipe[])
		r.kind === :curtain || continue
		k += 1
		k == which || continue
		return _curtain_outline_ds(r)
	end
	return GMTdataset[]
end

# The outline itself: the track at the top of the z range, back along the bottom, closed.
function _curtain_outline_ds(r::ElementRecipe)
	toks = split(get(r.params, "track", ""), '|'; keepempty=false)
	length(toks) < 2 && return GMTdataset[]
	xy = Matrix{Float64}(undef, length(toks), 2)
	for (i, t) in enumerate(toks)
		c = split(t, ',')
		length(c) < 2 && return GMTdataset[]
		xy[i, 1] = parse(Float64, c[1]); xy[i, 2] = parse(Float64, c[2])
	end
	zmn = parse(Float64, get(r.params, "zmin", "0")); zmx = parse(Float64, get(r.params, "zmax", "0"))
	n = size(xy, 1)
	M = Matrix{Float64}(undef, 2n + 1, 3)
	for i in 1:n
		M[i, 1] = xy[i, 1]; M[i, 2] = xy[i, 2]; M[i, 3] = zmx
		M[n + i, 1] = xy[n + 1 - i, 1]; M[n + i, 2] = xy[n + 1 - i, 2]; M[n + i, 3] = zmn
	end
	M[2n + 1, :] = M[1, :]
	return [GMT.mat2ds(M)]
end

"One focal-mechanism catalog, by group name, in GMT's Aki & Richards column order."
function _meca_layer(scene::Ptr{Cvoid}, name::String, want3d::Bool=false)
	for (nm, M) in get(_MECA_TABLE, scene, Tuple{String,Matrix{Float64}}[])
		nm == name || continue
		isempty(M) && break
		return [GMT.mat2ds(M)]
	end
	return GMTdataset[]
end

# DELETED, do not bring back: `_script_bind_vector!`, which wrote an ASCII sidecar and emitted a
# `gmtread(joinpath(@__DIR__, "script_data", …))` for it. NOTHING plotted in the viewer is on disk, so
# nothing plotted may be written to one: every vector element above is fetched from the live window
# instead. If you find yourself adding a sidecar for a plotted element, that is this bug coming back.

# GMT pen style for the viewer's lineStyle code (0 solid / 1 dashed / 2 dotted).
_script_linestyle(ls::Int) = ls == 1 ? :dash : ls == 2 ? :dot : nothing

# ONE GMT pen string "width,colour[,style]" — every line an emitter draws carries its pen in this one
# form, so the text sink can hoist it into a `penN = "..."` variable just above the call that uses it.
_script_pen(w::String, rgb::String, st::Union{Nothing,Symbol}=nothing) =
	w * "," * rgb * (st === :dash ? ",-" : st === :dot ? ",." : "")

# One line/point overlay -> plot! / plot3!. Lines carry a pen, points a marker; both keep the
# layer's own colour, width/size and dash pattern as the user last left them (the serializer reads
# those off the actor, which is where the context menu writes them).
function _script_emit_overlay!(ctx::ScriptCtx, ov::OverlayRow)
	mode = ov.mode
	r, g, b = ov.r, ov.g, ov.b
	lw, ps  = ov.lw, ov.ps
	lstyle  = ov.lstyle
	name    = ov.name
	segs    = ov.segs
	want3d  = ctx.view !== nothing && _script_has_z(segs)
	ds = _script_dataset(segs, want3d)
	isempty(ds) && return nothing
	ov.filled && mode == 1 && (ds = _script_fill_rings(ds))   # holes as " -Ph", as the viewer paints them
	# Plotted, never on disk: fetched from the window at run time.
	bind = _script_bind_live_vector!(ctx, "_overlay_layer", name, want3d, ds)
	kw = Pair{Symbol,Any}[]
	if mode == 1
		# The actor's line width is in screen PIXELS -> points, or the pen comes out a different weight.
		push!(kw, :pen => _script_pen(_script_ptstr_w(lw, ctx), _script_rgb(r, g, b), _script_linestyle(lstyle)))
		ov.filled && push!(kw, :fill => _script_rgb(r, g, b))   # filled overlay: area in the line colour
	else
		push!(kw, :marker => :point)
		push!(kw, :ms => _script_ptstr_sym(ps))
		push!(kw, :mc => _script_rgb(r, g, b))
	end
	return ScriptStep("$(isempty(name) ? "overlay" : name)  (overlay)", DataBind[bind],
	                  want3d ? :plot3 : :plot, bind.var, kw, true)
end

# One symbol layer -> plot!/plot3! with the layer's OWN GMT symbol code. The viewer sizes symbols in
# screen pixels and GMT in points; they are carried across as points, which is the closest thing GMT
# has to "constant on screen" and is flagged in the fidelity notes rather than silently rescaled.
function _script_emit_symbols!(ctx::ScriptCtx, sl::SymbolRow, segs::Vector{Matrix{Float64}})
	sym = sl.sym; size = sl.sizePx; filled = sl.filled
	r, g, b    = sl.r, sl.g, sl.b
	er, eg, eb = sl.er, sl.eg, sl.eb
	ew = sl.ew; evis = sl.evis
	name = sl.name
	want3d = ctx.view !== nothing && _script_has_z(segs)
	ds = _script_dataset(segs, want3d)
	isempty(ds) && return nothing
	# Plotted, never on disk: fetched from the window at run time.
	bind = _script_bind_live_vector!(ctx, "_symbol_layer", name, want3d, ds)
	# `sizePx` and the edge width are SCREEN PIXELS. GMT wants POINTS — a symbol asked for in px is
	# simply the wrong size on paper.
	kw = Pair{Symbol,Any}[:marker => sym, :ms => _script_ptstr_sym(size)]
	# Fill and outline are two different colours (the viewer's yellow triangle has a black edge);
	# an unfilled layer is the outline alone.
	filled ? push!(kw, :mc => _script_rgb(r, g, b)) : push!(kw, :mc => :none)
	evis && push!(kw, :ml => _script_pen(_script_ptstr_w(ew, ctx), _script_rgb(er, eg, eb)))
	return ScriptStep("$(isempty(name) ? "symbols" : name)  (symbols, $sym)", DataBind[bind],
	                  want3d ? :plot3 : :plot, bind.var, kw, true)
end

# An ARROW layer -> plot! with GMT vectors (-Sv), fetched live by `_arrow_layer`. The paper scale is
# the figure's: `figsize` cm across the region, whose width in WORLD units is (x1-x0)*xfac.
function _script_emit_arrows!(ctx::ScriptCtx, sl::SymbolRow)
	x0, x1, _, _ = ctx.region
	xf = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), ctx.scene)
	(isfinite(xf) && xf != 0.0) || (xf = 1.0)
	x1 > x0 || return nothing
	cm_per_unit = ctx.figsize / ((x1 - x0) * xf)
	want3d = ctx.view !== nothing && _script_has_z(filter(!isempty, [sl.xyz]))
	ds = _arrow_layer(ctx.scene, sl.name, cm_per_unit, want3d)
	isempty(ds) && return nothing
	var = _script_var!(ctx, "D", length(ds) == 1 ? ds[1] : ds)
	ctx.needs_base64 = true          # the script reaches into the window -> it needs InteractiveGMT
	expr = "InteractiveGMT._arrow_layer(Ptr{Nothing}(UInt(" * string(UInt(ctx.scene)) * ")), " *
	       "$(_lit(sl.name)), $(cm_per_unit), $(_lit(want3d)))"
	bind = DataBind(var, :live, expr)
	# Head: filled, as the solid arrow's cone — 0.3 of the LONGEST arrow, shrunk in proportion on the
	# shorter ones (+n at that longest length), so every arrow keeps the solid glyph's proportions.
	L = sl.worldSize * cm_per_unit
	kw = Pair{Symbol,Any}[:S => "v$(round(0.3L, sigdigits=3))c+e+a40+h0.5+n$(round(L, sigdigits=3))c"]
	name = isempty(sl.name) ? "arrows" : sl.name
	return ScriptStep("$name  (vector field)", DataBind[bind], want3d ? :plot3 : :plot, bind.var, kw, true)
end

# One user-drawn polygon/polyline/rect/circle -> plot!. Closed shapes carry their fill colour and
# opacity; open ones are a pen only. Read from the SAME `gmtvtk_serialize_polys` blob Save Session
# reads, so the two can never disagree about what was drawn.
function _script_emit_poly!(ctx::ScriptCtx, line::String)
	p = split(line, ';'; limit=13)
	length(p) < 13 && return nothing
	closed = parse(Int, p[1]) != 0
	lr, lg, lb = parse(Float64, p[3]), parse(Float64, p[4]), parse(Float64, p[5])
	lw = parse(Float64, p[6])
	fr, fg, fb, fop = parse(Float64, p[8]), parse(Float64, p[9]), parse(Float64, p[10]), parse(Float64, p[11])
	name = String(p[12])
	M = _script_verts(String(p[13]))
	isempty(M) && return nothing
	want3d = ctx.view !== nothing && any(!iszero, view(M, :, 3))
	bind = _script_bind_live_vector!(ctx, "_poly_layer", name, want3d, [GMT.mat2ds(want3d ? M : M[:, 1:2])])
	kw = Pair{Symbol,Any}[:pen => _script_pen(_script_ptstr_w(lw, ctx), _script_rgb(lr, lg, lb))]
	if closed && fop > 0
		push!(kw, :fill => _script_rgb(fr, fg, fb))
		# GMT expresses this as -t TRANSPARENCY (percent), the complement of the viewer's fill opacity.
		# It applies to the whole layer, not the fill alone — the closest GMT has, noted as such.
		fop < 1 && push!(kw, :alpha => round((1 - fop) * 100; digits=1))
	end
	return ScriptStep("$(isempty(name) ? "polygon" : name)  (drawn)", DataBind[bind],
	                  want3d ? :plot3 : :plot, bind.var, kw, true)
end

# The window's text labels -> text! calls, GROUPED by (size, colour): a batch of fifty city names is
# one call per style, not fifty calls. Same blob Save Session rebuilds its labels from.
function _script_emit_texts!(ctx::ScriptCtx, blob::String, skip::String = "")
	groups = Dict{Tuple{Int,String},Vector{Tuple{Float64,Float64,String}}}()
	order = Tuple{Int,String}[]
	for t in _parse_texts_blob(blob)
		(isempty(skip) || t.group != skip) || continue     # labels another call draws (contours)
		key = (t.size, _script_rgb(t.r, t.g, t.b))
		haskey(groups, key) || push!(order, key)
		push!(get!(() -> Tuple{Float64,Float64,String}[], groups, key), (t.x, t.y, t.text))
	end
	steps = ScriptStep[]
	for key in order
		items = groups[key]
		M = Matrix{Float64}(undef, length(items), 2)
		txt = Vector{String}(undef, length(items))
		for (i, it) in enumerate(items)
			M[i, 1] = it[1]; M[i, 2] = it[2]; txt[i] = it[3]
		end
		D = GMT.mat2ds(M; text=txt)
		bind = _script_bind_live_text!(ctx, key[1], key[2], [D], skip)
		kw = Pair{Symbol,Any}[:font => (key[1], "Helvetica", key[2])]
		push!(steps, ScriptStep("text labels ($(key[1]) pt)", DataBind[bind], :text, bind.var, kw, true))
	end
	return steps
end

# coast/borders/rivers are the ONE vector layer that must NOT be exported as captured geometry: GMT
# owns the same shoreline database, so the script asks for the feature by name and resolution (the
# recipe's own request string, which is already the full reproduction recipe) and gets a proper
# coastline instead of a frozen polyline. The live overlay is then skipped by name, or the layer
# would be drawn twice.
function _script_emit_geography(ctx::ScriptCtx, r::ElementRecipe)
	p = split(get(r.params, "req", ""), '/')
	length(p) >= 2 || return nothing
	# "borders:<class>" / "rivers:<class>": the GMT -N / -I class rides after the colon.
	full = String(p[1]); res = Symbol(strip(String(p[2])))
	kind, cls = occursin(':', full) ? String.(split(full, ':'; limit = 2)) : (full, "1")
	kind in ("coast", "borders", "rivers") || return nothing     # point layers land as symbol layers
	typ = something(tryparse(Int, cls), cls)
	# The pen the layer has ON SCREEN NOW, read by the session's own pen capture (a Line-properties
	# edit included); a reloaded recipe's saved pen when the overlay is not live. `pen_w` is the actor's
	# width in screen PIXELS — it goes through the line-width px->pt relation like every other line, or
	# a 1 pt coastline comes out dpi/72 times too thick on paper.
	r = _session_capture_overlay_pen(ctx.scene, r)
	getp(k, d) = (v = get(r.params, k, nothing); v === nothing ? d : parse(Float64, string(v)))
	pen = _script_pen(_script_ptstr_w(getp("pen_w", 1.0), ctx),
	                  _script_rgb(getp("pen_r", 0.0), getp("pen_g", 0.0), getp("pen_b", 0.0)),
	                  _script_linestyle(round(Int, getp("pen_style", 0.0))))
	kw = Pair{Symbol,Any}[:res => res]
	kind == "coast"   && push!(kw, :shore => pen)
	kind == "borders" && push!(kw, :borders => (type=typ, pen=pen))
	kind == "rivers"  && push!(kw, :rivers => (type=typ, pen=pen))
	return ScriptStep("$(r.name)  (geography, $kind)", DataBind[], :coast, nothing, kw, true)
end

# ── P3: faults, slip models, curtains, focal ─────────────────────────────────────────────────
# All four read the SAME `gmtvtk_serialize_faults` blob the session rebuilds from (plus, for focal,
# the per-window meca table stashed at plot time). Nothing here recomputes geometry the viewer
# already holds: the fault PLANE arrives as the quad read off its actor (tag `P`), never re-walked
# from strike/dip/width in Julia — that geodesic walk lives in updateFaultPlane and must stay there.
function _script_emit_faults!(ctx::ScriptCtx, h::Ptr{Cvoid})
	steps = ScriptStep[]
	slipgroups = String[]
	slippatch = Dict{String,Vector{Tuple{Matrix{Float64},String}}}()
	for line in split(_serialize_faults_raw(h), '\n'; keepempty=false)
		if startswith(line, "F;")
			p = split(line, ';'; limit=10)
			length(p) < 10 && continue
			name = String(p[9])
			M = _script_verts2(String(p[10]))
			size(M, 1) < 2 && continue
			bind = _script_bind_live_vector!(ctx, "_fault_layer", name, false, [GMT.mat2ds(M)])
			push!(steps, ScriptStep("$(isempty(name) ? "fault" : name)  (fault trace)", DataBind[bind],
			                        :plot, bind.var, Pair{Symbol,Any}[:pen => "1.5p,255/0/0"], true))
		elseif startswith(line, "P;")
			p = split(line, ';'; limit=3)
			length(p) < 3 && continue
			name = String(p[2])
			M = _script_verts(String(p[3]))
			size(M, 1) < 3 && continue
			# The plane is a 3-D body: it only means anything under a tilted view, and it is drawn as a
			# filled patch there. A top-down figure gets its surface projection (the trace already
			# emitted above), not a degenerate flat quad.
			ctx.view === nothing && continue
			bind = _script_bind_live_vector!(ctx, "_fault_plane", name, false, [GMT.mat2ds(M)])
			push!(steps, ScriptStep("$(isempty(name) ? "fault" : name)  (fault plane, 3-D)", DataBind[bind],
			                        :plot3, bind.var,
			                        Pair{Symbol,Any}[:fill => "160/160/160", :alpha => 30.0,
			                                         :pen => "0.5p,60/60/60"], true))
		elseif startswith(line, "S;")
			p = split(line, ';'; limit=12)
			length(p) < 12 && continue
			g = String(p[2])
			M = _script_verts2(String(p[12]))
			size(M, 1) < 3 && continue
			fc = split(p[11], ',')
			length(fc) < 3 && continue
			fill = _script_rgb(parse(Float64, fc[1]), parse(Float64, fc[2]), parse(Float64, fc[3]))
			g in slipgroups || push!(slipgroups, g)
			push!(get!(() -> Tuple{Matrix{Float64},String}[], slippatch, g), (M, fill))
		end
	end
	# One call per slip MODEL, not per patch: every patch becomes a segment carrying its own -G fill in
	# its header, which is how GMT colours a multisegment polygon file. A 200-patch model is one line
	# of script.
	for g in slipgroups
		ps = slippatch[g]
		ds = [GMT.mat2ds(M) for (M, _) in ps]
		for (k, d) in enumerate(ds)
			d.header = " -G" * ps[k][2]
		end
		bind = _script_bind_live_vector!(ctx, "_slip_layer", g, false, ds)
		push!(steps, ScriptStep("$(isempty(g) ? "slip model" : g)  (slip patches, $(length(ds)))",
		                        DataBind[bind], :plot, bind.var,
		                        Pair{Symbol,Any}[:pen => "0.25p,0/0/0"], true))
	end
	return steps
end

# "x,y|x,y" -> an n×2 matrix (the fault/slip blobs carry 2-D vertices; the plane tag carries 3-D).
function _script_verts2(blob::String)
	rows = split(blob, '|'; keepempty=false)
	M = Matrix{Float64}(undef, length(rows), 2)
	k = 0
	for row in rows
		c = split(row, ',')
		length(c) < 2 && continue
		x = tryparse(Float64, c[1]); y = tryparse(Float64, c[2])
		(x === nothing || y === nothing) && continue
		k += 1
		M[k, 1] = x; M[k, 2] = y
	end
	return k == 0 ? Matrix{Float64}(undef, 0, 2) : M[1:k, :]
end

# A vertical curtain: GMT has no draped-image-on-a-vertical-sheet, so what CAN be reproduced is the
# sheet's OUTLINE in 3-D — the track at the top of the z range, back along the bottom, closed. The
# texture is the part that is genuinely not representable, and it is said so in the notes rather than
# quietly dropped.
function _script_emit_curtain!(ctx::ScriptCtx, r::ElementRecipe)
	toks = split(get(r.params, "track", ""), '|'; keepempty=false)
	length(toks) < 2 && return nothing
	xy = Matrix{Float64}(undef, length(toks), 2)
	for (i, t) in enumerate(toks)
		c = split(t, ',')
		length(c) < 2 && return nothing
		xy[i, 1] = parse(Float64, c[1]); xy[i, 2] = parse(Float64, c[2])
	end
	zmn = parse(Float64, get(r.params, "zmin", "0")); zmx = parse(Float64, get(r.params, "zmax", "0"))
	n = size(xy, 1)
	M = Matrix{Float64}(undef, 2n + 1, 3)
	for i in 1:n
		M[i, 1] = xy[i, 1]; M[i, 2] = xy[i, 2]; M[i, 3] = zmx
		M[n + i, 1] = xy[n + 1 - i, 1]; M[n + i, 2] = xy[n + 1 - i, 2]; M[n + i, 3] = zmn
	end
	M[2n + 1, :] = M[1, :]
	push!(ctx.notes, "curtain texture NOT exported (GMT has no vertical image sheet); its outline is drawn instead")
	ctx.view === nothing && return nothing        # a vertical sheet is invisible from straight above
	ctx.ncurtain += 1
	bind = _script_bind_live_curtain!(ctx, ctx.ncurtain, [GMT.mat2ds(M)])
	return ScriptStep("curtain outline (texture not exported)", DataBind[bind], :plot3, bind.var,
	                  Pair{Symbol,Any}[:pen => "0.75p,80/80/80"], true)
end

# Focal mechanisms -> a real `meca!`. The stashed table is GMT's own Aki & Richards column order
# (lon lat depth strike dip rake mag), so GMT draws proper beachballs instead of the export freezing
# the viewer's patches into anonymous coloured polygons.
function _script_emit_meca!(ctx::ScriptCtx, h::Ptr{Cvoid})
	steps = ScriptStep[]
	for (name, M) in get(_MECA_TABLE, h, Tuple{String,Matrix{Float64}}[])
		isempty(M) && continue
		bind = _script_bind_live_vector!(ctx, "_meca_layer", name, false, [GMT.mat2ds(M)])
		# `proj` MUST be repeated here. GMT.jl reads psmeca's `scale` as a figure-scale change, and an
		# overlay that changes the scale without restating the projection is rejected outright
		# ("In Overlay mode you cannot change a fig scale and NOT repeat the projection").
		push!(steps, ScriptStep("$(isempty(name) ? "focal mechanisms" : name)  (meca, $(size(M,1)) events)",
		                        DataBind[bind], :meca, bind.var,
		                        Pair{Symbol,Any}[:aki => true, :scale => "0.4c",
		                                         :proj => ScriptVar(:PROJ, ctx.proj)], true))
	end
	return steps
end

# ── contours: ONE grdcontour on the grid they were traced from ───────────────────────────────
# The Contours tool (contours.jl) traces with GDAL and draws each level as a line, with flat labels
# sitting in gaps the viewer cuts. Replaying those pieces (a plot! per level + text!) loses what GMT
# does itself: labels turned along the line and the line broken under each one. So the script hands
# the SOURCE GRID — the in-memory object, bound like any other layer — to `grdcontour` with the same
# levels, labels and Min pts, and the traced lines and their labels are not replayed. Levels whose row
# is unchecked are left out; one call per distinct pen (a set coloured by the grid's colormap has one
# per level, an ordinary set one in all).
# Returns (steps, rank) — `rank` places the calls in the vector pile where the lines sat — or
# `nothing` when the window has no contour request on record; its contour rows are then replayed as
# plain overlays and labels.
function _script_emit_contours!(ctx::ScriptCtx, h::Ptr{Cvoid}, ovs::Vector{OverlayRow},
                                texts::Vector{TextRow}, recipes::Vector{ElementRecipe})
	levels = get(_CONTOUR_DRAWN, h, nothing)
	(levels === nothing || isempty(levels)) && return nothing
	src  = get(_CONTOUR_SRC, h, "")
	opts = get(_CONTOUR_OPTS, h, (labels = false, minpts = 0))
	G = try _contour_grid(h, src) catch; nothing end
	G === nothing && return nothing
	byname = Dict(ov.name => ov for ov in ovs if ov.group == _CONTOUR_GROUP)
	pens = Dict{String,Vector{Float64}}();  order = String[];  rank = typemax(Int)
	for lev in levels
		ov = get(byname, "Contour " * _contour_fmt(lev), nothing)
		(ov === nothing || !ov.visible) && continue
		pen = _script_pen(_script_ptstr_w(ov.lw, ctx), _script_rgb(ov.r, ov.g, ov.b), _script_linestyle(ov.lstyle))
		haskey(pens, pen) || push!(order, pen)
		push!(get!(() -> Float64[], pens, pen), lev)
		rank = min(rank, ov.stack)
	end
	isempty(order) && return (ScriptStep[], 0)          # every level unchecked: nothing to draw
	# The grid: the recipe that put it in the window when there is one (so a file-backed grid is read
	# from its file), else the live object, fetched by the very resolver the tool traced it through.
	gbind = nothing
	for r in recipes
		_script_israster(r) || continue
		_find_object(h, _script_obj_kind(r), r.name) === G || continue
		gbind = _script_bind!(ctx, r, G);  break
	end
	if gbind === nothing
		ctx.needs_base64 = true
		gbind = DataBind(_script_var!(ctx, "G", G), :live,
		                 "InteractiveGMT._contour_grid(Ptr{Nothing}(UInt(" * string(UInt(ctx.scene)) * ")), $(_lit(src)))")
	end
	# The labels' own style, as the viewer drew them.
	lab = findfirst(t -> t.group == _CONTOUR_GROUP, texts)
	labels = opts.labels && lab !== nothing
	font = labels ? "$(texts[lab].size)p,Helvetica,$(_script_rgb(texts[lab].r, texts[lab].g, texts[lab].b))" : ""
	steps = ScriptStep[]
	for pen in order
		lv = pens[pen]
		lst = join(_contour_fmt.(lv), ",") * (length(lv) == 1 ? "," : "")   # a single level needs the ","
		kw = Pair{Symbol,Any}[:cont => lst, :annot => labels ? lst * "+f" * font : "-", :pen => pen]
		opts.minpts > 0 && push!(kw, :cut => opts.minpts)
		push!(steps, ScriptStep("contours $(join(_contour_fmt.(lv), " "))  (grdcontour on the grid they were traced from)",
		                        isempty(steps) ? DataBind[gbind] : DataBind[], :grdcontour, gbind.var, kw, true))
	end
	return steps, rank
end

# Every vector element of the window, in the shared draw-order pile's order. GMT has NO depth buffer:
# under -p the call order IS the occlusion, so this sorts by the `stack` rank the viewer itself
# assigns rather than by whatever order the serializers happened to walk. Elements without a rank
# (drawn polygons, text) keep their scene order and follow, which is where they already sit.
function _script_vector_steps!(ctx::ScriptCtx, h::Ptr{Cvoid}, recipes::Vector{ElementRecipe})
	ranked = Tuple{Int,ScriptStep}[]
	skip = Dict{String,Int}()                       # overlay name -> its stack, for layers GMT redraws
	geosteps = Tuple{String,ScriptStep}[]
	for r in recipes
		r.kind === :geography || continue
		st = _script_emit_geography(ctx, r)
		st === nothing && continue
		skip[r.name] = 0
		push!(geosteps, (r.name, st))
	end
	ovs   = _parse_overlays_blob(_script_blob(:gmtvtk_serialize_overlays, h))
	tblob = _serialize_texts_raw(h; groups=true)
	# Contours go to grdcontour as ONE request; their traced lines and labels are then not replayed.
	cont  = _script_emit_contours!(ctx, h, ovs, _parse_texts_blob(tblob), recipes)
	cskip = cont === nothing ? "" : _CONTOUR_GROUP
	if cont !== nothing
		for st in cont[1]
			push!(ranked, (cont[2], st))
		end
	end
	for ov in ovs
		ov.visible || continue                       # unchecked row: not on screen, not in the script
		(!isempty(cskip) && ov.group == cskip) && continue
		if haskey(skip, ov.name)                     # a coastline GMT will redraw itself
			skip[ov.name] = ov.stack
			continue
		end
		st = _script_emit_overlay!(ctx, ov)
		st === nothing || push!(ranked, (ov.stack, st))
	end
	for (nm, st) in geosteps
		push!(ranked, (get(skip, nm, 0), st))
	end
	for sl in _parse_symbols_blob(_script_blob(:gmtvtk_serialize_symbols, h))
		sl.visible || continue                       # unchecked row
		st = isempty(sl.uv) ? _script_emit_symbols!(ctx, sl, filter(!isempty, [sl.xyz])) :
		                      _script_emit_arrows!(ctx, sl)
		st === nothing || push!(ranked, (sl.stack, st))
	end
	sort!(ranked; by = t -> t[1], alg = MergeSort)   # stable: equal ranks keep their scene order
	steps = ScriptStep[t[2] for t in ranked]
	# Drawn polygons and text carry no rank in their (session-shared, format-frozen) blobs, so they
	# follow in scene order — which is where the viewer draws them anyway.
	for line in split(_serialize_polys_raw(h), '\n'; keepempty=false)
		st = _script_emit_poly!(ctx, String(line))
		st === nothing || push!(steps, st)
	end
	# P3: faults + slip models + curtains + beachballs, then labels on top of everything.
	append!(steps, _script_emit_faults!(ctx, h))
	for r in recipes
		r.kind === :curtain || continue
		st = _script_emit_curtain!(ctx, r)
		st === nothing || push!(steps, st)
	end
	append!(steps, _script_emit_meca!(ctx, h))
	# `groups=true`: EVERY label on screen, batch ones included — see `_text_labels`.
	append!(steps, _script_emit_texts!(ctx, tblob, cskip))
	return steps
end

# ── the emitter ──────────────────────────────────────────────────────────────────────────────
# ── which raster layers are actually SEEN ────────────────────────────────────────────────────
# A raster recipe is only worth a GMT call if the window shows it. Two ways it does not: its row is
# unchecked, or — looking straight down — a layer higher in the pile covers its whole extent with
# opaque data. A nested grid on top of layer 0, or a grid of the same limits on top, IS seen and stays;
# what a covering layer buries is left out. The layer list is the viewer's own (`swipeLayers`, through
# `gmtvtk_raster_layers_h`): pile rank, checkbox, opacity and true extent, never a guess from here.
struct _ScriptLayer
	base::Bool
	rank::Int
	vis::Bool
	opacity::Float64
	box::NTuple{4,Float64}
	name::String
end

function _script_raster_layers(h::Ptr{Cvoid})::Vector{_ScriptLayer}
	n = ccall(_fn(:gmtvtk_raster_layers_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
	n <= 0 && return _ScriptLayer[]
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_raster_layers_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
	out = _ScriptLayer[]
	for line in split(String(buf[1:n]), '\n'; keepempty=false)
		p = split(line, '\t'; limit=9)
		length(p) < 9 && continue
		b = parse.(Float64, p[5:8])
		push!(out, _ScriptLayer(p[1] == "1", parse(Int, p[2]), p[3] == "1", parse(Float64, p[4]),
		                        (b[1], b[2], b[3], b[4]), String(p[9])))
	end
	return out
end

# The layer a recipe drew: an extra by its own name; the window's base for a base recipe. `nothing`
# when the window has no such layer — the recipe is then emitted as before, never dropped on a guess.
function _script_layer_of(layers::Vector{_ScriptLayer}, r::ElementRecipe)
	k = findfirst(L -> !L.base && L.name == r.name, layers)
	k === nothing && r.kind in (:basegrid, :image) && (k = findfirst(L -> L.base, layers))
	return k
end

# Does layer T's own data leave no gap anywhere over `box`? A grid's NaN nodes are holes in its mesh
# (the layer below shows through them), so ONE NaN within a cell of `box` means "not opaque". An
# image is opaque unless it carries alpha. Anything not found or of another kind: not opaque.
function _script_opaque_over(h::Ptr{Cvoid}, T::_ScriptLayer, box::NTuple{4,Float64})::Bool
	T.opacity < 1.0 && return false
	if (G = _find_object_exact(h, :grid, T.name)) isa GMTgrid
		nx, ny = _grid_dims(G)
		half = G.registration == 1 ? 0.5 : 0.0
		dx, dy = G.inc[1], G.inc[2]
		ix = [i for i in 1:nx if box[1] - dx <= G.range[1] + (i - 1 + half) * dx <= box[2] + dx]
		iy = [j for j in 1:ny if box[3] - dy <= G.range[3] + (j - 1 + half) * dy <= box[4] + dy]
		(isempty(ix) || isempty(iy)) && return false
		Z = _zmat(G)                                  # (ny,nx), row 1 = south
		return !any(isnan, view(Z, iy, ix))
	end
	if (I = _find_object_exact(h, :image, T.name)) isa GMTimage
		return size(I.image, 3) in (1, 3) && isempty(I.alpha)
	end
	return false
end

# Is layer k entirely buried under a visible layer above it in the pile? Pile ties go to the later
# layer, the viewer's own rule (swipeCurrentIndex / resolveActiveGrid).
function _script_layer_covered(h::Ptr{Cvoid}, layers::Vector{_ScriptLayer}, k::Int)::Bool
	L = layers[k]
	tol = 1e-6 * max(L.box[2] - L.box[1], L.box[4] - L.box[3], 1e-12)
	for (j, T) in enumerate(layers)
		j == k && continue
		(T.vis && (T.rank > L.rank || (T.rank == L.rank && j > k))) || continue
		(T.box[1] <= L.box[1] + tol && T.box[2] >= L.box[2] - tol &&
		 T.box[3] <= L.box[3] + tol && T.box[4] >= L.box[4] - tol) || continue
		_script_opaque_over(h, T, L.box) && return true
	end
	return false
end

"""
	_script_emit(h; figsize=15.0, recompute=false)

Walk the window's provenance recipes (`_SESSION_LOG`) and produce the ordered call list plus the
resolved figure-wide context. Pure: nothing is written and the window is not touched, except for a
T3 capture, which reads the render window.
"""
function _script_emit(h::Ptr{Cvoid}; figsize::Real=15.0, recompute::Bool=false, backdrop::Bool=false)
	st, stf = _script_state(h)
	get(st, "alive", 0) == 1 || error("gmtscript: that window is closed")
	crs = _window_crs(h)
	geog = occursin("longlat", crs.proj4) || (isempty(crs.proj4) && get(st, "crs", 0) == 1)
	ctx = ScriptCtx(h, (Float64(get(st, "x0", 0)), Float64(get(st, "x1", 0)),
	                    Float64(get(st, "y0", 0)), Float64(get(st, "y1", 0))),
	                geog, _script_proj(geog), Float64(figsize), nothing, nothing,
	                recompute, Dict{String,Int}(), String[], false, 0, 96.0)
	ctx.dpi = _script_dpi(h, stf)
	ctx.view  = _script_view(st, stf)
	ctx.zsize = _script_zsize(ctx, st, stf)
	# The globe and the cube are 3-D BODIES, not maps, and GMT has no projection for either — so the
	# whole window is captured and drawn as one image (see `_script_body_view!`). Checked BEFORE the
	# backdrop branch below because it IS the backdrop for these two modes, correctly framed: that
	# branch would put a lon/lat frame around a picture of a globe. `viewmode` is the four-state
	# (0 = 3-D, 1 = flat 2-D, 2 = globe, 3 = cube); a library that predates the key still answers
	# through the `flat2d` it has always written.
	vm = Int(get(stf, "viewmode", get(st, "flat2d", 0) == 1 ? 1 : 0))
	vm >= 2 && return _script_body_view!(ctx, vm == 3), ctx
	# The guaranteed floor: ONE capture of the whole window, placed under a real frame. Not a
	# reproduction and never presented as one — but a window made entirely of things GMT cannot draw
	# (a curtain-heavy 3-D scene) would otherwise export to nothing at all, and an honest picture beats
	# an empty script. Uses the same `_capture_rect_image` every other rasterize path uses.
	if backdrop
		x0, x1, y0, y1 = ctx.region
		I = _capture_rect_image(h, x0, x1, y0, y1; coords=true)
		var = _script_var!(ctx, "I", I)
		ctx.needs_base64 = true                        # the script asks the live window for the pixels
		bind = DataBind(var, :capture, _script_capture_call(h, x0, x1, y0, y1))
		push!(ctx.notes, "BACKDROP MODE: this is a PICTURE of the window, not a reproduction — nothing in it is editable or re-projectable")
		kw = Pair{Symbol,Any}[:region => ScriptVar(:REG, ctx.region), :proj => ScriptVar(:PROJ, ctx.proj),
		                      :figsize => ScriptVar(:FIGSIZE, ctx.figsize)]
		append!(kw, _script_frame_kw(h, "", false))
		return ScriptStep[ScriptStep("whole-window capture (backdrop mode)", DataBind[bind],
		                             :grdimage, var, kw, false)], ctx
	end
	steps = ScriptStep[]
	seen = Set{Tuple{Symbol,String}}()
	lastcpt = nothing
	recipes = get(_SESSION_LOG, h, ElementRecipe[])
	layers  = _script_raster_layers(h)
	# Covering only proves anything looking STRAIGHT DOWN: under a tilted view a buried layer's relief
	# can stand above the one stacked over it.
	topdown = ctx.view === nothing || ctx.view[2] >= 90.0
	for r in recipes
		if _script_israster(r)
			k = _script_layer_of(layers, r)
			k === nothing || (layers[k].vis && !(topdown && _script_layer_covered(h, layers, k))) || continue
		end
		if !_script_israster(r)
			# These are all emitted by the vector pass below — :geography as a `coast!` call (its point
			# layers as the symbol layers they became), :curtain as its 3-D outline, :focal as a real
			# `meca!` from the stashed catalog — so none of them is a gap.
			# :contour is `grdcontour` on its grid (`_script_emit_contours!`).
			r.kind in (:geography, :curtain, :focal, :contour) ||
				push!(ctx.notes, "NOT EXPORTED: $(r.kind)$(isempty(r.name) ? "" : " '" * r.name * "'")")
			continue
		end
		# One Scene Objects element is one (kind,name); a re-logged recipe must not emit twice. Same
		# dedup rule, for the same reason, as Save Session's generated-data packing.
		key = (r.kind, r.name)
		key in seen && continue
		push!(seen, key)
		obj = _find_object(h, _script_obj_kind(r), r.name)
		step = _script_emit_raster!(ctx, r, obj, isempty(steps))
		push!(steps, step)
		for b in step.binds
			b.tier === :cpt && (lastcpt = b.var)
		end
	end
	isempty(steps) && error("gmtscript: this window has no exportable layer yet")
	# Vectors always follow every raster (they are drawn ON TOP of them), then the colour bar last so
	# nothing paints over it.
	append!(steps, _script_vector_steps!(ctx, h, recipes))
	(get(st, "bar", 0) == 1 && lastcpt !== nothing) && push!(steps, _script_emit_colorbar(ctx, lastcpt))
	return steps, ctx
end

# ── sink 1: run it now, against the live objects ─────────────────────────────────────────────
# Resolve one rendered value to something GMT.jl can be handed. A ScriptVar standing for a layer
# already carries the live object; a CPT variable is built by CALLING makecpt with the same kwargs
# the text rendered, so the two sinks cannot drift (and nothing is eval'd).
_script_resolve(v, cpts::Dict{Symbol,Any}) =
	v isa ScriptVar ? (v.value === nothing ? get(cpts, v.name, nothing) : v.value) : v
# A kwarg can be a TUPLE holding one — `figsize=(FIGSIZE, 0)`, the "-JX<w>/0" the globe/cube capture
# is placed with. The text sink already renders that through `_lit(::Tuple)`; resolving elementwise
# is the live sink's half of the same thing, so the two sinks stay one emitter. Plain tuples
# (`view=(az, el)`) pass through unchanged.
_script_resolve(v::Tuple, cpts::Dict{Symbol,Any}) = map(x -> _script_resolve(x, cpts), v)

"""
	gmtreplay(fig; figsize=15, recompute=false) -> the GMT figure

Reproduce the window's display as GMT.jl calls, executed right now against the LIVE grid/image
objects — no files are written anywhere. Returns the last call's result and shows the figure.
"""
function gmtreplay(fig; figsize::Real=15.0, recompute::Bool=false, backdrop::Bool=false)
	h = _fig_handle(fig)
	steps, ctx = _script_emit(h; figsize=figsize, recompute=recompute, backdrop=backdrop)
	cpts = Dict{Symbol,Any}()
	out = nothing
	for s in steps
		for b in s.binds
			b.mk === nothing || (cpts[b.var.name] = GMT.makecpt(; b.mk...))
		end
		kwargs = Pair{Symbol,Any}[k => _script_resolve(v, cpts) for (k, v) in s.kw]
		f = getfield(GMT, s.bang ? Symbol(s.mod, :!) : s.mod)
		posv = _script_resolve(s.pos, cpts)
		out = posv === nothing ? f(; kwargs...) : f(posv; kwargs...)
	end
	GMT.showfig()
	return out
end

# ── sink 2: a standalone .jl script ──────────────────────────────────────────────────────────
# Render one call: `mod[!](pos, k=v, …)`.
function _script_render_call(s::ScriptStep)
	name = String(s.mod) * (s.bang ? "!" : "")
	# A name iGMT gives its OWN function (plot, plot!, feather, viz: gmtsyntax.jl) is written GMT.name: the
	# script draws GMT.jl's static figure, and runs where `using InteractiveGMT` is loaded too.
	sym = Symbol(name)
	isdefined(@__MODULE__, sym) && getfield(@__MODULE__, sym) !== getfield(GMT, sym) && (name = "GMT." * name)
	args = String[]
	s.pos === nothing || push!(args, _lit(s.pos))
	for (k, v) in s.kw
		push!(args, "$(k)=$(_lit(v))")
	end
	return name * "(" * join(args, ", ") * ")"
end

# Every pen of a call -> its own `penN` variable, declared on the line just above it, so a user changes
# a line's width/colour where it is used. Text sink only: the live sink keeps the literal strings.
# Pen-carrying kwargs: `pen` (plot/grdcontour), `shore` (coast), `ml` (symbol outline), and the `pen`
# field of coast's `borders`/`rivers` NamedTuples.
const _SCRIPT_PEN_KEYS = (:pen, :shore, :ml)
function _script_hoist_pens(s::ScriptStep, n0::Int)
	pens = Pair{String,String}[]
	newvar(p) = (v = "pen$(n0 + length(pens) + 1)"; push!(pens, v => p); ScriptVar(Symbol(v), p))
	kw = Pair{Symbol,Any}[]
	for (k, v) in s.kw
		if k in _SCRIPT_PEN_KEYS && v isa String
			v = newvar(v)
		elseif v isa NamedTuple && haskey(v, :pen) && v.pen isa String
			v = merge(v, (pen = newvar(v.pen),))
		end
		push!(kw, k => v)
	end
	return ScriptStep(s.comment, s.binds, s.mod, s.pos, kw, s.bang), pens
end

# The header: what produced this, and — just as important — what it does NOT reproduce. Every
# fidelity limit is stated in the file itself, so a script that came out of a shaded, curtained
# window can never be mistaken for a faithful copy of it.
function _script_header(ctx::ScriptCtx)
	io = IOBuffer()
	println(io, "# Generated by InteractiveGMT — reproduction of an iGMT window as GMT.jl calls.")
	println(io, "# ", Libc.strftime("%Y-%m-%dT%H:%M:%S", time()))
	println(io, "#")
	println(io, "# Fidelity: layer set, order, region, CPT and geometry are exact. A layer whose LOOK is the")
	println(io, "# viewer's rather than GMT's (shading, a drape, a composited image) is drawn from the pixels")
	println(io, "# that were on screen — captured as an image and, in 3-D, draped over its own grid — so the")
	println(io, "# figure matches the window; those layers are listed below and are not re-projectable.")
	println(io, "# Colour-bar placement and, for a tilted view, the -p azimuth/elevation approximate the")
	println(io, "# live camera.")
	if !isempty(ctx.notes)
		println(io, "#")
		println(io, "# ", length(ctx.notes), " thing(s) this script does NOT reproduce:")
		for n in ctx.notes
			println(io, "#   - ", n)
		end
	end
	println(io)
	println(io, "using GMT")
	ctx.needs_base64 && println(io, "using InteractiveGMT   # a displayed image is taken from the live window")
	println(io)
	# The knobs, hoisted: change the region, projection or plot width in ONE place and every call
	# below follows, which is the difference between a script you can edit and a transcript.
	println(io, "const REG     = ", _lit(ctx.region))
	println(io, "const PROJ    = ", _lit(ctx.proj))
	println(io, "const FIGSIZE = ", _lit(ctx.figsize))
	println(io)
	return String(take!(io))
end

"""
	_script_render_text(steps, ctx) -> String

Render the call list as a standalone script. Pure text: nothing is written anywhere. A layer that
exists only in memory is fetched by the script from the live window, never from a file.
"""
function _script_render_text(steps::Vector{ScriptStep}, ctx::ScriptCtx)
	io = IOBuffer()
	print(io, _script_header(ctx))
	npen = 0
	for s in steps
		println(io, "# ── ", s.comment)
		for b in s.binds
			b.tier === :capture &&
				println(io, "# ", b.var.name, ": the window's displayed pixels, taken from the live ",
				        "window as a GMTimage — nothing embedded, nothing written to disk")
			b.tier === :live &&
				println(io, "# ", b.var.name, ": exists only in the viewer, never on disk — fetched from ",
				        "the live window")
			println(io, String(b.var.name), " = ", b.expr)
		end
		s, pens = _script_hoist_pens(s, npen)
		for (v, p) in pens
			println(io, v, " = ", _lit(p), "    # pen: width,colour[,style] — edit here")
		end
		npen += length(pens)
		println(io, _script_render_call(s))
	end
	println(io)
	println(io, "showfig()")
	return _script_space_comments(String(take!(io)))
end

# Every comment that opens a block starts after an empty line, so the script reads as groups of
# commands, each under its own heading. Done once, on the finished text, so no emitter has to
# remember it; consecutive comment lines (the header, a step heading and its layer note) stay together.
function _script_space_comments(txt::String)::String
	out = String[]
	for l in split(txt, '\n')
		if startswith(lstrip(l), '#') && !isempty(out)
			prev = strip(out[end])
			(isempty(prev) || startswith(prev, '#')) || push!(out, "")
		end
		push!(out, String(l))
	end
	return join(out, '\n')
end

"""
	gmtscript(fig; path="", figsize=15, recompute=false) -> String

Reproduce the window's display as a GMT.jl script and return its text. With `path`, the script — and
nothing else — is written there. A layer that exists only in memory is fetched by the script from the
live window, so such a script runs while that window is open.

`recompute=true` emits the recorded GMT command for a computed grid instead of its data, which makes
the script editable — see docs/GMTSCRIPT_PLAN.md, tier T1. It is skipped, with a note, for any grid
whose command references files that no longer exist.

`backdrop=true` is the escape hatch: one capture of the whole window under a real frame, for a
display made of things GMT cannot draw. The script says plainly that it is a picture.
"""
function gmtscript(fig; path::String="", figsize::Real=15.0, recompute::Bool=false,
                   backdrop::Bool=false)
	h = _fig_handle(fig)
	steps, ctx = _script_emit(h; figsize=figsize, recompute=recompute, backdrop=backdrop)
	txt = _script_render_text(steps, ctx)
	isempty(path) || write(String(path), txt)                       # the caller's own file, nothing beside it
	return txt
end

# ── File > Plot with GMT.jl… (the editor dialog) ─────────────────────────────────────────────
# The menu opens the script in an EDITABLE box with Run and Save…. The text lives IN THE BOX — there is
# no file behind it, no working directory, nothing temporary:
#   * open: Julia builds the text and hands it to the dialog (`gmtvtk_script_editor_show_h`);
#   * Run:  Julia pulls the box's CURRENT text back (`gmtvtk_script_editor_text_h`, two-phase buffer),
#           so what runs is what the user EDITED, and runs it statement by statement;
#   * Save: the C++ side writes the box's text to the file the user picks — that file and nothing else.
# Every layer that exists only in memory is fetched by the script from the live window, so nothing has
# to sit beside it. (This used to be backed by a `mktempdir` that was never deleted plus a
# `script_data/` of sidecars — a temporary-file bridge, banned outright. Do not bring it back.)

"""
	_script_open_editor(scene) -> nothing

Generate the window's script and show it in the window's script editor. Whatever the window could not
reproduce is repeated into the Errors console, not just buried in the script header — a user exporting
a shaded or curtained window should be told while they are still looking at it.
"""
function _script_open_editor(scene::Ptr{Cvoid})::Nothing
	steps, ctx = _script_emit(scene)
	txt = _script_render_text(steps, ctx)
	for n in ctx.notes
		_viewer_log_error(scene, "script: $n")
	end
	ccall(_fn(:gmtvtk_script_editor_show_h), Cint, (Ptr{Cvoid}, Cstring), scene, txt)
	return nothing
end

# The editor box's current text: size query, then fill a Julia-owned buffer.
function _script_editor_text(scene::Ptr{Cvoid})::String
	n = ccall(_fn(:gmtvtk_script_editor_text_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), scene, C_NULL, Cint(0))
	n <= 0 && return ""
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_script_editor_text_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), scene, buf, Cint(n + 1))
	return String(buf[1:n])
end

"""
	_script_run(scene) -> nothing

The dialog's Run: execute the editor's text statement by statement, in `Main`, while a status window
names the statement being executed and how far along the script is. A GMT call can take seconds; the
user sees which one it is, never a dead window.

With `savefig` (the external-editor handoff, craftapps.jl) the script's last `showfig` writes that file
instead of showing the figure — see `_script_handoff!`. Only the statements run are changed, never the
text in the box. `what` heads the status window ("Creating the figure for VectorCraft..."), so a caller
that runs the script on the way to something else says so.
"""
function _script_run(scene::Ptr{Cvoid}; savefig::String = "", dpi::Int = 0, what::String = "")::Nothing
	src   = _script_editor_text(scene)
	lines = split(src, '\n')
	fname = "Plot with GMT.jl"                  # what an error's stack trace names: there is no file
	stmts = Tuple{LineNumberNode,Any}[]
	loc   = LineNumberNode(1, Symbol(fname))
	for a in Meta.parseall(src; filename = fname).args
		a isa LineNumberNode ? (loc = a) : push!(stmts, (loc, a))
	end
	isempty(savefig) || _script_handoff!(stmts, savefig, dpi)
	n = length(stmts)
	ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), Cint(n), isempty(what) ? "Plot with GMT.jl" : what)
	try
		for (k, (loc, ex)) in enumerate(stmts)
			stop = k < n ? stmts[k + 1][1].line - 1 : length(lines)
			ccall(_fn(:gmtvtk_progress_status), Cvoid, (Cint, Cstring), Cint(k - 1),
			      (isempty(what) ? "" : what * "\n") * "Step $k of $n\n" * _script_stmt_text(lines, loc.line, stop))
			Core.eval(Main, Expr(:toplevel, loc, ex))
		end
	finally
		ccall(_fn(:gmtvtk_progress_close), Cvoid, ())
	end
	return nothing
end

_is_showfig(ex) = ex isa Expr && ex.head === :call && (ex.args[1] === :showfig || ex.args[1] == :(GMT.showfig))

# The handoff: the LAST `showfig(…)` statement becomes `showfig(…; show=false, savefig=file[, dpi])` —
# GMT.jl's own way to finish the figure into a file without displaying it — keeping the user's other
# keywords. A script with no showfig gets one appended.
function _script_handoff!(stmts::Vector{Tuple{LineNumberNode,Any}}, savefig::String, dpi::Int)
	isempty(stmts) && error("Plot with GMT.jl: the script editor is empty")
	ours = Any[Expr(:kw, :show, false), Expr(:kw, :savefig, savefig)]
	dpi > 0 && push!(ours, Expr(:kw, :dpi, dpi))
	# A dpi means nothing to a PostScript/EPS file (GMT.jl warns that it was not consumed).
	vec = endswith(lowercase(savefig), ".ps") || endswith(lowercase(savefig), ".eps")
	taken = (:show, :savefig, :figname, :name, (dpi > 0 || vec ? (:dpi, :DPI) : ())...)
	k = findlast(s -> _is_showfig(s[2]), stmts)
	kept = Any[]
	if k !== nothing
		for x in stmts[k][2].args[2:end]
			for y in (x isa Expr && x.head === :parameters ? x.args : Any[x])
				y isa Expr && y.head === :kw && !(y.args[1] in taken) && push!(kept, y)
			end
		end
	end
	call = Expr(:call, GMT.showfig, Expr(:parameters, kept..., ours...))
	k === nothing ? push!(stmts, (stmts[end][1], call)) : (stmts[k] = (stmts[k][1], call))
	return stmts
end

# ── Save with "Standalone" checked: a zip that runs with GMT.jl alone ────────────────────────
# The editor's script reaches into the live window for every layer that exists only in memory
# (`X = InteractiveGMT._find_object(…)`, `_display_image`, `_overlay_layer`, …). A standalone copy
# cannot: it has to carry that data. So the EDITED text is taken as it is and only those fetch lines
# are rewritten — each one is evaluated now, its result goes into the zip under `data/`, and the line
# becomes a `gmtread` of that member. Everything the user edited survives; `using InteractiveGMT` is
# dropped; any other InteractiveGMT reference left in the text is an error, never a silent dependency.
#
# The bytes are made IN MEMORY and the only file written is the zip the user chose — no temporary
# file anywhere (banned). Rasters go through GDAL's in-memory filesystem; datasets through
# `_ds_table_bytes`, because GDAL's vector writer drops a dataset's text column (labels).

# One raster as GeoTIFF bytes, through GDAL's in-memory filesystem (/vsimem/ — RAM, not disk).
function _raster_bytes(obj)::Vector{UInt8}
	p = "/vsimem/igmt_standalone_" * string(rand(UInt64); base=16) * ".tif"
	try
		GMT.gdalwrite(p, obj)
		len = Ref{UInt64}(0)
		ptr = ccall((:VSIGetMemFileBuffer, GMT.Gdal.libgdal), Ptr{UInt8}, (Cstring, Ptr{UInt64}, Cint), p, len, 0)
		ptr == C_NULL && error("GDAL wrote nothing for this layer")
		return copy(unsafe_wrap(Array, ptr, Int(len[])))
	finally
		GMT.Gdal.VSIUnlink(p)
	end
end

# One dataset (or a vector of segments) as a GMT multi-segment ASCII table: a `>` header per segment,
# the numeric columns, then the row's text — what `gmtread` reads back, text column included.
function _ds_table_bytes(D)::Vector{UInt8}
	io = IOBuffer()
	for d in (D isa GMTdataset ? [D] : D)
		println(io, isempty(d.header) ? ">" : "> " * d.header)
		M = d.data
		T = d.text
		for i in axes(M, 1)
			join(io, (string(M[i, j]) for j in axes(M, 2)), '\t')
			(i <= length(T) && !isempty(T[i])) && print(io, '\t', T[i])
			println(io)
		end
	end
	return take!(io)
end

# Is this statement `X = InteractiveGMT.f(…)`? Returns (X, call) or nothing.
function _standalone_fetch(ex)
	(ex isa Expr && ex.head === :(=) && ex.args[1] isa Symbol) || return nothing
	rhs = ex.args[2]
	(rhs isa Expr && rhs.head === :call) || return nothing
	f = rhs.args[1]
	(f isa Expr && f.head === :. && f.args[1] === :InteractiveGMT) || return nothing
	return ex.args[1], rhs
end

_uses_igmt(ex) = ex === :InteractiveGMT || (ex isa Expr && any(_uses_igmt, ex.args))

"""
	_script_save_standalone(scene, dest) -> dest

Write `dest` (a .zip): the editor's script, made independent of InteractiveGMT, plus a `data/` folder
with every layer it used to fetch from the live window. The script depends on GMT.jl only.
"""
function _script_save_standalone(scene::Ptr{Cvoid}, dest::String)::String
	src   = _script_editor_text(scene)
	isempty(src) && error("Plot with GMT.jl: the script editor is empty")
	lines = String.(split(src, '\n'))
	fname = "Plot with GMT.jl"
	stmts = Tuple{Int,Any}[]
	for a in Meta.parseall(src; filename = fname).args
		a isa LineNumberNode ? push!(stmts, (a.line, nothing)) : (stmts[end] = (stmts[end][1], a))
	end
	files = Tuple{String,Vector{UInt8}}[]
	used  = Set{String}()                           # member names taken so far
	drop  = Set{Int}()                              # lines removed from the text
	for (k, (l0, ex)) in enumerate(stmts)
		ex === nothing && continue
		ex isa Expr && ex.head === :error && error("Plot with GMT.jl: line $l0 does not parse: $(ex.args[1])")
		l1 = k < length(stmts) ? stmts[k + 1][1] - 1 : length(lines)
		while l1 > l0 && (isempty(strip(lines[l1])) || startswith(strip(lines[l1]), '#')); l1 -= 1; end
		if ex isa Expr && ex.head === :using && _uses_igmt(ex)
			union!(drop, l0:l1)
			continue
		end
		fx = _standalone_fetch(ex)
		if fx === nothing
			_uses_igmt(ex) && error("Plot with GMT.jl: line $l0 still needs InteractiveGMT, so it cannot be made standalone")
			continue
		end
		var, call = fx
		obj = Core.eval(@__MODULE__, call)         # the very fetch the live script makes
		# The member is named after the element's own Scene Objects name when the fetch carries one
		# (the same naming Save Session uses), else after the script variable.
		nm = something(findfirst(a -> a isa String && !isempty(a), call.args[2:end]), 0)
		stem = nm > 0 ? call.args[nm + 1] : String(var)
		if obj isa GMTgrid || obj isa GMTimage
			id, bytes = _session_sidecar_id(stem, ".tif", used), _raster_bytes(obj)
		elseif obj isa GMTdataset || obj isa AbstractVector{<:GMTdataset}
			id, bytes = _session_sidecar_id(stem, ".txt", used), _ds_table_bytes(obj)
		else
			error("Plot with GMT.jl: line $l0 fetches a $(typeof(obj)), which cannot be saved as data")
		end
		push!(files, ("data/" * id, bytes))
		lines[l0] = "$(var) = gmtread(joinpath(@__DIR__, \"data\", $(repr(id))))"
		# The generated note above it said "fetched from the live window" — no longer true here.
		(l0 > 1 && startswith(lines[l0 - 1], "# $(var):")) &&
			(lines[l0 - 1] = "# $(var): saved with this script (data/$(id)) — it existed only in the iGMT window")
		union!(drop, (l0 + 1):l1)
	end
	out = [l for (i, l) in enumerate(lines) if !(i in drop)]
	script = splitext(basename(dest))[1] * ".jl"
	pushfirst!(files, (script, Vector{UInt8}(codeunits(join(out, '\n')))))
	_zip_write(dest, files)
	return dest
end

# The source of one statement as the status window shows it: its first line, marked "…" when the
# statement runs on, cut to a width a dialog label can hold.
function _script_stmt_text(lines::Vector{SubString{String}}, i0::Int, i1::Int)::String
	body = String[String(strip(lines[i])) for i in i0:min(i1, length(lines))
	              if !isempty(strip(lines[i])) && !startswith(strip(lines[i]), '#')]
	isempty(body) && return ""
	t = body[1] * (length(body) > 1 ? " …" : "")
	return length(t) > 160 ? first(t, 159) * "…" : t
end
