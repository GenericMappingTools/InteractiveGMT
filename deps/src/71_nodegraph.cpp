// 71_nodegraph.cpp — the "Plot with GMT.jl" script as a graph of boxes (the script editor's Graph… button).
//
// The graph is a VIEW OF THE EDITOR'S TEXT (docs/NODESCRIPT_PLAN.md). Julia parses the box's current text
// (src/nodescript.jl) and hands the result over as a line blob through gmtvtk_nodegraph_show_h; this file
// only DRAWS it: one box per statement, wires from the box that assigns a variable to the boxes that use
// it, and the program steps linked top to bottom in the order they paint. Editing a field asks Julia to
// rewrite that one statement in the editor's text (`_nodegraph_edit`), which re-sends the graph. Run and
// Save… press the editor's OWN buttons — one runner, one save path (SACRED_LAW.md).
//
// One graph window per script editor, keyed like the editor by the owner's handle (a 3-D viewer's Scene
// or an X,Y plot). Minimise / Esc park it as a handle in the owner's dock; the X destroys it, as the
// editor's does (it holds nothing of its own: Refresh rebuilds it from the text).

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QPainterPath>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QColorDialog>
#include <QCheckBox>
#include <QDir>
#include <QFrame>
#include <QToolButton>
#include <QGridLayout>
#include <memory>

// The CRS selector (73_crsselect.cpp, included after this fragment): a picked system as a GMT.jl proj.
static QString crsGmtProj(const QString &id);

static const double NG_W = 270.0;     // box width (scene units = pixels at zoom 1)
static const double NG_TH = 22.0;      // title bar height
static const double NG_NH = 16.0;      // note line height (the step's heading comment)
static const double NG_RH = 18.0;      // one field row
static const double NG_COLW = 330.0;   // column pitch: values sit one column left of their user
static const double NG_GAP = 26.0;     // vertical gap between boxes in one column

struct NGFieldRow {
	int stmt = 0;                      // the statement an edit rewrites (0 = not editable)
	QString key, label, value;         // which argument, its GMT.jl name, its GMT.jl code
	QString type, plain, shown, extra; // its dialog, its name and value in words, what the dialog needs
};

// Row data slots (QGraphicsItem::setData) carrying a field to its dialog.
enum { NGD_STMT = 0, NGD_KEY, NGD_VALUE, NGD_LABEL, NGD_TYPE, NGD_PLAIN, NGD_EXTRA, NGD_SHOWN };

// A field is editable here when it maps to a statement and is not a value made by another box.
static bool nodeGraphEditable(const QGraphicsItem *it) {
	if (!it || it->data(NGD_STMT).toInt() <= 0) return false;
	const QString t = it->data(NGD_TYPE).toString();
	return t != "ref" && t != "none";
}

struct NGBox {
	int id = 0;
	QString kind, title, note;
	int col = 0, anchor = 0;
	std::vector<NGFieldRow> fields;
	QGraphicsRectItem *item = nullptr;
	double h = 0.0;
	QPointF lastPos;
};

struct NGWire {
	int from = 0, to = 0;
	double dy = 0.0;                   // where on the target box the wire lands (its field's row)
	bool link = false;                 // a program-order link (top to bottom), not a data wire
	QGraphicsPathItem *path = nullptr;
};

struct NodeGraph {
	void *key = nullptr;
	QDialog *win = nullptr;
	QGraphicsScene *scene = nullptr;
	QGraphicsView *view = nullptr;
	std::vector<NGBox> boxes;
	std::vector<NGWire> wires;
	bool editing = false;              // inside a field edit: a re-sent graph is built after it returns
	QString pending;
	QHash<int, QImage> previews;       // box id -> the figure as it stands after that step (Julia, ps2image)
	bool fitted = false;               // the first graph shown is fitted to the view; later ones keep the user's zoom
	QGraphicsItem *pressRow = nullptr; // the field row a left press landed on (a click, not a drag, edits it)
	QPoint pressAt;
	QPointer<QLineEdit> inl;           // the in-place editor over a field row, while one is open
	QObject *flt = nullptr;            // the window's event filter (NodeGraphFilter), also on that editor
	QPointer<QFrame> panel;            // the control panel open under a field row (nodeGraphFieldDialog)
};

static QHash<void *, NodeGraph *> &nodeGraphRegistry() {
	static QHash<void *, NodeGraph *> reg;
	return reg;
}

static QColor nodeGraphKindColor(const QString &kind) {
	if (kind == "figure")  return QColor(105, 105, 110);
	if (kind == "data")    return QColor(58, 130, 74);
	if (kind == "palette") return QColor(170, 130, 40);
	if (kind == "call")    return QColor(56, 104, 170);
	if (kind == "output")  return QColor(168, 58, 58);
	return QColor(80, 80, 84);                                     // "julia": any other statement
}

static NGBox *nodeGraphBox(NodeGraph *g, int id) {
	for (auto &b : g->boxes) if (b.id == id) return &b;
	return nullptr;
}

// A Julia string literal holding `s` (what an eval'd call is handed: a field's new text).
static QString nodeGraphJlStr(const QString &s) {
	QString o = s;
	o.replace("\\", "\\\\").replace("\"", "\\\"").replace("$", "\\$");
	return "\"" + o + "\"";
}

// Every wire's curve, from where its two boxes are NOW. Data wires leave a box's right edge at its title
// and enter the target at the row of the field that uses the value; program links run bottom -> top.
static void nodeGraphRoute(NodeGraph *g) {
	for (auto &w : g->wires) {
		NGBox *a = nodeGraphBox(g, w.from), *b = nodeGraphBox(g, w.to);
		if (!a || !b || !a->item || !b->item || !w.path) continue;
		const QPointF pa = a->item->pos(), pb = b->item->pos();
		QPainterPath p;
		if (w.link) {
			const QPointF s(pa.x() + NG_W / 2, pa.y() + a->h), e(pb.x() + NG_W / 2, pb.y());
			const double k = std::max(20.0, std::abs(e.y() - s.y()) / 2);
			p.moveTo(s);
			p.cubicTo(s + QPointF(0, k), e - QPointF(0, k), e);
		}
		else {
			const QPointF s(pa.x() + NG_W, pa.y() + NG_TH / 2), e(pb.x(), pb.y() + w.dy);
			const double k = std::max(40.0, std::abs(e.x() - s.x()) / 2);
			p.moveTo(s);
			p.cubicTo(s + QPointF(k, 0), e - QPointF(k, 0), e);
		}
		w.path->setPath(p);
	}
	for (auto &b : g->boxes) if (b.item) b.lastPos = b.item->pos();
}

// The blob (src/nodescript.jl `_nodegraph_blob`) -> boxes, wires and their places.
static void nodeGraphBuild(NodeGraph *g, const QString &blob) {
	if (g->inl) g->inl->deleteLater();                             // an open in-place editor belongs to the old rows
	g->pressRow = nullptr;
	// Where the boxes stand now (the user may have moved them): a rebuilt graph of the SAME boxes — an edit
	// changes values, not the steps — keeps those places instead of jumping back to the automatic layout.
	QHash<int, QPair<QString, QPointF>> was;
	for (const auto &b : g->boxes) if (b.item) was.insert(b.id, qMakePair(b.title, b.item->pos()));
	g->scene->clear();                                             // every item, wire and socket goes
	g->boxes.clear();
	g->wires.clear();
	for (const QString &line : blob.split('\n', Qt::SkipEmptyParts)) {
		const QStringList t = line.split('\t');
		if (t[0] == "N" && t.size() >= 7) {
			NGBox b;
			b.id = t[1].toInt();  b.kind = t[2];  b.col = t[3].toInt();  b.anchor = t[4].toInt();
			b.title = t[5];  b.note = t[6];
			g->boxes.push_back(b);
		}
		else if (t[0] == "F" && t.size() >= 10) {
			if (NGBox *b = nodeGraphBox(g, t[1].toInt()))
				b->fields.push_back({ t[2].toInt(), t[3], t[4], t[5], t[6], t[7], t[8], t[9] });
		}
		else if ((t[0] == "W" || t[0] == "L") && t.size() >= 3) {
			NGWire w;
			w.from = t[1].toInt();  w.to = t[2].toInt();  w.link = t[0] == "L";
			// the wire's label (the field that uses the value) rides in its path item until the boxes exist
			g->wires.push_back(w);
			g->wires.back().path = new QGraphicsPathItem();
			g->wires.back().path->setData(0, t.size() >= 4 ? t[3] : QString());
		}
	}
	QFont fTitle = g->view->font();
	fTitle.setBold(true);
	QFont fNote = g->view->font();
	fNote.setItalic(true);
	QFont fRow = g->view->font();                                   // plain words, not code
	const QFontMetrics mTitle(fTitle), mNote(fNote), mRow(fRow);
	// ── the boxes
	for (auto &b : g->boxes) {
		const bool hasNote = !b.note.isEmpty();
		QImage pic = g->previews.value(b.id);
		if (!pic.isNull() && pic.width() > NG_W - 12)
			pic = pic.scaledToWidth((int)NG_W - 12, Qt::SmoothTransformation);
		if (!pic.isNull() && pic.height() > 2 * NG_W)
			pic = pic.scaledToHeight((int)(2 * NG_W), Qt::SmoothTransformation);
		const double picH = pic.isNull() ? 0.0 : pic.height() + 8.0;
		b.h = NG_TH + (hasNote ? NG_NH : 0.0) + b.fields.size() * NG_RH + picH + 6.0;
		// the box body: dark panel, draggable (its children — title, rows, sockets — move with it)
		b.item = g->scene->addRect(0, 0, NG_W, b.h, QPen(QColor(20, 20, 22), 1.2), QBrush(QColor(52, 52, 56)));
		b.item->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
		b.item->setZValue(1);
		// the title bar, coloured by kind (data green, palette yellow, call blue, output red)
		auto *bar = new QGraphicsRectItem(0, 0, NG_W, NG_TH, b.item);
		bar->setPen(Qt::NoPen);
		bar->setBrush(nodeGraphKindColor(b.kind));
		// the title: the call (grdimage!, plot…) or the variable it assigns (G1 = gmtread)
		auto *tt = new QGraphicsSimpleTextItem(mTitle.elidedText(b.title, Qt::ElideRight, (int)NG_W - 12), b.item);
		tt->setFont(fTitle);
		tt->setBrush(Qt::white);
		tt->setPos(6, (NG_TH - mTitle.height()) / 2);
		double y = NG_TH;
		if (hasNote) {
			// the step's heading comment from the script (# ── grid 'topo.nc'), what this box draws
			auto *nt = new QGraphicsSimpleTextItem(mNote.elidedText(b.note, Qt::ElideRight, (int)NG_W - 12), b.item);
			nt->setFont(fNote);
			nt->setBrush(QColor(175, 175, 180));
			nt->setPos(6, y + (NG_NH - mNote.height()) / 2);
			nt->setToolTip(b.note);
			y += NG_NH;
		}
		for (const auto &f : b.fields) {
			// one field row IN PLAIN LANGUAGE — "Colour palette   turbo" — the WHOLE width of the box: a click
			// anywhere on it opens the dialog for that kind of value (the hand cursor says which rows do)
			const bool ed = f.stmt > 0 && f.type != "ref" && f.type != "none";
			const QString code = f.label.isEmpty() ? f.value : f.label + "=" + f.value;
			auto *row = new QGraphicsRectItem(0, 0, NG_W, NG_RH, b.item);
			row->setPos(0, y);
			row->setPen(Qt::NoPen);
			row->setBrush(ed ? QColor(62, 62, 68) : QColor(52, 52, 56));
			if (ed) row->setCursor(Qt::PointingHandCursor);
			row->setToolTip((f.plain.isEmpty() ? f.shown : f.plain + ": " + f.shown) +
			                (ed ? "\n\nClick to change it. Right-click: edit as GMT.jl code" : "") +
			                (f.value.isEmpty() ? QString() : "\n\nGMT.jl: " + code));
			row->setData(NGD_STMT, ed ? f.stmt : 0);
			row->setData(NGD_KEY, f.key);
			row->setData(NGD_VALUE, f.value);
			row->setData(NGD_LABEL, f.label);
			row->setData(NGD_TYPE, f.type);
			row->setData(NGD_PLAIN, f.plain);
			row->setData(NGD_EXTRA, f.extra);
			row->setData(NGD_SHOWN, f.shown);
			// the name, in grey, then the value, in white, cut with "…" when it does not fit
			double x = 10.0;
			if (!f.plain.isEmpty()) {
				auto *lt = new QGraphicsSimpleTextItem(f.plain, row);
				lt->setFont(fRow);
				lt->setBrush(QColor(160, 160, 168));
				lt->setPos(x, (NG_RH - mRow.height()) / 2);
				lt->setAcceptedMouseButtons(Qt::NoButton);       // the row, not its text, takes the click
				x += mRow.horizontalAdvance(f.plain) + 10.0;
			}
			auto *rt = new QGraphicsSimpleTextItem(mRow.elidedText(f.shown, Qt::ElideRight,
			                                                       std::max(20, (int)(NG_W - x - 8))), row);
			rt->setFont(fRow);
			rt->setBrush(ed ? QColor(235, 235, 240) : QColor(150, 150, 155));
			rt->setPos(x, (NG_RH - mRow.height()) / 2);
			rt->setAcceptedMouseButtons(Qt::NoButton);
			// a hairline between editable rows, so each reads as its own field
			if (ed) {
				auto *ln = new QGraphicsLineItem(4, NG_RH - 0.5, NG_W - 4, NG_RH - 0.5, row);
				ln->setPen(QPen(QColor(44, 44, 48), 1));
			}
			y += NG_RH;
		}
		if (!pic.isNull()) {
			// the picture: what the figure looks like once this step has run (white page, thin frame)
			auto *pm = new QGraphicsPixmapItem(QPixmap::fromImage(pic), b.item);
			pm->setPos((NG_W - pic.width()) / 2, y + 4);
			pm->setTransformationMode(Qt::SmoothTransformation);
			pm->setToolTip("The figure after this step");
			auto *fr = new QGraphicsRectItem(pm->pos().x() - 1, y + 3, pic.width() + 2, pic.height() + 2, b.item);
			fr->setPen(QPen(QColor(20, 20, 22), 1));
			fr->setZValue(-1);
		}
		b.item->setToolTip(b.note.isEmpty() ? b.title : b.title + "\n" + b.note);
	}
	// ── where each wire lands, and its sockets (small discs on the box edges)
	for (auto &w : g->wires) {
		NGBox *a = nodeGraphBox(g, w.from), *b = nodeGraphBox(g, w.to);
		if (!a || !b) {
			delete w.path;
			w.path = nullptr;
			continue;
		}
		if (!w.link) {
			const QString lab = w.path->data(0).toString();
			w.dy = NG_TH / 2;
			double y = NG_TH + (b->note.isEmpty() ? 0.0 : NG_NH);
			for (const auto &f : b->fields) {
				if (!lab.isEmpty() && f.label == lab) { w.dy = y + NG_RH / 2; break; }
				y += NG_RH;
			}
			const QColor c = nodeGraphKindColor(a->kind).lighter(130);
			// input socket on the target box's left edge, at the field that uses the value
			auto *si = new QGraphicsEllipseItem(-4.5, w.dy - 4.5, 9, 9, b->item);
			si->setBrush(c);
			si->setPen(QPen(QColor(20, 20, 22), 1));
			// output socket on the source box's right edge, at its title
			auto *so = new QGraphicsEllipseItem(NG_W - 4.5, NG_TH / 2 - 4.5, 9, 9, a->item);
			so->setBrush(c);
			so->setPen(QPen(QColor(20, 20, 22), 1));
			w.path->setPen(QPen(c, 2.0));
		}
		else
			w.path->setPen(QPen(QColor(150, 150, 155), 3.0, Qt::SolidLine, Qt::RoundCap));   // paint-order link
		w.path->setZValue(0);                                       // under the boxes
		g->scene->addItem(w.path);
	}
	// ── layout: the program steps in one column, top to bottom; each value one column to the left of
	// the box that uses it, at that box's height, pushed down past whatever its column already holds.
	int maxCol = 0;
	for (const auto &b : g->boxes) maxCol = std::max(maxCol, b.col);
	const double x0 = maxCol * NG_COLW;
	double yc = 0.0;
	for (auto &b : g->boxes) {
		if (b.col != 0) continue;
		b.item->setPos(x0, yc);
		yc += b.h + NG_GAP;
	}
	std::vector<double> colY(maxCol + 1, 0.0);
	for (int c = 1; c <= maxCol; c++) {
		std::vector<NGBox *> in;
		for (auto &b : g->boxes) if (b.col == c) in.push_back(&b);
		auto ay = [g](NGBox *b) { NGBox *a = nodeGraphBox(g, b->anchor); return a && a->item ? a->item->pos().y() : 0.0; };
		std::stable_sort(in.begin(), in.end(), [&](NGBox *p, NGBox *q) { return ay(p) < ay(q); });
		for (NGBox *b : in) {
			const double y = std::max(ay(b), colY[c]);
			b->item->setPos(x0 - c * NG_COLW, y);
			colY[c] = y + b->h + NG_GAP;
		}
	}
	bool same = (int)was.size() == (int)g->boxes.size();
	for (const auto &b : g->boxes) same = same && was.contains(b.id) && was.value(b.id).first == b.title;
	if (same)
		for (auto &b : g->boxes) b.item->setPos(was.value(b.id).second);
	nodeGraphRoute(g);
	g->scene->setSceneRect(g->scene->itemsBoundingRect().adjusted(-200, -200, 200, 200));
}

// Fit the whole graph in the view, never zooming in past 1:1 (a three-box script stays readable size).
static void nodeGraphFit(NodeGraph *g) {
	const QRectF r = g->scene->itemsBoundingRect().adjusted(-30, -30, 30, 30);
	g->view->resetTransform();
	g->view->fitInView(r, Qt::KeepAspectRatio);
	if (g->view->transform().m11() > 1.0) g->view->resetTransform();
	g->view->centerOn(r.center());
}

// Ask Julia for something (open, refresh, edit), with the app's busy notice up while it works. A failure
// pops up: an action the user asked for never fails silently.
static bool nodeGraphEval(void *key, QWidget *parent, const QString &cmd, const char *busy) {
	if (!g_juliaEval) {
		QMessageBox::warning(parent, "Script graph", "The Julia bridge is not registered.");
		return false;
	}
	std::vector<char> buf(1 << 16);
	showBusyDialog(busy);
	const int n = g_juliaEval(key, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	closeBusyDialog();
	if (n < 0) {
		QMessageBox::warning(parent, "Script graph", QString::fromUtf8(buf.data(), -n).trimmed());
		return false;
	}
	return true;
}

static QString nodeGraphKeyLit(void *key) {
	return QString("Ptr{Nothing}(UInt(%1))").arg((qulonglong)(quintptr)key);
}

// The editor's Graph… button (and the graph's Refresh): Julia parses the editor's text and sends the graph
// back through gmtvtk_nodegraph_show_h, which builds or refreshes the window.
static void nodeGraphOpen(void *key, QWidget *parent) {
	if (!nodeGraphEval(key, parent, QString("InteractiveGMT._nodegraph_open(%1)").arg(nodeGraphKeyLit(key)),
	                   "Running the script to picture each step..."))
		return;
	// The graph was shown INSIDE the eval, under the busy notice; closing that notice hands activation
	// back to the window it came from (the script editor), which then stands over the graph. So the
	// graph is brought to the front here, once the notice is gone.
	if (NodeGraph *g = nodeGraphRegistry().value(key, nullptr))
		if (g->win->isVisible()) {
			g->win->raise();
			g->win->activateWindow();
		}
}

// Bring a parked graph back on screen.
static void nodeGraphUnpark(void *key) {
	NodeGraph *g = nodeGraphRegistry().value(key, nullptr);
	if (!g) return;
	if (sceneAlive(static_cast<Scene *>(key))) unparkTool(static_cast<Scene *>(key), g->win);
	else if (xyAlive(static_cast<XYPlot *>(key))) xyUnparkTool(static_cast<XYPlot *>(key), g->win);
	g->win->setWindowState(g->win->windowState() & ~Qt::WindowMinimized);
	g->win->showNormal();
	g->win->raise();
	g->win->activateWindow();
}

// The parked row's menu (properties button and context menu alike): Show, or Delete for good.
static std::function<void(const QPoint &)> nodeGraphParkedMenu(void *key) {
	return [key](const QPoint &gp) {
		NodeGraph *g = nodeGraphRegistry().value(key, nullptr);
		if (!g) return;
		QMenu m;
		m.addAction("Show", [key]() { nodeGraphUnpark(key); });
		m.addSeparator();
		m.addAction("Delete", [g]() { g->win->close(); });
		m.exec(gp);
	};
}

// Hide the graph and leave its handle in the owner's dock (Scene Objects / the X,Y Object Manager). An
// owner with neither closes it for real.
static void nodeGraphPark(void *key) {
	NodeGraph *g = nodeGraphRegistry().value(key, nullptr);
	if (!g) return;
	g->win->hide();
	const QString label = "GMT.jl script graph";
	if (sceneAlive(static_cast<Scene *>(key)))
		parkTool(static_cast<Scene *>(key), g->win, label, IC_Text,
		         "Parked GMT.jl script graph — double-click to bring it back, click for its menu",
		         [key]() { nodeGraphUnpark(key); }, nodeGraphParkedMenu(key));
	else if (xyAlive(static_cast<XYPlot *>(key)))
		xyParkTool(static_cast<XYPlot *>(key), g->win, label, [key]() { nodeGraphUnpark(key); },
		           nodeGraphParkedMenu(key));
	else
		g->win->close();
}

// Press one of the script editor's own buttons (Run, Save…): the graph has no runner or saver of its own.
static void nodeGraphPressEditor(NodeGraph *g, const QString &text) {
	QDialog *d = scriptEditorRegistry().value(g->key, nullptr);
	QPushButton *hit = nullptr;
	if (d)
		for (QPushButton *b : d->findChildren<QPushButton *>())
			if (b->text() == text) { hit = b; break; }
	if (!hit) {
		QMessageBox::warning(g->win, "Script graph", "The GMT.jl script editor of this window is gone.");
		return;
	}
	hit->click();
}

// A field's new value: let Julia rewrite that statement in the editor's text, which re-sends the graph.
static void nodeGraphCommit(NodeGraph *g, int stmt, const QString &key, const QString &val) {
	const QString cmd = QString("InteractiveGMT._nodegraph_edit(%1, %2, %3, %4)")
	                    .arg(nodeGraphKeyLit(g->key)).arg(stmt).arg(nodeGraphJlStr(key), nodeGraphJlStr(val));
	g->editing = true;                                           // the re-sent graph waits until we are out
	nodeGraphEval(g->key, g->win, cmd, "Updating the script...");
	g->editing = false;
	if (!g->pending.isEmpty()) {
		const QString blob = g->pending;
		g->pending.clear();
		QTimer::singleShot(0, g->win, [g, blob]() { nodeGraphBuild(g, blob); });
	}
}

// The in-place editor closes: Enter / clicking elsewhere keep the new value, Esc throws it away. Runs once
// per editor (hiding it moves the focus, which reports "finished" a second time).
static void nodeGraphInlineDone(NodeGraph *g, QLineEdit *le, bool keep) {
	if (!le || le->property("ngDone").toBool()) return;
	le->setProperty("ngDone", true);
	const int stmt = le->property("ngStmt").toInt();
	const QString key = le->property("ngKey").toString(), old = le->property("ngVal").toString();
	const QString nv = le->text().trimmed();
	le->hide();
	le->deleteLater();
	if (!keep || nv.isEmpty() || nv == old) return;
	// out of the line edit's own signal before the graph (and every row) is rebuilt
	QTimer::singleShot(0, g->win, [g, stmt, key, nv]() { nodeGraphCommit(g, stmt, key, nv); });
}

// A field row clicked: a line editor opens right over its value, text selected, ready to type over.
static void nodeGraphInlineEdit(NodeGraph *g, QGraphicsItem *row) {
	if (!nodeGraphEditable(row)) return;
	if (g->inl) nodeGraphInlineDone(g, g->inl, true);
	const QString val = row->data(NGD_VALUE).toString();
	const QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);   // the code reads as code
	QRect r = g->view->mapFromScene(row->mapToScene(QRectF(4, 0, NG_W - 8, NG_RH))).boundingRect();
	r.setWidth(std::max(r.width(), 360));                          // room for a tuple: may run past the box
	r.setHeight(std::max(r.height(), 22));
	// the in-place value editor (a line edit on the canvas, over the row)
	QLineEdit *le = new QLineEdit(g->view->viewport());
	le->setFont(f);
	le->setText(val);
	le->setGeometry(r);
	le->setStyleSheet("QLineEdit { background: #1d1d20; color: #f0f0f0; border: 1px solid #5c8fd6; "
	                  "selection-background-color: #3d6fb0; }");
	le->setToolTip("GMT.jl code for " + row->data(NGD_PLAIN).toString() + " — Enter keeps it, Esc cancels");
	le->setProperty("ngStmt", row->data(0));
	le->setProperty("ngKey", row->data(1));
	le->setProperty("ngVal", val);
	g->inl = le;
	le->installEventFilter(g->flt);                               // Esc cancels (and never parks the window)
	QObject::connect(le, &QLineEdit::editingFinished, le, [g, le]() { nodeGraphInlineDone(g, le, true); });
	le->show();
	le->setFocus();
	le->selectAll();
}

// `extra` (src/nodescript.jl _ng_pairs): name/value pairs, unit separator inside, record separator between.
static QList<QPair<QString, QString>> nodeGraphPairs(const QString &extra) {
	QList<QPair<QString, QString>> out;
	for (const QString &p : extra.split(QChar(0x1e), Qt::SkipEmptyParts)) {
		const int k = p.indexOf(QChar(0x1f));
		out.append(k < 0 ? qMakePair(p, QString()) : qMakePair(p.left(k), p.mid(k + 1)));
	}
	return out;
}
static QString nodeGraphPair(const QString &extra, const QString &name) {
	for (const auto &p : nodeGraphPairs(extra)) if (p.first == name) return p.second;
	return QString();
}

// A GMT colour ("red", "255/0/0", "#ff0000") as a QColor; black when it cannot be read.
static QColor nodeGraphColor(const QString &s) {
	const QStringList rgb = s.section('@', 0, 0).split('/');
	if (rgb.size() == 3) return QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt());
	const QColor c(s.section('@', 0, 0).trimmed());
	return c.isValid() ? c : QColor(Qt::black);
}
static QString nodeGraphRGB(const QColor &c) {
	return QString("%1/%2/%3").arg(c.red()).arg(c.green()).arg(c.blue());
}
static QString nodeGraphNum(double v) { return QString::number(v, 'g', 10); }

// Close the in-graph control panel, if one is open.
static void nodeGraphPanelClose(NodeGraph *g) {
	if (!g->panel) return;
	g->panel->hide();
	g->panel->deleteLater();
	g->panel = nullptr;
}

// A row of colour swatches (plus "More…" for any other colour). A click on one calls `pick` — no dialog.
static QWidget *nodeGraphSwatches(const QColor &cur, const std::function<void(const QColor &)> &pick) {
	static const char *cols[] = { "#000000", "#404040", "#808080", "#c0c0c0", "#ffffff", "#ff0000", "#ff8000",
	                              "#ffff00", "#80c000", "#008000", "#00c0c0", "#0080ff", "#0000ff", "#8000ff",
	                              "#ff00ff", "#804000" };
	QWidget *w = new QWidget();
	QGridLayout *gl = new QGridLayout(w);
	gl->setContentsMargins(0, 0, 0, 0);
	gl->setSpacing(2);
	for (int i = 0; i < 16; i++) {
		// one colour swatch; the current colour is outlined
		QToolButton *b = new QToolButton(w);
		const QColor c(cols[i]);
		b->setFixedSize(20, 20);
		b->setStyleSheet(QString("QToolButton { background:%1; border:%2; }")
		                 .arg(c.name(), c == cur ? "2px solid #5c8fd6" : "1px solid #202024"));
		b->setToolTip(nodeGraphRGB(c));
		QObject::connect(b, &QToolButton::clicked, w, [pick, c]() { pick(c); });
		gl->addWidget(b, i / 8, i % 8);
	}
	// any other colour: the full chooser
	QToolButton *more = new QToolButton(w);
	more->setText("More…");
	QObject::connect(more, &QToolButton::clicked, w, [w, cur, pick]() {
		const QColor c = QColorDialog::getColor(cur, w, "Choose a colour");
		if (c.isValid()) pick(c);
	});
	gl->addWidget(more, 0, 8, 2, 1);
	return w;
}

// A field row clicked: its control opens RIGHT THERE in the graph, under the row — a list, a number box,
// colour swatches, a line's thickness/colour/style, the map limits, the frame's parts — in plain words, and
// every change applies AT ONCE (the script line is rewritten, the pictures redrawn). No OK, no dialog asking
// anything. Esc or a click elsewhere closes it. Values the controls do not cover are edited as code in
// place, as is any field through the right-click.
static void nodeGraphFieldDialog(NodeGraph *g, QGraphicsItem *row) {
	if (!nodeGraphEditable(row)) return;
	nodeGraphPanelClose(g);
	const int stmt = row->data(NGD_STMT).toInt();
	const QString key = row->data(NGD_KEY).toString(), type = row->data(NGD_TYPE).toString();
	const QString plain = row->data(NGD_PLAIN).toString(), extra = row->data(NGD_EXTRA).toString();
	const QString shown = row->data(NGD_SHOWN).toString(), value = row->data(NGD_VALUE).toString();
	auto commit = [g, stmt, key](const QString &code) { nodeGraphCommit(g, stmt, key, code); };
	if (type == "bool") {                                          // Yes/No: a click flips it
		commit(extra == "1" ? "false" : "true");
		return;
	}
	static const QStringList known = { "choice", "number", "text", "file", "color", "pen", "nums", "frame" };
	if (!known.contains(type)) {                                   // anything else: as code, in place
		nodeGraphInlineEdit(g, row);
		return;
	}
	// the panel: a framed box on the canvas, under the row, with the field's name as its heading
	QFrame *p = new QFrame(g->view->viewport());
	p->setObjectName("nodeGraphPanel");
	p->setFrameShape(QFrame::Box);
	p->setAutoFillBackground(true);
	p->setStyleSheet("QFrame#nodeGraphPanel { background:#2c2c30; border:1px solid #5c8fd6; } "
	                 "QLabel, QCheckBox { color:#e8e8ec; }");
	QFormLayout *form = new QFormLayout(p);
	form->setContentsMargins(8, 6, 8, 8);
	QLabel *head = new QLabel("<b>" + plain.toHtmlEscaped() + "</b>", p);
	form->addRow(head);
	QWidget *focus = nullptr;
	if (type == "choice") {
		// the list of choices, opened at once: one click picks
		const auto pairs = nodeGraphPairs(extra);
		QComboBox *cb = new QComboBox(p);
		for (int i = 0; i < pairs.size(); i++) {
			cb->addItem(pairs[i].first, pairs[i].second);
			if (pairs[i].second == value || pairs[i].first == shown) cb->setCurrentIndex(i);
		}
		QObject::connect(cb, &QComboBox::activated, p, [g, cb, commit](int i) {
			// "Select CRS..." (nodescript.jl's _NG_CRS_PICK): the CRS selector Tools > Project opens, and
			// its pick becomes the value
			if (cb->itemData(i).toString() == "@crs") {
				crsPickAsync(g->win, QString(), [commit](const QString &id, const QString &) {
					if (!id.isEmpty()) commit(nodeGraphJlStr(crsGmtProj(id)));
				});
				return;
			}
			commit(cb->itemData(i).toString());
		});
		form->addRow(cb);
		focus = cb;
		QTimer::singleShot(0, cb, [cb]() { cb->showPopup(); });
	}
	else if (type == "number") {
		// the number, typed or stepped with the arrows; applied when Enter is pressed or the box is left
		QDoubleSpinBox *sb = new QDoubleSpinBox(p);
		sb->setRange(-1e12, 1e12);
		sb->setDecimals(4);
		sb->setValue(extra.toDouble());
		QObject::connect(sb, &QDoubleSpinBox::editingFinished, p, [sb, commit]() { commit(nodeGraphNum(sb->value())); });
		form->addRow(sb);
		focus = sb;
	}
	else if (type == "text" || type == "file") {
		// the text (or the file's path), applied on Enter
		QLineEdit *le = new QLineEdit(extra, p);
		le->setMinimumWidth(260);
		QObject::connect(le, &QLineEdit::returnPressed, p, [le, commit]() { commit(nodeGraphJlStr(le->text())); });
		form->addRow(le);
		focus = le;
		if (type == "file") {
			// another file, chosen in the file browser
			QPushButton *br = new QPushButton("Choose another file…", p);
			QObject::connect(br, &QPushButton::clicked, p, [p, le, commit]() {
				const QString f = QFileDialog::getOpenFileName(p, "Choose the file", le->text());
				if (!f.isEmpty()) commit(nodeGraphJlStr(QDir::toNativeSeparators(f)));
			});
			form->addRow(br);
		}
	}
	else if (type == "color") {
		form->addRow(nodeGraphSwatches(nodeGraphColor(extra), [commit](const QColor &c) {
			commit(nodeGraphJlStr(nodeGraphRGB(c)));
		}));
	}
	else if (type == "pen") {
		// a line: thickness, colour and style; each change applies at once
		QDoubleSpinBox *w = new QDoubleSpinBox(p);
		w->setRange(0.05, 50.0);
		w->setDecimals(2);
		w->setSingleStep(0.25);
		const double w0 = nodeGraphPair(extra, "width").toDouble();
		w->setValue(w0 > 0 ? w0 : 1.0);
		w->setSuffix(" pt");
		QComboBox *st = new QComboBox(p);
		st->addItems({ "Solid", "Dashed", "Dotted" });
		const QString s0 = nodeGraphPair(extra, "style");
		st->setCurrentIndex(s0 == "dashed" ? 1 : s0 == "dotted" ? 2 : 0);
		auto col = std::make_shared<QColor>(nodeGraphColor(nodeGraphPair(extra, "colour")));
		auto pen = [w, st, col, commit]() {
			QString s = nodeGraphNum(w->value()) + "p," + nodeGraphRGB(*col);
			if (st->currentIndex() == 1) s += ",-";
			if (st->currentIndex() == 2) s += ",.";
			commit(nodeGraphJlStr(s));
		};
		QObject::connect(w, &QDoubleSpinBox::editingFinished, p, pen);
		QObject::connect(st, &QComboBox::activated, p, pen);
		form->addRow("Thickness:", w);
		form->addRow("Colour:", nodeGraphSwatches(*col, [col, pen](const QColor &c) { *col = c; pen(); }));
		form->addRow("Style:", st);
		focus = w;
	}
	else if (type == "nums") {
		// one box per number (West, East, …); applied when Enter is pressed or a box is left
		const auto pairs = nodeGraphPairs(extra);
		auto eds = std::make_shared<QList<QLineEdit *>>();
		auto apply = [eds, commit]() {
			QStringList v;
			for (QLineEdit *e : *eds) v << nodeGraphNum(e->text().toDouble());
			commit("(" + v.join(", ") + (v.size() == 1 ? "," : "") + ")");
		};
		for (const auto &pr : pairs) {
			QLineEdit *e = new QLineEdit(pr.second, p);
			e->setValidator(new QDoubleValidator(e));
			QObject::connect(e, &QLineEdit::editingFinished, p, [e, apply]() {
				if (!e->isModified()) return;                      // leaving an untouched box changes nothing
				e->setModified(false);
				apply();
			});
			form->addRow(pr.first + ":", e);
			eds->append(e);
			if (!focus) focus = e;
		}
	}
	else if (type == "frame") {
		// which sides, numbers / ticks / grid, and a title; each change applies at once
		static const char *codes[] = { "WSen", "WSEN", "WS", "wsen", "none" };
		QComboBox *sides = new QComboBox(p);
		sides->addItems({ "Numbers left and bottom, box all round", "Numbers on all four sides",
		                  "Left and bottom only", "Box only, no numbers", "No frame at all" });
		const QString a0 = nodeGraphPair(extra, "axes");
		for (int i = 0; i < 5; i++) if (a0 == codes[i]) sides->setCurrentIndex(i);
		QCheckBox *an = new QCheckBox("Numbers along the axes", p);
		an->setChecked(nodeGraphPair(extra, "annot") == "1");
		QCheckBox *tk = new QCheckBox("Tick marks", p);
		tk->setChecked(nodeGraphPair(extra, "ticks") == "1");
		QCheckBox *gr = new QCheckBox("Grid lines over the map", p);
		gr->setChecked(nodeGraphPair(extra, "grid") == "1");
		QLineEdit *title = new QLineEdit(nodeGraphPair(extra, "title"), p);
		title->setPlaceholderText("no title");
		auto apply = [sides, an, tk, gr, title, commit]() {
			if (sides->currentIndex() == 4) { commit(":none"); return; }
			QStringList v;
			if (an->isChecked()) v << "annot=:auto";
			if (tk->isChecked()) v << "ticks=:auto";
			if (gr->isChecked()) v << "grid=:auto";
			v << QString("axes=:%1").arg(codes[sides->currentIndex()]);
			if (!title->text().trimmed().isEmpty()) v << "title=" + nodeGraphJlStr(title->text().trimmed());
			commit("(" + v.join(", ") + (v.size() == 1 ? "," : "") + ")");
		};
		QObject::connect(sides, &QComboBox::activated, p, apply);
		for (QCheckBox *c : { an, tk, gr }) QObject::connect(c, &QCheckBox::clicked, p, apply);
		QObject::connect(title, &QLineEdit::returnPressed, p, apply);
		form->addRow("Sides:", sides);
		form->addRow(an);
		form->addRow(tk);
		form->addRow(gr);
		form->addRow("Title:", title);
		focus = sides;
	}
	// placed right under the row, kept inside the view
	p->adjustSize();
	const QRect r = g->view->mapFromScene(row->sceneBoundingRect()).boundingRect();
	QPoint at(r.left(), r.bottom() + 2);
	const QSize vs = g->view->viewport()->size();
	at.setX(std::max(0, std::min(at.x(), vs.width() - p->width())));
	if (at.y() + p->height() > vs.height()) at.setY(std::max(0, r.top() - p->height() - 2));
	p->move(at);
	p->installEventFilter(g->flt);                                 // Esc closes it (and never parks the window)
	g->panel = p;
	p->show();
	p->raise();
	if (focus) focus->setFocus();
}

// The view's mouse and keys: wheel zooms under the cursor, a click on a field row edits it in place, F fits;
// on the window, Esc parks it (and is consumed: left to QDialog it would reject = destroy).
struct NodeGraphFilter : QObject {
	NodeGraph *g;
	NodeGraphFilter(QObject *parent, NodeGraph *ng) : QObject(parent), g(ng) {}
	bool eventFilter(QObject *o, QEvent *e) override {
		if (o == g->view->viewport() && e->type() == QEvent::Wheel) {
			const double f = std::pow(1.15, static_cast<QWheelEvent *>(e)->angleDelta().y() / 120.0);
			const double z = g->view->transform().m11() * f;
			if (z > 0.05 && z < 4.0) g->view->scale(f, f);
			return true;
		}
		if (o == g->inl && e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
			nodeGraphInlineDone(g, g->inl, false);
			return true;
		}
		if (o == g->panel && e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
			nodeGraphPanelClose(g);                                 // Esc closes the panel, never parks the window
			return true;
		}
		if (o == g->view->viewport() && (e->type() == QEvent::MouseButtonPress ||
		                                 e->type() == QEvent::MouseButtonDblClick)) {
			nodeGraphPanelClose(g);                                 // a click elsewhere on the canvas closes it
			// remember which field row the press is on; the drag (pan, move a box) goes on as usual
			auto *me = static_cast<QMouseEvent *>(e);
			g->pressRow = nullptr;
			if (me->button() == Qt::LeftButton)
				for (QGraphicsItem *it : g->view->items(me->pos()))
					if (nodeGraphEditable(it)) { g->pressRow = it; g->pressAt = me->pos(); break; }
			return false;
		}
		if (o == g->view->viewport() && e->type() == QEvent::MouseButtonRelease) {
			// a press and release on the same spot of an editable row is a CLICK: edit that value in place
			auto *me = static_cast<QMouseEvent *>(e);
			QGraphicsItem *row = g->pressRow;
			g->pressRow = nullptr;
			if (row && me->button() == Qt::LeftButton && (me->pos() - g->pressAt).manhattanLength() < 5) {
				NodeGraph *ng = g;
				QTimer::singleShot(0, g->win, [ng, row]() {
					if (ng->scene->items().contains(row)) nodeGraphFieldDialog(ng, row);
				});
			}
			return false;
		}
		if (o == g->view->viewport() && e->type() == QEvent::ContextMenu) {
			// right-click on a field: change it (its dialog) or, for those who want it, edit its GMT.jl code
			auto *ce = static_cast<QContextMenuEvent *>(e);
			QGraphicsItem *row = nullptr;
			for (QGraphicsItem *it : g->view->items(ce->pos()))
				if (nodeGraphEditable(it)) { row = it; break; }
			if (!row) return false;
			QMenu m;
			m.addAction("Change…", [this, row]() { nodeGraphFieldDialog(g, row); });
			m.addAction("Edit as GMT.jl code", [this, row]() { nodeGraphInlineEdit(g, row); });
			m.exec(ce->globalPos());
			return true;
		}
		if (e->type() == QEvent::KeyPress) {
			const int k = static_cast<QKeyEvent *>(e)->key();
			if (k == Qt::Key_Escape) { nodeGraphPark(g->key); return true; }
			if (k == Qt::Key_F && o == g->view) { nodeGraphFit(g); return true; }
		}
		return QObject::eventFilter(o, e);
	}
};

// The graph window for `key`, created on first use.
static NodeGraph *nodeGraphWindow(void *key, QWidget *parent) {
	if (NodeGraph *g = nodeGraphRegistry().value(key, nullptr)) return g;
	NodeGraph *g = new NodeGraph;
	g->key = key;
	g->win = new QDialog(parent);
	g->win->setWindowTitle("GMT.jl script graph");
	g->win->setObjectName("nodeGraphWindow");
	g->win->setAttribute(Qt::WA_DeleteOnClose);
	// WindowMinimizeButtonHint puts the MINIMISE button in the title bar (minimise parks the graph)
	g->win->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
	nodeGraphRegistry().insert(key, g);
	QObject::connect(g->win, &QObject::destroyed, [key, g]() {   // the X: the window and its row go for good
		nodeGraphRegistry().remove(key);
		if (sceneAlive(static_cast<Scene *>(key))) unparkTool(static_cast<Scene *>(key), g->win);
		delete g;
	});
	QVBoxLayout *lay = new QVBoxLayout(g->win);
	lay->setContentsMargins(6, 6, 6, 6);
	// the toolbar row: Run / Save… (the editor's own), Refresh (re-read the editor's text), Fit
	QHBoxLayout *row = new QHBoxLayout();
	QPushButton *bRun = new QPushButton("&Run", g->win);
	bRun->setToolTip("Run the script — the same Run as the script editor's");
	QPushButton *bSave = new QPushButton("&Save…", g->win);
	bSave->setToolTip("Save the script — the same Save… as the script editor's");
	QPushButton *bRefresh = new QPushButton("Re&fresh", g->win);
	bRefresh->setToolTip("Rebuild the graph from the script editor's current text (after editing the text there)");
	QPushButton *bFit = new QPushButton("Fi&t", g->win);
	bFit->setToolTip("Show the whole graph (F)");
	bRun->setObjectName("graphRun");
	bSave->setObjectName("graphSave");
	bRefresh->setObjectName("graphRefresh");
	bFit->setObjectName("graphFit");
	for (QPushButton *b : { bRun, bSave, bRefresh, bFit }) b->setAutoDefault(false);
	// the hint: how the view is driven
	QLabel *hint = new QLabel("Click a value to change it (right-click: edit as code) · wheel zooms · "
	                          "drag the background to pan, a box to move it", g->win);
	hint->setStyleSheet("color: gray;");
	row->addWidget(bRun);
	row->addWidget(bSave);
	row->addSpacing(12);
	row->addWidget(bRefresh);
	row->addWidget(bFit);
	row->addSpacing(12);
	row->addWidget(hint, 1);
	lay->addLayout(row);
	// the canvas: a dark, gridded Blender-like node area
	g->scene = new QGraphicsScene(g->win);
	g->view = new QGraphicsView(g->scene, g->win);
	g->view->setRenderHint(QPainter::Antialiasing);
	g->view->setRenderHint(QPainter::TextAntialiasing);
	g->view->setDragMode(QGraphicsView::ScrollHandDrag);
	g->view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
	g->view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);   // wires redrawn whole while dragging
	{
		QPixmap tile(24, 24);
		tile.fill(QColor(38, 38, 40));
		QPainter p(&tile);
		p.setPen(QColor(46, 46, 49));
		p.drawLine(0, 0, 23, 0);
		p.drawLine(0, 0, 0, 23);
		g->view->setBackgroundBrush(QBrush(tile));
	}
	lay->addWidget(g->view, 1);
	g->win->resize(1000, 700);
	// a moved box drags its wires along: re-route whenever a box is not where it was routed from
	QObject::connect(g->scene, &QGraphicsScene::changed, g->win, [g]() {
		for (const auto &b : g->boxes)
			if (b.item && b.item->pos() != b.lastPos) { nodeGraphRoute(g); return; }
	});
	QObject::connect(bRun, &QPushButton::clicked, g->win, [g]() { nodeGraphPressEditor(g, "&Run"); });
	QObject::connect(bSave, &QPushButton::clicked, g->win, [g]() { nodeGraphPressEditor(g, "&Save…"); });
	QObject::connect(bRefresh, &QPushButton::clicked, g->win, [g]() { nodeGraphOpen(g->key, g->win); });
	QObject::connect(bFit, &QPushButton::clicked, g->win, [g]() { nodeGraphFit(g); });
	NodeGraphFilter *flt = new NodeGraphFilter(g->win, g);
	g->flt = flt;
	g->win->installEventFilter(flt);
	g->view->installEventFilter(flt);
	g->view->viewport()->installEventFilter(flt);
	parkOnMinimise(g->win, [key]() { nodeGraphPark(key); });
	// the graph is a view of the editor's text: when the editor is destroyed, the graph goes with it
	if (QDialog *ed = scriptEditorRegistry().value(key, nullptr))
		QObject::connect(ed, &QObject::destroyed, g->win, [g]() { g->win->close(); });
	wrapTooltips(g->win);
	return g;
}

// Called (through gmtvtk_nodegraph_show_h) with the graph Julia parsed: build it and bring it up.
static void nodeGraphShow(void *key, QWidget *parent, const QString &blob) {
	NodeGraph *g = nodeGraphWindow(key, parent);
	if (g->editing) {                                             // a field edit's refresh: built after it returns
		g->pending = blob;
		return;
	}
	nodeGraphBuild(g, blob);
	nodeGraphUnpark(key);
	// the whole graph in view, once the window is laid out and the view has its real size
	if (!g->fitted) {
		g->fitted = true;
		QTimer::singleShot(100, g->win, [g]() { nodeGraphFit(g); });
	}
}
