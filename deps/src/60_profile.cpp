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
static XYPlot *openSeriesInXYTool(const std::vector<double> &x, const std::vector<double> &y,
                                  const char *title, const char *xlabel, const char *ylabel);

// 2D profile plot. Pure QPainter (VTK has no working context-2D GL backend in this
// build); a plain QWidget paints axes + the (s,z) polyline.
class ProfilePanel : public QWidget {
public:
	ProfilePanel(QWidget *parent = nullptr) : QWidget(parent) {
		setMinimumHeight(170);
		setAutoFillBackground(true);
		setMouseTracking(true);       // the hover coordinate readout needs moves with no button down
	}
	// `gridName` is the label of the grid that was sampled: it becomes the title, so a profile window is
	// named "Profile <grid name>" and nothing longer.
	void setProfile(const std::vector<double> &s, const std::vector<double> &z,
	                const QString &gridName = QString()) {
		m_s = s; m_z = z;
		if (z.empty()) setMark(std::numeric_limits<double>::quiet_NaN());   // no curve -> no marker
		m_title = z.empty() ? QString() : (gridName.isEmpty() ? QString("Profile")
		                                                      : QString("Profile %1").arg(gridName));
		m_xlabel = "Distance"; m_ylabel = "Elevation"; m_isDate = false;
		update();
	}
	// Generic (x,y) series (e.g. a downloaded tide gauge: x = epoch seconds, y = sea level).
	// isDate -> the x ticks are painted as date/time labels instead of plain numbers.
	void setSeries(const std::vector<double> &x, const std::vector<double> &y,
	               const QString &title, const QString &xlabel, const QString &ylabel, bool isDate) {
		m_s = x; m_z = y;
		setMark(std::numeric_limits<double>::quiet_NaN());   // a different series: the old marker means nothing
		m_title = title; m_xlabel = xlabel; m_ylabel = ylabel; m_isDate = isDate;
		update();
	}
	// A SECOND curve over the same axes -- a reference/analytic solution beside the modelled one.
	// It belongs here, in the one panel that already draws a curve, rather than in a second plotting
	// widget: the two curves are the same quantity on the same axes (SACRED_LAW.md). Empty vectors
	// clear it. Only the FIRST curve is what "Open in X,Y plot tool" hands over -- see below.
	void setSeries2(const std::vector<double> &x, const std::vector<double> &y, const QString &name) {
		m_s2 = x; m_z2 = y; m_name2 = name;
		update();
	}
	void clearSeries2() { m_s2.clear(); m_z2.clear(); m_name2.clear(); update(); }
	// Read the currently shown series (for "Open in X,Y plot tool" + its C API).
	const std::vector<double> &seriesX() const { return m_s; }
	const std::vector<double> &seriesY() const { return m_z; }
	// …and the SECOND curve, the reference/analytic one. Read-only, for the test that asserts it
	// follows the slice (gmtvtk_aqua_eta_curve_test): a reference that stops moving is a figure
	// comparing the wave against a solution from another instant, which is worse than no reference.
	const std::vector<double> &seriesX2() const { return m_s2; }
	const std::vector<double> &seriesY2() const { return m_z2; }
	// The curve marker, by its x data value (NaN = none). setMarker is how the 3-D view's own marker
	// drives this one; onMarker is told of EVERY change, whoever made it — the ONE sync path between
	// the two markers.
	double markerX() const { return m_mark; }
	void   setMarker(double x) { setMark(x); }
	std::function<void(double)> onMarker;
	QString seriesTitle()  const { return m_title; }
	QString seriesXLabel() const { return m_xlabel; }
	QString seriesYLabel() const { return m_ylabel; }
protected:
	std::vector<double> m_s, m_z;
	std::vector<double> m_s2, m_z2;      // optional second curve (a reference/analytic solution)
	QString m_name2;                     // its legend text
	QString m_title;
	QString m_xlabel = "Distance", m_ylabel = "Elevation";
	bool    m_isDate = false;
	QPoint  m_hover{-1, -1};             // last cursor position, widget coords (-1,-1 = outside)
	// Marker on the curve: a left click ON the curve drops a circle there; dragging it slides it ALONG
	// the curve (its x follows the cursor, its y is the curve's own value there) with an "x,  y" label
	// beside it. Stored as an x data value, so a live re-sample of the profile (end handle dragged)
	// keeps it at the same distance. NaN = no marker.
	double  m_mark = std::numeric_limits<double>::quiet_NaN();
	bool    m_markDrag = false;
	bool    m_markNew = false;           // the last press PLACED it (so its double-click keeps it)
	bool    m_markMoved = false;         // this press has started sliding it (past a 3 px dead zone)
	QPointF m_markPress;

	// Every marker change goes through here, so the 3-D marker can never miss one.
	void setMark(double x) {
		const bool same = (std::isnan(x) && std::isnan(m_mark)) || x == m_mark;
		m_mark = x;
		update();
		if (!same && onMarker) onMarker(x);
	}

	// The plot frame and the data ranges drawn inside it. The painter and the hover readout MUST
	// agree on this mapping or the numbers under the cursor would not be the numbers on the axes,
	// so it is derived ONCE here and used by both — never re-derived beside the painter.
	bool plotFrame(QRectF &plot, double &smin, double &smax, double &zmin, double &zmax) const {
		const int L = 62, R = 16, T = 12, B = 36;
		plot = QRectF(L, T, width() - L - R, height() - T - B);
		if (plot.width() < 20 || plot.height() < 20 || m_s.size() < 2)
			return false;
		smin = m_s.front(); smax = m_s.back();
		zmin = m_z[0];      zmax = m_z[0];
		for (double v : m_z) { zmin = std::min(zmin, v); zmax = std::max(zmax, v); }
		// A second curve shares these axes, so it has to be inside them or it would be drawn clipped
		// (or off-panel) and the two would not be comparable -- which is the only reason it is there.
		for (double v : m_s2) { smin = std::min(smin, v); smax = std::max(smax, v); }
		for (double v : m_z2) { zmin = std::min(zmin, v); zmax = std::max(zmax, v); }
		if (smax <= smin) smax = smin + 1.0;
		if (zmax <= zmin) zmax = zmin + 1.0;
		const double zpad = 0.06 * (zmax - zmin);
		zmin -= zpad; zmax += zpad;
		return true;
	}

	// Live coordinate readout, the same one the X,Y plot tool shows while the cursor is inside the
	// frame (xyMouseMove, 65_xyplot.cpp): cursor -> data coords off the axes actually drawn, same
	// "x,  y" text, same precision. A panel has no status bar, so it is painted at the LOWER LEFT.
	void mouseMoveEvent(QMouseEvent *e) override {
		m_hover = e->pos();
		QRectF plot;
		double smin, smax, zmin, zmax;
		if (m_markDrag && !m_markMoved && (e->position() - m_markPress).manhattanLength() > 3)
			m_markMoved = true;
		if (m_markDrag && m_markMoved && plotFrame(plot, smin, smax, zmin, zmax)) {   // slide along the curve
			const double x = smin + (e->position().x() - plot.left()) / plot.width() * (smax - smin);
			setMark(std::clamp(x, m_s.front(), m_s.back()));
		}
		// the same four-arrow cursor every other draggable element shows
		const bool over = m_markDrag || markerHit(e->position());
		const bool isAll = cursor().shape() == Qt::SizeAllCursor;
		if (over && !isAll)       setCursor(Qt::SizeAllCursor);
		else if (!over && isAll)  unsetCursor();
		update();
		QWidget::mouseMoveEvent(e);
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && m_s.size() >= 2) {
			m_markPress = e->position();  m_markMoved = false;
			if (markerHit(e->position())) { m_markDrag = true; m_markNew = false; return; }
			double x;
			if (curveHit(e->position(), x)) {          // click ON the curve: drop the marker there, already grabbed
				m_markDrag = true; m_markNew = true;
				setMark(x);
				return;
			}
		}
		QWidget::mousePressEvent(e);
	}
	// Double-click ON an existing marker removes it (and, through onMarker, its 3-D twin). The double-
	// click's FIRST press already went through mousePressEvent: when that press is what PLACED the
	// marker (m_markNew), this double-click is the placing one and keeps it. Double-click on the bare
	// curve places it, like a single click.
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && m_s.size() >= 2) {
			m_markDrag = false;
			if (markerHit(e->position())) {
				if (!m_markNew) setMark(std::numeric_limits<double>::quiet_NaN());
				m_markNew = false;
				return;
			}
			double x;
			if (curveHit(e->position(), x)) { setMark(x); m_markNew = false; return; }
		}
		QWidget::mouseDoubleClickEvent(e);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && m_markDrag) { m_markDrag = false; return; }
		QWidget::mouseReleaseEvent(e);
	}

	// The curve's y at data x (linear between samples; x ascends along the curve).
	double curveYAt(double x) const {
		if (m_s.size() < 2) return std::numeric_limits<double>::quiet_NaN();
		if (x <= m_s.front()) return m_z.front();
		if (x >= m_s.back())  return m_z.back();
		const size_t i = size_t(std::upper_bound(m_s.begin(), m_s.end(), x) - m_s.begin());   // m_s[i-1] <= x < m_s[i]
		const double ds = m_s[i] - m_s[i-1];
		const double t = ds > 0.0 ? (x - m_s[i-1]) / ds : 0.0;
		return m_z[i-1] + t * (m_z[i] - m_z[i-1]);
	}
	// Marker's centre in widget coords; false when there is none (or nothing to put it on).
	bool markerPos(QPointF &c) const {
		QRectF plot;
		double smin, smax, zmin, zmax;
		if (std::isnan(m_mark) || !plotFrame(plot, smin, smax, zmin, zmax))
			return false;
		const double x = std::clamp(m_mark, m_s.front(), m_s.back());
		c = QPointF(plot.left()   + (x - smin) / (smax - smin) * plot.width(),
		            plot.bottom() - (curveYAt(x) - zmin) / (zmax - zmin) * plot.height());
		return true;
	}
	bool markerHit(const QPointF &p) const {
		QPointF c;
		if (!markerPos(c)) return false;
		const double dx = p.x() - c.x(), dy = p.y() - c.y();
		return dx*dx + dy*dy <= 9.0 * 9.0;
	}
	// Is p within a few px of the drawn curve? `x` = the data x of the nearest point on it.
	bool curveHit(const QPointF &p, double &x) const {
		QRectF plot;
		double smin, smax, zmin, zmax;
		if (!plotFrame(plot, smin, smax, zmin, zmax)) return false;
		auto X = [&](double s) { return plot.left()   + (s - smin) / (smax - smin) * plot.width();  };
		auto Y = [&](double z) { return plot.bottom() - (z - zmin) / (zmax - zmin) * plot.height(); };
		double best = 6.0 * 6.0;
		bool hit = false;
		for (size_t i = 0; i + 1 < m_s.size(); ++i) {
			const double a[2] = { X(m_s[i]), Y(m_z[i]) }, b[2] = { X(m_s[i+1]), Y(m_z[i+1]) };
			const double d2 = segDist2(p.x(), p.y(), a, b);
			if (d2 <= best) {
				best = d2;  hit = true;
				const double sx = b[0] - a[0];       // project onto the segment along x
				const double t = sx != 0.0 ? std::clamp((p.x() - a[0]) / sx, 0.0, 1.0) : 0.0;
				x = m_s[i] + t * (m_s[i+1] - m_s[i]);
			}
		}
		return hit;
	}
	void leaveEvent(QEvent *e) override {
		m_hover = QPoint(-1, -1);
		update();
		QWidget::leaveEvent(e);
	}

	// The readout text for the current cursor, empty when it is outside the plot frame.
	QString hoverText() const {
		QRectF plot;
		double smin, smax, zmin, zmax;
		if (m_hover.x() < 0 || !plotFrame(plot, smin, smax, zmin, zmax) || !plot.contains(m_hover))
			return QString();
		const double dx = smin + (m_hover.x() - plot.left())   / plot.width()  * (smax - smin);
		const double dy = zmax - (m_hover.y() - plot.top())    / plot.height() * (zmax - zmin);
		return xyText(dx, dy);
	}
	// "x,  y" as the readout prints it — ONE formatter for the hover readout and the marker label.
	QString xyText(double dx, double dy) const {
		const QString xs = m_isDate
			? QDateTime::fromSecsSinceEpoch((qint64)dx, Qt::UTC).toString("yyyy-MM-dd hh:mm:ss")
			: QString("%1").arg(dx, 0, 'g', 8);
		return QString("%1,  %2").arg(xs).arg(dy, 0, 'g', 6);
	}

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

		double smin, smax, zmin, zmax;
		plotFrame(plot, smin, smax, zmin, zmax);

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

		// …and the reference curve, dashed, in a colour that reads against the first one.
		if (m_s2.size() >= 2 && m_z2.size() == m_s2.size()) {
			QPainterPath q;
			q.moveTo(X(m_s2[0]), Y(m_z2[0]));
			for (size_t i = 1; i < m_s2.size(); ++i)
				q.lineTo(X(m_s2[i]), Y(m_z2[i]));
			QPen pen2(QColor(200, 30, 30), 1.4);
			pen2.setStyle(Qt::DashLine);
			p.setPen(pen2);
			p.drawPath(q);
			if (!m_name2.isEmpty()) {                       // two curves -> say which is which
				const double ly = plot.top() + 8;
				p.setPen(QPen(QColor(235, 170, 0), 2));
				p.drawLine(QPointF(plot.right() - 74, ly), QPointF(plot.right() - 56, ly));
				p.setPen(QColor(60, 60, 60));
				p.drawText(QRectF(plot.right() - 52, ly - 8, 50, 16), Qt::AlignLeft | Qt::AlignVCenter, "model");
				p.setPen(pen2);
				p.drawLine(QPointF(plot.right() - 74, ly + 13), QPointF(plot.right() - 56, ly + 13));
				p.setPen(QColor(60, 60, 60));
				p.drawText(QRectF(plot.right() - 52, ly + 5, 50, 16), Qt::AlignLeft | Qt::AlignVCenter, m_name2);
			}
		}

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

		// …and the live coordinate readout, lower left — the panel's stand-in for the X,Y plot
		// tool's status bar.
		const QString hv = hoverText();
		if (!hv.isEmpty()) {
			p.setPen(QColor(60, 60, 60));
			p.drawText(QRectF(3, height() - 15, width() - 6, 14),
			           Qt::AlignLeft | Qt::AlignVCenter, hv);
		}

		// The marker: a circle on the curve + a tooltip-style "x,  y" box beside it, kept inside the frame.
		QPointF mc;
		if (markerPos(mc)) {
			p.setPen(QPen(QColor(30, 30, 30), 1.5));
			p.setBrush(QColor(255, 255, 255, 200));
			p.drawEllipse(mc, 5.5, 5.5);
			const double mx = std::clamp(m_mark, m_s.front(), m_s.back());
			const QString lab = xyText(mx, curveYAt(mx));
			const QFontMetrics fm(p.font());
			QRectF box(0, 0, fm.horizontalAdvance(lab) + 10, fm.height() + 4);
			box.moveBottomLeft(QPointF(mc.x() + 9, mc.y() - 7));
			if (box.right()  > plot.right())  box.moveRight(mc.x() - 9);
			if (box.top()    < plot.top())    box.moveTop(mc.y() + 7);
			p.setPen(QColor(120, 120, 120));
			p.setBrush(QColor(255, 255, 225));
			p.drawRect(box);
			p.setPen(Qt::black);
			p.drawText(box, Qt::AlignCenter, lab);
		}
	}

	// Right-click -> push the currently shown profile/series into a standalone X,Y plot window
	// (Object Manager + Analysis + save). Works for both the Ctrl-drag elevation profile and a
	// downloaded tide series — whatever this panel currently shows.
	void contextMenuEvent(QContextMenuEvent *e) override {
		QMenu m(this);
		QAction *a = m.addAction("Open in X,Y plot tool");
		a->setEnabled(m_s.size() >= 2);
		QAction *rm = std::isnan(m_mark) ? nullptr : m.addAction("Remove marker");
		QAction *got = m.exec(e->globalPos());
		if (got && got == rm)
			setMark(std::numeric_limits<double>::quiet_NaN());
		else if (got == a && m_s.size() >= 2)
			openSeriesInXYTool(m_s, m_z,
				m_title.isEmpty() ? "Profile" : m_title.toUtf8().constData(),
				m_xlabel.toUtf8().constData(), m_ylabel.toUtf8().constData());
	}
};

static void polyExitEdit(Scene *s);   // leave vertex-edit mode, drop the handles (85_polygon.cpp)

// Show the 3-D track marker at distance `x` along the track (the Profile panel's marker x; NaN =
// none). The track's own samples (profPD points <-> profS distances, pushed together) place it, so
// it sits ON the drawn line. Built lazily, styled like the edit handles (on top of every pile rank).
static void profMarkSync(Scene *s, double x) {
	if (!s || !s->ren) return;
	vtkPoints *lp = s->profPD ? s->profPD->GetPoints() : nullptr;
	const bool show = !std::isnan(x) && lp && s->profS.size() >= 2 &&
	                  lp->GetNumberOfPoints() == (vtkIdType)s->profS.size() &&
	                  s->profLine && s->profLine->GetVisibility();   // a hidden track hides its marker
	if (!show) {
		if (s->profMark && s->profMark->GetVisibility()) {
			s->profMark->SetVisibility(0);
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		}
		return;
	}
	const std::vector<double> &S = s->profS;
	x = std::clamp(x, S.front(), S.back());
	size_t i = size_t(std::upper_bound(S.begin(), S.end(), x) - S.begin());
	i = std::clamp<size_t>(i, 1, S.size() - 1);                 // S[i-1] <= x <= S[i]
	const double ds = S[i] - S[i-1];
	const double t = ds > 0.0 ? (x - S[i-1]) / ds : 0.0;
	double a[3], b[3];
	lp->GetPoint(vtkIdType(i - 1), a);  lp->GetPoint(vtkIdType(i), b);
	const double p[3] = { a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2]) };
	if (!s->profMark) {
		s->profMarkPD = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map; map->SetInputData(s->profMarkPD); map->ScalarVisibilityOff();
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		map->SetRelativeCoincidentTopologyPointOffsetParameter(-200000.0);   // above the track it rides
		s->profMark = vtkSmartPointer<vtkActor>::New();
		s->profMark->SetMapper(map);
		s->profMark->GetProperty()->SetColor(1.0, 1.0, 1.0);
		s->profMark->GetProperty()->SetPointSize(13.0);
		s->profMark->GetProperty()->SetRenderPointsAsSpheres(true);   // a round dot: the panel's circle
		s->profMark->GetProperty()->LightingOff();
		s->profMark->PickableOff();
		if (s->profLine) { double sc[3]; s->profLine->GetScale(sc); s->profMark->SetScale(sc); }
		s->ren->AddActor(s->profMark);
		if (s->globe) globeAttachActor(s, s->profMark, true, true);
		// Whatever hides or shows the TRACK (its Scene Objects row, its group's box) takes the marker
		// with it. Visibility only — no render from inside another actor's Modified.
		if (s->profLine) {
			vtkNew<vtkCallbackCommand> cb;
			cb->SetClientData(s);
			cb->SetCallback([](vtkObject *, unsigned long, void *cd, void *) {
				Scene *sc = static_cast<Scene *>(cd);
				if (!sc->profMark || !sc->profLine) return;
				const bool want = sc->profLine->GetVisibility() && sc->prof && !std::isnan(sc->prof->markerX());
				if ((sc->profMark->GetVisibility() != 0) != want) sc->profMark->SetVisibility(want ? 1 : 0);
			});
			s->profLine->AddObserver(vtkCommand::ModifiedEvent, cb);
		}
	}
	vtkNew<vtkPoints> pts;  pts->SetDataTypeToDouble();
	vtkNew<vtkCellArray> verts;
	const vtkIdType id = pts->InsertNextPoint(p);
	verts->InsertNextCell(1, &id);
	s->profMarkPD->SetPoints(pts);  s->profMarkPD->SetVerts(verts);  s->profMarkPD->Modified();
	s->profMark->SetVisibility(1);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Is display px (dx,dy) on the 3-D marker?
static bool profMarkHit(Scene *s, int dx, int dy) {
	if (!s || !s->ren || !s->profMark || !s->profMark->GetVisibility() || !s->profMarkPD ||
	    !s->profMarkPD->GetPoints() || s->profMarkPD->GetNumberOfPoints() < 1)
		return false;
	double p[3], sc[3];
	s->profMarkPD->GetPoint(0, p);  s->profMark->GetScale(sc);
	s->ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
	s->ren->WorldToDisplay();
	double d[3];  s->ren->GetDisplayPoint(d);
	const double ex = d[0] - dx, ey = d[1] - dy;
	return ex*ex + ey*ey <= 10.0 * 10.0;
}

// Distance along the track of the track point nearest to display px (dx,dy) — what the 3-D marker
// slides to. Same screen-space projection profileHitAt uses.
static bool profTrackDistAt(Scene *s, int dx, int dy, double &dist) {
	vtkPoints *lp = s->profPD ? s->profPD->GetPoints() : nullptr;
	if (!lp || !s->profLine || s->profS.size() < 2 || lp->GetNumberOfPoints() != (vtkIdType)s->profS.size())
		return false;
	double sc[3];  s->profLine->GetScale(sc);
	const vtkIdType np = lp->GetNumberOfPoints();
	std::vector<double> px(np), py(np);
	for (vtkIdType i = 0; i < np; ++i) {
		double p[3];  lp->GetPoint(i, p);
		s->ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
		s->ren->WorldToDisplay();
		double d[3];  s->ren->GetDisplayPoint(d);
		px[i] = d[0];  py[i] = d[1];
	}
	double best = std::numeric_limits<double>::max();
	for (vtkIdType i = 0; i + 1 < np; ++i) {
		const double a[2] = { px[i], py[i] }, b[2] = { px[i+1], py[i+1] };
		const double d2 = segDist2((double)dx, (double)dy, a, b);
		if (d2 < best) {
			best = d2;
			const double vx = b[0] - a[0], vy = b[1] - a[1], L2 = vx*vx + vy*vy;
			const double t = L2 > 0.0 ? std::clamp(((dx - a[0]) * vx + (dy - a[1]) * vy) / L2, 0.0, 1.0) : 0.0;
			dist = s->profS[i] + t * (s->profS[i+1] - s->profS[i]);
		}
	}
	return best < std::numeric_limits<double>::max();
}

// 3-D marker mouse: press on it grabs it, a drag slides it along the track (the panel's marker
// follows through setMarker -> onMarker -> profMarkSync), a double-click removes it from both views.
static bool profMarkPress(Scene *s, int x, int y) {
	if (!s || !s->prof || !profMarkHit(s, x, y)) return false;
	s->profMarkDrag = true;  s->profMarkMoved = false;
	s->profMarkPX = x;  s->profMarkPY = y;
	return true;
}
static bool profMarkMove(Scene *s, int x, int y) {
	if (!s || !s->profMarkDrag) return false;
	if (!s->profMarkMoved && std::abs(x - s->profMarkPX) + std::abs(y - s->profMarkPY) > 3)
		s->profMarkMoved = true;
	double d;
	if (s->profMarkMoved && s->prof && profTrackDistAt(s, x, y, d))
		s->prof->setMarker(d);
	return true;
}
static bool profMarkRelease(Scene *s) {
	if (!s || !s->profMarkDrag) return false;
	s->profMarkDrag = false;
	return true;
}
// Double-click on the 3-D marker removes it and the panel's (setMarker -> onMarker -> profMarkSync).
static bool profMarkDblClick(Scene *s, int x, int y) {
	if (!s || !s->prof || !profMarkHit(s, x, y)) return false;
	s->profMarkDrag = false;
	s->prof->setMarker(std::numeric_limits<double>::quiet_NaN());
	return true;
}

// Wipe the profile: empty the 3D line, drop its texture/state, clear the 2D panel.
static void profileClear(Scene *s) {
	if (!s || !s->profLine) return;
	if (s->profEdit) polyExitEdit(s);   // its end handles go with it
	vtkNew<vtkPolyData> empty;
	if (auto *mm = vtkPolyDataMapper::SafeDownCast(s->profLine->GetMapper()))
		mm->SetInputData(empty);
	s->profLine->SetVisibility(0);
	s->profLine->SetTexture(nullptr);
	s->profPD = nullptr; s->profStripe = nullptr; s->profStyle = 0;
	s->profS.clear(); s->profZ.clear();
	if (s->prof) s->prof->setProfile({}, {});
	setActorTopLayer(s, s->profLine, false);   // it left the pile -> back to the main 3-D layer
	applyStacking(s);                          // and re-rank what is left
	rebuildSceneObjects(s);   // drop the Profile row from the Scene Objects list
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Pick the surface point under cursor device px (dx,dy) and convert to TRUE (x,y) —
// undo the actor's horizontal scale (xfac). Returns false if the cursor misses the surface.
// EVERY VISIBLE data surface is in the pick list, not just the base relief: with a second grid
// displayed over the first, the cursor is over THAT grid, so the track must start/end on it (the
// z sampling already follows the topmost-visible grid through sampleActiveZ — the pick has to agree).
static bool pickSurfaceXY(Scene *s, int dx, int dy, double &tx, double &ty) {
	if (!s || !s->ren || !s->surf)
		return false;
	vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
	pk->PickFromListOn();
	pk->AddPickList(surfProp(s));
	for (auto &ex : s->extras) {
		if (ex.actor && ex.actor->GetVisibility()) pk->AddPickList(ex.actor);
		if (ex.drape && ex.drape->GetVisibility()) pk->AddPickList(ex.drape);
	}
	if (!pk->Pick((double)dx, (double)dy, 0.0, s->ren))
		return false;
	double pp[3]; pk->GetPickPosition(pp);
	// -> true coords through the scene's ONE inverse: `pp[0]/xfac, pp[1]` on any flat map, the sphere
	// inverse on the globe (where the pick position is a point in world XYZ, not a scaled lon/lat).
	double zz;
	sceneWorldToGeo(s, pp, tx, ty, zz);
	return true;
}

// Sample the surface elevation along the straight track (ax,ay)->(bx,by) by shooting a
// vertical ray at each densified (x,y) (one true z per sample), then refresh the 3D drape
// line + the 2D panel. Arc length is in metres (geographic) / data units (cartesian).
// keepStyle: an EDIT of an existing track (end handle dragged) keeps its dashed/dotted look; a
// fresh Ctrl+drag track starts solid.
static void computeProfile(Scene *s, double ax, double ay, double bx, double by, bool keepStyle = false) {
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
	const bool wasVisible = s->profLine->GetVisibility() != 0;
	s->profLine->SetVisibility(np >= 2 ? 1 : 0);
	s->track0[0] = ax; s->track0[1] = ay;              // the track's two ends — what the edit handles move
	s->track1[0] = bx; s->track1[1] = by;
	if (keepStyle && s->profStyle != 0 && np >= 2)     // edited track: re-stipple the NEW geometry
		lineApplyStyle(s, LineRef{ LK_Profile, s->profLine }, s->profStyle);
	else {
		s->profStyle = 0;                              // a fresh line starts solid
		s->profStripe = nullptr;
		s->profLine->SetTexture(nullptr);
		s->profLine->GetProperty()->SetOpacity(1.0);
	}
	s->profS = sv; s->profZ = zv;
	// The track is a vector: give it a rank on the SHARED pile and let applyStacking place it — that is
	// what lifts it above EVERY grid (the law), including a second grid added over the first. Only when
	// it JOINS the pile; re-ranking on every drag event would renumber the whole pile 60 times a second.
	if (np >= 2 && !wasVisible) {
		s->profStack = s->vecSeq++;
		applyStacking(s);
	}

	if (s->prof) {
		s->prof->setProfile(sv, zv, QString::fromStdString(activeGridName(s)));
		profMarkSync(s, s->prof->markerX());       // the 3-D marker rides the re-sampled track
	}
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
	// A POINT CLOUD is no surface to profile: its window refuses the track unless it also holds a grid
	// layer (a grid made from the cloud, a dropped grid), which is then what gets profiled
	if (s->surfCloud && s->gridZ.empty()) {
		bool grid = false;
		for (const auto &ex : s->extras) grid = grid || (!ex.isImage && !ex.isMesh && !ex.gridZ.empty());
		if (!grid)
			return false;
	}
	double tx, ty;
	if (!pickSurfaceXY(s, dx, dy, tx, ty))         // no grid/image under the cursor -> nothing to track
		return false;
	if (s->profEdit) polyExitEdit(s);              // a new track replaces the one whose ends were under edit
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
static bool polyEditCopyToClipboard(Scene *s);                   // Ctrl+C in vertex-edit mode (85)
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

	void devPx(const QPoint &p, double &dx, double &dy) {
		displayPxFromQt(this, renderWindow(), p, dx, dy);
	}
	void recenterAt(const QPoint &p) {
		if (!s || !s->ren) return;
		double x, y; devPx(p, x, y);
		const bool moved = (s->surfCloud && s->cloudPD) ? camRecenterOnCloud(s, x, y)   // a point cloud
		                                                : camRecenterOnPick(s->ren, sceneRecenterTargets(s), x, y);
		if (moved) renderWindow()->Render();
	}
	void panBy(const QPoint &prev, const QPoint &cur) {
		if (!s || !s->ren) return;
		double ox, oy, nx, ny; devPx(prev, ox, oy); devPx(cur, nx, ny);
		camPanByDisplay(s->ren, ox, oy, nx, ny);
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
		// vertex handle in edit mode). Skipped while the draw tool owns a crosshair. Deliberately
		// does NOT cover text labels or batch symbol points (Cities stars etc) — user found the
		// cursor swap over those annoying (2026-07-24); they're still draggable, just silently.
		if (s && !s->polyMode && !s->polyDragWhole) {   // skip hover-cursor while a whole-element drag owns the crosshair
			double dx, dy; devPx(e->position().toPoint(), dx, dy);
			const int *sz = renderWindow()->GetSize();
			const double nx = sz[0] > 0 ? dx / sz[0] : 0.0;
			const double ny = sz[1] > 0 ? dy / sz[1] : 0.0;
			const bool over = colorbarHit(s, nx, ny)
			               || (s->polyEdit >= 0 && polyHitHandle(s, (int)dx, (int)dy, 10.0) >= 0)
			               || mecaHitAt(s, (int)dx, (int)dy) >= 0
			               || s->profMarkDrag || profMarkHit(s, (int)dx, (int)dy)
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
	// Ctrl+C while ANY vector element is in vertex-edit mode (handles showing: a drawn
	// polygon/polyline/rect/circle/fault via Scene::polyEdit, or an overlay line/contour edited in
	// place via Scene::ovEdit) copies EVERY vertex — polyEditCopyToClipboard (85_polygon.cpp), the
	// one implementation, reading through the same EditVerts the handles and the drag use.
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
		if (s && e->matches(QKeySequence::Copy) && polyEditCopyToClipboard(s))
			return;
		QVTKOpenGLNativeWidget::keyPressEvent(e);
	}
};

// Build a window around a prepared surface and SHOW it (non-blocking). xfac/zfac
// are the base actor scales (geographic aspect + true-scale unit conversion); ve0
// is the initial vertical exaggeration. Cube-axis labels are pinned to the true
// (x0,x1,y0,y1,zmin,zmax) ranges so they stay correct under the scaling.
