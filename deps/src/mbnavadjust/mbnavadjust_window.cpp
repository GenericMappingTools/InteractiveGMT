// ============================================================================
//  mbnavadjust_window.cpp -- the navigation adjustment tool's Qt window: the other side of the
//  by-name widget bridge (mbna_motif.h) the Motif callbacks of MB-System's mbnavadjust were ported
//  onto (mbnavadjust_callbacks.c), the xg_* graphics the engine draws through (MB-System's
//  mbaux/mb_xgraphics.c, painted into QImages), and the visualization (libmbview upstream) as a
//  viewer window of its own.
//
//  A translation unit of its own, apart from the viewer's fragments (see mbnavadjust_window.h).
//  The engine and the callbacks hold every decision; this file builds the widgets from
//  deps/ui/mbnavadjust*.ui, wires each one to the callbacks the Motif creation code wired it to
//  (mbnavadjust_creation.c), with the same client data, and carries their widget calls out.
//
//  Drawing model, kept from X11: the engine draws straight onto a canvas and "erases" by drawing
//  again in the background colour, so each canvas is a persistent QImage, painted with NO
//  antialiasing, and shown as is.
//
//  Departures from the Motif program, all forced by the host:
//    - MBIO is the swath editor's (mbeditLoadMbio): found, or asked for, once for every MB tool.
//    - The file selection box is the platform file dialog, plus the MBIO format id asked for an
//      import (the format text field of the box), prefilled from the file name as upstream does.
//    - The message and error bulletin boards are the viewer's busy notice and a message box; the
//      About form is an About box.
//    - Quit closes the tool instead of ending the program; the window's X and minimise park it in
//      Scene Objects, as every MB-System tool window does.
//    - The visualization is a viewer window: the project grid opened through the viewer's file
//      door, each section's navigation and each tie a named line of it. A click on a line picks
//      it; two navigation picks in a row are the two sections of a crossing (mbview's nav pick).
//      mbview's "Add and open new crossing" action is View > Add and Open New Crossing here.
//    - Action > Analyze Crossings opens the analysis window on the crossing or section list shown:
//      upstream hands do_naverr_init a Widget where it expects a mode, which on a 64-bit build is
//      neither mode.
// ============================================================================

#include "mbnavadjust_window.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextBrowser>
#include <QUiLoader>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// the engine, the callbacks and the bridge (C); after every Qt header: the MBIO header maps a few
// POSIX names (access, mkdir, getcwd) for the C files
extern "C" {
#include "mbnavadjust.h"
}
#include "mbnavadjust_callbacks.h"

namespace {

const int kMbSuccess = 1;

// ---- the colour table: upstream's do_mbnavadjust_init, in its order ---------------------------
// the MB_NDrawingColors named X11 colours, then five ramps of 16 and magenta: 92 entries, and a
// "pixel value" is the index into this table (mbnavadjust_set_colors gets 0..91)
const int kNColors = MB_NDrawingColors + 16 * 5 + 1;
QColor g_colors[kNColors];

void buildColors() {
	const QColor named[] = {
		QColor(255, 255, 255),   // white
		QColor(0, 0, 0),         // black
		QColor(255, 0, 0),       // red
		QColor(255, 165, 0),     // orange
		QColor(255, 255, 0),     // yellow
		QColor(0, 255, 0),       // green
		QColor(0, 160, 160),     // bluegreen
		QColor(0, 0, 255),       // blue
		QColor(160, 32, 240),    // purple
		QColor(255, 127, 80),    // coral
		QColor(211, 211, 211),   // lightgrey
	};
	for (int i = 0; i < MB_NDrawingColors; i++)
		g_colors[i] = named[i];
	auto c16 = [](int v) { return std::max(0, std::min(255, v / 257)); };
	int j = MB_NDrawingColors;
	for (int i = 0; i < 16; i++)
		g_colors[j + i] = QColor(255, c16(i * 2048), 0);
	j += 16;
	for (int i = 0; i < 16; i++)
		g_colors[j + i] = QColor(c16(65535 - i * 4096), c16(32767 + i * 2048), 0);
	j += 16;
	for (int i = 0; i < 16; i++)
		g_colors[j + i] = QColor(0, 255, c16(i * 4096));
	j += 16;
	for (int i = 0; i < 16; i++)
		g_colors[j + i] = QColor(0, c16(65535 - i * 4096), 255);
	j += 16;
	for (int i = 0; i < 16; i++)
		g_colors[j + i] = QColor(c16(i * 4096), 0, 255);
	j += 16;
	g_colors[j] = QColor(255, 0, 255);
}

QColor pixelColor(unsigned int pixel) {
	return pixel < unsigned(kNColors) ? g_colors[pixel] : g_colors[1];
}

// ---- the xg_* canvases: naverr contours, misfit, z offset, modelplot -------------------------------
struct XgCanvas {
	QImage img;
	QPainter *p = nullptr;
	QPointer<QWidget> widget;
	QRect clip;   // empty: none
};
XgCanvas g_cv[4];
QFont g_font;

QPainter *xgPainter(XgCanvas *c) {
	if (!c || c->img.isNull())
		return nullptr;
	if (!c->p) {
		c->p = new QPainter(&c->img);
		c->p->setRenderHint(QPainter::Antialiasing, false);
		c->p->setRenderHint(QPainter::TextAntialiasing, false);
		c->p->setFont(g_font);
		if (!c->clip.isEmpty())
			c->p->setClipRect(c->clip);
	}
	return c->p;
}

QPen xgPen(unsigned int pixel, int style) {
	QPen pen(pixelColor(pixel));
	pen.setWidth(0);                      // one pixel
	if (style == XG_DASHLINE)
		pen.setDashPattern({4, 4});       // X11's default LineOnOffDash
	return pen;
}

void xgEnd(XgCanvas &c) {
	if (c.p) {
		c.p->end();
		delete c.p;
		c.p = nullptr;
	}
}

// one user action is many engine draws: the painters stay open across them, the widgets repaint once
void xgFlush() {
	for (XgCanvas &c : g_cv) {
		xgEnd(c);
		if (c.widget)
			c.widget->update();
	}
}

// (re)size a canvas to its drawing area, white
void xgSize(XgCanvas &c, QSize size) {
	if (c.img.size() == size)
		return;
	xgEnd(c);
	c.img = QImage(size, QImage::Format_RGB32);
	c.img.fill(Qt::white);
}

// ---- one Motif XmScale: slider + value label -----------------------------------------------------
struct MnaScale {
	QSlider *s = nullptr;
	QLabel *val = nullptr;
	int decimals = 0;   // XmNdecimalPoints

	QString fmt(int v) const {
		return decimals ? QString::number(v / std::pow(10.0, decimals), 'f', decimals) : QString::number(v);
	}
	void show() const {
		if (s && val)
			val->setText(fmt(s->value()));
	}
};

// ---- the tool ----------------------------------------------------------------------------------
typedef void (*MotifCB)(Widget, XtPointer, XtPointer);
struct Cb {
	MotifCB fn;
	XtPointer client;
};

struct MbNavAdjust {
	MbParking *parking = nullptr;
	MbNavAdjustHost host;
	QMainWindow *win = nullptr;
	QWidget *central = nullptr;
	QListWidget *list = nullptr;
	QPlainTextEdit *messages = nullptr;
	QDialog *naverr = nullptr, *controls = nullptr, *biases = nullptr, *modelplot = nullptr;
	bool placed[4] = {false, false, false, false};   // each dialog placed on its first show
	QAction *addCrossing = nullptr;                  // View > Add and Open New Crossing (mbview's action)

	// the widgets by name (every root's children, actions included) and what has no widget here
	std::map<std::string, QObject *> byName;
	std::map<std::string, QString> virtualText;      // label_message, label_error_*, textField_format, ...
	std::map<std::string, MnaScale> scales;
	std::map<std::string, std::vector<Cb>> cbs;      // the Motif callbacks of each widget
	QString filePattern = "*.nvh";                   // the file selection box's pattern

	// the visualization
	void *view = nullptr;
	QMetaObject::Connection viewGone;
	std::vector<std::string> viewNavPicked;          // the navs highlighted (mbna_view_picknav)
	std::string viewRoutePicked;                     // the route highlighted (mbna_view_pickroute)
	std::vector<std::string> clickedNavs;            // the last two nav clicks, oldest first
	std::map<std::string, std::pair<int, int>> lineStyle;   // name -> (mbview colour, size) as added
	bool inCallback = false;
	bool busy = false;                               // the busy notice is up (bulletinBoard_message managed)
};
MbNavAdjust *g_na = nullptr;

QObject *wobj(const char *name) {
	MbNavAdjust *m = g_na;
	if (!m || !name)
		return nullptr;
	auto it = m->byName.find(name);
	return it == m->byName.end() ? nullptr : it->second;
}

void indexChildren(MbNavAdjust *m, QObject *root) {
	if (!root)
		return;
	if (!root->objectName().isEmpty())
		m->byName[root->objectName().toStdString()] = root;
	for (QObject *o : root->findChildren<QObject *>())
		if (!o->objectName().isEmpty() && !o->objectName().startsWith("qt_"))
			m->byName.emplace(o->objectName().toStdString(), o);
}

// run the callbacks registered on `name`, with the call data given, then show what they drew
void fire(const char *name, XtPointer call = nullptr) {
	MbNavAdjust *m = g_na;
	if (!m)
		return;
	auto it = m->cbs.find(name);
	if (it == m->cbs.end())
		return;
	const std::vector<Cb> cbs = it->second;   // a callback may register nothing, but keep a copy
	const bool outer = !m->inCallback;
	m->inCallback = true;
	for (const Cb &cb : cbs)
		cb.fn(name, cb.client, call);
	if (outer) {
		m->inCallback = false;
		xgFlush();
	}
}

void addCb(MbNavAdjust *m, const char *name, MotifCB fn, XtPointer client = nullptr) {
	m->cbs[name].push_back({fn, client});
}

// ---- small helpers -------------------------------------------------------------------------------
QWidget *loadUi(MbNavAdjust *m, const char *file, QWidget *parent) {
	QFile f(QDir(m->host.base.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBnavadjust", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "MBnavadjust", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

QDialog *loadDialog(MbNavAdjust *m, const char *file) {
	QWidget *w = loadUi(m, file, m->win);
	auto *d = qobject_cast<QDialog *>(w);
	if (!d) {
		delete w;
		return nullptr;
	}
	d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
	if (!m->host.base.icon.isNull())
		d->setWindowIcon(m->host.base.icon);
	d->setModal(false);
	return d;
}

// BuilderXcessory's multi-line label ':::t"one":t"two""three"' (":t" and a closing+opening quote
// both start a new line), or a plain string
QString multiline(const char *str) {
	QString s = QString::fromLatin1(str ? str : "");
	if (!s.startsWith(":::"))
		return s;
	QStringList lines;
	QString cur;
	bool inq = false;
	for (int i = 3; i < s.size(); i++) {
		const QChar ch = s[i];
		if (ch == '"') {
			if (inq) {
				lines << cur;
				cur.clear();
			}
			inq = !inq;
		}
		else if (inq) {
			cur += ch;
		}
	}
	if (!cur.isEmpty())
		lines << cur;
	return lines.join('\n');
}

void showDialog(MbNavAdjust *m, QDialog *d, int slot) {
	if (!d)
		return;
	d->show();
	d->raise();
	d->activateWindow();
	if (!m->placed[slot]) {
		m->placed[slot] = true;
		mbPlaceRight(d, m->win);
	}
}

// ---- the canvases' events: paint the image, turn the mouse into the X event the callbacks read ------
class CanvasFilter : public QObject {
public:
	CanvasFilter(QObject *parent, int idx, const char *name) : QObject(parent), idx_(idx), name_(name) {}

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		auto *w = static_cast<QWidget *>(o);
		XgCanvas &c = g_cv[idx_];
		switch (e->type()) {
		case QEvent::Paint: {
			QPainter p(w);
			if (c.img.isNull())
				p.fillRect(w->rect(), Qt::white);
			else
				p.drawImage(0, 0, c.img);
			return true;
		}
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
		case QEvent::MouseMove: {
			auto *me = static_cast<QMouseEvent *>(e);
			XEvent ev;
			std::memset(&ev, 0, sizeof(ev));
			const int x = int(me->position().x()), y = int(me->position().y());
			int type = e->type() == QEvent::MouseButtonPress ? ButtonPress
			           : e->type() == QEvent::MouseButtonRelease ? ButtonRelease : MotionNotify;
			if (type == MotionNotify && me->buttons() == Qt::NoButton)
				return false;   // X11 reports motion only with a button held (ButtonMotionMask)
			unsigned int button = 0;
			if (type != MotionNotify)
				button = me->button() == Qt::LeftButton ? 1 : me->button() == Qt::MiddleButton ? 2
				         : me->button() == Qt::RightButton ? 3 : 0;
			ev.xany.type = type;
			ev.xbutton.type = type;
			ev.xbutton.x = x;
			ev.xbutton.y = y;
			ev.xbutton.button = button;
			ev.xmotion.type = type;
			ev.xmotion.x = x;
			ev.xmotion.y = y;
			XmAnyCallbackStruct acs;
			acs.reason = XmCR_INPUT;
			acs.event = &ev;
			fire(name_, &acs);
			return true;
		}
		default:
			break;
		}
		return QObject::eventFilter(o, e);
	}

private:
	int idx_;
	const char *name_;
};

// the modelplot window follows its size (do_modelplot_resize); its X is its Dismiss
class ModelplotFilter : public QObject {
public:
	explicit ModelplotFilter(QObject *parent) : QObject(parent) {}

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		MbNavAdjust *m = g_na;
		if (m && o == m->modelplot) {
			if (e->type() == QEvent::Resize && m->modelplot->isVisible()) {
				do_modelplot_resize(m->modelplot->width(), m->modelplot->height());
				fire("drawingArea_modelplot_expose");
			}
			else if (e->type() == QEvent::Close) {
				fire("pushButton_modelplot_dismiss");
			}
		}
		return QObject::eventFilter(o, e);
	}
};

// a dialog's X is its Dismiss button
class DismissFilter : public QObject {
public:
	DismissFilter(QObject *parent, const char *dismiss) : QObject(parent), dismiss_(dismiss) {}

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() == QEvent::Close && g_na) {
			fire(dismiss_);
			static_cast<QWidget *>(o)->hide();
			e->accept();
			return true;
		}
		return QObject::eventFilter(o, e);
	}

private:
	const char *dismiss_;
};

// The path the box hands over, as MB-System takes it on Windows: a project or a grid with FORWARD
// slashes (mbnavadjust splits a project path on '/' only, as on Unix, and every Windows file call takes
// them), a swath file or datalist to import with the native BACKSLASHES (MBIO's datalist reader takes a
// "C:/..." entry for a relative one when the datalist's own path holds a '/', and prefixes its folder).
QString boxPath(int mode, const QString &path) {
	return (mode == MBNA_FILESELECTION_IMPORTDATA || mode == MBNA_FILESELECTION_IMPORTSURVEY)
	           ? QDir::toNativeSeparators(path)
	           : QDir::fromNativeSeparators(path);
}

// ---- the file selection box: the platform dialog + the format id of an import ------------------------
void fileSelection(MbNavAdjust *m, int mode) {
	// what the menu entry's callbacks did: set the mode and the box's pattern
	do_fileselection_mode((Widget) "fileSelectionBox", (XtPointer)(intptr_t)mode, nullptr);
	const QString start = m->host.base.startDir ? m->host.base.startDir() : QString();
	QString path;
	const QString all = "All files (*)";
	if (mode == MBNA_FILESELECTION_NEW)
		path = QFileDialog::getSaveFileName(m->win, "MBnavadjust - New Project", start,
		                                    "MBnavadjust projects (" + m->filePattern + ");;" + all);
	else if (mode == MBNA_FILESELECTION_OPEN)
		path = QFileDialog::getOpenFileName(m->win, "MBnavadjust - Open Project", start,
		                                    "MBnavadjust projects (" + m->filePattern + ");;" + all);
	else if (mode == MBNA_FILESELECTION_REFERENCE)
		path = QFileDialog::getOpenFileName(m->win, "MBnavadjust - Import Reference Grid", start,
		                                    "Grids (" + m->filePattern + " *.nc);;" + all);
	else
		path = QFileDialog::getOpenFileName(m->win, mode == MBNA_FILESELECTION_IMPORTSURVEY
		                                                ? "MBnavadjust - Import Swath Data as Single Survey"
		                                                : "MBnavadjust - Import Swath Data",
		                                    start, "Datalists (" + m->filePattern + ");;Swath files (*.mb*);;" + all);
	if (path.isEmpty()) {
		do_fileselection_cancel((Widget) "fileSelectionBox", nullptr, nullptr);
		return;
	}
	if (m->host.base.rememberDir)
		m->host.base.rememberDir(path);
	m->virtualText["fileSelectionBox_text"] = boxPath(mode, path);
	if (mode == MBNA_FILESELECTION_IMPORTDATA || mode == MBNA_FILESELECTION_IMPORTSURVEY) {
		// the box's format text field, filled from the name as do_fileselection_list fills it
		do_fileselection_list((Widget) "fileSelectionBox_list", nullptr, nullptr);
		bool ok = false;
		const int f = QInputDialog::getInt(m->win, "MBnavadjust", "MBIO Format ID (-1 for a datalist):",
		                                   m->virtualText["textField_format"].trimmed().toInt(), -1, 10000, 1, &ok);
		if (!ok) {
			do_fileselection_cancel((Widget) "fileSelectionBox", nullptr, nullptr);
			return;
		}
		m->virtualText["textField_format"] = QString::number(f);
	}
	do_fileselection_ok((Widget) "fileSelectionBox", nullptr, nullptr);
	xgFlush();
}

void about(MbNavAdjust *m) {
	QMessageBox::about(m->win, "About MBnavadjust",
		QString("<b>MBnavadjust</b><br>Interactive Navigation Adjustment of Swath Bathymetry<br><br>"
		        "One Component of the <b>MB-System</b> Open Source Software Package<br>"
		        "for Processing and Display of Swath Sonar Data<br><br>"
		        "Created by: David W. Caress and Dale N. Chayes<br>"
		        "Monterey Bay Aquarium Research Institute &nbsp; / &nbsp; Lamont-Doherty Earth Observatory<br><br>"
		        "MB-System library in use: %1<br><br>"
		        "Ported to InteractiveGMT (Qt) from MB-System's mbnavadjust.")
		    .arg(QString::fromLatin1(MB_VERSION)));
}

// Help > How to use MBnavadjust
void help(MbNavAdjust *m) {
	static QPointer<QDialog> dlg;
	if (!dlg) {
		dlg = new QDialog(m->win);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowTitle("How to use MBnavadjust");
		dlg->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		auto *lay = new QVBoxLayout(dlg);
		lay->setContentsMargins(4, 4, 4, 4);
		auto *tb = new QTextBrowser(dlg);
		tb->setOpenExternalLinks(false);
		tb->setHtml(
			"<h3>MBnavadjust &mdash; interactive navigation adjustment</h3>"
			"<p>MBnavadjust makes the bathymetry of overlapping and crossing swath lines agree by adjusting "
			"their navigation. Each file is cut into <b>sections</b>; where two sections overlap they form a "
			"<b>crossing</b>. At each crossing you set (or let the program find) the offset that makes the two "
			"sections' contours match &mdash; a <b>tie</b>. An inversion then turns all the ties into a smooth "
			"navigation adjustment for every file, which mbprocess applies when it makes the processed files.</p>"

			"<h4>1. Project and data</h4>"
			"<p><b>File &gt; New</b> creates a project: a <code>.nvh</code> file and a data folder beside it. "
			"<b>File &gt; Import Swath Data</b> adds swath files, or a datalist (MBIO format id <b>-1</b>), each "
			"file a survey of its own; <i>as Single Survey</i> makes them one survey. The files are cut into "
			"sections (Option &gt; Controls sets their length and number of soundings), the crossings are "
			"found, and the project grids are made. <b>Import Reference Grid</b> adds a topography grid the "
			"sections can be tied to (<b>global ties</b>). <b>File &gt; Open</b> reopens a project.</p>"

			"<h4>2. Look at the project</h4>"
			"<p>The <b>List</b> menu chooses what the list on the right shows &mdash; data files, surveys, "
			"sections, crossings (all, or by overlap), ties, global ties, reference grids &mdash; and the "
			"<b>View</b> menu which of them: all surveys, one survey, one survey-vs-survey block, one file... "
			"<b>View &gt; Visualize Survey</b> opens the project grid in a viewer window with every section's "
			"navigation and every tie drawn on it: click a tie to analyse its crossing, or click two sections "
			"one after the other to open their crossing (<b>View &gt; Add and Open New Crossing</b> makes one "
			"where none was found). <b>View &gt; Show Modelplot</b> plots the adjustment model.</p>"

			"<h4>3. Analyse crossings</h4>"
			"<p>Selecting a crossing in the list, or <b>Action &gt; Analyze Crossings</b>, opens the analysis "
			"window. On the right the two sections' contours: drag with the <b>left</b> button to move section 2 "
			"over section 1, drag with the <b>middle</b> button to zoom, click with the <b>right</b> button to "
			"pick the tie points. On the left the misfit of every offset (click to set the offset; "
			"<i>Minimum Misfit</i> jumps to the best one) and the misfit against the z offset. "
			"<b>Set Offset</b> / <b>Add Tie</b> store the offset as a tie, <b>Skip Crossing</b> leaves the crossing "
			"out, <b>Next Unset</b> moves on. <i>Settings</i> holds the heading and roll biases and the "
			"contouring. <b>Action &gt; Auto-Pick Offsets</b> does all of this by correlation for every unset "
			"crossing listed.</p>"

			"<h4>4. Invert and apply</h4>"
			"<p><b>Action &gt; Invert Navigation</b> solves for the adjustment that fits the ties best "
			"(Option &gt; Controls: <i>Inversion Smoothing</i>). <b>Update Grids</b> applies it to the sections "
			"and grids them again (mbprocess, mbgrid) so you can look at the result. <b>Apply Adjusted "
			"Navigation</b> writes each file's adjusted navigation and sets it in the file's mbprocess "
			"parameters; Option &gt; Controls chooses whether it is used as primary, secondary or tertiary "
			"navigation.</p>"

			"<p>Closing or minimising the window only parks it in Scene Objects; its handle brings it back. "
			"<b>File &gt; Quit</b> saves the project and closes the tool.</p>");
		lay->addWidget(tb);
		dlg->resize(600, 640);
	}
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

// ---- the visualization's line picks -------------------------------------------------------------
// mbview's route colours, as ROUTE_COLOR_* of mbnavadjust_io.h number them
QColor routeColor(int c) {
	switch (c) {
	case ROUTE_COLOR_BLACK:
		return QColor(0, 0, 0);
	case ROUTE_COLOR_WHITE:
		return QColor(255, 255, 255);
	case ROUTE_COLOR_RED:
		return QColor(255, 0, 0);
	case ROUTE_COLOR_YELLOW:
		return QColor(255, 255, 0);
	case ROUTE_COLOR_GREEN:
		return QColor(0, 255, 0);
	case ROUTE_COLOR_BLUEGREEN:
		return QColor(0, 160, 160);
	case ROUTE_COLOR_BLUE:
		return QColor(0, 0, 255);
	case ROUTE_COLOR_PURPLE:
		return QColor(160, 32, 240);
	default:
		return QColor(0, 0, 0);
	}
}

const char *kNavGroup = "MBnavadjust navigation";
const char *kTieGroup = "MBnavadjust ties";

// a tie route's name has a blank ("cccc:t bb:ffff:ss bb:ffff:ss") or three fields ("bb:ffff:ss", a
// global tie); a section's navigation is "ffff:ssss"
bool isRouteName(const std::string &nm) {
	return nm.find(' ') != std::string::npos || std::count(nm.begin(), nm.end(), ':') == 2;
}

void restoreLine(MbNavAdjust *m, const std::string &nm) {
	auto it = m->lineStyle.find(nm);
	if (it == m->lineStyle.end() || !m->host.setLineStyle || !m->view)
		return;
	const QColor c = routeColor(it->second.first);
	m->host.setLineStyle(m->view, nm.c_str(), c.redF(), c.greenF(), c.blueF(), double(it->second.second));
}

void highlightLine(MbNavAdjust *m, const std::string &nm) {
	if (!m->host.setLineStyle || !m->view)
		return;
	auto it = m->lineStyle.find(nm);
	const double w = it == m->lineStyle.end() ? 2.0 : double(it->second.second);
	m->host.setLineStyle(m->view, nm.c_str(), 1.0, 0.0, 0.0, w + 2.0);   // mbview draws picks red
}

void updateAddCrossing(MbNavAdjust *m) {
	if (m->addCrossing)
		m->addCrossing->setEnabled(m->view && mbna_view_can_addcrossing());
}

void onLineClicked(const std::string &nm) {
	MbNavAdjust *m = g_na;
	if (!m || nm.empty())
		return;
	if (isRouteName(nm)) {
		m->clickedNavs.clear();
		mbna_view_picked_route(nm.c_str());
	}
	else {
		// mbview's nav pick: the last two navigation lines clicked are the two sections
		m->clickedNavs.push_back(nm);
		while (m->clickedNavs.size() > 2)
			m->clickedNavs.erase(m->clickedNavs.begin());
		const std::string a = m->clickedNavs[0];
		const std::string b = m->clickedNavs.size() > 1 ? m->clickedNavs[1] : std::string();
		mbna_view_picked_navs(a.c_str(), b.c_str());
		// mbview shows its nav picks itself: here the engine's re-pick does, or these two
		for (const std::string &n : m->clickedNavs)
			highlightLine(m, n);
	}
	updateAddCrossing(m);
	xgFlush();
}

// ---- building the tool ----------------------------------------------------------------------------
class MainFilter : public QObject {
public:
	explicit MainFilter(QObject *parent) : QObject(parent) {}

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		MbNavAdjust *m = g_na;
		if (m && o == m->win && e->type() == QEvent::Resize)
			layout(m);
		return QObject::eventFilter(o, e);
	}

public:
	// the Motif form's attachments: the message text grows down, the list right and down
	static void layout(MbNavAdjust *m) {
		if (!m->central || !m->list || !m->messages)
			return;
		const int W = m->central->width(), H = m->central->height();
		const QRect lr = m->list->geometry();
		m->list->setGeometry(lr.x(), lr.y(), std::max(120, W - lr.x() - 6), std::max(80, H - lr.y() - 6));
		const QRect tr = m->messages->geometry();
		m->messages->setGeometry(tr.x(), tr.y(), tr.width(), std::max(60, H - tr.y() - 6));
	}
};

void quitTool(MbNavAdjust *m) {
	// do_quit, then what the process's end did upstream: the project closed, the windows gone
	fire("pushButton_quit");
	int error = MB_ERROR_NO_ERROR;
	if (project.open)
		mbnavadjust_close_project(mbna_verbose, &project, &error);
	if (project.visualization_status)
		mbnavadjust_dismiss_visualization();
	mbParkQuit(m->parking);
}

MbNavAdjust *build(QWidget *parent, const MbNavAdjustHost &host) {
	auto *m = new MbNavAdjust;
	m->host = host;
	g_na = m;   // the dialogs' loads below index into it
	QWidget *root = loadUi(m, "mbnavadjust.ui", nullptr);
	auto *win = qobject_cast<QMainWindow *>(root);
	if (!win) {
		delete root;
		g_na = nullptr;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.base.icon.isNull())
		win->setWindowIcon(host.base.icon);
	m->naverr = loadDialog(m, "mbnavadjust_naverr.ui");
	m->controls = loadDialog(m, "mbnavadjust_controls.ui");
	m->biases = loadDialog(m, "mbnavadjust_biases.ui");
	m->modelplot = loadDialog(m, "mbnavadjust_modelplot.ui");
	if (!m->naverr || !m->controls || !m->biases || !m->modelplot) {
		delete win;
		g_na = nullptr;
		delete m;
		return nullptr;
	}
	m->modelplot->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint);

	// View > Add and Open New Crossing: mbview's action button "Add and open new crossing"
	m->addCrossing = new QAction("Add and Open New Crossing", win);
	m->addCrossing->setObjectName("pushButton_addcrossing");
	m->addCrossing->setToolTip("Two sections picked in the visualization one after the other, with no crossing "
	                           "between them: make that crossing and open it");
	m->addCrossing->setEnabled(false);
	if (QMenu *view = win->findChild<QMenu *>("pulldownMenu_view")) {
		QAction *vis = win->findChild<QAction *>("pushButton_showmodelplot");
		view->insertAction(vis, m->addCrossing);
	}

	for (QObject *r : std::initializer_list<QObject *>{win, m->naverr, m->controls, m->biases, m->modelplot})
		indexChildren(m, r);

	// every widget the callbacks name must be there: a renamed one in a .ui is reported by name
	static const char *const required[] = {
		"label_status", "text_messages", "label_listdata", "list_data", "form_mbnavadjust",
		"pushButton_new", "pushButton_open", "pushButton_close", "pushButton_importdata", "pushButton_importsurvey",
		"pushButton_importreference", "pushButton_quit", "pushButton_controls", "pushButton_showreferencegrids",
		"pushButton_showsurveys", "pushButton_showblocks", "pushButton_showdata", "pushButton_showsections",
		"pushButton_showcrossings", "pushButton_showmediocrecrossings", "pushButton_showgoodcrossings",
		"pushButton_showbettercrossings", "pushButton_showtruecrossings", "pushButton_showcrossingties",
		"pushButton_showcrossingtiessortedall", "pushButton_showcrossingtiessortedworst",
		"pushButton_showcrossingtiessortedbad", "pushButton_showglobalties", "pushButton_showglobaltiessorted",
		"toggleButton_showallsurveys", "toggleButton_showselectedsurvey", "toggleButton_showselectedblock",
		"toggleButton_showselectedfile", "toggleButton_showwithselectedsurvey", "toggleButton_showwithselectedfile",
		"toggleButton_showselectedsection", "pushButton_visualize", "pushButton_showmodelplot", "pushButton_poornav",
		"pushButton_goodnav", "pushButton_fixednav", "pushButton_fixedxynav", "pushButton_fixedznav",
		"pushButton_tie_xy", "pushButton_tie_z", "pushButton_tie_xyz", "pushButton_tie_unfixed", "pushButton_tie_fixed",
		"pushButton_autopick", "pushButton_autopickhorizontal", "pushButton_autosetsvsvertical",
		"pushButton_newcrossings", "pushButton_analyzecrossings", "pushButton_zerozoffsets", "pushButton_unsetskipped",
		"pushButton_invertnav", "pushButton_updategrids", "pushButton_applynav", "pushButton_about", "actionHelp",
		"bulletinBoard_naverr", "cascadeButton_settings", "pushButton_naverr_previous", "pushButton_naverr_next",
		"pushButton_naverr_nextunset", "pushButton_naverr_setnone", "pushButton_naverr_unset",
		"pushButton_naverr_settie", "pushButton_naverr_addtie", "pushButton_naverr_selecttie",
		"pushButton_naverr_dismiss", "pushButton_naverr_fullsize", "pushButton_naverr_resettie",
		"pushButton_naverr_zerozoffset", "pushButton_naverr_zerooffset", "pushButton_naverr_deletetie",
		"label_naverr_status", "label_naverr_offsets", "drawingArea_naverr_corr", "drawingArea_naverr_zcorr",
		"pushButton_naverr_minimum", "pushButton_naverr_minxymisfit", "toggleButton_misfitcenter_zero",
		"toggleButton_misfitcenter_auto", "drawingArea_naverr_cont", "pushButton_naverr_biases",
		"pushButton_naverr_controls", "form_controls", "scale_controls_sectionlength",
		"scale_controls_sectionsoundings", "scale_controls_decimation", "scale_controls_contourinterval",
		"scale_controls_colorinterval", "scale_controls_tickinterval", "scale_controls_smoothing",
		"scale_controls_zoffset", "toggleButton_controls_use_primary", "toggleButton_controls_use_secondary",
		"toggleButton_controls_use_tertiary", "pushButton_controls_apply", "pushButton_controls_dismiss",
		"bulletinBoard_biases", "label_biases_files", "toggleButton_biases_together", "toggleButton_biases_separate",
		"scale_biases_heading1", "scale_biases_roll1", "scale_biases_heading2", "scale_biases_roll2",
		"pushButton_biases_applyall", "pushButton_biases_apply", "pushButton_biases_dismiss",
		"bulletinBoard_modelplot", "pushButton_modelplot_dismiss", "toggleButton_modelplot_timeseries",
		"toggleButton_modelplot_perturbation", "toggleButton_modelplot_tieoffsets", "pushButton_modelplot_fullsize",
		"pushButton_modelplot_clearblock", "label_modelplot_status", "drawingArea_modelplot",
	};
	QStringList missing;
	for (const char *nm : required)
		if (!wobj(nm))
			missing << QString::fromLatin1(nm);
	for (auto &kv : m->byName)
		if (qobject_cast<QSlider *>(kv.second) && !wobj((kv.first + "Value").c_str()))
			missing << QString::fromStdString(kv.first + "Value");
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBnavadjust", "mbnavadjust*.ui lack: " + missing.join(", "));
		delete win;
		g_na = nullptr;
		delete m;
		return nullptr;
	}
	m->central = qobject_cast<QWidget *>(wobj("form_mbnavadjust"));
	m->list = qobject_cast<QListWidget *>(wobj("list_data"));
	m->messages = qobject_cast<QPlainTextEdit *>(wobj("text_messages"));

	// the scales, by name; their decimal points are set by the callbacks (XmNdecimalPoints)
	for (auto &kv : m->byName)
		if (auto *s = qobject_cast<QSlider *>(kv.second)) {
			MnaScale sc;
			sc.s = s;
			sc.val = qobject_cast<QLabel *>(wobj((kv.first + "Value").c_str()));
			if (kv.first.rfind("scale_biases_", 0) == 0)
				sc.decimals = 1;   // the uil's XmNdecimalPoints 1
			m->scales[kv.first] = sc;
		}
	for (auto &kv : m->scales)
		kv.second.show();

	// the drawing font: -*-fixed-bold-r-normal-*-13-*-75-75-c-70-iso8859-1
	g_font = QFont("Courier New");
	g_font.setStyleHint(QFont::TypeWriter);
	g_font.setBold(true);
	g_font.setPixelSize(13);
	g_font.setStyleStrategy(QFont::NoAntialias);

	// the four drawing areas
	const char *areas[4] = {"drawingArea_naverr_cont", "drawingArea_naverr_corr", "drawingArea_naverr_zcorr",
	                        "drawingArea_modelplot"};
	for (int i = 0; i < 4; i++) {
		auto *w = qobject_cast<QWidget *>(wobj(areas[i]));
		w->setAttribute(Qt::WA_OpaquePaintEvent);
		w->setMouseTracking(false);
		w->setCursor(Qt::CrossCursor);   // XC_target
		w->installEventFilter(new CanvasFilter(w, i, areas[i]));
		g_cv[i].widget = w;
		xgSize(g_cv[i], w->size());
	}
	mbna_window_canvases(&g_cv[0], &g_cv[1], &g_cv[2], &g_cv[3]);

	// ---- the Motif creation code's callbacks, widget by widget ----
	auto onAction = [m](const char *name) {
		if (auto *a = qobject_cast<QAction *>(wobj(name)))
			QObject::connect(a, &QAction::triggered, m->win, [name]() { fire(name); });
		else if (auto *b = qobject_cast<QAbstractButton *>(wobj(name)))
			QObject::connect(b, &QAbstractButton::clicked, m->win, [name]() { fire(name); });
	};
	auto onToggle = [m](const char *name) {
		if (auto *a = qobject_cast<QAction *>(wobj(name)))
			QObject::connect(a, &QAction::triggered, m->win, [name]() { fire(name); });
		else if (auto *b = qobject_cast<QAbstractButton *>(wobj(name)))
			QObject::connect(b, &QAbstractButton::toggled, m->win, [name]() { fire(name); });
	};
	// Motif's XmScale called back on release (and on keyboard steps); the value label follows the drag
	auto onScale = [m](const char *name) {
		MnaScale *sc = &m->scales[name];
		QSlider *s = sc->s;
		QObject::connect(s, &QSlider::valueChanged, s, [s, sc, name](int) {
			sc->show();
			if (!s->isSliderDown())
				fire(name);
		});
		QObject::connect(s, &QSlider::sliderReleased, s, [name]() { fire(name); });
	};

	// File
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_new")), &QAction::triggered, win,
	                 [m]() { fileSelection(m, MBNA_FILESELECTION_NEW); });
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_open")), &QAction::triggered, win,
	                 [m]() { fileSelection(m, MBNA_FILESELECTION_OPEN); });
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_importdata")), &QAction::triggered, win,
	                 [m]() { fileSelection(m, MBNA_FILESELECTION_IMPORTDATA); });
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_importsurvey")), &QAction::triggered, win,
	                 [m]() { fileSelection(m, MBNA_FILESELECTION_IMPORTSURVEY); });
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_importreference")), &QAction::triggered, win,
	                 [m]() { fileSelection(m, MBNA_FILESELECTION_REFERENCE); });
	addCb(m, "pushButton_close", do_file_close);
	onAction("pushButton_close");
	addCb(m, "pushButton_quit", do_quit);
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_quit")), &QAction::triggered, win,
	                 [m]() { quitTool(m); });
	// Option
	addCb(m, "pushButton_controls", do_controls_show);
	addCb(m, "pushButton_controls", [](Widget, XtPointer, XtPointer) { XtManageChild("form_controls"); });
	onAction("pushButton_controls");
	// List
	const struct {
		const char *name;
		MotifCB fn;
	} lists[] = {
		{"pushButton_showreferencegrids", do_list_showreferencegrids},
		{"pushButton_showsections", do_list_showsections},
		{"pushButton_showglobalties", do_list_showglobalties},
		{"pushButton_showglobaltiessorted", do_list_showglobaltiessorted},
		{"pushButton_showdata", do_list_showdata},
		{"pushButton_showsurveys", do_list_showsurveys},
		{"pushButton_showblocks", do_list_showblocks},
		{"pushButton_showcrossings", do_list_showcrossings},
		{"pushButton_showmediocrecrossings", do_list_showmediocrecrossings},
		{"pushButton_showgoodcrossings", do_list_showgoodcrossings},
		{"pushButton_showbettercrossings", do_list_showbettercrossings},
		{"pushButton_showtruecrossings", do_list_showtruecrossings},
		{"pushButton_showcrossingties", do_list_showcrossingties},
		{"pushButton_showcrossingtiessortedall", do_list_showcrossingtiessortedall},
		{"pushButton_showcrossingtiessortedbad", do_list_showcrossingtiessortedbad},
		{"pushButton_showcrossingtiessortedworst", do_list_showcrossingtiessortedworst},
	};
	for (const auto &l : lists) {
		addCb(m, l.name, l.fn);
		onAction(l.name);
	}
	// View
	const struct {
		const char *name;
		MotifCB fn;
	} views[] = {
		{"toggleButton_showallsurveys", do_view_showallsurveys},
		{"toggleButton_showselectedsurvey", do_view_showselectedsurvey},
		{"toggleButton_showselectedblock", do_view_showselectedblock},
		{"toggleButton_showselectedfile", do_view_showselectedfile},
		{"toggleButton_showwithselectedsurvey", do_view_showwithselectedsurvey},
		{"toggleButton_showwithselectedfile", do_view_showwithselectedfile},
		{"toggleButton_showselectedsection", do_view_showselectedsection},
	};
	for (const auto &v : views) {
		addCb(m, v.name, v.fn);
		onToggle(v.name);
	}
	addCb(m, "pushButton_visualize", do_visualize);
	onAction("pushButton_visualize");
	addCb(m, "pushButton_showmodelplot", [](Widget, XtPointer, XtPointer) { XtManageChild("bulletinBoard_modelplot"); });
	addCb(m, "pushButton_showmodelplot", do_modelplot_show);
	onAction("pushButton_showmodelplot");
	addCb(m, "pushButton_addcrossing", do_mbnavadjust_addcrossing);
	onAction("pushButton_addcrossing");
	// Action
	const struct {
		const char *name;
		MotifCB fn;
	} actions[] = {
		{"pushButton_poornav", do_action_poornav},
		{"pushButton_goodnav", do_action_goodnav},
		{"pushButton_fixednav", do_action_fixednav},
		{"pushButton_fixedxynav", do_action_fixedxynav},
		{"pushButton_fixedznav", do_action_fixedznav},
		{"pushButton_tie_xy", do_action_tie_xy},
		{"pushButton_tie_z", do_action_tie_z},
		{"pushButton_tie_xyz", do_action_tie_xyz},
		{"pushButton_tie_unfixed", do_action_tie_unfixed},
		{"pushButton_tie_fixed", do_action_tie_fixed},
		{"pushButton_autopick", do_action_autopick},
		{"pushButton_autopickhorizontal", do_action_autopickhorizontal},
		{"pushButton_autosetsvsvertical", do_action_autosetsvsvertical},
		{"pushButton_newcrossings", do_action_checknewcrossings},
		{"pushButton_zerozoffsets", do_zerozoffsets},
		{"pushButton_unsetskipped", do_unsetskipped},
		{"pushButton_invertnav", do_action_invertnav},
		{"pushButton_updategrids", do_action_updategrids},
		{"pushButton_applynav", do_apply_nav},
	};
	for (const auto &a : actions) {
		addCb(m, a.name, a.fn);
		onAction(a.name);
	}
	// Analyze Crossings: BxManageCB(bulletinBoard_naverr), do_naverr_init, do_action_analyzecrossings --
	// port: do_naverr_init gets the mode of the list shown (upstream hands it the Widget, see the top)
	addCb(m, "pushButton_analyzecrossings", [](Widget, XtPointer, XtPointer) {
		XtManageChild("bulletinBoard_naverr");
		const bool sections = mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS || mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES ||
		                      mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED;
		do_naverr_init(sections ? MBNA_NAVERR_MODE_SECTION : MBNA_NAVERR_MODE_CROSSING);
	});
	addCb(m, "pushButton_analyzecrossings", do_action_analyzecrossings);
	onAction("pushButton_analyzecrossings");
	// Help
	QObject::connect(qobject_cast<QAction *>(wobj("pushButton_about")), &QAction::triggered, win, [m]() { about(m); });
	QObject::connect(qobject_cast<QAction *>(wobj("actionHelp")), &QAction::triggered, win, [m]() { help(m); });

	// the list: XmNsingleSelectionCallback
	addCb(m, "list_data", do_list_data_select);
	QObject::connect(m->list, &QListWidget::itemClicked, win, [m](QListWidgetItem *it) {
		const int row = m->list->row(it);
		QByteArray text = it->text().toLatin1();
		XmString items[1] = {text.data()};
		XmListCallbackStruct acs;
		acs.reason = 0;
		acs.item_position = row + 1;
		acs.item = text.data();
		acs.selected_item_count = 1;
		acs.selected_items = items;
		fire("list_data", &acs);
	});

	// the analysis window
	auto *settings = qobject_cast<QPushButton *>(wobj("cascadeButton_settings"));
	auto *settingsMenu = new QMenu(settings);
	settingsMenu->addAction(qobject_cast<QAction *>(wobj("pushButton_naverr_biases")));
	settingsMenu->addAction(qobject_cast<QAction *>(wobj("pushButton_naverr_controls")));
	settings->setMenu(settingsMenu);
	const struct {
		const char *name;
		MotifCB fn;
	} naverrs[] = {
		{"pushButton_naverr_unset", do_naverr_unset},
		{"pushButton_naverr_minxymisfit", do_naverr_minxymisfit},
		{"pushButton_naverr_zerozoffset", do_naverr_zerozoffset},
		{"pushButton_naverr_settie", do_naverr_setoffset},
		{"pushButton_naverr_resettie", do_naverr_resettie},
		{"pushButton_naverr_addtie", do_naverr_addtie},
		{"pushButton_naverr_deletetie", do_naverr_deletetie},
		{"pushButton_naverr_fullsize", do_naverr_fullsize},
		{"pushButton_naverr_zerooffset", do_naverr_zerooffset},
		{"pushButton_naverr_nextunset", do_naverr_nextunset},
		{"pushButton_naverr_selecttie", do_naverr_selecttie},
		{"pushButton_naverr_minimum", do_naverr_minmisfit},
		{"pushButton_naverr_next", do_naverr_next},
		{"pushButton_naverr_previous", do_naverr_previous},
		{"pushButton_naverr_setnone", do_naverr_setnone},
	};
	for (const auto &n : naverrs) {
		addCb(m, n.name, n.fn);
		onAction(n.name);
	}
	addCb(m, "pushButton_naverr_dismiss", do_naverr_dismiss);
	addCb(m, "pushButton_naverr_dismiss", [](Widget, XtPointer, XtPointer) {
		XtUnmanageChild("bulletinBoard_biases");
		XtUnmanageChild("bulletinBoard_naverr");
	});
	onAction("pushButton_naverr_dismiss");
	addCb(m, "pushButton_naverr_biases", [](Widget, XtPointer, XtPointer) { XtManageChild("bulletinBoard_biases"); });
	addCb(m, "pushButton_naverr_biases", do_biases_init);
	onAction("pushButton_naverr_biases");
	addCb(m, "pushButton_naverr_controls", do_controls_show);
	addCb(m, "pushButton_naverr_controls", [](Widget, XtPointer, XtPointer) { XtManageChild("form_controls"); });
	onAction("pushButton_naverr_controls");
	auto *misfitGroup = new QButtonGroup(m->naverr);
	misfitGroup->addButton(qobject_cast<QAbstractButton *>(wobj("toggleButton_misfitcenter_zero")));
	misfitGroup->addButton(qobject_cast<QAbstractButton *>(wobj("toggleButton_misfitcenter_auto")));
	addCb(m, "toggleButton_misfitcenter_zero", do_naverr_misfitcenter);
	addCb(m, "toggleButton_misfitcenter_auto", do_naverr_misfitcenter);
	onToggle("toggleButton_misfitcenter_zero");
	onToggle("toggleButton_misfitcenter_auto");
	addCb(m, "drawingArea_naverr_cont", do_naverr_cont_input);
	addCb(m, "drawingArea_naverr_corr", do_naverr_corr_input);
	addCb(m, "drawingArea_naverr_zcorr", do_naverr_zcorr_input);
	m->naverr->installEventFilter(new DismissFilter(m->naverr, "pushButton_naverr_dismiss"));

	// the biases
	auto *biasGroup = new QButtonGroup(m->biases);
	biasGroup->addButton(qobject_cast<QAbstractButton *>(wobj("toggleButton_biases_together")));
	biasGroup->addButton(qobject_cast<QAbstractButton *>(wobj("toggleButton_biases_separate")));
	addCb(m, "pushButton_biases_applyall", do_biases_applyall);
	onAction("pushButton_biases_applyall");
	addCb(m, "pushButton_biases_apply", do_biases_apply);
	onAction("pushButton_biases_apply");
	addCb(m, "toggleButton_biases_together", do_biases_toggle);
	onToggle("toggleButton_biases_together");
	addCb(m, "scale_biases_roll1", do_biases_roll);
	onScale("scale_biases_roll1");
	addCb(m, "scale_biases_heading1", do_biases_heading);
	onScale("scale_biases_heading1");
	for (const char *s : {"scale_biases_roll2", "scale_biases_heading2"}) {
		MnaScale *sc = &m->scales[s];
		QObject::connect(sc->s, &QSlider::valueChanged, sc->s, [sc](int) { sc->show(); });
	}
	addCb(m, "pushButton_biases_dismiss", [](Widget, XtPointer, XtPointer) { XtUnmanageChild("bulletinBoard_biases"); });
	onAction("pushButton_biases_dismiss");
	m->biases->installEventFilter(new DismissFilter(m->biases, "pushButton_biases_dismiss"));

	// the controls
	const struct {
		const char *name;
		MotifCB fn;
	} scales[] = {
		{"scale_controls_sectionlength", do_scale_controls_sectionlength},
		{"scale_controls_sectionsoundings", do_scale_controls_sectionsoundings},
		{"scale_controls_zoffset", do_scale_controls_zoffset},
		{"scale_controls_smoothing", do_scale_controls_smoothing},
		{"scale_controls_tickinterval", do_scale_controls_tickinterval},
		{"scale_controls_colorinterval", do_controls_scale_colorinterval},
		{"scale_controls_contourinterval", do_scale_contourinterval},
		{"scale_controls_decimation", do_scale_controls_decimation},
	};
	for (const auto &s : scales) {
		addCb(m, s.name, s.fn);
		onScale(s.name);
	}
	auto *useGroup = new QButtonGroup(m->controls);
	for (const char *s : {"toggleButton_controls_use_primary", "toggleButton_controls_use_secondary",
	                      "toggleButton_controls_use_tertiary"})
		useGroup->addButton(qobject_cast<QAbstractButton *>(wobj(s)));
	addCb(m, "pushButton_controls_apply", do_controls_apply);
	onAction("pushButton_controls_apply");
	addCb(m, "pushButton_controls_dismiss", [](Widget, XtPointer, XtPointer) { XtUnmanageChild("form_controls"); });
	onAction("pushButton_controls_dismiss");
	m->controls->installEventFilter(new DismissFilter(m->controls, "pushButton_controls_dismiss"));

	// the modelplot
	auto *styleGroup = new QButtonGroup(m->modelplot);
	for (const char *s : {"toggleButton_modelplot_timeseries", "toggleButton_modelplot_perturbation",
	                      "toggleButton_modelplot_tieoffsets"})
		styleGroup->addButton(qobject_cast<QAbstractButton *>(wobj(s)));
	addCb(m, "pushButton_modelplot_clearblock", do_modelplot_clearblock);
	onAction("pushButton_modelplot_clearblock");
	addCb(m, "toggleButton_modelplot_timeseries", do_modelplot_timeseries);
	onToggle("toggleButton_modelplot_timeseries");
	addCb(m, "toggleButton_modelplot_perturbation", do_modelplot_perturbation);
	onToggle("toggleButton_modelplot_perturbation");
	addCb(m, "toggleButton_modelplot_tieoffsets", do_modelplot_tieoffsets);
	onToggle("toggleButton_modelplot_tieoffsets");
	addCb(m, "pushButton_modelplot_fullsize", do_modelplot_fullsize);
	onAction("pushButton_modelplot_fullsize");
	addCb(m, "drawingArea_modelplot", do_modelplot_input);
	addCb(m, "drawingArea_modelplot_expose", do_modelplot_expose);
	addCb(m, "pushButton_modelplot_dismiss", [](Widget, XtPointer, XtPointer) { XtUnmanageChild("bulletinBoard_modelplot"); });
	addCb(m, "pushButton_modelplot_dismiss", do_modelplot_dismiss);
	onAction("pushButton_modelplot_dismiss");
	m->modelplot->installEventFilter(new ModelplotFilter(m->modelplot));

	win->installEventFilter(new MainFilter(win));
	m->parking = mbParkable(win, host.base, "mbnavadjust");
	return m;
}

} // namespace

// ---- the bridge's window side (mbna_motif.h), and the engine's hooks (mbnavadjust.h) ------------
extern "C" {

void mbna_XtVaSetValues(Widget w, ...) {
	MbNavAdjust *m = g_na;
	va_list ap;
	va_start(ap, w);
	for (;;) {
		const char *res = va_arg(ap, const char *);
		if (!res)
			break;
		if (std::strcmp(res, XmNlabelString) == 0) {
			const char *s = va_arg(ap, const char *);
			mbna_set_label_multiline_string(w, (String)s);
			continue;
		}
		const int v = va_arg(ap, int);
		QObject *o = wobj(w);
		if (!m || !o)
			continue;
		if (std::strcmp(res, XmNsensitive) == 0) {
			if (auto *a = qobject_cast<QAction *>(o))
				a->setEnabled(v != 0);
			else if (auto *wd = qobject_cast<QWidget *>(o))
				wd->setEnabled(v != 0);
			// a scale's value label greys with it
			auto it = m->scales.find(w);
			if (it != m->scales.end() && it->second.val)
				it->second.val->setEnabled(v != 0);
		}
		else if (std::strcmp(res, XmNwidth) == 0 || std::strcmp(res, XmNheight) == 0) {
			if (auto *wd = qobject_cast<QWidget *>(o)) {
				QSize sz = wd->size();
				if (res[0] == 'w')
					sz.setWidth(v);
				else
					sz.setHeight(v);
				wd->resize(sz);
				for (XgCanvas &c : g_cv)
					if (c.widget == wd)
						xgSize(c, sz);
			}
		}
		else {
			auto it = m->scales.find(w);
			if (it == m->scales.end())
				continue;
			MnaScale &sc = it->second;
			QSignalBlocker b(sc.s);
			if (std::strcmp(res, XmNvalue) == 0)
				sc.s->setValue(v);
			else if (std::strcmp(res, XmNminimum) == 0)
				sc.s->setMinimum(v);
			else if (std::strcmp(res, XmNmaximum) == 0)
				sc.s->setMaximum(v);
			else if (std::strcmp(res, XmNdecimalPoints) == 0)
				sc.decimals = v;
			sc.show();
		}
	}
	va_end(ap);
}

void mbna_XtVaGetValues(Widget w, ...) {
	MbNavAdjust *m = g_na;
	va_list ap;
	va_start(ap, w);
	for (;;) {
		const char *res = va_arg(ap, const char *);
		if (!res)
			break;
		QObject *o = wobj(w);
		if (std::strcmp(res, XmNwidth) == 0 || std::strcmp(res, XmNheight) == 0) {
			auto *d = va_arg(ap, Dimension *);
			auto *wd = qobject_cast<QWidget *>(o);
			*d = Dimension(wd ? (res[0] == 'w' ? wd->width() : wd->height()) : 0);
			continue;
		}
		int *v = va_arg(ap, int *);
		auto it = m ? m->scales.find(w) : decltype(m->scales.end())();
		if (!m || it == m->scales.end()) {
			*v = 0;
			continue;
		}
		const MnaScale &sc = it->second;
		if (std::strcmp(res, XmNvalue) == 0)
			*v = sc.s->value();
		else if (std::strcmp(res, XmNminimum) == 0)
			*v = sc.s->minimum();
		else if (std::strcmp(res, XmNmaximum) == 0)
			*v = sc.s->maximum();
		else if (std::strcmp(res, XmNdecimalPoints) == 0)
			*v = sc.decimals;
		else
			*v = 0;
	}
	va_end(ap);
}

void mbna_XtSetValues(Widget w, Arg *args, Cardinal n) {
	MbNavAdjust *m = g_na;
	if (!m)
		return;
	for (Cardinal i = 0; i < n; i++) {
		if (std::strcmp(args[i].name, XmNpattern) == 0) {
			m->filePattern = QString::fromLatin1(reinterpret_cast<const char *>(args[i].value));
		}
		else if (std::strcmp(args[i].name, XmNwidth) == 0 || std::strcmp(args[i].name, XmNheight) == 0) {
			if (args[i].name[0] == 'w')
				mbna_XtVaSetValues(w, XmNwidth, int(args[i].value), nullptr);
			else
				mbna_XtVaSetValues(w, XmNheight, int(args[i].value), nullptr);
		}
	}
}

void mbna_XmToggleButtonSetState(Widget w, Boolean state, Boolean notify) {
	QObject *o = wobj(w);
	if (!o)
		return;
	if (auto *a = qobject_cast<QAction *>(o)) {
		const bool was = a->isChecked();
		{
			QSignalBlocker b(a);
			a->setChecked(state != 0);
		}
		if (notify && was != (state != 0))
			fire(w);
		return;
	}
	auto *btn = qobject_cast<QAbstractButton *>(o);
	if (!btn)
		return;
	// a radio box: the others change too, and Motif calls each one's callback
	std::vector<std::pair<QAbstractButton *, bool>> before;
	if (QButtonGroup *g = btn->group())
		for (QAbstractButton *x : g->buttons())
			before.push_back({x, x->isChecked()});
	else
		before.push_back({btn, btn->isChecked()});
	for (auto &p : before)
		p.first->blockSignals(true);
	btn->setChecked(state != 0);
	for (auto &p : before)
		p.first->blockSignals(false);
	if (notify)
		for (auto &p : before)
			if (p.first->isChecked() != p.second)
				fire(p.first->objectName().toLatin1().constData());
}

Boolean mbna_XmToggleButtonGetState(Widget w) {
	QObject *o = wobj(w);
	if (auto *a = qobject_cast<QAction *>(o))
		return a->isChecked() ? 1 : 0;
	if (auto *b = qobject_cast<QAbstractButton *>(o))
		return b->isChecked() ? 1 : 0;
	return 0;
}

void mbna_XmListDeleteAllItems(Widget w) {
	if (auto *l = qobject_cast<QListWidget *>(wobj(w))) {
		QSignalBlocker b(l);
		l->clear();
	}
}

void mbna_XmListAddItems(Widget w, XmString *items, int item_count, int position) {
	auto *l = qobject_cast<QListWidget *>(wobj(w));
	if (!l)
		return;
	QStringList s;
	s.reserve(item_count);
	for (int i = 0; i < item_count; i++)
		s << QString::fromLatin1(items[i] ? items[i] : "");
	QSignalBlocker b(l);
	if (position <= 0 || position > l->count())
		l->addItems(s);   // 0: at the end
	else
		l->insertItems(position - 1, s);
}

void mbna_XmListSelectPos(Widget w, int position, Boolean notify) {
	auto *l = qobject_cast<QListWidget *>(wobj(w));
	if (!l)
		return;
	const int row = position <= 0 ? l->count() - 1 : position - 1;   // 0: the last item
	if (row < 0 || row >= l->count())
		return;
	{
		QSignalBlocker b(l);
		l->setCurrentRow(row);
	}
	if (notify) {
		QByteArray text = l->item(row)->text().toLatin1();
		XmString items[1] = {text.data()};
		XmListCallbackStruct acs;
		acs.reason = 0;
		acs.item_position = row + 1;
		acs.item = text.data();
		acs.selected_item_count = 1;
		acs.selected_items = items;
		fire(w, &acs);
	}
}

void mbna_XmListSetPos(Widget w, int position) {
	auto *l = qobject_cast<QListWidget *>(wobj(w));
	if (!l || l->count() == 0)
		return;
	const int row = std::max(0, std::min(l->count() - 1, position <= 0 ? l->count() - 1 : position - 1));
	l->scrollToItem(l->item(row), QAbstractItemView::PositionAtTop);
}

Boolean mbna_XmListGetSelectedPos(Widget w, int **position_list, int *position_count) {
	auto *l = qobject_cast<QListWidget *>(wobj(w));
	*position_list = nullptr;
	*position_count = 0;
	if (!l || l->currentRow() < 0 || !l->currentItem() || !l->currentItem()->isSelected())
		return 0;
	int *p = static_cast<int *>(std::malloc(sizeof(int)));   // XtFree'd by the caller
	if (!p)
		return 0;
	p[0] = l->currentRow() + 1;
	*position_list = p;
	*position_count = 1;
	return 1;
}

void mbna_XtManageChild(Widget w) {
	MbNavAdjust *m = g_na;
	if (!m || !w)
		return;
	if (std::strcmp(w, "bulletinBoard_message") == 0) {
		m->busy = true;
		if (m->host.base.busyText)
			m->host.base.busyText(m->virtualText["label_message"].toUtf8().constData());
		return;
	}
	if (std::strcmp(w, "bulletinBoard_error") == 0) {
		QMessageBox::warning(m->win, "MBnavadjust",
		                     m->virtualText["label_error_one"] + "\n" + m->virtualText["label_error_two"] + "\n" +
		                         m->virtualText["label_error_three"]);
		return;
	}
	const char *dlgs[4] = {"bulletinBoard_naverr", "form_controls", "bulletinBoard_biases", "bulletinBoard_modelplot"};
	QDialog *ds[4] = {m->naverr, m->controls, m->biases, m->modelplot};
	for (int i = 0; i < 4; i++)
		if (std::strcmp(w, dlgs[i]) == 0) {
			showDialog(m, ds[i], i);
			if (ds[i] == m->modelplot)   // its drawing area follows the window from now on
				do_modelplot_resize(m->modelplot->width(), m->modelplot->height());
			return;
		}
	if (auto *wd = qobject_cast<QWidget *>(wobj(w)))
		wd->show();
}

void mbna_XtUnmanageChild(Widget w) {
	MbNavAdjust *m = g_na;
	if (!m || !w)
		return;
	if (std::strcmp(w, "bulletinBoard_message") == 0) {
		m->busy = false;
		if (m->host.base.busyOff)
			m->host.base.busyOff();
		return;
	}
	if (auto *wd = qobject_cast<QWidget *>(wobj(w)))
		wd->hide();
}

void mbna_XmUpdateDisplay(Widget) {
	xgFlush();
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void mbna_XmTextFieldSetString(Widget w, char *value) {
	MbNavAdjust *m = g_na;
	if (!m)
		return;
	if (auto *e = qobject_cast<QLineEdit *>(wobj(w)))
		e->setText(QString::fromLatin1(value ? value : ""));
	else
		m->virtualText[w] = QString::fromLatin1(value ? value : "");
}

void mbna_mb_get_text_string(Widget w, char *str, size_t len) {
	MbNavAdjust *m = g_na;
	if (!str || len == 0)
		return;
	QString s;
	if (auto *e = qobject_cast<QLineEdit *>(wobj(w)))
		s = e->text();
	else if (m)
		s = m->virtualText[w];
	std::snprintf(str, len, "%s", s.toLocal8Bit().constData());
}

void mbna_text_append(Widget w, const char *text) {
	auto *t = qobject_cast<QPlainTextEdit *>(wobj(w));
	if (!t)
		return;
	t->moveCursor(QTextCursor::End);
	t->insertPlainText(QString::fromLocal8Bit(text ? text : ""));
	t->moveCursor(QTextCursor::End);
	t->ensureCursorVisible();
}

void mbna_set_label_string(Widget w, String str) {
	MbNavAdjust *m = g_na;
	const QString s = QString::fromLatin1(str ? str : "");
	QObject *o = wobj(w);
	if (auto *l = qobject_cast<QLabel *>(o))
		l->setText(s);
	else if (auto *b = qobject_cast<QAbstractButton *>(o))
		b->setText(s);
	else if (auto *a = qobject_cast<QAction *>(o))
		a->setText(s);
	else if (m)
		m->virtualText[w] = s;
	// the busy notice follows its label while it is up (do_message_update)
	if (m && w && std::strcmp(w, "label_message") == 0 && m->busy && m->host.base.busyText)
		m->host.base.busyText(s.toUtf8().constData());
}

void mbna_set_label_multiline_string(Widget w, String str) {
	const QString s = multiline(str);
	const QByteArray b = s.toLatin1();
	mbna_set_label_string(w, const_cast<char *>(b.constData()));
}

void mbna_XBell(int) {
	QApplication::beep();
}

// ---- xg_* ----------------------------------------------------------------------------------------
void xg_drawline(void *xgid, int x1, int y1, int x2, int y2, unsigned int pixel, int style) {
	if (QPainter *p = xgPainter(static_cast<XgCanvas *>(xgid))) {
		p->setPen(xgPen(pixel, style));
		p->drawLine(x1, y1, x2, y2);
	}
}

void xg_drawrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style) {
	if (QPainter *p = xgPainter(static_cast<XgCanvas *>(xgid))) {
		p->setPen(xgPen(pixel, style));
		p->setBrush(Qt::NoBrush);
		p->drawRect(x, y, width, height);
	}
}

void xg_fillrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int) {
	if (QPainter *p = xgPainter(static_cast<XgCanvas *>(xgid)))
		p->fillRect(x, y, width, height, pixelColor(pixel));
}

void xg_drawstring(void *xgid, int x, int y, char *string, unsigned int pixel, int style) {
	if (QPainter *p = xgPainter(static_cast<XgCanvas *>(xgid))) {
		p->setPen(xgPen(pixel, style));
		p->drawText(x, y, QString::fromLatin1(string));
	}
}

void xg_justify(void *, char *string, int *width, int *ascent, int *descent) {
	const QFontMetrics fm(g_font);
	*width = fm.horizontalAdvance(QString::fromLatin1(string));
	*ascent = fm.ascent();
	*descent = fm.descent();
}

void xg_setclip(void *xgid, int x, int y, int width, int height) {
	auto *c = static_cast<XgCanvas *>(xgid);
	if (!c)
		return;
	c->clip = QRect(x, y, width, height);
	if (c->p)
		c->p->setClipRect(c->clip);
}

// ---- the MB-System programs upstream runs through system() ------------------------------------------
int mbna_system(const char *command) {
	MbNavAdjust *m = g_na;
	if (!m || !m->host.run)
		return -1;
	return m->host.run(command);
}

// ---- the visualization ---------------------------------------------------------------------------
bool mbna_view_open(const char *title, const char *gridfile) {
	MbNavAdjust *m = g_na;
	if (!m || !m->host.openFile)
		return false;
	if (!QFileInfo::exists(QString::fromLocal8Bit(gridfile))) {
		QMessageBox::warning(m->win, "MBnavadjust",
		                     QString("There is no project grid to visualize yet:\n%1\n\nRun Action > Update Grids first.")
		                         .arg(QString::fromLocal8Bit(gridfile)));
		return false;
	}
	void *v = m->host.openFile(gridfile, title);
	if (!v)
		return false;
	m->view = v;
	m->lineStyle.clear();
	m->viewNavPicked.clear();
	m->viewRoutePicked.clear();
	m->clickedNavs.clear();
	if (QWidget *vw = m->host.window ? m->host.window(v) : nullptr) {
		QObject::disconnect(m->viewGone);
		m->viewGone = QObject::connect(vw, &QObject::destroyed, m->win, [m, v]() {
			if (m->view != v)
				return;
			m->view = nullptr;
			mbna_view_closed();
			updateAddCrossing(m);
			xgFlush();
		});
	}
	if (m->host.pickLine)
		m->host.pickLine(v, [](const std::string &nm) { onLineClicked(nm); });
	return true;
}

void mbna_view_destroy(void) {
	MbNavAdjust *m = g_na;
	if (!m || !m->view)
		return;
	void *v = m->view;
	m->view = nullptr;
	QObject::disconnect(m->viewGone);
	if (m->host.pickLine && m->host.alive && m->host.alive(v))
		m->host.pickLine(v, nullptr);
	if (QWidget *vw = (m->host.alive && m->host.alive(v) && m->host.window) ? m->host.window(v) : nullptr)
		vw->close();
	updateAddCrossing(m);
}

void mbna_view_update(void) {
	MbNavAdjust *m = g_na;
	if (m && m->view && m->host.render && (!m->host.alive || m->host.alive(m->view)))
		m->host.render(m->view);
	if (m)
		updateAddCrossing(m);
}

bool mbna_view_addnav(const char *name, int npts, const double *lon, const double *lat, const double *, int color,
                      int size) {
	MbNavAdjust *m = g_na;
	if (!m || !m->view || !m->host.addLine || npts < 2)
		return false;
	std::vector<double> xyz(size_t(npts) * 3);
	for (int i = 0; i < npts; i++) {
		xyz[3 * size_t(i)] = lon[i];
		xyz[3 * size_t(i) + 1] = lat[i];
		xyz[3 * size_t(i) + 2] = 0.0;   // drawn on the grid: the host clamps a track to it
	}
	const QColor c = routeColor(color);
	m->lineStyle[name] = {color, size};
	return m->host.addLine(m->view, xyz.data(), npts, c.redF(), c.greenF(), c.blueF(), double(size), name, kNavGroup);
}

void mbna_view_deleteallroutes(void) {
	MbNavAdjust *m = g_na;
	if (!m || !m->view || !m->host.removeGroup)
		return;
	m->host.removeGroup(m->view, kTieGroup);
	for (auto it = m->lineStyle.begin(); it != m->lineStyle.end();)
		it = isRouteName(it->first) ? m->lineStyle.erase(it) : std::next(it);
	m->viewRoutePicked.clear();
}

bool mbna_view_addroute(int npts, const double *lon, const double *lat, int color, int size, const char *name) {
	MbNavAdjust *m = g_na;
	if (!m || !m->view || !m->host.addLine || npts < 1)
		return false;
	std::vector<double> xyz(size_t(npts) * 3, 0.0);
	for (int i = 0; i < npts; i++) {
		xyz[3 * size_t(i)] = lon[i];
		xyz[3 * size_t(i) + 1] = lat[i];
	}
	const QColor c = routeColor(color);
	m->lineStyle[name] = {color, size + 1};   // a tie stands out over the tracks
	return m->host.addLine(m->view, xyz.data(), npts, c.redF(), c.greenF(), c.blueF(), double(size + 1), name, kTieGroup);
}

void mbna_view_clearnavpicks(void) {
	MbNavAdjust *m = g_na;
	if (!m)
		return;
	for (const std::string &n : m->viewNavPicked)
		restoreLine(m, n);
	m->viewNavPicked.clear();
}

void mbna_view_picknav(const char *name) {
	MbNavAdjust *m = g_na;
	if (!m || !name)
		return;
	m->viewNavPicked.push_back(name);
	highlightLine(m, name);
}

void mbna_view_pickroute(const char *name) {
	MbNavAdjust *m = g_na;
	if (!m || !name)
		return;
	if (!m->viewRoutePicked.empty())
		restoreLine(m, m->viewRoutePicked);
	m->viewRoutePicked.clear();
	if (m->lineStyle.count(name)) {
		m->viewRoutePicked = name;
		highlightLine(m, name);
	}
}

} // extern "C"

// ---- entry point -----------------------------------------------------------------------------------
bool mbnavadjustOpenWindow(QWidget *parent, const MbNavAdjustHost &host, const QString &projectPath) {
	if (!g_na) {
		if (!mbeditLoadMbio(parent, host.base))
			return false;
		char msg[2048] = "";
		if (!mbna_mbio_open(msg, int(sizeof(msg)))) {
			QMessageBox::warning(parent, "MBnavadjust", QString::fromUtf8(msg));
			return false;
		}
		buildColors();
		MbNavAdjust *m = build(parent, host);
		if (!m)
			return false;
		mbna_set_grid_reader(host.readGrid);

		if (host.base.windowOpened)
			host.base.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			for (XgCanvas &c : g_cv) {
				xgEnd(c);
				c.img = QImage();
				c.widget = nullptr;
			}
			mbna_set_grid_reader(nullptr);
			if (m->host.base.windowClosed)
				m->host.base.windowClosed();
			if (g_na == m)
				g_na = nullptr;
			delete m;
		});

		m->win->show();
		mbPlaceRight(m->win, parent);
		QApplication::processEvents();
		MainFilter::layout(m);

		// do_mbnavadjust_init: the colours, the borders, mbnavadjust_init_globals / mbnavadjust_init
		do_mbnavadjust_init(nullptr, false, false);
		xgFlush();
	}
	MbNavAdjust *m = g_na;
	mbParkRebind(m->parking, host.base.parkScene);
	mbParkShow(m->parking);

	// mbnavadjust -I project
	if (!projectPath.isEmpty()) {
		m->virtualText["fileSelectionBox_text"] = boxPath(MBNA_FILESELECTION_OPEN, projectPath);
		do_fileselection_mode((Widget) "fileSelectionBox", (XtPointer)(intptr_t)MBNA_FILESELECTION_OPEN, nullptr);
		do_fileselection_ok((Widget) "fileSelectionBox", nullptr, nullptr);
		xgFlush();
		return project.open;
	}
	return true;
}

int mbnavadjustState(int *out, int n) {
	MbNavAdjust *m = g_na;
	const bool open = m != nullptr;
	const int v[18] = {open ? 1 : 0,
	                   open && project.open ? 1 : 0,
	                   open ? project.num_files : 0,
	                   open ? project.num_surveys : 0,
	                   open ? project.num_crossings : 0,
	                   open ? project.num_crossings_analyzed : 0,
	                   open ? project.num_truecrossings : 0,
	                   open ? project.num_ties : 0,
	                   open ? project.num_globalties : 0,
	                   open ? project.inversion_status : 0,
	                   open ? project.grid_status : 0,
	                   open ? mbna_naverr_mode : 0,
	                   open ? mbna_view_list : 0,
	                   open ? mbna_crossing_select : -1,
	                   open ? mbna_tie_select : -1,
	                   open && project.modelplot ? 1 : 0,
	                   open && project.visualization_status ? 1 : 0,
	                   open && m->list ? m->list->count() : 0};
	const int k = n < 18 ? n : 18;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool mbnavadjustPress(const QString &name) {
	MbNavAdjust *m = g_na;
	if (!m)
		return false;
	QObject *o = wobj(name.toLatin1().constData());
	if (auto *a = qobject_cast<QAction *>(o)) {
		if (!a->isEnabled())
			return false;
		a->trigger();
		return true;
	}
	if (auto *b = qobject_cast<QAbstractButton *>(o)) {
		if (!b->isEnabled())
			return false;
		b->click();
		return true;
	}
	return false;
}

bool mbnavadjustFileSelect(int mode, const QString &path, int format) {
	MbNavAdjust *m = g_na;
	if (!m || mode < MBNA_FILESELECTION_NEW || mode > MBNA_FILESELECTION_REFERENCE)
		return false;
	do_fileselection_mode((Widget) "fileSelectionBox", (XtPointer)(intptr_t)mode, nullptr);
	m->virtualText["fileSelectionBox_text"] = boxPath(mode, path);
	if (mode == MBNA_FILESELECTION_IMPORTDATA || mode == MBNA_FILESELECTION_IMPORTSURVEY) {
		do_fileselection_list((Widget) "fileSelectionBox_list", nullptr, nullptr);
		if (format != 0)
			m->virtualText["textField_format"] = QString::number(format);
	}
	do_fileselection_ok((Widget) "fileSelectionBox", nullptr, nullptr);
	xgFlush();
	return true;
}

bool mbnavadjustListSelect(int row) {
	MbNavAdjust *m = g_na;
	if (!m || !m->list || row < 0 || row >= m->list->count())
		return false;
	QListWidgetItem *it = m->list->item(row);
	m->list->setCurrentItem(it);
	emit m->list->itemClicked(it);
	return true;
}

QString mbnavadjustListItem(int row) {
	MbNavAdjust *m = g_na;
	if (!m || !m->list || row < 0 || row >= m->list->count())
		return QString();
	return m->list->item(row)->text();
}

bool mbnavadjustMouse(int canvas, int button, int x0, int y0, int x1, int y1) {
	if (!g_na || canvas < 0 || canvas > 3 || !g_cv[canvas].widget)
		return false;
	QWidget *c = g_cv[canvas].widget;
	auto send = [c](QEvent::Type type, Qt::MouseButton b, Qt::MouseButtons held, int x, int y) {
		const QPointF pos(x, y);
		QMouseEvent ev(type, pos, c->mapToGlobal(pos), b, held, Qt::NoModifier);
		QApplication::sendEvent(c, &ev);
	};
	const Qt::MouseButton b = button == 1 ? Qt::LeftButton : button == 2 ? Qt::MiddleButton : Qt::RightButton;
	send(QEvent::MouseButtonPress, b, b, x0, y0);
	if (x1 != x0 || y1 != y0) {
		const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
		for (int k = 1; k <= steps; k++)
			send(QEvent::MouseMove, Qt::NoButton, b, x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps);
	}
	send(QEvent::MouseButtonRelease, b, Qt::NoButton, x1, y1);
	return true;
}

bool mbnavadjustSavePng(int canvas, const QString &path) {
	if (!g_na || canvas < 0 || canvas > 3)
		return false;
	xgFlush();
	return !g_cv[canvas].img.isNull() && g_cv[canvas].img.save(path, "PNG");
}

QString mbnavadjustMessages() {
	MbNavAdjust *m = g_na;
	return m && m->messages ? m->messages->toPlainText() : QString();
}

bool mbnavadjustClose() {
	MbNavAdjust *m = g_na;
	if (!m)
		return false;
	quitTool(m);
	return true;
}
