# Export GMT.jl script — implementation plan

Reproduce what an iGMT window displays as GMT.jl calls: as a runnable command sequence in the live
session, and as a standalone `.jl` script. Independent of bug-fix / usability work — it adds one new
module plus a small number of getters, and changes no display path.

---

## 0. Relationship to Save Session (the main lever)

`_SESSION_LOG` (session.jl) already IS the inventory this needs: a per-window ordered list of
`ElementRecipe{kind, origin, source, name, params}` — what each layer is, where it came from, with
which parameters. Save Session and script export are **two backends on one inventory**. A second
scene walker for script export would violate SACRED_LAW's opening rule and would drift exactly as
the twelve derived-variable sites did.

| | Save Session | Script export |
|---|---|---|
| element inventory | `_SESSION_LOG` | same |
| C++-drawn snapshot | `gmtvtk_serialize_polys` / `_faults` / `_texts` | same |
| display state | `gmtvtk_scene_state_full` | same |
| per-element style | `gmtvtk_overlay_style_h` (+ the getters still open) | same |
| generated raster data | `_session_pack_generated` | same |
| output | `.igmtz` zip, replayed into iGMT | GMT.jl calls |

Consequence: every Save Session coverage gap is a script-export gap. The session plan's OPEN list
(symbol-layer style not captured, polygon fill round-trip unverified, standalone `add_symbols!`
layers unrecorded, vertex labels unrecorded) is this feature's blocker list too. Fixing them once
serves both, which raises their priority rather than adding new work.

## 1. Principle — it is always GMT

GMT.jl emits 2-D PostScript. Always. There is no "2-D backend" and no "3-D backend": whether the
iGMT window is top-down or tilted, the export is GMT calls. The camera contributes **kwargs**, not a
code path:

- `view=(az, el)` (`-p`) — derived from camera position − focal point. Omitted for a top-down window.
- `zsize=` (`-JZ`) — from `s->ve` and the ACTIVE layer's z-range, which must come from
  `activeGridZRange()` (50_scene.cpp), never `s->zmin`/`s->zmax` — see SACRED_LAW's derived-variable
  axes law, third layer.
- `region` gains its z pair when any layer draws in 3-D.

All layers in one figure share that single `view`/`zsize`. `flat2d` decides only whether the two
kwargs appear.

A 3-D scene is therefore normally a **mix of modules** — `grdview` for the surface, `grdimage` for a
flat backdrop, `plot3` for anything carrying depth, `plot` for base-plane vectors — not `grdview`
alone.

## 2. Two sinks, one emitter

The emitter produces an ordered list of calls (module + args + kwargs + a data binding per layer).
Two renderings of that same list:

1. **Live sink** — evaluate in the current session with the live `GMTgrid`/`GMTimage` objects bound.
   No files at all: `_SCENE_OBJS` (savefile.jl) already holds the live objects per window, and a
   captured layer is a GMTimage in memory (§3, T3). This is the "a command, or a set of them, that
   reproduces the display" half.
2. **File sink** — serialize to a `.jl` text file. Data bindings must resolve to something the script
   can reach at run time, so in-memory objects are materialized once into `script_data/` beside the
   script.

One emitter, two sinks. Never two emitters.

## 3. The data-source ladder (unconditional, per layer, in order)

| tier | mechanism | emitted line | applies when |
|---|---|---|---|
| **T1 recompute** | re-emit the GMT call recorded in `G.command` (already stamped at every grid-generating site) | `G = grdredpol("mag.grd", …)` | the layer was produced by one GMT call |
| **T2 reuse data** | the live object (live sink) or a sidecar written by `_session_pack_generated` (file sink) | `G = gmtread("script_data/rtp.nc")` | the layer has a data representation but no expressible command — host-composited RGBA blends land here, they are GMTimages |
| **T3 rasterize** | `_capture_rect_image(scene, w,e,s,n; coords=true)` → georeferenced GMTimage | `grdimage!(I, …)` / `grdview(G, drape=I)` | the layer has no data representation at all, **OR its displayed LOOK is not expressible in GMT** |

**T3 is not only for "no data".** The governing rule, stated at the outset and non-negotiable: *if the
displayed image cannot be reproduced by GMT, the in-memory image on screen becomes a GMTimage and is
passed to GMT.* So a layer whose colours are the viewer's rather than GMT's — a baked hillshade, cast
shadows, an external illumination grid, a PBR-baked flat image, an Aquamoto composite, a draped texture
— is rasterized, ALWAYS, not emitted as a plain `grdimage` with a "shading not exported" apology in the
header. `gmtvtk_layer_display` (90_c_api.cpp) answers `repro=0|1` + `why` + the layer's own footprint;
`_script_rasterize!` (gmtscript.jl) does the capture. Two shapes:
- **3-D grid** → `grdview(G, drape=I)`: geometry from the grid, colours from the screen. This needs the
  DATA-SPACE bake (`_capture_rect_image(…; baked_only=true)`) — it is georeferenced and camera-
  independent, so it can be a texture; a perspective screen grab cannot and is accepted only for a
  top-down figure.
- **anything else** → `grdimage(I)`, the capture *is* the layer, and no grid data is bound at all.

The layer's CPT is still built and bound either way, because the window's colour bar is that CPT —
only the `cmap=` kwarg is dropped (the image already carries the colours).

T1 is what makes the script genuinely *reusable* — the user edits a parameter and re-runs — so it is
tried first, always.

### T3 details

- **The primitive already exists.** `_capture_rect_image` (grid.jl:287) calls
  `gmtvtk_capture_rect_databaked`, falls back to `gmtvtk_capture_rect_rgb`, wraps the borrowed
  `(band, col, row)` buffer, and returns `GMT.mat2img(mat; x=[w,e], y=[s,n], proj4=…, wkt=…)` —
  in-memory, georeferenced, window CRS attached, buffer freed via `gmtvtk_free_rgb`. Script export
  calls it as-is; it does not get a parallel capture path.
- **`_databaked` is preferred and is not a screenshot**: it bakes in DATA space at native grid
  resolution, independent of window size and zoom. `_capture_rect_rgb` (screen-space NDC grab at the
  current camera) is the fallback for when there is no baked grid to read.
- **Capture the LAYER, not the window.** Hide every other element, capture, restore — otherwise the
  image bakes its neighbours in and they get drawn a second time by their own emitted call.
- **Top-down:** the captured image is georeferenced 1:1 onto the layer's own region, so later layers
  still overlay correctly and no `-p` is involved.
- **Perspective:** a grab of a tilted camera cannot be honestly georeferenced. Either place it at
  figure coordinates as a backdrop (`image!` / `psimage`), or fall through to the degenerate mode.
- **Alpha:** compositing a T3 image OVER existing layers needs a transparent background from the
  capture. To verify. If unavailable, T3 is backdrop-only — acceptable, because the layers that reach
  T3 (curtain textures, VTK-only appearance) sit at the bottom in practice.

### Degenerate mode — the guaranteed floor

An explicit **"export as backdrop"**: one whole-window capture placed under a real `basemap` frame,
with a loud header comment stating it is a picture and not a reproduction. Without it, a
curtain-heavy 3-D window exports to nothing.

## 4. Element → module map

Module choice follows what the element IS and whether it carries Z — never a global mode flag.

| element | module | Z |
|---|---|---|
| grid / relief surface | `grdview` (`-Qi` draped image; `-Qs`/`-Qm` for a mesh look) | yes |
| image or grid lying flat (basemap tile, drape backdrop, dropped image) | `grdimage` — projected under the shared `-p` | no |
| grid drawn flat inside a 3-D scene | `grdimage!` at its own level (`plane=` / `-N`) | level only |
| table / line on the base plane | `plot!` | no |
| line / points with real Z (LiDAR, 3-D picks, cube slices) | `plot3!` | yes |
| curtain track | `plot3!` polygon strips — geometry only, texture is T3 | yes |
| fault trace | `plot!`; its dipping plane → `plot3!` quad | mixed |
| slip patches | `plot3!` per-patch polygons, `fill=` from the slip CPT | yes |
| symbol layers (volcanoes / seismicity / cities) | `plot!` or `plot3!` (explicit z when the layer sits at depth) | per layer |
| text labels | `text!` (with `-Jz` when anchored in depth) | per label |
| focal beachballs | `meca!` | depth column |
| coastlines / borders / rivers | `coast!` + the captured pen | no |
| colorbar | `colorbar!` with explicit `pos=` (auto-placement is unreliable under `-p`) | — |

## 5. Painter order is load-bearing

GMT has no depth buffer: under `-p`, call order IS the occlusion. VTK z-sorts; GMT will not. So the
export must emit **back-to-front**, and the unified draw-order pile is exactly that ordering. This
makes the per-element order/visibility getter (§6) a correctness requirement for any 3-D export, not
a refinement — honour the pile or a 3-D script comes out visibly wrong even when every layer is
individually right.

## 6. What exists, what is missing

Exists and is reused unchanged: `_SESSION_LOG` + recipes; the three `gmtvtk_serialize_*` snapshots;
`gmtvtk_scene_state_full` (ve, flat2d, barX0/barY0, full camera); `gmtvtk_overlay_style_h`;
`_session_pack_generated`; `_capture_rect_image`; `G.command` stamping; `_SCENE_OBJS`.

Missing, and needed:

1. **CPT identity.** There is no `cptName` anywhere in `deps/src` — the C side holds only LUT nodes
   (`makeGridCTF`). The script needs `cmap=`. Record the `cmap` argument in the recipe params at add
   time; for an interactively edited LUT, dump its nodes to a CPT (sidecar for the file sink, a live
   `GMTcpt` for the live sink).
2. **Per-element draw order + visibility.** `gmtvtk_scene_state_full` carries neither. One new export
   `gmtvtk_serialize_display` — per element: tag, pile rank, visible flag, style — plus its
   `_LIB_SYMBOLS` entry (a new export is invisible to Julia without it). Serves §5 and skips
   unchecked layers.
3. **Light parameters.** The shading dock's relief look / azimuth / elevation are not serialized;
   needed to emit a matching `shade=`.
4. **The session OPEN items** listed in §0, for vector styles.

Note: emitting `grdgradient` / `shade=` in a script is NOT a fork of `applyReliefShade` — it is a
different renderer's input, not a second copy of the display maths. No display path is touched.

## 7. Fidelity ledger (stated in the generated script's header)

- **Exact:** region, projection / CRS, layer set and order, vector geometry and pens, coastlines, CPT,
  text content, beachballs.
- **Approximate:** relief shading (VTK PBR / relief looks are not GMT's `-I` intensity model — emit
  the dock's light parameters and say so in a comment); colorbar placement; camera → `-p az/el`; font
  metrics.
- **Rasterized (T3):** looks right, not editable, not re-projectable.
- **Not representable:** curtain textures, gizmo, VTK cube-axes billboards, interactive picks. Emitted
  as `# NOT EXPORTED: …` comments so nothing disappears silently.

## 8. Script shape

Flat linear script — no functions, no config parsing. One section per Scene Objects element, in pile
order, knobs hoisted to `const` at the top so one edit changes the whole figure.

```julia
# generated by InteractiveGMT <ver>  <timestamp>  window "seamount.nc"
# NOTE: relief shading is GMT's -I model, not the viewer's PBR relief look — see plan §7
using GMT
const REG = (-12.5, -6.0, 35.0, 40.0)
const CPT = "geo"

# ── layer 1  seamount.nc  (grid, file)
G = gmtread("D:/data/seamount.nc")
grdimage(G, region=REG, proj=:merc, cmap=CPT, shade=(azim=315, norm="e0.8"),
         frame=(axes=:WSen, annot=:auto), figsize=15)
# ── layer 2  coastlines (i)
coast!(res=:i, shore=(0.5, :black))
# ── layer 3  isochrons.dat
plot!("data/isochrons.dat", lw=0.75, lc="255/0/0")
colorbar!(pos=(anchor=:TR, length=(8, 0.35)), cmap=CPT, frame=(annot=500,))
showfig(savefig="figure.png")
```

## 9. Module layout

`src/gmtscript.jl` (new, included after `session.jl`):

- `_script_emit(scene) :: Vector{ScriptCall}` — the orchestrator: recipes + display state + C-drawn
  snapshots → an ordered call list. The one place that walks anything.
- `_script_render_live(calls)` / `_script_render_text(calls, dir) :: String` — the two sinks (§2).
- one `_emit_<kind>(ctx, r)` per recipe kind, dispatched from a table that mirrors
  `_session_replay!`'s switch — same kinds, same params, so a new element type is added to both at
  once.
- `ScriptCtx`: region, projection, `view`/`zsize`, cmap registry, sidecar dir, first-layer-vs-`!`
  bookkeeping, pile order, unsupported-layer log.
- `_script_data_binding!(ctx, r)` — the T1→T2→T3 ladder (§3).
- `_on_export_script(scene, path)` + lazy `@cfunction` registration in eventloop's
  `_ensure_callbacks` (tag `"script"`).

C++: the one new export from §6.2, plus the light parameters folded into `gmtvtk_scene_state_full`.
No menu restructuring — a single `addAction` in the File menu beside Save Session.

## 10. Phasing

- **P1 — spine + rasters.** `gmtscript.jl`, call list, both sinks, `ScriptCtx`, CPT capture, the T1–T3
  ladder, `region`/`proj`/`frame`/`colorbar`, `view`/`zsize` kwargs (they are two kwargs — no reason
  to defer), base grid/image + dropped rasters, File menu action. Deliverable: a single-grid window
  exports to a script whose figure matches.
- **P2 — vectors.** Geography layers, tables/overlays, symbol layers, polygons, text. Requires the
  session style getters (§0) — shared work.
- **P3 — depth.** `plot3` family, faults + slip models, curtain geometry, focal, basemap tiles.
- **P4 — polish.** T1 coverage widened as more tools stamp `G.command`, knob hoisting, the
  unsupported-layer report, degenerate backdrop mode.

## 10b. STATUS (2026-08-09) — P1–P4 implemented

Nothing committed. `deps/build/gmtvtk.dll` is rebuilt and current.

**Entry points.** `gmtscript(fig; path="", figsize=15, recompute=false, backdrop=false)` returns the
script text (writes it, plus `script_data/`, when `path` is given); `gmtreplay(fig; …)` runs the same
call list live against the in-memory objects.

**File → Export GMT.jl script…** opens an EDITABLE script box with **Save…** and **Run**
(`showScriptEditor`, 70_window.cpp), not a file dialog. It is backed by a real working file because
that is the only way both buttons can work: a script whose layers came out of the window reads them
through `joinpath(@__DIR__, "script_data", …)`, and `@__DIR__` only means anything for a file
`include` is reading. So `_script_prepare_for_editor` (gmtscript.jl) generates the script into a
per-window temp directory, the box shows that file's text, **Run** writes the box back over it and
`include`s it — which is what makes Run run what was EDITED — and **Save…** copies the same edited
file plus its `script_data/` to the chosen destination (`_script_save_bundle`), so what is kept
actually runs.

All three steps go over the console eval bridge (`g_juliaEval`), the one the ruler and the focal
dialog already use — there is no dedicated C callback for the export any more (the earlier
`gmtvtk_set_export_script_callback` / `JuliaExportScriptFn` / `_on_export_script` are deleted; a
second eval path would have been the fork this document keeps warning about).

**Done, verified by running the emitted script standalone and by `gmtreplay`:**
- rasters — base/dropped grids and images, basemap tiles; `grdimage` / `grdview -Qi` per element.
- `view=(az,el)` from the camera (round-trips exactly), `zsize` from the viewer's own `zfac`/`xfac`
  (added to `gmtvtk_scene_state_full`), region/projection/frame/colour bar, knobs hoisted to
  `const REG` / `PROJ` / `FIGSIZE`.
- CPTs — `_cmap_tag` (cpt.jl) records the colormap name at both grid doors; the script rebuilds the
  palette with the same `makecpt` call the viewer's LUT comes from, and the colour bar reuses it.
- vectors — overlays, symbol layers (fill AND edge), drawn polygons with fill+transparency, text
  grouped by (size, colour), coast/borders/rivers as a real `coast!` from the `:geography` recipe with
  the live overlay skipped by name.
- 3-D — `plot3!` chosen per element by "has z AND the figure is tilted".
- faults + slip models + curtains + focal — fault trace, the dipping plane READ OFF its actor (new `P`
  tag in `gmtvtk_serialize_faults`; the geodesic down-dip walk is never re-implemented), slip patches
  as one multisegment call with per-segment `-G`, curtain outline (texture noted as not exported),
  and beachballs as `meca!(aki=true)` from `_MECA_TABLE` (session.jl), which `_focal_plot` fills.
- painter order — overlays and symbol layers sorted by the viewer's own `stack` rank (§5).
- T1 guard — a recorded command is emitted only if every path in it still exists.
- backdrop mode — `backdrop=true`, one `_capture_rect_image` of the window under a real frame, with
  the header saying plainly that it is a picture.
- tests — `test/test-gmtscript-unit.jl`, 58 assertions on the pure layer (literals, blob parsers,
  camera→`-p`, `-JZ`, colours, T1 guard, sidecar ids, cmap capture). Full unit suite 868/868.

**New C exports** (both in `_LIB_SYMBOLS`): `gmtvtk_serialize_overlays`, `gmtvtk_serialize_symbols`;
plus `zfac`/`xfac` in `gmtvtk_scene_state_full` and the `P` (fault-plane) tag in
`gmtvtk_serialize_faults`. The export itself needs no C export of its own — it rides the eval bridge.

**Known gaps, deliberately left:**
1. **Relief shading still not exported** — needs the Shading dock's light parameters in the scene
   state (§6.3). Every script says so in its header.
2. Drawn polygons and text carry no `stack` in their session-shared blobs (format frozen for Save
   Session), so they follow in scene order rather than pile order.
3. `borders`/`rivers` emit `type=1`; the `:geography` request string does not record the level.
4. Symbol sizes are carried px→p 1:1; colour-bar placement is a fixed `anchor=:MR`.
5. Save Session does not yet consume the new overlay/symbol serializers — its own open items
   (symbol-layer style, standalone `add_symbols!` layers, vertex labels) are now easy from them, but
   no session behaviour was changed here.
6. The cube-layer grid door (`_show_cube_layer_image!`) takes resolved CPT nodes and has no colormap
   name in scope, so a cube layer exports with the default ramp name.

## 11. Verification

Cheap by default, no screenshots unless asked. Per element kind, a headless test asserting the
emitted text **parses** (`Meta.parse`), and in a `gmt`-enabled tier that it runs to a non-empty PNG.
The live sink is testable without any file at all — build the call list, evaluate it, assert the
returned figure is non-empty. Visual parity (window capture vs script figure, side by side) only on
request.
