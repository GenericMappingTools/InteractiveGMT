// 57_swipe.cpp — two ways to compare TWO rasters, sharing one toolbar slot (Swipe | Link).
//
// SWIPE compares two layers OF THE SAME WINDOW by dragging a vertical divider across the scene (the
// ArcGIS StoryMaps "swipe" gesture). Mechanism: ONE camera-aligned cut plane, applied to BOTH paired
// layers with opposite normals — the left layer keeps the half left of the divider, the right layer
// keeps the half right of it. Because the two halves never overlap there is no z-fighting and no
// dependence on which layer happens to be higher in the grid pile: the pair can be any two rasters,
// in any order, a grid against an image just as well as grid against grid.
//
// The plane is rebuilt from the CAMERA (normal = camera right-vector, origin = the world point
// under the divider's screen x) on every camera change, so the divider stays a fixed vertical line
// on screen while the user rotates/zooms/pans — in the flat-2D map and in the 3-D perspective view
// alike. The same rebuild re-stamps the plane onto the mappers, which is also how tiles born later
// by the LOD refine (a plain grid's surface is a quadtree of lazily-built tile actors) get clipped.
//
// While swipe is ON every OTHER raster in the window is hidden, so nothing can intrude between or
// above the pair; their prior visibility is saved and restored when swipe turns off. Scene Objects
// is rebuilt on both transitions, so its checkboxes show the real state (SACRED_LAW.md group-
// uncheck law: every row's checked state is recomputed from the CURRENT actor visibility).
//
// The divider itself is a narrow Qt overlay widget parented on the VTK widget (not a vtkActor2D):
// it owns the drag, the grab handle and the cursor with plain Qt mouse events, and leaves every
// pixel outside its own strip to VTK's interactor.
//
// LINK shows only ONE of a pair at a time instead of splitting them across a divider: HOLD THE RIGHT
// BUTTON to see the other one, RELEASE to snap back to the one you started on — a momentary peek, not
// a toggle, so the pair is compared by flicking the button rather than by hunting for the way back.
// That is why Link owns the raw press and release (LinkPeekFilter) instead of the context-menu
// request the rest of the app uses: one notification cannot say "still held" and then "let go".
// Two flavours, chosen by what the window actually holds — never by a mode switch the user has to find:
//   IN-WINDOW    — this window holds >=2 rasters: pair two of them, with the identical pairing Swipe
//                  uses (swipeLayers / swipePartnerDialog, reused verbatim), and the peek just flips
//                  which one is visible. The camera is NEVER touched — that is exactly what
//                  "respecting the current zoom" means, nothing about the view moves.
//   CROSS-WINDOW — this window holds only ONE raster: pair with ANOTHER OPEN WINDOW, and the peek
//                  raises it framed on the region THIS window is showing (that is where "at the zoom
//                  level of the first one" has to be done on purpose, since the partner has its own
//                  camera), raising this one back on release. The partner's axes/frame are never
//                  touched, only its camera.
// Either way the two extents only need to INTERSECT, never match (checked once, when Link turns on)
// — a linked pair can be a grid and an image of completely different resolution and footprint, as
// long as they describe overlapping ground.
//
// Both modes ARM THE MOMENT THEY ARE PICKED from the toolbar dropdown (swipeSelectMode): choosing a
// tool out of a menu is the act of starting it, never a silent mode flag awaiting a second click.

static QIcon makeSwipeIcon();               // split-tile glyph (85_polygon.cpp; also fwd-declared in 70_window.cpp)
static QIcon makeLinkIcon();                 // two-windows-and-a-chain glyph (85_polygon.cpp; ditto)

// One raster layer of a window as the swipe tool sees it: the base surface/image plus every
// dropped grid/image. `prop` is the identity + visibility handle (exactly what the Scene Objects
// group row toggles); `actors` are the renderables whose mappers carry the cut plane.
struct SwipeLayer {
	vtkProp3D *prop = nullptr;
	std::vector<vtkActor*> actors;
	std::string name;
	int  rank = 0;              // grid-pile rank; picks the "currently displayed" layer
	bool visible = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;   // TRUE data extent — Link's own-layers-must-intersect check
};

// Every raster in the window, base first then the extras in add order. Non-raster objects (lines,
// polygons, symbols, curtains, solids) are never swipe candidates.
static std::vector<SwipeLayer> swipeLayers(Scene *s) {
	std::vector<SwipeLayer> out;
	if (!s) return out;
	if (s->imageOnly) {                                   // bare image window: the drape IS the base layer
		if (s->drape) {
			SwipeLayer L;
			L.prop = s->drape; L.actors = { s->drape.Get() };
			L.name = s->surfName.empty() ? std::string("Image") : s->surfName;
			L.rank = s->surfStack; L.visible = s->drape->GetVisibility() != 0;
			L.x0 = s->x0; L.x1 = s->x1; L.y0 = s->y0; L.y1 = s->y1;
			out.push_back(L);
		}
	}
	else if (s->surf && !s->emptyStart) {                 // grid/solid window: the base surface (+ its drape)
		if (vtkProp3D *sp = surfProp(s)) {
			SwipeLayer L;
			L.prop = sp; L.actors = surfActors(s);
			if (s->drape) L.actors.push_back(s->drape.Get());
			L.name = (s->customLayerTexture && !s->aquaVarLabel.empty()) ? s->aquaVarLabel
			       : s->surfName.empty() ? std::string("Surface") : s->surfName;
			L.rank = s->surfStack; L.visible = sp->GetVisibility() != 0;
			if (!s->gridZ.empty()) { L.x0 = s->gx0; L.x1 = s->gx1; L.y0 = s->gy0; L.y1 = s->gy1; }
			else                   { L.x0 = s->x0;  L.x1 = s->x1;  L.y0 = s->y0;  L.y1 = s->y1;  }
			if (!L.actors.empty()) out.push_back(L);
		}
	}
	for (auto &ex : s->extras) {
		if (!ex.actor) continue;
		SwipeLayer L;
		L.prop = ex.actor.Get(); L.actors = { ex.actor.Get() };
		if (ex.drape) L.actors.push_back(ex.drape.Get());
		L.name = ex.name;
		L.rank = ex.gstack; L.visible = ex.actor->GetVisibility() != 0;
		if (ex.isImage) { L.x0 = ex.bx0; L.x1 = ex.bx1; L.y0 = ex.by0; L.y1 = ex.by1; }
		else            { L.x0 = ex.gx0; L.x1 = ex.gx1; L.y0 = ex.gy0; L.y1 = ex.gy1; }
		out.push_back(L);
	}
	return out;
}

// The layer the window is CURRENTLY showing = highest pile rank among the visible ones, later
// additions winning ties (images stack by zpos rather than the pile, so they all rank 0 and the
// most recently dropped one is the one on top). -1 when nothing is visible.
static int swipeCurrentIndex(const std::vector<SwipeLayer> &L) {
	int best = -1;
	for (size_t i = 0; i < L.size(); ++i) {
		if (!L[i].visible) continue;
		if (best < 0 || L[i].rank >= L[best].rank) best = (int)i;
	}
	return best;
}

// Index of `p` in the layer list, or -1 if that layer is gone (deleted while swipe was on).
static int swipeIndexOf(const std::vector<SwipeLayer> &L, vtkProp3D *p) {
	if (!p) return -1;
	for (size_t i = 0; i < L.size(); ++i) if (L[i].prop == p) return (int)i;
	return -1;
}

// Stamp the pair's cut planes onto their mappers (idempotent — a mapper already carrying exactly
// its own plane is left alone). Called on every camera change too, so LOD tiles created after the
// tool was switched on are clipped from their first frame.
static void swipeApplyPlanes(Scene *s) {
	if (!s || !s->swipePlaneA || !s->swipePlaneB) return;
	std::vector<SwipeLayer> L = swipeLayers(s);
	const int ia = swipeIndexOf(L, s->swipeAProp), ib = swipeIndexOf(L, s->swipeBProp);
	for (int k = 0; k < 2; ++k) {
		const int idx = k ? ib : ia;
		if (idx < 0) continue;
		vtkPlane *pl = k ? s->swipePlaneB.Get() : s->swipePlaneA.Get();
		for (vtkActor *a : L[idx].actors) {
			vtkMapper *m = a ? a->GetMapper() : nullptr;
			if (!m) continue;
			vtkPlaneCollection *pc = m->GetClippingPlanes();
			if (pc && pc->GetNumberOfItems() == 1 && pc->GetItem(0) == pl) continue;
			m->RemoveAllClippingPlanes();
			m->AddClippingPlane(pl);
		}
	}
}

// Drop the cut plane from EVERY raster in the window (not just the current pair — the pair may have
// changed, or a layer may have been re-added since).
static void swipeClearPlanes(Scene *s) {
	for (SwipeLayer &L : swipeLayers(s))
		for (vtkActor *a : L.actors)
			if (vtkMapper *m = a ? a->GetMapper() : nullptr) m->RemoveAllClippingPlanes();
}

static void swipePlaceBar(Scene *s);          // defined with the divider widget below

// Re-derive the cut plane from the camera so the divider reads as a fixed VERTICAL line on screen
// whatever the camera is doing: normal = the camera's right-vector, origin = the world point under
// the divider's screen x (taken at the focal point's depth). Does NOT render — it runs inside the
// camera-modified callback, immediately before the render that consumes it.
static void swipeUpdateGeometry(Scene *s) {
	if (!s || !s->swipeOn || !s->ren || !s->swipePlaneA || !s->swipePlaneB) return;
	vtkCamera *cam = s->ren->GetActiveCamera();
	if (!cam) return;
	double dir[3], up[3], right[3];
	cam->GetDirectionOfProjection(dir);
	cam->GetViewUp(up);
	vtkMath::Cross(dir, up, right);
	if (vtkMath::Normalize(right) < 1e-12) return;
	double fp[3];
	cam->GetFocalPoint(fp);
	s->ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0);
	s->ren->WorldToDisplay();
	double dp[3];
	s->ren->GetDisplayPoint(dp);
	const int *sz = s->ren->GetSize();
	s->ren->SetDisplayPoint(s->swipeFrac * (sz ? sz[0] : 0), dp[1], dp[2]);
	s->ren->DisplayToWorld();
	double wp[4];
	s->ren->GetWorldPoint(wp);
	if (wp[3] != 0.0) { wp[0] /= wp[3]; wp[1] /= wp[3]; wp[2] /= wp[3]; }
	s->swipePlaneA->SetOrigin(wp[0], wp[1], wp[2]);
	s->swipePlaneB->SetOrigin(wp[0], wp[1], wp[2]);
	s->swipePlaneA->SetNormal(-right[0], -right[1], -right[2]);   // A keeps the half LEFT of the line
	s->swipePlaneB->SetNormal( right[0],  right[1],  right[2]);   // B keeps the half RIGHT of it
	swipeApplyPlanes(s);
	swipePlaceBar(s);
}

static void swipeCameraCB(vtkObject *, unsigned long, void *cd, void *) {
	swipeUpdateGeometry(static_cast<Scene *>(cd));
}

// ── the divider ────────────────────────────────────────────────────────────────────────────────
// A narrow full-height child of the VTK widget: a thin light line with a round grab handle at
// mid-height. Only this strip takes mouse events; everything either side still reaches the VTK
// interactor (rotate / pan / hover readout keep working while swiping).
class SwipeDivider : public QWidget {
public:
	SwipeDivider(Scene *sc, QWidget *parent) : QWidget(parent), s(sc) {
		setCursor(Qt::SizeAllCursor);                 // every drag in this app uses SizeAll
		setAttribute(Qt::WA_NoMousePropagation, true);
		// Only the line and the handle are painted; the rest of the strip lets the rendered scene
		// through (the widget is composited over the QOpenGLWidget's content by Qt's backing store).
		setAttribute(Qt::WA_TranslucentBackground, true);
		setAttribute(Qt::WA_NoSystemBackground, true);
		setToolTip("Drag to swipe between the two layers");
		parent->installEventFilter(this);
		raise();
		show();
	}
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		const double cx = width() * 0.5, cy = height() * 0.5;
		p.setPen(QPen(QColor(0, 0, 0, 140), 3.0));            // dark casing keeps the line legible
		p.drawLine(QPointF(cx, 0), QPointF(cx, height()));     // over bright and dark rasters alike
		p.setPen(QPen(QColor(255, 255, 255, 230), 1.4));
		p.drawLine(QPointF(cx, 0), QPointF(cx, height()));
		p.setPen(QPen(QColor(0, 0, 0, 160), 1.2));             // the grab handle
		p.setBrush(QColor(250, 250, 250, 235));
		p.drawEllipse(QPointF(cx, cy), 9.0, 9.0);
		QPolygonF lt, rt;                                      // left / right chevrons inside it
		lt << QPointF(cx - 2.0, cy) << QPointF(cx + 1.0, cy - 3.5) << QPointF(cx + 1.0, cy + 3.5);
		rt << QPointF(cx + 2.0, cy) << QPointF(cx - 1.0, cy - 3.5) << QPointF(cx - 1.0, cy + 3.5);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(45, 45, 50));
		p.drawPolygon(lt.translated(-3.0, 0.0));
		p.drawPolygon(rt.translated( 3.0, 0.0));
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) { e->ignore(); return; }
		grabDX = mapToParent(e->pos()).x() - (x() + width() / 2);
		dragging = true;
		e->accept();
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!dragging || !s || !parentWidget()) { e->ignore(); return; }
		const int w = parentWidget()->width();
		if (w <= 0) return;
		double f = double(mapToParent(e->pos()).x() - grabDX) / double(w);
		s->swipeFrac = std::max(0.01, std::min(0.99, f));
		swipeUpdateGeometry(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		e->accept();
	}
	void mouseReleaseEvent(QMouseEvent *e) override { dragging = false; e->accept(); }
	// The strip must follow the view when the window is resized (its screen x is a FRACTION of the
	// widget width, and the plane origin is re-solved from the new render size).
	bool eventFilter(QObject *o, QEvent *ev) override {
		if (o == parentWidget() && ev->type() == QEvent::Resize) swipeUpdateGeometry(s);
		return QWidget::eventFilter(o, ev);
	}
private:
	Scene *s = nullptr;
	int  grabDX  = 0;
	bool dragging = false;
};

static void swipePlaceBar(Scene *s) {
	if (!s || !s->swipeBar) return;
	QWidget *par = s->swipeBar->parentWidget();
	if (!par) return;
	const int w = 22;
	s->swipeBar->setGeometry(int(s->swipeFrac * par->width()) - w / 2, 0, w, par->height());
	s->swipeBar->raise();
}

// Turn the tool on for the pair (a = left half, b = right half) or off. Toggling on twice with a
// different pair is fine: the previous pair's planes and hidden layers are undone first.
static void swipeSetActive(Scene *s, bool on, vtkProp3D *a, vtkProp3D *b) {
	if (!s || !s->ren) return;
	// --- always tear the previous state down first (idempotent for a plain off) ---
	if (s->swipeCamCmd) {
		if (vtkCamera *cam = s->ren->GetActiveCamera()) cam->RemoveObserver(s->swipeCamCmd);
		s->swipeCamCmd = nullptr;
	}
	swipeClearPlanes(s);
	if (s->swipeBar) { s->swipeBar->deleteLater(); s->swipeBar = nullptr; }
	if (s->swipeOn) {                                  // restore what the previous pair had hidden
		std::vector<SwipeLayer> L = swipeLayers(s);
		for (auto &kv : s->swipeSavedVis)
			if (swipeIndexOf(L, kv.first) >= 0) sceneLayerSetVisibleByProp(s, kv.first, kv.second != 0);
	}
	s->swipeSavedVis.clear();
	s->swipeOn = false;
	s->swipeAProp = s->swipeBProp = nullptr;
	s->swipePlaneA = s->swipePlaneB = nullptr;

	if (on && a && b && a != b && s->widget) {
		std::vector<SwipeLayer> L = swipeLayers(s);
		if (swipeIndexOf(L, a) >= 0 && swipeIndexOf(L, b) >= 0) {
			for (SwipeLayer &lay : L) {                // pair visible, EVERY other raster hidden
				s->swipeSavedVis.push_back({ lay.prop, lay.prop->GetVisibility() });
				sceneLayerSetVisibleByProp(s, lay.prop, lay.prop == a || lay.prop == b);
			}
			s->swipeAProp = a; s->swipeBProp = b;
			s->swipePlaneA = vtkSmartPointer<vtkPlane>::New();
			s->swipePlaneB = vtkSmartPointer<vtkPlane>::New();
			s->swipeOn = true;
			s->swipeBar = new SwipeDivider(s, s->widget);
			s->swipeCamCmd = vtkSmartPointer<vtkCallbackCommand>::New();
			s->swipeCamCmd->SetClientData(s);
			s->swipeCamCmd->SetCallback(swipeCameraCB);
			if (vtkCamera *cam = s->ren->GetActiveCamera())
				cam->AddObserver(vtkCommand::ModifiedEvent, s->swipeCamCmd);
			swipeUpdateGeometry(s);
		}
	}
	refreshGridColorbar(s);        // the visible-layer set changed -> retarget bar + hover readout
	rebuildSceneObjects(s);        // group-uncheck law: rows re-read the CURRENT actor visibility
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Ask which layer to pair the current one with. Returns the chosen index into `L`, or -1 if the
// user cancelled. Only shown when the window holds MORE than two rasters.
static int swipePartnerDialog(Scene *s, const std::vector<SwipeLayer> &L, int cur) {
	QDialog dlg(s->win);
	dlg.setWindowTitle("Swipe — pair with");
	QVBoxLayout *lay = new QVBoxLayout(&dlg);
	lay->addWidget(new QLabel(QString("Swipe \"%1\" against:").arg(QString::fromStdString(L[cur].name)), &dlg));
	QListWidget *list = new QListWidget(&dlg);
	std::vector<int> map;                              // list row -> index into L
	for (size_t i = 0; i < L.size(); ++i) {
		if ((int)i == cur) continue;
		list->addItem(QString::fromStdString(L[i].name));
		map.push_back((int)i);
	}
	list->setCurrentRow(0);
	lay->addWidget(list);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	lay->addWidget(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
	if (dlg.exec() != QDialog::Accepted) return -1;
	const int row = list->currentRow();
	return (row >= 0 && row < (int)map.size()) ? map[row] : -1;
}

// The toolbar toggle's handler: resolve the pair (auto with exactly two rasters, dialog with more)
// and hand it to swipeSetActive. Un-checks itself if there is nothing to pair or the user cancels.
static void swipeToggled(Scene *s, QAction *act, bool on) {
	if (!s) return;
	if (!on) { swipeSetActive(s, false, nullptr, nullptr); return; }
	std::vector<SwipeLayer> L = swipeLayers(s);
	if (L.size() < 2) {
		if (s->win) s->win->statusBar()->showMessage("Swipe needs two grids / images in the window", 3000);
		act->setChecked(false);
		return;
	}
	int cur = swipeCurrentIndex(L);
	if (cur < 0) cur = (int)L.size() - 1;              // nothing visible -> swipe the last one added
	int other = -1;
	if (L.size() == 2) other = (cur == 0) ? 1 : 0;
	else               other = swipePartnerDialog(s, L, cur);
	if (other < 0) { act->setChecked(false); return; }
	swipeSetActive(s, true, L[cur].prop, L[other].prop);
	if (s->win)
		s->win->statusBar()->showMessage(QString("Swipe: %1 | %2 — drag the divider")
		        .arg(QString::fromStdString(L[cur].name), QString::fromStdString(L[other].name)), 4000);
}

static std::vector<Scene*> linkPartnerWindows(Scene *s);   // other open windows Link could pair with

// Keep the toolbar button's enabled state honest. Enabled when EITHER mode has something to work
// with — two rasters in THIS window (what Swipe always needs, and Link's in-window pairing), or one
// raster here plus another OPEN WINDOW over the same region (Link's cross-window pairing). Never
// gated on the CURRENTLY selected mode: the dropdown that switches mode lives inside this very
// button, so a slot disabled for the active mode would make the other, usable mode unreachable —
// picking a mode that then has nothing to pair is refused by that mode's own handler, with a status
// line saying why. Called at the end of every rebuildSceneObjects, i.e. after every add / delete /
// visibility change, and for EVERY live scene whenever a window opens or closes (70_window.cpp —
// gaining or losing a window changes what Link can pair with in every OTHER window too). Switching
// the tool off is deferred to the event loop so this never re-enters the rebuild that called it.
static void swipeRefreshAvailability(Scene *s) {
	if (!s || !s->swipeAct) return;
	const size_t n = swipeLayers(s).size();
	const bool usable = (n >= 2) || (n >= 1 && !linkPartnerWindows(s).empty());
	s->swipeAct->setEnabled(usable);
	if (s->swipeToolBtn) s->swipeToolBtn->setEnabled(usable);   // the VISIBLE button -- swipeAct alone never reaches the screen
	if (s->swipeAct->isChecked() && !usable) {
		QAction *act = s->swipeAct;
		QTimer::singleShot(0, act, [act]() { act->setChecked(false); });
	}
}

// Put the shared toolbar slot IN a mode (icon, tooltip, availability) without arming anything. Not
// the dropdown's handler — swipeSelectMode below is. Used on its own only where a mode change must
// NOT re-run the arming flow: Link mirroring its mode onto the partner window it just paired with.
static void swipeSetMode(Scene *s, bool linkMode) {
	if (!s || !s->swipeAct) return;
	s->swipeToolMode = linkMode ? Scene::ToolMode::Link : Scene::ToolMode::Swipe;
	if (s->swipeToolBtn) {
		s->swipeToolBtn->setIcon(linkMode ? makeLinkIcon() : makeSwipeIcon());
		s->swipeToolBtn->setToolTip(linkMode ? "Link: right-click shows the other paired raster / window"
		                                     : "Swipe: compare two grids / images across a draggable divider");
	}
	swipeRefreshAvailability(s);
}

// The toolbar dropdown's handler: picking a mode SELECTS it AND TURNS IT ON. Picking a tool out of a
// menu IS the act of starting it — there is no second, invisible arming click. (The two-step version
// this replaces left Link looking stone dead: the mode was selected, nothing was armed, and a
// right-click over a raster with no overlay under it falls through every hit test in the context-menu
// handler and pops no menu at all, so the user got no feedback whatever.) Re-picking the mode already
// running restarts it with a fresh partner pick, which is why the toggle is unconditionally reset off
// first. The ONE implementation both dropdown entries (70_window.cpp) call, and the only thing
// gmtvtk_swipe_select_mode_h (90_c_api.cpp) wraps for host/test code.
static void swipeSelectMode(Scene *s, bool linkMode) {
	if (!s || !s->swipeAct) return;
	if (s->swipeAct->isChecked()) s->swipeAct->setChecked(false);   // always a fresh start
	swipeSetMode(s, linkMode);
	if (s->swipeAct->isEnabled()) s->swipeAct->setChecked(true);    // ARM: runs swipeToggled / linkToggled
}

// ── Link ──────────────────────────────────────────────────────────────────────────────────────

// Every OTHER open window Link could pair THIS one with: alive, actually built, holding at least one
// raster of its own, and with a data frame that INTERSECTS this window's — the same overlap-not-
// equality rule the in-window pairing applies to two layers, applied to two whole windows. Sorted by
// title because g_scenes is an unordered_set and the picker's row order must not wander.
static std::vector<Scene*> linkPartnerWindows(Scene *s) {
	std::vector<Scene*> out;
	if (!s) return out;
	for (Scene *o : g_scenes) {
		if (o == s || !o->win || !o->ren) continue;
		if (swipeLayers(o).empty()) continue;                                   // nothing to show there
		if (s->x1 <= o->x0 || s->x0 >= o->x1 || s->y1 <= o->y0 || s->y0 >= o->y1) continue;
		out.push_back(o);
	}
	std::sort(out.begin(), out.end(), [](Scene *a, Scene *b) {
		return a->win->windowTitle() < b->win->windowTitle();
	});
	return out;
}

// Ask which OTHER WINDOW to pair with — the cross-window twin of swipePartnerDialog, same shape and
// same keyboard/double-click behaviour, only the list's contents differ. nullptr if cancelled.
static Scene *linkPartnerWindowDialog(Scene *s, const std::vector<Scene*> &cand) {
	QDialog dlg(s->win);
	dlg.setWindowTitle("Link — pair with window");
	QVBoxLayout *lay = new QVBoxLayout(&dlg);
	lay->addWidget(new QLabel("Right-click in this window will show:", &dlg));
	QListWidget *list = new QListWidget(&dlg);
	for (Scene *o : cand) list->addItem(o->win->windowTitle());
	list->setCurrentRow(0);
	lay->addWidget(list);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	lay->addWidget(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
	if (dlg.exec() != QDialog::Accepted) return nullptr;
	const int row = list->currentRow();
	return (row >= 0 && row < (int)cand.size()) ? cand[row] : nullptr;
}

static void linkPeek(Scene *s, bool on);   // the held-right-button peek itself (end of this file)

// Link's gesture is a PRESS-AND-HOLD, so it has to own the raw right-button press and release — a
// context-menu request is a single notification, it cannot say "still held" or "let go". This filter
// sits on the VTK widget while Link is on and consumes both, which also stops VTK's own right-drag
// dolly from zooming the view under the peek. Everything else — every other button, and right-click
// itself while a rubber-band selection or a polygon drawing is in progress — passes straight
// through, exactly as the context-menu handler's guards did (70_window.cpp).
class LinkPeekFilter : public QObject {
public:
	explicit LinkPeekFilter(Scene *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	bool eventFilter(QObject *, QEvent *ev) override {
		if (!s || !s->linkOn) return false;
		const bool press = ev->type() == QEvent::MouseButtonPress;
		if (!press && ev->type() != QEvent::MouseButtonRelease) return false;
		QMouseEvent *me = static_cast<QMouseEvent*>(ev);
		if (me->button() != Qt::RightButton) return false;
		if (s->rbEnabled && (s->rbConsume ||
			(QApplication::keyboardModifiers() & Qt::ControlModifier))) return false;   // Ctrl+right = select
		if (s->polyMode && s->polyDrawing) return false;                                 // = remove last vertex
		linkPeek(s, press);
		return true;
	}
private:
	Scene *s = nullptr;
};

// A cross-window pair is stored on BOTH scenes (the right-click gesture works from either side), so
// it must also be BROKEN from both — set while mirroring the break onto the partner's own toolbar
// toggle, whose `toggled` handler would otherwise run the whole tear-down a second time.
static bool g_linkMirroring = false;

// Put the press-and-hold filter on a window's viewport, or take it off again. Idempotent, and the
// ONE place the filter is created/destroyed — both sides of a cross-window pair go through it.
static void linkSetPeekFilter(Scene *s, bool on) {
	if (!s || !s->widget) return;
	if (on) {
		if (!s->linkPeekFilter) {
			LinkPeekFilter *f = new LinkPeekFilter(s, s->widget);
			s->widget->installEventFilter(f);
			s->linkPeekFilter = f;
		}
	}
	else if (s->linkPeekFilter) {
		s->widget->removeEventFilter(s->linkPeekFilter);
		s->linkPeekFilter->deleteLater();
		s->linkPeekFilter = nullptr;
	}
}

// Drive the partner window's shared toggle to `on` for appearance only: its handler sees the guard
// and returns, because the pairing bookkeeping is done by the side that initiated it.
static void linkMirrorToggle(Scene *o, bool on) {
	if (!o || !o->swipeAct || o->swipeAct->isChecked() == on) return;
	g_linkMirroring = true;
	o->swipeAct->setChecked(on);
	g_linkMirroring = false;
}

// Break a cross-window pair from both sides. A no-op for an in-window pair, and safe on a half-dead
// one: a partner whose window has closed is already out of g_scenes, so sceneAlive rejects the stale
// handle before anything dereferences it.
static void linkUnlinkWindows(Scene *s) {
	if (!s) return;
	Scene *o = s->linkPartnerScene;
	s->linkPartnerScene = nullptr;
	if (!o || !sceneAlive(o)) return;
	if (o->linkPeeking) linkPeek(o, false);            // never leave the partner stuck mid-peek
	o->linkPartnerScene = nullptr;
	o->linkOn = false;
	linkSetPeekFilter(o, false);
	linkMirrorToggle(o, false);
}

// The toolbar toggle's handler for Link mode (dispatched from the SAME s->swipeAct::toggled signal
// swipeToggled uses for Swipe mode — see the toolbar wiring in 70_window.cpp). Two ways to resolve
// the pair, chosen by what this window actually holds:
//   >=2 rasters here -> pair two of THEM, EXACTLY as swipeToggled does (implicit with exactly two,
//                       swipePartnerDialog with more), requiring only that their extents intersect;
//   exactly 1 raster -> pair with ANOTHER OPEN WINDOW whose frame overlaps this one's.
// Un-checks itself if there is nothing to pair with, the user cancels, or the two don't overlap.
static void linkToggled(Scene *s, QAction *act, bool on) {
	if (!s) return;
	if (g_linkMirroring) return;      // partner's button is only being kept in sync; not a real toggle
	if (!on) {
		if (s->linkPeeking) linkPeek(s, false);        // switched off mid-hold: snap back first
		if (s->linkOn) {
			std::vector<SwipeLayer> L = swipeLayers(s);
			for (auto &kv : s->linkSavedVis)
				if (swipeIndexOf(L, kv.first) >= 0) sceneLayerSetVisibleByProp(s, kv.first, kv.second != 0);
		}
		linkUnlinkWindows(s);
		linkSetPeekFilter(s, false);
		s->linkSavedVis.clear();
		s->linkOn = false; s->linkAProp = s->linkBProp = nullptr;
		refreshGridColorbar(s);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}
	linkUnlinkWindows(s);                              // re-arming always starts from a clean pair
	std::vector<SwipeLayer> L = swipeLayers(s);
	if (L.size() >= 2) {
		int cur = swipeCurrentIndex(L);
		if (cur < 0) cur = (int)L.size() - 1;          // nothing visible -> link the last one added
		int other = -1;
		if (L.size() == 2) other = (cur == 0) ? 1 : 0;
		else               other = swipePartnerDialog(s, L, cur);   // SAME dialog Swipe uses (shape is identical)
		if (other < 0) { act->setChecked(false); return; }
		// The restriction the user asked for: the two do not need to be the same, only to INTERSECT —
		// otherwise there is nothing sensible to switch to.
		const SwipeLayer &A = L[cur], &B = L[other];
		if (A.x1 <= B.x0 || A.x0 >= B.x1 || A.y1 <= B.y0 || A.y0 >= B.y1) {
			if (s->win) s->win->statusBar()->showMessage(
				QString("Link: \"%1\" and \"%2\" don't overlap").arg(QString::fromStdString(A.name), QString::fromStdString(B.name)), 4000);
			act->setChecked(false);
			return;
		}
		s->linkSavedVis.clear();
		for (const SwipeLayer &lay : L) {
			s->linkSavedVis.push_back({ lay.prop, lay.prop->GetVisibility() });
			sceneLayerSetVisibleByProp(s, lay.prop, lay.prop == A.prop);   // start on the CURRENTLY displayed one
		}
		s->linkAProp = A.prop; s->linkBProp = B.prop;
		s->linkOn = true;
		linkSetPeekFilter(s, true);
		refreshGridColorbar(s);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		if (s->win)
			s->win->statusBar()->showMessage(QString("Link: showing \"%1\" — hold the right button to see \"%2\"")
			        .arg(QString::fromStdString(A.name), QString::fromStdString(B.name)), 4000);
		return;
	}
	// --- only one raster here: the partner is another WINDOW -------------------------------------
	std::vector<Scene*> cand = linkPartnerWindows(s);
	if (L.empty() || cand.empty()) {
		if (s->win) s->win->statusBar()->showMessage(
			"Link needs a second grid / image in this window, or another window over the same region", 4000);
		act->setChecked(false);
		return;
	}
	Scene *o = (cand.size() == 1) ? cand.front() : linkPartnerWindowDialog(s, cand);
	if (!o) { act->setChecked(false); return; }
	s->linkPartnerScene = o; o->linkPartnerScene = s;   // both sides: the gesture works from either
	s->linkOn = true;       o->linkOn = true;
	linkSetPeekFilter(s, true); linkSetPeekFilter(o, true);
	swipeSetMode(o, true);                              // partner's own slot must route to Link too
	linkMirrorToggle(o, true);                          // ... and LOOK on, without re-running the pairing
	if (s->win)
		s->win->statusBar()->showMessage(QString("Link: hold the right button to see \"%1\" here-framed")
		        .arg(o->win->windowTitle()), 4000);
}

// THE gesture, driven by LinkPeekFilter's raw right-button press (on = true) and release (false):
// hold to see the OTHER one, let go to snap back to the one you started on. Not a toggle — the
// starting layer/window is fixed at arm time (`linkAProp`, and `s` itself for a pair of windows), so
// the peek always ends where it began no matter how many times it is held. Idempotent: a repeated
// press or a release with nothing held does nothing.
//   IN-WINDOW pair    — show B while held, A when released. The camera is NEVER touched, so
//                       whatever region you were looking at stays exactly as framed.
//   CROSS-WINDOW pair — while held, raise the partner window with its camera pointed at the region
//                       THIS window is currently showing (sceneVisibleRegion -> cameraFitToScaledBBox,
//                       both 10_geometry.cpp — the SAME camera-fit maths gmtvtk_reframe_z_h uses,
//                       never a second copy); on release raise this one back. Only x/y are synced:
//                       the partner keeps its OWN z bounds (read back through surfGetBounds, which
//                       resolves the active layer's Z per SACRED_LAW's derived-variable axes law) and
//                       its own axes/frame are left untouched — this points a camera, it does not
//                       redefine what that window is.
static void linkPeek(Scene *s, bool on) {
	if (!s || !s->linkOn || s->linkPeeking == on) return;
	if (s->linkPartnerScene) {
		Scene *o = s->linkPartnerScene;
		if (!sceneAlive(o) || !o->win || !o->ren) {      // the paired window was closed meanwhile
			if (s->swipeAct) s->swipeAct->setChecked(false);   // runs linkToggled(s, act, false)
			if (s->win) s->win->statusBar()->showMessage("Link: the paired window was closed", 3000);
			return;
		}
		s->linkPeeking = on;
		if (!on) {                                       // released: back to the window you started in
			if (s->win) { s->win->raise(); s->win->activateWindow(); }
			return;
		}
		double W, E, S, N;
		if (sceneVisibleRegion(s, W, E, S, N)) {
			const double w = std::max(W, o->x0), e = std::min(E, o->x1);   // clamp to the partner's own frame
			const double y0 = std::max(S, o->y0), y1 = std::min(N, o->y1);
			if (e > w && y1 > y0) {
				double b[6];
				surfGetBounds(o, b);                       // partner's own SCALED bounds -> keep its z
				b[0] = w * o->xfac; b[1] = e * o->xfac;
				b[2] = y0;          b[3] = y1;
				cameraFitToScaledBBox(o, b, true);
				if (o->widget && o->widget->renderWindow()) o->widget->renderWindow()->Render();
			}
		}
		o->win->raise();
		o->win->activateWindow();
		return;
	}
	std::vector<SwipeLayer> L = swipeLayers(s);
	const int ia = swipeIndexOf(L, s->linkAProp), ib = swipeIndexOf(L, s->linkBProp);
	if (ia < 0 || ib < 0) {                             // one of the pair was deleted while Link was on
		s->linkPeeking = false;
		if (s->swipeAct) s->swipeAct->setChecked(false);   // runs linkToggled(s, act, false) via the toggle chain
		if (s->win) s->win->statusBar()->showMessage("Link: one of the paired layers was removed", 3000);
		return;
	}
	s->linkPeeking = on;
	sceneLayerSetVisibleByProp(s, L[ia].prop, !on);              // A is home, B is the one you peek at
	sceneLayerSetVisibleByProp(s, L[ib].prop, on);
	refreshGridColorbar(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}
