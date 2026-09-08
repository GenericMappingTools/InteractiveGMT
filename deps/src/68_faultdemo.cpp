// Qt/VTK view for the Julia fault-plane model; only camera interaction is installed.
// Julia reconstructs both meshes when dip changes; azimuth and slip use actor transforms.
using JuliaFaultDemoFn = int (*)(double, double, double, double, double *, char *, int);
using JuliaFaultDemoMeshFn = int (*)(double, double *, int, int *, char *, int);
static JuliaFaultDemoFn g_juliaFaultDemo = nullptr;
static JuliaFaultDemoMeshFn g_juliaFaultDemoMesh = nullptr;

static void faultDemoTrace(const char *line);   // gesture probe sink (defined below)

// The demo answers to the SAME mouse controls as every other iGMT 3-D view: left-drag rotates, right
// dollies and the wheel zooms (vtkInteractorStyleTrackballCamera, exactly what buildAndShow installs
// under the gizmo), and the MIDDLE button pans on a drag / recentres the rotation point on a click.
// The middle button never reaches VTK's observers, so the main window handles it in its own widget
// (GLView, 60_profile.cpp) — this one does the same, through the same camPanByDisplay /
// camRecenterOnPick, so there is one implementation of the gesture and not two.
// The demo's 3-D view IS the window's 3-D view: GLView (60_profile.cpp), which already owns the
// middle button — drag pans (camPanByDisplay), a drag-less click recentres on whatever
// sceneRecenterTargets names (camRecenterOnPick) — and hands everything else to VTK, where the
// gizmo's own observers pick it up. This class used to CARRY A COPY of that middle-button code, which
// is exactly the fork SACRED_LAW.md forbids: the gesture then had two implementations, and the one in
// here drifted from the one that works in the main window. It adds ONE thing GLView cannot know
// about: this dialog is full of sliders and spin boxes that steal the keyboard, so entering the view
// takes the focus back — without it 'c', 'x' and the view keys go to a SLIDER instead of the gizmo.
class FaultDemoView : public GLView {
public:
	FaultDemoView(QWidget *parent = nullptr) : GLView() { setParent(parent); }
	// THE WORLD POINT THE HANDLE STANDS ON. `Gizmo::panOff` is an offset from the camera's focal point,
	// and in this window something zeroes it once per mouse-move: VTK's own interactor adapter forwards
	// the drag to the trackball style, whose InteractionEvent runs the gizmo's ResetViewCB. Banking a
	// per-step offset therefore cannot hold — measured, 40 banked steps with panOff back to ~0 on every
	// frame and the handle stuck flickering at the viewport centre. So this view stores WHERE the handle
	// belongs and the offset is re-derived from it before every render (faultDemoAnchorCB), which no
	// reset can undo. It moves only for gestures that redefine the centre of interest: the opening
	// framing, 'c', a view snap, a drag-less middle click.
	double gizAnchor[3] = {0,0,0};
	bool   gizAnchorSet = false;
	int    panBanked = 0;               // probe counter (see the gesture trace below)

protected:
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
	// ...and ONE more, for this dialog's own furniture: the deformation inset is a second renderer in
	// the corner of this same window, and a middle press inside IT belongs to that renderer. Handed to
	// VTK, whose interactor style acts on the renderer the event is poked into, instead of to GLView's
	// pan/recentre for the main one. GLView is untouched: the inset exists only here, so the exception
	// lives only here.
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::MiddleButton && s && s->ren) {
			double x, y; displayPxFromQt(this, renderWindow(), e->position().toPoint(), x, y);
			const bool mine = renIsPoked(interactor(), s->ren, int(x), int(y));
			char buf[256];
			std::snprintf(buf, sizeof(buf), "[faultdemo mid] PRESSED px=(%.0f,%.0f) mine=%d renderers=%d "
			              "giz=%d\n", x, y, mine ? 1 : 0,
			              -1,   // (renderer count needs vtkRendererCollection; not worth an include here)
			              s->giz ? 1 : 0);
			faultDemoTrace(buf);
			if (!mine) {
				QVTKOpenGLNativeWidget::mousePressEvent(e);
				return;
			}
		}
		GLView::mousePressEvent(e);
	}
	// A middle-drag pans; the handle needs no per-step arithmetic here, because faultDemoAnchorCB
	// re-derives its offset from gizAnchor before every render and the anchor does NOT move for a
	// pan — that is exactly what "the handle stays on the body" means.
	void mouseMoveEvent(QMouseEvent *e) override {
		if (midDown) ++panBanked;                   // probe: a drag step was seen
		GLView::mouseMoveEvent(e);
	}
	// A GESTURE THAT REDEFINES THE CENTRE moves the anchor to wherever the focal point ended up:
	// 'c' and the view-snap keys, and a middle click with no drag (the recentre). Deferred one turn
	// so it reads the camera AFTER the shared handler has moved it.
	void adoptFocalAsAnchor() {
		QTimer::singleShot(0, this, [this]() {
			auto *cam = (s && s->ren) ? s->ren->GetActiveCamera() : nullptr;
			if (!cam) return;
			cam->GetFocalPoint(gizAnchor);
			gizAnchorSet = true;
			renderWindow()->Render();
		});
	}
	void keyPressEvent(QKeyEvent *e) override {
		const int k = e->key();
		if ((k == Qt::Key_C && !(e->modifiers() & Qt::ControlModifier)) ||
		    (k >= Qt::Key_1 && k <= Qt::Key_5))
			adoptFocalAsAnchor();
		GLView::keyPressEvent(e);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const bool clickNoDrag = (e->button() == Qt::MiddleButton && midDown && !midMoved);
		GLView::mouseReleaseEvent(e);
		if (clickNoDrag) adoptFocalAsAnchor();     // the recentre landed: adopt its point
	}
};

struct FaultDemo {
	QDialog *dialog = nullptr;
	FaultDemoView *view = nullptr;
	QSlider *dip = nullptr, *azimuth = nullptr, *rake = nullptr, *slip = nullptr;
	QTimer *timer = nullptr;
	QPushButton *play = nullptr;
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkActor> blocks[2];
	vtkSmartPointer<vtkPolyData> meshes[2];
	vtkSmartPointer<vtkMatrix4x4> matrices[2];
	vtkSmartPointer<vtkActor> arrow;
	BeachballWidget *beach = nullptr;
	int playDirection = 1;
	int meshDip = -1;
	double beachSDR[3] = {-1e9, -1e9, -1e9};   // last mechanism handed to the beachball preview
	// The demo drives the REAL gizmo (20_gizmo.cpp), and the gizmo is written against a Scene: it
	// reads the renderer and widget off it, writes the vertical exaggeration into Scene::ve and calls
	// applyVE. So the dialog owns a Scene of its own — not a copy of the gizmo, not a second set of
	// camera controls, the same ones every iGMT 3-D view has.
	Scene *gizScene = nullptr;
	double lastMats[48] = {};      // the matrices Julia last returned, BEFORE the VE factor
	double veApplied = 1.0;        // the VE currently folded into the actor matrices
	// The COORDINATE KIND the deformation is computed in. It follows whatever filled the Region boxes
	// last — a picked window grid hands over its own (Okada reads distances in km against degrees or
	// against metres depending on it), so the result lands in the reference system of the grid the
	// user aimed at. Degrees by default, which is what the Region defaults are.
	int coordGeog = 1;
	// THE DEMO INSET, top-left corner of the view: a second renderer on its own sub-viewport carrying
	// the Okada field over a zone three times the fault's true size, the fault at its centre. It is a
	// TEACHING picture and goes nowhere near the window this demo was opened from — that is what the
	// "Demo inset" checkbox distinguishes. The field itself is computed by Julia (GMT.okada through
	// InteractiveGMT._okada_demo_field), the same okada the Compute button reaches.
	Scene *hostScene = nullptr;                    // the window this demo belongs to (Julia calls need it)
	vtkSmartPointer<vtkRenderer> insetRen;
	vtkSmartPointer<vtkActor>    insetSurf;
	vtkSmartPointer<vtkTextActor>  insetLabel;
	vtkSmartPointer<vtkCubeAxesActor> insetAxes;
	// Z annotations: vtkVectorText on a camera-facing vtkFollower, so they are the SAME glyphs the cube
	// draws its x/y labels with and read horizontally across the axis. Anchor + width are what
	// faultDemoInsetLabelCB needs to place and size them each frame.
	vtkSmartPointer<vtkFollower>   insetZLab[3];
	// The E/N/Up TRIEDRON on the inset — the same marker the 3-D view carries in its own corner,
	// built by the same faultDemoTriedron() and following the INSET camera instead of the view's.
	// Kept here because it is switched with the "Demo inset" checkbox, exactly like insetRen.
	vtkSmartPointer<vtkOrientationMarkerWidget> compass, insetCompass;
	vtkSmartPointer<vtkVectorText> insetZTxt[3];
	double insetZAnchor[3][3] = {};
	double insetZWidth[3] = {};
};

// Fold Scene::ve into the block + arrow matrices. VE is a WORLD-Z stretch, so it premultiplies:
// row 2 of each row-major 4x4 (the world-Z row, translation included) is scaled by the factor. Doing
// it on the matrix and not with vtkProp3D::SetScale matters — a prop scale is applied INSIDE the user
// matrix, which would exaggerate the blocks but leave the gap and the slip offset unexaggerated, and
// the two walls would then interpenetrate.
// MINIMISING this dialog parks it into the Scene Objects panel of the window it belongs to, which is
// where a parked tool lives (parkTool, 50_scene.cpp: ONE list and ONE row builder for every
// parkable tool). Double-clicking the row, or ticking it, brings the demo back exactly as it was;
// the row's own Remove closes it for good. The X does NOT come here: it closes and destroys.
class FaultDemoParkFilter : public QObject {
public:
	FaultDemoParkFilter(QDialog *d, Scene *host)
		: QObject(d), dlg(d), s(host) {}
	bool closing = false;               // set by Remove: let that close through and destroy for good
protected:
	bool eventFilter(QObject *o, QEvent *e) override;
	QDialog *dlg = nullptr;
	Scene   *s   = nullptr;
};

static void faultDemoPark(QDialog *dlg, Scene *s, FaultDemoParkFilter *pf) {
	if (!s || !dlg) return;
	auto bringBack = [dlg, s]() { unparkTool(s, dlg); dlg->show(); dlg->raise(); dlg->activateWindow(); };
	parkTool(s, dlg, "Fault plane demo", 0, "Fault plane demo — double-click to bring it back",
	         bringBack,
	         [dlg, s, pf, bringBack](const QPoint &at) {
		QMenu m;
		QAction *show = m.addAction("Show");
		QAction *del  = m.addAction("Remove");
		QAction *a = m.exec(at);
		if (a == show)     bringBack();
		else if (a == del) { pf->closing = true; unparkTool(s, dlg); dlg->close(); dlg->deleteLater(); }
	});
}

bool FaultDemoParkFilter::eventFilter(QObject *o, QEvent *e) {
	if (o != dlg || closing) return QObject::eventFilter(o, e);
	// ESCAPE DOES NOTHING. By default it reaches QDialog::reject(), which hides the window without
	// even sending a QEvent::Close, so the demo just disappeared under a stray key. Swallowed here:
	// no reject, no hide, no park. The dialog goes away only through the X (which destroys it) or the
	// title-bar minimise (which parks it and leaves a Scene Objects row behind).
	if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape)
		return true;
	// ENTER/RETURN DOES NOTHING EITHER. In a QDialog, Return from any child — a QSpinBox included —
	// walks up to the dialog and fires the first autoDefault QPushButton it finds, which here is Play:
	// typing a dip and pressing Enter started the animation. Proven in 2026-07-10 (NswingDialog):
	// setAutoDefault(false)/setDefault(false) on the buttons is NOT enough on its own, the key must be
	// swallowed before QDialog::keyPressEvent sees it. Only-action-button-executes: a button runs when
	// it is clicked, never because a key was pressed somewhere else.
	if (e->type() == QEvent::KeyPress) {
		const int k = static_cast<QKeyEvent *>(e)->key();
		if (k == Qt::Key_Return || k == Qt::Key_Enter) return true;
	}
	// THE X CLOSES THIS DIALOG FOR GOOD (WA_DeleteOnClose, see faultDemoOpen) — it is NOT a park.
	// It used to swallow QEvent::Close and park instead, which meant the demo could never be
	// dismissed by the one gesture that dismisses every other window. Parking is still there,
	// on the title-bar MINIMISE (parkOnMinimise, below) — the gesture that actually means
	// "put it away for now".
	// The TITLE BAR's minimise button parks, instead of dropping the dialog into the taskbar where the
	// Scene Objects row would not know about it: come straight back out of the minimised state and
	// park. That is now the ONLY route to a park — the X destroys.
	// (The title-bar minimise is NOT handled here any more: it goes through parkOnMinimise,
	// 50_scene.cpp, the one implementation every parkable tool shares. This filter's own copy called
	// showNormal() inside the state change and killed the dialog instead of parking it.)
	return QObject::eventFilter(o, e);
}

// THE mechanism the demo is showing. A NEGATIVE slip runs the hanging wall the other way along the
// very same slip line, which is the mechanism with rake + 180 — so this, and not the slider's own
// rake, is what the beachball draws AND what the Okada request carries. One function: the picture and
// the computation can never describe different mechanisms.
static double faultDemoRake(FaultDemo *f) {
	double rake = f->rake->value();
	if (f->slip->value() < 0) rake += 180.0;
	if (rake > 180.0) rake -= 360.0;                     // back into the slider's own -180..180
	return rake;
}

// What the demo actually shows: the two blocks and the rake arrow, as currently transformed. NOT the
// renderer's visible props — the gizmo floats above the focal point and scales with the camera, so
// counting it would let the framing chase the handle instead of the model.
static bool faultDemoBounds(FaultDemo *f, double bb[6]) {
	bb[0] = bb[2] = bb[4] =  VTK_DOUBLE_MAX;
	bb[1] = bb[3] = bb[5] = -VTK_DOUBLE_MAX;
	for (vtkActor *a : {f->blocks[0].Get(), f->blocks[1].Get(), f->arrow.Get()}) {
		if (!a) continue;
		double b[6]; a->GetBounds(b);
		for (int i = 0; i < 3; ++i) {
			bb[2*i]   = std::min(bb[2*i],   b[2*i]);
			bb[2*i+1] = std::max(bb[2*i+1], b[2*i+1]);
		}
	}
	return bb[1] >= bb[0];
}

static void faultDemoApplyVE(FaultDemo *f) {
	const double ve = (f->gizScene && f->gizScene->ve > 0.0) ? f->gizScene->ve : 1.0;
	double m[48];
	std::copy(f->lastMats, f->lastMats + 48, m);
	for (int k = 0; k < 3; ++k)
		for (int j = 0; j < 4; ++j) m[k*16 + 8 + j] *= ve;
	for (int b = 0; b < 2; ++b) f->matrices[b]->DeepCopy(m + b*16);
	vtkNew<vtkMatrix4x4> arrow;
	arrow->DeepCopy(m + 32);
	f->arrow->SetUserMatrix(arrow);
	f->veApplied = ve;
	// Tell the Scene what its content is. surfGetBounds reads this override, and through it the gizmo
	// and its view-snap keys frame THIS model — the same field a cropped grid uses to say "the frame
	// is mine now", not a second bounds path written for the demo.
	double bb[6];
	if (f->gizScene && faultDemoBounds(f, bb)) {
		for (int i = 0; i < 6; ++i) f->gizScene->viewBounds[i] = bb[i];
		f->gizScene->viewBoundsOverride = true;
	}
}

// Copy Julia's reconstructed triangles into the existing VTK inputs; normals update downstream.
static bool faultDemoRebuild(FaultDemo *f, int dip, QString &error) {
	int counts[2] = {};
	char message[1024] = {};
	if (!g_juliaFaultDemoMesh || !g_juliaFaultDemoMesh(dip, nullptr, 0, counts, message, sizeof(message))) {
		error = message[0] ? QString::fromUtf8(message) : QString("Julia fault mesh callback is not registered.");
		return false;
	}
	std::vector<double> vertices(9*(counts[0]+counts[1]));
	if (!g_juliaFaultDemoMesh(dip, vertices.data(), int(vertices.size()), counts, message, sizeof(message))) {
		error = QString::fromUtf8(message); return false;
	}
	size_t offset = 0;
	for (int b = 0; b < 2; ++b) {
		vtkNew<vtkPoints> points;
		points->SetDataTypeToDouble();
		vtkNew<vtkCellArray> triangles;
		for (int t = 0; t < counts[b]; ++t) {
			vtkIdType ids[3];
			for (int j = 0; j < 3; ++j) {
				ids[j] = points->InsertNextPoint(vertices.data()+offset);
				offset += 3;
			}
			triangles->InsertNextCell(3, ids);
		}
		if (!f->meshes[b]) f->meshes[b] = vtkSmartPointer<vtkPolyData>::New();
		f->meshes[b]->SetPoints(points);
		f->meshes[b]->SetPolys(triangles);
		f->meshes[b]->Modified();
	}
	f->meshDip = dip;
	return true;
}

// Julia returns the two block matrices and the rake arrow matrix, all row-major.
static bool faultDemoUpdate(FaultDemo *f) {
	QString meshError;
	if (f->meshDip != f->dip->value() && !faultDemoRebuild(f, f->dip->value(), meshError)) {
		f->play->setChecked(false);
		QMessageBox::warning(f->dialog, "Fault plane demo", meshError);
		return false;
	}
	double matrices[48];
	char error[1024] = {};
	if (!g_juliaFaultDemo || !g_juliaFaultDemo(f->azimuth->value(), f->dip->value(),
		f->rake->value(), f->slip->value(), matrices, error, sizeof(error))) {
		f->play->setChecked(false);
		QMessageBox::warning(f->dialog, "Fault plane demo",
			error[0] ? QString::fromUtf8(error) : QString("Julia fault demo callback is not registered."));
		return false;
	}
	std::copy(matrices, matrices + 48, f->lastMats);
	faultDemoApplyVE(f);                 // the gizmo's vertical exaggeration rides on top
	f->renderer->ResetCameraClippingRange();
	f->view->renderWindow()->Render();
	// The beachball depends on strike/dip/rake ALONE, and refreshing it costs a Julia round-trip
	// (BeachballWidget::refreshPrecise -> _focal_demo_sectors). Play animates the SLIP slider, 33
	// frames a second: without this guard every one of those frames recomputes an identical ball.
	const double sdr[3] = {double(f->azimuth->value()), double(f->dip->value()), faultDemoRake(f)};
	if (sdr[0] != f->beachSDR[0] || sdr[1] != f->beachSDR[1] || sdr[2] != f->beachSDR[2]) {
		f->beachSDR[0] = sdr[0]; f->beachSDR[1] = sdr[1]; f->beachSDR[2] = sdr[2];
		f->beach->setMechanism(sdr[0], sdr[1], sdr[2]);
	}
	return true;
}

// ── The demo inset (top-left of the view) ─────────────────────────────────────────────────────
// A SECOND renderer on its own sub-viewport of the same render window: same trick the compass marker
// uses, so the demo camera's bounds and the model's framing are untouched by whatever the inset shows.
// It is non-interactive (the interactor never hands it a drag) and it clears its own rectangle, so it
// reads as a panel laid over the corner rather than as part of the scene.
// StartEvent on the inset renderer: size and place the z annotations exactly as the cube's own labels.
// THE FORMULA IS THE CUBE'S, not a second one — vtkAxisFollower::AutoScale, with the actor's own
// ScreenSize: a world scale of `2*ScreenSize*tan(viewAngle/2)/viewportHeight * distance`, recomputed
// every frame, which is what holds a vtkVectorText glyph at a constant pixel height. Matching x/y and z
// by choosing a font size instead cannot work: one is world geometry rescaled per frame, the other a
// rasterised face at a fixed point size, and they drifted apart by 2-3x.
// Placement rides along: the text hangs LEFT of its tick by its own measured width, along the screen's
// right vector (up x view-plane-normal), so it stays outside the box whichever way the inset is turned.
// StartEvent on the demo's renderer, registered ABOVE the gizmo's own PlaceCB (priority 1.0 against
// its default 0.0) so it runs FIRST every frame: the handle's offset from the focal point is
// re-derived from the world anchor the view holds. PlaceCB then places the handle at
// focal + panOff == the anchor, whatever anything else did to panOff in between — and something does,
// once per mouse-move (see FaultDemoView::gizAnchor). Nothing here moves a camera or an actor.
static void faultDemoAnchorCB(vtkObject *caller, unsigned long, void *cd, void *) {
	auto *f = static_cast<FaultDemo *>(cd);
	auto *ren = vtkRenderer::SafeDownCast(caller);
	if (!f || !f->view || !f->view->gizAnchorSet || !ren) return;
	Scene *s = f->gizScene;
	vtkCamera *cam = ren->GetActiveCamera();
	if (!s || !s->giz || !cam) return;
	double fp[3]; cam->GetFocalPoint(fp);
	for (int i = 0; i < 3; ++i) s->giz->panOff[i] = f->view->gizAnchor[i] - fp[i];
}

// Put the inset triedron in the inset's LOWER-LEFT corner and keep it there. The ABSOLUTE rect is
// written straight onto the marker's own renderer: its normal placement is a RELATIVE Viewport
// times whatever CurrentRenderer happens to be, and VTK moves that target on its own
// (SetEnabled(0) nulls it, the next enable re-resolves it to the poked renderer = the big 3-D
// view). Driven from BOTH renderers' StartEvent, so it is re-applied on every frame the window
// draws, whether or not the inset renderer itself emitted one.
static void faultDemoPinInsetTriedron(FaultDemo *f) {
	if (!f || !f->insetCompass || !f->insetRen) return;
	vtkRenderer *mr = f->insetCompass->GetRenderer();
	if (!mr) return;
	double v[4];
	f->insetRen->GetViewport(v);                 // the panel's live rect, so an inset resize follows
	const double w = v[2] - v[0], h = v[3] - v[1];
	// WHOLLY INSIDE THE PANEL, flush in its bottom-left corner. The marker's ORIGIN sits at the CENTRE
	// of its rect (the widget frames the axes actor as if its bounds were symmetric about the origin),
	// so the rect is kept SMALL — that is what brings the triedron close to the corner, since half the
	// rect is always the gap between corner and origin. SetZoom (faultDemoTriedron) then fills that
	// small rect, so small does not mean a speck.
	const double bw = 0.16 * w, bh = 0.20 * h;
	mr->SetViewport(v[0], v[1], v[0] + bw, v[1] + bh);
}

static void faultDemoInsetLabelCB(vtkObject *caller, unsigned long, void *cd, void *) {
	auto *f = static_cast<FaultDemo *>(cd);
	auto *ren = vtkRenderer::SafeDownCast(caller);
	if (!f || !ren) return;
	faultDemoPinInsetTriedron(f);                // before every early return below
	if (!f->insetAxes || !ren->GetActiveCamera()) return;
	vtkCamera *cam = ren->GetActiveCamera();
	const int *sz = ren->GetSize();
	if (!sz || sz[1] <= 0) return;
	const double factor = 2.0 * f->insetAxes->GetScreenSize() *
	                      std::tan(vtkMath::RadiansFromDegrees(cam->GetViewAngle() * 0.5)) / sz[1];
	double cp[3], up[3], vpn[3], right[3];
	cam->GetPosition(cp);  cam->GetViewUp(up);  cam->GetViewPlaneNormal(vpn);
	vtkMath::Cross(up, vpn, right);
	if (vtkMath::Normalize(right) <= 0.0) return;
	for (int k = 0; k < 3; ++k) {
		if (!f->insetZLab[k]) continue;
		const double *a = f->insetZAnchor[k];
		const double s = factor * std::sqrt(vtkMath::Distance2BetweenPoints(a, cp));
		if (!(s > 0.0)) continue;
		const double back = (f->insetZWidth[k] + 0.5) * s;   // its own width + half a glyph of air
		f->insetZLab[k]->SetScale(s);
		f->insetZLab[k]->SetPosition(a[0] - back*right[0] - 0.35*s*up[0],
		                             a[1] - back*right[1] - 0.35*s*up[1],
		                             a[2] - back*right[2] - 0.35*s*up[2]);
	}
}

// THE triedron builder for this dialog. The 3-D view and the deformation inset each carry an
// E/N/Up marker; they differ only in WHICH camera they follow and WHERE they sit, so they are
// built here once instead of twice (a second copy is how the two would end up with different
// labels or a different size). `follow` is the renderer whose camera the marker tracks — set
// BEFORE SetEnabled, or the widget picks the poked renderer on its own. The viewport is given in
// coords RELATIVE TO the followed renderer's own viewport (VTK scales them into it — so for a
// renderer that owns a corner of the window, 1.0 is that CORNER's width, not the window's).
static vtkSmartPointer<vtkOrientationMarkerWidget>
faultDemoTriedron(vtkRenderWindowInteractor *rwi, vtkRenderer *follow,
                  double x0, double y0, double x1, double y1) {
	if (!rwi || !follow) return nullptr;
	vtkNew<vtkAxesActor> axes;
	axes->SetXAxisLabelText("E"); axes->SetYAxisLabelText("N"); axes->SetZAxisLabelText("Up");
	auto w = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
	w->SetOrientationMarker(axes);
	w->SetInteractor(rwi);
	w->SetCurrentRenderer(follow);
	w->SetViewport(x0, y0, x1, y1);
	w->SetEnabled(1);
	w->InteractiveOff();          // a fixed marker, never a widget the mouse can grab and move
	// FILL the box. vtkAxesActor draws its three arms in +X/+Y/+Z ONLY, but the widget frames it as
	// if its bounds were symmetric about the origin (the class documents that requirement), so half
	// the box is empty space in the negative directions and the marker floats well inside its own
	// rectangle. Zoom pushes the arms out to the box edges, which is what puts the triedron IN the
	// corner instead of a marker-width away from it.
	w->SetZoom(2.0);
	return w;
}

// Switch one of those markers on/off. NOT symmetric, and this is why it needs a function of its own:
// VTK's SetEnabled(0) NULLS the widget's CurrentRenderer, and the next SetEnabled(1) re-resolves it
// with FindPokedRenderer — which on this window is the big 3-D view. The inset's marker therefore
// left the inset and reappeared in the VIEW's corner (its viewport is computed relative to whatever
// CurrentRenderer is, see UpdateInternalViewport), following the wrong camera. Re-assert the
// renderer every time it is switched back on.
static void faultDemoTriedronShow(vtkOrientationMarkerWidget *w, vtkRenderer *follow, bool on) {
	if (!w) return;
	if (!on) { w->SetEnabled(0); return; }
	w->SetCurrentRenderer(follow);
	w->SetEnabled(1);
}

static void faultDemoInsetInit(FaultDemo *f, vtkRenderWindow *rw) {
	if (!f || !rw) return;
	f->insetRen = vtkSmartPointer<vtkRenderer>::New();
	// 25% wider and taller than the first cut (0.32 x 0.34 of the view): the axes and their annotations
	// need room of their own, and this panel is where the deformation is actually read.
	f->insetRen->SetViewport(0.0, 0.575, 0.40, 1.0);
	// A drag, a wheel or a middle-click inside this rectangle drives the INSET camera and nothing else.
	// Two halves make that true: this renderer answers FindPokedRenderer (Interactive, set with Draw
	// below), which is how the interactor style picks the camera to drive; and the handlers that used to
	// assume a single renderer — DragCB and ResetViewCB (20_gizmo.cpp), FaultDemoView's middle button —
	// now ask renIsPoked() first, so the gizmo and the body stay out of gestures aimed at the corner.
	f->insetRen->InteractiveOff();
	f->insetRen->SetBackground(0.97, 0.97, 0.94);
	f->insetRen->GetActiveCamera()->ParallelProjectionOn();
	f->insetLabel = vtkSmartPointer<vtkTextActor>::New();
	f->insetLabel->SetInput("Demo inset: Okada over 3x the fault");
	f->insetLabel->GetTextProperty()->SetFontSize(11);
	f->insetLabel->GetTextProperty()->SetColor(0.15, 0.15, 0.15);
	f->insetLabel->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
	f->insetLabel->GetPositionCoordinate()->SetValue(0.04, 0.90);
	f->insetRen->AddActor2D(f->insetLabel);
	// The z annotations are re-sized and re-placed every frame from the cube's own AutoScale formula.
	vtkNew<vtkCallbackCommand> zlabCB;
	zlabCB->SetCallback(faultDemoInsetLabelCB);
	zlabCB->SetClientData(f);
	f->insetRen->AddObserver(vtkCommand::StartEvent, zlabCB);
	rw->AddRenderer(f->insetRen);          // added AFTER the main one: it paints over that corner
	f->insetRen->DrawOff();                // the checkbox turns it on; the demo opens without it
	// Draw and Interactive move TOGETHER, always: a hidden renderer that still answers FindPokedRenderer
	// would swallow every click in that corner of an empty view.
}

// Compute the inset field and (re)build its surface. The DEFORMATION is Julia's — the same GMT.okada
// the Compute button reaches, called through g_juliaEval as InteractiveGMT._okada_demo_field (see
// src/deform.jl for the payload contract) — so there is no second Okada on this side of the wire, only
// a mesh built from the nodes it returns. NOTHING is added to the host window: that is the whole point
// of the inset, and what tells a teaching picture apart from the real deformation.
static bool faultDemoInsetCompute(FaultDemo *f, double L, double W, double depthTop, double slip,
                                  QString &err) {
	if (!f || !f->insetRen) { err = "the inset is not built."; return false; }
	if (!g_juliaEval || !f->hostScene) { err = "the deformation tool is not wired."; return false; }
	const QString cmd = QString("InteractiveGMT._okada_demo_field(%1,%2,%3,%4,%5,%6,%7)")
	                        .arg(L, 0, 'f', 6).arg(W, 0, 'f', 6)
	                        .arg(double(f->azimuth->value()), 0, 'f', 6)
	                        .arg(double(f->dip->value()), 0, 'f', 6)
	                        .arg(depthTop, 0, 'f', 6)
	                        .arg(faultDemoRake(f), 0, 'f', 6).arg(slip, 0, 'f', 6);
	std::vector<char> buf(1 << 20);
	const int n = g_juliaEval(f->hostScene, cmd.toUtf8().constData(), buf.data(), int(buf.size()));
	if (n <= 0) { err = "the inset deformation returned nothing (see the log)."; return false; }
	const QStringList tok = QString::fromUtf8(buf.data(), n).split(';', Qt::SkipEmptyParts);
	if (tok.size() < 8) { err = "the inset deformation returned a short payload."; return false; }
	const int nx = tok[0].toInt(), ny = tok[1].toInt();
	const double x0 = tok[2].toDouble(), x1 = tok[3].toDouble();
	const double y0 = tok[4].toDouble(), y1 = tok[5].toDouble();
	const double zmn = tok[6].toDouble(), zmx = tok[7].toDouble();
	if (nx < 2 || ny < 2 || tok.size() < 8 + nx*ny || x1 <= x0 || y1 <= y0) {
		err = "the inset deformation returned an unusable grid."; return false;
	}
	// The relief is EXAGGERATED and says so on the label: centimetres of deformation across tens of
	// kilometres would otherwise be a flat sheet. The factor is chosen from the field's own amplitude,
	// so the picture reads the same whatever the slip is.
	const double amp  = std::max(std::abs(zmn), std::abs(zmx));
	const double exag = amp > 0.0 ? 0.18 * (x1 - x0) / amp : 0.0;
	const double dx = (x1 - x0) / (nx - 1), dy = (y1 - y0) / (ny - 1);
	vtkNew<vtkPoints> pts;      pts->SetNumberOfPoints(vtkIdType(nx) * ny);
	vtkNew<vtkFloatArray> vals; vals->SetNumberOfTuples(vtkIdType(nx) * ny);
	vals->SetName("Okada z");
	for (int j = 0; j < nx; ++j)                     // x-major, y fastest: the payload's own order
		for (int i = 0; i < ny; ++i) {
			const double v = tok[8 + j*ny + i].toDouble();
			const vtkIdType id = vtkIdType(j)*ny + i;
			pts->SetPoint(id, x0 + j*dx, y0 + i*dy, v * exag);
			vals->SetTuple1(id, v);
		}
	vtkNew<vtkCellArray> quads;
	for (int j = 0; j + 1 < nx; ++j)
		for (int i = 0; i + 1 < ny; ++i) {
			const vtkIdType q[4] = { vtkIdType(j)*ny + i,     vtkIdType(j+1)*ny + i,
			                         vtkIdType(j+1)*ny + i+1, vtkIdType(j)*ny + i+1 };
			quads->InsertNextCell(4, q);
		}
	auto pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts); pd->SetPolys(quads); pd->GetPointData()->SetScalars(vals);
	// Diverging ramp pinned on ZERO deformation, through the ONE transfer-function builder every grid
	// in this application is coloured by (makeGridCTF, 10_geometry.cpp).
	const double lim = amp > 0.0 ? amp : 1.0;
	const double cz[3]   = { -lim, 0.0, lim };
	const double crgb[9] = { 0.19, 0.31, 0.75,  0.97, 0.97, 0.97,  0.78, 0.13, 0.10 };
	auto ctf = makeGridCTF(nullptr, cz, crgb, 3);
	if (!f->insetSurf) {
		vtkNew<vtkPolyDataMapper> mapper;
		f->insetSurf = vtkSmartPointer<vtkActor>::New();
		f->insetSurf->SetMapper(mapper);
		f->insetRen->AddActor(f->insetSurf);
	}
	auto *mapper = vtkPolyDataMapper::SafeDownCast(f->insetSurf->GetMapper());
	mapper->SetInputData(pd);
	mapper->SetLookupTable(ctf);
	mapper->SetScalarModeToUsePointData();
	mapper->SetColorModeToMapScalars();
	mapper->ScalarVisibilityOn();
	mapper->SetScalarRange(-lim, lim);
	f->insetSurf->GetProperty()->SetAmbient(0.35);
	f->insetSurf->GetProperty()->SetDiffuse(0.65);
	// AXES on the inset. A plain self-labelling vtkCubeAxesActor, NOT the window's axes machinery
	// (axesBuild, 10_geometry.cpp): that builds a RASTER's axes inside a Scene — VE-scaled box, billboard
	// titles, tick actors, the whole per-raster set — and this panel has no Scene, no raster and no VE.
	// Different operation, so a different actor; what it must not do is re-derive the trick that IS
	// shared, and it does not: the box stands on the WORLD (metre, exaggerated-z) bounds while the
	// LABELS come from SetXAxisRange/SetYAxisRange/SetZAxisRange, exactly as axesBuild does for the same
	// reason — true numbers on a scaled actor. Labels are kilometres across and metres of deformation up.
	if (!f->insetAxes) {
		f->insetAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
		f->insetAxes->SetCamera(f->insetRen->GetActiveCamera());
		f->insetAxes->SetFlyModeToOuterEdges();
		// The cube keeps its X and Y labels but NOT its Z ones. Every native label is a vtkAxisFollower laid
		// ALONG its own axis (vtkAxisFollower.cxx translates a Z-type label by half its width in z), so on a
		// short vertical axis the numbers run up the line and into each other. Turning the whole actor to
		// Use2DMode fixes that by wrecking everything else — it drops the xz/yz tick geometry and re-lays
		// all three axes for a flat view. So the Z annotations are drawn the way the WINDOW's axes already
		// draw theirs (axesBuild, 10_geometry.cpp: native labels off, billboards instead): screen-aligned
		// vtkBillboardTextActor3D, which is horizontal by construction — across the axis, never along it.
		f->insetAxes->SetZAxisLabelVisibility(false);
		// Each axis SAYS WHICH ONE IT IS, units included. X/Y (not lon/lat) because the demo zone is a
		// cartesian teaching frame centred on the fault, which is the same naming rule the window's axes
		// follow for cartesian data (axesBuild: geog -> lon/lat, cartesian -> X/Y).
		f->insetAxes->SetXTitle("X (km)");
		f->insetAxes->SetYTitle("Y (km)");
		f->insetAxes->SetZTitle("Z (m)");
		for (int i = 0; i < 3; ++i) {
			f->insetAxes->GetTitleTextProperty(i)->SetColor(0.15, 0.15, 0.15);
			f->insetAxes->GetLabelTextProperty(i)->SetColor(0.20, 0.20, 0.20);
		}
		f->insetAxes->GetXAxesLinesProperty()->SetColor(0.35, 0.35, 0.35);
		f->insetAxes->GetYAxesLinesProperty()->SetColor(0.35, 0.35, 0.35);
		f->insetAxes->GetZAxesLinesProperty()->SetColor(0.35, 0.35, 0.35);
		f->insetRen->AddActor(f->insetAxes);
	}
	// A zero z span makes vtkAxisActor compute an invalid label count and abort the render — the same
	// guard axesSetBounds keeps for the window's own cube, and for the same reason (zero slip is a
	// perfectly reasonable thing to ask this demo for).
	double zlo = -amp*exag, zhi = amp*exag;
	if (zhi - zlo < 1e-9) { zlo = -1.0; zhi = 1.0; }
	f->insetAxes->SetBounds(x0, x1, y0, y1, zlo, zhi);
	f->insetAxes->SetXAxisRange(x0/1000.0, x1/1000.0);      // metres of world -> kilometres on the label
	f->insetAxes->SetYAxisRange(y0/1000.0, y1/1000.0);
	f->insetAxes->SetZAxisRange(amp > 0.0 ? -amp : -1.0, amp > 0.0 ? amp : 1.0);   // TRUE deformation
	// ...and here they are: bottom, zero and top of the z axis, off the box's front-left vertical edge.
	//
	// SAME ACTOR KIND AS THE X AND Y LABELS, which is the only way they can be the same font. Those are
	// the cube's native labels: vtkVectorText glyphs — world-space geometry, not a rasterised typeface —
	// rescaled EVERY FRAME by vtkAxisFollower::AutoScale to hold a constant screen size. A
	// vtkBillboardTextActor3D with a font size in points cannot track that (it was 2-3x too big), so
	// these are vtkVectorText too, on a plain vtkFollower: camera-facing, therefore horizontal ACROSS
	// the axis, with no vtkAxisFollower rotation to lay them along it. Their size is set by
	// faultDemoInsetLabelCB, which runs the cube's own AutoScale formula with the cube's own ScreenSize.
	{
		const double zt = (amp > 0.0) ? amp : 1.0;             // the TRUE metres the box ends stand for
		const double zpos[3] = { zlo, (zlo + zhi) * 0.5, zhi };
		const double zval[3] = { -zt, 0.0, zt };
		double lc[3] = { 0.20, 0.20, 0.20 };
		if (auto *lp = f->insetAxes->GetLabelTextProperty(2)) lp->GetColor(lc);   // the cube's own colour
		for (int k = 0; k < 3; ++k) {
			if (!f->insetZLab[k]) {
				f->insetZTxt[k] = vtkSmartPointer<vtkVectorText>::New();
				vtkNew<vtkPolyDataMapper> lm;
				lm->SetInputConnection(f->insetZTxt[k]->GetOutputPort());
				f->insetZLab[k] = vtkSmartPointer<vtkFollower>::New();
				f->insetZLab[k]->SetMapper(lm);
				f->insetZLab[k]->SetCamera(f->insetRen->GetActiveCamera());
				f->insetZLab[k]->GetProperty()->SetLighting(false);   // a label is not a lit surface
				f->insetRen->AddActor(f->insetZLab[k]);
			}
			f->insetZLab[k]->GetProperty()->SetColor(lc[0], lc[1], lc[2]);
			f->insetZTxt[k]->SetText(QString::number(zval[k], 'g', 3).toUtf8().constData());
			f->insetZTxt[k]->Update();
			// The ANCHOR is the tick's own place on the edge; the callback offsets the text left of it by
			// its own measured width, so the numbers hang outside the box instead of across the surface.
			f->insetZAnchor[k][0] = x0;  f->insetZAnchor[k][1] = y0;  f->insetZAnchor[k][2] = zpos[k];
			double tb[6]; f->insetZTxt[k]->GetOutput()->GetBounds(tb);
			f->insetZWidth[k] = (tb[1] > tb[0]) ? (tb[1] - tb[0]) : 0.0;
		}
	}
	f->insetLabel->SetInput(QString("Demo inset: Okada over 3x the fault\nz %1 .. %2 (x%3 relief)")
	                            .arg(zmn, 0, 'g', 3).arg(zmx, 0, 'g', 3)
	                            .arg(exag, 0, 'g', 2).toUtf8().constData());
	auto *cam = f->insetRen->GetActiveCamera();
	cam->SetFocalPoint(0.0, 0.0, 0.0);
	cam->SetPosition(0.9*(x1-x0), -1.3*(y1-y0), 0.9*(x1-x0));
	cam->SetViewUp(0, 0, 1);
	f->insetRen->ResetCamera();
	// ResetCamera fits the BOX. The annotations live OUTSIDE it — 2-D label text hanging off the bottom
	// and the left of the cube, plus the caption over the top — so the fit is then opened up to leave
	// them their room. Without this the surface fills the panel and the numbers are clipped away.
	cam->Zoom(0.72);
	f->insetRen->ResetCameraClippingRange();
	return true;
}

static void faultDemoCamera(FaultDemo *f) {
	auto *c = f->renderer->GetActiveCamera();
	c->SetPosition(4, -6, 4);
	c->SetFocalPoint(0, 0, 0);
	c->SetViewUp(0, 0, 1);
	c->ParallelProjectionOn();
	// Frame what is actually there instead of a hand-tuned parallel scale: the two lines above fix the
	// view DIRECTION, ResetCamera then fits the model along it, so a change to the model's proportions
	// (the 1:1 plan aspect) cannot leave the demo opening half-empty or clipped. The bounds are taken
	// from the BLOCKS and the arrow, never from the visible props: the gizmo floats above the focal
	// point and would drag the framing off the model — the same reason fitSnapView re-centres on the
	// surface bbox after its own ResetCamera.
	double bb[6];
	if (faultDemoBounds(f, bb)) f->renderer->ResetCamera(bb);
	else                        f->renderer->ResetCamera();
	// Open with room around the body, but not lost in it: 1.12 is a tight fit, times 1.25 = ONE tap of
	// the '-' key. This used to be 2.1875 (three taps out) and the model came up too small — 1.4 is
	// that framing zoomed in TWICE with '+', since a tap is cam->Zoom(1.25) either way (20_gizmo.cpp).
	c->SetParallelScale(c->GetParallelScale()*1.4);
	// The model sits LOW in the frame: the controls column next to it is tall, and the blocks read
	// better with the empty space above them than centred. Moving the camera UP moves the model down.
	{
		double vu[3], fp[3], pos[3];
		c->GetViewUp(vu); c->GetFocalPoint(fp); c->GetPosition(pos);
		const double d = 0.58*c->GetParallelScale();   // fraction of the half-height it drops by
		for (int i = 0; i < 3; ++i) { fp[i] += d*vu[i]; pos[i] += d*vu[i]; }
		c->SetFocalPoint(fp); c->SetPosition(pos);
		// Moving the focal point moves the GIZMO with it (PlaceCB anchors the handle there), which would
		// leave it hanging in the empty space above the blocks. Bank the opposite offset, exactly as the
		// arrow-pan and the +/- zoom do; it is an assignment, not a subtraction, because ResetCamera above
		// has just put the focal point back on the model centre, so this is the whole of the offset.
		if (f->gizScene && f->gizScene->giz)
			for (int i = 0; i < 3; ++i) f->gizScene->giz->panOff[i] = -d*vu[i];
		// ...and that IS the anchor: the model centre this framing put the body on.
		if (f->view) {
			for (int i = 0; i < 3; ++i) f->view->gizAnchor[i] = fp[i] - d*vu[i];
			f->view->gizAnchorSet = true;
		}
	}
	f->renderer->ResetCameraClippingRange();
	f->view->renderWindow()->Render();
}

// The demo's gesture probe writes HERE as well as to stderr: a fixed file, so a trace can be read
// back without anyone having to copy terminal output. Append-only, best effort, never throws.
// Delete-able at any time; it is recreated on the next gesture.
static void faultDemoTrace(const char *line) {
	if (!line) return;
	static const char *path = "C:/TMP/claude/faultdemo_trace.log";
	if (FILE *fp = fopen(path, "a")) { fputs(line, fp); fclose(fp); }
	fputs(line, stderr);
	fflush(stderr);
}

// 'c' DIAGNOSTIC, demo-side and read-only. The key is handled in shared code (KeyCB -> the shared
// camRecenterAtCursor, 20_gizmo.cpp / 10_geometry.cpp) which must not be touched to debug a dialog,
// and three rounds of reasoning about it from the source have been wrong. So this watches the key on
// the view and PRINTS what the shared path was given and what it did with it: nothing here changes a
// camera, a pick or a handle. It answers, in one line, which half is broken —
//   pick_targets=0            -> this scene never named what may be picked
//   focal unchanged           -> the pick missed (compare px= against the view size)
//   focal moved, gizmo lagging-> the handle is not following the focal point
// Prints to stderr, where the [mid] traces in 30_app.cpp already go.
class FaultDemoKeyProbe : public QObject {
public:
	FaultDemoKeyProbe(FaultDemo *fd, QObject *parent) : QObject(parent), f(fd) {}
protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		// MIDDLE-DRAG trace: one line on press, one on release with what actually moved. Read-only.
		if ((e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease) &&
		    f && f->gizScene && f->view &&
		    static_cast<QMouseEvent *>(e)->button() == Qt::MiddleButton) {
			Scene *s = f->gizScene;
			auto *cam = s->ren ? s->ren->GetActiveCamera() : nullptr;
			double fp[3] = {0,0,0};
			if (cam) cam->GetFocalPoint(fp);
			const bool press = (e->type() == QEvent::MouseButtonPress);
			if (press) f->view->panBanked = 0;
			char buf[512]; std::snprintf(buf, sizeof(buf), "[faultdemo mid] %s focal=(%.4g,%.4g,%.4g) panOff=(%.4g,%.4g,%.4g) "
			                "gizmo=(%.4g,%.4g,%.4g) banked=%d midDown-was=%d\n",
			        press ? "PRESS  " : "RELEASE", fp[0], fp[1], fp[2],
			        s->giz ? s->giz->panOff[0] : 0.0, s->giz ? s->giz->panOff[1] : 0.0,
			        s->giz ? s->giz->panOff[2] : 0.0,
			        s->giz ? s->giz->centre[0] : 0.0, s->giz ? s->giz->centre[1] : 0.0,
			        s->giz ? s->giz->centre[2] : 0.0,
			        f->view->panBanked, press ? 0 : 1);
			faultDemoTrace(buf);
		}
		if (e->type() == QEvent::KeyPress && f && f->gizScene && f->view) {
			auto *ke = static_cast<QKeyEvent *>(e);
			if (ke->key() == Qt::Key_C && !(ke->modifiers() & Qt::ControlModifier)) {
				Scene *s = f->gizScene;
				const QPoint lp = f->view->mapFromGlobal(QCursor::pos());
				double dx = 0, dy = 0;
				displayPxFromQt(f->view, f->view->renderWindow(), lp, dx, dy);
				const int *wh = f->view->renderWindow()->GetSize();
				double before[3] = {0,0,0};
				if (auto *cam = s->ren ? s->ren->GetActiveCamera() : nullptr) cam->GetFocalPoint(before);
				char buf[512]; std::snprintf(buf, sizeof(buf), "[faultdemo c] focus=%d qt=(%d,%d) px=(%.1f,%.1f) win=(%d,%d) "
				                "targets=%zu giz=%d focal_before=(%.4g,%.4g,%.4g)\n",
				        f->view->hasFocus() ? 1 : 0, lp.x(), lp.y(), dx, dy, wh[0], wh[1],
				        sceneRecenterTargets(s).size(), s->giz ? 1 : 0,
				        before[0], before[1], before[2]);
				faultDemoTrace(buf);
				// ...and what the shared handler made of it, once the event has been through it.
				FaultDemo *fd = f;
				QTimer::singleShot(0, fd->dialog, [fd]() {
					Scene *s = fd->gizScene;
					auto *cam = (s && s->ren) ? s->ren->GetActiveCamera() : nullptr;
					if (!cam) return;
					double after[3]; cam->GetFocalPoint(after);
					char buf[512]; std::snprintf(buf, sizeof(buf), "[faultdemo c] focal_after=(%.4g,%.4g,%.4g) gizmo=(%.4g,%.4g,%.4g) "
					                "panOff=(%.4g,%.4g,%.4g)\n",
					        after[0], after[1], after[2],
					        s->giz ? s->giz->centre[0] : 0.0, s->giz ? s->giz->centre[1] : 0.0,
					        s->giz ? s->giz->centre[2] : 0.0,
					        s->giz ? s->giz->panOff[0] : 0.0, s->giz ? s->giz->panOff[1] : 0.0,
					        s->giz ? s->giz->panOff[2] : 0.0);
					faultDemoTrace(buf);
				});
			}
		}
		return QObject::eventFilter(o, e);
	}
	FaultDemo *f = nullptr;
};

static QDialog *faultDemoOpen(QWidget *parent, Scene *scene) {
	if (auto *old = qobject_cast<QDialog *>(scene ? scene->faultDemoDlg : nullptr)) {
		unparkTool(scene, old);          // it may be PARKED in Scene Objects: the menu brings it back too
		old->show(); old->raise(); old->activateWindow(); return old;
	}
	auto *f = new FaultDemo;
	f->hostScene = scene;            // Julia calls (the inset field) are made against the OWNING window
	QString error;
	QFile file(QDir(gmtvtkUiDir()).filePath("fault_plane_demo.ui"));
	QUiLoader loader;
	QWidget *ui = file.open(QIODevice::ReadOnly) ? loader.load(&file) : nullptr;
	if (!ui) {
		QMessageBox::warning(parent, "Fault plane demo", "Cannot load " + file.fileName());
		delete f; return nullptr;
	}
	// Every control the demo drives, resolved ONCE and checked here: a widget renamed in the .ui has
	// to come back as an error box, never as a null dereference on the first slider read.
	f->dip     = ui->findChild<QSlider *>("dipSlider");
	f->azimuth = ui->findChild<QSlider *>("azimuthSlider");
	f->rake    = ui->findChild<QSlider *>("rakeSlider");
	f->slip    = ui->findChild<QSlider *>("slipSlider");
	f->play    = ui->findChild<QPushButton *>("playButton");
	auto *resetSlip = ui->findChild<QPushButton *>("resetSlipButton");
	auto *resetView = ui->findChild<QPushButton *>("resetViewButton");
	auto *host      = ui->findChild<QWidget *>("viewHost");
	const char *boxes[] = {"dipValue", "azimuthValue", "rakeValue", "slipValue"};
	QSpinBox *spins[4] = {};
	bool complete = f->dip && f->azimuth && f->rake && f->slip && f->play && resetSlip && resetView && host;
	for (int i = 0; i < 4; ++i) {
		spins[i] = ui->findChild<QSpinBox *>(boxes[i]);
		if (!spins[i]) complete = false;
	}
	if (!complete) {
		QMessageBox::warning(parent, "Fault plane demo",
			"fault_plane_demo.ui does not carry the demo's controls (a renamed or removed widget).");
		delete ui; delete f; return nullptr;
	}
	// NO PARENT. A parented top-level is an OWNED window on Windows and can never go behind its
	// owner, which is what kept the demo covering the main iGMT window whatever the flags said.
	// Unparented, the two windows stack like any two windows. The Scene keeps the pointer (the
	// dialog is no longer findable as a child) and drops it when the dialog dies, and the demo
	// still dies WITH the viewer through the destroyed() connection below — an orphan top-level
	// would otherwise keep the application alive.
	f->dialog = new QDialog(nullptr);
	f->dialog->setObjectName("faultPlaneDemo");
	if (scene) scene->faultDemoDlg = f->dialog;
	QObject::connect(f->dialog, &QObject::destroyed, f->dialog, [scene]() {
		if (scene && sceneAlive(scene)) scene->faultDemoDlg = nullptr;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, f->dialog,
	                             [dlg = f->dialog]() { dlg->deleteLater(); });
	f->dialog->setProperty("faultDemoState", QVariant::fromValue(static_cast<void *>(f)));
	f->dialog->setWindowTitle("Fault plane demo");
	f->dialog->setWindowIcon(appIcon());
	// A QDialog's title bar carries no minimise box by default; this one does, because parking is a
	// normal thing to do with it (the filter turns that click into a park, like the button and the X).
	// A tool WINDOW, not a child dialog pinned on top of the viewer: a QDialog keeps itself above its
	// parent, which is what made the main iGMT window unreachable while the demo was open. Same flags
	// and modality the region picker uses (70_window.cpp): independent window, close + minimise boxes,
	// explicitly non-modal.
	f->dialog->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
	f->dialog->setWindowModality(Qt::NonModal);
	f->dialog->setModal(false);
	// The X DESTROYS this dialog (the destroyed() connections above and below drop the Scene's
	// pointer and tear the demo state down). To keep the model, the sliders and the true-size
	// numbers alive across a dismissal, MINIMISE it instead: that parks it into Scene Objects.
	f->dialog->setAttribute(Qt::WA_DeleteOnClose);
	auto *parkFilter = new FaultDemoParkFilter(f->dialog, scene);
	f->dialog->installEventFilter(parkFilter);
	// Minimise parks, through the SHARED handler (parkOnMinimise, 50_scene.cpp) that the region
	// picker uses — same operation, ONE function, no second copy living in this file.
	parkOnMinimise(f->dialog, [f, scene, parkFilter]() { faultDemoPark(f->dialog, scene, parkFilter); });
	auto *root = new QVBoxLayout(f->dialog);
	root->setContentsMargins(0, 0, 0, 0);
	root->addWidget(ui);
	auto *layout = new QGridLayout(host);
	layout->setContentsMargins(0, 0, 0, 0);
	f->view = new FaultDemoView(host);
	f->view->setObjectName("faultView");
	layout->addWidget(f->view, 0, 0);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	f->view->setRenderWindow(rw);
	f->renderer = vtkSmartPointer<vtkRenderer>::New();
	applyBackgroundPref(f->renderer);   // Preferences "Background color" — the ONE applier (30_app.cpp)
	rw->AddRenderer(f->renderer);
	faultDemoInsetInit(f, rw);       // the top-left demo panel, dark until the "Demo inset" box is ticked
	// THE WINDOW COMES UP FIRST, EMPTY, WITH A NOTICE ON IT. Building the two meshes goes through Julia
	// (faultDemoRebuild -> the fault-plane geometry), and the FIRST call in a session pays for compiling
	// that code: seconds in which nothing at all was on screen, because the old order built the model
	// before there was a dialog to put it in. Now the canvas, the sliders and the true-size boxes are
	// painted and the notice says what the wait is, then the model arrives into a window that is already
	// there. The notice is a plate over the canvas, not a message box: a modal box would have to be
	// dismissed before the build could even start, and would leave the user clicking OK to see nothing.
	f->dialog->resize(1120, 740);
	f->dialog->show(); f->dialog->raise(); f->dialog->activateWindow();
	auto *notice = new QLabel("Building the fault plane demo…\n\nThe first one in a session takes a few "
	                          "seconds:\nthe geometry is compiled on its way through Julia.", host);
	notice->setObjectName("faultDemoNotice");
	notice->setAlignment(Qt::AlignCenter);
	notice->setStyleSheet("#faultDemoNotice { background-color: rgba(255,255,255,225); color: #202020;"
	                      " border: 1px solid #808080; border-radius: 8px; padding: 14px; }");
	layout->addWidget(notice, 0, 0, Qt::AlignCenter);
	qApp->processEvents();           // ...and it is actually PAINTED before the blocking call below
	if (!faultDemoRebuild(f, f->dip->value(), error)) {
		QMessageBox::warning(f->dialog, "Fault plane demo", error);
		f->dialog->deleteLater();    // takes the .ui, the view and (through destroyed()) `f` with it
		return nullptr;
	}
	delete notice;                   // the model is here: the plate has nothing left to say
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	f->view->interactor()->SetInteractorStyle(style);
	const double colours[2][3] = {{0.78, 0.58, 0.34}, {0.35, 0.65, 0.78}};
	for (int b = 0; b < 2; ++b) {
		vtkNew<vtkPolyDataNormals> normals;
		normals->SetInputData(f->meshes[b]);
		normals->SetFeatureAngle(30);
		vtkNew<vtkPolyDataMapper> mapper;
		mapper->SetInputConnection(normals->GetOutputPort());
		mapper->ScalarVisibilityOff();
		f->blocks[b] = vtkSmartPointer<vtkActor>::New();
		f->blocks[b]->SetMapper(mapper);
		f->blocks[b]->GetProperty()->SetColor(colours[b][0], colours[b][1], colours[b][2]);
		f->blocks[b]->GetProperty()->SetAmbient(0.3);
		f->blocks[b]->GetProperty()->SetDiffuse(0.7);
		f->matrices[b] = vtkSmartPointer<vtkMatrix4x4>::New();
		f->blocks[b]->SetUserMatrix(f->matrices[b]);
		f->renderer->AddActor(f->blocks[b]);
	}
	// THE gizmo, from 20_gizmo.cpp — the same scale cone, tilt ring and compass ring the main window
	// gets, owning left-drag (axis-locked rotate / tilt) and the 'x' and view keys through its own
	// observers. It is built against this dialog's own Scene (see FaultDemo::gizScene): the renderer
	// and the widget are all it needs from one, and its vertical-exaggeration handle reaches the
	// blocks through Scene::ve + faultDemoApplyVE.
	f->gizScene = new Scene();
	f->gizScene->ren    = f->renderer;
	f->gizScene->widget = f->view;
	f->gizScene->ve     = 1.0;
	// This scene's content is two blocks, not a base surface: that is what a recentre gesture — the
	// middle-click AND the gizmo's 'c' key, one function — is allowed to land on.
	f->gizScene->pickTargets = {f->blocks[0].Get(), f->blocks[1].Get()};
	f->view->s          = f->gizScene;
	f->gizScene->giz    = enableGizmo(f->gizScene, 0.01);
	// A SMALLER handle than a map window gets: this view opens deliberately zoomed out, and the stock
	// 16% of the viewport height then dwarfs the blocks it is supposed to be a handle for.
	if (f->gizScene->giz) f->gizScene->giz->sizeFrac = 0.10;
	// ...and the handle is pinned to its world anchor before every frame (see faultDemoAnchorCB).
	vtkNew<vtkCallbackCommand> anchorCB;
	anchorCB->SetCallback(faultDemoAnchorCB);
	anchorCB->SetClientData(f);
	f->renderer->AddObserver(vtkCommand::StartEvent, anchorCB, 1.0);   // ABOVE PlaceCB
	f->view->installEventFilter(new FaultDemoKeyProbe(f, f->dialog));   // read-only 'c' trace (stderr)
	// The gizmo writes Scene::ve and calls applyVE, which knows nothing of this dialog's actors, so
	// the demo picks the new factor off the SAME field at the next render — no fork of the handle.
	vtkNew<vtkCallbackCommand> veCB;
	veCB->SetCallback([](vtkObject *, unsigned long, void *cd, void *) {
		auto *fd = static_cast<FaultDemo *>(cd);
		if (fd->gizScene && fd->gizScene->ve != fd->veApplied) faultDemoApplyVE(fd);
		faultDemoPinInsetTriedron(fd);           // the inset panel may not emit its own StartEvent
	});
	veCB->SetClientData(f);
	f->renderer->AddObserver(vtkCommand::StartEvent, veCB);
	vtkNew<vtkArrowSource> arrow;
	arrow->SetTipResolution(24); arrow->SetShaftResolution(16);
	vtkNew<vtkPolyDataMapper> mapper;
	mapper->SetInputConnection(arrow->GetOutputPort());
	f->arrow = vtkSmartPointer<vtkActor>::New();
	f->arrow->SetMapper(mapper);
	f->arrow->GetProperty()->SetColor(0.85, 0.12, 0.10);
	f->renderer->AddActor(f->arrow);
	// A second viewport gives a camera-linked compass without changing the demo camera bounds.
	// TWO of them: one for the 3-D view, one for the deformation inset — same builder, so the
	// inset's triedron is the view's triedron and can never drift into a second look.
	f->compass      = faultDemoTriedron(f->view->interactor(), f->renderer, 0.0, 0.0, 0.22, 0.22);
	// Bottom-LEFT corner of the inset panel, the obvious place for it. The numbers are FRACTIONS
	// OF THE INSET'S OWN rectangle (see faultDemoTriedron) — the inset is 0.40 x 0.425 of the
	// window, so 0.30 of it is a real ~130 px marker, not a window-fraction speck. Flush INTO the
	// corner (0,0), no inset margin — same as the view compass and as VTK's own default. Follows the
	// INSET camera (parallel projection, its own orbit), not the view's.
	f->insetCompass = faultDemoTriedron(f->view->interactor(), f->insetRen, 0.0, 0.0, 0.30, 0.30);
	// Retain both markers until the dialog is destroyed (before deleting their interactor).
	QObject::connect(f->dialog, &QObject::destroyed, [c = f->compass, ic = f->insetCompass] {
		for (auto &w : { c, ic }) if (w) { w->SetEnabled(0); w->SetInteractor(nullptr); }
	});
	auto *inset = new QLabel(host);
	inset->setObjectName("parameterInset");
	QPixmap reference(QDir(gmtvtkDataDir()).filePath("fault_plane_demo/parameters.jpg"));
	inset->setPixmap(reference.scaled(300, 191, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	inset->setFixedSize(312, 203);
	inset->setContentsMargins(6, 6, 6, 6);
	inset->setAttribute(Qt::WA_TransparentForMouseEvents);
	// TOP-RIGHT: the top-LEFT corner of the viewport carries the deformation demo inset, and the right
	// pane's width is the scarce thing in this dialog.
	layout->addWidget(inset, 0, 0, Qt::AlignTop | Qt::AlignRight);
	// Replace the .ui placeholder in its existing layout position with the shared beachball preview.
	// The focal mechanism rides in the VIEW's LOWER-RIGHT corner, not in the controls column: the
	// column's width is what squeezes the 3-D view, and the viewport's top-left now belongs to the
	// deformation demo inset.
	// A light translucent plate keeps the ball and its caption readable over any background colour the
	// Preferences pick.
	auto *beachBox = new QWidget(host);
	beachBox->setObjectName("focalInset");
	beachBox->setStyleSheet("#focalInset { background-color: rgba(255,255,255,170); border-radius: 6px; }"
	                        " QLabel { color: #202020; }");
	auto *beachCol = new QVBoxLayout(beachBox);
	beachCol->setContentsMargins(8, 6, 8, 4);
	beachCol->setSpacing(2);
	f->beach = new BeachballWidget(beachBox);
	f->beach->asCanvas = true;
	f->beach->hostScene = scene;
	f->beach->setCursor(Qt::ArrowCursor);
	f->beach->setToolTip("Focal mechanism for the current strike, dip and signed rake");
	f->beach->setMinimumSize(0, 0);
	f->beach->setFixedSize(66, 66);
	auto *beachCaption = new QLabel("Focal mechanism", beachBox);
	beachCol->addWidget(f->beach, 0, Qt::AlignHCenter);
	beachCol->addWidget(beachCaption, 0, Qt::AlignHCenter);
	layout->addWidget(beachBox, 0, 0, Qt::AlignBottom | Qt::AlignRight);
	QSlider *sliders[] = {f->dip, f->azimuth, f->rake, f->slip};
	for (int i = 0; i < 4; ++i) {
		// Keyboard tracking OFF: with it on, typing "90" into the dip box lands as 9 and then 90, so
		// the demo reconstructs both meshes for a dip the user never asked for. The box now reports
		// on Enter / focus-out / its own arrows, which is one value per intended edit.
		spins[i]->setKeyboardTracking(false);
		QObject::connect(sliders[i], &QSlider::valueChanged, spins[i], &QSpinBox::setValue);
		QObject::connect(spins[i], QOverload<int>::of(&QSpinBox::valueChanged), sliders[i], &QSlider::setValue);
		QObject::connect(sliders[i], &QSlider::valueChanged, f->dialog, [f] { faultDemoUpdate(f); });
	}
	f->timer = new QTimer(f->dialog);
	f->timer->setInterval(30);
	QObject::connect(f->play, &QPushButton::toggled, f->dialog, [f](bool play) {
		f->play->setText(play ? "Pause" : "Play");
		if (play) f->timer->start();
		else f->timer->stop();
	});
	QObject::connect(f->timer, &QTimer::timeout, f->dialog, [f] {
		int v = f->slip->value() + f->playDirection;
		// Bounce at the SLIDER's own limits: the .ui is the single source of the slip range.
		if (v > f->slip->maximum() || v < f->slip->minimum()) {
			f->playDirection = -f->playDirection;
			v += 2*f->playDirection;
		}
		f->slip->setValue(v);
	});
	QObject::connect(resetSlip, &QPushButton::clicked, f->dialog, [f] {
		f->play->setChecked(false); f->slip->setValue(0);
	});
	QObject::connect(resetView, &QPushButton::clicked, f->dialog, [f] { faultDemoCamera(f); });

	// ── True size + Region: the Okada half ───────────────────────────────────────────────────────
	// The 3-D model stays 1:1 whatever these say — that is exactly what "true size" means here: the
	// picture is a teaching shape, these numbers are the real fault. They go to the SAME Okada path
	// the "Vertical elastic deformation" dialog uses (g_juliaElastic, the ';'-separated request
	// documented at the head of src/deform.jl), into the window this demo was opened from. No second
	// deformation implementation, and no second place where a fault's geometry becomes a slip model.
	auto *trueLength   = ui->findChild<QLineEdit *>("trueLength");
	auto *trueWidth    = ui->findChild<QLineEdit *>("trueWidth");
	auto *trueDepthTop = ui->findChild<QLineEdit *>("trueDepthTop");
	auto *trueSlip     = ui->findChild<QLineEdit *>("trueSlip");
	auto *rXmin = ui->findChild<QLineEdit *>("regionXmin"), *rXmax = ui->findChild<QLineEdit *>("regionXmax");
	auto *rYmin = ui->findChild<QLineEdit *>("regionYmin"), *rYmax = ui->findChild<QLineEdit *>("regionYmax");
	auto *rXinc = ui->findChild<QLineEdit *>("regionXinc"), *rYinc = ui->findChild<QLineEdit *>("regionYinc");
	auto *originX = ui->findChild<QLineEdit *>("traceOriginX"), *originY = ui->findChild<QLineEdit *>("traceOriginY");
	auto *computeBtn = ui->findChild<QPushButton *>("computeButton");
	// The two grid-source rows are DECLARED IN THE .ui (refGridEdit + refGridButton, windowGridCombo,
	// in refGridGroup, right above the Compute button). Only their BEHAVIOUR is attached here, through
	// the same wire* functions the code-built rows use (30_app.cpp) — nothing is created, moved,
	// reparented or resized behind the .ui's back. If a row is renamed or dropped in Designer, the
	// findChild returns null and the wire call is a no-op: the .ui decides what exists.
	auto *refEdit  = ui->findChild<QLineEdit *>("refGridEdit");
	auto *refBtn   = ui->findChild<QToolButton *>("refGridButton");
	auto *winCombo = ui->findChild<QComboBox *>("windowGridCombo");
	wireRefGridRow(f->dialog, refEdit, refBtn, rXmin, rXmax, rYmin, rYmax, rXinc, rYinc);
	wireWindowGridRow(f->dialog, winCombo, scene, rXmin, rXmax, rYmin, rYmax, rXinc, rYinc,
	                  [f](int geog) { f->coordGeog = geog; });
	// Belt and braces with the Return swallow above: no button in this dialog is a default button, so
	// even a key path that never reaches the filter cannot press one.
	for (QPushButton *b : ui->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
	if (computeBtn && trueLength && trueWidth && trueDepthTop && trueSlip && rXmin && rXmax && rYmin && rYmax) {
		QObject::connect(computeBtn, &QPushButton::clicked, f->dialog,
			[f, scene, trueLength, trueWidth, trueDepthTop, trueSlip, rXmin, rXmax, rYmin, rYmax, rXinc, rYinc,
			 originX, originY]() {
			auto num = [](QLineEdit *e, bool &ok) { return e->text().trimmed().toDouble(&ok); };
			bool ok = true, all = true;
			const double L  = num(trueLength, ok);   all &= ok;
			const double W  = num(trueWidth, ok);    all &= ok;
			const double DT = num(trueDepthTop, ok); all &= ok;
			const double S  = num(trueSlip, ok);     all &= ok;
			const double w  = num(rXmin, ok); all &= ok;
			const double e_ = num(rXmax, ok); all &= ok;
			const double s_ = num(rYmin, ok); all &= ok;
			const double n  = num(rYmax, ok); all &= ok;
			if (!all || L <= 0 || W <= 0 || e_ <= w || n <= s_) {
				QMessageBox::warning(f->dialog, "Fault plane demo",
					"Length, Width, Depth to top, Slip and a W < E / S < N region are all needed.");
				return;
			}
			if (!g_juliaElastic) {
				QMessageBox::warning(f->dialog, "Fault plane demo", "The deformation tool is not wired.");
				return;
			}
			const QString R = QString("%1/%2/%3/%4").arg(w, 0, 'f', 8).arg(e_, 0, 'f', 8)
			                                        .arg(s_, 0, 'f', 8).arg(n, 0, 'f', 8);
			QString I;
			if (rXinc && !rXinc->text().trimmed().isEmpty()) {
				I = rXinc->text().trimmed();
				if (rYinc && !rYinc->text().trimmed().isEmpty()) I += "/" + rYinc->text().trimmed();
			}
			// The 20 fields of deform.jl's contract, in its order. Depth (7) is the top depth: the
			// compute path uses field 8 and only checks that 7 is a number. xStart/yStart (18/19) come
			// from the Fault trace origin boxes when the user filled them in; left EMPTY (the default)
			// the fault is centred in the region instead, and walking half its length back along strike
			// is a geodesic, which belongs on the Julia side with every other one.
			const QString xStart = originX ? originX->text().trimmed() : QString();
			const QString yStart = originY ? originY->text().trimmed() : QString();
			QStringList p;
			p << "compute" << (f->coordGeog ? "geog" : "cart")
			  << QString::number(L, 'f', 6) << QString::number(W, 'f', 6)
			  << QString::number(f->azimuth->value()) << QString::number(f->dip->value())
			  << QString::number(DT, 'f', 6) << QString::number(DT, 'f', 6)
			  << QString::number(faultDemoRake(f)) << QString::number(S, 'f', 6)
			  << "0" << "0" << "" << "" << "" << R << I << xStart << yStart << "";
			f->dialog->setCursor(Qt::WaitCursor);
			g_juliaElastic(scene, p.join(';').toUtf8().constData());
			f->dialog->unsetCursor();
		});
	}
	// ── The demo inset: its own checkbox and its own button (both declared in the .ui) ───────────
	// The checkbox turns the top-left panel on; its button computes ONLY that panel. Two separate
	// buttons because the two results are two different things and must never be confused: "Compute
	// deformation" is the REAL fault, on the real region, into the real window; "Compute inset" is a
	// teaching picture over a zone three times the fault, which stays inside this dialog.
	auto *insetCheck = ui->findChild<QCheckBox *>("insetDemoCheck");
	auto *insetBtn   = ui->findChild<QPushButton *>("insetComputeButton");
	if (insetCheck && insetBtn) {
		insetBtn->setEnabled(insetCheck->isChecked());
		// THE .ui DECIDES whether the panel starts on — it does — so the renderer is put in that state here
		// instead of being assumed off, and it is given CONTENT: an empty white box in the corner is not a
		// demo. Deferred by one event loop turn so the dialog paints and answers the mouse first, because
		// the field is a Julia call (GMT.okada) and the first one in a session compiles as it goes.
		faultDemoTriedronShow(f->insetCompass, f->insetRen, insetCheck->isChecked());
		if (insetCheck->isChecked() && f->insetRen) {
			f->insetRen->DrawOn(); f->insetRen->InteractiveOn();
			QTimer::singleShot(0, f->dialog, [insetBtn]() { insetBtn->click(); });
		}
		QObject::connect(insetCheck, &QCheckBox::toggled, f->dialog, [f, insetBtn](bool on) {
			insetBtn->setEnabled(on);
			if (f->insetRen) {
				on ? f->insetRen->DrawOn() : f->insetRen->DrawOff();
				on ? f->insetRen->InteractiveOn() : f->insetRen->InteractiveOff();   // shown = it owns its corner
			}
			// The panel's triedron is part of the panel: it goes with it, never left floating over an
			// empty corner — and it comes back INTO the panel, not into the view (faultDemoTriedronShow).
			faultDemoTriedronShow(f->insetCompass, f->insetRen, on);
			f->view->renderWindow()->Render();
		});
	}
	if (insetBtn && trueLength && trueWidth && trueDepthTop && trueSlip) {
		QObject::connect(insetBtn, &QPushButton::clicked, f->dialog,
			[f, insetCheck, trueLength, trueWidth, trueDepthTop, trueSlip]() {
			auto num = [](QLineEdit *e, bool &ok) { return e->text().trimmed().toDouble(&ok); };
			bool ok = true, all = true;
			const double L  = num(trueLength, ok);   all &= ok;
			const double W  = num(trueWidth, ok);    all &= ok;
			const double DT = num(trueDepthTop, ok); all &= ok;
			const double S  = num(trueSlip, ok);     all &= ok;
			if (!all || L <= 0 || W <= 0) {
				QMessageBox::warning(f->dialog, "Fault plane demo",
					"The inset needs Length, Width, Depth to top and Slip — its own region is derived "
					"from the fault (three times its size), so the Region block is not used.");
				return;
			}
			QString err;
			f->dialog->setCursor(Qt::WaitCursor);
			const bool done = faultDemoInsetCompute(f, L, W, DT, S, err);
			f->dialog->unsetCursor();
			if (!done) { QMessageBox::warning(f->dialog, "Fault plane demo", err); return; }
			// Computing it shows it: the checkbox is the panel's switch, so a compute ticks it rather
			// than leaving the result invisible behind an unticked box.
			if (insetCheck && !insetCheck->isChecked()) insetCheck->setChecked(true);
			else if (f->insetRen) { f->insetRen->DrawOn(); f->insetRen->InteractiveOn(); }
			f->view->renderWindow()->Render();
		});
	}
	// DIP / AZIMUTH / RAKE change the deformation field, so the inset follows them by itself. It
	// recomputes through the SAME button the user would press — one compute path, not a second copy of
	// the request — and only while the panel is on.
	//
	// DEBOUNCED: a slider emits on every pixel of a drag and every recompute is a Julia GMT.okada call,
	// so a drag would queue dozens of them. The timer restarts on each change and fires once the user
	// stops, which is the same "last value wins" the spin boxes already use (keyboardTracking off).
	if (insetCheck && insetBtn) {
		auto *insetDebounce = new QTimer(f->dialog);
		insetDebounce->setSingleShot(true);
		insetDebounce->setInterval(150);
		QObject::connect(insetDebounce, &QTimer::timeout, f->dialog, [insetCheck, insetBtn]() {
			if (insetCheck->isChecked() && insetBtn->isEnabled()) insetBtn->click();
		});
		for (QSlider *sl : { f->dip, f->azimuth, f->rake })
			QObject::connect(sl, &QSlider::valueChanged, f->dialog, [insetCheck, insetDebounce](int) {
				if (insetCheck->isChecked()) insetDebounce->start();
			});
	}
	QObject::connect(f->dialog, &QObject::destroyed, [f] {
		// Same teardown order as a window's: the gizmo's props/light/observers come off the renderer
		// and the interactor FIRST (disableGizmo), then its Scene goes.
		if (f->gizScene) { disableGizmo(f->gizScene); delete f->gizScene; f->gizScene = nullptr; }
		delete f;
	});
	// Already SHOWN (above, before the mesh build) — raised and activated once more now that it is fully
	// furnished. An unparented window is no longer OWNED by the viewer, which is what stopped it covering
	// the main iGMT window, and that also stopped it being placed in front on its own: independent
	// stacking has to be ASKED for, in both directions.
	f->dialog->raise(); f->dialog->activateWindow();
	f->view->setFocus(Qt::OtherFocusReason);     // the keys belong to the 3-D view from the first frame
	if (!faultDemoUpdate(f)) { f->dialog->close(); return nullptr; }
	faultDemoCamera(f);
	return f->dialog;
}
