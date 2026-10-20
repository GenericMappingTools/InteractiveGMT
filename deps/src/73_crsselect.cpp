// =============================================================================================
// 73_crsselect.cpp -- the Coordinate Reference System selector (QGIS's "CRS Selector", same layout:
// filter, recently used, predefined tree, details + area-of-use map). A MODAL PICKER that returns a
// value, like a file dialog -- not a tool dialog, so it does not park.
//
// Everything it shows comes from PROJ's own database, asked on the Julia side (src/crsselect.jl)
// through ONE callback with the two-phase buffer protocol (size query, then fill):
//   "list"       one line per CRS: id \t name \t type \t deprecated \t bbox \t w \t s \t e \t n \t
//                method \t area \t body          (type = PROJ's PJ_TYPE number)
//   "coast"      "lon lat" lines, segments separated by ">" (GMT low-resolution shoreline)
//   "info <id>"  WKT2, a line holding only \x1f, then the PROJ string
// The catalogue and the coastline are read once per session and kept here.
// =============================================================================================

using JuliaCrsFn = int (*)(const char *req, char *out, int cap);
static JuliaCrsFn g_juliaCrs = nullptr;

// Answer of one request: -1 when the callback failed, else its length; written only when it fits.
static bool crsAsk(const QString &req, QByteArray &ans) {
	ans.clear();
	if (!g_juliaCrs) return false;
	const QByteArray r = req.toUtf8();
	const int n = g_juliaCrs(r.constData(), nullptr, 0);
	if (n < 0) return false;
	QByteArray buf(n + 1, '\0');
	const int m = g_juliaCrs(r.constData(), buf.data(), n + 1);
	if (m < 0 || m > n) return false;
	buf.resize(m);
	ans = buf;
	return true;
}

struct CrsRow {
	QString id, name, method, area, body;
	int type = 0;
	bool deprecated = false, bbox = false;
	double w = 0, s = 0, e = 0, n = 0;
};
static std::vector<CrsRow> g_crsRows;
static QHash<QString, int> g_crsIndex;          // id -> row
static QPainterPath g_crsLand, g_crsShore;      // closed shoreline polygons (filled) / open segments
static QHash<QString, QString> g_crsInfo;       // id -> "WKT \x1f PROJ" (asked once per id)

// PROJ's PJ_TYPE -> the group QGIS files it under.
static QString crsTypeLabel(int t) {
	switch (t) {
		case 9:  return "Geodetic";
		case 10: return "Geocentric";
		case 11: case 12: return "Geographic (2D)";
		case 13: return "Geographic (3D)";
		case 14: return "Vertical";
		case 15: return "Projected";
		case 16: return "Compound";
		case 17: return "Temporal";
		case 18: return "Engineering";
		case 19: return "Bound";
		default: return "Other";
	}
}

static bool crsLoadCatalogue(QString &err) {
	if (!g_crsRows.empty()) return true;
	if (!g_juliaCrs) { err = "the CRS selector is not wired."; return false; }
	showBusyDialog("Reading the CRS catalogue...");
	QByteArray list, coast;
	const bool ok = crsAsk("list", list) && crsAsk("coast", coast);
	closeBusyDialog();
	if (!ok || list.isEmpty()) { err = "PROJ's CRS catalogue could not be read (see the Messages panel)."; return false; }
	for (const QByteArray &ln : list.split('\n')) {
		const QList<QByteArray> f = ln.split('\t');
		if (f.size() < 12) continue;
		CrsRow r;
		r.id = QString::fromUtf8(f[0]);
		r.name = QString::fromUtf8(f[1]);
		r.type = f[2].toInt();
		r.deprecated = f[3].toInt() != 0;
		r.bbox = f[4].toInt() != 0;
		r.w = f[5].toDouble(); r.s = f[6].toDouble(); r.e = f[7].toDouble(); r.n = f[8].toDouble();
		r.method = QString::fromUtf8(f[9]);
		r.area = QString::fromUtf8(f[10]);
		r.body = QString::fromUtf8(f[11]);
		g_crsIndex.insert(r.id, int(g_crsRows.size()));
		g_crsRows.push_back(std::move(r));
	}
	QPolygonF seg;
	auto flush = [&]() {
		if (seg.size() >= 2) {
			if (seg.size() >= 4 && seg.first() == seg.last()) g_crsLand.addPolygon(seg);
			else { g_crsShore.moveTo(seg.first()); for (int i = 1; i < seg.size(); i++) g_crsShore.lineTo(seg[i]); }
		}
		seg.clear();
	};
	for (const QByteArray &ln : coast.split('\n')) {
		if (ln.startsWith('>')) { flush(); continue; }
		const QList<QByteArray> f = ln.simplified().split(' ');
		if (f.size() >= 2) seg << QPointF(f[0].toDouble(), f[1].toDouble());
	}
	flush();
	g_crsLand.setFillRule(Qt::OddEvenFill);
	return true;
}

// ---- the area-of-use map: plate carree, the shoreline drawn at -360/0/+360 so a box that crosses
// the antimeridian (west > east) is just a box past 180.
class CrsMapWidget : public QWidget {
public:
	explicit CrsMapWidget(QWidget *parent) : QWidget(parent) {}
	void setBox(bool valid, double w, double s, double e, double n) {
		has = valid; bw = w; bs = s; be = e; bn = n;
		if (has && be < bw) be += 360;   // crosses the antimeridian
		update();
	}
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), QColor(255, 255, 255));
		// the view: the box with a margin around it, or the whole world
		double x0 = -180, x1 = 180, y0 = -90, y1 = 90;
		if (has) {
			const double dw = std::max(be - bw, 2.0), dh = std::max(bn - bs, 2.0);
			x0 = bw - 0.4 * dw; x1 = be + 0.4 * dw;
			y0 = bs - 0.4 * dh; y1 = bn + 0.4 * dh;
		}
		// keep the aspect of the widget (degrees are square on screen)
		const double W = std::max(width(), 1), H = std::max(height(), 1);
		double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1), sx = x1 - x0, sy = y1 - y0;
		if (sx / sy > W / H) sy = sx * H / W; else sx = sy * W / H;
		if (sy > 180) { sy = 180; sx = sy * W / H; }
		cy = std::min(std::max(cy, -90 + 0.5 * sy), 90 - 0.5 * sy);
		x0 = cx - 0.5 * sx; y1 = cy + 0.5 * sy;
		const double k = W / sx;
		const QTransform T(k, 0, 0, -k, -x0 * k, y1 * k);
		QPen shore(QColor(170, 160, 145));
		shore.setCosmetic(true);
		for (int off = -360; off <= 360; off += 360) {
			p.setTransform(QTransform::fromTranslate(off, 0) * T);
			p.setPen(shore);
			p.setBrush(QColor(235, 231, 218));
			p.drawPath(g_crsLand);
			p.setBrush(Qt::NoBrush);
			p.drawPath(g_crsShore);
		}
		if (has) {
			p.setTransform(T);
			QPen red(QColor(240, 60, 60));
			red.setCosmetic(true);
			red.setWidth(2);
			p.setPen(red);
			p.setBrush(QColor(255, 80, 80, 90));
			p.drawRect(QRectF(QPointF(bw, bs), QPointF(be, bn)));
		}
	}
private:
	bool has = false;
	double bw = 0, bs = 0, be = 0, bn = 0;
};

// ---- the resize handler: the .ui is the DESIGN size (780x860); fixed rows keep their height, the
// three panes share the free height in their .ui proportions (150 : 320 : 227), widths stretch.
static void crsLayout(QDialog *d) {
	const int W = d->width(), H = d->height(), m = 11, full = W - 2 * m;
	auto place = [d](const char *name, int x, int y, int w, int h) {
		if (QWidget *c = d->findChild<QWidget *>(name)) c->setGeometry(x, y, std::max(w, 10), std::max(h, 10));
	};
	const int fixedH = 163, flex = std::max(H - fixedH, 150);
	const int hRecent = flex * 150 / 697, hTree = flex * 320 / 697, hInfo = flex - hRecent - hTree;
	int y = m;
	place("filterLabel", m, y, 40, 26);
	place("filterEdit", m + 45, y, full - 45, 26);
	y += 36;
	place("recentLabel", m, y, full - 160, 22);
	y += 24;
	place("recentTree", m, y, full, hRecent);
	y += hRecent + 10;
	place("predefLabel", m, y, full - 160, 22);
	place("hideDeprecated", W - m - 149, y, 149, 22);
	y += 24;
	place("crsTree", m, y, full, hTree);
	y += hTree + 10;
	const int half = (full - 10) / 2;
	place("infoText", m, y, half, hInfo);
	place("mapFrame", m + half + 10, y, full - half - 10, hInfo);
	if (QWidget *f = d->findChild<QWidget *>("mapFrame"))
		for (QWidget *c : f->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) c->setGeometry(f->rect().adjusted(1, 1, -1, -1));
	y += hInfo + 10;
	place("buttonBox", m, y, full, 28);
}

struct CrsResizeFilter : QObject {
	using QObject::QObject;
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() == QEvent::Resize) crsLayout(static_cast<QDialog *>(o));
		return false;
	}
};

static QString crsDetailsHtml(const CrsRow &r) {
	QString wkt, proj;
	auto it = g_crsInfo.find(r.id);
	if (it == g_crsInfo.end()) {
		QByteArray a;
		const QString s = crsAsk("info " + r.id, a) ? QString::fromUtf8(a) : QString();
		it = g_crsInfo.insert(r.id, s);
	}
	const int k = it.value().indexOf(QChar(0x1f));
	wkt = k >= 0 ? it.value().left(k).trimmed() : it.value().trimmed();
	proj = k >= 0 ? it.value().mid(k + 1).trimmed() : QString();
	QString h = "<h3>" + r.name.toHtmlEscaped() + "</h3><b>Properties</b><ul>";
	h += "<li>" + crsTypeLabel(r.type).toHtmlEscaped() + "</li>";
	if (!r.method.isEmpty()) h += "<li>Method: " + r.method.toHtmlEscaped() + "</li>";
	if (!r.body.isEmpty()) h += "<li>Celestial body: " + r.body.toHtmlEscaped() + "</li>";
	if (!r.area.isEmpty()) h += "<li>Area of use: " + r.area.toHtmlEscaped() + "</li>";
	if (r.bbox) h += QString("<li>Bounds: %1, %2, %3, %4</li>").arg(r.w).arg(r.s).arg(r.e).arg(r.n);
	if (r.deprecated) h += "<li><b>Deprecated</b></li>";
	h += "</ul>";
	if (!wkt.isEmpty()) h += "<b>WKT</b><pre>" + wkt.toHtmlEscaped() + "</pre>";
	if (!proj.isEmpty()) h += "<b>PROJ</b><pre>" + proj.toHtmlEscaped() + "</pre>";
	return h;
}

// The selector. `initial` preselects a system ("EPSG:3763"); `chosen` gets the authority ID picked.
// `onShown`, when given, runs once the dialog is on screen (the GUI test drives it from there).
static bool crsSelectDialog(QWidget *parent, const QString &initial, QString &chosen,
                            std::function<void(QDialog *)> onShown = {}) {
	chosen.clear();
	QString err;
	if (!crsLoadCatalogue(err)) {
		QMessageBox::warning(parent, "Coordinate Reference System", err);
		return false;
	}
	QUiLoader loader;
	QFile f(gmtvtkUiDir() + "/crs_select.ui");
	if (!f.open(QFile::ReadOnly)) {
		QMessageBox::warning(parent, "Coordinate Reference System", "Cannot open " + f.fileName());
		return false;
	}
	QDialog *d = qobject_cast<QDialog *>(loader.load(&f, parent));
	f.close();
	if (!d) {
		QMessageBox::warning(parent, "Coordinate Reference System", "Cannot load crs_select.ui");
		return false;
	}
	auto *filter = d->findChild<QLineEdit *>("filterEdit");
	auto *recent = d->findChild<QTreeWidget *>("recentTree");
	auto *tree = d->findChild<QTreeWidget *>("crsTree");
	auto *hideDep = d->findChild<QCheckBox *>("hideDeprecated");
	auto *info = d->findChild<QTextBrowser *>("infoText");
	auto *frame = d->findChild<QFrame *>("mapFrame");
	auto *box = d->findChild<QDialogButtonBox *>("buttonBox");
	if (!filter || !recent || !tree || !hideDep || !info || !frame || !box) {
		delete d;
		QMessageBox::warning(parent, "Coordinate Reference System", "crs_select.ui is missing a widget.");
		return false;
	}
	auto *map = new CrsMapWidget(frame);
	QPushButton *okBtn = box->button(QDialogButtonBox::Ok);
	okBtn->setEnabled(false);

	// ---- the predefined tree: type groups, Projected split by projection method, names sorted
	std::vector<int> order(g_crsRows.size());
	for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
	std::vector<QTreeWidgetItem *> itemOf(g_crsRows.size(), nullptr);   // row -> its tree item
	std::sort(order.begin(), order.end(), [](int a, int b) {
		return QString::compare(g_crsRows[a].name, g_crsRows[b].name, Qt::CaseInsensitive) < 0;
	});
	QFont gfont = tree->font();
	gfont.setBold(true);
	gfont.setItalic(true);
	// groups are attached after the loop, in key order, so both levels come out sorted
	QMap<QString, QTreeWidgetItem *> groups;
	QHash<QTreeWidgetItem *, QTreeWidgetItem *> parentOf;
	auto group = [&](const QString &key, const QString &label, QTreeWidgetItem *under) {
		auto it = groups.find(key);
		if (it != groups.end()) return it.value();
		auto *g = new QTreeWidgetItem(QStringList{label});
		g->setFont(0, gfont);
		g->setFlags(Qt::ItemIsEnabled);
		parentOf.insert(g, under);
		groups.insert(key, g);
		return g;
	};
	tree->setUpdatesEnabled(false);
	for (int i : order) {
		const CrsRow &r = g_crsRows[i];
		const QString top = crsTypeLabel(r.type);
		QTreeWidgetItem *g = group(top, top, nullptr);
		if (r.type == 15) {
			const QString meth = r.method.isEmpty() ? QString("Other") : r.method;
			g = group(top + "/" + meth, meth, g);
		}
		auto *it = new QTreeWidgetItem(QStringList{r.name, r.id});
		it->setData(0, Qt::UserRole, i);
		g->addChild(it);
		itemOf[i] = it;
	}
	for (auto it = groups.begin(); it != groups.end(); ++it) {
		QTreeWidgetItem *under = parentOf.value(it.value());
		if (under) under->addChild(it.value());
		else tree->addTopLevelItem(it.value());
	}
	tree->setUpdatesEnabled(true);

	// ---- recently used (iGMT.ini)
	const QStringList recentIds = igmtSettings().value("crs/recent").toStringList();
	for (const QString &id : recentIds) {
		auto ix = g_crsIndex.find(id);
		if (ix == g_crsIndex.end()) continue;
		auto *it = new QTreeWidgetItem(QStringList{g_crsRows[ix.value()].name, id});
		it->setData(0, Qt::UserRole, ix.value());
		recent->addTopLevelItem(it);
	}
	for (QTreeWidget *t : {recent, tree}) {
		t->header()->setStretchLastSection(false);
		t->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		t->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	}

	// ---- selection -> details + map + OK
	QString current;
	auto show = [&, map, info, okBtn](QTreeWidgetItem *it) {
		if (!it || !it->data(0, Qt::UserRole).isValid()) return;
		const CrsRow &r = g_crsRows[it->data(0, Qt::UserRole).toInt()];
		current = r.id;
		info->setHtml(crsDetailsHtml(r));
		map->setBox(r.bbox, r.w, r.s, r.e, r.n);
		okBtn->setEnabled(true);
	};
	QObject::connect(tree, &QTreeWidget::currentItemChanged, d, [&](QTreeWidgetItem *it, QTreeWidgetItem *) { show(it); });
	QObject::connect(recent, &QTreeWidget::currentItemChanged, d, [&](QTreeWidgetItem *it, QTreeWidgetItem *) { show(it); });
	for (QTreeWidget *t : {recent, tree})
		QObject::connect(t, &QTreeWidget::itemDoubleClicked, d, [&, d](QTreeWidgetItem *it, int) {
			if (it && it->data(0, Qt::UserRole).isValid()) { show(it); d->accept(); }
		});

	// ---- filter + hide deprecated
	auto applyFilter = [&, filter, hideDep, tree, recent]() {
		const QStringList words = filter->text().split(' ', Qt::SkipEmptyParts);
		const bool hide = hideDep->isChecked();
		auto keep = [&](QTreeWidgetItem *it) {
			const CrsRow &r = g_crsRows[it->data(0, Qt::UserRole).toInt()];
			if (hide && r.deprecated) return false;
			for (const QString &w : words)
				if (!r.name.contains(w, Qt::CaseInsensitive) && !r.id.contains(w, Qt::CaseInsensitive)) return false;
			return true;
		};
		std::function<bool(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *g) {
			bool any = false;
			for (int c = 0; c < g->childCount(); c++) {
				QTreeWidgetItem *ch = g->child(c);
				const bool v = ch->data(0, Qt::UserRole).isValid() ? keep(ch) : walk(ch);
				ch->setHidden(!v);
				any = any || v;
			}
			if (!words.isEmpty()) g->setExpanded(any);
			return any;
		};
		tree->setUpdatesEnabled(false);
		for (int t = 0; t < tree->topLevelItemCount(); t++) {
			QTreeWidgetItem *g = tree->topLevelItem(t);
			g->setHidden(!walk(g));
		}
		tree->setUpdatesEnabled(true);
		for (int t = 0; t < recent->topLevelItemCount(); t++)
			recent->topLevelItem(t)->setHidden(!keep(recent->topLevelItem(t)));
	};
	QObject::connect(filter, &QLineEdit::textChanged, d, [&](const QString &) { applyFilter(); });
	QObject::connect(hideDep, &QCheckBox::toggled, d, [&](bool) { applyFilter(); });

	// ---- preselect
	const QString want = initial.trimmed().toUpper();
	auto wantIx = g_crsIndex.find(want);
	if (wantIx == g_crsIndex.end()) wantIx = g_crsIndex.find(initial.trimmed());
	if (wantIx != g_crsIndex.end() && itemOf[wantIx.value()]) {
		QTreeWidgetItem *it = itemOf[wantIx.value()];
		tree->setCurrentItem(it);
		tree->scrollToItem(it, QAbstractItemView::PositionAtCenter);
	}

	QObject::connect(box, &QDialogButtonBox::accepted, d, &QDialog::accept);
	QObject::connect(box, &QDialogButtonBox::rejected, d, &QDialog::reject);

	// ---- resizable, never taller than the screen
	d->setSizeGripEnabled(true);
	d->setMinimumSize(520, 480);
	d->installEventFilter(new CrsResizeFilter(d));
	const QScreen *scr = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
	if (scr) {
		const QRect av = scr->availableGeometry();
		d->resize(std::min(d->width(), av.width() - 40), std::min(d->height(), av.height() - 60));
	}
	crsLayout(d);
	wrapTooltips(d);

	if (onShown) QTimer::singleShot(0, d, [d, onShown]() { onShown(d); });
	const bool ok = d->exec() == QDialog::Accepted && !current.isEmpty();
	if (ok) {
		chosen = current;
		QStringList r = igmtSettings().value("crs/recent").toStringList();
		r.removeAll(chosen);
		r.prepend(chosen);
		while (r.size() > 10) r.removeLast();
		igmtSettings().setValue("crs/recent", r);
	}
	delete d;
	return ok;
}

// The selector as a door from another dialog (Tools > Project's "Projections" combo, 70_window.cpp).
// The Julia side is wired on first use (warm_register "crs"), so the first call waits for it -- wait
// cursor, ~10 s cap -- exactly as the grdblend menu entry does. `done` gets the authority ID and the
// system's name, or two empty strings when the user cancelled; it is not called once `parent` is gone.
static void crsPickAsync(QWidget *parent, const QString &initial,
                         std::function<void(const QString &id, const QString &name)> done) {
	QPointer<QWidget> guard(parent);
	auto open = [guard, initial, done]() {
		if (!guard) return;
		QString id;
		if (!crsSelectDialog(guard.data(), initial, id)) { done(QString(), QString()); return; }
		auto it = g_crsIndex.find(id);
		done(id, it != g_crsIndex.end() ? g_crsRows[it.value()].name : id);
	};
	if (g_juliaCrs) { open(); return; }
	warmupTool("crs");
	QApplication::setOverrideCursor(Qt::WaitCursor);
	auto tries = std::make_shared<int>(0);
	auto step = std::make_shared<std::function<void()>>();
	*step = [guard, tries, step, open, done]() {
		if (!g_juliaCrs && guard && ++*tries < 200) {
			QTimer::singleShot(50, guard.data(), *step);
			return;
		}
		QApplication::restoreOverrideCursor();
		if (!guard) return;
		if (g_juliaCrs) { open(); return; }
		QMessageBox::warning(guard.data(), "Coordinate Reference System",
		                     "The CRS selector's Julia side did not answer (src/crsselect.jl).");
		done(QString(), QString());
	};
	QTimer::singleShot(0, guard.data(), *step);
}

// The window reprojected into the system picked on its status-corner EPSG chip. Asked of Julia first
// ("check"): when PROJ cannot carry the window's data there, a warning says why and nothing runs.
// Otherwise it is Tools > Project's own run (g_juliaProject, src/project.jl) -- the ONE reprojection,
// with the window's CRS as the source, exactly what the Project dialog's Source box holds.
static void crsReprojectWindow(Scene *s, const QString &id, const QString &name) {
	if (!sceneAlive(s) || !s->win) return;
	const QString grid = QString::fromStdString(activeGridName(s));
	QByteArray ans;
	const bool asked = crsAsk(QString("check\t%1\t%2\t%3").arg(quintptr(s), 0, 16).arg(grid, id), ans);
	const QString a = QString::fromUtf8(ans);
	if (!asked || !(a.startsWith("OK\t") || a.startsWith("NO\t"))) {
		QMessageBox::warning(s->win, "Reproject", "Could not find out whether this window can be reprojected "
		                     "into " + id + " (see the Messages panel).");
		return;
	}
	if (a.startsWith("NO\t")) {
		QMessageBox::warning(s->win, "Reproject", "This window cannot be reprojected into " + name + " (" + id +
		                     ").\n\n" + a.mid(3));
		return;
	}
	if (!g_juliaProject) {
		QMessageBox::warning(s->win, "Reproject", "The reprojection's Julia side is not wired (src/project.jl).");
		return;
	}
	QStringList kv;
	kv << "t_srs=" + id << "s_srs=" + a.mid(3).simplified() << "resample=bilinear" << "projname=" + name
	   << "grid=" + grid;
	if (id.startsWith("EPSG:")) kv << "t_epsg=" + id.mid(5);
	showBusyDialog(("Reprojecting into " + id + "...").toUtf8().constData());
	const int ok = g_juliaProject(s, kv.join("\n").toUtf8().constData());
	closeBusyDialog();
	if (!ok && sceneAlive(s))
		QMessageBox::warning(s->win, "Reproject", "The reprojection into " + id +
		                     " failed - see this window's Messages panel for details.");
}
