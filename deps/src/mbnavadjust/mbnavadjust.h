/*--------------------------------------------------------------------
 *    The MB-system:	mbnavadjust.h	3/23/00
 *
 *    Copyright © 2000-2026 by
 *    David W. Caress, caress@mbari.org
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
 * This include file contains global control parameters shared with
 * the Motif interface code.
 *
 * Author:	D. W. Caress
 * Date:	March 22, 2000
 *
 * InteractiveGMT port, deps/src/mbnavadjust/mbnavadjust.h: the engine -- mbnavadjust.c, the port of
 * mbnavadjust_prog.c -- and the host hooks it draws and talks through. The Qt window,
 * mbnavadjust_window.cpp, the port of mbnavadjust_callbacks.c + mbnavadjust_creation.c, implements
 * the hooks. Differences from the upstream header:
 *   - the Motif callback prototypes, on Widget and XtPointer, are gone: the window's slots replace them;
 *   - mbnavadjust_init takes the command line options as arguments;
 *   - the xg_* graphics, the do_* calls and the visualization, mbna_view_*, are mapped onto
 *     mbna_-prefixed symbols, so they can never bind to the other ported tools' own hooks
 *     mbedit_xg_*, mbnav_xg_*, ... in the same library, nor to a real MB-System library loaded
 *     in the same process.
 *--------------------------------------------------------------------*/

#ifndef MBNAVADJUST_H_
#define MBNAVADJUST_H_

#include <stdbool.h>
#include <stddef.h>

#include "mbnavadjust_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- host hooks: implemented by the Qt window, mbnavadjust_window.cpp ---------------------- */
#define XG_SOLIDLINE 0
#define XG_DASHLINE 1
#define xg_drawline        mbna_xg_drawline
#define xg_drawrectangle   mbna_xg_drawrectangle
#define xg_fillrectangle   mbna_xg_fillrectangle
#define xg_drawstring      mbna_xg_drawstring
#define xg_justify         mbna_xg_justify
#define xg_setclip         mbna_xg_setclip

void xg_drawline(void *xgid, int x1, int y1, int x2, int y2, unsigned int pixel, int style);
void xg_drawrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_fillrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_drawstring(void *xgid, int x, int y, char *string, unsigned int pixel, int style);
void xg_justify(void *xgid, char *string, int *width, int *ascent, int *descent);
void xg_setclip(void *xgid, int x, int y, int width, int height);

#define do_update_status               mbna_do_update_status
#define do_update_modelplot_status     mbna_do_update_modelplot_status
#define do_naverr_init                 mbna_do_naverr_init
#define do_naverr_update               mbna_do_naverr_update
#define do_message_on                  mbna_do_message_on
#define do_message_update              mbna_do_message_update
#define do_message_off                 mbna_do_message_off
#define do_error_dialog                mbna_do_error_dialog
#define do_info_add                    mbna_do_info_add
#define do_check_crossing_listok       mbna_do_check_crossing_listok
#define do_check_globaltie_listok      mbna_do_check_globaltie_listok
#define do_check_section_listok        mbna_do_check_section_listok
#define do_check_nav_active            mbna_do_check_nav_active
#define do_update_visualization_status mbna_do_update_visualization_status
#define do_visualize_sensitivity       mbna_do_visualize_sensitivity

void do_update_status(void);
void do_update_modelplot_status(void);
void do_naverr_init(int mode);
void do_naverr_update(void);
int do_message_on(char *message);
int do_message_update(char *message);
int do_message_off(void);
int do_error_dialog(char *s1, char *s2, char *s3);
int do_info_add(char *info, int timetag);
int do_check_crossing_listok(int icrossing);
int do_check_globaltie_listok(int ifile, int isection);
int do_check_section_listok(int ifile, int isection);
int do_check_nav_active(int ifile, int isection);
void do_update_visualization_status(void);
void do_visualize_sensitivity(void);

/* the visualization, libmbview upstream: the viewer's own window. mbna_view_open opens the grid
   through the viewer's file door into a window of its own and returns true on success; nav tracks and
   tie routes are lines of that window, picked back by name -- see mbnavadjust_window.cpp. */
bool mbna_view_open(const char *title, const char *gridfile);
void mbna_view_destroy(void);
void mbna_view_update(void);
bool mbna_view_addnav(const char *name, int npts, const double *lon, const double *lat, const double *z, int color,
                      int size);
void mbna_view_deleteallroutes(void);
bool mbna_view_addroute(int npts, const double *lon, const double *lat, int color, int size, const char *name);
/* the window's picks, by name: the sections and the tie route of the crossing being analysed */
void mbna_view_clearnavpicks(void);
void mbna_view_picknav(const char *name);
void mbna_view_pickroute(const char *name);

/* libmbview's route calls, as mbnavadjust_reset_visualization_navties makes them; a colour is
   mbview's colour index, ROUTE_COLOR_* of mbnavadjust_io.h */
#define mbview_deleteallroutes(verbose, instance, error) mbna_view_deleteallroutes()
#define mbview_addroute(verbose, instance, npoint, lon, lat, waypoint, color, size, editmode, name, id, error) \
	((void)(id), mbna_view_addroute((npoint), (lon), (lat), (color), (size), (name)) ? MB_SUCCESS : MB_FAILURE)

/* ---- the engine, mbnavadjust.c ------------------------------------------------------------ */
int mbnavadjust_init_globals(void);
int mbnavadjust_init(const char *ifile, bool dark_background, bool reset_crossings);
int mbnavadjust_set_colors(int ncol, int *pixels);
int mbnavadjust_set_borders(int *cn_brdr, int *cr_brdr, int *zc_brdr);
int mbnavadjust_set_graphics(void *cn_xgid, void *cr_xgid, void *zc_xgid);
int mbnavadjust_file_new(char *projectname);
int mbnavadjust_file_open(char *projectname);
int mbnavadjust_poornav_file(void);
int mbnavadjust_goodnav_file(void);
int mbnavadjust_fixednav_file(void);
int mbnavadjust_fixedxynav_file(void);
int mbnavadjust_fixedznav_file(void);
int mbnavadjust_set_tie_xy(void);
int mbnavadjust_set_tie_z(void);
int mbnavadjust_set_tie_xyz(void);
int mbnavadjust_set_tie_unfixed(void);
int mbnavadjust_set_tie_fixed(void);
int mbnavadjust_naverr_save(void);
int mbnavadjust_naverr_specific_crossing(int new_crossing, int new_tie);
int mbnavadjust_naverr_specific_section(int new_file, int new_section);
int mbnavadjust_naverr_next_crossing(void);
int mbnavadjust_naverr_next_section(void);
int mbnavadjust_naverr_previous_crossing(void);
int mbnavadjust_naverr_previous_section(void);
int mbnavadjust_naverr_nextunset_crossing(void);
int mbnavadjust_naverr_nextunset_section(void);
int mbnavadjust_naverr_selecttie(void);
int mbnavadjust_naverr_deletetie(void);
int mbnavadjust_deletetie(int icrossing, int jtie, int deletestatus);
int mbnavadjust_naverr_resettie(void);
int mbnavadjust_naverr_checkoksettie(void);
int mbnavadjust_naverr_skip(void);
int mbnavadjust_naverr_unset(void);
int mbnavadjust_naverr_replot(void);
int mbnavadjust_referencesection_replot(void);
void mbnavadjust_naverr_plot(int plotmode);
int mbnavadjust_autosetsvsvertical(void);
int mbnavadjust_zerozoffsets(void);
int mbnavadjust_unsetskipped(void);
int mbnavadjust_referenceplussection_load(void);
int mbnavadjust_referencegrid_unload(void);
int mbnavadjust_modelplot_plot(const char *sourcefile, int sourceline);
int mbnavadjust_set_modelplot_graphics(void *modp_xgid, int *modp_borders);
int mbnavadjust_modelplot_plot_timeseries(void);
int mbnavadjust_modelplot_plot_perturbation(void);
int mbnavadjust_modelplot_plot_tieoffsets(void);
int mbnavadjust_modelplot_pick(int x, int y);
int mbnavadjust_modelplot_pick_timeseries_crossingties(int x, int y);
int mbnavadjust_modelplot_pick_timeseries_globalties(int x, int y);
int mbnavadjust_modelplot_pick_perturbation_crossingties(int x, int y);
int mbnavadjust_modelplot_pick_perturbation_globalties(int x, int y);
int mbnavadjust_modelplot_pick_tieoffsets(int x, int y);
int mbnavadjust_modelplot_middlepick(int x, int y);
int mbnavadjust_modelplot_middlepick_timeseries_crossingties(int x, int y);
int mbnavadjust_modelplot_middlepick_timeseries_globalties(int x, int y);
int mbnavadjust_modelplot_middlepick_perturbation_crossingties(int x, int y);
int mbnavadjust_modelplot_middlepick_perturbation_globalties(int x, int y);
int mbnavadjust_modelplot_middlepick_tieoffsets(int x, int y);
int mbnavadjust_modelplot_setzoom(void);
int mbnavadjust_modelplot_clearblock(void);
int mbnavadjust_open_visualization(int which_grid);
int mbnavadjust_dismiss_visualization(void);
int mbnavadjust_reset_visualization_navties(void);
int mbnavadjust_visualization_selectcrossingfromroute(int icrossing, int itie);
int mbnavadjust_visualization_selectcrossingfromnav(int ifile1, int isection1, int ifile2, int isection2);

#ifdef __cplusplus
}
#endif

#endif /* MBNAVADJUST_H_ */
