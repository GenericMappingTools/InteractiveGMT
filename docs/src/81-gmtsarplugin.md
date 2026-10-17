# GMTSAR as a GMT plugin

GMTSAR's InSAR processing programs (`SAT_baseline`, `xcorr`, `phasediff`, `phasefilt`, `snaphu`,
the sensor preprocessors `make_slc_s1a`, `ALOS_pre_process`, …, 73 in all) are GMT modules in one
GMT supplement, `gmtsar`. **Geophysics ▸ GMTSAR ▸ Install as plugin** installs it:

1. downloads the archive for the system — `gmtsar-linux-x86_64.tar.gz`,
   `gmtsar-macos-arm64.tar.gz`, `gmtsar-macos-x86_64.tar.gz` or `gmtsar-windows-x86_64.tar.gz` —
   from the `gmtsar-latest` release of
   [joa-quim/gmtsar](https://github.com/joa-quim/gmtsar/releases/tag/gmtsar-latest);
2. unpacks it to `<InteractiveGMT>/gmtsar` (`lib/` holds `gmtsar.so` / `gmtsar.dylib` /
   `gmtsar.dll`, `share/gmtsar/` the filter kernels, snaphu configurations, PRM templates and the
   geoid grid), replacing any earlier copy;
3. loads it into GMT and tests it: GMT must have the GMTSAR modules, the library must be the one
   just installed, and `nearest_grid` must fill the NaN node of a small grid through GMT.

From then on every start of InteractiveGMT loads it again, and looks for a newer one on the
release; the menu then offers **Update the plugin**. On Windows it is offered only when GMT.jl runs
GMT_jll's GMT, the GMT the archive is built against.

The same from Julia:

```julia
using InteractiveGMT
InteractiveGMT.install_gmtsar_plugin()
```

## The archive

It is built by the `plugin` and `plugin-windows` jobs of the gmtsar repository's CI
(`.github/workflows/build.yml`) against **GMT_jll**, i.e. against the very `libgmt`, GDAL, libtiff,
HDF5 and OpenBLAS that GMT.jl loads: on Ubuntu 22.04, macOS 14 (Apple silicon), macOS 15 (Intel),
and on Windows with MSYS2's MINGW64 toolchain (the C runtime GMT_jll uses). The job checks that
nothing else is linked, that every module is exported, loads the unpacked plugin through GMT.jl and
runs a module on a grid before it publishes the archive. It contains only GMTSAR itself.

## How it is loaded

The same way as the [MB-System plugin](77-mbplugin.md): one mechanism serves both. The plugin is
added to `GMT_CUSTOM_LIBS` (keeping any plugin already there) in a throwaway `gmt.conf` that GMT
writes, and GMT.jl's session is re-created from it. No `gmt.conf` is left anywhere.

| | |
|---|---|
| Julia | `src/gmtplugin.jl` — `install_gmtsar_plugin` (= `install_plugin(GMTSAR_PLUGIN)`) |
| Menu | `deps/src/70_window.cpp`, Geophysics ▸ GMTSAR (`pluginEntry`) |
| Tests | `test/test-gmtplugin-unit.jl` |
| Archive | joa-quim/gmtsar `.github/workflows/build.yml`, jobs `plugin`, `plugin-windows` |
