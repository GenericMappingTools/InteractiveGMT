// ============================================================================
//  mbvelocity_window.cpp -- the water sound velocity profile editor's Qt window: the port of
//  MB-System's src/mbvelocitytool/mbvelocity_callbacks.c (the Motif interface of mbvelocitytool)
//  onto Qt, plus the xg_* graphics the engine draws through (MB-System's mbaux/mb_xgraphics.c),
//  painted into a QImage.
//
//  A translation unit of its own, apart from the viewer's fragments (see mbvelocity_window.h).
//  The engine (mbvelocity.c) holds the profiles and the data and does every edit, raytrace and
//  drawing decision; this file only turns widgets and mouse events into the same engine calls the
//  Motif callbacks made, with the same arguments, in the same order.
//
//  Drawing model, kept from X11: the engine draws straight onto the canvas and "erases" by drawing
//  again in white (a dragged node), with a clip rectangle that stays set between calls. So the
//  canvas is a persistent QImage, painted with NO antialiasing (a white line over an antialiased
//  black one would leave a grey ghost), and shown as is.
//
//  Departures from the Motif program, all forced by the host:
//    - MBIO is the swath editor's (mbeditLoadMbio): found, or asked for, once for both tools.
//    - The window opens empty; the files of the command line (-I -F -W -S) are mbvelocityOpenWindow's.
//    - Quit / the window's close box close the tool instead of ending the program.
//    - The node drag is event driven (mouse moves while the button is down) instead of polling
//      XQueryPointer.
//    - The canvas follows the window's size; the plot is laid out from its borders as before.
//    - The file selection box is the platform file dialog; the swath file's format id, which the
//      Motif box showed for that one case, is a small Open dialog of its own.
// ============================================================================

#include "mbvelocity_window.h"
#include "mbvelocity.h"

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
#include <QLabel>
#include <QLineEdit>
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

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

// the swath editor's engine: the MBIO version string and the format guess
extern "C" const char *mbedit_mbio_version(void);
extern "C" int mbvt_get_format(char *file, int *form);

namespace {

// MB-System's status values (mb_status.h)
const int MB_SUCCESS = 1;
const int MB_PATH_MAXLINE = 1024;

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
class MvCanvas;

struct XgCanvas {
	QImage img;
	QPainter *p = nullptr;
	QFont font;
	QRect clip;                            // xg_setclip: the GC's clip, kept between calls as in X11
	QPointer<MvCanvas> widget;
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
		if (g_xg.clip.isValid())
			g_xg.p->setClipRect(g_xg.clip);
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
struct MvScale {
	QSlider *s = nullptr;
	QLabel *val = nullptr;
	int decimals = 0;                      // 1: the slider runs in tenths (XmNdecimalPoints 1)

	QString fmt(int v) const {
		return decimals ? QString::number(v / 10.0, 'f', 1) : QString::number(v);
	}
	void setValue(int v) {
		if (!s)
			return;
		QSignalBlocker b(s);
		s->setValue(v);
		if (val)
			val->setText(fmt(s->value()));
	}
};

// ---- the tool: mbvelocity_callbacks.c's globals ---------------------------------------------
struct MbVelocity {
	MbEditHost host;
	QMainWindow *win = nullptr;
	MvCanvas *canvas = nullptr;

	QLabel *labelStatusDisplay = nullptr, *labelStatusEdit = nullptr, *labelStatusMb = nullptr;
	QPushButton *processButton = nullptr;
	QAction *actSaveSvp = nullptr, *actSaveSvpFile = nullptr, *actSaveResiduals = nullptr;

	// dialogs, built on first use and kept
	QDialog *scalingDlg = nullptr, *modeDlg = nullptr, *openDlg = nullptr;
	MvScale maxdepth, velrange, velcenter, resrange;
	QRadioButton *modeButton[3] = {};
	QLineEdit *openFile = nullptr;
	QSpinBox *openFormat = nullptr;

	// mbvelocity_callbacks.c state
	bool expose_plot_ok = false;
	int edit_gui = 0;
	int ndisplay_gui = 0;
	double maxdepth_gui = 0.0;
	double velrange_gui = 0.0;
	double velcenter_gui = 0.0;
	double resrange_gui = 0.0;
	int format_gui = 0;
	int anglemode_gui = 0;
	int nload = 0;
	char input_file[MB_PATH_MAXLINE] = "";
	int borders[4] = {0, 1019, 0, 550};

	// mouse: the button-1 drag of do_canvas_event
	bool dragging = false;
	bool ring_bell = false;
	int x_loc = 0, y_loc = 0;
};
MbVelocity *g_mv = nullptr;

void mvSetLabel(QLabel *l, const QString &s) {
	if (l)
		l->setText(s);
}

// ---- the canvas widget ---------------------------------------------------------------------
class MvCanvas : public QWidget {
public:
	explicit MvCanvas(QWidget *parent) : QWidget(parent) {
		setMouseTracking(false);
		setAttribute(Qt::WA_OpaquePaintEvent);
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

	// do_expose: the canvas changed size, so the plot follows its borders.
	void resizeEvent(QResizeEvent *) override {
		MbVelocity *m = g_mv;
		if (!m)
			return;
		if (g_xg.p) {
			g_xg.p->end();
			delete g_xg.p;
			g_xg.p = nullptr;
		}
		g_xg.img = QImage(qMax(1, width()), qMax(1, height()), QImage::Format_RGB32);
		g_xg.img.fill(Qt::white);
		g_xg.clip = QRect();
		m->borders[0] = 0;
		m->borders[1] = width() - 1;
		m->borders[2] = 0;
		m->borders[3] = height() - 1;
		static unsigned int mpixel_values[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
		mbvt_set_graphics(&g_xg, m->borders, kNColors, mpixel_values);
		if (m->expose_plot_ok) {
			mbvt_plot();
			xgFlush();
		}
		else
			update();
	}

	// do_canvas_event, ButtonPress
	void mousePressEvent(QMouseEvent *e) override {
		MbVelocity *m = g_mv;
		if (!m)
			return;
		const int x = int(e->position().x()), y = int(e->position().y());
		if (e->button() == Qt::LeftButton) {
			/* If left mouse button is pushed then move nearest svp node. */
			m->x_loc = x;
			m->y_loc = y;
			mbvt_action_select_node(m->x_loc, m->y_loc);
			m->dragging = true;
			m->ring_bell = false;
			dragTo(m, x, y);
		}
		else if (e->button() == Qt::MiddleButton) {
			/* If middle mouse button is pushed then add svp node. */
			const int status = mbvt_action_add_node(x, y);
			if (status != 1)
				QApplication::beep();
			/* replot graph */
			mbvt_plot();
			xgFlush();
		}
		else if (e->button() == Qt::RightButton) {
			/* If right mouse button is pushed then delete nearest svp node. */
			const int status = mbvt_action_delete_node(x, y);
			if (status != 1)
				QApplication::beep();
			/* replot graph */
			mbvt_plot();
			xgFlush();
		}
	}

	// the button is still pressed: run the drag again where the pointer is
	void mouseMoveEvent(QMouseEvent *e) override {
		MbVelocity *m = g_mv;
		if (!m || !m->dragging)
			return;
		dragTo(m, int(e->position().x()), int(e->position().y()));
	}

	// do_canvas_event, ButtonRelease
	void mouseReleaseEvent(QMouseEvent *e) override {
		MbVelocity *m = g_mv;
		if (!m || e->button() != Qt::LeftButton || !m->dragging)
			return;
		dragEnd(m);
	}

public:
	static void dragTo(MbVelocity *m, int x, int y) {
		m->x_loc = x;
		m->y_loc = y;
		const int status = mbvt_action_drag_node(m->x_loc, m->y_loc);
		if (status == 0 && !m->ring_bell) {
			m->ring_bell = true;
			QApplication::beep();
		}
		xgFlush();
	}
	static void dragEnd(MbVelocity *m) {
		m->dragging = false;
		if (m->ring_bell)
			QApplication::beep();
		/* replot graph */
		mbvt_plot();
		xgFlush();
		const int status = mbvt_action_mouse_up(m->x_loc, m->y_loc);
		if (status == 0)
			QApplication::beep();
	}
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

// replot everything (the closing lines of nearly every callback)
void mvReplot(MbVelocity *m) {
	do_set_controls();
	(void)m;
	mbvt_plot();
	xgFlush();
}

// ---- small helpers -------------------------------------------------------------------------
QWidget *mvLoadUi(MbVelocity *m, const char *file, QWidget *parent) {
	QFile f(QDir(m->host.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBvelocitytool", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "MBvelocitytool", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

// A child the .ui must carry; a missing one is reported by name, so a renamed widget in the .ui
// is found at once.
template <typename T>
T *mvChild(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

void mvBindScale(QWidget *root, const char *name, int decimals, MvScale &sc, QStringList &missing) {
	sc.s = mvChild<QSlider>(root, name, missing);
	sc.val = mvChild<QLabel>(root, (std::string(name) + "Value").c_str(), missing);
	sc.decimals = decimals;
}

// Motif's XmScale called back on release (and on keyboard steps); the value label follows the
// drag live.
void mvOnScale(MvScale &sc, std::function<void(int)> fn) {
	QSlider *s = sc.s;
	MvScale *scp = &sc;
	QObject::connect(s, &QSlider::valueChanged, s, [s, scp, fn](int v) {
		if (scp->val)
			scp->val->setText(scp->fmt(v));
		if (!s->isSliderDown())
			fn(v);
	});
	QObject::connect(s, &QSlider::sliderReleased, s, [s, fn]() { fn(s->value()); });
}

void mvShow(QDialog *d) {
	d->show();
	d->raise();
	d->activateWindow();
}

void mvSetValues(MbVelocity *m) {
	mbvt_set_values(m->edit_gui, m->ndisplay_gui, m->maxdepth_gui, m->velrange_gui, m->velcenter_gui, m->resrange_gui,
	                m->anglemode_gui);
}

// ---- dialogs -------------------------------------------------------------------------------
// bulletinBoard_scaling: do_maxdepth, do_velrange, do_velcenter, do_residual_range
void mvScalingDialog(MbVelocity *m) {
	if (!m->scalingDlg) {
		QWidget *w = mvLoadUi(m, "mbvelocitytool_scaling.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		MvScale maxdepth, velrange, velcenter, resrange;
		mvBindScale(d, "maxdepthSlider", 0, maxdepth, missing);
		mvBindScale(d, "velrangeSlider", 0, velrange, missing);
		mvBindScale(d, "velcenterSlider", 0, velcenter, missing);
		mvBindScale(d, "resrangeSlider", 1, resrange, missing);
		auto *dismiss = mvChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBvelocitytool", "mbvelocitytool_scaling.ui lacks: " + missing.join(", "));
			delete d;
			return;
		}
		m->scalingDlg = d;
		m->maxdepth = maxdepth;
		m->velrange = velrange;
		m->velcenter = velcenter;
		m->resrange = resrange;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		mvOnScale(m->maxdepth, [m](int v) {
			m->maxdepth_gui = double(v);
			mvSetValues(m);
			/* replot everything */
			mvReplot(m);
		});
		mvOnScale(m->velrange, [m](int v) {
			m->velrange_gui = double(v);
			mvSetValues(m);
			mvReplot(m);
		});
		mvOnScale(m->velcenter, [m](int v) {
			m->velcenter_gui = double(v);
			mvSetValues(m);
			mvReplot(m);
		});
		mvOnScale(m->resrange, [m](int v) {
			m->resrange_gui = double(v) / 10.0;
			mvSetValues(m);
			mvReplot(m);
		});
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
		do_set_controls();
	}
	mvShow(m->scalingDlg);
}

// bulletinBoard_mode: do_anglemode
void mvModeDialog(MbVelocity *m) {
	if (!m->modeDlg) {
		QWidget *w = mvLoadUi(m, "mbvelocitytool_mode.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		QRadioButton *b[3];
		b[0] = mvChild<QRadioButton>(d, "modeOk", missing);
		b[1] = mvChild<QRadioButton>(d, "modeSnell", missing);
		b[2] = mvChild<QRadioButton>(d, "modeNull", missing);
		auto *dismiss = mvChild<QPushButton>(d, "dismissButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBvelocitytool", "mbvelocitytool_mode.ui lacks: " + missing.join(", "));
			delete d;
			return;
		}
		m->modeDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		auto *group = new QButtonGroup(d);
		for (int i = 0; i < 3; i++) {
			m->modeButton[i] = b[i];
			group->addButton(b[i], i);
			QObject::connect(b[i], &QRadioButton::toggled, d, [m, i](bool on) {
				if (!on)
					return;
				m->anglemode_gui = i;
				mvSetValues(m);
				/* replot everything */
				mvReplot(m);
			});
		}
		QObject::connect(dismiss, &QPushButton::clicked, d, &QDialog::hide);
		do_set_controls();
	}
	mvShow(m->modeDlg);
}

// do_open, MBVT_IO_OPEN_MB
bool mvOpenSwath(MbVelocity *m, const QByteArray &file, int format) {
	if (file.isEmpty())
		return false;
	std::snprintf(m->input_file, sizeof(m->input_file), "%s", file.constData());

	/* turn off expose plots */
	m->expose_plot_ok = false;

	/* open file */
	m->format_gui = format;
	const int status = mbvt_open_swath_file(m->input_file, m->format_gui, &m->nload);

	/* reset status message */
	if (status == 1)
		mvSetLabel(m->labelStatusMb,
		           QString("Read %1 pings from swath file: %2").arg(m->nload).arg(QString::fromUtf8(m->input_file)));
	if (status == 1 && m->edit_gui != 1)
		mvSetLabel(m->labelStatusEdit, "Loaded default editable SVP");

	/* turn on expose plots */
	m->expose_plot_ok = true;

	if (status != 1)
		QApplication::beep();

	/* replot everything */
	mvReplot(m);
	return status == 1;
}

// bulletinBoard_fileselect in its "Open Swath Sonar Data" guise: the file and its format id
void mvOpenSwathDialog(MbVelocity *m) {
	if (!m->openDlg) {
		QWidget *w = mvLoadUi(m, "mbvelocitytool_open.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		m->openFile = mvChild<QLineEdit>(d, "fileEdit", missing);
		m->openFormat = mvChild<QSpinBox>(d, "formatBox", missing);
		auto *browse = mvChild<QPushButton>(d, "browseButton", missing);
		auto *ok = mvChild<QPushButton>(d, "okButton", missing);
		auto *cancel = mvChild<QPushButton>(d, "cancelButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBvelocitytool", "mbvelocitytool_open.ui lacks: " + missing.join(", "));
			delete d;
			m->openFile = nullptr;
			m->openFormat = nullptr;
			return;
		}
		m->openDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		// do_fileselection_list: a chosen file gets its format guessed from its name
		auto guess = [m]() {
			QByteArray path = QDir::fromNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (path.isEmpty())
				return;
			int form = m->format_gui;
			if (mbvt_get_format(path.data(), &form) == MB_SUCCESS) {
				m->format_gui = form;
				m->openFormat->setValue(m->format_gui);
			}
		};
		QObject::connect(browse, &QPushButton::clicked, d, [m, d, guess]() {
			const QString fn = QFileDialog::getOpenFileName(d, "Open Swath Sonar Data", m->host.startDir(), "All Files (*)");
			if (fn.isEmpty())
				return;
			m->host.rememberDir(fn);
			m->openFile->setText(QDir::toNativeSeparators(fn));
			guess();
		});
		QObject::connect(m->openFile, &QLineEdit::editingFinished, d, guess);
		QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::hide);
		QObject::connect(ok, &QPushButton::clicked, d, [m, d]() {
			const QByteArray input_file = QDir::fromNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (input_file.isEmpty()) {
				QApplication::beep();
				return;
			}
			d->hide();
			/* get format id value */
			mvOpenSwath(m, input_file, m->openFormat->value());
		});
	}
	m->openFormat->setValue(m->format_gui);
	mvShow(m->openDlg);
}

// do_open for the three SVP cases (MBVT_IO_OPEN_DISPLAY_SVP, _OPEN_EDIT_SVP, _SAVE_EDIT_SVP)
enum { IO_OPEN_DISPLAY_SVP = 1, IO_OPEN_EDIT_SVP = 2, IO_SAVE_EDIT_SVP = 3 };

bool mvSvpFile(MbVelocity *m, int open_type, const QByteArray &file) {
	if (file.isEmpty())
		return false;
	std::snprintf(m->input_file, sizeof(m->input_file), "%s", file.constData());
	const QString shown = QString::fromUtf8(m->input_file);
	int status = 0;
	if (open_type == IO_OPEN_DISPLAY_SVP) {
		/* open file */
		status = mbvt_open_display_profile(m->input_file);

		/* reset status message */
		if (status == 1)
			mvSetLabel(m->labelStatusDisplay, "Loaded display SVP from: " + shown);
	}
	else if (open_type == IO_OPEN_EDIT_SVP) {
		/* open file */
		status = mbvt_open_edit_profile(m->input_file);

		/* reset status message */
		if (status == 1) {
			m->edit_gui = 1;
			mvSetLabel(m->labelStatusEdit, "Loaded editable SVP from: " + shown);
		}
	}
	else if (open_type == IO_SAVE_EDIT_SVP && m->edit_gui == 1) {
		/* save file */
		status = mbvt_save_edit_profile(m->input_file);

		/* reset status message */
		if (status == 1)
			mvSetLabel(m->labelStatusEdit, "Saved editable SVP to: " + shown);
	}

	if (status != 1)
		QApplication::beep();

	/* replot everything */
	mvReplot(m);
	return status == 1;
}

void mvSvpDialog(MbVelocity *m, int open_type) {
	const char *title = open_type == IO_OPEN_DISPLAY_SVP ? "Open Display Sound Velocity Profile (SVP)"
	                    : open_type == IO_OPEN_EDIT_SVP  ? "Open Editable Sound Velocity Profile (SVP)"
	                                                     : "Save Editable Sound Velocity Profile (SVP)";
	const QString filter = "SVP files (*.svp);;All Files (*)";
	const QString fn = open_type == IO_SAVE_EDIT_SVP
	                       ? QFileDialog::getSaveFileName(m->win, title, m->host.startDir(), filter)
	                       : QFileDialog::getOpenFileName(m->win, title, m->host.startDir(), filter);
	if (fn.isEmpty())
		return;
	m->host.rememberDir(fn);
	mvSvpFile(m, open_type, QDir::toNativeSeparators(fn).toUtf8());
}

// do_new_profile
void mvNewProfile(MbVelocity *m) {
	/* get new edit velocity profile */
	mbvt_new_edit_profile();
	mvSetLabel(m->labelStatusEdit, "Loaded default editable SVP");

	/* replot everything */
	mvReplot(m);
}

// do_process_mb
bool mvProcess(MbVelocity *m) {
	/* turn off expose plots */
	m->expose_plot_ok = false;

	/* process Swath Sonar data */
	const int status = mbvt_process_multibeam();
	if (status != 1)
		QApplication::beep();

	/* turn on expose plots */
	m->expose_plot_ok = true;

	/* replot everything */
	mvReplot(m);
	return status == 1;
}

// do_save_swath_svp
bool mvSaveSwathSvp(MbVelocity *m) {
	int status = 0;
	if (m->edit_gui == 1) {
		/* save file */
		status = mbvt_save_swath_profile(m->input_file);

		/* reset status message */
		if (status == 1)
			mvSetLabel(m->labelStatusEdit, "Saved Editable Sound Velocity Profile: " + QString::fromUtf8(m->input_file));
	}

	if (status != 1)
		QApplication::beep();

	/* replot everything */
	mvReplot(m);
	return status == 1;
}

// do_save_residuals
bool mvSaveResiduals(MbVelocity *m) {
	int status = 0;
	if (m->edit_gui == 1 && m->nload > 0) {
		/* save file */
		status = mbvt_save_residuals(m->input_file);

		/* reset status message */
		if (status == 1)
			mvSetLabel(m->labelStatusEdit, "Saved Residuals as Beam Offsets: " + QString::fromUtf8(m->input_file));
	}

	if (status != 1)
		QApplication::beep();

	/* replot everything */
	mvReplot(m);
	return status == 1;
}

void mvAbout(MbVelocity *m) {
	QMessageBox::about(m->win, "About MBvelocitytool",
		QString("<b>MBvelocitytool</b><br>Interactive SVP Modeler/Editor<br><br>"
		        "One Component of the <b>MB-System</b> Open Source Software Package<br>"
		        "for Processing and Display of Swath Sonar Data<br><br>"
		        "Created by: David W. Caress and Dale N. Chayes<br>"
		        "Monterey Bay Aquarium Research Institute &nbsp; / &nbsp; Lamont-Doherty Earth Observatory<br><br>"
		        "MB-System library in use: %1<br><br>"
		        "Ported to InteractiveGMT (Qt) from MB-System's mbvelocitytool.")
		    .arg(QString::fromLatin1(mbedit_mbio_version())));
}

// ---- the window ----------------------------------------------------------------------------
class MvCloseFilter : public QObject {
public:
	explicit MvCloseFilter(QObject *parent) : QObject(parent) {}

protected:
	// do_quit: the tool's memory goes with the window
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() == QEvent::Close && g_mv && o == g_mv->win) {
			g_mv->expose_plot_ok = false;
			mbvt_quit();
		}
		return QObject::eventFilter(o, e);
	}
};

MbVelocity *mvBuild(QWidget *parent, const MbEditHost &host) {
	auto *m = new MbVelocity;
	m->host = host;
	QWidget *root = mvLoadUi(m, "mbvelocitytool.ui", nullptr);
	auto *win = qobject_cast<QMainWindow *>(root);
	if (!win) {
		delete root;
		delete m;
		return nullptr;
	}
	QStringList missing;
	auto *quitButton = mvChild<QPushButton>(win, "quitButton", missing);
	auto *scalingButton = mvChild<QPushButton>(win, "plotScalingButton", missing);
	auto *modeButton = mvChild<QPushButton>(win, "angleModeButton", missing);
	auto *aboutButton = mvChild<QPushButton>(win, "aboutButton", missing);
	m->processButton = mvChild<QPushButton>(win, "reprocessButton", missing);
	m->labelStatusDisplay = mvChild<QLabel>(win, "labelStatusDisplay", missing);
	m->labelStatusEdit = mvChild<QLabel>(win, "labelStatusEdit", missing);
	m->labelStatusMb = mvChild<QLabel>(win, "labelStatusMb", missing);
	auto *actOpenDisplay = mvChild<QAction>(win, "actionOpenDisplaySvp", missing);
	auto *actOpenEdit = mvChild<QAction>(win, "actionOpenEditSvp", missing);
	auto *actNewEdit = mvChild<QAction>(win, "actionNewEditSvp", missing);
	m->actSaveSvp = mvChild<QAction>(win, "actionSaveEditSvp", missing);
	auto *actOpenMb = mvChild<QAction>(win, "actionOpenSwath", missing);
	m->actSaveSvpFile = mvChild<QAction>(win, "actionSaveSwathSvp", missing);
	m->actSaveResiduals = mvChild<QAction>(win, "actionSaveResiduals", missing);
	auto *host_w = mvChild<QWidget>(win, "canvasHost", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBvelocitytool", "mbvelocitytool.ui lacks: " + missing.join(", "));
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
	m->canvas = new MvCanvas(host_w);
	lay->addWidget(m->canvas);
	g_xg.widget = m->canvas;
	// mbvelocitytool's font: -*-fixed-bold-r-normal-*-13-*
	g_xg.font = QFont("Courier New");
	g_xg.font.setStyleHint(QFont::TypeWriter);
	g_xg.font.setBold(true);
	g_xg.font.setPixelSize(13);
	g_xg.font.setStyleStrategy(QFont::NoAntialias);

	// buttons
	QObject::connect(quitButton, &QPushButton::clicked, win, &QWidget::close);
	QObject::connect(scalingButton, &QPushButton::clicked, win, [m]() { mvScalingDialog(m); });
	QObject::connect(modeButton, &QPushButton::clicked, win, [m]() { mvModeDialog(m); });
	QObject::connect(m->processButton, &QPushButton::clicked, win, [m]() { mvProcess(m); });
	QObject::connect(aboutButton, &QPushButton::clicked, win, [m]() { mvAbout(m); });

	// File menu
	QObject::connect(actOpenDisplay, &QAction::triggered, win, [m]() { mvSvpDialog(m, IO_OPEN_DISPLAY_SVP); });
	QObject::connect(actOpenEdit, &QAction::triggered, win, [m]() { mvSvpDialog(m, IO_OPEN_EDIT_SVP); });
	QObject::connect(actNewEdit, &QAction::triggered, win, [m]() { mvNewProfile(m); });
	QObject::connect(m->actSaveSvp, &QAction::triggered, win, [m]() { mvSvpDialog(m, IO_SAVE_EDIT_SVP); });
	QObject::connect(actOpenMb, &QAction::triggered, win, [m]() { mvOpenSwathDialog(m); });
	QObject::connect(m->actSaveSvpFile, &QAction::triggered, win, [m]() { mvSaveSwathSvp(m); });
	QObject::connect(m->actSaveResiduals, &QAction::triggered, win, [m]() { mvSaveResiduals(m); });

	win->installEventFilter(new MvCloseFilter(win));
	return m;
}

} // namespace

// ---- the engine's host hooks (declared in mbvelocity.h) ------------------------------------
extern "C" {

void xg_setclip(void *, int x, int y, int width, int height) {
	g_xg.clip = QRect(x, y, width, height);
	if (g_xg.p)
		g_xg.p->setClipRect(g_xg.clip);
}

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
	if (g_mv && g_mv->host.busyText)
		g_mv->host.busyText(message);
	return 1;
}

int do_message_off(void) {
	if (g_mv && g_mv->host.busyOff)
		g_mv->host.busyOff();
	return 1;
}

int do_error_dialog(char *s1, char *s2, char *s3) {
	QApplication::beep();
	QMessageBox::warning(g_mv ? g_mv->win : nullptr, "MBvelocitytool",
	                     QString::fromUtf8(s1) + "\n" + QString::fromUtf8(s2) + "\n" + QString::fromUtf8(s3));
	return 1;
}

// do_set_controls
void do_set_controls(void) {
	MbVelocity *m = g_mv;
	if (!m)
		return;
	/* get some values from mbvelocitytool */
	mbvt_get_values(&m->edit_gui, &m->ndisplay_gui, &m->maxdepth_gui, &m->velrange_gui, &m->velcenter_gui, &m->resrange_gui,
	                &m->anglemode_gui, &m->format_gui);

	if (m->ndisplay_gui < 1)
		mvSetLabel(m->labelStatusDisplay, "No display SVPs loaded...");
	else if (m->ndisplay_gui == 1)
		mvSetLabel(m->labelStatusDisplay, QString("Loaded %1 display SVP").arg(m->ndisplay_gui));
	else
		mvSetLabel(m->labelStatusDisplay, QString("Loaded %1 display SVPs").arg(m->ndisplay_gui));

	/* set pushbuttons */
	m->actSaveSvp->setEnabled(m->edit_gui == 1);
	m->actSaveSvpFile->setEnabled(m->edit_gui == 1);
	m->processButton->setEnabled(m->nload > 0);
	m->actSaveResiduals->setEnabled(m->nload > 0);

	/* set values of maximum depth, velocity range, velocity center and residual range sliders */
	m->maxdepth.setValue(int(m->maxdepth_gui));
	m->velrange.setValue(int(m->velrange_gui));
	m->velcenter.setValue(int(m->velcenter_gui));
	m->resrange.setValue(int(10 * m->resrange_gui));

	/* set values of angle mode radiobox */
	if (m->anglemode_gui >= 0 && m->anglemode_gui < 3 && m->modeButton[m->anglemode_gui]) {
		QSignalBlocker b(m->modeButton[m->anglemode_gui]);
		m->modeButton[m->anglemode_gui]->setChecked(true);
	}

	/* set value of format text item */
	if (m->openFormat) {
		QSignalBlocker b(m->openFormat);
		m->openFormat->setValue(m->format_gui);
	}
}

} // extern "C"


// ---- entry point ---------------------------------------------------------------------------
bool mbvelocityOpenWindow(QWidget *parent, const MbEditHost &host, const QString &swathFile, int format, const QString &editSvp,
                          const QString &displaySvp) {
	if (!g_mv) {
		if (!mbeditLoadMbio(parent, host))
			return false;
		char msg[2048] = "";
		if (!mbvt_mbio_open(msg, int(sizeof(msg)))) {
			QMessageBox::warning(parent, "MBvelocitytool", QString::fromUtf8(msg));
			return false;
		}
		MbVelocity *m = mvBuild(parent, host);
		if (!m)
			return false;
		g_mv = m;

		// do_mbvelocity_init
		m->expose_plot_ok = false;
		/* initialize some labels */
		mvSetLabel(m->labelStatusDisplay, "No display SVPs loaded...");
		mvSetLabel(m->labelStatusEdit, "No editable SVP loaded...");
		mvSetLabel(m->labelStatusMb, "No swath sonar data loaded...");
		static unsigned int mpixel_values[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
		mbvt_set_graphics(&g_xg, m->borders, kNColors, mpixel_values);
		/* initialize mbvelocitytool proper */
		mbvt_init();
		/* set the controls */
		do_set_controls();

		if (host.windowOpened)
			host.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (g_xg.p) {
				g_xg.p->end();
				delete g_xg.p;
				g_xg.p = nullptr;
			}
			g_xg.img = QImage();
			g_xg.clip = QRect();
			g_xg.widget = nullptr;
			if (m->host.windowClosed)
				m->host.windowClosed();
			if (g_mv == m)
				g_mv = nullptr;
			delete m;
		});

		m->win->show();
		/* finally allow expose plots */
		m->expose_plot_ok = true;
		// the canvas gets its size (and so its image) once the window is laid out
		QApplication::processEvents();
		mbvt_plot();
		xgFlush();
	}
	MbVelocity *m = g_mv;
	m->win->showNormal();
	m->win->raise();
	m->win->activateWindow();

	// do_open_commandline: the swath file, then the editable SVP, then the display SVP
	bool ok = true;
	if (!swathFile.isEmpty()) {
		QByteArray f = QDir::fromNativeSeparators(swathFile).toUtf8();
		int form = format;
		if (form == 0)
			mbvt_get_format(f.data(), &form);
		ok = mvOpenSwath(m, f, form) && ok;
	}
	if (!editSvp.isEmpty())
		ok = mvSvpFile(m, IO_OPEN_EDIT_SVP, QDir::toNativeSeparators(editSvp).toUtf8()) && ok;
	if (!displaySvp.isEmpty())
		ok = mvSvpFile(m, IO_OPEN_DISPLAY_SVP, QDir::toNativeSeparators(displaySvp).toUtf8()) && ok;
	return ok;
}

int mbvelocityState(int *out, int n) {
	MbVelocity *m = g_mv;
	int s_edit = 0, s_nedit = 0, s_ndisplay = 0, s_nbuffer = 0, s_nbeams = 0;
	if (m)
		mbvt_get_state(&s_edit, &s_nedit, &s_ndisplay, &s_nbuffer, &s_nbeams);
	const int v[8] = {m ? 1 : 0, s_edit, s_nedit, s_ndisplay, s_nbuffer, s_nbeams,
	                  m ? m->borders[1] + 1 : 0, m ? m->borders[3] + 1 : 0};
	const int k = n < 8 ? n : 8;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool mbvelocityMouse(int button, int x0, int y0, int x1, int y1) {
	MbVelocity *m = g_mv;
	if (!m || !m->canvas)
		return false;
	auto send = [m](QEvent::Type type, Qt::MouseButton b, Qt::MouseButtons held, int x, int y) {
		const QPointF pos(x, y);
		QMouseEvent ev(type, pos, m->canvas->mapToGlobal(pos), b, held, Qt::NoModifier);
		QApplication::sendEvent(m->canvas, &ev);
	};
	const Qt::MouseButton b = button == 1 ? Qt::LeftButton : button == 2 ? Qt::MiddleButton : Qt::RightButton;
	send(QEvent::MouseButtonPress, b, b, x0, y0);
	if (button == 1)
		send(QEvent::MouseMove, Qt::NoButton, b, x1, y1);
	send(QEvent::MouseButtonRelease, b, Qt::NoButton, button == 1 ? x1 : x0, button == 1 ? y1 : y0);
	return true;
}

bool mbvelocityEditNode(int i, double *depth, double *velocity) {
	return g_mv && mbvt_get_edit_node(i, depth, velocity) == 1;
}

bool mbvelocityReprocess() {
	return g_mv && mvProcess(g_mv);
}

bool mbvelocitySaveSwathSvp() {
	return g_mv && mvSaveSwathSvp(g_mv);
}

bool mbvelocitySaveResiduals() {
	return g_mv && mvSaveResiduals(g_mv);
}

bool mbvelocitySavePng(const QString &path) {
	if (!g_mv)
		return false;
	xgFlush();
	return g_xg.img.save(path, "PNG");
}

bool mbvelocityClose() {
	if (!g_mv)
		return false;
	g_mv->win->close();
	return true;
}
