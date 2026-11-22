# s1orbits.jl — Geophysics > GMTSAR > "Sentinel-1 orbits…": the precise (POEORB) or restituted (RESORB)
# orbit file of each Sentinel-1 scene, from ESA's public orbit archive, unzipped to the .EOF that
# GMTSAR's preprocessing (make_s1a_tops, align_tops.csh, preproc_batch_tops.csh) reads.
# The graphic face of GMTSAR's download_sentinel_orbits.csh (gmtsar/csh), same server and same
# rule, without wget/unzip/BSD-vs-GNU `date`. One more tool of the Remote sensing dialog family
# (RemoteSDialog, 70_window.cpp, over deps/ui/remotes_s1orbits.ui): same wire protocol, same progress
# dialog (`_rs_prog_*`).
#
# The archive (checked live 2026-11-22): https://step.esa.int/auxdata/orbits/Sentinel-1/<TYPE>/<SAT>/
# <yyyy>/<mm>/, a plain directory listing, no login. A file is
#   S1A_OPER_AUX_POEORB_OPOD_<produced>_V<valid from>_<valid to>.EOF.zip
# filed under the month its validity STARTS. A POEORB covers ~26 h (one day plus an hour each side)
# and appears ~20 days after the acquisition; a RESORB covers ~3 h and appears within hours.
# GMTSAR's fetchOrbit.py asks scihub.copernicus.eu/gnss, which is gone: not used.
#
# The scene's time span comes from its NAME: a product (S1A_IW_SLC__1SDV_<start>_<stop>_….SAFE or
# .zip, or the manifest.safe inside one) or a measurement file (s1a-iw1-slc-vv-<start>-<stop>-….tiff
# /.xml), the files GMTSAR's raw/ folder links.

const _S1ORB_URL = "https://step.esa.int/auxdata/orbits/Sentinel-1"
# The Orbits combo: (text, the archive's types in the order they are tried).
const _S1ORB_KINDS = Tuple{String,Vector{String}}[
	("Precise (POEORB), else restituted (RESORB)", ["POEORB", "RESORB"]),
	("Precise only (POEORB)",                      ["POEORB"]),
	("Restituted only (RESORB)",                   ["RESORB"]),
]
# How far the orbit must reach beyond the scene on each side: download_sentinel_orbits.csh asks a RESORB
# for 50 minutes, and a POEORB always has more than an hour.
const _S1ORB_MARGIN = Dict("POEORB" => GMT.Dates.Minute(50), "RESORB" => GMT.Dates.Minute(50))

struct S1OrbScene
	path::String
	name::String                 # the product's (or measurement's) name, as shown
	sat::String                  # "S1A" … "S1D"
	t0::GMT.Dates.DateTime
	t1::GMT.Dates.DateTime
end

struct S1OrbFile
	name::String                 # S1A_OPER_AUX_POEORB_OPOD_…EOF.zip
	made::GMT.Dates.DateTime
	v0::GMT.Dates.DateTime
	v1::GMT.Dates.DateTime
end

const _S1ORB_FMT = GMT.Dates.DateFormat("yyyymmddTHHMMSS")
_s1orb_t(s::AbstractString) = GMT.Dates.DateTime(uppercase(s), _S1ORB_FMT)

# A scene from a path; `nothing` when the name says no Sentinel-1 scene.
function _s1orb_scene(path::AbstractString)::Union{S1OrbScene,Nothing}
	p = rstrip(String(path), ['/', '\\'])
	n = basename(p)
	lowercase(n) == "manifest.safe" && (p = dirname(p); n = basename(p))
	m = match(r"^(S1[A-D])_.*?_(\d{8}T\d{6})_(\d{8}T\d{6})_"i, n)
	m === nothing && (m = match(r"^(s1[a-d])-.*?-(\d{8}t\d{6})-(\d{8}t\d{6})-"i, n))
	m === nothing && return nothing
	return S1OrbScene(p, n, uppercase(m[1]), _s1orb_t(m[2]), _s1orb_t(m[3]))
end

# The Sentinel-1 scenes in a folder: products (.SAFE folders, .zip) and, when it has none, measurement
# files (a GMTSAR raw/ folder).
function _s1orb_scan(dir::String)::Vector{String}
	isdir(dir) || error("No such folder: $(dir)")
	names = readdir(dir)
	prod = String[joinpath(dir, n) for n in names
	              if occursin(r"^S1[A-D]_.*\.(SAFE|zip)$"i, n) && _s1orb_scene(n) !== nothing]
	isempty(prod) || return prod
	return String[joinpath(dir, n) for n in names
	              if occursin(r"^s1[a-d]-.*\.(tiff?|xml)$"i, n) && _s1orb_scene(n) !== nothing]
end

# One month of the archive, read once per run (`cache`).
function _s1orb_listing(kind::String, sat::String, d::GMT.Dates.Date, cache::Dict{String,Vector{S1OrbFile}})::Vector{S1OrbFile}
	url = "$(_S1ORB_URL)/$(kind)/$(sat)/$(GMT.Dates.year(d))/$(lpad(GMT.Dates.month(d), 2, '0'))/"
	haskey(cache, url) && return cache[url]
	io = IOBuffer()
	r = Downloads.request(url; output = io, throw = false)
	isa(r, Downloads.Response) || throw(r)
	# A month that has no files yet (the precise orbits of the last ~3 weeks) is a 404: an empty month.
	files = S1OrbFile[]
	if r.status == 200
		rx = Regex("href=\"($(sat)_OPER_AUX_$(kind)_OPOD_(\\d{8}T\\d{6})_V(\\d{8}T\\d{6})_(\\d{8}T\\d{6})\\.EOF\\.zip)\"")
		for m in eachmatch(rx, String(take!(io)))
			push!(files, S1OrbFile(m[1], _s1orb_t(m[2]), _s1orb_t(m[3]), _s1orb_t(m[4])))
		end
	elseif r.status != 404
		error("ESA's orbit archive answered $(r.status) for $(url)")
	end
	cache[url] = files
	return files
end

# The orbit file of `kind` for a scene: of those reaching the margin on both sides, the one that
# reaches furthest on its shorter side, the newest production on a tie. `nothing` when there is none.
function _s1orb_find(sc::S1OrbScene, kind::String, cache::Dict{String,Vector{S1OrbFile}})::Union{Tuple{String,S1OrbFile},Nothing}
	months = unique([GMT.Dates.firstdayofmonth(GMT.Dates.Date(sc.t0 - GMT.Dates.Day(1))),
	                 GMT.Dates.firstdayofmonth(GMT.Dates.Date(sc.t0))])
	mg = _S1ORB_MARGIN[kind]
	best = nothing;  bestm = GMT.Dates.Millisecond(-1);  bestmo = GMT.Dates.Date(0)
	for mo in months, f in _s1orb_listing(kind, sc.sat, mo, cache)
		(f.v0 <= sc.t0 - mg && f.v1 >= sc.t1 + mg) || continue
		m = min(sc.t0 - f.v0, f.v1 - sc.t1)
		if best === nothing || m > bestm || (m == bestm && f.made > best.made)
			best, bestm, bestmo = f, m, mo
		end
	end
	best === nothing && return nothing
	url = "$(_S1ORB_URL)/$(kind)/$(sc.sat)/$(GMT.Dates.year(bestmo))/$(lpad(GMT.Dates.month(bestmo), 2, '0'))/$(best.name)"
	return url, best
end

# Download and unzip one orbit into `dir`; the .EOF path. Kept when it is already there.
function _s1orb_fetch(url::String, f::S1OrbFile, dir::String)::Tuple{String,Bool}
	eof = joinpath(dir, replace(f.name, r"\.zip$" => ""))
	isfile(eof) && filesize(eof) > 0 && return eof, false
	mkpath(dir)
	zip = eof * ".zip.part"
	try
		r = open(zip, "w") do io
			Downloads.request(url; output = io, throw = false)
		end
		isa(r, Downloads.Response) || throw(r)
		200 <= r.status < 300 || error("ESA's orbit archive answered $(r.status) for $(url)")
		head = open(io -> read(io, 4), zip)
		head == UInt8[0x50, 0x4b, 0x03, 0x04] || error("ESA's orbit archive did not send a zip file for $(f.name)")
		run(pipeline(`$(_ecmwf_tar()) -xf $zip -C $dir`; stdout = devnull, stderr = devnull))
	finally
		isfile(zip) && rm(zip; force = true)
	end
	isfile(eof) || error("$(f.name) did not hold $(basename(eof))")
	return eof, true
end

_s1orb_paths(d::Dict{String,String})::Vector{String} = String[s for s in split(_rs_get(d, "p_names"), ';') if !isempty(strip(s))]

function _s1orb_dir(d::Dict{String,String}, scenes::Vector{S1OrbScene})::String
	dir = _rs_get(d, "p_outdir")
	isempty(dir) || return dir
	isempty(scenes) && return joinpath(homedir(), "Downloads", "Sentinel-1", "orbits")
	return dirname(scenes[1].path)
end

_s1orb_when(sc::S1OrbScene)::String = "$(sc.sat) $(GMT.Dates.format(sc.t0, "yyyy-mm-dd HH:MM:SS"))"

# The scenes of the list, and a line per path that names none.
function _s1orb_scenes(d::Dict{String,String})::Tuple{Vector{S1OrbScene},Vector{String}}
	scenes = S1OrbScene[];  bad = String[]
	for p in _s1orb_paths(d)
		sc = _s1orb_scene(p)
		sc === nothing ? push!(bad, basename(rstrip(p, ['/', '\\']))) : push!(scenes, sc)
	end
	return scenes, bad
end

# The Log box: what the list holds, before anything is downloaded.
function _s1orb_preview(io::IOBuffer, scenes::Vector{S1OrbScene}, bad::Vector{String})
	lines = String["$(_s1orb_when(sc))   $(sc.name)" for sc in scenes]
	append!(lines, String["(not a Sentinel-1 scene name)   $(b)" for b in bad])
	_rs_set(io, "p_log", join(lines, '\t'))
end

function _rs_s1orbits(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	if what == "init"
		_rs_items(io, "p_kind", String[k[1] for k in _S1ORB_KINDS])
		_rs_set(io, "p_kind", 0)
		scenes, bad = _s1orb_scenes(d)
		_s1orb_preview(io, scenes, bad)
		_rs_status(io, "Add the Sentinel-1 scenes (.SAFE, .zip or their measurement files), or Add all in a folder, then Download.")
		return true
	end
	if what == "scan"
		dir = _rs_get(d, "p_scandir")
		isempty(dir) && error("Type the folder that holds the scenes (.SAFE folders, .zip, or a GMTSAR raw/ folder).")
		found = _s1orb_scan(dir)
		isempty(found) && error("No Sentinel-1 scene in $(dir).")
		lst = unique(vcat(_s1orb_paths(d), String[normpath(f) for f in found]))
		_rs_items(io, "p_names", lst)
		d2 = copy(d);  d2["p_names"] = join(lst, ';')
		scenes, bad = _s1orb_scenes(d2)
		_s1orb_preview(io, scenes, bad)
		isempty(_rs_get(d, "p_outdir")) && _rs_set(io, "p_outdir", dir)
		_rs_status(io, "$(length(found)) scene(s) found in $(dir): $(length(scenes)) in the list.")
		return true
	end
	if what == "refresh"
		scenes, bad = _s1orb_scenes(d)
		_s1orb_preview(io, scenes, bad)
		isempty(_rs_get(d, "p_outdir")) && !isempty(scenes) && _rs_set(io, "p_outdir", dirname(scenes[1].path))
		_rs_status(io, isempty(scenes) ? "No Sentinel-1 scene in the list yet." :
		               "$(length(scenes)) scene(s), $(join(unique(sc.sat for sc in scenes), ", ")), " *
		               "$(GMT.Dates.Date(minimum(sc.t0 for sc in scenes))) to $(GMT.Dates.Date(maximum(sc.t0 for sc in scenes))).")
		return true
	end
	what == "download" || error("unknown request: $what")
	scenes, bad = _s1orb_scenes(d)
	isempty(scenes) && error("No Sentinel-1 scene in the list: add the .SAFE folders (their manifest.safe), the .zip products or the measurement files.")
	kinds = _S1ORB_KINDS[clamp(_rs_idx(d, "p_kind") + 1, 1, length(_S1ORB_KINDS))][2]
	dir = _s1orb_dir(d, scenes)
	cache = Dict{String,Vector{S1OrbFile}}()
	lines = String[];  got = String[];  nnew = 0;  nfail = 0
	_rs_prog_show("Asking ESA's orbit archive for the orbits of $(length(scenes)) scene(s)…")
	try
		for (k, sc) in enumerate(scenes)
			_rs_prog(round(Int, 100 * (k - 1) / length(scenes)), "Orbit of $(_s1orb_when(sc)) ($(k) of $(length(scenes)))…")
			line = try
				hit = nothing;  kind = ""
				for kd in kinds
					hit = _s1orb_find(sc, kd, cache)
					hit === nothing || (kind = kd; break)
				end
				if hit === nothing
					nfail += 1
					"$(_s1orb_when(sc))   NO $(join(kinds, " or ")) orbit in ESA's archive yet" *
						(kinds == ["POEORB"] ? " (the precise ones come ~20 days after the acquisition: try restituted)" : "")
				else
					eof, isnew = _s1orb_fetch(hit[1], hit[2], dir)
					isnew && (nnew += 1)
					push!(got, eof)
					"$(_s1orb_when(sc))   $(kind)   $(basename(eof))" * (isnew ? "" : "   (already there)")
				end
			catch e
				nfail += 1
				"$(_s1orb_when(sc))   FAILED: " * replace(sprint(showerror, e), '\n' => ' ', '\t' => ' ')
			end
			push!(lines, line)
		end
	finally
		_rs_prog_close()
	end
	append!(lines, String["(not a Sentinel-1 scene name)   $(b)" for b in bad])
	nu = length(unique(got))
	_rs_set(io, "p_log", join(lines, '\t'))
	_rs_status(io, "$(nu) orbit file(s) in $(dir)" * (nnew > 0 ? " ($(nnew) downloaded)" : "") *
	               (nfail > 0 ? "; $(nfail) scene(s) without an orbit: see the log." : "."))
	isempty(got) && error("No orbit was obtained:\n\n" * join(lines, '\n'))
	return true
end
