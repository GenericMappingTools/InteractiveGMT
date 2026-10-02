// ============================================================================
//  mbeditviz_window.h -- the ONE entry point of the geographic swath bathymetry editor and patch
//  test tool (MB-System's mbeditviz, ported) for the rest of the viewer.
//
//  The tool lives in its own folder, deps/src/mbeditviz/, apart from the viewer's code:
//    mbeditviz.c / mbeditviz.h / mbeditviz_mbio.h   the engine (mbeditviz_prog.c), plain C, own TU
//    mbeditviz_window.cpp                            the file list window (mbeditviz_callbacks.c)
//    mb3dsoundings_window.cpp                        the 3-D sounding editor (libmbview mb3dsoundings), Qt+VTK
//    deps/ui/mbeditviz*.ui, mb3dsoundings.ui         its windows and dialogs, loaded at run time
//  libmbview's survey map is NOT ported: the grid and the navigation are shown in an ordinary
//  InteractiveGMT window, which the viewer makes and drives through the map functions of
//  MbEditVizHost (its own helpers are file-static in gmtvtk.cpp). The soundings to edit are chosen
//  with a shape drawn on that map with the viewer's own Draw tools.
//
//  It reads swath files through the swath editor's MBIO loader (deps/src/mbedit/), so it is built
//  only together with the editor.
// ============================================================================

#ifndef MBEDITVIZ_WINDOW_H_
#define MBEDITVIZ_WINDOW_H_

#include <array>
#include <string>
#include <vector>

#include <QString>

#include "../mbedit/mbedit_window.h"

class QWidget;

// A shape drawn on the map with the viewer's Draw tools: its Scene Objects name and its vertices
// in the map's own (projected, metre) coordinates.
struct MbEditVizShape {
	std::string name;
	bool closed = false;
	bool isRect = false;
	std::vector<std::array<double, 2>> v;
};

struct MbEditVizHost {
	MbEditHost base;
	// a new window showing the grid `z` (column-major, row 0 = south; NaN = no data) over the node
	// range x0..x1, y0..y1 with the colour nodes cz -> crgb; returns its handle, or null
	void *(*mapOpen)(const char *title, const float *z, int nx, int ny, double x0, double x1, double y0, double y1,
	                 const double *cz, const double *crgb, int ncolor) = nullptr;
	bool (*mapAlive)(void *map) = nullptr;
	void (*mapClose)(void *map) = nullptr;
	// the same grid with new heights (the edits of the 3-D editor); the camera stays
	bool (*mapUpdate)(void *map, const float *z, int nx, int ny, double x0, double x1, double y0, double y1, const double *cz,
	                  const double *crgb, int ncolor) = nullptr;
	// polylines (segment offsets into xyz) under the Scene Objects group `group`
	bool (*mapAddLines)(void *map, const double *xyz, int npts, const int *segoff, int nseg, double r, double g, double b,
	                    double width, const char *name, const char *group) = nullptr;
	std::vector<MbEditVizShape> (*mapShapes)(void *map) = nullptr;
	// the colour the map shows for height z (its colour bar's map)
	bool (*mapColor)(void *map, double z, double rgb[3]) = nullptr;
	QWidget *(*mapWindow)(void *map) = nullptr;
};

// Open the tool, or raise it if it is already open (the engine is a single instance: its state is
// file-static, as in mbeditviz). With a `file` (a swath file, or a datalist with format -1) it is
// read into the file list at once, as `mbeditviz -I file -F format`. `outputMode`: 0 edit, 1 browse.
bool mbeditvizOpenWindow(QWidget *parent, const MbEditVizHost &host, const QString &file = QString(), int format = 0,
                         int outputMode = 0);

// Drive / read the open tool (the host's C API and tests). All are no-ops without the tool.
//   state: [open, numfiles, numloaded, gridstatus, nx, ny, nselected, nselected_flagged, editor_open]
int mbeditvizState(int *out, int n);
bool mbeditvizViewAll(double cellsize);          // View All, then Apply with this cell size (<=0: suggested)
// edit the soundings inside / along / under the named drawn shape: what 0 region, 1 area, 2 nav
bool mbeditvizSelect(int what, const QString &shape);
bool mbeditvizSelectBox(double x0, double x1, double y0, double y1);   // a region given by its corners
bool mbeditvizEditorKey(int ch);
bool mbeditvizEditorMode(int mode);
bool mbeditvizEditorClickSounding(int i);        // one left click on sounding i of the editor
bool mbeditvizEditorSavePng(const QString &path);
bool mbeditvizCloseEditor();
bool mbeditvizCloseMap();
bool mbeditvizClose();                           // Quit: the edits are written, files unlocked

#endif // MBEDITVIZ_WINDOW_H_
