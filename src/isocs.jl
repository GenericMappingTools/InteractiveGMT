"""
isocs.jl — parse Mirone's magnetic-isochron `.dat` header line (`data/isocs/*.dat`) and write the
isochron polyline into a SHAPENC file via `shapenc` (shapenc.jl).

Header line format (first line of the file, when present):
```
> NAME KEY1"v1 v2 ..." KEY2"v1 v2 ..." ...
```
e.g.
```
> 5c EURASIA/NORTH AMERICA FIN"136.53 63.63 -3.951 16.37" STG0"147.6332 51.4257 19.60 16.37 0.3362"
```
`NAME` is the isochron/plate-pair name; each `KEY"..."` is a numeric attribute (finite pole, stage
pole, ...) — a space-separated number list with no enclosing quotes once parsed, so it's parsed
into a `Vector{Float64}` and stored as a proper netCDF `double[]` attribute, never as opaque text
(`shapenc`'s `tag` accepts numeric-vector values for exactly this).

Not every file in `data/isocs/` has this header: some start directly on data with NO `>` line at
all (e.g. `C6_muller_AF.dat`), some have a `>` line with just a name and ZERO `KEY"..."` attributes
(e.g. `C20_JM_EU.dat`: `> 20 JAN MAYEN/EURASIA`). `_isoc_read` handles all three shapes.
"""

# KEY"v1 v2 ..." tokens; KEY itself may contain letters/digits/._- (e.g. "STG3_5c-6").
const _ISOC_ATTR_RE = r"([A-Za-z0-9_.-]+)\"([^\"]*)\""

function _isoc_parse_header(line::String)
	s::String = startswith(line, ">") ? String(strip(line[2:end])) : String(strip(line))
	m = findfirst(_ISOC_ATTR_RE, s)
	name = m === nothing ? s : String(strip(SubString(s, 1, first(m) - 1)))
	attrs = Pair{String, Vector{Float64}}[]
	for mm in eachmatch(_ISOC_ATTR_RE, s)
		push!(attrs, String(mm.captures[1]) => parse.(Float64, split(mm.captures[2])))
	end
	(name, attrs)
end

"""
    _isoc_read(fname::String) -> (xy::Matrix{Float64}, name::String, attrs::Vector{Pair{String,Vector{Float64}}})

Read one Mirone isochron `.dat` file: an optional `>`-prefixed header line (see module docstring)
followed by whitespace-separated lon/lat rows. A file with no header line gets
`name = splitext(basename(fname))[1]` and empty `attrs`.
"""
function _isoc_read(fname::String)
	lines = readlines(fname)
	isempty(lines) && error("isocs: empty file $fname")
	has_header = startswith(lines[1], ">")
	name, attrs = has_header ? _isoc_parse_header(lines[1]) : (splitext(basename(fname))[1], Pair{String, Vector{Float64}}[])
	data_lines = has_header ? view(lines, 2:length(lines)) : lines
	xy = Matrix{Float64}(undef, length(data_lines), 2)
	for (i, ln) in enumerate(data_lines)
		toks = split(ln)
		xy[i, 1] = parse(Float64, toks[1])
		xy[i, 2] = parse(Float64, toks[2])
	end
	(xy, name, attrs)
end

"""
    isoc2shapenc(fname_in::String, fname_out::String; append::Bool=false) -> String

Read a Mirone isochron `.dat` file (`fname_in`, `data/isocs/*.dat` layout) and write it into a
SHAPENC netCDF file (`fname_out`) as a 2-D polyline ensemble, via `shapenc`. The parsed isochron
name becomes the ensemble's `name` tag attribute, and each parsed pole (`FIN`, `STG0`, `STG...`)
becomes its own numeric `double[]` tag attribute. Pass `append=true` to add this isochron to an
existing SHAPENC file instead of overwriting it (grouping several isochrons in one file).

### Example
```julia
isoc2shapenc(raw"C:\\SVN\\mironeWC\\data\\isocs\\c5c_EU_NA.dat", "c5c_EU_NA.nc")

# Group several isochrons of the same plate pair into one file:
isoc2shapenc(raw"C:\\SVN\\mironeWC\\data\\isocs\\c13_EU_NA.dat", "EU_NA.nc")
isoc2shapenc(raw"C:\\SVN\\mironeWC\\data\\isocs\\c20_EU_NA.dat", "EU_NA.nc"; append=true)
```
"""
function isoc2shapenc(fname_in::String, fname_out::String; append::Bool = false)
	xy, name, attrs = _isoc_read(fname_in)
	tag = Pair{String, Union{String, Vector{Float64}}}[]
	push!(tag, "name" => name)
	append!(tag, attrs)
	shapenc(fname_out, xy; tag = tag, append = append, polyline2D = true)
end

"""
    isoc2shapenc(; isoc_group::Vector{String}=["", "", ""]) -> String

Batch form: group every isochron `.dat` file belonging to the same plate pair, in one directory,
into ONE SHAPENC file — one ensemble per file.

`isoc_group = [dir, pattern, savedir]` (`savedir` optional):
- `dir`     — directory to search for `.dat` files.
- `pattern` — a plate-pair code `"AA_BB"` (e.g. `"EU_NA"`). A file matches when its name ends in
  `_AA_BB.dat` OR `_BB_AA.dat` — either plate order — so `"EU_NA"` also picks up a
  `..._NA_EU.dat` file, no separate call needed.
- `savedir` — directory the grouped file is written into; `""` (the default, and what an
  `isoc_group` with only 2 elements gets) means the current directory.

Each matching file's own name -- the WHOLE stem, verbatim, exactly as it appears on disk minus the
`.dat` extension (`c5c_EU_NA.dat` -> `"c5c_EU_NA"`, `c13_NA_EU.dat` -> `"c13_NA_EU"`) -- becomes
that ensemble's ACTUAL netCDF variable name (`shapenc`'s `ids`, e.g. `lonc5c_EU_NA`/
`latc5c_EU_NA` — no `Point_`/`Polygon_`/`PolyLine_` type prefix either), not just an attribute — a
real identifier a future tool can look up (or `ncdump -h`/GDAL can show) directly as a variable,
and NEVER shortened/reconstructed: both plate-order files for one isochron number normally exist
(`c13_EU_NA.dat` AND `c13_NA_EU.dat`) and are NOT duplicates — they're two DIFFERENT reconstructed
geometries, one per plate's reference frame — so the file's own full name is what unambiguously
says which one a given ensemble is. Also written as the container-variable `name` tag attribute
for convenience; the fuller descriptive name parsed from the file's own header line
(`_isoc_parse_header`) is kept too, under `full_name`.

A file whose id (the stem with the plate-pair suffix stripped) is longer than 4 characters is NOT
a real isochron (e.g. `dist_age_EU_NA.dat` -> `"dist_age"`, `polos_para_idades_EU_NA.dat` ->
`"polos_para_idades"` — unrelated tables that happen to share the plate-pair filename suffix) and
is SKIPPED with a warning, not written in.

The output file name is `"isochrons_" * pattern * ".nc"` — always `.nc` (input isochron files are
the ones that are normally `.dat`), so it also auto-recognizes as vector data on drag-drop/
File>Open (`_shnc_is_shapenc`, drop.jl) like any other SHAPENC file.

Re-running this on the same directory/pattern REBUILDS the output file from the CURRENT matching
file set (any stale copy is removed first) rather than piling more ensembles onto whatever was
already there. Each isochron is still written through `shapenc`'s ordinary `append=true` path, so
the result is an ordinary, further-appendable SHAPENC/netCDF4 file — a future targeted replace/add
tool can open it back up in update mode; this call just doesn't try to guess which of a re-run's
ensembles should survive.

### Example
```julia
isoc2shapenc(; isoc_group=["C:/SVN/mironeWC/data/isocs", "EU_NA"])
# -> "isochrons_EU_NA.nc" in the current directory, picking up both "..._EU_NA.dat" and
# "..._NA_EU.dat" files, one ensemble per file.
```
"""
function isoc2shapenc(; isoc_group::Vector{String} = ["", "", ""])
	length(isoc_group) >= 2 || error("isoc2shapenc: isoc_group needs at least [dir, pattern]")
	dir     = isoc_group[1]
	pattern = isoc_group[2]
	savedir = length(isoc_group) >= 3 ? isoc_group[3] : ""
	isempty(dir)     && error("isoc2shapenc: isoc_group[1] (directory) is empty")
	isempty(pattern) && error("isoc2shapenc: isoc_group[2] (pattern) is empty")
	isdir(dir) || error("isoc2shapenc: not a directory: $dir")

	parts = split(pattern, '_')
	length(parts) == 2 || error("isoc2shapenc: pattern must be \"AA_BB\" (got \"$pattern\")")
	reversed = join(reverse(parts), "_")
	suffix, rsuffix = "_" * pattern, "_" * reversed

	files = String[]
	for fn in sort(readdir(dir))
		lowercase(splitext(fn)[2]) == ".dat" || continue
		stem = splitext(fn)[1]
		(endswith(stem, suffix) || endswith(stem, rsuffix)) || continue
		push!(files, joinpath(dir, fn))
	end
	isempty(files) && error("isoc2shapenc: no files matching \"$pattern\"/\"$reversed\" found in $dir")

	outpath = isempty(savedir) ? "isochrons_$(pattern).nc" : joinpath(savedir, "isochrons_$(pattern).nc")
	isfile(outpath) && rm(outpath)   # fresh rebuild from the CURRENT matching file set, not a stale one

	# The filename-suffix match above is necessarily loose -- data/isocs/ also holds non-isochron
	# files sharing the same "_AA_BB.dat" naming (e.g. a distance-vs-age table, a pole-sheet with a
	# "#"-comment header, not the "> NAME ..." isochron convention `_isoc_read` expects). Real
	# isochron ids are always short (c0, c13, c33r, ...); a stem longer than 5 chars once the
	# plate-pair suffix is stripped (e.g. "dist_age", "flo") is NOT an isochron name and is SKIPPED
	# (warned) rather than silently written in with a bogus BoundingBox -- this is what was
	# producing "grossly in error" BoundingBoxes before. A file that fails to even PARSE
	# numerically is skipped the same way; over ~391 files, one odd file must not lose every other.
	#
	# The variable name/id used below is the file's stem VERBATIM (e.g. "c13_EU_NA"), never a
	# reconstructed/shortened form -- BOTH plate-order files for the same isochron number normally
	# exist (c13_EU_NA.dat AND c13_NA_EU.dat) and are NOT duplicates, they're two DIFFERENT
	# reconstructed geometries (one per plate's reference frame), so the full file name is what
	# unambiguously identifies which one a given ensemble is.
	wrote_any = false
	for fn in files
		stem = splitext(basename(fn))[1]
		is_fwd = endswith(stem, suffix)
		short_id = is_fwd ? stem[1:end-length(suffix)] : stem[1:end-length(rsuffix)]
		if length(short_id) > 5
			@warn "isoc2shapenc: skipping (isochron id \"$short_id\" longer than 5 chars -- not an isochron file)" file=fn
			continue
		end
		local xy, hdr_name, attrs
		try
			xy, hdr_name, attrs = _isoc_read(fn)
		catch e
			@warn "isoc2shapenc: skipping (does not parse as an isochron file)" file=fn exception=e
			continue
		end
		id = stem   # EXACTLY what's in the file name, verbatim -- not a reconstructed short_id+direction
		tag = Pair{String, Union{String, Vector{Float64}}}["name" => id, "full_name" => hdr_name]
		append!(tag, attrs)
		shapenc(outpath, xy; tag = tag, append = wrote_any, polyline2D = true, ids = [id])
		wrote_any = true
	end
	wrote_any || error("isoc2shapenc: none of the $(length(files)) matched files parsed as an isochron")
	outpath
end

# Fixed 6-decimal string, e.g. 39.1 -> "39.100000" (matches the source isocs/*.dat coordinate
# convention exactly -- round(;digits=6) alone can print fewer decimals when they're trailing
# zeros). No Printf dependency needed for one fixed-width float format.
function _fmt6(x::Float64)::String
	s = string(round(x; digits = 6))
	occursin('.', s) || return s * ".000000"
	ip, dp = split(s, '.')
	ip * "." * rpad(dp, 6, '0')[1:6]
end

"""
    shapenc2isoc(fname_nc::String, isoc_name::String, fname_out::String) -> String

Reverse of `isoc2shapenc`: given a SHAPENC file (single-isochron or grouped, e.g.
`isochrons_EU_NA.nc`) and one isochron's short `name` (the file-stem `???` id `isoc2shapenc`
tagged it with, e.g. `"c13"` — the netCDF variable/ensemble identity), write it back out as a
Mirone-format isochron `.dat`: a `"> "` header line (the isochron/plate-pair name plus its
`KEY"v1 v2 ..."` pole attributes) followed by tab-separated lon/lat rows — exactly the layout
`_isoc_read`/`isoc2shapenc` read on the way in.

Pole attribute ORDER in the rebuilt header is `FIN` first, then `STGn` ascending, then anything
else alphabetically (the convention every source `data/isocs/*.dat` file follows) — the original
creation order isn't recoverable once the attributes are in a `Dict` (`GMTdataset.attrib`), so this
reconstructs a clean, consistent header rather than guessing at byte order; the CONTENT (every
key/value, to full precision) is exact regardless.

### Example
```julia
shapenc2isoc("isochrons_EU_NA.nc", "c13", "c13_EU_NA_reconstructed.dat")
```
"""
function shapenc2isoc(fname_nc::String, isoc_name::String, fname_out::String)
	D = _shnc_read(fname_nc)
	i = findfirst(d -> get(d.attrib, "name", "") == isoc_name, D)
	i === nothing && error("shapenc2isoc: no ensemble named \"$isoc_name\" in $fname_nc")
	d = D[i]
	full_name = get(d.attrib, "full_name", isoc_name)

	keys_ = [k for k in keys(d.attrib) if !(k in ("name", "full_name", "BoundingBox", "SegmentBoundaries"))]
	stgnum(k::String)::Int = (m = match(r"^STG(\d+)", k); m === nothing ? typemax(Int) : parse(Int, m.captures[1]))
	sort!(keys_, by = k -> k == "FIN" ? (0, 0, "") : startswith(k, "STG") ? (1, stgnum(k), k) : (2, 0, k))

	open(fname_out, "w") do io
		print(io, "> ", full_name)
		for k in keys_
			print(io, " ", k, "\"", d.attrib[k], "\"")
		end
		println(io)
		for r in 1:size(d.data, 1)
			println(io, _fmt6(d.data[r, 1]), "\t", _fmt6(d.data[r, 2]))
		end
	end
	fname_out
end
