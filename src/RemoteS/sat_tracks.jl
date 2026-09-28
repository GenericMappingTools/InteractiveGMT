# Satellite tracks, scene footprints and scene finding.
#
# The orbit comes from InteractiveGMT's own SGP4/SDP4 propagator (`Satellite`, satellite.jl ->
# deps/src/satellite.cpp), not SatelliteToolbox: the geodetic sub-satellite point from `subpoint`,
# the Earth-fixed position from `propagate_ecef`. Nothing here does any orbital mechanics.

"""
    sat_tracks(; geocentric::Bool=false, tiles::Bool=false, position::Bool=false, kwargs...)

Compute satellite tracks using the TLE, or Two Line Elements set, a data format that contains
information about the orbit at a specific epoch of an Earth-orbiting object. It can also
calculate polygons arround the scene extents of AQUA and TERRA satellites as well as create
the scene names, which provides a mean to direct download that data.
  - `start`: A DateTime object or a string convertable to a DateTime with `DateTime(start)`
     specifying the start of the orbit calculation. If omited, current time in UTC will be used.
  - `duration`: Length of time for which the orbit is calculated. Accepts duration in days, hours,
     minutes or seconds. The default is minutes (100 minutes). To use other units use a string with
     the value appended with 'D', 'h', 'm' or 's'. _e.g._ `duration="55m"` to compute orbit 55 minutes from `start`
  - `step` or `inc` or `dt`: The time interval at which to compute locations along the orbit. The default
     unit here is seconds (30 sec) but minutes can be used as well by appending 'm'. _e.g._ `step="1m"`
  - `stop`: As alternative to `duration` provide the end date for the orbit. Same conditions as `start`
  - `position`: Computes only first location at the `start` time. Boolean, use `position=true`
  - `geocentric`: Boolean to controls if output is `lon,lat,alt,time` (the default) or `ECEF` coordinates + time.
     Altitude and ECEF coordinates are in meters, time is the Julian Day.
  - `tle` or `TLE`: a file name with the TLE data for a specific satellite and period. It can also be a
    two elements string vector with the first and second lines of the TLE file, or a `TLE` object.
  - `tiles`: Compute the scene limits and file names for some satellites. Currently AQUA only.
  - `sat`, `SAT` or `satellite`: Name of the satellite to use; choose from (string or symbols)
     :TERRA, :AQUA. Use only with the `tiles` option.

### Returns
A GMTdataset with the orbit or the scene polygons

## Example:
Compute ~one orbit of the AQUA satellite starting at current local time. Note, this
will be accurate for the month of September 2021. For other dates it needs an updated TLE.

    tle1 = "1 27424U 02022A   21245.83760660  .00000135  00000-0  39999-4 0  9997";
    tle2 = "2 27424  98.2123 186.0654 0002229  67.6025 313.3829 14.57107527 28342";
    orb = sat_tracks(tle=[tle1; tle2], duration=100);

and the orbit track can be visualized with

    imshow(orb,  proj=:Robinson, region=:global, coast=true)
"""
function sat_tracks(; geocentric::Bool=false, tiles::Bool=false, position::Bool=false, kwargs...)
	(position && tiles) && error("Cannot require tiles and a single position. Makes no sense.")
	d = KW(kwargs)

	start = ((val = find_in_dict(d, [:start])[1]) === nothing) ? now(UTC) : getitDTime(val)
	sat_name = (tiles) ? get_sat_name(d) : ""
	(tiles) && (start = round(start, Minute(5)))	# This is for MODIS only
	stop = _track_stop(d, start)
	dt = (tiles) ? 60 : _track_step(d)				# 60 s: arbitrary choice that works well for MODIS
	tle = ((val = find_in_dict(d, [:tle_obj])[1]) !== nothing) ? _as_tle(val) : loadTLE(d)	# Some other fun already got it

	t = (position) ? [start] : collect(start:Second(dt):stop)	# Single position, or the whole track
	out = _track_matrix(tle, t, geocentric)

	(tiles) && return sat_scenes(out, sat_name)
	(geocentric) && return mat2ds(out, geom=UInt32(2), colnames=["X", "Y", "Z", "JulianDay"])
	return mat2ds(out, geom=UInt32(2), colnames=["lon", "lat", "alt", "JulianDay"], proj=GMT.prj4WGS84)
end

# The track as an (n x 4) matrix: [lon lat alt_m jd] or, `geocentric`, [X Y Z jd] (ECEF metres).
function _track_matrix(tle::TLE, t::Vector{DateTime}, geocentric::Bool)::Matrix{Float64}
	sat = Satellite(tle)
	try
		j = Float64[_sat_jd(x) for x in t]		# the propagator's own DateTime -> JD, never a second one
		out = Matrix{Float64}(undef, length(j), 4)
		if (geocentric)
			E = propagate_ecef(sat, j)			# 3 x N, km
			@inbounds for n in eachindex(j)
				out[n,1], out[n,2], out[n,3], out[n,4] = E[1,n] * 1000, E[2,n] * 1000, E[3,n] * 1000, j[n]
			end
		else
			lon, lat, alt = subpoint(sat, j)	# degrees, degrees, km
			@inbounds for n in eachindex(j)
				out[n,1], out[n,2], out[n,3], out[n,4] = lon[n], lat[n], alt[n] * 1000, j[n]
			end
		end
		return out
	finally
		close!(sat)
	end
end

# `stop` from the `duration` (D, h, m or s; a number = minutes) or `stop` kwargs. Default ~Terra period.
function _track_stop(d::Dict, start::DateTime)::DateTime
	if ((val = find_in_dict(d, [:duration])[1]) !== nothing)
		return start + _duration(val)
	elseif ((val = find_in_dict(d, [:stop])[1]) !== nothing)
		return getitDTime(val)
	end
	return start + Minute(100)
end

_duration(val::Real)::Period = Minute(trunc(Int, val))		# Assume duration was given in minutes
function _duration(val::String)::Period						# Accept duration in D(ays), h(ours), m(inutes) or s(econds)
	n = parse(Int, val[1:end-1])
	endswith(val, "D") && return Day(n)
	endswith(val, "h") && return Hour(n)
	endswith(val, "m") && return Minute(n)
	endswith(val, "s") && return Second(n)
	error("Only 'D', 'h', 'm' or 's' are accepted in duration")
end
_duration(val) = _duration(string(val))

# The track step in seconds, from `step`/`inc`/`dt` (a number = seconds, or "Nm"/"Ns"). Default 30 s.
function _track_step(d::Dict)::Int
	((val = find_in_dict(d, [:step :inc :dt])[1]) === nothing) && return 30
	if (isa(val, String))
		endswith(val, "m") && return parse(Int, val[1:end-1]) * 60
		endswith(val, "s") && return parse(Int, val[1:end-1])
		error("Only 's' or 'm' are accepted in increment")
	end
	return trunc(Int, val)
end

# --------------------------------------------------------------------------------------------
_as_tle(t::TLE)::TLE = t
_as_tle(v::Vector{String})::TLE = (length(v) == 2) ? TLE("", v[1], v[2]) : error("BAD input TLE data")
function _as_tle(s::String)::TLE
	tles = read_tle(s)			# a file name or the TLE text itself
	isempty(tles) && error("No TLE found in $(s)")
	return tles[1]
end
_as_tle(x) = error("BAD input TLE data: $(typeof(x))")

function loadTLE(d::Dict)::TLE
	# Load a TLE or use a default one. In a function because it's used by two functions
	((val = find_in_dict(d, [:tle :TLE])[1]) !== nothing) && return _as_tle(val)
	isfile("C:\\v\\AQUA.tle") && return _as_tle("C:\\v\\AQUA.tle")		# RemoteS' historical default
	error("Must provide the satellite TLE with the `tle` option (a file name or the two TLE lines)")
end

# --------------------------------------------------------------------------------------------
getitDTime(val::String)::DateTime = DateTime(val)
getitDTime(val::Tuple)::DateTime = DateTime(val...)
getitDTime(val::DateTime)::DateTime = val
getitDTime(val) = error("Bad input type $(typeof(val)). Must be a DateTime, a String or a Tuple(Int)")

# --------------------------------------------------------------------------------------------
function get_sat_name(d::Dict)::String
	# Get the satellite name from kwargs (encoded in 'd'). Used by at least two functions
	((val = find_in_dict(d, [:sat :SAT :satellite])[1]) === nothing) &&
		error("Must provide the satellite name. Pick one of :TERRA, :AQUA")
	sat = uppercase(string(val))
	(sat != "TERRA" && sat != "AQUA" && sat != "LANDSAT8") &&
		error("Unknown satellite name $sat. Must be one of :TERRA, :AQUA")
	return sat
end

# --------------------------------------------------------------------------------------------
"""
sat_scenes(track, sat_name::String)

Compute polygons delimiting AQUA and TERRA scenes.

- `trac`: Is an orbit computed with `sat_tracks` at steps of 1 minute (crucial)
- `sat_name`: The satellite name. At this time only AQUA and TERRA are allowed.

Returns a GMTdataset vector with the polygons and the scene names in the dataset `header` field.

# Example
Imagine that `orb` was obtained with

orb = sat_tracks(tle=[tle1; tle2], start=DateTime("2021-09-02T13:30:00"),
	stop=DateTime("2021-09-02T13:40:00"), step="1m");

The scenes limits (two) are computed with:

```
Dscenes = sat_scenes(orb, "AQUA");
```

"""
sat_scenes(track::GMTdataset, sat_name::String) = sat_scenes(Matrix{Float64}(track.data), sat_name)
function sat_scenes(track::Matrix{Float64}, sat_name::String)::Vector{GMTdataset{Float64,2}}
	# Compute polygons with the scene limits. 'halfwidth' is half the scene width.
	halfwidth = get(SCENE_HALFW, sat_name, 0)
	(halfwidth == 0) && error("Currently only \"AQUA\" and \"TERRA\" satellite names are available")
	_, azim, = invgeod(track[1:end-1, 1:2], track[2:end, 1:2])	# distances and azimuths along the tracks
	azim = vec(azim)
	push!(azim, azim[end])		# To make it same size of tracks
	D = GMTdataset{Float64,2}[]
	for k = 1:5:length(azim)-5
		ll14 = geod(track[k,1:2],   [azim[k]+90, azim[k]-90], halfwidth)[1]
		ll23 = geod(track[k+5,1:2], [azim[k+5]+90, azim[k+5]-90], halfwidth)[1]
		sc = get_MODIS_scene_name(track[k,4], sat_name)
		push!(D, GMTdataset{Float64,2}(data=[ll14[1:1,:]; ll23[1:1,:]; ll23[2:2,:]; ll14[2:2,:]; ll14[1:1,:]],
		                               colnames=["lon","lat"], header=sc, geom=GMT.Gdal.wkbPolygon))
	end
	isempty(D) || (D[1].proj4 = GMT.prj4WGS84)
	D
end

# --------------------------------------------------------------------------------------------
function get_MODIS_scene_name(jd::Float64, prefix::String, sst::Bool=true)::String
	# Build the scene name after it's acquisition time coded in the JD argin. OB.DAAC's current scheme,
	# the same for both products (the old "A2021251125500.L2_LAC_OC.nc" chlorophyll names are gone).
	# The SECONDS are a guess: the real file carries the time of its first scan, which is :00 or :01
	# in runs that no orbit predicts. The minute is right; the exact name comes from OB.DAAC's listing.
	DT = julian2datetime(jd)
	p = (prefix[1] == 'A') ? "AQUA_MODIS." : "TERRA_MODIS."
	@sprintf("%s%.4d%.2d%.2dT%.2d%.2d01.L2.%s.NRT.nc", p, year(DT), month(DT), day(DT), hour(DT), minute(DT),
	         sst ? "SST" : "OC")
end

# --------------------------------------------------------------------------------------------
"""
    clip_orbits(track, BoundingBox::Vector{<:Real})

Clips the orbits that are contained inside a rectangular geographical region

- `track`: A GMTdataset or a Mx2 matrix with the orbits [lon, lat] position. This is normally calculated
   with the `sat_tracks` function.
- `BoundingBox`: A vector the region limits made up with [lon_min, lon_max, lat_min, lat_max]

Returns a GMTdataset vector with the chunks of `tracks` that cross inside the `BoundingBox` region.

#Example
Suppose `orb` holds orbits computed with sat_tracks() during 2 days, clip them inside the 20W-10E, 30N-45N window

```
D = clip_orbits(orb, [-20, 10, 30, 50]);
```
"""
function clip_orbits(track, bb::Vector{<:Real})::Vector{GMTdataset{Float64,2}}
	# Select the parts of the track that are contained inside the BB.
	(length(bb) != 4) && error("The BoundingBox vector must have 4 elements")
	segments = GMT.gmtselect(track, region=bb, f=:g)
	seg = Matrix{Float64}((isa(segments, Vector) ? segments[1] : segments).data)
	colnames, prj4 = isa(track, Vector) ? (track[1].colnames, track[1].proj4) :
	                 isa(track, GMTdataset) ? (track.colnames, track.proj4) : (String[], "")
	return _clip_orbits(seg, colnames, prj4)
end

function _clip_orbits(seg::Matrix{Float64}, colnames::Vector{String}, prj4::String)::Vector{GMTdataset{Float64,2}}
	azim = vec(invgeod(seg[1:end-1, 1:2], seg[2:end, 1:2])[2])
	inds = findall(abs.(diff(azim)) .> 10)			# Detect line breaks

	begin_seg = [1; inds[2:2:length(inds)].+1]		# Not easy to explain why those indices give seg boundaries
	end_seg   = [inds[2:2:length(inds)]; length(azim)+1]
	D = Vector{GMTdataset{Float64,2}}(undef, length(begin_seg))
	for k = 1:length(begin_seg)
		D[k] = GMTdataset{Float64,2}(data=seg[begin_seg[k]:end_seg[k], :], colnames=colnames, proj4=prj4,
		                             geom=GMT.Gdal.wkbLineString)
	end
	D[1].proj4 = GMT.prj4WGS84
	D
end

# --------------------------------------------------------------------------------------------
"""
    findscenes(lon::Real, lat::Real; kwargs...)

Find the names of the scenes that cover the location point `lon`, `lat` in the period determined
by the dates and satellite set via kwargs.
  - `day`: Search only on the day time part of the orbits.
  - `night`: Search only on the night time part of the orbits.
  - `oc`: For the AQUA or TERRA satellites pick only the chlorophyl content scenes.
  - `sst`: For the AQUA or TERRA satellites pick only the Sae Surface Temperature content scenes.
  - `sat`, `SAT` or `satellite`: Name of the satellite to use; choose from (string or symbols)
     :TERRA, :AQUA
  - `start`: A DateTime object or a string convertable to a DateTime with `DateTime(start)`
     specifying the start of the looking period. If omited, current time in UTC will be used.
  - `duration`: Length of time for which the scenes are searched. The duration is expected in days
     and can be a negative number, meaning we'll look that span days from `start`.
  - `stop`: As alternative to `duration` provide the end date for the serch. Same conditions as `start`
  - `tle` or `TLE`: a file name with the TLE data for a specific satellite and period. It can also be a
    two elements string vector with the first and second lines of the TLE file.

### Returns
A string vector with the scene names

## Example:
Find the AQUA scenes with chlorophyl-a (oceancolor) that cover the point (-8, 36) in the two days
before "2021-09-07T17:00:00"
Note, this will be accurate for the month of September 2021. For other dates it needs an updated TLE.

    tle1 = "1 27424U 02022A   21245.83760660  .00000135  00000-0  39999-4 0  9997";
    tle2 = "2 27424  98.2123 186.0654 0002229  67.6025 313.3829 14.57107527 28342";
    findscenes(-8,36, start="2021-09-07T17:00:00", sat=:aqua, day=true, duration=-2, oc=1, tle=[tle1, tle2])

	2-element Vector{String}:
	"AQUA_MODIS.20210906T131001.L2.OC.NRT.nc"
	"AQUA_MODIS.20210907T135001.L2.OC.NRT.nc"

The seconds (and "NRT") are a guess; the name OB.DAAC really gave the file is in its Level-2 listing.
"""
function findscenes(lon::Real, lat::Real; kwargs...)
	d = KW(kwargs)
	sat = get_sat_name(d)

	day   = haskey(d, :day)
	night = haskey(d, :night)
	sst   = haskey(d, :sst)

	start = ((val = find_in_dict(d, [:start])[1]) === nothing) ? now(UTC) - Day(2) : getitDTime(val)
	if ((val = find_in_dict(d, [:duration])[1]) !== nothing)
		period = round(Int, val)
		(period == 0) && (period = -1; @warn("Duration too short. Using -1 days"))
		# Negative = that many days BEFORE `start` (the original shifted the window 2 days into the future).
		if (period < 0)  stop, start = start, start - Day(-period)
		else             stop = start + Day(period)
		end
	else
		stop  = ((val = find_in_dict(d, [:stop])[1])  === nothing) ? start + Day(2) : getitDTime(val)
	end
	(start >= stop) && error("Start date ($start) is greater or equal to stop ($stop)")
	(haskey(d, :Vd)) && println("start = ",start, " stop = ", stop)

	tle = loadTLE(d)
	tracks = sat_tracks(start=start, stop=stop, tle_obj=tle)
	BB = [lon-10, lon+10, lat-10, lat+10]
	D = clip_orbits(tracks, BB)
	(day || night) && (D = day_night_orbits(D, day=day, night=night))
	isempty(D) && (@warn("No tracks left after this choice."); return nothing)

	scenes = String[]
	dists = mapproject(D, G=(lon,lat))
	for k = 1:GMT.numel(dists)
		dk, ind = findmin(view(dists[k].data, :,5))
		if (dk <= SCENE_HALFW[sat])
			jd = datetime2julian(floor(julian2datetime(dists[k][ind,4]), Minute(5)))
			push!(scenes, get_MODIS_scene_name(jd, sat, sst))
		end
	end
	scenes
end

# --------------------------------------------------------------------------------------------
function day_night_orbits(D::Vector{<:GMTdataset}; day::Bool=false, night::Bool=false)
	# Pick only the day or night orbits in D. D is normally the output of the clip_orbits() fun
	(!day && !night) && return D		# No selection requested

	pass = zeros(Bool, length(D))
	for k = 1:length(D)
		lon, lat = D[k][1,1], D[k][1,2]
		jd = D[k][1,4]
		sun = solar(I=@sprintf("%.4f/%.4f+d%s", lon, lat, string(julian2datetime(jd))), C=true)
		raise = Float64(sun[5]);  set = Float64(sun[6])
		# Subtract 0.5 because julian2datetime(0.0) = -4713-11-24T12:00:00. That is JDs are shifted of 0.5
		t1, t2 = jd - 0.5, D[k][end,4] - 0.5
		dday_first, dday_last = t1 - trunc(t1), t2 - trunc(t2)
		if (day)
			if ((raise < dday_first < set) && (raise < dday_last < set))
				pass[k] = true
			elseif (dday_first < raise && dday_last > raise) || (dday_first > raise && dday_last > set)
				@warn("Track $k contains day and night parts. Accepting it as day but be warned of this.")
				pass[k] = true
			end
		else		# night
			if !((raise < dday_first < set) && (raise < dday_last < set))
				pass[k] = true
			elseif (dday_first < raise && dday_last > set) || (dday_first > raise && dday_last > set)
				@warn("Track $k contains day and night parts. Accepting it as night but be warned of this.")
				pass[k] = true
			end
		end
	end
	D[pass]
end
