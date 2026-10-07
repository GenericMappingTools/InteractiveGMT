// ============================================================================
//  mb3dsoundings_window.cpp -- mbeditviz's 3-D sounding editor: MB-System's libmbview
//  mb3dsoundings (mb3dsoundings_callbacks.c + the Mb3dsdg widget) on Qt + VTK.
//
//  A translation unit of its own (see mb3dsoundings_window.h). What is mb3dsoundings_callbacks.c's,
//  kept as it was: the state (edit and mouse modes, rotation, exaggeration, pan, zoom, the bias
//  sliders' integer values and their re-centring, the view switches), every edit function (pick,
//  erase/restore, grab, flag/unflag view, the bad/good/zero/left/right ping macros), their thresholds
//  in window pixels, the order in which they notify the caller and flush, the key macros, the
//  colours of each sounding class, the status and mouse-mode texts.
//
//  What is new is only the drawing: instead of immediate-mode OpenGL into a Motif GLwDrawingArea,
//  the soundings, the bounding box, the profiles and the grab rectangle are VTK actors in a
//  QVTKOpenGLNativeWidget, viewed through the same transform (translate by the pan offset, rotate
//  about x by elevation-90, about z by azimuth; an orthographic view whose half height is 1/zoom).
//  The window pixel of each sounding (winx, winy: origin bottom left, as gluProject gave it) is
//  computed from that same transform after every draw, so the edit code reads the pixels of what is
//  on screen. Departures:
//    - the view keeps the window's aspect ratio (glOrtho stretched -1..1 over any window shape);
//    - the "dotted" (stippled) far edges of the bounding box are drawn light grey, since modern
//      OpenGL has no line stipple;
//    - pixel thresholds follow the screen's device pixel ratio.
// ============================================================================

#include "mb3dsoundings_window.h"
#include "mbeditviz.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QSet>
#include <QVTKOpenGLNativeWidget.h>
#include <QPainter>
#include <QPixmap>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QUiLoader>
#include <QVBoxLayout>

#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkCellData.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkTransform.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace {

// ---- mb3dsoundingsprivate.h -------------------------------------------------------------------
const int MBS_EDIT_NONE = -1;          // no edit mode armed: the left button is the view's (iGMT)
const int MBS_EDIT_TOGGLE = 0;
const int MBS_EDIT_PICK = 1;
const int MBS_EDIT_ERASE = 2;
const int MBS_EDIT_RESTORE = 3;
const int MBS_EDIT_GRAB = 4;
const int MBS_EDIT_INFO = 5;
const double MBS_PICK_THRESHOLD = 50;
const double MBS_ERASE_THRESHOLD = 10;   // Erase / Restore reach, screen px (15 in mb3dsoundings; the cursor circle shows it)
const int MBS_EDIT_GRAB_START = 0;
const int MBS_EDIT_GRAB_MOVE = 1;
const int MBS_EDIT_GRAB_END = 2;
const int MBS_VIEW_PROFILES_NONE = 0;
const int MBS_VIEW_PROFILES_UNFLAGGED = 1;
const int MBS_VIEW_PROFILES_ALL = 2;
const int MBS_VIEW_COLOR_FLAG = 0;
const int MBS_VIEW_COLOR_TOPO = 1;
const int MBS_VIEW_COLOR_AMP = 2;
const int MBS_VIEW_COLOR_SOUNDING = 3;
const int MBS_VIEW_COLOR_FILE = 4;     // iGMT: one colour per swath file (sounding->ifile)
const int MBV_NUM_COLORS = 11;

// ---- mbviewprivate.h colour tables ------------------------------------------------------------
const float colortable_haxby_red[MBV_NUM_COLORS] = {0.950f, 1.000f, 1.000f, 1.000f, 0.941f, 0.804f, 0.541f, 0.416f, 0.196f, 0.157f, 0.145f};
const float colortable_haxby_green[MBV_NUM_COLORS] = {0.950f, 0.729f, 0.631f, 0.741f, 0.925f, 1.000f, 0.925f, 0.922f, 0.745f, 0.498f, 0.224f};
const float colortable_haxby_blue[MBV_NUM_COLORS] = {0.950f, 0.522f, 0.267f, 0.341f, 0.475f, 0.635f, 0.682f, 1.000f, 1.000f, 0.984f, 0.686f};
const float colortable_redtoblue_red[MBV_NUM_COLORS] = {1.000f, 1.000f, 1.000f, 1.000f, 1.000f, 0.750f, 0.500f, 0.000f, 0.000f, 0.000f, 0.000f};
const float colortable_redtoblue_green[MBV_NUM_COLORS] = {0.000f, 0.250f, 0.500f, 0.750f, 1.000f, 1.000f, 1.000f, 1.000f, 1.000f, 0.500f, 0.000f};
const float colortable_redtoblue_blue[MBV_NUM_COLORS] = {0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 1.000f, 1.000f, 1.000f};
const float colortable_object_red[MBV_NUM_COLORS] = {0.000f, 1.000f, 1.000f, 1.000f, 0.000f, 0.000f, 0.000f, 1.000f, 0.000f, 0.000f, 0.000f};
const float colortable_object_green[MBV_NUM_COLORS] = {0.000f, 1.000f, 0.000f, 1.000f, 1.000f, 1.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f};
const float colortable_object_blue[MBV_NUM_COLORS] = {0.000f, 1.000f, 0.000f, 0.000f, 0.000f, 1.000f, 1.000f, 1.000f, 0.000f, 0.000f, 0.000f};

// mbview_getcolor (mbview_process.c), MBV_COLORTABLE_NORMAL
void mbviewGetColor(double value, double min, double max, float below_red, float below_green, float below_blue,
                    float above_red, float above_green, float above_blue, const float *colortable_red,
                    const float *colortable_green, const float *colortable_blue, float *red, float *green, float *blue) {
	double factor;
	if (max <= min)
		factor = 0.5;
	else
		factor = (max - value) / (max - min);
	if (factor >= 1.0) {
		*red = above_red;
		*green = above_green;
		*blue = above_blue;
	}
	else if (factor <= 0.0) {
		*red = below_red;
		*green = below_green;
		*blue = below_blue;
	}
	else {
		const int i = (int)(factor * (MBV_NUM_COLORS - 1));
		const double ff = factor * (MBV_NUM_COLORS - 1) - i;
		*red = (float)(colortable_red[i] + ff * (colortable_red[i + 1] - colortable_red[i]));
		*green = (float)(colortable_green[i] + ff * (colortable_green[i + 1] - colortable_green[i]));
		*blue = (float)(colortable_blue[i] + ff * (colortable_blue[i + 1] - colortable_blue[i]));
	}
}

// ---- mb3dsoundings_callbacks.c's `mb3dsoundings` struct ----------------------------------------
struct Mb3dsdg {
	MbParking *parking = nullptr;             // X / minimise park it in Scene Objects (mbParkable)
	MbEditHost host;
	QMainWindow *win = nullptr;
	// THE VIEWER'S OWN 3-D VIEW (MbEditHost::view3dMake): iGMT's navigation and gizmo, used as they are
	QWidget *canvasW = nullptr;
	void *view = nullptr;
	vtkRenderWindow *rw = nullptr;
	QString uiDir;
	Mb3dsdgNotify notify;
	mb3dsoundings_struct *soundingdata = nullptr;

	QLabel *labelStatus = nullptr;
	QRadioButton *modeButton[6] = {};
	QAction *actViewFlagged = nullptr, *actViewSecondary = nullptr, *actNoConnect = nullptr, *actConnectGood = nullptr,
	        *actConnectAll = nullptr, *actBoundingBox = nullptr, *actScaleWithFlagged = nullptr, *actColorByFlag = nullptr,
	        *actColorByTopo = nullptr, *actColorBySounding = nullptr, *actColorByAmp = nullptr;
	QSlider *scale_rollbias = nullptr, *scale_pitchbias = nullptr, *scale_headingbias = nullptr, *scale_timelag = nullptr,
	        *scale_snell = nullptr;
	QLabel *val_rollbias = nullptr, *val_pitchbias = nullptr, *val_headingbias = nullptr, *val_timelag = nullptr,
	       *val_snell = nullptr;
	QWidget *biasPanel = nullptr;

	// VTK
	vtkSmartPointer<vtkRenderer> ren;
	vtkSmartPointer<vtkActor> pointsActor, infoActor, boxSolidActor, boxDotActor, profileActor;
	vtkSmartPointer<vtkActor2D> grabActor;

	int edit_mode = MBS_EDIT_NONE;            // armed like an iGMT draw tool; none = the view navigates
	bool keyreverse_mode = false;

	/* drawing variables: the view direction is the CAMERA's (iGMT navigation); the exaggeration is the
	   gizmo's vertical exaggeration (the view's Scene::ve) */
	double elevation = 0.0;
	double azimuth = 0.0;
	double exaggeration = 1.0;
	int gl_width = 0, gl_height = 0;

	/* button parameters */
	bool button1down = false;
	int button_down_x = 0, button_down_y = 0, button_move_x = 0, button_move_y = 0, button_up_x = 0, button_up_y = 0;

	/* edit grab parameters */
	bool grab_start_defined = false, grab_end_defined = false;
	int grab_start_x = 0, grab_start_y = 0, grab_end_x = 0, grab_end_y = 0;

	/* patch test parameters */
	int irollbias = 0, ipitchbias = 0, iheadingbias = 0, itimelag = 0, isnell = 10000;

	/* view parameters */
	bool view_boundingbox = true;
	bool view_flagged = true;
	bool view_secondary = false;
	int view_profiles = MBS_VIEW_PROFILES_NONE;
	bool view_scalewithflagged = true;
	int view_color = MBS_VIEW_COLOR_FLAG;

	/* last sounding edited */
	bool last_sounding_defined = false;
	int last_sounding_edited = 0;

	int key_g_down = 0, key_z_down = 0, key_s_down = 0, key_a_down = 0, key_d_down = 0;

	// PANE MODE (mb3dsdgOpenPane): no window of its own. The soundings are drawn INTO the iGMT window's
	// own cloud actor (pointsActor borrows it), in TRUE coords (glx/gly/glz = lon, lat, z) under that
	// actor's scale; the other actors take the same scale before each frame. The controls are a narrow
	// dock of that window; `win` is built but never shown (its View menu and actions are reused).
	bool pane = false;
	QPointer<QWidget> paneCanvas;            // the window's view, while it lives (its cursor is reset at close)
	QCursor editCursor;                      // the armed mode's cursor (msUpdateCursor), held on the view by
	bool editCursorOn = false;               // cursorWatch whoever else sets one
	QPointer<QObject> cursorWatch;
	double paneKz = 0.0;                     // the window's z scale at the last frame (msVeCB's camera follow)
	bool shiftGrab = false;                  // a Shift+left-drag is running Grab; shiftGrabPrev comes back after
	int shiftGrabPrev = -1;
	void *scene = nullptr;
	QWidget *paneDock = nullptr;
	QRadioButton *paneMode[7] = {};           // Navigate + the six edit modes
	QLabel *paneStatus = nullptr;
	std::function<void(bool)> navShow;             // the window's navigation lines on / off (mb3dsdgSetNavToggle)
	vtkSmartPointer<vtkPolyData> cloudOrigInput;   // the cloud actor's own data and colouring, put back at close
	int cloudOrigScalarMode = 0, cloudOrigColorMode = 0, cloudOrigScalarVis = 1;
	unsigned long startTag = 0;
};
Mb3dsdg *g_ms = nullptr;

double msDpr() {
	return (g_ms && g_ms->win) ? g_ms->win->devicePixelRatioF() : 1.0;
}

void msPlot();
void msUpdateStatus();
void msUpdateModeToggles();
void msUpdateCursor();

void msBeep() {
	QApplication::beep();
}

// the notify the edit functions end with: flush the caller's pending edits
void msFlushPrevious() {
	if (g_ms->notify.edit)
		(g_ms->notify.edit)(0, 0, 0, MB_FLAG_NULL, MB3DSDG_EDIT_FLUSHPREVIOUS);
}

// PANE MODE: a sounding's drawn position is its own TRUE position -- x, y = the beam's lon/lat (the
// pane's soundings carry them there, mb3dsdgOpenCloud) and its z -- since the iGMT window's cloud
// actor that draws it carries the window's scale.
void msPaneTrue(mb3dsoundings_sounding_struct *sounding) {
	sounding->glx = (float)sounding->x;
	sounding->gly = (float)sounding->y;
	sounding->glz = (float)sounding->z;
}

// ---- mb3dsoundings_scale / _scalez / _setzscale ------------------------------------------------
void msScale() {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	if (g_ms->pane) {
		for (int i = 0; i < soundingdata->num_soundings; i++)
			msPaneTrue(&soundingdata->soundings[i]);
		return;
	}
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		sounding->glx = (float)(soundingdata->scale * sounding->x);
		sounding->gly = (float)(soundingdata->scale * sounding->y);
		sounding->glz = (float)(g_ms->exaggeration * soundingdata->zscale * (sounding->z - soundingdata->zorigin));
	}
}

void msScaleZ() {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	if (g_ms->pane)                          // true z under the window's own exaggeration
		return;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		sounding->glz = (float)(g_ms->exaggeration * soundingdata->zscale * (sounding->z - soundingdata->zorigin));
	}
}

// A FLAGGED sounding may be drawn: always when the user flagged it here (it was good when the data came
// in) -- an edit marks a sounding, it never makes it vanish -- and, for one that came in already flagged,
// when "Show flagged" is on. The one rule every colour mode, the box and the pick tests use.
static bool msFlagVisible(const Mb3dsdg *m, const mb3dsoundings_sounding_struct *sounding) {
	return m->view_flagged || mb_beam_ok(sounding->beamflagorg);
}

// Is this sounding drawn by msBuildScene in the current view? The SAME tests its point loops make
// (each colour mode's own), so the box can never hold a
// sounding the view does not show.
static bool msSoundingShown(const Mb3dsdg *m, const mb3dsoundings_sounding_struct *sounding) {
	if (mb_beam_ok(sounding->beamflag))
		return true;
	if (!msFlagVisible(m, sounding))
		return false;
	if (m->view_color == MBS_VIEW_COLOR_FLAG)
		return mb_beam_check_flag_manual(sounding->beamflag) || mb_beam_check_flag_filter(sounding->beamflag) ||
		       mb_beam_check_flag_filter2(sounding->beamflag) || mb_beam_check_flag_sonar(sounding->beamflag) ||
		       (m->view_secondary && mb_beam_check_flag_multipick(sounding->beamflag));
	return !mb_beam_check_flag_null(sounding->beamflag) &&
	       (m->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag));
}

// The vertical box hugs the soundings the view DRAWS (only the unflagged ones of those when
// "scale with flagged" is off): a hidden sounding -- a flagged spike, a secondary pick -- must not
// stretch the box with empty space that every exaggeration then multiplies.
void msSetZScale() {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;

	/* initialize zmin and zmax */
	double zmin = 0.0;
	double zmax = 0.0;

	/* get vertical min maxes of the shown soundings */
	int nused = 0;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		const mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		if (!msSoundingShown(g_ms, sounding))
			continue;
		if (!g_ms->view_scalewithflagged && !mb_beam_ok(sounding->beamflag))
			continue;
		if (nused == 0) {
			zmin = sounding->z;
			zmax = sounding->z;
		}
		else {
			zmin = MIN(sounding->z, zmin);
			zmax = MAX(sounding->z, zmax);
		}
		nused++;
	}

	soundingdata->zorigin = 0.5 * (zmin + zmax);
	soundingdata->zmin = -0.5 * (zmax - zmin);
	soundingdata->zmax = 0.5 * (zmax - zmin);
	if (g_ms->pane) {                        // true z: drawn under the window's own exaggeration -- and the
		                                     // window's box is sized by these same soundings, not by hidden ones
		if (nused > 1 && g_ms->host.attachZRange)
			g_ms->host.attachZRange(g_ms->view, zmin, zmax);
		return;
	}
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		soundingdata->soundings[i].glz =
		    (float)(g_ms->exaggeration * soundingdata->zscale * (soundingdata->soundings[i].z - soundingdata->zorigin));
	}
}

// ---- the edit functions (mb3dsoundings_pick ... _good_ping), verbatim but for XBell ------------
void msPick(int x, int y) {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	double rmin = 10000.0;
	int irmin = 0;
	bool editevent = false;
	mb3dsoundings_sounding_struct *sounding = nullptr;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		sounding = &(soundingdata->soundings[i]);
		const double dx = (double)(x - sounding->winx);
		const double dy = (double)(y - sounding->winy);
		const double r = sqrt(dx * dx + dy * dy);
		if (r < rmin && (g_ms->edit_mode == MBS_EDIT_TOGGLE || mb_beam_ok(sounding->beamflag))) {
			irmin = i;
			rmin = r;
		}
	}
	if (rmin < MBS_PICK_THRESHOLD * msDpr()) {
		sounding = &(soundingdata->soundings[irmin]);
		if (g_ms->edit_mode == MBS_EDIT_TOGGLE) {
			if (mb_beam_ok(sounding->beamflag)) {
				if (sounding->beamflag != sounding->beamflagorg)
					sounding->beamflag = sounding->beamflagorg;
				else
					sounding->beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
				soundingdata->num_soundings_unflagged--;
				soundingdata->num_soundings_flagged++;
				editevent = true;

				/* last sounding edited */
				g_ms->last_sounding_defined = true;
				g_ms->last_sounding_edited = irmin;
			}
			else if (g_ms->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag)) {
				sounding->beamflag = MB_FLAG_NONE;
				soundingdata->num_soundings_unflagged++;
				soundingdata->num_soundings_flagged--;
				editevent = true;

				/* last sounding edited */
				g_ms->last_sounding_defined = true;
				g_ms->last_sounding_edited = irmin;
			}
		}
		else if (g_ms->edit_mode == MBS_EDIT_PICK) {
			if (mb_beam_ok(sounding->beamflag)) {
				if (sounding->beamflag != sounding->beamflagorg)
					sounding->beamflag = sounding->beamflagorg;
				else
					sounding->beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
				soundingdata->num_soundings_unflagged--;
				soundingdata->num_soundings_flagged++;
				editevent = true;

				/* last sounding edited */
				g_ms->last_sounding_defined = true;
				g_ms->last_sounding_edited = irmin;
			}
		}
	}
	else {
		msBeep();
	}

	/* replot the data */
	if (editevent) {
		msPlot();
		msUpdateStatus();
	}

	/* communicate edit event back to calling application */
	if (editevent && g_ms->notify.edit) {
		(g_ms->notify.edit)(sounding->ifile, sounding->iping, sounding->ibeam, sounding->beamflag, MB3DSDG_EDIT_FLUSH);
	}
}

void msEraseRestore(int x, int y) {
	int neditevent = 0;
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		const double dx = x - sounding->winx;
		const double dy = y - sounding->winy;
		const double r = sqrt(dx * dx + dy * dy);
		if (r < MBS_ERASE_THRESHOLD * msDpr()) {
			bool editevent = false;
			if (g_ms->edit_mode == MBS_EDIT_ERASE && mb_beam_ok(sounding->beamflag)) {
				if (sounding->beamflag != sounding->beamflagorg)
					sounding->beamflag = sounding->beamflagorg;
				else
					sounding->beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
				soundingdata->num_soundings_unflagged--;
				soundingdata->num_soundings_flagged++;
				editevent = true;

				/* last sounding edited */
				g_ms->last_sounding_defined = true;
				g_ms->last_sounding_edited = i;
			}
			else if (g_ms->edit_mode == MBS_EDIT_RESTORE && !mb_beam_ok(sounding->beamflag) &&
			         (g_ms->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag))) {
				sounding->beamflag = MB_FLAG_NONE;
				soundingdata->num_soundings_unflagged++;
				soundingdata->num_soundings_flagged--;
				editevent = true;

				/* last sounding edited */
				g_ms->last_sounding_defined = true;
				g_ms->last_sounding_edited = i;
			}

			/* handle valid edit event */
			if (editevent) {
				neditevent++;

				/* communicate edit event back to calling application */
				if (g_ms->notify.edit)
					(g_ms->notify.edit)(sounding->ifile, sounding->iping, sounding->ibeam, sounding->beamflag,
					                    MB3DSDG_EDIT_NOFLUSH);
			}
		}
	}

	/* replot and flush the edit events in the calling application */
	if (neditevent > 0) {
		/* replot the data */
		msPlot();
		msUpdateStatus();

		/* flush the edit events in the calling application */
		msFlushPrevious();
	}
}

void msGrab(int x, int y, int grabmode) {
	/* save grab start point */
	if (grabmode == MBS_EDIT_GRAB_START) {
		/* set grab parameters */
		g_ms->grab_start_defined = true;
		g_ms->grab_end_defined = false;
		g_ms->grab_start_x = x;
		g_ms->grab_start_y = y;
		g_ms->grab_end_x = x;
		g_ms->grab_end_y = y;

		/* replot the data */
		msPlot();
	}

	/* save grab end point */
	else if (grabmode == MBS_EDIT_GRAB_MOVE) {
		/* set grab parameters */
		g_ms->grab_end_defined = true;
		g_ms->grab_end_x = x;
		g_ms->grab_end_y = y;

		/* replot the data */
		msPlot();
	}

	/* apply grab */
	else if (grabmode == MBS_EDIT_GRAB_END) {
		/* loop over all soundings */
		int neditevent = 0;
		const int xmin = MIN(g_ms->grab_start_x, g_ms->grab_end_x);
		const int xmax = MAX(g_ms->grab_start_x, g_ms->grab_end_x);
		const int ymin = MIN(g_ms->grab_start_y, g_ms->grab_end_y);
		const int ymax = MAX(g_ms->grab_start_y, g_ms->grab_end_y);
		mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
		for (int i = 0; i < soundingdata->num_soundings; i++) {
			mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
			if (sounding->winx >= xmin && sounding->winx <= xmax && sounding->winy >= ymin && sounding->winy <= ymax) {
				if (mb_beam_ok(sounding->beamflag)) {
					if (sounding->beamflag != sounding->beamflagorg)
						sounding->beamflag = sounding->beamflagorg;
					else
						sounding->beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
					soundingdata->num_soundings_unflagged--;
					soundingdata->num_soundings_flagged++;
					neditevent++;

					/* last sounding edited */
					g_ms->last_sounding_defined = true;
					g_ms->last_sounding_edited = i;

					/* communicate edit event back to calling application */
					if (g_ms->notify.edit)
						(g_ms->notify.edit)(sounding->ifile, sounding->iping, sounding->ibeam, sounding->beamflag,
						                    MB3DSDG_EDIT_NOFLUSH);
				}
			}
		}

		/* grab done so unset grab flags */
		g_ms->grab_start_defined = false;
		g_ms->grab_end_defined = false;

		/* replot and flush the edit events in the calling application */
		if (neditevent > 0) {
			/* replot the data */
			msPlot();
			msUpdateStatus();

			/* flush the edit events in the calling application */
			msFlushPrevious();
		}
		else
			msPlot();
	}
}

void msUnflagView() {
	int neditevent = 0;
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		if (!mb_beam_ok(sounding->beamflag)) {
			sounding->beamflag = MB_FLAG_NONE;
			soundingdata->num_soundings_unflagged++;
			soundingdata->num_soundings_flagged--;
			neditevent++;

			/* communicate edit event back to calling application */
			if (g_ms->notify.edit)
				(g_ms->notify.edit)(sounding->ifile, sounding->iping, sounding->ibeam, sounding->beamflag, MB3DSDG_EDIT_NOFLUSH);
		}
	}

	/* last sounding edited */
	g_ms->last_sounding_defined = false;
	g_ms->last_sounding_edited = 0;

	/* replot and flush the edit events in the calling application */
	if (neditevent > 0) {
		msPlot();
		msUpdateStatus();
		msFlushPrevious();
	}
}

void msFlagView() {
	int neditevent = 0;
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		if (mb_beam_ok(sounding->beamflag)) {
			if (sounding->beamflag != sounding->beamflagorg)
				sounding->beamflag = sounding->beamflagorg;
			else
				sounding->beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
			soundingdata->num_soundings_unflagged--;
			soundingdata->num_soundings_flagged++;
			neditevent++;

			/* communicate edit event back to calling application */
			if (g_ms->notify.edit)
				(g_ms->notify.edit)(sounding->ifile, sounding->iping, sounding->ibeam, sounding->beamflag, MB3DSDG_EDIT_NOFLUSH);
		}
	}

	/* last sounding edited */
	g_ms->last_sounding_defined = false;
	g_ms->last_sounding_edited = 0;

	/* replot and flush the edit events in the calling application */
	if (neditevent > 0) {
		msPlot();
		msUpdateStatus();
		msFlushPrevious();
	}
}

void msInfo(int x, int y) {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	double rmin = 10000.0;
	int irmin = 0;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		const double dx = (double)(x - sounding->winx);
		const double dy = (double)(y - sounding->winy);
		const double r = sqrt(dx * dx + dy * dy);
		if (r < rmin && (g_ms->edit_mode == MBS_EDIT_TOGGLE || mb_beam_ok(sounding->beamflag))) {
			irmin = i;
			rmin = r;
		}
	}
	if (rmin < MBS_PICK_THRESHOLD * msDpr()) {
		/* select closest sounding */
		g_ms->last_sounding_defined = true;
		g_ms->last_sounding_edited = irmin;

		msPlot();
		msUpdateStatus();
	}
	else {
		msBeep();
	}
}

// the five ping macros share one shape: every sounding of the last-edited sounding's ping that
// `inPing` admits gets `change`d, then the caller is notified and flushed
enum PingAction { PING_BAD, PING_ZERO, PING_LEFT, PING_RIGHT, PING_GOOD };

void msPingAction(PingAction action) {
	int neditevent = 0;
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	if (g_ms->last_sounding_defined && g_ms->last_sounding_edited < soundingdata->num_soundings) {
		/* loop over all soundings */
		mb3dsoundings_sounding_struct *lastsounding = &(soundingdata->soundings[g_ms->last_sounding_edited]);
		for (int i = 0; i < soundingdata->num_soundings; i++) {
			mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
			if (sounding->ifile != lastsounding->ifile || sounding->iping != lastsounding->iping)
				continue;
			if (action == PING_ZERO) {
				if (mb_beam_ok(sounding->beamflag))
					soundingdata->num_soundings_unflagged--;
				if (!mb_beam_ok(sounding->beamflag))
					soundingdata->num_soundings_flagged--;
				sounding->beamflag = MB_FLAG_NULL;
			}
			else if (action == PING_GOOD) {
				if (mb_beam_ok(sounding->beamflag))
					continue;
				sounding->beamflag = MB_FLAG_NONE;
				soundingdata->num_soundings_unflagged++;
				soundingdata->num_soundings_flagged--;
			}
			else {
				if (!mb_beam_ok(sounding->beamflag))
					continue;
				if (action == PING_LEFT && sounding->ibeam > lastsounding->ibeam)
					continue;
				if (action == PING_RIGHT && sounding->ibeam < lastsounding->ibeam)
					continue;
				if (sounding->beamflag != sounding->beamflagorg)
					sounding->beamflag = sounding->beamflagorg;
				else
					sounding->beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
				soundingdata->num_soundings_unflagged--;
				soundingdata->num_soundings_flagged++;
			}
			neditevent++;

			/* communicate edit event back to calling application */
			if (g_ms->notify.edit)
				(g_ms->notify.edit)(sounding->ifile, sounding->iping, sounding->ibeam, sounding->beamflag, MB3DSDG_EDIT_NOFLUSH);
		}
	}
	else if (action != PING_LEFT && action != PING_RIGHT) {
		msBeep();
	}

	/* replot and flush the edit events in the calling application */
	if (neditevent > 0) {
		msPlot();
		msUpdateStatus();
		msFlushPrevious();
	}
	else if (action == PING_LEFT || action == PING_RIGHT) {
		msBeep();
	}
}

// ---- the drawing (mb3dsoundings_plot) ----------------------------------------------------------
void msAddPoint(vtkPoints *pts, vtkCellArray *verts, vtkUnsignedCharArray *rgb, const mb3dsoundings_sounding_struct *s,
                float r, float g, float b) {
	const vtkIdType id = pts->InsertNextPoint(s->glx, s->gly, s->glz);
	verts->InsertNextCell(1, &id);
	const unsigned char c[3] = {(unsigned char)lround(255 * r), (unsigned char)lround(255 * g), (unsigned char)lround(255 * b)};
	rgb->InsertNextTypedTuple(c);
}

// A FLAGGED sounding, in every colour mode, is drawn in the colour of its flag -- manual red, filter
// blue, sonar green, secondary pick cyan (the Color by Flag State colours) -- so a pick shows as a
// change of colour, never as a sounding that blends into the depth / amplitude colours or vanishes.
// true = it was a flagged sounding and was added (the caller then skips its own colour).
bool msAddFlagged(const Mb3dsdg *m, vtkPoints *pts, vtkCellArray *verts, vtkUnsignedCharArray *rgb,
                  const mb3dsoundings_sounding_struct *s) {
	if (mb_beam_ok(s->beamflag))
		return false;
	if (mb_beam_check_flag_multipick(s->beamflag) && !m->view_secondary)
		return true;                         // secondary picks: shown only when asked (View menu)
	if (mb_beam_check_flag_manual(s->beamflag))
		msAddPoint(pts, verts, rgb, s, 1.0f, 0.0f, 0.0f);
	else if (mb_beam_check_flag_filter(s->beamflag) || mb_beam_check_flag_filter2(s->beamflag))
		msAddPoint(pts, verts, rgb, s, 0.0f, 0.0f, 1.0f);
	else if (mb_beam_check_flag_sonar(s->beamflag))
		msAddPoint(pts, verts, rgb, s, 0.0f, 1.0f, 0.0f);
	else if (m->view_secondary && mb_beam_check_flag_multipick(s->beamflag))
		msAddPoint(pts, verts, rgb, s, 0.0f, 1.0f, 1.0f);
	else
		msAddPoint(pts, verts, rgb, s, 1.0f, 0.0f, 0.0f);   // any other flag: shown flagged, never hidden
	return true;
}

// the composite transform world -> window pixel (origin bottom left), as gluProject applied it
void msProject(const double m[16], double x, double y, double z, int *wx, int *wy) {
	const double X = m[0] * x + m[1] * y + m[2] * z + m[3];
	const double Y = m[4] * x + m[5] * y + m[6] * z + m[7];
	const double W = m[12] * x + m[13] * y + m[14] * z + m[15];
	const double nx = (W != 0.0) ? X / W : X;
	const double ny = (W != 0.0) ? Y / W : Y;
	*wx = (int)(0.5 * (nx + 1.0) * g_ms->gl_width);
	*wy = (int)(0.5 * (ny + 1.0) * g_ms->gl_height);
}

// The view's direction in mb3dsoundings' own terms, read off the CAMERA (iGMT's navigation moves it):
// elevation 90 = looking straight down, 0 = level; azimuth = the bearing the view looks toward.
void msCameraAngles() {
	Mb3dsdg *m = g_ms;
	vtkCamera *cam = m->ren->GetActiveCamera();
	double pos[3], foc[3];
	cam->GetPosition(pos);
	cam->GetFocalPoint(foc);
	const double d[3] = {foc[0] - pos[0], foc[1] - pos[1], foc[2] - pos[2]};
	const double h = sqrt(d[0] * d[0] + d[1] * d[1]);
	const double deg = 57.29577951308232;
	m->elevation = atan2(-d[2], h) * deg;
	m->azimuth = atan2(d[0], d[1]) * deg;
	if (m->azimuth < 0.0)
		m->azimuth += 360.0;
}

// the window pixel of every sounding, through the camera AS IT IS NOW: the edit functions read them,
// so this runs right before each edit gesture (the view may have moved since the last draw)
void msProjectAll() {
	Mb3dsdg *m = g_ms;
	if (!m || !m->soundingdata || !m->rw)
		return;
	const int *sz = m->rw->GetSize();
	m->gl_width = sz[0] > 0 ? sz[0] : 1;
	m->gl_height = sz[1] > 0 ? sz[1] : 1;
	vtkMatrix4x4 *proj = m->ren->GetActiveCamera()->GetCompositeProjectionTransformMatrix(
	    (double)m->gl_width / (double)m->gl_height, -1.0, 1.0);
	double mm[16];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			mm[4 * r + c] = proj->GetElement(r, c);
	mb3dsoundings_struct *soundingdata = m->soundingdata;
	double sc[3] = {1.0, 1.0, 1.0};          // pane mode: true coords under the window's cloud-actor scale
	if (m->pane && m->pointsActor)
		m->pointsActor->GetScale(sc);
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		msProject(mm, sc[0] * sounding->glx, sc[1] * sounding->gly, sc[2] * sounding->glz, &sounding->winx, &sounding->winy);
	}
}

// the actors from the current soundings; no render (the view's StartEvent calls it too)
void msBuildScene() {
	Mb3dsdg *m = g_ms;
	if (!m || !m->view || !m->soundingdata)
		return;
	mb3dsoundings_struct *soundingdata = m->soundingdata;
	msCameraAngles();

	/* Plot the bounding box if desired */
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> solid, dotted;
		if (m->view_boundingbox) {
			double glxmin = soundingdata->scale * soundingdata->xmin;
			double glxmax = soundingdata->scale * soundingdata->xmax;
			double glymin = soundingdata->scale * soundingdata->ymin;
			double glymax = soundingdata->scale * soundingdata->ymax;
			double glzmin = m->exaggeration * soundingdata->zscale * soundingdata->zmin;
			double glzmax = m->exaggeration * soundingdata->zscale * soundingdata->zmax;
			if (m->pane) {                       // the box of the shown soundings, in their true coords
				bool first = true;
				for (int i = 0; i < soundingdata->num_soundings; i++) {
					const mb3dsoundings_sounding_struct *s = &(soundingdata->soundings[i]);
					if (!msSoundingShown(m, s) || (!m->view_scalewithflagged && !mb_beam_ok(s->beamflag)))
						continue;
					if (first) {
						glxmin = glxmax = s->glx;
						glymin = glymax = s->gly;
						glzmin = glzmax = s->glz;
						first = false;
					}
					else {
						glxmin = MIN(glxmin, (double)s->glx);
						glxmax = MAX(glxmax, (double)s->glx);
						glymin = MIN(glymin, (double)s->gly);
						glymax = MAX(glymax, (double)s->gly);
						glzmin = MIN(glzmin, (double)s->glz);
						glzmax = MAX(glzmax, (double)s->glz);
					}
				}
			}
			auto loop = [&](bool full, const double v[4][3]) {
				vtkIdType ids[5];
				for (int k = 0; k < 4; k++)
					ids[k] = pts->InsertNextPoint(v[k]);
				ids[4] = ids[0];
				(full ? solid.GetPointer() : dotted.GetPointer())->InsertNextCell(5, ids);
			};
			const double el = m->elevation, az = m->azimuth;
			const bool upright = (el >= -90.0 && el <= 90.0);
			const double bottomFace[4][3] = {{glxmin, glymin, glzmin}, {glxmax, glymin, glzmin}, {glxmax, glymax, glzmin}, {glxmin, glymax, glzmin}};
			loop(el <= 0.0, bottomFace);
			const double topFace[4][3] = {{glxmin, glymin, glzmax}, {glxmax, glymin, glzmax}, {glxmax, glymax, glzmax}, {glxmin, glymax, glzmax}};
			loop(el >= 0.0, topFace);
			const bool f3 = ((az >= 0.0 && az <= 90.0) || (az >= 270.0 && az <= 360.0)) ? upright : !upright;
			const double face3[4][3] = {{glxmin, glymin, glzmin}, {glxmax, glymin, glzmin}, {glxmax, glymin, glzmax}, {glxmin, glymin, glzmax}};
			loop(f3, face3);
			const bool f4 = (az >= 180.0 && az <= 360.0) ? upright : !upright;
			const double face4[4][3] = {{glxmax, glymin, glzmin}, {glxmax, glymax, glzmin}, {glxmax, glymax, glzmax}, {glxmax, glymin, glzmax}};
			loop(f4, face4);
			const bool f5 = (az >= 90.0 && az <= 270.0) ? upright : !upright;
			const double face5[4][3] = {{glxmax, glymax, glzmin}, {glxmin, glymax, glzmin}, {glxmin, glymax, glzmax}, {glxmax, glymax, glzmax}};
			loop(f5, face5);
			const bool f6 = ((az >= 0.0 && az <= 180.0) || (az >= 0.0 && az <= 90.0)) ? upright : !upright;
			const double face6[4][3] = {{glxmin, glymax, glzmin}, {glxmin, glymin, glzmin}, {glxmin, glymin, glzmax}, {glxmin, glymax, glzmax}};
			loop(f6, face6);
		}
		vtkNew<vtkPolyData> pdSolid, pdDot;
		pdSolid->SetPoints(pts);
		pdSolid->SetLines(solid);
		pdDot->SetPoints(pts);
		pdDot->SetLines(dotted);
		vtkPolyDataMapper::SafeDownCast(m->boxSolidActor->GetMapper())->SetInputData(pdSolid);
		vtkPolyDataMapper::SafeDownCast(m->boxDotActor->GetMapper())->SetInputData(pdDot);
	}

	/* Plot the profiles if desired */
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		vtkNew<vtkUnsignedCharArray> rgb;
		rgb->SetNumberOfComponents(3);
		if (m->view_profiles != MBS_VIEW_PROFILES_NONE) {
			for (int i = 0; i < soundingdata->num_soundings - 1; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
				mb3dsoundings_sounding_struct *sounding2 = &(soundingdata->soundings[i + 1]);

				/* plot segment only if soundings are from the same ping and profile */
				if (sounding2->ifile == sounding->ifile && sounding->iping == sounding2->iping &&
				    sounding->iprofile == sounding2->iprofile) {
					unsigned char c[3];
					/* plot in black if both soundings are good */
					if (mb_beam_ok(sounding->beamflag) && mb_beam_ok(sounding2->beamflag)) {
						c[0] = c[1] = c[2] = 0;
					}
					/* else plot in red if flagged profiles are desired */
					else if (m->view_profiles == MBS_VIEW_PROFILES_ALL) {
						c[0] = 255;
						c[1] = c[2] = 0;
					}
					else
						continue;
					vtkIdType ids[2];
					ids[0] = pts->InsertNextPoint(sounding->glx, sounding->gly, sounding->glz);
					ids[1] = pts->InsertNextPoint(sounding2->glx, sounding2->gly, sounding2->glz);
					lines->InsertNextCell(2, ids);
					rgb->InsertNextTypedTuple(c);
				}
			}
		}
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetLines(lines);
		pd->GetCellData()->SetScalars(rgb);
		vtkPolyDataMapper::SafeDownCast(m->profileActor->GetMapper())->SetInputData(pd);
	}

	/* Plot the soundings */
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> verts;
		vtkNew<vtkUnsignedCharArray> rgb;
		rgb->SetNumberOfComponents(3);

		/* Color by flag state */
		if (m->view_color == MBS_VIEW_COLOR_FLAG) {
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);

				/* plot unflagged sounding */
				if (mb_beam_ok(sounding->beamflag)) {
					const int c = (sounding->beamcolor >= 0 && sounding->beamcolor < MBV_NUM_COLORS) ? sounding->beamcolor : 0;
					msAddPoint(pts, verts, rgb, sounding, colortable_object_red[c], colortable_object_green[c],
					           colortable_object_blue[c]);
				}

				/* plot flagged sounding if requested */
				else if (msFlagVisible(m, sounding)) {
					msAddFlagged(m, pts, verts, rgb, sounding);   // the same flag colours every mode uses
				}
			}
		}

		/* Color by topography */
		else if (m->view_color == MBS_VIEW_COLOR_TOPO) {
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);

				/* plot sounding: good, or flagged when flagged soundings are shown. The original passed
				   "beamflag || view_secondary" into the unparenthesised multipick macro, which made EVERY
				   flagged sounding a secondary pick and hid it in this colour mode (and the two below) */
				if (mb_beam_ok(sounding->beamflag) ||
				    (msFlagVisible(m, sounding) && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (m->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag)))) {
					if (!msAddFlagged(m, pts, verts, rgb, sounding))
						msAddPoint(pts, verts, rgb, sounding, sounding->r, sounding->g, sounding->b);
				}
			}
		}

		/* Color by soundings - stretch the topography color scale over the
		   range of soundings currently displayed in the 3D Soundings view */
		else if (m->view_color == MBS_VIEW_COLOR_SOUNDING) {
			double zmin = 0.0;
			double zmax = 0.0;
			bool first = true;
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
				if (msFlagVisible(m, sounding) || mb_beam_ok(sounding->beamflag)) {
					if (first) {
						first = false;
						zmin = sounding->z;
						zmax = sounding->z;
					}
					else {
						zmin = MIN(zmin, sounding->z);
						zmax = MAX(zmax, sounding->z);
					}
				}
			}
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
				if (mb_beam_ok(sounding->beamflag) ||
				    (msFlagVisible(m, sounding) && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (m->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag)))) {
					if (msAddFlagged(m, pts, verts, rgb, sounding))
						continue;
					float r, g, b;
					mbviewGetColor(sounding->z, zmin, zmax, colortable_haxby_red[0], colortable_haxby_green[0],
					               colortable_haxby_blue[0], colortable_haxby_red[MBV_NUM_COLORS - 1],
					               colortable_haxby_green[MBV_NUM_COLORS - 1], colortable_haxby_blue[MBV_NUM_COLORS - 1],
					               colortable_haxby_red, colortable_haxby_green, colortable_haxby_blue, &r, &g, &b);
					msAddPoint(pts, verts, rgb, sounding, r, g, b);
				}
			}
		}

		/* Color by amplitude */
		else if (m->view_color == MBS_VIEW_COLOR_AMP) {
			double ampmin = 0.0;
			double ampmax = 0.0;
			bool first = true;
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
				if (mb_beam_ok(sounding->beamflag) ||
				    (msFlagVisible(m, sounding) && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (m->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag)))) {
					if (first) {
						first = false;
						ampmin = sounding->a;
						ampmax = sounding->a;
					}
					else {
						ampmin = MIN(ampmin, sounding->a);
						ampmax = MAX(ampmax, sounding->a);
					}
				}
			}
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
				if (mb_beam_ok(sounding->beamflag) ||
				    (msFlagVisible(m, sounding) && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (m->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag)))) {
					if (msAddFlagged(m, pts, verts, rgb, sounding))
						continue;
					float r, g, b;
					mbviewGetColor(sounding->a, ampmin, ampmax, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, colortable_redtoblue_red,
					               colortable_redtoblue_green, colortable_redtoblue_blue, &r, &g, &b);
					msAddPoint(pts, verts, rgb, sounding, r, g, b);
				}
			}
		}

		/* iGMT: Color by file - one colour per swath file, so overlapping lines tell apart. The palette
		   avoids the flag colours (red manual, blue filter, green sonar), which flagged soundings keep */
		else if (m->view_color == MBS_VIEW_COLOR_FILE) {
			static const float pal[12][3] = {
				{1.00f, 0.50f, 0.05f}, {0.58f, 0.40f, 0.74f}, {0.55f, 0.34f, 0.29f}, {0.89f, 0.47f, 0.76f},
				{0.74f, 0.74f, 0.13f}, {0.09f, 0.75f, 0.81f}, {0.50f, 0.50f, 0.50f}, {0.99f, 0.75f, 0.44f},
				{0.40f, 0.76f, 0.65f}, {0.80f, 0.60f, 0.20f}, {0.65f, 0.81f, 0.89f}, {0.20f, 0.20f, 0.20f}};
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
				if (mb_beam_ok(sounding->beamflag) ||
				    (msFlagVisible(m, sounding) && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (m->view_secondary || !mb_beam_check_flag_multipick(sounding->beamflag)))) {
					if (msAddFlagged(m, pts, verts, rgb, sounding))
						continue;
					const float *c = pal[(sounding->ifile >= 0 ? sounding->ifile : 0) % 12];
					msAddPoint(pts, verts, rgb, sounding, c[0], c[1], c[2]);
				}
			}
		}
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetVerts(verts);
		// one vertex per sounding: its colour is a POINT colour (a decimated LOD of the pane's cloud
		// actor keeps point data, not cell data)
		pd->GetPointData()->SetScalars(rgb);
		if (auto *pm = vtkPolyDataMapper::SafeDownCast(m->pointsActor->GetMapper())) {
			pm->SetInputData(pd);
			if (m->pane) {                       // the window's cloud mapper: our colours, as they are
				pm->SetScalarModeToUsePointData();
				pm->SetColorModeToDirectScalars();
				pm->ScalarVisibilityOn();
			}
		}
	}

	/* If in info mode and sounding picked plot it green if view color by flag, black otherwise */
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> verts;
		if (m->edit_mode == MBS_EDIT_INFO && m->last_sounding_defined && m->last_sounding_edited < soundingdata->num_soundings) {
			mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[m->last_sounding_edited]);
			const vtkIdType id = pts->InsertNextPoint(sounding->glx, sounding->gly, sounding->glz);
			verts->InsertNextCell(1, &id);
		}
		if (m->view_color == MBS_VIEW_COLOR_FLAG)
			m->infoActor->GetProperty()->SetColor(0.0, 1.0, 1.0);
		else
			m->infoActor->GetProperty()->SetColor(0.0, 0.0, 0.0);
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetVerts(verts);
		vtkPolyDataMapper::SafeDownCast(m->infoActor->GetMapper())->SetInputData(pd);
	}

	/* the box the view frames: what the gizmo and its view keys fit, what a recentre may land on
	   (pane mode: the window frames its own data) */
	if (!m->pane && m->host.view3dSetBounds) {
		const double b[6] = {soundingdata->scale * soundingdata->xmin, soundingdata->scale * soundingdata->xmax,
		                     soundingdata->scale * soundingdata->ymin, soundingdata->scale * soundingdata->ymax,
		                     m->exaggeration * soundingdata->zscale * soundingdata->zmin,
		                     m->exaggeration * soundingdata->zscale * soundingdata->zmax};
		m->host.view3dSetBounds(m->view, b);
	}

	/* plot grab rectangle (window pixels) */
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		if (m->button1down && m->grab_start_defined && m->grab_end_defined) {
			const int grabxmin = MIN(m->grab_start_x, m->grab_end_x);
			const int grabxmax = MAX(m->grab_start_x, m->grab_end_x);
			const int grabymin = MIN(m->grab_start_y, m->grab_end_y);
			const int grabymax = MAX(m->grab_start_y, m->grab_end_y);
			vtkIdType ids[5];
			ids[0] = pts->InsertNextPoint(grabxmin, grabymin, 0.0);
			ids[1] = pts->InsertNextPoint(grabxmax, grabymin, 0.0);
			ids[2] = pts->InsertNextPoint(grabxmax, grabymax, 0.0);
			ids[3] = pts->InsertNextPoint(grabxmin, grabymax, 0.0);
			ids[4] = ids[0];
			lines->InsertNextCell(5, ids);
		}
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetLines(lines);
		vtkPolyDataMapper2D::SafeDownCast(m->grabActor->GetMapper())->SetInputData(pd);
	}
}

void msPlot() {
	Mb3dsdg *m = g_ms;
	if (!m || !m->view || !m->soundingdata)
		return;
	if (m->pane)                             // an edit may have hidden (or brought back) the extreme soundings:
		msSetZScale();                       // the window's box follows what is shown
	msBuildScene();
	m->ren->ResetCameraClippingRange();
	m->rw->Render();
}

// ---- status labels (mb3dsoundings_updatestatus / _updatelabelmousemode / _updatemodetoggles) -----
void msUpdateStatus() {
	Mb3dsdg *m = g_ms;
	mb3dsoundings_struct *soundingdata = m->soundingdata;
	char value_text[2048];

	/* if in info mode and sounding picked print info as status */
	if (m->edit_mode == MBS_EDIT_INFO && m->last_sounding_defined && m->last_sounding_edited < soundingdata->num_soundings) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[m->last_sounding_edited]);
		value_text[0] = '\0';
		if (m->notify.info)
			(m->notify.info)(sounding->ifile, sounding->iping, sounding->ibeam, value_text);
		m->biasPanel->hide();
	}

	/* else set standard status label */
	else {
		msCameraAngles();
		snprintf(value_text, sizeof(value_text), "Azi:%.2f | Elev: %.2f | exagger:%.2f | Tot:%d Good:%d Flagged:%d", m->azimuth,
		         m->elevation, m->exaggeration, soundingdata->num_soundings, soundingdata->num_soundings_unflagged,
		         soundingdata->num_soundings_flagged);
		m->biasPanel->show();
	}

	/* put up the new status string */
	m->labelStatus->setText(QString::fromLatin1(value_text));
	if (m->paneStatus) {                     // the pane: one item per line, the counts only (the window
		                                     // shows its own view angles and exaggeration)
		if (m->edit_mode == MBS_EDIT_INFO && m->last_sounding_defined &&
		    m->last_sounding_edited < soundingdata->num_soundings)
			m->paneStatus->setText(QString::fromLatin1(value_text).replace(" | ", "\n"));
		else
			m->paneStatus->setText(QString("Tot:%1\nGood:%2\nFlagged:%3").arg(soundingdata->num_soundings)
			                       .arg(soundingdata->num_soundings_unflagged).arg(soundingdata->num_soundings_flagged));
	}
}


void msUpdateCursor() {
	Mb3dsdg *m = g_ms;
	// mb3dsoundings' cursors while an edit mode is armed. Pick, Erase and Restore: a circle as wide as
	// their reach (msToolMouse: Pick takes the nearest sounding within MBS_PICK_THRESHOLD screen pixels,
	// Erase / Restore every one within MBS_ERASE_THRESHOLD). The rest: a cross.
	auto ring = [m](double radius, const QColor &col) {
		const qreal dpr = m->canvasW->devicePixelRatioF();
		const int r = int(radius), side = 2 * r + 4;
		QPixmap pm(int(side * dpr), int(side * dpr));
		pm.setDevicePixelRatio(dpr);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing, true);
		const QPointF c(side / 2.0, side / 2.0);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(Qt::white, 3.0));      // a light rim under a dark line: seen on any ground
		p.drawEllipse(c, r, r);
		p.setPen(QPen(col, 1.4));
		p.drawEllipse(c, r, r);
		p.drawLine(c - QPointF(2, 0), c + QPointF(2, 0));
		p.drawLine(c - QPointF(0, 2), c + QPointF(0, 2));
		p.end();
		return QCursor(pm, side / 2, side / 2);
	};
	QCursor cur(Qt::ArrowCursor);
	const bool armedMode = m->edit_mode >= 0;
	if (m->edit_mode == MBS_EDIT_PICK)
		cur = ring(MBS_PICK_THRESHOLD, QColor(200, 0, 0));
	else if (m->edit_mode == MBS_EDIT_ERASE)
		cur = ring(MBS_ERASE_THRESHOLD, QColor(200, 0, 0));
	else if (m->edit_mode == MBS_EDIT_RESTORE)
		cur = ring(MBS_ERASE_THRESHOLD, QColor(0, 120, 0));
	else if (armedMode)
		cur = QCursor(Qt::CrossCursor);
	// VTK puts its "default" cursor back on the widget at every hover (the view's widgets ask for it),
	// so the edit cursor must BE the view's default -- setCursor alone lasts until the mouse moves
	if (auto *vw = qobject_cast<QVTKOpenGLNativeWidget *>(m->canvasW))
		vw->setDefaultCursor(cur);
	m->editCursor = cur;
	m->editCursorOn = armedMode;
	// ...and whoever else sets one on the view while a mode is armed is overridden: the watcher puts the
	// edit cursor back at every cursor change (and says, once per kind, what replaced it)
	if (!m->cursorWatch) {
		struct CursorWatch : QObject {
			bool busy = false;
			QSet<int> told;
			using QObject::QObject;
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() != QEvent::CursorChange || busy || !g_ms || !g_ms->editCursorOn)
					return false;
				auto *w = qobject_cast<QWidget *>(o);
				if (!w)
					return false;
				const QCursor &want = g_ms->editCursor;
				const QCursor now = w->cursor();
				const bool same = now.shape() == want.shape() &&
				                  (want.shape() != Qt::BitmapCursor || now.pixmap().cacheKey() == want.pixmap().cacheKey());
				if (same)
					return false;
				if (!told.contains(int(now.shape()))) {
					told.insert(int(now.shape()));
					fprintf(stderr, "3D Soundings: the view's cursor was replaced (Qt shape %d) in edit mode %d; put back\n",
					        int(now.shape()), g_ms->edit_mode);
				}
				busy = true;
				w->setCursor(want);
				busy = false;
				return false;
			}
		};
		m->cursorWatch = new CursorWatch(m->canvasW);
		m->canvasW->installEventFilter(m->cursorWatch);
	}
	if (armedMode)
		m->canvasW->setCursor(cur);
	else
		m->canvasW->unsetCursor();
}

void msUpdateModeToggles() {
	Mb3dsdg *m = g_ms;
	for (int i = 0; i < 6; i++) {
		QSignalBlocker b(m->modeButton[i]);
		m->modeButton[i]->setChecked(i == m->edit_mode);
	}
	for (int i = 0; i < 7; i++)              // the pane: [0] Navigate = no edit mode, then the six
		if (m->paneMode[i]) {
			QSignalBlocker b(m->paneMode[i]);
			m->paneMode[i]->setChecked(i == m->edit_mode + 1);
		}
}

// arm an edit mode (MBS_EDIT_NONE disarms: the left button goes back to the view)
void msSetEditMode(int mode) {
	g_ms->edit_mode = mode;
	msUpdateModeToggles();
	msUpdateCursor();
	msPlot();
	msUpdateStatus();
}

// ---- the bias sliders (do_mb3dsdg_rollbias ... _snell) -------------------------------------------
void msSetSliderValue(QSlider *s, QLabel *val, int v, double unit, int decimals) {
	QSignalBlocker b(s);
	if (v <= s->minimum() || v >= s->maximum())
		s->setRange(v - 100, v + 100);
	s->setValue(v);
	val->setText(QString::number(v * unit, 'f', decimals));
}

// mb3dsoundings_updategui: the sliders follow the stored integer values
void msUpdateGui() {
	Mb3dsdg *m = g_ms;
	msSetSliderValue(m->scale_rollbias, m->val_rollbias, m->irollbias, 0.01, 2);
	msSetSliderValue(m->scale_pitchbias, m->val_pitchbias, m->ipitchbias, 0.01, 2);
	msSetSliderValue(m->scale_headingbias, m->val_headingbias, m->iheadingbias, 0.01, 2);
	msSetSliderValue(m->scale_timelag, m->val_timelag, m->itimelag, 0.01, 2);
	msSetSliderValue(m->scale_snell, m->val_snell, m->isnell, 0.0001, 4);
}

void msBiasChanged(QSlider *s, int *target, int value) {
	Mb3dsdg *m = g_ms;
	*target = value;

	/* send bias parameters to calling program */
	if (m->notify.bias)
		(m->notify.bias)(0.01 * ((double)m->irollbias), 0.01 * ((double)m->ipitchbias), 0.01 * ((double)m->iheadingbias),
		                 0.01 * ((double)m->itimelag), 0.0001 * ((double)m->isnell));

	/* rescale data to the gl coordinates */
	msScale();
	msSetZScale();

	/* replot the data */
	msPlot();

	/* reset scale min max */
	if (value == s->minimum() || value == s->maximum()) {
		QSignalBlocker b(s);
		s->setRange(value - 100, value + 100);
		s->setValue(value);
	}
}

void msOptimize(int mode) {
	Mb3dsdg *m = g_ms;
	if (!m->notify.optimizebiasvalues)
		return;
	/* get bias parameters */
	double rollbias = 0.01 * ((double)m->irollbias);
	double pitchbias = 0.01 * ((double)m->ipitchbias);
	double headingbias = 0.01 * ((double)m->iheadingbias);
	double timelag = 0.01 * ((double)m->itimelag);
	double snell = 0.0001 * ((double)m->isnell);

	(m->notify.optimizebiasvalues)(mode, &rollbias, &pitchbias, &headingbias, &timelag, &snell);

	/* set the bias parameters stored for the gui */
	m->irollbias = (int)round(100 * rollbias);
	m->ipitchbias = (int)round(100 * pitchbias);
	m->iheadingbias = (int)round(100 * headingbias);
	m->itimelag = (int)round(100 * timelag);
	m->isnell = (int)round(10000 * snell);

	/* update the gui */
	msUpdateGui();

	/* rescale data to the gl coordinates */
	msScale();
	msSetZScale();

	/* replot the data */
	msPlot();
}

// ---- the left button of an ARMED edit mode (do_mb3dsdg_glwda_input, button 1) ---------------------
// The view (MbEditHost::view3dMake) hands it over only while an edit mode is armed, in window pixels
// (device px, origin bottom left), and VTK never sees it. Everything else is iGMT's navigation.
void msToolMouse(int what, int x, int y) {
	Mb3dsdg *m = g_ms;
	if (!m || !m->soundingdata || m->edit_mode < 0)
		return;
	msProjectAll();                      // the pixels of what is on screen NOW (the view may have moved)
	if (what == 0) {                     // ButtonPress
		m->button_down_x = x;
		m->button_down_y = y;
		m->button1down = true;
		if (m->edit_mode == MBS_EDIT_TOGGLE || m->edit_mode == MBS_EDIT_PICK)
			msPick(x, y);
		else if (m->edit_mode == MBS_EDIT_ERASE || m->edit_mode == MBS_EDIT_RESTORE)
			msEraseRestore(x, y);
		else if (m->edit_mode == MBS_EDIT_GRAB)
			msGrab(x, y, MBS_EDIT_GRAB_START);
		else if (m->edit_mode == MBS_EDIT_INFO)
			msInfo(x, y);
	}
	else if (what == 1 && m->button1down) {   // MotionNotify while pressed
		m->button_move_x = x;
		m->button_move_y = y;
		if (m->edit_mode == MBS_EDIT_ERASE || m->edit_mode == MBS_EDIT_RESTORE)
			msEraseRestore(x, y);
		else if (m->edit_mode == MBS_EDIT_GRAB)
			msGrab(x, y, MBS_EDIT_GRAB_MOVE);
	}
	else if (what == 2) {                // ButtonRelease
		m->button_up_x = x;
		m->button_up_y = y;
		if (m->button1down && m->edit_mode == MBS_EDIT_GRAB)
			msGrab(m->button_down_x, m->button_down_y, MBS_EDIT_GRAB_END);
		m->button1down = false;
		msPlot();
	}
}

// ---- the key macros (do_mb3dsdg_glwda_input, KeyPress / KeyRelease) ---------------------------
// Each key reaches the view (iGMT's keys, the gizmo's included) FIRST. The gizmo owns x, c and e, so
// mb3dsoundings' x / c (Flag View / Unflag View) and e (Erase mode) are not macros here: Flag and
// Unflag View keep their other keys (< , and > .) and the Action menu; Erase keeps o.
void msKeyEvent(QKeyEvent *e, bool press) {
	Mb3dsdg *m = g_ms;
	const QString t = e->text();
	if (!m || !m->soundingdata || t.isEmpty())
		return;
	if (press) {
		switch (t[0].toLatin1()) {
		case 'G':
		case 'g':
			m->key_g_down = 1;
			break;
		case 'M':
		case 'm':
		case 'Z':
		case 'z':
			msPingAction(PING_BAD);
			m->key_z_down = 1;
			m->key_s_down = 0;
			m->key_a_down = 0;
			m->key_d_down = 0;
			break;
		case 'K':
		case 'k':
		case 'S':
		case 's':
			msPingAction(PING_GOOD);
			m->key_z_down = 0;
			m->key_s_down = 1;
			m->key_a_down = 0;
			m->key_d_down = 0;
			break;
		case 'J':
		case 'j':
		case 'A':
		case 'a':
			msPingAction(!m->keyreverse_mode ? PING_LEFT : PING_RIGHT);
			m->key_z_down = 0;
			m->key_s_down = 0;
			m->key_a_down = 1;
			m->key_d_down = 0;
			break;
		case 'L':
		case 'l':
		case 'D':
		case 'd':
			msPingAction(!m->keyreverse_mode ? PING_RIGHT : PING_LEFT);
			m->key_z_down = 0;
			m->key_s_down = 0;
			m->key_a_down = 0;
			m->key_d_down = 1;
			break;
		case '<':
		case ',':
			msFlagView();
			break;
		case '>':
		case '.':
			msUnflagView();
			break;
		case '!':
			msPingAction(PING_ZERO);
			break;
		case 'U':
		case 'u':
		case 'Q':
		case 'q':
			msSetEditMode(MBS_EDIT_TOGGLE);
			break;
		case 'I':
		case 'i':
		case 'W':
		case 'w':
			msSetEditMode(MBS_EDIT_PICK);
			break;
		case 'O':
		case 'o':
			msSetEditMode(MBS_EDIT_ERASE);
			break;
		case 'P':
		case 'p':
		case 'R':
		case 'r':
			msSetEditMode(MBS_EDIT_RESTORE);
			break;
		case '{':
		case '[':
		case 'T':
		case 't':
			msSetEditMode(MBS_EDIT_GRAB);
			break;
		case '}':
		case ']':
		case 'Y':
		case 'y':
			msSetEditMode(MBS_EDIT_INFO);
			break;
		default:
			break;
		}
	}
	else {
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
			break;
		}
	}
}

// ---- the window ------------------------------------------------------------------------------
template <typename T>
T *msChild(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

class MsCloseFilter : public QObject {
public:
	explicit MsCloseFilter(QObject *parent) : QObject(parent) {}

protected:
	// do_mb3dsdg_dismiss: the caller is told, then the window goes
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() == QEvent::Close && g_ms && o == g_ms->win) {
			auto dismiss = g_ms->notify.dismiss;
			g_ms->notify = Mb3dsdgNotify();
			if (dismiss)
				dismiss();
		}
		return QObject::eventFilter(o, e);
	}
};

vtkSmartPointer<vtkActor> msMakeActor(double pointSize, double lineWidth) {
	vtkNew<vtkPolyDataMapper> mapper;
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(mapper);
	a->GetProperty()->SetPointSize(pointSize);
	a->GetProperty()->SetLineWidth(lineWidth);
	a->GetProperty()->LightingOff();
	return a;
}

// the gizmo's vertical exaggeration IS the soundings' exaggeration: picked up before every frame
void msVeCB(vtkObject *, unsigned long, void *, void *) {
	Mb3dsdg *m = g_ms;
	if (m && m->pane) {                      // pane: the box/profiles/info take the window's cloud scale
		if (!m->pointsActor)
			return;
		double sc[3];
		m->pointsActor->GetScale(sc);
		for (vtkActor *a : {m->boxSolidActor.Get(), m->boxDotActor.Get(), m->profileActor.Get(), m->infoActor.Get()})
			if (a)
				a->SetScale(sc);
		// The window's exaggeration scales z about 0 (sea level): soundings 3000 m down slide away as it
		// grows and leave the view. So the camera follows the centre of the SHOWN soundings by exactly the
		// distance that centre moved -- they stay where the user is looking, and stretch about themselves.
		if (m->paneKz > 0.0 && sc[2] != m->paneKz && m->ren && m->soundingdata) {
			const double dz = (sc[2] - m->paneKz) * m->soundingdata->zorigin;
			vtkCamera *cam = m->ren->GetActiveCamera();
			double f[3], p[3];
			cam->GetFocalPoint(f);
			cam->GetPosition(p);
			cam->SetFocalPoint(f[0], f[1], f[2] + dz);
			cam->SetPosition(p[0], p[1], p[2] + dz);
			m->ren->ResetCameraClippingRange();
		}
		m->paneKz = sc[2];
		return;
	}
	if (!m || !m->soundingdata || !m->host.view3dVE)
		return;
	const double ve = m->host.view3dVE(m->view);
	if (ve != m->exaggeration) {
		m->exaggeration = ve;
		msScaleZ();
		msBuildScene();                  // no render: this IS the render starting
		msUpdateStatus();
	}
}

// `paneScene`: build for PANE MODE on that iGMT window (its view, its cloud actor); null = own window
Mb3dsdg *msBuild(QWidget *parent, const MbEditHost &host, void *paneScene = nullptr) {
	const QString uiDir = host.uiDir;
	const QIcon icon = host.icon;
	if (paneScene && (!host.view3dAttach || !host.attachCloudActor || !host.addPane)) {
		QMessageBox::warning(parent, "MBeditviz", "The 3-D soundings pane needs the viewer's window hooks.");
		return nullptr;
	}
	if (!paneScene && !host.view3dMake) {
		QMessageBox::warning(parent, "MBeditviz", "The 3-D soundings view needs the viewer's 3-D view.");
		return nullptr;
	}
	QFile f(QDir(uiDir).filePath("mb3dsoundings.ui"));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBeditviz", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	auto *win = qobject_cast<QMainWindow *>(loader.load(&f, nullptr));
	if (!win) {
		QMessageBox::warning(parent, "MBeditviz", QString("Cannot load %1").arg(f.fileName()));
		return nullptr;
	}
	auto *m = new Mb3dsdg;
	m->uiDir = uiDir;
	m->host = host;
	QStringList missing;
	const char *modes[6] = {"modeToggle", "modePick", "modeErase", "modeRestore", "modeGrab", "modeInfo"};
	for (int i = 0; i < 6; i++)
		m->modeButton[i] = msChild<QRadioButton>(win, modes[i], missing);
	m->labelStatus = msChild<QLabel>(win, "labelStatus", missing);
	m->biasPanel = msChild<QWidget>(win, "biasPanel", missing);
	m->scale_rollbias = msChild<QSlider>(win, "rollSlider", missing);
	m->scale_pitchbias = msChild<QSlider>(win, "pitchSlider", missing);
	m->scale_headingbias = msChild<QSlider>(win, "headingSlider", missing);
	m->scale_timelag = msChild<QSlider>(win, "timelagSlider", missing);
	m->scale_snell = msChild<QSlider>(win, "snellSlider", missing);
	m->val_rollbias = msChild<QLabel>(win, "rollSliderValue", missing);
	m->val_pitchbias = msChild<QLabel>(win, "pitchSliderValue", missing);
	m->val_headingbias = msChild<QLabel>(win, "headingSliderValue", missing);
	m->val_timelag = msChild<QLabel>(win, "timelagSliderValue", missing);
	m->val_snell = msChild<QLabel>(win, "snellSliderValue", missing);
	auto *resetButton = msChild<QPushButton>(win, "resetButton", missing);
	m->actViewFlagged = msChild<QAction>(win, "actionViewFlagged", missing);
	m->actViewSecondary = msChild<QAction>(win, "actionViewSecondary", missing);
	m->actNoConnect = msChild<QAction>(win, "actionNoConnect", missing);
	m->actConnectGood = msChild<QAction>(win, "actionConnectGood", missing);
	m->actConnectAll = msChild<QAction>(win, "actionConnectAll", missing);
	m->actBoundingBox = msChild<QAction>(win, "actionBoundingBox", missing);
	m->actScaleWithFlagged = msChild<QAction>(win, "actionScaleWithFlagged", missing);
	m->actColorByFlag = msChild<QAction>(win, "actionColorByFlag", missing);
	m->actColorByTopo = msChild<QAction>(win, "actionColorByTopo", missing);
	m->actColorBySounding = msChild<QAction>(win, "actionColorBySounding", missing);
	m->actColorByAmp = msChild<QAction>(win, "actionColorByAmp", missing);
	auto *actDismiss = msChild<QAction>(win, "actionDismiss", missing);
	auto *actResetView = msChild<QAction>(win, "actionResetView", missing);
	auto *actApplyBias = msChild<QAction>(win, "actionApplyBias", missing);
	const char *voxels[6] = {"actionFlagSparseA", "actionFlagSparseB", "actionFlagSparseC", "actionFlagSparseD",
	                         "actionFlagSparseE", "actionFlagSparseF"};
	QAction *actVoxel[6];
	for (int i = 0; i < 6; i++)
		actVoxel[i] = msChild<QAction>(win, voxels[i], missing);
	const char *colors[7] = {"actionColorBlack", "actionColorRed", "actionColorYellow", "actionColorGreen",
	                         "actionColorBlueGreen", "actionColorBlue", "actionColorPurple"};
	QAction *actColor[7];
	for (int i = 0; i < 7; i++)
		actColor[i] = msChild<QAction>(win, colors[i], missing);
	const char *opts[7] = {"actionOptimizeR", "actionOptimizeP", "actionOptimizeH", "actionOptimizeRP",
	                       "actionOptimizeRPH", "actionOptimizeT", "actionOptimizeS"};
	QAction *actOpt[7];
	for (int i = 0; i < 7; i++)
		actOpt[i] = msChild<QAction>(win, opts[i], missing);
	auto *host_w = msChild<QWidget>(win, "canvasHost", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBeditviz", "mb3dsoundings.ui lacks: " + missing.join(", "));
		delete win;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!icon.isNull())
		win->setWindowIcon(icon);

	// THE VIEWER'S OWN 3-D VIEW: iGMT's mouse, keys and gizmo exactly as every iGMT 3-D view has them.
	// mb3dsoundings' own camera (its model rotation, pan, zoom and mouse modes) is gone; an armed edit
	// mode is the only thing that takes the left button.
	vtkRenderer *ren = nullptr;
	auto armed = []() { return g_ms && g_ms->edit_mode >= 0; };
	// Shift+left-drag in the pane's window (the host hands it over even unarmed): Grab, for that drag
	// only -- the edit mode in force is put back on release
	auto tool = [](int what, int x, int y) {
		if (!g_ms) return;
		if (what == 0 && g_ms->pane && (QApplication::keyboardModifiers() & Qt::ShiftModifier) &&
		    g_ms->edit_mode != MBS_EDIT_GRAB) {
			g_ms->shiftGrabPrev = g_ms->edit_mode;
			g_ms->shiftGrab = true;
			msSetEditMode(MBS_EDIT_GRAB);
		}
		msToolMouse(what, x, y);
		if (what == 2 && g_ms && g_ms->shiftGrab) {
			g_ms->shiftGrab = false;
			msSetEditMode(g_ms->shiftGrabPrev);
		}
	};
	auto key = [](QKeyEvent *e, bool press) { msKeyEvent(e, press); };
	if (paneScene) {
		// PANE MODE: the iGMT window's own view (its navigation, gizmo, keys) takes the same three hooks
		m->view = host.view3dAttach(paneScene, armed, tool, key, &m->canvasW, &ren);
		if (!m->view) {
			delete win;
			delete m;
			return nullptr;
		}
		m->pane = true;
		m->scene = paneScene;
		m->paneCanvas = m->canvasW;
	}
	else {
		auto *lay = new QVBoxLayout(host_w);
		lay->setContentsMargins(0, 0, 0, 0);
		m->view = host.view3dMake(host_w, armed, tool, key, &m->canvasW, &ren);
		lay->addWidget(m->canvasW);
	}
	m->ren = ren;
	m->rw = ren->GetRenderWindow();
	if (!m->pane) {
		// mb3dsoundings' white ground: its colours ARE the flag code (good soundings and the box in black,
		// manual flags red, filter blue, sonar green), and on a dark ground the good ones vanish
		m->ren->GradientBackgroundOff();
		m->ren->SetBackground(1.0, 1.0, 1.0);
	}
	{
		vtkNew<vtkCallbackCommand> veCB;
		veCB->SetCallback(msVeCB);
		m->startTag = m->ren->AddObserver(vtkCommand::StartEvent, veCB);
	}
	m->boxSolidActor = msMakeActor(1.0, 1.0);
	m->boxSolidActor->GetProperty()->SetColor(0.0, 0.0, 0.0);
	m->boxDotActor = msMakeActor(1.0, 1.0);
	m->boxDotActor->GetProperty()->SetColor(0.75, 0.75, 0.75);
	m->profileActor = msMakeActor(1.0, 1.0);
	m->infoActor = msMakeActor(6.0, 1.0);
	if (m->pane) {
		// the soundings go INTO the window's cloud actor: its data and colouring kept to be put back
		m->pointsActor = host.attachCloudActor(m->view);
		auto *pm = m->pointsActor ? vtkPolyDataMapper::SafeDownCast(m->pointsActor->GetMapper()) : nullptr;
		if (!pm) {
			if (host.view3dDetach)
				host.view3dDetach(m->view);
			m->ren->RemoveObserver(m->startTag);
			delete win;
			delete m;
			return nullptr;
		}
		m->cloudOrigInput = pm->GetInput();
		m->cloudOrigScalarMode = pm->GetScalarMode();
		m->cloudOrigColorMode = pm->GetColorMode();
		m->cloudOrigScalarVis = pm->GetScalarVisibility();
		m->boxSolidActor->GetProperty()->SetColor(1.0, 1.0, 1.0);   // on the window's own (dark) ground
	}
	else {
		m->pointsActor = msMakeActor(3.0, 1.0);
		m->ren->AddActor(m->pointsActor);
	}
	for (vtkActor *a : {m->boxSolidActor.Get(), m->boxDotActor.Get(), m->profileActor.Get(), m->infoActor.Get()})
		m->ren->AddActor(a);
	{
		vtkNew<vtkPolyDataMapper2D> m2;
		vtkNew<vtkPolyData> empty;            // a first render may come before the first msPlot
		m2->SetInputData(empty);
		m->grabActor = vtkSmartPointer<vtkActor2D>::New();
		m->grabActor->SetMapper(m2);
		m->grabActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
		m->grabActor->GetProperty()->SetLineWidth(3.0);
		m->ren->AddViewProp(m->grabActor);	// AddActor2D: gone in VTK 9.7 (macOS CI)
	}

	// edit mode toggles, ARMED like iGMT's draw tools: a click arms that mode (the left button edits),
	// a click on the armed one disarms it (the left button navigates again). One armed at a time.
	for (int i = 0; i < 6; i++) {
		m->modeButton[i]->setAutoExclusive(false);
		QObject::connect(m->modeButton[i], &QRadioButton::clicked, win, [i](bool on) {
			if (g_ms)
				msSetEditMode(on ? i : MBS_EDIT_NONE);
		});
	}

	// View menu
	QObject::connect(m->actViewFlagged, &QAction::toggled, win, [](bool on) {
		g_ms->view_flagged = on;
		msSetZScale();                    // the box follows what is shown
		msPlot();
	});
	QObject::connect(m->actViewSecondary, &QAction::toggled, win, [](bool on) {
		g_ms->view_secondary = on;
		msSetZScale();
		msPlot();
	});
	auto *gProfiles = new QActionGroup(win);
	gProfiles->addAction(m->actNoConnect);
	gProfiles->addAction(m->actConnectGood);
	gProfiles->addAction(m->actConnectAll);
	QObject::connect(m->actNoConnect, &QAction::triggered, win, []() {
		g_ms->view_profiles = MBS_VIEW_PROFILES_NONE;
		msPlot();
	});
	QObject::connect(m->actConnectGood, &QAction::triggered, win, []() {
		g_ms->view_profiles = MBS_VIEW_PROFILES_UNFLAGGED;
		msPlot();
	});
	QObject::connect(m->actConnectAll, &QAction::triggered, win, []() {
		g_ms->view_profiles = MBS_VIEW_PROFILES_ALL;
		msPlot();
	});
	// do_mb3dsdg_resetview: exaggeration 1 (the gizmo's own), the opening direction, framed by the
	// window's own fit
	auto resetView = []() {
		Mb3dsdg *mm = g_ms;
		if (!mm || !mm->soundingdata)
			return;
		if (mm->pane) {                      // the window's own fit; its exaggeration stays the window's
			if (mm->host.attachFrame)
				mm->host.attachFrame(mm->view);
			msPlot();
			msUpdateStatus();
			return;
		}
		if (mm->host.view3dSetVE)
			mm->host.view3dSetVE(mm->view, 1.0);
		mm->exaggeration = 1.0;
		msScaleZ();
		msBuildScene();
		if (mm->host.view3dFrame)
			mm->host.view3dFrame(mm->view);
		msUpdateStatus();
		msPlot();
	};
	QObject::connect(actResetView, &QAction::triggered, win, resetView);
	QObject::connect(resetButton, &QPushButton::clicked, win, resetView);
	QObject::connect(m->actBoundingBox, &QAction::toggled, win, [](bool on) {
		g_ms->view_boundingbox = on;
		msPlot();
	});
	QObject::connect(m->actScaleWithFlagged, &QAction::toggled, win, [](bool on) {
		g_ms->view_scalewithflagged = on;
		msSetZScale();
		msPlot();
	});
	auto *gColor = new QActionGroup(win);
	gColor->addAction(m->actColorByFlag);
	gColor->addAction(m->actColorByTopo);
	gColor->addAction(m->actColorBySounding);
	gColor->addAction(m->actColorByAmp);
	QObject::connect(m->actColorByFlag, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_FLAG;
		msSetZScale();                    // each colour mode shows its own set of flagged soundings
		msPlot();
	});
	QObject::connect(m->actColorByTopo, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_TOPO;
		msSetZScale();
		msPlot();
	});
	QObject::connect(m->actColorBySounding, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_SOUNDING;
		msSetZScale();
		msPlot();
	});
	QObject::connect(m->actColorByAmp, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_AMP;
		msSetZScale();
		msPlot();
	});
	if (auto *actFile = win->findChild<QAction *>("actionColorByFile")) {   // iGMT: one colour per swath file
		gColor->addAction(actFile);
		QObject::connect(actFile, &QAction::triggered, win, []() {
			g_ms->view_color = MBS_VIEW_COLOR_FILE;
			msSetZScale();
			msPlot();
		});
	}

	// Action menu
	QObject::connect(actApplyBias, &QAction::triggered, win, []() {
		Mb3dsdg *mm = g_ms;
		/* send bias parameters to calling program to be applied */
		if (mm->notify.biasapply)
			(mm->notify.biasapply)(0.01 * ((double)mm->irollbias), 0.01 * ((double)mm->ipitchbias),
			                       0.01 * ((double)mm->iheadingbias), 0.01 * ((double)mm->itimelag),
			                       0.0001 * ((double)mm->isnell));
	});
	const int voxelArgs[6][2] = {{1, 10}, {1, 2}, {4, 10}, {4, 2}, {8, 10}, {8, 2}};
	for (int i = 0; i < 6; i++) {
		const int a = voxelArgs[i][0], b = voxelArgs[i][1];
		QObject::connect(actVoxel[i], &QAction::triggered, win, [a, b]() {
			if (g_ms->notify.flagsparsevoxels)
				(g_ms->notify.flagsparsevoxels)(a, b);
			msPlot();
			msUpdateStatus();
		});
	}
	const int colorIds[7] = {MBV_COLOR_BLACK, MBV_COLOR_RED, MBV_COLOR_YELLOW, MBV_COLOR_GREEN, MBV_COLOR_BLUEGREEN,
	                         MBV_COLOR_BLUE, MBV_COLOR_PURPLE};
	for (int i = 0; i < 7; i++) {
		const int c = colorIds[i];
		QObject::connect(actColor[i], &QAction::triggered, win, [c]() {
			/* notify calling program to color current selected unflagged soundings */
			if (g_ms->notify.colorsoundings)
				(g_ms->notify.colorsoundings)(c);
			msPlot();
		});
	}
	const int optModes[7] = {MB3DSDG_OPTIMIZEBIASVALUES_R, MB3DSDG_OPTIMIZEBIASVALUES_P, MB3DSDG_OPTIMIZEBIASVALUES_H,
	                         MB3DSDG_OPTIMIZEBIASVALUES_RP, MB3DSDG_OPTIMIZEBIASVALUES_RPH, MB3DSDG_OPTIMIZEBIASVALUES_T,
	                         MB3DSDG_OPTIMIZEBIASVALUES_S};
	for (int i = 0; i < 7; i++) {
		const int mode = optModes[i];
		QObject::connect(actOpt[i], &QAction::triggered, win, [mode]() { msOptimize(mode); });
	}
	QObject::connect(actDismiss, &QAction::triggered, win, []() { if (g_ms) mbParkQuit(g_ms->parking); });

	// bias sliders: Motif's XmScale reported on release (and keyboard steps); the label follows live
	struct SliderDef {
		QSlider *s;
		QLabel *v;
		int *target;
		double unit;
		int decimals;
	};
	const SliderDef defs[5] = {{m->scale_rollbias, m->val_rollbias, &m->irollbias, 0.01, 2},
	                           {m->scale_pitchbias, m->val_pitchbias, &m->ipitchbias, 0.01, 2},
	                           {m->scale_headingbias, m->val_headingbias, &m->iheadingbias, 0.01, 2},
	                           {m->scale_timelag, m->val_timelag, &m->itimelag, 0.01, 2},
	                           {m->scale_snell, m->val_snell, &m->isnell, 0.0001, 4}};
	for (const SliderDef &d : defs) {
		QSlider *s = d.s;
		QLabel *v = d.v;
		int *target = d.target;
		const double unit = d.unit;
		const int dec = d.decimals;
		QObject::connect(s, &QSlider::valueChanged, s, [s, v, target, unit, dec](int val) {
			v->setText(QString::number(val * unit, 'f', dec));
			if (!s->isSliderDown())
				msBiasChanged(s, target, val);
		});
		QObject::connect(s, &QSlider::sliderReleased, s, [s, target]() { msBiasChanged(s, target, s->value()); });
	}

	win->installEventFilter(new MsCloseFilter(win));
	return m;
}

} // namespace

// ---- entry points ----------------------------------------------------------------------------
bool mb3dsdgOpen(QWidget *parent, const MbEditHost &host, mb3dsoundings_struct *data, const Mb3dsdgNotify &notify) {
	if (!data)
		return false;

	/* print out some statistics of the selected soundings */
	{
		mb3dsoundings_sounding_struct *soundings = data->soundings;
		int num_soundings = 0, num_soundings_null = 0, num_soundings_unflagged = 0, num_soundings_flagged = 0,
		    num_soundings_flagged_manual = 0, num_soundings_flagged_sonar = 0, num_soundings_flagged_filter = 0,
		    num_soundings_flagged_filter2 = 0, num_soundings_flagged_secondary = 0, num_soundings_flagged_interpolated = 0;
		for (int i = 0; i < data->num_soundings; i++) {
			num_soundings++;
			num_soundings_null += mb_beam_check_flag_null(soundings[i].beamflag);
			num_soundings_unflagged += mb_beam_ok(soundings[i].beamflag);
			num_soundings_flagged += mb_beam_check_flag_flagged(soundings[i].beamflag);
			num_soundings_flagged_manual += mb_beam_check_flag_manual(soundings[i].beamflag);
			num_soundings_flagged_sonar += mb_beam_check_flag_sonar(soundings[i].beamflag);
			num_soundings_flagged_filter += mb_beam_check_flag_filter(soundings[i].beamflag);
			num_soundings_flagged_filter2 += mb_beam_check_flag_filter2(soundings[i].beamflag);
			num_soundings_flagged_secondary += mb_beam_check_flag_multipick(soundings[i].beamflag);
			num_soundings_flagged_interpolated += mb_beam_check_flag_interpolate(soundings[i].beamflag);
		}
		fprintf(stdout, "\nMBeditviz 3D Sounding View:\n");
		fprintf(stdout, "  Soundings:                        %d\n", num_soundings);
		fprintf(stdout, "  Null Soundings:                   %d\n", num_soundings_null);
		fprintf(stdout, "  Unflagged Soundings:              %d\n", num_soundings_unflagged);
		fprintf(stdout, "  Flagged Soundings:                %d\n", num_soundings_flagged);
		fprintf(stdout, "  Manual Flagged Soundings:         %d\n", num_soundings_flagged_manual);
		fprintf(stdout, "  Sonar Flagged Soundings:          %d\n", num_soundings_flagged_sonar);
		fprintf(stdout, "  Filter Flagged Soundings:         %d\n", num_soundings_flagged_filter);
		fprintf(stdout, "  Filter2 Flagged Soundings:        %d\n", num_soundings_flagged_filter2);
		fprintf(stdout, "  Secondary Flagged Soundings:      %d\n", num_soundings_flagged_secondary);
		fprintf(stdout, "  Interpolated Flagged Soundings:   %d\n", num_soundings_flagged_interpolated);
		fflush(stdout);
	}

	bool first = false;
	if (g_ms && g_ms->pane)                  // one editor at a time: a cloud's pane goes for the window
		mb3dsdgEnd();
	if (!g_ms) {
		Mb3dsdg *m = msBuild(parent, host);
		if (!m)
			return false;
		g_ms = m;
		m->parking = mbParkable(m->win, host, "mbeditviz 3-D soundings");   // after MsCloseFilter: a park comes first
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (g_ms == m)
				g_ms = nullptr;
			delete m;
		});
		m->win->resize(1040, 600);
		msUpdateModeToggles();
		msUpdateCursor();
		first = true;
	}
	Mb3dsdg *m = g_ms;
	m->notify = notify;

	/* set the data pointer */
	m->soundingdata = data;
	msScale();

	/* reset info flag */
	m->last_sounding_defined = false;
	m->last_sounding_edited = 0;

	mbParkRebind(m->parking, host.parkScene);
	mbParkShow(m->parking);              // a parked one comes back off its handle
	QApplication::processEvents();

	/* update gui widgets */
	msUpdateGui();

	/* recalculate vertical scaling */
	msSetZScale();

	/* replot the data; the first time, framed by the window's own fit */
	msBuildScene();
	if (first && m->host.view3dFrame)
		m->host.view3dFrame(m->view);
	msPlot();
	msUpdateStatus();
	return true;
}

namespace {
// The pane is gone (its dock closed, or its window closing): the window's cloud actor gets its own data
// and colouring back, the tool's actors and hooks come off the window, the caller is told, and the
// tool goes.
void msPaneTeardown(Mb3dsdg *m) {
	if (!m || g_ms != m || !m->pane)
		return;
	m->editCursorOn = false;
	if (m->cursorWatch)
		delete m->cursorWatch.data();
	if (m->paneCanvas) {                     // the window keeps living: its own arrow back
		if (auto *vw = qobject_cast<QVTKOpenGLNativeWidget *>(m->paneCanvas.data()))
			vw->setDefaultCursor(QCursor(Qt::ArrowCursor));
		m->paneCanvas->unsetCursor();
	}
	if (m->paneDock) {                       // ended from here (not by the dock's own close): the dock goes
		QWidget *dock = m->paneDock;         // too, without calling back into a tool that is already gone
		QObject::disconnect(dock, &QObject::destroyed, nullptr, nullptr);
		dock->close();
	}
	m->paneDock = nullptr;
	if (auto *pm = m->pointsActor ? vtkPolyDataMapper::SafeDownCast(m->pointsActor->GetMapper()) : nullptr) {
		pm->SetInputData(m->cloudOrigInput);
		pm->SetScalarMode(m->cloudOrigScalarMode);
		pm->SetColorMode(m->cloudOrigColorMode);
		pm->SetScalarVisibility(m->cloudOrigScalarVis);
	}
	if (m->ren) {
		for (vtkActor *a : {m->boxSolidActor.Get(), m->boxDotActor.Get(), m->profileActor.Get(), m->infoActor.Get()})
			if (a)
				m->ren->RemoveActor(a);
		m->ren->RemoveViewProp(m->grabActor);
		m->ren->RemoveObserver(m->startTag);
	}
	if (m->host.view3dDetach)
		m->host.view3dDetach(m->view);
	m->view = nullptr;
	// redrawn only while the window stays on screen (not when the pane goes because the window closes)
	QWidget *sw = m->host.sceneWindow ? m->host.sceneWindow(m->scene) : nullptr;
	if (sw && sw->isVisible() && m->rw)
		m->rw->Render();
	auto dismiss = m->notify.dismiss;
	m->notify = Mb3dsdgNotify();
	m->soundingdata = nullptr;
	g_ms = nullptr;                          // gone NOW: a new editor may open before the deferred delete
	if (dismiss)
		dismiss();
	m->win->deleteLater();                  // destroyed -> m deleted (mb3dsdgOpenPane)
}
} // namespace

bool mb3dsdgOpenPane(void *scene, const MbEditHost &host, mb3dsoundings_struct *data, const Mb3dsdgNotify &notify) {
	if (!data || !scene)
		return false;
	if (g_ms && !(g_ms->pane && g_ms->scene == scene))
		mb3dsdgEnd();                        // one 3-D sounding editor at a time: the other one goes
	if (!g_ms) {
		Mb3dsdg *m = msBuild(nullptr, host, scene);
		if (!m)
			return false;
		g_ms = m;
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (g_ms == m)
				g_ms = nullptr;
			delete m;
		});
		// the pane: mb3dsoundings_pane.ui, its radios on the SAME msSetEditMode, its View menu the window's
		// own, its Action menu the window's minus Apply Bias and the Optimize entries
		QFile f(QDir(host.uiDir).filePath("mb3dsoundings_pane.ui"));
		QWidget *content = nullptr;
		if (f.open(QIODevice::ReadOnly)) {
			QUiLoader loader;
			content = loader.load(&f, nullptr);
		}
		if (!content) {
			QMessageBox::warning(nullptr, "3D Soundings", "Cannot load mb3dsoundings_pane.ui");
			m->notify = Mb3dsdgNotify();
			msPaneTeardown(m);
			return false;
		}
		const char *modes[7] = {"modeNavigate", "modeToggle", "modePick", "modeErase", "modeRestore", "modeGrab", "modeInfo"};
		auto *group = new QButtonGroup(content);
		for (int i = 0; i < 7; i++) {
			m->paneMode[i] = content->findChild<QRadioButton *>(modes[i]);
			if (m->paneMode[i])
				group->addButton(m->paneMode[i], i);
		}
		QObject::connect(group, &QButtonGroup::idClicked, content, [](int id) {
			if (g_ms)
				msSetEditMode(id == 0 ? MBS_EDIT_NONE : id - 1);
		});
		m->paneStatus = content->findChild<QLabel *>("labelStatus");
		// Navigation: the window's navigation lines on / off (the host's setter, mb3dsdgSetNavToggle)
		if (auto *nb = content->findChild<QRadioButton *>("navButton")) {
			nb->setAutoExclusive(false);
			QObject::connect(nb, &QRadioButton::toggled, content, [](bool on) {
				if (g_ms && g_ms->navShow)
					g_ms->navShow(on);
			});
		}
		// Save: the caller writes the edits made so far (the swath files' .esf); the pane stays open
		if (auto *sb = content->findChild<QPushButton *>("saveButton"))
			QObject::connect(sb, &QPushButton::clicked, content, []() {
				if (g_ms && g_ms->notify.save) {
					QApplication::setOverrideCursor(Qt::WaitCursor);
					g_ms->notify.save();
					QApplication::restoreOverrideCursor();
				}
			});
		// Discard (an area pane only, mb3dsdgSetAreaMode): its edits are dropped
		if (auto *db = content->findChild<QPushButton *>("discardButton")) {
			db->hide();
			QObject::connect(db, &QPushButton::clicked, content, []() {
				if (g_ms && g_ms->notify.discard)
					g_ms->notify.discard();
			});
		}
		// Gridding: the caller opens its gridding on these soundings (the good ones only)
		if (auto *gb = content->findChild<QPushButton *>("gridButton"))
			QObject::connect(gb, &QPushButton::clicked, content, []() {
				if (g_ms && g_ms->notify.grid)
					g_ms->notify.grid();
			});
		// CUBE filter: the caller opens CUBE on these soundings, set to flag (its dialog also grids)
		if (auto *cb = content->findChild<QPushButton *>("cubeFilterButton"))
			QObject::connect(cb, &QPushButton::clicked, content, []() {
				if (g_ms && g_ms->notify.cube)
					g_ms->notify.cube(true);
			});
		if (auto *vb = content->findChild<QToolButton *>("viewButton"))
			vb->setMenu(m->win->findChild<QMenu *>("menuView"));
		if (auto *ab = content->findChild<QToolButton *>("actionButton")) {
			auto *menu = new QMenu(content);
			for (const char *n : {"actionFlagSparseA", "actionFlagSparseB", "actionFlagSparseC", "actionFlagSparseD",
			                      "actionFlagSparseE", "actionFlagSparseF"})
				if (auto *a = m->win->findChild<QAction *>(n))
					menu->addAction(a);
			menu->addSeparator();
			for (const char *n : {"actionColorBlack", "actionColorRed", "actionColorYellow", "actionColorGreen",
			                      "actionColorBlueGreen", "actionColorBlue", "actionColorPurple"})
				if (auto *a = m->win->findChild<QAction *>(n))
					menu->addAction(a);
			ab->setMenu(menu);
		}
		// on the window's own ground the flag colours' black good soundings would vanish: the pane opens
		// coloured by soundings (the topography scale stretched over them)
		m->view_color = MBS_VIEW_COLOR_SOUNDING;
		if (m->actColorBySounding)
			m->actColorBySounding->setChecked(true);
		m->paneDock = host.addPane(scene, content, "3D Soundings", [m]() {
			if (g_ms != m)
				return;
			m->paneDock = nullptr;           // the dock is being destroyed: never touched again
			msPaneTeardown(m);
		});
		if (!m->paneDock) {
			m->notify = Mb3dsdgNotify();
			msPaneTeardown(m);
			return false;
		}
	}
	Mb3dsdg *m = g_ms;
	m->notify = notify;
	m->soundingdata = data;
	m->last_sounding_defined = false;
	m->last_sounding_edited = 0;
	msScale();
	msSetZScale();
	msUpdateModeToggles();
	msUpdateCursor();
	msPlot();
	msUpdateStatus();
	return true;
}

void mb3dsdgSetAreaMode() {
	Mb3dsdg *m = g_ms;
	if (!m || !m->pane || !m->paneDock)
		return;
	QWidget *d = m->paneDock;
	if (auto *sb = d->findChild<QPushButton *>("saveButton")) {
		sb->setText("Accept flags");
		sb->setToolTip("Hand the flags made here to the full cloud's window (its Save writes them to the .esf), "
		               "close this view and give the full cloud its pane back");
	}
	if (auto *db = d->findChild<QPushButton *>("discardButton"))
		db->show();
	// CUBE filter and Gridding STAY: a sub-cloud view (an area's, a track's) is a full 3D Soundings pane.
	// Both act on THIS window's pane -- Gridding grids its good soundings into its own window, CUBE's
	// flags become this pane's edits, which Accept hands to the full cloud like any other.
}

// The pane's Navigation toggle drives `show` (the host's own visibility setter for the window's
// navigation lines); its box starts at `on`, what the lines are now.
void mb3dsdgSetNavToggle(std::function<void(bool)> show, bool on) {
	Mb3dsdg *m = g_ms;
	if (!m || !m->pane)
		return;
	m->navShow = std::move(show);
	if (auto *nb = m->paneDock ? m->paneDock->findChild<QRadioButton *>("navButton") : nullptr) {
		QSignalBlocker b(nb);
		nb->setChecked(on);
	}
}

void mb3dsdgEnd() {
	if (!g_ms)
		return;
	if (g_ms->pane) {
		msPaneTeardown(g_ms);                // synchronous; its caller is told (a cloud's edits get saved)
		return;
	}
	g_ms->notify = Mb3dsdgNotify();
	g_ms->soundingdata = nullptr;
	mbParkQuit(g_ms->parking);
}

void mb3dsdgPlot() {
	if (g_ms && g_ms->soundingdata) {
		msPlot();
		msUpdateStatus();
	}
}

bool mb3dsdgIsOpen() {
	return g_ms != nullptr && g_ms->soundingdata != nullptr;
}

void mb3dsdgGetBiasValues(double *rollbias, double *pitchbias, double *headingbias, double *timelag, double *snell) {
	const int ir = g_ms ? g_ms->irollbias : 0, ip = g_ms ? g_ms->ipitchbias : 0, ih = g_ms ? g_ms->iheadingbias : 0,
	          it = g_ms ? g_ms->itimelag : 0, is = g_ms ? g_ms->isnell : 10000;
	*rollbias = 0.01 * ((double)ir);
	*pitchbias = 0.01 * ((double)ip);
	*headingbias = 0.01 * ((double)ih);
	*timelag = 0.01 * ((double)it);
	*snell = 0.0001 * ((double)is);
}

bool mb3dsdgCanvasSize(int *w, int *h) {
	if (!g_ms || !g_ms->canvasW)
		return false;
	*w = g_ms->canvasW->width();
	*h = g_ms->canvasW->height();
	return true;
}

// one press-drag-release of the left button on the view, as a user makes it: it edits only with an
// edit mode armed, so with none armed this arms Toggle first (what mb3dsoundings opened in)
bool mb3dsdgMouseEdit(int x0, int y0, int x1, int y1) {
	if (!mb3dsdgIsOpen())
		return false;
	if (g_ms->edit_mode < 0)
		msSetEditMode(MBS_EDIT_TOGGLE);
	QWidget *c = g_ms->canvasW;
	auto send = [c](QEvent::Type type, Qt::MouseButton b, Qt::MouseButtons held, int x, int y) {
		const QPointF pos(x, y);
		QMouseEvent ev(type, pos, c->mapToGlobal(pos), b, held, Qt::NoModifier);
		QApplication::sendEvent(c, &ev);
	};
	send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, x0, y0);
	if (x1 != x0 || y1 != y0)
		send(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, x1, y1);
	send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, x1, y1);
	return true;
}

bool mb3dsdgCounts(int *drawn, int *good, int *flagged) {
	if (!mb3dsdgIsOpen())
		return false;
	// what the view really renders: the points actor's own input
	auto *pm = g_ms->pointsActor ? vtkPolyDataMapper::SafeDownCast(g_ms->pointsActor->GetMapper()) : nullptr;
	vtkPolyData *pd = pm ? vtkPolyData::SafeDownCast(pm->GetInput()) : nullptr;
	if (drawn) *drawn = pd ? int(pd->GetNumberOfPoints()) : -1;
	if (good) *good = g_ms->soundingdata->num_soundings_unflagged;
	if (flagged) *flagged = g_ms->soundingdata->num_soundings_flagged;
	return true;
}

int mb3dsdgPaneButtons() {
	Mb3dsdg *m = g_ms;
	if (!m || !m->pane || !m->paneDock)
		return -1;
	int bits = 0, k = 0;
	for (const char *nm : {"cubeFilterButton", "gridButton", "discardButton"}) {
		auto *b = m->paneDock->findChild<QPushButton *>(nm);
		if (b && !b->isHidden())
			bits |= 1 << k;
		k++;
	}
	return bits;
}

bool mb3dsdgSetShowFlagged(bool on) {
	if (!mb3dsdgIsOpen() || !g_ms->actViewFlagged)
		return false;
	g_ms->actViewFlagged->setChecked(on);   // the menu entry itself: its toggled() does the rest
	return true;
}

bool mb3dsdgKey(int ch) {
	if (!mb3dsdgIsOpen())
		return false;
	const QString t = QString(QChar(char16_t(ch)));
	QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, t);
	QApplication::sendEvent(g_ms->canvasW, &press);
	QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, t);
	QApplication::sendEvent(g_ms->canvasW, &release);
	return true;
}

bool mb3dsdgSetEditMode(int mode) {
	if (!mb3dsdgIsOpen() || mode < 0 || mode > 5)
		return false;
	msSetEditMode(mode);
	return true;
}

bool mb3dsdgSoundingPixel(int i, int *x, int *y) {
	if (!mb3dsdgIsOpen() || i < 0 || i >= g_ms->soundingdata->num_soundings)
		return false;
	msProjectAll();
	const double dpr = g_ms->canvasW->devicePixelRatioF();
	const mb3dsoundings_sounding_struct &s = g_ms->soundingdata->soundings[i];
	*x = (int)(s.winx / dpr);
	*y = (int)((g_ms->gl_height - 1 - s.winy) / dpr);
	return true;
}

bool mb3dsdgSavePng(const QString &path) {
	if (!mb3dsdgIsOpen())
		return false;
	msPlot();
	vtkNew<vtkWindowToImageFilter> w2i;
	w2i->SetInput(g_ms->rw);
	w2i->ReadFrontBufferOff();
	w2i->Update();
	vtkNew<vtkPNGWriter> png;
	png->SetFileName(path.toUtf8().constData());
	png->SetInputConnection(w2i->GetOutputPort());
	png->Write();
	return QFile::exists(path);
}
