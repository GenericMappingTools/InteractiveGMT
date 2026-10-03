// ============================================================================
//  mbedit_window.cpp -- the swath bathymetry editor's Qt window: the port of MB-System's
//  src/mbedit/mbedit_callbacks.c (the Motif interface of mbedit) onto Qt, plus the xg_*
//  graphics the engine draws through (MB-System's mbaux/mb_xgraphics.c), painted into a QImage.
//
//  A translation unit of its own, apart from the viewer's fragments (see mbedit_window.h).
//  The engine (mbedit.c) holds the data and does every edit, filter and drawing decision; this
//  file only turns widgets, keys and mouse events into the same engine calls the Motif
//  callbacks made, with the same arguments, in the same order.
//
//  Drawing model, kept from X11: the engine draws straight onto the canvas and "erases" by
//  drawing again in white. So the canvas is a persistent QImage, painted with NO antialiasing
//  (a white line over an antialiased black one would leave a grey ghost), and shown as is.
//
//  Departures from the Motif program, all forced by the host:
//    - mbio is loaded at run time (mbedit_mbio_open); if it cannot be found the user is asked
//      for it once and the answer is kept in iGMT.ini.
//    - The window opens empty and offers File > Open straight away (there is no command line).
//    - Quit / the window's close box close the editor, saving the file being edited, instead of
//      ending the program.
//    - "Hold the mouse button to keep editing" is event driven (mouse moves while the button is
//      down) instead of polling XQueryPointer; the middle/right button scroll keeps its "repeat
//      after 2 seconds" through a timer.
// ============================================================================

#include "mbedit_window.h"
#include "mbedit.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QUiLoader>
#include <QVBoxLayout>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

// ---- mbedit_callbacks.c constants -------------------------------------------------------
enum { MODE_TOGGLE = 0, MODE_PICK = 1, MODE_ERASE = 2, MODE_RESTORE = 3, MODE_GRAB = 4, MODE_INFO = 5 };
enum { VIEW_WATERFALL = 0, VIEW_ALONGTRACK = 1, VIEW_ACROSSTRACK = 2 };
enum { BEAM_MODE_FLAG = 0, BEAM_MODE_DETECT = 1, BEAM_MODE_PULSE = 2 };
enum { OUTPUT_MODE_OUTPUT = 0, OUTPUT_MODE_EDIT = 1, OUTPUT_MODE_BROWSE = 2 };
enum { GRAB_START = 0, GRAB_MOVE = 1, GRAB_END = 2 };
const int NUM_FILES_MAX = 500;
// MB-System's status values (mb_status.h); mbedit_mbio.h is not included here, its mb_* macros
// belong to the engine only
const int MB_SUCCESS = 1;
const int MB_FAILURE = 0;
const char *const kMbioSettingKey = "mbedit/mbio_path";

// MB-System's drawing colours (mb_define.h mb_color_t order), as X11 names them
const QColor kColors[] = {
	QColor(255, 255, 255),   // MB_COLOR_WHITE
	QColor(0, 0, 0),         // MB_COLOR_BLACK
	QColor(255, 0, 0),       // MB_COLOR_RED
	QColor(255, 165, 0),     // MB_COLOR_ORANGE
	QColor(255, 255, 0),     // MB_COLOR_YELLOW
	QColor(0, 255, 0),       // MB_COLOR_GREEN
	QColor(0, 160, 160),     // MB_COLOR_BLUEGREEN
	QColor(0, 0, 255),       // MB_COLOR_BLUE
	QColor(160, 32, 240),    // MB_COLOR_PURPLE
	QColor(255, 127, 80),    // MB_COLOR_CORAL
	QColor(211, 211, 211),   // MB_COLOR_LIGHTGREY
};
const int kNColors = int(sizeof(kColors) / sizeof(kColors[0]));

// ---- the xg_* canvas ----------------------------------------------------------------------
class MbCanvas;

struct XgCanvas {
	QImage img;
	QPainter *p = nullptr;
	QFont font;
	QPointer<MbCanvas> widget;
};
XgCanvas g_xg;

QPainter *xgPainter() {
	if (g_xg.img.isNull())
		return nullptr;
	if (!g_xg.p) {
		g_xg.p = new QPainter(&g_xg.img);
		g_xg.p->setRenderHint(QPainter::Antialiasing, false);
		g_xg.p->setRenderHint(QPainter::TextAntialiasing, false);
		g_xg.p->setFont(g_xg.font);
	}
	return g_xg.p;
}

QPen xgPen(unsigned int pixel, int style) {
	QPen pen(pixel < unsigned(kNColors) ? kColors[pixel] : kColors[1]);
	pen.setWidth(0);                       // X11's line_width 0: one pixel
	if (style == XG_DASHLINE)
		pen.setDashPattern({4, 4});        // X11's default LineOnOffDash
	return pen;
}

// One user action is many engine draws; the painter stays open across them and the widget is
// repainted once at the end.
void xgFlush();

// ---- one Motif XmScale: slider + value label (+ max label) ----------------------------------
struct MeScale {
	QSlider *s = nullptr;
	QLabel *val = nullptr;
	QLabel *max = nullptr;
	int decimals = 0;                      // 2: the slider runs in hundredths (XmNdecimalPoints 2)

	QString fmt(int v) const {
		return decimals ? QString::number(v / 100.0, 'f', 2) : QString::number(v);
	}
	void setValue(int v) {
		if (!s)
			return;
		QSignalBlocker b(s);
		s->setValue(v);
		if (val)
			val->setText(fmt(s->value()));
	}
	void setRange(int lo, int hi) {
		if (!s)
			return;
		QSignalBlocker b(s);
		s->setRange(lo, hi);
		if (max)
			max->setText(fmt(hi));
		if (val)
			val->setText(fmt(s->value()));
	}
	void setMax(int hi) { setRange(s ? s->minimum() : 1, hi); }
	int maximum() const { return s ? s->maximum() : 0; }
	int value() const { return s ? s->value() : 0; }
};

struct MbFile {
	std::string path;
	int format = 0;
	int lock = -1;
	int esf = -1;
};

// ---- the editor: mbedit_callbacks.c's globals ----------------------------------------------
struct MbEdit {
	MbParking *parking = nullptr;        // X / minimise park it in Scene Objects (mbParkable)
	MbEditHost host;
	QMainWindow *win = nullptr;
	MbCanvas *canvas = nullptr;

	QPushButton *quitButton = nullptr, *nextButton = nullptr, *doneButton = nullptr, *forwardButton = nullptr,
	            *reverseButton = nullptr, *startButton = nullptr, *endButton = nullptr, *flagViewButton = nullptr,
	            *unflagViewButton = nullptr, *unflagForwardButton = nullptr, *aboutButton = nullptr;
	MeScale width, exag, pings, step;
	QRadioButton *modeButton[6] = {};
	QAction *viewAct[3] = {};
	QAction *beamAct[3] = {};
	QAction *plotAct[13] = {};
	QAction *showSoundingsAct = nullptr, *showProfilesAct = nullptr, *reverseKeysAct = nullptr, *reverseMouseAct = nullptr;

	// dialogs, built on first use and kept
	QDialog *openDlg = nullptr, *fileListDlg = nullptr, *gotoDlg = nullptr, *bufferDlg = nullptr,
	        *annotDlg = nullptr, *filtersDlg = nullptr;
	QLineEdit *openFile = nullptr;
	QSpinBox *openFormat = nullptr;
	QRadioButton *openEdit = nullptr, *openBrowse = nullptr;
	QCheckBox *openMbprocess = nullptr;
	QListWidget *fileList = nullptr;
	QRadioButton *listEdit = nullptr, *listBrowse = nullptr;
	QLineEdit *gotoEdit[6] = {};
	MeScale bufferSize, bufferHold, xInterval, yInterval;
	QCheckBox *fMedianCheck = nullptr, *fWrongsideCheck = nullptr, *fCutbeamCheck = nullptr, *fCutdistCheck = nullptr,
	          *fCutangleCheck = nullptr;
	MeScale fMedianThreshold, fMedianXtrack, fMedianLtrack, fWrongside, fCutbeamStart, fCutbeamEnd, fCutdistStart,
	        fCutdistEnd, fCutangleStart, fCutangleEnd;

	// mbedit_callbacks.c state
	bool expose_plot_ok = false;
	int step_n = 5;
	int mode_pick = MODE_TOGGLE;
	int mshow_beammode = BEAM_MODE_FLAG;
	int mshow_flaggedsoundings = true;
	int mshow_flaggedprofiles = false;
	int mview_mode = VIEW_WATERFALL;
	int mshow_time = 1;
	int mode_output = OUTPUT_MODE_EDIT;
	bool mode_reverse_keys = false;
	bool mode_reverse_mouse = false;
	int plot_size_max = 0, mplot_size = 10, buffer_size_max = 0, buffer_size = 0, hold_size = 0, mformat = 0;
	int mplot_width = 0, mexagger = 0, mx_interval = 0, my_interval = 0;
	int ttime_i[7] = {};
	int nbuffer = 0, ngood = 0, icurrent = 0, mnplot = 0, ndumped = 0, nloaded = 0;
	int f_beams_max = 0;
	double f_distance_max = 0.0;
	std::vector<MbFile> files;
	int currentfile = -1;
	int currentfile_shown = -1;
	int key_g_down = 0, key_z_down = 0, key_s_down = 0, key_a_down = 0, key_d_down = 0;
	int mb_borders[4] = {0, 1016, 0, 525};

	// mouse
	bool editButtonDown = false;
	int grab_mode = GRAB_START;
	QTimer *scrollTimer = nullptr;
	QElapsedTimer scrollClock;
	int scrollStep = 0;                    // +1 forward (right button), -1 reverse (middle button)

	QTimer *fileListTimer = nullptr;
};
MbEdit *g_me = nullptr;

#define ME_PLOT_ARGS(m) (m)->mplot_width, (m)->mexagger, (m)->mx_interval, (m)->my_interval, (m)->mplot_size, \
	(m)->mshow_beammode, (m)->mshow_flaggedsoundings, (m)->mshow_flaggedprofiles, (m)->mshow_time
#define ME_OUT_ARGS(m) &(m)->nbuffer, &(m)->ngood, &(m)->icurrent, &(m)->mnplot

// XBell when an engine action reports failure, then show what it drew.
void meDone(int status) {
	if (status == 0)
		QApplication::beep();
	xgFlush();
}

void mePlot(MbEdit *m) {
	meDone(mbedit_action_plot(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
}

void meSetupData(MbEdit *m);
void meLoadSpecificFile(MbEdit *m, int i_file, int useEsf = -1);
void meBuildFileList(MbEdit *m);
void meGetFilters(MbEdit *m);
void meMouseEdit(MbEdit *m, int x_loc, int y_loc);
void meStepScroll(MbEdit *m, int dir);

// ---- the canvas widget ---------------------------------------------------------------------
class MbCanvas : public QWidget {
public:
	explicit MbCanvas(QWidget *parent) : QWidget(parent) {
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(false);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setCursor(Qt::CrossCursor);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		if (g_xg.img.isNull())
			p.fillRect(rect(), Qt::white);
		else
			p.drawImage(0, 0, g_xg.img);
	}

	// do_expose: the canvas changed size, so the plot area follows it.
	void resizeEvent(QResizeEvent *) override {
		MbEdit *m = g_me;
		if (!m)
			return;
		if (g_xg.p) {
			g_xg.p->end();
			delete g_xg.p;
			g_xg.p = nullptr;
		}
		g_xg.img = QImage(qMax(1, width()), qMax(1, height()), QImage::Format_RGB32);
		g_xg.img.fill(Qt::white);
		m->mb_borders[0] = 0;
		m->mb_borders[1] = width() - 1;
		m->mb_borders[2] = 0;
		m->mb_borders[3] = height() - 1;
		mbedit_set_scaling(m->mb_borders, m->mshow_time);
		if (m->expose_plot_ok)
			mePlot(m);
		else
			update();
	}

	void enterEvent(QEnterEvent *) override { setFocus(Qt::MouseFocusReason); }

	void mousePressEvent(QMouseEvent *e) override {
		MbEdit *m = g_me;
		if (!m)
			return;
		setFocus(Qt::MouseFocusReason);
		const int x = int(e->position().x()), y = int(e->position().y());
		const Qt::MouseButton editBtn = m->mode_reverse_mouse ? Qt::RightButton : Qt::LeftButton;
		const Qt::MouseButton fwdBtn = m->mode_reverse_mouse ? Qt::LeftButton : Qt::RightButton;
		if (e->button() == editBtn) {
			// left mouse button: toggle, pick, erase, restore, grab, or info
			m->editButtonDown = true;
			m->grab_mode = GRAB_START;
			meMouseEdit(m, x, y);
		}
		else if (e->button() == Qt::MiddleButton) {
			meStepScroll(m, -1);               // middle mouse button: scroll in reverse
		}
		else if (e->button() == fwdBtn) {
			meStepScroll(m, +1);               // right mouse button: scroll forward
		}
	}

	// While the edit button is held, Erase / Restore / Grab keep acting where the pointer goes
	// (the Motif loop polled XQueryPointer for this).
	void mouseMoveEvent(QMouseEvent *e) override {
		MbEdit *m = g_me;
		if (!m || !m->editButtonDown)
			return;
		if (m->mode_pick == MODE_TOGGLE || m->mode_pick == MODE_PICK || m->mode_pick == MODE_INFO)
			return;
		meMouseEdit(m, int(e->position().x()), int(e->position().y()));
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		MbEdit *m = g_me;
		if (!m)
			return;
		const Qt::MouseButton editBtn = m->mode_reverse_mouse ? Qt::RightButton : Qt::LeftButton;
		if (e->button() == editBtn && m->editButtonDown) {
			m->editButtonDown = false;
			/* if grab on but mouse released, end grab */
			if (m->grab_mode == GRAB_MOVE) {
				meDone(mbedit_action_mouse_grab(GRAB_END, int(e->position().x()), int(e->position().y()), ME_PLOT_ARGS(m),
				                                ME_OUT_ARGS(m)));
				m->grab_mode = GRAB_START;
			}
		}
		else if (m->scrollTimer) {
			m->scrollTimer->stop();
			m->scrollStep = 0;
		}
	}

	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
};

void xgFlush() {
	if (g_xg.p) {
		g_xg.p->end();
		delete g_xg.p;
		g_xg.p = nullptr;
	}
	if (g_xg.widget)
		g_xg.widget->update();
}

// ---- small helpers -------------------------------------------------------------------------
QWidget *meLoadUi(MbEdit *m, const char *file, QWidget *parent) {
	QFile f(QDir(m->host.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBedit", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "MBedit", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

// A child the .ui must carry; a missing one is reported by name, so a renamed widget in the .ui
// is found at once.
template <typename T>
T *meChild(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

void meBindScale(QWidget *root, const char *name, int decimals, MeScale &sc, QStringList &missing) {
	sc.s = meChild<QSlider>(root, name, missing);
	sc.val = meChild<QLabel>(root, (std::string(name) + "Value").c_str(), missing);
	sc.max = root->findChild<QLabel *>(QString::fromLatin1(name) + "Max");      // optional
	sc.decimals = decimals;
}

// Motif's XmScale called back on release (and on keyboard steps); the value label follows the
// drag live.
void meOnScale(MeScale &sc, std::function<void(int)> fn) {
	QSlider *s = sc.s;
	MeScale *scp = &sc;
	QObject::connect(s, &QSlider::valueChanged, s, [s, scp, fn](int v) {
		if (scp->val)
			scp->val->setText(scp->fmt(v));
		if (!s->isSliderDown())
			fn(v);
	});
	QObject::connect(s, &QSlider::sliderReleased, s, [s, fn]() { fn(s->value()); });
}

// The Motif sliders double their range when pushed to the top and halve it at the bottom.
int meDoubleOrHalve(int value, int maxx, int limit) {
	if (value == 1 || value == maxx) {
		if (value == 1)
			maxx = maxx / 2;
		else
			maxx = 2 * maxx;
		if (limit > 0 && maxx > limit)
			maxx = limit;
		if (maxx < 2)
			maxx = 2;
	}
	return maxx;
}

void meSetModeButtons(MbEdit *m) {
	for (int i = 0; i < 6; i++) {
		QSignalBlocker b(m->modeButton[i]);
		m->modeButton[i]->setChecked(i == m->mode_pick);
	}
	// mbedit's cursors: a target to pick, an exchange to sweep
	const bool sweep = (m->mode_pick == MODE_ERASE || m->mode_pick == MODE_RESTORE);
	m->canvas->setCursor(sweep ? Qt::PointingHandCursor : Qt::CrossCursor);
}

void meSetMode(MbEdit *m, int mode) {
	m->mode_pick = mode;
	meSetModeButtons(m);
}

void meSetView(MbEdit *m, int view) {
	m->mview_mode = view;
	mbedit_set_viewmode(m->mview_mode);
	for (int i = 0; i < 3; i++) {
		QSignalBlocker b(m->viewAct[i]);
		m->viewAct[i]->setChecked(i == view);
	}
	mePlot(m);
}

// ---- the edit-button action (do_event's inner loop, one pass) -------------------------------
void meMouseEdit(MbEdit *m, int x_loc, int y_loc) {
	int status = MB_SUCCESS;
	if (m->mode_pick == MODE_TOGGLE)
		status = mbedit_action_mouse_toggle(x_loc, y_loc, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	else if (m->mode_pick == MODE_PICK)
		status = mbedit_action_mouse_pick(x_loc, y_loc, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	else if (m->mode_pick == MODE_ERASE)
		status = mbedit_action_mouse_erase(x_loc, y_loc, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	else if (m->mode_pick == MODE_RESTORE)
		status = mbedit_action_mouse_restore(x_loc, y_loc, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	else if (m->mode_pick == MODE_GRAB) {
		status = mbedit_action_mouse_grab(m->grab_mode, x_loc, y_loc, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
		if (status == MB_SUCCESS)
			m->grab_mode = GRAB_MOVE;
		else
			m->grab_mode = GRAB_START;
	}
	else if (m->mode_pick == MODE_INFO)
		status = mbedit_action_mouse_info(x_loc, y_loc, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	if (status == 0)
		QApplication::beep();
	else if (m->key_z_down) {
		status = mbedit_action_bad_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	}
	else if (m->key_s_down) {
		status = mbedit_action_good_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	}
	else if (m->key_a_down) {
		if (!m->mode_reverse_keys)
			status = mbedit_action_left_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
		else
			status = mbedit_action_right_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	}
	else if (m->key_d_down) {
		if (!m->mode_reverse_keys)
			status = mbedit_action_right_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
		else
			status = mbedit_action_left_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	}
	xgFlush();
}

// do_forward / do_reverse (and the 'g'-held jump to the end / start)
void meStepOnce(MbEdit *m, int dir) {
	int status;
	if (m->key_g_down == 0)
		status = mbedit_action_step(dir > 0 ? m->step_n : -m->step_n, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	else
		status = mbedit_action_step(dir > 0 ? m->nbuffer - m->icurrent - 1 : -m->icurrent, ME_PLOT_ARGS(m), ME_OUT_ARGS(m));
	meDone(status);
}

// Middle / right button: one step now, then steps for as long as the button is held past 2 s.
void meStepScroll(MbEdit *m, int dir) {
	meStepOnce(m, dir);
	m->scrollStep = dir;
	m->scrollClock.start();
	m->scrollTimer->start();
}

// ---- keys (do_event's KeyPress / KeyRelease switch) ----------------------------------------
void MbCanvas::keyPressEvent(QKeyEvent *e) {
	MbEdit *m = g_me;
	const QString t = e->text();
	if (!m || t.isEmpty()) {
		QWidget::keyPressEvent(e);
		return;
	}
	if (e->isAutoRepeat() && QString("GgZzMmSsKkAaJjDdLl").contains(t[0]))
		return;
	switch (t[0].toLatin1()) {
	case 'F':
	case 'f':
		meStepOnce(m, +1);
		break;
	case 'V':
	case 'v':
		meStepOnce(m, -1);
		break;
	case 'G':
	case 'g':
		m->key_g_down = 1;
		break;
	case 'M':
	case 'm':
	case 'Z':
	case 'z':
		meDone(mbedit_action_bad_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		m->key_z_down = 1;
		m->key_s_down = 0;
		m->key_a_down = 0;
		m->key_d_down = 0;
		break;
	case 'K':
	case 'k':
	case 'S':
	case 's':
		meDone(mbedit_action_good_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		m->key_z_down = 0;
		m->key_s_down = 1;
		m->key_a_down = 0;
		m->key_d_down = 0;
		break;
	case 'J':
	case 'j':
	case 'A':
	case 'a':
		if (!m->mode_reverse_keys)
			meDone(mbedit_action_left_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		else
			meDone(mbedit_action_right_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		m->key_z_down = 0;
		m->key_s_down = 0;
		m->key_a_down = 1;
		m->key_d_down = 0;
		break;
	case 'L':
	case 'l':
	case 'D':
	case 'd':
		if (!m->mode_reverse_keys)
			meDone(mbedit_action_right_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		else
			meDone(mbedit_action_left_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		m->key_z_down = 0;
		m->key_s_down = 0;
		m->key_a_down = 0;
		m->key_d_down = 1;
		break;
	case '<':
	case ',':
	case 'X':
	case 'x':
		meDone(mbedit_action_flag_view(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		break;
	case '>':
	case '.':
	case 'C':
	case 'c':
		meDone(mbedit_action_unflag_view(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		break;
	case '!':
		meDone(mbedit_action_zero_ping(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		break;
	case 'U':
	case 'u':
	case 'Q':
	case 'q':
		meSetMode(m, MODE_TOGGLE);
		break;
	case 'I':
	case 'i':
	case 'W':
	case 'w':
		meSetMode(m, MODE_PICK);
		break;
	case 'O':
	case 'o':
	case 'E':
	case 'e':
		meSetMode(m, MODE_ERASE);
		break;
	case 'P':
	case 'p':
	case 'R':
	case 'r':
		meSetMode(m, MODE_RESTORE);
		break;
	case '{':
	case '[':
	case 'T':
	case 't':
		meSetMode(m, MODE_GRAB);
		break;
	case '}':
	case ']':
	case 'Y':
	case 'y':
		meSetMode(m, MODE_INFO);
		break;
	case '2':
	case '@':
		meSetView(m, VIEW_WATERFALL);
		break;
	case '3':
	case '#':
		meSetView(m, VIEW_ALONGTRACK);
		break;
	case '4':
	case '$':
		meSetView(m, VIEW_ACROSSTRACK);
		break;
	default:
		QWidget::keyPressEvent(e);
		break;
	}
}

void MbCanvas::keyReleaseEvent(QKeyEvent *e) {
	MbEdit *m = g_me;
	const QString t = e->text();
	if (!m || t.isEmpty() || e->isAutoRepeat()) {
		QWidget::keyReleaseEvent(e);
		return;
	}
	switch (t[0].toLatin1()) {
	case 'G':
	case 'g':
		m->key_g_down = 0;
		break;
	case 'M':
	case 'm':
	case 'Z':
	case 'z':
		m->key_z_down = 0;
		break;
	case 'K':
	case 'k':
	case 'S':
	case 's':
		m->key_s_down = 0;
		break;
	case 'J':
	case 'j':
	case 'A':
	case 'a':
		m->key_a_down = 0;
		break;
	case 'L':
	case 'l':
	case 'D':
	case 'd':
		m->key_d_down = 0;
		break;
	default:
		QWidget::keyReleaseEvent(e);
		break;
	}
}

// ---- do_setup_data -------------------------------------------------------------------------
void meSetupData(MbEdit *m) {
	/* get some default values from mbedit */
	mbedit_get_defaults(&m->plot_size_max, &m->mplot_size, &m->mshow_beammode, &m->mshow_flaggedsoundings,
	                    &m->mshow_flaggedprofiles, &m->mshow_time, &m->buffer_size_max, &m->buffer_size, &m->hold_size,
	                    &m->mformat, &m->mplot_width, &m->mexagger, &m->mx_interval, &m->my_interval, m->ttime_i,
	                    &m->mode_output);

	/* set values of the sliders */
	m->pings.setValue(m->mplot_size);
	m->step.setValue(m->step_n);
	m->bufferSize.setRange(1, m->buffer_size_max);
	m->bufferSize.setValue(m->buffer_size);
	m->bufferHold.setRange(1, m->buffer_size_max);
	m->bufferHold.setValue(m->hold_size);
	m->width.setValue(m->mplot_width);
	m->exag.setValue(m->mexagger);
	m->xInterval.setValue(m->mx_interval);
	m->yInterval.setValue(m->my_interval);

	/* set starting values in go to time widgets */
	if (m->gotoDlg) {
		const char *fmt[6] = {"%4.4d", "%2.2d", "%2.2d", "%2.2d", "%2.2d", "%2.2d"};
		for (int i = 0; i < 6; i++) {
			char text[32];
			snprintf(text, sizeof(text), fmt[i], m->ttime_i[i]);
			m->gotoEdit[i]->setText(QString::fromLatin1(text));
		}
	}

	/* set value of format text item */
	if (m->openFormat)
		m->openFormat->setValue(m->mformat);

	/* set the output mode */
	const bool edit = (m->mode_output == OUTPUT_MODE_EDIT);
	for (QRadioButton *r : {m->openEdit, m->listEdit})
		if (r) {
			QSignalBlocker b(r);
			r->setChecked(edit);
		}
	for (QRadioButton *r : {m->openBrowse, m->listBrowse})
		if (r) {
			QSignalBlocker b(r);
			r->setChecked(!edit);
		}

	/* set the mode toggles */
	meSetModeButtons(m);

	/* set the show flagged toggles */
	{
		QSignalBlocker b1(m->showSoundingsAct), b2(m->showProfilesAct);
		m->showSoundingsAct->setChecked(m->mshow_flaggedsoundings);
		m->showProfilesAct->setChecked(m->mshow_flaggedprofiles);
	}

	/* view mode, beam colouring and plot mode toggles */
	for (int i = 0; i < 3; i++) {
		QSignalBlocker b1(m->viewAct[i]), b2(m->beamAct[i]);
		m->viewAct[i]->setChecked(i == m->mview_mode);
		m->beamAct[i]->setChecked(i == m->mshow_beammode);
	}
	mbedit_set_viewmode(m->mview_mode);
	for (int i = 0; i < 13; i++) {
		QSignalBlocker b(m->plotAct[i]);
		m->plotAct[i]->setChecked(i == m->mshow_time);
	}

	/* get filter values and set widgets */
	meGetFilters(m);

	/* build available file list */
	meBuildFileList(m);
}

// ---- file list (do_build_filelist / do_parse_datalist / do_load_specific_file / do_load) ----
void meBuildFileList(MbEdit *m) {
	if (!m->fileList)
		return;
	const int numfiles = int(m->files.size());
	bool update_filelist = (m->fileList->count() != numfiles);

	/* check current file shown vs loaded */
	if (m->currentfile != m->currentfile_shown) {
		m->currentfile_shown = m->currentfile;
		update_filelist = true;
	}

	/* check for change in lock status or esf status */
	for (MbFile &f : m->files) {
		const int locked = mbedit_file_locked(const_cast<char *>(f.path.c_str()));
		if (locked != f.lock) {
			f.lock = locked;
			update_filelist = true;
		}
		const int esf_exists = QFileInfo(QString::fromStdString(f.path + ".esf")).isFile() ? 1 : 0;
		if (esf_exists != f.esf) {
			f.esf = esf_exists;
			update_filelist = true;
		}
	}

	/* only rebuild the filelist if necessary */
	if (update_filelist) {
		const int selection = m->fileList->currentRow();
		const int item_count = m->fileList->count();
		m->fileList->clear();
		for (int i = 0; i < numfiles; i++) {
			const MbFile &f = m->files[i];
			const char *lockstr = (m->currentfile == i) ? "<loaded>" : (f.lock ? "<Locked>" : "        ");
			const char *esfstr = f.esf ? "<esf>" : "     ";
			char value_text[2 * 1024];
			snprintf(value_text, sizeof(value_text), "%s %s %s %3d", lockstr, esfstr, f.path.c_str(), f.format);
			m->fileList->addItem(QString::fromUtf8(value_text));
		}
		/* reinstate selection if the number of items is the same as before */
		if (item_count == numfiles && selection >= 0)
			m->fileList->setCurrentRow(selection);
	}
}

void meAddFile(void *ctx, const char *path, int format) {
	MbEdit *m = static_cast<MbEdit *>(ctx);
	if (int(m->files.size()) >= NUM_FILES_MAX)
		return;
	MbFile f;
	f.path = path;
	f.format = format;
	m->files.push_back(f);
}

void meLoad(MbEdit *m, bool save_mode) {
	/* turn off expose plots */
	m->expose_plot_ok = false;

	int status = MB_SUCCESS;
	/* only load valid file */
	if (m->currentfile >= 0) {
		/* process input file name */
		MbFile &f = m->files[m->currentfile];
		status = mbedit_action_open(const_cast<char *>(f.path.c_str()), f.format, m->currentfile, int(m->files.size()),
		                            save_mode, m->mode_output, ME_PLOT_ARGS(m), &m->buffer_size, &m->buffer_size_max,
		                            &m->hold_size, &m->ndumped, &m->nloaded, ME_OUT_ARGS(m));
		if (status == MB_FAILURE) {
			QApplication::beep();
			m->currentfile = -1;
		}
	}

	/* display data from chosen file */
	if (status == MB_SUCCESS)
		mePlot(m);
	else
		xgFlush();

	/* set widget values */
	meSetupData(m);

	/* turn on expose plots */
	m->expose_plot_ok = true;
}

void meLoadSpecificFile(MbEdit *m, int i_file, int useEsf) {
	/* check the specified file is in the list */
	if (i_file < 0 || i_file >= int(m->files.size()))
		return;
	m->currentfile = i_file;

	/* check for edit save file */
	const QString base = QString::fromStdString(m->files[i_file].path);
	const bool esf = QFileInfo(base + ".esf").isFile() || QFileInfo(base + ".mbesf").isFile();

	/* if esf file exists ask if it should be used (bulletinBoard_editsave), unless the caller
	   already said (mbedit -S at startup) */
	if (esf && useEsf >= 0) {
		meLoad(m, useEsf == 1);
	}
	else if (esf) {
		const auto ans = QMessageBox::question(m->win, "Use MBedit edit save file?",
			"An edit save file exists for the specified input data file...\n\n"
			"Do you want to apply the saved edits to the data?",
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		meLoad(m, ans == QMessageBox::Yes);
	}
	else {
		/* else just try to load the data without an esf */
		meLoad(m, false);
	}
}

// do_done / do_editlistselection / the open dialog: close what is loaded (saving its edits).
void meCloseCurrent(MbEdit *m) {
	int quit = 0;
	meDone(mbedit_action_done(m->buffer_size, &m->ndumped, &m->nloaded, &m->nbuffer, &m->ngood, &m->icurrent, &quit));
}

// do_load_check: close what is loaded, add the file (or every file of a datalist) to the list,
// load the first one added.
bool meOpenInput(MbEdit *m, const QByteArray &input_file, int format, int useEsf) {
	/* turn off expose plots */
	m->expose_plot_ok = false;

	/* close out previously open file */
	meCloseCurrent(m);
	m->currentfile = -1;

	/* try to parse the selection */
	const int numfilessave = int(m->files.size());
	mbedit_parse_datalist(const_cast<char *>(input_file.constData()), format, meAddFile, m);

	/* load first new file in the list */
	const bool added = int(m->files.size()) > numfilessave;
	if (added)
		meLoadSpecificFile(m, numfilessave, useEsf);
	else
		QMessageBox::warning(m->win, "MBedit",
			"No swath file was found in\n" + QString::fromUtf8(input_file) +
			"\n\nCheck the MBIO format id (-1 for a datalist).");

	/* turn on expose plots */
	m->expose_plot_ok = true;
	meSetupData(m);
	return added && m->currentfile >= 0;
}

// ---- dialogs -------------------------------------------------------------------------------
void meShow(QDialog *d) {
	d->show();
	d->raise();
	d->activateWindow();
}

void meOpenDialog(MbEdit *m) {
	if (!m->openDlg) {
		QWidget *w = meLoadUi(m, "mbedit_open.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		m->openFile = meChild<QLineEdit>(d, "fileEdit", missing);
		m->openFormat = meChild<QSpinBox>(d, "formatBox", missing);
		m->openEdit = meChild<QRadioButton>(d, "outputEdit", missing);
		m->openBrowse = meChild<QRadioButton>(d, "outputBrowse", missing);
		m->openMbprocess = meChild<QCheckBox>(d, "mbprocessCheck", missing);
		auto *browse = meChild<QPushButton>(d, "browseButton", missing);
		auto *ok = meChild<QPushButton>(d, "okButton", missing);
		auto *cancel = meChild<QPushButton>(d, "cancelButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBedit", "mbedit_open.ui lacks: " + missing.join(", "));
			delete d;
			m->openFile = nullptr;
			m->openFormat = nullptr;
			m->openEdit = m->openBrowse = nullptr;
			m->openMbprocess = nullptr;
			return;
		}
		m->openDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		// do_fileselection_list: a chosen file gets its format guessed from its name
		auto guess = [m]() {
			const QByteArray path = QDir::fromNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (path.isEmpty())
				return;
			int form = m->openFormat->value();
			if (mbedit_get_format(const_cast<char *>(path.constData()), &form) == MB_SUCCESS)
				m->openFormat->setValue(form);
		};
		QObject::connect(browse, &QPushButton::clicked, d, [m, d, guess]() {
			const QString fn = QFileDialog::getOpenFileName(d, "Swath file or datalist", m->host.startDir(),
			                                                "All Files (*)");
			if (fn.isEmpty())
				return;
			m->host.rememberDir(fn);
			m->openFile->setText(QDir::toNativeSeparators(fn));
			guess();
		});
		QObject::connect(m->openFile, &QLineEdit::editingFinished, d, guess);
		QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::hide);
		// do_load_check
		QObject::connect(ok, &QPushButton::clicked, d, [m, d]() {
			const QByteArray input_file = QDir::fromNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (input_file.isEmpty()) {
				QApplication::beep();
				return;
			}
			d->hide();
			m->mode_output = m->openBrowse->isChecked() ? OUTPUT_MODE_BROWSE : OUTPUT_MODE_EDIT;
			mbedit_set_run_mbprocess(m->openMbprocess->isChecked() ? 1 : 0);
			meOpenInput(m, input_file, m->openFormat->value(), -1);
		});
	}
	m->openFormat->setValue(m->mformat);
	meShow(m->openDlg);
}

void meFileListDialog(MbEdit *m) {
	if (!m->fileListDlg) {
		QWidget *w = meLoadUi(m, "mbedit_filelist.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		auto *list = meChild<QListWidget>(d, "fileList", missing);
		auto *edit = meChild<QRadioButton>(d, "outputEdit", missing);
		auto *browse = meChild<QRadioButton>(d, "outputBrowse", missing);
		auto *editBtn = meChild<QPushButton>(d, "editButton", missing);
		auto *removeBtn = meChild<QPushButton>(d, "removeButton", missing);
		auto *dismiss = meChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBedit", "mbedit_filelist.ui lacks: " + missing.join(", "));
			delete d;
			return;
		}
		m->fileListDlg = d;
		m->fileList = list;
		m->listEdit = edit;
		m->listBrowse = browse;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		QFont mono("Courier New");
		mono.setStyleHint(QFont::TypeWriter);
		list->setFont(mono);
		// do_output_edit_filelist / do_output_browse_filelist
		QObject::connect(edit, &QRadioButton::toggled, d, [m](bool on) {
			if (on)
				m->mode_output = OUTPUT_MODE_EDIT;
		});
		QObject::connect(browse, &QRadioButton::toggled, d, [m](bool on) {
			if (on)
				m->mode_output = OUTPUT_MODE_BROWSE;
		});
		// do_editlistselection
		auto editSelected = [m]() {
			const int sel = m->fileList->currentRow();
			/* if the selected file is different than what's already loaded, unload the old file and load the new one */
			if (sel >= 0 && sel != m->currentfile) {
				m->expose_plot_ok = false;
				meCloseCurrent(m);
				if (sel < int(m->files.size()))
					meLoadSpecificFile(m, sel);
				m->expose_plot_ok = true;
			}
			meSetupData(m);
		};
		QObject::connect(editBtn, &QPushButton::clicked, d, editSelected);
		QObject::connect(list, &QListWidget::itemDoubleClicked, d, editSelected);
		// do_filelist_remove
		QObject::connect(removeBtn, &QPushButton::clicked, d, [m]() {
			const int sel = m->fileList->currentRow();
			/* if the selected file is different than what's already loaded, remove it from the list */
			if (sel >= 0 && sel != m->currentfile && sel < int(m->files.size())) {
				m->files.erase(m->files.begin() + sel);
				if (m->currentfile > sel)
					m->currentfile--;
			}
			meSetupData(m);
			mePlot(m);
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	meSetupData(m);
	meShow(m->fileListDlg);
}

void meGotoDialog(MbEdit *m) {
	if (!m->gotoDlg) {
		QWidget *w = meLoadUi(m, "mbedit_goto.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		const char *names[6] = {"yearEdit", "monthEdit", "dayEdit", "hourEdit", "minuteEdit", "secondEdit"};
		for (int i = 0; i < 6; i++)
			m->gotoEdit[i] = meChild<QLineEdit>(d, names[i], missing);
		auto *apply = meChild<QPushButton>(d, "applyButton", missing);
		auto *cancel = meChild<QPushButton>(d, "cancelButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBedit", "mbedit_goto.ui lacks: " + missing.join(", "));
			delete d;
			for (auto &e : m->gotoEdit)
				e = nullptr;
			return;
		}
		m->gotoDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		// do_goto_apply
		QObject::connect(apply, &QPushButton::clicked, d, [m]() {
			for (int i = 0; i < 6; i++)
				m->ttime_i[i] = m->gotoEdit[i]->text().trimmed().toInt();
			m->ttime_i[6] = 0;
			m->expose_plot_ok = false;
			meDone(mbedit_action_goto(m->ttime_i, m->hold_size, m->buffer_size, ME_PLOT_ARGS(m), &m->ndumped,
			                          &m->nloaded, ME_OUT_ARGS(m)));
			m->expose_plot_ok = true;
		});
		QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::hide);
	}
	meSetupData(m);
	meShow(m->gotoDlg);
}

void meBufferDialog(MbEdit *m) {
	if (!m->bufferDlg) {
		QWidget *w = meLoadUi(m, "mbedit_buffer.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		meBindScale(d, "bufferSlider", 0, m->bufferSize, missing);
		meBindScale(d, "holdSlider", 0, m->bufferHold, missing);
		auto *dismiss = meChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBedit", "mbedit_buffer.ui lacks: " + missing.join(", "));
			delete d;
			m->bufferSize = MeScale();
			m->bufferHold = MeScale();
			return;
		}
		m->bufferDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		// do_buffer_size / do_buffer_hold
		meOnScale(m->bufferSize, [m](int v) { m->buffer_size = v; });
		meOnScale(m->bufferHold, [m](int v) { m->hold_size = v; });
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	meSetupData(m);
	meShow(m->bufferDlg);
}

// do_x_interval / do_y_interval
void meIntervalChanged(MbEdit *m, MeScale &sc, int &target, int v) {
	target = v;
	const int maxx = meDoubleOrHalve(v, sc.maximum(), 0);
	if (maxx != sc.maximum())
		sc.setMax(maxx);
	mePlot(m);
}

void meAnnotationDialog(MbEdit *m) {
	if (!m->annotDlg) {
		QWidget *w = meLoadUi(m, "mbedit_annotation.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		meBindScale(d, "xIntervalSlider", 0, m->xInterval, missing);
		meBindScale(d, "yIntervalSlider", 0, m->yInterval, missing);
		auto *dismiss = meChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBedit", "mbedit_annotation.ui lacks: " + missing.join(", "));
			delete d;
			m->xInterval = MeScale();
			m->yInterval = MeScale();
			return;
		}
		m->annotDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		meOnScale(m->xInterval, [m](int v) { meIntervalChanged(m, m->xInterval, m->mx_interval, v); });
		meOnScale(m->yInterval, [m](int v) { meIntervalChanged(m, m->yInterval, m->my_interval, v); });
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	meSetupData(m);
	meShow(m->annotDlg);
}

// do_get_filters
void meGetFilters(MbEdit *m) {
	int f_medianspike, f_medianspike_threshold, f_medianspike_xtrack, f_medianspike_ltrack, f_wrongside,
	    f_wrongside_threshold, f_cutbeam, f_cutbeam_begin, f_cutbeam_end, f_cutdistance, f_cutangle;
	double f_cutdistance_begin, f_cutdistance_end, f_cutangle_begin, f_cutangle_end;
	mbedit_get_filters(&m->f_beams_max, &m->f_distance_max, &f_medianspike, &f_medianspike_threshold,
	                   &f_medianspike_xtrack, &f_medianspike_ltrack, &f_wrongside, &f_wrongside_threshold, &f_cutbeam,
	                   &f_cutbeam_begin, &f_cutbeam_end, &f_cutdistance, &f_cutdistance_begin, &f_cutdistance_end,
	                   &f_cutangle, &f_cutangle_begin, &f_cutangle_end);
	if (!m->filtersDlg)
		return;

	/* set values of median spike filter widgets */
	m->fMedianCheck->setChecked(f_medianspike);
	m->fMedianThreshold.setRange(1, 100);
	m->fMedianThreshold.setValue(f_medianspike_threshold);
	m->fMedianXtrack.setRange(1, m->f_beams_max);
	m->fMedianXtrack.setValue(f_medianspike_xtrack);
	m->fMedianLtrack.setRange(1, m->f_beams_max);
	m->fMedianLtrack.setValue(f_medianspike_ltrack);

	/* set values of wrong side filter widgets */
	m->fWrongsideCheck->setChecked(f_wrongside);
	m->fWrongside.setRange(0, m->f_beams_max);
	m->fWrongside.setValue(f_wrongside_threshold);

	/* set values of cut by beam number filter widgets */
	m->fCutbeamCheck->setChecked(f_cutbeam);
	m->fCutbeamStart.setRange(0, m->f_beams_max);
	m->fCutbeamStart.setValue(f_cutbeam_begin);
	m->fCutbeamEnd.setRange(0, m->f_beams_max);
	m->fCutbeamEnd.setValue(f_cutbeam_end);

	/* set values of cut by distance filter widgets */
	m->fCutdistCheck->setChecked(f_cutdistance);
	m->fCutdistStart.setRange(int(-100 * m->f_distance_max - 0.5), int(100 * m->f_distance_max + 0.5));
	m->fCutdistStart.setValue(int(100 * f_cutdistance_begin + 0.5));
	m->fCutdistEnd.setRange(int(-100 * m->f_distance_max - 0.5), int(100 * m->f_distance_max + 0.5));
	m->fCutdistEnd.setValue(int(100 * f_cutdistance_end + 0.5));

	/* set values of cut by angle filter widgets */
	m->fCutangleCheck->setChecked(f_cutangle);
	m->fCutangleStart.setRange(-9000, 9000);
	m->fCutangleStart.setValue(int(100 * f_cutangle_begin + 0.5));
	m->fCutangleEnd.setRange(-9000, 9000);
	m->fCutangleEnd.setValue(int(100 * f_cutangle_end + 0.5));
}

void meFiltersDialog(MbEdit *m) {
	if (!m->filtersDlg) {
		QWidget *w = meLoadUi(m, "mbedit_filters.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		m->fMedianCheck = meChild<QCheckBox>(d, "medianCheck", missing);
		m->fWrongsideCheck = meChild<QCheckBox>(d, "wrongsideCheck", missing);
		m->fCutbeamCheck = meChild<QCheckBox>(d, "cutbeamCheck", missing);
		m->fCutdistCheck = meChild<QCheckBox>(d, "cutdistCheck", missing);
		m->fCutangleCheck = meChild<QCheckBox>(d, "cutangleCheck", missing);
		meBindScale(d, "medianThresholdSlider", 0, m->fMedianThreshold, missing);
		meBindScale(d, "medianXtrackSlider", 0, m->fMedianXtrack, missing);
		meBindScale(d, "medianLtrackSlider", 0, m->fMedianLtrack, missing);
		meBindScale(d, "wrongsideSlider", 0, m->fWrongside, missing);
		meBindScale(d, "cutbeamStartSlider", 0, m->fCutbeamStart, missing);
		meBindScale(d, "cutbeamEndSlider", 0, m->fCutbeamEnd, missing);
		meBindScale(d, "cutdistStartSlider", 2, m->fCutdistStart, missing);
		meBindScale(d, "cutdistEndSlider", 2, m->fCutdistEnd, missing);
		meBindScale(d, "cutangleStartSlider", 2, m->fCutangleStart, missing);
		meBindScale(d, "cutangleEndSlider", 2, m->fCutangleEnd, missing);
		auto *apply = meChild<QPushButton>(d, "applyButton", missing);
		auto *reset = meChild<QPushButton>(d, "resetButton", missing);
		auto *dismiss = meChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBedit", "mbedit_filters.ui lacks: " + missing.join(", "));
			delete d;
			m->fMedianCheck = m->fWrongsideCheck = m->fCutbeamCheck = m->fCutdistCheck = m->fCutangleCheck = nullptr;
			return;
		}
		m->filtersDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		// value labels follow the sliders; nothing is applied until Apply
		for (MeScale *sc : {&m->fMedianThreshold, &m->fWrongside, &m->fCutbeamStart, &m->fCutbeamEnd, &m->fCutdistStart,
		                    &m->fCutdistEnd, &m->fCutangleStart, &m->fCutangleEnd})
			meOnScale(*sc, [](int) {});
		// do_check_median_xtrack / do_check_median_ltrack: the median windows are odd
		meOnScale(m->fMedianXtrack, [m](int v) {
			if (v % 2 == 0)
				m->fMedianXtrack.setValue(v + 1);
		});
		meOnScale(m->fMedianLtrack, [m](int v) {
			if (v % 2 == 0)
				m->fMedianLtrack.setValue(v + 1);
		});
		// do_set_filters
		QObject::connect(apply, &QPushButton::clicked, d, [m]() {
			mbedit_set_filters(m->fMedianCheck->isChecked(), m->fMedianThreshold.value(), m->fMedianXtrack.value(),
			                   m->fMedianLtrack.value(), m->fWrongsideCheck->isChecked(), m->fWrongside.value(),
			                   m->fCutbeamCheck->isChecked(), m->fCutbeamStart.value(), m->fCutbeamEnd.value(),
			                   m->fCutdistCheck->isChecked(), 0.01 * m->fCutdistStart.value(), 0.01 * m->fCutdistEnd.value(),
			                   m->fCutangleCheck->isChecked(), 0.01 * m->fCutangleStart.value(),
			                   0.01 * m->fCutangleEnd.value());
			meDone(mbedit_action_filter_all(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		});
		// do_reset_filters
		QObject::connect(reset, &QPushButton::clicked, d, [m]() { meGetFilters(m); });
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	meGetFilters(m);
	meShow(m->filtersDlg);
}

void meAbout(MbEdit *m) {
	QMessageBox::about(m->win, "About MBedit",
		QString("<b>MBedit</b><br>Interactive Swath Bathymetry Editor<br><br>"
		        "One Component of the <b>MB-System</b> Open Source Software Package<br>"
		        "for Processing and Display of Swath Sonar Data<br><br>"
		        "Created by: David W. Caress and Dale N. Chayes<br><br>"
		        "MB-System library in use: %1<br><br>"
		        "Ported to InteractiveGMT (Qt) from MB-System's mbedit.")
		    .arg(QString::fromLatin1(mbedit_mbio_version())));
}

// ---- finding the MBIO library --------------------------------------------------------------
QString meDefaultMbioName() {
#if defined(_WIN32)
	return "mbio.dll";             // an MB-System build; GMT's supplement ships it as mbio_w64.dll
#elif defined(__APPLE__)
	return "libmbio.dylib";
#else
	return "libmbio.so";
#endif
}

QString g_meMbioHint;   // the MBIO GMT itself loaded with its MB-System supplement (mbeditSetMbioHint)

// INTERACTIVEGMT_MBIO, then the path remembered in iGMT.ini, then the MBIO GMT loaded with the
// MB-System supplement it is configured with (GMT_CUSTOM_LIBS). No library is looked up by a
// name of our own: GMT is what finds MB-System. When none works, ask where it is (and remember).
bool meLoadMbio(QWidget *parent, const MbEditHost &host) {
	if (mbedit_mbio_loaded())
		return true;
	char msg[2048] = "";
	QStringList tried;
	QStringList candidates;
	// The PROCESS environment, not the C runtime's copy: a variable set from Julia (ENV[...] = ...)
	// after start-up reaches only the former on Windows.
#ifdef _WIN32
	wchar_t wenv[2048];
	const DWORD nenv = GetEnvironmentVariableW(L"INTERACTIVEGMT_MBIO", wenv, 2048);
	if (nenv > 0 && nenv < 2048)
		candidates << QString::fromWCharArray(wenv, int(nenv));
#else
	const char *env = std::getenv("INTERACTIVEGMT_MBIO");
	if (env && env[0])
		candidates << QString::fromLocal8Bit(env);
#endif
	const QString saved = host.setting ? host.setting(kMbioSettingKey) : QString();
	if (!saved.isEmpty())
		candidates << saved;
	if (!g_meMbioHint.isEmpty())
		candidates << g_meMbioHint;
	for (const QString &c : candidates) {
		if (mbedit_mbio_open(c.toUtf8().constData(), msg, int(sizeof(msg))))
			return true;
		tried << QString::fromUtf8(msg);
	}
	for (;;) {
		const auto ans = QMessageBox::question(parent, "MBedit",
			"The swath editor reads swath files with MB-System's MBIO library (5.7.x or 5.8.x), "
			"which was not found:\n\n" + tried.join("\n") +
			"\n\nLocate the library (" + meDefaultMbioName() + ") now?",
			QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
		if (ans != QMessageBox::Yes)
			return false;
		const QString fn = QFileDialog::getOpenFileName(parent, "MB-System MBIO library", QString(),
#if defined(_WIN32)
		                                                "MBIO library (mbio*.dll);;All Files (*)");
#else
		                                                "MBIO library (libmbio*);;All Files (*)");
#endif
		if (fn.isEmpty())
			return false;
		const QString path = QDir::toNativeSeparators(fn);
		if (mbedit_mbio_open(path.toUtf8().constData(), msg, int(sizeof(msg)))) {
			if (host.setSetting)
				host.setSetting(kMbioSettingKey, path);
			return true;
		}
		tried = QStringList{QString::fromUtf8(msg)};
	}
}

// ---- the window ----------------------------------------------------------------------------
class MbCloseFilter : public QObject {
public:
	explicit MbCloseFilter(QObject *parent) : QObject(parent) {}

protected:
	// do_quit: the edits of the file being edited are saved before the window goes
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() == QEvent::Close && g_me && o == g_me->win) {
			MbEdit *m = g_me;
			m->expose_plot_ok = false;
			if (m->scrollTimer)
				m->scrollTimer->stop();
			meDone(mbedit_action_quit(m->buffer_size, &m->ndumped, &m->nloaded, &m->nbuffer, &m->ngood, &m->icurrent));
		}
		return QObject::eventFilter(o, e);
	}
};

MbEdit *meBuild(QWidget *parent, const MbEditHost &host) {
	auto *m = new MbEdit;
	m->host = host;
	QWidget *root = meLoadUi(m, "mbedit.ui", nullptr);
	auto *win = qobject_cast<QMainWindow *>(root);
	if (!win) {
		delete root;
		delete m;
		return nullptr;
	}
	QStringList missing;
	m->quitButton = meChild<QPushButton>(win, "quitButton", missing);
	m->nextButton = meChild<QPushButton>(win, "nextBufferButton", missing);
	m->doneButton = meChild<QPushButton>(win, "doneButton", missing);
	m->forwardButton = meChild<QPushButton>(win, "forwardButton", missing);
	m->reverseButton = meChild<QPushButton>(win, "reverseButton", missing);
	m->startButton = meChild<QPushButton>(win, "startButton", missing);
	m->endButton = meChild<QPushButton>(win, "endButton", missing);
	m->flagViewButton = meChild<QPushButton>(win, "flagViewButton", missing);
	m->unflagViewButton = meChild<QPushButton>(win, "unflagViewButton", missing);
	m->unflagForwardButton = meChild<QPushButton>(win, "unflagForwardButton", missing);
	m->aboutButton = meChild<QPushButton>(win, "aboutButton", missing);
	meBindScale(win, "widthSlider", 0, m->width, missing);
	meBindScale(win, "exagSlider", 2, m->exag, missing);
	meBindScale(win, "pingsSlider", 0, m->pings, missing);
	meBindScale(win, "stepSlider", 0, m->step, missing);
	const char *modes[6] = {"modeToggle", "modePick", "modeErase", "modeRestore", "modeGrab", "modeInfo"};
	for (int i = 0; i < 6; i++)
		m->modeButton[i] = meChild<QRadioButton>(win, modes[i], missing);
	const char *views[3] = {"actionWaterfall", "actionAlongtrack", "actionAcrosstrack"};
	for (int i = 0; i < 3; i++)
		m->viewAct[i] = meChild<QAction>(win, views[i], missing);
	const char *beams[3] = {"actionShowFlags", "actionShowDetects", "actionShowPulses"};
	for (int i = 0; i < 3; i++)
		m->beamAct[i] = meChild<QAction>(win, beams[i], missing);
	const char *plots[13] = {"actionPlotWide", "actionPlotTime", "actionPlotInterval", "actionPlotLon", "actionPlotLat",
	                         "actionPlotHeading", "actionPlotSpeed", "actionPlotDepth", "actionPlotAltitude",
	                         "actionPlotSensorDepth", "actionPlotRoll", "actionPlotPitch", "actionPlotHeave"};
	for (int i = 0; i < 13; i++)
		m->plotAct[i] = meChild<QAction>(win, plots[i], missing);
	m->showSoundingsAct = meChild<QAction>(win, "actionShowFlaggedSoundings", missing);
	m->showProfilesAct = meChild<QAction>(win, "actionShowFlaggedProfiles", missing);
	m->reverseKeysAct = meChild<QAction>(win, "actionReverseKeys", missing);
	m->reverseMouseAct = meChild<QAction>(win, "actionReverseMouse", missing);
	auto *actOpen = meChild<QAction>(win, "actionOpen", missing);
	auto *actFileList = meChild<QAction>(win, "actionFileList", missing);
	auto *actGoto = meChild<QAction>(win, "actionGoto", missing);
	auto *actBuffer = meChild<QAction>(win, "actionBuffer", missing);
	auto *actAnnotation = meChild<QAction>(win, "actionAnnotation", missing);
	auto *actFilters = meChild<QAction>(win, "actionFilters", missing);
	auto *host_w = meChild<QWidget>(win, "canvasHost", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBedit", "mbedit.ui lacks: " + missing.join(", "));
		delete win;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.icon.isNull())
		win->setWindowIcon(host.icon);

	// the drawing canvas
	auto *lay = new QVBoxLayout(host_w);
	lay->setContentsMargins(0, 0, 0, 0);
	m->canvas = new MbCanvas(host_w);
	lay->addWidget(m->canvas);
	g_xg.widget = m->canvas;
	g_xg.font = QFont("Courier New");
	g_xg.font.setStyleHint(QFont::TypeWriter);
	g_xg.font.setBold(true);
	g_xg.font.setPixelSize(13);
	g_xg.font.setStyleStrategy(QFont::NoAntialias);

	// exclusive groups (the Motif radio toggles)
	auto *gView = new QActionGroup(win);
	for (QAction *a : m->viewAct)
		gView->addAction(a);
	auto *gBeam = new QActionGroup(win);
	for (QAction *a : m->beamAct)
		gBeam->addAction(a);
	auto *gPlot = new QActionGroup(win);
	for (QAction *a : m->plotAct)
		gPlot->addAction(a);

	// buttons
	QObject::connect(m->quitButton, &QPushButton::clicked, win, [m]() { mbParkQuit(m->parking); });
	// do_next_buffer
	QObject::connect(m->nextButton, &QPushButton::clicked, win, [m]() {
		int quit = 0;
		m->expose_plot_ok = false;
		meDone(mbedit_action_next_buffer(m->hold_size, m->buffer_size, ME_PLOT_ARGS(m), &m->ndumped, &m->nloaded,
		                                 ME_OUT_ARGS(m), &quit));
		meSetupData(m);
		m->expose_plot_ok = true;
	});
	// do_done
	QObject::connect(m->doneButton, &QPushButton::clicked, win, [m]() {
		m->expose_plot_ok = false;
		meCloseCurrent(m);
		/* if there is another file in the list open it */
		if (m->currentfile >= 0 && m->currentfile < int(m->files.size()) - 1)
			meLoadSpecificFile(m, m->currentfile + 1);
		else
			m->currentfile = -1;
		m->expose_plot_ok = true;
		meSetupData(m);
	});
	QObject::connect(m->forwardButton, &QPushButton::clicked, win, [m]() { meStepOnce(m, +1); });
	QObject::connect(m->reverseButton, &QPushButton::clicked, win, [m]() { meStepOnce(m, -1); });
	QObject::connect(m->startButton, &QPushButton::clicked, win, [m]() {
		meDone(mbedit_action_step(-m->icurrent, ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
	});
	QObject::connect(m->endButton, &QPushButton::clicked, win, [m]() {
		meDone(mbedit_action_step(m->nbuffer - m->icurrent - 1, ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
	});
	QObject::connect(m->flagViewButton, &QPushButton::clicked, win, [m]() {
		meDone(mbedit_action_flag_view(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
	});
	QObject::connect(m->unflagViewButton, &QPushButton::clicked, win, [m]() {
		meDone(mbedit_action_unflag_view(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
	});
	QObject::connect(m->unflagForwardButton, &QPushButton::clicked, win, [m]() {
		meDone(mbedit_action_unflag_all(ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
	});
	QObject::connect(m->aboutButton, &QPushButton::clicked, win, [m]() { meAbout(m); });

	// sliders: do_scale_x, do_scale_y, do_number_pings, do_number_step
	meOnScale(m->width, [m](int v) {
		m->mplot_width = v;
		const int maxx = meDoubleOrHalve(v, m->width.maximum(), 0);
		if (maxx != m->width.maximum())
			m->width.setMax(maxx);
		mePlot(m);
	});
	meOnScale(m->exag, [m](int v) {
		m->mexagger = v;
		const int maxx = meDoubleOrHalve(v, m->exag.maximum(), 0);
		if (maxx != m->exag.maximum())
			m->exag.setMax(maxx);
		mePlot(m);
	});
	meOnScale(m->pings, [m](int v) {
		/* Save old ratio of mplot_size / step */
		const double ratio = double(m->step_n) / double(m->mplot_size);
		m->mplot_size = v;
		int maxx = meDoubleOrHalve(v, m->pings.maximum(), m->plot_size_max);
		if (maxx != m->pings.maximum())
			m->pings.setMax(maxx);
		/* set step to have same ratio with mplot_size as before
		    also set slider maximum to the same as for mplot_size */
		m->step_n = int(ratio * m->mplot_size);
		if (m->step_n < 1)
			m->step_n = 1;
		if (m->step_n > m->mplot_size)
			m->step_n = m->mplot_size;
		m->step.setMax(maxx);
		m->step.setValue(m->step_n);
		mePlot(m);
	});
	meOnScale(m->step, [m](int v) {
		m->step_n = v;
		const int maxx = meDoubleOrHalve(v, m->step.maximum(), m->plot_size_max);
		if (maxx != m->step.maximum())
			m->step.setMax(maxx);
		if (m->step_n > m->mplot_size) {
			m->mplot_size = m->step_n;
			m->pings.setMax(maxx);
			m->pings.setValue(m->mplot_size);
		}
	});

	// mode toggles
	for (int i = 0; i < 6; i++)
		QObject::connect(m->modeButton[i], &QRadioButton::toggled, win, [m, i](bool on) {
			if (on)
				meSetMode(m, i);
		});

	// File menu
	QObject::connect(actOpen, &QAction::triggered, win, [m]() { meOpenDialog(m); });
	QObject::connect(actFileList, &QAction::triggered, win, [m]() { meFileListDialog(m); });

	// View menu: do_view_mode, do_show_flaggedsoundings/profiles, do_show_flags/detects/pulsetypes, do_show_time
	for (int i = 0; i < 3; i++) {
		QObject::connect(m->viewAct[i], &QAction::triggered, win, [m, i]() { meSetView(m, i); });
		QObject::connect(m->beamAct[i], &QAction::triggered, win, [m, i]() {
			m->mshow_beammode = i;
			mePlot(m);
		});
	}
	QObject::connect(m->showSoundingsAct, &QAction::toggled, win, [m](bool on) {
		m->mshow_flaggedsoundings = on;
		mePlot(m);
	});
	QObject::connect(m->showProfilesAct, &QAction::toggled, win, [m](bool on) {
		m->mshow_flaggedprofiles = on;
		mePlot(m);
	});
	for (int i = 0; i < 13; i++)
		QObject::connect(m->plotAct[i], &QAction::triggered, win, [m, i]() {
			m->mshow_time = i;
			/* reset scaling */
			mbedit_set_scaling(m->mb_borders, m->mshow_time);
			mePlot(m);
		});

	// Controls menu
	QObject::connect(actGoto, &QAction::triggered, win, [m]() { meGotoDialog(m); });
	QObject::connect(actBuffer, &QAction::triggered, win, [m]() { meBufferDialog(m); });
	QObject::connect(actAnnotation, &QAction::triggered, win, [m]() { meAnnotationDialog(m); });
	QObject::connect(actFilters, &QAction::triggered, win, [m]() { meFiltersDialog(m); });
	QObject::connect(m->reverseKeysAct, &QAction::toggled, win, [m](bool on) { m->mode_reverse_keys = on; });
	QObject::connect(m->reverseMouseAct, &QAction::toggled, win, [m](bool on) { m->mode_reverse_mouse = on; });

	// middle / right button held: keep stepping after 2 seconds
	m->scrollTimer = new QTimer(win);
	m->scrollTimer->setInterval(60);
	QObject::connect(m->scrollTimer, &QTimer::timeout, win, [m]() {
		if (m->scrollStep == 0 || !(QApplication::mouseButtons() & (Qt::MiddleButton | Qt::RightButton | Qt::LeftButton))) {
			m->scrollTimer->stop();
			m->scrollStep = 0;
			return;
		}
		if (m->scrollClock.elapsed() > 2000)
			meStepOnce(m, m->scrollStep);
	});

	// do_mbedit_settimer: keep the file list's lock / esf marks current
	m->fileListTimer = new QTimer(win);
	m->fileListTimer->setInterval(1000);
	QObject::connect(m->fileListTimer, &QTimer::timeout, win, [m]() {
		if (m->fileListDlg && m->fileListDlg->isVisible() && !m->files.empty() && m->expose_plot_ok)
			meBuildFileList(m);
	});
	m->fileListTimer->start();

	win->installEventFilter(new MbCloseFilter(win));
	m->parking = mbParkable(win, host, "mbedit");   // after the close filter: a park comes first
	return m;
}

} // namespace

// ---- the engine's host hooks (declared in mbedit.h) ----------------------------------------
extern "C" {

void xg_drawline(void *, int x1, int y1, int x2, int y2, unsigned int pixel, int style) {
	if (QPainter *p = xgPainter()) {
		p->setPen(xgPen(pixel, style));
		p->drawLine(x1, y1, x2, y2);
	}
}

void xg_drawrectangle(void *, int x, int y, int width, int height, unsigned int pixel, int style) {
	if (QPainter *p = xgPainter()) {
		p->setPen(xgPen(pixel, style));
		p->setBrush(Qt::NoBrush);
		p->drawRect(x, y, width, height);
	}
}

void xg_fillrectangle(void *, int x, int y, int width, int height, unsigned int pixel, int) {
	if (QPainter *p = xgPainter())
		p->fillRect(x, y, width, height, pixel < unsigned(kNColors) ? kColors[pixel] : kColors[1]);
}

void xg_drawstring(void *, int x, int y, char *string, unsigned int pixel, int style) {
	if (QPainter *p = xgPainter()) {
		p->setPen(xgPen(pixel, style));
		p->drawText(x, y, QString::fromLatin1(string));
	}
}

void xg_justify(void *, char *string, int *width, int *ascent, int *descent) {
	const QFontMetrics fm(g_xg.font);
	*width = fm.horizontalAdvance(QString::fromLatin1(string));
	*ascent = fm.ascent();
	*descent = fm.descent();
}

int do_message_on(char *message) {
	if (g_me && g_me->host.busyText)
		g_me->host.busyText(message);
	return 1;
}

int do_message_off(void) {
	if (g_me && g_me->host.busyOff)
		g_me->host.busyOff();
	return 1;
}

int do_error_dialog(char *s1, char *s2, char *s3) {
	QApplication::beep();
	QMessageBox::warning(g_me ? g_me->win : nullptr, "MBedit",
	                     QString::fromUtf8(s1) + "\n" + QString::fromUtf8(s2) + "\n" + QString::fromUtf8(s3));
	return 1;
}

void do_filebutton_on(void) {
	MbEdit *m = g_me;
	if (!m)
		return;
	m->doneButton->setEnabled(false);
	m->doneButton->setText("Done");
	m->forwardButton->setEnabled(false);
	m->reverseButton->setEnabled(false);
	m->startButton->setEnabled(false);
	m->endButton->setEnabled(false);
}

void do_filebutton_off(void) {
	MbEdit *m = g_me;
	if (!m)
		return;
	m->doneButton->setEnabled(true);
	const int numfiles = int(m->files.size());
	if (numfiles > 0 && m->currentfile >= 0 && m->currentfile < numfiles - 1)
		m->doneButton->setText("Next File");
	else
		m->doneButton->setText("Done");
	m->forwardButton->setEnabled(true);
	m->reverseButton->setEnabled(true);
	m->startButton->setEnabled(true);
	m->endButton->setEnabled(true);
}

void do_nextbutton_on(void) {
	if (g_me)
		g_me->nextButton->setEnabled(true);
}

void do_nextbutton_off(void) {
	if (g_me)
		g_me->nextButton->setEnabled(false);
}

int do_reset_scale_x(int pwidth, int maxx, int xntrvl, int yntrvl) {
	MbEdit *m = g_me;
	if (!m)
		return 1;
	m->mplot_width = pwidth;

	/* check max value */
	if (pwidth > maxx - 1) {
		maxx = 2 * pwidth;
		if (maxx < 2)
			maxx = 2;
	}
	m->mx_interval = xntrvl;
	m->my_interval = yntrvl;

	/* set values of plot width slider */
	m->width.setRange(1, maxx);
	m->width.setValue(m->mplot_width);

	/* set values of x and y interval sliders */
	m->xInterval.setValue(m->mx_interval);
	m->yInterval.setValue(m->my_interval);
	return 1;
}

} // extern "C"


// ---- entry point ---------------------------------------------------------------------------
bool mbeditOpenWindow(QWidget *parent, const MbEditHost &host, const QString &file, int format, int useEsf) {
	if (!g_me) {
		if (!meLoadMbio(parent, host))
			return false;
		MbEdit *m = meBuild(parent, host);
		if (!m)
			return false;
		g_me = m;

		// do_mbedit_init
		m->expose_plot_ok = false;
		static unsigned int mpixel_values[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
		mbedit_set_graphics(&g_xg, 11, mpixel_values);
		mbedit_set_scaling(m->mb_borders, m->mshow_time);
		int startup_file = 0, startup_use_esf = 0;
		mbedit_init(&startup_file, &startup_use_esf);
		meSetupData(m);
		do_filebutton_on();
		do_nextbutton_off();

		if (host.windowOpened)
			host.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (g_xg.p) {
				g_xg.p->end();
				delete g_xg.p;
				g_xg.p = nullptr;
			}
			g_xg.img = QImage();
			g_xg.widget = nullptr;
			if (m->host.windowClosed)
				m->host.windowClosed();
			if (g_me == m)
				g_me = nullptr;
			delete m;
		});

		m->win->show();
		m->expose_plot_ok = true;
		// the canvas gets its size (and so its image) once the window is laid out
		QApplication::processEvents();

		// No file given: offer File > Open straight away.
		if (file.isEmpty())
			QTimer::singleShot(0, m->win, [m]() { meOpenDialog(m); });
	}
	MbEdit *m = g_me;
	mbParkRebind(m->parking, host.parkScene);
	mbParkShow(m->parking);                              // a parked one comes back off its handle
	if (!file.isEmpty())
		return meOpenInput(m, QDir::fromNativeSeparators(file).toUtf8(), format, useEsf);
	return true;
}

void mbeditSetMbioHint(const QString &path) {
	g_meMbioHint = path;
}

bool mbeditLoadMbio(QWidget *parent, const MbEditHost &host) {
	return meLoadMbio(parent, host);
}

int mbeditState(int *out, int n) {
	MbEdit *m = g_me;
	int nflagged = 0, nunflagged = 0;
	if (m)
		mbedit_count_flags(&nflagged, &nunflagged);
	const int v[9] = {m ? 1 : 0,
	                  m ? int(m->files.size()) : 0,
	                  m ? m->currentfile : -1,
	                  m ? m->nbuffer : 0,
	                  m ? m->ngood : 0,
	                  m ? m->icurrent : 0,
	                  m ? m->mnplot : 0,
	                  nflagged,
	                  nunflagged};
	const int k = n < 9 ? n : 9;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool mbeditKey(int ch) {
	if (!g_me)
		return false;
	const QString t = QString(QChar(char16_t(ch)));
	QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, t);
	QApplication::sendEvent(g_me->canvas, &press);
	QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, t);
	QApplication::sendEvent(g_me->canvas, &release);
	return true;
}

bool mbeditClick(int x, int y) {
	MbEdit *m = g_me;
	if (!m)
		return false;
	m->grab_mode = GRAB_START;
	meMouseEdit(m, x, y);
	if (m->grab_mode == GRAB_MOVE) {
		meDone(mbedit_action_mouse_grab(GRAB_END, x, y, ME_PLOT_ARGS(m), ME_OUT_ARGS(m)));
		m->grab_mode = GRAB_START;
	}
	return true;
}

bool mbeditSavePng(const QString &path) {
	if (!g_me)
		return false;
	xgFlush();
	return g_xg.img.save(path, "PNG");
}

bool mbeditClose() {
	if (!g_me)
		return false;
	mbParkQuit(g_me->parking);
	return true;
}

// ---- parking: the ONE implementation every MB-System tool window uses (mbedit_window.h) ------
struct MbParking : QObject {
	QWidget *win;
	MbEditHost host;
	QString label;
	std::function<void *()> where;
	void *parkedIn = nullptr;            // the viewer window whose Scene Objects holds the handle
	bool reallyClose = false;            // Quit / the handle's Delete

	MbParking(QWidget *w, const MbEditHost &h, const QString &l, std::function<void *()> wh)
		: QObject(w), win(w), host(h), label(l), where(std::move(wh)) {}
	~MbParking() override { unparkNow(); }   // no handle may outlive the window it brings back

	void unparkNow() {
		if (parkedIn && host.unpark)
			host.unpark(parkedIn, win);      // the host checks the viewer window is still alive
		parkedIn = nullptr;
	}
	// hide and leave a handle; false when there is no viewer window to hold one
	bool park() {
		void *pref = where ? where() : nullptr;
		void *into = host.parkWhere ? host.parkWhere(pref ? pref : host.parkScene) : nullptr;
		if (!into || !host.park)
			return false;
		unparkNow();                         // re-parked elsewhere: never two handles
		win->hide();
		parkedIn = into;
		const QByteArray l = label.toUtf8();
		host.park(into, win, l.constData(), [this]() { mbParkShow(this); }, [this]() { mbParkQuit(this); });
		return true;
	}
	bool eventFilter(QObject *o, QEvent *e) override {
		if (o == win && e->type() == QEvent::Close && !reallyClose && park()) {
			e->ignore();
			return true;                     // the tool's own close filter (its quit) never sees it
		}
		return QObject::eventFilter(o, e);
	}
};

MbParking *mbParkable(QWidget *win, const MbEditHost &host, const QString &label, std::function<void *()> where) {
	if (!win)
		return nullptr;
	auto *p = new MbParking(win, host, label, std::move(where));
	win->installEventFilter(p);
	// minimise means park: the viewer's shared handler (it hides first; with nowhere to park, back it comes)
	if (host.parkOnMinimise)
		host.parkOnMinimise(win, [p]() {
			if (!p->park())
				p->win->show();
		});
	return p;
}

void mbParkShow(MbParking *p) {
	if (!p)
		return;
	p->unparkNow();
	p->win->setWindowState(p->win->windowState() & ~Qt::WindowMinimized);
	p->win->showNormal();
	p->win->raise();
	p->win->activateWindow();
}

void mbParkRebind(MbParking *p, void *scene) {
	if (p && scene)
		p->host.parkScene = scene;
}

void mbParkQuit(MbParking *p) {
	if (!p)
		return;
	p->reallyClose = true;
	p->unparkNow();
	if (!p->win->close())
		p->reallyClose = false;              // the tool kept itself open: its X parks again
}
