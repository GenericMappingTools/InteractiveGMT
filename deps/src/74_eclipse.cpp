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

// The user's own place, from GMT.jl's whereami() (src/whereami.jl): out[0..1] = lon, lat; the place's
// name ("City, Country") to `name`. 1, or 0 when it could not be found.
using JuliaWhereamiFn = int (*)(double *, char *, int);
static JuliaWhereamiFn g_juliaWhereami = nullptr;

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

static const double kEclRate = 12.190749;                  // mean Sun-Moon elongation rate, deg/day

// The instant (TT) near `t` when the elongation is `target` (0 new Moon, 180 full Moon): Newton.
static double eclSyzygy(double t, double target) {
	for (int i = 0; i < 8; ++i) {
		const double dt = eclElong(t, target) / kEclRate;
		t -= dt;
		if (std::fabs(dt) < 1e-6) break;
	}
	return t;
}

// Every eclipse with greatest eclipse between the UT Julian Dates `jd0` and `jd1`.
static std::vector<EclEvent> eclSearch(double jd0, double jd1) {
	std::vector<EclEvent> out;
	double t = ephTT(jd0);
	const double t1 = ephTT(jd1);
	// The first syzygy after jd0, new or full, then alternately the other, half a synodic month apart.
	const double e0 = eclElong(t, 0.0);
	double target = e0 < 0.0 ? 0.0 : 180.0;
	t += (target - e0) / kEclRate;
	while (t < t1 + 1.0) {
		t = eclSyzygy(t, target);
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

// The Moon's disc as a place sees it: the lit part from the phase, turned so that "up" is that place's
// zenith -- a crescent's tilt changes with the latitude and the hour, and is upside down south of the
// equator. Plain painting: the disc has no texture.
class MoonPhaseView : public QWidget {
public:
	double illum = 0.5;              // lit fraction, 0..1
	double bx = 1.0, by = 0.0;       // the bright limb's direction on screen (x right, y down)
	bool up = true;                  // the Moon above the horizon
	explicit MoonPhaseView(QWidget *parent) : QWidget(parent) { setFixedSize(160, 160); }
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), QColor(8, 8, 20));
		const double r = 0.4 * std::min(width(), height());
		p.translate(width() / 2.0, height() / 2.0 - (up ? 0.0 : 6.0));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(48, 48, 56));                       // the night side (earthshine)
		p.drawEllipse(QPointF(0, 0), r, r);
		// The lit part, in a frame whose +x is the bright limb: the limb's half circle out, the terminator
		// (a half ellipse, r * (1 - 2 * lit) wide) back.
		p.save();
		p.rotate(std::atan2(by, bx) / kEphRad);
		QPainterPath lit;
		const double w = r * (1.0 - 2.0 * std::clamp(illum, 0.0, 1.0));
		const int n = 90;
		for (int i = 0; i <= n; ++i) {
			const double f = (-90.0 + 180.0 * i / n) * kEphRad;
			const QPointF q(r * std::cos(f), r * std::sin(f));
			if (i == 0) lit.moveTo(q);
			else lit.lineTo(q);
		}
		for (int i = n; i >= 0; --i) {
			const double f = (-90.0 + 180.0 * i / n) * kEphRad;
			lit.lineTo(w * std::cos(f), r * std::sin(f));
		}
		lit.closeSubpath();
		p.setBrush(up ? QColor(236, 232, 218) : QColor(140, 138, 130));
		p.drawPath(lit);
		p.restore();
		if (!up) {
			p.resetTransform();
			p.setPen(QColor(200, 160, 90));
			p.drawText(QRect(0, height() - 18, width(), 16), Qt::AlignCenter, "below the horizon");
		}
	}
};

class EclipseView : public GLView {
public:
	vtkRenderer *ren = nullptr;                // the scene's renderer, for Shift+drag panning
	EclipseView(QWidget *parent) : GLView() { setParent(parent); }
protected:
	bool panning = false;
	QPoint panLast;
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
	// Shift+left-drag pans (a plain drag rotates).
	void mousePressEvent(QMouseEvent *e) override {
		if (ren && e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier)) {
			panning = true;
			panLast = e->position().toPoint();
			return;
		}
		GLView::mousePressEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (panning) {
			const QPoint cp = e->position().toPoint();
			double ox, oy, nx, ny;
			devPx(panLast, ox, oy);
			devPx(cp, nx, ny);
			camPanByDisplay(ren, ox, oy, nx, ny);
			ren->ResetCameraClippingRange();
			renderWindow()->Render();
			panLast = cp;
			return;
		}
		GLView::mouseMoveEvent(e);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (panning && e->button() == Qt::LeftButton) {
			panning = false;
			return;
		}
		GLView::mouseReleaseEvent(e);
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
	QPushButton *animBtn = nullptr;          // "Animate eclipse": the one at the instant shown, start to end
	double animBegin = 0.0, animEnd = 0.0, animStep = 0.0;   // its contacts (UT JD; animEnd 0 = not running)
	double animFinish = 0.0;                 // the last animated eclipse's last contact (kept after it ends)
	bool animFollow = false;                 // a lunar one: the camera follows the Moon
	QCheckBox *backBox = nullptr, *conesBox = nullptr, *pathBox = nullptr,
	          *solarBox = nullptr, *lunarBox = nullptr, *penBox = nullptr;
	QLabel *info = nullptr;
	QListWidget *list = nullptr;
	QWidget *side = nullptr;
	QToolButton *sideBtn = nullptr;
	QTimer *timer = nullptr;
	QSlider *slider = nullptr;               // time, smoothly: one lunar month round sliderMid
	QComboBox *place = nullptr;              // "Moon from a place": a city, or the lat/lon boxes
	QDoubleSpinBox *lat = nullptr, *lon = nullptr;
	MoonPhaseView *phase = nullptr;
	QLabel *moonInfo = nullptr;
	bool parked = false;
	bool reallyClose = false;
	double jd = 2451545.0;                   // the instant shown (UT)
	double sliderMid = 2451545.0;            // the slider's centre (UT)
	int camMode = 0;                         // the last camera placed: 0 free, 1 Earth from the Sun, 2 Moon from the Earth, 3 Earth from the Moon
	double S[3] = {1, 0, 0}, M[3] = {60, 0, 0};   // Sun and Moon now (scene units)
	double moonUmbra = 0.0;                  // share of the Moon's sunward half in the Earth's umbra
	double earthShadow = 0.0;                // largest share of the Sun hidden anywhere on the Earth
	double litFrac = 0.0;                    // the Moon's lit fraction (the panel's "% lit")
	// "Moon from a place", for the tests: altitude, azimuth, age (d), elongation (deg), next rise / set (UT
	// JD, 0 = none within 36 h), next new / full Moon (UT JD).
	double mAlt = 0.0, mAz = 0.0, mAge = 0.0, mElong = 0.0, mRise = 0.0, mSet = 0.0, mNew = 0.0, mFull = 0.0;
	std::vector<EclEvent> events;

	vtkSmartPointer<vtkRenderer> ren, labRen;
	vtkSmartPointer<vtkActor> earth, moon, sunLine, path, ecliptic, nodes;
	vtkSmartPointer<vtkActor> cone[4];       // Earth umbra, Earth penumbra, Moon umbra, Moon penumbra
	vtkSmartPointer<vtkPolyData> conePD[4], pathPD, eclPD, nodePD;
	vtkSmartPointer<vtkActor> edge[2];       // a solar eclipse on the Earth's surface: umbra, penumbra edges
	vtkSmartPointer<vtkPolyData> edgePD[2];
	// "Animate eclipse"'s track on the Earth, in the EARTH's frame (unit sphere; the actor wears earthM, so
	// it turns with the globe): the line of greatest eclipse, and the umbra's north and south limits
	// (a NaN x = no umbra on the Earth at that frame); in light grey, the penumbra's limits (the partial
	// eclipse's zone).
	vtkSmartPointer<vtkActor> track, trackPen;
	vtkSmartPointer<vtkPolyData> trackPD, trackPenPD;
	std::vector<std::array<double, 3>> trC, trN, trS, trPN, trPS;
	double eAx[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // the Earth's axes now, in the ecliptic frame
	vtkSmartPointer<vtkMatrix4x4> earthM, moonM;
	vtkSmartPointer<vtkLineSource> sunSrc;
	EclSkin skin[2];                         // 0 Earth, 1 Moon
	vtkSmartPointer<vtkBillboardTextActor3D> sunLbl, nodeLbl[2];

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

	// Where a shadow cone of the Moon (radius Rm + slope * x at x along `um` from the Moon; past an
	// umbra's tip, the antumbra) meets the Earth's surface: its edge, on the sunward side only. True
	// geometry, the same the skins are darkened with, put on the globe as it is drawn.
	void shadowEdge(vtkPolyData *pd, const double um[3], double slope) {
		const double Rm = kEphMoonRkm / kEclRE, R = drawR(0) * 1.006;
		double ex[3], ey[3];
		const double z[3] = {0, 0, 1};
		vtkMath::Cross(z, um, ex);
		if (vtkMath::Normalize(ex) == 0.0) ex[0] = 1.0;
		vtkMath::Cross(um, ex, ey);
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		std::vector<vtkIdType> run;
		auto flush = [&]() {
			if (run.size() > 1) lines->InsertNextCell(vtkIdType(run.size()), run.data());
			run.clear();
		};
		const int n = 360;
		for (int i = 0; i <= n; ++i) {
			const double t = 2.0 * vtkMath::Pi() * i / n;
			double A[3], B[3];                                // the cone's generator: A + x * B
			for (int a = 0; a < 3; ++a) {
				const double e = std::cos(t) * ex[a] + std::sin(t) * ey[a];
				A[a] = M[a] + Rm * e;
				B[a] = um[a] + slope * e;
			}
			const double qa = eclDot(B, B), qb = 2.0 * eclDot(A, B), qc = eclDot(A, A) - 1.0;
			const double disc = qb * qb - 4.0 * qa * qc;
			const double x = disc < 0.0 ? -1.0 : (-qb - std::sqrt(disc)) / (2.0 * qa);
			if (x < 0.0) {                                    // this generator misses the Earth
				flush();
				continue;
			}
			double P[3];
			for (int a = 0; a < 3; ++a) P[a] = R * (A[a] + x * B[a]);
			run.push_back(pts->InsertNextPoint(P));
		}
		flush();
		pd->SetPoints(pts);
		pd->SetLines(lines);
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
				eAx[0][i] = e1[i];
				eAx[1][i] = e2[i];
				eAx[2][i] = e3[i];
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
		// Shadow cones, as wide as the bodies are drawn ("Size ×" times the true width), their tips where
		// they truly are. An umbra ends at its apex; the penumbrae (and the Earth's umbra, apex ~217 radii)
		// run far past the Moon's orbit, so no cone is seen ending in empty space.
		{
			const double Rs = kEphSunRkm / kEclRE, Rm = kEphMoonRkm / kEclRE, Re = kEclDanjon;
			const double reach = 400.0, sz = sizeBox->value();
			const double dS = eclNorm(S);
			double u[3];
			for (int i = 0; i < 3; ++i) u[i] = -S[i] / dS;
			coneMesh(conePD[0], origin, u, sz * Re, -sz * (Rs - Re) / dS, std::min(Re * dS / (Rs - Re), reach));
			coneMesh(conePD[1], origin, u, sz * Re, sz * (Rs + Re) / dS, reach);
			double SM[3], um[3];
			for (int i = 0; i < 3; ++i) SM[i] = M[i] - S[i];
			const double dSM = eclNorm(SM);
			for (int i = 0; i < 3; ++i) um[i] = SM[i] / dSM;
			coneMesh(conePD[2], M, um, sz * Rm, -sz * (Rs - Rm) / dSM, std::min(Rm * dSM / (Rs - Rm), reach));
			coneMesh(conePD[3], M, um, sz * Rm, sz * (Rs + Rm) / dSM, reach);
			shadowEdge(edgePD[0], um, -(Rs - Rm) / dSM);
			shadowEdge(edgePD[1], um, (Rs + Rm) / dSM);
			// From the Earth's sunward surface, never from inside it (a line through the globe shows on it).
			const double r0 = drawR(0) * 1.01;
			sunSrc->SetPoint1(r0 * -u[0], r0 * -u[1], r0 * -u[2]);
			sunSrc->SetPoint2(75.0 * -u[0], 75.0 * -u[1], 75.0 * -u[2]);
			sunLbl->SetPosition(80.0 * -u[0], 80.0 * -u[1], 80.0 * -u[2]);
		}
		buildPath(tt);
		// A change of time moves the bodies and nothing else: the camera stays where it is.
		updateInfo(tt);
		updatePlace(tt);
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// Before every render: the near plane as far out as the nearest body allows. VTK's own fit sets it at
	// a thousandth of the far plane (the long cones push that far away), and with so little depth
	// precision the terminator rings and the lines lose to the globes they lie on.
	void fixClip() {
		if (!sizeBox) return;
		vtkCamera *cam = ren->GetActiveCamera();
		double p[3], pm[3], r[2];
		cam->GetPosition(p);
		cam->GetClippingRange(r);
		for (int i = 0; i < 3; ++i) pm[i] = p[i] - M[i];
		const double nearest = std::min(eclNorm(p) - drawR(0), eclNorm(pm) - drawR(1));
		if (nearest > 0.0) r[0] = std::max(r[0], 0.3 * nearest);
		cam->SetClippingRange(r[0], std::max(r[1], 1.01 * r[0]));
	}

	// A camera preset, placed ONCE (its button, or going to an eclipse); time changes never move it.
	void applyCamera() {
		placeCamera();
		ren->ResetCameraClippingRange();
		view->renderWindow()->Render();
	}

	// Where each camera preset stands, for the instant shown.
	void placeCamera() {
		// The Sun line runs straight through the camera of "Earth from the Sun": hidden there.
		sunLine->SetVisibility(camMode != 1);
		sunLbl->SetVisibility(camMode != 1);
		if (camMode == 0) return;
		vtkCamera *cam = ren->GetActiveCamera();
		double s[3], m[3];
		for (int i = 0; i < 3; ++i) s[i] = S[i];
		vtkMath::Normalize(s);
		const double dm = eclNorm(M);
		for (int i = 0; i < 3; ++i) m[i] = M[i] / dm;
		// Each preset frames its body as it is DRAWN: the distances follow the "Size ×" box.
		const double sz = sizeBox->value();
		if (camMode == 1) {                                   // the Earth from the Sun's side
			cam->SetFocalPoint(0, 0, 0);
			cam->SetPosition(9.0 * sz * s[0], 9.0 * sz * s[1], 9.0 * sz * s[2]);
			cam->SetViewAngle(30.0);
		}
		else if (camMode == 2) {                              // the Moon from the Earth
			cam->SetFocalPoint(M);
			cam->SetPosition(M[0] - 3.0 * sz * m[0], M[1] - 3.0 * sz * m[1], M[2] - 3.0 * sz * m[2]);
			cam->SetViewAngle(30.0);
		}
		else {                                                // the Earth from beside the Moon
			cam->SetFocalPoint(0, 0, 0);
			const double p[3] = {M[0] * 0.85, M[1] * 0.85, M[2] * 0.85 + 2.0};
			cam->SetPosition(p);
			cam->SetViewAngle(std::min(80.0, 2.0 * std::atan(5.4 * drawR(0) / eclNorm(p)) / kEphRad));
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
		litFrac = 0.5 * (1.0 - ce);
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
			.arg(dM, 0, 'f', 0).arg(dM / kEclRE, 0, 'f', 2).arg(100.0 * litFrac, 0, 'f', 0).arg(dS, 0, 'f', 4)
			.arg(now));
	}

	// The Moon's direction from the place (east, north, up; unit) at the UT Julian Date `t`, and the Sun's.
	void placeDirs(double t, double m[3], double s[3]) const {
		const double tt = ephTT(t), la = lat->value(), lo = lon->value();
		double sq[3], mq[3], H[3][3];
		ephGeocentric(SKY_SUN, tt, sq);
		ephGeocentric(SKY_MOON, tt, mq);
		ephTopocentric(t, tt, la, lo, mq);
		ephHorizonMatrix(t, tt, la, lo, H);
		for (int i = 0; i < 3; ++i) {
			m[i] = H[i][0] * mq[0] + H[i][1] * mq[1] + H[i][2] * mq[2];
			s[i] = H[i][0] * sq[0] + H[i][1] * sq[1] + H[i][2] * sq[2];
		}
		vtkMath::Normalize(m);
		vtkMath::Normalize(s);
	}

	// The next instant within 36 h after `jd` when the Moon's centre crosses the rise/set altitude going
	// up (`rising`) or down; 0 = none. -0.833 deg: the upper limb on a horizon lifted by refraction.
	double nextCrossing(bool rising) const {
		const double h0 = -0.833;
		auto alt = [this](double t) {
			double m[3], s[3], a, z;
			placeDirs(t, m, s);
			ephAltAz(m, a, z);
			return a;
		};
		const double step = 10.0 / 1440.0;
		double t0 = jd, a0 = alt(t0) - h0;
		for (double t1 = jd + step; t1 <= jd + 1.5; t1 += step) {
			const double a1 = alt(t1) - h0;
			if ((rising && a0 < 0.0 && a1 >= 0.0) || (!rising && a0 >= 0.0 && a1 < 0.0)) {
				double lo = t0, hi = t1;
				for (int i = 0; i < 20; ++i) {
					const double mid = 0.5 * (lo + hi);
					if (((alt(mid) - h0) >= 0.0) == rising) hi = mid;
					else lo = mid;
				}
				return 0.5 * (lo + hi);
			}
			t0 = t1;
			a0 = a1;
		}
		return 0.0;
	}

	// "Moon from a place": the disc as that place sees it, and its numbers.
	void updatePlace(double tt) {
		double m[3], s[3], alt, az;
		placeDirs(jd, m, s);
		ephAltAz(m, alt, az);
		// The disc's own axes: "up" is the zenith squared to the line of sight (north if the Moon is AT the
		// zenith), "right" is sight x up; the bright limb points at the Sun along the disc.
		double u[3] = {0.0, 0.0, 1.0}, r[3], b[3];
		const double zm = eclDot(u, m);
		for (int i = 0; i < 3; ++i) u[i] -= zm * m[i];
		if (vtkMath::Normalize(u) < 1e-9) {
			const double n[3] = {0.0, 1.0, 0.0};
			const double nm = eclDot(n, m);
			for (int i = 0; i < 3; ++i) u[i] = n[i] - nm * m[i];
			vtkMath::Normalize(u);
		}
		vtkMath::Cross(m, u, r);
		const double sm = eclDot(s, m);
		for (int i = 0; i < 3; ++i) b[i] = s[i] - sm * m[i];
		vtkMath::Normalize(b);
		phase->illum = litFrac;
		phase->bx = eclDot(b, r);
		phase->by = -eclDot(b, u);
		phase->up = alt > -0.833;
		phase->update();
		// Phase name and age, from the Sun-Moon elongation (0 new, 180 full; growing = waxing).
		double e = eclElong(tt, 0.0);
		if (e < 0.0) e += 360.0;
		static const char *names[8] = {"New Moon", "Waxing crescent", "First quarter", "Waxing gibbous",
		                               "Full Moon", "Waning gibbous", "Last quarter", "Waning crescent"};
		const int sec = int(std::floor((e + 22.5) / 45.0)) % 8;
		const double tNew = eclSyzygy(tt - e / kEclRate, 0.0);
		double tNext = eclSyzygy(tt + (360.0 - e) / kEclRate, 0.0);
		if (tNext <= tt) tNext = eclSyzygy(tNext + 29.530589, 0.0);
		double tFull = eclSyzygy(tt + std::fmod(540.0 - e, 360.0) / kEclRate, 180.0);
		if (tFull <= tt) tFull = eclSyzygy(tFull + 29.530589, 180.0);
		mAlt = alt;
		mAz = az;
		mElong = e;
		mAge = tt - tNew;
		mNew = eclUT(tNext);
		mFull = eclUT(tFull);
		mRise = nextCrossing(true);
		mSet = nextCrossing(false);
		static const char *pts[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW",
		                              "W", "WNW", "NW", "NNW"};
		auto when = [](double t) {
			return t > 0.0 ? ephDateTime(t).toString("yyyy-MM-dd HH:mm") : QString("none within 36 h");
		};
		moonInfo->setText(QString("<b>%1</b>, %2% lit, %3 days old<br>Altitude %4°, azimuth %5° (%6)%7<br>"
		                          "Next rise: %8<br>Next set: %9<br>New Moon: %10<br>Full Moon: %11<br>"
		                          "<small>Times in UTC.</small>")
			.arg(names[sec]).arg(100.0 * litFrac, 0, 'f', 0).arg(mAge, 0, 'f', 1)
			.arg(alt, 0, 'f', 1).arg(az, 0, 'f', 1).arg(pts[int(std::floor(az / 22.5 + 0.5)) % 16])
			.arg(phase->up ? "" : " — <b>below the horizon</b>")
			.arg(when(mRise)).arg(when(mSet))
			.arg(ephDateTime(mNew).toString("yyyy-MM-dd HH:mm")).arg(ephDateTime(mFull).toString("yyyy-MM-dd HH:mm")));
	}

	// The place picked: kept in the same settings the Sky viewer stands on (one place for both).
	void savePlace() {
		QSettings st = igmtSettings();
		st.setValue("sky/lat", lat->value());
		st.setValue("sky/lon", lon->value());
		st.setValue("sky/place", place->currentText());
	}

	// "My location": where the user is, asked of GMT.jl's whereami() once per session (a web lookup),
	// put first in the place list and picked.
	void setHome() {
		static bool found = false;
		static double ll[2] = {0.0, 0.0};
		static QString name;
		if (!found && g_juliaWhereami) {
			showBusyDialog("Finding your location...");
			char buf[256] = {};
			found = g_juliaWhereami(ll, buf, int(sizeof(buf))) == 1;
			closeBusyDialog();
			name = QString::fromUtf8(buf);
		}
		if (!found || !dlg) return;
		const QString txt = "My location" + (name.isEmpty() ? QString() : " (" + name + ")");
		if (place->findText(txt) < 0) place->insertItem(1, txt, QVariant(QPointF(ll[1], ll[0])));
		place->setCurrentIndex(place->findText(txt));   // sets the lat/lon boxes, saves, redraws
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
		clearTrack();                                         // another eclipse: the last one's track goes
		setJD(events[size_t(i)].jdUT);
		applyCamera();
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
		animEnd = 0.0;
		if (animBtn) animBtn->setText("Animate eclipse");
	}

	// The listed eclipse at the instant shown (greatest eclipse within half a day of it), or -1.
	int eventAt() const {
		int best = -1;
		double bd = 0.5;
		for (int i = 0; i < int(events.size()); ++i) {
			const double d = std::fabs(events[size_t(i)].jdUT - jd);
			if (d < bd) {
				bd = d;
				best = i;
			}
		}
		return best;
	}

	// An eclipse's first and last contacts (UT JD): where the list's own test (eclClassify) starts and
	// stops finding it -- out from greatest eclipse a minute at a time, then halved down to a second.
	void contacts(const EclEvent &e, double &t0, double &t1) const {
		auto in = [&](double t) {
			EclEvent x;
			return eclClassify(ephTT(t), e.solar, x);
		};
		for (int dir = -1; dir <= 1; dir += 2) {
			double a = e.jdUT, b = a;
			for (int k = 0; k < 720; ++k) {
				b = a + dir / 1440.0;
				if (!in(b)) break;
				a = b;
			}
			for (int k = 0; k < 11; ++k) {
				const double c = 0.5 * (a + b);
				(in(c) ? a : b) = c;
			}
			(dir < 0 ? t0 : t1) = a;
		}
	}

	// The track's new frame: where the Moon's shadow axis meets the Earth (in a partial eclipse, the point
	// of the Earth nearest it), and the umbra edge's points farthest either side of the way it goes.
	void addTrack() {
		const double nan = std::numeric_limits<double>::quiet_NaN();
		auto body = [this](const double p[3], double s) {     // ecliptic frame -> the Earth's, above its skin
			std::array<double, 3> b;
			for (int i = 0; i < 3; ++i) b[size_t(i)] = s * eclDot(p, eAx[i]);
			return b;
		};
		double SM[3], um[3], Q[3], c[3];
		for (int i = 0; i < 3; ++i) SM[i] = M[i] - S[i];
		const double dSM = eclNorm(SM);
		for (int i = 0; i < 3; ++i) um[i] = SM[i] / dSM;
		const double xq = -eclDot(M, um);
		for (int i = 0; i < 3; ++i) Q[i] = M[i] + xq * um[i];
		const double d = eclNorm(Q);
		if (d < 1.0) {
			const double x = xq - std::sqrt(1.0 - d * d);
			for (int i = 0; i < 3; ++i) c[i] = M[i] + x * um[i];
		}
		else
			for (int i = 0; i < 3; ++i) c[i] = Q[i] / d;
		trC.push_back(body(c, 1.007));
		// The limits: the edge's generator square to the way the shadow moves over the ground, followed down
		// to the Earth -- the umbra's (k 0) and the penumbra's (k 1), north and south. A generator that misses
		// the Earth = no limit there (the zone runs off the Earth on that side).
		const double Rs = kEphSunRkm / kEclRE, Rm = kEphMoonRkm / kEclRE;
		double e[3] = {0, 0, 0};
		bool haveDir = false;
		if (trC.size() >= 2) {
			const auto &a = trC[trC.size() - 2], &b = trC.back();
			double dir[3] = {0, 0, 0}, side[3];
			for (int j = 0; j < 3; ++j)                           // the ground motion, Earth frame -> ecliptic
				for (int i = 0; i < 3; ++i) dir[i] += (b[size_t(j)] - a[size_t(j)]) * eAx[j][i];
			vtkMath::Cross(c, dir, side);
			const double sa = eclDot(side, um);
			for (int i = 0; i < 3; ++i) e[i] = side[i] - sa * um[i];
			haveDir = vtkMath::Normalize(e) > 0.0;
		}
		for (int k = 0; k < 2; ++k) {
			const double slope = k == 0 ? -(Rs - Rm) / dSM : (Rs + Rm) / dSM;
			std::array<double, 3> lim[2] = {{nan, 0, 0}, {nan, 0, 0}};
			for (int sd = 0; haveDir && sd < 2; ++sd) {
				double A[3], B[3];                                // the generator through +-e: A + x * B
				for (int i = 0; i < 3; ++i) {
					const double ei = (sd == 0 ? 1.0 : -1.0) * e[i];
					A[i] = M[i] + Rm * ei;
					B[i] = um[i] + slope * ei;
				}
				const double qa = eclDot(B, B), qb = 2.0 * eclDot(A, B), qc = eclDot(A, A) - 1.0;
				const double disc = qb * qb - 4.0 * qa * qc;
				const double x = disc < 0.0 ? -1.0 : (-qb - std::sqrt(disc)) / (2.0 * qa);
				if (x < 0.0) continue;
				double P[3];
				for (int i = 0; i < 3; ++i) P[i] = A[i] + x * B[i];
				lim[sd] = body(P, 1.007);
			}
			(k == 0 ? trN : trPN).push_back(lim[0]);
			(k == 0 ? trS : trPS).push_back(lim[1]);
		}
		buildTrack();
	}

	// One track polydata from point lists (a NaN x breaks the line).
	static void trackLines(vtkPolyData *pd, std::initializer_list<const std::vector<std::array<double, 3>> *> lists) {
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		for (const auto *v : lists) {
			std::vector<vtkIdType> run;
			auto flush = [&]() {
				if (run.size() > 1) lines->InsertNextCell(vtkIdType(run.size()), run.data());
				run.clear();
			};
			for (const auto &p : *v) {
				if (std::isnan(p[0])) {
					flush();
					continue;
				}
				run.push_back(pts->InsertNextPoint(p.data()));
			}
			flush();
		}
		pd->SetPoints(pts);
		pd->SetLines(lines);
		pd->Modified();
	}

	void buildTrack() {
		trackLines(trackPD, {&trC, &trN, &trS});
		trackLines(trackPenPD, {&trPN, &trPS});
	}

	void clearTrack() {
		for (auto *v : {&trC, &trN, &trS, &trPN, &trPS}) v->clear();
		if (trackPD) buildTrack();
	}

	// "Animate eclipse": the eclipse at the instant shown, first contact to last, in 600 frames (~24 s).
	// Its camera is placed once, at the start; a lunar one follows the Moon (as the user turned it).
	void animateEclipse() {
		if (animEnd > 0.0) {
			stopPlay();
			return;
		}
		const int i = eventAt();
		if (i < 0) {
			QMessageBox::information(dlg, dlg->windowTitle(),
			                         "There is no eclipse at the instant shown.\n\nPick one in the list (or go to "
			                         "one with ◀ Eclipse / Eclipse ▶), then animate it.");
			return;
		}
		stopPlay();
		const EclEvent &e = events[size_t(i)];
		double t0 = e.jdUT, t1 = e.jdUT;
		contacts(e, t0, t1);
		animBegin = t0;
		animEnd = animFinish = t1;
		animStep = (t1 - t0) / 600.0;
		animFollow = !e.solar;
		camMode = e.solar ? 1 : 2;
		clearTrack();
		setJD(t0);
		if (e.solar) addTrack();
		applyCamera();
		play->setText("Pause");
		animBtn->setText("Stop");
		timer->start();
	}

	double stepDays() const {
		static const double d[] = {1.0 / 1440.0, 5.0 / 1440.0, 15.0 / 1440.0, 1.0 / 24.0, 6.0 / 24.0, 1.0};
		return d[std::clamp(stepBox->currentIndex(), 0, 5)];
	}

	// Every change of time comes here. The slider keeps its centre while `jd` stays within its month (and
	// always when the slider itself moved); otherwise it re-centres on the new instant.
	void setJD(double v, bool fromSlider = false) {
		const double lo = ephJD(when->minimumDateTime()), hi = ephJD(when->maximumDateTime());
		jd = std::clamp(v, lo, hi);
		{
			QSignalBlocker b(when);
			when->setDateTime(ephDateTime(jd));
		}
		if (!fromSlider) {
			if (std::fabs(jd - sliderMid) * 1440.0 > slider->maximum()) sliderMid = jd;
			QSignalBlocker b(slider);
			slider->setValue(int(std::lround((jd - sliderMid) * 1440.0)));
		}
		update();
	}

	void tick() {
		if (animEnd > 0.0) {                                   // "Animate eclipse" running
			const double t = std::min(jd + animStep, animEnd), M0[3] = {M[0], M[1], M[2]};
			setJD(t);
			if (!animFollow) {                                 // a solar eclipse leaves its track on the Earth
				addTrack();
				view->renderWindow()->Render();
			}
			else {                                             // keep the Moon where it was in the view
				vtkCamera *cam = ren->GetActiveCamera();
				double p[3], f[3];
				cam->GetPosition(p);
				cam->GetFocalPoint(f);
				for (int i = 0; i < 3; ++i) {
					p[i] += M[i] - M0[i];
					f[i] += M[i] - M0[i];
				}
				cam->SetPosition(p);
				cam->SetFocalPoint(f);
				view->renderWindow()->Render();
			}
			if (t >= animEnd) stopPlay();
			return;
		}
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
	m->view->ren = m->ren;
	{
		vtkNew<vtkCallbackCommand> clip;                    // fixClip, before every render (interaction too)
		clip->SetClientData(m);
		clip->SetCallback([](vtkObject *, unsigned long, void *cd, void *) { static_cast<EclipseDialog *>(cd)->fixClip(); });
		m->ren->AddObserver(vtkCommand::StartEvent, clip);
	}
	m->labRen =vtkSmartPointer<vtkRenderer>::New();        // labels: overlay, same camera
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
	const double cEdge[3] = {1.0, 1.0, 1.0};
	for (int k = 0; k < 2; ++k) {                             // a solar eclipse's edges on the Earth, in white
		m->edgePD[k] = vtkSmartPointer<vtkPolyData>::New();
		m->edge[k] = eclActor(m->edgePD[k], cEdge, 1.0, k == 0 ? 3.0 : 1.5);
		m->ren->AddActor(m->edge[k]);
	}
	{                                                         // "Animate eclipse"'s track, turning with the Earth
		const double cTrack[3] = {1.0, 0.3, 0.2};
		m->trackPD = vtkSmartPointer<vtkPolyData>::New();
		m->track = eclActor(m->trackPD, cTrack, 1.0, 2.5);
		m->track->SetUserMatrix(m->earthM);
		m->ren->AddActor(m->track);
		const double cPenTrack[3] = {0.8, 0.8, 0.8};
		m->trackPenPD = vtkSmartPointer<vtkPolyData>::New();
		m->trackPen = eclActor(m->trackPenPD, cPenTrack, 1.0, 1.8);
		m->trackPen->SetUserMatrix(m->earthM);
		m->ren->AddActor(m->trackPen);
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
	m->sunLbl = eclLabel("to the Sun", 12, cSun);
	m->nodeLbl[0] = eclLabel("", 11, cNode);
	m->nodeLbl[1] = eclLabel("", 11, cNode);
	for (auto *a : {m->sunLbl.GetPointer(), m->nodeLbl[0].GetPointer(), m->nodeLbl[1].GetPointer()})
		m->labRen->AddActor(a);

	// ---- the controls ----------------------------------------------------------------------------
	m->when = new QDateTimeEdit(dlg);                         // the instant shown (UTC)
	m->when->setTimeSpec(Qt::UTC);
	m->when->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
	m->when->setMinimumDateTime(QDateTime(QDate(100, 1, 1), QTime(0, 0), Qt::UTC));
	m->when->setMaximumDateTime(QDateTime(QDate(2999, 12, 31), QTime(23, 59), Qt::UTC));
	m->when->setDateTime(QDateTime::currentDateTimeUtc());
	m->when->setCalendarPopup(true);
	m->when->setCurrentSection(QDateTimeEdit::DaySection);   // the arrows step days until another part is clicked
	m->when->setToolTip("The instant shown, in UTC (proleptic Gregorian calendar). The arrows (and the mouse "
	                    "wheel) step the part the cursor is in: click the hour to step hours. The button opens "
	                    "a calendar.");
	m->slider = new QSlider(Qt::Horizontal, dlg);              // time, smoothly, through one lunar month
	m->slider->setRange(-21262, 21262);                       // +-14.77 days (half a synodic month), in minutes
	m->slider->setSingleStep(10);
	m->slider->setPageStep(360);
	m->slider->setToolTip("Drag to move time smoothly through one lunar month: 14.8 days either side of the "
	                      "date in the box. Only the Moon, the Sun's direction and the shadows move; the view "
	                      "stays put. Arrow keys: 10 minutes; Page Up / Down: 6 hours.");
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
	m->animBtn = new QPushButton("Animate eclipse", dlg);     // the eclipse at the instant shown, start to end
	m->animBtn->setToolTip("Run the eclipse at the instant shown from its first contact to its last, in about "
	                       "24 seconds: a solar eclipse's shadow (the white circles) crossing the Earth, leaving "
	                       "its track in red (the line of greatest eclipse and the umbra's limits) and light "
	                       "grey (the limits of the partial zone), or the "
	                       "Moon crossing the Earth's shadow. Its camera is placed once, at the start. Click "
	                       "again (or Pause) to stop.");
	m->sizeBox = new QDoubleSpinBox(dlg);                     // drawn size of the two bodies
	m->sizeBox->setRange(1.0, 10.0);
	m->sizeBox->setDecimals(1);
	m->sizeBox->setValue(2.0);                               // at true scale (×1) the two are specks in the Overview
	m->sizeBox->setPrefix("Size ×");
	m->sizeBox->setToolTip("Draw the Earth and the Moon this many times their size (they open at ×2). The shadow "
	                       "cones widen with them; distances, and where each cone ends, stay true. At ×1 "
	                       "everything is to scale, and the Moon is 60 Earth radii away.");
	m->conesBox = new QCheckBox("Shadow cones", dlg);         // umbra and penumbra of both bodies
	m->conesBox->setChecked(true);
	m->pathBox = new QCheckBox("Moon path && nodes", dlg);    // the Moon's path, the ecliptic and the nodes
	m->pathBox->setChecked(true);
	QPushButton *ovBtn = new QPushButton("Overview", dlg);            // free camera over the system
	QPushButton *sunBtn = new QPushButton("Earth from the Sun", dlg); // camera once: where the Moon's shadow falls
	QPushButton *moonBtn = new QPushButton("Moon from the Earth", dlg);   // camera once: the Moon and the Earth's shadow
	QPushButton *eFromMBtn = new QPushButton("Earth from the Moon", dlg); // camera once
	ovBtn->setToolTip("Free camera over the whole system: the Earth, the Moon's orbit and the shadow cones. "
	                  "Drag to rotate, Shift+drag to pan, wheel to zoom.");
	sunBtn->setToolTip("Look at the Earth's day side from the Sun: the Moon's shadow in a solar eclipse. The camera "
	                   "is placed once; changing the time leaves it there.");
	moonBtn->setToolTip("Look at the Moon from the Earth: its phase, and the Earth's shadow in a lunar eclipse. The "
	                    "camera is placed once; changing the time leaves it there.");
	eFromMBtn->setToolTip("Look at the Earth from beside the Moon. The camera is placed once; changing the time "
	                      "leaves it there.");
	for (QPushButton *b : {nowBtn, m->play, prevBtn, nextBtn, ovBtn, sunBtn, moonBtn, eFromMBtn}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
	m->info = new QLabel(dlg);                                // now: distances, phase, what is eclipsed
	m->info->setTextFormat(Qt::RichText);
	m->info->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	m->info->setWordWrap(true);
	// "Moon from a place": the place is the Sky viewer's (the same settings), picked by city or lat/lon.
	{
		QSettings st = igmtSettings();
		m->place = new QComboBox(dlg);                        // a city: sets the latitude and longitude
		m->place->setMinimumContentsLength(12);
		m->place->setToolTip("Pick a city to see the Moon from; or type a latitude and longitude beside it. "
		                     "\"My location\" is where you are (GMT.jl's whereami); the panel opens there.");
		m->place->addItem("(lat / lon)", QVariant());
		for (const EphCity &c : ephCities()) m->place->addItem(c.name, QVariant(QPointF(c.lat, c.lon)));
		m->lat = new QDoubleSpinBox(dlg);                     // the place's latitude
		m->lat->setRange(-90.0, 90.0);
		m->lat->setDecimals(2);
		m->lat->setPrefix("lat ");
		m->lat->setSuffix("°");
		m->lat->setValue(st.value("sky/lat", 38.72).toDouble());
		m->lat->setToolTip("The place's latitude (north positive).");
		m->lon = new QDoubleSpinBox(dlg);                     // the place's longitude
		m->lon->setRange(-180.0, 180.0);
		m->lon->setDecimals(2);
		m->lon->setPrefix("lon ");
		m->lon->setSuffix("°");
		m->lon->setValue(st.value("sky/lon", -9.13).toDouble());
		m->lon->setToolTip("The place's longitude (east positive).");
		const int k = m->place->findText(st.value("sky/place", "Lisbon").toString());
		m->place->setCurrentIndex(k >= 0 ? k : 0);
	}
	m->phase = new MoonPhaseView(dlg);                        // the Moon's disc as the place sees it
	m->phase->setToolTip("The Moon as seen from the place at the instant shown: its phase, and the tilt of the "
	                     "lit side in that place's sky (up = the zenith). A crescent lies on its back near the "
	                     "equator and turns over south of it.");
	m->moonInfo = new QLabel(dlg);                            // phase name, age, altitude, rise / set, next new / full
	m->moonInfo->setTextFormat(Qt::RichText);
	m->moonInfo->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	m->moonInfo->setWordWrap(true);
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
	row1->addWidget(m->animBtn);
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
	row2->addStretch(1);
	row2->addWidget(ovBtn);
	row2->addWidget(sunBtn);
	row2->addWidget(moonBtn);
	row2->addWidget(eFromMBtn);
	m->side = new QWidget(dlg);                               // the right-hand panel (foldable)
	auto *sideLay = new QVBoxLayout(m->side);
	sideLay->setContentsMargins(0, 0, 0, 0);
	sideLay->addWidget(m->info);
	{
		auto *box = new QGroupBox("Moon from a place", m->side);
		auto *bl = new QVBoxLayout(box);
		bl->setContentsMargins(6, 4, 6, 4);
		auto *pr = new QHBoxLayout;
		pr->addWidget(m->place, 1);
		pr->addWidget(m->lat);
		pr->addWidget(m->lon);
		bl->addLayout(pr);
		auto *mr = new QHBoxLayout;
		mr->addWidget(m->phase, 0, Qt::AlignTop);
		mr->addWidget(m->moonInfo, 1);
		bl->addLayout(mr);
		sideLay->addWidget(box);
	}
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
	{
		auto *row0 = new QHBoxLayout;
		row0->addWidget(new QLabel("−14.8 d", dlg));
		row0->addWidget(m->slider, 1);
		row0->addWidget(new QLabel("+14.8 d", dlg));
		lay->addLayout(row0);
	}
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
	QObject::connect(m->when, &QDateTimeEdit::dateTimeChanged, dlg, [m](const QDateTime &t) { m->setJD(ephJD(t)); });
	QObject::connect(m->slider, &QSlider::valueChanged, dlg, [m](int v) { m->setJD(m->sliderMid + v / 1440.0, true); });
	QObject::connect(m->place, &QComboBox::currentIndexChanged, dlg, [m](int) {
		const QVariant v = m->place->currentData();
		if (v.isValid()) {
			const QPointF p = v.toPointF();
			QSignalBlocker a(m->lat), b(m->lon);
			m->lat->setValue(p.x());
			m->lon->setValue(p.y());
		}
		m->savePlace();
		m->update();
	});
	for (QDoubleSpinBox *s : {m->lat, m->lon})
		QObject::connect(s, &QDoubleSpinBox::valueChanged, dlg, [m](double) {
			{
				QSignalBlocker b(m->place);
				m->place->setCurrentIndex(0);                     // typed by hand: no city any more
			}
			m->savePlace();
			m->update();
		});
	QObject::connect(nowBtn, &QPushButton::clicked, dlg, [m]() { m->setJD(ephJD(QDateTime::currentDateTimeUtc())); });
	QObject::connect(stepLeft, &QToolButton::clicked, dlg, [m]() { m->setJD(m->jd - m->stepDays()); });
	QObject::connect(stepRight, &QToolButton::clicked, dlg, [m]() { m->setJD(m->jd + m->stepDays()); });
	QObject::connect(prevBtn, &QPushButton::clicked, dlg, [m]() { m->stepEvent(-1); });
	QObject::connect(nextBtn, &QPushButton::clicked, dlg, [m]() { m->stepEvent(1); });
	QObject::connect(m->animBtn, &QPushButton::clicked, dlg, [m]() { m->animateEclipse(); });
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
	for (QCheckBox *b : {m->solarBox, m->lunarBox, m->penBox})
		QObject::connect(b, &QCheckBox::toggled, dlg, [m](bool) { m->fillList(); });
	QObject::connect(m->list, &QListWidget::itemClicked, dlg, [m](QListWidgetItem *it) {
		m->gotoEvent(it->data(Qt::UserRole).toInt());
	});
	QObject::connect(ovBtn, &QPushButton::clicked, dlg, [m]() { m->overview(); });
	QObject::connect(sunBtn, &QPushButton::clicked, dlg, [m]() { m->camMode = 1; m->applyCamera(); });
	QObject::connect(moonBtn, &QPushButton::clicked, dlg, [m]() { m->camMode = 2; m->applyCamera(); });
	QObject::connect(eFromMBtn, &QPushButton::clicked, dlg, [m]() { m->camMode = 3; m->applyCamera(); });
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
	m->sliderMid = m->jd;
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
	// "My location", once the host has wired whereami (lazily, on this menu entry; ~10 s at most).
	warmupTool("whereami");
	auto tries = std::make_shared<int>(0);
	auto step = std::make_shared<std::function<void()>>();
	*step = [dlg, tries, step]() {
		if (g_eclipse == nullptr || g_eclipse->dlg != dlg) return;
		if (!g_juliaWhereami && ++*tries < 200) {
			QTimer::singleShot(50, dlg, *step);
			return;
		}
		g_eclipse->setHome();
	};
	QTimer::singleShot(0, dlg, *step);
	return true;
}
