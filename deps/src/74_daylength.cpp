// 74_daylength.cpp -- T & L > "Earth orbit & day length".
//
// A teaching demo: the Earth goes round the Sun with its rotation axis fixed in space, and the length
// of the day at any latitude follows from the angle between the sunlight and that axis. Everything is
// drawn here, natively, so a step of the year costs a re-render and nothing else:
//
//   * the ORBIT VIEW (VTK): the Sun is the scene's only light, a point light at the origin, so the
//     day/night line on the globe is the real terminator of the lighting, not a painted one;
//   * an INSET looking at the Earth from 90 degrees off the Sun line, where the terminator is seen
//     edge-on and its lean against the axis IS the Sun's declination;
//   * the chosen LATITUDE drawn on the globe as a ring cut into its day arc (yellow) and night arc
//     (blue): the day arc's share of the ring is the day length, read straight off the picture;
//   * a CHART of that latitude's day length through the year, with the day marked, and a 24-hour bar.
//
// THE MATHS (one place, below): the ecliptic is the world XY plane, ecliptic north +Z, the Sun at the
// origin. The Sun's ecliptic longitude for a day comes from the low-precision solar ephemeris (Astronomical
// Almanac / NOAA: mean longitude + equation of centre); the Earth sits opposite it, at its distance in
// AU. The axis is fixed in space, tilted by the obliquity towards the June-solstice Sun, so the Sun's
// declination is sin(delta) = sin(tilt) sin(lambda) -- not a formula pasted on top of the picture, but
// the angle the drawn geometry makes. Day length is the geometric sunrise equation,
// cos(H0) = -tan(lat) tan(delta), L = 2 H0 / 15 h (Sun's centre on a flat horizon, no refraction).
//
// SCALE: the Earth is drawn 1 unit in radius on an orbit of 14 units -- the real ratio (1 : 23 500)
// would leave it invisible. Directions, which are all the demo is about, are exact. The orbit is the
// true Kepler ellipse (e = 0.0167, Sun at a focus), so it is nearly a circle, as the real one is.

static const double kDlOrbitR = 14.0;      // orbit radius at 1 AU, scene units
static const double kDlSunR   = 2.2;
static const double kDlEarthTilt = 23.44;
static const double kDlEcc = 0.01671;      // the orbit's eccentricity
static const double kDlAUkm = 149.598e6;

// The orbit is a Kepler ellipse, the Sun at a focus: mean anomaly M (deg) and the Sun's longitude of
// perigee (deg) at `n` days from J2000.0 (2000-01-01 12:00 UTC).
static double dlMeanAnomaly(double n) { return 357.528 + 0.9856003 * n; }
static double dlPerigeeLon(double n) { return (280.460 + 0.9856474 * n) - dlMeanAnomaly(n); }

// Kepler's equation, E - e sin E = M: eccentric anomaly E and true anomaly nu (radians) of M (deg).
static void dlKepler(double Mdeg, double &E, double &nu) {
	const double M = vtkMath::RadiansFromDegrees(std::fmod(Mdeg, 360.0));
	E = M + kDlEcc * std::sin(M);
	for (int i = 0; i < 6; ++i) E -= (E - kDlEcc * std::sin(E) - M) / (1.0 - kDlEcc * std::cos(E));
	nu = 2.0 * std::atan2(std::sqrt(1.0 + kDlEcc) * std::sin(E / 2), std::sqrt(1.0 - kDlEcc) * std::cos(E / 2));
}

// The Sun's ecliptic longitude (deg) and the Earth-Sun distance (AU) at noon UTC of day `doy` of 2026.
static void dlSun(int doy, double &lambda, double &rAU) {
	const double n = 9497.0 + (doy - 1);   // days from J2000.0 (2000-01-01 12:00 UTC) to 2026-01-01 12:00 UTC
	double E, nu;
	dlKepler(dlMeanAnomaly(n), E, nu);
	lambda = std::fmod(dlPerigeeLon(n) + vtkMath::DegreesFromRadians(nu), 360.0);
	if (lambda < 0) lambda += 360.0;
	rAU = 1.0 - kDlEcc * std::cos(E);
}

// The Earth's perihelion direction (heliocentric longitude, deg) for 2026, mid-year.
static double dlPerihelionLon() { return dlPerigeeLon(9497.0 + 182.0) + 180.0; }

// The point of the orbit (scene units) at heliocentric longitude `lon` (deg): r = a(1-e^2)/(1+e cos nu).
static void dlOrbitAt(double lon, double R, double p[3]) {
	const double nu = vtkMath::RadiansFromDegrees(lon - dlPerihelionLon());
	const double r = R * (1.0 - kDlEcc * kDlEcc) / (1.0 + kDlEcc * std::cos(nu));
	const double l = vtkMath::RadiansFromDegrees(lon);
	p[0] = r * std::cos(l);
	p[1] = r * std::sin(l);
	p[2] = 0.0;
}

// The date (2026) on which the mean anomaly reaches `Mdeg` (deg).
static QString dlDateOfAnomaly(double Mdeg) {
	const double n0 = 9497.0;
	double dd = std::fmod(Mdeg - dlMeanAnomaly(n0), 360.0);
	if (dd < 0) dd += 360.0;
	const double days = dd / 0.9856003 + 0.5;   // from 2026-01-01 00:00 UTC
	return QDate(2026, 1, 1).addDays(int(std::floor(days))).toString("d MMM");
}

// The Sun's declination (deg) for a planet tilted `tilt` deg, at ecliptic longitude `lambda`.
static double dlDeclination(double lambda, double tilt) {
	return vtkMath::DegreesFromRadians(std::asin(std::sin(vtkMath::RadiansFromDegrees(tilt)) *
	                                             std::sin(vtkMath::RadiansFromDegrees(lambda))));
}

// Hours of daylight at latitude `lat` (deg) when the Sun's declination is `dec` (deg).
static double dlDayLength(double lat, double dec) {
	const double c = -std::tan(vtkMath::RadiansFromDegrees(lat)) * std::tan(vtkMath::RadiansFromDegrees(dec));
	if (c >= 1.0) return 0.0;
	if (c <= -1.0) return 24.0;
	return 2.0 * vtkMath::DegreesFromRadians(std::acos(c)) / 15.0;
}

static QString dlDateOf(int doy) {
	return QDate(2026, 1, 1).addDays(doy - 1).toString("d MMMM");
}

// The chart under the view: the chosen latitude's day length through the year, the day marked, and a
// 24-hour bar of that day (daylight centred on local noon).
class DayLengthChart : public QWidget {
public:
	double lat = 40.0, tilt = kDlEarthTilt;
	int doy = 172;
	std::vector<double> decl;                // declination of every day, for the current tilt

	std::function<void(int)> onDay;          // the yellow disc dragged to day `d` (does what the slider does)

	DayLengthChart(QWidget *parent) : QWidget(parent) {
		setMinimumHeight(190);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setMouseTracking(true);
	}
	QSize sizeHint() const override { return QSize(900, 210); }

protected:
	bool dragging = false;
	QRect plotRect() const { return QRect(60, 44, width() - 80, height() - 70); }
	int dayAtX(double x) const {
		const QRect plot = plotRect();
		const int nd = int(decl.size());
		const int d = int(std::lround(1 + (x - plot.left()) * (nd - 1) / double(plot.width())));
		return std::clamp(d, 1, nd);
	}
	bool overDisc(const QPointF &pt) const {
		if (decl.empty()) return false;
		const QRect plot = plotRect();
		const int nd = int(decl.size());
		const double x = plot.left() + plot.width() * (doy - 1) / double(nd - 1);
		const double y = plot.bottom() - plot.height() * dlDayLength(lat, decl[doy - 1]) / 24.0;
		return std::hypot(pt.x() - x, pt.y() - y) <= 9.0;
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && overDisc(e->position())) {
			dragging = true;
			e->accept();
			return;
		}
		QWidget::mousePressEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (dragging) {
			const int d = dayAtX(e->position().x());
			if (d != doy && onDay) onDay(d);
			return;
		}
		setCursor(overDisc(e->position()) ? Qt::SizeAllCursor : Qt::ArrowCursor);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) dragging = false;
		QWidget::mouseReleaseEvent(e);
	}

	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), Qt::white);
		if (decl.empty()) return;
		const int nd = int(decl.size());
		const QRect bar(60, 8, width() - 80, 18);
		const QRect plot = plotRect();

		// 24-hour bar of the day: night | day | night, centred on noon.
		const double L = dlDayLength(lat, decl[doy - 1]);
		auto hx = [&](double h) { return bar.left() + bar.width() * h / 24.0; };
		p.fillRect(bar, QColor(25, 35, 80));
		p.fillRect(QRectF(hx(12.0 - L / 2), bar.top(), hx(12.0 + L / 2) - hx(12.0 - L / 2), bar.height()),
		           QColor(255, 205, 40));
		p.setPen(QPen(Qt::black, 1));
		p.drawRect(bar);
		QFont f = font();
		f.setPointSizeF(8.5);
		p.setFont(f);
		for (int h = 0; h <= 24; h += 6)
			p.drawText(QRectF(hx(h) - 15, bar.bottom() + 1, 30, 14), Qt::AlignCenter, QString("%1h").arg(h));
		f.setBold(true);
		p.setFont(f);
		p.drawText(QRect(0, bar.top(), bar.left() - 6, bar.height()), Qt::AlignRight | Qt::AlignVCenter, "24 h");

		// The year.
		auto px = [&](double d) { return plot.left() + plot.width() * (d - 1) / double(nd - 1); };
		auto py = [&](double h) { return plot.bottom() - plot.height() * h / 24.0; };
		f.setBold(false);
		p.setFont(f);
		p.setPen(QPen(QColor(210, 210, 210), 1));
		for (int h = 0; h <= 24; h += 6) p.drawLine(QPointF(plot.left(), py(h)), QPointF(plot.right(), py(h)));
		static const int mdoy[12] = {1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
		static const char *mtxt[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		for (int k = 0; k < 12; ++k) p.drawLine(QPointF(px(mdoy[k]), plot.top()), QPointF(px(mdoy[k]), plot.bottom()));
		p.setPen(Qt::black);
		for (int k = 0; k < 12; ++k)
			p.drawText(QRectF(px(mdoy[k]), plot.bottom() + 2, px(k < 11 ? mdoy[k + 1] : nd) - px(mdoy[k]), 14),
			           Qt::AlignCenter, mtxt[k]);
		for (int h = 0; h <= 24; h += 6)
			p.drawText(QRectF(plot.left() - 34, py(h) - 7, 28, 14), Qt::AlignRight | Qt::AlignVCenter,
			           QString("%1 h").arg(h));
		// 12 h, the equator's (and everyone's at the equinoxes).
		p.setPen(QPen(QColor(120, 120, 120), 1, Qt::DashLine));
		p.drawLine(QPointF(plot.left(), py(12)), QPointF(plot.right(), py(12)));
		// The chosen latitude.
		QPolygonF curve;
		for (int d = 1; d <= nd; ++d) curve << QPointF(px(d), py(dlDayLength(lat, decl[d - 1])));
		p.setPen(QPen(QColor(220, 120, 0), 2.2));
		p.drawPolyline(curve);
		p.setPen(QPen(Qt::black, 1));
		p.drawRect(plot);
		// Today.
		p.setPen(QPen(QColor(200, 0, 0), 1.5));
		p.drawLine(QPointF(px(doy), plot.top()), QPointF(px(doy), plot.bottom()));
		p.setBrush(QColor(255, 205, 40));
		p.setPen(QPen(Qt::black, 1));
		p.drawEllipse(QPointF(px(doy), py(L)), 5, 5);
		f.setBold(true);
		f.setPointSizeF(9.5);
		p.setFont(f);
		const QString txt = QString("%1  —  latitude %2%3:  %4 h %5 min of daylight")
		    .arg(dlDateOf(doy)).arg(std::fabs(lat), 0, 'f', 1).arg(lat >= 0 ? "°N" : "°S")
		    .arg(int(L)).arg(int(std::round((L - int(L)) * 60)) % 60, 2, 10, QChar('0'));
		p.drawText(QRect(plot.left() + 8, plot.top() + 4, plot.width() - 16, 18), Qt::AlignLeft, txt);
	}
};

// The 3-D view. Same one addition the other 3-D tool views make: entering it takes the keyboard back
// from the spin boxes, so the camera keys reach the camera.
class DayLengthView : public GLView {
public:
	DayLengthView(QWidget *parent) : GLView() { setParent(parent); }
protected:
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
};

struct DayLengthDialog {
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	DayLengthView *view = nullptr;
	DayLengthChart *chart = nullptr;
	QLabel *dateLbl = nullptr;
	QScrollBar *slider = nullptr;
	QPushButton *play = nullptr;
	QDoubleSpinBox *tilt = nullptr, *lat = nullptr;
	QSpinBox *step = nullptr;
	QTimer *timer = nullptr;
	bool parked = false;
	bool reallyClose = false;

	vtkSmartPointer<vtkRenderer> ren, inset, labRen;
	vtkSmartPointer<vtkActor> sun, earth, axis, ring, circles, orbit, night, term;
	vtkSmartPointer<vtkPolyData> ringPD, circlesPD;
	vtkSmartPointer<vtkMatrix4x4> earthM, nightM;   // nightM: local +z = towards the Sun, at the Earth
	vtkSmartPointer<vtkBillboardTextActor3D> seasonLbl[4];
	double sunDir[3] = {1, 0, 0};            // from the Earth towards the Sun (unit), current day
	double earthPos[3] = {0, 0, 0};

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

	double tiltDeg() const { return tilt->value(); }

	// The axis, fixed in space: tilted from ecliptic north towards the June-solstice Sun (+Y).
	void axisDir(double a[3]) const {
		const double t = vtkMath::RadiansFromDegrees(tiltDeg());
		a[0] = 0.0;
		a[1] = std::sin(t);
		a[2] = std::cos(t);
	}

	// Tropics and polar circles for the current tilt, in the globe's own frame (z = north pole).
	void buildCircles() {
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		const double t = tiltDeg();
		for (double phi : {t, -t, 90.0 - t, t - 90.0}) {
			const double c = std::cos(vtkMath::RadiansFromDegrees(phi)) * 1.004;
			const double z = std::sin(vtkMath::RadiansFromDegrees(phi)) * 1.004;
			std::vector<vtkIdType> ids;
			for (int i = 0; i <= 180; ++i) {
				const double l = 2.0 * vtkMath::Pi() * i / 180.0;
				ids.push_back(pts->InsertNextPoint(c * std::cos(l), c * std::sin(l), z));
			}
			lines->InsertNextCell(vtkIdType(ids.size()), ids.data());
		}
		circlesPD->SetPoints(pts);
		circlesPD->SetLines(lines);
		circlesPD->Modified();
	}

	// The chosen latitude, cut into day (yellow) and night (blue) by the Sun of declination `dec`. In the
	// globe's frame the subsolar point is (lat dec, lon 0), so a point is lit when its dot product with
	// (cos dec, 0, sin dec) is positive.
	void buildRing(double dec) {
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> segs;
		vtkNew<vtkUnsignedCharArray> rgb;
		rgb->SetNumberOfComponents(3);
		const double phi = vtkMath::RadiansFromDegrees(lat->value());
		const double d = vtkMath::RadiansFromDegrees(dec);
		const double c = std::cos(phi) * 1.012, z = std::sin(phi) * 1.012;
		const int n = 360;
		for (int i = 0; i <= n; ++i) {
			const double l = 2.0 * vtkMath::Pi() * i / n;
			pts->InsertNextPoint(c * std::cos(l), c * std::sin(l), z);
		}
		for (int i = 0; i < n; ++i) {
			const double lm = 2.0 * vtkMath::Pi() * (i + 0.5) / n;
			const bool lit = std::cos(phi) * std::cos(d) * std::cos(lm) + std::sin(phi) * std::sin(d) > 0.0;
			const vtkIdType ids[2] = {i, i + 1};
			segs->InsertNextCell(2, ids);
			const unsigned char day[3] = {255, 205, 40}, night[3] = {40, 70, 200};
			rgb->InsertNextTypedTuple(lit ? day : night);
		}
		ringPD->SetPoints(pts);
		ringPD->SetLines(segs);
		ringPD->GetCellData()->SetScalars(rgb);
		ringPD->Modified();
	}

	// Put the year at day `doy`: the Earth on its orbit, its body turned so Greenwich faces the Sun at
	// noon, the latitude ring re-cut, the inset camera beside the Earth, the chart moved on.
	void setDay(int doy) {
		double lambda, rAU;
		dlSun(doy, lambda, rAU);
		const double dec = dlDeclination(lambda, tiltDeg());
		const double le = vtkMath::RadiansFromDegrees(lambda + 180.0);
		earthPos[0] = kDlOrbitR * rAU * std::cos(le);
		earthPos[1] = kDlOrbitR * rAU * std::sin(le);
		earthPos[2] = 0.0;
		const double r = std::sqrt(earthPos[0] * earthPos[0] + earthPos[1] * earthPos[1]);
		sunDir[0] = -earthPos[0] / r;
		sunDir[1] = -earthPos[1] / r;
		sunDir[2] = 0.0;
		// Body frame: e3 = the axis, e1 = the Sun's direction with its along-axis part removed (lon 0
		// faces the Sun), e2 = e3 x e1.
		double e3[3], e1[3], e2[3];
		axisDir(e3);
		const double k = vtkMath::Dot(sunDir, e3);
		for (int i = 0; i < 3; ++i) e1[i] = sunDir[i] - k * e3[i];
		vtkMath::Normalize(e1);
		vtkMath::Cross(e3, e1, e2);
		for (int i = 0; i < 3; ++i) {
			earthM->SetElement(i, 0, e1[i]);
			earthM->SetElement(i, 1, e2[i]);
			earthM->SetElement(i, 2, e3[i]);
			earthM->SetElement(i, 3, earthPos[i]);
		}
		earthM->Modified();
		// The night half and the terminator: local z towards the Sun, x along ecliptic north x z.
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
		buildRing(dec);
		// Inset: from 90 degrees off the Sun line, in the ecliptic, ecliptic north up -- the terminator
		// is edge-on and leans from the axis by the declination.
		double side[3];
		const double up[3] = {0, 0, 1};
		vtkMath::Cross(up, sunDir, side);
		vtkCamera *cam = inset->GetActiveCamera();
		cam->SetFocalPoint(earthPos);
		cam->SetPosition(earthPos[0] + 6.0 * side[0], earthPos[1] + 6.0 * side[1], earthPos[2] + 6.0 * side[2]);
		cam->SetViewUp(0, 0, 1);
		cam->SetViewAngle(30.0);
		inset->ResetCameraClippingRange();
		chart->doy = doy;
		chart->update();
		dateLbl->setText(QString("%1   δ = %2°").arg(dlDateOf(doy)).arg(dec, 0, 'f', 1));
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// A new tilt: the declinations of the whole year, the circles, and the picture of the current day.
	void retilt() {
		chart->tilt = tiltDeg();
		chart->decl.resize(365);
		for (int d = 1; d <= 365; ++d) {
			double lambda, rAU;
			dlSun(d, lambda, rAU);
			chart->decl[d - 1] = dlDeclination(lambda, tiltDeg());
		}
		buildCircles();
		setDay(slider->value());
	}

	void stopPlay() {
		if (timer && timer->isActive()) timer->stop();
		if (play) play->setText("Play");
	}

	void tick() {
		int d = slider->value() + step->value();
		if (d > slider->maximum()) d -= slider->maximum();
		slider->setValue(d);                 // valueChanged -> setDay
	}
};

static DayLengthDialog *g_dayLength = nullptr;

// A billboard label in the orbit view.
static vtkSmartPointer<vtkBillboardTextActor3D> dlLabel(const char *txt, const double pos[3], int size,
                                                       const unsigned char *rgb = nullptr) {
	auto a = vtkSmartPointer<vtkBillboardTextActor3D>::New();
	a->SetInput(txt);
	a->SetPosition(pos[0], pos[1], pos[2]);
	a->GetTextProperty()->SetFontSize(size);
	if (rgb) a->GetTextProperty()->SetColor(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0);
	else a->GetTextProperty()->SetColor(0.92, 0.92, 0.92);
	a->GetTextProperty()->SetJustificationToCentered();
	return a;
}

// Open (or bring back) the demo.
static bool dayLengthOpen(QWidget *parent, Scene *scene) {
	if (g_dayLength) {
		g_dayLength->unpark();
		g_dayLength->scn = scene;
		return true;
	}
	auto *m = new DayLengthDialog;
	m->scn = scene;
	QDialog *dlg = new QDialog(parent);
	m->dlg = dlg;
	dlg->setWindowTitle("Earth orbit & day length");
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint |
	                    Qt::WindowMaximizeButtonHint);

	// ---- the view --------------------------------------------------------------------------------
	m->view = new DayLengthView(dlg);
	m->view->setMinimumSize(600, 380);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->view->setRenderWindow(rw);
	m->ren = vtkSmartPointer<vtkRenderer>::New();
	m->ren->SetBackground(0.02, 0.02, 0.06);
	m->ren->AutomaticLightCreationOff();
	rw->AddRenderer(m->ren);
	m->inset = vtkSmartPointer<vtkRenderer>::New();
	m->inset->SetViewport(0.70, 0.62, 0.995, 0.99);
	m->inset->SetBackground(0.22, 0.24, 0.32);
	m->inset->AutomaticLightCreationOff();
	m->inset->InteractiveOff();
	m->inset->SetLayer(2);
	m->inset->SetPreserveColorBuffer(false);   // its own background, over the orbit view
	m->inset->SetPreserveDepthBuffer(false);
	// The labels live in an overlay renderer of their own (layer 1, the orbit view's camera, its own
	// headlight), never in `ren`: the Sun's point light would dim the billboard quads by view angle.
	m->labRen = vtkSmartPointer<vtkRenderer>::New();
	m->labRen->SetLayer(1);
	m->labRen->InteractiveOff();
	m->labRen->SetActiveCamera(m->ren->GetActiveCamera());
	rw->SetNumberOfLayers(3);
	rw->AddRenderer(m->labRen);
	rw->AddRenderer(m->inset);
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	m->view->interactor()->SetInteractorStyle(style);
	// THE SUN IS THE LIGHT: a point light at the origin, in both renderers, and nothing else.
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

	// The Sun.
	vtkNew<vtkSphereSource> sunSrc;
	sunSrc->SetRadius(kDlSunR);
	sunSrc->SetThetaResolution(48);
	sunSrc->SetPhiResolution(32);
	vtkNew<vtkPolyDataMapper> sunMap;
	sunMap->SetInputConnection(sunSrc->GetOutputPort());
	m->sun = vtkSmartPointer<vtkActor>::New();
	m->sun->SetMapper(sunMap);
	m->sun->GetProperty()->SetColor(1.0, 0.82, 0.25);
	m->sun->GetProperty()->SetLighting(false);
	m->ren->AddActor(m->sun);

	// The orbit: the Kepler ellipse, the Sun at its focus, the Earth's position on every day lying on it.
	{
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
		m->orbit = vtkSmartPointer<vtkActor>::New();
		m->orbit->SetMapper(map);
		m->orbit->GetProperty()->SetColor(0.55, 0.55, 0.65);
		m->orbit->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->orbit);
		// The four seasons' points, where the Earth stands on those days (Sun at 0/90/180/270 deg).
		const char *names[4] = {"March equinox", "June solstice", "September equinox", "December solstice"};
		for (int k = 0; k < 4; ++k) {
			const double le = vtkMath::RadiansFromDegrees(90.0 * k + 180.0);
			const double pos[3] = {(kDlOrbitR + 2.4) * std::cos(le), (kDlOrbitR + 2.4) * std::sin(le), 0.0};
			m->seasonLbl[k] = dlLabel(names[k], pos, 13);
			m->labRen->AddActor(m->seasonLbl[k]);
		}
		const double sunTop[3] = {0, 0, kDlSunR + 1.0};
		m->labRen->AddActor(dlLabel("Sun", sunTop, 14));
	}

	// The orbit's geometry: the line of the equinoxes and of the solstices (both through the Sun, a dot
	// where each meets the orbit) and the major axis between the apsides (through the Sun). A colour
	// legend says which is which.
	QString legendHtml;
	{
		const double R = kDlOrbitR, e = kDlEcc;
		const double w = vtkMath::RadiansFromDegrees(dlPerihelionLon());
		const double uP[2] = {std::cos(w), std::sin(w)};
		const double peri[3] = {R * (1 - e) * uP[0], R * (1 - e) * uP[1], 0.0};
		const double aph[3] = {-R * (1 + e) * uP[0], -R * (1 + e) * uP[1], 0.0};
		double eq1[3], eq2[3], so1[3], so2[3];
		dlOrbitAt(0.0, R, eq1);
		dlOrbitAt(180.0, R, eq2);
		dlOrbitAt(90.0, R, so1);
		dlOrbitAt(270.0, R, so2);
		const unsigned char cEq[3] = {90, 220, 120}, cSo[3] = {255, 150, 50};
		const unsigned char cMaj[3] = {80, 200, 255};
		struct Seg { const double *p, *q; const unsigned char *c; };
		const Seg segs[] = {{eq1, eq2, cEq}, {so1, so2, cSo}, {peri, aph, cMaj}};
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		vtkNew<vtkUnsignedCharArray> rgb;
		rgb->SetNumberOfComponents(3);
		for (const Seg &s : segs) {
			const vtkIdType ids[2] = {pts->InsertNextPoint(s.p), pts->InsertNextPoint(s.q)};
			lines->InsertNextCell(2, ids);
			rgb->InsertNextTypedTuple(s.c);
		}
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetLines(lines);
		pd->GetCellData()->SetScalars(rgb);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(pd);
		vtkNew<vtkActor> act;
		act->SetMapper(map);
		act->GetProperty()->SetLineWidth(1.5);
		act->GetProperty()->SetLighting(false);
		m->ren->AddActor(act);
		// A dot where each line meets the orbit: equinoxes, solstices, apsides.
		struct Dot { const double *p; const unsigned char *c; };
		const Dot dots[] = {{eq1, cEq}, {eq2, cEq}, {so1, cSo}, {so2, cSo}, {peri, cMaj}, {aph, cMaj}};
		for (const Dot &d : dots) {
			vtkNew<vtkSphereSource> ds;
			ds->SetCenter(d.p[0], d.p[1], d.p[2]);
			ds->SetRadius(0.25);
			vtkNew<vtkPolyDataMapper> dm;
			dm->SetInputConnection(ds->GetOutputPort());
			vtkNew<vtkActor> da;
			da->SetMapper(dm);
			da->GetProperty()->SetColor(d.c[0] / 255.0, d.c[1] / 255.0, d.c[2] / 255.0);
			da->GetProperty()->SetLighting(false);
			m->ren->AddActor(da);
		}
		// Names OUTSIDE the ellipse, a ring further out than the season names (at R + 2.4) so the two
		// sets never meet: Perihelion and Aphelion only; their values are in the legend.
		const double lP[3] = {peri[0] + 1.1 * uP[0], peri[1] + 1.1 * uP[1], 0.0};
		const double lA[3] = {aph[0] - 1.1 * uP[0], aph[1] - 1.1 * uP[1], 0.0};
		m->labRen->AddActor(dlLabel("Perihelion", lP, 14, cMaj));
		m->labRen->AddActor(dlLabel("Aphelion", lA, 14, cMaj));
		// The legend (a Qt label beside the chart, so Qt lays it out: lines never overlap), each entry
		// in its line's colour.
		auto entry = [](const unsigned char *c, const QString &t) {
			return QString("<span style='color:#%1%2%3'>&#9632;</span>&nbsp;%4<br>")
				.arg(c[0], 2, 16, QChar('0')).arg(c[1], 2, 16, QChar('0')).arg(c[2], 2, 16, QChar('0')).arg(t);
		};
		const unsigned char cOrb[3] = {150, 150, 170};
		legendHtml = entry(cOrb, QString("Orbit: ellipse, e = %1, Sun at a focus").arg(e, 0, 'f', 4)) +
			entry(cEq, "Equinoxes (line and dots)") +
			entry(cSo, "Solstices (line and dots)") +
			entry(cMaj, QString("Major axis, 2a; a = %1 M km").arg(kDlAUkm / 1e6, 0, 'f', 1)) +
			entry(cMaj, QString("&nbsp;&nbsp;Perihelion %1, %2 M km").arg(dlDateOfAnomaly(0.0))
			                .arg((1 - e) * kDlAUkm / 1e6, 0, 'f', 1)) +
			entry(cMaj, QString("&nbsp;&nbsp;Aphelion %1, %2 M km").arg(dlDateOfAnomaly(180.0))
			                .arg((1 + e) * kDlAUkm / 1e6, 0, 'f', 1));
	}

	// The Earth: the textured globe of the magnetic-field demo (magFieldSphere / magFieldTexture,
	// 69_magfield.cpp), its user matrix carrying the axis tilt, the turn and the orbit position.
	m->earthM = vtkSmartPointer<vtkMatrix4x4>::New();
	{
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(magFieldSphere(180, 90));
		map->ScalarVisibilityOff();
		m->earth = vtkSmartPointer<vtkActor>::New();
		m->earth->SetMapper(map);
		m->earth->SetUserMatrix(m->earthM);
		m->earth->GetProperty()->SetColor(0.35, 0.55, 0.85);   // seen only until the picture arrives
		// Unlit, at full brightness: the night is the translucent shade below, as in "Day length &
		// axial tilt", so both sides stay readable and the day/night edge is sharp.
		m->earth->GetProperty()->SetLighting(false);
	}
	// The night half (dark, translucent, opacity 0.55 as the other demo) and the terminator (orange).
	m->nightM = vtkSmartPointer<vtkMatrix4x4>::New();
	{
		vtkNew<vtkSphereSource> src;
		src->SetRadius(1.006);
		src->SetStartPhi(90.0);   // phi from local +z (the Sun): 90..180 is the far half
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
	// The axis, poking out of both poles.
	{
		vtkNew<vtkLineSource> src;
		src->SetPoint1(0, 0, -1.7);
		src->SetPoint2(0, 0, 1.7);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		m->axis = vtkSmartPointer<vtkActor>::New();
		m->axis->SetMapper(map);
		m->axis->SetUserMatrix(m->earthM);
		m->axis->GetProperty()->SetColor(1.0, 1.0, 1.0);
		m->axis->GetProperty()->SetLineWidth(2.5);
		m->axis->GetProperty()->SetLighting(false);
	}
	// Tropics and polar circles.
	{
		m->circlesPD = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(m->circlesPD);
		m->circles = vtkSmartPointer<vtkActor>::New();
		m->circles->SetMapper(map);
		m->circles->SetUserMatrix(m->earthM);
		m->circles->GetProperty()->SetColor(0.85, 0.85, 0.85);
		m->circles->GetProperty()->SetLineWidth(1.0);
		m->circles->GetProperty()->SetLighting(false);
	}
	// The chosen latitude, day arc and night arc.
	{
		m->ringPD = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(m->ringPD);
		map->SetScalarModeToUseCellData();
		m->ring = vtkSmartPointer<vtkActor>::New();
		m->ring->SetMapper(map);
		m->ring->SetUserMatrix(m->earthM);
		m->ring->GetProperty()->SetLineWidth(3.0);
		m->ring->GetProperty()->SetLighting(false);
	}
	m->circles->SetVisibility(false);   // off until asked for (the "Tropics & polar circles" box)
	for (vtkRenderer *r : {m->ren.GetPointer(), m->inset.GetPointer()})
		for (vtkActor *a : {m->earth.GetPointer(), m->night.GetPointer(), m->term.GetPointer(), m->axis.GetPointer(),
		                    m->circles.GetPointer(), m->ring.GetPointer()})
			r->AddActor(a);

	// ---- the chart and the controls --------------------------------------------------------------
	m->chart = new DayLengthChart(dlg);

	m->play = new QPushButton("Play", dlg);
	m->play->setToolTip("Run the year.");
	m->slider = new QScrollBar(Qt::Horizontal, dlg);
	m->slider->setRange(1, 365);
	m->slider->setSingleStep(1);
	m->slider->setPageStep(7);
	m->slider->setValue(172);
	m->slider->setToolTip("Day of the year (2026).");
	// ARROWS AT BOTH TIPS, as Aquamoto's slice slider (75_aquamoto.cpp): no native style draws them
	// reliably in this Qt build, so the bar is a plain groove + handle and the arrows are real buttons.
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
	m->dateLbl->setMinimumWidth(170);
	m->step = new QSpinBox(dlg);
	m->step->setRange(1, 15);
	m->step->setValue(1);
	m->step->setPrefix("step ");
	m->step->setSuffix(" d");
	m->step->setToolTip("Days advanced at each Play frame.");
	m->lat = new QDoubleSpinBox(dlg);
	m->lat->setRange(-90.0, 90.0);
	m->lat->setDecimals(1);
	m->lat->setSingleStep(5.0);
	m->lat->setValue(40.0);
	m->lat->setPrefix("lat ");
	m->lat->setSuffix("°");
	m->lat->setToolTip("The latitude drawn on the globe (yellow = its day arc, blue = its night arc) and in the chart.");
	m->tilt = new QDoubleSpinBox(dlg);
	m->tilt->setRange(0.0, 90.0);
	m->tilt->setDecimals(2);
	m->tilt->setSingleStep(1.0);
	m->tilt->setValue(kDlEarthTilt);
	m->tilt->setPrefix("tilt ");
	m->tilt->setSuffix("°");
	m->tilt->setToolTip("Axial tilt (obliquity). Earth's is 23.44°. At 0° every latitude has 12 h all year; "
	                    "more tilt pushes the polar circles towards the equator.");
	QPushButton *earthBtn = new QPushButton("Earth", dlg);
	earthBtn->setToolTip("Back to Earth's tilt, 23.44°.");
	QPushButton *resetBtn = new QPushButton("Reset view", dlg);
	for (QPushButton *b : {m->play, earthBtn, resetBtn}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}

	auto *row = new QHBoxLayout;
	row->addWidget(m->play);
	row->addWidget(dayLeft);
	row->addWidget(m->slider, 1);
	row->addWidget(dayRight);
	row->addWidget(m->dateLbl);
	row->addWidget(m->step);
	row->addWidget(m->lat);
	row->addWidget(m->tilt);
	row->addWidget(earthBtn);
	row->addWidget(resetBtn);
	auto *circlesBox = new QCheckBox("Tropics && polar circles", dlg);
	circlesBox->setToolTip("Show the tropics and the polar circles on the globe.");
	row->addWidget(circlesBox);
	auto *legend = new QLabel(legendHtml, dlg);
	legend->setTextFormat(Qt::RichText);
	legend->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	auto *chartRow = new QHBoxLayout;
	chartRow->addWidget(m->chart, 1);
	chartRow->addWidget(legend);
	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->addWidget(m->view, 1);
	lay->addLayout(chartRow);
	lay->addLayout(row);
	// Fit the screen it opens on: never taller or wider than its available area.
	{
		QScreen *scr = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
		const QRect av = scr->availableGeometry();
		const int w = std::min(1150, int(av.width() * 0.9)), h = std::min(860, int(av.height() * 0.85));
		dlg->resize(w, h);
		dlg->move(av.x() + (av.width() - w) / 2, av.y() + (av.height() - h - 30) / 2);
	}
	QObject::connect(circlesBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->circles->SetVisibility(on);
		m->view->renderWindow()->Render();
	});

	auto resetView = [m]() {
		vtkCamera *cam = m->ren->GetActiveCamera();
		cam->SetFocalPoint(0, 0, 0);
		cam->SetPosition(0, -kDlOrbitR * 2.1, kDlOrbitR * 1.05);
		cam->SetViewUp(0, 0, 1);
		cam->SetViewAngle(30.0);
		m->ren->ResetCameraClippingRange();
		m->view->renderWindow()->Render();
	};

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
	m->chart->onDay = [m](int d) { m->slider->setValue(d); };   // the chart's disc drives the slider
	QObject::connect(dayLeft, &QToolButton::clicked, dlg, [m]() {
		m->slider->setValue(m->slider->value() > 1 ? m->slider->value() - 1 : m->slider->maximum());
	});
	QObject::connect(dayRight, &QToolButton::clicked, dlg, [m]() {
		m->slider->setValue(m->slider->value() < m->slider->maximum() ? m->slider->value() + 1 : 1);
	});
	QObject::connect(m->lat, &QDoubleSpinBox::valueChanged, dlg, [m](double v) {
		m->chart->lat = v;
		m->setDay(m->slider->value());
	});
	QObject::connect(m->tilt, &QDoubleSpinBox::valueChanged, dlg, [m](double) { m->retilt(); });
	QObject::connect(earthBtn, &QPushButton::clicked, dlg, [m]() { m->tilt->setValue(kDlEarthTilt); });
	QObject::connect(resetBtn, &QPushButton::clicked, dlg, resetView);

	// PARKED LIKE EVERY TOOL WINDOW (parkTool / parkOnMinimise, 50_scene.cpp).
	struct CloseParks : QObject {
		DayLengthDialog *d;
		CloseParks(QObject *p, DayLengthDialog *dd) : QObject(p), d(dd) {}
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
		if (g_dayLength == m) g_dayLength = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	wrapTooltips(dlg);
	g_dayLength = m;
	m->chart->lat = m->lat->value();
	m->retilt();
	resetView();
	dlg->show();
	QApplication::processEvents();
	// The Earth's picture last: the first one in a session may still have to come from the GMT data
	// server, and the demo is already running without it.
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
