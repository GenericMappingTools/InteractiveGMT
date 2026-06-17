// gmtvtk.cpp — umbrella translation unit for the Qt6 + VTK 3-D viewer (InteractiveGMT).
//
// The viewer source is split for readability into the *.inc fragments #included below, in
// order. They are NOT independent translation units: this single .cpp is the only file
// CMake compiles, so every `static` helper and shared struct keeps internal linkage exactly
// as when it all lived in one main.cpp. Edit a fragment, rebuild this one TU (deps/build.bat).
//
//   00_includes    Qt/VTK headers + file banner
//   10_geometry    Overlay/Curtain/Scene structs, grid/cloud/fv builders, axis labels, VE, picking
//   20_gizmo       Fledermaus-style scale/tilt/azimuth handle
//   30_app         QApplication globals, ensureApp, middle-button pan/recenter
//   40_shading     sky env + applyShading (light rig, SSAO/tone/FXAA)
//   50_scene       Scene Objects panel, overlays, curtains, line styles, context menus
//   60_profile     ProfilePanel (2D s,z) + profile sampling + GLView
//   70_window      buildAndShow — the Qt main window
//   80_rubberband  Ctrl+right-drag point-cloud selection
//   90_c_api       extern "C" gmtvtk_* exports (the host C API) + demo main()
#include "00_includes.inc"
#include "10_geometry.inc"
#include "20_gizmo.inc"
#include "30_app.inc"
#include "40_shading.inc"
#include "50_scene.inc"
#include "60_profile.inc"
#include "70_window.inc"
#include "80_rubberband.inc"
#include "90_c_api.inc"
