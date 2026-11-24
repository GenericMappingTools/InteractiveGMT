# mbmesh_dialog.jl — Geophysics > MB-System > "3-D mesh (mbmesh)…": MB-System's mbmesh (its GMT module,
# from the MB-System supplement) on any point data iGMT reads, and the mesh it makes shown in the window.
# One more tool of the Remote sensing dialog family (RemoteSDialog, 70_window.cpp, over
# deps/ui/remotes_mbmesh.ui): same wire protocol, same busy notice.
#
# THE INPUT. A swath file (.mbXX, XX the MBIO format) or a datalist (.mb-1) goes to mbmesh as it is, and
# MB-System reads it. Any other point data (LAS/LAZ, an x,y,z table: what the open path shows as a point
# cloud) is read here, taken to WGS 84 lon/lat when it carries another coordinate system, and handed to
# mbmesh in memory (mbmesh_run_mesh_points), read as MBIO's generic lon lat topography points. Its z is a
# height, positive up, as the window shows it. No file is written for it.
#
# THE RESULT. mesh.glb is in Earth-centred coordinates and Draco-compressed, which neither VTK nor
# GMT.jl reads. MB-System reads it (mbglb2tiles' reader, Draco included) and hands it back in lon/lat/
# height through `mbmesh_read_glb_geodetic`, exported by the plugin (src/mbmesh/mbmesh_capi.h in
# MB-System): the mesh then goes into the window as a GMTfv, through the same mesh doors a mesh file
# goes through (`_promote_fv` into an empty window, `gmtvtk_add_mesh_h` + `_adopt_new_element` beside
# what is there).

# Point data this tool takes: MB-System swath files and datalists, and the point-cloud files.
const _MBMESH_SWATH = r"\.mb(\d{2,3}|-1)$"i
const _MBMESH_POINT_EXTS = Set{String}([".las", ".laz", ".xyz", ".xyt", ".txt", ".dat", ".csv"])

_mbmesh_is_swath(p::String)::Bool = occursin(_MBMESH_SWATH, p)
_mbmesh_is_points(p::String)::Bool = _mbmesh_is_swath(p) || lowercase(splitext(p)[2]) in _MBMESH_POINT_EXTS

# The point file this window shows, "" when it shows none: the prefill of the Input box.
function _mbmesh_window_file(scene::Ptr{Cvoid})::String
	for (path, e) in _OPEN_FILES
		e[1] == scene && _mbmesh_is_points(path) && isfile(path) && return path
	end
	return ""
end

# The output folder when the box is empty: beside the input, named after it.
_mbmesh_default_outdir(input::String)::String =
	joinpath(dirname(input), splitext(basename(input))[1] * "_mbmesh")

# What the input is, for the status line. Reads nothing but the name: a LAS file can be gigabytes.
function _mbmesh_describe(input::String)::String
	isempty(input) && return "Choose the point data: an MB-System swath file or datalist, a LAS/LAZ file or an x,y,z table."
	isfile(input) || return "No such file: $(input)"
	if _mbmesh_is_swath(input)
		ext = lowercase(splitext(input)[2])
		return ext == ".mb-1" ? "MB-System datalist: mbmesh reads its swath files." :
		                        "MB-System swath file, MBIO format $(ext[4:end])."
	end
	return "Point data: read as x, y, z (z a height, positive up) and taken to lon/lat when the file has another coordinate system."
end

# A path as MB-System's datalist reader wants it: on Windows with backslashes (a "C:/..." entry is
# taken as relative to the datalist's folder).
_mbmesh_native(p::String)::String = Sys.iswindows() ? replace(p, '/' => '\\') : p

# The points of a table (LAS/LAZ, x,y,z text) as an n x 3 matrix of lon, lat and height, taken to WGS 84
# lon/lat from the file's coordinate system, or from `srs` (the dialog's CRS box) when given.
function _mbmesh_table_points(input::String, srs::String = "")::Matrix{Float64}
	isfile(input) || error("input not found: $(input)")
	ext = lowercase(splitext(input)[2])
	# GMT.jl reads LAS/LAZ itself; any other table must be named a table (an .xyz, .txt or .csv
	# extension does not tell gmtread what it holds)
	D = ext in (".las", ".laz") ? _gmtread(input) : _gmtread(input; data = true)
	ds = D isa GMTdataset ? GMTdataset[D] : (D isa AbstractVector{<:GMTdataset} ? collect(D) : GMTdataset[])
	isempty(ds) && error("$(basename(input)) is not a table of points")
	M = reduce(vcat, (Float64.(d.data) for d in ds))
	size(M, 2) >= 3 || error("$(basename(input)) has no third column: a mesh needs x, y and z")
	keep = .!(isnan.(M[:, 1]) .| isnan.(M[:, 2]) .| isnan.(M[:, 3]))
	M = M[keep, 1:3]
	size(M, 1) >= 3 || error("$(basename(input)) has fewer than 3 points with x, y and z")
	isempty(srs) && (srs = _dataset_srs(ds[1]))
	isempty(srs) && error("$(basename(input)) is not in lon/lat and carries no coordinate system to take it there: " *
	                      "type it in CRS (an EPSG code such as EPSG:3763, or a PROJ string). The mesh is placed " *
	                      "on the Earth from longitude and latitude")
	_project_is_geog(srs) || (M[:, 1:2] = _srs_xy(M[:, 1:2], srs, _LONLAT))
	return M
end

# THE FAST METHOD. The soundings themselves are the vertices, joined by a Delaunay triangulation of their
# lon/lat (GMT's triangulate): under a second for a survey of hundreds of thousands of soundings, where
# mbmesh's Poisson surface takes tens of seconds. Swath files and datalists give their good soundings
# through MB-System's mblist (all beams, flagged ones left out); the other point data is read as for
# mbmesh. The triangulation also spans the gaps between swaths and the hull of the survey: a triangle
# with an edge longer than three times the median of the longest edges is left out.
function _mbmesh_fast_points(input::String, srs::String)::Matrix{Float64}
	_mbmesh_is_swath(input) || return _mbmesh_table_points(input, srs)
	D = GMT.gmt("mblist -I" * _gmt_quote_path(_mbmesh_native(input)) * " -MA -OXYZ")
	ds = D isa GMTdataset ? GMTdataset[D] : (D isa AbstractVector{<:GMTdataset} ? collect(D) : GMTdataset[])
	isempty(ds) && error("MB-System found no good soundings in $(basename(input))")
	M = reduce(vcat, (Float64.(d.data[:, 1:3]) for d in ds))
	size(M, 1) >= 3 || error("$(basename(input)) has fewer than 3 good soundings")
	return M
end

function _mbmesh_fast_fv(M::Matrix{Float64})::GMTfv
	T = GMT.triangulate(M[:, 1:2])
	ti = T isa GMTdataset ? T.data : reduce(vcat, (d.data for d in T))
	F = Matrix{Int}(round.(Int, ti[:, 1:3])) .+ 1
	# edge lengths in meters, lon scaled to the survey's mean latitude
	kx = 111320.0 * cosd(sum(@view M[:, 2]) / size(M, 1));  ky = 110540.0
	elen(a, b) = hypot(kx * (M[a, 1] - M[b, 1]), ky * (M[a, 2] - M[b, 2]))
	longest = [max(elen(F[k, 1], F[k, 2]), elen(F[k, 2], F[k, 3]), elen(F[k, 3], F[k, 1])) for k in 1:size(F, 1)]
	limit = 3.0 * sort(longest)[cld(length(longest), 2)]
	F = F[longest .<= limit, :]
	isempty(F) && error("the triangulation of the soundings left no triangle")
	bb = Float64[extrema(@view M[:, 1])..., extrema(@view M[:, 2])..., extrema(@view M[:, 3])...]
	zs = _resolve_zscale(:auto, bb[2] - bb[1], bb[4] - bb[3], bb[6] - bb[5], 0.2, true, :auto)
	return GMTfv(verts = M, faces = [F], bbox = bb, proj4 = _LONLAT, zscale = zs)
end

# The mesh of a GLB mbmesh (or mbgrd2gltf) wrote, in lon/lat/height, read by MB-System itself.
function _mbmesh_glb_geodetic(glb::String)::GMTfv
	lib = _gmt_plugin_file("mbsystem")
	isempty(lib) && error("MB-System's GMT supplement is not loaded: install it from Geophysics > MB-System")
	h = Libdl.dlopen(lib)                    # GMT holds it already: this only takes it once more
	try
		fn = Libdl.dlsym(h, :mbmesh_read_glb_geodetic; throw_error = false)
		fn === nothing && error("This MB-System supplement cannot read its meshes back: update it " *
		                        "(Geophysics > MB-System > Update the plugin)")
		fr = Libdl.dlsym(h, :mbmesh_free)
		v = Ref{Ptr{Cdouble}}(C_NULL);  t = Ref{Ptr{Cint}}(C_NULL)
		nv = Ref{Csize_t}(0);  nt = Ref{Csize_t}(0)
		err = zeros(UInt8, 1024)
		r = ccall(fn, Cint, (Cstring, Cint, Ref{Ptr{Cdouble}}, Ref{Csize_t}, Ref{Ptr{Cint}}, Ref{Csize_t}, Ptr{UInt8}, Csize_t),
		          glb, Cint(Threads.nthreads()), v, nv, t, nt, err, Csize_t(length(err)))
		r == 0 || error("MB-System could not read $(basename(glb)): " * unsafe_string(pointer(err)))
		V = permutedims(copy(unsafe_wrap(Array, v[], (3, Int(nv[])))))
		F = Matrix{Int}(permutedims(copy(unsafe_wrap(Array, t[], (3, Int(nt[])))))) .+ 1
		ccall(fr, Cvoid, (Ptr{Cvoid},), v[])
		ccall(fr, Cvoid, (Ptr{Cvoid},), t[])
		bb = Float64[minimum(V[:, 1]), maximum(V[:, 1]), minimum(V[:, 2]), maximum(V[:, 2]), minimum(V[:, 3]), maximum(V[:, 3])]
		# degrees across, metres up: the vertical scale every geographic mesh gets (poly2fv's :auto)
		zs = _resolve_zscale(:auto, bb[2] - bb[1], bb[4] - bb[3], bb[6] - bb[5], 0.2, true, :auto)
		return GMTfv(verts = V, faces = [F], bbox = bb, proj4 = _LONLAT, zscale = zs)
	finally
		Libdl.dlclose(h)
	end
end

# mbmesh run by MB-System's plugin with the mesh handed back in memory, in lon/lat/height (mbmesh_run_mesh, in the
# plugin's mbmesh_capi.h): no mesh.glb written and read back. `save`: the program writes its files as well.
function _mbmesh_run_in_memory(args::Vector{String}, save::Bool, points::Union{Nothing,Matrix{Float64}} = nothing)
	lib = _gmt_plugin_file("mbsystem")
	isempty(lib) && error("MB-System's GMT supplement is not loaded: install it from Geophysics > MB-System")
	h = Libdl.dlopen(lib)                    # GMT holds it already: this only takes it once more
	try
		fn = Libdl.dlsym(h, points === nothing ? :mbmesh_run_mesh : :mbmesh_run_mesh_points; throw_error = false)
		fn === nothing && error("This MB-System supplement cannot hand its meshes over: update it " *
		                        "(Geophysics > MB-System > Update the plugin)")
		fr = Libdl.dlsym(h, :mbmesh_free)
		v = Ref{Ptr{Cdouble}}(C_NULL);  t = Ref{Ptr{Cint}}(C_NULL)
		nv = Ref{Csize_t}(0);  nt = Ref{Csize_t}(0)
		st = if points === nothing
			ccall(fn, Cint, (Cint, Ptr{Cstring}, Cint, Ref{Ptr{Cdouble}}, Ref{Csize_t}, Ref{Ptr{Cint}}, Ref{Csize_t}),
			      Cint(length(args)), args, Cint(save), v, nv, t, nt)
		else
			P = permutedims(points)          # longitude, latitude, height of each point, one after the other
			ccall(fn, Cint, (Cint, Ptr{Cstring}, Ptr{Cdouble}, Csize_t, Cint, Ref{Ptr{Cdouble}}, Ref{Csize_t}, Ref{Ptr{Cint}}, Ref{Csize_t}),
			      Cint(length(args)), args, P, Csize_t(size(points, 1)), Cint(save), v, nv, t, nt)
		end
		st == 0 || error("mbmesh failed (status $(st))")
		nv[] > 0 && nt[] > 0 || error("mbmesh made no mesh")
		# the plugin's arrays are already what the window takes: x,y,z per vertex and 0-based triangle corners
		xyz = copy(unsafe_wrap(Array, v[], 3 * Int(nv[])))
		indices = copy(unsafe_wrap(Array, t[], 3 * Int(nt[])))
		ccall(fr, Cvoid, (Ptr{Cvoid},), v[])
		ccall(fr, Cvoid, (Ptr{Cvoid},), t[])
		V = permutedims(reshape(xyz, 3, :))
		F = permutedims(reshape(indices, 3, :)) .+ 1
		bb = Float64[extrema(@view V[:, 1])..., extrema(@view V[:, 2])..., extrema(@view V[:, 3])...]
		zs = _resolve_zscale(:auto, bb[2] - bb[1], bb[4] - bb[3], bb[6] - bb[5], 0.2, true, :auto)
		fv = GMTfv(verts = V, faces = [Matrix{Int}(F)], bbox = bb, proj4 = _LONLAT, zscale = zs)
		return fv, (xyz, fill(Cint(3), Int(nt[])), indices)
	finally
		Libdl.dlclose(h)
	end
end

# mbmesh's advance, read off its own log (-V, on stderr: "[input] completed", "chunk 7 of 40 done", "seams:",
# ...): the step it is on, and how far along the whole run is, 0..100. The chunks are the run's bulk (the
# Poisson surface of each), so they take the 5..90 stretch, one share per chunk. `st` carries what was seen.
function _mbmesh_progress_line!(st::Dict{Symbol,Any}, line::AbstractString)
	if occursin("[input] reading", line)
		st[:step] = "Reading the soundings";  st[:pct] = 1
	elseif occursin("[input] completed", line)
		m = match(r"accepted (\d+) of (\d+) soundings", line)
		st[:info] = m === nothing ? "" : "$(m[1]) of $(m[2]) soundings accepted"
		st[:step] = "Planning the reconstruction";  st[:pct] = 3
	elseif (m = match(r"\[chunking\].*?(\d+) chunks contain points", line)) !== nothing
		st[:nchunks] = parse(Int, m[1])
		st[:step] = "Reconstructing the surface, chunk by chunk";  st[:pct] = 5
	elseif (m = match(r"\[chunking\] (?:chunk (\d+) of (\d+) \S+ done|\s*(\d+) of (\d+) chunks,)", line)) !== nothing
		m = m[1] === nothing ? (m[3], m[4]) : (m[1], m[2])
		k, n = parse(Int, m[1]), parse(Int, m[2])
		k > get(st, :chunk, (0,))[1] || return st         # the summary line may trail the chunk lines
		st[:step] = "Reconstructing the surface: chunk $(k) of $(n)"
		st[:pct] = 5 + round(Int, 85 * k / max(n, 1))
		st[:chunk] = (k, n, time())
	elseif occursin("[chunking] completed", line)
		st[:step] = "Joining the chunk meshes along their seams";  st[:pct] = 90
	elseif occursin("[chunking] seams:", line)
		st[:step] = "Removing the pieces with no soundings near them";  st[:pct] = 95
	elseif occursin("[chunking] islands:", line) || occursin("[reconstruction] completed", line)
		st[:step] = "Handing the mesh over";  st[:pct] = 97
	end
	return st
end

_mbmesh_hms(s::Real)::String = (s = round(Int, s); s < 60 ? "$(s) s" : "$(s ÷ 60) min $(lpad(s % 60, 2, '0')) s")

# The text of the progress dialog: the step, then the time spent and, once chunks are being done, the time left.
function _mbmesh_progress_label(st::Dict{Symbol,Any})::String
	now = time()
	tail = "elapsed $(_mbmesh_hms(now - st[:t0]))"
	if haskey(st, :chunk) && st[:pct] < 90
		k, n, _ = st[:chunk]
		per = (now - st[:t0]) / k                        # chunks run on several threads: the mean so far
		tail *= ", about $(_mbmesh_hms(per * (n - k))) left for the chunks"
	end
	info = get(st, :info, "")
	return "mbmesh — $(st[:step])…\n" * (isempty(info) ? "" : info * "\n") * tail
end

# `_mbmesh_run_in_memory` with its advance on screen. The run is one long call into the plugin, so it goes on a
# worker thread while this one (the UI thread, inside the dialog's request) reads mbmesh's log as it is written
# and moves the progress dialog: the step, the percentage, the time. The log is caught the way the console
# catches a C library's output (`_console_capture_file`, both C runtimes). mbmesh calls no GMT, so the thread
# is safe; in a single-threaded session the run goes on this thread and the dialog shows only its first step.
function _mbmesh_run_watched(args::Vector{String}, save::Bool, points::Union{Nothing,Matrix{Float64}})
	st = Dict{Symbol,Any}(:t0 => time(), :step => "Starting", :pct => 0)
	_rs_prog_show(_mbmesh_progress_label(st))           # the caller closes it, once the mesh is in the window
	Threads.nthreads() > 1 || (_rs_prog(0, "mbmesh — computing the mesh…");
	                           return _mbmesh_run_in_memory(args, save, points))
	cap, msg = _console_capture_file(2)
	cap === nothing && @warn msg
	res = Ref{Any}(nothing)
	# -V: mbmesh reports each chunk only at its DEBUG level (at INFO, a line every few seconds)
	vargs = "-V" in args ? args : vcat(args, "-V")
	task = Threads.@spawn (res[] = try _mbmesh_run_in_memory(vargs, save, points) catch e; e end)
	pos = 0;  partial = ""
	try
		while true
			done = istaskdone(task)
			if cap !== nothing
				bytes = try
					open(cap.path) do io
						seek(io, pos);  read(io)
					end
				catch
					UInt8[]
				end
				pos += length(bytes)
				text = partial * String(bytes)
				lines = split(text, '\n')
				partial = String(pop!(lines))       # an unfinished line waits for the rest of it
				for l in lines
					_mbmesh_progress_line!(st, l)
				end
			end
			_rs_prog(st[:pct], _mbmesh_progress_label(st))
			done && break
			Libc.systemsleep(0.2)                    # no yield: this runs inside a Qt callback
		end
	finally
		_console_capture_file_end(cap)
	end
	res[] isa Exception && throw(res[])
	return res[]
end

# A mesh written as GLB the way mbmesh writes mesh.glb, by MB-System's own writer (mbmesh_write_glb in the plugin):
# Draco-compressed and, for a mesh in lon/lat, georeferenced as mbmesh's (Save mesh… > glTF binary).
function _save_mesh_glb(fv::GMTfv, path::String)
	lib = _gmt_plugin_file("mbsystem")
	isempty(lib) && error("GLB is written by MB-System's GMT supplement, which is not loaded: install it from Geophysics > MB-System")
	h = Libdl.dlopen(lib)
	try
		fn = Libdl.dlsym(h, :mbmesh_write_glb; throw_error = false)
		fn === nothing && error("This MB-System supplement cannot write GLB: update it (Geophysics > MB-System > Update the plugin)")
		V = fv.verts
		xyz = Vector{Float64}(undef, 3 * size(V, 1))
		@inbounds for i in 1:size(V, 1)
			xyz[3i - 2] = V[i, 1];  xyz[3i - 1] = V[i, 2];  xyz[3i] = V[i, 3]
		end
		F = reduce(vcat, fv.faces)
		size(F, 2) == 3 || error("GLB takes triangles; this mesh has faces of $(size(F, 2)) corners")
		tri = Vector{Cint}(undef, 3 * size(F, 1))
		@inbounds for k in 1:size(F, 1)
			tri[3k - 2] = F[k, 1] - 1;  tri[3k - 1] = F[k, 2] - 1;  tri[3k] = F[k, 3] - 1
		end
		err = zeros(UInt8, 1024)
		r = ccall(fn, Cint, (Cstring, Ptr{Cdouble}, Csize_t, Ptr{Cint}, Csize_t, Cint, Ptr{UInt8}, Csize_t),
		          path, xyz, Csize_t(size(V, 1)), tri, Csize_t(size(F, 1)), Cint(GMT.isgeog(fv)), err, Csize_t(length(err)))
		r == 0 || error("MB-System could not write $(basename(path)): " * unsafe_string(pointer(err)))
	finally
		Libdl.dlclose(h)
	end
	return path
end

# A Scene Objects name not in use in this window: `base`, else "base (2)", "base (3)", ...
function _mbmesh_free_name(scene::Ptr{Cvoid}, base::String)::String
	has(n) = ccall(_fn(:gmtvtk_has_element_h), Cint, (Ptr{Cvoid}, Cstring), scene, n) != 0
	has(base) || return base
	k = 2
	while has("$(base) ($(k))");  k += 1;  end
	return "$(base) ($(k))"
end

# The mesh into the window: into an empty window in place, else beside what is there, as the layer shown.
# `packed`: the window's arrays (x,y,z per vertex, the corners of each face, 0-based) when they are at hand already,
# as mbmesh hands them over, sparing their rebuilding from the GMTfv.
function _mbmesh_show(scene::Ptr{Cvoid}, fv::GMTfv, name::String, packed = nothing)
	_remember_object!(scene, :mesh, name, fv)   # Save mesh… finds it by name (as GLB, savefile.jl)
	xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale = packed === nothing ?
		_fv_resolve(fv; color = false, geographic = true) :
		(packed[1], packed[2], packed[3], UInt8[], Float64[], _cpt_nodes_range(fv.bbox[5], fv.bbox[6], :turbo)...,
		 _fv_bbox(fv), true, fv.zscale > 0 ? Float64(fv.zscale) : 1.0)
	if _rs_empty(scene)
		_promote_fv(scene, xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale, false, name) == 1 && return
	end
	ok = GC.@preserve xyz sides indices cz crgb ccall(_fn(:gmtvtk_add_mesh_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Ptr{Cint}, Ptr{Cuchar}, Ptr{Cuchar}, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cint, Cdouble, Cstring),
		scene, xyz, Cint(length(xyz) ÷ 3), sides, Cint(length(sides)), indices, Ptr{Cuchar}(C_NULL), Ptr{Cuchar}(C_NULL),
		cz, crgb, Cint(ncolor), Cint(geog ? 1 : 0), Float64(zscale), name)
	ok == 1 || error("the mesh could not be added to the window")
	_adopt_new_element(scene, name, fv)
	return
end

function _rs_mbmesh(scene::Ptr{Cvoid}, d::Dict{String,String}, io::IOBuffer)::Bool
	what = _rs_get(d, "what")
	input = _rs_get(d, "p_input")
	if what == "init"
		win = _mbmesh_window_file(scene)
		if isempty(input) && !isempty(win)
			input = win
			_rs_set(io, "p_input", input)
		end
	end
	if what in ("init", "refresh")
		# Detail, Decimate, GeoOrigin, the output folder and the extra files are mbmesh's: the fast method uses none
		mb = !startswith(_rs_get(d, "p_method"), "Fast")
		for w in ("p_outdir", "p_lod", "p_decimate", "p_geoorigin", "p_save", "p_tileset", "p_html", "p_cesium", "p_track")
			_rs_enable(io, w, mb)
		end
		isfile(input) && isempty(_rs_get(d, "p_outdir")) && _rs_set(io, "p_outdir", _mbmesh_default_outdir(input))
		what == "refresh" && isfile(input) && _rs_set(io, "p_outdir", _mbmesh_default_outdir(input))
		_rs_status(io, _mbmesh_describe(input))
		return true
	end
	what == "mesh" || error("unknown request: $what")
	isempty(input) && error("Choose the point data to mesh (Input).")
	isfile(input) || error("No such file: $(input)")
	if startswith(_rs_get(d, "p_method"), "Fast")
		M = _mbmesh_fast_points(input, _rs_get(d, "p_srs"))
		fv = _mbmesh_fast_fv(M)
		name = _mbmesh_free_name(scene, splitext(basename(input))[1] * " mesh")
		_mbmesh_show(scene, fv, name)
		_rs_status(io, "\"$(name)\": $(size(fv.faces[1], 1)) triangles.")
		return true
	end
	isempty(_gmt_plugin_file("mbsystem")) && error("mbmesh is in MB-System's GMT supplement, which is not loaded: install it from Geophysics > MB-System.")
	extras = any(box -> _rs_on(d, box), ("p_tileset", "p_html", "p_cesium", "p_track"))
	# The mesh comes back in memory, and nothing is written unless asked for (the tileset and the pages are files, and
	# need the mesh files). mbmesh reads a swath file or a datalist itself; other point data goes to it from memory.
	save = _rs_on(d, "p_save") || extras
	outdir = _rs_get(d, "p_outdir")
	isempty(outdir) && (outdir = _mbmesh_default_outdir(input))
	points = _mbmesh_is_swath(input) ? nothing : _mbmesh_table_points(input, _rs_get(d, "p_srs"))
	args = String["mbmesh", "-I" * _mbmesh_native(input), "-O" * _mbmesh_native(outdir)]
	for (box, opt) in (("p_lod", "--level-of-detail"), ("p_decimate", "--decimate"))
		v = _rs_get(d, box)
		isempty(v) && continue
		x = tryparse(Float64, v)
		(x === nothing || x <= 0) && error("$(box == "p_lod" ? "Level of detail" : "Decimate") is a size in meters, more than zero: \"$(v)\"")
		push!(args, "$(opt)=$(v)")
	end
	for (box, opt) in (("p_geoorigin", "--geoorigin"), ("p_tileset", "--tileset"), ("p_html", "--html"),
	                   ("p_cesium", "--cesium-html"), ("p_track", "--track-glb"))
		_rs_on(d, box) && push!(args, opt)
	end
	name = _mbmesh_free_name(scene, splitext(basename(input))[1] * " mesh")
	local fv, packed
	try
		fv, packed = _mbmesh_run_watched(args, save, points)
		_rs_prog(99, "mbmesh — putting the mesh ($(size(fv.faces[1], 1)) triangles) in the window…")
		_mbmesh_show(scene, fv, name, packed)
	finally
		_rs_prog_close()
	end
	_rs_status(io, "\"$(name)\": $(size(fv.faces[1], 1)) triangles.")
	return true
end
