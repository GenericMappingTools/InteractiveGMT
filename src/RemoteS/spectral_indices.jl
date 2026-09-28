const generic_docs = "
- The first form accepts inputs as matrices, or file names of the data bands.
- The last form is more versatile but also more complex to describe.
  - `cube`: Is the file name of a 'cube', a multi-layered file normally created with the [`cutcube`](@ref) function.
     If this file was created with band descriptions one can use the `bands` or the `bandnames` options.
  - `bands`: _cubes_ created with [`cutcube`](@ref) assign descriptions starting with \"Band1 ...\" an so on
    the other bands. So when `bands` is used we search for bands named \"Band'band[k]'\", where band[k] loops
    over all elements of the `bands` vector. WARNING: the elements order in the vector must be sorted in increasing
    wavelength numbers, _i.e._ like the example for the first form.
  - `layers`: Use this option when you are certain of the bands order in the cube or the it doesn't have a bands
    description. The selection will be made with cube[:,:,layer[1]], etc... WARNING: same warn as above.
  - `bandnames`: When we know the common designation of a band, for example \"Green\", or any part of a band
    description, for example \"NIR\", we can use that info to create a `bandnames` string vector that will be
    matched against the cube's bands descriptions.

### Kwargs
  - `threshold`: When a threshold is provided we return a GMTgrid where `vals[ij] < threshold = NaN`
  - `classes`: is a vector with up to 3 elements (class separators, increasing) and we return a UInt8 GMTimage with the
    indices categorized into vals[ij] >= classes[1] = 1; vals[ij] >= classes[2] = 2; vals[ij] >= classes[3] = 3 and 0 otherwise.
  - `mask`: Used together with `threshold` outputs a UInt8 GMTimage mask with `vals[ij] >= threshold = 255` and 0 otherwise
     If `mask=-1` (or any other negative number) we compute instead a mask where `vals[ij] < threshold = 255` and 0 otherwise
  - `save`: Use `save=\"file_name.ext\"` to save the result in a disk file. File format is picked from file extension.
  - `order` | `bands_order` | `rgb`: For the ``GLI``, ``TGI`` and ``VARI`` (RGB) indices, we allow to reorder the bands
    and change the expected RGB order. Pass in a string, or symbol, with the color order. For example, `order=:rbg`
	will swap the green and blue components making the result index identify the _reds_ instead of the _greens_.
	Not good for vegetation indices, but potentially useful for other purposes.

If none of `bands`, `layers` or `bandnames` is provided, we use the default band names shown in the first form.

See also https://www.indexdatabase.de/ for a list of indices and the appropriate band names per sensor.

Returns either a Float32 GMTgrid or a UInt8 GMTimage if the `mask` or `classes` options are used.
"

const _Cube3 = Union{GMT.GMTimage{UInt16, 3}, AbstractArray{<:AbstractFloat, 3}}

# ----------------------------------------------------------------------------------------------------------
# Helper function to compute Spectral Indices from a 'cube' file and somehow band selection.
function helper_si_method(cube::String, index::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[],
	                      bandnames::Vector{String}=String[], defbandnames::Vector{String}=String[], kw...)
	if (index == "GLI" || index == "TGI" || index == "VARI") && ((ext = lowercase(splitext(cube)[2])) == ".png" || ext == ".jpg")
		return sp_indices(gdalread(cube); index=index, kw...)	# Try to read it as an image
	end
	(isempty(bands) && isempty(bandnames) && isempty(layers)) && (bandnames = defbandnames)
	sc = subcube(cube, bands=bands, layers=layers, bandnames=bandnames)
	sp_indices(sc, collect(1:size(sc,3)); index=index, kw...)	# Here we know that the layers are all of those in cube
end

# Method for in memory cubes
function helper_si_method(cube::_Cube3, index::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[],
                          bandnames::Vector{String}=String[], defbandnames::Vector{String}=String[], kw...)
	(isempty(bands) && isempty(bandnames) && isempty(layers)) && (bandnames = defbandnames)
	lay = !isempty(layers) ? layers : find_layers(cube, bandnames, bands)
	sp_indices(cube, lay; index=index, kw...)
end

# ----------------------------------------------------------------------------------------------------------
"""
    CLG = clg(green, redEdge3; kw...)
or

    CLG = clg(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Green cholorphyl index. Wu et al 2012.

CLG = (redEdge3)/(green)-1
"""
clg(green, redEdge3; kw...) = sp_indices(green, redEdge3; index="CLG", kw...)
clg(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "CLG"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "redEdge3"], kw...)
clg(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "CLG"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "redEdge3"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    CLRE = clre(redEdge1, redEdge3; kw...)
or

    CLRE = clre(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

RedEdge cholorphyl index. Clevers and Gitelson 2013.

CLRE = (redEdge3)/(redEdge1)-1
"""
clre(redEdge1, redEdge3; kw...) = sp_indices(redEdge1, redEdge3; index="CLRE", kw...)
clre(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "CLRE"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["redEdge1", "redEdge3"], kw...)
clre(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "CLRE"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["redEdge1", "redEdge3"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    EVI = evi(blue, red, nir; kw...)
or

    EVI = evi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Enhanced vegetation index. Huete et al 1990

EVI = G * ((nir - red) / (nir + C1 * red - C2 * blue + Levi));
C1, C2, G, Levi = 6.0, 7.5, 2.5, 1.

$(generic_docs)

"""
evi(blue, red, nir; kw...) = sp_indices(blue, red, nir; index="EVI", kw...)
evi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "EVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["blue", "red", "nir"], kw...)
evi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "EVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["blue", "red", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    EVI2 = evi2(red, nir; kw...)
or

    EVI2 = evi2(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Two-band Enhanced vegetation index. Jiang et al 2008

EVI2 = G * (nir - red) / (nir + 2.4 * red + 1),  G = 2.5

$(generic_docs)
"""
evi2(red, nir; kw...) = sp_indices(red, nir; index="EVI2", kw...)
evi2(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "EVI2"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)
evi2(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "EVI2"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    GNDVI = gndvi(green, nir; kw...)
or

    GNDVI = gndvi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

green Normalized diff vegetation index: more sensitive to cholorphyll than ndvi. Gitelson, A., and M. Merzlyak

GNDVI = (nir - green) / (nir + green)

$(generic_docs)
"""
gndvi(green, nir; kw...) = sp_indices(green, nir; index="GNDVI", kw...)
gndvi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "GNDVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "nir"], kw...)
gndvi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "GNDVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    GLI = gli(red, green, blue; kw...)
or (here fname is a .png or .jpg file name)

    GLI = gli(fname::String; kw...)
or

    GLI = gli(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Green Leaf Index. Louhaichi, M., Borman, M.M., Johnson, D.E., 2001.

GLI = (2green - red - blue) / (2green + red + blue)

$(generic_docs)
"""
gli(rgb::GMTimage{UInt8, 3}; kw...) = sp_indices(rgb; index="GLI", kw...)
gli(red, green, blue; kw...) = sp_indices(red, green, blue; index="GLI", kw...)
gli(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "GLI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "green", "blue"], kw...)
gli(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "GLI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "green", "blue"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    MNDWI = mndwi(green, swir2; kw...)
or

    MNDWI = mndwi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Modified Normalised Difference Water Index. Xu2006

MNDWI = (green-swir2) / (green+swir2)

$(generic_docs)
"""
mndwi(green, swir2; kw...) = sp_indices(green, swir2; index="MNDWI", kw...)
mndwi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MNDWI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "swir2"], kw...)
mndwi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MNDWI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "swir2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    MTCI = mtci(red, redEdge1, redEdge2; kw...)

Meris Terrestrial Chlorophyll Index. Clevers and Gitelson 2013, Dash and Curran 2004

MTCI = (redEdge2-redEdge1) / (redEdge1-red)
"""
mtci(red, redEdge1, redEdge2; kw...) = sp_indices(red, redEdge1, redEdge2; index="MTCI", kw...)
mtci(cube::GMT.GMTimage{UInt16, 3}, bnds::Vector{Int}; kw...) = sp_indices(cube, find_layers(cube, bnds, 3); index="MTCI", kw...)
mtci(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MTCI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "redEdge1", "redEdge2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    MCARI = mcari(green, red, redEdge1; kw...)
or

	MCARI = mcari(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Modified Chlorophyll Absorption ratio index. Daughtery et al. 2000

MCARI = (redEdge1 - red - 0.2 * (redEdge1 - green)) * (redEdge1 / red)

(Sentinel-2 Band 5 (VNIR), Band 4 (Red) and Band 3 (Green)).
"""
mcari(green, red, redEdge1; kw...) = sp_indices(green, red, redEdge1; index="MCARI", kw...)
mcari(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MCARI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "red", "redEdge1"], kw...)
mcari(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MCARI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "red", "redEdge1"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    MSAVI = msavi(red, nir; kw...)
or

    MSAVI = msavi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Modified soil adjusted vegetation index. Qi 1994

MSAVI = (2 * nir + 1 - sqrt((2 * nir + 1)^2 - 8 * (nir - red))) / 2

$(generic_docs)
"""
msavi(red, nir; kw...) = sp_indices(red, nir; index="MSAVI", kw...)
msavi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MSAVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)
msavi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "MSAVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    NBRI = nbri(nir, swir2; kw...)
or

	NBRI = nbri(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Normalised Burn Ratio Index. Garcia 1991

NBRI = (nir - swir2) / (nir + swir2)
"""
nbri(nir, swir2; kw...) = sp_indices(nir, swir2; index="NBRI", kw...)
nbri(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NBRI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["nir", "swir2"], kw...)
nbri(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NBRI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["nir", "swir2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    NDVI = ndvi(red, nir; kw...)
or

    NDVI = ndvi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Compute the NDVI vegetation index. Input can be either the bands file names, or GMTimage objects
with the band's data.

NDVI = (nir - red) / (nir + red)

$(generic_docs)
"""
ndvi(red, nir; kw...) = sp_indices(red, nir; index="NDVI", kw...)
ndvi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)
ndvi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    NDWI = ndwi(green, nir; kw...)
or

    NDWI = ndwi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Normalized difference water index. McFeeters 1996. NDWI => (green - nir)/(green + nir)

NDWI = (green - nir)/(green + nir)

$(generic_docs)
"""
ndwi(green, nir; kw...) = sp_indices(green, nir; index="NDWI", kw...)
ndwi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDWI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "nir"], kw...)
ndwi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDWI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["green", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    NDWI2 = ndwi2(nir, swir2; kw...)
or

    NDWI2 = ndwi2(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Normalized difference water index. Gao 1996, Chen 2005 (also known as Normalized Difference Moisture Index
NDBI and LSWI)

NDWI2 = (nir - swir2)/(nir + swir2)

$(generic_docs)
"""
ndwi2(nir, swir2; kw...) = sp_indices(nir, swir2; index="NDWI2", kw...)
ndwi2(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDWI2"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["nir", "swir2"], kw...)
ndwi2(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDWI2"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["nir", "swir2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    NDREI1 = ndrei1(redEdge1, redEdge2; kw...)

Normalized difference red edge index. Gitelson and Merzlyak 1994

NDREI1 = (redEdge2 - redEdge1) / (redEdge2 + redEdge1)
"""
ndrei1(redEdge1, redEdge2; kw...) = sp_indices(redEdge1, redEdge2; index="NDREI1", kw...)
ndrei1(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDREI1"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["redEdge1", "redEdge2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    NDREI2 = ndrei2(redEdge1, redEdge3; kw...)
or

	NDREI2 = ndrei2(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Normalized difference red edge index 2. Barnes et al 2000

NDREI2 = (redEdge3 - redEdge1) / (redEdge3 + redEdge1)
"""
ndrei2(redEdge1, redEdge3; kw...) = sp_indices(redEdge1, redEdge3; index="NDREI2", kw...)
ndrei2(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDREI2"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["redEdge1", "redEdge3"], kw...)
ndrei2(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "NDREI2"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["redEdge1", "redEdge3"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    SATVI = satvi(red, swir1, swir2; kw...)
or

	SATVI = satvi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Soil adjusted total vegetation index. Marsett 2006

SATVI = ((swir1 - red) / (swir1 + red + L)) * (1.0 + L) - (swir2 / 2.0)
"""
satvi(red, swir1, swir2; kw...) = sp_indices(red, swir1, swir2; index="SATVI", kw...)
satvi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "SATVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "swir1", "swir2"], kw...)
satvi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "SATVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "swir1", "swir2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    SAVI = savi(red, nir; kw...)
or

    SAVI = savi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Soil adjusted vegetation index. Huete 1988

SAVI = (nir - red) * (1.0 + L) / (nir + red + L),  L = 0.5

$(generic_docs)
"""
savi(red, nir; kw...) = sp_indices(red, nir; index="SAVI", kw...)
savi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "SAVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)
savi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "SAVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    SLAVI = slavi(red, nir, swir2; kw...)
or

    SLAVI = slavi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Specific Leaf Area Vegetation Index. Lymburger 2000

SLAVI = nir / (red + swir2)

$(generic_docs)
"""
slavi(red, nir, swir2; kw...) = sp_indices(red, nir, swir2; index="SLAVI", kw...)
slavi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "SLAVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir", "swir2"], kw...)
slavi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "SLAVI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "nir", "swir2"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    TGI = tgi(red, green, blue; kw...)
or (here fname is a .png or .jpg file name)

    TGI = tgi(fname::String; kw...)
or

    TGI = tgi(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Triangular Greenness Index. Hunt et al. 2013

TGI = green - 0.39 * red - 0.61 * blue

$(generic_docs)
"""
tgi(rgb::GMTimage{UInt8, 3}; kw...) = sp_indices(rgb; index="TGI", kw...)
tgi(red, green, blue; kw...) = sp_indices(red, green, blue; index="TGI", kw...)
tgi(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "TGI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "green", "blue"], kw...)
tgi(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "TGI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "green", "blue"], kw...)

# ----------------------------------------------------------------------------------------------------------
"""
    VARI = vari(red, green, blue; kw...)
or (here fname is a .png or .jpg file name)

    VARI = vari(fname::String; kw...)
or

    VARI = vari(cube::Union{String, GMTgrid}; [bands=Int[], bandnames=String[], layers=Int[]], kwargs...)

Visible Atmospherically Resistant Index. Gitelson, A.A., Kaufman, Y.J., Stark, R., Rundquist, D., 2002

VARI = (green - red) / (green + red - blue)

$(generic_docs)
"""
vari(rgb::GMTimage{UInt8, 3}; kw...) = sp_indices(rgb; index="VARI", kw...)
vari(red, green, blue; kw...) = sp_indices(red, green, blue; index="VARI", kw...)
vari(cube::String; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "VARI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "green", "blue"], kw...)
vari(cube::_Cube3; bands::Vector{Int}=Int[], layers::Vector{Int}=Int[], bandnames::Vector{String}=String[], kw...) =
	helper_si_method(cube, "VARI"; bands=bands, layers=layers, bandnames=bandnames, defbandnames=["red", "green", "blue"], kw...)

# ==========================================================================================================
# The engine. Every index above lands in `_sp_compute`, which does three things:
#   1. parses the kwargs ONCE into a concrete `SIOpts` (the original read them from a Dict{Symbol,Any}
#      inside the hot path and carried `Any`-typed thresholds into the threaded loops);
#   2. reduces each band to the plain array under it (`_band_array`), so the loops index a
#      Matrix/SubArray and never a GMTimage/GMTgrid wrapper;
#   3. looks up the index's scalar formula and hands it to `_si_values`/`_si_mask`/`_si_classes`,
#      function barriers that specialise on the formula AND the array types. The original had one
#      `if index == ...` chain whose branches re-assigned the band variables captured by every
#      `Threads.@threads` closure, so they were all boxed (Core.Box) and every pixel went through
#      dynamic dispatch.

struct SIOpts
	mask::Bool
	rev_mask::Bool
	threshold::Float64            # NaN = none
	classes::Vector{Float64}      # empty = none
	save::String
	dbg::Bool
end

function _si_opts(kw)::SIOpts
	d = KW(kw)
	mval = find_in_dict(d, [:mask])[1]
	mask = mval !== nothing
	rev_mask = mask && isa(mval, Real) && mval < 0			# See if we want to reverse the mask
	tval = find_in_dict(d, [:threshold])[1]
	threshold = (tval === nothing) ? NaN : Float64(tval)
	cval = (tval === nothing) ? find_in_dict(d, [:classes])[1] : nothing
	classes = (cval === nothing) ? Float64[] : Float64[Float64(x) for x in cval]
	(length(classes) > 3) && (classes = classes[1:3]; @warn("`classes` maximum elements is 3. Clipping the others"))
	(mask && isnan(threshold)) && error("The `mask` option requires the `threshold=x` option")
	save_name::String = ((val = find_in_dict(d, [:save])[1]) !== nothing) ? string(val) : ""
	dbg = (find_in_dict(d, [:Vd :dbg])[1] !== nothing)
	for k in (:rgb, :order, :bands_order, :dn2radiance, :dn2reflectance, :reflectance_surf)  delete!(d, k)  end
	(length(d) > 0) && @warn("sp_indices: the following options were not consumed => $(collect(keys(d)))")
	return SIOpts(mask, rev_mask, threshold, classes, save_name, dbg)
end

# The scalar formulas, on bands already scaled to [0,1] for integer data. `a, b, c` are the bands
# in the order the index's own method receives them.
const _C1, _C2, _G, _L, _Levi = 6.0, 7.5, 2.5, 0.5, 1.0
_f_cl(a, b, c)    = b / a - 1                                                  # CLG, CLRE
_f_evi(a, b, c)   = _G * ((c - b) / (c + _C1 * b - _C2 * a + _Levi))            # a=blue b=red c=nir
_f_evi2(a, b, c)  = _G * (b - a) / (b + 2.4 * a + 1.0)                         # a=red b=nir
_f_gli(a, b, c)   = (2b - (a + c)) / (2b + (a + c))                            # a=red b=green c=blue
_f_mtci(a, b, c)  = (c - b) / (b - a)                                          # a=red b=redEdge1 c=redEdge2
_f_mcari(a, b, c) = (c - b - 0.2 * (c - a)) * (c / b)                          # a=green b=red c=redEdge1
_f_msavi(a, b, c) = (2b + 1 - sqrt((2b + 1)^2 - 8 * (b - a))) / 2             # a=red b=nir
_f_nd(a, b, c)    = (b - a) / (a + b)                                          # (2nd - 1st) / sum
_f_ndr(a, b, c)   = (a - b) / (a + b)                                          # (1st - 2nd) / sum
_f_satvi(a, b, c) = ((b - a) / (b + a + _L)) * (1.0 + _L) - (c / 2.0)          # a=red b=swir1 c=swir2
_f_savi(a, b, c)  = (b - a) * (1.0 + _L) / (b + a + _L)                        # a=red b=nir
_f_slavi(a, b, c) = b / (a + c)                                                # a=red b=nir c=swir2
_f_tgi(a, b, c)   = b - 0.39 * a - 0.61 * c                                    # a=red b=green c=blue
_f_vari(a, b, c)  = (b - a) / (b + a - c)                                      # a=red b=green c=blue

# index -> (formula, n bands, keep only [-1,1] values, mask only <= 1)
function _si_formula(index::String)
	(index == "CLG" || index == "CLRE") && return (_f_cl, 2, false, false)
	(index == "EVI")   && return (_f_evi,   3, false, false)
	(index == "EVI2")  && return (_f_evi2,  2, true,  false)
	(index == "GLI")   && return (_f_gli,   3, false, false)
	(index == "MTCI")  && return (_f_mtci,  3, false, false)
	(index == "MCARI") && return (_f_mcari, 3, false, false)
	(index == "MSAVI") && return (_f_msavi, 2, false, false)
	# Normalized differences. MNDWI (green,swir2), NBRI (nir,swir2), NDWI (green,nir) and NDWI2
	# (nir,swir2) are (1st - 2nd); GNDVI, NDVI, NDREI1, NDREI2 are (2nd - 1st).
	(index == "MNDWI" || index == "NBRI" || index == "NDWI" || index == "NDWI2") && return (_f_ndr, 2, true, true)
	(index == "GNDVI" || index == "NDVI" || index == "NDREI1" || index == "NDREI2") && return (_f_nd, 2, true, true)
	(index == "SATVI") && return (_f_satvi, 3, false, false)
	(index == "SAVI")  && return (_f_savi,  2, false, false)
	(index == "SLAVI") && return (_f_slavi, 3, false, false)
	(index == "TGI")   && return (_f_tgi,   3, false, false)
	(index == "VARI")  && return (_f_vari,  3, true,  true)
	error("Unknown spectral index \"$index\"")
end

# The plain array under a band, and the object that georeferences it (or nothing).
_band_array(x::GMTimage) = x.image
_band_array(x::GMTgrid)  = x.z
_band_array(x::SubArray) = (p = parent(x); isa(p, GMT.GItype) ? view(_band_array(p), parentindices(x)...) : x)
_band_array(x::AbstractArray) = x
_georef(x::GMT.GItype) = x
_georef(x::SubArray) = (p = parent(x); isa(p, GMT.GItype) ? p : nothing)
_georef(x) = nothing

@inline _bv(b::AbstractArray, k::Int, s::Float64)::Float64 = @inbounds Float64(b[k]) * s
@inline _bv(::Nothing, ::Int, ::Float64)::Float64 = 0.0

function _si_values(f::F, clip::Bool, b1::A, b2::B, b3::C, s::Float64, o::SIOpts) where {F,A,B,C}
	img = zeros(Float32, size(b1))		# 0.0f0 is the neutral instead of NaN. Classification algos don't work with NaN
	@inbounds Threads.@threads for k = 1:length(img)
		t = f(_bv(b1,k,s), _bv(b2,k,s), _bv(b3,k,s))
		(!clip || (t >= -1 && t <= 1)) && (img[k] = t)
	end
	if !isnan(o.threshold)				# A GMTgrid where < threshold = NaN
		thr = o.threshold
		@inbounds Threads.@threads for k = 1:length(img)
			(img[k] < thr) && (img[k] = NaN32)
		end
	end
	return img
end

function _si_mask(f::F, clip::Bool, b1::A, b2::B, b3::C, s::Float64, o::SIOpts) where {F,A,B,C}
	mask = zeros(UInt8, size(b1))
	thr, rev = o.threshold, o.rev_mask
	@inbounds Threads.@threads for k = 1:length(mask)
		t = f(_bv(b1,k,s), _bv(b2,k,s), _bv(b3,k,s))
		((rev ? t < thr : t > thr) && (!clip || t <= 1)) && (mask[k] = 0xff)
	end
	return mask
end

function _si_classes(f::F, clip::Bool, b1::A, b2::B, b3::C, s::Float64, o::SIOpts) where {F,A,B,C}
	# Up to 3 separators -> classes 0..3: the number of separators the value reaches.
	img = _si_values(f, clip, b1, b2, b3, s, o)
	cls = zeros(UInt8, size(img))
	c = o.classes
	@inbounds Threads.@threads for k = 1:length(img)
		q = 0x00
		for i in eachindex(c)
			(img[k] >= c[i]) && (q = UInt8(i))
		end
		cls[k] = q
	end
	return cls
end

function _sp_compute(index::String, bnd1, bnd2, bnd3, o::SIOpts)
	(index == "") && error("Must select which index to compute")
	@assert size(bnd1) == size(bnd2)
	(bnd3 !== nothing) && @assert size(bnd3) == size(bnd1)
	f, nb, clipimg, clipmask = _si_formula(index)
	(nb == 3 && bnd3 === nothing) && error("The $index index needs three bands")
	a1, a2 = _band_array(bnd1), _band_array(bnd2)
	a3 = (nb == 3) ? _band_array(bnd3) : nothing
	s = (eltype(a1) <: Integer) ? 1.0 / Float64(typemax(eltype(a1))) : 1.0	# Floats go unchanged
	ref = _georef(bnd1)
	if (o.mask || !isempty(o.classes))
		m = o.mask ? _si_mask(f, clipmask, a1, a2, a3, s, o) : _si_classes(f, clipimg, a1, a2, a3, s, o)
		(ref === nothing) && return m
		I = mat2img(m, ref)
		I.layout = "BRPa"
		I.range[5], I.range[6] = 0, (o.mask) ? 255 : length(o.classes)
		return I
	end
	img = _si_values(f, clipimg, a1, a2, a3, s, o)
	return (ref === nothing) ? img : mat2grid(img, ref)
end

# Name the result and save it if asked.
function _si_finish(O, index::String, o::SIOpts)
	isa(O, GMT.GItype) && (O.names = [index * " index"])
	(o.save != "") && (gmtwrite(o.save, O); return nothing)
	return O
end

# ----------------------------------------------------------------------------------------------------------
function sp_indices(bnd1::String, bnd2::String, bnd3::String=""; index::String="", kwargs...)
	# Compute spectral indices from band files, optionally converting the DNs first
	do_radTOA = haskey(kwargs, :dn2radiance)
	do_refTOA = haskey(kwargs, :dn2reflectance)
	do_refSrf = haskey(kwargs, :reflectance_surf)
	rd(b) = (do_radTOA) ? dn2radiance(b) : (do_refTOA) ? dn2reflectance(b) : (do_refSrf) ? reflectance_surf(b) : gmtread(b)
	sp_indices(rd(bnd1), rd(bnd2), (bnd3 != "") ? rd(bnd3) : nothing; index=index, kwargs...)
end

# ----------------------------------------------------------------------------------------------------------
function sp_indices(rgb::GMT.GMTimage{UInt8, 3}; index::String="", kw...)
	# This method applyies only in the case of the RGB vegetation indices (GLI, TGI, VARI)
	(index != "GLI" && index != "TGI" && index != "VARI") && error("With RGB images input, only `GLI`, `TGI` and `VARI` indices are supported, not $index")
	(rgb.layout[3] != 'B') && error("For now, only band interleavedRGB composition is supported and not $(rgb.layout)")
	# Here we are allowing cheating the indices by altering the bands order. These indices expect (were deffined)
	# the bands in RGB order but nothing stops us to to change that and convert a green index into a blue index.
	# For that pass string with the R,G,B in the wished order to the 'order' option. E.g. 'order="rbg"'
	bds = [1,2,3]			# The default RGB order
	if ((val = find_in_kwargs(kw, [:order :bands_order :rgb])[1]) !== nothing)
		o = lowercase(string(val))
		for k = 1:3
			bds[k] = (o[k] == 'r') ? 1 : (o[k] == 'g') ? 2 : (o[k] == 'b') ? 3 : error("Non 'r', 'g' or 'b' in the 'order' option")
		end
	end
	o = _si_opts(kw)
	O = _sp_compute(index, view(rgb, :, :, bds[1]), view(rgb, :, :, bds[2]), view(rgb, :, :, bds[3]), o)
	return _si_finish(O, index, o)
end

# ----------------------------------------------------------------------------------------------------------
function sp_indices(cube::_Cube3, bands::Vector{Int}; index::String="", kw...)
	# This method recieves the cube and a vector with the bands list and calls the worker with @view
	o = _si_opts(kw)			# Do this first because if it errors no point in continuing
	(o.dbg && isa(cube, GMT.GItype)) && println(cube.names)
	b3 = (length(bands) == 2) ? nothing : @view(cube[:,:,bands[3]])
	O = _sp_compute(index, @view(cube[:,:,bands[1]]), @view(cube[:,:,bands[2]]), b3, o)
	return _si_finish(O, index, o)		# a plain Array cube has no georeference: its result is a plain Matrix
end

# ----------------------------------------------------------------------------------------------------------
function sp_indices(bnd1, bnd2, bnd3=nothing; index::String="", kwargs...)
	o = _si_opts(kwargs)
	return _si_finish(_sp_compute(index, bnd1, bnd2, bnd3, o), index, o)
end
