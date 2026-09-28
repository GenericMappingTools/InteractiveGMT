# GMT.jl issues found while porting RemoteS (2026-09-27)

Six defects in GMT.jl, found while porting RemoteS into InteractiveGMT (`src/RemoteS/`). Each one is
**independent**: its own cause, its own fix, its own test, its own commit. None depends on another.

All numbers below were measured with GMT.jl 1.43.2 (dev checkout), Julia 1.10, Windows.

| # | Where | Symptom | Status |
|---|---|---|---|
| 1 | `grid_init` (gmt_main.jl) | a `BR…` grid reaches GMT upside down | open |
| 2 | `grid_init` (gmt_main.jl) | a `TC…` grid reaches GMT upside down | open |
| 3 | `grid_init` / GMT C, by-reference path | a `TR…` grid is read correctly by `grdtrack` but upside down by `grd2xyz` | open |
| 4 | `grdinterp_local_opt_S` (grdinterpolate.jl) | Float32 cube + vector of point sets -> `convert` error; Float64 cube + 1-element vector -> `no field data` | open |
| 5 | `grdtrack` on a `GMTimage` | a UInt16 image cube cannot be sampled at all | open |
| 6 | `gmtread` of a GeoTIFF (Windows) | the file stays open (locked) after the read | open |

---

## Shared reproduction for issues 1–3

One 5×5 grid, `x = y = 1:5`, `z(x=1,y=1) = 1` (SW corner) … `z(x=5,y=5) = 25` (NE corner). The file is
read back in each of GMT's four memory layouts. Every read is the SAME grid, so every module must give
the same answer for all four.

```julia
using GMT
G = mat2grid(reshape(collect(1.0:25), 5, 5))
gmtwrite("lay.grd", G)
ref = grd2xyz(gmtread("lay.grd", layout="BCB")).data
for L in ("BCB", "TCB", "TRB", "BRB")
    g = gmtread("lay.grd", layout=L)
    println(L, ": grdtrack SW=", grdtrack(g, [1.0 1.0]).data[3], " NE=", grdtrack(g, [5.0 5.0]).data[3],
            "  grd2xyz==BCB: ", grd2xyz(g).data == ref)
end
```

Expected for every layout: `SW=1.0 NE=25.0  grd2xyz==BCB: true`.

Measured:

```
BCB: grdtrack SW=1.0 NE=25.0  grd2xyz==BCB: true
TCB: grdtrack SW=5.0 NE=21.0  grd2xyz==BCB: false     <- issue 2
TRB: grdtrack SW=1.0 NE=25.0  grd2xyz==BCB: false     <- issue 3
BRB: grdtrack SW=5.0          grd2xyz==BCB: false     <- issue 1 (NE not measured)
```

`gmtread` itself is right in every layout: the existing asserts in `test/test_B-GMTs.jl` (the
`Gr.z[1:5] == …` block after `gmtwrite("lixo.grd",GG)`) all pass. The defects are all on the way
**into** a module, in `grid_init`.

How `grid_init` hands a grid to GMT (gmt_main.jl, `function grid_init`):

- `layout[2] == 'R'` (row-major) -> `GMT_CONTAINER_ONLY`: no copy, `Gb.data` points at `Grid.z`, and
  `h.mem_layout` is set to the layout string.
- anything else -> `GMT_CONTAINER_AND_DATA`: the values are copied into GMT's own padded buffer by

  ```julia
  for col = 1:n_cols, row = n_rows:-1:1
      t[((row-1) + pad) * mx + col + off] = grd[k];  k += 1
  end
  ```

  i.e. the loop assumes `BC` (column-major, first row = south) whatever the layout says.

---

## Issue 1 — `BR…` grids reach GMT upside down

**Symptom.** `grdtrack(gmtread("lay.grd", layout="BRB"), [1.0 1.0])` returns `5.0` (the NW value, want
1); `grd2xyz` does not match `BCB`.

**Observed.** A `BR` grid is row-major, so `grid_init` hands it over by reference with
`h.mem_layout = "BRB"`. The module result is the one a `TR` grid over the same buffer would give: the
`B` has no effect. Where that is lost (GMT.jl or GMT's C side) is not established.

**Test to add** (`test/test_B-GMTs.jl`, right after the `layout="BRB"` read):

```julia
@test grd2xyz(Gr).data == grd2xyz(gmtread("lixo.grd", layout="BCB")).data
@test grdtrack(Gr, [1.0 1.0]).data[3] == 1.0 && grdtrack(Gr, [5.0 5.0]).data[3] == 25.0
```

**Who needs it.** InteractiveGMT.RemoteS `grid_at_sensor(…, NSIDC_N/NSIDC_S=true)`: the NSIDC rows come
off the file south-first, and the port says so in the layout (`TRB` -> `BRB`) instead of reversing the
buffer. Until this is resolved in GMT.jl, every GMT module sees that grid upside down (iGMT's own
viewer, which reads the layout itself, does not).

---

## Issue 2 — `TC…` grids reach GMT upside down

**Symptom.** `TCB`: `grdtrack` SW = `5.0`, NE = `21.0` (want 1, 25); `grd2xyz` ≠ `BCB`.

**Cause.** `TC` is column-major, so it goes through the copy loop, and the loop is hard-wired for `BC`
(`row = n_rows:-1:1` puts Julia row 1 at the bottom). For `TC`, Julia row 1 is the NORTH row.

**Suggested fix.** In the copy branch, index the source according to the layout. For node `(col, row)`,
`row` counted from the north in GMT's buffer, 0-based source offset within a band:

| layout | source index |
|---|---|
| `BC` | `(col-1)*ny + (ny - row)` |
| `TC` | `(col-1)*ny + (row - 1)` |
| `TR` | `(row - 1)*nx + (col - 1)` |

`TC` only needs the row order of the existing loop reversed (`row = 1:n_rows`).

**Test.** Same block, after the `layout="TCB"` read: the two asserts of issue 1 with `layout="TCB"`.

---

## Issue 3 — zero-copy `TR…` grids: `grdtrack` right, `grd2xyz` upside down

**Symptom.** `TRB`: `grdtrack` SW = 1, NE = 25 (correct) but `grd2xyz` ≠ `BCB`. First two rows:

```
BCB grd2xyz: [1.0 5.0 5.0; 2.0 5.0 10.0]     (x=1, y=5, z=5)  correct
TRB grd2xyz: [1.0 1.0 5.0; 2.0 1.0 10.0]     (x=1, y=1, z=5)  wrong y
```

**Cause (partly known).** `TR` goes by reference (`GMT_CONTAINER_ONLY` + `h.mem_layout = "TRB"`). Two
modules given the same referenced grid disagree, so the by-reference row-major handover is honoured by
some GMT modules and not by others (grdtrack yes, grd2xyz no). Which part of GMT's C side does
it — how `grd2xyz` walks a grid it did not allocate — still has to be found.

**Options.** (a) find and fix the module-side handling in GMT (C), keeping zero-copy; or (b) send `TR`
through the copy loop too (issue 2's table), which is correct for every module but gives up the zero-copy
handover for the layout InteractiveGMT reads all its grids in (`TRB`). This is a design choice for GMT.jl.

**Test.** Same block, after the `layout="TRB"` read: the two asserts of issue 1 with `layout="TRB"`.
Today the `grdtrack` assert passes and the `grd2xyz` one fails.

---

## Issue 4 — `grdinterpolate(cube, S=Vector{GMTdataset}, nocoords=true)`

`grdinterp_local_opt_S(arg1::GItype, pts::Vector{<:GMTdataset}, …)` (grdinterpolate.jl). Used by
RemoteS `train_raster` (sampling a band cube inside training polygons).

```julia
using GMT
C32 = mat2grid(rand(Float32, 20, 20, 3));  C64 = mat2grid(rand(Float64, 20, 20, 3))
d1 = mat2ds([5.0 5; 8 9; 12 14]);  d2 = mat2ds([3.0 4; 6 7]);  d3 = mat2ds([15.0 15; 16 11])
grdinterpolate(C32, S=[d1, d2, d3], nocoords=true)
```

Measured:

| cube | `S=GMTdataset` | `S=[ds]` (1 element) | `S=[ds,ds,ds]` |
|---|---|---|---|
| Float32 grid | OK | `Cannot convert GMTdataset{Float32,2} to GMTdataset{Float64,2}` | same error |
| Float64 grid | OK | `type Float64 has no field data` | OK |

Two separate causes in the same function:

- **4a — output vector typed Float64.** `D = Vector{GMTdataset{Float64,2}}(undef, length(pts))` (line
  ~197), but each element is built as `mat2ds(Matrix{DT}(…))` with `DT = eltype(arg1)` when
  `no_coords` — Float32 for a Float32 cube. Fix: `Vector{GMTdataset{DT,2}}`, with `DT` computed first.
- **4b — one point set comes back as a scalar dataset.** `t = grdtrack(slicecube(arg1, k), pts, o=2)`
  (line ~206) returns a single `GMTdataset` when `pts` has one element, so `t[n]` is a `Float64` and
  `t[n].data` fails. Fix: normalise, e.g. `t isa GMTdataset && (t = [t])`.

**Tests.** The table above, all six cells returning a `Vector{GMTdataset}` (or `GMTdataset` for the
first column), with the sampled values checked against `grdtrack` of each layer.

---

## Issue 5 — a UInt16 image cube cannot be sampled

```julia
C16 = mat2img(rand(UInt16, 20, 20, 3), noconv=1)
grdinterpolate(C16, S=mat2ds([5.0 5; 8 9]), nocoords=true)
# ERROR: conversion to pointer not defined for GMTimage{UInt16, 2}
```

Fails for every form of `S`. Stack: `grdinterp_local_opt_S` -> `grdtrack(slicecube(C16, k), …)` ->
`common_grd` -> `gmt(…, ::GMTimage{UInt16,2})` -> `GMTJL_Set_Object` -> `dataset_init(API, ::GMTimage, …)`
-> `pointer(x)`. i.e. `grdtrack` is handed a `GMTimage` and `gmt()` routes it to the DATASET initialiser.

This is the common case for RemoteS: a Landsat/Sentinel cube read with `gmtread` is a
`GMTimage{UInt16,3}`, so `train_raster(cube, polygons)` / `classify(cube, polygons)` fail on it (the
original RemoteS.jl makes the same call and fails the same way). Today the only workaround is converting
the whole cube to a Float64 `GMTgrid` first.

**Suggested fix.** In `grdinterp_local_opt_S` (both methods), sample a `GMTimage` layer as a grid —
`mat2grid` of the layer, or a direct index lookup, since the points only need the node values — or teach
`grdtrack` to accept a `GMTimage`. Either way the result keeps the image's integer values.

**Test.** `grdinterpolate(C16, S=…, nocoords=true)` returns the UInt16 values at the points (compare
against `C16.image[i, j, k]` for points on nodes).

---

## Issue 6 — `gmtread` of a GeoTIFF leaves the file open (Windows)

```julia
using GMT
f = joinpath(mktempdir(), "cube.tif")
gdaltranslate(mat2img(rand(UInt16, 60, 50, 7), noconv=1), dest=f)
g = joinpath(dirname(f), "copy.tif")
cp(f, g); gmtread(g);  GC.gc(); rm(g)   # ERROR: IOError: unlink(...): resource busy or locked (EBUSY)
cp(f, g); gdalread(g); GC.gc(); rm(g)   # fine
```

Measured, each read on a fresh copy of the same file, then `GC.gc()` and `rm`:

| call | file after the call |
|---|---|
| `gmtread(f)` | **locked** |
| `gmtread(f, band=[4,5], layout="TRBa")` | **locked** |
| `gdalread(f)` | released |
| `grdinfo(f)` | released |
| `gmtread` of a `.grd` (netCDF) | released |

So the lock is specific to `gmtread`'s path for GDAL-read rasters: a dataset handle is not closed.
The same family as the known `gdalinfo(path)` leak. Consequences seen from InteractiveGMT: a band cube
read by RemoteS (`subcube`, `truecolor`, `classify` all go through `gmtread`) cannot be deleted or
overwritten while the session is running — e.g. re-running Band cube (cut) onto the same output
file, or a test cleaning up its temporary cube.

Where to look: the image branch of `gmtread` (gmtreadwrite.jl) and whatever it hands to GDAL — the
open dataset must be `GDALClose`d on every path, including errors.

**Test.** The snippet above: after `gmtread`, `rm` must succeed.

Side observation from the same probe, not investigated: `gmtread(f, band=1)` (a scalar band) fails with
"Option ->: Given more than once … GMT error number = 72", while `band=[4,5]` works.