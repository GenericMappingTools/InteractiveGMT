/*======================================================================================
 * mbnavedit.h -- the interactive navigation editor engine (mbnavedit.c, a port of MB-System's
 * src/mbnavedit/mbnavedit_prog.c) and the host hooks it draws and talks through.
 *
 * mbnavedit.c is plain C, a translation unit of gmtvtk of its own (like mbedit.c and
 * mbvelocity.c), and holds NO Qt. The Qt window is mbnavedit_window.cpp, the port of
 * mbnavedit_callbacks.c; it implements the hooks declared at the bottom of this file (the
 * original's xg_* graphics and do_* dialog calls).
 *
 * The prototypes are MB-System's (mbnavedit.h, 5.8.3), with these changes:
 *   - The control globals mbnavedit.h shared between mbnavedit_prog.c and the Motif callbacks
 *     (output_mode, plot_tint, model_mode, ...) are the fields of ONE struct, mbnavedit_g, under
 *     their own names; the engine reaches them under the original names (mbnavedit.c maps them).
 *   - mbnavedit_init() takes no argc/argv: the command line's options are the window's
 *     (mbnaveditOpenWindow) and its File menu.
 *   - The swath files are read through the MBIO library the swath editor (deps/src/mbedit/)
 *     loads at run time; mbnavedit_mbio_open() picks the extra entry points this tool needs out of
 *     that same library.
 *====================================================================================*/

#ifndef MBNAVEDIT_H_
#define MBNAVEDIT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MB_PATH_MAXLINE / MB_PATHPLUS_MAXLINE of mb_define.h */
#define MBNAVEDIT_PATH_MAXLINE 1024
#define MBNAVEDIT_PATHPLUS_MAXLINE 1152

/* Mode value defines */
#define PICK_MODE_PICK 0
#define PICK_MODE_SELECT 1
#define PICK_MODE_DESELECT 2
#define PICK_MODE_SELECTALL 3
#define PICK_MODE_DESELECTALL 4
#define OUTPUT_MODE_OUTPUT 0
#define OUTPUT_MODE_BROWSE 1
#define PLOT_TINT 0
#define PLOT_LONGITUDE 1
#define PLOT_LATITUDE 2
#define PLOT_SPEED 3
#define PLOT_HEADING 4
#define PLOT_DRAFT 5
#define PLOT_ROLL 6
#define PLOT_PITCH 7
#define PLOT_HEAVE 8
#define MODEL_MODE_OFF 0
#define MODEL_MODE_MEAN 1
#define MODEL_MODE_DR 2
#define MODEL_MODE_INVERT 3
#define NUM_FILES_MAX 1000

/* mbnavedit global control parameters (mbnavedit.h's MBNAVEDIT_EXTERNAL globals) */
struct mbnavedit_globals {
	int output_mode;
	bool run_mbprocess;
	bool gui_mode;
	int data_show_max;
	int data_show_size;
	int data_step_max;
	int data_step_size;
	int mode_pick;
	int mode_set_interval;
	int plot_tint;
	int plot_tint_org;
	int plot_lon;
	int plot_lon_org;
	int plot_lon_dr;
	int plot_lat;
	int plot_lat_org;
	int plot_lat_dr;
	int plot_speed;
	int plot_speed_org;
	int plot_smg;
	int plot_heading;
	int plot_heading_org;
	int plot_cmg;
	int plot_draft;
	int plot_draft_org;
	int plot_draft_dr;
	int plot_roll;
	int plot_pitch;
	int plot_heave;
	int mean_time_window;
	int drift_lon;
	int drift_lat;
	bool timestamp_problem;
	bool use_ping_data;
	bool strip_comments;
	int format;
	char ifile[MBNAVEDIT_PATH_MAXLINE];
	char nfile[MBNAVEDIT_PATHPLUS_MAXLINE];
	int nfile_defined;
	int model_mode;
	double weight_speed;
	double weight_acceleration;
	int scrollcount;
	double offset_lon;
	double offset_lat;
	double offset_lon_applied;
	double offset_lat_applied;

	/* mbnavedit plot size parameters */
	int plot_width;
	int plot_height;
	int number_plots;
	int window_width;
	int window_height;
};
extern struct mbnavedit_globals mbnavedit_g;

/* ---- port additions ---------------------------------------------------------------- */
/* Pick the entry points this tool needs (mb_format_source, mb_coor_scale, mb_get_date,
 * mb_pr_update_nav) out of the MBIO library the swath editor has ALREADY loaded
 * (mbedit_mbio_open). Returns 1 when every one was found; otherwise 0 with the reason in `msg`. */
int mbnavedit_mbio_open(char *msg, int msglen);
/* free the record buffer (mbnavedit_init allocates it): the tool's memory goes with its window */
void mbnavedit_release(void);
/* for the host's state read-out: a file open, the records in the buffer, the first one shown, the
 * number shown, the records loaded and written for the file so far */
void mbnavedit_get_state(int *s_file_open, int *s_nbuff, int *s_current_id, int *s_nplot, int *s_nload_total,
                         int *s_ndump_total);
/* record i of the buffer: time, lon, lat, speed, heading, sensor depth, and which of the six plots
 * (time interval, lon, lat, speed, heading, sensor depth) have it selected (bits 0..5); 0 = no record */
int mbnavedit_get_record(int i, double *time_d, double *lon, double *lat, double *speed, double *heading, double *draft,
                         int *selected);
/* the plot box of plot iplot on the canvas (pixels) and its type (PLOT_*); 0 = no such plot */
int mbnavedit_get_plot_box(int iplot, int *ixmin, int *ixmax, int *iymin, int *iymax, int *type);
/* the pixel record i is drawn at in plot iplot; 0 = not shown */
int mbnavedit_get_record_xy(int iplot, int i, int *x, int *y);
/* the format of a file, guessed from its name as mb_get_format does */
int mbnavedit_get_format(char *file, int *form);

/* ---- the engine (mbnavedit_prog.c) --------------------------------------------------- */
int mbnavedit_init_globals(void);
int mbnavedit_init(void);
int mbnavedit_set_graphics(void *xgid, int ncol, unsigned int *pixels);
int mbnavedit_action_open(int useprevious);
int mbnavedit_open_file(int useprevious);
int mbnavedit_close_file(void);
int mbnavedit_dump_data(int hold);
int mbnavedit_load_data(void);
int mbnavedit_clear_screen(void);
int mbnavedit_action_next_buffer(int *quit);
int mbnavedit_action_offset(void);
int mbnavedit_action_close(void);
int mbnavedit_action_done(int *quit);
int mbnavedit_action_quit(void);
int mbnavedit_action_step(int step);
int mbnavedit_action_start(void);
int mbnavedit_action_end(void);
int mbnavedit_action_mouse_pick(int xx, int yy);
int mbnavedit_action_mouse_select(int xx, int yy);
int mbnavedit_action_mouse_deselect(int xx, int yy);
int mbnavedit_action_mouse_selectall(int xx, int yy);
int mbnavedit_action_mouse_deselectall(int xx, int yy);
int mbnavedit_action_deselect_all(int type);
int mbnavedit_action_set_interval(int xx, int yy, int which);
int mbnavedit_action_use_dr(void);
int mbnavedit_action_use_smg(void);
int mbnavedit_action_use_cmg(void);
int mbnavedit_action_interpolate(void);
int mbnavedit_action_interpolaterepeats(void);
int mbnavedit_action_revert(void);
int mbnavedit_action_flag(void);
int mbnavedit_action_unflag(void);
int mbnavedit_action_fixtime(void);
int mbnavedit_action_deletebadtime(void);
int mbnavedit_action_showall(void);
int mbnavedit_get_smgcmg(int i);
int mbnavedit_get_model(void);
int mbnavedit_get_gaussianmean(void);
int mbnavedit_get_dr(void);
int mbnavedit_get_inversion(void);
int mbnavedit_plot_all(void);
int mbnavedit_plot_tint(int iplot);
int mbnavedit_plot_lon(int iplot);
int mbnavedit_plot_lat(int iplot);
int mbnavedit_plot_speed(int iplot);
int mbnavedit_plot_heading(int iplot);
int mbnavedit_plot_draft(int iplot);
int mbnavedit_plot_roll(int iplot);
int mbnavedit_plot_pitch(int iplot);
int mbnavedit_plot_heave(int iplot);
int mbnavedit_plot_tint_value(int iplot, int iping);
int mbnavedit_plot_lon_value(int iplot, int iping);
int mbnavedit_plot_lat_value(int iplot, int iping);
int mbnavedit_plot_speed_value(int iplot, int iping);
int mbnavedit_plot_heading_value(int iplot, int iping);
int mbnavedit_plot_draft_value(int iplot, int iping);

/* ---- host hooks: implemented by the Qt window (mbnavedit_window.cpp) -------------------- */
/* The engine calls these under their MB-System names; the names are mapped onto mbnav_-prefixed
 * symbols so they can never bind to the other ported tools' own hooks (mbedit_xg_*, mbvt_xg_*,
 * do_* ...) in the same library, nor to a real MB-System libmbaux loaded in the same process. */
#define XG_SOLIDLINE 0
#define XG_DASHLINE 1
#define xg_drawline        mbnav_xg_drawline
#define xg_drawrectangle   mbnav_xg_drawrectangle
#define xg_fillrectangle   mbnav_xg_fillrectangle
#define xg_drawstring      mbnav_xg_drawstring
#define xg_justify         mbnav_xg_justify
#define do_message_on      mbnav_do_message_on
#define do_message_off     mbnav_do_message_off
#define do_error_dialog    mbnav_do_error_dialog
#define do_set_controls    mbnav_do_set_controls
#define do_filebutton_on   mbnav_do_filebutton_on
#define do_filebutton_off  mbnav_do_filebutton_off

void xg_drawline(void *xgid, int x1, int y1, int x2, int y2, unsigned int pixel, int style);
void xg_drawrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_fillrectangle(void *xgid, int x, int y, int width, int height, unsigned int pixel, int style);
void xg_drawstring(void *xgid, int x, int y, char *string, unsigned int pixel, int style);
void xg_justify(void *xgid, char *string, int *width, int *ascent, int *descent);
int do_message_on(char *message);
int do_message_off(void);
int do_error_dialog(char *s1, char *s2, char *s3);
void do_set_controls(void);
void do_filebutton_on(void);
void do_filebutton_off(void);

#ifdef __cplusplus
}
#endif

#endif /* MBNAVEDIT_H_ */
