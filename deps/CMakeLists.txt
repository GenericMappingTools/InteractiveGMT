cmake_minimum_required(VERSION 3.16)
project(gmtvtk LANGUAGES CXX)

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
