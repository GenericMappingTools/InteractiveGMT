# SWOT (sea surface and water heights)

!!! tip "Tutorial"
    Worked examples and what every variable is: [Tutorial: SWOT](74c-swot-tutorial.md) (the green **?** in the dialog).

*Satellite → SWOT…*

SWOT (Surface Water and Ocean Topography, NASA/CNES, launched December 2022) measures the height of
the sea and of rivers and lakes with a radar interferometer, KaRIn, that sees two 60 km wide swaths
at once, one on each side of the satellite's track, with a 20 km gap under it. This dialog finds the
SWOT files over a region and a period at NASA's PO.DAAC, downloads them with your Earthdata login,
and shows the variable you choose in the window.

## The products

| Product | What it is | Shown as |
|---|---|---|
| **LR sea surface height, Basic (2 km)** | the ocean: sea surface height and its anomaly, 2 km pixels | gridded over the region |
| **LR sea surface height, Expert (2 km)** | the same, plus radar backscatter, wave height, wind speed and the corrections | gridded over the region |
| **LR sea surface height L3, Basic (AVISO, 2 km)** | the LR passes calibrated, edited and denoised by CNES/CLS (DUACS): `ssha_filtered`, `ssha_unfiltered`, `mdt` | gridded over the region |
| **LR sea surface height L3, Expert (AVISO, 2 km)** | the same plus MSS, tides, DAC, `sigma0` and geostrophic currents (`ugos_filtered`, `vgos_filtered`, …) | gridded over the region |
| **HR water surface, Raster 100 m** | rivers, lakes, coasts: water surface elevation, water area | the tile's own UTM grid |
| **HR water surface, Raster 250 m** | the same at 250 m | the tile's own UTM grid |
| **HR rivers, reaches (shapefile)** | river stretches of ~10 km (SWORD database): height, width, slope, discharge | lines over the window |
| **HR rivers, nodes (shapefile)** | points every ~200 m along the rivers: height, width | points over the window |
| **HR lakes, prior lakes (shapefile)** | the lakes of the prior lake database seen by the pass: height, area | outlines over the window |

An **LR** file is one *pass*: half an orbit, from 78° N to 78° S (or back). Its pixels are not on a
map grid, so the part of the pass inside the region is gridded (GMT `nearneighbor`, search radius two
increments). An **HR Raster** file is one 128 km *tile*, already a regular grid in its UTM zone, and
is shown as it is, with its own axes in metres. The **river and lake** products are one zip file of
shapefiles per pass and continent; it is read where it lies (GDAL reads inside the zip, nothing is
unpacked). Only the features over the region are drawn, as ONE layer in Scene Objects laid over what
the window shows (no new axes); hovering a feature shows its id and the value of the chosen
**Variable** (`no value` where SWOT has none). Nodes are one point layer, not thousands.

SWOT repeats its ground tracks every 21 days (a *cycle*), so a place is seen by the same passes, with
the same numbers, every cycle.

## The dialog

Two tabs, **Search** and **Accounts**, and a status line under them.

### Region

The four limits, in degrees. **Use the window's limits** copies the region the window is showing.

### Product and period

- **Product** — one of the four above.
- **From**, **To** — the period (UTC days, both included). The dialog opens on the last 30 days.
- **Find the passes** — asks for the files of the product over the region in the period, lists them
  in **Pass**, and draws the two edges of every swath of their passes: one line element per pass
  (`Pass 432 (cycles 55, 56)`), all under one Scene Objects group named for the search, e.g.
  `SWOT swaths 2026-09-01 to 2026-09-28`. On an empty window a map with coastlines is made around the
  region first; otherwise the lines are laid over what is shown. A new search replaces the previous
  swaths.
- **Pick a pass** — then click one of those swath lines in the view: that pass is **shown**, exactly
  as choosing it in the **Pass** list and pressing **Download and show** (downloaded first if it is
  not on disk; the oldest of its files when the period holds several cycles — the list has the
  others). Press the button again to cancel. The same is in every swath line's own menu,
  **Show this pass (SWOT)** (it brings the dialog back if it was closed to the dock).
  The swath lines are guides, not data: their menus have no *Clamp to ground*, *Fill polygons* or
  *Show names*.

!!! tip "Double-click edits"
    Double-clicking a line, any line, puts it in vertex edit mode (handles on its points); it does
    not choose a pass. Double-click it again to leave.

!!! note "Passes that do not cross the region are left out"
    NASA's search gives every pass that crosses the antimeridian (180°) a footprint covering the
    **whole globe**, so it returns those passes for any region you ask about. The dialog removes them
    using the orbit itself: a pass follows the same ground track in every cycle, and one pass's track
    is the next one's shifted by a fixed longitude, so the outline of every pass is known. The status
    line says how many such files were left out. For Raster tiles the tile's UTM zone must also be
    one the region covers. A region that reaches beyond 75° of latitude is not filtered (there the
    outline is not accurate enough), so every file NASA returns is listed.

### File

- **Pass** — the files found, oldest first: start time (UTC), cycle and pass, and for the Raster the
  UTM tile (e.g. `UTM29T 040F`). `(on disk)` marks a file already in the **Folder**.
- **Variable** — the array to show. Before the file is downloaded the list holds the usual ones;
  once it is on disk the list is the file's own. For the LR products:
  - `ssha_karin` — sea surface height **anomaly**: the height with the mean sea surface, the tides and
    the atmospheric effect removed. What you normally want for currents and eddies.
  - `ssh_karin` — sea surface height above the ellipsoid.
  - `ssha_karin_2`, `ssh_karin_2` — the same, with a model wet troposphere correction instead of the
    radiometer's.

  For the Raster: `wse` (water surface elevation), `water_area`, `water_frac`, `sig0`, … For rivers
  and lakes: the attribute shown on hover — `wse`, `width`, `slope`, `dschg_c` (discharge),
  `area_total`, …
- **Quality** — uses the variable's own quality field (`<variable>_qual`), when it has one:
  - *Good only* — no flag raised.
  - *Good and suspect* — also values flagged only as suspect (LR: the `suspect_*` bits of the flag;
    Raster: quality 1), but not those degraded or bad. Rivers and lakes use the feature's own
    quality attribute (`reach_q`, `node_q`, `quality_f`) the same way.
  - *Every value* — no quality filter.
- **Increment** — grid spacing in degrees for the LR products (their pixels are 2 km, about 0.02°).
- **Folder** — where the files are downloaded to. It is made if it does not exist; a file already
  there is not downloaded again. Default: `Downloads\SWOT` in your home folder (the environment
  variable `IGMT_SWOT_DIR` changes it).
- **Download** — downloads the chosen file only.
- **Download and show** — downloads it if needed, then adds the variable to the window as a new
  grid with its own name, e.g. `ssha_karin — SWOT cycle 55 pass 432 2026-09-05`.

A pass that only crosses land in your region has no sea surface height there; the status line then
says that the pass has no value of that quality inside the region.

### Accounts

PO.DAAC needs a free **NASA Earthdata** account, the same one the *MODIS scenes* and *Oceancolor*
tools use. Type its login and password and press **Save login**: they are written to the
`machine urs.earthdata.nasa.gov` entry of `.netrc` in your home folder, the file curl, wget and GDAL
read. Every other entry of that file is kept.

## The Level-3 products (AVISO+)

The L3 products are made by CNES/CLS from the same passes and distributed by AVISO+, not by NASA:

- **Finding them needs no account.** They are not in NASA's search; the dialog reads AVISO's own
  public THREDDS catalogue (version 3.0, the reprocessed archive first, then the recent "forward"
  cycles) for the cycles of the period, and keeps the passes that cross the region, with the same
  orbit geometry as above. Only the **science orbit** is searched (from 26 July 2023); before that
  SWOT flew a 1-day cal/val orbit.
- **Downloading them needs an AVISO+ login**, saved in the Accounts tab like the Earthdata one (the
  `machine tds-odatis.aviso.altimetry.fr` entry of `.netrc`). A refused login is reported as such.
- **Variables:** `ssha_filtered` (noise removed — the one to look at first), `ssha_unfiltered` (before
  the noise removal; AVISO recommends it if you replace the mean sea surface), `mdt`; Expert adds
  `ssha_unedited`, `mss`, `ocean_tide`, `dac`, `internal_tide`, `sigma0` and the geostrophic
  velocities `ugos_filtered`/`vgos_filtered` (absolute) and `ugosa_*`/`vgosa_*` (anomalies).
- **Quality** uses the file's `quality_flag` (one code per pixel): *Good only* keeps 0; *Good and
  suspect* keeps codes up to 30, AVISO's own advice for events such as hurricanes, where its strict
  editing removes real signal (most useful with `ssha_unedited`). The `ssha_filtered` and
  `ssha_unfiltered` arrays already have the flags applied by AVISO.
