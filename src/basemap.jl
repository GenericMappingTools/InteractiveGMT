# basemap.jl — World Topo Tiles basemap picker (ported from Mirone's bg_map.m, src_figs/bg_map.m).
# The C++ Qt dialog (data/etopo4_logo.jpg world image + a 4x8 tile grid) calls `_on_basemap` with a
# clicked tile's geographic region; we crop the big data/etopo4.jpg to it with GMT.jl
# (gdaltranslate -srcwin), tag it WGS84 (EPSG:4326) and add it to the receiving window as a
# referenced flat image, on top of any existing grid/image.
#
# The big-image dims (5400x2700, covering [-180 180]/[-90 90]) come from etopo4.jpg — the same
# magic numbers bg_map.m hard-codes; change them here if the bundled image is swapped.
#
# As with the console + drop callbacks, the @cfunction and its registration are RUNTIME values,
# created in __init__ (a precompiled @cfunction is invalid), never at top level.

const _ETOPO4_NX = 5400
const _ETOPO4_NY = 2700

_basemap_dir() = joinpath(_PKGROOT, "data")
_etopo4_path() = joinpath(_basemap_dir(), "etopo4.jpg")
_etopo4_logo() = joinpath(_basemap_dir(), "etopo4_logo.jpg")
_basemap_icon() = joinpath(_basemap_dir(), "basemap_icon.png")

# Geographic region (W/E/S/N in the etopo4 [-180 180]/[-90 90] domain) -> gdal -srcwin pixel window
# (xoff yoff xsize ysize), clamped to the image. Mirrors bg_map.m's pixel arithmetic (origin = the
# image's UPPER-left, so the y offset counts down from the north edge).
function _etopo4_srcwin(W, E, S, N)
	xoff = clamp(round(Int, (W + 180) / 360 * _ETOPO4_NX), 0, _ETOPO4_NX - 1)
	yoff = clamp(round(Int, (90 - N)  / 180 * _ETOPO4_NY), 0, _ETOPO4_NY - 1)
	xs   = clamp(round(Int, (E - W)   / 360 * _ETOPO4_NX), 1, _ETOPO4_NX - xoff)
	ys   = clamp(round(Int, (N - S)   / 180 * _ETOPO4_NY), 1, _ETOPO4_NY - yoff)
	return xoff, yoff, xs, ys
end

# Crop the big etopo4.jpg to a geographic region, returning a GMTimage.
function _crop_etopo4(W, E, S, N)
	xoff, yoff, xs, ys = _etopo4_srcwin(W, E, S, N)
	return GMT.gdaltranslate(_etopo4_path(), "-srcwin $xoff $yoff $xs $ys")
end

# [0 360] whole-world view: chop the image at lon=0, swap the two halves so the western hemisphere
# ([-180 0]) sits AFTER the eastern ([0 180]), then the longitudes run 0..360 -> a Pacific-centred
# Earth (mirrors bg_map.m's column concat). etopo4 is band-planar rowmajor ("TRBa": dim1=lon,
# dim3=band), so a circshift of the lon dim by half the width does the swap without touching bands.
function _roll_lon_to_360!(I)
	lay = I.layout
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	londim   = rowmajor ? 1 : 2
	half     = size(I.image, londim) ÷ 2
	shifts   = ntuple(d -> d == londim ? -half : 0, ndims(I.image))
	I.image  = circshift(I.image, shifts)
	return I
end

# Per-window set of already-loaded basemap handle names, so re-clicking a loaded tile is ignored.
const _BASEMAP_LOADED = Dict{Ptr{Cvoid}, Set{String}}()

# C callback: copt = "W/E/S/N/wrap/name" — wrap=1 presents the X range in [0 360]; name is the tile
# id ("RxC", 1-based row x col) or "global". The handle is named "Base image (name)". Crop the tile,
# tag it WGS84, and add it to `scene` via the proven drop-path: an EMPTY launcher is PROMOTED in
# place (real scales + geographic coord grid + coord readout + centred view); a populated window
# keeps its view and gets the tile on top as an ExtraObj image.
# Re-selecting an already-loaded tile in the same window is ignored.
function _on_basemap(scene::Ptr{Cvoid}, copt::Cstring)::Cvoid
	try
		p = split(unsafe_string(copt), '/')
		W, E, S, N = parse.(Float64, p[1:4])
		wrap = length(p) >= 5 && strip(p[5]) == "1"
		tag  = length(p) >= 6 ? String(strip(p[6])) : "global"
		# A rubber-band sub-region ("region") is a fresh rectangle each time -> give it a unique name so
		# successive draws each add their own image (tiles keep the dedup so re-clicking one is a no-op).
		name = tag == "region" ?
			"Base image ($(round(W;digits=1))/$(round(E;digits=1))/$(round(S;digits=1))/$(round(N;digits=1)))" :
			"Base image ($tag)"
		loaded = get!(() -> Set{String}(), _BASEMAP_LOADED, scene)
		name in loaded && return                                 # already on this window -> ignore
		if wrap && tag == "global"
			# Whole world in [0 360]: swap hemispheres at lon=0 -> Pacific-centred Earth, lon 0..360.
			I = _crop_etopo4(-180.0, 180.0, S, N)
			_roll_lon_to_360!(I)
			dW, dE = 0.0, 360.0
		else
			# A tile/region: same pixels, only relabel a western piece into [180 360] for the wrap view.
			I = _crop_etopo4(W, E, S, N)
			dW, dE = (wrap && W < 0) ? (W + 360.0, E + 360.0) : (Float64(W), Float64(E))
		end
		I.range = [Float64(dW), Float64(dE), Float64(S), Float64(N), 0.0, 255.0]
		I.proj4 = "+proj=longlat +datum=WGS84 +no_defs"          # so _isgeog(I) -> geographic axes
		I.epsg  = 4326
		# An EMPTY launcher has no axes (blankStart). To give the tile the SAME ExtraObj image row +
		# properties menu it gets on a populated window, we first PROMOTE a blank flat geographic base
		# over the tile bbox (gmtvtk_promote_surface_h lays down the framed axes + coord readout +
		# centred flat-2-D view), then add the tile itself as an ExtraObj image on top. A populated
		# window already has the frame, so the tile is just added on top.
		if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
			zblank = zeros(Float32, 2, 2)
			ccall(_fn(:gmtvtk_promote_surface_h), Cint,
			      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
			       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
			      scene, zblank, Cint(2), Cint(2), Float64(dW), Float64(dE), Float64(S), Float64(N),
			      Cint(1), C_NULL, C_NULL, Cint(0), C_NULL, Cint(0), Cint(0), Cint(0), Cint(1), "")
			ccall(_fn(:gmtvtk_hide_surface), Cvoid, (Ptr{Cvoid},), scene)   # plane is scaffold only
		else
			# Already a flat geographic map: extend the frame + axes + hover domain to cover this tile
			# too (no-op if it already fits). Keeps xfac fixed so the existing tiles stay aligned.
			ccall(_fn(:gmtvtk_grow_frame_h), Cint,
			      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble),
			      scene, Float64(dW), Float64(dE), Float64(S), Float64(N))
		end
		_add_image_to_scene(scene, I, name; promote=false)   # always an ExtraObj image -> has the properties menu
		ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
			  scene, I.proj4, "", Cint(4326))                    # referenced -> reveals Geography menu
		push!(loaded, name)
	catch e
		_viewer_log_error(scene, "Base Map FAILED: $(sprint(showerror, e))")
		@warn "basemap: could not crop/add the tile" exception=(e,)
	end
	return
end

# Build the C-callable pointer + push the logo path to the viewer. Called once from __init__.
function _register_basemap()
	fptr = @cfunction(_on_basemap, Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_basemap_callback), Cvoid, (Ptr{Cvoid},), fptr)
	ccall(_fn(:gmtvtk_set_basemap_logo), Cvoid, (Cstring,), _etopo4_logo())
	ccall(_fn(:gmtvtk_set_basemap_icon), Cvoid, (Cstring,), _basemap_icon())
	return
end
