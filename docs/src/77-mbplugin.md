# MB-System as a GMT plugin (Linux, macOS)

MB-System's programs are GMT modules (`mbinfo`, `mbgrid`, `mblist`, `mbswath`, …) living in one
GMT supplement, `mbsystem`. The MB-System tools of this viewer (MBedit, MBeditviz, MBgrdviz,
MBnavedit, MBvelocitytool, the water column viewer, CUBE) read swath files through the MBIO library that
supplement brings into the process.

On Windows the supplement comes with GMT. On Linux and macOS, **Geophysics ▸ MB-System ▸ Install
as plugin** installs it:

1. downloads the archive for the system — `mbsystem-linux-x86_64.tar.gz`,
   `mbsystem-macos-arm64.tar.gz` or `mbsystem-macos-x86_64.tar.gz` — from the `mbsystem-latest`
   release of [joa-quim/MB-System](https://github.com/joa-quim/MB-System/releases/tag/mbsystem-latest);
2. unpacks it to `<InteractiveGMT>/mbsystem` (`lib/` holds `mbsystem.so` / `mbsystem.dylib` and the
   MB libraries, `share/mbsystem/` the Levitus database), replacing any earlier copy;
3. loads it into GMT and reports whether GMT now has the MB-System modules and where MBIO is.

From then on every start of InteractiveGMT loads it again. Running the menu item again updates it.

The same from Julia:

```julia
using InteractiveGMT
InteractiveGMT.install_mbsystem_plugin()
```

## The archive

It is built by the `plugin` job of the MB-System repository's CI against **GMT_jll**, i.e. against
the very `libgmt`, GDAL, PROJ, netCDF and FFTW that GMT.jl loads: on Ubuntu 22.04 (glibc 2.35, the
same floor as InteractiveGMT's own Linux library), macOS 14 (Apple silicon) and macOS 15 (Intel),
with `$ORIGIN` / `@loader_path` so it works from any folder. The job checks that nothing else is linked, loads
the unpacked plugin through GMT.jl and runs a module before it publishes the archive. It contains
only MB-System's own libraries; everything else is already in the process.

## How it is loaded

GMT builds its list of plugins once, when a session is created, from `GMT_CUSTOM_LIBS` as read
from a `gmt.conf`. Setting the parameter in a running session does not load anything. So
InteractiveGMT has GMT write the current session's settings, plus `GMT_CUSTOM_LIBS` pointing at
`<InteractiveGMT>/mbsystem/lib/mbsystem.so` (`.dylib` on macOS), into a temporary directory, re-creates GMT.jl's session
there (`GMT.gmt_restart`) and deletes the directory. No `gmt.conf` is left anywhere, and the user's
`~/.gmt/gmt.conf` is never touched. Plugins already listed in `GMT_CUSTOM_LIBS` are kept.

| | |
|---|---|
| Julia | `src/mbplugin.jl` — `install_mbsystem_plugin`, activation at start-up from `__init__` |
| Menu | `deps/src/70_window.cpp`, Geophysics ▸ MB-System (shown on Linux and macOS) |
| Tests | `test/test-mbplugin-unit.jl` |
| Archive | joa-quim/MB-System `.github/workflows/build.yml`, job `plugin` |
