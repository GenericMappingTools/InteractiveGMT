// ============================================================================
//  mbnavadjust_window.h -- the ONE entry point of the navigation adjustment tool (MB-System's
//  mbnavadjust, ported) for the rest of the viewer.
//
//  The tool lives in its own folder, deps/src/mbnavadjust/, apart from the viewer's code:
//    mbnavadjust_io.c, _autopick.c, _invertnav.c   MB-System's GUI-free core, as it is
//    mbnavadjust.c                                 the engine (mbnavadjust_prog.c), plain C
//    mbnavadjust_callbacks.c                       the Motif callbacks, plain C, on the widget
//                                                  bridge of mbna_motif.h
//    mbna_aux.c, mbna_grid.c, mbna_mbio.c          libmbaux's pieces, the grid reader, MBIO
//    mbnavadjust_window.cpp                        the Qt window: the bridge's other side
//    deps/ui/mbnavadjust*.ui                       its window and dialogs, loaded at run time
//  It reads swath files through the swath editor's MBIO loader (deps/src/mbedit/), so it is built
//  only together with the editor.
// ============================================================================

#ifndef MBNAVADJUST_WINDOW_H_
#define MBNAVADJUST_WINDOW_H_

#include <QString>

#include <functional>
#include <string>

#include "../mbedit/mbedit_window.h"
#include "mbna_grid.h"

class QWidget;

// What the tool needs of the viewer beyond the swath editor's host. A "win" is a viewer window.
struct MbNavAdjustHost {
	MbEditHost base;
	bool (*alive)(void *win) = nullptr;
	QWidget *(*window)(void *win) = nullptr;
	// open a file into a NEW viewer window through the viewer's one file-open door; the window, or null
	void *(*openFile)(const char *path, const char *title) = nullptr;
	// a polyline (lon, lat, z placeholder) as a named line of `win`, under the Scene Objects group `group`
	bool (*addLine)(void *win, const double *xyz, int npts, double r, double g, double b, double width, const char *name,
	                const char *group) = nullptr;
	// remove every line of group `group`
	bool (*removeGroup)(void *win, const char *group) = nullptr;
	// the pen of the line called `name`
	bool (*setLineStyle)(void *win, const char *name, double r, double g, double b, double width) = nullptr;
	// arm the window's click pick on lines: each click answers with the clicked line's name ("" = none);
	// a null `cb` disarms
	bool (*pickLine)(void *win, std::function<void(const std::string &)> cb) = nullptr;
	void (*render)(void *win) = nullptr;
	// run an MB-System program command line as upstream's system() does ("cd dir ; prog args", a
	// script of such lines, chmod); the exit status
	int (*run)(const char *command) = nullptr;
	// read a grid file, as mbna_grid.h describes
	mbna_grid_reader_fn readGrid = nullptr;
};

// Open the tool, or raise it if it is already open (the engine is a single instance: its state is
// file-static, as in mbnavadjust). With a `project` (.nvh) it is opened at once, as
// `mbnavadjust -I project` does. Returns false when the tool could not be opened.
bool mbnavadjustOpenWindow(QWidget *parent, const MbNavAdjustHost &host, const QString &project = QString());

// Drive / read the open tool (the host's C API and tests). All are no-ops without the tool.
//   state: [open, project_open, num_files, num_surveys, num_crossings, num_crossings_analyzed,
//           num_truecrossings, num_ties, num_globalties, inversion_status, grid_status, naverr_mode,
//           view_list, crossing_select, tie_select, modelplot, visualization_status, list_count]
int mbnavadjustState(int *out, int n);
// a click on the window's or a dialog's button / toggle / menu entry of that object name (the Motif
// widget names of mbnavadjust.uil: "pushButton_analyzecrossings", ...)
bool mbnavadjustPress(const QString &name);
// File > New / Open / Import ... without the file dialog: `mode` is the file selection mode
// (1 new, 2 open, 3 import data, 4 import as one survey, 5 reference grid), `format` the MBIO format
// id of an import (0: from the file name)
bool mbnavadjustFileSelect(int mode, const QString &path, int format);
// select row `row` (0-based) of the list, as a click on it
bool mbnavadjustListSelect(int row);
// the text of row `row` of the list ("" past the end)
QString mbnavadjustListItem(int row);
// a mouse action on a drawing area (0 contours, 1 misfit, 2 z offset, 3 modelplot): button 1..3
// pressed at (x0,y0), dragged to (x1,y1) and released
bool mbnavadjustMouse(int canvas, int button, int x0, int y0, int x1, int y1);
// a drawing area as drawn (0..3 as above)
bool mbnavadjustSavePng(int canvas, const QString &path);
// the message log
QString mbnavadjustMessages();
bool mbnavadjustClose();                         // Quit: save the project, close

#endif // MBNAVADJUST_WINDOW_H_
