// ============================================================================
//  pce_window.h -- the ONE entry point of the point cloud editor (MB-System's pointCloudEditor,
//  src/pointCloudEditor/ + the pieces of src/qt-guilib/ it uses, ported) for the rest of the viewer.
//
//  The tool lives in its own folder, deps/src/pointcloudeditor/:
//    pce_window.cpp            PointCloudEditor, PointsSelectInteractorStyle, ZScaleCallback,
//                              EditModeGroup / RadioButtonGroup and TopoDataReader's grid-to-surface
//                              step, in one Qt window holding the original's own VTK scene
//    deps/ui/pointcloudeditor.ui   that window (a menu bar around the VTK view)
//
//  The original reads a GMT grid (.grd, through the GMT API) or a swath file (.mb*, gridded by
//  mbeditviz's engine). Here a grid is the grid of an InteractiveGMT window (opened through the
//  viewer's file door when it comes from File > Open), and a swath file is gridded by the ported
//  mbeditviz engine (deps/src/mbeditviz/), in browse mode. Geographic grids are shown in UTM, as the
//  original does, through MBIO's projection (deps/src/mbgrdviz/'s glue). Like the original it
//  writes nothing: Erase and Restore only recolour the points.
// ============================================================================

#ifndef PCE_WINDOW_H_
#define PCE_WINDOW_H_

#include <QString>

#include "../mbedit/mbedit_window.h"
#include "../mbgrdviz/mbgrdviz_window.h"

class QWidget;

struct PceHost {
	MbEditHost base;
	bool (*alive)(void *win) = nullptr;
	// the window's grid (mbgrdviz's door: z column-major from the south-west node, NaN = no data)
	bool (*grid)(void *win, MbGrdVizGrid &g) = nullptr;
	// open a file through the viewer's ONE file door into a new window; that window, or null
	void *(*openFile)(void *win, const char *path) = nullptr;
};

// Open the editor (or raise it) on the grid of window `win`, or on `file` (a grid file, opened in a
// window of its own, or a swath file .mb*, gridded); `elevProfile` is the original's -elev. With
// neither, the editor opens empty and File > Open is offered. False when it could not be opened.
bool pceOpenWindow(QWidget *parent, const PceHost &host, void *win = nullptr, const QString &file = QString(),
                   bool elevProfile = false);

// Drive / read the open editor (the host's C API and tests). All are no-ops without the editor.
//   state: [open, npoints, ncells, nbad, editmode (0 erase, 1 restore), selecting (rubber band on),
//           elevprofile mode, nprofile (points in the last elevation profile)]
int pceState(int *out, int n);
bool pceSetEditMode(int mode);                         // 0 ERASE, 1 RESTORE (the radio buttons)
bool pceSetVerticalExagg(double value);                // the slider
// a rubber band from (x0,y0) to (x1,y1), in window pixels from the top left as Qt counts, released
// in select mode: the selection (or, in elevation profile mode, the profile line)
bool pceRubberBand(int x0, int y0, int x1, int y1);
bool pceSetElevProfile(bool on);
bool pceCanvasSize(int *w, int *h);
bool pceSavePng(const QString &path);
bool pceClose();

#endif // PCE_WINDOW_H_
