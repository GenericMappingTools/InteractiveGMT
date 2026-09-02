cmake_minimum_required(VERSION 3.21)  # 3.21+ for install(... RUNTIME_DEPENDENCIES ...)
# C as well as CXX: src/mbgrid.c (the MBGRID gridder) is a real C translation unit, not one of the
# NN_*.cpp fragments — see the GMTVTK_SRC comment below. Without C in this list CMake has no C
# compiler configured and refuses the file outright.
project(gmtvtk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Declared up here (used below to gate gmtvtk_demo) — see the big comment near the bottom of
# this file for what it turns on.
option(GMTVTK_PACKAGE "Bundle all runtime DLLs and enable CPack packaging" OFF)

option(GMTVTK_LINUX_BUNDLE Stage_a_self-contained_Linux_Conda_runtime OFF)

option(GMTVTK_MACOS_BUNDLE Stage_a_self-contained_macOS_Homebrew_runtime OFF)

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

# macOS: the dependencies are ASSUMED PRESENT, installed by Homebrew — nothing here builds,
# fetches or vendors VTK/Qt/TBB. `brew --prefix` (not a hard-coded /opt/homebrew) is the only
# honest way to find them: it is /opt/homebrew on Apple Silicon and /usr/local on Intel, and a
# user may have relocated it. The per-formula opt/ paths are prepended as well because Homebrew
# can keep a formula keg-only (unlinked from the top-level prefix), in which case only
# <prefix>/opt/<formula> resolves. An explicit -DCMAKE_PREFIX_PATH still wins over all of it.
if(APPLE AND NOT CMAKE_PREFIX_PATH)
  find_program(GMTVTK_BREW_EXE brew)
  if(GMTVTK_BREW_EXE)
    execute_process(COMMAND "${GMTVTK_BREW_EXE}" --prefix
      OUTPUT_VARIABLE _brew_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _brew_rc)
    if(_brew_rc EQUAL 0 AND IS_DIRECTORY "${_brew_prefix}")
      list(PREPEND CMAKE_PREFIX_PATH
        "${_brew_prefix}"
        "${_brew_prefix}/opt/vtk" "${_brew_prefix}/opt/qtbase"
        "${_brew_prefix}/opt/qttools" "${_brew_prefix}/opt/tbb")
    endif()
  endif()
endif()

# ...and the test that errors if they are NOT installed. find_package(REQUIRED) below does abort
# on its own, but its message names a CMake package ("Could not find a package configuration file
# provided by Qt6"), which tells a macOS user nothing about what to type. QUIET probes first, then
# one FATAL_ERROR naming the missing FORMULAE. The VTK probe asks for GUISupportQt specifically:
# a VTK built WITHOUT Qt is found as a package but fails the component, so this also catches a
# hand-built/conda VTK that cannot host a QVTK widget — the failure that would otherwise surface
# as a wall of link errors.
if(APPLE)
  set(_igmt_missing "")
  find_package(Qt6 QUIET COMPONENTS Widgets UiTools)
  if(NOT Qt6_FOUND)
    list(APPEND _igmt_missing qtbase qttools)
  endif()
  find_package(TBB QUIET CONFIG)
  if(NOT TBB_FOUND)
    list(APPEND _igmt_missing tbb)
  endif()
  find_package(VTK QUIET COMPONENTS GUISupportQt RenderingOpenGL2)
  if(NOT VTK_FOUND)
    list(APPEND _igmt_missing vtk)
  endif()
  if(_igmt_missing)
    list(REMOVE_DUPLICATES _igmt_missing)
    string(JOIN " " _igmt_brew_line ${_igmt_missing})
    message(FATAL_ERROR
      "iGMT needs VTK 9.6+ (built WITH Qt6), Qt6 Widgets + UiTools, and oneTBB. "
      "Missing or unusable: ${_igmt_brew_line}\n"
      "Install them with Homebrew:\n"
      "    brew install ${_igmt_brew_line}\n"
      "(`vtk` already enables Qt6 — the old `--with-qt` option no longer exists. `qttools` "
      "provides Qt6::UiTools, which the vtk formula does not pull in.)\n"
      "Searched: ${CMAKE_PREFIX_PATH}\n"
      "Override with -DCMAKE_PREFIX_PATH=... or -DVTK_DIR=/-DQt6_DIR=/-DTBB_DIR=.")
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
# mbgrid.c is NOT one of those fragments and must NOT be #included into gmtvtk.cpp: it is plain
# C99 + libm, shares no file-static helper with the viewer, includes none of its headers, and has
# to stay compilable and testable on its own (the test_mbgrid target below). So it is a real second
# translation unit in this list. Its gmtvtk-facing exports are listed in src/libgmtvtk.jl's
# _LIB_SYMBOLS like every other export of this DLL.
set(GMTVTK_SRC src/gmtvtk.cpp src/mbgrid.c)

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
  # Ship a PDB even for the optimised build. Costs nothing at run time (the file sits beside the DLL
  # and is never loaded unless something asks), and without it a crash address is unreadable: an
  # intermittent access violation could only be chased through raw disassembly.
  add_compile_options(/Zi)
  add_link_options(/DEBUG /OPT:REF /OPT:ICF)
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

  # Numerics regression for mbgrid.c — pure C, links nothing but libm, runs in well under a second.
  # Built by the default `all` target, same as gmtvtk_demo; run deps/build/test_mbgrid.
  add_executable(test_mbgrid src/mbgrid.c src/test_mbgrid.c)
  if(NOT MSVC)
    target_link_libraries(test_mbgrid PRIVATE m)
  endif()
endif()

# The desktop launcher — deps/src/launcher.c, a tiny standalone executable that has NOTHING to do
# with gmtvtk.dll: it starts julia + iview_app.jl, shows the splash, and creates the desktop icon.
# It must run on a machine where Qt/VTK are missing or broken (that is exactly when the user needs
# a readable error), so it links only OS libraries — never Qt, VTK, TBB or gmtvtk itself.
#
# One binary per OS, replacing the Windows-only iview_app.vbs + make_desktop_shortcut.vbs +
# iview_splash.hta, which had no equivalent on Linux/macOS (so those platforms had no icon at all).
# On macOS it compiles as Objective-C: the splash is Cocoa, and ObjC is a superset of C, so the one
# source file serves all three platforms without a second implementation.
add_executable(igmt src/launcher.c)
if(WIN32)
  set_target_properties(igmt PROPERTIES WIN32_EXECUTABLE TRUE)  # no console flash on double-click
  # shell32/ole32/uuid: IShellLink (the .lnk) + SHGetKnownFolderPath. oleaut32: OleLoadPicture,
  # which is what decodes the splash JPEG without pulling in an image library.
  # msimg32: AlphaBlend, for compositing the splash's corner icon over the nebula with real alpha
  # (the area-averaged bitmap icon_scaled_bitmap builds -- see launcher.c).
  target_link_libraries(igmt PRIVATE shell32 ole32 oleaut32 uuid shlwapi user32 gdi32 msimg32)
elseif(APPLE)
  # OBJC is NOT in project(... LANGUAGES C CXX) — only this one file needs it, and asking for it
  # unconditionally would demand an Objective-C compiler on Windows and Linux, which have none.
  # Without this, setting LANGUAGE OBJC below fails the GENERATE step outright: "The language OBJC
  # was requested for compilation but was not enabled" (broke MacBinaries.yml on 8fcd733).
  enable_language(OBJC)
  set_source_files_properties(src/launcher.c PROPERTIES LANGUAGE OBJC)
  target_link_libraries(igmt PRIVATE "-framework Cocoa" "-framework QuartzCore")
else()
  find_package(X11 REQUIRED)
  target_include_directories(igmt PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(igmt PRIVATE ${X11_LIBRARIES})
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

# ELF ONLY, and it is what makes this library usable at all on Linux.
#
# The twin is the SAME source compiled twice, so both libraries define the same global symbols. On
# Windows that is harmless — a DLL is self-contained and each keeps its own copy. On ELF, global
# symbols are INTERPOSABLE: a call inside libgmtvtk_test.so to any non-static function they share
# binds to whichever library the loader saw FIRST, which is libgmtvtk.so (Julia dlopens it before
# the tests ask for the twin). The test hooks then run against the production library's state
# instead of their own, and read back nonsense — "\b" for a combo's text, unparseable bytes for a
# number, 0 for a hook that plainly succeeded.
#
# -Bsymbolic binds this library's own references to its own definitions at link time, restoring the
# per-library isolation the Windows build gets for free. macOS needs nothing: Mach-O's two-level
# namespace already records which library each symbol came from.
if(UNIX AND NOT APPLE)
  target_link_options(gmtvtk_test PRIVATE "-Wl,-Bsymbolic")
endif()

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
  install(TARGETS igmt RUNTIME DESTINATION .)   # the desktop launcher — see add_executable(igmt)
  list(GET CMAKE_PREFIX_PATH 0 GMTVTK_LINUX_CONDA_PREFIX)
  configure_file(cmake/linux_bundle.cmake.in linux_bundle.cmake @ONLY)
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/linux_bundle.cmake)
endif()

# macOS runtime staging -- same job as GMTVTK_LINUX_BUNDLE, same division of labour (this stages,
# deps/build_mac.sh rewrites install names + codesigns), different loader vocabulary: @loader_path
# where Linux says $ORIGIN. Turned on by deps/build_mac.sh and by .github/workflows/MacBinaries.yml;
# a plain dev build on a Mac wants none of it (it links straight against Homebrew's own dylibs).
if(GMTVTK_MACOS_BUNDLE)
  if(NOT APPLE)
    message(FATAL_ERROR GMTVTK_MACOS_BUNDLE_is_macOS-only)
  endif()
  if(NOT CMAKE_PREFIX_PATH)
    message(FATAL_ERROR GMTVTK_MACOS_BUNDLE_needs_CMAKE_PREFIX_PATH)
  endif()
  # INSTALL_NAME_DIR, not INSTALL_RPATH: a dylib's own LC_ID_DYLIB is what every OTHER file in the
  # bundle records as the path to it, so leaving it at the build-tree default would bake an absolute
  # path into the plugins. @loader_path here means "beside whoever loaded me", which is exactly the
  # flat bundle layout. build_mac.sh re-asserts it after its rewrite pass, since install_name_tool
  # is what has the last word.
  set_target_properties(gmtvtk PROPERTIES INSTALL_NAME_DIR "@loader_path" BUILD_WITH_INSTALL_NAME_DIR ON)
  install(TARGETS gmtvtk LIBRARY DESTINATION .)
  install(TARGETS igmt RUNTIME DESTINATION .)   # the desktop launcher — see add_executable(igmt)
  list(GET CMAKE_PREFIX_PATH 0 GMTVTK_MACOS_PREFIX)
  configure_file(cmake/macos_bundle.cmake.in macos_bundle.cmake @ONLY)
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/macos_bundle.cmake)
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
# destination machine. The installed launcher (deps/build/igmt.exe, given the
# install root as --root) assumes Julia + this package's deps (GMT.jl, ...) are already
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

  # The launcher rides in the small zip TOO. It is not a VTK module (see the note below about
  # never smuggling those in here) — it links no bundled library at all, so it cannot desync from
  # the pinned runtime. It has to be here because a machine that already has the full runtime only
  # ever fetches this zip: leaving igmt.exe out of it would mean no desktop icon until the next
  # RUNTIME_VERSION bump.
  install(FILES "$<TARGET_FILE:igmt>" DESTINATION deps/build COMPONENT dll)

  # The TEST TWIN rides in the small zip as well, and ONLY in the small zip: it is the sole home of
  # the gmtvtk_*_test API, and a machine that did not build this repo cannot produce it (that needs
  # the Qt/VTK SDK, not the runtime bundle). Without it every test asserting through that API fails
  # as "not found" on any CI runner, which is why the GUI tier could never be run there. Deliberately
  # NOT in COMPONENT full — the runtime bundle is what a user installs, and the test API has no
  # business on a user's machine.
  install(FILES "$<TARGET_FILE:gmtvtk_test>" DESTINATION deps/build COMPONENT dll)

  # ...and NOTHING ELSE: the dll-only zip is gmtvtk.dll, the launcher and the test twin.
  #
  # It used to carry a `_GMTVTK_DLL_EXTRA_MODULES` list as well — any VTK module the PINNED full
  # bundle predated. That existed because the two zips move at different speeds (deps/build.jl):
  # link one module the published iGMT-win64-full.zip does not contain and every existing install
  # gets a DLL that LoadLibrary cannot load at all, reported by Julia as a missing gmtvtk_* symbol
  # naming nothing useful. Found 2026-07-29 — vtkHDFReader pulled in vtkIOHDF + vtkhdf5 +
  # vtkhdf5_hl + vtkFiltersTemporal, none of them in runtime-0.1.
  #
  # Dropped 2026-08-26: deps/RUNTIME_VERSION now pins runtime-0.2, whose bundle carries all four,
  # and .full_runtime_installed stores the installed TAG so a pin bump actually re-fetches on
  # machines that already have a runtime. The entries were duplicates of files the target machine
  # already has, exactly the condition this block's own note said to remove it under. Verified by
  # deps/check_dll_deps.ps1 against the published runtime-0.2 bundle with NO dll zip in the
  # baseline: 81 modules walked, remainder empty.
  #
  # If the C++ ever links a module a published bundle lacks, the fix is a new full bundle + a
  # RUNTIME_VERSION bump, not a module smuggled into this component. Run check_dll_deps.ps1 before
  # every upload — that is what tells the two apart.

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
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/solar_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdfft_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdhisteq_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/xyz2grd_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdfill_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/gravfft_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdrotater_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/talwani2d_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/talwani3d_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/greenspline_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/gmtflexure_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdflexure_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdvolume_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/gravprisms_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/grdvector_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/earthregions_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/trend2d_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/cpt_build_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/project_dialog.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/euler_stuff.ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/plate_calculator.ui"
    DESTINATION deps/ui COMPONENT full)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../data/" DESTINATION data COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../Project.toml"        DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../iview_app.jl"        DESTINATION . COMPONENT full)
  # Splash background, both encodings: the .jpg is what Win32/Cocoa decode, the .bmp is what the
  # X11 splash reads (Xlib decodes no image format). Shipped together so one installed tree serves
  # whichever platform's launcher is looking, and so a git checkout and an installed tree behave
  # the same — splash_image_path() looks here first, then in deps/assets.
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/crystalball.jpg" DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/crystalball.bmp" DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../iview_splash.hta"    DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/installer/iview_launch.vbs" DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/igmt.ico"        DESTINATION . COMPONENT full)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.png"    DESTINATION . COMPONENT full)  # Linux .desktop icon
  # The launcher itself, beside gmtvtk.dll. `igmt --install-shortcut` then makes the desktop icon
  # on whatever OS this tree was installed on; the NSIS shortcuts below point straight at it.
  install(TARGETS igmt RUNTIME DESTINATION deps/build COMPONENT full)

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
  # deps/src/_app_icon.h). Raw NSIS script (CPACK_NSIS_EXTRA_*_COMMANDS) rather
  # than CPACK_PACKAGE_EXECUTABLES/CPACK_NSIS_MENU_LINKS so the shortcut can name
  # the icon file explicitly.
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
    CreateShortCut \\\"$DESKTOP/iGMT.lnk\\\" \\\"$INSTDIR/deps/build/igmt.exe\\\" '--root=\\\"$INSTDIR\\\"' \\\"$INSTDIR/igmt.ico\\\"
    CreateDirectory \\\"$SMPROGRAMS/iGMT\\\"
    CreateShortCut \\\"$SMPROGRAMS/iGMT/iGMT.lnk\\\" \\\"$INSTDIR/deps/build/igmt.exe\\\" '--root=\\\"$INSTDIR\\\"' \\\"$INSTDIR/igmt.ico\\\"
  ")
  set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    Delete \\\"$DESKTOP/iGMT.lnk\\\"
    Delete \\\"$SMPROGRAMS/iGMT/iGMT.lnk\\\"
    RMDir \\\"$SMPROGRAMS/iGMT\\\"
  ")
  include(CPack)
endif()
