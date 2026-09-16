# ecv.jl — Satellite > Copernicus > "Essential Climate Variables…": the CDS collection
# `ecv-for-climate-change`, which the ERA5 dialog cannot express.
#
# WHY A SECOND DIALOG AND NOT A SECOND DATASET IN THE FIRST. The ERA5 dialog builds its request out
# of `GMT.era5vars` + `GMT.era5time` and GMT.jl stamps `"product_type": ["reanalysis"]` into it
# (weather.jl). This collection's request has FOUR inputs that shape simply does not have — `origin`,
# `product_type` (anomaly/climatology/monthly_mean), `climate_reference_period`, `time_aggregation` —
# and no `area` at all. Adding its name to the ERA5 dialog's dataset combo would advertise support
# the builder has not got: the request would be assembled in ERA5's shape and the server would
# reject it, which is exactly the "invalid request" this tool exists to stop producing.
#
# WHAT IS NOT FORKED. Everything after the JSON body is the ECMWF tool's, called as it stands:
# `_ecmwf_download` (naming, the "already on disk" short-circuit, starting the job), `_ecmwf_job_poll`,
# `_ecmwf_job_finish` (the longitude frame, the unit, and the SHARED file door `_on_drop`) and
# `_ecmwf_reply`. This file contributes the request and nothing else — SACRED_LAW.md: one operation,
# one function. It reaches them by handing `_ecmwf_era5` a ready-made `params` body, which is the
# same door a pasted CDS snippet goes through.
#
# The enums below are this collection's own, read from the server's process description
# (`GET {url}/retrieve/v1/processes/ecv-for-climate-change`, 3.8 KB, no credentials needed). They are
# baked rather than fetched so that opening the dialog costs nothing: the list a user picks from must
# appear instantly, not after a round trip.

const _ECV_DATASET  = "ecv-for-climate-change"

const _ECV_VARS     = ["sea_surface_temperature", "surface_air_temperature",
                       "surface_air_relative_humidity", "precipitation", "sea_ice_cover",
                       "0_7cm_volumetric_soil_moisture"]
const _ECV_ORIGIN   = ["era5", "era5_land"]
const _ECV_PRODUCT  = ["anomaly", "climatology", "monthly_mean"]
const _ECV_REFPER   = ["1991_2020", "1981_2010"]
const _ECV_TIMEAGG  = ["1_month_mean", "12_month_running_mean", "seasonal_mean"]
const _ECV_FORMAT   = ["netcdf4", "grib"]
const _ECV_YEAR1    = 1979                 # the collection's first year, per its own schema

# The catalogue the dialog fills its widgets from: one "key\tvalue,value,…" line per input. The C++
# side never carries a copy of these lists (a second table would drift from this one).
function _ecv_catalog()::String
	io = IOBuffer()
	for (k, v) in (("variable", _ECV_VARS), ("origin", _ECV_ORIGIN), ("product_type", _ECV_PRODUCT),
	               ("climate_reference_period", _ECV_REFPER), ("time_aggregation", _ECV_TIMEAGG),
	               ("data_format", _ECV_FORMAT))
		println(io, k, '\t', join(v, ','))
	end
	return String(take!(io))
end

# "2025" / "2024,2025" / "2020:2026" -> the expanded list of years, as the strings the request takes.
# The range syntax is `_ecmwf_range`, the SAME one the ERA5 dialog's boxes use, so a range means the
# same thing in both tools.
function _ecv_years(s::String)::Vector{String}
	t = strip(s)
	isempty(t) && error("give me at least one year (1979-$(GMT.Dates.year(GMT.Dates.today())))")
	out = String[]
	for part in split(t, (',', ';'); keepempty = false)
		p = String(strip(part))
		occursin(':', p) ? append!(out, _ecmwf_range(p)) : push!(out, p)
	end
	out = unique(out)
	for y in out
		n = tryparse(Int, y)
		(n === nothing || n < _ECV_YEAR1) && error("'$y' is not a year this collection covers (from $_ECV_YEAR1)")
	end
	return out
end

# The Months box -> two-digit month strings. Empty means all twelve, which is what the CDS form's own
# "select all" does; a range ("1:11") goes through the shared `_ecmwf_range` and is padded here.
function _ecv_months(s::String)::Vector{String}
	t = strip(s)
	isempty(t) && return [lpad(m, 2, '0') for m in 1:12]
	out = String[]
	for part in split(t, (',', ';'); keepempty = false)
		p = String(strip(part))
		vals = occursin(':', p) ? _ecmwf_range(p) : [p]
		for v in vals
			n = tryparse(Int, v)
			(n === nothing || n < 1 || n > 12) && error("'$v' is not a month")
			push!(out, lpad(n, 2, '0'))
		end
	end
	return sort!(unique(out))
end

# The request body, as the JSON object CDS expects under "inputs". Values come from the enums above,
# so nothing here needs escaping — but a value that is not in its enum is refused BEFORE the post,
# because the server's answer to one ("invalid request") names neither the input nor the value.
function _ecv_body(d::Dict{String,String})::String
	pick(key, allowed, what) = begin
		v = String(strip(_get(d, key)))
		isempty(v) && error("choose a $what")
		v in allowed || error("'$v' is not a valid $what for $_ECV_DATASET")
		v
	end
	vars = _ecmwf_varlist(_get(d, "vars"))
	isempty(vars) && error("pick at least one variable")
	for v in vars
		v in _ECV_VARS || error("'$v' is not a variable of $_ECV_DATASET")
	end
	product = pick("product", _ECV_PRODUCT, "product type")
	fmt     = pick("format",  _ECV_FORMAT,  "data format")
	jlist(v) = '[' * join(("\"" * x * "\"" for x in v), ',') * ']'
	kv = String["\"variable\": " * jlist(vars),
	            "\"origin\": "   * jlist([pick("origin", _ECV_ORIGIN, "origin")]),
	            "\"product_type\": " * jlist([product])]
	# The reference period only means something for a departure from it. Sending it with a plain
	# monthly mean is not an error, but it is noise in a request the user may read back.
	product == "monthly_mean" ||
		push!(kv, "\"climate_reference_period\": " * jlist([pick("refperiod", _ECV_REFPER, "reference period")]))
	push!(kv, "\"time_aggregation\": " * jlist([pick("timeagg", _ECV_TIMEAGG, "time aggregation")]))
	push!(kv, "\"year\": "  * jlist(_ecv_years(_get(d, "years"))))
	push!(kv, "\"month\": " * jlist(_ecv_months(_get(d, "months"))))
	push!(kv, "\"data_format\": \"" * fmt * "\"")
	return "{" * join(kv, ", ") * "}"
end

# What the dialog sends: the same "key=value" block every tool uses, answered in the same way.
function _on_ecv(scene::Ptr{Cvoid}, cparams::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		d    = _nswing_parse(unsafe_string(cparams))
		what = _get(d, "what", "download")
		what == "catalog" && (_ecmwf_reply(out, cap, _ecv_catalog()); return Cint(1))
		what == "destdir" && (_ecmwf_reply(out, cap, _ecmwf_tmpdir()); return Cint(1))
		what == "poll"    && (_ecmwf_reply(out, cap, _ecmwf_job_poll()); return Cint(1))
		what == "finish"  && return Base.invokelatest(_ecmwf_job_finish, scene, out, cap)::Cint
		return Base.invokelatest(_ecv_run, scene, d, out, cap)::Cint
	catch e
		_tool_failed(scene, "Copernicus / ECV", e)
		_ecmwf_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# Build the body, then hand the whole thing to the ECMWF tool's own download path. `params` is what
# makes that work without a second implementation: `_ecmwf_era5` posts a ready-made body as it
# stands, which is the pasted-request door, and `dataset` decides the endpoint it goes to.
function _ecv_run(scene::Ptr{Cvoid}, d::Dict{String,String}, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		body = _ecv_body(d)
		if _get(d, "what") == "dryrun"
			_ecmwf_reply(out, cap, "POST " * _ECV_DATASET * "\n\n" * body)
			return Cint(1)
		end
		fmt    = _get(d, "format", "netcdf4")
		outbox = String(strip(_get(d, "out")))
		dir    = _ecmwf_destdir(outbox)
		isdir(dir) || error("the output directory does not exist: $dir")
		# The keys the ECMWF path reads out of `d`: the body to post, the endpoint to post it to, and
		# the extension `_ecmwf_autoname` gives an unnamed download. `region` stays empty (this
		# collection is global and has no `area` input), which is also what tells `_ecmwf_job_finish`
		# to put a global grid in the -180/180 frame.
		d["params"]  = body
		d["dataset"] = _ECV_DATASET
		d["region"]  = ""
		d["nameprefix"] = "ECV"          # not "ERA5": this is a different product
		# NO TEMPERATURE CONVERSION, EVER, ON THIS COLLECTION. `_ecmwf_to_unit!` turns kelvin into
		# Celsius by subtracting 273.15, which is right for an ABSOLUTE temperature and nonsense for
		# everything this collection mostly serves: an anomaly and a climatology are DIFFERENCES, and a
		# difference of 0.4 K is a difference of 0.4 °C, not -272.75. The unit test upstream cannot
		# tell the two apart -- both are declared "K" -- so the caller that knows says so here.
		# "K" is `_ecmwf_to_unit!`'s own "leave it as the server sent it".
		d["tunit"]    = "K"
		d["lonframe"] = _get(d, "lonframe", "180")
		# Always shown, so the dialog has no "load when it arrives" box to get wrong. What lands is ONE
		# thing however many files the server packed: an archive is unpacked into its own dated folder
		# and stacked into a .vrt (`_ecmwf_unzip!` / `_ecmwf_vrt`), and that is what opens.
		d["load"] = "1"
		if _get(d, "product", "") == "monthly_mean"
			# The one product here that IS an absolute temperature. Still not converted: a monthly mean
			# of 2025 arrives as a 12-layer cube, and the converter cannot rewrite a cube (see the guard
			# in `_ecmwf_to_unit!`). Saying so plainly beats a silent no-op.
			_viewer_log_info(scene, "Copernicus / ECV: values are kept in the unit the server sent (kelvin for temperatures).")
		end
		return Base.invokelatest(_ecmwf_download, scene, d, "era5", "",
		                         fmt == "grib" ? "grib" : "netcdf", outbox, dir, out, cap)::Cint
	catch e
		_tool_failed(scene, "Copernicus / ECV", e)
		_ecmwf_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# Lazily registered at the first window open (eventloop.jl _ensure_callbacks) -- never at top level:
# a precompiled @cfunction is invalid.
function _register_ecv()
	fptr = @cfunction((s, c, o, n) -> Base.invokelatest(_on_ecv, s, c, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_ecv_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
