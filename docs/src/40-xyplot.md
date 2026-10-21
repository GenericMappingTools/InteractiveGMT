```@meta
CurrentModule = InteractiveGMT
```

# X,Y Plot Tool

A standalone 2-D plotter for profiles, time series and spectra: its own window, with a chart, an
**Object Manager** (one row per series), a foldable **Data Viewer**, pages (tabs), and the
**File / Analysis / GMT / Misc** menus. It is a port of Mirone's `ecran`.

Ways to open it:

- from Julia, with `xyplot` (below);
- from the 3-D viewer, **Tools > X,Y plot** (an empty window);
- from a 3-D profile: right-click the profile panel > **Open in X,Y plot tool**;
- by dropping (or `iview`-ing) a two-column table that is not geographic.

One X,Y window serves all `xyplot` calls: while one is open, a new `xyplot` call adds a **page** to
it instead of opening a second window.

## Opening the Plotter

```julia
using InteractiveGMT, GMT

# x, y vectors (y may be a matrix: one line per column)
t = collect(0:0.1:10)
fig = xyplot(t, sin.(t); name="sin", xlabel="time", ylabel="amplitude")

# a matrix: column 1 = X, the others = Y series
fig = xyplot([t sin.(t) cos.(t)])

# y alone: X = 1:length(y)
fig = xyplot(rand(100))

# a GMTdataset (column names become the series names) or a multi-segment vector
D = gmtread("data.txt")
fig = xyplot(D)
```

Keywords: `name`, `color` (`:red`, `"blue"`, a 0–255 or 0–1 triple), `linewidth`, `linestyle`
(`:solid`, `:dash`, `:dot`, `:dashdot`, `:dashdotdot`, `:none`), `marker` (`:none`, `:cross`,
`:plus`, `:square`, `:circle`, `:diamond`), `markersize`, `title`, `xlabel`, `ylabel`, `xtime`,
`xscale`, `yscale`, `track`.

## Adding Series

```julia
add!(fig, more_data)                                    # a GMTdataset, vector of them, or matrix
add!(fig, x, y; name="Series 2", linestyle=:dash)      # or one series by hand
add!(fig, x, y; kind=:bar, barwidth=0.5)                # drawn as bars
```

## Clearing

```julia
clear!(fig)
```

## Series Properties

Right-click a series in the Object Manager: **Line properties…** (colour, width, style, marker;
bar width for bars), **Show in Data Table**, **Save data…**, **Rename…**, **Delete**. Analysis items
act on the series selected in the Object Manager.

## Time Series

With X in Unix epoch seconds, the X axis can be labelled as dates or times. Ticks follow zoom and pan.

```julia
t = time() .+ (0:3600)
y = rand(length(t))
fig = xyplot(t, y; xtime=:date)          # xtime=true is the same as :date
```

or later:

```julia
xtime!(fig, :date_ymd)
```

Modes: `:linear`, `:date` (by span), `:date_ymd`, `:time`, `:decyear`, `:doy`. Same choices in
**Misc > Time axis (X)**.

## Log Axes

```julia
fig = xyplot(x, y; xscale=:log)                     # log X
fig = xyplot(x, y; yscale=:log)                     # log Y
fig = xyplot(x, y; xscale=:log, yscale=:log)        # both
```

or later (`true` = log, `false` = linear):

```julia
logscale!(fig; x=true, y=false)
```

Same in **Misc > Log X axis / Log Y axis**.

## Stick Diagrams

```julia
t = collect(0:0.1:10)
fig = stickplot(t, cos.(t), sin.(t))                 # vector components u, v

az  = 360 .* rand(100)
fig = stickplot(1:100, az; mag=rand(100))           # azimuth (0 = North) + magnitude
```

## Profiles from the 3-D Viewer

Right-click a 3-D profile > **Open in X,Y plot tool**, or from Julia `profile_to_xyplot(fig)`.
The profile keeps its track (lon/lat), so the magnetic model below takes its azimuth and distances
from it. Redrawing the profile in the 3-D window updates the X,Y copy. From Julia, a series gets a
track with `add!(p, x, y; track=(lon, lat))`.

## File Menu

- **New**: an empty X,Y window.
- **Open… / Save…**: read and write tables (through GMT).
- **Session > Open… / Save…**: the whole window (pages, series, styles, axes, magnetic state) in an
  `.igmtz` session file, the same container as the 3-D viewer's sessions.
- **Plot with GMT.jl…**: the current page as a GMT.jl script, in an editor, to Run, Save or run
  standalone.

## Analysis

Pick a series in the Object Manager first.

| Item | Result |
|------|--------|
| Remove Mean, Remove Trend | new series on the same page |
| FFT — Amplitude Spectrum, FFT — Power Spectrum Density | new page |
| Autocorrelation | new page |
| 1st derivative, 2nd derivative | new page (keeps a time axis) |
| Fit polynomial… | fitted curve, degree asked |
| Smoothing (Savitzky-Golay)… | smoothed curve, window asked |
| Filter (Butterworth)… | low- or high-pass, cutoff asked |
| Filter outliers (despike)… | despiked curve, threshold in robust σ |
| Histogram… | new page, bars (0 bins = automatic) |
| Bar graph | the series as bars on a new page |
| Sound Velocity Profile… | new page "SVP diagram" |
| Reference a curve to another… | new page "Referenced" |
| Depth to sources (Spector-Grant) | interactive fit on a spectrum |

### Sound Velocity Profile

For CTD or Argo-type data: pick the Temperature (°C), Salinity and Pressure (dbar) columns and give
the latitude. Sound speed comes from Chen & Millero (UNESCO 1983), depth from pressure by the
TEOS-10 formula. The result is a page of sound speed (m/s) against depth (m).

### Reference a Curve to Another

Click points alternately on the selected curve (red) and on a second curve (green); a right- or
double-click ends picking. Right-click a red pick > **Reference me**: the selected curve is
stretched between the picks to match the other and drawn on a new page.

### Depth to Sources (Spector-Grant)

Turn the item on over a power spectrum and left-drag a frequency band: a fit line and the depth are
shown live. Fit lines stay; drag their ends to edit them. Right-click a line for **Slope(m/cycle) &
Intercept**, **Bandpass Filter** (applies the band to the 3-D window's grid the spectrum came
from, as a new grid), **Recomp Slope/Intercept** and **Delete this line**.

## GMT Menu

**spectrum1d…** runs GMT's `spectrum1d` on any column (auto-spectrum) or a pair of columns
(cross-spectra): segment size, sample interval, detrend mode, wavelength axis, and the outputs X/Y
power, cross power, noise power, coherency, admittance, gain and phase, each on its own page.

## Magnetic Anomaly Modelling

Toolbar **Geomagnetic bar code** shows a strip under the chart: **Start age** / **End age**,
**Create Mag Bar** (the polarity bar code above the chart), **Synthetic RTP** (a synthetic profile
reduced to the pole), **Parameters…** (declination, inclination, spreading rate, azimuths, layer
depth, distance unit, bathymetry grid, isochron list), the contamination slider, the isochron age,
and **Fit** (moves the synthetic to its best correlation with the measured profile).

On the chart, right-click the red star > **Guide me** and click to give a first guess; right-click
the fitted isochron > **Pin-point me** to keep it; right-click a kept one > **Delete me**.

## Pages

Click **+** to add a page. Right-click a tab: **New**, **Duplicate**, **Rename…**, **Copy to Page**,
**Delete**. Each page keeps its own series, axes and time mode.

## Toolbar and Misc

**Zoom to fit**, **Grid** (also Misc > Grid), and Misc > **Panels** (shows or hides the side panels).
