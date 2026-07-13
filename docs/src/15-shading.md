# Shading Dock

The **Shading** dock (right edge, folds to a side strip) tunes how a grid surface is lit and
drawn. It mixes four *unrelated* concerns, so read it by group rather than top-to-bottom.

| Group | Purpose |
|-------|---------|
| [1. Geometry](#1-geometry) | Flat 2-D map vs real 3-D surface |
| [2. Relief look](#2-relief-look-illumination) | How the terrain is illuminated (hillshade / cast shadows) |
| [3. Material & lights](#3-material-lights-pbr) | PBR material + light sliders (only when no hillshade is on) |
| [4. Post-process](#4-post-process-passes) | Screen-space passes over the whole frame |

Only groups 2–4 are *shading*. Group 1 is a geometry switch that happens to live here.

---

## 1. Geometry

### Shaded image (2-D)

A **geometry switch.** ON = flat map, OFF = 3-D surface.

- **ON** — the grid is drawn as a flat, draped 2-D image (fast map, no relief).
- **OFF** — the grid is drawn as a real 3-D warped surface.

In flat mode the shade is baked into the texture, so the two [Hillshade](#2-relief-look-illumination)
boxes are the **only** illumination control — the PBR material / light / IBL / occlusion controls do
nothing to a baked image and are **greyed out**. With **both** Hillshade boxes off the image is drawn
as plain CPT colour, no shade (a deliberate flat-map look). The sun **Az / El** still steer whichever
hillshade is on.

Enabled only when there is a grid to flip (`gnx > 1`).

---

## 2. Relief look (illumination)

Three **mutually exclusive** checkboxes — turning one on unchecks the other two — and **all may be
off**. With all off, the surface is lit by [group 3](#3-material-lights-pbr) instead.

| Checkbox | What it does |
|----------|--------------|
| **Cast shadows** | Lit render where the sun casts real self-shadows on the terrain. 3-D only — a flat plane has no relief, so it is a no-op in 2-D. |
| **Hillshade (Lambert)** | Baked per-vertex shade `CPT(z) · (ambient + (1-ambient)·max(0, n·L))`. Uses the mesh normal, corrected for vertical exaggeration. Drawn unlit. |
| **Hillshade (grdimage)** | The GMT `grdimage` look: z-gradient normal, `atan` soft-clip, `gmt_illuminate` in HSV. Independent of vertical exaggeration. Drawn unlit. |

Both hillshades bake colour flat and render **unlit**, so the material and light sliders of
group 3 have no visible effect while a hillshade is on.

Knobs (fixed defaults, no dock slider): `hillAmbient` = 0.25 (Lambert shadow floor),
`hillGain` = 2.0 (grdimage contrast). See [Hillshade](hillshade.md) for the full algorithm.

---

## 3. Material & lights (PBR)

These bite **only when all relief looks in group 2 are off** — i.e. a PBR-lit surface. Under a
hillshade they do nothing. In flat "Shaded image" mode they are all **greyed out** (a baked texture
takes no PBR light).

| Control | Effect |
|---------|--------|
| **Roughness** slider | PBR microfacet roughness (matte ↔ glossy) |
| **Metallic** slider | PBR metalness |
| **Light intensity** slider | Sun brightness |
| **Az** / **El** sliders | Sun direction (azimuth / elevation) |
| **Fill** slider | Opposite-side fill light strength |
| **Image-based light** checkbox | Enables environment (IBL) lighting |
| **Env (IBL)** slider | IBL strength |

---

## 4. Post-process passes

Screen-space passes applied to the **whole frame** (surface, axes, gridlines and all). Pass order:
`base → [SSAO] → [tone] → [FXAA]`.

| Control | Effect |
|---------|--------|
| **Ambient occlusion (SSAO)** checkbox + **SSAO radius** slider | Darkens creases and contact shadows |
| **Tone mapping** checkbox | HDR → display tone curve |
| **FXAA** checkbox | Screen-space anti-aliasing. Because it re-samples the whole frame, it can shift the apparent thickness of thin lines. |

---

## Notes / gotchas

- **Group 1 is geometry, not shading** — "Shaded image (2-D)" toggles flat vs 3-D; it never
  shades on its own.
- **Groups 2 and 3 cancel** — turning a hillshade on silently deadens every group-3 slider,
  since a hillshade renders unlit.
- **Group 4 is global** — post passes touch the whole frame, not just the surface. In flat 2-D
  the cube-axes X/Y gridlines are suppressed so FXAA no longer re-thicknesses them.
