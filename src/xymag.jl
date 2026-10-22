# xymag.jl — magnetic anomaly modelling for the X,Y tool: the geomagnetic bar code over a profile, the
# synthetic reduced-to-the-pole profile, and the isochron age fit by correlation. A port of Mirone's
# ecran.m (push_magBar_CB, push_syntheticRTP_CB, slider_filter_CB, push_ageFit_CB, sanitize_shift,
# magmodel, obliquity_care, calcmag, fcalcmagpt). magmodel itself was adapted by ecran from MODMAG
# (V. Mendel, M. Munschy, D. Sauter, Computers & Geosciences 31 (2005) 589-597).
#
# The C++ side (65_xyplot.cpp, the "Geomagnetic bar code" strip) owns the GUI and the page state; it
# hands every request over as an op string on the Analysis callback ("magbar:…", "magsynth:…",
# "magfit:…", ":"-joined key=value pairs) and gets the result back through gmtvtk_xyplot_mag_set_*.

const _CK95_FILE = "Cande_Kent_95.dat"

# The polarity timescale: one row per NORMAL-polarity interval — chron name, age start, age end (Ma),
# and the anomaly label ("a" = unnamed). ecran reads it with strread('%s %f %f %s').
function _ck95_read(path::String=joinpath(_PKGROOT, "data", _CK95_FILE))
	chron = String[]; a0 = Float64[]; a1 = Float64[]; txt = String[]
	for ln in eachline(path)
		t = split(ln)
		(length(t) < 4 || startswith(t[1], "#")) && continue
		push!(chron, t[1]); push!(a0, parse(Float64, t[2])); push!(a1, parse(Float64, t[3])); push!(txt, t[4])
	end
	return chron, a0, a1, txt
end

# push_magBar_CB: the normal-polarity bricks between two ages (ascending), clipped at both ends.
# Returns (age_start, age_end, age_txt) of the bricks the bar shows.
function _magbar_bricks(age1::Real, age2::Real)
	lo, hi = minmax(Float64(age1), Float64(age2))
	_, a0, a1, txt = _ck95_read()
	id_ini = findfirst(>=(lo), a0)
	id_ini === nothing && error("the start age $lo Ma is older than the timescale ($(a1[end]) Ma)")
	k = findfirst(>(hi), a0)
	id_fim = k === nothing ? length(a0) : k - 1
	left = id_ini > 1 ? (a0[id_ini-1], a1[id_ini-1]) : (a0[1], a1[1])	# for a left-clipped brick
	s = a0[id_ini:id_fim]; e = a1[id_ini:id_fim]; t = txt[id_ini:id_fim]
	# Take care of end ages which certainly do not coincide with what was asked
	if (isempty(s) || s[1] > lo) && lo < left[2]			# we must create a clipped first brick
		pushfirst!(s, lo); pushfirst!(e, left[2]); pushfirst!(t, "a")
	end
	isempty(s) && error("no normal-polarity chron between $lo and $hi Ma")
	e[end] > hi && (e[end] = hi)						# clip the last brick
	return s, e, t
end

# MATLAB interp1(x, y, xi, 'linear'[, extrap]) for an ASCENDING x. Outside [x1,xn] -> `extrap`
# (NaN by default, as interp1 without an extrapolation value), or linear extrapolation when
# extrap === :extrap.
function _interp1(x::AbstractVector, y::AbstractVector, xi::Real, extrap=NaN)
	n = length(x)
	if xi < x[1] || xi > x[n]
		extrap === :extrap || return Float64(extrap)
		k = xi < x[1] ? 1 : n - 1
	else
		k = clamp(searchsortedlast(x, xi), 1, n - 1)
	end
	x[k+1] == x[k] && return Float64(y[k])
	return y[k] + (xi - x[k]) * (y[k+1] - y[k]) / (x[k+1] - x[k])
end

# obliquity_care: the obliquity between the profile direction and the opening direction, taking into
# account that the ridge may open aslant as well. OBLIQUITY is positive when DIR_PROFIL is outside the
# obliquity cone and negative otherwise.
function _obliquity_care(dir_spread::Float64, dir_profil::Float64, ridgeObliquity::Float64=0.0)
	obliquity = dir_profil - dir_spread
	if abs(obliquity) > 90
		temp1 = mod(obliquity, 90)
		temp2 = mod(obliquity, -90)
		obliquity = abs(temp1) <= abs(temp2) ? temp1 : temp2
		dir_spread = dir_profil - obliquity
	end
	obliquity = abs(obliquity)
	ridgeObliquity == 0 && return dir_spread, obliquity
	if dir_spread > 180			# the left side branch
		if dir_profil <= dir_spread || dir_profil >= dir_spread + 2ridgeObliquity
			obliquity = abs(dir_profil - dir_spread)
		else
			obliquity = -abs(ridgeObliquity - mod(dir_profil - dir_spread, ridgeObliquity))
		end
	else						# the right side branch
		if dir_profil >= dir_spread + 2ridgeObliquity || dir_profil <= dir_spread
			obliquity = abs(dir_profil - dir_spread)
		else
			obliquity = -abs(ridgeObliquity - mod(dir_profil - dir_spread, ridgeObliquity))
		end
	end
	return dir_spread, obliquity
end

# fcalcmagpt: the anomaly every edge of one magnetized polygon (polxx, polzz, closed) makes at every
# station. Returns the (x, z) components.
function _fcalcmagpt(stax::Vector{Float64}, staz::Vector{Float64}, polxx::Vector{Float64},
                     polzz::Vector{Float64}, dd1::Float64, dd2::Float64, dd3::Float64)
	ns = length(stax); nv = length(polxx)
	amxx = zeros(ns); amzz = zeros(ns)
	@inbounds for j in 1:ns
		sxx = 0.0; sxz = 0.0; szz = 0.0; szx = 0.0
		for k in 1:nv-1
			x1 = polxx[k] - stax[j];   z1 = polzz[k] - staz[j]
			x2 = polxx[k+1] - stax[j]; z2 = polzz[k+1] - staz[j]
			x1 == 0 && (x1 = 1e-11); z1 == 0 && (z1 = 1e-11)
			x2 == 0 && (x2 = 1e-11); z2 == 0 && (z2 = 1e-11)
			t12  = atan(z1, x1) - atan(z2, x2)
			z21  = z2 - z1;  x21 = x2 - x1
			xz12 = x1 * z2 - x2 * z1
			r21s = x21 * x21 + z21 * z21
			r21s == 0 && continue			# a zero-length edge (repeated vertex) adds nothing
			r1s  = x1^2 + z1^2;  r2s = x2^2 + z2^2
			rln  = 0.5 * log(r2s / r1s)
			p = (xz12 / r21s) * ((x1 * x21 - z1 * z21) / r1s - (x2 * x21 - z2 * z21) / r2s)
			q = (xz12 / r21s) * ((x1 * z21 + z1 * x21) / r1s - (x2 * z21 + z2 * x21) / r2s)
			f1 = (t12 * z21 - rln * x21) / r21s
			f2 = (t12 * x21 + rln * z21) / r21s
			sxx += p + z21 * f1			# dxx
			sxz += q - x21 * f1			# dxz
			szz += -p + x21 * f2		# dzz
			szx += q - z21 * f2			# dzx
		end
		amxx[j] = dd3 * (dd1 * sxz + dd2 * sxx)
		amzz[j] = dd3 * (dd1 * szz + dd2 * szx)
	end
	return amxx, amzz
end

# calcmag: the anomaly created by all the magnetized bodies.
function _calcmag(fInc, fDec, dir_spread, blockMag, stat_x, stat_z, PolXX, PolZZ)
	D2R = pi / 180
	c1 = sin(fInc * D2R)
	c2 = cos(fInc * D2R) * cos(dir_spread * D2R - fDec * D2R)
	d1 = sin(fInc * D2R)								# magInc = fInc
	d2 = cos(fInc * D2R) * cos((dir_spread + 90 - 90) * D2R - fDec * D2R)	# dir_anomag - 90
	anomax = zeros(length(stat_x)); anomaz = zeros(length(stat_x))
	for k in eachindex(PolXX)
		amx, amz = _fcalcmagpt(stat_x, stat_z, PolXX[k], PolZZ[k], d1, d2, 200 * blockMag[k])
		anomax .+= amx; anomaz .+= amz
	end
	return c2 .* anomax .+ c1 .* anomaz
end

"""
    _magmodel(barX1, dist, depth, fDec, fInc, speed, dir_spread, dir_profil, spreadObliqDir, contam)
        -> (anomaly, age_line, obliquity)

ecran's `magmodel`: the synthetic magnetic anomaly over a spreading model. `barX1` is the age (Ma) of
the first brick the bar code shows — the profile's first point sits on it. `dist` is the distance
along the profile in km (from 0), `depth` the depth of the top of the magnetized layer in km (one
value per point, NaNs re-invented by interpolation), `speed` the FULL spreading rate in cm/yr,
`contam` the Tissot & Patriat contamination factor. `age_line` is the age under every point.
"""
function _magmodel(barX1::Real, dist::Vector{Float64}, depth::Vector{Float64}, fDec::Real, fInc::Real,
                   speed::Real, dir_spread::Real, dir_profil::Real, spreadObliqDir::Real=0.0, contam::Real=1.0)
	zObs = 0.0; magAtAxe = 15.0; magFlatOfEachBlock = 5.0
	psubsi = 0.35				# coefficient of the thermal subsidence equation
	thickness = 0.5
	speed = speed * 10			# from full rate in cm/year to rate in km/Ma

	_, age_start, age_end, _ = _ck95_read()
	nAges = length(age_start)
	BA = zeros(2nAges, 2)
	BA[1:2:end-1, 1] = age_start;  BA[2:2:end, 1] = age_end
	BA[1:2:end-1, 2] = age_end
	BA[2:2:end-2, 2] = age_start[2:end]
	BA = BA[1:end-1, :]

	distAlongProfile = copy(dist)
	profileDepth = copy(depth)
	bad = isnan.(profileDepth)
	if any(bad)					# many files have gaps in bathymetry. Reinvent it
		gx = distAlongProfile[.!bad]; gz = profileDepth[.!bad]
		for i in findall(bad)
			profileDepth[i] = _interp1(gx, gz, distAlongProfile[i])
		end
	end

	nBlocks = size(BA, 1)
	nPts = length(distAlongProfile)
	stat_z = fill(zObs, nPts)

	# Age limits of the normal and inverse polarity bodies and their magnetization
	twiceNBlocks = 2nBlocks - 1
	blockAge = zeros(twiceNBlocks, 2)
	blockMag = zeros(twiceNBlocks)
	blockAge[1:nBlocks-1, 1] = -BA[nBlocks:-1:2, 2]
	blockAge[1:nBlocks-1, 2] = -BA[nBlocks:-1:2, 1]
	blockAge[nBlocks, :] = [-BA[1, 2], BA[1, 2]]
	blockAge[nBlocks+1:twiceNBlocks, :] = BA[2:nBlocks, :]
	blockMag[nBlocks] = magAtAxe
	blockMag[nBlocks-2:-2:1] .= magFlatOfEachBlock
	blockMag[nBlocks+2:2:twiceNBlocks] .= magFlatOfEachBlock
	blockMag[nBlocks-1:-2:1] .= -magFlatOfEachBlock
	blockMag[nBlocks+1:2:twiceNBlocks] .= -magFlatOfEachBlock

	# Magnetized bodies position in km
	polygon_x = zeros(4, twiceNBlocks)
	polygon_x[1, :] = blockAge[:, 1] .* speed ./ 2
	polygon_x[2, :] = blockAge[:, 2] .* speed ./ 2
	polygon_x[3, :] = polygon_x[2, :]
	polygon_x[4, :] = polygon_x[1, :]

	# The spreading direction has to be less than 90 deg away from the profile direction for the
	# distance to run in the right sense: the obliquity between the two.
	dir_spread, obliquity = _obliquity_care(Float64(dir_spread), Float64(dir_profil), Float64(spreadObliqDir))
	if obliquity != 0
		if obliquity > 0
			polygon_x ./= cos(obliquity * pi / 180)
		else			# rarer cases where ridge spreading is oblique and the profile lies inside the obliquity cone
			polygon_x .*= cos(obliquity * pi / 180)
		end
	end

	# Put the profile start on the first displayed brick: the closest block start to its left
	ind = findlast(>(0), barX1 .- blockAge[:, 1])
	ind === nothing && error("magmodel: the bar code starts before the timescale")
	f = (barX1 - blockAge[ind, 1]) / (blockAge[ind, 2] - blockAge[ind, 1])
	distAlongProfile .+= polygon_x[1, ind] + f * (polygon_x[2, ind] - polygon_x[1, ind])

	# Magnetized bodies depth
	polygon_z = zeros(4, twiceNBlocks)
	ind = findall(<(distAlongProfile[1]), polygon_x[1, :])		# block starts left of the profile start
	if !isempty(ind)
		maxlignm11 = maximum(ind)
		if polygon_x[1, maxlignm11] < 0
			polygon_z[1, ind] = profileDepth[1] .+ psubsi .* sqrt.(blockAge[maxlignm11, 1] .- blockAge[ind, 1])
		else
			polygon_z[1, ind] .= profileDepth[1]
		end
		polygon_z[4, ind] = polygon_z[1, ind] .+ thickness
	end

	ind = findall(<(distAlongProfile[1]), polygon_x[2, :])		# block ends left of the profile start
	if !isempty(ind)
		maxlignm12 = maximum(ind)
		if polygon_x[2, maxlignm12] < 0
			polygon_z[2, ind] = profileDepth[1] .+ psubsi .* sqrt.(blockAge[maxlignm12, 2] .- blockAge[ind, 2])
		else
			polygon_z[2, ind] .= profileDepth[1]
		end
		polygon_z[3, ind] = polygon_z[2, ind] .+ thickness
	else
		maxlignm12 = 0
	end

	ind = findall(>=(distAlongProfile[end]), polygon_x[1, :])	# block starts right of the profile end
	if !isempty(ind)
		minlignm21 = minimum(ind)
		if polygon_x[1, minlignm21] > 0
			polygon_z[1, ind] = profileDepth[end] .+ psubsi .* sqrt.(blockAge[ind, 1] .- blockAge[minlignm21, 1])
		else
			polygon_z[1, ind] .= profileDepth[end]
		end
		polygon_z[4, ind] = polygon_z[1, ind] .+ thickness
	else
		minlignm21 = twiceNBlocks + 1
	end

	ind = findall(>=(distAlongProfile[end]), polygon_x[2, :])	# block ends right of the profile end
	if !isempty(ind)
		minlignm22 = minimum(ind)
		if polygon_x[2, minlignm22] > 0
			polygon_z[2, ind] = profileDepth[end] .+ psubsi .* sqrt.(blockAge[ind, 2] .- blockAge[minlignm22, 2])
		else
			polygon_z[2, ind] .= profileDepth[end]
		end
		polygon_z[3, ind] = polygon_z[2, ind] .+ thickness
	end

	# The block limits that are inside the profile
	for (row, top) in ((1, 4), (2, 3))
		ind = findall(v -> v >= distAlongProfile[1] && v < distAlongProfile[end], polygon_x[row, :])
		for i in ind
			polygon_z[row, i] = _interp1(distAlongProfile, profileDepth, polygon_x[row, i])
			polygon_z[top, i] = polygon_z[row, i] + thickness
		end
	end

	PolXX = [vcat(polygon_x[:, i], polygon_x[1, i]) for i in 1:twiceNBlocks]
	PolZZ = [vcat(polygon_z[:, i], polygon_z[1, i]) for i in 1:twiceNBlocks]

	# Blocks under the profile follow the sea floor (and its +thickness base) point by point
	for i in maxlignm12+1:minlignm21-1
		ptdist = findall(d -> d > polygon_x[1, i] + 0.00001 && d < polygon_x[2, i] - 0.00001, distAlongProfile)
		isempty(ptdist) && continue
		tx1 = distAlongProfile[ptdist]; tz1 = profileDepth[ptdist]
		tx3 = vcat(polygon_x[1, i], tx1, polygon_x[2, i], polygon_x[3, i], reverse(tx1), polygon_x[4, i])
		tz3 = vcat(polygon_z[1, i], tz1, polygon_z[2, i], polygon_z[3, i], reverse(tz1 .+ thickness), polygon_z[4, i])
		PolXX[i] = vcat(tx3, polygon_x[1, i])
		PolZZ[i] = vcat(tz3, polygon_z[1, i])
	end

	# Re-position the magnetized bodies if the contamination coefficient is different from 1
	if contam != 1
		for i in 1:twiceNBlocks
			PolXX[i] .*= contam
		end
		distAlongProfile .*= contam
	end

	anoma = _calcmag(Float64(fInc), Float64(fDec), dir_spread, blockMag, distAlongProfile, stat_z, PolXX, PolZZ)
	age_line = distAlongProfile ./ speed .* 2
	return anoma, age_line, obliquity
end

"""
    _sanitize_shift(y_synt, y_ano, percent) -> Int

ecran's `sanitize_shift`: the index shift of `y_synt` at which it best correlates with `y_ano` (same
length). The maximum is searched within ±20 % of zero lag only, to avoid outlier maxima. Before each
correlation the synthetic is expanded, then shrunk, by 0..`percent`-1 per cent; the best of all wins.
"""
function _sanitize_shift(y_synt::Vector{Float64}, y_ano::Vector{Float64}, percent::Int)
	percent == 0 && (percent = 1)
	y_synt = reverse(y_synt)			# revert because we want CORR, not CONV
	n_pts = length(y_synt)
	x_half = isodd(n_pts) ? cld(n_pts, 2) : n_pts ÷ 2
	x = isodd(n_pts) ? collect(Float64, -(x_half-1):(x_half-1)) : collect(Float64, -(x_half-1):x_half)
	c = round(Int, n_pts * 0.20)		# how many points are 20% of the profile full length
	bond = n_pts-c:n_pts+c				# lags about Tau = 0 that count

	function best(yi)
		w = _conv_full(yi, y_ano)
		m = -Inf; im = 1
		for (k, b) in enumerate(bond)
			(1 <= b <= length(w)) || continue
			v = w[b]
			if v > m
				m = v; im = k
			end
		end
		return m, im
	end

	mimi = zeros(percent); ind_ = zeros(Int, percent)
	for k in 1:percent					# expand the synthetic curve up to PERCENT percent
		xi = x .* (1 - (k-1) * 1e-2)
		yi = [_interp1(x, y_synt, v) for v in xi]
		mimi[k], ind_[k] = best(yi)
	end
	max_expand, ind = findmax(mimi)
	bak = ind_[ind] + first(bond) - 1

	for k in 1:percent					# now shrink
		xi = x .* (1 + (k-1) * 1e-2)
		yi = [_interp1(x, y_synt, v, 0.0) for v in xi]	# extrapolate with 0 and use that knowledge below
		n = 1
		while n <= length(yi) && yi[n] == 0;  yi[n] = y_synt[1];   n += 1; end	# zeros -> first value (crude but ...)
		n = length(yi)
		while n >= 1 && yi[n] == 0;           yi[n] = y_synt[end]; n -= 1; end	# zeros -> last value
		mimi[k], ind_[k] = best(yi)
	end
	max_shrink, ind = findmax(mimi)

	lag = max_expand > max_shrink ? bak : ind_[ind] + first(bond) - 1
	return lag - n_pts
end

# conv(a, b) 'full'.
function _conv_full(a::AbstractVector{Float64}, b::AbstractVector{Float64})
	na = length(a); nb = length(b)
	w = zeros(na + nb - 1)
	@inbounds for i in 1:na, j in 1:nb
		w[i+j-1] += a[i] * b[j]
	end
	return w
end

# Distance unit code (the Parameters dialog's combo) -> (scale to km, ecran's distSpeedFact).
_mag_unit(u::Int) = u == 0 ? (1e-3, 1e-4) : u == 2 ? (1.852, 1.852e-1) : (1.0, 1e-1)

_kvnum(d, k, def=NaN) = (v = get(d, k, ""); isempty(v) ? Float64(def) : parse(Float64, v))

# The current page's age-under-every-point of its synthetic (gmtvtk_xyplot_mag_ageline).
function _xy_mag_ageline(plot::Ptr{Cvoid})
	n = ccall(_fn(:gmtvtk_xyplot_mag_ageline), Cint, (Ptr{Cvoid}, Ptr{Float64}, Cint), plot, C_NULL, 0)
	n <= 0 && return Float64[]
	buf = Vector{Float64}(undef, n)
	ccall(_fn(:gmtvtk_xyplot_mag_ageline), Cint, (Ptr{Cvoid}, Ptr{Float64}, Cint), plot, buf, n)
	return buf
end

# Undo the C side's op-string escaping of a free-text value (xyOpEsc: '%' -> %25, ':' -> %3A).
_mag_unesc(v::AbstractString) = replace(replace(v, "%3A" => ":"), "%25" => "%")

# The track under series `sel` of the current page: (x, y, geog) or nothing (gmtvtk_xyplot_series_track).
function _xy_series_track(plot::Ptr{Cvoid}, sel::Int)
	g = Ref{Cint}(-1)
	n = ccall(_fn(:gmtvtk_xyplot_series_track), Cint, (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint, Ref{Cint}),
	          plot, sel, C_NULL, C_NULL, 0, g)
	n <= 0 && return nothing
	x = Vector{Float64}(undef, n); y = Vector{Float64}(undef, n)
	ccall(_fn(:gmtvtk_xyplot_series_track), Cint, (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint, Ref{Cint}),
	      plot, sel, x, y, n, g)
	return (x, y, g[] == 1)
end

# The profile's azimuth, first point to last (push_syntheticRTP_CB's azimuth_geo), and its distance
# along the track (ecran's get_distances: geodesic when geographic) — both through THE app's distance /
# azimuth function, `_seg_dist_azim` (measure.jl): km on a lon/lat track, data units on a cartesian one.
# `datype` is the Preferences "Dist/Azim type" the C side sends — the same one the 3-D profile's own
# distance axis and the ruler are measured with, so the model and the panel agree on the track's length.
_mag_track_azimuth(x, y, geog::Bool; datype::AbstractString="Ellipsoidal") =
	_seg_dist_azim([x[1], x[end]], [y[1], y[end]], geog; datype)[2][2]
_mag_track_dist(x, y, geog::Bool; datype::AbstractString="Ellipsoidal") =
	cumsum(_seg_dist_azim(collect(Float64, x), collect(Float64, y), geog; datype)[1])

# ecran's BAT: the bathymetry grid sampled at the track points, as depth in km (the grid is read once
# per file and kept, ecran's `batTrack`).
const _MAG_BAT = Ref{Tuple{String,Float64,Any}}(("", 0.0, nothing))
function _mag_bat_depth(path::String, x, y)
	isfile(path) || error("Bathymetry grid $path does not exist")
	mt = mtime(path)
	if _MAG_BAT[][1] != path || _MAG_BAT[][2] != mt
		G = _gmtread_trb(path)
		G isa GMTgrid || error("$path is not a grid")
		_MAG_BAT[] = (path, mt, G)
	end
	D = GMT.grdtrack(_MAG_BAT[][3], hcat(x, y); no_skip = true)
	M = D isa AbstractVector ? D[1].data : D.data
	return abs.(Float64.(M[:, end])) ./ 1000				# need Z in km
end

# "Create Mag Bar" (push_magBar_CB): the bricks between the two ages -> the page's bar code. `stretch`
# (ecran's stretchMagBar) expands / shrinks them to simulate an oblique profile and/or spreading.
function _xy_magbar(plot::Ptr{Cvoid}, d::Dict{String,String})
	a1 = _kvnum(d, "aS"); a2 = _kvnum(d, "aE")
	(isnan(a1) || isnan(a2)) && error("Take a second look to what you are asking for. Wrong ages")
	s, e, t = _magbar_bricks(a1, a2)
	st = _kvnum(d, "stretch", 1)
	if st > 0 && st != 1
		s .*= st; e .*= st
	end
	ccall(_fn(:gmtvtk_xyplot_mag_set_bar), Cvoid,
	      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cstring, Cdouble, Cdouble),
	      plot, s, e, length(s), join(t, '\x1f'), a1, a2)
	_xy_log(plot, "Mag bar: $(length(s)) normal-polarity bricks between $(min(a1,a2)) and $(max(a1,a2)) Ma")
	return
end

# "Synthetic RTP" (push_syntheticRTP_CB) and the contamination slider (slider_filter_CB) — the SAME
# model run, the slider only changing `contam`. `sel` is the measured anomaly series.
function _xy_magsynth(plot::Ptr{Cvoid}, d::Dict{String,String}, sel::Int)
	x, _ = _xy_get_series(plot, sel)
	length(x) < 2 && error("Synthetic RTP: the measured profile has fewer than 2 points")
	aS = _kvnum(d, "aS"); aE = _kvnum(d, "aE"); x0bar = _kvnum(d, "x0")
	rev = get(d, "rev", "0") == "1"
	scale_x, distSpeedFact = _mag_unit(round(Int, _kvnum(d, "unit", 1)))
	dist = abs.(x .- x[1])
	pdir = _kvnum(d, "pdir", 0); sdir = _kvnum(d, "sdir")
	depth = fill(_kvnum(d, "depth", 2.5), length(x))
	trk = _xy_series_track(plot, sel)
	(trk !== nothing && length(trk[1]) != length(x)) && (trk = nothing)
	datype = _mag_unesc(get(d, "dtype", ""))
	isempty(datype) && (datype = "Ellipsoidal")
	if trk !== nothing && trk[3]
		# A lon/lat track: the distance is measured ON it, geodesically, in km (ecran's get_distances
		# with the 'k' unit) — not read off the curve's x, whatever unit that is in.
		dist = _mag_track_dist(trk[1], trk[2], true; datype)
		scale_x, distSpeedFact = _mag_unit(1)
	end
	speed = _kvnum(d, "speed", 0)
	speed > 0 || (speed = 2 * dist[end] / abs(aE - aS) * distSpeedFact)	# full rate in cm/yr
	if trk !== nothing
		tx, ty, geog = trk
		pdir = _mag_track_azimuth(tx, ty, geog; datype)	# the profile's own direction
		bat = _mag_unesc(get(d, "bat", ""))
		if !isempty(bat)
			zz = _mag_bat_depth(bat, tx, ty)
			if all(v -> isnan(v) || v == 0, zz)
				_xy_log(plot, "Bat grid used but profile is probably out of its limits. Ignoring bat grid track."; err=true)
			else
				depth = zz
			end
		end
	elseif !isempty(get(d, "bat", ""))
		error("The bathymetry grid needs the profile's track: draw the profile in the 3-D viewer and open it here")
	end
	isnan(sdir) && (sdir = pdir)
	contam = _kvnum(d, "contam", 1)
	(0.5 <= contam <= 1) || (contam = 1.0)
	anom, age_line, _ = _magmodel(x0bar, dist .* scale_x, depth, _kvnum(d, "dec", 0), _kvnum(d, "inc", 90),
	                              speed, sdir, pdir, 0.0, contam)
	xs = rev ? reverse(x) : x
	ccall(_fn(:gmtvtk_xyplot_mag_set_synth), Cvoid,
	      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Ptr{Float64}, Cint, Cdouble),
	      plot, xs, anom, age_line, length(xs), speed)
	_xy_log(plot, "Synthetic RTP: full rate $(round(speed; sigdigits=4)) cm/yr, profile azimuth " *
	              "$(round(pdir; digits=1))°$(trk === nothing ? "" : " (from the track)"), layer depth " *
	              "$(all(==(depth[1]), depth) ? "$(depth[1]) km" : "from the bathymetry grid"), " *
	              "contamination $(round(contam; digits=3))")
	return
end

# "Fit" (push_ageFit_CB): slide a chunk of the synthetic centred on the chosen age along the measured
# profile and keep the shift of maximum correlation (sanitize_shift). `sel` is the measured series.
function _xy_magfit(plot::Ptr{Cvoid}, d::Dict{String,String}, sel::Int)
	syn = round(Int, _kvnum(d, "syn", -1))
	syn < 0 && error("Fit: there is no synthetic profile on this page")
	x, y = _xy_get_series(plot, syn)
	_, y_ano = _xy_get_series(plot, sel)
	age_line = _xy_mag_ageline(plot)
	(length(age_line) == length(x) == length(y_ano)) ||
		error("Fit: the synthetic and the measured profile do not have the same points")
	xx = _kvnum(d, "age"); agePad = _kvnum(d, "pad", 1.5); ageStretch = round(Int, _kvnum(d, "stretch", 15))
	isnan(xx) && error("Pelease, fit what? Your anniversary? How many Ma?")
	rev = get(d, "rev", "0") == "1"
	star = round(Int, _kvnum(d, "star", 0)) + 1			# 0-based (C) -> 1-based
	guess = round(Int, _kvnum(d, "guess", -1)) + 1		# 0 = no guess marker
	if rev		# profile drawn from old to new: some vectors are reversed, which makes a bit of a mess
		age_line = reverse(age_line); x = reverse(x); y = reverse(y)
	end

	# A chunk of the synthetic centred on the age marker
	ind_a = argmin(abs.(age_line .- (xx - agePad)))
	ind_b = argmin(abs.(age_line .- (xx + agePad)))
	ind_a = max(1, ind_a); ind_b = min(length(age_line), ind_b)
	rev && ((ind_a, ind_b) = (ind_b, ind_a))
	ind_a > ind_b && ((ind_a, ind_b) = (ind_b, ind_a))
	yc = y[ind_a:ind_b]

	# A first approximate solution given by the user (the "Guide me" marker)
	guess_shift = 0
	if guess > 0
		guess_shift = guess - star
		ind_a += guess_shift; ind_b += guess_shift
		(ind_a < 1 || ind_b > length(x)) &&
			error("Can't continue. Guess solution implies a data shortage of points at one of the limits.")
	end

	ya = y_ano[ind_a:ind_b]			# the corresponding chunk of the measured anomaly
	bad = isnan.(ya)
	if any(bad)
		xc = x[ind_a:ind_b]; gx = xc[.!bad]; gy = ya[.!bad]
		p = sortperm(gx)
		for i in findall(bad)
			ya[i] = _interp1(gx[p], gy[p], xc[i], :extrap)
		end
	end

	# Normalize and remove the mean
	ya = (ya .- minimum(ya)) ./ (maximum(ya) - minimum(ya));  ya .-= sum(ya) / length(ya)
	yn = (yc .- minimum(yc)) ./ (maximum(yc) - minimum(yc));  yn .-= sum(yn) / length(yn)

	shift = _sanitize_shift(yn, ya, ageStretch)

	(ind_a + shift < 1 || ind_b + shift > length(x)) &&
		error("Guess work by convolution failed (index out of bounds). Try increase the isochron pad limits")
	xf = x[ind_a+shift:ind_b+shift]			# new abscissae after the correlation fit

	ind_ageMarker = star + guess_shift
	rev && (ind_ageMarker = length(age_line) - ind_ageMarker + 1)
	delta_shift = ind_ageMarker - ind_a + 1		# the age marker moves by this after the fit
	(1 <= delta_shift <= length(xf)) ||
		error("Fit: the age marker falls outside the fitted chunk (increase the isochron pad)")
	ccall(_fn(:gmtvtk_xyplot_mag_set_fit), Cvoid,
	      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cdouble, Cstring),
	      plot, xf, yc, length(xf), xf[delta_shift], "Fit $(xx) Ma")
	_xy_log(plot, "Fit: age $(xx) Ma -> x = $(round(xf[delta_shift]; sigdigits=8)) (shift $shift pts)")
	return
end

# A request of the X,Y window failed: the message goes back to the C door (xyRequest), which shows it
# in a box in front of the user, and to the window's Errors tab.
function _xy_request_failed(plot::Ptr{Cvoid}, msg::AbstractString)
	_xy_log(plot, msg; err=true)
	ccall(_fn(:gmtvtk_xyplot_request_failed), Cvoid, (Ptr{Cvoid}, Cstring), plot, String(msg))
	return
end

# The one door every "mag…" op of the Analysis callback goes through.
function _xy_mag(plot::Ptr{Cvoid}, op::String, sel::Int)
	i = findfirst(==(':'), op)
	cmd = i === nothing ? op : op[1:prevind(op, i)]
	d = i === nothing ? Dict{String,String}() : _parse_kv(op[nextind(op, i):end])
	try
		if cmd == "magbar"
			_xy_magbar(plot, d)
		elseif cmd == "magsynth"
			_xy_magsynth(plot, d, sel)
		elseif cmd == "magfit"
			_xy_magfit(plot, d, sel)
		else
			error("unknown magnetic op '$cmd'")
		end
	catch e
		_dbg("xy-mag", op, "ERR", sprint(showerror, e))
		_xy_request_failed(plot, e isa ErrorException ? e.msg : sprint(showerror, e))
	end
	return
end
