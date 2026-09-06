// ============================================================================================
// Aquamoto viewer — port of Mirone's aquamoto.m NETCDF TAB ONLY (the first of its three tabs).
// Visualizes NSWING/tsunami netCDF output where dry land and ocean wave height must be coloured
// SEPARATELY (very different scales) and blended only at render time — see src/aquamoto.jl for
// the actual compositing math (indLand mask / clamp / colourize / blend / hard land overwrite,
// a direct port of aqua_suppfuns.m's coards_sliceShow + aquamoto.m's do_imgWater/do_imgBat/
// mixe_images, IamTSU branch).
//
// Loaded from aquamoto.ui via QUiLoader and used DIRECTLY as the returned QMainWindow (the .ui's
// own root class — unlike every other tool dialog in this file, which loads a QDialog-rooted .ui
// and wraps it). A plain non-QObject wrapper (`win` member holds the real widget), same
// self-deleting idiom as IgrfDialog/MagBarcodeDialog: `win`'s WA_DeleteOnClose frees the Qt widget
// on close, and a `destroyed -> delete this` connection frees the wrapper alongside it. Non-modal,
// stays open across any number of slice/run-in actions.
//
// Every Julia round-trip goes through the generic console-eval bridge (g_juliaEval) — the SAME
// synchronous mechanism NswingDialog already uses for its own small queries — rather than a
// dedicated typed callback: no new @cfunction/registration needed, only the composited-texture
// push (gmtvtk_show_layer_rgba_h, 90_c_api.cpp) is a new C export.
//
// Show mesh (ANUGA triangulated-mesh display) and Derived var stay disabled/unwired -- the .ui
// itself ships Show-mesh disabled, and there is no VTK/GMT triangulated-mesh equivalent on hand
// for the derived-var formulas. The Primary-quantities picker (Stage/Xmoment/Ymoment/Or…) IS wired:
// every time-varying variable a netCDF file actually has must be loadable and switchable, not just
// the first match discarding the rest (see aquamoto.jl's _aqua_find_all_varnames/_aquamoto_set_var).
// ============================================================================================

// Run a Julia expression synchronously via the console-eval bridge, with `scene` as the acting
// window. Fills `out` with printed stdout and returns true on success; on failure (an exception in
// the evaluated code, or the bridge not registered yet) fills `out` with the error text and
// returns false. Mirrors NswingDialog::juliaEvalCall (70_window.cpp) — one eval helper per caller
// convention already established in this file, not reinvented here.
static bool aquaEval(Scene *scene, const QString &call, QString &out) {
	if (!g_juliaEval) { out = "Julia eval bridge not registered"; return false; }
	std::vector<char> buf(1 << 16);
	int n = g_juliaEval(scene, call.toUtf8().constData(), buf.data(), (int)buf.size());
	out = QString::fromUtf8(buf.data(), n < 0 ? -n : n);
	return n >= 0;
}

// The acting window's Scene *as a Julia pointer literal, the same "Ptr{Cvoid}(UInt(...))" spelling
// NswingDialog's own calls use.
static QString aquaScenePtr(Scene *scene) {
	return QString("Ptr{Cvoid}(UInt(%1))").arg((quintptr)scene);
}

// The Aquamoto window is never DESTROYED by its X -- clicking the title-bar X HIDES it (state kept)
// and PARKS it, exactly like every other tool window: the X,Y plot, Contours and Illumination all go
// through parkTool into the Scene Objects bottom strip, and so does this one. It used to hide itself
// and rely on a hand-built "Aquamoto viewer" row planted at the TOP of the tree instead — one
// operation (a tool window hidden by its X, kept as a handle that brings it back) with two
// implementations, and the bespoke one put a TOOL among the DATA elements (SACRED_LAW.md).
static void aquamotoUnpark(Scene *scene);                       // defined with the other hooks below
static std::function<void(const QPoint &)> aquamotoParkedMenu(Scene *scene);

// "Shaded image (2-D)" — the ONE geometry switch (70_window.cpp, included after this fragment).
// The Cinema tab's "3-D surface" box throws exactly the switch the Shading dock's checkbox throws;
// it never rebuilds a surface of its own (SACRED_LAW.md).
static void sceneSetShadedImage2D(Scene *s, bool on);

// ============================================================================================
//  The little η(x) figure that floats over the 3-D view (Cinema tab, "Show η(x) profile").
//
//  It is a ProfilePanel (60_profile.cpp) — the SAME 2-D plotter the Ctrl-drag elevation profile
//  paints into, with the same axes, the same right-click "Open in X,Y plot tool" and, when the
//  analytic solution is added, its second curve. Nothing here draws a curve itself.
//
//  The frame around the panel is what makes it a FIGURE rather than a docked panel: a title strip
//  to drag it by and a corner to resize it with, so it can be parked anywhere over the render area
//  and sized to taste. It is a plain child widget of the render widget, so it floats above the 3-D
//  view without a window of its own and the panel keeps its own mouse handling.
// ============================================================================================
class EtaFigure : public QFrame {
public:
	ProfilePanel *panel = nullptr;

	EtaFigure(QWidget *parent, const QString &title) : QFrame(parent), title_(title) {
		setFrameShape(QFrame::StyledPanel);
		setFrameShadow(QFrame::Raised);
		setAutoFillBackground(true);
		auto *lay = new QVBoxLayout(this);
		lay->setContentsMargins(kMargin, kTitleH, kMargin, kMargin);
		panel = new ProfilePanel(this);
		panel->setMinimumHeight(60);                 // the figure is small: override the panel's own 170
		lay->addWidget(panel);
		resize(380, 200);
	}

	// What the MINIMISE button runs. Set by the owner to the same parkTool call every other tool
	// window's X and minimise go through, so the figure parks in Scene Objects like the rest.
	std::function<void()> onMinimize;

	void setCurve(const std::vector<double> &x, const std::vector<double> &y,
	              const QString &title, const QString &xlabel, const QString &ylabel) {
		panel->setSeries(x, y, title, xlabel, ylabel, /*isDate=*/false);
	}

	// The strip text. The slice's MODEL TIME goes here, from the one string the data already
	// publishes for the viewer's own titlebar (Scene::titleExtra) — never a second time format.
	void setTitle(const QString &t) {
		if (t == title_) return;
		title_ = t;
		update();
	}

protected:
	static const int kMargin = 5, kTitleH = 19, kGrip = 16, kBtn = 15;

	// The minimise button, top-RIGHT of the title strip.
	QRect minRect() const { return QRect(width() - kBtn - 3, 2, kBtn, kTitleH - 4); }
	QString title_;
	QPoint  press_;                 // cursor position when a drag started (global)
	QRect   startGeom_;             // this figure's geometry when the drag started
	bool    moving_ = false, resizing_ = false;

	QRect gripRect() const { return QRect(width() - kGrip, height() - kGrip, kGrip, kGrip); }

	void paintEvent(QPaintEvent *e) override {
		QFrame::paintEvent(e);
		QPainter p(this);
		p.fillRect(QRect(1, 1, width() - 2, kTitleH - 2), QColor(232, 232, 238));
		p.setPen(QColor(70, 70, 70));
		p.drawText(QRect(7, 1, width() - 14 - kBtn, kTitleH - 2),
		           Qt::AlignVCenter | Qt::AlignLeft, title_);
		{	/* Minimise button, top-right: parks the figure in Scene Objects. */
			const QRect mr = minRect();
			p.setPen(QColor(160, 160, 168));
			p.drawRect(mr.adjusted(0, 0, -1, -1));
			p.setPen(QColor(70, 70, 70));
			p.drawLine(mr.left() + 3, mr.bottom() - 3, mr.right() - 3, mr.bottom() - 3);
		}
		p.setPen(QColor(150, 150, 150));                       // the resize corner's three ticks
		for (int k = 0; k < 3; ++k) {
			const int o = 4 + 4 * k;
			p.drawLine(width() - o, height() - 3, width() - 3, height() - o);
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) { QFrame::mousePressEvent(e); return; }
		if (minRect().contains(e->pos())) {          // minimise: park, never start a title-strip drag
			if (onMinimize) onMinimize();
			return;
		}
		press_ = e->globalPosition().toPoint();
		startGeom_ = geometry();
		resizing_ = gripRect().contains(e->pos());
		moving_   = !resizing_ && e->pos().y() < kTitleH;
		// Every DRAG in this program uses the move cursor; the resize corner keeps the diagonal one.
		if (moving_)   setCursor(Qt::SizeAllCursor);
		if (resizing_) setCursor(Qt::SizeFDiagCursor);
		if (!moving_ && !resizing_) QFrame::mousePressEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!moving_ && !resizing_) {
			setCursor(minRect().contains(e->pos())  ? Qt::ArrowCursor
			        : gripRect().contains(e->pos()) ? Qt::SizeFDiagCursor
			                                        : (e->pos().y() < kTitleH ? Qt::SizeAllCursor : Qt::ArrowCursor));
			QFrame::mouseMoveEvent(e);
			return;
		}
		const QPoint d = e->globalPosition().toPoint() - press_;
		if (moving_) {
			QRect g = startGeom_.translated(d);
			if (parentWidget()) {                              // keep it inside the render area
				const QRect pr = parentWidget()->rect();
				g.moveLeft(std::max(0, std::min(g.left(), pr.width()  - g.width())));
				g.moveTop (std::max(0, std::min(g.top(),  pr.height() - g.height())));
			}
			setGeometry(g);
		} else {
			resize(std::max(180, startGeom_.width()  + d.x()),
			       std::max(110, startGeom_.height() + d.y()));
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		moving_ = resizing_ = false;
		setCursor(Qt::ArrowCursor);
		QFrame::mouseReleaseEvent(e);
	}
};

class AquamotoHideOnClose : public QObject {
public:
	Scene *scene_;
	AquamotoHideOnClose(QObject *parent, Scene *scene) : QObject(parent), scene_(scene) {}
	// PARKING IS ONE OPERATION, so the X and the minimise button share it verbatim -- the window is
	// hidden (never destroyed) and a handle appears in Scene Objects that brings it back.
	void park(QWidget *w) {
		if (!w) return;
		w->hide();                                              // hidden, NOT destroyed
		if (!scene_) return;
		Scene *sc = scene_;
		parkTool(sc, w, "Aquamoto viewer", IC_Image,
		         "Parked Aquamoto viewer — double-click to bring it back, click for its menu",
		         [sc]() { aquamotoUnpark(sc); }, aquamotoParkedMenu(sc));
		// A handle the user cannot see is the same as no handle at all — same reveal the
		// X,Y plot does when it parks.
		unfoldSceneObjects(sc);
	}
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (ev->type() == QEvent::Close) {
			ev->ignore();
			park(qobject_cast<QWidget *>(obj));
			return true;
		}
		// MINIMISE PARKS IT, exactly as the X does. Qt has already applied the minimised state by the
		// time this event arrives, so the bit is cleared first -- otherwise the window would come back
		// out of Scene Objects still minimised, i.e. not come back at all.
		if (ev->type() == QEvent::WindowStateChange) {
			QWidget *w = qobject_cast<QWidget *>(obj);
			if (w && w->isMinimized()) {
				w->setWindowState(w->windowState() & ~Qt::WindowMinimized);
				park(w);
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

class AquamotoWindow {
public:
	QMainWindow *win = nullptr;           // the loaded aquamoto.ui window itself (NOT wrapped/copied)
	Scene *scene_ = nullptr;              // the viewer window this Aquamoto session renders into
	bool busy_ = false;                   // a blocking aquaEval() call is currently in flight (reentrancy guard)
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once `this` is destroyed
	~AquamotoWindow() { if (alive_) *alive_ = false; }

	// One live (possibly hidden) Aquamoto window per viewer scene. Closing hides the window and keeps
	// its entry here; the scene's grid handle re-shows it via openFor(). Keyed by Scene*.
	static QHash<Scene *, AquamotoWindow *> &registry() {
		static QHash<Scene *, AquamotoWindow *> m;
		return m;
	}
	// Open (or re-show) the Aquamoto window for `scene`: reuse the existing one -- with its file, slice
	// and every other bit of state intact -- if there is one, else build a fresh window. Used by both
	// the Geophysics menu and the grid handle's "Aquamoto viewer…" entry.
	static void openFor(QWidget *parent, Scene *scene) {
		AquamotoWindow *w = registry().value(scene, nullptr);
		if (!w || !w->win) {
			w = new AquamotoWindow(parent, scene);
			if (!w->win) { delete w; return; }
			registry().insert(scene, w);
		}
		w->win->show();
		w->win->raise();
		w->win->activateWindow();
		// Compile the slice path while the user is still choosing a file / reaching for the slider.
		// Without it the FIRST slider move pays several seconds of Julia JIT (read + composite + RGBA
		// pack + relight) and the slider looks dead — the same dead time every other tool spends this
		// way (warmup.jl, _aqua_warm).
		warmupTool("aquamoto");
	}
	QLineEdit *pathEdit = nullptr;
	QLabel *timeStepsLabel = nullptr, *waterTransparencyLabel = nullptr;
	QScrollBar *sliceSlider = nullptr;    // QScrollBar, NOT QSlider -- arrow buttons at each tip (Mirone-style)
	QSlider *waterTransparencySlider = nullptr;
	QLineEdit *sliceSpin = nullptr;       // a PLAIN edit box (replaces the .ui's QSpinBox at runtime --
	                                      // see the constructor), not a spinner: user wants a simple box
	QCheckBox *splitDryWetCheck = nullptr, *scaleGlobalCheck = nullptr;
	QPushButton *loadRamBtn = nullptr, *runInBtn = nullptr;
	// Benchs tab — "10% slope beach, Benchmark 1": run it, load a previous run, say where it is saved,
	// and watch it advance.
	QPushButton *benchRunBtn = nullptr, *benchLoadBtn = nullptr, *benchBrowseBtn = nullptr;
	QLineEdit *benchSaveEdit = nullptr;
	QLabel *benchExistsLabel = nullptr;   // "that file is already there, and it can be loaded" (see benchAnnounceExisting)
	QProgressBar *benchProgress = nullptr;
	QCheckBox *benchKeepRamCheck = nullptr;
	bool benchGotoNetcdf_ = false;        // a Benchs run/load is in flight: show netCDF when it lands

	// Bring one of the dialog's tabs to the front, by its .ui object name.
	void selectTab(const char *objName) {
		if (!win) return;
		if (auto *tabs = win->findChild<QTabWidget *>("mainTabWidget"))
			if (QWidget *page = win->findChild<QWidget *>(objName))
				tabs->setCurrentWidget(page);
	}
	QRadioButton *stageRadioButton = nullptr, *xmomentRadioButton = nullptr, *ymomentRadioButton = nullptr;
	QComboBox *orComboBox = nullptr;
	QString activeVar_;                   // the varname currently selected in the quantity picker
	bool settingVar_ = false;              // guard: suppress fireSlice while WE are (un)checking radios
	QRadioButton *shadeWaterBtn = nullptr, *shadeLandBtn = nullptr;   // split which side's colour scale is shown
	bool opened_ = false;                 // a file has been successfully opened this session

	// ---- Cinema tab: playback, the view boxes and the floating η(x) figure -------------------
	// Playback drives the SLICE SLIDER, never _aquamoto_slice directly: the slider is the one place
	// that says which slice is showing, and everything (the box, the Julia call, the figure) already
	// hangs off it. A timer that called the Julia side itself would be a second player.
	QTimer *cineTimer = nullptr;
	QPushButton *cinePlayBtn = nullptr, *cineFirstBtn = nullptr, *cinePrevBtn = nullptr,
	            *cineNextBtn = nullptr, *cineLastBtn = nullptr, *cineResetViewBtn = nullptr;
	QLineEdit *cineRateEdit = nullptr, *cineFromEdit = nullptr, *cineToEdit = nullptr,
	          *cineAzEdit = nullptr, *cineElEdit = nullptr, *cineZoomEdit = nullptr, *cineVeEdit = nullptr,
	          *cineSpinEdit = nullptr, *cineProfX0Edit = nullptr, *cineProfLenEdit = nullptr;
	QCheckBox *cineLoopCheck = nullptr, *cine3DCheck = nullptr, *cineSpinCheck = nullptr,
	          *cineProfCheck = nullptr;
	// WHAT THE SLICE MUST STAND ON, read off the SCENE before each push overwrites it (fireSlice) and
	// put back after it (afterSliceShown). Not the Cinema checkbox: that box is only ONE of the
	// switches that can put this window on the 3-D surface (the Shading dock's "Shaded image (2-D)"
	// and the tank's own 3-D birth are the others), so gating on it dropped every window back to a
	// flat quad on the next slice — and a flat quad has no relief for the vertical exaggeration to
	// act on, which is the water surface staying flat at every VE.
	bool wants3D_ = false;
	bool sliceDirty_ = false;             // a slice was asked for while one was in flight (see fireSlice)
	QPointer<EtaFigure> etaFig;           // lives in the RENDER widget, which can outlive/predecease us
	QTimer *viewSyncTimer = nullptr;      // the boxes FOLLOW the mouse (see syncViewBoxes)

	explicit AquamotoWindow(QWidget *parent, Scene *scene) : scene_(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/aquamoto.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("AquamotoWindow: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		const QByteArray uiBytes = f.readAll();          // keep the raw text -- QUiLoader consumes the
		QBuffer buf(const_cast<QByteArray *>(&uiBytes));  // device, but a QMainWindow-rooted .ui's own
		buf.open(QIODevice::ReadOnly);                    // declared size is NOT reproduced by load()
		// No `parent` on purpose: a QMainWindow-rooted .ui loaded WITH a parent widget embeds as a
		// plain CHILD widget inside it (invisible, since the caller never adds it to a layout) --
		// only a parentless top-level QMainWindow shows up as its own real window.
		win = qobject_cast<QMainWindow *>(loader.load(&buf));
		f.close();
		if (!win) { qWarning("AquamotoWindow: QUiLoader failed to load the .ui"); return; }

		// NO MAXIMIZE BUTTON. Nobody asked for one -- it comes free with Qt's default top-level frame
		// (see the note further down), and a window sized to its layout's true minimum has nothing to
		// gain from being blown up to the screen. Set HERE, straight after load(): the native window
		// does not exist yet (winId() below is what creates it), so this cannot trigger the recreate
		// the warning further down is about.
		win->setWindowFlags(win->windowFlags() & ~Qt::WindowMaximizeButtonHint);

		// Deliberately do NOT apply the .ui's own declared <property name="geometry"> size here (that
		// number just drifts every time the window gets resized in Designer, see git history of this
		// file). The window is instead sized to its layout's true minimum at the very end of this
		// constructor (win->adjustSize(), after every widget including the runtime-added slider arrow
		// buttons exists) -- it always opens at the smallest size that fits everything, never bigger.

		// Pin Aquamoto's OS z-order above the main iGMT window (never hidden behind it), WITHOUT
		// reparenting into the widget tree (that would embed it as a child, see the loader comment
		// above) and WITHOUT setWindowFlags (recreates the native window, see the comment below).
		// winId() forces each native handle to exist, then setTransientParent() makes `win` an
		// OWNED window of the main window -- Windows keeps an owned window above its owner in
		// z-order, but (unlike Qt::WindowStaysOnTopHint) only relative to that one window, and it
		// stays a fully independent top-level (still get its own taskbar/alt-tab entry etc.).
		// Use scene_->win, not the `parent` argument -- callers pass nullptr from some call sites.
		win->winId();
		if (scene_ && scene_->win) {
			scene_->win->winId();
			if (QWindow *wh = win->windowHandle()) wh->setTransientParent(scene_->win->windowHandle());
		}

		// NO WA_DeleteOnClose: the window must survive a close (see AquamotoHideOnClose) -- closing
		// only HIDES it, so reopening shows the same window with its file/slice/state intact. The
		// wrapper therefore lives for the whole session (kept in `registry()`, keyed by scene); it is
		// deleted only when the window is genuinely destroyed (app teardown), which is what the
		// destroyed handler below is for.
		// NO setWindowFlags() here: it recreates the native window and drops the "explicit size was
		// set" state, undoing the resize() just above. A parentless top-level QMainWindow already
		// gets a normal frame (close/min/max) by default; there is nothing to add here.
		win->setWindowModality(Qt::NonModal);
		win->installEventFilter(new AquamotoHideOnClose(win, scene_));
		QObject::connect(win, &QObject::destroyed, win, [this]() { registry().remove(scene_); delete this; });

		QMainWindow *w = win;   // local copy for lambda capture (member `win` still usable directly)

		pathEdit                = w->findChild<QLineEdit *>("filePathLineEdit");
		timeStepsLabel          = w->findChild<QLabel *>("timeStepsLabel");
		waterTransparencyLabel  = w->findChild<QLabel *>("waterTransparencyLabel");
		sliceSlider             = w->findChild<QScrollBar *>("sliceSlider");
		sliceSpin               = w->findChild<QLineEdit *>("sliceNSpinBox");   // aquamoto.ui: a plain edit box, not a QSpinBox
		waterTransparencySlider = w->findChild<QSlider *>("waterTransparencySlider");
		splitDryWetCheck        = w->findChild<QCheckBox *>("splitDryWetCheckBox");
		scaleGlobalCheck        = w->findChild<QCheckBox *>("scaleColorGlobalCheckBox");
		loadRamBtn            = w->findChild<QPushButton *>("loadRamButton");
		runInBtn                = w->findChild<QPushButton *>("plotRunInButton");
		benchRunBtn             = w->findChild<QPushButton *>("benchRunButton");
		benchLoadBtn            = w->findChild<QPushButton *>("benchLoadButton");
		benchBrowseBtn          = w->findChild<QPushButton *>("benchSaveBrowseButton");
		benchSaveEdit           = w->findChild<QLineEdit *>("benchSaveEdit");
		benchProgress           = w->findChild<QProgressBar *>("benchProgressBar");
		benchKeepRamCheck       = w->findChild<QCheckBox *>("benchKeepRamCheck");
		shadeWaterBtn           = w->findChild<QRadioButton *>("shadeWaterButton");
		shadeLandBtn            = w->findChild<QRadioButton *>("shadeLandButton");
		stageRadioButton        = w->findChild<QRadioButton *>("stageRadioButton");
		xmomentRadioButton      = w->findChild<QRadioButton *>("xmomentRadioButton");
		ymomentRadioButton      = w->findChild<QRadioButton *>("ymomentRadioButton");
		orComboBox              = w->findChild<QComboBox *>("orComboBox");
		auto *browseBtn         = w->findChild<QToolButton *>("browseFileButton");

		// Force Water/Land mutually exclusive with an explicit group -- do NOT rely on QRadioButton's
		// implicit parent-based auto-exclusive grouping here: the two buttons sit inside a nested
		// QHBoxLayout loaded via QUiLoader, and whether they end up with the exact same QObject
		// parent (required for the automatic grouping to kick in) is not guaranteed. A QButtonGroup
		// is unambiguous regardless of the widget tree shape.
		if (shadeWaterBtn && shadeLandBtn) {
			auto *shadeGroup = new QButtonGroup(w);
			shadeGroup->setExclusive(true);
			shadeGroup->addButton(shadeWaterBtn);
			shadeGroup->addButton(shadeLandBtn);
		}

		// Show mesh (ANUGA-only) never gets wired -- ships disabled in the .ui and stays that way, as
		// does Derived var (still out of scope: no VTK/GMT triangulated-mesh equivalent on hand).
		// Primary quantities (Stage/Xmoment/Ymoment/Or…) DO get wired now -- every time-varying
		// variable the nc file actually has must be loadable, not just the first match (see
		// aquamoto.jl's _aqua_find_all_varnames/_aquamoto_set_var). Radios/combo start disabled here;
		// openPath() enables exactly the ones the just-opened file's varnames cover.
		if (stageRadioButton) stageRadioButton->setEnabled(false);
		if (xmomentRadioButton) xmomentRadioButton->setEnabled(false);
		if (ymomentRadioButton) ymomentRadioButton->setEnabled(false);
		if (orComboBox) { orComboBox->setEnabled(false); orComboBox->clear(); orComboBox->addItem("Or ..."); }

		// sliceNSpinBox is a plain, EDITABLE QLineEdit in aquamoto.ui (not a QSpinBox). setFixedWidth
		// (not maximumWidth) locks BOTH min and max to the same value -- the layout literally cannot
		// stretch it, no matter what policy or row space is in play.
		if (sliceSpin) {
			sliceSpin->setValidator(new QIntValidator(1, 1, sliceSpin));   // real range set in openPath()
			sliceSpin->setReadOnly(false);
			sliceSpin->setFixedWidth(50);   // exactly enough for a 5-digit number, hard-locked
		}

		if (!pathEdit || !sliceSlider || !sliceSpin || !splitDryWetCheck || !scaleGlobalCheck ||
		    !waterTransparencySlider || !loadRamBtn || !runInBtn) {
			qWarning("AquamotoWindow: could not find one or more expected controls in aquamoto.ui");
		}

		// A native QScrollBar/QSlider's OWN chrome is unreliable across styles (verified live:
		// "windowsvista" draws NO arrows at all in this Qt6 build; "windows" draws arrows but ugly
		// chunky Win95 buttons) -- stop fighting native styles entirely. Re-skin sliceSlider via a
		// stylesheet into a plain thin modern groove+handle (no native arrows drawn at all: add-line/
		// sub-line width forced to 0), then add two SEPARATE small arrow QToolButtons flanking it in
		// its own row layout, wired straight to its value -- simple slider look + real, always-visible
		// arrows-at-the-tips, independent of whatever QStyle plugin this machine has.
		if (sliceSlider) {
			sliceSlider->setStyleSheet(
				"QScrollBar:horizontal { border: none; background: #e0e0e0; height: 8px; margin: 0px; border-radius: 4px; }"
				"QScrollBar::handle:horizontal { background: #808080; border-radius: 4px; min-width: 20px; }"
				"QScrollBar::handle:horizontal:hover { background: #606060; }"
				"QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
				"QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }");
			if (auto *row = w->findChild<QHBoxLayout *>("sliceSliderLayout")) {
				const int idx = row->indexOf(sliceSlider);
				auto *leftBtn  = new QToolButton(w);
				auto *rightBtn = new QToolButton(w);
				leftBtn->setArrowType(Qt::LeftArrow);
				rightBtn->setArrowType(Qt::RightArrow);
				leftBtn->setAutoRepeat(true);
				rightBtn->setAutoRepeat(true);
				row->insertWidget(idx, leftBtn);
				row->insertWidget(idx + 2, rightBtn);   // sliceSlider shifted to idx+1 by the insert above
				QObject::connect(leftBtn,  &QToolButton::clicked, sliceSlider, [this]() {
					if (sliceSlider) sliceSlider->setValue(sliceSlider->value() - 1);
				});
				QObject::connect(rightBtn, &QToolButton::clicked, sliceSlider, [this]() {
					if (sliceSlider) sliceSlider->setValue(sliceSlider->value() + 1);
				});
			}
		}

		bool *guard = new bool(false);    // slider<->spin re-entrancy guard, freed with the window
		QObject::connect(w, &QObject::destroyed, w, [guard]{ delete guard; });

		auto openFile = [this, w]() {
			QString p = QFileDialog::getOpenFileName(w, "Select SWW or NC file", prefStartDir(),
			                                         "netCDF files (*.nc *.sww *.NC *.SWW);;All files (*)");
			if (p.isEmpty()) return;
			rememberStartDir(p);
			setAndOpenPath(p);
		};
		if (browseBtn) QObject::connect(browseBtn, &QToolButton::clicked, w, openFile);
		if (pathEdit) {
			QObject::connect(pathEdit, &QLineEdit::returnPressed, w, [this]() {
				if (pathEdit && !pathEdit->text().trimmed().isEmpty()) setAndOpenPath(pathEdit->text().trimmed());
			});
		}

		if (sliceSlider && sliceSpin) {
			QObject::connect(sliceSlider, &QScrollBar::valueChanged, w, [this, guard](int v) {
				if (*guard) return;
				*guard = true; sliceSpin->setText(QString::number(v)); *guard = false;
				fireSlice();
			});
			// A plain edit box has no live valueChanged(int) -- act when the user commits (Enter or
			// focus-out), not on every keystroke (a half-typed number would fire mid-edit otherwise).
			QObject::connect(sliceSpin, &QLineEdit::editingFinished, w, [this, guard]() {
				if (*guard) return;
				bool ok = false;
				int v = sliceSpin->text().toInt(&ok);
				if (!ok) { sliceSpin->setText(QString::number(sliceSlider->value())); return; }
				v = std::clamp(v, sliceSlider->minimum(), sliceSlider->maximum());
				*guard = true; sliceSlider->setValue(v); *guard = false;
				sliceSpin->setText(QString::number(v));   // reflect the clamp back into the box
				fireSlice();
			});
		}
		if (splitDryWetCheck) QObject::connect(splitDryWetCheck, &QCheckBox::toggled, w, [this](bool) { fireSlice(); });
		if (scaleGlobalCheck) QObject::connect(scaleGlobalCheck, &QCheckBox::toggled, w, [this](bool) { fireSlice(); });
		// Shade Water / Shade Land radio = the selector of WHERE the Shading dock operates (water or land).
		// Flipping it SWAPS the on-screen colorbar (water bar <-> land bar) as the INDICATOR of which side
		// can now be changed, and routes the next Shading-dock edit to that side (aquaShadeSelWater). It
		// must NOT re-composite the images -- so NO fireSlice; the land/water illumination stays exactly as
		// is. refreshGridColorbar only rebuilds the colorbar actors, never the drape texture.
		if (shadeWaterBtn) QObject::connect(shadeWaterBtn, &QRadioButton::toggled, w, [this](bool on) {
			if (on && scene_) {
				scene_->aquaShadeSelWater = true; scene_->aquaShowWater = true;
				refreshGridColorbar(scene_); rebuildSceneObjects(scene_);
			}
		});
		if (shadeLandBtn)  QObject::connect(shadeLandBtn,  &QRadioButton::toggled, w, [this](bool on) {
			if (on && scene_) {
				scene_->aquaShadeSelWater = false; scene_->aquaShowWater = false;
				refreshGridColorbar(scene_); rebuildSceneObjects(scene_);
			}
		});
		// Primary-quantities picker: Stage/Xmoment/Ymoment/Or… switches which nc variable is the
		// ACTIVE one (see aquamoto.jl's _aquamoto_set_var). Exclusive as a group -- an explicit
		// QButtonGroup, same reasoning as the shadeWater/shadeLand group above (QUiLoader nesting
		// doesn't guarantee implicit auto-exclusive grouping).
		if (stageRadioButton && xmomentRadioButton && ymomentRadioButton) {
			auto *varGroup = new QButtonGroup(w);
			varGroup->setExclusive(true);
			varGroup->addButton(stageRadioButton);
			varGroup->addButton(xmomentRadioButton);
			varGroup->addButton(ymomentRadioButton);
		}
		auto onVarRadio = [this](QRadioButton *btn) {
			if (settingVar_ || !btn) return;
			if (btn->isChecked()) setActiveVar(btn->property("aquaVar").toString());
		};
		if (stageRadioButton)   QObject::connect(stageRadioButton,   &QRadioButton::toggled, w, [this, onVarRadio](bool) { onVarRadio(stageRadioButton); });
		if (xmomentRadioButton) QObject::connect(xmomentRadioButton, &QRadioButton::toggled, w, [this, onVarRadio](bool) { onVarRadio(xmomentRadioButton); });
		if (ymomentRadioButton) QObject::connect(ymomentRadioButton, &QRadioButton::toggled, w, [this, onVarRadio](bool) { onVarRadio(ymomentRadioButton); });
		if (orComboBox) {
			QObject::connect(orComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), w, [this](int idx) {
				if (settingVar_ || idx <= 0) return;   // index 0 is the "Or ..." placeholder, not a real var
				setActiveVar(orComboBox->itemData(idx).toString());
			});
		}
		if (waterTransparencySlider) {
			QObject::connect(waterTransparencySlider, &QSlider::valueChanged, w, [this](int v) {
				if (waterTransparencyLabel) waterTransparencyLabel->setText(QString("Water transparency %1%").arg(v));
				fireSlice();
			});
		}
		if (loadRamBtn) QObject::connect(loadRamBtn, &QPushButton::clicked, w, [this]() { fireLoadAllRam(); });
		if (runInBtn) QObject::connect(runInBtn, &QPushButton::clicked, w, [this]() { fireRunIn(); });

		wireCinemaTab(w);
		wireBenchsTab(w);
		// The cube can become resident while another tab is in front (the Benchs box, or a run that was
		// asked to keep it). Re-ask Julia whenever a tab comes forward, so "Load all in RAM" is never
		// offering work that is already done — and never stays disabled for a file that was replaced.
		if (auto *tabs = w->findChild<QTabWidget *>("mainTabWidget"))
			QObject::connect(tabs, &QTabWidget::currentChanged, w, [this](int) { refreshRamButton(); });

		// Restore a prior session on this SAME scene (the panel was closed and reopened, or opened a
		// 2nd time on a window that already had a file loaded) instead of starting blank -- Julia
		// still has the file/bathymetry/step-count cached even though this is a brand-new panel.
		QString state;
		if (aquaEval(scene_, QString("InteractiveGMT._aquamoto_state(%1)").arg(aquaScenePtr(scene_)), state) &&
		    !state.trimmed().isEmpty()) {
			const QStringList parts = state.trimmed().split('|');
			if (parts.size() == 4) {
				bool nok = false;
				const int n = parts[1].toInt(&nok);
				if (nok && n >= 1) {
					if (pathEdit) pathEdit->setText(parts[0]);
					if (timeStepsLabel) timeStepsLabel->setText(QString("Time steps = %1").arg(n));
					if (sliceSlider) { sliceSlider->setRange(1, n); sliceSlider->setEnabled(true); }
					if (sliceSpin) {
						sliceSpin->setValidator(new QIntValidator(1, n, sliceSpin));
						sliceSpin->setEnabled(true);
					}
					if (loadRamBtn) loadRamBtn->setEnabled(true);
					if (runInBtn) runInBtn->setEnabled(true);
					populateVarPicker(parts[2], parts[3].split(',', Qt::SkipEmptyParts));
					opened_ = true;
					refreshRamButton();   // this scene may already have its cube in memory
					cinemaSetRange(n);
				}
			}
		}

		win->adjustSize();   // open at the layout's true minimum -- see the loader comment above
	}

	// Run one blocking Julia call. `aquaEval` pumps QApplication::processEvents(), so the user can
	// close the window DURING this call -- WA_DeleteOnClose then deletes `win` and (via destroyed ->
	// delete this) this very object while we are still on the stack. We detect that WITHOUT ever
	// blocking the close: hold a LOCAL copy of the shared `alive_` token (survives the object's
	// death); if it reads false after aquaEval returns, `this` is gone. `closedNow` comes back true
	// and every caller MUST return immediately, touching no member of `this` afterward.
	bool runBlocking(const QString &call, QString &out, bool &closedNow) {
		closedNow = false;
		auto alive = alive_;              // local copy -- outlives `this` if it gets deleted mid-call
		busy_ = true;
		const bool ok = aquaEval(scene_, call, out);
		if (!*alive) { closedNow = true; return ok; }   // `this` was destroyed during the pump -- bail
		busy_ = false;
		return ok;
	}

	// "This file is now the session's file": show it in the path box, then open it. THE one entry for
	// naming a file to this window -- the Browse button, the path box's Enter and the file-open route
	// (gmtvtk_aqua_queue_open, 90_c_api.cpp) all come through here, so none of them can end up with a
	// path box that disagrees with what is loaded.
	void setAndOpenPath(const QString &path) {
		if (pathEdit) pathEdit->setText(path);
		openPath(path);
	}

	void openPath(const QString &path) {
		if (busy_) return;   // reentrancy guard -- see fireSlice
		// Busy cursor (hourglass) for the duration of the open -- header read + the eager per-layer
		// min/max prescan (every layer, every time) can take real seconds; the Julia side also raises
		// its own progress dialog for it, this cursor covers the header read on top of that.
		QApplication::setOverrideCursor(Qt::WaitCursor);
		QString out;
		bool closedNow = false;
		const bool ok0 = runBlocking(QString("InteractiveGMT._aquamoto_open(%1,raw\"%2\")")
		                             .arg(aquaScenePtr(scene_)).arg(path), out, closedNow);
		QApplication::restoreOverrideCursor();
		if (closedNow) return;   // `this` may already be destroyed -- touch NOTHING below
		if (!ok0) {
			QMessageBox::warning(win, "Aquamoto", out.isEmpty() ? "could not open the file" : out);
			return;
		}
		const QStringList parts = out.trimmed().split('|');   // "nsteps|activevar|var1,var2,…"
		bool ok = false;
		int n = parts.isEmpty() ? 0 : parts[0].toInt(&ok);
		if (!ok || n < 1) n = 1;
		opened_ = true;
		if (timeStepsLabel) timeStepsLabel->setText(QString("Time steps = %1").arg(n));
		if (sliceSlider) { sliceSlider->setRange(1, n); sliceSlider->setValue(1); sliceSlider->setEnabled(true); }
		if (sliceSpin) {
			sliceSpin->setValidator(new QIntValidator(1, n, sliceSpin));
			sliceSpin->setText("1");
			sliceSpin->setEnabled(true);
		}
		// A NEW FILE IS ON DISK UNTIL SOMETHING SAYS OTHERWISE. This resets "In RAM ✓" — the previous
		// file's residency says nothing about this one.
		markCubeOnDisk();
		// A run or a load asked for from the Benchs tab ends HERE, with its cube on screen: the work
		// is done and the controls the user now wants are the netCDF tab's, so go there.
		if (benchGotoNetcdf_) {
			benchGotoNetcdf_ = false;
			// THE TWO TABS MUST NAME THE SAME FILE. What is displayed is the run CROPPED to the display
			// window (…_display20km.nc), written beside the run itself — so the Benchs box, left showing
			// the run's own name, disagreed with the netCDF tab about what was loaded. It now shows the
			// file that is actually on screen; a Run strips the marker back off to get its target.
			if (benchSaveEdit && pathEdit && !pathEdit->text().trimmed().isEmpty())
				benchSaveEdit->setText(QDir::toNativeSeparators(pathEdit->text().trimmed()));
			benchRefreshRam();
			selectTab("netcdfTab");
		}
		if (runInBtn) runInBtn->setEnabled(true);
		if (parts.size() == 3) populateVarPicker(parts[1], parts[2].split(',', Qt::SkipEmptyParts));
		cinemaSetRange(n);                     // Cinema tab's From/To + its zoom/profile defaults
		// A TANK STARTS IN 3-D. The window this opens into was promoted from the empty launcher, which
		// is a flat 2-D map — and flat-2D locks drag-rotation and hides the gizmo, so a tsunami would
		// arrive as a picture you cannot turn. The mode is set HERE, once, where the tank is born,
		// through the same switch the toolbar's view-mode flyout uses; nothing else changes it behind
		// the user's back afterwards.
		if (scene_ && sceneAlive(scene_) && scene_->flat2d) sceneSetViewMode(scene_, IGVIEW_3D);
		fireSlice();
		// …AND IT STANDS ON ITS SURFACE. A slice is always pushed as a flat draped image
		// (showLayerImageTail sets layerImgMode on every push), so the mode switch above leaves a
		// picture lying flat under an oblique camera. This is the window's OWN "Shaded image (2-D)"
		// switch — the same one the Shading dock's box and the Cinema tab's "3-D surface" throw —
		// applied once, where the tank is born, so a tank opened by hand and a tank opened by a
		// benchmark come up alike.
		if (scene_ && sceneAlive(scene_)) sceneSetShadedImage2D(scene_, false);
		// THE VTK ILLUMINATION. A tsunami window comes up lit like every other window in this program:
		// the VTK (PBR) look, set through the ONE switch the Shading dock's own box throws
		// (sceneSetReliefLook). It used to open at RL_None — no relief light at all — so the composite
		// arrived flat.
		if (scene_ && sceneAlive(scene_)) sceneSetReliefLook(scene_, RL_PBR, /*keepExternShade=*/true);
		// THE LAYER'S GEOMETRY IS NOT TOUCHED HERE. A tsunami opens as the composited image it has
		// always opened as — land coloured from the bathymetry, water from the stage. Switching it to
		// the 3-D surface hands it to the plain-grid builder, which colours the stage with the WATER
		// CPT and paints the land red; doing that automatically at open made every tank arrive that
		// way. It is the user's switch to throw, not this function's.
		// The first slice is what fills in the tank's extent, so the figure is put up after it.
		if (cineProfCheck && cineProfCheck->isChecked()) showEtaFigure(true);
	}

	// Fill the Stage/Xmoment/Ymoment/Or… quantity picker from a just-opened (or restored) file's
	// variable list, and check/select whichever is the currently active one. `settingVar_` guards
	// every widget mutation here so none of it fires the toggled/currentIndexChanged handlers back
	// into setActiveVar -- this function only reflects state Julia already has, never changes it.
	void populateVarPicker(const QString &activeVar, const QStringList &allVars) {
		settingVar_ = true;
		activeVar_ = activeVar;
		if (stageRadioButton)   { stageRadioButton->setEnabled(false);   stageRadioButton->setProperty("aquaVar", QString()); stageRadioButton->setChecked(false); }
		if (xmomentRadioButton) { xmomentRadioButton->setEnabled(false); xmomentRadioButton->setProperty("aquaVar", QString()); xmomentRadioButton->setChecked(false); }
		if (ymomentRadioButton) { ymomentRadioButton->setEnabled(false); ymomentRadioButton->setProperty("aquaVar", QString()); ymomentRadioButton->setChecked(false); }
		if (orComboBox) { orComboBox->clear(); orComboBox->addItem("Or ..."); orComboBox->setEnabled(false); }
		for (const QString &v : allVars) {
			const QString lv = v.toLower();
			if (lv == "stage" && stageRadioButton) {
				stageRadioButton->setProperty("aquaVar", v); stageRadioButton->setEnabled(true);
			} else if ((lv == "xmoment" || lv == "xmomentum") && xmomentRadioButton) {
				xmomentRadioButton->setProperty("aquaVar", v); xmomentRadioButton->setEnabled(true);
			} else if ((lv == "ymoment" || lv == "ymomentum") && ymomentRadioButton) {
				ymomentRadioButton->setProperty("aquaVar", v); ymomentRadioButton->setEnabled(true);
			} else if (orComboBox) {
				orComboBox->addItem(v, v);
				orComboBox->setEnabled(true);
			}
		}
		if (stageRadioButton && stageRadioButton->property("aquaVar").toString() == activeVar) {
			stageRadioButton->setChecked(true);
		} else if (xmomentRadioButton && xmomentRadioButton->property("aquaVar").toString() == activeVar) {
			xmomentRadioButton->setChecked(true);
		} else if (ymomentRadioButton && ymomentRadioButton->property("aquaVar").toString() == activeVar) {
			ymomentRadioButton->setChecked(true);
		} else if (orComboBox) {
			const int idx = orComboBox->findData(activeVar);
			orComboBox->setCurrentIndex(idx >= 0 ? idx : 0);
		}
		settingVar_ = false;
	}

	// Switch the active quantity variable (called from the Stage/Xmoment/Ymoment/Or… picker) and
	// re-render the current slice against it. Reentrancy-guarded like every other blocking call here.
	void setActiveVar(const QString &varname) {
		if (busy_ || varname.isEmpty() || varname == activeVar_) return;
		QString out;
		bool closedNow = false;
		const bool ok = runBlocking(QString("InteractiveGMT._aquamoto_set_var(%1,raw\"%2\")")
		                            .arg(aquaScenePtr(scene_)).arg(varname), out, closedNow);
		if (closedNow) return;   // `this` may already be destroyed -- touch NOTHING below
		if (!ok) {
			QMessageBox::warning(win, "Aquamoto", out.isEmpty() ? "could not switch quantity variable" : out);
			return;
		}
		activeVar_ = varname;
		// RAM residency is PER VARIABLE (each is its own cube), so the button now describes the one
		// just switched to — which may be on disk even though the previous variable is in memory.
		refreshRamButton();
		fireSlice();
	}

	// ==========================================================================================
	//  CINEMA TAB — playback, the camera boxes and the floating η(x) figure.
	// ==========================================================================================

	static double editNum(QLineEdit *e, double dflt) {
		if (!e) return dflt;
		bool ok = false;
		const double v = e->text().trimmed().toDouble(&ok);
		return ok ? v : dflt;
	}

	void wireCinemaTab(QMainWindow *w) {
		cinePlayBtn      = w->findChild<QPushButton *>("cinemaPlayButton");
		cineFirstBtn     = w->findChild<QPushButton *>("cinemaFirstButton");
		cinePrevBtn      = w->findChild<QPushButton *>("cinemaPrevButton");
		cineNextBtn      = w->findChild<QPushButton *>("cinemaNextButton");
		cineLastBtn      = w->findChild<QPushButton *>("cinemaLastButton");
		cineResetViewBtn = w->findChild<QPushButton *>("cinemaResetViewButton");
		cineRateEdit     = w->findChild<QLineEdit *>("cinemaRateEdit");
		cineFromEdit     = w->findChild<QLineEdit *>("cinemaFromEdit");
		cineToEdit       = w->findChild<QLineEdit *>("cinemaToEdit");
		cineAzEdit       = w->findChild<QLineEdit *>("cinemaAzimuthEdit");
		cineElEdit       = w->findChild<QLineEdit *>("cinemaElevationEdit");
		cineZoomEdit     = w->findChild<QLineEdit *>("cinemaZoomEdit");
		cineVeEdit       = w->findChild<QLineEdit *>("cinemaVeEdit");
		cineSpinEdit     = w->findChild<QLineEdit *>("cinemaSpinEdit");
		cineProfX0Edit   = w->findChild<QLineEdit *>("cinemaProfileX0Edit");
		cineProfLenEdit  = w->findChild<QLineEdit *>("cinemaProfileLenEdit");
		cineLoopCheck    = w->findChild<QCheckBox *>("cinemaLoopCheckBox");
		cine3DCheck      = w->findChild<QCheckBox *>("cinema3DCheckBox");
		cineSpinCheck    = w->findChild<QCheckBox *>("cinemaSpinCheckBox");
		cineProfCheck    = w->findChild<QCheckBox *>("cinemaProfileCheckBox");

		cineTimer = new QTimer(w);
		QObject::connect(cineTimer, &QTimer::timeout, w, [this]() { cinemaTick(); });

		if (cinePlayBtn)
			QObject::connect(cinePlayBtn, &QPushButton::toggled, w, [this](bool on) { cinemaSetPlaying(on); });
		if (cineFirstBtn) QObject::connect(cineFirstBtn, &QPushButton::clicked, w, [this]() { cinemaGoto(cinemaFrom()); });
		if (cineLastBtn)  QObject::connect(cineLastBtn,  &QPushButton::clicked, w, [this]() { cinemaGoto(cinemaTo()); });
		// ONE STEP PER PRESS. These four are single-shot buttons: no auto-repeat, and (see fireSlice)
		// they are disabled while the slice they asked for is being drawn, so a held button cannot pile
		// up presses that then replay as a burst of frames.
		for (QPushButton *b : { cinePrevBtn, cineNextBtn, cineFirstBtn, cineLastBtn })
			if (b) b->setAutoRepeat(false);
		if (cinePrevBtn)  QObject::connect(cinePrevBtn,  &QPushButton::clicked, w, [this]() {
			if (sliceSlider) cinemaGoto(sliceSlider->value() - 1); });
		if (cineNextBtn)  QObject::connect(cineNextBtn,  &QPushButton::clicked, w, [this]() {
			if (sliceSlider) cinemaGoto(sliceSlider->value() + 1); });
		// A new frame rate applies to the run in progress, not only to the next one.
		if (cineRateEdit) QObject::connect(cineRateEdit, &QLineEdit::editingFinished, w, [this]() {
			if (cineTimer && cineTimer->isActive()) cineTimer->start(cinemaIntervalMs()); });
		// The 3-D box is the Shading dock's own switch and nothing more: it chooses the GEOMETRY the
		// layer is drawn on (a warped surface vs a flat draped image), never the window's view mode.
		// The window is already in 3-D — a tank window is put there when it opens (openPath).
		if (cine3DCheck) QObject::connect(cine3DCheck, &QCheckBox::toggled, w, [this](bool on) {
			if (!scene_ || !sceneAlive(scene_)) return;
			if (scene_->layerImgMode == on) sceneSetShadedImage2D(scene_, !on); });

		auto applyView = [this]() { applyCameraFromBoxes(); };
		for (QLineEdit *e : { cineAzEdit, cineElEdit, cineZoomEdit, cineVeEdit })
			if (e) QObject::connect(e, &QLineEdit::editingFinished, w, applyView);
		if (cineResetViewBtn) QObject::connect(cineResetViewBtn, &QPushButton::clicked, w, [this]() { cinemaResetView(); });

		// THE BOXES FOLLOW THE MOUSE. Rotating or zooming with the mouse is the same operation the
		// Azimuth/Elevation/Zoom boxes perform, so the window has ONE view and the boxes must report
		// it — a box that still says -35 while the view is at 12 is simply lying. The camera is read
		// back through sceneGetViewAzElSpan (the mirror of the setter the boxes use), never re-derived
		// here. A box the user is typing in is left alone until focus leaves it.
		viewSyncTimer = new QTimer(w);
		viewSyncTimer->setInterval(200);
		QObject::connect(viewSyncTimer, &QTimer::timeout, w, [this]() { syncViewBoxes(); });
		viewSyncTimer->start();

		if (cineProfCheck) QObject::connect(cineProfCheck, &QCheckBox::toggled, w, [this](bool on) {
			showEtaFigure(on); });
		for (QLineEdit *e : { cineProfX0Edit, cineProfLenEdit })
			if (e) QObject::connect(e, &QLineEdit::editingFinished, w, [this]() { updateEtaFigure(); });
	}

	int  cinemaFrom() const { return sliceSlider ? std::max(sliceSlider->minimum(), (int)editNum(cineFromEdit, sliceSlider->minimum())) : 1; }
	int  cinemaTo()   const { return sliceSlider ? std::min(sliceSlider->maximum(), (int)editNum(cineToEdit,   sliceSlider->maximum())) : 1; }
	int  cinemaIntervalMs() const {
		const double fps = editNum(cineRateEdit, 5.0);
		return std::max(20, int(1000.0 / ((fps > 0.0) ? fps : 5.0)));
	}
	bool cinemaPlaying() const { return cineTimer && cineTimer->isActive(); }

	// The slice range a freshly opened file offers, and the zoom box's starting value (the whole
	// tank). Called from openPath/the session restore, where the step count becomes known.
	void cinemaSetRange(int n) {
		if (cineFromEdit) cineFromEdit->setText("1");
		if (cineToEdit)   cineToEdit->setText(QString::number(std::max(1, n)));
		if (cineZoomEdit && cineZoomEdit->text().trimmed().isEmpty() && scene_ && sceneAlive(scene_))
			cineZoomEdit->setText(QString::number(scene_->gx1 - scene_->gx0, 'g', 6));
		if (cineProfX0Edit && cineProfX0Edit->text().trimmed().isEmpty() && scene_ && sceneAlive(scene_))
			cineProfX0Edit->setText(QString::number(scene_->gx0, 'g', 6));
	}

	void cinemaSetPlaying(bool on) {
		if (!cineTimer) return;
		if (on && opened_) { cineTimer->start(cinemaIntervalMs()); }
		else               { cineTimer->stop(); }
		if (cinePlayBtn) {
			QSignalBlocker b(cinePlayBtn);              // setChecked here must not re-enter this slot
			cinePlayBtn->setChecked(cinemaPlaying());
			cinePlayBtn->setText(cinemaPlaying() ? "Pause" : "Play");
		}
	}

	// ==========================================================================================
	//  BENCHS TAB — the benchmark runs. One group per benchmark; the first is "10% slope beach,
	//  Benchmark 1" (the solitary wave up a sloping beach). Opening the tool never starts a run:
	//  the model is put on screen at t = 0 by the menu entry, and THIS tab is where a run is asked
	//  for, or a previous one loaded.
	// ==========================================================================================

	// Where a run is written, unless the user says otherwise: the OS temp directory, in a `claude`
	// folder of its own. Only a DEFAULT — the box is editable and the "…" button browses.
	static QString benchDefaultSavePath() {
		return QDir(QDir::tempPath()).filePath("claude/benchmark1.nc");
	}

	// SAY IT — DO NOT ASK IT. A run of this benchmark already sitting at the proposed path is
	// INFORMATION the user must have the moment the tool comes up: what the file is, how big it is,
	// and that it can be loaded. It is NOT a question, and nothing is loaded here — the offer to load
	// instead of recomputing belongs to the Run button alone (benchOfferExisting), never to the
	// opening of the dialog.
	void benchAnnounceExisting() {
		if (!win || !benchSaveEdit) return;
		const QString out = benchSaveEdit->text().trimmed();
		const QFileInfo fi(out);
		const bool there = !out.isEmpty() && fi.exists();
		// THE NOTICE STANDS IN THE TAB, where it cannot be wiped. The status bar was not enough: the
		// tank opening right behind this overwrites it within the same second, so the one place the
		// user was told was gone before it could be read.
		if (benchExistsLabel) {
			benchExistsLabel->setVisible(there);
			if (there)
				benchExistsLabel->setText(
					QString("%1 ALREADY EXISTS (%2 MB, %3) — press \"Load from disk…\" to open it "
					        "instead of running the model again.")
					    .arg(QDir::toNativeSeparators(out))
					    .arg(fi.size() / (1024.0 * 1024.0), 0, 'f', 1)
					    .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm")));
		}
		if (!there) return;
		win->statusBar()->showMessage(
			QString("Benchmark 1: %1 already exists (%2 MB, %3) — \"Load from disk…\" opens it")
			    .arg(QDir::toNativeSeparators(out))
			    .arg(fi.size() / (1024.0 * 1024.0), 0, 'f', 1)
			    .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm")));
	}

	void wireBenchsTab(QMainWindow *w) {
		(void)w;
		if (benchSaveEdit && benchSaveEdit->text().trimmed().isEmpty())
			benchSaveEdit->setText(QDir::toNativeSeparators(benchDefaultSavePath()));
		if (benchProgress) { benchProgress->setRange(0, 100); benchProgress->setValue(0); }
		// The notice's own row, created here and NOT in the .ui: it is data, shown only while the file
		// it names is really on disk, and it sits where it belongs — under the save-path row, above the
		// RAM box. Nothing else in the tab moves.
		if (!benchExistsLabel && benchKeepRamCheck) {
			if (auto *lay = w->findChild<QVBoxLayout *>("bench1Layout")) {
				benchExistsLabel = new QLabel(w);
				benchExistsLabel->setWordWrap(true);
				benchExistsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
				const int idx = lay->indexOf(benchKeepRamCheck);
				if (idx >= 0) lay->insertWidget(idx, benchExistsLabel);
				else          lay->addWidget(benchExistsLabel);
				benchExistsLabel->hide();
			}
		}

		benchRefreshRam();
		benchAnnounceExisting();

		if (benchBrowseBtn) QObject::connect(benchBrowseBtn, &QPushButton::clicked, win, [this]() {
			const QString f = QFileDialog::getSaveFileName(win, "Save the simulation as",
			                       benchSaveEdit ? benchSaveEdit->text() : benchDefaultSavePath(),
			                       "netCDF (*.nc);;All files (*)");
			if (!f.isEmpty() && benchSaveEdit) benchSaveEdit->setText(QDir::toNativeSeparators(f));
			benchRefreshRam();                       // a different file is a different size
			benchAnnounceExisting();                 // …and may or may not already be on disk
		});
		// A different path may point at a run that already exists — the size and the notice follow it.
		if (benchSaveEdit) QObject::connect(benchSaveEdit, &QLineEdit::editingFinished, win,
		                                    [this]() { benchRefreshRam(); benchAnnounceExisting(); });

		// Keeping the cube in memory is the netCDF tab's own "Load all in RAM" (`_aqua_load_all`),
		// reached from here for the file this tab produced — never a second loader. Ticked before the
		// file exists, it is applied when the run's cube is opened (see fireBenchRun).
		if (benchKeepRamCheck) QObject::connect(benchKeepRamCheck, &QCheckBox::toggled, win, [this](bool on) {
			if (!on || !scene_ || !sceneAlive(scene_) || !opened_ || busy_) return;
			// FIRE AND FORGET. Reading the whole cube takes seconds; doing it inside this click would
			// freeze the dialog with nothing to look at (and a tick that appears to do nothing). The
			// Julia side does it on a task and the netCDF tab's button reports the outcome.
			QString reply; bool closedNow = false;
			runBlocking(QString("(@async InteractiveGMT._bm1_keep_in_ram(%1); nothing)").arg(aquaScenePtr(scene_)),
			            reply, closedNow);
			if (closedNow) return;
			if (win) win->statusBar()->showMessage("Reading the whole cube into memory…", 4000);
		});

		// RUN. The Julia side launches NSWING the way every other run in this program is launched —
		// off-process, watched by a timer that drives the progress bar (nswing.jl) — so this call
		// RETURNS as soon as the run has started and the window stays live while it computes. The
		// tab's own bar is registered as a mirror of that one progress source, never a second one.
		if (benchRunBtn) QObject::connect(benchRunBtn, &QPushButton::clicked, win, [this]() {
			if (!scene_ || !sceneAlive(scene_)) return;
			// The box may be showing the DISPLAY crop of an earlier run (that is what the two tabs agree
			// on once one is loaded). A run writes the RUN, so the marker comes back off first — and
			// everything below, the overwrite check included, then talks about the same file.
			if (benchSaveEdit) {
				QString p = benchSaveEdit->text().trimmed();
				const int cut = p.indexOf("_display");
				if (cut > 0 && p.endsWith(".nc")) benchSaveEdit->setText(p.left(cut) + ".nc");
			}
			const QString out = benchSaveEdit ? benchSaveEdit->text().trimmed() : QString();
			if (out.isEmpty()) {
				QMessageBox::warning(win, "Benchmark 1", "Say where the simulation is to be saved.");
				return;
			}
			// Already computed into this very file? Offer it rather than spend the minutes again.
			if (benchOfferExisting()) return;
			benchGotoNetcdf_ = true;      // when the run lands, the netCDF tab is what is wanted
			if (benchProgress) { benchProgress->setValue(0); g_progressMirror = benchProgress; }
			QString reply;
			bool closedNow = false;
			const bool keepram = benchKeepRamCheck && benchKeepRamCheck->isChecked();
			const bool ok = runBlocking(QString("InteractiveGMT._on_bench1_run(%1,raw\"%2\",%3)")
			                            .arg(aquaScenePtr(scene_)).arg(QDir::toNativeSeparators(out))
			                            .arg(keepram ? 1 : 0),
			                            reply, closedNow);
			if (closedNow) return;
			if (!ok) QMessageBox::warning(win, "Benchmark 1",
			                              reply.isEmpty() ? "could not start the run" : reply);
		});

		// LOAD a run made earlier — the same file any other NSWING cube arrives as, so it goes in
		// through the viewer's drop door on the Julia side.
		if (benchLoadBtn) QObject::connect(benchLoadBtn, &QPushButton::clicked, win, [this]() {
			if (!scene_ || !sceneAlive(scene_)) return;
			const QString f = QFileDialog::getOpenFileName(win, "Open a simulation",
			                       benchSaveEdit ? benchSaveEdit->text() : benchDefaultSavePath(),
			                       "netCDF (*.nc);;All files (*)");
			if (!f.isEmpty()) benchLoad(f);
		});
	}

	// Is there already a run of this benchmark at the file a RUN is about to write? Then offer it
	// instead of spending the minutes again. Asked ONLY when Run is pressed — never while the dialog
	// is being opened, where a question nobody asked for stands between the user and the tool.
	// Returns true if a file was loaded (so the caller does not run).
	bool benchOfferExisting() {
		if (!benchSaveEdit) return false;
		const QString out = benchSaveEdit->text().trimmed();
		if (out.isEmpty() || !QFileInfo::exists(out)) return false;
		QMessageBox box(win);
		box.setWindowTitle("Benchmark 1");
		box.setIcon(QMessageBox::Question);
		box.setText("A simulation already exists at\n" + out);
		box.setInformativeText("Load it, or run the model again and overwrite it?");
		QPushButton *bLoad = box.addButton("Load it", QMessageBox::AcceptRole);
		QPushButton *bRam  = box.addButton("Load it and keep it all in RAM", QMessageBox::AcceptRole);
		box.addButton("Run again", QMessageBox::DestructiveRole);
		box.setDefaultButton(bLoad);
		box.exec();
		if (box.clickedButton() == bRam && benchKeepRamCheck)
			benchKeepRamCheck->setChecked(true);   // benchLoad passes this flag to the open
		if (box.clickedButton() == bLoad || box.clickedButton() == bRam) { benchLoad(out); return true; }
		return false;                              // "Run again"
	}

	// Open a simulation that already exists — what the "Load from disk…" button does.
	void benchLoad(const QString &path) {
		if (!scene_ || !sceneAlive(scene_)) return;
		benchGotoNetcdf_ = true;      // same for a load: the file lands, the netCDF tab takes over
		if (benchSaveEdit) benchSaveEdit->setText(QDir::toNativeSeparators(path));
		benchRefreshRam();
		QString reply;
		bool closedNow = false;
		const bool keepram = benchKeepRamCheck && benchKeepRamCheck->isChecked();
		const bool ok = runBlocking(QString("InteractiveGMT._on_bench1_load(%1,raw\"%2\",%3)")
		                            .arg(aquaScenePtr(scene_)).arg(QDir::toNativeSeparators(path))
		                            .arg(keepram ? 1 : 0), reply, closedNow);
		if (closedNow) return;
		if (!ok) QMessageBox::warning(win, "Benchmark 1",
		                              reply.isEmpty() ? "could not open that file" : reply);
	}

	// SAY WHAT IT COSTS. The RAM box carries the size the cube would take in memory — read off the
	// file when it exists, and predicted from the model the run will produce when it does not, so the
	// number is there before anything has been computed.
	void benchRefreshRam() {
		if (!benchKeepRamCheck || !scene_ || !sceneAlive(scene_)) return;
		// NEVER FROM INSIDE THE WINDOW'S CONSTRUCTION, and never while a slice is in flight: this is a
		// blocking Julia call, and one made while the dialog is still being built (or while another
		// one is running) is a stall with no visible cause. Deferred to the next turn of the loop, so
		// the tab is on screen first, and skipped outright while the window is busy.
		if (busy_) return;
		auto alive = alive_;
		QTimer::singleShot(0, win, [this, alive]() {
			if (!*alive || busy_) return;
			benchRefreshRamNow();
		});
	}

	void benchRefreshRamNow() {
		if (!benchKeepRamCheck || !scene_ || !sceneAlive(scene_)) return;
		const QString path = benchSaveEdit ? benchSaveEdit->text().trimmed() : QString();
		QString reply;
		if (!aquaEval(scene_, QString("InteractiveGMT._on_bench1_ram_mb(raw\"%1\")").arg(path), reply))
			return;
		bool ok = false;
		const double mb = reply.trimmed().toDouble(&ok);
		if (!ok) return;
		benchKeepRamCheck->setText(mb >= 1024.0
			? QString("Keep whole cube in RAM (≈ %1 GB)").arg(mb / 1024.0, 0, 'f', 1)
			: QString("Keep whole cube in RAM (≈ %1 MB)").arg(mb, 0, 'f', mb < 10 ? 1 : 0));
	}

	// "Load all in RAM" — the option every OTHER netCDF cube gets from the cube dock's button, which a
	// tsunami file never reaches because it opens through this dialog. Same Julia side (_aqua_load_all
	// -> the shared _read_whole_cube / _cube_fits_ram), same return codes and the same three answers
	// the dock gives, so the two buttons behave identically. After it, slice changes slice memory.
	void fireLoadAllRam() {
		if (!opened_ || busy_ || !loadRamBtn) return;
		QApplication::setOverrideCursor(Qt::WaitCursor);
		QApplication::processEvents();                  // let the cursor paint before we block
		QString out;
		bool closedNow = false;
		const bool ok = runBlocking(QString("InteractiveGMT._aqua_load_all(%1)").arg(aquaScenePtr(scene_)),
		                            out, closedNow);
		if (closedNow) return;                          // the window died during the call: touch nothing
		QApplication::restoreOverrideCursor();
		const int rc = ok ? out.trimmed().toInt() : 2;
		if (rc == 0) {
			markCubeInRam();
			if (scene_ && sceneAlive(scene_) && scene_->win)
				scene_->win->statusBar()->showMessage(
					"Cube loaded into RAM — slice changes are now instant", 5000);
		}
		else if (rc == 1) {
			QMessageBox::warning(win, "Load all in RAM",
				"Not enough free RAM to hold the whole cube in memory.\n"
				"Keeping the per-layer disk reads.");
		}
		else {
			QMessageBox::warning(win, "Load all in RAM", "Failed to load the cube into memory.");
		}
	}

	// The button's two states. Resident is a dead end (nothing left to load); anything else offers the
	// load. Both are set from what JULIA actually holds, never from a flag kept on this side.
	void markCubeInRam() {
		if (!loadRamBtn) return;
		loadRamBtn->setText("In RAM \xE2\x9C\x93");
		loadRamBtn->setEnabled(false);
	}
	void markCubeOnDisk() {
		if (!loadRamBtn) return;
		loadRamBtn->setText("Load all in RAM");
		loadRamBtn->setEnabled(opened_);
	}
	// Ask the Julia side whether the ACTIVE variable is resident and show that. Called wherever the
	// answer can have changed: a file opened, the variable switched, the panel reopened on a scene
	// that already had a cube loaded.
	void refreshRamButton() {
		if (!loadRamBtn || !opened_ || busy_) return;   // never a blocking call on top of another
		QString out;
		if (aquaEval(scene_, QString("InteractiveGMT._aqua_in_ram(%1)").arg(aquaScenePtr(scene_)), out)
		    && out.trimmed() == "1")
			markCubeInRam();
		else
			markCubeOnDisk();
	}

	// The step buttons, off while a slice is being drawn and on again after. Qt drops clicks aimed at a
	// disabled widget, so a held (or hammered) < / > gives exactly ONE step per completed redraw
	// instead of a queue of frames replayed afterwards. Play/Stop is deliberately NOT in here — the
	// user must be able to stop an animation while its frame is still drawing.
	void transportEnable(bool on) {
		for (QPushButton *b : { cinePrevBtn, cineNextBtn, cineFirstBtn, cineLastBtn })
			if (b) b->setEnabled(on);
	}

	// Show slice `k` (1-based) by MOVING THE SLIDER -- the one control that says which slice is up.
	void cinemaGoto(int k) {
		if (!sliceSlider || !opened_) return;
		const int v = std::max(sliceSlider->minimum(), std::min(sliceSlider->maximum(), k));
		if (v == sliceSlider->value()) fireSlice();      // same slice: still redraw (e.g. after a toggle)
		else                           sliceSlider->setValue(v);   // -> valueChanged -> fireSlice
	}

	void cinemaTick() {
		if (!sliceSlider || !opened_) return;
		if (busy_) return;                               // a slice is still being computed: skip this beat
		const int from = cinemaFrom(), to = cinemaTo();
		int next = sliceSlider->value() + 1;
		if (next > to) {
			if (cineLoopCheck && cineLoopCheck->isChecked()) next = from;
			else { cinemaSetPlaying(false); return; }
		}
		if (next < from) next = from;
		cinemaGoto(next);
	}

	// Everything the window owes a freshly drawn slice: put it back on the 3-D surface if that is what
	// the user asked for, advance a spinning camera, and refresh the η(x) figure. Called at the END of
	// fireSlice, so it covers the slider, the transport buttons, the timer and the display toggles
	// alike -- there is no second path by which a slice reaches the screen.
	void afterSliceShown() {
		if (!scene_ || !sceneAlive(scene_)) return;
		// An Aquamoto slice is always pushed as a flat draped image (showLayerImageTail sets
		// layerImgMode), so the mode the window was in has to be re-asserted after each one -- through
		// the shared switch, from the state fireSlice read off the scene just before the push.
		if (wants3D_ && scene_->layerImgMode) sceneSetShadedImage2D(scene_, false);
		// …and the Cinema box says what the window IS. It is a switch, not the owner of the mode.
		if (cine3DCheck && cine3DCheck->isChecked() != wants3D_) {
			QSignalBlocker b(cine3DCheck);
			cine3DCheck->setChecked(wants3D_);
		}
		if (cinemaPlaying() && cineSpinCheck && cineSpinCheck->isChecked() && cineAzEdit) {
			const double step = editNum(cineSpinEdit, 0.5);
			if (step != 0.0) {
				QSignalBlocker b(cineAzEdit);
				cineAzEdit->setText(QString::number(std::fmod(editNum(cineAzEdit, 0.0) + step, 360.0), 'g', 5));
				applyCameraFromBoxes();
			}
		}
		updateEtaFigure();
	}

	// The camera, from the four boxes, through the ONE placement function (10_geometry.cpp) the host
	// also reaches from Julia (gmtvtk_set_view_azel_h). No camera maths lives in this dialog.
	void applyCameraFromBoxes() {
		if (!scene_ || !sceneAlive(scene_)) return;
		sceneSetViewAzElSpan(scene_, editNum(cineAzEdit, -35.0), editNum(cineElEdit, 20.0),
		                     editNum(cineZoomEdit, 0.0), nullptr, editNum(cineVeEdit, 0.0));
	}

	// Write the LIVE camera into the three view boxes. Only boxes the user is not editing are touched
	// (focus), and only when the number really moved, so a box never fights the cursor or steals a
	// half-typed value. Also keeps the z× box in step with the gizmo's own VE handle.
	void syncViewBoxes() {
		if (!scene_ || !sceneAlive(scene_) || !win || !win->isVisible()) return;
		double az = 0, el = 0, wd = 0;
		if (!sceneGetViewAzElSpan(scene_, az, el, wd)) return;
		auto put = [](QLineEdit *e, double v, int prec) {
			if (!e || e->hasFocus()) return;
			const QString t = QString::number(v, 'g', prec);
			if (e->text() == t) return;
			QSignalBlocker b(*e);          // this is a REPORT, not an edit: must not re-apply the view
			e->setText(t);
		};
		put(cineAzEdit, az, 4);
		put(cineElEdit, el, 3);
		put(cineZoomEdit, wd, 6);
		put(cineVeEdit, scene_->ve, 3);
	}

	void cinemaResetView() {
		if (!scene_ || !sceneAlive(scene_)) return;
		if (cineAzEdit)   cineAzEdit->setText("-35");
		if (cineElEdit)   cineElEdit->setText("20");
		if (cineVeEdit)   cineVeEdit->setText("1");
		if (cineZoomEdit) cineZoomEdit->setText(QString::number(scene_->gx1 - scene_->gx0, 'g', 6));
		applyCameraFromBoxes();
	}

	// ---- the floating η(x) figure ----------------------------------------------------------
	void showEtaFigure(bool on) {
		if (!scene_ || !sceneAlive(scene_) || !scene_->widget) return;
		if (on && !etaFig) {
			etaFig = new EtaFigure(scene_->widget, "\xCE\xB7 (x)");     // UTF-8 "η (x)"
			etaFig->move(10, 10);
			// The minimise button parks it in Scene Objects — the SAME parkTool/unparkTool pair the
			// Aquamoto window, the X,Y plot, Contours and Illumination all park through, so the row,
			// its double-click, its checkbox and its menu behave identically for every tool.
			Scene *sc = scene_;
			QPointer<EtaFigure> fig = etaFig;
			auto unpark = [sc, fig]() {
				if (!fig || !sceneAlive(sc)) return;
				unparkTool(sc, fig);                       // drops the row + rebuilds the dock
				fig->show();
				fig->raise();
			};
			etaFig->onMinimize = [this, sc, fig, unpark]() {
				if (!fig || !sceneAlive(sc)) return;
				fig->hide();                               // hidden, NOT destroyed
				parkTool(sc, fig, "\xCE\xB7 (x) profile", IC_Line,
				         "Parked \xCE\xB7 (x) profile — double-click to bring it back, click for its menu",
				         unpark,
				         [this, sc, fig, unpark](const QPoint &g) {
					QMenu m;
					QAction *aShow = m.addAction("Show");
					m.addSeparator();
					QAction *aDel  = m.addAction("Remove");
					QAction *pick  = m.exec(g);
					if (pick == aShow) unpark();
					else if (pick == aDel) {
						if (sceneAlive(sc) && fig) unparkTool(sc, fig);
						// Removing the figure is the same thing as clearing its Cinema-tab box, so
						// the box follows — a checked box with no figure is a control that lies.
						if (cineProfCheck) cineProfCheck->setChecked(false);
					}
				});
				unfoldSceneObjects(sc);      // a handle the user cannot see is no handle at all
			};
		}
		if (!etaFig) return;
		etaFig->setVisible(on);
		if (on) { etaFig->raise(); updateEtaFigure(); }
	}

	// The profile of the slice ON SCREEN: the row of the window's active data layer nearest mid-tank,
	// over [x0, x0+length]. The layer is the one the hover readout reads, refreshed by every slice, so
	// the curve can never describe a different slice than the surface does.
	void updateEtaFigure() {
		if (!etaFig || !etaFig->isVisible() || !scene_ || !sceneAlive(scene_)) return;
		const double x0  = editNum(cineProfX0Edit, scene_->gx0);
		const double len = editNum(cineProfLenEdit, 5000.0);
		std::vector<double> xs, zs;
		if (!sceneGridRowSeries(scene_, 0.5 * (scene_->gy0 + scene_->gy1), x0, x0 + len, xs, zs)) return;
		const int k = sliceSlider ? sliceSlider->value() : 0;
		etaFig->setCurve(xs, zs, QString("\xCE\xB7  slice %1").arg(k), "x (m)", "\xCE\xB7 (m)");
		// The slice's MODEL TIME in the figure's own title strip. It is the very string the data
		// already publishes for the viewer's titlebar (_aqua_title_time -> gmtvtk_set_title_extra_h
		// -> Scene::titleExtra), read back here: one time value, one format, two places showing it.
		const QString when = QString::fromStdString(scene_->titleExtra);
		etaFig->setTitle(when.isEmpty() ? QString("\xCE\xB7 (x)")
		                                : QString("\xCE\xB7 (x)   \xE2\x80\x94   %1").arg(when));
	}

	void fireSlice() {
		// REENTRANCY GUARD (why the window "could not be killed"): every widget signal here fires a
		// blocking runBlocking(), and aquaEval() pumps QApplication::processEvents() internally. Without
		// this guard a second signal delivered during that pump -- e.g. sliceSpin's editingFinished,
		// which fires the instant focus leaves the box on a Close click -- re-enters fireSlice and starts
		// a NESTED runBlocking. The nested call clears the single `busy_` flag on return, so the outer
		// call's close-defer logic (AquamotoCloseFilter -> closePending_ -> win->close()) desyncs and the
		// user's Close is silently dropped. Refuse to start any new work while one is already in flight.
		// …AND WHAT ARRIVES DURING ONE IS NOT THROWN AWAY, IT IS COALESCED. Holding < or > (or dragging
		// the slider) delivers click after click INTO the processEvents pump of the call already in
		// flight. Dropping them left the screen behind the slider; running them all would run one
		// blocking Julia round-trip per click, which is the whole UI stalled until the backlog drains.
		// One flag: the slider already carries WHICH slice is wanted, so a single redraw at the end
		// catches up with all of them.
		if (busy_) { sliceDirty_ = true; return; }
		if (!opened_ || !sliceSlider) return;
		const int k = sliceSlider->value() - 1;              // 0-based for the Julia side
		const bool split = splitDryWetCheck && splitDryWetCheck->isChecked();
		const bool global = scaleGlobalCheck && scaleGlobalCheck->isChecked();
		const double transp = waterTransparencySlider ? waterTransparencySlider->value() / 100.0 : 0.0;
		const bool shadeWater = !shadeWaterBtn || shadeWaterBtn->isChecked();   // no button found -> behave as always-on
		const bool shadeLand  = !shadeLandBtn  || shadeLandBtn->isChecked();
		// The geometry mode the window is in RIGHT NOW, before the push flattens it (see wants3D_).
		if (scene_ && sceneAlive(scene_)) wants3D_ = !scene_->layerImgMode;
		transportEnable(false);      // a press that lands while this draws is dropped, not queued
		QString out;
		bool closedNow = false;
		const bool ok = runBlocking(QString("InteractiveGMT._aquamoto_slice(%1,%2,%3,%4,%5,%6,%7)")
		                            .arg(aquaScenePtr(scene_)).arg(k)
		                            .arg(split ? "true" : "false").arg(global ? "true" : "false")
		                            .arg(transp, 0, 'f', 4)
		                            .arg(shadeWater ? "true" : "false").arg(shadeLand ? "true" : "false"), out, closedNow);
		if (closedNow) return;   // `this` may already be destroyed -- touch NOTHING below
		transportEnable(true);
		if (!ok && win) win->statusBar()->showMessage("Aquamoto: " + out, 5000);
		if (ok) afterSliceShown();          // 3-D geometry, a spinning camera, the η(x) figure
		// Requests that came in while this one was running: ONE catch-up redraw, on the next turn of
		// the loop (never a recursive call), at whatever slice the slider ended up on.
		if (sliceDirty_) {
			sliceDirty_ = false;
			QTimer::singleShot(0, win, [this]() { fireSlice(); });
		}
	}

	void fireRunIn() {
		if (busy_) return;   // reentrancy guard -- see fireSlice
		if (!opened_) return;
		QString out;
		bool closedNow = false;
		const bool ok = runBlocking(QString("InteractiveGMT._aquamoto_runin(%1)").arg(aquaScenePtr(scene_)), out, closedNow);
		if (closedNow) return;   // `this` may already be destroyed -- touch NOTHING below
		if (!ok) QMessageBox::warning(win, "Aquamoto", out.isEmpty() ? "could not compute the inundation zone" : out);
	}
};

// Install the hooks surfaceObjectMenu (50_scene.cpp) uses to offer "Aquamoto viewer…" on an Aquamoto
// layer's surface handle. This fragment is compiled AFTER 50_scene.cpp/30_app.cpp in the single TU, so
// the globals already exist; a file-scope initializer wires them at load, before any menu can pop.
static bool aquamotoHasWindow(Scene *scene) { return AquamotoWindow::registry().contains(scene); }
static void aquamotoReopen(Scene *scene)    { AquamotoWindow::openFor(nullptr, scene); rebuildSceneObjects(scene); }
static bool aquamotoIsVisible(Scene *scene) {
	AquamotoWindow *w = AquamotoWindow::registry().value(scene, nullptr);
	return w && w->win && w->win->isVisible();
}
static void aquamotoSetVisible(Scene *scene, int on) {
	AquamotoWindow *w = AquamotoWindow::registry().value(scene, nullptr);
	if (!w || !w->win) { if (on) AquamotoWindow::openFor(nullptr, scene); return; }
	if (on) {
		unparkTool(scene, w->win);          // back on screen -> it is no longer a parked handle
		w->win->show(); w->win->raise(); w->win->activateWindow();
	}
	else { w->win->hide(); }
}

// Bring the parked window back — what a double-click (or ticking its parked row) runs.
static void aquamotoUnpark(Scene *scene) { aquamotoSetVisible(scene, 1); }

// The parked row's menu, serving BOTH its properties button and its context menu (parkTool takes one
// menu for both, so a tool cannot end up with two). "Show" is the same route as the double-click;
// Remove is deliberately absent — this window's lifetime is tied to its cube surface, whose own
// Remove destroys it (g_aquamotoDestroy, sceneRemoveSurface).
static std::function<void(const QPoint &)> aquamotoParkedMenu(Scene *scene) {
	return [scene](const QPoint &g) {
		if (!sceneAlive(scene)) return;
		QMenu m(scene->widget);
		QAction *aShow = m.addAction("Show");
		if (m.exec(g) == aShow) aquamotoUnpark(scene);
	};
}
// Destroy the window for good -- called when its nc cube surface is removed (the window is otherwise
// un-killable). Deleting `win` bypasses the close filter (which only swallows QCloseEvent); its
// destroyed handler removes the registry entry and deletes the wrapper.
static void aquamotoDestroy(Scene *scene) {
	AquamotoWindow *w = AquamotoWindow::registry().value(scene, nullptr);
	if (!w || !w->win) return;
	// OFF SCREEN AND SILENT *NOW*, deleted on the next turn of the loop. The delete has to stay
	// deferred (this can be reached from inside a blocking Julia call made BY this window — the
	// `alive_` guard exists for exactly that), but the caller may be the viewer window's own
	// destroyed handler, which frees the Scene the moment it returns. Between those two instants
	// this window's timers (the Cinema playback beat, the 200 ms view-box sync) would fire against a
	// freed Scene. Stopping them and hiding here closes that gap, and it is also what the user sees:
	// the viewer goes, its Aquamoto goes with it, in the same instant.
	if (w->cineTimer)     w->cineTimer->stop();
	if (w->viewSyncTimer) w->viewSyncTimer->stop();
	w->win->hide();
	w->win->deleteLater();
}
// "Color Bar water"/"Color Bar Land" colormap chooser (50_scene.cpp aquaWaterColorbarRow/
// aquaLandColorbarRow) -- side 0=water, 1=land. Stores the new cmap in the Julia _AquaState
// (_aquamoto_set_cmap) then re-renders the current slice through the SAME fireSlice() every other
// Aquamoto control (split/global/transparency/shade toggles) already uses, so the composited
// texture AND the picked side's colorbar legend both pick up the new colormap in one call.
static void aquamotoSetCmap(Scene *scene, int side, const char *cmap) {
	AquamotoWindow *w = AquamotoWindow::registry().value(scene, nullptr);
	if (!w || !cmap || !cmap[0]) return;
	QString out; bool closedNow = false;
	const bool ok = w->runBlocking(QString("InteractiveGMT._aquamoto_set_cmap(%1,%2,raw\"%3\")")
	                    .arg(aquaScenePtr(scene)).arg(side).arg(QString::fromUtf8(cmap)), out, closedNow);
	if (closedNow) return;   // `w` may already be destroyed -- touch NOTHING below
	if (!ok) { if (w->win) w->win->statusBar()->showMessage("Aquamoto: " + out, 5000); return; }
	w->fireSlice();
}
// Show this scene's Aquamoto window with the "Benchs" tab in front, opening NO file. A benchmark's
// menu entry calls this FIRST: the dialog the user is about to work in must be there immediately,
// not after a model has been built and a tank opened.
//
// IT ASKS NOTHING. No question about an existing file, or anything else, stands between the user and
// the tool while it is opening — the existing-run offer belongs to the Run button alone. It does TELL:
// a run already on disk at the proposed path is announced in the status bar (benchAnnounceExisting),
// which is information, not a prompt.
static int aquamotoShowBenchs(Scene *scene) {
	if (!sceneAlive(scene)) return 0;
	AquamotoWindow::openFor(scene->win, scene);
	AquamotoWindow *w = AquamotoWindow::registry().value(scene, nullptr);
	if (!w || !w->win) return 0;
	if (auto *tabs = w->win->findChild<QTabWidget *>("mainTabWidget"))
		if (QWidget *page = w->win->findChild<QWidget *>("benchsTab"))
			tabs->setCurrentWidget(page);
	w->benchAnnounceExisting();      // tell, before anything else happens
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);   // paint it before the caller works
	return 0;
}

static const struct AquamotoHookInstaller {
	AquamotoHookInstaller() {
		g_aquamotoShowBenchs = &aquamotoShowBenchs;
		g_aquamotoHasWindow = &aquamotoHasWindow;
		g_aquamotoReopen    = &aquamotoReopen;
		g_aquamotoIsVisible = &aquamotoIsVisible;
		g_aquamotoSetVisible = &aquamotoSetVisible;
		g_aquamotoDestroy   = &aquamotoDestroy;
		g_aquamotoSetCmap   = &aquamotoSetCmap;
	}
} g_aquamotoHookInstaller;
