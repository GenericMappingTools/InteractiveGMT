# Sentinel Hub

*Geophysics → Copernicus → Sentinel Hub imagery…*

Sentinel-1, Sentinel-2 and the Copernicus DEM as pictures and as grids, fetched for a region you
name and dropped straight into the window. It is a port of the QGIS
[SentinelHub plugin](https://plugins.qgis.org/plugins/SentinelHub/) onto iGMT's own machinery, with
one difference that runs through everything below: QGIS adds the service as a *live* WMS layer that
re-fetches as you pan, and iGMT has no such layer. Every button here therefore fetches **one image,
once**, which lands as a file and is opened like any other raster — with its own axes, its own Z
range and its own colour bar.

## Before anything: the Copernicus paperwork

Getting credentials out of the Copernicus Data Space is the hardest part of this tool, and none of
that difficulty is iGMT's. Set aside a quiet quarter of an hour. What you need at the end is **two
strings**: a *client id* and a *client secret*.

1. **Register** at [dataspace.copernicus.eu](https://dataspace.copernicus.eu). The account is free.
   The confirmation mail can take a few minutes.
2. **Create an OAuth client.** The dashboard moves around and the documentation does not always
   point at where it currently is; the durable link is the one the dialog carries, under *Create
   OAuth client*:
   [documentation.dataspace.copernicus.eu → Authentication → registering an OAuth client](https://documentation.dataspace.copernicus.eu/APIs/SentinelHub/Overview/Authentication.html#registering-oauth-client).
3. **Copy the secret immediately.** It is shown exactly once, when the client is created. If you
   close that page without copying it, the secret cannot be retrieved — you delete the client and
   make another one.

That is all. In particular you do **not** need to create a "configuration instance", which is what
the QGIS plugin requires and what makes a fresh account look broken there: this tool ships its own
sources (below) and asks the Process API, which needs nothing but the OAuth client.

### Where the credentials live

Type the two strings into the **Login** tab and tick **Remember them in `~/.sentinelhub`**. The file
is plain text, two named lines, and you can write it by hand instead:

```
# Sentinel Hub / Copernicus Data Space OAuth client, used by InteractiveGMT
client_id sh-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
client_secret xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

Once it exists the two boxes open **greyed with what is on disk** — the client id, and a reminder
that the secret is there too — and can be left empty. Anything you do type overrides the file for
that session; the file is never a second place the credentials silently come from.

**Login** asks the service for a token and reports, in the panel under the boxes, which step
succeeded or failed: the file not being there, the client being rejected, or the token coming back.
A successful login fills the **Configuration** and **Layer** combos on the Image tab. Tokens are
rate limited by Copernicus, so one is kept for the session and refreshed a minute before it expires
— pressing Login repeatedly is the one thing that will get you throttled.

The **Service URL** combo picks the deployment. `https://sh.dataspace.copernicus.eu` is the
Copernicus Data Space (the free one, and the default); `https://services.sentinel-hub.com` is a
commercial Sentinel Hub account, if you have one.

## Sources: built-in collections, and your own configurations

The **Configuration** combo lists two different kinds of thing, built-in first:

| Entry | What it is | How it is asked |
|---|---|---|
| *Sentinel-2 L2A (built in)* | Surface reflectance, the everyday optical collection | Process API, with an evalscript this tool carries |
| *Sentinel-2 L1C (built in)* | Top-of-atmosphere reflectance | Process API |
| *Sentinel-1 GRD (built in)* | C-band radar backscatter, cloud-blind | Process API |
| *Copernicus DEM (built in)* | The elevation model | Process API |
| anything else | A **configuration instance** you made in the Copernicus dashboard | The OGC endpoints, exactly as the QGIS plugin asks them |

The **Layer** combo then means whichever of the two the source is: for a built-in collection it
lists the *renderings* below; for a configuration of your own it lists that configuration's layers,
read from your account.

| Collection | Rendering | Comes back as |
|---|---|---|
| Sentinel-2 L1C/L2A | True colour (B04 B03 B02) | an RGB image |
| | False colour (B08 B04 B03) | an RGB image |
| | SWIR (B12 B8A B04) | an RGB image |
| | NDVI | a **grid** (32-bit float) |
| Sentinel-1 GRD | VV backscatter | a **grid** |
| | VV / VH false colour | an RGB image |
| Copernicus DEM | Height | a **grid** |

The ones marked *grid* arrive as real numbers, not as a picture of numbers: they get a Z axis in
their own units, a colour bar, and everything the Grid Tools menu can do to a grid. Sentinel-3 and
Sentinel-5P are deliberately **not** in the built-in list — their band names would have to be
guessed, and a wrong guess is a silent wrong answer. Reach them through a configuration of your own,
which is precisely what configurations are for.

## The Image tab

What to ask for, and for where.

**Cloud coverage** is a ceiling, not a target: 20 % means "ignore scenes cloudier than this". It has
no meaning for Sentinel-1 or the DEM and is ignored there.

**Image priority** decides which scene wins where several qualify — *most recent*, *least recent*, or
*least cloud coverage*. With a wide time range, *least cloud coverage* is usually what you want.

**Time range** takes two dates, `yyyy-mm-dd`. The calendar below fills whichever end is still open:
the first click sets *From*, the second *To*, and a click with both set starts a new range. With
**Exact date** ticked there is one date and the calendar always fills it. Leave both empty and the
service is free to use its whole archive.

**Region** is W/E/S/N in degrees, and the dialog opens with the limits of whatever the window is
already showing. Otherwise: type them, press **From window**, or pick a grid in **OR Ref grid** —
the same row every other Region group in iGMT has. An empty or backwards region is refused by the
dialog, naming the box, before anything is sent.

**Image size** is the pixel count of the *longer* side; the other follows the region's aspect, so
nothing comes back stretched. The service caps a single request at 2500.

**CRS** is the coordinate system the image is requested in. The region boxes are always degrees — a
projected CRS is reached through GDAL, not by rounding a formula in here.

**Get image** fetches it, and the result lands in this window with its own axes. The file is named
after the request (source, layer, dates, region), so asking twice for the same thing finds the first
answer still on disk and does not go back to the server.

## The Download tab

The same request, sized in **metres on the ground** instead of pixels, and written to a folder you
keep.

* **Bounding box** — the window's current extent, or one you type. The window's own extent is the
  default, so the usual gesture is: frame what you want on screen, come here, press Download.
* **Resolution** X and Y, in metres. 10 m is Sentinel-2's native optical resolution; asking for
  finer does not create detail that is not there.
* **Image format** — TIFF, PNG or JPEG. TIFF is a GeoTIFF and is what you want for anything you will
  compute on. PNG and JPEG are pictures with no coordinates of their own, so iGMT stamps the corners
  and the CRS into a GeoTIFF copy before handing it over — you get both files.
* **Folder** — where the image is kept. Left empty, it goes to a temporary folder.
* **Show logo** adds the service's attribution strip to the image, which their terms ask for in
  published figures.
* **Load the downloaded image into the window** — untick it to fetch a pile of images without
  displaying each.

## When something fails

The message says what the *service* said, because that is where the answer is:

* **"the service answered 401"** — the token was refused. Press Login again and read the panel.
* **"the Process API answered 400"** followed by JSON — the request was malformed for that
  collection: usually a time range with no data in it, or a region in the wrong CRS.
* **An empty image** — the region and the dates are legal, but no scene matches. Widen the time
  range, or raise the cloud ceiling.
* **429** — you are being rate limited. Wait, and stop pressing Login.

Failures go to the window's **Messages** log, which opens itself, so the text stays there to read
next to whatever else was printed on the way up.

## Where the work happens

The dialog is `deps/ui/sentinelhub_dialog.ui`, loaded at runtime, and its C++ side is
`SentinelHubDialog` in `deps/src/70_window.cpp`. Everything that touches the network — the token,
the catalogues, the evalscripts, the Process API and the OGC URLs — is `src/sentinelhub.jl`. The
image that comes back goes through the same file door a drag-and-drop uses, which is why it behaves
like every other raster in the application.
