# climate.jl — T & L > ENSO / NAO / SOI / Keeling curve: four climate records, each a page of the X,Y
# plot tool. The data is GMT.jl's: `enso(data=true)` (NOAA CPC's Oceanic Niño
# Index), `nao(data=true)`, `soi(data=true)`. The Keeling curve's page is the read `keeling()` plots from
# (NOAA GML, Mauna Loa) — its own `keeling(data=true)` dies in GMT.jl's gmtread (BoundsError naming the
# 6 columns it asks for), so the plot path's 2-column read is taken. nao() and soi() are newer than the
# GMT.jl release in use may be: until one has them, temporary stand-ins read NOAA here (see _climate_nao).
#
# A page drawn from a GMT.jl function is marked so (XYPage::gjSpec, "fn=enso" ...), so File > Plot with
# GMT.jl writes the one call — `enso()`, `nao()`, `soi()`, `keeling()` — that downloads and draws the
# record itself (_xy_climate_steps, xyscript.jl).

# Decimal years -> Unix epoch seconds, the X,Y window's time axis (xtime!). GMT.jl's `yeardecimal` does
# the calendar.
_climate_unix(t::Vector{Float64})::Vector{Float64} = [GMT.Dates.datetime2unix(GMT.yeardecimal(v)) for v in t]

# A NOAA text file into a local copy, the way GMT.jl's enso() fetches its own (curl, which those sites
# answer quickly; Downloads as the fallback). -> the local file's name; the caller removes it.
function _climate_fetch(url::String)::String
	fname = joinpath(GMT.TMPDIR_USR.dir, "iGMT_climate_" * basename(url))
	try
		run(`curl -s --show-error --fail $url -o $fname`)
	catch
		GMT.Downloads.download(url, fname)
	end
	return fname
end

# A NOAA CPC table, read by gmtread from the local copy.
function _climate_cpc_read(url::String; kw...)
	fname = _climate_fetch(url)
	try
		return GMT.gmtread(fname; kw...)
	finally
		rm(fname; force = true)
	end
end

# A signed record split at its zero crossings: the crossing points inserted (linear between the samples
# either side), so the part above zero and the part below each fill exactly to the axis.
function _climate_split_sign(x::Vector{Float64}, y::Vector{Float64})
	xs, ys = Float64[], Float64[]
	for k in eachindex(x)
		if k > 1 && y[k - 1] * y[k] < 0
			push!(xs, x[k - 1] + (x[k] - x[k - 1]) * y[k - 1] / (y[k - 1] - y[k]))
			push!(ys, 0.0)
		end
		push!(xs, x[k]);  push!(ys, y[k])
	end
	return xs, max.(ys, 0.0), min.(ys, 0.0)
end

# An index page: the positive part filled red, the negative blue (`swap`: the other way round — the SOI,
# whose El Niño side is the negative one), each opaque, down to zero.
function _climate_index_page(x::Vector{Float64}, y::Vector{Float64}; title::String, ylabel::String,
                             pos::String, neg::String, swap::Bool = false)
	xs, yp, yn = _climate_split_sign(x, y)
	p = _xyplot(xs, yp; name = pos, color = swap ? :blue : :red, linewidth = 0.5, title = title,
	            xlabel = "Year", ylabel = ylabel, xtime = :date)
	_xyadd!(p, xs, yn; name = neg, color = swap ? :red : :blue, linewidth = 0.5)
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), p.h)
	for idx in (n - 2, n - 1)
		ccall(_fn(:gmtvtk_xyplot_set_series_kind), Cint, (Ptr{Cvoid}, Cint, Cint), p.h, idx, 2)
		ccall(_fn(:gmtvtk_xyplot_set_series_fill_alpha), Cint, (Ptr{Cvoid}, Cint, Cdouble), p.h, idx, 1.0)
	end
	return p
end

function _climate_enso()
	D = GMT.enso(data = true)                       # [decimal year  ONI]
	p = _climate_index_page(_climate_unix(Float64.(D.data[:, 1])), Float64.(D.data[:, 2]);
	                        title = "Oceanic Niño Index", ylabel = "ONI (°C)", pos = "El Niño", neg = "La Niña")
	ccall(_fn(:gmtvtk_xyplot_set_gmtjl), Cint, (Ptr{Cvoid}, Cint, Cstring), p.h, 0, "fn=enso")
	return p
end

# NAO and SOI come from GMT.jl's own nao() / soi() (added beside enso() and keeling(), GMT.jl after
# v1.44.1), and their pages are then marked as drawn by them, so the script calls them. A GMT.jl release
# without them gets the stand-ins below, whose pages export as plain X,Y pages.
# TEMPORARY: delete _climate_nao_patch / _climate_soi_patch (and the isdefined branches) once a released
# GMT.jl has nao() and soi().
function _climate_nao()
	x, y, fn = if isdefined(GMT, :nao)
		D = GMT.nao(data = true)                    # [decimal year  NAO]
		Float64.(D.data[:, 1]), Float64.(D.data[:, 2]), "nao"
	else
		(_climate_nao_patch()..., "")
	end
	p = _climate_index_page(_climate_unix(x), y; title = "North Atlantic Oscillation", ylabel = "NAO index",
	                        pos = "NAO+", neg = "NAO−")
	isempty(fn) || ccall(_fn(:gmtvtk_xyplot_set_gmtjl), Cint, (Ptr{Cvoid}, Cint, Cstring), p.h, 0, "fn=$fn")
	return p
end

function _climate_soi()
	x, y, fn = if isdefined(GMT, :soi)
		D = GMT.soi(data = true)                    # [decimal year  SOI]
		Float64.(D.data[:, 1]), Float64.(D.data[:, 2]), "soi"
	else
		(_climate_soi_patch()..., "")
	end
	p = _climate_index_page(_climate_unix(x), y; title = "Southern Oscillation Index", ylabel = "SOI",
	                        pos = "SOI+ (La Niña)", neg = "SOI− (El Niño)", swap = true)
	isempty(fn) || ccall(_fn(:gmtvtk_xyplot_set_gmtjl), Cint, (Ptr{Cvoid}, Cint, Cstring), p.h, 0, "fn=$fn")
	return p
end

# TEMPORARY stand-in for GMT.jl's nao(data=true): NOAA CPC's monthly NAO, one row per month
# (year  month  index) -> (decimal years, index).
function _climate_nao_patch()
	D = _climate_cpc_read("https://www.cpc.ncep.noaa.gov/products/precip/CWlink/pna/norm.nao.monthly.b5001.current.ascii";
	                      data = true)              # ".ascii" says nothing to gmtread: it is a table
	M = Float64.(D.data)
	return M[:, 1] .+ (M[:, 2] .- 0.5) ./ 12, M[:, 3]
end

# TEMPORARY stand-in for GMT.jl's soi(data=true): CPC's SOI as NOAA PSL serves it — a "first last" year
# line, one row per year (year + 12 monthly values, -99.99 = none), trailer lines. Only rows that are
# exactly a year are taken. -> (decimal years, index).
function _climate_soi_patch()
	fname = _climate_fetch("https://psl.noaa.gov/data/correlation/soi.data")
	x, y = Float64[], Float64[]
	try
		for l in eachline(fname)
			f = split(l)
			length(f) == 13 || continue
			yr = tryparse(Int, f[1]);  yr === nothing && continue
			for m in 1:12
				v = tryparse(Float64, f[m + 1])
				(v === nothing || v <= -99) && continue
				push!(x, yr + (m - 0.5) / 12);  push!(y, v)
			end
		end
	finally
		rm(fname; force = true)
	end
	isempty(y) && error("SOI: NOAA PSL returned no monthly values")
	return x, y
end

function _climate_keeling()
	D = GMT.gmtread("https://gml.noaa.gov/webdata/ccgg/trends/co2/co2_mm_mlo.txt", i = "2,3")   # keeling()'s read
	p = _xyplot(_climate_unix(Float64.(D.data[:, 1])), Float64.(D.data[:, 2]); name = "Monthly mean", color = :red,
	            linewidth = 1, title = "Keeling Curve - Mauna Loa CO2", xlabel = "Year", ylabel = "CO2 (ppm)",
	            xtime = :date)
	ccall(_fn(:gmtvtk_xyplot_set_gmtjl), Cint, (Ptr{Cvoid}, Cint, Cstring), p.h, 0, "fn=keeling")
	return p
end

# T & L menu entry: `which` is "enso", "nao", "soi" or "keeling". Called through the eval bridge, behind the
# viewer's busy notice; a failure is thrown so the menu shows it. The X,Y window the page landed in comes
# to the front — out of its parked row if it was parked.
function _climate_xy(which::AbstractString)
	p = which == "enso"    ? _climate_enso() :
	    which == "nao"     ? _climate_nao() :
	    which == "soi"     ? _climate_soi() :
	    which == "keeling" ? _climate_keeling() : error("climate: unknown record '$which'")
	ccall(_fn(:gmtvtk_xyplot_bring_front), Cvoid, (Ptr{Cvoid},), p.h)
	return nothing
end
