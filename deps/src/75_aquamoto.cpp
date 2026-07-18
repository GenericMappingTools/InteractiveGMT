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
// Show mesh (ANUGA triangulated-mesh display) and the Primary-quantities radios/derived-var group
// stay disabled/unwired: the .ui itself ships Show-mesh disabled, and Mirone's own set_common()
// disables the quantity radios for this exact (COARDS/NSWING) file class — this is a faithful
// port of that gating, not a scope cut.
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

// The acting window's Scene* as a Julia pointer literal, the same "Ptr{Cvoid}(UInt(...))" spelling
// NswingDialog's own calls use.
static QString aquaScenePtr(Scene *scene) {
	return QString("Ptr{Cvoid}(UInt(%1))").arg((quintptr)scene);
}

// The Aquamoto window is never DESTROYED by its X -- clicking the title-bar X HIDES it (state kept),
// and its "Aquamoto viewer" handle in Scene Objects unticks to reflect that; re-ticking the handle
// brings it back. The QCloseEvent is swallowed (no WA_DeleteOnClose), the window hidden, and the
// scene's object panel refreshed so the handle checkbox mirrors the new hidden state.
class AquamotoHideOnClose : public QObject {
public:
	Scene *scene_;
	AquamotoHideOnClose(QObject *parent, Scene *scene) : QObject(parent), scene_(scene) {}
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (ev->type() == QEvent::Close) {
			ev->ignore();
			if (auto *w = qobject_cast<QWidget *>(obj)) w->hide();   // hidden, NOT destroyed
			if (scene_) rebuildSceneObjects(scene_);                // untick the handle checkbox
			return true;
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
	}
	QLineEdit *pathEdit = nullptr;
	QLabel *timeStepsLabel = nullptr, *waterTransparencyLabel = nullptr;
	QScrollBar *sliceSlider = nullptr;    // QScrollBar, NOT QSlider -- arrow buttons at each tip (Mirone-style)
	QSlider *waterTransparencySlider = nullptr;
	QLineEdit *sliceSpin = nullptr;       // a PLAIN edit box (replaces the .ui's QSpinBox at runtime --
	                                      // see the constructor), not a spinner: user wants a simple box
	QCheckBox *splitDryWetCheck = nullptr, *scaleGlobalCheck = nullptr;
	QPushButton *showSliceBtn = nullptr, *runInBtn = nullptr;
	QRadioButton *shadeWaterBtn = nullptr, *shadeLandBtn = nullptr;   // split which side's colour scale is shown
	bool opened_ = false;                 // a file has been successfully opened this session

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
		showSliceBtn            = w->findChild<QPushButton *>("showSliceButton");
		runInBtn                = w->findChild<QPushButton *>("plotRunInButton");
		shadeWaterBtn           = w->findChild<QRadioButton *>("shadeWaterButton");
		shadeLandBtn            = w->findChild<QRadioButton *>("shadeLandButton");
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

		// Show mesh (ANUGA-only) never gets wired -- ships disabled in the .ui and stays that way.
		// Primary quantities (Stage/Xmoment/Ymoment/Or…) + Derived var: Mirone's own set_common()
		// disables these for this exact (COARDS/NSWING) file class -- do the same here at runtime,
		// matching the .ui's own derivedVarCheckBox default rather than inventing new scope.
		for (const char *nm : {"stageRadioButton", "xmomentRadioButton", "ymomentRadioButton", "orComboBox"})
			if (QWidget *cw = w->findChild<QWidget *>(nm)) cw->setEnabled(false);

		// sliceNSpinBox is a plain, EDITABLE QLineEdit in aquamoto.ui (not a QSpinBox). setFixedWidth
		// (not maximumWidth) locks BOTH min and max to the same value -- the layout literally cannot
		// stretch it, no matter what policy or row space is in play.
		if (sliceSpin) {
			sliceSpin->setValidator(new QIntValidator(1, 1, sliceSpin));   // real range set in openPath()
			sliceSpin->setReadOnly(false);
			sliceSpin->setFixedWidth(50);   // exactly enough for a 5-digit number, hard-locked
		}

		if (!pathEdit || !sliceSlider || !sliceSpin || !splitDryWetCheck || !scaleGlobalCheck ||
		    !waterTransparencySlider || !showSliceBtn || !runInBtn) {
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
			if (pathEdit) pathEdit->setText(p);
			openPath(p);
		};
		if (browseBtn) QObject::connect(browseBtn, &QToolButton::clicked, w, openFile);
		if (pathEdit) {
			QObject::connect(pathEdit, &QLineEdit::returnPressed, w, [this]() {
				if (pathEdit && !pathEdit->text().trimmed().isEmpty()) openPath(pathEdit->text().trimmed());
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
		if (waterTransparencySlider) {
			QObject::connect(waterTransparencySlider, &QSlider::valueChanged, w, [this](int v) {
				if (waterTransparencyLabel) waterTransparencyLabel->setText(QString("Water transparency %1%").arg(v));
				fireSlice();
			});
		}
		if (showSliceBtn) QObject::connect(showSliceBtn, &QPushButton::clicked, w, [this]() { fireSlice(); });
		if (runInBtn) QObject::connect(runInBtn, &QPushButton::clicked, w, [this]() { fireRunIn(); });

		// Restore a prior session on this SAME scene (the panel was closed and reopened, or opened a
		// 2nd time on a window that already had a file loaded) instead of starting blank -- Julia
		// still has the file/bathymetry/step-count cached even though this is a brand-new panel.
		QString state;
		if (aquaEval(scene_, QString("InteractiveGMT._aquamoto_state(%1)").arg(aquaScenePtr(scene_)), state) &&
		    !state.trimmed().isEmpty()) {
			const QStringList parts = state.trimmed().split('|');
			if (parts.size() == 2) {
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
					if (showSliceBtn) showSliceBtn->setEnabled(true);
					if (runInBtn) runInBtn->setEnabled(true);
					opened_ = true;
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
		bool ok = false;
		int n = out.trimmed().toInt(&ok);
		if (!ok || n < 1) n = 1;
		opened_ = true;
		if (timeStepsLabel) timeStepsLabel->setText(QString("Time steps = %1").arg(n));
		if (sliceSlider) { sliceSlider->setRange(1, n); sliceSlider->setValue(1); sliceSlider->setEnabled(true); }
		if (sliceSpin) {
			sliceSpin->setValidator(new QIntValidator(1, n, sliceSpin));
			sliceSpin->setText("1");
			sliceSpin->setEnabled(true);
		}
		if (showSliceBtn) showSliceBtn->setEnabled(true);
		if (runInBtn) runInBtn->setEnabled(true);
		fireSlice();
	}

	void fireSlice() {
		// REENTRANCY GUARD (why the window "could not be killed"): every widget signal here fires a
		// blocking runBlocking(), and aquaEval() pumps QApplication::processEvents() internally. Without
		// this guard a second signal delivered during that pump -- e.g. sliceSpin's editingFinished,
		// which fires the instant focus leaves the box on a Close click -- re-enters fireSlice and starts
		// a NESTED runBlocking. The nested call clears the single `busy_` flag on return, so the outer
		// call's close-defer logic (AquamotoCloseFilter -> closePending_ -> win->close()) desyncs and the
		// user's Close is silently dropped. Refuse to start any new work while one is already in flight.
		if (busy_) return;
		if (!opened_ || !sliceSlider) return;
		const int k = sliceSlider->value() - 1;              // 0-based for the Julia side
		const bool split = splitDryWetCheck && splitDryWetCheck->isChecked();
		const bool global = scaleGlobalCheck && scaleGlobalCheck->isChecked();
		const double transp = waterTransparencySlider ? waterTransparencySlider->value() / 100.0 : 0.0;
		const bool shadeWater = !shadeWaterBtn || shadeWaterBtn->isChecked();   // no button found -> behave as always-on
		const bool shadeLand  = !shadeLandBtn  || shadeLandBtn->isChecked();
		QString out;
		bool closedNow = false;
		const bool ok = runBlocking(QString("InteractiveGMT._aquamoto_slice(%1,%2,%3,%4,%5,%6,%7)")
		                            .arg(aquaScenePtr(scene_)).arg(k)
		                            .arg(split ? "true" : "false").arg(global ? "true" : "false")
		                            .arg(transp, 0, 'f', 4)
		                            .arg(shadeWater ? "true" : "false").arg(shadeLand ? "true" : "false"), out, closedNow);
		if (closedNow) return;   // `this` may already be destroyed -- touch NOTHING below
		if (!ok && win) win->statusBar()->showMessage("Aquamoto: " + out, 5000);
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
	if (on) { w->win->show(); w->win->raise(); w->win->activateWindow(); }
	else    { w->win->hide(); }
}
// Destroy the window for good -- called when its nc cube surface is removed (the window is otherwise
// un-killable). Deleting `win` bypasses the close filter (which only swallows QCloseEvent); its
// destroyed handler removes the registry entry and deletes the wrapper.
static void aquamotoDestroy(Scene *scene) {
	AquamotoWindow *w = AquamotoWindow::registry().value(scene, nullptr);
	if (w && w->win) w->win->deleteLater();
}
static const struct AquamotoHookInstaller {
	AquamotoHookInstaller() {
		g_aquamotoHasWindow = &aquamotoHasWindow;
		g_aquamotoReopen    = &aquamotoReopen;
		g_aquamotoIsVisible = &aquamotoIsVisible;
		g_aquamotoSetVisible = &aquamotoSetVisible;
		g_aquamotoDestroy   = &aquamotoDestroy;
	}
} g_aquamotoHookInstaller;
