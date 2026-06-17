// ============================================================================
//  Profile track (Ctrl+left-drag): sample the surface elevation along a line and
//  show it as a 2D (distance, elevation) graph docked at the bottom — the
//  Fledermaus / GMTF3D "Profile" panel. Ported from the f3dx L2 profiler, but the
//  sampling is VERTICAL-RAY (a vtkCellLocator shoots a vertical ray at each point
//  densified along the track -> exactly one true z per sample, no off-track noise;
//  NOT a vtkCutter/polyplane, which spills onto the whole surface).
// ============================================================================

// 2D profile plot. Pure QPainter (VTK has no working context-2D GL backend in this
// build); a plain QWidget paints axes + the (s,z) polyline.
class ProfilePanel : public QWidget {
public:
	ProfilePanel(QWidget* parent = nullptr) : QWidget(parent) {
		setMinimumHeight(170);
		setAutoFillBackground(true);
	}
	void setProfile(const std::vector<double>& s, const std::vector<double>& z) {
		m_s = s; m_z = z; update();
	}
protected:
	std::vector<double> m_s, m_z;

	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.fillRect(rect(), QColor(250, 250, 250));
		const int L = 62, R = 16, T = 12, B = 36;
		QRectF plot(L, T, width() - L - R, height() - T - B);
		if (plot.width() < 20 || plot.height() < 20)
			return;

		if (m_s.size() < 2) {
			p.setPen(QColor(120, 120, 120));
			p.drawText(rect(), Qt::AlignCenter,
				"Ctrl + left-drag on the surface to draw an elevation profile");
			return;
		}

		double smin = m_s.front(), smax = m_s.back();
		double zmin = m_z[0], zmax = m_z[0];
		for (double v : m_z) { zmin = std::min(zmin, v); zmax = std::max(zmax, v); }
		if (smax <= smin) smax = smin + 1.0;
		if (zmax <= zmin) zmax = zmin + 1.0;
		double zpad = 0.06 * (zmax - zmin);
		zmin -= zpad; zmax += zpad;

		auto X = [&](double s) { return plot.left()   + (s - smin) / (smax - smin) * plot.width();  };
		auto Y = [&](double z) { return plot.bottom() - (z - zmin) / (zmax - zmin) * plot.height(); };

		// gridlines + tick labels (nice 1/2/5 steps)
		p.setFont(QFont(font().family(), 8));
		const double sstep = niceNum(niceNum(smax - smin, false) / 6.0, true);
		const double zstep = niceNum(niceNum(zmax - zmin, false) / 5.0, true);
		p.setPen(QColor(225, 225, 225));
		for (double v = std::ceil(smin / sstep) * sstep; v <= smax; v += sstep)
			p.drawLine(QPointF(X(v), plot.top()), QPointF(X(v), plot.bottom()));
		for (double v = std::ceil(zmin / zstep) * zstep; v <= zmax; v += zstep)
			p.drawLine(QPointF(plot.left(), Y(v)), QPointF(plot.right(), Y(v)));

		p.setPen(QColor(110, 110, 110));
		for (double v = std::ceil(smin / sstep) * sstep; v <= smax; v += sstep)
			p.drawText(QRectF(X(v) - 30, plot.bottom() + 2, 60, 16),
					   Qt::AlignHCenter | Qt::AlignTop, QString::number(v, 'g', 4));
		for (double v = std::ceil(zmin / zstep) * zstep; v <= zmax; v += zstep)
			p.drawText(QRectF(0, Y(v) - 8, L - 6, 16),
					   Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'g', 4));

		p.setPen(QColor(140, 140, 140));
		p.drawRect(plot);

		// the profile curve
		QPainterPath path;
		path.moveTo(X(m_s[0]), Y(m_z[0]));
		for (size_t i = 1; i < m_s.size(); ++i)
			path.lineTo(X(m_s[i]), Y(m_z[i]));
		p.setPen(QPen(QColor(235, 170, 0), 2));
		p.drawPath(path);

		// axis captions
		p.setPen(Qt::black);
		p.setFont(QFont(font().family(), 8));
		p.drawText(QRectF(plot.left(), height() - 16, plot.width(), 14),
				   Qt::AlignHCenter, "Distance");
		p.save();
		p.translate(12, plot.center().y());
		p.rotate(-90);
		p.drawText(QRectF(-60, -10, 120, 14), Qt::AlignHCenter, "Elevation");
		p.restore();
	}
};

// Wipe the profile: empty the 3D line, drop its texture/state, clear the 2D panel.
static void profileClear(Scene* s) {
	if (!s || !s->profLine) return;
	vtkNew<vtkPolyData> empty;
	if (auto* mm = vtkPolyDataMapper::SafeDownCast(s->profLine->GetMapper()))
		mm->SetInputData(empty);
	s->profLine->SetVisibility(0);
	s->profLine->SetTexture(nullptr);
	s->profPD = nullptr; s->profStripe = nullptr; s->profStyle = 0;
	s->profS.clear(); s->profZ.clear();
	if (s->prof) s->prof->setProfile({}, {});
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Pick the surface point under cursor device px (dx,dy) and convert to TRUE (x,y) —
// undo the actor's horizontal scale (xfac). Returns false if the cursor misses the surface.
static bool pickSurfaceXY(Scene* s, int dx, int dy, double& tx, double& ty) {
	if (!s || !s->ren || !s->surf)
		return false;
	vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
	pk->PickFromListOn(); pk->AddPickList(s->surf);
	if (!pk->Pick((double)dx, (double)dy, 0.0, s->ren))
		return false;
	double pp[3]; pk->GetPickPosition(pp);
	tx = (s->xfac != 0.0) ? pp[0] / s->xfac : pp[0];   // -> true coords
	ty = pp[1];
	return true;
}

// Sample the surface elevation along the straight track (ax,ay)->(bx,by) by shooting a
// vertical ray at each densified (x,y) (one true z per sample), then refresh the 3D drape
// line + the 2D panel. Arc length is in metres (geographic) / data units (cartesian).
static void computeProfile(Scene* s, double ax, double ay, double bx, double by) {
	if (!s || s->gridZ.empty())                        // sample the full-res DATA layer (no locator)
		return;
	const int N = 300;
	const double m_per_base = (s->zfac > 0.0) ? (1.0 / s->zfac) : 1.0;  // base horiz unit -> metres

	std::vector<double> sv, zv;
	vtkNew<vtkPoints> line; line->SetDataTypeToDouble();
	double lastx = ax, lasty = ay, sdist = 0.0; bool have = false;
	for (int i = 0; i < N; ++i) {
		const double t = (N == 1) ? 0.0 : double(i) / (N - 1);
		const double x = ax + t * (bx - ax), y = ay + t * (by - ay);
		const double z = sampleZ(s, x, y);             // bilinear, full-res, render-LOD independent
		if (std::isnan(z))
			continue;                                  // off-grid / NaN sample -> skip
		if (have) {
			const double ddx = (x - lastx) * s->xfac, ddy = (y - lasty);
			sdist += std::sqrt(ddx * ddx + ddy * ddy) * m_per_base;
		}
		lastx = x; lasty = y; have = true;
		line->InsertNextPoint(x, y, z);
		sv.push_back(sdist); zv.push_back(z);
	}

	const vtkIdType np = line->GetNumberOfPoints();
	vtkNew<vtkCellArray> ca;
	if (np >= 2) {
		ca->InsertNextCell(np);
		for (vtkIdType i = 0; i < np; ++i)
			ca->InsertCellPoint(i);
	}
	vtkNew<vtkPolyData> lpd; lpd->SetPoints(line); lpd->SetLines(ca);
	s->profPD = lpd;                                   // keep for restyle + save
	if (auto* m = vtkPolyDataMapper::SafeDownCast(s->profLine->GetMapper()))
		m->SetInputData(lpd);
	s->profLine->SetVisibility(np >= 2 ? 1 : 0);
	s->profStyle = 0;                                  // a fresh line starts solid
	s->profStripe = nullptr;
	s->profLine->SetTexture(nullptr);
	s->profLine->GetProperty()->SetOpacity(1.0);
	s->profS = sv; s->profZ = zv;

	if (s->prof) s->prof->setProfile(sv, zv);
	if (s->win && np >= 2)
		s->win->statusBar()->showMessage(
			QString("Profile: 2D distance %1   elevation %2 .. %3   (%4 samples)")
			.arg(sv.back(), 0, 'f', 2)
			.arg(*std::min_element(zv.begin(), zv.end()), 0, 'f', 2)
			.arg(*std::max_element(zv.begin(), zv.end()), 0, 'f', 2)
			.arg((int)np));
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

static void profilerBegin(Scene* s, int dx, int dy) {
	if (!s)
		return;
	double tx, ty;
	if (!pickSurfaceXY(s, dx, dy, tx, ty))
		return;
	s->track0[0] = tx; s->track0[1] = ty; s->profiling = true;
	if (s->bottomDock) {                       // surface the Profile tab in the bottom dock
		s->bottomDock->setVisible(true);
		setBottomCollapsed(s, false);
		if (s->bottomTabs && s->prof) s->bottomTabs->setCurrentWidget(s->prof);
	}
}

static void profilerDrag(Scene* s, int dx, int dy) {
	if (!s || !s->profiling)
		return;
	double tx, ty;
	if (!pickSurfaceXY(s, dx, dy, tx, ty))
		return;
	computeProfile(s, s->track0[0], s->track0[1], tx, ty);
}

static void profilerEnd(Scene* s) {
	if (s) s->profiling = false;
}

class GLView : public QVTKOpenGLNativeWidget {
public:
	Scene* s = nullptr;
	GLView() : QVTKOpenGLNativeWidget() {}
protected:
	bool   midDown = false, midMoved = false;
	QPoint midPress, midLast;

	void devPx(const QPoint& p, double& dx, double& dy) {
		const double r = devicePixelRatioF();
		const int    H = renderWindow()->GetSize()[1];
		dx = p.x() * r;                 // VTK display coords = bottom-up device px
		dy = H - p.y() * r;
	}
	void recenterAt(const QPoint& p) {
		if (!s || !s->ren || !s->surf) return;
		vtkRenderer* ren = s->ren; vtkCamera* cam = ren->GetActiveCamera();
		if (!cam) return;
		double x, y; devPx(p, x, y);
		vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
		pk->PickFromListOn(); pk->AddPickList(s->surf);
		if (pk->Pick(x, y, 0.0, ren)) {
			double pick[3]; pk->GetPickPosition(pick);
			double pos[3], fp[3]; cam->GetPosition(pos); cam->GetFocalPoint(fp);
			const double d[3] = { pos[0]-fp[0], pos[1]-fp[1], pos[2]-fp[2] };
			cam->SetFocalPoint(pick);
			cam->SetPosition(pick[0]+d[0], pick[1]+d[1], pick[2]+d[2]);
			ren->ResetCameraClippingRange();
			renderWindow()->Render();
		}
	}
	void panBy(const QPoint& prev, const QPoint& cur) {
		if (!s || !s->ren) return;
		vtkRenderer* ren = s->ren; vtkCamera* cam = ren->GetActiveCamera();
		if (!cam) return;
		double ox, oy, nx, ny; devPx(prev, ox, oy); devPx(cur, nx, ny);
		double fp[3]; cam->GetFocalPoint(fp);
		ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0); ren->WorldToDisplay();
		const double depth = ren->GetDisplayPoint()[2];
		ren->SetDisplayPoint(nx, ny, depth); ren->DisplayToWorld();
		double np[4]; for (int i=0;i<4;++i) np[i]=ren->GetWorldPoint()[i];
		ren->SetDisplayPoint(ox, oy, depth); ren->DisplayToWorld();
		double op[4]; for (int i=0;i<4;++i) op[i]=ren->GetWorldPoint()[i];
		if (np[3]!=0.0) { np[0]/=np[3]; np[1]/=np[3]; np[2]/=np[3]; }
		if (op[3]!=0.0) { op[0]/=op[3]; op[1]/=op[3]; op[2]/=op[3]; }
		const double m[3] = { op[0]-np[0], op[1]-np[1], op[2]-np[2] };
		double pos[3]; cam->GetPosition(pos);
		cam->SetFocalPoint(fp[0]+m[0], fp[1]+m[1], fp[2]+m[2]);
		cam->SetPosition (pos[0]+m[0], pos[1]+m[1], pos[2]+m[2]);
		ren->ResetCameraClippingRange();
		renderWindow()->Render();
	}
	void mousePressEvent(QMouseEvent* e) override {
		if (e->button() == Qt::MiddleButton) {
			midDown = true; midMoved = false;
			midPress = e->position().toPoint(); midLast = midPress;
			renderWindow()->SetDesiredUpdateRate(15.0);   // LOD decimation while panning
			return;                     // consume; keep VTK out of the middle button
		}
		QVTKOpenGLNativeWidget::mousePressEvent(e);
	}
	void mouseMoveEvent(QMouseEvent* e) override {
		if (midDown) {
			const QPoint cp = e->position().toPoint();
			if ((cp - midPress).manhattanLength() > 3) midMoved = true;
			if (midMoved) { panBy(midLast, cp); midLast = cp; }
			return;
		}
		QVTKOpenGLNativeWidget::mouseMoveEvent(e);
	}
	void mouseReleaseEvent(QMouseEvent* e) override {
		if (e->button() == Qt::MiddleButton) {
			if (midDown && !midMoved) recenterAt(e->position().toPoint());
			midDown = false;
			renderWindow()->SetDesiredUpdateRate(0.0001);   // back to full resolution when still
			renderWindow()->Render();
			return;
		}
		QVTKOpenGLNativeWidget::mouseReleaseEvent(e);
	}
};

// Build a window around a prepared surface and SHOW it (non-blocking). xfac/zfac
// are the base actor scales (geographic aspect + true-scale unit conversion); ve0
// is the initial vertical exaggeration. Cube-axis labels are pinned to the true
// (x0,x1,y0,y1,zmin,zmax) ranges so they stay correct under the scaling.
