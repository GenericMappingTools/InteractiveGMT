// ============================================================================
//  wcd_window.h -- the ONE entry point of the water column viewer (a port of kmwcd_viewer.py,
//  Christian dos Santos Ferreira, MARUM / MB-System, github.com/cdsferreira/wcd_viewer) for the rest of
//  the viewer.
//
//  The tool lives in its own folder, deps/src/wcdviewer/, apart from the viewer's code:
//    wcd_window.cpp                 the Qt window: the plot, the controls, the picks table
//    deps/ui/wcdviewer*.ui          its window and the picks dialog, loaded at run time
//  Everything that is not the window -- reading the Kongsberg KMALL datagrams (#MWC water column,
//  #SPO position, #SKM heading, #MRZ bottom detections), the ping geometry, the navigation and the
//  pick -- is Julia's (src/wcdviewer.jl), reached through the callbacks in WcdHost.
// ============================================================================

#ifndef WCD_WINDOW_H_
#define WCD_WINDOW_H_

#include <QString>

#include "../mbedit/mbedit_window.h"

class QWidget;

struct WcdHost {
	MbEditHost base;                     // ui dir, icon, busy notice, file dialogs' folder, parking
	// open a file: info = [npings, ampmin, ampmax, depthmax, ncolour, cz(n), rgb(3n)] (the colour
	// scale over 0..1); 0 + a message on failure
	int (*open)(const char *path, double *info, int cap, char *err, int ecap) = nullptr;
	// ping i: dims = [nbeams, nsamples]; with room, amp (beam-major, raw amplitude, NaN = none), the
	// beam angles, xtra = [sample range, x min, x max] and the header line. 0 = no such ping
	int (*ping)(int i, int *dims, float *amp, int acap, double *ang, int gcap, double *xtra, char *meta,
	            int mcap) = nullptr;
	// the ping's bottom detections, 4 doubles each: x as drawn, depth, colour code, hollow; the count
	int (*bottom)(int i, double *out, int cap) = nullptr;
	// a pick at (x, depth) on ping i: the picked-positions row, tab-separated
	int (*pick)(int i, double x, double depth, char *out, int cap) = nullptr;
};

// Open the viewer (or raise it: one at a time), with `file` loaded when given.
bool wcdOpenWindow(QWidget *parent, const WcdHost &host, const QString &file = QString());

// For the host's C API and tests: [open, npings, current ping, npicks, nbottom]
int wcdState(int *out, int n);
bool wcdSetPing(int i);
bool wcdPickAt(double x, double depth);          // as a click there in Pick mode
bool wcdSavePng(const QString &path);
bool wcdGrabWindow(const QString &path);         // the whole window as on screen
bool wcdClose();

#endif // WCD_WINDOW_H_
