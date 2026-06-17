---
name: gmtf3d-vcurtain
description: "vcurtain — vertical seismic \"curtains\" as a view_grid/f3dview option (not a standalone viewer)"
metadata: 
  node_type: memory
  type: project
  originSessionId: a7ebfbed-20ff-4d94-b8e8-1bfcbc6f9cde
---

Fledermaus-style **vertical curtains** (seismic / midwater profile images hung as vertical walls weaving through bathymetry) — shipped as an **option of `view_grid` (hence `f3dview`)**, NOT a standalone viewer. The curtain renders INTO the grid's scene, sharing its coordinate space and vertical scale.

**API:** `view_grid(G; vcurtain=spec)` where `spec` is a NamedTuple (or a vector of them for several curtains):
`(; image, path, zrange, spacing=:distance, cols=nothing, flipv=false)`
- `image`: `GMTimage` (in-memory texture via `img_to_texbuf`) OR a file-path `String` loaded **by F3D itself** (`f3d_image_new_path` → copy buffer; no gmtread).
- `path`: `N×2` matrix OR `GMTdataset` (typed `::AbstractArray` so both share ONE `_curtain_xy` method — `GMTdataset <: AbstractArray`, not `AbstractMatrix`). `N=2` = the simplest two-points straight curtain; more points weave. **ASCII-file path input deliberately NOT supported** (type stability — user).
- `zrange=(zmin,zmax)`: vertical extent (TRUE z units, same as grid data).
- `spacing`: `:distance` (cumulative chord length) | `:simple` (even per point) | `:geomatch` (`cols` = per-point pixel columns).
- `flipv=false`: image first scanline → curtain top (verified correct, in-mem AND disk).

**Implementation** (`src/curtain.jl`): `_curtain_xy(::AbstractArray)`, `_curtain_u`, `_curtain_mesh` (2N shared verts, N-1 quads, horizontal normals), `_curtain_texbuf` (GMTimage→img_to_texbuf | String→f3d_image_new_path), `_add_curtain!` (one spec), `_add_curtains!` (nothing|NamedTuple|vector — 3 dispatch methods, function barrier keeps the path-type instability off the viewer body). Wired in `_view_fv_impl` (fv.jl): `vcurtain=nothing` kwarg, `_add_curtains!(scene, engine, opts, vcurtain, zs)` called right after the grid mesh add, BEFORE camera reset (so curtains are framed). Shares the grid's `zs = fv.zscale` as `transform=(;scale=(1,1,zs))` so it lines up with the relief.

**Why per-mesh emissive texture (gap#1 fold), not global options:** two meshes in one scene. The grid drape uses the GLOBAL `model.*.texture` — a curtain can't also use it (collision). So curtain image rides the per-mesh `baseColorTexture` with `emissive=true` (shows true colour WITHOUT killing the grid's lighting; global `render.light.intensity=0` would flatten the grid). See [[gmtf3d-mesh-view-bindings]] (gap#1).

**Clip-to-surface (optional, 2026-06-10):** spec field `clip=true` (or `:surface`) cuts the curtain's top edge to the grid bathymetry — the wall hugs the relief and the image ABOVE the surface is dropped (sub-seafloor only). Resolved in `view_grid` (which has the grid): `_resolve_vcurtain_clip`/`_clip_one` densify the track (`_densify_polyline`, `clip_n=300` cols) and `GMT.grdtrack` the seafloor z along it, baking `path`(densified)+`topz` into the spec. `_curtain_mesh` gained a `topz` arg: per-column top vertex sits at the seafloor z (clamped to zrange), texture v sampled linearly at that height (`vat(z)`). Default OFF (no `clip` field → spec passes through untouched → flat-top slab). Clip ONLY via view_grid/f3dview (needs the surface); a bare view_fv curtain can't clip. Verified offscreen + interactive. Image vertical registration assumed linear over zrange — set zrange to the section's true top/bottom.

**Status (2026-06-10):** built + verified offscreen — GMTimage path, disk-file path, GMTdataset path, vector-of-curtains, :simple/:distance/:geomatch, all error guards. Combined render (peaks + curtain) confirmed visually: vertical wall cuts through relief, RED-top/BLUE-bottom marker correct. **Gallery:** `grid_vcurtain` example added to `examples/gallery.jl` + `docs/src/gallery.md`; bundled test asset `examples/assets/seismic_E46.jpg` (the WSW–ENE E46/E52 profile the user supplied). NOT committed yet. No f3d core change needed.
