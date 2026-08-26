```@meta
CurrentModule = InteractiveGMT
```

# Geography Tools

## Coastlines

Add GSHHG shoreline data to your view:

**Menu:** Geography → Plot Coastline…

Choose resolution: crude, low, intermediate, full, high.

## Volcanoes

Plot global volcano locations with hover metadata:

**Menu:** Geography → Plot Volcanoes

Hover over any point to see:
- Name
- Country
- Type
- Elevation
- Last eruption

## Tides

Download and plot tide gauge data from IOC:

**Menu:** Geography → Download Mareg

Options:
- **Last 2 Days** — Auto-fetch recent data
- **Calendar…** — Choose date range

Data downloads and plots automatically.

## Sun and terminators

Draw the day/night terminator and the twilight circles on the map, paint the night they enclose, and
read where the sun is:

**Menu:** Geography → Sun and terminators…

- **Date and time** — "Now", or any instant (UTC) plus an optional time-zone offset.
- **Terminators** — day/night (sun on the horizon), civil (−6°), nautical (−12°) and astronomical
  (−18°) twilight. Each ticked one is drawn as its own line layer, at the line width given here.
- **Paint / Colour / Transp. %** — per terminator, also fill the region beyond it, which is what
  `pssolar`'s `-G` paints. The colour and the transparency are the same two things the PostScript
  option takes; the result is a normal filled polygon in the window, so its fill, its opacity and its
  area are on its own properties afterwards.
- **Sun position** — the sun's longitude, latitude, azimuth and elevation. Give an observer position
  (type it, or click it on the map) to also get sunrise, noon, sunset and the day length there.
  "Mark the sub-solar point" puts a star where the sun is overhead.
- **Save terminators to** — optionally write the drawn polygons to a table.

The lines fold under one "Sun & terminators" row in Scene Objects and the painted regions under
"Sun & terminators — night", so either set can be hidden or removed in one click. Recomputing for
another instant REPLACES both: 10:00 and 11:00 are the same terminator at two instants, not two
layers.

## CRS

When you load georeferenced data (images/grids), the window automatically sets its coordinate reference system. The Geography menu unhides for geographic data.

**Supported:**
- EPSG codes
- Proj4 strings
- WKT

## Implementation Details

The geography tools use:
- `GMT.coast` for shorelines
- Built-in datasets for volcanoes/meteorites
- `GMT.maregrams` for tide data
- `GMT.solar` for the terminators and the night they enclose (asked for as data with `-M` and drawn
  by the window — never as PostScript, which would not be an object anyone could restyle) and for the
  sun report (`-I -C`)
