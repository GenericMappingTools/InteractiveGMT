# MBnavadjust — navigation adjustment of swath surveys

MB-System's **mbnavadjust**, ported into iGMT. It makes the bathymetry of overlapping and crossing
swath lines agree by adjusting their navigation. Each file of a project is cut into **sections**.
Where two sections overlap they form a **crossing**, and at each crossing an offset, a **tie**, is
set by hand (dragging one section's contours over the other's) or by correlation (Auto-Pick). An
inversion turns all the ties into a smooth navigation adjustment for every file, which mbprocess
then applies when it makes the processed files.

| | |
|---|---|
| Menu | **Geophysics ▸ MB-System ▸ MBnavadjust** |
| Julia | `mbnavadjust(project = ""; fig = nothing)` (`src/mbnavadjust.jl`) |
| C / C++ | `deps/src/mbnavadjust/`, `deps/ui/mbnavadjust*.ui` |
| Needs | MB-System's MBIO library (found as the swath editor finds it); GMT's MB-System supplement for mbdatalist, mbprocess and mbgrid |
| Tests | `test/test-mbnavadjust-gui.jl` (`:gui`) |

---

## Workflow

1. **File ▸ New** creates a project: a `.nvh` file and a `.dir` data folder beside it. **File ▸ Open**
   reopens one.
2. **File ▸ Import Swath Data** adds swath files, or a datalist (MBIO format id `-1`), each file a
   survey of its own. *Import Swath Data as Single Survey* makes them one survey. The files are cut
   into sections (**Option ▸ Controls**: section length and number of soundings), the crossings are
   found, and the project grids are made with mbgrid.
3. The **List** menu chooses what the list shows: data files, surveys, blocks, sections, crossings
   (all, or by overlap), ties, global ties, reference grids. The **View** menu chooses which of them.
4. Selecting a crossing, or **Action ▸ Analyze Crossings**, opens the analysis window. On the right
   are the two sections' contours: drag with the **left** button to move section 2 over section 1,
   drag with the **middle** button to zoom, click the **right** button to pick the tie points. On the
   left are the misfit of every offset (click to set it; *Minimum Misfit* jumps to the best one) and
   the misfit against the z offset. **Add Tie** / **Set Offset** store the tie, **Skip Crossing**
   leaves the crossing out, **Next Unset** moves on. *Settings* holds the heading and roll biases and
   the contouring. **Action ▸ Auto-Pick Offsets** does all this by correlation for every unset
   crossing listed.
5. **Action ▸ Invert Navigation** solves for the adjustment (Option ▸ Controls: *Inversion
   Smoothing*). **Update Grids** applies it to the sections and grids them again. **Apply Adjusted
   Navigation** writes each file's adjusted navigation and sets it in the file's mbprocess
   parameters, as primary, secondary or tertiary navigation (Option ▸ Controls).
6. **View ▸ Show Modelplot** plots the adjustment model as a time series, as a perturbation, or the
   tie offsets.
7. **View ▸ Visualize Survey** opens the project grid in a viewer window with every section's
   navigation and every tie drawn on it. Click a tie to analyse its crossing, or click two sections
   one after the other to open their crossing. **View ▸ Add and Open New Crossing** makes one where
   none was found.

Closing or minimising the window parks it in Scene Objects. **File ▸ Quit** saves the project and
closes the tool. **Help ▸ How to use MBnavadjust** (F1) holds the same guide.

## How it is ported

- MB-System's GUI-free core (`mbnavadjust_io.c`, `_autopick.c`, `_invertnav.c`) is carried as it is.
- The engine (`mbnavadjust_prog.c`) and the Motif callbacks (`mbnavadjust_callbacks.c`) stay C. The
  callbacks run on a small widget bridge (`mbna_motif.h`): each widget is its Motif name, which is
  its objectName in the `.ui` files. The Qt window carries out the widget calls and draws the xg
  graphics into images.
- The pieces of libmbaux it needs (the swath contourer, the Delaunay triangulator, LSQR) are in
  `mbna_aux.c`. MB-System's grid reader keeps its own logic (`mbna_grid.c`); the grid values come
  from iGMT's grid reader.
- The programs upstream runs through `system()` (mbdatalist, mbprocess, mbgrid) run in iGMT as
  MB-System's GMT modules, in the project folder.
- The visualization, libmbview upstream, is an iGMT viewer window.
