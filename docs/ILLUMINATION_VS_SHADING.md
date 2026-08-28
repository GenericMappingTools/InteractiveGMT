# Illumination (Hillshade) tool vs the Shading dock

Status 2026-07-28. Written after porting Mirone's `shading_params.m`. Two control surfaces now
produce relief shading in the same window, and some of their entries carry the same NAME while
running different code. This is the audit of what each one actually computes, where the SACRED LAW
is really violated, and what is only a naming problem.

---

## 1. What was built (this session)

`View > Illumination (Hillshade)…` — port of Mirone `src_figs/shading_params.m`.
C++ dialog `HillshadeDialog` (70_window.cpp) + one Julia callback `_on_hillshade` (src/hillshade.jl).

Mirone's 3 (Peucker), 5 (Manip Raster) and 6 (ESRI hillshade) were dropped by request; the survivors
are RENUMBERED 1..9 continuously (Mirone's own number in brackets; this program's own looks have none):

| # | Method | Computed by | Result |
|---|---|---|---|
| 1 | GMT grdgradient classic | `grdgradient -A<az> -Nt` (+`-M` when geographic) | intensity |
| 2 | GMT grdgradient Lambertian | `grdgradient -Es<az>/<el>` | intensity |
| 3 | Lambertian with lighting (4) | `grdgradient -E<az>/<el>+a<amb>+d<dif>+p<spec>+s<shine>` | intensity |
| 4 | Hillshade (grdimage) | the Shading dock's look, in C++ (`applyReliefShade`) — never reaches Julia | C++ look |
| 5 | Hillshade (Lambert) | the Shading dock's look, in C++ (`applyReliefShade`) — never reaches Julia | C++ look |
| 6 | Shade (PBR) | the Shading dock's look, in C++ — VTK's PBR material on a 3-D surface, `applyPBRShade`'s CPU Cook-Torrance bake under "Shaded image (2-D)" | C++ look |
| 7 | False colour (7) | three azimuths → R,G,B | **new IMAGE** |
| 8 | Dynamic Range Compression (8) | `GMT.kovesi` (ppdrc), then illuminated as model 1 | **new GRID** |
| 9 | Remove illumination (9) | — | restore original |

Plus the "Old algorithm" sub-option of model 7 (a port of `shade_manip_raster`, mirone.m) with its
Amp factor — restored after it was wrongly dropped with Manip Raster.

Model 6's own four sliders are the dock's **Light**, **Fill**, **Roughness** and **Metallic** — the
Scene fields `applyPBRShade` reads besides the sun, which comes from the dialog's existing azimuth
compass and elevation quarter-circle. Metalness was hard-wired to the dielectric case in the bake
(so the dock's Metallic slider moved nothing on a flat image while it plainly changed the 3-D
surface); it now follows VTK's own PBR shader — F0 lerped from 4% toward the albedo, diffuse scaled
by `(1 - metallic)`, and the fill/ambient stand-in for the missing environment tinted by F0 rather
than dropped, so a fully metallic map does not go black.

**Where the intensity goes.** Models 1/2/3/8 produce a per-node REFLECTANCE grid, exactly what
Mirone hands to `mex_illuminate`. `mex_illuminate` IS `gmt_illuminate`, which this program already
owns as `gmtIlluminate` (40_shading.cpp) — the one HSV modulator every shade ends in. So the grid
is pushed down (`gmtvtk_set_shade_intensity_h`) into `Scene::shadeInten` and the shade engine takes
the intensity FROM IT instead of deriving one from the surface normal. Sampled by WORLD position,
so the 3-D surface, every LOD tile, the flat 2-D bake and Aquamoto all read it through one call.

**Fixed along the way**

- `GMT.kovesi` no longer needs FFTW. New `GMT.fft2d` (GMT_FFT_2D) in GMT.jl; `src/kovesi.jl` is a
  plain include, the FFTW stub is gone, and the broken duplicate `ext/GMTKovesiExt` was deleted.
  kovesi's private `bwdist` was shadowing GMT's real one — removed, infill uses `bwdist_idx`.
- ✕ ("Remove illumination") = Mirone's `ImageResetOrigImg_CB` = **restore the original**, not just
  drop the reflectance. Models 5/6 display a derived product and uncheck the source, so light-off
  alone changed nothing on screen. `_hs_restore_original` un-displays the product, re-checks the
  base grid, re-frames axes + Z to it.
- `gmtvtk_set_object_visible` could not act on the BASE surface (extras-only loop), so nothing could
  re-check a base that the derived-variable display law had unchecked. Base branch added there.
- Models are ALTERNATIVES, not a pipeline: every model calls `_hs_restore_original` before computing,
  so 6 (ppdrc) then 1 no longer paints the source's reflectance onto the ppdrc field.

---

## 2. The Shading dock

`View > Shading Panel`. Four MUTUALLY EXCLUSIVE relief "looks" (all four may be off), plus the
independent geometry toggle "Shaded image (2-D)":

| Dock entry | Scene flags | What it computes |
|---|---|---|
| Shade (PBR) | `!useHillshade && !useShadows`, `litBake` | 3-D: the GPU PBR material. Flat image: `applyPBRShade`, a CPU Cook-Torrance (GGX + Smith + Schlick, metallic 0) |
| Cast shadows | `useShadows` | VTK shadow-map pass, sun self-shadowing. 3-D only |
| Hillshade (Lambert) | `useHillshade && !hillGrd` | `applyReliefShade` branch (A) |
| Hillshade (grdimage) | `useHillshade && hillGrd` | `applyReliefShade` branch (B) |

Sliders: sun Azimuth / Elevation, gain (`hillGain`), ambient (`hillAmbient`), roughness, metallic,
key + fill intensity, IBL, SSAO, tone, FXAA.

The two hillshade branches (40_shading.cpp, `applyReliefShade`):

```
(A) Lambert   n' = normalize(n.x/xfac, n.y, n.z/(zfac·ve))        # VE-corrected to the SCREEN relief
              I  = amb + (1-amb)·max(0, n'·L)
              rgb = CPT(z) · I                                     # darken-only multiply

(B) grdimage  raw   = n·Lg - Lg.z                                  # Lg = sun with INVERTED elevation
              inten = (2/π)·atan(gain·raw)                         # soft clip to (-1,1)
              rgb   = gmtIlluminate(CPT(z), inten)                 # GMT's HSV modulation

(ext)         rgb   = gmtIlluminate(CPT(z), shadeInten(x,y))       # the Illumination tool's grid
```

---

## 3. Overlap — where the two really collide

### 3.1 "Hillshade (grdimage)" (dock) vs model 1 "GMT grdgradient classic"

**Same operation, two implementations.** `grdgradient -Nt` IS an `atan` normalisation of the
directional derivative; dock branch (B) is `(2/π)·atan(gain·raw)` of a directional derivative taken
from the VTK normal. Both then call `gmtIlluminate`. Same result kind, same intent, same advertised
name ("grdimage"), different code: one is a GMT module call in Julia over the grid, the other is a
C++ re-derivation from the mesh normal.

**Verdict: this is the genuine SACRED LAW exposure.** Mitigating facts: the *modulator* is already
shared (`gmtIlluminate`, one function), and the two live at different layers — (B) needs no Julia
round-trip and works per LOD tile with no grid resident, which is why it exists. But the maths of
"reflectance from a slope and a sun" is written twice.

### 3.2 "Hillshade (Lambert)" (dock) vs model 2 `-Es` and model 3 `-E`

Three things called Lambertian:
- dock (A): `amb + (1-amb)·max(0, n'·L)`, darken-only multiply, normal VE-corrected to the DISPLAYED
  relief (so it changes when the user drags vertical exaggeration).
- model 2 `-Es`: GMT's simulated illumination, Lambertian from the TRUE-coord gradient, VE-independent,
  fed through `gmtIlluminate` (lightens AND darkens).
- model 3 `-E`: GMT's Lambertian + ambient/diffuse/specular/shine — a Phong model, not Lambert at all.

**Verdict: naming violation, not a maths fork.** They are genuinely different reflectance models;
the problem is that the UI gives two of them the same word and the third a word that is wrong.

### 3.3 The word "Hillshade"

Means two different things: the dock's two branches (which the tool now offers as its own models 4
and 5, calling the very same `sceneSetReliefLook`) and the tool's own name, "Illumination
(Hillshade)". The third meaning is gone with the ESRI model.

### 3.4 The dock's checkboxes LIE after a model is loaded

`gmtvtk_set_shade_intensity_h` sets `useHillshade = true; hillGrd = true` for any loaded model, and
`syncShadeChecks` derives the boxes from those flags. So after running model 1 or model 2
(`-Es`), the dock shows **"Hillshade (grdimage)" checked** — a look that is NOT what is on screen
(`applyReliefShade` short-circuits to the external grid before it ever reaches branch B).

**Verdict: a real defect.** A shared control reporting a state that isn't true. Needs a fifth,
read-only state in the dock — e.g. "External (Illumination model N)" — or the boxes must go
tri-state while a model is loaded.

### 3.5 Sun Az/El after a model is loaded

The dock's sun sliders call `dropExternShade` and revert to the dock's own analytic shade. That
obeys "a shared control must bite, never sit inert", but the user's model silently disappears while
the Illumination dialog still shows that model selected. Two defensible designs:
- (a) current: dock wins, model dropped — then the dialog must visibly deselect its model;
- (b) better: the sliders re-run the LOADED model with the new azimuth/elevation (Mirone's own
  behaviour — the compass drives the model), and only the four look checkboxes drop it.

(b) is the one that matches the tool's compass and removes the surprise. Not implemented.

---

## 4. What is NOT a violation

- `gmtIlluminate` — every path ends there: dock branch (B), the external reflectance, the flat 2-D
  bake, Aquamoto. One function, no forks.
- `applyReliefShade` / `makeReliefLight` / `bakeLayerRGBA` / `hillshadeMapper` — the 3-D surface and
  the flat image bake share them verbatim, which is why a model looks the same in 2-D and 3-D.
- `externShadeAt` — one sampler, world coordinates, so no consumer needs to know the grid's indexing.
- `_hs_restore_original` — the ✕ and every model switch use the same restore, not two copies.

---

## 5. Proposed fixes (not applied)

1. **Rename the dock's entries** to what they actually are:
   `Hillshade (Lambert)` → `Relief (Lambert, VE-aware)`, `Hillshade (grdimage)` → `Relief
   (grdimage -I)`. Keeps the honest label, stops competing with the tool's model names.
2. **Show the loaded model in the dock.** A read-only row "Illumination: model N" that appears when
   `haveExternShade(s)`, with the four look boxes unchecked while it is up. Kills 3.4.
3. **Sun sliders re-run the loaded model** (option (b) of 3.5) — the dock asks the Illumination
   callback for a new reflectance at the new az/el instead of discarding it.
4. **Collapse 3.1 if it ever needs to change.** Branch (B) and model 1 must never be "fixed"
   independently; if the grdimage look is ever tuned, the tuning belongs in one place. Today the
   shared piece is only `gmtIlluminate`; the slope→intensity step is duplicated.
5. **Model 3 is not Lambertian** — label it "Phong (ambient/diffuse/specular)" in the dialog.

---

## 6. Open items

- **JIT latency in the Illumination tool.** First click on any model pays Julia compilation
  (`grdgradient`, `kovesi`, the push path). Next task, not addressed here.
- Model 6 wavelength defaults to `floor(max(size)/2)`; Mirone's dialog leaves it blank meaning the
  same. Confirmed equivalent, no action.
- `grdgradient` still round-trips grids through GMT with the pad copies the user wants gone
  (stated intent: convert it to C++ eventually).
