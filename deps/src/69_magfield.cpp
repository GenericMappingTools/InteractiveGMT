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
using JuliaMagTexFn   = int (*)(unsigned char *, int, int *, char *, int);
static JuliaMagLinesFn g_juliaMagLines = nullptr;
static JuliaMagTexFn   g_juliaMagTex   = nullptr;

// The most field lines one view will draw. The dialog's own spin boxes cannot ask for more
// (longitudes x rings x 2 hemispheres), and Julia refuses anything above it rather than overrun
// the counts array this side allocates.
static const int kMagMaxLines = 4096;

// Same one addition GLView cannot know about as in the fault demo: this dialog is full of spin
// boxes that keep the keyboard, so entering the view takes the focus back — without it the view
// keys go to a spin box instead of the camera.
class MagFieldView : public GLView {
public:
	MagFieldView(QWidget *parent = nullptr) : GLView() { setParent(parent); }
protected:
	void enterEvent(QEnterEvent *e) override {
		setFocus(Qt::MouseFocusReason);
		GLView::enterEvent(e);
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
	// The Earth picture is KEPT here, not read back off the actor: the "Earth image" box takes it off
	// the globe, and an actor with no texture has none to put back.
	vtkSmartPointer<vtkTexture>  tex;
	// GLView is written against a Scene (it reads the renderer off one for the middle button), so
	// this dialog owns one of its own — the same thing the fault demo does, and for the same reason.
	Scene *viewScene = nullptr;
	QDoubleSpinBox *date = nullptr, *lat0 = nullptr, *lat1 = nullptr, *rmax = nullptr, *tubeR = nullptr;
	QSpinBox       *nlon = nullptr, *nring = nullptr;
	QCheckBox      *showEarth = nullptr, *colorByB = nullptr;
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

// Fetch the Earth picture from Julia (two-phase: dims, then bytes) and wrap it in a texture.
static vtkSmartPointer<vtkTexture> magFieldTexture(QString &error) {
	if (!g_juliaMagTex) { error = "The Earth-texture callback is not registered."; return nullptr; }
	int dims[3] = {};
	char message[1024] = {};
	if (!g_juliaMagTex(nullptr, 0, dims, message, sizeof(message)) || dims[0] <= 0 || dims[1] <= 0) {
		error = message[0] ? QString::fromUtf8(message) : QString("The Earth texture could not be read.");
		return nullptr;
	}
	const int nx = dims[0], ny = dims[1], nc = dims[2] >= 4 ? 4 : 3;
	vtkNew<vtkImageData> img;
	img->SetDimensions(nx, ny, 1);
	img->AllocateScalars(VTK_UNSIGNED_CHAR, nc);
	auto *dst = static_cast<unsigned char *>(img->GetScalarPointer());
	const int nbytes = nx * ny * nc;
	if (!g_juliaMagTex(dst, nbytes, dims, message, sizeof(message))) {
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

// Apply the two looks the check boxes pick: tubes coloured by |B| or plain, and the Earth picture
// on or off (off leaves the sphere as a plain body, so the lines are still read against a globe).
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
	if (m->globe && m->showEarth) {
		const bool on = m->showEarth->isChecked() && m->tex;
		m->globe->SetTexture(on ? m->tex : nullptr);
		if (on) m->globe->GetProperty()->SetColor(1.0, 1.0, 1.0);
		else    m->globe->GetProperty()->SetColor(0.45, 0.55, 0.70);   // a plain body to read the lines against
	}
}

static void magFieldResetView(MagField *m) {
	if (!m->renderer) return;
	// An OBLIQUE opening view, set BEFORE ResetCamera (which only moves the camera along the
	// direction it already has): the rotation axis stands up the screen and the lines arch across
	// it, the way the reference figure reads. Setting the view-up afterwards, with the default
	// camera still looking down +z, asks for an up-vector parallel to the view normal — VTK then
	// resets it itself and warns.
	auto *cam = m->renderer->GetActiveCamera();
	cam->SetFocalPoint(0.0, 0.0, 0.0);
	cam->SetPosition(1.0, -2.4, 0.9);
	cam->SetViewUp(0.0, 0.0, 1.0);
	m->renderer->ResetCamera();
	m->renderer->ResetCameraClippingRange();
	if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
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
	m->showEarth = ui->findChild<QCheckBox *>("earthCheck");
	m->colorByB  = ui->findChild<QCheckBox *>("colorCheck");
	m->status    = ui->findChild<QLabel *>("statusLabel");
	auto *compute   = ui->findChild<QPushButton *>("computeButton");
	auto *resetView = ui->findChild<QPushButton *>("resetViewButton");
	auto *host      = ui->findChild<QWidget *>("viewHost");
	if (!(m->date && m->lat0 && m->lat1 && m->rmax && m->tubeR && m->nlon && m->nring &&
	      m->showEarth && m->colorByB && m->status && compute && resetView && host)) {
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
	m->dialog->setWindowTitle("Earth magnetic field (IGRF) — 3-D");
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
	m->tex = magFieldTexture(error);
	if (m->tex) m->globe->SetTexture(m->tex);
	else if (!error.isEmpty() && m->status) m->status->setText(error);
	m->renderer->AddActor(m->globe);
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
	QObject::connect(m->dialog, &QObject::destroyed, [m]() { delete m->viewScene; delete m; });
	if (!magFieldTrace(m, error)) m->status->setText(error);
	magFieldApplyLooks(m);
	delete notice;
	magFieldResetView(m);
	// ONLY THE ACTION BUTTON COMPUTES. The spin boxes never trigger a trace on their own — that is
	// the standing rule for every module dialog in this project (an edit box is an edit box).
	QObject::connect(compute, &QPushButton::clicked, m->dialog, [m]() {
		QString err;
		m->tubeFlt->SetRadius(m->tubeR->value());
		if (!magFieldTrace(m, err)) m->status->setText(err);
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	QObject::connect(resetView, &QPushButton::clicked, m->dialog, [m]() { magFieldResetView(m); });
	// The two LOOK controls are not computations: they repaint what is already traced, so they act
	// at once (a check box that needed a Compute click would be lying about what it does).
	QObject::connect(m->colorByB, &QCheckBox::toggled, m->dialog, [m](bool) {
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	QObject::connect(m->showEarth, &QCheckBox::toggled, m->dialog, [m](bool) {
		magFieldApplyLooks(m);
		if (m->view && m->view->renderWindow()) m->view->renderWindow()->Render();
	});
	return m->dialog;
}
