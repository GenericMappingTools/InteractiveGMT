# bandslist.jl — Image > Load Bands (port of Mirone's src_figs/bands_list.m + the `toBandsList`
# registration in utils/aux_funs.m).
#
# WHAT MIRONE DOES, and what this mirrors. `.vrt` is not a special format there: aux_funs's
# findFileType routes `.vrt` (with `.n1`, `.n14`-`.n17`) to the MULTIBAND open path, GDAL reads it,
# and the file is registered — path, band count, band names — so `bands_list` can later pull any
# single band, or three of them as an R/G/B composite, into the window that is already open. That
# registration is `_bands_register!` here, and any GDAL raster with more than one band gets it
# (Mirone's own rule: the extension list only decides which files take the multiband OPEN path; the
# tool itself works off the band count).
#
# Loading SWAPS what the window shows, exactly as Mirone's push_Load does — a band is a layer of one
# dataset, not a new quantity, so it goes through the "replace the displayed raster" door, not the
# derived-variable one.
#
# WHAT A BAND IS — pixels or data, and the cut is at Int16:
#   Byte, UInt16          PIXELS. A picture, kept as a GMTimage. The non-8-bit part of that is the
#                         shared image path's job (`_IMG_ORIG` + `_stretch_to_u8`), not this file's.
#   Int16 and wider       DATA. Elevation, anomalies, whatever the stack holds — kept as a GMTgrid so
#                         the values keep their own z range, colour bar and hover readout (Mirone's
#                         "Pretend this a GMT grid", push_Load, which flags exactly uint16/int16 as
#                         `haveGrid`). They are shown ONE AT A TIME and never composed into RGB.
# GDAL already draws that line when it hands a band over (Byte/UInt16 -> GMTimage, Int16 and wider ->
# GMTgrid), so nothing here converts one kind into the other.

# What a window's bands come from: a file on disk, or the PCA components computed from it (which
# exist only in RAM — there is no file to re-read them from). One registry, one lookup.
const _BANDS_INFO = Dict{Ptr{Cvoid},NamedTuple{(:path, :n, :names, :bytes, :isdata),
                                               Tuple{String,Int,Vector{String},Bool,Bool}}}()
# The PCA result, kept as the ONE cube `GMT.pca` returned — never sliced into per-component copies.
# A satellite stack is big (a Sentinel-2 scene is 120 M pixels a band), so holding the components
# separately would mean that many more full-size arrays alive for as long as the window is open.
# Slicing on demand keeps ONE. The kind is whatever `pca` answered: an image cube for a picture
# stack, a grid cube for a data stack (see `_bands_pca`).
const _BANDS_PCA  = Dict{Ptr{Cvoid},Union{GMTgrid,GMTimage}}()

# The extensions Mirone sends down the multiband path by name alone (aux_funs findFileType). This
# list only widens the door: a raster of any other extension still qualifies by its band count.
const _BANDS_EXT = (".vrt", ".n1", ".n14", ".n15", ".n16", ".n17")

# Band count + one name per band, from GDAL. Names follow Mirone's own precedence (mirone.m:2085):
# the band's Description, else a `Band_*` metadata entry, else the source files a VRT points at, else
# "Band_k". `_emp_att` (empilhador.jl) is THE gdalinfo parse in this package — never a second one.
function _bands_probe(path::String)
	att = _emp_att(path)
	n = att.nbands
	n < 1 && return (0, String[], false, false)
	info = GMT.gdalinfo(_emp_vsi(path))
	names = String[]
	for m in eachmatch(r"Band \d+ Block=[^\n]*\n(?:\s+Description\s*=\s*([^\n\r]+))?", info)
		push!(names, m.captures[1] === nothing ? "" : String(strip(String(m.captures[1]))))
	end
	length(names) == n || (names = fill("", n))
	if all(isempty, names)                                    # no Description: `Band_*` metadata
		meta = [s for s in att.meta if occursin("Band_", s) && occursin('=', s)]
		length(meta) == n &&
			(names = [replace(String(strip(String(split(s, '=', limit = 2)[2]))), ' ' => '_') for s in meta])
	end
	if all(isempty, names)                                    # a VRT: name the bands after its sources
		# gdalinfo prints the FIRST file on the "Files:" line itself and the rest indented under it —
		# the container first, then one source per band, which is the order the bands are in.
		files = String[]
		mf = match(r"Files:[ \t]*([^\n]*)\n((?:[ \t]+\S[^\n]*\n)*)", info)
		if mf !== nothing
			for ln in vcat(String(mf.captures[1]), split(String(mf.captures[2]), '\n'))
				s = String(strip(ln))
				(isempty(s) || lowercase(s) == lowercase(path) || lowercase(s) == lowercase(_emp_vsi(path))) && continue
				push!(files, first(splitext(basename(s))))
			end
		end
		length(files) == n && (names = files)
	end
	for k in 1:n
		isempty(names[k]) && (names[k] = "Band_$k")
	end
	# Two different questions, two different answers:
	#  `bytes` — is band 1 8-bit? A 3/4-band Byte file is an RGB(A) PHOTO and opens as the picture it
	#            is, rather than as a stack to pick bands from (Mirone's multiband open).
	#  `isdata` — are the bands DATA rather than pixels? Byte and UInt16 are PIXELS (photographs,
	#            satellite reflectance). Int16 and wider — Int16, Int32, the floats — are
	#            MEASUREMENTS: elevation, anomalies. They display one at a time AS GRIDS, with their
	#            own z range, colour bar and hover readout, and cannot be composed into an RGB
	#            picture (Mirone disables that, bands_list.m:72-76). GDAL draws exactly the same
	#            line: Byte/UInt16 arrive as GMTimage, Int16 and wider as GMTgrid.
	bytes  = occursin(r"Band 1 Block=[^\n]*Type=Byte", info)
	isdata = !bytes && !occursin(r"Band 1 Block=[^\n]*Type=UInt16", info)
	return (n, names, bytes, isdata)
end

# Register `path` as the multiband source behind `scene`'s display, so Image > Load Bands can serve
# it. Returns the band count (0 = not multiband, nothing registered).
function _bands_register!(scene::Ptr{Cvoid}, path::String)::Int
	base = first(split(path, '?'))
	(isfile(base) || startswith(base, "/vsi")) || return 0
	n, names, bytes, isdata = try
		_bands_probe(path)
	catch e
		@debug "Load Bands: probe failed" exception = (e,)
		return 0
	end
	(n > 1 || lowercase(splitext(base)[2]) in _BANDS_EXT) || return 0
	n < 1 && return 0
	_BANDS_INFO[scene] = (path = path, n = n, names = names, bytes = bytes, isdata = isdata)
	delete!(_BANDS_PCA, scene)          # a freshly opened file replaces any previous components
	return n
end

# ONE band, exactly as GDAL hands it over — see the pixels/data split at the top of this file.
# Byte and UInt16 come back as a GMTimage (a picture; `_add_image_to_scene` stashes the original in
# `_IMG_ORIG` and stretches it, so "Auto histogram stretch" has full precision to work from); Int16
# and wider come back as a GMTgrid and are shown as the data they are. No conversion either way.
function _bands_get(scene::Ptr{Cvoid}, k::Int)
	P = get(_BANDS_PCA, scene, nothing)
	P !== nothing && return _bands_pca_layer(P, k)
	return GMT.gdaltranslate(_emp_vsi(_BANDS_INFO[scene].path), ["-b", string(k)])
end

# Can these bands be composed into an RGB picture at all? Only PIXEL bands can — Byte and UInt16.
# Int16 and wider are measurements: they are grids, they carry no colour scale in common, and Mirone
# refuses to compose them (bands_list.m:72-76). PCA COMPONENTS are the exception: they are grids too,
# but they are components of ONE analysis on a common scale, and their false-colour composite is the
# point of running a PCA — GMT's `mat2img` scales them, so composing them is always offered.
function _bands_rgb_ok(scene::Ptr{Cvoid})::Bool
	P = get(_BANDS_PCA, scene, nothing)
	P !== nothing && return size(P, 3) >= 3         # components ALWAYS compose (see _bands_pca_rgb)
	info = get(_BANDS_INFO, scene, nothing)
	return info !== nothing && !info.isdata
end

# Three bands as one RGB picture — Mirone's RGB Color mode. Nothing is scaled on this side: a
# non-8-bit composite reaches `_add_image_to_scene` at full precision and is stretched there.
function _bands_get_rgb(scene::Ptr{Cvoid}, r::Int, g::Int, b::Int)
	info = _BANDS_INFO[scene]
	P = get(_BANDS_PCA, scene, nothing)
	P !== nothing && return _bands_pca_rgb(P, r, g, b)   # components compose whatever the source was
	return _bands_read_rgb_file(info.path, r, g, b)
end

# Three bands of the FILE as ONE picture, handed over RAW. A UInt16 satellite composite is NOT
# stretched here: `_add_image_to_scene` (drop.jl) already owns that step for every non-8-bit image —
# it stashes the original in `_IMG_ORIG` and stretches with `_stretch_to_u8`, which is what makes the
# row's "Auto histogram stretch" work. Stretching here would be a second implementation of that AND
# would throw the full-precision original away before the shared path ever saw it.
#
# FLOAT bands are not composable: Mirone disables RGB Color for them (bands_list.m:72-76, `if
# (isa(handles.image_bands,'single'))` -> force Gray Scale), because three different physical
# quantities share no common colour scale. The caller checks first; this is the backstop.
function _bands_read_rgb_file(path::String, r::Int, g::Int, b::Int)
	C = GMT.gdaltranslate(_emp_vsi(path), ["-b", string(r), "-b", string(g), "-b", string(b)])
	C isa GMTimage || error("these are data bands (Int16 or wider) — show them one at a time (Gray Scale)")
	return C
end

# ---------------------------------------------------------------------------------------------
# Compute PCA — Mirone's push_pca_CB, through GMT.jl's `pca` and NOTHING else. One public call: no
# reaching into `princomp!`, `GI2vectors` or any other GMT internal from this package.
#
# `pca` answers in the kind it was given, and that is exactly right for both kinds of stack:
#   a GMTimage stack (Byte/UInt16 satellite bands) -> a UInt8 image cube, each component already
#     stretched to 0..255 ON ITS OWN (pca.jl does that per k). Three of those bands ARE the
#     false-colour composite, with no scaling of ours anywhere.
#   a GMTgrid stack (Int16 and wider — data)       -> a grid cube, components keeping their values,
#     so a single one shows with its own colour bar and hover readout.
# THE FILL PIXELS TAKE NO PART IN IT. A satellite scene is a rotated swath inside a north-up
# rectangle, so a third of the file is fill that GDAL declares (this Landsat VRT: 18.1 M of 59.6 M
# pixels, NoDataValue=0). Fed to a PCA as if they were radiance they move the mean, spike the
# covariance at the origin — explained variance came out 96.5/3.2/0.3 with them in and 93.5/6.0/0.4
# with them out, i.e. DIFFERENT COMPONENTS — and they swallow the display range (PC1 spanned
# -65909..12568 while the real data's 1st percentile was -11929, so the scene was squeezed into the
# top quarter of the scale: the washed-out composite).
#
# `pca` cannot be told to skip them, but it does not have to be: `pca(X::AbstractArray; npc)` is the
# documented matrix method, so the valid pixels go in as a plain n-by-p matrix and the scores come
# back to be put in their places. Public API only — no `princomp!`, no `GI2vectors`.
function _bands_pca(scene::Ptr{Cvoid}, q::Int = 3)
	info = _BANDS_INFO[scene]
	cube = GMT.gdaltranslate(_emp_vsi(info.path))      # the whole stack, one read, in its own type
	nd   = _bands_nodata(info.path)
	X    = _bands_cube_matrix(cube)                    # nothing to mask -> plain `pca(cube)` below
	(X === nothing || isnan(nd)) && (P0 = GMT.pca(cube; npc = info.n); return (P0, String.(P0.names)))
	keep = _bands_valid_rows(X, Float32(nd), _bands_nan_possible(cube))
	nk = count(keep)
	(nk == length(keep) || nk <= size(X, 2)) &&
		(P0 = GMT.pca(cube; npc = info.n); return (P0, String.(P0.names)))
	score, _, _, explained, = GMT.pca(X[keep, :]; npc = info.n)     # the documented matrix method
	Z = Matrix{Float32}(undef, size(X, 1), size(score, 2))
	_bands_scatter!(Z, score, keep)
	P = GMT.mat2grid(reshape(Z, size(cube, 1), size(cube, 2), size(Z, 2)), cube)
	P.names = [string("PC ", k, ", explained variance ", round(explained[k], digits = 1))
	           for k in 1:size(Z, 2)]
	return (P, copy(P.names))
end

# The scene's declared no-data, through `_emp_att` — THE gdalinfo parse in this package.
function _bands_nodata(path::String)::Float64
	att = _emp_att(path)
	return att.has_nodata ? att.nodata : NaN
end

# The cube as an n-by-p Float32 matrix (one row a pixel, one column a band). BAND-PLANAR only: that
# is a reshape of the buffer as it lies. A pixel-interleaved image would need de-interleaving, which
# is GMT's own job inside `pca` — `nothing` says "hand the cube to `pca` whole".
function _bands_cube_matrix(cube)::Union{Matrix{Float32},Nothing}
	buf = cube isa GMTimage ? cube.image : cube.z
	(cube isa GMTimage && length(cube.layout) > 2 && cube.layout[3] == 'P') && return nothing
	ndims(buf) == 3 || return nothing
	return Float32.(reshape(buf, size(buf, 1) * size(buf, 2), size(buf, 3)))
end

# Can a NaN even occur in this cube? An IMAGE cannot hold one (integer pixels), and a GRID says so
# itself — GMT sets `hasnans` to 1 when there are none and 2 when there are (0 = not yet known). The
# question is asked ONCE, here, instead of once per element: on a 4-band Landsat scene that is 240 M
# `isnan` tests that already knew their answer.
_bands_nan_possible(cube) = (cube isa GMTgrid) && cube.hasnans != 1

# A pixel counts only when EVERY band has a value there: a component combines all of them, so one
# missing band makes the whole pixel meaningless.
#
# The masking runs in one of two kernels, picked before the loop starts, so nothing is branched on
# inside it — and `keep` is a Vector{Bool}, not a BitVector, so the test is an AND into a byte the
# compiler can vectorize rather than a read-modify-write of a bit.
function _bands_valid_rows(X::Matrix{Float32}, nodata::Float32, nan_possible::Bool)
	keep = fill(true, size(X, 1))
	nan_possible ? _bands_mask_nodata_nan!(keep, X, nodata) : _bands_mask_nodata!(keep, X, nodata)
	return keep
end

# The usual case: a declared no-data value and no NaNs to worry about.
function _bands_mask_nodata!(keep::Vector{Bool}, X::Matrix{Float32}, nodata::Float32)
	@inbounds for j in axes(X, 2)
		@simd for i in axes(X, 1)
			keep[i] &= (X[i, j] != nodata)
		end
	end
	return nothing
end

# ... and the case where the grid admits it may hold NaNs.
function _bands_mask_nodata_nan!(keep::Vector{Bool}, X::Matrix{Float32}, nodata::Float32)
	@inbounds for j in axes(X, 2)
		@simd for i in axes(X, 1)
			v = X[i, j]
			keep[i] &= (v != nodata) & (v == v)      # v == v is false only for NaN
		end
	end
	return nothing
end

# Scores back into their pixels; the fill keeps no score at all (NaN -> the Preferences NaN colour on
# a component, and this file's own fill value in a composite).
function _bands_scatter!(Z::Matrix{Float32}, score::AbstractMatrix, keep::Vector{Bool})
	@inbounds for k in 1:size(Z, 2)
		j = 0
		for i in 1:size(Z, 1)
			Z[i, k] = keep[i] ? Float32(score[(j += 1), k]) : NaN32
		end
	end
	return nothing
end

# ONE component. `slicecube` is GMT's own layer extractor and answers in the cube's kind — an image
# band for an image cube, a grid for a grid cube.
_bands_pca_layer(P, k::Int) = GMT.slicecube(P, clamp(k, 1, size(P, 3)))

# Three components as ONE false-colour picture — the classic PCA composite, and the reason `pca` is
# in this dialog at all. Any three components, any channel order.
#
# From an IMAGE cube the components are already 0..255 each, so the composite is just those three
# bands side by side. From a GRID cube each component is scaled by `mat2img`, ONE AT A TIME: handing
# `mat2img` the 3-layer cube instead would scale all three by the CUBE's range, and a PCA's
# components are unequal by construction (PC1 96.5 %, PC2 3.2 %, PC3 0.3 % on this stack), so PC2 and
# PC3 would come out as flat grey — the dull composite.
function _bands_pca_rgb(P, r::Int, g::Int, b::Int)
	n = size(P, 3)
	sel = [clamp(r, 1, n), clamp(g, 1, n), clamp(b, 1, n)]
	(P isa GMTimage) && return GMT.mat2img(P.image[:, :, sel], P)
	# A GRID cube: scale each component here. NOT `mat2img` per component — on a scene with fill it
	# paints every NaN WHITE (pca.jl / _mat2img_float set NaN to 255) and scales the rest by min/max,
	# which put the real data at p50 = 36/198/29 of 255 and took 32 s. Percentile limits from a
	# subsample + one SIMD pass: the picture fills the range and the fill stays black.
	npx = size(P, 1) * size(P, 2)
	out = Array{UInt8}(undef, size(P, 1), size(P, 2), 3)
	for (ch, k) in enumerate(sel)
		lo, hi = _pca_pct_limits(P.z, k, npx)
		_pca_to_u8!(out, P.z, ch, k, npx, lo, hi)
	end
	return GMT.mat2img(out, P)
end

# 2nd/98th percentile of one component, from a 1-in-31 sample of its finite values — the limits a
# histogram stretch uses, at a hundredth of the cost of scanning 60 M pixels.
function _pca_pct_limits(z::Array{Float32,3}, k::Int, npx::Int)
	off = (k - 1) * npx
	s = Float32[]
	sizehint!(s, npx ÷ 31 + 1)
	@inbounds for i in 1:31:npx
		v = z[off + i]
		isnan(v) || push!(s, v)
	end
	length(s) < 16 && return (0.0f0, 1.0f0)
	sort!(s)
	lo = s[max(1, round(Int, 0.02 * length(s)))]
	hi = s[min(length(s), max(1, round(Int, 0.98 * length(s))))]
	return hi > lo ? (lo, hi) : (s[1], s[end] > s[1] ? s[end] : s[1] + 1.0f0)
end

# One component -> one 0..255 channel, clipped at the limits. NaN (the scene's fill) stays 0 = black.
function _pca_to_u8!(out::Array{UInt8,3}, z::Array{Float32,3}, ch::Int, k::Int,
                     npx::Int, lo::Float32, hi::Float32)
	sc  = 255.0f0 / (hi - lo)
	src = (k - 1) * npx
	dst = (ch - 1) * npx
	@inbounds @simd for i in 1:npx
		v = z[src + i]
		out[dst + i] = isnan(v) ? UInt8(0) : round(UInt8, clamp((v - lo) * sc, 0.0f0, 255.0f0))
	end
	return nothing
end

# ---------------------------------------------------------------------------------------------
# THE C CALLBACK. Requests:
#   probe            -> txt = "<n>\n<name1>\n<name2>…" (ret n; 0 = this window has no bands)
#   gray;<k>         -> load band k into the window (picture or grid, per its type)
#   rgb;<r>;<g>;<b>  -> load the three bands as one RGB picture
#   pca              -> compute the components, register THEM as the band source, return their names
# A negative return is an error whose |ret| bytes are in txt.
function _on_bands(scene::Ptr{Cvoid}, creq::Cstring, txt::Ptr{UInt8}, txtcap::Cint)::Cint
	try
		req = split(unsafe_string(creq), ';')
		op  = String(req[1])
		info = get(_BANDS_INFO, scene, nothing)
		if info === nothing
			op == "probe" && (_pal_puttxt(txt, txtcap, "0"); return Cint(0))
			return _bands_err(txt, txtcap, "this window has no multiband file")
		end
		# The warm-up may still be compiling (the user was fast); wait for it rather than run GMT twice
		# at once — worst case is exactly the old behaviour, never a clash (warmup.jl).
		op == "probe" || warm_wait("bandslist")
		if op == "probe"
			# Line 1 is "<nbands> <rgb_ok>". rgb_ok is 0 when the bands are float — three different
			# physical quantities share no colour scale, so Mirone forces Gray Scale and disables RGB
			# for them (bands_list.m:72-76); the dialog greys the radio out on that answer.
			_pal_puttxt(txt, txtcap, string(info.n, ' ', _bands_rgb_ok(scene) ? 1 : 0,
			                                '\n', join(info.names, '\n')))
			return Cint(info.n)
		elseif op == "gray"
			k = clamp(parse(Int, req[2]), 1, info.n)
			_bands_show!(scene, Base.invokelatest(_bands_get, scene, k), info.names[k])
			return Cint(1)
		elseif op == "rgb"
			r = clamp(parse(Int, req[2]), 1, info.n)
			g = clamp(parse(Int, req[3]), 1, info.n)
			b = clamp(parse(Int, req[4]), 1, info.n)
			img = Base.invokelatest(_bands_get_rgb, scene, r, g, b)
			_bands_show!(scene, img, string(info.names[r], " | ", info.names[g], " | ", info.names[b]))
			return Cint(1)
		elseif op == "pca"
			delete!(_BANDS_PCA, scene)          # let the PREVIOUS run's cube go before allocating a new one
			P, names = Base.invokelatest(_bands_pca, scene)
			npc = size(P, 3)
			npc < 1 && return _bands_err(txt, txtcap, "PCA produced nothing")
			_BANDS_PCA[scene] = P
			_BANDS_INFO[scene] = (path = info.path, n = npc, names = names,
			                      bytes = false, isdata = false)
			# Same first line as "probe", and the rgb_ok flag comes from the SAME `_bands_rgb_ok` —
			# never a second rule written out here (it answers false for components, so the dialog
			# greys RGB Color out the moment a PCA lands).
			_pal_puttxt(txt, txtcap, string(npc, ' ', _bands_rgb_ok(scene) ? 1 : 0, '\n', join(names, '\n')))
			return Cint(npc)
		end
		return _bands_err(txt, txtcap, "unknown request '$op'")
	catch e
		return _bands_err(txt, txtcap, "Load Bands: " * sprint(showerror, e))
	end
end

_bands_err(txt::Ptr{UInt8}, cap::Cint, msg::String)::Cint = Cint(-_pal_puttxt(txt, cap, msg))

# The band this window is currently showing (so the next Load can take its place).
const _BANDS_SHOWN = Dict{Ptr{Cvoid},Tuple{Symbol,String}}()

# Put the loaded band/composite on screen, REPLACING the previously loaded one — Load swaps what the
# window shows (Mirone's push_Load writes over the displayed CData), it does not pile bands up. The
# previous band is removed through the SAME primitive a Scene Objects "Remove" uses, and the new one
# enters by `_drop_into`, the door every opened raster comes through, so it gets its handle, its
# axes and its colour bar built the one way. The raster the window was OPENED from is never touched:
# only bands this dialog loaded are replaced.
function _bands_show!(scene::Ptr{Cvoid}, data, name::AbstractString)
	prev = get(_BANDS_SHOWN, scene, nothing)
	if prev !== nothing && prev[2] != String(name)
		kind, pname = prev
		try
			ccall(_fn(kind === :grid ? :gmtvtk_remove_grid_h : :gmtvtk_remove_image_h),
			      Cint, (Ptr{Cvoid}, Cstring), scene, pname)
			_forget_object!(scene, kind, pname)
		catch e
			@debug "Load Bands: could not remove the previous band" exception = (e,)
		end
	end
	_drop_into(scene, data, String(name))
	_adopt_new_element(scene, String(name), data)      # this is what the window shows now
	_BANDS_SHOWN[scene] = (data isa GMTgrid ? :grid : :image, String(name))
	return
end

# THE OPEN DOOR for a multiband raster. GMT's own reader cannot even TYPE a .vrt ("Must select the
# input data type"), so such a file used to fail to open at all — and, failing, never registered, so
# Image > Load Bands stayed greyed out. Mirone's multiband open shows the FIRST band and lists the
# rest for bands_list; this does the same. Returns false when `spec` is not a band stack, and the
# caller then opens it the ordinary way.
#
# A 3/4-band BYTE file is deliberately NOT a band stack: it is an RGB(A) photograph and must open as
# the picture it is (Mirone shows those as colour too). It is still registered, so Load Bands can
# pull its individual channels afterwards.
function _bands_open_first!(scene::Ptr{Cvoid}, spec::String, name::String, promote::Bool, recent::String)::Bool
	base = first(split(spec, '?'))
	ext  = lowercase(splitext(base)[2])
	# A subdataset spec, or a file GMT reads natively as a grid/cube, is none of this tool's business.
	(occursin('?', spec) || ext in (".nc", ".grd", ".cpt")) && return false
	n = _bands_register!(scene, spec)
	n > 1 || return false
	info = _BANDS_INFO[scene]
	(info.bytes && n <= 4) && return false            # an RGB(A) photo: let the normal image path show it
	B = _bands_get(scene, 1)                          # "display the first raster in the vrt"
	isempty(recent) || _record_recent(recent, B)
	bname = string(name, " — ", info.names[1])
	_drop_into(scene, B, bname; promote = promote, source = spec)
	_adopt_new_element(scene, bname, B)
	_BANDS_SHOWN[scene] = (B isa GMTgrid ? :grid : :image, bname)
	return true
end

# JIT warm-up (warmup.jl). Compute PCA and Load reach `GMT.pca`, `mat2img`/`slicecube` and the
# grid-add path for the FIRST time when the button is pressed, so that one press used to pay ~3.4 s of
# compiler time on top of the work. The dialog is open long before then — spend that time compiling on
# a 4-band toy cube. Pure computation, no scene contact, exactly as the other tools' bodies do.
function _bands_warm()
	z = Float32[i + 2j + 3k + 0.1f0 * i * k for i in 1:16, j in 1:16, k in 1:4]
	C = GMT.mat2grid(z; x = collect(0.0:1.0:15.0), y = collect(0.0:1.0:15.0))
	P = GMT.pca(C; npc = 3);                            yield()
	L = _bands_pca_layer(P, 1);                         yield()
	_cpt_nodes(L, _default_cmap(L));                    yield()
	_grid_zbuf(L)
	precompile(_on_bands, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	precompile(_bands_show!, (Ptr{Cvoid}, GMTgrid, String))
	precompile(_bands_get, (Ptr{Cvoid}, Int))
	precompile(_add_grid_to_scene, (Ptr{Cvoid}, GMTgrid, String))
	return nothing
end

function _register_bands()
	fptr = @cfunction((s, r, t, c) -> Base.invokelatest(_on_bands, s, r, t, c),
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_bands_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("bandslist", _bands_warm)     # C++ fires this when the dialog opens (70_window.cpp)
	return
end
