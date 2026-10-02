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
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QUiLoader>
#include <QVBoxLayout>

#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPNGWriter.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
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
const int MBS_MOUSE_ROTATE = 0;
const int MBS_MOUSE_PANZOOM = 1;
const int MBS_EDIT_TOGGLE = 0;
const int MBS_EDIT_PICK = 1;
const int MBS_EDIT_ERASE = 2;
const int MBS_EDIT_RESTORE = 3;
const int MBS_EDIT_GRAB = 4;
const int MBS_EDIT_INFO = 5;
const double MBS_PICK_THRESHOLD = 50;
const double MBS_ERASE_THRESHOLD = 15;
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

class MsCanvas;

// ---- mb3dsoundings_callbacks.c's `mb3dsoundings` struct ----------------------------------------
struct Mb3dsdg {
	QMainWindow *win = nullptr;
	MsCanvas *canvas = nullptr;
	QWidget *canvasW = nullptr;               // the same canvas, for the code above its class
	vtkRenderWindow *rw = nullptr;
	QString uiDir;
	Mb3dsdgNotify notify;
	mb3dsoundings_struct *soundingdata = nullptr;

	QLabel *labelStatus = nullptr, *labelMouseMode = nullptr;
	QRadioButton *modeButton[6] = {};
	QRadioButton *mouseRotate1 = nullptr, *mousePanZoom1 = nullptr;
	QAction *actMouseRotate = nullptr, *actMousePanZoom = nullptr;
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
	vtkSmartPointer<vtkTransform> model;

	int edit_mode = MBS_EDIT_TOGGLE;
	int mouse_mode = MBS_MOUSE_ROTATE;
	bool keyreverse_mode = false;

	/* drawing variables */
	double elevation = 0.0;
	double azimuth = 0.0;
	double exaggeration = 1.0;
	double elevation_save = 0.0, azimuth_save = 0.0, exaggeration_save = 1.0;
	int gl_width = 0, gl_height = 0;
	double right = -1.0, left = 1.0, top = 1.0, bottom = -1.0;
	double gl_offset_x = 0.0, gl_offset_y = 0.0, gl_offset_x_save = 0.0, gl_offset_y_save = 0.0;
	double gl_size = 1.0, gl_size_save = 1.0;

	/* button parameters */
	bool button1down = false, button2down = false, button3down = false;
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
};
Mb3dsdg *g_ms = nullptr;

double msDpr() {
	return (g_ms && g_ms->win) ? g_ms->win->devicePixelRatioF() : 1.0;
}

void msPlot();
void msUpdateStatus();
void msUpdateModeToggles();
void msUpdateLabelMouseMode();
void msUpdateCursor();

void msBeep() {
	QApplication::beep();
}

// the notify the edit functions end with: flush the caller's pending edits
void msFlushPrevious() {
	if (g_ms->notify.edit)
		(g_ms->notify.edit)(0, 0, 0, MB_FLAG_NULL, MB3DSDG_EDIT_FLUSHPREVIOUS);
}

// ---- mb3dsoundings_scale / _scalez / _setzscale ------------------------------------------------
void msScale() {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		sounding->glx = (float)(soundingdata->scale * sounding->x);
		sounding->gly = (float)(soundingdata->scale * sounding->y);
		sounding->glz = (float)(g_ms->exaggeration * soundingdata->zscale * (sounding->z - soundingdata->zorigin));
	}
}

void msScaleZ() {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;
	for (int i = 0; i < soundingdata->num_soundings; i++) {
		mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
		sounding->glz = (float)(g_ms->exaggeration * soundingdata->zscale * (sounding->z - soundingdata->zorigin));
	}
}

void msSetZScale() {
	mb3dsoundings_struct *soundingdata = g_ms->soundingdata;

	/* initialize zmin and zmax */
	double zmin = 0.0;
	double zmax = 0.0;

	/* get vertical min maxes for scaling of all soundings */
	if (g_ms->view_scalewithflagged && soundingdata->num_soundings > 0) {
		zmin = soundingdata->soundings[0].z;
		zmax = soundingdata->soundings[0].z;
		for (int i = 0; i < soundingdata->num_soundings; i++) {
			zmin = MIN(soundingdata->soundings[i].z, zmin);
			zmax = MAX(soundingdata->soundings[i].z, zmax);
		}
	}

	/* else get vertical min maxes for scaling of only unflagged soundings */
	else if (soundingdata->num_soundings > 0) {
		int nunflagged = 0;
		for (int i = 1; i < soundingdata->num_soundings; i++) {
			if (mb_beam_ok(soundingdata->soundings[i].beamflag)) {
				if (nunflagged == 0) {
					zmin = soundingdata->soundings[i].z;
					zmax = soundingdata->soundings[i].z;
				}
				else {
					zmin = MIN(soundingdata->soundings[i].z, zmin);
					zmax = MAX(soundingdata->soundings[i].z, zmax);
				}
				nunflagged++;
			}
		}
	}

	soundingdata->zorigin = 0.5 * (zmin + zmax);
	soundingdata->zmin = -0.5 * (zmax - zmin);
	soundingdata->zmax = 0.5 * (zmax - zmin);
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

// the composite transform model -> window pixel (origin bottom left), as gluProject applied it
void msProject(const double m[16], double x, double y, double z, int *wx, int *wy) {
	const double X = m[0] * x + m[1] * y + m[2] * z + m[3];
	const double Y = m[4] * x + m[5] * y + m[6] * z + m[7];
	const double W = m[12] * x + m[13] * y + m[14] * z + m[15];
	const double nx = (W != 0.0) ? X / W : X;
	const double ny = (W != 0.0) ? Y / W : Y;
	*wx = (int)(0.5 * (nx + 1.0) * g_ms->gl_width);
	*wy = (int)(0.5 * (ny + 1.0) * g_ms->gl_height);
}

void msPlot() {
	Mb3dsdg *m = g_ms;
	if (!m || !m->canvas || !m->soundingdata)
		return;
	mb3dsoundings_struct *soundingdata = m->soundingdata;
	vtkRenderWindow *rw = m->rw;
	const int *sz = rw->GetSize();
	m->gl_width = sz[0] > 0 ? sz[0] : 1;
	m->gl_height = sz[1] > 0 ? sz[1] : 1;

	/* set projection */
	m->left = -1.0 / m->gl_size;
	m->right = 1.0 / m->gl_size;
	m->bottom = -1.0 / m->gl_size;
	m->top = 1.0 / m->gl_size;
	vtkCamera *cam = m->ren->GetActiveCamera();
	cam->ParallelProjectionOn();
	cam->SetPosition(0.0, 0.0, 1000.0);
	cam->SetFocalPoint(0.0, 0.0, 0.0);
	cam->SetViewUp(0.0, 1.0, 0.0);
	cam->SetParallelScale(1.0 / m->gl_size);
	cam->SetClippingRange(1.0, 2000.0);

	/* set up translations */
	m->model->Identity();
	m->model->Translate(m->gl_offset_x, m->gl_offset_y, 0.0);
	m->model->RotateX(m->elevation - 90.0);
	m->model->RotateZ(m->azimuth);

	/* Plot the bounding box if desired */
	{
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> solid, dotted;
		if (m->view_boundingbox) {
			const double glxmin = soundingdata->scale * soundingdata->xmin;
			const double glxmax = soundingdata->scale * soundingdata->xmax;
			const double glymin = soundingdata->scale * soundingdata->ymin;
			const double glymax = soundingdata->scale * soundingdata->ymax;
			const double glzmin = m->exaggeration * soundingdata->zscale * soundingdata->zmin;
			const double glzmax = m->exaggeration * soundingdata->zscale * soundingdata->zmax;
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
				else if (m->view_flagged) {
					if (mb_beam_check_flag_manual(sounding->beamflag)) {
						msAddPoint(pts, verts, rgb, sounding, 1.0f, 0.0f, 0.0f);
					}
					else if (mb_beam_check_flag_filter(sounding->beamflag) || mb_beam_check_flag_filter2(sounding->beamflag)) {
						msAddPoint(pts, verts, rgb, sounding, 0.0f, 0.0f, 1.0f);
					}
					else if (mb_beam_check_flag_sonar(sounding->beamflag)) {
						msAddPoint(pts, verts, rgb, sounding, 0.0f, 1.0f, 0.0f);
					}
					else if (m->view_secondary && mb_beam_check_flag_multipick(sounding->beamflag)) {
						msAddPoint(pts, verts, rgb, sounding, 0.0f, 1.0f, 1.0f);
					}
				}
			}
		}

		/* Color by topography */
		else if (m->view_color == MBS_VIEW_COLOR_TOPO) {
			for (int i = 0; i < soundingdata->num_soundings; i++) {
				mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);

				/* plot sounding (the original's test, parenthesis included) */
				if (mb_beam_ok(sounding->beamflag) ||
				    (m->view_flagged && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (!mb_beam_check_flag_multipick(sounding->beamflag || m->view_secondary)))) {
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
				if (m->view_flagged || mb_beam_ok(sounding->beamflag)) {
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
				    (m->view_flagged && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (!mb_beam_check_flag_multipick(sounding->beamflag || m->view_secondary)))) {
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
				    (m->view_flagged && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (!mb_beam_check_flag_multipick(sounding->beamflag || m->view_secondary)))) {
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
				    (m->view_flagged && !mb_beam_check_flag_null(sounding->beamflag) &&
				     (!mb_beam_check_flag_multipick(sounding->beamflag || m->view_secondary)))) {
					float r, g, b;
					mbviewGetColor(sounding->a, ampmin, ampmax, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, colortable_redtoblue_red,
					               colortable_redtoblue_green, colortable_redtoblue_blue, &r, &g, &b);
					msAddPoint(pts, verts, rgb, sounding, r, g, b);
				}
			}
		}
		vtkNew<vtkPolyData> pd;
		pd->SetPoints(pts);
		pd->SetVerts(verts);
		pd->GetCellData()->SetScalars(rgb);
		vtkPolyDataMapper::SafeDownCast(m->pointsActor->GetMapper())->SetInputData(pd);
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

	/* save the screen positions of the soundings to facilitate picking */
	{
		vtkMatrix4x4 *proj = cam->GetCompositeProjectionTransformMatrix((double)m->gl_width / (double)m->gl_height, -1.0, 1.0);
		vtkNew<vtkMatrix4x4> full;
		vtkMatrix4x4::Multiply4x4(proj, m->model->GetMatrix(), full);
		double mm[16];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				mm[4 * r + c] = full->GetElement(r, c);
		for (int i = 0; i < soundingdata->num_soundings; i++) {
			mb3dsoundings_sounding_struct *sounding = &(soundingdata->soundings[i]);
			msProject(mm, sounding->glx, sounding->gly, sounding->glz, &sounding->winx, &sounding->winy);
		}
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

	rw->Render();
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
		snprintf(value_text, sizeof(value_text), "Azi:%.2f | Elev: %.2f | exagger:%.2f | Tot:%d Good:%d Flagged:%d", m->azimuth,
		         m->elevation, m->exaggeration, soundingdata->num_soundings, soundingdata->num_soundings_unflagged,
		         soundingdata->num_soundings_flagged);
		m->biasPanel->show();
	}

	/* put up the new status string */
	m->labelStatus->setText(QString::fromLatin1(value_text));
}

void msUpdateLabelMouseMode() {
	Mb3dsdg *m = g_ms;
	static const char *const modes[6] = {"Toggle", "Pick", "Erase", "Restore", "Grab", "Info"};
	const QString edit = QString("L: Edit (%1)").arg(modes[m->edit_mode]);
	if (m->mouse_mode == MBS_MOUSE_PANZOOM)
		m->labelMouseMode->setText("Mouse Mode:\n" + edit + "\nM: Pan\nR: Zoom");
	else
		m->labelMouseMode->setText("Mouse Mode:\n" + edit + "\nM: Rotate Soundings\nR: exaggeration");
}

void msUpdateCursor() {
	Mb3dsdg *m = g_ms;
	// mb3dsoundings' cursors: a target to pick, an exchange to sweep, a fleur while rotating
	if (m->button2down || m->button3down)
		m->canvasW->setCursor(Qt::SizeAllCursor);
	else if (m->edit_mode == MBS_EDIT_ERASE || m->edit_mode == MBS_EDIT_RESTORE)
		m->canvasW->setCursor(Qt::PointingHandCursor);
	else
		m->canvasW->setCursor(Qt::CrossCursor);
}

void msUpdateModeToggles() {
	Mb3dsdg *m = g_ms;
	for (int i = 0; i < 6; i++) {
		QSignalBlocker b(m->modeButton[i]);
		m->modeButton[i]->setChecked(i == m->edit_mode);
	}
	msUpdateLabelMouseMode();
}

void msSetEditMode(int mode) {
	g_ms->edit_mode = mode;
	msUpdateModeToggles();
	msUpdateCursor();
	msPlot();
	msUpdateStatus();
}

void msSetMouseMode(int mode) {
	Mb3dsdg *m = g_ms;
	m->mouse_mode = mode;
	{
		QSignalBlocker b1(m->actMouseRotate), b2(m->actMousePanZoom), b3(m->mouseRotate1), b4(m->mousePanZoom1);
		m->actMouseRotate->setChecked(mode == MBS_MOUSE_ROTATE);
		m->actMousePanZoom->setChecked(mode == MBS_MOUSE_PANZOOM);
		m->mouseRotate1->setChecked(mode == MBS_MOUSE_ROTATE);
		m->mousePanZoom1->setChecked(mode == MBS_MOUSE_PANZOOM);
	}
	msUpdateLabelMouseMode();
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

// ---- the canvas ------------------------------------------------------------------------------
class MsCanvas : public QVTKOpenGLNativeWidget {
public:
	explicit MsCanvas(QWidget *parent) : QVTKOpenGLNativeWidget(parent) {
		setFocusPolicy(Qt::StrongFocus);
		setCursor(Qt::CrossCursor);
	}

	// window pixel (origin bottom left, device pixels) of a Qt position
	void glPos(const QPointF &p, int *x, int *y) const {
		const double dpr = devicePixelRatioF();
		*x = (int)(p.x() * dpr);
		*y = g_ms->gl_height - 1 - (int)(p.y() * dpr);
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		QVTKOpenGLNativeWidget::resizeEvent(e);
		if (g_ms && g_ms->soundingdata)
			QMetaObject::invokeMethod(this, []() { if (g_ms) msPlot(); }, Qt::QueuedConnection);
	}

	void enterEvent(QEnterEvent *) override { setFocus(Qt::MouseFocusReason); }

	// do_mb3dsdg_glwda_input, ButtonPress
	void mousePressEvent(QMouseEvent *e) override {
		Mb3dsdg *m = g_ms;
		if (!m || !m->soundingdata)
			return;
		setFocus(Qt::MouseFocusReason);
		glPos(e->position(), &m->button_down_x, &m->button_down_y);

		/* If left mouse button is pushed */
		if (e->button() == Qt::LeftButton) {
			m->button1down = true;
			if (m->edit_mode == MBS_EDIT_TOGGLE || m->edit_mode == MBS_EDIT_PICK)
				msPick(m->button_down_x, m->button_down_y);
			else if (m->edit_mode == MBS_EDIT_ERASE || m->edit_mode == MBS_EDIT_RESTORE)
				msEraseRestore(m->button_down_x, m->button_down_y);
			else if (m->edit_mode == MBS_EDIT_GRAB)
				msGrab(m->button_down_x, m->button_down_y, MBS_EDIT_GRAB_START);
			else if (m->edit_mode == MBS_EDIT_INFO)
				msInfo(m->button_down_x, m->button_down_y);
		}

		/* If middle mouse button is pushed */
		else if (e->button() == Qt::MiddleButton) {
			m->button2down = true;
			if (m->mouse_mode == MBS_MOUSE_ROTATE) {
				m->azimuth_save = m->azimuth;
				m->elevation_save = m->elevation;
			}
			else {
				m->gl_offset_x_save = m->gl_offset_x;
				m->gl_offset_y_save = m->gl_offset_y;
			}
			msUpdateCursor();
		}

		/* If right mouse button is pushed */
		else if (e->button() == Qt::RightButton) {
			m->button3down = true;
			if (m->mouse_mode == MBS_MOUSE_ROTATE)
				m->exaggeration_save = m->exaggeration;
			else
				m->gl_size_save = m->gl_size;
			msUpdateCursor();
		}
	}

	// MotionNotify while pressed
	void mouseMoveEvent(QMouseEvent *e) override {
		Mb3dsdg *m = g_ms;
		if (!m || !m->soundingdata)
			return;
		glPos(e->position(), &m->button_move_x, &m->button_move_y);

		/* If left mouse button is dragged */
		if (m->button1down) {
			if (m->edit_mode == MBS_EDIT_ERASE || m->edit_mode == MBS_EDIT_RESTORE)
				msEraseRestore(m->button_move_x, m->button_move_y);
			else if (m->edit_mode == MBS_EDIT_GRAB)
				msGrab(m->button_move_x, m->button_move_y, MBS_EDIT_GRAB_MOVE);
		}

		/* If middle mouse button is dragged */
		else if (m->button2down) {
			if (m->mouse_mode == MBS_MOUSE_ROTATE) {
				/* rotate viewpoint of 3D map */
				m->azimuth = m->azimuth_save + 180.0 * ((double)(m->button_move_x - m->button_down_x)) / ((double)m->gl_width);
				m->elevation =
				    m->elevation_save + 180.0 * ((double)(m->button_down_y - m->button_move_y)) / ((double)m->gl_height);

				/* keep elevation and azimuth values in appropriate bounds */
				if (m->elevation > 180.0)
					m->elevation -= 360.0;
				if (m->elevation < -180.0)
					m->elevation += 360.0;
				if (m->azimuth < 0.0)
					m->azimuth += 360.0;
				if (m->azimuth > 360.0)
					m->azimuth -= 360.0;
			}
			else {
				/* pan (world units per window pixel, which the aspect-keeping view makes equal on both axes) */
				const double perPixel = (m->top - m->bottom) / ((double)m->gl_height);
				m->gl_offset_x = m->gl_offset_x_save + ((double)(m->button_move_x - m->button_down_x)) * perPixel;
				m->gl_offset_y = m->gl_offset_y_save + ((double)(m->button_move_y - m->button_down_y)) * perPixel;
			}
			msUpdateStatus();
			msPlot();
		}

		/* If right mouse button is dragged */
		else if (m->button3down) {
			if (m->mouse_mode == MBS_MOUSE_ROTATE) {
				/* change vertical exaggeration of 3D map */
				m->exaggeration =
				    m->exaggeration_save * exp(((double)(m->button_move_y - m->button_down_y)) / ((double)m->gl_height));
				msScaleZ();
			}
			else {
				/* change zoom */
				m->gl_size = m->gl_size_save * exp(((double)(m->button_move_y - m->button_down_y)) / ((double)m->gl_height));
			}
			msUpdateStatus();
			msPlot();
		}
	}

	// ButtonRelease
	void mouseReleaseEvent(QMouseEvent *e) override {
		Mb3dsdg *m = g_ms;
		if (!m || !m->soundingdata)
			return;
		glPos(e->position(), &m->button_up_x, &m->button_up_y);
		if (m->button1down && m->edit_mode == MBS_EDIT_GRAB)
			msGrab(m->button_down_x, m->button_down_y, MBS_EDIT_GRAB_END);

		/* unset all buttondown flags */
		m->button1down = false;
		m->button2down = false;
		m->button3down = false;

		/* set edit cursor */
		msUpdateCursor();
		msPlot();
	}

	void wheelEvent(QWheelEvent *) override {}       // no VTK camera interaction behind our back
	void mouseDoubleClickEvent(QMouseEvent *) override {}

	// KeyPress: the key macros
	void keyPressEvent(QKeyEvent *e) override {
		Mb3dsdg *m = g_ms;
		const QString t = e->text();
		if (!m || !m->soundingdata || t.isEmpty()) {
			QVTKOpenGLNativeWidget::keyPressEvent(e);
			return;
		}
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
		case 'X':
		case 'x':
			msFlagView();
			break;
		case '>':
		case '.':
		case 'C':
		case 'c':
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
		case 'E':
		case 'e':
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

	void keyReleaseEvent(QKeyEvent *e) override {
		Mb3dsdg *m = g_ms;
		const QString t = e->text();
		if (!m || t.isEmpty())
			return;
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
};

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

Mb3dsdg *msBuild(QWidget *parent, const QString &uiDir, const QIcon &icon) {
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
	QStringList missing;
	const char *modes[6] = {"modeToggle", "modePick", "modeErase", "modeRestore", "modeGrab", "modeInfo"};
	for (int i = 0; i < 6; i++)
		m->modeButton[i] = msChild<QRadioButton>(win, modes[i], missing);
	m->mouseRotate1 = msChild<QRadioButton>(win, "mouseRotate1", missing);
	m->mousePanZoom1 = msChild<QRadioButton>(win, "mousePanZoom1", missing);
	m->labelStatus = msChild<QLabel>(win, "labelStatus", missing);
	m->labelMouseMode = msChild<QLabel>(win, "labelMouseMode", missing);
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
	m->actMouseRotate = msChild<QAction>(win, "actionMouseRotate", missing);
	m->actMousePanZoom = msChild<QAction>(win, "actionMousePanZoom", missing);
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

	// the canvas
	auto *lay = new QVBoxLayout(host_w);
	lay->setContentsMargins(0, 0, 0, 0);
	m->canvas = new MsCanvas(host_w);
	lay->addWidget(m->canvas);
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	m->canvas->setRenderWindow(rw);
	m->canvasW = m->canvas;
	m->rw = rw;
	m->ren = vtkSmartPointer<vtkRenderer>::New();
	m->ren->SetBackground(1.0, 1.0, 1.0);
	rw->AddRenderer(m->ren);
	m->model = vtkSmartPointer<vtkTransform>::New();
	m->model->PreMultiply();
	m->boxSolidActor = msMakeActor(1.0, 1.0);
	m->boxSolidActor->GetProperty()->SetColor(0.0, 0.0, 0.0);
	m->boxDotActor = msMakeActor(1.0, 1.0);
	m->boxDotActor->GetProperty()->SetColor(0.75, 0.75, 0.75);
	m->profileActor = msMakeActor(1.0, 1.0);
	m->pointsActor = msMakeActor(3.0, 1.0);
	m->infoActor = msMakeActor(6.0, 1.0);
	for (vtkActor *a : {m->boxSolidActor.Get(), m->boxDotActor.Get(), m->profileActor.Get(), m->pointsActor.Get(),
	                    m->infoActor.Get()}) {
		a->SetUserTransform(m->model);
		m->ren->AddActor(a);
	}
	{
		vtkNew<vtkPolyDataMapper2D> m2;
		vtkNew<vtkPolyData> empty;            // a first render may come before the first msPlot
		m2->SetInputData(empty);
		m->grabActor = vtkSmartPointer<vtkActor2D>::New();
		m->grabActor->SetMapper(m2);
		m->grabActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
		m->grabActor->GetProperty()->SetLineWidth(3.0);
		m->ren->AddActor2D(m->grabActor);
	}

	// edit mode toggles
	auto *gMode = new QButtonGroup(win);
	for (int i = 0; i < 6; i++) {
		gMode->addButton(m->modeButton[i], i);
		QObject::connect(m->modeButton[i], &QRadioButton::toggled, win, [i](bool on) {
			if (on && g_ms)
				msSetEditMode(i);
		});
	}
	// mouse modes: the menu pair and the radio pair are one setting
	auto *gMouse = new QActionGroup(win);
	gMouse->addAction(m->actMouseRotate);
	gMouse->addAction(m->actMousePanZoom);
	auto *gMouse1 = new QButtonGroup(win);
	gMouse1->addButton(m->mouseRotate1);
	gMouse1->addButton(m->mousePanZoom1);
	QObject::connect(m->actMouseRotate, &QAction::triggered, win, []() { msSetMouseMode(MBS_MOUSE_ROTATE); });
	QObject::connect(m->actMousePanZoom, &QAction::triggered, win, []() { msSetMouseMode(MBS_MOUSE_PANZOOM); });
	QObject::connect(m->mouseRotate1, &QRadioButton::toggled, win, [](bool on) {
		if (on)
			msSetMouseMode(MBS_MOUSE_ROTATE);
	});
	QObject::connect(m->mousePanZoom1, &QRadioButton::toggled, win, [](bool on) {
		if (on)
			msSetMouseMode(MBS_MOUSE_PANZOOM);
	});

	// View menu
	QObject::connect(m->actViewFlagged, &QAction::toggled, win, [](bool on) {
		g_ms->view_flagged = on;
		msPlot();
	});
	QObject::connect(m->actViewSecondary, &QAction::toggled, win, [](bool on) {
		g_ms->view_secondary = on;
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
	// do_mb3dsdg_resetview
	auto resetView = []() {
		g_ms->elevation = 0.0;
		g_ms->azimuth = 0.0;
		g_ms->exaggeration = 1.0;
		msScaleZ();
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
		msPlot();
	});
	QObject::connect(m->actColorByTopo, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_TOPO;
		msPlot();
	});
	QObject::connect(m->actColorBySounding, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_SOUNDING;
		msPlot();
	});
	QObject::connect(m->actColorByAmp, &QAction::triggered, win, []() {
		g_ms->view_color = MBS_VIEW_COLOR_AMP;
		msPlot();
	});

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
	QObject::connect(actDismiss, &QAction::triggered, win, &QWidget::close);

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
bool mb3dsdgOpen(QWidget *parent, const QString &uiDir, const QIcon &icon, mb3dsoundings_struct *data,
                 const Mb3dsdgNotify &notify) {
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

	if (!g_ms) {
		Mb3dsdg *m = msBuild(parent, uiDir, icon);
		if (!m)
			return false;
		g_ms = m;
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (g_ms == m)
				g_ms = nullptr;
			delete m;
		});
		m->win->resize(1040, 600);
		msUpdateModeToggles();
		msSetMouseMode(m->mouse_mode);
		msUpdateCursor();
	}
	Mb3dsdg *m = g_ms;
	m->notify = notify;

	/* set the data pointer */
	m->soundingdata = data;
	msScale();

	/* reset info flag */
	m->last_sounding_defined = false;
	m->last_sounding_edited = 0;

	m->win->show();
	m->win->raise();
	m->win->activateWindow();
	QApplication::processEvents();

	/* update gui widgets */
	msUpdateGui();

	/* recalculate vertical scaling */
	msSetZScale();

	/* replot the data */
	msPlot();
	msUpdateStatus();
	return true;
}

void mb3dsdgEnd() {
	if (!g_ms)
		return;
	g_ms->notify = Mb3dsdgNotify();
	g_ms->soundingdata = nullptr;
	g_ms->win->close();
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
	if (!g_ms || !g_ms->canvas)
		return false;
	*w = g_ms->canvas->width();
	*h = g_ms->canvas->height();
	return true;
}

bool mb3dsdgMouseEdit(int x0, int y0, int x1, int y1) {
	if (!mb3dsdgIsOpen())
		return false;
	MsCanvas *c = g_ms->canvas;
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

bool mb3dsdgKey(int ch) {
	if (!mb3dsdgIsOpen())
		return false;
	const QString t = QString(QChar(char16_t(ch)));
	QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, t);
	QApplication::sendEvent(g_ms->canvas, &press);
	QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, t);
	QApplication::sendEvent(g_ms->canvas, &release);
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
	const double dpr = g_ms->canvas->devicePixelRatioF();
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
	w2i->SetInput(g_ms->canvas->renderWindow());
	w2i->ReadFrontBufferOff();
	w2i->Update();
	vtkNew<vtkPNGWriter> png;
	png->SetFileName(path.toUtf8().constData());
	png->SetInputConnection(w2i->GetOutputPort());
	png->Write();
	return QFile::exists(path);
}
