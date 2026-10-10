// ============================================================================
//  mbedit_window.h -- the ONE entry point of the swath bathymetry editor (MB-System's mbedit,
//  ported) for the rest of the viewer.
//
//  The editor lives in its own folder, deps/src/mbedit/, apart from the viewer's code:
//    mbedit.c / mbedit.h / mbedit_mbio.h   the engine (mbedit_prog.c), plain C, own TU
//    mbedit_window.cpp                      the Qt window (mbedit_callbacks.c), own TU
//    deps/ui/mbedit*.ui                     its window and dialogs, loaded at run time
//  The viewer reaches it only through mbeditOpenWindow(), from Geophysics > MB-System. What the
//  window needs from the viewer it is HANDED in MbEditHost (the viewer's helpers are
//  file-static in gmtvtk.cpp, so a separate translation unit cannot call them by name).
// ============================================================================

#ifndef MBEDIT_WINDOW_H_
#define MBEDIT_WINDOW_H_

#include <QIcon>
#include <QString>

#include <functional>

class QWidget;
class QKeyEvent;
class vtkRenderer;
class vtkActor;

struct MbEditHost {
	QString uiDir;                                   // deps/ui, where the mbedit*.ui files are
	QIcon icon;                                      // the app's window icon
	void (*busyText)(const char *text);              // raise (or re-label) the app's busy notice
	void (*busyOff)();                               // close it
	void (*windowOpened)();                          // a top-level window the pump must keep alive
	void (*windowClosed)();
	QString (*startDir)();                           // the file dialogs' starting folder
	void (*rememberDir)(const QString &path);
	QString (*setting)(const char *key);             // iGMT.ini
	void (*setSetting)(const char *key, const QString &value);
	// PARKING, through the viewer's ONE parked-tool list (Scene Objects). Every MB-System tool window
	// parks: its X and its minimise put it away as a handle in a viewer window; only its own Quit (or
	// the handle's Delete) closes it. `parkScene` is the viewer window it was opened from.
	void *parkScene = nullptr;
	void *(*parkWhere)(void *preferred) = nullptr;   // `preferred` if alive, else the viewer window to use
	void (*park)(void *win, QWidget *tool, const char *label, std::function<void()> show,
	             std::function<void()> remove) = nullptr;
	void (*unpark)(void *win, QWidget *tool) = nullptr;
	void (*parkOnMinimise)(QWidget *tool, std::function<void()> park) = nullptr;
	// the top-level window of a viewer window (null when it is gone): a tool BELONGS to the viewer window
	// it was opened from or parks in, and goes when that window goes, like every iGMT tool window
	QWidget *(*sceneWindow)(void *scene) = nullptr;
	// THE VIEWER'S OWN 3-D VIEW for a tool window (mbeditviz's 3-D soundings): GLView + a Scene + the
	// trackball style + the gizmo, used exactly as every iGMT 3-D view uses them — same mouse, same keys,
	// same handle. Nothing of that navigation is re-done by the tool. While `armed()` says an edit mode
	// owns the left button, its press/move/release go to `tool(what 0/1/2, x, y)` (device pixels,
	// origin bottom left) and never reach VTK; every other event is the view's. `key` sees each key
	// after the view did. The handle is what the view3d* calls below take.
	void *(*view3dMake)(QWidget *parent, std::function<bool()> armed, std::function<void(int, int, int)> tool,
	                    std::function<void(QKeyEvent *, bool)> key, QWidget **widget, vtkRenderer **ren) = nullptr;
	void (*view3dSetBounds)(void *view, const double b[6]) = nullptr;   // what the view frames (gizmo, view keys)
	double (*view3dVE)(void *view) = nullptr;                            // the gizmo's vertical exaggeration
	void (*view3dSetVE)(void *view, double ve) = nullptr;
	void (*view3dFrame)(void *view) = nullptr;                           // Reset View: the window's own fit
	// THE SAME, ON AN EXISTING VIEWER WINDOW (the 3D Soundings pane): the tool draws into the window's own
	// renderer and takes the left button while armed, the keys after the view saw them (and only armed).
	// `attachCloudActor` is the window's own point-cloud actor: the tool draws the soundings INTO it (its
	// data in true x,y,z; the actor's scale is the window's data -> world mapping), so the cloud's row,
	// LOD, recentre and readout stay the window's own;
	// `addPane` puts `content` in a narrow right-side dock of that window (null: no window), and
	// `paneClosed` is called once when the user closes it.
	void *(*view3dAttach)(void *scene, std::function<bool()> armed, std::function<void(int, int, int)> tool,
	                      std::function<void(QKeyEvent *, bool)> key, QWidget **widget, vtkRenderer **ren) = nullptr;
	void (*view3dDetach)(void *view) = nullptr;
	vtkActor *(*attachCloudActor)(void *view) = nullptr;
	void (*attachFrame)(void *view) = nullptr;
	// the window's z range (its axes box, frame and Reset View) while the tool draws into it: that of the
	// soundings the tool SHOWS, so hidden outliers stop sizing the box the exaggeration stretches.
	// The window's own range comes back at view3dDetach.
	void (*attachZRange)(void *view, double zmin, double zmax) = nullptr;
	QWidget *(*addPane)(void *scene, QWidget *content, const char *title, std::function<void()> paneClosed) = nullptr;
	// CUBE gridding (Geophysics > MB-System > CUBE gridding's dialog) on that window's swath point cloud,
	// `name` the cloud's: its input is the pane's soundings, not a file
	void (*openCubeOnCloud)(void *scene, const char *name, bool filter) = nullptr;   // filter: Flag soundings preset
	// the Interpolate dialog on that window's swath point cloud (its good soundings), set to mbgrid
	void (*openGridOnCloud)(void *scene, const char *name) = nullptr;
	// the residues of that window's swath point cloud: its good soundings minus the surface its Gridding
	// made from them (the host samples the grid and calls back mb3dsdgOpenResidueCloud)
	void (*residuesOnCloud)(void *scene) = nullptr;
	// the points selected in that window's cloud (Shift+left-drag / Ctrl+right-drag), ids into the points
	// it was made with: into ids (up to cap; null counts). How many
	int (*cloudSelection)(void *scene, int *ids, int cap) = nullptr;
};

// Make a tool window parkable — the ONE implementation every MB-System tool uses. Install it AFTER
// the tool's own close filter (filters run newest first): a park swallows the close before the
// tool's quit runs. `where` names the viewer window it prefers (null: the host's parkScene). The
// object is a child of `win` and lives as long as it.
struct MbParking;
MbParking *mbParkable(QWidget *win, const MbEditHost &host, const QString &label,
                      std::function<void *()> where = nullptr);
void mbParkShow(MbParking *p);   // bring it back (the handle, or the menu entry again): unparks
void mbParkQuit(MbParking *p);   // close for good (Quit, the handle's Delete)
void mbParkRebind(MbParking *p, void *scene);   // opened again from `scene`: park there from now on (null: keep)

// Where every MB-System tool window first comes up: against the RIGHT edge of the viewer window it was
// opened from (`viewer`, any widget of it), keeping that window's vertical centre, kept on its screen.
// Call it right after the window's FIRST show() (its frame size is known then), never on a re-show.
void mbPlaceRight(QWidget *tool, QWidget *viewer);

// Open the editor, or raise it if it is already open (the engine is a single instance: its
// state is file-static, as in mbedit). With a `file` (a swath file or a datalist) it is loaded
// at once, as `mbedit -I file -F format` does; without one, File > Open is offered.
// `useEsf` decides about an existing edit save file: -1 ask (mbedit's dialog), 0 ignore it,
// 1 apply it (mbedit -S). Returns false when the editor could not be opened.
bool mbeditOpenWindow(QWidget *parent, const MbEditHost &host, const QString &file = QString(), int format = 0,
                      int useEsf = -1);

// Load MB-System's MBIO library the way the editor does (INTERACTIVEGMT_MBIO, iGMT.ini, the one GMT
// loaded, then asking), without opening the editor. The ONE MBIO loader: the sound velocity tool
// (deps/src/mbvelocitytool/) reads its swath files through the same library and table.
bool mbeditLoadMbio(QWidget *parent, const MbEditHost &host);

// The MBIO library GMT loaded with its MB-System supplement (GMT_CUSTOM_LIBS), found by the host
// through GMT. Tried right after INTERACTIVEGMT_MBIO and the saved path. Empty = none.
void mbeditSetMbioHint(const QString &path);

// Drive / read the open editor (the host's C API and tests). All are no-ops without an editor.
//   state: [open, numfiles, currentfile, nbuffer, ngood, icurrent, nplot, nflagged, nunflagged]
int mbeditState(int *out, int n);
bool mbeditKey(int ch);                          // a key on the canvas, as typed
bool mbeditClick(int x, int y);                  // one press + release of the edit button
bool mbeditSavePng(const QString &path);         // the canvas as drawn
bool mbeditClose();                              // Quit: save the file being edited, close

#endif // MBEDIT_WINDOW_H_
