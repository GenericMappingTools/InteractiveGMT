# Shading Dock

The **Shading** dock (right edge, folds to a side strip) tunes how a grid surface is lit and
drawn. It mixes four *unrelated* concerns, so read it by group rather than top-to-bottom.

| Group | Purpose |
|-------|---------|
| [1. Geometry](#1-geometry) | Flat 2-D map vs real 3-D surface |
| [2. Relief look](#2-relief-look-illumination) | How the terrain is illuminated (PBR / hillshade / cast shadows) |
| [3. Material & lights](#3-material-lights-pbr) | PBR material + light sliders (tune the **Shade (PBR)** look) |
| [4. Post-process](#4-post-process-passes) | Screen-space passes over the whole frame |

Only groups 2–4 are *shading*. Group 1 is a geometry switch that happens to live here.

---

## 1. Geometry

### Shaded image (2-D)

A **geometry switch.** ON = flat map, OFF = 3-D surface.

- **ON** — the grid is drawn as a flat, draped 2-D image (fast map, no relief).
- **OFF** — the grid is drawn as a real 3-D warped surface.

In flat mode the shade is baked into the texture. The relief look is chosen by the group-2 boxes:
**Shade (PBR)** bakes a CPU approximation of the lit 3-D surface (so "Shaded image" on its own
reproduces the loaded-grid look), or a **Hillshade** style, or — with every look off — plain CPT
colour, no shade (a deliberate flat-map look). The sun **Az / El** steer any lit look; the PBR
material + Light / Fill sliders are live only under **Shade (PBR)**.

Enabled only when there is a grid to flip (`gnx > 1`).

---

## 2. Relief look (illumination)

Four **mutually exclusive** checkboxes — turning one on unchecks the other three — and **all may be
off** (on a flat image, all off = plain, unshaded).

| Checkbox | What it does |
|----------|--------------|
| **Shade (PBR)** | The default lit look. On a 3-D surface it is the GPU physically-based shading (group 3). On a flat image it bakes a CPU approximation of that same look into the texture, so "Shaded image" alone reproduces the loaded-grid relief. |
| **Cast shadows** | Lit render where the sun casts real self-shadows on the terrain. 3-D only — a flat plane has no relief, so it is a no-op in 2-D. |
| **Hillshade (Lambert)** | Baked shade: colour × directional slope shade (`ambient` floor keeps valleys visible). Corrected for vertical exaggeration. Drawn unlit. |
| **Hillshade (grdimage)** | The GMT `grdimage` look: slope-toward-sun relief, soft-clipped, illuminated in HSV. Independent of vertical exaggeration. Drawn unlit. |

Both hillshades bake colour flat and render **unlit**, so the material and light sliders of
group 3 have no visible effect while a hillshade is on. The sun **Az / El** (group 3) do steer both
the hillshades and the PBR bake.

Knobs (fixed defaults, no dock slider): `hillAmbient` = 0.25 (Lambert shadow floor),
`hillGain` = 2.0 (grdimage contrast). See [Hillshade](hillshade.md) for the full algorithm.

---

## 3. Material & lights (PBR)

The default look — selected by **Shade (PBR)** in group 2. A 3-D surface is lit by **physically-based
rendering (PBR)**: a microfacet (Cook–Torrance) material catching a directional *sun* light plus a
softer *fill* light, and optionally a sky environment (IBL). This is the shaded relief you see the
moment a grid is loaded; the surface's own slopes catch the light, so no baked hillshade is needed.

On the GPU this needs 3-D geometry to shade. A flat "Shaded image" has no geometry, so the same look
is instead **baked on the CPU** into the drape texture (a close approximation — the sky-IBL and
screen passes don't carry over). These controls tune the look and are **live** on a 3-D surface or a
flat PBR bake; they are **greyed out** on a flat image showing a hillshade or a plain (unshaded)
picture, where they'd do nothing. Under a hillshade the baked colour replaces the lighting, so they
have no effect there either.

The controls below tune the PBR look:

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
| **Tone mapping** checkbox | HDR → display tone curve. Greyed out under a hillshade (baked colours are shown verbatim). |
| **FXAA** checkbox | Screen-space anti-aliasing. Greyed out under a hillshade. Because it re-samples the whole frame, it can shift the apparent thickness of thin lines. |

---

## Notes / gotchas

- **The default look is PBR** — a freshly loaded grid uses **Shade (PBR)**: physically-based
  rendering (sun + fill light) on a 3-D surface, or a baked CPU approximation on a flat image.
- **Group 1 is geometry, not shading** — "Shaded image (2-D)" toggles flat vs 3-D; it never
  shades on its own.
- **Only one relief look at a time** — PBR, Cast shadows, and the two Hillshades are mutually
  exclusive. Under a hillshade the group-3 sliders deaden (a hillshade renders unlit); on a flat
  image the controls a given look can't use are greyed out, so nothing silently does nothing.
- **Group 4 is global** — post passes touch the whole frame, not just the surface. In flat 2-D
  the cube-axes X/Y gridlines are suppressed so FXAA no longer re-thicknesses them.
