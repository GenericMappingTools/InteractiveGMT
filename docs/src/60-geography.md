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
