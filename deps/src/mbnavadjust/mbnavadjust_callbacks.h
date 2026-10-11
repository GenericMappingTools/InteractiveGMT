/*======================================================================================
 * mbnavadjust_callbacks.h -- what the Qt window, mbnavadjust_window.cpp, calls in the callbacks of
 * the mbnavadjust port, mbnavadjust_callbacks.c: the Motif callbacks under their upstream names
 * and signatures, a Widget being its name, mbna_motif.h, plus the few port entry points that
 * replace what X11 delivered -- canvas input, expose, resize, the file selection.
 *
 * Every name is mapped onto an mbna_-prefixed symbol, so the callbacks can never bind to another
 * ported tool's do_* functions in the same library.
 *====================================================================================*/

#ifndef MBNAVADJUST_CALLBACKS_H_
#define MBNAVADJUST_CALLBACKS_H_

#include <stdbool.h>
#include "mbna_motif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define do_action_analyzecrossings mbna_do_action_analyzecrossings
#define do_action_autopick mbna_do_action_autopick
#define do_action_autopickhorizontal mbna_do_action_autopickhorizontal
#define do_action_autosetsvsvertical mbna_do_action_autosetsvsvertical
#define do_action_checknewcrossings mbna_do_action_checknewcrossings
#define do_action_fixednav mbna_do_action_fixednav
#define do_action_fixedxynav mbna_do_action_fixedxynav
#define do_action_fixedznav mbna_do_action_fixedznav
#define do_action_goodnav mbna_do_action_goodnav
#define do_action_invertnav mbna_do_action_invertnav
#define do_action_poornav mbna_do_action_poornav
#define do_action_tie_fixed mbna_do_action_tie_fixed
#define do_action_tie_unfixed mbna_do_action_tie_unfixed
#define do_action_tie_xy mbna_do_action_tie_xy
#define do_action_tie_xyz mbna_do_action_tie_xyz
#define do_action_tie_z mbna_do_action_tie_z
#define do_action_updategrids mbna_do_action_updategrids
#define do_apply_nav mbna_do_apply_nav
#define do_biases_apply mbna_do_biases_apply
#define do_biases_applyall mbna_do_biases_applyall
#define do_biases_heading mbna_do_biases_heading
#define do_biases_init mbna_do_biases_init
#define do_biases_roll mbna_do_biases_roll
#define do_biases_toggle mbna_do_biases_toggle
#define do_controls_apply mbna_do_controls_apply
#define do_controls_scale_colorinterval mbna_do_controls_scale_colorinterval
#define do_controls_show mbna_do_controls_show
#define do_file_close mbna_do_file_close
#define do_file_importdata mbna_do_file_importdata
#define do_file_new mbna_do_file_new
#define do_file_open mbna_do_file_open
#define do_fileselection_cancel mbna_do_fileselection_cancel
#define do_fileselection_mode mbna_do_fileselection_mode
#define do_fileselection_ok mbna_do_fileselection_ok
#define do_list_data_select mbna_do_list_data_select
#define do_list_showbettercrossings mbna_do_list_showbettercrossings
#define do_list_showblocks mbna_do_list_showblocks
#define do_list_showcrossings mbna_do_list_showcrossings
#define do_list_showcrossingties mbna_do_list_showcrossingties
#define do_list_showcrossingtiessorted mbna_do_list_showcrossingtiessorted
#define do_list_showcrossingtiessortedall mbna_do_list_showcrossingtiessortedall
#define do_list_showcrossingtiessortedbad mbna_do_list_showcrossingtiessortedbad
#define do_list_showcrossingtiessortedworst mbna_do_list_showcrossingtiessortedworst
#define do_list_showdata mbna_do_list_showdata
#define do_list_showglobalties mbna_do_list_showglobalties
#define do_list_showglobaltiessorted mbna_do_list_showglobaltiessorted
#define do_list_showgoodcrossings mbna_do_list_showgoodcrossings
#define do_list_showmediocrecrossings mbna_do_list_showmediocrecrossings
#define do_list_showreferencegrids mbna_do_list_showreferencegrids
#define do_list_showsections mbna_do_list_showsections
#define do_list_showsurveys mbna_do_list_showsurveys
#define do_list_showtruecrossings mbna_do_list_showtruecrossings
#define do_modelplot_clearblock mbna_do_modelplot_clearblock
#define do_modelplot_dismiss mbna_do_modelplot_dismiss
#define do_modelplot_expose mbna_do_modelplot_expose
#define do_modelplot_fullsize mbna_do_modelplot_fullsize
#define do_modelplot_perturbation mbna_do_modelplot_perturbation
#define do_modelplot_show mbna_do_modelplot_show
#define do_modelplot_tieoffsets mbna_do_modelplot_tieoffsets
#define do_modelplot_timeseries mbna_do_modelplot_timeseries
#define do_naverr_addtie mbna_do_naverr_addtie
#define do_naverr_applyzoffset mbna_do_naverr_applyzoffset
#define do_naverr_cont_expose mbna_do_naverr_cont_expose
#define do_naverr_corr_expose mbna_do_naverr_corr_expose
#define do_naverr_deletetie mbna_do_naverr_deletetie
#define do_naverr_dismiss mbna_do_naverr_dismiss
#define do_naverr_fullsize mbna_do_naverr_fullsize
#define do_naverr_minmisfit mbna_do_naverr_minmisfit
#define do_naverr_minxymisfit mbna_do_naverr_minxymisfit
#define do_naverr_misfitcenter mbna_do_naverr_misfitcenter
#define do_naverr_next mbna_do_naverr_next
#define do_naverr_nextunset mbna_do_naverr_nextunset
#define do_naverr_previous mbna_do_naverr_previous
#define do_naverr_resettie mbna_do_naverr_resettie
#define do_naverr_selecttie mbna_do_naverr_selecttie
#define do_naverr_setnone mbna_do_naverr_setnone
#define do_naverr_setoffset mbna_do_naverr_setoffset
#define do_naverr_settie mbna_do_naverr_settie
#define do_naverr_unset mbna_do_naverr_unset
#define do_naverr_zerooffset mbna_do_naverr_zerooffset
#define do_naverr_zerozoffset mbna_do_naverr_zerozoffset
#define do_quit mbna_do_quit
#define do_scale_contourinterval mbna_do_scale_contourinterval
#define do_scale_controls_decimation mbna_do_scale_controls_decimation
#define do_scale_controls_sectionlength mbna_do_scale_controls_sectionlength
#define do_scale_controls_sectionsoundings mbna_do_scale_controls_sectionsoundings
#define do_scale_controls_smoothing mbna_do_scale_controls_smoothing
#define do_scale_controls_tickinterval mbna_do_scale_controls_tickinterval
#define do_scale_controls_zoffset mbna_do_scale_controls_zoffset
#define do_unsetskipped mbna_do_unsetskipped
#define do_view_showallsurveys mbna_do_view_showallsurveys
#define do_view_showbettercrossings mbna_do_view_showbettercrossings
#define do_view_showblocks mbna_do_view_showblocks
#define do_view_showcrossings mbna_do_view_showcrossings
#define do_view_showdata mbna_do_view_showdata
#define do_view_showgoodcrossings mbna_do_view_showgoodcrossings
#define do_view_showmediocrecrossings mbna_do_view_showmediocrecrossings
#define do_view_showsections mbna_do_view_showsections
#define do_view_showselectedblock mbna_do_view_showselectedblock
#define do_view_showselectedfile mbna_do_view_showselectedfile
#define do_view_showselectedsection mbna_do_view_showselectedsection
#define do_view_showselectedsurvey mbna_do_view_showselectedsurvey
#define do_view_showsurveys mbna_do_view_showsurveys
#define do_view_showtruecrossings mbna_do_view_showtruecrossings
#define do_view_showwithselectedfile mbna_do_view_showwithselectedfile
#define do_view_showwithselectedsurvey mbna_do_view_showwithselectedsurvey
#define do_visualize mbna_do_visualize
#define do_zerozoffsets mbna_do_zerozoffsets

void do_action_analyzecrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_autopick(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_autopickhorizontal(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_autosetsvsvertical(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_checknewcrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_fixednav(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_fixedxynav(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_fixedznav(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_goodnav(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_invertnav(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_poornav(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_tie_fixed(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_tie_unfixed(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_tie_xy(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_tie_xyz(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_tie_z(Widget w, XtPointer client_data, XtPointer call_data);
void do_action_updategrids(Widget w, XtPointer client_data, XtPointer call_data);
void do_apply_nav(Widget w, XtPointer client_data, XtPointer call_data);
void do_biases_apply(Widget w, XtPointer client_data, XtPointer call_data);
void do_biases_applyall(Widget w, XtPointer client_data, XtPointer call_data);
void do_biases_heading(Widget w, XtPointer client_data, XtPointer call_data);
void do_biases_init(Widget w, XtPointer client_data, XtPointer call_data);
void do_biases_roll(Widget w, XtPointer client_data, XtPointer call_data);
void do_biases_toggle(Widget w, XtPointer client_data, XtPointer call_data);
void do_controls_apply(Widget w, XtPointer client_data, XtPointer call_data);
void do_controls_scale_colorinterval(Widget w, XtPointer client_data, XtPointer call_data);
void do_controls_show(Widget w, XtPointer client_data, XtPointer call_data);
void do_file_close(Widget w, XtPointer client_data, XtPointer call_data);
void do_file_importdata(Widget w, XtPointer client_data, XtPointer call_data);
void do_file_new(Widget w, XtPointer client_data, XtPointer call_data);
void do_file_open(Widget w, XtPointer client_data, XtPointer call_data);
void do_fileselection_cancel(Widget w, XtPointer client_data, XtPointer call_data);
void do_fileselection_mode(Widget w, XtPointer client_data, XtPointer call_data);
void do_fileselection_ok(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_data_select(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showbettercrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showblocks(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showcrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showcrossingties(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showcrossingtiessorted(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showcrossingtiessortedall(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showcrossingtiessortedbad(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showcrossingtiessortedworst(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showdata(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showglobalties(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showglobaltiessorted(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showgoodcrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showmediocrecrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showreferencegrids(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showsections(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showsurveys(Widget w, XtPointer client_data, XtPointer call_data);
void do_list_showtruecrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_clearblock(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_dismiss(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_expose(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_fullsize(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_perturbation(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_show(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_tieoffsets(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_timeseries(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_addtie(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_applyzoffset(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_cont_expose(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_corr_expose(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_deletetie(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_dismiss(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_fullsize(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_minmisfit(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_minxymisfit(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_misfitcenter(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_next(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_nextunset(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_previous(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_resettie(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_selecttie(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_setnone(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_setoffset(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_settie(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_unset(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_zerooffset(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_zerozoffset(Widget w, XtPointer client_data, XtPointer call_data);
void do_quit(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_contourinterval(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_controls_decimation(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_controls_sectionlength(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_controls_sectionsoundings(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_controls_smoothing(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_controls_tickinterval(Widget w, XtPointer client_data, XtPointer call_data);
void do_scale_controls_zoffset(Widget w, XtPointer client_data, XtPointer call_data);
void do_unsetskipped(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showallsurveys(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showbettercrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showblocks(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showcrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showdata(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showgoodcrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showmediocrecrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showsections(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showselectedblock(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showselectedfile(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showselectedsection(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showselectedsurvey(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showsurveys(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showtruecrossings(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showwithselectedfile(Widget w, XtPointer client_data, XtPointer call_data);
void do_view_showwithselectedsurvey(Widget w, XtPointer client_data, XtPointer call_data);
void do_visualize(Widget w, XtPointer client_data, XtPointer call_data);
void do_zerozoffsets(Widget w, XtPointer client_data, XtPointer call_data);

#define do_set_controls mbna_do_set_controls
void do_set_controls(void);
#define do_naverr_offsetlabel mbna_do_naverr_offsetlabel
void do_naverr_offsetlabel(void);

/* the file selection box's modes (mbnavadjust_creation.h), the client data of its menu entries */
#define MBNA_FILESELECTION_NONE 0
#define MBNA_FILESELECTION_NEW 1
#define MBNA_FILESELECTION_OPEN 2
#define MBNA_FILESELECTION_IMPORTDATA 3
#define MBNA_FILESELECTION_IMPORTSURVEY 4
#define MBNA_FILESELECTION_REFERENCE 5
/* a file chosen in the box: its format id from the name, into the format text field */
#define do_fileselection_list mbna_do_fileselection_list
void do_fileselection_list(Widget w, XtPointer client, XtPointer call);

/* ---- port entry points --------------------------------------------------------------------- */
/* do_mbnavadjust_init without argc/argv: the command line's -I, -D and -R */
#define do_mbnavadjust_init mbna_do_mbnavadjust_init
void do_mbnavadjust_init(const char *ifile, bool dark_background, bool reset_crossings);
/* the window's four drawing areas, as XG graphics ids: naverr contours, naverr misfit,
   naverr z offset, modelplot */
void mbna_window_canvases(void *cont, void *corr, void *zoff, void *modp);

/* the drawing areas' input: upstream's callbacks, the call_data an XmAnyCallbackStruct whose
   XEvent the window fills (mbna_motif.h) */
#define do_naverr_cont_input mbna_do_naverr_cont_input
#define do_naverr_corr_input mbna_do_naverr_corr_input
#define do_naverr_zcorr_input mbna_do_naverr_zcorr_input
#define do_modelplot_input mbna_do_modelplot_input
void do_naverr_cont_input(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_corr_input(Widget w, XtPointer client_data, XtPointer call_data);
void do_naverr_zcorr_input(Widget w, XtPointer client_data, XtPointer call_data);
void do_modelplot_input(Widget w, XtPointer client_data, XtPointer call_data);
/* port: the modelplot window's resize -- the shell's new size, width and height in pixels */
#define do_modelplot_resize mbna_do_modelplot_resize
void do_modelplot_resize(int width, int height);
/* mbview's action button "Add and open new crossing", and whether it can act now */
#define do_mbnavadjust_addcrossing mbna_do_mbnavadjust_addcrossing
void do_mbnavadjust_addcrossing(Widget w, XtPointer client, XtPointer call);
int mbna_view_can_addcrossing(void);
/* the visualization window's picks, by the names mbna_view_addnav / mbna_view_addroute gave */
void mbna_view_picked_route(const char *name);
void mbna_view_picked_navs(const char *name1, const char *name2);
/* the visualization window went away */
void mbna_view_closed(void);

#ifdef __cplusplus
}
#endif

#endif /* MBNAVADJUST_CALLBACKS_H_ */
