// ============================================================================
//  mbvelocity_window.h -- the ONE entry point of the water sound velocity profile editor
//  (MB-System's mbvelocitytool, ported) for the rest of the viewer.
//
//  The tool lives in its own folder, deps/src/mbvelocitytool/, apart from the viewer's code:
//    mbvelocity.c / mbvelocity.h     the engine (mbvelocity_prog.c), plain C, own TU
//    mbvelocity_window.cpp           the Qt window (mbvelocity_callbacks.c), own TU
//    deps/ui/mbvelocitytool*.ui      its window and dialogs, loaded at run time
//  It reads swath files through the swath editor's MBIO loader (deps/src/mbedit/), so it is built
//  only together with the editor, and is handed the same MbEditHost the editor gets.
// ============================================================================

#ifndef MBVELOCITY_WINDOW_H_
#define MBVELOCITY_WINDOW_H_

#include <QString>

#include "../mbedit/mbedit_window.h"

class QWidget;

// Open the tool, or raise it if it is already open (the engine is a single instance: its state is
// file-static, as in mbvelocitytool). The files are mbvelocitytool's command line:
//   swathFile  -I (with `format`, -F; 0 guesses it from the name)   editSvp  -W   displaySvp  -S
// Each that is given is loaded at once, in the order mbvelocitytool loads them. Returns false
// when the tool could not be opened.
bool mbvelocityOpenWindow(QWidget *parent, const MbEditHost &host, const QString &swathFile = QString(), int format = 0,
                          const QString &editSvp = QString(), const QString &displaySvp = QString());

// Drive / read the open tool (the host's C API and tests). All are no-ops without the tool.
//   state: [open, edit, nedit, ndisplay, nbuffer, nbeams_with_residuals, canvas_width, canvas_height]
int mbvelocityState(int *out, int n);
// The canvas's mouse, as mbvelocitytool reads it: button 1 drags the nearest node from (x0,y0) to
// (x1,y1), button 2 adds a node at (x0,y0), button 3 deletes the node at (x0,y0).
bool mbvelocityMouse(int button, int x0, int y0, int x1, int y1);
bool mbvelocityEditNode(int i, double *depth, double *velocity);
bool mbvelocityReprocess();                          // the Reprocess button
bool mbvelocitySaveSwathSvp();                       // File > Save swath svp file
bool mbvelocitySaveResiduals();                      // File > Save residuals as offsets
bool mbvelocitySavePng(const QString &path);         // the canvas as drawn
bool mbvelocityClose();                              // Quit

#endif // MBVELOCITY_WINDOW_H_
