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
};

// mb3dsoundings_open: show `data` (raise the window, or make it); `uiDir` holds mb3dsoundings.ui.
bool mb3dsdgOpen(QWidget *parent, const QString &uiDir, const QIcon &icon, mb3dsoundings_struct *data,
                 const Mb3dsdgNotify &notify);
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
// one of the six edit modes (MB3DSDG_MOUSE_TOGGLE .. _INFO)
bool mb3dsdgSetEditMode(int mode);
// the window-pixel position (y down) of sounding i as last drawn
bool mb3dsdgSoundingPixel(int i, int *x, int *y);
bool mb3dsdgSavePng(const QString &path);

#endif // MB3DSOUNDINGS_WINDOW_H_
