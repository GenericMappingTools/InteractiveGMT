# ── Aquamoto: "Sat img" — ONE SATELLITE IMAGE ────────────────────────────────────────────────────
#
# With "Sat img" ticked the land wears a downloaded satellite mosaic. THERE IS EXACTLY ONE OF IT: the
# "Satellite image" row in Scene Objects, fetched once at the resolution the dialog's "Res" box asks
# for (`_aqua_sat_size`), draped on the relief with its water texels transparent so the live, lit
# water of the layer shows through. Everything else that needs the photograph — the method-1 land
# render and the "Rendered image" (`_aqua_sat_lit`) — reads THAT picture (`_aqua_sat_fetch`).
#
# There used to be three: a copy at one pixel per node baked into the layer's land colours, the
# draped one at Res, and a third fetched two zoom levels finer for the lit land render — three
# downloads, three resolutions, and a low-resolution photograph under a higher-resolution one. The
# layer's own land now keeps its colour map, lit like any grid, under the drape.
#
# GEOMETRY (SACRED_LAW.md, derived-from-a-grid geometry law): the mosaic is warped onto the grid's
# exact box, W x H texels, and a texel shows the node its registration says it lies on
# (`_aqua_sat_node`) — the same rule in the full build and in the per-slice cut.

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

# THE SATELLITE IMAGE, its own Scene Objects row, draped on the relief. Its WATER texels are fully
# transparent, so the live, lit water of the layer shows through; the dry/wet mask moves with the
# wave, so the alpha is re-cut at every slice (`_aqua_sat_drape!`).
const AQUA_SAT_NAME = "Satellite image"
const _AQUA_SAT_MAXPIX = 16_000_000

# THE ONE SIZE: W x H texels = the node count times "Res" (capped, aspect kept), fetched at the tile
# zoom whose ground resolution matches. Every user of the photograph asks here, so none can pick a
# resolution of its own.
function _aqua_sat_size(st::_AquaState)::NTuple{3,Int}
	nx, ny = _grid_dims(st.bat)
	f = get(_AQUA_SAT_FACTOR, st, 1.0)
	W, H = max(2, round(Int, nx * f)), max(2, round(Int, ny * f))
	if W * H > _AQUA_SAT_MAXPIX
		sc = sqrt(_AQUA_SAT_MAXPIX / (W * H));  W = max(2, round(Int, W * sc));  H = max(2, round(Int, H * sc))
	end
	return (W, H, _aqua_sat_zoom(st.bat, f))
end

# THE ONE FETCH: the mosaic warped onto the layer's box at W x H — as the warped image (what `grdimage`
# lights, `_aqua_sat_lit`) and as (row, col, band) RGB with row 1 = SOUTH (what the drape is built
# from). One entry: the current box, size and zoom; the tiles themselves live in GMT's cache.
const _AQUA_SAT_PIC = Ref{Any}(nothing)      # (key, warped GMTimage, S)

function _aqua_sat_fetch(bat::GMTgrid, W::Int, H::Int, zoom::Int)::Tuple{GMTimage,Array{UInt8,3}}
	box = (Float64(bat.range[1]), Float64(bat.range[2]), Float64(bat.range[3]), Float64(bat.range[4]))
	key = (box, W, H, zoom)
	hit = _AQUA_SAT_PIC[]
	(hit !== nothing && hit[1] == key) && return (hit[2], hit[3])
	I = lock(_TILE_LOCK) do                    # one lock for every tile fetch (GMT is not reentrant)
		GMT.mosaic([box[1], box[2]], [box[3], box[4]]; zoom = zoom, cache = "gmt")
	end
	# PIXEL-INTERLEAVED IN, BAND-PLANAR TO GDAL (`_to_band_planar`, drape.jl): the row-major branch
	# reads the buffer as band-planar and hands back colour noise.
	opts = ["-t_srs", "EPSG:4326", "-te", string(box[1]), string(box[3]), string(box[2]), string(box[4]),
	        "-ts", string(W), string(H), "-r", "bilinear"]
	Wr = GMT.gdalwarp(_to_band_planar(I), opts)
	pix, nb, nlon, nlat, rowmajor = _pixaccess_img(Wr)
	(nlon == W && nlat == H) || error("Aquamoto: the satellite image warped to $(nlon)x$(nlat), not $(W)x$(H)")
	north = _north_first(Wr.layout, rowmajor)
	S = Array{UInt8,3}(undef, H, W, 3)
	@inbounds for r in 1:H
		lat = north ? (H - r + 1) : r
		for c in 1:W, b in 1:3
			S[r, c, b] = UInt8(pix(lat, c, b <= nb ? b : nb))
		end
	end
	_AQUA_SAT_PIC[] = (key, Wr, S)
	return (Wr, S)
end

# The drape's RGBA: the satellite colours `S` (H x W x 3) with the land/water mask `dm` ((ny, nx),
# row 1 = SOUTH, true = dry) as alpha — water is see-through to the lit layer.
# A FUNCTION BARRIER, and that is the whole point of it being separate. In `_aqua_sat_drape!` the
# mask is built from a grid whose type is a Union with a non-concrete GMTgrid (`_aqua_layer`), so
# `dm` was inferred `Any` and this loop dispatched dynamically on every one of its W*H pixels:
# 1.2 s per slice at Res 4 on a 765x476 tank, which is what froze the < / > hold with "Sat img" on.
# Here every argument is concrete and the loop is tens of milliseconds.
#
# WHICH NODE A TEXEL SHOWS depends on the grid's REGISTRATION (SACRED_LAW.md, derived-from-a-grid
# geometry): the texture spans the grid's `range`, which is node to node for a gridline grid (texel u
# lands on node u*(nx-1), nearest) and cell EDGE to cell edge for a pixel-registered one (texel u lies
# inside cell floor(u*nx)). Using the gridline rule on a pixel grid put the cut half a cell off.
# `gmtvtk_image_set_alpha_mask_h` (90_c_api.cpp) samples with exactly this rule.
_aqua_sat_node(u::Float64, n::Int, pixreg::Bool)::Int =
	pixreg ? clamp(floor(Int, u * n) + 1, 1, n) : clamp(round(Int, u * (n - 1)) + 1, 1, n)

function _aqua_sat_rgba(S::Array{UInt8,3}, dm::BitMatrix, nx::Int, ny::Int, pixreg::Bool)::Array{UInt8,3}
	H, W = size(S, 1), size(S, 2)
	A = Array{UInt8,3}(undef, H, W, 4)
	ixs = [_aqua_sat_node((c - 0.5) / W, nx, pixreg) for c in 1:W]
	iys = [_aqua_sat_node((r - 0.5) / H, ny, pixreg) for r in 1:H]
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
	W, H, zoom = _aqua_sat_size(st)
	if !full && get(_AQUA_SAT_BUILT, st, (0, 0, 0)) == (W, H, zoom)
		mask = _aqua_pack_landmask(_aqua_indland(st.bat.z, Gw.z), _grid_layout_code(Gw), Int(nx), Int(ny))
		ccall(_fn(:gmtvtk_image_set_alpha_mask_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint, Cint, Cint),
		      scene, AQUA_SAT_NAME, mask, Cint(nx), Cint(ny), Cint(st.bat.registration == 1)) != 0 && return nothing
		# no such texture any more (removed from Scene Objects, say): build it whole below
	end
	delete!(_AQUA_SAT_BUILT, st)
	_, S = _aqua_sat_fetch(st.bat, W, H, zoom)
	dm = _aqua_indland(_zmat(st.bat), _zmat(Gw))   # (iy, ix), row 1 = SOUTH: true = dry land
	A = _aqua_sat_rgba(S, dm, nx, ny, st.bat.registration == 1)
	r = st.bat.range
	I = GMT.mat2img(A; x = [Float64(r[1]), Float64(r[2])], y = [Float64(r[3]), Float64(r[4])])
	I.layout = "BCBa"
	if !_update_image_pixels!(scene, AQUA_SAT_NAME, I)
		_add_image_to_scene(scene, I, AQUA_SAT_NAME; promote = false, record = false)
		_aqua_own!(scene, st.path, AQUA_SAT_NAME)
		ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, AQUA_SAT_NAME, Cint(1))
		ccall(_fn(:gmtvtk_image_set_draped_h), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, AQUA_SAT_NAME, Cint(1))
	end
	_AQUA_SAT_BUILT[st] = (W, H, zoom)
	return nothing
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

THE satellite image (`_aqua_sat_fetch`, at the one size `_aqua_sat_size`), lit by grid `G`'s reflectance
(`_hs_reflectance`, with `model` and the sun in `p` — method 1 is lit as 2, as in the composite)
through the same `grdimage -I` painter `_aqua_side_picture` uses. `G` must stand on the bathymetry's
nodes. Laid south-first, labelled "BCBa", the convention `_drape_buf` reads a grid-derived image by.
"""
function _aqua_sat_lit(st::_AquaState, G::GMTgrid, model::Int, p::Dict{String,String})::GMTimage
	J, _ = _aqua_sat_fetch(st.bat, _aqua_sat_size(st)...)       # the ONE picture, never a finer copy
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
