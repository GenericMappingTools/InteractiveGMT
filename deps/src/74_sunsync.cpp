// 74_sunsync.cpp -- T & L > "Sun-synchronous orbit".
//
// A teaching demo, the companion of "Earth orbit & day length" (74_daylength.cpp), whose Sun, orbit,
// globe and view it uses as they are. The Earth goes round the Sun; round the Earth, a satellite's
// orbital PLANE. Left alone, that plane would stay fixed among the stars, so in three months the Sun
// would have swung a quarter turn round it: a morning orbit becomes a midnight one. The Earth's
// equatorial bulge (J2) makes the plane turn about the Earth's axis instead -- the nodal precession --
// and at one inclination for each altitude it turns exactly as fast as the Sun moves along the
// ecliptic, 360 deg a year. That is a SUN-SYNCHRONOUS orbit: it crosses the equator at the same local
// solar time every day of the year.
//
//   * the ORBIT VIEW (VTK): Sun, Earth's orbit, the Earth with its fixed axis, and the satellite's orbit
//     drawn round it, cut into its sunlit (yellow) and shadowed (blue) arcs. Optionally the same orbit
//     WITHOUT precession (grey), so the difference is seen, not told;
//   * an INSET looking at the Earth FROM THE SUN: a dawn-dusk orbit is a ring round the sunlit disc,
//     a noon-midnight orbit is seen edge-on, a line across it;
//   * a CHART of the year: the local time of the ascending node (flat for a sun-synchronous orbit)
//     and the share of each revolution spent in the Earth's shadow.
//
// THE MATHS (one place, below): circular orbit, semi-major axis a = Re + h, mean motion n = sqrt(mu/a^3).
// The secular J2 drift of the node is dOmega/dt = -3/2 n J2 (Re/a)^2 cos i, so sun-synchronism,
// dOmega/dt = 360 deg / 365.2422 d, gives cos i = -(2/3) (dOmega/dt) / (n J2 (Re/a)^2) -- about 98 deg
// at 700 km, retrograde, as every real one is. The node's local time is LTAN = 12 h + (Omega - alpha_m)
// / 15, alpha_m the mean Sun's right ascension (mean solar time: a perfect sun-synchronous orbit gives a
// flat line, the equation of time is not mixed in). Shadow is the cylindrical one: the Earth's shadow is
// a cylinder of radius Re behind it; a circular orbit with beta = the Sun's angle above its plane spends
// f = acos(sqrt(h^2 + 2 Re h) / (a cos beta)) / pi of each revolution in it, and none when
// |beta| >= asin(Re/a).
//
// FRAMES: the scene is 74_daylength.cpp's: ecliptic XY, ecliptic north +Z, the Sun at the origin, the
// vernal equinox +X, the Earth's axis tilted towards +Y. The equatorial frame is then Xeq = X,
// Zeq = (0, sin e, cos e), Yeq = Zeq x Xeq.
//
// SCALE: as in the Earth-orbit demo the Earth is 1 unit on an orbit of 14. The satellite's ALTITUDE is
// drawn 3x (700 km would hug the globe at 1.11 radii); every number -- period, shadow, rates -- is
// computed with the true altitude.

static const double kSsMu   = 398600.4418;     // km^3/s^2
static const double kSsRe   = 6378.137;        // km
static const double kSsJ2   = 1.08262668e-3;
static const double kSsSunRate = 360.0 / 365.2422;   // deg/day: what the node has to turn by to keep up
static const double kSsExag = 3.0;             // drawn altitude exaggeration
static const double kSsSunR = 1.25 * kDlSunR / 3.0;   // this demo's Sun: 1.25 x a third of the Earth-orbit demo's

// Mean motion (rad/s) and period (min) of a circular orbit `h` km up.
static double ssMeanMotion(double h) {
	const double a = kSsRe + h;
	return std::sqrt(kSsMu / (a * a * a));
}
static double ssPeriodMin(double h) { return 2.0 * vtkMath::Pi() / ssMeanMotion(h) / 60.0; }

// The node's J2 drift, deg/day (positive = eastward, with the Sun).
static double ssNodeRate(double h, double incl) {
	const double q = kSsRe / (kSsRe + h);
	const double w = -1.5 * ssMeanMotion(h) * kSsJ2 * q * q * std::cos(vtkMath::RadiansFromDegrees(incl));
	return vtkMath::DegreesFromRadians(w) * 86400.0;
}

// The inclination (deg) that makes an orbit `h` km up sun-synchronous; NaN where none exists (too high).
static double ssSunSyncIncl(double h) {
	const double q = kSsRe / (kSsRe + h);
	const double c = -(2.0 / 3.0) * vtkMath::RadiansFromDegrees(kSsSunRate) / 86400.0 /
	                 (ssMeanMotion(h) * kSsJ2 * q * q);
	if (c < -1.0) return std::nan("");
	return vtkMath::DegreesFromRadians(std::acos(c));
}

// The mean Sun's right ascension (deg), day `doy` of 2026 (same J2000 day count as dlSun).
static double ssMeanSunRA(int doy) {
	const double n = 9497.0 + (doy - 1);
	return std::fmod(280.460 + 0.9856474 * n, 360.0);
}

// The equatorial frame in scene coordinates, for the Earth's tilt.
static void ssEqFrame(double X[3], double Y[3], double Z[3]) {
	const double t = vtkMath::RadiansFromDegrees(kDlEarthTilt);
	X[0] = 1; X[1] = 0; X[2] = 0;
	Z[0] = 0; Z[1] = std::sin(t); Z[2] = std::cos(t);
	vtkMath::Cross(Z, X, Y);
}

// The orbit's node direction N and in-plane 90-deg-ahead direction M (scene frame, unit) for node
// right ascension `raan` and inclination `incl`, both deg.
static void ssPlane(double raan, double incl, double N[3], double M[3]) {
	double X[3], Y[3], Z[3];
	ssEqFrame(X, Y, Z);
	const double O = vtkMath::RadiansFromDegrees(raan), i = vtkMath::RadiansFromDegrees(incl);
	const double n[3] = {std::cos(O), std::sin(O), 0.0};                                    // equatorial
	const double m[3] = {-std::sin(O) * std::cos(i), std::cos(O) * std::cos(i), std::sin(i)};
	for (int k = 0; k < 3; ++k) {
		N[k] = n[0] * X[k] + n[1] * Y[k] + n[2] * Z[k];
		M[k] = m[0] * X[k] + m[1] * Y[k] + m[2] * Z[k];
	}
}

// The Sun's angle above the orbit plane (deg), and the share of a revolution in the Earth's shadow.
static double ssBeta(const double N[3], const double M[3], const double sunDir[3]) {
	double H[3];
	vtkMath::Cross(N, M, H);
	return vtkMath::DegreesFromRadians(std::asin(std::clamp(vtkMath::Dot(H, sunDir), -1.0, 1.0)));
}
static double ssShadowFraction(double h, double beta) {
	const double a = kSsRe + h;
	const double cb = std::cos(vtkMath::RadiansFromDegrees(beta));
	const double c = std::sqrt(h * h + 2.0 * kSsRe * h) / (a * cb);
	if (c >= 1.0) return 0.0;
	return std::acos(c) / vtkMath::Pi();
}

// Is the orbit point at unit direction `u` (true radius a = Re + h) in the Earth's cylindrical shadow?
static bool ssInShadow(const double u[3], double h, const double sunDir[3]) {
	const double a = (kSsRe + h) / kSsRe;   // in Earth radii
	const double along = a * vtkMath::Dot(u, sunDir);
	if (along >= 0.0) return false;
	return a * a - along * along < 1.0;
}

static QString ssHhMm(double hours) {
	hours = std::fmod(hours, 24.0);
	if (hours < 0) hours += 24.0;
	int m = int(std::lround(hours * 60.0)) % 1440;
	return QString("%1:%2").arg(m / 60, 2, 10, QChar('0')).arg(m % 60, 2, 10, QChar('0'));
}

// The year chart: two panels -- the node's local time, and the time in shadow per revolution.
class SunSyncChart : public QWidget {
public:
	int doy = 172;
	std::vector<double> ltan, ltanFixed, shadow;   // per day: this orbit, the same without precession, shadow share
	bool showFixed = true;
	double periodMin = 98.8;
	std::function<void(int)> onDay;

	SunSyncChart(QWidget *parent) : QWidget(parent) {
		setMinimumHeight(190);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setMouseTracking(true);
	}
	QSize sizeHint() const override { return QSize(900, 210); }

protected:
	bool dragging = false;
	QRect plotRect(int k) const {   // k = 0 left (LTAN), 1 right (shadow)
		const int w = (width() - 140) / 2;
		return QRect(k == 0 ? 52 : 52 + w + 70, 30, w, height() - 54);
	}
	int dayAtX(double x, int k) const {
		const QRect plot = plotRect(k);
		const int nd = int(ltan.size());
		const int d = int(std::lround(1 + (x - plot.left()) * (nd - 1) / double(plot.width())));
		return std::clamp(d, 1, nd);
	}
	int panelAt(const QPointF &pt) const {
		for (int k = 0; k < 2; ++k)
			if (plotRect(k).adjusted(-10, -10, 10, 10).contains(pt.toPoint())) return k;
		return -1;
	}
	int dragPanel = 0;
	void mousePressEvent(QMouseEvent *e) override {
		const int k = panelAt(e->position());
		if (e->button() == Qt::LeftButton && k >= 0 && !ltan.empty()) {
			dragging = true;
			dragPanel = k;
			const int d = dayAtX(e->position().x(), k);
			if (d != doy && onDay) onDay(d);
			e->accept();
			return;
		}
		QWidget::mousePressEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (dragging) {
			const int d = dayAtX(e->position().x(), dragPanel);
			if (d != doy && onDay) onDay(d);
			return;
		}
		setCursor(panelAt(e->position()) >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) dragging = false;
		QWidget::mouseReleaseEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), Qt::white);
		if (ltan.empty()) return;
		const int nd = int(ltan.size());
		QFont f = font();
		static const int mdoy[12] = {1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
		static const char *mtxt[12] = {"J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D"};
		const double shMax = 50.0;   // % of a revolution: the most a circular orbit can spend in shadow is < 50 %
		for (int k = 0; k < 2; ++k) {
			const QRect plot = plotRect(k);
			const double ymax = k == 0 ? 24.0 : shMax;
			auto px = [&](double d) { return plot.left() + plot.width() * (d - 1) / double(nd - 1); };
			auto py = [&](double v) { return plot.bottom() - plot.height() * v / ymax; };
			f.setPointSizeF(8.5);
			f.setBold(false);
			p.setFont(f);
			p.setPen(QPen(QColor(215, 215, 215), 1));
			const double ystep = k == 0 ? 6.0 : 10.0;
			for (double v = 0; v <= ymax + 1e-9; v += ystep) p.drawLine(QPointF(plot.left(), py(v)), QPointF(plot.right(), py(v)));
			for (int m = 0; m < 12; ++m) p.drawLine(QPointF(px(mdoy[m]), plot.top()), QPointF(px(mdoy[m]), plot.bottom()));
			p.setPen(Qt::black);
			for (int m = 0; m < 12; ++m)
				p.drawText(QRectF(px(mdoy[m]), plot.bottom() + 2, px(m < 11 ? mdoy[m + 1] : nd) - px(mdoy[m]), 14),
				           Qt::AlignCenter, mtxt[m]);
			for (double v = 0; v <= ymax + 1e-9; v += ystep)
				p.drawText(QRectF(plot.left() - 46, py(v) - 7, 40, 14), Qt::AlignRight | Qt::AlignVCenter,
				           k == 0 ? QString("%1 h").arg(int(v)) : QString("%1 %").arg(int(v)));
			// A curve that wraps 24 h -> 0 h is broken there, never drawn across the panel.
			auto curve = [&](const std::vector<double> &v, const QPen &pen) {
				p.setPen(pen);
				QPolygonF poly;
				for (int d = 1; d <= nd; ++d) {
					if (d > 1 && std::fabs(v[d - 1] - v[d - 2]) > ymax / 2) {
						p.drawPolyline(poly);
						poly.clear();
					}
					poly << QPointF(px(d), py(v[d - 1]));
				}
				p.drawPolyline(poly);
			};
			std::vector<double> sh;
			if (k == 0) {
				if (showFixed) curve(ltanFixed, QPen(QColor(140, 140, 140), 1.6, Qt::DashLine));
				curve(ltan, QPen(QColor(220, 120, 0), 2.2));
			}
			else {
				sh.resize(nd);
				for (int d = 0; d < nd; ++d) sh[d] = 100.0 * shadow[d];
				curve(sh, QPen(QColor(40, 70, 200), 2.2));
			}
			p.setPen(QPen(Qt::black, 1));
			p.setBrush(Qt::NoBrush);
			p.drawRect(plot);
			p.setPen(QPen(QColor(200, 0, 0), 1.5));
			p.drawLine(QPointF(px(doy), plot.top()), QPointF(px(doy), plot.bottom()));
			p.setBrush(QColor(255, 205, 40));
			p.setPen(QPen(Qt::black, 1));
			p.drawEllipse(QPointF(px(doy), py(k == 0 ? ltan[doy - 1] : sh[doy - 1])), 5, 5);
			f.setBold(true);
			f.setPointSizeF(9.0);
			p.setFont(f);
			const QString title = k == 0
				? QString("Local time of the ascending node:  %1").arg(ssHhMm(ltan[doy - 1]))
				: QString("In Earth's shadow:  %1 %  =  %2 min of each %3 min revolution")
				      .arg(100.0 * shadow[doy - 1], 0, 'f', 1).arg(shadow[doy - 1] * periodMin, 0, 'f', 1)
				      .arg(periodMin, 0, 'f', 1);
			p.drawText(QRect(plot.left(), 6, plot.width() + 60, 18), Qt::AlignLeft, title);
		}
	}
};

struct SunSyncDialog {
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	DayLengthView *view = nullptr;
	SunSyncChart *chart = nullptr;
	QLabel *dateLbl = nullptr, *facts = nullptr;
	QScrollBar *slider = nullptr;
	QPushButton *play = nullptr;
	QDoubleSpinBox *alt = nullptr, *incl = nullptr, *ltan0 = nullptr;
	QCheckBox *fixedBox = nullptr, *followBox = nullptr, *planeBox = nullptr, *orthoBox = nullptr;
	QSpinBox *step = nullptr;
	QTimer *timer = nullptr;
	bool parked = false;
	bool reallyClose = false;

	vtkSmartPointer<vtkRenderer> ren, inset, labRen;
	vtkSmartPointer<vtkActor> earth, axis, night, term, ring, ringFixed, node, disc, normalArrow, sunArrow;
	vtkSmartPointer<vtkMatrix4x4> normalM, sunArrowM;
	vtkSmartPointer<vtkPolyData> ringPD, ringFixedPD, discPD;
	vtkSmartPointer<vtkSphereSource> nodeSrc;
	vtkSmartPointer<vtkMatrix4x4> earthM, nightM, atEarthM;   // atEarthM: a plain translation to the Earth
	double sunDir[3] = {1, 0, 0};
	double earthPos[3] = {0, 0, 0};
	double raan = 0.0, beta = 0.0;           // the current day's node right ascension and Sun angle (deg)

	void unpark() {
		if (!dlg) return;
		parked = false;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel = m.addAction("Delete");
			QAction *pick = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();
			}
		};
	}

	bool parkNow() {
		stopPlay();
		if (reallyClose || !dlg || !scn || !sceneAlive(scn)) return false;
		if (parked) {
			dlg->hide();
			return true;
		}
		parked = true;
		dlg->hide();
		parkTool(scn, dlg, dlg->windowTitle(), IC_Image,
		         "Closed " + dlg->windowTitle() + " — double-click to bring it back, click for Show / Delete",
		         [this]() { unpark(); }, parkedMenu());
		return true;
	}

	double h() const { return alt->value(); }

	// The inset: a square in the top-right corner, just big enough for the Earth and the drawn orbit,
	// its camera's angle fitted to that orbit so nothing is wasted round it.
	void placeInset() {
		const double W = std::max(1, view->width()), H = std::max(1, view->height());
		const double S = std::min(240.0, 0.32 * std::min(W, H)), mg = 4.0;
		inset->SetViewport(1.0 - (S + mg) / W, 1.0 - (S + mg) / H, 1.0 - mg / W, 1.0 - mg / H);
		const double r = 1.0 + kSsExag * h() / kSsRe;
		inset->GetActiveCamera()->SetViewAngle(2.0 * vtkMath::DegreesFromRadians(std::atan(1.08 * r / 9.0)));
		inset->GetActiveCamera()->SetParallelScale(1.08 * r);   // same framing when orthographic
	}

	// Orthographic or perspective, both cameras. Switching keeps the picture's size: the parallel
	// scale (half the view's height) is what the perspective cone spans at the focal point.
	void setOrtho(bool on) {
		vtkCamera *c = ren->GetActiveCamera();
		if (on) c->SetParallelScale(c->GetDistance() * std::tan(vtkMath::RadiansFromDegrees(c->GetViewAngle() / 2.0)));
		c->SetParallelProjection(on);
		inset->GetActiveCamera()->SetParallelProjection(on);
		ren->ResetCameraClippingRange();
		inset->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// Earth on screen, or the orbit's plane alone (the "Orbit plane only" box).
	void showPlaneOnly(bool on) {
		for (vtkActor *a : {earth.GetPointer(), night.GetPointer(), term.GetPointer(), axis.GetPointer()})
			a->SetVisibility(!on);
		disc->SetVisibility(on);
		normalArrow->SetVisibility(on);
		sunArrow->SetVisibility(on);
	}

	// The orbit's plane as a filled disc out to the drawn orbit, cut along the TERMINATOR (the plane's
	// line through the Earth's centre perpendicular to the Sun): its day half (yellow, facing the Sun)
	// and night half (blue). Two exact half-disc fans, so the cut is a straight chord. When the plane
	// faces the Sun square on (|beta| ~ 90, the terminator plane itself) all of it is day.
	void buildDisc(const double N[3], const double M[3]) {
		const double r = 1.0 + kSsExag * h() / kSsRe;
		// The Sun's direction within the plane, as an angle from the node.
		const double sn = vtkMath::Dot(sunDir, N), sm = vtkMath::Dot(sunDir, M);
		const bool allDay = std::hypot(sn, sm) < 1e-3;
		const double phiS = std::atan2(sm, sn);
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> tris;
		vtkNew<vtkUnsignedCharArray> rgb;
		rgb->SetNumberOfComponents(3);
		const unsigned char lit[3] = {255, 205, 40}, dark[3] = {40, 90, 230};
		const vtkIdType c = pts->InsertNextPoint(0.0, 0.0, 0.0);
		auto fan = [&](double u0, const unsigned char *col) {   // a half disc from angle u0 to u0 + pi
			const int n = 90;
			vtkIdType prev = -1;
			for (int i = 0; i <= n; ++i) {
				const double u = u0 + vtkMath::Pi() * i / n;
				const vtkIdType id = pts->InsertNextPoint(r * (std::cos(u) * N[0] + std::sin(u) * M[0]),
				                                          r * (std::cos(u) * N[1] + std::sin(u) * M[1]),
				                                          r * (std::cos(u) * N[2] + std::sin(u) * M[2]));
				if (prev >= 0) {
					const vtkIdType ids[3] = {c, prev, id};
					tris->InsertNextCell(3, ids);
					rgb->InsertNextTypedTuple(col);
				}
				prev = id;
			}
		};
		const double half = vtkMath::Pi() / 2.0;
		fan(phiS - half, lit);
		fan(phiS + half, allDay ? lit : dark);
		discPD->SetPoints(pts);
		discPD->SetPolys(tris);
		discPD->GetCellData()->SetScalars(rgb);
		discPD->Modified();
	}

	// An arrow from the Earth's centre along unit `dir`, `len` scene units long.
	void aimArrow(vtkMatrix4x4 *A, const double dir[3], double len) {
		double x[3] = {dir[0], dir[1], dir[2]}, y[3], z[3];
		const double ref[3] = {std::fabs(x[2]) < 0.9 ? 0.0 : 1.0, 0.0, std::fabs(x[2]) < 0.9 ? 1.0 : 0.0};
		vtkMath::Cross(ref, x, y);
		vtkMath::Normalize(y);
		vtkMath::Cross(x, y, z);
		for (int i = 0; i < 3; ++i) {
			A->SetElement(i, 0, len * x[i]);
			A->SetElement(i, 1, len * y[i]);
			A->SetElement(i, 2, len * z[i]);
			A->SetElement(i, 3, earthPos[i]);
		}
		A->Modified();
	}

	// The node's right ascension on day `doy`: set by the LTAN box on 1 January, then drifting at the
	// J2 rate (`rate`, deg/day).
	double raanAt(int doy, double rate) const {
		const double O0 = ssMeanSunRA(1) + 15.0 * (ltan0->value() - 12.0);
		return O0 + rate * (doy - 1);
	}
	static double ltanOf(double raan, int doy) {
		double l = std::fmod(12.0 + (raan - ssMeanSunRA(doy)) / 15.0, 24.0);
		return l < 0 ? l + 24.0 : l;
	}

	// The Sun's direction from the Earth on day `doy` (unit, scene frame), and the Earth's position.
	static void sunAt(int doy, double s[3], double pos[3]) {
		double lambda, rAU;
		dlSun(doy, lambda, rAU);
		const double le = vtkMath::RadiansFromDegrees(lambda + 180.0);
		pos[0] = kDlOrbitR * rAU * std::cos(le);
		pos[1] = kDlOrbitR * rAU * std::sin(le);
		pos[2] = 0.0;
		const double r = std::hypot(pos[0], pos[1]);
		s[0] = -pos[0] / r;
		s[1] = -pos[1] / r;
		s[2] = 0.0;
	}

	// One orbit ring, drawn at the exaggerated radius, each segment coloured by the TRUE geometry:
	// sunlit (yellow) or in the Earth's shadow (blue); `grey` draws it in one colour.
	void buildRing(vtkPolyData *pd, double raanDeg, bool grey) {
		double N[3], M[3];
		ssPlane(raanDeg, incl->value(), N, M);
		const double r = 1.0 + kSsExag * h() / kSsRe;
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> segs;
		vtkNew<vtkUnsignedCharArray> rgb;
		rgb->SetNumberOfComponents(3);
		const int n = 360;
		for (int i = 0; i <= n; ++i) {
			const double u = 2.0 * vtkMath::Pi() * i / n;
			pts->InsertNextPoint(r * (std::cos(u) * N[0] + std::sin(u) * M[0]),
			                     r * (std::cos(u) * N[1] + std::sin(u) * M[1]),
			                     r * (std::cos(u) * N[2] + std::sin(u) * M[2]));
		}
		const unsigned char lit[3] = {255, 205, 40}, dark[3] = {40, 90, 230}, gr[3] = {150, 150, 150};
		for (int i = 0; i < n; ++i) {
			const double u = 2.0 * vtkMath::Pi() * (i + 0.5) / n;
			double d[3];
			for (int k = 0; k < 3; ++k) d[k] = std::cos(u) * N[k] + std::sin(u) * M[k];
			const vtkIdType ids[2] = {i, i + 1};
			segs->InsertNextCell(2, ids);
			rgb->InsertNextTypedTuple(grey ? gr : (ssInShadow(d, h(), sunDir) ? dark : lit));
		}
		pd->SetPoints(pts);
		pd->SetLines(segs);
		pd->GetCellData()->SetScalars(rgb);
		pd->Modified();
		if (!grey) {
			nodeSrc->SetCenter(r * N[0], r * N[1], r * N[2]);
			nodeSrc->Modified();
			buildDisc(N, M);
		}
	}

	// Put the year at day `doy`.
	void setDay(int doy) {
		sunAt(doy, sunDir, earthPos);
		// The Earth's body, as in the Earth-orbit demo: axis fixed, Greenwich towards the noon Sun.
		double e3[3], e1[3], e2[3], X[3], Y[3];
		ssEqFrame(X, Y, e3);
		const double k = vtkMath::Dot(sunDir, e3);
		for (int i = 0; i < 3; ++i) e1[i] = sunDir[i] - k * e3[i];
		vtkMath::Normalize(e1);
		vtkMath::Cross(e3, e1, e2);
		for (int i = 0; i < 3; ++i) {
			earthM->SetElement(i, 0, e1[i]);
			earthM->SetElement(i, 1, e2[i]);
			earthM->SetElement(i, 2, e3[i]);
			earthM->SetElement(i, 3, earthPos[i]);
			atEarthM->SetElement(i, 3, earthPos[i]);
		}
		earthM->Modified();
		atEarthM->Modified();
		{
			const double up[3] = {0, 0, 1};
			double nx[3], ny[3];
			vtkMath::Cross(up, sunDir, nx);
			vtkMath::Normalize(nx);
			vtkMath::Cross(sunDir, nx, ny);
			for (int i = 0; i < 3; ++i) {
				nightM->SetElement(i, 0, nx[i]);
				nightM->SetElement(i, 1, ny[i]);
				nightM->SetElement(i, 2, sunDir[i]);
				nightM->SetElement(i, 3, earthPos[i]);
			}
			nightM->Modified();
		}
		const double rate = ssNodeRate(h(), incl->value());
		raan = raanAt(doy, rate);
		double N[3], M[3];
		ssPlane(raan, incl->value(), N, M);
		beta = ssBeta(N, M, sunDir);
		// The two arrows of "Orbit plane only": the orbit's normal and the Sun's direction. Seen from
		// ecliptic north, the angle between them stays put through the year for a sun-synchronous orbit.
		{
			double H[3];
			vtkMath::Cross(N, M, H);
			const double len = 1.6 * (1.0 + kSsExag * h() / kSsRe);
			aimArrow(normalM, H, len);
			aimArrow(sunArrowM, sunDir, len);
		}
		buildRing(ringPD, raan, false);
		buildRing(ringFixedPD, raanAt(doy, 0.0), true);
		// Inset: the Earth seen FROM THE SUN, ecliptic north up.
		vtkCamera *cam = inset->GetActiveCamera();
		cam->SetFocalPoint(earthPos);
		cam->SetPosition(earthPos[0] + 9.0 * sunDir[0], earthPos[1] + 9.0 * sunDir[1], earthPos[2] + 9.0 * sunDir[2]);
		cam->SetViewUp(0, 0, 1);
		placeInset();
		inset->ResetCameraClippingRange();
		if (followBox->isChecked()) {
			vtkCamera *c = ren->GetActiveCamera();
			double fp[3], pos[3];
			c->GetFocalPoint(fp);
			c->GetPosition(pos);
			for (int i = 0; i < 3; ++i) pos[i] += earthPos[i] - fp[i];
			c->SetFocalPoint(earthPos);
			c->SetPosition(pos);
		}
		chart->doy = doy;
		chart->update();
		dateLbl->setText(QString("%1   β = %2°").arg(dlDateOf(doy)).arg(beta, 0, 'f', 1));
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// A new orbit (altitude, inclination or starting node time): the year's curves, the facts, the picture.
	void reorbit() {
		const double rate = ssNodeRate(h(), incl->value());
		chart->ltan.resize(365);
		chart->ltanFixed.resize(365);
		chart->shadow.resize(365);
		for (int d = 1; d <= 365; ++d) {
			double s[3], pos[3], N[3], M[3];
			sunAt(d, s, pos);
			const double O = raanAt(d, rate);
			chart->ltan[d - 1] = ltanOf(O, d);
			chart->ltanFixed[d - 1] = ltanOf(raanAt(d, 0.0), d);
			ssPlane(O, incl->value(), N, M);
			chart->shadow[d - 1] = ssShadowFraction(h(), ssBeta(N, M, s));
		}
		chart->periodMin = ssPeriodMin(h());
		const double iss = ssSunSyncIncl(h());
		const bool isSso = std::isfinite(iss) && std::fabs(rate - kSsSunRate) < 0.005;
		facts->setText(QString(
			"<b>Orbit</b>: %1 km up, period %2 min, %3 revolutions a day<br>"
			"<b>Node drift (J2)</b>: %4°/day<br>"
			"<b>Sun's drift</b>: %5°/day (360° a year)<br>"
			"<b>Sun-synchronous at this height</b>: i = %6<br>"
			"%7")
			.arg(h(), 0, 'f', 0).arg(ssPeriodMin(h()), 0, 'f', 1).arg(1440.0 / ssPeriodMin(h()), 0, 'f', 2)
			.arg(rate, 0, 'f', 4).arg(kSsSunRate, 0, 'f', 4)
			.arg(std::isfinite(iss) ? QString("%1°").arg(iss, 0, 'f', 2) : QString("none (too high)"))
			.arg(isSso ? QString("<span style='color:#1a8a1a'><b>This orbit IS sun-synchronous</b></span>")
			           : QString("<span style='color:#c03000'><b>Not sun-synchronous</b>: node gains %1 min of "
			                     "local time a day</span>").arg((rate - kSsSunRate) * 4.0, 0, 'f', 2)));
		setDay(slider->value());
	}

	void stopPlay() {
		if (timer && timer->isActive()) timer->stop();
		if (play) play->setText("Play");
	}

	void tick() {
		int d = slider->value() + step->value();
		if (d > slider->maximum()) d -= slider->maximum();
		slider->setValue(d);
	}
};

static SunSyncDialog *g_sunSync = nullptr;

// Open (or bring back) the demo.
static bool sunSyncOpen(QWidget *parent, Scene *scene) {
	if (g_sunSync) {
		g_sunSync->unpark();
		g_sunSync->scn = scene;
		return true;
	}
	auto *m = new SunSyncDialog;
	m->scn = scene;
	QDialog *dlg = new QDialog(parent);
	m->dlg = dlg;
	dlg->setWindowTitle("Sun-synchronous orbit");
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint |
	                    Qt::WindowMaximizeButtonHint);

	// ---- the view (renderers and lights exactly as the Earth-orbit demo's) ------------------------
	m->view = new DayLengthView(dlg);
	m->view->setMinimumSize(600, 380);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->view->setRenderWindow(rw);
	m->ren = vtkSmartPointer<vtkRenderer>::New();
	m->ren->SetBackground(0.02, 0.02, 0.06);
	m->ren->AutomaticLightCreationOff();
	rw->AddRenderer(m->ren);
	m->inset = vtkSmartPointer<vtkRenderer>::New();
	m->inset->SetBackground(0.22, 0.24, 0.32);
	m->inset->AutomaticLightCreationOff();
	m->inset->InteractiveOff();
	m->inset->SetLayer(2);
	m->inset->SetPreserveColorBuffer(false);
	m->inset->SetPreserveDepthBuffer(false);
	m->labRen = vtkSmartPointer<vtkRenderer>::New();
	m->labRen->SetLayer(1);
	m->labRen->InteractiveOff();
	m->labRen->SetActiveCamera(m->ren->GetActiveCamera());
	rw->SetNumberOfLayers(3);
	rw->AddRenderer(m->labRen);
	rw->AddRenderer(m->inset);
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	m->view->interactor()->SetInteractorStyle(style);
	for (vtkRenderer *r : {m->ren.GetPointer(), m->inset.GetPointer()}) {
		vtkNew<vtkLight> l;
		l->SetLightTypeToSceneLight();
		l->SetPositional(true);
		l->SetPosition(0, 0, 0);
		l->SetFocalPoint(1, 0, 0);
		l->SetConeAngle(180.0);
		l->SetIntensity(1.0);
		r->AddLight(l);
	}

	// The Sun and the Earth's orbit.
	{
		vtkNew<vtkSphereSource> sunSrc;
		sunSrc->SetRadius(kSsSunR);
		sunSrc->SetThetaResolution(48);
		sunSrc->SetPhiResolution(32);
		vtkNew<vtkPolyDataMapper> sunMap;
		sunMap->SetInputConnection(sunSrc->GetOutputPort());
		vtkNew<vtkActor> sun;
		sun->SetMapper(sunMap);
		sun->GetProperty()->SetColor(1.0, 0.82, 0.25);
		sun->GetProperty()->SetLighting(false);
		m->ren->AddActor(sun);
		vtkNew<vtkPoints> pts;
		std::vector<vtkIdType> ids;
		for (int i = 0; i <= 720; ++i) {
			double p[3];
			dlOrbitAt(0.5 * i, kDlOrbitR, p);
			ids.push_back(pts->InsertNextPoint(p));
		}
		vtkNew<vtkCellArray> line;
		line->InsertNextCell(vtkIdType(ids.size()), ids.data());
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetLines(line);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(pd);
		vtkNew<vtkActor> orbit;
		orbit->SetMapper(map);
		orbit->GetProperty()->SetColor(0.55, 0.55, 0.65);
		orbit->GetProperty()->SetLighting(false);
		m->ren->AddActor(orbit);
		const char *names[4] = {"March equinox", "June solstice", "September equinox", "December solstice"};
		for (int k = 0; k < 4; ++k) {
			const double le = vtkMath::RadiansFromDegrees(90.0 * k + 180.0);
			const double pos[3] = {(kDlOrbitR + 2.4) * std::cos(le), (kDlOrbitR + 2.4) * std::sin(le), 0.0};
			m->labRen->AddActor(dlLabel(names[k], pos, 13));
		}
		const double sunTop[3] = {0, 0, kSsSunR + 1.0};
		m->labRen->AddActor(dlLabel("Sun", sunTop, 14));
	}

	// The Earth (the magnetic-field demo's textured globe), its night half, terminator and axis.
	m->earthM = vtkSmartPointer<vtkMatrix4x4>::New();
	m->nightM = vtkSmartPointer<vtkMatrix4x4>::New();
	m->atEarthM = vtkSmartPointer<vtkMatrix4x4>::New();
	{
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(magFieldSphere(180, 90));
		map->ScalarVisibilityOff();
		m->earth = vtkSmartPointer<vtkActor>::New();
		m->earth->SetMapper(map);
		m->earth->SetUserMatrix(m->earthM);
		m->earth->GetProperty()->SetColor(0.35, 0.55, 0.85);
		m->earth->GetProperty()->SetLighting(false);
	}
	{
		vtkNew<vtkSphereSource> src;
		src->SetRadius(1.006);
		src->SetStartPhi(90.0);
		src->SetEndPhi(180.0);
		src->SetThetaResolution(96);
		src->SetPhiResolution(48);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		m->night = vtkSmartPointer<vtkActor>::New();
		m->night->SetMapper(map);
		m->night->SetUserMatrix(m->nightM);
		m->night->GetProperty()->SetColor(0.0, 0.0, 0.12);
		m->night->GetProperty()->SetOpacity(0.55);
		m->night->GetProperty()->SetLighting(false);
	}
	{
		vtkNew<vtkPoints> pts;
		std::vector<vtkIdType> ids;
		for (int i = 0; i <= 180; ++i) {
			const double l = 2.0 * vtkMath::Pi() * i / 180.0;
			ids.push_back(pts->InsertNextPoint(1.008 * std::cos(l), 1.008 * std::sin(l), 0.0));
		}
		vtkNew<vtkCellArray> line;
		line->InsertNextCell(vtkIdType(ids.size()), ids.data());
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetLines(line);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(pd);
		m->term = vtkSmartPointer<vtkActor>::New();
		m->term->SetMapper(map);
		m->term->SetUserMatrix(m->nightM);
		m->term->GetProperty()->SetColor(1.0, 0.55, 0.1);
		m->term->GetProperty()->SetLineWidth(2.0);
		m->term->GetProperty()->SetLighting(false);
	}
	{
		vtkNew<vtkLineSource> src;
		src->SetPoint1(0, 0, -1.9);
		src->SetPoint2(0, 0, 1.9);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		m->axis = vtkSmartPointer<vtkActor>::New();
		m->axis->SetMapper(map);
		m->axis->SetUserMatrix(m->earthM);
		m->axis->GetProperty()->SetColor(1.0, 1.0, 1.0);
		m->axis->GetProperty()->SetLineWidth(2.5);
		m->axis->GetProperty()->SetLighting(false);
	}
	// The satellite's orbit (lit / shadowed), the same orbit without precession (grey), the ascending node.
	auto ringActor = [m](vtkSmartPointer<vtkPolyData> &pd, double width) {
		pd = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(pd);
		map->SetScalarModeToUseCellData();
		auto a = vtkSmartPointer<vtkActor>::New();
		a->SetMapper(map);
		a->SetUserMatrix(m->atEarthM);
		a->GetProperty()->SetLineWidth(width);
		a->GetProperty()->SetLighting(false);
		return a;
	};
	m->ring = ringActor(m->ringPD, 3.0);
	m->ringFixed = ringActor(m->ringFixedPD, 1.5);
	{
		m->nodeSrc = vtkSmartPointer<vtkSphereSource>::New();
		m->nodeSrc->SetRadius(0.07);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(m->nodeSrc->GetOutputPort());
		m->node = vtkSmartPointer<vtkActor>::New();
		m->node->SetMapper(map);
		m->node->SetUserMatrix(m->atEarthM);
		m->node->GetProperty()->SetColor(1.0, 0.25, 0.85);
		m->node->GetProperty()->SetLighting(false);
	}
	// The orbit's plane, filled (shown by "Orbit plane only" in place of the Earth).
	{
		m->discPD = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(m->discPD);
		map->SetScalarModeToUseCellData();
		m->disc = vtkSmartPointer<vtkActor>::New();
		m->disc->SetMapper(map);
		m->disc->SetUserMatrix(m->atEarthM);
		m->disc->GetProperty()->SetOpacity(0.6);
		m->disc->GetProperty()->SetLighting(false);
		m->disc->SetVisibility(false);
	}
	// ...and its two arrows: the orbit's normal (green) and the direction of the Sun (orange). Orbit
	// view only: in the inset, looking from the Sun, the Sun's arrow would point at the eye.
	auto arrowActor = [m](vtkSmartPointer<vtkMatrix4x4> &M, double r, double g, double b) {
		M = vtkSmartPointer<vtkMatrix4x4>::New();
		vtkNew<vtkArrowSource> src;
		src->SetShaftResolution(24);
		src->SetTipResolution(32);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		auto a = vtkSmartPointer<vtkActor>::New();
		a->SetMapper(map);
		a->SetUserMatrix(M);
		a->GetProperty()->SetColor(r, g, b);
		a->GetProperty()->SetLighting(false);
		a->SetVisibility(false);
		m->ren->AddActor(a);
		return a;
	};
	m->normalArrow = arrowActor(m->normalM, 0.3, 0.95, 0.45);
	m->sunArrow = arrowActor(m->sunArrowM, 1.0, 0.55, 0.1);
	for (vtkRenderer *r : {m->ren.GetPointer(), m->inset.GetPointer()})
		for (vtkActor *a : {m->earth.GetPointer(), m->night.GetPointer(), m->term.GetPointer(), m->axis.GetPointer(),
		                    m->disc.GetPointer(), m->ring.GetPointer(), m->ringFixed.GetPointer(), m->node.GetPointer()})
			r->AddActor(a);
	{
		vtkNew<vtkTextActor> t;
		t->SetInput("Seen from the Sun");
		t->GetTextProperty()->SetFontSize(11);
		t->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
		t->SetDisplayPosition(6, 6);
		m->inset->AddViewProp(t);	// AddActor2D: gone in VTK 9.7 (macOS CI)
	}

	// ---- the chart, the facts and the controls -------------------------------------------------
	m->chart = new SunSyncChart(dlg);
	m->facts = new QLabel(dlg);
	m->facts->setTextFormat(Qt::RichText);
	m->facts->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	auto entry = [](const char *rgb, const QString &t) {
		return QString("<span style='color:%1'>&#9632;</span>&nbsp;%2<br>").arg(QString::fromLatin1(rgb)).arg(t);
	};
	auto *legend = new QLabel(
		entry("#ffcd28", "Orbit, sunlit") + entry("#285ae6", "Orbit, in Earth's shadow") +
		entry("#ff40d9", "Ascending node") + entry("#969696", "Same orbit, no precession") +
		entry("#4df273", "Orbit normal (plane only)") + entry("#ff8c1a", "Towards the Sun (plane only)") +
		QString("<i>Altitude drawn %1×</i>").arg(kSsExag, 0, 'f', 0), dlg);
	legend->setTextFormat(Qt::RichText);
	legend->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	m->play = new QPushButton("Play", dlg);
	m->play->setToolTip("Run the year.");
	m->slider = new QScrollBar(Qt::Horizontal, dlg);
	m->slider->setRange(1, 365);
	m->slider->setSingleStep(1);
	m->slider->setPageStep(7);
	m->slider->setValue(1);
	m->slider->setToolTip("Day of the year (2026).");
	m->slider->setStyleSheet(
		"QScrollBar:horizontal { border: none; background: #e0e0e0; height: 8px; margin: 0px; border-radius: 4px; }"
		"QScrollBar::handle:horizontal { background: #808080; border-radius: 4px; min-width: 20px; }"
		"QScrollBar::handle:horizontal:hover { background: #606060; }"
		"QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
		"QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }");
	auto *dayLeft  = new QToolButton(dlg);
	auto *dayRight = new QToolButton(dlg);
	dayLeft->setArrowType(Qt::LeftArrow);
	dayRight->setArrowType(Qt::RightArrow);
	dayLeft->setAutoRepeat(true);
	dayRight->setAutoRepeat(true);
	dayLeft->setToolTip("One day back");
	dayRight->setToolTip("One day forward");
	m->dateLbl = new QLabel(dlg);
	m->dateLbl->setMinimumWidth(150);
	m->dateLbl->setToolTip("β: the Sun's angle above the orbit plane. 90° = the orbit rides the terminator.");
	m->step = new QSpinBox(dlg);
	m->step->setRange(1, 15);
	m->step->setValue(2);
	m->step->setPrefix("step ");
	m->step->setSuffix(" d");
	m->step->setToolTip("Days advanced at each Play frame.");
	m->alt = new QDoubleSpinBox(dlg);
	m->alt->setRange(200.0, 6000.0);
	m->alt->setDecimals(0);
	m->alt->setSingleStep(50.0);
	m->alt->setValue(700.0);
	m->alt->setPrefix("h ");
	m->alt->setSuffix(" km");
	m->alt->setToolTip("Orbit altitude (circular orbit). Above ~5970 km no inclination is sun-synchronous.");
	m->incl = new QDoubleSpinBox(dlg);
	m->incl->setRange(0.0, 180.0);
	m->incl->setDecimals(2);
	m->incl->setSingleStep(1.0);
	m->incl->setValue(ssSunSyncIncl(700.0));
	m->incl->setPrefix("i ");
	m->incl->setSuffix("°");
	m->incl->setToolTip("Inclination. Below 90° the node drifts west, above 90° east; at 90° (polar) it does not drift.");
	m->ltan0 = new QDoubleSpinBox(dlg);
	m->ltan0->setRange(0.0, 24.0);
	m->ltan0->setDecimals(1);
	m->ltan0->setSingleStep(0.5);
	m->ltan0->setWrapping(true);
	m->ltan0->setValue(22.5);
	m->ltan0->setPrefix("LTAN ");
	m->ltan0->setSuffix(" h");
	m->ltan0->setToolTip("Local (mean solar) time of the ascending node on 1 January. 22:30 = the classic "
	                     "10:30 morning descending pass; 6 or 18 = a dawn-dusk orbit; 12 = noon-midnight.");
	QPushButton *ssoBtn = new QPushButton("Make sun-synchronous", dlg);
	ssoBtn->setToolTip("Set the inclination that makes the node drift 360° a year at this altitude.");
	QPushButton *resetBtn = new QPushButton("Reset view", dlg);
	for (QPushButton *b : {m->play, ssoBtn, resetBtn}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
	m->fixedBox = new QCheckBox("No-precession orbit", dlg);
	m->fixedBox->setChecked(true);
	m->fixedBox->setToolTip("Also draw the same orbit as it would be without the Earth's bulge: its plane stays "
	                        "fixed among the stars, and the Sun walks round it in a year.");
	m->planeBox = new QCheckBox("Orbit plane only", dlg);
	m->planeBox->setToolTip("Hide the Earth and fill the orbit's plane instead, cut along the terminator: its "
	                        "day half (yellow) and night half (blue). Two arrows: the orbit's normal and the "
	                        "Sun's direction -- seen from above the Earth's orbit, a sun-synchronous orbit "
	                        "keeps the angle between them all year.");
	m->followBox = new QCheckBox("Follow Earth", dlg);
	m->followBox->setToolTip("Keep the camera on the Earth as it moves round the Sun.");

	auto *row = new QHBoxLayout;
	row->addWidget(m->play);
	row->addWidget(dayLeft);
	row->addWidget(m->slider, 1);
	row->addWidget(dayRight);
	row->addWidget(m->dateLbl);
	row->addWidget(m->step);
	row->addWidget(m->alt);
	row->addWidget(m->incl);
	row->addWidget(ssoBtn);
	row->addWidget(m->ltan0);
	auto *row2 = new QHBoxLayout;
	row2->addWidget(m->fixedBox);
	row2->addWidget(m->planeBox);
	row2->addWidget(m->followBox);
	m->orthoBox = new QCheckBox("Orthographic", dlg);
	m->orthoBox->setToolTip("Orthographic (parallel) projection instead of perspective, in both views: sizes "
	                        "and angles no longer depend on distance from the camera, so a top view shows "
	                        "the true angle between the two arrows.");
	row2->addWidget(m->orthoBox);
	row2->addStretch(1);
	row2->addWidget(resetBtn);
	auto *chartRow = new QHBoxLayout;
	chartRow->addWidget(m->chart, 1);
	auto *side = new QVBoxLayout;
	side->addWidget(m->facts);
	side->addWidget(legend);
	side->addStretch(1);
	chartRow->addLayout(side);
	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->addWidget(m->view, 1);
	lay->addLayout(chartRow);
	lay->addLayout(row);
	lay->addLayout(row2);
	{
		QScreen *scr = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
		const QRect av = scr->availableGeometry();
		const int w = std::min(1250, int(av.width() * 0.9)), hh = std::min(900, int(av.height() * 0.88));
		dlg->resize(w, hh);
		dlg->move(av.x() + (av.width() - w) / 2, av.y() + (av.height() - hh - 30) / 2);
	}

	auto resetView = [m]() {
		vtkCamera *cam = m->ren->GetActiveCamera();
		cam->SetFocalPoint(0, 0, 0);
		cam->SetPosition(0, -kDlOrbitR * 2.1, kDlOrbitR * 1.05);
		cam->SetViewUp(0, 0, 1);
		cam->SetViewAngle(30.0);
		m->followBox->setChecked(false);
		m->setOrtho(m->orthoBox->isChecked());   // re-fits the parallel scale to the new distance
	};
	QObject::connect(m->orthoBox, &QCheckBox::toggled, dlg, [m](bool on) { m->setOrtho(on); });

	m->timer = new QTimer(dlg);
	m->timer->setInterval(30);
	QObject::connect(m->timer, &QTimer::timeout, dlg, [m]() { m->tick(); });
	QObject::connect(m->play, &QPushButton::clicked, dlg, [m]() {
		if (m->timer->isActive()) {
			m->stopPlay();
			return;
		}
		m->play->setText("Pause");
		m->timer->start();
	});
	QObject::connect(m->slider, &QScrollBar::valueChanged, dlg, [m](int d) { m->setDay(d); });
	m->chart->onDay = [m](int d) { m->slider->setValue(d); };
	QObject::connect(dayLeft, &QToolButton::clicked, dlg, [m]() {
		m->slider->setValue(m->slider->value() > 1 ? m->slider->value() - 1 : m->slider->maximum());
	});
	QObject::connect(dayRight, &QToolButton::clicked, dlg, [m]() {
		m->slider->setValue(m->slider->value() < m->slider->maximum() ? m->slider->value() + 1 : 1);
	});
	for (QDoubleSpinBox *b : {m->alt, m->incl, m->ltan0})
		QObject::connect(b, &QDoubleSpinBox::valueChanged, dlg, [m](double) { m->reorbit(); });
	QObject::connect(ssoBtn, &QPushButton::clicked, dlg, [m]() {
		const double i = ssSunSyncIncl(m->h());
		if (std::isfinite(i)) m->incl->setValue(i);
		else
			QMessageBox::information(m->dlg, "Sun-synchronous orbit",
			                         QString("At %1 km no inclination is sun-synchronous: the bulge's pull is too "
			                                 "weak this far out to turn the plane 360° a year, even for a "
			                                 "retrograde equatorial orbit.").arg(m->h(), 0, 'f', 0));
	});
	QObject::connect(m->fixedBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->ringFixed->SetVisibility(on);
		m->chart->showFixed = on;
		m->chart->update();
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->planeBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->showPlaneOnly(on);
		m->view->renderWindow()->Render();
	});
	// The inset keeps its pixel size and corner whatever the window does.
	struct InsetOnResize : QObject {
		SunSyncDialog *d;
		InsetOnResize(QObject *p, SunSyncDialog *dd) : QObject(p), d(dd) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Resize) d->placeInset();
			return QObject::eventFilter(o, e);
		}
	};
	m->view->installEventFilter(new InsetOnResize(m->view, m));
	QObject::connect(m->followBox, &QCheckBox::toggled, dlg, [m](bool on) {
		if (on) {
			vtkCamera *c = m->ren->GetActiveCamera();
			c->SetFocalPoint(m->earthPos);
			c->SetPosition(m->earthPos[0], m->earthPos[1] - 9.0, m->earthPos[2] + 4.5);
			c->SetViewUp(0, 0, 1);
			m->setOrtho(m->orthoBox->isChecked());   // a closer camera: re-fit the parallel scale
		}
		m->setDay(m->slider->value());
	});
	QObject::connect(resetBtn, &QPushButton::clicked, dlg, resetView);

	// PARKED LIKE EVERY TOOL WINDOW (parkTool / parkOnMinimise, 50_scene.cpp).
	struct CloseParks : QObject {
		SunSyncDialog *d;
		CloseParks(QObject *p, SunSyncDialog *dd) : QObject(p), d(dd) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close) {
				if (d->parkNow()) {
					e->ignore();
					return true;
				}
				d->dlg->deleteLater();
			}
			return QObject::eventFilter(o, e);
		}
	};
	dlg->installEventFilter(new CloseParks(dlg, m));
	parkOnMinimise(dlg, [m]() { m->parkNow(); });
	QObject::connect(dlg, &QDialog::rejected, dlg, [m]() {   // Esc
		if (!m->parkNow()) m->dlg->deleteLater();
	});
	QObject::connect(dlg, &QObject::destroyed, [m]() {
		unparkTool(m->scn, m->dlg);
		if (g_sunSync == m) g_sunSync = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	wrapTooltips(dlg);
	g_sunSync = m;
	m->reorbit();
	resetView();
	dlg->show();
	QApplication::processEvents();
	QString error;
	QApplication::setOverrideCursor(Qt::WaitCursor);
	auto tex = magFieldTexture(0, 2026.0, error);
	QApplication::restoreOverrideCursor();
	if (tex) {
		m->earth->SetTexture(tex);
		m->earth->GetProperty()->SetColor(1.0, 1.0, 1.0);
		m->view->renderWindow()->Render();
	}
	return true;
}
