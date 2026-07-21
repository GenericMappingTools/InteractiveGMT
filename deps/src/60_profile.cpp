// ============================================================================
//  Profile track (Ctrl+left-drag): sample the surface elevation along a line and
//  show it as a 2D (distance, elevation) graph docked at the bottom — the
//  Fledermaus / GMTF3D "Profile" panel. Ported from the f3dx L2 profiler, but the
//  sampling is VERTICAL-RAY (a vtkCellLocator shoots a vertical ray at each point
//  densified along the track -> exactly one true z per sample, no off-track noise;
//  NOT a vtkCutter/polyplane, which spills onto the whole surface).
// ============================================================================

// Forward decl: the standalone X,Y plot tool (65_xyplot.cpp, #included later). The Profile panel's
// right-click "Open in X,Y plot tool" hands its current (x,y) series to this to spawn a full plotter.
struct XYPlot;
static XYPlot *openSeriesInXYTool(const std::vector<double>& x, const std::vector<double>& y,
                                  const char *title, const char *xlabel, const char *ylabel);

// 2D profile plot. Pure QPainter (VTK has no working context-2D GL backend in this
// build); a plain QWidget paints axes + the (s,z) polyline.
class ProfilePanel : public QWidget {
public:
	ProfilePanel(QWidget *parent = nullptr) : QWidget(parent) {
		setMinimumHeight(170);
		setAutoFillBackground(true);
	}
	void setProfile(const std::vector<double>& s, const std::vector<double>& z) {
		m_s = s; m_z = z;
		m_title.clear(); m_xlabel = "Distance"; m_ylabel = "Elevation"; m_isDate = false;
		update();
	}
	// Generic (x,y) series (e.g. a downloaded tide gauge: x = epoch seconds, y = sea level).
	// isDate -> the x ticks are painted as date/time labels instead of plain numbers.
	void setSeries(const std::vector<double>& x, const std::vector<double>& y,
	               const QString& title, const QString& xlabel, const QString& ylabel, bool isDate) {
		m_s = x; m_z = y;
		m_title = title; m_xlabel = xlabel; m_ylabel = ylabel; m_isDate = isDate;
		update();
	}
	// Read the currently shown series (for "Open in X,Y plot tool" + its C API).
	const std::vector<double>& seriesX() const { return m_s; }
	const std::vector<double>& seriesY() const { return m_z; }
	QString seriesTitle()  const { return m_title; }
	QString seriesXLabel() const { return m_xlabel; }
	QString seriesYLabel() const { return m_ylabel; }
protected:
	std::vector<double> m_s, m_z;
	QString m_title;
	QString m_xlabel = "Distance", m_ylabel = "Elevation";
	bool    m_isDate = false;

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
		double sstep;
		if (m_isDate) {                              // pick a natural time step (hours .. months)
			const double span = smax - smin;
			const double cand[] = {3600, 3*3600, 6*3600, 12*3600, 86400, 2*86400.0,
			                       7*86400.0, 14*86400.0, 30*86400.0};
			sstep = cand[(sizeof(cand)/sizeof(cand[0])) - 1];
			for (double c : cand) if (span / c <= 8) { sstep = c; break; }
		} else {
			sstep = niceNum(niceNum(smax - smin, false) / 6.0, true);
		}
		const double zstep = niceNum(niceNum(zmax - zmin, false) / 5.0, true);
		// x-tick label: a date/time string for a date axis, else a plain number.
		const double xspan = smax - smin;
		auto xlab = [&](double v) -> QString {
			if (!m_isDate) return QString::number(v, 'g', 4);
			QDateTime dt = QDateTime::fromSecsSinceEpoch((qint64)v, Qt::UTC);
			return dt.toString(xspan < 2*86400 ? "MM-dd hh:mm" : "MM-dd");
		};
		p.setPen(QColor(225, 225, 225));
		for (double v = std::ceil(smin / sstep) * sstep; v <= smax; v += sstep)
			p.drawLine(QPointF(X(v), plot.top()), QPointF(X(v), plot.bottom()));
		for (double v = std::ceil(zmin / zstep) * zstep; v <= zmax; v += zstep)
			p.drawLine(QPointF(plot.left(), Y(v)), QPointF(plot.right(), Y(v)));

		p.setPen(QColor(110, 110, 110));
		for (double v = std::ceil(smin / sstep) * sstep; v <= smax; v += sstep)
			p.drawText(QRectF(X(v) - 34, plot.bottom() + 2, 68, 16),
					   Qt::AlignHCenter | Qt::AlignTop, xlab(v));
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
				   Qt::AlignHCenter, m_xlabel);
		p.save();
		p.translate(12, plot.center().y());
		p.rotate(-90);
		p.drawText(QRectF(-60, -10, 120, 14), Qt::AlignHCenter, m_ylabel);
		p.restore();
	}

	// Right-click -> push the currently shown profile/series into a standalone X,Y plot window
	// (Object Manager + Analysis + save). Works for both the Ctrl-drag elevation profile and a
	// downloaded tide series — whatever this panel currently shows.
	void contextMenuEvent(QContextMenuEvent *e) override {
		QMenu m(this);
		QAction *a = m.addAction("Open in X,Y plot tool");
		a->setEnabled(m_s.size() >= 2);
		if (m.exec(e->globalPos()) == a && m_s.size() >= 2)
			openSeriesInXYTool(m_s, m_z,
				m_title.isEmpty() ? "i'GMT  —  Profile" : m_title.toUtf8().constData(),
				m_xlabel.toUtf8().constData(), m_ylabel.toUtf8().constData());
	}
};

// Wipe the profile: empty the 3D line, drop its texture/state, clear the 2D panel.
static void profileClear(Scene *s) {
	if (!s || !s->profLine) return;
	vtkNew<vtkPolyData> empty;
	if (auto *mm = vtkPolyDataMapper::SafeDownCast(s->profLine->GetMapper()))
		mm->SetInputData(empty);
	s->profLine->SetVisibility(0);
	s->profLine->SetTexture(nullptr);
	s->profPD = nullptr; s->profStripe = nullptr; s->profStyle = 0;
	s->profS.clear(); s->profZ.clear();
	if (s->prof) s->prof->setProfile({}, {});
	rebuildSceneObjects(s);   // drop the Profile row from the Scene Objects list
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Pick the surface point under cursor device px (dx,dy) and convert to TRUE (x,y) —
// undo the actor's horizontal scale (xfac). Returns false if the cursor misses the surface.
static bool pickSurfaceXY(Scene *s, int dx, int dy, double& tx, double& ty) {
	if (!s || !s->ren || !s->surf)
		return false;
	vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
	pk->PickFromListOn(); pk->AddPickList(surfProp(s));
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
static void computeProfile(Scene *s, double ax, double ay, double bx, double by) {
	if (!s || (s->gridZ.empty() && !(s->actZ && !s->actZ->empty())))   // need a data layer (active grid or base)
		return;
	const int N = 300;
	const double m_per_base = (s->zfac > 0.0) ? (1.0 / s->zfac) : 1.0;  // base horiz unit -> metres

	std::vector<double> sv, zv;
	vtkNew<vtkPoints> line; line->SetDataTypeToDouble();
	double lastx = ax, lasty = ay, sdist = 0.0; bool have = false;
	for (int i = 0; i < N; ++i) {
		const double t = (N == 1) ? 0.0 : double(i) / (N - 1);
		const double x = ax + t * (bx - ax), y = ay + t * (by - ay);
		const double z = sampleActiveZ(s, x, y);       // bilinear on the ACTIVE grid, full-res, LOD independent
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
	if (auto *m = vtkPolyDataMapper::SafeDownCast(s->profLine->GetMapper()))
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

static bool profilerBegin(Scene *s, int dx, int dy) {
	if (!s)
		return false;
	// A blank empty-launcher plane and a Background-region canvas are both real pickable actors
	// (imageOnly, no drape) — pickSurfaceXY would hit them same as real data. Gate on the SAME
	// "does this window actually hold a grid/image" predicate the Save menu uses (30_app.cpp) so
	// vector-only / Background-region windows never arm a profile track over nothing.
	if (!sceneHasGrid(s) && !sceneHasImage(s))
		return false;
	double tx, ty;
	if (!pickSurfaceXY(s, dx, dy, tx, ty))         // no grid/image under the cursor -> nothing to track
		return false;
	s->track0[0] = tx; s->track0[1] = ty; s->profiling = true;
	if (s->bottomDock) {                       // surface the Profile tab in the bottom dock
		s->bottomDock->setVisible(true);
		setBottomCollapsed(s, false);
		if (s->bottomTabs && s->prof) s->bottomTabs->setCurrentWidget(s->prof);
	}
	return true;
}

static void profilerDrag(Scene *s, int dx, int dy) {
	if (!s || !s->profiling)
		return;
	double tx, ty;
	if (!pickSurfaceXY(s, dx, dy, tx, ty))
		return;
	computeProfile(s, s->track0[0], s->track0[1], tx, ty);
}

static void profilerEnd(Scene *s) {
	if (!s) return;
	s->profiling = false;
	rebuildSceneObjects(s);   // a profile line now exists -> show its row in the Scene Objects list
}

// Polygon tool mouse handlers (defined in 85_polygon.cpp, #included later). GLView drives them
// directly because VTK's adapter doesn't deliver Qt double-clicks as a second LeftButtonPress.
// Each returns true when it consumed the event (the widget then skips VTK's base handler).
static bool polygonHandlePress(Scene *s, int button, int x, int y, bool shift);
static bool polygonHandleDblClick(Scene *s, int x, int y);
static bool polygonHandleMove(Scene *s, int x, int y);
static bool polygonHandleRelease(Scene *s);
static int  polyHitHandle(Scene *s, int x, int y, double tol);   // vertex handle under cursor (85)
static int  polyHitText(Scene *s, int x, int y, double tol);     // text label under cursor (85)
static int  mecaHitAt(Scene *s, int x, int y);                   // focal-mechanism ball under cursor (85)
static bool symHitHandle(Scene *s, int x, int y, double tol);    // armed symbol under cursor (85)

class GLView : public QVTKOpenGLNativeWidget {
public:
	Scene *s = nullptr;
	GLView() : QVTKOpenGLNativeWidget() { setFocusPolicy(Qt::StrongFocus); }   // so Ctrl+C (keyPressEvent) reaches it
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
		vtkRenderer *ren = s->ren; vtkCamera *cam = ren->GetActiveCamera();
		if (!cam) return;
		double x, y; devPx(p, x, y);
		vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
		pk->PickFromListOn(); pk->AddPickList(surfProp(s));
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
		vtkRenderer *ren = s->ren; vtkCamera *cam = ren->GetActiveCamera();
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
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::MiddleButton) {
			midDown = true; midMoved = false;
			midPress = e->position().toPoint(); midLast = midPress;
			renderWindow()->SetDesiredUpdateRate(15.0);   // LOD decimation while panning
			return;                     // consume; keep VTK out of the middle button
		}
		// Colorbar: left-press inside its frame grabs it for dragging (overlay, so checked first).
		if (s && e->button() == Qt::LeftButton) {
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			const int *sz = renderWindow()->GetSize();
			if (sz[0] > 0 && sz[1] > 0 && colorbarGrab(s, dx / sz[0], dy / sz[1]))
				return;                 // consumed -> keep VTK (gizmo / dolly) out of it
		}
		// Polygon tool: left adds/edits vertices, right removes the last while drawing.
		if (s && (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)) {
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			const bool shift = (e->modifiers() & Qt::ShiftModifier) != 0;
			if (polygonHandlePress(s, e->button() == Qt::LeftButton ? 0 : 1, (int)dx, (int)dy, shift))
				return;                 // consumed -> keep VTK (gizmo / dolly) out of it
		}
		QVTKOpenGLNativeWidget::mousePressEvent(e);
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		// Double-left-click closes the polygon (draw mode) or enters/leaves vertex-edit mode.
		if (s && e->button() == Qt::LeftButton) {
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			if (polygonHandleDblClick(s, (int)dx, (int)dy))
				return;
		}
		QVTKOpenGLNativeWidget::mouseDoubleClickEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (midDown) {
			const QPoint cp = e->position().toPoint();
			if ((cp - midPress).manhattanLength() > 3) midMoved = true;
			if (midMoved) { panBy(midLast, cp); midLast = cp; }
			return;
		}
		// Colorbar drag in progress: move it and consume.
		if (s && s->barDragging) {
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			const int *sz = renderWindow()->GetSize();
			if (sz[0] > 0 && sz[1] > 0) colorbarDragTo(s, dx / sz[0], dy / sz[1]);
			return;
		}
		// Hover feedback: show the quadruple-arrow over any draggable element (colorbar / polygon
		// vertex handle in edit mode / text label). Skipped while the draw tool owns a crosshair.
		if (s && !s->polyMode && !s->polyDragWhole) {   // skip hover-cursor while a whole-element drag owns the crosshair
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			const int *sz = renderWindow()->GetSize();
			const double nx = sz[0] > 0 ? dx / sz[0] : 0.0;
			const double ny = sz[1] > 0 ? dy / sz[1] : 0.0;
			const bool over = colorbarHit(s, nx, ny)
			               || (s->polyEdit >= 0 && polyHitHandle(s, (int)dx, (int)dy, 10.0) >= 0)
			               || polyHitText(s, (int)dx, (int)dy, 14.0) >= 0
			               || mecaHitAt(s, (int)dx, (int)dy) >= 0
			               || (s->symArmed >= 0 && symHitHandle(s, (int)dx, (int)dy, 16.0));
			const bool isAll = cursor().shape() == Qt::SizeAllCursor;
			if (over && !isAll)       setCursor(Qt::SizeAllCursor);
			else if (!over && isAll)  unsetCursor();
		}
		// Polygon tool: extend the draw preview / drag a grabbed vertex (consumes only when active).
		if (s) {
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			if (polygonHandleMove(s, (int)dx, (int)dy))
				return;
		}
		QVTKOpenGLNativeWidget::mouseMoveEvent(e);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::MiddleButton) {
			if (midDown && !midMoved) recenterAt(e->position().toPoint());
			midDown = false;
			renderWindow()->SetDesiredUpdateRate(0.0001);   // back to full resolution when still
			renderWindow()->Render();
			return;
		}
		if (s && e->button() == Qt::LeftButton && colorbarRelease(s))
			return;                     // ended a colorbar drag
		if (s && e->button() == Qt::LeftButton && polygonHandleRelease(s))
			return;                     // ended a vertex drag
		QVTKOpenGLNativeWidget::mouseReleaseEvent(e);
	}
	// Ctrl+C while a symbol is armed (double-click "edit mode" selection, see Scene::symArmed) copies
	// its X/Y[/Z] (TRUE coords, x un-baked out of xfac) to the clipboard as a tab-separated line —
	// same numeric formatting as the Show Data Table / Save line float precision.
	// Ctrl+C while a line-family object (polygon/polyline/rect/circle/fault) is in vertex-edit mode
	// (double-click selection, see Scene::polyEdit) copies EVERY vertex, one tab-separated row per
	// line — same X/Y/Z columns and 'g',10 precision as showLineDataTable (55_lineprops.cpp), and
	// the same Z-drop in flat-2D.
	void keyPressEvent(QKeyEvent *e) override {
		if (s && s->symArmed >= 0 && s->symArmed < (int)s->symbols.size() && e->matches(QKeySequence::Copy)) {
			SymbolLayer &sl = s->symbols[s->symArmed];
			if (auto *pd = symInputPD(sl)) {
				if (pd->GetPoints() && pd->GetPoints()->GetNumberOfPoints() > 0) {
					double p[3]; pd->GetPoints()->GetPoint(0, p);
					const double xfacInv = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
					QString txt = QString::number(p[0] * xfacInv, 'g', 10) + "\t" + QString::number(p[1], 'g', 10);
					if (!s->flat2d) txt += "\t" + QString::number(p[2], 'g', 10);
					QApplication::clipboard()->setText(txt);
				}
			}
			return;
		}
		if (s && s->polyEdit >= 0 && s->polyEdit < (int)s->polys.size() && e->matches(QKeySequence::Copy)) {
			const Polygon &pg = s->polys[s->polyEdit];
			QStringList rows;
			for (const auto &v : pg.v) {
				QString row = QString::number(v[0], 'g', 10) + "\t" + QString::number(v[1], 'g', 10);
				if (!s->flat2d) row += "\t" + QString::number(v[2], 'g', 10);
				rows << row;
			}
			QApplication::clipboard()->setText(rows.join("\n"));
			return;
		}
		QVTKOpenGLNativeWidget::keyPressEvent(e);
	}
};

// Build a window around a prepared surface and SHOW it (non-blocking). xfac/zfac
// are the base actor scales (geographic aspect + true-scale unit conversion); ve0
// is the initial vertical exaggeration. Cube-axis labels are pinned to the true
// (x0,x1,y0,y1,zmin,zmax) ranges so they stay correct under the scaling.
