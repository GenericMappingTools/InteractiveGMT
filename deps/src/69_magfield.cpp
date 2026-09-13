// 69_magfield.cpp — Geophysics > Magnetics > "Magnetic field lines (3-D)".
//
// A textured globe with the IGRF field lines of a chosen date traced around it. The maths is NOT
// here: Julia (src/magfield.jl) traces the streamlines through GMT.jl's `magref`, the same IGRF
// entry point the IGRF dialog uses, and hands back one polyline per line plus the total field at
// every point; this file is the window, the tubes and the sphere. The date box is the model
// selector the task asks for — `magref` picks the IGRF generation from the decimal year, so
// "select the date" and "pick the right IGRF model" are one control, not two.
//
// THE VIEW IS THE WINDOW'S VIEW. MagFieldView subclasses GLView (60_profile.cpp) exactly as the
// fault demo does, so left-drag rotates, the wheel zooms and the middle button pans / recentres
// through the SAME camPanByDisplay / camRecenterOnPick every other iGMT 3-D view uses — no second
// implementation of a camera gesture (SACRED_LAW.md, and the second law's "use the shared function
// as it is; the difference goes in the new subclass").
//
// UNITS: one world unit = one Earth radius (Julia divides by the IGRF reference radius, 6371.2 km),
// so the sphere has radius 1 and "max radius" in the dialog reads directly as R_E.
#include <vtkTubeFilter.h>

using JuliaMagLinesFn = int (*)(const char *, double *, int, int *, int, char *, int);
// which: 0 = the Earth picture, 1 = the IGRF total field at the surface for that date.
using JuliaMagTexFn   = int (*)(int, double, unsigned char *, int, int *, char *, int);
// The two dip poles for a date: [lonN, latN, incN, lonS, latS, incS].
using JuliaMagPolesFn = int (*)(double, double *, char *, int);
// The polar-cap shoreline for the 2-D track plot: lon/lat pairs, (NaN, NaN) between segments.
using JuliaMagCoastFn = int (*)(double, double, double *, int, int *, char *, int);
// The land/sea mask of a sector (grdlandmask): one byte per node, GMT's own grid order.
using JuliaMagMaskFn  = int (*)(double, double, double, double, double, unsigned char *, int, int *,
                                char *, int);

// A sector's land mask as it arrives: node (ix, iy) at `ix*ny + iy`, iy counted from the south.
struct MagLandMask {
	std::vector<unsigned char> m;
	int    nx = 0, ny = 0;
	double lon0 = 0.0, lat0 = 0.0, inc = 0.0;
	bool   ok() const { return nx > 1 && ny > 1 && int(m.size()) >= nx * ny; }
	bool   land(int ix, int iy) const { return m[size_t(ix) * size_t(ny) + size_t(iy)] != 0; }
	double lon(int ix) const { return lon0 + inc * ix; }
	double lat(int iy) const { return lat0 + inc * iy; }
};
static JuliaMagLinesFn g_juliaMagLines = nullptr;
static JuliaMagTexFn   g_juliaMagTex   = nullptr;
static JuliaMagPolesFn g_juliaMagPoles = nullptr;
static JuliaMagCoastFn g_juliaMagCoast = nullptr;
static JuliaMagMaskFn  g_juliaMagMask  = nullptr;

// The most field lines one view will draw. The dialog's own spin boxes cannot ask for more
// (longitudes x rings x 2 hemispheres), and Julia refuses anything above it rather than overrun
// the counts array this side allocates.
static const int kMagMaxLines = 4096;

// Same one addition GLView cannot know about as in the fault demo: this dialog is full of spin
// boxes that keep the keyboard, so entering the view takes the focus back — without it the view
// keys go to a spin box instead of the camera.
class MagFieldView : public GLView {
public:
	MagFieldView(QWidget *parent = nullptr) : GLView() {
		setParent(parent);
		setMouseTracking(true);     // the pole labels only show under the cursor, with no button down
	}
	// Called with the cursor in VTK DISPLAY coordinates (origin bottom-left, device pixels), or
	// (-1,-1) when the pointer leaves the view. The picking itself is not done here: this class
	// knows about a widget, magFieldHoverPoles knows about the poles.
	std::function<void(double, double)> onHover;
protected:
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
	}
	void leaveEvent(QEvent *e) override {
		if (onHover) onHover(-1.0, -1.0);
		GLView::leaveEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (onHover) {
			const double dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
			onHover(e->position().x() * dpr, (height() - e->position().y()) * dpr);
		}
		GLView::mouseMoveEvent(e);
	}
};

struct MagField {
	QDialog       *dialog = nullptr;
	MagFieldView  *view   = nullptr;
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkActor>    globe;      // the textured sphere
	vtkSmartPointer<vtkActor>    tubes;      // the field lines
	vtkSmartPointer<vtkPolyData> lines;      // their polylines, before the tube filter
	vtkSmartPointer<vtkTubeFilter> tubeFlt;
	vtkSmartPointer<vtkLookupTable> lut;
	// The two skins the sphere can wear, KEPT here rather than read back off the actor: the combo box
	// takes the texture off the globe, and an actor with no texture has none to put back. Each is
	// built on demand and the IGRF one is dropped whenever the date changes, since it IS the date.
	vtkSmartPointer<vtkTexture>  texEarth, texIgrf;
	double texIgrfDate = -1e9;
	// The dip poles: a marker each, on the surface, with a stick and a label.
	vtkSmartPointer<vtkActor> poleMark[2], polePin[2];
	vtkSmartPointer<vtkBillboardTextActor3D> poleLabel[2];
	double poleLonLat[2][2] = {{0,0},{0,0}};
	bool   polesKnown = false;
	int    poleHover  = -1;        // which pole ball the cursor is on, -1 = none: the label's ONLY trigger
	int    trailYear  = -1;        // the year under the cursor on a trail, -1 = not on one
	int    hoverCalls = 0;         // how often the view has reported a cursor position (test probe)
	// The pole-track animation: the whole track in hand (one row per year, fetched once), a timer
	// walking it, and the path each pole has drawn so far.
	std::vector<std::array<double, 7>> track;
	vtkSmartPointer<vtkActor>    trail[2];
	vtkSmartPointer<vtkActor>    geoStar[2];       // the GEOGRAPHIC poles, so the offset can be seen
	// The year under the cursor when it is on a trail: a track line is a row of years, and the only
	// question anyone asks of a point on it is "when".
	vtkSmartPointer<vtkBillboardTextActor3D> trailTag;
	vtkSmartPointer<vtkPolyData> trailPD[2];
	std::vector<double> coastN, coastS;      // the two sectors' shoreline, for the 2-D plot
	MagLandMask maskN, maskS;                // ...and the land under it
	QTimer *anim    = nullptr;
	int     animRow = 0;
	bool    animOn  = false;
	// The animation ENDS holding its last year: the field lines stay away and the year stays on the
	// window, so the globe can be turned and zoomed around the bare track for as long as the user
	// likes. "Restore field lines" — an explicit press, never a mouse gesture that would end the
	// inspection the moment it began — is what puts the 3-D view back.
	bool    restorePending = false;
	// The view swings onto the pole ONCE, for the first run. After that the camera is the user's: a
	// second run plays wherever they have left it.
	bool    poleFramed = false;
	// The year over the sphere. A VTK TEXT ACTOR, not a Qt label: the label was a widget floating
	// above the render surface, so vtkWindowToImageFilter — which reads that surface and nothing else
	// — captured frames with no year on them. Inside the scene it is part of every frame, on screen
	// and in the MP4 alike.
	vtkSmartPointer<vtkTextActor> yearTag;
	QString baseTitle;
	// GLView is written against a Scene (it reads the renderer off one for the middle button), so
	// this dialog owns one of its own — the same thing the fault demo does, and for the same reason.
	Scene *viewScene = nullptr;
	QDoubleSpinBox *date = nullptr, *lat0 = nullptr, *lat1 = nullptr, *rmax = nullptr, *tubeR = nullptr;
	QSpinBox       *nlon = nullptr, *nring = nullptr;
	QComboBox      *skin = nullptr;          // what the sphere wears: Earth picture / IGRF field / plain
	QCheckBox      *colorByB = nullptr, *showPoles = nullptr;
	QPushButton    *trackButton = nullptr, *trackPlotButton = nullptr, *restoreLinesButton = nullptr;
	QPushButton    *saveMovieButton = nullptr;
	QLabel         *status = nullptr;
};

// The globe: a lon/lat mesh with EXPLICIT texture coordinates, not vtkTexturedSphereSource. The
// texture Julia packs (_drape_buf, the one texture packer in this project) is row 0 = SOUTH,
// west->east, and t=0 is an image's first row — so u = (lon+180)/360 and v = (lat+90)/180 place it
// with no guessing about which pole a generated sphere's seam and v-direction happen to point at.
static vtkSmartPointer<vtkPolyData> magFieldSphere(int nlon, int nlat) {
	vtkNew<vtkPoints> pts;
	pts->SetDataTypeToFloat();
	vtkNew<vtkFloatArray> tc;
	tc->SetNumberOfComponents(2);
	tc->SetName("TextureCoordinates");
	for (int j = 0; j <= nlat; ++j) {
		const double lat = -90.0 + 180.0 * j / nlat;
		const double clat = std::cos(vtkMath::RadiansFromDegrees(lat));
		const double slat = std::sin(vtkMath::RadiansFromDegrees(lat));
		for (int i = 0; i <= nlon; ++i) {           // the seam column is duplicated: u wraps 0 -> 1
			const double lon = -180.0 + 360.0 * i / nlon;
			const double clon = std::cos(vtkMath::RadiansFromDegrees(lon));
			const double slon = std::sin(vtkMath::RadiansFromDegrees(lon));
			pts->InsertNextPoint(clat * clon, clat * slon, slat);
			const float uv[2] = {float(i) / float(nlon), float(j) / float(nlat)};
			tc->InsertNextTuple(uv);
		}
	}
	vtkNew<vtkCellArray> quads;
	for (int j = 0; j < nlat; ++j)
		for (int i = 0; i < nlon; ++i) {
			const vtkIdType a = vtkIdType(j) * (nlon + 1) + i, b = a + 1;
			const vtkIdType c = a + (nlon + 1), d = c + 1;
			const vtkIdType ids[4] = {a, b, d, c};
			quads->InsertNextCell(4, ids);
		}
	auto pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetPolys(quads);
	pd->GetPointData()->SetTCoords(tc);
	return pd;
}

// Fetch a sphere skin from Julia (two-phase: dims, then bytes) and wrap it in a texture.
// which = 0 the Earth picture, 1 the IGRF total field at the surface for `date`.
static vtkSmartPointer<vtkTexture> magFieldTexture(int which, double date, QString &error) {
	if (!g_juliaMagTex) { error = "The sphere-texture callback is not registered."; return nullptr; }
	int dims[3] = {};
	char message[1024] = {};
	if (!g_juliaMagTex(which, date, nullptr, 0, dims, message, sizeof(message)) ||
	    dims[0] <= 0 || dims[1] <= 0) {
		error = message[0] ? QString::fromUtf8(message) : QString("The sphere texture could not be built.");
		return nullptr;
	}
	const int nx = dims[0], ny = dims[1], nc = dims[2] >= 4 ? 4 : 3;
	vtkNew<vtkImageData> img;
	img->SetDimensions(nx, ny, 1);
	img->AllocateScalars(VTK_UNSIGNED_CHAR, nc);
	auto *dst = static_cast<unsigned char *>(img->GetScalarPointer());
	const int nbytes = nx * ny * nc;
	if (!g_juliaMagTex(which, date, dst, nbytes, dims, message, sizeof(message))) {
		error = QString::fromUtf8(message); return nullptr;
	}
	auto tex = vtkSmartPointer<vtkTexture>::New();
	tex->SetInputData(img);
	tex->InterpolateOn();
	tex->SetWrap(vtkTexture::ClampToEdge);
	return tex;
}

// Ask Julia for the field lines at the dialog's settings and rebuild the polylines. Two-phase, the
// same protocol the fault demo's mesh uses: first call reports the per-line point counts, the
// second fills a buffer of (x, y, z, |B|) rows.
static bool magFieldTrace(MagField *m, QString &error) {
	if (!g_juliaMagLines) { error = "The field-line callback is not registered."; return false; }
	const QString params = QString("%1/%2/%3/%4/%5/%6/1500")
	    .arg(m->date->value(), 0, 'f', 3).arg(m->nlon->value()).arg(m->nring->value())
	    .arg(m->lat0->value(), 0, 'f', 3).arg(m->lat1->value(), 0, 'f', 3)
	    .arg(m->rmax->value(), 0, 'f', 3);
	std::vector<int> counts(kMagMaxLines, 0);
	char message[2048] = {};
	const QByteArray p = params.toUtf8();
	const int nlines = g_juliaMagLines(p.constData(), nullptr, 0, counts.data(), kMagMaxLines,
	                                   message, sizeof(message));
	if (nlines <= 0) {
		error = message[0] ? QString::fromUtf8(message) : QString("No field line could be traced.");
		return false;
	}
	long long npts = 0;
	for (int i = 0; i < nlines; ++i) npts += counts[i];
	std::vector<double> xyzb(size_t(npts) * 4);
	if (!g_juliaMagLines(p.constData(), xyzb.data(), int(xyzb.size()), counts.data(), kMagMaxLines,
	                     message, sizeof(message))) {
		error = QString::fromUtf8(message); return false;
	}
	vtkNew<vtkPoints> pts;
	pts->SetDataTypeToFloat();
	pts->Allocate(vtkIdType(npts));
	vtkNew<vtkFloatArray> field;
	field->SetName("|B| (nT)");
	vtkNew<vtkCellArray> polys;
	size_t k = 0;
	double bmin = 1e30, bmax = -1e30;
	for (int l = 0; l < nlines; ++l) {
		const int n = counts[l];
		if (n < 2) { k += size_t(n) * 4; continue; }
		// Sized with a NAMED count, not `ids(size_t(n))`: that form is a function declaration
		// (the most vexing parse), and the subscript below then fails to compile.
		const size_t np = size_t(n);
		std::vector<vtkIdType> ids(np);
		for (int i = 0; i < n; ++i) {
			ids[size_t(i)] = pts->InsertNextPoint(xyzb[k], xyzb[k+1], xyzb[k+2]);
			const double b = xyzb[k+3];
			field->InsertNextValue(float(b));
			if (b < bmin) bmin = b;
			if (b > bmax) bmax = b;
			k += 4;
		}
		polys->InsertNextCell(vtkIdType(n), ids.data());
	}
	if (!m->lines) m->lines = vtkSmartPointer<vtkPolyData>::New();
	m->lines->SetPoints(pts);
	m->lines->SetLines(polys);
	m->lines->GetPointData()->SetScalars(field);
	m->lines->Modified();
	// The colour scale is LOGARITHMIC: |B| runs from a few hundred nT out at the apex to ~65000 at
	// the poles, and a linear ramp over that leaves everything but the footpoints one flat colour.
	if (m->lut && bmax > bmin && bmin > 0.0) {
		m->lut->SetScaleToLog10();
		m->lut->SetTableRange(bmin, bmax);
		m->lut->Build();
	}
	if (m->status)
		m->status->setText(QString("%1 field lines, %2 points, IGRF %3")
		                   .arg(nlines).arg(npts).arg(m->date->value(), 0, 'f', 1));
	return true;
}

// (lon, lat) in degrees -> a point at radius r on the globe, the SAME mapping magFieldSphere builds
// its mesh with, so a marker lands exactly on the skin under it.
static void magFieldGeoToXYZ(double lon, double lat, double r, double p[3]) {
	const double cl = std::cos(vtkMath::RadiansFromDegrees(lat));
	p[0] = r * cl * std::cos(vtkMath::RadiansFromDegrees(lon));
	p[1] = r * cl * std::sin(vtkMath::RadiansFromDegrees(lon));
	p[2] = r * std::sin(vtkMath::RadiansFromDegrees(lat));
}

// Stand pole `k` (0 = north, 1 = south) at lon/lat: the ball on the skin, the stick out of it and
// the (hover-only) label on the stick's tip. THE ONE PLACER — the date's own poles and every frame
// of the track animation move the markers through it, so a marker cannot end up describing one
// thing and reading another.
static void magFieldPlacePole(MagField *m, int k, double lon, double lat) {
	if (!m->poleMark[k]) return;
	m->poleLonLat[k][0] = lon;
	m->poleLonLat[k][1] = lat;
	double onSkin[3], tip[3];
	magFieldGeoToXYZ(lon, lat, 1.0,  onSkin);
	magFieldGeoToXYZ(lon, lat, 1.22, tip);
	m->poleMark[k]->SetPosition(onSkin);
	// The stick is a line source rebuilt in place: a cylinder would need an orientation matrix for
	// no gain at this size.
	auto *pin = vtkLineSource::SafeDownCast(m->polePin[k]->GetMapper()->GetInputAlgorithm());
	if (pin) { pin->SetPoint1(onSkin); pin->SetPoint2(tip); pin->Update(); }
	m->poleLabel[k]->SetPosition(tip);
	m->poleLabel[k]->SetInput(QString("%1 magnetic\n%2°, %3°")
	                          .arg(k == 0 ? "North" : "South")
	                          .arg(lon, 0, 'f', 2).arg(lat, 0, 'f', 2).toUtf8().constData());
}

// Ask Julia where the two DIP poles are for the dialog's date (the extremum of |inclination|; see
// src/magfield.jl) and stand a marker on each: a ball on the skin, a stick out of it, and a label.
// The poles MOVE — that is most of what the date box is for — so this runs with every Compute.
static bool magFieldPoles(MagField *m, QString &error) {
	if (!g_juliaMagPoles) { error = "The magnetic-pole callback is not registered."; return false; }
	double pole[6] = {};
	char message[1024] = {};
	if (!g_juliaMagPoles(m->date->value(), pole, message, sizeof(message))) {
		error = message[0] ? QString::fromUtf8(message)
		                   : QString("The magnetic poles could not be located.");
		return false;
	}
	for (int k = 0; k < 2; ++k) magFieldPlacePole(m, k, pole[3*k], pole[3*k + 1]);
	m->polesKnown = true;
	return true;
}

// Build the skin the combo is asking for, if it is not already in hand. The Earth picture is fetched
// once and kept; the IGRF one IS the date, so it is rebuilt whenever the date box has moved since the
// one we hold. Both are cheap enough to make on demand — and neither is made at all until the user
// actually asks for it.
static void magFieldEnsureSkin(MagField *m) {
	if (!m->skin) return;
	const int want = m->skin->currentIndex();
	QString error;
	if (want == 0 && !m->texEarth) {
		m->texEarth = magFieldTexture(0, m->date->value(), error);
		if (!m->texEarth && !error.isEmpty() && m->status) m->status->setText(error);
	}
	else if (want == 1 && (!m->texIgrf || m->texIgrfDate != m->date->value())) {
		if (auto t = magFieldTexture(1, m->date->value(), error)) {
			m->texIgrf = t;
			m->texIgrfDate = m->date->value();
		}
		else if (!error.isEmpty() && m->status) m->status->setText(error);
	}
}

// Apply the looks the Display block picks: which skin the sphere wears, tubes coloured by |B| or
// plain, and the pole markers on or off.
static void magFieldApplyLooks(MagField *m) {
	if (m->tubes && m->tubes->GetMapper()) {
		auto *mp = m->tubes->GetMapper();
		if (m->colorByB && m->colorByB->isChecked()) {
			mp->SetLookupTable(m->lut);
			mp->SetScalarModeToUsePointData();
			mp->SelectColorArray("|B| (nT)");
			mp->ScalarVisibilityOn();
			if (m->lut) mp->SetScalarRange(m->lut->GetTableRange());
		}
		else {
			mp->ScalarVisibilityOff();
			m->tubes->GetProperty()->SetColor(0.88, 0.88, 0.86);   // the reference figure's silver
		}
	}
	if (m->globe && m->skin) {
		// 0 = the Earth picture, 1 = the IGRF total field at the surface, 2 = no picture at all (a
		// plain body, which is still a globe to read the lines against).
		const int want = m->skin->currentIndex();
		vtkTexture *t = (want == 0) ? m->texEarth.Get() : (want == 1) ? m->texIgrf.Get() : nullptr;
		m->globe->SetTexture(t);
		if (t) m->globe->GetProperty()->SetColor(1.0, 1.0, 1.0);
		else   m->globe->GetProperty()->SetColor(0.45, 0.55, 0.70);
	}
	// THE ANIMATION IS THE POLES AND NOTHING ELSE: while it plays the field lines are put away, which
	// is what "just the pole moving" means — the tubes are not deleted, only hidden, and come back
	// exactly as they were when it stops.
	const bool tracking = m->animOn || m->restorePending;
	if (m->tubes) m->tubes->SetVisibility(tracking ? 0 : 1);
	for (int k = 0; k < 2; ++k)
		if (m->trail[k])
			m->trail[k]->SetVisibility(tracking ? 1 : 0);
	if (m->showPoles) {
		// The markers are the animation's whole subject, so it shows them whatever the check box says
		// — an animation of an invisible pole is not a state this tool can be left in.
		const bool on = (m->showPoles->isChecked() || tracking) && m->polesKnown;
		for (int k = 0; k < 2; ++k) {
			if (m->poleMark[k])  m->poleMark[k]->SetVisibility(on ? 1 : 0);
			if (m->polePin[k])   m->polePin[k]->SetVisibility(on ? 1 : 0);
			// The label is HOVER-ONLY: the globe stays clean, and the text appears over the ball the
			// cursor is on (magFieldHoverPoles sets poleHover).
			if (m->poleLabel[k]) m->poleLabel[k]->SetVisibility(on && m->poleHover == k ? 1 : 0);
		}
	}
}

// The cursor moved over the view (VTK display coordinates, or (-1,-1) on leaving): pick the two pole
// balls ONLY and let the one under the cursor, if any, show its label. Nothing else in the view is in
// the pick list, so a hover over the globe or a tube costs one miss and no repaint.
static void magFieldHoverPoles(MagField *m, double x, double y) {
	int hit = -1;
	const bool shown = m->showPoles && (m->showPoles->isChecked() || m->animOn || m->restorePending);
	if (m->polesKnown && m->renderer && shown && x >= 0.0 && y >= 0.0) {
		vtkNew<vtkCellPicker> pk;
		pk->SetTolerance(0.002);
		pk->PickFromListOn();
		for (int k = 0; k < 2; ++k) if (m->poleMark[k]) pk->AddPickList(m->poleMark[k]);
		if (pk->Pick(x, y, 0.0, m->renderer)) {
			vtkProp3D *p = pk->GetProp3D();
			for (int k = 0; k < 2; ++k) if (p == m->poleMark[k].Get()) hit = k;
		}
	}
	// THE TRAIL ANSWERS "WHEN". A track line is a row of years, so the cursor on it gets the year of
	// the nearest year-point, written where the cursor is. Picked off the trail actors only, and only
	// while a trail is actually showing.
	// A TRACK POINT IS FOUND IN DISPLAY SPACE, NOT BY A PICKER. A trail is a 2-3 pixel polyline and
	// vtkCellPicker/vtkPropPicker walk straight past those (the overlay hover in 10_geometry.cpp hit
	// this years ago and solved it the same way): every year-point of the visible trails is projected
	// to the screen and the nearest one within a few pixels wins. The point must also be FACING the
	// camera — the globe is opaque, and a year on the far side is not what the cursor is over.
	int trailYear = -1;
	double tagAt[3] = {0.0, 0.0, 0.0};
	if (hit < 0 && m->renderer && !m->track.empty() && x >= 0.0 && y >= 0.0) {
		vtkCamera *cam = m->renderer->GetActiveCamera();
		double eye[3] = {0.0, 0.0, 1.0};
		if (cam) cam->GetPosition(eye);
		double best = 16.0 * 16.0;                  // pixels, squared
		for (int k = 0; k < 2; ++k) {
			if (!m->trail[k] || !m->trail[k]->GetVisibility()) continue;
			const int lonCol = (k == 0) ? 1 : 4, latCol = (k == 0) ? 2 : 5;
			const int last = std::min(int(m->track.size()) - 1,
			                          m->animOn ? m->animRow : int(m->track.size()) - 1);
			for (int i = 0; i <= last; ++i) {
				double p[3];
				magFieldGeoToXYZ(m->track[size_t(i)][size_t(lonCol)],
				                 m->track[size_t(i)][size_t(latCol)], 1.004, p);
				if (p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] <= 0.0) continue;   // far side
				m->renderer->SetWorldPoint(p[0], p[1], p[2], 1.0);
				m->renderer->WorldToDisplay();
				double d3[3];
				m->renderer->GetDisplayPoint(d3);
				const double dx = d3[0] - x, dy = d3[1] - y;
				const double d = dx * dx + dy * dy;
				if (d < best) {
					best = d;
					trailYear = int(m->track[size_t(i)][0]);
					std::copy(p, p + 3, tagAt);
				}
			}
		}
	}
	const bool tagChanged = (trailYear != m->trailYear);
	if (hit == m->poleHover && !tagChanged) return;      // nothing moved, nothing to redraw
	m->poleHover  = hit;
	m->trailYear  = trailYear;
	if (m->trailTag) {
		if (trailYear > 0) {
			m->trailTag->SetInput(QString::number(trailYear).toUtf8().constData());
			m->trailTag->SetPosition(tagAt[0] * 1.03, tagAt[1] * 1.03, tagAt[2] * 1.03);
			m->trailTag->SetVisibility(1);
		}
		else {
			m->trailTag->SetVisibility(0);
		}
	}
	magFieldApplyLooks(m);
	if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
}

// The last year the animation can reach: TODAY's year, inside IGRF's own window. The track is a
// record of where the poles HAVE BEEN, so it ends at the present, not at whatever future date the
// date box happens to carry.
static double magFieldTrackEndYear() {
	const double y = double(QDate::currentDate().year());
	return std::min(std::max(y, 1901.0), 2030.0);
}

// Fetch the whole track from Julia, ONCE per dialog: one pole search per year, which is seconds of
// GMT work, and the animation then plays entirely off what came back — no evaluation per frame.
//
// THE NOTICE IS REPAINTED, NOT PUMPED. `qApp->processEvents()` before a call this long runs the
// viewer's own queued events — its pump renders the main iGMT window and Windows hands the front to
// it, which is how this dialog ended up BEHIND the viewer the first time the track was asked for.
// repaint() paints this one label synchronously and touches nothing else; the raise afterwards
// covers whatever the Julia side itself pumps while it works.
static bool magFieldFetchTrack(MagField *m, QString &error) {
	if (!m->track.empty()) return true;
	if (!g_juliaMagPoles) { error = "The magnetic-pole callback is not registered."; return false; }
	const double y0 = 1900.0, y1 = magFieldTrackEndYear();
	const int rows = int(y1 - y0) + 1;
	// A WINDOW OF ITS OWN, not an overlay: a label laid over the view is a sibling of a NATIVE
	// OpenGL widget, and on Windows that widget paints over it — which is why the notice this
	// replaces never actually reached the screen. A top-level says what is happening, counts the
	// years as they are found, and cannot be painted over by anything.
	auto *prog = new QDialog(m->dialog, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
	prog->setWindowTitle("Magnetic pole track");
	auto *plv = new QVBoxLayout(prog);
	auto *ptx = new QLabel(QString("Finding where the magnetic poles stood, %1 to %2.\n"
	                               "One IGRF search per year — a few seconds, computed only once.")
	                       .arg(y0, 0, 'f', 0).arg(y1, 0, 'f', 0), prog);
	auto *bar = new QProgressBar(prog);
	bar->setRange(0, rows);
	auto *yr = new QLabel(prog);
	plv->addWidget(ptx);
	plv->addWidget(bar);
	plv->addWidget(yr);
	prog->resize(380, 130);
	prog->show();
	prog->raise();
	qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
	m->track.clear();
	m->track.reserve(size_t(rows));
	char message[1024] = {};
	int  failed = 0;
	QString firstErr;
	for (int i = 0; i < rows; ++i) {
		const double year = y0 + i;
		double pole[6] = {};
		// THE SAME SEARCH THE MARKERS USE — one year at a time through the poles callback, which is
		// also what makes an honest progress bar possible: the year on screen is the year just found.
		//
		// A YEAR THAT FAILS IS A YEAR THAT FAILS, NOT A TRACK THAT FAILS. Aborting on the first one
		// threw away 126 good years and left the window with no trail at all — nothing to animate and
		// nothing to hover. Missing years are skipped and counted; only an empty result is an error.
		if (!g_juliaMagPoles(year, pole, message, sizeof(message))) {
			if (firstErr.isEmpty())
				firstErr = message[0] ? QString::fromUtf8(message)
				                      : QString("the poles for %1").arg(year, 0, 'f', 0);
			++failed;
			message[0] = '\0';
			continue;
		}
		std::array<double, 7> row = {year, pole[0], pole[1], pole[2], pole[3], pole[4], pole[5]};
		m->track.push_back(row);
		bar->setValue(i + 1);
		yr->setText(QString("%1  —  north %2°, %3°    south %4°, %5°")
		            .arg(year, 0, 'f', 0)
		            .arg(pole[0], 0, 'f', 1).arg(pole[1], 0, 'f', 1)
		            .arg(pole[3], 0, 'f', 1).arg(pole[4], 0, 'f', 1));
		// PUMPED EVERY FEW YEARS, NOT EVERY YEAR. Each pump can run whatever else is queued while a
		// GMT call is in flight, and GMT is not re-entrant; five years of work between pumps keeps the
		// bar moving at a tenth of the risk.
		if ((i % 5) == 0) qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
	}
	prog->close();
	prog->deleteLater();
	if (m->track.empty()) {
		error = firstErr.isEmpty() ? QString("The pole track could not be computed.")
		                           : QString("The pole track could not be computed: %1").arg(firstErr);
		return false;
	}
	if (failed > 0 && m->status)
		m->status->setText(QString("Pole track: %1 years (%2 could not be found).")
		                   .arg(m->track.size()).arg(failed));
	return true;
}

// The path each pole has walked up to (and including) `row`, as a polyline just above the skin so it
// is not z-fought by the sphere it lies on.
static void magFieldTrailTo(MagField *m, int row) {
	for (int k = 0; k < 2; ++k) {
		if (!m->trailPD[k]) continue;
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		const int lonCol = (k == 0) ? 1 : 4, latCol = (k == 0) ? 2 : 5;
		vtkIdType prev = -1;
		for (int i = 0; i <= row && i < int(m->track.size()); ++i) {
			double p[3];
			magFieldGeoToXYZ(m->track[size_t(i)][size_t(lonCol)], m->track[size_t(i)][size_t(latCol)],
			                 1.004, p);
			const vtkIdType id = pts->InsertNextPoint(p);
			// The north pole walks across the date line, so a segment is drawn only between neighbours
			// that are actually close in space — the seam is a jump in longitude, not in position, and
			// a straight chord through the globe is what a blind polyline would draw there.
			if (prev >= 0) {
				double a[3];
				pts->GetPoint(prev, a);
				const double d = vtkMath::Distance2BetweenPoints(a, p);
				if (d < 0.04) {
					const vtkIdType seg[2] = {prev, id};
					lines->InsertNextCell(2, seg);
				}
			}
			prev = id;
		}
		m->trailPD[k]->SetPoints(pts);
		m->trailPD[k]->SetLines(lines);
		m->trailPD[k]->Modified();
	}
}

// One frame: both markers moved to that year's poles, the trails grown to it, and the numbers
// written into the text box — the box is the readout of the animation, so it is filled HERE, by the
// same step that moves the balls.
static void magFieldTrackFrame(MagField *m, int row) {
	if (row < 0 || row >= int(m->track.size())) return;
	const std::array<double, 7> &t = m->track[size_t(row)];
	magFieldPlacePole(m, 0, t[1], t[2]);
	magFieldPlacePole(m, 1, t[4], t[5]);
	m->polesKnown = true;
	magFieldTrailTo(m, row);
	// THE YEAR IS THE READOUT, and it is written where the eye already is: over the sphere, and in
	// the window's own title bar. No panel of numbers — the poles' coordinates are on the markers
	// themselves (hover a ball), which is where they belong.
	const QString year = QString::number(t[0], 'f', 0);
	if (m->yearTag) { m->yearTag->SetInput(year.toUtf8().constData()); m->yearTag->SetVisibility(1); }
	if (m->dialog)  m->dialog->setWindowTitle(m->baseTitle + " — " + year);
	magFieldApplyLooks(m);
	if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
}

// THE ANIMATION LOOKS DOWN A POLE. Which one is not a setting: it is whichever hemisphere the user
// already has in front of them, read off the camera (its height above the equatorial plane), so the
// view swings to the nearer pole and never flips the globe over. Then the cap is zoomed to fill the
// window — a pole track drawn across a quarter of the frame is not what the tool is for.
static void magFieldFaceVisiblePole(MagField *m) {
	if (!m->renderer) return;
	vtkCamera *cam = m->renderer->GetActiveCamera();
	if (!cam) return;
	double pos[3], fp[3];
	cam->GetPosition(pos);
	cam->GetFocalPoint(fp);
	const bool north = (pos[2] - fp[2]) >= 0.0;
	// THE DISTANCE IS COMPUTED, not reset-and-nudged. ResetCamera fits the bounding box of every
	// visible prop (the pole pins reach 1.22 R, the billboards further still) and then leaves its own
	// margin on top, which is why a "zoom factor" after it never filled the window. The globe has
	// radius 1 in world units and VTK's view angle is the VERTICAL one, so a camera at
	// 1/sin(angle/2) puts the sphere's limb exactly on the top and bottom edges — full, every time,
	// at any window size. A parallel camera says the same thing with its scale.
	cam->SetFocalPoint(0.0, 0.0, 0.0);
	cam->SetViewUp(0.0, 1.0, 0.0);          // 0 deg meridian towards the top of the window
	if (cam->GetParallelProjection()) {
		cam->SetParallelScale(1.0);
		cam->SetPosition(0.0, 0.0, north ? 4.0 : -4.0);
	}
	else {
		const double half = vtkMath::RadiansFromDegrees(0.5 * cam->GetViewAngle());
		const double d = 1.0 / std::max(1e-6, std::sin(half));
		cam->SetPosition(0.0, 0.0, north ? d : -d);
	}
	m->renderer->ResetCameraClippingRange();
}

// Stop WALKING, but stay in the animation's end state: the last year's poles, its trail, its year on
// the title bar, and the field lines still away. What comes next is the user's camera.
static void magFieldTrackStop(MagField *m) {
	if (m->anim) m->anim->stop();
	if (!m->animOn && !m->restorePending) return;
	m->animOn = false;
	m->restorePending = true;
	if (m->trackButton) m->trackButton->setText("Animate 1900 → now");
	magFieldApplyLooks(m);
	if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
}

// ...and THIS is the way back to the 3-D view: the markers return to the DATE BOX's poles (the date
// box is what the rest of the tool is about), the trail and the year go away, the field lines come
// back. Triggered by the first wheel or button press after the animation — never on a timer.
static void magFieldTrackRestore(MagField *m) {
	if (m->anim) m->anim->stop();
	if (!m->animOn && !m->restorePending) return;
	m->animOn = false;
	m->restorePending = false;
	if (m->trackButton) m->trackButton->setText("Animate 1900 → now");
	if (m->yearTag) m->yearTag->SetVisibility(0);
	if (m->dialog)  m->dialog->setWindowTitle(m->baseTitle);
	QString err;
	if (!magFieldPoles(m, err) && m->status) m->status->setText(err);
	magFieldApplyLooks(m);
	if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
}

static void magFieldResetView(MagField *m) {
	if (!m->renderer) return;
	// An OBLIQUE opening view, set BEFORE ResetCamera (which only moves the camera along the
	// direction it already has): the rotation axis stands up the screen and the lines arch across
	// it, the way the reference figure reads. Setting the view-up afterwards, with the default
	// camera still looking down +z, asks for an up-vector parallel to the view normal — VTK then
	// resets it itself and warns.
	// CENTRED ON GREENWICH: lon 0 is the +x axis, so the camera sits out along +x, a little above the
	// equator, with the rotation axis up the screen. The tool opens looking at the prime meridian.
	auto *cam = m->renderer->GetActiveCamera();
	cam->SetFocalPoint(0.0, 0.0, 0.0);
	cam->SetPosition(2.6, 0.0, 0.9);
	cam->SetViewUp(0.0, 0.0, 1.0);
	m->renderer->ResetCamera();
	m->renderer->ResetCameraClippingRange();
	if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
}

// The polar cap's shoreline, fetched once per cap and kept with the dialog. A cap that cannot be
// fetched is not an error worth stopping for — the plot is about the track, and it draws without it.
static bool magFieldFetchCoast(std::vector<double> &out, double lat0, double lat1, QString &error) {
	if (!out.empty()) return true;
	if (!g_juliaMagCoast) { error = "The coastline callback is not registered."; return false; }
	int pts = 0;
	char message[1024] = {};
	if (!g_juliaMagCoast(lat0, lat1, nullptr, 0, &pts, message, sizeof(message)) || pts <= 0) {
		error = message[0] ? QString::fromUtf8(message) : QString("No shoreline came back for the cap.");
		return false;
	}
	out.assign(size_t(pts) * 2, 0.0);
	if (!g_juliaMagCoast(lat0, lat1, out.data(), int(out.size()), &pts, message, sizeof(message))) {
		out.clear();
		error = message[0] ? QString::fromUtf8(message) : QString("The cap shoreline could not be read.");
		return false;
	}
	return true;
}


// Record the whole track to an MP4: every year rendered into this window, grabbed off the back
// buffer, and the frames handed to ffmpeg. The FRAMES ARE THE WINDOW — the same magFieldTrackFrame
// the on-screen animation walks, so the film cannot drift from what the tool shows.
//
// ffmpeg is GMT's own encoder (its `movie` module shells out to exactly this binary, and a GMT
// install ships it), so it is looked up on PATH rather than bundled or reimplemented.
static void magFieldSaveMovie(MagField *m) {
	if (m->track.empty()) return;
	const QString ff = QStandardPaths::findExecutable("ffmpeg");
	if (ff.isEmpty()) {
		QMessageBox::warning(m->dialog, "Save animation",
			"ffmpeg was not found on PATH. It ships with GMT (beside gmt.exe) and is what writes the "
			"MP4 — put that folder on PATH and try again.");
		return;
	}
	const QString fn = QFileDialog::getSaveFileName(m->dialog, "Save the pole-track animation",
	                                                prefStartDir("pole_track.mp4"),
	                                                "MP4 video (*.mp4);;All files (*)");
	if (fn.isEmpty()) return;
	rememberStartDir(fn);
	QTemporaryDir tmp;
	if (!tmp.isValid()) {
		QMessageBox::warning(m->dialog, "Save animation", "No temporary folder for the frames.");
		return;
	}
	vtkRenderWindow *rw = m->view ? m->view->renderWindow() : nullptr;
	if (!rw) return;
	auto *prog = new QDialog(m->dialog, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
	prog->setWindowTitle("Save animation");
	auto *plv = new QVBoxLayout(prog);
	plv->addWidget(new QLabel(QString("Rendering %1 frames, one per year.").arg(m->track.size()), prog));
	auto *bar = new QProgressBar(prog);
	bar->setRange(0, int(m->track.size()));
	plv->addWidget(bar);
	auto *yr = new QLabel(prog);
	plv->addWidget(yr);
	prog->resize(360, 120);
	prog->show();
	prog->raise();
	qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
	const bool wasAnim = m->animOn;
	m->animOn = true;                       // the film is the animation: no field lines, trail growing
	for (size_t i = 0; i < m->track.size(); ++i) {
		magFieldTrackFrame(m, int(i));
		rw->Render();
		vtkNew<vtkWindowToImageFilter> w2i;
		w2i->SetInput(rw);
		w2i->ShouldRerenderOff();
		w2i->ReadFrontBufferOff();          // the frame just rendered, not whatever is on the glass
		w2i->Update();
		vtkNew<vtkPNGWriter> wr;
		wr->SetFileName(QDir(tmp.path()).filePath(QString("f%1.png").arg(int(i), 5, 10, QChar('0')))
		                .toUtf8().constData());
		wr->SetInputConnection(w2i->GetOutputPort());
		wr->Write();
		bar->setValue(int(i) + 1);
		yr->setText(QString::number(m->track[i][0], 'f', 0));
		qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
	}
	m->animOn = wasAnim;
	yr->setText("Encoding…");
	qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
	// -vf scale: H.264 needs even dimensions and a window is whatever size the user left it.
	QStringList args;
	args << "-y" << "-framerate" << "10" << "-i" << QDir(tmp.path()).filePath("f%05d.png")
	     << "-c:v" << "libx264" << "-pix_fmt" << "yuv420p"
	     << "-vf" << "scale=trunc(iw/2)*2:trunc(ih/2)*2" << QDir::toNativeSeparators(fn);
	QProcess enc;
	enc.start(ff, args);
	enc.waitForFinished(-1);
	const bool ok = (enc.exitStatus() == QProcess::NormalExit && enc.exitCode() == 0 &&
	                 QFileInfo(fn).size() > 0);
	prog->close();
	prog->deleteLater();
	m->dialog->raise();
	m->dialog->activateWindow();
	if (ok) {
		m->status->setText(QString("Animation written: %1").arg(QFileInfo(fn).fileName()));
	}
	else {
		const QString tail = QString::fromLocal8Bit(enc.readAllStandardError()).right(600);
		QMessageBox::warning(m->dialog, "Save animation",
		                     "ffmpeg could not write the file.\n\n" + tail);
	}
}

// The land mask of a sector, fetched once and kept with the dialog. 0.25 deg is a node every ~28 km
// at the equator and a good deal finer where these sectors are — fine enough that the fill sits under
// the shoreline, coarse enough to be one quick grdlandmask call.
static bool magFieldFetchMask(MagLandMask &mask, double lon0, double lon1, double lat0, double lat1,
                              double inc, QString &error) {
	if (mask.ok()) return true;
	if (!g_juliaMagMask) { error = "The land-mask callback is not registered."; return false; }
	int dims[2] = {0, 0};
	char message[1024] = {};
	if (!g_juliaMagMask(lon0, lon1, lat0, lat1, inc, nullptr, 0, dims, message, sizeof(message)) ||
	    dims[0] < 2 || dims[1] < 2) {
		error = message[0] ? QString::fromUtf8(message) : QString("No land mask came back for the sector.");
		return false;
	}
	mask.nx = dims[0];
	mask.ny = dims[1];
	mask.m.assign(size_t(mask.nx) * size_t(mask.ny), 0);
	if (!g_juliaMagMask(lon0, lon1, lat0, lat1, inc, mask.m.data(), int(mask.m.size()), dims,
	                    message, sizeof(message))) {
		mask.m.clear();
		mask.nx = mask.ny = 0;
		error = message[0] ? QString::fromUtf8(message) : QString("The land mask could not be read.");
		return false;
	}
	mask.lon0 = lon0;
	mask.lat0 = lat0;
	mask.inc  = inc;
	return true;
}

// ONE HEMISPHERE of the 2-D track plot, over the SAME GROUND the published charts use (WDC Kyoto's
// pole_ns / pole_ss): not a whole polar cap but the SECTOR the pole has ever been in — 120 E through
// 180 to 60 W above 60 N, and 100 E to 160 E below 60 S — on an azimuthal EQUIDISTANT projection, so
// the parallels come out evenly spaced and the track's shape is its real shape. Pure QPainter, like
// the profile panel: this is a chart, not a scene.
class MagTrackPanel : public QWidget {
public:
	MagTrackPanel(bool north, const std::vector<std::array<double, 7>> *track,
	              const std::vector<double> *coast, const MagLandMask *mask, QWidget *parent = nullptr)
		: QWidget(parent), m_north(north), m_track(track), m_coast(coast), m_mask(mask) {
		if (m_north) { m_lon0 = 120.0; m_lon1 = 300.0; m_dlon = 30.0; m_latMin =  60.0; }
		else         { m_lon0 = 100.0; m_lon1 = 160.0; m_dlon = 10.0; m_latMin = -60.0; }
		setMinimumSize(380, 400);
		setAutoFillBackground(true);
		setMouseTracking(true);       // the year-under-the-cursor readout needs moves with no button
		QPalette pal = palette();
		pal.setColor(QPalette::Window, QColor(255, 255, 255));
		setPalette(pal);
	}
	double lon0()   const { return m_lon0; }
	double lon1()   const { return m_lon1; }
	double latMin() const { return m_latMin; }
protected:
	// Pole at the origin, distance from it = colatitude (that IS azimuthal equidistant), the sector's
	// middle meridian pointing up. Longitude runs anticlockwise in the north and clockwise in the
	// south — what a reader looking down on each pole sees, and how the published charts are drawn.
	QPointF local(double lon, double lat) const {
		const double colat = m_north ? (90.0 - lat) : (90.0 + lat);
		double d = lon - 0.5 * (m_lon0 + m_lon1);
		while (d < -180.0) d += 360.0;
		while (d >  180.0) d -= 360.0;
		const double a = vtkMath::RadiansFromDegrees(m_north ? -d : d);
		return QPointF(colat * std::sin(a), -colat * std::cos(a));
	}
	QPointF project(double lon, double lat) const {
		const QPointF p = local(lon, lat);
		return QPointF(m_ox + p.x() * m_k, m_oy + p.y() * m_k);
	}
	bool inSector(double lon, double lat) const {
		double d = lon - m_lon0;
		while (d < 0.0)     d += 360.0;
		while (d >= 360.0)  d -= 360.0;
		double span = m_lon1 - m_lon0;
		while (span < 0.0)  span += 360.0;
		if (d > span) return false;
		return m_north ? (lat >= m_latMin) : (lat <= m_latMin);
	}
	// The whole wedge as one path: the two radial edges and the outer arc. Used for the fill, for the
	// outline, and as the CLIP for the shoreline, so all three agree by construction.
	QPainterPath sectorPath() const {
		QPainterPath path;
		path.moveTo(project(m_lon0, m_north ? 90.0 : -90.0));
		for (double l = m_lon0; l <= m_lon1 + 1e-9; l += 1.0) path.lineTo(project(l, m_latMin));
		path.closeSubpath();
		return path;
	}
	// The wedge is fitted to the widget by its OWN bounding box, not by an assumed circle: a 180 deg
	// fan and a 60 deg one have nothing in common but the pole, and both have to fill their panel.
	void fit() {
		double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
		auto acc = [&](const QPointF &p) {
			x0 = std::min(x0, p.x());  x1 = std::max(x1, p.x());
			y0 = std::min(y0, p.y());  y1 = std::max(y1, p.y());
		};
		for (double l = m_lon0; l <= m_lon1 + 1e-9; l += 1.0) acc(local(l, m_latMin));
		acc(local(0.5 * (m_lon0 + m_lon1), m_north ? 90.0 : -90.0));
		const double w = std::max(1e-6, x1 - x0), h = std::max(1e-6, y1 - y0);
		const double marg = 44.0;
		m_k  = std::max(0.1, std::min((width() - 2 * marg) / w, (height() - 2 * marg) / h));
		m_ox = 0.5 * width()  - 0.5 * (x0 + x1) * m_k;
		m_oy = 0.5 * height() - 0.5 * (y0 + y1) * m_k;
	}
	// HOVER THE LINE, READ THE YEAR: the nearest year-point within a few pixels of the cursor, which
	// is the one question a track line raises. Same rule as the globe's trail.
	void mouseMoveEvent(QMouseEvent *e) override {
		int hit = -1;
		if (m_track && !m_track->empty()) {
			const int lonCol = m_north ? 1 : 4, latCol = m_north ? 2 : 5;
			double best = 14.0 * 14.0;
			for (size_t i = 0; i < m_track->size(); ++i) {
				const std::array<double, 7> &t = (*m_track)[i];
				if (!inSector(t[size_t(lonCol)], t[size_t(latCol)])) continue;
				const QPointF q = project(t[size_t(lonCol)], t[size_t(latCol)]);
				const double dx = q.x() - e->position().x(), dy = q.y() - e->position().y();
				const double d = dx * dx + dy * dy;
				if (d < best) { best = d; hit = int(i); }
			}
		}
		if (hit == m_hover) return;
		m_hover = hit;
		update();
	}
	void leaveEvent(QEvent *e) override {
		if (m_hover >= 0) { m_hover = -1; update(); }
		QWidget::leaveEvent(e);
	}
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.fillRect(rect(), QColor(255, 255, 255));
		fit();
		const QPainterPath sector = sectorPath();
		p.fillPath(sector, QColor(233, 241, 246));            // sea inside the wedge
		// THE LAND IS PAINTED FROM THE MASK, not from the shoreline: GMT hands the shoreline over as
		// OPEN ARCS cut at the region's tile edges, and filling those as polygons is what shredded
		// Antarctica. Each mask row becomes runs of land, and a run becomes one quad whose top and
		// bottom edges are sampled along the parallel so they curve with the projection.
		p.save();
		p.setClipPath(sector);
		if (m_mask && m_mask->ok()) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(205, 203, 198));
			const double h = 0.5 * m_mask->inc;
			for (int iy = 0; iy < m_mask->ny; ++iy) {
				int ix = 0;
				while (ix < m_mask->nx) {
					if (!m_mask->land(ix, iy)) { ++ix; continue; }
					int ix2 = ix;
					while (ix2 + 1 < m_mask->nx && m_mask->land(ix2 + 1, iy)) ++ix2;
					const double la = m_mask->lat(iy), l0 = m_mask->lon(ix) - h, l1 = m_mask->lon(ix2) + h;
					const double stp = std::max(m_mask->inc, (l1 - l0) / 24.0);
					QPolygonF q;
					for (double l = l0; l < l1; l += stp) q << project(std::min(l, l1), la + h);
					q << project(l1, la + h);
					for (double l = l1; l > l0; l -= stp) q << project(std::max(l, l0), la - h);
					q << project(l0, la - h);
					p.drawPolygon(q);
					ix = ix2 + 1;
				}
			}
		}
		// The shoreline on top, as LINES (a NaN pair lifts the pen) — the crisp edge over the fill.
		if (m_coast && m_coast->size() >= 4) {
			p.setPen(QPen(QColor(90, 90, 90), 0.9));
			p.setBrush(Qt::NoBrush);
			QPolygonF poly;
			for (size_t i = 0; i + 1 < m_coast->size(); i += 2) {
				const double lon = (*m_coast)[i], lat = (*m_coast)[i + 1];
				if (std::isnan(lon) || std::isnan(lat)) {
					if (poly.size() > 1) p.drawPolyline(poly);
					poly.clear();
					continue;
				}
				poly << project(lon, lat);
			}
			if (poly.size() > 1) p.drawPolyline(poly);
		}
		p.restore();
		// The graticule: a parallel every 5 deg and a meridian every 30 (north) / 10 (south) — the
		// spacing the published charts carry.
		p.setPen(QPen(QColor(90, 90, 90), 0.8));
		const double colatMax = m_north ? (90.0 - m_latMin) : (m_latMin + 90.0);
		for (double c = 5.0; c <= colatMax - 1e-9; c += 5.0) {
			const double lat = m_north ? 90.0 - c : c - 90.0;
			QPainterPath arc;
			for (double l = m_lon0; l <= m_lon1 + 1e-9; l += 1.0) {
				const QPointF q = project(l, lat);
				if (l == m_lon0) arc.moveTo(q); else arc.lineTo(q);
			}
			p.drawPath(arc);
		}
		for (double l = m_lon0; l <= m_lon1 + 1e-9; l += m_dlon)
			p.drawLine(project(l, m_north ? 90.0 : -90.0), project(l, m_latMin));
		p.setPen(QPen(QColor(60, 60, 60), 1.4));
		p.drawPath(sector);
		// Labels: the parallels along the middle meridian, the meridians just outside the rim.
		QFont f = p.font();
		f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
		p.setFont(f);
		p.setPen(QColor(70, 70, 70));
		const double lonMid = 0.5 * (m_lon0 + m_lon1);
		for (double c = 5.0; c <= colatMax - 1e-9; c += 5.0) {
			const double lat = m_north ? 90.0 - c : c - 90.0;
			const QPointF q = project(lonMid, lat);
			p.drawText(QPointF(q.x() + 4.0, q.y() - 3.0),
			           QString("%1°%2").arg(std::abs(lat), 0, 'f', 0).arg(m_north ? "N" : "S"));
		}
		for (double l = m_lon0; l <= m_lon1 + 1e-9; l += m_dlon) {
			const QPointF a = project(l, m_latMin), b = project(l, m_north ? 90.0 : -90.0);
			const double dx = a.x() - b.x(), dy = a.y() - b.y();
			const double len = std::max(1e-6, std::sqrt(dx * dx + dy * dy));
			const QPointF o(a.x() + dx / len * 20.0, a.y() + dy / len * 14.0);
			double lv = l;
			while (lv > 180.0) lv -= 360.0;
			const QString txt = (lv == 0.0) ? "0°" : (std::abs(lv) == 180.0) ? "180°"
			                  : QString("%1°%2").arg(std::abs(lv), 0, 'f', 0).arg(lv > 0 ? "E" : "W");
			p.drawText(QRectF(o.x() - 28.0, o.y() - 9.0, 56.0, 18.0), Qt::AlignCenter, txt);
		}
		if (!m_track || m_track->empty()) return;
		// The track: the same rows the 3-D animation walks, so the two can never disagree.
		const int lonCol = m_north ? 1 : 4, latCol = m_north ? 2 : 5;
		const QColor col = m_north ? QColor(60, 200, 60) : QColor(60, 200, 60);
		QPolygonF line;
		for (const std::array<double, 7> &t : *m_track)
			if (inSector(t[size_t(lonCol)], t[size_t(latCol)]))
				line << project(t[size_t(lonCol)], t[size_t(latCol)]);
		p.setPen(QPen(col, 2.6));
		p.setBrush(Qt::NoBrush);
		p.drawPolyline(line);
		for (const std::array<double, 7> &t : *m_track) {
			if (!inSector(t[size_t(lonCol)], t[size_t(latCol)])) continue;
			const QPointF q = project(t[size_t(lonCol)], t[size_t(latCol)]);
			// A DOT every ten years, a NUMBER every twenty until 2000 and every ten after: before the
			// war the pole barely moved, and ten labels inside a centimetre are unreadable — which is
			// why the published charts thin them out exactly there too.
			if (std::fmod(t[0], 10.0) != 0.0) continue;
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(20, 20, 20));
			p.drawEllipse(q, 3.6, 3.6);
			const bool label = (t[0] >= 2000.0) || std::fmod(t[0], 20.0) == 0.0;
			if (!label) continue;
			p.setPen(QColor(20, 20, 20));
			p.drawText(QPointF(q.x() + 5.0, q.y() - 5.0), QString::number(t[0], 'f', 0));
		}
		// The last year gets a red dot of its own, as the charts mark the current epoch.
		const std::array<double, 7> &last = m_track->back();
		if (inSector(last[size_t(lonCol)], last[size_t(latCol)])) {
			const QPointF q = project(last[size_t(lonCol)], last[size_t(latCol)]);
			p.setBrush(QColor(215, 30, 30));
			p.setPen(QPen(QColor(20, 20, 20), 1.0));
			p.drawEllipse(q, 4.4, 4.4);
			p.setPen(QColor(20, 20, 20));
			p.drawText(QPointF(q.x() + 6.0, q.y() + 12.0), QString::number(last[0], 'f', 0));
		}
		// The year under the cursor, in a little box beside it, with its own ring on the point.
		if (m_hover >= 0 && m_hover < int(m_track->size())) {
			const std::array<double, 7> &t = (*m_track)[size_t(m_hover)];
			const QPointF q = project(t[size_t(lonCol)], t[size_t(latCol)]);
			p.setBrush(QColor(255, 255, 255));
			p.setPen(QPen(QColor(30, 30, 30), 1.2));
			p.drawEllipse(q, 4.0, 4.0);
			const QString txt = QString("%1    %2°, %3°")
				.arg(t[0], 0, 'f', 0)
				.arg(t[size_t(lonCol)], 0, 'f', 2).arg(t[size_t(latCol)], 0, 'f', 2);
			QFontMetricsF fm(p.font());
			const double w = fm.horizontalAdvance(txt) + 10.0, h = fm.height() + 6.0;
			double bx = q.x() + 10.0, by = q.y() - h - 8.0;
			bx = std::min(bx, double(width()) - w - 2.0);
			by = std::max(by, 2.0);
			const QRectF box(bx, by, w, h);
			p.setBrush(QColor(255, 255, 255, 235));
			p.setPen(QPen(QColor(70, 70, 70), 1.0));
			p.drawRoundedRect(box, 3.0, 3.0);
			p.setPen(QColor(20, 20, 20));
			p.drawText(box, Qt::AlignCenter, txt);
		}
		QFont bf = p.font();
		bf.setBold(true);
		p.setFont(bf);
		p.setPen(QColor(20, 20, 20));
		p.drawText(QRectF(0, 2, width(), 22), Qt::AlignHCenter | Qt::AlignTop,
		           m_north ? "North magnetic pole" : "South magnetic pole");
	}
private:
	bool m_north;
	const std::vector<std::array<double, 7>> *m_track;
	const std::vector<double> *m_coast;
	const MagLandMask *m_mask = nullptr;
	double m_lon0 = 0.0, m_lon1 = 0.0, m_dlon = 30.0, m_latMin = 60.0;
	int    m_hover = -1;                    // the track row under the cursor, -1 = none
	mutable double m_ox = 0.0, m_oy = 0.0, m_k = 1.0;
};

// The GMT.jl script that draws these two maps. The plot above is QPainter — fast, and it lives in a
// dialog — but what a user takes AWAY from it is a GMT figure, so the window hands over the calls
// that make one: same region, same projection, same track, GMT's own coastline instead of the
// cap dump this panel drew. The track travels INSIDE the script as a literal matrix, so the file is
// self-contained — a script that names data files the user has to produce first reproduces nothing.
static QString magFieldTrackScript(const MagField *m, const MagTrackPanel *pn, const MagTrackPanel *ps) {
	auto matrix = [m](int lonCol, int latCol) {
		QString s;
		for (size_t i = 0; i < m->track.size(); ++i) {
			const std::array<double, 7> &t = m->track[i];
			s += QString(i == 0 ? "" : "\n     ") +
			     QString("%1 %2 %3").arg(t[0], 8, 'f', 1)
			                        .arg(t[size_t(lonCol)], 10, 'f', 4)
			                        .arg(t[size_t(latCol)], 10, 'f', 4);
		}
		return s;
	};
	const double y0 = m->track.front()[0], y1 = m->track.back()[0];
	const QString span = QString("%1-%2").arg(y0, 0, 'f', 0).arg(y1, 0, 'f', 0);
	QString s;
	s += QString("# The magnetic pole track %1, as iGMT's \"2-D track plot\" draws it.\n").arg(span);
	s += "# Each row is [year lon lat], the IGRF magnetic pole for that year — where the horizontal\n";
	s += "# field vanishes — found on GMT's own magref, the same way the 3-D tool finds it.\n";
	s += "using GMT\n\n";
	s += "N = [" + matrix(1, 2) + "]\n\n";
	s += "S = [" + matrix(4, 5) + "]\n\n";
	s += "dot(T) = findall(y -> mod(y, 10.0) == 0.0, T[:,1])                        # a dot every decade\n";
	s += "lab(T) = findall(y -> y >= 2000 || mod(y, 20.0) == 0.0, T[:,1])           # ...a number less often\n\n";
	for (int k = 0; k < 2; ++k) {
		const bool north = (k == 0);
		const MagTrackPanel *panel = north ? pn : ps;
		const QString var  = north ? "N" : "S";
		const QString name = north ? "pole_north.png" : "pole_south.png";
		// THE SAME SECTOR AND THE SAME PROJECTION the panel above draws — region straight off the
		// panel, azimuthal equidistant (-JE) centred on the sector's middle meridian and its pole.
		const QString reg  = north
			? QString("%1/%2/%3/90").arg(panel->lon0(), 0, 'f', 0).arg(panel->lon1(), 0, 'f', 0)
			                        .arg(panel->latMin(), 0, 'f', 0)
			: QString("%1/%2/-90/%3").arg(panel->lon0(), 0, 'f', 0).arg(panel->lon1(), 0, 'f', 0)
			                         .arg(panel->latMin(), 0, 'f', 0);
		const QString proj = QString("E%1/%2/15c")
			.arg(0.5 * (panel->lon0() + panel->lon1()), 0, 'f', 0).arg(north ? 90 : -90);
		const QString frame = north ? "a30g30f10" : "a10g10f5";
		s += QString("# %1 magnetic pole\n").arg(north ? "North" : "South");
		s += QString("coast(R=\"%1\", J=\"%2\", B=\"%3\", G=\"gray80\", S=\"lightblue\",\n"
		             "      W=\"0.25p,gray40\", title=\"%4 magnetic pole %5\")\n")
		     .arg(reg).arg(proj).arg(frame).arg(north ? "North" : "South").arg(span);
		s += QString("plot!(%1[:,2:3], W=\"2p,green\")\n").arg(var);
		s += QString("plot!(%1[dot(%1),2:3], marker=:circle, ms=\"0.18c\", mc=:black)\n").arg(var);
		s += QString("text!(mat2ds(%1[lab(%1),2:3], string.(Int.(%1[lab(%1),1]))),\n"
		             "      F=\"+f8p,Helvetica,black+jLB\", D=\"0.15c/0.15c\", savefig=\"%2\", show=true)\n\n")
		     .arg(var).arg(name);
	}
	return s;
}

// ...shown in its own window, with a Save that writes it where the user says.
static void magFieldTrackScriptDialog(QWidget *parent, const QString &script) {
	auto *dlg = new QDialog(nullptr);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("GMT commands — magnetic pole track");
	dlg->setWindowIcon(appIcon());
	auto *lay = new QVBoxLayout(dlg);
	auto *box = new QPlainTextEdit(script, dlg);
	box->setReadOnly(true);
	box->setLineWrapMode(QPlainTextEdit::NoWrap);
	QFont mono("Consolas");
	mono.setStyleHint(QFont::Monospace);
	box->setFont(mono);
	lay->addWidget(box, 1);
	auto *row = new QHBoxLayout();
	auto *copy = new QPushButton("Copy", dlg);
	auto *save = new QPushButton("Save script…", dlg);
	auto *close = new QPushButton("Close", dlg);
	row->addStretch(1);
	row->addWidget(copy);
	row->addWidget(save);
	row->addWidget(close);
	lay->addLayout(row);
	QObject::connect(copy,  &QPushButton::clicked, dlg, [script]() { QApplication::clipboard()->setText(script); });
	QObject::connect(close, &QPushButton::clicked, dlg, &QDialog::close);
	QObject::connect(save,  &QPushButton::clicked, dlg, [dlg, script]() {
		const QString fn = QFileDialog::getSaveFileName(dlg, "Save the GMT script",
		                                                prefStartDir("pole_track.jl"),
		                                                "Julia script (*.jl);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		QFile f(fn);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
			QMessageBox::warning(dlg, "Save", "Could not write " + fn);
			return;
		}
		QTextStream(&f) << script;
	});
	if (parent) dlg->resize(std::min(920, parent->width() + 120), 560);
	else        dlg->resize(920, 560);
	dlg->show(); dlg->raise(); dlg->activateWindow();
}

// The 2-D plot window: both caps side by side. It is a VIEW of the track the dialog already holds —
// it never computes one of its own, so opening it after an animation costs nothing.
static void magFieldTrackPlot(MagField *m) {
	QString err;
	if (m->track.empty()) {
		m->status->setText(QString("Computing where the poles stood, 1900-%1…")
		                   .arg(magFieldTrackEndYear(), 0, 'f', 0));
		m->status->repaint();
		const bool ok = magFieldFetchTrack(m, err);   // puts up its own progress window, year by year
		m->dialog->raise();
		m->dialog->activateWindow();
		if (!ok) { m->status->setText(err); return; }
		m->status->setText(QString("Pole track: %1 years.").arg(m->track.size()));
	}
	// The caps are fetched to 50 deg — wider than any panel draws — so a panel that sizes itself to
	// the track (MagTrackPanel's constructor) always has shoreline out to its own rim.
	if (!magFieldFetchCoast(m->coastN,  50.0,  90.0, err) && m->status) m->status->setText(err);
	if (!magFieldFetchCoast(m->coastS, -90.0, -50.0, err) && m->status) m->status->setText(err);
	// ...and the land under it, over each panel's own sector.
	if (!magFieldFetchMask(m->maskN, 120.0, 300.0,  55.0,  90.0, 0.25, err) && m->status)
		m->status->setText(err);
	if (!magFieldFetchMask(m->maskS, 100.0, 160.0, -90.0, -55.0, 0.25, err) && m->status)
		m->status->setText(err);
	auto *dlg = new QDialog(nullptr);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(QString("Magnetic pole track %1-%2")
	                    .arg(m->track.front()[0], 0, 'f', 0).arg(m->track.back()[0], 0, 'f', 0));
	dlg->setWindowIcon(appIcon());
	auto *root = new QVBoxLayout(dlg);
	// The two maps live on a canvas of their own, which is ALSO what Save Image grabs — so the file
	// is the picture the user is looking at, with none of the window's buttons in it.
	auto *canvas = new QWidget(dlg);
	canvas->setAutoFillBackground(true);
	QPalette cpal = canvas->palette();
	cpal.setColor(QPalette::Window, QColor(255, 255, 255));
	canvas->setPalette(cpal);
	auto *lay = new QHBoxLayout(canvas);
	lay->setContentsMargins(0, 0, 0, 0);
	auto *panelN = new MagTrackPanel(true,  &m->track, &m->coastN, &m->maskN, canvas);
	auto *panelS = new MagTrackPanel(false, &m->track, &m->coastS, &m->maskS, canvas);
	lay->addWidget(panelN);
	lay->addWidget(panelS);
	root->addWidget(canvas, 1);
	auto *row = new QHBoxLayout();
	auto *saveImg = new QPushButton("Save image…", dlg);
	auto *saveCmd = new QPushButton("GMT commands…", dlg);
	saveImg->setToolTip("Write the two maps above to a PNG file.");
	saveCmd->setToolTip("The GMT.jl calls that draw the same two maps, track data included.");
	row->addStretch(1);
	row->addWidget(saveImg);
	row->addWidget(saveCmd);
	root->addLayout(row);
	QObject::connect(saveImg, &QPushButton::clicked, dlg, [dlg, canvas]() {
		const QString fn = QFileDialog::getSaveFileName(dlg, "Save the track plot",
		                                                prefStartDir("pole_track.png"),
		                                                "PNG image (*.png);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		// grab() paints the widget at the device pixel ratio it is shown with, so the file is as
		// sharp as the window — no second rendering path to keep in step with paintEvent.
		if (!canvas->grab().save(fn))
			QMessageBox::warning(dlg, "Save", "Could not write " + fn);
	});
	QObject::connect(saveCmd, &QPushButton::clicked, dlg, [dlg, m, panelN, panelS]() {
		magFieldTrackScriptDialog(dlg, magFieldTrackScript(m, panelN, panelS));
	});
	// The panels hold pointers into the dialog's own track, so they cannot outlive it.
	QObject::connect(m->dialog, &QObject::destroyed, dlg, [dlg]() { dlg->close(); });
	dlg->resize(900, 540);
	// SHOWN IN FRONT. A top-level with no parent comes up wherever the window manager feels like on
	// Windows — which, launched from a dialog that is itself over the viewer, is behind both.
	dlg->show(); dlg->raise(); dlg->activateWindow();
}

// Open (or raise) the tool for a window.
static QDialog *magFieldOpen(QWidget *parent, Scene *scene) {
	if (auto *old = qobject_cast<QDialog *>(scene ? scene->magFieldDlg : nullptr)) {
		old->show(); old->raise(); old->activateWindow(); return old;
	}
	auto *m = new MagField;
	QFile file(QDir(gmtvtkUiDir()).filePath("magfield3d.ui"));
	QUiLoader loader;
	QWidget *ui = file.open(QIODevice::ReadOnly) ? loader.load(&file) : nullptr;
	if (!ui) {
		QMessageBox::warning(parent, "Magnetic field lines", "Cannot load " + file.fileName());
		delete m; return nullptr;
	}
	m->date      = ui->findChild<QDoubleSpinBox *>("dateBox");
	m->lat0      = ui->findChild<QDoubleSpinBox *>("lat0Box");
	m->lat1      = ui->findChild<QDoubleSpinBox *>("lat1Box");
	m->rmax      = ui->findChild<QDoubleSpinBox *>("rmaxBox");
	m->tubeR     = ui->findChild<QDoubleSpinBox *>("tubeBox");
	m->nlon      = ui->findChild<QSpinBox *>("lonBox");
	m->nring     = ui->findChild<QSpinBox *>("ringBox");
	m->skin      = ui->findChild<QComboBox *>("skinCombo");
	m->colorByB  = ui->findChild<QCheckBox *>("colorCheck");
	m->showPoles = ui->findChild<QCheckBox *>("polesCheck");
	m->trackButton  = ui->findChild<QPushButton *>("trackButton");
	m->trackPlotButton = ui->findChild<QPushButton *>("trackPlotButton");
	m->restoreLinesButton = ui->findChild<QPushButton *>("restoreLinesButton");
	m->saveMovieButton    = ui->findChild<QPushButton *>("saveMovieButton");
	m->status    = ui->findChild<QLabel *>("statusLabel");
	auto *compute   = ui->findChild<QPushButton *>("computeButton");
	auto *resetView = ui->findChild<QPushButton *>("resetViewButton");
	auto *host      = ui->findChild<QWidget *>("viewHost");
	if (!(m->date && m->lat0 && m->lat1 && m->rmax && m->tubeR && m->nlon && m->nring &&
	      m->skin && m->colorByB && m->showPoles && m->trackButton &&
	      m->trackPlotButton && m->restoreLinesButton && m->saveMovieButton && m->status &&
	      compute && resetView && host)) {
		QMessageBox::warning(parent, "Magnetic field lines",
			"magfield3d.ui does not carry this tool's controls (a renamed or removed widget).");
		delete ui; delete m; return nullptr;
	}
	// NO PARENT — a parented top-level is an OWNED window on Windows and could never go behind the
	// viewer. The Scene keeps the pointer and drops it when the dialog dies, and the dialog still
	// dies WITH the viewer through the destroyed() connection below.
	m->dialog = new QDialog(nullptr);
	m->dialog->setObjectName("magField3D");
	if (scene) scene->magFieldDlg = m->dialog;
	QObject::connect(m->dialog, &QObject::destroyed, m->dialog, [scene]() {
		if (scene && sceneAlive(scene)) scene->magFieldDlg = nullptr;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, m->dialog,
	                             [dlg = m->dialog]() { dlg->deleteLater(); });
	m->dialog->setProperty("magFieldState", QVariant::fromValue(static_cast<void *>(m)));
	m->baseTitle = "Earth magnetic field (IGRF) — 3-D";
	m->dialog->setWindowTitle(m->baseTitle);
	m->dialog->setWindowIcon(appIcon());
	m->dialog->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
	m->dialog->setWindowModality(Qt::NonModal);
	m->dialog->setModal(false);
	m->dialog->setAttribute(Qt::WA_DeleteOnClose);
	auto *root = new QVBoxLayout(m->dialog);
	root->setContentsMargins(0, 0, 0, 0);
	root->addWidget(ui);
	auto *layout = new QGridLayout(host);
	layout->setContentsMargins(0, 0, 0, 0);
	m->view = new MagFieldView(host);
	m->view->setObjectName("magFieldView");
	layout->addWidget(m->view, 0, 0);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->view->setRenderWindow(rw);
	m->renderer = vtkSmartPointer<vtkRenderer>::New();
	applyBackgroundPref(m->renderer);       // Preferences "Background color" — the ONE applier
	rw->AddRenderer(m->renderer);
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	m->view->interactor()->SetInteractorStyle(style);
	// THE WINDOW COMES UP FIRST, with a notice on it: the first trace in a session pays for
	// compiling the Julia tracer, and the texture may still be downloading. Same order as the fault
	// demo — canvas and controls painted, then the model arrives into a window that is already there.
	m->dialog->resize(1080, 760);
	m->dialog->show(); m->dialog->raise(); m->dialog->activateWindow();
	auto *notice = new QLabel("Tracing the IGRF field lines…\n\nThe first run in a session takes a "
	                          "few seconds:\nthe tracer is compiled on its way through Julia and the\n"
	                          "Earth image is fetched from the GMT data server.", host);
	notice->setObjectName("magFieldNotice");
	notice->setAlignment(Qt::AlignCenter);
	notice->setStyleSheet("#magFieldNotice { background-color: rgba(255,255,255,225); color: #202020;"
	                      " border: 1px solid #808080; border-radius: 8px; padding: 14px; }");
	layout->addWidget(notice, 0, 0, Qt::AlignCenter);
	qApp->processEvents();
	// The globe.
	auto sphere = magFieldSphere(360, 180);
	vtkNew<vtkPolyDataMapper> gmap;
	gmap->SetInputData(sphere);
	gmap->ScalarVisibilityOff();
	m->globe = vtkSmartPointer<vtkActor>::New();
	m->globe->SetMapper(gmap);
	m->globe->GetProperty()->SetAmbient(0.35);
	m->globe->GetProperty()->SetDiffuse(0.75);
	m->globe->GetProperty()->SetSpecular(0.05);
	QString error;
	m->texEarth = magFieldTexture(0, m->date->value(), error);
	if (m->texEarth) m->globe->SetTexture(m->texEarth);
	else if (!error.isEmpty() && m->status) m->status->setText(error);
	m->renderer->AddActor(m->globe);
	// The dip-pole markers: a ball standing on the skin, a stick out of it and a label. Built empty
	// here (magFieldPoles puts them where the date says) so nothing has to be created per Compute.
	const double poleRGB[2][3] = {{0.90, 0.16, 0.12}, {0.15, 0.35, 0.95}};   // N red, S blue
	for (int k = 0; k < 2; ++k) {
		vtkNew<vtkSphereSource> ball;
		ball->SetRadius(0.022);
		ball->SetThetaResolution(24);
		ball->SetPhiResolution(24);
		vtkNew<vtkPolyDataMapper> bmap;
		bmap->SetInputConnection(ball->GetOutputPort());
		m->poleMark[k] = vtkSmartPointer<vtkActor>::New();
		m->poleMark[k]->SetMapper(bmap);
		m->poleMark[k]->GetProperty()->SetColor(poleRGB[k][0], poleRGB[k][1], poleRGB[k][2]);
		m->poleMark[k]->GetProperty()->SetAmbient(0.45);
		m->renderer->AddActor(m->poleMark[k]);
		vtkNew<vtkLineSource> pin;
		pin->SetPoint1(0.0, 0.0, 0.0);
		pin->SetPoint2(0.0, 0.0, 0.0);
		vtkNew<vtkPolyDataMapper> pmap;
		pmap->SetInputConnection(pin->GetOutputPort());
		m->polePin[k] = vtkSmartPointer<vtkActor>::New();
		m->polePin[k]->SetMapper(pmap);
		m->polePin[k]->GetProperty()->SetColor(poleRGB[k][0], poleRGB[k][1], poleRGB[k][2]);
		m->polePin[k]->GetProperty()->SetLineWidth(2.0);
		m->polePin[k]->GetProperty()->SetLighting(false);
		m->renderer->AddActor(m->polePin[k]);
		m->poleLabel[k] = vtkSmartPointer<vtkBillboardTextActor3D>::New();
		m->poleLabel[k]->SetInput(k == 0 ? "N" : "S");
		m->poleLabel[k]->GetTextProperty()->SetFontSize(15);
		m->poleLabel[k]->GetTextProperty()->SetColor(poleRGB[k][0], poleRGB[k][1], poleRGB[k][2]);
		m->poleLabel[k]->GetTextProperty()->SetJustificationToCentered();
		m->poleLabel[k]->GetTextProperty()->SetBold(true);
		m->renderer->AddActor(m->poleLabel[k]);
		// The track's trail: empty until the animation runs, in the pole's own colour.
		m->trailPD[k] = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> lmap;
		lmap->SetInputData(m->trailPD[k]);
		lmap->ScalarVisibilityOff();
		m->trail[k] = vtkSmartPointer<vtkActor>::New();
		m->trail[k]->SetMapper(lmap);
		m->trail[k]->GetProperty()->SetColor(poleRGB[k][0], poleRGB[k][1], poleRGB[k][2]);
		m->trail[k]->GetProperty()->SetLineWidth(2.5);
		m->trail[k]->GetProperty()->SetLighting(false);
		m->trail[k]->SetVisibility(0);
		m->renderer->AddActor(m->trail[k]);
		// THE GEOGRAPHIC POLE, as a star: the whole point of the magnetic markers is how FAR they are
		// from the rotation axis, which needs the axis marked. A flat five-pointed star lying in the
		// tangent plane at (0,0,±1) — that plane is the xy plane at a pole, so no orientation matrix
		// is needed — a hair above the skin so the sphere does not z-fight it.
		// TRIANGULATED HERE, BY HAND: a five-pointed star is not convex, and the filters that could
		// cut it up want a closed CONTOUR (lines), not a polygon — fed a polygon they hand back an
		// empty mesh, which is a star nobody can see. A fan from the centre to each pair of rim
		// points is ten triangles and cannot fail.
		const double zTop = (k == 0) ? 1.004 : -1.004;
		vtkNew<vtkPoints> spts;
		vtkNew<vtkCellArray> stris;
		// Two thirds the size of a magnetic-pole marker: the ball's radius is 0.022, so the star's
		// points reach 2/3 of that and its waist sits at the usual 0.42 of its own reach.
		const double rOut = 0.022 * 2.0 / 3.0, rIn = rOut * 0.42;
		const vtkIdType cen = spts->InsertNextPoint(0.0, 0.0, zTop);
		vtkIdType rim[10];
		for (int i = 0; i < 10; ++i) {
			const double a = vtkMath::Pi() * 0.5 + double(i) * vtkMath::Pi() / 5.0;
			const double r = (i % 2 == 0) ? rOut : rIn;
			rim[i] = spts->InsertNextPoint(r * std::cos(a), r * std::sin(a), zTop);
		}
		for (int i = 0; i < 10; ++i) {
			const vtkIdType tri[3] = {cen, rim[i], rim[(i + 1) % 10]};
			stris->InsertNextCell(3, tri);
		}
		vtkNew<vtkPolyData> starPD;
		starPD->SetPoints(spts);
		starPD->SetPolys(stris);
		vtkNew<vtkPolyDataMapper> smap;
		smap->SetInputData(starPD);
		smap->ScalarVisibilityOff();
		m->geoStar[k] = vtkSmartPointer<vtkActor>::New();
		m->geoStar[k]->SetMapper(smap);
		m->geoStar[k]->GetProperty()->SetColor(1.0, 0.84, 0.05);
		m->geoStar[k]->GetProperty()->SetAmbient(1.0);        // lit or not, a symbol reads the same
		m->geoStar[k]->GetProperty()->SetDiffuse(0.0);
		m->geoStar[k]->GetProperty()->SetLighting(false);
		m->renderer->AddActor(m->geoStar[k]);
	}
	// The year over the sphere, IN the scene (so it is in every captured frame too): top centre of
	// the viewport, on its own white plate. Hidden until the track plays.
	m->yearTag = vtkSmartPointer<vtkTextActor>::New();
	m->yearTag->SetTextScaleModeToNone();
	m->yearTag->GetTextProperty()->SetFontSize(30);
	m->yearTag->GetTextProperty()->SetBold(true);
	m->yearTag->GetTextProperty()->SetColor(0.06, 0.06, 0.06);
	m->yearTag->GetTextProperty()->SetJustificationToCentered();
	m->yearTag->GetTextProperty()->SetVerticalJustificationToTop();
	m->yearTag->GetTextProperty()->SetBackgroundColor(1.0, 1.0, 1.0);
	m->yearTag->GetTextProperty()->SetBackgroundOpacity(0.78);
	m->yearTag->GetTextProperty()->SetFrame(true);
	m->yearTag->GetTextProperty()->SetFrameColor(0.35, 0.35, 0.35);
	m->yearTag->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
	m->yearTag->SetPosition(0.5, 0.965);
	m->yearTag->SetVisibility(0);
	m->renderer->AddActor2D(m->yearTag);
	// The hover tag for the trails — one label, whichever trail the cursor is on.
	m->trailTag = vtkSmartPointer<vtkBillboardTextActor3D>::New();
	m->trailTag->GetTextProperty()->SetFontSize(15);
	// LIGHT, with a dark drop shadow: the tag stands over the globe — ocean, land, night side — and
	// black on any of them is unreadable. White reads on all of them, and the shadow keeps it legible
	// over the ice caps too.
	m->trailTag->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
	m->trailTag->GetTextProperty()->SetShadow(true);
	m->trailTag->GetTextProperty()->SetShadowOffset(1, -1);
	m->trailTag->GetTextProperty()->SetBold(true);
	m->trailTag->GetTextProperty()->SetJustificationToCentered();
	m->trailTag->SetVisibility(0);
	m->renderer->AddActor(m->trailTag);
	// The field lines, as tubes.
	m->lut = vtkSmartPointer<vtkLookupTable>::New();
	m->lut->SetHueRange(0.62, 0.0);          // blue (weak) -> red (strong)
	m->lut->SetNumberOfTableValues(256);
	m->lut->Build();
	m->lines = vtkSmartPointer<vtkPolyData>::New();
	m->tubeFlt = vtkSmartPointer<vtkTubeFilter>::New();
	m->tubeFlt->SetInputData(m->lines);
	m->tubeFlt->SetNumberOfSides(8);
	m->tubeFlt->CappingOn();
	m->tubeFlt->SetRadius(m->tubeR->value());
	vtkNew<vtkPolyDataMapper> tmap;
	tmap->SetInputConnection(m->tubeFlt->GetOutputPort());
	m->tubes = vtkSmartPointer<vtkActor>::New();
	m->tubes->SetMapper(tmap);
	m->tubes->GetProperty()->SetAmbient(0.25);
	m->tubes->GetProperty()->SetDiffuse(0.8);
	m->renderer->AddActor(m->tubes);
	// GLView's middle button acts on a Scene's renderer, so give it one whose pick targets are this
	// view's own two actors — the same arrangement the fault demo makes for its blocks.
	m->viewScene = new Scene();
	m->viewScene->ren    = m->renderer;
	m->viewScene->widget = m->view;
	m->viewScene->ve     = 1.0;
	m->viewScene->pickTargets = {m->globe.Get(), m->tubes.Get()};
	m->view->s = m->viewScene;
	m->view->onHover = [m](double x, double y) { ++m->hoverCalls; magFieldHoverPoles(m, x, y); };
	QObject::connect(m->dialog, &QObject::destroyed, [m]() { delete m->viewScene; delete m; });
	if (!magFieldTrace(m, error)) m->status->setText(error);
	if (!magFieldPoles(m, error)) m->status->setText(error);
	magFieldEnsureSkin(m);
	magFieldApplyLooks(m);
	delete notice;
	magFieldResetView(m);
	// IN FRONT AGAIN, at the end. The window was raised before the first trace, but the seconds of
	// GMT work that follow run the viewer's own event loop, and the viewer takes the front back — so
	// the tool would surface behind the iGMT window the very first time it is opened.
	m->dialog->raise();
	m->dialog->activateWindow();
	// ONLY THE ACTION BUTTON COMPUTES. The spin boxes never trigger a trace on their own — that is
	// the standing rule for every module dialog in this project (an edit box is an edit box).
	QObject::connect(compute, &QPushButton::clicked, m->dialog, [m]() {
		magFieldTrackRestore(m);                 // a trace is about the date box, not about 1943
		QString err;
		m->tubeFlt->SetRadius(m->tubeR->value());
		if (!magFieldTrace(m, err)) m->status->setText(err);
		// The poles and the IGRF skin ARE the date, so Compute re-derives both: a new date with the
		// old poles still on the globe would be the tool contradicting its own date box.
		if (!magFieldPoles(m, err)) m->status->setText(err);
		magFieldEnsureSkin(m);
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	QObject::connect(resetView, &QPushButton::clicked, m->dialog, [m]() { magFieldResetView(m); });
	// THE POLE TRACK. One year per tick, from 1900 to this year; the track itself is fetched the first
	// time the button is pressed (seconds of GMT work, said so on the status line) and kept, so a
	// second run starts instantly.
	m->anim = new QTimer(m->dialog);
	m->anim->setInterval(90);
	QObject::connect(m->anim, &QTimer::timeout, m->dialog, [m]() {
		if (m->animRow >= int(m->track.size())) { magFieldTrackStop(m); return; }
		magFieldTrackFrame(m, m->animRow++);
	});
	QObject::connect(m->trackButton, &QPushButton::clicked, m->dialog, [m]() {
		if (m->animOn) { magFieldTrackStop(m); return; }
		if (m->track.empty()) {
			m->status->setText(QString("Computing where the poles stood, 1900-%1…")
			                   .arg(magFieldTrackEndYear(), 0, 'f', 0));
			m->trackButton->setEnabled(false);
			m->trackButton->setText("Computing…");
			m->trackButton->repaint();
			m->status->repaint();
			QString err;
			// The wait has a window of its own (magFieldFetchTrack): what is being computed, a bar,
			// and the year it is on.
			const bool ok = magFieldFetchTrack(m, err);
			m->trackButton->setEnabled(true);
			m->trackButton->setText("Animate 1900 → now");
			// ...and the tool comes back to the front: the seconds of Julia work behind this button
			// are exactly when the viewer window climbs over it.
			m->dialog->raise();
			m->dialog->activateWindow();
			if (!ok) { m->status->setText(err); return; }
			m->status->setText(QString("Pole track: %1 years.").arg(m->track.size()));
		}
		m->animOn  = true;
		m->restorePending = false;
		m->animRow = 0;
		m->trackButton->setText("Stop");
		if (!m->poleFramed) { magFieldFaceVisiblePole(m); m->poleFramed = true; }
		magFieldApplyLooks(m);
		m->anim->start();
	});
	QObject::connect(m->trackPlotButton, &QPushButton::clicked, m->dialog, [m]() { magFieldTrackPlot(m); });
	QObject::connect(m->restoreLinesButton, &QPushButton::clicked, m->dialog, [m]() { magFieldTrackRestore(m); });
	// THE DATE BOX MEANS SOMETHING ELSE WHILE THE TRACK IS SHOWING. With the field lines away — after
	// an animation — the window is a picture of where the poles have been, so changing the date walks
	// the markers to THAT year instead of bringing the lines back (that is the Restore button's job,
	// and only its job). Outside that state the date box still computes nothing on its own, which is
	// the standing rule for every module dialog here.
	QObject::connect(m->date, QOverload<double>::of(&QDoubleSpinBox::valueChanged), m->dialog,
	                 [m](double v) {
		if (m->animOn || !m->restorePending) return;
		QString err;
		if (!magFieldPoles(m, err)) { m->status->setText(err); return; }
		const QString year = QString::number(v, 'f', (v == std::floor(v)) ? 0 : 2);
		if (m->yearTag) { m->yearTag->SetInput(year.toUtf8().constData()); m->yearTag->SetVisibility(1); }
		m->dialog->setWindowTitle(m->baseTitle + " — " + year);
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	QObject::connect(m->saveMovieButton, &QPushButton::clicked, m->dialog, [m]() {
		if (m->animOn) magFieldTrackStop(m);          // one player at a time
		if (m->track.empty()) {
			QString err;
			m->trackButton->setEnabled(false);
			const bool ok = magFieldFetchTrack(m, err);
			m->trackButton->setEnabled(true);
			if (!ok) { m->status->setText(err); return; }
		}
		magFieldSaveMovie(m);
	});
	// The two LOOK controls are not computations: they repaint what is already traced, so they act
	// at once (a check box that needed a Compute click would be lying about what it does).
	QObject::connect(m->colorByB, &QCheckBox::toggled, m->dialog, [m](bool) {
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	QObject::connect(m->showPoles, &QCheckBox::toggled, m->dialog, [m](bool) {
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	// The skin is a look too — but the IGRF one may have to be BUILT the first time it is asked for,
	// which is a magref grid, not a repaint. Hence the ensure step before the repaint.
	QObject::connect(m->skin, QOverload<int>::of(&QComboBox::currentIndexChanged), m->dialog, [m](int) {
		magFieldEnsureSkin(m);
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	return m->dialog;
}
