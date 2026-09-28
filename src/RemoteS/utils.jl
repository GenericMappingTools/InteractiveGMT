struct MTL_short
	band::Int
	rad_mul::Float64
	rad_add::Float64
	rad_max::Float64
	reflect_mul::Float64
	reflect_add::Float64
	reflect_max::Float64
	sun_azim::Float64
	sun_elev::Float64
	sun_dist::Float64
	K1::Float64
	K2::Float64
end

const Lsat8_desc = Dict{Int,String}(
	1  => "Band1 - Coastal aerosol [0.43-0.45]",
	2  => "Band2 - Blue [0.45-0.51]",
	3  => "Band3 - Green [0.53-0.59]",
	4  => "Band4 - Red [0.64-0.67]",
	5  => "Band5 - NIR [0.85-0.88]",
	6  => "Band6 - SWIR1 [1.57-1.65]",
	7  => "Band7 - SWIR2 [2.11-2.29]",
	9  => "Band9 - Cirrus [1.36-1.38]",
	10 => "Band10 - Thermal IR1 [10.6-11.19]",
	11 => "Band11 - Thermal IR2 [11.50-12.51]")

const Sentinel2_10m_desc = Dict{Int,String}(
	2  => "Band2 - Blue [0.490]",
	3  => "Band3 - Green [0.560]",
	4  => "Band4 - Red [0.665]",
	8  => "Band8 - NIR [0.842]")

const Sentinel2_20m_desc = Dict{Int,String}(	# Common to both 20 & 60 m
	1  => "Band1 - Coastal aerosol [0.443]",	# Actually only available in 60 m.
	2  => "Band2 - Blue [0.490]",
	3  => "Band3 - Green [0.560]",
	4  => "Band4 - Red [0.665]",
	5  => "Band5 - Red Edge1 [0.705]",
	6  => "Band6 - Red Edge2 [0.740]",
	7  => "Band7 - Red Edge3 [0.783]",
	8  => "Band8A - Red Edge4 (NIR) [0.865]",	# ~Landsat8 Band 5
	9  => "Band9 - Water vapour [0.945]",
	10 => "Band10 - Cirrus [1.375]",			# ~Landsat8 Band 9
	11 => "Band11 - SWIR1 [1.610]",				# ~Landsat8 Band 6
	12 => "Band12 - SWIR2 [2.190]")				# ~Landsat8 Band 7

# ----------------------------------------------------------------------------------------------------------
"""
    Irgb = truecolor(bndR, bndG, bndB)

Take three Landsat8/Sentinel2 UINT16 GMTimages or the file names of those bands and compose
an RGB true color image applying automatic histogram stretching.

Return an UInt8 RGB GMTimage

    Irgb = truecolor(cube::GMTImage, bands::Vector{Int})

Make an RGB composition of the 3 bands passed in the vector 'bands' from the layers in the multi-layered GMTimage `cube`

Return an auto-stretched UInt8 RGB GMTimage

    Irgb = truecolor(cube::String, [bands::Vector{Int}], [bandnames::Vector{String}], [raw=false])

Make an RGB composition of 3 bands from the `cube` file holding a UInt16 multi-layered array (often created with `cutcube`)
The band selection can be made with `bands` vector, case in which we will search for bands named "Band[k]"
or where the bands description contain the contents of `bandnames`. If none of `bands` or `bandnames` is used
we search for a made up `bandnames=["red", "green", "blue"]`.

Return an auto-stretched UInt8 RGB GMTimage OR a GMTimage{UInt16,3} if the `raw` option is set to `true`.

    Irgb = truecolor(cube::GMTgrid, [bands|layers::Vector{Int}], [bandnames::Vector{String}], [type=UInt8])

Make an RGB composition of 3 bands from the `cube` file holding a Float32 multi-layered array.
The band selection can be made with `bands` vector, case in which we will search for bands named "Band[k]"
or where the bands description contain the contents of `bandnames`. If none of `bands` or `bandnames` is used
we search for a made up `bandnames=["red", "green", "blue"]`.

By default we scale the bands to 0-255. Use `type=UInt16` to scale the bands to 0-65535`. Note that this
will matter only for the guessing of the good limits to perform the histogram stretching.


### Example:
Make an RGB composite from data in the cube file "LC08__cube.tiff"
```julia
I = truecolor("LC08__cube.tiff");
```
"""
function truecolor(bndR, bndG, bndB)
	IR = _as_image(bndR)
	img = Array{UInt8}(undef, size(IR,1), size(IR,2), 3)
	_stretch_into!(img, IR, 1)
	I = _as_image(bndG);  _stretch_into!(img, I, 2)
	I = _as_image(bndB);  _stretch_into!(img, I, 3)
	Io = mat2img(img, I);
	Io.layout = (isa(bndR, GMT.GMTimage)) ? "T" * bndR.layout[2] * "Ba" : "TRBa"	# This is shitty fragile
	(isa(bndR, GMT.GMTimage) && startswith(bndR.layout, "BC")) && (Io.layout = "BCBa")	# Horrible patch that needs to know why.
	Io
end

_as_image(x::GMT.GMTimage)::GMT.GMTimage = x
_as_image(x::String)::GMT.GMTimage = gmtread(x)

# One band into layer `k` of `img`: copied when already UInt8, auto-stretched otherwise.
function _stretch_into!(img::Array{UInt8,3}, I::GMT.GMTimage, k::Int)
	@assert size(I,1) == size(img,1) && size(I,2) == size(img,2)
	if (eltype(I) == UInt8)
		img[:,:,k] .= I.image
	else
		_ = mat2img(I.image, stretch=true, img8=view(img,:,:,k), scale_only=1)
	end
	return nothing
end

# ----------------------------------------------------------------------------------------------------------
# This method fall into the description of the general 'truecolor(bndR, bndG, bndB)'
function truecolor(bandR::T, bandG::T, bandB::T) where {T<:Union{GMT.GMTgrid{<:AbstractFloat, 2},Matrix{<:AbstractFloat}}}
	@assert size(bandR) == size(bandG) == size(bandB)
	img = Array{UInt8}(undef, size(bandR,1), size(bandR,2), 3)
	_scale_u8!(view(img,:,:,1), _band_array(bandR), _band_minmax(bandR))
	_scale_u8!(view(img,:,:,2), _band_array(bandG), _band_minmax(bandG))
	_scale_u8!(view(img,:,:,3), _band_array(bandB), _band_minmax(bandB))
	return isa(bandR, GMTgrid) ? mat2img(img, bandR) : mat2img(img)
end

_band_minmax(G::GMT.GMTgrid)::Tuple{Float64,Float64} = (Float64(G.range[5]), Float64(G.range[6]))
_band_minmax(A::AbstractMatrix)::Tuple{Float64,Float64} = (mm = GMT.extrema_nan(A); (Float64(mm[1]), Float64(mm[2])))

function _scale_u8!(out::AbstractMatrix{UInt8}, band::AbstractMatrix{<:AbstractFloat}, mima::Tuple{Float64,Float64})
	if (mima == (0.0, 1.0))
		@inbounds for k in eachindex(out, band)  out[k] = round(UInt8, band[k] * 255)  end
	else
		d = 255.0 / (mima[2] - mima[1])
		@inbounds for k in eachindex(out, band)  out[k] = round(UInt8, (band[k] - mima[1]) * d)  end
	end
	return nothing
end

truecolor(cube::GMT.GMTimage{UInt16, 3}, layers::Vector{Int}; stretch=true) = truecolor(cube, layers=layers, stretch=stretch)
function truecolor(cube::GMT.GMTimage{UInt16, 3}; layers::Vector{Int}=Int[], stretch=true)
	(length(layers) != 3) && error("For an RGB composition 'bands' must be a 3 elements array and not $(length(layers))")
	(cube.layout[3] != 'B') && error("For an RGB composition the image object must be Band interleaved and not $(cube.layout)")
	img = Array{UInt8, 3}(undef, size(cube,1), size(cube,2), 3)
	layers = find_layers(cube, layers, 3)
	stch = (stretch == 1) ? true : stretch		# This bloody type unstable and does not test stupid inputs
	for k = 1:3
		_ = mat2img(@view(cube.image[:,:,layers[k]]), stretch=stch, img8=view(img,:,:,k), scale_only=1)
	end
	Io = mat2img(img, cube);	Io.layout = "TRBa"
	Io
end

truecolor(cube::GMT.GMTgrid{Float32, 3}, layers::Vector{Int}; stretch=true) = truecolor(cube, layers=layers, stretch=stretch)
function truecolor(cube::GMT.GMTgrid{Float32, 3}; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[],
	               bandnames::Vector{String}=String[], stretch=true, type::DataType=UInt8)
	(isempty(bands) && isempty(bandnames) && isempty(layers)) && (bandnames = ["red", "green", "blue"])
	isempty(layers) && (layers = find_layers(cube, bandnames, bands))
	(length(layers) != 3) && error("For an RGB composition 'bands' must be a 3 elements array and not $(length(layers))")
	stch = (stretch == 1) ? true : stretch		# This bloody type unstable and does not test stupid inputs
	img = Array{type, 3}(undef, size(cube,1), size(cube,2), 3)
	for k = 1:3
		img[:,:,k] = rescale(@view(cube.z[:,:,layers[k]]), stretch=stch, type=type)
	end
	Io = mat2img(img, cube);	Io.layout = "TRBa"
	Io
end

function truecolor(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[],
                   raw::Bool=false, stretch=true)
	# The `raw` option returns a GMTimage{UInt16, 3} and does not convert to UInt8 with auto-stretch (default)
	(isempty(bands) && isempty(bandnames) && isempty(layers)) && (bandnames = ["red", "green", "blue"])
	rgb = subcube(cube, bands=bands, layers=layers, bandnames=bandnames)
	(raw) && return rgb
	return (eltype(rgb) <: AbstractFloat) ? truecolor(rgb, layers=[1,2,3], stretch=stretch) : mat2img(rgb, stretch=stretch)
end

# ----------------------------------------------------------------------------------------------------------
"""
    subcube(cube::String; bands=Int[], bandnames=String[], layers=Int[])

Extracts a subcube from `cube` with the layers in the `bands` vector, case in which we will search for bands
named "Band band[k]", or those whose names correspond (even partially and case insensitive) to the descriptions
in `bandnames` string vector. This means that the options `bands` and `bandnames` can only be used in 'cubes'
with bands description. The `layers` option blindly extract the `cube` planes listed in the `layer` vector.

Returns a GMTimage

    subcube(cube::Union{GMT.GMTimage{UInt16, 3}, AbstractArray{<:AbstractFloat, 3}}; bands=Int[], bandnames=String[], layers=Int[])

Does the same but from an already in memory cube. Returns a type equal to the input type. No views, a data copy.

### Example
Extracts the Red, Green and Blue layers from a Landsat 8 cube created with `cutcube`

```
Irgb = subcube("LC08__cube.tiff", bandnames = ["red", "green", "blue"])
```
"""
function subcube(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[])
	# This is also used as a helper function in 'truecolor' and the radiometric indices functions.
	alllayers = isempty(bands) && isempty(bandnames) && isempty(layers)	# Read them all
	lay = isempty(layers) ? find_layers(cube, bands=bands, bandnames=bandnames, alllayers=alllayers)[1] : layers
	gmtread(cube, band=lay, layout="TRBa")		# This one is still UInt16
end

function subcube(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[])
	(isempty(bands) && isempty(bandnames) && isempty(layers)) && return cube	# Stupid but silently ignore it.
	lay = isempty(layers) ? find_layers(cube, bandnames, bands) : layers
	slicecube(cube, lay)
end

# ----------------------------------------------------------------------------------------------------------
function find_layers(cube::_Cube3, list::Vector{Int}, n_layers::Int)::Vector{Int}
	# The list of bands to pass to the calling fun. (A wavelength search on `cube.v` was never finished.)
	(maximum(list) >= 200) && error("The `cube` object does not have a frequencies (`v` coordinates) vector as required here.")
	(maximum(list) > size(cube,3)) && error("Not enough bands to satisfy the bands list request.")
	(length(list) != n_layers) && error("Need $(n_layers) bands but got $(length(list))")
	list
end

function find_layers(cube::AbstractArray, bandnames::Vector{String}=String[], bands::Vector{Int}=Int[])::Vector{Int}
	# Find the layers corresponding to the (parts of) contents of "bandnames"
	(!isa(cube, GMT.GMTimage{UInt16, 3}) && !isa(cube, GMT.GMTgrid{Float32, 3})) && error("'cube' must be a 3D GMTimage or GMTgrid")
	names::Vector{String} = cube.names
	(isempty(names)) && error("The `cube` object does not have a `names` (band names) assigned field as required here.")
	bn = (!isempty(bands)) ? ["Band $(bands[k])" for k = 1:length(bands)] : bandnames		# Create a bandnames vector
	helper_find_layers(lowercase.(names), bn)
end

function find_layers(fname::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], alllayers::Bool=false)
	# 'layers' just return itself after checking that the cube file actually contain that many layers
	# 'bands' search for "Band bands[k]"
	# 'bandnames' search the bands description for the first layer that contains bandnames[k]
	# Returns the numeric layers (1-based) corresponding to the search criteria and the bands description
	(!alllayers && isempty(bands) && isempty(layers) && isempty(bandnames)) &&
		error("Must use either the 'bands' OR the 'bandnames' option.")
	desc = _band_descriptions(fname)
	nbands = length(desc)
	(nbands < 2) && error("This file ($fname) does not contain cube data (more than one layer).")
	(!isempty(layers) && maximum(layers) > nbands) && error("Asked for more 'layers' than this cube contains.")

	(!isempty(layers)) && return layers, desc
	(all(desc .== "")) && error("This cube file has no band descriptions so cannot use the 'band' or 'bandnames' options.")
	(alllayers) && return collect(1:nbands), desc	# OK, just return them ALL

	bn = (!isempty(bands)) ? ["Band$(bands[k])" for k = 1:length(bands)] : bandnames		# Create a bandnames vector
	return helper_find_layers(lowercase.(desc), bn), desc
end

# The band descriptions of a raster file (one per band) and, optionally, its metadata.
function _band_descriptions(fname::String)::Vector{String}
	GMT.ressurectGDAL()		# Yep, shit, sometimes its needed.
	ds = GMT.Gdal.unsafe_read(fname)
	nbands = GMT.Gdal.nraster(ds)
	desc = String[GMT.Gdal.GDALGetDescription(GMT.Gdal.GDALGetRasterBand(ds.ptr, k)) for k = 1:nbands]
	GMT.Gdal.GDALClose(ds.ptr)
	return desc
end

helper_find_layers(desc::Vector{String}, band_names::String) = helper_find_layers(desc, [band_names])
function helper_find_layers(desc::Vector{String}, band_names::Vector{String})::Vector{Int}
	# Note, 'desc' is supposed to be in lowercase already.
	bnd_names = lowercase.(band_names)
	layers, n = zeros(Int, length(bnd_names)), 0
	for bnd_name in bnd_names
		((ind = findfirst(contains.(desc, bnd_name))) !== nothing) && (layers[n += 1] = ind)
	end
	(n != length(bnd_names)) && error("All or some of the names in $(band_names) are not present in the `cube` names/description.\n\tUse the `reportbands()` function to check the bands description.")
	layers
end

# ----------------------------------------------------------------------------------------------------------
"""
    reportbands(in; [layers=Int[]])
or

    reportbands(in, layer;)

Report the Bands description of the `in` input argument. This can be a GMTimage, a GMTgrid or a file name (a String) of
a 'cube' file. Normally one made with the `cutcube` function. When the use conditions of this function are not met,
either a warning or an error message (if too deep to be caught as a warning) will be issued.

- `layers`: When this optional parameter is used, report the description of the bands in the vector `layers`
- `layer`: A scalar with a unique band number. Alternative form to `reportbands(in, layers=[layer])`

Returns a string vector.
"""
reportbands(in, layer::Int) = reportbands(in, layers=[layer])
function reportbands(in::GMT.GItype; layers::Vector{Int}=Int[])
	names::Vector{String} = in.names
	isempty(names) && (println("This image object does not have a `names` assigned field"); return nothing)
	return (isempty(layers)) ? names : names[layers]
end
function reportbands(in::String; layers::Vector{Int}=Int[])
	lay, desc = find_layers(in, layers=layers, alllayers=isempty(layers))
	return desc[lay]
end
reportbands(in; layers::Vector{Int}=Int[]) =
	error("Bad input. Must be a GMTimage, a GMTgrid or a file name. Not $(typeof(in))")

# ----------------------------------------------------------------------------------------------------------
"""
    cutcube(names=String[], bands=Int[], template="", region=nothing, extension=".TIF", description=String[], mtl="", sentinel2=0, save="")

Cut a 3D cube out of a Landsat/Sentinel scene within a subregion `region` and a selection of bands.

- `names`: (optional) A vector with the individual bands full file name
- `bands`: When `names` is not provided give a vector of integers corresponding to the choosen bands.
           This works well for Landsat and most of Sentinel bands. However, in later case, there are also
           bands that contain characters, for example band 8A. In this case `bands` should be a vector of
           strings including the extension. _e.g._ ["02.jp2", "8A.jp2"]
- `template`: Goes together with the `bands` option. They are both composed a template * band[n] to recreate
           the full file name of each band.
- `region` Is the region to extract and must contain the extracting region limits as [W, E, S, N] or a
           GMT style -R string (without the leading "-R").
- `extension`: In case the `bands` is numeric but file extensions are not "*.TIF" (case insensitive),
           use the extension passed by this option.
- `description`: A vector of strings (as many as bands) with a description for each band. If not provided and
           the file is recognized as a Landasat 8, band description is added automatically, otherwise
           we build one with the bands file names. This info will saved if data is written to a file.
- `mtl`:   If reading from Landsat and the MTL file is not automatically found (you get an error) use this
           option to pass the full name of the MTL file.
- `sentinel2`: ESA is just unconsistent and names change with time and band numbers can have character (e.g. 8A)
           hence we need help to recognize Sentinel files so the known description can be assigned.
           Use `sentinel=10`, or `=20` or `=60` to indicate Sentinel files at those resolutions.
- `save`:  The file name where to save the output. If not provided, a GMTimage is returned.

Return: `nothing` if the result is written in file or a GMTimage otherwise.

## Examples

```julia
# Cut a Landsat 8 scene for a small region (in UTM) and return a GMTimage with 3 bands in UInt16.
temp = "C:\\SIG_AnaliseDadosSatelite\\SIG_ADS\\DadosEx2\\LC82040332015145LGN00\\LC82040332015145LGN00_B";
cube = cutcube(bands=[2,3,4], template=temp, region=[479670,492720,4282230,4294500])

# The same example as above but save the data in a GeoTIFF disk file and use a string for `region`
cutcube(bands=[2,3,4], template=temp, region="479670/492720/4282230/4294500", save="landsat_cube.tif")
```
"""
function cutcube(; names::Vector{String}=String[], bands::AbstractVector=Int[], template::String="",
                   region=nothing, extension::String=".TIF", description::Vector{String}=String[], save::String="", sentinel2::Int=0, mtl::String="")

	fnames = isempty(names) ? _cutcube_names(bands, template, extension) : copy(names)
	# Now test if any of the file names, either passed in or generated here, do not exist
	for k in eachindex(fnames)
		name = fnames[k]
		isfile(name) && continue
		if (startswith(splitdir(name)[2], "LC0") && length(template) > 3 && template[end-3:end-2] == "SR")	# For Landsat 9 termal bands are now _ST_B10
			alt = replace(name, "_SR_B" => "_ST_B")
			isfile(alt) ? (fnames[k] = alt) : error("File name $name does not exist in $(pwd()) Must stop here.")
		else
			error("File name $name does not exist in $(pwd()) Must stop here.")
		end
	end
	MTL = read_mtl_lines(fnames[1], mtl)
	meta = (MTL !== nothing) ? ["MTL=" * join(MTL, "\n")] : String[]

	# Little parsing of the -R string but does not test if W < E & S < N
	_region = _region_str(region, fnames[1])

	desc = assign_description(fnames, description, sentinel2)
	cube = grdcut(fnames[1], R=_region)
	mats = Any[isa(cube, GMTimage) ? cube.image : cube.z]
	for k = 2:length(fnames)
		B = grdcut(fnames[k], R=_region)
		push!(mats, isa(cube, GMTimage) ? B.image : B.z)
	end
	mat = cat(mats..., dims=3)
	cube = isa(cube, GMTimage) ? mat2img(mat, cube, names=desc) : mat2grid(mat, reg=cube.registration, x=cube.x, y=cube.y, proj4=cube.proj4, wkt=cube.wkt, names=desc)
	if (save != "")
		_, ext = splitext(save)
		if (lowercase(ext) == ".nc")		# Let save as a nc cube as well
			gdalwrite(cube, save, bands, dim_name="bands")
		else
			isempty(meta) ? gdaltranslate(cube, dest=save) : gdaltranslate(cube, dest=save, meta=meta)
		end
	end
	return (save != "") ? nothing : cube
end

# The band file names built from `template` * band * extension.
function _cutcube_names(bands::AbstractVector, template::String, extension::String)::Vector{String}
	(isempty(bands) || template == "") && error("When band file `names` are not provided, MUST indicate `bands` AND `template`")
	if isa(bands, Vector{<:Integer})
		names = String[@sprintf("%s%d%s", template, b, extension) for b in bands]
		if (!isfile(names[1]))		# Landsat uses "B2.TIF" and Sentinel "B02.jp2", try again.
			n_name = @sprintf("%s%.02d%s", template, bands[1], extension)
			!isfile(n_name) && error("Neither file name $(names[1]) nor $(n_name) do exist in $(pwd()) Check the file extensions and use the 'extension' option if needed.")
			names = String[@sprintf("%s%.02d%s", template, b, extension) for b in bands]
		end
		return names
	elseif isa(bands, Vector{<:AbstractString})
		return String[template * b for b in bands]
	end
	error("`bands` must a vector of Int or Strings.")
end

# A region, however given (nothing = the whole of `fname`), as a GMT -R argument without the "-R".
function _region_str(region, fname::String)::String
	if (region === nothing)							# Swallow the entire region
		r = grdinfo(fname, C=true)
		reg = Float64[r[1], r[2], r[3], r[4]]
	elseif isa(region, String)
		return startswith(region, "-R") ? region[3:end] : region	# Tolerate a region that starts with "-R"
	else
		reg = Float64[region[1], region[2], region[3], region[4]]
	end
	return @sprintf("%.12g/%.12g/%.12g/%.12g", reg[1], reg[2], reg[3], reg[4])
end

function assign_description(names::Vector{String}, description::Vector{String}, sentinel2::Int=0)::Vector{String}
	# Create a description for each band. If 'description', the cutcube() kwarg, is provided we use it as is.
	# Next we try to find if 'names' indicate a Landsat8 origin and if yes we use the known names & frequencies
	# Otherwise we use the file names as descriptors.
	(!isempty(description) && length(names) != length(description)) &&
		error("'description' and 'names' vectors must have the same length")
	(!isempty(description)) && return description
	desc = Vector{String}(undef, length(names))

	t = splitext(splitdir(names[1])[2])[1]
	if (startswith(t, "LC"))		# Have Landsat data
		for k = 1:length(names)
			t = splitext(splitdir(names[k])[2])[1]
			ind = findfirst("_B", t)
			bnd = parse(Int, t[ind[end]+1:end])
			desc[k] = Lsat8_desc[bnd]
		end
	elseif (sentinel2 == 10 || sentinel2 == 20 || sentinel2 == 60)
		for k = 1:length(names)
			t = splitdir(names[k])[2]
			ind = findfirst("_B", t)
			bnd = tryparse(Int, t[ind[end]+1:ind[end]+2])
			(bnd === nothing && t[ind[end]+1:ind[end]+2] == "8A") && (bnd = 8)	# ESA is crazzy. Fck character
			(bnd === nothing) && error("Could not find the band number in file name $t")
			desc[k] = (sentinel2 == 10) ? Sentinel2_10m_desc[bnd] : Sentinel2_20m_desc[bnd]	# 20m applyies also for 60m
		end
	else
		for k = 1:length(names)  desc[k] = splitext(splitdir(names[k])[2])[1]  end
	end
	return desc
end

# ----------------------------------------------------------------------------------------------------------
"""
read_mtl(band_name::String, mtl::String=""; get_full=false)

Use the `band_name` of a Landsat8 band to find the MTL file with the scene parameters at which that band
belongs and read the params needed to compute Brightness temperature, radiance at top of atmosphere, etc.
If the MTL file does not lieve next to the band file, send its name via the `mtl` argument.

The `get_full` option makes this function return a tring with contents of the MTL file or `nothing` if
the MTL file is not found.

### Returns a `MTL_short` with:

(band=band, rad_mul=rad_mul, rad_add=rad_add, rad_max=rad_max, reflect_mul=reflect_mul, reflect_add=reflect_add, reflect_max=reflect_max, sun_azim=sun_azim, sun_elev=sun_elev, sun_dis=sun_azim, K1=K1, K2=K2)

or a string vector with MTL contents (or nothing if MTL file is not found)
"""
function read_mtl(fname::String, mtl::String=""; get_full::Bool=false)
	get_full && return read_mtl_lines(fname, mtl)
	_fname = splitdir(fname)[2]
	((ind = findfirst("_B", _fname)) === nothing) &&
		error("This $(fname) is not a valid Landsat8 band file name or of a Landsat 8 cube file.")
	lines = read_mtl_lines(fname, mtl)
	(lines === nothing) && error("MTL file was not transmitted in input and I couldn't find it for $(fname).")
	band = parse(Int, splitext(_fname)[1][ind[1]+2:end])
	return parse_mtl(lines, band)
end

# The MTL file's lines, or `nothing` when `fname` is not a Landsat band or its MTL cannot be found.
function read_mtl_lines(fname::String, mtl::String="")::Union{Nothing, Vector{String}}
	pato, _fname = splitdir(fname)
	((ind = findfirst("_B", _fname)) === nothing) && return nothing
	if (mtl == "")
		mtl = joinpath(pato, _fname[1:ind[1]] * "MTL.txt")
		if (!isfile(mtl))
			lst = filter(x -> endswith(x, "MTL.txt"), readdir((pato == "") ? "." : pato))
			(length(lst) == 1 && length(_fname) >= 16 && startswith(lst[1], _fname[1:16])) && (mtl = joinpath(pato, lst[1]))
		end
	end
	!isfile(mtl) && return nothing			# Not fatal error (if we can live without a MTL).
	return readlines(mtl)
end

function parse_mtl(mtl::Vector{<:AbstractString}, band::Int)::MTL_short
	# Parse the 'mtl' string vector and extract the info relevant for 'band'
	# Make this a separate function so it can be called from read_mtl() or from the
	# MTL metadata stored in a cube file created with cutcube().
	function get_par(str::String)::Float64
		ind = findfirst(l -> occursin(str, l), mtl)
		(ind === nothing) && error("Parameter $str not found in the MTL data")
		parse(Float64, split(mtl[ind], "=")[2])
	end

	rad_mul = get_par("RADIANCE_MULT_BAND_$(band)")
	rad_add = get_par("RADIANCE_ADD_BAND_$(band)")
	rad_max = get_par("RADIANCE_MAXIMUM_BAND_$(band)")
	reflect_mul = (band < 10) ? get_par("REFLECTANCE_MULT_BAND_$(band)") : 1.0
	reflect_add = (band < 10) ? get_par("REFLECTANCE_ADD_BAND_$(band)") : 0.0
	reflect_max = (band < 10) ? get_par("REFLECTANCE_MAXIMUM_BAND_$(band)") : 0.0
	sun_azim = get_par("SUN_AZ")
	sun_elev = get_par("SUN_EL")
	sun_dist = get_par("EARTH_SUN")
	K1 = (band >= 10) ? get_par("K1_CONSTANT_BAND_$(band)") : 0.0
	K2 = (band >= 10) ? get_par("K2_CONSTANT_BAND_$(band)") : 0.0
	MTL_short(band, rad_mul, rad_add, rad_max, reflect_mul, reflect_add, reflect_max, sun_azim, sun_elev, sun_dist, K1, K2)
end

# ----------------------------------------------------------------------------------------------------------
function helper1_sats(fname::String, band_layer::Int)
	I = ((band_layer == 0) ? gmtread(fname) : gmtread(fname, layer=band_layer, layout="TRB"))::GMT.GMTimage{UInt16, 2}
	indNaN = isnodata(I)::AbstractMatrix{Bool}
	o = Matrix{Float32}(undef, size(I))
	return I, indNaN, o
end

function parse_lsat8_file(fname::String; band::Int=0, layer::Int=0, mtl::String="")::Tuple{Int, MTL_short, Int}
	# See if 'fname' is of a plain Landsat8 .tif file or of a cube created with cutcube().
	# Depending on the case find the MTL info from file or from the cube's Metadata. Former case
	# still accepts that the MTL file name be transmitted via the 'mtl' option.
	# If no errors so far, parse the MTL and extract the parameters concerning the wished 'band'.
	# This band number will be fetch from the band file name (full Landsat8 product name), or must be
	# transmitted via 'band' option when reading a cube file.
	# In alternative to 'band' we can use 'layer' and fetch the Band number that is stored in that layer.
	GMT.ressurectGDAL()				# Another black-hole plug attempt.
	ds = GMT.Gdal.unsafe_read(fname)
	nbands = GMT.Gdal.nraster(ds)
	meta = GMT.Gdal.GDALGetMetadata(ds.ptr, C_NULL)
	desc = (nbands > 1) ? String[GMT.Gdal.GDALGetDescription(GMT.Gdal.GDALGetRasterBand(ds.ptr, bd)) for bd = 1:nbands] : [""]
	GMT.Gdal.GDALClose(ds.ptr)

	nbands == 1 && return 0, read_mtl(fname, mtl), nbands	# No 'band' here: it's suposed to be findable in 'fname'

	bnd = band
	if (0 < layer < 12)			# Try to find which Band is at layer 'layer'
		((_band = tryparse(Int, split(desc[layer])[1][5:end])) === nothing) && @warn("Failed to find Band with description in layer $layer")
		(_band !== nothing) && (bnd = _band)
	end
	(bnd < 1) && error("The `band` option must contain the wished Landsat 8 band number. Use `band=N` to set it.")
	(bnd > 11) && throw(ArgumentError("Bad Landsat 8 band number $bnd"))
	((ind = findfirst(startswith.(meta, "MTL=GROUP ="))) === nothing) &&
		error("Data in a cube (> one layer) must contain the MTL info in Metadata and this one does not.")
	MTL::String = meta[ind][5:end]

	((band_layer = findfirst(startswith.(desc, "Band$bnd"))) === nothing) && error("Band $bnd not found in this cube")
	return band_layer, parse_mtl(split(MTL, "\n"), bnd), nbands
end

# ----------------------------------------------------------------------------------------------------------
function helper_dns(fname::String, band::Int, bandname::String, bandnames::String, mtl::String)::Tuple{Int, MTL_short}
	# Helper function for the dn2temperature, dn2radiance, dn2... functions
	(band == 0 && bandname == "" && bandnames == "") && error("No information provided on what Band to process.")
	bn = (bandname == "" && bandnames != "") ? bandnames : bandname		# Accept singular and plural name
	if (bn != "")
		layers, = find_layers(fname, bandnames=[bn])
		band_layer, pars, = parse_lsat8_file(fname, layer=layers[1], mtl=mtl)
		(band_layer != layers[1]) && error("Internal error. $band_layer and $(layers[1]) should be equal.")
	else
		band_layer, pars, = parse_lsat8_file(fname, band=band, mtl=mtl)
	end
	band_layer, pars
end

const dns_doc = "
- `fname`: The name of either a ``LANDSAT_PRODUCT_ID`` geotiff band, or the name of a cube file created with
  the `cutcube` function. In the first case, if the companion ``...MTL.txt`` file is not in the same directory
  as `fname` one can still pass it via the `mtl=path-to-MTL-file` option. In the second case it is mandatory
  to use one of the following two options.
- `band`: _cubes_ created with [`cutcube`](@ref) assign descriptions starting with \"Band 1 ...\" an so on
  the other bands. So when `band` is used we search for the band named \"Band N\", where N = `band`.
- `bandname`: When we know the common designation of a band, for example \"Green\", or any part of a band
  description, for example \"NIR\", we can use that info to create a `bandname` string that will be
  matched against the cube's bands descriptions. We can use the `reportbands` function to see the bands description.
- `save`:  The file name where to save the output. If not provided, a GMTgrid is returned.

Returns a Float32 GMTgrid
"

# ----------------------------------------------------------------------------------------------------------
"""
    R = dn2temperature(fname::String; band::Int=0, mtl::String="", save::String)

Computes the brigthness temperature of Landasat8 termal band (10 or 11)

$dns_doc

# Example:
Compute the brightness temperature of Band 10 stored in a `cube`

```
T = dn2temperature(cube, band=10)
```
"""
function dn2temperature(fname::String; band::Int=0, mtl::String="", save::String="")
	band_layer, pars, = parse_lsat8_file(fname, band=band, mtl=mtl)
	(pars.band < 10) && throw(ArgumentError("Brightness temperature is only for bands 10 or 11. Not this one: $(pars.band)"))
	I, indNaN, o = helper1_sats(fname, band_layer)
	_dn2temperature!(o, I.image, indNaN, pars)
	G = mat2grid(o, I)
	(save != "") && (gdaltranslate(G, dest=save); return nothing)
	return G
end

function _dn2temperature!(o::Matrix{Float32}, dn::AbstractMatrix{UInt16}, indNaN::AbstractMatrix{Bool}, pars::MTL_short)
	K1, K2, mul, add = pars.K1, pars.K2, pars.rad_mul, pars.rad_add
	@inbounds Threads.@threads for k = 1:length(o)
		o[k] = indNaN[k] ? NaN32 : K2 / (log(K1 / (dn[k] * mul + add) + 1.0)) - 273.15
	end
	return nothing
end

# ----------------------------------------------------------------------------------------------------------
function helper_dns_op(I::GMTimage, mul::Float64, add::Float64, indNaN::AbstractMatrix{Bool}, o::Matrix{Float32})
	# Helper function for the dn2radiance, dn2reflectance functions. Avoid IF branch if no NaNs
	_dns_op!(o, I.image, mul, add, indNaN)
	mat2grid(o, I)
end

function _dns_op!(o::Matrix{Float32}, dn::AbstractMatrix{UInt16}, mul::Float64, add::Float64, indNaN::AbstractMatrix{Bool})
	if any(indNaN)
		@inbounds Threads.@threads for k = 1:length(o)
			o[k] = indNaN[k] ? NaN32 : dn[k] * mul + add
		end
	else
		@inbounds Threads.@threads for k = 1:length(o)  o[k] = dn[k] * mul + add  end
	end
	return nothing
end

# ----------------------------------------------------------------------------------------------------------
"""
    R = dn2radiance(fname::String, [band::Int, bandname::String, mtl::String, save::String])

Computes the radiance at TopOfAtmosphere of a Landsat 8 file

$dns_doc

# Example:
Compute the radiance TOA of Band 2 file.
```
R = dn2radiance("LC08_L1TP_204033_20210525_20210529_02_T1_B2.TIF")
```
"""
function dn2radiance(fname::String; band::Int=0, bandname::String="", bandnames::String="", mtl::String="", save::String="")
	dn2aux(fname, false; band=band, bandname=bandname, bandnames=bandnames, mtl=mtl, save=save)
end

# ----------------------------------------------------------------------------------------------------------
"""
    R = dn2reflectance(fname::String, [band::Int, bandname::String, mtl::String, save::String])

Computes the TopOfAtmosphere planetary reflectance of a Landsat8 file

$dns_doc

# Example:
Compute the reflectance TOA of Red Band stored in a `cube`
```
R = dn2reflectance(cube, bandname="red")
```
"""
function dn2reflectance(fname::String; band::Int=0, bandname::String="", bandnames::String="", mtl::String="", save::String="")
	dn2aux(fname, true; band=band, bandname=bandname, bandnames=bandnames, mtl=mtl, save=save)
end

# One band of radiance (reflect=false) or reflectance (reflect=true).
function _dn2one(fname::String, reflect::Bool, band::Int, bandname::String, bandnames::String, mtl::String)
	band_layer, pars = helper_dns(fname, band, bandname, bandnames, mtl)
	(reflect && pars.band >= 10) && error("Computing Reflectance for Thermal bands is not defined.")
	I, indNaN, o = helper1_sats(fname, band_layer)
	reflect || return helper_dns_op(I, pars.rad_mul, pars.rad_add, indNaN, o)
	s_elev = sin(pars.sun_elev * pi/180)
	return helper_dns_op(I, pars.reflect_mul / s_elev, pars.reflect_add / s_elev, indNaN, o)
end

function dn2aux(fname::String, reflect::Bool; band::Int=0, bandname::String="", bandnames::String="", mtl::String="", save::String="")
	# Most of dn2reflectance & dn2radiance codes are similar, so gather it here under a common function.
	bnd = band
	if (bnd != 0)			# Else fish band from bandname
		_band_layer, _pars, n_bands = parse_lsat8_file(fname, band=bnd, mtl=mtl)
		(_band_layer == 0 && n_bands == 1) && (bnd = _pars.band)
	end

	if (bnd == 0 && bandname == "" && bandnames == "")						# Reading a cube
		bdnames::Vector{String} = reportbands(fname)
		if (reflect)
			((ind = findfirst(contains.(bdnames, "Band 10"))) !== nothing) && (deleteat!(bdnames, ind))
			((ind = findfirst(contains.(bdnames, "Band 11"))) !== nothing) && (deleteat!(bdnames, ind))
		end
		Gs = [_dn2one(fname, reflect, 0, bn, "", mtl) for bn in bdnames]
		G = mat2grid(cat([g.z for g in Gs]..., dims=3), Gs[1])
		G.names = bdnames
		G.v = collect(1:length(bdnames))
	else
		G = _dn2one(fname, reflect, bnd, bandname, bandnames, mtl)
	end
	(save != "") && (gdaltranslate(G, dest=save); return nothing)
	return G
end

# ----------------------------------------------------------------------------------------------------------
"""
    R = reflectance_surf(fname::String, [band::Int, bandname::String, mtl::String, save::String])

Computes the radiance-at-surface of Landsat8 band using the COST model.

$dns_doc
"""
function reflectance_surf(fname::String; band::Int=0, mtl::String="", save::String="")
	band_layer, pars, = parse_lsat8_file(fname, band=band, mtl=mtl)
	(pars.band >= 10) && error("Computing Surface Reflectance for Thermal bands is not defined.")
	I, indNaN, o = helper1_sats(fname, band_layer)

	s_elev = sin(pars.sun_elev * pi/180)
	Esun = (pi * pars.sun_dist ^2) * pars.rad_max / pars.reflect_max
	TAUv = 1.0;		TAUz = s_elev;		Esky = 0.0;		sun_prct = 1.0
	(pars.band == 6 || pars.band == 7 || pars.band == 9) && (TAUz = 1.0)
	Sun_Radiance = TAUv * (Esun * s_elev * TAUz + Esky) / (pi * pars.sun_dist ^2)

	tmp = filter(!=(0), I.image)		# This is 3 times faster then: tmp = I.image[I.image .> 0]
	darkDN = quantile!(tmp, 0.01)
	radiance_dark = pars.rad_mul * darkDN + pars.rad_add	# 0.01%
	LHaze = radiance_dark - sun_prct * Sun_Radiance / 100
	_reflectance_surf!(o, I.image, indNaN, pars.rad_mul, pars.rad_add, LHaze, Sun_Radiance)
	G = mat2grid(o, I)
	(save != "") && (gdaltranslate(G, dest=save); return nothing)
	return G
end

function _reflectance_surf!(o::Matrix{Float32}, dn::AbstractMatrix{UInt16}, indNaN::AbstractMatrix{Bool},
                            mul::Float64, add::Float64, LHaze::Float64, Sun_Radiance::Float64)
	@inbounds Threads.@threads for k = 1:length(o)
		rad = Float32(dn[k] * mul + add)			# the radiance is held as Float32, as it always was
		v = Float32((rad - LHaze) / Sun_Radiance)
		o[k] = indNaN[k] ? NaN32 : clamp(v, 0.0f0, 1.0f0)
	end
	return nothing
end

# ----------------------------------------------------------------------------------------------------------
"""
    I = classify(cube::GItype, train::Union{Vector{<:GMTdataset}, String}) -> GMTimage

- `cube`: The cube wtih band data to classify.
- `train`: A vector of GMTdatasets or a file name of one containing the polygons used to train the model.
   NOTE: The individual datasets MUST have associated an attribute called "class" containing the class name as a string.
   This can be achieved for text data in the form of a GMT multi-segment file (one where segments are separated by the '>'
   symbol) if the multi-segment separator line contains the text ``Attrib(class=name)``

Returns an image with the classification results where each class name was assigned a different integer number.
That colorized image can plotted with ``viz(I, colorbar=true)``.
"""
function classify(cube::GMT.GItype, train::Union{Vector{<:GMTdataset}, String})
	model, classes = train_raster(cube, train)
	I = classify(cube, model)
	cpt = makecpt(cmap=:categorical, range=classes);
	image_cpt!(I, cpt)
	return I
end

# The 3-D array of a cube, whichever GMT type holds it.
_cube_array(c::GMTimage) = c.image
_cube_array(c::GMTgrid)  = c.z

# ----------------------------------------------------------------------------------------------------------
"""
    I = classify(cube::GItype, model; class_names::Union{String, Vector{String}}="") -> GMTimage

- `cube`: The cube wtih band data to classify.
- `model`: The trained model obtained from the `train_raster` function.
- `class_names`: A vector of strings with the class names to be used in the categorical colorbar or a
   comma separated single with those class names. The number of class names must match the number used
   when training the model with `train_raster`.
"""
function classify(cube::GMT.GItype, model::DecisionTreeModel{UInt8}; class_names::Union{String, Vector{String}}="")
	mat = _classify_pixels(_cube_array(cube), model)
	I = mat2img(mat, cube)
	(class_names == "") && return I			# No class names, no CPT
	classes = isa(class_names, Vector) ? join(class_names, ",") : class_names
	cpt = makecpt(cmap=:categorical, range=classes)
	image_cpt!(I, cpt)
	return I
end

# Each pixel's band vector straight out of the cube, one column per thread; no transposed copy.
function _classify_pixels(A::AbstractArray{<:Real,3}, model::DecisionTreeModel{UInt8})::Matrix{UInt8}
	nr, nc, nb = size(A)
	(nb != model.n_features) && error("The model was trained with $(model.n_features) bands but the cube has $nb")
	mat = Matrix{UInt8}(undef, nr, nc)
	Threads.@threads for j = 1:nc
		@inbounds for i = 1:nr
			mat[i,j] = model.classes[model.label[_leaf(model, b -> Float64(A[i,j,b]))]]
		end
	end
	return mat
end

# ----------------------------------------------------------------------------------------------------------
"""
    I = classification_proba(cube::GItype, model; class_number=1) -> GMTimage

Returns an image with the assigned probabilities when classifying the class number `class_number`

- `cube`: The cube wtih band data to classify
- `model`: is the model obtained from the `train_raster` function
- `class_number`: is the class number to be classified
"""
function classification_proba(cube::GMT.GItype, model::DecisionTreeModel; class_number::Int=1)
	(1 <= class_number <= length(model.classes)) || error("class_number must be in 1:$(length(model.classes))")
	mat = _proba_pixels(_cube_array(cube), model, class_number)
	mat2img(mat, cube)
end

function _proba_pixels(A::AbstractArray{<:Real,3}, model::DecisionTreeModel, c::Int)::Matrix{UInt8}
	nr, nc, nb = size(A)
	(nb != model.n_features) && error("The model was trained with $(model.n_features) bands but the cube has $nb")
	nclass = length(model.classes)
	mat = Matrix{UInt8}(undef, nr, nc)
	Threads.@threads for j = 1:nc
		@inbounds for i = 1:nr
			id = _leaf(model, b -> Float64(A[i,j,b]))
			tot = 0
			for q = 1:nclass  tot += model.counts[q, id]  end
			mat[i,j] = round(UInt8, model.counts[c, id] / tot * 255)
		end
	end
	return mat
end

# ----------------------------------------------------------------------------------------------------------
"""
    model, classes = train_raster(cube::GItype, train::Union{Vector{<:GMTdataset}, String}; np::Int=0, density=0.1, max_depth=3)

- `cube`: The cube wtih band data to classify.
- `train`: A vector of GMTdatasets or a file name of one containing the polygons used to train the model.
   NOTE: The individual datasets MUST have associated an attribute called "class" containing the class name as a string.
   This can be achieved for text data in the form of a GMT multi-segment file (one where segments are separated by the '>'
   symbol) if the multi-segment separator line contains the text ``Attrib(class=name)``
- `np`: Number of points per polygon to be determined by ``randinpolygon``
- `density`: Alternative to `np`. See also the help of the ``randinpolygon`` function.
- `max_depth`: Maximum depth of the decision tree.

Returns the trained model (a `DecisionTreeModel`) and the class names.
"""
function train_raster(cube::GMT.GItype, train::Union{Vector{<:GMTdataset}, String}; np::Int=0, density=0.1, max_depth::Int=3)
	samples = (isa(train, String) ? gmtread(train) : train)::Vector{<:GMTdataset}
	get(samples[1].attrib, "class", "") == "" && error("The datasets used for training MUST have an attribute called 'class'.")
	(get(samples[1].attrib, "id", "") == "") && add_class_id!(samples)	# If no 'id' attribute, create one from 'class'

	pts = randinpolygon(samples, np=np, density=density)
	# GMT.jl's grdinterpolate cannot sample a UInt16 image cube nor a Float32 grid cube with several
	# point sets (docs/GMTJL_GRID_ISSUES.md, issues 4a and 5), and those are exactly what a Landsat /
	# Sentinel cube reads as. Sample a Float64 grid copy of the cube instead: same nodes, same values.
	# A ONE-element vector of point sets fails there too (issue 4b): hand that one over as a dataset.
	(pts isa Vector && length(pts) == 1) && (pts = pts[1])
	LCsamp = grdinterpolate(_as_f64_grid(cube), S=pts, nocoords=true)
	LCsamp isa GMTdataset && (LCsamp = [LCsamp])			# one polygon -> one dataset, not a vector
	features = Matrix{Float64}(GMT.ds2ds(LCsamp).data)
	labels = UInt8[]
	for D in LCsamp
		id = D.attrib["id"]
		append!(labels, fill(parse(UInt8, isa(id, Vector) ? first(id) : id), size(D, 1)))
	end

	model = fit_tree(features, labels; max_depth=max_depth)
	classes = join(unique(GMT.make_attrtbl(samples, false)[1][:,1]), ",")
	return model, classes
end

# A cube as a Float64 GMTgrid on the same nodes, band names kept (the one form grdinterpolate samples).
_as_f64_grid(c::GMTgrid{Float64,3})::GMTgrid{Float64,3} = c
function _as_f64_grid(c::GMT.GItype)::GMTgrid{Float64,3}
	G = mat2grid(Float64.(_cube_array(c)), c)
	G.names = c.names
	return G
end

function add_class_id!(D::Vector{<:GMTdataset})
	seen = Dict{String,Int}()
	n = 0
	for d in D
		v = d.attrib["class"]
		cls = v isa Vector ? first(v) : v
		haskey(seen, cls) || (n += 1; seen[cls] = n)
	end
	for d in D
		v = d.attrib["class"]
		cls = v isa Vector ? first(v) : v
		d.attrib["id"] = string(seen[cls])
	end
	return nothing
end
