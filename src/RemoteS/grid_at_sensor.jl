"""
    G = grid_at_sensor(fname::String, sds_name::String=""; V::Bool=false, kw...)

Read one of those netCDF files that are not regular grids but have instead the coordinates in the
LONGITUDE and LATITUDE arrays. MODIS L2 files are a good example of this. Data in theses files are
not layed down on a regular grid and we must interpolate to get one. Normally the lon and lat arrays
are called ``longitude`` and ``latitude`` and these it's what is seek for by default. But files exist
that pretend to comply to CF but use other names. In this case, use the kwargs `xarray` & `yarray`
to pass in the variable names. For example: `xarray="XLONG"`, `yarray="XLAT"`
The other fundamental info to pass in is the name of the array to be read/interpolated. We do that
via the `sds_name` arg.

- `band`:
  In simpler cases the variable to be interpolated lays down on a 2D array but it is also possible that
  it is stored in a 3D array. If that is the case, use the keyword 'band' to select a band (ex: 'band=2')
  Bands are numbered from 1.

- `region` | `limits`, `inc` | `increment` | `spacing` and `search_radius`:
  The interpolation is done so far with ``nearneighbor`` Both the region (-R) and increment (-I) are estimated
  from data but they can be set with `region` and `inc` kwargs as well. One can also set the ``nearneighbor``
  serach radius with option `search_radius`. The defaul is to set `search_radius` equal to two times
  the average increment.

- `quality`:
  For MODIS data we can select the quality flag to filter by data quality. By default the best quality (=0) is
  used, but one can select another with the `quality=val` kwarg. Positive 'val' values select data of quality
  <= quality, whilst negative 'val' values select only data with quality >= abs(val). This allows for example
  to extract only the cloud coverage.

- `t_srs` or `target_proj`: Some polar grids come with ``longitude``, ``latitude`` (or just ``lon``, ``lat``)
  arrays in geographical coordinates. There must be an (obscure) reason for this but the practical result is
  messy because coordinate spacings are highly variable preventing any decent guess. In these cases it is useful
  to reproject the data before griding. For that purpose use the `t_srs` or `target_proj` option to tell the
  program to do a coordinate conversion before gridding. `t_srs` should then be a proj4 string with the destiny
  projection system.

- `nodata`: Sometimes datasets use other than NaN to represent nodata but they don't specify it in the
  netCDF attributes (*e.g.* the NSIDC products). This option allows to fix this (*i.e* `nodata=-9999`)
  Note that this is automatically set for the NSIDC products.

- `nointerp`: Means to not do any nearneighbor interpolation but needs that `region` has been set.

- `NSIDC_N` and `NSIDC_S`: Set the `s_srs`, `region`, `nointerp`, `nodata` appropriate to read the See Ice NSIDC
  https://nsidc.org/data/polar-stereo/ps_grids.html grids.

- `dataset` or `xyz`: If instead of calculating a grid (returned as a GMTgrid type) user wants the x,y,z data
  intself, use the keywords `dataset`, or `xyz` and the output will be in a GMTdataset (i.e. use `dataset=true`).

To inquire just the list of available arrays use `list=true` or `gdalinfo=true` to get the full file info.

    Examples:

    G = grid_at_sensor("AQUA_MODIS.20020717T135006.L2.SST.nc", "sst", V=true);

    G = grid_at_sensor("TXx-narr-annual-timavg.nc", "T2MAX", xarray="XLONG", yarray="XLAT", V=true);

    G = grid_at_sensor("RDEFT4_20101021.nc", "sea_ice_thickness", NSIDC_N=true);
"""
function grid_at_sensor(fname::String, sds_name::String=""; quality::Int=0, V::Bool=false, kw...)

	d = KW(kw)
	(haskey(d, :gdalinfo)) && (return println(_gdalinfo_str(fname)))
	inc = _inc_arg(find_in_dict(d, [:inc :increment :spacing])[1])
	info, ind_EOLs, is_MODIS = _subdatasets_info(fname)

	if (haskey(d, :list))
		println("List of bands in this file:")
		for nm in _sds_names(info, ind_EOLs)  println("\t", nm)  end
		return nothing
	end

	(sds_name == "") && error("Must provide the band name to process. Try grid_at_sensor(\"\", list=true) to print available bands")

	# Get the arrays  SUBDATASET names
	sds_z, sds_qual, sds_lon, sds_lat = _sensor_sds(sds_name, d, info, ind_EOLs, is_MODIS)

	# Get the arrays with the data
	band::Int = ((val = find_in_dict(d, [:band])[1]) !== nothing) ? Int(val) : 1
	t_srs = _get_prj(d, [:t_srs :target_proj])
	nodata::Float64 = ((val = find_in_dict(d, [:nodata])[1]) !== nothing) ? Float64(val) : Inf
	opt_R::String = GMT.parse_R(d, "")[1]

	if ((haskey(d, :nointerp) && opt_R != "") || haskey(d, :NSIDC_N) || haskey(d, :NSIDC_S))	# No interps
		return _grid_at_sensor_nointerp(d, sds_z, band, nodata, opt_R)
	end
	lon, lat, z_vals, inc, proj4 = get_xyz_qual(sds_lon, sds_lat, sds_z, quality, sds_qual, inc, band, t_srs, nodata, V)

	if (opt_R == "" && !haskey(d, :xyz) && !haskey(d, :dataset))	# If != "" believe it makes sense as a -R option
		opt_R = @sprintf("%.10g/%.10g/%.10g/%.10g", _sensor_region(lon, lat, inc)...)
	else
		opt_R = opt_R[4:end]					# Because it already came with " -R....." from parse_R()
	end

	in = [vec(lon) vec(lat) vec(z_vals)]
	if (haskey(d, :xyz) || haskey(d, :dataset))
		O = mat2ds(in)
	else
		s_rad::String = ((val = find_in_dict(d, [:S :search_radius])[1]) !== nothing) ? string(val) : string(inc[1]+inc[2])
		O = nearneighbor(in, I=inc, R=opt_R, S=s_rad, Vd=(V) ? 1 : 0)
	end
	O.proj4 = proj4
	return O
end

# The full SUBDATASET names of the data array, its MODIS quality array ("" if none) and the lon/lat arrays.
function _sensor_sds(sds_name::String, d::Dict, info::String, ind_EOLs::Vector{UnitRange{Int64}}, is_MODIS::Bool)
	sds_z  = helper_find_sds(sds_name, info, ind_EOLs)		# Return the full SUBDATASET name (a string)
	x_name::String = ((val = find_in_dict(d, [:xarray])[1]) !== nothing) ? string(val) : "longitude"
	y_name::String = ((val = find_in_dict(d, [:yarray])[1]) !== nothing) ? string(val) : "latitude"
	sds_qual = (is_MODIS) ? helper_find_sds("qual_" * sds_name, info, ind_EOLs) : ""
	return sds_z, sds_qual, helper_find_sds(x_name, info, ind_EOLs), helper_find_sds(y_name, info, ind_EOLs)
end

# The -R grid_at_sensor grids on when none is given: the data's extent, W and S rounded to the
# increment's decimals, E and N a whole number of increments away.
function _sensor_region(lon::AbstractArray{<:Real}, lat::AbstractArray{<:Real}, inc::Vector{Float64})::NTuple{4,Float64}
	inc_txt = split("$(inc[1])", '.')[2]	# To count the number of decimal digits to use in rounding
	nd = length(inc_txt)					# and the number of decimals count
	min_lon, max_lon = extrema(lon)
	min_lat, max_lat = extrema(lat)
	west  = round(min_lon; digits=nd);	east  = west  + round(Int, (max_lon - west)  / inc[1]) * inc[1]
	south = round(min_lat; digits=nd);	north = south + round(Int, (max_lat - south) / inc[2]) * inc[2]
	return Float64(west), Float64(east), Float64(south), Float64(north)
end

"""
    inc, (W, E, S, N) = sensor_limits(fname, sds_name; quality=0, inc=[0.0, 0.0])

The increment and the region `grid_at_sensor(fname, sds_name; quality, inc)` grids on when it is
given no region — the same arrays read, the same quality filter, the same estimate (when `inc` is
zeros) — without the gridding.
"""
function sensor_limits(fname::String, sds_name::String; quality::Int=0,
                       inc::Vector{Float64}=[0.0, 0.0])::Tuple{Vector{Float64},NTuple{4,Float64}}
	info, ind_EOLs, is_MODIS = _subdatasets_info(fname)
	sds_z, sds_qual, sds_lon, sds_lat = _sensor_sds(sds_name, Dict{Symbol,Any}(), info, ind_EOLs, is_MODIS)
	lon, lat, _, inc, _ = get_xyz_qual(sds_lon, sds_lat, sds_z, quality, sds_qual, inc)
	return inc, _sensor_region(lon, lat, inc)
end

# gdalinfo of a file, retried once after a GMT reset (something still screws it time to time).
function _gdalinfo_str(fname::String)::String
	!isfile(fname) && error("This file $(fname) does not exist")
	_info = gdalinfo(fname)
	if (_info === nothing)
		GMT.resetGMT()
		_info = gdalinfo(fname)
	end
	(_info === nothing) && error("gdalinfo failed to read $(fname)")
	return _info
end

# The "Subdatasets:" block of a file's gdalinfo, chopped to where the needed info lives, its line
# ends, and whether the file is a MODIS Level-2 product.
function _subdatasets_info(fname::String)::Tuple{String, Vector{UnitRange{Int64}}, Bool}
	info = _gdalinfo_str(fname)
	((ind = findfirst("Subdatasets:", info)) === nothing) && error("This file " * fname * " has no Subdatasets")
	is_MODIS = findfirst("MODISA Level-2", info) !== nothing
	info = info[ind[1]+12:end]		# Chop up the long string into smaller chunk where all needed info lives
	ind = findlast("SUBDATASET_", info)
	info = info[1:ind[1]]			# Chop even last SUBDATASET_X_DESC string that we wouldn't use anyway
	return info, findall("\n", info), is_MODIS
end

# The array names of the subdatasets (what `list=true` prints and `sds_name` takes).
function _sds_names(info::String, ind_EOLs::Vector{UnitRange{Int64}})::Vector{String}
	c = (findlast("/", info[ind_EOLs[1][1] : ind_EOLs[2][1]-1]) !== nothing) ? '/' : ':'
	return String[String(split(info[ind_EOLs[k-1][1] : ind_EOLs[k][1]-1], c)[end]) for k = 2:2:length(ind_EOLs)]
end

"""
    sds_names(fname::String) -> Vector{String}

The names of the arrays in a swath file, i.e. the `sds_name` values `grid_at_sensor` accepts.
"""
sds_names(fname::String)::Vector{String} = _sds_names(_subdatasets_info(fname)[1:2]...)

# `inc` in any of the forms the kwarg accepts -> [dx, dy] (zeros = "estimate it from the data").
_inc_arg(::Nothing)::Vector{Float64} = [0.0, 0.0]
_inc_arg(v::Real)::Vector{Float64} = [Float64(v), Float64(v)]
_inc_arg(v::Tuple)::Vector{Float64} = Float64[Float64(x) for x in v]
_inc_arg(v::AbstractVector{<:Real})::Vector{Float64} = Float64.(v)
_inc_arg(v::String)::Vector{Float64} = (p = parse.(Float64, split(v, '/')); length(p) == 1 ? [p[1], p[1]] : p)

# A proj/epsg setting from kwargs, as a proj4 string ("" = none).
function _get_prj(d::Dict, symbs)::String
	((val = find_in_dict(d, symbs)[1]) === nothing) && return ""
	isa(val, Integer) && return epsg2proj(Int(val))
	return string(val)
end

# The no-interpolation path: the data array IS the grid, only its georeferencing is set.
function _grid_at_sensor_nointerp(d::Dict, sds_z::String, band::Int, nodata::Float64, opt_R::String)
	G = gd2gmt(sds_z; band=band)::GMTgrid
	do_flip = true
	if (haskey(d, :NSIDC_N))
		lims = [-3850000.0, 3750000, -5350000, 5850000];	G.proj4 = epsg2proj(3413);	nodata == Inf && (nodata = -9999.0)
	elseif (haskey(d, :NSIDC_S))
		lims = [-3950000.0, 3950000, -3950000, 4350000];	G.proj4 = epsg2proj(3976);	nodata == Inf && (nodata = -9999.0)
	else
		lims = parse.(Float64, split(opt_R[4:end], "/"));	do_flip = false
	end

	G.inc = [(lims[2] - lims[1]) / size(G, 1), (lims[4] - lims[3]) / size(G, 2)]
	G.range[1:4], G.registration = lims, 1
	((s_srs = _get_prj(d, [:s_srs :source_proj])) != "") && (G.proj4 = s_srs)
	# The rows come off the file in the other north/south order: say so in the layout (T <-> B),
	# never by reversing the buffer. (Was `G.z = G.z[:, end:-1:1]`, a full copy.)
	(haskey(d, :flipud) || do_flip) && (G.layout = _flip_ns_layout(G.layout))

	(nodata != Inf) && _nodata_to_nan!(G.z, nodata, G)	# If asked to replace the nodata by NaNs
	return G
end

# The same buffer read from the other end in y: only the layout's first letter changes.
function _flip_ns_layout(layout::String)::String
	lay = (layout == "") ? "TRB" : layout		# gd2gmt's own default
	return ((lay[1] == 'T') ? "B" : "T") * lay[2:end]
end

function _nodata_to_nan!(z::AbstractArray{T}, nodata::Float64, G::GMTgrid) where T<:AbstractFloat
	@inbounds Threads.@threads for k = 1:length(z)
		(z[k] == nodata) && (z[k] = T(NaN))
	end
	G.range[5] = GMT.minimum_nan(z)
	return nothing
end

# ---------------------------------------------------------------------------------------------------
function helper_find_sds(sds::String, info::String, ind_EOLs::Vector{UnitRange{Int64}})::String
	if ((ind = findfirst("/" * sds, info)) === nothing)
		odeusse = false
		if ((ind = findfirst(":" * sds, info)) === nothing)		# Try the variations "lon", "lat"
			if (startswith(sds, "lat"))
				((ind = findfirst(":lat", info)) === nothing) && (odeusse = true)
			elseif (startswith(sds, "lon"))
				((ind = findfirst(":lon", info)) === nothing) && (odeusse = true)
			else
				odeusse = true
			end
		end
		odeusse && error("The band name -- " * sds * " -- does not exist")
	end
	k = 1;	while (ind_EOLs[k][1] < ind[1])  k += 1  end
	return info[ind_EOLs[k-1][1]+3:ind_EOLs[k][1]-1]	# +3 because 1 = \n and the other 2 are blanks
end

# ---------------------------------------------------------------------------------------------------
function get_xyz_qual(sds_lon::String, sds_lat::String, sds_z::String, quality::Int, sds_qual::String="",
	                  inc::Vector{Float64}=[0.,0.], band::Int=1, t_srs::String="", nodata::Float64=Inf, V::Bool=false)
	# Get a Mx3 matrix with data to feed interpolator. Filter with quality if that's the case
	# If INC == 0, also estimate a reasonable increment for interpolation
	(V) && println("Extract lon, lat, " * sds_z * " from file")
	G = gd2gmt(sds_z; band=band)::GMTgrid
	z_vals = G.z				# a Matrix, or the Vector of the nodes that pass the quality/fill filter
	proj4::String = G.proj4
	dx = Float64[];  dy = Float64[]

	if (sds_qual != "")
		Gqual = gd2gmt(sds_qual)::GMTimage
		qual = (quality >= 0) ? (Gqual.image .< quality + 1) :	# Best (0), Best+Intermediate (1) or all (2)
		                        (Gqual.image .> -quality - 1)		# Pick only, Intermediate+Lousy (-1) or Lousy (-2)
		qual = reshape(qual, size(qual,1), size(qual,2))
		z_vals = z_vals[qual]
		xx, yy, dx, dy = get_lon_lat_qual(sds_lon, sds_lat, qual, inc)
		(proj4 == "") && (proj4 = GMT.prj4WGS84)					# Almost for sure that's always the case
	else
		info::String = gdalinfo(GMT.trim_SUBDATASET_str(sds_z))
		if (nodata != Inf)  fill_val, got_fill_val = nodata, true
		else                fill_val, got_fill_val = GMT.get_FillValue(info)
		end
		if (got_fill_val)
			qual = (z_vals .!= fill_val)
			z_vals = z_vals[qual]
			xx, yy, dx, dy = get_lon_lat_qual(sds_lon, sds_lat, qual, inc)
		else
			xx = Float64.((gd2gmt(sds_lon)::GMTgrid).z)
			yy = Float64.((gd2gmt(sds_lat)::GMTgrid).z)
			(inc[1] == 0.0) && (dx = diff(xx[:, round(Int, size(xx, 1)/2)]))
			(inc[2] == 0.0) && (dy = diff(yy[round(Int, size(yy, 2)/2), :]))
		end
		(proj4 == "") && (proj4 = GMT.seek_wkt_in_gdalinfo(info))
	end

	if (t_srs != "")		# TODO. Allow other than PROJ4
		s_srs = (proj4 == "" || startswith(proj4, "+proj=lon") || startswith(proj4, "+proj=lat")) ? "+proj=longlat +datum=WGS84" : proj4
		if (isa(xx, Matrix))		# No 'qual' or 'fill_values'
			xy = lonlat2xy([vec(xx) vec(yy)], t_srs=t_srs, s_srs=s_srs)
			xx, yy = xy[:,1], xy[:,2]
			dx = diff(xx[1:round(Int, size(z_vals,2)/4)])	# Dividining by 4 may save from dateline jumps too.
			dy = diff(yy[1:size(z_vals,1):10*size(z_vals,1)])
			z_vals = vec(z_vals)
		else
			xy = lonlat2xy([xx yy], t_srs=t_srs, s_srs=s_srs)
			xx, yy = xy[:,1], xy[:,2]
			dx = diff(xx[1:round(Int, size(G.z,2)/4)])		# ERRADO. AQUI JA NAO SEI O TAMANHO DO XX,YY
			dy = diff(yy[1:size(G.z,1):10*size(G.z,1)])
		end
		proj4 = t_srs
	end

	(inc[1] == 0) && (inc = guess_increment_from_coordvecs(dx, dy))
	(V) && println("Finished extraction ($(length(z_vals)) points), now intepolate")
	return xx, yy, z_vals, inc, proj4
end

function get_lon_lat_qual(sds_lon::String, sds_lat::String, qual::AbstractArray{Bool}, inc::Vector{Float64})
	# Another helper function to get only the lon, lat values that pass the 'qual' criteria
	dx, dy = Float64[], Float64[]
	z = Float64.((gd2gmt(sds_lon)::GMTgrid).z)
	# The dx,y <10 test is because NASA keeps screwing and sometimes puting test values as -999.0
	# in the coordinates arrays. This is so f annoying.
	(inc[1] == 0.0) && (dx = diff(z[:, round(Int, size(z, 1)/2)]); dx = dx[dx .< 10])
	lon = z[qual]
	z = Float64.((gd2gmt(sds_lat)::GMTgrid).z)
	(inc[2] == 0.0) && (dy = diff(z[round(Int, size(z, 1)/2), :]); dy = dy[dy .< 10])
	lat = z[qual]
	return lon, lat, dx, dy
end

# ---------------------------------------------------------------------------------------------------
function guess_increment_from_coordvecs(dx::AbstractVector{<:Real}, dy::AbstractVector{<:Real})::Vector{Float64}
	# Guess a good -I<inc> from the spacings in the x (lon), y(lat) arrays
	x_mean = Float64(median(abs.(dx)));		y_mean = Float64(median(abs.(dy)));
	x_std, y_std = Float64(std(dx)), Float64(std(dy))
	inc_x = (x_std == 0) ? x_mean : round(x_mean, digits=round(Int, abs(log10(x_std))))
	inc_y = (y_std == 0) ? y_mean : round(y_mean, digits=round(Int, abs(log10(y_std))))
	[inc_x, inc_y]
end
