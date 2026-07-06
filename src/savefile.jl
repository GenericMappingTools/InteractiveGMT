# savefile.jl — File > Save Grid / Save Image  +  per-object "Save…" in the Scene Objects panel.
# The C++ side opens the format-picker dialog (SaveFormatDialog, 30_app.cpp) and hands
# "<kind>;<fmt>;<path>;<name>" to `_on_save`. We write a scene object — converting to the on-disk
# format the user picked:
#   grids  : netCDF (.nc/.grd) + Surfer 6 (.grd) via GMT.gmtwrite;
#            GeoTIFF / JPEG2000 / Erdas Imagine / ENVI via GMT.gdalwrite (driver from extension)
#   images : ALL formats via GMT.gdalwrite (GeoTIFF/JP2/Erdas/ENVI + generic jpg/png/tif/bmp)
#
# WHICH object is saved: every grid/image added to a window is remembered here (`_remember_object!`)
# keyed by the window's Scene* — primary surfaces (view_grid/view_image) AND extras (drops, basemap,
# tiles, iview_image_obj). `name` (the Scene Objects label) selects the exact object; an empty name
# (the File menu "save the window's grid/image") picks the first object of that kind = the primary.
# Falls back to the `_FIGREG` primary figure if the store has no match.
#
# As with the other menu callbacks, the @cfunction + its registration are RUNTIME values (created
# lazily on first window open via eventloop.jl `_ensure_callbacks`), never at module top level.

# Per-window store of saveable objects: scene (Scene*) -> [(kind, name, data), …] in add order
# (primary first). `data` is the live GMTgrid / GMTimage. Keyed by the opaque handle, so it leaks a
# tiny entry when a window closes (same minor pattern as basemap's _BASEMAP_LOADED) — pruning a
# closed Scene* is a follow-up.
const _SCENE_OBJS = Dict{Ptr{Cvoid}, Vector{Tuple{Symbol,String,Any}}}()

# Remember a grid/image just added to `scene` so File>Save / the Scene Objects "Save…" can write it.
# Returns `data` so it can be used inline. name="" is fine for an unnamed primary (lookup falls back
# to first-of-kind). Idempotent enough: a re-added (kind,name,data) just appends a duplicate.
function _remember_object!(scene::Ptr{Cvoid}, kind::Symbol, name, data)
    scene == C_NULL && return data
    v = get!(() -> Tuple{Symbol,String,Any}[], _SCENE_OBJS, scene)
    push!(v, (kind, name === nothing ? "" : String(name), data))
    return data
end

# Resolve the object to save: exact (kind,name) match first; else the first object of `kind` (the
# primary); else the _FIGREG primary figure. Returns the GMTgrid/GMTimage or nothing.
function _find_object(scene::Ptr{Cvoid}, kind::Symbol, name::AbstractString)
    v = get(_SCENE_OBJS, scene, nothing)
    if v !== nothing
        if !isempty(name)
            for (k, n, d) in v
                (k === kind && n == name) && return d
            end
        end
        for (k, n, d) in v
            k === kind && return d                       # first of kind = the primary
        end
    end
    fig = get(_FIGREG, scene, nothing)                   # fallback: the window's primary figure
    kind === :grid  && fig isa QtFigure && return fig.G
    kind === :image && fig isa QtImage  && return fig.I
    return nothing
end

# Canonical on-disk extension for each format code (kept in sync with the C++ dialog filters).
const _GRID_EXT = Dict("nc"=>".nc", "surfer"=>".grd", "gtiff"=>".tif",
                       "jp2"=>".jp2", "erdas"=>".img", "envi"=>".hdr")
const _IMG_EXT  = Dict("gtiff"=>".tif", "jp2"=>".jp2", "erdas"=>".img", "envi"=>".hdr",
                       "jpg"=>".jpg", "png"=>".png", "tif"=>".tif", "bmp"=>".bmp")

# Append the format's canonical extension if the chosen name carries no recognised one (QFileDialog
# usually appends the filter's default suffix, but the user can type a bare name).
function _ensure_ext(path::AbstractString, want::AbstractString, known)
    lc = lowercase(path)
    any(endswith(lc, e) for e in values(known)) ? String(path) : String(path) * want
end

# Write a grid: netCDF/Surfer via gmtwrite, everything else via GDAL (driver inferred from the ext).
function _save_grid(G, fmt::AbstractString, path::AbstractString)
    if fmt == "nc"
        GMT.gmtwrite(path, G)                 # netCDF (GMT's default grid format)
    elseif fmt == "surfer"
        GMT.gmtwrite(path * "=sf", G)         # Surfer 6 binary grid (GMT grid-format code =sf)
    else
        GMT.gdalwrite(path, G)                # GeoTIFF / JPEG2000 / Erdas(HFA) / ENVI — driver by ext
    end
end

# Images always go through GDAL (GeoTIFF/JP2/Erdas/ENVI + generic jpg/png/tif/bmp; driver by ext).
_save_image(I, ::AbstractString, path::AbstractString) = GMT.gdalwrite(path, I)

# Screenshot -> GeoTIFF: `tmp` is a plain (non-georeferenced) PNG the C++ side already cropped to
# the axes interior (no axis numbers/colorbar baked in) for a top-down, north-up, edge-to-edge view
# of the data bbox (x0,x1,y0,y1) — the only camera state where pixels map affinely to world coords.
# Re-reads it via the universal `gmtread` and re-tags it with the window's own CRS (proj4/wkt,
# already resolved by crs.jl and pushed to the C++ side) before writing the real GeoTIFF via GDAL.
function _save_screenshot_geotiff(tmp::AbstractString, path::AbstractString,
                                   x0::Float64, x1::Float64, y0::Float64, y1::Float64,
                                   proj4::AbstractString, wkt::AbstractString)
    try
        I = GMT.gmtread(tmp)
        Iout = mat2img(I.image; x=[x0, x1], y=[y0, y1], proj4=String(proj4), wkt=String(wkt))
        GMT.gdalwrite(path, Iout)
    finally
        rm(tmp, force=true)
    end
    return path
end

# C callback: req = "<kind>;<fmt>;<path>;<name>" (name optional/empty) for kind grid/image, or
# "geotiff;<tmpPng>;<outPath>;<x0>;<x1>;<y0>;<y1>;<proj4>;<wkt>" for a screenshot GeoTIFF export
# (see [`_save_screenshot_geotiff`](@ref)). Resolve the named scene object (or the primary), write
# it in the chosen format. Errors are reported in the Errors console.
function _on_save(scene::Ptr{Cvoid}, req::Cstring)::Cvoid
    kind = ""
    path = ""
    try
        s = unsafe_string(req)
        kind = String(strip(split(s, ';', limit=2)[1]))
        if kind == "geotiff"
            parts = split(s, ';', limit=9)
            length(parts) >= 9 || error("Save: malformed screenshot GeoTIFF request '$s'")
            tmp  = String(strip(parts[2]))
            path = String(strip(parts[3]))
            x0, x1 = parse(Float64, parts[4]), parse(Float64, parts[5])
            y0, y1 = parse(Float64, parts[6]), parse(Float64, parts[7])
            _save_screenshot_geotiff(tmp, path, x0, x1, y0, y1, strip(parts[8]), strip(parts[9]))
        else
            parts = split(s, ';', limit=4)
            length(parts) >= 3 || error("Save: malformed request '$s'")
            fmt = strip(parts[2]); path = String(strip(parts[3]))
            name = length(parts) >= 4 ? String(strip(parts[4])) : ""
            isempty(path) && return
            if kind == "grid"
                G = _find_object(scene, :grid, name)
                G === nothing && error("No grid to save in this window")
                path = _ensure_ext(path, get(_GRID_EXT, fmt, ".nc"), _GRID_EXT)
                _save_grid(G, fmt, path)
            elseif kind == "image"
                I = _find_object(scene, :image, name)
                I === nothing && error("No image to save in this window")
                path = _ensure_ext(path, get(_IMG_EXT, fmt, ".tif"), _IMG_EXT)
                _save_image(I, fmt, path)
            else
                error("Save: unknown kind '$kind'")
            end
        end
        _viewer_log_error(scene, "Saved $kind -> $path")
    catch e
        _viewer_log_error(scene, "Save $kind FAILED: $(sprint(showerror, e))")
        @warn "save: could not write file" exception=(e,)
    end
    return
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_save()
    fptr = @cfunction((s,c)->Base.invokelatest(_on_save,s,c), Cvoid, (Ptr{Cvoid}, Cstring))
    ccall(_fn(:gmtvtk_set_save_callback), Cvoid, (Ptr{Cvoid},), fptr)
    return
end

# C callback: req = "<kind>;<name>" (kind = "grid"). Resolve the live scene grid (same lookup as Save)
# and re-open it in a NEW iGMT window (view_grid). Return 1 on success so the C++ side then removes it
# from the source window (= a MOVE); 0 on any failure leaves the source untouched. Grids only for now.
function _on_move(scene::Ptr{Cvoid}, req::Cstring)::Cint
    kind = ""
    try
        parts = split(unsafe_string(req), ';', limit=2)
        kind = strip(parts[1])
        name = length(parts) >= 2 ? String(strip(parts[2])) : ""
        if kind == "grid"
            G = _find_object(scene, :grid, name)
            G === nothing && error("No grid to move in this window")
            view_grid(G; title = isempty(name) ? "i'GMT" : name)
        else
            error("Move: unknown kind '$kind'")
        end
        return Cint(1)
    catch e
        _viewer_log_error(scene, "Move to new window FAILED: $(sprint(showerror, e))")
        @warn "move: could not open new window" exception=(e,)
        return Cint(0)
    end
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_move()
    fptr = @cfunction((s,c)->Base.invokelatest(_on_move,s,c), Cint, (Ptr{Cvoid}, Cstring))
    ccall(_fn(:gmtvtk_set_move_callback), Cvoid, (Ptr{Cvoid},), fptr)
    return
end
