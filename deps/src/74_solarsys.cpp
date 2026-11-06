// 74_solarsys.cpp -- T & L > "Solar system".
//
// The Sun, the eight planets and the dwarf planets Pluto, Ceres and Eris where they are at any instant
// from 100 AD to 2999 AD, the orbits they run on, and the Moon round the Earth. Positions come from
// 74_ephem.cpp (JPL's Keplerian elements, the same numbers the "Sky viewer" uses), so a planet seen here
// on a date is the one the sky shows that night. The asteroid Apophis comes from its own table
// (74_apophis.cpp, 1900-2100), through the same ellipse-to-XYZ (ephElemsHelio). Everything is native: a step of time costs a re-render
// and nothing else.
//
// FRAME: the J2000 ecliptic -- the world XY plane is the ecliptic, +X the March equinox, +Z ecliptic
// north, the Sun at the origin and the scene's only light (a point light, so each planet's lit half
// faces it and the phases are real).
//
// SCALE: true distances leave the inner planets in a dot next to the Sun (Neptune is 77x Mercury's
// distance), so the radial distance is mapped -- linear, square root or logarithmic, the user's pick --
// along the body's TRUE direction: the angles, which is where a body is in its year, are exact in every
// mode. The fourth pick, Titius-Bode, moves each body's whole orbit out (or in) to the distance the
// Titius-Bode law gives it, a = 0.4 + 0.3 * 2^n AU, by the ratio of that distance to the body's real
// semi-major axis, and then draws it like the square-root pick: the law laid out next to the real thing.
// Sizes are never to scale (that would be invisible); they grow as the square root of the real radius,
// times the "Size" factor. The Moon is drawn at 3.5 Earth-radii (drawn) from the Earth, in its true
// direction.

static const double kSolUnit = 10.0;       // scene units per mapped AU
static const double kSolSunR = 1.8;
static const int kSolApo = EPH_NPLANETS;   // the asteroid Apophis (74_apophis.cpp), after the ephemeris' bodies
static const int kSolN = EPH_NPLANETS + 1; // the bodies drawn
static const int kSolSun = kSolN;          // the Sun's label, after the bodies'

static double solDot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double solNorm(const double a[3]) { return std::sqrt(solDot(a, a)); }

// Each body's drawn colour (the orbit gets the same, darker): the planets, then Pluto, Ceres, Eris, Apophis.
static const double kSolColor[kSolN][3] = {
	{0.62, 0.60, 0.58}, {0.95, 0.85, 0.60}, {0.35, 0.55, 0.95}, {0.85, 0.42, 0.25},
	{0.86, 0.72, 0.52}, {0.92, 0.82, 0.56}, {0.60, 0.85, 0.90}, {0.38, 0.52, 0.96},
	{0.82, 0.70, 0.58}, {0.70, 0.70, 0.66}, {0.88, 0.88, 0.92}, {0.95, 0.45, 0.70}};

// A body's name and mean radius (km): the ephemeris' bodies, then Apophis.
static const char *solName(int p) { return p == kSolApo ? "Apophis" : kEphPlanetName[p]; }
static double solRkm(int p) { return p == kSolApo ? kApophisRkm : kEphPlanetRkm[p]; }

// Body p's heliocentric position (AU, J2000 ecliptic) at `tt`; `Mforce` (deg) puts it at that mean
// anomaly on the instant's orbit (NaN = where it is).
static void solHelio(int p, double tt, double Mforce, double xyz[3]) {
	if (p != kSolApo) ephPlanetHelioM(p, tt, Mforce, xyz);
	else if (std::isnan(Mforce)) ephApophisHelio(tt, xyz);
	else ephApophisOrbit(tt, Mforce, xyz);
}

// The view: entering it takes the keyboard back from the spin boxes, as the other 3-D tool views do;
// the bare cursor (no button down) reports where it is, so a hidden name can show under it.
class SolarSysView : public GLView {
public:
	std::function<void(const QPoint &, bool)> onHover;   // point, inside the view
	SolarSysView(QWidget *parent) : GLView() {
		setParent(parent);
		setMouseTracking(true);
	}
protected:
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		GLView::mouseMoveEvent(e);
		if (e->buttons() == Qt::NoButton && onHover) onHover(e->position().toPoint(), true);
	}
	void leaveEvent(QEvent *e) override {
		if (onHover) onHover(QPoint(), false);
		GLView::leaveEvent(e);
	}
};

struct SolarSysDialog {
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	SolarSysView *view = nullptr;
	QDateTimeEdit *when = nullptr;
	QComboBox *stepBox = nullptr, *scaleBox = nullptr;
	QDoubleSpinBox *sizeBox = nullptr;
	QPushButton *play = nullptr;
	QCheckBox *backBox = nullptr, *orbitsBox = nullptr, *labelsBox = nullptr, *moonBox = nullptr,
	          *dwarfBox = nullptr, *apoBox = nullptr;
	QLabel *info = nullptr;
	QWidget *side = nullptr;
	QToolButton *sideBtn = nullptr;
	QTimer *timer = nullptr;
	bool parked = false;
	bool reallyClose = false;
	double jd = 2451545.0;                   // the instant shown (Julian Date, UT)
	double T = 0.0;                          // its Julian centuries from J2000 (TT)
	int hovered = -1;                        // the body whose name the cursor is showing, -1 = none

	vtkSmartPointer<vtkRenderer> ren, labRen;
	vtkSmartPointer<vtkActor> sun, planet[kSolN], orbit[kSolN], moon, ring, equinox;
	vtkSmartPointer<vtkPolyData> orbitPD[kSolN];
	vtkSmartPointer<vtkMatrix4x4> planetM[kSolN], moonM, ringM;
	vtkSmartPointer<vtkBillboardTextActor3D> label[kSolN + 1], eqLbl;   // the bodies', then the Sun's
	vtkSmartPointer<vtkLineSource> eqSrc;
	double helio[kSolN][3] = {};             // heliocentric, AU, J2000 ecliptic
	double pos[kSolN][3] = {};               // drawn, scene units
	bool apoSpan = true;                     // the instant is inside Apophis' table (1900-2100)

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

	bool titiusBode() const { return scaleBox && scaleBox->currentIndex() == 3; }

	// Is body `p` drawn? (Ceres and Eris have their own box; Apophis its own, and only inside its table.)
	bool shown(int p) const {
		if (p == kSolApo) return apoSpan && apoBox->isChecked();
		return !(p == EPH_CERES || p == EPH_ERIS) || dwarfBox->isChecked();
	}

	// AU from the Sun -> scene units from the Sun, by the "Distances" pick (Titius-Bode draws as the
	// square root, after its own per-body stretch, solFactor).
	double mapR(double r) const {
		switch (scaleBox ? scaleBox->currentIndex() : 1) {
		case 0: return kSolUnit * r;
		case 2: return kSolUnit * 2.2 * std::log2(1.0 + r / 0.25);
		default: return kSolUnit * std::sqrt(r);
		}
	}
	// The Titius-Bode stretch of body p's orbit: its law distance over its real semi-major axis (1 in
	// every other mode, and for Apophis, which has no slot in the law).
	double solFactor(int p) const {
		if (!titiusBode() || p == kSolApo) return 1.0;
		return ephTitiusBode(p) / (kEphEl[p].a + kEphRate[p].a * T);
	}
	double bodyR(int p) const {
		// Square root of the real radius, never below a dot that can still be seen (Ceres, Apophis).
		return sizeBox->value() * std::max(0.12, 0.32 * std::sqrt(solRkm(p) / kEphPlanetRkm[EPH_EMB]));
	}
	void mapPoint(int p, const double h[3], double out[3]) const {
		const double r = solNorm(h), k = r > 0 ? mapR(r * solFactor(p)) / r : 0.0;
		for (int i = 0; i < 3; ++i) out[i] = h[i] * k;
	}

	// A body's place: scale (its drawn radius) and, for the Earth, its turn -- the body frame of the
	// J2000 equator turned by the sidereal time, so Greenwich faces where it faces at that instant.
	void placePlanet(int p) {
		const double R = bodyR(p);
		double e1[3] = {1, 0, 0}, e2[3] = {0, 1, 0}, e3[3] = {0, 0, 1};
		if (p == EPH_EMB) {
			const double g = ephGMST(jd) * kEphRad;
			const double q1[3] = {std::cos(g), std::sin(g), 0.0}, q2[3] = {-std::sin(g), std::cos(g), 0.0};
			const double q3[3] = {0.0, 0.0, 1.0};
			ephEquToEcl(q1, e1);
			ephEquToEcl(q2, e2);
			ephEquToEcl(q3, e3);
		}
		for (int i = 0; i < 3; ++i) {
			planetM[p]->SetElement(i, 0, R * e1[i]);
			planetM[p]->SetElement(i, 1, R * e2[i]);
			planetM[p]->SetElement(i, 2, R * e3[i]);
			planetM[p]->SetElement(i, 3, pos[p][i]);
		}
		planetM[p]->Modified();
		label[p]->SetPosition(pos[p][0], pos[p][1], pos[p][2] + R + 0.45);
	}

	// The orbit through the date's elements: the mean anomaly run once round, each point mapped.
	void buildOrbit(int p, double tt) {
		vtkNew<vtkPoints> pts;
		std::vector<vtkIdType> ids;
		for (int i = 0; i <= 360; ++i) {
			double h[3], q[3];
			solHelio(p, tt, double(i), h);
			mapPoint(p, h, q);
			ids.push_back(pts->InsertNextPoint(q));
		}
		vtkNew<vtkCellArray> line;
		line->InsertNextCell(vtkIdType(ids.size()), ids.data());
		orbitPD[p]->SetPoints(pts);
		orbitPD[p]->SetLines(line);
		orbitPD[p]->Modified();
	}

	// Saturn's rings in the plane of its equator: IAU pole RA 40.589, Dec 83.537 (J2000).
	void placeRing() {
		const double a = 40.589 * kEphRad, d = 83.537 * kEphRad;
		const double q[3] = {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
		double n[3], u[3], v[3];
		ephEquToEcl(q, n);
		const double z[3] = {0, 0, 1};
		vtkMath::Cross(z, n, u);
		vtkMath::Normalize(u);
		vtkMath::Cross(n, u, v);
		const double R = bodyR(EPH_SATURN);
		for (int i = 0; i < 3; ++i) {
			ringM->SetElement(i, 0, R * u[i]);
			ringM->SetElement(i, 1, R * v[i]);
			ringM->SetElement(i, 2, R * n[i]);
			ringM->SetElement(i, 3, pos[EPH_SATURN][i]);
		}
		ringM->Modified();
	}

	// Who is drawn and whose name shows: every name with "Names" ticked, else only the hovered one.
	void applyVisibility() {
		const bool names = labelsBox->isChecked();
		for (int p = 0; p < kSolN; ++p) {
			planet[p]->SetVisibility(shown(p));
			orbit[p]->SetVisibility(shown(p) && orbitsBox->isChecked());
			label[p]->SetVisibility(shown(p) && (names || hovered == p));
		}
		label[kSolSun]->SetVisibility(names || hovered == kSolSun);
		ring->SetVisibility(true);
		moon->SetVisibility(moonBox->isChecked());
	}

	// Put the system at the instant `jd`.
	void update() {
		const double tt = ephTT(jd);
		T = (tt - 2451545.0) / 36525.0;
		apoSpan = ephApophisSpan(tt);
		for (int p = 0; p < kSolN; ++p) {
			if (p == kSolApo && !apoSpan) continue;   // no position: stays hidden (shown())
			solHelio(p, tt, std::nan(""), helio[p]);
			mapPoint(p, helio[p], pos[p]);
			placePlanet(p);
			buildOrbit(p, tt);
		}
		placeRing();
		// The Moon: its true direction from the Earth, at a drawn distance.
		{
			double mq[3], me[3];
			ephGeocentric(SKY_MOON, tt, mq);
			ephEquToEcl(mq, me);
			vtkMath::Normalize(me);
			const double Re = bodyR(EPH_EMB), Rm = Re * 0.2727, dm = Re * 3.5;
			double c[3];
			for (int i = 0; i < 3; ++i) c[i] = pos[EPH_EMB][i] + dm * me[i];
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) moonM->SetElement(i, j, i == j ? Rm : 0.0);
				moonM->SetElement(i, 3, c[i]);
			}
			moonM->Modified();
			// No name: it would race round the Earth at every Play frame.
		}
		// The March-equinox direction: from the Sun out past Mars' orbit.
		eqSrc->SetPoint2(mapR(1.9), 0.0, 0.0);
		eqLbl->SetPosition(mapR(1.9) + 0.6, 0.0, 0.4);
		applyVisibility();
		updateInfo(tt);
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// The cursor over a body (within its drawn disc + 8 px) shows that body's name, when names are off.
	void hover(const QPoint &p, bool inside) {
		int hit = -1;
		if (inside && !labelsBox->isChecked()) {
			double dx, dy;
			displayPxFromQt(view, view->renderWindow(), p, dx, dy);
			double best = 1e30;
			const double tol = 8.0 * view->devicePixelRatioF();
			auto test = [&](int id, const double c[3], double R) {
				double d0[3], d1[3];
				ren->SetWorldPoint(c[0], c[1], c[2], 1.0);
				ren->WorldToDisplay();
				ren->GetDisplayPoint(d0);
				// The disc's screen radius: a point R away along the camera's up vector.
				double up[3];
				ren->GetActiveCamera()->GetViewUp(up);
				ren->SetWorldPoint(c[0] + R * up[0], c[1] + R * up[1], c[2] + R * up[2], 1.0);
				ren->WorldToDisplay();
				ren->GetDisplayPoint(d1);
				if (d0[2] < 0.0 || d0[2] > 1.0) return;
				const double rr = std::hypot(d1[0] - d0[0], d1[1] - d0[1]);
				const double d = std::hypot(d0[0] - dx, d0[1] - dy);
				if (d <= rr + tol && d - rr < best) {
					best = d - rr;
					hit = id;
				}
			};
			for (int b = 0; b < kSolN; ++b)
				if (shown(b)) test(b, pos[b], bodyR(b));
			const double o[3] = {0, 0, 0};
			test(kSolSun, o, kSolSunR);
		}
		if (hit == hovered) return;
		hovered = hit;
		applyVisibility();
		view->renderWindow()->Render();
	}

	// The table beside the view: where each body is, as numbers.
	void updateInfo(double tt) {
		const double *E = helio[EPH_EMB];
		const double rE = solNorm(E);
		QString h = QString("<b>%1 UTC</b><br>TT − UT = %2 s &nbsp; JD %3<br><br>")
			.arg(ephDateTime(jd).toString("yyyy-MM-dd HH:mm")).arg(ephDeltaT(2000.0 + (jd - 2451545.0) / 365.25), 0, 'f', 0)
			.arg(jd, 0, 'f', 4);
		h += "<table cellspacing=0 cellpadding=2>"
		     "<tr><th align=left></th><th align=right>Sun dist.<br>AU</th><th align=right>Earth dist.<br>AU</th>"
		     "<th align=right>Helioc.<br>longitude</th><th align=right>Mag.</th></tr>";
		for (int p = 0; p < kSolN; ++p) {
			if (p == kEphMajor)
				h += "<tr><td colspan=5><i>dwarf planets</i></td></tr>";
			if (p == kSolApo)
				h += "<tr><td colspan=5><i>asteroid</i></td></tr>";
			const QColor c = QColor::fromRgbF(kSolColor[p][0], kSolColor[p][1], kSolColor[p][2]);
			if (p == kSolApo && !apoSpan) {
				h += QString("<tr><td><span style='color:%1'>&#9679;</span> %2</td>"
				             "<td colspan=4 align=right>no position outside 1900–2100</td></tr>").arg(c.name()).arg(solName(p));
				continue;
			}
			const double r = solNorm(helio[p]);
			double lon = std::atan2(helio[p][1], helio[p][0]) / kEphRad;
			if (lon < 0) lon += 360.0;
			QString dE = "—", mag = "—";
			if (p != EPH_EMB) {
				double g[3];
				for (int i = 0; i < 3; ++i) g[i] = helio[p][i] - E[i];
				const double d = solNorm(g);
				const double ci = std::clamp((r * r + d * d - rE * rE) / (2.0 * r * d), -1.0, 1.0);
				const double ph = std::acos(ci) / kEphRad;
				// A close pass (Apophis, 2029) reads in km: 0.000 AU says nothing.
				dE = d < 0.01 ? QString("%1 km").arg(d * kEphAUkm, 0, 'f', 0) : QString::number(d, 'f', 3);
				mag = QString::number(p == kSolApo ? ephAsteroidMag(kApophisH, kApophisG, r, d, ph)
				                                   : ephPlanetMag(p, r, d, ph), 'f', 1);
			}
			h += QString("<tr><td><span style='color:%1'>&#9679;</span> %2</td><td align=right>%3</td>"
			             "<td align=right>%4</td><td align=right>%5°</td><td align=right>%6</td></tr>")
				.arg(c.name()).arg(solName(p)).arg(r, 0, 'f', 3).arg(dE).arg(lon, 0, 'f', 1).arg(mag);
		}
		h += "</table>";
		// The Moon: distance and phase (illuminated share, from the Sun-Moon elongation).
		double l, b, dkm, mq[3], sq[3];
		ephMoon(tt, l, b, dkm);
		ephGeocentric(SKY_MOON, tt, mq);
		ephGeocentric(SKY_SUN, tt, sq);
		const double ce = std::clamp(solDot(mq, sq) / (solNorm(mq) * solNorm(sq)), -1.0, 1.0);
		const double illum = 0.5 * (1.0 - ce);
		h += QString("<br><b>Moon</b>: %1 km from the Earth, %2% lit").arg(dkm, 0, 'f', 0).arg(100.0 * illum, 0, 'f', 0);
		info->setText(h);
	}

	void stopPlay() {
		if (timer && timer->isActive()) timer->stop();
		if (play) play->setText("Play");
	}

	// Days per step, by the "Step" pick.
	double stepDays() const {
		static const double d[] = {1.0 / 24.0, 0.25, 1.0, 7.0, 30.4375, 365.25};
		return d[std::clamp(stepBox->currentIndex(), 0, 5)];
	}

	// Move the clock by `days` and show it (the date box follows without re-entering update twice).
	void setJD(double v) {
		const double lo = ephJD(when->minimumDateTime()), hi = ephJD(when->maximumDateTime());
		jd = std::clamp(v, lo, hi);
		QSignalBlocker b(when);
		when->setDateTime(ephDateTime(jd));
		update();
	}

	void tick() {
		setJD(jd + (backBox->isChecked() ? -1.0 : 1.0) * stepDays());
	}

	// The farthest drawn body from the Sun now, in mapped AU: what "All bodies" has to hold.
	double outerAU() const {
		double r = 0.0;
		for (int p = 0; p < kSolN; ++p)
			if (shown(p)) r = std::max(r, solNorm(helio[p]) * solFactor(p));
		return r;
	}

	// Camera presets: the inner planets, or the whole system, from above the ecliptic at a slant.
	void frame(double rAU, bool top) {
		vtkCamera *cam = ren->GetActiveCamera();
		const double R = mapR(rAU);
		cam->SetFocalPoint(0, 0, 0);
		if (top) {
			cam->SetPosition(0, 0, R * 3.6);
			cam->SetViewUp(0, 1, 0);
		}
		else {
			cam->SetPosition(0, -R * 2.6, R * 1.35);
			cam->SetViewUp(0, 0, 1);
		}
		cam->SetViewAngle(30.0);
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// Fold / unfold the right-hand panel; the arrow points where the panel will go.
	void setSide(bool open) {
		side->setVisible(open);
		sideBtn->setArrowType(open ? Qt::RightArrow : Qt::LeftArrow);
		sideBtn->setToolTip(open ? "Fold the panel away" : "Show the panel");
	}
};

static SolarSysDialog *g_solarSys = nullptr;

// A billboard label (overlay renderer: never dimmed by the Sun's light).
static vtkSmartPointer<vtkBillboardTextActor3D> solLabel(const QString &txt, int size, const double rgb[3]) {
	auto a = vtkSmartPointer<vtkBillboardTextActor3D>::New();
	a->SetInput(txt.toUtf8().constData());
	a->GetTextProperty()->SetFontSize(size);
	a->GetTextProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
	a->GetTextProperty()->SetJustificationToCentered();
	return a;
}

// Open (or bring back) the demo.
static bool solarSysOpen(QWidget *parent, Scene *scene) {
	if (g_solarSys) {
		g_solarSys->unpark();
		g_solarSys->scn = scene;
		return true;
	}
	auto *m = new SolarSysDialog;
	m->scn = scene;
	QDialog *dlg = new QDialog(parent);
	m->dlg = dlg;
	dlg->setWindowTitle("Solar system");
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint |
	                    Qt::WindowMaximizeButtonHint);

	// ---- the view --------------------------------------------------------------------------------
	m->view = new SolarSysView(dlg);                          // the 3-D view of the system
	m->view->setMinimumSize(560, 420);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->view->setRenderWindow(rw);
	m->ren = vtkSmartPointer<vtkRenderer>::New();
	m->ren->SetBackground(0.01, 0.01, 0.04);
	m->ren->AutomaticLightCreationOff();
	rw->AddRenderer(m->ren);
	// Labels in an overlay renderer (layer 1, the same camera, its own headlight): the Sun's point light
	// would dim billboard quads by view angle.
	m->labRen = vtkSmartPointer<vtkRenderer>::New();
	m->labRen->SetLayer(1);
	m->labRen->InteractiveOff();
	m->labRen->SetActiveCamera(m->ren->GetActiveCamera());
	rw->SetNumberOfLayers(2);
	rw->AddRenderer(m->labRen);
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	m->view->interactor()->SetInteractorStyle(style);
	{
		// THE SUN IS THE LIGHT: a point light at the origin, and nothing else.
		vtkNew<vtkLight> l;
		l->SetLightTypeToSceneLight();
		l->SetPositional(true);
		l->SetPosition(0, 0, 0);
		l->SetFocalPoint(1, 0, 0);
		l->SetConeAngle(180.0);
		l->SetIntensity(1.0);
		m->ren->AddLight(l);
	}

	// The Sun.
	{
		vtkNew<vtkSphereSource> src;
		src->SetRadius(kSolSunR);
		src->SetThetaResolution(48);
		src->SetPhiResolution(32);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		m->sun = vtkSmartPointer<vtkActor>::New();
		m->sun->SetMapper(map);
		m->sun->GetProperty()->SetColor(1.0, 0.82, 0.25);
		m->sun->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->sun);
		const double c[3] = {1.0, 0.9, 0.5};
		m->label[kSolSun] = solLabel("Sun", 14, c);
		m->label[kSolSun]->SetPosition(0, 0, kSolSunR + 0.8);
		m->labRen->AddActor(m->label[kSolSun]);
	}

	// The bodies: unit spheres (the Earth: the textured globe of 69_magfield.cpp, with normals for the
	// Sun's light), each placed by its user matrix. Their orbits, darker, in the same colour.
	vtkSmartPointer<vtkPolyData> globe = vtkSmartPointer<vtkPolyData>::New();
	globe->DeepCopy(magFieldSphere(180, 90));
	{
		vtkNew<vtkFloatArray> nrm;
		nrm->SetNumberOfComponents(3);
		nrm->SetName("Normals");
		for (vtkIdType i = 0; i < globe->GetNumberOfPoints(); ++i) nrm->InsertNextTuple(globe->GetPoint(i));
		globe->GetPointData()->SetNormals(nrm);
	}
	for (int p = 0; p < kSolN; ++p) {
		vtkNew<vtkPolyDataMapper> map;
		if (p == EPH_EMB) {
			map->SetInputData(globe);
			map->ScalarVisibilityOff();
		}
		else {
			vtkNew<vtkSphereSource> src;
			src->SetRadius(1.0);
			src->SetThetaResolution(40);
			src->SetPhiResolution(24);
			map->SetInputConnection(src->GetOutputPort());
		}
		m->planetM[p] = vtkSmartPointer<vtkMatrix4x4>::New();
		m->planet[p] = vtkSmartPointer<vtkActor>::New();
		m->planet[p]->SetMapper(map);
		m->planet[p]->SetUserMatrix(m->planetM[p]);
		m->planet[p]->GetProperty()->SetColor(kSolColor[p][0], kSolColor[p][1], kSolColor[p][2]);
		m->planet[p]->GetProperty()->SetAmbient(0.10);    // the night side stays faintly readable
		m->planet[p]->GetProperty()->SetDiffuse(1.0);
		m->planet[p]->GetProperty()->SetSpecular(0.0);
		m->ren->AddActor(m->planet[p]);
		m->orbitPD[p] = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> om;
		om->SetInputData(m->orbitPD[p]);
		// The orbit runs through the body's centre and must go BEHIND its disc. VTK pulls lines towards
		// the eye by default (line offset units -4, which this app's polygon-offset mode applies); cancel
		// it for the orbits so plain depth decides.
		om->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, 4.0);
		m->orbit[p] = vtkSmartPointer<vtkActor>::New();
		m->orbit[p]->SetMapper(om);
		m->orbit[p]->GetProperty()->SetColor(0.55 * kSolColor[p][0], 0.55 * kSolColor[p][1], 0.55 * kSolColor[p][2]);
		m->orbit[p]->GetProperty()->SetLineWidth(1.5);
		m->orbit[p]->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->orbit[p]);
		m->label[p] = solLabel(solName(p), 13, kSolColor[p]);
		m->labRen->AddActor(m->label[p]);
	}
	// Saturn's rings: an annulus 1.24..2.27 planet radii (C ring inner edge to A ring outer edge).
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> quads;
		const int n = 120;
		for (int i = 0; i <= n; ++i) {
			const double t = 2.0 * vtkMath::Pi() * i / n;
			pts->InsertNextPoint(1.24 * std::cos(t), 1.24 * std::sin(t), 0.0);
			pts->InsertNextPoint(2.27 * std::cos(t), 2.27 * std::sin(t), 0.0);
		}
		for (int i = 0; i < n; ++i) {
			const vtkIdType q[4] = {2 * i, 2 * i + 1, 2 * i + 3, 2 * i + 2};
			quads->InsertNextCell(4, q);
		}
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetPolys(quads);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(pd);
		m->ringM = vtkSmartPointer<vtkMatrix4x4>::New();
		m->ring = vtkSmartPointer<vtkActor>::New();
		m->ring->SetMapper(map);
		m->ring->SetUserMatrix(m->ringM);
		m->ring->GetProperty()->SetColor(0.80, 0.72, 0.55);
		m->ring->GetProperty()->SetOpacity(0.75);
		m->ring->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->ring);
	}
	// The Moon (no name: it would race round the Earth at every Play frame).
	{
		vtkNew<vtkSphereSource> src;
		src->SetRadius(1.0);
		src->SetThetaResolution(32);
		src->SetPhiResolution(20);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		m->moonM = vtkSmartPointer<vtkMatrix4x4>::New();
		m->moon = vtkSmartPointer<vtkActor>::New();
		m->moon->SetMapper(map);
		m->moon->SetUserMatrix(m->moonM);
		m->moon->GetProperty()->SetColor(0.80, 0.80, 0.78);
		m->moon->GetProperty()->SetAmbient(0.10);
		m->ren->AddActor(m->moon);
	}
	// The March-equinox direction (ecliptic longitude 0), the zero every longitude in the table counts from.
	const double cEq[3] = {0.35, 0.85, 0.45};
	{
		m->eqSrc = vtkSmartPointer<vtkLineSource>::New();
		m->eqSrc->SetPoint1(kSolSunR, 0, 0);
		m->eqSrc->SetPoint2(20, 0, 0);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(m->eqSrc->GetOutputPort());
		m->equinox = vtkSmartPointer<vtkActor>::New();
		m->equinox->SetMapper(map);
		m->equinox->GetProperty()->SetColor(cEq[0], cEq[1], cEq[2]);
		m->equinox->GetProperty()->SetLineWidth(1.5);
		m->equinox->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->equinox);
		m->eqLbl = solLabel("♈ 0°", 13, cEq);
		m->labRen->AddActor(m->eqLbl);
	}

	// ---- the controls ----------------------------------------------------------------------------
	m->when = new QDateTimeEdit(dlg);                         // the instant shown (UTC)
	m->when->setTimeSpec(Qt::UTC);
	m->when->setDisplayFormat("yyyy-MM-dd HH:mm");
	m->when->setMinimumDateTime(QDateTime(QDate(100, 1, 1), QTime(0, 0), Qt::UTC));
	m->when->setMaximumDateTime(QDateTime(QDate(2999, 12, 31), QTime(23, 59), Qt::UTC));
	m->when->setDateTime(QDateTime::currentDateTimeUtc());
	m->when->setToolTip("The instant shown, in UTC (proleptic Gregorian calendar). The planetary elements "
	                    "are valid from 3000 BC to 3000 AD; the dwarf planets' from 1800 to 2200.");
	QPushButton *nowBtn = new QPushButton("Now", dlg);        // back to the present instant
	nowBtn->setToolTip("The present instant.");
	m->play = new QPushButton("Play", dlg);                   // run the clock
	m->play->setToolTip("Run time forward (or backward, with \"Backward\" ticked), one step per frame.");
	auto *stepLeft = new QToolButton(dlg);                    // one step back in time
	auto *stepRight = new QToolButton(dlg);                   // one step forward in time
	stepLeft->setArrowType(Qt::LeftArrow);
	stepRight->setArrowType(Qt::RightArrow);
	stepLeft->setAutoRepeat(true);
	stepRight->setAutoRepeat(true);
	stepLeft->setToolTip("One step back");
	stepRight->setToolTip("One step forward");
	m->stepBox = new QComboBox(dlg);                          // how much time one step (and one Play frame) is
	m->stepBox->addItems({"1 hour", "6 hours", "1 day", "1 week", "1 month", "1 year"});
	m->stepBox->setCurrentIndex(2);
	m->stepBox->setToolTip("Time advanced at each step and each Play frame.");
	m->backBox = new QCheckBox("Backward", dlg);              // Play runs time backwards
	m->backBox->setToolTip("Play runs time backwards.");
	m->scaleBox = new QComboBox(dlg);                         // how the distances from the Sun are drawn
	m->scaleBox->addItems({"Distances: true", "Distances: square root", "Distances: logarithmic",
	                       "Distances: Titius-Bode"});
	m->scaleBox->setCurrentIndex(1);
	m->scaleBox->setToolTip("How the distance from the Sun is drawn. The directions are always true. At true "
	                        "distances the inner planets crowd the Sun; the square root and the logarithm open "
	                        "them up. Titius-Bode moves each orbit to the law's distance, a = 0.4 + 0.3·2^n AU "
	                        "(Mercury n = −∞, Venus 0, Earth 1, Mars 2, Ceres 3, Jupiter 4 ... Pluto 8, Eris 9), "
	                        "drawn as the square root.");
	m->sizeBox = new QDoubleSpinBox(dlg);                     // drawn body size factor
	m->sizeBox->setRange(0.1, 8.0);
	m->sizeBox->setDecimals(1);
	m->sizeBox->setSingleStep(0.2);
	m->sizeBox->setValue(1.0);
	m->sizeBox->setPrefix("Size ×");
	m->sizeBox->setToolTip("Body size factor. Sizes are never to scale: they grow as the square root of "
	                       "the real radius.");
	m->orbitsBox = new QCheckBox("Orbits", dlg);              // show/hide the orbit lines
	m->orbitsBox->setChecked(true);
	m->labelsBox = new QCheckBox("Names", dlg);               // show/hide the body names
	m->labelsBox->setChecked(true);
	m->labelsBox->setToolTip("Show every name. Unticked, a body's name shows while the cursor is over it.");
	m->moonBox = new QCheckBox("Moon", dlg);                  // show/hide the Moon
	m->moonBox->setChecked(true);
	m->dwarfBox = new QCheckBox("Ceres && Eris", dlg);        // show/hide the dwarf planets Ceres and Eris
	m->dwarfBox->setChecked(true);
	m->dwarfBox->setToolTip("Show the dwarf planets Ceres and Eris, with their orbits.");
	m->apoBox = new QCheckBox("Apophis", dlg);                // show/hide the asteroid 99942 Apophis
	m->apoBox->setChecked(true);
	m->apoBox->setToolTip("Show the near-Earth asteroid 99942 Apophis and its orbit of the instant (JPL Horizons, "
	                      "1900-2100; no position outside). On 2029-04-13 21:46 UTC it passes 38 000 km from the "
	                      "Earth's centre, and that pass changes its orbit: step through April 2029 to see it.");
	QPushButton *innerBtn = new QPushButton("Inner planets", dlg);   // camera on Mercury..Mars
	QPushButton *allBtn = new QPushButton("All bodies", dlg);        // camera on the whole system
	QPushButton *topBtn = new QPushButton("From above", dlg);        // camera on the ecliptic pole
	innerBtn->setToolTip("Frame Mercury to Mars.");
	allBtn->setToolTip("Frame everything drawn, out to the farthest body.");
	topBtn->setToolTip("Look down from ecliptic north on the whole system.");
	for (QPushButton *b : {nowBtn, m->play, innerBtn, allBtn, topBtn}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
	m->info = new QLabel(dlg);                                // the numbers: distances, longitudes, magnitudes
	m->info->setTextFormat(Qt::RichText);
	m->info->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	m->info->setTextInteractionFlags(Qt::TextSelectableByMouse);
	// The legend: every line in the view, in its colour.
	auto entry = [](const double *c, const QString &t) {
		return QString("<span style='color:%1'>&#9632;</span>&nbsp;%2<br>")
			.arg(QColor::fromRgbF(c[0], c[1], c[2]).name()).arg(t);
	};
	const double cOrb[3] = {0.6, 0.6, 0.6};
	auto *legend = new QLabel(entry(cOrb, "Orbits: each in its body's colour") +
	                          entry(cEq, "♈ March-equinox direction (longitude 0°)") +
	                          "<br>Directions true; distances drawn as picked;<br>sizes not to scale.<br>"
	                          "Moon drawn at 3.5 Earth radii (drawn).", dlg);
	legend->setTextFormat(Qt::RichText);
	legend->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	auto *row1 = new QHBoxLayout;
	row1->addWidget(m->when);
	row1->addWidget(nowBtn);
	row1->addWidget(m->play);
	row1->addWidget(stepLeft);
	row1->addWidget(m->stepBox);
	row1->addWidget(stepRight);
	row1->addWidget(m->backBox);
	row1->addStretch(1);
	auto *row2 = new QHBoxLayout;
	row2->addWidget(m->scaleBox);
	row2->addWidget(m->sizeBox);
	row2->addWidget(m->orbitsBox);
	row2->addWidget(m->labelsBox);
	row2->addWidget(m->moonBox);
	row2->addWidget(m->dwarfBox);
	row2->addWidget(m->apoBox);
	row2->addStretch(1);
	row2->addWidget(innerBtn);
	row2->addWidget(allBtn);
	row2->addWidget(topBtn);
	// The right-hand panel (table + legend), foldable by the arrow button on its edge.
	m->side = new QWidget(dlg);
	auto *sideLay = new QVBoxLayout(m->side);
	sideLay->setContentsMargins(0, 0, 0, 0);
	sideLay->addWidget(m->info);
	sideLay->addSpacing(10);
	sideLay->addWidget(legend);
	sideLay->addStretch(1);
	m->sideBtn = new QToolButton(dlg);                        // fold / unfold the right-hand panel
	m->sideBtn->setAutoRaise(true);
	auto *edge = new QVBoxLayout;
	edge->setContentsMargins(0, 0, 0, 0);
	edge->addWidget(m->sideBtn);
	edge->addStretch(1);
	auto *top = new QHBoxLayout;
	top->addWidget(m->view, 1);
	top->addLayout(edge);
	top->addWidget(m->side);
	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->addLayout(top, 1);
	lay->addLayout(row1);
	lay->addLayout(row2);
	{
		QScreen *scr = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
		const QRect av = scr->availableGeometry();
		const int w = std::min(1250, int(av.width() * 0.9)), h = std::min(820, int(av.height() * 0.85));
		dlg->resize(w, h);
		dlg->move(av.x() + (av.width() - w) / 2, av.y() + (av.height() - h - 30) / 2);
	}

	m->timer = new QTimer(dlg);
	m->timer->setInterval(40);
	QObject::connect(m->timer, &QTimer::timeout, dlg, [m]() { m->tick(); });
	QObject::connect(m->play, &QPushButton::clicked, dlg, [m]() {
		if (m->timer->isActive()) {
			m->stopPlay();
			return;
		}
		m->play->setText("Pause");
		m->timer->start();
	});
	QObject::connect(m->when, &QDateTimeEdit::dateTimeChanged, dlg, [m](const QDateTime &t) {
		m->jd = ephJD(t);
		m->update();
	});
	QObject::connect(nowBtn, &QPushButton::clicked, dlg, [m]() { m->setJD(ephJD(QDateTime::currentDateTimeUtc())); });
	QObject::connect(stepLeft, &QToolButton::clicked, dlg, [m]() { m->setJD(m->jd - m->stepDays()); });
	QObject::connect(stepRight, &QToolButton::clicked, dlg, [m]() { m->setJD(m->jd + m->stepDays()); });
	QObject::connect(m->scaleBox, &QComboBox::currentIndexChanged, dlg, [m](int) {
		m->update();
		m->frame(m->outerAU(), false);
	});
	QObject::connect(m->sizeBox, &QDoubleSpinBox::valueChanged, dlg, [m](double) { m->update(); });
	for (QCheckBox *b : {m->orbitsBox, m->labelsBox, m->moonBox, m->dwarfBox, m->apoBox})
		QObject::connect(b, &QCheckBox::toggled, dlg, [m](bool) {
			m->hovered = -1;
			m->applyVisibility();
			m->view->renderWindow()->Render();
		});
	m->view->onHover = [m](const QPoint &p, bool inside) { m->hover(p, inside); };
	QObject::connect(m->sideBtn, &QToolButton::clicked, dlg, [m]() { m->setSide(!m->side->isVisible()); });
	QObject::connect(innerBtn, &QPushButton::clicked, dlg, [m]() { m->frame(1.7, false); });
	QObject::connect(allBtn, &QPushButton::clicked, dlg, [m]() { m->frame(m->outerAU(), false); });
	QObject::connect(topBtn, &QPushButton::clicked, dlg, [m]() { m->frame(m->outerAU(), true); });

	// PARKED LIKE EVERY TOOL WINDOW (parkTool / parkOnMinimise, 50_scene.cpp).
	struct CloseParks : QObject {
		SolarSysDialog *d;
		CloseParks(QObject *p, SolarSysDialog *dd) : QObject(p), d(dd) {}
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
		if (g_solarSys == m) g_solarSys = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	wrapTooltips(dlg);
	g_solarSys = m;
	m->setSide(true);
	m->jd = ephJD(m->when->dateTime());
	m->update();
	m->frame(1.7, false);
	dlg->show();
	QApplication::processEvents();
	// The Earth's picture last (it may have to come from the GMT data server); the demo already runs.
	QString error;
	QApplication::setOverrideCursor(Qt::WaitCursor);
	auto tex = magFieldTexture(0, 2026.0, error);
	QApplication::restoreOverrideCursor();
	if (tex) {
		m->planet[EPH_EMB]->SetTexture(tex);
		m->planet[EPH_EMB]->GetProperty()->SetColor(1.0, 1.0, 1.0);
		m->view->renderWindow()->Render();
	}
	return true;
}
