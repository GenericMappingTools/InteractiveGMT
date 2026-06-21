cmake_minimum_required(VERSION 3.21)  # 3.21+ for install(... RUNTIME_DEPENDENCIES ...)
project(gmtvtk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Toolchain locations (this machine, verified 2026-06-11). Override on the cmake command line
# (-DVTK_DIR=... etc.) for a different box.
if(NOT VTK_DIR)
  set(VTK_DIR "C:/programs/compa_libs/VTK-9.6.2/compileds/lib/cmake/vtk")
endif()
if(NOT Qt6_DIR)
  set(Qt6_DIR "C:/programs/Qt6/6.11.1/msvc2022_64/lib/cmake/Qt6")
endif()
if(NOT TBB_DIR)
  set(TBB_DIR "C:/programs/compa_libs/tbb_GIT/compileds/VC14_64/lib/cmake/TBB")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(TBB REQUIRED CONFIG)
find_package(VTK REQUIRED COMPONENTS
  CommonCore CommonDataModel CommonColor CommonTransforms
  FiltersSources FiltersGeneral
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

# Shared library — the Julia/host bridge (C API: gmtvtk_view_grid / gmtvtk_view_demo / ...).
add_library(gmtvtk SHARED ${GMTVTK_SRC})
target_link_libraries(gmtvtk PRIVATE Qt6::Widgets TBB::tbb ${VTK_LIBRARIES})
vtk_module_autoinit(TARGETS gmtvtk MODULES ${VTK_LIBRARIES})

# Standalone demo executable (shows the synthetic peaks surface via the demo main()).
add_executable(gmtvtk_demo WIN32 ${GMTVTK_SRC})
target_link_libraries(gmtvtk_demo PRIVATE Qt6::Widgets TBB::tbb ${VTK_LIBRARIES})
vtk_module_autoinit(TARGETS gmtvtk_demo MODULES ${VTK_LIBRARIES})

# ============================================================================
# Optional redistributable package — gmtvtk.dll + demo + EVERY runtime DLL it
# needs (VTK, Qt, TBB) + the Qt platform plugins, so the package runs on a box
# WITHOUT VTK/Qt installed. OFF by default (a normal build wants none of this).
#
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGMTVTK_PACKAGE=ON
#   cmake --build build
#   cmake --build build --target package      # -> gmtvtk-0.1.0-win64.zip
#
# How the DLLs are gathered:
#   * VTK + TBB + Qt6 core DLLs: install(... RUNTIME_DEPENDENCIES ...) scans the
#     built binaries for their imports and resolves them from the toolchain bins.
#   * Qt *plugins* (platforms/qwindows.dll, styles, imageformats) are loaded at
#     RUNTIME, not import-linked, so the scanner cannot see them -> windeployqt.
# ============================================================================
option(GMTVTK_PACKAGE "Bundle all runtime DLLs and enable CPack packaging" OFF)

if(GMTVTK_PACKAGE)
  # Derive the toolchain runtime-DLL dirs from the *_DIR cmake-config locations
  # (each is <root>/lib/cmake/<pkg>, so the bin dir is three levels up + /bin).
  get_filename_component(_VTK_ROOT "${VTK_DIR}/../../.." ABSOLUTE)
  get_filename_component(_QT_ROOT  "${Qt6_DIR}/../../.." ABSOLUTE)
  get_filename_component(_TBB_ROOT "${TBB_DIR}/../../.." ABSOLUTE)
  set(_VTK_BIN "${_VTK_ROOT}/bin")
  set(_QT_BIN  "${_QT_ROOT}/bin")
  set(_TBB_BIN "${_TBB_ROOT}/bin")

  # Install the two project binaries, and have CMake resolve + install their
  # dependent DLLs into the same bin dir. System/Windows DLLs are excluded.
  install(TARGETS gmtvtk gmtvtk_demo
    RUNTIME_DEPENDENCY_SET gmtvtk_deps
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION lib)

  install(RUNTIME_DEPENDENCY_SET gmtvtk_deps
    PRE_EXCLUDE_REGEXES
      "api-ms-.*" "ext-ms-.*" "^hvsifiletrust.dll$" "^pdmutilities.dll$"
    POST_EXCLUDE_REGEXES
      ".*[\\/][Ss]ystem32[\\/].*\\.dll$" ".*[\\/]WINDOWS[\\/].*"
    DIRECTORIES "${_VTK_BIN}" "${_QT_BIN}" "${_TBB_BIN}"
    DESTINATION bin)

  # Qt runtime plugins (not import-linked) — let windeployqt drop them next to
  # the installed demo exe. Runs at `cmake --install` / package time.
  find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_QT_BIN}")
  if(WINDEPLOYQT_EXE)
    install(CODE "
      message(STATUS \"windeployqt: staging Qt plugins into bin\")
      execute_process(COMMAND \"${WINDEPLOYQT_EXE}\"
        --release --no-translations --no-compiler-runtime
        --dir \"\${CMAKE_INSTALL_PREFIX}/bin\"
        \"\${CMAKE_INSTALL_PREFIX}/bin/gmtvtk_demo.exe\")
    ")
  else()
    message(WARNING "windeployqt not found in ${_QT_BIN}; Qt platform plugins will be MISSING from the package")
  endif()

  # CPack — a flat ZIP (gmtvtk-<version>-win64.zip) with everything under bin/.
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_NAME "gmtvtk")
  set(CPACK_PACKAGE_VENDOR "InteractiveGMT")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_SYSTEM_NAME "win64")
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Self-contained Qt6 + VTK 3-D viewer for GMT.jl (bundled runtime)")
  include(CPack)
endif()
