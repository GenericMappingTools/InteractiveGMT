// ============================================================================
//  mbgrdviz_window.h -- the ONE entry point of the survey planning and display tool
//  (MB-System's mbgrdviz, ported) for the rest of the viewer.
//
//  The tool lives in its own folder, deps/src/mbgrdviz/, apart from the viewer's code:
//    mbgrdviz.c / mbgrdviz.h           the engine (mbgrdviz_callbacks.c), plain C, own TU
//    mbgrdviz_mbview.c / _mbview.h     the data half of libmbview it drives, plain C, own TU
//    mbgrdviz_window.cpp               the Qt window, own TU
//    deps/ui/mbgrdviz*.ui              its window and the survey dialog, loaded at run time
//
//  mbgrdviz shows its grids in libmbview's 3-D windows. Here there is no second viewer: a grid is
//  an ordinary InteractiveGMT window, and what mbgrdviz puts in an mbview window goes into that
//  window as its own elements -- each route a polyline, each site a symbol, each navigation file
//  a line (and its swath bounds), each vector a symbol layer coloured by its values -- with their
//  Scene Objects rows. The window is also where they are drawn and edited: every open polyline in
//  it is a route, every placed symbol a site, a two-point line the survey area's centre line and
//  a rectangle the region. Reading a swath file goes through the swath editor's MBIO loader
//  (deps/src/mbedit/); the navigation editors it launches are the ported ones.
// ============================================================================

#ifndef MBGRDVIZ_WINDOW_H_
#define MBGRDVIZ_WINDOW_H_

#include <QString>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "../mbedit/mbedit_window.h"

class QWidget;

// a window's grid (the one the tool plans on): z column-major z[i*ny+j] from the south-west node,
// NaN = no data
struct MbGrdVizGrid {
	std::vector<float> z;
	int nx = 0, ny = 0;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0, dx = 0, dy = 0;
	bool geographic = false;
	std::string crs;                     // the projection of a projected grid, for MBIO ("EPSG:32629", PROJ text)
	std::string name;
};

// a polygon of the window (drawn, or put there by the tool), vertices in the grid's coordinates
struct MbGrdVizLine {
	std::string name;
	std::string group;                   // its Scene Objects group ("" = a row of its own)
	std::vector<std::array<double, 3>> v;
	bool closed = false;
	bool isRect = false;
	double rgb[3] = {0, 0, 0};
	double width = 2.0;                  // pixels
};

// a one-point symbol of the window (the Symbols tool's, or a site the tool put there)
struct MbGrdVizPoint {
	std::string name;
	double x = 0, y = 0, z = 0;
	double rgb[3] = {0, 0, 0};
	double sizePx = 10.0;
};

struct MbGrdVizHost {
	MbEditHost base;
	bool (*alive)(void *win) = nullptr;
	QWidget *(*window)(void *win) = nullptr;
	QString (*title)(void *win) = nullptr;
	// the grid the window SHOWS (its last opened, topmost visible one), not merely its first
	bool (*grid)(void *win, MbGrdVizGrid &g) = nullptr;
	// a cheap identity of that grid (no copy of z): changes when another grid takes the window; "" = none
	std::string (*gridKey)(void *win) = nullptr;
	// open a file into the window through the viewer's ONE file-open door (a grid joins it as a
	// raster of its own); with no window, into a new one. Returns the window it went into.
	void *(*openFile)(void *win, const char *path) = nullptr;
	// a new window showing this grid (Open Region as New View)
	void *(*newWindow)(const char *title, const MbGrdVizGrid &g) = nullptr;
	std::vector<MbGrdVizLine> (*lines)(void *win) = nullptr;
	bool (*addLine)(void *win, const MbGrdVizLine &l) = nullptr;
	bool (*removeLine)(void *win, const char *name) = nullptr;
	std::vector<MbGrdVizPoint> (*points)(void *win) = nullptr;
	// each a one-point symbol (placed and moved like the Symbols tool's), under the master row `master`
	bool (*addPoints)(void *win, const std::vector<MbGrdVizPoint> &p, const char *master) = nullptr;
	bool (*removePoint)(void *win, const char *name) = nullptr;
	// polylines (segment offsets into xyz, nseg+1 of them) under the Scene Objects group `group`
	bool (*addLines)(void *win, const double *xyz, int npts, const int *segoff, int nseg, double r, double g, double b,
	                 double width, const char *name, const char *group) = nullptr;
	// one symbol layer of npts points with their own colours (rgb 0..1, 3 per point), under `master`
	bool (*addColoredPoints)(void *win, const double *xyz, int npts, const double *rgb, double sizePx, const char *name,
	                         const char *master) = nullptr;
	void (*render)(void *win) = nullptr;
	// the ported navigation editors, as mbgrdviz launched them (one file at a time); `replace`: mbeditviz
	// starts from an empty file list (the first of the selected lines), as a fresh mbeditviz process did
	bool (*openMbedit)(QWidget *parent, const QString &file, int format) = nullptr;
	bool (*openMbeditviz)(QWidget *parent, const QString &file, int format, bool replace) = nullptr;
	bool (*openMbvelocity)(QWidget *parent, const QString &file, int format) = nullptr;
	// arm the window's "point at a line" pick: each click on a line answers with the navigation track's
	// name as addLines gave it ("" = the line clicked is not a track); a null `cb` disarms
	bool (*pickNav)(void *win, std::function<void(const std::string &)> cb) = nullptr;
};

// Open the tool, or raise it if it is already open (the engine is a single instance, as mbgrdviz
// is), and bind `win` to it (null: none). With a `file`, it is opened as mbgrdviz's command line
// opens a grid: into `win`, or a new window. Returns false when the tool could not be opened.
bool mbgrdvizOpenWindow(QWidget *parent, const MbGrdVizHost &host, void *win = nullptr, const QString &file = QString());

// Drive / read the open tool (the host's C API and tests). All are no-ops without the tool.
//   state: [open, nviews, current_view_ready, nroute, nsite, nnav, nvector, working_route]
int mbgrdvizState(int *out, int n);
bool mbgrdvizOpen(int what, const QString &path);        // MBGRDVIZ_OPEN_* (mbgrdviz.h)
bool mbgrdvizSave(int what, const QString &path);        // MBGRDVIZ_SAVE_*
bool mbgrdvizSelectRoute(const QString &name);           // empty: all routes
bool mbgrdvizSetArea(const QString &line, double width); // a two-point line of the window, by name
bool mbgrdvizSetRegion(const QString &rect);             // a rectangle of the window, by name
// the survey dialog's parameters (mbgrdviz.h struct), then Generate Route; the route's name or ""
QString mbgrdvizGenerateSurvey(int mode, int platform, int direction, int crosslines, int crosslinesLast, int linespacing,
                               int swathwidth, int altitude, int depth, int interleaving, int color, const QString &name);
bool mbgrdvizSurveyDismiss();
bool mbgrdvizOpenRegion();                               // Open Region as New View
bool mbgrdvizSelectNav(int nav, bool selected);
// the navigation line a track of the window was drawn for (its name as addLines gave it, a "(swath
// bounds)" one included); -1 = none, or no tool
int mbgrdvizNavIndex(const std::string &name);
// the Action menu's editor `which` (0 MBedit, 1 MBeditviz, 2 MBnavedit, 3 MBvelocitytool) on these
// tracks instead of the checked ones: a track's own (and its group's) "MB-System" menu
bool mbgrdvizNavEditor(int which, const std::vector<std::string> &names);
bool mbgrdvizPickNav(bool on);                           // "Pick in view" down / up; true = armed
int mbgrdvizNavSelected(int nav);                        // 1 checked, 0 not, -1 no such line
bool mbgrdvizClose();

#endif // MBGRDVIZ_WINDOW_H_
