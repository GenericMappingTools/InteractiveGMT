cmake_minimum_required(VERSION 3.21)  # 3.21+ for install(... RUNTIME_DEPENDENCIES ...)
project(gmtvtk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Declared up here (used below to gate gmtvtk_demo) — see the big comment near the bottom of
# this file for what it turns on.
option(GMTVTK_PACKAGE "Bundle all runtime DLLs and enable CPack packaging" OFF)

option(GMTVTK_LINUX_BUNDLE Stage_a_self-contained_Linux_Conda_runtime OFF)

# Toolchain locations (this machine, verified 2026-06-11). Override on the cmake command line
# (-DVTK_DIR=... etc.) for a different box.
if(WIN32 AND NOT VTK_DIR)
  set(VTK_DIR "C:/programs/compa_libs/VTK-9.6.2/compileds/lib/cmake/vtk")
endif()
if(WIN32 AND NOT Qt6_DIR)
  set(Qt6_DIR "C:/programs/Qt6/6.11.1/msvc2022_64/lib/cmake/Qt6")
endif()
if(WIN32 AND NOT TBB_DIR)
  set(TBB_DIR "C:/programs/compa_libs/oneTBB/compileds/VC14_64/lib/cmake/TBB")
endif()

# WSL/Linux dependency root. Prefer the activated environment, with this machine's base Conda as
# the fallback. An explicit -DCMAKE_PREFIX_PATH always wins.
if(UNIX AND NOT APPLE AND NOT CMAKE_PREFIX_PATH)
  if(DEFINED ENV{CONDA_PREFIX})
    list(PREPEND CMAKE_PREFIX_PATH $ENV{CONDA_PREFIX})
  elseif(EXISTS /home/j/anaconda3)
    list(PREPEND CMAKE_PREFIX_PATH /home/j/anaconda3)
  endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets UiTools)
find_package(TBB REQUIRED CONFIG)
find_package(VTK REQUIRED COMPONENTS
  CommonCore CommonDataModel CommonColor CommonTransforms
  FiltersSources FiltersGeneral
  # 87_vtkio.cpp reads/writes VTK's OWN formats natively — nothing else in the project links a VTK
  # IO module (all grid/image I/O goes through GMT.jl + GDAL on the Julia side). IOXML covers the
  # XML family (.vti/.vtr/.vts/.vtp/.vtu + the .pvt* parallel forms + .vtm), IOLegacy the old .vtk,
  # IOHDF the modern .vtkhdf. FiltersGeometry gives vtkDataSetSurfaceFilter, which is how a
  # structured/unstructured dataset becomes the polydata every iGMT display path already speaks.
  IOXML IOXMLParser IOLegacy IOHDF
  FiltersGeometry FiltersCore
  RenderingCore RenderingOpenGL2 RenderingAnnotation RenderingFreeType RenderingLOD
  RenderingContext2D RenderingContextOpenGL2
  ChartsCore ViewsContext2D
  InteractionStyle InteractionWidgets
  GUISupportQt)

# The whole viewer is ONE translation unit: src/gmtvtk.cpp #includes the src/NN_*.cpp fragments
# (00_includes, 10_geometry, ... 90_c_api). Only gmtvtk.cpp is in this source list; the fragments
# are #included textually (NOT compiled separately — do not add them here, or their file-static
# helpers would multiply-define), keeping internal linkage just as in the old main.cpp.
set(GMTVTK_SRC src/gmtvtk.cpp)

# deps/ui/*.ui are loaded at RUNTIME (QUiLoader, see FocalMechanismsDialog in 70_window.cpp) for
# dialogs where the .ui must stay the single source of truth — no hand-ported C++ to fall out of
# sync with. GMTVTK_UI_DIR points at the source tree's ui/ dir (dev-only unregistered package,
# same pragmatic hard-coded-path style as VTK_DIR/Qt6_DIR/TBB_DIR above).
add_compile_definitions(GMTVTK_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui")
add_compile_definitions(GMTVTK_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data")

# The viewer is ONE translation unit (gmtvtk.cpp #includes every NN_*.cpp fragment), so its object
# file long ago passed the 65279-section COFF limit that /bigobj lifts — the test build, which adds
# the GMTVTK_TEST_API hooks on top, hit it first ("fatal error C1128"). All three targets compile the
# same TU, so all three need the flag.
if(MSVC)
  add_compile_options(/bigobj)
endif()

# Shared library — the Julia/host bridge (C API: gmtvtk_view_grid / gmtvtk_view_demo / ...).
add_library(gmtvtk SHARED ${GMTVTK_SRC})
target_link_libraries(gmtvtk PRIVATE Qt6::Widgets Qt6::UiTools TBB::tbb ${VTK_LIBRARIES} ${CMAKE_DL_LIBS})
vtk_module_autoinit(TARGETS gmtvtk MODULES ${VTK_LIBRARIES})

# Standalone demo executable (shows the synthetic peaks surface via the demo main()) — a dev
# smoke-test binary only (deps/run.bat), no Julia involved. Skipped entirely for a package build:
# the redistributable installs the real Julia-driven app (iview_app.jl), not this demo.
if(NOT GMTVTK_PACKAGE)
  if(WIN32)
    add_executable(gmtvtk_demo WIN32 ${GMTVTK_SRC})
  else()
    add_executable(gmtvtk_demo ${GMTVTK_SRC})
  endif()
  target_link_libraries(gmtvtk_demo PRIVATE Qt6::Widgets Qt6::UiTools TBB::tbb ${VTK_LIBRARIES} ${CMAKE_DL_LIBS})
  vtk_module_autoinit(TARGETS gmtvtk_demo MODULES ${VTK_LIBRARIES})
endif()

# Test-only shared library — SAME source, built a second time with GMTVTK_TEST_API defined so
# the gmtvtk_*_test hooks in 90_c_api.cpp (guarded by #ifdef GMTVTK_TEST_API) compile in. The
# production `gmtvtk` target above gets no such define, so those symbols never enter its export
# table. Needed as a second full compile (not a separate .cpp) because the test hooks call
# static-linkage internals (faultApplyGeom, mecaDragTo, addSymbols, ...) that only exist inside
# this one TU. Built automatically by `deps/build.bat`'s plain `cmake --build build` (Ninja's
# default target is "all") — no build.bat edit needed.
add_library(gmtvtk_test SHARED ${GMTVTK_SRC})
target_compile_definitions(gmtvtk_test PRIVATE GMTVTK_TEST_API)
target_link_libraries(gmtvtk_test PRIVATE Qt6::Widgets Qt6::UiTools TBB::tbb ${VTK_LIBRARIES} ${CMAKE_DL_LIBS})
vtk_module_autoinit(TARGETS gmtvtk_test MODULES ${VTK_LIBRARIES})

# Linux runtime staging is independent of the Windows packaging block below.
if(GMTVTK_LINUX_BUNDLE)
  if(NOT UNIX OR APPLE)
    message(FATAL_ERROR GMTVTK_LINUX_BUNDLE_is_Linux-only)
  endif()
  if(NOT CMAKE_PREFIX_PATH)
    message(FATAL_ERROR GMTVTK_LINUX_BUNDLE_needs_CMAKE_PREFIX_PATH)
  endif()
  set_target_properties(gmtvtk PROPERTIES INSTALL_RPATH $ORIGIN)
  install(TARGETS gmtvtk LIBRARY DESTINATION .)
  list(GET CMAKE_PREFIX_PATH 0 GMTVTK_LINUX_CONDA_PREFIX)
  configure_file(cmake/linux_bundle.cmake.in linux_bundle.cmake @ONLY)
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/linux_bundle.cmake)
endif()

# ============================================================================
# Optional redistributable package — gmtvtk.dll + EVERY runtime DLL it needs
# (VTK, Qt, TBB) + the Qt platform plugins, so the package runs on a box
# WITHOUT VTK/Qt installed — PLUS the Julia bridge (src/, Project.toml,
# deps/ui, data/) and an NSIS installer that drops a Desktop + Start Menu icon
# launching the real Julia-driven app (iview_app.jl). gmtvtk_demo is skipped
# entirely above when this is ON — it is never built or shipped in the
# package. OFF by default (a normal dev build wants none of this).
#
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGMTVTK_PACKAGE=ON
#   cmake --build build
#   cmake --build build --target package      # -> iGMT-0.1.0-win64.{zip,exe}
#
# How the DLLs are gathered:
#   * VTK + TBB + Qt6 core DLLs: install(... RUNTIME_DEPENDENCIES ...) scans the
#     built binaries for their imports and resolves them from the toolchain bins.
#   * Qt *plugins* (platforms/qwindows.dll, styles, imageformats) are loaded at
#     RUNTIME, not import-linked, so the scanner cannot see them -> windeployqt
#     (pointed at gmtvtk.dll itself — windeployqt accepts a dll, not just an exe).
#
# Installed layout mirrors the dev tree exactly (deps/build/gmtvtk.dll + its
# runtime DLLs beside it, deps/ui/*.ui, src/, Project.toml, data/) so
# src/libgmtvtk.jl's _PKGROOT-relative paths and gmtvtk.dll's own runtime
# self-location (gmtvtkUiDir(), 30_app.cpp) resolve unchanged on the
# destination machine. The installed launcher (deps/installer/iview_launch.vbs)
# assumes Julia + this package's deps (GMT.jl, ...) are already
# installed/instantiated on the destination machine — this does not bundle or
# bootstrap Julia itself.
# ============================================================================

if(GMTVTK_PACKAGE AND NOT WIN32)
  message(FATAL_ERROR GMTVTK_PACKAGE_is_currently_Windows-only)
endif()

if(GMTVTK_PACKAGE AND WIN32)
  # Derive the toolchain runtime-DLL dirs from the *_DIR cmake-config locations
  # (each is <root>/lib/cmake/<pkg>, so the bin dir is three levels up + /bin).
  get_filename_component(_VTK_ROOT "${VTK_DIR}/../../.." ABSOLUTE)
  get_filename_component(_QT_ROOT  "${Qt6_DIR}/../../.." ABSOLUTE)
  get_filename_component(_TBB_ROOT "${TBB_DIR}/../../.." ABSOLUTE)
  set(_VTK_BIN "${_VTK_ROOT}/bin")
  set(_QT_BIN  "${_QT_ROOT}/bin")
  set(_TBB_BIN "${_TBB_ROOT}/bin")

  # Install gmtvtk.dll into the SAME relative dir the dev build already uses
  # (deps/build), and have CMake resolve + install its dependent DLLs there
  # too. System/Windows DLLs are excluded. COMPONENT full: this goes into the
  # big runtime bundle (see the two-zip split below).
  install(TARGETS gmtvtk
    RUNTIME_DEPENDENCY_SET gmtvtk_deps
    RUNTIME DESTINATION deps/build COMPONENT full
    LIBRARY DESTINATION deps/build COMPONENT full
    ARCHIVE DESTINATION deps/build COMPONENT full)

  # A SECOND, independent copy of just gmtvtk.dll under its own component —
  # this is what makes the small "dll-only" zip below. Same file, different
  # component staging dir; no conflict with the COMPONENT full install above.
  install(FILES "$<TARGET_FILE:gmtvtk>" DESTINATION deps/build COMPONENT dll)

  # ...plus any VTK/Qt module the PINNED full runtime bundle predates. The two
  # zips move at different speeds (deps/build.jl): the full bundle is fetched
  # ONCE EVER per machine, gated on .full_runtime_installed, so bumping
  # deps/RUNTIME_VERSION does NOT reach an existing install — the dll-only zip
  # is the only channel that does. The moment the C++ links a VTK module that
  # is not inside the published iGMT-win64-full.zip, shipping gmtvtk.dll alone
  # gives every existing install a DLL that cannot load at all (LoadLibrary
  # fails -> the Julia side reports a missing gmtvtk_* symbol, with no hint
  # that a dependency is what is actually absent). So the dll component carries
  # those modules too.
  #
  # Found 2026-07-29: reading VTK's own file formats (vtkHDFReader) pulled in
  # vtkIOHDF + vtkhdf5 + vtkhdf5_hl + vtkFiltersTemporal, none of which exist in
  # runtime-0.1, and the dll-only zip bricked every machine it reached.
  #
  # To check this list after linking new VTK components: walk gmtvtk.dll's
  # transitive imports (dumpbin /dependents) and subtract the file list of the
  # published iGMT-win64-full.zip — whatever remains belongs here. When
  # RUNTIME_VERSION is next bumped to a bundle that already contains them, the
  # entries become harmless duplicates and can be dropped.
  # NOTE: this list becomes unnecessary the moment deps/RUNTIME_VERSION is bumped to a bundle
  # that already contains these — deps/build.jl now stores the installed TAG in
  # .full_runtime_installed and re-fetches the bundle when the pin moves, so a bump finally
  # reaches existing installs. Until that new bundle is published, the modules must keep riding
  # along in the dll-only zip.
  set(_GMTVTK_DLL_EXTRA_MODULES
    vtkIOHDF vtkhdf5 vtkhdf5_hl vtkFiltersTemporal)
  foreach(_m IN LISTS _GMTVTK_DLL_EXTRA_MODULES)
    if(EXISTS "${_VTK_BIN}/${_m}.dll")
      install(FILES "${_VTK_BIN}/${_m}.dll" DESTINATION deps/build COMPONENT dll)
    else()
      message(WARNING "dll-only zip: ${_m}.dll not found in ${_VTK_BIN} — the zip will ship an unloadable gmtvtk.dll if the pinned runtime bundle lacks it too")
    endif()
  endforeach()

  # `.dll_requires` — the contract between the two zips. Lists every non-system DLL gmtvtk.dll
  # imports, transitively, computed at package time by the same resolver install(RUNTIME_DEPENDENCY_SET)
  # uses. deps/build.jl checks it after extracting and src/libgmtvtk.jl repeats it when dlopen
  # fails, so "a module is missing" is reported AS THAT, instead of as a missing gmtvtk_* symbol.
  # Shipped in BOTH components: the full bundle should always satisfy its own manifest, and the
  # dll-only zip is the one that can outrun the installed bundle.
  install(CODE "
    file(GET_RUNTIME_DEPENDENCIES
      LIBRARIES \"$<TARGET_FILE:gmtvtk>\"
      RESOLVED_DEPENDENCIES_VAR _res
      UNRESOLVED_DEPENDENCIES_VAR _unres
      DIRECTORIES \"${_VTK_BIN}\" \"${_QT_BIN}\" \"${_TBB_BIN}\"
      PRE_EXCLUDE_REGEXES \"api-ms-.*\" \"ext-ms-.*\"
      POST_EXCLUDE_REGEXES \".*[\\\\/][Ss]ystem32[\\\\/].*\\\\.dll$\" \".*[\\\\/]WINDOWS[\\\\/].*\")
    set(_lines \"# gmtvtk.dll runtime dependencies, generated at package time. One filename per line.\\n\")
    foreach(_dep IN LISTS _res)
      get_filename_component(_n \"\${_dep}\" NAME)
      string(APPEND _lines \"\${_n}\\n\")
    endforeach()
    file(WRITE \"\${CMAKE_INSTALL_PREFIX}/deps/build/.dll_requires\" \"\${_lines}\")
    if(_unres)
      message(WARNING \"gmtvtk.dll has UNRESOLVED dependencies: \${_unres}\")
    endif()
  " COMPONENT dll)
  install(CODE "
    file(GET_RUNTIME_DEPENDENCIES
      LIBRARIES \"$<TARGET_FILE:gmtvtk>\"
      RESOLVED_DEPENDENCIES_VAR _res
      UNRESOLVED_DEPENDENCIES_VAR _unres
      DIRECTORIES \"${_VTK_BIN}\" \"${_QT_BIN}\" \"${_TBB_BIN}\"
      PRE_EXCLUDE_REGEXES \"api-ms-.*\" \"ext-ms-.*\"
      POST_EXCLUDE_REGEXES \".*[\\\\/][Ss]ystem32[\\\\/].*\\\\.dll$\" \".*[\\\\/]WINDOWS[\\\\/].*\")
    set(_lines \"# gmtvtk.dll runtime dependencies, generated at package time. One filename per line.\\n\")
    foreach(_dep IN LISTS _res)
      get_filename_component(_n \"\${_dep}\" NAME)
      string(APPEND _lines \"\${_n}\\n\")
    endforeach()
    file(WRITE \"\${CMAKE_INSTALL_PREFIX}/deps/build/.dll_requires\" \"\${_lines}\")
  " COMPONENT full)

  install(RUNTIME_DEPENDENCY_SET gmtvtk_deps
    PRE_EXCLUDE_REGEXES
      "api-ms-.*" "ext-ms-.*" "^hvsifiletrust.dll$" "^pdmutilities.dll$"
    POST_EXCLUDE_REGEXES
      ".*[\\/][Ss]ystem32[\\/].*\\.dll$" ".*[\\/]WINDOWS[\\/].*"
    DIRECTORIES "${_VTK_BIN}" "${_QT_BIN}" "${_TBB_BIN}"
    DESTINATION deps/build
    COMPONENT full)

  # Qt runtime plugins (not import-linked) — let windeployqt drop them next to
  # the installed gmtvtk.dll. Runs at `cmake --install` / package time.
  # COMPONENT full only — the dll-only zip ships no Qt plugins.
  find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_QT_BIN}")
  if(WINDEPLOYQT_EXE)
    install(CODE "
      message(STATUS \"windeployqt: staging Qt plugins into deps/build\")
      execute_process(COMMAND \"${WINDEPLOYQT_EXE}\"
        --release --no-translations --no-compiler-runtime
        --dir \"\${CMAKE_INSTALL_PREFIX}/deps/build\"
        \"\${CMAKE_INSTALL_PREFIX}/deps/build/gmtvtk.dll\")
    " COMPONENT full)
  else()
    message(WARNING "windeployqt not found in ${_QT_BIN}; Qt platform plugins will be MISSING from the package")
  endif()

  # The Julia bridge — same relative layout as the dev checkout, trailing
  # slashes so CONTENTS land at DESTINATION (not nested one level deeper).
  # Only these ship: they're the ONLY .ui files loaded at RUNTIME (QUiLoader — grep
  # `gmtvtkUiDir() +` across deps/src to check the list is complete; a missing one
  # means that dialog simply refuses to open on an installed copy). Every other
  # deps/ui/*.ui is a hand-ported design reference the running app never reads, not
  # needed on the destination machine. COMPONENT full only — the dll-only zip is just
  # the DLL, the .jl side is already on disk from the git clone.
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../src/"  DESTINATION src COMPONENT full)
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/focal_mechanisms.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/igrf_calculator.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/magnetic_barcode.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/cube_layer_selector.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/rtp3d.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/clipp_grid.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/aquamoto.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/gravmag3d_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdgravmag3d_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdredpol_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdgradient_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdseamount_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grid_calculator.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdtrend_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdlandmask_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdfilter_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/euler_stuff.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/plate_calculator.ui"
    DESTINATION deps/ui COMPONENT full)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../data/" DESTINATION data COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../Project.toml"        DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../iview_app.jl"        DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../iview_splash.hta"    DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/crystalball.jpg" DESTINATION . COMPONENT full)  # splash background
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/installer/iview_launch.vbs" DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/igmt.ico"        DESTINATION . COMPONENT full)

  # CPack — TWO zips (full runtime bundle + dll-only) plus an NSIS installer
  # (Desktop + Start Menu icon, always the full bundle — see below).
  #
  # CPACK_ARCHIVE_COMPONENT_INSTALL + COMPONENTS_GROUPING IGNORE make the ZIP
  # generator emit ONE ARCHIVE PER COMPONENT instead of one archive with
  # everything: component "full" -> CPACK_ARCHIVE_FULL_FILE_NAME.zip (rarely
  # changes — VTK/Qt/TBB runtime, pinned by deps/RUNTIME_VERSION on the Julia
  # side), component "dll" -> CPACK_ARCHIVE_DLL_FILE_NAME.zip (just gmtvtk.dll,
  # re-uploaded to the fixed "dll-latest" release tag on every rebuild — see
  # deps/build.jl). This does NOT affect NSIS: CPACK_NSIS_COMPONENT_INSTALL is
  # left OFF (default), so the .exe installer stays monolithic and always
  # contains the full set regardless of component tags.
  #
  # CPACK_COMPONENTS_ALL is deliberately NOT set here — CPack auto-fills it
  # from the COMPONENT tags on the install() calls above. A hardcoded
  # set(CPACK_COMPONENTS_ALL "full;dll") here would silently overwrite any
  # `cpack -D CPACK_COMPONENTS_ALL=dll` passed on the command line: CPack runs
  # this whole file as a cmake script on every `cpack` invocation, and a plain
  # set() always wins over a pre-existing -D value. Leaving it unset is what
  # makes single-component builds (below) actually work.
  set(CPACK_GENERATOR "ZIP;NSIS")
  set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
  set(CPACK_COMPONENTS_GROUPING IGNORE)
  set(CPACK_ARCHIVE_FULL_FILE_NAME "iGMT-win64-full")
  set(CPACK_ARCHIVE_DLL_FILE_NAME  "gmtvtk-win64")
  set(CPACK_PACKAGE_NAME "iGMT")
  set(CPACK_PACKAGE_VENDOR "InteractiveGMT")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_SYSTEM_NAME "win64")
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Self-contained Qt6 + VTK 3-D viewer for GMT.jl (bundled runtime)")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "iGMT")

  # NSIS: Desktop + Start Menu shortcuts to the installed launcher, using the
  # iGMT icon (deps/assets/igmt.ico, same PNG source as the in-app QIcon, see
  # deps/src/_app_icon.h). The launcher is a .vbs (not an .exe), so this uses
  # raw NSIS script (CPACK_NSIS_EXTRA_*_COMMANDS) instead of the
  # CPACK_PACKAGE_EXECUTABLES/CPACK_NSIS_MENU_LINKS machinery, which assumes an
  # installed .exe.
  #
  # include(CPack) DUMPS every CPACK_* variable verbatim into a GENERATED
  # CPackConfig.cmake as set(VAR "..."), which `cpack.exe` then RE-PARSES as
  # CMake — so this value gets CMake-string-parsed TWICE. A literal `"` here
  # closes that second parse's string early and corrupts the .nsi it writes
  # (verified: produced `CreateShortCut ;$DESKTOP/...` — the `"` vanished and
  # NSIS then read stray text as a comment, "expects 2-9 parameters, got 0").
  # `\"` (one literal backslash + quote, i.e. `\\\"` in THIS file's source)
  # survives the first parse as an escaped quote the second parse then turns
  # into a real `"` — the double-escaping CPack's own docs call out for
  # CPACK_NSIS_EXTRA_INSTALL_COMMANDS. Forward slashes throughout (NSIS's file
  # functions accept "/" fine on Windows; sidesteps a THIRD escaping layer).
  set(CPACK_NSIS_MUI_ICON    "${CMAKE_CURRENT_SOURCE_DIR}/assets/igmt.ico")
  set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/igmt.ico")
  set(CPACK_NSIS_DISPLAY_NAME "iGMT")
  set(CPACK_NSIS_PACKAGE_NAME "iGMT")
  set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    CreateShortCut \\\"$DESKTOP/iGMT.lnk\\\" \\\"$SYSDIR/wscript.exe\\\" '\\\"$INSTDIR/iview_launch.vbs\\\"' \\\"$INSTDIR/igmt.ico\\\"
    CreateDirectory \\\"$SMPROGRAMS/iGMT\\\"
    CreateShortCut \\\"$SMPROGRAMS/iGMT/iGMT.lnk\\\" \\\"$SYSDIR/wscript.exe\\\" '\\\"$INSTDIR/iview_launch.vbs\\\"' \\\"$INSTDIR/igmt.ico\\\"
  ")
  set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    Delete \\\"$DESKTOP/iGMT.lnk\\\"
    Delete \\\"$SMPROGRAMS/iGMT/iGMT.lnk\\\"
    RMDir \\\"$SMPROGRAMS/iGMT\\\"
  ")
  include(CPack)
endif()
