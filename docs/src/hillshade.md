# Hillshade

The viewer offers **two alternative baked-hillshade styles** for a relief surface. Both write a
per-vertex RGB colour, leave the active z scalars untouched (so the colour bar still maps z), and
are drawn UNLIT so the baked colour shows verbatim (`applySurfStyle` sets flat / ambient 1 /
diffuse 0 when hillshade is on).

**Where:** `deps/src/40_shading.cpp` — `hillshadeMapper(Scene *s, vtkActor *act)` plus the
`gmtIlluminate(double intensity, double *rgb)` helper just above it. Driven per-surface by
`applySurfStyle()`, called from BOTH `applyShading()` (all resident actors) and `ensureNodeActor()`
(each new LOD tile), so a tile built mid-flight matches the rest.

**Toggles (Shading dock):** three mutually-exclusive checkboxes — *Cast shadows*, *Hillshade
(Lambert)*, *Hillshade (grdimage)* — all may be off. State lives in `s->useHillshade` (master
on/off) + `s->hillGrd` (false = Lambert, true = grdimage) + `s->useShadows`. Defaults all OFF.

**Knobs:** `s->hillAmbient` (default 0.25) — Lambert shadow floor; `s->hillGain` (default 2.0) —
grdimage atan contrast (higher = punchier).

---

## Style A — Lambert (the original look)

Per-node colour = `CPT(z) * Lambert shade`. The baked mesh normal is **VE-corrected** onto the
displayed relief `(xfac, 1, zfac*ve)`:

```
n' = normalize(n.x/xfac, n.y, n.z/(zfac*ve))
shade = max(0, n'·L)
I = hillAmbient + (1 - hillAmbient) * shade        // ambient floor so valleys aren't pure black
rgb = CPT(z) * I                                   // darken-only multiply
```

The shade tracks the on-screen vertical exaggeration (change VE → shade changes). `el=90` (sun
overhead) → flat ground brightest. This is the classic look; kept as an option.

---

## Style B — GMT grdimage from the z-gradient (VE-independent)

It is the GMT `grdgradient | grdimage -I` pipeline, baked on the CPU into a per-vertex RGB array.

### 1. Intensity from the z-gradient

The grid's per-vertex normal is baked in `makeGridFromArray` / `makeGridTile` (`10_geometry.cpp`)
from **central differences of z in TRUE DATA units**:

```
gx = dz/dx,  gy = dz/dy           // data units: dx,dy in data coords, z in data z-units
n  = normalize(-gx, -gy, 1)
```

So the normal **is** the z-gradient — no separate gradient pass is needed, and because it is the
DATA-space normal the relief does **NOT** change with the display vertical exaggeration (VE-
independent). The sun direction uses an **inverted (complementary) elevation** `90 - lightEl`, so
the slider reads the right way round for this mode:

```
elG = 90 - lightEl
LxG = sin(az)cos(elG),  LyG = cos(az)cos(elG),  LzG = sin(elG)
raw = n·LG - LzG                  // slope-toward-sun signal: 0 on flat, >0 sun-facing, <0 in shade
```

`raw` is the directional-derivative relief signal (0 on flat ground). The elevation is flipped
because, for this raw data-space normal, the un-flipped angle drove the slider backwards.

Soft-clip to `(-1,1)` with an atan, GMT `grdgradient -Nt` style (`amp = s->hillGain`):

```
intensity = (2/π) * atan(hillGain * raw)
```

No global gradient statistic is needed (unlike `-Nt`'s default RMS sigma or `-Ne`'s Laplace CDF),
so it is seam-free across LOD tiles and cheap to recompute on a slider move.

### 2. Colour modulation (grdimage -I via gmt_illuminate)

`gmtIlluminate()` ports GMT's `gmt_illuminate()` (gmt_support.c): modulate the CPT colour by the
intensity **in HSV space, preserving hue**. Positive lightens, negative darkens, 0 = unchanged.
GMT defaults reproduced verbatim:

```
intensity > 0:  di = 1-intensity;  s = di*s + intensity*0.1;  v = di*v + intensity*1.0   // MAX_S, MAX_V
intensity < 0:  di = 1+intensity;  s = di*s - intensity*1.0;  v = di*v - intensity*0.3   // MIN_S, MIN_V
```

(`COLOR_HSV_MAX_S 0.1, MIN_S 1.0, MAX_V 1.0, MIN_V 0.3`.) RGB↔HSV via `vtkMath::RGBToHSV` /
`HSVToRGB` (doubles, 0..1). The CPT colour itself comes from `lut->MapValue(z)`; the active z
scalars are left untouched so the colour bar still maps z.

### 3. Mapper wiring

Baked RGB written to a point-field `vtkUnsignedCharArray` named `"hillshade"`; the mapper is set to
`SetScalarModeToUsePointFieldData` + `SelectColorArray("hillshade")` + `SetColorModeToDirectScalars`
(no LUT). When `useHillshade` is off, revert to `UsePointData` + `MapScalars` (live CPT through the
LUT).

---

## Cost / scope

CPU loop over every surface point each time `applyShading` runs (and once per new LOD tile). For the
tiled-LOD grid (any grid >512 in either dim) it runs per resident tile, so it scales with the drawn
LOD, not the full grid. Light az/el sliders re-bake (cheap, local). Both single-actor and tiled
paths bake identical data-unit normals, so the look is uniform and seam-free.
