// 57_swipe.cpp — the Swipe tool: compare TWO rasters (grids and/or images) in one window by
// dragging a vertical divider across the scene (the ArcGIS StoryMaps "swipe" gesture).
//
// Mechanism: ONE camera-aligned cut plane, applied to BOTH paired layers with opposite normals —
// the left layer keeps the half left of the divider, the right layer keeps the half right of it.
// Because the two halves never overlap there is no z-fighting and no dependence on which layer
// happens to be higher in the grid pile: the pair can be any two rasters, in any order, a grid
// against an image just as well as grid against grid.
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

// One raster layer of a window as the swipe tool sees it: the base surface/image plus every
// dropped grid/image. `prop` is the identity + visibility handle (exactly what the Scene Objects
// group row toggles); `actors` are the renderables whose mappers carry the cut plane.
struct SwipeLayer {
	vtkProp3D *prop = nullptr;
	std::vector<vtkActor*> actors;
	std::string name;
	int  rank = 0;              // grid-pile rank; picks the "currently displayed" layer
	bool visible = false;
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
			if (swipeIndexOf(L, kv.first) >= 0) kv.first->SetVisibility(kv.second);
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
				lay.prop->SetVisibility((lay.prop == a || lay.prop == b) ? 1 : 0);
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

// Keep the toolbar button's enabled state honest: the tool needs two rasters. Called at the end of
// every rebuildSceneObjects, i.e. after every add / delete / visibility change. Switching the tool
// off is deferred to the event loop so this never re-enters the rebuild that called it.
static void swipeRefreshAvailability(Scene *s) {
	if (!s || !s->swipeAct) return;
	const size_t n = swipeLayers(s).size();
	s->swipeAct->setEnabled(n >= 2);
	if (s->swipeOn && n < 2) {
		QAction *act = s->swipeAct;
		QTimer::singleShot(0, act, [act]() { act->setChecked(false); });
	}
}
