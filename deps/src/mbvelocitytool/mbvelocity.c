/*--------------------------------------------------------------------
 *    The MB-system:    mbvelocitytool.c        6/6/93
 *
 *    Copyright (c) 1993-2025 by
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
 * MBVELOCITYTOOL is an interactive water velocity profile editor
 * used to examine multiple water velocity profiles and to create
 * new water velocity profiles which can be used for the processing
 * of multibeam sonar data.  In general, this tool is used to examine
 * water velocity profiles obtained from XBTs, CTDs, or databases,
 * and to construct new profiles consistent with these various
 * sources of information.
 *
 * Author:      D. W. Caress
 * Date:        June 6, 1993
 */

/*--------------------------------------------------------------------
 * InteractiveGMT port (deps/src/mbvelocitytool/mbvelocity.c)
 *
 * MB-System 5.8.3's src/mbvelocitytool/mbvelocity_prog.c, the engine of mbvelocitytool, carried
 * over line for line: the edit and display profiles, the node editing, the swath file buffer, the
 * raytracing, the residuals, the files written for mbprocess and the drawing. What changed, and
 * only that:
 *   - No X11, no getopt: the drawing goes through the host's xg_* hooks (mbvelocity_window.cpp
 *     paints them into a QImage) and the command-line options (-I -F -S -W) are the window's, so
 *     mbvt_init() keeps the defaults and drops the argument parsing.
 *   - MBIO is not linked. The swath editor (deps/src/mbedit/) loads it at run time and fills the
 *     table of mbedit_mbio.h; this file calls that table under the original names, and picks the
 *     entry points only this tool needs out of the SAME library (mbvt_mbio_open below). Where
 *     MB-System 5.7.x and 5.8.x differ (mb_format_flags writes ints in 5.7, bools in 5.8; 5.7's
 *     mb_rt has no travel-time plot list), an adapter passes what the loaded library wants.
 *   - mblevitus is not run as a program writing a temporary file in the current folder: its
 *     calculation (MB-System src/utilities/mblevitus.cc) is carried in as mbvt_levitus_profile()
 *     and the profile goes straight into the display list. The Levitus database is the one that
 *     sits with the MBIO library (<lib>/../share/mbsystem/LevitusAnnual82.dat), as mblevitus finds
 *     it next to itself.
 *   - exit() is gone: this code runs inside the host process, so a failure reports and returns.
 *   - mb_mallocd / mb_freed / mb_memory_list (MBIO's allocation book-keeping) are plain
 *     calloc / free; mb_user_host_date (absent from 5.7.x) is carried in, unchanged.
 *   - File-scope globals are static, so this translation unit exports nothing but the mbvt_*
 *     functions.
 *--------------------------------------------------------------------*/

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#	define _GNU_SOURCE
#	include <dlfcn.h>
#	include <unistd.h>
#endif

#include "../mbedit/mbedit_mbio.h"
#include "mbvelocity.h"

/* the swath editor's MBIO loader (deps/src/mbedit/mbedit.c) */
int mbedit_mbio_loaded(void);
const char *mbedit_mbio_version(void);
#define MB_VERSION (mbedit_mbio_version())

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DTR (M_PI / 180.)

/* ---- mb_process.h ------------------------------------------------------------------ */
#define MBP_ANGLES_OK 0
#define MBP_ANGLES_SNELL 1
#define MBP_ANGLES_SNELLNULL 2

/* ---- the entry points this tool adds to the swath editor's table ---------------------- */
struct mbvt_mbio_table {
	int (*format_flags)(int verbose, int *format, void *variable_beams, void *traveltime, void *beam_flagging, int *error);
	int (*ttimes)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, int *nbeams, double *ttimes, double *angles,
	              double *angles_forward, double *angles_null, double *heave, double *alongtrack_offset, double *draft,
	              double *ssv, int *error);
	int (*xyz_to_takeoff)(int verbose, double x, double y, double z, double *theta, double *phi, int *error);
	int (*rt_init)(int verbose, int number_node, double *depth, double *velocity, void **modelptr, int *error);
	int (*rt_deall)(int verbose, void **modelptr, int *error);
	void *rt;   /* the 5.7 and 5.8 signatures differ: see mbvt_rt */
	int (*pr_get_svp)(int verbose, char *file, int *mbp_svp_mode, char *mbp_svpfile, int *mbp_angle_mode, int *mbp_corrected,
	                  int *error);
	int (*pr_update_svp)(int verbose, char *file, int mbp_svp_mode, char *mbp_svpfile, int mbp_angle_mode, int mbp_corrected,
	                     int *error);
	int (*pr_get_static)(int verbose, char *file, int *mbp_static_mode, char *mbp_staticfile, int *error);
	int (*pr_update_static)(int verbose, char *file, int mbp_static_mode, char *mbp_staticfile, int *error);
};
static struct mbvt_mbio_table mbvt_mbio;
static bool mbvt_mbio_ok = false;
static bool mbvt_mbio_legacy = false;   /* a 5.7.x library */
static char mbvt_levitus_path[MB_PATH_MAXLINE] = "";

static int mbvt_format_flags(int verbose, int *format, bool *variable_beams, bool *traveltime, bool *beam_flagging, int *error);
static int mbvt_rt(int verbose, void *modelptr, double source_depth, double source_angle, double end_time, int ssv_mode,
                   double surface_vel, double null_angle, int nplot_max, int *nplot, double *xplot, double *zplot, double *tplot,
                   double *x, double *z, double *travel_time, int *ray_stat, int *error);
static int mbvt_user_host_date(int verbose, char user[256], char host[256], char date[32], int *error);
static int mbvt_mallocd(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
static int mbvt_freed(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error);
static void mb_file_open_error_message(const char *path, char *line1, char *line2, char *line3, size_t linesize);
static int mbvt_levitus_profile(double longitude, double latitude);

#define mb_format_flags       mbvt_format_flags
#define mb_ttimes             mbvt_mbio.ttimes
#define mb_xyz_to_takeoff     mbvt_mbio.xyz_to_takeoff
#define mb_rt_init            mbvt_mbio.rt_init
#define mb_rt_deall           mbvt_mbio.rt_deall
#define mb_rt                 mbvt_rt
#define mb_pr_get_svp         mbvt_mbio.pr_get_svp
#define mb_pr_update_svp      mbvt_mbio.pr_update_svp
#define mb_pr_get_static      mbvt_mbio.pr_get_static
#define mb_pr_update_static   mbvt_mbio.pr_update_static
#define mb_user_host_date     mbvt_user_host_date
#define mb_mallocd            mbvt_mallocd
#define mb_freed              mbvt_freed

/* velocity profile structure definition */
struct profile {
	int n;
	int nalloc;
	mb_path name;
	double *depth;
	double *velocity;
};

/* ping structure definition */
struct mbvt_ping_struct {
	int allocated;
	int time_i[7];
	double time_d;
	double navlon;
	double navlat;
	double speed;
	double heading;
	double sensordepth;
	double ssv;
	int beams_bath;
	char *beamflag;
	double *bath;
	double *bathacrosstrack;
	double *bathalongtrack;
	double *ttimes;
	double *angles;
	double *angles_forward;
	double *angles_null;
	double *heave;
	double *alongtrack_offset;
};

static char program_name[] = "MBVELOCITYTOOL";

static int error = MB_ERROR_NO_ERROR;
static int verbose = 0;
static char *message = NULL;

/* mbvelocitytool control variables */
static struct profile profile_display[MAX_PROFILES];
static struct profile profile_edit;
static int *edit_x = NULL;
static int *edit_y = NULL;
static bool edit = false;
static int ndisplay = 0;
static void *mbvt_xgid;
static int borders[4];
static double maxdepth = 3000.0;
static double velrange = 500.0;
static double velcenter = 1490.0;
static double resrange = 200.0;
static double ssv_start = 0.0;
static int anglemode = MBP_ANGLES_SNELL;

/* plotting variables */
static int xmin, xmax, ymin, ymax;
static double xminimum, xmaximum, yminimum, ymaximum;
static double xscale, yscale;
static int xrmin, xrmax, yrmin, yrmax;
static double xrminimum, xrmaximum, yrminimum, yrmaximum;
static double xrscale, yrscale;
static int xpmin, xpmax, ypmin, ypmax;
static double xpminimum, xpmaximum, ypminimum, ypmaximum;
static double xpscale, ypscale;
static int active = -1;

/* default edit profile */
static int depthedit[NUM_EDIT_START] = {0, 300, 1000, 3000, 7000, 12000};
static int veledit[NUM_EDIT_START] = {1500, 1500, 1500, 1500, 1500, 1500};

/* MBIO control parameters */
static int format;
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
static mb_path swathfile;
static void *mbio_ptr;

/* mbio read and write values */
static void *store_ptr = NULL;
static int nbeams;

/* buffer control variables */
#define MBVT_BUFFER_SIZE 25000
static int nbuffer;

/* survey ping raytracing arrays */
static int namp, nss;
static char *beamflag = NULL;
static double *bath = NULL;
static double *amp = NULL;
static double *bathacrosstrack = NULL;
static double *bathalongtrack = NULL;
static double *ss = NULL;
static double *ssacrosstrack = NULL;
static double *ssalongtrack = NULL;
static char comment[MB_COMMENT_MAXLINE];
static int nraypathmax;
static int *nraypath = NULL;
static double **raypathx = NULL;
static double **raypathy = NULL;
static double **raypatht = NULL;
static double *depth = NULL;
static double *acrosstrack = NULL;
static double rayxmax;
static double raydepthmin;
static double raydepthmax;
static struct mbvt_ping_struct ping[MBVT_BUFFER_SIZE];

/* ESF File read */
static char esffile[MB_PATH_MAXLINE];
static struct mb_esf_struct esf;

/* depth range variables */
static double bath_min = 0.0;
static double bath_max = 0.0;

/* residual variables */
static double *angle = NULL;
static double *residual_acrosstrack = NULL;
static double *residual_altitude = NULL;
static double *residual = NULL;
static double *res_sd = NULL;
static int *nresidual = NULL;

/* beam range variables */
static int beam_first = 0;
static int beam_last = 100;

/* color control values */
static int ncolors;
static int pixel_values[256];

/*--------------------------------------------------------------------*/
/* Initialize the 'mbio' struct                                       */
/* Called by:                                                         */
/*                  main                                              */
/* Functions called:                                                  */
/*                  mb_defaults                                       */
/* Function returns:                                                  */
/*                  int status                                        */
/*--------------------------------------------------------------------*/
int mbvt_init(void) {
	int status = MB_SUCCESS;

	/* set default values */
	status = mb_defaults(verbose, &format, &pings, &lonflip, bounds, btime_i, etime_i, &speedmin, &timegap);
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
	etime_i[0] = 2062;
	etime_i[1] = 2;
	etime_i[2] = 21;
	etime_i[3] = 10;
	etime_i[4] = 30;
	etime_i[5] = 0;
	speedmin = 0.0;
	timegap = 1000000000.0;
	nbeams = 16;

	/* The command line (getopt_long over -B -E -F -I -S -W -V -H) is not ported: the input
	 * files are the window's (mbvelocityOpenWindow, the File menu), so nothing is opened here.
	 * mbvt_init used to end in do_open_commandline; the window calls that itself. */

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
/* exits the program - from "QUIT" on menu bar.                       */
/* Called by:                                                         */
/*                  mbvelocity_callbacks.c                            */
/* Functions called:                                                  */
/*                  mb_freed                                          */
/*                  mb_memory_list                                    */
/* Function returns:                                                  */
/*                  int status                                        */
/*--------------------------------------------------------------------*/
int mbvt_quit(void) {
	struct profile *profile;
	int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* get profile pointer */
	profile = &profile_edit;

	/* deallocate previously loaded data, if any */
	mbvt_deallocate_swath();

	/* deallocate editable svp model */
	if (edit) {
		edit = false;
		profile->n = 0;
		strcpy(profile->name, "");
		mb_freed(verbose, __FILE__, __LINE__, (void **)&edit_x, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&edit_y, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->depth, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->velocity, &error);
	}

	/* deallocate any display svp profiles */
	if (ndisplay > 0) {
		for (int i = ndisplay - 1; i >= 0; i--) {
			mbvt_delete_display_profile(i);
		}
	}
	/* mbvt_delete_display_profile leaves the count to its caller (see mbvt_delete_display_profile) */
	ndisplay = 0;

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
/* mbvt_set_graphics - sets mbvt_xgrid to a pointer to the display    */
/*                     and sets borders.                              */
/* Called by:                                                         */
/*                  main                                              */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  int status                                        */
/*--------------------------------------------------------------------*/
int mbvt_set_graphics(void *xgid, int *brdr, int ncol, unsigned int *pixels) {
	int status = MB_SUCCESS;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       xgid:         %p\n", xgid);
		for (i = 0; i < 4; i++)
			fprintf(stderr, "dbg2       borders[%d]:   %d\n", i, brdr[i]);
		fprintf(stderr, "dbg2       ncolors:      %d\n", ncol);
		for (i = 0; i < ncol; i++)
			fprintf(stderr, "dbg2       pixel[%d]:     %d\n", i, pixels[i]);
	}

	/* set graphics id */
	mbvt_xgid = xgid;

	/* set graphics bounds */
	for (i = 0; i < 4; i++)
		borders[i] = brdr[i];

	/* set colors */
	ncolors = ncol;
	for (i = 0; i < ncolors; i++)
		pixel_values[i] = pixels[i];

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
/* Sets some of the mbio variables                                    */
/* Called by:                                                         */
/*                  main                                              */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  int status                                        */
/*--------------------------------------------------------------------*/
int mbvt_get_values(int *s_edit, int *s_ndisplay, double *s_maxdepth, double *s_velrange, double *s_velcenter, double *s_resrange,
                    int *s_anglemode, int *s_format) {
	int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* set values */
	*s_edit = edit;
	*s_ndisplay = ndisplay;
	*s_maxdepth = maxdepth;
	*s_velrange = velrange;
	*s_velcenter = velcenter;
	*s_resrange = resrange;
	*s_anglemode = anglemode;
	*s_format = format;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       s_edit:      %d\n", *s_edit);
		fprintf(stderr, "dbg2       s_ndisplay:  %d\n", *s_ndisplay);
		fprintf(stderr, "dbg2       s_maxdepth:  %f\n", *s_maxdepth);
		fprintf(stderr, "dbg2       s_velrange:  %f\n", *s_velrange);
		fprintf(stderr, "dbg2       s_velcenter: %f\n", *s_velcenter);
		fprintf(stderr, "dbg2       s_resrange:  %f\n", *s_resrange);
		fprintf(stderr, "dbg2       s_anglemode: %d\n", *s_anglemode);
		fprintf(stderr, "dbg2       s_format:    %d\n", *s_format);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}

/*--------------------------------------------------------------------*/
/* Sets some of the mbio variables.                                   */
/* Called by:                                                         */
/*                  action_maxdepth - mbvelocity_callbacks - slider   */
/*                  action_velrange - mbvelocity_callbacks - slider   */
/*                  action_velcenter - mbvelocity_callbacks - slider   */
/*                  action_residual_range - mbvelocity_callbacks      */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_set_values(int s_edit, int s_ndisplay, double s_maxdepth, double s_velrange, double s_velcenter, double s_resrange,
                    int s_anglemode) {
	int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       s_edit:      %d\n", s_edit);
		fprintf(stderr, "dbg2       s_ndisplay:  %d\n", s_ndisplay);
		fprintf(stderr, "dbg2       s_maxdepth:  %f\n", s_maxdepth);
		fprintf(stderr, "dbg2       s_velrange:  %f\n", s_velrange);
		fprintf(stderr, "dbg2       s_velcenter: %f\n", s_velcenter);
		fprintf(stderr, "dbg2       s_resrange:  %f\n", s_resrange);
		fprintf(stderr, "dbg2       s_anglemode: %d\n", s_anglemode);
	}

	/* set values */
	edit = s_edit;
	ndisplay = s_ndisplay;
	maxdepth = s_maxdepth;
	velrange = s_velrange;
	velcenter = s_velcenter;
	resrange = s_resrange;
	anglemode = s_anglemode;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function attempts to open a file as editable.                 */
/* Called by:                                                         */
/*                 open_file_ok in mbvelocity_callbacks.c. This is called */
/*		     by selecting the "OK" button in the file         */
/*		     selection widget for an editable file.           */
/* Functions called:                                                  */
/*                  mb_freed                                          */
/*                  mb_mallocd                                        */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_open_edit_profile(char *file) {
	int status = MB_SUCCESS;
	int size;
	mb_path buffer;
	char *result;
	struct profile *profile;
	FILE *fp;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
	}

	/* get profile pointer */
	profile = &profile_edit;

	/* clear out old velocity data */
	if (edit) {
		edit = false;
		profile->n = 0;
		strcpy(profile->name, "");
		mb_freed(verbose, __FILE__, __LINE__, (void **)&edit_x, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&edit_y, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->depth, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->velocity, &error);
	}

	/* open the file if possible and count the velocity points */
	profile->n = 0;
	if ((fp = fopen(file, "r")) == NULL) {
		status = MB_FAILURE;
		fprintf(stderr, "\nUnable to Open Velocity Profile File <%s> for reading\n", file);
		do_error_dialog("Unable to open input SVP file.", "File may not exist or you may not have",
		                "read permission in this directory!");
		return (status);
	}
	while ((result = fgets(buffer, MB_PATH_MAXLINE, fp)) == buffer)
		if (buffer[0] != '#')
			profile->n++;
	fclose(fp);

	/* allocate space for the velocity profile and raytracing tables */
	profile->nalloc = MAX(10 * profile->n, 60);
	size = profile->nalloc * sizeof(int);
	status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&edit_x, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&edit_y, &error);
	size = profile->nalloc * sizeof(double);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&(profile->depth), &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&(profile->velocity), &error);

	/* if error initializing memory then quit */
	if (status != MB_SUCCESS) {
		mb_error(verbose, error, &message);
		fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
		do_error_dialog("Unable to allocate memory", "for the editable SVP.", "");
		return (status);
	}

	/* open the file if possible and read the velocity points */
	profile->n = 0;
	strcpy(profile->name, file);
	if ((fp = fopen(file, "r")) == NULL) {
		status = MB_FAILURE;
		fprintf(stderr, "\nUnable to Open Velocity Profile File <%s> for reading\n", file);
		return (status);
	}
	strncpy(buffer, "", sizeof(buffer));
	while ((result = fgets(buffer, MB_PATH_MAXLINE, fp)) == buffer) {
		if (buffer[0] != '#') {
			sscanf(buffer, "%lf %lf", &(profile->depth[profile->n]), &(profile->velocity[profile->n]));

			/* output some debug values */
			if (verbose >= 5) {
				fprintf(stderr, "\ndbg5  New velocity value read in program <%s>\n", program_name);
				fprintf(stderr, "dbg5       dep[%d]: %f  vel[%d]: %f\n", profile->n, profile->depth[profile->n], profile->n,
				        profile->velocity[profile->n]);
			}
			profile->n++;
		}
		strncpy(buffer, "", sizeof(buffer));
	}
	fclose(fp);

	/* assume success */
	edit = true;
	status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function displays a new editable profile.                     */
/* Called by:                                                         */
/*                  action_new_profile in mbvelocity_callbacks.c      */
/*		      		which is called by selecting the "NEW EDITABLE    */
/*                    PROFILE" from the "FILE" pulldown menu.         */
/*		    -       also called from mbvt_process_multibeam()	      */
/*		      		if no editable profile already exists.	          */
/* Functions called:                                                  */
/*                  mb_freed                                          */
/*		            mb_mallocd                                        */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_new_edit_profile(void) {
	int status = MB_SUCCESS;
	struct profile *profile;
	int size;
	double dz;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* get profile pointer */
	profile = &profile_edit;

	/* clear out old velocity data */
	if (edit) {
		edit = false;
		profile->n = 0;
		strcpy(profile->name, "");
		mb_freed(verbose, __FILE__, __LINE__, (void **)&edit_x, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&edit_y, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->depth, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->velocity, &error);
	}

	/* allocate space for the velocity profile and raytracing tables */
	profile->nalloc = 10 * NUM_EDIT_START;
	size = profile->nalloc * sizeof(int);
	status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&edit_x, &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&edit_y, &error);
	size = profile->nalloc * sizeof(double);
	profile->depth = NULL;
	profile->velocity = NULL;
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&(profile->depth), &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, size, (void **)&(profile->velocity), &error);

	/* if error initializing memory then quit */
	if (status != MB_SUCCESS) {
		mb_error(verbose, error, &message);
		fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
		do_error_dialog("Unable to allocate memory", "for the editable SVP.", "");
		return (status);
	}

	/* copy the default values */
	strcpy(profile->name, "new");
	if (bath_max > bath_min && bath_max < 2000) {
		profile->n = NUM_EDIT_START;
		dz = 1.25 * bath_max / (profile->n - 2);
		for (i = 0; i < profile->n - 1; i++) {
			profile->depth[i] = i * dz;
			profile->velocity[i] = veledit[i];
		}
		profile->depth[profile->n - 1] = depthedit[profile->n - 1];
		profile->velocity[profile->n - 1] = veledit[profile->n - 1];
	}
	else {
		profile->n = NUM_EDIT_START;
		for (i = 0; i < profile->n; i++) {
			profile->depth[i] = depthedit[i];
			profile->velocity[i] = veledit[i];
		}
	}

	/* assume success */
	edit = true;
	status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function saves the editable profile.                          */
/* Called by:                                                         */
/*                  controls_save_file in mbvelocity_callbacks.c. It is   */
/*                    called when the user selects "OK" from the      */
/*                    save editable file widget.                      */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_save_edit_profile(char *file) {
	int status = MB_SUCCESS;
	struct profile *profile;
	FILE *fp;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
	}

	/* get profile pointer */
	profile = &profile_edit;

	/* open the file if possible */
	if ((fp = fopen(file, "w")) == NULL) {
		status = MB_FAILURE;
		fprintf(stderr, "\nUnable to Open Output Velocity Profile File <%s> for writing\n", file);
		do_error_dialog("Unable to open output file.", "You may not have write", "permission in this directory!");
		return (status);
	}

	/* write the svp */
	fprintf(fp, "## Water Sound Velocity Profile (SVP)\n");
	fprintf(fp, "## Output by Program %s\n", program_name);
	fprintf(fp, "## MB-System Version %s\n", MB_VERSION);
	char user[256], host[256], date[32];
	status = mb_user_host_date(verbose, user, host, date, &error);
	fprintf(fp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
	fprintf(fp, "## Number of SVP Points: %d\n", profile->n);
	for (i = 0; i < profile->n; i++)
		fprintf(fp, "%f %f\n", profile->depth[i], profile->velocity[i]);

	/* close the file */
	fclose(fp);

	/* assume success */
	edit = true;
	status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function saves the editable profile and sets up               */
/*                  its use by mbprocess                              */
/* Called by:                                                         */
/*                  controls_save_file in mbvelocity_callbacks.c. It is   */
/*                    called when the user selects "OK" from the      */
/*                    save editable file widget.                      */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_save_swath_profile(char *file) {
	int status = MB_SUCCESS;
	struct profile *profile;
	FILE *fp;
	int oldmode, oldanglemode, corrected;
	mb_path oldfile;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
	}

	/* get profile pointer */
	profile = &profile_edit;

	/* do this if edit profile exists and swath data read */
	if (profile->n > 2 && nbuffer > 0) {

		/* open the file if possible */
		sprintf(file, "%s.svp", swathfile);
		if ((fp = fopen(file, "w")) == NULL) {
			status = MB_FAILURE;
			fprintf(stderr, "\nUnable to Open Output Velocity Profile File <%s> for writing\n", file);
			do_error_dialog("Unable to open output file.", "You may not have write", "permission in this directory!");
			return (status);
		}

		/* write the svp */
		fprintf(fp, "## Water Sound Velocity Profile (SVP)\n");
		fprintf(fp, "## Output by Program %s\n", program_name);
		fprintf(fp, "## MB-System Version %s\n", MB_VERSION);
		char user[256], host[256], date[32];
		status = mb_user_host_date(verbose, user, host, date, &error);
		fprintf(fp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
		fprintf(fp, "## Swath File: %s\n", swathfile);
		fprintf(fp, "## Number of SVP Points: %d\n", profile->n);
		for (i = 0; i < profile->n; i++)
			fprintf(fp, "%f %f\n", profile->depth[i], profile->velocity[i]);

		/* close the file */
		fclose(fp);

		/* set par file for use with mbprocess */
		status = mb_pr_get_svp(verbose, swathfile, &oldmode, oldfile, &oldanglemode, &corrected, &error);
		status = mb_pr_update_svp(verbose, swathfile, true, file, anglemode, corrected, &error);

		/* check success */
		if (status == MB_SUCCESS)
			edit = true;
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function saves the residuals at beam statics and sets up      */
/*                  their use by mbprocess                            */
/* Called by:                                                         */
/*                  do_save_residuals() in mbvelocity_callbacks.c.    */
/*                    called when the user selects "Save Residuals"   */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_save_residuals(char *file) {
	int status = MB_SUCCESS;
	FILE *fp;
	int oldmode;
	mb_path oldfile;
	double rr, xx, dangle;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
	}

	/* do this if edit profile exists and swath data read */
	if (profile_edit.n > 2 && nbuffer > 0) {
		char user[256], host[256], date[32];
		status = mb_user_host_date(verbose, user, host, date, &error);

		/* open the *.sbo file if possible */
		sprintf(file, "%s.sbo", swathfile);
		if ((fp = fopen(file, "w")) == NULL) {
			status = MB_FAILURE;
			fprintf(stderr, "\nUnable to Open Output Static Beam Offset File <%s> for writing\n", file);
			do_error_dialog("Unable to open output file.", "You may not have write", "permission in this directory!");
			return (status);
		}

		/* write the sbo file */
		fprintf(fp, "## Static Beam Offset (SBO)\n");
		fprintf(fp, "## Output by Program %s\n", program_name);
		fprintf(fp, "## MB-System Version %s\n", MB_VERSION);
		fprintf(fp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
		fprintf(fp, "## Swath File: %s\n", swathfile);
		fprintf(fp, "## Number of Static Beam Offset Points: %d\n", nbeams);
		for (i = 0; i < nbeams; i++)
			fprintf(fp, " %4d  %9.3f  %9.3f\n", i, residual[i], res_sd[i]);

		/* close the file */
		fclose(fp);

		/* set par file for use with mbprocess */
		status = mb_pr_get_static(verbose, swathfile, &oldmode, oldfile, &error);
		status = mb_pr_update_static(verbose, swathfile, true, file, &error);

		/* check success */
		if (status == MB_SUCCESS)
			edit = true;

		/* open the *.sbao file if possible */
		sprintf(file, "%s.sbao", swathfile);
		if ((fp = fopen(file, "w")) == NULL) {
			status = MB_FAILURE;
			fprintf(stderr, "\nUnable to Open Output Static Beam Angle Offset File <%s> for writing\n", file);
			do_error_dialog("Unable to open output file.", "You may not have write", "permission in this directory!");
			return (status);
		}

		/* write the sbao file */
		fprintf(fp, "## Static Beam Angle Offset (SBAO)\n");
		fprintf(fp, "## Output by Program %s\n", program_name);
		fprintf(fp, "## MB-System Version %s\n", MB_VERSION);
		fprintf(fp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
		fprintf(fp, "## Swath File: %s\n", swathfile);
		fprintf(fp, "## Number of Static Beam Angle Offset Points: %d\n", nbeams);
		for (i = 0; i < nbeams; i++) {
			rr = sqrt(residual_acrosstrack[i] * residual_acrosstrack[i] +
			          (residual_altitude[i] + residual[i]) * (residual_altitude[i] + residual[i]));
			xx = copysign(sqrt(rr * rr - residual_altitude[i] * residual_altitude[i]), residual_acrosstrack[i]);
			if (rr > 0.0)
				dangle = asin(residual_acrosstrack[i] / rr) - asin(xx / rr);
			else
				dangle = 0.0;
			fprintf(fp, " %4d  %9.3f  %9.3f\n", i, angle[i], dangle);
		}

		/* close the file */
		fclose(fp);
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function reads the data in the requested display file.        */
/* Called by:                                                         */
/*                  open_file_ok in mbvelocity_callbacks.c                */
/* Functions called:                                                  */
/*                  mb_mallocd                                        */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_open_display_profile(char *file) {
	int status = MB_SUCCESS;
	mb_path buffer;
	char *result;
	struct profile *profile;
	FILE *fp;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
	}

	/* check that there is room for these data */
	if (ndisplay >= MAX_PROFILES) {
		status = MB_FAILURE;
		fprintf(stderr, "\nNo room for another display velocity profile\n");
		do_error_dialog("Unable to open input SVP file.", "There is no room for another", "display SVP!");
		return (status);
	}

	/* get profile pointer */
	profile = &profile_display[ndisplay];

	/* open the file if possible and count the velocity points */
	profile->n = 0;
	if ((fp = fopen(file, "r")) == NULL) {
		status = MB_FAILURE;
		fprintf(stderr, "\nUnable to Open Velocity Profile File <%s> for reading\n", file);
		do_error_dialog("Unable to open input SVP file.", "File may not exist or you may not have",
		                "read permission in this directory!");
		return (status);
	}
	while ((result = fgets(buffer, MB_PATH_MAXLINE, fp)) == buffer)
		if (buffer[0] != '#')
			profile->n++;
	fclose(fp);

	/* allocate space for the velocity profile and raytracing tables */
	profile->depth = NULL;
	profile->velocity = NULL;
	profile->nalloc = profile->n;
	status = mb_mallocd(verbose, __FILE__, __LINE__, profile->nalloc * sizeof(double), (void **)&(profile->depth), &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, profile->nalloc * sizeof(double), (void **)&(profile->velocity), &error);

	/* if error initializing memory then quit */
	if (status != MB_SUCCESS) {
		mb_error(verbose, error, &message);
		fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
		do_error_dialog("Unable to allocate memory", "for the display SVP.", "");
		return (status);
	}

	/* open the file if possible and read the velocity points */
	profile->n = 0;
	strcpy(profile->name, file);
	if ((fp = fopen(file, "r")) == NULL) {
		status = MB_FAILURE;
		fprintf(stderr, "\nUnable to Open Velocity Profile File <%s> for reading\n", file);
		return (status);
	}
	strncpy(buffer, "", sizeof(buffer));
	while ((result = fgets(buffer, MB_PATH_MAXLINE, fp)) == buffer) {
		if (buffer[0] != '#') {
			sscanf(buffer, "%lf %lf", &(profile->depth[profile->n]), &(profile->velocity[profile->n]));

			/* output some debug values */
			if (verbose >= 5) {
				fprintf(stderr, "\ndbg5  New velocity value read in program <%s>\n", program_name);
				fprintf(stderr, "dbg5       dep[%d]: %f  vel[%d]: %f\n", profile->n, profile->depth[profile->n], profile->n,
				        profile->velocity[profile->n]);
			}
			profile->n++;
		}
		strncpy(buffer, "", sizeof(buffer));
	}
	fclose(fp);

	/* assume success */
	ndisplay++;
	status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is used to keep track of the display profiles that   */
/*   are being displayed so the user can remove unwanted displayed.   */
/*   This feature is not functional at this time.                     */
/* Called by:                                                         */
/*                  mbvelocitytool_set_menu which is not being used   */
/*		      at this time.                                   */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_get_display_names(int *nlist, char *list[MAX_PROFILES]) {
	int status = MB_SUCCESS;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       list:        %p\n", (void *)list);
	}

	/* set values */
	*nlist = ndisplay;
	for (i = 0; i < *nlist; i++)
		list[i] = profile_display[i].name;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nlist:       %d\n", *nlist);
		for (i = 0; i < *nlist; i++)
			fprintf(stderr, "dbg2       name[%d]: %s\n", i, list[i]);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is used to delete an unwanted display profile from   */
/*   the screen. This feature is not in use at this time.             */
/* Called by:                                                         */
/*                  none - not in use                                 */
/* Functions called:                                                  */
/*                  mb_freed                                          */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_delete_display_profile(int select) {
	struct profile *profile;
	int status = MB_SUCCESS;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       select:      %d\n", select);
	}

	/* check if reasonable */
	if (select < ndisplay) {
		/* remove selected profile */
		profile = &profile_display[select];
		profile->n = 0;
		profile->nalloc = 0;
		strcpy(profile->name, "");
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->depth, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&profile->velocity, &error);

		/* reorder remaining profiles -- move each struct (including its
		   depth/velocity pointers and nalloc) down as a unit rather than
		   copying array elements into the previous slot's existing buffer,
		   which was sized for that slot's own (possibly smaller) profile */
		for (i = select + 1; i < ndisplay; i++) {
			profile_display[i - 1] = profile_display[i];
		}
	}
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This is the main plotting function. It does all the drawing in the */
/*   canvas part of the screen.                                       */
/* Called by:                                                         */
/*                  main                                              */
/*                  display_menu                                      */
/*                  action_new_profile                                */
/*                  controls_save_file                                */
/*                  open_file_ok                                      */
/*                  action_maxdepth                                   */
/*                  action_menu_close_profile                         */
/*                  action_velrange                                   */
/*                  action_canvas_event                               */
/*                  action_residual_range                             */
/*                  action_process_mb                                 */
/*                  mbvt_open_swath_file                             */
/* Functions called:                                                  */
/*                  xg_setclip                                        */
/*                  xg_fillrectangle                                  */
/*                  xg_drawline                                       */
/* 		    xg_justify                                        */
/* 		    xg_drawstring                                     */
/* 		    xg_drawrectangle                                  */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_plot(void) {
	int status = MB_SUCCESS;

	/* plotting variables */
	int margin;
	int xcen, ycen;
	double deltax, deltay;
	int nx_int, ny_int;
	int x_int, y_int;
	int xrcen, yrcen;
	double deltaxr, deltayr;
	int nxr_int, nyr_int;
	int xr_int, yr_int;
	int xpcen, ypcen;
	double deltaxp, deltayp;
	int nxp_int, nyp_int;
	int xp_int, yp_int;
	int xx = 0, yy = 0;
	int yyl, yyu;
	double vx, vy;
	int xxo = 0, yyo = 0;
	int swidth, sascent, sdescent;
	mb_path string;
	char format_str[10];
	int color;
	int i, j;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, " borders[0] = %d\n", borders[0]);
		fprintf(stderr, " borders[1] = %d\n", borders[1]);
		fprintf(stderr, " borders[2] = %d\n", borders[2]);
		fprintf(stderr, " borders[3] = %d\n", borders[3]);
		fprintf(stderr, " mbvt_xgid  = %p\n", mbvt_xgid);
	}

	/* turn clipp mask back to whole canvas */
	/* set clip rectangle */

	xg_setclip(mbvt_xgid, borders[0], borders[2], borders[1] - borders[0], borders[3] - borders[2]);

	/* clear screen */
	xg_fillrectangle(mbvt_xgid, borders[0], borders[2], borders[1] - borders[0], borders[3] - borders[2], pixel_values[MB_COLOR_WHITE],
	                 XG_SOLIDLINE);

	/* set scaling */
	margin = (borders[3] - borders[2]) / 15;
	xmin = 2.25 * margin;
	xmax = 0.5 * (borders[1] - borders[0]) - margin;
	ymin = margin;
	ymax = 0.5 * (borders[3] - borders[2]);
	xcen = xmin + (xmax - xmin) / 2;
	ycen = ymin + (ymax - ymin) / 2;
	xminimum = velcenter - velrange / 2;
	xmaximum = velcenter + velrange / 2;
	deltax = 0.15 * (xmaximum - xminimum);
	xscale = (xmax - xmin) / (xmaximum - xminimum);
	x_int = deltax * xscale;
	nx_int = (xmaximum - xminimum) / deltax + 1;
	yminimum = 0.0;
	ymaximum = maxdepth;
	deltay = 0.1 * (ymaximum - yminimum);
	yscale = (ymax - ymin) / (ymaximum - yminimum);
	y_int = deltay * yscale;
	ny_int = (ymaximum - yminimum) / deltay + 1;

	/* plot grid */
	xg_drawline(mbvt_xgid, xmin, ymin, xmin, ymax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	xg_drawline(mbvt_xgid, xmax, ymin, xmax, ymax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	for (i = 0; i < nx_int; i++) {
		xx = xmin + i * x_int;
		vx = xminimum + i * deltax;
		xg_drawline(mbvt_xgid, xx, ymin, xx, ymax, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, "%.1f", vx);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xx - swidth / 2, ymax + sascent + 5, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	xg_drawline(mbvt_xgid, xmin, ymin, xmax, ymin, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	xg_drawline(mbvt_xgid, xmin, ymax, xmax, ymax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	for (i = 0; i < ny_int; i++) {
		yy = ymin + i * y_int;
		vy = yminimum + i * deltay;
		xg_drawline(mbvt_xgid, xmin, yy, xmax, yy, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, "%.1f", vy);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xmin - swidth - 5, yy + sascent / 2, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	strcpy(string, "Water Sound Velocity Profiles");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xcen - swidth / 2, ymin - 2 * sascent + 10, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "Water Sound Velocity (m/s)");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xcen - swidth / 2, ymax + 2 * sascent + 10, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "Depth");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xmin - 2 * swidth - 10, ycen - sascent, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "(m)");
	xg_drawstring(mbvt_xgid, xmin - 2 * swidth, ycen + sascent, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

	/* turn clipping on */

	xg_setclip(mbvt_xgid, xmin, ymin, (xmax - xmin), (ymax - ymin));

	/* plot display profiles */
	for (i = 0; i < ndisplay; i++) {
		color = i % 3 + 2;
		for (j = 0; j < profile_display[i].n; j++) {
			xx = xmin + (profile_display[i].velocity[j] - xminimum) * xscale;
			yy = ymin + (profile_display[i].depth[j] - yminimum) * yscale;
			xx = MIN(xx, 32000);
			yy = MIN(yy, 32000);
			/*		xg_drawrectangle(mbvt_xgid, xx-2, yy-2, 4, 4,
			            pixel_values[MB_COLOR_color],XG_SOLIDLINE);*/
			if (j > 0)
				xg_drawline(mbvt_xgid, xxo, yyo, xx, yy, pixel_values[color], XG_SOLIDLINE);
			xxo = xx;
			yyo = yy;
		}
	}

	/* plot edit profile */
	if (edit) {
		for (j = 0; j < profile_edit.n; j++) {
			xx = xmin + (profile_edit.velocity[j] - xminimum) * xscale;
			yy = ymin + (profile_edit.depth[j] - yminimum) * yscale;
			xx = MIN(xx, 32000);
			yy = MIN(yy, 32000);
			xg_fillrectangle(mbvt_xgid, xx - 2, yy - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			if (j > 0) {
				xg_drawline(mbvt_xgid, xxo, yyo, xx, yy, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
			}
			xxo = xx;
			yyo = yy;
			edit_x[j] = xx;
			edit_y[j] = yy;
		}
	}

	/* now plot grid for Bathymetry Residuals */
	/* turn clip mask back to whole canvas */

	xg_setclip(mbvt_xgid, borders[0], borders[2], borders[1] - borders[0], borders[3] - borders[2]);

	/* set scaling */
	xrmin = 0.5 * (borders[1] - borders[0]) + 2 * margin;
	xrmax = borders[1] - 0.5 * margin;
	yrmin = margin;
	yrmax = 0.5 * (borders[3] - borders[2]);
	xrcen = xrmin + (xrmax - xrmin) / 2;
	yrcen = yrmin + (yrmax - yrmin) / 2;
	xrminimum = beam_first - 1.0;
	xrmaximum = beam_last + 1.0;
	deltaxr = (int)(0.1 * (xrmaximum - xrminimum));
	xrscale = (xrmax - xrmin) / (xrmaximum - xrminimum);
	xr_int = deltaxr * xrscale;
	nxr_int = (xrmaximum - xrminimum) / deltaxr + 1;
	yrminimum = -resrange;
	yrmaximum = resrange;
	deltayr = 0.1 * (yrmaximum - yrminimum);
	yrscale = (yrmax - yrmin) / (yrmaximum - yrminimum);
	yr_int = deltayr * yrscale;
	nyr_int = (yrmaximum - yrminimum) / deltayr / 2 + 1;

	/* plot grid */
	xg_drawline(mbvt_xgid, xrmin, yrmin, xrmin, yrmax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	xg_drawline(mbvt_xgid, xrmax, yrmin, xrmax, yrmax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	for (i = 0; i < nxr_int; i++) {
		xx = xrmin + i * xr_int;
		vx = xrminimum + i * deltaxr;
		xg_drawline(mbvt_xgid, xx, yrmin, xx, yrmax, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, "%.0f", vx);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xx - swidth / 2, yrmax + sascent + 5, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	xg_drawline(mbvt_xgid, xrmin, yrmin, xrmax, yrmin, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	xg_drawline(mbvt_xgid, xrmin, yrmax, xrmax, yrmax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	if (resrange > 100.0)
		sprintf(format_str, "%s", "%.0f");
	else if (resrange > 10.0)
		sprintf(format_str, "%s", "%.1f");
	else
		sprintf(format_str, "%s", "%.2f");
	for (i = 0; i < nyr_int; i++) {
		yy = yrcen + i * yr_int;
		vy = i * deltayr;
		xg_drawline(mbvt_xgid, xrmin, yy, xrmax, yy, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, format_str, vy);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xrmin - swidth - 5, yy + sascent / 2, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
		yy = yrcen - i * yr_int;
		vy = -i * deltayr;
		xg_drawline(mbvt_xgid, xrmin, yy, xrmax, yy, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, format_str, vy);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xrmin - swidth - 5, yy + sascent / 2, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	if (nbuffer > 0) {
		sprintf(string, "Depth Range:  minimum: %5.0f m   maximum: %5.0f m", bath_min, bath_max);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, borders[1] / 2 - swidth / 2, yrmin - 4 * sascent + 14, string, pixel_values[MB_COLOR_BLACK],
		              XG_SOLIDLINE);
	}
	strcpy(string, "Swath Bathymetry Beam Residuals");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xrcen - swidth / 2, yrmin - 2 * sascent + 10, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "Bathymetry Beam Number");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xrcen - swidth / 2, yrmax + 2 * sascent + 10, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "Residual");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xrmin - swidth - 30, yrcen - sascent, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "(m)");
	xg_drawstring(mbvt_xgid, xrmin - swidth - 10, yrcen + sascent, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

	/* turn clipping on for residual plot box */
	xg_setclip(mbvt_xgid, xrmin, yrmin, (xrmax - xrmin), (yrmax - yrmin));

	/* plot residuals */
	if (nbuffer > 0)
		for (i = 0; i < nbeams; i++) {
			if (nresidual[i] > 0) {
				xx = xrmin + (i - xrminimum) * xrscale;
				yy = yrmin + (residual[i] - yrminimum) * yrscale;
				xx = MIN(xx, 32000);
				yy = MIN(yy, 32000);
				yyl = yrmin + (residual[i] - res_sd[i] - yrminimum) * yrscale;
				yyu = yrmin + (residual[i] + res_sd[i] - yrminimum) * yrscale;
				xg_fillrectangle(mbvt_xgid, xx - 2, yy - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
				xg_drawline(mbvt_xgid, xx, yyl, xx, yyu, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
				if (i > 0 && nresidual[i - 1] > 0)
					xg_drawline(mbvt_xgid, xxo, yyo, xx, yy, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
				xxo = xx;
				yyo = yy;
			}
		}

	/* now plot grid for raypaths */
	/* turn clip mask back to whole canvas */

	xg_setclip(mbvt_xgid, borders[0], borders[2], borders[1] - borders[0], borders[3] - borders[2]);

	/* set scaling */
	xpmin = 2.25 * margin;
	xpmax = borders[1] - 0.5 * margin;
	ypmin = 0.5 * (borders[3] - borders[2]) + 1.5 * margin;
	ypmax = ypmin + (xpmax - xpmin) / 5.0;
	xpcen = xpmin + (xpmax - xpmin) / 2;
	ypcen = ypmin + (ypmax - ypmin) / 2;

	if (nbuffer == 0 || nraypath == NULL) {
		raydepthmin = 0.0;
		raydepthmax = maxdepth;
	}

	ypminimum = raydepthmin - 0.02 * (raydepthmax - raydepthmin);
	ypmaximum = raydepthmax + 0.02 * (raydepthmax - raydepthmin);
	ypscale = (ypmax - ypmin) / (ypmaximum - ypminimum);
	xpscale = ypscale;
	xpmaximum = (xpmax - xpmin) / (2 * xpscale);
	xpminimum = -xpmaximum;
	if (xpmaximum < rayxmax) {
		xpmaximum = 1.02 * rayxmax;
		xpminimum = -xpmaximum;
		xpscale = (xpmax - xpmin) / (xpmaximum - xpminimum);
		ypscale = xpscale;
		ypmaximum = ypminimum + (ypmax - ypmin) / ypscale;
	}

	deltaxp = 0.4 * (raydepthmax - raydepthmin);
	xp_int = deltaxp * xpscale;
	nxp_int = (xpmaximum - xpminimum) / deltaxp / 2 + 1;
	deltayp = 0.2 * (ypmaximum - ypminimum);
	yp_int = deltayp * ypscale;
	nyp_int = (ypmaximum - ypminimum) / deltayp + 1;

	/* plot grid */
	xg_drawline(mbvt_xgid, xpmin, ypmin, xpmin, ypmax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	xg_drawline(mbvt_xgid, xpmax, ypmin, xpmax, ypmax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	for (i = 0; i < nxp_int; i++) {
		xx = xpcen + i * xp_int;
		vx = i * deltaxp;
		xg_drawline(mbvt_xgid, xx, ypmin, xx, ypmax, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, "%.1f", vx);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xx - swidth / 2, ypmax + sascent + 5, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
		xx = xpcen - i * xp_int;
		vx = -i * deltaxp;
		xg_drawline(mbvt_xgid, xx, ypmin, xx, ypmax, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, "%.1f", vx);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xx - swidth / 2, ypmax + sascent + 5, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	xg_drawline(mbvt_xgid, xpmin, ypmin, xpmax, ypmin, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	xg_drawline(mbvt_xgid, xpmin, ypmax, xpmax, ypmax, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	for (i = 0; i < nyp_int; i++) {
		yy = ypmin + i * yp_int;
		vy = ypminimum + i * deltayp;
		xg_drawline(mbvt_xgid, xpmin, yy, xpmax, yy, pixel_values[MB_COLOR_BLACK], XG_DASHLINE);
		sprintf(string, "%.1f", vy);
		xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
		xg_drawstring(mbvt_xgid, xpmin - swidth - 5, yy + sascent / 2, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	strcpy(string, "Raypaths");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xpcen - swidth / 2, ypmin - 2 * sascent + 10, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "Acrosstrack Distance (m)");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xpcen - swidth / 2, ypmax + 2 * sascent + 10, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "Depth");
	xg_justify(mbvt_xgid, string, &swidth, &sascent, &sdescent);
	xg_drawstring(mbvt_xgid, xpmin - 2 * swidth - 10, ypcen - sascent, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	strcpy(string, "(m)");
	xg_drawstring(mbvt_xgid, xpmin - 2 * swidth, ypcen + sascent, string, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);

	/* turn clipping on for raypath plot box */
	xg_setclip(mbvt_xgid, xpmin, ypmin, (xpmax - xpmin), (ypmax - ypmin));

	/* plot raypaths */
	if (nbuffer > 0 && nraypath != NULL)
		for (i = 0; i < nbeams; i++) {
			if (nraypath[i] > 0) {
				xxo = xpmin + (raypathx[i][0] - xpminimum) * xpscale;
				yyo = ypmin + (raypathy[i][0] - ypminimum) * ypscale;
				for (j = 1; j < nraypath[i]; j++) {
					xx = xpmin + (raypathx[i][j] - xpminimum) * xpscale;
					yy = ypmin + (raypathy[i][j] - ypminimum) * ypscale;
					xx = MIN(xx, 32000);
					yy = MIN(yy, 32000);
					xg_drawline(mbvt_xgid, xxo, yyo, xx, yy, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
					xxo = xx;
					yyo = yy;
				}
				xg_fillrectangle(mbvt_xgid, xx - 2, yy - 2, 4, 4, pixel_values[MB_COLOR_RED], XG_SOLIDLINE);
			}
		}

	/* turn clipping on for velocity profile box */
	xg_setclip(mbvt_xgid, xmin, ymin, (xmax - xmin), (ymax - ymin));

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is called when the right mouse button is pressed     */
/*   when it is in the canvas area. It finds the mouse location so    */
/*   the program knows which editable point to move.                  */
/* Called by:                                                         */
/*                  action_canvas_event                               */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_action_select_node(int x, int y) {
	int status = MB_SUCCESS;
	double distance, distance_min;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       x:            %d\n", x);
		fprintf(stderr, "dbg2       y:            %d\n", y);
	}

	/* select node if possible */
	if (x >= xmin && x <= xmax && y >= ymin && y <= ymax) {

		/* find the closest node */
		distance_min = 20000.0;
		active = -1;
		for (i = 0; i < profile_edit.n; i++) {
			distance = (edit_x[i] - x) * (edit_x[i] - x) + (edit_y[i] - y) * (edit_y[i] - y);
			if (distance < distance_min) {
				distance_min = distance;
				active = i;
			}
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is called when the right mouse button is released.   */
/*   The only thing it really does is set the active flag to -1.      */
/* Called by:                                                         */
/*                  action_canvas_event                               */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_action_mouse_up(int x, int y) {
	int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       x:            %d\n", x);
		fprintf(stderr, "dbg2       y:            %d\n", y);
	}

	/* relocate and deselect node if selected */
	if (active > 0) {
		active = -1;
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is continuously called as long as the left mouse is  */
/*   is depressed. It moves the selected point with elastic lines     */
/*   until the button is released.                                   */
/* Called by:                                                         */
/*                  action_canvas_event                               */
/* Functions called:                                                  */
/*                  xg_fillrectangle                                  */
/*                  xg_drawline                                       */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_action_drag_node(int x, int y) {
	int status = MB_SUCCESS;
	int ylim_min, ylim_max;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       x:            %d\n", x);
		fprintf(stderr, "dbg2       y:            %d\n", y);
	}

	/* relocate node if selected */
	if (active > -1 && x >= xmin && x <= xmax && y >= ymin && y <= ymax) {
		/* find upper and lower bounds for current node */
		if (active == 0)
			ylim_min = ymin;
		else
			ylim_min = edit_y[active - 1];
		if (active == profile_edit.n - 1)
			ylim_max = ymax;
		else
			ylim_max = edit_y[active + 1];

		/* get new location */
		if (x <= xmin)
			x = xmin + 1;
		if (x >= xmax)
			x = xmax - 1;
		if (y <= ylim_min)
			y = ylim_min + 1;
		if (y >= ylim_max)
			y = ylim_max;
		if (active == 0)
			y = ymin;

		/* unplot the current ping */
		xg_fillrectangle(mbvt_xgid, edit_x[active] - 2, edit_y[active] - 2, 4, 4, pixel_values[MB_COLOR_WHITE], XG_SOLIDLINE);
		if (active > 0) {
			xg_drawline(mbvt_xgid, edit_x[active - 1], edit_y[active - 1], edit_x[active], edit_y[active], pixel_values[MB_COLOR_WHITE],
			            XG_SOLIDLINE);
		}
		if (active < profile_edit.n - 1) {
			xg_drawline(mbvt_xgid, edit_x[active], edit_y[active], edit_x[active + 1], edit_y[active + 1], pixel_values[MB_COLOR_WHITE],
			            XG_SOLIDLINE);
		}

		/* get new location and velocity values */
		edit_x[active] = x;
		edit_y[active] = y;
		profile_edit.velocity[active] = (x - xmin) / xscale + xminimum;
		profile_edit.depth[active] = (y - ymin) / yscale + yminimum;

		/* replot the current svp */
		if (active > 0) {
			xg_drawline(mbvt_xgid, edit_x[active - 1], edit_y[active - 1], edit_x[active], edit_y[active], pixel_values[MB_COLOR_BLACK],
			            XG_SOLIDLINE);
		}
		if (active < profile_edit.n - 1) {
			xg_drawline(mbvt_xgid, edit_x[active], edit_y[active], edit_x[active + 1], edit_y[active + 1], pixel_values[MB_COLOR_BLACK],
			            XG_SOLIDLINE);
		}
		if (active > 0)
			xg_fillrectangle(mbvt_xgid, edit_x[active - 1] - 2, edit_y[active - 1] - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
		xg_fillrectangle(mbvt_xgid, edit_x[active] - 2, edit_y[active] - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
		if (active < profile_edit.n - 1)
			xg_fillrectangle(mbvt_xgid, edit_x[active + 1] - 2, edit_y[active + 1] - 2, 4, 4, pixel_values[MB_COLOR_BLACK], XG_SOLIDLINE);
	}
	else
		status = MB_FAILURE;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is called when the middle mouse button is pressed    */
/*   when it is in the canvas area. It finds the mouse location so    */
/*   the program knows where to add a new svp node .                  */
/* Called by:                                                         */
/*                  action_canvas_event                               */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_action_add_node(int x, int y) {
	int status = MB_SUCCESS;
	int add_i, add_x = 0, add_y = 0;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       x:            %d\n", x);
		fprintf(stderr, "dbg2       y:            %d\n", y);
	}

	/* select node if possible */
	status = MB_FAILURE;
	if (x >= xmin && x <= xmax && y >= ymin && y <= ymax) {

		/* find the vertical place of new node */
		add_i = -1;
		for (i = 1; i < profile_edit.n && add_i == -1; i++) {
			if (y > edit_y[i - 1] && y < edit_y[i]) {
				add_i = i;
				add_x = x;
				add_y = y;
			}
			else if (y == edit_y[i]) {
				add_i = i;
				add_x = x;
				add_y = y - 1;
			}
		}

		/* add in the node */
		if (add_i > -1 && profile_edit.n < profile_edit.nalloc) {
			for (i = profile_edit.n - 1; i >= add_i; i--) {
				profile_edit.depth[i + 1] = profile_edit.depth[i];
				profile_edit.velocity[i + 1] = profile_edit.velocity[i];
				edit_x[i + 1] = edit_x[i];
				edit_y[i + 1] = edit_y[i];
			}
			profile_edit.n++;
			edit_x[add_i] = add_x;
			edit_y[add_i] = add_y;
			profile_edit.velocity[add_i] = (add_x - xmin) / xscale + xminimum;
			profile_edit.depth[add_i] = (add_y - ymin) / yscale + yminimum;

			status = MB_SUCCESS;

			mbvt_plot();
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function is called when the right mouse button is pressed    */
/*   when it is in the canvas area. It finds the mouse location so    */
/*   the program knows where to add a new svp node .                  */
/* Called by:                                                         */
/*                  action_canvas_event                               */
/* Functions called:                                                  */
/*                  none                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_action_delete_node(int x, int y) {
	int status = MB_SUCCESS;
	double distance, distance_min;
	int delete;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input values:\n");
		fprintf(stderr, "dbg2       x:            %d\n", x);
		fprintf(stderr, "dbg2       y:            %d\n", y);
	}

	/* select node if possible */
	status = MB_FAILURE;
	if (x >= xmin && x <= xmax && y >= ymin && y <= ymax) {

		/* find the closest node */
		distance_min = 10.0;
		delete = -1;
		for (i = 0; i < profile_edit.n; i++) {
			distance = (edit_x[i] - x) * (edit_x[i] - x) + (edit_y[i] - y) * (edit_y[i] - y);
			if (distance < distance_min) {
				distance_min = distance;
				delete = i;
			}
		}

		/* delete the node */
		if (delete > -1 && profile_edit.n > 2) {
			/* (the original copied from i+1 up to i = n-1, one read past the last node) */
			for (i = delete; i < profile_edit.n - 1; i++) {
				profile_edit.depth[i] = profile_edit.depth[i + 1];
				profile_edit.velocity[i] = profile_edit.velocity[i + 1];
				edit_x[i] = edit_x[i + 1];
				edit_y[i] = edit_y[i + 1];
			}
			profile_edit.n--;

			status = MB_SUCCESS;

			mbvt_plot();
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
int mbvt_get_format(char *file, int *form) {
	int status = MB_SUCCESS;
	mb_path tmp;
	int tform;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
		fprintf(stderr, "dbg2       format:      %d\n", *form);
	}

	/* get filenames */
	/* look for MB suffix convention */
	if ((status = mb_get_format(verbose, file, tmp, &tform, &error)) == MB_SUCCESS) {
		*form = tform;
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       format:      %d\n", *form);
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:     %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function reads the data from the multibeam file and makes    */
/*   the calls to display it.                                         */
/* Called by:                                                         */
/*                  action_process_mb                                 */
/*                  open_file_ok                                      */
/* Functions called:                                                  */
/*                  mb_mallocd                                        */
/*                  mb_format                                         */
/*                  mb_buffer_close                                   */
/*                  mb_close                                          */
/*                  mb_freed                                          */
/*                  mb_read_init                                      */
/*                  mb_error                                          */
/*                  mb_buffer_init                                    */
/*                  mb_buffer_load                                    */
/*                  mbvt_setup_raytracing                             */
/*                  mbvt_process_multibeam                           */
/*                  mbvt_plot                                         */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_open_swath_file(char *file, int form, int *numload) {
	int status = MB_SUCCESS;
	int kind;
	double navlon_levitus, navlat_levitus;
	double distance;
	double altitude;
	double sensordepth;
	bool variable_beams;
	bool traveltime;
	bool beam_flagging;
	mb_path string;
	char svp_file[2048];
	int count;
	struct stat file_status;
	int fstat;
	double rr, zz;
	int i, k;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       file:        %s\n", file);
		fprintf(stderr, "dbg2       format:      %d\n", form);
	}

	/* check for format with travel time data */
	format = form;
	if (format == 0)
		mb_get_format(verbose, file, NULL, &format, &error);
	status = mb_format_flags(verbose, &format, &variable_beams, &traveltime, &beam_flagging, &error);
	if (status == MB_FAILURE) {
		fprintf(stderr, "\nFormat id %d does not correspond to a supported format.\n", format);
		fprintf(stderr, "\nSwath Sonar File <%s> not initialized for reading\n", file);
		status = MB_FAILURE;
		do_error_dialog("Data loading aborted.", "The specified swath data", "format is incorrect!");
		return (status);
	}
	if (!traveltime) {
		fprintf(stderr, "\nProgram <%s> requires travel time data.\n", program_name);
		fprintf(stderr, "Format %d is does not include travel time data.\n", format);
		fprintf(stderr, "Travel times and angles are being estimated\n");
		fprintf(stderr, "assuming a 1500 m/s half-space\n");
		status = MB_FAILURE;
		do_error_dialog("Data doesn't include travel times!", "Travel times and angles estimated",
		                "assuming 1500 m/s sound speed.");
	}
	/* if (!traveltime)
	    {
	    fprintf(stderr,"\nProgram <%s> requires travel time data.\n",program_name);
	    fprintf(stderr,"Format %d is unacceptable because it does not include travel time data.\n",format);
	    fprintf(stderr,"\nSwath Sonar File <%s> not initialized for reading\n",file);
	    status = MB_FAILURE;
	    do_error_dialog("Data loading aborted.",
	            "The specified swath data format does",
	            "not include travel time data!");
	    return(status);
	    }*/

	/* deallocate previously loaded data, if any */
	mbvt_deallocate_swath();

	/* initialize reading the input multibeam file */
	strcpy(swathfile, file);
	if ((status = mb_read_init(verbose, swathfile, format, pings, lonflip, bounds, btime_i, etime_i, speedmin, timegap, &mbio_ptr,
	                           &btime_d, &etime_d, &beams_bath, &beams_amp, &pixels_ss, &error)) != MB_SUCCESS) {
		mb_error(verbose, error, &message);
		fprintf(stderr, "\nMBIO Error returned from function <mb_read_init>:\n%s\n", message);
		fprintf(stderr, "\nSwath Sonar File <%s> not initialized for reading\n", swathfile);
		status = MB_FAILURE;
		char error1[MB_PATH_MAXLINE];
		char error2[MB_PATH_MAXLINE];
		char error3[MB_PATH_MAXLINE];
		mb_file_open_error_message(swathfile, error1, error2, error3, sizeof(error1));
		do_error_dialog(error1, error2, error3);
		return (status);
	}

	/* turn message on */
	do_message_on("MBvelocitytool is loading data...");

	/* set beam_first and beam_last */
	beam_first = 0;
	beam_last = beams_bath;

	/* allocate memory for data arrays */
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(char), (void **)&beamflag, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bath, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_AMPLITUDE, sizeof(double), (void **)&amp, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathacrosstrack, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathalongtrack, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ss, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ssacrosstrack, &error);
	if (error == MB_ERROR_NO_ERROR)
		status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ssalongtrack, &error);

	/* if error initializing memory then quit */
	if (error != MB_ERROR_NO_ERROR) {
		mb_error(verbose, error, &message);
		fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
		mb_close(verbose, &mbio_ptr, &error);
		do_message_off();
		do_error_dialog("Data loading aborted.", "Unable to allocate memory", "for the swath data arrays!");
		return (MB_FAILURE);
	}

	/* initialize the buffer */
	nbuffer = 0;
	ssv_start = 0.0;
	navlon_levitus = 0.0;
	navlat_levitus = 0.0;

	/* turn message on */
	*numload = 0;
	sprintf(string, "MBvelocitytool: %d records loaded so far...", *numload);
	do_message_on(string);

	/* Load with ESF File if avialable */
	if (status == MB_SUCCESS) {
		status = mb_esf_load(verbose, program_name, swathfile, true, false, esffile, &esf, &error);
	}

	/* load data */
	do {
		status = mb_get_all(verbose, mbio_ptr, &store_ptr, &kind, ping[nbuffer].time_i, &ping[nbuffer].time_d,
		                    &ping[nbuffer].navlon, &ping[nbuffer].navlat, &ping[nbuffer].speed, &ping[nbuffer].heading, &distance,
		                    &altitude, &sensordepth, &ping[nbuffer].beams_bath, &namp, &nss, beamflag, bath, amp, bathacrosstrack,
		                    bathalongtrack, ss, ssacrosstrack, ssalongtrack, comment, &error);
		if (error <= MB_ERROR_NO_ERROR && (kind == MB_DATA_DATA) &&
		    (error == MB_ERROR_NO_ERROR || error == MB_ERROR_TIME_GAP || error == MB_ERROR_OUT_BOUNDS ||
		     error == MB_ERROR_SPEED_TOO_SMALL)) {
			status = MB_SUCCESS;
			error = MB_ERROR_NO_ERROR;
		}
		else if (error <= MB_ERROR_NO_ERROR) {
			status = MB_FAILURE;
			error = MB_ERROR_OTHER;
		}

		/* Apply ESF Edits if available */
		if (esf.nedit > 0 && error == MB_ERROR_NO_ERROR && kind == MB_DATA_DATA) {
			status = mb_esf_apply(verbose, &esf, ping[nbuffer].time_d, 0, ping[nbuffer].beams_bath, beamflag, &error);
		}

		if (status == MB_SUCCESS && ping[nbuffer].allocated > 0 && ping[nbuffer].allocated < ping[nbuffer].beams_bath) {
			ping[nbuffer].allocated = 0;
			free(ping[nbuffer].beamflag);
			free(ping[nbuffer].bath);
			free(ping[nbuffer].bathacrosstrack);
			free(ping[nbuffer].bathalongtrack);
			free(ping[nbuffer].ttimes);
			free(ping[nbuffer].angles);
			free(ping[nbuffer].angles_forward);
			free(ping[nbuffer].angles_null);
			free(ping[nbuffer].heave);
			free(ping[nbuffer].alongtrack_offset);
		}
		if (status == MB_SUCCESS && ping[nbuffer].allocated < ping[nbuffer].beams_bath) {
			ping[nbuffer].beamflag = NULL;
			ping[nbuffer].bath = NULL;
			ping[nbuffer].bathacrosstrack = NULL;
			ping[nbuffer].bathalongtrack = NULL;
			ping[nbuffer].ttimes = NULL;
			ping[nbuffer].angles = NULL;
			ping[nbuffer].angles_forward = NULL;
			ping[nbuffer].angles_null = NULL;
			ping[nbuffer].heave = NULL;
			ping[nbuffer].alongtrack_offset = NULL;
			ping[nbuffer].beamflag = (char *)malloc(ping[nbuffer].beams_bath * sizeof(char));
			ping[nbuffer].bath = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].bathacrosstrack = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].bathalongtrack = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].ttimes = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].angles = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].angles_forward = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].angles_null = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].heave = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].alongtrack_offset = (double *)malloc(ping[nbuffer].beams_bath * sizeof(double));
			ping[nbuffer].allocated = ping[nbuffer].beams_bath;
		}
		if (status == MB_SUCCESS && beams_bath < ping[nbuffer].beams_bath)
			beams_bath = ping[nbuffer].beams_bath;

		if (status == MB_SUCCESS && ping[nbuffer].allocated > 0) {
			for (i = 0; i < ping[nbuffer].beams_bath; i++) {
				ping[nbuffer].beamflag[i] = beamflag[i];
				ping[nbuffer].bath[i] = bath[i];
				ping[nbuffer].bathacrosstrack[i] = bathacrosstrack[i];
				ping[nbuffer].bathalongtrack[i] = bathalongtrack[i];
			}
			if (traveltime) {
				status = mb_ttimes(verbose, mbio_ptr, store_ptr, &kind, &nbeams, ping[nbuffer].ttimes, ping[nbuffer].angles,
				                   ping[nbuffer].angles_forward, ping[nbuffer].angles_null, ping[nbuffer].heave,
				                   ping[nbuffer].alongtrack_offset, &ping[nbuffer].sensordepth, &ping[nbuffer].ssv, &error);
			}
			else {
				nbeams = ping[nbuffer].beams_bath;
				ping[nbuffer].sensordepth = sensordepth;
				ping[nbuffer].ssv = 1500.0;
				for (i = 0; i < ping[nbuffer].beams_bath; i++) {
					if (mb_beam_ok(ping[nbuffer].beamflag[i])) {
						zz = bath[i] - sensordepth;
						rr = sqrt(zz * zz + bathacrosstrack[i] * bathacrosstrack[i] + bathalongtrack[i] * bathalongtrack[i]);
						ping[nbuffer].ttimes[i] = rr / 750.0;
						mb_xyz_to_takeoff(verbose, bathacrosstrack[i], bathalongtrack[i], (bath[i] - sensordepth),
						                  &ping[nbuffer].angles[i], &ping[nbuffer].angles_forward[i], &error);
						ping[nbuffer].angles_null[i] = 0.0;
						ping[nbuffer].heave[i] = 0.0;
						ping[nbuffer].alongtrack_offset[i] = 0.0;
					}
				}
			}

			/* get first nav */
			if (navlon_levitus == 0.0 && navlat_levitus == 0.0) {
				navlon_levitus = ping[nbuffer].navlon;
				navlat_levitus = ping[nbuffer].navlat;
			}

			/* check for first nonzero ssv */
			if (ping[nbuffer].ssv > 0.0 && ssv_start == 0.0)
				ssv_start = ping[nbuffer].ssv;
		}
		if (status == MB_SUCCESS) {
			nbuffer++;
			(*numload)++;

			/* update message every 250 records */
			if ((*numload) % 250 == 0) {
				sprintf(string, "MBvelocitytool: %d records loaded so far...", *numload);
				do_message_on(string);
			}
		}
	} while (error <= MB_ERROR_NO_ERROR && nbuffer < MBVT_BUFFER_SIZE);

	/* close input file */
	status = mb_close(verbose, &mbio_ptr, &error);

	/* Close ESF file if avialable */
	if (esf.edit != NULL || esf.esffp != NULL)
		mb_esf_close(verbose, &esf, &error);

	/* define success */
	if (nbuffer > 0) {
		status = MB_SUCCESS;
		error = MB_ERROR_NO_ERROR;
	}

	/* turn message off -- before the error dialog below, which the busy notice would cover */
	do_message_off();

	/* set error message */
	if (nbuffer <= 0)
		do_error_dialog("No data were read from the input", "swath file. You may have specified an",
		                "incorrect MB-System format id!");

	/* if successful, process and display */
	if (status == MB_SUCCESS && nbuffer > 0) {

		/* allocate resicual arrays to accommodate greatest number of beams
		   (check status after every call -- mb_mallocd() resets *error to
		   MB_ERROR_NO_ERROR on each successful call, so a later success would
		   otherwise silently mask an earlier allocation failure and let the
		   NULL result be used below) */
		status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&depth, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&acrosstrack, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&angle, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&residual_acrosstrack, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&residual_altitude, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&residual, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double), (void **)&res_sd, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(int), (void **)&nresidual, &error);

		/* if error initializing memory then quit rather than use the NULL result below */
		if (status != MB_SUCCESS) {
			mb_error(verbose, error, &message);
			fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
			mbvt_deallocate_swath();
			do_error_dialog("Data loading aborted.", "Unable to allocate memory", "for the residual arrays!");
			return (MB_FAILURE);
		}

		if (ssv_start <= 0.0)
			ssv_start = 1500.0;

		/* get approximate min max depths */
		bath_min = 10000.0;
		bath_max = 0.0;
		for (k = 0; k < nbuffer; k++) {
			/* loop over the beams */
			for (i = 0; i < ping[k].beams_bath; i++) {
				if (mb_beam_ok(ping[k].beamflag[i])) {
					depth[i] = 750 * ping[k].ttimes[i] * cos(DTR * ping[k].angles[i]) + ping[k].sensordepth + ping[k].heave[i];

					/* get min max depths */
					if (depth[i] < bath_min)
						bath_min = depth[i];
					if (depth[i] > bath_max)
						bath_max = depth[i];
				}
			}
		}

		/* set maxdepth and apply */
		if (bath_max > 0.0 && bath_max < 13000.0) {
			maxdepth = 1.25 * bath_max;
			resrange = 0.02 * bath_max;
			if (resrange > 200.0)
				resrange = 200.0;
			else if (resrange < 0.1)
				resrange = 0.1;
			do_set_controls();
		}

		/* output info */
		if (verbose >= 1) {
			if (status == MB_SUCCESS)
				fprintf(stderr, "\nSwath Sonar File <%s> read\n", swathfile);
			else
				fprintf(stderr, "\nSwath Sonar File <%s> not read\n", swathfile);
			fprintf(stderr, "Swath Sonar Data Format ID:   %d\n", format);
			fprintf(stderr, "Records loaded into buffer: %d\n", *numload);
			fprintf(stderr, "Records in buffer:          %d\n", nbuffer);
		}

		/* get editable svp if needed */
		if (edit != true)
			mbvt_new_edit_profile();

		/* add Levitus display profile if nav available */
		if (navlon_levitus != 0.0 || navlat_levitus != 0.0) {
			/* mblevitus -R<lon>/<lat> -Ombvt_levitus_tmp.svp, then that file opened as a display
			   profile and removed: done here in-process, straight into the display list */
			mbvt_levitus_profile(navlon_levitus, navlat_levitus);
		}

		/* load svp files generated by mbsvplist if available */
		bool done = false;
		count = 0;
		while (!done) {
			sprintf(svp_file, "%s_%3.3d.svp", swathfile, count);
			if ((fstat = stat(svp_file, &file_status)) == 0 && (file_status.st_mode & S_IFMT) != S_IFDIR) {
				mbvt_open_display_profile(svp_file);
			}
			else if (count > 0)
				done = true;
			count++;
		}

		/* allocate memory for raytracing arrays */
		status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(int), (void **)&nraypath, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double *), (void **)&raypathx, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double *), (void **)&raypathy, &error);
		if (status == MB_SUCCESS)
			status = mb_mallocd(verbose, __FILE__, __LINE__, beams_bath * sizeof(double *), (void **)&raypatht, &error);
		nraypathmax = 100 * profile_edit.n;
		for (i = 0; status == MB_SUCCESS && i < beams_bath; i++) {
			status = mb_mallocd(verbose, __FILE__, __LINE__, nraypathmax * sizeof(double), (void **)&(raypathx[i]), &error);
			if (status == MB_SUCCESS)
				status = mb_mallocd(verbose, __FILE__, __LINE__, nraypathmax * sizeof(double), (void **)&(raypathy[i]), &error);
			if (status == MB_SUCCESS)
				status = mb_mallocd(verbose, __FILE__, __LINE__, nraypathmax * sizeof(double), (void **)&(raypatht[i]), &error);
		}

		/* if error initializing memory then quit */
		if (status != MB_SUCCESS) {
			mb_error(verbose, error, &message);
			fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", message);
			mbvt_deallocate_swath();
			do_error_dialog("Data loading aborted.", "Unable to allocate memory", "for the raytracing arrays!");
			return (MB_FAILURE);
		}

		/* process the data */
		if (status == MB_SUCCESS && edit)
			status = mbvt_process_multibeam();

		/* plot everything */
		mbvt_plot();
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       numload:    %d\n", *numload);
		fprintf(stderr, "dbg2       error:      %d\n", error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:     %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------*/
/* This function deallocates swath data.                              */
/* Called by:                                                         */
/*                  mbvt_open_swath_file                              */
/*                  mbvt_quit                                         */
/* Functions called:                                                  */
/*                  mb_freed                                          */
/*                  free                                              */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_deallocate_swath(void) {
	int status = MB_SUCCESS;
	int i;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* deallocate previously loaded data, if any */
	if (nbuffer > 0) {
		if (raypathx != NULL && raypathy != NULL && raypatht != NULL)
			for (i = 0; i < beams_bath; i++) {
				mb_freed(verbose, __FILE__, __LINE__, (void **)&(raypathx[i]), &error);
				mb_freed(verbose, __FILE__, __LINE__, (void **)&(raypathy[i]), &error);
				mb_freed(verbose, __FILE__, __LINE__, (void **)&(raypatht[i]), &error);
			}
		mb_freed(verbose, __FILE__, __LINE__, (void **)&nraypath, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&raypathx, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&raypathy, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&raypatht, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&depth, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&acrosstrack, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&angle, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&residual_acrosstrack, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&residual_altitude, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&residual, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&res_sd, &error);
		mb_freed(verbose, __FILE__, __LINE__, (void **)&nresidual, &error);
		nraypath = NULL;
		raypathx = NULL;
		raypathy = NULL;
		raypatht = NULL;
		depth = NULL;
		acrosstrack = NULL;
		angle = NULL;
		residual_acrosstrack = NULL;
		residual_altitude = NULL;
		residual = NULL;
		res_sd = NULL;
		nresidual = NULL;

		for (i = 0; i < MBVT_BUFFER_SIZE; i++) {
			if (ping[i].allocated > 0) {
				ping[i].allocated = 0;
				free(ping[i].beamflag);
				free(ping[i].bath);
				free(ping[i].bathacrosstrack);
				free(ping[i].bathalongtrack);
				free(ping[i].ttimes);
				free(ping[i].angles);
				free(ping[i].angles_forward);
				free(ping[i].angles_null);
				free(ping[i].heave);
				free(ping[i].alongtrack_offset);
			}
		}
		nbuffer = 0;
	}

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
/* This function is called when the "PROCESS MULTIBEAM"               */
/*   selection is made from the menu bar.                             */
/* Called by:                                                         */
/*                  action_process_mb                                 */
/* Functions called:                                                  */
/*                  mb_ttimes                                         */
/* Function returns:                                                  */
/*                  status                                            */
/*--------------------------------------------------------------------*/
int mbvt_process_multibeam(void) {
	int status = MB_SUCCESS;
	double *dep;
	double *vel;
	int nvel;
	void *rt_svp = NULL;
	bool first;
	double ttime;
	int ray_stat;
	double factor;
	double sx, sy, sxx, sxy;
	double delta, a = 0.0, b = 0.0;
	int ns;
	double depth_predict, res;
	double sensordepth, sensordepthshift, heave_use;
	bool found;
	int i, j, k;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
	}

	/* check for data and velocity profile */
	if (profile_edit.n <= 0) {
		fprintf(stderr, "\nNo edit velocity profile available - bathymetry processing aborted.\n");
		status = MB_FAILURE;
		return (status);
	}
	if (nbuffer <= 0) {
		fprintf(stderr, "\nNo swath sonar data available - bathymetry processing aborted.\n");
		status = MB_FAILURE;
		return (status);
	}

	/* turn message on */
	do_message_on("MBvelocitytool is processing data...");

	/* initialize residuals and raypaths */
	for (i = 0; i < beams_bath; i++) {
		angle[i] = 0.0;
		residual_altitude[i] = 0.0;
		residual_acrosstrack[i] = 0.0;
		residual[i] = 0.0;
		res_sd[i] = 0.0;
		nresidual[i] = 0;
		nraypath[i] = 0;
	}

	/* initialize min-max variables */
	bath_min = 10000.0;
	bath_max = 0.0;

	/* set up raytracing */
	nvel = profile_edit.n;
	vel = profile_edit.velocity;
	dep = profile_edit.depth;
	status = mb_rt_init(verbose, nvel, dep, vel, (void **)&rt_svp, &error);
	first = true;
	nbeams = 0;
	rayxmax = 0.0;
	raydepthmin = 10000;
	raydepthmax = 0.0;

	/* loop over the data records */
	for (k = 0; k < nbuffer; k++) {
		/* initialize linear fit variables */
		sx = 0.0;
		sy = 0.0;
		sxx = 0.0;
		sxy = 0.0;
		ns = 0;
		depth_predict = 0.0;
		res = 0.0;

		/* set surface sound speed to default if needed */
		if (ping[k].ssv <= 0.0)
			ping[k].ssv = ssv_start;
		else
			ssv_start = ping[k].ssv;

		/* find a good heave value */
		found = false;
		heave_use = 0.0;
		for (i = 0; i < ping[k].beams_bath && !found; i++) {
			if (mb_beam_ok(ping[k].beamflag[i])) {
				heave_use = ping[k].heave[i];
				found = true;
			}
		}

		sensordepth = heave_use + ping[k].sensordepth;
		sensordepthshift = 0.0;
		if (first)
			raydepthmin = MIN(raydepthmin, sensordepth);
		if (sensordepth < 0.0) {
			sensordepthshift = sensordepth;
			sensordepth = 0.0;
		}

		/* loop over the beams */
		for (i = 0; i < ping[k].beams_bath; i++) {
			/* trace the ray */
			if (mb_beam_ok(ping[k].beamflag[i])) {
				/* get max beam id */
				nbeams = MAX(nbeams, i + 1);

				/* get factor relating lateral distance to
				    acrosstrack distance */
				factor = cos(DTR * ping[k].angles_forward[i]);

				/* trace rays */
				if (!first) {
					/* call raytracing without keeping
					plotting list */
					status =
					    mb_rt(verbose, rt_svp, sensordepth, ping[k].angles[i], 0.5 * ping[k].ttimes[i], anglemode, ping[k].ssv,
					          ping[k].angles_null[i], 0, NULL, NULL, NULL, NULL, &acrosstrack[i], &depth[i], &ttime, &ray_stat, &error);
				}
				else {
					/* call raytracing keeping
					plotting list */
					status = mb_rt(verbose, rt_svp, sensordepth, ping[k].angles[i], 0.5 * ping[k].ttimes[i],
					               anglemode, ping[k].ssv, ping[k].angles_null[i],
					               nraypathmax, &nraypath[i], raypathx[i], raypathy[i], raypatht[i],
					               &acrosstrack[i], &depth[i], &ttime, &ray_stat, &error);

					/* reset acrosstrack distances */
					for (j = 0; j < nraypath[i]; j++)
						raypathx[i][j] = factor * raypathx[i][j];
				}

				/* get acrosstrack distance */
				acrosstrack[i] = factor * acrosstrack[i];

				/* add to depth if needed */
				depth[i] += sensordepthshift;

				/* get min max depths */
				if (depth[i] < bath_min)
					bath_min = depth[i];
				if (depth[i] > bath_max)
					bath_max = depth[i];
				if (first) {
					rayxmax = MAX(rayxmax, fabs(acrosstrack[i]));
					raydepthmax = MAX(raydepthmax, depth[i]);
				}

				/* output some debug values */
				if (verbose >= 5)
					fprintf(stderr, "dbg5       %3d %3d %6.3f %6.3f %8.2f %8.2f %8.2f %8.2f\n", k, i, 0.5 * ping[k].ttimes[i],
					        ping[k].angles[i], acrosstrack[i], ping[k].heave[i], ping[k].sensordepth, depth[i]);

				/* get sums for linear fit */
				sx += acrosstrack[i];
				sy += depth[i];
				sxx += acrosstrack[i] * acrosstrack[i];
				sxy += acrosstrack[i] * depth[i];
				ns++;
			}
		}

		/* reset first flag */
		first = false;

		/* get linear fit to ping -- need at least two points for a well-defined
		   fit; with exactly one point delta is exactly zero (0/0 = NaN) */
		if (ns > 1) {
			delta = ns * sxx - sx * sx;
			if (delta != 0.0) {
				a = (sxx * sy - sx * sxy) / delta;
				b = (ns * sxy - sx * sy) / delta;
			}
		}

		/* get residuals */
		if (ns > 1) {
			/* output some debug values */
			if (verbose >= 5)
				fprintf(stderr, "dbg5       beam   xtrack   depth     fit    residual\n");

			/* loop over all beams */
			for (i = 0; i < ping[k].beams_bath; i++)
				if (mb_beam_ok(ping[k].beamflag[i])) {
					depth_predict = a + b * acrosstrack[i];
					res = depth[i] - depth_predict;
					angle[i] += ping[k].angles[i];
					residual_altitude[i] += depth[i] - sensordepth;
					residual_acrosstrack[i] += acrosstrack[i];
					residual[i] += res;
					res_sd[i] += res * res;
					nresidual[i]++;

					/* output some debug values */
					if (verbose >= 5)
						fprintf(stderr, "dbg5       %4d %10f %10f %10f %10f\n", i, acrosstrack[i], depth[i], depth_predict, res);
				}
		}
	}

	/* end raytracing */
	status = mb_rt_deall(verbose, (void **)&rt_svp, &error);

	/* calculate final residuals */
	beam_first = nbeams;
	beam_last = -1;
	for (i = 0; i < nbeams; i++)
		if (nresidual[i] > 0) {
			angle[i] = angle[i] / nresidual[i];
			residual_acrosstrack[i] = residual_acrosstrack[i] / nresidual[i];
			residual_altitude[i] = residual_altitude[i] / nresidual[i];
			residual[i] = residual[i] / nresidual[i];
			res_sd[i] = sqrt(res_sd[i] / nresidual[i] - residual[i] * residual[i]);
			if (i < beam_first)
				beam_first = i;
			if (i > beam_last)
				beam_last = i;
		}

	/* output residuals and stuff */
	if (verbose >= 1) {
		fprintf(stderr, "\nCurrent Bathymetry Depth Range:\n");
		fprintf(stderr, "\tminimum depth: %f\n", bath_min);
		fprintf(stderr, "\tmaximum depth: %f\n", bath_max);
		fprintf(stderr, "\nSwath Bathymetry Beam Residuals:\n");
		fprintf(stderr, " beam   angle   acrosstrack   altitude   residual     sigma  calculations\n");
		for (i = 0; i < nbeams; i++)
			fprintf(stderr, " %4d  %7.3f  %9.3f   %9.3f  %9.3f  %9.3f  %5d\n", i, angle[i], residual_acrosstrack[i],
			        residual_altitude[i], residual[i], res_sd[i], nresidual[i]);
	}
	/* turn message off */
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

/*====================================================================*/
/* Port additions                                                     */
/*====================================================================*/

/*--------------------------------------------------------------------*/
/* The module that holds the swath editor's MBIO table: the library mbedit_mbio_open loaded. */
#ifdef _WIN32
static void *mbvt_mbio_module(char *path, int pathlen) {
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
static void *mbvt_dlsym(void *h, const char *name) {
	return (void *)GetProcAddress((HMODULE)h, name);
}
#else
static void *mbvt_mbio_module(char *path, int pathlen) {
	Dl_info info;
	if (dladdr((void *)mbedit_mbio.get_all, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	snprintf(path, pathlen, "%s", info.dli_fname);
	/* the library is already loaded: this only hands back its handle */
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}
static void *mbvt_dlsym(void *h, const char *name) {
	return dlsym(h, name);
}
#endif

int mbvt_mbio_open(char *msg, int msglen) {
	if (msg && msglen > 0)
		msg[0] = '\0';
	if (mbvt_mbio_ok)
		return 1;
	if (!mbedit_mbio_loaded()) {
		snprintf(msg, msglen, "The MB-System MBIO library is not loaded");
		return 0;
	}
	char libpath[MB_PATH_MAXLINE] = "";
	void *h = mbvt_mbio_module(libpath, (int)sizeof(libpath));
	if (h == NULL) {
		snprintf(msg, msglen, "Cannot find the module of the loaded MBIO library");
		return 0;
	}

	struct mbvt_mbio_table t;
	memset(&t, 0, sizeof(t));
	struct {
		void **slot;
		const char *name;
	} syms[] = {
		{(void **)&t.format_flags, "mb_format_flags"},
		{(void **)&t.ttimes, "mb_ttimes"},
		{(void **)&t.xyz_to_takeoff, "mb_xyz_to_takeoff"},
		{(void **)&t.rt_init, "mb_rt_init"},
		{(void **)&t.rt_deall, "mb_rt_deall"},
		{(void **)&t.rt, "mb_rt"},
		{(void **)&t.pr_get_svp, "mb_pr_get_svp"},
		{(void **)&t.pr_update_svp, "mb_pr_update_svp"},
		{(void **)&t.pr_get_static, "mb_pr_get_static"},
		{(void **)&t.pr_update_static, "mb_pr_update_static"},
	};
	for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		*syms[i].slot = mbvt_dlsym(h, syms[i].name);
		if (*syms[i].slot == NULL) {
			snprintf(msg, msglen, "%s does not export %s, which mbvelocitytool needs", libpath, syms[i].name);
			return 0;
		}
	}

	/* 5.7.x or 5.8.x, told apart as the swath editor does: 5.7.x has no mb_version (or reports 5.7) */
	bool legacy = (mbedit_mbio.version == NULL);
	if (!legacy) {
		char vstring[MB_PATH_MAXLINE] = "";
		int vid = 0, vmajor = 0, vminor = 0, varchive = 0, verror = MB_ERROR_NO_ERROR;
		mbedit_mbio.version(0, vstring, &vid, &vmajor, &vminor, &varchive, &verror);
		legacy = (vminor == 7);
	}

	/* the Levitus database ships with MB-System: <prefix>/share/mbsystem, beside the <prefix>/bin
	   (or lib) folder the library is in -- of the library's REAL folder (a library reached through a
	   linked folder reports the link), else of the folder as named; INTERACTIVEGMT_LEVITUS overrides.
	   An MBIO used straight from an MB-System BUILD tree (<src>/build/src/mbio/mbio.dll) has no
	   share/mbsystem: there the database is the source tree's src/share/LevitusAnnual82.dat. So each
	   folder above the library, up to four, is asked for either layout. */
	mbvt_levitus_path[0] = '\0';
	{
		char cand[17][MB_PATH_MAXLINE];
		int ncand = 0;
		const char *env = getenv("INTERACTIVEGMT_LEVITUS");
		if (env != NULL && env[0] != '\0')
			snprintf(cand[ncand++], MB_PATH_MAXLINE, "%s", env);
		char real[MB_PATH_MAXLINE] = "";
#ifdef _WIN32
		{
			wchar_t wlib[MB_PATH_MAXLINE], wreal[MB_PATH_MAXLINE];
			if (MultiByteToWideChar(CP_UTF8, 0, libpath, -1, wlib, MB_PATH_MAXLINE) > 0) {
				HANDLE fh = CreateFileW(wlib, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
				                        FILE_FLAG_BACKUP_SEMANTICS, NULL);
				if (fh != INVALID_HANDLE_VALUE) {
					const DWORD n = GetFinalPathNameByHandleW(fh, wreal, MB_PATH_MAXLINE, FILE_NAME_NORMALIZED);
					CloseHandle(fh);
					if (n > 0 && n < MB_PATH_MAXLINE) {
						const wchar_t *w = wreal;
						if (wcsncmp(w, L"\\\\?\\", 4) == 0)
							w += 4;
						WideCharToMultiByte(CP_UTF8, 0, w, -1, real, MB_PATH_MAXLINE, NULL, NULL);
					}
				}
			}
		}
#else
		if (realpath(libpath, real) == NULL)
			real[0] = '\0';
#endif
		const char *bases[2] = {real, libpath};
		for (int b = 0; b < 2; b++) {
			if (bases[b][0] == '\0')
				continue;
			char dir[MB_PATH_MAXLINE];
			snprintf(dir, sizeof(dir), "%s", bases[b]);
			for (int up = 0; up < 5; up++) {          /* the file name, then one folder at a time */
				char *sep = strrchr(dir, '/');
				char *sep2 = strrchr(dir, '\\');
				if (sep2 > sep)
					sep = sep2;
				if (sep == NULL)
					break;
				*sep = '\0';
				if (up == 0 || dir[0] == '\0' || ncand + 2 > 17)
					continue;                          /* the library's own folder holds no share/ */
				snprintf(cand[ncand++], MB_PATH_MAXLINE, "%s/share/mbsystem/LevitusAnnual82.dat", dir);   /* installed */
				snprintf(cand[ncand++], MB_PATH_MAXLINE, "%s/src/share/LevitusAnnual82.dat", dir);        /* build tree */
			}
		}
		for (int i = 0; i < ncand; i++) {
			struct stat st;
			if (stat(cand[i], &st) == 0) {
				snprintf(mbvt_levitus_path, sizeof(mbvt_levitus_path), "%s", cand[i]);
				break;
			}
		}
		if (mbvt_levitus_path[0] == '\0' && ncand > 0)    /* none there: name the installed layout's, for the message */
			snprintf(mbvt_levitus_path, sizeof(mbvt_levitus_path), "%s", cand[env != NULL && env[0] != '\0' && ncand > 1 ? 1 : 0]);
	}

	mbvt_mbio = t;
	mbvt_mbio_legacy = legacy;
	mbvt_mbio_ok = true;
	return 1;
}

/*--------------------------------------------------------------------*/
/* mb_format_flags: 5.7.x writes ints, 5.8.x bools. */
static int mbvt_format_flags(int verbose, int *format, bool *variable_beams, bool *traveltime, bool *beam_flagging, int *error) {
	if (mbvt_mbio_legacy) {
		int vb = 0, tt = 0, bf = 0;
		const int status = mbvt_mbio.format_flags(verbose, format, &vb, &tt, &bf, error);
		*variable_beams = (vb != 0);
		*traveltime = (tt != 0);
		*beam_flagging = (bf != 0);
		return status;
	}
	return mbvt_mbio.format_flags(verbose, format, variable_beams, traveltime, beam_flagging, error);
}

/* mb_rt: 5.8.x keeps a travel-time list beside the plotted ray (tplot); 5.7.x has no such
   argument. The tool never plots the travel times, so 5.7 is called without them. */
static int mbvt_rt(int verbose, void *modelptr, double source_depth, double source_angle, double end_time, int ssv_mode,
                   double surface_vel, double null_angle, int nplot_max, int *nplot, double *xplot, double *zplot, double *tplot,
                   double *x, double *z, double *travel_time, int *ray_stat, int *error) {
	if (mbvt_mbio_legacy) {
		int (*rt57)(int, void *, double, double, double, int, double, double, int, int *, double *, double *, double *, double *,
		            double *, int *, int *) =
		    (int (*)(int, void *, double, double, double, int, double, double, int, int *, double *, double *, double *, double *,
		             double *, int *, int *))mbvt_mbio.rt;
		return rt57(verbose, modelptr, source_depth, source_angle, end_time, ssv_mode, surface_vel, null_angle, nplot_max, nplot,
		            xplot, zplot, x, z, travel_time, ray_stat, error);
	}
	int (*rt58)(int, void *, double, double, double, int, double, double, int, int *, double *, double *, double *, double *,
	            double *, double *, int *, int *) =
	    (int (*)(int, void *, double, double, double, int, double, double, int, int *, double *, double *, double *, double *,
	             double *, double *, int *, int *))mbvt_mbio.rt;
	return rt58(verbose, modelptr, source_depth, source_angle, end_time, ssv_mode, surface_vel, null_angle, nplot_max, nplot, xplot,
	            zplot, tplot, x, z, travel_time, ray_stat, error);
}

/*--------------------------------------------------------------------*/
/* mb_user_host_date -- MB-System 5.8.3 mbio/mb_defaults.c, unchanged (5.7.x does not export it).
   gethostname() fails on Windows without a Winsock start-up, which is why the original falls back
   to USERDOMAIN; on Windows that fallback is all that runs. */
static int mbvt_user_host_date(int verbose, char user[256], char host[256], char date[32], int *error) {
	(void)verbose;
	(void)error;
	memset(user, 0, 256);
	char *user_ptr = NULL;
	char *unknown = "Unknown";
	if ((user_ptr = getenv("USER")) == NULL)
		if ((user_ptr = getenv("LOGNAME")) == NULL)
			user_ptr = unknown;
	strncpy(user, user_ptr, 255);

	memset(host, 0, 256);
#ifndef _WIN32
	gethostname(host, 255);
#endif
	if (host[0] == '\0') {
		const char *host_ptr = getenv("USERDOMAIN");
		if (host_ptr != NULL)
			strncpy(host, host_ptr, 255);
	}

	memset(date, 0, 32);
	time_t right_now = time((time_t *)0);
	strncpy(date, ctime(&right_now), 31);
	date[strlen(date) - 1] = '\0';  // trim line return

	return (MB_SUCCESS);
}

/* mb_mallocd / mb_freed without MBIO's allocation list: zeroed memory, the pointer cleared on free */
static int mbvt_mallocd(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error) {
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

static int mbvt_freed(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error) {
	(void)verbose;
	(void)sourcefile;
	(void)sourceline;
	free(*ptr);
	*ptr = NULL;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
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
/* mblevitus -- MB-System src/utilities/mblevitus.cc: the mean annual water velocity profile of the
   1x1 degree Levitus cell holding (longitude, latitude), from the temperature and salinity of the
   Levitus database and the DelGrosso sound speed equation. The calculation is the program's,
   unchanged; instead of an output file the profile becomes the next display profile, exactly as
   mbvelocitytool did with the file mblevitus wrote. */
#define MBLEVITUS_NO_DATA (-1000000000.0)
#define MBLEVITUS_NDEPTH_MAX 46
#define MBLEVITUS_NLEVITUS_MAX 33

static void mbvt_swap_float(float *a) {
	unsigned char *b = (unsigned char *)a;
	unsigned char t = b[0];
	b[0] = b[3];
	b[3] = t;
	t = b[1];
	b[1] = b[2];
	b[2] = t;
}

static int mbvt_levitus_profile(double longitude, double latitude) {
	static const float ldepth[MBLEVITUS_NDEPTH_MAX] =
	    {0.0,    10.0,    20.0,    30.0,    50.0,    75.0,   100.0,  125.0,
	     150.0,  200.0,   250.0,   300.0,   400.0,   500.0,  600.0,  700.0,
	     800.0,  900.0,   1000.0,  1100.0,  1200.0,  1300.0, 1400.0, 1500.0,
	     1750.0, 2000.0,  2500.0,  3000.0,  3500.0,  4000.0, 4500.0, 5000.0,
	     5500.0, 6000.0,  6500.0,  7000.0,  7500.0,  8000.0, 8500.0, 9000.0,
	     9500.0, 10000.0, 10500.0, 11000.0, 11500.0, 12000.0};
	static float temperature[MBLEVITUS_NLEVITUS_MAX][180];
	static float salinity[MBLEVITUS_NLEVITUS_MAX][180];

	/* check that there is room for these data (mbvt_open_display_profile's check) */
	if (ndisplay >= MAX_PROFILES) {
		fprintf(stderr, "\nNo room for another display velocity profile\n");
		do_error_dialog("Unable to open input SVP file.", "There is no room for another", "display SVP!");
		return (MB_FAILURE);
	}

	FILE *ifp = (mbvt_levitus_path[0] != '\0') ? fopen(mbvt_levitus_path, "rb") : NULL;
	if (ifp == NULL) {
		fprintf(stderr, "\nUnable to Open Levitus database file <%s> for reading\n", mbvt_levitus_path);
		do_error_dialog("Unable to open the Levitus database", "(share/mbsystem/LevitusAnnual82.dat of MB-System):",
		                mbvt_levitus_path);
		return (MB_FAILURE);
	}

	if (longitude < -360.0 || longitude > 360.0 || latitude < -90.0 || latitude > 90.0) {
		fprintf(stderr, "\nInvalid location specified:  longitude: %f  latitude: %f\n", longitude, latitude);
		fclose(ifp);
		return (MB_FAILURE);
	}

	/* get the longitude and latitude indices */
	int ilon;
	if (longitude < 0.0)
		ilon = (int)(longitude + 360.0);
	else if (longitude >= 360.0)
		ilon = (int)(longitude - 360.0);
	else
		ilon = (int)longitude;
	const double lon_actual = ilon + 0.5;
	int ilat = (int)(latitude + 90.0);
	if (ilat > 179)
		ilat = 179;
	const double lat_actual = ilat - 89.5;

	int status = MB_SUCCESS;

	/* read the temperature */
	const int record_size = sizeof(float) * MBLEVITUS_NLEVITUS_MAX * 180;
	long location = ilon * record_size;
	/* int status = */ fseek(ifp, location, 0);
	if (fread(&temperature[0][0], 1, record_size, ifp) != (size_t)record_size) {
		status = MB_FAILURE;
		fprintf(stderr, "ERROR: EOF reading temperature\n");
	}

	/* read the salinity */
	location = location + 360 * record_size;
	/* status = */ fseek(ifp, location, 0);
	if (fread(&salinity[0][0], 1, record_size, ifp) != (size_t)record_size) {
		status = MB_FAILURE;
		fprintf(stderr, "ERROR: EOF reading salinity\n");
	}

	fclose(ifp);
	if (status != MB_SUCCESS) {
		do_error_dialog("Unable to read the Levitus database", "(it is shorter than it should be):", mbvt_levitus_path);
		return (status);
	}

	/* the database is big-endian (BYTESWAPPED in mblevitus is the little-endian host) */
	{
		const unsigned int one = 1;
		if (*(const unsigned char *)&one == 1)
			for (int i = 0; i < MBLEVITUS_NLEVITUS_MAX; i++) {
				mbvt_swap_float(&temperature[i][ilat]);
				mbvt_swap_float(&salinity[i][ilat]);
			}
	}

	/* calculate velocity from temperature and salinity */
	int nvelocity = 0;
	int nvelocity_tot = 0;
	int last_good = -1;
	float velocity[MBLEVITUS_NDEPTH_MAX];
	for (int i = 0; i < MBLEVITUS_NDEPTH_MAX; i++) {
		if (i < MBLEVITUS_NLEVITUS_MAX && salinity[i][ilat] > MBLEVITUS_NO_DATA) {
			last_good = i;
			nvelocity++;
		}
		if (last_good >= 0) {
			/* set counter */
			nvelocity_tot++;

			/* get pressure for a given depth
			  as a function of latitude */
			const double pressure_dbar = 1.0052405 * ldepth[i] * (1.0 + 0.00528 * sin(DTR * lat_actual) * sin(DTR * lat_actual))
			                              + 0.00000236 * ldepth[i] * ldepth[i];

			/* calculate water sound speed using
			  DelGrosso equations */
			/* convert decibar to kg/cm**2 */
			const double pressure = pressure_dbar * 0.1019716;
			const double c0 = 1402.392;
			const double dltact  = temperature[last_good][ilat]
			      * ( 5.01109398873 + temperature[last_good][ilat]
			        * (-0.0550946843172 + temperature[last_good][ilat] * 0.000221535969240));
			const double dltacs = salinity[last_good][ilat] * (1.32952290781 + salinity[last_good][ilat] * 0.000128955756844);

			const double dltacp = pressure * (0.156059257041E0 + pressure * (0.000024499868841 + pressure * -0.00000000883392332513));
			const double dcstp = temperature[last_good][ilat] *
			            (-0.0127562783426 * salinity[last_good][ilat] +
			             pressure * (0.00635191613389 +
			                         pressure * (0.265484716608E-7 * temperature[last_good][ilat] - 0.00000159349479045 +
			                                     0.522116437235E-9 * pressure) -
			                         0.000000438031096213 * temperature[last_good][ilat] * temperature[last_good][ilat])) +
			        salinity[last_good][ilat] *
			            (-0.161674495909E-8 * salinity[last_good][ilat] * pressure * pressure +
			             temperature[last_good][ilat] *
			                 (0.0000968403156410 * temperature[last_good][ilat] +
			                  pressure * (0.00000485639620015 * salinity[last_good][ilat] - 0.000340597039004)));
			velocity[i] = c0 + dltact + dltacs + dltacp + dcstp;
		}
	}

	/* check for existence of water velocity profile */
	if (nvelocity < 1) {
		fprintf(stderr, "\nNo water velocity profile available for specified location.\n");
		fprintf(stderr, "This place is probably subaerial!\n");
		return (MB_FAILURE);
	}

	/* the profile mbvt_open_display_profile would have read back from mblevitus's file */
	struct profile *profile = &profile_display[ndisplay];
	profile->depth = NULL;
	profile->velocity = NULL;
	profile->nalloc = nvelocity_tot;
	status = mb_mallocd(verbose, __FILE__, __LINE__, profile->nalloc * sizeof(double), (void **)&(profile->depth), &error);
	if (status == MB_SUCCESS)
		status = mb_mallocd(verbose, __FILE__, __LINE__, profile->nalloc * sizeof(double), (void **)&(profile->velocity), &error);
	if (status != MB_SUCCESS) {
		mb_freed(verbose, __FILE__, __LINE__, (void **)&(profile->depth), &error);
		do_error_dialog("Unable to allocate memory", "for the Levitus SVP.", "");
		return (status);
	}
	snprintf(profile->name, sizeof(profile->name), "Levitus annual mean at %.4f/%.4f", lon_actual, lat_actual);
	for (int i = 0; i < nvelocity_tot; i++) {
		/* mblevitus wrote "%f %f": the values the display profile was read back with */
		char line[64];
		snprintf(line, sizeof(line), "%f %f", ldepth[i], velocity[i]);
		sscanf(line, "%lf %lf", &profile->depth[i], &profile->velocity[i]);
	}
	profile->n = nvelocity_tot;
	ndisplay++;

	return (MB_SUCCESS);
}

/*--------------------------------------------------------------------*/
void mbvt_get_state(int *s_edit, int *s_nedit, int *s_ndisplay, int *s_nbuffer, int *s_nbeams) {
	*s_edit = edit ? 1 : 0;
	*s_nedit = edit ? profile_edit.n : 0;
	*s_ndisplay = ndisplay;
	*s_nbuffer = nbuffer;
	int n = 0;
	if (nbuffer > 0 && nresidual != NULL)
		for (int i = 0; i < nbeams; i++)
			if (nresidual[i] > 0)
				n++;
	*s_nbeams = n;
}

int mbvt_get_edit_node(int i, double *dep, double *vel) {
	if (!edit || i < 0 || i >= profile_edit.n)
		return 0;
	*dep = profile_edit.depth[i];
	*vel = profile_edit.velocity[i];
	return 1;
}
/*--------------------------------------------------------------------*/
