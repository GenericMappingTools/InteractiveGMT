# InteractiveGMT — Vision

## Mission

InteractiveGMT (**i'GMT**) is a graphical front end to [GMT](https://www.generic-mapping-tools.org/)
and its Julia extensions ([GMT.jl](https://github.com/GenericMappingTools/GMT.jl)) — not a
replacement for it. GMT's power has always come from being a scriptable, composable command-line
toolset; i'GMT's job is to make that power discoverable, just not and immediate, :).
Most dialogs are thin skin over a real GMT.jl call, and every action a user takes graphically should be,
in future, reproducible, inspectable, and scriptable from the
Julia console docked in the same window. **Scripting is the backbone, not a fallback.**

## The Five Pillars

### 1. A real GUI for GMT — without abandoning the command line

Grids, images, tables, and solids open into a native Qt6 + VTK window with proper interaction
(gizmo, cube axes, shading, colour bars, context menus) — but the in-window Julia console keeps
the underlying GMT.jl calls one keystroke away. Menu actions are GMT.jl one-liners with a UI in
front of them, not a parallel reimplementation. A user should be able to build a figure by
clicking, then lift the equivalent script straight out of the console history.

### 2. 3-D, volumetric, and spherical, natively

GMT's native domain is the 2-D map; i'GMT's is what comes after: 3-D grids and point clouds today,
extending toward true volumetric rendering (netCDF cubes, layered earth models) and spherical
(whole-globe, non-planar) visualization — the geometries a flat `-JX`/`-JM` projection was never
meant to carry. The Qt+VTK core exists specifically because this class of rendering has no home in
classic GMT PostScript output.

### 3. Rescue the Mirone legacy

[Mirone](https://github.com/joa-quim/Mirone) accumulated two decades of MATLAB-based geoscience
utilities — many with no equivalent in any other package, free or commercial: tsunami modelling
(NSWING), Okada elastic deformation, IGRF, focal mechanisms, grid transplanting, nested-grid
tools, seismicity catalogs, tile mosaics, and more. Most would otherwise be lost to an aging,
license-gated runtime. Porting them into i'GMT — one dialog, one Julia module at a time — is not
nostalgia; it's preservation of working science that deserves a modern, open home.

### 4. Native multibeam ingestion

Bring [MB-System](https://www.mbari.org/technology/mb-system/)-readable multibeam formats
directly into i'GMT, and mirror the useful parts of MB-System's own GUI tools (swath editing,
cleaning, mosaicking) inside the same window a grid or point cloud already opens in — instead of
requiring a separate application and a file round-trip.

### 5. Investigate swallowing GMTSAR

InSAR processing ([GMTSAR](https://topex.ucsd.edu/gmtsar/)) is presently a shell-script-driven
pipeline bolted onto GMT. Whether it can be "librarified" — exposed as a callable library GMT.jl
(and by extension i'GMT) can drive directly, the way GMT's own modules already are — is an open
feasibility question, not a commitment. Worth investigating; worth swallowing whole if it proves
tractable.

## What Stays True Throughout

- **GMT command-line fidelity first.** Every graphical feature maps to real GMT/GMT.jl calls a
  script could also make. No dialog is allowed to become the *only* way to do something.
- **Julia, not a shadow language.** Extensions are Julia modules calling GMT.jl, not a bespoke
  scripting layer competing with it.
- **Windows today, portable in spirit.** The current viewer binary is Windows-only by
  circumstance (the build toolchain), not by design — nothing in the architecture is
  Windows-specific.
