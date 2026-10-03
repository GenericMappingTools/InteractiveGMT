# deps/src/cube — CUBE (Combined Uncertainty and Bathymetry Estimator)

`mb_cube.c` / `mb_cube.h` are a VERBATIM copy of `src/mbaux/mb_cube.{c,h}` in the MB-System fork
(`C:\progs_cygw\MB-System_take2`), where the engine lives and is tested
(`test/mbaux/mb_cube_test.cc`, plus a node-for-node comparison against bathycube's `cube.py`).

That engine is the C port of NOAA OCS Hydrography's bathycube `cube.py`
(https://github.com/noaa-ocs-hydrography/bathycube, MIT; notice in `mb_cube.h`). CUBE itself is
Calder & Mayer's, CCOM/JHC, University of New Hampshire.

Never edit these two files here. Change them in MB-System and copy them over, so the gridder MB-System's
`mbgrid -F9` runs and the one iGMT's GMT > Interpolate > CUBE runs stay one function.

The only build difference is `MB_CUBE_BUILD_DLL`, defined for `mb_cube.c` in `deps/CMakeLists.txt`:
gmtvtk.dll exports only what is marked, so the header's `MB_CUBE_API` becomes `dllexport` here
(MB-System exports every symbol and leaves it empty).

Julia face: `src/cube.jl` (`cubegrid`, `cubegrid_all`).
