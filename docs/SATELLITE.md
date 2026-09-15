| **File** / **URL** | Which source to read. The radio button picks; the `...` button browses. Naming a source reads it at once — there is no "load" step. |
# Satellite — orbit propagation and ground tracks

SGP4/SDP4 satellite orbit propagation, from TLEs, plotted as ground tracks in an iGMT window.

The maths is Bill Gray's [`sat_code`](https://github.com/Bill-Gray/sat_code) (MIT), vendored
verbatim in `deps/src/sat_code/`. It is compiled straight into `gmtvtk.dll` — there is no external
program, no plugin to install, and no network access needed unless you fetch TLEs from a URL.

---

## 1. Quick start

1. Open a window showing a **world map** (or leave an empty launcher window — see §5).
2. **Satellite → Ground tracks…**
3. Paste a URL (§2) into the **URL** field, or browse to a `.tle` file.
4. The list fills as soon as the source is given.
5. Select one or more, press **Plot ground track**.

That is the whole loop. Double-clicking a name in the list plots it directly.

---

## 2. Where TLEs come from

A **TLE** (Two-Line Element set) is the standard 2- or 3-line text description of an orbit:

```
ISS (ZARYA)
1 25544U 98067A   24015.50000000  .00016717  00000-0  30777-3 0  9005
2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49514637 10110
```

**Celestrak** is the usual free source — no account, no key. Paste any of these into the URL field:

| What | URL |
|---|---|
| ISS + space stations (~20) | `https://celestrak.org/NORAD/elements/gp.php?GROUP=stations&FORMAT=tle` |
| One object by catalogue number | `https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=tle` |
| Search by name | `https://celestrak.org/NORAD/elements/gp.php?NAME=ISS&FORMAT=tle` |
| Everything active (~16000) | `https://celestrak.org/NORAD/elements/gp.php?GROUP=active&FORMAT=tle` |

Other `GROUP=` values Celestrak publishes: `weather`, `noaa`, `goes`, `resource`, `gps-ops`,
`galileo`, `glo-ops`, `beidou`, `starlink`, `science`, `geo`, `amateur`, `cubesat`, `visual`.

**`FORMAT=tle` matters** — the reader wants plain three-line text, not their JSON or CSV.

**space-track.org** has the full catalogue and historical elements, but requires a login, so its
URLs will not work in the URL box. Download the file yourself and use **File** instead.

### TLEs go stale

A TLE is accurate for roughly a few days either side of **its own epoch**, and a low-earth orbit
degrades noticeably within a week. Re-download rather than keeping an old file. This is why the
dialog's **Start** default is *TLE epoch*, not *now*: it propagates from where the elements are
actually valid. Use *Now (UTC)* when you have just downloaded fresh ones.

---

## 3. The dialog

| Control | What it does |
|---|---|
| **Pick** | A ready-made Celestrak query (Earth-observation ones first). Choosing one fills the URL box, which stays editable. The first entry, *key missions (short list)*, is a curated ~25-row set (Terra, Aqua, Landsat 8/9, the Sentinels, Suomi NPP, NOAA 20, SWOT, GOES, Meteosat-11/12): it reads several small group files — `\|`-joined URLs are read as one list — and keeps only the named missions, because Celestrak has no query for "these twelve" and its own groups are either far too broad or split across files. Typing in the URL box clears that filter; the query is then yours. |
| **File** / **URL** | Which source to read. The radio button picks; the `...` button browses. |
| **Satellite list** | Everything found. Ctrl/Shift-click for several; double-click plots one. |
| **Start** | *Now (UTC)* (default) or *TLE epoch*. Anchored at **now** the span runs BACKWARD, so the track ENDS at this instant — which is where the spacecraft model is planted. Anchored at the TLE's own epoch it runs forward from there. |
| **Duration** + unit | How much orbit to draw: `revolutions` (default), `minutes`, or `hours`. |
| **Step (s)** | Sampling interval along the track. 30 s keeps a LEO track smooth. |
| **Draw at real altitude** | On (default): the true 3-D ORBIT at the satellite's height (§5). A flat-2-D window is switched to 3-D, since looking straight down a 420 km lift is invisible. Off: the flat GROUND TRACK on the map — the sub-satellite point. |
| **Frame** | Which curve the 3-D orbit is: *Earth-fixed* (over the ground — each revolution lands further west), *Inertial* (the orbital plane, the closed ring), or *Automatic* (default: inertial for a geosynchronous orbit, Earth-fixed for everything else). See §5. |
| **Plot track** | Propagates the selected objects and draws them. |
| **Update orbit** | Brings what is already plotted up to THIS instant: sets the anchor to *Now* and re-propagates, so the track ends and the spacecraft stands where the satellite is right now. |

Every widget of the **Time span** block is live: changing the anchor, the duration, its unit or the
step — Enter included — re-propagates and redraws at once, spacecraft body and all. Nothing is
computed until you have plotted once, so typing in a fresh dialog does nothing.

The status line at the bottom reports what happened, including warnings.

### Sensible values

- **LEO** (ISS, Starlink, imaging satellites): 1–3 revolutions, step 30 s.
- **Geostationary**: `revolutions` is nearly a whole day. The ORBIT is a full ring at 35 786 km, so
  the view is backed off to take it in. Step 300 s is plenty. (The flat GROUND TRACK of the same
  satellite is the tiny figure-of-eight — the analemma — because in the Earth-fixed frame a
  geostationary satellite does not travel.)
- **Molniya / highly eccentric**: 1–2 revolutions, step 60 s. These are deep-space orbits and are
  propagated with SDP4 automatically (§7).

---

## 4. Parking (minimise)

The window's **X** (or Esc, or Minimise) does **not** destroy the dialog — there is no Close button.
It hides and leaves a row in the **Scene Objects** dock called *Satellite tracks*. From there:

- **double-click** the row — brings the dialog back, list and settings intact
- **click** the row — a menu with **Show** and **Delete**
- **Delete** — really closes it

Re-picking **Satellite → Ground tracks…** also brings back a parked dialog rather than opening a
second one. This is the same parking mechanism as *Make movie* and *Illumination*.

---

## 5. Which window to plot into

This is the one thing worth understanding, because it decides whether you see anything.

**A ground track is global data** — it spans the full ±180° of longitude.

| Window | What happens |
|---|---|
| **Empty launcher** | A **whole-world** Base Map is laid down — never cropped to the track — and the track drawn on it. Works. |
| **World map / global grid** | The track is drawn on top. Works. |
| **Small region** (a local grid) | The track is plotted correctly but lies **outside the view**, so you see nothing. The status line says so. |

The third case is deliberate, not a bug: a vector overlay dropped on a window that already shows a
raster never moves that raster's axes (this is a standing rule in `SACRED_LAW.md` — the
vector-import law). Cropping your view to Portugal and asking for the ISS track cannot be answered
by silently re-framing the map you were looking at. **Open a world map, or an empty window.**

### Where it lands in Scene Objects

One master row, **Satellites**, and under it ONE ROW PER SATELLITE, named after it. Each of those is a
group holding the two parts it is drawn in:

```
Satellites
  SENTINEL-1A
      Track
      Spacecraft
  SENTINEL-2A
      Track
      Spacecraft
```

The satellite's own row switches it as a whole and its **Remove** takes both parts with it; each part
keeps its own checkbox and its own properties menu underneath. The master's **Remove** takes every
satellite. Standard elements throughout: toggle, properties, remove.

*Show data table* gives `lon`, `lat`, `alt_km` and `time (UTC)` — what the track IS. The plotted `z`
is deliberately NOT there: it is an internal of the drawing, described next.

`z` is the *plotted* height, in the viewer's world units — the altitude divided by 111.195 km, one
degree of equatorial arc. That conversion is the whole trick: the globe engine places a point at
`r = globeR + z·(zfac·ve)` with `globeR = 180/π`, i.e. **one degree of arc is one world unit**, so a
height expressed that way is geometrically true on the flat map *and* on the sphere, with no second
formula. An ISS orbit at 420 km becomes 3.78 units — a radius 6.6 % above the surface, which is
exactly 420/6371.

Putting the raw kilometres in that column (the obvious-looking thing, and how this first shipped)
draws the line 420 units over a 360-wide world — off-screen, and the reason it once looked as though
nothing had been plotted. With **Draw at real altitude** off, `z` is 0 and you get a plain flat
ground track.

The track is drawn as a thin ORANGE TUBE — real 3-D geometry (the same `makeCurveTube` the magnetic
field lines use), so it takes perspective, stands off the globe at its true altitude, and is occluded
by the planet rather than painted over it. Orange because a track has to read against ocean, land,
ice and the night side alike.

The track is **cut at the dateline**, so it draws as a proper map track instead of streaking back
across the plot. Each pass is its own segment, and the cut lands EXACTLY on ±180 with a point on
both sides — on the globe those two meridians are the same line, so the orbit stays continuous
there instead of showing a gap.

### Which FRAME the 3-D curve is drawn in

Two different questions, and neither answers both:

- **Earth-fixed** — where the satellite goes relative to the PLANET, lifted to its altitude. Each
  revolution lands about 22.5° further west, because the Earth turned underneath. That is the whole
  story of a low orbit: TERRA is sun-synchronous and must be seen to walk around the globe.
- **Inertial** — the ORBIT itself, the closed path in the frame it is closed in. For a geostationary
  satellite this is the only one that shows an orbit at all: Earth-fixed it does not travel, and its
  curve collapses to a thin figure-of-eight (the analemma) hanging over one longitude.

The **Frame** combo picks. **Automatic** (the default) decides from the orbit's own physics and never
from a list of names: a satellite that keeps station with the Earth's rotation — period within 1 % of
a sidereal day — has no Earth-fixed path worth drawing and gets the inertial curve; everything else is
drawn over the ground. Say which one you want and that always wins.

The inertial curve is built from the propagator's own TEME positions and brought into the display
frame by ONE rotation: the Earth's rotation angle at a single reference epoch, the END of the track —
where the spacecraft body stands. The shape is then the true orbit and it is hung over the geography
correctly for that instant. (Any other reference epoch just spins the ring about the pole.) That angle
is not re-derived here: it is read off the same propagator by asking one epoch in both frames and
taking the angle between them.

With **Draw at real altitude** OFF you get the sub-satellite point, flat on the map — which is what a
ground track is, and has no frame question to answer.

A high orbit also has to FIT: a geostationary ring is 6.6 Earth radii, well outside a camera framed on
the planet, so the view is backed off far enough to see it. Only when it does not already fit — a LEO
track leaves your view untouched — and the camera only, never the axes.

---

## 6. Using it from Julia

The GUI is a thin skin over a normal API.

```julia
using InteractiveGMT, GMT

# read TLEs — a path, or the text itself
sats = read_tle("stations.tle")

s = Satellite(sats[1])              # parse + set the model up once
println(s)                          # Satellite(ISS (ZARYA), SGP4, 92.9 min, incl 51.64°, epoch …)

# a window to draw in
fig = view_grid(gmtread("@earth_relief_01d"))

plot_groundtrack!(fig, s; revolutions = 2, step = Second(30))
```

Other entry points:

```julia
D = groundtrack(s; revolutions = 1, step = Second(60))          # 3-D, frame chosen by the orbit (§5)
D = groundtrack(s; revolutions = 2, frame = :earthfixed)        # …over the ground, always
D = groundtrack(s; revolutions = 1, frame = :inertial)          # …the orbital plane, always
D = groundtrack(s; revolutions = 1, altitude = false)           # flat ground track (sub-satellite point)
pos, vel = propagate(s, DateTime(2024,1,15,12,0,0))      # TEME state, km and km/s, 3×N
ecef     = propagate_ecef(s, when)                       # earth-fixed, km, 3×N
lon, lat, alt = subpoint(s, when)                        # sub-satellite point
```

`when` accepts a `DateTime`, a vector or range of them, or Julian Days as `Float64`.

Accessors: `epoch`, `epoch_jd`, `norad_number`, `mean_motion`, `inclination`, `eccentricity`,
`period` (minutes), `model`. They are not exported (`epoch`, `period` and `model` are far too
generic to put in your namespace) — reach them as `InteractiveGMT.period(s)`.

Handles are freed by a finalizer; `InteractiveGMT.close!(s)` frees one early.

---

## 7. What it computes, and how accurate it is

**Model choice is automatic.** The ephemeris type in a TLE is a request, not an answer — published
TLEs nearly all claim type 0. The real decision comes from the orbit's period: 225 minutes or more
is "deep space" and gets **SDP4**, otherwise **SGP4**. Without this, every Molniya and every
geostationary object would be propagated with the near-earth model and be wrong by hundreds of
kilometres. `model(s)` reports what was actually chosen.

**Units.** Positions km, velocities km/s, in TEME (true equator, mean equinox of date). Longitude
and latitude are geodetic degrees on WGS84; altitude is km above the **ellipsoid**, not the geoid.

**Accuracy floor.** Ground tracks are rotated to earth-fixed using GMST alone — UT1−UTC (< 0.9 s),
polar motion and the equation of the equinoxes are neglected. That is worth well under a kilometre,
and is far below the error of the TLE itself, which is good to about a kilometre *at epoch* and
degrades daily. Do not read metre-level meaning into these numbers.

**Verification.** `deps/src/test_satellite.c` checks the propagator against Bill Gray's own
published state vectors (`deps/src/sat_code/test2_reference.txt`) — 61 blocks, 419 epochs, all five
models, near-earth and deep-space — and matches them to **1e-8 km, the last printed digit**, plus
independent tests of GMST, the geodetic conversion and the calendar.

### One subtlety, if you ever compare against published vectors

`propagate` takes a Julian Day. A JD near 2.44e6 resolves only ~40 µs, so `epoch + tsince/1440`
does not round-trip exactly. For most orbits that is sub-metre. For a **resonant** deep-space orbit
it is not: SDP4 integrates the resonance term by stepping a persistent state toward the requested
time, and that is genuinely discontinuous at that resolution — measured at ~12 m on Gray's own
11801 test case. If you need to reproduce a published vector exactly, the C API has
`sat_propagate_tsince` (minutes from epoch). For map work the JD path is the right default.

---

## 8. Where the code lives

| Path | What |
|---|---|
| `deps/src/sat_code/` | Upstream, verbatim, MIT. **Never edit** — see its `PROVENANCE.md`. |
| `deps/src/satellite.h/.cpp` | The C API (`sat_*`). All project-added code lives here. |
| `deps/src/test_satellite.c` | Regression against upstream's published vectors. |
| `src/satellite.jl` | Julia API + the dialog's callback. |
| `deps/ui/satellite.ui` | The dialog, loaded at runtime via QUiLoader. |
| `deps/src/70_window.cpp` | `SatelliteDialog` + the **Satellite** menu. |

`satellite.cpp` and the `sat_code` files are a second translation unit of `gmtvtk.dll` (like
`mbgrid.c`) — not fragments `#include`d into `gmtvtk.cpp`.
