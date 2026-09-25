# ── Aquamoto: SATELLITE IMAGERY AS THE LAND SIDE'S ALBEDO ────────────────────────────────────────
#
# An Aquamoto layer is two images on two surfaces (SACRED_LAW.md, two-surface illumination law):
# water on the live stage, land on the static bathymetry. The land side has always been a COLOURMAP
# of the relief. With "Sat img" ticked it is a downloaded satellite mosaic instead — and nothing else
# about the side changes: the LIGHT is still `_hs_reflectance` over the bathymetry, per side, exactly
# as before. Only the albedo the light multiplies is swapped.
#
# THE ONE LAND ALBEDO. `_aqua_land_albedo(st)` (aquamoto.jl) is what every land painter asks — the
# flat composite, the lit combine (`aqua_shade_image`) and the two-surface land actor. A path that
# made its own land colours would be this file's whole purpose defeated.
#
# GEOMETRY IS NOT NEGOTIABLE (SACRED_LAW.md, derived-from-a-grid geometry law): the mosaic comes back
# on web-Mercator tiles at a zoom level, and it leaves here on the BATHYMETRY'S OWN NODES — same nx,
# same ny, one pixel per node, resampled by `gdalwarp` onto the exact outer box and node count. A
# count that does not match is an ERROR, never a stretch.
#
# ONE FETCH. `GMT.mosaic` keeps the tiles in GMT's own cache, and the finished per-node array is kept
# here too, keyed on the box, the node count, the provider and the zoom — the region does not change
# while a cube is being scrubbed, so the download happens once per window and never per slice.

# The finished land albedo, on the bathymetry's nodes, in ITS memory order. Key: the grid's box, its
# node count, the provider and the zoom — every input that can change a pixel.
const _AQUA_SAT_CACHE = Dict{Tuple{NTuple{4,Float64},Int,Int,String,Int},Array{UInt8,3}}()

"""
    _aqua_sat_zoom(bat) -> Int

The tile zoom whose ground resolution is closest to the grid's own cell size, so the warp below is a
near 1:1 resample instead of a big downsample of tiles nobody needed. Web-Mercator resolution is
`156543.034 * cos(lat) / 2^z` metres per pixel; the grid's is its x increment on the ground.
Clamped to the levels tile servers actually serve.
"""
function _aqua_sat_zoom(bat::GMTgrid, factor::Float64 = 1.0)::Int
	coslat = max(cos(0.5 * (Float64(bat.range[3]) + Float64(bat.range[4])) * pi / 180), 1e-6)
	m = _aqua_sat_res_m(bat, factor)
	(m > 0) || return 12
	return clamp(round(Int, log2(156543.03392 * coslat / m)), 1, 19)
end

# The land grid's node spacing ON THE GROUND (metres, x increment at the box's mid latitude) divided
# by the dialog's "Res" refinement factor: the satellite resolution that factor asks for. The ONE
# place that number is made — the hover text and the download zoom both read it.
function _aqua_sat_res_m(bat::GMTgrid, factor::Float64 = 1.0)::Float64
	nx, _ = _grid_dims(bat)
	coslat = max(cos(0.5 * (Float64(bat.range[3]) + Float64(bat.range[4])) * pi / 180), 1e-6)
	mx = abs(Float64(bat.range[2]) - Float64(bat.range[1])) / max(nx - 1, 1) * 111320.0 * coslat
	return (factor > 0) ? mx / factor : mx
end

"""
    _aqua_sat_rgb(bat; provider="", zoom=0) -> Array{UInt8,3}

The satellite mosaic over `bat`'s region, resampled onto `bat`'s OWN nodes and returned in `bat`'s
own memory order — i.e. exactly what `_aqua_colorize(bat, …)` returns, so it is a drop-in albedo for
every land painter. `provider` empty = GMT's own default (Bing aerial); `zoom` 0 = `_aqua_sat_zoom`.
"""
function _aqua_sat_rgb(bat::GMTgrid; provider::String = "", zoom::Int = 0)::Array{UInt8,3}
	nx, ny = _grid_dims(bat)
	z = zoom > 0 ? zoom : _aqua_sat_zoom(bat)
	box = (Float64(bat.range[1]), Float64(bat.range[2]), Float64(bat.range[3]), Float64(bat.range[4]))
	key = (box, nx, ny, provider, z)
	haskey(_AQUA_SAT_CACHE, key) && return _AQUA_SAT_CACHE[key]

	kw = Dict{Symbol,Any}(:zoom => z, :cache => "gmt")
	isempty(provider) || (kw[:provider] = provider)
	# Under the tiles lock: GMT is not reentrant, and the Tiles Tool can be fetching through the same
	# library from its own dialog. One lock for every tile fetch in this program, never a second one.
	I = lock(_TILE_LOCK) do
		GMT.mosaic([box[1], box[2]], [box[3], box[4]]; kw...)
	end
	# PIXEL-INTERLEAVED IN, BAND-PLANAR TO GDAL. A row-major RGB image sends GMT/GDAL down a branch
	# that reads the buffer as if it were band-planar and hands back colour noise — the same trap
	# `GMT.crop` is documented with. `_to_band_planar` (drape.jl) is THE de-interleaver.
	W = _aqua_sat_warp(_to_band_planar(I), bat)
	return _AQUA_SAT_CACHE[key] = _aqua_sat_in_grid_order(_aqua_sat_south_first(W, nx, ny), bat)
end

"""
    _aqua_sat_warp(I, bat) -> GMTimage

`I` resampled onto `bat`'s exact node geometry: one output pixel CENTRED on every node. `-te` wants
the outer EDGES of the raster, which for a pixel-registered grid is its `range` and for a gridline
one is `range` grown by half a cell — get that wrong and every pixel sits half a cell off its node,
which is the silent half-cell shift SACRED_LAW.md's geometry law exists to forbid.
"""
function _aqua_sat_warp(I::GMTimage, bat::GMTgrid)::GMTimage
	nx, ny = _grid_dims(bat)
	pixreg = (bat.registration == 1)
	incx = abs(Float64(bat.range[2]) - Float64(bat.range[1])) / (pixreg ? nx : max(nx - 1, 1))
	incy = abs(Float64(bat.range[4]) - Float64(bat.range[3])) / (pixreg ? ny : max(ny - 1, 1))
	hx = pixreg ? 0.0 : incx / 2
	hy = pixreg ? 0.0 : incy / 2
	opts = ["-t_srs", "EPSG:4326",
	        "-te", string(Float64(bat.range[1]) - hx), string(Float64(bat.range[3]) - hy),
	               string(Float64(bat.range[2]) + hx), string(Float64(bat.range[4]) + hy),
	        "-ts", string(nx), string(ny),
	        "-r", "bilinear"]
	return GMT.gdalwarp(I, opts)
end

"""
    _aqua_sat_south_first(I, nx, ny) -> Array{UInt8,3}

`I` as a plain `(iy, ix, band)` array with row 1 in the SOUTH — the convention `_zmat` uses for a
grid, so the two can be paired node for node. Read through `_pixaccess_img` / `_north_first`
(drape.jl), which are THE accessors for a GMTimage's real pixels whatever its layout claims.
"""
function _aqua_sat_south_first(I::GMTimage, nx::Int, ny::Int)::Array{UInt8,3}
	pix, nb, nlon, nlat, rowmajor = _pixaccess_img(I)
	(nlon == nx && nlat == ny) ||
		error("Aquamoto: the satellite mosaic warped to $(nlon)x$(nlat), not the bathymetry's $(nx)x$(ny)")
	north = _north_first(I.layout, rowmajor)
	S = Array{UInt8,3}(undef, ny, nx, 3)
	@inbounds for iy in 1:ny
		lat = north ? (ny - iy + 1) : iy
		for ix in 1:nx, b in 1:3
			S[iy, ix, b] = UInt8(pix(lat, ix, b <= nb ? b : nb))
		end
	end
	return S
end

"""
    _aqua_sat_in_grid_order(S, bat) -> Array{UInt8,3}

`S` (south-first) re-laid in `bat`'s own memory order, so it can be indexed element-wise beside
`bat.z` exactly as `_aqua_colorize`'s output is. The re-ordering itself is `_z_as` (drop.jl) — THE
one place on the Julia side a buffer is re-laid (SACRED_LAW.md's grid-memory-layout law); this only
carries each band through it.
"""
function _aqua_sat_in_grid_order(S::Array{UInt8,3}, bat::GMTgrid)::Array{UInt8,3}
	_grid_layout_code(bat) == 0 && return S            # "BCB" already IS (ny,nx) south-first
	out = Array{UInt8,3}(undef, size(bat.z, 1), size(bat.z, 2), 3)
	x, y = Float64.(bat.x), Float64.(bat.y)
	for b in 1:3
		# THE GRID'S OWN REGISTRATION: a pixel-registered grid's x/y are its nx+1 / ny+1 cell EDGES, and
		# mat2grid refuses them as gridline nodes ("size of x,y vectors incompatible").
		Sg = GMT.mat2grid(Float32.(S[:, :, b]); x = x, y = y, reg = Int(bat.registration))
		out[:, :, b] = round.(UInt8, _z_as(Sg, bat))
	end
	return out
end

"""
    _aqua_sat_image(rgb, bat) -> GMTimage

The land albedo as a georeferenced image on `bat`'s own box — what `grdimage` takes as the picture to
modulate with an intensity (`-I`), which is how the satellite land half is LIT by the very same
`_hs_reflectance` the colourmapped one is. Labelled "BCBa" and built south-first, the convention
`GMT.mat2img` yields from a grid-derived matrix and every consumer here is written around.
"""
function _aqua_sat_image(rgb::Array{UInt8,3}, bat::GMTgrid)::GMTimage
	S = _grid_layout_code(bat) == 0 ? rgb : _aqua_sat_south_first_of(rgb, bat)
	I = GMT.mat2img(S; x = Float64.(bat.x), y = Float64.(bat.y))
	I.layout = "BCBa"
	return I
end

# The inverse of `_aqua_sat_in_grid_order`: a land albedo held in the grid's memory order, read back
# as (iy, ix) south-first. `_zmat` is the accessor for that direction, so each band goes through it
# rather than through index arithmetic written a second time here.
function _aqua_sat_south_first_of(rgb::Array{UInt8,3}, bat::GMTgrid)::Array{UInt8,3}
	nx, ny = _grid_dims(bat)
	out = Array{UInt8,3}(undef, ny, nx, 3)
	for b in 1:3
		Gb = deepcopy(bat)
		Gb.z = Float32.(rgb[:, :, b])
		out[:, :, b] = round.(UInt8, Matrix(_zmat(Gb)))
	end
	return out
end

# THE SATELLITE LAND AT THE RESOLUTION "Res" ASKS FOR — ON SCREEN, not only in the download.
#
# The layer draws its land one pixel per bathymetry node, so a finer download averaged back onto the
# nodes showed nothing finer. The picture is therefore its OWN image, W x H = the node count times the
# factor (capped), fetched at the matching tile zoom, and DRAPED on the relief as a Scene Objects row
# of its own ("Satellite image") that the user sees, toggles and removes like any image. Its WATER
# texels are fully transparent, so the live, lit water of the layer shows through; the dry/wet mask
# moves with the wave, so the alpha is re-cut at every slice (`_aqua_sat_drape!`).
const AQUA_SAT_NAME = "Satellite image"
const _AQUA_SAT_MAXPIX = 16_000_000
const _AQUA_SAT_DRAPE_CACHE = Dict{Tuple{NTuple{4,Float64},Int,Int,Int},Array{UInt8,3}}()

# The mosaic warped onto the layer's box at W x H, as (row, col, band) RGB, row 1 = SOUTH.
function _aqua_sat_drape_rgb(bat::GMTgrid, W::Int, H::Int, zoom::Int)::Array{UInt8,3}
	box = (Float64(bat.range[1]), Float64(bat.range[2]), Float64(bat.range[3]), Float64(bat.range[4]))
	key = (box, W, H, zoom)
	haskey(_AQUA_SAT_DRAPE_CACHE, key) && return _AQUA_SAT_DRAPE_CACHE[key]
	I = lock(_TILE_LOCK) do
		GMT.mosaic([box[1], box[2]], [box[3], box[4]]; zoom = zoom, cache = "gmt")
	end
	opts = ["-t_srs", "EPSG:4326", "-te", string(box[1]), string(box[3]), string(box[2]), string(box[4]),
	        "-ts", string(W), string(H), "-r", "bilinear"]
	Wr = GMT.gdalwarp(_to_band_planar(I), opts)
	pix, nb, nlon, nlat, rowmajor = _pixaccess_img(Wr)
	(nlon == W && nlat == H) || error("Aquamoto: the satellite drape warped to $(nlon)x$(nlat), not $(W)x$(H)")
	north = _north_first(Wr.layout, rowmajor)
	S = Array{UInt8,3}(undef, H, W, 3)
	@inbounds for r in 1:H
		lat = north ? (H - r + 1) : r
		for c in 1:W, b in 1:3
			S[r, c, b] = UInt8(pix(lat, c, b <= nb ? b : nb))
		end
	end
	empty!(_AQUA_SAT_DRAPE_CACHE)                 # one entry: the current box, size and zoom
	return _AQUA_SAT_DRAPE_CACHE[key] = S
end

# The drape's RGBA: the satellite colours `S` (H x W x 3) with the land/water mask `dm` ((ny, nx),
# row 1 = SOUTH, true = dry) as alpha — water is see-through to the lit layer.
# A FUNCTION BARRIER, and that is the whole point of it being separate. In `_aqua_sat_drape!` the
# mask is built from a grid whose type is a Union with a non-concrete GMTgrid (`_aqua_layer`), so
# `dm` was inferred `Any` and this loop dispatched dynamically on every one of its W*H pixels:
# 1.2 s per slice at Res 4 on a 765x476 tank, which is what froze the < / > hold with "Sat img" on.
# Here every argument is concrete and the loop is tens of milliseconds.
function _aqua_sat_rgba(S::Array{UInt8,3}, dm::BitMatrix, nx::Int, ny::Int)::Array{UInt8,3}
	H, W = size(S, 1), size(S, 2)
	A = Array{UInt8,3}(undef, H, W, 4)
	ixs = [clamp(round(Int, (c - 0.5) / W * (nx - 1)) + 1, 1, nx) for c in 1:W]
	iys = [clamp(round(Int, (r - 0.5) / H * (ny - 1)) + 1, 1, ny) for r in 1:H]
	@inbounds for c in 1:W, r in 1:H
		A[r, c, 1] = S[r, c, 1];  A[r, c, 2] = S[r, c, 2];  A[r, c, 3] = S[r, c, 3]
		A[r, c, 4] = dm[iys[r], ixs[c]] ? 0xff : 0x00
	end
	return A
end

# Put up / refresh / take down the satellite drape. `on` false removes it. Called by the "Sat img"
# box and after every slice while it is on (the land/water mask moves with the wave).
#
# ONLY THE CUT MOVES. The colours are the same photograph at every slice; what the wave changes is
# which nodes are land. So once the picture is up at a given size, a slice sends the per-node mask
# alone (`gmtvtk_image_set_alpha_mask_h`, nx*ny bytes) and the viewer rewrites the texture's alpha in
# place, with the same sampling `_aqua_sat_rgba` uses. Rebuilding and re-sending the whole RGBA each
# slice (3060x1904 at Res 4, plus a forced render inside the pixel setter) is what made < / > crawl
# with "Sat img" on. `full = true` forces the whole build (the test compares the two).
const _AQUA_SAT_BUILT = IdDict{_AquaState,NTuple{3,Int}}()   # (W, H, zoom) of the picture now up

function _aqua_sat_drape!(scene::Ptr{Cvoid}, st::_AquaState, G::Union{GMTgrid,Nothing} = nothing;
                          full::Bool = false)
	if !st.satimg
		delete!(_AQUA_SAT_BUILT, st)
		if ccall(_fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), scene, AQUA_SAT_NAME) != 0
			_forget_object!(scene, :image, AQUA_SAT_NAME)
		end
		return nothing
	end
	Gw = G === nothing ? _aqua_layer(st, st.cur) : G
	Gw === nothing && return nothing
	nx, ny = _grid_dims(st.bat)
	f = get(_AQUA_SAT_FACTOR, st, 1.0)
	W, H = max(2, round(Int, nx * f)), max(2, round(Int, ny * f))
	if W * H > _AQUA_SAT_MAXPIX                    # capped, aspect kept
		sc = sqrt(_AQUA_SAT_MAXPIX / (W * H));  W = max(2, round(Int, W * sc));  H = max(2, round(Int, H * sc))
	end
	zoom = _aqua_sat_zoom(st.bat, f)
	if !full && get(_AQUA_SAT_BUILT, st, (0, 0, 0)) == (W, H, zoom)
		mask = _aqua_pack_landmask(_aqua_indland(st.bat.z, Gw.z), _grid_layout_code(Gw), Int(nx), Int(ny))
		ccall(_fn(:gmtvtk_image_set_alpha_mask_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint, Cint),
		      scene, AQUA_SAT_NAME, mask, Cint(nx), Cint(ny)) != 0 && return nothing
		# no such texture any more (removed from Scene Objects, say): build it whole below
	end
	delete!(_AQUA_SAT_BUILT, st)
	S = _aqua_sat_drape_rgb(st.bat, W, H, zoom)
	dm = _aqua_indland(_zmat(st.bat), _zmat(Gw))   # (iy, ix), row 1 = SOUTH: true = dry land
	A = _aqua_sat_rgba(S, dm, nx, ny)
	r = st.bat.range
	I = GMT.mat2img(A; x = [Float64(r[1]), Float64(r[2])], y = [Float64(r[3]), Float64(r[4])])
	I.layout = "BCBa"
	if !_update_image_pixels!(scene, AQUA_SAT_NAME, I)
		_add_image_to_scene(scene, I, AQUA_SAT_NAME; promote = false, record = false)
		ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, AQUA_SAT_NAME, Cint(1))
		ccall(_fn(:gmtvtk_image_set_draped_h), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, AQUA_SAT_NAME, Cint(1))
	end
	_AQUA_SAT_BUILT[st] = (W, H, zoom)
	return nothing
end

# THE 3-D DRAPE IS NOT BOUND TO THE NODES. The per-node albedo above feeds the 2-D composite, which is
# one pixel per node by construction. A texture draped on the land SURFACE has no such limit, so it is
# fetched finer (two zoom levels) and warped onto the grid's box at up to 4x the node count. Cached on
# the box and the texture size; the tiles themselves live in GMT's cache.
const _AQUA_SAT_TEX_MAX = 4096
const _AQUA_SAT_HI_CACHE = Dict{Tuple{NTuple{4,Float64},Int,Int},GMTimage}()

function _aqua_sat_hires(bat::GMTgrid)::GMTimage
	nx, ny = _grid_dims(bat)
	box = (Float64(bat.range[1]), Float64(bat.range[2]), Float64(bat.range[3]), Float64(bat.range[4]))
	f = clamp(_AQUA_SAT_TEX_MAX ÷ max(nx, ny), 1, 4)
	W, Hh = nx * f, ny * f
	key = (box, W, Hh)
	haskey(_AQUA_SAT_HI_CACHE, key) && return _AQUA_SAT_HI_CACHE[key]
	z = min(_aqua_sat_zoom(bat) + 2, 19)
	I = lock(_TILE_LOCK) do
		GMT.mosaic([box[1], box[2]], [box[3], box[4]]; zoom = z, cache = "gmt")
	end
	# ONTO THE BOX THE SURFACE'S TEXTURE COORDINATES SPAN: u,v run 0..1 over x0..x1 / y0..y1 (the
	# grid's range, makeGridFromArray), so the picture's outer edges are exactly that range.
	opts = ["-t_srs", "EPSG:4326",
	        "-te", string(box[1]), string(box[3]), string(box[2]), string(box[4]),
	        "-ts", string(W), string(Hh), "-r", "bilinear"]
	return _AQUA_SAT_HI_CACHE[key] = GMT.gdalwarp(_to_band_planar(I), opts)
end

# The node reflectance `R` ((ny, nx), row 1 = SOUTH) at every pixel of a W x Hh texture over the same
# box: node k sits at u = (k-1)/(nx-1), pixel i's centre at u = (i-0.5)/W. Bilinear; a NaN node (the
# other side) stays NaN around it, which grdimage leaves unshaded.
function _aqua_upsample_nodes(R::AbstractMatrix, W::Int, Hh::Int)::Matrix{Float32}
	ny, nx = size(R)
	out = Matrix{Float32}(undef, Hh, W)
	@inbounds for j in 1:Hh
		fy = ((j - 0.5) / Hh) * (ny - 1) + 1
		y0 = clamp(floor(Int, fy), 1, ny - 1); ty = Float32(fy - y0)
		for i in 1:W
			fx = ((i - 0.5) / W) * (nx - 1) + 1
			x0 = clamp(floor(Int, fx), 1, nx - 1); tx = Float32(fx - x0)
			a = (1 - tx) * R[y0, x0]     + tx * R[y0, x0 + 1]
			b = (1 - tx) * R[y0 + 1, x0] + tx * R[y0 + 1, x0 + 1]
			out[j, i] = (1 - ty) * a + ty * b
		end
	end
	return out
end

"""
    _aqua_sat_lit(st, G, model, p) -> GMTimage

The satellite mosaic at texture resolution (`_aqua_sat_hires`), lit by grid `G`'s reflectance
(`_hs_reflectance`, with `model` and the sun in `p` — method 1 is lit as 2, as in the composite)
through the same `grdimage -I` painter `_aqua_side_picture` uses. `G` must stand on the bathymetry's
nodes. Laid south-first, labelled "BCBa", the convention `_drape_buf` reads a grid-derived image by.
"""
function _aqua_sat_lit(st::_AquaState, G::GMTgrid, model::Int, p::Dict{String,String})::GMTimage
	J = _aqua_sat_hires(st.bat)
	_, _, W, Hh, _ = _pixaccess_img(J)
	m = (model in (2, 3, 4)) ? model : 2
	az = _get(p, "azim") == "" ? "45" : _get(p, "azim")
	el = _get(p, "elev") == "" ? "30" : _get(p, "elev")
	Rn = _hs_reflectance(G, m, Dict{String,String}("azim" => az, "elev" => el))
	Rh = _aqua_upsample_nodes(Rn, W, Hh)
	b = st.bat.range
	Rg = GMT.mat2grid(Rh; reg = 1, x = collect(range(b[1], b[2], length = W + 1)),
	                  y = collect(range(b[3], b[4], length = Hh + 1)))
	rgb = _aqua_rgb_plane(GMT.grdimage(J, I = Rg, A = ""))          # row 1 = NORTH
	I = GMT.mat2img(reverse(rgb, dims = 1))
	I.layout = "BCBa"
	return I
end

"""
    _aqua_land_drape(scene, st) -> Union{Nothing,GMTimage}

The LAND half as a picture to drape on a land SURFACE: `nothing` unless "Sat img" is on. The land
half the last combine cut (`_AQUA_HALF_GRID`), lit with the land's own model and sun.
"""
function _aqua_land_drape(scene::Ptr{Cvoid}, st::_AquaState)::Union{Nothing,GMTimage}
	st.satimg || return nothing
	H = get(_AQUA_HALF_GRID, AQUA_LAND, nothing)
	H === nothing && return nothing
	_, ml = get(_AQUA_LAST_MODELS, scene, (2, 2))
	return _aqua_sat_lit(st, H, ml, st.illum[2])
end
