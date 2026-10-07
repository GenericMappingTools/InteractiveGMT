// ============================================================================
//  mbnavedit_window.cpp -- the interactive navigation editor's Qt window: the port of MB-System's
//  src/mbnavedit/mbnavedit_callbacks.c (the Motif interface of mbnavedit) onto Qt, plus the xg_*
//  graphics the engine draws through (MB-System's mbaux/mb_xgraphics.c), painted into a QImage.
//
//  A translation unit of its own, apart from the viewer's fragments (see mbnavedit_window.h).
//  The engine (mbnavedit.c) holds the buffer and does every edit, model and drawing decision; this
//  file only turns widgets, keys and mouse events into the same engine calls the Motif callbacks
//  made, with the same arguments, in the same order. The globals the callbacks shared with the
//  engine are the fields of mbnavedit_g (mbnavedit.h).
//
//  Drawing model, kept from X11: the engine draws straight onto the canvas and "erases" by drawing
//  again in white (a toggled selection, the Pick Zoom bounds). So the canvas is a persistent
//  QImage, painted with NO antialiasing (a white line over an antialiased one would leave a grey
//  ghost), and shown as is.
//
//  Departures from the Motif program, all forced by the host:
//    - MBIO is the swath editor's (mbeditLoadMbio): found, or asked for, once for every MB tool.
//    - The window opens empty; the files and switches of the command line (-I -F -D -X -P -N) are
//      mbnaveditOpenWindow's, and File > Open's.
//    - Quit closes the tool instead of ending the program; the window's X and minimise park it in
//      Scene Objects, as every MB-System tool window does.
//    - The left-button drag of Select / Deselect is event driven (mouse moves while the button is
//      down) instead of polling XQueryPointer.
//    - The plots follow the window's size: their width is the plot area's, their height shares it
//      out among the plots shown (never under kMinPlotHeight, below which the area scrolls).
//    - The file selection box is the platform file dialog plus a small Open dialog (file, format id,
//      output mode); the file list shows each file and its format only, and is rebuilt when it
//      changes instead of on a one-second timer.
//    - The message and error bulletin boards are the viewer's busy notice and a message box.
// ============================================================================

#include "mbnavedit_window.h"
#include "mbnavedit.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
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
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTextBrowser>
#include <QUiLoader>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

// the swath editor's engine: the MBIO version string and the datalist reader (do_parse_datalist)
extern "C" const char *mbedit_mbio_version(void);
extern "C" void mbedit_parse_datalist(char *file, int form, void (*add)(void *ctx, const char *path, int format), void *ctx);

namespace {

// MB-System's status values (mb_status.h)
const int MB_SUCCESS = 1;
const int MB_FAILURE = 0;

// the smallest height a plot is given before the plot area scrolls instead
const int kMinPlotHeight = 120;

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
class MnCanvas;

struct XgCanvas {
	QImage img;
	QPainter *p = nullptr;
	QFont font;
	QPointer<QWidget> widget;
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
	pen.setWidth(0);                       // one pixel
	if (style == XG_DASHLINE)
		pen.setDashPattern({4, 4});        // X11's default LineOnOffDash
	return pen;
}

// One user action is many engine draws; the painter stays open across them and the widget is
// repainted once at the end.
void xgFlush() {
	if (g_xg.p) {
		g_xg.p->end();
		delete g_xg.p;
		g_xg.p = nullptr;
	}
	if (g_xg.widget)
		g_xg.widget->update();
}

// ---- one Motif XmScale: slider + value label ---------------------------------------------------
struct MnScale {
	QSlider *s = nullptr;
	QLabel *val = nullptr;
	int decimals = 0;                      // XmNdecimalPoints

	QString fmt(int v) const {
		return decimals ? QString::number(v / std::pow(10.0, decimals), 'f', decimals) : QString::number(v);
	}
	void set(int minimum, int maximum, int v) {
		if (!s)
			return;
		QSignalBlocker b(s);
		s->setRange(minimum, maximum);
		s->setValue(v);
		if (val)
			val->setText(fmt(s->value()));
	}
	void setValue(int v) {
		if (s)
			set(s->minimum(), s->maximum(), v);
	}
};

struct MnFile {
	std::string path;
	int format = 0;
};

// ---- the tool: mbnavedit_callbacks.c's globals ------------------------------------------------
struct MbNavEdit {
	MbParking *parking = nullptr;        // X / minimise park it in Scene Objects (mbParkable)
	MbEditHost host;
	QMainWindow *win = nullptr;
	QWidget *central = nullptr;
	QScrollArea *scroll = nullptr;
	QWidget *canvas = nullptr;            // the MnCanvas

	// the main window's widgets, by their Motif names
	QPushButton *pushButton_start = nullptr, *pushButton_reverse = nullptr, *pushButton_forward = nullptr,
	            *pushButton_end = nullptr, *pushButton_done = nullptr, *pushButton_solution = nullptr,
	            *pushButton_flag = nullptr, *pushButton_unflag = nullptr, *pushButton_speed_smg = nullptr,
	            *pushButton_heading_cmg = nullptr;
	QAction *pushButton_file = nullptr, *pushButton_controls_timeinterpolation = nullptr,
	        *pushButton_controls_deletebadtimetag = nullptr;
	QCheckBox *toggleButton_time = nullptr, *toggleButton_org_time = nullptr, *toggleButton_lon = nullptr,
	          *toggleButton_org_lon = nullptr, *toggleButton_dr_lon = nullptr, *toggleButton_lat = nullptr,
	          *toggleButton_org_lat = nullptr, *toggleButton_dr_lat = nullptr, *toggleButton_speed = nullptr,
	          *toggleButton_org_speed = nullptr, *toggleButton_show_smg = nullptr, *toggleButton_heading = nullptr,
	          *toggleButton_org_heading = nullptr, *toggleButton_show_cmg = nullptr, *toggleButton_sensordepth = nullptr,
	          *toggleButton_org_sensordepth = nullptr, *toggleButton_vru = nullptr;
	QRadioButton *modeButton[5] = {};    // Pick, Select, Deselect, Select All, Deselect All

	// dialogs, built on first use and kept
	QDialog *openDlg = nullptr, *fileListDlg = nullptr, *stepDlg = nullptr, *modelDlg = nullptr,
	        *timeInterpDlg = nullptr, *deleteBadDlg = nullptr, *offsetDlg = nullptr;
	QLineEdit *openFile = nullptr;
	QSpinBox *textField_format = nullptr;
	QRadioButton *toggleButton_output_on = nullptr, *toggleButton_output_off = nullptr;
	QCheckBox *openMbprocess = nullptr;
	QListWidget *list_filelist = nullptr;
	QRadioButton *toggleButton_output_on_filelist = nullptr, *toggleButton_output_off_filelist = nullptr;
	MnScale scale_timespan, scale_timestep, scale_meantimewindow, scale_driftlon, scale_driftlat;
	QLabel *label_timespan_2 = nullptr, *label_timestep_2 = nullptr;
	QRadioButton *modelButton[4] = {};   // Off, Gaussian Mean, Dead Reckoning, Inversion
	QLineEdit *textField_modeling_speed = nullptr, *textField_modeling_acceleration = nullptr;
	QLineEdit *textField_lon_offset = nullptr, *textField_lat_offset = nullptr;

	// mbnavedit_callbacks.c state
	bool expose_plot_ok = true;
	std::vector<MnFile> files;           // filepaths / fileformats
	int currentfile = -1;
	int currentfile_shown = -1;
	int usePrevious = -1;                // the .nve question: -1 ask, 0 no, 1 yes (mbnaveditOpenWindow)

	// mouse: the left-button drag of do_event
	bool leftDown = false;
};
MbNavEdit *g_mn = nullptr;

void mnSetVisible(QWidget *w, bool on) {
	if (w)
		w->setVisible(on);
}

// "get and set size of canvas": count the plots shown (do_set_controls and every plot toggle)
void mnCountPlots() {
	mbnavedit_g.number_plots = 0;
	if (mbnavedit_g.plot_tint)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_lon)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_lat)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_speed)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_heading)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_draft)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_roll)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_pitch)
		mbnavedit_g.number_plots++;
	if (mbnavedit_g.plot_heave)
		mbnavedit_g.number_plots++;
}

// XtVaSetValues(drawingArea, XmNwidth, plot_width, XmNheight, number_plots * plot_height): the
// canvas takes the plot area's width, and its height is shared out among the plots shown.
void mnSizeCanvas(MbNavEdit *m) {
	if (!m->scroll || !m->canvas)
		return;
	const QSize vp = m->scroll->maximumViewportSize();
	const int nplots = mbnavedit_g.number_plots > 0 ? mbnavedit_g.number_plots : 1;
	int width = vp.width();
	int height = vp.height() / nplots;
	if (height < kMinPlotHeight) {
		height = kMinPlotHeight;   // the area scrolls: leave room for its scroll bar
		width -= m->scroll->verticalScrollBar()->sizeHint().width();
	}
	mbnavedit_g.plot_width = width > 100 ? width : 100;
	mbnavedit_g.plot_height = height;
	const QSize need(mbnavedit_g.plot_width, nplots * mbnavedit_g.plot_height);
	if (m->canvas->size() != need)
		m->canvas->setFixedSize(need);
	if (g_xg.img.size() != need) {
		if (g_xg.p) {
			g_xg.p->end();
			delete g_xg.p;
			g_xg.p = nullptr;
		}
		g_xg.img = QImage(need, QImage::Format_RGB32);
		g_xg.img.fill(Qt::white);
	}
}

// replot (the closing lines of nearly every callback)
void mnPlot(MbNavEdit *m) {
	mnSizeCanvas(m);
	mbnavedit_plot_all();
	xgFlush();
}

// mbnavedit_bell
void mnBell() {
	QApplication::beep();
}

// the canvas's cursor (mbnavedit_pickcursor, _selectcursor, ...)
void mnCursor(MbNavEdit *m);

// do_unset_interval
int mnUnsetInterval(MbNavEdit *m) {
	/* turn off set interval mode */
	mbnavedit_action_set_interval(0, 0, 3);
	if (mbnavedit_g.mode_set_interval) {
		mbnavedit_g.mode_set_interval = false;
		mnCursor(m);
	}
	return (MB_SUCCESS);
}

// the time span / time step sliders of the Time Stepping dialog
void mnSetTimeSpanSlider(MbNavEdit *m) {
	/* set values of number of data shown slider */
	m->scale_timespan.set(1, mbnavedit_g.data_show_max, mbnavedit_g.data_show_size > 1 ? mbnavedit_g.data_show_size : 1);
	if (m->label_timespan_2)
		m->label_timespan_2->setText(QString::number(mbnavedit_g.data_show_max));
}
void mnSetTimeStepSlider(MbNavEdit *m) {
	/* set values of number of data to step slider */
	m->scale_timestep.set(1, mbnavedit_g.data_step_max, mbnavedit_g.data_step_size);
	if (m->label_timestep_2)
		m->label_timestep_2->setText(QString::number(mbnavedit_g.data_step_max));
}

// ---- left-button actions (do_event, ButtonPress, button 1) ------------------------------------
void mnLeftAction(MbNavEdit *m, int x_loc, int y_loc) {
	(void)m;
	if (mbnavedit_g.mode_set_interval) {
		const int status = mbnavedit_action_set_interval(x_loc, y_loc, 0);
		if (status == MB_FAILURE)
			mnBell();
	}
	else if (mbnavedit_g.mode_pick == PICK_MODE_PICK)
		mbnavedit_action_mouse_pick(x_loc, y_loc);
	else if (mbnavedit_g.mode_pick == PICK_MODE_SELECT)
		mbnavedit_action_mouse_select(x_loc, y_loc);
	else if (mbnavedit_g.mode_pick == PICK_MODE_DESELECT)
		mbnavedit_action_mouse_deselect(x_loc, y_loc);
	else if (mbnavedit_g.mode_pick == PICK_MODE_SELECTALL)
		mbnavedit_action_mouse_selectall(x_loc, y_loc);
	else if (mbnavedit_g.mode_pick == PICK_MODE_DESELECTALL)
		mbnavedit_action_mouse_deselectall(x_loc, y_loc);
	xgFlush();
}

// the pick mode radio box (do_toggle_pick ... do_toggle_deselectall, and the keys of do_event)
void mnSetMode(MbNavEdit *m, int mode) {
	mbnavedit_g.mode_pick = mode;
	mnUnsetInterval(m);
	if (mode >= 0 && mode < 5 && m->modeButton[mode]) {
		QSignalBlocker b(m->modeButton[mode]);
		m->modeButton[mode]->setChecked(true);
	}
	mnCursor(m);
}

// ---- the canvas widget ---------------------------------------------------------------------
class MnCanvas : public QWidget {
public:
	explicit MnCanvas(QWidget *parent) : QWidget(parent) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setFocusPolicy(Qt::StrongFocus);
		setCursor(Qt::CrossCursor);       // XC_target
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		if (g_xg.img.isNull())
			p.fillRect(rect(), Qt::white);
		else
			p.drawImage(0, 0, g_xg.img);
	}

	// do_event, KeyPress
	void keyPressEvent(QKeyEvent *e) override {
		MbNavEdit *m = g_mn;
		if (!m || e->text().isEmpty()) {
			QWidget::keyPressEvent(e);
			return;
		}
		switch (e->text().at(0).toLatin1()) {
		case 'Y':
		case 'y':
		case 'Q':
		case 'q':
			mnSetMode(m, PICK_MODE_PICK);
			break;
		case 'U':
		case 'u':
		case 'W':
		case 'w':
			mnSetMode(m, PICK_MODE_SELECT);
			break;
		case 'I':
		case 'i':
		case 'E':
		case 'e':
			mnSetMode(m, PICK_MODE_DESELECT);
			break;
		case 'O':
		case 'o':
		case 'R':
		case 'r':
			mnSetMode(m, PICK_MODE_SELECTALL);
			break;
		case 'P':
		case 'p':
		case 'T':
		case 't':
			mnSetMode(m, PICK_MODE_DESELECTALL);
			break;
		default:
			QWidget::keyPressEvent(e);
			break;
		}
	}

	// do_event, ButtonPress
	void mousePressEvent(QMouseEvent *e) override {
		MbNavEdit *m = g_mn;
		if (!m)
			return;
		setFocus();
		const int x_loc = int(e->position().x()), y_loc = int(e->position().y());
		/* If left mouse button is pushed then
		  pick, erase, restore or set time interval. */
		if (e->button() == Qt::LeftButton) {
			m->leftDown = true;
			mnLeftAction(m, x_loc, y_loc);
		}
		/* If middle mouse button is pushed. */
		else if (e->button() == Qt::MiddleButton) {
			/* set second interval bound */
			if (mbnavedit_g.mode_set_interval) {
				const int status = mbnavedit_action_set_interval(x_loc, y_loc, 1);
				if (status == MB_FAILURE)
					mnBell();
			}
			/* scroll in reverse */
			else {
				const int status = mbnavedit_action_step(-mbnavedit_g.data_step_size);
				if (status == 0)
					mnBell();
			}
			xgFlush();
		}
		/* If right mouse button is pushed. */
		else if (e->button() == Qt::RightButton) {
			/* apply interval bounds */
			if (mbnavedit_g.mode_set_interval) {
				const int status = mbnavedit_action_set_interval(0, 0, 2);
				if (status == MB_FAILURE)
					mnBell();
				mnUnsetInterval(m);
				mnSetTimeSpanSlider(m);
				mnSetTimeStepSlider(m);
			}
			/* scroll forward */
			else {
				const int status = mbnavedit_action_step(mbnavedit_g.data_step_size);
				if (status == 0)
					mnBell();
			}
			xgFlush();
		}
	}

	// the left button is still pressed: run the action again where the pointer is (the
	// XQueryPointer loop of do_event; Pick and the interval bound act once per press)
	void mouseMoveEvent(QMouseEvent *e) override {
		MbNavEdit *m = g_mn;
		if (!m || !m->leftDown || mbnavedit_g.mode_pick == PICK_MODE_PICK || mbnavedit_g.mode_set_interval)
			return;
		mnLeftAction(m, int(e->position().x()), int(e->position().y()));
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		MbNavEdit *m = g_mn;
		if (m && e->button() == Qt::LeftButton)
			m->leftDown = false;
	}
};

void mnCursor(MbNavEdit *m) {
	if (!m || !m->canvas)
		return;
	QWidget *c = m->canvas;
	if (mbnavedit_g.mode_set_interval)
		c->setCursor(Qt::SplitHCursor);                       // XC_crosshair
	else if (mbnavedit_g.mode_pick == PICK_MODE_PICK)
		c->setCursor(Qt::CrossCursor);                        // XC_target
	else if (mbnavedit_g.mode_pick == PICK_MODE_SELECT || mbnavedit_g.mode_pick == PICK_MODE_DESELECT)
		c->setCursor(Qt::PointingHandCursor);                 // XC_exchange
	else
		c->setCursor(Qt::SizeAllCursor);                      // XC_cross
}

// ---- small helpers -------------------------------------------------------------------------
QWidget *mnLoadUi(MbNavEdit *m, const char *file, QWidget *parent) {
	QFile f(QDir(m->host.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBnavedit", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "MBnavedit", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

// A child the .ui must carry; a missing one is reported by name, so a renamed widget in the .ui
// is found at once.
template <typename T>
T *mnChild(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

void mnBindScale(QWidget *root, const char *name, int decimals, MnScale &sc, QStringList &missing) {
	sc.s = mnChild<QSlider>(root, name, missing);
	sc.val = mnChild<QLabel>(root, (std::string(name) + "Value").c_str(), missing);
	sc.decimals = decimals;
}

// Motif's XmScale called back on release (and on keyboard steps); the value label follows the
// drag live.
void mnOnScale(MnScale &sc, std::function<void(int)> fn) {
	QSlider *s = sc.s;
	MnScale *scp = &sc;
	QObject::connect(s, &QSlider::valueChanged, s, [s, scp, fn](int v) {
		if (scp->val)
			scp->val->setText(scp->fmt(v));
		if (!s->isSliderDown())
			fn(v);
	});
	QObject::connect(s, &QSlider::sliderReleased, s, [s, fn]() { fn(s->value()); });
}

void mnShow(QDialog *d) {
	d->show();
	d->raise();
	d->activateWindow();
}

// a loaded dialog, or nullptr (with the .ui's missing widgets reported)
QDialog *mnDialog(MbNavEdit *m, const char *file) {
	QWidget *w = mnLoadUi(m, file, m->win);
	auto *d = qobject_cast<QDialog *>(w);
	if (!d) {
		delete w;
		return nullptr;
	}
	d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
	return d;
}

bool mnMissing(MbNavEdit *m, QDialog *d, const char *file, const QStringList &missing) {
	if (missing.isEmpty())
		return false;
	QMessageBox::warning(m->win, "MBnavedit", QString::fromLatin1(file) + " lacks: " + missing.join(", "));
	delete d;
	return true;
}

// ---- the output mode: the Open dialog's radio box and the file list's mirror each other ----------
// (do_toggle_output_on / _off / _on_filelist / _off_filelist)
void mnSetOutputMode(MbNavEdit *m, int mode) {
	mbnavedit_g.output_mode = mode;
	const bool on = (mode == OUTPUT_MODE_OUTPUT);
	QRadioButton *pairs[4] = {m->toggleButton_output_on, m->toggleButton_output_off, m->toggleButton_output_on_filelist,
	                          m->toggleButton_output_off_filelist};
	for (int i = 0; i < 4; i++) {
		if (!pairs[i])
			continue;
		QSignalBlocker b(pairs[i]);
		pairs[i]->setChecked((i % 2 == 0) == on);
	}
}

// ---- file list (do_build_filelist / do_parse_datalist / do_load_specific_file / do_load) ------
void mnBuildFileList(MbNavEdit *m) {
	if (!m->list_filelist)
		return;
	const int numfiles = int(m->files.size());
	bool update_filelist = (m->list_filelist->count() != numfiles);

	/* check current file shown vs loaded */
	if (m->currentfile != m->currentfile_shown) {
		m->currentfile_shown = m->currentfile;
		update_filelist = true;
	}

	/* only rebuild the filelist if necessary */
	if (update_filelist) {
		const int selection = m->list_filelist->currentRow();
		const int item_count = m->list_filelist->count();
		m->list_filelist->clear();
		for (int i = 0; i < numfiles; i++) {
			// the file and its format only: no Motif status prefixes (<loaded>/<Locked>/<nve>)
			char value_text[2 * 1024];
			snprintf(value_text, sizeof(value_text), "%s %d", m->files[i].path.c_str(), m->files[i].format);
			m->list_filelist->addItem(QString::fromUtf8(value_text));
		}
		/* reinstate selection if the number of items is the same as before */
		if (item_count == numfiles && selection >= 0)
			m->list_filelist->setCurrentRow(selection);
	}
}

void mnAddFile(void *ctx, const char *path, int format) {
	MbNavEdit *m = static_cast<MbNavEdit *>(ctx);
	if (int(m->files.size()) >= NUM_FILES_MAX)
		return;
	MnFile f;
	f.path = path;
	f.format = format;
	m->files.push_back(f);
}

// do_load
void mnLoad(MbNavEdit *m, bool useprevious) {
	/* turn off expose plots */
	m->expose_plot_ok = false;

	/* open the file */
	const MnFile &f = m->files[m->currentfile];
	std::snprintf(mbnavedit_g.ifile, sizeof(mbnavedit_g.ifile), "%s", f.path.c_str());
	mbnavedit_g.format = f.format;
	mnSizeCanvas(m);
	const int status = mbnavedit_action_open(useprevious ? 1 : 0);

	if (status == MB_FAILURE)
		mnBell();

	mnUnsetInterval(m);

	/* set values of number of data shown slider */
	mnSetTimeSpanSlider(m);

	/* replot */
	if (status == MB_SUCCESS)
		mnPlot(m);
	else
		xgFlush();

	/* turn on expose plots */
	m->expose_plot_ok = true;

	do_set_controls();
}

// do_load_specific_file
void mnLoadSpecificFile(MbNavEdit *m, int i_file) {
	/* check the specified file is in the list */
	if (i_file < 0 || i_file >= int(m->files.size()))
		return;
	/* set current_file */
	m->currentfile = i_file;

	/* check for edit save file */
	const bool nve = QFileInfo(QString::fromStdString(m->files[i_file].path + ".nve")).isFile();

	/* if nve file exists deal with it: bring up dialog asking if nve should be used
	   (bulletinBoard_useprevious), unless the caller already said */
	if (nve && m->usePrevious >= 0) {
		mnLoad(m, m->usePrevious == 1);
	}
	else if (nve) {
		const auto ans = QMessageBox::question(m->win, "MBnavedit",
			"Previously edited navigation exists for the specified input file.\n"
			"Do you want to use the previously edited navigation?",
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		mnLoad(m, ans == QMessageBox::Yes);
	}
	/* else just try to load the data without an nve */
	else {
		mnLoad(m, false);
	}
}

// do_done / do_editlistselection / do_fileselection_ok: finish with the file loaded
void mnCloseCurrent(MbNavEdit *m) {
	int quit = 0;
	const int status = mbnavedit_action_done(&quit);
	if (status == 0)
		mnBell();
	xgFlush();
}

// do_fileselection_ok: close what is loaded, add the file (or every file of a datalist) to the
// list, load the first one added.
bool mnOpenInput(MbNavEdit *m, const QByteArray &input_file, int format) {
	/* turn off expose plots */
	m->expose_plot_ok = false;

	/* close out previously open file */
	mnCloseCurrent(m);
	m->currentfile = -1;

	/* try to parse the selection */
	const int numfilessave = int(m->files.size());
	QByteArray f = input_file;
	mbedit_parse_datalist(f.data(), format, mnAddFile, m);

	/* load first new file in the list */
	const bool added = int(m->files.size()) > numfilessave;
	if (added)
		mnLoadSpecificFile(m, numfilessave);
	else
		QMessageBox::warning(m->win, "MBnavedit",
			"No swath file was found in\n" + QString::fromUtf8(input_file) +
			"\n\nCheck the MBIO format id (-1 for a datalist).");

	/* turn on expose plots */
	m->expose_plot_ok = true;
	do_set_controls();
	return added && m->currentfile >= 0;
}

// ---- dialogs -------------------------------------------------------------------------------
// bulletinBoard_fileselection: the file, its format id and the output mode
void mnOpenDialog(MbNavEdit *m) {
	const char *ui = "mbnavedit_open.ui";
	if (!m->openDlg) {
		QDialog *d = mnDialog(m, ui);
		if (!d)
			return;
		QStringList missing;
		auto *file = mnChild<QLineEdit>(d, "fileEdit", missing);
		auto *format = mnChild<QSpinBox>(d, "textField_format", missing);
		auto *on = mnChild<QRadioButton>(d, "toggleButton_output_on", missing);
		auto *off = mnChild<QRadioButton>(d, "toggleButton_output_off", missing);
		auto *mbprocess = mnChild<QCheckBox>(d, "mbprocessCheck", missing);
		auto *browse = mnChild<QPushButton>(d, "browseButton", missing);
		auto *ok = mnChild<QPushButton>(d, "okButton", missing);
		auto *cancel = mnChild<QPushButton>(d, "cancelButton", missing);
		if (mnMissing(m, d, ui, missing))
			return;
		m->openDlg = d;
		m->openFile = file;
		m->textField_format = format;
		m->toggleButton_output_on = on;
		m->toggleButton_output_off = off;
		m->openMbprocess = mbprocess;
		auto *group = new QButtonGroup(d);
		group->addButton(on);
		group->addButton(off);
		// do_fileselection_list: a chosen file gets its format guessed from its name
		auto guess = [m]() {
			QByteArray path = QDir::toNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (path.isEmpty())
				return;
			int form = m->textField_format->value();
			if (mbnavedit_get_format(path.data(), &form) == MB_SUCCESS)
				m->textField_format->setValue(form);
		};
		QObject::connect(browse, &QPushButton::clicked, d, [m, d, guess]() {
			const QString fn = QFileDialog::getOpenFileName(d, "Swath file or datalist", m->host.startDir(), "All Files (*)");
			if (fn.isEmpty())
				return;
			m->host.rememberDir(fn);
			m->openFile->setText(QDir::toNativeSeparators(fn));
			guess();
		});
		QObject::connect(m->openFile, &QLineEdit::editingFinished, d, guess);
		QObject::connect(on, &QRadioButton::toggled, d, [m](bool v) {
			if (v)
				mnSetOutputMode(m, OUTPUT_MODE_OUTPUT);
		});
		QObject::connect(off, &QRadioButton::toggled, d, [m](bool v) {
			if (v)
				mnSetOutputMode(m, OUTPUT_MODE_BROWSE);
		});
		QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::hide);
		// do_fileselection_ok
		QObject::connect(ok, &QPushButton::clicked, d, [m, d]() {
			// a datalist is read with its own path as typed (MB's datalist reader takes Windows
			// entries as relative unless the list is named with backslashes)
			const QByteArray input_file = QDir::toNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (input_file.isEmpty()) {
				mnBell();
				return;
			}
			d->hide();
			mbnavedit_g.run_mbprocess = m->openMbprocess->isChecked();
			m->usePrevious = -1;
			mnOpenInput(m, input_file, m->textField_format->value());
		});
	}
	{
		QSignalBlocker b(m->textField_format);
		m->textField_format->setValue(mbnavedit_g.format);
	}
	mnSetOutputMode(m, mbnavedit_g.output_mode);
	m->openMbprocess->setChecked(mbnavedit_g.run_mbprocess);
	mnShow(m->openDlg);
}

// form_filelist
void mnFileListDialog(MbNavEdit *m) {
	const char *ui = "mbnavedit_filelist.ui";
	if (!m->fileListDlg) {
		QDialog *d = mnDialog(m, ui);
		if (!d)
			return;
		QStringList missing;
		auto *list = mnChild<QListWidget>(d, "list_filelist", missing);
		auto *on = mnChild<QRadioButton>(d, "toggleButton_output_on_filelist", missing);
		auto *off = mnChild<QRadioButton>(d, "toggleButton_output_off_filelist", missing);
		auto *edit = mnChild<QPushButton>(d, "pushButton_filelist_edit", missing);
		auto *remove = mnChild<QPushButton>(d, "pushButton_filelist_remove", missing);
		auto *dismiss = mnChild<QPushButton>(d, "pushButton_filelist_dismiss", missing);
		if (mnMissing(m, d, ui, missing))
			return;
		m->fileListDlg = d;
		m->list_filelist = list;
		m->toggleButton_output_on_filelist = on;
		m->toggleButton_output_off_filelist = off;
		auto *group = new QButtonGroup(d);
		group->addButton(on);
		group->addButton(off);
		QFont mono("Courier New");
		mono.setStyleHint(QFont::TypeWriter);
		list->setFont(mono);
		QObject::connect(on, &QRadioButton::toggled, d, [m](bool v) {
			if (v)
				mnSetOutputMode(m, OUTPUT_MODE_OUTPUT);
		});
		QObject::connect(off, &QRadioButton::toggled, d, [m](bool v) {
			if (v)
				mnSetOutputMode(m, OUTPUT_MODE_BROWSE);
		});
		// do_editlistselection
		auto editSelected = [m]() {
			/* turn off expose plots */
			m->expose_plot_ok = false;
			const int sel = m->list_filelist->currentRow();
			/* if the selected file is different than what's already loaded, unload the old file and load the new one */
			if (sel >= 0 && sel != m->currentfile) {
				m->currentfile = sel;
				mnCloseCurrent(m);
				m->usePrevious = -1;
				if (sel < int(m->files.size()))
					mnLoadSpecificFile(m, sel);
			}
			/* turn on expose plots */
			m->expose_plot_ok = true;
			/* update controls */
			do_set_controls();
		};
		QObject::connect(edit, &QPushButton::clicked, d, editSelected);
		QObject::connect(list, &QListWidget::itemDoubleClicked, d, editSelected);
		// do_filelist_remove
		QObject::connect(remove, &QPushButton::clicked, d, [m]() {
			const int sel = m->list_filelist->currentRow();
			/* if the selected file is different than what's already loaded, remove it from the list */
			if (sel >= 0 && sel != m->currentfile && sel < int(m->files.size())) {
				m->files.erase(m->files.begin() + sel);
				if (m->currentfile > sel)
					m->currentfile--;
			}
			/* update controls */
			do_set_controls();
			/* replot */
			mnPlot(m);
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	mnSetOutputMode(m, mbnavedit_g.output_mode);
	mnBuildFileList(m);
	mnShow(m->fileListDlg);
}

// do_timespan / do_timestep: a slider dragged to its end doubles its maximum, to 1 halves it
int mnDoubleOrHalve(int value, int maxx) {
	if (value == maxx || value == 1) {
		if (value == maxx)
			maxx = 2 * maxx;
		else if (value == 1)
			maxx = maxx / 2;
		if (maxx < 10)
			maxx = 10;
	}
	return maxx;
}

// bulletinBoard_timestepping: do_timespan, do_timestep
void mnTimeSteppingDialog(MbNavEdit *m) {
	const char *ui = "mbnavedit_timestepping.ui";
	if (!m->stepDlg) {
		QDialog *d = mnDialog(m, ui);
		if (!d)
			return;
		QStringList missing;
		MnScale span, step;
		mnBindScale(d, "scale_timespan", 0, span, missing);
		mnBindScale(d, "scale_timestep", 0, step, missing);
		auto *span2 = mnChild<QLabel>(d, "label_timespan_2", missing);
		auto *step2 = mnChild<QLabel>(d, "label_timestep_2", missing);
		auto *dismiss = mnChild<QPushButton>(d, "pushButton_timestepping_dismiss", missing);
		if (mnMissing(m, d, ui, missing))
			return;
		m->stepDlg = d;
		m->scale_timespan = span;
		m->scale_timestep = step;
		m->label_timespan_2 = span2;
		m->label_timestep_2 = step2;
		mnOnScale(m->scale_timespan, [m](int v) {
			/* get values */
			mbnavedit_g.data_show_max = m->scale_timespan.s->maximum();
			mbnavedit_g.data_show_size = v;

			/* reset maximum if necessary */
			const int maxx = mnDoubleOrHalve(mbnavedit_g.data_show_size, mbnavedit_g.data_show_max);
			if (maxx != mbnavedit_g.data_show_max) {
				mbnavedit_g.data_show_max = maxx;
				mnSetTimeSpanSlider(m);
			}

			/* replot */
			mnPlot(m);
		});
		mnOnScale(m->scale_timestep, [m](int v) {
			/* get values */
			mbnavedit_g.data_step_max = m->scale_timestep.s->maximum();
			mbnavedit_g.data_step_size = v;

			/* reset maximum if necessary */
			const int maxx = mnDoubleOrHalve(mbnavedit_g.data_step_size, mbnavedit_g.data_step_max);
			if (maxx != mbnavedit_g.data_step_max) {
				mbnavedit_g.data_step_max = maxx;
				mnSetTimeStepSlider(m);
			}
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	mnSetTimeSpanSlider(m);
	mnSetTimeStepSlider(m);
	mnShow(m->stepDlg);
}

// bulletinBoard_modeling: do_model_mode, do_meantimewindow, do_driftlon, do_driftlat,
// do_modeling_apply
void mnModelingDialog(MbNavEdit *m) {
	const char *ui = "mbnavedit_modeling.ui";
	if (!m->modelDlg) {
		QDialog *d = mnDialog(m, ui);
		if (!d)
			return;
		QStringList missing;
		QRadioButton *b[4];
		b[0] = mnChild<QRadioButton>(d, "toggleButton_modeling_off", missing);
		b[1] = mnChild<QRadioButton>(d, "toggleButton_modeling_meanfilter", missing);
		b[2] = mnChild<QRadioButton>(d, "toggleButton_modeling_dr", missing);
		b[3] = mnChild<QRadioButton>(d, "toggleButton_modeling_inversion", missing);
		MnScale mean, dlon, dlat;
		mnBindScale(d, "scale_meantimewindow", 1, mean, missing);
		mnBindScale(d, "scale_driftlon", 5, dlon, missing);
		mnBindScale(d, "scale_driftlat", 5, dlat, missing);
		auto *speed = mnChild<QLineEdit>(d, "textField_modeling_speed", missing);
		auto *accel = mnChild<QLineEdit>(d, "textField_modeling_acceleration", missing);
		auto *apply = mnChild<QPushButton>(d, "pushButton_modeling_apply", missing);
		auto *dismiss = mnChild<QPushButton>(d, "pushButton_modeling_dismiss", missing);
		if (mnMissing(m, d, ui, missing))
			return;
		m->modelDlg = d;
		m->scale_meantimewindow = mean;
		m->scale_driftlon = dlon;
		m->scale_driftlat = dlat;
		m->scale_meantimewindow.set(1, 10000, mbnavedit_g.mean_time_window);
		m->scale_driftlon.set(-1000, 1000, mbnavedit_g.drift_lon);
		m->scale_driftlat.set(-1000, 1000, mbnavedit_g.drift_lat);
		m->textField_modeling_speed = speed;
		m->textField_modeling_acceleration = accel;
		auto *group = new QButtonGroup(d);
		for (int i = 0; i < 4; i++) {
			m->modelButton[i] = b[i];
			group->addButton(b[i], i);
			// do_model_mode
			QObject::connect(b[i], &QRadioButton::toggled, d, [m, i](bool on) {
				if (!on)
					return;
				mbnavedit_g.model_mode = i;   // MODEL_MODE_OFF .. MODEL_MODE_INVERT, in the radio box's order
				if (mbnavedit_g.model_mode != MODEL_MODE_OFF) {
					mbnavedit_g.plot_lon_dr = true;
					mbnavedit_g.plot_lat_dr = true;
				}

				do_set_controls();

				/* recalculate model */
				mbnavedit_get_model();

				/* replot */
				mnPlot(m);
			});
		}
		mnOnScale(m->scale_meantimewindow, [m](int v) {
			mbnavedit_g.mean_time_window = v;
			/* recalculate model */
			mbnavedit_get_model();
			/* replot */
			mnPlot(m);
		});
		mnOnScale(m->scale_driftlon, [m](int v) {
			mbnavedit_g.drift_lon = v;
			mbnavedit_get_model();
			mnPlot(m);
		});
		mnOnScale(m->scale_driftlat, [m](int v) {
			mbnavedit_g.drift_lat = v;
			mbnavedit_get_model();
			mnPlot(m);
		});
		// do_modeling_apply
		QObject::connect(apply, &QPushButton::clicked, d, [m]() {
			double dvalue;
			if (std::sscanf(m->textField_modeling_speed->text().toUtf8().constData(), "%lf", &dvalue) == 1)
				mbnavedit_g.weight_speed = dvalue;
			if (std::sscanf(m->textField_modeling_acceleration->text().toUtf8().constData(), "%lf", &dvalue) == 1)
				mbnavedit_g.weight_acceleration = dvalue;

			do_set_controls();

			/* recalculate model */
			mbnavedit_get_model();

			/* replot */
			mnPlot(m);
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	do_set_controls();
	mnShow(m->modelDlg);
}

// bulletinBoard_timeinterpolation (do_timeinterpolation_apply) and bulletinBoard_deletebadtimetag
// (do_deletebadtimetag_apply): the same small Apply / Dismiss board, each with its action
void mnApplyDialog(MbNavEdit *m, QDialog *&slot, const char *ui, const char *applyName, const char *dismissName,
                   int (*action)(void)) {
	if (!slot) {
		QDialog *d = mnDialog(m, ui);
		if (!d)
			return;
		QStringList missing;
		auto *apply = mnChild<QPushButton>(d, applyName, missing);
		auto *dismiss = mnChild<QPushButton>(d, dismissName, missing);
		if (mnMissing(m, d, ui, missing))
			return;
		slot = d;
		QObject::connect(apply, &QPushButton::clicked, d, [m, action]() {
			/* interpolate time stamps */
			action();

			/* reset timestamp problem flag */
			mbnavedit_g.timestamp_problem = false;

			/* replot */
			mnPlot(m);

			/* update controls */
			do_set_controls();
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	mnShow(slot);
}

// form_offset: do_offset_apply
void mnOffsetDialog(MbNavEdit *m) {
	const char *ui = "mbnavedit_offset.ui";
	if (!m->offsetDlg) {
		QDialog *d = mnDialog(m, ui);
		if (!d)
			return;
		QStringList missing;
		auto *lon = mnChild<QLineEdit>(d, "textField_lon_offset", missing);
		auto *lat = mnChild<QLineEdit>(d, "textField_lat_offset", missing);
		auto *apply = mnChild<QPushButton>(d, "pushButton_offset_apply", missing);
		auto *dismiss = mnChild<QPushButton>(d, "pushButton_offset_dismiss", missing);
		if (mnMissing(m, d, ui, missing))
			return;
		m->offsetDlg = d;
		m->textField_lon_offset = lon;
		m->textField_lat_offset = lat;
		QObject::connect(apply, &QPushButton::clicked, d, [m]() {
			/* get values from widgets */
			double dvalue;
			if (std::sscanf(m->textField_lon_offset->text().toUtf8().constData(), "%lf", &dvalue) == 1)
				mbnavedit_g.offset_lon = dvalue;
			if (std::sscanf(m->textField_lat_offset->text().toUtf8().constData(), "%lf", &dvalue) == 1)
				mbnavedit_g.offset_lat = dvalue;

			/* reset widgets so user sees what got applied */
			m->textField_lon_offset->setText(QString::number(mbnavedit_g.offset_lon, 'f', 5));
			m->textField_lat_offset->setText(QString::number(mbnavedit_g.offset_lat, 'f', 5));

			/* apply offsets */
			mbnavedit_action_offset();

			/* replot */
			mnPlot(m);
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
	}
	do_set_controls();
	mnShow(m->offsetDlg);
}

void mnAbout(MbNavEdit *m) {
	QMessageBox::about(m->win, "About MBnavedit",
		QString("<b>MBnavedit</b><br>Interactive Navigation Editor<br><br>"
		        "One Component of the <b>MB-System</b> Open Source Software Package<br>"
		        "for Processing and Display of Swath Sonar Data<br><br>"
		        "Created by: David W. Caress and Dale N. Chayes<br>"
		        "Monterey Bay Aquarium Research Institute &nbsp; / &nbsp; Lamont-Doherty Earth Observatory<br><br>"
		        "MB-System library in use: %1<br><br>"
		        "Ported to InteractiveGMT (Qt) from MB-System's mbnavedit.")
		    .arg(QString::fromLatin1(mbedit_mbio_version())));
}

// Help > How to use MBnavedit
void mnHelp(MbNavEdit *m) {
	static QPointer<QDialog> dlg;
	if (!dlg) {
		dlg = new QDialog(m->win);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowTitle("How to use MBnavedit");
		dlg->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		auto *lay = new QVBoxLayout(dlg);
		lay->setContentsMargins(4, 4, 4, 4);
		auto *tb = new QTextBrowser(dlg);
		tb->setOpenExternalLinks(false);
		tb->setHtml(
			"<h3>MBnavedit &mdash; interactive navigation editor</h3>"
			"<p>MBnavedit shows the navigation of a swath file as time series &mdash; the time between fixes, "
			"longitude, latitude, speed, heading and sonar depth &mdash; and lets you repair it: bad fixes are "
			"selected and replaced by interpolation, by speed/course made good or by a navigation model. "
			"The edited navigation is written to <code>&lt;file&gt;.nve</code> and set in the file's "
			"mbprocess parameter file, so <b>mbprocess</b> applies it when it makes the processed file.</p>"

			"<h4>1. Open the data</h4>"
			"<p><b>File &gt; Open</b> takes a swath file (its MB-System format id is guessed from the name) "
			"or a datalist (format id <b>-1</b>), whose files all join the <b>File Selection List</b>. "
			"<i>Output Edited Data</i> writes the .nve; <i>Browse Only</i> writes nothing. "
			"<i>Run mbprocess when done</i> applies the edits as soon as a file is finished. "
			"If a file already has a .nve you are asked whether to start from it.</p>"

			"<h4>2. Look at it</h4>"
			"<p>The check boxes on the left choose the plots; under each, <i>Show Original Data</i> draws the "
			"values as read (before your edits), and the speed and heading plots can show the speed and course "
			"made good between the fixes. <b>Start / Reverse / Forward / End</b> move through the buffer by the "
			"time step, and the middle / right mouse buttons step back / forward too. "
			"<b>Controls &gt; Time Stepping</b> sets how many seconds are shown and the step; a slider dragged to "
			"its end doubles its range. <b>Pick Zoom</b> zooms in time: left button marks one bound, middle "
			"button the other, right button applies. <b>Show All</b> shows the whole buffer again.</p>"

			"<h4>3. Select</h4>"
			"<p>The left button acts in the plot it is pressed in, as the mode chosen beside the edit buttons:</p>"
			"<ul><li><b>Pick</b> (key Y or Q) toggles the value nearest the pointer;</li>"
			"<li><b>Select</b> (U, W) and <b>Deselect</b> (I, E) work on every value the pointer passes over "
			"while the button is held;</li>"
			"<li><b>Select All</b> (O, R) and <b>Deselect All</b> (P, T) work on every value shown.</li></ul>"
			"<p>Selected values are drawn in red. Selecting in one plot deselects the others.</p>"

			"<h4>4. Repair</h4>"
			"<p><b>Interpolate</b> replaces the selected values by values interpolated between their unselected "
			"neighbours; <b>Interpolate Repeats</b> does it only where a value repeats the one before it. "
			"<b>Revert</b> puts the original values back. <i>Use Speed-Made-Good</i> and <i>Use Course-Made-Good</i> "
			"replace the selected speeds or headings by those computed from the fixes.</p>"
			"<p><b>Controls &gt; Nav Modeling</b> computes a model shown under the longitude and latitude plots: "
			"a <i>Gaussian Mean</i> of the fixes, <i>Dead Reckoning</i> from speed and heading (with optional "
			"drifts), or a smooth <i>Inversion</i> penalising speed and acceleration. <b>Use Solution</b> replaces "
			"the selected positions by the model; <b>Flag</b> / <b>Unflag</b> keep fixes out of the inversion.</p>"
			"<p>Duplicate or reversed time stamps are reported when a file is read; <b>Controls &gt; Time "
			"Interpolation</b> spreads repeated times evenly, <b>Delete Bad Times</b> removes them. "
			"<b>Controls &gt; Position Offset</b> shifts every position by a constant.</p>"

			"<h4>5. Finish</h4>"
			"<p><b>Next Buffer</b> writes the records shown and reads on (very long files are edited a buffer at "
			"a time). <b>Done</b> finishes the file &mdash; everything is written &mdash; and opens the next file "
			"of the list (it then reads <i>Next File</i>). <b>Quit</b> finishes the file and closes the editor. "
			"Closing or minimising the window only parks it in Scene Objects; its handle brings it back.</p>");
		lay->addWidget(tb);
		dlg->resize(560, 600);
	}
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

// ---- the window ----------------------------------------------------------------------------
class MnWindowFilter : public QObject {
public:
	explicit MnWindowFilter(QObject *parent) : QObject(parent) {}

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		MbNavEdit *m = g_mn;
		if (m && o == m->win) {
			// do_quit / BxExitCB: finish with the current file; the tool's memory goes with the window
			if (e->type() == QEvent::Close) {
				m->expose_plot_ok = false;
				const int status = mbnavedit_action_quit();
				if (status == 0)
					mnBell();
				mbnavedit_release();
			}
			// do_resize: the plot area follows the window
			else if (e->type() == QEvent::Resize) {
				mnLayout(m);
			}
		}
		return QObject::eventFilter(o, e);
	}

public:
	static void mnLayout(MbNavEdit *m) {
		if (!m->scroll || !m->central)
			return;
		const QRect r = m->scroll->geometry();
		const int w = m->central->width() - r.x() - 6;
		const int h = m->central->height() - r.y() - 4;
		m->scroll->setGeometry(r.x(), r.y(), w > 100 ? w : 100, h > 100 ? h : 100);
		if (m->expose_plot_ok)
			mnPlot(m);
		else
			mnSizeCanvas(m);
	}
};

MbNavEdit *mnBuild(QWidget *parent, const MbEditHost &host) {
	auto *m = new MbNavEdit;
	m->host = host;
	QWidget *root = mnLoadUi(m, "mbnavedit.ui", nullptr);
	auto *win = qobject_cast<QMainWindow *>(root);
	if (!win) {
		delete root;
		delete m;
		return nullptr;
	}
	QStringList missing;
	m->central = mnChild<QWidget>(win, "centralwidget", missing);
	m->scroll = mnChild<QScrollArea>(win, "scrollArea", missing);
	m->pushButton_start = mnChild<QPushButton>(win, "pushButton_start", missing);
	m->pushButton_reverse = mnChild<QPushButton>(win, "pushButton_reverse", missing);
	m->pushButton_forward = mnChild<QPushButton>(win, "pushButton_forward", missing);
	m->pushButton_end = mnChild<QPushButton>(win, "pushButton_end", missing);
	auto *pushButton_nextbuffer = mnChild<QPushButton>(win, "pushButton_nextbuffer", missing);
	auto *pushButton_showall = mnChild<QPushButton>(win, "pushButton_showall", missing);
	auto *pushButton_set_interval = mnChild<QPushButton>(win, "pushButton_set_interval", missing);
	m->pushButton_done = mnChild<QPushButton>(win, "pushButton_done", missing);
	auto *pushButton_quit = mnChild<QPushButton>(win, "pushButton_quit", missing);
	auto *pushButton_about = mnChild<QPushButton>(win, "pushButton_about", missing);
	auto *pushButton_interpolate = mnChild<QPushButton>(win, "pushButton_interpolate", missing);
	auto *pushButton_interpolaterepeats = mnChild<QPushButton>(win, "pushButton_interpolaterepeats", missing);
	auto *pushButton_revert = mnChild<QPushButton>(win, "pushButton_revert", missing);
	m->pushButton_solution = mnChild<QPushButton>(win, "pushButton_solution", missing);
	m->pushButton_flag = mnChild<QPushButton>(win, "pushButton_flag", missing);
	m->pushButton_unflag = mnChild<QPushButton>(win, "pushButton_unflag", missing);
	m->pushButton_speed_smg = mnChild<QPushButton>(win, "pushButton_speed_smg", missing);
	m->pushButton_heading_cmg = mnChild<QPushButton>(win, "pushButton_heading_cmg", missing);
	const char *modeNames[5] = {"toggleButton_pick", "toggleButton_select", "toggleButton_deselect",
	                            "toggleButton_selectall", "toggleButton_deselectall"};
	for (int i = 0; i < 5; i++)
		m->modeButton[i] = mnChild<QRadioButton>(win, modeNames[i], missing);
	m->toggleButton_time = mnChild<QCheckBox>(win, "toggleButton_time", missing);
	m->toggleButton_org_time = mnChild<QCheckBox>(win, "toggleButton_org_time", missing);
	m->toggleButton_lon = mnChild<QCheckBox>(win, "toggleButton_lon", missing);
	m->toggleButton_org_lon = mnChild<QCheckBox>(win, "toggleButton_org_lon", missing);
	m->toggleButton_dr_lon = mnChild<QCheckBox>(win, "toggleButton_dr_lon", missing);
	m->toggleButton_lat = mnChild<QCheckBox>(win, "toggleButton_lat", missing);
	m->toggleButton_org_lat = mnChild<QCheckBox>(win, "toggleButton_org_lat", missing);
	m->toggleButton_dr_lat = mnChild<QCheckBox>(win, "toggleButton_dr_lat", missing);
	m->toggleButton_speed = mnChild<QCheckBox>(win, "toggleButton_speed", missing);
	m->toggleButton_org_speed = mnChild<QCheckBox>(win, "toggleButton_org_speed", missing);
	m->toggleButton_show_smg = mnChild<QCheckBox>(win, "toggleButton_show_smg", missing);
	m->toggleButton_heading = mnChild<QCheckBox>(win, "toggleButton_heading", missing);
	m->toggleButton_org_heading = mnChild<QCheckBox>(win, "toggleButton_org_heading", missing);
	m->toggleButton_show_cmg = mnChild<QCheckBox>(win, "toggleButton_show_cmg", missing);
	m->toggleButton_sensordepth = mnChild<QCheckBox>(win, "toggleButton_sensordepth", missing);
	m->toggleButton_org_sensordepth = mnChild<QCheckBox>(win, "toggleButton_org_sensordepth", missing);
	m->toggleButton_vru = mnChild<QCheckBox>(win, "toggleButton_vru", missing);
	m->pushButton_file = mnChild<QAction>(win, "pushButton_file", missing);
	auto *pushButton_filelist = mnChild<QAction>(win, "pushButton_filelist", missing);
	auto *actionQuit = mnChild<QAction>(win, "actionQuit", missing);
	auto *pushButton_controls_timespan = mnChild<QAction>(win, "pushButton_controls_timespan", missing);
	auto *pushButton_controls_modeling = mnChild<QAction>(win, "pushButton_controls_modeling", missing);
	m->pushButton_controls_timeinterpolation = mnChild<QAction>(win, "pushButton_controls_timeinterpolation", missing);
	m->pushButton_controls_deletebadtimetag = mnChild<QAction>(win, "pushButton_controls_deletebadtimetag", missing);
	auto *pushButton_controls_offset = mnChild<QAction>(win, "pushButton_controls_offset", missing);
	auto *actionHelp = mnChild<QAction>(win, "actionHelp", missing);
	auto *actionAbout = mnChild<QAction>(win, "actionAbout", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBnavedit", "mbnavedit.ui lacks: " + missing.join(", "));
		delete win;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.icon.isNull())
		win->setWindowIcon(host.icon);

	// the drawing canvas, in the scrolled window
	m->canvas = new MnCanvas(m->scroll);
	m->scroll->setWidget(m->canvas);
	m->scroll->setBackgroundRole(QPalette::Base);
	g_xg.widget = m->canvas;
	// mbnavedit's font: -*-fixed-bold-r-normal-*-13-*-75-75-c-70-iso8859-1
	g_xg.font = QFont("Courier New");
	g_xg.font.setStyleHint(QFont::TypeWriter);
	g_xg.font.setBold(true);
	g_xg.font.setPixelSize(13);
	g_xg.font.setStyleStrategy(QFont::NoAntialias);

	// the pick mode radio box: one group (siblings of a QUiLoader form are not reliably exclusive)
	auto *modeGroup = new QButtonGroup(win);
	for (int i = 0; i < 5; i++) {
		modeGroup->addButton(m->modeButton[i], i);
		QObject::connect(m->modeButton[i], &QRadioButton::toggled, win, [m, i](bool on) {
			if (on)
				mnSetMode(m, i);   // PICK_MODE_PICK .. PICK_MODE_DESELECTALL, in the radio box's order
		});
	}

	// the navigation buttons (do_start, do_reverse, do_forward, do_end, do_nextbuffer, do_showall)
	QObject::connect(m->pushButton_start, &QPushButton::clicked, win, [m]() {
		const int status = mbnavedit_action_start();
		if (status == 0)
			mnBell();
		mnUnsetInterval(m);
		xgFlush();
	});
	QObject::connect(m->pushButton_reverse, &QPushButton::clicked, win, [m]() {
		const int status = mbnavedit_action_step(-mbnavedit_g.data_step_size);
		if (status == 0)
			mnBell();
		mnUnsetInterval(m);
		xgFlush();
	});
	QObject::connect(m->pushButton_forward, &QPushButton::clicked, win, [m]() {
		const int status = mbnavedit_action_step(mbnavedit_g.data_step_size);
		if (status == 0)
			mnBell();
		mnUnsetInterval(m);
		xgFlush();
	});
	QObject::connect(m->pushButton_end, &QPushButton::clicked, win, [m]() {
		const int status = mbnavedit_action_end();
		if (status == 0)
			mnBell();
		mnUnsetInterval(m);
		xgFlush();
	});
	QObject::connect(pushButton_nextbuffer, &QPushButton::clicked, win, [m]() {
		/* turn off expose plots */
		m->expose_plot_ok = false;
		/* get next buffer */
		int quit = 0;
		mnSizeCanvas(m);
		const int status = mbnavedit_action_next_buffer(&quit);
		if (status == 0)
			mnBell();
		mnUnsetInterval(m);
		/* turn on expose plots */
		m->expose_plot_ok = true;
		xgFlush();
		/* (quit only in GUI mode, which this port has not: the editor stays open) */
	});
	QObject::connect(pushButton_showall, &QPushButton::clicked, win, [m]() {
		/* Show entire data buffer */
		mbnavedit_action_showall();
		mnUnsetInterval(m);
		mnSetTimeSpanSlider(m);
		xgFlush();
	});
	// do_set_interval
	QObject::connect(pushButton_set_interval, &QPushButton::clicked, win, [m]() {
		/* turn on set interval mode */
		mbnavedit_g.mode_set_interval = true;
		mnCursor(m);
	});
	// do_done
	QObject::connect(m->pushButton_done, &QPushButton::clicked, win, [m]() {
		/* turn off expose plots */
		m->expose_plot_ok = false;
		/* finish with the current file */
		mnCloseCurrent(m);
		mnUnsetInterval(m);
		/* if there is another file in the list open it */
		if (m->currentfile >= 0 && m->currentfile < int(m->files.size()) - 1) {
			mnLoadSpecificFile(m, m->currentfile + 1);
		}
		/* else do not open a file */
		else {
			m->currentfile = -1;
		}
		/* turn on expose plots */
		m->expose_plot_ok = true;
		do_set_controls();
	});
	QObject::connect(pushButton_quit, &QPushButton::clicked, win, [m]() { mbParkQuit(m->parking); });
	QObject::connect(actionQuit, &QAction::triggered, win, [m]() { mbParkQuit(m->parking); });
	QObject::connect(pushButton_about, &QPushButton::clicked, win, [m]() { mnAbout(m); });
	QObject::connect(actionAbout, &QAction::triggered, win, [m]() { mnAbout(m); });
	QObject::connect(actionHelp, &QAction::triggered, win, [m]() { mnHelp(m); });

	// the edit buttons (do_interpolation, do_interpolationrepeats, do_revert, do_button_use_dr,
	// do_flag, do_unflag, do_button_use_smg, do_button_use_cmg)
	QObject::connect(pushButton_interpolate, &QPushButton::clicked, win, [m]() {
		/* Interpolate any current selected data */
		mbnavedit_action_interpolate();
		mnUnsetInterval(m);
		/* replot */
		mnPlot(m);
	});
	QObject::connect(pushButton_interpolaterepeats, &QPushButton::clicked, win, [m]() {
		mbnavedit_action_interpolaterepeats();
		mnUnsetInterval(m);
		mnPlot(m);
	});
	QObject::connect(pushButton_revert, &QPushButton::clicked, win, [m]() {
		/* Revert to original values for all selected data */
		mbnavedit_action_revert();
		mnUnsetInterval(m);
		mnPlot(m);
	});
	QObject::connect(m->pushButton_solution, &QPushButton::clicked, win, [m]() {
		/* Use dr for selected lonlat values */
		mbnavedit_action_use_dr();
		mnPlot(m);
	});
	QObject::connect(m->pushButton_flag, &QPushButton::clicked, win, [m]() {
		mbnavedit_action_flag();
		mnPlot(m);
		do_set_controls();
	});
	QObject::connect(m->pushButton_unflag, &QPushButton::clicked, win, [m]() {
		mbnavedit_action_unflag();
		mnPlot(m);
		do_set_controls();
	});
	QObject::connect(m->pushButton_speed_smg, &QPushButton::clicked, win, [m]() {
		/* Use speed made good for selected speed values */
		mbnavedit_action_use_smg();
		mnPlot(m);
	});
	QObject::connect(m->pushButton_heading_cmg, &QPushButton::clicked, win, [m]() {
		/* Use course made good for selected heading values */
		mbnavedit_action_use_cmg();
		mnPlot(m);
	});

	// the plot toggles (do_toggle_time, _lon, _lat, _speed, _heading, _sensordepth, _vru): a plot
	// turned off loses its selection, and the canvas is sized for the plots left
	struct PlotToggle {
		QCheckBox *box;
		int *flag;
		int type;   // its selection is cleared when it goes; -1: none (the vru plots)
	};
	const PlotToggle plots[] = {
		{m->toggleButton_time, &mbnavedit_g.plot_tint, PLOT_TINT},
		{m->toggleButton_lon, &mbnavedit_g.plot_lon, PLOT_LONGITUDE},
		{m->toggleButton_lat, &mbnavedit_g.plot_lat, PLOT_LATITUDE},
		{m->toggleButton_speed, &mbnavedit_g.plot_speed, PLOT_SPEED},
		{m->toggleButton_heading, &mbnavedit_g.plot_heading, PLOT_HEADING},
		{m->toggleButton_sensordepth, &mbnavedit_g.plot_draft, PLOT_DRAFT},
	};
	for (const PlotToggle &pt : plots) {
		QObject::connect(pt.box, &QCheckBox::toggled, win, [m, pt](bool on) {
			*pt.flag = on;
			if (!on)
				mbnavedit_action_deselect_all(pt.type);
			do_set_controls();   // hide or display the plot's own items
			/* get and set size of canvas, replot */
			mnCountPlots();
			mnPlot(m);
		});
	}
	QObject::connect(m->toggleButton_vru, &QCheckBox::toggled, win, [m](bool on) {
		mbnavedit_g.plot_roll = on;
		mbnavedit_g.plot_pitch = on;
		mbnavedit_g.plot_heave = on;
		mnCountPlots();
		mnPlot(m);
	});
	// the "show" toggles (do_toggle_org_*, do_toggle_dr_*, do_toggle_show_smg / _cmg): replot
	struct ShowToggle {
		QCheckBox *box;
		int *flag;
	};
	const ShowToggle shows[] = {
		{m->toggleButton_org_time, &mbnavedit_g.plot_tint_org},
		{m->toggleButton_org_lon, &mbnavedit_g.plot_lon_org},
		{m->toggleButton_dr_lon, &mbnavedit_g.plot_lon_dr},
		{m->toggleButton_org_lat, &mbnavedit_g.plot_lat_org},
		{m->toggleButton_dr_lat, &mbnavedit_g.plot_lat_dr},
		{m->toggleButton_org_speed, &mbnavedit_g.plot_speed_org},
		{m->toggleButton_show_smg, &mbnavedit_g.plot_smg},
		{m->toggleButton_org_heading, &mbnavedit_g.plot_heading_org},
		{m->toggleButton_show_cmg, &mbnavedit_g.plot_cmg},
		{m->toggleButton_org_sensordepth, &mbnavedit_g.plot_draft_org},
	};
	for (const ShowToggle &st : shows) {
		QObject::connect(st.box, &QCheckBox::toggled, win, [m, st](bool on) {
			*st.flag = on;
			mnPlot(m);
		});
	}

	// File and Controls menus
	QObject::connect(m->pushButton_file, &QAction::triggered, win, [m]() { mnOpenDialog(m); });
	QObject::connect(pushButton_filelist, &QAction::triggered, win, [m]() { mnFileListDialog(m); });
	QObject::connect(pushButton_controls_timespan, &QAction::triggered, win, [m]() { mnTimeSteppingDialog(m); });
	QObject::connect(pushButton_controls_modeling, &QAction::triggered, win, [m]() { mnModelingDialog(m); });
	QObject::connect(m->pushButton_controls_timeinterpolation, &QAction::triggered, win, [m]() {
		mnApplyDialog(m, m->timeInterpDlg, "mbnavedit_timeinterpolation.ui", "pushButton_timeinterpolation_apply",
		              "pushButton_timeinterpolation_dismiss", mbnavedit_action_fixtime);
	});
	QObject::connect(m->pushButton_controls_deletebadtimetag, &QAction::triggered, win, [m]() {
		mnApplyDialog(m, m->deleteBadDlg, "mbnavedit_deletebadtimetag.ui", "pushButton_deletebadtimetag_apply",
		              "pushButton_deletebadtimetag_dismiss", mbnavedit_action_deletebadtime);
	});
	QObject::connect(pushButton_controls_offset, &QAction::triggered, win, [m]() { mnOffsetDialog(m); });

	win->installEventFilter(new MnWindowFilter(win));
	m->parking = mbParkable(win, host, "mbnavedit");   // after the close filter: a park comes first
	return m;
}

} // namespace

// ---- the engine's host hooks (declared in mbnavedit.h) ---------------------------------------
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
	if (g_mn && g_mn->host.busyText)
		g_mn->host.busyText(message);
	return MB_SUCCESS;
}

int do_message_off(void) {
	if (g_mn && g_mn->host.busyOff)
		g_mn->host.busyOff();
	return MB_SUCCESS;
}

int do_error_dialog(char *s1, char *s2, char *s3) {
	mnBell();
	QMessageBox::warning(g_mn ? g_mn->win : nullptr, "MBnavedit",
	                     QString::fromUtf8(s1) + "\n" + QString::fromUtf8(s2) + "\n" + QString::fromUtf8(s3));
	return MB_SUCCESS;
}

// do_filebutton_on: no file loaded
void do_filebutton_on(void) {
	MbNavEdit *m = g_mn;
	if (!m)
		return;
	m->pushButton_file->setEnabled(true);
	m->pushButton_done->setEnabled(false);
	m->pushButton_done->setText("Done");
	m->pushButton_forward->setEnabled(false);
	m->pushButton_reverse->setEnabled(false);
	m->pushButton_start->setEnabled(false);
	m->pushButton_end->setEnabled(false);
}

// do_filebutton_off: a file loaded
void do_filebutton_off(void) {
	MbNavEdit *m = g_mn;
	if (!m)
		return;
	m->pushButton_file->setEnabled(true);
	m->pushButton_done->setEnabled(true);
	const int numfiles = int(m->files.size());
	m->pushButton_done->setText((numfiles > 0 && m->currentfile >= 0 && m->currentfile < numfiles - 1) ? "Next File"
	                                                                                                   : "Done");
	m->pushButton_forward->setEnabled(true);
	m->pushButton_reverse->setEnabled(true);
	m->pushButton_start->setEnabled(true);
	m->pushButton_end->setEnabled(true);
}

// do_set_controls
void do_set_controls(void) {
	MbNavEdit *m = g_mn;
	if (!m)
		return;
	const struct mbnavedit_globals &g = mbnavedit_g;

	/* set value of format text item */
	if (m->textField_format) {
		QSignalBlocker b(m->textField_format);
		m->textField_format->setValue(g.format);
	}

	/* set the output mode */
	mnSetOutputMode(m, g.output_mode);

	/* set values of number of data shown slider, and of number of data to step slider */
	mnSetTimeSpanSlider(m);
	mnSetTimeStepSlider(m);

	/* set the pick mode */
	if (g.mode_pick >= 0 && g.mode_pick < 5 && m->modeButton[g.mode_pick]) {
		QSignalBlocker b(m->modeButton[g.mode_pick]);
		m->modeButton[g.mode_pick]->setChecked(true);
	}

	/* set the lon, lat, speed and heading plot toggles */
	struct {
		QCheckBox *box;
		int on;
	} toggles[] = {
		{m->toggleButton_time, g.plot_tint},         {m->toggleButton_org_time, g.plot_tint_org},
		{m->toggleButton_lon, g.plot_lon},           {m->toggleButton_org_lon, g.plot_lon_org},
		{m->toggleButton_dr_lon, g.plot_lon_dr},     {m->toggleButton_lat, g.plot_lat},
		{m->toggleButton_org_lat, g.plot_lat_org},   {m->toggleButton_dr_lat, g.plot_lat_dr},
		{m->toggleButton_speed, g.plot_speed},       {m->toggleButton_org_speed, g.plot_speed_org},
		{m->toggleButton_show_smg, g.plot_smg},      {m->toggleButton_heading, g.plot_heading},
		{m->toggleButton_org_heading, g.plot_heading_org}, {m->toggleButton_show_cmg, g.plot_cmg},
		{m->toggleButton_sensordepth, g.plot_draft}, {m->toggleButton_org_sensordepth, g.plot_draft_org},
		{m->toggleButton_vru, g.plot_roll},
	};
	for (auto &t : toggles) {
		QSignalBlocker b(t.box);
		t.box->setChecked(t.on != 0);
	}

	/* hide or display items according to toggle states */
	mnSetVisible(m->toggleButton_org_time, g.plot_tint);
	const char *drLabel = g.model_mode == MODEL_MODE_MEAN ? "Show Gaussian Mean"
	                      : g.model_mode == MODEL_MODE_DR ? "Show Dead Reckoning"
	                                                      : "Show Smooth Inversion";
	mnSetVisible(m->toggleButton_org_lon, g.plot_lon);
	m->toggleButton_dr_lon->setText(drLabel);
	mnSetVisible(m->toggleButton_dr_lon, g.plot_lon && g.model_mode != MODEL_MODE_OFF);
	mnSetVisible(m->toggleButton_org_lat, g.plot_lat);
	m->toggleButton_dr_lat->setText(drLabel);
	mnSetVisible(m->toggleButton_dr_lat, g.plot_lat && g.model_mode != MODEL_MODE_OFF);
	mnSetVisible(m->toggleButton_org_speed, g.plot_speed);
	mnSetVisible(m->toggleButton_show_smg, g.plot_speed);
	mnSetVisible(m->pushButton_speed_smg, g.plot_speed);
	mnSetVisible(m->toggleButton_org_heading, g.plot_heading);
	mnSetVisible(m->toggleButton_show_cmg, g.plot_heading);
	mnSetVisible(m->pushButton_heading_cmg, g.plot_heading);
	mnSetVisible(m->toggleButton_org_sensordepth, g.plot_draft);

	/* get and set size of canvas */
	mnCountPlots();
	mnSizeCanvas(m);

	/* set modeling controls and hide or display buttons */
	if (g.model_mode >= 0 && g.model_mode < 4 && m->modelButton[g.model_mode]) {
		QSignalBlocker b(m->modelButton[g.model_mode]);
		m->modelButton[g.model_mode]->setChecked(true);
	}
	mnSetVisible(m->pushButton_solution, g.model_mode != MODEL_MODE_OFF);
	mnSetVisible(m->pushButton_flag, g.model_mode == MODEL_MODE_MEAN || g.model_mode == MODEL_MODE_INVERT);
	mnSetVisible(m->pushButton_unflag, g.model_mode == MODEL_MODE_MEAN || g.model_mode == MODEL_MODE_INVERT);
	m->scale_meantimewindow.setValue(g.mean_time_window);
	m->scale_driftlon.setValue(g.drift_lon);
	m->scale_driftlat.setValue(g.drift_lat);
	if (m->textField_modeling_speed)
		m->textField_modeling_speed->setText(QString::number(g.weight_speed, 'f', 2));
	if (m->textField_modeling_acceleration)
		m->textField_modeling_acceleration->setText(QString::number(g.weight_acceleration, 'f', 2));

	/* enable or disable time interpolation */
	m->pushButton_controls_timeinterpolation->setEnabled(g.timestamp_problem);
	m->pushButton_controls_deletebadtimetag->setEnabled(g.timestamp_problem);
	if (!g.timestamp_problem) {
		if (m->deleteBadDlg)
			m->deleteBadDlg->hide();
		if (m->timeInterpDlg)
			m->timeInterpDlg->hide();
	}

	/* set offset values */
	if (m->textField_lon_offset)
		m->textField_lon_offset->setText(QString::number(g.offset_lon, 'f', 5));
	if (m->textField_lat_offset)
		m->textField_lat_offset->setText(QString::number(g.offset_lat, 'f', 5));

	/* the file list */
	mnBuildFileList(m);
}

} // extern "C"


// ---- entry point ---------------------------------------------------------------------------
bool mbnaveditOpenWindow(QWidget *parent, const MbEditHost &host, const QStringList &files,
                         const std::vector<int> &formats, int browse, int runMbprocess, int usePingData, int stripComments,
                         int usePrevious) {
	if (!g_mn) {
		if (!mbeditLoadMbio(parent, host))
			return false;
		char msg[2048] = "";
		if (!mbnavedit_mbio_open(msg, int(sizeof(msg)))) {
			QMessageBox::warning(parent, "MBnavedit", QString::fromUtf8(msg));
			return false;
		}
		MbNavEdit *m = mnBuild(parent, host);
		if (!m)
			return false;
		g_mn = m;

		// do_mbnavedit_init
		m->expose_plot_ok = false;
		/* initialize graphics */
		mbnavedit_init_globals();
		static unsigned int mpixel_values[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
		mbnavedit_set_graphics(&g_xg, kNColors, mpixel_values);
		/* initialize mbnavedit proper */
		if (mbnavedit_init() != MB_SUCCESS) {
			QMessageBox::warning(parent, "MBnavedit", "Not enough memory for the navigation buffer.");
			g_mn = nullptr;
			g_xg.widget = nullptr;
			delete m->win;
			delete m;
			return false;
		}
		do_set_controls();
		do_filebutton_on();

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
			if (g_mn == m)
				g_mn = nullptr;
			delete m;
		});

		m->win->show();
		mbPlaceRight(m->win, parent);
		// the plot area gets its size once the window is laid out
		QApplication::processEvents();
		/* finally allow expose plots */
		m->expose_plot_ok = true;
		MnWindowFilter::mnLayout(m);
	}
	MbNavEdit *m = g_mn;
	mbParkRebind(m->parking, host.parkScene);
	mbParkShow(m->parking);                              // a parked one comes back off its handle

	// the command line's switches (-D -X -P -N)
	if (browse >= 0)
		mnSetOutputMode(m, browse ? OUTPUT_MODE_BROWSE : OUTPUT_MODE_OUTPUT);
	if (runMbprocess >= 0)
		mbnavedit_g.run_mbprocess = runMbprocess != 0;
	if (usePingData >= 0)
		mbnavedit_g.use_ping_data = usePingData != 0;
	if (stripComments >= 0)
		mbnavedit_g.strip_comments = stripComments != 0;

	// do_mbnavedit_init's startup file: every -I joins the list, the first new one is loaded
	if (files.isEmpty())
		return true;
	m->expose_plot_ok = false;
	mnCloseCurrent(m);
	m->currentfile = -1;
	const int numfilessave = int(m->files.size());
	for (int i = 0; i < files.size(); i++) {
		QByteArray f = QDir::toNativeSeparators(files[i]).toUtf8();
		const int form = i < int(formats.size()) ? formats[i] : 0;
		mbedit_parse_datalist(f.data(), form, mnAddFile, m);
	}
	m->usePrevious = usePrevious;
	if (int(m->files.size()) > numfilessave)
		mnLoadSpecificFile(m, numfilessave);
	m->expose_plot_ok = true;
	do_set_controls();
	int s_file_open = 0, s_nbuff = 0, s_current_id = 0, s_nplot = 0, s_nload_total = 0, s_ndump_total = 0;
	mbnavedit_get_state(&s_file_open, &s_nbuff, &s_current_id, &s_nplot, &s_nload_total, &s_ndump_total);
	return m->currentfile == numfilessave && s_file_open == 1;
}

int mbnaveditState(int *out, int n) {
	MbNavEdit *m = g_mn;
	int s_file_open = 0, s_nbuff = 0, s_current_id = 0, s_nplot = 0, s_nload_total = 0, s_ndump_total = 0;
	if (m)
		mbnavedit_get_state(&s_file_open, &s_nbuff, &s_current_id, &s_nplot, &s_nload_total, &s_ndump_total);
	const int v[14] = {m ? 1 : 0,
	                   s_file_open,
	                   m ? int(m->files.size()) : 0,
	                   m ? m->currentfile : -1,
	                   s_nbuff,
	                   s_current_id,
	                   s_nplot,
	                   s_nload_total,
	                   s_ndump_total,
	                   m ? mbnavedit_g.number_plots : 0,
	                   m ? mbnavedit_g.model_mode : 0,
	                   m ? mbnavedit_g.mode_pick : 0,
	                   m ? g_xg.img.width() : 0,
	                   m ? g_xg.img.height() : 0};
	const int k = n < 14 ? n : 14;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool mbnaveditRecord(int i, double *out6, int *selected) {
	return g_mn && mbnavedit_get_record(i, &out6[0], &out6[1], &out6[2], &out6[3], &out6[4], &out6[5], selected) == 1;
}

bool mbnaveditPlotBox(int iplot, int *out5) {
	return g_mn && mbnavedit_get_plot_box(iplot, &out5[0], &out5[1], &out5[2], &out5[3], &out5[4]) == 1;
}

bool mbnaveditRecordXY(int iplot, int i, int *x, int *y) {
	return g_mn && mbnavedit_get_record_xy(iplot, i, x, y) == 1;
}

bool mbnaveditMouse(int button, int x0, int y0, int x1, int y1) {
	MbNavEdit *m = g_mn;
	if (!m || !m->canvas)
		return false;
	QWidget *c = m->canvas;
	auto send = [c](QEvent::Type type, Qt::MouseButton b, Qt::MouseButtons held, int x, int y) {
		const QPointF pos(x, y);
		QMouseEvent ev(type, pos, c->mapToGlobal(pos), b, held, Qt::NoModifier);
		QApplication::sendEvent(c, &ev);
	};
	const Qt::MouseButton b = button == 1 ? Qt::LeftButton : button == 2 ? Qt::MiddleButton : Qt::RightButton;
	send(QEvent::MouseButtonPress, b, b, x0, y0);
	if (button == 1 && (x1 != x0 || y1 != y0)) {
		// the drag, a pixel at a time along the longer axis, as the pointer passes
		const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
		for (int k = 1; k <= steps; k++)
			send(QEvent::MouseMove, Qt::NoButton, b, x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps);
	}
	send(QEvent::MouseButtonRelease, b, Qt::NoButton, button == 1 ? x1 : x0, button == 1 ? y1 : y0);
	return true;
}

bool mbnaveditKey(int ch) {
	MbNavEdit *m = g_mn;
	if (!m || !m->canvas)
		return false;
	const QString t = QString(QChar(char16_t(ch)));
	QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, t);
	QApplication::sendEvent(m->canvas, &press);
	QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, t);
	QApplication::sendEvent(m->canvas, &release);
	return true;
}

bool mbnaveditPress(const QString &name) {
	MbNavEdit *m = g_mn;
	if (!m)
		return false;
	// the window and every dialog built so far
	QList<QWidget *> roots = {m->win};
	for (QDialog *d : {m->openDlg, m->fileListDlg, m->stepDlg, m->modelDlg, m->timeInterpDlg, m->deleteBadDlg, m->offsetDlg})
		if (d)
			roots << d;
	for (QWidget *r : roots) {
		if (auto *b = r->findChild<QAbstractButton *>(name)) {
			if (!b->isEnabled())
				return false;
			b->click();
			return true;
		}
		if (auto *a = r->findChild<QAction *>(name)) {
			if (!a->isEnabled())
				return false;
			a->trigger();
			return true;
		}
	}
	return false;
}

bool mbnaveditSavePng(const QString &path) {
	if (!g_mn)
		return false;
	xgFlush();
	return g_xg.img.save(path, "PNG");
}

bool mbnaveditClose() {
	if (!g_mn)
		return false;
	mbParkQuit(g_mn->parking);
	return true;
}
