# aquashade.jl — THE TSUNAMI'S PICTURE: SPLIT, ILLUMINATE EACH HALF, COMBINE.
#
# ONE function does the whole job, and it is the only place any of it happens:
#
#   1. SPLIT      the slice into DRY and WET by the mask (`_aqua_indland`, THE dry/wet test the
#                 composite has always been painted with — there is no second rule anywhere).
#   2. ILLUMINATE each half SEPARATELY, as the picture it is: its OWN colour scale over its OWN data
#                 range, its OWN method, its OWN light vector. Each half comes back a finished RGB
#                 IMAGE, never a reflectance: the CPT lookup AND the intensity modulation are done in
#                 one `grdimage` call, so both are GMT's own code and neither is re-implemented here.
#   3. COMBINE    the two images BY THAT SAME MASK, pixel for pixel. That single image is what
#                 Aquamoto displays.
#
# WHY IMAGES AND NOT REFLECTANCES. A reflectance has to be paired back with the nodes it came from,
# and that pairing is where this kept going wrong: `grdgradient` returns a PLAIN (BCB) grid whatever
# layout it was handed, so pairing it with a mask built in the source grid's order puts every value
# on the wrong node — vertical stripes across the water, measured on tsu_time.nc. Here the mask is
# pushed through the SAME `grdimage` as the two halves, so it lands on exactly the same pixels and
# the combine needs no index arithmetic against any grid.

"""
    aqua_shade_image(bat, G; method_water, method_land, …) -> (img, mask, iw, il)

THE function. `bat` is the bathymetry, `G` the slice's stage; both as they were read, no copies, no
blanking, no NaN.

`method_*` is the Illumination dialog's own numbering, per side:

  2  GMT grdgradient classic      -A<azim> -Nt
  3  GMT grdgradient Lambertian   -Es<azim>/<elev>
  4  Lambertian with lighting     -E<azim>/<elev>+a<amb>+d<diff>+p<spec>+s<shine>
  5  Hillshade (grdimage)         the classic reflectance with a gain
  6  Hillshade (Lambert)          -Es<azim>/<elev> with an ambient term

  1  VTK (PBR)                    VTK's own render, captured

Method 1 is not a reflectance and is not computed here: it is VTK's PBR RENDER, whose material lives
on an ACTOR (`SetInterpolationToPBR` + the IBL sky — `applySurfStyle` / `applyShading`,
40_shading.cpp). Its half is therefore DISPLAYED and LIT through the Illumination tool's own door
(`_on_hillshade` with `model=1`, the very request the dialog sends) and its pixels are taken with the
capture that already exists (`gmtvtk_capture_rect_rgb`). Nothing of that maths is re-implemented, and
it is NOT method 7 — 7 is the CPU Cook-Torrance bake (`applyPBRShade`) that imitates the render per
pixel, which lives in C++ and is a different picture.

Returns the combined image and, for inspection, the two halves' pictures as plain (row, col, band)
RGB arrays.
"""
function aqua_shade_image(bat::GMTgrid, G::GMTgrid;
                          method_water::Int = 2, method_land::Int = 2,
                          azim_water::Float64 = 45.0, elev_water::Float64 = 30.0,
                          azim_land::Float64  = 45.0, elev_land::Float64  = 30.0,
                          cmap_water = :polar, cmap_land = :geo,
                          range_water::Union{Nothing,Tuple{Float64,Float64}} = nothing,
                          range_land::Union{Nothing,Tuple{Float64,Float64}} = nothing,
                          ambient::Float64 = 0.55, diffuse::Float64 = 0.6,
                          specular::Float64 = 0.4, shine::Float64 = 10.0, gain::Float64 = 1.0,
                          scene::Ptr{Cvoid} = C_NULL)
	# ---- 1. THE SPLIT -------------------------------------------------------------------------
	# No entry checks. This runs per animation frame and the caller is Aquamoto, which read both
	# grids itself from one cube — sizes, types and layouts are known, not to be re-discovered here.
	# (`any(wet)` / `any(dry)` were two full-array scans per frame to reject a case that cannot occur.)
	dry = _aqua_indland(bat.z, G.z)
	wet = .!dry

	# ---- 2. EACH HALF, LIT AS THE PICTURE IT IS -----------------------------------------------
	# The FIELD handed to grdimage is the whole grid; what scopes a half is its RANGE, taken over its
	# own nodes. Nothing is blanked out of anything.
	# THE WATER SPAN IS SYMMETRIC ABOUT ZERO, and it is not computed here: `_aqua_water_range`
	# (aquamoto.jl) is THE water half's colour span, and every path that colours water uses it.
	# Scoping the water with plain extrema instead put the diverging palette's WHITE at the middle of
	# (min, max) — +0.705 m on a slice measured at (-0.662, 2.072) — so the palette drew a white band
	# along the 0.705 m contour, which hugs the shoreline. That was the "line of NaN along the coast":
	# `:polar`'s own white, in the wrong place, because a second range function had been written here.
	wrng = range_water === nothing ? _aqua_water_span(G.z, wet)   : range_water
	lrng = range_land  === nothing ? _aqua_half_range(bat.z, dry) : range_land
	kw = (ambient = ambient, diffuse = diffuse, specular = specular, shine = shine, gain = gain)

	# Method 1 is a RENDER, the rest are reflectances — but what comes back from either is the SAME
	# thing: that half's finished picture, as a plain (row, col, band) RGB array. So there is ONE
	# combine below, whatever made each half, and a mixed pair (say water by 1, land by 3) composes
	# exactly like a matched one.
	iw = _aqua_side_picture(G, dry, method_water, azim_water, elev_water, cmap_water, wrng,
	                        AQUA_WATER; scene=scene, kw...)
	# THE LAND HALF IS THE SAME PICTURE AT EVERY LAYER. It is the STATIC bathymetry, lit by a sun that
	# only moves when the user moves it — so re-rendering and re-grabbing it per timestep was half the
	# cost of a frame for nothing. Cached on everything that can change it; the wet mask is in the key
	# because the zero-fill follows the shoreline, which moves with the wave.
	lkey = (method_land, azim_land, elev_land, cmap_land, lrng, hash(wet))
	il = get(_AQUA_LAND_CACHE, lkey, nothing)
	if il === nothing
		il = _aqua_side_picture(bat, wet, method_land, azim_land, elev_land, cmap_land, lrng,
		                        AQUA_LAND; scene=scene, kw...)
		empty!(_AQUA_LAND_CACHE)                       # one entry: the current settings, nothing older
		_AQUA_LAND_CACHE[lkey] = il
	end
	return _aqua_combine_halves(iw, il, bat, G), iw, il
end

# ---- 3. COMBINE, ON THE GRID'S OWN NODES ------------------------------------------------------
# THE RESULT IS ONE PIXEL PER NODE, IN THE GRID'S OWN ELEMENT ORDER. That is what the consumer takes:
# `_aqua_shaded_rgb` (aquamoto.jl) hands the picture to `_aqua_pack_rgba` as the layer's texture and
# requires exactly `length(G.z)` pixels — a render at some window's resolution is not a thing it can
# use. Emitting here on the nodes also makes the MASK EXACT: the dry/wet question is asked at the
# nodes where the answer is defined (`_aqua_isdry`, the same element rule the composite has always
# been painted with, and the very array `_aqua_side_picture` filled each half's empty side by), so no
# interpolated shoreline and no quantisation of one against the other.
#
# The two halves are read at the node's own position in each — they may be different sizes, since a
# rendered half comes back at whatever its window gave — and each node is taken whole from one side:
# never blended, never split across bands.
#
# `dry` is in the GRID'S OWN buffer order (it was computed element-wise on the raw buffers), so the
# walk is over that order too and the output inherits it: index k of the picture is node k of `G.z`,
# which is the pairing `_aqua_pack_rgba` assumes. The node's geographic position comes from the
# LAYOUT (SACRED_LAW.md's grid-memory-layout law: the buffer is consumed where it lies, with its
# layout code beside it — never transposed).
function _aqua_combine_halves(Aw::Array{UInt8,3}, Al::Array{UInt8,3}, bat::GMTgrid, G::GMTgrid)
	nx, ny   = _grid_dims(G)
	drym     = _aqua_indland(_zmat(bat), _zmat(G))     # (iy, ix), row 1 = SOUTH — the ONE accessor's view
	Hw, Ww   = size(Aw, 1), size(Aw, 2)
	Hl, Wl   = size(Al, 1), size(Al, 2)
	# ROW 1 IS THE SOUTH ROW, and the image is labelled "BCBa" below — column-major, band-planar,
	# bottom-first, which is exactly what `GMT.mat2img` yields from a grid-derived matrix and what
	# every consumer in this program is written around (`_pixaccess_img` / `_north_first`, drape.jl).
	# Handing over a north-first array shredded the picture into horizontal stripes.
	out      = Array{UInt8,3}(undef, ny, nx, 3)
	@inbounds for r in 1:ny
		iy = r                                          # row 1 = SOUTH, counting up
		fy = ny > 1 ? (ny - r) / (ny - 1) : 0.0         # …as a fraction from the NORTH, which is where
		                                                # both halves' row 1 is
		for c in 1:nx
			fx = nx > 1 ? (c - 1) / (nx - 1) : 0.0
			A, Hh, Wh = drym[iy, c] ? (Al, Hl, Wl) : (Aw, Hw, Ww)
			rr = clamp(1 + round(Int, fy * (Hh - 1)), 1, Hh)
			cc = clamp(1 + round(Int, fx * (Wh - 1)), 1, Wh)
			out[r, c, 1] = A[rr, cc, 1]
			out[r, c, 2] = A[rr, cc, 2]
			out[r, c, 3] = A[rr, cc, 3]
		end
	end
	I = GMT.mat2img(out; x = [Float64(G.range[1]), Float64(G.range[2])],
	                     y = [Float64(G.range[3]), Float64(G.range[4])])
	I.layout = "BCBa"                                  # bottom-first, column-major, band-planar
	return I
end

# ONE half's finished picture, as a plain (row, col, band) RGB array.
#
#   method 1  -> VTK's own PBR RENDER: the half is displayed and lit through the Illumination tool's
#                own door and its pixels captured (`_pbr_capture`).
#   otherwise -> coloured and lit in a single `grdimage`: the CPT lookup and the intensity modulation
#                are GMT's, together, exactly as they are for any grid this program draws.
#
# THE OTHER SIDE IS SET TO ZERO — never NaN, never left as it lies.
#
# A COMPOSED PICTURE CANNOT CONTAIN A NaN PIXEL, and the line of them along every coast was not the
# mask missing: it was a BOUNDARY EFFECT, and it was already baked into each half before any
# combining. Illumination is a NEIGHBOURHOOD operation — a gradient, a normal, a reflectance all read
# the nodes around the one being lit. At the edge of the other side, those neighbours are not
# ordinary data:
#
#   * NaN there (the first attempt) makes the reflectance at the adjacent VALID node undefined —
#     grdgradient drops it, the rendered surface interpolates colour across the triangle joining a
#     valid node to a NaN one — so the pixels on the KEPT side of the shoreline came out unlit or
#     NaN-coloured. No mask can remove that: the damage is INSIDE pixels that legitimately belong to
#     the half being kept.
#   * the raw data there (the second attempt) is worse in a different way: a tsunami stage stores the
#     LAND ELEVATION on its dry cells, so the water half carried a 60 m cliff at the coast and every
#     water pixel beside it was lit by that cliff's normal, not by the sea's.
#
# ZERO has neither problem: it is defined, so nothing is dropped, and it is FLAT, so the neighbourhood
# the boundary nodes see is smooth and the light at the shoreline is the light of the half itself.
# The zeroed side is never read by the combine — it exists only to give the edge honest neighbours.
#
# This is not the banned "mask a half before lighting it": what scopes a half is still its RANGE,
# taken over its own nodes and passed in explicitly. Nothing is blanked to set contrast.
function _aqua_side_picture(G::GMTgrid, other::AbstractArray{Bool}, method::Int, azim::Float64,
                            elev::Float64, cmap, zrange::Tuple{Float64,Float64}, name::String;
                            scene::Ptr{Cvoid} = C_NULL, kw...)
	H = deepcopy(G)
	H.z[other] .= 0f0                                   # the empty side: flat, defined, never NaN
	H.range[5], H.range[6] = zrange[1], zrange[2]       # its OWN range still scopes it
	if method == 1
		return _pbr_capture(H, name, cmap, azim, elev, scene)
	end
	R = _aqua_half_reflectance(H, method, azim, elev; kw...)
	C = GMT.makecpt(cmap = cmap, range = (zrange[1], zrange[2]))
	_aqua_set_nan_color!(C)
	return _aqua_rgb_plane(GMT.grdimage(_hs_lend(H), C = C, I = R, A = ""))  # A="" -> in memory
end

# THE Preferences NaN fill colour, onto a CPT that is about to paint a grid.
#
# SACRED_LAW.md's preference-application law: ONE read, applied by construction. In the window that
# is `makeGridCTF` (10_geometry.cpp), which sets the LUT's NanColor itself so no builder can forget
# it. A half coloured OUT HERE by `grdimage` is the same duty with a different painter, so it takes
# the SAME value — asked of the viewer (`gmtvtk_pref_nan_color` -> `prefNanColorRGB`, 30_app.cpp),
# never re-read from the .ini here, which would be the second source the law forbids.
#
# A CPT's NaN colour is its `bfn` third row (background, foreground, NaN), RGB in 0..1.
function _aqua_set_nan_color!(C)
	haskey(_LIB_FNS, :gmtvtk_pref_nan_color) || return C
	rgb = zeros(Float64, 3)
	ccall(_fn(:gmtvtk_pref_nan_color), Cvoid, (Ptr{Cdouble},), rgb)
	C.bfn[3, 1], C.bfn[3, 2], C.bfn[3, 3] = rgb[1], rgb[2], rgb[3]
	return C
end

# A grdimage GMTimage's bytes as a plain (row, col, band) array.
#
# A GMTimage out of grdimage is PIXEL-INTERLEAVED ("TRPa" — the 'P'), and GMT.jl wraps those bytes as
# an (ny, nx, nb) Julia array WITHOUT de-interleaving them. So `A[i, j, b]` is NOT row i, col j, band
# b: indexing it that way shreds the picture into a block mosaic. (Seen, on tsu_time.nc.) The byte
# layout is the one thing that IS known: byte k (0-based) is band k % nb of pixel k ÷ nb, pixels in
# row-major order, first row north. The interleave is ASSERTED, never assumed.
function _aqua_rgb_plane(I)::Array{UInt8,3}
	(length(I.layout) >= 3 && I.layout[3] == 'P') ||
		error("Aquamoto: expected a pixel-interleaved image from grdimage, got layout '$(I.layout)'")
	# AND ITS DIMENSIONS COME OUT (nx, ny, nb) — NOT (ny, nx, nb). Measured on a deliberately
	# non-square case: a grid of nx=12, ny=8 gives `size(image) == (12, 8, 3)`. Reading those the
	# other way round walks every row at the wrong length, which shears the picture diagonally —
	# seen, on this very composite.
	W, H, nb = size(I.image, 1), size(I.image, 2), size(I.image, 3)
	out = Array{UInt8,3}(undef, H, W, 3)
	@inbounds for r in 1:H, c in 1:W
		p = ((r - 1) * W + (c - 1)) * nb
		for b in 1:3
			out[r, c, b] = I.image[p + min(b, nb)]
		end
	end
	return out
end

# THE reflectance for one half, by method. Every branch is a GMT module call -- `_hs_classic` is the
# very function the Illumination tool's own model 2 uses (hillshade.jl), so there is one classic
# reflectance in this program, not two.
function _aqua_half_reflectance(G::GMTgrid, method::Int, azim::Float64, elev::Float64;
                                ambient::Float64 = 0.55, diffuse::Float64 = 0.6,
                                specular::Float64 = 0.4, shine::Float64 = 10.0, gain::Float64 = 1.0)
	azim = mod(azim, 360.0)
	if method == 1
		error("Aquamoto: method 1 is a RENDER, not a reflectance — it is produced by " *
		      "`_aqua_side_picture`, which never calls this")
	elseif method == 7
		error("Aquamoto: illumination method 7 (Shade PBR) is the CPU Cook-Torrance bake " *
		      "(applyPBRShade, 40_shading.cpp), not a reflectance — producing it here would be a " *
		      "second implementation of that maths")
	elseif method == 2
		return _hs_classic(G, azim)
	elseif method == 3
		return GMT.grdgradient(_hs_lend(G); E = "s$(azim)/$(elev)")
	elseif method == 4
		return GMT.grdgradient(_hs_lend(G); E = "$(azim)/$(90.0 - elev)+a$(ambient)+d$(diffuse)" *
		                                        "+p$(specular)+s$(shine)")
	elseif method == 5
		g = _hs_classic(G, azim)                          # grdimage's own hillshade, with a gain
		gain == 1.0 || (g.z .= Float32.(clamp.(g.z .* gain, -1.0, 1.0)))
		return g
	elseif method == 6
		g = GMT.grdgradient(_hs_lend(G); E = "s$(azim)/$(elev)")
		ambient == 0.0 || (g.z .= Float32.(clamp.(g.z .* (1.0 - ambient) .+ ambient, -1.0, 1.0)))
		return g
	end
	error("Aquamoto: unknown illumination method $method")
end

# THE WATER HALF'S COLOUR SPAN, over the nodes `m` selects. The extrema are this file's own, the
# SYMMETRY is not: `_aqua_water_range` (aquamoto.jl) is THE rule for a water span in this program —
# symmetric about zero, so calm water sits at the diverging palette's centre and trough and crest
# read as the two sides they are — and it is called here rather than restated, so the half and the
# Aquamoto layer can never disagree about where white is.
function _aqua_water_span(A::AbstractArray, m::AbstractArray{Bool})::Tuple{Float64,Float64}
	lo, hi = _aqua_half_range(A, m)
	stub = GMT.mat2grid(zeros(Float32, 2, 2))          # carrier for the range; nothing is computed on it
	stub.range[5], stub.range[6] = lo, hi
	return _aqua_water_range(stub)
end

# The extrema of `A` over the nodes `m` selects — a half's OWN range, read without copying or
# blanking anything. A degenerate half is nudged so a CPT can still be built over it.
function _aqua_half_range(A::AbstractArray, m::AbstractArray{Bool})::Tuple{Float64,Float64}
	lo, hi = Inf, -Inf
	@inbounds for i in eachindex(A)
		m[i] || continue
		v = Float64(A[i])
		isfinite(v) || continue
		v < lo && (lo = v)
		v > hi && (hi = v)
	end
	(isfinite(lo) && isfinite(hi)) || return (0.0, 1.0)
	hi > lo || (hi = lo + 1.0)
	return (lo, hi)
end

"""
    tsushade(path; k=nothing, var="z", method=2, …) -> GMTimage

READ A TSUNAMI SLICE, BUILD THE COMBINED PICTURE, AND SHOW IT. One call, nothing else to set up:

    using InteractiveGMT
    tsushade("C:/v/tsu/tsu_ocean2/tsu_time.nc")                  # middle layer
    tsushade("C:/v/tsu/tsu_ocean2/tsu_time.nc"; k = 40)          # layer 41 (k is 0-based)
    tsushade(path; method_water = 3, method_land = 6, azim_land = 300.0)
    tsushade(path; savefile = "C:/v/combined.png")               # …and write it out

`method` sets both sides at once; `method_water` / `method_land` override either. The image is the
one `aqua_shade_image` makes — the two halves lit apart and combined by the dry/wet mask — and it is
the same picture Aquamoto shows. It is displayed with GMT's own `imshow` and returned, so it can be
looked at, written, or passed on.
"""
function tsushade(path::String; k::Union{Int,Nothing} = nothing, var::String = "z",
                  method::Int = 2, method_water::Int = 0, method_land::Int = 0,
                  azim_water::Float64 = 45.0, elev_water::Float64 = 30.0,
                  azim_land::Float64  = 300.0, elev_land::Float64 = 25.0,
                  cmap_water = :polar, cmap_land = :geo,
                  savefile::String = "", show::Bool = true)
	nt = 0
	for v in _netcdf_subdatasets(path)
		v.name == var && (nt = v.dims[1])
	end
	nt > 0 || error("tsushade: '$var' not found in $path")
	kk  = k === nothing ? nt ÷ 2 : k
	(0 <= kk < nt) || error("tsushade: layer $kk out of range (0..$(nt - 1))")
	bat = GMT.gmtread(path * "?bathymetry", layout = "TRB")
	G   = GMT.gmtread(path * "?$(var)[$(kk)]", layout = "TRB")
	mw  = method_water > 0 ? method_water : method
	ml  = method_land  > 0 ? method_land  : method
	img, = aqua_shade_image(bat, G; method_water = mw, method_land = ml,
	                        azim_water = azim_water, elev_water = elev_water,
	                        azim_land = azim_land, elev_land = elev_land,
	                        cmap_water = cmap_water, cmap_land = cmap_land)
	println("tsushade: layer $(kk + 1)/$nt — water by method $mw (az $azim_water), " *
	        "land by method $ml (az $azim_land)")
	isempty(savefile) || (GMT.gmtwrite(savefile, img); println("tsushade: wrote $savefile"))
	show && GMT.imshow(img)
	return img
end

# ──────────────────────────────────────────────────────────────────────────────────────────────
# METHOD 1 (VTK PBR) IN THE IMAGE COMPOSITION
#
# Method 1 is VTK's own PBR render — the material lives on an ACTOR (`SetInterpolationToPBR` +
# the IBL sky, applySurfStyle/applyShading in 40_shading.cpp) — so its picture is what VTK DRAWS,
# not something computed per node. It is NOT method 7 (the CPU Cook-Torrance bake), and neither
# one stands in for the other.
#
# Nothing here is re-implemented. The half is shown through `view_grid`, lit through the
# Illumination tool's own door (`_on_hillshade` with `model=1`, exactly as the dialog sends it), and
# its pixels are taken with the capture that already exists (`gmtvtk_capture_rect_rgb`, via
# `_capture_rect_image`). It goes into `aqua_shade_image`'s OWN combine, the same one every other
# method's half goes into — there is no second composition.

# THE RENDER WINDOWS ARE OPENED ONCE AND KEPT.
#
# This runs PER ANIMATION FRAME. Opening a window and tearing it down again for each half cost 0.15 s
# + 0.35 s of the 0.70 s a half took — more than the render itself. The pair is therefore built on
# the first frame and reused: a later frame only pushes its new grid through
# `gmtvtk_replace_base_grid_h` (the SAME door every in-place grid swap in this program uses) and
# grabs the pixels. Keyed by the half's NAME, so water and land keep one window each.
const _PBR_WIN = Dict{String,Ptr{Cvoid}}()

# The land half, kept between layers — see `aqua_shade_image`. One entry, replaced whenever anything
# that decides the picture changes.
const _AQUA_LAND_CACHE = Dict{Any,Array{UInt8,3}}()

"""
    aqua_pbr_release!()

Close the kept render windows. Call when the animation is over; the next call reopens them.
"""
function aqua_pbr_release!()
    for (_, h) in _PBR_WIN
        ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
    end
    empty!(_PBR_WIN)
    _pump_once()
    return nothing
end

# ONE half: shown, lit with METHOD 1 through the Illumination tool's own door, captured.
# Comes back as a plain (row, col, band) RGB array, which is what the combine takes from every half.
#
# NO SLEEPS. The pump used to be `_pump_once(); sleep(0.02)` a fixed 150 times per half — 3 s of
# sleeping for work that takes a tenth of that, and measured worthless: dropping to three pumps with
# no sleep changed 3 991 bytes out of 15 009 300 (0.027 %, max 19). The event loop is pumped only to
# let Qt deliver what was just asked for.
function _pbr_capture(H::GMTgrid, name::String, cmap, azim::Float64, elev::Float64,
                      scene::Ptr{Cvoid} = C_NULL)::Array{UInt8,3}
	# NO WINDOW AT ALL. `gmtvtk_pbr_render_offscreen` runs VTK's own PBR render — the same material,
	# the same sky environment, the same lights the on-screen path uses — into an OFFSCREEN buffer, at
	# the grid's node resolution. Nothing appears on the desktop, nothing of the user's is borrowed,
	# and the grab cannot fail because a window was covered or moved (which is what the window routes
	# did, each in their own way). This is method 1, rendered, and it is never stood in for.
	if haskey(_LIB_FNS, :gmtvtk_pbr_render_offscreen)
		zb, nxc, nyc, zlay = _grid_zbuf(H)
		cz, crgb, ncol = _cpt_nodes_range(Float64(H.range[5]), Float64(H.range[6]), cmap)
		pRgb = Ref{Ptr{UInt8}}(C_NULL); pW = Ref{Cint}(0); pH = Ref{Cint}(0)
		ok = ccall(_fn(:gmtvtk_pbr_render_offscreen), Cint,
		           (Ptr{Cfloat}, Cint, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
		            Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
		            Cdouble, Cdouble, Cint, Cint, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}),
		           zb, nxc, nyc, zlay, H.range[1], H.range[2], H.range[3], H.range[4],
		           cz, crgb, Cint(ncol), azim, elev, 0.45, 0.0, 1.0, 0.3,
		           nxc, nyc, pRgb, pW, pH)
		if ok != 0
			try
				v = unsafe_wrap(Array, pRgb[], (3, Int(pW[]), Int(pH[])))   # (band, col, row), borrowed
				return permutedims(v, (3, 2, 1))                            # (row, col, band), owned
			finally
				ccall(_fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), pRgb[])
			end
		end
	end
	# NO WINDOW OF ITS OWN WHEN THE CALLER HAS ONE. A render target is a window, and a window costs
	# ~0.15 s to build and ~0.35 s to tear down — per half, per layer — besides flashing on screen.
	# The caller's own window already has a live GL context and the right camera, so the half is pushed
	# into IT through `gmtvtk_replace_base_grid_h` (the in-place swap every grid change uses), lit,
	# photographed, and left; the slice's own push, which follows immediately, puts the composite back.
	if scene != C_NULL
		zb, nxc, nyc, zlay = _grid_zbuf(H)
		cz, crgb, ncol = _cpt_nodes_range(Float64(H.range[5]), Float64(H.range[6]), cmap)
		ccall(_fn(:gmtvtk_replace_base_grid_h), Cint,
		      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring, Cint),
		      scene, zb, nxc, nyc, H.range[1], H.range[2], H.range[3], H.range[4], Cint(1),
		      cz, crgb, Cint(ncol), name, zlay)
		_remember_object!(scene, :grid, name, H)
		_pump_once()
		# THE LOOK IS SET DIRECTLY, NOT THROUGH `_on_hillshade`. That function is the Illumination
		# dispatcher, and on an Aquamoto scene model 1 routes straight back into `_aqua_illuminate!` ->
		# slice -> `_aqua_shaded_rgb` -> here: unbounded recursion, and the crash you get on picking the
		# method. `gmtvtk_set_relief_look_h` with RL_PBR (1) IS what model 1 means — the very call the
		# dialog makes for a plain grid — and it cannot recurse.
		ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), scene,
		      "sunaz=$(azim);sunel=$(elev);")     # this half's own sun
		ccall(_fn(:gmtvtk_set_relief_look_h), Cvoid, (Ptr{Cvoid}, Cint, Cint), scene, Cint(1), Cint(0))
		_pump_once()
		haskey(_LIB_FNS, :gmtvtk_lod_settle_h) &&
			ccall(_fn(:gmtvtk_lod_settle_h), Cint, (Ptr{Cvoid}, Cint), scene, Cint(0))
		img = _capture_rect_image(scene, H.range[1], H.range[2], H.range[3], H.range[4];
		                          coords = false, prefer_screen = true)
		(img === nothing) && error("Aquamoto: the $name render could not be captured")
		return img.image
	end
	# A FRESH TARGET EVERY TIME. Keeping the window between layers and pushing the new grid through
	# `gmtvtk_replace_base_grid_h` was faster and WRONG: the water half came back as a picture of the
	# window BACKGROUND — its surface stopped being drawn after the swap, while the land half (the same
	# code, a grid with real relief) survived it. Measured on one deep-sea node, same slice, same call:
	#     reused target -> [25, 40, 59]     the background's own colour
	#     fresh  target -> [114, 114, 219]  the water's polar blue
	# The picture is the product; a faster way to make a wrong one is worth nothing. Reuse can come
	# back when the swap is understood, not before.
	h = get(_PBR_WIN, name, C_NULL)
	if h != C_NULL
		ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
		delete!(_PBR_WIN, name)
		_pump_once()
		h = C_NULL
	end
	fresh = true
	if fresh
		fig = view_grid(H; cmap = cmap, title = name)
		h = fig.h
		_PBR_WIN[name] = h
		# The surface needs its NAME on both sides — the C side's (what the dialog aims at) and the
		# Julia registry's (what `_find_object` hands back) — or the tool refuses the window.
		ccall(_fn(:gmtvtk_set_surface_name_h), Cvoid, (Ptr{Cvoid}, Cstring), h, name)
		# Straight down, flat 2-D, and SSAO off for good: a screen-space pass does not stitch under
		# `gmtvtk_capture_rect_rgb`'s scale-2 tiled magnification and left a seam at the picture's row
		# and column midlines (7.89/255; 0.03 with it off, while tone mapping and FXAA changed nothing).
		# These windows exist only to be photographed, so the setting is made once and left.
		ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, "imgmode=0;flat2d=1;ssao=0;")
		# …and photograph it at 1x, not the interactive 2x. The grab costs its pixel count, and the
		# consumer (`_aqua_shaded_rgb`) needs the picture at the grid's own node resolution anyway, so
		# every extra pixel is bought and thrown away — it was 0.146 s of the 0.2 s a half took.
		haskey(_LIB_FNS, :gmtvtk_set_capture_scale_h) &&
			ccall(_fn(:gmtvtk_set_capture_scale_h), Cvoid, (Ptr{Cvoid}, Cint), h, Cint(1))
		# NOT MOVED OFF THE SCREEN. It was, to keep two render targets from flickering beside the user's
		# window during an animation — and that is what turned the water half into a picture of the
		# window BACKGROUND: a window sitting at (-32000, -32000) stops drawing, and the grab then
		# returns whatever is behind it. Measured: sea pixels came back [25,39,59], the background's own
		# colour, while the land half (drawn in a window that still had a framebuffer) was correct.
		# The picture is what matters; hiding these windows is a comfort that cost the image.
	else
		# A LATER FRAME: same window, same camera, new data — the in-place swap, not a new window.
		zb, nxc, nyc, zlay = _grid_zbuf(H)
		cz, crgb, ncol = _cpt_nodes_range(Float64(H.range[5]), Float64(H.range[6]), cmap)
		ccall(_fn(:gmtvtk_replace_base_grid_h), Cint,
		      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring, Cint),
		      h, zb, nxc, nyc, H.range[1], H.range[2], H.range[3], H.range[4], Cint(1),
		      cz, crgb, Cint(ncol), name, zlay)
	end
	_remember_object!(h, :grid, name, H)
	_pump_once()
	_on_hillshade(h, "model=1\ngrid=$name\nazim=$azim\nelev=$elev\n")
	_pump_once()
	# …AND WAIT FOR THE TILES. The base grid renders as a quadtree of LOD tiles refined only when the
	# CAMERA MOVES, so a window that is set up and then grabbed — no camera event ever fired — can be
	# captured before the pyramid has settled at this camera. `gmtvtk_lod_settle_h` runs refine passes
	# until one neither builds nor re-adds a tile.
	if haskey(_LIB_FNS, :gmtvtk_lod_settle_h)
		ccall(_fn(:gmtvtk_lod_settle_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(0))
		_pump_once()
	end
	img = _capture_rect_image(h, H.range[1], H.range[2], H.range[3], H.range[4];
	                          coords = false, prefer_screen = true)
	# CLOSED AS SOON AS IT HAS BEEN PHOTOGRAPHED. It is a render target, not a window the user asked
	# for: left standing until the next layer, the two of them (water and land) sit on screen beside
	# the Aquamoto window for the whole session.
	delete!(_PBR_WIN, name)
	ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
	_pump_once()
	(img === nothing) && error("Aquamoto: the $name render could not be captured")
	# `_capture_rect_image` already hands over a (row, col, band) array of its own (it de-interleaves
	# the grab on the way out), first row NORTH — the shape the combine takes from every half.
	return img.image
end

_pbr_pump(n::Int) = for _ in 1:n; _pump_once(); sleep(0.02); end

"""
    tsupbr(path; k=nothing, var="z", …) -> GMTimage

READ A TSUNAMI SLICE AND SHOW IT COMPOSED WITH METHOD 1 (VTK PBR) ON BOTH HALVES:

    using InteractiveGMT
    tsupbr("C:/v/tsu/tsu_ocean2/tsu_time.nc")
    tsupbr(path; k = 40, azim_land = 300.0)

It is `tsushade` with both methods 1 — same reader, same split, same combine. Each half is rendered
by VTK with its PBR material and captured; the two captures are combined by the dry/wet mask.
"""
function tsupbr(path::String; k::Union{Int,Nothing} = nothing, var::String = "z",
                azim_water::Float64 = 45.0, elev_water::Float64 = 30.0,
                azim_land::Float64  = 300.0, elev_land::Float64 = 25.0,
                cmap_water = :polar, cmap_land = :geo,
                savefile::String = "", show::Bool = true)
	nt = 0
	for v in _netcdf_subdatasets(path)
		v.name == var && (nt = v.dims[1])
	end
	nt > 0 || error("tsupbr: '$var' not found in $path")
	kk = k === nothing ? nt ÷ 2 : k
	(0 <= kk < nt) || error("tsupbr: layer $kk out of range (0..$(nt - 1))")
	bat = GMT.gmtread(path * "?bathymetry", layout = "TRB")
	G   = GMT.gmtread(path * "?$(var)[$(kk)]", layout = "TRB")
	img, = aqua_shade_image(bat, G; method_water = 1, method_land = 1,
	                        azim_water = azim_water, elev_water = elev_water,
	                        azim_land = azim_land, elev_land = elev_land,
	                        cmap_water = cmap_water, cmap_land = cmap_land)
	println("tsupbr: layer $(kk + 1)/$nt — both halves by method 1 (VTK PBR), " *
	        "water az $azim_water, land az $azim_land")
	isempty(savefile) || (GMT.gmtwrite(savefile, img); println("tsupbr: wrote $savefile"))
	show && GMT.imshow(img)
	return img
end
