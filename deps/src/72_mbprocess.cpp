// 72_mbprocess.cpp -- Geophysics > MB-System > "Process swath data (mbprocess)".
//
// A dialog over MB-System's own mbset and mbprocess, run IN THIS PROCESS by the host through GMT
// (the MB-System GMT supplement): src/mbprocess.jl. mbprocess is driven by each swath file's
// parameter file (file.par), which mbset writes and mbedit / mbeditviz / CUBE already update. The
// dialog reads that file to show what it says, writes the user's changes into it with mbset, and
// runs mbprocess file by file so the log and the busy notice can say which one is being worked on.
// The .par is the one source of truth: the dialog keeps no settings of its own.
//
// Every request goes through ONE door, `ask`: a newline-separated "key=value" block in, text out.
// The callback returns the reply's full length (negative = failure); a reply longer than the buffer
// is fetched again whole with "what=again" (the two-phase protocol every host payload uses).

typedef int (*JuliaMbProcessFn)(void *scene, const char *params, char *out, int cap);
static JuliaMbProcessFn g_juliaMbProcess = nullptr;

struct MbProcessDialog {
	struct File {
		QString path;
		int format = 0;
	};

	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QLineEdit *input = nullptr, *format = nullptr, *esf = nullptr;
	QToolButton *btnInput = nullptr, *btnEsf = nullptr;
	QCheckBox *force = nullptr, *strip = nullptr, *verbose = nullptr, *edits = nullptr;
	QListWidget *files = nullptr;
	QLabel *parState = nullptr;
	QPlainTextEdit *log = nullptr;
	QPushButton *savePar = nullptr, *process = nullptr, *close = nullptr;
	std::vector<File> list;
	QString listedFor;                       // the input + format the list was made from
	bool parDirty = false;                   // the user changed the Edits tab since the .par was read
	bool filling = false;                    // the widgets are being filled from a .par: not a change
	// THE OTHER TABS ARE DATA, NOT CODE. Every widget of the .ui with a "parKey" property is one
	// mbset key (NAVMODE, TIDEFILE, ...): a check box writes 0/1, an edit box its text, a combo box
	// its index -- or the index-th entry of its "parValues" list when the values do not start at 0.
	// "parDefault" is what it shows when the .par does not have the key. So a tab can be rearranged,
	// or a key added, in Designer alone.
	std::vector<QWidget *> parWidgets;
	QSet<QString> changed;                   // the keys the user changed since the .par was read
	bool parked = false;
	bool reallyClose = false;                // the parked row's Delete: the one close that is not a park

	// PARKED LIKE EVERY TOOL WINDOW: close, minimise and Esc hide it into the Scene Objects of the
	// window it was opened from (the shared parkTool / parkOnMinimise, 50_scene.cpp), and the row
	// brings it back with its input, its parameters and its log intact.
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

	// Every way out (X, Minimise, Esc) lands here; idempotent so parking twice leaves one row. False
	// when there is no window to park in (opened from the REPL): the close then really closes.
	bool parkNow() {
		if (reallyClose || !dlg || !scn || !sceneAlive(scn)) return false;
		if (parked) {
			dlg->hide();
			return true;
		}
		parked = true;
		dlg->hide();
		parkTool(scn, dlg, dlg->windowTitle(), IC_Rect,
		         "Closed " + dlg->windowTitle() + " — double-click to bring it back, click for Show / Delete",
		         [this]() { unpark(); }, parkedMenu());
		return true;
	}

	static QString parKey(QWidget *w) {
		return w->property("parKey").toString();
	}

	static QString parValue(QWidget *w) {
		if (auto *c = qobject_cast<QCheckBox *>(w)) return c->isChecked() ? "1" : "0";
		if (auto *e = qobject_cast<QLineEdit *>(w)) return e->text().trimmed();
		if (auto *b = qobject_cast<QComboBox *>(w)) {
			const QStringList v = b->property("parValues").toString().split(',', Qt::SkipEmptyParts);
			const int i = b->currentIndex();
			return (i >= 0 && i < v.size()) ? v[i].trimmed() : QString::number(i);
		}
		return QString();
	}

	static void parShow(QWidget *w, const QString &val) {
		if (auto *c = qobject_cast<QCheckBox *>(w)) {
			c->setChecked(val.toDouble() != 0.0);
			return;
		}
		if (auto *e = qobject_cast<QLineEdit *>(w)) {
			e->setText(val);
			return;
		}
		if (auto *b = qobject_cast<QComboBox *>(w)) {
			const QStringList v = b->property("parValues").toString().split(',', Qt::SkipEmptyParts);
			int i = v.isEmpty() ? val.toInt() : (int)v.indexOf(val.trimmed());
			b->setCurrentIndex((i >= 0 && i < b->count()) ? i : 0);
		}
	}

	QString ask(const QString &kv, bool *ok) {
		*ok = false;
		if (!g_juliaMbProcess) return QStringLiteral("mbprocess: the Julia side is not wired.");
		std::vector<char> buf(1 << 16);
		buf[0] = '\0';
		int r = g_juliaMbProcess(scn, kv.toUtf8().constData(), buf.data(), (int)buf.size());
		int n = r < 0 ? -r : r;
		if (n >= (int)buf.size()) {
			buf.assign((size_t)n + 1, '\0');
			r = g_juliaMbProcess(scn, "what=again\n", buf.data(), (int)buf.size());
			n = r < 0 ? -r : r;
		}
		*ok = r >= 0;
		return QString::fromUtf8(buf.data(), std::min(n, (int)buf.size() - 1));
	}

	void say(const QString &txt) {
		if (txt.isEmpty()) return;
		log->appendPlainText(txt.endsWith('\n') ? txt.left(txt.size() - 1) : txt);
		log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
	}

	QString inputKey() const {
		return input->text().trimmed() + "\n" + format->text().trimmed();
	}

	// The files the input stands for: itself, or every file of the datalist (MBIO reads it).
	bool listFiles() {
		const QString in = input->text().trimmed();
		list.clear();
		files->clear();
		listedFor = inputKey();
		if (in.isEmpty()) {
			showPar();
			return false;
		}
		bool ok = false;
		const QString ans = ask("what=files\ninput=" + in + "\nformat=" + format->text().trimmed() + "\n", &ok);
		if (!ok) {
			say(ans);
			showPar();
			return false;
		}
		for (const QString &line : ans.split('\n', Qt::SkipEmptyParts)) {
			File f;
			f.path = line.section('\t', 0, 0);
			f.format = line.section('\t', 1, 1).toInt();
			list.push_back(f);
			files->addItem(QString("%1   (format %2)").arg(QDir::toNativeSeparators(f.path)).arg(f.format));
		}
		if (list.empty()) say("No swath file found in " + in + ".");
		files->setCurrentRow(list.empty() ? -1 : 0);
		showPar();
		return !list.empty();
	}

	// The selected file's parameters, as its .par says (or what mbprocess would infer without one).
	// Changes not yet written are never overwritten by it: they stay on screen until Save .par /
	// Process writes them, into every file of the list.
	void showPar() {
		const int row = files->currentRow();
		const bool single = list.size() == 1;
		esf->setEnabled(single);
		btnEsf->setEnabled(single);
		if (row < 0 || row >= (int)list.size()) {
			parState->setText(QString());
			return;
		}
		if (anyChange()) {
			parState->setText("Changed parameters, not written yet: Save .par or Process writes them to every file.");
			return;
		}
		bool ok = false;
		const QString ans = ask("what=par\nfile=" + list[row].path + "\n", &ok);
		if (!ok) {
			parState->setText(ans.trimmed());
			return;
		}
		QMap<QString, QString> kv;
		for (const QString &line : ans.split('\n', Qt::SkipEmptyParts))
			kv[line.section('=', 0, 0)] = line.section('=', 1);
		filling = true;
		edits->setChecked(kv.value("editmode") == "1");
		esf->setText(QDir::toNativeSeparators(kv.value("editfile")));
		for (QWidget *w : parWidgets) {
			const QString k = "par." + parKey(w);
			parShow(w, kv.contains(k) ? kv.value(k) : w->property("parDefault").toString());
		}
		filling = false;
		parDirty = false;
		changed.clear();
		const QString par = QFileInfo(list[row].path + ".par").fileName();
		parState->setText(kv.value("exists") == "1" ? "From " + par
		                  : "No " + par + " yet: it is made (mbset -L) when the file is processed.");
	}

	bool anyChange() const {
		return parDirty || !changed.isEmpty();
	}

	// mbset on one file: the parameters the user changed, else only a missing .par made. A key the
	// user did not touch is not sent, so it keeps whatever each file's own .par says.
	bool setPar(const File &f) {
		QString kv = "what=setpar\nfile=" + f.path + QString("\nformat=%1\n").arg(f.format);
		kv += QString("ifmissing=%1\n").arg(anyChange() ? 0 : 1);
		if (parDirty) {
			kv += QString("editmode=%1\n").arg(edits->isChecked() ? 1 : 0);
			if (list.size() == 1) kv += "editfile=" + esf->text().trimmed() + "\n";
		}
		for (QWidget *w : parWidgets)
			if (changed.contains(parKey(w))) kv += "set." + parKey(w) + "=" + parValue(w) + "\n";
		bool ok = false;
		say(ask(kv, &ok));
		return ok;
	}

	// The two action buttons. `run` = also process.
	void apply(bool run) {
		if (listedFor != inputKey() || list.empty()) {
			if (!listFiles()) return;
		}
		const int n = (int)list.size();
		showBusyDialog(run ? "Running mbprocess..." : "Writing the parameter files (mbset)...");
		int nok = 0;
		for (int k = 0; k < n; k++) {
			const File &f = list[k];
			const QString name = QFileInfo(f.path).fileName();
			if (g_progress) {
				g_progress->setLabelText(QString("%1 %2 of %3: %4").arg(run ? "Processing" : "Writing the .par of")
				                         .arg(k + 1).arg(n).arg(name));
				QApplication::processEvents();
			}
			say(QString("== %1 (%2/%3)").arg(QDir::toNativeSeparators(f.path)).arg(k + 1).arg(n));
			if (!setPar(f)) continue;
			if (!run) {
				nok++;
				continue;
			}
			QString kv = "what=process\nfile=" + f.path + QString("\nformat=%1\n").arg(f.format);
			kv += QString("force=%1\nstrip=%2\nverbose=%3\n").arg(force->isChecked() ? 1 : 0)
			      .arg(strip->isChecked() ? 1 : 0).arg(verbose->isChecked() ? 1 : 0);
			bool ok = false;
			say(ask(kv, &ok));
			if (ok) nok++;
		}
		closeBusyDialog();
		say(QString("== %1: %2 of %3 file(s) done.").arg(run ? "mbprocess" : "mbset").arg(nok).arg(n));
		parDirty = false;
		changed.clear();
		showPar();
	}
};

static MbProcessDialog *g_mbProcess = nullptr;

// Open (or raise) the dialog, with `file` as its input when one is given.
static bool mbprocessOpen(QWidget *parent, Scene *scene, const QString &file) {
	if (g_mbProcess) {
		MbProcessDialog *m = g_mbProcess;
		m->unpark();                         // parked or not: ONE way back in (drops its row, if any)
		m->scn = scene;                      // it parks in the window it was last opened from
		if (!file.isEmpty()) {
			m->input->setText(QDir::toNativeSeparators(file));
			m->listFiles();
		}
		return true;
	}
	QFile uif(QDir(gmtvtkUiDir()).filePath("mbprocess.ui"));
	QUiLoader loader;
	QDialog *dlg = uif.open(QIODevice::ReadOnly) ? qobject_cast<QDialog *>(loader.load(&uif, nullptr)) : nullptr;
	uif.close();
	if (!dlg) {
		QMessageBox::warning(parent, "mbprocess", "Cannot load " + uif.fileName());
		return false;
	}
	auto *m = new MbProcessDialog;
	m->dlg = dlg;
	m->scn = scene;
	m->input    = dlg->findChild<QLineEdit *>("editInput");
	m->format   = dlg->findChild<QLineEdit *>("editFormat");
	m->esf      = dlg->findChild<QLineEdit *>("editEsf");
	m->btnInput = dlg->findChild<QToolButton *>("btnInput");
	m->btnEsf   = dlg->findChild<QToolButton *>("btnEsf");
	m->force    = dlg->findChild<QCheckBox *>("chkForce");
	m->strip    = dlg->findChild<QCheckBox *>("chkStrip");
	m->verbose  = dlg->findChild<QCheckBox *>("chkVerbose");
	m->edits    = dlg->findChild<QCheckBox *>("chkEdits");
	m->files    = dlg->findChild<QListWidget *>("listFiles");
	m->parState = dlg->findChild<QLabel *>("lblParState");
	m->log      = dlg->findChild<QPlainTextEdit *>("textLog");
	m->savePar  = dlg->findChild<QPushButton *>("btnSavePar");
	m->process  = dlg->findChild<QPushButton *>("btnProcess");
	m->close    = dlg->findChild<QPushButton *>("btnClose");
	if (!(m->input && m->format && m->esf && m->btnInput && m->btnEsf && m->force && m->strip && m->verbose &&
	      m->edits && m->files && m->parState && m->log && m->savePar && m->process && m->close)) {
		QMessageBox::warning(parent, "mbprocess",
		                     "mbprocess.ui does not carry this tool's controls (a renamed or removed widget).");
		delete dlg;
		delete m;
		return false;
	}
	g_mbProcess = m;
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
	// NOT WA_DeleteOnClose: a close parks, and the parked dialog must survive to come back. Only the
	// row's Delete (reallyClose), the owning window going away, or a close with nowhere to park
	// (opened from the REPL) destroy it.
	struct CloseParks : QObject {
		MbProcessDialog *mp;
		CloseParks(QObject *p, MbProcessDialog *m) : QObject(p), mp(m) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close) {
				if (mp->parkNow()) {
					e->ignore();
					return true;
				}
				mp->dlg->deleteLater();
			}
			return QObject::eventFilter(o, e);
		}
	};
	dlg->installEventFilter(new CloseParks(dlg, m));
	parkOnMinimise(dlg, [m]() { m->parkNow(); });    // the shared handler (50_scene.cpp)
	QObject::connect(dlg, &QDialog::rejected, dlg, [m]() {   // Esc
		if (!m->parkNow()) m->dlg->deleteLater();
	});
	{
		QFont mono("Consolas");
		mono.setStyleHint(QFont::Monospace);
		m->log->setFont(mono);
	}
	// Enter in an edit box must never press an action button.
	for (QPushButton *b : {m->savePar, m->process, m->close}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
	QObject::connect(dlg, &QObject::destroyed, [m]() {
		unparkTool(m->scn, m->dlg);          // never leave a row pointing at a dead window (no-op if none)
		if (g_mbProcess == m) g_mbProcess = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	QObject::connect(m->btnInput, &QToolButton::clicked, dlg, [m]() {
		const QString p = QFileDialog::getOpenFileName(m->dlg, "Select swath data or datalist", prefStartDir(),
			"MB-System swath data (*.mb-1 *.mb* *.all *.kmall *.s7k *.gsf);;All files (*)");
		if (p.isEmpty()) return;
		rememberStartDir(p);
		m->input->setText(QDir::toNativeSeparators(p));
		m->listFiles();
	});
	QObject::connect(m->btnEsf, &QToolButton::clicked, dlg, [m]() {
		const QString p = QFileDialog::getOpenFileName(m->dlg, "Select the edit save file", prefStartDir(),
			"Edit save files (*.esf);;All files (*)");
		if (p.isEmpty()) return;
		rememberStartDir(p);
		m->esf->setText(QDir::toNativeSeparators(p));
		m->parDirty = true;
	});
	QObject::connect(m->files, &QListWidget::currentRowChanged, dlg, [m](int) { m->showPar(); });
	QObject::connect(m->edits, &QCheckBox::toggled, dlg, [m](bool) {
		if (!m->filling) m->parDirty = true;
	});
	QObject::connect(m->esf, &QLineEdit::textEdited, dlg, [m](const QString &) { m->parDirty = true; });
	// The keyed widgets of the other tabs: a change by the user marks its key (local state only).
	for (QWidget *w : dlg->findChildren<QWidget *>()) {
		const QString key = MbProcessDialog::parKey(w);
		if (key.isEmpty()) continue;
		m->parWidgets.push_back(w);
		auto mark = [m, key]() {
			if (!m->filling) m->changed.insert(key);
		};
		if (auto *c = qobject_cast<QCheckBox *>(w))
			QObject::connect(c, &QCheckBox::toggled, dlg, [mark](bool) { mark(); });
		else if (auto *e = qobject_cast<QLineEdit *>(w))
			QObject::connect(e, &QLineEdit::textEdited, dlg, [mark](const QString &) { mark(); });
		else if (auto *b = qobject_cast<QComboBox *>(w))
			QObject::connect(b, &QComboBox::activated, dlg, [mark](int) { mark(); });
	}
	// "..." buttons: a tool button's "browseFor" names the edit box its file goes into.
	for (QToolButton *t : dlg->findChildren<QToolButton *>()) {
		const QString forName = t->property("browseFor").toString();
		if (forName.isEmpty()) continue;                 // (an empty name would match ANY edit box)
		auto *target = dlg->findChild<QLineEdit *>(forName);
		if (!target) continue;
		QObject::connect(t, &QToolButton::clicked, dlg, [m, target]() {
			const QString p = QFileDialog::getOpenFileName(m->dlg, "Select the file", prefStartDir(), "All files (*)");
			if (p.isEmpty()) return;
			rememberStartDir(p);
			target->setText(QDir::toNativeSeparators(p));
			const QString key = MbProcessDialog::parKey(target);
			if (!key.isEmpty()) m->changed.insert(key);
		});
	}
	QObject::connect(m->savePar, &QPushButton::clicked, dlg, [m]() { m->apply(false); });
	QObject::connect(m->process, &QPushButton::clicked, dlg, [m]() { m->apply(true); });
	QObject::connect(m->close, &QPushButton::clicked, dlg, [dlg]() { dlg->close(); });

	wrapTooltips(dlg);
	if (!file.isEmpty()) {
		m->input->setText(QDir::toNativeSeparators(file));
		m->listFiles();
	}
	else {
		m->showPar();
	}
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
	return true;
}
