# mbgrid — MB-System's gridder

`mbgrid` grids scattered `x, y, z` in two stages: a **Gaussian-weighted mean into the grid cells**,
then a **spline through the cells that stayed empty**. No GMT module has that shape, and it is why
mbgrid is the tool for multibeam bathymetry — the along-track sounding density is enormous, so what
you want is the soundings *averaged* per cell and the *between-swath gaps* interpolated, not a
spline fighting ten thousand near-coincident points for one node.

It is not a GMT module. Mirone offered it (Surface window, "Minimum Curvature - mbgrid") through the
`mbgmt` supplement, which had to be built and installed separately. Here the numerics are C inside
the viewer DLL, so it is available wherever `gmtvtk` loads.

| | |
|---|---|
| Julia | `src/mbgrid.jl` (316 lines) — the `mbgrid` function, exported |
| C | `deps/src/mbgrid.c` (745) + `deps/src/mbgrid.h` (188) |
| C tests | `deps/src/test_mbgrid.c` (580) → `deps/build/test_mbgrid.exe`, 69 checks, standalone |
| Julia tests | `test/test-mbgrid-unit.jl` (153), 75 checks |
| Dialog | GMT menu ▸ **Interpolate**, Griding Method ▸ *Minimum Curvature - mbgrid* |

---

## Using it

### From Julia

```julia
using InteractiveGMT, GMT

D = gmtread("soundings.xyz")
G = mbgrid(D; region = (-9.5, -8.5, 36.5, 37.5), inc = 0.002,
              scale = 1.5, clipmode = :near, clip = 5)
view_grid(G)
```

`data` may be a `GMTdataset`, a `Vector` of them, an N×3 `Matrix`, or three vectors passed
positionally: `mbgrid(x, y, z; ...)`.

**Required**

| kwarg | forms |
|---|---|
| `region` | `(w,e,s,n)`, a 4-element vector, or `"w/e/s/n"` |
| `inc` | one number, `(dx,dy)`, or `"dx/dy"` |

**Options**

| kwarg | default | gmtmbgrid | meaning |
|---|---|---|---|
| `scale` | `1.0` | `-W` | Gaussian half-width, in **grid cells**. Larger = more soundings averaged into each node = smoother. |
| `clipmode` | `:all` | `-C` | Which empty nodes the spline may fill. `:all` everything it reached; `:near` only within `clip` cells of data; `:gap` only gaps with data on **opposite** sides; `:none` bin only, gaps stay `NaN`. |
| `clip` | `0` | `-C<n>` | The search radius in cells that `:near` and `:gap` use. |
| `tension` | `0.0` | `-T` | Spline tension, 0 = minimum curvature. `solver = :zgrid` only. |
| `extend` | `0.0` | `-E` | Widen the working grid by this fraction of nx/ny so data just outside `region` still constrain the edge, then crop back. |
| `breakline` | `nothing` | `-D` | `GMTdataset`, vector of them, N×3 matrix, or a file path. Nodes the line crosses are pinned to **its** z, undiluted by nearby soundings. |
| `solver` | `:zgrid` | — | `:zgrid` the IGPP/SIO thin-plate spline in C; `:surface` hands the binned nodes to `GMT.surface`. |
| `registration` | `:gridline` | — | Or `:pixel`. |
| `verbose` | `false` | — | Spline iteration progress on stderr. |

Every option also accepts the **string** form the dialog sends (`scale = "1.5"`, `clipmode = "near"`).

Returns a `GMTgrid`, `NaN` at every node nothing reached. `G.command` carries the full recipe,
`G.remark` the node counts (`"mbgrid: 11078 nodes from data, 5439 by zgrid, 0 empty (19967 points)"`).

### Choosing a solver

- **`:zgrid`** — what MBGRID itself uses. Fast, in C, no GMT round-trip. It extrapolates large empty
  areas freely, so far from any data it can invent dramatic relief. Bound it with `:near` or `:gap`.
- **`:surface`** — GMT's own solver, tension and all. Slower (a table out, a grid back), but it is
  the same spline the rest of the Interpolate dialog offers.

Both go through the **same** binning and the **same** `-C` merge; only the solver step differs.

### From the dialog

GMT menu ▸ **Interpolate**. Pick *Minimum Curvature - mbgrid* in the Griding Method combo (second
entry, under `surface`), fill the input file and the Griding Line Geometry block as for any other
method, press **Options...** for the seven knobs above, then **Compute**.

The result is delivered exactly like every other gridded product (`_gm3d_deliver`): a new named
handle in Scene Objects, checked, with the axes reframed to its own extent and the source unchecked.

Nothing about the dialog is special-cased for mbgrid: `_interp_module("mbgrid")` returns this same
function, called with the same `region`/`inc` keywords as the GMT modules beside it. The one
difference is that `-fg` is not sent — mbgrid measures its Gaussian in **cells**, so there is no
distance unit to resolve.

!!! note "No manual page"
    The dialog's ? button builds a GMT.jl documentation URL from the method name. mbgrid is not a
    GMT module, so that page does not exist and the button 404s.

---

## The algorithm, step by step

The C API is four stages plus a one-shot wrapper. Every stage is **stateless** and every buffer is
**caller-allocated** — nothing crosses the DLL boundary that the caller did not allocate and does
not free, so there is no `mbgrid_free` to forget.

```
mbgrid_bin  →  mbgrid_zgrid  →  mbgrid_fill  →  mbgrid_extract
                    ↑
              (or: mbgrid_nodes → GMT.surface → back in as MBGRID_SG_COLMAJOR)

mbgrid_run = all four, zgrid solver
```

### 1. `mbgrid_bin` — Gaussian-weighted binning

Each point is spread over a `(2·xtradim+1)²` neighbourhood, `xtradim = floor(scale) + 2`, with
weight `exp(-r²·factor)`, `factor = 4/(scale²·dx·dy)` — so the weight has fallen to `exp(-4) ≈ 1.8%`
one `scale` cell away, as in MBGRID. A node ends up with the weighted mean of everything that
reached it, **but only if a point fell in that node's own cell**; nodes that merely caught leaked
weight stay `NaN`. That "own cell" rule is what makes the output a binning rather than a smear.

Breaklines are densified to about one point per cell and binned with a `pin` flag: a breakline point
owns its own cell outright, overwriting whatever else reached it.

### 2a. `mbgrid_zgrid` — the IGPP/SIO thin-plate spline

Translated from the f2c'd Fortran in `gmtmbgrid.c`, with the `goto` lattice unwound.

1. **Seed.** Every unset node takes a value from an already-known neighbour, one ring per sweep, up
   to `nrng` sweeps (`nrng` = the effective clip). Nodes the sweep never reaches are marked `1e35`
   and are ignored downstream — they are holes, not values.
2. **Relax.** Point over-relaxation (Carré's method) on the Laplace/spline equation, 100 sweeps max,
   with the over-relaxation factor retuned at iterations 20, 40 and 60 and an early exit on
   convergence. Data nodes are stored **negated** (and biased by `zbase` so the negation is
   unambiguous); that sign is how the sweep knows not to move them.
3. **Unbias.** Undo the sign and the bias.

### 2b. `mbgrid_nodes` — the `GMT.surface` alternative

Hands back the set nodes as plain `x, y, z` triplets over the **working** region (the `-E` margin
included — the merge would read past the end of the solution otherwise). The solved grid comes back
in through `mbgrid_fill` as `MBGRID_SG_COLMAJOR`.

### 3. `mbgrid_fill` — the `-C` clip modes

Decides which of the spline's answers are allowed into the output.

- **`:all`** — every node the spline reached.
- **`:near`** — a node is filled if any data node lies within `clip` rings of it.
- **`:gap`** — a node is filled only when data has been seen in two **opposite** directions
  (a 3×3 direction mask; the four opposite pairs are tested). This is what stops the spline from
  extrapolating off the edge of the survey.
- **`:none`** — nothing.

`:near` and `:gap` build a mask over the whole grid *first* and only then write, so a node filled
early can never act as "data" for a later node.

### 4. `mbgrid_extract` — crop and emit

Drops the `-E` margins, converts to the requested output layout, and fills `mbgrid_stats`
(`zmin`, `zmax`, `n_data`, `n_set`, `n_spline`, `n_unset`). A spline node the solver never reached
is written as `NaN`, not as `1e35`.

---

## Memory layouts

Nothing here transposes anything silently; every buffer states what it is. This is
`SACRED_LAW.md`'s grid memory-layout law applied to a second gridder.

| buffer | layout |
|---|---|
| `work[]` | always column-major over the working grid, `work[ix*gydim + iy]`, y ascending |
| `sgrid[]` | `MBGRID_SG_SOUTHUP` row-major x-fastest, row 0 = south (what `mbgrid_zgrid` writes) · `MBGRID_SG_NORTHUP` row-major, row 0 = north · `MBGRID_SG_COLMAJOR` column-major, y ascending — a GMT/GMT.jl `"BCB"` grid, handed over **untransposed** |
| `out[]` | `MBGRID_LAY_BCB` `out[ix*ny + iy]`, y ascending (GMT's own order) · `MBGRID_LAY_TRB` `out[(ny-1-iy)*nx + ix]`, row 0 = north |

`MBGRID_SG_COLMAJOR` exists precisely so the `:surface` path does **not** `permutedims` GMT's
answer: a `(gy, gx)` column-major `"BCB"` matrix *is* that layout. `src/mbgrid.jl` checks
`Gs.layout` rather than assuming it — the whole point of carrying a layout code is that a buffer
says what it is instead of a caller believing what it usually is.

The output side: `Matrix{Float32}(ny, nx)` column-major in Julia **is** `MBGRID_LAY_BCB`, so
`mat2grid` receives the buffer the C wrote, with no copy and no transpose anywhere in the path.

### Coordinate vectors

The C works in **node centres** — for pixel registration the SW-most node sits half a cell inside
the region (`mbgrid_work_origin`). GMT writes a grid down differently: `mat2grid` wants
`length(x) == nx + reg`, i.e. `nx` centres for gridline and `nx+1` **cell boundaries** for pixel,
both starting at the region's west/south edge. `src/mbgrid.jl` emits `range(west, step=dx,
length=nx+reg)` for exactly that reason.

---

## What was dropped from `gmtmbgrid.c`

- **`mb_surface()` and its ~1300 lines of helpers** (`iterate`, `fill_in_forecast`,
  `find_nearest_point`, `remove_planar_trend`, `get_prime_factors`, `gcd_euclid`, …) — a fork of
  GMT 4-era `surface.c`. `mbgrid_nodes()` hands the binned nodes to `GMT.surface` instead and
  `mbgrid_fill()` merges the answer back through the same clip logic. That is `solver = :surface`.
- **Option parsing, `usage()`, GMT record I/O, the `-F` background grid** — the caller's job.
- **`mb_zgrid`'s `knxt` chains and its "shift data points back" block.** This removal is *exact,
  not a simplification*: the input here is already one value per node, so the chains that averaged
  several points sharing a node are all length 1, and the sub-cell offset the block computes is
  identically zero for a node-centred point — its parabolic correction is zero and it re-pins each
  data node to the value it already holds. The argument is written out above the function so nobody
  has to re-derive it before touching the solver.

Also not carried over: the `-F` background grid and the second-input-file-as-background path.

---

## Defects fixed relative to `gmtmbgrid.c`

Each has a regression test in `deps/src/test_mbgrid.c`.

| defect | test |
|---|---|
| `mb_zgrid` fed a **world coordinate** into `i = (int)(x + 1.5)` and then wrote `z[i + j*nx]` (`gmtmbgrid.c:1564`), where every other site in the same function divides by `dx` first. An out-of-bounds write on any grid not starting at (0,0) with unit spacing — i.e. all of them — every tenth iteration. | `test_translation_invariance` grids the same data at origin (0,0) and (−25,37) and requires identical output |
| `-Cg`'s ring search ran in `uint64_t`. `i - ir` underflows at the west/south edges, so those rings were silently skipped; `(ii - i)` wrapped to ~1.8e19, making `dmask[iii*3 + jjj]` a stack overrun on a 9-element array. Signed throughout now. | `test_clip_modes` |
| `interp_breakline()` dereferenced a NULL grid header on its first statement — `-D` can never have executed. Rewritten from scratch. | `test_breakline` |
| The `zmin`/`zmax` scan started at node 1. | the plane tests |
| `-E` binned against the **extended** corner but built node coordinates from the **un-extended** one, so data sat `offx` cells off the nodes they had been binned into. One origin now, `mb_geometry`. | `test_extend` requires `-E0.25` to leave the cropped output unchanged |
| The unset marker was the magic value `99999`, which any z above it collided with. `NaN` now. | throughout |
| Node indices came from truncation, so the half-cell strip west/south of the working region folded onto column/row 0 and falsely marked those nodes as data-bearing. `floor()` now. | `test_binning` |

### Further changes made during review of the port

| change | why |
|---|---|
| `reserved` → `registration`, pixel registration implemented | The Interpolate dialog's registration checkbox is shared by every gridding method. A shared control that does nothing for one method is a `SACRED_LAW.md` violation. |
| `MBGRID_SG_COLMAJOR` added | The `:surface` path was doing `permutedims(Gs.z)`. The grid memory-layout law says a buffer is consumed where it lies, with a layout code beside it. |
| `z = sgrid - (nx + 1)` → the `ZZ(i,j)` macro | That pointer is formed *before* the start of the object, which is undefined behaviour whether or not it is dereferenced. The macro is the same 1-based Fortran view with no pointer arithmetic. |
| `-C` ALL radius taken from the **working** dims | It was sized from the output dims while every sweep that consumes it walks the working grid — under-reached whenever `extend > 0`. |
| `mbgrid_run` validates the layout code up front | It used to solve the whole grid and only then let `mbgrid_extract` reject the code. |
| `mbgrid_work_origin` exported | The `:surface` path was re-deriving the working origin in Julia — a second copy of the one piece of arithmetic that knows how `-E` and the registration combine. |
| GAP/NEAR ring test factored to `mb_ring_hit` | It was duplicated verbatim in both ring walks, free to drift. |
| `MBGRID_ABI_VERSION` / `mbgrid_abi_version` dropped | One DLL, one version guard: `gmtvtk_abi_version` against `_ABI_REQUIRED`. A second, mbgrid-private counter is a parallel implementation of a mechanism that already exists. |
| `_mbfn` lazy-dlsym resolver dropped | Same reason — the `mbgrid_*` exports go in `_LIB_SYMBOLS` and resolve through `_fn`, like every other export of this library. |

---

## Build and ABI

`deps/src/mbgrid.c` is a **real second translation unit** in `GMTVTK_SRC`, not one of the
`NN_*.cpp` fragments — it must never be `#include`d into `gmtvtk.cpp`. It shares no file-static
helper with the viewer, includes none of its headers, and has to stay compilable and testable on
its own. Because of it, `deps/CMakeLists.txt` declares `project(... LANGUAGES C CXX)`; with `CXX`
alone CMake has no C compiler configured and refuses the file.

The ten exports (`mbgrid_dims`, `mbgrid_work_dims`, `mbgrid_work_origin`, `mbgrid_bin`,
`mbgrid_zgrid`, `mbgrid_nodes`, `mbgrid_fill`, `mbgrid_extract`, `mbgrid_run`, `mbgrid_strerror`)
are listed in `_LIB_SYMBOLS` (`src/libgmtvtk.jl`) with everything else. `_try_load` resolves the
whole tuple or rejects the library, so a `gmtvtk.dll` built before mbgrid existed — **including a
cached copy in `~/.julia/gmtvtk_runtime`** — now reports *"stale build"* and is skipped. That is the
intended mechanism, but it means the published dll-only zip needs a republish before a non-dev
install can use mbgrid.

`mbgrid_params`, `mbgrid_breakline` and `mbgrid_stats` are mirrored field-for-field in
`src/mbgrid.jl` (96 / 40 / 48 bytes, asserted in `test/test-mbgrid-unit.jl`). Reorder or retype a
field and you must bump `gmtvtk_abi_version` (`90_c_api.cpp`) together with `_ABI_REQUIRED`
(`src/libgmtvtk.jl`) — the same pair as for any other host-facing signature change.

## Running the tests

```
deps\build\test_mbgrid.exe                  # 69 checks, no Julia, no DLL
```

```julia
ENV["INTERACTIVEGMT_TEST_FILE"] = "mbgrid"
using Pkg; Pkg.test("InteractiveGMT")       # 75 checks
```

The Julia items that call the DLL are gated on the library having loaded, so a DLL-less checkout
still passes.
