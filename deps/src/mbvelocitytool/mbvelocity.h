/*======================================================================================
 * mbvelocity.h -- the water sound velocity profile editor engine (mbvelocity.c, a port of
 * MB-System's src/mbvelocitytool/mbvelocity_prog.c) and the host hooks it draws and talks through.
 *
 * mbvelocity.c is plain C, a translation unit of gmtvtk of its own (like mbgrid.c and mbedit.c),
 * and holds NO Qt. The Qt window is mbvelocity_window.cpp, the port of mbvelocity_callbacks.c; it
 * implements the hooks declared at the bottom of this file (the original's xg_* graphics and do_*
 * dialog calls).
 *
 * The prototypes are MB-System's (mbvelocity.h, 5.8.3), with these changes:
 *   - mbvt_init() takes no argc/argv: the options the command line set are the window's
 *     (mbvelocityOpenWindow) and its File menu.
 *   - The swath file and the profiles are read through the MBIO library the swath editor
 *     (deps/src/mbedit/) loads at run time; mbvt_mbio_open() picks the extra entry points this
 *     tool needs out of that same library.
 *====================================================================================*/

#ifndef MBVELOCITY_H_
#define MBVELOCITY_H_

#ifdef __cplusplus
extern "C" {
#endif

/* mbvelocitytool control defines */
#define MAX_PROFILES 100
#define PICK_DISTANCE 50
#define NUM_EDIT_START 6

/* ---- port additions ---------------------------------------------------------------- */
/* Pick the entry points this tool needs (mb_rt, mb_ttimes, mb_format_flags, the mbprocess
 * parameter calls, ...) out of the MBIO library the swath editor has ALREADY loaded
 * (mbedit_mbio_open). Returns 1 when every one was found; otherwise 0 with the reason in `msg`. */
int mbvt_mbio_open(char *msg, int msglen);
/* for the host's state read-out: the edit profile's node count, the display profiles, the pings
 * read, the number of beams with residuals */
void mbvt_get_state(int *s_edit, int *s_nedit, int *s_ndisplay, int *s_nbuffer, int *s_nbeams);
/* the edit profile's node i (depth, velocity); 0 when there is no such node */
int mbvt_get_edit_node(int i, double *depth, double *velocity);

/* ---- the engine (mbvelocity_prog.c) -------------------------------------------------- */
int mbvt_init(void);
int mbvt_quit(void);
int mbvt_set_graphics(void *xgid, int *brdr, int ncol, unsigned int *pixels);
int mbvt_get_values(int *s_edit, int *s_ndisplay, double *s_maxdepth, double *s_velrange, double *s_velcenter, double *s_resrange,
                    int *s_anglemode, int *s_format);
int mbvt_set_values(int s_edit, int s_ndisplay, double s_maxdepth, double s_velrange, double s_velcenter, double s_resrange,
                    int s_anglemode);
int mbvt_open_edit_profile(char *file);
int mbvt_new_edit_profile(void);
int mbvt_save_edit_profile(char *file);
int mbvt_save_swath_profile(char *file);
int mbvt_save_residuals(char *file);
int mbvt_open_display_profile(char *file);
int mbvt_get_display_names(int *nlist, char *list[MAX_PROFILES]);
int mbvt_delete_display_profile(int select);
int mbvt_plot(void);
int mbvt_action_select_node(int x, int y);
int mbvt_action_mouse_up(int x, int y);
int mbvt_action_drag_node(int x, int y);
int mbvt_action_add_node(int x, int y);
int mbvt_action_delete_node(int x, int y);
int mbvt_get_format(char *file, int *form);
int mbvt_open_swath_file(char *file, int form, int *numload);
int mbvt_deallocate_swath(void);
int mbvt_process_multibeam(void);

/* ---- host hooks: implemented by the Qt window (mbvelocity_window.cpp) -------------------- */
/* The engine calls these under their MB-System names; the names are mapped onto mbvt_-prefixed
 * symbols so they can never bind to the swath editor's own hooks (mbedit_xg_*, mbedit_do_*) in the
 * same library, nor to a real MB-System libmbaux loaded in the same process. */
#define XG_SOLIDLINE 0
#define XG_DASHLINE 1
#define xg_setclip         mbvt_xg_setclip
#define xg_drawline        mbvt_xg_drawline
#define xg_drawrectangle   mbvt_xg_drawrectangle
#define xg_fillrectangle   mbvt_xg_fillrectangle
#define xg_drawstring      mbvt_xg_drawstring
#define xg_justify         mbvt_xg_justify
#define do_message_on      mbvt_do_message_on
#define do_message_off     mbvt_do_message_off
#define do_error_dialog    mbvt_do_error_dialog
#define do_set_controls    mbvt_do_set_controls

void xg_setclip(void *xgid, int x, int y, int width, int height);
void xg_drawline(void *xgid, int x1, int y1, int x2, int y2, unsigned int pixel, int style);
void xg_drawrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_fillrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_drawstring(void *xgid, int x, int y, char *string, unsigned int pixel, int style);
void xg_justify(void *xgid, char *string, int *width, int *ascent, int *descent);
int do_message_on(char *message);
int do_message_off(void);
int do_error_dialog(char *s1, char *s2, char *s3);
void do_set_controls(void);

#ifdef __cplusplus
}
#endif

#endif /* MBVELOCITY_H_ */
