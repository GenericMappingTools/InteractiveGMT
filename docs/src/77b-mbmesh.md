# 3-D mesh (mbmesh)

*Geophysics → MB-System → 3-D mesh (mbmesh)…*

This dialog runs MB-System's **mbmesh** on point data and shows the mesh it makes in the window.
mbmesh fits a surface to the soundings (screened Poisson reconstruction), extracts it as triangles
and trims what the soundings do not support. It writes the mesh as a glTF binary, `mesh.glb`, and,
if asked, an OGC 3D Tiles tileset and viewer pages for a web browser.

The entry is in the MB-System menu when the GMT in use has MB-System's modules: the
[MB-System plugin](77-mbplugin.md) installed, or a Windows GMT that brings them itself.

## The dialog

### Input

**Points** is the point data to mesh. It can be:

- an MB-System swath file (`.mbXX`, `XX` its MBIO format number) or datalist (`.mb-1`). mbmesh reads
  it directly;
- a LAS or LAZ file;
- an x,y,z table (`.xyz`, `.txt`, `.dat`, `.csv`, …). The third column is a height, positive up, as
  the window shows it.

When the window shows a point file, the box starts with it. Double-click the box, or press **…**, to
choose another.

mbmesh reads MB-System datalists only. A LAS file or a table is therefore read by iGMT, taken to
longitude and latitude, and written beside the outputs for MBIO's generic "lon lat topography"
format (number 168): `mbmesh_points.xyt`, with the one-line datalist `mbmesh_input.mb-1` that names
it. Rows without a z are left out.

**Save in** is the folder mbmesh writes to. It is created when missing. Empty, or when a new input is
chosen, it is the input's folder plus `<name>_mbmesh`.

**CRS** is the coordinate system of the points, needed when they are not in longitude and latitude
and the file does not say what they are in: an EPSG code (`EPSG:3763`) or a PROJ string. LAS and LAZ
files are read **without** their coordinate system, so a projected LAS file needs it here. Empty means
the system the file declares, or longitude and latitude when the coordinates are in their range.
MB-System data is always in longitude and latitude and ignores the box.

### Mesh

**Detail** is the smallest feature, in metres, the mesh should keep (`--level-of-detail`). Empty lets
mbmesh choose it from the spacing of the soundings, which is right for most data.

**Decimate** averages the soundings in cells of that size, in metres, before the reconstruction
(`--decimate`): fewer points, a faster run and smaller files, at the cost of detail finer than the
cell. Empty uses every sounding.

**GeoOrigin** stores the vertices relative to a point at the centre of the data (`--geoorigin`). It is
on by default: without it the vertices are absolute Earth-centred coordinates in 32-bit floats, which
resolve only about half a metre.

### Also write

`mesh.glb` is always written. These add to it, in **Save in**:

| Box | mbmesh option | Writes |
|-----|---------------|--------|
| 3D Tiles tileset | `--tileset` | `tileset/tileset.json` and its tiles, for viewers that stream them |
| X3DOM page | `--html` | an X3DOM page of the outputs |
| Cesium page | `--cesium-html` | a CesiumJS page, the mesh on the globe |
| Track (swath data) | `--track-glb` | the sensor track as a 3-D line, `track.glb`; only swath data has one |

The pages load their files over HTTP, as browsers require for local files: mbmesh writes in its log
the command that serves the folder and the address to open.

### Make the mesh

**Make the mesh** runs mbmesh with these settings. The busy notice stays up until it ends: a large
survey can take minutes.

The mesh then appears in the window as a new element of Scene Objects, named after the input
(`<name> mesh`; a second run adds `<name> mesh (2)`), coloured by height. Into an empty window it
becomes the window's content; next to a grid or a point cloud it is added beside them and becomes the
layer shown, with its own axes.

`mesh.glb` is in Earth-centred coordinates and compressed with Draco. MB-System itself reads it back
(the reader of its `mbglb2tiles`) and hands the mesh over in longitude, latitude and height, which is
how the window can show it in the same frame as the rest of its data. A plugin older than that reader
is told so, and asked to be updated (**Geophysics ▸ MB-System ▸ Update the plugin**).

The status line reports the number of triangles, how many points went in, and the folder of the
outputs.
