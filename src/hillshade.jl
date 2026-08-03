# hillshade.jl — View > "Illumination (Hillshade)…": port of Mirone's src_figs/shading_params.m,
# the little window with the astrolabe azimuth dial + the elevation quarter-circle that picks the
# GMT illumination model and its light vector.
#
# The C++ dialog is HillshadeDialog (70_window.cpp); it is hand-built (not a .ui) because its two
# main controls are CUSTOM PAINTED, DRAGGABLE widgets — the astrolabe dial (one red hand for a plain
# azimuth, three R/G/B hands for the false-colour model) and the quarter-circle elevation hand —
# exactly Mirone's axes1/axes2 lines.
#
# Mirone's 3 (Peucker) and 5 (Manip Raster) are DROPPED by request. The models that remain are
# RENUMBERED 1..7, CONTINUOUSLY — Mirone's numbering had holes once two entries went, and a toolbar
# that reads 1 2 4 6 7 8 is nonsense. Mirone's own number is noted only so the two programs can still
# be compared:
#
#   1  GMT grdgradient classic     -A<azim> -Nt  (+ -M when the grid is geographic)   -> intensity
#   2  GMT grdgradient Lambertian  -Es<azim>/<elev>                                   -> intensity
#   3  Lambertian with lighting    -E<azim>/<elev>+a<amb>+d<diff>+p<spec>+s<shine>    -> intensity  (Mirone 4)
#   4  Hillshade (ESRI)            ported here (GMT6 has no -Eh)                      -> intensity  (Mirone 6)
#   5  False colour                three azimuths -> R,G,B                            -> new IMAGE  (Mirone 7)
#   6  Dynamic Range Compression   GMT.kovesi (ppdrc) -> new GRID, then illuminated as 1 (Mirone 8)
#   7  Remove illumination                                                            -> clear      (Mirone 9)
#
# WHERE THE INTENSITY GOES. Models 1/2/3/4/6 produce a per-node REFLECTANCE grid, exactly what
# Mirone hands to mex_illuminate. mex_illuminate IS GMT_illuminate, which iGMT already owns as
# `gmtIlluminate` (40_shading.cpp) — the one HSV modulator every shade in this program ends in. So
# the grid is pushed down (gmtvtk_set_shade_intensity_h) and the shade engine uses it INSTEAD of the
# intensity it would derive from the surface normal; the modulation itself is untouched, shared, one
# function (SACRED_LAW: same operation, same function).
#
# Models 5 and 6 do not modulate — they make a new variable, so they follow the derived-variable
# display law: a new, descriptively named handle in the SAME window, checked, source unchecked.
#
# The false colour keeps BOTH of Mirone's algorithms (its two radio buttons): the grdgradient one and
# the "Old algorithm", which is `shade_manip_raster` (mirone.m) — the same maths dropped method 5 was
# built on, but it stays HERE because it is the false colour's own second flavour, and with it stay
# its two inputs, the elevation and the Amp factor.

# ---------------------------------------------------------------------------------------------
# GMT grdgradient classic: the directional derivative, atan-normalized (-Nt), which is the intensity
# grdimage's -I wants. Mirone adds -M whenever the grid is geographic (mirone.m ImageIllum), which is
# GMT6's f=:g. Shared by model 1, by each of the false colour's three azimuths, and by model 8.
function _hs_classic(G::GMTgrid, azim::Float64)
	kw = Dict{Symbol,Any}(:A => azim, :N => "t")
	_isgeographic(G) && (kw[:f] = :g)
	return GMT.grdgradient(G; kw...)
end

# ---------------------------------------------------------------------------------------------
# ESRI's hillshade. GMT6's grdgradient has no -Eh, so this is a PORT of the algorithm Mirone calls —
# `hillshade()` in mex/grdgradient_m.c — not a lookalike substitute:
#
#   elev -> ZENITH angle (90-elev), azim -> counter-clockwise-from-east (90-azim), both as the mex
#   does before entering the loop; a 3x3 Sobel pair on the padded grid; z_factor = x_factor*y_factor
#   with x_factor = -1/(2dx) and y_factor = -1/(2dy)/2 (the mex's way of folding ESRI's 1/(8*cell));
#   slope = atan(z_factor*|grad|), aspect = atan2(-dzdy,-dzdx) (the mex's sign, NOT the ESRI page's),
#   and NO clipping of negatives (the mex deliberately skips the page's `if (data[k] < 0) = 0`).
#
# The mex runs on a GMT-padded copy with GMT_boundcond_set filling the pad; here the 3x3 window is
# edge-REPLICATED instead, which differs from the mex only on the outermost row/column.
function _hs_esri(G::GMTgrid, azim::Float64, elev::Float64)
	z = G.z
	ny, nx = size(z)
	el = (90.0 - elev) * pi / 180
	az = 90.0 - azim
	while (az < 0.0);   az += 360.0;  end
	while (az > 360.0); az -= 360.0;  end
	az *= pi / 180
	cos_elev = cos(el);  sin_elev = sin(el)
	dx = Float64(G.inc[1]);  dy = Float64(G.inc[2])
	x_factor = -1.0 / (2.0 * dx)
	y_factor = -1.0 / (2.0 * dy) / 2.0     # the mex's y_factor /= 2, so x*y == 1/(8*dx*dy)
	z_factor = x_factor * y_factor
	R = Matrix{Float32}(undef, ny, nx)
	nan32 = NaN32
	@inbounds for i in 1:nx
		im1 = max(i - 1, 1);  ip1 = min(i + 1, nx)
		for j in 1:ny
			jm1 = max(j - 1, 1);  jp1 = min(j + 1, ny)
			# the mex's work[] 3x3, laid out (column = x, row = y):
			#   w0=(i-1,j-1) w3=(i,j-1) w6=(i+1,j-1)
			#   w1=(i-1,j)   w4=(i,j)   w7=(i+1,j)
			#   w2=(i-1,j+1) w5=(i,j+1) w8=(i+1,j+1)
			w0 = Float64(z[jm1, im1]);  w1 = Float64(z[j, im1]);  w2 = Float64(z[jp1, im1])
			w3 = Float64(z[jm1, i]);                              w5 = Float64(z[jp1, i])
			w6 = Float64(z[jm1, ip1]);  w7 = Float64(z[j, ip1]);  w8 = Float64(z[jp1, ip1])
			if isnan(w0) || isnan(w1) || isnan(w2) || isnan(w3) || isnan(z[j, i]) ||
			   isnan(w5) || isnan(w6) || isnan(w7) || isnan(w8)
				R[j, i] = nan32
				continue
			end
			dzdx = (w6 + 2*w7 + w8) - (w0 + 2*w1 + w2)
			dzdy = (w2 + 2*w5 + w8) - (w0 + 2*w3 + w6)
			slope = atan(z_factor * sqrt(dzdx*dzdx + dzdy*dzdy))
			aspect = dzdx == 0.0 ? (dzdy > 0.0 ? pi/2 : (dzdy < 0.0 ? -pi/2 : 0.0)) : atan(-dzdy, -dzdx)
			R[j, i] = Float32(cos_elev * cos(slope) + sin_elev * sin(slope) * cos(az - aspect))
		end
	end
	return R
end

# ---------------------------------------------------------------------------------------------
# Push a per-node reflectance to the viewer. Layout is the grid's own: column-major R[ix*ny + iy],
# the same the surface z uses (see view_grid), so no transposition happens anywhere.
function _hs_push(scene::Ptr{Cvoid}, R::Matrix{Float32}, x0::Float64, x1::Float64,
                  y0::Float64, y1::Float64, model::Int)
	ny, nx = size(R)
	ccall(_fn(:gmtvtk_set_shade_intensity_h), Cvoid,
	      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint),
	      scene, R, Cint(nx), Cint(ny), x0, x1, y0, y1, Cint(model))
	return nothing
end

_hs_f32(R::GMTgrid) = eltype(R.z) === Float32 ? R.z : Float32.(R.z)

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
	ny, nx = size(G.z)
	rgb = Array{UInt8,3}(undef, ny, nx, 3)
	for (b, az) in enumerate((azR, azG, azB))
		# mirone.m calls it as shade_manip_raster((azim-90)*D2R, elev*D2R, Z, size_amp)
		R = oldalgo ? _hs_manip_raster(G.z, az - 90.0, elev, amp) : _hs_f32(_hs_classic(G, az))
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

# Mirone's model 9 IS `ImageResetOrigImg_CB` — RESTORE THE ORIGINAL IMAGE, not merely "drop the
# reflectance". That distinction is the whole bug: models 5 and 6 do not modulate anything, they put
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
	# Models 1-4 only modulate: the light is already off by here and NOTHING was ever swapped on
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
		d = _nswing_parse(unsafe_string(cparams))
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
		if model == 7
			_hs_push(scene, Matrix{Float32}(undef, 0, 0), 0.0, 0.0, 0.0, 0.0, -1)
			_hs_restore_original(scene, gname)
			return Cint(1)
		end

		# EVERY model starts from the window's SOURCE grid and from a clean screen — the models are
		# alternatives, never a pipeline. Models 5/6 leave a derived product displayed with the source
		# unchecked; without this, picking 1 next computed its reflectance from the source (below) while
		# the ppdrc field was still the thing on screen, so 6 "contaminated" 1. Same restore function the
		# ✕ uses, so there is ONE way to put the original back. No-op when no product is showing.
		_hs_restore_original(scene, gname)

		G = _find_object(scene, :grid, gname)
		(G isa GMTgrid) || error("Illumination works on the grid this window is showing, and this window is showing none")

		azim = mod(num("azim", 0.0), 360.0)        # mirone.m: luz.azim = rem(luz.azim, 360)
		elev = num("elev", 30.0)

		if model == 5                              # false colour -> a NEW IMAGE, not a modulation
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

		if model == 6                              # PPDRC: a new derived GRID, then illuminated as 1
			# GMT.kovesi is a plain GMT.jl function (src/kovesi.jl): its FFTs go through GMT's own
			# fft2d (GMT_FFT_2D), so there is no FFTW dependency to load and no stub to guard against.
			wl = _get(d, "wavelength")
			P = isempty(wl) ? GMT.kovesi(G) : GMT.kovesi(G; wavelength = parse(Float64, wl))
			_gm3d_deliver(scene, P, _HS_PPDRC, "", false, "kovesi ppdrc") == 0 &&
				error("could not add the compressed grid to the window")
			G = P                                  # illuminate the ppdrc field, as Mirone does
		end

		R = if model == 1 || model == 6
			_hs_f32(_hs_classic(G, azim))
		elseif model == 2
			_hs_f32(GMT.grdgradient(G; E = "s$(azim)/$(elev)"))
		elseif model == 3
			_hs_f32(GMT.grdgradient(G; E = "$(azim)/$(elev)+a$(num("ambient", 0.55))" *
			                            "+d$(num("diffuse", 0.6))+p$(num("specular", 0.4))" *
			                            "+s$(num("shine", 10.0))"))
		elseif model == 4
			_hs_esri(G, azim, elev)
		else
			error("unknown illumination model: $model")
		end

		_hs_push(scene, R, Float64(G.range[1]), Float64(G.range[2]),
		         Float64(G.range[3]), Float64(G.range[4]), model)
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
	_hs_f32(GMT.grdgradient(G; E = "s315.0/30.0"))        # model 2
	yield()
	_hs_f32(GMT.grdgradient(G; E = "315.0/30.0+a0.55+d0.6+p0.4+s10.0"))   # model 3
	yield()
	_hs_esri(G, 315.0, 30.0)                              # model 4
	yield()
	_hs_manip_raster(G.z, 225.0, 30.0, 125.0)             # model 5, "Old algorithm" branch
	_hs_false_color(G, 0.0, 120.0, 240.0)                 # model 5, grdgradient branch (-> mat2img)
	yield()
	GMT.kovesi(G; wavelength = 16.0)                      # model 6 (ppdrc: filtergrid + 4 FFTs)
	yield()
	precompile(_on_hillshade,       (Ptr{Cvoid}, Cstring))
	precompile(_hs_push,            (Ptr{Cvoid}, Matrix{Float32}, Float64, Float64, Float64, Float64, Int))
	precompile(_hs_restore_original,(Ptr{Cvoid},))
	return nothing
end

function _register_hillshade()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_hillshade, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_hillshade_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("illumination", _hs_warm)   # C++ fires this when the dialog opens (70_window.cpp)
	return
end
