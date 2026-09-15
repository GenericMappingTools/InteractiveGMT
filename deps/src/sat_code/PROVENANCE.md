# sat_code — vendored, VERBATIM. Do not edit anything in this directory.

SGP4/SDP4 satellite motion model by Bill Gray / Project Pluto.

| | |
|---|---|
| Upstream | https://github.com/Bill-Gray/sat_code |
| Commit | `7df4b3eced79c7a8070850a1e9fe0e4fe0f5272b` |
| Vendored | 2026-09-15 |
| Licence | MIT — see `LICENSE`, kept beside the sources |

## Why the `lunar` dependency is not here

Upstream's README says the only dependency is
[`lunar`](https://github.com/Bill-Gray/lunar). That is true **of the repository**, not
of the propagator: `lunar` is needed by upstream's own command-line tools (`sat_id`,
`get_vect`, `sat_eph`), which are not taken. Every file below includes nothing but libc
headers and sat_code's own two — checked, not assumed — and `-llunar` appears exactly
once in upstream's makefile, on the `get_vect` demo.

`lunar_solar_position()`, declared in `norad.h` and the one plausible route to that
library, is **defined in `sdp4.cpp`** in this same directory. Nothing is missing.

## What is taken

Exactly the ten translation units of upstream's `libsatell` core — its `OBJS` list in
both `makefile` and `msvc.mak`, unabridged:

    sgp.cpp  sgp4.cpp  sgp8.cpp  sdp4.cpp  sdp8.cpp
    deep.cpp  basics.cpp  common.cpp  get_el.cpp  tle_out.cpp

plus the headers `norad.h` (public) and `norad_in.h` (the model's private constants),
and `LICENSE`.

`test2_reference.txt` is upstream's own `test2.txt`: published state vectors for a set
of TLEs spanning every one of the five models, near-earth and deep-space. It is the
fixture for `deps/src/test_satellite.c`, so this port is checked against the author's
numbers rather than against itself.

## Rules

- **Never edit these files.** A local fix is invisible at the next update and silently
  forks maths that has one upstream source of truth. Everything this project adds goes
  in `deps/src/satellite.cpp`, which is the only file that includes `norad.h`.
- `norad_in.h` is upstream's *private* header (WGS72 gravity constants, model macros).
  `satellite.cpp` deliberately does not include it.
- To update: re-copy the files above from a newer upstream commit, refresh
  `test2_reference.txt` from the same commit, record the new hash here, and run
  `deps/build/test_satellite`. A diff in those numbers is upstream changing its own
  answers and needs reading before it is accepted.
