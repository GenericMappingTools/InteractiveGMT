// ============================================================================
//  mb3dsoundings_window.h -- the 3-D sounding editor of mbeditviz (MB-System's libmbview
//  mb3dsoundings, rebuilt on Qt + VTK).
//
//  The soundings an mbeditviz selection picked (struct mb3dsoundings_struct, filled by the engine)
//  are shown as a point cloud that rotates, pans, zooms and exaggerates as mb3dsoundings did, and
//  are flagged and unflagged with the same six edit modes, the same key macros, the same view and
//  action menus and the same five patch-test sliders (roll, pitch, heading, time lag, Snell). The
//  edit logic (pick, erase/restore, grab, flag/unflag view, the ping macros) is mb3dsoundings_
//  callbacks.c's, in window pixels, unchanged: only the drawing is VTK's.
//
//  Every result goes back through the notify functions mbeditviz registers, exactly as with
//  libmbview (mb3dsoundings_set_*_notify).
// ============================================================================

#ifndef MB3DSOUNDINGS_WINDOW_H_
#define MB3DSOUNDINGS_WINDOW_H_

#include <QIcon>
#include <QString>

#include "../mbedit/mbedit_window.h"

class QWidget;
struct mb3dsoundings_struct;

struct Mb3dsdgNotify {
	void (*dismiss)() = nullptr;
	void (*edit)(int ifile, int iping, int ibeam, char beamflag, int flush) = nullptr;
	void (*info)(int ifile, int iping, int ibeam, char *infostring) = nullptr;
	void (*bias)(double rollbias, double pitchbias, double headingbias, double timelag, double snell) = nullptr;
	void (*biasapply)(double rollbias, double pitchbias, double headingbias, double timelag, double snell) = nullptr;
	void (*flagsparsevoxels)(int sizemultiplier, int nsoundingthreshold) = nullptr;
	void (*colorsoundings)(int color) = nullptr;
	void (*optimizebiasvalues)(int mode, double *rollbias, double *pitchbias, double *headingbias, double *timelag,
	                           double *snell) = nullptr;
	void (*save)() = nullptr;   // the pane's Save button: write the edits so far (null: no button)
	void (*cube)(bool filter) = nullptr;   // the pane's CUBE filter button (true: Flag soundings preset)
	void (*grid)() = nullptr;              // the pane's Gridding button: grid the pane's good soundings
	void (*discard)() = nullptr;           // an area pane's Discard button (its Save is then "Accept flags")
};

// mb3dsoundings_open: show `data` (raise the window, or make it). `host` is mbeditviz's: its uiDir
// holds mb3dsoundings.ui, and the window parks through it (mbParkable) like every MB-System tool.
bool mb3dsdgOpen(QWidget *parent, const MbEditHost &host, mb3dsoundings_struct *data, const Mb3dsdgNotify &notify);
// The SAME editor as a narrow right-side PANE of the iGMT window `scene` (the 3D Soundings pane of a
// swath point cloud): its soundings are drawn into that window's own cloud actor, its edit modes take
// that window's left button only while armed ("Navigate" = none), View is the window's View menu,
// Action its Action menu without Apply Bias and the Optimize entries. Closing the pane dismisses it
// (notify.dismiss). One editor at a time: an open 3-D soundings window or pane is ended first.
bool mb3dsdgOpenPane(void *scene, const MbEditHost &host, mb3dsoundings_struct *data, const Mb3dsdgNotify &notify);
// The open pane as an AREA pane (a line area's or a track's "Show point-cloud"): Save becomes "Accept
// flags" and Discard shows. CUBE filter / Gridding stay, acting on this pane's own soundings.
void mb3dsdgSetAreaMode();
// The pane's Navigation toggle calls `show` (the host's visibility setter for the window's navigation
// lines, ordinary line elements of the window); its box starts at `on`.
void mb3dsdgSetNavToggle(std::function<void(bool)> show, bool on);
// mb3dsoundings_end: close the window without notifying (the caller is tearing down).
void mb3dsdgEnd();
// mb3dsoundings_plot: redraw (the soundings were recoloured or moved by the caller).
void mb3dsdgPlot();
bool mb3dsdgIsOpen();
// mb3dsoundings_get_bias_values
void mb3dsdgGetBiasValues(double *rollbias, double *pitchbias, double *headingbias, double *timelag, double *snell);

// For the host's C API and tests: the window's canvas size, a press-drag-release of the left
// (edit) button in window pixels (y down, as Qt counts), and a key typed on the canvas.
bool mb3dsdgCanvasSize(int *w, int *h);
bool mb3dsdgMouseEdit(int x0, int y0, int x1, int y1);
bool mb3dsdgKey(int ch);
// what the view draws: the number of points the soundings actor renders, and the good / flagged counts
bool mb3dsdgCounts(int *drawn, int *good, int *flagged);
// View > Show flagged, through the menu entry itself
bool mb3dsdgSetShowFlagged(bool on);
// The open pane's buttons that are shown: bit 0 CUBE filter, bit 1 Gridding, bit 2 Discard; -1 = no pane
int mb3dsdgPaneButtons();
// one of the six edit modes (MB3DSDG_MOUSE_TOGGLE .. _INFO)
bool mb3dsdgSetEditMode(int mode);
// the window-pixel position (y down) of sounding i as last drawn
bool mb3dsdgSoundingPixel(int i, int *x, int *y);
bool mb3dsdgSavePng(const QString &path);

#endif // MB3DSOUNDINGS_WINDOW_H_
