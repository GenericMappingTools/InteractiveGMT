# hillshade.jl — View > "Illumination (Hillshade)…": port of Mirone's src_figs/shading_params.m,
# the little window with the astrolabe azimuth dial + the elevation quarter-circle that picks the
# GMT illumination model and its light vector.
#
# The C++ dialog is HillshadeDialog (70_window.cpp); it is hand-built (not a .ui) because its two
# main controls are CUSTOM PAINTED, DRAGGABLE widgets — the astrolabe dial (one red hand for a plain
# azimuth, three R/G/B hands for the false-colour model) and the quarter-circle elevation hand —
# exactly Mirone's axes1/axes2 lines.
#
# Mirone's 3 (Peucker), 5 (Manip Raster) and 6 (ESRI hillshade) are DROPPED by request. The models
# that remain are RENUMBERED CONTINUOUSLY — Mirone's numbering had holes once those entries went, and
# a toolbar that reads 1 2 4 7 8 is nonsense. Two of this program's own looks (4, 5) and a third added
# 2026-08-28 (6, the PBR shade) are numbered into the same sequence. Mirone's own number is noted only
# where one exists, so the two programs can still be compared:
#
#   1  GMT grdgradient classic     -A<azim> -Nt  (+ -M when the grid is geographic)   -> intensity
#   2  GMT grdgradient Lambertian  -Es<azim>/<elev>  (GMT's own module, not a port)   -> intensity
#   3  Lambertian with lighting    -E<azim>/<elev>+a<amb>+d<diff>+p<spec>+s<shine>    -> intensity  (Mirone 4)
#   4  Hillshade (grdimage)        the Shading dock's look — NEVER REACHES JULIA      -> C++ look
#   5  Hillshade (Lambert)         the Shading dock's look — NEVER REACHES JULIA      -> C++ look
#   6  Shade (PBR)                 the Shading dock's look — NEVER REACHES JULIA      -> C++ look
#   7  False colour                three azimuths -> R,G,B                            -> new IMAGE  (Mirone 7)
#   8  Dynamic Range Compression   GMT.kovesi (ppdrc) -> new GRID, then illuminated as 1 (Mirone 8)
#   9  Remove illumination                                                            -> clear      (Mirone 9)
#
# 4, 5 AND 6 ARE NOT PORTS. They are the three looks the Shading dock offers, whose reflectance is
# computed in C++ from the surface itself (`applyReliefShade` / `applyPBRShade`, 40_shading.cpp)
# rather than as a GMT grid. The dialog applies them by calling `sceneSetReliefLook` — the very
# function the dock's checkboxes call — so each look has ONE implementation reachable from both
# (SACRED_LAW.md), and the dock may lose its boxes later without the methods going with them. Nothing
# for those three arrives here. Method 6 is the dock's PBR: VTK's own material on a 3-D surface, and
# with "Shaded image (2-D)" on, the CPU Cook-Torrance bake that imitates it per flat-image pixel.
#
# WHERE THE INTENSITY GOES. Models 1/2/3/8 produce a per-node REFLECTANCE grid, exactly what
# Mirone hands to mex_illuminate. mex_illuminate IS GMT_illuminate, which iGMT already owns as
# `gmtIlluminate` (40_shading.cpp) — the one HSV modulator every shade in this program ends in. So
# the grid is pushed down (gmtvtk_set_shade_intensity_h) and the shade engine uses it INSTEAD of the
# intensity it would derive from the surface normal; the modulation itself is untouched, shared, one
# function (SACRED_LAW: same operation, same function).
#
# Models 7 and 8 do not modulate — they make a new variable, so they follow the derived-variable
# display law: a new, descriptively named handle in the SAME window, checked, source unchecked.
#
# The false colour keeps BOTH of Mirone's algorithms (its two radio buttons): the grdgradient one and
# the "Old algorithm", which is `shade_manip_raster` (mirone.m) — the same maths dropped method 5 was
# built on, but it stays HERE because it is the false colour's own second flavour, and with it stay
# its two inputs, the elevation and the Amp factor.

# ---------------------------------------------------------------------------------------------
# GMT grdgradient classic: the directional derivative, atan-normalized (-Nt), which is the intensity
# grdimage's -I wants. Mirone adds -M whenever the grid is geographic (mirone.m ImageIllum), which is
# GMT6's f=:g. Shared by model 1, by each of the false colour's three azimuths, and by model 8 (DRC).
function _hs_classic(G::GMTgrid, azim::Float64)
	kw = Dict{Symbol,Any}(:A => azim, :N => "t")
	_isgeographic(G) && (kw[:f] = :g)
	return GMT.grdgradient(_hs_lend(G); kw...)
end

# HAND A GMT MODULE A GRID IT MAY SCRIBBLE ON, NEVER THE CALLER'S OWN.
# `GMT.grdgradient(G)` writes back into G.z: the buffer comes out REORDERED while `G.layout` still
# says what it said before, so the grid silently stops matching its own label. Measured on
# @earth_relief_30m read "TRB": after one grdgradient the same `_zmat(G)` differs from itself by
# 10370.5, and model 2 — which reads through `_zmat`, correctly — then computes a reflectance that is
# off by 1.89 on a quantity that lives in (-1, 1). So running model 1 CORRUPTED THE GRID and model 2
# (and 4, and the false-colour path, and anything else reading the grid afterwards) was the victim.
# It is the grid memory-layout law's own failure mode — a buffer and its layout code parting company —
# arriving from outside, so it is caught at the one door every module call in this file goes through.
_hs_lend(G::GMTgrid) = deepcopy(G)

# ---------------------------------------------------------------------------------------------
# Push a per-node reflectance to the viewer. Layout is the grid's own: column-major R[ix*ny + iy],
# the same the surface z uses (see view_grid), so no transposition happens anywhere.
#
# `side` names WHICH surface the reflectance was computed from, because an Aquamoto tsunami layer has
# two of them: 0 = the window's only surface, and in Aquamoto the WATER (the live stage); 1 = the
# Aquamoto LAND (the static bathymetry). A window that shows one grid only ever uses side 0.
function _hs_push(scene::Ptr{Cvoid}, R::Matrix{Float32}, x0::Float64, x1::Float64,
                  y0::Float64, y1::Float64, model::Int, side::Int=0)
	ny, nx = size(R)
	ccall(_fn(:gmtvtk_set_shade_intensity_h), Cvoid,
	      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cint),
	      scene, R, Cint(nx), Cint(ny), x0, x1, y0, y1, Cint(model), Cint(side))
	return nothing
end

# THE reflectance: one illumination model + its dialog parameters -> a per-node intensity for the
# grid handed in. EVERY illuminated surface comes through here -- a plain grid, and EACH of an
# Aquamoto layer's two surfaces (the live stage the water stands on, the static bathymetry the land
# stands on). That is what keeps a tsunami's wet and dry halves lit by their own relief instead of
# by one of them twice; see `_aqua_illuminate!` (aquamoto.jl). Models 7 (false colour) and 8 (ppdrc)
# are NOT reflectances -- they build a new variable and are handled by the caller; 8 then arrives
# here with its derived grid and is illuminated as model 1, exactly as Mirone does it.
_hs_reflectance(G::GMTgrid, model::Int, d::Dict{String,String})::Matrix{Float32} =
	_hs_reflectance_rng(G, model, d)[1]

# …and the same thing WITH the box the reflectance actually covers (see `_hs_reframe`). Every caller
# that hands the array on to the viewer must use this one: the array and its range are one answer.
function _hs_reflectance_rng(G::GMTgrid, model::Int,
                             d::Dict{String,String})::Tuple{Matrix{Float32},NTuple{4,Float64}}
	num(key, dflt) = (v = _get(d, key); isempty(v) ? dflt : parse(Float64, v))
	azim = mod(num("azim", 0.0), 360.0)        # mirone.m: luz.azim = rem(luz.azim, 360)
	elev = num("elev", 30.0)
	if model == 1 || model == 8
		g = _hs_classic(G, azim)
		return _hs_reframe(G, _hs_f32(g), g), (G.range[1], G.range[2], G.range[3], G.range[4])
	elseif model == 2
		# GMT's OWN simple Lambertian, `-Es<azim>/<elev>` — what the model is advertised to be, and
		# what it now runs. It was a hand-written port that skipped GMT's final [-0.95, 0.95] rescale;
		# without that rescale the raw cosine deviation is tiny (std 0.027 against method 1's 0.33), so
		# the model barely illuminated at all, and with the port's replacement normalisation — a
		# deviation-over-sigma atan, which IS grdgradient's -Nt — it came out as a copy of method 1.
		# GMT owns this algorithm; call it.
		g = GMT.grdgradient(_hs_lend(G); E = "s$(azim)/$(elev)")
		return _hs_reframe(G, _hs_f32(g), g), (G.range[1], G.range[2], G.range[3], G.range[4])
	elseif model == 3
		# GMT's FULL Lambertian takes the angle as a ZENITH, not an elevation: grdgradient.c does
		# `if (Ctrl->E.mode == 3) Ctrl->E.elevation = 90 - Ctrl->E.elevation;` before building the
		# light vector, so what reaches the sun is the complement of what is typed. Measured on a
		# plane tilted 30 deg: the reflectance peaked at elev 30 when a real sun peaks at 90-30 = 60.
		# Pre-complementing here cancels GMT's own flip and the dialog's number means elevation again.
		g = GMT.grdgradient(_hs_lend(G); E = "$(azim)/$(90.0 - elev)+a$(num("ambient", 0.55))" *
		                                     "+d$(num("diffuse", 0.6))+p$(num("specular", 0.4))" *
		                                     "+s$(num("shine", 10.0))")
		return _hs_reframe(G, _hs_f32(g), g), (G.range[1], G.range[2], G.range[3], G.range[4])
	end
	error("unknown illumination model: $model")
end

# One grid, one side, pushed. The bbox is the grid's OWN range (the viewer samples the reflectance by
# world position), so a caller never has to know how the two sides line up.
function _hs_push_grid(scene::Ptr{Cvoid}, G::GMTgrid, model::Int, d::Dict{String,String}, side::Int)
	R, (x0, x1, y0, y1) = _hs_reflectance_rng(G, model, d)
	_hs_push(scene, R, x0, x1, y0, y1, model, side)
	return nothing
end

_hs_f32(R::GMTgrid) = eltype(R.z) === Float32 ? R.z : Float32.(R.z)

# A GMT MODULE CAN HAND THE REFLECTANCE BACK IN A DIFFERENT LONGITUDE WINDOW, and it does:
# `grdgradient` RE-WRAPS a global geographic grid — give it @earth_relief_30m at -180..180 and the
# result comes back at 0..360, same size, columns rolled by half the world. Pushed under the source's
# range, that lands every model going through a GMT module (1, 2, 3, 8 and the false colour's three
# azimuths) exactly 180 degrees out of register with the image it is lighting: a -180/180 picture
# wearing a 0/360 illumination.
#
# It is put right HERE, by rolling the columns back, and not by teaching the viewer's sampler to try
# the other window: that sampler serves EVERY model, the false colour's own image included, so a
# wrap-around retry there would move things that were never in the wrong frame. Fix the reflectance
# that moved, at the one door it comes back through; leave the sampler alone.
#
# Only ever rolls when the source really is a 360-wide geographic grid and the shift is a whole
# number of columns. Anything else is returned untouched, so a projected or regional grid cannot be
# rotated by a rounding accident.
function _hs_reframe(Gsrc::GMTgrid, R::Matrix{Float32}, Gout::GMTgrid)::Matrix{Float32}
	dlon = Gout.range[1] - Gsrc.range[1]
	(abs(dlon) < 1e-9 || Gsrc.inc[1] <= 0.0) && return R          # same window already: nothing to do
	isapprox(Gsrc.range[2] - Gsrc.range[1], 360.0; atol = 1e-6) || return R   # not a whole-world grid: not ours to roll
	k = round(Int, dlon / Gsrc.inc[1])
	isapprox(k * Gsrc.inc[1], dlon; atol = 1e-6 * Gsrc.inc[1]) || return R
	nx = size(R, 2)
	# A gridline-registered whole-world grid repeats its first meridian as its last column: roll the
	# UNIQUE columns and re-append the duplicate, or the seam ends up one cell wide in the wrong place.
	dup = (Gsrc.registration == 0) && nx > 1
	Rc  = dup ? R[:, 1:nx-1] : R
	Rr  = circshift(Rc, (0, mod(k, size(Rc, 2))))
	return dup ? hcat(Rr, Rr[:, 1:1]) : Rr
end

# ---------------------------------------------------------------------------------------------
# The false colour's "Old algorithm" — a PORT of `shade_manip_raster` (mirone.m), the Manip Raster
# shading, verbatim quirks and all:
#
#   u = (sin(az)cos(el), -cos(az)cos(el), sin(el))          az, el in RADIANS
#   dZdr, dZdc: forward differences on the four edges, CENTRED inside — and every one of them
#               divided by `size_amp` ALONE (never by 2, and never by the cell size: this shading is
#               index-based, `size_amp` is its only scale = the dialog's "Amp factor")
#   img = (dZdr*u1 + dZdc*u2 + 2*u3) / sqrt(dZdr^2 + dZdc^2 + 4),  clipped to [0,1]
#
# z is (ny, nx) with row = y, so `dZdr` is the row (y) derivative and `dZdc` the column (x) one, the
# same meaning they have in Mirone (its grids are GMT-ordered too, row 1 = south).
# The caller passes the azimuth already turned into Mirone's convention ((azim-90) in degrees).
function _hs_manip_raster(z::AbstractMatrix, azim::Float64, elev::Float64, size_amp::Float64)
	ny, nx = size(z)
	az = azim * pi / 180;  el = elev * pi / 180
	u1 = sin(az) * cos(el);  u2 = -cos(az) * cos(el);  u3 = sin(el)
	out = Matrix{Float32}(undef, ny, nx)
	@inbounds for i in 1:nx
		ic0 = i == 1 ? 1 : i - 1                       # forward difference on the left/right edges,
		ic1 = i == nx ? nx : i + 1                     # centred (still /size_amp, not /2) inside
		for j in 1:ny
			jr0 = j == 1 ? 1 : j - 1
			jr1 = j == ny ? ny : j + 1
			dZdr = (Float64(z[jr1, i]) - Float64(z[jr0, i])) / size_amp
			dZdc = (Float64(z[j, ic1]) - Float64(z[j, ic0])) / size_amp
			v = (dZdr * u1 + dZdc * u2 + 2 * u3) / sqrt(dZdr * dZdr + dZdc * dZdc + 4)
			out[j, i] = Float32(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v))
		end
	end
	return out
end

# ---------------------------------------------------------------------------------------------
# False colour (Mirone's ImageIllumFalseColor): illuminate the grid from three directions and make
# the three reflectances the R, G and B of one image. Both of Mirone's algorithms are here, its two
# radio buttons: `oldalgo=false` is the grdgradient one (mercedes_type 1), `oldalgo=true` is
# shade_manip_raster (mercedes_type 0), which is the only branch that reads `elev` and `amp`.
# Either way the scaling is Mirone's: cvlib CvtScale(R, 254, 1) into a uint8, so negatives saturate
# to 0 — for the old algorithm that clamp never bites, its output is already in [0,1].
function _hs_false_color(G::GMTgrid, azR::Float64, azG::Float64, azB::Float64;
                         oldalgo::Bool=false, elev::Float64=30.0, amp::Float64=125.0)
	Z = _zmat(G)            # (ny,nx), row 1 = south, for ANY layout the grid was read in
	ny, nx = size(Z)
	rgb = Array{UInt8,3}(undef, ny, nx, 3)
	for (b, az) in enumerate((azR, azG, azB))
		# mirone.m calls it as shade_manip_raster((azim-90)*D2R, elev*D2R, Z, size_amp)
		R = oldalgo ? _hs_manip_raster(Z, az - 90.0, elev, amp) : _hs_f32(_hs_classic(G, az))
		@inbounds for k in eachindex(R)
			v = 254.0f0 * R[k] + 1.0f0
			rgb[k + (b-1)*ny*nx] = isnan(v) ? UInt8(0) : (v <= 0.0f0 ? UInt8(0) : (v >= 255.0f0 ? UInt8(255) : round(UInt8, v)))
		end
	end
	return GMT.mat2img(rgb, G)
end

# ---------------------------------------------------------------------------------------------
# C callback (the dialog's OK button). `cparams` is the newline-separated "key=value" block built by
# HillshadeDialog (see JuliaHillshadeFn, 30_app.cpp). Returns Cint 1 on success, 0 on failure.
# The two derived products this tool can put on screen. Named ONCE, here, because "Remove
# illumination" has to un-display exactly what the models displayed — a second spelling of either
# string at the call site is a silent way for Remove to stop finding them.
const _HS_FALSECOLOR = "False colour illumination"
const _HS_PPDRC      = "Dynamic range compressed"

# Model 9 (Mirone's 9) IS `ImageResetOrigImg_CB` — RESTORE THE ORIGINAL IMAGE, not merely "drop the
# reflectance". That distinction is the whole bug: models 7 and 8 do not modulate anything, they put
# a NEW picture (false colour) or a NEW grid (ppdrc) on screen and UNCHECK the source, per the
# derived-variable display law. Killing the light on those leaves the derived variable still
# displayed — the screen does not change, so the ✕ "does nothing". So Remove also puts the source
# back: this tool's derived products are unchecked, the original grid is re-checked, and the axes are
# re-framed to IT (derived-variable axes law, run in reverse).
function _hs_restore_original(scene::Ptr{Cvoid}, srcname::AbstractString = "")
	had = false
	for nm in (_HS_FALSECOLOR, _HS_PPDRC)      # returns 0 when that product was never made
		had |= ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint),
		             scene, nm, Cint(0)) != 0
	end
	# Models 1-6 only modulate: the light is already off by here and NOTHING was ever swapped on
	# screen, so leave the camera and the axes exactly where the user put them. Re-framing (below)
	# re-fits the view, and doing that on a plain "remove the shade" would yank the 3-D camera to a
	# top-down snap for no reason.
	had || return
	# The grid to put back is the one the tool was aimed at — the DISPLAYED layer the caller resolved
	# (`srcname`), not "the window's first grid", which in a multi-grid window is a different layer.
	# Only when that name is not on record does it fall back to the primary ("" = the base surface).
	name, G = "", nothing
	if !isempty(srcname)
		G = _find_object_exact(scene, :grid, srcname)
		G !== nothing && (name = String(srcname))
	end
	G === nothing && ((name, G) = _find_object_named(scene, :grid))
	# Putting the ORIGINAL back on display is the SAME operation as putting a derived result on it —
	# same one function (`_adopt_derived!`, grid.jl), just aimed at the source: it is re-checked, the
	# products are unchecked, and the axes+camera go back to ITS own limits (derived-variable axes law,
	# run in reverse). `G` non-grid (nothing on record) -> show/hide only, no re-frame.
	_adopt_derived!(scene, name, G isa GMTgrid ? G : nothing)
	return
end

function _on_hillshade(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		# If the dialog's warm-up (see _hs_warm) is still compiling, let it finish before we start:
		# the two would otherwise be inside GMT at the same time. Returns at once in the normal case,
		# where the user spent longer than the compile picking a model.
		warm_wait("illumination")
		raw = unsafe_string(cparams)
		# What the DIALOG actually sent, on the console, verbatim. The only way to tell a wrong model
		# number leaving the dialog apart from a wrong dispatch after it.
		println("Illumination <- ", replace(raw, "\n" => " | "))
		d = _nswing_parse(raw)
		model = parse(Int, _get(d, "model"))
		num(key, dflt) = (v = _get(d, key); isempty(v) ? dflt : parse(Float64, v))

		# WHICH GRID: the one the window is CURRENTLY SHOWING. The dialog resolves it C++-side through
		# the same `resolveActiveGrid` the colour bar, the Z axis and the hover readout use, and sends
		# its Scene Objects name — so illumination lands on the layer the user is looking at, never on
		# "the first grid this window ever had" (which is what an empty name resolves to, and which was
		# a different layer the moment a second grid was dropped/downloaded into the window).
		gname = _get(d, "grid")
		# ... unless what is on screen is one of THIS TOOL's own derived products from a previous run.
		# Those are not a source to illuminate: the models are alternatives, never a pipeline, so the
		# name is dropped and the source grid resolves normally below.
		(gname == _HS_FALSECOLOR || gname == _HS_PPDRC) && (gname = "")

		# "Remove illumination" (Mirone's ImageResetOrigImg_CB): EVERY light off, not just this tool's
		# reflectance — model -1 is the viewer's code for that, and it leaves the grid in plain CPT
		# colour. (model 0, used below, is the quieter clear: the model painted its own picture, so the
		# reflectance goes but the Shading dock keeps its look.)
		if model == 9
			_hs_push(scene, Matrix{Float32}(undef, 0, 0), 0.0, 0.0, 0.0, 0.0, -1)
			# Removal undoes what the models did: an Aquamoto layer also stops re-lighting itself at
			# every new timestep (the loaded model is what makes _aquamoto_slice do that).
			haskey(_AQUA, scene) && empty!(_AQUA[scene].illum)
			_hs_restore_original(scene, gname)
			return Cint(1)
		end

		# EVERY model starts from the window's SOURCE grid and from a clean screen — the models are
		# alternatives, never a pipeline. Models 7/8 leave a derived product displayed with the source
		# unchecked; without this, picking 1 next computed its reflectance from the source (below) while
		# the ppdrc field was still the thing on screen, so 8 "contaminated" 1. Same restore function the
		# ✕ uses, so there is ONE way to put the original back. No-op when no product is showing.
		_hs_restore_original(scene, gname)

		# AQUAMOTO. What this window shows is not one grid but a COMPOSITE of two: water standing on
		# the live stage and land standing on the static bathymetry, each already lit by its own light
		# (bakeAquaShade, 40_shading.cpp). One reflectance cannot describe both surfaces -- illuminating
		# "the window's grid" resolved to the bathymetry and then smeared its relief over the sea too.
		# So each side is illuminated FROM ITS OWN SURFACE, through the same _hs_reflectance, and the
		# water is re-lit again at every timestep because its surface is a different one each time.
		# Models 7/8 make a NEW variable rather than modulating, so they fall through to the plain path.
		if haskey(_AQUA, scene) && model in (1, 2, 3)
			_aqua_illuminate!(scene, model, d)
			return Cint(1)
		end

		G = _find_object(scene, :grid, gname)
		(G isa GMTgrid) || error("Illumination works on the grid this window is showing, and this window is showing none")

		elev = num("elev", 30.0)                   # the false colour's own input; the reflectance models
		                                           # read azim/elev inside `_hs_reflectance`

		if model == 7                              # false colour -> a NEW IMAGE, not a modulation
			I = _hs_false_color(G, mod(num("azimR", 0.0), 360.0), mod(num("azimG", 120.0), 360.0),
			                    mod(num("azimB", 240.0), 360.0);
			                    oldalgo = _get(d, "oldalgo") == "1", elev = elev, amp = num("amp", 125.0))
			_hs_push(scene, Matrix{Float32}(undef, 0, 0), 0.0, 0.0, 0.0, 0.0, 0)   # its colours ARE the shade
			name = _HS_FALSECOLOR
			_add_image_to_scene(scene, I, name; promote=false) ||
				error("could not add the false-colour image to the window")
			# SACRED_LAW.md derived-variable display law, in the ONE shared transition (`_adopt_derived!`,
			# grid.jl): the new variable is CHECKED, everything it was derived from is UNCHECKED — the
			# source GRID included, which is why this no longer needs a separate cross-kind hide call —
			# and Scene Objects unfolds to reveal it.
			_adopt_derived!(scene, name, I)
			return Cint(1)
		end

		if model == 8                              # PPDRC: a new derived GRID, then illuminated as 1
			# GMT.kovesi is a plain GMT.jl function (src/kovesi.jl): its FFTs go through GMT's own
			# fft2d (GMT_FFT_2D), so there is no FFTW dependency to load and no stub to guard against.
			wl = _get(d, "wavelength")
			P = isempty(wl) ? GMT.kovesi(_hs_lend(G)) : GMT.kovesi(_hs_lend(G); wavelength = parse(Float64, wl))
			_gm3d_deliver(scene, P, _HS_PPDRC, "", false, "kovesi ppdrc") == 0 &&
				error("could not add the compressed grid to the window")
			G = P                                  # illuminate the ppdrc field, as Mirone does
		end

		# ONE reflectance function for every surface this tool lights (see `_hs_reflectance`): the plain
		# grid here, and each Aquamoto side above. `side = 0` is "the window's surface".
		_hs_push_grid(scene, G, model, d, 0)
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Illumination FAILED: $(sprint(showerror, e))")
		@warn "Illumination FAILED" exception=(e,)
		return Cint(0)
	end
end

# ---------------------------------------------------------------------------------------------
# JIT WARM-UP (warmup.jl). Opening the dialog starts this; pressing OK waits for it. Everything here
# runs on a 32x32 throw-away grid — the point is to COMPILE the code, and a method compiles once
# whatever the data size, so the actual arithmetic is over before it starts.
#
# The scene-mutating half of the tool (_hs_push, _hs_restore_original, _gm3d_deliver,
# _add_image_to_scene) must NOT be executed here: a window is on screen and running those would
# change what the user is looking at. `precompile` infers and generates their code without running
# them, which is all we need.
#
# `yield()` between steps matters when Julia has a single thread (the launcher's default): it lets
# the Qt pump timer fire, so the dialog keeps painting while this works. With -t 2 or more the task
# is on another thread and the yields cost nothing.
function _hs_warm()
	G = GMT.mat2grid(rand(Float32, 32, 32))
	_hs_f32(_hs_classic(G, 315.0))                        # model 1, and the false colour's 3 azimuths
	yield()
	GMT.grdgradient(G; E = "s315.0/30.0")                 # model 2
	yield()
	_hs_f32(GMT.grdgradient(G; E = "315.0/30.0+a0.55+d0.6+p0.4+s10.0"))   # model 3
	yield()
	_hs_manip_raster(G.z, 225.0, 30.0, 125.0)             # model 7, "Old algorithm" branch
	_hs_false_color(G, 0.0, 120.0, 240.0)                 # model 7, grdgradient branch (-> mat2img)
	yield()
	GMT.kovesi(G; wavelength = 16.0)                      # model 8 (ppdrc: filtergrid + 4 FFTs)
	yield()
	precompile(_on_hillshade,       (Ptr{Cvoid}, Cstring))
	precompile(_hs_push,            (Ptr{Cvoid}, Matrix{Float32}, Float64, Float64, Float64, Float64, Int, Int))
	precompile(_hs_reflectance,     (GMTgrid{Float32,2}, Int, Dict{String,String}))
	precompile(_hs_push_grid,       (Ptr{Cvoid}, GMTgrid{Float32,2}, Int, Dict{String,String}, Int))
	precompile(_aqua_illuminate!,   (Ptr{Cvoid}, Int, Dict{String,String}))
	precompile(_hs_restore_original,(Ptr{Cvoid},))
	return nothing
end

function _register_hillshade()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_hillshade, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_hillshade_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("illumination", _hs_warm)   # C++ fires this when the dialog opens (70_window.cpp)
	return
end
