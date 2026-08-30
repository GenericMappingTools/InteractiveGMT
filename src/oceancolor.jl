# oceancolor.jl — Tools > Ocean Color Data Browser (deps/ui/oceancolor_browser.ui).
#
# The catalogue/URL half of the browser: given (Instrument, Product, Period, Date) it builds the
# OB.DAAC address of the L3 browse image, probes the server for what actually exists, and caches the
# PNG that the dialog's two preview tiles show. The dialog itself (QUiLoader on the .ui, C++ side)
# comes later — everything here is callable and testable on its own.
#
# ---------------------------------------------------------------------------------------------
# The address scheme (verified live against the server, 2026-08-03)
#
#   https://oceandata.sci.gsfc.nasa.gov/showimages/<DIR>/L3BRS/<yyyy>/<mmdd>/<FILE>.png
#
#   <DIR>   per instrument — MODISA, MODIST, VIIRS (=SNPP), VIIRSJ1 (=NOAA-20/JPSS-1)
#   <mmdd>  the period's START day (a monthly composite lives under .../2026/0601/, not 0630)
#   <FILE>  <PREFIX>.<stamp>.L3m.<PERIOD>.<SUITE>.<VAR>.<RES>[.NRT].nc
#             <stamp>  = yyyymmdd for DAY, yyyymmdd_yyyymmdd (start_end) for 8D / MO / YR
#             <PERIOD> = DAY | 8D | MO | YR
#             NRT      = near-real-time. Recent days are NRT-only; older days exist BOTH ways
#                        (the reprocessed non-NRT plus a leftover NRT), and annual files are never
#                        NRT. So every lookup tries both spellings — see `_oc_png_urls`.
#
# The browse PNG is the full L3 grid, not a decorated figure: 8640x4320 (4 km) / 4320x2160 (9 km),
# 8-bit palette, plate carrée over the whole globe. That means it can be dropped straight into a
# window as a georeferenced image ([-180 180 -90 90], pixel registration) — no cropping of legends.
#
# The netCDF behind an image is at .../ob/getfile/<FILE> but that redirects to Earthdata URS and
# needs credentials (~/.netrc), unlike the freely readable browse images. `oc_data_url` builds it;
# wiring the authenticated download is the Extract/Download button's job, still to come.
#
# ---------------------------------------------------------------------------------------------
# The 8-day calendar
#
# OB.DAAC 8-day composites are NOT a rolling window: each year restarts at day-of-year 1 and steps
# 1, 9, 17, … 361, so the last period of a year is short (5 or 6 days) and periods never straddle
# New Year. `_oc_period_span` implements exactly that.

using GMT: Dates, GMTuserdir		# not exported by GMT, so named one by one
import Downloads					# HEAD-probing the catalogue + fetching the browse PNGs

# ------------------------------------------------------------------------------------------------
# Catalogue

"""
One spacecraft/sensor row of the Instrument combo.

* `dir`     — the `showimages/<DIR>` path element
* `prefix`  — the file-name prefix (`AQUA_MODIS.2026…`)
* `first`   — first day the mission has L3 data for; the browser must never look further back
* `yr_first`/`yr_last` — the years for which ANNUAL (YR) composites exist. `yr_last == 0` marks a
  mission with no annual products at all (NOAA-20), so the Period combo can grey "Annual" out.
* `products` — indices into `_OC_PRODUCTS` this mission actually flies. Not every sensor carries
  every product: the OLCI pair is an ocean-colour imager with no thermal bands, so it has
  chlorophyll and no SST at all. The dialog greys the rest out rather than offering a search that
  can only come back empty.
"""
struct OCInstrument
	label::String
	dir::String
	prefix::String
	first::Dates.Date
	yr_first::Int
	yr_last::Int
	products::Vector{Int}
end

# Order MUST match comboInstrument in oceancolor_browser.ui. End dates are open — the mission rows
# below carry only the START (a hard fact) and the annual-file range (what has been produced so
# far); "what is the newest image?" is never assumed, it is asked of the server by `oc_latest`.
const _OC_INSTRUMENTS = OCInstrument[
	OCInstrument("Aqua-MODIS",   "MODISA",  "AQUA_MODIS",     Dates.Date(2002, 7,  4), 2002, 2025, [1, 2]),
	OCInstrument("Terra-MODIS",  "MODIST",  "TERRA_MODIS",    Dates.Date(2000, 2, 24), 2000, 2024, [1, 2]),
	OCInstrument("SNPP-VIIRS",   "VIIRS",   "SNPP_VIIRS",     Dates.Date(2012, 1,  2), 2012, 2024, [1, 2]),
	OCInstrument("NOAA20-VIIRS", "VIIRSJ1", "JPSS1_VIIRS",    Dates.Date(2017, 12, 13),   0,    0, [1, 2]),
	# Sentinel-3 OLCI. Verified against the OB.DAAC file_search API + the browse tree (2026-08-03):
	# the browse directories are S3OLCI / S3BOLCI (NOT OLCIS3A/OLCIS3B) and the file prefix carries the
	# processing stream — S3A_OLCI_ERRNT, "Extended Reduced Resolution, Non-Time-critical". Chlorophyll
	# only: OLCI has no thermal bands, so there is no SST product to look for. S3B's record starts
	# 2018-05-15 (its own first L3 day, the satellite launched that April), not with S3A's.
	OCInstrument("S3A-OLCI",     "S3OLCI",  "S3A_OLCI_ERRNT", Dates.Date(2016, 4,  6), 2016, 2024, [2]),
	OCInstrument("S3B-OLCI",     "S3BOLCI", "S3B_OLCI_ERRNT", Dates.Date(2018, 5, 15), 2018, 2024, [2]),
]

"""
One row of the Product combo: an OB.DAAC *suite* (the `NSST` in the file name) plus the *variable*
it stores (`sst`) and the grid resolution to ask for.
"""
struct OCProduct
	label::String
	suite::String
	var::String
	res::String
end

# Order MUST match comboProduct in oceancolor_browser.ui.
const _OC_PRODUCTS = OCProduct[
	OCProduct("Sea surface temperature (night 11 mic)", "NSST", "sst",     "4km"),
	OCProduct("Chlorophyll concentration",              "CHL",  "chlor_a", "4km"),
]

# Order MUST match comboPeriod in oceancolor_browser.ui: (UI label, file-name token).
const _OC_PERIODS = [("Daily", "DAY"), ("8-Day", "8D"), ("Monthly", "MO"), ("Annual", "YR")]

_oc_instrument(i::Int)  = _OC_INSTRUMENTS[clamp(i, 1, length(_OC_INSTRUMENTS))]
_oc_product(i::Int)     = _OC_PRODUCTS[clamp(i, 1, length(_OC_PRODUCTS))]
_oc_period(i::Int)      = _OC_PERIODS[clamp(i, 1, length(_OC_PERIODS))][2]

# The product index the dialog asked for, snapped to one this instrument actually carries.
_oc_clamp_product(inst::OCInstrument, i::Int) = i in inst.products ? i : first(inst.products)

# Tell the dialog which Product rows this instrument really has (bit i-1 set = row i is real) and
# which one is in force. It greys the rest out and moves the combo silently — silently because the
# clamp above has ALREADY decided, so a second round trip would only ask the same question again.
_oc_push_products(dlg::Ptr{Cvoid}, inst::OCInstrument, cur::Int) =
	(dlg != C_NULL && ccall(_fn(:gmtvtk_oc_set_products), Cvoid, (Ptr{Cvoid}, Cint, Cint), dlg,
	                        Cint(mapreduce(i -> 1 << (i - 1), |, inst.products; init = 0)), Cint(cur));
	 nothing)

const _OC_BASE = "https://oceandata.sci.gsfc.nasa.gov"

# The browse PNG is the bare L3 grid over the whole globe, pixel-registered. Both resolutions are a
# plain 360/nx degree cell, so one region for all of them.
const _OC_PNG_REGION = (-180.0, 180.0, -90.0, 90.0)
_oc_png_size(res::String) = res == "9km" ? (4320, 2160) : (8640, 4320)

# ------------------------------------------------------------------------------------------------
# The period calendar

"""
    _oc_period_span(period, d) -> (start, stop)

Snap day `d` onto the composite grid of `period` and return the period's inclusive span. `DAY` is
`(d, d)`; `MO` is the containing calendar month; `YR` the containing calendar year; `8D` the
day-of-year 1/9/17/… bucket, clipped at 31 December (periods never cross a year boundary).
"""
function _oc_period_span(period::String, d::Dates.Date)
	if (period == "DAY")   return (d, d)
	elseif (period == "MO")
		s = Dates.Date(Dates.year(d), Dates.month(d), 1)
		return (s, Dates.lastdayofmonth(s))
	elseif (period == "YR")
		return (Dates.Date(Dates.year(d), 1, 1), Dates.Date(Dates.year(d), 12, 31))
	elseif (period == "8D")
		doy = Dates.dayofyear(d)
		s   = Dates.Date(Dates.year(d), 1, 1) + Dates.Day(8 * div(doy - 1, 8))
		return (s, min(s + Dates.Day(7), Dates.Date(Dates.year(d), 12, 31)))
	end
	error("Ocean Color: unknown period '$period'")
end

"""
    _oc_period_prev(period, start) -> Date

The start day of the composite immediately BEFORE the one starting at `start`. Stepping back one
day and re-snapping handles the short end-of-year 8-day bucket and month lengths for free.
"""
_oc_period_prev(period::String, start::Dates.Date) = _oc_period_span(period, start - Dates.Day(1))[1]

# yyyymmdd for a single day; yyyymmdd_yyyymmdd for every composite.
function _oc_stamp(period::String, start::Dates.Date, stop::Dates.Date)
	f(x) = Dates.format(x, "yyyymmdd")
	period == "DAY" ? f(start) : string(f(start), '_', f(stop))
end

# ------------------------------------------------------------------------------------------------
# URLs

"""
    _oc_basenames(inst, prod, period, d) -> Vector{String}

The netCDF file names (no `.png`) that could hold this instrument/product/period at day `d`, NRT
spelling first. Annual composites are never NRT, so that variant is dropped for `YR`.
"""
function _oc_basenames(inst::OCInstrument, prod::OCProduct, period::String, d::Dates.Date)
	start, stop = _oc_period_span(period, d)
	stem = string(inst.prefix, '.', _oc_stamp(period, start, stop), ".L3m.", period,
	              '.', prod.suite, '.', prod.var, '.', prod.res)
	period == "YR" ? [stem * ".nc"] : [stem * ".NRT.nc", stem * ".nc"]
end

"""
    _oc_png_urls(inst, prod, period, d) -> Vector{String}

Full browse-image URLs for day `d`, in the order they should be probed (NRT first — that is what a
recent date will have; an old date usually answers on the second try).
"""
function _oc_png_urls(inst::OCInstrument, prod::OCProduct, period::String, d::Dates.Date)
	start, _ = _oc_period_span(period, d)
	dir = string(_OC_BASE, "/showimages/", inst.dir, "/L3BRS/",
	             Dates.format(start, "yyyy"), '/', Dates.format(start, "mmdd"), '/')
	[dir * b * ".png" for b in _oc_basenames(inst, prod, period, d)]
end

"""
    oc_data_url(basename) -> String

Address of the L3 netCDF behind a browse image. The browse PNG is the L4 product and the data file
is the L3 one OF THE SAME NAME, so this is a pure rename — there is no second catalogue to consult
and no second lookup to get wrong:

    …/showimages/MODISA/L3BRS/2026/0801/AQUA_MODIS.20260801.….nc.png     the image
    …/cgi/getfile/AQUA_MODIS.20260801.….nc                              the grid

Unlike the freely readable browse images this one is behind Earthdata URS. Verified live
(2026-08-03): `/cgi/getfile/X` → 301 → `/getfile/X` → 302 → `/getfile/urs/?next=…`, i.e. an OAuth
login. So a download needs credentials — a `~/.netrc` entry for `urs.earthdata.nasa.gov` (what curl,
wget and GDAL's `/vsicurl/` all read) or an OB.DAAC `appkey`. See `oc_download_data`.
"""
oc_data_url(basename::AbstractString) = string(_OC_BASE, "/cgi/getfile/", basename)

# The L3 netCDF address for the image a browse-PNG url points at.
oc_data_url_of_png(pngurl::AbstractString) = oc_data_url(_oc_basename_of(pngurl))

# The netCDF name a browse-image URL was built from (strip the trailing ".png").
_oc_basename_of(pngurl::AbstractString) = replace(basename(pngurl), r"\.png$" => "")

# ------------------------------------------------------------------------------------------------
# Asking the server what exists

# HEAD the URL. `throw=false` keeps a 404 a value, not an exception, and the short timeout stops one
# stalled connection from freezing the dialog while it fills its preview tiles.
function _oc_url_exists(url::AbstractString; timeout::Real = 20)
	try
		r = Downloads.request(url; method = "HEAD", throw = false, timeout = timeout)
		return isa(r, Downloads.Response) && 200 <= r.status < 300
	catch
		return false
	end
end

"""
    _oc_resolve(inst, prod, period, d) -> String

The URL that actually exists for day `d`, or `""`. Tries the NRT and reprocessed spellings in turn.
"""
function _oc_resolve(inst::OCInstrument, prod::OCProduct, period::String, d::Dates.Date)
	for u in _oc_png_urls(inst, prod, period, d)
		_oc_url_exists(u) && return u
	end
	return ""
end

# How far back a "find the newest" scan may walk before giving up, per period. Generous enough to
# ride out a processing outage, small enough that a dead product fails in seconds, not minutes.
_oc_scan_depth(period::String) = period == "DAY" ? 20 : period == "8D" ? 8 : period == "MO" ? 8 : 4

"""
    oc_latest(inst, prod, period; n=2, from=today()) -> Vector{NamedTuple}

The `n` most recent images that exist on the server, newest first, as
`(start=Date, stop=Date, url=String)`. This is the "what is the end date?" answer the catalogue
above deliberately does not hard-code: the mission rows know only when each instrument STARTED.

Walks the composite calendar backwards from `from`, probing candidate periods concurrently (the
scan is entirely network-latency bound, so the tasks are worth it), then keeps the newest `n` hits
in date order. Stops early at the instrument's first day, and returns fewer than `n` — possibly
none — rather than inventing an entry.
"""
function oc_latest(inst::OCInstrument, prod::OCProduct, period::String;
                   n::Int = 2, from::Dates.Date = Dates.today())
	starts = Dates.Date[]
	s = _oc_period_span(period, from)[1]
	for _ in 1:(_oc_scan_depth(period) + n)
		s < inst.first && break
		push!(starts, s)
		s = _oc_period_prev(period, s)
	end
	isempty(starts) && return NamedTuple[]

	urls = asyncmap(d -> _oc_resolve(inst, prod, period, d), starts; ntasks = 8)
	out  = NamedTuple[]
	for (d, u) in zip(starts, urls)
		isempty(u) && continue
		a, b = _oc_period_span(period, d)
		push!(out, (start = a, stop = b, url = u))
		length(out) >= n && break
	end
	return out
end

# Convenience wrapper for the dialog, which speaks 1-based combo indices.
oc_latest(iinst::Int, iprod::Int, iperiod::Int; kw...) =
	oc_latest(_oc_instrument(iinst), _oc_product(iprod), _oc_period(iperiod); kw...)

# ------------------------------------------------------------------------------------------------
# The image cache
#
# Browse images are a few MB each and the dialog re-shows the same ones every time it is opened, so
# they are kept next to the other GMT user files rather than in a per-session temp dir.

_oc_cache_dir() = joinpath(GMTuserdir[1], "cache_oceancolor")

"""
    oc_fetch_png(url; refresh=false) -> String

Local path of the browse image, downloading it once and reusing it afterwards. Returns `""` when
the download fails — a preview tile with no image is a normal outcome (offline, product gap), never
an error worth aborting the dialog for.

Downloads to a `.part` file and renames only on success, so an interrupted transfer can never leave
a truncated PNG in the cache to be served forever after.
"""
function oc_fetch_png(url::AbstractString; refresh::Bool = false)
	isempty(url) && return ""
	dir = _oc_cache_dir()
	f   = joinpath(dir, basename(url))
	(!refresh && isfile(f) && filesize(f) > 0) && return f
	try
		mkpath(dir)
		part = f * ".part"
		Downloads.download(url, part; timeout = 300)
		filesize(part) == 0 && (rm(part, force = true); return "")
		mv(part, f, force = true)
		return f
	catch
		rm(f * ".part", force = true)
		return ""
	end
end

# ------------------------------------------------------------------------------------------------
# Downloading the DATA (the L3 netCDF), as opposed to the freely readable browse image
#
# `/cgi/getfile/` is behind Earthdata URS: the request bounces 301 -> 302 -> an OAuth login page.
# libcurl walks that dance on its own given two things — credentials and somewhere to keep the
# session cookie — so the downloader below turns both on:
#
#   CURLOPT_NETRC  = optional  -> read ~/.netrc for a `urs.earthdata.nasa.gov` machine entry, the
#                                 same file curl, wget and GDAL's /vsicurl/ already use
#   COOKIEFILE/JAR             -> URS hands out a session cookie on the redirect and expects it back
#   UNRESTRICTED_AUTH          -> credentials must survive the hop to the login host, or the dance
#                                 never completes
#
# Without a ~/.netrc entry the download fails with the login page instead of the file. That is the
# expected outcome today, not a bug in this code: the credential half is not wired yet.

_oc_cookie_jar() = joinpath(_oc_cache_dir(), "urs_cookies.txt")

const _OC_URS_HOST = "urs.earthdata.nasa.gov"

# The credentials file. libcurl's DEFAULT netrc name is platform-dependent (`_netrc` on Windows,
# `.netrc` elsewhere), so the file is never left to that guess: this ONE path is both what gets
# written and what libcurl is pointed at with CURLOPT_NETRC_FILE.
_oc_netrc() = joinpath(homedir(), ".netrc")

# Does the credentials file already name Earthdata? Both netrc spellings (one line, or `machine` on
# its own line with login/password under it) contain the host verbatim, so one test covers them.
function _oc_have_urs_login()
	f = _oc_netrc()
	return isfile(f) && occursin(_OC_URS_HOST, try read(f, String) catch; "" end)
end

function _oc_downloader()
	dl  = Downloads.Downloader()
	jar = _oc_cookie_jar()
	mkpath(dirname(jar))
	nrc = _oc_netrc()
	C = Downloads.Curl
	dl.easy_hook = (easy, info) -> begin
		C.setopt(easy, C.CURLOPT_NETRC, C.CURL_NETRC_OPTIONAL)
		isfile(nrc) && C.setopt(easy, C.CURLOPT_NETRC_FILE, nrc)
		C.setopt(easy, C.CURLOPT_COOKIEFILE, jar)
		C.setopt(easy, C.CURLOPT_COOKIEJAR, jar)
		C.setopt(easy, C.CURLOPT_UNRESTRICTED_AUTH, 1)
	end
	return dl
end

# --- getting the user logged in ------------------------------------------------------------------
# OB.DAAC gives the browse IMAGES away but keeps the DATA behind NASA Earthdata URS. There is no way
# around that and no anonymous alternative: without a login the grid download returns the URS login
# PAGE, which is where the "trash file in the cache" came from. So the credentials are asked for,
# once, and written to ~/.netrc — the standard file curl, wget and GDAL all already read.

"""
    _oc_ensure_login(scene) -> Bool

Make sure `~/.netrc` has an Earthdata entry, asking the user for one if it has not. Returns false
only when the user cancelled — a caller must then not attempt the download.

The file is WRITTEN here (appended to, if it already exists for other machines), and the user is
told exactly that: where it went, what is in it, and that it is plain text, which is what the netrc
format is and what every tool reading it expects.
"""
function _oc_ensure_login(scene::Ptr{Cvoid})::Bool
	_oc_have_urs_login() && return true
	u = Vector{UInt8}(undef, 256); p = Vector{UInt8}(undef, 256)
	msg = """NASA requires an Earthdata login to download Ocean Color DATA files (the browse images \
	         are free, the grids are not).

	         Type the username and password of your Earthdata account below. Register at \
	         https://urs.earthdata.nasa.gov if you have none.

	         They will be stored in $(_oc_netrc()) — the standard 'netrc' file curl, wget and GDAL \
	         all read — so this is asked only once."""
	ok = ccall(_fn(:gmtvtk_oc_ask_login), Cint,
	           (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint, Ptr{UInt8}, Cint),
	           scene, msg, u, Cint(length(u)), p, Cint(length(p)))
	ok == 0 && return false
	user = _oc_cstr(u); pass = _oc_cstr(p)
	(isempty(user) || isempty(pass)) && return false
	f = _oc_netrc()
	try
		open(f, "a") do io
			filesize(f) > 0 && write(io, "\n")
			write(io, "machine $(_OC_URS_HOST) login $(user) password $(pass)\n")
		end
		Sys.isunix() && chmod(f, 0o600)		# netrc readers refuse a world-readable file on Unix
	catch err
		@error "Ocean Color: could not write $(f)" exception = (err, catch_backtrace())
		_oc_message(scene, "Earthdata login", "Could not write $(f).\n\nThe login was NOT saved.")
		return false
	end
	_oc_message(scene, "Earthdata login saved", """Your Earthdata username and password were written to

	                                               $(f)

	                                               as one line:  machine $(_OC_URS_HOST) login $(user) password ********

	                                               That is the standard 'netrc' file — plain text, in your home directory — \
	                                               and it is what curl, wget, GDAL and this viewer read to log in to NASA. \
	                                               Delete that line to remove the login; edit it if you change your password.""")
	return true
end

# NUL-terminated C buffer -> String.
_oc_cstr(b::Vector{UInt8}) = String(b[1:something(findfirst(==(0x00), b), length(b) + 1) - 1])

_oc_message(scene::Ptr{Cvoid}, title::AbstractString, text::AbstractString) =
	ccall(_fn(:gmtvtk_oc_message), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), scene, String(title), String(text))

# Is this really the DATA file, or the URS login page saved under its name? netCDF classic starts
# "CDF\1"/"CDF\2", netCDF-4 is HDF5 ("\x89HDF\r\n\x1a\n") — an HTML page is neither. Checked on every
# fresh download AND on every cache hit, so a bad file can neither be kept nor be served later.
function _oc_is_data_file(f::AbstractString)
	try
		b = open(io -> read(io, 8), f)
		length(b) >= 4 || return false
		(b[1] == 0x43 && b[2] == 0x44 && b[3] == 0x46) && return true				# "CDF"
		return length(b) >= 8 && b[1] == 0x89 && b[2] == 0x48 && b[3] == 0x44 && b[4] == 0x46	# HDF5
	catch
		return false
	end
end

"""
    _oc_probe_size(url) -> (nbytes, ranged)

Total size of `url` and whether the server will serve byte ranges of it. `(0, false)` when the
server does not say — which is the signal to fall back to one plain stream.
"""
function _oc_probe_size(url::AbstractString)
	try
		r = Downloads.request(url; method = "HEAD", throw = false, timeout = 30,
		                      downloader = _oc_downloader())
		isa(r, Downloads.Response) || return (0, false)
		200 <= r.status < 300 || return (0, false)
		n = 0
		ranged = false
		for (k, v) in r.headers
			lk = lowercase(k)
			lk == "content-length" && (n = something(tryparse(Int, strip(v)), 0))
			lk == "accept-ranges"  && (ranged = occursin("bytes", lowercase(v)))
		end
		return (n, ranged && n > 0)
	catch
		return (0, false)
	end
end

# --- the progress bar -------------------------------------------------------------------------
# The download runs on the UI thread (the button calls Julia synchronously), so these three are the
# only thing repainting the window while it happens — `_set` pumps the Qt loop on the C++ side. It
# returns 1 when the user pressed Cancel, which aborts the transfer.

_oc_prog_begin(scene::Ptr{Cvoid}, what::AbstractString) =
	ccall(_fn(:gmtvtk_oc_progress_begin), Cvoid, (Ptr{Cvoid}, Cstring), scene, String(what))

_oc_prog_set(what::AbstractString, done::Real, total::Real) =
	ccall(_fn(:gmtvtk_oc_progress_set), Cint, (Cstring, Cdouble, Cdouble),
	      String(what), Float64(done), Float64(total)) != 0

_oc_prog_end() = ccall(_fn(:gmtvtk_oc_progress_end), Cvoid, ())

struct OCCancelled <: Exception end

"""
    oc_download_data(url; out="", nchunks=4, minchunk=8*1024*1024) -> String

Download the L3 netCDF at `url` into the cache and return its path (`""` on failure).

Split into `nchunks` PARALLEL byte-range requests when the server both advertises `Accept-Ranges:
bytes` and the file is big enough for the split to be worth its overhead; otherwise one plain
stream. The chunks are fetched concurrently and written in order, so the result is byte-identical
either way — the parallel path is a speed-up, never a different file.

Writes to `.part` files and renames only once every chunk is in, so an interrupted transfer can
never leave a half-file in the cache to be reused as if it were whole.
"""
function oc_download_data(url::AbstractString; out::AbstractString = "", nchunks::Int = 4,
                          minchunk::Int = 8 * 1024 * 1024, progress = nothing)
	isempty(url) && return ""
	dir = _oc_cache_dir()
	f   = isempty(out) ? joinpath(dir, basename(url)) : String(out)
	# A cache hit is only a hit if what is cached is the DATA. A previous un-authenticated attempt may
	# have saved the URS login PAGE under this name; that file is deleted here rather than served for
	# ever after (and rather than left lying in the cache directory as junk).
	if isfile(f) && filesize(f) > 0
		_oc_is_data_file(f) && return f
		rm(f, force = true)
	end
	mkpath(dir)

	total, ranged = _oc_probe_size(url)
	k = (ranged && total >= nchunks * minchunk) ? nchunks : 1
	# libcurl reports (total, now) per easy handle. On one stream that IS the answer; across chunks
	# each handle only knows its own slice, so the slices are summed into one figure and the total
	# comes from the size probe (which the chunked path already needed anyway).
	report = progress === nothing ? nothing : (dn, tt) -> progress(dn, tt)
	try
		if k == 1
			part = f * ".part"
			cb = report === nothing ? nothing :
			     (tt, dn) -> report(dn, tt > 0 ? tt : total)		# Downloads gives (total, now)
			Downloads.download(url, part; timeout = 1800, downloader = _oc_downloader(),
			                   progress = cb)
			filesize(part) == 0 && (rm(part, force = true); return "")
			# What came back may be the URS login page with a 200 on it. It never becomes a cached file.
			_oc_is_data_file(part) || (rm(part, force = true); return "")
			mv(part, f, force = true)
			return f
		end
		# Parallel byte ranges. Each task gets its OWN Downloader: one libcurl easy handle cannot be
		# driven by several tasks at once.
		edges = [div(total * i, k) for i in 0:k]
		parts = ["$f.part$i" for i in 1:k]
		seen  = zeros(Int, k)			# bytes in per chunk, summed for the one progress figure
		@sync for i in 1:k
			Threads.@spawn begin
				rng = "bytes=$(edges[i])-$(edges[i+1] - 1)"
				cb  = report === nothing ? nothing :
				      (tt, dn) -> (seen[i] = dn; report(sum(seen), total))
				open(parts[i], "w") do io
					Downloads.request(url; output = io, headers = ["Range" => rng],
					                  timeout = 1800, downloader = _oc_downloader(), progress = cb)
				end
			end
		end
		got = sum(filesize, parts)
		if got != total
			foreach(p -> rm(p, force = true), parts)
			error("Ocean Color: chunked download got $got of $total bytes")
		end
		open(f * ".part", "w") do io
			for p in parts
				write(io, read(p))
				rm(p, force = true)
			end
		end
		_oc_is_data_file(f * ".part") || (rm(f * ".part", force = true); return "")
		mv(f * ".part", f, force = true)
		return f
	catch err
		# Cancel is a decision, not a fault: clean up quietly, no error report.
		isa(err, OCCancelled) || @error "Ocean Color: data download failed" url exception = (err, catch_backtrace())
		rm(f * ".part", force = true)
		for i in 1:k; rm("$f.part$i", force = true); end
		return ""
	end
end

"""
    oc_png_georef(prod) -> NamedTuple

Georeferencing of a browse image of `prod`: the L3 grid is global plate carrée, pixel-registered,
with a plain `360/nx` degree cell. Handed to the image builders so a preview tile can be promoted
into a window as a real map rather than a bare picture.
"""
function oc_png_georef(prod::OCProduct)
	nx, ny = _oc_png_size(prod.res)
	inc = 360.0 / nx
	(region = _OC_PNG_REGION, nx = nx, ny = ny, inc = inc, registration = 1, geographic = true)
end

# ------------------------------------------------------------------------------------------------
# The dialog bridge (OceanColorDialog, 70_window.cpp)
#
# One callback serves every request the browser can make. The answer is not a return value: the two
# preview tiles and the status line are filled by calling straight back into the live dialog while
# this call is still on its stack (`gmtvtk_oc_set_tile` / `gmtvtk_oc_status`), which is the same
# shape the Tiles Tool picker uses.

_oc_set_tile(dlg::Ptr{Cvoid}, i::Int, png::AbstractString, caption::AbstractString,
             url::AbstractString, ymd::AbstractString) =
	ccall(_fn(:gmtvtk_oc_set_tile), Cvoid, (Ptr{Cvoid}, Cint, Cstring, Cstring, Cstring, Cstring),
	      dlg, Cint(i), String(png), String(caption), String(url), String(ymd))

_oc_status(dlg::Ptr{Cvoid}, msg::AbstractString) =
	ccall(_fn(:gmtvtk_oc_status), Cvoid, (Ptr{Cvoid}, Cstring), dlg, String(msg))

_oc_cache_info(dlg::Ptr{Cvoid}, msg::AbstractString) =
	ccall(_fn(:gmtvtk_oc_cache_info), Cvoid, (Ptr{Cvoid}, Cstring), dlg, String(msg))

"""
    _oc_cache_line() -> String

The cache directory and what it currently holds, for the dialog's cache line: the FULL path (the
user has to be able to go and empty it) plus its size in MB, counted over the files actually on
disk — never a running total this code keeps, which would drift the moment anything else wrote
there. A directory that does not exist yet says so instead of reporting 0 MB.
"""
function _oc_cache_line()
	dir = _oc_cache_dir()
	isdir(dir) || return string("Cache: ", dir, "  (empty — nothing downloaded yet)")
	n = 0; tot = 0
	for (root, _, files) in walkdir(dir), f in files
		tot += try filesize(joinpath(root, f)) catch; 0 end
		n += 1
	end
	return string("Cache: ", dir, "  —  ", round(tot / 1048576, digits = 1), " MB in ", n,
	              n == 1 ? " file" : " files")
end

# Refresh the dialog's cache line. Called after every request, because every request can have changed
# it (a browse caches PNGs, a download caches a grid) — one push, no bookkeeping to keep in sync.
_oc_push_cache(dlg::Ptr{Cvoid}) = (dlg != C_NULL && _oc_cache_info(dlg, _oc_cache_line()); nothing)

# "key=value" block -> Dict. Values may hold '=' (a URL query would), so only the FIRST one splits.
function _oc_params(s::AbstractString)
	d = Dict{String,String}()
	for ln in split(s, '\n')
		i = findfirst('=', ln)
		i === nothing && continue
		d[strip(ln[1:i-1])] = strip(ln[i+1:end])
	end
	return d
end

_oc_ymd(d::Dates.Date) = Dates.format(d, "yyyymmdd")

# What goes under a tile: the day for DAY, the closed span for every composite.
_oc_caption(period::String, a::Dates.Date, b::Dates.Date) =
	period == "DAY" ? string(a) : string(a, "  →  ", b)

"""
    _oc_pair(inst, prod, period, anchor) -> Vector

The two-tile pair anchored at the composite starting on `anchor`: `[older, newer]`, newer = the
anchor itself. Each entry carries its own start day even when the image does not exist, so `<`/`>`
can keep walking across a gap in the archive instead of dead-ending on it.
"""
function _oc_pair(inst::OCInstrument, prod::OCProduct, period::String, anchor::Dates.Date)
	older = _oc_period_prev(period, anchor)
	map((older, anchor)) do d
		a, b = _oc_period_span(period, d)
		(start = a, stop = b, url = _oc_resolve(inst, prod, period, d))
	end |> collect
end

# Push a [older, newer] pair into the dialog's two tiles and summarise it on the status line.
function _oc_fill(dlg::Ptr{Cvoid}, inst::OCInstrument, prod::OCProduct, period::String, pair)
	shown = 0
	for i in 0:1
		if i + 1 > length(pair)
			_oc_set_tile(dlg, i, "", "—", "", "")
			continue
		end
		e = pair[i+1]
		cap = _oc_caption(period, e.start, e.stop)
		if isempty(e.url)
			_oc_set_tile(dlg, i, "", cap * "   (not available)", "", _oc_ymd(e.start))
		else
			_oc_set_tile(dlg, i, oc_fetch_png(e.url), cap, e.url, _oc_ymd(e.start))
			shown += 1
		end
	end
	label = string(inst.label, " · ", prod.suite, " · ", prod.res)
	if shown == 0
		_oc_status(dlg, label * " — nothing on the server for this selection")
	else
		_oc_status(dlg, string(label, " — ", shown, shown == 1 ? " image" : " images"))
	end
	return shown
end

"""
    _on_oceancolor(scene, dlg, cparams) -> Cint

C callback for the Ocean Color Data Browser. `cparams` is the "key=value" block documented in
30_app.cpp. Returns 1 when the request was served, 0 on failure (the dialog reports it, because the
Errors console may be on a window the user is not looking at).
"""
function _on_oceancolor(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		p      = _oc_params(unsafe_string(cparams))
		inst   = _oc_instrument(parse(Int, get(p, "inst", "1")))
		# A product this instrument does not fly is snapped to one it does, and the dialog is told which
		# rows are real so the greyed-out ones cannot be picked again. The catalogue is the ONLY place
		# that knows this; the dialog holds no catalogue of its own (that is the whole design here).
		iprod  = _oc_clamp_product(inst, parse(Int, get(p, "prod", "1")))
		prod   = _oc_product(iprod)
		period = _oc_period(parse(Int, get(p, "period", "1")))
		req    = get(p, "req", "latest")
		_oc_push_products(dlg, inst, iprod)

		# One exit, so the cache line is refreshed after EVERY request and nothing has to remember to do
		# it: browsing caches PNGs, extracting caches a grid — both change what the line reports.
		rc = if     req == "open"  ; _oc_open(scene, dlg, get(p, "url", ""))
		     elseif req == "grid"  ; _oc_grid(scene, dlg, get(p, "url", ""))
		     elseif req == "place" ; _oc_place(scene, dlg, get(p, "file", ""), get(p, "region", ""))
		     else                   ; _oc_browse(dlg, inst, prod, period, req, p)
		     end
		_oc_push_cache(dlg)
		return rc
	catch err
		@error "Ocean Color browser failed" exception = (err, catch_backtrace())
		dlg != C_NULL && _oc_status(dlg, "Failed — see this window's Errors console.")
		return Cint(0)
	end
end

# The BROWSE half: walk the catalogue to the requested pair of composites and push them into the two
# preview tiles. Split out of `_on_oceancolor` only so that function has one exit.
function _oc_browse(dlg::Ptr{Cvoid}, inst::OCInstrument, prod::OCProduct,
                    period::String, req::String, p::Dict{String,String})::Cint
	pair =
		if req == "latest"
			# Newest-first from the scan; the tiles want [older, newer].
			reverse(oc_latest(inst, prod, period; n = 2))
		elseif req == "at"
			d = Dates.Date(get(p, "date", _oc_ymd(Dates.today())), "yyyymmdd")
			_oc_pair(inst, prod, period, _oc_period_span(period, d)[1])
		elseif req == "step"
			d0  = Dates.Date(get(p, "start", _oc_ymd(Dates.today())), "yyyymmdd")
			dir = parse(Int, get(p, "dir", "-1"))
			# One composite away from where the user is, in the direction of the arrow. Forward is
			# a span-snap of "the day after this period ends"; backward is the calendar's own step.
			anchor = dir < 0 ? _oc_period_prev(period, d0) :
			                   _oc_period_span(period, _oc_period_span(period, d0)[2] + Dates.Day(1))[1]
			anchor < inst.first && (anchor = _oc_period_span(period, inst.first)[1])
			_oc_pair(inst, prod, period, anchor)
		else
			error("Ocean Color: unknown request '$req'")
		end

	return Cint(_oc_fill(dlg, inst, prod, period, pair) > 0 ? 1 : 0)
end

# Extract: the browse image into the window as a real, georeferenced map. It goes in through
# `_place_image_in_window` — the SAME builder a dropped image file uses — so it gets its own axes
# and its own properties menu with no second code path (SACRED_LAW.md raster-own-axes law).
function _oc_open(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, url::AbstractString)
	isempty(url) && (_oc_status(dlg, "Nothing to extract."); return Cint(0))
	scene == C_NULL && (_oc_status(dlg, "Extract: no window to put it in."); return Cint(0))
	f = oc_fetch_png(url)
	isempty(f) && (_oc_status(dlg, "Could not download that image."); return Cint(0))
	nc0  = _oc_basename_of(url)
	dup  = _oc_already_shown(scene, dlg, f, :image, replace(nc0, r"\.nc$" => ""))
	dup === nothing || return dup

	I = gmtread(f)
	W, E, S, N = _OC_PNG_REGION
	_georef_image!(I, W, E, S, N)		# range + x + y + inc + registration, never .range alone
	nc   = nc0									# the L3 netCDF name; the image is the L4 of the same name
	name = replace(nc, r"\.nc$" => "")
	_place_image_in_window(scene, I, name; geographic = true)
	# Pin the "Download grid" button under this image. The button is what turns the L4 picture into
	# the L3 data, and it carries the address with it so nothing has to re-derive it later.
	ccall(_fn(:gmtvtk_oc_attach_grid_button_h), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cstring),
	      scene, name, oc_data_url(nc), "Download the L3 grid behind this image:\n" * nc)
	_mark_file_open(f, scene, name)			# a second Extract of this tile now finds it, never re-plots it
	_oc_status(dlg, "Placed " * name)
	return Cint(1)
end

# ------------------------------------------------------------------------------------------------
# NEVER THE SAME THING TWICE
#
# Extracting the same tile again — or downloading a grid whose file is already in the cache and
# already on screen — must not stack a second copy of it in the window. This is the SAME rule a
# re-dropped file follows (`_open_window_for` / `_mark_file_open`, dispatch.jl): the window already
# showing a path is raised and the open is dropped. There is no second duplicate rule here.
#
# Returns `nothing` when the caller should go ahead and plot; a Cint when it must return that instead.
function _oc_already_shown(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, path::AbstractString,
                           kind::Symbol, name::AbstractString)
	h = _open_window_for(path)					# raises the window that has it, if any
	h == C_NULL && return nothing
	if h == scene
		# It is in THIS window. If the user had unchecked it, "already displayed" while nothing is on
		# screen would be a lie — so put it back through the ONE transition a new element goes through
		# (`_adopt_new_element`): checked, everything else off, axes framed to it. Never a second copy.
		prev = _find_object_exact(scene, kind, name)
		prev === nothing || _adopt_new_element(scene, name, prev)
		_oc_status(dlg, "Already in this window — " * name)
	else
		_oc_status(dlg, "Already open in another window — " * name)
	end
	return Cint(1)
end

# Extract / Download, and the "Download grid" button pinned under a placed browse image: fetch the L3
# netCDF and open it in the SAME window as a grid.
#
# `url` may be either address — the browse image (what the dialog's tiles carry) or the netCDF itself
# (what the pinned button carries). They differ by a trailing ".png" and nothing else, so this
# normalises rather than asking the caller to know which one it holds.
#
# When the data file cannot be had — the usual reason is that OB.DAAC keeps it behind Earthdata URS
# and no ~/.netrc entry exists — the browse IMAGE is placed instead, so the button always leaves
# something on screen. That fallback only makes sense from a browse address; the pinned button is
# already standing on the image it would place.
function _oc_grid(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, url::AbstractString)
	f = _oc_fetch_grid(scene, dlg, url)
	isempty(f) && return Cint(0)
	println("Ocean Color: opening ", f)
	# NOT opened from here. This function runs inside the button's clicked() handler, so the Qt loop is
	# mid-event; an open path's own async progress dialog would go up inside that blocked turn and never
	# get the idle turn it needs to close again. Queue it instead — C++ fires `req=place` back at us from
	# a clean turn.
	ccall(_fn(:gmtvtk_oc_queue_place), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), scene, f, "")
	_oc_status(dlg, "Opening " * basename(f))
	return Cint(1)
end

"""
    _oc_fetch_grid(scene, dlg, url) -> String

The L3 netCDF on disk, downloading it if it is not cached yet. `""` when it could not be had (no
login, cancelled, or the server did not return a netCDF) — the reason is reported here, so a caller
only has to check for the empty string. Shared by the Extract button and the draw-a-rectangle path,
which differ only in how much of the file they then READ.
"""
function _oc_fetch_grid(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, url::AbstractString)::String
	isempty(url) && (_oc_status(dlg, "No grid address for this image."); return "")
	scene == C_NULL && (_oc_status(dlg, "No window to put the grid in."); return "")
	ispng = endswith(lowercase(String(url)), ".png")
	url   = ispng ? oc_data_url_of_png(url) : String(url)
	nm    = basename(url)
	# The login comes FIRST — before a byte is asked for. Without it the server answers with its login
	# page, which used to be saved into the cache under the grid's own name. Cancel means the user
	# declined to log in, which is an answer, not a failure.
	if !_oc_ensure_login(scene)
		_oc_status(dlg, "Not downloaded — an Earthdata login is needed for the data files.")
		return ""
	end
	println("Ocean Color: downloading ", nm)
	# The bar goes up BEFORE the first byte: a request that is still resolving/authenticating must not
	# look like a hang either. `_oc_prog_set` returning true = the user pressed Cancel.
	_oc_prog_begin(scene, "Downloading " * nm)
	f = try
		oc_download_data(url; progress = (done, total) -> (_oc_prog_set(nm, done, total) && throw(OCCancelled())))
	finally
		_oc_prog_end()
	end
	if isempty(f)
		# A login exists (checked above) and the transfer still did not produce a netCDF: either the
		# credentials are wrong/expired — URS answers a login page, which `_oc_is_data_file` rejects and
		# which is NOT left in the cache — or the transfer itself failed. Say which file to fix.
		@error """Ocean Color: could not download the L3 grid.
		          $(url)
		          The server did not return a netCDF. If the login in $(_oc_netrc()) is wrong or expired,
		          Earthdata answers with its login page instead of the file. Correct (or delete) the
		          `machine $(_OC_URS_HOST)` line there and try again."""
		_oc_status(dlg, "Download failed — check the Earthdata login in " * _oc_netrc())
		return ""
	end
	return f
end

"""
    _oc_region(scene, rect, url) -> Cint

"Load only this region" on a rectangle drawn over an Ocean Color browse image: fetch the L3 file and
read ONLY the drawn box out of it.

`rect` is "w/e/s/n" in data coordinates — the same string, from the same rectangle bounding box, that
Roi Crop Tools already builds (rectRoiCrop, 55_lineprops.cpp). One rectangle convention, not a
second.

Note what is and is not saved. The FILE still comes down whole: OB.DAAC's server refuses HTTP range
requests (`416 Requested Range Not Satisfiable`, verified 2026-08-03 against a live 8.7 MB browse
image that advertises no `Accept-Ranges`), so no partial fetch is possible against it from any
client. What this avoids is READING and holding the whole global grid: `grdcut` pulls the box
straight off disk — measured 0.03 s / 240x240 against 0.63 s / 4320x8640 for the same file. If a
ranged endpoint ever appears (the Earthdata Cloud S3 mirror does serve ranges), only
`_oc_fetch_grid` has to change; this function is already asking for exactly one box.
"""
function _oc_region(scene::Ptr{Cvoid}, rect::AbstractString, url::AbstractString)::Cint
	parts = split(String(rect), '/')
	length(parts) == 4 || (_viewer_log_error(scene, "Ocean Color: malformed rectangle '$rect'"); return Cint(0))
	w, e, s, n = parse.(Float64, parts)
	(e > w && n > s) || (_viewer_log_error(scene, "Ocean Color: empty rectangle"); return Cint(0))
	f = _oc_fetch_grid(scene, C_NULL, url)
	isempty(f) && return Cint(0)
	ccall(_fn(:gmtvtk_oc_queue_place), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), scene, f,
	      string(w, '/', e, '/', s, '/', n))
	return Cint(1)
end

# ------------------------------------------------------------------------------------------------
# Opening a downloaded L3m file — WITHOUT the variable picker
#
# An OB.DAAC L3m file is not an anonymous netCDF: it holds the product variable (`sst`, `chlor_a`),
# a per-pixel quality flag of the same shape (`qual_sst` …) and the product's own 256-colour lookup
# table (`palette`). Which one the user wants is therefore KNOWN, so the multi-variable picker that a
# plain dropped netCDF gets must never appear here — and the colours are the file's own, not this
# viewer's default ramp.

# "w/e/s/n" -> (w,e,s,n), or `nothing` for "" (the whole grid) and for anything malformed/empty.
function _oc_parse_region(r::AbstractString)
	isempty(r) && return nothing
	p = split(String(r), '/')
	length(p) == 4 || return nothing
	v = try parse.(Float64, p) catch; return nothing end
	return (v[2] > v[1] && v[4] > v[3]) ? (v[1], v[2], v[3], v[4]) : nothing
end

# The variable to display: the FIRST real one — never a `qual_*` mask, never the palette itself, and
# never a 1-D coordinate vector. Empty when the file has no subdataset list at all (a plain
# single-variable grid), which the caller reads as "open the file as it is".
function _oc_pick_var(vars)::String
	for v in vars
		startswith(v.name, "qual_") && continue
		v.name == "palette" && continue
		length(v.dims) >= 2 || continue
		return v.name
	end
	return ""
end

# The file's OWN colour table, in the byte order the FILE stores it. `palette` is declared
# `(3, 256)`: THREE 256-byte rows whose concatenation, in netCDF (row-major) order, is the 256 RGB
# TRIPLETS r,g,b, r,g,b, … Read through GMT and NEVER through GDAL: GDAL's netCDF driver hands those
# three rows over as an IMAGE, and an image is bottom-up, so `gdalread` returns them REVERSED — the
# same 768 bytes rotated by 512, and 512 % 3 == 2, so the rotation is not even triplet-aligned and
# every entry came out built from three unrelated channels of three unrelated entries. That is what a
# "de-interleaved" read of the gdalread array still produced. Verified byte-for-byte against the
# browse PNG's own embedded colormap (the palette NASA renders the picture with) for a CHL and an SST
# file: identical, 0 difference, over all 256 entries.
# Returns the entries as n x 3 in 0..1, or `nothing`.
function _oc_read_palette(file::AbstractString)
	M = try
		P = GMT.gmtread(string(file, "?palette"))
		A = P isa GMTgrid ? P.z : (P isa GMTimage ? P.image : P)
		A isa AbstractArray ? Array(A) : nothing
	catch
		nothing
	end
	(M === nothing || ndims(M) != 2 || size(M, 1) != 3 || size(M, 2) < 2) && return nothing
	S = vec(permutedims(M))                  # row-major = netCDF order = the interleaved triplet stream
	n = div(length(S), 3)
	n < 2 && return nothing
	rgb = Float64.(permutedims(reshape(S[1:3n], 3, n))) ./ 255
	# The LAST entry is not part of the ramp: every L3m palette reserves it (black) for land/masked
	# pixels. Keeping it would end the colour ramp on black.
	(n > 2 && all(iszero, rgb[end, :]) && !all(iszero, rgb[end-1, :])) && (rgb = rgb[1:end-1, :])
	return rgb
end

# The scaling NASA renders this product with, carried by the file itself as the global attributes
# `suggested_image_scaling_{type,minimum,maximum}` — LOG between 0.01 and 20 mg/m3 for chlorophyll,
# LINEAR between -2 and 45 degC for SST. Spreading the palette EVENLY over the data's raw extrema
# instead is the other half of "the colours are wrong": chlor_a runs to ~100 mg/m3, so on an even
# ramp the entire ocean lives in the first two or three entries. Read off the tiny `palette`
# subdataset, so no raster is touched. Returns (lo, hi, islog), or `nothing` when the file says
# nothing usable.
function _oc_scaling(file::AbstractString)
	txt = try GMT.gdalinfo("NETCDF:\"$(file)\":palette") catch; "" end
	isempty(txt) && return nothing
	att(k) = (m = match(Regex("suggested_image_scaling_$(k)=(.*)"), txt); m === nothing ? "" : String(strip(m.captures[1])))
	lo = tryparse(Float64, att("minimum"))
	hi = tryparse(Float64, att("maximum"))
	(lo === nothing || hi === nothing || !(hi > lo)) && return nothing
	return (lo, hi, uppercase(att("type")) == "LOG")
end

"""
    _oc_palette_cpt(file, zmn, zmx) -> GMTcpt or nothing

The file's OWN colour table as a CPT spanning [zmn, zmx]. L3m files carry it as a `palette` variable
of 3x256 bytes — 256 RGB TRIPLETS, r,g,b, r,g,b, … — which is the palette NASA renders the browse
images with, so a grid opened here looks like the tile it was picked from. `nothing` when the file
has no palette, which leaves the caller on the viewer's ordinary default colormap.

The CPT always SPANS [zmn, zmx] — the grid's own range — so the colour bar stays linear in z and its
numbers keep meaning what they say. The file's `suggested_image_scaling_*` only decides WHERE inside
that span the entries fall: log-bunched near the bottom for chlorophyll, evenly for SST, with the
ends clamped to the first/last colour, which is exactly what the browse image does.
"""
function _oc_palette_cpt(file::AbstractString, zmn::Real, zmx::Real)
	rgb = _oc_read_palette(file)
	if (rgb === nothing)
		@warn "Ocean Color: no readable `palette` variable in $(basename(file)) — falling back to the viewer's default colormap"
		return nothing
	end
	zmn, zmx = Float64(zmn), Float64(zmx)
	!(zmx > zmn) && return nothing
	n  = size(rgb, 1)
	sc = _oc_scaling(file)
	z  = collect(range(zmn, zmx, length = n))             # no recipe in the file -> the even spread
	if (sc !== nothing)
		lo, hi, islog = sc
		islog && (lo <= 0) && (islog = false)             # a log ramp needs a positive bottom
		t  = range(0.0, 1.0, length = n)
		# The file's mapping is ABSOLUTE — SST is -2..45 degC whether or not today's grid reaches
		# either end — so it is never re-stretched onto the data's extrema: a grid that spans a third
		# of the scale must use a third of the palette, or it would not look like the browse picture
		# it was picked from. The nodes outside the grid's own range are dropped and the two boundary
		# nodes take the entry the file's own scale puts there, which is the same clamping the browse
		# image does.
		zz   = collect(islog ? exp10.(log10(lo) .+ t .* (log10(hi) - log10(lo))) : lo .+ t .* (hi - lo))
		keep = findall(v -> zmn < v < zmx, zz)
		if (!isempty(keep))
			lo_i = max(1, first(keep) - 1)                 # entry the scale puts at/below the grid's floor
			hi_i = min(n, last(keep) + 1)                  # …and at/above its ceiling
			z   = vcat(zmn, zz[keep], zmx)
			rgb = vcat(rgb[lo_i:lo_i, :], rgb[keep, :], rgb[hi_i:hi_i, :])
			n   = size(rgb, 1)
		end
	end
	z = accumulate(max, z)                                # strictly increasing, whatever the rounding did
	for i in 2:n
		z[i] <= z[i-1] && (z[i] = nextfloat(z[i-1]))
	end
	# A GMTcpt is n-1 slices: colour i runs from z[i] to z[i+1], and the slice's far end takes the next
	# colour so the ramp is continuous rather than n flat steps. `_cpt_nodes_of` (cpt.jl) reads these z
	# back — the log bunching only survives because it does.
	m   = n - 1
	col = rgb[1:m, :]
	far = rgb[2:n, :]
	return GMTcpt(col, zeros(m), hcat(z[1:m], z[2:n]), [zmn, zmx],
	              [1.0 1.0 1.0; 0.0 0.0 0.0; 0.5 0.5 0.5], Cint(24), NaN, hcat(col, far),
	              0, String[], String[], "rgb", String[])
end

# Open the downloaded L3 file `file` in `scene`: its first real variable, coloured with its own
# palette. Goes in through `_add_grid_to_scene` (the ONE grid builder every other load path uses) and
# is adopted by `_adopt_new_element` (the ONE new-file transition), so a downloaded grid behaves
# exactly like a dropped one — its own axes, its own frame (SACRED_LAW.md).
function _oc_place(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, file::AbstractString, region::AbstractString = "")
	(isempty(file) || !isfile(file)) && (_oc_status(dlg, "Nothing to open."); return Cint(0))
	scene == C_NULL && (_oc_status(dlg, "No window to put the grid in."); return Cint(0))
	reg = _oc_parse_region(region)
	v    = _oc_pick_var(_netcdf_subdatasets(file))
	spec = isempty(v) ? String(file) : string(file, '?', v)
	name = replace(basename(file), r"\.nc$" => "")
	if reg === nothing
		# The download is cached, so pressing Extract twice arrives here twice with the same file. It is
		# plotted ONCE. Same rule as a re-dropped file, and the same one the browse image above follows.
		dup = _oc_already_shown(scene, dlg, file, :grid, name)
		dup === nothing || return dup
	else
		# A REGION is a different variable from the whole grid — its own name, its own axes — so the
		# whole-file duplicate rule does not apply to it. Each box drawn is a result of its own, so they
		# are numbered: the next free number, never a name that would replace a box already on screen.
		base = name * " - subregion "
		k = 1
		while _find_object_exact(scene, :grid, base * string(k)) !== nothing; k += 1; end
		name = base * string(k)
	end
	# Only the drawn box is READ: `grdcut` pulls it straight off disk instead of materialising the whole
	# global grid and throwing most of it away.
	G    = reg === nothing ? _gmtread_trb(spec) : GMT.grdcut(spec, region = reg)
	fin  = filter(isfinite, G.z)
	cmap = isempty(fin) ? :auto : something(_oc_palette_cpt(file, extrema(fin)...), :auto)
	empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
	_add_grid_to_scene(scene, G, name; cmap = cmap, promote = empty, source = spec) || return Cint(0)
	_record_recent(String(file), G)
	_adopt_new_element(scene, name, G)
	# Only the WHOLE file claims the path: a region is one box out of it, and claiming the file for a box
	# would make the next Extract (or the next box) think the file was already fully displayed.
	reg === nothing && _mark_file_open(String(file), scene, name)
	empty && ccall(_fn(:gmtvtk_set_title_h), Cvoid, (Ptr{Cvoid}, Cstring), scene, "i'GMT -- " * basename(file))
	_oc_status(dlg, "Opened " * name * (isempty(v) ? "" : "  ·  " * v))
	return Cint(1)
end

# Warm-up body (src/warmup.jl): fired when the dialog opens, so the first server answer does not also
# pay the JIT for the catalogue + URL machinery. Deliberately OFFLINE — the network is what the
# dialog is about to do for real, and doing it twice would only slow it down.
function _oc_warm()
	d = Dates.Date(2026, 7, 21)
	for period in ("DAY", "8D", "MO", "YR")
		_oc_period_span(period, d)
		_oc_period_prev(period, _oc_period_span(period, d)[1])
		_oc_png_urls(_OC_INSTRUMENTS[1], _OC_PRODUCTS[1], period, d)
		yield()
	end
	oc_png_georef(_OC_PRODUCTS[1])
	_oc_cache_line()						# the cache line is drawn the moment the dialog opens
	precompile(_on_oceancolor, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	precompile(_oc_resolve, (OCInstrument, OCProduct, String, Dates.Date))
	precompile(_oc_open, (Ptr{Cvoid}, Ptr{Cvoid}, String))
	precompile(_oc_grid, (Ptr{Cvoid}, Ptr{Cvoid}, String))
	precompile(_oc_place, (Ptr{Cvoid}, Ptr{Cvoid}, String))
	return nothing
end

function _register_oceancolor()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_oceancolor, s, d, c)::Cint,
	                  Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_oceancolor_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("oceancolor", _oc_warm)		# C++ fires this when the dialog opens (70_window.cpp)
	return
end
