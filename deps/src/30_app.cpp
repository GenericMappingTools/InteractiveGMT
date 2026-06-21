// (Julia) pumps the loop via gmtvtk_process_events so the REPL stays interactive.
#include "_app_icon.h"               // embedded iGMT icon bytes (kAppIconPng / kAppIconPngLen)
static QApplication* g_app = nullptr;
static int           g_openWindows = 0;
static vtkRenderWindow* g_lastRW = nullptr;   // most-recent window, for gmtvtk_save_png
static Scene*           g_lastScene = nullptr;   // most-recent scene, for gmtvtk_add_overlay

// Julia console callback. The viewer lives IN-PROCESS in the Julia session, so a console
// dock can hand a typed command straight back to Julia to eval in Main. `scene` is the
// window's own Scene* (so the callback can bind `fig` to it); `cmd` is the line typed;
// the result text is written into `outbuf` (capacity `outcap`); returns its length (or -1
// if no callback registered). Set from Julia via gmtvtk_set_julia_eval.
typedef int (*JuliaEvalFn)(void* scene, const char* cmd, char* outbuf, int outcap);
static JuliaEvalFn g_juliaEval = nullptr;

// File drag-and-drop: a window receives a dropped file and hands its local path to Julia
// (g_juliaDrop), which reads it (gmtread) and views it in a NEW window. Set via
// gmtvtk_set_drop_callback. nullptr -> drops ignored.
typedef void (*JuliaDropFn)(void* scene, const char* path);
static JuliaDropFn g_juliaDrop = nullptr;

// World Topo Tiles basemap picker (port of Mirone's bg_map.m). The "Base Map" menubar button opens
// a tile picker; a clicked tile's geographic region ("W/E/S/N/wrap") is handed to Julia (g_juliaBaseMap),
// which crops data/etopo4.jpg and adds it as a referenced flat image. g_basemapLogo is the path to
// the world logo image painted in the picker, pushed from Julia via gmtvtk_set_basemap_logo.
typedef void (*JuliaBaseMapFn)(void* scene, const char* region);
static JuliaBaseMapFn g_juliaBaseMap = nullptr;
static QString        g_basemapLogo;
static QString        g_basemapIcon;   // path to the Base Map toolbar-button icon (data/basemap_icon.png)

// Geography menu (Plot coastline / political boundaries / rivers). A leaf action computes the
// CURRENT visible geographic region (i.e. honouring the zoom level) and hands the request
// "<kind>/<res>/W/E/S/N" to Julia (g_juliaGeo), which runs GMT.coast and adds the resulting
// GMTdataset as a line overlay. kind = "coast" (others reserved); res = l/i/h/f. Set via
// gmtvtk_set_geography_callback. nullptr -> the leaf falls back to a "not implemented" status.
typedef void (*JuliaGeoFn)(void* scene, const char* req);
static JuliaGeoFn g_juliaGeo = nullptr;

// Tide-station download menu. A right-click on a "Tide Stations" star adds two entries —
// "Download Mareg (2 days)" / "Download Mareg (Calendar)" — that hand (mode, station) to Julia,
// which opens the Mareg download window. mode = "2days" | "calendar"; station = the clicked star's
// "Name:/Code:/Country:" hover block. Set via gmtvtk_set_tides_callback; nullptr -> entries hidden.
typedef void (*JuliaTidesFn)(void* scene, const char* mode, const char* station);
static JuliaTidesFn g_juliaTides = nullptr;

// Live scenes, keyed by the Scene* returned to the host as an opaque figure handle.
// A handle is valid only while its window is open; the window-destroyed lambda erases
// it here so a stale handle from a closed figure is rejected instead of dereferenced.
static std::unordered_set<Scene*> g_scenes;
static bool sceneAlive(Scene* s) { return s && g_scenes.count(s) != 0; }

// iGMT application/window icon, decoded once from the embedded PNG (see _app_icon.h).
static QIcon appIcon() {
	static QIcon ic = []{
		QPixmap pm;
		pm.loadFromData(kAppIconPng, kAppIconPngLen, "PNG");
		return QIcon(pm);
	}();
	return ic;
}

static void ensureApp() {
	if (g_app) return;
	// QApplication needs argc/argv that outlive it; there is none when driven from
	// a host, so fabricate a persistent dummy argv.
	static int   s_argc = 1;
	static char  s_arg0[] = "gmtvtk";
	static char* s_argv[] = { s_arg0, nullptr };
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
	g_app = new QApplication(s_argc, s_argv);
	g_app->setWindowIcon(appIcon());   // taskbar / app-wide default icon
}

// Middle button, done by hand (not the default trackball, which the gizmo's left-drag
// observer + Qt make unreliable here):
//   - DRAG  -> pan: translate camera position + focal point so the world point under the
//              cursor (at the focal-plane depth) stays under the cursor.
//   - CLICK -> recenter: pick the SURFACE point (never the gizmo) and make it the new
//              centre of rotation, keeping the view direction + distance. The gizmo,
//              pinned to the focal point by PlaceCB, follows on the next render.
// Observed at priority 10; sets the abort flag per-event (so a plain mouse-move with the
// middle button up still reaches the coordinate readout + gizmo hover).
void MiddleCB(vtkObject* caller, unsigned long eid, void* clientData, void*) {
	Scene* s = static_cast<Scene*>(clientData);
	vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!s || !rwi) return;
	vtkRenderer* ren = s->ren;
	vtkCamera* cam = (ren && ren->GetActiveCamera()) ? ren->GetActiveCamera() : nullptr;
	if (!ren || !cam) return;
	bool handled = false;

	if (eid == vtkCommand::MiddleButtonPressEvent) {
		fprintf(stderr, "[mid] PRESS @ %d,%d\n", rwi->GetEventPosition()[0], rwi->GetEventPosition()[1]); fflush(stderr);
		s->midDown = true; s->midMoved = false;
		s->midX = rwi->GetEventPosition()[0];
		s->midY = rwi->GetEventPosition()[1];
		vtkNew<vtkCellPicker> pk;
		pk->SetTolerance(0.0005);
		pk->PickFromListOn(); pk->AddPickList(surfProp(s));   // recenter target = surface only
		s->midPicked = (pk->Pick(s->midX, s->midY, 0.0, ren) != 0);
		if (s->midPicked) pk->GetPickPosition(s->midPick);
		handled = true;
	}
	else if (eid == vtkCommand::MouseMoveEvent && s->midDown) {
		const int x = rwi->GetEventPosition()[0], y = rwi->GetEventPosition()[1];
		fprintf(stderr, "[mid] DRAG -> %d,%d\n", x, y); fflush(stderr);
		if (std::abs(x - s->midX) > 2 || std::abs(y - s->midY) > 2) s->midMoved = true;
		// Project the focal point to display to get its depth, then unproject the old and
		// new cursor positions at that depth; the difference is the world-space pan.
		double fp[3]; cam->GetFocalPoint(fp);
		ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0); ren->WorldToDisplay();
		const double depth = ren->GetDisplayPoint()[2];
		ren->SetDisplayPoint((double)x, (double)y, depth);            ren->DisplayToWorld();
		double np[4]; for (int i=0;i<4;++i) np[i]=ren->GetWorldPoint()[i];
		ren->SetDisplayPoint((double)s->midX, (double)s->midY, depth); ren->DisplayToWorld();
		double op[4]; for (int i=0;i<4;++i) op[i]=ren->GetWorldPoint()[i];
		if (np[3]!=0.0) { np[0]/=np[3]; np[1]/=np[3]; np[2]/=np[3]; }
		if (op[3]!=0.0) { op[0]/=op[3]; op[1]/=op[3]; op[2]/=op[3]; }
		const double mot[3] = { op[0]-np[0], op[1]-np[1], op[2]-np[2] };
		double pos[3]; cam->GetPosition(pos);
		cam->SetFocalPoint(fp[0]+mot[0], fp[1]+mot[1], fp[2]+mot[2]);
		cam->SetPosition (pos[0]+mot[0], pos[1]+mot[1], pos[2]+mot[2]);
		ren->ResetCameraClippingRange();
		s->midX = x; s->midY = y;
		rwi->Render();
		handled = true;
	}
	else if (eid == vtkCommand::MiddleButtonReleaseEvent) {
		if (s->midDown && !s->midMoved && s->midPicked) {  // click, no drag -> recenter
			double pos[3], fpc[3]; cam->GetPosition(pos); cam->GetFocalPoint(fpc);
			const double dir[3] = { pos[0]-fpc[0], pos[1]-fpc[1], pos[2]-fpc[2] };
			cam->SetFocalPoint(s->midPick);
			cam->SetPosition(s->midPick[0]+dir[0], s->midPick[1]+dir[1], s->midPick[2]+dir[2]);
			ren->ResetCameraClippingRange();
			rwi->Render();
		}
		s->midDown = false;
		handled = true;
	}

	if (s->midCmd) s->midCmd->SetAbortFlagOnExecute(handled ? 1 : 0);
}

// Middle button reaches the Qt widget (left/right do) but the VTK interactor adapter was
// NOT delivering MiddleButton events to our observers ([mid] PRESS never fired). So drive
// middle-button PAN (drag) and RECENTER (click, no drag) straight from a Qt event filter
// installed on the widget — the same event path left-drag/right-click already use.
class MidPanFilter : public QObject {
public:
	Scene* s = nullptr;
	bool   down = false, moved = false;
	double lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	explicit MidPanFilter(Scene* sc, QObject* parent) : QObject(parent), s(sc) {}
protected:
	// VTK display coords are bottom-up device pixels; Qt gives top-down logical pixels.
	void devPos(QMouseEvent* me, double& dx, double& dy) {
		const double r = s->widget->devicePixelRatioF();
		const int    H = s->widget->renderWindow()->GetSize()[1];
		dx = me->position().x() * r;
		dy = H - me->position().y() * r;
	}
	void panTo(double ox, double oy, double nx, double ny) {
		vtkRenderer* ren = s->ren; vtkCamera* cam = ren->GetActiveCamera(); if (!cam) return;
		double fp[3]; cam->GetFocalPoint(fp);
		ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0); ren->WorldToDisplay();
		const double depth = ren->GetDisplayPoint()[2];
		ren->SetDisplayPoint(nx, ny, depth); ren->DisplayToWorld();
		double np[4]; for (int i=0;i<4;++i) np[i]=ren->GetWorldPoint()[i];
		ren->SetDisplayPoint(ox, oy, depth); ren->DisplayToWorld();
		double op[4]; for (int i=0;i<4;++i) op[i]=ren->GetWorldPoint()[i];
		if (np[3]!=0.0){ np[0]/=np[3]; np[1]/=np[3]; np[2]/=np[3]; }
		if (op[3]!=0.0){ op[0]/=op[3]; op[1]/=op[3]; op[2]/=op[3]; }
		const double m[3] = { op[0]-np[0], op[1]-np[1], op[2]-np[2] };
		double pos[3]; cam->GetPosition(pos);
		cam->SetFocalPoint(fp[0]+m[0], fp[1]+m[1], fp[2]+m[2]);
		cam->SetPosition (pos[0]+m[0], pos[1]+m[1], pos[2]+m[2]);
		ren->ResetCameraClippingRange();
		s->widget->renderWindow()->Render();
	}
	void recenter(double x, double y) {
		vtkRenderer* ren = s->ren; vtkCamera* cam = ren->GetActiveCamera(); if (!cam) return;
		vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
		pk->PickFromListOn(); pk->AddPickList(surfProp(s));
		if (pk->Pick(x, y, 0.0, ren)) {
			double pick[3]; pk->GetPickPosition(pick);
			double pos[3], fp[3]; cam->GetPosition(pos); cam->GetFocalPoint(fp);
			const double d[3] = { pos[0]-fp[0], pos[1]-fp[1], pos[2]-fp[2] };
			cam->SetFocalPoint(pick);
			cam->SetPosition(pick[0]+d[0], pick[1]+d[1], pick[2]+d[2]);
			ren->ResetCameraClippingRange();
			s->widget->renderWindow()->Render();
		}
	}
	bool eventFilter(QObject* obj, QEvent* ev) override {
		if (!s || !s->ren) return QObject::eventFilter(obj, ev);
		const QEvent::Type t = ev->type();
		if (t == QEvent::MouseButtonPress) {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::MiddleButton) {
				fprintf(stderr, "[mid] PRESS (qt filter)\n"); fflush(stderr);
				down = true; moved = false;
				devPos(me, pressX, pressY); lastX = pressX; lastY = pressY;
				return true;
			}
		}
		else if (t == QEvent::MouseMove && down) {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			double x, y; devPos(me, x, y);
			if (std::abs(x - pressX) > 2 || std::abs(y - pressY) > 2) moved = true;
			panTo(lastX, lastY, x, y);
			lastX = x; lastY = y;
			return true;
		}
		else if (t == QEvent::MouseButtonRelease && down) {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::MiddleButton) {
				if (!moved) { double x, y; devPos(me, x, y); recenter(x, y); }
				down = false;
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Accept dropped files on a window: on a URL drop, hand each LOCAL file path + THIS window's
// Scene* to Julia (g_juliaDrop), which reads the file and adds it INTO this window. One filter
// per window so it knows which Scene received the drop.
struct DropFilter : QObject {
	Scene* s = nullptr;
	explicit DropFilter(Scene* sc) : s(sc) {}
protected:
	bool eventFilter(QObject* obj, QEvent* ev) override {
		const QEvent::Type t = ev->type();
		if (t == QEvent::DragEnter || t == QEvent::DragMove) {
			auto* de = static_cast<QDragMoveEvent*>(ev);
			if (de->mimeData() && de->mimeData()->hasUrls()) { de->acceptProposedAction(); return true; }
		} else if (t == QEvent::Drop) {
			auto* de = static_cast<QDropEvent*>(ev);
			if (de->mimeData() && de->mimeData()->hasUrls()) {
				for (const QUrl& u : de->mimeData()->urls()) {
					const QString f = u.toLocalFile();
					if (!f.isEmpty() && g_juliaDrop) {
						const QByteArray utf8 = f.toUtf8();        // keep the buffer alive across the call
						g_juliaDrop(s, utf8.constData());
					}
				}
				de->acceptProposedAction();
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Wire file drag-and-drop into a window (called for EVERY viewer window). The widget must
// accept drops AND have the filter installed so QEvent::Drop is delivered + intercepted.
static void enableFileDrops(QMainWindow* win, QWidget* widget, Scene* s) {
	DropFilter* filt = new DropFilter(s);   // one per window (carries its Scene*); freed with the window
	filt->setParent(win);                   // parented to the window -> destroyed with it
	win->setAcceptDrops(true);
	widget->setAcceptDrops(true);
	win->installEventFilter(filt);
	widget->installEventFilter(filt);
}

// Procedural HDR environment for image-based lighting. A flat azimuthal gradient
// looks dull (no directional light), so this bakes a studio-ish equirectangular
// sky: cool bright zenith -> warm horizon -> darker ground, PLUS a soft bright
// "sun" disk that gives PBR its directional specular pop. Values >1 are HDR.
