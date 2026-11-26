# Sentinel-2 MSI products

*Satellite → Copernicus → Sentinel-2 MSI products…*

The Sentinel-2 optical scenes (Level-2A surface reflectance or Level-1C top-of-atmosphere
reflectance) found in the Copernicus Data Space catalogue over a region, a period and a cloud limit.
Each scene comes with its quicklook. You can download the whole product, or only the bands you tick,
cut to your region, as one band file shown in the window.

This tool shares its account, catalogue and download with [Sentinel-1 SAR products](73b-sentinel1.md).
It is different from [Sentinel Hub](73-sentinelhub.md), which gives you a picture of a mosaic, not
the bands of one scene.

## The account

Searching and the quicklooks need no account. Downloading needs a free
[Copernicus Data Space](https://dataspace.copernicus.eu) account: the **e-mail and password** of the
web site, typed in the **Account** tab. **Save login** writes them to the
`machine identity.dataspace.copernicus.eu` entry of `~/.netrc` (the same entry Sentinel-1 uses). An
account with two-factor login also needs the code of the moment in **2FA code**.

## Search

* **Region**: W/E/S/N in degrees. A window that already shows something fills it on opening;
  **Use the window's limits** does it again.
* **Product**: **L2A** (surface reflectance, atmosphere removed) or **L1C** (top of atmosphere).
* **From / To**: the period, both days included (UTC).
* **Clouds**: the largest cloud cover accepted, in percent of the whole 110 km tile (ESA's own
  figure, not computed over your region). 100 keeps every scene.

**Find the scenes** lists the scenes newest first and draws each scene's own footprint (two passes
over one tile can cover different parts of it). In an empty window the footprints are drawn over a
coastline map. The chosen scene's footprint is drawn in red, and its quicklook and band list appear
on the right.

To choose a scene, pick it in the **Scene** list, or click inside its footprint in the window.
Clicking the same place again steps to the next scene that covers it. A footprint's own menu item
*Choose this scene (Sentinel-2)* does the same.

Clicks in the window choose scenes only while you are searching. Once **Download the bands and show
the cube** has put data in the window, a click is an ordinary click again: it no longer chooses a
scene. The next **Find the scenes** turns scene picking back on.

The list always belongs to the region it was found for. If you change the **Region** after a search,
the list is emptied and the status line says *Region changed: Find the scenes again*, so a scene that
does not reach the new region can never stay chosen. Changing the **Product** does the same.

## Quicklooks 3-D

**Quicklooks 3-D** lays every scene's quicklook flat on the tile it covers, one level per pass
(tiles of the same acquisition side by side), oldest at the bottom, in a 3-D view seen from the
south. A level slider at the map's corner hides the levels above its knob. Click a quicklook to
choose its scene; the chosen one gets a thin red outline.

Press **Quicklooks 3-D** again to remove the stack: the window goes back to the view it had before
(normally the 2-D map). The stack is also removed when **Download the bands and show the cube** puts
the bands in the window, so the data never opens under floating quicklooks.

While the quicklooks are fetched and laid, a progress dialog says which one it is on.

## Download

**Download the whole product (.zip)** fetches the chosen scene into **Folder** as the zip ESA
distributes. A scene already in the folder is marked *(on disk)* and is not downloaded again.

## Bands

**Resolution** (10, 20 or 60 m) and the band boxes choose what to get. A box is enabled only for a
band the product really has at that spacing; the line beside the spacing says how many pixels the
ticked bands make. **TCI** is ESA's 8-bit true-colour picture: it comes as an image of its own, never
as a band of the cube.

**Cut to the region** fetches only the part of the region that the chosen scene covers: the region
is cut to the scene's tile and to its data footprint. Unticked, it fetches the whole 110 km tile.

**Download the bands and show the cube** gets the ticked bands from Copernicus's own service, which
cuts the window on the server and sends only it. Nothing else is downloaded. The service answers at
most 2500 x 2500 pixels per request, so a larger window comes in *blocks*. Four blocks are asked for
at a time, and the progress dialog counts them: *n of N block(s) received*. Copernicus computes each
block before it starts sending it (about 9 s with no network traffic, then the transfer). A block
that gets no answer, or whose transfer stops, is asked for again (up to three tries); if it still
fails, the error names the block. Closing the window stops the download.

The bands are written as ONE GeoTIFF in a folder named after the product, inside **Folder**:
`<tile>_<date>_L2A_<res>m_<bands>_cut_<window>.tiff`. Asking again for the same bands over the same
window reads it from there.

## The bands in the window

The file opens like any multi-band file: band 1 is shown, as an image of its own. Below it in Scene
Objects, the group lists:

* a **file** row: the `.tiff`'s name (hover it for the full path). **Load file** opens it again.
* one **band** row per band. These bands are **not in memory**: they are in that file. Hover a band
  row and it says so, with the file's name and folder. **Load band** reads the band from the file
  and adds it to the window as a layer of its own.

*Image → Load Bands* makes colour compositions from the same file. A composition is added beside the
bands; it never replaces the band that was opened from the file.

## Where the work happens

The dialog is `deps/ui/remotes_s2.ui`, run by the generic Remote sensing dialog (`RemoteSDialog`).
The search, quicklooks, footprints, quicklook stack and band download are in `src/s2products.jl`;
the account and the whole-product download are shared with `src/s1products.jl`.
