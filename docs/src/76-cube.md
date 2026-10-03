# CUBE — Combined Uncertainty and Bathymetry Estimator

CUBE grids multibeam soundings into a depth surface **with an uncertainty for every node**, and it
does not average blunders or conflicting swaths into the answer. Each sounding is offered to the
grid nodes around it; at each node the soundings pass through a short median pre-filter and then a
Kalman filter with Bayesian monitoring. A sounding that does not fit the depth a node is tracking —
a spike, or the other side of a vertical offset between two overlapping lines — starts a new
**depth hypothesis** instead of being blended in. At the end each node reports the hypothesis its
selection method picks, how uncertain that depth is, how many hypotheses it held, and how sure the
choice is.

CUBE is Calder & Mayer's (Center for Coastal and Ocean Mapping / NOAA–UNH Joint Hydrographic Center,
University of New Hampshire). The code here is a C port of NOAA OCS Hydrography's Python
implementation, [bathycube](https://github.com/noaa-ocs-hydrography/bathycube) (MIT), and it is the
**same engine MB-System's `mbgrid -F9` runs**.

| | |
|---|---|
| Menu | **Geophysics ▸ MB-System ▸ CUBE gridding (mbgrid -F9)** — swath files and datalists |
| Menu | **GMT ▸ Interpolate**, Griding Method ▸ *CUBE (bathymetry with uncertainty)* — x,y,z tables (and swath data too) |
| Julia | `cubegrid`, `cubegrid_all` (`src/cube.jl`) |
| C | `deps/src/cube/mb_cube.c` + `mb_cube.h` — verbatim copy of MB-System's `src/mbaux/mb_cube.{c,h}` |
| C | `deps/src/cube/cube_swath.c` — reads the soundings of swath data through MB-System's MBIO |
| Tests | MB-System `test/mbaux/mb_cube_test.cc` (24 GTest cases, bathycube's own expected values) |

---

## Gridding a multibeam survey

**Geophysics ▸ MB-System ▸ CUBE gridding (mbgrid -F9)** opens the gridding dialog already on CUBE,
with swath data offered first in the file picker.

1. **Input** — an MB-System **datalist** (`.mb-1`) or a single swath file (`.mbNN`, `.all`,
   `.kmall`, `.s7k`, `.gsf`, …). It is read through MB-System's MBIO library — the same one the
   swath editor uses (the one GMT loads with its MB-System supplement, `INTERACTIVEGMT_MBIO`, or the
   path remembered from an earlier session). Beams flagged in MB-System are left out, exactly as
   `mbgrid` leaves them out.
2. **Griding Line Geometry** — region and spacing. Left **empty**, they are what GMT would choose for
   the soundings (their limits and `estimate_RI`), so a survey can be gridded without typing a
   region. Spacing is in the data's units (degrees for swath data).
3. **Options…** — the CUBE settings below.
4. **Compute.**

The depth grid opens in the window as a new handle. With *Also make uncertainty / hypotheses /
ratio grids* ticked, four more grids arrive as their own handles in Scene Objects (the depth stays
the one on show):

| Grid | Meaning |
|---|---|
| *Gridded (cube)* | Depth of the chosen hypothesis — **elevation**, negative below sea level |
| *CUBE uncertainty (95%)* | CUBE's uncertainty of that depth, at 95% confidence, in metres |
| *CUBE hypotheses* | How many depth hypotheses the node holds. More than 1 = the soundings disagree there |
| *CUBE hypothesis strength* | 0 with a single hypothesis; larger = CUBE is less sure the reported one is right |
| *CUBE soundings* | Soundings in the reported hypothesis |

The *hypotheses* and *strength* grids are the ones to look at: they map where the data are
ambiguous — overlapping lines that disagree, spikes, a wrong sound speed on one line.

Swath soundings come back as longitude, latitude and **elevation** (MBIO's depth negated), so the
grid has the sign of every other grid in iGMT. MB-System's own `mbgrid -A1` writes depth positive
down; the values are the same with the opposite sign.

## Options

| Option | Default | Meaning |
|---|---|---|
| IHO order | Order 1a | IHO S-44 survey order: *Exclusive*, *Special*, *1a*, *1b*, *2*. It bounds how far a sounding may spread, and is the default sounding uncertainty. |
| Hypothesis selection | local | *local*: the nearest node that holds a single hypothesis guides the choice; *prior*: the hypothesis with the most soundings; *posterior*: both combined; *predicted*: the one closest to the node's predicted depth. |
| Vertical uncertainty a/b | IHO order | Sounding TVU at 95%: `sqrt(a² + (b·depth)²)` m. |
| Horizontal uncertainty a/b | IHO order | Sounding THU at 95%: `a + b·depth` m. |
| Reported uncertainty | CUBE posterior | *cube*: CUBE's posterior; *input*: spread of the soundings in the hypothesis; *max*: the larger. |
| Z is depth (positive down) | off | For **tables** only: tick it when z is depth. Swath data is handled by itself. |
| Skip the median pre-filter | off | Feed soundings to the estimator in input order (the CUBE User Manual's "reordering" step off). |
| Also make … grids | off | The four extra grids above. |
| Parameter file | — | A bathycube `CubeParameters` JSON file (any of its keys, e.g. `{"capture_dist_scale": 0.1}`); the boxes override it. |

### The sounding uncertainty

CUBE weighs each sounding by its a priori uncertainty. MB-System's swath data carry none per beam,
so it is modelled in the IHO S-44 form at 95% confidence. Left empty, the coefficients are the
**limits of the chosen IHO order** — a conservative choice that assumes the survey just meets its
order:

| Order | TVU a | TVU b | THU a | THU b |
|---|---|---|---|---|
| Exclusive | 0.15 m | 0.0075 | 1 m | 0 |
| Special | 0.25 m | 0.0075 | 2 m | 0 |
| 1a / 1b | 0.5 m | 0.013 | 5 m | 0.05 |
| 2 | 1.0 m | 0.023 | 20 m | 0.10 |

For a better grid, give the real performance of the system (e.g. a modern deep-water multibeam:
TVU `0.3/0.005`, THU `2/0.02`).

### Units

CUBE measures in **metres**. Geographic data are converted to local metres at the grid centre with
GMT's own geodesic (`mapproject -G`); projected (Cartesian) data are taken to be in metres.

---

## From Julia

```julia
using InteractiveGMT

G  = cubegrid("survey.mb-1")                              # a datalist; region and spacing from the data
G  = cubegrid("line.all"; region = (-11.8, -11.39, 36.71, 36.92), inc = 0.001, iho_order = :special)
nt = cubegrid_all("survey.mb-1"; inc = 0.0011, tvu = (0.3, 0.005), thu = (2, 0.02))
nt.depth; nt.uncertainty; nt.n_hypotheses; nt.ratio; nt.n_points    # five GMTgrids on the same nodes

D  = gmtread("soundings.xyz", data = true)               # an x,y,z table: z is elevation...
G  = cubegrid(D; region = (-9.5, -8.5, 36.5, 37.5), inc = 0.002)
G  = cubegrid(D; region = ..., inc = ..., zdown = true)  # ...unless zdown = true
```

`data` may be a swath path (as above), a text-table path, a `GMTdataset`, a vector of them, an N×3
matrix, or `x, y, z` vectors. Keywords: `region`, `inc`, `iho_order`, `method`, `tvu`, `thu`,
`variance`, `noqueue`, `paramfile`, `zdown`, `geographic`, `registration`, `verbose` — the dialog's
options, by the same names. Every one also takes the string form the dialog sends.

## From MB-System

The same engine is in MB-System's `mbgrid` as algorithm 9:

```
mbgrid -I survey.mb-1 -F9 -E100/0/meters -M -O survey_cube \
       --cube-iho-order=special --cube-method=local \
       --cube-uncertainty=0.3/0.005/2/0.02 --cube-variance=cube
```

`-M` also writes `_num` (soundings in the hypothesis), `_sd` (CUBE uncertainty, 95%), `_hyp` and
`_ratio`. The GMT-module form of mbgrid takes the same settings as `-F9` modifiers:
`-F9+o<order>+m<method>+u<tvu_a>/<tvu_b>/<thu_a>/<thu_b>+v<variance>+p<paramfile>+q`.

---

## About the port

The C engine follows bathycube's `cube.py` function for function and keeps its comments. Where
`cube.py` is wrong, the port is fixed rather than copied; each fix is marked `PORT FIX` in
`mb_cube.c` and listed at its top. The ones that change results:

- the median pre-filter queues are flushed before depths are read (`cube.py` never did, so the last
  soundings at every node — and every node with fewer than 11 — were lost);
- the neighbourhood search for *local* / *posterior* uses `max(1, …)` nodes (`cube.py` had `min`,
  which pinned it to one ring);
- the IHO error budget uses the squared terms (`cube.py` mixed metres and metres²);
- soundings that reach only the last row or column reach it;
- the context node's *variance*, not its 95% interval, guides *local* / *posterior*.

Checked node for node against a copy of `cube.py` with the same fixes (1,421 nodes × 3 methods,
differences at float rounding), and on 420,254 EM122 soundings against `mbgrid -F9` (39,434 vs
39,437 nodes set; mean depth difference 0.09 m at ~4.4 km, from the degree-to-metre scale).

## References

- Calder, B. R. and Mayer, L. A. (2003). Automatic processing of high-rate, high-density multibeam
  echosounder data. *Geochemistry, Geophysics, Geosystems*, 4(6), 1048.
- NOAA OCS Hydrography, bathycube: <https://github.com/noaa-ocs-hydrography/bathycube>
- IHO S-44, Standards for Hydrographic Surveys, 6th edition (2020), Table 1.
