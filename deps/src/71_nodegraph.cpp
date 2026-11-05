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
#include <QSplitter>
#include <QListWidget>
#include <QMimeData>
#include <QPainterPathStroker>
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
	int stmt = 0;                      // the statement that made it (a step is added after / removed by it)
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

// A module the graph offers to add (nodescript.jl _NG_MODULES): its key, what it is in words, its menu
// group, and the kinds of data box it draws from ("grid,image", "table"; empty = nothing).
struct NGModuleDef {
	QString key, plain, group, input;
};

// A data box a new step can draw from: its variable, what it holds ("?" = not known), its box title.
struct NGVar {
	QString var, kind, title;
};

// The drag payload of a module dragged out of the palette.
static const char *NG_MIME = "application/x-igmt-ngmodule";

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
	std::vector<NGModuleDef> modules;  // what can be added (sent with every graph)
	std::vector<NGVar> vars;           // what a new step can draw from
	QListWidget *palette = nullptr;    // the module palette: dragged onto the canvas, or double-clicked
	int dropWire = -1;                 // the program link a dragged module would land on (highlighted)
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

// The palette: the modules Julia offers, under their group headings. A module item is dragged onto the
// canvas (or double-clicked); a heading is neither.
static void nodeGraphFillPalette(NodeGraph *g) {
	if (!g->palette) return;
	g->palette->clear();
	QString group;
	for (const auto &m : g->modules) {
		if (m.group != group) {
			group = m.group;
			// the group heading
			auto *h = new QListWidgetItem(group, g->palette);
			QFont f = h->font();
			f.setBold(true);
			h->setFont(f);
			h->setFlags(Qt::ItemIsEnabled);
		}
		// one module: dragged onto the graph to add it
		auto *it = new QListWidgetItem("   " + m.plain, g->palette);
		it->setData(Qt::UserRole, m.key);
		it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
		it->setToolTip("Drag onto the graph: dropped on the line between two steps it goes there, on a step it "
		               "goes after it, anywhere else at the end. Double-click: add it at the end");
	}
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
	g->modules.clear();
	g->vars.clear();
	g->dropWire = -1;
	for (const QString &line : blob.split('\n', Qt::SkipEmptyParts)) {
		const QStringList t = line.split('\t');
		if (t[0] == "N" && t.size() >= 7) {
			NGBox b;
			b.id = t[1].toInt();  b.kind = t[2];  b.col = t[3].toInt();  b.anchor = t[4].toInt();
			b.title = t[5];  b.note = t[6];
			if (t.size() >= 8) b.stmt = t[7].toInt();
			g->boxes.push_back(b);
		}
		else if (t[0] == "M" && t.size() >= 5)
			g->modules.push_back({ t[1], t[2], t[3], t[4] });
		else if (t[0] == "V" && t.size() >= 4)
			g->vars.push_back({ t[1], t[2], t[3] });
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
	nodeGraphFillPalette(g);
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

// A change to the script (a field's value, a step added or removed): Julia rewrites the editor's text,
// which re-sends the graph — built once we are out of the call that asked.
static void nodeGraphApply(NodeGraph *g, const QString &cmd, const char *busy) {
	g->editing = true;                                           // the re-sent graph waits until we are out
	nodeGraphEval(g->key, g->win, cmd, busy);
	g->editing = false;
	if (!g->pending.isEmpty()) {
		const QString blob = g->pending;
		g->pending.clear();
		QTimer::singleShot(0, g->win, [g, blob]() { nodeGraphBuild(g, blob); });
	}
}

// A field's new value: let Julia rewrite that statement in the editor's text, which re-sends the graph.
static void nodeGraphCommit(NodeGraph *g, int stmt, const QString &key, const QString &val) {
	const QString cmd = QString("InteractiveGMT._nodegraph_edit(%1, %2, %3, %4)")
	                    .arg(nodeGraphKeyLit(g->key)).arg(stmt).arg(nodeGraphJlStr(key), nodeGraphJlStr(val));
	nodeGraphApply(g, cmd, "Updating the script...");
}

// Add module `mkey` to the script, placed `where` ("after" / "before" statement `stmt`, or "end"). A module
// that draws from data takes it from `var` when given; else from the one data box that holds the right
// kind, or — several of them — the one picked in a menu at `gp`, or a file chosen in the file browser.
static void nodeGraphAdd(NodeGraph *g, const QString &mkey, int stmt, const QString &where, const QPoint &gp,
                         const QString &var = QString()) {
	QString plain, input;
	for (const auto &m : g->modules)
		if (m.key == mkey) { plain = m.plain; input = m.input; }
	if (plain.isEmpty()) return;
	auto run = [g, mkey, stmt, where](const QString &v, const QString &file) {
		const QString cmd = QString("InteractiveGMT._nodegraph_add(%1, %2, %3, %4, %5, %6)")
		                    .arg(nodeGraphKeyLit(g->key)).arg(stmt)
		                    .arg(nodeGraphJlStr(where), nodeGraphJlStr(mkey), nodeGraphJlStr(v), nodeGraphJlStr(file));
		nodeGraphApply(g, cmd, "Adding the step to the script...");
	};
	if (input.isEmpty() || !var.isEmpty()) {
		run(var, QString());
		return;
	}
	auto openFile = [g, plain, run]() {
		const QString f = QFileDialog::getOpenFileName(g->win, "Data for " + plain);
		if (!f.isEmpty()) run(QString(), QDir::toNativeSeparators(f));
	};
	const QStringList kinds = input.split(',');
	QList<NGVar> cand;
	for (const auto &v : g->vars)
		if (v.kind == "?" || kinds.contains(v.kind)) cand.append(v);
	if (cand.isEmpty()) {
		openFile();
		return;
	}
	if (cand.size() == 1) {
		run(cand[0].var, QString());
		return;
	}
	// several boxes it could draw from: which one
	QMenu m;
	m.addSection(plain + " — draw from");
	for (const auto &v : cand) {
		const QString name = v.var;
		m.addAction(v.title, [run, name]() { run(name, QString()); });
	}
	m.addSeparator();
	m.addAction("Open a file…", openFile);
	m.exec(gp);
}

// Remove the step of statement `stmt` (and every value only it used) from the script.
static void nodeGraphRemove(NodeGraph *g, int stmt) {
	const QString cmd = QString("InteractiveGMT._nodegraph_remove(%1, %2)").arg(nodeGraphKeyLit(g->key)).arg(stmt);
	nodeGraphApply(g, cmd, "Removing the step from the script...");
}

// The modules, one submenu per group, into `menu`. Picking one adds it `where` statement `stmt` — after
// the menu has closed, as its data may need a second menu or the file browser. `kind` non-empty: only the
// modules that draw from that kind of data, drawing from `var`.
static void nodeGraphModuleMenu(NodeGraph *g, QMenu *menu, int stmt, const QString &where, const QPoint &gp,
                                const QString &kind = QString(), const QString &var = QString()) {
	QHash<QString, QMenu *> groups;
	for (const auto &m : g->modules) {
		if (!kind.isEmpty() && !(kind == "?" ? !m.input.isEmpty() : m.input.split(',').contains(kind))) continue;
		QMenu *sub = groups.value(m.group, nullptr);
		if (!sub) groups.insert(m.group, sub = menu->addMenu(m.group));
		const QString key = m.key;
		sub->addAction(m.plain, [g, key, stmt, where, gp, var]() {
			QTimer::singleShot(0, g->win, [g, key, stmt, where, gp, var]() { nodeGraphAdd(g, key, stmt, where, gp, var); });
		});
	}
}

// The box under view point `pos` (a click on its title, a row, its picture), or null.
static NGBox *nodeGraphBoxAt(NodeGraph *g, const QPoint &pos) {
	for (QGraphicsItem *it : g->view->items(pos)) {
		QGraphicsItem *top = it->topLevelItem();
		for (auto &b : g->boxes) if (b.item == top) return &b;
	}
	return nullptr;
}

// A right-click on a box: add a step after it (before the figure is shown, on the output box); on a data
// box, draw it with a module; remove a drawing step, or a value nothing uses.
static void nodeGraphBoxMenu(NodeGraph *g, NGBox *b, const QPoint &gp) {
	QMenu m;
	const int stmt = b->stmt, id = b->id;
	if (b->kind == "data") {
		for (const auto &v : g->vars)
			if (v.title == b->title)
				nodeGraphModuleMenu(g, m.addMenu("Draw this with"), 0, "end", gp, v.kind, v.var);
	}
	else if (b->kind == "output")
		nodeGraphModuleMenu(g, m.addMenu("Add a step before"), stmt, "before", gp);
	else if (b->kind != "palette")
		nodeGraphModuleMenu(g, m.addMenu("Add a step after"), stmt, "after", gp);
	bool used = false;
	for (const auto &w : g->wires) used = used || (!w.link && w.from == id);
	if (b->kind == "call" || ((b->kind == "data" || b->kind == "palette") && !used)) {
		m.addSeparator();
		m.addAction(b->kind == "call" ? "Remove this step" : "Remove this value", [g, stmt]() {
			QTimer::singleShot(0, g->win, [g, stmt]() { nodeGraphRemove(g, stmt); });
		});
	}
	if (!m.isEmpty()) m.exec(gp);
}

// Where a module dropped at view point `pos` goes: on a step, after it (before it, on the output box); on
// the line between two steps, after the upper one; anywhere else, at the end. `wire` = the link it is on.
static void nodeGraphDropPlace(NodeGraph *g, const QPoint &pos, int &stmt, QString &where, int &wire) {
	stmt = 0;
	where = "end";
	wire = -1;
	if (NGBox *b = nodeGraphBoxAt(g, pos)) {
		if (b->kind == "output") { stmt = b->stmt; where = "before"; }
		else if (b->kind != "data" && b->kind != "palette") { stmt = b->stmt; where = "after"; }
		return;
	}
	const QPointF sp = g->view->mapToScene(pos);
	QPainterPathStroker sk;
	sk.setWidth(16.0 / std::max(0.05, g->view->transform().m11()));   // 16 px wide on screen at any zoom
	for (int i = 0; i < (int)g->wires.size(); i++) {
		const NGWire &w = g->wires[i];
		if (!w.link || !w.path || !sk.createStroke(w.path->path()).contains(sp)) continue;
		if (NGBox *a = nodeGraphBox(g, w.from)) {
			stmt = a->stmt;
			where = "after";
			wire = i;
		}
		return;
	}
}

// The link a dragged module would land on, highlighted (the others drawn as usual).
static void nodeGraphDropHighlight(NodeGraph *g, int wire) {
	if (wire == g->dropWire) return;
	for (int i : { g->dropWire, wire })
		if (i >= 0 && i < (int)g->wires.size() && g->wires[i].path)
			g->wires[i].path->setPen(i == wire ? QPen(QColor(92, 143, 214), 5.0, Qt::SolidLine, Qt::RoundCap)
			                                   : QPen(QColor(150, 150, 155), 3.0, Qt::SolidLine, Qt::RoundCap));
	g->dropWire = wire;
}

// The palette: its items carry the module's key as the drag payload.
struct NodeGraphPalette : QListWidget {
	using QListWidget::QListWidget;
	QStringList mimeTypes() const override { return { NG_MIME }; }
	QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override {
		auto *md = new QMimeData();
		if (!items.isEmpty()) md->setData(NG_MIME, items[0]->data(Qt::UserRole).toString().toUtf8());
		return md;
	}
};

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
		// which sides, numbers / ticks / grid, a title and the X / Y labels; each change applies at once.
		// A COLOUR BAR's frame (bar=1) has no sides, grid or title of its own: its form is the numbers,
		// the ticks and the two labels — X along the bar, Y at its top (GMT colorbar -Bx+l / -By+l).
		const bool bar = nodeGraphPair(extra, "bar") == "1";
		static const char *codes[] = { "WSen", "WSEN", "WS", "wsen", "none" };
		QComboBox *sides = new QComboBox(p);
		sides->addItems({ "Numbers left and bottom, box all round", "Numbers on all four sides",
		                  "Left and bottom only", "Box only, no numbers", "No frame at all" });
		const QString a0 = nodeGraphPair(extra, "axes");
		for (int i = 0; i < 5; i++) if (a0 == codes[i]) sides->setCurrentIndex(i);
		QCheckBox *an = new QCheckBox(bar ? "Numbers along the bar" : "Numbers along the axes", p);
		an->setChecked(nodeGraphPair(extra, "annot") == "1");
		QCheckBox *tk = new QCheckBox("Tick marks", p);
		tk->setChecked(nodeGraphPair(extra, "ticks") == "1");
		QCheckBox *gr = new QCheckBox("Grid lines over the map", p);
		gr->setChecked(nodeGraphPair(extra, "grid") == "1");
		QLineEdit *title = new QLineEdit(nodeGraphPair(extra, "title"), p);
		title->setPlaceholderText("no title");
		QLineEdit *xl = new QLineEdit(nodeGraphPair(extra, "xlabel"), p);
		xl->setPlaceholderText(bar ? "none (written along the bar)" : "none");
		QLineEdit *yl = new QLineEdit(nodeGraphPair(extra, "ylabel"), p);
		yl->setPlaceholderText(bar ? "none (written over its top)" : "none");
		auto apply = [bar, sides, an, tk, gr, title, xl, yl, commit]() {
			if (!bar && sides->currentIndex() == 4) { commit(":none"); return; }
			QStringList v;
			if (an->isChecked()) v << "annot=:auto";
			if (tk->isChecked()) v << "ticks=:auto";
			if (!bar && gr->isChecked()) v << "grid=:auto";
			if (!bar) v << QString("axes=:%1").arg(codes[sides->currentIndex()]);
			if (!bar && !title->text().trimmed().isEmpty()) v << "title=" + nodeGraphJlStr(title->text().trimmed());
			if (!xl->text().trimmed().isEmpty()) v << "xlabel=" + nodeGraphJlStr(xl->text().trimmed());
			if (!yl->text().trimmed().isEmpty()) v << "ylabel=" + nodeGraphJlStr(yl->text().trimmed());
			if (v.isEmpty()) v << "annot=:auto";                   // a bar with nothing asked still numbers itself
			commit("(" + v.join(", ") + (v.size() == 1 ? "," : "") + ")");
		};
		QObject::connect(sides, &QComboBox::activated, p, apply);
		for (QCheckBox *c : { an, tk, gr }) QObject::connect(c, &QCheckBox::clicked, p, apply);
		for (QLineEdit *e : { title, xl, yl }) QObject::connect(e, &QLineEdit::returnPressed, p, apply);
		if (!bar) form->addRow("Sides:", sides);
		form->addRow(an);
		form->addRow(tk);
		if (!bar) {
			form->addRow(gr);
			form->addRow("Title:", title);
		}
		else {
			sides->hide();
			gr->hide();
			title->hide();
		}
		form->addRow("X label:", xl);
		form->addRow("Y label:", yl);
		focus = bar ? static_cast<QWidget *>(xl) : static_cast<QWidget *>(sides);
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
	// The view's viewport, guarded: while the window deletes its children it still sends this filter events
	// (ChildRemoved) after the view is gone, so `g->view->viewport()` there would read a freed view.
	QPointer<QWidget> vp;
	NodeGraphFilter(QObject *parent, NodeGraph *ng) : QObject(parent), g(ng), vp(ng->view->viewport()) {}
	bool eventFilter(QObject *o, QEvent *e) override {
		const QObject *port = vp.data();
		if (!port) return QObject::eventFilter(o, e);             // the view is gone: the window is going too
		if (o == port && e->type() == QEvent::Wheel) {
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
		if (o == port && (e->type() == QEvent::MouseButtonPress ||
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
		if (o == port && e->type() == QEvent::MouseButtonRelease) {
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
		if (o == port && e->type() == QEvent::ContextMenu) {
			// right-click on a field: change it (its dialog) or, for those who want it, edit its GMT.jl code
			auto *ce = static_cast<QContextMenuEvent *>(e);
			QGraphicsItem *row = nullptr;
			for (QGraphicsItem *it : g->view->items(ce->pos()))
				if (nodeGraphEditable(it)) { row = it; break; }
			if (!row) {
				// anywhere else on a box: the box's own menu (add a step, remove it)
				NGBox *b = nodeGraphBoxAt(g, ce->pos());
				if (!b) return false;
				nodeGraphBoxMenu(g, b, ce->globalPos());
				return true;
			}
			QMenu m;
			m.addAction("Change…", [this, row]() { nodeGraphFieldDialog(g, row); });
			m.addAction("Edit as GMT.jl code", [this, row]() { nodeGraphInlineEdit(g, row); });
			m.exec(ce->globalPos());
			return true;
		}
		// a module dragged out of the palette: the link it would land on lights up; dropped, it is added there
		if (o == port && (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove)) {
			auto *de = static_cast<QDragMoveEvent *>(e);
			if (!de->mimeData()->hasFormat(NG_MIME)) return false;
			int stmt, wire;
			QString where;
			nodeGraphDropPlace(g, de->position().toPoint(), stmt, where, wire);
			nodeGraphDropHighlight(g, wire);
			de->acceptProposedAction();
			return true;
		}
		if (o == port && e->type() == QEvent::DragLeave) {
			nodeGraphDropHighlight(g, -1);
			return false;
		}
		if (o == port && e->type() == QEvent::Drop) {
			auto *de = static_cast<QDropEvent *>(e);
			if (!de->mimeData()->hasFormat(NG_MIME)) return false;
			int stmt, wire;
			QString where;
			const QPoint at = de->position().toPoint();
			nodeGraphDropPlace(g, at, stmt, where, wire);
			nodeGraphDropHighlight(g, -1);
			de->acceptProposedAction();
			const QString key = QString::fromUtf8(de->mimeData()->data(NG_MIME));
			const QPoint gp = g->view->viewport()->mapToGlobal(at);
			NodeGraph *ng = g;
			// out of the drag before anything is asked or rebuilt
			QTimer::singleShot(0, g->win, [ng, key, stmt, where, gp]() { nodeGraphAdd(ng, key, stmt, where, gp); });
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
	// add a module at the end of the script (before the figure is shown); its menu is built when opened,
	// from the modules the last graph was sent with
	QPushButton *bAdd = new QPushButton("&Add step", g->win);
	bAdd->setToolTip("Add a module (contours, coastlines, text…) at the end of the script. To place it "
	                 "elsewhere: right-click a step, or drag a module from the list on the left onto the graph");
	QMenu *addMenu = new QMenu(bAdd);
	bAdd->setMenu(addMenu);
	QObject::connect(addMenu, &QMenu::aboutToShow, addMenu, [g, addMenu]() {
		addMenu->clear();
		nodeGraphModuleMenu(g, addMenu, 0, "end", QCursor::pos());
	});
	bRun->setObjectName("graphRun");
	bSave->setObjectName("graphSave");
	bRefresh->setObjectName("graphRefresh");
	bFit->setObjectName("graphFit");
	bAdd->setObjectName("graphAdd");
	for (QPushButton *b : { bRun, bSave, bRefresh, bFit, bAdd }) b->setAutoDefault(false);
	// the hint: how the view is driven
	QLabel *hint = new QLabel("Click a value to change it (right-click: edit as code; on a step: add or remove "
	                          "one) · wheel zooms · drag the background to pan, a box to move it", g->win);
	hint->setStyleSheet("color: gray;");
	row->addWidget(bRun);
	row->addWidget(bSave);
	row->addSpacing(12);
	row->addWidget(bAdd);
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
	g->view->viewport()->setAcceptDrops(true);                      // modules dropped from the palette
	// the module palette, left of the canvas: drag a module onto the graph, or double-click it
	g->palette = new NodeGraphPalette(g->win);
	g->palette->setObjectName("graphPalette");
	g->palette->setDragEnabled(true);
	g->palette->setDragDropMode(QAbstractItemView::DragOnly);
	g->palette->setSelectionMode(QAbstractItemView::SingleSelection);
	g->palette->setToolTip("Modules to add: drag one onto the graph, or double-click it to add it at the end");
	QObject::connect(g->palette, &QListWidget::itemDoubleClicked, g->win, [g](QListWidgetItem *it) {
		const QString key = it->data(Qt::UserRole).toString();
		if (key.isEmpty()) return;                                  // a group heading
		QTimer::singleShot(0, g->win, [g, key]() { nodeGraphAdd(g, key, 0, "end", QCursor::pos()); });
	});
	QSplitter *split = new QSplitter(Qt::Horizontal, g->win);
	split->addWidget(g->palette);
	split->addWidget(g->view);
	split->setStretchFactor(1, 1);
	split->setSizes({ 160, 840 });
	lay->addWidget(split, 1);
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
