// ============================================================================
//  mbnavedit_window.h -- the ONE entry point of the interactive navigation editor (MB-System's
//  mbnavedit, ported) for the rest of the viewer.
//
//  The tool lives in its own folder, deps/src/mbnavedit/, apart from the viewer's code:
//    mbnavedit.c / mbnavedit.h      the engine (mbnavedit_prog.c), plain C, own TU
//    mbnavedit_window.cpp           the Qt window (mbnavedit_callbacks.c), own TU
//    deps/ui/mbnavedit*.ui          its window and dialogs, loaded at run time
//  It reads swath files through the swath editor's MBIO loader (deps/src/mbedit/), so it is built
//  only together with the editor, and is handed the same MbEditHost the editor gets.
// ============================================================================

#ifndef MBNAVEDIT_WINDOW_H_
#define MBNAVEDIT_WINDOW_H_

#include <QString>
#include <QStringList>

#include <vector>

#include "../mbedit/mbedit_window.h"

class QWidget;

// Open the editor, or raise it if it is already open (the engine is a single instance: its state
// is file-static, as in mbnavedit). `files` (each a swath file or a datalist, `formats` their format
// ids, 0 = guessed from the name, -1 = a datalist) are mbnavedit's -I/-F pairs: they join the file
// list and the first of them is loaded at once. The switches are mbnavedit's -D (browse: nothing is
// written), -X (run mbprocess when a file is done), -P (navigation from the survey pings) and -N
// (mbprocess strips comments); -1 leaves one as it is. `usePrevious` decides about an existing
// edited navigation (.nve) of the file loaded: -1 ask (mbnavedit's dialog), 0 ignore it, 1 use it.
// Returns false when the editor could not be opened.
bool mbnaveditOpenWindow(QWidget *parent, const MbEditHost &host, const QStringList &files = QStringList(),
                         const std::vector<int> &formats = std::vector<int>(), int browse = -1, int runMbprocess = -1,
                         int usePingData = -1, int stripComments = -1, int usePrevious = -1);

// Drive / read the open editor (the host's C API and tests). All are no-ops without an editor.
//   state: [open, file_open, numfiles, currentfile, nbuff, current_id, nplot, nload_total,
//           ndump_total, number_plots, model_mode, mode_pick, canvas_width, canvas_height]
int mbnaveditState(int *out, int n);
// record i of the buffer: [time_d, lon, lat, speed, heading, sensor depth]; `selected` bits 0..5 =
// the time interval, lon, lat, speed, heading and sensor depth plots have it selected
bool mbnaveditRecord(int i, double *out6, int *selected);
// plot iplot's box on the canvas: [ixmin, ixmax, iymin, iymax, type]
bool mbnaveditPlotBox(int iplot, int *out5);
// the canvas pixel record i is drawn at in plot iplot
bool mbnaveditRecordXY(int iplot, int i, int *x, int *y);
// The canvas's mouse, as mbnavedit reads it: button 1 pressed at (x0,y0), dragged to (x1,y1) and
// released; buttons 2 and 3 pressed and released at (x0,y0).
bool mbnaveditMouse(int button, int x0, int y0, int x1, int y1);
bool mbnaveditKey(int ch);                       // a key on the canvas, as typed
// a click on the window's or a dialog's button / toggle / menu entry of that object name (the
// Motif widget names of mbnavedit.ui and its dialogs: "pushButton_interpolate", ...)
bool mbnaveditPress(const QString &name);
bool mbnaveditSavePng(const QString &path);      // the canvas as drawn
bool mbnaveditClose();                           // Quit: finish the file being edited, close

#endif // MBNAVEDIT_WINDOW_H_
