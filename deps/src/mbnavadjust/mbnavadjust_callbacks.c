/*--------------------------------------------------------------------
 *    The MB-system:  mbnavadjust_callbacks.c  2/22/2000
 *
 *    Copyright (c) 2000-2025 by
 *    David W. Caress (caress@mbari.org)
 *      Monterey Bay Aquarium Research Institute
 *      Moss Landing, California, USA
 *    Dale N. Chayes 
 *      Center for Coastal and Ocean Mapping
 *      University of New Hampshire
 *      Durham, New Hampshire, USA
 *    Christian dos Santos Ferreira
 *      MARUM
 *      University of Bremen
 *      Bremen Germany
 *     
 *    MB-System was created by Caress and Chayes in 1992 at the
 *      Lamont-Doherty Earth Observatory
 *      Columbia University
 *      Palisades, NY 10964
 *
 *    See README.md file for copying and redistribution conditions.
 *--------------------------------------------------------------------*/
/*
 * mbnavadjust is an interactive navigation adjustment package
 * for swath sonar data.
 * It can work with any data format supported by the MBIO library.
 * This file contains the callback functions from the MOTIF interface.
 *
 * Author:  D. W. Caress
 * Date:  March 22, 2000
 *
 * InteractiveGMT port, deps/src/mbnavadjust/mbnavadjust_callbacks.c: MB-System 5.8.3's
 * src/mbnavadjust/mbnavadjust_callbacks.c, the callbacks of the Motif interface, kept in C. What
 * changed, and only that:
 *   - Motif and Xt are the by-name widget bridge of mbna_motif.h: a Widget is its name, the
 *     objectName of the same widget in deps/ui/mbnavadjust*.ui, and every XtVaSetValues,
 *     toggle, list, label and text call goes to the Qt window, mbnavadjust_window.cpp, under the
 *     same text. The BuilderXcessory helpers -- BxManageCB and friends -- are gone with Motif.
 *   - What talked to X11 directly is the window's: the colours, graphics contexts, fonts and
 *     cursors of do_mbnavadjust_init and do_naverr_init, the expose and input events of the
 *     drawing areas, the modelplot window's resize, the event loop of do_wait_until_viewed and
 *     the file selection box. Each such function is rewritten here to the same effect and
 *     marked "port:".
 *   - The visualization, libmbview upstream, is the viewer's own window: the nav and route picks
 *     are the mbna_view_* hooks of mbnavadjust.h.
 *   - exit() is gone: this code runs inside the host process.
 */

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbnavadjust.h"
#include "mbna_motif.h"
#include "mbnavadjust_callbacks.h"

/*--------------------------------------------------------------------*/

/* id variables */
static const char program_name[] = "MBnavadjust";

/* port: the XG graphics ids are the window's canvases, handed over by mbna_window_canvases() */
static void *cont_xgid = NULL; /* XG graphics id */
static void *corr_xgid = NULL; /* XG graphics id */
static void *zoff_xgid = NULL; /* XG graphics id */
static void *modp_xgid = NULL; /* XG graphics id */

/* Set these to the dimensions of your canvas drawing */
/* areas, minus 1, located in the uil file       */
/* local_cont_borders/local_corr_borders hold the widget-derived canvas
    dimensions just long enough to seed the shared cont_borders/corr_borders
    (declared in mbnavadjust_core.h) via mbnavadjust_set_borders() below;
    named distinctly from the shared globals to avoid colliding with them. */
static int local_cont_borders[4] = {0, 600, 0, 600};
static int local_corr_borders[4] = {0, 301, 0, 301};
static int zoff_borders[4] = {0, 300, 0, 60};
static int modp_borders[4];

/* file opening parameters */
#define FILE_MODE_NONE 0
#define FILE_MODE_NEW 1
#define FILE_MODE_OPEN 2
#define FILE_MODE_IMPORTDATA 3
#define FILE_MODE_IMPORTSURVEY 4
#define FILE_MODE_REFERENCE 5
static size_t file_mode = FILE_MODE_NONE;
static int format = 0;

/* button parameters */
static bool button1down = false;
static bool button2down = false;
static bool button3down = false;
static int loc_x;
static int loc_y;

static int status;

/* the Arg list of the two XtSetValues calls */
static Cardinal ac = 0;
static Arg args[256];
static Boolean argok;
static XmString tmp0;

/* port: the visualization's pick notifications, called from the window's picks below */
static int do_visualize_dismiss_notify(size_t instance);
static void do_pickroute_notify(size_t instance);
static void do_picknav_notify(size_t instance);
static void mbna_view_clear_navadjust_picks(void);

/* the widgets: each is its Motif name, which is its objectName in deps/ui/mbnavadjust*.ui */
static Widget const bulletinBoard_modelplot = "bulletinBoard_modelplot";
static Widget const pushButton_modelplot_clearblock = "pushButton_modelplot_clearblock";
static Widget const radioBox = "radioBox";
static Widget const toggleButton_modelplot_timeseries = "toggleButton_modelplot_timeseries";
static Widget const toggleButton_modelplot_perturbation = "toggleButton_modelplot_perturbation";
static Widget const toggleButton_modelplot_tieoffsets = "toggleButton_modelplot_tieoffsets";
static Widget const label_modelplot_status = "label_modelplot_status";
static Widget const drawingArea_modelplot = "drawingArea_modelplot";
static Widget const scale_controls_zoffset = "scale_controls_zoffset";
static Widget const scale_controls_smoothing = "scale_controls_smoothing";
static Widget const scale_controls_sectionlength = "scale_controls_sectionlength";
static Widget const scale_controls_tickinterval = "scale_controls_tickinterval";
static Widget const scale_controls_colorinterval = "scale_controls_colorinterval";
static Widget const scale_controls_contourinterval = "scale_controls_contourinterval";
static Widget const scale_controls_decimation = "scale_controls_decimation";
static Widget const scale_controls_sectionsoundings = "scale_controls_sectionsoundings";
static Widget const label_radioBox_controls_use = "label_radioBox_controls_use";
static Widget const radioBox_controls_use = "radioBox_controls_use";
static Widget const toggleButton_controls_use_primary = "toggleButton_controls_use_primary";
static Widget const toggleButton_controls_use_secondary = "toggleButton_controls_use_secondary";
static Widget const toggleButton_controls_use_tertiary = "toggleButton_controls_use_tertiary";
static Widget const label_about_version = "label_about_version";
static Widget const pushButton_biases_applyall = "pushButton_biases_applyall";
static Widget const scale_biases_heading2 = "scale_biases_heading2";
static Widget const scale_biases_roll2 = "scale_biases_roll2";
static Widget const scale_biases_roll1 = "scale_biases_roll1";
static Widget const scale_biases_heading1 = "scale_biases_heading1";
static Widget const label_biases_files = "label_biases_files";
static Widget const toggleButton_biases_together = "toggleButton_biases_together";
static Widget const toggleButton_biases_separate = "toggleButton_biases_separate";
static Widget const bulletinBoard_naverr = "bulletinBoard_naverr";
static Widget const pushButton_naverr_unset = "pushButton_naverr_unset";
static Widget const pushButton_naverr_minxymisfit = "pushButton_naverr_minxymisfit";
static Widget const drawingArea_naverr_zcorr = "drawingArea_naverr_zcorr";
static Widget const pushButton_naverr_zerozoffset = "pushButton_naverr_zerozoffset";
static Widget const pushButton_naverr_settie = "pushButton_naverr_settie";
static Widget const toggleButton_misfitcenter_zero = "toggleButton_misfitcenter_zero";
static Widget const toggleButton_misfitcenter_auto = "toggleButton_misfitcenter_auto";
static Widget const pushButton_naverr_resettie = "pushButton_naverr_resettie";
static Widget const pushButton_naverr_addtie = "pushButton_naverr_addtie";
static Widget const pushButton_naverr_deletetie = "pushButton_naverr_deletetie";
static Widget const pushButton_naverr_fullsize = "pushButton_naverr_fullsize";
static Widget const pushButton_naverr_zerooffset = "pushButton_naverr_zerooffset";
static Widget const label_naverr_offsets = "label_naverr_offsets";
static Widget const pushButton_naverr_nextunset = "pushButton_naverr_nextunset";
static Widget const label_naverr_status = "label_naverr_status";
static Widget const drawingArea_naverr_cont = "drawingArea_naverr_cont";
static Widget const drawingArea_naverr_corr = "drawingArea_naverr_corr";
static Widget const pushButton_naverr_dismiss = "pushButton_naverr_dismiss";
static Widget const pushButton_naverr_selecttie = "pushButton_naverr_selecttie";
static Widget const pushButton_naverr_minimum = "pushButton_naverr_minimum";
static Widget const pushButton_naverr_next = "pushButton_naverr_next";
static Widget const pushButton_naverr_previous = "pushButton_naverr_previous";
static Widget const pushButton_naverr_setnone = "pushButton_naverr_setnone";
static Widget const xmDialogShell_fileselection = "xmDialogShell_fileselection";
static Widget const bulletinBoard_fileselection = "bulletinBoard_fileselection";
static Widget const textField_format = "textField_format";
static Widget const label_format = "label_format";
static Widget const fileSelectionBox = "fileSelectionBox";
static Widget const bulletinBoard_error = "bulletinBoard_error";
static Widget const label_error_two = "label_error_two";
static Widget const label_error_one = "label_error_one";
static Widget const label_error_three = "label_error_three";
static Widget const bulletinBoard_message = "bulletinBoard_message";
static Widget const label_message = "label_message";
static Widget const pushButton_new = "pushButton_new";
static Widget const pushButton_open = "pushButton_open";
static Widget const pushButton_close = "pushButton_close";
static Widget const pushButton_importdata = "pushButton_importdata";
static Widget const pushButton_importsurvey = "pushButton_importsurvey";
static Widget const pushButton_importreference = "pushButton_importreference";
static Widget const pushButton_quit = "pushButton_quit";
static Widget const pushButton_showreferencegrids = "pushButton_showreferencegrids";
static Widget const pushButton_showsurveys = "pushButton_showsurveys";
static Widget const pushButton_showblocks = "pushButton_showblocks";
static Widget const pushButton_showdata = "pushButton_showdata";
static Widget const pushButton_showsections = "pushButton_showsections";
static Widget const pushButton_showcrossings = "pushButton_showcrossings";
static Widget const pushButton_showmediocrecrossings = "pushButton_showmediocrecrossings";
static Widget const pushButton_showgoodcrossings = "pushButton_showgoodcrossings";
static Widget const pushButton_showbettercrossings = "pushButton_showbettercrossings";
static Widget const pushButton_showtruecrossings = "pushButton_showtruecrossings";
static Widget const pushButton_showcrossingties = "pushButton_showcrossingties";
static Widget const pushButton_showcrossingtiessortedall = "pushButton_showcrossingtiessortedall";
static Widget const pushButton_showcrossingtiessortedworst = "pushButton_showcrossingtiessortedworst";
static Widget const pushButton_showcrossingtiessortedbad = "pushButton_showcrossingtiessortedbad";
static Widget const pushButton_showglobalties = "pushButton_showglobalties";
static Widget const pushButton_showglobaltiessorted = "pushButton_showglobaltiessorted";
static Widget const toggleButton_showallsurveys = "toggleButton_showallsurveys";
static Widget const toggleButton_showselectedsurvey = "toggleButton_showselectedsurvey";
static Widget const toggleButton_showselectedblock = "toggleButton_showselectedblock";
static Widget const toggleButton_showselectedfile = "toggleButton_showselectedfile";
static Widget const toggleButton_showwithselectedsurvey = "toggleButton_showwithselectedsurvey";
static Widget const toggleButton_showwithselectedfile = "toggleButton_showwithselectedfile";
static Widget const toggleButton_showselectedsection = "toggleButton_showselectedsection";
static Widget const pushButton_visualize = "pushButton_visualize";
static Widget const pushButton_showmodelplot = "pushButton_showmodelplot";
static Widget const pushButton_poornav = "pushButton_poornav";
static Widget const pushButton_goodnav = "pushButton_goodnav";
static Widget const pushButton_fixednav = "pushButton_fixednav";
static Widget const pushButton_fixedxynav = "pushButton_fixedxynav";
static Widget const pushButton_fixedznav = "pushButton_fixedznav";
static Widget const pushButton_tie_xy = "pushButton_tie_xy";
static Widget const pushButton_tie_z = "pushButton_tie_z";
static Widget const pushButton_tie_xyz = "pushButton_tie_xyz";
static Widget const pushButton_tie_unfixed = "pushButton_tie_unfixed";
static Widget const pushButton_tie_fixed = "pushButton_tie_fixed";
static Widget const pushButton_autopick = "pushButton_autopick";
static Widget const pushButton_autopickhorizontal = "pushButton_autopickhorizontal";
static Widget const pushButton_autosetsvsvertical = "pushButton_autosetsvsvertical";
static Widget const pushButton_newcrossings = "pushButton_newcrossings";
static Widget const pushButton_analyzecrossings = "pushButton_analyzecrossings";
static Widget const pushButton_zerozoffsets = "pushButton_zerozoffsets";
static Widget const pushButton_unsetskipped = "pushButton_unsetskipped";
static Widget const pushButton_invertnav = "pushButton_invertnav";
static Widget const pushButton_updategrids = "pushButton_updategrids";
static Widget const pushButton_applynav = "pushButton_applynav";
static Widget const form_mbnavadjust = "form_mbnavadjust";
static Widget const scrolledWindow_messages = "scrolledWindow_messages";
static Widget const text_messages = "text_messages";
static Widget const label_status = "label_status";
static Widget const label_listdata = "label_listdata";
static Widget const scrolledWindow_datalist = "scrolledWindow_datalist";
static Widget const list_data = "list_data";
static Widget const fileSelectionBox_list = "fileSelectionBox_list";
static Widget const fileSelectionBox_text = "fileSelectionBox_text";

/*--------------------------------------------------------------------*/

/* port: no translation tables, no X colormap. The drawing colours are the window's colour table,
   in upstream's order: the MB_NDrawingColors named colours, then the 5 x 16 misfit ramp and its last
   colour -- so a "pixel value" is simply its index (mbnavadjust_window.cpp builds the same table).
   The command line's -I, -D and -R are the arguments; the file selection box's *.nvh pattern is the
   window's file dialog filter. The window hands its canvases over first (mbna_window_canvases). */
void do_mbnavadjust_init(const char *ifile, bool dark_background, bool reset_crossings) {
  /* Load the colors that will be used in this program. */
  static int mpixel_values[MB_NDrawingColors + 16 * 5 + 1];
  for (int i = 0; i < MB_NDrawingColors + 16 * 5 + 1; i++)
    mpixel_values[i] = i;

  status = mbnavadjust_set_colors(MB_NDrawingColors + 16 * 5 + 1, mpixel_values);
  status = mbnavadjust_set_borders(local_cont_borders, local_corr_borders, zoff_borders);

  /* set verbose */
  mbna_verbose = 0;

  /* put up info text */
  mb_path string = "";
  snprintf(string, sizeof(string), "Program MBnavadjust initialized.\nMB-System Release %s %s\n", MB_VERSION, MB_VERSION_DATE);
  do_info_add(string, true);

  /* initialize mbnavadjust proper */
  status = mbnavadjust_init_globals();
  status = mbnavadjust_init(ifile, dark_background, reset_crossings);
  do_set_controls();
  do_update_status();
}

/* port: the window's four drawing areas, as XG graphics ids, with the borders of the two naverr
   canvases the window gave them (their size minus one, as the uil file has them) */
void mbna_window_canvases(void *cont, void *corr, void *zoff, void *modp) {
  cont_xgid = cont;
  corr_xgid = corr;
  zoff_xgid = zoff;
  modp_xgid = modp;
}

/*--------------------------------------------------------------------*/

void do_set_controls() {
  char value_text[MB_PATH_MAXLINE];

  /* set about version label */
  snprintf(value_text, sizeof(value_text), ":::t\"MB-System Release %s\":t\"%s\"", MB_VERSION, MB_VERSION_DATE);
  set_label_multiline_string(label_about_version, value_text);

  /* set value of format text item */
  mb_path string = "";
  snprintf(string, sizeof(string), "%2.2d", format);
  XmTextFieldSetString(textField_format, string);

  /* set model view style togglebuttons */
  if (project.modelplot_style == MBNA_MODELPLOT_TIMESERIES) {
    XmToggleButtonSetState(toggleButton_modelplot_timeseries, TRUE, TRUE);
  }
  else if (project.modelplot_style == MBNA_MODELPLOT_PERTURBATION) {
    XmToggleButtonSetState(toggleButton_modelplot_perturbation, TRUE, TRUE);
  }
  else {
    XmToggleButtonSetState(toggleButton_modelplot_tieoffsets, TRUE, TRUE);
  }
}
/*--------------------------------------------------------------------*/

void do_update_status() {

  /* set status label */
  mb_path refgrid_name;
  char *use_mode_primary = "Primary";
  char *use_mode_secondary = "Secondary";
  char *use_mode_tertiary = "Tertiary";
  char *use_mode = NULL;
  if (project.use_mode == MBNA_USE_MODE_PRIMARY)
    use_mode = use_mode_primary;
  else if (project.use_mode == MBNA_USE_MODE_SECONDARY)
    use_mode = use_mode_secondary; 
  else if (project.use_mode == MBNA_USE_MODE_TERTIARY)
    use_mode = use_mode_tertiary; 
  else {
    project.use_mode = MBNA_USE_MODE_PRIMARY;
    use_mode = use_mode_primary;
  }
  if (project.refgrid_select >= 0 && project.refgrid_select < project.num_refgrids)
    strcpy(refgrid_name, project.refgrid_names[project.refgrid_select]);
  else
    strcpy(refgrid_name, "<Previously Selected Reference Grid>");
  mb_command string = "";
  snprintf(string, sizeof(string), 
    ":::t\"Project: %s\""
    ":t\"Navigation Adjustment Use Mode:       %s\""
    ":t\"Number of Files:                               %4d      Selected Survey:  %4d\""
    ":t\"Number of Crossings Found:             %4d     Selected File:    %4d\""
    ":t\"Number of Crossings Analyzed:        %4d     Selected Section: %4d\""
    ":t\"Number of True Crossings:               %4d     Selected Crossing:%4d\""
    ":t\"Number of True Crossings Analyzed: %4d     Selected Tie:     %4d\""
    ":t\"Number of Ties Set:                        %4d\""
    ":t\"Number of Global Ties Set:              %4d\""
    ":t\"Reference Grid: %s\"",
    project.name, use_mode, project.num_files, mbna_survey_select, project.num_crossings, mbna_file_select,
    project.num_crossings_analyzed, mbna_section_select, project.num_truecrossings, mbna_crossing_select,
    project.num_truecrossings_analyzed, mbna_tie_select, project.num_ties, project.num_globalties, refgrid_name);

  if (project.inversion_status == MBNA_INVERSION_CURRENT)
    strcat(string, ":t\"Inversion Performed:                     Current\"");
  else if (project.inversion_status == MBNA_INVERSION_OLD)
    strcat(string, ":t\"Inversion Performed:                     Out of Date\"");
  else
    strcat(string, ":t\"Inversion Performed:                         No\"");
  if (project.grid_status == MBNA_GRID_CURRENT)
    strcat(string, ":t\"Topography Grid Status:                      Current\"");
  else if (project.grid_status == MBNA_GRID_OLD)
    strcat(string, ":t\"Topography Grid Status:                    Out of Date\"");
  else
    strcat(string, ":t\"Topography Grid Status:                    Not made yet\"");
  set_label_multiline_string(label_status, string);
  if (mbna_verbose > 0) {
    snprintf(string, sizeof(string),
      "Project:                           %s\n"
      "Number of Files:                   %d\n"
      "Number of Crossings Found:         %d\n"
      "Number of Crossings Analyzed:      %d\n"
      "Number of True Crossings:          %d\n"
      "Number of True Crossings Analyzed: %d\n"
      "Number of Ties Set:                %d\n"
      "Number of Global Ties Set:         %d\n"
      "Reference Grid:                    %s\n",
      project.name, project.num_files, project.num_crossings, project.num_crossings_analyzed, project.num_truecrossings,
      project.num_truecrossings_analyzed, project.num_ties, project.num_globalties, refgrid_name);
    if (project.inversion_status == MBNA_INVERSION_CURRENT)
      strcat(string, "Inversion Performed:               Current\n");
    else if (project.inversion_status == MBNA_INVERSION_OLD)
      strcat(string, "Inversion Performed:               Out of Date\n");
    else
      strcat(string, "Inversion Performed:               No\n");
    if (project.grid_status == MBNA_GRID_CURRENT)
      strcat(string, "Topography Grid Status:            Current\n");
    else if (project.grid_status == MBNA_GRID_OLD)
      strcat(string, "Topography Grid Status:            Out of Date\n");
    else
      strcat(string, "Topography Grid Status:            Not made yet\n");
    fprintf(stderr, "%s", string);
  }

  char *tiestatus_xyz =   "XYZU";
  char *tiestatus_xy =    "XY_U";
  char *tiestatus_z =     "__ZU";
  char *tiestatus_xyz_f = "XYZF";
  char *tiestatus_xy_f =  "XY_F";
  char *tiestatus_z_f =   "__ZF";
  char *filestatus;
  char *filestatus_poor = " poor  ";
  char *filestatus_good = " good  ";
  char *filestatus_fixed = " fixed ";
  char *filestatus_fixedxy = "fixedxy";
  char *filestatus_fixedz = "fixedz ";
  char *filestatus_unknown = "unknown";

  /* set list_data */
  int iselect = MBNA_SELECT_NONE;
  XmListDeleteAllItems(list_data);
  if (mbna_view_list == MBNA_VIEW_LIST_REFERENCEGRIDS) {
    snprintf(string, sizeof(string), "Reference Grids:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_refgrids > 0) {
      XmString *xstr = (XmString *)malloc((project.num_refgrids + 1) * sizeof(XmString));
      if (xstr != NULL) {
        xstr[0] = XmStringCreateLocalized("<Previously Selected Grid>");
        if (mbna_verbose > 0)
          fprintf(stderr, "<Previously Selected Grid>\n");
        for (int i = 0; i < project.num_refgrids; i++) {
          xstr[i+1] = XmStringCreateLocalized(project.refgrid_names[i]);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", project.refgrid_names[i]);
        }
        XmListAddItems(list_data, xstr, project.num_refgrids + 1, 0);
        for (int i = 0; i < project.num_refgrids + 1; i++) {
          XmStringFree(xstr[i]);
        }
        free(xstr);
      }
    }
    XmListSelectPos(list_data, project.refgrid_select + 2, 0);
    XmListSetPos(list_data, MAX(project.refgrid_select + 2 - 5, 1));
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS) {
    snprintf(string, sizeof(string), "Surveys:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_surveys > 0 && project.num_files > 0) {
      XmString *xstr = (XmString *)malloc(project.num_surveys * sizeof(XmString));
      if (xstr != NULL) {
    	for (int isurvey = 0; isurvey < project.num_surveys; isurvey++) {
    		int num_files_survey = 0;
    		int num_global_ties_survey = 0;
				bool first = true;
				double btime_d = 0.0;
				double etime_d = 0.0;
    		for (int ifile=0; ifile < project.num_files; ifile++) {
					struct mbna_file *file = &(project.files[ifile]);
	
					if (file->survey == isurvey) {
						if (first) {
							btime_d = file->sections[0].btime_d;
							first = false;
							if (file->status == MBNA_FILE_POORNAV)
								filestatus = filestatus_poor;
							else if (file->status == MBNA_FILE_GOODNAV)
								filestatus = filestatus_good;
							else if (file->status == MBNA_FILE_FIXEDNAV)
								filestatus = filestatus_fixed;
							else if (file->status == MBNA_FILE_FIXEDXYNAV)
								filestatus = filestatus_fixedxy;
							else if (file->status == MBNA_FILE_FIXEDZNAV)
								filestatus = filestatus_fixedz;
							else
								filestatus = filestatus_unknown;
						}
						num_files_survey++;
						etime_d = file->sections[file->num_sections-1].etime_d;
						for (int isection=0; isection < file->num_sections; isection++) {
							struct mbna_section *section = &file->sections[isection];
							if (section->globaltie.status != MBNA_TIE_NONE) {
								num_global_ties_survey++;
							}
						}
					}
        }
			int btime_i[7], etime_i[7];
			mb_get_date(mbna_verbose, btime_d, btime_i);
			mb_get_date(mbna_verbose, etime_d, etime_i);
			mb_path string = "";
			snprintf(string, sizeof(string), 
				"%3d %3d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d %s g-ties:%d",
				isurvey, num_files_survey, btime_i[0], btime_i[1], btime_i[2], btime_i[3], btime_i[4], btime_i[5],
				btime_i[6], etime_i[0], etime_i[1], etime_i[2], etime_i[3], etime_i[4], etime_i[5], etime_i[6],
				filestatus, num_global_ties_survey);
			xstr[isurvey] = XmStringCreateLocalized(string);
			if (mbna_verbose > 0)
				fprintf(stderr, "%s\n", string);
      }
      XmListAddItems(list_data, xstr, project.num_surveys, 0);
      for (int i = 0; i < project.num_surveys; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (mbna_survey_select != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, mbna_survey_select + 1, 0);
      XmListSetPos(list_data, MAX(mbna_survey_select + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_BLOCKS) {
    snprintf(string, sizeof(string), "Survey-vs-Survey Blocks:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* calculate the number of blocks */
      int num_blocks = project.num_surveys + (project.num_surveys * (project.num_surveys - 1) / 2);
      XmString *xstr = (XmString *)malloc(num_blocks * sizeof(XmString));
      int *survey1 = (int *)malloc(num_blocks * sizeof(int));
      int *survey2 = (int *)malloc(num_blocks * sizeof(int));
      int *n_tcrossing = (int *)malloc(num_blocks * sizeof(int));
      int *n_50crossing = (int *)malloc(num_blocks * sizeof(int));
      int *n_25crossing = (int *)malloc(num_blocks * sizeof(int));
      int *n_allcrossing = (int *)malloc(num_blocks * sizeof(int));
      int *n_tie = (int *)malloc(num_blocks * sizeof(int));
      if (xstr == NULL || survey1 == NULL || survey2 == NULL || n_tcrossing == NULL
          || n_50crossing == NULL || n_25crossing == NULL || n_allcrossing == NULL || n_tie == NULL) {
        free(xstr);
        free(survey1);
        free(survey2);
        free(n_tcrossing);
        free(n_50crossing);
        free(n_25crossing);
        free(n_allcrossing);
        free(n_tie);
      } else {
      memset(survey1, 0, (num_blocks * sizeof(int)));
      memset(survey2, 0, (num_blocks * sizeof(int)));
      memset(n_tcrossing, 0, (num_blocks * sizeof(int)));
      memset(n_50crossing, 0, (num_blocks * sizeof(int)));
      memset(n_25crossing, 0, (num_blocks * sizeof(int)));
      memset(n_allcrossing, 0, (num_blocks * sizeof(int)));
      memset(n_tie, 0, (num_blocks * sizeof(int)));
      int iblock = 0;
      for (int isurvey2=0; isurvey2 < project.num_surveys; isurvey2++) {
        for (int isurvey1 = 0; isurvey1 <= isurvey2; isurvey1++) {
          survey1[iblock] = isurvey1;
          survey2[iblock] = isurvey2;
          iblock++;
        }
      }
      /* generate list */
      for (int k = 0; k < project.num_crossings; k++) {
        struct mbna_crossing *crossing = &project.crossings[k];
        int iblock = project.files[crossing->file_id_1].survey
               + (project.files[crossing->file_id_2].survey * (project.files[crossing->file_id_2].survey + 1) / 2);
        if (crossing->truecrossing)
          n_tcrossing[iblock]++;
        if (crossing->overlap >= 50)
          n_50crossing[iblock]++;
        if (crossing->overlap >= 25)
          n_25crossing[iblock]++;
        n_allcrossing[iblock]++;
        n_tie[iblock] += crossing->num_ties;
      }

      int nblocklist = 0;
      int iblocklist_select = MBNA_SELECT_NONE;
      for (int iblock=0; iblock < num_blocks; iblock++) {
        if (mbna_view_mode == MBNA_VIEW_MODE_ALL
            || (mbna_view_mode == MBNA_VIEW_MODE_SURVEY
          && survey1[iblock] == mbna_survey_select
          && survey2[iblock] == mbna_survey_select)
            || (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
            || (mbna_view_mode == MBNA_VIEW_MODE_FILE
          && survey1[iblock] == project.files[mbna_file_select].survey
          && survey2[iblock] == project.files[mbna_file_select].survey)
            || (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY
          && (survey1[iblock] == mbna_survey_select
              || survey2[iblock] == mbna_survey_select))
            || (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE
          && (survey1[iblock] == project.files[mbna_file_select].survey
              || survey2[iblock] == project.files[mbna_file_select].survey))
            || (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION
          && (survey1[iblock] == project.files[mbna_file_select].survey
              || survey2[iblock] == project.files[mbna_file_select].survey))) {
          snprintf(string, sizeof(string), "block %4.4d: Survey %2.2d vs Survey %2.2d : Crossings: %4d %4d %4d %4d : Ties: %4d",
            iblock, survey1[iblock], survey2[iblock],
            n_tcrossing[iblock], n_50crossing[iblock], n_25crossing[iblock],
            n_allcrossing[iblock], n_tie[iblock]);
          xstr[nblocklist] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (iblock == mbna_survey_select)
            iblocklist_select = nblocklist;
          nblocklist++;
        }
      }
fprintf(stderr, "%s:%d:%s: \n", __FILE__, __LINE__, __FUNCTION__);
      XmListAddItems(list_data, xstr, nblocklist, 0);
fprintf(stderr, "%s:%d:%s: \n", __FILE__, __LINE__, __FUNCTION__);
      for (int iblocklist = 0; iblocklist < nblocklist; iblocklist++) {
        XmStringFree(xstr[iblocklist]);
      }
fprintf(stderr, "%s:%d:%s: \n", __FILE__, __LINE__, __FUNCTION__);
      free(xstr);
      free(survey1);
      free(survey2);
      free(n_tcrossing);
      free(n_50crossing);
      free(n_25crossing);
      free(n_allcrossing);
      free(n_tie);
fprintf(stderr, "%s:%d:%s: \n", __FILE__, __LINE__, __FUNCTION__);
      if (mbna_survey_select != MBNA_SELECT_NONE && iblocklist_select != MBNA_SELECT_NONE) {
        XmListSelectPos(list_data, iblocklist_select + 1, 0);
        XmListSetPos(list_data, MAX(iblocklist_select + 1 - 5, 1));
      }
fprintf(stderr, "%s:%d:%s: \n", __FILE__, __LINE__, __FUNCTION__);
      }
    }
fprintf(stderr, "%s:%d:%s: \n", __FILE__, __LINE__, __FUNCTION__);
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_FILES) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Data Files:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Data Files of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Data Files of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Data File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Data Files of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Data File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Data File of Selected Section %d:%d:%d:", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "Data Files:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count files */
      int num_files = 0;
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
            (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
            (mbna_view_mode == MBNA_VIEW_MODE_BLOCK && (mbna_survey_select1 == file->survey || mbna_survey_select2 == file->survey)) ||
            (mbna_view_mode == MBNA_VIEW_MODE_FILE) ||
            (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
            (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE) || (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION))
          num_files++;
      }
      XmString *xstr = (XmString *)malloc(num_files * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_files = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
            (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
            (mbna_view_mode == MBNA_VIEW_MODE_FILE) ||
            (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
            (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE) || (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)) {
          if (file->status == MBNA_FILE_POORNAV)
            filestatus = filestatus_poor;
          else if (file->status == MBNA_FILE_GOODNAV)
            filestatus = filestatus_good;
          else if (file->status == MBNA_FILE_FIXEDNAV)
            filestatus = filestatus_fixed;
          else if (file->status == MBNA_FILE_FIXEDXYNAV)
            filestatus = filestatus_fixedxy;
          else if (file->status == MBNA_FILE_FIXEDZNAV)
            filestatus = filestatus_fixedz;
          else
            filestatus = filestatus_unknown;
          int num_global_ties = 0;
          for (int isection=0; isection < file->num_sections; isection++) {
            struct mbna_section *section = &file->sections[isection];
            if (section->globaltie.status != MBNA_TIE_NONE) {
              num_global_ties++;
            }
          }
          snprintf(string, sizeof(string), "%2.2d:%4.4d %s %4d %4.1f %4.1f g-ties:%d  %s", 
          	file->survey, file->id, filestatus, file->num_sections,
            file->heading_bias, file->roll_bias, num_global_ties, file->file);
          xstr[num_files] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (i == mbna_file_select)
            iselect = num_files;
          num_files++;
        }
      }
      XmListAddItems(list_data, xstr, num_files, 0);
      for (int i = 0; i < num_files; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Data File Sections:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Data File Sections of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Data Files Sections of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Data File Sections of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Data File Sections of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Data File Sections of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Data File Sections of File %d:%d:", mbna_survey_select, mbna_file_select);
    else
      snprintf(string, sizeof(string), "Data Files Sections:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count sections */
      int num_sections = 0;
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
              (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
              (mbna_view_mode == MBNA_VIEW_MODE_BLOCK && (mbna_survey_select1 == file->survey || mbna_survey_select2 == file->survey)) ||
              (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i)) {
            num_sections++;
          }
        }
      }
      XmString *xstr = (XmString *)malloc(num_sections * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_sections = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          struct mbna_section *section = &(file->sections[j]);
          if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
              (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
              (mbna_view_mode == MBNA_VIEW_MODE_BLOCK && (mbna_survey_select1 == file->survey || mbna_survey_select2 == file->survey)) ||
              (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i)) {
            int btime_i[7], etime_i[7];
            mb_get_date(mbna_verbose, section->btime_d, btime_i);
            mb_get_date(mbna_verbose, section->etime_d, etime_i);
            char status_char;
            if (section->status == MBNA_CROSSING_STATUS_NONE)
              status_char = 'U';
            else if (section->status == MBNA_CROSSING_STATUS_SET)
              status_char = '*';
            else
              status_char = '-';
            if (section->status != MBNA_CROSSING_STATUS_SET)
              snprintf(string, sizeof(string), 
                "%c %2.2d:%4.4d:%2.2d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d %4.4d/%2.2d/%2.2d "
                "%2.2d:%2.2d:%2.2d.%6.6d",
                status_char, file->survey, file->id, j, btime_i[0], btime_i[1], btime_i[2], btime_i[3], btime_i[4], btime_i[5],
                btime_i[6], etime_i[0], etime_i[1], etime_i[2], etime_i[3], etime_i[4], etime_i[5], etime_i[6]);
            else {
              char *tiestatus = NULL;
              if (section->globaltie.status == MBNA_TIE_XYZ)
                tiestatus = tiestatus_xyz;
              else if (section->globaltie.status == MBNA_TIE_XY)
                tiestatus = tiestatus_xy;
              else if (section->globaltie.status == MBNA_TIE_Z)
                tiestatus = tiestatus_z;
              else if (section->globaltie.status == MBNA_TIE_XYZ_FIXED)
                tiestatus = tiestatus_xyz_f;
              else if (section->globaltie.status == MBNA_TIE_XY_FIXED)
                tiestatus = tiestatus_xy_f;
              else if (section->globaltie.status == MBNA_TIE_Z_FIXED)
                tiestatus = tiestatus_z_f;
							int refgrid_id = project.refgrid_select;
							mb_path refgrid_name = " | No reference grid";
							if (section->globaltie.refgrid_id >= 0 && project.refgrid_select < 0) {
								refgrid_id = section->globaltie.refgrid_id;
							}
							if (refgrid_id >= 0 && refgrid_id < project.num_refgrids) {
								strncpy(refgrid_name, project.refgrid_names[refgrid_id], sizeof(refgrid_name));
							}
              if (section->globaltie.inversion_status == MBNA_INVERSION_CURRENT)
                snprintf(string, sizeof(string), 
                  "%c %2.2d:%4.4d:%2.2d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d"
                  " | %2.2d %s %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f | %s",
                  status_char, file->survey, file->id, j,
                  btime_i[0], btime_i[1], btime_i[2], btime_i[3], btime_i[4], btime_i[5], btime_i[6],
                  etime_i[0], etime_i[1], etime_i[2], etime_i[3], etime_i[4], etime_i[5], etime_i[6],
                  section->globaltie.snav, tiestatus,
                  section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
                  section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3,
                  section->globaltie.dx_m, section->globaltie.dy_m, section->globaltie.dz_m,
                  section->globaltie.sigma_m, section->globaltie.rsigma_m,
                  refgrid_name);
              else if (section->globaltie.inversion_status == MBNA_INVERSION_OLD)
                snprintf(string, sizeof(string), 
                  "%c %2.2d:%4.4d:%2.2d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d"
                  " | %2.2d %s %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f *** | %s",
                  status_char, file->survey, file->id, j,
                  btime_i[0], btime_i[1], btime_i[2], btime_i[3], btime_i[4], btime_i[5], btime_i[6],
                  etime_i[0], etime_i[1], etime_i[2], etime_i[3], etime_i[4], etime_i[5], etime_i[6],
                  section->globaltie.snav, tiestatus,
                  section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
                  section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3,
                  section->globaltie.dx_m, section->globaltie.dy_m, section->globaltie.dz_m,
                  section->globaltie.sigma_m, section->globaltie.rsigma_m,
                  refgrid_name);
              else
                snprintf(string, sizeof(string), 
                  "%c %2.2d:%4.4d:%2.2d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%6.6d"
                  " | %2.2d %s %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f",
                  status_char, file->survey, file->id, j,
                  btime_i[0], btime_i[1], btime_i[2], btime_i[3], btime_i[4], btime_i[5], btime_i[6],
                  etime_i[0], etime_i[1], etime_i[2], etime_i[3], etime_i[4], etime_i[5], etime_i[6],
                  section->globaltie.snav, tiestatus,
                  section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
                  section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3);
 
            }
            xstr[num_sections] = XmStringCreateLocalized(string);
            if (mbna_verbose > 0)
              fprintf(stderr, "%s\n", string);
            if (i == mbna_file_select && j == mbna_section_select)
              iselect = num_sections;
            num_sections++;
          }
        }
      }
      XmListAddItems(list_data, xstr, num_sections, 0);
      for (int i = 0; i < num_sections; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Crossings:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Crossings of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Crossings of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Crossings of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Crossings with Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Crossings with File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Crossings with Section %d:%d:%d:", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "Crossings:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossings */
      int num_crossings = 0;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i))
          num_crossings++;
      }
      XmString *xstr = (XmString *)malloc(num_crossings * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_crossings = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          char status_char = '-';
          char truecrossing = ' ';
          if (crossing->status == MBNA_CROSSING_STATUS_NONE)
            status_char = 'U';
          else if (crossing->status == MBNA_CROSSING_STATUS_SET)
            status_char = '*';
          else
            status_char = '-';
          if (!crossing->truecrossing)
            truecrossing = ' ';
          else
            truecrossing = 'X';
          snprintf(string, sizeof(string), "%c%c %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d %3d %2d", status_char, truecrossing, i,
            project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
            project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, crossing->overlap,
            crossing->num_ties);
          xstr[num_crossings] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (i == mbna_crossing_select)
            iselect = num_crossings;
          num_crossings++;
        }
      }
      XmListAddItems(list_data, xstr, num_crossings, 0);
      for (int i = 0; i < num_crossings; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_MEDIOCRECROSSINGS) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings with Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings with File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), ">10%% Overlap Crossings with Section %d:%d:%d:", mbna_survey_select, mbna_file_select,
        mbna_section_select);
    else
      snprintf(string, sizeof(string), ">10%% Overlap Crossings:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossings */
      int num_crossings = 0;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i))
          num_crossings++;
      }
      XmString *xstr = (XmString *)malloc(num_crossings * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_crossings = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          char status_char = '-';
          char truecrossing = ' ';
          if (crossing->status == MBNA_CROSSING_STATUS_NONE)
            status_char = 'U';
          else if (crossing->status == MBNA_CROSSING_STATUS_SET)
            status_char = '*';
          else
            status_char = '-';
          if (!crossing->truecrossing)
            truecrossing = ' ';
          else
            truecrossing = 'X';
          snprintf(string, sizeof(string), "%c%c %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d %3d %2d", status_char, truecrossing, i,
            project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
            project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, crossing->overlap,
            crossing->num_ties);
          xstr[num_crossings] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (i == mbna_crossing_select)
            iselect = num_crossings;
          num_crossings++;
        }
      }
      XmListAddItems(list_data, xstr, num_crossings, 0);
      for (int i = 0; i < num_crossings; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings with Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings with File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), ">25%% Overlap Crossings with Section %d:%d:%d:", mbna_survey_select, mbna_file_select,
        mbna_section_select);
    else
      snprintf(string, sizeof(string), ">25%% Overlap Crossings:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossings */
      int num_crossings = 0;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i))
          num_crossings++;
      }
      XmString *xstr = (XmString *)malloc(num_crossings * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_crossings = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          char status_char = '-';
          char truecrossing = ' ';
          if (crossing->status == MBNA_CROSSING_STATUS_NONE)
            status_char = 'U';
          else if (crossing->status == MBNA_CROSSING_STATUS_SET)
            status_char = '*';
          else
            status_char = '-';
          if (!crossing->truecrossing)
            truecrossing = ' ';
          else
            truecrossing = 'X';
          snprintf(string, sizeof(string), "%c%c %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d %3d %2d", status_char, truecrossing, i,
            project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
            project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, crossing->overlap,
            crossing->num_ties);
          xstr[num_crossings] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (i == mbna_crossing_select)
            iselect = num_crossings;
          num_crossings++;
        }
      }
      XmListAddItems(list_data, xstr, num_crossings, 0);
      for (int i = 0; i < num_crossings; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_BETTERCROSSINGS) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), ">50%% Crossings:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), ">50%% Crossings of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), ">50%% Overlap Crossings of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), ">50%% Crossings of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), ">50%% Overlap Crossings with Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), ">50%% Overlap Crossings with File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), ">50%% Overlap Crossings with Section %d:%d:%d:", mbna_survey_select, mbna_file_select,
        mbna_section_select);
    else
      snprintf(string, sizeof(string), ">50%% Crossings:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossings */
      int num_crossings = 0;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i))
          num_crossings++;
      }
      XmString *xstr = (XmString *)malloc(num_crossings * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_crossings = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          char status_char = '-';
          char truecrossing = ' ';
          if (crossing->status == MBNA_CROSSING_STATUS_NONE)
            status_char = 'U';
          else if (crossing->status == MBNA_CROSSING_STATUS_SET)
            status_char = '*';
          else
            status_char = '-';
          if (!crossing->truecrossing)
            truecrossing = ' ';
          else
            truecrossing = 'X';
          snprintf(string, sizeof(string), "%c%c %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d %3d %2d", status_char, truecrossing, i,
            project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
            project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, crossing->overlap,
            crossing->num_ties);
          xstr[num_crossings] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (i == mbna_crossing_select)
            iselect = num_crossings;
          num_crossings++;
        }
      }
      XmListAddItems(list_data, xstr, num_crossings, 0);
      for (int i = 0; i < num_crossings; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "True Crossings:");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "True Crossings of Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "True Crossings of Survey-vs-Survey Block %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "True Crossings of File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "True Crossings with Survey %d:", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "True Crossings with File %d:%d:", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "True Crossings with Section %d:%d:%d:", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "True Crossings:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossings */
      int num_crossings = 0;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i))
          num_crossings++;
      }
      XmString *xstr = (XmString *)malloc(num_crossings * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_crossings = 0;
      iselect = MBNA_SELECT_NONE;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          char status_char = '-';
          char truecrossing = ' ';
          if (crossing->status == MBNA_CROSSING_STATUS_NONE)
            status_char = 'U';
          else if (crossing->status == MBNA_CROSSING_STATUS_SET)
            status_char = '*';
          else
            status_char = '-';
          if (crossing->truecrossing)
            truecrossing = ' ';
          else
            truecrossing = 'X';
          snprintf(string, sizeof(string), "%c%c %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d %3d %2d", status_char, truecrossing, i,
            project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
            project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, crossing->overlap,
            crossing->num_ties);
          xstr[num_crossings] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (i == mbna_crossing_select)
            iselect = num_crossings;
          num_crossings++;
        }
      }
      XmListAddItems(list_data, xstr, num_crossings, 0);
      for (int i = 0; i < num_crossings; i++) {
        XmStringFree(xstr[i]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_TIES) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Ties:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Ties of Survey %d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Ties of Survey-vs-Survey Block %d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Ties of File %d:%d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Ties with Survey %d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Ties with File %d:%d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Ties with Section %d:%d:%d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "Ties:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossing ties and global ties */
      int num_ties = 0;

      /* count crossing ties */
      project.tiessortedthreshold = 0.0;
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          num_ties += crossing->num_ties;
        }
      }

      /* allocate strings for list */
      XmString *xstr = (XmString *)malloc(num_ties * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_ties = 0;
      iselect = MBNA_SELECT_NONE;

      /* list crossing ties */
      for (int i = 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          struct mbna_crossing *crossing = &(project.crossings[i]);
          for (int j = 0; j < crossing->num_ties; j++) {
            struct mbna_tie *tie = (struct mbna_tie *)&crossing->ties[j];
            char *tiestatus = NULL;
            if (tie->status == MBNA_TIE_XYZ)
              tiestatus = tiestatus_xyz;
            else if (tie->status == MBNA_TIE_XY)
              tiestatus = tiestatus_xy;
            else if (tie->status == MBNA_TIE_Z)
              tiestatus = tiestatus_z;
            else if (tie->status == MBNA_TIE_XYZ_FIXED)
              tiestatus = tiestatus_xyz_f;
            else if (tie->status == MBNA_TIE_XY_FIXED)
              tiestatus = tiestatus_xy_f;
            else if (tie->status == MBNA_TIE_Z_FIXED)
              tiestatus = tiestatus_z_f;
            if (tie->inversion_status == MBNA_INVERSION_CURRENT) {
              sprintf(string,
                "%6d %2d %s %2.2d:%4.4d:%3.3d:%2.2d %2.2d:%4.4d:%3.3d:%2.2d "
                "%8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f",
                i, j, tiestatus, project.files[crossing->file_id_1].survey, crossing->file_id_1,
                crossing->section_1, tie->snav_1, project.files[crossing->file_id_2].survey,
                crossing->file_id_2, crossing->section_2, tie->snav_2,
                tie->offset_x_m, tie->offset_y_m, tie->offset_z_m,
                tie->sigmar1, tie->sigmar2, tie->sigmar3,
                tie->dx_m, tie->dy_m, tie->dz_m, tie->sigma_m, tie->rsigma_m);
            }
            else if (tie->inversion_status == MBNA_INVERSION_OLD) {
              sprintf(string,
                "%6d %2d %s %2.2d:%4.4d:%3.3d:%2.2d %2.2d:%4.4d:%3.3d:%2.2d "
                "%8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f ***",
                i, j, tiestatus, project.files[crossing->file_id_1].survey, crossing->file_id_1,
                crossing->section_1, tie->snav_1, project.files[crossing->file_id_2].survey,
                crossing->file_id_2, crossing->section_2, tie->snav_2,
                tie->offset_x_m, tie->offset_y_m, tie->offset_z_m,
                tie->sigmar1, tie->sigmar2, tie->sigmar3,
                tie->dx_m, tie->dy_m, tie->dz_m, tie->sigma_m, tie->rsigma_m);
            }
            else {
              sprintf(string,
                "%6d %2d %s %2.2d:%4.4d:%3.3d:%2.2d %2.2d:%4.4d:%3.3d:%2.2d "
                "%8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f",
                i, j, tiestatus, project.files[crossing->file_id_1].survey, crossing->file_id_1,
                crossing->section_1, tie->snav_1, project.files[crossing->file_id_2].survey,
                crossing->file_id_2, crossing->section_2, tie->snav_2,
                tie->offset_x_m, tie->offset_y_m, tie->offset_z_m,
                tie->sigmar1, tie->sigmar2, tie->sigmar3);
            }
            xstr[num_ties] = XmStringCreateLocalized(string);
            if (mbna_verbose > 0)
              fprintf(stderr, "%s\n", string);
            if (i == mbna_crossing_select && j == mbna_tie_select)
              iselect = num_ties;
            num_ties++;
          }
        }
      }

      XmListAddItems(list_data, xstr, num_ties, 0);
      for (int k = 0; k < num_ties; k++) {
        XmStringFree(xstr[k]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
            || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
            || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {

    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Sorted Ties:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Sorted Ties of Survey %d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Sorted Ties of Survey-vs-Survey Block %d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Sorted Ties of File %d:%d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Sorted Ties with Survey %d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Sorted Ties with File %d:%d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Sorted Ties with Section %d:%d:%d:  Xing Tie Stat Sur1:Fil1:Sec1:Nv1 Sur2:Fil2:Sec2:Nv2 Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "Sorted Ties:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count crossing ties */
      int num_ties = 0;
      project.tiessortedthreshold = 0.0;
      for (int icrossing = 0; icrossing < project.num_crossings; icrossing++) {
        if (do_check_crossing_listok(icrossing)) {
          struct mbna_crossing *crossing = &(project.crossings[icrossing]);
          num_ties += crossing->num_ties;
        }
      }

      /* allocate strings for list */
      XmString *xstr = (XmString *)malloc(num_ties * sizeof(XmString));

      /* allocate array of tie pointers for list to be sorted */
      struct mbna_tie **tie_ptr_list = NULL;
      tie_ptr_list = (struct mbna_tie **) malloc(num_ties * sizeof(struct mbna_tie *));

      if (xstr == NULL || tie_ptr_list == NULL) {
        free(xstr);
        free(tie_ptr_list);
      } else {

      /* get list of ties */
      num_ties = 0;
      for (int icrossing = 0; icrossing < project.num_crossings; icrossing++) {
        if (do_check_crossing_listok(icrossing)) {
          struct mbna_crossing *crossing = &(project.crossings[icrossing]);
          for (int itie = 0; itie < crossing->num_ties; itie++) {
            struct mbna_tie *tie = &crossing->ties[itie];
            tie->icrossing = icrossing;
            tie->itie = itie;
            tie_ptr_list[num_ties] = tie;
            num_ties++;
          }
        }
      }

      /* sort the ties from smallest to largest model misfit and get threshold for displaying */
      if (num_ties > 0) {
        qsort((void *)tie_ptr_list, (size_t) num_ties, sizeof(struct mbna_tie *), mbnavadjust_tie_compare);

        /* get threshold for displaying */
        project.tiessortedthreshold = 0.0;
        if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL) {
          project.tiessortedthreshold = 0.0;
        }
        else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST) {
          project.tiessortedthreshold = tie_ptr_list[99 * num_ties / 100]->sigma_m;
        }
        else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {
          project.tiessortedthreshold = tie_ptr_list[19 * num_ties / 20]->sigma_m;
        }
      }

      /* generate list */
      iselect = MBNA_SELECT_NONE;
      int num_ties_list = 0;
      for (int ktie  = num_ties - 1; ktie >= 0; ktie--) {
        struct mbna_tie *tie = tie_ptr_list[ktie];
        if (do_check_crossing_listok(tie->icrossing) && tie->sigma_m >= project.tiessortedthreshold) {
          struct mbna_crossing *crossing = &(project.crossings[tie->icrossing]);
          tie = (struct mbna_tie *)&crossing->ties[tie->itie];
          char *tiestatus = NULL;
          if (tie->status == MBNA_TIE_XYZ)
            tiestatus = tiestatus_xyz;
          else if (tie->status == MBNA_TIE_XY)
            tiestatus = tiestatus_xy;
          else if (tie->status == MBNA_TIE_Z)
            tiestatus = tiestatus_z;
          else if (tie->status == MBNA_TIE_XYZ_FIXED)
            tiestatus = tiestatus_xyz_f;
          else if (tie->status == MBNA_TIE_XY_FIXED)
            tiestatus = tiestatus_xy_f;
          else if (tie->status == MBNA_TIE_Z_FIXED)
            tiestatus = tiestatus_z_f;
          if (tie->inversion_status == MBNA_INVERSION_CURRENT)
            sprintf(string,
              "%6d %2d %s %2.2d:%4.4d:%3.3d:%2.2d %2.2d:%4.4d:%3.3d:%2.2d "
              "%8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f",
              tie->icrossing, tie->itie, tiestatus, project.files[crossing->file_id_1].survey, crossing->file_id_1,
              crossing->section_1, tie->snav_1, project.files[crossing->file_id_2].survey,
              crossing->file_id_2, crossing->section_2, tie->snav_2,
              tie->offset_x_m, tie->offset_y_m, tie->offset_z_m,
              tie->sigmar1, tie->sigmar2, tie->sigmar3,
              tie->dx_m, tie->dy_m, tie->dz_m, tie->sigma_m, tie->rsigma_m);
          else if (tie->inversion_status == MBNA_INVERSION_OLD)
            sprintf(string,
              "%6d %2d %s %2.2d:%4.4d:%3.3d:%2.2d %2.2d:%4.4d:%3.3d:%2.2d "
              "%8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f ***",
              tie->icrossing, tie->itie, tiestatus, project.files[crossing->file_id_1].survey, crossing->file_id_1,
              crossing->section_1, tie->snav_1, project.files[crossing->file_id_2].survey,
              crossing->file_id_2, crossing->section_2, tie->snav_2,
              tie->offset_x_m, tie->offset_y_m, tie->offset_z_m,
              tie->sigmar1, tie->sigmar2, tie->sigmar3,
              tie->dx_m, tie->dy_m, tie->dz_m, tie->sigma_m, tie->rsigma_m);
          else
            sprintf(string,
              "%6d %2d %s %2.2d:%4.4d:%3.3d:%2.2d %2.2d:%4.4d:%3.3d:%2.2d "
              "%8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f",
              tie->icrossing, tie->itie, tiestatus, project.files[crossing->file_id_1].survey, crossing->file_id_1,
              crossing->section_1, tie->snav_1, project.files[crossing->file_id_2].survey,
              crossing->file_id_2, crossing->section_2, tie->snav_2,
              tie->offset_x_m, tie->offset_y_m, tie->offset_z_m,
              tie->sigmar1, tie->sigmar2, tie->sigmar3);
          xstr[num_ties_list] = XmStringCreateLocalized(string);
          if (mbna_verbose > 0)
            fprintf(stderr, "%s\n", string);
          if (tie->icrossing == mbna_crossing_select && tie->itie == mbna_tie_select)
            iselect = num_ties_list;
          num_ties_list++;
        }
      }

      XmListAddItems(list_data, xstr, num_ties_list, 0);
      for (int kk = 0; kk < num_ties_list; kk++) {
        XmStringFree(xstr[kk]);
      }
      free(xstr);

      free(tie_ptr_list);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Global Ties:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Global Ties of Survey %d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Global Ties of Survey-vs-Survey Block %d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Global Ties of File %d:%d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Global Ties with Survey %d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Global Ties of File %d:%d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Global Ties of Section %d:%d:%d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "Global Ties:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {
      /* count global ties */
      int num_globalties = 0;

      /* count global ties */
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          struct mbna_section *section = &(file->sections[j]);
          if (section->status == MBNA_CROSSING_STATUS_SET &&
              ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
               (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i && mbna_section_select == j))) {
            num_globalties++;
          }
        }
      }

      /* allocate strings for list */
      XmString *xstr = (XmString *)malloc(num_globalties * sizeof(XmString));
      if (xstr != NULL) {

      /* generate list */
      num_globalties = 0;
      iselect = MBNA_SELECT_NONE;

      /* list global ties */
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          struct mbna_section *section = &(file->sections[j]);
          if (section->status == MBNA_CROSSING_STATUS_SET &&
              ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
               (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i && mbna_section_select == j))) {
            char *tiestatus = NULL;
            if (section->globaltie.status == MBNA_TIE_XYZ)
              tiestatus = tiestatus_xyz;
            else if (section->globaltie.status == MBNA_TIE_XY)
              tiestatus = tiestatus_xy;
            else if (section->globaltie.status == MBNA_TIE_Z)
              tiestatus = tiestatus_z;
            else if (section->globaltie.status == MBNA_TIE_XYZ_FIXED)
              tiestatus = tiestatus_xyz_f;
            else if (section->globaltie.status == MBNA_TIE_XY_FIXED)
              tiestatus = tiestatus_xy_f;
            else if (section->globaltie.status == MBNA_TIE_Z_FIXED)
              tiestatus = tiestatus_z_f;
						int refgrid_id = project.refgrid_select;
						mb_path refgrid_name = " | No reference grid";
						if (section->globaltie.refgrid_id >= 0 && project.refgrid_select < 0) {
							refgrid_id = section->globaltie.refgrid_id;
						}
						if (refgrid_id >= 0 && refgrid_id < project.num_refgrids) {
							strncpy(refgrid_name, project.refgrid_names[refgrid_id], sizeof(refgrid_name));
						}
            if (section->globaltie.inversion_status == MBNA_INVERSION_CURRENT)
              sprintf(string,
                "%2.2d:%4.4d:%3.3d:%2.2d %s %2d %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f | %s",
                project.files[i].survey, i, j, section->globaltie.snav, tiestatus, section->globaltie.refgrid_id,
                section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
                section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3,
                section->globaltie.dx_m, section->globaltie.dy_m, section->globaltie.dz_m,
                section->globaltie.sigma_m, section->globaltie.rsigma_m, refgrid_name);
            else if (section->globaltie.inversion_status == MBNA_INVERSION_OLD)
              sprintf(string,
                "%2.2d:%4.4d:%3.3d:%2.2d %s %2d %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f *** | %s",
                project.files[i].survey, i, j, section->globaltie.snav, tiestatus, section->globaltie.refgrid_id,
                section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
                section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3,
                section->globaltie.dx_m, section->globaltie.dy_m, section->globaltie.dz_m,
                section->globaltie.sigma_m, section->globaltie.rsigma_m, refgrid_name);
            else
              sprintf(string,
                "%2.2d:%4.4d:%3.3d:%2.2d %s %2d %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %s",
                project.files[i].survey, i, j, section->globaltie.snav, tiestatus, section->globaltie.refgrid_id,
                section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
                section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3, refgrid_name);
            xstr[num_globalties] = XmStringCreateLocalized(string);
            if (mbna_verbose > 0)
              fprintf(stderr, "%s\n", string);
            if (section->section_id == mbna_section_select && section->file_id == mbna_file_select)
              iselect = num_globalties;
            num_globalties++;
          }
        }
      }

      XmListAddItems(list_data, xstr, num_globalties, 0);
      for (int k = 0; k < num_globalties; k++) {
        XmStringFree(xstr[k]);
      }
      free(xstr);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
    if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
      snprintf(string, sizeof(string), "Global Ties:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr");
    else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
      snprintf(string, sizeof(string), "Global Ties of Survey %d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
      snprintf(string, sizeof(string), "Global Ties of Survey-vs-Survey Block %d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
      snprintf(string, sizeof(string), "Global Ties of File %d:%d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
      snprintf(string, sizeof(string), "Global Ties with Survey %d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
      snprintf(string, sizeof(string), "Global Ties of File %d:%d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select);
    else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
      snprintf(string, sizeof(string), "Global Ties of Section %d:%d:%d:  Sur:File:Sec:Nv Stat RefGrid  Offx Offy Offz | S1 S2 S3 | Ex Ey Ez | Se Sr", mbna_survey_select, mbna_file_select, mbna_section_select);
    else
      snprintf(string, sizeof(string), "Global Ties:");
    set_label_string(label_listdata, string);
    if (mbna_verbose > 0)
      fprintf(stderr, "%s\n", string);
    if (project.num_files > 0) {

      /* count global ties */
      int num_globalties = 0;
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          struct mbna_section *section = &(file->sections[j]);
          if (section->status == MBNA_CROSSING_STATUS_SET &&
              ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
               (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i && mbna_section_select == j))) {
            num_globalties++;
          }
        }
      }

      /* allocate strings for list */
      XmString *xstr = (XmString *)malloc(num_globalties * sizeof(XmString));

      /* allocate array of section pointers for list to be sorted */
      struct mbna_section **section_ptr_list = NULL;
      section_ptr_list = (struct mbna_section **) malloc(num_globalties * sizeof(struct mbna_section *));

      if (xstr == NULL || section_ptr_list == NULL) {
        free(xstr);
        free(section_ptr_list);
      } else {

      /* get list of global ties */
      num_globalties = 0;
      for (int i = 0; i < project.num_files; i++) {
        struct mbna_file *file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          struct mbna_section *section = &(file->sections[j]);
          if (section->status == MBNA_CROSSING_STATUS_SET &&
              ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
               (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
               (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i && mbna_section_select == j))) {
            section_ptr_list[num_globalties] = section;
            num_globalties++;
          }
        }
      }

      /* sort the ties from smallest to largest model misfit */
      qsort((void *)section_ptr_list, (size_t) num_globalties, sizeof(struct mbna_section *), mbnavadjust_globaltie_compare);

      /* generate list */
      iselect = MBNA_SELECT_NONE;
      int kk = 0;
      for (int kglobaltie  = num_globalties - 1; kglobaltie >= 0; kglobaltie--) {
        struct mbna_section *section = section_ptr_list[kglobaltie];
        char *tiestatus = NULL;
        if (section->globaltie.status == MBNA_TIE_XYZ)
          tiestatus = tiestatus_xyz;
        else if (section->globaltie.status == MBNA_TIE_XY)
          tiestatus = tiestatus_xy;
        else if (section->globaltie.status == MBNA_TIE_Z)
          tiestatus = tiestatus_z;
        else if (section->globaltie.status == MBNA_TIE_XYZ_FIXED)
          tiestatus = tiestatus_xyz_f;
        else if (section->globaltie.status == MBNA_TIE_XY_FIXED)
          tiestatus = tiestatus_xy_f;
        else if (section->globaltie.status == MBNA_TIE_Z_FIXED)
          tiestatus = tiestatus_z_f;
				int refgrid_id = project.refgrid_select;
				mb_path refgrid_name = " | No reference grid";
				if (section->globaltie.refgrid_id >= 0 && project.refgrid_select < 0) {
					refgrid_id = section->globaltie.refgrid_id;
				}
				if (refgrid_id >= 0 && refgrid_id < project.num_refgrids) {
					strncpy(refgrid_name, project.refgrid_names[refgrid_id], sizeof(refgrid_name));
				}
        if (section->globaltie.inversion_status == MBNA_INVERSION_CURRENT)
          sprintf(string,
            "%2.2d:%4.4d:%3.3d:%2.2d %s %2d %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f | %s",
            project.files[section->file_id].survey,
            section->file_id, section->section_id,
            section->globaltie.snav, tiestatus, section->globaltie.refgrid_id,
            section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
            section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3,
            section->globaltie.dx_m, section->globaltie.dy_m, section->globaltie.dz_m,
            section->globaltie.sigma_m, section->globaltie.rsigma_m, refgrid_name);
        else if (section->globaltie.inversion_status == MBNA_INVERSION_OLD)
          sprintf(string,
            "%2.2d:%4.4d:%3.3d:%2.2d %s %2d %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %8.2f %6.3f *** | %s",
            project.files[section->file_id].survey,
            section->file_id, section->section_id,
            section->globaltie.snav, tiestatus, section->globaltie.refgrid_id,
            section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
            section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3,
            section->globaltie.dx_m, section->globaltie.dy_m, section->globaltie.dz_m,
            section->globaltie.sigma_m, section->globaltie.rsigma_m, refgrid_name);
        else
          sprintf(string,
            "%2.2d:%4.4d:%3.3d:%2.2d %s %2d %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %s",
            project.files[section->file_id].survey,
            section->file_id, section->section_id,
            section->globaltie.snav, tiestatus, section->globaltie.refgrid_id,
            section->globaltie.offset_x_m, section->globaltie.offset_y_m, section->globaltie.offset_z_m,
            section->globaltie.sigmar1, section->globaltie.sigmar2, section->globaltie.sigmar3, refgrid_name);
        xstr[kk] = XmStringCreateLocalized(string);
        if (mbna_verbose > 0)
          fprintf(stderr, "%s\n", string);
        if (section->section_id == mbna_section_select && section->file_id == mbna_file_select)
          iselect = kk;
        kk++;
      }

      XmListAddItems(list_data, xstr, num_globalties, 0);
      for (kk = 0; kk < num_globalties; kk++) {
        XmStringFree(xstr[kk]);
      }
      free(xstr);

      free(section_ptr_list);
      }
    }
    if (iselect != MBNA_SELECT_NONE) {
      XmListSelectPos(list_data, iselect + 1, 0);
      XmListSetPos(list_data, MAX(iselect + 1 - 5, 1));
    }
  }

  XtVaSetValues(toggleButton_showallsurveys, XmNsensitive, True, NULL);
  XtVaSetValues(toggleButton_showselectedsurvey, XmNsensitive, True, NULL);
  XtVaSetValues(toggleButton_showselectedblock, XmNsensitive, True, NULL);
  XtVaSetValues(toggleButton_showselectedfile, XmNsensitive, True, NULL);
  XtVaSetValues(toggleButton_showwithselectedsurvey, XmNsensitive, True, NULL);
  XtVaSetValues(toggleButton_showwithselectedfile, XmNsensitive, True, NULL);
  XtVaSetValues(toggleButton_showselectedsection, XmNsensitive, True, NULL);
  XmToggleButtonSetState(toggleButton_showallsurveys, FALSE, FALSE);
  XmToggleButtonSetState(toggleButton_showselectedsurvey, FALSE, FALSE);
  XmToggleButtonSetState(toggleButton_showselectedblock, FALSE, FALSE);
  XmToggleButtonSetState(toggleButton_showselectedfile, FALSE, FALSE);
  XmToggleButtonSetState(toggleButton_showwithselectedsurvey, FALSE, FALSE);
  XmToggleButtonSetState(toggleButton_showwithselectedfile, FALSE, FALSE);
  XmToggleButtonSetState(toggleButton_showselectedsection, FALSE, FALSE);
  if (mbna_view_mode == MBNA_VIEW_MODE_ALL)
    XmToggleButtonSetState(toggleButton_showallsurveys, TRUE, FALSE);
  else if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
    XmToggleButtonSetState(toggleButton_showselectedsurvey, TRUE, FALSE);
  else if (mbna_view_mode == MBNA_VIEW_MODE_BLOCK)
    XmToggleButtonSetState(toggleButton_showselectedblock, TRUE, FALSE);
  else if (mbna_view_mode == MBNA_VIEW_MODE_FILE)
    XmToggleButtonSetState(toggleButton_showselectedfile, TRUE, FALSE);
  else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY)
    XmToggleButtonSetState(toggleButton_showwithselectedsurvey, TRUE, FALSE);
  else if (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE)
    XmToggleButtonSetState(toggleButton_showwithselectedfile, TRUE, FALSE);
  else if (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)
    XmToggleButtonSetState(toggleButton_showselectedsection, TRUE, FALSE);

  if (mbna_view_list == MBNA_VIEW_LIST_REFERENCEGRIDS) {
    XtVaSetValues(pushButton_poornav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_goodnav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_fixednav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_fixedxynav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_fixedznav, XmNsensitive, False, NULL);
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS && project.num_files > 0 && mbna_survey_select != MBNA_SELECT_NONE) {
    XtVaSetValues(pushButton_poornav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_goodnav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_fixednav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_fixedxynav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_fixedznav, XmNsensitive, True, NULL);
    if (project.files[mbna_file_select].status == MBNA_FILE_POORNAV) {
      XtVaSetValues(pushButton_poornav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_GOODNAV) {
      XtVaSetValues(pushButton_goodnav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_FIXEDNAV) {
      XtVaSetValues(pushButton_fixednav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_FIXEDXYNAV) {
      XtVaSetValues(pushButton_fixedxynav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_FIXEDZNAV) {
      XtVaSetValues(pushButton_fixedznav, XmNsensitive, False, NULL);
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_FILES && project.num_files > 0 && mbna_file_select != MBNA_SELECT_NONE) {
    XtVaSetValues(pushButton_poornav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_goodnav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_fixednav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_fixedxynav, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_fixedznav, XmNsensitive, True, NULL);
    if (project.files[mbna_file_select].status == MBNA_FILE_POORNAV) {
      XtVaSetValues(pushButton_poornav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_GOODNAV) {
      XtVaSetValues(pushButton_goodnav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_FIXEDNAV) {
      XtVaSetValues(pushButton_fixednav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_FIXEDXYNAV) {
      XtVaSetValues(pushButton_fixedxynav, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].status == MBNA_FILE_FIXEDZNAV) {
      XtVaSetValues(pushButton_fixedznav, XmNsensitive, False, NULL);
    }
  }
  else {
    XtVaSetValues(pushButton_poornav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_goodnav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_fixednav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_fixedxynav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_fixedznav, XmNsensitive, False, NULL);
  }

  if ((mbna_view_list == MBNA_VIEW_LIST_TIES
        || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
        || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
        || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD)
      && project.num_files > 0 && mbna_tie_select != MBNA_SELECT_NONE) {
    if (project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_XY
        || project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_XY_FIXED) {
      XtVaSetValues(pushButton_tie_xyz, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_xy, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_tie_z, XmNsensitive, True, NULL);
    }
    else if (project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_Z
        || project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_Z_FIXED) {
      XtVaSetValues(pushButton_tie_xyz, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_xy, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_z, XmNsensitive, False, NULL);
    }
    else if (project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_XYZ
        || project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_XYZ_FIXED) {
      XtVaSetValues(pushButton_tie_xyz, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_tie_xy, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_z, XmNsensitive, True, NULL);
    }
    if (project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_XY
        || project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_Z
        || project.crossings[mbna_crossing_select].ties[mbna_tie_select].status == MBNA_TIE_XYZ) {
      XtVaSetValues(pushButton_tie_unfixed, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_tie_fixed, XmNsensitive, True, NULL);
    }
    else {
      XtVaSetValues(pushButton_tie_unfixed, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_fixed, XmNsensitive, False, NULL);
    }
  }
  else if ((mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES
            || mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED)
      && project.num_files > 0 && mbna_file_select != MBNA_SELECT_NONE
      && mbna_section_select != MBNA_SELECT_NONE
      && project.files[mbna_file_select].sections[mbna_section_select].status == MBNA_CROSSING_STATUS_SET) {
    if (project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_XY
        || project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_XY_FIXED) {
      XtVaSetValues(pushButton_tie_xyz, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_xy, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_tie_z, XmNsensitive, True, NULL);
    }
    else if (project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_Z
        || project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_Z_FIXED) {
      XtVaSetValues(pushButton_tie_xyz, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_xy, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_z, XmNsensitive, False, NULL);
    }
    else if (project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_XYZ
        || project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_XYZ_FIXED) {
      XtVaSetValues(pushButton_tie_xyz, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_tie_xy, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_z, XmNsensitive, True, NULL);
    }
    if (project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_XY
        || project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_Z
        || project.files[mbna_file_select].sections[mbna_section_select].globaltie.status == MBNA_TIE_XYZ) {
      XtVaSetValues(pushButton_tie_unfixed, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_tie_fixed, XmNsensitive, True, NULL);
    }
    else {
      XtVaSetValues(pushButton_tie_unfixed, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_tie_fixed, XmNsensitive, False, NULL);
    }
  }
  else {
    XtVaSetValues(pushButton_tie_xyz, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_tie_xy, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_tie_z, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_tie_unfixed, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_tie_fixed, XmNsensitive, False, NULL);
  }

  if (mbna_status != MBNA_STATUS_GUI) {
    XtVaSetValues(pushButton_new, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_open, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_close, XmNsensitive, False, NULL);
  }
  else if (project.open) {
    XtVaSetValues(pushButton_new, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_open, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_close, XmNsensitive, True, NULL);
  }
  else {
    XtVaSetValues(pushButton_new, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_open, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_close, XmNsensitive, False, NULL);
  }
  if (mbna_status == MBNA_STATUS_GUI && project.open && project.num_files >= 0) {
    XtVaSetValues(pushButton_importdata, XmNsensitive, True, NULL);
  }
  else {
    XtVaSetValues(pushButton_importdata, XmNsensitive, False, NULL);
  }

  if (project.open && project.num_files > 0) {
    XtVaSetValues(pushButton_showreferencegrids, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showsurveys, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showblocks, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showdata, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showsections, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showcrossings, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showmediocrecrossings, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showgoodcrossings, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showbettercrossings, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showtruecrossings, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showcrossingties, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showcrossingtiessortedall, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showcrossingtiessortedworst, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showcrossingtiessortedbad, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showglobalties, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_showglobaltiessorted, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showallsurveys, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showselectedsurvey, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showselectedblock, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showselectedfile, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showwithselectedsurvey, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showwithselectedfile, XmNsensitive, True, NULL);
    XtVaSetValues(toggleButton_showselectedsection, XmNsensitive, True, NULL);
    if (mbna_view_list == MBNA_VIEW_LIST_REFERENCEGRIDS) {
      XtVaSetValues(pushButton_showreferencegrids, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS) {
      XtVaSetValues(pushButton_showsurveys, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_BLOCKS) {
      XtVaSetValues(pushButton_showblocks, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILES) {
      XtVaSetValues(pushButton_showdata, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS) {
      XtVaSetValues(pushButton_showsections, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
      XtVaSetValues(pushButton_showcrossings, XmNsensitive, False, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_MEDIOCRECROSSINGS) {
      XtVaSetValues(pushButton_showmediocrecrossings, XmNsensitive, False, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS) {
      XtVaSetValues(pushButton_showgoodcrossings, XmNsensitive, False, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_BETTERCROSSINGS) {
      XtVaSetValues(pushButton_showbettercrossings, XmNsensitive, False, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
      XtVaSetValues(pushButton_showtruecrossings, XmNsensitive, False, NULL);
      if (project.num_truecrossings == project.num_truecrossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIES) {
      XtVaSetValues(pushButton_showcrossingties, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL) {
      XtVaSetValues(pushButton_showcrossingtiessortedall, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST) {
      XtVaSetValues(pushButton_showcrossingtiessortedworst, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {
      XtVaSetValues(pushButton_showcrossingtiessortedbad, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
      XtVaSetValues(pushButton_showglobalties, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
      XtVaSetValues(pushButton_showglobaltiessorted, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
  }
  else {
    XtVaSetValues(pushButton_showsurveys, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showblocks, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showdata, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showsections, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showcrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showmediocrecrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showgoodcrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showbettercrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showtruecrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showcrossingties, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showcrossingtiessortedall, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showcrossingtiessortedworst, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showcrossingtiessortedbad, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showglobalties, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_showglobaltiessorted, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showallsurveys, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showselectedsurvey, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showselectedblock, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showselectedfile, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showwithselectedsurvey, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showwithselectedfile, XmNsensitive, False, NULL);
    XtVaSetValues(toggleButton_showselectedsection, XmNsensitive, False, NULL);
  }

  if (mbna_status == MBNA_STATUS_GUI && project.open && project.num_files > 0) {
  	if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS
  		|| mbna_view_list == MBNA_VIEW_LIST_MEDIOCRECROSSINGS
  		|| mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS
  		|| mbna_view_list == MBNA_VIEW_LIST_BETTERCROSSINGS
  		|| mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
    	XtVaSetValues(pushButton_autopick, XmNsensitive, True, NULL);
    	XtVaSetValues(pushButton_autopickhorizontal, XmNsensitive, True, NULL);
    }
    else {
    	XtVaSetValues(pushButton_autopick, XmNsensitive, False, NULL);
    	XtVaSetValues(pushButton_autopickhorizontal, XmNsensitive, False, NULL);
    }
    XtVaSetValues(pushButton_autosetsvsvertical, XmNsensitive, True, NULL);
    if (project.inversion_status == MBNA_INVERSION_CURRENT)
      XtVaSetValues(pushButton_newcrossings, XmNsensitive, True, NULL);
    else
      XtVaSetValues(pushButton_newcrossings, XmNsensitive, False, NULL);
    do_visualize_sensitivity();
    XtVaSetValues(pushButton_analyzecrossings, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_zerozoffsets, XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_unsetskipped, XmNsensitive, True, NULL);
    if (project.num_truecrossings == project.num_truecrossings_analyzed
        || project.num_crossings_analyzed >= 10)
      XtVaSetValues(pushButton_invertnav, XmNsensitive, True, NULL);
    else
      XtVaSetValues(pushButton_invertnav, XmNsensitive, False, NULL);
    if (project.grid_status == MBNA_GRID_CURRENT)
      XtVaSetValues(pushButton_updategrids, XmNsensitive, False, NULL);
    else
      XtVaSetValues(pushButton_updategrids, XmNsensitive, True, NULL);
    if (project.inversion_status != MBNA_INVERSION_NONE)
      XtVaSetValues(pushButton_showmodelplot, XmNsensitive, True, NULL);
    else
      XtVaSetValues(pushButton_showmodelplot, XmNsensitive, False, NULL);
    if (project.inversion_status == MBNA_INVERSION_CURRENT)
      XtVaSetValues(pushButton_applynav, XmNsensitive, True, NULL);
    else
      XtVaSetValues(pushButton_applynav, XmNsensitive, False, NULL);
  }
  else {
    XtVaSetValues(pushButton_autopick, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_autopickhorizontal, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_autosetsvsvertical, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_newcrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_visualize, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_analyzecrossings, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_zerozoffsets, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_unsetskipped, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_invertnav, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_updategrids, XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_applynav, XmNsensitive, False, NULL);
    if (project.inversion_status != MBNA_INVERSION_NONE)
      XtVaSetValues(pushButton_showmodelplot, XmNsensitive, True, NULL);
    else
      XtVaSetValues(pushButton_showmodelplot, XmNsensitive, False, NULL);
  }
}
/*--------------------------------------------------------------------*/

void do_update_modelplot_status() {

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  /* deal with modelplot */
  if (project.modelplot) {
    mb_path string = "";

    /* set model view clear block button sensitivity */
    if (mbna_crossing_select == MBNA_SELECT_NONE) {
      XtVaSetValues(pushButton_modelplot_clearblock, XmNsensitive, False, NULL);
    }
    else {
      XtVaSetValues(pushButton_modelplot_clearblock, XmNsensitive, True, NULL);
    }

    struct mbna_crossing *crossing;

    /* set modelplot mode */
    if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS 
      	|| mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES
        || mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
      mbna_modelplot_mode = MBNA_MODELPLOT_MODE_SECTIONS;
    }
    else {
    	mbna_modelplot_mode = MBNA_MODELPLOT_MODE_CROSSINGS;
    }

    /* set model view status label */
    if (mbna_modelplot_mode == MBNA_MODELPLOT_MODE_CROSSINGS) {
			if (project.modelplot_style == MBNA_MODELPLOT_TIMESERIES || project.modelplot_style == MBNA_MODELPLOT_PERTURBATION) {
				if (mbna_crossing_select == MBNA_SELECT_NONE) {
					snprintf(string, sizeof(string), ":::t\"Mouse: <left> select  tie; <middle> select untied crossing; <right> drag zoom "
						"extent\":t\"No Selection\"");
				}
				else {
					crossing = &(project.crossings[mbna_crossing_select]);
					snprintf(string, sizeof(string),
						":::t\"Mouse: <left> select  tie; <middle> select untied crossing; <right> drag zoom "
						"extent\":t\"Selected Crossing: %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d\"",
						mbna_crossing_select, project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
						project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2);
				}
			}
			else {
				if (mbna_crossing_select == MBNA_SELECT_NONE) {
					snprintf(string, sizeof(string), ":::t\"Mouse: <left> select  tie; <middle> select block to view; <right> drag zoom "
						"extent\":t\"No Selection\"");
				}
				else {
					crossing = &(project.crossings[mbna_crossing_select]);
					snprintf(string, sizeof(string),
						":::t\"Mouse: <left> select  tie; <middle> select block to view; <right> drag zoom extent\":t\"Selected "
						"Crossing: %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d\"",
						mbna_crossing_select, project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
						project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2);
				}
			}
    }
    else /* if (mbna_modelplot_mode == MBNA_MODELPLOT_MODE_SECTIONS) */ {
			if (project.modelplot_style == MBNA_MODELPLOT_TIMESERIES || project.modelplot_style == MBNA_MODELPLOT_PERTURBATION) {
				if (mbna_section_select == MBNA_SELECT_NONE) {
					snprintf(string, sizeof(string), ":::t\"Mouse: <left> select global tie; <middle> select untied section; <right> drag zoom "
						"extent\":t\"No Selection\"");
				}
				else /* if (mbna_section_select != MBNA_SELECT_NONE) */ {
					crossing = &(project.crossings[mbna_crossing_select]);
					snprintf(string, sizeof(string),
						":::t\"Mouse: <left> select global tie; <middle> select untied section <right> drag zoom "
						"extent\":t\"Selected Section: %2.2d:%3.3d:%3.3d\"",
						project.files[mbna_current_file].survey, mbna_current_file, mbna_current_section);
				}
			}
			else {
				if (mbna_crossing_select == MBNA_SELECT_NONE) {
					snprintf(string, sizeof(string), ":::t\"Mouse: <left> select  tie; <middle> select block to view; <right> drag zoom "
						"extent\":t\"No Selection\"");
				}
				else {
					crossing = &(project.crossings[mbna_crossing_select]);
					snprintf(string, sizeof(string),
						":::t\"Mouse: <left> select  tie; <middle> select block to view; <right> drag zoom extent\":t\"Selected "
						"Crossing: %4d %2.2d:%3.3d:%3.3d %2.2d:%3.3d:%3.3d\"",
						mbna_crossing_select, project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
						project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2);
				}
			}
    }

    set_label_multiline_string(label_modelplot_status, string);
  }

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}
/*--------------------------------------------------------------------*/

void do_update_visualization_status() {

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();
  // fprintf(stderr,"do_update_visualization_status: mbna_crossing_select:%d mbna_tie_select:%d\n",
  // mbna_crossing_select,mbna_tie_select);
  /* deal with modelplot */
  int error = MB_ERROR_NO_ERROR;
  if (project.visualization_status) {

    /* clear any navadjust related interactive nav picks */
    /* port: the picks are the window's own (mbna_view_*), no libmbview shared data */

    if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING
        && mbna_crossing_select != MBNA_SELECT_NONE) {
      /* select the data sections associated with the loaded crossing */
      struct mbna_crossing *crossing = &(project.crossings[mbna_crossing_select]);
      mbna_view_clearnavpicks();
      mbna_view_clear_navadjust_picks();
      mb_path name;
      sprintf(name, "%4.4d:%4.4d", crossing->file_id_1, crossing->section_1);
      mbna_view_picknav(name);
      sprintf(name, "%4.4d:%4.4d", crossing->file_id_2, crossing->section_2);
      mbna_view_picknav(name);

      /* select the route associated with the selected tie */
      if (mbna_tie_select != MBNA_SELECT_NONE) {
        struct mbna_file *file_1 = (struct mbna_file *)&project.files[crossing->file_id_1];
        struct mbna_file *file_2 = (struct mbna_file *)&project.files[crossing->file_id_2];
        sprintf(name, "%4.4d:%1d %2.2d:%4.4d:%2.2d %2.2d:%4.4d:%2.2d", mbna_crossing_select, mbna_tie_select,
          file_1->block, crossing->file_id_1, crossing->section_1, file_2->block, crossing->file_id_2,
          crossing->section_2);
      }
      else {
        sprintf(name, "MBNA_SELECT_NONE");
      }
      mbna_view_pickroute(name);
    }

    else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION
            && mbna_file_select != MBNA_SELECT_NONE
            && mbna_section_select != MBNA_SELECT_NONE) {
      /* select the data section associated with the loaded section */
      mbna_view_clearnavpicks();
      mbna_view_clear_navadjust_picks();
      mb_path name;
      sprintf(name, "%4.4d:%4.4d", mbna_file_select, mbna_section_select);
      mbna_view_picknav(name);
    }
  }
  // fprintf(stderr,"Calling mbview_update from do_update_visualization_status\n");

  if (project.visualization_status)   /* port: only a window that is there */
    mbna_view_update();
  (void)error;

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}
/*--------------------------------------------------------------------*/

void do_naverr_init(int mode) {
  if (mbna_verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called   mode:%d\n", __func__, mode);
  }

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  /* manage naverr */
  XtManageChild(bulletinBoard_naverr);

  /* port: the window's canvases need no graphics contexts, fonts or cursors of X11: they are
     made with the window, and handed over by mbna_window_canvases() */
  status = mbnavadjust_set_graphics(cont_xgid, corr_xgid, zoff_xgid);

  /* set status flag */
  mbna_status = MBNA_STATUS_NAVERR;

  /* set naverr mode and open crossing or section appropriately */
  if (mode == MBNA_NAVERR_MODE_CROSSING) {
    /* get current crossing - mbna_naverr_mode will be set to MBNA_NAVERR_MODE_CROSSING */
    if (mbna_crossing_select == MBNA_SELECT_NONE)
      mbnavadjust_naverr_nextunset_crossing();
    else
      mbnavadjust_naverr_specific_crossing(mbna_crossing_select, mbna_tie_select);
  } else if (mode == MBNA_NAVERR_MODE_SECTION) {
    /* get current section - mbna_naverr_mode will be set to MBNA_NAVERR_MODE_SECTION */
    if (mbna_section_select == MBNA_SELECT_NONE)
      mbnavadjust_naverr_nextunset_section();
    else
      mbnavadjust_naverr_specific_section(mbna_file_select, mbna_section_select);
  }

  /* update naverr labels */
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}

/*--------------------------------------------------------------------*/

void do_naverr_update() {

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  mb_path string = "";

  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING && mbna_current_crossing >= 0) {

    const double zoom_factor =
      (mbna_plot_lon_max - mbna_plot_lon_min) > 0.0
      ? 100.0 * MAX((mbna_lon_max - mbna_lon_min) / (mbna_plot_lon_max - mbna_plot_lon_min),
        (mbna_lat_max - mbna_lat_min) / (mbna_plot_lat_max - mbna_plot_lat_min))
      : 0.0;

    const double plot_width = (mbna_plot_lon_max - mbna_plot_lon_min) / mbna_mtodeglon;
    const double misfit_width = (mbna_plot_lon_max - mbna_plot_lon_min) / mbna_mtodeglon;

    /* get time difference */
    const double timediff =
      (project.files[mbna_file_id_2].sections[mbna_section_2].btime_d -
       project.files[mbna_file_id_1].sections[mbna_section_1].btime_d) /
      86400.0;

    /* set main naverr status label */
    struct mbna_crossing *crossing = &project.crossings[mbna_current_crossing];
    struct mbna_tie *tie = &crossing->ties[mbna_current_tie];
    if (crossing->status == MBNA_CROSSING_STATUS_NONE) {
      snprintf(string, sizeof(string), ":::t\"Crossing: %d of %d\"\
:t\"Sections: %2.2d:%3.3d:%3.3d and %2.2d:%3.3d:%3.3d\"\
:t\"Time Difference: %f days \"\
:t\"Status: Unset \"\
:t\"Plot Widths (m): Contour: %.2f Misfit: %.2f Z: %.2f\"\
:t\"Zoom Factor: %.2f \"\
:t\"Relative Offsets:   None None None\"", mbna_current_crossing, project.num_crossings,
        project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
        project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, timediff, plot_width,
        misfit_width, project.zoffsetwidth, zoom_factor);
    }
    else if (crossing->status == MBNA_CROSSING_STATUS_SET) {
      snprintf(string, sizeof(string), ":::t\"Crossing: %d of %d\"\
:t\"Sections: %2.2d:%3.3d:%3.3d and %2.2d:%3.3d:%3.3d\"\
:t\"Time Difference: %f days \"\
:t\"Current Tie Point: %2d of %2d  Nav Points: %4d %4d\"\
:t\"Plot Widths (m): Contour: %.2f Misfit: %.2f Z: %.2f\"\
:t\"Zoom Factor: %.2f \"\
:t\"Relative Offsets (m):   %.3f %.3f %.3f\"\
:t\"Sigma (m):   %.3f %.3f %.3f\"", mbna_current_crossing, project.num_crossings,
        project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
        project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, timediff,
        mbna_current_tie, crossing->num_ties, tie->snav_1, tie->snav_2, plot_width, misfit_width,
        project.zoffsetwidth, zoom_factor, tie->offset_x_m, tie->offset_y_m, tie->offset_z_m, tie->sigmar1,
        tie->sigmar2, tie->sigmar3);
    }
    else {
      snprintf(string, sizeof(string), ":::t\"Crossing: %d of %d\"\
:t\"Sections: %2.2d:%3.3d:%3.3d and %2.2d:%3.3d:%3.3d\"\
:t\"Time Difference: %f days \"\
:t\"Status: Skipped \"\
:t\"Plot Widths (m): Contour: %.2f Misfit: %.2f Z: %.2f\"\
:t\"Zoom Factor: %.2f \"\
:t\"Relative Offsets:   Skipped Skipped Skipped\"", mbna_current_crossing, project.num_crossings,
        project.files[crossing->file_id_1].survey, crossing->file_id_1, crossing->section_1,
        project.files[crossing->file_id_2].survey, crossing->file_id_2, crossing->section_2, timediff, plot_width,
        misfit_width, project.zoffsetwidth, zoom_factor);
    }
    set_label_multiline_string(label_naverr_status, string);

    /* set some button sensitivities */
    XtVaSetValues(pushButton_naverr_deletetie, XmNsensitive, (mbna_current_tie >= 0), NULL);
    XtVaSetValues(pushButton_naverr_selecttie, XmNsensitive, (crossing->num_ties > 0), NULL);
    XtVaSetValues(pushButton_naverr_fullsize, XmNsensitive,
            (mbna_plot_lon_min != mbna_lon_min || mbna_plot_lon_max != mbna_lon_max ||
             mbna_plot_lat_min != mbna_lat_min || mbna_plot_lat_max != mbna_lat_max),
            NULL);
    if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILES) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_truecrossings == project.num_truecrossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIES) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }

    do_naverr_offsetlabel();
  }

  else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION && mbna_current_file >= 0 && mbna_current_section >= 0) {

    const double zoom_factor =
      (mbna_plot_lon_max - mbna_plot_lon_min) > 0.0
      ? 100.0 * MAX((mbna_lon_max - mbna_lon_min) / (mbna_plot_lon_max - mbna_plot_lon_min),
        (mbna_lat_max - mbna_lat_min) / (mbna_plot_lat_max - mbna_plot_lat_min))
      : 0.0;

    const double plot_width = (mbna_plot_lon_max - mbna_plot_lon_min) / mbna_mtodeglon;
    const double misfit_width = (mbna_plot_lon_max - mbna_plot_lon_min) / mbna_mtodeglon;

    /* set main naverr status label */
    struct mbna_file *file = &project.files[mbna_current_file];
    struct mbna_section *section = &file->sections[mbna_current_section];
    struct mbna_globaltie *globaltie = &section->globaltie;
    int refgrid_id = project.refgrid_select;
    mb_path refgrid_name = "none";
    if (globaltie->refgrid_id >= 0 && project.refgrid_select < 0) {
      refgrid_id = globaltie->refgrid_id;
    }
    if (refgrid_id >= 0 && refgrid_id < project.num_refgrids) {
    	strncpy(refgrid_name, project.refgrid_names[refgrid_id], sizeof(refgrid_name));
    }

    if (section->status == MBNA_CROSSING_STATUS_NONE) {
      snprintf(string, sizeof(string), ":::t\"Section: %2.2d:%3.3d:%3.3d\"\
:t\"Global Tie Status: Unset \"\
:t\"Ref Grid: %s \"\
:t\"Plot Widths (m): Contour: %.2f Misfit: %.2f Z: %.2f\"\
:t\"Zoom Factor: %.2f \"\
:t\"Relative Offsets:   None None None\"",
        project.files[mbna_current_file].survey, mbna_current_file, mbna_current_section, 
        refgrid_name, plot_width, misfit_width, project.zoffsetwidth, zoom_factor);
    }
    else if (section->status == MBNA_CROSSING_STATUS_SKIP) {
      snprintf(string, sizeof(string), ":::t\"Section: %2.2d:%3.3d:%3.3d\"\
:t\"Global Tie Status: Unset (skipped) \"\
:t\"Ref Grid: %s \"\
:t\"Plot Widths (m): Contour: %.2f Misfit: %.2f Z: %.2f\"\
:t\"Zoom Factor: %.2f \"\
:t\"Relative Offsets:   None None None\"",
        project.files[mbna_current_file].survey, mbna_current_file, mbna_current_section, 
        refgrid_name, plot_width, misfit_width, project.zoffsetwidth, zoom_factor);
    }
    else {
      snprintf(string, sizeof(string), ":::t\"Section: %2.2d:%3.3d:%3.3d\"\
:t\"Global Tie Status: Set \"\
:t\"Ref Grid: %s \"\
:t\"Nav Point: %4d\"\
:t\"Plot Widths (m): Contour: %.2f Misfit: %.2f Z: %.2f\"\
:t\"Zoom Factor: %.2f \"\
:t\"Relative Offsets (m):   %.3f %.3f %.3f\"\
:t\"Sigma (m):   %.3f %.3f %.3f\"",
        project.files[mbna_current_file].survey, mbna_current_file, mbna_current_section,
        refgrid_name, 
        globaltie->snav, plot_width, misfit_width, project.zoffsetwidth, zoom_factor,
        globaltie->offset_x_m, globaltie->offset_y_m, globaltie->offset_z_m,
        globaltie->sigmar1, globaltie->sigmar2, globaltie->sigmar3);
    }
    set_label_multiline_string(label_naverr_status, string);

    /* set some button sensitivities */
    XtVaSetValues(pushButton_naverr_deletetie, XmNsensitive, (section->status == MBNA_CROSSING_STATUS_SET), NULL);
    XtVaSetValues(pushButton_naverr_selecttie, XmNsensitive, false, NULL);
    XtVaSetValues(pushButton_naverr_fullsize, XmNsensitive,
            (mbna_plot_lon_min != mbna_lon_min || mbna_plot_lon_max != mbna_lon_max ||
             mbna_plot_lat_min != mbna_lat_min || mbna_plot_lat_max != mbna_lat_max),
            NULL);
    if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILES) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_globalties == project.num_globalties_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_globalties == project.num_globalties_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_crossings == project.num_crossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      if (project.num_truecrossings == project.num_truecrossings_analyzed)
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
      else
        XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, True, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIES) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
      XtVaSetValues(pushButton_naverr_previous, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_next, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_naverr_nextunset, XmNsensitive, False, NULL);
    }

    do_naverr_offsetlabel();
  }

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}

/*--------------------------------------------------------------------*/

void do_naverr_offsetlabel() {
  mb_path string = "";
  
  /* look at current crossing */
  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING && mbna_current_crossing >= 0) {
    snprintf(string, sizeof(string), ":::t\"Working Offsets (m): %.3f %.3f %.3f %d:%d\":t\"Sigma (m): %.3f %.3f %.3f\"",
      mbna_offset_x / mbna_mtodeglon, mbna_offset_y / mbna_mtodeglat, mbna_offset_z, mbna_snav_1, mbna_snav_2,
      mbna_minmisfit_sr1, mbna_minmisfit_sr2, mbna_minmisfit_sr3);
  }

  else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION && mbna_current_file >= 0 && mbna_current_section >= 0) {
    snprintf(string, sizeof(string), ":::t\"Working Offsets (m): %.3f %.3f %.3f %d:%d\":t\"Sigma (m): %.3f %.3f %.3f\"",
      mbna_offset_x / mbna_mtodeglon, mbna_offset_y / mbna_mtodeglat, mbna_offset_z, mbna_snav_1, mbna_snav_2,
      mbna_minmisfit_sr1, mbna_minmisfit_sr2, mbna_minmisfit_sr3);
  }

  else {
    snprintf(string, sizeof(string), ":::t\"Working Offsets (m): %.3f %.3f %.3f\":t\"Working Tie Points: %d:%d\"", 0.0, 0.0, 0.0, 0, 0);
  }
  set_label_multiline_string(label_naverr_offsets, string);

  /* set button sensitivity for setting or resetting offsets */
  XtVaSetValues(pushButton_naverr_settie, XmNsensitive, mbna_allow_set_tie, NULL);
  XtVaSetValues(pushButton_naverr_resettie, XmNsensitive, mbna_allow_set_tie, NULL);
  XtVaSetValues(pushButton_naverr_addtie, XmNsensitive, mbna_allow_add_tie, NULL);
}

/*--------------------------------------------------------------------*/

void do_list_data_select(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  int position_count = 0;
  int *position_list = NULL;

  if (XmListGetSelectedPos(list_data, &position_list, &position_count)) {
    XmListCallbackStruct *acs = (XmListCallbackStruct *)call_data;
    mb_path selected_item;
    char *tmp = NULL;
    struct mbna_file *file;
    int num_files, num_sections, num_crossings, num_ties;
    bool found = true;

    if (mbna_view_list == MBNA_VIEW_LIST_REFERENCEGRIDS) {
      project.refgrid_select = position_list[0] - 2;
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS) {
      mbna_section_select = 0;
      mbna_file_select = MBNA_SELECT_NONE;
      mbna_survey_select = position_list[0] - 1;

      /* get selected file from list */
      for (int i= 0; i < project.num_files; i++) {
        file = &(project.files[i]);
        if (mbna_file_select == MBNA_SELECT_NONE && mbna_survey_select == file->survey) {
          mbna_file_select = i;
          mbna_section_select = 0;
        }
      }
      // fprintf(stderr,"mbna_survey_select:%d:%d:%d\n",mbna_survey_select,mbna_file_select,mbna_section_select);
      project.modelplot_uptodate = false;
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_BLOCKS) {
      memset(selected_item, 0, sizeof(selected_item));
      if (acs->item != NULL) {
        tmp = (char *)XmStringUnparse(acs->item, NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
      }
      if (tmp == NULL && acs->selected_items != NULL) {
        tmp = (char *)XmStringUnparse(acs->selected_items[0], NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
      }
      if (tmp != NULL) {
        strncpy(selected_item, tmp, sizeof(selected_item) - 1);
        XtFree(tmp);
        tmp = NULL;
      }
      int iblock_select, isurvey1, isurvey2, d1, d2, d3, d4, d5;
      const int nscan = sscanf(selected_item, "block %d: Survey %d vs Survey %d : Crossings: %d %d %d %d : Ties: %d",
             &iblock_select, &isurvey1, &isurvey2, &d1, &d2, &d3, &d4, &d5);
      if (nscan == 8) {
        mbna_survey_select = iblock_select;
        mbna_survey_select1 = isurvey1;
        mbna_survey_select2 = isurvey2;
      }
      project.modelplot_uptodate = false;
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILES) {
      num_files = 0;

      /* get selected file from list */
      for (int i= 0; i < project.num_files; i++) {
        file = &(project.files[i]);
        if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
            (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
            (mbna_view_mode == MBNA_VIEW_MODE_FILE) ||
            (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
            (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE) || (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION)) {
          if (num_files == position_list[0] - 1) {
            mbna_section_select = 0;
            mbna_file_select = i;
            mbna_survey_select = file->survey;
          }
          num_files++;
        }
      }
      project.modelplot_uptodate = false;
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS) {
      /* get selected section from list */
      num_sections = 0;
      for (int i= 0; i < project.num_files; i++) {
        file = &(project.files[i]);
        for (int j = 0; j < file->num_sections; j++) {
          if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
              (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
              (mbna_view_mode == MBNA_VIEW_MODE_BLOCK && (mbna_survey_select1 == file->survey || mbna_survey_select2 == file->survey)) ||
              (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == i) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == i) ||
              (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == i)) {
            if (num_sections == position_list[0] - 1) {
              mbna_section_select = j;
              mbna_file_select = i;
              mbna_survey_select = file->survey;
            }
            num_sections++;
          }
        }
      }
      project.modelplot_uptodate = false;

      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_SECTION);
      }

      /* else if naverr window is up, load selected section */
      else {
        mbnavadjust_naverr_specific_section(mbna_file_select, mbna_section_select);
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS || mbna_view_list == MBNA_VIEW_LIST_MEDIOCRECROSSINGS ||
       mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS || mbna_view_list == MBNA_VIEW_LIST_BETTERCROSSINGS ||
       mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
      /* get selected crossing from list */
      num_crossings = 0;
      for (int i= 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          if (num_crossings == position_list[0] - 1) {
            mbna_crossing_select = i;
            mbna_tie_select = 0;
          }
          num_crossings++;
        }
      }
      project.modelplot_uptodate = false;

      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }

      /* else if naverr window is up, load selected crossing */
      else {
        mbnavadjust_naverr_specific_crossing(mbna_crossing_select, mbna_tie_select);
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIES) {
      /* get crossing and tie from list */
      num_crossings = 0;
      num_ties = 0;
      int found = false;
      for (int i= 0; i < project.num_crossings; i++) {
        if (do_check_crossing_listok(i)) {
          for (int j = 0; j < project.crossings[i].num_ties; j++) {
            if (num_ties == position_list[0] - 1) {
              mbna_crossing_select = i;
              mbna_tie_select = j;
              found = true;
            }
            num_ties++;
          }
          num_crossings++;
        }
      }

      /* load selected crossing tie into naverr window, global ties ignored */
      if (found) {
        /* bring up naverr window if required */
        if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
          do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
        }

        /* else if naverr window is up, load selected crossing */
        else {
          mbnavadjust_naverr_specific_crossing(mbna_crossing_select, mbna_tie_select);
          mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
          do_naverr_update();
        }
        project.modelplot_uptodate = false;
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {
      /* get crossing and tie from selected item in the list */
    	if (acs->item != NULL) {
    		tmp = (char *)XmStringUnparse(acs->item, NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
    		if (tmp == NULL && acs->selected_items != NULL) {
    			tmp = (char *)XmStringUnparse(acs->selected_items[0], NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
    		}
    		if (tmp != NULL) {
        	strncpy(selected_item, tmp, sizeof(selected_item) - 1);
        	selected_item[sizeof(selected_item) - 1] = '\0';
        	XtFree(tmp);
        	tmp = NULL;
					int i;
					int j;
					const int nscan = sscanf(selected_item, "%d %d ", &i, &j);
					if (nscan == 2 && i >= 0 && i < project.num_crossings
							&& do_check_crossing_listok(i)
							&& j >= 0 && j < project.crossings[i].num_ties) {
						mbna_crossing_select = i;
						mbna_tie_select = j;
						found = true;
					}
    		}
    	}

      /* load selected crossing tie into naverr window, global ties ignored */
      if (found) {
        /* bring up naverr window if required */
        if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
          do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
        }

        /* else if naverr window is up, load selected crossing */
        else {
          mbnavadjust_naverr_specific_crossing(mbna_crossing_select, mbna_tie_select);
          mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
          do_naverr_update();
        }
        project.modelplot_uptodate = false;
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES || mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
      /* get global tie from selected item in the list */
    	if (acs->item != NULL) {
    		tmp = (char *)XmStringUnparse(acs->item, NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
    		if (tmp == NULL && acs->selected_items != NULL) {
    			tmp = (char *)XmStringUnparse(acs->selected_items[0], NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
    		}
    		if (tmp != NULL) {
        	strncpy(selected_item, tmp, sizeof(selected_item) - 1);
        	selected_item[sizeof(selected_item) - 1] = '\0';
        	XtFree(tmp);
        	tmp = NULL;
					int isurvey, ifile, jsection, ksnav;
					const int nscan = sscanf(selected_item, "%d:%d:%d:%d ", &isurvey, &ifile, &jsection, &ksnav);
					if (nscan == 4 && ifile >= 0 && ifile < project.num_files
							&& jsection >= 0 && jsection < project.files[ifile].num_sections) {
						mbna_file_select = ifile;
						mbna_section_select = jsection;
						found = true;
					}
    		}
    	}

      /* load selected section into naverr window */
      if (found) {
        /* bring up naverr window if required */
        if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
          do_naverr_init(MBNA_NAVERR_MODE_SECTION);
        }

        /* else if naverr window is up, load selected crossing */
        else {
          mbnavadjust_naverr_specific_section(mbna_file_select, mbna_section_select);
          mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
          do_naverr_update();
        }
        project.modelplot_uptodate = false;
      }
    }

    XtFree((char *)position_list);
  }

  /* else user selected same list item, deselecting it  - don't change anything
      - bring up naverr if needed - and let do_update_status redraw list with
     previous item selected */
  else {
    if (mbna_view_list == MBNA_VIEW_LIST_REFERENCEGRIDS) {
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_SURVEYS) {
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILES) {
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_SECTION);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_BETTERCROSSINGS) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIES) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
              || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_SECTION);
      }
    }
    else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_SECTION);
      }
    }
  }

  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}

/*--------------------------------------------------------------------*/

int do_check_crossing_listok(int icrossing) {
  bool use_status = false;

  /* get crossing */
  struct mbna_crossing *crossing = &(project.crossings[icrossing]);

  /* check for list type */
  if (icrossing == mbna_crossing_select) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_CROSSINGS) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_MEDIOCRECROSSINGS && crossing->overlap >= MBNA_MEDIOCREOVERLAP_THRESHOLD) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GOODCROSSINGS && crossing->overlap >= MBNA_GOODOVERLAP_THRESHOLD) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_BETTERCROSSINGS && crossing->overlap >= MBNA_BETTEROVERLAP_THRESHOLD) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_TRUECROSSINGS && crossing->truecrossing) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_TIES && crossing->num_ties > 0) {
    use_status = true;
  }
  else if ((mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDALL
            || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDWORST
            || mbna_view_list == MBNA_VIEW_LIST_TIESSORTEDBAD)
          && crossing->num_ties > 0) {
    for (int itie=0; itie < crossing->num_ties; itie++) {
      struct mbna_tie *tie = &crossing->ties[itie];
      if (tie->sigma_m >= project.tiessortedthreshold) {
        use_status = true;
      }
    }
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
    use_status = false;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
    use_status = false;
  }
  else {
    use_status = false;
  }

  /* check view mode modifiers */
  if (use_status) {
    // fprintf(stderr,"icrossing:%d mbna_view_mode:%d mbna_survey_select:%d mbna_survey_select:%d:%d:%d mbna_file_select:%d
    // mbna_section_select:%d",
    // icrossing,mbna_view_mode,mbna_survey_select,mbna_survey_select,mbna_survey_select1,mbna_survey_select2,
    // mbna_file_select,mbna_section_select);
    if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
        (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == project.files[crossing->file_id_1].survey &&
         mbna_survey_select == project.files[crossing->file_id_2].survey) ||
        (mbna_view_mode == MBNA_VIEW_MODE_BLOCK && mbna_survey_select1 == project.files[crossing->file_id_1].survey &&
         mbna_survey_select2 == project.files[crossing->file_id_2].survey) ||
        (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == crossing->file_id_1 &&
         mbna_file_select == crossing->file_id_2) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && (mbna_survey_select == project.files[crossing->file_id_1].survey ||
                     mbna_survey_select == project.files[crossing->file_id_2].survey)) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE &&
         (mbna_file_select == crossing->file_id_1 || mbna_file_select == crossing->file_id_2)) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == crossing->file_id_1 &&
         mbna_section_select == crossing->section_1) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == crossing->file_id_2 &&
         mbna_section_select == crossing->section_2)) {
      use_status = true;
    }
    else {
      use_status = false;
    }
  }

  return (use_status);
}

/*--------------------------------------------------------------------*/

int do_check_section_listok(int ifile, int isection) {
  bool use_status = false;

  /* check for list type */
  if (ifile == mbna_file_select && isection == mbna_section_select) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS) {
    use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES) {
    if (project.files[ifile].sections[isection].status == MBNA_CROSSING_STATUS_SET)
      use_status = true;
  }
  else if (mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
    if (project.files[ifile].sections[isection].status == MBNA_CROSSING_STATUS_SET)
      use_status = true;
  }
  else {
    use_status = false;
  }

  /* check view mode modifiers */
  if (use_status) {
    if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
        (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == project.files[ifile].survey) ||
        (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == ifile) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_file_select == project.files[ifile].survey) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == ifile) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == ifile && mbna_section_select == isection) ) {
      use_status = true;
    }
    else {
      use_status = false;
    }
  }

  return (use_status);
}

/*--------------------------------------------------------------------*/

int do_check_globaltie_listok(int ifile, int isection) {
  /* get file and section */
  struct mbna_file *file = &(project.files[ifile]);
  struct mbna_section *section = &(file->sections[isection]);

  /* if there is a global tie check for view mode */
  bool use_status = false;
  if (section->status == MBNA_CROSSING_STATUS_SET) {
    if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
        (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
        (mbna_view_mode == MBNA_VIEW_MODE_BLOCK &&
         (mbna_survey_select1 == file->survey || mbna_survey_select2 == file->survey)) ||
        (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == ifile) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == ifile) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == ifile && mbna_section_select == isection) ||
        (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == ifile && mbna_section_select == isection)) {
      use_status = true;
    }
  }

  return (use_status);
}
/*--------------------------------------------------------------------*/

int do_check_nav_active(int ifile, int isection) {
  /* get file and section */
  struct mbna_file *file = &(project.files[ifile]);

  /* check section nav for view mode */
  bool active = false;
  if ((mbna_view_mode == MBNA_VIEW_MODE_ALL) ||
      (mbna_view_mode == MBNA_VIEW_MODE_SURVEY && mbna_survey_select == file->survey) ||
      (mbna_view_mode == MBNA_VIEW_MODE_BLOCK &&
       (mbna_survey_select1 == file->survey || mbna_survey_select2 == file->survey)) ||
      (mbna_view_mode == MBNA_VIEW_MODE_FILE && mbna_file_select == ifile) ||
      (mbna_view_mode == MBNA_VIEW_MODE_WITHSURVEY && mbna_survey_select == file->survey) ||
      (mbna_view_mode == MBNA_VIEW_MODE_WITHFILE && mbna_file_select == ifile) ||
      (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == ifile && mbna_section_select == isection) ||
      (mbna_view_mode == MBNA_VIEW_MODE_WITHSECTION && mbna_file_select == ifile && mbna_section_select == isection)) {
    active = true;
  }

  return (active);
}

/*--------------------------------------------------------------------*/

void do_naverr_cont_expose(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
}

/*--------------------------------------------------------------------*/

void do_naverr_corr_expose(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
}

/*--------------------------------------------------------------------*/

void do_naverr_cont_input(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* If there is input in the drawing area */
  if (acs->reason == XmCR_INPUT) {
    XEvent *event = acs->event;

    /* Check for mouse pressed. */
    if (event->xany.type == ButtonPress) {
      /* If left mouse button is pushed then save position. */
      if (event->xbutton.button == 1) {
        button1down = true;
        loc_x = event->xbutton.x;
        loc_y = event->xbutton.y;
        mbna_offset_x_old = mbna_offset_x;
        mbna_offset_y_old = mbna_offset_y;
        mbna_offset_z_old = mbna_offset_z;

        /* reset offset label */
        mbnavadjust_naverr_checkoksettie();
        do_naverr_offsetlabel();

      } /* end of left button events */

      /* If middle mouse button is pushed */
      if (event->xbutton.button == 2) {
        button2down = true;
        mbna_zoom_x1 = event->xbutton.x;
        mbna_zoom_y1 = event->xbutton.y;
        mbna_zoom_x2 = event->xbutton.x;
        mbna_zoom_y2 = event->xbutton.y;

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_ZOOMFIRST);
      } /* end of middle button events */

      /* If right mouse button is pushed */
      if (event->xbutton.button == 3) {
        button3down = true;

        /* get new snav points */
        mbnavadjust_naverr_snavpoints(event->xbutton.x, event->xbutton.y);

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
      } /* end of right button events */
    } /* end of button press events */

    /* Check for mouse released. */
    if (event->xany.type == ButtonRelease) {
      if (event->xbutton.button == 1) {
        button1down = false;
      }
      if (event->xbutton.button == 2) {
        button2down = false;
        mbna_zoom_x2 = event->xbutton.x;
        mbna_zoom_y2 = event->xbutton.y;

        const double x1 = mbna_zoom_x1 / mbna_plotx_scale + mbna_plot_lon_min;
        const double y1 = (cont_borders[3] - mbna_zoom_y1) / mbna_ploty_scale + mbna_plot_lat_min;
        const double x2 = mbna_zoom_x2 / mbna_plotx_scale + mbna_plot_lon_min;
        const double y2 = (cont_borders[3] - mbna_zoom_y2) / mbna_ploty_scale + mbna_plot_lat_min;

        /* get new plot bounds */
        mbna_plot_lon_min = MIN(x1, x2);
        mbna_plot_lon_max = MAX(x1, x2);
        mbna_plot_lat_min = MIN(y1, y2);
        mbna_plot_lat_max = MAX(y1, y2);

        /* replot contours and misfit */
        mbnavadjust_get_misfit();
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
      }
      if (event->xbutton.button == 3) {
        button3down = false;
      }

    } /* end of button release events */

    /* Check for mouse motion while pressed. */
    if (event->xany.type == MotionNotify) {
      if (button1down) {
        /* move offset */
        mbna_offset_x = mbna_offset_x_old + (event->xmotion.x - loc_x) / mbna_plotx_scale;
        mbna_offset_y = mbna_offset_y_old - (event->xmotion.y - loc_y) / mbna_ploty_scale;

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_MOVE);
        do_naverr_offsetlabel();

        /* reset old position */
        loc_x = event->xmotion.x;
        loc_y = event->xmotion.y;
        mbna_offset_x_old = mbna_offset_x;
        mbna_offset_y_old = mbna_offset_y;
      }
      else if (button2down) {
        mbna_zoom_x2 = event->xmotion.x;
        mbna_zoom_y2 = event->xmotion.y;

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_ZOOM);
      }
    }
  } /* end of inputs from window */

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}

/*--------------------------------------------------------------------*/

void do_naverr_corr_input(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* If there is input in the drawing area */
  if (acs->reason == XmCR_INPUT) {
    XEvent *event = acs->event;

    /* Check for mouse pressed. */
    if (event->xany.type == ButtonPress) {
      /* If left mouse button is pushed then save position. */
      if (event->xbutton.button == 1) {
        button1down = true;
        mbna_offset_x_old = mbna_offset_x;
        mbna_offset_y_old = mbna_offset_y;
        mbna_offset_z_old = mbna_offset_z;
        mbna_offset_x =
          mbna_misfit_offset_x + (event->xbutton.x - (corr_borders[0] + corr_borders[1]) / 2) / mbna_misfit_xscale;
        mbna_offset_y =
          mbna_misfit_offset_y - (event->xbutton.y - (corr_borders[3] + corr_borders[2]) / 2) / mbna_misfit_yscale;

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_MOVE);
        do_naverr_update();
        do_naverr_offsetlabel();

      } /* end of left button events */
    } /* end of button press events */

    /* Check for mouse released. */
    if (event->xany.type == ButtonRelease) {
      if (event->xbutton.button == 1) {
        button1down = false;
      }
    } /* end of button release events */

    /* Check for mouse motion while pressed. */
    if (event->xany.type == MotionNotify) {
      if (button1down) {
        /* move offset */
        mbna_offset_x =
          mbna_misfit_offset_x + (event->xmotion.x - (corr_borders[0] + corr_borders[1]) / 2) / mbna_misfit_xscale;
        mbna_offset_y =
          mbna_misfit_offset_y - (event->xmotion.y - (corr_borders[3] + corr_borders[2]) / 2) / mbna_misfit_yscale;

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_MOVE);
        do_naverr_update();
        do_naverr_offsetlabel();

        /* reset old position */
        mbna_offset_x_old = mbna_offset_x;
        mbna_offset_y_old = mbna_offset_y;
        mbna_offset_z_old = mbna_offset_z;
      }
    }
  } /* end of inputs from window */

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}
/*--------------------------------------------------------------------*/

void do_naverr_zcorr_input(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter

  XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* If there is input in the drawing area */
  if (acs->reason == XmCR_INPUT) {
    XEvent *event = acs->event;

    /* Check for mouse pressed. */
    if (event->xany.type == ButtonPress) {
      /* If left mouse button is pushed then save position. */
      if (event->xbutton.button == 1) {
        button1down = true;
        mbna_offset_x_old = mbna_offset_x;
        mbna_offset_y_old = mbna_offset_y;
        mbna_offset_z_old = mbna_offset_z;
        mbna_offset_z = ((event->xbutton.x - zoff_borders[0]) / mbna_zoff_scale_x) + mbna_misfit_offset_z -
            0.5 * project.zoffsetwidth;
        /* fprintf(stderr,"%s %d: mbna_offset_z:%f\n",__FILE__,__LINE__,mbna_offset_z); */

        /* recalculate minimum misfit at current z offset */
        mbnavadjust_get_misfitxy();

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_MOVE);
        do_naverr_update();
        do_naverr_offsetlabel();
      } /* end of left button events */
    } /* end of button press events */

    /* Check for mouse released. */
    if (event->xany.type == ButtonRelease) {
      if (event->xbutton.button == 1) {
        button1down = false;
        mbnavadjust_naverr_replot();
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
        do_naverr_offsetlabel();
      }
    } /* end of button release events */

    /* Check for mouse motion while pressed. */
    if (event->xany.type == MotionNotify) {
      if (button1down) {
        /* move offset */
        mbna_offset_z = ((event->xbutton.x - zoff_borders[0]) / mbna_zoff_scale_x) + mbna_misfit_offset_z -
            0.5 * project.zoffsetwidth;
        /* fprintf(stderr,"buttonx:%d %f  mbna_misfit_offset_z:%f project.zoffsetwidth:%f  mbna_offset_z:%f\n",
           event->xbutton.x,((event->xbutton.x - zoff_borders[0])/mbna_zoff_scale_x), mbna_misfit_offset_z,
           project.zoffsetwidth, mbna_offset_z);
           fprintf(stderr,"%s %d: mbna_offset_z:%f\n",__FILE__,__LINE__,mbna_offset_z); */

        /* recalculate minimum misfit at current z offset */
        mbnavadjust_get_misfitxy();

        /* replot contours */
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_MOVE);
        do_naverr_update();
        do_naverr_offsetlabel();

        /* reset old position */
        mbna_offset_x_old = mbna_offset_x;
        mbna_offset_y_old = mbna_offset_y;
        mbna_offset_z_old = mbna_offset_z;
      }
    }
  } /* end of inputs from window */
}

/*--------------------------------------------------------------------*/

void do_naverr_previous(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING) {
    mbnavadjust_naverr_previous_crossing();
  } else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION) {
    mbnavadjust_naverr_previous_section();
  }
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_next(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING) {
    mbnavadjust_naverr_next_crossing();
  } else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION) {
    mbnavadjust_naverr_next_section();
  }
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }

// clock_t end = clock();
// double time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
// fprintf(stderr, "%s:%d:%s done in %.6f seconds...\n\n",
// __FILE__, __LINE__, __FUNCTION__, time_used);
}

/*--------------------------------------------------------------------*/

void do_naverr_nextunset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING) {
    mbnavadjust_naverr_nextunset_crossing();
  } else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION) {
    mbnavadjust_naverr_nextunset_section();
  }
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_addtie(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_addtie();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    project.modelplot_uptodate = false;
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_deletetie(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_deletetie();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    project.modelplot_uptodate = false;
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_selecttie(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_selecttie();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    project.modelplot_uptodate = false;
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_unset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_unset();
  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING) {
    mbnavadjust_naverr_next_crossing();
  } else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION) {
    mbnavadjust_naverr_next_section();
  }
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    project.modelplot_uptodate = false;
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_setnone(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_skip();
  if (mbna_naverr_mode == MBNA_NAVERR_MODE_CROSSING) {
    mbnavadjust_naverr_nextunset_crossing();
  } else if (mbna_naverr_mode == MBNA_NAVERR_MODE_SECTION) {
    mbnavadjust_naverr_nextunset_section();
  }
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    project.modelplot_uptodate = false;
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_setoffset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_naverr_save();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    project.modelplot_uptodate = false;
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_resettie(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  mbnavadjust_naverr_resettie();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_naverr_dismiss(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* unload loaded crossing */
  if (mbna_naverr_mode != MBNA_NAVERR_MODE_UNLOADED) {
    status = mbnavadjust_crossing_unload();
    status = mbnavadjust_referencegrid_unload();

    /* deallocate graphics (only allocated when a crossing was actually
        loaded - matches the guard already used in do_quit(); freeing these
        unconditionally double-frees cont_gc/corr_gc whenever dismiss is
        invoked with no crossing loaded, e.g. immediately after clearing
        all ties, crashing in XFreeGC/_XFreeExtData) */
    XFreeGC(display, cont_gc);
    XFreeGC(display, corr_gc);
    xg_free(cont_xgid);
    xg_free(corr_xgid);
  }

  mbna_status = MBNA_STATUS_GUI;
  mbna_current_crossing = MBV_SELECT_NONE;
  mbna_current_tie = MBV_SELECT_NONE;
  mbna_current_file = MBV_SELECT_NONE;
  mbna_current_section = MBV_SELECT_NONE;
  //mbna_crossing_select = MBV_SELECT_NONE;
  //mbna_tie_select = MBV_SELECT_NONE;
  mbnavadjust_naverr_checkoksettie();
  do_naverr_update();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_naverr_fullsize(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* reset the plot bounds */
  mbna_plot_lon_min = mbna_lon_min;
  mbna_plot_lon_max = mbna_lon_max;
  mbna_plot_lat_min = mbna_lat_min;
  mbna_plot_lat_max = mbna_lat_max;
  mbnavadjust_get_misfit();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
}

/*--------------------------------------------------------------------*/

void do_naverr_zerooffset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* move offset */
  mbna_offset_x = 0.0;
  mbna_offset_y = 0.0;
  mbna_offset_z = 0.0;
  /* fprintf(stderr,"%s %d: mbna_offset_z:%f\n",__FILE__,__LINE__,mbna_offset_z); */

  /* recalculate minimum misfit at current z offset */
  mbnavadjust_get_misfitxy();

  /* replot contours */
  mbnavadjust_naverr_replot();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_offsetlabel();
}
/*--------------------------------------------------------------------*/

void do_naverr_zerozoffset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* move offset */
  mbna_offset_z = 0.0;
  /* fprintf(stderr,"%s %d: mbna_offset_z:%f\n",__FILE__,__LINE__,mbna_offset_z); */

  /* recalculate minimum misfit at current z offset */
  mbnavadjust_get_misfitxy();

  /* replot contours */
  mbnavadjust_naverr_replot();

  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_offsetlabel();
}
/*--------------------------------------------------------------------*/

void do_naverr_applyzoffset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* recalculate minimum misfit at current z offset */
  mbnavadjust_get_misfitxy();

  /* replot contours */
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_offsetlabel();
//  do_update_status();
//  if (project.modelplot) {
//    do_update_modelplot_status();
//    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
//  }
//  if (project.visualization_status) {
//    mbnavadjust_reset_visualization_navties();
//    do_update_visualization_status();
//  }
}

/*--------------------------------------------------------------------*/

void do_naverr_minmisfit(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* move offset */
  mbna_offset_x = mbna_minmisfit_x;
  mbna_offset_y = mbna_minmisfit_y;
  mbna_offset_z = mbna_minmisfit_z;
  /* fprintf(stderr,"%s %d: mbna_offset_z:%f\n",__FILE__,__LINE__,mbna_offset_z); */

  /* recalculate minimum misfit at current z offset */
  mbnavadjust_get_misfitxy();

  /* replot contours */
  mbnavadjust_naverr_replot();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
//  do_update_status();
//  if (project.modelplot) {
//    do_update_modelplot_status();
//    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
//  }
//  if (project.visualization_status) {
//    mbnavadjust_reset_visualization_navties();
//    do_update_visualization_status();
//  }
}

/*--------------------------------------------------------------------*/

void do_naverr_minxymisfit(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* move offset */
  mbna_offset_x = mbna_minmisfit_xh;
  mbna_offset_y = mbna_minmisfit_yh;
  mbna_offset_z = mbna_minmisfit_zh;
//fprintf(stderr,"do_naverr_minxymisfit mbna_minmisfit_xh:%f mbna_minmisfit_yh:%f mbna_minmisfit_zh:%f\n",
//mbna_minmisfit_xh, mbna_minmisfit_yh, mbna_minmisfit_zh);
//fprintf(stderr,"%s %d: mbna_offset_z:%f mbna_offset_z:%f mbna_offset_z:%f\n",
//__FILE__, __LINE__, mbna_offset_x, mbna_offset_y, mbna_offset_z);

  /* replot contours */
  mbnavadjust_naverr_replot();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();

}
/*--------------------------------------------------------------------*/

void do_naverr_misfitcenter(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (XmToggleButtonGetState(toggleButton_misfitcenter_zero))
    mbna_misfit_center = MBNA_MISFIT_ZEROCENTER;
  else
    mbna_misfit_center = MBNA_MISFIT_AUTOCENTER;

  /* replot contours and misfit */
  mbnavadjust_get_misfit();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_naverr_update();
//  do_update_status();
//  if (project.modelplot) {
//    do_update_modelplot_status();
//    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
//  }
//  if (project.visualization_status) {
//    mbnavadjust_reset_visualization_navties();
//    do_update_visualization_status();
//  }
}

/*--------------------------------------------------------------------*/

void do_biases_apply(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* get file structures */
  struct mbna_file *file1 = &(project.files[mbna_file_id_1]);
  struct mbna_file *file2 = &(project.files[mbna_file_id_2]);

  int ivalue;
  XtVaGetValues(scale_biases_heading1, XmNvalue, &ivalue, NULL);
  file1->heading_bias = 0.1 * ivalue;

  XtVaGetValues(scale_biases_roll1, XmNvalue, &ivalue, NULL);
  file1->roll_bias = 0.1 * ivalue;

  XtVaGetValues(scale_biases_heading2, XmNvalue, &ivalue, NULL);
  file2->heading_bias = 0.1 * ivalue;

  XtVaGetValues(scale_biases_roll2, XmNvalue, &ivalue, NULL);
  file2->roll_bias = 0.1 * ivalue;

  /* set contours out of date */
  for (int isection = 0; isection < file1->num_sections; isection++) {
    file1->sections[isection].contoursuptodate = false;
  }
  for (int isection = 0; isection < file2->num_sections; isection++) {
    file2->sections[isection].contoursuptodate = false;
  }

  mbnavadjust_naverr_replot();
  mbnavadjust_get_misfit();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_update_status();
//  if (project.modelplot) {
//    do_update_modelplot_status();
//    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
//  }
//  if (project.visualization_status) {
//    mbnavadjust_reset_visualization_navties();
//    do_update_visualization_status();
//  }
}

/*--------------------------------------------------------------------*/

void do_biases_applyall(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int ivalue;

  /* get bias values */
  XtVaGetValues(scale_biases_heading1, XmNvalue, &ivalue, NULL);
  const double heading_bias = 0.1 * ivalue;
  XtVaGetValues(scale_biases_roll1, XmNvalue, &ivalue, NULL);
  const double roll_bias = 0.1 * ivalue;

  /* loop over files */
  for (int ifile = 0; ifile < project.num_files; ifile++) {
    struct mbna_file *file = &(project.files[ifile]);
    file->heading_bias = heading_bias;
    file->roll_bias = roll_bias;

    /* set contours out of date */
    for (int isection = 0; isection < file->num_sections; isection++) {
      file->sections[isection].contoursuptodate = false;
    }
  }

  mbnavadjust_naverr_replot();
  mbnavadjust_get_misfit();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_biases_init(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* get file structures */
  struct mbna_file *file1 = &(project.files[mbna_file_id_1]);
  struct mbna_file *file2 = &(project.files[mbna_file_id_2]);

  /* set biases label */
  char value_text[MB_PATH_MAXLINE];
  snprintf(value_text, sizeof(value_text), ":::t\"Section ID\'s (file:section):\":t\"  Section 1: %4.4d:%4.4d\"\"  Section 2: %4.4d:%4.4d\"",
    mbna_file_id_1, mbna_section_1, mbna_file_id_2, mbna_section_2);
  set_label_multiline_string(label_biases_files, value_text);

  /* set biases radiobox */
  if (file1->heading_bias == file2->heading_bias && file1->roll_bias == file2->roll_bias) {
    mbna_bias_mode = MBNA_BIAS_SAME;
    XmToggleButtonSetState(toggleButton_biases_together, TRUE, TRUE);
  }
  else {
    mbna_bias_mode = MBNA_BIAS_DIFFERENT;
    XmToggleButtonSetState(toggleButton_biases_separate, TRUE, TRUE);
  }

  /* set values of bias sliders */
  XtVaSetValues(scale_biases_heading1, XmNvalue, (int)(10 * file1->heading_bias), NULL);
  XtVaSetValues(scale_biases_roll1, XmNvalue, (int)(10 * file1->roll_bias), NULL);
  if (mbna_bias_mode == MBNA_BIAS_DIFFERENT) {
    XtVaSetValues(scale_biases_heading2, XmNvalue, (int)(10 * file2->heading_bias), XmNsensitive, True, NULL);
    XtVaSetValues(scale_biases_roll2, XmNvalue, (int)(10 * file2->roll_bias), XmNsensitive, True, NULL);
    XtVaSetValues(pushButton_biases_applyall, XmNsensitive, False, NULL);
  }
  else {
    XtVaSetValues(scale_biases_heading2, XmNvalue, (int)(10 * file2->heading_bias), XmNsensitive, False, NULL);
    XtVaSetValues(scale_biases_roll2, XmNvalue, (int)(10 * file2->roll_bias), XmNsensitive, False, NULL);
    XtVaSetValues(pushButton_biases_applyall, XmNsensitive, True, NULL);
  }
}

/*--------------------------------------------------------------------*/

void do_biases_toggle(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (XmToggleButtonGetState(toggleButton_biases_together)) {
    if (mbna_bias_mode == MBNA_BIAS_DIFFERENT) {
      mbna_bias_mode = MBNA_BIAS_SAME;
      int ivalue;
      XtVaGetValues(scale_biases_heading1, XmNvalue, &ivalue, NULL);
      XtVaSetValues(scale_biases_heading2, XmNvalue, ivalue, XmNsensitive, False, NULL);
      XtVaGetValues(scale_biases_roll1, XmNvalue, &ivalue, NULL);
      XtVaSetValues(scale_biases_roll2, XmNvalue, ivalue, XmNsensitive, False, NULL);
      XtVaSetValues(pushButton_biases_applyall, XmNsensitive, True, NULL);
    }
  }
  else {
    if (mbna_bias_mode == MBNA_BIAS_SAME) {
      mbna_bias_mode = MBNA_BIAS_DIFFERENT;
      XtVaSetValues(scale_biases_heading2, XmNsensitive, True, NULL);
      XtVaSetValues(scale_biases_roll2, XmNsensitive, True, NULL);
      XtVaSetValues(pushButton_biases_applyall, XmNsensitive, False, NULL);
    }
  }
}

/*--------------------------------------------------------------------*/

void do_biases_heading(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_bias_mode == MBNA_BIAS_SAME) {
    int ivalue;
    XtVaGetValues(scale_biases_heading1, XmNvalue, &ivalue, NULL);
    XtVaSetValues(scale_biases_heading2, XmNvalue, ivalue, XmNsensitive, False, NULL);
  }
}

/*--------------------------------------------------------------------*/

void do_biases_roll(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_bias_mode == MBNA_BIAS_SAME) {
    int ivalue;
    XtVaGetValues(scale_biases_roll1, XmNvalue, &ivalue, NULL);
    XtVaSetValues(scale_biases_roll2, XmNvalue, ivalue, XmNsensitive, False, NULL);
  }
}

/*--------------------------------------------------------------------*/

void do_controls_show(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* set values of decimation slider */
  XtVaSetValues(scale_controls_decimation, XmNvalue, project.decimation, NULL);

  /* set values of section length slider */
  int ivalue = (int)(100 * project.section_length);
  int imax = (int)(100 * 50.0);
  XtVaSetValues(scale_controls_sectionlength, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);

  /* set values of section soundings slider */
  XtVaGetValues(scale_controls_sectionsoundings, XmNvalue, &ivalue, XmNmaximum, &imax, NULL);
  ivalue = project.section_soundings;
  if (ivalue >= imax) {
    if (ivalue >= 2 * imax)
      imax = 2 * ivalue;
    else
      imax = 2 * imax;
  } else if (ivalue < imax/2) {
    imax = MIN(2 * ivalue, 100000);
  }
  XtVaSetValues(scale_controls_sectionsoundings, XmNminimum, 1, XmNmaximum, imax, XmNvalue, ivalue, NULL);

  /* set values of contour interval slider */
  ivalue = (int)(100 * project.cont_int);
  if (project.cont_int >= 10.0)
    imax = (int)(100 * 400.0);
  else
    imax = (int)(100 * 50.0);
  XtVaSetValues(scale_controls_contourinterval, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);

  /* set values of color interval slider */
  ivalue = (int)(100 * project.col_int);
  if (project.col_int >= 10.0)
    imax = (int)(100 * 400.0);
  else
    imax = (int)(100 * 50.0);
  XtVaSetValues(scale_controls_colorinterval, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);

  /* set values of tick interval slider */
  ivalue = (int)(100 * project.tick_int);
  if (project.tick_int >= 10.0)
    imax = (int)(100 * 400.0);
  else
    imax = (int)(100 * 50.0);
  XtVaSetValues(scale_controls_tickinterval, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);

  /* set values of inversion smoothing weight slider */
  ivalue = (int)(100 * project.smoothing);
  imax = (int)(100 * 10.0);
  XtVaSetValues(scale_controls_smoothing, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);

  /* set values of z offset width slider */
  ivalue = (int)(10 * project.zoffsetwidth);
  XtVaSetValues(scale_controls_zoffset, XmNvalue, ivalue, NULL);

  /* set value of use mode radioBox toggles */
  if (project.use_mode <= MBNA_USE_MODE_PRIMARY) {
    XmToggleButtonSetState(toggleButton_controls_use_primary, TRUE, TRUE);
    project.use_mode = MBNA_USE_MODE_PRIMARY;
  }
  else if (project.use_mode == MBNA_USE_MODE_SECONDARY) {
    XmToggleButtonSetState(toggleButton_controls_use_secondary, TRUE, TRUE);
  }
  else {
    XmToggleButtonSetState(toggleButton_controls_use_tertiary, TRUE, TRUE);
    project.use_mode = MBNA_USE_MODE_TERTIARY;
  }

  /* set misfit offset center toggles */
  if (mbna_misfit_center == MBNA_MISFIT_ZEROCENTER) {
    XmToggleButtonSetState(toggleButton_misfitcenter_zero, TRUE, TRUE);
  }
  else {
    XmToggleButtonSetState(toggleButton_misfitcenter_auto, TRUE, TRUE);
  }

}

/*--------------------------------------------------------------------*/

void do_controls_apply(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

// fprintf(stderr, "%s:%d:%s start\n",
// __FILE__, __LINE__, __FUNCTION__);
// clock_t start = clock();

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* get value of decimation slider */
  XtVaGetValues(scale_controls_decimation, XmNvalue, &project.decimation, NULL);

  /* get values of section length slider */
  int ivalue;
  XtVaGetValues(scale_controls_sectionlength, XmNvalue, &ivalue, NULL);
  project.section_length = ((double)ivalue) / 100.0;

  /* get values of section soundings slider */
  XtVaGetValues(scale_controls_sectionsoundings, XmNvalue, &ivalue, NULL);
  project.section_soundings = ivalue;

  /* get values of contour interval slider */
  XtVaGetValues(scale_controls_contourinterval, XmNvalue, &ivalue, NULL);
  project.cont_int = ((double)ivalue) / 100.0;

  /* get values of color interval slider */
  XtVaGetValues(scale_controls_colorinterval, XmNvalue, &ivalue, NULL);
  project.col_int = ((double)ivalue) / 100.0;

  /* get values of tick interval slider */
  XtVaGetValues(scale_controls_tickinterval, XmNvalue, &ivalue, NULL);
  project.tick_int = ((double)ivalue) / 100.0;

  /* get values of inversion smoothing slider */
  XtVaGetValues(scale_controls_smoothing, XmNvalue, &ivalue, NULL);
  project.smoothing = ((double)ivalue) / 100.0;

  /* get values of z offset width slider */
  XtVaGetValues(scale_controls_zoffset, XmNvalue, &ivalue, NULL);
  project.zoffsetwidth = ((double)ivalue) / 10.0;

  /* get value of use mode toggles */
  if (XmToggleButtonGetState(toggleButton_controls_use_primary))
    project.use_mode = MBNA_USE_MODE_PRIMARY;
  else if (XmToggleButtonGetState(toggleButton_controls_use_secondary))
    project.use_mode = MBNA_USE_MODE_SECONDARY;
  else if (XmToggleButtonGetState(toggleButton_controls_use_tertiary))
    project.use_mode = MBNA_USE_MODE_TERTIARY;
  else
    project.use_mode = MBNA_USE_MODE_PRIMARY;

  if (mbna_file_id_1 >= 0 && mbna_section_1 >= 0)
    project.files[mbna_file_id_1].sections[mbna_section_1].contoursuptodate = false;
  if (mbna_file_id_2 >= 0 && mbna_section_2 >= 0)
    project.files[mbna_file_id_2].sections[mbna_section_2].contoursuptodate = false;

  mbnavadjust_naverr_replot();
  int error = MB_ERROR_NO_ERROR;
  mbnavadjust_write_project(mbna_verbose, &project, __FILE__, __LINE__, __FUNCTION__, &error);
  mbnavadjust_get_misfit();
  mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
  do_update_status();
  do_naverr_update();
//  do_update_status();
//  if (project.modelplot) {
//    do_update_modelplot_status();
//    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
//  }
//  if (project.visualization_status) {
//    mbnavadjust_reset_visualization_navties();
//    do_update_visualization_status();
//  }
}

/*--------------------------------------------------------------------*/
void do_scale_controls_sectionlength(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
}

/*--------------------------------------------------------------------*/
void do_scale_controls_sectionsoundings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int ivalue;
  int imin;
  int imax;

  /* get values of section soundings slider */
  XtVaGetValues(scale_controls_sectionsoundings, XmNvalue, &ivalue, XmNminimum, &imin, XmNmaximum, &imax, NULL);

  /* recalculate max value if needed */
  if (ivalue == imin) {
    imax = MAX(imax / 2, 2 * imin);
  }
  if (ivalue == imax) {
    imax = 2 * imax;
  }

  /* reset values of section soundings slider */
  XtVaSetValues(scale_controls_sectionsoundings, XmNmaximum, imax, XmNvalue, ivalue, NULL);
}

/*--------------------------------------------------------------------*/
void do_scale_controls_decimation(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
}

/*--------------------------------------------------------------------*/
void do_scale_contourinterval(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int ivalue;
  int imax;

  /* get values of contour interval slider */
  XtVaGetValues(scale_controls_contourinterval, XmNvalue, &ivalue, XmNmaximum, &imax, NULL);

  /* reset values of contour interval slider to round numbers */
  if (ivalue > 2500)
    ivalue = ((ivalue + 500) / 1000) * 1000;
  else if (ivalue > 500)
    ivalue = ((ivalue + 250) / 500) * 500;
  else if (ivalue > 100)
    ivalue = ((ivalue + 50) / 100) * 100;
  else if (ivalue > 50)
    ivalue = ((ivalue + 25) / 50) * 50;
  else if (ivalue > 10)
    ivalue = ((ivalue + 5) / 10) * 10;
  else if (ivalue > 5)
    ivalue = ((ivalue + 2) / 5) * 5;
  if (ivalue == 1 && imax >= 40000)
    imax = 500;
  if (ivalue == imax && imax <= 500)
    imax = 40000;
  XtVaSetValues(scale_controls_contourinterval, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);
}

/*--------------------------------------------------------------------*/
void do_scale_controls_tickinterval(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int ivalue;
  int imax;

  /* get values of tick interval slider */
  XtVaGetValues(scale_controls_tickinterval, XmNvalue, &ivalue, XmNmaximum, &imax, NULL);

  /* reset values of tick interval slider to round numbers */
  if (ivalue > 2500)
    ivalue = ((ivalue + 500) / 1000) * 1000;
  else if (ivalue > 500)
    ivalue = ((ivalue + 250) / 500) * 500;
  else if (ivalue > 100)
    ivalue = ((ivalue + 50) / 100) * 100;
  else if (ivalue > 50)
    ivalue = ((ivalue + 25) / 50) * 50;
  else if (ivalue > 10)
    ivalue = ((ivalue + 5) / 10) * 10;
  else if (ivalue > 5)
    ivalue = ((ivalue + 2) / 5) * 5;
  if (ivalue == 1 && imax >= 40000)
    imax = 500;
  if (ivalue == imax && imax <= 500)
    imax = 40000;
  XtVaSetValues(scale_controls_tickinterval, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);
}

/*--------------------------------------------------------------------*/
void do_controls_scale_colorinterval(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int ivalue;
  int imax;

  /* get values of color interval slider */
  XtVaGetValues(scale_controls_colorinterval, XmNvalue, &ivalue, XmNmaximum, &imax, NULL);

  /* reset values of color interval slider to round numbers */
  if (ivalue > 2500)
    ivalue = ((ivalue + 500) / 1000) * 1000;
  else if (ivalue > 500)
    ivalue = ((ivalue + 250) / 500) * 500;
  else if (ivalue > 100)
    ivalue = ((ivalue + 50) / 100) * 100;
  else if (ivalue > 50)
    ivalue = ((ivalue + 25) / 50) * 50;
  else if (ivalue > 10)
    ivalue = ((ivalue + 5) / 10) * 10;
  else if (ivalue > 5)
    ivalue = ((ivalue + 2) / 5) * 5;
  if (ivalue == 1 && imax >= 40000)
    imax = 500;
  if (ivalue == imax && imax <= 500)
    imax = 40000;
  XtVaSetValues(scale_controls_colorinterval, XmNminimum, 1, XmNmaximum, imax, XmNdecimalPoints, 2, XmNvalue, ivalue, NULL);
}
/*--------------------------------------------------------------------*/

void do_scale_controls_smoothing(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
}
/*--------------------------------------------------------------------*/

void do_scale_controls_zoffset(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
}

/*--------------------------------------------------------------------*/
void do_file_new(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  fprintf(stderr, "do_file_new\n");
}

/*--------------------------------------------------------------------*/
void do_file_open(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  fprintf(stderr, "do_file_open\n");
}

/*--------------------------------------------------------------------*/
void do_file_importdata(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  fprintf(stderr, "do_file_importdata\n");
}

/*--------------------------------------------------------------------*/
void do_file_close(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int error = MB_ERROR_NO_ERROR;

  mbnavadjust_close_project(mbna_verbose, &project, &error);
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_quit(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* unload loaded crossing */
  if (mbna_naverr_mode != MBNA_NAVERR_MODE_UNLOADED) {
    status = mbnavadjust_crossing_unload();

    /* deallocate graphics */
    mbna_status = MBNA_STATUS_GUI;
    XFreeGC(display, cont_gc);
    XFreeGC(display, corr_gc);
    xg_free(cont_xgid);
    xg_free(corr_xgid);
    mbnavadjust_naverr_checkoksettie();
    do_naverr_update();
    do_update_status();
  }

  /* write project file if there are outstanding changes */
  int error = MB_ERROR_NO_ERROR;
  if (project.save_count != 0) {
    mbnavadjust_write_project(mbna_verbose, &project, __FILE__, __LINE__, __FUNCTION__, &error);
    project.save_count = 0;
  }

}

/*--------------------------------------------------------------------*/
void do_fileselection_mode(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  file_mode = (size_t)client_data;

  /* desl with selection */
  if (file_mode == FILE_MODE_NEW) {
    tmp0 = (XmString)BX_CONVERT(fileSelectionBox, "*.nvh", XmRXmString, 0, &argok);
  }
  else if (file_mode == FILE_MODE_OPEN) {
    tmp0 = (XmString)BX_CONVERT(fileSelectionBox, "*.nvh", XmRXmString, 0, &argok);
  }
  else if (file_mode == FILE_MODE_IMPORTDATA) {
    tmp0 = (XmString)BX_CONVERT(fileSelectionBox, "*.mb-1", XmRXmString, 0, &argok);
  }
  else if (file_mode == FILE_MODE_IMPORTSURVEY) {
    tmp0 = (XmString)BX_CONVERT(fileSelectionBox, "*.mb-1", XmRXmString, 0, &argok);
  }
  else if (file_mode == FILE_MODE_REFERENCE) {
    tmp0 = (XmString)BX_CONVERT(fileSelectionBox, "*.grd", XmRXmString, 0, &argok);
  }
  else {
    tmp0 = (XmString)BX_CONVERT(fileSelectionBox, "*.nvh", XmRXmString, 0, &argok);
  }

  ac = 0;
  XtSetArg(args[ac], XmNpattern, tmp0);
  ac++;
  XtSetValues(fileSelectionBox, args, ac);
  XmStringFree((XmString)tmp0);
}

/*--------------------------------------------------------------------*/
void do_fileselection_ok(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* get input filename */
  mb_path ifile;
  mb_get_text_string(fileSelectionBox_text, ifile, sizeof(ifile));

  /* desl with selection */
  int error = MB_ERROR_NO_ERROR;
  mb_pathplus message;
  if (file_mode == FILE_MODE_NEW) {
    snprintf(message, sizeof(message), "Creating new MBnavadjust project %s", ifile);
    do_message_on(message);
    status = mbnavadjust_file_new(ifile);
    do_message_off();
    do_update_status();
  }
  else if (file_mode == FILE_MODE_OPEN) {
    snprintf(message, sizeof(message), "Opening MBnavadjust project %s", ifile);
    do_message_on(message);
    status = mbnavadjust_file_open(ifile);
    do_message_off();
    do_update_status();
  }
  else if (file_mode == FILE_MODE_IMPORTDATA || file_mode == FILE_MODE_IMPORTSURVEY) {
    char format_text[40];
    mb_get_text_string(textField_format, format_text, sizeof(format_text));
    sscanf(format_text, "%d", &format);
    bool import_single_survey = false;
    if (file_mode == FILE_MODE_IMPORTSURVEY) {
    	import_single_survey = true;
    	snprintf(message, sizeof(message), "Importing single survey from %s %d", ifile, format);
    } else {
    	snprintf(message, sizeof(message), "Importing data from %s %d", ifile, format);
    }
    do_message_on(message);
    status = mbnavadjust_import_data(mbna_verbose, &project, ifile, format, import_single_survey, &error);
    do_message_off();

    /* update datalist files and topography grids */
    mbna_status = MBNA_STATUS_NAVSOLVE;
    snprintf(message, sizeof(message), "Updating bathymetry grids...");
    do_message_on(message);
    mbnavadjust_updategrid(mbna_verbose, &project);
    do_message_off();
    mbna_status = MBNA_STATUS_GUI;
    do_update_status();
    if (project.modelplot) {
      do_update_modelplot_status();
      mbnavadjust_modelplot_plot(__FILE__, __LINE__);
    }
    if (project.visualization_status) {
      mbnavadjust_reset_visualization_navties();
      do_update_visualization_status();
    }
  }
  else if (file_mode == FILE_MODE_REFERENCE) {
    status = mbnavadjust_import_reference(mbna_verbose, &project, ifile, &error);
    if (status == MB_SUCCESS) {
      mbnavadjust_write_project(mbna_verbose, &project, __FILE__, __LINE__, __FUNCTION__, &error);
      project.save_count = 0;
    } else {
      status = MB_SUCCESS;
      error = MB_ERROR_NO_ERROR;
    }
    do_update_status();
  }
}

/*--------------------------------------------------------------------*/
void do_fileselection_cancel(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  file_mode = FILE_MODE_NONE;
}

/*--------------------------------------------------------------------*/

void do_view_showallsurveys(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_mode != MBNA_VIEW_MODE_ALL) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_ALL;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_view_showselectedsurvey(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_mode != MBNA_VIEW_MODE_SURVEY) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_SURVEY;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_view_showselectedblock(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_mode != MBNA_VIEW_MODE_BLOCK) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_BLOCK;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_view_showselectedfile(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* no file is selected - nothing to view */
  if (mbna_file_select == MBNA_SELECT_NONE)
    return;

  if (mbna_view_mode != MBNA_VIEW_MODE_FILE) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_FILE;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_view_showwithselectedsurvey(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_mode != MBNA_VIEW_MODE_WITHSURVEY) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_WITHSURVEY;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_view_showwithselectedfile(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* no file is selected - nothing to view */
  if (mbna_file_select == MBNA_SELECT_NONE)
    return;

  if (mbna_view_mode != MBNA_VIEW_MODE_WITHFILE) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_WITHFILE;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_view_showselectedsection(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* no file/section is selected - nothing to view */
  if (mbna_file_select == MBNA_SELECT_NONE || mbna_section_select == MBNA_SELECT_NONE)
    return;

  if (mbna_view_mode != MBNA_VIEW_MODE_WITHSECTION) {
    project.modelplot_uptodate = false;
  }
  mbna_view_mode = MBNA_VIEW_MODE_WITHSECTION;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_list_showreferencegrids(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_REFERENCEGRIDS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_REFERENCEGRIDS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_list_showsurveys(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_SURVEYS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_SURVEYS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_list_showblocks(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_BLOCKS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_BLOCKS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showdata(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_FILES) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_FILES;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}
/*--------------------------------------------------------------------*/

void do_list_showsections(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_FILESECTIONS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_FILESECTIONS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showcrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_CROSSINGS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_CROSSINGS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}
/*--------------------------------------------------------------------*/

void do_list_showmediocrecrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_MEDIOCRECROSSINGS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_MEDIOCRECROSSINGS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}
/*--------------------------------------------------------------------*/

void do_list_showgoodcrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_GOODCROSSINGS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_GOODCROSSINGS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}
/*--------------------------------------------------------------------*/

void do_list_showbettercrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_BETTERCROSSINGS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_BETTERCROSSINGS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showtruecrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_TRUECROSSINGS) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_TRUECROSSINGS;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showcrossingties(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_TIES) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_TIES;
  do_update_status();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showcrossingtiessortedall(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_TIESSORTEDALL) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_TIESSORTEDALL;
  do_update_status();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}


/*--------------------------------------------------------------------*/
void do_list_showcrossingtiessortedworst(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_TIESSORTEDWORST) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_TIESSORTEDWORST;
  do_update_status();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}


/*--------------------------------------------------------------------*/
void do_list_showcrossingtiessortedbad(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_TIESSORTEDBAD) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_TIESSORTEDBAD;
  do_update_status();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showglobalties(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_GLOBALTIES) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_GLOBALTIES;
  do_update_status();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/
void do_list_showglobaltiessorted(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  if (mbna_view_list != MBNA_VIEW_LIST_GLOBALTIESSORTED) {
    project.modelplot_uptodate = false;
  }
  mbna_view_list = MBNA_VIEW_LIST_GLOBALTIESSORTED;
  do_update_status();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_action_poornav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_poornav_file();
  do_update_status();
}

/*--------------------------------------------------------------------*/

void do_action_goodnav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_goodnav_file();
  do_update_status();
}

/*--------------------------------------------------------------------*/

void do_action_fixednav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_fixednav_file();
  do_update_status();
}
/*--------------------------------------------------------------------*/

void do_action_fixedxynav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_fixedxynav_file();
  do_update_status();
}
/*--------------------------------------------------------------------*/

void do_action_fixedznav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_fixedznav_file();
  do_update_status();
}
/*--------------------------------------------------------------------*/

void do_action_tie_xy(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_set_tie_xy();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

void do_action_tie_z(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_set_tie_z();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

void do_action_tie_xyz(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_set_tie_xyz();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

void do_action_tie_unfixed(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_set_tie_unfixed();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

void do_action_tie_fixed(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_set_tie_fixed();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

/* called by mbnavadjust_autopick() every tenth crossing it processes, so
    the crossing/section/tie list display and model plot (if open) do not
    sit unrefreshed for the whole duration of a long autopick run */
static void do_action_autopick_progress(void) {
  do_update_status();
  XmUpdateDisplay(list_data);
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
}

void do_action_autopick(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int error = MB_ERROR_NO_ERROR;

  mbna_status = MBNA_STATUS_AUTOPICK;
  mbnavadjust_autopick(mbna_verbose, &project, mbna_view_list, mbna_view_mode, mbna_survey_select, mbna_survey_select1,
                       mbna_survey_select2, mbna_file_select, mbna_section_select, MBNA_MEDIOCREOVERLAP_THRESHOLD,
                       true, do_action_autopick_progress, &error);
  mbna_status = MBNA_STATUS_GUI;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}
/*--------------------------------------------------------------------*/

void do_action_autopickhorizontal(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  int error = MB_ERROR_NO_ERROR;

  mbna_status = MBNA_STATUS_AUTOPICK;
  mbnavadjust_autopick(mbna_verbose, &project, mbna_view_list, mbna_view_mode, mbna_survey_select, mbna_survey_select1,
                       mbna_survey_select2, mbna_file_select, mbna_section_select, MBNA_MEDIOCREOVERLAP_THRESHOLD,
                       false, do_action_autopick_progress, &error);
  mbna_status = MBNA_STATUS_GUI;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}
/*--------------------------------------------------------------------*/

void do_action_autosetsvsvertical(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbna_status = MBNA_STATUS_AUTOPICK;
  mbnavadjust_autosetsvsvertical();
  mbna_status = MBNA_STATUS_GUI;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_action_analyzecrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_action_checknewcrossings(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* turn on message */
  mb_path message;
  sprintf(message, "Checking for crossings...");
  do_message_on(message);

  int error = MB_ERROR_NO_ERROR;
  mbnavadjust_findcrossings(mbna_verbose, &project, &error);

  /* turn off message */
  do_message_off();

  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}

/*--------------------------------------------------------------------*/

void do_zerozoffsets(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_zerozoffsets();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

void do_unsetskipped(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  mbnavadjust_unsetskipped();
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}

/*--------------------------------------------------------------------*/

void do_action_invertnav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  mb_pathplus message;

  mbna_status = MBNA_STATUS_NAVSOLVE;
  snprintf(message, sizeof(message), "Inverting for navigation adjustment model...");
  do_message_on(message);
  mbnavadjust_invertnav(mbna_verbose, &project);
  do_message_off();
  mbna_status = MBNA_STATUS_GUI;
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_action_updategrids(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  mb_pathplus message;

  mbna_status = MBNA_STATUS_NAVSOLVE;
  snprintf(message, sizeof(message), "Updating bathymetry grids...");
  do_message_on(message);
  mbnavadjust_updategrid(mbna_verbose, &project);
  do_message_off();
  mbna_status = MBNA_STATUS_GUI;
  do_update_status();
//  if (project.modelplot) {
//    do_update_modelplot_status();
//    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
//  }
  if (project.visualization_status) {
    mbnavadjust_reset_visualization_navties();
    do_update_visualization_status();
  }
}

/*--------------------------------------------------------------------*/

void do_apply_nav(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  mb_pathplus message;
  snprintf(message, sizeof(message), "Applying adjusted navigation...");
  do_message_on(message);
  mbnavadjust_applynav(mbna_verbose, &project);
  do_message_off();
  do_update_status();
}

/*--------------------------------------------------------------------*/

void do_modelplot_show(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* get drawingArea size */
  Dimension width;
  Dimension height;
  XtVaGetValues(drawingArea_modelplot, XmNwidth, &width, XmNheight, &height, NULL);
  mbna_modelplot_width = width;
  mbna_modelplot_height = height;
  modp_borders[0] = 0;
  modp_borders[1] = mbna_modelplot_width - 1;
  modp_borders[2] = 0;
  modp_borders[3] = mbna_modelplot_height - 1;

  /* port: the drawing area is the window's canvas (mbna_window_canvases): no X11 graphics
     context, font or cursor to make -- it follows the size set above */
  status = mbnavadjust_set_modelplot_graphics(modp_xgid, modp_borders);

  /* set status flag */
  project.modelplot = true;
  project.modelplot_uptodate = false;
  mbna_modelplot_zoom = false;
  mbna_modelplot_zoom_x1 = 0;
  mbna_modelplot_zoom_x2 = 0;
  mbna_modelplot_start = 0;
  mbna_modelplot_end = 0;

  /* update status */
  do_update_status();
  if (project.modelplot) {
    do_update_modelplot_status();
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
  if (project.visualization_status)
    do_update_visualization_status();
}
/*--------------------------------------------------------------------*/

void do_modelplot_dismiss(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* deallocate graphics */
  project.modelplot = false;
  XFreeGC(display, modp_gc);
  xg_free(modp_xgid);
}

/*--------------------------------------------------------------------*/

/* port: called by the window on its resize event with the shell's new size, which upstream
   asks the bulletin board for on a ConfigureNotify */
void do_modelplot_resize(int width, int height) {
  /* do this only if a resize event happens */
  {

    /* do this only if the shell was REALLY resized and not just moved */
    if (mbna_modelplot_width != width - MBNA_MODELPLOT_LEFT_WIDTH ||
        mbna_modelplot_height != height - MBNA_MODELPLOT_LEFT_HEIGHT) {
      /* set drawing area size */
      mbna_modelplot_width = width - MBNA_MODELPLOT_LEFT_WIDTH;
      mbna_modelplot_height = height - MBNA_MODELPLOT_LEFT_HEIGHT;
      ac = 0;
      XtSetArg(args[ac], XmNwidth, (Dimension)mbna_modelplot_width);
      ac++;
      XtSetArg(args[ac], XmNheight, (Dimension)mbna_modelplot_height);
      ac++;
      XtSetValues(drawingArea_modelplot, args, ac);

      /* deallocate graphics */
      XFreeGC(display, modp_gc);
      xg_free(modp_xgid);

      /* get drawingArea size */
      modp_borders[0] = 0;
      modp_borders[1] = mbna_modelplot_width - 1;
      modp_borders[2] = 0;
      modp_borders[3] = mbna_modelplot_height - 1;

      /* port: the drawing area is the window's canvas (mbna_window_canvases): no X11 graphics
         context, font or cursor to make -- it follows the size set above */
      status = mbnavadjust_set_modelplot_graphics(modp_xgid, modp_borders);

      /* set to replot the model after the final resize when the expose events start */
      project.modelplot_uptodate = false;
    }
  }
}

/*--------------------------------------------------------------------*/

void do_modelplot_fullsize(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* replot model */
  mbna_modelplot_zoom_x1 = 0;
  mbna_modelplot_zoom_x2 = 0;
  if (project.modelplot_style == MBNA_MODELPLOT_TIMESERIES) {
    mbna_modelplot_zoom = false;
    mbna_modelplot_start = 0;
    mbna_modelplot_end = 0;
  }
  else if (project.modelplot_style == MBNA_MODELPLOT_PERTURBATION) {
    mbna_modelplot_zoom = false;
    mbna_modelplot_start = 0;
    mbna_modelplot_end = 0;
  }
  else {
    mbna_modelplot_tiezoom = false;
    mbna_modelplot_tiestartzoom = 0;
    mbna_modelplot_tieendzoom = 0;
    mbna_survey_select = MBNA_SELECT_NONE;
    mbna_survey_select1 = MBNA_SELECT_NONE;
    mbna_survey_select2 = MBNA_SELECT_NONE;
  }

  project.modelplot_uptodate = false;
  mbnavadjust_modelplot_setzoom();
  mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  do_update_modelplot_status();
}

/*--------------------------------------------------------------------*/

void do_modelplot_input(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter

  XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* If there is input in the drawing area */
  if (acs->reason == XmCR_INPUT) {
    XEvent *event = acs->event;

    /* Check for mouse pressed. */
    if (event->xany.type == ButtonPress) {
      /* If left mouse button is pushed */
      if (event->xbutton.button == 1) {
        button1down = true;

      } /* end of left button events */

      /* If middle mouse button is pushed */
      if (event->xbutton.button == 2) {
        button2down = true;
      } /* end of middle button events */

      /* If right mouse button is pushed */
      if (event->xbutton.button == 3) {
        button3down = true;
        mbna_modelplot_zoom_x1 = event->xbutton.x;
        mbna_modelplot_zoom_x2 = event->xbutton.x;

        /* replot model if zoom set */
        project.modelplot_uptodate = false;
        mbnavadjust_modelplot_plot(__FILE__, __LINE__);
      } /* end of right button events */
    } /* end of button press events */

    /* Check for mouse released. */
    if (event->xany.type == ButtonRelease) {
      /* If left mouse button is released */
      if (event->xbutton.button == 1) {
        button1down = false;

        /* pick nearest tie point */
        mbnavadjust_modelplot_pick(event->xbutton.x, event->xbutton.y);

        /* if a pick happened update everything */
        if (!project.modelplot_uptodate) {
          /* update model status */
          do_update_modelplot_status();

          /* update status */
          do_update_status();

          /* update model status */
          do_update_modelplot_status();

          /* replot model */
          mbnavadjust_modelplot_plot(__FILE__, __LINE__);

          /* update visualization */
          if (project.visualization_status)
            do_update_visualization_status();
        }
      }

      /* If middle mouse button is released */
      if (event->xbutton.button == 2) {
        button2down = false;

        /* pick nearest tie point */
        mbnavadjust_modelplot_middlepick(event->xbutton.x, event->xbutton.y);

        /* if a pick happened update everything */
        if (!project.modelplot_uptodate) {
          /* update model status */
          do_update_modelplot_status();

          /* update status */
          do_update_status();

          /* update model status */
          do_update_modelplot_status();

          /* replot model */
          mbnavadjust_modelplot_plot(__FILE__, __LINE__);

          /* update visualization */
          if (project.visualization_status)
            do_update_visualization_status();
        }
      }

      /* If right mouse button is released */
      if (event->xbutton.button == 3) {
        button3down = false;
        mbna_modelplot_zoom_x2 = event->xbutton.x;

        /* update model status */
        do_update_modelplot_status();

        /* replot model if zoom set */
        mbnavadjust_modelplot_setzoom();
        project.modelplot_uptodate = false;
        mbnavadjust_modelplot_plot(__FILE__, __LINE__);
        mbna_modelplot_zoom_x1 = 0;
        mbna_modelplot_zoom_x2 = 0;
      }

    } /* end of button release events */

    /* Check for mouse motion while pressed. */
    if (event->xany.type == MotionNotify) {
      /* If right mouse button is held during motion */
      if (button3down) {
        mbna_modelplot_zoom_x2 = event->xbutton.x;

        /* replot model */
        project.modelplot_uptodate = false;
        mbnavadjust_modelplot_plot(__FILE__, __LINE__);
      }
    }
  } /* end of inputs from window */
}

/*--------------------------------------------------------------------*/

void do_modelplot_expose(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  /* update model status */
  do_update_modelplot_status();

  /* replot the model */
  // project.modelplot_uptodate = false;
  mbnavadjust_modelplot_plot(__FILE__, __LINE__);
}

/*--------------------------------------------------------------------*/

void do_modelplot_tieoffsets(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  // fprintf(stderr,"Called do_modelplot_tieoffsets %d\n",XmToggleButtonGetState(toggleButton_modelplot_tieoffsets));

  if (XmToggleButtonGetState(toggleButton_modelplot_tieoffsets)) {
    project.modelplot_style = MBNA_MODELPLOT_TIEOFFSETS;
    project.modelplot_uptodate = false;

    /* update model status */
    do_update_modelplot_status();

    /* replot the model */
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
}

/*--------------------------------------------------------------------*/

void do_modelplot_perturbation(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  // fprintf(stderr,"Called do_modelplot_perturbation %d\n",XmToggleButtonGetState(toggleButton_modelplot_perturbation));

  if (XmToggleButtonGetState(toggleButton_modelplot_perturbation)) {
    project.modelplot_style = MBNA_MODELPLOT_PERTURBATION;
    project.modelplot_uptodate = false;

    /* update model status */
    do_update_modelplot_status();

    /* replot the model */
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
}

/*--------------------------------------------------------------------*/

void do_modelplot_timeseries(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  // fprintf(stderr,"Called do_modelplot_timeseries %d\n",XmToggleButtonGetState(toggleButton_modelplot_timeseries));

  if (XmToggleButtonGetState(toggleButton_modelplot_timeseries)) {
    project.modelplot_style = MBNA_MODELPLOT_TIMESERIES;
    project.modelplot_uptodate = false;

    /* update model status */
    do_update_modelplot_status();

    /* replot the model */
    mbnavadjust_modelplot_plot(__FILE__, __LINE__);
  }
}

/*--------------------------------------------------------------------*/

void do_modelplot_clearblock(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;
  /* fprintf(stderr,"Called do_modelplot_clearblock\n"); */

  mbnavadjust_modelplot_clearblock();

  if (mbna_naverr_mode != MBNA_NAVERR_MODE_UNLOADED) {
    mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
    do_naverr_update();
  }

  /* update status */
  do_update_status();

  /* update model status */
  do_update_modelplot_status();

  /* replot the model */
  mbnavadjust_modelplot_plot(__FILE__, __LINE__);
}
/*--------------------------------------------------------------------*/

void do_visualize(Widget w, XtPointer client_data, XtPointer call_data) {
  (void)w; // Unused parameter
  (void)client_data; // Unused parameter
  (void)call_data; // Unused parameter

  // XmAnyCallbackStruct *acs = (XmAnyCallbackStruct *)call_data;

  int grid_id = MBNA_GRID_PROJECT;
  if (mbna_view_mode == MBNA_VIEW_MODE_SURVEY)
    grid_id = mbna_survey_select;
  mbnavadjust_open_visualization(grid_id);
}
/*--------------------------------------------------------------------*/

static int do_visualize_dismiss_notify(size_t instance) {
  (void)instance; // Unused parameter
  const int status = mbnavadjust_dismiss_visualization();
  do_visualize_sensitivity();
  return (status);
}
/*--------------------------------------------------------------------*/

void do_visualize_sensitivity(void) {
  if (project.grid_status != MBNA_GRID_NONE && !project.visualization_status)
    XtVaSetValues(pushButton_visualize, XmNsensitive, True, NULL);
  else
    XtVaSetValues(pushButton_visualize, XmNsensitive, False, NULL);
}
/*--------------------------------------------------------------------*/

/* port: libmbview's shared data held the picks (route_selected, nav_selected[2]) with the routes'
   and navs' names; the viewer window answers with the NAMES it was given (mbna_view_addnav,
   mbna_view_addroute), kept here. An empty name = MBV_SELECT_NONE. */
static mb_path view_route_selected = "";
static mb_path view_nav_selected[2] = {"", ""};
static mb_path view_nav_selected_mbnavadjust[2] = {"", ""};

void mbna_view_picked_route(const char *name) {
  snprintf(view_route_selected, sizeof(view_route_selected), "%s", name != NULL ? name : "");
  do_pickroute_notify(0);
}

void mbna_view_picked_navs(const char *name1, const char *name2) {
  snprintf(view_nav_selected[0], sizeof(view_nav_selected[0]), "%s", name1 != NULL ? name1 : "");
  snprintf(view_nav_selected[1], sizeof(view_nav_selected[1]), "%s", name2 != NULL ? name2 : "");
  do_picknav_notify(0);
}

/* the window went away: its X, as mbview's dismiss notify */
void mbna_view_closed(void) {
  view_route_selected[0] = '\0';
  view_nav_selected[0][0] = view_nav_selected[1][0] = '\0';
  view_nav_selected_mbnavadjust[0][0] = view_nav_selected_mbnavadjust[1][0] = '\0';
  do_visualize_dismiss_notify(0);
}

/* port: do_update_visualization_status clears the navadjust picks it is about to re-pick */
static void mbna_view_clear_navadjust_picks(void) {
  view_nav_selected_mbnavadjust[0][0] = view_nav_selected_mbnavadjust[1][0] = '\0';
}

/*--------------------------------------------------------------------*/

static void do_pickroute_notify(size_t instance) {
  if (mbna_verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:    %zu\n", instance);
  }

  if (mbna_verbose > 0)
    fprintf(stderr, "do_pickroute_notify:%zu\n", instance);

  int status = MB_SUCCESS;

  /* get selected route and translate to selected crossing */
  if (view_route_selected[0] != '\0') {
    if (mbna_modelplot_mode == MBNA_MODELPLOT_MODE_SECTIONS) {
      int isurvey;
      int ifile;
      int isection;
      sscanf(view_route_selected, "%d:%d:%d", &isurvey, &ifile, &isection);
      mbna_current_file = ifile;
      mbna_current_section = isection;
      mbna_file_select = ifile;
      mbna_section_select = isection;
      mbna_survey_select = project.files[mbna_file_select].survey;
      mbna_file_id_2 = ifile;
      mbna_section_2 = isection;

      /* bring up naverr window if required */
      if (mbna_current_section != MBV_SELECT_NONE && mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_SECTION);
      }

      /* or replot the existing window */
      else if (mbna_current_section != MBV_SELECT_NONE) {
        mbnavadjust_naverr_specific_section(mbna_file_select, mbna_section_select);
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
        do_update_status();
      }
    }
    else {
      int icrossing;
      int itie;
      int ifile1;
      int isection1;
      int isnav1;
      int ifile2;
      int isection2;
      int isnav2;
      sscanf(view_route_selected, "%d:%d %d:%d:%d %d:%d:%d", &icrossing, &itie, &ifile1, &isection1, &isnav1, &ifile2,
             &isection2, &isnav2);
      mbnavadjust_visualization_selectcrossingfromroute(icrossing, itie);

      /* bring up naverr window if required */
      if (mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }
      else {
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
        do_update_status();
      }
    }
    if (project.modelplot) {
      do_update_modelplot_status();
      mbnavadjust_modelplot_plot(__FILE__, __LINE__);
    }
    if (project.visualization_status)
      do_update_visualization_status();
  }

  if (mbna_verbose > 0)
    fprintf(stderr, "return do_pickroute_notify status:%d\n", status);
}

/*--------------------------------------------------------------------*/

static void do_picknav_notify(size_t instance) {
  /* print input debug statements */
  if (mbna_verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:    %zu\n", instance);
  }

  int status = MB_SUCCESS;

  /* if set to look at global ties then translate selected navigation to
     single section and open that in naverr with reference grid */
  if (mbna_view_list == MBNA_VIEW_LIST_FILESECTIONS
      || mbna_view_list == MBNA_VIEW_LIST_GLOBALTIES
      || mbna_view_list == MBNA_VIEW_LIST_GLOBALTIESSORTED) {
    memcpy(view_nav_selected_mbnavadjust, view_nav_selected, sizeof(view_nav_selected));
    if (view_nav_selected_mbnavadjust[0][0] != '\0') {
      int ifile1;
      int isection1;
      sscanf(view_nav_selected_mbnavadjust[0], "%d:%d", &ifile1, &isection1);
      mbna_current_file = ifile1;
      mbna_current_section = isection1;
      mbna_file_select = ifile1;
      mbna_section_select = isection1;
      mbna_survey_select = project.files[mbna_file_select].survey;
      mbna_file_id_2 = ifile1;
      mbna_section_2 = isection1;

      /* bring up naverr window if required */
      if (mbna_current_section != MBV_SELECT_NONE && mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_SECTION);
      }

      /* or replot the existing window */
      else if (mbna_current_section != MBV_SELECT_NONE) {
        mbnavadjust_naverr_specific_section(mbna_file_select, mbna_section_select);
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
        do_update_status();
      }
      if (project.modelplot) {
        do_update_modelplot_status();
        mbnavadjust_modelplot_plot(__FILE__, __LINE__);
      }
      if (project.visualization_status)
        do_update_visualization_status();
    }
  }

  /* else if set to look at crossings get selected navigation and translate to selected crossing */
  else {
    memcpy(view_nav_selected_mbnavadjust, view_nav_selected, sizeof(view_nav_selected));
    if (view_nav_selected_mbnavadjust[0][0] != '\0' && view_nav_selected_mbnavadjust[1][0] != '\0') {
      int ifile1;
      int isection1;
      int ifile2;
      int isection2;
      sscanf(view_nav_selected_mbnavadjust[0], "%d:%d", &ifile1, &isection1);
      sscanf(view_nav_selected_mbnavadjust[1], "%d:%d", &ifile2, &isection2);
      status = mbnavadjust_visualization_selectcrossingfromnav(ifile1, isection1, ifile2, isection2);

      /* bring up naverr window if required */
      if (mbna_current_crossing != MBV_SELECT_NONE && mbna_naverr_mode == MBNA_NAVERR_MODE_UNLOADED) {
        do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      }

      /* or replot the existing window */
      else if (mbna_current_crossing != MBV_SELECT_NONE) {
        mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
        do_naverr_update();
        do_update_status();
      }
      if (project.modelplot) {
        do_update_modelplot_status();
        mbnavadjust_modelplot_plot(__FILE__, __LINE__);
      }
      if (project.visualization_status)
        do_update_visualization_status();
    }
  }

  if (mbna_verbose > 0)
    fprintf(stderr, "return do_picknav_notify status:%d\n", status);
}

/*--------------------------------------------------------------------*/
/* ARGSUSED */
void do_mbnavadjust_addcrossing(Widget w, XtPointer client, XtPointer call) {
  (void)w; // Unused parameter
  (void)client; // Unused parameter
  (void)call; // Unused parameter

  int error = MB_ERROR_NO_ERROR;

  if (mbna_current_crossing == MBNA_SELECT_NONE && view_nav_selected_mbnavadjust[0][0] != '\0' &&
      view_nav_selected_mbnavadjust[1][0] != '\0' &&
      strcmp(view_nav_selected_mbnavadjust[0], view_nav_selected_mbnavadjust[1]) != 0) {
    /* add and select a new crossing */
    int ifile1;
    int isection1;
    int ifile2;
    int isection2;
    sscanf(view_nav_selected_mbnavadjust[0], "%d:%d", &ifile1, &isection1);
    sscanf(view_nav_selected_mbnavadjust[1], "%d:%d", &ifile2, &isection2);
    status = mbnavadjust_addcrossing(mbna_verbose, &project, ifile1, isection1, ifile2, isection2, &error);
    if (status == MB_SUCCESS)
      mbna_crossing_select = project.num_crossings - 1;
    else
      mbna_crossing_select = MBNA_SELECT_NONE;
    mbna_tie_select = MBNA_SELECT_NONE;

    /* bring up naverr window */
    if (status == MB_SUCCESS) {
      do_naverr_init(MBNA_NAVERR_MODE_CROSSING);
      mbnavadjust_naverr_plot(MBNA_PLOT_MODE_FIRST);
      do_naverr_update();
      do_update_status();
    }
    if (project.modelplot) {
      project.modelplot_uptodate = false;
      do_update_modelplot_status();
      mbnavadjust_modelplot_plot(__FILE__, __LINE__);
    }
    if (project.visualization_status)
      do_update_visualization_status();
  }
}

/* port: whether "Add and open new crossing" (mbview's action button) has what it needs */
int mbna_view_can_addcrossing(void) {
  return mbna_current_crossing == MBNA_SELECT_NONE && view_nav_selected_mbnavadjust[0][0] != '\0' &&
         view_nav_selected_mbnavadjust[1][0] != '\0' &&
         strcmp(view_nav_selected_mbnavadjust[0], view_nav_selected_mbnavadjust[1]) != 0;
}

/*--------------------------------------------------------------------*/
/* ARGSUSED */
void do_fileselection_list(Widget w, XtPointer client, XtPointer call) {
  (void)w; // Unused parameter
  (void)client; // Unused parameter
  (void)call; // Unused parameter

  /* get selected text */
  mb_path string = "";
  mb_get_text_string(fileSelectionBox_text, string, sizeof(string));

  /* get output file */
  if ((int)strlen(string) > 0) {
    int error;
    char fileroot[MB_PATH_MAXLINE];
    int form;
    status = mb_get_format(mbna_verbose, string, fileroot, &form, &error);
    if (status == MB_SUCCESS) {
      format = form;
      char value_text[MB_PATH_MAXLINE];
      snprintf(value_text, sizeof(value_text), "%d", format);
      XmTextFieldSetString(textField_format, value_text);
    }
  }
}

/*--------------------------------------------------------------------*/
/* port: do_wait_until_viewed ran the X event loop until the main window was mapped; the window
   is shown and laid out by the host before do_mbnavadjust_init, so there is nothing to wait for */

/*--------------------------------------------------------------------*/

/* port: the message bulletin board is the viewer's busy notice -- the window raises it, labelled
   with label_message, when bulletinBoard_message is managed, and paints it at once, so the wait
   loop upstream runs until the dialog is mapped has nothing left to wait for */
int do_message_on(char *message) {
  if (mbna_verbose >= 1)
    fprintf(stderr, "%s\n", message);

  set_label_string(label_message, message);
  XtManageChild(bulletinBoard_message);
  XmUpdateDisplay(bulletinBoard_message);

  return (MB_SUCCESS);
}
/*--------------------------------------------------------------------*/

int do_message_update(char *message) {
  if (mbna_verbose >= 1)
    fprintf(stderr, "%s\n", message);

  set_label_string(label_message, message);
  XmUpdateDisplay(bulletinBoard_message);

  return (MB_SUCCESS);
}

/*--------------------------------------------------------------------*/

int do_message_off() {
  XtUnmanageChild(bulletinBoard_message);
  XmUpdateDisplay(bulletinBoard_message);

  return (MB_SUCCESS);
}

/*--------------------------------------------------------------------*/

int do_info_add(char *info, int timetag) {
  int status = MB_SUCCESS;

  /* add text -- port: at the end of the message text, which is then shown (XmTextInsert at the
     last position, XmTextShowPosition) */
  if (timetag)
    mbna_text_append(text_messages, info);
  if (project.logfp != NULL)
    fputs(info, project.logfp);
  if (mbna_verbose > 0)
    fputs(info, stderr);

  /* put time tag in if requested */
  if (timetag) {
    int error = MB_ERROR_NO_ERROR;
    char user[256], host[256], date[32];
    status = mb_user_host_date(mbna_verbose, user, host, date, &error);
    mb_path tag;
    snprintf(tag, sizeof(tag), " > User <%s> on cpu <%s> at <%s>\n", user, host, date);
    mbna_text_append(text_messages, tag);
    if (project.logfp != NULL)
      fputs(tag, project.logfp);
    if (mbna_verbose > 0)
      fputs(tag, stderr);
  }

  return (status);
}

/*--------------------------------------------------------------------*/

/* port: the error bulletin board is a message box of the window, made of its three labels */
int do_error_dialog(char *s1, char *s2, char *s3) {
  set_label_string(label_error_one, s1);
  set_label_string(label_error_two, s2);
  set_label_string(label_error_three, s3);
  mbna_XBell(100);
  XtManageChild(bulletinBoard_error);

  return (MB_SUCCESS);
}

/*--------------------------------------------------------------------*/
void do_bell(int length) {
  mbna_XBell(length);
}

/*--------------------------------------------------------------------*/
/* port: set_label_string and set_label_multiline_string are the window's (mbna_motif.h) */
/*--------------------------------------------------------------------*/
