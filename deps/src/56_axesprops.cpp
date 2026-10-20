// ============================================================================
//  Axes Properties dialog — ONE raster's own axes (its AxesSet), opened from the
//  "Axes" row of that raster's Scene Objects group.
//
//  The layout is deps/ui/axes_props.ui, loaded at run time. Its tabs follow the
//  groups of MATLAB's Axes Properties page; its fields are GMT's own gmt.conf
//  parameters (MAP_FRAME_*, MAP_TICK_*, MAP_ANNOT_*, MAP_GRID_*, FONT_*,
//  FORMAT_*_MAP) plus every -B frame setting (sides, +b +g +i +n +o +t +s +w +x +y +z,
//  Z corners) and axis setting (p/s a f g stride/phase/unit, +a +e +f +l +L +s +S +p +u)
//  and -R. What MATLAB has and GMT has not is not there. Images have no Z: widgets the
//  .ui tags "vertical" are greyed out for them.
//
//  THE FIELDS ARE DATA, NOT CODE. Every widget carrying a "gmtKey" dynamic property
//  is one field; "gmtPart" names the piece when one parameter takes several widgets.
//  The values live in AxesSet::props under "KEY" / "KEY.part" (Raster-own-axes law:
//  each raster's set holds its own), so a field added to the .ui needs no code here.
// ============================================================================

// GMT's 35 standard PostScript fonts, in GMT's own numbering (gmt text -L).
static const char *const kGmtFontNames[] = {
	"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
	"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
	"Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
	"Symbol",
	"AvantGarde-Book", "AvantGarde-BookOblique", "AvantGarde-Demi", "AvantGarde-DemiOblique",
	"Bookman-Demi", "Bookman-DemiItalic", "Bookman-Light", "Bookman-LightItalic",
	"Helvetica-Narrow", "Helvetica-Narrow-Bold", "Helvetica-Narrow-Oblique", "Helvetica-Narrow-BoldOblique",
	"NewCenturySchlbk-Roman", "NewCenturySchlbk-Italic", "NewCenturySchlbk-Bold", "NewCenturySchlbk-BoldItalic",
	"Palatino-Roman", "Palatino-Italic", "Palatino-Bold", "Palatino-BoldItalic",
	"ZapfChancery-MediumItalic", "ZapfDingbats"
};

// The storage key of one field: "KEY" or "KEY.part". Empty = not a field.
static QString axpKey(const QWidget *w) {
	const QString k = w->property("gmtKey").toString();
	if (k.isEmpty()) return k;
	const QString p = w->property("gmtPart").toString();
	return p.isEmpty() ? k : k + "." + p;
}

// A colour button shows its colour as its face; the colour itself is the "gmtColor" property.
static void axpPaintSwatch(QPushButton *b, const QString &c) {
	b->setProperty("gmtColor", c);
	QColor q(c);
	if (!q.isValid()) q = Qt::black;
	b->setStyleSheet(QString("QPushButton{background:%1;border:1px solid #808080;}").arg(q.name()));
}

// Read / write one field, whatever widget it is.
static QString axpGet(QWidget *w) {
	if (auto *e = qobject_cast<QLineEdit *>(w)) return e->text().trimmed();
	if (auto *c = qobject_cast<QComboBox *>(w)) return c->currentText();
	if (auto *c = qobject_cast<QCheckBox *>(w)) return c->isChecked() ? "1" : "0";
	if (auto *sp = qobject_cast<QSpinBox *>(w)) return QString::number(sp->value());
	if (auto *b = qobject_cast<QPushButton *>(w)) return b->property("gmtColor").toString();
	return QString();
}
static void axpSet(QWidget *w, const QString &v) {
	if (auto *e = qobject_cast<QLineEdit *>(w)) {
		e->setText(v);
	}
	else if (auto *c = qobject_cast<QComboBox *>(w)) {
		const int i = c->findText(v);
		if (i >= 0) c->setCurrentIndex(i);
		else if (c->isEditable()) c->setEditText(v);
	}
	else if (auto *c = qobject_cast<QCheckBox *>(w)) {
		c->setChecked(v == "1");
	}
	else if (auto *sp = qobject_cast<QSpinBox *>(w)) {
		sp->setValue(v.toInt());
	}
	else if (auto *b = qobject_cast<QPushButton *>(w)) {
		axpPaintSwatch(b, v);
	}
}

// ---------------------------------------------------------------------------------------------------
// FROM THE FIELDS TO THE AXES ON SCREEN. GMT speaks in points and PostScript font names; the viewer
// draws in pixels with VTK's three freetype families. These are the only conversions, both ways, so
// what the dialog shows is what the axes are drawn with.
// ---------------------------------------------------------------------------------------------------
static const double kAxpPxPerPt = 96.0 / 72.0;      // a screen point: 1p = 4/3 px at 96 dpi

// "1.5p" / "0.05c" / "0.02i" / "2" (bare = points) -> pixels. False when it is not a length ("auto").
static bool axpLengthPx(const QString &txt, double &px) {
	QString t = txt.trimmed();
	if (t.isEmpty()) return false;
	double f = 1.0;                                   // points
	const QChar u = t.back();
	if (u == 'p') { t.chop(1); }
	else if (u == 'c') { t.chop(1); f = 72.0 / 2.54; }
	else if (u == 'i') { t.chop(1); f = 72.0; }
	bool ok = false;
	const double v = t.toDouble(&ok);
	if (!ok || v < 0.0) return false;
	px = v * f * kAxpPxPerPt;
	return true;
}

// A GMT font name onto a VTK text property: family, bold, italic.
static void axpSetFace(vtkTextProperty *tp, const QString &gmtName) {
	if (!tp || gmtName.isEmpty()) return;
	if (gmtName.startsWith("Courier"))
		tp->SetFontFamilyToCourier();
	else if (gmtName.startsWith("Times") || gmtName.startsWith("NewCentury") || gmtName.startsWith("Palatino") ||
	         gmtName.startsWith("Bookman") || gmtName.startsWith("ZapfChancery"))
		tp->SetFontFamilyToTimes();
	else
		tp->SetFontFamilyToArial();
	tp->SetBold(gmtName.contains("Bold") || gmtName.contains("Demi") ? 1 : 0);
	tp->SetItalic(gmtName.contains("Oblique") || gmtName.contains("Italic") ? 1 : 0);
}

// One font onto a text property: "KEY.size" / "KEY.name" / "KEY.color" out of `v`.
static void axpApplyFont(vtkTextProperty *tp, const QMap<QString, QString> &v, const QString &key) {
	if (!tp) return;
	double px = 0.0;
	if (axpLengthPx(v.value(key + ".size"), px)) tp->SetFontSize(std::max(1, int(std::lround(px / kAxpPxPerPt))));
	axpSetFace(tp, v.value(key + ".name"));
	const QColor c(v.value(key + ".color"));
	if (c.isValid()) tp->SetColor(c.redF(), c.greenF(), c.blueF());
}
// One pen onto a line property: "KEY.width" / "KEY.color".
static void axpApplyPen(vtkProperty *pr, const QMap<QString, QString> &v, const QString &key) {
	if (!pr) return;
	double px = 0.0;
	if (axpLengthPx(v.value(key + ".width"), px)) pr->SetLineWidth(float(std::max(0.5, px)));
	const QColor c(v.value(key + ".color"));
	if (c.isValid()) pr->SetColor(c.redF(), c.greenF(), c.blueF());
}

// THE APPLY: this set's stored fields onto this set's OWN actors — its numbers, its axis names, its
// frame lines, its ticks, its grid lines. Nothing of any other raster's axes is reachable from here.
// One -B stride box: "auto", empty (not drawn) or a number with an optional arc d/m/s suffix; the unit
// combo can say the same. Out: on/auto/value in the axis' own units.
static void axpStride(const QMap<QString, QString> &v, const QString &key, bool &on, bool &isAuto, double &val,
                      double &phase) {
	QString t = v.value(key).trimmed();
	on = !t.isEmpty();
	isAuto = t.compare("auto", Qt::CaseInsensitive) == 0;
	val = 0.0;
	if (on && !isAuto) {
		double f = 1.0;
		const QChar u = t.back();
		if (u == 'd') t.chop(1);
		else if (u == 'm') { t.chop(1); f = 1.0 / 60.0; }
		else if (u == 's') { t.chop(1); f = 1.0 / 3600.0; }
		const QString un = v.value(key + ".unit");
		if (un == "m") f = 1.0 / 60.0;
		else if (un == "s") f = 1.0 / 3600.0;
		bool ok = false;
		val = t.toDouble(&ok) * f;
		if (!ok || val <= 0.0) { isAuto = true; val = 0.0; }
	}
	bool ok = false;
	phase = v.value(key + ".phase").trimmed().toDouble(&ok);
	if (!ok) phase = 0.0;
}

// The stored fields -> how rebuildAxesFor draws this set (AxesSet::draw).
static void axpParseDraw(const QMap<QString, QString> &v, AxesSet &A) {
	AxesDraw &D = A.draw;
	D.custom = true;
	static const char *const kAx[3] = { "x", "y", "z" };
	for (int k = 0; k < 3; k++) {
		const QString pre = QString("-Bp") + kAx[k] + ".";
		bool on = false;
		axpStride(v, pre + "a", D.aOn[k], D.aAuto[k], D.a[k], D.aPh[k]);
		axpStride(v, pre + "f", D.fOn[k], D.fAuto[k], D.f[k], D.fPh[k]);
		axpStride(v, pre + "g", on, D.gAuto[k], D.g[k], D.gPh[k]);
		if (!on) D.gAuto[k] = true;
	}
	static const char *const kSide[5] = { "W", "E", "S", "N", "Z" };
	static const char *const kMode[4] = { "Annot. + ticks", "Line + ticks", "Line only", "Not drawn" };
	for (int i = 0; i < 5; i++) {
		const QString m = v.value(QString("MAP_FRAME_AXES.") + kSide[i]);
		for (int j = 0; j < 4; j++)
			if (m == kMode[j]) D.side[i] = j;
	}
	D.gridW = v.value("MAP_FRAME_AXES.Wg") == "1";
	D.gridS = v.value("MAP_FRAME_AXES.Sg") == "1";
	D.noFrame = v.value("-B.+n") == "1";
	static const char *const kLine[4] = { "drawn", "hairline", "not drawn", "bare frame" };
	D.frameLine = 0;
	for (int j = 0; j < 4; j++)
		if (v.value("-B.line") == kLine[j]) D.frameLine = j;
}

// THE APPLY: this set's stored fields onto this set's OWN actors — its numbers, its axis names, its
// frame lines, its ticks, its grid lines. Nothing of any other raster's axes is reachable from here.
// The dialog's defaults are GMT's (paper: black, 12p...). The SCREEN keeps its own look for every font,
// pen and length left at that default: only what the user CHANGED from GMT's default is drawn.
static void axesApplyProps(Scene *s, AxesSet &A, const QMap<QString, QString> &defaults) {
	QMap<QString, QString> v, vc;                  // every field / only the ones changed from GMT's
	for (const auto &kv : A.props) {
		const QString k = QString::fromStdString(kv.first), val = QString::fromStdString(kv.second);
		v[k] = val;
		if (!defaults.contains(k) || defaults.value(k) != val) vc[k] = val;
	}
	AxesDraw &D = A.draw;
	if (!D.annot0) {                               // first Apply: remember the screen look
		D.annot0 = vtkSmartPointer<vtkTextProperty>::New();
		if (!A.xlab.empty() && A.xlab[0]) D.annot0->ShallowCopy(A.xlab[0]->GetTextProperty());
		else { D.annot0->SetFontFamilyToArial(); D.annot0->SetFontSize(10); D.annot0->SetColor(1, 1, 1); }
		D.label0 = vtkSmartPointer<vtkTextProperty>::New();
		if (A.title[0]) D.label0->ShallowCopy(A.title[0]->GetTextProperty());
		if (A.cube) {
			vtkProperty *ln[3] = { A.cube->GetXAxesLinesProperty(), A.cube->GetYAxesLinesProperty(), A.cube->GetZAxesLinesProperty() };
			vtkProperty *gr[3] = { A.cube->GetXAxesGridlinesProperty(), A.cube->GetYAxesGridlinesProperty(), A.cube->GetZAxesGridlinesProperty() };
			for (int i = 0; i < 3; i++) {
				D.frame0[i] = vtkSmartPointer<vtkProperty>::New(); D.frame0[i]->DeepCopy(ln[i]);
				D.grid0[i]  = vtkSmartPointer<vtkProperty>::New(); D.grid0[i]->DeepCopy(gr[i]);
			}
		}
		if (A.ticks) { D.tick0 = vtkSmartPointer<vtkProperty>::New(); D.tick0->DeepCopy(A.ticks->GetProperty()); }
	}
	axpParseDraw(v, A);
	double px = 0.0;                                // tick lengths: the screen's own unless changed
	D.tickMajorPx = axpLengthPx(vc.value("MAP_TICK_LENGTH_PRIMARY.major"), px) ? px : 0.0;
	D.tickMinorPx = axpLengthPx(vc.value("MAP_TICK_LENGTH_PRIMARY.minor"), px) ? px : 0.0;
	if (!D.annot) D.annot = vtkSmartPointer<vtkTextProperty>::New();
	axesCopyFace(D.annot, D.annot0);
	axpApplyFont(D.annot, vc, "FONT_ANNOT_PRIMARY");
	axesEnsureLineActors(s, A);
	for (auto *pool : { &A.xlab, &A.ylab, &A.zlab })
		for (auto &t : *pool)
			if (t) axesCopyFace(t->GetTextProperty(), D.annot);
	const char *labKey[2] = { "-Bx.label", "-By.label" };
	for (int i = 0; i < 2; i++) {
		if (!A.title[i]) continue;
		vtkTextProperty *tp = A.title[i]->GetTextProperty();
		axesCopyFace(tp, D.label0);
		axpApplyFont(tp, vc, "FONT_LABEL");
		const QString txt = v.value(labKey[i]);
		A.title[i]->SetInput(txt.isEmpty() ? " " : txt.toUtf8().constData());   // " ": VTK errors on ""
	}
	// A pen back to its screen look, then what was changed on top.
	auto pen = [&vc](vtkProperty *pr, vtkProperty *orig, const QString &key) {
		if (!pr) return;
		if (orig) { pr->SetColor(orig->GetColor()); pr->SetLineWidth(orig->GetLineWidth()); }
		axpApplyPen(pr, vc, key);
	};
	if (A.cube) {
		vtkProperty *ln[3] = { A.cube->GetXAxesLinesProperty(), A.cube->GetYAxesLinesProperty(), A.cube->GetZAxesLinesProperty() };
		vtkProperty *gr[3] = { A.cube->GetXAxesGridlinesProperty(), A.cube->GetYAxesGridlinesProperty(), A.cube->GetZAxesGridlinesProperty() };
		for (int i = 0; i < 3; i++) {
			pen(ln[i], D.frame0[i], "MAP_FRAME_PEN");
			pen(gr[i], D.grid0[i], "MAP_GRID_PEN_PRIMARY");
		}
	}
	if (A.ticks) pen(A.ticks->GetProperty(), D.tick0, "MAP_TICK_PEN_PRIMARY");
	if (A.frame) pen(A.frame->GetProperty(), D.tick0, "MAP_FRAME_PEN");       // the border drew in the tick look
	if (A.grid)  pen(A.grid->GetProperty(), D.tick0, "MAP_GRID_PEN_PRIMARY");
	if (s && s->widget) s->widget->renderWindow()->Render();
}

struct AxesPropsDialog {
	QDialog *dlg = nullptr;
	Scene   *s = nullptr;
	int      owner = kAxesOwnerBase;       // WHOSE set: re-resolved on every use, never a held pointer
	std::vector<QWidget *> fields;
	QMap<QString, QString> defaults;       // every field as the .ui has it (the "Defaults" button)
	QCheckBox *dataLimits[3] = { nullptr, nullptr, nullptr };    // "Follow the data", per axis X Y Z
	QLineEdit *limitEdits[3][2] = { { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr } };

	// The set this dialog edits, or null when its raster is gone. Looked up by OWNER TAG each time:
	// an extra's AxesSet lives inside s->extras, which moves when another extra is added or removed.
	AxesSet *set() const {
		if (!s) return nullptr;
		if (owner == kAxesOwnerBase) return &s->baseAxes;
		for (auto &ex : s->extras)
			if (ex.tag == owner) return &ex.ax;
		return nullptr;
	}
	QString rasterName() const {
		if (!s) return QString();
		if (owner == kAxesOwnerBase) return QString::fromStdString(s->surfName.empty() ? "Surface" : s->surfName);
		for (auto &ex : s->extras)
			if (ex.tag == owner) return QString::fromStdString(ex.name);
		return QString();
	}
	// An IMAGE has no Z: every vertical property of its axes is meaningless, so it is greyed out.
	bool isImage() const {
		if (!s) return false;
		if (owner == kAxesOwnerBase) return s->imageOnly;
		for (auto &ex : s->extras)
			if (ex.tag == owner) return ex.isImage;
		return false;
	}

	QWidget *field(const QString &key) const {
		for (QWidget *w : fields)
			if (axpKey(w) == key) return w;
		return nullptr;
	}

	// The data's own limits into one axis' -R boxes (THIS raster's frame, in its own units).
	void showDataLimits(const AxesSet &A, int k) {
		const double v[3][2] = { { A.x0, A.x1 }, { A.y0, A.y1 }, { A.z0, A.z1 } };
		for (int j = 0; j < 2; j++)
			if (limitEdits[k][j]) limitEdits[k][j]->setText(QString::number(v[k][j], 'g', 12));
	}
	void showDataLimitsWhereFollowed(const AxesSet &A) {
		for (int k = 0; k < 3; k++)
			if (dataLimits[k] && dataLimits[k]->isChecked()) showDataLimits(A, k);
	}
	void syncLimitsEnabled() {
		for (int k = 0; k < 3; k++) {
			if (!dataLimits[k]) continue;
			const bool own = !dataLimits[k]->isChecked() && dataLimits[k]->isEnabled();
			for (int j = 0; j < 2; j++)
				if (limitEdits[k][j]) limitEdits[k][j]->setEnabled(own);
		}
	}

	// The defaults, then (unless `defaultsOnly`) what this set has stored over them. A never-edited set
	// opens on the .ui's values, with what comes from the raster itself (limits, axis names) filled in.
	void fill(bool defaultsOnly) {
		const AxesSet *A = set();
		for (QWidget *w : fields) axpSet(w, defaults.value(axpKey(w)));
		if (A) {
			// A geographic raster's axes need no name: the degree annotations say what they are.
			if (!A->geog) {
				if (QWidget *w = field("-Bx.label")) axpSet(w, QString::fromStdString(A->name[0]));
				if (QWidget *w = field("-By.label")) axpSet(w, QString::fromStdString(A->name[1]));
			}
			// Fonts, pens, lengths: GMT's own defaults (the .ui's values), never the viewer's screen look.
			if (!defaultsOnly)
				for (const auto &kv : A->props)
					if (QWidget *w = field(QString::fromStdString(kv.first))) axpSet(w, QString::fromStdString(kv.second));
			showDataLimitsWhereFollowed(*A);
		}
		syncLimitsEnabled();
	}

	// Apply: the fields into THIS set's own store. False when the raster is gone.
	bool store() {
		AxesSet *A = set();
		if (!A) return false;
		showDataLimitsWhereFollowed(*A);              // "follow the data" stores the data's limits
		A->props.clear();
		for (QWidget *w : fields) A->props[axpKey(w).toStdString()] = axpGet(w).toStdString();
		return true;
	}
};

// One dialog per (window, raster): opening it again brings the open one forward.
static std::vector<AxesPropsDialog *> g_axesProps;

static void axesPropsOpen(Scene *s, AxesSet *A) {
	if (!s || !A) return;
	const int owner = A->owner;
	for (AxesPropsDialog *p : g_axesProps)
		if (p->s == s && p->owner == owner) {
			p->dlg->show();
			p->dlg->raise();
			p->dlg->activateWindow();
			return;
		}
	QWidget *parent = s->win;
	QFile uif(QDir(gmtvtkUiDir()).filePath("axes_props.ui"));
	QUiLoader loader;
	QDialog *dlg = uif.open(QIODevice::ReadOnly) ? qobject_cast<QDialog *>(loader.load(&uif, parent)) : nullptr;
	uif.close();
	if (!dlg) {
		QMessageBox::warning(parent, "Axes properties", "Cannot load " + uif.fileName());
		return;
	}
	auto *p = new AxesPropsDialog;
	p->dlg = dlg;
	p->s = s;
	p->owner = owner;
	static const char *const kAx[3] = { "X", "Y", "Z" };
	for (int k = 0; k < 3; k++) {
		p->dataLimits[k]    = dlg->findChild<QCheckBox *>(QString("chkDataLimits") + kAx[k]);
		p->limitEdits[k][0] = dlg->findChild<QLineEdit *>(QString("edit") + kAx[k] + "min");
		p->limitEdits[k][1] = dlg->findChild<QLineEdit *>(QString("edit") + kAx[k] + "max");
	}
	auto *tabs   = dlg->findChild<QTabWidget *>("tabs");
	auto *apply  = dlg->findChild<QPushButton *>("btnApply");
	auto *defs   = dlg->findChild<QPushButton *>("btnDefaults");
	if (!(tabs && apply && defs)) {
		QMessageBox::warning(parent, "Axes properties",
		                     "axes_props.ui does not carry this dialog's controls (a renamed or removed widget).");
		delete dlg;
		delete p;
		return;
	}
	g_axesProps.push_back(p);

	// The font lists are GMT's, so they come from here, not from each combo of the .ui.
	for (QComboBox *c : dlg->findChildren<QComboBox *>())
		if (c->property("gmtPart").toString() == "name" && c->count() == 0) {
			for (const char *f : kGmtFontNames) c->addItem(f);
			if (c->property("gmtKey").toString() == "FONT") c->insertItem(0, QString());   // "leave as is"
			c->setCurrentIndex(0);
		}
	// ...and so are the -B interval units (arc d/m/s on maps, the time units otherwise).
	for (QComboBox *c : dlg->findChildren<QComboBox *>())
		if (c->property("gmtPart").toString().endsWith(".unit") && c->count() == 0) {
			for (const char *u : { "", "d", "m", "s", "Y", "y", "O", "o", "U", "u", "r", "K", "k", "D", "R",
			                       "H", "h", "M", "S" })
				c->addItem(u);
			c->setCurrentIndex(0);
		}
	for (QWidget *w : dlg->findChildren<QWidget *>()) {
		const QString key = axpKey(w);
		if (key.isEmpty()) continue;
		p->fields.push_back(w);
		if (auto *b = qobject_cast<QPushButton *>(w)) axpPaintSwatch(b, b->property("gmtColor").toString());
		p->defaults[key] = axpGet(w);
	}
	// AN IMAGE HAS NO Z. Everything the .ui tags "vertical" (the Z axis and 3-D tabs, the vertical side,
	// Z orthogonal annotations, ...) is greyed out: a whole tab page is disabled as a tab.
	if (p->isImage())
		for (QWidget *w : dlg->findChildren<QWidget *>()) {
			if (!w->property("vertical").toBool()) continue;
			const int ti = tabs->indexOf(w);
			if (ti >= 0) tabs->setTabEnabled(ti, false);
			else w->setEnabled(false);
		}

	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	const QString nm = p->rasterName();
	dlg->setWindowTitle(nm.isEmpty() ? QString("Axes properties") : "Axes properties - " + nm);

	// Only Apply acts. Enter in a box must never press it (QDialog accepts on Enter even with
	// every button's autoDefault off), so the dialog swallows it.
	struct NoEnter : QObject {
		using QObject::QObject;
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::KeyPress) {
				const int k = static_cast<QKeyEvent *>(e)->key();
				if (k == Qt::Key_Return || k == Qt::Key_Enter) return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	dlg->installEventFilter(new NoEnter(dlg));
	for (QPushButton *b : dlg->findChildren<QPushButton *>()) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}

	// Colour buttons pick their colour.
	for (QWidget *w : p->fields) {
		auto *b = qobject_cast<QPushButton *>(w);
		if (!b) continue;
		QObject::connect(b, &QPushButton::clicked, dlg, [p, b]() {
			const QColor c = QColorDialog::getColor(QColor(b->property("gmtColor").toString()), p->dlg, "Colour");
			if (!c.isValid()) return;
			axpPaintSwatch(b, c.name());
			// FONT colour: every font takes it (FONT sets all fonts)
			if (b->property("gmtKey").toString() == "FONT")
				for (QWidget *f : p->fields)
					if (f != b && f->property("gmtKey").toString().startsWith("FONT_") &&
					    f->property("gmtPart").toString() == "color")
						axpSet(f, c.name());
		});
	}
	// FONT size / name: copied onto every font row as it is chosen (UI state only).
	auto toAllFonts = [p](const QString &part, const QString &v) {
		if (v.isEmpty()) return;
		for (QWidget *f : p->fields)
			if (f->property("gmtKey").toString().startsWith("FONT_") && f->property("gmtPart").toString() == part)
				axpSet(f, v);
	};
	if (auto *e = qobject_cast<QLineEdit *>(p->field("FONT.size")))
		QObject::connect(e, &QLineEdit::textEdited, dlg, [toAllFonts](const QString &v) { toAllFonts("size", v.trimmed()); });
	if (auto *c = qobject_cast<QComboBox *>(p->field("FONT.name")))
		QObject::connect(c, &QComboBox::activated, dlg, [toAllFonts, c](int) { toAllFonts("name", c->currentText()); });

	for (int k = 0; k < 3; k++)
		if (p->dataLimits[k])
			QObject::connect(p->dataLimits[k], &QCheckBox::toggled, dlg, [p, k](bool on) {
				if (on)
					if (const AxesSet *A = p->set()) p->showDataLimits(*A, k);
				p->syncLimitsEnabled();
			});

	auto doApply = [p]() -> bool {
		if (p->store()) {
			if (AxesSet *A = p->set()) axesApplyProps(p->s, *A, p->defaults);
			return true;
		}
		QMessageBox::warning(p->dlg, "Axes properties", "The raster these axes belong to is no longer in the window.");
		p->dlg->close();
		return false;
	};
	QObject::connect(apply, &QPushButton::clicked, dlg, [doApply]() { doApply(); });
	QObject::connect(defs, &QPushButton::clicked, dlg, [p]() { p->fill(true); });
	QObject::connect(dlg, &QObject::destroyed, [p]() {
		g_axesProps.erase(std::remove(g_axesProps.begin(), g_axesProps.end(), p), g_axesProps.end());
		delete p;
	});

	p->fill(false);
	wrapTooltips(dlg);
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}
