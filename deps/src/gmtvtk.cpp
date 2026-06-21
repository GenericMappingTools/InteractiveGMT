// gmtvtk.cpp — umbrella translation unit for the Qt6 + VTK 3-D viewer (InteractiveGMT).
//
// The viewer source is split for readability into the NN_*.cpp fragments #included below, in
// order. They are NOT independent translation units: CMake compiles ONLY this gmtvtk.cpp (the
// fragments are #included textually, not in the source list), so every `static` helper and shared
// struct keeps internal linkage exactly as when it all lived in one main.cpp. They carry the .cpp
// extension purely so IDEs give them C++ language support; never add them to the CMake sources.
// Edit a fragment, rebuild this one TU (deps/build.bat).
//
//   00_includes    Qt/VTK headers + file banner
//   10_geometry    Overlay/Curtain/Scene structs, grid/cloud/fv builders, axis labels, VE, picking
//   20_gizmo       Fledermaus-style scale/tilt/azimuth handle
//   30_app         QApplication globals, ensureApp, middle-button pan/recenter
//   40_shading     sky env + applyShading (light rig, SSAO/tone/FXAA)
//   50_scene       Scene Objects panel, overlays, curtains, line styles, context menus
//   55_lineprops   shared Line Properties dialog + unified line right-click menu
//   60_profile     ProfilePanel (2D s,z) + profile sampling + GLView
//   65_xyplot      standalone X,Y plot tool (vtkChartXY + Object Manager + Data Viewer)
//   70_window      buildAndShow — the Qt main window
//   80_rubberband  Ctrl+right-drag point-cloud selection
//   85_polygon     toolbar polygon draw/edit tool (3-D vertices draped on the relief)
//   90_c_api       extern "C" gmtvtk_* exports (the host C API) + demo main()
#include "00_includes.cpp"
#include "10_geometry.cpp"
#include "20_gizmo.cpp"
#include "30_app.cpp"
#include "40_shading.cpp"
#include "50_scene.cpp"
#include "55_lineprops.cpp"
#include "60_profile.cpp"
#include "65_xyplot.cpp"
#include "70_window.cpp"
#include "80_rubberband.cpp"
#include "85_polygon.cpp"
#include "90_c_api.cpp"
