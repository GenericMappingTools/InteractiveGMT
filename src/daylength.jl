# daylength.jl — T & L > "Day length & axial tilt": how the tilt of Earth's rotation axis sets the
# length of the day through a year. The dialog (deps/src/74_daylength_gmt.cpp) shows it; the movie
# maker (examples/daylength_anim.jl) encodes it.
#
# Modelled on Engaging-Data's "Visualizing the Variation in Sunlight by Latitude and Time of Year"
# (https://engaging-data.com/?p=2906): a globe on the left, the hours of daylight per latitude band on
# the right, plus a whole-year latitude x day-of-year map of the day length with a cursor on the day.
#
# TWO LAYERS, so a day costs nothing. The BASE -- globe, coastlines, graticule, circles, axis, Sun, the
# bar panel's frame, the year map with its contours, the overhead-Sun curve, the colour bar -- is the
# same on every day: GMT draws it once per tilt and view (`daylength_base`). Everything that moves with
# the day -- night, terminator, overhead-Sun spot, bars and their values, the year map's cursor, the title
# -- is a small DISPLAY LIST in page centimetres (`daylength_overlay`), pure arithmetic. The dialog paints
# that list over the base picture with Qt; the movie draws the very same list with GMT
# (`_dl_render_overlay!`). One geometry, two painters -- and no GMT or Ghostscript run per day, which is
# where the second a frame used to cost went (0.53 s of it is Ghostscript's start-up alone).
#
# THE GLOBE is an orthographic view with the Sun on the right: from the side (90 degrees away from the
# Sun, axis upright), or looking down on the North or the South pole. From the side the terminator is a
# straight diameter leaning by the Sun's declination -- upright at the equinoxes, 23.44 deg at the
# solstices; from a pole it is the curve that leaves the polar cap wholly lit or wholly dark. Night is
# computed exactly for any view (_dl_night). Polar circles blue, tropics red. Coastlines are GMT's crude
# resolution: the globe is a few cm across.
#
# THE DECLINATION is GMT's own (`solar -C -I`, the Sun's latitude at noon UTC of each day). With `tilt`
# other than Earth's, that real declination is rescaled to what a planet with that obliquity would have
# at the same point of the orbit: delta = asind(sind(delta_earth) * sind(tilt) / sind(23.44)).
#
# THE DAY LENGTH is the geometric sunrise equation, cos(H0) = -tan(lat) tan(delta), L = 2 H0 / 15 h —
# the Sun's centre on a flat horizon, no refraction, so the equator gets exactly 12 h. Clamped to
# 0 h (polar night) and 24 h (midnight sun).

using GMT: Dates, @sprintf

const EARTH_TILT = 23.44
const _DL_GLOBE  = 9.0                 # globe diameter, cm

# THE PAGE, cm from its lower-left corner. The base paints it white first, so the cropped picture is
# exactly this rectangle and a page centimetre maps to a pixel by one scale (picture width / _DL_W).
const _DL_W, _DL_H = 26.5, 20.6
const _DL_GX, _DL_GY = 2.5, 9.0                                  # globe panel origin
const _DL_BX, _DL_BY, _DL_BW = _DL_GX + _DL_GLOBE + 3.8, 9.0, 8.5  # bar panel (height = globe)
const _DL_YX, _DL_YY, _DL_YW, _DL_YH = 2.5, 0.8, 21.5, 6.0         # year map

# The Sun's latitude at noon UTC of day-of-year `doy`, from GMT's solar ephemeris.
function sun_declination(year::Int, doy::Int)::Float64
	d = Dates.Date(year) + Dates.Day(doy - 1)
	D = gmt("solar -C -I+d$(d)T12:00")
	return D.data[2]
end

# The declination a planet with axial tilt `tilt` would have at the same point of the orbit.
_dl_scale_decl(dec::Float64, tilt::Float64) =
	tilt == EARTH_TILT ? dec : asind(clamp(sind(dec) * sind(tilt) / sind(EARTH_TILT), -1.0, 1.0))

"""
    day_length(lat, dec) -> hours

Hours of daylight at latitude `lat` when the Sun's declination is `dec` (geometric sunrise, no
refraction), 0 in polar night and 24 under the midnight sun.
"""
function day_length(lat::Float64, dec::Float64)::Float64
	c = -tand(lat) * tand(dec)
	c >= 1.0 && return 0.0
	c <= -1.0 && return 24.0
	return 2acosd(c) / 15
end

# Small circle at latitude `lat`, dense enough that `plot` follows the parallel, not a great circle.
_dl_parallel(lat::Float64) = hcat(collect(-180.0:2.0:180.0), fill(lat, 181))

# The three views of the globe (the dialog's projection box, in its order): orthographic centres
# (lon0, lat0). Every view is centred on Greenwich (lon0 = 0), and the Sun stands over lon _DL_SUNLON
# = 90 E (06:00 UTC), which puts it on the right of the picture in all three.
const _DL_VIEWS = ((0.0, 0.0), (0.0, 90.0), (0.0, -90.0))   # side, North pole, South pole
const _DL_SUNLON = 90.0

_dl_unit(lon::Float64, lat::Float64) = [cosd(lat) * cosd(lon), cosd(lat) * sind(lon), sind(lat)]
_dl_dot(a::Vector{Float64}, b::Vector{Float64}) = a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
_dl_cross(a::Vector{Float64}, b::Vector{Float64}) =
	[a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1]]
_dl_norm(a::Vector{Float64}) = a ./ sqrt(_dl_dot(a, a))

# Orthographic projection centred on (lon0, lat0), as GMT's -JG draws it: view direction v, east and
# north at the centre as the paper's x and y. A point p of the unit sphere lands at R*(1 + p.ex, 1 + p.ey)
# in the globe panel.
function _dl_view(lon0::Float64, lat0::Float64)
	v  = _dl_unit(lon0, lat0)
	ex = [-sind(lon0), cosd(lon0), 0.0]
	ey = [-sind(lat0) * cosd(lon0), -sind(lat0) * sind(lon0), cosd(lat0)]
	return v, ex, ey
end
_dl_paper(p::Vector{Float64}, ex::Vector{Float64}, ey::Vector{Float64}) =
	[(_DL_GLOBE / 2) * (1 + _dl_dot(p, ex))  (_DL_GLOBE / 2) * (1 + _dl_dot(p, ey))]

# The night seen from (lon0, lat0) with the Sun over (lon _DL_SUNLON, lat dec), in globe-panel cm: the night
# polygon (the visible arc of the terminator, then the night arc of the globe's rim) and that terminator
# arc. Both arcs run between the two points where the terminator meets the rim, +-(v x s): the terminator
# through its point nearest the viewer, the rim through its point farthest from the Sun. Exact for any
# view, so the side view's straight diameter and a pole view's curve come out of the same lines.
function _dl_night(dec::Float64, lon0::Float64, lat0::Float64)
	v, ex, ey = _dl_view(lon0, lat0)
	s = _dl_unit(_DL_SUNLON, dec)
	c = _dl_cross(v, s)
	if _dl_dot(c, c) < 1e-18                     # looking straight down the Sun line, or away from it
		_dl_dot(v, s) > 0 && return zeros(0, 2), zeros(0, 2)
		return vcat([_dl_paper(cosd(t) .* ex .+ sind(t) .* ey, ex, ey) for t in 0.0:2.0:360.0]...), zeros(0, 2)
	end
	a  = _dl_norm(c)
	mt = _dl_norm(v .- _dl_dot(v, s) .* s)
	mr = _dl_norm(-(s .- _dl_dot(s, v) .* v))
	term = vcat([_dl_paper(cosd(t) .* a .+ sind(t) .* mt, ex, ey) for t in 0.0:1.0:180.0]...)
	rim  = vcat([_dl_paper(-cosd(t) .* a .+ sind(t) .* mr, ex, ey) for t in 0.0:1.0:180.0]...)
	return vcat(term, rim), term
end

"""
    daylength_year(; year=2026, tilt=23.44, dlat=0.5) -> (decl, G)

Declination for every day of `year` and the day-length grid over day-of-year (x) x latitude (y).
"""
function daylength_year(; year::Int=2026, tilt::Float64=EARTH_TILT, dlat::Float64=0.5)
	ndays = Dates.daysinyear(year)
	decl = [_dl_scale_decl(sun_declination(year, n), tilt) for n in 1:ndays]
	lats = collect(-90.0:dlat:90.0)
	L = [day_length(φ, δ) for φ in lats, δ in decl]                 # (nlat, ndays): rows = y
	G = mat2grid(Float32.(L), x=collect(1.0:ndays), y=lats)
	return decl, G
end

daylength_cpt() = makecpt(cmap=:lajolla, range=(0, 24, 1))           # dark = night, pale = day

const _DL_MONTH_DOY = [1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335]
const _DL_MONTH_TXT = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

# RGB (0-255) of value `v` in `cpt`.
function _dl_cpt_rgb(cpt::GMTcpt, v::Float64)::NTuple{3,Float64}
	k = clamp(searchsortedlast(cpt.range[:, 1], v), 1, size(cpt.colormap, 1))
	return (round(255 * cpt.colormap[k, 1]), round(255 * cpt.colormap[k, 2]), round(255 * cpt.colormap[k, 3]))
end

"""
    daylength_base(; tilt=23.44, decl, G, cpt, view=0)

Draw, with GMT, everything of the picture that does not change with the day (see the file's banner).
Leaves the figure open, its origin at the year map's lower-left corner. `view`: 0 orthographic (side),
1 Polar (North), 2 Polar (South).
"""
function daylength_base(; tilt::Float64=EARTH_TILT, decl::Vector{Float64}, G::GMTgrid, cpt::GMTcpt, view::Int=0)
	ndays = length(decl)
	S = _DL_GLOBE
	R = S / 2
	lon0, lat0 = _DL_VIEWS[clamp(view, 0, 2) + 1]
	# The page, white: it fixes the crop (see _DL_W).
	GMT.plot([0 0; _DL_W 0; _DL_W _DL_H; 0 _DL_H; 0 0], region=(0, _DL_W, 0, _DL_H), proj=:linear,
	     figsize=(_DL_W, _DL_H), fill=:white, frame=:none, xshift=0.0, yshift=0.0)

	# ---- Globe: the chosen view, Sun from the right ---------------------------------------------
	coast!(region=:global, proj=(name=:ortho, center=(lon0, lat0)), figsize=S, land="#d2bc8c",
	       water="#a9cfe6", shore=(0.25, "#7a6a4a"), res=:crude, frame=(axes=:none, grid=30),
	       par=(MAP_GRID_PEN_PRIMARY="0.25p,gray50", MAP_FRAME_PEN="1p"), xshift=_DL_GX, yshift=_DL_GY)
	# One call per pen: every GMT call costs a module start.
	GMT.plot!([mat2ds(_dl_parallel(φ)) for φ in (tilt, -tilt)], pen=(0.9, :red, :dash))
	GMT.plot!([mat2ds(_dl_parallel(φ)) for φ in (90 - tilt, tilt - 90)], pen=(0.9, :blue, :dash))
	GMT.plot!(_dl_parallel(0.0), pen=(0.8, :black))
	if lat0 == 0.0
		# Rotation axis, upright, poking out of both poles.
		GMT.plot!([R -0.7; R S + 0.7], region=(0, S, 0, S), proj=:linear, figsize=(S, S), pen=(1.5, :black),
		      arrow=(len=0.35, stop=true, shape=0.5), fill=:black, noclip=true)
	else
		# Looking down the axis: it is the centre of the picture.
		GMT.plot!([R R], region=(0, S, 0, S), proj=:linear, figsize=(S, S), marker=:circle, ms=0.3, fill=:black,
		      ml=(0.5, :white))
	end
	# The Sun.
	GMT.plot!([S + 1.6 R], marker=:circle, ms=1.0, fill=:gold, ml=(0.6, :orange), noclip=true)

	# ---- Bar panel: frame only (the bars are the overlay's) ----------------------------------------
	basemap!(region=(0, 24, -90, 90), proj=:linear, figsize=(_DL_BW, S), xshift=_DL_BX - _DL_GX,
	         frame=(axes=:WSen, title="Hours of daylight"), xaxis=(annot=6, ticks=1, grid=6, label="hours"),
	         yaxis=(annot=30, ticks=10, grid=30, label="Latitude"),
	         par=(MAP_GRID_PEN_PRIMARY="0.25p,gray75", FONT_TITLE="14p,Helvetica-Bold"))

	# ---- Whole year: latitude x day-of-year ---------------------------------------------------------
	grdimage!(G, cmap=cpt, region=(1, ndays, -90, 90), proj=:linear, figsize=(_DL_YW, _DL_YH),
	          xshift=_DL_YX - _DL_BX, yshift=_DL_YY - _DL_BY,
	          frame=(axes=:WSen, title="Hours of daylight over the year  (blue: latitude of the overhead Sun)"),
	          xaxis=(custom=(pos=_DL_MONTH_DOY, type_=fill("ag", 12), label=_DL_MONTH_TXT),),
	          yaxis=(annot=30, ticks=10, label="Latitude"), par=(FONT_TITLE="14p,Helvetica-Bold",))
	grdcontour!(G, cont=2, annot=(int=6, labels=(font=(7, "Helvetica"),)), pen=(0.3, "gray25"))
	GMT.plot!(hcat(collect(1.0:ndays), decl), pen=(1.5, :blue))
	colorbar!(pos=(anchor=:MR, offset=(0.4, 0), length=_DL_YH, width=0.35), cmap=cpt,
	          frame=(annot=6, ticks=1, ylabel="h"))
	return nothing
end

# Display-list records (page cm, colours 0-255). Every record starts with its kind:
#   1 filled polygon:  1, n, r, g, b, opacity, x1, y1, ... xn, yn
#   2 polyline:        2, n, r, g, b, width_pt, dashed(0/1), x1, y1, ...
#   3 disc:            3, x, y, radius_cm, r, g, b, outline_pt
#   4 number "%.1f":   4, x, y, value, justify (-1 left end at x, +1 right end at x), size_pt
#   5 title:           5, x, y, size_pt, doy, declination, tilt
#   6 drag handle:     6, x, y, radius_cm, x_day1, x_lastday, ndays   (not drawn; the dialog's disc drag)
_dl_put_poly!(L, kind, xy, rgb, extra...) =
	(append!(L, (kind, size(xy, 1), rgb..., extra...)); for i in axes(xy, 1); push!(L, xy[i, 1], xy[i, 2]); end)

"""
    daylength_overlay(doy; tilt=23.44, decl, cpt, view=0) -> Vector{Float64}

Everything of day `doy`'s picture that changes with the day, as a display list in page cm (record
layout above), led by the page size [W, H]. Pure arithmetic: no GMT call.
"""
function daylength_overlay(doy::Int; tilt::Float64=EARTH_TILT, decl::Vector{Float64}, cpt::GMTcpt, view::Int=0)
	ndays = length(decl)
	1 <= doy <= ndays || error("day $doy is outside the year (1-$ndays)")
	δ = decl[doy]
	S = _DL_GLOBE
	lon0, lat0 = _DL_VIEWS[clamp(view, 0, 2) + 1]
	L = Float64[_DL_W, _DL_H]
	# Globe: night, terminator, the overhead Sun when it is on the visible side.
	night, term = _dl_night(δ, lon0, lat0)
	isempty(night) || _dl_put_poly!(L, 1, night .+ [_DL_GX _DL_GY], (0.0, 0.0, 30.0), 0.55)
	isempty(term)  || _dl_put_poly!(L, 2, term .+ [_DL_GX _DL_GY], (255.0, 165.0, 0.0), 1.5, 0.0)
	v, ex, ey = _dl_view(lon0, lat0)
	s = _dl_unit(_DL_SUNLON, δ)
	if _dl_dot(s, v) >= -1e-9
		p = _dl_paper(s, ex, ey)
		append!(L, (3, p[1] + _DL_GX, p[2] + _DL_GY, 0.22, 255.0, 215.0, 0.0, 0.6))
	end
	# Bars: hours of daylight per 5-degree band, coloured like the year map, and their values.
	bx(h) = _DL_BX + _DL_BW * h / 24
	by(φ) = _DL_BY + S * (φ + 90) / 180
	for φ in -87.5:5.0:87.5
		h = day_length(φ, δ)
		if h > 0
			box = [bx(0) by(φ - 2.2); bx(h) by(φ - 2.2); bx(h) by(φ + 2.2); bx(0) by(φ + 2.2); bx(0) by(φ - 2.2)]
			_dl_put_poly!(L, 1, box, _dl_cpt_rgb(cpt, h), 1.0)
			_dl_put_poly!(L, 2, box, (77.0, 77.0, 77.0), 0.3, 0.0)
		end
		inside = h > 19
		append!(L, (4, inside ? bx(h) - 0.08 : bx(h) + 0.08, by(φ), h, inside ? 1.0 : -1.0, 6.5))
	end
	_dl_put_poly!(L, 2, [bx(12) by(-90); bx(12) by(90)], (51.0, 51.0, 51.0), 0.8, 1.0)
	# Year map: the day's cursor and the overhead Sun on its curve.
	cx = _DL_YX + _DL_YW * (doy - 1) / (ndays - 1)
	_dl_put_poly!(L, 2, [cx _DL_YY; cx _DL_YY + _DL_YH], (255.0, 255.0, 255.0), 1.5, 0.0)
	append!(L, (3, cx, _DL_YY + _DL_YH * (δ + 90) / 180, 0.2, 255.0, 215.0, 0.0, 0.6))
	# The dialog drags that disc along the year: where it is, and the day axis it moves on.
	append!(L, (6, cx, _DL_YY + _DL_YH * (δ + 90) / 180, 0.2, _DL_YX, _DL_YX + _DL_YW, ndays))
	# Title.
	append!(L, (5, _DL_GX + S / 2 + 4.0, _DL_GY + S + 1.3, 15.0, doy, δ, tilt))
	return L
end

# The title's text, as both painters write it.
_dl_title(year::Int, doy::Int, δ::Float64, tilt::Float64) =
	@sprintf("%s  ·  Sun's declination %+5.1f°  ·  axial tilt %.2f°",
	         Dates.format(Dates.Date(year) + Dates.Day(doy - 1), "d U"), δ, tilt)

# Draw a display list with GMT, over a figure left open by daylength_base (origin at the year map).
function _dl_render_overlay!(L::Vector{Float64}, year::Int)
	W, H = L[1], L[2]
	page = (region=(0, W, 0, H), proj=:linear, figsize=(W, H))
	rgb(r, g, b) = @sprintf("%d/%d/%d", r, g, b)
	first = true
	frame!() = first ? (first = false; (; page..., xshift=-_DL_YX, yshift=-_DL_YY)) : page
	k = 3
	while k <= length(L)
		kind = Int(L[k])
		if kind == 1 || kind == 2
			n = Int(L[k + 1])
			r, g, b = L[k + 2], L[k + 3], L[k + 4]
			a, w = L[k + 5], L[k + 5]
			dashed = kind == 2 && L[k + 6] != 0
			o = k + (kind == 1 ? 6 : 7)
			xy = permutedims(reshape(L[o:o + 2n - 1], 2, n))
			if kind == 1
				GMT.plot!(xy; frame!()..., fill=rgb(r, g, b) * @sprintf("@%d", round(Int, 100 * (1 - a))), close=true)
			else
				GMT.plot!(xy; frame!()..., pen=dashed ? (w, rgb(r, g, b), :dash) : (w, rgb(r, g, b)), noclip=true)
			end
			k = o + 2n
		elseif kind == 3
			GMT.plot!([L[k + 1] L[k + 2]]; frame!()..., marker=:circle, ms=2 * L[k + 3], fill=rgb(L[k + 4], L[k + 5], L[k + 6]),
			      ml=(L[k + 7], :black), noclip=true)
			k += 8
		elseif kind == 4
			x, y, val, just, sz = L[k + 1], L[k + 2], L[k + 3], L[k + 4], L[k + 5]
			text!(text=@sprintf("%.1f", val), x=x, y=y; frame!()..., font=(sz, "Helvetica"),
			      justify=just > 0 ? :MR : :ML, noclip=true)
			k += 6
		elseif kind == 5
			x, y, sz, doy, δ, tilt = L[k + 1], L[k + 2], L[k + 3], Int(L[k + 4]), L[k + 5], L[k + 6]
			t = replace(_dl_title(year, doy, δ, tilt), "·" => "\\267", "°" => "\\232")
			text!(text=t, x=x, y=y; frame!()..., font=(sz, "Helvetica-Bold"), noclip=true)
			k += 7
		elseif kind == 6                              # the dialog's drag handle: nothing to draw
			k += 7
		else
			error("unknown display-list record $kind")
		end
	end
	return nothing
end

"""
    daylength_frame(doy; year=2026, tilt=23.44, decl, G, cpt, savefig="", show=false, dpi=150, view=0)

Draw the whole picture of day-of-year `doy` with GMT: the base, then the day's overlay. `decl`/`G`/`cpt`
are the year's precomputed data (`daylength_year`, `daylength_cpt`); they are computed when not given.
"""
function daylength_frame(doy::Int; year::Int=2026, tilt::Float64=EARTH_TILT, decl=nothing, G=nothing,
                         cpt=nothing, savefig::String="", show::Bool=false, dpi::Int=150, view::Int=0)
	(decl === nothing || G === nothing) && ((decl, G) = daylength_year(; year=year, tilt=tilt))
	cpt === nothing && (cpt = daylength_cpt())
	daylength_base(; tilt=tilt, decl=decl, G=G, cpt=cpt, view=view)
	_dl_render_overlay!(daylength_overlay(doy; tilt=tilt, decl=decl, cpt=cpt, view=view), year)
	# Finished by showfig: colorbar!/plot! do not pass `dpi` on to psconvert. No margin: the white page
	# is the picture (see _DL_W).
	if savefig != "" || show
		GMT.FIG_MARGIN[] = 0
		showfig(savefig=savefig, show=show, dpi=dpi)
	end
	return nothing
end

# ---- The dialog's host side -------------------------------------------------------------------------

const _DL_YEAR = Ref{Any}(nothing)     # (tilt, decl, G, cpt) of the last tilt drawn
const _DL_PNG  = Ref{Vector{UInt8}}(UInt8[])
const _DL_LIST = Ref{Vector{Float64}}(Float64[])

function _dl_year_for(tilt::Float64)
	y = _DL_YEAR[]
	if y === nothing || y[1] != tilt
		decl, G = daylength_year(; tilt=tilt)
		y = _DL_YEAR[] = (tilt, decl, G, daylength_cpt())
	end
	return y
end

# The base picture for `tilt` and `view`, drawn into GMT.jl's own figure file; its bytes.
function _daylength_base_png(tilt::Float64, view::Int)::Vector{UInt8}
	_, decl, G, cpt = _dl_year_for(tilt)
	daylength_base(; tilt=tilt, decl=decl, G=G, cpt=cpt, view=view)
	fig = GMT.PSname[][1:end-2] * "png"
	GMT.FIG_MARGIN[] = 0
	showfig(savefig=fig, show=false, dpi=110)
	return read(fig)
end

# C callbacks (74_daylength_gmt.cpp). Both two-phase: out == NULL computes and returns the size, the
# second call copies. 0 = failure, the reason written to `err`.
# The base picture (PNG bytes) for a tilt and a view.
function _on_daylength_base(tilt::Cdouble, view::Cint, out::Ptr{UInt8}, cap::Cint, err::Ptr{UInt8}, errcap::Cint)::Cint
	try
		out == C_NULL && (_DL_PNG[] = _daylength_base_png(Float64(tilt), Int(view)))
		b = _DL_PNG[]
		isempty(b) && error("no picture was drawn")
		out == C_NULL && return Cint(length(b))
		cap >= length(b) || error("picture buffer too small ($(length(b)) bytes needed)")
		unsafe_copyto!(out, pointer(b), length(b))
		return Cint(length(b))
	catch e
		nb = _console_write(err, errcap, sprint(showerror, e))
		errcap > 0 && unsafe_store!(err, 0x00, Int(nb) + 1)
		return Cint(0)
	end
end

# The day's display list (doubles).
function _on_daylength_overlay(doy::Cint, tilt::Cdouble, view::Cint, out::Ptr{Cdouble}, cap::Cint,
                               err::Ptr{UInt8}, errcap::Cint)::Cint
	try
		if out == C_NULL
			_, decl, _, cpt = _dl_year_for(Float64(tilt))
			_DL_LIST[] = daylength_overlay(Int(doy); tilt=Float64(tilt), decl=decl, cpt=cpt, view=Int(view))
		end
		L = _DL_LIST[]
		out == C_NULL && return Cint(length(L))
		cap >= length(L) || error("display-list buffer too small ($(length(L)) needed)")
		unsafe_copyto!(out, pointer(L), length(L))
		return Cint(length(L))
	catch e
		nb = _console_write(err, errcap, sprint(showerror, e))
		errcap > 0 && unsafe_store!(err, 0x00, Int(nb) + 1)
		return Cint(0)
	end
end

# NOTHING OF THIS TOOL RUNS AT START-UP: the menu entry fires warmupTool("daylength") and waits for the
# callbacks; the entry below is a plain Dict insertion baked into the precompiled image.
const _DL_WIRED = Ref(false)
function _register_daylength()
	_DL_WIRED[] && return
	haskey(_LIB_FNS, :gmtvtk_set_daylength_callback) || return
	b = @cfunction((t, w, o, c, e, ec) -> Base.invokelatest(_on_daylength_base, t, w, o, c, e, ec),
	               Cint, (Cdouble, Cint, Ptr{UInt8}, Cint, Ptr{UInt8}, Cint))
	l = @cfunction((d, t, w, o, c, e, ec) -> Base.invokelatest(_on_daylength_overlay, d, t, w, o, c, e, ec),
	               Cint, (Cint, Cdouble, Cint, Ptr{Cdouble}, Cint, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_daylength_callback), Cvoid, (Ptr{Cvoid}, Ptr{Cvoid}), b, l)
	_DL_WIRED[] = true
	return
end
warm_register("daylength", _register_daylength)
