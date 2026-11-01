// 74_eclipse.cpp -- T & L > "Earth-Moon system".
//
// The Earth and the Moon at true scale at any instant, each with its day and night, the shadows each
// casts, and the eclipses those shadows make -- with the demo's own list of every solar and lunar
// eclipse from 1900 to 2100, found by the same ephemeris that draws the picture (74_ephem.cpp), never
// typed in from a catalogue.
//
// FRAME: geocentric J2000 ecliptic, the world XY plane the ecliptic, +X the March equinox; ONE SCENE
// UNIT = THE EARTH'S EQUATORIAL RADIUS. The Moon sits at its true ~60 units, at its true size (0.27).
// The Sun is a direction (it is 23 500 units away): a yellow line points at it.
//
// SHADING: the bodies are unlit; one translucent skin over each (shadeSkin) carries ALL the darkening,
// vertex by vertex: the night half, and the eclipse -- for every surface point, the share of the
// Sun's disc the other body hides as seen FROM THAT POINT (eclObscured: two discs, the exact overlap
// area). So the umbra, the penumbra and the day/night line on either body come from one formula; the
// Earth's shadow on the Moon is drawn copper-red where it is total, as the Earth's atmosphere makes it.
// The Earth's shadow is enlarged by 1/85 of its radius for its atmosphere (Danjon), as NASA's eclipse
// canon does.
//
// ECLIPSES (eclSearch): every syzygy (new and full Moon, Newton on the Sun-Moon elongation) is tested.
// Greatest eclipse = the instant the shadow axis passes nearest the Earth's centre (solar) or the Moon's
// centre passes nearest the axis of the Earth's shadow (lunar); gamma = that distance in Earth radii,
// + north. Solar: central (total / annular by whether the Moon's umbra reaches the surface) when
// |gamma| < 1, partial when the penumbra still touches the Earth. Lunar: umbral and penumbral
// magnitudes from the shadow radii at the Moon's distance.

static const double kEclRE = kEphEarthRkm;                 // km per scene unit
static const double kEclDanjon = 1.0 + 1.0 / 85.0;        // the Earth's shadow, enlarged for its air

static double eclDot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double eclNorm(const double a[3]) { return std::sqrt(eclDot(a, a)); }

// The Sun and the Moon from the Earth's centre at `tt` (scene units, J2000 ecliptic).
static void eclSunMoon(double tt, double S[3], double M[3]) {
	double sq[3], mq[3];
	ephGeocentric(SKY_SUN, tt, sq);
	ephGeocentric(SKY_MOON, tt, mq);
	ephEquToEcl(sq, S);
	ephEquToEcl(mq, M);
	for (int i = 0; i < 3; ++i) {
		S[i] *= kEphAUkm / kEclRE;
		M[i] *= kEphAUkm / kEclRE;
	}
}

// Area of the overlap of two discs, radii a and b, centres d apart.
static double eclOverlap(double a, double b, double d) {
	if (d >= a + b) return 0.0;
	if (d <= std::fabs(a - b)) return vtkMath::Pi() * std::min(a, b) * std::min(a, b);
	const double ca = std::clamp((d * d + a * a - b * b) / (2.0 * d * a), -1.0, 1.0);
	const double cb = std::clamp((d * d + b * b - a * a) / (2.0 * d * b), -1.0, 1.0);
	const double k = std::max(0.0, (-d + a + b) * (d + a - b) * (d - a + b) * (d + a + b));
	return a * a * std::acos(ca) + b * b * std::acos(cb) - 0.5 * std::sqrt(k);
}

// The share (0..1) of the Sun's disc that a body of radius `rc` at `C` hides, seen from `P`.
static double eclObscured(const double P[3], const double S[3], const double C[3], double rc) {
	double s[3], c[3];
	for (int i = 0; i < 3; ++i) {
		s[i] = S[i] - P[i];
		c[i] = C[i] - P[i];
	}
	const double ds = eclNorm(s), dc = eclNorm(c);
	if (dc <= rc || dc >= ds || eclDot(s, c) <= 0.0) return 0.0;
	const double as = std::asin(kEphSunRkm / kEclRE / ds), ac = std::asin(std::min(1.0, rc / dc));
	const double d = std::acos(std::clamp(eclDot(s, c) / (ds * dc), -1.0, 1.0));
	return eclOverlap(as, ac, d) / (vtkMath::Pi() * as * as);
}

// ---- the eclipse search ------------------------------------------------------------------------

struct EclEvent {
	double jdUT = 0.0;
	bool solar = false;
	int kind = 0;            // solar: 0 partial, 1 annular, 2 total; lunar: 0 penumbral, 1 partial, 2 total
	double gamma = 0.0;      // Earth radii, + north
	double mag = 0.0;        // lunar: umbral magnitude (penumbral when kind 0); solar: unused
};

static QString eclKindName(const EclEvent &e) {
	static const char *s[3] = {"partial", "annular", "total"}, *l[3] = {"penumbral", "partial", "total"};
	return QString(e.solar ? "Solar " : "Lunar ") + (e.solar ? s[e.kind] : l[e.kind]);
}

// Sun-Moon elongation in ecliptic longitude (deg, -180..180), the quantity a syzygy zeroes.
static double eclElong(double tt, double target) {
	double S[3], M[3];
	eclSunMoon(tt, S, M);
	double d = (std::atan2(M[1], M[0]) - std::atan2(S[1], S[0])) / kEphRad - target;
	return std::remainder(d, 360.0);
}

// Signed distance (scene units, + north of the equator) of the relevant shadow axis: solar = from the
// Earth's centre to the Moon's shadow axis; lunar = from the Moon's centre to the Earth's shadow axis.
static double eclAxisDistance(double tt, bool solar) {
	double S[3], M[3], u[3], Q[3], qe[3];
	eclSunMoon(tt, S, M);
	if (solar) {
		for (int i = 0; i < 3; ++i) u[i] = M[i] - S[i];
	}
	else {
		for (int i = 0; i < 3; ++i) u[i] = -S[i];
	}
	vtkMath::Normalize(u);
	const double k = eclDot(M, u);
	for (int i = 0; i < 3; ++i) Q[i] = M[i] - k * u[i];        // the axis' nearest point to the Earth (solar),
	ephEclToEqu(Q, qe);                                           // or the Moon off the axis (lunar)
	return (qe[2] >= 0 ? 1.0 : -1.0) * eclNorm(Q);
}

// Greatest eclipse near the syzygy `t0` (TT): the minimum of |axis distance| (golden section).
static double eclGreatest(double t0, bool solar) {
	double a = t0 - 0.3, b = t0 + 0.3;
	const double g = 0.5 * (std::sqrt(5.0) - 1.0);
	double c = b - g * (b - a), d = a + g * (b - a);
	double fc = std::fabs(eclAxisDistance(c, solar)), fd = std::fabs(eclAxisDistance(d, solar));
	for (int i = 0; i < 40; ++i) {
		if (fc < fd) {
			b = d; d = c; fd = fc;
			c = b - g * (b - a);
			fc = std::fabs(eclAxisDistance(c, solar));
		}
		else {
			a = c; c = d; fc = fd;
			d = a + g * (b - a);
			fd = std::fabs(eclAxisDistance(d, solar));
		}
	}
	return 0.5 * (a + b);
}

// UT of a TT Julian Date.
static double eclUT(double tt) {
	double ut = tt - ephDeltaT(2000.0 + (tt - 2451545.0) / 365.25) / 86400.0;
	return tt - ephDeltaT(2000.0 + (ut - 2451545.0) / 365.25) / 86400.0;
}

// Classify the syzygy whose greatest eclipse is at `tt`; false = no eclipse.
static bool eclClassify(double tt, bool solar, EclEvent &e) {
	double S[3], M[3];
	eclSunMoon(tt, S, M);
	const double Rs = kEphSunRkm / kEclRE, Rm = kEphMoonRkm / kEclRE;
	e.jdUT = eclUT(tt);
	e.solar = solar;
	e.gamma = eclAxisDistance(tt, solar);
	const double g = std::fabs(e.gamma);
	if (solar) {
		double u[3], SM[3];
		for (int i = 0; i < 3; ++i) SM[i] = M[i] - S[i];
		const double dSM = eclNorm(SM);
		for (int i = 0; i < 3; ++i) u[i] = SM[i] / dSM;
		const double D = -eclDot(M, u);                       // Moon -> fundamental plane, along the axis
		const double tp = (Rs + Rm) / dSM, tu = (Rs - Rm) / dSM;
		const double l1 = Rm + D * tp;                        // penumbra radius in the fundamental plane
		if (g >= 1.0 + l1) return false;
		if (g < 1.0) {
			const double l2s = Rm - (D - std::sqrt(1.0 - g * g)) * tu;   // umbra radius at the surface
			e.kind = l2s > 0.0 ? 2 : 1;
		}
		else e.kind = 0;
		return true;
	}
	const double dS = eclNorm(S);
	double u[3];
	for (int i = 0; i < 3; ++i) u[i] = -S[i] / dS;
	const double x = eclDot(M, u), Re = kEclDanjon;
	const double ru = Re - x * (Rs - Re) / dS, rp = Re + x * (Rs + Re) / dS;
	const double umag = (ru + Rm - g) / (2.0 * Rm), pmag = (rp + Rm - g) / (2.0 * Rm);
	if (pmag <= 0.0) return false;
	e.kind = umag >= 1.0 ? 2 : (umag > 0.0 ? 1 : 0);
	e.mag = e.kind == 0 ? pmag : umag;
	return true;
}

// Every eclipse with greatest eclipse between the UT Julian Dates `jd0` and `jd1`.
static std::vector<EclEvent> eclSearch(double jd0, double jd1) {
	std::vector<EclEvent> out;
	const double rate = 12.190749;                            // mean Sun-Moon elongation rate, deg/day
	double t = ephTT(jd0);
	const double t1 = ephTT(jd1);
	// The first syzygy after jd0, new or full, then alternately the other, half a synodic month apart.
	const double e0 = eclElong(t, 0.0);
	double target = e0 < 0.0 ? 0.0 : 180.0;
	t += (target - e0) / rate;
	while (t < t1 + 1.0) {
		for (int i = 0; i < 8; ++i) {
			const double dt = eclElong(t, target) / rate;
			t -= dt;
			if (std::fabs(dt) < 1e-6) break;
		}
		const bool solar = target == 0.0;
		EclEvent e;
		if (eclClassify(eclGreatest(t, solar), solar, e) && e.jdUT >= jd0 && e.jdUT <= jd1) out.push_back(e);
		target = solar ? 180.0 : 0.0;
		t += 29.530589 / 2.0;
	}
	return out;
}

// ---- the demo ----------------------------------------------------------------------------------

// One body's skin: a unit sphere slightly above the surface whose vertex colours carry the night and
// the eclipse (RGBA), and the terminator ring.
struct EclSkin {
	vtkSmartPointer<vtkPolyData> pd;
	vtkSmartPointer<vtkUnsignedCharArray> rgba;
	vtkSmartPointer<vtkActor> actor, ring;
	vtkSmartPointer<vtkMatrix4x4> m, ringM;
};

class EclipseView : public GLView {
public:
	EclipseView(QWidget *parent) : GLView() { setParent(parent); }
protected:
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
};

struct EclipseDialog {
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	EclipseView *view = nullptr;
	QDateTimeEdit *when = nullptr;
	QComboBox *stepBox = nullptr;
	QDoubleSpinBox *sizeBox = nullptr;
	QPushButton *play = nullptr;
	QCheckBox *backBox = nullptr, *conesBox = nullptr, *pathBox = nullptr, *namesBox = nullptr,
	          *solarBox = nullptr, *lunarBox = nullptr, *penBox = nullptr;
	QLabel *info = nullptr;
	QListWidget *list = nullptr;
	QWidget *side = nullptr;
	QToolButton *sideBtn = nullptr;
	QTimer *timer = nullptr;
	bool parked = false;
	bool reallyClose = false;
	double jd = 2451545.0;                   // the instant shown (UT)
	int camMode = 0;                         // 0 free, 1 Earth from the Sun, 2 Moon from the Earth, 3 Earth from the Moon
	double S[3], M[3];                       // Sun and Moon now (scene units)
	double moonUmbra = 0.0;                  // share of the Moon's sunward half in the Earth's umbra
	double earthShadow = 0.0;                // largest share of the Sun hidden anywhere on the Earth
	std::vector<EclEvent> events;

	vtkSmartPointer<vtkRenderer> ren, labRen;
	vtkSmartPointer<vtkActor> earth, moon, sunLine, path, ecliptic, nodes;
	vtkSmartPointer<vtkActor> cone[4];       // Earth umbra, Earth penumbra, Moon umbra, Moon penumbra
	vtkSmartPointer<vtkPolyData> conePD[4], pathPD, eclPD, nodePD;
	vtkSmartPointer<vtkMatrix4x4> earthM, moonM;
	vtkSmartPointer<vtkLineSource> sunSrc;
	EclSkin skin[2];                         // 0 Earth, 1 Moon
	vtkSmartPointer<vtkBillboardTextActor3D> earthLbl, moonLbl, sunLbl, nodeLbl[2];

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

	double drawR(int body) const { return sizeBox->value() * (body == 0 ? 1.0 : kEphMoonRkm / kEclRE); }

	// A skin's colours: night, then the eclipse, for every vertex -- the point's TRUE position on the
	// body (centre C, true radius), whatever size the body is drawn at.
	void shadeSkin(int body, const double C[3]) {
		EclSkin &k = skin[body];
		const double Rt = body == 0 ? 1.0 : kEphMoonRkm / kEclRE;
		const double *O = body == 0 ? M : nullptr;            // the occluder's centre
		const double origin[3] = {0, 0, 0};
		const double Ro = body == 0 ? kEphMoonRkm / kEclRE : kEclDanjon;
		double sd[3];
		for (int i = 0; i < 3; ++i) sd[i] = S[i] - C[i];
		vtkMath::Normalize(sd);
		vtkPoints *pts = k.pd->GetPoints();
		const vtkIdType n = pts->GetNumberOfPoints();
		int day = 0, dayUmbra = 0;
		double worst = 0.0;
		for (vtkIdType v = 0; v < n; ++v) {
			double p[3], P[3];
			pts->GetPoint(v, p);
			for (int i = 0; i < 3; ++i) P[i] = C[i] + Rt * p[i];
			unsigned char c[4] = {0, 0, 0, 0};
			if (eclDot(p, sd) < 0.0) c[3] = 150;                // night
			else {
				const double f = eclObscured(P, S, O ? O : origin, Ro);
				++day;
				worst = std::max(worst, f);
				if (f > 0.0) {
					if (body == 1 && f > 0.995) {                // totality on the Moon: copper red
						c[0] = 120; c[1] = 30; c[2] = 10; c[3] = 215;
						++dayUmbra;
					}
					else c[3] = (unsigned char)std::lround(std::min(1.0, f) * (body == 0 ? 235.0 : 200.0));
					if (body == 0 && f > 0.995) ++dayUmbra;
				}
			}
			k.rgba->SetTypedTuple(v, c);
		}
		k.rgba->Modified();
		k.pd->Modified();
		if (body == 1) moonUmbra = day ? double(dayUmbra) / day : 0.0;
		else earthShadow = worst;
		// The skin and the terminator ring follow the body: drawn size, the ring square to the Sun.
		const double R = drawR(body);
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) k.m->SetElement(i, j, i == j ? R * 1.004 : 0.0);
			k.m->SetElement(i, 3, C[i]);
		}
		k.m->Modified();
		double ex[3], ey[3];
		const double z[3] = {0, 0, 1};
		vtkMath::Cross(z, sd, ex);
		if (vtkMath::Normalize(ex) == 0.0) ex[0] = 1.0;
		vtkMath::Cross(sd, ex, ey);
		for (int i = 0; i < 3; ++i) {
			k.ringM->SetElement(i, 0, R * 1.008 * ex[i]);
			k.ringM->SetElement(i, 1, R * 1.008 * ey[i]);
			k.ringM->SetElement(i, 2, R * 1.008 * sd[i]);
			k.ringM->SetElement(i, 3, C[i]);
		}
		k.ringM->Modified();
	}

	// A shadow cone (or frustum) of a body of radius r0 at C, along `u` (away from the Sun): radius
	// r0 + slope * x for x in 0..len.
	static void coneMesh(vtkPolyData *pd, const double C[3], const double u[3], double r0, double slope, double len) {
		double ex[3], ey[3];
		const double z[3] = {0, 0, 1};
		vtkMath::Cross(z, u, ex);
		if (vtkMath::Normalize(ex) == 0.0) ex[0] = 1.0;
		vtkMath::Cross(u, ex, ey);
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> quads;
		const int n = 72;
		for (int j = 0; j <= 1; ++j) {
			const double x = j * len, r = std::max(0.0, r0 + slope * x);
			for (int i = 0; i < n; ++i) {
				const double t = 2.0 * vtkMath::Pi() * i / n;
				double p[3];
				for (int a = 0; a < 3; ++a) p[a] = C[a] + x * u[a] + r * (std::cos(t) * ex[a] + std::sin(t) * ey[a]);
				pts->InsertNextPoint(p);
			}
		}
		for (int i = 0; i < n; ++i) {
			const vtkIdType q[4] = {i, (i + 1) % n, n + (i + 1) % n, n + i};
			quads->InsertNextCell(4, q);
		}
		pd->SetPoints(pts);
		pd->SetPolys(quads);
		pd->Modified();
	}

	// The Moon's path over +-15 days, and where it crosses the ecliptic (the nodes: eclipses happen
	// only when a syzygy falls near one).
	void buildPath(double tt) {
		vtkNew<vtkPoints> pts;
		std::vector<vtkIdType> ids;
		vtkNew<vtkPoints> np;
		vtkNew<vtkCellArray> nv;
		double prev[3] = {0, 0, 0};
		int nn = 0;
		for (int i = -90; i <= 90; ++i) {
			double s[3], m[3];
			eclSunMoon(tt + i / 6.0, s, m);
			ids.push_back(pts->InsertNextPoint(m));
			if (i > -90 && (prev[2] < 0.0) != (m[2] < 0.0) && nn < 2) {
				const double f = prev[2] / (prev[2] - m[2]);
				double q[3];
				for (int a = 0; a < 3; ++a) q[a] = prev[a] + f * (m[a] - prev[a]);
				const vtkIdType id = np->InsertNextPoint(q);
				nv->InsertNextCell(1, &id);
				nodeLbl[nn]->SetInput(m[2] > prev[2] ? "☊ ascending node" : "☋ descending node");
				nodeLbl[nn]->SetPosition(q);
				++nn;
			}
			for (int a = 0; a < 3; ++a) prev[a] = m[a];
		}
		for (int k = nn; k < 2; ++k) nodeLbl[k]->SetInput("");
		vtkNew<vtkCellArray> line;
		line->InsertNextCell(vtkIdType(ids.size()), ids.data());
		pathPD->SetPoints(pts);
		pathPD->SetLines(line);
		pathPD->Modified();
		nodePD->SetPoints(np);
		nodePD->SetVerts(nv);
		nodePD->Modified();
		// The ecliptic at the Moon's distance today.
		vtkNew<vtkPoints> ep;
		vtkNew<vtkCellArray> el;
		const double r = eclNorm(M);
		for (int i = 0; i < 180; i += 2) {
			const double a0 = 2.0 * i * kEphRad, a1 = 2.0 * (i + 1) * kEphRad;
			const vtkIdType q[2] = {ep->InsertNextPoint(r * std::cos(a0), r * std::sin(a0), 0.0),
			                        ep->InsertNextPoint(r * std::cos(a1), r * std::sin(a1), 0.0)};
			el->InsertNextCell(2, q);
		}
		eclPD->SetPoints(ep);
		eclPD->SetLines(el);
		eclPD->Modified();
	}

	void update() {
		const double tt = ephTT(jd);
		eclSunMoon(tt, S, M);
		// The Earth, turned by the sidereal time (Greenwich towards where it faces now).
		{
			const double g = ephGMST(jd) * kEphRad, R = drawR(0);
			const double q1[3] = {std::cos(g), std::sin(g), 0.0}, q2[3] = {-std::sin(g), std::cos(g), 0.0};
			const double q3[3] = {0.0, 0.0, 1.0};
			double e1[3], e2[3], e3[3];
			ephEquToEcl(q1, e1);
			ephEquToEcl(q2, e2);
			ephEquToEcl(q3, e3);
			for (int i = 0; i < 3; ++i) {
				earthM->SetElement(i, 0, R * e1[i]);
				earthM->SetElement(i, 1, R * e2[i]);
				earthM->SetElement(i, 2, R * e3[i]);
				earthM->SetElement(i, 3, 0.0);
			}
			earthM->Modified();
		}
		{
			const double R = drawR(1);
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) moonM->SetElement(i, j, i == j ? R : 0.0);
				moonM->SetElement(i, 3, M[i]);
			}
			moonM->Modified();
		}
		const double origin[3] = {0, 0, 0};
		shadeSkin(0, origin);
		shadeSkin(1, M);
		// Shadow cones at true scale.
		{
			const double Rs = kEphSunRkm / kEclRE, Rm = kEphMoonRkm / kEclRE, Re = kEclDanjon;
			const double dS = eclNorm(S);
			double u[3];
			for (int i = 0; i < 3; ++i) u[i] = -S[i] / dS;
			coneMesh(conePD[0], origin, u, Re, -(Rs - Re) / dS, std::min(Re * dS / (Rs - Re), 90.0));
			coneMesh(conePD[1], origin, u, Re, (Rs + Re) / dS, 90.0);
			double SM[3], um[3];
			for (int i = 0; i < 3; ++i) SM[i] = M[i] - S[i];
			const double dSM = eclNorm(SM);
			for (int i = 0; i < 3; ++i) um[i] = SM[i] / dSM;
			const double reach = eclNorm(M) + 3.0;
			coneMesh(conePD[2], M, um, Rm, -(Rs - Rm) / dSM, std::min(Rm * dSM / (Rs - Rm), reach));
			coneMesh(conePD[3], M, um, Rm, (Rs + Rm) / dSM, reach);
			sunSrc->SetPoint2(75.0 * -u[0], 75.0 * -u[1], 75.0 * -u[2]);
			sunLbl->SetPosition(80.0 * -u[0], 80.0 * -u[1], 80.0 * -u[2]);
		}
		buildPath(tt);
		earthLbl->SetPosition(0, 0, drawR(0) + 0.8);
		moonLbl->SetPosition(M[0], M[1], M[2] + drawR(1) + 0.6);
		placeCamera();
		updateInfo(tt);
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// The camera presets that follow the bodies (re-placed at every step).
	void placeCamera() {
		// The Sun line runs straight through the camera of "Earth from the Sun": hidden there.
		sunLine->SetVisibility(camMode != 1);
		sunLbl->SetVisibility(camMode != 1 && namesBox->isChecked());
		if (camMode == 0) return;
		vtkCamera *cam = ren->GetActiveCamera();
		double s[3], m[3];
		for (int i = 0; i < 3; ++i) s[i] = S[i];
		vtkMath::Normalize(s);
		const double dm = eclNorm(M);
		for (int i = 0; i < 3; ++i) m[i] = M[i] / dm;
		if (camMode == 1) {                                   // the Earth from the Sun's side
			cam->SetFocalPoint(0, 0, 0);
			cam->SetPosition(9.0 * s[0], 9.0 * s[1], 9.0 * s[2]);
			cam->SetViewAngle(30.0);
		}
		else if (camMode == 2) {                              // the Moon from the Earth
			cam->SetFocalPoint(M);
			cam->SetPosition(M[0] - 3.0 * m[0], M[1] - 3.0 * m[1], M[2] - 3.0 * m[2]);
			cam->SetViewAngle(30.0);
		}
		else {                                                // the Earth from beside the Moon
			cam->SetFocalPoint(0, 0, 0);
			cam->SetPosition(M[0] * 0.85, M[1] * 0.85, M[2] * 0.85 + 2.0);
			cam->SetViewAngle(12.0);
		}
		cam->SetViewUp(0, 0, 1);
	}

	void overview() {
		camMode = 0;
		vtkCamera *cam = ren->GetActiveCamera();
		cam->SetFocalPoint(0, 0, 0);
		cam->SetPosition(0, -150, 90);
		cam->SetViewUp(0, 0, 1);
		cam->SetViewAngle(30.0);
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// The panel's numbers: distances, phase, and what is eclipsed now.
	void updateInfo(double tt) {
		const double dM = eclNorm(M) * kEclRE, dS = eclNorm(S) * kEclRE / kEphAUkm;
		const double ce = std::clamp(eclDot(S, M) / (eclNorm(S) * eclNorm(M)), -1.0, 1.0);
		// The list's own classifier, at this instant (one formula for the list and for "now").
		QString now = "no eclipse";
		EclEvent e;
		if (eclClassify(tt, ce > 0.0, e)) {
			now = "<b>" + eclKindName(e) + " eclipse</b>";
			if (e.solar) now += QString(", up to %1% of the Sun hidden").arg(100.0 * earthShadow, 0, 'f', 0);
			else now += QString(", %1 magnitude %2").arg(e.kind == 0 ? "penumbral" : "umbral").arg(e.mag, 0, 'f', 2);
		}
		info->setText(QString("<b>%1 UTC</b><br>TT − UT = %2 s<br><br>Moon: %3 km (%4 Earth radii), %5% lit<br>"
		                      "Sun: %6 AU<br><br>Now: %7")
			.arg(ephDateTime(jd).toString("yyyy-MM-dd HH:mm:ss")).arg(ephDeltaT(2000.0 + (jd - 2451545.0) / 365.25), 0, 'f', 0)
			.arg(dM, 0, 'f', 0).arg(dM / kEclRE, 0, 'f', 2).arg(50.0 * (1.0 - ce), 0, 'f', 0).arg(dS, 0, 'f', 4)
			.arg(now));
	}

	// The list, by the Solar / Lunar / Penumbral boxes; the row nearest `jd` current.
	void fillList() {
		QSignalBlocker b(list);
		list->clear();
		int best = -1;
		double bestD = 1e30;
		for (size_t i = 0; i < events.size(); ++i) {
			const EclEvent &e = events[i];
			if (e.solar ? !solarBox->isChecked() : !lunarBox->isChecked()) continue;
			if (!e.solar && e.kind == 0 && !penBox->isChecked()) continue;
			QString t = ephDateTime(e.jdUT).toString("yyyy-MM-dd HH:mm") + "  " + eclKindName(e) +
			            QString("  γ %1").arg(e.gamma, 0, 'f', 3);
			if (!e.solar) t += QString("  mag %1").arg(e.mag, 0, 'f', 2);
			auto *it = new QListWidgetItem(t, list);
			it->setData(Qt::UserRole, int(i));
			if (std::fabs(e.jdUT - jd) < bestD) {
				bestD = std::fabs(e.jdUT - jd);
				best = list->count() - 1;
			}
		}
		if (best >= 0) list->scrollToItem(list->item(best), QAbstractItemView::PositionAtCenter);
	}

	// Go to eclipse `i`, with the camera that shows it.
	void gotoEvent(int i) {
		if (i < 0 || i >= int(events.size())) return;
		camMode = events[size_t(i)].solar ? 1 : 2;
		setJD(events[size_t(i)].jdUT);
	}

	// The next (dir +1) or previous (-1) listed eclipse from the current instant.
	void stepEvent(int dir) {
		int pick = -1;
		for (int r = 0; r < list->count(); ++r) {
			const int i = list->item(r)->data(Qt::UserRole).toInt();
			const double t = events[size_t(i)].jdUT;
			if (dir > 0 && t > jd + 1e-4) {
				pick = r;
				break;
			}
			if (dir < 0 && t < jd - 1e-4) pick = r;
		}
		if (pick < 0) return;
		list->setCurrentRow(pick);
		gotoEvent(list->item(pick)->data(Qt::UserRole).toInt());
	}

	void stopPlay() {
		if (timer && timer->isActive()) timer->stop();
		if (play) play->setText("Play");
	}

	double stepDays() const {
		static const double d[] = {1.0 / 1440.0, 5.0 / 1440.0, 15.0 / 1440.0, 1.0 / 24.0, 6.0 / 24.0, 1.0};
		return d[std::clamp(stepBox->currentIndex(), 0, 5)];
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

	void setSide(bool open) {
		side->setVisible(open);
		sideBtn->setArrowType(open ? Qt::RightArrow : Qt::LeftArrow);
		sideBtn->setToolTip(open ? "Fold the panel away" : "Show the panel");
	}
};

static EclipseDialog *g_eclipse = nullptr;

static vtkSmartPointer<vtkBillboardTextActor3D> eclLabel(const QString &txt, int size, const double rgb[3]) {
	auto a = vtkSmartPointer<vtkBillboardTextActor3D>::New();
	a->SetInput(txt.toUtf8().constData());
	a->GetTextProperty()->SetFontSize(size);
	a->GetTextProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
	a->GetTextProperty()->SetJustificationToCentered();
	return a;
}

// An unlit actor over a polydata (lines or a translucent surface).
static vtkSmartPointer<vtkActor> eclActor(vtkPolyData *pd, const double rgb[3], double opacity, double width = 1.0) {
	vtkNew<vtkPolyDataMapper> map;
	map->SetInputData(pd);
	map->ScalarVisibilityOff();
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
	a->GetProperty()->SetOpacity(opacity);
	a->GetProperty()->SetLineWidth(float(width));
	a->GetProperty()->SetPointSize(7.0f);
	a->GetProperty()->SetRenderPointsAsSpheres(true);
	a->GetProperty()->SetLighting(false);
	return a;
}

static bool eclipseOpen(QWidget *parent, Scene *scene) {
	if (g_eclipse) {
		g_eclipse->unpark();
		g_eclipse->scn = scene;
		return true;
	}
	auto *m = new EclipseDialog;
	m->scn = scene;
	QDialog *dlg = new QDialog(parent);
	m->dlg = dlg;
	dlg->setWindowTitle("Earth-Moon system");
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint |
	                    Qt::WindowMaximizeButtonHint);

	// ---- the view --------------------------------------------------------------------------------
	m->view = new EclipseView(dlg);                           // the 3-D view of the Earth and the Moon
	m->view->setMinimumSize(560, 420);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->view->setRenderWindow(rw);
	m->ren = vtkSmartPointer<vtkRenderer>::New();
	m->ren->SetBackground(0.01, 0.01, 0.04);
	rw->AddRenderer(m->ren);
	m->labRen = vtkSmartPointer<vtkRenderer>::New();        // labels: overlay, same camera
	m->labRen->SetLayer(1);
	m->labRen->InteractiveOff();
	m->labRen->SetActiveCamera(m->ren->GetActiveCamera());
	rw->SetNumberOfLayers(2);
	rw->AddRenderer(m->labRen);
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	m->view->interactor()->SetInteractorStyle(style);

	// The bodies: unlit (the skins do the darkening). The Earth is the textured globe of 69_magfield.cpp.
	m->earthM = vtkSmartPointer<vtkMatrix4x4>::New();
	m->moonM = vtkSmartPointer<vtkMatrix4x4>::New();
	for (int b = 0; b < 2; ++b) {
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputData(magFieldSphere(b == 0 ? 180 : 120, b == 0 ? 90 : 60));
		map->ScalarVisibilityOff();
		auto a = vtkSmartPointer<vtkActor>::New();
		a->SetMapper(map);
		a->SetUserMatrix(b == 0 ? m->earthM : m->moonM);
		if (b == 0) a->GetProperty()->SetColor(0.35, 0.55, 0.85);
		else a->GetProperty()->SetColor(0.78, 0.77, 0.74);
		a->GetProperty()->SetLighting(false);
		m->ren->AddActor(a);
		(b == 0 ? m->earth : m->moon) = a;
		// Its skin: RGBA per vertex, in the ecliptic frame (it does not turn with the body).
		EclSkin &k = m->skin[b];
		k.pd = vtkSmartPointer<vtkPolyData>::New();
		k.pd->DeepCopy(magFieldSphere(b == 0 ? 180 : 120, b == 0 ? 90 : 60));
		k.pd->GetPointData()->SetTCoords(nullptr);
		k.rgba = vtkSmartPointer<vtkUnsignedCharArray>::New();
		k.rgba->SetNumberOfComponents(4);
		k.rgba->SetNumberOfTuples(k.pd->GetNumberOfPoints());
		k.pd->GetPointData()->SetScalars(k.rgba);
		vtkNew<vtkPolyDataMapper> km;
		km->SetInputData(k.pd);
		km->SetColorModeToDirectScalars();
		km->SetScalarModeToUsePointData();
		k.m = vtkSmartPointer<vtkMatrix4x4>::New();
		k.actor = vtkSmartPointer<vtkActor>::New();
		k.actor->SetMapper(km);
		k.actor->SetUserMatrix(k.m);
		k.actor->GetProperty()->SetLighting(false);
		k.actor->ForceTranslucentOn();
		m->ren->AddActor(k.actor);
		// The terminator: a ring square to the Sun.
		vtkNew<vtkPoints> pts;
		std::vector<vtkIdType> ids;
		for (int i = 0; i <= 180; ++i) {
			const double t = 2.0 * vtkMath::Pi() * i / 180.0;
			ids.push_back(pts->InsertNextPoint(std::cos(t), std::sin(t), 0.0));
		}
		vtkNew<vtkCellArray> line;
		line->InsertNextCell(vtkIdType(ids.size()), ids.data());
		vtkNew<vtkPolyData> rp;
		rp->SetPoints(pts);
		rp->SetLines(line);
		const double cT[3] = {1.0, 0.55, 0.1};
		k.ringM = vtkSmartPointer<vtkMatrix4x4>::New();
		k.ring = eclActor(rp, cT, 1.0, 1.8);
		k.ring->SetUserMatrix(k.ringM);
		m->ren->AddActor(k.ring);
	}
	// Shadow cones, the Sun's direction, the Moon's path, the ecliptic and the nodes.
	const double cUmb[3] = {0.75, 0.25, 0.15}, cPen[3] = {0.55, 0.55, 0.70}, cSun[3] = {1.0, 0.85, 0.3};
	const double cPath[3] = {0.75, 0.75, 0.80}, cEcl[3] = {0.90, 0.66, 0.25}, cNode[3] = {0.35, 0.85, 0.45};
	for (int c = 0; c < 4; ++c) {
		m->conePD[c] = vtkSmartPointer<vtkPolyData>::New();
		m->cone[c] = eclActor(m->conePD[c], c % 2 == 0 ? cUmb : cPen, c % 2 == 0 ? 0.16 : 0.06);
		// Outward faces only: a camera INSIDE a cone (the Moon seen from the Earth in an eclipse) sees
		// through it instead of through a tinted wall.
		m->cone[c]->GetProperty()->BackfaceCullingOn();
		m->ren->AddActor(m->cone[c]);
	}
	m->sunSrc = vtkSmartPointer<vtkLineSource>::New();
	m->sunSrc->SetPoint1(0, 0, 0);
	m->sunSrc->Update();
	{
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(m->sunSrc->GetOutputPort());
		m->sunLine = vtkSmartPointer<vtkActor>::New();
		m->sunLine->SetMapper(map);
		m->sunLine->GetProperty()->SetColor(cSun[0], cSun[1], cSun[2]);
		m->sunLine->GetProperty()->SetLineWidth(1.5f);
		m->sunLine->GetProperty()->SetLighting(false);
		m->ren->AddActor(m->sunLine);
	}
	m->pathPD = vtkSmartPointer<vtkPolyData>::New();
	m->path = eclActor(m->pathPD, cPath, 1.0, 1.2);
	m->ren->AddActor(m->path);
	m->eclPD = vtkSmartPointer<vtkPolyData>::New();
	m->ecliptic = eclActor(m->eclPD, cEcl, 1.0, 1.0);
	m->ren->AddActor(m->ecliptic);
	m->nodePD = vtkSmartPointer<vtkPolyData>::New();
	m->nodes = eclActor(m->nodePD, cNode, 1.0);
	m->ren->AddActor(m->nodes);
	const double cE[3] = {0.55, 0.75, 1.0}, cM[3] = {0.85, 0.85, 0.82};
	m->earthLbl = eclLabel("Earth", 13, cE);
	m->moonLbl = eclLabel("Moon", 12, cM);
	m->sunLbl = eclLabel("to the Sun", 12, cSun);
	m->nodeLbl[0] = eclLabel("", 11, cNode);
	m->nodeLbl[1] = eclLabel("", 11, cNode);
	for (auto *a : {m->earthLbl.GetPointer(), m->moonLbl.GetPointer(), m->sunLbl.GetPointer(),
	                m->nodeLbl[0].GetPointer(), m->nodeLbl[1].GetPointer()})
		m->labRen->AddActor(a);

	// ---- the controls ----------------------------------------------------------------------------
	m->when = new QDateTimeEdit(dlg);                         // the instant shown (UTC)
	m->when->setTimeSpec(Qt::UTC);
	m->when->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
	m->when->setMinimumDateTime(QDateTime(QDate(100, 1, 1), QTime(0, 0), Qt::UTC));
	m->when->setMaximumDateTime(QDateTime(QDate(2999, 12, 31), QTime(23, 59), Qt::UTC));
	m->when->setDateTime(QDateTime::currentDateTimeUtc());
	m->when->setToolTip("The instant shown, in UTC (proleptic Gregorian calendar).");
	QPushButton *nowBtn = new QPushButton("Now", dlg);        // back to the present instant
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
	m->stepBox = new QComboBox(dlg);                          // time per step / Play frame
	m->stepBox->addItems({"1 minute", "5 minutes", "15 minutes", "1 hour", "6 hours", "1 day"});
	m->stepBox->setCurrentIndex(1);
	m->backBox = new QCheckBox("Backward", dlg);              // Play runs time backwards
	QPushButton *prevBtn = new QPushButton("◀ Eclipse", dlg); // the previous listed eclipse
	QPushButton *nextBtn = new QPushButton("Eclipse ▶", dlg); // the next listed eclipse
	prevBtn->setToolTip("Go to the previous eclipse in the list (as filtered).");
	nextBtn->setToolTip("Go to the next eclipse in the list (as filtered).");
	m->sizeBox = new QDoubleSpinBox(dlg);                     // drawn size of the two bodies
	m->sizeBox->setRange(1.0, 6.0);
	m->sizeBox->setDecimals(1);
	m->sizeBox->setValue(1.0);
	m->sizeBox->setPrefix("Size ×");
	m->sizeBox->setToolTip("Draw the Earth and the Moon this many times their size. Distances, shadows and "
	                       "cones stay true: at ×1 everything is to scale.");
	m->conesBox = new QCheckBox("Shadow cones", dlg);         // umbra and penumbra of both bodies
	m->conesBox->setChecked(true);
	m->pathBox = new QCheckBox("Moon path && nodes", dlg);    // the Moon's path, the ecliptic and the nodes
	m->pathBox->setChecked(true);
	m->namesBox = new QCheckBox("Names", dlg);                // body names
	m->namesBox->setChecked(true);
	QPushButton *ovBtn = new QPushButton("Overview", dlg);            // free camera over the system
	QPushButton *sunBtn = new QPushButton("Earth from the Sun", dlg); // follows: where the Moon's shadow falls
	QPushButton *moonBtn = new QPushButton("Moon from the Earth", dlg);   // follows: the Moon, its phase and the Earth's shadow
	QPushButton *eFromMBtn = new QPushButton("Earth from the Moon", dlg); // follows
	ovBtn->setToolTip("Free camera over the whole system: the Earth, the Moon's orbit and the shadow cones. "
	                  "Drag to rotate, wheel to zoom.");
	sunBtn->setToolTip("Follow the Earth's day side, seen from the Sun: the Moon's shadow in a solar eclipse.");
	moonBtn->setToolTip("Follow the Moon, seen from the Earth: its phase, and the Earth's shadow in a lunar eclipse.");
	eFromMBtn->setToolTip("Follow the Earth, seen from beside the Moon.");
	for (QPushButton *b : {nowBtn, m->play, prevBtn, nextBtn, ovBtn, sunBtn, moonBtn, eFromMBtn}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
	m->info = new QLabel(dlg);                                // now: distances, phase, what is eclipsed
	m->info->setTextFormat(Qt::RichText);
	m->info->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	m->info->setWordWrap(true);
	m->solarBox = new QCheckBox("Solar", dlg);                // list solar eclipses
	m->lunarBox = new QCheckBox("Lunar", dlg);                // list lunar eclipses
	m->penBox = new QCheckBox("Penumbral", dlg);              // list penumbral lunar eclipses too
	m->solarBox->setChecked(true);
	m->lunarBox->setChecked(true);
	m->penBox->setToolTip("Also list penumbral lunar eclipses (the Moon only dims).");
	m->list = new QListWidget(dlg);                           // every eclipse 1900-2100; a click goes there
	m->list->setToolTip("Every solar and lunar eclipse from 1900 to 2100, found by this demo's own "
	                    "ephemeris. Time = greatest eclipse (UTC). γ: the shadow axis' distance from the "
	                    "Earth's centre (solar) or the Moon's from the shadow axis (lunar), Earth radii, + north. "
	                    "mag: umbral magnitude (penumbral for penumbral eclipses). Click one to go there.");
	auto entry = [](const double *c, const QString &t) {
		return QString("<span style='color:%1'>&#9632;</span>&nbsp;%2<br>")
			.arg(QColor::fromRgbF(c[0], c[1], c[2]).name()).arg(t);
	};
	const double cT[3] = {1.0, 0.55, 0.1};
	auto *legend = new QLabel(entry(cT, "Day / night line") + entry(cUmb, "Umbra (full shadow)") +
	                          entry(cPen, "Penumbra (part shadow)") + entry(cSun, "Direction of the Sun") +
	                          entry(cPath, "The Moon's path, ±15 days") +
	                          entry(cEcl, "Ecliptic, at the Moon's distance") +
	                          entry(cNode, "Nodes: eclipses happen only near them") +
	                          "<br><small>To scale: 1 unit = the Earth's radius. Against NASA's catalogue: "
	                          "greatest eclipse within 2 min, γ within 0.01; see docs/ASTRONOMY_NOTES.md.</small>", dlg);
	legend->setTextFormat(Qt::RichText);
	legend->setWordWrap(true);

	auto *row1 = new QHBoxLayout;
	row1->addWidget(m->when);
	row1->addWidget(nowBtn);
	row1->addWidget(m->play);
	row1->addWidget(stepLeft);
	row1->addWidget(m->stepBox);
	row1->addWidget(stepRight);
	row1->addWidget(m->backBox);
	row1->addSpacing(12);
	row1->addWidget(prevBtn);
	row1->addWidget(nextBtn);
	row1->addStretch(1);
	// The green ? disk: this demo's tutorial page (docs/src/82-earth-moon.md) or the manual, in the
	// browser -- the same plain openUrl the Help menu's manual uses.
	{
		auto *help = new QToolButton(dlg);
		help->setIcon(makeHelpDiskIcon(18));
		help->setIconSize(QSize(18, 18));
		help->setAutoRaise(true);
		help->setCursor(Qt::PointingHandCursor);
		help->setFocusPolicy(Qt::NoFocus);
		help->setToolTip("Open the Earth-Moon system tutorial, or the InteractiveGMT manual, in the browser.");
		QObject::connect(help, &QToolButton::clicked, dlg, [dlg, help]() {
			QMenu menu;
			QAction *aTut = menu.addAction("Tutorial: The Earth-Moon System");
			menu.addAction("InteractiveGMT Manual");
			QAction *pick = menu.exec(help->mapToGlobal(QPoint(0, help->height())));
			if (!pick) return;
			const QUrl u(pick == aTut ? "https://www.generic-mapping-tools.org/InteractiveGMT/dev/82-earth-moon/"
			                          : "https://www.generic-mapping-tools.org/InteractiveGMT");
			if (!QDesktopServices::openUrl(u))
				QMessageBox::warning(dlg, dlg->windowTitle(), QString("Could not open a browser for\n\n%1").arg(u.toString()));
		});
		row1->addWidget(help);
	}
	auto *row2 = new QHBoxLayout;
	row2->addWidget(m->sizeBox);
	row2->addWidget(m->conesBox);
	row2->addWidget(m->pathBox);
	row2->addWidget(m->namesBox);
	row2->addStretch(1);
	row2->addWidget(ovBtn);
	row2->addWidget(sunBtn);
	row2->addWidget(moonBtn);
	row2->addWidget(eFromMBtn);
	m->side = new QWidget(dlg);                               // the right-hand panel (foldable)
	auto *sideLay = new QVBoxLayout(m->side);
	sideLay->setContentsMargins(0, 0, 0, 0);
	sideLay->addWidget(m->info);
	auto *filt = new QHBoxLayout;
	filt->addWidget(new QLabel("<b>Eclipses</b>", m->side));
	filt->addWidget(m->solarBox);
	filt->addWidget(m->lunarBox);
	filt->addWidget(m->penBox);
	filt->addStretch(1);
	sideLay->addLayout(filt);
	sideLay->addWidget(m->list, 1);
	sideLay->addWidget(legend);
	m->side->setFixedWidth(400);
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
		const int w = std::min(1350, int(av.width() * 0.92)), h = std::min(860, int(av.height() * 0.88));
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
	QObject::connect(prevBtn, &QPushButton::clicked, dlg, [m]() { m->stepEvent(-1); });
	QObject::connect(nextBtn, &QPushButton::clicked, dlg, [m]() { m->stepEvent(1); });
	QObject::connect(m->sizeBox, &QDoubleSpinBox::valueChanged, dlg, [m](double) { m->update(); });
	QObject::connect(m->conesBox, &QCheckBox::toggled, dlg, [m](bool on) {
		for (auto &c : m->cone) c->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->pathBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->path->SetVisibility(on);
		m->ecliptic->SetVisibility(on);
		m->nodes->SetVisibility(on);
		m->nodeLbl[0]->SetVisibility(on);
		m->nodeLbl[1]->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	QObject::connect(m->namesBox, &QCheckBox::toggled, dlg, [m](bool on) {
		m->earthLbl->SetVisibility(on);
		m->moonLbl->SetVisibility(on);
		m->sunLbl->SetVisibility(on);
		m->view->renderWindow()->Render();
	});
	for (QCheckBox *b : {m->solarBox, m->lunarBox, m->penBox})
		QObject::connect(b, &QCheckBox::toggled, dlg, [m](bool) { m->fillList(); });
	QObject::connect(m->list, &QListWidget::itemClicked, dlg, [m](QListWidgetItem *it) {
		m->gotoEvent(it->data(Qt::UserRole).toInt());
	});
	QObject::connect(ovBtn, &QPushButton::clicked, dlg, [m]() { m->overview(); });
	QObject::connect(sunBtn, &QPushButton::clicked, dlg, [m]() { m->camMode = 1; m->update(); });
	QObject::connect(moonBtn, &QPushButton::clicked, dlg, [m]() { m->camMode = 2; m->update(); });
	QObject::connect(eFromMBtn, &QPushButton::clicked, dlg, [m]() { m->camMode = 3; m->update(); });
	QObject::connect(m->sideBtn, &QToolButton::clicked, dlg, [m]() { m->setSide(!m->side->isVisible()); });

	// PARKED LIKE EVERY TOOL WINDOW (parkTool / parkOnMinimise, 50_scene.cpp).
	struct CloseParks : QObject {
		EclipseDialog *d;
		CloseParks(QObject *p, EclipseDialog *dd) : QObject(p), d(dd) {}
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
		if (g_eclipse == m) g_eclipse = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	wrapTooltips(dlg);
	g_eclipse = m;
	m->setSide(true);
	// The eclipse list: two centuries of syzygies (the work of a moment, but never with a dead window).
	QApplication::setOverrideCursor(Qt::WaitCursor);
	m->events = eclSearch(ephJD(QDateTime(QDate(1900, 1, 1), QTime(0, 0), Qt::UTC)),
	                      ephJD(QDateTime(QDate(2100, 12, 31), QTime(23, 59), Qt::UTC)));
	QApplication::restoreOverrideCursor();
	m->jd = ephJD(m->when->dateTime());
	m->update();
	m->fillList();
	m->overview();
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
