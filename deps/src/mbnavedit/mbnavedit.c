/*--------------------------------------------------------------------
 *    The MB-system:	mbnavedit_prog.c	6/23/95
 *
 *    Copyright (c) 1995-2025 by
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
 * MBNAVEDIT is an interactive navigation editor for swath sonar data.
 * It can work with any data format supported by the MBIO library.
 * This file contains the code that does not directly depend on the
 * MOTIF interface - the companion files mbnavedit.c and
 * mbnavedit_callbacks.c contain the user interface related code.
 *
 * Author:	D. W. Caress
 * Date:	June 23,  1995
 * Date:	August 28, 2000 (New version - no buffered i/o)
 */

/*--------------------------------------------------------------------
 * InteractiveGMT port (deps/src/mbnavedit/mbnavedit.c)
 *
 * MB-System 5.8.3's src/mbnavedit/mbnavedit_prog.c, the engine of mbnavedit, carried over line
 * for line: the buffer, the reading and the .nve writing, the picking and selecting, the
 * interpolation, the navigation models (Gaussian mean, dead reckoning, smooth inversion), the
 * time stamp repairs and the drawing. What changed, and only that:
 *   - No X11, no getopt: the drawing goes through the host's xg_* hooks (mbnavedit_window.cpp
 *     paints them into a QImage) and the command-line options are the window's, so
 *     mbnavedit_init() keeps the defaults and drops the argument parsing.
 *   - The globals mbnavedit.h shared with the Motif callbacks are the fields of mbnavedit_g; this
 *     file reaches them under their original names (the #defines below).
 *   - MBIO is not linked. The swath editor (deps/src/mbedit/) loads it at run time and fills the
 *     table of mbedit_mbio.h; this file calls that table under the original names, and picks the
 *     entry points only this tool needs out of the SAME library (mbnavedit_mbio_open below).
 *   - The smooth inversion's sparse least squares solver (lsqup, lspeig and their helpers) is
 *     libmbaux's (mbaux/mb_cheb.c), which the MBIO library does not carry: it is carried in
 *     below, unchanged, as static functions.
 *   - The record buffer (1,000,000 records, ~440 MB as a static array) is allocated on the heap
 *     at mbnavedit_init() instead of sitting in the library's image.
 *   - "Use previously edited nav" copied the .nve with `cp` through the shell: it is a plain
 *     file copy here (the shell has no cp on Windows).
 *   - exit() is gone: this code runs inside the host process, so a failure reports and returns,
 *     and the busy notice is taken down on the failures that returned with it still up.
 *   - Indices upstream could leave unset (mbnavedit_action_step's new_id, the repeats
 *     interpolation's iafter) are initialised: a garbage index here takes the host down with it.
 *     Each is marked "port:".
 *   - mb_mallocd / mb_freed / mb_memory_list (MBIO's allocation book-keeping) are plain
 *     calloc / free.
 *   - File-scope globals are static, so this translation unit exports nothing but the
 *     mbnavedit_* functions and mbnavedit_g.
 *--------------------------------------------------------------------*/

// TODO(schwehr): useprevious int boolean -> bool

/* Dl_info / dladdr are GNU extensions: _GNU_SOURCE must precede the FIRST system header */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#	define _GNU_SOURCE
#endif

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#	include <io.h>
#	define access _access
#	ifndef R_OK
#		define R_OK 4
#	endif
#else
#	include <dlfcn.h>
#	include <unistd.h>
#endif

#include "../mbedit/mbedit_mbio.h"
#include "mbnavedit.h"

/* the swath editor's MBIO loader (deps/src/mbedit/mbedit.c) */
int mbedit_mbio_loaded(void);
const char *mbedit_mbio_version(void);
#define MB_VERSION (mbedit_mbio_version())

#ifndef S_IFMT
#define S_IFMT _S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif

/* ---- mb_define.h / mb_format.h / mb_process.h ------------------------------------------- */
#define MB_PATHPLUS_MAXLINE 1152
#define MB_PATHPLUSPLUS_MAXLINE 2304
#define MB_COMMAND_LENGTH 8192
typedef char mb_pathplusplus[MB_PATHPLUSPLUS_MAXLINE];
typedef char mb_command[MB_COMMAND_LENGTH];
#define MBF_MBPRONAV 166
#define MBP_NAV_ON 1
#define MBP_NAV_LINEAR 0
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DTR (M_PI / 180.)

/* ---- the entry points this tool adds to the swath editor's table ---------------------- */
struct mbnavedit_mbio_table {
	int (*format_source)(int verbose, int *format, int *platform_source, int *nav_source, int *sensordepth_source,
	                     int *heading_source, int *attitude_source, int *svp_source, int *error);
	int (*coor_scale)(int verbose, double latitude, double *mtodeglon, double *mtodeglat);
	int (*get_date)(int verbose, double time_d, int time_i[7]);
	int (*pr_update_nav)(int verbose, char *file, int mbp_nav_mode, char *mbp_navfile, int mbp_nav_format,
	                     int mbp_nav_heading, int mbp_nav_speed, int mbp_nav_draft, int mbp_nav_attitude,
	                     int mbp_nav_algorithm, double mbp_nav_timeshift, int *error);
};
static struct mbnavedit_mbio_table mbnavedit_mbio;
static bool mbnavedit_mbio_ok = false;

static int mbnavedit_mallocd(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
static int mbnavedit_freed(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error);
static void mb_file_open_error_message(const char *path, char *line1, char *line2, char *line3, size_t linesize);
static int mbnavedit_copy_file(const char *from, const char *to);
static void lsqup(const double *a, const int *ia, const int *nia, int nnz, int nc, int nr, double *x, double *dx,
                  const double *d, int nfix, const int *ifix, const double *fix, int ncycle, const double *sigma);
static void lspeig(const double *a, const int *ia, const int *nia, int nnz, int nc, int nr, int ncyc, int *nsig, double *x,
                   double *dx, double *sigma, double *w, double *smax, double *err, double *sup);
static void chebyu(double *sigma, int ncycle, double shi, double slo, double *work);
static void splits(double *x, double *t, int n);
static double errlim(double *sigma, int ncycle, double shi, double slo);
static double errrat(double x1, double x2, double *sigma, int ncycle);

#define mb_format_source      mbnavedit_mbio.format_source
#define mb_coor_scale         mbnavedit_mbio.coor_scale
#define mb_get_date           mbnavedit_mbio.get_date
#define mb_pr_update_nav      mbnavedit_mbio.pr_update_nav
#define mb_mallocd            mbnavedit_mallocd
#define mb_freed              mbnavedit_freed

/* ---- mbnavedit.h's globals, under their own names ----------------------------------------- */
struct mbnavedit_globals mbnavedit_g;
#define output_mode         mbnavedit_g.output_mode
#define run_mbprocess       mbnavedit_g.run_mbprocess
#define gui_mode            mbnavedit_g.gui_mode
#define data_show_max       mbnavedit_g.data_show_max
#define data_show_size      mbnavedit_g.data_show_size
#define data_step_max       mbnavedit_g.data_step_max
#define data_step_size      mbnavedit_g.data_step_size
#define mode_pick           mbnavedit_g.mode_pick
#define mode_set_interval   mbnavedit_g.mode_set_interval
#define plot_tint           mbnavedit_g.plot_tint
#define plot_tint_org       mbnavedit_g.plot_tint_org
#define plot_lon            mbnavedit_g.plot_lon
#define plot_lon_org        mbnavedit_g.plot_lon_org
#define plot_lon_dr         mbnavedit_g.plot_lon_dr
#define plot_lat            mbnavedit_g.plot_lat
#define plot_lat_org        mbnavedit_g.plot_lat_org
#define plot_lat_dr         mbnavedit_g.plot_lat_dr
#define plot_speed          mbnavedit_g.plot_speed
#define plot_speed_org      mbnavedit_g.plot_speed_org
#define plot_smg            mbnavedit_g.plot_smg
#define plot_heading        mbnavedit_g.plot_heading
#define plot_heading_org    mbnavedit_g.plot_heading_org
#define plot_cmg            mbnavedit_g.plot_cmg
#define plot_draft          mbnavedit_g.plot_draft
#define plot_draft_org      mbnavedit_g.plot_draft_org
#define plot_draft_dr       mbnavedit_g.plot_draft_dr
#define plot_roll           mbnavedit_g.plot_roll
#define plot_pitch          mbnavedit_g.plot_pitch
#define plot_heave          mbnavedit_g.plot_heave
#define mean_time_window    mbnavedit_g.mean_time_window
#define drift_lon           mbnavedit_g.drift_lon
#define drift_lat           mbnavedit_g.drift_lat
#define timestamp_problem   mbnavedit_g.timestamp_problem
#define use_ping_data       mbnavedit_g.use_ping_data
#define strip_comments      mbnavedit_g.strip_comments
#define format              mbnavedit_g.format
#define ifile               mbnavedit_g.ifile
#define nfile               mbnavedit_g.nfile
#define nfile_defined       mbnavedit_g.nfile_defined
#define model_mode          mbnavedit_g.model_mode
#define weight_speed        mbnavedit_g.weight_speed
#define weight_acceleration mbnavedit_g.weight_acceleration
#define scrollcount         mbnavedit_g.scrollcount
#define offset_lon          mbnavedit_g.offset_lon
#define offset_lat          mbnavedit_g.offset_lat
#define offset_lon_applied  mbnavedit_g.offset_lon_applied
#define offset_lat_applied  mbnavedit_g.offset_lat_applied
#define plot_width          mbnavedit_g.plot_width
#define plot_height         mbnavedit_g.plot_height
#define number_plots        mbnavedit_g.number_plots
#define window_width        mbnavedit_g.window_width
#define window_height       mbnavedit_g.window_height

struct mbnavedit_ping_struct {
	int id;
	int record;
	int time_i[7];
	double time_d;
	double file_time_d;
	double tint;
	double lon;
	double lat;
	double speed;
	double heading;
	double draft;
	double roll;
	double pitch;
	double heave;
	double time_d_org;
	double tint_org;
	double lon_org;
	double lat_org;
	int mean_ok;
	double lon_dr;
	double lat_dr;
	double speed_org;
	double heading_org;
	double draft_org;
	double speed_made_good;
	double course_made_good;
	int tint_x;
	int tint_y;
	int lon_x;
	int lon_y;
	int lat_x;
	int lat_y;
	int speed_x;
	int speed_y;
	int heading_x;
	int heading_y;
	int draft_x;
	int draft_y;
	int tint_select;
	int lon_select;
	int lat_select;
	int speed_select;
	int heading_select;
	int draft_select;
	int lonlat_flag;
};

/* plot structure definition */
struct mbnavedit_plot_struct {
	int type;
	int ixmin;
	int ixmax;
	int iymin;
	int iymax;
	double xmin;
	double xmax;
	double ymin;
	double ymax;
	double xscale;
	double yscale;
	double xinterval;
	double yinterval;
	char xlabel[128];
	char ylabel1[128];
	char ylabel2[128];
};

/* id variables */
static char program_name[] = "MBNAVEDIT";

/* status variables */
static int error = MB_ERROR_NO_ERROR;
static int verbose = 0;
static char *message = NULL;

/* MBIO control parameters */
static int platform_source;
static int nav_source;
static int sensordepth_source;
static int heading_source;
static int attitude_source;
static int svp_source;
static int pings;
static int lonflip;
static double bounds[4];
static int btime_i[7];
static int etime_i[7];
static double btime_d;
static double etime_d;
static double speedmin;
static double timegap;
static int beams_bath;
static int beams_amp;
static int pixels_ss;
static void *imbio_ptr = NULL;
static bool uselockfiles = true;

/* mbio read and write values */
static void *store_ptr = NULL;
static int kind;
static double distance;
static double altitude;
static double sensordepth;
static int nbath;
static int namp;
static int nss;
static char *beamflag = NULL;
static double *bath = NULL;
static double *bathacrosstrack = NULL;
static double *bathalongtrack = NULL;
static double *amp = NULL;
static double *ss = NULL;
static double *ssacrosstrack = NULL;
static double *ssalongtrack = NULL;
static char comment[MB_COMMENT_MAXLINE];

/* buffer control variables */
#define MBNAVEDIT_BUFFER_SIZE 1000000
static bool file_open = false;
static bool nfile_open = false;
static FILE *nfp;
static int hold_size = 100;
static int nload = 0;
static int ndump = 0;
static int nbuff = 0;
static int current_id = 0;
static int nload_total = 0;
static int ndump_total = 0;
static int first_read = false;

/* plotting control variables */
#define NUMBER_PLOTS_MAX 9
#define DEFAULT_PLOT_WIDTH 767
#define DEFAULT_PLOT_HEIGHT 300
#define MBNAVEDIT_PICK_DISTANCE 50
#define MBNAVEDIT_ERASE_DISTANCE 10
static struct mbnavedit_ping_struct *ping = NULL;   /* MBNAVEDIT_BUFFER_SIZE records, mbnavedit_init */
static double plot_start_time;
static double plot_end_time;
static int nplot;
static void *mbnavedit_xgid;
static struct mbnavedit_plot_struct mbnavplot[NUMBER_PLOTS_MAX];
static int data_save;
static double file_start_time_d;

/* color control values */
static int ncolors;
static int pixel_values[256];

/*--------------------------------------------------------------------*/
int mbnavedit_init_globals() {
	/* set default global control parameters */
	output_mode = OUTPUT_MODE_OUTPUT;
	run_mbprocess = false;
	gui_mode = false;
	data_show_max = 2000;
	data_show_size = 1000;
	data_step_max = 2000;
	data_step_size = 750;
	mode_pick = PICK_MODE_PICK;
	mode_set_interval = false;
	plot_tint = true;
	plot_tint_org = true;
	plot_lon = true;
	plot_lon_org = true;
	plot_lon_dr = false;
	plot_lat = true;
	plot_lat_org = true;
	plot_lat_dr = false;
	plot_speed = true;
	plot_speed_org = true;
	plot_smg = true;
	plot_heading = true;
	plot_heading_org = true;
	plot_cmg = true;
	plot_draft = true;
	plot_draft_org = true;
	plot_draft_dr = false;
	plot_roll = false;
	plot_pitch = false;
	plot_heave = false;
	mean_time_window = 100;
	drift_lon = 0;
	drift_lat = 0;
	strcpy(ifile, "");
	plot_width = DEFAULT_PLOT_WIDTH;
	plot_height = DEFAULT_PLOT_HEIGHT;
	number_plots = 0;
	if (plot_tint)
		number_plots++;
	if (plot_lon)
		number_plots++;
	if (plot_lat)
		number_plots++;
	if (plot_speed)
		number_plots++;
	if (plot_heading)
		number_plots++;
	if (plot_draft)
		number_plots++;
	timestamp_problem = false;
	use_ping_data = false;
	strip_comments = false;
	model_mode = MODEL_MODE_OFF;
	weight_speed = 100.0;
	weight_acceleration = 100.0;
	scrollcount = 0;
	offset_lon = 0.0;
	offset_lat = 0.0;
	offset_lon_applied = 0.0;
	offset_lat_applied = 0.0;

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}

/*--------------------------------------------------------------------*/
int mbnavedit_init(void) {
	int status = mb_defaults(verbose, &format, &pings, &lonflip, bounds, btime_i, etime_i, &speedmin, &timegap);
	status = mb_uselockfiles(verbose, &uselockfiles);
	pings = 1;
	lonflip = 0;
	bounds[0] = -360.;
	bounds[1] = 360.;
	bounds[2] = -90.;
	bounds[3] = 90.;
	btime_i[0] = 1962;
	btime_i[1] = 2;
	btime_i[2] = 21;
	btime_i[3] = 10;
	btime_i[4] = 30;
	btime_i[5] = 0;
	btime_i[6] = 0;
	etime_i[0] = 2062;
	etime_i[1] = 2;
	etime_i[2] = 21;
	etime_i[3] = 10;
	etime_i[4] = 30;
	etime_i[5] = 0;
	etime_i[6] = 0;
	speedmin = 0.0;
	timegap = 1000000000.0;
	strcpy(ifile, "");

	/* the record buffer (a static array of MBNAVEDIT_BUFFER_SIZE records in mbnavedit_prog.c) */
	if (ping == NULL)
		ping = (struct mbnavedit_ping_struct *)calloc(MBNAVEDIT_BUFFER_SIZE, sizeof(struct mbnavedit_ping_struct));
	if (ping == NULL) {
		error = MB_ERROR_MEMORY_FAIL;
		status = MB_FAILURE;
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}

/*--------------------------------------------------------------------*/
int mbnavedit_set_graphics(void *xgid, int ncol, unsigned int *pixels) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xgid:         %p\n", xgid);
		fprintf(stderr, "dbg2       ncolors:      %d\n", ncol);
		for (int i = 0; i < ncol; i++)
			fprintf(stderr, "dbg2       pixel[%d]:     %d\n", i, pixels[i]);
	}

	/* set graphics id */
	mbnavedit_xgid = xgid;

	/* set colors */
	ncolors = ncol;
	for (int i = 0; i < ncolors; i++)
		pixel_values[i] = pixels[i];

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_open(int useprevious) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* clear the screen */
	int status = mbnavedit_clear_screen();

	/* open the file */
	status = mbnavedit_open_file(useprevious);

	/* load the buffer */
	if (status == MB_SUCCESS)
		status = mbnavedit_load_data();

	/* set up plotting */
	if (nbuff > 0) {
		/* set time span to zero so plotting resets it */
		data_show_size = 0;

		/* turn file button off */
		do_filebutton_off();

		/* now plot it */
		status = mbnavedit_plot_all();
	}

	/* if no data read show error dialog */
	else
		do_error_dialog("No data were read from the input", "file. You may have specified an", "incorrect MB-System format id!");

	/* reset data_save */
	data_save = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  File open attempted in MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Buffer values:\n");
		fprintf(stderr, "dbg2       nload:       %d\n", ndump);
		fprintf(stderr, "dbg2       nload:       %d\n", nload);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       current_id:  %d\n", current_id);
		fprintf(stderr, "dbg2       error:       %d\n", error);
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_open_file(int useprevious) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       file:        %s\n", ifile);
		fprintf(stderr, "dbg2       format:      %d\n", format);
		fprintf(stderr, "dbg2       useprevious: %d\n", useprevious);
	}

	mb_pathplusplus ifile_use;
	mb_command command;
	int format_use;
	int form;
	int format_error;
	struct stat file_status;
	int fstat;
	mb_path error1 = "";
	char error2[2100] = "";
	char error3[2100] = "";

	/* swath file locking variables */
	bool locked = false;
	int lock_purpose = MBP_LOCK_NONE;
	mb_path lock_program;
	mb_path lock_cpu;
	mb_path lock_user;
	char lock_date[25];

	/* reset message */
	do_message_on("MBedit is opening a data file...");

	/* get format if required */
	if (format == 0) {
		if (mb_get_format(verbose, ifile, NULL, &form, &format_error) == MB_SUCCESS) {
			format = form;
		}
	}

	/* get the output filename */
	strcpy(nfile, ifile);
	strcat(nfile, ".nve");

	int status = MB_SUCCESS;

	/* try to lock file */
	if (output_mode == OUTPUT_MODE_OUTPUT && uselockfiles) {
		status = mb_pr_lockswathfile(verbose, ifile, MBP_LOCK_EDITNAV, program_name, &error);
	}
	else {
		// const int lock_status =
		    mb_pr_lockinfo(verbose, ifile, &locked, &lock_purpose, lock_program, lock_user, lock_cpu, lock_date, &error);

		/* if locked get lock info */
		if (error == MB_ERROR_FILE_LOCKED) {
			fprintf(stderr, "\nFile %s locked but lock ignored\n", ifile);
			fprintf(stderr, "File locked by <%s> running <%s>\n", lock_user, lock_program);
			fprintf(stderr, "on cpu <%s> at <%s>\n", lock_cpu, lock_date);
			error = MB_ERROR_NO_ERROR;
		}
	}

	/* if locked let the user know file can't be opened */
	if (status == MB_FAILURE) {
		/* turn off message */
		do_message_off();

		/* if locked get lock info */
		if (error == MB_ERROR_FILE_LOCKED) {
			// const int lock_status =
			    mb_pr_lockinfo(verbose, ifile, &locked, &lock_purpose, lock_program, lock_user, lock_cpu, lock_date, &error);

			sprintf(error1, "Unable to open input file:");
			sprintf(error2, "File locked by <%s> running <%s>", lock_user, lock_program);
			sprintf(error3, "on cpu <%s> at <%s>", lock_cpu, lock_date);
			fprintf(stderr, "\nUnable to open input file:\n");
			fprintf(stderr, "  %s\n", ifile);
			fprintf(stderr, "File locked by <%s> running <%s>\n", lock_user, lock_program);
			fprintf(stderr, "on cpu <%s> at <%s>\n", lock_cpu, lock_date);
		}

		/* else if unable to create lock file there is a permissions problem */
		else if (error == MB_ERROR_OPEN_FAIL) {
			sprintf(error1, "Unable to create lock file");
			sprintf(error2, "for intended input file:");
			sprintf(error3, "-Likely permissions issue");
			fprintf(stderr, "Unable to create lock file\n");
			fprintf(stderr, "for intended input file:\n");
			fprintf(stderr, "  %s\n", ifile);
			fprintf(stderr, "-Likely permissions issue\n");
		}

		/* put up error dialog */
		do_error_dialog(error1, error2, error3);
	}

	/* if successfully locked (or lock ignored) proceed */
	if (status == MB_SUCCESS) {
		/* if output on and using previously edited nav first copy old nav
		    and then read it as input instead of specified
		    input file */
		if (useprevious && output_mode != OUTPUT_MODE_BROWSE) {
			/* get temporary file name */
			snprintf(ifile_use, sizeof(ifile_use), "%s.tmp", nfile);

			/* copy old edit save file to tmp file */
			(void)command;
			format_use = MBF_MBPRONAV;
			mbnavedit_copy_file(nfile, ifile_use);
			fstat = stat(ifile_use, &file_status);
			if (fstat != 0 || (file_status.st_mode & S_IFMT) == S_IFDIR) {
				do_message_off();   /* port: the busy notice must not outlive the failure */
				do_error_dialog("Unable to copy previously edited", "navigation. You may not have read",
				                "permission in this directory!");
				status = MB_FAILURE;
				return (status);
			}
		}

		/* if output off and using previously edited nav
		    reset input names */
		else if (useprevious) {
			snprintf(ifile_use, sizeof(ifile_use), "%s", nfile);
			format_use = MBF_MBPRONAV;
		}

		/* else just read from previously edited nav */
		else {
			strcpy(ifile_use, ifile);
			format_use = format;
		}

		/* initialize reading the input multibeam file */
		status = mb_format_source(verbose, &format_use, &platform_source, &nav_source, &sensordepth_source, &heading_source,
		                          &attitude_source, &svp_source, &error);
		if ((status = mb_read_init(verbose, ifile_use, format_use, pings, lonflip, bounds, btime_i, etime_i, speedmin, timegap,
		                           &imbio_ptr, &btime_d, &etime_d, &beams_bath, &beams_amp, &pixels_ss, &error)) != MB_SUCCESS) {
			mb_error(verbose, error, &message);
			fprintf(stderr, "\nMBIO Error returned from function <mb_read_init>:\n%s\n", message);
			fprintf(stderr, "\nMultibeam File <%s> not initialized for reading\n", ifile);
			status = MB_FAILURE;
			do_message_off();   /* port: the busy notice must not outlive the failure */
			mb_file_open_error_message(ifile_use, error1, error2, error3, sizeof(error1));
			do_error_dialog(error1, error2, error3);
			return (status);
		}

		/* allocate memory for data arrays */
		beamflag = NULL;
		bath = NULL;
		amp = NULL;
		bathacrosstrack = NULL;
		bathalongtrack = NULL;
		ss = NULL;
		ssacrosstrack = NULL;
		ssalongtrack = NULL;
		if (error == MB_ERROR_NO_ERROR)
			status = mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(char), (void **)&beamflag, &error);
		if (error == MB_ERROR_NO_ERROR)
			status = mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bath, &error);
		if (error == MB_ERROR_NO_ERROR)
			status = mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_AMPLITUDE, sizeof(double), (void **)&amp, &error);
		if (error == MB_ERROR_NO_ERROR)
			status =
			    mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathacrosstrack, &error);
		if (error == MB_ERROR_NO_ERROR)
			status =
			    mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathalongtrack, &error);
		if (error == MB_ERROR_NO_ERROR)
			status = mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ss, &error);
		if (error == MB_ERROR_NO_ERROR)
			status = mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ssacrosstrack, &error);
		if (error == MB_ERROR_NO_ERROR)
			status = mb_register_array(verbose, imbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ssalongtrack, &error);

		/* if error initializing memory then quit (here: report it and give the file up) */
		if (error != MB_ERROR_NO_ERROR) {
			mb_error(verbose, error, &message);
			fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
			mb_close(verbose, &imbio_ptr, &error);
			do_message_off();
			do_error_dialog("MBIO Error allocating data arrays:", message, "");
			return (MB_FAILURE);
		}

		/* initialize the buffer */
		nbuff = 0;
		first_read = false;

		/* reset plotting time span */
		plot_start_time = 0.0;
		plot_end_time = data_show_size;

		/* now deal with new nav save file */
		nfile_open = false;
		if (status == MB_SUCCESS && output_mode != OUTPUT_MODE_BROWSE) {
			/* get nav edit save file */
      snprintf(nfile, sizeof(nfile), "%s.nve", ifile);

			/* open the nav edit save file */
			if ((nfp = fopen(nfile, "w")) != NULL) {
				nfile_open = true;
			}
			else {
				nfile_open = false;
				fprintf(stderr, "\nUnable to open new nav save file %s\n", nfile);
				do_error_dialog("Unable to open new nav edit save file.", "You may not have write",
				                "permission in this directory!");
			}
		}

		/* if we got here we must have succeeded */
		if (verbose >= 1) {
			if (useprevious) {
				fprintf(stderr, "\nSwath data file <%s> specified for input\n", ifile);
				fprintf(stderr, "MB-System Data Format ID: %d\n", format);
				fprintf(stderr, "Navigation data file <%s> initialized for reading\n", ifile_use);
				fprintf(stderr, "MB-System Data Format ID: %d\n", format_use);
			}
			else {
				fprintf(stderr, "\nSwath data file <%s> initialized for reading\n", ifile_use);
				fprintf(stderr, "MB-System Data Format ID: %d\n", format_use);
			}
			if (output_mode == OUTPUT_MODE_OUTPUT)
				fprintf(stderr, "Navigation File <%s> initialized for writing\n", nfile);
		}
		file_open = true;
	}

	/* turn off message */
	do_message_off();

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:     %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_close_file() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* reset message */
	do_message_on("MBedit is closing a data file...");

	/* close the files */
	int status = mb_close(verbose, &imbio_ptr, &error);
	if (nfile_open) {
		/* close navigation file */
		fclose(nfp);
		nfile_open = false;
	}

	/* if not in browse mode, deal with locking and processing */
	if (output_mode == OUTPUT_MODE_OUTPUT) {

		/* unlock the raw swath file */
		if (uselockfiles)
			status = mb_pr_unlockswathfile(verbose, ifile, MBP_LOCK_EDITNAV, program_name, &error);

		/* update mbprocess parameter file */
		status = mb_pr_update_format(verbose, ifile, true, format, &error);
		status = mb_pr_update_nav(verbose, ifile, MBP_NAV_ON, nfile, 9, MBP_NAV_ON, MBP_NAV_ON, MBP_NAV_ON, MBP_NAV_ON,
		                          MBP_NAV_LINEAR, (double)0.0, &error);

		/* run mbprocess if desired */
		if (run_mbprocess) {
			/* turn message on */
			do_message_on("Navigation edits being applied using mbprocess...");

			/* run mbprocess */
			char command[MB_PATH_MAXLINE+100];
			if (strip_comments)
				sprintf(command, "mbprocess -I %s -N\n", ifile);
			else
				sprintf(command, "mbprocess -I %s\n", ifile);
			if (verbose >= 1)
				fprintf(stderr, "\nExecuting command:\n%s\n", command);
			/* const int shellstatus = */ system(command);

			/* turn message off */
			do_message_off();
		}
	}

	/* check memory */
	if (verbose >= 4)
		status = mb_memory_list(verbose, &error);

	/* if we got here we must have succeeded */
	if (verbose >= 1) {
		fprintf(stderr, "\nMultibeam Input File <%s> closed\n", ifile);
		if (output_mode == OUTPUT_MODE_OUTPUT)
			fprintf(stderr, "Navigation Output File <%s> closed\n", nfile);
		fprintf(stderr, "%d data records loaded\n", nload_total);
		fprintf(stderr, "%d data records dumped\n", ndump_total);
	}
	file_open = false;
	nload_total = 0;
	ndump_total = 0;

	/* reset offsets */
	offset_lon = 0.0;
	offset_lat = 0.0;
	offset_lon_applied = offset_lon;
	offset_lat_applied = offset_lat;

	/* turn file button on */
	do_filebutton_on();

	/* turn off message */
	do_message_off();

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_dump_data(int hold) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       hold:       %d\n", hold);
	}

	/* write out edited data */
	if (nfile_open) {
		for (int iping = 0; iping < nbuff - hold; iping++) {
			/* write the nav out */
			fprintf(nfp, "%4.4d %2.2d %2.2d %2.2d %2.2d %2.2d.%6.6d %16.6f %.10f %.10f %.3f %.3f %.4f %.3f %.3f %.4f\r\n",
			        ping[iping].time_i[0], ping[iping].time_i[1], ping[iping].time_i[2], ping[iping].time_i[3],
			        ping[iping].time_i[4], ping[iping].time_i[5], ping[iping].time_i[6], ping[iping].time_d, ping[iping].lon,
			        ping[iping].lat, ping[iping].heading, ping[iping].speed, ping[iping].draft, ping[iping].roll,
			        ping[iping].pitch, ping[iping].heave);
		}
	}

	/* dump or clear data from the buffer */
	ndump = 0;
	if (nbuff > 0) {
		/* turn message on */
		do_message_on("MBnavedit is clearing data...");

		/* copy data to be held */
		for (int iping = 0; iping < hold; iping++) {
			ping[iping] = ping[iping + nbuff - hold];
		}
		ndump = nbuff - hold;
		nbuff = hold;

		/* turn message off */
		do_message_off();
	}
	ndump_total += ndump;

	/* reset current data pointer */
	if (ndump > 0)
		current_id = current_id - ndump;
	if (current_id < 0)
		current_id = 0;
	if (current_id > nbuff - 1)
		current_id = nbuff - 1;

	/* print out information */
	if (verbose >= 1) {
		if (output_mode == OUTPUT_MODE_OUTPUT)
			fprintf(stderr, "\n%d data records dumped to output file <%s>\n", ndump, nfile);
		else
			fprintf(stderr, "\n%d data records dumped from buffer\n", ndump);
		fprintf(stderr, "%d data records remain in buffer\n", nbuff);
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_load_data() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* turn message on */
	nload = 0;
	timestamp_problem = false;
	char string[MB_PATH_MAXLINE];
	sprintf(string, "MBnavedit: %d records loaded so far...", nload);
	do_message_on(string);

	/* load data */
	int status = MB_SUCCESS;
	if (status == MB_SUCCESS)
		do {
			status = mb_get_all(verbose, imbio_ptr, &store_ptr, &kind, ping[nbuff].time_i, &ping[nbuff].time_d, &ping[nbuff].lon,
			                    &ping[nbuff].lat, &ping[nbuff].speed, &ping[nbuff].heading, &distance, &altitude, &sensordepth,
			                    &nbath, &namp, &nss, beamflag, bath, amp, bathacrosstrack, bathalongtrack, ss, ssacrosstrack,
			                    ssalongtrack, comment, &error);
			if (error <= MB_ERROR_NO_ERROR && (kind == nav_source || (kind == MB_DATA_DATA && use_ping_data)) &&
			    (error == MB_ERROR_NO_ERROR || error == MB_ERROR_TIME_GAP || error == MB_ERROR_OUT_BOUNDS ||
			     error == MB_ERROR_OUT_TIME || error == MB_ERROR_SPEED_TOO_SMALL)) {
				status = MB_SUCCESS;
				error = MB_ERROR_NO_ERROR;
			}
			else if (error <= MB_ERROR_NO_ERROR) {
				status = MB_FAILURE;
				error = MB_ERROR_OTHER;
			}
			if (error == MB_ERROR_NO_ERROR && (kind == nav_source || (kind == MB_DATA_DATA && use_ping_data))) {
				status = mb_extract_nav(verbose, imbio_ptr, store_ptr, &kind, ping[nbuff].time_i, &ping[nbuff].time_d,
				                        &ping[nbuff].lon, &ping[nbuff].lat, &ping[nbuff].speed, &ping[nbuff].heading,
				                        &ping[nbuff].draft, &ping[nbuff].roll, &ping[nbuff].pitch, &ping[nbuff].heave, &error);
			}
			if (status == MB_SUCCESS) {
				/* get first time value if first record */
				if (!first_read) {
					file_start_time_d = ping[nbuff].time_d;
					first_read = true;
				}

				/* get original values */
				ping[nbuff].id = nload;
				ping[nbuff].record = ping[nbuff].id + ndump_total;
				ping[nbuff].lon_org = ping[nbuff].lon;
				ping[nbuff].lat_org = ping[nbuff].lat;
				ping[nbuff].speed_org = ping[nbuff].speed;
				ping[nbuff].heading_org = ping[nbuff].heading;
				ping[nbuff].draft_org = ping[nbuff].draft;
				ping[nbuff].file_time_d = ping[nbuff].time_d - file_start_time_d;

				/* apply offsets */
				ping[nbuff].lon += offset_lon;
				ping[nbuff].lat += offset_lat;

				/* set starting dr */
				ping[nbuff].mean_ok = false;
				ping[nbuff].lon_dr = ping[nbuff].lon;
				ping[nbuff].lat_dr = ping[nbuff].lat;

				/* set everything deselected */
				ping[nbuff].tint_select = false;
				ping[nbuff].lon_select = false;
				ping[nbuff].lat_select = false;
				ping[nbuff].speed_select = false;
				ping[nbuff].heading_select = false;
				ping[nbuff].draft_select = false;
				ping[nbuff].lonlat_flag = false;

				/* select repeated data */
				if (nbuff > 0 && ping[nbuff].lon == ping[nbuff - 1].lon && ping[nbuff].lat == ping[nbuff - 1].lat) {
					ping[nbuff].lonlat_flag = true;
				}

							if (verbose >= 5) {
					fprintf(stderr, "\ndbg5  Next good data found in function <%s>:\n", __func__);
					fprintf(stderr,
					        "dbg5       %4d %4d %4d  %d/%d/%d %2.2d:%2.2d:%2.2d.%6.6d  %15.10f %15.10f %6.3f %7.3f %8.4f %6.3f "
					        "%6.3f %8.4f\n",
					        nbuff, ping[nbuff].id, ping[nbuff].record, ping[nbuff].time_i[1], ping[nbuff].time_i[2],
					        ping[nbuff].time_i[0], ping[nbuff].time_i[3], ping[nbuff].time_i[4], ping[nbuff].time_i[5],
					        ping[nbuff].time_i[6], ping[nbuff].lon, ping[nbuff].lat, ping[nbuff].speed, ping[nbuff].heading,
					        ping[nbuff].draft, ping[nbuff].roll, ping[nbuff].pitch, ping[nbuff].heave);
				}

				/* increment counting variables */
				nbuff++;
				nload++;

				/* update message every 250 records */
				if (nload % 250 == 0) {
					sprintf(string, "MBnavedit: %d records loaded so far...", nload);
					do_message_on(string);
				}
			}
		} while (error <= MB_ERROR_NO_ERROR && nbuff < MBNAVEDIT_BUFFER_SIZE);
	nload_total += nload;

	/* define success */
	if (nbuff > 0) {
		status = MB_SUCCESS;
		error = MB_ERROR_NO_ERROR;
	}

	/* check for time stamp repeats */
	timestamp_problem = false;
	for (int i = 0; i < nbuff - 1; i++) {
		if (ping[i + 1].time_d <= ping[i].time_d) {
			timestamp_problem = true;
		}
	}

	/* calculate expected time */
	if (nbuff > 1) {
		for (int i = 1; i < nbuff; i++) {
			ping[i].tint = ping[i].time_d - ping[i - 1].time_d;
			ping[i].tint_org = ping[i].tint;
			ping[i].time_d_org = ping[i].time_d;
		}
		ping[0].tint = ping[1].tint;
		ping[0].tint_org = ping[1].tint_org;
		ping[0].time_d_org = ping[0].time_d;
	}
	else if (nbuff == 0) {
		ping[0].tint = 0.0;
		ping[0].tint_org = 0.0;
		ping[0].time_d_org = ping[0].time_d;
	}

	/* find index of current ping */
	current_id = 0;

	/* reset plotting time span */
	if (nbuff > 0) {
		data_show_size = 0;
		plot_start_time = ping[0].file_time_d;
		plot_end_time = ping[nbuff - 1].file_time_d;
		nplot = nbuff;
	}

	/* calculate speed-made-good and course-made-good */
	for (int i = 0; i < nbuff; i++)
		mbnavedit_get_smgcmg(i);

	/* calculate model */
	mbnavedit_get_model();

	/* turn message off */
	do_message_off();

	/* print out information */
	if (verbose >= 1) {
		fprintf(stderr, "\n%d data records loaded from input file <%s>\n", nload, ifile);
		fprintf(stderr, "%d data records now in buffer\n", nbuff);
		fprintf(stderr, "Current data record:        %d\n", current_id);
		fprintf(stderr, "Current global data record: %d\n", current_id + ndump_total);
	}

	/* put up warning if timestamp problem detected */
	if (timestamp_problem) {
		do_error_dialog("Duplicate or reverse order time", "stamps detected!! Time interpolation",
		                "available under Controls menu.");
	}

	/* update controls */
	do_set_controls();

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_clear_screen() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* clear screen */
	xg_fillrectangle(mbnavedit_xgid, 0, 0, plot_width, NUMBER_PLOTS_MAX * plot_height, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_next_buffer(int *quit) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* clear the screen */
	int status = mbnavedit_clear_screen();

	/* set quit off */
	*quit = false;

	/* check if a file has been opened */
	if (file_open) {

		/* dump the buffer */
		status = mbnavedit_dump_data(hold_size);

		/* load the buffer */
		status = mbnavedit_load_data();

		/* if end of file reached then
		    dump last buffer and close file */
		if (nload <= 0) {
			const int save_dumped = ndump;
			status = mbnavedit_dump_data(0);
			status = mbnavedit_close_file();
			ndump = ndump + save_dumped;

			/* if in normal mode last next_buffer
			    does not mean quit,
			    if in gui mode it does mean quit */
			if (gui_mode)
				*quit = true;
			else
				*quit = false;

			/* if quitting let the world know... */
			if (*quit && verbose >= 1)
				fprintf(stderr, "\nQuitting MBnavedit\nBye Bye...\n");
		}

		/* else plot it */
		else {
			status = mbnavedit_plot_all();
		}
	}

	/* if no file open set failure status */
	else {
		status = MB_FAILURE;
		ndump = 0;
		nload = 0;
		current_id = 0;
	}

	/* reset data_save */
	data_save = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       quit:        %d\n", *quit);
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_offset() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* check if a file has been opened */
	if (file_open) {
		/* apply position offsets to the data */
		for (int i = 0; i < nbuff; i++) {
			ping[i].lon += offset_lon - offset_lon_applied;
			ping[i].lat += offset_lat - offset_lat_applied;
		}
	}
	offset_lon_applied = offset_lon;
	offset_lat_applied = offset_lat;

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_close() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = mbnavedit_clear_screen();

	/* if file has been opened and browse mode
	    just dump the current buffer and close the file */
	if (file_open && output_mode == OUTPUT_MODE_BROWSE) {
		/* dump the buffer */
		status = mbnavedit_dump_data(0);

		/* now close the file */
		status = mbnavedit_close_file();
	}
	/* if file has been opened deal with it */
	else if (file_open) {
		/* dump and load until the end of the file is reached */
		int save_ndumped = 0;
		int save_nloaded = 0;
		do {
			/* dump the buffer */
			status = mbnavedit_dump_data(0);
			save_ndumped += ndump;

			/* load the buffer */
			status = mbnavedit_load_data();
			save_nloaded += nload;
		} while (nload > 0);
		ndump = save_ndumped;
		nload = save_nloaded;

		/* now close the file */
		status = mbnavedit_close_file();
	}
	else {
		ndump = 0;
		nload = 0;
		nbuff = 0;
		current_id = 0;
		status = MB_FAILURE;
	}

	/* reset data_save */
	data_save = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_done(int *quit) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* if in normal mode done does not mean quit,
	    if in gui mode done does mean quit */
	if (gui_mode)
		*quit = true;
	else
		*quit = false;

	/* if quitting let the world know... */
	if (*quit && verbose >= 1)
		fprintf(stderr, "\nShutting MBnavedit down without further ado...\n");

	/* call routine to deal with saving the current file, if any */
	int status = MB_SUCCESS;
	if (file_open)
		status = mbnavedit_action_close();

	/* if quitting let the world know... */
	if (*quit && verbose >= 1)
		fprintf(stderr, "\nQuitting MBnavedit\nBye Bye...\n");

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       quit:        %d\n", *quit);
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_quit() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* let the world know... */
	if (verbose >= 1)
		fprintf(stderr, "\nShutting MBnavedit down without further ado...\n");

	/* call routine to deal with saving the current file, if any */
	int status = MB_SUCCESS;
	if (file_open)
		status = mbnavedit_action_close();

	/* let the world know... */
	if (verbose >= 1)
		fprintf(stderr, "\nQuitting MBnavedit\nBye Bye...\n");

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_step(int step) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       step:       %d\n", step);
	}

	int status = MB_SUCCESS;

	/* check if a file has been opened */
	if (file_open && nbuff > 0) {

		/* if current time span includes last data don't step */
		if (step >= 0 && plot_end_time < ping[nbuff - 1].file_time_d) {
			plot_start_time = plot_start_time + step;
			plot_end_time = plot_start_time + data_show_size;
		}
		else if (step < 0 && plot_start_time > ping[0].file_time_d) {
			plot_start_time = plot_start_time + step;
			plot_end_time = plot_start_time + data_show_size;
		}

		/* get current start of plotting data */
		bool set = false;
		const int old_id = current_id;
		int new_id = nbuff - 1;   /* port: no record at or after the start = the last one (was left unset) */
		for (int i = 0; i < nbuff; i++) {
			if (!set && ping[i].file_time_d >= plot_start_time) {
				new_id = i;
				set = true;
			}
		}
		if (new_id < 0)
			new_id = 0;
		if (new_id >= nbuff)
			new_id = nbuff - 1;
		if (step < 0 && new_id > 0 && new_id == old_id)
			new_id--;
		if (step > 0 && new_id < nbuff - 1 && new_id == old_id)
			new_id++;
		current_id = new_id;

		/* replot */
		if (nbuff > 0) {
			status = mbnavedit_plot_all();
		}

		/* set failure flag if no step was made */
		if (new_id == old_id)
			status = MB_FAILURE;
	}

	/* if no file open set failure status */
	else {
		status = MB_FAILURE;
		current_id = 0;
	}

	/* print out information */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  Current buffer values:\n");
		fprintf(stderr, "dbg2       nload:       %d\n", nload);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       current_id:  %d\n", current_id);
	}

	/* reset data_save */
	data_save = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_end() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* check if a file has been opened */
	if (file_open && nbuff > 0) {
		/* set time span to include last data */
		plot_end_time = ping[nbuff - 1].file_time_d;
		plot_start_time = plot_end_time - data_show_size;

		/* get current start of plotting data */
		const int old_id = current_id;
		bool set = false;
		for (int i = 0; i < nbuff && !set; i++) {
			if (ping[i].file_time_d >= plot_start_time) {
				current_id = i;
				set = true;
			}
		}

		/* replot */
		status = mbnavedit_plot_all();

		/* set failure flag if no step was made */
		if (current_id == old_id)
			status = MB_FAILURE;
	}

	/* if no file open set failure status */
	else {
		status = MB_FAILURE;
		current_id = 0;
	}

	/* print out information */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  Current buffer values:\n");
		fprintf(stderr, "dbg2       nload:       %d\n", nload);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       current_id:  %d\n", current_id);
	}

	/* reset data_save */
	data_save = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_start() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* check if a file has been opened */
	if (file_open && nbuff > 0) {
		const int old_id = current_id;
		current_id = 0;
		plot_start_time = ping[current_id].file_time_d;
		plot_end_time = plot_start_time + data_show_size;

		/* replot */
		if (nbuff > 0) {
			status = mbnavedit_plot_all();
		}

		/* set failure flag if no step was made */
		if (current_id == old_id)
			status = MB_FAILURE;
	}

	/* if no file open set failure status */
	else {
		status = MB_FAILURE;
		current_id = 0;
	}

	/* print out information */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  Current buffer values:\n");
		fprintf(stderr, "dbg2       nload:       %d\n", nload);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       nbuff:       %d\n", nbuff);
		fprintf(stderr, "dbg2       current_id:  %d\n", current_id);
	}

	/* reset data_save */
	data_save = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_mouse_pick(int xx, int yy) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xx:         %d\n", xx);
		fprintf(stderr, "dbg2       yy:         %d\n", yy);
	}

	/* don't try to do anything if no data */
	int active_plot = -1;
	if (nplot > 0) {
		/* figure out which plot the cursor is in */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (xx >= mbnavplot[iplot].ixmin && xx <= mbnavplot[iplot].ixmax && yy <= mbnavplot[iplot].iymin &&
			    yy >= mbnavplot[iplot].iymax)
				active_plot = iplot;
		}
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data or not in plot */
	if (nplot > 0 && active_plot > -1) {
		/* deselect everything in non-active plots */
		bool deselect = false;
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (iplot != active_plot) {
				status = mbnavedit_action_deselect_all(mbnavplot[iplot].type);
				if (status == MB_SUCCESS)
					deselect = true;
			}
		}

		/* if anything was actually deselected, replot */
		if (deselect == MB_SUCCESS) {
			/* clear the screen */
			status = mbnavedit_clear_screen();

			/* replot the screen */
			status = mbnavedit_plot_all();
		}
		status = MB_SUCCESS;

		/* figure out which data point is closest to cursor */
		int range_min = 100000;
		int iping;
		int ix;
		int iy;
		for (int i = current_id; i < current_id + nplot; i++) {
			// TODO(schwehr): Why not a switch?
			if (mbnavplot[active_plot].type == PLOT_TINT) {
				ix = xx - ping[i].tint_x;
				iy = yy - ping[i].tint_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_LONGITUDE) {
				ix = xx - ping[i].lon_x;
				iy = yy - ping[i].lon_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_LATITUDE) {
				ix = xx - ping[i].lat_x;
				iy = yy - ping[i].lat_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_SPEED) {
				ix = xx - ping[i].speed_x;
				iy = yy - ping[i].speed_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_HEADING) {
				ix = xx - ping[i].heading_x;
				iy = yy - ping[i].heading_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_DRAFT) {
				ix = xx - ping[i].draft_x;
				iy = yy - ping[i].draft_y;
			}
			// TODO(schwehr): What about else?  It might get a prior value.
			const int range = (int)sqrt((double)(ix * ix + iy * iy));
			if (range < range_min) {
				range_min = range;
				iping = i;
			}
		}

		/* if it is close enough select or unselect the value
		    and replot it */
		if (range_min <= MBNAVEDIT_PICK_DISTANCE) {
			if (mbnavplot[active_plot].type == PLOT_TINT) {
				if (ping[iping].tint_select)
					ping[iping].tint_select = false;
				else
					ping[iping].tint_select = true;
				mbnavedit_plot_tint_value(active_plot, iping);
			}
			else if (mbnavplot[active_plot].type == PLOT_LONGITUDE) {
				if (ping[iping].lon_select)
					ping[iping].lon_select = false;
				else
					ping[iping].lon_select = true;
				mbnavedit_plot_lon_value(active_plot, iping);
			}
			else if (mbnavplot[active_plot].type == PLOT_LATITUDE) {
				if (ping[iping].lat_select)
					ping[iping].lat_select = false;
				else
					ping[iping].lat_select = true;
				mbnavedit_plot_lat_value(active_plot, iping);
			}
			else if (mbnavplot[active_plot].type == PLOT_SPEED) {
				if (ping[iping].speed_select)
					ping[iping].speed_select = false;
				else
					ping[iping].speed_select = true;
				mbnavedit_plot_speed_value(active_plot, iping);
			}
			else if (mbnavplot[active_plot].type == PLOT_HEADING) {
				if (ping[iping].heading_select)
					ping[iping].heading_select = false;
				else
					ping[iping].heading_select = true;
				mbnavedit_plot_heading_value(active_plot, iping);
			}
			else if (mbnavplot[active_plot].type == PLOT_DRAFT) {
				if (ping[iping].draft_select)
					ping[iping].draft_select = false;
				else
					ping[iping].draft_select = true;
				mbnavedit_plot_draft_value(active_plot, iping);
			}
		}
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_mouse_select(int xx, int yy) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xx:         %d\n", xx);
		fprintf(stderr, "dbg2       yy:         %d\n", yy);
	}

	/* don't try to do anything if no data */
	int active_plot = -1;
	if (nplot > 0) {

		/* figure out which plot the cursor is in */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (xx >= mbnavplot[iplot].ixmin && xx <= mbnavplot[iplot].ixmax && yy <= mbnavplot[iplot].iymin &&
			    yy >= mbnavplot[iplot].iymax)
				active_plot = iplot;
		}
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data or not in plot */
	if (nplot > 0 && active_plot > -1) {

		/* deselect everything in non-active plots */
		bool deselect = false;
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (iplot != active_plot) {
				status = mbnavedit_action_deselect_all(mbnavplot[iplot].type);
				if (status == MB_SUCCESS)
					deselect = true;
			}
		}

		/* if anything was actually deselected, replot */
		if (deselect == MB_SUCCESS) {
			/* clear the screen */
			status = mbnavedit_clear_screen();

			/* replot the screen */
			status = mbnavedit_plot_all();
		}
		status = MB_SUCCESS;

		/* find all data points that are close enough */
		int ix;
		int iy;
		for (int i = current_id; i < current_id + nplot; i++) {
			if (mbnavplot[active_plot].type == PLOT_TINT) {
				ix = xx - ping[i].tint_x;
				iy = yy - ping[i].tint_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_LONGITUDE) {
				ix = xx - ping[i].lon_x;
				iy = yy - ping[i].lon_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_LATITUDE) {
				ix = xx - ping[i].lat_x;
				iy = yy - ping[i].lat_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_SPEED) {
				ix = xx - ping[i].speed_x;
				iy = yy - ping[i].speed_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_HEADING) {
				ix = xx - ping[i].heading_x;
				iy = yy - ping[i].heading_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_DRAFT) {
				ix = xx - ping[i].draft_x;
				iy = yy - ping[i].draft_y;
			}
			const int range = (int)sqrt((double)(ix * ix + iy * iy));

			/* if it is close enough select the value
			    and replot it */
			if (range <= MBNAVEDIT_ERASE_DISTANCE) {
				if (mbnavplot[active_plot].type == PLOT_TINT) {
					ping[i].tint_select = true;
					mbnavedit_plot_tint_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_LONGITUDE) {
					ping[i].lon_select = true;
					mbnavedit_plot_lon_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_LATITUDE) {
					ping[i].lat_select = true;
					mbnavedit_plot_lat_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_SPEED) {
					ping[i].speed_select = true;
					mbnavedit_plot_speed_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_HEADING) {
					ping[i].heading_select = true;
					mbnavedit_plot_heading_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_DRAFT) {
					ping[i].draft_select = true;
					mbnavedit_plot_draft_value(active_plot, i);
				}
			}
		}
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_mouse_deselect(int xx, int yy) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xx:         %d\n", xx);
		fprintf(stderr, "dbg2       yy:         %d\n", yy);
	}

	/* don't try to do anything if no data */
	int active_plot = -1;
	if (nplot > 0) {
		/* figure out which plot the cursor is in */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (xx >= mbnavplot[iplot].ixmin && xx <= mbnavplot[iplot].ixmax && yy <= mbnavplot[iplot].iymin &&
			    yy >= mbnavplot[iplot].iymax)
				active_plot = iplot;
		}
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data or not in plot */
	if (nplot > 0 && active_plot > -1) {
		/* deselect everything in non-active plots */
		bool deselect = false;
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (iplot != active_plot) {
				status = mbnavedit_action_deselect_all(mbnavplot[iplot].type);
				if (status == MB_SUCCESS)
					deselect = true;
			}
		}

		/* if anything was actually deselected, replot */
		if (deselect == MB_SUCCESS) {
			/* clear the screen */
			status = mbnavedit_clear_screen();

			/* replot the screen */
			status = mbnavedit_plot_all();
		}
		status = MB_SUCCESS;

		/* find all data points that are close enough */
		int ix;
		int iy;
		for (int i = current_id; i < current_id + nplot; i++) {
			if (mbnavplot[active_plot].type == PLOT_TINT) {
				ix = xx - ping[i].tint_x;
				iy = yy - ping[i].tint_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_LONGITUDE) {
				ix = xx - ping[i].lon_x;
				iy = yy - ping[i].lon_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_LATITUDE) {
				ix = xx - ping[i].lat_x;
				iy = yy - ping[i].lat_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_SPEED) {
				ix = xx - ping[i].speed_x;
				iy = yy - ping[i].speed_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_HEADING) {
				ix = xx - ping[i].heading_x;
				iy = yy - ping[i].heading_y;
			}
			else if (mbnavplot[active_plot].type == PLOT_DRAFT) {
				ix = xx - ping[i].draft_x;
				iy = yy - ping[i].draft_y;
			}
			const int range = (int)sqrt((double)(ix * ix + iy * iy));

			/* if it is close enough deselect the value
			    and replot it */
			if (range <= MBNAVEDIT_ERASE_DISTANCE) {
				if (mbnavplot[active_plot].type == PLOT_TINT) {
					ping[i].tint_select = false;
					mbnavedit_plot_tint_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_LONGITUDE) {
					ping[i].lon_select = false;
					mbnavedit_plot_lon_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_LATITUDE) {
					ping[i].lat_select = false;
					mbnavedit_plot_lat_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_SPEED) {
					ping[i].speed_select = false;
					mbnavedit_plot_speed_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_HEADING) {
					ping[i].heading_select = false;
					mbnavedit_plot_heading_value(active_plot, i);
				}
				else if (mbnavplot[active_plot].type == PLOT_DRAFT) {
					ping[i].draft_select = false;
					mbnavedit_plot_draft_value(active_plot, i);
				}
			}
		}
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_mouse_selectall(int xx, int yy) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xx:         %d\n", xx);
		fprintf(stderr, "dbg2       yy:         %d\n", yy);
	}

	/* don't try to do anything if no data */
	int active_plot = -1;
	if (nplot > 0) {

		/* figure out which plot the cursor is in */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (xx >= mbnavplot[iplot].ixmin && xx <= mbnavplot[iplot].ixmax && yy <= mbnavplot[iplot].iymin &&
			    yy >= mbnavplot[iplot].iymax)
				active_plot = iplot;
		}
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data or not in plot */
	if (nplot > 0 && active_plot > -1) {
		/* deselect everything in non-active plots */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (iplot != active_plot) {
				mbnavedit_action_deselect_all(mbnavplot[iplot].type);
			}
		}

		/* select all data points in active plot */
		for (int i = current_id; i < current_id + nplot; i++) {
			if (mbnavplot[active_plot].type == PLOT_TINT)
				ping[i].tint_select = true;
			else if (mbnavplot[active_plot].type == PLOT_LONGITUDE)
				ping[i].lon_select = true;
			else if (mbnavplot[active_plot].type == PLOT_LATITUDE)
				ping[i].lat_select = true;
			else if (mbnavplot[active_plot].type == PLOT_SPEED)
				ping[i].speed_select = true;
			else if (mbnavplot[active_plot].type == PLOT_HEADING)
				ping[i].heading_select = true;
			else if (mbnavplot[active_plot].type == PLOT_DRAFT)
				ping[i].draft_select = true;
		}

		/* clear the screen */
		status = mbnavedit_clear_screen();

		/* replot the screen */
		status = mbnavedit_plot_all();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_mouse_deselectall(int xx, int yy) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xx:         %d\n", xx);
		fprintf(stderr, "dbg2       yy:         %d\n", yy);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {

		/* deselect all data points in all plots
		    - this logic follows from deselecting all
		    active plots plus all non-active plots */
		for (int i = current_id; i < current_id + nplot; i++) {
			ping[i].tint_select = false;
			ping[i].lon_select = false;
			ping[i].lat_select = false;
			ping[i].speed_select = false;
			ping[i].heading_select = false;
			ping[i].draft_select = false;
		}

		/* clear the screen */
		status = mbnavedit_clear_screen();

		/* replot the screen */
		status = mbnavedit_plot_all();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_deselect_all(int type) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       type:       %d\n", type);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {
		/* deselect all data points in specified data type */
		int ndeselect = 0;
		for (int i = 0; i < nbuff; i++) {
			if (type == PLOT_TINT && ping[i].tint_select) {
				ping[i].tint_select = false;
				ndeselect++;
			}
			else if (type == PLOT_LONGITUDE && ping[i].lon_select) {
				ping[i].lon_select = false;
				ndeselect++;
			}
			else if (type == PLOT_LATITUDE && ping[i].lat_select) {
				ping[i].lat_select = false;
				ndeselect++;
			}
			else if (type == PLOT_SPEED && ping[i].speed_select) {
				ping[i].speed_select = false;
				ndeselect++;
			}
			else if (type == PLOT_HEADING && ping[i].heading_select) {
				ping[i].heading_select = false;
				ndeselect++;
			}
			else if (type == PLOT_DRAFT && ping[i].draft_select) {
				ping[i].draft_select = false;
				ndeselect++;
			}
		}
		if (ndeselect > 0)
			status = MB_SUCCESS;
		else
			status = MB_FAILURE;
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_set_interval(int xx, int yy, int which) {
	// TODO(schwehr): Explain why these need to be static.
	static int interval_bound1;
	static int interval_bound2;
	static double interval_time1;
	static double interval_time2;
	static bool interval_set1 = false;
	static bool interval_set2 = false;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xx:         %d\n", xx);
		fprintf(stderr, "dbg2       yy:         %d\n", yy);
		fprintf(stderr, "dbg2       which:      %d\n", which);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0 && number_plots > 0) {
		/* if which = 0 set first bound and draw dashed lines */
		if (which == 0) {
			/* unplot old line on all plots */
			if (interval_set1)
				for (int i = 0; i < number_plots; i++) {
					xg_drawline(mbnavedit_xgid, interval_bound1, mbnavplot[i].iymin, interval_bound1, mbnavplot[i].iymax,
					            pixel_values[MB_COLOR_WHITE], XG_DASHLINE);
				}

			if (xx < mbnavplot[0].ixmin)
				xx = mbnavplot[0].ixmin;
			if (xx > mbnavplot[0].ixmax)
				xx = mbnavplot[0].ixmax;

			/* get lower bound time and location */
			interval_bound1 = xx;
			interval_time1 = mbnavplot[0].xmin + (xx - mbnavplot[0].ixmin) / mbnavplot[0].xscale;
			interval_set1 = true;

			/* plot line on all plots */
			for (int i = 0; i < number_plots; i++) {
				xg_drawline(mbnavedit_xgid, interval_bound1, mbnavplot[i].iymin, interval_bound1, mbnavplot[i].iymax,
				            pixel_values[MB_COLOR_RED], XG_DASHLINE);
			}
		}

		/* if which = 1 set second bound and draw dashed lines */
		else if (which == 1) {
			/* unplot old line on all plots */
			if (interval_set1)
				for (int i = 0; i < number_plots; i++) {
					xg_drawline(mbnavedit_xgid, interval_bound2, mbnavplot[i].iymin, interval_bound2, mbnavplot[i].iymax,
					            pixel_values[MB_COLOR_WHITE], XG_DASHLINE);
				}

			if (xx < mbnavplot[0].ixmin)
				xx = mbnavplot[0].ixmin;
			if (xx > mbnavplot[0].ixmax)
				xx = mbnavplot[0].ixmax;

			/* get lower bound time and location */
			interval_bound2 = xx;
			interval_time2 = mbnavplot[0].xmin + (xx - mbnavplot[0].ixmin) / mbnavplot[0].xscale;
			interval_set2 = true;

			/* plot line on all plots */
			for (int i = 0; i < number_plots; i++) {
				xg_drawline(mbnavedit_xgid, interval_bound2, mbnavplot[i].iymin, interval_bound2, mbnavplot[i].iymax,
				            pixel_values[MB_COLOR_RED], XG_DASHLINE);
			}
		}

		/* if which = 2 use bounds and replot */
		else if (which == 2 && interval_set1 && interval_set2 && interval_bound1 != interval_bound2) {
			/* switch bounds if necessary */
			if (interval_bound1 > interval_bound2) {
				const int itmp = interval_bound2;
				const double dtmp = interval_time2;
				interval_bound2 = interval_bound1;
				interval_time2 = interval_time1;
				interval_bound1 = itmp;
				interval_time1 = dtmp;
			}

			/* reset plotting parameters */
			plot_start_time = interval_time1;
			plot_end_time = interval_time2;
			data_show_size = plot_end_time - plot_start_time;

			/* reset time stepping parameters */
			data_step_size = data_show_size / 4;
			if (data_step_size > data_step_max)
				data_step_max = 2 * data_step_size;

			/* get current start of plotting data */
			bool set = false;
			for (int i = 0; i < nbuff; i++) {
				if (!set && ping[i].file_time_d >= plot_start_time) {
					current_id = i;
					set = true;
				}
			}
			if (current_id < 0)
				current_id = 0;
			if (current_id >= nbuff)
				current_id = nbuff - 1;

			/* replot */
			mbnavedit_plot_all();
		}

		/* else if which = 3 unset bounds */
		else if (which == 3) {
			interval_set1 = false;
			interval_set2 = false;
		}

		/* else failure */
		else
			status = MB_FAILURE;
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_use_dr() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {
		/* make sure either a lon or lat plot is active */
		int active_plot = -1;
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (mbnavplot[iplot].type == PLOT_LONGITUDE)
				active_plot = iplot;
			else if (mbnavplot[iplot].type == PLOT_LATITUDE)
				active_plot = iplot;
		}

		/* set lonlat to dr lonlat for selected visible data */
		if (active_plot > -1) {
			for (int i = current_id; i < current_id + nplot; i++) {
				if (ping[i].lon_select || ping[i].lat_select) {
					ping[i].lon = ping[i].lon_dr;
					ping[i].lat = ping[i].lat_dr;
				}
			}

			/* calculate speed-made-good and course-made-good */
			for (int i = 0; i < nbuff; i++)
				mbnavedit_get_smgcmg(i);

			/* clear the screen */
			status = mbnavedit_clear_screen();

			/* replot the screen */
			status = mbnavedit_plot_all();
		}

		else
			status = MB_FAILURE;
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_use_smg() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {
		/* figure out which plot is speed */
		int active_plot = -1;
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (mbnavplot[iplot].type == PLOT_SPEED)
				active_plot = iplot;
		}

		/* set speed to speed made good for selected visible data */
		if (active_plot > -1) {
			bool speedheading_change = false;
			for (int i = current_id; i < current_id + nplot; i++) {
				if (ping[i].speed_select) {
					ping[i].speed = ping[i].speed_made_good;
					speedheading_change = true;
				}
			}

			/* recalculate model */
			if (speedheading_change && model_mode == MODEL_MODE_DR)
				mbnavedit_get_model();

			status = mbnavedit_clear_screen();

			/* replot the screen */
			status = mbnavedit_plot_all();
		}

		else
			status = MB_FAILURE;
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_use_cmg() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {
		/* figure out which plot is heading */
		int active_plot = -1;
		for (int iplot = 0; iplot < number_plots; iplot++) {
			if (mbnavplot[iplot].type == PLOT_HEADING)
				active_plot = iplot;
		}

		/* set heading to course made good for selected visible data */
		if (active_plot > -1) {
			bool speedheading_change = false;
			for (int i = current_id; i < current_id + nplot; i++) {
				if (ping[i].heading_select) {
					ping[i].heading = ping[i].course_made_good;
					speedheading_change = true;
				}
			}

			/* recalculate model */
			if (speedheading_change && model_mode == MODEL_MODE_DR)
				mbnavedit_get_model();

			/* clear the screen */
			status = mbnavedit_clear_screen();

			/* replot the screen */
			status = mbnavedit_plot_all();
		}

		else
			status = MB_FAILURE;
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_interpolate() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;
  double dtime;

	/* don't try to do anything if no data */
	if (nplot > 0) {
		/* look for position or time changes */
		bool timelonlat_change = false;
		bool speedheading_change = false;

		/* do expected time */
		for (int iping = 0; iping < nbuff; iping++) {
			if (ping[iping].tint_select) {
				int ibefore = iping;
				for (int i = iping - 1; i >= 0; i--) {
					if (!ping[i].tint_select && ibefore == iping)
						ibefore = i;
				}
				int iafter = iping;
				for (int i = iping + 1; i < nbuff; i++) {
					if (!ping[i].tint_select && iafter == iping)
						iafter = i;
				}
				if (ibefore < iping && iafter > iping) {
					ping[iping].time_d = ping[ibefore].time_d + (ping[iafter].time_d - ping[ibefore].time_d) *
					                                                ((double)(iping - ibefore)) / ((double)(iafter - ibefore));
					ping[iping].tint = ping[iping].time_d - ping[iping - 1].time_d;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping && ibefore > 0) {
					ping[iping].time_d =
					    ping[ibefore].time_d + (ping[ibefore].time_d - ping[ibefore - 1].time_d) * (iping - ibefore);
					ping[iping].tint = ping[iping].time_d - ping[iping - 1].time_d;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping) {
					ping[iping].time_d = ping[ibefore].time_d;
					ping[iping].tint = ping[iping].time_d - ping[iping - 1].time_d;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping && iafter < nbuff - 1) {
					ping[iping].time_d = ping[iafter].time_d + (ping[iafter + 1].time_d - ping[iafter].time_d) * (iping - iafter);
					ping[iping].tint = 0.0;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping) {
					ping[iping].time_d = ping[iafter].time_d;
					ping[iping].tint = ping[iping].time_d - ping[iping - 1].time_d;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				ping[iping].file_time_d = ping[iping].time_d - file_start_time_d;
				status = mb_get_date(verbose, ping[iping].time_d, ping[iping].time_i);
				if (iping < nbuff - 1)
					if (!ping[iping + 1].tint_select)
						ping[iping + 1].tint = ping[iping + 1].time_d - ping[iping].time_d;
			}
		}

		/* do longitude */
		for (int iping = 0; iping < nbuff; iping++) {
			if (ping[iping].lon_select) {
				int ibefore = iping;
				for (int i = iping - 1; i >= 0; i--) {
					if (!ping[i].lon_select && ibefore == iping)
						ibefore = i;
				}
				int iafter = iping;
				for (int i = iping + 1; i < nbuff; i++) {
					if (!ping[i].lon_select && iafter == iping)
						iafter = i;
				}
				if (ibefore < iping && iafter > iping) {
          dtime = ping[iafter].time_d - ping[ibefore].time_d;
          if (dtime > 0.0)
					  ping[iping].lon = ping[ibefore].lon + (ping[iafter].lon - ping[ibefore].lon) *
					                                          (ping[iping].time_d - ping[ibefore].time_d) /
					                                          (ping[iafter].time_d - ping[ibefore].time_d);
          else
					  ping[iping].lon = ping[ibefore].lon + 0.5 * (ping[iafter].lon - ping[ibefore].lon);
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping && ibefore > 0) {
          dtime = ping[iafter].time_d - ping[ibefore - 1].time_d;
					if (dtime > 0.0)
					  ping[iping].lon = ping[ibefore].lon + (ping[ibefore].lon - ping[ibefore - 1].lon) *
					                                          (ping[iping].time_d - ping[ibefore].time_d) /
					                                          (ping[ibefore].time_d - ping[ibefore - 1].time_d);
          else
            ping[iping].lon = ping[ibefore].lon;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping) {
					ping[iping].lon = ping[ibefore].lon;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping && iafter < nbuff - 1) {
          dtime = ping[iafter + 1].time_d - ping[iafter].time_d;
					if (dtime > 0.0)
					  ping[iping].lon = ping[iafter].lon + (ping[iafter + 1].lon - ping[iafter].lon) *
					                                         (ping[iping].time_d - ping[iafter].time_d) /
					                                         (ping[iafter + 1].time_d - ping[iafter].time_d);
          else
            ping[iping].lon = ping[iafter].lon;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping) {
					ping[iping].lon = ping[iafter].lon;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
			}
		}

		/* do latitude */
		for (int iping = 0; iping < nbuff; iping++) {
			if (ping[iping].lat_select) {
				int ibefore = iping;
				for (int i = iping - 1; i >= 0; i--) {
					if (!ping[i].lat_select && ibefore == iping)
						ibefore = i;
				}
				int iafter = iping;
				for (int i = iping + 1; i < nbuff; i++) {
					if (!ping[i].lat_select && iafter == iping)
						iafter = i;
				}
				if (ibefore < iping && iafter > iping) {
					dtime = ping[iafter].time_d - ping[ibefore].time_d;
          if (dtime > 0.0)
					  ping[iping].lat = ping[ibefore].lat + (ping[iafter].lat - ping[ibefore].lat) *
					                                          (ping[iping].time_d - ping[ibefore].time_d) /
					                                          (ping[iafter].time_d - ping[ibefore].time_d);
          else
					  ping[iping].lat = ping[ibefore].lat + 0.5 * (ping[iafter].lat - ping[ibefore].lat);
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping && ibefore > 0) {
					dtime = ping[iafter].time_d - ping[ibefore - 1].time_d;
					if (dtime > 0.0)
					  ping[iping].lat = ping[ibefore].lat + (ping[ibefore].lat - ping[ibefore - 1].lat) *
					                                          (ping[iping].time_d - ping[ibefore].time_d) /
					                                          (ping[ibefore].time_d - ping[ibefore - 1].time_d);
          else
            ping[iping].lat = ping[ibefore].lat;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping) {
					ping[iping].lat = ping[ibefore].lat;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping && iafter < nbuff - 1) {
					dtime = ping[iafter + 1].time_d - ping[iafter].time_d;
					if (dtime > 0.0)
					  ping[iping].lat = ping[iafter].lat + (ping[iafter + 1].lat - ping[iafter].lat) *
					                                         (ping[iping].time_d - ping[iafter].time_d) /
					                                         (ping[iafter + 1].time_d - ping[iafter].time_d);
          else
            ping[iping].lat = ping[iafter].lat;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping) {
					ping[iping].lat = ping[iafter].lat;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
			}
		}

		/* do speed */
		for (int iping = 0; iping < nbuff; iping++) {
			if (ping[iping].speed_select) {
				int ibefore = iping;
				for (int i = iping - 1; i >= 0; i--) {
					if (!ping[i].speed_select && ibefore == iping)
						ibefore = i;
				}
				int iafter = iping;
				for (int i = iping + 1; i < nbuff; i++) {
					if (!ping[i].speed_select && iafter == iping)
						iafter = i;
				}
				if (ibefore < iping && iafter > iping) {
					dtime = ping[iafter].time_d - ping[ibefore].time_d;
					if (dtime > 0.0)
					  ping[iping].speed = ping[ibefore].speed + (ping[iafter].speed - ping[ibefore].speed) *
					                                              (ping[iping].time_d - ping[ibefore].time_d) /
					                                              (ping[iafter].time_d - ping[ibefore].time_d);
          else
					  ping[iping].speed = ping[ibefore].speed + 0.5 * (ping[iafter].speed - ping[ibefore].speed);
					speedheading_change = true;
				}
				else if (ibefore < iping) {
					ping[iping].speed = ping[ibefore].speed;
					speedheading_change = true;
				}
				else if (iafter > iping) {
					ping[iping].speed = ping[iafter].speed;
					speedheading_change = true;
				}
			}
		}

		/* do heading */
		for (int iping = 0; iping < nbuff; iping++) {
			if (ping[iping].heading_select) {
				int ibefore = iping;
				for (int i = iping - 1; i >= 0; i--) {
					if (!ping[i].heading_select && ibefore == iping)
						ibefore = i;
				}
				int iafter = iping;
				for (int i = iping + 1; i < nbuff; i++)
					if (!ping[i].heading_select && iafter == iping)
						iafter = i;
				if (ibefore < iping && iafter > iping) {
					dtime = ping[iafter].time_d - ping[ibefore].time_d;
					if (dtime > 0.0)
					  ping[iping].heading = ping[ibefore].heading + (ping[iafter].heading - ping[ibefore].heading) *
					                                                  (ping[iping].time_d - ping[ibefore].time_d) /
					                                                  (ping[iafter].time_d - ping[ibefore].time_d);
					else
					  ping[iping].heading = ping[ibefore].heading + 0.5 * (ping[iafter].heading - ping[ibefore].heading);
					speedheading_change = true;
				}
				else if (ibefore < iping) {
					ping[iping].heading = ping[ibefore].heading;
					speedheading_change = true;
				}
				else if (iafter > iping) {
					ping[iping].heading = ping[iafter].heading;
					speedheading_change = true;
				}
			}
		}

		/* do draft */
		for (int iping = 0; iping < nbuff; iping++) {
			if (ping[iping].draft_select) {
				int ibefore = iping;
				for (int i = iping - 1; i >= 0; i--) {
					if (!ping[i].draft_select && ibefore == iping)
						ibefore = i;
				}
				int iafter = iping;
				for (int i = iping + 1; i < nbuff; i++)
					if (!ping[i].draft_select && iafter == iping)
						iafter = i;
				if (ibefore < iping && iafter > iping) {
					dtime = ping[iafter].time_d - ping[ibefore].time_d;
					if (dtime > 0.0)
					  ping[iping].draft = ping[ibefore].draft + (ping[iafter].draft - ping[ibefore].draft) *
					                                              (ping[iping].time_d - ping[ibefore].time_d) /
					                                              (ping[iafter].time_d - ping[ibefore].time_d);
					else
					  ping[iping].draft = ping[ibefore].draft + 0.5 * (ping[iafter].draft - ping[ibefore].draft);
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (ibefore < iping) {
					ping[iping].draft = ping[ibefore].draft;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
				else if (iafter > iping) {
					ping[iping].draft = ping[iafter].draft;
					ping[iping].lonlat_flag = true;
					timelonlat_change = true;
				}
			}
		}

		/* recalculate speed-made-good and course-made-good */
		if (timelonlat_change)
			for (int i = 0; i < nbuff; i++)
				mbnavedit_get_smgcmg(i);

		/* recalculate model */
		if (speedheading_change && model_mode == MODEL_MODE_DR)
			mbnavedit_get_model();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_interpolaterepeats() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;
	// bool timelonlat_change;
	// bool speedheading_change;

	/* don't try to do anything if no data */
	if (nplot > 0) {
		/* look for position or time changes */
		bool timelonlat_change = false;
		bool speedheading_change = false;
		int iafter;

		/* do expected time */
		for (int iping = 1; iping < nbuff - 1; iping++) {
			if (ping[iping].tint_select && ping[iping].time_d == ping[iping - 1].time_d) {
				/* find next changed value */
				bool found = false;
				int ibefore = iping - 1;
				iafter = iping;   /* port: no later change found = nothing to interpolate (was left unset) */
				for (int j = iping + 1; j < nbuff && !found; j++) {
					if (ping[iping].time_d != ping[j].time_d) {
						found = true;
						iafter = j;
					}
				}
				for (int j = iping; j < iafter; j++) {
					if (ping[j].tint_select) {
						ping[j].time_d = ping[ibefore].time_d + (ping[iafter].time_d - ping[ibefore].time_d) *
						                                            ((double)(iping - ibefore)) / ((double)(iafter - ibefore));
						timelonlat_change = true;
					}
				}
			}
		}

		/* do longitude */
		for (int iping = 1; iping < nbuff - 1; iping++) {
			if (ping[iping].lon_select && ping[iping].lon == ping[iping - 1].lon) {
				/* find next changed value */
				bool found = false;
				int ibefore = iping - 1;
				iafter = iping;   /* port: no later change found = nothing to interpolate (was left unset) */
				for (int j = iping + 1; j < nbuff && !found; j++) {
					if (ping[iping].lon != ping[j].lon) {
						found = true;
						iafter = j;
					}
				}
				for (int j = iping; j < iafter; j++) {
					if (ping[j].lon_select) {
						ping[j].lon = ping[ibefore].lon + (ping[iafter].lon - ping[ibefore].lon) *
						                                      (ping[j].time_d - ping[ibefore].time_d) /
						                                      (ping[iafter].time_d - ping[ibefore].time_d);
						timelonlat_change = true;
					}
				}
			}
		}

		/* do latitude */
		for (int iping = 1; iping < nbuff - 1; iping++) {
			if (ping[iping].lat_select && ping[iping].lat == ping[iping - 1].lat) {
				/* find next changed value */
				bool found = false;
				int ibefore = iping - 1;
				iafter = iping;   /* port: no later change found = nothing to interpolate (was left unset) */
				for (int j = iping + 1; j < nbuff && !found; j++) {
					if (ping[iping].lat != ping[j].lat) {
						found = true;
						iafter = j;
					}
				}
				for (int j = iping; j < iafter; j++) {
					if (ping[j].lat_select) {
						ping[j].lat = ping[ibefore].lat + (ping[iafter].lat - ping[ibefore].lat) *
						                                      (ping[j].time_d - ping[ibefore].time_d) /
						                                      (ping[iafter].time_d - ping[ibefore].time_d);
						timelonlat_change = true;
					}
				}
			}
		}

		/* do speed */
		for (int iping = 1; iping < nbuff - 1; iping++) {
			if (ping[iping].speed_select && ping[iping].speed == ping[iping - 1].speed) {
				/* find next changed value */
				bool found = false;
				int ibefore = iping - 1;
				iafter = iping;   /* port: no later change found = nothing to interpolate (was left unset) */
				for (int j = iping + 1; j < nbuff && !found; j++) {
					if (ping[iping].speed != ping[j].speed) {
						found = true;
						iafter = j;
					}
				}
				for (int j = iping; j < iafter; j++) {
					if (ping[j].speed_select) {
						ping[j].speed = ping[ibefore].speed + (ping[iafter].speed - ping[ibefore].speed) *
						                                          (ping[j].time_d - ping[ibefore].time_d) /
						                                          (ping[iafter].time_d - ping[ibefore].time_d);
						speedheading_change = true;
					}
				}
			}
		}

		/* do heading */
		for (int iping = 1; iping < nbuff - 1; iping++) {
			if (ping[iping].heading_select && ping[iping].heading == ping[iping - 1].heading) {
				/* find next changed value */
				bool found = false;
				int ibefore = iping - 1;
				iafter = iping;   /* port: no later change found = nothing to interpolate (was left unset) */
				for (int j = iping + 1; j < nbuff && !found; j++) {
					if (ping[iping].heading != ping[j].heading) {
						found = true;
						iafter = j;
					}
				}
				for (int j = iping; j < iafter; j++) {
					if (ping[j].heading_select) {
						ping[j].heading = ping[ibefore].heading + (ping[iafter].heading - ping[ibefore].heading) *
						                                              (ping[j].time_d - ping[ibefore].time_d) /
						                                              (ping[iafter].time_d - ping[ibefore].time_d);
						speedheading_change = true;
					}
				}
			}
		}

		/* do draft */
		for (int iping = 1; iping < nbuff - 1; iping++) {
			if (ping[iping].draft_select && ping[iping].draft == ping[iping - 1].draft) {
				/* find next changed value */
				bool found = false;
				int ibefore = iping - 1;
				iafter = iping;   /* port: no later change found = nothing to interpolate (was left unset) */
				for (int j = iping + 1; j < nbuff && !found; j++) {
					if (ping[iping].draft != ping[j].draft) {
						found = true;
						iafter = j;
					}
				}
				for (int j = iping; j < iafter; j++) {
					if (ping[j].draft_select) {
						ping[j].draft = ping[ibefore].draft + (ping[iafter].draft - ping[ibefore].draft) *
						                                          (ping[j].time_d - ping[ibefore].time_d) /
						                                          (ping[iafter].time_d - ping[ibefore].time_d);
						timelonlat_change = true;
					}
				}
			}
		}

		/* recalculate speed-made-good and course-made-good */
		if (timelonlat_change)
			for (int i = 0; i < nbuff; i++)
				mbnavedit_get_smgcmg(i);

		/* recalculate model */
		if (speedheading_change && model_mode == MODEL_MODE_DR)
			mbnavedit_get_model();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_revert() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {

		/* look for position changes */
		bool timelonlat_change = false;
		bool speedheading_change = false;

		/* loop over each of the plots */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			for (int i = current_id; i < current_id + nplot; i++) {
				if (mbnavplot[iplot].type == PLOT_TINT) {
					if (ping[i].tint_select) {
						ping[i].time_d = ping[i].time_d_org;
						ping[i].file_time_d = ping[i].time_d - file_start_time_d;
						ping[i].tint = ping[i].time_d - ping[i - 1].time_d;
						timelonlat_change = true;
						if (i < nbuff - 1)
							ping[i + 1].tint = ping[i + 1].time_d - ping[i].time_d;
						status = mb_get_date(verbose, ping[i].time_d, ping[i].time_i);
					}
				}
				else if (mbnavplot[iplot].type == PLOT_LONGITUDE) {
					if (ping[i].lon_select) {
						ping[i].lon = ping[i].lon_org;
						timelonlat_change = true;
					}
				}
				else if (mbnavplot[iplot].type == PLOT_LATITUDE) {
					if (ping[i].lat_select) {
						ping[i].lat = ping[i].lat_org;
						timelonlat_change = true;
					}
				}
				else if (mbnavplot[iplot].type == PLOT_SPEED) {
					if (ping[i].speed_select) {
						ping[i].speed = ping[i].speed_org;
						speedheading_change = true;
					}
				}
				else if (mbnavplot[iplot].type == PLOT_HEADING) {
					if (ping[i].heading_select) {
						ping[i].heading = ping[i].heading_org;
						speedheading_change = true;
					}
				}
				else if (mbnavplot[iplot].type == PLOT_DRAFT) {
					if (ping[i].draft_select) {
						ping[i].draft = ping[i].draft_org;
					}
				}
			}
		}

		/* recalculate speed-made-good and course-made-good */
		if (timelonlat_change)
			for (int i = 0; i < nbuff; i++)
				mbnavedit_get_smgcmg(i);

		/* recalculate model */
		if (speedheading_change && model_mode == MODEL_MODE_DR)
			mbnavedit_get_model();

		/* clear the screen */
		status = mbnavedit_clear_screen();

		/* replot the screen */
		status = mbnavedit_plot_all();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_flag() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {

		/* loop over each of the plots */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			for (int i = current_id; i < current_id + nplot; i++) {
				if (mbnavplot[iplot].type == PLOT_LONGITUDE) {
					if (ping[i].lon_select) {
						ping[i].lonlat_flag = true;
					}
				}
				else if (mbnavplot[iplot].type == PLOT_LATITUDE) {
					if (ping[i].lat_select) {
						ping[i].lonlat_flag = true;
					}
				}
			}
		}

		/* clear the screen */
		status = mbnavedit_clear_screen();

		/* replot the screen */
		status = mbnavedit_plot_all();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_unflag() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int status = MB_SUCCESS;

	/* don't try to do anything if no data */
	if (nplot > 0) {

		/* loop over each of the plots */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			for (int i = current_id; i < current_id + nplot; i++) {
				if (mbnavplot[iplot].type == PLOT_LONGITUDE) {
					if (ping[i].lon_select) {
						ping[i].lonlat_flag = false;
					}
				}
				else if (mbnavplot[iplot].type == PLOT_LATITUDE) {
					if (ping[i].lat_select) {
						ping[i].lonlat_flag = false;
					}
				}
			}
		}

		/* clear the screen */
		status = mbnavedit_clear_screen();

		/* replot the screen */
		status = mbnavedit_plot_all();
	}
	/* if no data then set failure flag */
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_fixtime() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	int istart, iend;
	double start_time_d, end_time_d;

	/* loop over the data */
	for (int i = 0; i < nbuff; i++) {
		if (i == 0) {
			istart = i;
			start_time_d = ping[i].time_d;
		}
		else if (ping[i].time_d > start_time_d) {
			iend = i;
			end_time_d = ping[i].time_d;
			for (int j = istart + 1; j < iend; j++) {
				ping[j].time_d = start_time_d + (j - istart) * (end_time_d - start_time_d) / (iend - istart);
				mb_get_date(verbose, ping[j].time_d, ping[j].time_i);
				ping[j].file_time_d = ping[j].time_d - file_start_time_d;
				if (j > 0)
					ping[j - 1].tint = ping[j].time_d - ping[j - 1].time_d;
				if (j < nbuff - 1)
					ping[j].tint = ping[j + 1].time_d - ping[j].time_d;
			}
			istart = i;
			start_time_d = ping[i].time_d;
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_deletebadtime() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	double lastgood_time_d;
	int nbuffnew;

	/* loop over the data looking for bad times */
	lastgood_time_d = ping[0].time_d;
	for (int i = 1; i < nbuff; i++) {
		if ((ping[i].time_d - lastgood_time_d) <= 0.0) {
			ping[i].id = -1;
		}
		else if ((ping[i].time_d - lastgood_time_d) > 60.0) {
			if (i == nbuff - 1)
				ping[i].id = -1;
			else if (ping[i + 1].time_d - ping[i].time_d <= 0.0)
				ping[i].id = -1;
			else
				lastgood_time_d = ping[i].time_d;
		}
		else if (ping[i].time_d > ping[nbuff - 1].time_d) {
			ping[i].id = -1;
		}
		else {
			lastgood_time_d = ping[i].time_d;
		}
	}

	/* loop over the data in reverse deleting data with bad times */
	nbuffnew = nbuff;
	for (int i = nbuff - 1; i >= 0; i--) {
		if (ping[i].id == -1) {
			for (int j = i; j < nbuffnew - 1; j++) {
				ping[j] = ping[j + 1];
			}
			nbuffnew--;
		}
	}
	fprintf(stderr, "Data deleted: nbuff:%d nbuffnew:%d\n", nbuff, nbuffnew);
	nbuff = nbuffnew;

	/* recompute tint for the compacted buffer -- patching it up incrementally
	   during the deletion loop above was unreliable when multiple points were
	   removed in one pass (stale nbuffnew comparisons, tint taken from a point
	   being deleted), so just recompute cleanly the same way the initial load does */
	for (int i = 1; i < nbuff; i++) {
		ping[i].tint = ping[i].time_d - ping[i - 1].time_d;
	}
	if (nbuff > 1)
		ping[0].tint = ping[1].tint;
	else if (nbuff == 1)
		ping[0].tint = 0.0;

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_action_showall() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* reset plotting time span */
	if (nbuff > 0) {
		plot_start_time = ping[0].file_time_d;
		plot_end_time = ping[nbuff - 1].file_time_d;
		data_show_size = 0;
		current_id = 0;
	}

	/* replot */
	const int status = mbnavedit_plot_all();

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_get_smgcmg(int i) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       i:          %d\n", i);
	}

	int status = MB_SUCCESS;

	/* calculate speed made good and course made for ping i */
	if (i < nbuff && nbuff == 1) {
		/* only one point in the buffer -- no neighbor exists to compute motion from */
		ping[i].speed_made_good = 0.0;
		ping[i].course_made_good = ping[i].heading;
		status = MB_SUCCESS;
	}
	else if (i < nbuff) {
		double time_d1, lon1, lat1;
		double time_d2, lon2, lat2;
		if (i == 0) {
			time_d1 = ping[i].time_d;
			lon1 = ping[i].lon;
			lat1 = ping[i].lat;
			time_d2 = ping[i + 1].time_d;
			lon2 = ping[i + 1].lon;
			lat2 = ping[i + 1].lat;
		}
		else if (i == nbuff - 1) {
			time_d1 = ping[i - 1].time_d;
			lon1 = ping[i - 1].lon;
			lat1 = ping[i - 1].lat;
			time_d2 = ping[i].time_d;
			lon2 = ping[i].lon;
			lat2 = ping[i].lat;
		}
		else {
			time_d1 = ping[i - 1].time_d;
			lon1 = ping[i - 1].lon;
			lat1 = ping[i - 1].lat;
			time_d2 = ping[i].time_d;
			lon2 = ping[i].lon;
			lat2 = ping[i].lat;
		}
		double mtodeglon;
		double mtodeglat;
		mb_coor_scale(verbose, lat1, &mtodeglon, &mtodeglat);
		const double del_time = time_d2 - time_d1;
		const double dx = (lon2 - lon1) / mtodeglon;
		const double dy = (lat2 - lat1) / mtodeglat;
		const double dist = sqrt(dx * dx + dy * dy);
		if (del_time > 0.0)
			ping[i].speed_made_good = 3.6 * dist / del_time;
		else
			ping[i].speed_made_good = 0.0;
		if (dist > 0.0)
			ping[i].course_made_good = RTD * atan2(dx / dist, dy / dist);
		else
			ping[i].course_made_good = ping[i].heading;
		if (ping[i].course_made_good < 0.0)
			ping[i].course_made_good = ping[i].course_made_good + 360.0;

		status = MB_SUCCESS;
	}
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_get_model() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
	}

	/* only model if data available */
	if (nbuff > 0) {
		/* call correct modeling function */
		if (model_mode == MODEL_MODE_MEAN)
			mbnavedit_get_gaussianmean();
		else if (model_mode == MODEL_MODE_DR)
			mbnavedit_get_dr();
		else if (model_mode == MODEL_MODE_INVERT)
			mbnavedit_get_inversion();
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_get_gaussianmean() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
	}

	/* loop over navigation calculating gaussian mean positions */
	const double timewindow = 0.1 * mean_time_window;
	const double a = -4.0 / (timewindow * timewindow);
	int jstart = 0;
	for (int i = 0; i < nbuff; i++) {
		double dt = 0.0;
		double weight = 0.0;
		double sumlon = 0.0;
		double sumlat = 0.0;
		int nsum = 0;
		int npos = 0;
		int nneg = 0;
		for (int j = jstart; j < nbuff && dt <= timewindow; j++) {
			dt = ping[j].time_d - ping[i].time_d;
			if (!ping[j].lonlat_flag && fabs(dt) <= timewindow) {
				const double w = exp(a * dt * dt);
				nsum++;
				if (dt < 0.0)
					nneg++;
				if (dt >= 0.0)
					npos++;
				weight += w;
				sumlon += w * ping[j].lon;
				sumlat += w * ping[j].lat;
				if (nsum == 1)
					jstart = j;
			}
		}
		if (npos > 0 && nneg > 0) {
			ping[i].mean_ok = true;
			ping[i].lon_dr = sumlon / weight;
			ping[i].lat_dr = sumlat / weight;
		}
		else {
			ping[i].mean_ok = false;
			ping[i].lon_dr = ping[i].lon;
			ping[i].lat_dr = ping[i].lat;
		}
	}

	/* loop over navigation performing linear interpolation to fill gaps */
	int jbefore = -1;
	for (int i = 0; i < nbuff; i++) {
		/* only work on nav not smoothed in first past due to lack of nearby data */
		if (!ping[i].mean_ok) {
			/* find valid points before and after */
			int jafter = i;
			for (int j = jbefore; j < nbuff && jafter == i; j++) {
				if (j < i && !ping[j].lonlat_flag)
					jbefore = j;
				if (j > i && !ping[j].lonlat_flag)
					jafter = j;
			}
			if (jbefore >= 0 && jafter > i) {
				const double dt = (ping[i].time_d - ping[jbefore].time_d) / (ping[jafter].time_d - ping[jbefore].time_d);
				ping[i].lon_dr = ping[jbefore].lon + dt * (ping[jafter].lon - ping[jbefore].lon);
				ping[i].lat_dr = ping[jbefore].lat + dt * (ping[jafter].lat - ping[jbefore].lat);
			}
			else if (jbefore >= 0) {
				ping[i].lon_dr = ping[jbefore].lon;
				ping[i].lat_dr = ping[jbefore].lat;
			}
			else if (jafter > i) {
				ping[i].lon_dr = ping[jafter].lon;
				ping[i].lat_dr = ping[jafter].lat;
			}
			else {
				ping[i].lon_dr = ping[i].lon;
				ping[i].lat_dr = ping[i].lat;
			}
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_get_dr() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
	}

	double mtodeglon, mtodeglat;
	double del_time, dx, dy;
	double driftlon, driftlat;

	/* calculate dead reckoning */
	driftlon = 0.00001 * drift_lon;
	driftlat = 0.00001 * drift_lat;
	for (int i = 0; i < nbuff; i++) {
		if (i == 0) {
			ping[i].lon_dr = ping[i].lon;
			ping[i].lat_dr = ping[i].lat;
		}
		else {
			del_time = ping[i].time_d - ping[i - 1].time_d;
			if (del_time < 300.0) {
				mb_coor_scale(verbose, ping[i].lat, &mtodeglon, &mtodeglat);
				dx = sin(DTR * ping[i].heading) * ping[i].speed * del_time / 3.6;
				dy = cos(DTR * ping[i].heading) * ping[i].speed * del_time / 3.6;
				ping[i].lon_dr = ping[i - 1].lon_dr + dx * mtodeglon + del_time * driftlon / 3600.0;
				ping[i].lat_dr = ping[i - 1].lat_dr + dy * mtodeglat + del_time * driftlat / 3600.0;
			}
			else {
				ping[i].lon_dr = ping[i].lon;
				ping[i].lat_dr = ping[i].lat;
			}
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_get_inversion() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
	}

	/* nothing to invert with fewer than one point */
	if (nplot < 1) {
		if (verbose >= 2) {
			fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
			fprintf(stderr, "dbg2  Return values:\n");
			fprintf(stderr, "dbg2       error:       %d\n", error);
		}
		return (MB_SUCCESS);
	}

	/* set maximum dimensions of the inverse problem
	   (one row per point constrains lon/lat; up to nplot-1 rows constrain speed;
	   up to nplot-2 rows constrain acceleration -- clamp both to zero for small nplot) */
	const int nrows = nplot + MAX(nplot - 1, 0) + MAX(nplot - 2, 0);
	const int ncols = nplot;
	const int nnz = 3;
	const int ncycle = 512;
	const double bandwidth = 10000.0;

	/* get average lon value */
	double lon_avg = 0.0;
	int nlon_avg = 0;
	double lat_avg = 0.0;
	int nlat_avg = 0;
	int first = current_id;
	int last = current_id;
	for (int i = current_id; i < current_id + nplot; i++) {
		/* constrain lon unless flagged by user */
		if (!ping[i].lonlat_flag) {
			lon_avg += ping[i].lon;
			nlon_avg++;
			lat_avg += ping[i].lat;
			nlat_avg++;
			last = i;
		}
		else if (first == i && i < current_id + nplot - 1) {
			first = i + 1;
		}
	}
	if (nlon_avg > 0)
		lon_avg /= nlon_avg;
	if (nlat_avg > 0)
		lat_avg /= nlat_avg;

	double mtodeglon;
	double mtodeglat;
	mb_coor_scale(verbose, lat_avg, &mtodeglon, &mtodeglat);

	/* allocate space for the inverse problem
	   (check status after every call -- mb_mallocd() resets *error to
	   MB_ERROR_NO_ERROR on each successful call, so a later success would
	   otherwise silently mask an earlier allocation failure) */
	double *a = NULL;
	int *ia = NULL;
	int *nia = NULL;
	double *d = NULL;
	double *x = NULL;
	int *nx = NULL;
	double *dx = NULL;
	double *sigma = NULL;
	double *work = NULL;
	int status = mb_mallocd(verbose, __FILE__, __LINE__, nnz * nrows * sizeof(double), (void **)&a, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, nnz * nrows * sizeof(int), (void **)&ia, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, nrows * sizeof(int), (void **)&nia, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, nrows * sizeof(double), (void **)&d, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, ncols * sizeof(double), (void **)&x, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, ncols * sizeof(int), (void **)&nx, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, ncols * sizeof(double), (void **)&dx, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, ncycle * sizeof(double), (void **)&sigma, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, ncycle * sizeof(double), (void **)&work, &error);

	/* do inversion */
	if (status == MB_SUCCESS) {
		/* set message */
		char string[MB_PATH_MAXLINE];
		sprintf(string, "Setting up inversion of %d longitude points", nplot);
		do_message_on(string);

		/* initialize arrays */
		for (int i = 0; i < nrows; i++) {
			nia[i] = 0;
			d[i] = 0.0;
			for (int j = 0; j < nnz; j++) {
				const int k = nnz * i + j;
				ia[k] = 0;
				a[k] = 0.0;
			}
		}
		for (int i = 0; i < ncols; i++) {
			nx[i] = 0;
			x[i] = 0;
			dx[i] = 0.0;
		}
		for (int i = 0; i < ncycle; i++) {
			sigma[i] = 0;
			work[i] = 0.0;
		}

		double dtime_d;
		double dtime_d_sq;

		/* loop over all nav points - add constraints for
		   original lon values, speed, acceleration */
		int nr = 0;
		int nc = nplot;
		for (int i = current_id; i < current_id + nplot; i++) {
			const int ii = i - current_id;

			/* constrain lon unless flagged by user */
			if (!ping[i].lonlat_flag) {
				const int k = nnz * nr;
				d[nr] = (ping[i].lon_org - lon_avg) / mtodeglon;
				nia[nr] = 1;
				ia[k] = ii;
				a[k] = 1.0;
				nr++;
			}

			/* constrain speed */
			if (weight_speed > 0.0 && ii > 0 && ping[i].time_d > ping[i - 1].time_d) {
				/* get time difference */
				dtime_d = ping[i].time_d - ping[i - 1].time_d;

				/* constrain lon speed */
				const int k = nnz * nr;
				d[nr] = 0.0;
				nia[nr] = 2;
				ia[k] = ii - 1;
				a[k] = -weight_speed / dtime_d;
				ia[k + 1] = ii;
				a[k + 1] = weight_speed / dtime_d;
				nr++;
			}

			/* constrain acceleration */
			if (weight_acceleration > 0.0 && ii > 0 && ii < nplot - 1 && ping[i + 1].time_d > ping[i - 1].time_d) {
				/* get time difference */
				dtime_d = ping[i + 1].time_d - ping[i - 1].time_d;
				dtime_d_sq = dtime_d * dtime_d;

				/* constrain lon acceleration */
				const int k = nnz * nr;
				d[nr] = 0.0;
				nia[nr] = 3;
				ia[k] = ii - 1;
				a[k] = weight_acceleration / dtime_d_sq;
				ia[k + 1] = ii;
				a[k + 1] = -2.0 * weight_acceleration / dtime_d_sq;
				ia[k + 2] = ii + 1;
				a[k + 2] = weight_acceleration / dtime_d_sq;
				nr++;
			}
		}

		/* set message */
		sprintf(string, "Inverting %dX%d for smooth longitude...", nc, nr);
		do_message_on(string);

		/* compute upper bound on maximum eigenvalue */
		int ncyc = 0;
		int nsig = 0;
		double smax;
		double sup;
		double err;
		lspeig(a, ia, nia, nnz, nc, nr, ncyc, &nsig, x, dx, sigma, work, &smax, &err, &sup);
		double supt = smax + err;
		if (sup > supt)
			supt = sup;
		if (verbose > 1)
			fprintf(stderr, "Initial lspeig: %g %g %g %g\n", sup, smax, err, supt);
		ncyc = 16;
		for (int i = 0; i < 4; i++) {
			lspeig(a, ia, nia, nnz, nc, nr, ncyc, &nsig, x, dx, sigma, work, &smax, &err, &sup);
			supt = smax + err;
			if (sup > supt)
				supt = sup;
			if (verbose > 1)
				fprintf(stderr, "lspeig[%d]: %g %g %g %g\n", i, sup, smax, err, supt);
		}

		/* calculate chebyshev factors (errlsq is the theoretical error) */
		double slo = supt / bandwidth;
		chebyu(sigma, ncycle, supt, slo, work);
		double errlsq = errlim(sigma, ncycle, supt, slo);
		if (verbose > 1)
			fprintf(stderr, "Theoretical error: %f\n", errlsq);
		if (verbose > 1)
			for (int i = 0; i < ncycle; i++)
				fprintf(stderr, "sigma[%d]: %f\n", i, sigma[i]);

		/* solve the problem */
		for (int i = 0; i < nc; i++)
			x[i] = 0.0;
		lsqup(a, ia, nia, nnz, nc, nr, x, dx, d, 0, NULL, NULL, ncycle, sigma);

		/* generate solution */
		for (int i = current_id; i < current_id + nplot; i++) {
			const int ii = i - current_id;
			ping[i].lon_dr = lon_avg + mtodeglon * x[ii];
		}

		/* make flagged ends of data flat */
		for (int i = current_id; i < first; i++) {
			const int ii = first - current_id;
			ping[i].lon_dr = lon_avg + mtodeglon * x[ii];
		}
		for (int i = last + 1; i < current_id + nplot; i++) {
			const int ii = last - current_id;
			ping[i].lon_dr = lon_avg + mtodeglon * x[ii];
		}

		/* set message */
		sprintf(string, "Setting up inversion of %d latitude points", nplot);
		do_message_on(string);

		/* initialize arrays */
		for (int i = 0; i < nrows; i++) {
			nia[i] = 0;
			d[i] = 0.0;
			for (int j = 0; j < nnz; j++) {
				const int k = nnz * i + j;
				ia[k] = 0;
				a[k] = 0.0;
			}
		}
		for (int i = 0; i < ncols; i++) {
			nx[i] = 0;
			x[i] = 0;
			dx[i] = 0.0;
		}
		for (int i = 0; i < ncycle; i++) {
			sigma[i] = 0;
			work[i] = 0.0;
		}

		/* loop over all nav points - add constraints for
		   original lat values, speed, acceleration */
		nr = 0;
		nc = nplot;
		for (int i = current_id; i < current_id + nplot; i++) {
			const int ii = i - current_id;

			/* constrain lat unless flagged by user */
			if (!ping[i].lonlat_flag) {
				const int k = nnz * nr;
				d[nr] = (ping[i].lat_org - lat_avg) / mtodeglat;
				nia[nr] = 1;
				ia[k] = ii;
				a[k] = 1.0;
				nr++;
			}

			/* constrain speed */
			if (weight_speed > 0.0 && ii > 0 && ping[i].time_d > ping[i - 1].time_d) {
				/* get time difference */
				dtime_d = ping[i].time_d - ping[i - 1].time_d;

				/* constrain lat speed */
				const int k = nnz * nr;
				d[nr] = 0.0;
				nia[nr] = 2;
				ia[k] = ii - 1;
				a[k] = -weight_speed / dtime_d;
				ia[k + 1] = ii;
				a[k + 1] = weight_speed / dtime_d;
				nr++;
			}

			/* constrain acceleration */
			if (weight_acceleration > 0.0 && ii > 0 && ii < nplot - 1 && ping[i + 1].time_d > ping[i - 1].time_d) {
				/* get time difference */
				dtime_d = ping[i + 1].time_d - ping[i - 1].time_d;
				dtime_d_sq = dtime_d * dtime_d;

				/* constrain lat acceleration */
				const int k = nnz * nr;
				d[nr] = 0.0;
				nia[nr] = 3;
				ia[k] = ii - 1;
				a[k] = weight_acceleration / dtime_d_sq;
				ia[k + 1] = ii;
				a[k + 1] = -2.0 * weight_acceleration / dtime_d_sq;
				ia[k + 2] = ii + 1;
				a[k + 2] = weight_acceleration / dtime_d_sq;
				nr++;
			}
		}

		/* set message */
		sprintf(string, "Inverting %dX%d for smooth latitude...", nc, nr);
		do_message_on(string);

		/* compute upper bound on maximum eigenvalue */
		ncyc = 0;
		nsig = 0;
		lspeig(a, ia, nia, nnz, nc, nr, ncyc, &nsig, x, dx, sigma, work, &smax, &err, &sup);
		supt = smax + err;
		if (sup > supt)
			supt = sup;
		if (verbose > 1)
			fprintf(stderr, "Initial lspeig: %g %g %g %g\n", sup, smax, err, supt);
		ncyc = 16;
		for (int i = 0; i < 4; i++) {
			lspeig(a, ia, nia, nnz, nc, nr, ncyc, &nsig, x, dx, sigma, work, &smax, &err, &sup);
			supt = smax + err;
			if (sup > supt)
				supt = sup;
			if (verbose > 1)
				fprintf(stderr, "lspeig[%d]: %g %g %g %g\n", i, sup, smax, err, supt);
		}

		/* calculate chebyshev factors (errlsq is the theoretical error) */
		slo = supt / bandwidth;
		chebyu(sigma, ncycle, supt, slo, work);
		errlsq = errlim(sigma, ncycle, supt, slo);
		if (verbose > 1)
			fprintf(stderr, "Theoretical error: %f\n", errlsq);
		if (verbose > 1)
			for (int i = 0; i < ncycle; i++)
				fprintf(stderr, "sigma[%d]: %f\n", i, sigma[i]);

		/* solve the problem */
		for (int i = 0; i < nc; i++)
			x[i] = 0.0;
		lsqup(a, ia, nia, nnz, nc, nr, x, dx, d, 0, NULL, NULL, ncycle, sigma);

		/* generate solution */
		for (int i = current_id; i < current_id + nplot; i++) {
			const int ii = i - current_id;
			ping[i].lat_dr = lat_avg + mtodeglat * x[ii];
		}

		/* make flagged ends of data flat */
		for (int i = current_id; i < first; i++) {
			const int ii = first - current_id;
			ping[i].lat_dr = lat_avg + mtodeglat * x[ii];
		}
		for (int i = last + 1; i < current_id + nplot; i++) {
			const int ii = last - current_id;
			ping[i].lat_dr = lat_avg + mtodeglat * x[ii];
		}

		/* deallocate arrays */
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&a, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&ia, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&nia, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&d, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&x, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&nx, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&dx, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&sigma, &error);
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)&work, &error);

		/* turn message off */
		do_message_off();
	}

	/* if error initializing memory then don't invert */
	else if (status != MB_SUCCESS) {
		/* free whatever was allocated before the failure */
		mb_freed(verbose, __FILE__, __LINE__, (void **)&a, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&ia, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&nia, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&d, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&x, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&nx, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&dx, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&sigma, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&work, &error);

		mb_error(verbose, error, &message);
		fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
		do_error_dialog("Unable to invert for smooth", "navigation due to a memory", "allocation error!");
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:       %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_all() {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* figure out which pings to plot */
	nplot = 0;
	if (data_show_size > 0 && nbuff > 0) {
		plot_start_time = ping[current_id].file_time_d;
		plot_end_time = plot_start_time + data_show_size;
		for (int i = current_id; i < nbuff; i++)
			if (ping[i].file_time_d <= plot_end_time)
				nplot++;
	} else if (nbuff > 0) {
		plot_start_time = ping[0].file_time_d;
		plot_end_time = ping[nbuff - 1].file_time_d;
		data_show_size = plot_end_time - plot_start_time + 1;
		if (data_show_max < data_show_size)
			data_show_max = data_show_size;
		nplot = nbuff;
	}

	/* deselect data outside plots */
	for (int i = 0; i < current_id; i++) {
		ping[i].tint_select = false;
		ping[i].lon_select = false;
		ping[i].lat_select = false;
		ping[i].speed_select = false;
		ping[i].heading_select = false;
		ping[i].draft_select = false;
	}
	for (int i = current_id + nplot; i < nbuff; i++) {
		ping[i].tint_select = false;
		ping[i].lon_select = false;
		ping[i].lat_select = false;
		ping[i].speed_select = false;
		ping[i].heading_select = false;
		ping[i].draft_select = false;
	}

	/* don't try to plot if no data */
	int status = MB_SUCCESS;
	if (nplot > 0) {
		/* find min max values */
		double time_min = plot_start_time;
		double time_max = plot_end_time;
		double tint_min = ping[current_id].tint;
		double tint_max = ping[current_id].tint;
		double lon_min = ping[current_id].lon;
		double lon_max = ping[current_id].lon;
		double lat_min = ping[current_id].lat;
		double lat_max = ping[current_id].lat;
		double speed_min = 0.0;
		double speed_max = ping[current_id].speed;
		double heading_min = ping[current_id].heading;
		double heading_max = ping[current_id].heading;
		double draft_min = ping[current_id].draft;
		double draft_max = ping[current_id].draft;
		double roll_min = ping[current_id].roll;
		double roll_max = ping[current_id].roll;
		double pitch_min = ping[current_id].pitch;
		double pitch_max = ping[current_id].pitch;
		double heave_min = ping[current_id].heave;
		double heave_max = ping[current_id].heave;
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			tint_min = MIN(ping[i].tint, tint_min);
			tint_max = MAX(ping[i].tint, tint_max);
			if (plot_tint_org) {
				tint_min = MIN(ping[i].tint_org, tint_min);
				tint_max = MAX(ping[i].tint_org, tint_max);
			}
			lon_min = MIN(ping[i].lon, lon_min);
			lon_max = MAX(ping[i].lon, lon_max);
			if (plot_lon_org) {
				lon_min = MIN(ping[i].lon_org, lon_min);
				lon_max = MAX(ping[i].lon_org, lon_max);
			}
			if (model_mode != MODEL_MODE_OFF && plot_lon_dr) {
				lon_min = MIN(ping[i].lon_dr, lon_min);
				lon_max = MAX(ping[i].lon_dr, lon_max);
			}
			lat_min = MIN(ping[i].lat, lat_min);
			lat_max = MAX(ping[i].lat, lat_max);
			if (plot_lat_org) {
				lat_min = MIN(ping[i].lat_org, lat_min);
				lat_max = MAX(ping[i].lat_org, lat_max);
			}
			if (model_mode != MODEL_MODE_OFF && plot_lat_dr) {
				lat_min = MIN(ping[i].lat_dr, lat_min);
				lat_max = MAX(ping[i].lat_dr, lat_max);
			}
			speed_min = MIN(ping[i].speed, speed_min);
			speed_max = MAX(ping[i].speed, speed_max);
			if (plot_speed_org) {
				speed_min = MIN(ping[i].speed_org, speed_min);
				speed_max = MAX(ping[i].speed_org, speed_max);
			}
			if (plot_smg) {
				speed_min = MIN(ping[i].speed_made_good, speed_min);
				speed_max = MAX(ping[i].speed_made_good, speed_max);
			}
			heading_min = MIN(ping[i].heading, heading_min);
			heading_max = MAX(ping[i].heading, heading_max);
			if (plot_heading_org) {
				heading_min = MIN(ping[i].heading_org, heading_min);
				heading_max = MAX(ping[i].heading_org, heading_max);
			}
			if (plot_cmg) {
				heading_min = MIN(ping[i].course_made_good, heading_min);
				heading_max = MAX(ping[i].course_made_good, heading_max);
			}
			draft_min = MIN(ping[i].draft, draft_min);
			draft_max = MAX(ping[i].draft, draft_max);
			if (plot_draft_org) {
				draft_min = MIN(ping[i].draft_org, draft_min);
				draft_max = MAX(ping[i].draft_org, draft_max);
			}
			roll_min = MIN(ping[i].roll, roll_min);
			roll_max = MAX(ping[i].roll, roll_max);
			pitch_min = MIN(ping[i].pitch, pitch_min);
			pitch_max = MAX(ping[i].pitch, pitch_max);
			heave_min = MIN(ping[i].heave, heave_min);
			heave_max = MAX(ping[i].heave, heave_max);
		}

		/* scale the min max a bit larger so all points fit on plots */
		double center = 0.5 * (time_min + time_max);
		double range = 0.51 * (time_max - time_min);
		time_min = center - range;
		time_max = center + range;
		center = 0.5 * (tint_min + tint_max);
		range = 0.55 * (tint_max - tint_min);
		tint_min = center - range;
		tint_max = center + range;
		center = 0.5 * (lon_min + lon_max);
		range = 0.55 * (lon_max - lon_min);
		lon_min = center - range;
		lon_max = center + range;
		center = 0.5 * (lat_min + lat_max);
		range = 0.55 * (lat_max - lat_min);
		lat_min = center - range;
		lat_max = center + range;
		if (speed_min < 0.0) {
			center = 0.5 * (speed_min + speed_max);
			range = 0.55 * (speed_max - speed_min);
			speed_min = center - range;
			speed_max = center + range;
		}
		else
			speed_max = 1.05 * speed_max;
		center = 0.5 * (heading_min + heading_max);
		range = 0.55 * (heading_max - heading_min);
		heading_min = center - range;
		heading_max = center + range;
		center = 0.5 * (draft_min + draft_max);
		range = 0.55 * (draft_max - draft_min);
		draft_min = center - range;
		draft_max = center + range;
		roll_max = 1.1 * MAX(fabs(roll_min), fabs(roll_max));
		roll_min = -roll_max;
		pitch_max = 1.1 * MAX(fabs(pitch_min), fabs(pitch_max));
		pitch_min = -pitch_max;
		heave_max = 1.1 * MAX(fabs(heave_min), fabs(heave_max));
		heave_min = -heave_max;

		/* make sure lon and lat scaled the same if both plotted */
		if (plot_lon && plot_lat) {
			if ((lon_max - lon_min) > (lat_max - lat_min)) {
				center = 0.5 * (lat_min + lat_max);
				lat_min = center - 0.5 * (lon_max - lon_min);
				lat_max = center + 0.5 * (lon_max - lon_min);
			}
			else {
				center = 0.5 * (lon_min + lon_max);
				lon_min = center - 0.5 * (lat_max - lat_min);
				lon_max = center + 0.5 * (lat_max - lat_min);
			}
		}

		/* make sure min max values aren't too small */
		if ((tint_max - tint_min) < 0.01) {
			center = 0.5 * (tint_min + tint_max);
			tint_min = center - 0.005;
			tint_max = center + 0.005;
		}
		if ((lon_max - lon_min) < 0.001) {
			center = 0.5 * (lon_min + lon_max);
			lon_min = center - 0.0005;
			lon_max = center + 0.0005;
		}
		if ((lat_max - lat_min) < 0.001) {
			center = 0.5 * (lat_min + lat_max);
			lat_min = center - 0.0005;
			lat_max = center + 0.0005;
		}
		if (speed_max < 10.0)
			speed_max = 10.0;
		if ((heading_max - heading_min) < 10.0) {
			center = 0.5 * (heading_min + heading_max);
			heading_min = center - 5;
			heading_max = center + 5;
		}
		if ((draft_max - draft_min) < 0.1) {
			center = 0.5 * (draft_min + draft_max);
			draft_min = center - 0.05;
			draft_max = center + 0.05;
		}
		if ((roll_max - roll_min) < 2.0) {
			center = 0.5 * (roll_min + roll_max);
			roll_min = center - 1;
			roll_max = center + 1;
		}
		if ((pitch_max - pitch_min) < 2.0) {
			center = 0.5 * (pitch_min + pitch_max);
			pitch_min = center - 1;
			pitch_max = center + 1;
		}
		if ((heave_max - heave_min) < 0.02) {
			center = 0.5 * (heave_min + heave_max);
			heave_min = center - 0.01;
			heave_max = center + 0.01;
		}

		/* print out information */
		if (verbose >= 2) {
			fprintf(stderr, "\n%d data records set for plotting (%d desired)\n", nplot, data_show_size);
			for (int i = current_id; i < current_id + nplot; i++)
				fprintf(stderr,
				        "dbg5       %4d %4d %4d  %d/%d/%d %2.2d:%2.2d:%2.2d.%6.6d  %11.6f  %11.6f  %11.6f  %11.6f %11.6f %5.2f "
				        "%5.1f %5.1f %5.1f %5.1f %5.1f\n",
				        i, ping[i].id, ping[i].record, ping[i].time_i[1], ping[i].time_i[2], ping[i].time_i[0], ping[i].time_i[3],
				        ping[i].time_i[4], ping[i].time_i[5], ping[i].time_i[6], ping[i].time_d, ping[i].file_time_d,
				        ping[i].tint, ping[i].lon, ping[i].lat, ping[i].speed, ping[i].heading, ping[i].draft, ping[i].roll,
				        ping[i].pitch, ping[i].heave);
		}

		/* get plot margins */
		const int margin_x = plot_width / 10;
		const int margin_y = plot_height / 6;

		/* get date at start of file */
		int xtime_i[7];
		mb_get_date(verbose, file_start_time_d + plot_start_time, xtime_i);

		/* figure out how many plots to make */
		number_plots = 0;
		if (plot_tint) {
			mbnavplot[number_plots].type = PLOT_TINT;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = tint_min;
			mbnavplot[number_plots].ymax = tint_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 5.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "dT");
			sprintf(mbnavplot[number_plots].ylabel2, "(seconds)");
			number_plots++;
		}
		if (plot_lon) {
			mbnavplot[number_plots].type = PLOT_LONGITUDE;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = lon_min;
			mbnavplot[number_plots].ymax = lon_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Longitude");
			sprintf(mbnavplot[number_plots].ylabel2, "(degrees)");
			number_plots++;
		}
		if (plot_lat) {
			mbnavplot[number_plots].type = PLOT_LATITUDE;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = lat_min;
			mbnavplot[number_plots].ymax = lat_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Latitude");
			sprintf(mbnavplot[number_plots].ylabel2, "(degrees)");
			number_plots++;
		}
		if (plot_speed) {
			mbnavplot[number_plots].type = PLOT_SPEED;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = speed_min;
			mbnavplot[number_plots].ymax = speed_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 10;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Speed");
			sprintf(mbnavplot[number_plots].ylabel2, "(km/hr)");
			number_plots++;
		}
		if (plot_heading) {
			mbnavplot[number_plots].type = PLOT_HEADING;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = heading_min;
			mbnavplot[number_plots].ymax = heading_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Heading");
			sprintf(mbnavplot[number_plots].ylabel2, "(degrees)");
			number_plots++;
		}
		if (plot_draft) {
			mbnavplot[number_plots].type = PLOT_DRAFT;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = draft_max;
			mbnavplot[number_plots].ymax = draft_min;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Sonar Depth");
			sprintf(mbnavplot[number_plots].ylabel2, "(meters)");
			number_plots++;
		}
		if (plot_roll) {
			mbnavplot[number_plots].type = PLOT_ROLL;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = roll_min;
			mbnavplot[number_plots].ymax = roll_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Roll");
			sprintf(mbnavplot[number_plots].ylabel2, "(degrees)");
			number_plots++;
		}
		if (plot_pitch) {
			mbnavplot[number_plots].type = PLOT_PITCH;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = pitch_min;
			mbnavplot[number_plots].ymax = pitch_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Pitch");
			sprintf(mbnavplot[number_plots].ylabel2, "(degrees)");
			number_plots++;
		}
		if (plot_heave) {
			mbnavplot[number_plots].type = PLOT_HEAVE;
			mbnavplot[number_plots].ixmin = 1.25 * margin_x;
			mbnavplot[number_plots].ixmax = plot_width - margin_x / 2;
			mbnavplot[number_plots].iymin = plot_height - margin_y + number_plots * plot_height;
			mbnavplot[number_plots].iymax = number_plots * plot_height + margin_y;
			mbnavplot[number_plots].xmin = time_min;
			mbnavplot[number_plots].xmax = time_max;
			mbnavplot[number_plots].ymin = heave_min;
			mbnavplot[number_plots].ymax = heave_max;
			mbnavplot[number_plots].xscale = (mbnavplot[number_plots].ixmax - mbnavplot[number_plots].ixmin) /
			                                 (mbnavplot[number_plots].xmax - mbnavplot[number_plots].xmin);
			mbnavplot[number_plots].yscale = (mbnavplot[number_plots].iymax - mbnavplot[number_plots].iymin) /
			                                 (mbnavplot[number_plots].ymax - mbnavplot[number_plots].ymin);
			mbnavplot[number_plots].xinterval = 100.0;
			mbnavplot[number_plots].yinterval = 45.0;
			sprintf(mbnavplot[number_plots].xlabel, "Time (HH:MM:SS.SSS) beginning on %2.2d/%2.2d/%4.4d", xtime_i[1], xtime_i[2],
			        xtime_i[0]);
			sprintf(mbnavplot[number_plots].ylabel1, "Heave");
			sprintf(mbnavplot[number_plots].ylabel2, "(meters)");
			number_plots++;
		}

		/* clear screen */
		status = mbnavedit_clear_screen();

		/* do plots */
		for (int iplot = 0; iplot < number_plots; iplot++) {
			/* get center locations */
			const int center_x = (mbnavplot[iplot].ixmin + mbnavplot[iplot].ixmax) / 2;
			const int center_y = (mbnavplot[iplot].iymin + mbnavplot[iplot].iymax) / 2;

			/* plot filename */
			char string[MB_PATH_MAXLINE+30];
			sprintf(string, "Data File: %s", ifile);
			int swidth;
			int sascent;
			int sdescent;
			xg_justify(mbnavedit_xgid, string, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, center_x - swidth / 2, mbnavplot[iplot].iymax - 5 * sascent / 2, string,
			              pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

			/* get bounds for position bar */
			int fpx = center_x - 2 * margin_x + (4 * margin_x * current_id) / nbuff;
			const int fpdx = MAX(((4 * margin_x * nplot) / nbuff), 5);
			const int fpy = mbnavplot[iplot].iymax - 2 * sascent;
			const int fpdy = sascent;
			if (fpdx > 4 * margin_x)
				fpx = center_x + 2 * margin_x - fpdx;

			/* plot file position bar */
			xg_drawrectangle(mbnavedit_xgid, center_x - 2 * margin_x, fpy, 4 * margin_x, fpdy, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			xg_drawrectangle(mbnavedit_xgid, center_x - 2 * margin_x - 1, fpy - 1, 4 * margin_x + 2, fpdy + 2,
			                 pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			xg_fillrectangle(mbnavedit_xgid, fpx, fpy, fpdx, fpdy, pixel_values[MB_COLOR_LIGHTGREY], XG_SOLIDLINE);
			xg_drawrectangle(mbnavedit_xgid, fpx, fpy, fpdx, fpdy, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			sprintf(string, "0 ");
			xg_justify(mbnavedit_xgid, string, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, (int)(center_x - 2 * margin_x - swidth), fpy + sascent, string, pixel_values[MB_COLOR_BLACK],
			              XG_SOLIDLINE);
			sprintf(string, " %d", nbuff);
			xg_drawstring(mbnavedit_xgid, (int)(center_x + 2 * margin_x), fpy + sascent, string, pixel_values[MB_COLOR_BLACK],
			              XG_SOLIDLINE);

			/* plot x label */
			xg_justify(mbnavedit_xgid, mbnavplot[iplot].xlabel, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, (int)(center_x - swidth / 2), (int)(mbnavplot[iplot].iymin + 0.75 * margin_y),
			              mbnavplot[iplot].xlabel, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

			/* plot y labels */
			xg_justify(mbnavedit_xgid, mbnavplot[iplot].ylabel1, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, (int)(mbnavplot[iplot].ixmin - swidth / 2 - 0.75 * margin_x), (int)(center_y - sascent),
			              mbnavplot[iplot].ylabel1, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			xg_justify(mbnavedit_xgid, mbnavplot[iplot].ylabel2, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, (int)(mbnavplot[iplot].ixmin - swidth / 2 - 0.75 * margin_x),
			              (int)(center_y + 2 * sascent), mbnavplot[iplot].ylabel2, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

			/* plot x axis time annotation */
			const double dx = (plot_end_time - plot_start_time) / 5;
			for (int i = 0; i < 6; i++) {
				/* get x position */
				double x = plot_start_time + i * dx;
				const int ix = mbnavplot[iplot].ixmin + mbnavplot[iplot].xscale * (x - mbnavplot[iplot].xmin);
				x += file_start_time_d;

				/* draw tickmarks */
				xg_drawline(mbnavedit_xgid, ix, mbnavplot[iplot].iymin, ix, mbnavplot[iplot].iymin + 5, pixel_values[MB_COLOR_BLACK],
				            XG_SOLIDLINE);

				/* draw annotations */
				mb_get_date(verbose, x, xtime_i);
				sprintf(string, "%2.2d:%2.2d:%2.2d.%3.3d", xtime_i[3], xtime_i[4], xtime_i[5], (int)(0.001 * xtime_i[6]));
				xg_justify(mbnavedit_xgid, string, &swidth, &sascent, &sdescent);
				xg_drawstring(mbnavedit_xgid, (int)(ix - swidth / 2), (int)(mbnavplot[iplot].iymin + 5 + 1.75 * sascent), string,
				              pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			}

			/* plot y min max values */
			char yformat[10];
			if (mbnavplot[iplot].type == PLOT_LONGITUDE || mbnavplot[iplot].type == PLOT_LATITUDE)
				strcpy(yformat, "%11.6f");
			else
				strcpy(yformat, "%6.2f");
			sprintf(string, yformat, mbnavplot[iplot].ymin);
			xg_justify(mbnavedit_xgid, string, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, (int)(mbnavplot[iplot].ixmin - swidth - 0.03 * margin_x),
			              (int)(mbnavplot[iplot].iymin + 0.5 * sascent), string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			sprintf(string, yformat, mbnavplot[iplot].ymax);
			xg_justify(mbnavedit_xgid, string, &swidth, &sascent, &sdescent);
			xg_drawstring(mbnavedit_xgid, (int)(mbnavplot[iplot].ixmin - swidth - 0.03 * margin_x),
			              (int)(mbnavplot[iplot].iymax + 0.5 * sascent), string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

			/* plot zero values */
			if ((mbnavplot[iplot].ymax > 0.0 && mbnavplot[iplot].ymin < 0.0) ||
			    (mbnavplot[iplot].ymax < 0.0 && mbnavplot[iplot].ymin > 0.0)) {
				if (mbnavplot[iplot].type == PLOT_LONGITUDE || mbnavplot[iplot].type == PLOT_LATITUDE)
					strcpy(yformat, "%11.6f");
				else
					strcpy(yformat, "%6.2f");
				sprintf(string, yformat, 0.0);
				xg_justify(mbnavedit_xgid, string, &swidth, &sascent, &sdescent);
				const int iyzero = mbnavplot[iplot].iymin - mbnavplot[iplot].yscale * mbnavplot[iplot].ymin;
				xg_drawstring(mbnavedit_xgid, (int)(mbnavplot[iplot].ixmin - swidth - 0.03 * margin_x),
				              (int)(iyzero + 0.5 * sascent), string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
				xg_drawline(mbnavedit_xgid, mbnavplot[iplot].ixmin, iyzero, mbnavplot[iplot].ixmax, iyzero, pixel_values[MB_COLOR_BLACK],
				            XG_DASHLINE);
			}

			/* plot bounding box */
			xg_drawrectangle(mbnavedit_xgid, mbnavplot[iplot].ixmin, mbnavplot[iplot].iymax,
			                 mbnavplot[iplot].ixmax - mbnavplot[iplot].ixmin, mbnavplot[iplot].iymin - mbnavplot[iplot].iymax,
			                 pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			xg_drawrectangle(mbnavedit_xgid, mbnavplot[iplot].ixmin - 1, mbnavplot[iplot].iymax - 1,
			                 mbnavplot[iplot].ixmax - mbnavplot[iplot].ixmin + 2,
			                 mbnavplot[iplot].iymin - mbnavplot[iplot].iymax + 2, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

			/* now plot the data */
			if (mbnavplot[iplot].type == PLOT_TINT)
				mbnavedit_plot_tint(iplot);
			else if (mbnavplot[iplot].type == PLOT_LONGITUDE)
				mbnavedit_plot_lon(iplot);
			else if (mbnavplot[iplot].type == PLOT_LATITUDE)
				mbnavedit_plot_lat(iplot);
			else if (mbnavplot[iplot].type == PLOT_SPEED)
				mbnavedit_plot_speed(iplot);
			else if (mbnavplot[iplot].type == PLOT_HEADING)
				mbnavedit_plot_heading(iplot);
			else if (mbnavplot[iplot].type == PLOT_DRAFT)
				mbnavedit_plot_draft(iplot);
			else if (mbnavplot[iplot].type == PLOT_ROLL)
				mbnavedit_plot_roll(iplot);
			else if (mbnavplot[iplot].type == PLOT_PITCH)
				mbnavedit_plot_pitch(iplot);
			else if (mbnavplot[iplot].type == PLOT_HEAVE)
				mbnavedit_plot_heave(iplot);
		}
	}

	/* set status */
	if (nplot > 0)
		status = MB_SUCCESS;
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_tint(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* get scaling values */
	const int ixmin = mbnavplot[iplot].ixmin;
	const int iymin = mbnavplot[iplot].iymin;
	const double xmin = mbnavplot[iplot].xmin;
	const double ymin = mbnavplot[iplot].ymin;
	const double xscale = mbnavplot[iplot].xscale;
	const double yscale = mbnavplot[iplot].yscale;

	/* plot original expected time data */
	if (plot_tint_org) {
		int tint_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int tint_y1 = iymin + yscale * (ping[current_id].tint_org - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int tint_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int tint_y2 = iymin + yscale * (ping[i].tint_org - ymin);
			xg_drawline(mbnavedit_xgid, tint_x1, tint_y1, tint_x2, tint_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			tint_x1 = tint_x2;
			tint_y1 = tint_y2;
		}
	}

	/* plot basic expected time data */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].tint_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].tint_y = iymin + yscale * (ping[i].tint - ymin);
		if (ping[i].tint_select)
			xg_drawrectangle(mbnavedit_xgid, ping[i].tint_x - 2, ping[i].tint_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
		else if (ping[i].tint != ping[i].tint_org)
			xg_drawrectangle(mbnavedit_xgid, ping[i].tint_x - 2, ping[i].tint_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
		else
			xg_fillrectangle(mbnavedit_xgid, ping[i].tint_x - 2, ping[i].tint_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_lon(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* get scaling values */
	const int ixmin = mbnavplot[iplot].ixmin;
	const int iymin = mbnavplot[iplot].iymin;
	const double xmin = mbnavplot[iplot].xmin;
	const double ymin = mbnavplot[iplot].ymin;
	const double xscale = mbnavplot[iplot].xscale;
	const double yscale = mbnavplot[iplot].yscale;

	/* plot original longitude data */
	if (plot_lon_org) {
		int lon_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int lon_y1 = iymin + yscale * (ping[current_id].lon_org - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int lon_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int lon_y2 = iymin + yscale * (ping[i].lon_org - ymin);
			xg_drawline(mbnavedit_xgid, lon_x1, lon_y1, lon_x2, lon_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			lon_x1 = lon_x2;
			lon_y1 = lon_y2;
		}
	}

	/* plot dr longitude data */
	if (model_mode != MODEL_MODE_OFF && plot_lon_dr) {
		int lon_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int lon_y1 = iymin + yscale * (ping[current_id].lon_dr - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int lon_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int lon_y2 = iymin + yscale * (ping[i].lon_dr - ymin);
			xg_drawline(mbnavedit_xgid, lon_x1, lon_y1, lon_x2, lon_y2, pixel_values[MB_COLOR_BLUE], XG_SOLIDLINE);
			lon_x1 = lon_x2;
			lon_y1 = lon_y2;
		}
	}

	/* plot flagged longitude data first so it is overlain by all else */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].lon_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].lon_y = iymin + yscale * (ping[i].lon - ymin);
		if (ping[i].lonlat_flag)
			xg_drawrectangle(mbnavedit_xgid, ping[i].lon_x - 2, ping[i].lon_y - 2, 4, 4, pixel_values[MB_COLOR_ORANGE], XG_SOLIDLINE);
	}

	/* plot basic longitude data */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].lon_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].lon_y = iymin + yscale * (ping[i].lon - ymin);
		if (ping[i].lon_select)
			xg_drawrectangle(mbnavedit_xgid, ping[i].lon_x - 2, ping[i].lon_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
		else if (ping[i].lonlat_flag) {
			;
		}
		else if (ping[i].lon != ping[i].lon_org)
			xg_drawrectangle(mbnavedit_xgid, ping[i].lon_x - 2, ping[i].lon_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
		else
			xg_fillrectangle(mbnavedit_xgid, ping[i].lon_x - 2, ping[i].lon_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_lat(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* get scaling values */
	const int ixmin = mbnavplot[iplot].ixmin;
	const int iymin = mbnavplot[iplot].iymin;
	const double xmin = mbnavplot[iplot].xmin;
	const double ymin = mbnavplot[iplot].ymin;
	const double xscale = mbnavplot[iplot].xscale;
	const double yscale = mbnavplot[iplot].yscale;

	/* plot original latitude data */
	if (plot_lat_org) {
		int lat_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int lat_y1 = iymin + yscale * (ping[current_id].lat_org - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int lat_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int lat_y2 = iymin + yscale * (ping[i].lat_org - ymin);
			xg_drawline(mbnavedit_xgid, lat_x1, lat_y1, lat_x2, lat_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			lat_x1 = lat_x2;
			lat_y1 = lat_y2;
		}
	}

	/* plot dr latitude data */
	if (model_mode != MODEL_MODE_OFF && plot_lat_dr) {
		int lat_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int lat_y1 = iymin + yscale * (ping[current_id].lat_dr - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int lat_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int lat_y2 = iymin + yscale * (ping[i].lat_dr - ymin);
			xg_drawline(mbnavedit_xgid, lat_x1, lat_y1, lat_x2, lat_y2, pixel_values[MB_COLOR_BLUE], XG_SOLIDLINE);
			lat_x1 = lat_x2;
			lat_y1 = lat_y2;
		}
	}

	/* plot flagged latitude data first so it is overlain by all else */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].lat_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].lat_y = iymin + yscale * (ping[i].lat - ymin);
		if (ping[i].lonlat_flag)
			xg_drawrectangle(mbnavedit_xgid, ping[i].lat_x - 2, ping[i].lat_y - 2, 4, 4, pixel_values[MB_COLOR_ORANGE], XG_SOLIDLINE);
	}

	/* plot basic latitude data */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].lat_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].lat_y = iymin + yscale * (ping[i].lat - ymin);
		if (ping[i].lat_select)
			xg_drawrectangle(mbnavedit_xgid, ping[i].lat_x - 2, ping[i].lat_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
		else if (ping[i].lonlat_flag) {
			;
		}
		else if (ping[i].lat != ping[i].lat_org)
			xg_drawrectangle(mbnavedit_xgid, ping[i].lat_x - 2, ping[i].lat_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
		else
			xg_fillrectangle(mbnavedit_xgid, ping[i].lat_x - 2, ping[i].lat_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_speed(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* get scaling values */
	const int ixmin = mbnavplot[iplot].ixmin;
	const int iymin = mbnavplot[iplot].iymin;
	const double xmin = mbnavplot[iplot].xmin;
	const double ymin = mbnavplot[iplot].ymin;
	const double xscale = mbnavplot[iplot].xscale;
	const double yscale = mbnavplot[iplot].yscale;

	/* plot original speed data */
	if (plot_speed_org) {
		int speed_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int speed_y1 = iymin + yscale * (ping[current_id].speed - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int speed_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int speed_y2 = iymin + yscale * (ping[i].speed_org - ymin);
			xg_drawline(mbnavedit_xgid, speed_x1, speed_y1, speed_x2, speed_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			speed_x1 = speed_x2;
			speed_y1 = speed_y2;
		}
	}

	/* plot speed made good data */
	if (plot_smg) {
		int speed_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int speed_y1 = iymin + yscale * (ping[current_id].speed_made_good - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int speed_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int speed_y2 = iymin + yscale * (ping[i].speed_made_good - ymin);
			xg_drawline(mbnavedit_xgid, speed_x1, speed_y1, speed_x2, speed_y2, pixel_values[MB_COLOR_BLUE], XG_SOLIDLINE);
			speed_x1 = speed_x2;
			speed_y1 = speed_y2;
		}
	}

	/* plot basic speed data */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].speed_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].speed_y = iymin + yscale * (ping[i].speed - ymin);
		if (ping[i].speed_select)
			xg_drawrectangle(mbnavedit_xgid, ping[i].speed_x - 2, ping[i].speed_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
		else if (ping[i].speed != ping[i].speed_org)
			xg_drawrectangle(mbnavedit_xgid, ping[i].speed_x - 2, ping[i].speed_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
		else
			xg_fillrectangle(mbnavedit_xgid, ping[i].speed_x - 2, ping[i].speed_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}

	int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_heading(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* get scaling values */
	const int ixmin = mbnavplot[iplot].ixmin;
	const int iymin = mbnavplot[iplot].iymin;
	const double xmin = mbnavplot[iplot].xmin;
	const double ymin = mbnavplot[iplot].ymin;
	const double xscale = mbnavplot[iplot].xscale;
	const double yscale = mbnavplot[iplot].yscale;

	/* plot original heading data */
	if (plot_heading_org) {
		int heading_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int heading_y1 = iymin + yscale * (ping[current_id].heading - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int heading_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int heading_y2 = iymin + yscale * (ping[i].heading_org - ymin);
			xg_drawline(mbnavedit_xgid, heading_x1, heading_y1, heading_x2, heading_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			heading_x1 = heading_x2;
			heading_y1 = heading_y2;
		}
	}

	/* plot course made good data */
	if (plot_cmg) {
		int heading_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int heading_y1 = iymin + yscale * (ping[current_id].course_made_good - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int heading_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int heading_y2 = iymin + yscale * (ping[i].course_made_good - ymin);
			xg_drawline(mbnavedit_xgid, heading_x1, heading_y1, heading_x2, heading_y2, pixel_values[MB_COLOR_BLUE], XG_SOLIDLINE);
			heading_x1 = heading_x2;
			heading_y1 = heading_y2;
		}
	}

	/* plot basic heading data */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].heading_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].heading_y = iymin + yscale * (ping[i].heading - ymin);
		if (ping[i].heading_select)
			xg_drawrectangle(mbnavedit_xgid, ping[i].heading_x - 2, ping[i].heading_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
		else if (ping[i].heading != ping[i].heading_org)
			xg_drawrectangle(mbnavedit_xgid, ping[i].heading_x - 2, ping[i].heading_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE],
			                 XG_SOLIDLINE);
		else
			xg_fillrectangle(mbnavedit_xgid, ping[i].heading_x - 2, ping[i].heading_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK],
			                 XG_SOLIDLINE);
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_draft(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* get scaling values */
	const int ixmin = mbnavplot[iplot].ixmin;
	const int iymin = mbnavplot[iplot].iymin;
	const double xmin = mbnavplot[iplot].xmin;
	const double ymin = mbnavplot[iplot].ymin;
	const double xscale = mbnavplot[iplot].xscale;
	const double yscale = mbnavplot[iplot].yscale;

	/* plot original draft data */
	if (plot_draft_org) {
		int draft_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int draft_y1 = iymin + yscale * (ping[current_id].draft - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int draft_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int draft_y2 = iymin + yscale * (ping[i].draft_org - ymin);
			xg_drawline(mbnavedit_xgid, draft_x1, draft_y1, draft_x2, draft_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			draft_x1 = draft_x2;
			draft_y1 = draft_y2;
		}
	}

	/* plot basic draft data */
	for (int i = current_id; i < current_id + nplot; i++) {
		ping[i].draft_x = ixmin + xscale * (ping[i].file_time_d - xmin);
		ping[i].draft_y = iymin + yscale * (ping[i].draft - ymin);
		if (ping[i].draft_select)
			xg_drawrectangle(mbnavedit_xgid, ping[i].draft_x - 2, ping[i].draft_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
		else if (ping[i].draft != ping[i].draft_org)
			xg_drawrectangle(mbnavedit_xgid, ping[i].draft_x - 2, ping[i].draft_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
		else
			xg_fillrectangle(mbnavedit_xgid, ping[i].draft_x - 2, ping[i].draft_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_roll(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* plot roll data */
	if (plot_roll) {
		/* get scaling values */
		const int ixmin = mbnavplot[iplot].ixmin;
		const int iymin = mbnavplot[iplot].iymin;
		const double xmin = mbnavplot[iplot].xmin;
		const double ymin = mbnavplot[iplot].ymin;
		const double xscale = mbnavplot[iplot].xscale;
		const double yscale = mbnavplot[iplot].yscale;

		int roll_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int roll_y1 = iymin + yscale * (ping[current_id].roll - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int roll_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int roll_y2 = iymin + yscale * (ping[i].roll - ymin);
			xg_drawline(mbnavedit_xgid, roll_x1, roll_y1, roll_x2, roll_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			roll_x1 = roll_x2;
			roll_y1 = roll_y2;
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_pitch(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* plot pitch data */
	if (plot_pitch) {
		/* get scaling values */
		const int ixmin = mbnavplot[iplot].ixmin;
		const int iymin = mbnavplot[iplot].iymin;
		const double xmin = mbnavplot[iplot].xmin;
		const double ymin = mbnavplot[iplot].ymin;
		const double xscale = mbnavplot[iplot].xscale;
		const double yscale = mbnavplot[iplot].yscale;

		int pitch_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int pitch_y1 = iymin + yscale * (ping[current_id].pitch - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int pitch_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int pitch_y2 = iymin + yscale * (ping[i].pitch - ymin);
			xg_drawline(mbnavedit_xgid, pitch_x1, pitch_y1, pitch_x2, pitch_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			pitch_x1 = pitch_x2;
			pitch_y1 = pitch_y2;
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_heave(int iplot) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
	}

	/* plot heave data */
	if (plot_heave) {
		/* get scaling values */
		const int ixmin = mbnavplot[iplot].ixmin;
		const int iymin = mbnavplot[iplot].iymin;
		const double xmin = mbnavplot[iplot].xmin;
		const double ymin = mbnavplot[iplot].ymin;
		const double xscale = mbnavplot[iplot].xscale;
		const double yscale = mbnavplot[iplot].yscale;

		int heave_x1 = ixmin + xscale * (ping[current_id].file_time_d - xmin);
		int heave_y1 = iymin + yscale * (ping[current_id].heave - ymin);
		for (int i = current_id + 1; i < current_id + nplot; i++) {
			const int heave_x2 = ixmin + xscale * (ping[i].file_time_d - xmin);
			const int heave_y2 = iymin + yscale * (ping[i].heave - ymin);
			xg_drawline(mbnavedit_xgid, heave_x1, heave_y1, heave_x2, heave_y2, pixel_values[MB_COLOR_GREEN], XG_SOLIDLINE);
			heave_x1 = heave_x2;
			heave_y1 = heave_y2;
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_tint_value(int iplot, int iping) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
		fprintf(stderr, "dbg2       iping:       %d\n", iping);
	}

	/* unplot basic expected time data value */
	xg_drawrectangle(mbnavedit_xgid, ping[iping].tint_x - 2, ping[iping].tint_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);
	xg_fillrectangle(mbnavedit_xgid, ping[iping].tint_x - 2, ping[iping].tint_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);

	/* replot basic expected time data value */
	if (ping[iping].tint_select)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].tint_x - 2, ping[iping].tint_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
	else if (ping[iping].tint != ping[iping].tint_org)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].tint_x - 2, ping[iping].tint_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE],
		                 XG_SOLIDLINE);
	else
		xg_fillrectangle(mbnavedit_xgid, ping[iping].tint_x - 2, ping[iping].tint_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_lon_value(int iplot, int iping) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
		fprintf(stderr, "dbg2       iping:       %d\n", iping);
	}

	/* unplot basic lon data value */
	xg_drawrectangle(mbnavedit_xgid, ping[iping].lon_x - 2, ping[iping].lon_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);
	xg_fillrectangle(mbnavedit_xgid, ping[iping].lon_x - 2, ping[iping].lon_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);

	/* replot basic lon data value */
	if (ping[iping].lon_select)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].lon_x - 2, ping[iping].lon_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
	else if (ping[iping].lonlat_flag)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].lon_x - 2, ping[iping].lon_y - 2, 4, 4, pixel_values[MB_COLOR_ORANGE], XG_SOLIDLINE);
	else if (ping[iping].lon != ping[iping].lon_org)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].lon_x - 2, ping[iping].lon_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
	else
		xg_fillrectangle(mbnavedit_xgid, ping[iping].lon_x - 2, ping[iping].lon_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_lat_value(int iplot, int iping) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
		fprintf(stderr, "dbg2       iping:       %d\n", iping);
	}

	/* unplot basic lat data value */
	xg_drawrectangle(mbnavedit_xgid, ping[iping].lat_x - 2, ping[iping].lat_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);
	xg_fillrectangle(mbnavedit_xgid, ping[iping].lat_x - 2, ping[iping].lat_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);

	/* replot basic lat data value */
	if (ping[iping].lat_select)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].lat_x - 2, ping[iping].lat_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
	else if (ping[iping].lonlat_flag)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].lat_x - 2, ping[iping].lat_y - 2, 4, 4, pixel_values[MB_COLOR_ORANGE], XG_SOLIDLINE);
	else if (ping[iping].lat != ping[iping].lat_org)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].lat_x - 2, ping[iping].lat_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE], XG_SOLIDLINE);
	else
		xg_fillrectangle(mbnavedit_xgid, ping[iping].lat_x - 2, ping[iping].lat_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_speed_value(int iplot, int iping) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
		fprintf(stderr, "dbg2       iping:       %d\n", iping);
	}

	/* unplot basic speed data value */
	xg_drawrectangle(mbnavedit_xgid, ping[iping].speed_x - 2, ping[iping].speed_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);
	xg_fillrectangle(mbnavedit_xgid, ping[iping].speed_x - 2, ping[iping].speed_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);

	/* replot basic speed data value */
	if (ping[iping].speed_select)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].speed_x - 2, ping[iping].speed_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
	else if (ping[iping].speed != ping[iping].speed_org)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].speed_x - 2, ping[iping].speed_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE],
		                 XG_SOLIDLINE);
	else
		xg_fillrectangle(mbnavedit_xgid, ping[iping].speed_x - 2, ping[iping].speed_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK],
		                 XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_heading_value(int iplot, int iping) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
		fprintf(stderr, "dbg2       iping:       %d\n", iping);
	}

	/* unplot basic heading data value */
	xg_drawrectangle(mbnavedit_xgid, ping[iping].heading_x - 2, ping[iping].heading_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE],
	                 XG_SOLIDLINE);
	xg_fillrectangle(mbnavedit_xgid, ping[iping].heading_x - 2, ping[iping].heading_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE],
	                 XG_SOLIDLINE);

	/* replot basic heading data value */
	if (ping[iping].heading_select)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].heading_x - 2, ping[iping].heading_y - 2, 4, 4, pixel_values[MB_COLOR_RED],
		                 XG_SOLIDLINE);
	else if (ping[iping].heading != ping[iping].heading_org)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].heading_x - 2, ping[iping].heading_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE],
		                 XG_SOLIDLINE);
	else
		xg_fillrectangle(mbnavedit_xgid, ping[iping].heading_x - 2, ping[iping].heading_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK],
		                 XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbnavedit_plot_draft_value(int iplot, int iping) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       iplot:       %d\n", iplot);
		fprintf(stderr, "dbg2       iping:       %d\n", iping);
	}

	/* unplot basic draft data value */
	xg_drawrectangle(mbnavedit_xgid, ping[iping].draft_x - 2, ping[iping].draft_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);
	xg_fillrectangle(mbnavedit_xgid, ping[iping].draft_x - 2, ping[iping].draft_y - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);

	/* replot basic draft data value */
	if (ping[iping].draft_select)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].draft_x - 2, ping[iping].draft_y - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
	else if (ping[iping].draft != ping[iping].draft_org)
		xg_drawrectangle(mbnavedit_xgid, ping[iping].draft_x - 2, ping[iping].draft_y - 2, 4, 4, pixel_values[MB_COLOR_PURPLE],
		                 XG_SOLIDLINE);
	else
		xg_fillrectangle(mbnavedit_xgid, ping[iping].draft_x - 2, ping[iping].draft_y - 2, 4, 4, pixel_values[MB_COLOR_BLACK],
		                 XG_SOLIDLINE);

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/

/*--------------------------------------------------------------------
 * InteractiveGMT port additions
 *--------------------------------------------------------------------*/

/*--------------------------------------------------------------------
 * lsqup / lspeig and their helpers -- MB-System mbaux/mb_cheb.c, unchanged but for `static`.
 * The mb_cheb library is a least squares matrix solver using Richardson's algorithm with
 * chebyshev acceleration (Olson, A. H., Phys. Earth Planet. Inter., 47, 333-345, 1987). It
 * lives in libmbaux, which the MBIO library does not carry, and only the smooth inversion uses it.
 *--------------------------------------------------------------------*/
/*----------------------------------------------------------------------
 *
 *     least squares solution using richardson's algorithm
 *     with chebyshev acceleration.  the step size is varied to obtain
 *     uniform convergence over a prescribed range of eigenvalues.
 *
 *                             ..... allen h. olson 6-29-85.
 *                                   university of california, san diego
 *
 *     Altered to allow particular values to be fixed.
 *                             ..... David W. Caress 4-14-88
 *----------------------------------------------------------------------
 *
 *                       nc                   ~
 *          given :     sum [ a(j,i) * x(j) ] = d(i)   ;  i=1, ... nr
 *                      j=1
 *                                               t
 *          minimize :  || a*x - d || = (a*x - d) * (a*x -d)
 *
 *----------------------------------------------------------------------*/
/*----------------------------------------------------------------------
 * -------
 *  input
 * -------
 *     a(j,i)  -------  - packed matrix defined above dimensioned at least
 *                        (nnz,nr) note  that the first index is the column
 *                        index! i.e. the matrix is stored in row order.
 *     ia(j,i)  ------  - indices of values in packed matrix a
 *                        i.e. a(j,i) packed = a(j,ia(i,j)) unpacked
 *     nia(i)  -------  - number of nonzero values in each row
 *     nnz   ---------  - number of values in packed rows of a and ia
 *     nc,nr  --------  - number of columns and number of rows of unpacked
 *                        matrix as defined above.
 *     x(1...nc) -----  - initial guess solution; can be set to zero
 *                        or values returned form previous calls to lsquc.
 *     dx(1...nc) ----  - temporary storage array
 *     d(1...nr) -----  - data as defined above
 *     nfix  ---------  - number of solution values to be fixed
 *     ifix  ---------  - idices of fixed values
 *     fix  ----------  - fixed values:  x(ifix(j)) = fix(j)
 *     ncycle  -------  - number of iterations to perform. must be power of 2.
 *     sigma(1..ncycle) - array containing the weights for step sizes.
 *                        see subroutine 'chebyu' for computing these.
 * --------
 *  output
 * --------
 *     x(1..nc)   - solution vector as defined above
 *                  only array x(..) is over-written by lsquc.
 *----------------------------------------------------------------------*/
static void lsqup(const double *a, const int *ia, const int *nia, int nnz, int nc, int nr,
           double *x, double *dx, const double *d, int nfix, const int *ifix,
           const double *fix, int ncycle, const double *sigma)
{
	/* loop over all cycles */
	for (int icyc = 0; icyc < ncycle; icyc++) {
		/* initialize dx */
		for (int j = 0; j < nc; j++)
			dx[j] = 0.0;

		/* loop over each row */
		for (int i = 0; i < nr; i++) {
			double res = 0.0;
			for (int j = 0; j < nia[i]; j++) {
				const int k = nnz * i + j;
				res += a[k] * x[ia[k]];
			}
			res = d[i] - res;
			for (int j = 0; j < nia[i]; j++) {
				const int k = nnz * i + j;
				dx[ia[k]] += res * a[k];
			}
		}

		/* update x */
		for (int j = 0; j < nc; j++)
			x[j] += dx[j] / sigma[icyc];

		/* apply fixed values */
		for (int j = 0; j < nfix; j++)
			x[ifix[j]] = fix[j];
	}
}
/*----------------------------------------------------------------------
 *
 * computes the chebyshev weights with uniform distribution.
 * weights are ordered pair-wise in such a fashion that after an even
 * number of steps they are distributed uniformly on the interval [slo,shi].
 * this ordering provides optimum numerical stability of routine lsquc.
 *
 *                             ..... allen h. olson 6-29-85.
 *                                   university of california, san diego
 *
 *----------------------------------------------------------------------
 * -------
 *  input
 * -------
 *
 *     ncycle   ------  - must be a power of two!  number of iterations.
 *
 *     shi,slo  ------  - high and low limits defining the band of eigenvalues
 *                        to retain in the solution.  shi >= largest eigenvalue
 *                        of the normal equations.
 *     work(1..ncycle)  - work array for sorting array sigma(..).
 *
 * -------
 *  output
 * -------
 *
 *     sigma(1..ncycle) - weights for the step sizes in routine lsquc.
 * -------
 * calls function splits.
 *
 *----------------------------------------------------------------------*/
static void chebyu(double *sigma, int ncycle, double shi, double slo, double *work)
{
	/* set up the chebyshev weights in increasing order */
	for (int i = 0; i < ncycle; i++) {
		sigma[i] = -cos((2 * (i + 1) - 1) * M_PI / 2 / ncycle);
		sigma[i] = (sigma[i] * (shi - slo) + (shi + slo)) / 2;
	}

	/* sort the weights */
	int len = ncycle;
	while (len > 2) {
		const int nsort = ncycle / len;
		for (int is = 0; is < nsort; is++) {
			const int i0 = is * len;
			splits(&sigma[i0], work, len);
		}
		len /= 2;
	}
}
/*----------------------------------------------------------------------*/
static void splits(double *x, double *t, int n) {
	int l = 0;
	for (int i = 0; i < n; i += 2) {
		t[l] = x[i];
		l++;
	}
	for (int i = 1; i < n; i += 2) {
		t[l] = x[i];
		l++;
	}

	const int nb2 = n / 2;
	const int nb2m1 = nb2 - 1;
	if (nb2 >= 2) {
		for (int i = 0; i < nb2; i++) {
			x[i] = t[nb2m1 - i];
		}
		for (int i = nb2; i < n; i++) {
			x[i] = t[i];
		}
	} else {
		for (int i = 0; i < n; i++)
			x[i] = t[i];
	}
}
/*----------------------------------------------------------------------*/
/* returns limit of the maximum theoretical error using chebyshev weights */
static double errlim(double *sigma, int ncycle, double shi, double slo)
{
	double errlim = 1.0;
	const double delta = 0.25 * (shi - slo);
	for (int i = 0; i < ncycle; i++) {
		errlim *= delta / sigma[i];
	}
	errlim = 2 * errlim;
	return (errlim);
}
/*----------------------------------------------------------------------*/
/* computes the ratio of the error at eigenvalue x1 to the error at x2. */
static double errrat(double x1, double x2, double *sigma, int ncycle)
{
	double errrat = 1.0;
	const double rat = x1 / x2;
	for (int k = 0; k < ncycle; k++) {
		errrat = errrat * rat * (1.0 - sigma[k] / x1) / (1.0 - sigma[k] / x2);
	}
	errrat = fabs(errrat);
	return (errrat);
}
/*----------------------------------------------------------------------
 *    least-squares eigenvalue
 *
 *    iteratively estimates the largest eigenvalue and eigenvectior
 *    with error bounds for the least-squares normal matrix a'a.
 *    a chebyshev criterion is used to calculate the optimum set of
 *    origin shifts in order to accelerate convergence.
 *    based upon the rayleigh quotient and error analysis presented in
 *    j. h. wilkinson's "the algebraic eigenvalue problem",
 *    (pp 170 ...), (pp 572 ...).
 *    under very pesimistic assumptions regarding the starting vector,
 *    the algorithm will initially converge to an eigenvalue less than
 *    the largest.  hence, a corresponding pessimistic estimate of an
 *    upper-bound on the largest eigenvalue is also made.
 *
 *                            ..... allen h. olson 10-4-85.
 *                                  university of california, san diego
 *----------------------------------------------------------------------*/
/*-------
 * input
 *-------
 *    a(j,i)  -------  - packed matrix defined above dimensioned at least
 *                       (nnz,nr) note  that the first index is the column
 *                       index! i.e. the matrix is stored in row order.
 *    ia(j,i)  ------  - indices of values in packed matrix a
 *                       i.e. a(j,i) packed = a(j,ia(i,j)) unpacked
 *    nia(i)  -------  - number of nonzero values in each row
 *    nnz   ---------  - number of values in packed rows of a and ia
 *    nc,nr  --------  - number of columns and number of rows of unpacked
 *                       matrix as defined above.
 *    ncyc  ---------  - number of chebyshev iterations to perform.
 *                         must be a power of two.
 *    nsig  ---------  - cumulative number of iterations performed by
 *                         previous calls to this routine.  must be set
 *                         to zero on initial call.  nsig is automatically
 *                         incremented by this routine and must not be
 *                         redefined on subsequent calls by calling program.
 *    x(1..nc)  -----  - initial guess for the eigenvector.
 *    dx(1...nc) ----  - temporary storage array for x(.).
 *    sigma(1..nsmx)-  - array for holding the chebyshev origin shifts.
 *                         each call to lseig performs ncyc+1 iterations.
 *                         nsmx must be greater than or equal to the
 *                         cumulative number of iterations to be performed.
 *    w(1..nsmx)  ---  - temporary storage array for sigma(.).
 *    smax  ---------  - initial guess for the eigenvalue.
 *--------
 * output
 *--------
 *    x(1...nc) -----  - revised estimate of largest eigenvector.
 *    smax  ---------  - reivsed estimate of largest eigenvalue of a'a.
 *    err   ---------  - error bound for smax. we are guaranteed that
 *                         at least one eigenvalue is contained in the
 *                         interval smax+-err.  in the neighborhood of
 *                         convergence, this will contain the maximum.
 *    sup   ---------  - a pessimistic upper bound for the largest
 *                         eigenvector.
 *------
 * note:(1) for ncyc=0, an initial guess for the eigenvector is formed by
 *------  summing the rows of the matrix so that the accumulated vector
 *        increases in length as each row is added.  one iteration of the
 *        power method is then performed to estimate smax.
 *      (2) caution: an anomalous bad guess for the initial eigenvector
 *        being virtually orthogonal to the largest eigenvector
 *        will cause earlier iterations to converge to the next largest
 *        eigenvector.  this is impossible to detect.  in this case, smax
 *        may be much less than the largest eigenvalue.
 *          the parameter eps set below is a pessimistic assumption about
 *        the relative size of the component of the largest eigenvector in
 *        the initial iteration.  from this, an upper-bound is calculated
 *        for the largest eigenvalue, sup.  sup will always be larger than
 *        smax and reflects the uncertainty due to an anomalous bad choice
 *        for the starting vector.
 *-------------------------
 * sample calling sequence
 *-------------------------
 *    ncyc=0
 *    nsig=0
 *    call lseig(a,nc,nr,ncyc,nsig,x,dx,sigma,w,smax,err,sup)
 *    ncyc=4
 *    call lseig(a,nc,nr,ncyc,nsig,x,dx,sigma,w,smax,err,sup)
 *    ncyc=8
 *    call lseig(a,nc,nr,ncyc,nsig,x,dx,sigma,w,smax,err,sup)
 *
 *    the first call with ncyc=0 initializes x(.) and smax.  if these
 *      are already known then ncyc can be set to a nonzero value for the
 *      first call.  nsig must always be zero for the first call however.
 *    the next two calls perform chebyshev iteration to improve x(.) and smax.
 *      upon completion, a total of nsig=1+5+9=15 iterations have actually
 *      been performed.
 *    by making repeated calls to lseig, the error in smax and the difference
 *      between smax and sup can be monitored until the desired level of
 *      certainty is obtained. */
/*----------------------------------------------------------------------*/
static void lspeig(const double *a, const int *ia, const int *nia, int nnz, int nc,
            int nr, int ncyc, int *nsig, double *x, double *dx, double *sigma,
            double *w, double *smax, double *err, double *sup)
{
	int i;
	double eps = 1.0e-6;
	double res = 0.0;
	double slo;
	double smp;
	double errsmp;

	if (ncyc == 0) {
		i = 0;
		for (int j = 0; j < nia[i]; j++) {
			const int k = nnz * i + j;
			x[ia[k]] = a[k];
		}
		for (i = 1; i < nr; i++) {
			res = 0.0;
			for (int j = 0; j < nia[i]; j++) {
				const int k = nnz * i + j;
				res += x[ia[k]] * a[k];
			}
			if (fabs(res) <= 1.0e-30)
				res = 1.0;
			else
				res = res / fabs(res);
			for (int j = 0; j < nia[i]; j++) {
				const int k = nnz * i + j;
				x[ia[k]] += res * a[k];
			}
		}
		res = 0.0;
		for (int j = 0; j < nc; j++) {
			res += x[j] * x[j];
		}
		res = 1.0 / sqrt(res);
		for (int j = 0; j < nc; j++) {
			x[j] = x[j] * res;
		}
	} else {
		slo = 0.0;
		chebyu(&sigma[*nsig], ncyc, *smax, slo, w);
	}

	int nsig1 = *nsig + 1;
	*nsig = nsig1 + ncyc;
	sigma[*nsig - 1] = 0.0;
	for (int icyc = nsig1 - 1; icyc < *nsig; icyc++) {
		for (int j = 0; j < nc; j++) {
			dx[j] = 0.0;
		}
		for (int i = 0; i < nr; i++) {
			res = 0.0;
			for (int j = 0; j < nia[i]; j++) {
				const int k = nnz * i + j;
				res += a[k] * x[ia[k]];
			}
			for (int j = 0; j < nia[i]; j++) {
				const int k = nnz * i + j;
				dx[ia[k]] += res * a[k];
			}
		}
		for (int j = 0; j < nc; j++) {
			dx[j] -= sigma[icyc] * x[j];
		}
		*smax = 0.0;
		for (int j = 0; j < nc; j++) {
			*smax += dx[j] * dx[j];
		}
		*smax = sqrt(*smax);

		if (icyc == *nsig - 1) {
			*err = 0.0;
			for (int j = 0; j < nc; j++) {
				res = dx[j] - *smax * x[j];
				*err += res * res;
			}
			*err = sqrt(*err);
		}

		for (int j = 0; j < nc; j++) {
			x[j] = dx[j] / *smax;
		}
	}

	slo = *smax;
	*sup = (1.0 + eps) * (*smax) * pow(eps, -1.0 / *nsig);
	res = 1.0;
	for (int icyc = 0; icyc < 25 && res > eps; icyc++) {
		smp = 0.5 * (*sup + slo);
		errsmp = errrat(*smax, smp, sigma, *nsig);
		if (errsmp > eps)
			slo = smp;
		else
			*sup = smp;
		res = (*sup - slo) / slo;
	}
}

/*--------------------------------------------------------------------*/
/* mb_mallocd / mb_freed: MBIO's allocation book-keeping, as plain calloc / free */
static int mbnavedit_mallocd(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error) {
	(void)verbose;
	(void)sourcefile;
	(void)sourceline;
	free(*ptr);
	*ptr = (size > 0) ? calloc(1, size) : NULL;
	if (size > 0 && *ptr == NULL) {
		*error = MB_ERROR_MEMORY_FAIL;
		return (MB_FAILURE);
	}
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}

static int mbnavedit_freed(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error) {
	(void)verbose;
	(void)sourcefile;
	(void)sourceline;
	free(*ptr);
	*ptr = NULL;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}

/*--------------------------------------------------------------------*/
/* "cp nfile ifile_use": a plain file copy (mbnavedit_open_file, "use previously edited nav") */
static int mbnavedit_copy_file(const char *from, const char *to) {
	FILE *in = fopen(from, "rb");
	if (in == NULL)
		return (MB_FAILURE);
	FILE *out = fopen(to, "wb");
	if (out == NULL) {
		fclose(in);
		return (MB_FAILURE);
	}
	char buf[65536];
	size_t n;
	int status = MB_SUCCESS;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			status = MB_FAILURE;
			break;
		}
	}
	fclose(in);
	if (fclose(out) != 0)
		status = MB_FAILURE;
	return (status);
}

/*--------------------------------------------------------------------*/
/* mb_file_open_error_message -- MB-System mbaux/mb_xmutil.c, unchanged */
static void mb_file_open_error_message(const char *path, char *line1, char *line2, char *line3, size_t linesize) {
	if (linesize == 0)
		return;

	snprintf(line1, linesize, "Unable to open input file:");
	snprintf(line2, linesize, "%s", path);

	/* Diagnose the actual cause with stat()/access() rather than asserting
	 * one fixed explanation - mb_read_init() does not preserve errno from
	 * its failed fopen() through to this point, and a generic "file may not
	 * exist or you may not have read permission" message is shown for every
	 * failure including an overly long path (errno ENAMETOOLONG). */
	struct stat file_status;
	if (stat(path, &file_status) != 0) {
		const int stat_errno = errno;
		if (stat_errno == ENOENT)
			snprintf(line3, linesize, "File does not exist.");
		else if (stat_errno == ENAMETOOLONG)
			snprintf(line3, linesize, "Path is too long (%zu characters).", strlen(path));
		else if (stat_errno == EACCES)
			snprintf(line3, linesize, "Permission denied.");
		else if (stat_errno == ENOTDIR)
			snprintf(line3, linesize, "A component of the path is not a directory.");
		else
			snprintf(line3, linesize, "%s", strerror(stat_errno));
	}
	else if (access(path, R_OK) != 0) {
		snprintf(line3, linesize, "Permission denied.");
	}
	else {
		snprintf(line3, linesize, "File exists but could not be read (wrong format?).");
	}
}

/*--------------------------------------------------------------------*/
/* The module that holds the swath editor's MBIO table: the library mbedit_mbio_open loaded
   (found as the sound velocity tool finds it, mbvelocity.c). */
#ifdef _WIN32
static void *mbnavedit_mbio_module(char *path, int pathlen) {
	HMODULE h = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)mbedit_mbio.get_all, &h))
		return NULL;
	wchar_t wpath[MB_PATH_MAXLINE];
	const DWORD n = GetModuleFileNameW(h, wpath, MB_PATH_MAXLINE);
	if (n == 0 || n >= MB_PATH_MAXLINE || WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, pathlen, NULL, NULL) == 0)
		path[0] = '\0';
	return (void *)h;
}
static void *mbnavedit_dlsym(void *h, const char *name) {
	return (void *)GetProcAddress((HMODULE)h, name);
}
#else
static void *mbnavedit_mbio_module(char *path, int pathlen) {
	Dl_info info;
	if (dladdr((void *)mbedit_mbio.get_all, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	snprintf(path, pathlen, "%s", info.dli_fname);
	/* the library is already loaded: this only hands back its handle */
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}
static void *mbnavedit_dlsym(void *h, const char *name) {
	return dlsym(h, name);
}
#endif

int mbnavedit_mbio_open(char *msg, int msglen) {
	if (msg && msglen > 0)
		msg[0] = '\0';
	if (mbnavedit_mbio_ok)
		return 1;
	if (!mbedit_mbio_loaded()) {
		snprintf(msg, msglen, "The MB-System MBIO library is not loaded");
		return 0;
	}
	char libpath[MB_PATH_MAXLINE] = "";
	void *h = mbnavedit_mbio_module(libpath, (int)sizeof(libpath));
	if (h == NULL) {
		snprintf(msg, msglen, "Cannot find the module of the loaded MBIO library");
		return 0;
	}

	/* the four prototypes are the same in MB-System 5.7.x and 5.8.x */
	struct mbnavedit_mbio_table t;
	memset(&t, 0, sizeof(t));
	struct {
		void **slot;
		const char *name;
	} syms[] = {
		{(void **)&t.format_source, "mb_format_source"},
		{(void **)&t.coor_scale, "mb_coor_scale"},
		{(void **)&t.get_date, "mb_get_date"},
		{(void **)&t.pr_update_nav, "mb_pr_update_nav"},
	};
	for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		*syms[i].slot = mbnavedit_dlsym(h, syms[i].name);
		if (*syms[i].slot == NULL) {
			snprintf(msg, msglen, "%s does not export %s, which mbnavedit needs", libpath, syms[i].name);
			return 0;
		}
	}
	mbnavedit_mbio = t;
	mbnavedit_mbio_ok = true;
	return 1;
}

/*--------------------------------------------------------------------*/
/* the record buffer goes with the tool */
void mbnavedit_release(void) {
	free(ping);
	ping = NULL;
	nbuff = 0;
	nplot = 0;
	current_id = 0;
}

void mbnavedit_get_state(int *s_file_open, int *s_nbuff, int *s_current_id, int *s_nplot, int *s_nload_total,
                         int *s_ndump_total) {
	*s_file_open = file_open ? 1 : 0;
	*s_nbuff = nbuff;
	*s_current_id = current_id;
	*s_nplot = nplot;
	*s_nload_total = nload_total;
	*s_ndump_total = ndump_total;
}

int mbnavedit_get_record(int i, double *time_d, double *lon, double *lat, double *speed, double *heading, double *draft,
                         int *selected) {
	if (ping == NULL || i < 0 || i >= nbuff)
		return 0;
	*time_d = ping[i].time_d;
	*lon = ping[i].lon;
	*lat = ping[i].lat;
	*speed = ping[i].speed;
	*heading = ping[i].heading;
	*draft = ping[i].draft;
	*selected = (ping[i].tint_select ? 1 : 0) | (ping[i].lon_select ? 2 : 0) | (ping[i].lat_select ? 4 : 0) |
	            (ping[i].speed_select ? 8 : 0) | (ping[i].heading_select ? 16 : 0) | (ping[i].draft_select ? 32 : 0);
	return 1;
}

int mbnavedit_get_plot_box(int iplot, int *ixmin, int *ixmax, int *iymin, int *iymax, int *type) {
	if (iplot < 0 || iplot >= number_plots || iplot >= NUMBER_PLOTS_MAX)
		return 0;
	*ixmin = mbnavplot[iplot].ixmin;
	*ixmax = mbnavplot[iplot].ixmax;
	*iymin = mbnavplot[iplot].iymin;
	*iymax = mbnavplot[iplot].iymax;
	*type = mbnavplot[iplot].type;
	return 1;
}

int mbnavedit_get_record_xy(int iplot, int i, int *x, int *y) {
	if (ping == NULL || iplot < 0 || iplot >= number_plots || i < current_id || i >= current_id + nplot || i >= nbuff)
		return 0;
	switch (mbnavplot[iplot].type) {
	case PLOT_TINT:
		*x = ping[i].tint_x;
		*y = ping[i].tint_y;
		return 1;
	case PLOT_LONGITUDE:
		*x = ping[i].lon_x;
		*y = ping[i].lon_y;
		return 1;
	case PLOT_LATITUDE:
		*x = ping[i].lat_x;
		*y = ping[i].lat_y;
		return 1;
	case PLOT_SPEED:
		*x = ping[i].speed_x;
		*y = ping[i].speed_y;
		return 1;
	case PLOT_HEADING:
		*x = ping[i].heading_x;
		*y = ping[i].heading_y;
		return 1;
	case PLOT_DRAFT:
		*x = ping[i].draft_x;
		*y = ping[i].draft_y;
		return 1;
	default:
		return 0;
	}
}

/* do_fileselection_list: the file's format from its name */
int mbnavedit_get_format(char *file, int *form) {
	int format_error = MB_ERROR_NO_ERROR;
	int f = 0;
	if (mb_get_format(0, file, NULL, &f, &format_error) == MB_SUCCESS) {
		*form = f;
		return (MB_SUCCESS);
	}
	return (MB_FAILURE);
}
