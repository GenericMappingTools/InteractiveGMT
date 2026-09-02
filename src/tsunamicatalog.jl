# tsunamicatalog.jl — Geophysics > Tsunamis > "NOAA historical catalog".
#
# The NOAA/NCEI Historical Tsunami Event Database shipped as data/noaa_historical_tsunami_events.dat
# (converted from NCEI's CSV: `lon lat` first, then 18 numeric columns, then the country as trailing
# text). One screen-constant circle per event.
#
# `deaths` / `housesDestroyed` are the TSUNAMI's own toll, not NCEI's `*Total` columns — those count
# the WHOLE source event, earthquake and wave together, which is a different quantity and a wildly
# different number: Haiti 2010 is 7 tsunami deaths against a 316000 event total, and the 1920 Gansu
# and 2008 Sichuan earthquakes carry six-figure totals with no tsunami deaths at all. Established
# from the data (the totals are >= the tsunami figure in all 289 rows carrying both, and the runups
# file's per-location deaths — tsunami deaths by definition — sum to the tsunami figure, not the
# total); NCEI documents neither field.
#
# It is a POINT DATASET OVER THE VIEW, exactly like Geography's volcanoes / meteorites / vents, so it
# is reached through the SAME door they are: the C++ menu leaf calls `geoPlot("noaa_tsunami", "")`,
# `_on_geography` (geography.jl) dispatches, and the read + region clip is `_geo_select` — the one
# reader those datasets already use. SACRED_LAW: same operation, same function; this file only knows
# what the catalog's OWN columns mean.
#
# The circle DIAMETER is `_seis_mag_size` (seismicity.jl) applied to the event's `eqMagnitude` — THE
# magnitude->size scale of this app (the USGS one Seismicity already draws with). It is not restated
# here: a magnitude is a magnitude, and there is exactly one function that sizes a circle from one.

# `causeCode` spelled out. NOT invented and NOT guessed from the field name: these are NCEI's OWN
# labels, read off their published service — the same database's map layer carries the code and its
# text side by side (gis.ngdc.noaa.gov .../hazards/MapServer/1, fields CAUSE_CODE and CAUSE), so this
# tuple is a transcription of NCEI's answer, indexed by code+1.
#
# Cross-checked against the source database before being trusted, using its `earthquakeEventId` /
# `volcanoEventId` links (columns the .dat no longer keeps): every label containing "Earthquake"
# (1,2,3,4,5) landed on events carrying an earthquake link (43-98% of them) and every label
# containing "Volcano" (4,5,6,7) on events carrying a volcano link (86-100%), while the labels
# naming neither (0 Unknown, 8 Landslide, 9 Meteorological, 10 Explosion) sat at 0-3% and 0-1%.
# A shifted or mis-ordered table could not produce that.
#
# The catalog also holds TWO events with `causeCode` 11 (ids 910 and 2687, USA, May 1861), and NCEI
# publishes no label for it: their map layer has no such event (it carries only events WITH
# coordinates, and both of these have none) and their event API returns the bare code. So 11 — and
# any future code — is shown as the NUMBER, never as a made-up meaning. Both are dropped before
# plotting anyway, having no position.
const _NOAA_TSU_CAUSE = ("Unknown", "Earthquake", "Questionable Earthquake", "Earthquake and Landslide",
                         "Volcano and Earthquake", "Volcano, Earthquake and Landslide", "Volcano",
                         "Volcano and Landslide", "Landslide", "Meteorological", "Explosion")

# `eventValidity` spelled out, same rule and same sources as the cause codes above — NCEI's words,
# never ours. Codes 0-4 are the CAUSE table's own source (the map layer's EVENT_VALIDITY_CODE /
# EVENT_VALIDITY pair). That layer holds no event at all below 0, so -1 came from NCEI's database
# documentation instead, which states it outright: "there are more than 2,200 source events in the
# database with event validities >0 (-1 = erroneous entry, 0 = seiche)". The two sources agree where
# they overlap (its "0 = seiche" is the layer's "Event that only caused a seiche"), which is what
# makes them safe to join. Only the capitalisation of -1 is ours. INDEXED BY code+2: this scale
# starts at -1, and 215 events in the catalog carry it (83 of them with coordinates, so they DO get
# plotted — unlike the label-less cause code 11, this one could not be left as a bare number).
const _NOAA_TSU_VALIDITY = ("Erroneous entry", "Event that only caused a seiche", "Very Doubtful Tsunami",
                            "Questionable Tsunami", "Probable Tsunami", "Definite Tsunami")

# The catalog's numeric columns, in the file's own order after lon/lat. The LABELS are the column
# names re-spaced for reading and nothing else — the file carries no units, and the one code book
# that could be sourced from NCEI is `causeCode` above; nothing else is decoded. `nd` is how many
# decimals the value is shown with, or a sentinel: -1 = the 0/1 the converter made out of the
# source's True/False, printed back as True/False; -2 = a cause code, spelled out through
# `_NOAA_TSU_CAUSE`; -3 = an event-validity code, through `_NOAA_TSU_VALIDITY`.
const _NOAA_TSU_COLS = ((  3, "Year",                    0),
                        (  4, "Month",                   0),
                        (  5, "Day",                     0),
                        (  6, "Id",                      0),
                        (  7, "Event validity",         -3),
                        (  8, "Cause",                  -2),
                        (  9, "Num runups",              0),
                        ( 10, "Ts intensity",            1),
                        ( 11, "Ts Mt Ii",                1),
                        ( 12, "Ts Mt Abe",               1),
                        ( 13, "Max water height",        2),
                        ( 14, "Eq magnitude",            1),
                        ( 15, "Eq depth",                1),
                        ( 16, "Deaths",                  0),
                        ( 17, "Houses destroyed",        0),
                        ( 18, "Oceanic tsunami",        -1))

# Columns used by name, so a change of file layout is caught here instead of silently mis-read.
const _NOAA_TSU_YEAR  = 3
const _NOAA_TSU_MONTH = 4
const _NOAA_TSU_DAY   = 5
const _NOAA_TSU_MAG   = 14
const _NOAA_TSU_DATE_COLS = (_NOAA_TSU_YEAR, _NOAA_TSU_MONTH, _NOAA_TSU_DAY)

# One cell. NaN prints blank — an event that carries no value is not the number zero.
function _noaa_tsu_cell(v::Float64, nd::Int)::String
	isnan(v) && return ""
	(nd == -1) && return (v != 0) ? "True" : "False"
	# A code with no published label is shown as the NUMBER — never as a made-up meaning.
	if (nd == -2)
		c = trunc(Int, v)
		return (0 <= c <= length(_NOAA_TSU_CAUSE) - 1) ? _NOAA_TSU_CAUSE[c+1] : string(c)
	end
	if (nd == -3)
		c = trunc(Int, v)
		return (-1 <= c <= length(_NOAA_TSU_VALIDITY) - 2) ? _NOAA_TSU_VALIDITY[c+2] : string(c)
	end
	(nd == 0) && return string(trunc(Int, v))
	return string(round(v; digits=nd))
end

# year/month/day -> a date string. The catalog runs back to -2000 and its early events carry a year
# with no month and often no day, so the date is written with exactly as much of it as the event has:
# "2011-03-11", "1650-04", "-2000". Never a fabricated January 1st.
function _noaa_tsu_date(y::Float64, mo::Float64, dy::Float64)::String
	isnan(y) && return ""
	s = string(trunc(Int, y))
	isnan(mo) && return s
	moi = trunc(Int, mo)
	(1 <= moi <= 12) || return s
	s *= "-" * lpad(moi, 2, '0')
	isnan(dy) && return s
	di = trunc(Int, dy)
	(1 <= di <= GMT.Dates.daysinmonth(trunc(Int, y), moi)) || return s
	return s * "-" * lpad(di, 2, '0')
end

# Read the catalog, clipped to the view by the shared `_geo_select`, and drop the events that carry
# NO POSITION. 464 of the 3130 records have an empty latitude/longitude in NCEI's own database;
# gmtselect passes such records through untested (there is nothing to test them against), so they
# arrive here whatever region was asked for and would otherwise be stamped at NaN,NaN.
# Returns (xs, ys, data, country), every vector aligned 1:1 and `data` one row per kept event.
function _noaa_tsunami_data(W, E, S, N)
	Sel, xs = _geo_select("noaa_historical_tsunami_events.dat", W, E, S, N)
	Sel === nothing && return (Float64[], Float64[], zeros(0, 0), String[])
	ys   = Sel.data[:, 2]
	txt  = Sel.text
	keep = [k for k in eachindex(xs) if !isnan(xs[k]) && !isnan(ys[k])]
	isempty(keep) && return (Float64[], Float64[], zeros(0, 0), String[])
	country = Vector{String}(undef, length(keep))
	for (q, k) in enumerate(keep)
		c = strip((txt !== nothing && k <= length(txt)) ? txt[k] : "")
		country[q] = (c == "-") ? "" : String(c)      # "-" is the .dat's placeholder for an empty field
	end
	return xs[keep], ys[keep], Sel.data[keep, :], country
end

# Hover tooltip for one event: the date, the country, then every column the event actually carries.
# A column it does not carry is left out entirely rather than shown blank — the block is read at a
# glance over the map, so empty lines are noise.
function _noaa_tsu_info(row, country::String)::String
	parts = String[]
	d = _noaa_tsu_date(row[_NOAA_TSU_YEAR], row[_NOAA_TSU_MONTH], row[_NOAA_TSU_DAY])
	isempty(d) || push!(parts, d)
	isempty(country) || push!(parts, country)
	for (ci, label, nd) in _NOAA_TSU_COLS
		(ci in _NOAA_TSU_DATE_COLS) && continue              # already spelled out as the date
		v = _noaa_tsu_cell(row[ci], nd)
		isempty(v) || push!(parts, string(label, ": ", v))
	end
	return isempty(parts) ? "tsunami event" : join(parts, "\n")
end

# "Show data table" columns: the event's own record — position, date, country and every numeric
# column of the catalog. How big its circle is drawn is a property of the picture, not of the event,
# and has no business in a data table (same rule as the seismicity table).
const _NOAA_TSU_TABLE_COLS = vcat(["Lon", "Lat", "Date", "Country"],
                                  [lb for (ci, lb, _) in _NOAA_TSU_COLS if !(ci in _NOAA_TSU_DATE_COLS)])

function _noaa_tsu_row(x::Float64, y::Float64, row, country::String)::Vector{String}
	cells = [string(round(x; digits=4)), string(round(y; digits=4)),
	         _noaa_tsu_date(row[_NOAA_TSU_YEAR], row[_NOAA_TSU_MONTH], row[_NOAA_TSU_DAY]), country]
	for (ci, _, nd) in _NOAA_TSU_COLS
		(ci in _NOAA_TSU_DATE_COLS) && continue
		push!(cells, _noaa_tsu_cell(row[ci], nd))
	end
	return cells
end

# The layer. Circles, screen-constant, sized by `_seis_mag_size` off the event's earthquake
# magnitude — the SAME scale Seismicity draws with, so a M7 reads the same size in both windows. An
# event with no magnitude (the catalog is full of them: volcanic, landslide, and the pre-instrumental
# entries) lands on that scale's smallest size, which is what it already does for a magnitude-less
# earthquake. Returns false when the view holds no event, so the caller records no session recipe.
function _plot_noaa_tsunami(scene::Ptr{Cvoid}, W, E, S, N)::Bool
	xs, ys, data, country = _noaa_tsunami_data(W, E, S, N)
	isempty(xs) && return false
	n     = length(xs)
	rows  = [view(data, k, :) for k in 1:n]
	sizes = [_seis_mag_size(Float64(data[k, _NOAA_TSU_MAG])) for k in 1:n]
	return add_symbols!(scene, xs, ys; symbol=:circle, size=sizes, fill=:cyan, edge=:black,
	                    edgewidth=1.0, name=_geo_layer_name("noaa_tsunami"),
	                    info=[_noaa_tsu_info(rows[k], country[k]) for k in 1:n],
	                    datanames=_NOAA_TSU_TABLE_COLS,
	                    datarows=[_noaa_tsu_row(xs[k], ys[k], rows[k], country[k]) for k in 1:n])
end
