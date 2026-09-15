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
4. Press **Load satellites** — the list fills.
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
| **Pick** | A ready-made Celestrak query (Earth-observation ones first). Choosing one fills the URL box, which stays editable. |
| **File** / **URL** | Which source to read. The radio button picks; the `...` button browses. |
| **Load satellites** | Reads the source and fills the list. Nothing is propagated yet. |
| **Satellite list** | Everything found. Ctrl/Shift-click for several; double-click plots one. |
| **Start** | *TLE epoch* (default, recommended) or *Now (UTC)*. |
| **Duration** + unit | How much orbit to draw: `revolutions` (default), `minutes`, or `hours`. |
| **Step (s)** | Sampling interval along the track. 30 s keeps a LEO track smooth. |
| **Draw at real altitude** | On (default): a true 3-D curve at the satellite's height, so it rises off the globe in 3-D. Off: flat on the map. |
| **Plot ground track** | Propagates the selected objects and draws them. |
| **Close** | **Parks** the dialog (see §4) — it does not throw your settings away. |

The status line at the bottom reports what happened, including warnings.

### Sensible values

- **LEO** (ISS, Starlink, imaging satellites): 1–3 revolutions, step 30 s.
- **Geostationary**: `revolutions` is nearly a whole day; the track is a tiny figure-of-eight or a
  point. Step 300 s is plenty.
- **Molniya / highly eccentric**: 1–2 revolutions, step 60 s. These are deep-space orbits and are
  propagated with SDP4 automatically (§7).

---

## 4. Parking (minimise)

Pressing **Close** (or the window's X) does **not** destroy the dialog. It hides and leaves a row
in the **Scene Objects** dock called *Satellite tracks*. From there:

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

Every track gets a row under one **Satellite tracks** group, named after the satellite. Standard
element: toggle it, open its properties, remove it. The track carries `lon`, `lat`, `z`, `alt_km`,
`time_jd` — visible in *Show data table*.

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

The track is **cut at the dateline**, so it draws as a proper map track instead of streaking back
across the plot. Each pass therefore appears as its own segment.

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
D = groundtrack(s; revolutions = 1, step = Second(60))          # 3-D curve at true altitude
D = groundtrack(s; revolutions = 1, altitude = false)           # flat ground track
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
