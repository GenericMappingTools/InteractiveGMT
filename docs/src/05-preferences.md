```@meta
CurrentModule = InteractiveGMT
```

# Preferences

**File → Preferences…** opens a small settings dialog. Values are persisted with `QSettings`
(under organisation `InteractiveGMT`, application `i'GMT`) and reloaded every session, so a choice
made once survives restarts. Settings apply to **newly created** objects / **subsequent** actions —
existing objects are not retro-changed (use *Line Properties* for per-object overrides).

The **OK** button is the only control that writes the settings; editing a combo box does not commit
anything until you press OK.

## Default directory

The folder every file **Open** / **Save** dialog starts in. It is the head of a most-recently-used
(MRU) list: after you pick a file anywhere, that file's folder becomes the new default and joins the
list (deduplicated, capped at 12 entries). The combo offers the whole list; the `...` button browses
for a folder. The list persists across sessions.

## Dist/Azim type

The approximation used by the *Line length…* and *Azimuth…* measure tools (right-click a line):

| Type | Engine | Notes |
|------|--------|-------|
| `Ellipsoidal` | `GMT.invgeod` (Karney geodesic on WGS84) | exact ellipsoidal distance/azimuth |
| `Spherical`   | `GMT.invgeod` on a sphere (`+proj=longlat +ellps=sphere`) | great-circle |
| `Flat Earth`  | `GMT.mapproject -jf` | ignores meridian convergence (fast, least accurate at high latitude) |

**Dir** selects forward (default) or backward azimuths.

## Measure units

Unit the *Line length…* tool reports in: `meters`, `kilometers`, `nautical miles`, or `miles`.
Cartesian (non-geographic) objects are always reported in data units regardless of this setting.

Polygon **area** uses `GMT.geodesicarea` for `Ellipsoidal` and `GMT.gmtspatial -Q` otherwise
(e.g. a spherical geographic area), reported in km² (+ m²).

## Coastlines color

Line colour for the **Geography** menu's coastlines / political boundaries / rivers overlays.
Limited to **Black** or **White** — any other colour is set per-object through *Line Properties*.

## Default line color / thickness

Colour and width given to **new** lines and polygons drawn with the toolbar tools.

- **Default line color** — the leading entry, **Orange**, is the program's original line colour and
  the default, so the familiar look is preserved. The rest are the basic named colours; anything
  else is set per-line via *Line Properties*.
- **Default line thickness** — `1`–`4 pt`. A point maps to `1.25` screen pixels, so `2 pt`
  (the default) is the historical `2.5 px` width.
