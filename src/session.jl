# session.jl — File > Save Session / Load Session (Mirone-style). A session stores HOW to rebuild a
# window, not its pixels: file-backed layers keep only a source ref + params; only iGMT-generated data
# is serialized (grids/images -> netCDF/GeoTIFF, vector tables -> ASCII). One window per session.
#
# See SESSION_SAVE_PLAN.md for the full design. This file is the dependency-free P1 spine:
#   * the provenance registry (_SESSION_LOG / ElementRecipe / _session_record!) — every add site logs
#     one recipe at add time, when the source path / params are still in hand;
#   * a hand-controlled `[section] key=value` manifest (no JSON/TOML dep);
#   * a pure-Julia STORE-ONLY ZIP writer/reader (a real, standard `.zip` = the `.igmtz` container) —
#     Project.toml is off-limits, so no ZipFile/Tar; store-only keeps it ~100 lines and grids are
#     already netCDF, vectors tiny ASCII, so compression buys little;
#   * save/load orchestration. P1 replays base grid/image; camera/VE/drops/menu-layers land in P2/P3
#     alongside the C++ scene-state get/apply exports.

# ── provenance registry ──────────────────────────────────────────────────────────────────────
# One recipe per Scene element, in add order. `origin`: :file (source is a disk path, no data stored)
# | :generated (source is a sidecar id inside the zip, data serialized) | :menu (rebuilt from params
# alone, e.g. coastlines). `name` is the Scene Objects label (selects the live object for :generated).
mutable struct ElementRecipe
	kind::Symbol
	origin::Symbol
	source::String
	name::String
	params::Dict{String,Any}
end
ElementRecipe(kind, origin, source=""; name="", params=Dict{String,Any}()) =
	ElementRecipe(Symbol(kind), Symbol(origin), String(source), String(name), params)

# Per-window provenance log, keyed by the opaque Scene* (parallel to savefile.jl `_SCENE_OBJS`).
const _SESSION_LOG = Dict{Ptr{Cvoid}, Vector{ElementRecipe}}()

"Append one recipe for `scene` (no-op on a null handle). Returns the recipe."
function _session_record!(scene::Ptr{Cvoid}, r::ElementRecipe)
	scene == C_NULL && return r
	push!(get!(() -> ElementRecipe[], _SESSION_LOG, scene), r)
	return r
end
# Convenience: build + record in one call.
_session_record!(scene::Ptr{Cvoid}, kind, origin, source=""; name="", params=Dict{String,Any}()) =
	_session_record!(scene, ElementRecipe(kind, origin, source; name=name, params=params))

"Forget a window's provenance log (call on window close / re-promote)."
_session_reset!(scene::Ptr{Cvoid}) = (delete!(_SESSION_LOG, scene); nothing)

# Drop every recipe of `kind` this window holds. For a STATE a window has one of at a time -- the
# Illumination model lighting it, say -- where a second run REPLACES the first rather than adding a
# second element: recording without this would replay three superseded lights before the one the
# user actually left on.
function _session_forget!(scene::Ptr{Cvoid}, kind::Symbol)
	scene == C_NULL && return nothing
	rs = get(_SESSION_LOG, scene, nothing)
	rs === nothing || filter!(r -> r.kind !== kind, rs)
	return nothing
end

"Record `r` as the ONLY recipe of its kind for `scene` (replaces any earlier one). See `_session_forget!`."
function _session_record_single!(scene::Ptr{Cvoid}, kind, origin, source=""; name="", params=Dict{String,Any}())
	_session_forget!(scene, Symbol(kind))
	return _session_record!(scene, kind, origin, source; name=name, params=params)
end

# In-memory curtain textures pending sidecar serialization (per scene: sidecar id -> PNG bytes). A
# curtain built from a GMTimage (not a file path) has no on-disk source, so its texture is stashed here
# at add time (curtain.jl) and written into the zip at save; a file-path curtain stores only the path.
const _CURTAIN_IMG = Dict{Ptr{Cvoid}, Dict{String,Vector{UInt8}}}()
_curtain_img_store!(scene::Ptr{Cvoid}, id::String, bytes::Vector{UInt8}) =
	(get!(() -> Dict{String,Vector{UInt8}}(), _CURTAIN_IMG, scene)[id] = bytes; id)
_curtain_img_next_id(scene::Ptr{Cvoid})::String =
	"curtain_" * string(length(get(_CURTAIN_IMG, scene, Dict{String,Vector{UInt8}}())) + 1) * ".png"

# Focal-mechanism catalogs, per window: group name -> the KEPT events in GMT's Aki & Richards column
# order (lon lat depth strike dip rake mag), one row per beachball actually drawn.
#
# Same reason `_CURTAIN_IMG` exists: the scene keeps a beachball as PATCH GEOMETRY (MecaBall holds
# actors, not angles), so the numbers that produced it cannot be read back off the display. They are
# stashed here at plot time by `_focal_plot`, which is the only place that has them. A freshly built
# matrix — never a view of anything handed to a ccall — so `_forget_window!` may purge it.
const _MECA_TABLE = Dict{Ptr{Cvoid}, Vector{Tuple{String,Matrix{Float64}}}}()
function _meca_table_store!(scene::Ptr{Cvoid}, name::String, M::Matrix{Float64})
	(scene == C_NULL || isempty(M)) && return M
	v = get!(() -> Tuple{String,Matrix{Float64}}[], _MECA_TABLE, scene)
	filter!(t -> t[1] != name, v)                  # re-plotting a group replaces it, never piles up
	push!(v, (name, M))
	return M
end

# Stamp a computed grid with the GMT.jl command that produced it. GMTgrid carries a `command` string
# that GMT persists into the netCDF header (grdinfo shows it), so every grid iGMT generates from a
# GMT.jl call records how it was made. Returns G for inline use.
function _grid_command!(G::GMTgrid, cmd::String)
	G.command = cmd
	return G
end

# ── manifest (hand-controlled text; no JSON/TOML dependency) ──────────────────────────────────
# Format: `[session]` / `[window]` / repeated `[element]` sections of `key=value` lines. `param.*`
# lines under an element collect into its params dict. Values are single-line strings, typed on use.
function _session_write_manifest(meta::Dict{String,String}, display::Dict{String,String}, recipes::Vector{ElementRecipe})
	io = IOBuffer()
	println(io, "[session]")
	for (k, v) in meta;    println(io, "$k=$v"); end
	println(io, "[window]")
	for (k, v) in display; println(io, "$k=$v"); end
	for r in recipes
		println(io, "[element]")
		println(io, "kind=$(r.kind)")
		println(io, "origin=$(r.origin)")
		println(io, "source=$(r.source)")
		println(io, "name=$(r.name)")
		for (k, v) in r.params; println(io, "param.$k=$v"); end
	end
	return String(take!(io))
end

# Parse a manifest string -> (meta::Dict, display::Dict, recipes::Vector{ElementRecipe}). Splits each
# `k=v` on the FIRST '=' so values may contain '='. Blank lines and unknown sections are ignored.
function _session_read_manifest(str::String)
	meta = Dict{String,String}(); display = Dict{String,String}(); recipes = ElementRecipe[]
	section = ""; cur = nothing
	for raw in split(str, '\n')
		line = strip(raw)
		isempty(line) && continue
		if startswith(line, "[") && endswith(line, "]")
			section = String(line[2:end-1])
			if section == "element"
				cur = ElementRecipe(:unknown, :unknown, ""; name="")
				push!(recipes, cur)
			end
			continue
		end
		eq = findfirst('=', line); eq === nothing && continue
		k = String(line[1:eq-1]); v = String(line[eq+1:end])
		if section == "session"
			meta[k] = v
		elseif section == "window"
			display[k] = v
		elseif section == "element" && cur !== nothing
			if     k == "kind";              cur.kind   = Symbol(v)
			elseif k == "origin";            cur.origin = Symbol(v)
			elseif k == "source";            cur.source = v
			elseif k == "name";              cur.name   = v
			elseif startswith(k, "param.");  cur.params[k[7:end]] = v
			end
		end
	end
	return (meta, display, recipes)
end

# ── pure-Julia STORE-ONLY ZIP (the `.igmtz` container) ───────────────────────────────────────
# A dependency-free, standard, store-only (method 0) zip: local file headers + central directory +
# EOCD, CRC-32/IEEE per entry. Reads and writes only method-0 members (all we produce). No Project.toml
# change — ZipFile/Tar are off-limits. Output opens in any zip tool.

# CRC-32 (IEEE, reflected poly 0xEDB88320) lookup table.
const _CRC32_TABLE = let t = Vector{UInt32}(undef, 256)
	for n in 0:255
		c = UInt32(n)
		for _ in 1:8
			c = (c & 0x00000001) != 0 ? (0xEDB88320 ⊻ (c >> 1)) : (c >> 1)
		end
		t[n+1] = c
	end
	t
end
function _crc32(data)::UInt32
	c = 0xFFFFFFFF
	for b in data
		c = _CRC32_TABLE[((c ⊻ UInt32(b)) & 0xff) + 1] ⊻ (c >> 8)
	end
	return c ⊻ 0xFFFFFFFF
end

_w16(io, x) = write(io, htol(UInt16(x)))
_w32(io, x) = write(io, htol(UInt32(x)))
# Little-endian readers from a 1-based byte vector at 0-based offset `o`.
_r16(d, o) = UInt16(d[o+1]) | (UInt16(d[o+2]) << 8)
_r32(d, o) = UInt32(d[o+1]) | (UInt32(d[o+2]) << 8) | (UInt32(d[o+3]) << 16) | (UInt32(d[o+4]) << 24)

"Build a standard store-only zip from `files` (Vector of (name, bytes)) as an in-memory byte vector."
function _zip_bytes(files)
	io = IOBuffer(); central = IOBuffer(); n = 0
	for (name, data) in files
		nb = Vector{UInt8}(codeunits(String(name)))
		crc = _crc32(data); sz = UInt32(length(data)); off = UInt32(position(io))
		# local file header
		_w32(io, 0x04034b50); _w16(io, 20); _w16(io, 0); _w16(io, 0); _w16(io, 0); _w16(io, 0)
		_w32(io, crc); _w32(io, sz); _w32(io, sz); _w16(io, length(nb)); _w16(io, 0)
		write(io, nb); write(io, data)
		# central directory record (buffered, appended after all locals)
		_w32(central, 0x02014b50); _w16(central, 20); _w16(central, 20)
		_w16(central, 0); _w16(central, 0); _w16(central, 0); _w16(central, 0)
		_w32(central, crc); _w32(central, sz); _w32(central, sz)
		_w16(central, length(nb)); _w16(central, 0); _w16(central, 0)
		_w16(central, 0); _w16(central, 0); _w32(central, 0); _w32(central, off)
		write(central, nb)
		n += 1
	end
	cdoff = UInt32(position(io)); cdbytes = take!(central); write(io, cdbytes)
	# end of central directory
	_w32(io, 0x06054b50); _w16(io, 0); _w16(io, 0); _w16(io, n); _w16(io, n)
	_w32(io, length(cdbytes)); _w32(io, cdoff); _w16(io, 0)
	return take!(io)
end

"Write `files` (Vector of (name, bytes)) to a standard store-only zip at `path`."
_zip_write(path::String, files) = (open(f -> write(f, _zip_bytes(files)), path, "w"); path)

"Parse store-only zip bytes -> Dict(name => bytes)."
function _zip_parse(d::Vector{UInt8})
	# locate EOCD: scan backwards for its signature (comment length is 0, so it is near the end)
	sig = UInt8[0x50, 0x4b, 0x05, 0x06]; eocd = -1
	for i in (length(d) - 21):-1:1
		if d[i] == sig[1] && d[i+1] == sig[2] && d[i+2] == sig[3] && d[i+3] == sig[4]
			eocd = i - 1; break                     # 0-based offset of EOCD
		end
	end
	eocd < 0 && error("zip: no end-of-central-directory record in '$path'")
	total = Int(_r16(d, eocd + 10)); cdoff = Int(_r32(d, eocd + 16))
	out = Dict{String,Vector{UInt8}}(); p = cdoff
	for _ in 1:total
		_r32(d, p) == 0x02014b50 || error("zip: bad central-directory signature")
		csize  = Int(_r32(d, p + 20)); fnlen = Int(_r16(d, p + 28))
		exlen  = Int(_r16(d, p + 30)); cmlen = Int(_r16(d, p + 32))
		lho    = Int(_r32(d, p + 42))
		name   = String(d[p + 46 + 1 : p + 46 + fnlen])
		lfn    = Int(_r16(d, lho + 26)); lex = Int(_r16(d, lho + 28))
		dstart = lho + 30 + lfn + lex                # 0-based data offset
		out[name] = d[dstart + 1 : dstart + csize]
		p += 46 + fnlen + exlen + cmlen
	end
	return out
end

"Read a store-only zip file -> Dict(name => bytes)."
_zip_read(path::String) = _zip_parse(read(path))

# ── save / load orchestration ────────────────────────────────────────────────────────────────
# Map a recipe kind to the savefile.jl object kind (:grid / :image) used by _find_object.
_session_obj_kind(r::ElementRecipe) = r.kind in (:image, :dropimage) ? :image : :grid

# Known raster extensions we strip off a Scene Objects name before appending the sidecar's own format
# extension, so "long_beach.grd" -> "long_beach.nc" rather than "long_beach.grd.nc".
const _SESSION_RASTER_EXT = (".nc", ".grd", ".tif", ".tiff", ".img", ".jp2", ".png", ".jpg", ".jpeg", ".bmp", ".hdr")

# Build a sidecar file name from the element's Scene Objects name: spaces -> underscores, path/illegal
# characters neutralized, any raster extension dropped, `ext` appended. Empty name -> "element". Kept
# UNIQUE within a session by suffixing _2, _3, … on collision (`used` tracks the taken ids).
function _session_sidecar_id(name::String, ext::String, used::Set{String})
	base = strip(name)
	stem, e = splitext(base)
	(!isempty(stem) && lowercase(e) in _SESSION_RASTER_EXT) && (base = stem)
	base = replace(base, r"[ \t]+" => "_")            # spaces -> underscores (the user's rule)
	base = replace(base, r"[\\/:*?\"<>|]" => "_")      # neutralize path separators / illegal filename chars
	isempty(base) && (base = "element")
	id = base * ext; n = 1
	while id in used
		n += 1; id = "$(base)_$(n)$(ext)"
	end
	push!(used, id)
	return id
end

# Serialize one :generated recipe's live data to bytes. Grids -> netCDF, images -> GeoTIFF (P1). The
# sidecar id mirrors the element's Scene Objects name (spaces -> underscores); it becomes the recipe's
# stored `source`. `used` keeps ids unique across the session.
function _session_pack_generated(scene::Ptr{Cvoid}, r::ElementRecipe, used::Set{String})
	objkind = _session_obj_kind(r)
	# EXACT name match for a named element (never the "first of kind" fallback, which would grab an
	# unrelated grid and serialize it twice); the unnamed base uses the ordered first-of-kind.
	obj = isempty(r.name) ? _find_object(scene, objkind, "") : _find_object_exact(scene, objkind, r.name)
	obj === nothing && error("session: no live $(objkind) named '$(r.name)' to serialize")
	ext = objkind === :grid ? ".nc" : ".tif"
	id = _session_sidecar_id(r.name, ext, used)
	tmp = tempname() * ext
	_serialize_object(obj, tmp)
	bytes = read(tmp); rm(tmp; force=true)
	return (id, bytes)
end

# THE writer for "put this live scene object on disk as a sidecar": grids -> netCDF (GMT.gmtwrite),
# everything else (images) -> GDAL, driver by extension. One function, because there are now two
# callers that must agree on the format choice — Save Session's zip member above, and the GMT.jl
# script export's `script_data/` directory (gmtscript.jl), which is the same operation aimed at a
# different destination.
_serialize_object(obj, path::String) =
	(obj isa GMTgrid || obj isa GMTdataset || obj isa Vector{<:GMTdataset}) ?
		GMT.gmtwrite(path, obj) : GMT.gdalwrite(path, obj)

# Fetch the window's RESTORABLE display state (camera/VE/flat2d/colorbar/shading) as the raw "k=v;" string the
# C serializer emits — stored verbatim under the manifest [window] `state=` key and fed back to
# gmtvtk_apply_scene_state on load (no Julia-side re-serialization, so it is lossless). Two-pass buffer.
function _scene_state_full_raw(h::Ptr{Cvoid})::String
	n = ccall(_fn(:gmtvtk_scene_state_full), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
	n <= 0 && return ""
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_scene_state_full), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
	return unsafe_string(pointer(buf))
end

# Snapshot the window's display state into the manifest [window] section: one `state=<raw kv>` line
# (the value may contain '=' and ';' — the manifest reader splits only on the FIRST '=', so it round-
# trips intact).
function _session_display(scene::Ptr{Cvoid})
	raw = try _scene_state_full_raw(scene) catch; "" end
	d = isempty(raw) ? Dict{String,String}() : Dict("state" => raw)
	chk = try _session_checked(scene) catch; "" end
	isempty(chk) || (d["checked"] = chk)
	return d
end

# EXACTLY which Scene Objects rows are checked, base surface included, as
# "<name>\x1e<0|1>" entries joined by \x1f (separators no object name can contain; the manifest
# reader splits a line only on its FIRST '=', so the value round-trips whatever it holds).
# Read from the LIVE actors (`gmtvtk_scene_state`: `surfvis` for the base, `extravis<i>` for each
# extra) rather than from anything the recipes imply — what the user sees is the thing to store.
function _session_checked(scene::Ptr{Cvoid})
	st = _scene_state(scene)
	parts = String[]
	haskey(st, "surfvis") &&
		push!(parts, string(String(get(st, "surf_name", "")), '\x1e', Int(st["surfvis"]) != 0 ? 1 : 0))
	for (i, (_, nm)) in enumerate(get(st, "extras", Tuple{String,String}[]))
		vis = get(st, "extravis$(i - 1)", 1)          # `extras` is 1-based here, the keys are 0-based
		push!(parts, string(nm, '\x1e', Int(vis) != 0 ? 1 : 0))
	end
	return join(parts, '\x1f')
end

# Put the checkboxes back EXACTLY as they were saved. `gmtvtk_set_object_visible` is the ONE
# visibility setter for both an extra (by name) and the base surface (by its own name, or ""), so
# this is the same door the panel's own checkbox uses — not a parallel show/hide path. Runs LAST in
# the load, after every replay and after the display state, so what the user saved is what wins.
function _session_apply_checked!(fig, display)
	fig === nothing && return
	blob = get(display, "checked", "")
	isempty(blob) && return
	h = getfield(fig, :h)
	for e in split(blob, '\x1f'; keepempty=false)
		p = split(e, '\x1e')
		length(p) < 2 && continue
		v = tryparse(Int, String(p[2]))
		v === nothing && continue
		ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), h, String(p[1]), Cint(v))
	end
	return
end

# Serialize the window's text labels to the C serializer's raw "x;y;r;g;b;size;group;text\n" blob
# (empty if none). C++-drawn elements have no add-time recipe, so they are SNAPSHOTTED at save time
# and rebuilt on load (P3). Two-pass buffer.
#
# `groups` picks WHICH labels: Save Session takes the default (false = user-placed only, a batch's
# labels come back with its recipe), the GMT.jl script export passes true because nothing else redraws
# them there and the capture no longer bakes them into the pixels. ONE function, one flag — see the
# export's own comment in 90_c_api.cpp.
function _serialize_texts_raw(h::Ptr{Cvoid}; groups::Bool=false)::String
	g = Cint(groups)
	n = ccall(_fn(:gmtvtk_serialize_texts), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint, Cint), h, C_NULL, Cint(0), g)
	n <= 0 && return ""
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_serialize_texts), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint, Cint), h, buf, Cint(n + 1), g)
	return unsafe_string(pointer(buf))
end

# Serialize the window's user-drawn polygons/polylines/rects/circles to the C serializer's raw blob
# (one line each; empty if none). Snapshotted at save like text (no add-time recipe). Two-pass buffer.
function _serialize_polys_raw(h::Ptr{Cvoid})::String
	n = ccall(_fn(:gmtvtk_serialize_polys), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
	n <= 0 && return ""
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_serialize_polys), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
	return unsafe_string(pointer(buf))
end

# Rebuild polygons from a saved blob into `fig`'s window via gmtvtk_add_poly_full (one per line). Each
# line is "closed;isRect;lr;lg;lb;lw;lstyle;fr;fg;fb;fop;name;x,y,z|x,y,z|…". Malformed lines skipped.
function _session_rebuild_polys!(fig, blob::String)
	(fig === nothing || isempty(blob)) && return
	h = getfield(fig, :h)
	for line in split(blob, '\n'; keepempty=false)
		try
			p = split(line, ';'; limit=13)
			length(p) < 13 && continue
			verts = Float64[]
			for vp in split(p[13], '|'; keepempty=false)
				c = split(vp, ',')
				length(c) < 3 && continue
				push!(verts, parse(Float64, c[1]), parse(Float64, c[2]), parse(Float64, c[3]))
			end
			nv = length(verts) ÷ 3
			nv == 0 && continue
			# The saved blob carries no group tag (gmtvtk_serialize_polys never wrote one), so a rebuilt
			# polygon comes back ungrouped — "" is exactly the pre-group behaviour, one loose row each.
			ccall(_fn(:gmtvtk_add_poly_full), Cint,
			      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
			       Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
			      h, verts, Cint(nv), Cint(parse(Int, p[1])), Cint(parse(Int, p[2])),
			      parse(Float64, p[3]), parse(Float64, p[4]), parse(Float64, p[5]), parse(Float64, p[6]),
			      Cint(parse(Int, p[7])), parse(Float64, p[8]), parse(Float64, p[9]), parse(Float64, p[10]),
			      parse(Float64, p[11]), String(p[12]), "")
		catch e
			@warn "session: skipped a malformed polygon line" exception=(e,)
		end
	end
	return
end

# Serialize the window's fault traces + slip-model patches (the Polygon kinds serialize_polys skips).
function _serialize_faults_raw(h::Ptr{Cvoid})::String
	n = ccall(_fn(:gmtvtk_serialize_faults), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
	n <= 0 && return ""
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_serialize_faults), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
	return unsafe_string(pointer(buf))
end

# Parse "x,y|x,y|…" -> flat Float64[x0,y0,x1,y1,…] (2 per vertex) + vertex count.
function _parse_xy2(blob::AbstractString)
	xy = Float64[]
	for vp in split(blob, '|'; keepempty=false)
		c = split(vp, ',')
		length(c) < 2 && continue
		push!(xy, parse(Float64, c[1]), parse(Float64, c[2]))
	end
	return xy, length(xy) ÷ 2
end

# Rebuild faults + slip models from a saved blob (inverse of gmtvtk_serialize_faults). Fault (F) lines
# rebuild one at a time (add_fault_geom_h when the plane geometry is known, add_fault_h otherwise);
# slip (S) lines are grouped by name and rebuilt as one batch per group via gmtvtk_add_slip_patches_h.
function _session_rebuild_faults!(fig, blob::String)
	(fig === nothing || isempty(blob)) && return
	h = getfield(fig, :h)
	# collect slip patches per group (in file order) for a single batched add each
	groups = String[]; gpatch = Dict{String,Vector{Vector{SubString{String}}}}()
	for line in split(blob, '\n'; keepempty=false)
		try
			if startswith(line, "F;")
				p = split(line, ';'; limit=10)
				length(p) < 10 && continue
				slip = parse(Float64, p[2]); rake = parse(Float64, p[3])
				strike = parse(Float64, p[4]); dip = parse(Float64, p[5])
				width = parse(Float64, p[6]); depthTop = parse(Float64, p[7]); geog = parse(Int, p[8])
				xy, nv = _parse_xy2(p[10]); nv < 2 && continue
				if isnan(strike) || isnan(dip) || isnan(width) || isnan(depthTop)
					ccall(_fn(:gmtvtk_add_fault_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble),
					      h, xy, Cint(nv), slip, rake)
				else
					ccall(_fn(:gmtvtk_add_fault_geom_h), Cint,
					      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cint),
					      h, xy, Cint(nv), slip, rake, strike, dip, width, depthTop, Cint(geog))
				end
			elseif startswith(line, "S;")
				p = split(line, ';'; limit=12)
				length(p) < 12 && continue
				g = String(p[2])
				g in groups || push!(groups, g)
				push!(get!(() -> Vector{Vector{SubString{String}}}(), gpatch, g), p)
			elseif startswith(line, "N;")   # "Nested grids" rectangle: N;xi;yi;reg;name;verts
				p = split(line, ';'; limit=6)
				length(p) < 6 && continue
				xi = parse(Float64, p[2]); yi = parse(Float64, p[3]); reg = parse(Int, p[4])
				name = String(p[5])
				xy, nv = _parse_xy2(p[6])
				nv < 2 && continue
				ccall(_fn(:gmtvtk_add_nested_rect), Cint,
				      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cint, Cstring),
				      h, xy, Cint(nv), xi, yi, Cint(reg), name)
			end
		catch e
			@warn "session: skipped a malformed fault line" exception=(e,)
		end
	end
	# one gmtvtk_add_slip_patches_h per group
	for g in groups
		try
			ps = gpatch[g]
			np = length(ps)
			xy = Float64[]; vcounts = Cint[]; rgb = Float64[]
			slip = Float64[]; rake = Float64[]; strike = Float64[]; dip = Float64[]; depthTop = Float64[]; seg = Cint[]
			dx = 0.0; dy = 0.0
			for (k, p) in enumerate(ps)
				vx, nv = _parse_xy2(p[12]); nv < 3 && continue
				append!(xy, vx); push!(vcounts, Cint(nv))
				push!(slip, parse(Float64, p[3])); push!(rake, parse(Float64, p[4]))
				push!(strike, parse(Float64, p[5])); push!(dip, parse(Float64, p[6]))
				push!(depthTop, parse(Float64, p[7])); push!(seg, Cint(parse(Int, p[10])))
				fc = split(p[11], ','); append!(rgb, (parse(Float64, fc[1]), parse(Float64, fc[2]), parse(Float64, fc[3])))
				k == 1 && (dx = parse(Float64, p[8]); dy = parse(Float64, p[9]))   # model-wide length/width
			end
			isempty(vcounts) && continue
			ccall(_fn(:gmtvtk_add_slip_patches_h), Cint,
			      (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cint}, Cint, Ptr{Cdouble}, Cstring,
			       Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Cdouble, Cdouble, Ptr{Cint}),
			      h, xy, vcounts, Cint(length(vcounts)), rgb, g,
			      slip, rake, strike, dip, depthTop, dx, dy, seg)
		catch e
			@warn "session: skipped a malformed slip group" group=g exception=(e,)
		end
	end
	return
end

# Rebuild the text labels from a saved blob into `fig`'s window via gmtvtk_add_text_h (one per line).
# Lines are "x;y;r;g;b;size;text" (text last, may contain ';'). Malformed lines are skipped.
"One parsed label out of the text serializer's blob."
const TextRow = @NamedTuple{x::Float64, y::Float64, r::Float64, g::Float64, b::Float64,
                            size::Int, group::String, text::String}

# THE parser for `gmtvtk_serialize_texts`' blob — session rebuild and the GMT.jl script export read
# labels through this one function, so a field added on the C side can never be understood two ways
# (SACRED_LAW: one operation, one function). Lines are "x;y;r;g;b;size;group;text"; a 7-field line is
# a blob written before the group field existed (an older session zip) and reads as group="".
function _parse_texts_blob(blob::String)::Vector{TextRow}
	out = TextRow[]
	isempty(blob) && return out
	for line in split(blob, '\n'; keepempty=false)
		p = split(line, ';'; limit=8)
		length(p) < 7 && continue
		x = tryparse(Float64, p[1]); y = tryparse(Float64, p[2])
		r = tryparse(Float64, p[3]); g = tryparse(Float64, p[4]); b = tryparse(Float64, p[5])
		sz = tryparse(Int, p[6])
		(x === nothing || y === nothing || r === nothing || g === nothing || b === nothing || sz === nothing) && continue
		grp, txt = length(p) >= 8 ? (String(p[7]), String(p[8])) : ("", String(p[7]))
		push!(out, (x=x, y=y, r=r, g=g, b=b, size=sz, group=grp, text=txt))
	end
	return out
end

# ── the vector snapshots: overlays, symbol layers, rulers ────────────────────────────────────
# THE parsers for the three remaining `gmtvtk_serialize_*` blobs, same rule as `_parse_texts_blob`
# above: Save Session and the GMT.jl script export are two CONSUMERS of one snapshot, never two
# snapshots (SACRED_LAW). Vertex strings go through `_script_verts` — the one "x,y,z|…" reader.

"One parsed line/point overlay out of `gmtvtk_serialize_overlays`' blob."
const OverlayRow = @NamedTuple{mode::Int, r::Float64, g::Float64, b::Float64, lw::Float64,
                               ps::Float64, lstyle::Int, stack::Int, visible::Bool,
                               group::String, name::String, segs::Vector{Matrix{Float64}}}

function _parse_overlays_blob(blob::String)::Vector{OverlayRow}
	out = OverlayRow[]
	isempty(blob) && return out
	for line in split(blob, '\n'; keepempty=false)
		f = split(line, ';'; limit=12)
		length(f) < 12 && continue
		nums = map(i -> tryparse(Float64, f[i]), 1:9)
		any(isnothing, nums) && continue
		segs = filter(!isempty, [_script_verts(String(sg)) for sg in split(f[12], '>'; keepempty=false)])
		isempty(segs) && continue
		push!(out, (mode=Int(nums[1]), r=nums[2], g=nums[3], b=nums[4], lw=nums[5], ps=nums[6],
		            lstyle=Int(nums[7]), stack=Int(nums[8]), visible=nums[9] != 0,
		            group=String(f[10]), name=String(f[11]), segs=segs))
	end
	return out
end

"""
One parsed symbol layer out of `gmtvtk_serialize_symbols`' blob. `xyz` holds the points AS STORED —
x is still multiplied by `xfac` (`addSymbols` bakes it in); `_symbol_layer` un-bakes it for GMT, and
the session rebuild hands them back to a C side that bakes it again, so neither may do it here.
`scale` / `rgb` are empty when the layer has no per-point size / colour.
"""
const SymbolRow = @NamedTuple{sym::String, sizePx::Float64, filled::Bool,
                              r::Float64, g::Float64, b::Float64,
                              er::Float64, eg::Float64, eb::Float64, ew::Float64, evis::Bool,
                              stack::Int, visible::Bool, oneShot::Bool, name::String,
                              xyz::Matrix{Float64}, scale::Vector{Float64}, rgb::Matrix{Float64}}

function _parse_symbols_blob(blob::String)::Vector{SymbolRow}
	out = SymbolRow[]
	isempty(blob) && return out
	for line in split(blob, '\n'; keepempty=false)
		f = split(line, ';'; limit=18)
		length(f) < 18 && continue
		nums = map(i -> tryparse(Float64, f[i]), 2:16)
		any(isnothing, nums) && continue
		hasScale = nums[14] != 0; hasRGB = nums[15] != 0
		pts = split(f[18], '|'; keepempty=false)
		isempty(pts) && continue
		xyz = Matrix{Float64}(undef, length(pts), 3)
		scale = hasScale ? Vector{Float64}(undef, length(pts)) : Float64[]
		rgb = hasRGB ? Matrix{Float64}(undef, length(pts), 3) : Matrix{Float64}(undef, 0, 3)
		bad = false
		for (i, p) in enumerate(pts)
			c = split(p, ',')
			# 3 coords, then the optional per-point size scale, then the optional r,g,b (0-255).
			length(c) < 3 + (hasScale ? 1 : 0) + (hasRGB ? 3 : 0) && (bad = true; break)
			for k in 1:3
				v = tryparse(Float64, c[k]); v === nothing && (bad = true; break)
				xyz[i, k] = v
			end
			bad && break
			j = 4
			if hasScale
				v = tryparse(Float64, c[j]); v === nothing && (bad = true; break)
				scale[i] = v; j += 1
			end
			if hasRGB
				for k in 1:3
					v = tryparse(Float64, c[j]); v === nothing && (bad = true; break)
					rgb[i, k] = v / 255; j += 1
				end
				bad && break
			end
		end
		bad && continue
		push!(out, (sym=String(f[1]), sizePx=nums[1], filled=nums[2] != 0,
		            r=nums[3], g=nums[4], b=nums[5], er=nums[6], eg=nums[7], eb=nums[8],
		            ew=nums[9], evis=nums[10] != 0, stack=Int(nums[11]), visible=nums[12] != 0,
		            oneShot=nums[13] != 0, name=String(f[17]), xyz=xyz, scale=scale, rgb=rgb))
	end
	return out
end

"One parsed ruler measurement out of `gmtvtk_serialize_rulers`' blob (its clicked vertices)."
const RulerRow = @NamedTuple{id::Int, verts::Matrix{Float64}}

function _parse_rulers_blob(blob::String)::Vector{RulerRow}
	out = RulerRow[]
	isempty(blob) && return out
	for line in split(blob, '\n'; keepempty=false)
		f = split(line, ';'; limit=2)
		length(f) < 2 && continue
		id = tryparse(Int, f[1]); id === nothing && continue
		V = _script_verts(String(f[2]))
		size(V, 1) < 2 && continue
		push!(out, (id=id, verts=V))
	end
	return out
end

for (fname, sym) in ((:_serialize_overlays_raw, :gmtvtk_serialize_overlays),
                     (:_serialize_symbols_raw,  :gmtvtk_serialize_symbols),
                     (:_serialize_rulers_raw,   :gmtvtk_serialize_rulers))
	@eval function $fname(h::Ptr{Cvoid})::String
		n = ccall(_fn($(QuoteNode(sym))), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, C_NULL, Cint(0))
		n <= 0 && return ""
		buf = Vector{UInt8}(undef, n + 1)
		ccall(_fn($(QuoteNode(sym))), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, Cint(n + 1))
		return unsafe_string(pointer(buf))
	end
end

# Show/hide one vector element by its Scene Objects name, whatever family it belongs to (overlay,
# symbol layer, polygon, text) — the one C setter, so nothing here needs to know the container.
_set_vector_visible(h::Ptr{Cvoid}, name::String, vis::Bool) =
	ccall(_fn(:gmtvtk_set_vector_visible_h), Cint, (Ptr{Cvoid}, Cstring, Cint), h, name, Cint(vis))

# Rebuild the line/point overlays from a saved blob via gmtvtk_add_overlay_ex_h — the SAME export every
# live import uses — then stamp back the style bits the add call has no argument for (line style, and
# an unchecked row's hidden state) through gmtvtk_set_overlay_style_h. `skip` holds names that some
# RECIPE already put back (coastlines, plate boundaries, city layers …): those elements are rebuilt by
# their own recipe, so re-adding them here would double every one of them.
function _session_rebuild_overlays!(fig, blob::String, skip::Set{String})
	(fig === nothing || isempty(blob)) && return
	h = getfield(fig, :h)
	# The blob's `lw` is the actor's width in PIXELS; gmtvtk_add_overlay_ex_h takes POINTS and converts
	# with the render window's own DPI (162 on a HiDPI display, not 72) — hand it pixels and every pen
	# comes back 2.25x heavier. Converted here, with the DPI the window itself reports, so a session
	# reopened on a different display restores the same POINT width rather than the same pixel count.
	dpi = Float64(get(_parse_scene_state(_scene_state_full_raw(h)), "dpi", 72))
	dpi > 0 || (dpi = 72.0)
	for ov in _parse_overlays_blob(blob)
		ov.name in skip && continue
		try
			npts = sum(M -> size(M, 1), ov.segs)
			xyz = Vector{Float64}(undef, 3npts)
			segoff = Cint[0]; k = 0
			for M in ov.segs
				for i in 1:size(M, 1)
					xyz[3k+1] = M[i, 1]; xyz[3k+2] = M[i, 2]; xyz[3k+3] = M[i, 3]; k += 1
				end
				push!(segoff, Cint(k))
			end
			GC.@preserve xyz segoff ccall(_fn(:gmtvtk_add_overlay_ex_h), Cint,
				(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
				 Cdouble, Cdouble, Cstring, Cstring, Cstring),
				h, xyz, Cint(npts), segoff, Cint(length(segoff) - 1), Cint(ov.mode),
				ov.r, ov.g, ov.b, ov.lw * 72 / dpi, ov.ps, ov.name, ov.group, "")
			# dashed/dotted is not an argument of the add call — it is applied afterwards, by the same
			# setter the Line Properties dialog uses (opacity 1.0: the blob carries no transparency).
			ov.lstyle != 0 && ccall(_fn(:gmtvtk_set_overlay_style_h), Cint,
			                        (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble),
			                        h, ov.name, ov.r, ov.g, ov.b, ov.lw, Cint(ov.lstyle), 1.0)
			ov.visible || _set_vector_visible(h, ov.name, false)
		catch e
			@warn "session: skipped a malformed overlay line" name=ov.name exception=(e,)
		end
	end
	return
end

# Rebuild the symbol layers from a saved blob via gmtvtk_add_symbols_ex_h — the same export the live
# plotters use, per-point size scale and colour included, so a seismicity catalog comes back scaled by
# magnitude and coloured by depth instead of as uniform dots. `skip` as above (a :focal or :geography
# recipe has already replayed its own layers).
function _session_rebuild_symbols!(fig, blob::String, skip::Set{String})
	(fig === nothing || isempty(blob)) && return
	h = getfield(fig, :h)
	xf = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), h)
	isfinite(xf) && xf != 0.0 || (xf = 1.0)
	for sl in _parse_symbols_blob(blob)
		sl.name in skip && continue
		try
			n = size(sl.xyz, 1)
			xyz = Vector{Float64}(undef, 3n)
			for i in 1:n
				# x comes out of the blob still baked; addSymbols bakes it again -> un-bake it once here,
				# exactly as `_symbol_layer` does for the script (SACRED_LAW: the same correction, in the
				# consumer, never a second copy of the factor inside C).
				xyz[3i-2] = sl.xyz[i, 1] / xf; xyz[3i-1] = sl.xyz[i, 2]; xyz[3i] = sl.xyz[i, 3]
			end
			scale = isempty(sl.scale) ? C_NULL : pointer(sl.scale)
			rgbv = Float64[]
			if !isempty(sl.rgb)
				rgbv = Vector{Float64}(undef, 3n)
				for i in 1:n, k in 1:3; rgbv[3(i-1)+k] = sl.rgb[i, k]; end
			end
			GC.@preserve xyz sl rgbv ccall(_fn(:gmtvtk_add_symbols_ex_h), Cint,
				(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cstring, Cdouble, Cint,
				 Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble,
				 Cstring, Cstring, Ptr{Cdouble}, Ptr{Cdouble}),
				h, xyz, Cint(n), sl.sym, sl.sizePx, Cint(sl.filled),
				sl.r, sl.g, sl.b, sl.er, sl.eg, sl.eb, sl.ew,
				sl.name, "", scale, isempty(rgbv) ? C_NULL : pointer(rgbv))
			sl.visible || _set_vector_visible(h, sl.name, false)
		catch e
			@warn "session: skipped a malformed symbol layer" name=sl.name exception=(e,)
		end
	end
	return
end

# Rebuild the ruler measurements: vertices only, re-measured on the spot by the ruler machinery itself
# (gmtvtk_add_ruler_h). Nothing to skip — no recipe ever draws a ruler.
function _session_rebuild_rulers!(fig, blob::String)
	(fig === nothing || isempty(blob)) && return
	h = getfield(fig, :h)
	for r in _parse_rulers_blob(blob)
		try
			n = size(r.verts, 1)
			xyz = Vector{Float64}(undef, 3n)
			for i in 1:n, k in 1:3; xyz[3(i-1)+k] = r.verts[i, k]; end
			GC.@preserve xyz ccall(_fn(:gmtvtk_add_ruler_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint),
			                       h, xyz, Cint(n))
		catch e
			@warn "session: skipped a malformed ruler line" id=r.id exception=(e,)
		end
	end
	return
end

function _session_rebuild_texts!(fig, blob::String)
	(fig === nothing || isempty(blob)) && return
	h = getfield(fig, :h)
	for t in _parse_texts_blob(blob)
		ccall(_fn(:gmtvtk_add_text_h), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cstring, Cdouble, Cdouble, Cdouble, Cint),
		      h, t.x, t.y, t.text, t.r, t.g, t.b, Cint(t.size))
	end
	return
end

"File > Save Session: write the `scene` window's recipes + generated data to a `.igmtz` zip at `path`."
function _on_save_session(scene::Ptr{Cvoid}, path::String)
	recipes = get(_SESSION_LOG, scene, ElementRecipe[])
	files = Tuple{String,Vector{UInt8}}[]
	out = ElementRecipe[]; used = Set{String}(); seen = Set{Tuple{Symbol,String}}()
	for r in recipes
		if r.origin === :generated
			# Dedup: an element can get logged more than once (e.g. a nested layer re-materialized); a
			# given (kind,name) is one Scene Objects element, so serialize + replay it exactly once.
			(r.kind, r.name) in seen && continue
			push!(seen, (r.kind, r.name))
			id, bytes = _session_pack_generated(scene, r, used)
			push!(files, ("data/" * id, bytes))
			push!(out, ElementRecipe(r.kind, :generated, id; name=r.name, params=copy(r.params)))
		else
			# Menu vector layers (coastlines/borders/rivers) replay with the default pen; capture the
			# live overlay's current pen so Line-properties edits (colour/width/style/opacity) survive.
			r.kind === :geography && !isempty(r.name) && (r = _session_capture_overlay_pen(scene, r))
			push!(out, r)
		end
	end
	# Curtains built from in-memory GMTimages: write their stashed PNG textures as sidecars (file-path
	# curtains keep only the path in their recipe).
	cimgs = get(_CURTAIN_IMG, scene, Dict{String,Vector{UInt8}}())
	for r in out
		if r.kind === :curtain && get(r.params, "image_origin", "") == "generated"
			b = get(cimgs, get(r.params, "image", ""), nothing)
			b !== nothing && push!(files, ("data/" * r.params["image"], b))
		end
	end
	meta = Dict("schema" => "1", "saved" => Libc.strftime("%Y-%m-%dT%H:%M:%S", time()))
	manifest = _session_write_manifest(meta, _session_display(scene), out)
	push!(files, ("session.manifest", Vector{UInt8}(codeunits(manifest))))
	# C++-drawn elements (no add-time recipe) are snapshotted here and rebuilt on load: polygons,
	# faults/slip models + nested rects, text, line/point overlays, symbol layers, rulers. EVERY vector
	# family the Scene holds is in this list — an element with no recipe and no snapshot is an element
	# the session loses, which is what "several graphics elements missing" was.
	for (entry, blob) in (("drawn/polys.txt",    _serialize_polys_raw(scene)),
	                      ("drawn/faults.txt",   _serialize_faults_raw(scene)),
	                      ("drawn/texts.txt",    _serialize_texts_raw(scene)),
	                      ("drawn/overlays.txt", _serialize_overlays_raw(scene)),
	                      ("drawn/symbols.txt",  _serialize_symbols_raw(scene)),
	                      ("drawn/rulers.txt",   _serialize_rulers_raw(scene)))
		isempty(blob) || push!(files, (entry, Vector{UInt8}(codeunits(blob))))
	end
	_zip_write(path, files)
	# A saved session is a file the user opens again — it goes in Recent Files (cat 3 = Sessions),
	# exactly like every successful open does. Saved AND loaded sessions both register, so a session
	# is reachable from the menu straight after writing it, not only after loading it once.
	ccall(_fn(:gmtvtk_add_recent), Cvoid, (Cstring, Cint), abspath(String(path)), Cint(3))
	return path
end

# Rehydrate a recipe's data object: read the disk source (:file) or the zip sidecar (:generated);
# :menu recipes carry no data (rebuilt from params). Returns the GMT object or nothing.
function _session_load_object(r::ElementRecipe, entries::Dict{String,Vector{UInt8}})
	if r.kind === :curtain                               # returns the curtain's image PATH, not a GMT object
		if get(r.params, "image_origin", "") == "file"
			return get(r.params, "image", "")
		end
		id = get(r.params, "image", ""); key = "data/" * id
		haskey(entries, key) || (@warn "session: curtain texture missing, skipped" key=key; return nothing)
		tmp = tempname() * ".png"; write(tmp, entries[key]); return tmp
	end
	if r.origin === :file
		isfile(r.source) || (@warn "session: source file missing, layer skipped" file=r.source; return nothing)
		return _gmtread_trb(r.source)      # grids are READ in "TRB" — THE reader (non-grids pass through)
	elseif r.origin === :generated
		key = "data/" * r.source
		haskey(entries, key) || (@warn "session: sidecar missing, layer skipped" key=key; return nothing)
		tmp = tempname() * splitext(r.source)[2]; write(tmp, entries[key])
		data = _gmtread_trb(tmp); rm(tmp; force=true)
		return data
	end
	return nothing
end

# Rebuild a curtain into `fig`'s grid window from its recipe + texture path. add_curtain! is called
# with record=false (it would otherwise re-log the temp texture path as a :file curtain); provenance
# is re-recorded here with the ORIGINAL recipe params, re-stashing the texture bytes for a re-save.
function _session_replay_curtain!(fig::QtFigure, r::ElementRecipe, imgpath::String)
	toks = split(get(r.params, "track", ""), '|'; keepempty=false)
	length(toks) < 2 && return
	P = Matrix{Float64}(undef, length(toks), 2)
	for (i, t) in enumerate(toks)
		c = split(t, ',')
		length(c) < 2 && return
		P[i, 1] = parse(Float64, c[1]); P[i, 2] = parse(Float64, c[2])
	end
	zr = (parse(Float64, r.params["zmin"]), parse(Float64, r.params["zmax"]))
	add_curtain!(fig, P; image=imgpath, zrange=zr, spacing=Symbol(get(r.params, "spacing", "distance")),
	             flipv=parse(Bool, get(r.params, "flipv", "false")), clip=parse(Bool, get(r.params, "clip", "false")),
	             clip_n=parse(Int, get(r.params, "clip_n", "300")), record=false)
	h = getfield(fig, :h)
	_session_record!(h, :curtain, :menu; params=copy(r.params))
	get(r.params, "image_origin", "") == "generated" && _curtain_img_store!(h, r.params["image"], read(imgpath))
	return
end

# Restore an element's name (Scene Objects label / surface name). Extras already carry it (added by
# name); this is for the base surface, which view_grid/view_image open unnamed.
_session_set_name!(h::Ptr{Cvoid}, name::String) =
	isempty(name) || ccall(_fn(:gmtvtk_set_surface_name_h), Cvoid, (Ptr{Cvoid}, Cstring), h, name)

# Pick the Scene* a menu BASE layer (e.g. basemap) should build into: the invoking window if it is a
# bare launcher (load in place), else a fresh empty launcher. The menu callback promotes it itself.
_session_base_scene(target::Ptr{Cvoid})::Ptr{Cvoid} =
	(target != C_NULL && ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), target) == 0) ?
		target : getfield(iview(), :h)

# Save-time: read a menu overlay's live pen (colour/width/style/opacity) into the recipe params so
# Line-properties edits survive a round-trip. Returns `r` unchanged if the overlay isn't found.
function _session_capture_overlay_pen(scene::Ptr{Cvoid}, r::ElementRecipe)::ElementRecipe
	out = zeros(Float64, 6)
	found = ccall(_fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), scene, r.name, out)
	found == 0 && return r
	p = copy(r.params)
	p["pen_r"] = out[1]; p["pen_g"] = out[2]; p["pen_b"] = out[3]
	p["pen_w"] = out[4]; p["pen_style"] = Int(round(out[5])); p["pen_op"] = out[6]
	return ElementRecipe(r.kind, r.origin, r.source; name=r.name, params=p)
end

# Load-time: re-apply a captured pen to the just-regenerated overlay. No-op if none was saved. Values
# arrive as strings from the manifest, so parse each (string() first covers both string + Float64).
function _session_apply_overlay_pen!(h::Ptr{Cvoid}, r::ElementRecipe)
	haskey(r.params, "pen_r") || return
	getp(k, d) = (v = get(r.params, k, nothing); v === nothing ? d : parse(Float64, string(v)))
	ccall(_fn(:gmtvtk_set_overlay_style_h), Cint,
	      (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble),
	      h, r.name, getp("pen_r", 0.0), getp("pen_g", 0.0), getp("pen_b", 0.0),
	      getp("pen_w", 1.0), round(Int, getp("pen_style", 0.0)), getp("pen_op", 1.0))
	return
end

# Replay one recipe. The FIRST layer (fig === nothing) creates the base window: if `target` is an
# EMPTY launcher (has_surface == 0) it is promoted IN PLACE — the session loads into the window the
# user invoked Load from — otherwise a new window opens. Later layers add onto that window as extras.
# P1 handles grids/images; menu layers, vectors and geophysics (P2/P3) are logged and skipped. A
# missing source (obj === nothing) skips that layer but keeps the window. The add path re-records
# provenance for the REBUILT window (its own Scene*), passing the disk path back for :file layers so a
# re-save keeps the file ref (not the data). Base layers restore the saved element name.
function _session_replay!(fig, r::ElementRecipe, obj, display, target::Ptr{Cvoid})
	src = r.origin === :file ? r.source : ""             # keep :file ref on re-save; else data (:generated)
	if fig === nothing                                   # base layer: create the window
		if r.kind === :basemap                           # menu base (no data): the callback frames + adds it
			sc = _session_base_scene(target)
			_on_basemap(sc, get(r.params, "copt", ""))
			return get(_FIGREG, sc, QtEmpty(sc))
		end
		obj === nothing && (@warn "session: base layer data missing; window not opened"; return nothing)
		# Promote the invoking window when it is a bare launcher; else open a fresh window.
		promote = target != C_NULL && ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), target) == 0
		if r.kind === :basegrid
			promote && (_add_grid_to_scene(target, obj, r.name; promote=true, source=src); return get(_FIGREG, target, nothing))
			f = view_grid(obj); _session_set_name!(getfield(f, :h), r.name); return f
		elseif r.kind === :image
			promote && (_add_image_to_scene(target, obj, r.name; promote=true, source=src); return get(_FIGREG, target, nothing))
			f = view_image(obj); _session_set_name!(getfield(f, :h), r.name); return f
		end
		@warn "session: first layer kind=$(r.kind) is not a base grid/image; window not opened"
		return nothing
	end
	h = getfield(fig, :h)
	if r.kind === :geography                             # menu layer: re-dispatch the saved request
		_on_geography(h, get(r.params, "req", ""))
		_session_apply_overlay_pen!(h, r)                # re-apply any saved Line-properties edits
		return fig
	elseif r.kind === :basemap                           # basemap tile on top of the existing window
		_on_basemap(h, get(r.params, "copt", ""))
		return fig
	elseif r.kind === :curtain                           # `obj` is the texture PATH from _session_load_object
		(fig isa QtFigure && obj isa AbstractString && !isempty(obj)) && _session_replay_curtain!(fig, r, String(obj))
		return fig
	elseif r.kind === :focal                             # re-dispatch the catalog request (newlines unescaped)
		_on_focal(h, replace(get(r.params, "cparams", ""), '\x1e' => '\n'))
		return fig
	elseif r.kind === :illum                             # re-dispatch the Illumination model onto the rebuilt grid
		# The reflectance is DERIVED, never stored: the same request block, through the same door the
		# dialog uses, recomputes it from the layer that is now on screen. Replayed with the vectors
		# (after every raster), so the grid it lights is already there; the shading state that goes
		# with it lands afterwards, in _session_apply_display! -- which keeps this reflectance
		# (gmtvtk_apply_scene_state restores the look with keepExternShade).
		_on_hillshade(h, replace(get(r.params, "cparams", ""), '\x1e' => '\n'))
		return fig
	end
	obj === nothing && return fig                        # data-backed layers: skip on missing source
	if r.kind in (:basegrid, :dropgrid)
		_add_grid_to_scene(h, obj, r.name; promote=false, source=src)
	elseif r.kind in (:image, :dropimage)
		_add_image_to_scene(h, obj, r.name; promote=false, source=src)
	else
		@warn "session: replay of kind=$(r.kind) not yet implemented; layer skipped"
	end
	return fig
end

# Apply the snapshotted display state (camera/VE/flat2d/colorbar/shading) to the rebuilt window via
# gmtvtk_apply_scene_state. The raw "k=v;" string was stored verbatim under `state` (see
# _session_display). No-op if absent (e.g. a session saved before this state existed).
function _session_apply_display!(fig, display)
	fig === nothing && return
	raw = get(display, "state", "")
	isempty(raw) && return
	ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), getfield(fig, :h), raw)
	return
end

# C callback wrappers: unwrap the Cstring path, run the orchestration, report failures in the Errors
# console (save) or a warning (load, which has no scene to log to). Never let an exception cross back
# into C.
function _on_save_session_cb(scene::Ptr{Cvoid}, cpath::Cstring)::Cvoid
	path = ""
	try
		path = unsafe_string(cpath)
		_on_save_session(scene, path)
		_viewer_log_info(scene, "Saved session -> $path")
	catch e
		_tool_failed(scene, "Save session", e)
	end
	return
end
function _on_load_session_cb(scene::Ptr{Cvoid}, cpath::Cstring)::Cvoid
	try
		_on_load_session(scene, unsafe_string(cpath))
	catch e
		@tool_error "session: could not load" exception=(e,)
	end
	return
end

# Build the C-callable pointers + register them. Lazy (first window) via _ensure_callbacks — the
# @cfunctions are thin invokelatest trampolines so they drag no GMT into compile.
function _register_session()
	sptr = @cfunction((s, c) -> Base.invokelatest(_on_save_session_cb, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_save_session_callback), Cvoid, (Ptr{Cvoid},), sptr)
	lptr = @cfunction((s, c) -> Base.invokelatest(_on_load_session_cb, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_load_session_callback), Cvoid, (Ptr{Cvoid},), lptr)
	return
end

# File > Load Session: rebuild from a `.igmtz` zip. `scene` is the window Load was invoked from — an
# empty launcher is promoted in place (session loads into it); a populated window (or C_NULL from the
# console) opens a fresh window instead. Returns the primary figure (or nothing).
function _on_load_session(scene::Ptr{Cvoid}, path::String)
	# Progress dialog already shown from C++ - convert to determinate and update progress
	ccall(_fn(:gmtvtk_progress_show), Cint, (Cint, Cstring), 100, "Loading Session...")
	try
		entries = _zip_read(path)
		haskey(entries, "session.manifest") || error("session: '$path' has no session.manifest")
		ccall(_fn(:gmtvtk_progress_update), Cvoid, (Cint,), 10)
		_, display, recipes = _session_read_manifest(String(entries["session.manifest"]))
		fig = nothing
		# Replay RASTERS first (grids/images/basemap), then vector/menu layers — vectors are always drawn on
		# top of grids/images, and the shared draw-order pile ranks by add order, so vectors must be added
		# LAST to outrank every raster (else e.g. a grid replayed after coastlines would bury them).
		israster(r) = r.kind in (:basegrid, :image, :dropgrid, :dropimage, :basemap)
		nraster = count(israster, recipes)
		ri = 0
		lastraster = nothing                  # (name, data) of the last raster that actually replayed
		for r in recipes
			if israster(r)
				ri += 1
				ccall(_fn(:gmtvtk_progress_update), Cvoid, (Cint,), round(Int, 10 + 50*ri/nraster))
				obj = _session_load_object(r, entries)
				fig = _session_replay!(fig, r, obj, display, scene)
				(obj === nothing) || (lastraster = (r.name, obj))
			end
		end
		# SACRED_LAW.md, raster-own-axes law: EVERY raster add goes through the ONE transition
		# `_adopt_new_element` — shown, everything else unchecked whatever its kind, axes reframed to
		# its OWN extent — with no gate, no exception for any raster kind or any code path. Session
		# replay was the exception nobody had closed: it added each raster with promote=false and
		# NOTHING ELSE, so the last grid in the file came back with a Scene Objects row and nothing on
		# screen (reported on C:/v/sess.igmtz: tejo10_geo.grd listed, never displayed). Adopting the
		# LAST replayed raster is the same rule a drop of those same files in the same order gives.
		# NO GATE. Not "if it has a name", not "if the window was empty", not "if it is an extra" —
		# the law is explicit that writing such a condition around this call IS the recurring mistake,
		# and it names the three earlier attempts that each added one. The base surface is not a
		# special case either: replay names it (_session_set_name!), and the C side resolves the base
		# by its own surfName and checks it back on when the base is what gets adopted.
		if fig !== nothing && lastraster !== nothing
			_adopt_new_element(getfield(fig, :h), lastraster[1], lastraster[2])
		end
		nvec = length(recipes) - nraster
		vi = 0
		for r in recipes
			if !israster(r)
				vi += 1
				ccall(_fn(:gmtvtk_progress_update), Cvoid, (Cint,), round(Int, 60 + 20*vi/nvec))
				fig = _session_replay!(fig, r, _session_load_object(r, entries), display, scene)
			end
		end
		ccall(_fn(:gmtvtk_progress_update), Cvoid, (Cint,), 85)
		# No raster in the session (or none of them could be replayed) but there ARE drawn elements: they
		# still need a window to land in, or every one of them is silently lost — the rebuilds below all
		# take `fig`. Reuse the invoking window when it is an empty launcher (the same "promote in place"
		# rule every drop/menu path follows), else open one.
		if fig === nothing && any(k -> startswith(k, "drawn/"), keys(entries))
			hv = (scene != C_NULL && ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0) ?
			     scene : ccall(_fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "i'GMT  —  session")
			hv == C_NULL || (fig = get(() -> _register_fig!(QtEmpty(hv)), _FIGREG, hv))
		end
		# C++-drawn elements rebuilt after the layers exist: polygons, faults/slip + nested rects, text
		# labels, line/point overlays, symbol layers, rulers.
		#
		# Overlays and symbol layers are the two families a RECIPE can also produce (coastlines, plate
		# boundaries, isochrons, cities, focal-mechanism anchors …). Those recipes have just replayed, so
		# whatever they already put back is asked of the SCENE ITSELF — the same serializers, read again
		# now — and skipped, instead of guessing from recipe kinds which names they own. A name that is
		# already standing is never added twice.
		haskey(entries, "drawn/polys.txt")  && _session_rebuild_polys!(fig, String(entries["drawn/polys.txt"]))
		haskey(entries, "drawn/faults.txt") && _session_rebuild_faults!(fig, String(entries["drawn/faults.txt"]))
		haskey(entries, "drawn/texts.txt")  && _session_rebuild_texts!(fig, String(entries["drawn/texts.txt"]))
		if fig !== nothing
			hf = getfield(fig, :h)
			have_ov = Set{String}(ov.name for ov in _parse_overlays_blob(_serialize_overlays_raw(hf)))
			have_sl = Set{String}(sl.name for sl in _parse_symbols_blob(_serialize_symbols_raw(hf)))
			haskey(entries, "drawn/overlays.txt") &&
				_session_rebuild_overlays!(fig, String(entries["drawn/overlays.txt"]), have_ov)
			haskey(entries, "drawn/symbols.txt") &&
				_session_rebuild_symbols!(fig, String(entries["drawn/symbols.txt"]), have_sl)
			haskey(entries, "drawn/rulers.txt") &&
				_session_rebuild_rulers!(fig, String(entries["drawn/rulers.txt"]))
		end
		ccall(_fn(:gmtvtk_progress_update), Cvoid, (Cint,), 95)
		# The checkboxes exactly as they were saved, FIRST: they decide which raster is the active one,
		# and the view mode below is refused for a non-geographic active grid (sceneSetViewMode ->
		# igViewIsBody + activeGridGeog). Restoring the view while the wrong layer was still active is
		# how a session saved in Globe or Cube came back flat.
		fig !== nothing && _session_apply_checked!(fig, display)
		# …then the display state — VE, view mode (2-D / 3-D / Globe / Cube), camera, shading. LAST, so
		# what the user saved is what wins over anything the replay order left behind.
		fig !== nothing && _session_apply_display!(fig, display)
		# Same "new content appeared" convention every promote/drop/derive path uses (grid.jl,
		# clipgrid.jl, grdsample.jl, igrf.jl, rtp3d.jl, deform.jl) -- an empty launcher's Scene
		# Objects dock starts FOLDED (gmtvtk_open_empty), so a session replayed straight into one
		# must unfold it too, never leave it collapsed (SACRED_LAW.md derived-variable display law).
		fig !== nothing && ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), getfield(fig, :h))
		# Faults were rebuilt before the display state was applied; re-seat their planes/surface-projection
		# in the final scaled space now (else they only appear after opening+closing the elastic dialog).
		if fig !== nothing && haskey(entries, "drawn/faults.txt")
			ccall(_fn(:gmtvtk_refresh_fault_planes), Cvoid, (Ptr{Cvoid},), getfield(fig, :h))
		end
		# Loaded session -> Recent Files (cat 3 = Sessions), the same registration a successful open
		# of any other file does. Here, not in the callback wrapper, so EVERY route lands it: the File
		# > Load Session menu, a dropped .igmtz, the Recent Files entry itself, and the console helper.
		ccall(_fn(:gmtvtk_add_recent), Cvoid, (Cstring, Cint), abspath(String(path)), Cint(3))
		return fig
	finally
		ccall(_fn(:gmtvtk_progress_close), Cvoid, ())
	end
end
"Console convenience: load a session into a fresh window (no invoking scene)."
_on_load_session(path::String) = _on_load_session(C_NULL, path)
