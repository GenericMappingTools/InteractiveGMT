"""
    InteractiveGMT.RemoteS

Satellite data processing: true colour images with automatic contrast stretch, spectral indices,
Landsat 8 radiometric conversions, MODIS L2 swath gridding, supervised classification, satellite
tracks / scene footprints, and NASA HLS (Harmonized Landsat Sentinel-2) index time series.

Ported from the standalone RemoteS.jl package (Joaquim Luis, MIT). Two dependencies are gone:
  * SatelliteToolbox (orbit propagation) -> InteractiveGMT's own SGP4/SDP4 (`Satellite`, satellite.jl)
  * DecisionTree (supervised classification) -> a CART tree in decisiontree.jl
so, like the rest of InteractiveGMT, the only dependency is GMT.jl. The stdlibs (Dates, Printf,
Statistics) are reached THROUGH GMT, the same way satellite.jl reaches Dates.

Kept as a submodule, apart from the viewer code. The Satellite > Remote sensing dialogs
(remotes_gui.jl) are its GUI; from the REPL use it as `using InteractiveGMT.RemoteS`.
"""
module RemoteS

using GMT
import GMT.Printf: @sprintf
import GMT.Statistics: median, std, quantile!
import GMT.Dates: DateTime, Date, Period, Day, Hour, Minute, Second, UTC, now, julian2datetime,
                  datetime2julian, year, month, day, hour, minute, dayofyear

# HLS (hls.jl): the parent's JSON reader and HTTP GET (sentinelhub.jl, included before this module);
# the .netrc helpers of remotes_gui.jl come AFTER it, so they are reached through the module at run time.
import ..InteractiveGMT
import ..InteractiveGMT: _shub_json, _shub_http_get, _shub_esc, _shub_str

# The orbit propagator: InteractiveGMT's own (SGP4/SDP4 in deps/src/satellite.cpp). Never a second one.
import ..InteractiveGMT: TLE, Satellite, read_tle, subpoint, propagate_ecef, close!
import ..InteractiveGMT: jd as _sat_jd

const SCENE_HALFW = Dict{String,Int}("AQUA" => 1163479, "TERRA" => 1163479, "LANDSAT8" => 92500)	# half widths

export
	cutcube, subcube, dn2temperature, dn2radiance, dn2reflectance, reflectance_surf, grid_at_sensor, truecolor,
	clg, clre, evi, evi2, gli, gndvi, mndwi, mtci, mcari, msavi, nbri, ndmi, ndvi, ndwi, ndwi2, ndrei1,
	ndrei2, satvi, savi, slavi, tgi, vari,
	classify, classification_proba, train_raster,
	clip_orbits, findscenes, sat_scenes, sat_tracks, reportbands,
	HLSItem, hls_search, hls_series, s2_search, s2_series

include("decisiontree.jl")
include("grid_at_sensor.jl")
include("spectral_indices.jl")
include("utils.jl")
include("sat_tracks.jl")
include("hls.jl")

end # module
