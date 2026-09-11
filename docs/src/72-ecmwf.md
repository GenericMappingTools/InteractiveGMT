# Copernicus / ECMWF

*Geophysics → Copernicus → ERA5 / ECMWF download…*

Weather and climate data, fetched from the two European services and dropped straight into the
window. The tool wraps GMT.jl's `ecmwf`, which is **one function with two halves** — and so this is
one dialog with two blocks:

| Source | What it is | Credentials |
|---|---|---|
| **ERA5 reanalysis (CDS)** | The Copernicus Climate Data Store's reanalysis — a reconstruction of the past, hour by hour, from 1940 onwards | A `~/.cdsapirc` file with your API key |
| **ECMWF forecast (open data)** | The open-data forecast runs from `data.ecmwf.int` — the coming days, not the past | None |

The **Source** radio at the top picks which half you are asking, and greys the other block out.
Everything about *what a variable is*, what the request looks like and how it is posted belongs to
GMT.jl; the dialog is the side that knows which box belongs to which option.

## Getting an ERA5 key

The reanalysis half talks to the Climate Data Store, which needs a free account. Register at
[cds.climate.copernicus.eu](https://cds.climate.copernicus.eu), open your profile, and copy the two
lines it shows under *API key* into a file called `.cdsapirc` in your home directory:

```
url: https://cds.climate.copernicus.eu/api
key: <the key the site shows you>
```

You must also **accept the licence of each dataset once**, on its own download page, before the API
will serve it. A request for a dataset whose terms you have not accepted comes back as a refusal
naming the licence — that is the CDS talking, not iGMT.

The forecast half needs none of this.

## The fastest way in: Example

**Example** is a toggle at the bottom-left. Press it and the block that is currently selected fills
with a request that runs as it stands:

| | ERA5 | Forecast |
|---|---|---|
| Dataset | `reanalysis-era5-single-levels` | — |
| Variables | `t2m` | `2t` |
| Pressure levels | off, empty | empty |
| Date / hour | `2024-12-06`, `12` | most recent run |
| Steps | — | `0,6,12` |

`t2m` and `2t` are the same field — 2‑metre air temperature — spelled the way each catalogue spells
it. Switching the Source radio while Example is down moves the example to the other block, so
exactly one example is ever standing.

Press it again and every box it filled is **emptied**. It does not put back what was there before:
a released Example leaves a clean dialog for a request of your own.

## Variables

The **…** button beside either Variables box opens the catalogue — GMT's own table, not a copy —
with a checkbox on every row and whatever the box already held pre-ticked:

```
alnid    Near Ir Albedo For Diffuse Radiation   (0 - 1)
t2m      2 Metre Temperature                    K
skt      Skin Temperature                       K
…
```

The **Filter** box at the top keeps only the rows containing what you type — `temp` finds both
`2m_temperature` and `skin_temperature`, `[K]` every variable in kelvin. It is plain text, not a
pattern, and case does not matter. Filtering only *hides* rows: anything already ticked stays
ticked and is still returned, so you can tick, filter again, tick more, and take the lot with **OK**.

Which catalogue you get follows the **Pressure levels** tick: single-level and pressure-level ERA5
are two different CDS datasets with two different variable lists, and a variable from one does not
exist in the other. The tick moves the dataset name with it.

## Pressure levels

The level box takes one level, a list (`1000,925,850`) or a range (`1000:-100:500`). Legal values
are 1000, 925, 850, 700, 500, 300, 250, 200, 50 (and 5, 4, 3, 2, 1), or `all`.

It only means anything for a **3‑D variable** — `t`, `u`, `v`, `w`, `z`, `q`, `r` on the ERA5 side,
`t`, `u`, `v`, `w`, `gh`, `q`, `r`, `d`, `vo` on the forecast side. A surface field like `t2m` or
`2t` ignores it.

Left empty:

* **ERA5** sends no pressure level at all, which only a single-level dataset accepts. A
  pressure-level dataset needs the box filled.
* **Forecast** defaults to **1000 hPa**.

Asking for more than one level (and, on the forecast side, more than one step) gives a **3‑D cube**,
if *Save multi-level/step as a 3‑D cube* is ticked. The cube opens with its **Cube layers** window,
whose slider walks the levels or the steps; the iGMT title bar names the layer you are on.

## Dates, hours and steps

| Box | Takes | Empty means |
|---|---|---|
| **Date(s)** (ERA5) | one date `2024-12-06`, a list, or a range `2024-12-06:2024-12-10` | five days ago |
| **Hour** (ERA5) | `12`, a list `0,6,12`, or a range `10:14` | the API's own default |
| **Date** (forecast) | a full date, or a plain days-back count `0`–`3` | the most recent run |
| **Run hour** (forecast) | 00, 06, 12 or 18 UTC | the most recent run on the server |
| **Steps (h)** (forecast) | `0`, a list `0,3,6`, or a range `0:3:12` | step 0 |

The calendar glyph inside either date box picks a date without typing. The ERA5 box may hold a list
or a range, so its calendar **adds** to what is there — two picks make `d1,d2`, and you turn that
comma into a colon for a range. The forecast box holds one date and is replaced.

A CDS request is a *year × month × day* product, exactly as the CDS web form is: a span that crosses
a month boundary therefore also asks for the other month's days.

Hours travel to the server as `HH:00`, which is the only form the CDS accepts — type `12`, `12:00`
or `0,6,12` and the right thing goes out.

## Pasting a request from the CDS site

Every CDS dataset page has a **Show API request** button. Two ways to use what it gives you:

* Paste it into the big box at the bottom of the ERA5 block, or
* copy it and tick **Request from clipboard … (optional)**.

Either way the request travels as it stands and the Variables, levels, date and hour boxes above are
ignored — they grey out to say so. The **Dataset** box still matters: it is what the request is
posted to.

## Region and output

**Region** takes `W/E/S/N`, or a GMT region code (`PT`, `IHO23`, …). Empty means the whole globe.

**Longitude** decides how a *global* result is framed: `-180/180` (the default, and what the rest of
iGMT works in) or `0/360`, which is ERA5's own frame. The shift is GMT's `grdedit -S`, which moves
values across the periodic boundary — nothing is resampled and no value changes. A request with a
Region of its own keeps the frame that region asked for.

**Temperature** is Celsius by default. ECMWF serves kelvin, which reads badly on a colour bar, so
273.15 is subtracted from the downloaded grid and its unit relabelled. The test is the grid's **own
declared unit**, never the variable name: `t2m`, `skt`, `sst` and a pressure-level `t` all convert,
while a wind component, a pressure or a humidity never does.

**Format** is netCDF or GRIB. **Load the result into this window** sends whatever lands through the
same file door a drag-and-drop goes through, so a downloaded grid arrives with its own axes, its own
colour bar and its own Scene Objects row.

### Where the file goes

Leave **Save as** empty and the box shows, greyed, where the download will land: a per-user temp
folder, under a name built from the request —

```
ERA5_t2m_2024-12-06_12h_degC.nc
ERA5_t_850-500hPa_2024-12-06-2024-12-08_0-6h.nc
ECMWF_2t_step0-6-12.nc
```

Source, variables, levels, dates, and the display choices that were applied to the file. Nothing is
ever written inside the package directory.

That name is a **function of the request**, which is what makes the second ask cheap: the same
request twice finds the first answer still on disk, and the tool says *"Already downloaded, reused"*
instead of going back to the server. Change the variable, the date, the longitude frame or the
temperature unit and it is a different name, so it is fetched properly.

Type a path into **Save as** and that is used instead. A *directory* is enough for the forecast
mode, which names each variable's file itself.

## Dry run, and the download

**Dry run** builds the request and shows it without posting anything:

```json
{"product_type": ["reanalysis"],
"variable": [
	"2m_temperature"
],
"year": ["2024"],
"month": ["12"],
"day": ["06"],
"time": ["12:00"],
"data_format": "netcdf",
"download_format": "unarchived"
}
```

Its one button is **Copy to clipboard** — a dry run exists to be used elsewhere, pasted into a
script, a mail or the CDS page. The window's close box dismisses it.

**Download** does the real thing. A CDS request is queued on the server before any bytes move, so
the progress window first says *waiting for the Copernicus server* and then counts the megabytes as
they land. The fetch runs as a background task and the viewer stays usable throughout — you can turn
the scene, open another dialog, and watch it arrive.

## When something goes wrong

Failures do not open a message box over the boxes they are about. They go to the window's
**Messages** log, which opens itself so nothing passes unseen, and stays there beside whatever the
Julia side printed on its own way up. Common ones:

| Message | Means |
|---|---|
| `Variable "t2m" not found in the pressure-level dataset` | A surface variable with **Pressure levels** ticked — untick it, or ask for a 3‑D variable |
| `give me at least one variable` | The Variables box is empty and no request was pasted |
| A licence refusal naming the dataset | Accept that dataset's terms once, on its CDS page |
| `credentials file …/.cdsapirc not found` | The ERA5 half has no key — see *Getting an ERA5 key* above |

## Minimising

The dialog's **minimise** parks it as a row in Scene Objects instead of vanishing to the task bar:
a request half filled in is kept while you look at something else, and a double-click on the row
brings it back exactly as you left it.
