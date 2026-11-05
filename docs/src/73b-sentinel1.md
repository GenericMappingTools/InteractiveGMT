# Sentinel-1 SAR products

*Satellite → Copernicus → Sentinel-1 SAR products…*

The Sentinel-1 radar **product files**, the `.SAFE` archives ESA distributes, found in the
Copernicus Data Space catalogue over a region and a period, then downloaded to a folder you keep.
A GRD can also be shown in the window as a longitude/latitude grid.

This tool is different from [Sentinel Hub](73-sentinelhub.md). Sentinel Hub gives you a *picture*
of Sentinel-1 backscatter, resampled to your region by the service. This tool gives you the *file
itself*: every polarisation, full 10 m resolution, the calibration tables, and the phase for an SLC.
The two also log in differently, as explained below.

## The account

Searching needs no account. Downloading needs a free
[Copernicus Data Space](https://dataspace.copernicus.eu) account: the **e-mail and password** you
log in to the web site with. This is not the OAuth client id and secret the Sentinel Hub dialog
uses. Type them in the **Account** tab. **Save login** writes them to the
`machine identity.dataspace.copernicus.eu` entry of `~/.netrc`, next to the Earthdata login the
SWOT and MODIS tools keep there. If you don't save them, the boxes are used for the session only.

If your account has two-factor login, type the code your authenticator shows into **2FA code**
just before the first download. One code opens a login that lasts the session.

## Search

* **Region** is W/E/S/N in degrees. A window that already shows something fills it on opening;
  **Use the window's limits** does it again.
* **Product**:
    * **GRD**: backscatter on a ground-range grid, about 1.7 GB per IW scene. This is the only
      product the tool can show.
    * **GRD COG**: the same data as cloud-optimised GeoTIFFs, about half the size.
    * **SLC**: the complex signal, for interferometry (4–8 GB).
    * **OCN**: ocean wind, waves and radial currents.
    * **RAW**: the Level-0 signal.
* **Mode**: IW over land and coastal seas, EW over polar seas, SM for small targets, WV for open-ocean
  wave vignettes.
* **Pass** and **Track** (the relative orbit, 1–175) narrow the search to one viewing geometry. For
  interferometry or change detection, compare scenes of the same pass and track.
* **Find the scenes** lists the scenes newest first and draws their footprints. There is one
  footprint per *frame*, the same piece of the same track, because a frame comes back every 6 or 12
  days. In an empty window the footprints are drawn over a coastline map. **Pick a frame**, then a
  click on a footprint, chooses that frame's newest scene in the list. The footprint's own menu item
  *Choose this scene (Sentinel-1)* does the same.

The catalogue returns at most 1000 scenes per search. If more match, the status line says so;
shorten the period to see them.

## Download

**Download** fetches the chosen scene into **Folder** as the zip ESA distributes, with the `.SAFE`
folder inside it. A scene already in the folder is marked *(on disk)* and is not downloaded again.
A progress bar shows the megabytes received.

## Show a GRD

**Download and show** downloads the scene if needed. It then reads the chosen **polarisation**
straight out of the zip with GDAL's SAFE driver and resamples it to longitude/latitude, following
the product's own geolocation points (thin-plate spline). The 10 m pixels are averaged into the
**Spacing** you ask for, 0.001° (about 100 m) by default. **Cut to the region** keeps only the
part inside the region boxes.

**Values**:

* **sigma0, dB**: the backscatter coefficient, calibrated with the product's own tables, in
  decibels.
* **Raw amplitude (DN)**: the digital numbers as stored, uncalibrated.

The grid arrives like any other grid, with its own axes, Z range and colour bar. Its name gives
the satellite, date, track and pass.

## Where the work happens

The dialog is `deps/ui/remotes_s1.ui`, run by the generic Remote sensing dialog (`RemoteSDialog`).
The search, login, download and resampling are in `src/s1products.jl`.
