// ============================================================================
//  mbgrdviz_window.cpp -- the Qt window of the survey planning and display tool: MB-System's
//  mbgrdviz (src/mbgrdviz/mbgrdviz_callbacks.c, its Motif half), ported onto InteractiveGMT windows.
//
//  What mbgrdviz does through libmbview's windows is done here through the viewer's own:
//    - a "view" is an InteractiveGMT window bound to the tool, planning on its grid (mbgrdviz's
//      Open Primary Grid opens one through the viewer's file-open door);
//    - routes, sites, navigation and vectors are libmbview's shared data (mbgrdviz_mbview.c). The
//      WINDOW is their truth for what can be drawn: before every action the route and site lists
//      are rebuilt from the window's open polylines and one-point symbols (pull), and after it
//      whatever the engine made or changed is put back as those elements (push). A route drawn
//      with the Polyline tool, or a site placed with the Symbols tool, is therefore a route or a
//      site; one moved with the window's own vertex/drag editing is moved.
//    - mbview's mouse picks become shapes of the window: the survey area is a two-point line (its
//      centre line) plus a width, the region a rectangle, the selected route and navigation are
//      chosen in this window;
//    - "Open Selected Nav in ..." opens the ported editors in this process, one file after the
//      other, where mbgrdviz started them as programs; mbnavedit is not ported, so it is started as
//      the program, as mbgrdviz does.
// ============================================================================

#include "mbgrdviz_window.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextBrowser>
#include <QTimer>
#include <QUiLoader>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

extern "C" {
#include "mbgrdviz.h"
const char *mbedit_mbio_version(void);   // mbedit.c: the loaded MBIO's version, for About
}

namespace {

// libmbview's object colours (colortable_object_*), MBV_COLOR_BLACK .. MBV_COLOR_PURPLE
const double kObjRGB[MBGRDVIZ_NCOLORS][3] = {
	{0, 0, 0}, {1, 1, 1}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};

// libmbview's bright colour table (colortable_bright_*), which mbview_drawvector colours vectors by
const double kBrightR[11] = {1.000, 1.000, 1.000, 1.000, 0.500, 0.000, 0.000, 0.000, 0.000, 0.500, 1.000};
const double kBrightG[11] = {0.000, 0.250, 0.500, 1.000, 1.000, 1.000, 1.000, 0.500, 0.000, 0.000, 0.000};
const double kBrightB[11] = {0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 1.000, 1.000, 1.000, 1.000, 1.000};

struct GvView {
	void *win = nullptr;
	QString title;
	bool ready = false;
	std::string gridKey;                 // the identity of the grid it was set up on (host gridKey)
};

struct MbGrdViz {
	MbGrdVizHost host;
	QMainWindow *win = nullptr;
	QComboBox *viewCombo = nullptr;
	QComboBox *routeCombo = nullptr;
	QComboBox *areaCombo = nullptr;
	MbGrdVizLine selLine;                                // the last line selected in the view with a double-click
	void *selLineWin = nullptr;                          // the window it was selected in (null: none)
	std::string selNow;                                  // the name selected at the last look ("" = none)
	QComboBox *regionCombo = nullptr;
	QDoubleSpinBox *areaWidth = nullptr;
	QListWidget *navList = nullptr;
	QPushButton *navPick = nullptr;                      // "Pick in view": down while the pick is armed
	void *pickWin = nullptr;                             // the window it is armed on (null: none)
	QLabel *routeInfo = nullptr;
	QLabel *status = nullptr;
	QTimer *timer = nullptr;
	GvView views[MBGRDVIZ_MAX_VIEWS];
	int current = -1;

	// the window-side bookkeeping of the pull/push
	std::map<std::string, std::vector<int>> wpTypes;    // route name -> its waypoint kinds
	std::set<std::string> pulledRoutes, pulledSites;     // the names the last pull read / push wrote
	std::vector<std::set<int>> navDrawn;                 // the views each navigation line is drawn in
	std::vector<std::set<int>> vecDrawn;                 // the views each vector is drawn in
	QString lastSurvey;                                  // the open dialog's route, by name
	MbParking *parking = nullptr;                        // X / minimise park it in Scene Objects (mbParkable)

	// the survey dialog
	QDialog *survey = nullptr;
	QLabel *svInfo = nullptr;
	QComboBox *svLineControl = nullptr, *svDirection = nullptr, *svPlatform = nullptr, *svCrossFirstLast = nullptr,
	          *svColor = nullptr;
	QSpinBox *svLineSpacing = nullptr, *svCrossLines = nullptr, *svSwathWidth = nullptr, *svAltitude = nullptr,
	         *svInterleaving = nullptr, *svDepth = nullptr;
	QLineEdit *svName = nullptr;
};

MbGrdViz *g_gv = nullptr;

// ---- small helpers -------------------------------------------------------------------------
QWidget *gvLoadUi(MbGrdViz *m, const char *file, QWidget *parent) {
	QFile f(QDir(m->host.base.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBgrdviz", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "MBgrdviz", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

// A child the .ui must carry; a missing one is reported by name.
template <typename T>
T *gvChild(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

void gvStatus(MbGrdViz *m, const QString &text) {
	if (m->status)
		m->status->setText(text);
}

int gvNearestColor(const double rgb[3]) {
	int best = 0;
	double bestd = 1e30;
	for (int i = 0; i < MBGRDVIZ_NCOLORS; i++) {
		const double dr = rgb[0] - kObjRGB[i][0], dg = rgb[1] - kObjRGB[i][1], db = rgb[2] - kObjRGB[i][2];
		const double d = dr * dr + dg * dg + db * db;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

void gvColorRGB(int color, double rgb[3]) {
	const int c = (color >= 0 && color < MBGRDVIZ_NCOLORS) ? color : 0;
	rgb[0] = kObjRGB[c][0];
	rgb[1] = kObjRGB[c][1];
	rgb[2] = kObjRGB[c][2];
}

// mbview_getcolor, MBV_COLORTABLE_NORMAL, on the bright table: blue below, black above
void gvVectorColor(double value, double min, double max, double rgb[3]) {
	double factor;
	if (max <= min)
		factor = 0.5;
	else
		factor = (max - value) / (max - min);
	if (factor >= 1.0) {
		rgb[0] = 0.0;
		rgb[1] = 0.0;
		rgb[2] = 0.0;
	}
	else if (factor <= 0.0) {
		rgb[0] = 0.0;
		rgb[1] = 0.0;
		rgb[2] = 1.0;
	}
	else {
		const int i = int(factor * 10);
		const double ff = factor * 10 - i;
		rgb[0] = kBrightR[i] + ff * (kBrightR[i + 1] - kBrightR[i]);
		rgb[1] = kBrightG[i] + ff * (kBrightG[i + 1] - kBrightG[i]);
		rgb[2] = kBrightB[i] + ff * (kBrightB[i + 1] - kBrightB[i]);
	}
}

bool gvViewAlive(MbGrdViz *m, int v) {
	return v >= 0 && v < MBGRDVIZ_MAX_VIEWS && m->views[v].win && m->host.alive && m->host.alive(m->views[v].win);
}

// the view's window is ready to plan on (bound and its grid read); says why not
bool gvNeedView(MbGrdViz *m) {
	if (!gvViewAlive(m, m->current)) {
		QMessageBox::information(m->win, "MBgrdviz", "Open a primary grid first (File > Open Primary Grid), or choose "
		                                             "Geophysics > MB-System > Survey planning (mbgrdviz) in a window that shows one.");
		return false;
	}
	if (!m->views[m->current].ready || !mbgrdviz_view_ready(m->current)) {
		QMessageBox::information(m->win, "MBgrdviz", "The window \"" + m->views[m->current].title +
		                                             "\" has no grid to plan on (or a projected grid whose projection "
		                                             "is not known).");
		return false;
	}
	return true;
}

// read the window's grid into its view; false when it has none (yet)
bool gvSetupView(MbGrdViz *m, int v) {
	GvView &gv = m->views[v];
	if (gv.ready)
		mbgrdviz_view_release(v);        // another grid took the window: set up afresh, never on top
	gv.ready = false;
	gv.gridKey = m->host.gridKey ? m->host.gridKey(gv.win) : std::string();
	MbGrdVizGrid g;
	if (!m->host.grid || !m->host.grid(gv.win, g) || g.nx < 2 || g.ny < 2)
		return false;
	if (!g.name.empty())
		gv.title = QString::fromStdString(g.name);   // the View field names the grid it plans on
	const QByteArray title = gv.title.toUtf8();
	gv.ready = mbgrdviz_view_setup(v, title.constData(), g.geographic ? 1 : 0, g.crs.c_str(), g.z.data(), g.nx, g.ny,
	                               g.x0, g.x1, g.y0, g.y1, g.dx, g.dy) == 1;
	return gv.ready;
}

void gvRefreshViews(MbGrdViz *m) {
	QSignalBlocker b(m->viewCombo);
	m->viewCombo->clear();
	int sel = -1;
	for (int v = 0; v < MBGRDVIZ_MAX_VIEWS; v++) {
		if (!m->views[v].win)
			continue;
		// named for the GRID it plans on, never for the window's title (an empty launcher's title is
		// its own "drop a file" hint, which meant nothing here)
		const GvView &gv = m->views[v];
		const QString text = gv.ready ? gv.title
		                     : !gv.gridKey.empty() ? gv.title + "  (projection unknown: cannot plan on it)"
		                                           : QString("Empty window — load a grid");
		m->viewCombo->addItem(text, v);
		if (v == m->current)
			sel = m->viewCombo->count() - 1;
	}
	if (sel >= 0)
		m->viewCombo->setCurrentIndex(sel);
}

// bind a window as a view (or find it); makes it the current one
int gvBind(MbGrdViz *m, void *win) {
	if (!win)
		return -1;
	int v = -1;
	for (int i = 0; i < MBGRDVIZ_MAX_VIEWS; i++)
		if (m->views[i].win == win)
			v = i;
	if (v < 0) {
		for (int i = 0; i < MBGRDVIZ_MAX_VIEWS && v < 0; i++)
			if (!m->views[i].win)
				v = i;
		if (v < 0) {
			QMessageBox::warning(m->win, "MBgrdviz",
			                     QString("Unable to bind the window: %1 views are already open.").arg(MBGRDVIZ_MAX_VIEWS));
			return -1;
		}
		m->views[v].win = win;
		m->views[v].title = m->host.title ? m->host.title(win) : QString("View %1").arg(v);
		gvSetupView(m, v);
	}
	m->current = v;
	gvRefreshViews(m);
	return v;
}

// ---- the pull / push of routes and sites (see the head of this file) -------------------------
bool gvIsAreaLine(MbGrdViz *m, const MbGrdVizLine &l) {
	return m->areaCombo && !l.closed && l.v.size() >= 2 && m->areaCombo->currentText() == QString::fromStdString(l.name);
}

void gvPull(MbGrdViz *m) {
	const int v = m->current;
	if (!gvViewAlive(m, v) || !mbgrdviz_view_ready(v))
		return;
	void *win = m->views[v].win;

	// routes: every open polyline but the area's centre line
	mbgrdviz_routes_clear(v);
	m->pulledRoutes.clear();
	for (const MbGrdVizLine &l : m->host.lines(win)) {
		if (l.closed || l.v.size() < 2 || gvIsAreaLine(m, l))
			continue;
		const int n = int(l.v.size());
		std::vector<double> x(n), y(n);
		for (int j = 0; j < n; j++) {
			x[j] = l.v[j][0];
			y[j] = l.v[j][1];
		}
		std::vector<int> wp(n, MBGRDVIZ_WAYPOINT_SIMPLE);
		auto it = m->wpTypes.find(l.name);
		if (it != m->wpTypes.end() && int(it->second.size()) == n)
			wp = it->second;
		const int size = std::max(1, std::min(5, int(std::lround(l.width / 2.0))));
		mbgrdviz_route_add(v, l.name.c_str(), gvNearestColor(l.rgb), size, x.data(), y.data(), wp.data(), n);
		m->pulledRoutes.insert(l.name);
	}

	// the open survey dialog's route, found again by its name
	int wr = -1;
	if (!m->lastSurvey.isEmpty()) {
		const QByteArray want = m->lastSurvey.toUtf8();
		char name[1024];
		for (int i = 0; i < mbgrdviz_route_count(); i++)
			if (mbgrdviz_route_info(i, name, int(sizeof(name)), nullptr, nullptr, nullptr) && want == name)
				wr = i;
	}
	mbgrdviz_survey_set_working(wr);

	// sites: every one-point symbol
	const std::vector<MbGrdVizPoint> pts = m->host.points(win);
	const int ns = int(pts.size());
	std::vector<double> sx(ns), sy(ns);
	std::vector<int> sc(ns), ssz(ns);
	std::vector<const char *> names(ns);
	m->pulledSites.clear();
	for (int i = 0; i < ns; i++) {
		sx[i] = pts[i].x;
		sy[i] = pts[i].y;
		sc[i] = gvNearestColor(pts[i].rgb);
		ssz[i] = std::max(1, int(std::lround((pts[i].sizePx - 8.0) / 2.0)));
		names[i] = pts[i].name.c_str();
		m->pulledSites.insert(pts[i].name);
	}
	mbgrdviz_sites_set(v, ns, sx.data(), sy.data(), sc.data(), ssz.data(), names.data());
}

void gvPushRoutes(MbGrdViz *m, int v) {
	if (!gvViewAlive(m, v) || !mbgrdviz_view_ready(v))
		return;
	void *win = m->views[v].win;
	std::map<std::string, MbGrdVizLine> have;
	for (const MbGrdVizLine &l : m->host.lines(win))
		if (!l.closed)
			have[l.name] = l;
	std::set<std::string> now;
	char cname[1024];
	for (int i = 0; i < mbgrdviz_route_count(); i++) {
		int color = 0, size = 1, n = 0;
		if (!mbgrdviz_route_info(i, cname, int(sizeof(cname)), &color, &size, &n) || n < 1)
			continue;
		std::string name = cname[0] ? cname : "Route";
		if (now.count(name)) {   // two routes, one name: the window tells elements apart by name
			int k = 2;
			while (now.count(name + " (" + std::to_string(k) + ")"))
				k++;
			name += " (" + std::to_string(k) + ")";
			mbgrdviz_route_rename(i, name.c_str());
		}
		now.insert(name);
		std::vector<double> x(n), y(n), z(n);
		std::vector<int> wp(n);
		mbgrdviz_route_waypoints(v, i, x.data(), y.data(), z.data(), wp.data(), n);
		m->wpTypes[name] = wp;
		auto it = have.find(name);
		std::string group = "Routes";
		if (it != have.end()) {
			const MbGrdVizLine &l = it->second;
			bool same = int(l.v.size()) == n;
			for (int j = 0; same && j < n; j++) {
				const double tol = 1e-9 * (std::fabs(x[j]) + std::fabs(y[j]) + 1.0);
				same = std::fabs(l.v[j][0] - x[j]) <= tol && std::fabs(l.v[j][1] - y[j]) <= tol;
			}
			if (same)
				continue;
			group = l.group;
			m->host.removeLine(win, name.c_str());
		}
		MbGrdVizLine l;
		l.name = name;
		l.group = group;
		for (int j = 0; j < n; j++)
			l.v.push_back({x[j], y[j], z[j]});
		gvColorRGB(color, l.rgb);
		l.width = 2.0 * size;
		m->host.addLine(win, l);
	}
	// the routes the engine deleted
	for (const std::string &name : m->pulledRoutes)
		if (!now.count(name) && have.count(name))
			m->host.removeLine(win, name.c_str());
	m->pulledRoutes = now;
}

void gvPushSites(MbGrdViz *m, int v, const char *master) {
	if (!gvViewAlive(m, v) || !mbgrdviz_view_ready(v))
		return;
	void *win = m->views[v].win;
	std::map<std::string, MbGrdVizPoint> have;
	for (const MbGrdVizPoint &p : m->host.points(win))
		have[p.name] = p;
	std::set<std::string> now;
	std::vector<MbGrdVizPoint> add;
	char cname[1024];
	for (int i = 0; i < mbgrdviz_site_count(); i++) {
		MbGrdVizPoint p;
		int color = 0, size = 1;
		if (!mbgrdviz_site_get(v, i, cname, int(sizeof(cname)), &p.x, &p.y, &p.z, &color, &size))
			continue;
		p.name = cname[0] ? cname : ("Site " + std::to_string(i));
		while (now.count(p.name))
			p.name += "'";
		now.insert(p.name);
		gvColorRGB(color, p.rgb);
		p.sizePx = 8.0 + 2.0 * size;
		auto it = have.find(p.name);
		if (it != have.end()) {
			const double tol = 1e-9 * (std::fabs(p.x) + std::fabs(p.y) + 1.0);
			if (std::fabs(it->second.x - p.x) <= tol && std::fabs(it->second.y - p.y) <= tol)
				continue;
			m->host.removePoint(win, p.name.c_str());
		}
		add.push_back(p);
	}
	for (const std::string &name : m->pulledSites)
		if (!now.count(name) && have.count(name))
			m->host.removePoint(win, name.c_str());
	if (!add.empty())
		m->host.addPoints(win, add, master);
	m->pulledSites = now;
}

// the navigation and vectors (libmbview's shared lists) that view v does not show yet
void gvPushNavVectors(MbGrdViz *m, int v) {
	if (!gvViewAlive(m, v) || !mbgrdviz_view_ready(v))
		return;
	void *win = m->views[v].win;
	char name[1024], path[1024];
	m->navDrawn.resize(size_t(mbgrdviz_nav_count()));
	m->vecDrawn.resize(size_t(mbgrdviz_vector_count()));
	for (int i = 0; i < mbgrdviz_nav_count(); i++) {
		if (m->navDrawn[i].count(v))
			continue;
		m->navDrawn[i].insert(v);
		int format = 0, n = 0, swathbounds = 0, nsel = 0;
		mbgrdviz_nav_info(i, name, int(sizeof(name)), path, int(sizeof(path)), &format, &n, &swathbounds, &nsel);
		if (n < 1)
			continue;
		std::vector<double> x(n), y(n), z(n), px(n), py(n), sx(n), sy(n);
		mbgrdviz_nav_points(v, i, x.data(), y.data(), z.data(), px.data(), py.data(), sx.data(), sy.data(), n);
		std::vector<double> xyz;
		for (int j = 0; j < n; j++) {
			xyz.push_back(x[j]);
			xyz.push_back(y[j]);
			xyz.push_back(0.0);
		}
		const int seg[2] = {0, n};
		m->host.addLines(win, xyz.data(), n, seg, 1, 0.0, 0.0, 0.0, 1.5, name, "Navigation");
		if (swathbounds) {
			xyz.clear();
			for (int j = 0; j < n; j++) {
				xyz.push_back(px[j]);
				xyz.push_back(py[j]);
				xyz.push_back(0.0);
			}
			for (int j = 0; j < n; j++) {
				xyz.push_back(sx[j]);
				xyz.push_back(sy[j]);
				xyz.push_back(0.0);
			}
			const int seg2[3] = {0, n, 2 * n};
			const std::string bn = std::string(name) + " (swath bounds)";
			m->host.addLines(win, xyz.data(), 2 * n, seg2, 2, 0.5, 0.5, 0.5, 1.0, bn.c_str(), "Navigation");
		}
	}
	for (int i = 0; i < mbgrdviz_vector_count(); i++) {
		if (m->vecDrawn[i].count(v))
			continue;
		m->vecDrawn[i].insert(v);
		int n = 0;
		double dmin = 0, dmax = 0;
		mbgrdviz_vector_info(i, name, int(sizeof(name)), &n, &dmin, &dmax);
		if (n < 1)
			continue;
		std::vector<double> x(n), y(n), z(n), d(n), xyz, rgb;
		mbgrdviz_vector_points(v, i, x.data(), y.data(), z.data(), d.data(), n);
		for (int j = 0; j < n; j++) {
			xyz.push_back(x[j]);
			xyz.push_back(y[j]);
			xyz.push_back(z[j]);
			double c[3];
			gvVectorColor(d[j], dmin, dmax, c);
			rgb.push_back(c[0]);
			rgb.push_back(c[1]);
			rgb.push_back(c[2]);
		}
		m->host.addColoredPoints(win, xyz.data(), n, rgb.data(), 6.0, name[0] ? name : "Vector", "Vectors");
	}
	if (m->host.render)
		m->host.render(win);
}

// ---- the lists of this window -------------------------------------------------------------
void gvRefreshLists(MbGrdViz *m) {
	const int v = m->current;
	QStringList routes, lines2, rects;
	if (gvViewAlive(m, v)) {
		for (const MbGrdVizLine &l : m->host.lines(m->views[v].win)) {
			const QString n = QString::fromStdString(l.name);
			if (l.closed && l.isRect)
				rects << n;
			else if (!l.closed && l.v.size() >= 2) {
				if (!gvIsAreaLine(m, l))   // the area's centre line is the area, not a route (gvPull)
					routes << n;
				if (l.v.size() == 2)
					lines2 << n;
			}
		}
	}
	// the line selected in the view with a double-click (a ship track too): an Area centre line by its
	// first-to-last chord. Kept as selected while that line is in the window; a NEW selection is chosen.
	bool pickSel = false;
	if (gvViewAlive(m, v) && m->host.selectedLine) {
		MbGrdVizLine l;
		std::string now;
		if (m->host.selectedLine(m->views[v].win, l)) {
			now = l.name;
			m->selLine = l;
			m->selLineWin = m->views[v].win;
		}
		pickSel = !now.empty() && now != m->selNow;
		m->selNow = now;
	}
	if (m->selLineWin && gvViewAlive(m, v) && m->selLineWin == m->views[v].win && m->host.hasLine &&
	    m->host.hasLine(m->selLineWin, m->selLine.name.c_str())) {
		const QString n = QString::fromStdString(m->selLine.name);
		if (!lines2.contains(n))
			lines2 << n;
	}
	else {
		m->selLineWin = nullptr;
		m->selLine = MbGrdVizLine();
	}
	auto refill = [](QComboBox *c, const QStringList &items, const QString &first) {
		const QString keep = c->currentText();
		QStringList want;
		if (!first.isEmpty())
			want << first;
		want << items;
		QStringList now;
		for (int i = 0; i < c->count(); i++)
			now << c->itemText(i);
		if (now == want)
			return;
		QSignalBlocker b(c);
		c->clear();
		c->addItems(want);
		const int k = c->findText(keep);
		c->setCurrentIndex(k >= 0 ? k : 0);
	};
	refill(m->routeCombo, routes, "All routes");
	refill(m->areaCombo, lines2, "");
	if (pickSel) {
		const int k = m->areaCombo->findText(QString::fromStdString(m->selNow));
		if (k >= 0) {
			QSignalBlocker b(m->areaCombo);
			m->areaCombo->setCurrentIndex(k);
		}
	}
	refill(m->regionCombo, rects, "");

	// the navigation (shared by every view, as libmbview's nav list), checkable
	QStringList want;
	std::vector<int> idx;
	char name[1024];
	for (int i = 0; i < mbgrdviz_nav_count(); i++) {
		if (mbgrdviz_nav_info(i, name, int(sizeof(name)), nullptr, 0, nullptr, nullptr, nullptr, nullptr)) {
			want << QString::fromUtf8(name);
			idx.push_back(i);
		}
	}
	QStringList now;
	for (int i = 0; i < m->navList->count(); i++)
		now << m->navList->item(i)->text();
	if (now != want) {
		QSignalBlocker b(m->navList);
		m->navList->clear();
		for (int k = 0; k < want.size(); k++) {
			auto *it = new QListWidgetItem(want[k], m->navList);
			it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
			int nsel = 0;
			mbgrdviz_nav_info(idx[k], nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, &nsel);
			it->setCheckState(nsel > 0 ? Qt::Checked : Qt::Unchecked);
			it->setData(Qt::UserRole, idx[k]);
		}
	}
}

// the engine's index of the route the Route combo names (after a pull), -1 for "All routes"
int gvSelectedRoute(MbGrdViz *m) {
	if (m->routeCombo->currentIndex() <= 0)
		return -1;
	const QByteArray want = m->routeCombo->currentText().toUtf8();
	char name[1024];
	for (int i = 0; i < mbgrdviz_route_count(); i++)
		if (mbgrdviz_route_info(i, name, int(sizeof(name)), nullptr, nullptr, nullptr) && want == name)
			return i;
	return -1;
}

void gvRouteInfo(MbGrdViz *m) {
	QString text;
	if (m->routeCombo->currentIndex() > 0 && gvViewAlive(m, m->current) && mbgrdviz_view_ready(m->current)) {
		gvPull(m);
		const int r = gvSelectedRoute(m);
		int n = 0;
		double lat = 0, topo = 0;
		if (mbgrdviz_route_info(r, nullptr, 0, nullptr, nullptr, &n) && mbgrdviz_route_distances(m->current, r, &lat, &topo))
			text = QString("Waypoints: %1   Distance: %2 m (lateral) %3 m (over bottom)").arg(n).arg(lat, 0, 'f', 1).arg(topo, 0, 'f', 1);
	}
	m->routeInfo->setText(text);
}

void gvTick(MbGrdViz *m) {
	// windows that went away; windows whose grid arrived
	bool changed = false, lost = false;
	for (int v = 0; v < MBGRDVIZ_MAX_VIEWS; v++) {
		if (!m->views[v].win)
			continue;
		if (!m->host.alive || !m->host.alive(m->views[v].win)) {
			lost = true;
			mbgrdviz_view_release(v);
			m->views[v] = GvView();
			for (auto &s : m->navDrawn)
				s.erase(v);
			for (auto &s : m->vecDrawn)
				s.erase(v);
			if (m->current == v)
				m->current = -1;
			changed = true;
		}
		// a grid arrived, or the window now shows another one (the last opened into it): plan on THAT
		// one. Only on a change: a grid that cannot be set up is not re-copied every tick.
		else if (m->host.gridKey ? m->host.gridKey(m->views[v].win) != m->views[v].gridKey : !m->views[v].ready) {
			gvSetupView(m, v);
			changed = true;
		}
	}
	if (m->current < 0) {
		for (int v = 0; v < MBGRDVIZ_MAX_VIEWS && m->current < 0; v++)
			if (m->views[v].win)
				m->current = v;
	}
	// the tool belongs to the windows it plans on: the last of them closed, it goes with it, as every
	// iGMT tool window does (never left open with no window behind it)
	if (lost && m->current < 0) {
		mbParkQuit(m->parking);
		return;
	}
	if (changed)
		gvRefreshViews(m);
	gvRefreshLists(m);
	// the window the pick was armed on went away: the button pops back up
	if (m->pickWin && !(m->host.alive && m->host.alive(m->pickWin))) {
		m->pickWin = nullptr;
		QSignalBlocker b(m->navPick);
		m->navPick->setChecked(false);
	}
}

// ---- the actions -------------------------------------------------------------------------
// What File > Open Navigation / Open Swath Data take: a swath sonar file (its ship track is read
// out of it), its .fnv navigation file (MB-System's text track, written by mbdatalist -O), or a
// datalist naming several of them. MBIO picks the format from the name, hence the suffixes.
static const char *kGvSwathFilter =
	"Swath files, navigation and datalists (*.mb-1 *.mb?? *.mb??? *.fnv *.all *.kmall *.s7k *.gsf *.xse);;"
	"Datalists (*.mb-1);;All Files (*)";
static const char *kGvNavHelp =
	"Navigation = the ship track (time, lon, lat, heading, speed) recorded in swath sonar files. Pick a swath "
	"file MBIO reads (.mb59, .all, .kmall, .s7k, .gsf ...), its .fnv navigation file, or a datalist (.mb-1). "
	"Open Swath Data also draws the swath edges.";

QString gvFileDialog(MbGrdViz *m, bool save, const QString &title, const QString &filter) {
	const QString fn = save ? QFileDialog::getSaveFileName(m->win, title, m->host.base.startDir(), filter)
	                        : QFileDialog::getOpenFileName(m->win, title, m->host.base.startDir(), filter);
	if (!fn.isEmpty())
		m->host.base.rememberDir(fn);
	return fn;
}

// the busy notice around the engine's reading (the no-dead-time law), its text the engine's own
void gvMessage(const char *message) {
	if (g_gv && g_gv->host.base.busyText)
		g_gv->host.base.busyText(message);
}

bool gvDoOpen(MbGrdViz *m, int what, const QString &path) {
	if (path.isEmpty() || !gvNeedView(m))
		return false;
	const int v = m->current;
	gvPull(m);
	const QByteArray p = QDir::fromNativeSeparators(path).toUtf8();
	const char *busy = what == MBGRDVIZ_OPEN_NAV     ? "Reading navigation..."
	                   : what == MBGRDVIZ_OPEN_SWATH ? "Reading swath data..."
	                                                 : "Reading...";
	if (m->host.base.busyText)
		m->host.base.busyText(busy);
	const int nnavBefore = mbgrdviz_nav_count();
	bool ok = mbgrdviz_open(v, what, p.constData()) == 1;
	// the engine answers "success" for a datalist it got nothing out of: say what happened instead
	const bool navWhat = what == MBGRDVIZ_OPEN_NAV || what == MBGRDVIZ_OPEN_SWATH;
	if (navWhat && mbgrdviz_nav_count() == nnavBefore)
		ok = false;
	if (what == MBGRDVIZ_OPEN_ROUTE)
		gvPushRoutes(m, v);
	else if (what == MBGRDVIZ_OPEN_SITE) {
		const std::string master = QFileInfo(path).fileName().toStdString();
		gvPushSites(m, v, master.c_str());
	}
	gvPushNavVectors(m, v);
	if (m->host.base.busyOff)
		m->host.base.busyOff();
	gvRefreshLists(m);
	gvStatus(m, (ok ? "Read " : "Unable to read ") + QFileInfo(path).fileName());
	if (navWhat && !ok) {   // no navigation line came out of the file: a message box, never silence
		// non-modal: it must not hold the event loop (a scripted open waits on that loop too)
		auto *mb = new QMessageBox(QMessageBox::Warning, "MBgrdviz",
			"No navigation was read from " + QFileInfo(path).fileName() + ".\n\n"
			"Pick a swath file MB-System's MBIO reads (e.g. .mb59, .all, .kmall, .s7k, .gsf), its .fnv navigation "
			"file, or a datalist (.mb-1) listing such files with their format numbers. The file name must tell "
			"MBIO the format (a .mbXX suffix, or a known vendor suffix).", QMessageBox::Ok, m->win);
		mb->setAttribute(Qt::WA_DeleteOnClose);
		mb->setModal(false);
		mb->show();
	}
	return ok;
}

bool gvDoSave(MbGrdViz *m, int what, const QString &path) {
	if (path.isEmpty() || !gvNeedView(m))
		return false;
	const int v = m->current;
	gvPull(m);
	const int r = gvSelectedRoute(m);
	mbgrdviz_route_select(r);
	if (what == MBGRDVIZ_SAVE_PROFILE) {
		if (r < 0) {
			QMessageBox::information(m->win, "MBgrdviz", "Choose the route the profile follows (Route:).");
			return false;
		}
		if (mbgrdviz_route_profile(v, r) < 2) {
			QMessageBox::warning(m->win, "MBgrdviz", "No profile along that route: it does not cross the grid.");
			return false;
		}
	}
	const QByteArray p = QDir::fromNativeSeparators(path).toUtf8();
	const bool ok = mbgrdviz_save(v, what, p.constData()) == 1;
	gvStatus(m, (ok ? "Wrote " : "Unable to write ") + QFileInfo(path).fileName());
	return ok;
}

// the last primary grid opened in this tool (iGMT.ini), and the grid dialogs' filter
static const char *const kGvLastGridKey = "mbgrdviz/last_grid";
static const char *const kGvGridFilter = "Grid Files (*.grd *.nc *.tif *.tiff);;All Files (*)";

// a primary grid from a file: File > Open Primary Grid, the Load grid button and a drop all come here
void gvOpenPrimaryFile(MbGrdViz *m, const QString &fn) {
	if (fn.isEmpty() || !m->host.openFile)
		return;
	const QByteArray p = QDir::toNativeSeparators(fn).toUtf8();
	// into the current window when it has no grid yet (an empty launcher), else into a new one
	void *into = (gvViewAlive(m, m->current) && !m->views[m->current].ready) ? m->views[m->current].win : nullptr;
	void *w = m->host.openFile(into, p.constData());
	if (w) {
		if (m->host.base.setSetting)            // Open Primary Grid proposes it next time (gvOpenPrimary)
			m->host.base.setSetting(kGvLastGridKey, QDir::fromNativeSeparators(fn));
		const int v = gvBind(m, w);
		if (v >= 0) {
			gvSetupView(m, v);
			gvRefreshViews(m);
		}
	}
	gvRefreshLists(m);
}

// File > Open Primary Grid and the Load grid button: the dialog opens with the last grid opened here
// already selected, so Enter opens it again. A last grid that is gone falls back to the usual folder.
void gvOpenPrimary(MbGrdViz *m) {
	const QString last = m->host.base.setting ? m->host.base.setting(kGvLastGridKey) : QString();
	if (last.isEmpty() || !QFileInfo::exists(last)) {
		gvOpenPrimaryFile(m, gvFileDialog(m, false, "Open Primary Grid", kGvGridFilter));
		return;
	}
	const QString fn = QFileDialog::getOpenFileName(m->win, "Open Primary Grid", last, kGvGridFilter);
	if (!fn.isEmpty() && m->host.base.rememberDir)
		m->host.base.rememberDir(fn);
	gvOpenPrimaryFile(m, fn);
}

void gvOpenOverlay(MbGrdViz *m) {
	if (!gvNeedView(m))
		return;
	const QString fn = gvFileDialog(m, false, "Open Overlay Grid", "Grid Files (*.grd *.nc *.tif *.tiff);;All Files (*)");
	if (fn.isEmpty() || !m->host.openFile)
		return;
	const QByteArray p = QDir::toNativeSeparators(fn).toUtf8();
	m->host.openFile(m->views[m->current].win, p.constData());
}

// a navigation file an editor opens
struct GvSel {
	QString path;
	int format;
};

// the navigation line a track was drawn for: by the name gvPushNavVectors gave it, or its swath
// bounds' "<name> (swath bounds)"; -1 = none
int gvNavIndex(const std::string &track) {
	static const std::string kBounds = " (swath bounds)";
	std::string want = track;
	if (want.size() > kBounds.size() && want.compare(want.size() - kBounds.size(), kBounds.size(), kBounds) == 0)
		want.resize(want.size() - kBounds.size());
	char name[1024];
	for (int i = 0; i < mbgrdviz_nav_count(); i++)
		if (mbgrdviz_nav_info(i, name, int(sizeof(name)), nullptr, 0, nullptr, nullptr, nullptr, nullptr) && want == name)
			return i;
	return -1;
}

// do_mbgrdviz_open_mbedit / _mbeditviz / _mbnavedit / _mbvelocitytool, on these files
void gvRunEditor(MbGrdViz *m, int which, const std::vector<GvSel> &sel) {
	QStringList files;
	std::vector<int> formats;
	for (const GvSel &s : sel) {
		files << s.path;
		formats.push_back(s.format);
	}
	mbRunNavEditor(m->host, m->win, which, files, formats);
}

// the Action menu: the editor on the navigation checked in the Navigation list
void gvOpenEditor(MbGrdViz *m, int which) {
	if (!gvNeedView(m))
		return;
	std::vector<GvSel> sel;
	char path[1024];
	for (int i = 0; i < mbgrdviz_nav_count(); i++) {
		int format = 0, nsel = 0;
		if (mbgrdviz_nav_info(i, nullptr, 0, path, int(sizeof(path)), &format, nullptr, nullptr, &nsel) && nsel > 0)
			sel.push_back({QString::fromUtf8(path), format});
	}
	if (sel.empty()) {
		QMessageBox::information(m->win, "MBgrdviz", "Check the navigation to open (Navigation), or press Pick in view "
		                                             "and click the tracks in the window.");
		return;
	}
	gvRunEditor(m, which, sel);
}

// a click of "Pick in view" on a line of the window: a track toggles its check in the Navigation
// list, as picking a navigation line in an mbview window selected it
void gvNavPicked(MbGrdViz *m, const std::string &track) {
	if (g_gv != m)
		return;
	const int i = gvNavIndex(track);
	if (i < 0) {
		gvStatus(m, "Not a navigation track: click a ship track");
		return;
	}
	char name[1024] = "";
	int nsel = 0;
	mbgrdviz_nav_info(i, name, int(sizeof(name)), nullptr, 0, nullptr, nullptr, nullptr, &nsel);
	mbgrdvizSelectNav(i, nsel <= 0);
	gvStatus(m, QString(nsel <= 0 ? "Selected %1" : "Unselected %1").arg(QString::fromUtf8(name)));
}

// disarm the pick, wherever it is armed (no widget touched: also run while the tool goes away)
void gvPickOff(MbGrdViz *m) {
	if (m->pickWin && m->host.pickNav && m->host.alive && m->host.alive(m->pickWin))
		m->host.pickNav(m->pickWin, nullptr);
	m->pickWin = nullptr;
}

// "Pick in view" pressed / released: armed on the view the tool plans on
void gvPickNav(MbGrdViz *m, bool on) {
	gvPickOff(m);
	if (on && m->host.pickNav && gvViewAlive(m, m->current)) {
		void *win = m->views[m->current].win;
		if (m->host.pickNav(win, [m](const std::string &track) { gvNavPicked(m, track); }))
			m->pickWin = win;
	}
	if (m->navPick) {
		QSignalBlocker b(m->navPick);
		m->navPick->setChecked(m->pickWin != nullptr);
	}
	if (m->pickWin)
		gvStatus(m, "Click the tracks in the window to check / uncheck them");
	else if (on)
		gvStatus(m, "No window to pick in: open a primary grid first");
}

// the region / area shapes, read off the window by name
bool gvShape(MbGrdViz *m, const QString &name, MbGrdVizLine &out) {
	if (name.isEmpty() || !gvViewAlive(m, m->current))
		return false;
	for (const MbGrdVizLine &l : m->host.lines(m->views[m->current].win))
		if (QString::fromStdString(l.name) == name) {
			out = l;
			return true;
		}
	// the line selected in the view with a double-click (an imported line, a ship track)
	if (m->selLineWin == m->views[m->current].win && QString::fromStdString(m->selLine.name) == name) {
		out = m->selLine;
		return true;
	}
	return false;
}

// do_mbgrdviz_open_region
bool gvOpenRegion(MbGrdViz *m) {
	if (!gvNeedView(m))
		return false;
	MbGrdVizLine r;
	if (!gvShape(m, m->regionCombo->currentText(), r) || r.v.size() < 2) {
		QMessageBox::information(m->win, "MBgrdviz", "Draw a rectangle in the window, and choose it as the Region.");
		return false;
	}
	double x0 = r.v[0][0], x1 = x0, y0 = r.v[0][1], y1 = y0;
	for (const auto &p : r.v) {
		x0 = std::min(x0, p[0]);
		x1 = std::max(x1, p[0]);
		y0 = std::min(y0, p[1]);
		y1 = std::max(y1, p[1]);
	}
	const int v = m->current;
	gvPull(m);
	if (!mbgrdviz_set_region(v, x0, y0, x1, y1))
		return false;
	MbGrdVizGrid src;
	if (!m->host.grid(m->views[v].win, src))
		return false;
	MbGrdVizGrid g;
	float *z = nullptr;
	if (!mbgrdviz_region_grid(v, &z, &g.nx, &g.ny, &g.x0, &g.x1, &g.y0, &g.y1)) {
		QMessageBox::warning(m->win, "MBgrdviz", "The region does not cover the grid.");
		return false;
	}
	g.z.assign(z, z + size_t(g.nx) * size_t(g.ny));
	mbgrdviz_free(z);
	g.dx = src.dx;
	g.dy = src.dy;
	g.geographic = src.geographic;
	g.crs = src.crs;
	g.name = "Region from " + m->views[v].title.toStdString();
	void *w = m->host.newWindow ? m->host.newWindow(g.name.c_str(), g) : nullptr;
	if (!w)
		return false;
	const int nv = gvBind(m, w);
	if (nv < 0)
		return false;
	// libmbview shows its shared sites, routes and navigation in every view
	m->pulledRoutes.clear();
	m->pulledSites.clear();
	gvPushRoutes(m, nv);
	gvPushSites(m, nv, "Sites");
	gvPushNavVectors(m, nv);
	gvRefreshLists(m);
	return true;
}

// ---- the survey dialog (do_mbgrdviz_make_survey, _generate_survey, _arearoute_*) ------------
bool gvSetArea(MbGrdViz *m) {
	MbGrdVizLine l;
	if (!gvShape(m, m->areaCombo->currentText(), l) || l.v.size() < 2 || l.closed)
		return false;
	// a two-point line is the centre line itself; a longer one (a ship track) counts by its first-to-last chord
	const auto &a = l.v.front(), &b = l.v.back();
	return mbgrdviz_set_area(m->current, a[0], a[1], b[0], b[1], m->areaWidth->value()) == 1;
}

// do_mbgrdviz_arearoute_recalc: the sensitivity of the controls, then the info text
void gvSurveyRecalc(MbGrdViz *m) {
	if (!m->survey)
		return;
	const int mode = m->svLineControl->currentIndex();
	const int platform = m->svPlatform->currentIndex();
	m->svLineSpacing->setEnabled(mode == 0);
	m->svPlatform->setEnabled(mode == 1);
	m->svSwathWidth->setEnabled(mode == 1);
	m->svAltitude->setEnabled(mode == 1 && platform == 1);
	m->svDepth->setEnabled(mode == 1 && platform == 2);
	char text[4096];
	mbgrdviz_survey_info(m->current, text, int(sizeof(text)));
	m->svInfo->setText(QString::fromUtf8(text));
}

// do_mbgrdviz_arearoute_parameterchange
void gvSurveyRead(MbGrdViz *m) {
	struct mbgrdviz_survey p;
	mbgrdviz_survey_get(&p);
	p.mode = m->svLineControl->currentIndex();
	p.direction = m->svDirection->currentIndex();
	p.crosslines = m->svCrossLines->value();
	p.crosslines_last = m->svCrossFirstLast->currentIndex();
	p.interleaving = m->svInterleaving->value();
	p.color = m->svColor->currentIndex();
	p.linespacing = m->svLineSpacing->value();
	p.platform = m->svPlatform->currentIndex();
	p.swathwidth = m->svSwathWidth->value();
	p.altitude = m->svAltitude->value();
	p.depth = m->svDepth->value();
	const QByteArray name = m->svName->text().toUtf8();
	snprintf(p.name, sizeof(p.name), "%s", name.constData());
	mbgrdviz_survey_set(&p);
	mbgrdviz_survey_get(&p);
	if (m->svName->text() != QString::fromUtf8(p.name))
		m->svName->setText(QString::fromUtf8(p.name));
	gvSurveyRecalc(m);
}

// do_mbgrdviz_arearoute_*_increment: the spin step grows with the value
void gvStepIncrement(QSpinBox *s) {
	const int v = s->value();
	s->setSingleStep(v < 25 ? 1 : v < 100 ? 5 : v < 250 ? 10 : v < 1000 ? 25 : v < 2000 ? 50 : 100);
}

QString gvGenerate(MbGrdViz *m) {
	if (!gvNeedView(m))
		return QString();
	gvSurveyRead(m);
	gvPull(m);
	if (!gvSetArea(m)) {
		QMessageBox::information(m->win, "MBgrdviz", "Draw a line (two points) across the survey area's length in the window, or double-click a line or ship track in it, "
		                                             "and choose it as the Area centre line.");
		return QString();
	}
	const int r = mbgrdviz_generate_survey(m->current);
	QString name;
	if (r >= 0) {
		char cname[1024];
		gvPushRoutes(m, m->current);   // may rename a duplicate: read the name after
		if (mbgrdviz_route_info(r, cname, int(sizeof(cname)), nullptr, nullptr, nullptr))
			name = QString::fromUtf8(cname);
		m->lastSurvey = name;
	}
	if (m->host.render)
		m->host.render(m->views[m->current].win);
	gvSurveyRecalc(m);
	gvRefreshLists(m);
	return name;
}

void gvSurveyDismiss(MbGrdViz *m) {
	mbgrdviz_survey_dismiss();
	m->lastSurvey.clear();
	if (m->survey)
		m->survey->hide();
}

bool gvMakeSurvey(MbGrdViz *m) {
	if (!gvNeedView(m))
		return false;
	gvPull(m);
	if (!gvSetArea(m)) {
		QMessageBox::information(m->win, "MBgrdviz", "Draw a line (two points) across the survey area's length in the window, or double-click a line or ship track in it, "
		                                             "and choose it as the Area centre line.");
		return false;
	}
	if (!m->survey) {
		QWidget *w = gvLoadUi(m, "mbgrdviz_survey.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return false;
		}
		QStringList missing;
		m->svInfo = gvChild<QLabel>(d, "labelInfo", missing);
		m->svLineControl = gvChild<QComboBox>(d, "lineControl", missing);
		m->svLineSpacing = gvChild<QSpinBox>(d, "lineSpacing", missing);
		m->svDirection = gvChild<QComboBox>(d, "direction", missing);
		m->svPlatform = gvChild<QComboBox>(d, "platform", missing);
		m->svCrossLines = gvChild<QSpinBox>(d, "crossLines", missing);
		m->svSwathWidth = gvChild<QSpinBox>(d, "swathWidth", missing);
		m->svCrossFirstLast = gvChild<QComboBox>(d, "crossFirstLast", missing);
		m->svAltitude = gvChild<QSpinBox>(d, "altitude", missing);
		m->svInterleaving = gvChild<QSpinBox>(d, "interleaving", missing);
		m->svDepth = gvChild<QSpinBox>(d, "depth", missing);
		m->svName = gvChild<QLineEdit>(d, "name", missing);
		m->svColor = gvChild<QComboBox>(d, "color", missing);
		auto *gen = gvChild<QPushButton>(d, "generateButton", missing);
		auto *dis = gvChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBgrdviz", "mbgrdviz_survey.ui lacks: " + missing.join(", "));
			delete d;
			return false;
		}
		m->survey = d;
		QObject::connect(gen, &QPushButton::clicked, d, [m]() { gvGenerate(m); });
		QObject::connect(dis, &QPushButton::clicked, d, [m]() { gvSurveyDismiss(m); });
		QObject::connect(d, &QDialog::rejected, d, [m]() {
			mbgrdviz_survey_dismiss();
			m->lastSurvey.clear();
		});
		for (QComboBox *c : {m->svLineControl, m->svDirection, m->svPlatform, m->svCrossFirstLast, m->svColor})
			QObject::connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), d, [m](int) { gvSurveyRead(m); });
		for (QSpinBox *s : {m->svLineSpacing, m->svCrossLines, m->svSwathWidth, m->svAltitude, m->svInterleaving, m->svDepth})
			QObject::connect(s, QOverload<int>::of(&QSpinBox::valueChanged), d, [m](int) { gvSurveyRead(m); });
		for (QSpinBox *s : {m->svLineSpacing, m->svAltitude, m->svDepth})
			QObject::connect(s, QOverload<int>::of(&QSpinBox::valueChanged), d, [s](int) { gvStepIncrement(s); });
		QObject::connect(m->svName, &QLineEdit::editingFinished, d, [m]() { gvSurveyRead(m); });
	}

	// do_mbgrdviz_make_survey: the widgets from the current parameters
	struct mbgrdviz_survey p;
	mbgrdviz_survey_get(&p);
	{
		const QSignalBlocker b1(m->svLineControl), b2(m->svDirection), b3(m->svCrossLines), b4(m->svCrossFirstLast),
		    b5(m->svInterleaving), b6(m->svColor), b7(m->svLineSpacing), b8(m->svPlatform), b9(m->svSwathWidth),
		    b10(m->svAltitude), b11(m->svDepth), b12(m->svName);
		m->svLineControl->setCurrentIndex(p.mode);
		m->svDirection->setCurrentIndex(p.direction);
		m->svCrossLines->setValue(p.crosslines);
		m->svCrossFirstLast->setCurrentIndex(p.crosslines_last);
		m->svInterleaving->setValue(p.interleaving);
		m->svColor->setCurrentIndex(p.color);
		m->svLineSpacing->setValue(p.linespacing);
		m->svPlatform->setCurrentIndex(p.platform);
		m->svSwathWidth->setValue(p.swathwidth);
		m->svAltitude->setValue(p.altitude);
		m->svDepth->setValue(p.depth);
		m->svName->setText(QString::fromUtf8(p.name));
	}
	gvSurveyRecalc(m);
	m->survey->show();
	m->survey->raise();
	m->survey->activateWindow();
	return true;
}

void gvAbout(MbGrdViz *m) {
	QMessageBox::about(m->win, "About MBgrdviz",
	                   "<b>MBgrdviz</b><br>Simple 3D Visualization of GMT Grids<br><br>"
	                   "One of the MB-System programs: an Open Source Software Package for Processing and Display "
	                   "of Swath Sonar Data, by David W. Caress and Dale N. Chayes (Monterey Bay Aquarium Research "
	                   "Institute, Lamont-Doherty Earth Observatory of Columbia University).<br><br>"
	                   "Ported to InteractiveGMT: the grids, sites, routes and navigation are shown in InteractiveGMT "
	                   "windows.<br>MBIO: " +
	                       QString::fromUtf8(mbedit_mbio_version()));
}

// Help > How to use MBgrdviz (F1): the working guide to this panel, non-modal so it can stay open
// beside it while the user works. One instance, raised when asked again.
void gvHelp(MbGrdViz *m) {
	static QPointer<QDialog> dlg;
	if (!dlg) {
		dlg = new QDialog(m->win);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowTitle("How to use MBgrdviz");
		dlg->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		auto *lay = new QVBoxLayout(dlg);
		auto *tb = new QTextBrowser(dlg);
		tb->setOpenExternalLinks(false);
		tb->setHtml(
			"<h3>MBgrdviz &mdash; survey planning on a grid</h3>"
			"<p>MBgrdviz plans and reviews swath surveys on a bathymetry grid. It has no map of its own: "
			"it works <b>on an InteractiveGMT window</b>. What you draw there is what it plans with, and "
			"what it makes (routes, sites, tracks) lands there as Scene Objects elements.</p>"

			"<h4>1. Pick the grid (View)</h4>"
			"<p><b>View</b> lists the windows MBgrdviz plans on; the actions work on the one chosen there, on "
			"the grid that window shows. A window joins the list when <i>Survey planning (mbgrdviz)</i> is "
			"opened from it, or when a grid is loaded with <b>Load grid&hellip;</b> / File &gt; Open Primary "
			"Grid, or dropped on this panel. <i>Open Overlay Grid</i> adds another grid to the same window.</p>"

			"<h4>2. Routes</h4>"
			"<p><b>Every open polyline in the window is a route.</b> Draw one with the window's "
			"<i>Polyline</i> (or <i>Line</i>) tool, or read one with File &gt; Open Route File. Move its "
			"waypoints with the window's own vertex editing (double-click the line). The <b>Route</b> box "
			"chooses which route the writers save (File &gt; Save Route File, Save Route As &gt; Hypack, "
			"Kongsberg, SIS, TECDIS, Greensea, &hellip;) and which one <i>Save Profile File</i> follows; "
			"<i>All routes</i> saves every one. The line under it gives the route's length and waypoints.</p>"

			"<h4>3. Sites</h4>"
			"<p><b>Every one-point symbol in the window is a site.</b> Place them with the window's "
			"<i>Symbols</i> tool, or read them with File &gt; Open Site File; File &gt; Save Site File writes "
			"them all.</p>"

			"<h4>4. Navigation (ship tracks)</h4>"
			"<p>File &gt; <i>Open Navigation</i> reads the ship track of swath files (or a datalist); "
			"<i>Open Swath Data</i> adds the swath edges too. Each track is a line of the window, listed in "
			"<b>Navigation</b>. Check the tracks you want, or press <b>Pick in view</b> and click tracks in "
			"the window (each click checks or unchecks one; press again to stop). Then <b>Action &gt; Open "
			"Selected Nav in</b> MBedit (ping editor), MBeditviz (3-D bathymetry editor and patch test), "
			"MBnavedit or MBvelocitytool opens the checked files. The same editors are on each track's "
			"right-click menu, under <i>MB-System</i>.</p>"

			"<h4>5. Generate a survey (Area)</h4>"
			"<ol>"
			"<li>Give the area a <b>centre line</b>: draw a two-point line along the survey's length "
			"(<i>Line</i> tool), or double-click a line or ship track in the window. A selected line is "
			"chosen in <b>Area centre line</b> by itself; a line with more than two points counts by the "
			"straight chord from its first to its last point.</li>"
			"<li>Set <b>Area width (m)</b> across that line (0 = half its length).</li>"
			"<li>Press <b>Survey&hellip;</b> (or Action &gt; Generate Survey Route from Area). In the dialog "
			"choose uniform line spacing or spacing by swath width (with the platform: surface vessel, "
			"constant altitude or constant depth), the start corner, cross lines, the route name and colour, "
			"then <b>Generate Route</b>. The lawnmower route appears in the window as a new route, ready "
			"to be saved in any of the route formats.</li>"
			"</ol>"

			"<h4>6. Region</h4>"
			"<p>Draw a rectangle in the window (<i>Rectangle</i> tool), choose it in <b>Region</b>, and "
			"Action &gt; <i>Open Region as New View</i> cuts that part of the grid into a new window to plan "
			"on in detail.</p>"

			"<h4>Good to know</h4>"
			"<ul>"
			"<li>The window is the truth: deleting, renaming or moving a line or symbol there changes the "
			"route or site it is.</li>"
			"<li>Closing or minimising this panel parks it as a row in the window's Scene Objects; "
			"double-click that row to bring it back. File &gt; Quit closes it for good.</li>"
			"<li>Hover any control of the panel for a short description.</li>"
			"</ul>");
		lay->addWidget(tb);
		dlg->resize(560, 620);
	}
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

// the window to park in (mbParkable): the view it plans on, else any bound one
void *gvParkWhere(MbGrdViz *m) {
	if (gvViewAlive(m, m->current))
		return m->views[m->current].win;
	for (int v = 0; v < MBGRDVIZ_MAX_VIEWS; v++)
		if (gvViewAlive(m, v))
			return m->views[v].win;
	return nullptr;
}

// the survey dialog goes away with the window, parked (Hide) or closed
struct GvCloseFilter : QObject {
	using QObject::QObject;
	bool eventFilter(QObject *o, QEvent *e) override {
		if ((e->type() == QEvent::Close || e->type() == QEvent::Hide) && g_gv && o == g_gv->win) {
			if (g_gv->survey)
				g_gv->survey->hide();
			if (g_gv->pickWin)
				gvPickNav(g_gv, false);              // no clicks taken by a tool that is not on screen
		}
		return QObject::eventFilter(o, e);
	}
};

// a grid file dropped anywhere on the tool's window opens as Load grid opens one
struct GvDropFilter : QObject {
	MbGrdViz *m;
	GvDropFilter(QObject *parent, MbGrdViz *mm) : QObject(parent), m(mm) {}
	static QStringList files(const QMimeData *md) {
		QStringList out;
		if (md && md->hasUrls())
			for (const QUrl &u : md->urls())
				if (u.isLocalFile())
					out << u.toLocalFile();
		return out;
	}
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove) {
			auto *de = static_cast<QDragMoveEvent *>(e);
			if (!files(de->mimeData()).isEmpty()) {
				de->acceptProposedAction();
				return true;
			}
		}
		else if (e->type() == QEvent::Drop) {
			auto *de = static_cast<QDropEvent *>(e);
			const QStringList fl = files(de->mimeData());
			if (!fl.isEmpty()) {
				de->acceptProposedAction();
				// after the drop returns: the open is a long, blocking read
				QTimer::singleShot(0, m->win, [mm = m, fl]() {
					for (const QString &f : fl) {
						if (mm->host.base.rememberDir)
							mm->host.base.rememberDir(f);
						gvOpenPrimaryFile(mm, f);
					}
				});
				return true;
			}
		}
		return QObject::eventFilter(o, e);
	}
};

MbGrdViz *gvBuild(QWidget *parent, const MbGrdVizHost &host) {
	auto *m = new MbGrdViz;
	m->host = host;
	QWidget *w = gvLoadUi(m, "mbgrdviz.ui", parent);
	auto *win = qobject_cast<QMainWindow *>(w);
	if (!win) {
		delete w;
		delete m;
		return nullptr;
	}
	QStringList missing;
	m->viewCombo = gvChild<QComboBox>(win, "viewCombo", missing);
	m->routeCombo = gvChild<QComboBox>(win, "routeCombo", missing);
	m->areaCombo = gvChild<QComboBox>(win, "areaCombo", missing);
	m->regionCombo = gvChild<QComboBox>(win, "regionCombo", missing);
	m->areaWidth = gvChild<QDoubleSpinBox>(win, "areaWidth", missing);
	m->navList = gvChild<QListWidget>(win, "navList", missing);
	m->navPick = gvChild<QPushButton>(win, "navPickButton", missing);
	m->routeInfo = gvChild<QLabel>(win, "labelRouteInfo", missing);
	m->status = gvChild<QLabel>(win, "labelStatus", missing);
	QPushButton *loadGrid = gvChild<QPushButton>(win, "loadGridButton", missing);
	struct Act {
		const char *name;
		std::function<void()> fn;
	};
	const std::vector<Act> acts = {
		{"actionOpenPrimary", [m]() { gvOpenPrimary(m); }},
		{"actionOpenOverlay", [m]() { gvOpenOverlay(m); }},
		{"actionOpenSite", [m]() { gvDoOpen(m, MBGRDVIZ_OPEN_SITE, gvFileDialog(m, false, "Open Site File", "Site Files (*.ste *.site);;All Files (*)")); }},
		{"actionOpenRoute", [m]() { gvDoOpen(m, MBGRDVIZ_OPEN_ROUTE, gvFileDialog(m, false, "Open Route File", "Route Files (*.rte);;All Files (*)")); }},
		{"actionOpenNav", [m]() { gvDoOpen(m, MBGRDVIZ_OPEN_NAV, gvFileDialog(m, false, "Open Navigation (the ship track of swath files)", kGvSwathFilter)); }},
		{"actionOpenSwath", [m]() { gvDoOpen(m, MBGRDVIZ_OPEN_SWATH, gvFileDialog(m, false, "Open Swath Data (ship track + swath edges)", kGvSwathFilter)); }},
		{"actionOpenVector", [m]() { gvDoOpen(m, MBGRDVIZ_OPEN_VECTOR, gvFileDialog(m, false, "Open Vector File", "All Files (*)")); }},
		{"actionSaveSite", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_SITE, gvFileDialog(m, true, "Save Site File", "Site Files (*.ste);;All Files (*)")); }},
		{"actionSaveRoute", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_ROUTE, gvFileDialog(m, true, "Save Route File", "Route Files (*.rte);;All Files (*)")); }},
		{"actionSaveRouteReversed", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_ROUTEREVERSED, gvFileDialog(m, true, "Save Route File Reversed", "Route Files (*.rte);;All Files (*)")); }},
		{"actionSaveRisiHeading", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_RISISCRIPTHEADING, gvFileDialog(m, true, "Save Risi Script File (variable heading)", "All Files (*)")); }},
		{"actionSaveRisiNoHeading", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_RISISCRIPTNOHEADING, gvFileDialog(m, true, "Save Risi Script File (static heading)", "All Files (*)")); }},
		{"actionSaveRisi2Heading", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_RISI2SCRIPTHEADING, gvFileDialog(m, true, "Save Risi 2 Script File (variable heading)", "All Files (*)")); }},
		{"actionSaveRisi2NoHeading", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_RISI2SCRIPTNOHEADING, gvFileDialog(m, true, "Save Risi 2 Script File (static heading)", "All Files (*)")); }},
		{"actionSaveDegDecMin", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_DEGDECMIN, gvFileDialog(m, true, "Save Route as Degrees + Decimal Minutes File", "All Files (*)")); }},
		{"actionSaveLnw", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_LNW, gvFileDialog(m, true, "Save Route as Hypack LNW File", "Hypack Files (*.lnw);;All Files (*)")); }},
		{"actionSaveGreenseaYml", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_GREENSEAYML, gvFileDialog(m, true, "Save Route as Greensea YML File", "YML Files (*.yml);;All Files (*)")); }},
		{"actionSaveTecdisLst", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_TECDISLST, gvFileDialog(m, true, "Save Route as TECDIS LST File", "LST Files (*.lst);;All Files (*)")); }},
		{"actionSaveKongsbergDp", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_KONGSBERGDP, gvFileDialog(m, true, "Save Route as Kongsberg DP waypoint File", "All Files (*)")); }},
		{"actionSaveSisAsciiPlan1", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_SISASCIIPLAN1, gvFileDialog(m, true, "Save Route as SIS ASCIIPlan line File", "All Files (*)")); }},
		{"actionSaveSisAsciiPlan2", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_SISASCIIPLAN2, gvFileDialog(m, true, "Save Route as SIS ASCIIPlan route File", "All Files (*)")); }},
		{"actionSaveProfile", [m]() { gvDoSave(m, MBGRDVIZ_SAVE_PROFILE, gvFileDialog(m, true, "Save Profile File", "All Files (*)")); }},
		{"actionQuit", [m]() { mbParkQuit(m->parking); }},
		{"actionMbedit", [m]() { gvOpenEditor(m, 0); }},
		{"actionMbeditviz", [m]() { gvOpenEditor(m, 1); }},
		{"actionMbnavedit", [m]() { gvOpenEditor(m, 2); }},
		{"actionMbvelocitytool", [m]() { gvOpenEditor(m, 3); }},
		{"actionOpenRegion", [m]() { gvOpenRegion(m); }},
		{"actionMakeSurvey", [m]() { gvMakeSurvey(m); }},
		{"actionHelp", [m]() { gvHelp(m); }},
		{"actionAbout", [m]() { gvAbout(m); }},
	};
	std::vector<QAction *> found;
	for (const Act &a : acts)
		found.push_back(gvChild<QAction>(win, a.name, missing));
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBgrdviz", "mbgrdviz.ui lacks: " + missing.join(", "));
		delete win;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.base.icon.isNull())
		win->setWindowIcon(host.base.icon);
	for (size_t i = 0; i < acts.size(); i++) {
		const std::function<void()> fn = acts[i].fn;
		QObject::connect(found[i], &QAction::triggered, win, [fn]() { fn(); });
		// File > Open Navigation / Open Swath Data: say in the menu what such a file is
		if (std::string(acts[i].name) == "actionOpenNav" || std::string(acts[i].name) == "actionOpenSwath") {
			found[i]->setToolTip(kGvNavHelp);
			found[i]->setStatusTip(kGvNavHelp);
		}
	}
	if (QMenu *fm = found[0]->associatedObjects().isEmpty() ? nullptr : qobject_cast<QMenu *>(found[0]->associatedObjects().first()))
		fm->setToolTipsVisible(true);   // the File menu shows those tooltips on hover
	QObject::connect(m->viewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), win, [m](int k) {
		const int v = m->viewCombo->itemData(k).toInt();
		if (v >= 0 && v < MBGRDVIZ_MAX_VIEWS && m->views[v].win) {
			m->current = v;
			m->pulledRoutes.clear();
			m->pulledSites.clear();
			gvRefreshLists(m);
			if (m->pickWin && m->pickWin != m->views[v].win)
				gvPickNav(m, true);                  // the pick follows the view the tool plans on
		}
	});
	m->navPick->setCheckable(true);
	QObject::connect(m->navPick, &QPushButton::toggled, win, [m](bool on) { gvPickNav(m, on); });
	QObject::connect(m->routeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), win, [m](int) { gvRouteInfo(m); });
	QObject::connect(loadGrid, &QPushButton::clicked, win, [m]() { gvOpenPrimary(m); });
	// "Survey...": the Action menu's Generate Survey Route from Area, beside the line it works on
	if (auto *survey = win->findChild<QPushButton *>("surveyButton"))
		QObject::connect(survey, &QPushButton::clicked, win, [m]() { gvMakeSurvey(m); });
	// drops: the window takes them (no child of it accepts drops, so they all land here)
	win->setAcceptDrops(true);
	win->installEventFilter(new GvDropFilter(win, m));
	QObject::connect(m->navList, &QListWidget::itemChanged, win, [](QListWidgetItem *it) {
		mbgrdviz_nav_select(it->data(Qt::UserRole).toInt(), it->checkState() == Qt::Checked ? 1 : 0);
	});
	m->timer = new QTimer(win);
	QObject::connect(m->timer, &QTimer::timeout, win, [m]() { gvTick(m); });
	m->timer->start(1000);
	win->installEventFilter(new GvCloseFilter(win));
	m->parking = mbParkable(win, host.base, "mbgrdviz", [m]() { return gvParkWhere(m); });
	return m;
}

} // namespace

// ---- entry point ---------------------------------------------------------------------------
bool mbgrdvizOpenWindow(QWidget *parent, const MbGrdVizHost &host, void *win, const QString &file) {
	if (!g_gv) {
		if (!mbeditLoadMbio(parent, host.base))
			return false;
		char msg[2048] = "";
		if (!mbgrdviz_mbio_open(msg, int(sizeof(msg)))) {
			QMessageBox::warning(parent, "MBgrdviz", QString::fromUtf8(msg));
			return false;
		}
		MbGrdViz *m = gvBuild(parent, host);
		if (!m)
			return false;
		g_gv = m;
		mbgrdviz_set_message_hook(gvMessage);
		if (host.base.windowOpened)
			host.base.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			gvPickOff(m);
			for (int v = 0; v < MBGRDVIZ_MAX_VIEWS; v++)
				mbgrdviz_view_release(v);
			mbgrdviz_set_message_hook(nullptr);
			if (m->host.base.windowClosed)
				m->host.base.windowClosed();
			if (g_gv == m)
				g_gv = nullptr;
			delete m;
		});
		m->win->show();
		mbPlaceRight(m->win, parent);
	}
	MbGrdViz *m = g_gv;
	mbParkShow(m->parking);                              // a parked one comes back off its handle
	if (win)
		gvBind(m, win);
	if (!file.isEmpty() && m->host.openFile) {
		const QByteArray p = QDir::toNativeSeparators(file).toUtf8();
		void *into = (gvViewAlive(m, m->current) && !m->views[m->current].ready) ? m->views[m->current].win : nullptr;
		void *w = m->host.openFile(into, p.constData());
		if (!w)
			return false;
		const int v = gvBind(m, w);
		if (v < 0 || !gvSetupView(m, v))
			return false;
		gvRefreshViews(m);
	}
	gvRefreshLists(m);
	return true;
}

int mbgrdvizState(int *out, int n) {
	MbGrdViz *m = g_gv;
	int nviews = 0;
	if (m)
		for (int v = 0; v < MBGRDVIZ_MAX_VIEWS; v++)
			if (m->views[v].win)
				nviews++;
	if (m)
		gvPull(m);
	int wr = -1;
	if (m && !m->lastSurvey.isEmpty()) {
		const QByteArray want = m->lastSurvey.toUtf8();
		char name[1024];
		for (int i = 0; i < mbgrdviz_route_count(); i++)
			if (mbgrdviz_route_info(i, name, int(sizeof(name)), nullptr, nullptr, nullptr) && want == name)
				wr = i;
	}
	const int v[8] = {m ? 1 : 0,
	                  nviews,
	                  (m && gvViewAlive(m, m->current) && mbgrdviz_view_ready(m->current)) ? 1 : 0,
	                  m ? mbgrdviz_route_count() : 0,
	                  m ? mbgrdviz_site_count() : 0,
	                  m ? mbgrdviz_nav_count() : 0,
	                  m ? mbgrdviz_vector_count() : 0,
	                  wr};
	const int k = n < 8 ? n : 8;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool mbgrdvizOpen(int what, const QString &path) {
	return g_gv && gvDoOpen(g_gv, what, path);
}

bool mbgrdvizSave(int what, const QString &path) {
	return g_gv && gvDoSave(g_gv, what, path);
}

bool mbgrdvizSelectRoute(const QString &name) {
	if (!g_gv)
		return false;
	gvRefreshLists(g_gv);
	const int k = name.isEmpty() ? 0 : g_gv->routeCombo->findText(name);
	if (k < 0)
		return false;
	g_gv->routeCombo->setCurrentIndex(k);
	return true;
}

bool mbgrdvizSetArea(const QString &line, double width) {
	if (!g_gv)
		return false;
	gvRefreshLists(g_gv);
	const int k = g_gv->areaCombo->findText(line);
	if (k < 0)
		return false;
	g_gv->areaCombo->setCurrentIndex(k);
	g_gv->areaWidth->setValue(width);
	return true;
}

bool mbgrdvizSetRegion(const QString &rect) {
	if (!g_gv)
		return false;
	gvRefreshLists(g_gv);
	const int k = g_gv->regionCombo->findText(rect);
	if (k < 0)
		return false;
	g_gv->regionCombo->setCurrentIndex(k);
	return true;
}

QString mbgrdvizGenerateSurvey(int mode, int platform, int direction, int crosslines, int crosslinesLast, int linespacing,
                               int swathwidth, int altitude, int depth, int interleaving, int color, const QString &name) {
	MbGrdViz *m = g_gv;
	if (!m || !gvMakeSurvey(m))
		return QString();
	{
		const QSignalBlocker b1(m->svLineControl), b2(m->svDirection), b3(m->svCrossLines), b4(m->svCrossFirstLast),
		    b5(m->svInterleaving), b6(m->svColor), b7(m->svLineSpacing), b8(m->svPlatform), b9(m->svSwathWidth),
		    b10(m->svAltitude), b11(m->svDepth), b12(m->svName);
		m->svLineControl->setCurrentIndex(mode);
		m->svPlatform->setCurrentIndex(platform);
		m->svDirection->setCurrentIndex(direction);
		m->svCrossLines->setValue(crosslines);
		m->svCrossFirstLast->setCurrentIndex(crosslinesLast);
		m->svLineSpacing->setValue(linespacing);
		m->svSwathWidth->setValue(swathwidth);
		m->svAltitude->setValue(altitude);
		m->svDepth->setValue(depth);
		m->svInterleaving->setValue(interleaving);
		m->svColor->setCurrentIndex(color);
		m->svName->setText(name);
	}
	return gvGenerate(m);
}

bool mbgrdvizSurveyDismiss() {
	if (!g_gv)
		return false;
	gvSurveyDismiss(g_gv);
	return true;
}

bool mbgrdvizOpenRegion() {
	return g_gv && gvOpenRegion(g_gv);
}

bool mbgrdvizSelectNav(int nav, bool selected) {
	if (!g_gv || nav < 0 || nav >= mbgrdviz_nav_count())
		return false;
	mbgrdviz_nav_select(nav, selected ? 1 : 0);
	gvRefreshLists(g_gv);
	for (int i = 0; i < g_gv->navList->count(); i++) {
		QListWidgetItem *it = g_gv->navList->item(i);
		if (it->data(Qt::UserRole).toInt() == nav) {
			QSignalBlocker b(g_gv->navList);
			it->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
		}
	}
	return true;
}

// do_mbgrdviz_open_mbedit / _mbeditviz / _mbnavedit / _mbvelocitytool, on these files
void mbRunNavEditor(const MbGrdVizHost &host, QWidget *parent, int which, const QStringList &files,
                    const std::vector<int> &formats) {
	if (files.isEmpty() || int(formats.size()) != files.size())
		return;
	if (which == 2) {   // mbnavedit: not ported, started as the program, as mbgrdviz does
		QStringList args;
		for (int i = 0; i < files.size(); i++)
			args << QString("-F%1").arg(formats[i]) << "-I" + files[i];
		if (!QProcess::startDetached("mbnavedit", args))
			QMessageBox::warning(parent, "MBgrdviz", "Unable to start mbnavedit (MB-System's navigation editor is not "
			                                         "part of InteractiveGMT, and was not found on the PATH).");
		return;
	}
	if (which == 3) {   // mbvelocitytool reads one swath file
		if (host.openMbvelocity)
			host.openMbvelocity(parent, files[0], formats[0]);
		return;
	}
	for (int i = 0; i < files.size(); i++) {
		if (which == 0 && host.openMbedit)
			host.openMbedit(parent, files[i], formats[i]);
		else if (which == 1 && host.openMbeditviz)
			host.openMbeditviz(parent, files[i], formats[i], i == 0);   // mbeditviz holds the selection, nothing else
	}
}

int mbgrdvizNavIndex(const std::string &name) {
	return g_gv ? gvNavIndex(name) : -1;
}

bool mbgrdvizNavEditor(int which, const std::vector<std::string> &names) {
	if (!g_gv || which < 0 || which > 3)
		return false;
	std::vector<GvSel> sel;
	std::set<int> seen;                                  // a track and its swath bounds: one file
	char path[1024];
	for (const std::string &n : names) {
		const int i = gvNavIndex(n);
		int format = 0;
		if (i < 0 || !seen.insert(i).second ||
		    !mbgrdviz_nav_info(i, nullptr, 0, path, int(sizeof(path)), &format, nullptr, nullptr, nullptr))
			continue;
		sel.push_back({QString::fromUtf8(path), format});
	}
	if (sel.empty())
		return false;
	gvRunEditor(g_gv, which, sel);
	return true;
}

bool mbgrdvizPickNav(bool on) {
	if (!g_gv)
		return false;
	g_gv->navPick->setChecked(on);                       // the button's own path (gvPickNav)
	return g_gv->pickWin != nullptr;
}

int mbgrdvizNavSelected(int nav) {
	int nsel = 0;
	if (!g_gv || nav < 0 || nav >= mbgrdviz_nav_count() ||
	    !mbgrdviz_nav_info(nav, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, &nsel))
		return -1;
	return nsel > 0 ? 1 : 0;
}

bool mbgrdvizClose() {
	if (!g_gv)
		return false;
	mbParkQuit(g_gv->parking);
	return true;
}
