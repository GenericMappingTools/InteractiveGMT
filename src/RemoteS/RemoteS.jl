"""
    InteractiveGMT.RemoteS

Satellite data processing: true colour images with automatic contrast stretch, spectral indices,
Landsat 8 radiometric conversions, MODIS L2 swath gridding, supervised classification and satellite
tracks / scene footprints.

Ported from the standalone RemoteS.jl package (Joaquim Luis, MIT). Two dependencies are gone:
  * SatelliteToolbox (orbit propagation) -> InteractiveGMT's own SGP4/SDP4 (`Satellite`, satellite.jl)
  * DecisionTree (supervised classification) -> a CART tree in decisiontree.jl
so, like the rest of InteractiveGMT, the only dependency is GMT.jl. The stdlibs (Dates, Printf,
Statistics) are reached THROUGH GMT, the same way satellite.jl reaches Dates.

Kept as a submodule, apart from the viewer code: nothing in InteractiveGMT calls into it yet.
Use it as `using InteractiveGMT.RemoteS`.
"""
module RemoteS

using GMT
import GMT.Printf: @sprintf
import GMT.Statistics: median, std, quantile!
import GMT.Dates: DateTime, Period, Day, Hour, Minute, Second, UTC, now, julian2datetime,
                  datetime2julian, year, month, day, hour, minute, dayofyear

# The orbit propagator: InteractiveGMT's own (SGP4/SDP4 in deps/src/satellite.cpp). Never a second one.
import ..InteractiveGMT: TLE, Satellite, read_tle, subpoint, propagate_ecef, close!
import ..InteractiveGMT: jd as _sat_jd

const SCENE_HALFW = Dict{String,Int}("AQUA" => 1163479, "TERRA" => 1163479, "LANDSAT8" => 92500)	# half widths

export
	cutcube, subcube, dn2temperature, dn2radiance, dn2reflectance, reflectance_surf, grid_at_sensor, truecolor,
	clg, clre, evi, evi2, gli, gndvi, mndwi, mtci, mcari, msavi, nbri, ndvi, ndwi, ndwi2, ndrei1,
	ndrei2, satvi, savi, slavi, tgi, vari,
	classify, classification_proba, train_raster,
	clip_orbits, findscenes, sat_scenes, sat_tracks, reportbands

include("decisiontree.jl")
include("grid_at_sensor.jl")
include("spectral_indices.jl")
include("utils.jl")
include("sat_tracks.jl")

end # module
