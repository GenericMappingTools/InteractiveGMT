
// ============================================================================
//  Gizmo — Fledermaus-style scale/tilt/azimuth handle. We own the renderer + the
//  surface actor directly, so vertical scale just drives Scene::ve via applyVE().
//
//  Floating widget pinned to the camera focal point. Handles:
//   - vertical amber cone (on a vertical axis)  -> VERTICAL EXAGGERATION (cone
//     stretches with the factor). Also driven by the legacy Ctrl+left-drag.
//   - ring at the horizontal axis tip           -> TILT (camera elevation).
//   - compass ring at the body top              -> AZIMUTH (heading about world +Z).
//  Billboard labels read out z×, heading°, inclination°.
// ============================================================================
// ============================================================================
enum class Grab { None, VScale, Tilt, Azimuth, Free, Profile };

struct Gizmo {
	Scene* s = nullptr;

	// drawing
	vtkSmartPointer<vtkActor> shaft, vcone, shaftH, harrow, ring;
	vtkSmartPointer<vtkConeSource> vconeSrc;
	vtkSmartPointer<vtkLineSource>  shaftSrc, shaftHSrc;
	vtkSmartPointer<vtkBillboardTextActor3D> label, azLabel, inclLabel;
	vtkSmartPointer<vtkLight> light;
	vtkSmartPointer<vtkCallbackCommand> placeCmd, dragCmd, keyCmd;
	unsigned long placeTag = 0, dragTags[3] = {0,0,0}, keyTag = 0;
	bool visible = true;

	// dragging
	double sensitivity = 0.01;  // vertical-scale exp factor per pixel
	double rotSpeed    = 0.5;   // degrees per pixel for tilt/azimuth
	Grab   grab = Grab::None;
	int    lastX = 0, lastY = 0, startX = 0, startY = 0;
	int    freeAxis = 0;        // 0 = undecided, 1 = horizontal (rotate), 2 = vertical (tilt)
	double startSz = 1.0, curSz = 1.0;
	double veBase  = 1.0;   // VE at gizmo enable; cone stretch is curSz/veBase so it ALWAYS starts at default size regardless of auto-VE

	// current placement (world)
	double centre[3] = {0,0,0};
	double scale = 1.0;
	double right[3] = {1,0,0};  // camera screen-right (horizontal-axis direction)
	double haxisLen = 0.0;      // world half-extent of data; 0 = unknown
	double haxisRatio = 0.0;    // tilt-ring offset in units of `scale` (camera dist); 0 = not yet calibrated
	                            // calibrated once at startup from haxisLen so the ring keeps a
	                            // CONSTANT screen distance (always slightly outside cube, always visible)
	double azimuth = 0.0, incl = 0.0;

	// Active view-snap (number-key views): 0 = free/none, 1 = front, 2 = top, 3 = right.
	// Pressing the same view key again is a no-op while this matches (prevents the snap from
	// re-running and re-zooming cumulatively). Reset to 0 on any manual camera move.
	int curView = 0;
	vtkSmartPointer<vtkCallbackCommand> interactCmd;
	unsigned long interactTag = 0;
};

namespace {

// Local-space layout (gizmo-relative units; z=0 at the focal point / rotation centre).
constexpr double kBodyZ  = 1.4;     // floating control-body height above focal pt
constexpr double kConeR  = 0.047;   // vertical arrowhead base radius
constexpr double kConeH0 = 0.11;    // vertical arrowhead height at scale 1
constexpr double kRingR  = 0.328;   // ring radius (shared by both rings) — 20% smaller
constexpr double kRingH  = 0.064;   // ring band width (shared) — 20% smaller
constexpr double kHaxisFallback = 3.10; // horizontal-axis length when bbox unknown

void renderWin(Gizmo* g) { g->s->widget->renderWindow()->Render(); }

// Zoom a freshly-positioned snap view (perspective) so the data fills the available viewport.
//   topMode=false -> fill WIDTH               (front/back/left/right side views)
//   topMode=true  -> fill the LIMITING dim    (top view: width OR height by data aspect)
// A margin is kept so the overlay axis annotations (tick numbers / titles, which live in the
// overlay renderer and are NOT counted by ResetCamera) are not clipped. The projected size is
// measured from the camera's composite transform (immediate, no Render needed); Zoom changes
// the perspective view angle so a second pass corrects the slight non-linearity.
void fitSnapView(Scene* s, bool topMode, double fill = -1.0) {
	if (!s || !s->ren || !s->surf || !s->widget) return;
	vtkRenderer* ren = s->ren;
	vtkCamera*   cam = ren->GetActiveCamera();
	ren->ResetCamera();                                   // baseline fit + distance
	double b[6]; surfGetBounds(s, b);
	// ResetCamera centres on ALL visible props (gizmo floats above the surface, cube axes
	// overhang), which pushes the surface low in the viewport. Pan so the SURFACE bbox centre
	// is the focal point -> the relief is centred at mid-height, not sitting at the bottom.
	{
		const double sc[3] = { 0.5*(b[0]+b[1]), 0.5*(b[2]+b[3]), 0.5*(b[4]+b[5]) };
		double pos[3], foc[3]; cam->GetPosition(pos); cam->GetFocalPoint(foc);
		const double d[3] = { pos[0]-foc[0], pos[1]-foc[1], pos[2]-foc[2] };
		cam->SetFocalPoint(sc);
		cam->SetPosition(sc[0]+d[0], sc[1]+d[1], sc[2]+d[2]);
	}
	const int* sz = s->widget->renderWindow()->GetSize();
	const double aspect = (sz && sz[1] > 0) ? double(sz[0]) / double(sz[1]) : 1.0;
	// Top view fills the viewport edge-to-edge (data bbox spans the limiting dim); the lon/lat
	// tick numbers + titles sit just outside the data and are intentionally pushed off-screen.
	// Side views keep a margin so their annotation text stays visible. Caller may override the
	// fill fraction (e.g. a referenced image wants a margin so its lon/lat axes stay visible).
	const double targetFill = (fill > 0.0) ? fill : (topMode ? 1.0 : 0.88);
	for (int pass = 0; pass < 2; ++pass) {
		vtkMatrix4x4* M = cam->GetCompositeProjectionTransformMatrix(aspect, -1.0, 1.0);
		double nx0=1e300, nx1=-1e300, ny0=1e300, ny1=-1e300;
		for (double cx : { b[0], b[1] })
			for (double cy : { b[2], b[3] })
				for (double cz : { b[4], b[5] }) {
					double p[4] = { cx, cy, cz, 1.0 }, o[4];
					M->MultiplyPoint(p, o);
					if (o[3] == 0.0) continue;
					const double ndcx = o[0]/o[3], ndcy = o[1]/o[3];   // clip -> NDC, viewport = [-1,1]
					nx0 = std::min(nx0, ndcx); nx1 = std::max(nx1, ndcx);
					ny0 = std::min(ny0, ndcy); ny1 = std::max(ny1, ndcy);
				}
		const double wfrac = (nx1 - nx0) / 2.0, hfrac = (ny1 - ny0) / 2.0;   // NDC span 2 = full
		const double frac  = topMode ? std::max(wfrac, hfrac) : wfrac;
		if (frac <= 1e-6) break;
		cam->Zoom(targetFill / frac);
	}
	ren->ResetCameraClippingRange();
}

vtkSmartPointer<vtkActor> makeActor(vtkPolyDataAlgorithm* src, double r, double gc, double b) {
	vtkNew<vtkPolyDataMapper> m;
	m->SetInputConnection(src->GetOutputPort());
	// Draw the gizmo ON TOP of the surface via a large negative depth offset.
	vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
	m->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -66000.0);
	m->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -66000.0);
	vtkSmartPointer<vtkActor> a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(m);
	a->GetProperty()->SetColor(r, gc, b);
	a->GetProperty()->LightingOff();
	return a;
}
void litLook(vtkActor* a) {
	a->GetProperty()->LightingOn();
	a->GetProperty()->SetAmbient(0.6);
	a->GetProperty()->SetDiffuse(0.5);
	a->GetProperty()->SetSpecular(0.12);
	a->GetProperty()->SetSpecularPower(15.0);
}
void ringLook(vtkActor* a) { a->GetProperty()->SetColor(0.50,0.50,0.50); litLook(a); }

void updateVCone(Gizmo& c) {
	if (!c.vconeSrc) return;
	double h = kConeH0 * std::clamp(c.curSz / c.veBase, 0.15, 8.0);  // relative to enable VE -> default size at startup
	c.vconeSrc->SetHeight(h);
	c.vconeSrc->SetRadius(kConeR);
	c.vconeSrc->SetDirection(0.0, 0.0, 1.0);
	c.vconeSrc->SetCenter(0.0, 0.0, kBodyZ + 0.5 * h); // base fixed at shaft top
}
void updateLabel(Gizmo& c) {
	if (!c.label) return;
	char buf[64]; std::snprintf(buf, sizeof(buf), "z x %.2f", c.curSz);
	c.label->SetInput(buf);
}

void cross3(const double a[3], const double b[3], double o[3]) {
	o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
bool normalize3(double v[3]) {
	const double n = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
	if (n < 1e-9) return false;
	v[0]/=n; v[1]/=n; v[2]/=n; return true;
}

// Place the gizmo. Vertical parts are world-aligned (+Z up); the horizontal axis is
// oriented along the camera screen-right vector so it stays left-right in the window.
void placeAll(Gizmo& c) {
	const double* p = c.centre;
	const double s = c.scale;
	for (vtkActor* a : { c.shaft.Get(), c.vcone.Get(), c.ring.Get() }) {
		if (a) { a->SetUserMatrix(nullptr); a->SetScale(s); a->SetPosition(p[0],p[1],p[2]); }
	}
	double X[3] = { c.right[0], c.right[1], c.right[2] };
	double up[3] = { 0.0, 0.0, 1.0 }, Y[3], Z[3];
	cross3(up, X, Y);
	if (!normalize3(Y)) { double alt[3]={0,1,0}; cross3(alt,X,Y); normalize3(Y); }
	cross3(X, Y, Z); normalize3(Z);

	const double L = s * ((c.haxisRatio > 0.0) ? c.haxisRatio : kHaxisFallback);
	double tip[3] = { p[0]+L*X[0], p[1]+L*X[1], p[2]+L*X[2] };

	if (c.shaftHSrc) {
		c.shaftHSrc->SetPoint1(p[0],p[1],p[2]);
		c.shaftHSrc->SetPoint2(tip[0],tip[1],tip[2]); c.shaftHSrc->Modified();
	}
	if (c.shaftH) { c.shaftH->SetUserMatrix(nullptr); c.shaftH->SetScale(1.0); c.shaftH->SetPosition(0,0,0); }
	if (c.harrow) {
		vtkNew<vtkMatrix4x4> M; M->Identity();
		for (int i=0;i<3;++i) {
			M->SetElement(i,0,s*X[i]); M->SetElement(i,1,s*Y[i]);
			M->SetElement(i,2,s*Z[i]); M->SetElement(i,3,tip[i]);
		}
		c.harrow->SetUserMatrix(M);
	}

	const double aboveRing = kBodyZ + 0.28;
	const double zSide = kRingR * 0.8;
	const double ringSide = kRingR + 0.096;   // gap scaled with the 20%-smaller ring
	const double tipTop = kRingR + 0.112;     // gap scaled with the 20%-smaller ring
	if (c.label)
		c.label->SetPosition(p[0]+s*zSide*c.right[0], p[1]+s*zSide*c.right[1], p[2]+s*aboveRing);
	if (c.azLabel) {
		c.azLabel->SetPosition(p[0]-s*ringSide*c.right[0], p[1]-s*ringSide*c.right[1], p[2]+s*kBodyZ);
		char buf[32]; std::snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", c.azimuth);
		c.azLabel->SetInput(buf);
	}
	if (c.inclLabel) {
		c.inclLabel->SetPosition(tip[0], tip[1], tip[2]+s*tipTop);
		char buf[32]; std::snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", c.incl);
		c.inclLabel->SetInput(buf);
	}
}

// StartEvent on the renderer: follow the focal point, size from camera distance.
void PlaceCB(vtkObject* caller, unsigned long, void* clientData, void*) {
	Gizmo* c = static_cast<Gizmo*>(clientData);
	vtkRenderer* ren = vtkRenderer::SafeDownCast(caller);
	if (!c || !ren || !ren->GetActiveCamera()) return;
	vtkCamera* cam = ren->GetActiveCamera();
	cam->GetFocalPoint(c->centre);
	double d = cam->GetDistance();
	if (cam->GetParallelProjection()) d = cam->GetParallelScale() * 2.0;
	c->scale = std::max(1e-6, 0.085 * d);

	// Calibrate the tilt-ring offset ONCE (first frame): lock the world data-extent position
	// (slightly outside the cube) to a ratio of `scale`. Thereafter L = scale*ratio, so the ring
	// keeps a constant SCREEN distance regardless of zoom or focal-point change -> always visible.
	if (c->haxisRatio <= 0.0)
		c->haxisRatio = (c->haxisLen > 1e-9) ? (c->haxisLen / c->scale) : kHaxisFallback;

	double pos[3], foc[3];
	cam->GetPosition(pos); cam->GetFocalPoint(foc);
	double up[3] = {0,0,1};
	double vpn[3] = { pos[0]-foc[0], pos[1]-foc[1], pos[2]-foc[2] };
	double r[3]; cross3(up, vpn, r);
	if (normalize3(r)) { c->right[0]=r[0]; c->right[1]=r[1]; c->right[2]=r[2]; }

	const double horiz = std::sqrt(vpn[0]*vpn[0]+vpn[1]*vpn[1]);
	c->incl = std::atan2(std::abs(vpn[2]), horiz) * 180.0 / vtkMath::Pi();
	// Displayed camera azimuth = compass bearing of the view direction (focal - position),
	// projected onto the world XY plane. Convention: 0deg = +Y (north), 90deg = +X (east),
	// clockwise, range 0..360. Same as GMT azimuth (deg CW from north). NB: this is the
	// camera HEADING readout; the azimuth *gesture* rotates about world +Z (see RotateZ below).
	double az = std::atan2(foc[0]-pos[0], foc[1]-pos[1]) * 180.0 / vtkMath::Pi();
	if (az < 0.0) az += 360.0;
	c->azimuth = az;
	placeAll(*c);
}

void worldToDisplay(vtkRenderer* ren, double wx, double wy, double wz, double out[2]) {
	ren->SetWorldPoint(wx, wy, wz, 1.0);
	ren->WorldToDisplay();
	double d[3]; ren->GetDisplayPoint(d); out[0]=d[0]; out[1]=d[1];
}
double dist2(const double a[2], double bx, double by) {
	const double dx=a[0]-bx, dy=a[1]-by; return dx*dx+dy*dy;
}
// Distance from point (px,py) to segment [a,b], all in display coords.
double distToSeg(double px, double py, const double a[2], const double b[2]) {
	const double vx=b[0]-a[0], vy=b[1]-a[1];
	const double L2=vx*vx+vy*vy;
	double t = (L2>1e-12) ? ((px-a[0])*vx + (py-a[1])*vy)/L2 : 0.0;
	t = std::clamp(t, 0.0, 1.0);
	const double cx=a[0]+t*vx, cy=a[1]+t*vy;
	const double dx=px-cx, dy=py-cy;
	return std::sqrt(dx*dx+dy*dy);
}

// Screen-space hit-test of the click against the handles.
Grab hitTest(Gizmo& c, vtkRenderer* ren, int x, int y) {
	if (!ren) return Grab::None;
	const double* p = c.centre;
	const double s = c.scale;
	const double coneH = kConeH0 * std::clamp(c.curSz / c.veBase, 0.15, 8.0);

	// Cone grab = the base->apex SEGMENT with the (thin) base radius. Measuring a
	// radius from the cone CENTRE would balloon with a tall cone (big VE) and eat
	// the surrounding compass ring; the segment test keeps the grab cone-shaped.
	double baseC[2], apexC[2], baseE[2];
	worldToDisplay(ren, p[0], p[1], p[2]+s*kBodyZ, baseC);
	worldToDisplay(ren, p[0], p[1], p[2]+s*(kBodyZ+coneH), apexC);
	worldToDisplay(ren, p[0]+s*kConeR, p[1], p[2]+s*kBodyZ, baseE);
	double rBase = std::sqrt(dist2(baseC, baseE[0], baseE[1]));
	double rGrab = std::max(rBase*1.6, 12.0);
	if (distToSeg(x, y, baseC, apexC) <= rGrab) return Grab::VScale;

	{
		const double L = s * ((c.haxisRatio > 0.0) ? c.haxisRatio : kHaxisFallback);
		double tx=p[0]+L*c.right[0], ty=p[1]+L*c.right[1], tz=p[2]+L*c.right[2];
		double dA[2], dAedge[2];
		worldToDisplay(ren, tx, ty, tz, dA);
		worldToDisplay(ren, tx, ty, tz+s*kRingR, dAedge);
		double rA = std::sqrt(dist2(dA, dAedge[0], dAedge[1]));
		rA = std::max(rA*2.5, 16.0);
		if (dist2(dA, x, y) <= rA*rA) return Grab::Tilt;
	}

	double dCtr[2], dRim[2];
	worldToDisplay(ren, p[0], p[1], p[2]+s*kBodyZ, dCtr);
	worldToDisplay(ren, p[0]+s*kRingR, p[1], p[2]+s*kBodyZ, dRim);
	const double r = std::sqrt(dist2(dCtr, x, y));
	const double rRim = std::sqrt(dist2(dCtr, dRim[0], dRim[1]));
	if (r >= 0.45*rRim && r <= 1.25*rRim) return Grab::Azimuth;
	return Grab::None;
}

void DragCB(vtkObject* caller, unsigned long eid, void* clientData, void*) {
	Gizmo* c = static_cast<Gizmo*>(clientData);
	vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!c || !rwi) return;
	vtkRenderer* ren = c->s->ren;
	vtkCamera* cam = (ren && ren->GetActiveCamera()) ? ren->GetActiveCamera() : nullptr;
	bool handled = false;

	// The polygon tool owns the left button while a vertex is being dragged or a polygon is being
	// drawn — the gizmo must NEVER rotate/tilt then (a vertex edit must not also move the camera).
	// Abort so neither the gizmo nor the trackball acts on this event.
	if (c->s->polyDragVert >= 0 || (c->s->polyMode && c->s->polyDrawing)) {
		c->grab = Grab::None;
		if (c->dragCmd) c->dragCmd->SetAbortFlagOnExecute(1);
		return;
	}

	if (eid == vtkCommand::LeftButtonPressEvent) {
		const int x = rwi->GetEventPosition()[0], y = rwi->GetEventPosition()[1];
		c->lastX = x; c->lastY = y;
		// Tell the render window we're interacting -> vtkLODActor draws its decimated subset
		// while dragging (the gizmo renders directly, bypassing VTK's normal Start/EndInteraction).
		c->s->widget->renderWindow()->SetDesiredUpdateRate(15.0);
		if (rwi->GetControlKey()) {            // Ctrl+left-drag: draw a surface-elevation profile track
			profilerBegin(c->s, x, y);
			c->grab = Grab::Profile; handled = true;
		}
		else {
			c->grab = c->s->flat2d ? Grab::None : hitTest(*c, ren, x, y);   // 2D map: gizmo handles off
			if (c->grab == Grab::VScale) { c->startY = y; c->startSz = c->curSz; }
			else if (c->grab == Grab::None) {     // gizmo miss: try an overlay, else rotate/tilt
				int ovMode = 1;
				vtkActor* ov = pickOverlayAt(c->s, x, y, ovMode);
				if (ov) {                         // left-click ON a line/point overlay -> its menu
					// Pop its context menu DEFERRED: a modal QMenu must not run inside this VTK
					// press callback (reentrant event loop). singleShot(0) fires it next loop turn,
					// after press/release. VTK display (bottom-up device px) -> Qt global (top-down).
					Scene* sc = c->s;
					const double dpr = sc->widget->devicePixelRatioF();
					const int    Hpx = sc->widget->renderWindow()->GetSize()[1];
					QPoint gp = sc->widget->mapToGlobal(QPoint(int(x / dpr), int((Hpx - y) / dpr)));
					QTimer::singleShot(0, sc->widget, [sc, ov, ovMode, gp]() {
						popupOverlayMenu(sc, ov, ovMode, gp);
					});
					c->grab = Grab::None;         // selected; do NOT rotate
				}
				else {                            // empty space -> axis-locked rotate OR tilt
					if (!c->s->flat2d)            // 2D map: rotation + tilt locked
						c->grab = Grab::Free;
					c->startX = x; c->startY = y; c->freeAxis = 0;
				}
			}
			handled = true;                    // we own left-drag (no trackball roll)
		}
	}
	else if (eid == vtkCommand::MouseMoveEvent) {
		const int x = rwi->GetEventPosition()[0], y = rwi->GetEventPosition()[1];
		if (c->grab == Grab::VScale) {
			const double dy = static_cast<double>(y - c->startY);
			double sz = std::max(1e-6, c->startSz * std::exp(c->sensitivity * dy));
			sz = std::clamp(sz, 0.01, 1.0e4);   // geographic grids start at large auto-VE
			c->curSz = sz; updateVCone(*c); updateLabel(*c);
			c->s->ve = sz; applyVE(c->s);   // drives Scene::ve + cube-axes sync + render
			handled = true;
		}
		else if (c->grab == Grab::Profile) {
			profilerDrag(c->s, x, y);              // re-sample the surface along press->cursor, refresh 2D panel
			handled = true;
		}
		else if (c->grab == Grab::Tilt && cam) {
			const double dy = static_cast<double>(y - c->lastY);
			cam->Elevation(-dy * c->rotSpeed);
			cam->OrthogonalizeViewUp();
			ren->ResetCameraClippingRange();
			renderWin(c); handled = true;
		}
		else if (c->grab == Grab::Azimuth && cam) {
			// Pure heading rotation about world +Z through the focal point.
			const double dx = static_cast<double>(x - c->lastX);
			const double ang = -dx * c->rotSpeed;
			double fp[3], pos[3], vu[3];
			cam->GetFocalPoint(fp); cam->GetPosition(pos); cam->GetViewUp(vu);
			vtkNew<vtkTransform> tp;
			tp->Translate(fp[0],fp[1],fp[2]); tp->RotateZ(ang); tp->Translate(-fp[0],-fp[1],-fp[2]);
			double npos[3]; tp->TransformPoint(pos, npos);
			vtkNew<vtkTransform> tv; tv->RotateZ(ang);
			double nvu[3]; tv->TransformVector(vu, nvu);
			cam->SetPosition(npos); cam->SetViewUp(nvu);
			ren->ResetCameraClippingRange();
			renderWin(c); handled = true;
		}
		else if (c->grab == Grab::Free && cam) {
			// Empty left-drag is AXIS-LOCKED: the first dominant direction wins for the
			// whole gesture. Horizontal gesture ONLY rotates (azimuth); vertical gesture
			// ONLY tilts. Never both in one drag.
			if (c->freeAxis == 0) {
				const int tdx = std::abs(x - c->startX), tdy = std::abs(y - c->startY);
				if (tdx >= 3 || tdy >= 3) c->freeAxis = (tdx >= tdy) ? 1 : 2;  // lock past deadzone
			}
			if (c->freeAxis == 1) {                // HORIZONTAL: azimuth about world +Z only
				const double ang = -static_cast<double>(x - c->lastX) * c->rotSpeed;
				double fp[3], pos[3], vu[3];
				cam->GetFocalPoint(fp); cam->GetPosition(pos); cam->GetViewUp(vu);
				vtkNew<vtkTransform> tp;
				tp->Translate(fp[0],fp[1],fp[2]); tp->RotateZ(ang); tp->Translate(-fp[0],-fp[1],-fp[2]);
				double npos[3]; tp->TransformPoint(pos, npos);
				vtkNew<vtkTransform> tv; tv->RotateZ(ang);
				double nvu[3]; tv->TransformVector(vu, nvu);
				cam->SetPosition(npos); cam->SetViewUp(nvu);
				ren->ResetCameraClippingRange(); renderWin(c);
			}
			else if (c->freeAxis == 2) {           // VERTICAL: tilt (elevation) only
				cam->Elevation(-static_cast<double>(y - c->lastY) * c->rotSpeed);
				cam->OrthogonalizeViewUp();
				ren->ResetCameraClippingRange(); renderWin(c);
			}
			handled = true;
		}
		// Any manual camera move invalidates the active view-snap so a view key re-snaps.
		if (c->grab == Grab::Tilt || c->grab == Grab::Azimuth || c->grab == Grab::Free)
			c->curView = 0;
		c->lastX = x; c->lastY = y;
	}
	else if (eid == vtkCommand::LeftButtonReleaseEvent) {
		if (c->grab == Grab::Profile) profilerEnd(c->s);
		if (c->grab != Grab::None) { c->grab = Grab::None; handled = true; }
		// Drag ended -> back to still update rate and one full-resolution render.
		c->s->widget->renderWindow()->SetDesiredUpdateRate(0.0001);
		renderWin(c);
	}

	if (c->dragCmd) c->dragCmd->SetAbortFlagOnExecute(handled ? 1 : 0);
}

void setGizmoVisible(Gizmo& c, bool on) {
	for (vtkProp* a : { (vtkProp*)c.shaft.Get(), (vtkProp*)c.shaftH.Get(),
						(vtkProp*)c.vcone.Get(), (vtkProp*)c.harrow.Get(), (vtkProp*)c.ring.Get() })
		if (a) a->SetVisibility(on ? 1 : 0);
	for (vtkBillboardTextActor3D* a : { c.label.Get(), c.azLabel.Get(), c.inclLabel.Get() })
		if (a) a->SetVisibility(on ? 1 : 0);
	c.visible = on;
}

// 'x' toggles the gizmo visibility (observe low priority, never abort).
void KeyCB(vtkObject* caller, unsigned long, void* clientData, void*) {
	Gizmo* c = static_cast<Gizmo*>(clientData);
	vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!c || !rwi) return;
	const char* key = rwi->GetKeySym();
	if (key && (key[0]=='x' || key[0]=='X') && key[1]=='\0') {
		setGizmoVisible(*c, !c->visible); renderWin(c);
	}
	// 'e' toggles the surface mesh (wire edges) on the base + drape actors.
	if (key && (key[0]=='e' || key[0]=='E') && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->surf) {
			int on = s->surf->GetProperty()->GetEdgeVisibility() ? 0 : 1;
			for (vtkActor* a : surfActors(s)) {       // toggle wire on every tile (or the surface)
				a->GetProperty()->SetEdgeVisibility(on);
				a->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12);
				a->GetProperty()->SetLineWidth(1.0);
			}
			if (s->drape) {
				s->drape->GetProperty()->SetEdgeVisibility(on);
				s->drape->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12);
				s->drape->GetProperty()->SetLineWidth(1.0);
			}
			renderWin(c);
		}
	}
	// 'c' recenters the view on the surface point under the mouse: reset the camera focal
	// point to that point and translate the camera by the same delta (pure pan) so the
	// picked point lands at the centre of the viewport. Same path as the middle-click recenter.
	if (key && (key[0]=='c' || key[0]=='C') && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->ren && s->surf && s->widget) {
			vtkCamera* cam = s->ren->GetActiveCamera();
			// Qt logical (top-down) cursor -> VTK display (bottom-up device) pixels.
			const QPoint lp = s->widget->mapFromGlobal(QCursor::pos());
			const double r = s->widget->devicePixelRatioF();
			const int H = s->widget->renderWindow()->GetSize()[1];
			const double dx = lp.x() * r;
			const double dy = H - lp.y() * r;
			vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
			pk->PickFromListOn(); pk->AddPickList(surfProp(s));
			if (cam && pk->Pick(dx, dy, 0.0, s->ren)) {
				double pick[3]; pk->GetPickPosition(pick);
				double pos[3], fp[3]; cam->GetPosition(pos); cam->GetFocalPoint(fp);
				const double d[3] = { pos[0]-fp[0], pos[1]-fp[1], pos[2]-fp[2] };
				cam->SetFocalPoint(pick);
				cam->SetPosition(pick[0]+d[0], pick[1]+d[1], pick[2]+d[2]);
				c->curView = 0;                  // focal moved: a view key should re-snap
				s->ren->ResetCameraClippingRange();
				renderWin(c);
			}
		}
	}
	// '1' snaps to front view: azimuth 0 (look north, +Y), elevation 0 (horizontal),
	// world +Z up. Camera due south of the focal point at the same height. Same
	// convention as the default view (buildAndShow) but with elevation 0.
	if (key && key[0]=='1' && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->ren && c->curView != 1) {      // already front view -> no-op (not cumulative)
			vtkCamera* cam = s->ren->GetActiveCamera();
			double fp[3]; cam->GetFocalPoint(fp);
			double dist = cam->GetDistance();
			cam->SetViewUp(0.0, 0.0, 1.0);
			cam->SetPosition(fp[0], fp[1] - dist, fp[2]);
			fitSnapView(s, /*topMode=*/false);     // fill viewport width (annotations kept clear)
			c->curView = 1;
			renderWin(c);
		}
	}
	// '2' snaps to top view: elevation 90 (camera straight above the focal point, looking
	// down -Z). At elevation 90 the +Z up-vector is degenerate, so use +Y (north up), which
	// matches azimuth 0. Map looks down from directly overhead.
	if (key && key[0]=='2' && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->ren && c->curView != 2) {      // already top view -> no-op (not cumulative)
			vtkCamera* cam = s->ren->GetActiveCamera();
			double fp[3]; cam->GetFocalPoint(fp);
			double dist = cam->GetDistance();
			cam->SetViewUp(0.0, 1.0, 0.0);
			cam->SetPosition(fp[0], fp[1], fp[2] + dist);
			fitSnapView(s, /*topMode=*/true);      // fill width OR height by data aspect
			c->curView = 2;
			renderWin(c);
		}
	}
	// '3' snaps to right view: azimuth 270 (look west, -X), elevation 0 (horizontal),
	// world +Z up. Camera due east of the focal point at the same height. Azimuth here is
	// the compass bearing of the view direction (foc - pos): east-of-focal looking west
	// gives atan2(-1, 0) = -90 => 270.
	if (key && key[0]=='3' && key[1]=='\0') {
		// Blank the key code so VTK's built-in trackball OnChar does NOT also fire its
		// hardwired '3' = stereo-render toggle on the following CharEvent. We only want the
		// right-view snap. KeyPressEvent (here) runs before CharEvent, so this lands in time.
		rwi->SetKeyCode(0);
		Scene* s = c->s;
		if (s && s->ren && c->curView != 3) {      // already right view -> no-op (not cumulative)
			vtkCamera* cam = s->ren->GetActiveCamera();
			double fp[3]; cam->GetFocalPoint(fp);
			double dist = cam->GetDistance();
			cam->SetViewUp(0.0, 0.0, 1.0);
			cam->SetPosition(fp[0] + dist, fp[1], fp[2]);
			fitSnapView(s, /*topMode=*/false);     // fill viewport width (annotations kept clear)
			c->curView = 3;
			renderWin(c);
		}
	}
	// '4' snaps to back view: azimuth 180 (look south, -Y), elevation 0, world +Z up.
	// Camera due north of the focal point at the same height. (Front view is azimuth 0.)
	if (key && key[0]=='4' && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->ren && c->curView != 4) {      // already back view -> no-op (not cumulative)
			vtkCamera* cam = s->ren->GetActiveCamera();
			double fp[3]; cam->GetFocalPoint(fp);
			double dist = cam->GetDistance();
			cam->SetViewUp(0.0, 0.0, 1.0);
			cam->SetPosition(fp[0], fp[1] + dist, fp[2]);
			fitSnapView(s, /*topMode=*/false);     // fill viewport width (annotations kept clear)
			c->curView = 4;
			renderWin(c);
		}
	}
	// '5' snaps to left view: azimuth 90 (look east, +X), elevation 0, world +Z up.
	// Camera due west of the focal point at the same height (mirror of right view).
	if (key && key[0]=='5' && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->ren && c->curView != 5) {      // already left view -> no-op (not cumulative)
			vtkCamera* cam = s->ren->GetActiveCamera();
			double fp[3]; cam->GetFocalPoint(fp);
			double dist = cam->GetDistance();
			cam->SetViewUp(0.0, 0.0, 1.0);
			cam->SetPosition(fp[0] - dist, fp[1], fp[2]);
			fitSnapView(s, /*topMode=*/false);     // fill viewport width (annotations kept clear)
			c->curView = 5;
			renderWin(c);
		}
	}
	// '6' toggles ORTHOGRAPHIC (parallel) projection on/off, keeping the current view
	// direction. Turning it on refits the parallel scale to the data; turning it off
	// returns to perspective. Does not change the view-snap state.
	if (key && key[0]=='6' && key[1]=='\0') {
		Scene* s = c->s;
		if (s && s->ren) {
			vtkCamera* cam = s->ren->GetActiveCamera();
			cam->SetParallelProjection(cam->GetParallelProjection() ? 0 : 1);   // toggle ortho<->persp
			// Re-MAXIMIZE in the new projection using the active view's fill mode (side views
			// fill width; top / free views fill the limiting dim). fitSnapView ResetCameras
			// first, so this is idempotent — repeated '6' presses do NOT zoom cumulatively.
			const bool sideView = (c->curView==1 || c->curView==3 || c->curView==4 || c->curView==5);
			fitSnapView(s, /*topMode=*/!sideView);
			renderWin(c);
		}
	}
}

void buildGeometry(Gizmo &c) {
	vtkNew<vtkLineSource> shaftSrc;
	shaftSrc->SetPoint1(0,0,0); shaftSrc->SetPoint2(0,0,kBodyZ);
	c.shaftSrc = shaftSrc;
	c.shaft = makeActor(shaftSrc, 0.95,0.95,0.95);
	c.shaft->GetProperty()->SetLineWidth(2.0); c.shaft->PickableOff();

	vtkNew<vtkConeSource> vconeSrc; vconeSrc->SetResolution(24);
	c.vconeSrc = vconeSrc;
	c.vcone = makeActor(vconeSrc, 1.0,0.85,0.2);  // amber
	litLook(c.vcone); c.vcone->PickableOn(); updateVCone(c);

	vtkNew<vtkLineSource> shaftHSrc;
	shaftHSrc->SetPoint1(0,0,0); shaftHSrc->SetPoint2(1,0,0);
	c.shaftHSrc = shaftHSrc;
	c.shaftH = makeActor(shaftHSrc, 0.95,0.95,0.95);
	c.shaftH->GetProperty()->SetLineWidth(2.0); c.shaftH->PickableOff();

	vtkNew<vtkCylinderSource> harr;
	harr->SetRadius(kRingR); harr->SetHeight(kRingH); harr->SetResolution(72);
	harr->CappingOff(); harr->SetCenter(0,0,0);
	vtkNew<vtkTransform> harrXf; harrXf->PostMultiply(); harrXf->RotateZ(-90.0);
	vtkNew<vtkTransformPolyDataFilter> harrTf;
	harrTf->SetInputConnection(harr->GetOutputPort()); harrTf->SetTransform(harrXf);
	c.harrow = makeActor(harrTf, 0.55,0.55,0.6); ringLook(c.harrow); c.harrow->PickableOn();

	vtkNew<vtkCylinderSource> ringSrc;
	ringSrc->SetRadius(kRingR); ringSrc->SetHeight(kRingH); ringSrc->SetResolution(72);
	ringSrc->CappingOff(); ringSrc->SetCenter(0,0,0);
	vtkNew<vtkTransform> ringXf; ringXf->PostMultiply();
	ringXf->RotateX(90.0); ringXf->Translate(0,0,kBodyZ);
	vtkNew<vtkTransformPolyDataFilter> ringTf;
	ringTf->SetInputConnection(ringSrc->GetOutputPort()); ringTf->SetTransform(ringXf);
	c.ring = makeActor(ringTf, 0.55,0.55,0.6); ringLook(c.ring); c.ring->PickableOn();

	auto mkLabel = [](double fs) {
		vtkSmartPointer<vtkBillboardTextActor3D> t = vtkSmartPointer<vtkBillboardTextActor3D>::New();
		t->GetTextProperty()->SetColor(1,1,1);
		t->GetTextProperty()->SetFontSize((int)fs);
		t->GetTextProperty()->SetJustificationToCentered();
		t->ForceOpaqueOn();                      // never sorted/faded as translucent
		return t;
	};
	c.label = mkLabel(10); updateLabel(c);                       // VE z× label
	c.azLabel = mkLabel(10); c.azLabel->SetInput("0\xC2\xB0");   // azimuth °
	c.inclLabel = mkLabel(10); c.inclLabel->SetInput("0\xC2\xB0"); // inclination °
}

// Any interactor-driven camera move (wheel zoom, middle-button pan, trackball) invalidates
// the active number-key view-snap, so the next view key re-snaps instead of no-op'ing.
void ResetViewCB(vtkObject*, unsigned long, void* clientData, void*) {
	if (Gizmo* c = static_cast<Gizmo*>(clientData)) c->curView = 0;
}

// Build, add to the scene, wire the follow + drag + key observers.
Gizmo *enableGizmo(Scene *s, double sensitivity) {
	Gizmo *c = new Gizmo();
	c->s = s;
	c->curSz = s->ve;                 // reflect the initial exaggeration on the cone/label
	c->veBase = (s->ve > 0.0) ? s->ve : 1.0;   // baseline so the cone starts at default size regardless of auto-VE
	c->sensitivity = (sensitivity > 0.0) ? sensitivity : 0.01;

	vtkRenderer* ren = s->ren;
	vtkRenderWindowInteractor* rwi = s->widget->interactor();

	// Pin horizontal-axis length to the data bbox (XY half-extent) BEFORE adding gizmo props.
	double b[6]; ren->ComputeVisiblePropBounds(b);
	if (b[1] >= b[0] && b[3] >= b[2]) c->haxisLen = 0.5 * std::max(b[1]-b[0], b[3]-b[2]);

	buildGeometry(*c);

	// Cone added LAST so it draws over the rings under the on-top depth offset.
	ren->AddViewProp(c->shaft);  ren->AddViewProp(c->shaftH); ren->AddViewProp(c->ring);
	ren->AddViewProp(c->harrow); ren->AddViewProp(c->vcone);
	// The angle/VE TEXT labels go in the overlay renderer (its headlight lights the camera-facing
	// text uniformly) so they stay WHITE at every tilt — in s->ren the directional sun lit the text
	// quads and they dimmed/vanished at some angles.
	vtkRenderer* tren = s->axesRen ? s->axesRen.Get() : ren;
	tren->AddViewProp(c->label); tren->AddViewProp(c->azLabel); tren->AddViewProp(c->inclLabel);

	vtkNew<vtkLight> light;
	light->SetLightTypeToSceneLight(); light->SetPositional(false);
	light->SetPosition(0.6,0.4,1.0); light->SetFocalPoint(0,0,0);
	light->SetColor(1,1,1); light->SetIntensity(1.4);
	c->light = light; ren->AddLight(light);

	PlaceCB(ren, 0, c, nullptr);

	vtkNew<vtkCallbackCommand> placeCmd; placeCmd->SetCallback(PlaceCB); placeCmd->SetClientData(c);
	c->placeCmd = placeCmd; c->placeTag = ren->AddObserver(vtkCommand::StartEvent, placeCmd);

	vtkNew<vtkCallbackCommand> dragCmd; dragCmd->SetCallback(DragCB); dragCmd->SetClientData(c);
	c->dragCmd = dragCmd;
	c->dragTags[0] = rwi->AddObserver(vtkCommand::LeftButtonPressEvent, dragCmd, 10.0);
	c->dragTags[1] = rwi->AddObserver(vtkCommand::MouseMoveEvent, dragCmd, 10.0);
	c->dragTags[2] = rwi->AddObserver(vtkCommand::LeftButtonReleaseEvent, dragCmd, 10.0);

	vtkNew<vtkCallbackCommand> keyCmd; keyCmd->SetCallback(KeyCB); keyCmd->SetClientData(c);
	c->keyCmd = keyCmd; c->keyTag = rwi->AddObserver(vtkCommand::KeyPressEvent, keyCmd, -1.0);

	vtkNew<vtkCallbackCommand> interactCmd; interactCmd->SetCallback(ResetViewCB); interactCmd->SetClientData(c);
	c->interactCmd = interactCmd; c->interactTag = rwi->AddObserver(vtkCommand::InteractionEvent, interactCmd);

	renderWin(c);
	return c;
}

} // namespace (gizmo)

// One shared QApplication for the whole process (created lazily). The viewer is
// NON-BLOCKING: buildAndShow creates and shows a window then returns; the host
