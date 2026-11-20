// ============================================================================================
// Grid Tools > Drainage analysis… — catchments, rivers and flow products of the DEM on display
// (deps/ui/hydrology_dialog.ui, Julia side src/hydrology.jl over GMT.jl's hydro functions).
//
// Run hands the ticked results to `_hydro_run`, which starts the work on a spawned Julia thread and
// returns at once. The dialog's OWN bar then follows it: a QTimer asks `_hydro_poll` every 200 ms for
// "state|percent|text" (no global progress dialog: the bar lives here, beside the Stop button that
// can end the run). When the work is done ("adopt") `_hydro_finish` adds the results to the window
// on the main thread, through the same doors a dropped file and every derived grid go through.
//
// ONE dialog per window: the menu raises the existing one. Every Julia call goes through ask(), the
// dialog's only door, guarded against re-entry (a nested eval while the window pumps would run a
// second poll inside the first).
// ============================================================================================
class HydroDialog;
static std::map<Scene *, HydroDialog *> g_hydroDlgs;

class HydroDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QRadioButton *radioCarve = nullptr;
	QCheckBox *chkSea = nullptr, *chkBasins = nullptr, *chkStreams = nullptr, *chkOrder = nullptr;
	QCheckBox *chkAcc = nullptr, *chkTwi = nullptr, *chkDist = nullptr, *chkLdd = nullptr;
	QLineEdit *edBasinArea = nullptr, *edStreamArea = nullptr;
	QComboBox *cbAccMethod = nullptr, *cbTwiMethod = nullptr, *cbUnit = nullptr;
	QProgressBar *bar = nullptr;
	QLabel *lblStage = nullptr;
	QPushButton *btnRun = nullptr, *btnStop = nullptr;
	QTimer *timer = nullptr;
	QTimer *gridWatch = nullptr;
	bool asking = false;
	bool running = false;
	bool blocked = false;

	explicit HydroDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/hydrology_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("HydroDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) {
			qWarning("HydroDialog: QUiLoader failed to load the .ui");
			return;
		}
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("Drainage analysis");
		QDialog *d = dlg;

		radioCarve   = d->findChild<QRadioButton *>("radioCarve");
		chkSea       = d->findChild<QCheckBox *>("chkSea");
		chkBasins    = d->findChild<QCheckBox *>("chkBasins");
		chkStreams   = d->findChild<QCheckBox *>("chkStreams");
		chkOrder     = d->findChild<QCheckBox *>("chkOrder");
		chkAcc       = d->findChild<QCheckBox *>("chkAcc");
		chkTwi       = d->findChild<QCheckBox *>("chkTwi");
		chkDist      = d->findChild<QCheckBox *>("chkDist");
		chkLdd       = d->findChild<QCheckBox *>("chkLdd");
		edBasinArea  = d->findChild<QLineEdit *>("edBasinArea");
		edStreamArea = d->findChild<QLineEdit *>("edStreamArea");
		cbAccMethod  = d->findChild<QComboBox *>("cbAccMethod");
		cbTwiMethod  = d->findChild<QComboBox *>("cbTwiMethod");
		cbUnit       = d->findChild<QComboBox *>("cbUnit");
		bar          = d->findChild<QProgressBar *>("progress");
		lblStage     = d->findChild<QLabel *>("lblStage");
		btnRun       = d->findChild<QPushButton *>("btnRun");
		btnStop      = d->findChild<QPushButton *>("btnStop");

		// Item text shown, item DATA sent to Julia (the method names GMT.jl's flowacc/twi take).
		if (cbAccMethod) {
			cbAccMethod->addItem("D8", "d8");
			cbAccMethod->addItem("MFD", "mfd");
			cbAccMethod->addItem("D\xE2\x88\x9E", "dinf");
		}
		if (cbTwiMethod) {
			cbTwiMethod->addItem("MFD", "mfd");
			cbTwiMethod->addItem("D\xE2\x88\x9E", "dinf");
			cbTwiMethod->addItem("D8", "d8");
		}
		if (cbUnit) {
			cbUnit->addItem("cells", "cells");
			cbUnit->addItem("km\xC2\xB2", "km2");
		}
		if (bar) bar->setRange(0, 100);
		// The help: the green ? disk, opening this tool's HELP page of the iGMT manual (its `page`
		// property in the .ui, docs/src/83-drainage.md) in the browser — as the "tutorial" button of
		// the generic tool dialogs does (70_window.cpp).
		if (auto *tb = d->findChild<QToolButton *>("tutorial")) {
			const QString page = tb->property("page").toString();
			tb->setIcon(makeHelpDiskIcon(18));
			tb->setIconSize(QSize(18, 18));
			tb->setAutoRaise(true);
			tb->setText(QString());
			tb->setCursor(Qt::PointingHandCursor);
			tb->setFocusPolicy(Qt::NoFocus);
			tb->setToolTip("Open the help page: every control of this dialog explained.");
			QObject::connect(tb, &QToolButton::clicked, d, [d, page]() {
				const QUrl u("https://www.generic-mapping-tools.org/InteractiveGMT/dev/" + page + "/");
				if (!QDesktopServices::openUrl(u))
					QMessageBox::warning(d, d->windowTitle(), QString("Could not open a browser for\n\n%1").arg(u.toString()));
			});
		}
		// The tutorial: the "Tutorial" button beside the ?, opening the worked example
		// (docs/src/83b-drainage-tutorial.md) — the link_ button of the generic tool dialogs.
		if (auto *lb = d->findChild<QPushButton *>("link_tutorial")) {
			const QString page = lb->property("page").toString();
			lb->setCursor(Qt::PointingHandCursor);
			lb->setFocusPolicy(Qt::NoFocus);
			QObject::connect(lb, &QPushButton::clicked, d, [d, page]() {
				const QUrl u("https://www.generic-mapping-tools.org/InteractiveGMT/dev/" + page + "/");
				if (!QDesktopServices::openUrl(u))
					QMessageBox::warning(d, d->windowTitle(), QString("Could not open a browser for\n\n%1").arg(u.toString()));
			});
		}
		// No Enter-accepts-the-dialog: Run is an explicit click (NswingDialog lesson).
		for (QPushButton *b : d->findChildren<QPushButton *>()) {
			b->setAutoDefault(false);
			b->setDefault(false);
		}
		wrapTooltips(d);

		// No grid in the window: the dialog still opens (its tutorial must be reachable), with every
		// other control blocked. A light 1 s watch unblocks it as soon as a grid is loaded.
		gridWatch = new QTimer(d);
		gridWatch->setInterval(1000);
		QObject::connect(gridWatch, &QTimer::timeout, d, [this]() { checkGrid(); });

		timer = new QTimer(d);
		timer->setInterval(200);
		QObject::connect(timer, &QTimer::timeout, d, [this]() { poll(); });
		if (btnRun)   QObject::connect(btnRun,   &QPushButton::clicked, d, [this]() { run(); });
		if (btnStop)  QObject::connect(btnStop,  &QPushButton::clicked, d, [this]() { stop(); });
		// Closing the dialog while a run goes on stops that run: nobody is left to collect its results.
		QObject::connect(d, &QObject::destroyed, [this]() {
			if (running && sceneAlive(scn) && g_juliaEval) {
				QString r;
				ask(QString("InteractiveGMT._hydro_stop(%1)").arg(scenePtr()), r);
			}
			g_hydroDlgs.erase(scn);
			delete this;
		});
	}

	QString scenePtr() const {
		return QString("Ptr{Cvoid}(UInt(%1))").arg((qulonglong)reinterpret_cast<uintptr_t>(scn));
	}

	// The dialog's ONE door to Julia. Returns false (with the message in `out`) on a Julia error.
	bool ask(const QString &cmd, QString &out) {
		out.clear();
		if (asking || !g_juliaEval) return false;
		asking = true;
		static std::vector<char> buf(1 << 14);
		const int n = g_juliaEval(scn, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		asking = false;
		if (n < 0) {
			out = QString::fromUtf8(buf.data(), -n);
			return false;
		}
		out = QString::fromUtf8(buf.data(), n).trimmed();
		// The eval shows a returned String the way the REPL does: quoted, with \" \$ \\ \n escaped.
		if (out.size() >= 2 && out.startsWith('"') && out.endsWith('"')) {
			const QString s = out.mid(1, out.size() - 2);
			QString u;
			u.reserve(s.size());
			for (int i = 0; i < s.size(); i++) {
				if (s[i] == '\\' && i + 1 < s.size()) {
					const QChar c = s[++i];
					u += (c == 'n') ? QChar('\n') : (c == 't') ? QChar('\t') : c;
				}
				else
					u += s[i];
			}
			out = u;
		}
		return true;
	}

	// Block (or release) everything but the tutorial button: the two option groups and Run.
	void setBlocked(bool on) {
		blocked = on;
		for (const char *name : {"gbDep", "gbRes"})
			if (auto *w = dlg->findChild<QWidget *>(name)) w->setEnabled(!on);
		if (btnRun) btnRun->setEnabled(!on && !running);
		if (lblStage) lblStage->setText(on ? QString("Load a grid (a DEM) into this window first.") : QString());
		if (on)
			gridWatch->start();
		else
			gridWatch->stop();
	}

	void checkGrid() {
		const bool has = sceneAlive(scn) && sceneHasGrid(scn);
		if (has == !blocked) return;               // no change
		setBlocked(!has);
	}

	void setRunning(bool on) {
		running = on;
		if (btnRun)  btnRun->setEnabled(!on);
		if (btnStop) btnStop->setEnabled(on);
	}

	static QString flag(QCheckBox *c) { return (c && c->isChecked()) ? "1" : "0"; }
	static QString data(QComboBox *c, const char *def) { return c ? c->currentData().toString() : QString(def); }

	void run() {
		if (running || blocked || !sceneAlive(scn)) return;
		if (!g_juliaEval) {
			QMessageBox::warning(dlg, "Drainage analysis", "This computation needs the Julia/GMT host.");
			return;
		}
		const QString kv = QString("carve=%1;sea=%2;basins=%3;basin_area=%4;streams=%5;stream_area=%6;"
		                           "order=%7;acc=%8;acc_method=%9;twi=%10;twi_method=%11;dist=%12;ldd=%13;unit=%14")
			.arg((radioCarve && radioCarve->isChecked()) ? "1" : "0").arg(flag(chkSea))
			.arg(flag(chkBasins)).arg(edBasinArea ? edBasinArea->text().trimmed() : QString("1000"))
			.arg(flag(chkStreams)).arg(edStreamArea ? edStreamArea->text().trimmed() : QString("1000"))
			.arg(flag(chkOrder)).arg(flag(chkAcc)).arg(data(cbAccMethod, "d8"))
			.arg(flag(chkTwi)).arg(data(cbTwiMethod, "mfd"))
			.arg(flag(chkDist)).arg(flag(chkLdd)).arg(data(cbUnit, "cells"));
		QString reply;
		const bool ok = ask(QString("InteractiveGMT._hydro_run(%1,\"%2\")").arg(scenePtr()).arg(kv), reply);
		if (!ok || !reply.isEmpty()) {
			QMessageBox::warning(dlg, "Drainage analysis", reply.isEmpty() ? QString("Could not start the run.") : reply);
			return;
		}
		if (bar) bar->setValue(0);
		if (lblStage) lblStage->setText("Starting\xE2\x80\xA6");
		setRunning(true);
		timer->start();
	}

	void stop() {
		if (!running) return;
		QString r;
		ask(QString("InteractiveGMT._hydro_stop(%1)").arg(scenePtr()), r);
		if (lblStage) lblStage->setText("Stopping\xE2\x80\xA6");
	}

	void poll() {
		if (asking) return;                       // a tick while a call is still out: try again next tick
		if (!sceneAlive(scn)) {
			timer->stop();
			setRunning(false);
			return;
		}
		QString reply;
		if (!ask(QString("InteractiveGMT._hydro_poll(%1)").arg(scenePtr()), reply)) {
			finish(QString("Error: ") + reply, true);
			return;
		}
		const QStringList p = reply.split('|');
		const QString state = p.value(0);
		const int pct = p.value(1).toInt();
		const QString text = p.mid(2).join('|');
		if (state == "run") {
			if (bar) bar->setValue(pct);
			if (lblStage) lblStage->setText(text);
			return;
		}
		if (state == "adopt") {
			// The results go into the window now: a second or two on the main thread for big grids.
			timer->stop();
			if (bar) bar->setValue(100);
			if (lblStage) lblStage->setText(text);
			QApplication::setOverrideCursor(Qt::WaitCursor);
			QApplication::processEvents();
			QString done;
			const bool ok = ask(QString("InteractiveGMT._hydro_finish(%1)").arg(scenePtr()), done);
			QApplication::restoreOverrideCursor();
			const QStringList q = done.split('|');
			if (!ok || q.value(0) == "fail")
				finish(QString("Failed: ") + (ok ? q.mid(2).join('|') : done), true);
			else
				finish(q.mid(2).join('|'), false);
			return;
		}
		if (state == "cancel") {
			if (bar) bar->setValue(0);
			finish(text, false);
			return;
		}
		if (state == "fail") {
			finish(QString("Failed: ") + text, true);
			return;
		}
		finish(QString(), false);                 // "none": the job is gone
	}

	void finish(const QString &msg, bool error) {
		timer->stop();
		setRunning(false);
		if (lblStage) lblStage->setText(msg);
		if (error) QMessageBox::warning(dlg, "Drainage analysis", msg);
	}
};

static bool hydroOpen(QWidget *parent, Scene *scene) {
	auto it = g_hydroDlgs.find(scene);
	if (it != g_hydroDlgs.end() && it->second && it->second->dlg) {
		it->second->checkGrid();
		it->second->dlg->show();
		it->second->dlg->raise();
		it->second->dlg->activateWindow();
		return true;
	}
	auto *w = new HydroDialog(parent, scene);
	if (!w->dlg) {
		delete w;
		return false;
	}
	g_hydroDlgs[scene] = w;
	w->setBlocked(!sceneHasGrid(scene));
	w->dlg->show();
	w->dlg->raise();
	w->dlg->activateWindow();
	return true;
}
