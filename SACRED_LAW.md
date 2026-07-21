# THE SACRED LAW

Same operation, ALWAYS same function. No forks. No parallel re-implementations. No special-casing
that makes a shared operation behave differently — or do nothing — for one element type.

**Diagnostic:** if a shared control does something DIFFERENT (or nothing) for element X vs a plain
grid, the law is violated. Example: unchecking "Shaded image (2-D)" must run the SAME 3-D surface
shading a plain grid runs (`gmtvtk_replace_base_grid_h` -> `hillshadeMapper`). Making it a no-op /
leaving the tsunami flat = violation.

**Why:** stated after an entire afternoon of fury (2026-07-18). Every fork of the maths (a separate
Julia `grdgradient` hillshade), every third baker added, every DISABLED/short-circuited control "to
prevent breakage" for one element type (tsunami vs grid) was a fresh violation. Disabling a control
to prevent breakage is NOT a fix — it IS the violation.

**How to apply:** before touching any shared control/operation, ask: "does a plain grid do this
through function F? then EVERY element type must go through F too." Concrete engine for the
Aquamoto/shading case: `applyReliefShade` / `applyPBRShade` / `makeReliefLight` / `bakeLayerRGBA` /
`hillshadeMapper` (`40_shading.cpp`) is the ONE relief-shade source of truth — never fork that
maths again. Flat-image restore is the only legitimate custom bit (a host-composited blend has no
single CPT); the 3-D surface toggle uses the plain-grid builder verbatim.

Related rules, same law applied to other areas:
- one quantity (length/area/azimuth) = one function, never reimplemented for a seed/preview
- same-type elements (nested rects, drawn polygons) share ONE source of truth for behavior, never
  a per-construction-site flag
- elements belong to a tagged GROUP; per-group ops apply BY TAG, never by topmost/active guessing

## Derived-variable display law

Whenever an operation computes a NEW derived variable (RTP, component, cropped grid, filtered
grid, any "compute X from Y" result):

- the new variable is added to the SAME iGMT window, as a new handle with a DESCRIPTIVE name, in
  Scene Objects — never a bare/silent replace, never an unnamed row
- the new variable is CHECKED (visible) by default; the previous/source variable is UNCHECKED
- Scene Objects always UNFOLDS to reveal the new variable — never leave it collapsed/hidden inside
  a closed group

## Group-uncheck law

Unchecking the TOP element of a group in Scene Objects (e.g. a grid's own handle/container row)
must uncheck EVERY element of that group too — Surface, Color Bar, Axes, drape, every child row.
A group with its top box unchecked but any child still showing checked is a violation, full stop.

**Why:** found 2026-07-21 — hiding a base surface (crop's "uncheck the source" rule above) hid the
Surface row correctly but left Color Bar and Axes still checked, because `rebuildSceneObjects`
computed those two child rows' checked state independently of the container's own visibility
instead of gating on it. Confirmed live by the user's own Scene Objects screenshot: `layer0.grd`
unchecked + Surface unchecked, but Color Bar and Axes still checked underneath it.

**How to apply:** every child row builder (colorbar, axes, drape, or any future per-group row) must
take the container's own visibility as a `grpVisible` gate on its OWN checked state — never default
it to always-true. This applies whether the group got hidden by clicking its own checkbox (already
cascades correctly, see `beginGroupHandle`'s toggle handler) OR programmatically, by some other code
calling `SetVisibility(0)` directly on the group's actor (crop, RTP3D, IGRF, Okada, grdsample, or
any future derived-variable path) — `rebuildSceneObjects` must recompute every child's checked state
from the CURRENT actor visibility, not carry a stale independent default.

## Derived-variable axes law

A NEW derived variable's axes/frame must fit ITS OWN limits, not the parent's. Cropping a
[-3,3]x[-3,3] grid down to [-1,1]x[-1,1] must re-frame the axes cube (and camera) to [-1,1]x[-1,1]
— never leave the axes still spanning the original, now-hidden parent's extent.

**Why:** found 2026-07-21 — the axes cube (`s->axes`, a `vtkCubeAxesActor`) only ever tracked the
window's PRIMARY surface bounds (`applyVE`, 10_geometry.cpp: `surfGetBounds(s,b); s->axes->SetBounds(b)`).
A crop lives as an EXTRA (never replaces the primary — the "new named handle, never a silent
replace" rule above), so hiding the parent and showing the crop left the axes/camera still framed
to the parent's original, now-invisible extent. Same law as everything above: the derived result
must look like a first-class, self-contained thing, not a fragment still wearing its parent's frame.

**How to apply:** `gmtvtk_reframe_h(handle, x0,x1,y0,y1, keepMargin)` (90_c_api.cpp) sets the axes
bounds AND re-fits the camera to an arbitrary caller-supplied bbox (generalizes `fitSnapView`'s
technique, which is hard-wired to `surfGetBounds` = the primary only). `keepMargin` matters: images
keep a margin (fill=0.84, same as `gmtvtk_view_grid`'s referenced-image path) so their axis labels
stay on screen; grids fill edge-to-edge (fill=1.0) — that's the EXISTING, correct grid convention,
not something this law should change. Call it with the derived variable's own bbox after every
crop/derive path that can leave the axes pointing at stale parent limits — same call sites as the
derived-variable display law and group-uncheck law above.

**Second layer, found the same day when the first fix still showed a correctly-sized/margined box
with ZERO tick-label text:** `SetBounds`/`SetXAxisRange`/`SetYAxisRange` on `s->axes` only move the
`vtkCubeAxesActor`'s own (invisible — deliberately turned off, different text engine) native labels.
The actual VISIBLE tick text for X, Y, **and** Z is a wholly separate custom freetype-billboard
system (`rebuildAxisLabels`, 10_geometry.cpp), and it positions every billboard from
`surfGetBounds()` — the PRIMARY surface's bounds — same root cause as the first layer, one function
deeper. Fixed at the true source: `Scene::viewBoundsOverride`/`viewBounds` (10_geometry.cpp), checked
by `surfGetBounds()` itself before falling back to the real actor bounds. `gmtvtk_reframe_h` sets it;
every bounds-driven function (`applyVE`, `fitSnapView`, `rebuildAxisLabels` — anything that calls
`surfGetBounds`) then stays consistent automatically, with zero changes to any of them individually.
**Lesson: when a bounds/frame value has more than one consumer, fix the SHARED source they all read
from, not each call site — a per-call-site fix (the first layer here) will keep surfacing "new"
bugs that are really the same one.**
