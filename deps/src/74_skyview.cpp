// 74_skyview.cpp -- T & L > "Sky viewer".
//
// The sky seen from a place on the Earth at any instant from 100 AD to 2999 AD: the 9110 stars of the
// Yale Bright Star Catalog, the constellation figures and names, the Sun, the Moon (lit from the Sun's
// direction, so its phase is the real one) and the planets, with the ground, the horizon and the
// cardinal points. The viewer stands at the centre of the celestial sphere: drag to look around, wheel
// to zoom, click a star or a body for what it is. Every position comes from 74_ephem.cpp -- the same
// numbers as the "Solar system" demo -- the idea, the catalogue and the figures from astroterm
// (https://github.com/da-luce/astroterm), drawn here in VTK instead of in a terminal.
//
// FRAMES: the WORLD is the observer's horizon: +X east, +Y north, +Z the zenith, the eye at the origin.
// Stars, figures, the RA/Dec grid and the ecliptic are built ONCE in the J2000 equatorial frame and
// carried into the world by ONE matrix (eqM = horizon <- of-date <- J2000: the precession of the date,
// then the turn of the local sidereal time and the tilt of the latitude), so a step of time moves the
// whole sky by changing sixteen numbers. The bodies move against the stars, so they are placed each step.
//
// NOT MODELLED: atmospheric refraction (it lifts the horizon by ~0.5 deg), nutation, aberration and
// light time (all under a minute of arc). The Moon IS topocentric: its ~1 deg of parallax is taken out
// for the observer's place.

static const double kSkyR = 100.0;         // the stars
static const double kSkyLineR = 99.5;      // figures and grids, just inside the stars
static const double kSkySunR = 95.0;       // the Sun, the planets and the Moon: in front of the stars,
static const double kSkyPlanetR = 94.0;    // ...the Moon nearest (it eclipses the Sun, it occults planets)
static const double kSkyMoonR = 93.0;
static const double kSkyGroundR = 90.0;    // the ground: in front of everything below the horizon

static const int kSkyBins = 8;             // stars by magnitude: <0, 0-1, ..., 5-6, >= 6
static const float kSkyBinPx[kSkyBins] = {8.5f, 7.0f, 5.6f, 4.5f, 3.6f, 2.9f, 2.3f, 1.8f};
static int skyBin(float mag) { return std::clamp(int(std::floor(mag)) + 1, 0, kSkyBins - 1); }

// A star's colour from its spectral class (blue O/B .. red M).
static void skySpColor(char sp, unsigned char c[3]) {
	switch (sp) {
	case 'O': c[0] = 155; c[1] = 176; c[2] = 255; break;
	case 'B': c[0] = 170; c[1] = 191; c[2] = 255; break;
	case 'A': c[0] = 202; c[1] = 215; c[2] = 255; break;
	case 'F': c[0] = 248; c[1] = 247; c[2] = 255; break;
	case 'G': c[0] = 255; c[1] = 244; c[2] = 234; break;
	case 'K': c[0] = 255; c[1] = 210; c[2] = 161; break;
	case 'M': case 'N': case 'R': case 'S': case 'C': c[0] = 255; c[1] = 190; c[2] = 120; break;
	default: c[0] = 235; c[1] = 235; c[2] = 235; break;
	}
}

// Each planet's dot colour in the sky (Mercury .. Neptune, SKY_ order from SKY_MERCURY).
static const double kSkyPlanetColor[7][3] = {
	{0.80, 0.78, 0.75}, {1.00, 0.97, 0.85}, {1.00, 0.55, 0.35}, {1.00, 0.92, 0.75},
	{1.00, 0.88, 0.60}, {0.65, 0.90, 0.95}, {0.55, 0.65, 1.00}};

// The lines of the view, and their legend.
static const double kSkyCFig[3]  = {0.40, 0.58, 0.90};   // constellation figures
static const double kSkyCEq[3]   = {0.30, 0.33, 0.55};   // RA/Dec grid
static const double kSkyCAz[3]   = {0.30, 0.55, 0.32};   // Alt/Az grid
static const double kSkyCEcl[3]  = {0.90, 0.66, 0.25};   // ecliptic
static const double kSkyCHor[3]  = {0.85, 0.50, 0.20};   // horizon

// The view: the eye at the centre. Left-drag turns the gaze (the zenith stays up), the wheel zooms,
// a click without a drag asks what is there, and the bare cursor reads its sky coordinates. Nothing is
// forwarded to VTK's interactor: the camera never leaves the centre.
class SkyView : public GLView {
public:
	std::function<void(double, double)> onDrag;      // dx, dy in logical pixels
	std::function<void(double)> onZoom;              // factor on the field of view
	std::function<void(const QPoint &)> onClick, onHover;
	SkyView(QWidget *parent) : GLView() {
		setParent(parent);
		setMouseTracking(true);
	}
protected:
	bool down = false, moved = false;
	QPoint press, last;
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			down = true;
			moved = false;
			press = last = e->position().toPoint();
			setCursor(Qt::ClosedHandCursor);
		}
		e->accept();
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		const QPoint p = e->position().toPoint();
		if (down) {
			if ((p - press).manhattanLength() > 3) moved = true;
			if (moved && onDrag) onDrag(p.x() - last.x(), p.y() - last.y());
			last = p;
		}
		else if (onHover) onHover(p);
		e->accept();
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && down) {
			down = false;
			unsetCursor();
			if (!moved && onClick) onClick(e->position().toPoint());
		}
		e->accept();
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override { e->accept(); }
	void wheelEvent(QWheelEvent *e) override {
		if (onZoom) onZoom(e->angleDelta().y() > 0 ? 1.0 / 1.15 : 1.15);
		e->accept();
	}
};

struct SkyViewDialog {
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	SkyView *view = nullptr;
	QComboBox *place = nullptr, *stepBox = nullptr;
	QDoubleSpinBox *lat = nullptr, *lon = nullptr, *magLim = nullptr, *bodySize = nullptr;
	QDateTimeEdit *when = nullptr;
	QPushButton *play = nullptr;
	QCheckBox *backBox = nullptr, *figBox = nullptr, *figNameBox = nullptr, *starNameBox = nullptr,
	          *planetBox = nullptr, *azGridBox = nullptr, *eqGridBox = nullptr, *eclBox = nullptr,
	          *groundBox = nullptr, *dayBox = nullptr;
	QLabel *timeLbl = nullptr, *bodiesLbl = nullptr, *pickLbl = nullptr, *hoverLbl = nullptr;
	QTimer *timer = nullptr;
	bool parked = false;
	bool reallyClose = false;

	double jd = 2451545.0;                   // the instant shown (Julian Date, UT)
	double az = 180.0, alt = 30.0, fov = 90.0;   // the gaze (deg) and the vertical field of view
	double M[3][3];                          // world (horizon) <- J2000 equatorial
	double builtYears = 1e9;                 // proper-motion epoch the star geometry was built for
	double builtLimit = 1e9;                 // magnitude limit the star geometry was built for
	double effLimit = 6.5;                   // the limit in force (the user's, or less in daylight)
	double sunAlt = -90.0;
	int nShown = 0;                          // stars drawn
	double bodyW[SKY_NBODIES][3];            // each body's world direction (unit)
	double bodyMag[SKY_NBODIES];
	double moonIllum = 0.0;
	std::vector<double> starU;               // every star's J2000 unit vector at builtYears (x,y,z)

	vtkSmartPointer<vtkRenderer> ren, labRen;
	vtkSmartPointer<vtkMatrix4x4> eqM;
	vtkSmartPointer<vtkPolyData> starPD[kSkyBins], figPD;
	vtkSmartPointer<vtkActor> stars[kSkyBins], figs, eqGrid, ecl, azGrid, horizon, ground, sun, moon;
	vtkSmartPointer<vtkActor> planet[7];
	vtkSmartPointer<vtkPoints> planetPt[7];
	vtkSmartPointer<vtkSphereSource> sunSrc, moonSrc;
	vtkSmartPointer<vtkLight> moonLight;
	std::vector<vtkSmartPointer<vtkBillboardTextActor3D>> starLbl, figLbl, cardLbl;
	std::vector<int> starLblIdx;             // catalogue index of each star label
	std::vector<std::array<double, 3>> figU; // each figure's label direction (J2000)
	vtkSmartPointer<vtkBillboardTextActor3D> bodyLbl[SKY_NBODIES];

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

	void toWorld(const double q[3], double w[3]) const {
		for (int i = 0; i < 3; ++i) w[i] = M[i][0] * q[0] + M[i][1] * q[1] + M[i][2] * q[2];
	}
	void toJ2000(const double w[3], double q[3]) const {
		for (int i = 0; i < 3; ++i) q[i] = M[0][i] * w[0] + M[1][i] * w[1] + M[2][i] * w[2];
	}
	static void altAz(const double w[3], double &a, double &z) {
		const double n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		a = std::asin(std::clamp(w[2] / n, -1.0, 1.0)) / kEphRad;
		z = std::atan2(w[0], w[1]) / kEphRad;
		if (z < 0) z += 360.0;
	}
	static void raDec(const double q[3], double &ra, double &de) {
		const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
		de = std::asin(std::clamp(q[2] / n, -1.0, 1.0)) / kEphRad;
		ra = std::atan2(q[1], q[0]) / kEphRad;
		if (ra < 0) ra += 360.0;
	}
	double lst() const {
		double l = std::fmod(ephGMST(jd) + lon->value(), 360.0);
		return l < 0 ? l + 360.0 : l;
	}

	// eqM: horizon <- of date (sidereal time, latitude) <- J2000 (precession).
	void buildMatrix(double tt) {
		double P[3][3];
		ephPrecession(tt, P);
		const double L = lst() * kEphRad, f = lat->value() * kEphRad;
		const double H[3][3] = {
			{-std::sin(L), std::cos(L), 0.0},
			{-std::sin(f) * std::cos(L), -std::sin(f) * std::sin(L), std::cos(f)},
			{std::cos(f) * std::cos(L), std::cos(f) * std::sin(L), std::sin(f)}};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) M[i][j] = H[i][0] * P[0][j] + H[i][1] * P[1][j] + H[i][2] * P[2][j];
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) eqM->SetElement(i, j, M[i][j]);
			eqM->SetElement(i, 3, 0.0);
		}
		eqM->Modified();
	}

	// The stars brighter than the limit in force, and the constellation figures through them, for the
	// proper motions of `years` from J2000. Rebuilt only when the epoch moves a year or the limit moves.
	void buildStars(double years) {
		const EphCatalog &c = ephCatalog();
		starU.resize(c.stars.size() * 3);
		for (size_t i = 0; i < c.stars.size(); ++i) ephStarVec(c.stars[i], years, &starU[3 * i]);
		vtkSmartPointer<vtkPoints> pts[kSkyBins];
		vtkSmartPointer<vtkUnsignedCharArray> rgb[kSkyBins];
		vtkSmartPointer<vtkCellArray> verts[kSkyBins];
		for (int b = 0; b < kSkyBins; ++b) {
			pts[b] = vtkSmartPointer<vtkPoints>::New();
			rgb[b] = vtkSmartPointer<vtkUnsignedCharArray>::New();
			rgb[b]->SetNumberOfComponents(3);
			verts[b] = vtkSmartPointer<vtkCellArray>::New();
		}
		nShown = 0;
		for (size_t i = 0; i < c.stars.size(); ++i) {
			const EphStar &s = c.stars[i];
			if (s.mag > effLimit) continue;
			const int b = skyBin(s.mag);
			unsigned char col[3];
			skySpColor(s.sp, col);
			const vtkIdType id = pts[b]->InsertNextPoint(kSkyR * starU[3 * i], kSkyR * starU[3 * i + 1],
			                                             kSkyR * starU[3 * i + 2]);
			verts[b]->InsertNextCell(1, &id);
			rgb[b]->InsertNextTypedTuple(col);
			++nShown;
		}
		for (int b = 0; b < kSkyBins; ++b) {
			starPD[b]->SetPoints(pts[b]);
			starPD[b]->SetVerts(verts[b]);
			starPD[b]->GetPointData()->SetScalars(rgb[b]);
			starPD[b]->Modified();
		}
		if (std::fabs(years - builtYears) > 0.5) {
			vtkNew<vtkPoints> fp;
			vtkNew<vtkCellArray> fl;
			for (const EphConstellation &k : c.constellations)
				for (const auto &sg : k.segs) {
					if (sg.first <= 0 || sg.second <= 0 || sg.first >= int(c.byHr.size()) ||
					    sg.second >= int(c.byHr.size()))
						continue;
					const int a = c.byHr[size_t(sg.first)], b = c.byHr[size_t(sg.second)];
					if (a < 0 || b < 0) continue;
					const vtkIdType ids[2] = {
						fp->InsertNextPoint(kSkyLineR * starU[3 * a], kSkyLineR * starU[3 * a + 1], kSkyLineR * starU[3 * a + 2]),
						fp->InsertNextPoint(kSkyLineR * starU[3 * b], kSkyLineR * starU[3 * b + 1], kSkyLineR * starU[3 * b + 2])};
					fl->InsertNextCell(2, ids);
				}
			figPD->SetPoints(fp);
			figPD->SetLines(fl);
			figPD->Modified();
		}
		builtYears = years;
		builtLimit = effLimit;
	}

	// The limit in force: the user's, or what the twilight leaves visible when "Daylight" is on.
	double daylightLimit() const {
		if (!dayBox->isChecked()) return magLim->value();
		const double h = sunAlt;
		double l;
		if (h >= 0.0) l = -2.0;
		else if (h >= -6.0) l = -2.0 + 3.0 * (-h / 6.0);
		else if (h >= -12.0) l = 1.0 + 3.0 * ((-h - 6.0) / 6.0);
		else if (h >= -18.0) l = 4.0 + (magLim->value() - 4.0) * ((-h - 12.0) / 6.0);
		else l = magLim->value();
		return std::min(l, magLim->value());
	}

	// Sky and ground colour from the Sun's altitude: day, the three twilights, night.
	void paintSky() {
		static const double day[3] = {0.30, 0.50, 0.80}, civ[3] = {0.10, 0.15, 0.35}, nau[3] = {0.03, 0.04, 0.12},
		                    night[3] = {0.005, 0.005, 0.02};
		double c[3];
		auto mix = [&](const double *a, const double *b, double t) {
			for (int i = 0; i < 3; ++i) c[i] = a[i] + (b[i] - a[i]) * std::clamp(t, 0.0, 1.0);
		};
		const double h = dayBox->isChecked() ? sunAlt : -90.0;
		if (h >= 0.0) mix(day, day, 0);
		else if (h >= -6.0) mix(day, civ, -h / 6.0);
		else if (h >= -12.0) mix(civ, nau, (-h - 6.0) / 6.0);
		else mix(nau, night, (-h - 12.0) / 6.0);
		ren->SetBackground(c);
		const double gd[3] = {0.20, 0.24, 0.15}, gn[3] = {0.035, 0.045, 0.035};
		const double t = std::clamp((h + 12.0) / 12.0, 0.0, 1.0);
		ground->GetProperty()->SetColor(gn[0] + (gd[0] - gn[0]) * t, gn[1] + (gd[1] - gn[1]) * t,
		                                gn[2] + (gd[2] - gn[2]) * t);
	}

	// A body, placed at its world direction and drawn distance.
	void placeBodies(double tt) {
		// Sun and planets: geocentric (the parallax is seconds of arc).
		for (int b = 0; b < SKY_NBODIES; ++b) {
			double q[3];
			ephGeocentric(b, tt, q);
			if (b == SKY_MOON) {
				// Topocentric: the observer's place on the Earth taken out (up to ~1 deg).
				double P[3][3];
				ephPrecession(tt, P);
				const double L = lst() * kEphRad, f = lat->value() * kEphRad;
				const double od[3] = {std::cos(f) * std::cos(L), std::cos(f) * std::sin(L), std::sin(f)};
				for (int i = 0; i < 3; ++i)
					q[i] -= kEphEarthRkm / kEphAUkm * (P[0][i] * od[0] + P[1][i] * od[1] + P[2][i] * od[2]);
			}
			double w[3];
			toWorld(q, w);
			vtkMath::Normalize(w);
			for (int i = 0; i < 3; ++i) bodyW[b][i] = w[i];
		}
		// Magnitudes and the Moon's phase.
		double sq[3], mq[3], earth[3];
		ephGeocentric(SKY_SUN, tt, sq);
		ephGeocentric(SKY_MOON, tt, mq);
		ephPlanetHelio(EPH_EMB, tt, earth);
		const double rE = std::sqrt(earth[0] * earth[0] + earth[1] * earth[1] + earth[2] * earth[2]);
		bodyMag[SKY_SUN] = -26.74;
		{
			const double ce = std::clamp((sq[0] * mq[0] + sq[1] * mq[1] + sq[2] * mq[2]) /
			                             (std::sqrt(sq[0] * sq[0] + sq[1] * sq[1] + sq[2] * sq[2]) *
			                              std::sqrt(mq[0] * mq[0] + mq[1] * mq[1] + mq[2] * mq[2])), -1.0, 1.0);
			moonIllum = 0.5 * (1.0 - ce);
			const double i = 180.0 - std::acos(ce) / kEphRad;        // phase angle ~ 180 - elongation
			bodyMag[SKY_MOON] = -12.73 + 0.026 * i + 4e-9 * std::pow(i, 4.0);
		}
		for (int b = SKY_MERCURY; b < SKY_NBODIES; ++b) {
			const int p = ephSkyPlanet(b);
			double h[3], g[3];
			ephPlanetHelio(p, tt, h);
			ephGeocentric(b, tt, g);
			const double r = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
			const double d = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
			const double ci = std::clamp((r * r + d * d - rE * rE) / (2.0 * r * d), -1.0, 1.0);
			bodyMag[b] = ephPlanetMag(p, r, d, std::acos(ci) / kEphRad);
		}
		double z;
		altAz(bodyW[SKY_SUN], sunAlt, z);

		// Sun and Moon: discs of their true angular size (Sun 0.2666 deg, Moon from its distance) times
		// the "Sun & Moon" factor.
		const double k = bodySize->value();
		double l, be, dkm;
		ephMoon(tt, l, be, dkm);
		sunSrc->SetRadius(kSkySunR * std::tan(0.2666 * k * kEphRad));
		sunSrc->SetCenter(kSkySunR * bodyW[SKY_SUN][0], kSkySunR * bodyW[SKY_SUN][1], kSkySunR * bodyW[SKY_SUN][2]);
		moonSrc->SetRadius(kSkyMoonR * std::tan(std::asin(1737.4 / dkm) * k));
		moonSrc->SetCenter(kSkyMoonR * bodyW[SKY_MOON][0], kSkyMoonR * bodyW[SKY_MOON][1],
		                   kSkyMoonR * bodyW[SKY_MOON][2]);
		// The Moon's only light comes from the Sun's direction: its lit side and phase are the real ones.
		moonLight->SetPosition(1000.0 * bodyW[SKY_SUN][0], 1000.0 * bodyW[SKY_SUN][1], 1000.0 * bodyW[SKY_SUN][2]);
		moonLight->SetFocalPoint(0, 0, 0);
		for (int b = SKY_MERCURY; b < SKY_NBODIES; ++b) {
			const int j = b - SKY_MERCURY;
			planetPt[j]->SetPoint(0, kSkyPlanetR * bodyW[b][0], kSkyPlanetR * bodyW[b][1], kSkyPlanetR * bodyW[b][2]);
			planetPt[j]->Modified();
			planet[j]->GetProperty()->SetPointSize(float(std::clamp(8.5 - 1.1 * bodyMag[b], 3.0, 12.0)));
		}
		for (int b = 0; b < SKY_NBODIES; ++b) {
			const double r = b == SKY_SUN ? kSkySunR : (b == SKY_MOON ? kSkyMoonR : kSkyPlanetR);
			bodyLbl[b]->SetPosition(r * bodyW[b][0], r * bodyW[b][1], r * bodyW[b][2]);
		}
	}

	// Labels: star names by zoom (brighter as the view widens), figure names, body names, all hidden
	// below the horizon while the ground is drawn (the overlay renderer has no depth to hide them).
	void placeLabels() {
		const EphCatalog &c = ephCatalog();
		const bool gnd = groundBox->isChecked();
		const double nameLim = std::min(effLimit, std::clamp(1.0 + 2.5 * std::log2(100.0 / fov), -1.0, 6.5));
		for (size_t k = 0; k < starLbl.size(); ++k) {
			const int i = starLblIdx[k];
			bool on = starNameBox->isChecked() && c.stars[size_t(i)].mag <= nameLim;
			if (on) {
				double w[3];
				toWorld(&starU[3 * size_t(i)], w);
				on = !gnd || w[2] > 0.0;
				starLbl[k]->SetPosition(kSkyR * w[0], kSkyR * w[1], kSkyR * w[2]);
			}
			starLbl[k]->SetVisibility(on);
		}
		for (size_t k = 0; k < figLbl.size(); ++k) {
			bool on = figNameBox->isChecked();
			if (on) {
				double w[3];
				toWorld(figU[k].data(), w);
				on = !gnd || w[2] > 0.0;
				figLbl[k]->SetPosition(kSkyLineR * w[0], kSkyLineR * w[1], kSkyLineR * w[2]);
			}
			figLbl[k]->SetVisibility(on);
		}
		for (int b = 0; b < SKY_NBODIES; ++b)
			bodyLbl[b]->SetVisibility(planetBox->isChecked() && (!gnd || bodyW[b][2] > 0.0));
	}

	// Put the sky at the instant `jd` for the place on the spin boxes.
	void update() {
		const double tt = ephTT(jd);
		buildMatrix(tt);
		placeBodies(tt);
		effLimit = daylightLimit();
		const double years = (tt - 2451545.0) / 365.25;
		if (std::fabs(years - builtYears) > 0.5 || std::fabs(effLimit - builtLimit) > 0.05) buildStars(years);
		paintSky();
		placeLabels();
		updateText();
		view->renderWindow()->Render();
	}

	void setCamera() {
		alt = std::clamp(alt, -89.5, 89.5);
		az = std::fmod(az, 360.0);
		if (az < 0) az += 360.0;
		fov = std::clamp(fov, 2.0, 120.0);
		vtkCamera *cam = ren->GetActiveCamera();
		const double a = alt * kEphRad, z = az * kEphRad;
		cam->SetPosition(0, 0, 0);
		cam->SetFocalPoint(std::cos(a) * std::sin(z), std::cos(a) * std::cos(z), std::sin(a));
		cam->SetViewUp(0, 0, 1);
		cam->SetViewAngle(fov);
		cam->SetClippingRange(1.0, 400.0);
		placeLabels();
		view->renderWindow()->Render();
	}

	// The panel's text: the clock, and where each body stands (a link turns the gaze onto it).
	void updateText() {
		const double y = 2000.0 + (jd - 2451545.0) / 365.25;
		const double L = lst() / 15.0;
		timeLbl->setText(QString("<b>%1 UTC</b><br>Local sidereal time %2h %3m<br>TT − UT = %4 s<br>"
		                         "Stars shown: %5 (mag ≤ %6)")
			.arg(ephDateTime(jd).toString("yyyy-MM-dd HH:mm")).arg(int(L)).arg(int((L - int(L)) * 60.0), 2, 10, QChar('0'))
			.arg(ephDeltaT(y), 0, 'f', 0).arg(nShown).arg(effLimit, 0, 'f', 1));
		QString h = "<table cellspacing=0 cellpadding=2><tr><th align=left></th><th align=right>Alt</th>"
		            "<th align=right>Az</th><th align=right>Mag</th></tr>";
		for (int b = 0; b < SKY_NBODIES; ++b) {
			double a, z;
			altAz(bodyW[b], a, z);
			const QString col = a > 0 ? "" : " style='color:#888'";
			h += QString("<tr%1><td><a href='%2'>%3</a></td><td align=right>%4°</td><td align=right>%5°</td>"
			             "<td align=right>%6</td></tr>")
				.arg(col).arg(b).arg(kSkyBodyName[b]).arg(a, 0, 'f', 1).arg(z, 0, 'f', 1).arg(bodyMag[b], 0, 'f', 1);
		}
		h += QString("</table><br>Moon %1% lit").arg(100.0 * moonIllum, 0, 'f', 0);
		bodiesLbl->setText(h);
	}

	// The world direction under a widget point.
	bool dirAt(const QPoint &p, double w[3]) {
		double dx, dy;
		displayPxFromQt(view, view->renderWindow(), p, dx, dy);
		ren->SetDisplayPoint(dx, dy, 0.5);
		ren->DisplayToWorld();
		double wp[4];
		ren->GetWorldPoint(wp);
		if (wp[3] == 0.0) return false;
		for (int i = 0; i < 3; ++i) w[i] = wp[i] / wp[3];
		return vtkMath::Normalize(w) > 0.0;
	}
	// Display position (VTK px) of a world direction; false when it is behind the eye.
	bool screenOf(const double w[3], double &x, double &y) {
		vtkCamera *cam = ren->GetActiveCamera();
		double f[3];
		cam->GetDirectionOfProjection(f);
		if (f[0] * w[0] + f[1] * w[1] + f[2] * w[2] <= 0.0) return false;
		ren->SetWorldPoint(50.0 * w[0], 50.0 * w[1], 50.0 * w[2], 1.0);
		ren->WorldToDisplay();
		double d[3];
		ren->GetDisplayPoint(d);
		x = d[0];
		y = d[1];
		return true;
	}

	void hover(const QPoint &p) {
		double w[3], q[3], a, z, ra, de;
		if (!dirAt(p, w)) return;
		altAz(w, a, z);
		toJ2000(w, q);
		raDec(q, ra, de);
		hoverLbl->setText(QString("Alt %1°  Az %2°   RA %3  Dec %4 (J2000)").arg(a, 0, 'f', 1).arg(z, 0, 'f', 1)
		                  .arg(ephRaText(ra)).arg(ephDecText(de)));
	}

	// What is under a click: a body within 12 px first, else the nearest drawn star within 8 px.
	void identify(const QPoint &p) {
		double dx, dy;
		displayPxFromQt(view, view->renderWindow(), p, dx, dy);
		const bool gnd = groundBox->isChecked();
		int best = -1;
		double bestD = 12.0 * view->devicePixelRatioF();
		for (int b = 0; b < SKY_NBODIES; ++b) {
			if (b >= SKY_MERCURY && !planetBox->isChecked()) continue;
			if (gnd && bodyW[b][2] < 0.0) continue;
			double x, y;
			if (!screenOf(bodyW[b], x, y)) continue;
			const double d = std::hypot(x - dx, y - dy);
			if (d < bestD) {
				bestD = d;
				best = b;
			}
		}
		if (best >= 0) {
			pickLbl->setText(bodyText(best));
			return;
		}
		const EphCatalog &c = ephCatalog();
		int star = -1;
		bestD = 8.0 * view->devicePixelRatioF();
		for (size_t i = 0; i < c.stars.size(); ++i) {
			if (c.stars[i].mag > effLimit) continue;
			double w[3], x, y;
			toWorld(&starU[3 * i], w);
			if (gnd && w[2] < 0.0) continue;
			if (!screenOf(w, x, y)) continue;
			const double d = std::hypot(x - dx, y - dy);
			if (d < bestD) {
				bestD = d;
				star = int(i);
			}
		}
		pickLbl->setText(star >= 0 ? starText(star) : QString());
	}

	QString posText(const double w[3]) const {
		double q[3], a, z, ra, de;
		toJ2000(w, q);
		raDec(q, ra, de);
		altAz(w, a, z);
		return QString("RA %1, Dec %2 (J2000)<br>Alt %3°, Az %4°").arg(ephRaText(ra)).arg(ephDecText(de))
			.arg(a, 0, 'f', 1).arg(z, 0, 'f', 1);
	}
	QString bodyText(int b) const {
		QString t = QString("<b>%1</b><br>magnitude %2<br>").arg(kSkyBodyName[b]).arg(bodyMag[b], 0, 'f', 1);
		const double tt = ephTT(jd);
		double g[3];
		ephGeocentric(b, tt, g);
		const double d = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
		if (b == SKY_MOON) t += QString("%1 km away, %2% lit<br>").arg(d * kEphAUkm, 0, 'f', 0).arg(100.0 * moonIllum, 0, 'f', 0);
		else t += QString("%1 AU away (light: %2 min)<br>").arg(d, 0, 'f', 3).arg(d * kEphAUkm / 299792.458 / 60.0, 0, 'f', 1);
		return t + posText(bodyW[b]);
	}
	QString starText(int i) const {
		const EphCatalog &c = ephCatalog();
		const EphStar &s = c.stars[size_t(i)];
		auto nm = c.names.find(s.hr);
		QString t = nm != c.names.end() ? QString("<b>%1</b> (HR %2)").arg(nm->second).arg(s.hr)
		                                : QString("<b>HR %1</b>").arg(s.hr);
		t += QString("<br>magnitude %1, spectral class %2<br>").arg(s.mag, 0, 'f', 2).arg(QChar(s.sp));
		for (const EphConstellation &k : c.constellations) {
			bool in = false;
			for (const auto &sg : k.segs) in = in || sg.first == s.hr || sg.second == s.hr;
			if (in) {
				t += "in the figure of " + k.name + "<br>";
				break;
			}
		}
		double w[3];
		toWorld(&starU[3 * size_t(i)], w);
		return t + posText(w);
	}

	// Turn the gaze onto body `b`.
	void lookAt(int b) {
		altAz(bodyW[b], alt, az);
		setCamera();
		pickLbl->setText(bodyText(b));
	}

	void stopPlay() {
		if (timer && timer->isActive()) timer->stop();
		if (play) play->setText("Play");
	}

	double stepDays() const {
		static const double d[] = {1.0 / 1440.0, 10.0 / 1440.0, 1.0 / 24.0, 0.99726957, 1.0, 7.0, 30.4375, 365.25};
		return d[std::clamp(stepBox->currentIndex(), 0, 7)];
	}

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

	void savePlace() {
		QSettings st = igmtSettings();
		st.setValue("sky/lat", lat->value());
		st.setValue("sky/lon", lon->value());
		st.setValue("sky/place", place->currentText());
	}
};

static SkyViewDialog *g_skyView = nullptr;

// A billboard label for the sky (overlay renderer), nudged `dy` pixels up off its point.
static vtkSmartPointer<vtkBillboardTextActor3D> skyLabel(const QString &txt, int size, const double rgb[3],
                                                         int dy = 0, bool italic = false) {
	auto a = vtkSmartPointer<vtkBillboardTextActor3D>::New();
	a->SetInput(txt.toUtf8().constData());
	a->GetTextProperty()->SetFontSize(size);
	a->GetTextProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
	a->GetTextProperty()->SetJustificationToCentered();
	a->GetTextProperty()->SetItalic(italic);
	a->SetDisplayOffset(0, dy);
	return a;
}

// A polyline actor (unlit), points from a vector of xyz triples, one cell per run.
static vtkSmartPointer<vtkActor> skyLines(const std::vector<std::vector<std::array<double, 3>>> &runs,
                                          const double rgb[3], double width, vtkMatrix4x4 *m = nullptr) {
	vtkNew<vtkPoints> pts;
	vtkNew<vtkCellArray> cells;
	for (const auto &r : runs) {
		std::vector<vtkIdType> ids;
		for (const auto &p : r) ids.push_back(pts->InsertNextPoint(p.data()));
		cells->InsertNextCell(vtkIdType(ids.size()), ids.data());
	}
	vtkNew<vtkPolyData> pd;
	pd->SetPoints(pts);
	pd->SetLines(cells);
	vtkNew<vtkPolyDataMapper> map;
	map->SetInputData(pd);
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
	a->GetProperty()->SetLineWidth(float(width));
	a->GetProperty()->SetLighting(false);
	if (m) a->SetUserMatrix(m);
	return a;
}

// A point (lon, lat in deg) on a sphere of radius r: x towards lon 0, z towards lat 90.
static std::array<double, 3> skySph(double lonDeg, double latDeg, double r) {
	const double l = lonDeg * kEphRad, b = latDeg * kEphRad;
	return {r * std::cos(b) * std::cos(l), r * std::cos(b) * std::sin(l), r * std::sin(b)};
}

// Open (or bring back) the sky viewer.
static bool skyViewOpen(QWidget *parent, Scene *scene) {
	if (g_skyView) {
		g_skyView->unpark();
		g_skyView->scn = scene;
		return true;
	}
	const EphCatalog &cat = ephCatalog();
	if (cat.stars.empty()) {
		QMessageBox::warning(parent, "Sky viewer", "The star catalogue could not be read: " + cat.error);
		return false;
	}
	auto *m = new SkyViewDialog;
	m->scn = scene;
	QDialog *dlg = new QDialog(parent);
	m->dlg = dlg;
	dlg->setWindowTitle("Sky viewer");
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint |
	                    Qt::WindowMaximizeButtonHint);

	// ---- the view --------------------------------------------------------------------------------
	m->view = new SkyView(dlg);                               // the sky, seen from its centre
	m->view->setMinimumSize(600, 420);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->view->setRenderWindow(rw);
	m->ren = vtkSmartPointer<vtkRenderer>::New();
	m->ren->AutomaticLightCreationOff();
	rw->AddRenderer(m->ren);
	m->labRen = vtkSmartPointer<vtkRenderer>::New();        // labels: overlay, same camera
	m->labRen->SetLayer(1);
	m->labRen->InteractiveOff();
	m->labRen->SetActiveCamera(m->ren->GetActiveCamera());
	rw->SetNumberOfLayers(2);
	rw->AddRenderer(m->labRen);
	// The Moon's light, from the Sun's direction (everything else in the sky is unlit).
	m->moonLight = vtkSmartPointer<vtkLight>::New();
	m->moonLight->SetLightTypeToSceneLight();
	m->moonLight->SetPositional(false);
	m->moonLight->SetIntensity(1.0);
	m->ren->AddLight(m->moonLight);
	m->eqM = vtkSmartPointer<vtkMatrix4x4>::New();

	// Stars: one actor per magnitude bin, so each bin has its own dot size.
	for (int b = 0; b < kSkyBins; ++b) {
		m->starPD[b] = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(m->starPD[b]);
		map->SetScalarModeToUsePointData();
		m->stars[b] = vtkSmartPointer<vtkActor>::New();
		m->stars[b]->SetMapper(map);
		m->stars[b]->SetUserMatrix(m->eqM);
		m->stars[b]->GetProperty()->SetPointSize(kSkyBinPx[b]);
		m->stars[b]->GetProperty()->SetRenderPointsAsSpheres(true);
		m->stars[b]->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->stars[b]);
	}
	// Constellation figures.
	{
		m->figPD = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(m->figPD);
		m->figs = vtkSmartPointer<vtkActor>::New();
		m->figs->SetMapper(map);
		m->figs->SetUserMatrix(m->eqM);
		m->figs->GetProperty()->SetColor(kSkyCFig[0], kSkyCFig[1], kSkyCFig[2]);
		m->figs->GetProperty()->SetLineWidth(1.2f);
		m->figs->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->figs);
	}
	// RA/Dec grid (J2000): hour circles every 1 h, parallels every 10 deg.
	{
		std::vector<std::vector<std::array<double, 3>>> runs;
		for (int h = 0; h < 24; ++h) {
			std::vector<std::array<double, 3>> r;
			for (int d = -80; d <= 80; d += 2) r.push_back(skySph(15.0 * h, d, kSkyLineR));
			runs.push_back(r);
		}
		for (int d = -80; d <= 80; d += 10) {
			std::vector<std::array<double, 3>> r;
			for (int a = 0; a <= 360; a += 2) r.push_back(skySph(a, d, kSkyLineR));
			runs.push_back(r);
		}
		m->eqGrid = skyLines(runs, kSkyCEq, 1.0, m->eqM);
		m->ren->AddActor(m->eqGrid);
	}
	// The ecliptic (J2000).
	{
		std::vector<std::array<double, 3>> r;
		for (int a = 0; a <= 360; a += 2) {
			const auto e = skySph(a, 0.0, kSkyLineR);
			double q[3];
			ephEclToEqu(e.data(), q);
			r.push_back({q[0], q[1], q[2]});
		}
		m->ecl = skyLines({r}, kSkyCEcl, 1.5, m->eqM);
		m->ren->AddActor(m->ecl);
	}
	// Alt/Az grid (horizon frame): altitude circles every 10 deg, azimuths every 15 deg, above the horizon.
	{
		std::vector<std::vector<std::array<double, 3>>> runs;
		for (int a = 0; a < 360; a += 15) {
			std::vector<std::array<double, 3>> r;
			for (int h = 0; h <= 80; h += 2) r.push_back(skySph(90.0 - a, h, kSkyLineR));
			runs.push_back(r);
		}
		for (int h = 10; h <= 80; h += 10) {
			std::vector<std::array<double, 3>> r;
			for (int a = 0; a <= 360; a += 2) r.push_back(skySph(a, h, kSkyLineR));
			runs.push_back(r);
		}
		m->azGrid = skyLines(runs, kSkyCAz, 1.0);
		m->ren->AddActor(m->azGrid);
		std::vector<std::array<double, 3>> hz;
		for (int a = 0; a <= 360; a += 1) hz.push_back(skySph(a, 0.0, kSkyLineR - 0.5));
		m->horizon = skyLines({hz}, kSkyCHor, 2.0);
		m->ren->AddActor(m->horizon);
	}
	// The ground: the lower hemisphere, in front of everything below the horizon.
	{
		vtkNew<vtkSphereSource> src;
		src->SetRadius(kSkyGroundR);
		src->SetStartPhi(90.0);
		src->SetEndPhi(180.0);
		src->SetThetaResolution(72);
		src->SetPhiResolution(18);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(src->GetOutputPort());
		m->ground = vtkSmartPointer<vtkActor>::New();
		m->ground->SetMapper(map);
		m->ground->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->ground);
	}
	// The Sun (unlit disc) and the Moon (lit from the Sun's direction).
	{
		m->sunSrc = vtkSmartPointer<vtkSphereSource>::New();
		m->sunSrc->SetThetaResolution(32);
		m->sunSrc->SetPhiResolution(16);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(m->sunSrc->GetOutputPort());
		m->sun = vtkSmartPointer<vtkActor>::New();
		m->sun->SetMapper(map);
		m->sun->GetProperty()->SetColor(1.0, 0.92, 0.55);
		m->sun->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->sun);
		m->moonSrc = vtkSmartPointer<vtkSphereSource>::New();
		m->moonSrc->SetThetaResolution(48);
		m->moonSrc->SetPhiResolution(24);
		vtkNew<vtkPolyDataMapper> mm;
		mm->SetInputConnection(m->moonSrc->GetOutputPort());
		m->moon = vtkSmartPointer<vtkActor>::New();
		m->moon->SetMapper(mm);
		m->moon->GetProperty()->SetColor(0.92, 0.91, 0.86);
		m->moon->GetProperty()->SetAmbient(0.04);
		m->moon->GetProperty()->SetDiffuse(1.0);
		m->moon->GetProperty()->SetSpecular(0.0);
		m->ren->AddActor(m->moon);
	}
	// The planets: one round dot each, sized by magnitude.
	for (int j = 0; j < 7; ++j) {
		m->planetPt[j] = vtkSmartPointer<vtkPoints>::New();
		m->planetPt[j]->InsertNextPoint(0, 0, 0);
		vtkNew<vtkCellArray> v;
		const vtkIdType id = 0;
		v->InsertNextCell(1, &id);
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(m->planetPt[j]);
		pd->SetVerts(v);
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(pd);
		m->planet[j] = vtkSmartPointer<vtkActor>::New();
		m->planet[j]->SetMapper(map);
		m->planet[j]->GetProperty()->SetColor(kSkyPlanetColor[j][0], kSkyPlanetColor[j][1], kSkyPlanetColor[j][2]);
		m->planet[j]->GetProperty()->SetRenderPointsAsSpheres(true);
		m->planet[j]->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->planet[j]);
	}

	// Labels: body names, star names (named stars only), figure names, cardinal points.
	for (int b = 0; b < SKY_NBODIES; ++b) {
		static const double cSun[3] = {1.0, 0.9, 0.5}, cMoon[3] = {0.9, 0.9, 0.85};
		const double *c = b == SKY_SUN ? cSun : (b == SKY_MOON ? cMoon : kSkyPlanetColor[b - SKY_MERCURY]);
		m->bodyLbl[b] = skyLabel(kSkyBodyName[b], 13, c, 14);
		m->labRen->AddActor(m->bodyLbl[b]);
	}
	{
		const double cs[3] = {0.82, 0.82, 0.70};
		for (const auto &nm : cat.names) {
			if (nm.first <= 0 || nm.first >= int(cat.byHr.size()) || cat.byHr[size_t(nm.first)] < 0) continue;
			m->starLblIdx.push_back(cat.byHr[size_t(nm.first)]);
			m->starLbl.push_back(skyLabel(nm.second, 11, cs, 10));
			m->labRen->AddActor(m->starLbl.back());
		}
		const double cf[3] = {0.55, 0.70, 1.00};
		for (const EphConstellation &k : cat.constellations) {
			double s[3] = {0, 0, 0};
			int n = 0;
			for (const auto &sg : k.segs)
				for (int hr : {sg.first, sg.second}) {
					if (hr <= 0 || hr >= int(cat.byHr.size()) || cat.byHr[size_t(hr)] < 0) continue;
					double v[3];
					ephStarVec(cat.stars[size_t(cat.byHr[size_t(hr)])], 0.0, v);
					for (int i = 0; i < 3; ++i) s[i] += v[i];
					++n;
				}
			if (n == 0 || vtkMath::Normalize(s) == 0.0) continue;
			m->figU.push_back({s[0], s[1], s[2]});
			m->figLbl.push_back(skyLabel(k.name.toUpper(), 11, cf, 0, true));
			m->labRen->AddActor(m->figLbl.back());
		}
		const char *card[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
		const double cc[3] = {1.0, 0.65, 0.30};
		for (int k = 0; k < 8; ++k) {
			const auto p = skySph(90.0 - 45.0 * k, 0.0, kSkyLineR);
			auto a = skyLabel(card[k], k % 2 ? 12 : 16, cc, 12);
			a->SetPosition(p[0], p[1], p[2]);
			m->cardLbl.push_back(a);
			m->labRen->AddActor(a);
		}
	}

	// ---- the controls ----------------------------------------------------------------------------
	QSettings st = igmtSettings();
	m->place = new QComboBox(dlg);                            // a city: sets the latitude and longitude
	m->place->setEditable(false);
	m->place->setMinimumContentsLength(14);
	m->place->setToolTip("Pick a city to stand in; or type a latitude and longitude beside it.");
	{
		std::vector<std::tuple<QString, double, double>> cities;
		QFile f(QDir(gmtvtkDataDir()).filePath("wcity_major.dat"));
		if (f.open(QIODevice::ReadOnly | QIODevice::Text))
			while (!f.atEnd()) {
				const QStringList w = QString::fromUtf8(f.readLine()).simplified().split(' ');
				if (w.size() >= 3)
					cities.emplace_back(w.mid(2).join(' ').replace('_', ' '), w[1].toDouble(), w[0].toDouble());
			}
		std::sort(cities.begin(), cities.end());
		m->place->addItem("(lat / lon)", QVariant());
		for (const auto &c : cities)
			m->place->addItem(std::get<0>(c), QVariant(QPointF(std::get<1>(c), std::get<2>(c))));
	}
	m->lat = new QDoubleSpinBox(dlg);                         // the observer's latitude
	m->lat->setRange(-90.0, 90.0);
	m->lat->setDecimals(2);
	m->lat->setPrefix("lat ");
	m->lat->setSuffix("°");
	m->lat->setValue(st.value("sky/lat", 38.72).toDouble());
	m->lat->setToolTip("The observer's latitude (north positive).");
	m->lon = new QDoubleSpinBox(dlg);                         // the observer's longitude
	m->lon->setRange(-180.0, 180.0);
	m->lon->setDecimals(2);
	m->lon->setPrefix("lon ");
	m->lon->setSuffix("°");
	m->lon->setValue(st.value("sky/lon", -9.13).toDouble());
	m->lon->setToolTip("The observer's longitude (east positive).");
	{
		const int k = m->place->findText(st.value("sky/place", "Lisbon").toString());
		QSignalBlocker b(m->place);
		m->place->setCurrentIndex(k >= 0 ? k : 0);
	}
	m->when = new QDateTimeEdit(dlg);                         // the instant shown (UTC)
	m->when->setTimeSpec(Qt::UTC);
	m->when->setDisplayFormat("yyyy-MM-dd HH:mm");
	m->when->setMinimumDateTime(QDateTime(QDate(100, 1, 1), QTime(0, 0), Qt::UTC));
	m->when->setMaximumDateTime(QDateTime(QDate(2999, 12, 31), QTime(23, 59), Qt::UTC));
	m->when->setDateTime(QDateTime::currentDateTimeUtc());
	m->when->setToolTip("The instant shown, in UTC (proleptic Gregorian calendar).");
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
	m->stepBox->addItems({"1 minute", "10 minutes", "1 hour", "1 sidereal day", "1 day", "1 week", "1 month",
	                      "1 year"});
	m->stepBox->setCurrentIndex(1);
	m->stepBox->setToolTip("Time advanced at each step and each Play frame. One sidereal day brings the "
	                       "stars back to the same place: only the Sun, the Moon and the planets move.");
	m->backBox = new QCheckBox("Backward", dlg);              // Play runs time backwards
	m->figBox = new QCheckBox("Figures", dlg);                // constellation stick figures
	m->figBox->setChecked(true);
	m->figNameBox = new QCheckBox("Constellations", dlg);     // constellation names
	m->figNameBox->setChecked(true);
	m->starNameBox = new QCheckBox("Star names", dlg);        // names of the bright named stars
	m->starNameBox->setChecked(true);
	m->starNameBox->setToolTip("Names of the named stars; fainter ones appear as you zoom in.");
	m->planetBox = new QCheckBox("Planets", dlg);             // the planets' dots and every body's name
	m->planetBox->setChecked(true);
	m->azGridBox = new QCheckBox("Alt/Az grid", dlg);         // altitude/azimuth grid
	m->eqGridBox = new QCheckBox("RA/Dec grid", dlg);         // right ascension/declination grid (J2000)
	m->eclBox = new QCheckBox("Ecliptic", dlg);               // the Sun's yearly path
	m->eclBox->setChecked(true);
	m->groundBox = new QCheckBox("Ground", dlg);              // hide what is below the horizon
	m->groundBox->setChecked(true);
	m->dayBox = new QCheckBox("Daylight", dlg);               // the sky brightens with the Sun's altitude
	m->dayBox->setChecked(true);
	m->dayBox->setToolTip("Colour the sky by the Sun's altitude (day, civil, nautical and astronomical "
	                      "twilight) and hide the stars the twilight would hide.");
	m->magLim = new QDoubleSpinBox(dlg);                      // faintest star drawn
	m->magLim->setRange(-1.0, 8.0);
	m->magLim->setDecimals(1);
	m->magLim->setSingleStep(0.5);
	m->magLim->setValue(6.5);
	m->magLim->setPrefix("mag ≤ ");
	m->magLim->setToolTip("The faintest star drawn (6.5: the naked eye under a dark sky).");
	m->bodySize = new QDoubleSpinBox(dlg);                    // Sun & Moon drawn size factor
	m->bodySize->setRange(1.0, 10.0);
	m->bodySize->setDecimals(1);
	m->bodySize->setValue(2.0);
	m->bodySize->setPrefix("Sun/Moon ×");
	m->bodySize->setToolTip("Sun and Moon drawn this many times their true size (half a degree).");
	QPushButton *zenBtn = new QPushButton("Whole sky", dlg);  // look straight up, widest field
	zenBtn->setToolTip("Look at the zenith with the widest field.");
	QPushButton *southBtn = new QPushButton("Horizon", dlg);  // back to the default gaze (the equator side)
	southBtn->setToolTip("Look at the horizon towards the equator (south in the northern hemisphere).");
	for (QPushButton *b : {nowBtn, m->play, zenBtn, southBtn}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
	m->timeLbl = new QLabel(dlg);                             // the clock: UTC, sidereal time, TT-UT
	m->bodiesLbl = new QLabel(dlg);                           // the bodies' altitudes; a click looks at one
	m->bodiesLbl->setToolTip("Click a name to look at it. Grey: below the horizon.");
	m->pickLbl = new QLabel("Click a star or a body to identify it.", dlg);   // what was clicked
	m->hoverLbl = new QLabel(dlg);                            // the cursor's sky coordinates
	for (QLabel *l : {m->timeLbl, m->bodiesLbl, m->pickLbl}) {
		l->setTextFormat(Qt::RichText);
		l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		l->setWordWrap(true);
		l->setTextInteractionFlags(Qt::TextBrowserInteraction);
	}
	m->pickLbl->setMinimumHeight(110);
	auto entry = [](const double *c, const QString &t) {
		return QString("<span style='color:%1'>&#9632;</span>&nbsp;%2<br>")
			.arg(QColor::fromRgbF(c[0], c[1], c[2]).name()).arg(t);
	};
	auto *legend = new QLabel(entry(kSkyCFig, "Constellation figures") + entry(kSkyCEcl, "Ecliptic") +
	                          entry(kSkyCHor, "Horizon") + entry(kSkyCAz, "Alt/Az grid (10°, 15°)") +
	                          entry(kSkyCEq, "RA/Dec grid, J2000 (1 h, 10°)") +
	                          "Drag to look around, wheel to zoom.", dlg);
	legend->setTextFormat(Qt::RichText);
	legend->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	auto *row1 = new QHBoxLayout;
	row1->addWidget(m->place);
	row1->addWidget(m->lat);
	row1->addWidget(m->lon);
	row1->addSpacing(12);
	row1->addWidget(m->when);
	row1->addWidget(nowBtn);
	row1->addWidget(m->play);
	row1->addWidget(stepLeft);
	row1->addWidget(m->stepBox);
	row1->addWidget(stepRight);
	row1->addWidget(m->backBox);
	row1->addStretch(1);
	auto *row2 = new QHBoxLayout;
	for (QWidget *w : std::initializer_list<QWidget *>{m->figBox, m->figNameBox, m->starNameBox, m->planetBox,
	                                                   m->eclBox, m->azGridBox, m->eqGridBox, m->groundBox,
	                                                   m->dayBox, m->magLim, m->bodySize})
		row2->addWidget(w);
	row2->addStretch(1);
	row2->addWidget(southBtn);
	row2->addWidget(zenBtn);
	auto *side = new QVBoxLayout;
	side->addWidget(m->timeLbl);
	side->addSpacing(6);
	side->addWidget(m->bodiesLbl);
	side->addSpacing(6);
	side->addWidget(m->pickLbl);
	side->addStretch(1);
	side->addWidget(legend);
	auto *sideW = new QWidget(dlg);
	sideW->setLayout(side);
	sideW->setFixedWidth(270);
	auto *top = new QHBoxLayout;
	top->addWidget(m->view, 1);
	top->addWidget(sideW);
	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->addLayout(top, 1);
	lay->addWidget(m->hoverLbl);
	lay->addLayout(row1);
	lay->addLayout(row2);
	{
		QScreen *scr = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
		const QRect av = scr->availableGeometry();
		const int w = std::min(1300, int(av.width() * 0.92)), h = std::min(860, int(av.height() * 0.88));
		dlg->resize(w, h);
		dlg->move(av.x() + (av.width() - w) / 2, av.y() + (av.height() - h - 30) / 2);
	}

	// ---- wiring ----------------------------------------------------------------------------------
	m->view->onDrag = [m](double dx, double dy) {
		const double k = m->fov / std::max(1, m->view->height());   // deg per pixel
		m->az -= dx * k;
		m->alt += dy * k;
		m->setCamera();
	};
	m->view->onZoom = [m](double f) {
		m->fov *= f;
		m->setCamera();
	};
	m->view->onClick = [m](const QPoint &p) { m->identify(p); };
	m->view->onHover = [m](const QPoint &p) { m->hover(p); };
	QObject::connect(m->bodiesLbl, &QLabel::linkActivated, dlg, [m](const QString &l) { m->lookAt(l.toInt()); });
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
	QObject::connect(m->place, &QComboBox::currentIndexChanged, dlg, [m](int) {
		const QVariant v = m->place->currentData();
		if (!v.isValid()) return;
		const QPointF p = v.toPointF();
		{
			QSignalBlocker a(m->lat), b(m->lon);
			m->lat->setValue(p.x());
			m->lon->setValue(p.y());
		}
		m->savePlace();
		m->update();
	});
	for (QDoubleSpinBox *s : {m->lat, m->lon})
		QObject::connect(s, &QDoubleSpinBox::valueChanged, dlg, [m](double) {
			QSignalBlocker b(m->place);
			m->place->setCurrentIndex(0);                   // typed by hand: no city any more
			m->savePlace();
			m->update();
		});
	for (QCheckBox *b : {m->dayBox, m->starNameBox, m->figNameBox})
		QObject::connect(b, &QCheckBox::toggled, dlg, [m](bool) { m->update(); });
	QObject::connect(m->figBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->figs->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->eclBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->ecl->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->azGridBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->azGrid->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->eqGridBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->eqGrid->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->groundBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->ground->SetVisibility(on);
		m->update();
	});
	QObject::connect(m->planetBox, &QCheckBox::toggled, dlg, [m](bool on) {
		for (auto &p : m->planet) p->SetVisibility(on);
		m->update();
	});
	QObject::connect(m->magLim, &QDoubleSpinBox::valueChanged, dlg, [m](double) { m->update(); });
	QObject::connect(m->bodySize, &QDoubleSpinBox::valueChanged, dlg, [m](double) { m->update(); });
	QObject::connect(zenBtn, &QPushButton::clicked, dlg, [m]() {
		m->alt = 89.5;
		m->az = m->lat->value() >= 0 ? 180.0 : 0.0;
		m->fov = 120.0;
		m->setCamera();
	});
	QObject::connect(southBtn, &QPushButton::clicked, dlg, [m]() {
		m->alt = 25.0;
		m->az = m->lat->value() >= 0 ? 180.0 : 0.0;
		m->fov = 90.0;
		m->setCamera();
	});

	// PARKED LIKE EVERY TOOL WINDOW (parkTool / parkOnMinimise, 50_scene.cpp).
	struct CloseParks : QObject {
		SkyViewDialog *d;
		CloseParks(QObject *p, SkyViewDialog *dd) : QObject(p), d(dd) {}
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
		if (g_skyView == m) g_skyView = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	wrapTooltips(dlg);
	g_skyView = m;
	m->azGrid->SetVisibility(false);
	m->eqGrid->SetVisibility(false);
	m->az = m->lat->value() >= 0 ? 180.0 : 0.0;
	m->jd = ephJD(m->when->dateTime());
	m->update();
	m->setCamera();
	dlg->show();
	return true;
}
