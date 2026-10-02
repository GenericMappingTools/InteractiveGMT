/*======================================================================================
 * mbedit.h -- the swath bathymetry editor engine (mbedit.c, a port of MB-System's
 * src/mbedit/mbedit_prog.c) and the host hooks it draws and talks through.
 *
 * mbedit.c is plain C, a second translation unit of gmtvtk (like mbgrid.c), and holds NO Qt.
 * The Qt window is src/mbedit/mbedit_window.cpp, the port of mbedit_callbacks.c; it implements the hooks
 * declared at the bottom of this file (the original's xg_* graphics and do_* dialog calls).
 *
 * The prototypes are MB-System's (mbedit.h, 5.8.3), with one change: mbedit_init() takes no
 * argc/argv, since the options the command line set are dialog controls here.
 *====================================================================================*/

#ifndef MBEDIT_H_
#define MBEDIT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- port additions ---------------------------------------------------------------- */
/* Load the MBIO library at `path` (UTF-8; a bare name is searched for by the system loader).
 * Returns 1 when it loaded, exports every function the editor needs and is MB-System 5.8.x;
 * otherwise 0 with the reason in `msg`. Loading again after a success is a no-op. */
int mbedit_mbio_open(const char *path, char *msg, int msglen);
int mbedit_mbio_loaded(void);
const char *mbedit_mbio_version(void);
/* do_parse_datalist (mbedit_callbacks.c): a single swath file, or (format -1) every file in a
 * datalist, handed to `add` one at a time. */
void mbedit_parse_datalist(char *file, int form, void (*add)(void *ctx, const char *path, int format), void *ctx);
/* do_build_filelist's lock query: 1 if another program holds a lock on this swath file. */
int mbedit_file_locked(char *file);
/* mbedit -X (--run-mbprocess): run mbprocess on a file once its edits are saved. */
void mbedit_set_run_mbprocess(int on);
/* soundings in the buffer that are flagged / unflagged (null beams in neither) */
void mbedit_count_flags(int *nflagged, int *nunflagged);

/* ---- the engine (mbedit_prog.c) ----------------------------------------------------- */
int mbedit_init(int *startup_file, int *startup_use_esf);
int mbedit_set_graphics(void *xgid, int ncol, unsigned int *pixels);
int mbedit_set_scaling(int *brdr, int sh_time);
int mbedit_set_filters(int f_m, int f_m_t, int f_m_x, int f_m_l, int f_w, int f_w_t, int f_b, int f_b_b, int f_b_e, int f_d,
                       double f_d_b, double f_d_e, int f_a, double f_a_b, double f_a_e);
int mbedit_get_filters(int *b_m, double *d_m, int *f_m, int *f_m_t, int *f_m_x, int *f_m_l, int *f_w, int *f_w_t, int *f_b,
                       int *f_b_b, int *f_b_e, int *f_d, double *f_d_b, double *f_d_e, int *f_a, double *f_a_b, double *f_a_e);
int mbedit_get_defaults(int *plt_size_max, int *plt_size, int *sh_mode, int *sh_flggdsdg, int *sh_flggdprf, int *sh_time, int *buffer_size_max,
                        int *buffer_size, int *hold_size, int *form, int *plwd, int *exgr, int *xntrvl, int *yntrvl, int *ttime_i,
                        int *outmode);
int mbedit_get_viewmode(int *vw_mode);
int mbedit_set_viewmode(int vw_mode);
int mbedit_action_open(char *file, int form, int fileid, int numfiles, int savemode, int outmode, int plwd, int exgr, int xntrvl,
                       int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time, int *buffer_size, int *buffer_size_max,
                       int *hold_size, int *ndumped, int *nloaded, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_next_buffer(int hold_size, int buffer_size, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size,
                              int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time, int *ndumped, int *nloaded, int *nbuffer, int *ngood,
                              int *icurrent, int *nplt, int *quit);
int mbedit_action_close(int buffer_size, int *ndumped, int *nloaded, int *nbuffer, int *ngood, int *icurrent);
int mbedit_action_done(int buffer_size, int *ndumped, int *nloaded, int *nbuffer, int *ngood, int *icurrent, int *quit);
int mbedit_action_quit(int buffer_size, int *ndumped, int *nloaded, int *nbuffer, int *ngood, int *icurrent);
int mbedit_action_step(int step, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf,
                       int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_plot(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                       int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_mouse_toggle(int x_loc, int y_loc, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode,
                               int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_mouse_pick(int x_loc, int y_loc, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode,
                             int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_mouse_erase(int x_loc, int y_loc, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode,
                              int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_mouse_restore(int x_loc, int y_loc, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode,
                                int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_mouse_grab(int grabmode, int x_loc, int y_loc, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size,
                             int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_mouse_info(int x_loc, int y_loc, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode,
                             int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_zap_outbounds(int iping, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf,
                                int sh_time, int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_bad_ping(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                           int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_good_ping(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                            int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_left_ping(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                            int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_right_ping(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                             int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_zero_ping(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                            int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_flag_view(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                            int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_unflag_view(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                              int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_unflag_all(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                             int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_action_filter_all(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time,
                             int *nbuffer, int *ngood, int *icurrent, int *nplt);
int mbedit_filter_ping(int iping);
int mbedit_get_format(char *file, int *form);
int mbedit_open_file(char *file, int form, bool savemode);
int mbedit_close_file(void);
int mbedit_dump_data(int hold_size, int *ndumped, int *nbuffer);
int mbedit_load_data(int buffer_size, int *nloaded, int *nbuffer, int *ngood, int *icurrent);
int mbedit_clear_screen(void);
int mbedit_plot_all(int plwd, int exgr, int xntrvl, int yntrvl, int plt_size, int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time, int *nplt,
                    bool autoscale);
int mbedit_plot_beam(int iping, int jbeam);
int mbedit_plot_ping(int iping);
int mbedit_plot_ping_label(int iping, bool save);
int mbedit_plot_info(void);
int mbedit_unplot_beam(int iping, int jbeam);
int mbedit_unplot_ping(int iping);
int mbedit_unplot_info(void);
int mbedit_action_goto(int ttime_i[7], int hold_size, int buffer_size, int plwd, int exgr, int xntrvl, int yntrvl, int plt_size,
                       int sh_mode, int sh_flggdsdg, int sh_flggdprf, int sh_time, int *ndumped, int *nloaded, int *nbuffer, int *ngood,
                       int *icurrent, int *nplt);
int mbedit_tslabel(int data_id, char *label);
int mbedit_tsvalue(int iping, int data_id, double *value);
int mbedit_tsminmax(int iping, int nping, int data_id, double *tsmin, double *tsmax);
int mbedit_xtrackslope(int iping, double *slope);

/* ---- host hooks: implemented by the Qt window (src/mbedit/mbedit_window.cpp) ------------------------ */
/* The engine calls these under their MB-System names; the names are mapped onto mbedit_-prefixed
 * symbols so they can never bind to a real MB-System libmbaux loaded in the same process. */
#define XG_SOLIDLINE 0
#define XG_DASHLINE 1
#define xg_drawline        mbedit_xg_drawline
#define xg_drawrectangle   mbedit_xg_drawrectangle
#define xg_fillrectangle   mbedit_xg_fillrectangle
#define xg_drawstring      mbedit_xg_drawstring
#define xg_justify         mbedit_xg_justify
#define do_message_on      mbedit_do_message_on
#define do_message_off     mbedit_do_message_off
#define do_error_dialog    mbedit_do_error_dialog
#define do_filebutton_on   mbedit_do_filebutton_on
#define do_filebutton_off  mbedit_do_filebutton_off
#define do_nextbutton_on   mbedit_do_nextbutton_on
#define do_nextbutton_off  mbedit_do_nextbutton_off
#define do_reset_scale_x   mbedit_do_reset_scale_x

void xg_drawline(void *xgid, int x1, int y1, int x2, int y2, unsigned int pixel, int style);
void xg_drawrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_fillrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_drawstring(void *xgid, int x, int y, char *string, unsigned int pixel, int style);
void xg_justify(void *xgid, char *string, int *width, int *ascent, int *descent);
int do_message_on(char *message);
int do_message_off(void);
int do_error_dialog(char *s1, char *s2, char *s3);
void do_filebutton_on(void);
void do_filebutton_off(void);
void do_nextbutton_on(void);
void do_nextbutton_off(void);
int do_reset_scale_x(int pwidth, int maxx, int xntrvl, int yntrvl);

#ifdef __cplusplus
}
#endif

#endif /* MBEDIT_H_ */
