cmake_minimum_required(VERSION 3.21)
project(chart_spike VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
# The chart path: ChartsCore + the 2D context render stack + Qt support.
find_package(VTK REQUIRED COMPONENTS
  CommonCore CommonDataModel CommonColor
  RenderingCore RenderingOpenGL2 RenderingContext2D RenderingContextOpenGL2
  ChartsCore ViewsContext2D
  GUISupportQt)

add_executable(chart_spike WIN32 chart_spike.cpp)
target_link_libraries(chart_spike PRIVATE Qt6::Widgets TBB::tbb ${VTK_LIBRARIES})
vtk_module_autoinit(TARGETS chart_spike MODULES ${VTK_LIBRARIES})
