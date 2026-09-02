# dgtlidar.jl — Tools > "DGT LIDAR (Portugal)": Portugal's national LIDAR survey, downloaded from the
# DGT CDD portal and mosaicked into one grid.
#
# The C++ dialog is DgtLidarDialog (70_window.cpp, loads deps/ui/dgt_lidar_dialog.ui).
#
# EVERYTHING about the survey is `GMT.dgt_lidar` / `GMT.dgt_mosaic` (GMT.jl, src/extras/dgt_lidar.jl):
# the authentication, the STAC queries, which collections exist, the resumable downloads, the VRT
# mosaic and its clipping. None of that is restated here — this file only turns the dialog's
# "key=value" block into ONE call of one of those two functions and puts what comes back where this
# window can show it. The two modes are exactly the two functions:
#
#   download  `dgt_lidar(bbox; …)` — fetch every tile intersecting the region into the tile directory
#             (already-downloaded tiles are skipped by the function itself). With the Mosaic box
#             ticked the same call also mosaics them, which is what its `mosaic` keyword is for: one
#             call, not a download followed by a separate mosaic that would have to re-list the tiles.
#   mosaic    `dgt_mosaic(bbox; …)` — the mosaic ALONE, over tiles already on disk. No account and no
#             network, for when the download happened in an earlier session.
#
# A mosaic that comes back as data is a NEW RASTER, so it goes in through the shared doors a derived
# grid uses (`_gm3d_deliver` / `_place_image_in_window`), which honour the raster-own-axes law. A
# mosaic written to a file is read back and shown the same way — a file the user asked for is still a
# result they asked to see.
#
# Like every C->Julia callback the @cfunction and its registration are RUNTIME values, created lazily
# at the first window open (eventloop.jl `_ensure_callbacks`) — never at top level.

# The five collections `dgt_lidar` accepts, in the dialog's order. Kept here only to catch a typo
# before the download starts; every rule about what they hold stays in GMT.jl.
const _DGT_COLLECTIONS = ("MDS-2m", "MDT-2m", "MDS-50cm", "MDT-50cm", "LAZ")

# Append one line to the dialog's log pane (gmtvtk_dgt_log). Best-effort and NEVER throws, so it can
# be called from a catch block.
_dgt_log(dlg::Ptr{Cvoid}, msg::AbstractString) = (try
	ccall(_fn(:gmtvtk_dgt_log), Cvoid, (Ptr{Cvoid}, Cstring), dlg, String(msg))
catch; end; nothing)

# "W/E/S/N" -> [W, E, S, N]. The region is the whole request here (the survey is queried BY AREA), so
# it is checked properly: four numbers that really make a box. Whether the box is over Portugal is
# `dgt_lidar`'s own question, and it answers it with its own message.
function _dgt_bbox(s::AbstractString)::Vector{Float64}
	p = split(strip(String(s)), '/')
	length(p) == 4 || error("the region takes four numbers, West/East/South/North, not '$s'")
	v = tryparse.(Float64, p)
	any(x -> x === nothing, v) && error("the region takes four NUMBERS, not '$s'")
	(v[1] < v[2]) || error("West must be smaller than East")
	(v[3] < v[4]) || error("South must be smaller than North")
	(-90 <= v[3] && v[4] <= 90) || error("South and North are latitudes, between -90 and 90")
	return Float64[v[1], v[2], v[3], v[4]]
end

# One optional number out of a text box: empty is the function's own default, anything else must
# really be a number so a typo does not reach GDAL as a silent 0.
function _dgt_num(s::AbstractString, what::AbstractString)::Float64
	t = strip(String(s))
	isempty(t) && return 0.0
	v = tryparse(Float64, t)
	v === nothing && error("$what is a number, not '$s'")
	return v
end

# Does the mosaic come out in lon/lat? The tiles are in the survey's own metric CRS (ETRS89 /
# PT-TM06), so ONLY an explicit reprojection to a geographic CRS makes them geographic. This is the
# dialog's own answer to "are x,y lon/lat", passed on to the delivery so the axes are labelled from
# what was asked for instead of guessed from the numbers.
function _dgt_isgeog(proj::AbstractString)::Bool
	t = lowercase(strip(String(proj)))
	isempty(t) && return false
	return startswith(t, "geo") || occursin("4326", t) || occursin("+proj=longlat", t)
end

# Run `f` with stdout captured, and give back what it printed. Used ONLY for the dry run, whose whole
# result IS its printout (`dgt_lidar(dry=true)` lists the tiles it found and returns nothing) — a real
# download is left printing to the terminal, where its progress can be watched while it runs.
function _dgt_capture(f::Function)::String
	old = stdout
	rd, wr = redirect_stdout()
	reader = @async read(rd, String)          # drain it, or a chatty run deadlocks on a full pipe
	err = nothing
	try
		f()
	catch e
		err = e
	finally
		redirect_stdout(old);  close(wr)
	end
	txt = fetch(reader);  close(rd)
	err === nothing || throw(err)
	return txt
end

# The mosaic -> the window. `R` is what the function returned: the grid itself when the mosaic was
# asked for in memory, or the path it wrote (which is read back — a result the user asked for is a
# result they asked to SEE).
function _dgt_deliver(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, R, dest::AbstractString, coll::AbstractString,
                      bbox::Vector{Float64}, proj::AbstractString, inc::Float64)::Cint
	G = R
	if !isa(G, GMTgrid) && !isa(G, GMTimage)
		path = isa(R, AbstractString) ? String(R) : String(dest)
		(path == "grid") &&
			error("no mosaic came back — the region may have no $coll tiles in it (try the dry run)")
		isfile(path) || error("no mosaic came back, and '$path' was not written")
		_dgt_log(dlg, "reading back $(basename(path))…")
		G = _gmtread_trb(path)
	end
	geog  = _dgt_isgeog(proj)
	title = "DGT $coll ($(round(bbox[1]; digits=4))/$(round(bbox[2]; digits=4))/" *
	        "$(round(bbox[3]; digits=4))/$(round(bbox[4]; digits=4)))"
	if isa(G, GMTimage)
		_place_image_in_window(scene, G, title; geographic = geog)
		_dgt_log(dlg, "\"$title\" added to the window")
		return Cint(1)
	end
	isa(G, GMTgrid) || error("got a $(typeof(G)), not a grid or an image")
	recipe = "dgt_mosaic $(bbox[1])/$(bbox[2])/$(bbox[3])/$(bbox[4]) collection=$coll" *
	         (inc != 0 ? " inc=$inc" : "") * (isempty(proj) ? "" : " proj=$proj")
	ok = _gm3d_deliver(scene, G, title, "", false, recipe; geographic = geog)
	ok == Cint(1) && _dgt_log(dlg, "\"$title\" added to the window " *
	                               "($(size(G.z, 2))×$(size(G.z, 1)) nodes)")
	return ok
end

# C callback (Download / Mosaic what I have): `cparams` is the newline-separated "key=value" block
# described in 30_app.cpp's JuliaDgtFn. Returns Cint 1 on success, 0 on failure.
function _on_dgt(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d    = _nswing_parse(unsafe_string(cparams))
		mode = _get(d, "mode", "download")

		# ---- Is this machine able to download at all? Asked ONCE, before the tool is of any use, so
		# the answer arrives while the user can still act on it instead of after they have picked an
		# area. Thorough on purpose: the file existing proves nothing, and a wrong password looks
		# exactly like a working one until the first download fails.
		if mode == "checkcreds"
			return _dgt_check_credentials(dlg)
		end

		# ---- Write ~/.dgt from the two boxes and immediately prove it works. `dgt_lidar` writes that
		# file too (its `save` keyword), but only as a side effect of a download — which is no use to
		# somebody who cannot download yet because the file is what is missing.
		if mode == "savecreds"
			u, p = _get(d, "user"), _get(d, "password")
			(isempty(u) || isempty(p)) && error("type both the e-mail and the password first")
			f = joinpath(homedir(), ".dgt")
			open(f, "w") do io                          # the exact format GMT._read_dgt_credentials reads
				println(io, "# Login data for the DGT LIDAR downloads")
				println(io, "login $u")
				println(io, "password $p")
			end
			_dgt_log(dlg, "wrote $f")
			return _dgt_check_credentials(dlg)
		end
		bbox = _dgt_bbox(_get(d, "region"))
		coll = _get(d, "collection", "MDS-2m")
		(coll in _DGT_COLLECTIONS) || error("unknown collection '$coll'")
		outdir = _get(d, "output_dir")
		domos  = _on(d, "mosaic")
		dest   = _get(d, "dest")                       # "grid", or the file to write
		inc    = _dgt_num(_get(d, "inc"), "the resample cell size")
		method = _get(d, "method", "cubicspline")
		proj   = _get(d, "proj")
		reg    = "$(bbox[1])/$(bbox[2])/$(bbox[3])/$(bbox[4])"

		# ---- The mosaic alone, over tiles already on disk. Nothing is downloaded and no account is
		# needed; if the tiles are not there, `dgt_mosaic` says so itself.
		if mode == "mosaic"
			domos || error("tick \"Mosaic the tiles into one grid\" to say where the mosaic should go")
			_dgt_log(dlg, "mosaicking the $coll tiles already on disk over $reg…")
			R = GMT.dgt_mosaic(bbox; src_dir = outdir, collection = coll, outfile = dest,
			                   inc = inc, method = method, proj = proj, verbose = 1)
			return _dgt_deliver(scene, dlg, R, dest, coll, bbox, proj, inc)
		end
		(mode == "download") || error("unknown mode '$mode'")

		user = _get(d, "user");  password = _get(d, "password")
		save = _on(d, "save")
		(save && (isempty(user) || isempty(password))) &&
			error("to remember the account, type both the e-mail and the password")
		delay = _dgt_num(_get(d, "delay", "1"), "the delay between requests")
		delay <= 0 && (delay = 1.0)
		latest   = _on(d, "latest")
		compress = _get(d, "compress")

		# ---- Dry run: the catalogue is queried and the tiles it found are LISTED, nothing else. The
		# listing is the function's own printout, so it is captured and shown in the dialog's log —
		# the one place this dialog has for text.
		if _on(d, "dry")
			_dgt_log(dlg, "asking the DGT catalogue which $coll tiles cover $reg…")
			txt = _dgt_capture() do
				GMT.dgt_lidar(bbox; user = user, password = password, save = save,
				              output_dir = outdir, delay = delay, collection = coll, dry = true,
				              latest = latest, compress = compress, verbose = 2)
			end
			for ln in split(txt, '\n')
				isempty(strip(ln)) || _dgt_log(dlg, String(rstrip(ln)))
			end
			_dgt_log(dlg, "dry run — nothing was downloaded")
			return Cint(1)
		end

		_dgt_log(dlg, "downloading the $coll tiles over $reg (already-fetched tiles are skipped)…")
		R = GMT.dgt_lidar(bbox; user = user, password = password, save = save, output_dir = outdir,
		                  delay = delay, collection = coll, latest = latest, compress = compress,
		                  mosaic = (domos ? dest : ""), inc = inc, method = method, proj = proj,
		                  verbose = 1)
		if !domos
			_dgt_log(dlg, "tiles downloaded to " * (isempty(outdir) ? "~/.gmt/DGT/$coll" : "$outdir/$coll"))
			return Cint(1)
		end
		return _dgt_deliver(scene, dlg, R, dest, coll, bbox, proj, inc)
	catch e
		msg = sprint(showerror, e)
		_dgt_log(dlg, "FAILED: $msg")
		_viewer_log_error(scene, "DGT LIDAR FAILED: $msg")
		return Cint(0)
	end
end

"""
    _dgt_check_credentials(dlg) -> Cint

Can this machine actually download from the DGT portal? Three questions, in the order that tells the
user something useful about which one failed:

1. does `~/.dgt` exist and carry BOTH a `login` and a `password` line (`GMT._read_dgt_credentials`
   answers this, and its own error text is the instructions for writing the file);
2. does the portal ACCEPT them (`GMT._authenticate` — the real login, not a guess);
3. does the session it hands back actually work (`GMT._test_session` against the STAC endpoint).

Returns 1 when all three pass, 0 otherwise, and says which one failed in the dialog's log.
"""
function _dgt_check_credentials(dlg::Ptr{Cvoid})::Cint
	user, password = try
		GMT._read_dgt_credentials()
	catch e
		_dgt_log(dlg, "NO ACCOUNT: $(sprint(showerror, e))")
		return Cint(0)
	end
	_dgt_log(dlg, "~/.dgt found (login $user) — checking it with the DGT portal…")
	ok = try
		GMT._authenticate(user, password, 0)
	catch e
		_dgt_log(dlg, "LOGIN FAILED: $(sprint(showerror, e))")
		return Cint(0)
	end
	if ok !== true
		_dgt_log(dlg, "LOGIN REJECTED: the DGT portal did not accept the e-mail/password in ~/.dgt")
		return Cint(0)
	end
	if !GMT._test_session()
		_dgt_log(dlg, "SESSION FAILED: logged in, but the portal's search endpoint refused the session")
		return Cint(0)
	end
	_dgt_log(dlg, "DGT account OK — downloads are available")
	return Cint(1)
end

function _register_dgt()
	fptr = @cfunction((s, w, c) -> Base.invokelatest(_on_dgt, s, w, c)::Cint,
	                  Cint, (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_dgt_callback), Cvoid, (Ptr{Cvoid},), fptr)
	_fp_register("dgt", _dgt_footprints)   # the region picker asks this "how is this ground tiled?"
	return
end

# The survey's own tiling under a region: the footprints the map draws beneath the picked rectangle
# (tilestool.jl's provider registry, name "dgt"; `arg` is the collection the dialog has selected).
#
# It is the SAME question `dgt_lidar(bbox; dry=true)` answers — which tiles cover this box — asked of
# the SAME STAC endpoint. What differs is only what is kept: the dry run keeps the URLs and throws the
# geometry away (`_collect_urls`), while a picture needs the geometry, so the response is read here
# for the `bbox` of each feature. No account and no download: the search endpoint is open, exactly as
# a dry run is.
function _dgt_footprints(W::Float64, E::Float64, S::Float64, N::Float64,
                         arg::String)::Tuple{Vector{Float64}, Vector{String}}
	coll = isempty(strip(arg)) ? "MDS-2m" : String(strip(arg))
	rects, names = Float64[], String[]
	try
		resp  = GMT._search_stac((W, E, S, N); collections=coll, delay=0.0)
		feats = get(resp, "features", [])
		seen  = Set{String}()
		for item in feats
			bb = get(item, "bbox", nothing)
			(bb === nothing || length(bb) < 4) && continue
			# STAC bbox order is [min_lon, min_lat, max_lon, max_lat]; ours is W,E,S,N.
			w, s, e, n = Float64(bb[1]), Float64(bb[2]), Float64(bb[3]), Float64(bb[4])
			id = string(get(item, "id", ""))
			# One rectangle per GROUND tile: a tile re-flown in several years is several STAC items
			# over the very same box, and drawing it four times just thickens the line.
			key = "$w/$e/$s/$n"
			(key in seen) && continue
			push!(seen, key)
			append!(rects, (w, e, s, n))
			push!(names, id)
		end
	catch e
		@tool_error "dgt footprints failed" exception=(e,)
		return Float64[], String[]
	end
	return rects, names
end
