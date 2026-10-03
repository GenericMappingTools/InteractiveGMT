/*--------------------------------------------------------------------
 *    The MB-system:  mbgrdviz_callbacks.c    10/9/2002
 *
 *    Copyright (c) 2002-2025 by
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
 * InteractiveGMT port (deps/src/mbgrdviz/mbgrdviz.c): the engine of MB-System 5.8.3's mbgrdviz --
 * every function of src/mbgrdviz/mbgrdviz_callbacks.c that is not a Motif callback.
 *
 * Between the two VERBATIM markers below is that file's text, unchanged but for:
 *   - exit() after a failed projection set-up is gone (this code runs inside the host process,
 *     and the mb_memory_clear() before it would free every MBIO allocation of the process,
 *     libmbview's routes included): the file is closed and the function fails instead;
 *   - XBell() is empty (the reason is already printed, as in the original);
 *   - do_mbgrdviz_opennav no longer spins forever when the datalist cannot be opened (the
 *     original's outer loop only ended through the inner one);
 *   - do_mbgrdviz_saveroutereversed resets its point allocation count when it frees the route
 *     arrays inside the route loop (the original freed them and kept the count, so the next route
 *     reused freed memory);
 *   - do_mbgrdviz_generate_survey takes the view instance instead of the Motif callback arguments
 *     (the original read it from survey_instance anyway).
 * libmbview's data calls go to mbgrdviz_mbview.c. Below the second marker, the port's own: what the
 * Motif callbacks did with widgets (the survey parameters, the area info text, the region read-out
 * for "Open Region as New View", the navigation list for the editors) as plain functions.
 */

/* Dl_info / dladdr are GNU extensions: _GNU_SOURCE must precede the FIRST system header */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#	define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>   /* _getcwd / _chdir: datalists are read from inside their folder (mbgv_abs_path) */
#endif

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#else
#	include <dlfcn.h>
#	include <unistd.h>
#endif

#include "mbgrdviz_mbview.h"
#include "mbgrdviz.h"

/* fileSelectionBox modes */
#define MBGRDVIZ_OPENGRID 0
#define MBGRDVIZ_OPENOVERLAY 1
#define MBGRDVIZ_OPENSITE 2
#define MBGRDVIZ_OPENROUTE 3
#define MBGRDVIZ_OPENVECTOR 4
#define MBGRDVIZ_OPENNAV 5
#define MBGRDVIZ_OPENSWATH 6
#define MBGRDVIZ_SAVEROUTE 7
#define MBGRDVIZ_SAVEROUTEREVERSED 8
#define MBGRDVIZ_SAVERISISCRIPTHEADING 9
#define MBGRDVIZ_SAVERISISCRIPTNOHEADING 10
#define MBGRDVIZ_SAVERISI2SCRIPTHEADING 11
#define MBGRDVIZ_SAVERISI2SCRIPTNOHEADING 12
#define MBGRDVIZ_SAVEDEGDECMIN 13
#define MBGRDVIZ_SAVELNW 14
#define MBGRDVIZ_SAVEGREENSEAYML 15
#define MBGRDVIZ_SAVETECDISLST 16
#define MBGRDVIZ_SAVEKONGSBERGDP 17
#define MBGRDVIZ_SAVESISASCIIPLAN1 18
#define MBGRDVIZ_SAVESISASCIIPLAN2 19
#define MBGRDVIZ_SAVESITE 20
#define MBGRDVIZ_SAVEPROFILE 21
#define MBGRDVIZ_REALTIME 22

/* Projection defines */
#define ModelTypeProjected 1
#define ModelTypeGeographic 2
#define GCS_WGS_84 4326

/* Site and route file versions */
#define MBGRDVIZ_SITE_VERSION_MAJOR 2
#define MBGRDVIZ_SITE_VERSION_MINOR 0
#define MBGRDVIZ_ROUTE_VERSION_MAJOR 2
#define MBGRDVIZ_ROUTE_VERSION_MINOR 0
#define MBGRDVIZ_PROFILE_VERSION "1.00"
#define MBGRDVIZ_RISISCRIPT_VERSION "1.00"
#define MBGRDVIZ_RISI2SCRIPT_VERSION "2.00"

/* Survey planning parameters */
#define MBGRDVIZ_SURVEY_MODE_UNIFORM 0
#define MBGRDVIZ_SURVEY_MODE_VARIABLE 1
#define MBGRDVIZ_SURVEY_PLATFORM_SURFACE 0
#define MBGRDVIZ_SURVEY_PLATFORM_SUBMERGED_ALTITUDE 1
#define MBGRDVIZ_SURVEY_PLATFORM_SUBMERGED_DEPTH 2
#define MBGRDVIZ_SURVEY_DIRECTION_SW 0
#define MBGRDVIZ_SURVEY_DIRECTION_SE 1
#define MBGRDVIZ_SURVEY_DIRECTION_NW 2
#define MBGRDVIZ_SURVEY_DIRECTION_NE 3
static int working_route = -1;
static int survey_instance = 0;
static int survey_mode = MBGRDVIZ_SURVEY_MODE_UNIFORM;
static int survey_platform = MBGRDVIZ_SURVEY_PLATFORM_SUBMERGED_ALTITUDE;
static int survey_interleaving = 1;
static int survey_direction = MBGRDVIZ_SURVEY_DIRECTION_SW;
static bool survey_crosslines_last = false;
static int survey_crosslines = 0;
static int survey_linespacing = 200;
static int survey_swathwidth = 120;
static int survey_depth = 0;
static int survey_altitude = 150;
static int survey_color = MBV_COLOR_BLACK;
static char survey_name[MB_PATH_MAXLINE] = "Survey";

static const char program_name[] = "MBgrdviz";

/* status variables */
static int verbose;
static int error;

/* the widget side, not ported (see the head of this file) */
static bool mbview_id[MBV_MAX_WINDOWS];
#define XBell(display, percent) ((void)0)
static void do_mbgrdviz_sensitivity(void) {
}
void do_mbgrdviz_arearoute_info(size_t instance);

/* libuuid is not linked (USE_UUID undefined): the greensea writer uses its numbered ids, as the
   original does without it */

int do_mbgrdviz_readnav(size_t instance, char *swathfile, int pathstatus, char *pathraw, char *pathprocessed, int format,
                        int formatorg, double weight, bool wantbounds, int *error);

/*--------------------------------------------------------------------*/
/* The MBIO entry points of mbgrdviz_mbio.h, filled by mbgrdviz_mbio_open. */
struct mbgv_mbio_table mbgv_mbio;
static bool mbgv_mbio_ok = false;

/* The module that holds the swath editor's MBIO table: the library mbedit_mbio_open loaded. */
#ifdef _WIN32
static void *mbgv_mbio_module(char *path, int pathlen) {
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
static void *mbgv_dlsym(void *h, const char *name) {
	return (void *)GetProcAddress((HMODULE)h, name);
}
#else
static void *mbgv_mbio_module(char *path, int pathlen) {
	Dl_info info;
	if (dladdr((void *)mbedit_mbio.get_all, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	snprintf(path, pathlen, "%s", info.dli_fname);
	/* the library is already loaded: this only hands back its handle */
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}
static void *mbgv_dlsym(void *h, const char *name) {
	return dlsym(h, name);
}
#endif

int mbgrdviz_mbio_open(char *msg, int msglen) {
	if (msg && msglen > 0)
		msg[0] = '\0';
	if (mbgv_mbio_ok)
		return 1;
	if (!mbedit_mbio_loaded()) {
		snprintf(msg, msglen, "The MB-System MBIO library is not loaded");
		return 0;
	}
	char libpath[MB_PATH_MAXLINE] = "";
	void *h = mbgv_mbio_module(libpath, (int)sizeof(libpath));
	if (h == NULL) {
		snprintf(msg, msglen, "Cannot find the module of the loaded MBIO library");
		return 0;
	}

	struct mbgv_mbio_table t;
	memset(&t, 0, sizeof(t));
	struct {
		void **slot;
		const char *name;
	} syms[] = {
		{(void **)&t.proj_init, "mb_proj_init"},
		{(void **)&t.proj_free, "mb_proj_free"},
		{(void **)&t.proj_forward, "mb_proj_forward"},
		{(void **)&t.proj_inverse, "mb_proj_inverse"},
		{(void **)&t.coor_scale, "mb_coor_scale"},
		{(void **)&t.get_date, "mb_get_date"},
		{(void **)&t.day_name, "mb_day_name"},
		{(void **)&t.month_name, "mb_month_name"},
		{(void **)&t.get_fbt, "mb_get_fbt"},
		{(void **)&t.get_fnv, "mb_get_fnv"},
		{(void **)&t.mallocd, "mb_mallocd"},
		{(void **)&t.reallocd, "mb_reallocd"},
		{(void **)&t.freed, "mb_freed"},
		{(void **)&t.memory_clear, "mb_memory_clear"},
		{(void **)&t.segynumber, "mb_segynumber"},
		{(void **)&t.singlebeam_swathbounds, "mbsys_singlebeam_swathbounds"},
	};
	for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		*syms[i].slot = mbgv_dlsym(h, syms[i].name);
		if (*syms[i].slot == NULL) {
			snprintf(msg, msglen, "%s does not export %s, which mbgrdviz needs", libpath, syms[i].name);
			return 0;
		}
	}
	/* optional: absent from 5.7.x (the stand-ins below take over) */
	*(void **)&t.user_host_date = mbgv_dlsym(h, "mb_user_host_date");
	*(void **)&t.datalist_read3 = mbgv_dlsym(h, "mb_datalist_read3");

	mbgv_mbio = t;
	mbgv_mbio_ok = true;
	return 1;
}

/* mb_user_host_date: the library's when it has one, else MB-System 5.8.3 mbio/mb_defaults.c's text
   (gethostname() needs a Winsock start-up on Windows, which is why the original falls back to
   USERDOMAIN; on Windows that fallback is all that runs). */
int mbgv_user_host_date(int verbose, char user[256], char host[256], char date[32], int *error) {
	if (mbgv_mbio.user_host_date != NULL)
		return mbgv_mbio.user_host_date(verbose, user, host, date, error);
	memset(user, 0, 256);
	char *user_ptr = NULL;
	char *unknown = "Unknown";
	if ((user_ptr = getenv("USER")) == NULL)
		if ((user_ptr = getenv("LOGNAME")) == NULL)
			if ((user_ptr = getenv("USERNAME")) == NULL)
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

	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}

/* mb_proj_forward / mb_proj_inverse. MB-System 5.8.3's (mbio/mb_proj.c, the PROJ 6+ code) declare
   `PJ_COORD c;` and set only c.v[0] and c.v[1] before proj_trans(); c.v[2] and c.v[3] are whatever
   the stack held there, and when that is a NaN -- which the survey generator's locals leave behind --
   PROJ returns NaN for a perfectly good lon/lat, libmbview rejects the point ("far outside view") and
   mbview_addroute goes on to index a route that was never made. Measured in this process: the same
   call returns the right easting/northing over a zeroed stack and NaN over a NaN-filled one. Until
   MBIO initializes the coordinate, the stack the call is about to use is zeroed first. */
#if defined(_MSC_VER)
#	define MBGV_NOINLINE __declspec(noinline)
#else
#	define MBGV_NOINLINE __attribute__((noinline))
#endif
static MBGV_NOINLINE void mbgv_stack_zero(void) {
	volatile char pad[8192];
	for (size_t i = 0; i < sizeof(pad); i++)
		pad[i] = 0;
}
MBGV_NOINLINE int mbgv_proj_forward(int verbose, void *pjptr, double lon, double lat, double *easting, double *northing,
                                    int *error) {
	mbgv_stack_zero();
	return mbgv_mbio.proj_forward(verbose, pjptr, lon, lat, easting, northing, error);
}
MBGV_NOINLINE int mbgv_proj_inverse(int verbose, void *pjptr, double easting, double northing, double *lon, double *lat,
                                    int *error) {
	mbgv_stack_zero();
	return mbgv_mbio.proj_inverse(verbose, pjptr, easting, northing, lon, lat, error);
}

/* mb_datalist_read3: the library's when it has one, else mb_datalist_read2 with no alternative
   navigation (what read3 reports for a datalist entry that names none) */
/* MBIO takes a datalist entry as absolute only when it starts with '/', and glues the datalist's
   folder in front of any other one -- but only when the datalist's own path holds a '/'. So on
   Windows an entry "C:\data\line.mb59" became "<datalist folder>/C:\data\line.mb59", failed MBIO's
   stat() and was dropped ("Attempted to load 0 files"). do_mbgrdviz_opennav therefore opens a
   datalist by its backslash path, from inside its folder (relative entries resolve there), and the
   entries coming back are made absolute here, so the editors launched later still find them. */
static void mbgv_abs_path(char *path) {
#ifdef _WIN32
	if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\' || (path[0] != '\0' && path[1] == ':'))
		return;
	char cwd[MB_PATH_MAXLINE], tmp[MB_PATH_MAXLINE];
	if (_getcwd(cwd, (int)sizeof(cwd)) == NULL)
		return;
	snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
	for (char *c = tmp; *c; c++)
		if (*c == '\\')
			*c = '/';
	strncpy(path, tmp, MB_PATH_MAXLINE - 1);
	path[MB_PATH_MAXLINE - 1] = '\0';
#else
	(void)path;
#endif
}

int mbgv_datalist_read3(int verbose, void *datalist_ptr, int *pstatus, char *path, char *ppath, int *astatus, char *apath,
                        char *dpath, int *format, double *weight, int *error) {
	int status;
	if (mbgv_mbio.datalist_read3 != NULL)
		status = mbgv_mbio.datalist_read3(verbose, datalist_ptr, pstatus, path, ppath, astatus, apath, dpath, format,
		                                  weight, error);
	else {
		*astatus = MB_ALTNAV_NONE;
		apath[0] = '\0';
		status = mb_datalist_read2(verbose, datalist_ptr, pstatus, path, ppath, dpath, format, weight, error);
	}
	if (status == MB_SUCCESS) {
		mbgv_abs_path(path);
		mbgv_abs_path(ppath);
		mbgv_abs_path(apath);
	}
	return status;
}

/*======================================================================================
 * VERBATIM from here: MB-System 5.8.3 src/mbgrdviz/mbgrdviz_callbacks.c (see the head)
 *======================================================================================*/
int do_mbgrdviz_opensite(size_t instance, char *input_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  mb_path buffer;
  mb_path lonstring, latstring;
  int nsite;
  double *sitelon;
  double *sitelat;
  double *sitetopo;
  int *sitecolor;
  int *sitesize;
  mb_path *sitename;
  char *result;
  char *name;
  int nget;
  bool site_ok;
  double londeg, lonmin, latdeg, latmin;
  int site_version_major = 0;
  int site_version_minor = 0;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       input_file_ptr:  %s\n", input_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* count the sites in the input file */
    nsite = 0;
    if ((sfp = fopen(input_file_ptr, "r")) == NULL) {
      error = MB_ERROR_OPEN_FAIL;
      fprintf(stderr, "\nUnable to Open Site File <%s> for reading\n", input_file_ptr);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
      return (status);
    }
    while ((result = fgets(buffer, MB_PATH_MAXLINE, sfp)) == buffer) {
      if (buffer[0] != '#')
        nsite++;
    }
    fclose(sfp);

    /* allocate arrays for sites */
    if (nsite > 0) {
      /* allocate the arrays */
      sitelon = NULL;
      sitelat = NULL;
      sitetopo = NULL;
      sitecolor = NULL;
      sitesize = NULL;
      sitename = NULL;
      status =
          mbview_allocsitearrays(verbose, nsite, &sitelon, &sitelat, &sitetopo, &sitecolor, &sitesize, &sitename, &error);

      /* if error initializing memory then cancel dealing with sites */
      if (status == MB_FAILURE) {
        nsite = 0;
        fprintf(stderr, "\nUnable to allocate arrays for %d sites\n", nsite);
        XBell((Display *)XtDisplay(mainWindow), 100);
        return (status);
      }
    }

    /* read the sites from the input file */
    if (nsite > 0) {
      nsite = 0;
      if ((sfp = fopen(input_file_ptr, "r")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to open site file <%s> for reading\n", input_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
        return (status);
      }
      while ((result = fgets(buffer, MB_PATH_MAXLINE, sfp)) == buffer) {
        site_ok = false;

        /* deal with site in form: lon lat topo color size name */
        if (strncmp(buffer, "## Site File Version", strlen("## Site File Version")) == 0) {
          nget = sscanf(buffer, "## Site File Version %d.%d", &site_version_major, &site_version_minor);
        }
        else if (buffer[0] != '#') {
          if (site_version_major > 1 || (site_version_major == 0 && strchr(buffer, ',') != NULL)) {
            nget = sscanf(buffer, "%[^','],%[^','],%lf,%d,%d,%[^\n]", 
                        lonstring, latstring, &sitetopo[nsite],
                        &sitecolor[nsite], &sitesize[nsite], sitename[nsite]);
            if (nget >= 2)
            	site_version_major = 2;
          }
          else {
            nget = sscanf(buffer, "%s %s %lf %d %d %[^\n]", 
                        lonstring, latstring, &sitetopo[nsite],
                        &sitecolor[nsite], &sitesize[nsite], sitename[nsite]);
          }
          if (nget >= 2) {
            if (strchr(lonstring, ':') != NULL) {
              if (sscanf(lonstring, "%lf:%lf", &londeg,&lonmin) == 2) {
                sitelon[nsite] = copysign((fabs(londeg) + fabs(lonmin) / 60.0), londeg);
                site_ok = true;
              }
            } else if (sscanf(lonstring, "%lf", &sitelon[nsite]) == 1) {
                site_ok = true;
            }
            if (site_ok) {
              if (strchr(latstring, ':') != NULL) {
                if (sscanf(latstring, "%lf:%lf", &latdeg,&latmin) == 2) {
                  sitelat[nsite] = copysign((fabs(latdeg) + fabs(latmin) / 60.0), latdeg);
                }
              } else if (sscanf(latstring, "%lf", &sitelat[nsite]) == 1) {
              } else {
                site_ok = false;
              }
            }
          }
        }
        if (site_ok) {
          if (nget < 6) {
            name = (char *)sitename[nsite];
            name[0] = '\0';
          }
          if (nget < 5)
            sitesize[nsite] = 0;
          if (nget < 4)
            sitecolor[nsite] = 0;
          if (nget < 3)
            sitetopo[nsite] = MBV_DEFAULT_NODATA;
        }

        /* output some debug values */
        if (verbose > 0 && site_ok) {
          fprintf(stderr, "\nSite point read in program <%s>\n", program_name);
          fprintf(stderr, "     site[%d]: %f %f %f  %d %d  %s\n", nsite, sitelon[nsite], sitelat[nsite],
                  sitetopo[nsite], sitecolor[nsite], sitesize[nsite], sitename[nsite]);
        }
        else if (verbose > 0 && !site_ok && buffer[0] != '#') {
          fprintf(stderr, "\nUnintelligible line read from site file in program <%s>\n", program_name);
          fprintf(stderr, "     buffer:  %s\n", buffer);
        }

        strncpy(buffer, "", sizeof(buffer));
        if (site_ok)
          nsite++;
      }
      fclose(sfp);
    }

    /* add the sites */
    if (nsite > 0) {
      status = mbview_addsites(verbose, instance, nsite, sitelon, sitelat, sitetopo, sitecolor, sitesize, sitename, &error);

      /* update widgets */
      if (status == MB_SUCCESS)
        status = mbview_update(verbose, instance, &error);
    }

    /* deallocate memory */
    if (nsite > 0) {
      status = mbview_freesitearrays(verbose, &sitelon, &sitelat, &sitetopo, &sitecolor, &sitesize, &sitename, &error);
    }
  }

  /* set sensitivity of widgets that require an mbview instance to be active */
  do_mbgrdviz_sensitivity();

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savesite(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nsite;
  double *sitelon;
  double *sitelat;
  double *sitetopo;
  int *sitecolor;
  int *sitesize;
  mb_path *sitename;
  int i;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of sites to be written to the output file */
    status = mbview_getsitecount(verbose, instance, &nsite, &error);
    if (status == MB_SUCCESS && nsite <= 0) {
      fprintf(stderr, "Unable to write site file...\nCurrently %d sites defined for instance %zu!\n", nsite, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* allocate arrays for sites */
    if (status == MB_SUCCESS && nsite > 0) {
      /* allocate the arrays */
      sitelon = NULL;
      sitelat = NULL;
      sitetopo = NULL;
      sitecolor = NULL;
      sitesize = NULL;
      sitename = NULL;
      status =
          mbview_allocsitearrays(verbose, nsite, &sitelon, &sitelat, &sitetopo, &sitecolor, &sitesize, &sitename, &error);

      /* if error initializing memory then cancel dealing with sites */
      if (status == MB_FAILURE) {
        nsite = 0;
        fprintf(stderr, "Unable to write site file...\nArray allocation for %d sites failed for instance %zu!\n", nsite,
                instance);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* get the sites */
    if (status == MB_SUCCESS) {
      status =
          mbview_getsites(verbose, instance, &nsite, sitelon, sitelat, sitetopo, sitecolor, sitesize, sitename, &error);
    }

    /* write the sites to the output file */
    if (status == MB_SUCCESS) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the site file header */
        fprintf(sfp, "## Site File Version %d.%2.2d\n", MBGRDVIZ_SITE_VERSION_MAJOR, MBGRDVIZ_SITE_VERSION_MINOR);
        fprintf(sfp, "## Output by Program %s\n", program_name);
        fprintf(sfp, "## MB-System Version %s\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
        fprintf(sfp, "## Number of sites: %d\n", nsite);
        fprintf(sfp, "## Site colors:\n");
        fprintf(sfp, "##   COLOR_BLACK     0\n");
        fprintf(sfp, "##   COLOR_WHITE     1\n");
        fprintf(sfp, "##   COLOR_RED       2\n");
        fprintf(sfp, "##   COLOR_YELLOW    3\n");
        fprintf(sfp, "##   COLOR_GREEN     4\n");
        fprintf(sfp, "##   COLOR_BLUEGREEN 5\n");
        fprintf(sfp, "##   COLOR_BLUE      6\n");
        fprintf(sfp, "##   COLOR_PURPLE    7\n");
        fprintf(sfp, "## Site point format:\n");
        fprintf(sfp, "##   <longitude (deg)>,<latitude (deg)>,<topography (m)>,<color>,<size>,<name>\n");

        /* loop over the sites */
        for (i = 0; i < nsite; i++) {
          fprintf(sfp, "%12.7f,%12.7f,%10.3f,%2d,%2d,%s\n", sitelon[i], sitelat[i], sitetopo[i], sitecolor[i],
                  sitesize[i], sitename[i]);
        }

        /* close the output file */
        fclose(sfp);
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        fprintf(stderr, "\nUnable to Open Site File <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
        status = MB_FAILURE;
      }
    }

    /* deallocate arrays for sites */
    if (nsite > 0) {
      status = mbview_freesitearrays(verbose, &sitelon, &sitelat, &sitetopo, &sitecolor, &sitesize, &sitename, &error);
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_openroute(size_t instance, char *input_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  char buffer[MB_PATH_MAXLINE];
  int npoint = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  double lon, lat, topo;
  int *routewaypoint = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  int iroute;
  int waypoint;
  bool rawroutefile = true;
  char *result;
  int nget;
  bool point_ok;
  int route_version_major = 0;
  int route_version_minor = 0;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       input_file_ptr:  %s\n", input_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {
    /* initialize route values */
    routecolor = MBV_COLOR_BLUE;
    routesize = 1;
    routeeditmode = true;
    routename[0] = '\0';
    rawroutefile = true;
    npoint = 0;
    npointalloc = 0;

    /* open the input file */
    if ((sfp = fopen(input_file_ptr, "r")) == NULL) {
      error = MB_ERROR_OPEN_FAIL;
      status = MB_FAILURE;
      fprintf(stderr, "\nUnable to open route file <%s> for reading\n", input_file_ptr);
      XBell((Display *)XtDisplay(mainWindow), 100);
    }

    /* loop over reading */
    if (status == MB_SUCCESS) {
      while ((result = fgets(buffer, MB_PATH_MAXLINE, sfp)) == buffer) {
        /* deal with comments */
        if (buffer[0] == '#') {
          if (rawroutefile && strncmp(buffer, "## Route File Version", 21) == 0) {
            rawroutefile = false;
            sscanf(buffer, "## Route File Version %d.%d", 
                    &route_version_major, &route_version_minor);
          }
          else if (strncmp(buffer, "## ROUTENAME", 12) == 0) {
            strcpy(routename, &buffer[13]);
            size_t routename_len = strlen(routename);
            if (routename_len > 0 && routename[routename_len - 1] == '\n') {
              routename[routename_len - 1] = '\0';
              routename_len--;
            }
            if (routename_len > 0 && routename[routename_len - 1] == '\r')
              routename[routename_len - 1] = '\0';
          }
          else if (strncmp(buffer, "## ROUTECOLOR", 13) == 0) {
            sscanf(buffer, "## ROUTECOLOR %d", &routecolor);
          }
          else if (strncmp(buffer, "## ROUTESIZE", 12) == 0) {
            sscanf(buffer, "## ROUTESIZE %d", &routesize);
          }
          else if (strncmp(buffer, "## ROUTEEDITMODE", 16) == 0) {
            sscanf(buffer, "## ROUTEEDITMODE %d", &routeeditmode);
          }
        }

        /* deal with route segment marker */
        else if (buffer[0] == '>') {
          if (strncmp(buffer, "> ## STARTROUTE", 14) == 0 && route_version_major > 1) {
            int np = 0;
            nget = sscanf(buffer, "> ## STARTROUTE %d,%d,%d,%d,%[^\n]", 
                          &np, &routecolor, &routesize, &routeeditmode, routename);
          }

          /* if data accumulated call mbview_addroute() */
          if (npoint > 0) {
            status = mbview_addroute(verbose, instance, npoint, routelon, routelat, routewaypoint, routecolor,
                                     routesize, routeeditmode, routename, &iroute, &error);
            npoint = 0;
          }
        }

        /* deal with data */
        else {
          /* read the data from the buffer */
          if (route_version_major > 1) {
            nget = sscanf(buffer, "%lf,%lf,%lf,%d", &lon, &lat, &topo, &waypoint);
          } else if (route_version_major == 1) {
            nget = sscanf(buffer, "%lf %lf %lf %d", &lon, &lat, &topo, &waypoint);
          } else {
            nget = sscanf(buffer, "%lf,%lf,%lf,%d", &lon, &lat, &topo, &waypoint);
            if (nget < 2) {
            	nget = sscanf(buffer, "%lf %lf %lf %d", &lon, &lat, &topo, &waypoint);
            }
          }
          if ((rawroutefile && nget >= 2) ||
              (!rawroutefile && nget >= 3 && waypoint > MBV_ROUTE_WAYPOINT_NONE))
            point_ok = true;
          else
            point_ok = false;

          /* if good data check for need to allocate more space */
          if (point_ok && npoint + 1 > npointalloc) {
            npointalloc += MBV_ALLOC_NUM;
            status = mbview_allocroutearrays(verbose, npointalloc, &routelon, &routelat, &routewaypoint, NULL, NULL,
                                             NULL, NULL, NULL, &error);
            if (status != MB_SUCCESS) {
              npointalloc = 0;
            }
          }

          /* add good point to route */
          if (point_ok && npointalloc > npoint) {
            routelon[npoint] = lon;
            routelat[npoint] = lat;
            routewaypoint[npoint] = waypoint;
            npoint++;
          }
        }
      }

      /* add last route if not already handled */
      if (npoint > 0) {
        status = mbview_addroute(verbose, instance, npoint, routelon, routelat, routewaypoint, routecolor, routesize,
                                 routeeditmode, routename, &iroute, &error);
        npoint = 0;
      }

      /* free the memory */
      if (npointalloc > 0)
        status =
            mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, NULL, NULL, NULL, NULL, NULL, &error);

      /* close the input file */
      fclose(sfp);
    }

    /* update widgets */
    mbview_updateroutelist();
    status = mbview_update(verbose, instance, &error);
  }

  /* set sensitivity of widgets that require an mbview instance to be active */
  do_mbgrdviz_sensitivity();

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saveroute(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int nroutewrite = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  int iroute, j;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected)
        nroutewrite++;
    }
    if (nroutewrite == 0)
      nroutewrite = nroute;
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the route file header */
        fprintf(sfp, "## Route File Version %d.%2.2d\n", 
                      MBGRDVIZ_ROUTE_VERSION_MAJOR, MBGRDVIZ_ROUTE_VERSION_MINOR);
        fprintf(sfp, "## Output by Program %s\n", program_name);
        fprintf(sfp, "## MB-System Version %s\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
        fprintf(sfp, "##\n");
        fprintf(sfp, "## Each route starts with a line of the form:\n");
        fprintf(sfp, "##   > ## STARTROUTE np,c,s,m,String\n");
        fprintf(sfp, "## where:\n");
        fprintf(sfp, "##   np:                   Number of points in route (waypoints plus topography points)\n");
        fprintf(sfp, "##   c:                    Color\n");
        fprintf(sfp, "##   s:                    Size\n");
        fprintf(sfp, "##   m:                    Edit mode\n");
        fprintf(sfp, "##   String:               Survey name (can have spaces)\n");
        fprintf(sfp, "## Each route ends with a line of the form:\n");
        fprintf(sfp, "##   > ## ENDROUTE\n");
        fprintf(sfp, "## Route color definitions:\n");
        fprintf(sfp, "##   BLACK                 0\n");
        fprintf(sfp, "##   WHITE                 1\n");
        fprintf(sfp, "##   RED                   2\n");
        fprintf(sfp, "##   YELLOW                3\n");
        fprintf(sfp, "##   GREEN                 4\n");
        fprintf(sfp, "##   BLUEGREEN             5\n");
        fprintf(sfp, "##   BLUE                  6\n");
        fprintf(sfp, "##   PURPLE                7\n");
        fprintf(sfp, "##\n");
        fprintf(sfp, "## Route point format:\n");
        fprintf(sfp, "##   <longitude (deg)>,<latitude (deg)>,<topography (m)>,<waypoint type>,<bearing (deg)>,<lateral distance (m)>,<distance along topography (m)>,<slope (m/m)>\n");
        fprintf(sfp, "## Route waypoint type definitions:\n");
        fprintf(sfp, "##   WAYPOINT_NONE         0  Defines topography between waypoints\n");
        fprintf(sfp, "##   WAYPOINT_SIMPLE       1  Waypoint along survey line\n");
        fprintf(sfp, "##   WAYPOINT_TRANSIT      2  Waypoint along survey line\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE    3  Start survey line type 1\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE      4  End survey line type 1\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE2   5  Start survey line type 2\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE2     6  End survey line type 2\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE3   7  Start survey line type 3\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE3     8  End survey line type 3\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE4   9  Start survey line type 4\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE4    10  End survey line type 4\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE5  11  Start survey line type 5\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE5    12  End survey line type 5\n");
        fprintf(sfp, "##\n");
        fprintf(sfp, "## Number of routes: %d\n", nroutewrite);
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* check if this route is selected for writing */
        if (nroutewrite == nroute)
          selected = true;
        else
          mbview_getrouteselected(verbose, instance, iroute, &selected, &error);

        /* output if selected */
        if (selected) {
          /* get point count for current route */
          status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

          /* allocate route arrays */
          npointtotal = npoint + nintpoint;
          if (status == MB_SUCCESS && npointalloc < npointtotal) {
            status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                             &routebearing, &distlateral, &distovertopo, &slope, &error);
            if (status == MB_SUCCESS) {
              npointalloc = npointtotal;
            }

            /* if error initializing memory then cancel dealing with this route */
            else {
              fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                      npointtotal, instance);
              XBell((Display *)XtDisplay(mainWindow), 100);
              npoint = 0;
              nintpoint = 0;
              npointtotal = 0;
            }
          }

          /* extract data for route */
          status = mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint,
                                   routetopo, routebearing, distlateral, distovertopo, slope, &routecolor, &routesize,
                                   &routeeditmode, routename, &error);

          /* write the route header */
          fprintf(sfp, "> ## STARTROUTE %d,%d,%d,%d,%s\n", 
                      npointtotal, routecolor, routesize, routeeditmode, routename);

          /* write the route points */
          for (j = 0; j < npointtotal; j++) {
            fprintf(sfp, "%f,%f,%f,%d,%f,%f,%f,%f", routelon[j], routelat[j], routetopo[j], routewaypoint[j],
                    routebearing[j], distlateral[j], distovertopo[j], slope[j]);
            if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_SIMPLE)
              fprintf(sfp, " ## WAYPOINT\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_TRANSIT)
              fprintf(sfp, " ## WAYPOINT TRANSIT\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE)
              fprintf(sfp, " ## WAYPOINT STARTLINE\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE)
              fprintf(sfp, " ## WAYPOINT ENDLINE\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE2)
              fprintf(sfp, " ## WAYPOINT STARTLINE2\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE2)
              fprintf(sfp, " ## WAYPOINT ENDLINE2\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE3)
              fprintf(sfp, " ## WAYPOINT STARTLINE3\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE3)
              fprintf(sfp, " ## WAYPOINT ENDLINE3\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE4)
              fprintf(sfp, " ## WAYPOINT STARTLINE4\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE4)
              fprintf(sfp, " ## WAYPOINT ENDLINE4\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE5)
              fprintf(sfp, " ## WAYPOINT STARTLINE5\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE5)
              fprintf(sfp, " ## WAYPOINT ENDLINE5\n");
            else
              fprintf(sfp, "\n");
          }

          /* write the route end */
          fprintf(sfp, "> ## ENDROUTE\n");
        }
      }

      /* close the output file */
      fclose(sfp);

	  /* deallocate arrays */
	  if (npointalloc > 0) {
		status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
										&distlateral, &distovertopo, &slope, &error);
	  }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saveroutereversed(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int nroutewrite = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  int iroute, j;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected)
        nroutewrite++;
    }
    if (nroutewrite == 0)
      nroutewrite = nroute;
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the route file header */
        fprintf(sfp, "## Route File Version %d.%2.2d\n", 
                      MBGRDVIZ_ROUTE_VERSION_MAJOR, MBGRDVIZ_ROUTE_VERSION_MINOR);
        fprintf(sfp, "## Output by Program %s\n", program_name);
        fprintf(sfp, "## MB-System Version %s\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
        fprintf(sfp, "##\n");
        fprintf(sfp, "## Each route starts with a line of the form:\n");
        fprintf(sfp, "##   > ## STARTROUTE np,c,s,m,String\n");
        fprintf(sfp, "## where:\n");
        fprintf(sfp, "##   np:                   Number of points in route (waypoints plus topography points)\n");
        fprintf(sfp, "##   c:                    Color\n");
        fprintf(sfp, "##   s:                    Size\n");
        fprintf(sfp, "##   m:                    Edit mode\n");
        fprintf(sfp, "##   String:               Survey name (can have spaces)\n");
        fprintf(sfp, "## Each route ends with a line of the form:\n");
        fprintf(sfp, "##   > ## ENDROUTE\n");
        fprintf(sfp, "## Route color definitions:\n");
        fprintf(sfp, "##   BLACK                 0\n");
        fprintf(sfp, "##   WHITE                 1\n");
        fprintf(sfp, "##   RED                   2\n");
        fprintf(sfp, "##   YELLOW                3\n");
        fprintf(sfp, "##   GREEN                 4\n");
        fprintf(sfp, "##   BLUEGREEN             5\n");
        fprintf(sfp, "##   BLUE                  6\n");
        fprintf(sfp, "##   PURPLE                7\n");
        fprintf(sfp, "##\n");
        fprintf(sfp, "## Route point format:\n");
        fprintf(sfp, "##   <longitude (deg)>,<latitude (deg)>,<topography (m)>,<waypoint type>,<bearing (deg)>,<lateral distance (m)>,<distance along topography (m)>,<slope (m/m)>\n");
        fprintf(sfp, "## Route waypoint type definitions:\n");
        fprintf(sfp, "##   WAYPOINT_NONE         0  Defines topography between waypoints\n");
        fprintf(sfp, "##   WAYPOINT_SIMPLE       1  Waypoint along survey line\n");
        fprintf(sfp, "##   WAYPOINT_TRANSIT      2  Waypoint along survey line\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE    3  Start survey line type 1\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE      4  End survey line type 1\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE2   5  Start survey line type 2\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE2     6  End survey line type 2\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE3   7  Start survey line type 3\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE3     8  End survey line type 3\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE4   9  Start survey line type 4\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE4    10  End survey line type 4\n");
        fprintf(sfp, "##   WAYPOINT_STARTLINE5  11  Start survey line type 5\n");
        fprintf(sfp, "##   WAYPOINT_ENDLINE5    12  End survey line type 5\n");
        fprintf(sfp, "##\n");
        fprintf(sfp, "## Number of routes: %d\n", nroutewrite);
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* check if this route is selected for writing */
        if (nroutewrite == nroute)
          selected = true;
        else
          mbview_getrouteselected(verbose, instance, iroute, &selected, &error);

        /* output if selected */
        if (selected) {
          /* get point count for current route */
          status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

          /* allocate route arrays */
          npointtotal = npoint + nintpoint;
          if (status == MB_SUCCESS && npointalloc < npointtotal) {
            status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                             &routebearing, &distlateral, &distovertopo, &slope, &error);
            if (status == MB_SUCCESS) {
              npointalloc = npointtotal;
            }

            /* if error initializing memory then cancel dealing with this route */
            else {
              fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                      npointtotal, instance);
              XBell((Display *)XtDisplay(mainWindow), 100);
              npoint = 0;
              nintpoint = 0;
              npointtotal = 0;
            }
          }

          /* extract data for route */
          status = mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint,
                                   routetopo, routebearing, distlateral, distovertopo, slope, &routecolor, &routesize,
                                   &routeeditmode, routename, &error);

           /* write the route header */
          fprintf(sfp, "> ## STARTROUTE %d,%d,%d,%d,%s\n", 
                      npointtotal, routecolor, routesize, routeeditmode, routename);

          /* write the route points */
          for (j = npointtotal - 1; j >= 0; j-- ) {
            double bearing = *routebearing - 180.0;
            if (bearing < 0.0)
              bearing += 360.0;
            fprintf(sfp, "%f,%f,%f,%d,%f,%f,%f,%f", routelon[j], routelat[j], routetopo[j], routewaypoint[j],
                    routebearing[j], distlateral[j], distovertopo[j], slope[j]);
            if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_SIMPLE)
              fprintf(sfp, " ## WAYPOINT\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_TRANSIT)
              fprintf(sfp, " ## WAYPOINT TRANSIT\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE)
              fprintf(sfp, " ## WAYPOINT STARTLINE\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE)
              fprintf(sfp, " ## WAYPOINT ENDLINE\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE2)
              fprintf(sfp, " ## WAYPOINT STARTLINE2\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE2)
              fprintf(sfp, " ## WAYPOINT ENDLINE2\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE3)
              fprintf(sfp, " ## WAYPOINT STARTLINE3\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE3)
              fprintf(sfp, " ## WAYPOINT ENDLINE3\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE4)
              fprintf(sfp, " ## WAYPOINT STARTLINE4\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE4)
              fprintf(sfp, " ## WAYPOINT ENDLINE4\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_STARTLINE5)
              fprintf(sfp, " ## WAYPOINT STARTLINE5\n");
            else if (routewaypoint[j] == MBV_ROUTE_WAYPOINT_ENDLINE5)
              fprintf(sfp, " ## WAYPOINT ENDLINE5\n");
            else
              fprintf(sfp, "\n");
          }

          /* write the route end */
          fprintf(sfp, "> ## ENDROUTE\n");
        }

        /* deallocate arrays */
        if (npointalloc > 0) {
          status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                          &distlateral, &distovertopo, &slope, &error);
          npointalloc = 0;   /* InteractiveGMT port: the arrays are gone, so is their size */
        }
      }

      /* close the output file */
      fclose(sfp);
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saverisiscriptheading(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int nroutewrite = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  int iroute, j;
  void *pjptr = NULL;
  double origin_x, origin_y;
  double vvspeed = 0.2;
  double settlingtime = 3.0;
  double altitude = 3.0;
  int turndirection = 1;

  if (verbose >= 0) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected)
        nroutewrite++;
    }
    if (nroutewrite == 0)
      nroutewrite = nroute;
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the route file header */
        fprintf(sfp, "## Risi Script Version %s\r\n", MBGRDVIZ_RISISCRIPT_VERSION);
        fprintf(sfp, "## Output by Program %s\r\n", program_name);
        fprintf(sfp, "## MB-System Version %s\r\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\r\n", user, host, date);
        fprintf(sfp, "## Number of routes: %d\r\n", nroutewrite);
        fprintf(sfp, "## Risi script format:\r\n");
        fprintf(sfp, "##   ALT, <altitude (m)>, <speed (m/s)>, <settling time (sec)>\r\n");
        fprintf(sfp, "##   HDG, <heading (deg)>, <turn direction +/-1>, <rate (deg/sec)>, <settling time (sec)>\r\n");
        fprintf(sfp, "##   POS, <north (m)>, <east (m)>, <down (m) (ignored)>, <speed (m/sec)>, <settling time (sec)>\r\n");
        fprintf(sfp, "##\r\n");
        fprintf(sfp, "## This script assumes the survey platform starts at the origin with heading 0.0\r\n");
        fprintf(sfp, "##\r\n");
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\r\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* check if this route is selected for writing */
        if (nroutewrite == nroute)
          selected = true;
        else
          mbview_getrouteselected(verbose, instance, iroute, &selected, &error);

        /* output if selected */
        if (selected) {
          /* get point count for current route */
          status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

          /* allocate route arrays */
          npointtotal = npoint + nintpoint;
          if (status == MB_SUCCESS && npointalloc < npointtotal) {
            status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                             &routebearing, &distlateral, &distovertopo, &slope, &error);
            if (status == MB_SUCCESS) {
              npointalloc = npointtotal;
            }

            /* if error initializing memory then cancel dealing with this route */
            else {
              fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                      npointtotal, instance);
              XBell((Display *)XtDisplay(mainWindow), 100);
              npoint = 0;
              nintpoint = 0;
              npointtotal = 0;
            }
          }

          /* extract data for route */
          status = mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint,
                                   routetopo, routebearing, distlateral, distovertopo, slope, &routecolor, &routesize,
                                   &routeeditmode, routename, &error);

          /* if this the first route define the projection */
          if (pjptr == NULL && npointtotal > 0) {
            double reference_lon = routelon[0];
            double reference_lat = routelat[0];
            mb_path projection_id;

            /* calculate eastings and northings using an LTM projection */
            //if (reference_lat > -80.0 && reference_lat < 84.0) {
            //  if (reference_lon > 180.0)
            //    reference_lon -= 360.0;
            //  sprintf(projection_id, "LTM%.5f/%.5f", reference_lon, reference_lat);
            //}

            /* calculate eastings and northings using an UTM projection */
            if (reference_lat > -80.0 && reference_lat < 84.0) {
							if (reference_lon < 180.0)
								reference_lon += 360.0;
							if (reference_lon >= 180.0)
								reference_lon -= 360.0;
							const int utm_zone = (int)(((reference_lon + 183.0) / 6.0) + 0.5);
							if (reference_lat >= 0.0)
								snprintf(projection_id, sizeof(projection_id), "UTM%2.2dN", utm_zone);
							else
								snprintf(projection_id, sizeof(projection_id), "UTM%2.2dS", utm_zone);
            }

            /* else if more northerly than 84 deg N then use
                    North Universal Polar Stereographic Projection */
            else if (reference_lat > 84.0) {
              int projectionid = 32661;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }

            /* else if more southerly than 80 deg S then use
                    South Universal Polar Stereographic Projection */
            else if (reference_lat < 80.0) {
              int projectionid = 32761;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }
            fprintf(stderr, "Reference longitude: %.9f latitude:%.9f Projection ID: %s\n",
                    reference_lon, reference_lat, projection_id);

            /* initialize projection */
            if (mb_proj_init(verbose, projection_id, &(pjptr), &error) != MB_SUCCESS) {
              char *error_message = NULL;
              mb_error(verbose, error, &error_message);
              fprintf(stderr, "\nMBIO Error initializing projection:\n%s\n", error_message);
              fprintf(stderr, "\nProgram terminated in <%s>\n", __func__);
              fclose(sfp);   /* InteractiveGMT port: was mb_memory_clear() + exit() */
              return (MB_FAILURE);
            }
            mb_proj_forward(verbose, pjptr, reference_lon, reference_lat, &origin_x, &origin_y, &error);
          }

          /* output route as Risi script */
          if (pjptr != NULL && npointtotal > 0) {
            /* write the route header */
            fprintf(sfp, "## ROUTENAME %s\r\n", routename);
            fprintf(sfp, "## ROUTEPOINTS %d\r\n", npointtotal);
            fprintf(sfp, "## STARTROUTE\r\n");

            /* write the route points */
            vvspeed = 0.20;
            settlingtime = 3.0;
            altitude = 3.0;
            turndirection = 1;
            double turns = 0.0;
            double heading = routebearing[0];
            double headinglast = 0.0;
            double dheading = heading - headinglast;
            if (dheading > 180.0)
              dheading -= 360.0;
            else if (dheading < -180.0)
              dheading += 360.0;
            if (dheading >= -180.0 && dheading < 0.0) {
              turndirection = -1;
            } else if (dheading >= 0.0 && dheading < 180.0) {
              turndirection = 1;
            }

            fprintf(sfp, "ALT, %.3f, 0.1, 3\r\n", altitude);
            fprintf(sfp, "##\n");
            fprintf(sfp, "HDG, %.3f, %d, 6, %.3f\r\n", heading, turndirection, settlingtime);
            headinglast = heading;
            for (j = 0; j < npointtotal; j++) {
              if (j >= 0 && routewaypoint[j] > MBV_ROUTE_WAYPOINT_NONE) {
                double xxxx, yyyy, zz;
                mb_proj_forward(verbose, pjptr, routelon[j], routelat[j], &xxxx, &yyyy, &error);
                xxxx = xxxx - origin_x;
                yyyy = yyyy - origin_y;
                zz = -altitude;
                heading = routebearing[j];
                dheading = heading - headinglast;
                if (dheading > 180.0)
                  dheading -= 360.0;
                else if (dheading < -180.0)
                  dheading += 360.0;
                if (dheading >= -180.0 && dheading < 0.0) {
                  turndirection = -1;
                } else if (dheading >= 0.0 && dheading < 180.0) {
                  turndirection = 1;
                }
                fprintf(sfp, "POS, %.3f, %.3f, %.3f, %.3f, %.3f\r\n", yyyy, xxxx, zz, vvspeed, settlingtime);
                fprintf(sfp, "##\n");
                fprintf(sfp, "HDG, %.3f, %d, 6, %.3f\r\n", heading, turndirection, settlingtime);
                turns += dheading / 360.0;
                headinglast = heading;
                fprintf(stderr, "j:%d turns: %f\n", j, turns);
              }
            }
            if (turns < 0.0)
              turndirection = -1;
            else
              turndirection = 1;
            fprintf(sfp, "HDG, %.3f, %d, 6, %.3f\r\n", 0.0, turndirection, settlingtime);

            /* write the route end */
            fprintf(sfp, "## End\r\n");
          }

          /* deallocate arrays */
          if (npointalloc > 0) {
            status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                            &distlateral, &distovertopo, &slope, &error);
          }
        }
      }

      /* close the output file */
      fclose(sfp);
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saverisiscriptnoheading(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int nroutewrite = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  int iroute, j;
  void *pjptr = NULL;
  double origin_x, origin_y;
  double vvspeed = 0.2;
  double settlingtime = 3.0;
  double altitude = 3.0;
  int turndirection = 1;

  if (verbose >= 0) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected)
        nroutewrite++;
    }
    if (nroutewrite == 0)
      nroutewrite = nroute;
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the route file header */
        fprintf(sfp, "## Risi Script Version %s\r\n", MBGRDVIZ_RISISCRIPT_VERSION);
        fprintf(sfp, "## Output by Program %s\r\n", program_name);
        fprintf(sfp, "## MB-System Version %s\r\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\r\n", user, host, date);
        fprintf(sfp, "## Number of routes: %d\r\n", nroutewrite);
        fprintf(sfp, "## Risi script format:\r\n");
        fprintf(sfp, "##   ALT, <altitude (m)>, <speed (m/s)>, <settling time (sec)>\r\n");
        fprintf(sfp, "##   HDG, <heading (deg)>, <turn direction +/-1>, <rate (deg/sec)>, <settling time (sec)>\r\n");
        fprintf(sfp, "##   POS, <north (m)>, <east (m)>, <down (m) (ignored)>, <speed (m/sec)>, <settling time (sec)>\r\n");
        fprintf(sfp, "##\r\n");
        fprintf(sfp, "## This script assumes the survey platform starts at the origin with heading 0.0\r\n");
        fprintf(sfp, "##\r\n");
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\r\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* check if this route is selected for writing */
        if (nroutewrite == nroute)
          selected = true;
        else
          mbview_getrouteselected(verbose, instance, iroute, &selected, &error);

        /* output if selected */
        if (selected) {
          /* get point count for current route */
          status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

          /* allocate route arrays */
          npointtotal = npoint + nintpoint;
          if (status == MB_SUCCESS && npointalloc < npointtotal) {
            status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                             &routebearing, &distlateral, &distovertopo, &slope, &error);
            if (status == MB_SUCCESS) {
              npointalloc = npointtotal;
            }

            /* if error initializing memory then cancel dealing with this route */
            else {
              fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                      npointtotal, instance);
              XBell((Display *)XtDisplay(mainWindow), 100);
              npoint = 0;
              nintpoint = 0;
              npointtotal = 0;
            }
          }

          /* extract data for route */
          status = mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint,
                                   routetopo, routebearing, distlateral, distovertopo, slope, &routecolor, &routesize,
                                   &routeeditmode, routename, &error);

          /* if this the first route define the projection using the starting point */
          if (pjptr == NULL && npointtotal > 0) {
            double reference_lon = routelon[0];
            double reference_lat = routelat[0];
            mb_path projection_id;

            /* calculate eastings and northings using an LTM projection */
            if (reference_lat > -80.0 && reference_lat < 84.0) {
              if (reference_lon > 180.0)
                reference_lon -= 360.0;
              sprintf(projection_id, "LTM%.5f/%.5f", reference_lon, reference_lat);
            }

            /* else if more northerly than 84 deg N then use
                    North Universal Polar Stereographic Projection */
            else if (reference_lat > 84.0) {
              int projectionid = 32661;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }

            /* else if more southerly than 80 deg S then use
                    South Universal Polar Stereographic Projection */
            else if (reference_lat < 80.0) {
              int projectionid = 32761;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }
            fprintf(stderr, "Reference longitude: %.9f latitude:%.9f Projection ID: %s\n",
                    reference_lon, reference_lat, projection_id);

            /* initialize projection */
            if (mb_proj_init(verbose, projection_id, &(pjptr), &error) != MB_SUCCESS) {
              char *error_message = NULL;
              mb_error(verbose, error, &error_message);
              fprintf(stderr, "\nMBIO Error initializing projection:\n%s\n", error_message);
              fprintf(stderr, "\nProgram terminated in <%s>\n", __func__);
              fclose(sfp);   /* InteractiveGMT port: was mb_memory_clear() + exit() */
              return (MB_FAILURE);
            }
            mb_proj_forward(verbose, pjptr, reference_lon, reference_lat, &origin_x, &origin_y, &error);
          }

          /* output route as Risi script */
          if (pjptr != NULL && npointtotal > 0) {
            /* write the route header */
            fprintf(sfp, "## ROUTENAME %s\r\n", routename);
            fprintf(sfp, "## ROUTEPOINTS %d\r\n", npointtotal);
            fprintf(sfp, "## STARTROUTE\r\n");

            /* write the route points */
            vvspeed = 0.20;
            settlingtime = 3.0;
            altitude = 3.0;
            turndirection = 1;
            double turns = 0.0;
            double heading = routebearing[0];
            double headinglast = 0.0;
            double dheading = heading - headinglast;
            if (dheading > 180.0)
              dheading -= 360.0;
            else if (dheading < -180.0)
              dheading += 360.0;
            if (dheading >= -180.0 && dheading < 0.0) {
              turndirection = -1;
            } else if (dheading >= 0.0 && dheading < 180.0) {
              turndirection = 1;
            }

            fprintf(sfp, "ALT, %.3f, 0.1, 3\r\n", altitude);
            fprintf(sfp, "##\n");
            fprintf(sfp, "HDG, %.3f, %d, 6, %.3f\r\n", heading, turndirection, settlingtime);
            headinglast = heading;
            for (j = 0; j < npointtotal; j++) {
              if (j >= 0 && routewaypoint[j] > MBV_ROUTE_WAYPOINT_NONE) {
                double xxxx, yyyy, zz;
                mb_proj_forward(verbose, pjptr, routelon[j], routelat[j], &xxxx, &yyyy, &error);
                xxxx = xxxx - origin_x;
                yyyy = yyyy - origin_y;
                zz = -altitude;
                //heading = routebearing[j];
                dheading = heading - headinglast;
                if (dheading > 180.0)
                  dheading -= 360.0;
                else if (dheading < -180.0)
                  dheading += 360.0;
                if (dheading >= -180.0 && dheading < 0.0) {
                  turndirection = -1;
                } else if (dheading >= 0.0 && dheading < 180.0) {
                  turndirection = 1;
                }
                fprintf(sfp, "POS, %.3f, %.3f, %.3f, %.3f, %.3f\r\n", yyyy, xxxx, zz, vvspeed, settlingtime);
                //fprintf(sfp, "##\n");
                //fprintf(sfp, "HDG, %.3f, %d, 6, %.3f\r\n", heading, turndirection, settlingtime);
                turns += dheading / 360.0;
                headinglast = heading;
                fprintf(stderr, "j:%d turns: %f\n", j, turns);
              }
            }
            if (turns < 0.0)
              turndirection = -1;
            else
              turndirection = 1;
            fprintf(sfp, "HDG, %.3f, %d, 6, %.3f\r\n", 0.0, turndirection, settlingtime);

            /* write the route end */
            fprintf(sfp, "## End\r\n");
          }

          /* deallocate arrays */
          if (npointalloc > 0) {
            status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                            &distlateral, &distovertopo, &slope, &error);
          }
        }
      }

      /* close the output file */
      fclose(sfp);
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saverisi2scriptheading(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int nroutewrite = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  int iroute, j;
  void *pjptr = NULL;
  double origin_x, origin_y;
  double vvspeed = 0.2;
  double settlingtime = 3.0;
  double altitude = 3.0;
  int turndirection = 1;

  if (verbose >= 0) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected)
        nroutewrite++;
    }
    if (nroutewrite == 0)
      nroutewrite = nroute;
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the route file header */
        fprintf(sfp, "## Risi 2 Script Version %s\r\n", MBGRDVIZ_RISI2SCRIPT_VERSION);
        fprintf(sfp, "## Output by Program %s\r\n", program_name);
        fprintf(sfp, "## MB-System Version %s\r\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\r\n", user, host, date);
        fprintf(sfp, "## Number of routes: %d\r\n", nroutewrite);
        fprintf(sfp, "## Risi script format:\r\n");
        fprintf(sfp, "##   altitude <altitude (m)>\r\n");
        fprintf(sfp, "##   heading rate <rate (deg/sec)>\r\n");
        fprintf(sfp, "##   heading abs <+/-heading (deg)>, <turn direction +/-1>\r\n");
        fprintf(sfp, "##   heading rel <+/-heading (deg)>\r\n");
        fprintf(sfp, "##   move bearing, <range (m)>, <bearing (deg)>\r\n");
        fprintf(sfp, "##\r\n");
        fprintf(sfp, "## This script assumes the survey platform starts at the origin with heading 0.0\r\n");
        fprintf(sfp, "##\r\n");
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\r\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* check if this route is selected for writing */
        if (nroutewrite == nroute)
          selected = true;
        else
          mbview_getrouteselected(verbose, instance, iroute, &selected, &error);

        /* output if selected */
        if (selected) {
          /* get point count for current route */
          status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

          /* allocate route arrays */
          npointtotal = npoint + nintpoint;
          if (status == MB_SUCCESS && npointalloc < npointtotal) {
            status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                             &routebearing, &distlateral, &distovertopo, &slope, &error);
            if (status == MB_SUCCESS) {
              npointalloc = npointtotal;
            }

            /* if error initializing memory then cancel dealing with this route */
            else {
              fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                      npointtotal, instance);
              XBell((Display *)XtDisplay(mainWindow), 100);
              npoint = 0;
              nintpoint = 0;
              npointtotal = 0;
            }
          }

          /* extract data for route */
          status = mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint,
                                   routetopo, routebearing, distlateral, distovertopo, slope, &routecolor, &routesize,
                                   &routeeditmode, routename, &error);

          /* if this the first route define the projection */
          if (pjptr == NULL && npointtotal > 0) {
            double reference_lon = routelon[0];
            double reference_lat = routelat[0];
            mb_path projection_id;

            /* calculate eastings and northings using an LTM projection */
            if (reference_lat > -80.0 && reference_lat < 84.0) {
              if (reference_lon > 180.0)
                reference_lon -= 360.0;
              sprintf(projection_id, "LTM%.5f/%.5f", reference_lon, reference_lat);
            }

            /* else if more northerly than 84 deg N then use
                    North Universal Polar Stereographic Projection */
            else if (reference_lat > 84.0) {
              int projectionid = 32661;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }

            /* else if more southerly than 80 deg S then use
                    South Universal Polar Stereographic Projection */
            else if (reference_lat < 80.0) {
              int projectionid = 32761;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }
            fprintf(stderr, "Reference longitude: %.9f latitude:%.9f Projection ID: %s\n",
                    reference_lon, reference_lat, projection_id);

            /* initialize projection */
            if (mb_proj_init(verbose, projection_id, &(pjptr), &error) != MB_SUCCESS) {
              char *error_message = NULL;
              mb_error(verbose, error, &error_message);
              fprintf(stderr, "\nMBIO Error initializing projection:\n%s\n", error_message);
              fprintf(stderr, "\nProgram terminated in <%s>\n", __func__);
              fclose(sfp);   /* InteractiveGMT port: was mb_memory_clear() + exit() */
              return (MB_FAILURE);
            }
            mb_proj_forward(verbose, pjptr, reference_lon, reference_lat, &origin_x, &origin_y, &error);
          }

          /* output route as Risi 2 script with start at the origin and relative movements */
          if (pjptr != NULL && npointtotal > 0) {
            /* write the route header */
            fprintf(sfp, "## ROUTENAME %s\r\n", routename);
            fprintf(sfp, "## ROUTEPOINTS %d\r\n", npointtotal);
            fprintf(sfp, "## STARTROUTE\r\n");

            /* write the route points */
            fprintf(sfp, "## altitude, %.3f\r\n", altitude);
            fprintf(sfp, "##\n");
            fprintf(sfp, "## heading abs 0.0\r\n");
            double xx0, yy0, xx1, yy1;
            double heading0, heading1, dheading;
            for (j = 0; j < npointtotal; j++) {
              double xxxx, yyyy;
              double range;
              double bearing, heading0, heading1;
              if (routewaypoint[j] > MBV_ROUTE_WAYPOINT_NONE) {
                mb_proj_forward(verbose, pjptr, routelon[j], routelat[j], &xxxx, &yyyy, &error);
                xxxx = xxxx - origin_x;
                yyyy = yyyy - origin_y;
                if (j == 0) {
                  xx0 = xxxx;
                  yy0 = yyyy;
                  heading0 = routebearing[j];
                }
                else {
                  xx1 = xxxx;
                  yy1 = yyyy;
                  range = sqrt((xx1 - xx0) * (xx1 - xx0) + (yy1 - yy0) * (yy1 - yy0));
                  heading1 = routebearing[j];
                  dheading = heading1 - heading0;
                  if (dheading > 180.0) {
                    dheading -= 360.0;
                  } else if (dheading < -180.0) {
                    dheading += 360.0;
                  }
                  fprintf(sfp, "move bearing 0.0, %.2f", range);
                  fprintf(sfp, "heading rel %.2f", dheading);
                  xx0 = xx1;
                  yy0 = yy1;
                  heading0 = heading1;
                }
              }
            }

            /* write the route end */
            fprintf(sfp, "## End\r\n");
          }

          /* deallocate arrays */
          if (npointalloc > 0) {
            status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                            &distlateral, &distovertopo, &slope, &error);
          }
        }
      }

      /* close the output file */
      fclose(sfp);
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saverisi2scriptnoheading(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int nroutewrite = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  int iroute, j;
  void *pjptr = NULL;
  double origin_x, origin_y;
  double vvspeed = 0.2;
  double settlingtime = 3.0;
  double altitude = 3.0;
  int turndirection = 1;

  if (verbose >= 0) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected)
        nroutewrite++;
    }
    if (nroutewrite == 0)
      nroutewrite = nroute;
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the route file header */
        fprintf(sfp, "## Risi 2 Script Version %s\r\n", MBGRDVIZ_RISI2SCRIPT_VERSION);
        fprintf(sfp, "## Output by Program %s\r\n", program_name);
        fprintf(sfp, "## MB-System Version %s\r\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\r\n", user, host, date);
        fprintf(sfp, "## Number of routes: %d\r\n", nroutewrite);
        fprintf(sfp, "## Risi script format:\r\n");
        fprintf(sfp, "##   altitude <altitude (m)>\r\n");
        fprintf(sfp, "##   heading rate <rate (deg/sec)>\r\n");
        fprintf(sfp, "##   heading abs <+/-heading (deg)>, <turn direction +/-1>\r\n");
        fprintf(sfp, "##   heading rel <+/-heading (deg)>\r\n");
        fprintf(sfp, "##   move bearing, <range (m)>, <bearing (deg)>\r\n");
        fprintf(sfp, "##\r\n");
        fprintf(sfp, "## This script assumes the survey platform starts at the origin with heading 0.0\r\n");
        fprintf(sfp, "##\r\n");
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\r\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS && nroutewrite > 0) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* check if this route is selected for writing */
        if (nroutewrite == nroute)
          selected = true;
        else
          mbview_getrouteselected(verbose, instance, iroute, &selected, &error);

        /* output if selected */
        if (selected) {
          /* get point count for current route */
          status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

          /* allocate route arrays */
          npointtotal = npoint + nintpoint;
          if (status == MB_SUCCESS && npointalloc < npointtotal) {
            status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                             &routebearing, &distlateral, &distovertopo, &slope, &error);
            if (status == MB_SUCCESS) {
              npointalloc = npointtotal;
            }

            /* if error initializing memory then cancel dealing with this route */
            else {
              fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                      npointtotal, instance);
              XBell((Display *)XtDisplay(mainWindow), 100);
              npoint = 0;
              nintpoint = 0;
              npointtotal = 0;
            }
          }

          /* extract data for route */
          status = mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint,
                                   routetopo, routebearing, distlateral, distovertopo, slope, &routecolor, &routesize,
                                   &routeeditmode, routename, &error);

          /* if this the first route define the projection */
          if (pjptr == NULL && npointtotal > 0) {
            double reference_lon = routelon[0];
            double reference_lat = routelat[0];
            mb_path projection_id;

            /* calculate eastings and northings using an LTM projection */
            if (reference_lat > -80.0 && reference_lat < 84.0) {
              if (reference_lon > 180.0)
                reference_lon -= 360.0;
              sprintf(projection_id, "LTM%.5f/%.5f", reference_lon, reference_lat);
            }

            /* else if more northerly than 84 deg N then use
                    North Universal Polar Stereographic Projection */
            else if (reference_lat > 84.0) {
              int projectionid = 32661;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }

            /* else if more southerly than 80 deg S then use
                    South Universal Polar Stereographic Projection */
            else if (reference_lat < 80.0) {
              int projectionid = 32761;
              sprintf(projection_id, "EPSG:%d", projectionid);
            }
            fprintf(stderr, "Reference longitude: %.9f latitude:%.9f Projection ID: %s\n",
                    reference_lon, reference_lat, projection_id);

            /* initialize projection */
            if (mb_proj_init(verbose, projection_id, &(pjptr), &error) != MB_SUCCESS) {
              char *error_message = NULL;
              mb_error(verbose, error, &error_message);
              fprintf(stderr, "\nMBIO Error initializing projection:\n%s\n", error_message);
              fprintf(stderr, "\nProgram terminated in <%s>\n", __func__);
              fclose(sfp);   /* InteractiveGMT port: was mb_memory_clear() + exit() */
              return (MB_FAILURE);
            }
            mb_proj_forward(verbose, pjptr, reference_lon, reference_lat, &origin_x, &origin_y, &error);
          }

          /* output route as Risi 2 script with start at the origin and relative movements */
          if (pjptr != NULL && npointtotal > 0) {
            /* write the route header */
            fprintf(sfp, "## ROUTENAME %s\r\n", routename);
            fprintf(sfp, "## ROUTEPOINTS %d\r\n", npointtotal);
            fprintf(sfp, "## STARTROUTE\r\n");

            /* write the route points */
            fprintf(sfp, "## altitude, %.3f\r\n", altitude);
            fprintf(sfp, "##\n");
            fprintf(sfp, "## heading abs 0.0\r\n");
            double xx0, yy0, xx1, yy1;
            double heading0, heading1, dheading;
            for (j = 0; j < npointtotal; j++) {
              double xxxx, yyyy;
              double range;
              double bearing, heading0, heading1;
              if (routewaypoint[j] > MBV_ROUTE_WAYPOINT_NONE) {
                mb_proj_forward(verbose, pjptr, routelon[j], routelat[j], &xxxx, &yyyy, &error);
                xxxx = xxxx - origin_x;
                yyyy = yyyy - origin_y;
                if (j == 0) {
                  xx0 = xxxx;
                  yy0 = yyyy;
                  heading0 = routebearing[j];
                }
                else {
                  xx1 = xxxx;
                  yy1 = yyyy;
                  range = sqrt((xx1 - xx0) * (xx1 - xx0) + (yy1 - yy0) * (yy1 - yy0));
                  heading1 = routebearing[j];
                  dheading = heading1 - heading0;
                  if (dheading > 180.0) {
                    dheading -= 360.0;
                  } else if (dheading < -180.0) {
                    dheading += 360.0;
                  }
                  fprintf(sfp, "move bearing 0.0, %.2f", range);
                  fprintf(sfp, "heading rel %.2f", dheading);
                  xx0 = xx1;
                  yy0 = yy1;
                  heading0 = heading1;
                }
              }
            }

            /* write the route end */
            fprintf(sfp, "## End\r\n");
          }

          /* deallocate arrays */
          if (npointalloc > 0) {
            status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                            &distlateral, &distovertopo, &slope, &error);
          }
        }
      }

      /* close the output file */
      fclose(sfp);
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savedegdecmin(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  char latNS, lonEW;
  int latDeg, lonDeg;
  double latMin, lonMin;
  int iroute, j, n;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroute > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* get point count for current route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

        /* allocate route arrays */
        npointtotal = npoint + nintpoint;
        if (status == MB_SUCCESS && npointalloc < npointtotal) {
          status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                           &routebearing, &distlateral, &distovertopo, &slope, &error);
          if (status == MB_SUCCESS) {
            npointalloc = npointtotal;
          }

          /* if error initializing memory then cancel dealing with this route */
          else {
            fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                    npointtotal, instance);
            XBell((Display *)XtDisplay(mainWindow), 100);
            npoint = 0;
            nintpoint = 0;
            npointtotal = 0;
          }
        }

        /* extract data for route */
        status =
            mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint, routetopo,
                            routebearing, distlateral, distovertopo, slope, &routecolor, &routesize, 
                            &routeeditmode, routename, &error);

        /* write the route points */
        n = 0;
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            n++;
          }
        }
        if (iroute > 0)
          fprintf(sfp, "#\r\n");
        fprintf(sfp, "# Route: %s\r\n", routename);
        fprintf(sfp, "# Number of waypoints: %d\r\n", n);
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            if (routelat[j] >= 0.0)
              latNS = 'N';
            else
              latNS = 'S';
            latDeg = (int)(floor(fabs(routelat[j])));
            latMin = (fabs(routelat[j]) - (double)latDeg) * 60.0;
            if (routelon[j] >= 0.0)
              lonEW = 'E';
            else
              lonEW = 'W';
            lonDeg = (int)(floor(fabs(routelon[j])));
            lonMin = (fabs(routelon[j]) - (double)lonDeg) * 60.0;

            fprintf(sfp, "%c %3d %9.6f   %c %3d %9.6f \r\n", latNS, latDeg, latMin, lonEW, lonDeg, lonMin);
          }
        }
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savelnw(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  char *error_message;
  char projection_id[MB_PATH_MAXLINE];
  void *pjptr = NULL;
  int proj_status;
  int utm_zone;
  double reference_lon, reference_lat;
  double easting, northing;
  int iroute, j, n;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroute > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open route file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {
      /* output number of routes */
      fprintf(sfp, "LNS %d\r\n", nroute);

      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* get point count for current route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

        /* allocate route arrays */
        npointtotal = npoint + nintpoint;
        if (status == MB_SUCCESS && npointalloc < npointtotal) {
          status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                           &routebearing, &distlateral, &distovertopo, &slope, &error);
          if (status == MB_SUCCESS) {
            npointalloc = npointtotal;
          }

          /* if error initializing memory then cancel dealing with this route */
          else {
            fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                    npointtotal, instance);
            XBell((Display *)XtDisplay(mainWindow), 100);
            npoint = 0;
            nintpoint = 0;
            npointtotal = 0;
          }
        }

        /* extract data for route */
        status =
            mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint, routetopo,
                            routebearing, distlateral, distovertopo, slope, &routecolor, &routesize, 
                            &routeeditmode, routename, &error);

        /* if this the first route define the projection */
        if (pjptr == NULL && npointtotal > 0) {
          reference_lon = 0.0;
          reference_lat = 0.0;
          for (j = 0; j < npointtotal; j++) {
            reference_lon += routelon[j];
            reference_lat += routelat[j];
          }
          reference_lon = reference_lon / npointtotal;
          reference_lat = reference_lat / npointtotal;
          if (reference_lon < 180.0)
            reference_lon += 360.0;
          if (reference_lon >= 180.0)
            reference_lon -= 360.0;
          utm_zone = (int)(((reference_lon + 183.0) / 6.0) + 0.5);
          if (reference_lat >= 0.0)
            sprintf(projection_id, "UTM%2.2dN", utm_zone);
          else
            sprintf(projection_id, "UTM%2.2dS", utm_zone);
          fprintf(stderr, "Reference longitude: %.9f latitude:%.9f\nOutput lnw file in projection:%s\n", reference_lon,
                  reference_lat, projection_id);

          /* initialize appropriate UTM projection */
          proj_status = mb_proj_init(verbose, projection_id, &(pjptr), &error);

          /* quit if projection fails */
          if (proj_status != MB_SUCCESS) {
            mb_error(verbose, error, &error_message);
            fprintf(stderr, "\nMBIO Error initializing projection:\n%s\n", error_message);
            fprintf(stderr, "\nProgram terminated in <%s>\n", __func__);
            fclose(sfp);   /* InteractiveGMT port: was mb_memory_clear() + exit() */
            return (MB_FAILURE);
          }
        }

        /* write the route points */
        n = 0;
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            n++;
          }
        }
        fprintf(sfp, "LIN %d\r\n", n);
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            proj_status = mb_proj_forward(verbose, pjptr, routelon[j], routelat[j], &easting, &northing, &error);
            fprintf(sfp, "PTS %.2f %.2f\r\n", easting, northing);
          }
        }
        fprintf(sfp, "LNN %d\r\nEOL\r\n", iroute + 1);
      }

      /* free the projection */
      mb_proj_free(verbose, &pjptr, &error);

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savegreenseayml(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  bool selected;
  // char *error_message;
  // char projection_id[MB_PATH_MAXLINE];
  // void *pjptr = NULL;
  // int proj_status;
  int nroutewrite = 0;
  int iroutewrite = 0;
  int iroute, j, n;
  #ifdef USE_UUID
  uuid_t mission_uuid, *waypoints_uuid;
  #endif
  char uuid_str[37];

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    for (iroute = 0; iroute < nroute; iroute++) {
      mbview_getrouteselected(verbose, instance, iroute, &selected, &error);
      if (selected) {
        nroutewrite++;
        iroutewrite = iroute;
      }
    }
    if (nroutewrite == 0 && nroute == 1) {
      nroutewrite = 1;
      iroutewrite = 0;
    }
    if (nroutewrite != 1) {
      fprintf(stderr, "Unable to write Greensea YML survey script...\n");
      fprintf(stderr, "Exactly one route must be selected, but %d routes are selected for instance %zu!\n", nroutewrite, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroutewrite == 1) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open Greensea survey script file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {
      /* get point count for current route */
      status = mbview_getroutepointcount(verbose, instance, iroutewrite, &npoint, &nintpoint, &error);

      /* allocate route arrays */
      npointtotal = npoint + nintpoint;
      if (status == MB_SUCCESS && npointalloc < npointtotal) {
        status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                         &routebearing, &distlateral, &distovertopo, &slope, &error);
#ifdef USE_UUID
        if (status == MB_SUCCESS) {
          status = mb_mallocd(verbose, __FILE__, __LINE__,
                              npointtotal * sizeof(uuid_t),
                              (void **)&waypoints_uuid, &error);
        }
#endif
        if (status == MB_SUCCESS) {
          npointalloc = npointtotal;
        }

        /* if error initializing memory then cancel dealing with this route */
        else {
          fprintf(stderr, "Unable to write route...\nArray allocation for %d points failed for instance %zu!\n",
                  npointtotal, instance);
          XBell((Display *)XtDisplay(mainWindow), 100);
          npoint = 0;
          nintpoint = 0;
          npointtotal = 0;
        }
      }

      /* extract data for route */
      status = mbview_getroute(verbose, instance, iroutewrite, &npointtotal,
                                routelon, routelat, routewaypoint, routetopo,
                                routebearing, distlateral, distovertopo, slope,
                                &routecolor, &routesize, &routeeditmode, routename, &error);

      /* output header of mission */
      fprintf(sfp, "mission_data:\n");
#ifdef USE_UUID
      uuid_generate(mission_uuid);
      uuid_unparse(mission_uuid, uuid_str);
#else
      sprintf(uuid_str, "MBsystem-1962-1991-2018-%12.12d", npointtotal);
#endif
      fprintf(sfp, "  - id: %s\n", uuid_str);
      fprintf(sfp, "    name: Low_Altitude_Survey\n");
      fprintf(sfp, "    locked: true\n");
      fprintf(sfp, "    waypoints:\n");

      /* write the route points */
      for (j = 0; j < npointtotal; j++) {
        if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
#ifdef USE_UUID
          uuid_generate(waypoints_uuid[j]);
          uuid_unparse(waypoints_uuid[j], uuid_str);
#else
          sprintf(uuid_str, "Waypoint-abcd-efgh-ijkl-%12.12d", j);
#endif
          fprintf(sfp, "    - id: %s\n", uuid_str);
        }
      }
      fprintf(sfp, "waypoint_data:\n");
      n = 0;
      for (j = 0; j < npointtotal; j++) {
        if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
#ifdef USE_UUID
          uuid_unparse(waypoints_uuid[j], uuid_str);
#else
          sprintf(uuid_str, "Waypoint-abcd-efgh-ijkl-%12.12d", j);
#endif
          fprintf(sfp, "  - id: %s\n", uuid_str);
          fprintf(sfp, "    name: SPS%4.4d\n", n);
          fprintf(sfp, "    x: %.9f\n", routelon[j]);
          fprintf(sfp, "    y: %.9f\n", routelat[j]);
          fprintf(sfp, "    z: %.3f\n", 3.0);
          fprintf(sfp, "    tolerance: %.3f\n", 0.500);
          fprintf(sfp, "    z_alt: true\n");
          fprintf(sfp, "    z_matters: true\n");
          fprintf(sfp, "    speed: %.3f\n", 0.150);
          fprintf(sfp, "    use_speed: true\n");
          fprintf(sfp, "    effort: 70.000\n");
          if (j==0)
            fprintf(sfp, "    heading: %.3f\n", routebearing[j]);
          else
              fprintf(sfp, "    heading: %.3f\n", routebearing[j-1]);
          fprintf(sfp, "    heading_mode: FIXED\n");
          n++;
        }
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
#ifdef USE_UUID
        status = mb_freed(verbose, __FILE__, __LINE__, (void **)&waypoints_uuid, &error);
#endif
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savetecdislst(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  char latNS, lonEW;
  int latDeg, lonDeg;
  double latMin, lonMin;
  int iroute, j, n;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write TECDIS LST route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroute > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to open route file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {
      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* get point count for current route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

        /* allocate route arrays */
        npointtotal = npoint + nintpoint;
        if (status == MB_SUCCESS && npointalloc < npointtotal) {
          status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                           &routebearing, &distlateral, &distovertopo, &slope, &error);
          if (status == MB_SUCCESS) {
            npointalloc = npointtotal;
          }

          /* if error initializing memory then cancel dealing with this route */
          else {
            fprintf(stderr, "Unable to write TECDIS LST route file...\nArray allocation for %d points failed for instance %zu!\n",
                    npointtotal, instance);
            XBell((Display *)XtDisplay(mainWindow), 100);
            npoint = 0;
            nintpoint = 0;
            npointtotal = 0;
          }
        }

        /* extract data for route */
        status =
            mbview_getroute(verbose, instance, iroute, &npointtotal, routelon, routelat, routewaypoint, routetopo,
                            routebearing, distlateral, distovertopo, slope, &routecolor, &routesize, 
                            &routeeditmode, routename, &error);

        /* write the route points */
        n = 0;
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            n++;
          }
        }
        if (iroute > 0)
          fprintf(sfp, "#\r\n");
        fprintf(sfp, "# Route: %s\r\n", routename);
        fprintf(sfp, "# Number of waypoints: %d\r\n", n);
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            if (routelat[j] >= 0.0)
              latNS = 'N';
            else
              latNS = 'S';
            latDeg = (int)(floor(fabs(routelat[j])));
            latMin = (fabs(routelat[j]) - (double)latDeg) * 60.0;
            if (routelon[j] >= 0.0)
              lonEW = 'E';
            else
              lonEW = 'W';
            lonDeg = (int)(floor(fabs(routelon[j])));
            lonMin = (fabs(routelon[j]) - (double)lonDeg) * 60.0;

			if (j == 0) {
              fprintf(sfp, "$PTLKR,0,0,%s\r\n", output_file_ptr);
              fprintf(sfp, "$PTLKP,8,%2.2d%9.6f,%c,%3.3d%9.6f,%c\r\n", latDeg, latMin, latNS, lonDeg, lonMin, lonEW);
			}
            fprintf(sfp, "$PTLKP,9,%2.2d%9.6f,%c,%3.3d%9.6f,%c\r\n", latDeg, latMin, latNS, lonDeg, lonMin, lonEW);
          }
        }
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savekongsbergdp(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  char latNS, lonEW;
  int latDeg, lonDeg;
  double latMin, lonMin;
  int iroute, j, n;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write Kongsberg DP waypoint file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroute > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to open Kongsberg DP waypoint file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {

			/* get the creation time */
			//char user[256], host[256], date[32];
			//status = mb_user_host_date(verbose, user, host, date, &error);
			//fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
			time_t right_now = time(NULL);
			struct tm *tm_ptr = gmtime(&right_now);
			double time_d = (double) right_now;
			int time_i[7];
			mb_get_date(verbose, time_d, time_i);
			fprintf(sfp, "CreateDate (UTC),%s, %s %d, %d %2.2d:%2.2d:%2.2d\r\nVersion,4\r\n", 
								mb_day_name(verbose, tm_ptr->tm_wday + 1), mb_month_name(verbose, 
								time_i[1]), time_i[2], time_i[0], time_i[3], time_i[4], time_i[5]);

      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* get point count for current route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

        /* allocate route arrays */
        npointtotal = npoint + nintpoint;
        if (status == MB_SUCCESS && npointalloc < npointtotal) {
          status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                           &routebearing, &distlateral, &distovertopo, &slope, &error);
          if (status == MB_SUCCESS) {
            npointalloc = npointtotal;
          }

          /* if error initializing memory then cancel dealing with this route */
          else {
            fprintf(stderr, "Unable to write Kongsberg DP waypoint file...\nArray allocation for %d points failed for instance %zu!\n",
                    npointtotal, instance);
            XBell((Display *)XtDisplay(mainWindow), 100);
            npoint = 0;
            nintpoint = 0;
            npointtotal = 0;
          }
        }

        /* extract data for route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);
        status = mbview_getroute(verbose, instance, iroute, &npointtotal, 
        										routelon, routelat, routewaypoint, routetopo, routebearing, 
        										distlateral, distovertopo, slope, &routecolor, &routesize, 
                            &routeeditmode, routename, &error);
                            
			  fprintf(sfp, "TrackName,%s\r\n", routename);
			  fprintf(sfp, "NoOfWp,%d\r\n", npoint);
			  fprintf(sfp, "Datum,WGS84\r\n");
			  fprintf(sfp, "WPFormat,WPId,WPHemisNS,WPLatDeg,WPLatMin,WPHemisEW,WPLonDeg,WPLonMin,WPLegType,WPHead,WPSpeed,WPTurnRad\r\n");

        /* write the route points */
        int noutputwaypoints = 0;
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
            noutputwaypoints++;
            if (routelat[j] >= 0.0)
              latNS = 'N';
            else
              latNS = 'S';
            latDeg = (int)(floor(fabs(routelat[j])));
            latMin = (fabs(routelat[j]) - (double)latDeg) * 60.0;
            if (routelon[j] >= 0.0)
              lonEW = 'E';
            else
              lonEW = 'W';
            lonDeg = (int)(floor(fabs(routelon[j])));
            lonMin = (fabs(routelon[j]) - (double)lonDeg) * 60.0;

            fprintf(sfp, "WP,%d,%c,%2.2d,%09.6f,%c,%3.3d,%09.6f,0,  0.000,5.0000,350.00\r\n", 
            				noutputwaypoints,latNS, latDeg, latMin, lonEW, lonDeg, lonMin);
          }
        }
			  fprintf(sfp, "END\r\n");
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savesisasciiplan1(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  char latNS, lonEW;
  int latDeg, lonDeg;
  double latMin, lonMin;
  int iroute, j, n;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write SIS Asciiplan line file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroute > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to open SIS Asciiplan line file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {

			/* get the creation time */
			time_t right_now = time(NULL);
			struct tm *tm_ptr = gmtime(&right_now);
			double time_d = (double) right_now;
			int time_i[7];
			mb_get_date(verbose, time_d, time_i);
			fprintf(sfp, "DEG\r\n\r\n0 0 0 0\r\n");

      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* get point count for current route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

        /* allocate route arrays */
        npointtotal = npoint + nintpoint;
        if (status == MB_SUCCESS && npointalloc < npointtotal) {
          status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                           &routebearing, &distlateral, &distovertopo, &slope, &error);
          if (status == MB_SUCCESS) {
            npointalloc = npointtotal;
          }

          /* if error initializing memory then cancel dealing with this route */
          else {
            fprintf(stderr, "Unable to write SIS Asciiplan line file...\nArray allocation for %d points failed for instance %zu!\n",
                    npointtotal, instance);
            XBell((Display *)XtDisplay(mainWindow), 100);
            npoint = 0;
            nintpoint = 0;
            npointtotal = 0;
          }
        }

        /* extract data for route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);
        status = mbview_getroute(verbose, instance, iroute, &npointtotal, 
        										routelon, routelat, routewaypoint, routetopo, routebearing, 
        										distlateral, distovertopo, slope, &routecolor, &routesize, 
                            &routeeditmode, routename, &error);
                            
        /* write the route points */
        int noutputlines = 0;
        double lastlon, lastlat;
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
          	if (j > 0) {
							fprintf(sfp, "_LINE Line%d %d %4.4d%2.2d%2.2d%2.2d%2.2d%2.2d 0 %.9f %.9f %.9f %.9f \"\r\n",
											noutputlines, noutputlines, 
											time_i[0], time_i[1], time_i[2], time_i[3], time_i[4], time_i[5], 
											lastlat, lastlon, routelat[j], routelon[j]);
							noutputlines++;
            }
						lastlon = routelon[j];
						lastlat = routelat[j];
          }
        }
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_savesisasciiplan2(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int nroute = 0;
  int npoint = 0;
  int nintpoint = 0;
  int npointtotal = 0;
  int npointalloc = 0;
  double *routelon = NULL;
  double *routelat = NULL;
  int *routewaypoint = NULL;
  double *routetopo = NULL;
  double *routebearing = NULL;
  double *distlateral = NULL;
  double *distovertopo = NULL;
  double *slope = NULL;
  int routecolor;
  int routesize;
  int routeeditmode;
  mb_path routename;
  char latNS, lonEW;
  int latDeg, lonDeg;
  double latMin, lonMin;
  int iroute, j, n;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of routes to be written to the output file */
    status = mbview_getroutecount(verbose, instance, &nroute, &error);
    if (nroute <= 0) {
      fprintf(stderr, "Unable to write SIS Asciiplan route file...\nCurrently %d routes defined for instance %zu!\n", nroute, instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && nroute > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) == NULL) {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to open SIS Asciiplan route file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output routes */
    if (status == MB_SUCCESS) {

			/* get the creation time */
			time_t right_now = time(NULL);
			struct tm *tm_ptr = gmtime(&right_now);
			double time_d = (double) right_now;
			int time_i[7];
			mb_get_date(verbose, time_d, time_i);
			fprintf(sfp, "DEG\r\n\r\n0 0 0 0\r\n");

      /* loop over routes */
      for (iroute = 0; iroute < nroute; iroute++) {
        /* get point count for current route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);

        /* allocate route arrays */
        npointtotal = npoint + nintpoint;
        if (status == MB_SUCCESS && npointalloc < npointtotal) {
          status = mbview_allocroutearrays(verbose, npointtotal, &routelon, &routelat, &routewaypoint, &routetopo,
                                           &routebearing, &distlateral, &distovertopo, &slope, &error);
          if (status == MB_SUCCESS) {
            npointalloc = npointtotal;
          }

          /* if error initializing memory then cancel dealing with this route */
          else {
            fprintf(stderr, "Unable to write SIS Asciiplan route file...\nArray allocation for %d points failed for instance %zu!\n",
                    npointtotal, instance);
            XBell((Display *)XtDisplay(mainWindow), 100);
            npoint = 0;
            nintpoint = 0;
            npointtotal = 0;
          }
        }

        /* extract data for route */
        status = mbview_getroutepointcount(verbose, instance, iroute, &npoint, &nintpoint, &error);
        status = mbview_getroute(verbose, instance, iroute, &npointtotal, 
        										routelon, routelat, routewaypoint, routetopo, routebearing, 
        										distlateral, distovertopo, slope, &routecolor, &routesize, 
                            &routeeditmode, routename, &error);
                            
        /* write the route points */
				fprintf(sfp, "_LINE Route%d %d %4.4d%2.2d%2.2d%2.2d%2.2d%2.2d 0",
								iroute, iroute, 
								time_i[0], time_i[1], time_i[2], time_i[3], time_i[4], time_i[5]);
        for (j = 0; j < npointtotal; j++) {
          if (routewaypoint[j] != MBV_ROUTE_WAYPOINT_NONE) {
						fprintf(sfp, " %.9f %.9f", routelat[j], routelon[j]);
          }
        }
				fprintf(sfp, " \"\r\n");
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeroutearrays(verbose, &routelon, &routelat, &routewaypoint, &routetopo, &routebearing,
                                        &distlateral, &distovertopo, &slope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_openvector(size_t instance, char *input_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  char buffer[MB_PATH_MAXLINE];
  int npoint = 0;
  int npointalloc = 0;
  double *vectorlon = NULL;
  double *vectorlat = NULL;
  double *vectorz = NULL;
  double *vectordata = NULL;
  double lon, lat, z, data;
  int vectorcolor;
  int vectorsize;
  double vectordatamin;
  double vectordatamax;
  bool minmax_set = false;
  mb_path vectorname;
  bool rawvectorfile = true;
  char *result;
  int nget;
  int point_ok;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       input_file_ptr:  %s\n", input_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {
    /* initialize vector values */
    vectorcolor = MBV_COLOR_BLUE;
    vectorsize = 4;
    vectorname[0] = '\0';
    rawvectorfile = true;
    npoint = 0;
    npointalloc = 0;
    vectordatamin = 0.0;
    vectordatamax = 0.0;
    minmax_set = false;

    /* open the input file */
    if ((sfp = fopen(input_file_ptr, "r")) == NULL) {
      error = MB_ERROR_OPEN_FAIL;
      status = MB_FAILURE;
      fprintf(stderr, "\nUnable to open vector file <%s> for reading\n", input_file_ptr);
      XBell((Display *)XtDisplay(mainWindow), 100);
    }

    /* loop over reading */
    if (status == MB_SUCCESS) {
      fprintf(stderr, "Reading from vector file:%s\n", input_file_ptr);
      while ((result = fgets(buffer, MB_PATH_MAXLINE, sfp)) == buffer) {
        /* deal with comments */
        if (buffer[0] == '#') {
          if (rawvectorfile && strncmp(buffer, "## Vector File Version", 21) == 0) {
            rawvectorfile = false;
          }
          else if (strncmp(buffer, "## VECTORNAME", 12) == 0) {
            sscanf(buffer, "## VECTORNAME %s", vectorname);
          }
          else if (strncmp(buffer, "## VECTORCOLOR", 13) == 0) {
            sscanf(buffer, "## ROUTECOLOR %d", &vectorcolor);
          }
          else if (strncmp(buffer, "## ROUTESIZE", 12) == 0) {
            sscanf(buffer, "## ROUTESIZE %d", &vectorsize);
          }
          else if (strncmp(buffer, "## MIN", 6) == 0) {
            sscanf(buffer, "## MIN %lf", &vectordatamin);
            minmax_set = true;
          }
          else if (strncmp(buffer, "## MAX", 6) == 0) {
            sscanf(buffer, "## MAX %lf", &vectordatamax);
            minmax_set = true;
          }
        }

        /* deal with vector segment marker */
        else if (buffer[0] == '>') {
          /* if data accumulated call mbview_addvector() */
          if (npoint > 0) {
            status = mbview_addvector(verbose, instance, npoint, vectorlon, vectorlat, vectorz, vectordata,
                                      vectorcolor, vectorsize, vectorname, vectordatamin, vectordatamax, &error);
            npoint = 0;
          }
        }

        /* deal with data */
        else {
          /* read the data from the buffer */
          nget = sscanf(buffer, "%lf %lf %lf %lf", &lon, &lat, &z, &data);
          if (nget == 4)
            point_ok = true;
          else
            point_ok = false;

          /* if good data check for need to allocate more space */
          if (point_ok && npoint + 1 > npointalloc) {
            npointalloc += MBV_ALLOC_NUM;
            status =
                mbview_allocvectorarrays(verbose, npointalloc, &vectorlon, &vectorlat, &vectorz, &vectordata, &error);
            if (status != MB_SUCCESS) {
              npointalloc = 0;
            }
          }

          /* add good point to vector */
          if (point_ok && npointalloc > npoint) {
            vectorlon[npoint] = lon;
            vectorlat[npoint] = lat;
            vectorz[npoint] = z;
            vectordata[npoint] = data;

            /* get min max bounds if not set in file header */
            if (!minmax_set) {
              if (npoint == 0) {
                vectordatamin = data;
                vectordatamax = data;
              }
              else {
                vectordatamin = MIN(vectordatamin, data);
                vectordatamax = MAX(vectordatamax, data);
              }
            }

            /* increment the counter */
            npoint++;
          }
        }
      }

      /* add last vector if not already handled */
      if (npoint > 0) {
        fprintf(stderr, "Adding vector npoints:%d value min max: %f %f\n", npoint, vectordatamin, vectordatamax);
        status = mbview_addvector(verbose, instance, npoint, vectorlon, vectorlat, vectorz, vectordata, vectorcolor,
                                  vectorsize, vectorname, vectordatamin, vectordatamax, &error);
        npoint = 0;
      }

      /* free the memory */
      if (npointalloc > 0)
        status = mbview_freevectorarrays(verbose, &vectorlon, &vectorlat, &vectorz, &vectordata, &error);

      /* close the input file */
      fclose(sfp);
    }

    /* update widgets */
    mbview_enableviewvectors(verbose, instance, &error);
    status = mbview_update(verbose, instance, &error);
  }

  /* set sensitivity of widgets that require an mbview instance to be active */
  do_mbgrdviz_sensitivity();

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_saveprofile(size_t instance, char *output_file_ptr) {
  int status = MB_SUCCESS;
  FILE *sfp;
  int npoints = 0;
  int npointalloc = 0;
  double *prdistance = NULL;
  double *prtopo = NULL;
  int *prboundary = NULL;
  double *prlon = NULL;
  double *prlat = NULL;
  double *prdistovertopo = NULL;
  double *prbearing = NULL;
  double *prslope = NULL;
  mb_path prsourcename;
  double prlength;
  double przmin;
  double przmax;
  int j;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       output_file_ptr: %s\n", output_file_ptr);
  }

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {

    /* get the number of profiles to be written to the output file */
    status = mbview_getprofilecount(verbose, instance, &npoints, &error);
    if (npoints <= 0) {
      fprintf(stderr, "Unable to write profile file...\nCurrently %d profile points defined for instance %zu!\n", npoints,
              instance);
      XBell((Display *)XtDisplay(mainWindow), 100);
      status = MB_FAILURE;
    }

    /* initialize the output file */
    if (status == MB_SUCCESS && npoints > 0) {
      /* open the output file */
      if ((sfp = fopen(output_file_ptr, "w")) != NULL) {
        /* write the profile file header */
        fprintf(sfp, "## Profile File Version %s\n", MBGRDVIZ_PROFILE_VERSION);
        fprintf(sfp, "## Output by Program %s\n", program_name);
        fprintf(sfp, "## MB-System Version %s\n", MB_VERSION);
        char user[256], host[256], date[32];
        status = mb_user_host_date(verbose, user, host, date, &error);
        fprintf(sfp, "## Run by user <%s> on cpu <%s> at <%s>\n", user, host, date);
        fprintf(sfp, "## Number of profile points: %d\n", npoints);
        fprintf(sfp, "## Profile point format:\n");
        fprintf(sfp, "##   <lateral distance (m)> <topography (m)> <boundary (boolean)> <longitude (deg)> <latitude "
                     "(deg)> <distance over topo (m)> <bearing (deg)> <slope (m/m)>\n");
      }

      /* output error message */
      else {
        error = MB_ERROR_OPEN_FAIL;
        status = MB_FAILURE;
        fprintf(stderr, "\nUnable to Open profile file <%s> for writing\n", output_file_ptr);
        XBell((Display *)XtDisplay(mainWindow), 100);
      }
    }

    /* if all ok proceed to extract and output profiles */
    if (status == MB_SUCCESS) {
      /* allocate profile arrays */
      if (status == MB_SUCCESS && npointalloc < npoints) {
        status = mbview_allocprofilearrays(verbose, npoints, &prdistance, &prtopo, &prboundary, &prlon, &prlat,
                                           &prdistovertopo, &prbearing, &prslope, &error);
        if (status == MB_SUCCESS) {
          npointalloc = npoints;
        }

        /* if error initializing memory then cancel dealing with this profile */
        else {
          fprintf(stderr, "Unable to write profile...\nArray allocation for %d points failed for instance %zu!\n",
                  npoints, instance);
          XBell((Display *)XtDisplay(mainWindow), 100);
          npoints = 0;
        }
      }

      /* extract data for profile */
      status = mbview_getprofile(verbose, instance, prsourcename, &prlength, &przmin, &przmax, &npoints, prdistance, prtopo,
                                 prboundary, prlon, prlat, prdistovertopo, prbearing, prslope, &error);

      /* write the profile header */
      fprintf(sfp, "## PROFILESOURCE %s\n", prsourcename);
      fprintf(sfp, "## PROFILELENGTH %f\n", prlength);
      fprintf(sfp, "## PROFILEZMIN %f\n", przmin);
      fprintf(sfp, "## PROFILEZMAX %f\n", przmax);
      fprintf(sfp, "## PROFILEPOINTS %d\n", npoints);

      /* write the profile points */
      for (j = 0; j < npoints; j++) {
        fprintf(sfp, "%f %f %d %f %f %f %f %f\n", prdistance[j], prtopo[j], prboundary[j], prlon[j], prlat[j],
                prdistovertopo[j], prbearing[j], prslope[j]);
      }

      /* close the output file */
      fclose(sfp);

      /* deallocate arrays */
      if (npointalloc > 0) {
        status = mbview_freeprofilearrays(verbose, &prdistance, &prtopo, &prboundary, &prlon, &prlat, &prdistovertopo,
                                          &prbearing, &prslope, &error);
      }
    }
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/
int do_mbgrdviz_opennav(size_t instance, bool swathbounds, char *input_file_ptr) {
  int status = MB_SUCCESS;
  void *datalist;
  mb_path swathfile;
  int swathfilestatus;
  mb_path swathfileraw;
  mb_path swathfileprocessed;
  mb_path dfile;
  int astatus = MB_ALTNAV_USE;
  mb_path apath;
  int format;
  int formatorg;
  double weight;
  mb_path messagestr;
  char *lastslash;
  int nfiledatalist = 0;
  int nfileread = 0;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       swathbounds:     %d\n", swathbounds);
    fprintf(stderr, "dbg2       input_file_ptr:  %s\n", input_file_ptr);
  }

  /* InteractiveGMT port: a single swath (or .fnv navigation) file is read as itself. The original
     handed every pick to mb_datalist_open, which parses a binary swath file as datalist text: it read
     nothing, or crashed the process. MBIO names its format from the file name (-1 = a datalist). */
  if (instance != MBV_NO_WINDOW) {
    int sformat = 0;
    mb_get_format(verbose, input_file_ptr, NULL, &sformat, &error);
    if (sformat > 0 && sformat != MBF_ASCIIXYZ && sformat != MBF_ASCIIYXZ && sformat != MBF_ASCIIXYT &&
        sformat != MBF_ASCIIYXT) {
      strcpy(swathfile, input_file_ptr);
      format = sformat;
      if (!swathbounds)
        mb_get_fnv(verbose, swathfile, &format, &error);
      else
        mb_get_fbt(verbose, swathfile, &format, &error);
      lastslash = strrchr(swathfile, '/');
      const char *swathfile_base = (lastslash != NULL) ? &(lastslash[1]) : swathfile;
      snprintf(messagestr, sizeof(messagestr), "%s: %s", swathbounds ? "Reading swath data" : "Reading navigation",
               swathfile_base);
      do_mbview_message_on(messagestr, instance);
      do_mbgrdviz_readnav(instance, swathfile, MB_PROCESSED_NONE, swathfile, swathfile, format, sformat, 1.0, swathbounds,
                          &error);
      mbview_enableviewnavs(verbose, instance, &error);
      return mbview_update(verbose, instance, &error);
    }
  }

  /* InteractiveGMT port: on Windows the datalist is read from inside its folder, by its backslash
     path (see mbgv_abs_path): MBIO then leaves "C:\..." entries alone and relative ones resolve. */
#ifdef _WIN32
  char cwdSave[MB_PATH_MAXLINE] = "";
  const bool cwdOk = _getcwd(cwdSave, (int)sizeof(cwdSave)) != NULL;
  mb_path dlpath;
  snprintf(dlpath, sizeof(dlpath), "%s", input_file_ptr);
  for (char *c = dlpath; *c; c++)
    if (*c == '/')
      *c = '\\';
  char *dlslash = strrchr(dlpath, '\\');
  if (dlslash != NULL && cwdOk && instance != MBV_NO_WINDOW) {   /* restored after the read, below */
    *dlslash = '\0';
    _chdir(dlpath[0] != '\0' ? dlpath : "\\");
    *dlslash = '\\';
  }
  input_file_ptr = dlpath;
#endif

  /* read data for valid instance */
  if (instance != MBV_NO_WINDOW) {
    bool done = false;
    while (!done) {
      if ((status = mb_datalist_open(verbose, &datalist, input_file_ptr, MB_DATALIST_LOOK_UNSET, &error)) == MB_SUCCESS) {
        while (!done) {
          if ((status = mb_datalist_read3(verbose, datalist, &swathfilestatus, swathfileraw, swathfileprocessed, 
                                          &astatus, apath, dfile,
                                          &format, &weight, &error)) == MB_SUCCESS) {
            nfiledatalist++;
            if (format != MBF_ASCIIXYZ && format != MBF_ASCIIYXZ && format != MBF_ASCIIXYT &&
                format != MBF_ASCIIYXT) {
              /* check for available nav file if that is
                 all that is needed */
              if (swathfilestatus == MB_PROCESSED_USE)
                strcpy(swathfile, swathfileprocessed);
              else
                strcpy(swathfile, swathfileraw);
              formatorg = format;
              if (!swathbounds)
                mb_get_fnv(verbose, swathfile, &format, &error);

              /* else check for available fbt file  */
              else
                mb_get_fbt(verbose, swathfile, &format, &error);

              /* read the swath or nav data using mbio calls */

              /* update message */
              lastslash = strrchr(swathfile, '/');
              const char *swathfile_base = (lastslash != NULL) ? &(lastslash[1]) : swathfile;
              if (!swathbounds)
                snprintf(messagestr, sizeof(messagestr), "Reading navigation: %s", swathfile_base);
              else
                snprintf(messagestr, sizeof(messagestr), "Reading swath data: %s", swathfile_base);
              do_mbview_message_on(messagestr, instance);
              fprintf(stderr, "%s\n", messagestr);

              /* read the data */
              nfileread++;
              do_mbgrdviz_readnav(instance, swathfile, swathfilestatus, swathfileraw, swathfileprocessed, format,
                                  formatorg, weight, swathbounds, &error);
            }
            else
              fprintf(stderr, "Skipped xyz data: %s\n", swathfile);
          }
          else {
            mb_datalist_close(verbose, &datalist, &error);
            done = true;
          }
        }
      }
      else   /* InteractiveGMT port: the original looped here forever */
        done = true;
    }
    fprintf(stderr, "Attempted to load %d files, actually read %d files\n", nfiledatalist, nfileread);
#ifdef _WIN32
    if (cwdOk)
      _chdir(cwdSave);   /* back where the process was */
#endif

    /* update widgets */
    mbview_enableviewnavs(verbose, instance, &error);
    status = mbview_update(verbose, instance, &error);
  }

  return (status);
}
/*---------------------------------------------------------------------------------------*/

int do_mbgrdviz_readnav(size_t instance, char *swathfile, int pathstatus, char *pathraw, char *pathprocessed, int format,
                        int formatorg, double weight, bool wantbounds, int *error) {
  int status = MB_SUCCESS;
  char *error_message;

  /* MBIO control parameters */
  int pings = 1;
  int lonflip;
  double bounds[4];
  int btime_i[7];
  int etime_i[7];
  double btime_d;
  double etime_d;
  double speedmin;
  double timegap;
  int beams_bath;
  int beams_amp;
  int pixels_ss;
  void *mbio_ptr = NULL;

  /* mbio read and write values */
  void *store_ptr = NULL;
  int kind;
  int time_i[7];
  double time_d;
  double lon;
  double lat;
  double speed;
  double heading;
  double distance;
  double altitude;
  double sensordepth;
  char *beamflag = NULL;
  double *bath = NULL;
  double *bathacrosstrack = NULL;
  double *bathalongtrack = NULL;
  double *amp = NULL;
  double *ss = NULL;
  double *ssacrosstrack = NULL;
  double *ssalongtrack = NULL;
  char comment[MB_COMMENT_MAXLINE];

  int npoint;
  int npointread;
  int npointalloc;
  double *navtime_d = NULL;
  double *navlon = NULL;
  double *navlat = NULL;
  double *navz = NULL;
  double *navheading = NULL;
  double *navspeed = NULL;
  double *navportlon = NULL;
  double *navportlat = NULL;
  double *navstbdlon = NULL;
  double *navstbdlat = NULL;
  unsigned int *navline = NULL;
  unsigned int *navshot = NULL;
  unsigned int *navcdp = NULL;
  int color;
  int size;
  mb_path name;
  bool swathbounds;
  int line;
  int shot;
  int cdp;
  int decimation;

  struct mbview_struct *data;

  double mtodeglon, mtodeglat;
  double headingx, headingy;
  double xd, yd, zd;

  double cellsize;
  double distancealongtrack;

  int form;
  int icenter, iport, istbd;
  double centerdistance, portdistance, stbddistance;
  char *lastslash;
  int i;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:        %zu\n", instance);
    fprintf(stderr, "dbg2       swathfile:       %s\n", swathfile);
    fprintf(stderr, "dbg2       pathstatus:      %d\n", pathstatus);
    fprintf(stderr, "dbg2       pathraw:         %s\n", pathraw);
    fprintf(stderr, "dbg2       pathprocessed:   %s\n", pathprocessed);
    fprintf(stderr, "dbg2       format:          %d\n", format);
    fprintf(stderr, "dbg2       formatorg:       %d\n", formatorg);
    fprintf(stderr, "dbg2       weight:          %f\n", weight);
  }

  *error = MB_ERROR_NO_ERROR;

  /* initialize nav values */
  color = MBV_COLOR_BLACK;
  size = 2;
  name[0] = '\0';
  lastslash = strrchr(swathfile, '/');
  if ((lastslash = strrchr(swathfile, '/')) != NULL)
    strcpy(name, &(lastslash[1]));
  else
    strcpy(name, swathfile);

  swathbounds = false;
  line = false;
  shot = true;
  cdp = false;
  npoint = 0;
  npointread = 0;
  npointalloc = 0;
  distancealongtrack = 0.0;

  /* set mbio default values */
  status = mb_defaults(verbose, &form, &pings, &lonflip, bounds, btime_i, etime_i, &speedmin, &timegap);

  /* get data structure of current instance */
  status = mbview_getdataptr(verbose, instance, &data, error);
  if (status == MB_SUCCESS) {
    bounds[0] = data->primary_xmin;
    bounds[1] = data->primary_xmax;
    bounds[2] = data->primary_ymin;
    bounds[3] = data->primary_ymax;
    status = mbview_projectforward(instance, true, data->primary_xmin, data->primary_ymin,
                                   0.5 * (data->primary_min + data->primary_max), &bounds[0], &bounds[2], &xd, &yd, &zd);
    status = mbview_projectforward(instance, true, data->primary_xmax, data->primary_ymax,
                                   0.5 * (data->primary_min + data->primary_max), &bounds[1], &bounds[3], &xd, &yd, &zd);
    mb_coor_scale(verbose, 0.5 * (bounds[2] + bounds[3]), &mtodeglon, &mtodeglat);
    cellsize = 0.0005 * (((bounds[3] - bounds[2]) / ((double)data->primary_n_rows) / mtodeglat) +
                         ((bounds[1] - bounds[0]) / ((double)data->primary_n_columns) / mtodeglon));
  }

  /* rationalize bounds and lonflip */
  if (bounds[1] > 180.0) {
    lonflip = 1;
  }
  else if (bounds[0] < -180.0) {
    lonflip = -1;
  }
  else {
    lonflip = 0;
  }

  /* initialize reading the swath file */
  if ((status = mb_read_init(verbose, swathfile, format, pings, lonflip, bounds, btime_i, etime_i, speedmin, timegap, &mbio_ptr,
                             &btime_d, &etime_d, &beams_bath, &beams_amp, &pixels_ss, error)) != MB_SUCCESS) {
    mb_error(verbose, *error, &error_message);
    fprintf(stderr, "\nMBIO Error returned from function <mb_read_init>:\n%s\n", error_message);
    fprintf(stderr, "\nSwath sonar File <%s> not initialized for reading\n", swathfile);
  }
  /* allocate memory for data arrays.
     InteractiveGMT port: REGISTERED with MBIO, as mbedit/mbeditviz do, not mb_mallocd'ed at the
     sizes mb_read_init reports. Those are only the opening sizes: MBIO grows the beam count while it
     reads (an EM122 .mb59 does), and mb_get_all then wrote past fixed arrays -- the process died on
     the first raw swath file read (measured). Registered arrays grow with it; mb_close frees them. */
  if (status == MB_SUCCESS) {
    status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(char), (void **)&beamflag, error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bath, error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathacrosstrack,
                                 error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathalongtrack,
                                 error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_AMPLITUDE, sizeof(double), (void **)&amp, error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ss, error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ssacrosstrack, error);
    if (status == MB_SUCCESS)
      status = mb_register_array(verbose, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ssalongtrack, error);

    /* if error initializing memory then don't read the file */
    if (*error != MB_ERROR_NO_ERROR) {
      mb_error(verbose, *error, &error_message);
      fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", error_message);
    }
  }

  /* read data */
  if (status == MB_SUCCESS) {
    /* set swathbounds true if nore than one beam is expected */
    if (beams_bath > 1)
      swathbounds = true;

    /* enable line and cdp values if segy data */
    if (format == MBF_SEGYSEGY) {
      line = true;
      cdp = true;
    }

    /* loop over successful reads and nonfatal errors
       until a fatal error is encountered */
    while (*error <= MB_ERROR_NO_ERROR) {
      /* read a ping of data */
      status = mb_get_all(verbose, mbio_ptr, &store_ptr, &kind, time_i, &time_d, &lon, &lat, &speed, &heading, &distance,
                          &altitude, &sensordepth, &beams_bath, &beams_amp, &pixels_ss, beamflag, bath, amp, bathacrosstrack,
                          bathalongtrack, ss, ssacrosstrack, ssalongtrack, comment, error);

      /* ignore minor errors */
      if (kind == MB_DATA_DATA &&
          (*error == MB_ERROR_TIME_GAP || *error == MB_ERROR_OUT_TIME || *error == MB_ERROR_SPEED_TOO_SMALL)) {
        status = MB_SUCCESS;
        *error = MB_ERROR_NO_ERROR;
      }

      if (kind == MB_DATA_DATA && *error == MB_ERROR_NO_ERROR) {
        /*fprintf(stderr,"Ping %d: %4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d.%.6.6d %f %f\n",
        npoint,time_i[0],time_i[1],time_i[2],time_i[3],time_i[4],time_i[5],time_i[6],lon,lat);*/
        /* overwrite previous nav point if distance change does not
            exceed cell size */
        if (npoint == 0) {
          distancealongtrack = 0.0;
        }
        else if (distancealongtrack < cellsize) {
          npoint--;
          distancealongtrack += distance;
        }
        else {
          distancealongtrack = 0.0;
        }

        /* allocate memory if required */
        if (npoint >= npointalloc) {
          npointalloc += MBV_ALLOC_NUM;
          status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navtime_d, error);
          if (status == MB_SUCCESS)
            status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navlon, error);
          if (status == MB_SUCCESS)
            status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navlat, error);
          if (status == MB_SUCCESS)
            status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navz, error);
          if (status == MB_SUCCESS)
            status =
                mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navheading, error);
          if (status == MB_SUCCESS)
            status =
                mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navspeed, error);
          if (status == MB_SUCCESS)
            status =
                mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navportlon, error);
          if (status == MB_SUCCESS)
            status =
                mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navportlat, error);
          if (status == MB_SUCCESS)
            status =
                mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navstbdlon, error);
          if (status == MB_SUCCESS)
            status =
                mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(double), (void **)&navstbdlat, error);
          if (status == MB_SUCCESS)
            status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(int), (void **)&navline, error);
          if (status == MB_SUCCESS)
            status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(int), (void **)&navshot, error);
          if (status == MB_SUCCESS)
            status = mb_reallocd(verbose, __FILE__, __LINE__, npointalloc * sizeof(int), (void **)&navcdp, error);

          /* if error initializing memory then don't read the file */
          if (*error != MB_ERROR_NO_ERROR) {
            npointalloc = 0;
            mb_error(verbose, *error, &error_message);
            fprintf(stderr, "\nMBIO Error allocating navigation data arrays:\n%s\n", error_message);
          }
        }

        /* only use the nav arrays below if they are actually allocated -
            if the reallocation above failed, status != MB_SUCCESS and
            navtime_d/navlon/navlat/etc may be NULL */
        if (status == MB_SUCCESS) {
          /* get swathbounds */
          if (format == MBF_MBPRONAV) {
            status = mbsys_singlebeam_swathbounds(verbose, mbio_ptr, store_ptr, &kind, &navportlon[npoint],
                                                  &navportlat[npoint], &navstbdlon[npoint], &navstbdlat[npoint], error);
            if (navportlon[npoint] != navstbdlon[npoint] || navportlat[npoint] != navstbdlat[npoint])
              swathbounds = true;
          }

          else {
            /* find centermost beam */
            icenter = -1;
            iport = -1;
            istbd = -1;
            centerdistance = 0.0;
            portdistance = 0.0;
            stbddistance = 0.0;
            for (i = 0; i < beams_bath; i++) {
              if (mb_beam_ok(beamflag[i])) {
                if (icenter == -1 || fabs(bathacrosstrack[i]) < centerdistance) {
                  icenter = i;
                  centerdistance = bathacrosstrack[i];
                }
                if (iport == -1 || bathacrosstrack[i] < portdistance) {
                  iport = i;
                  portdistance = bathacrosstrack[i];
                }
                if (istbd == -1 || bathacrosstrack[i] > stbddistance) {
                  istbd = i;
                  stbddistance = bathacrosstrack[i];
                }
              }
            }

            mb_coor_scale(verbose, lat, &mtodeglon, &mtodeglat);
            headingx = sin(heading * DTR);
            headingy = cos(heading * DTR);
            if (icenter >= 0) {
              navportlon[npoint] =
                  lon + headingy * mtodeglon * bathacrosstrack[iport] + headingx * mtodeglon * bathalongtrack[iport];
              navportlat[npoint] =
                  lat - headingx * mtodeglat * bathacrosstrack[iport] + headingy * mtodeglat * bathalongtrack[iport];
              navstbdlon[npoint] =
                  lon + headingy * mtodeglon * bathacrosstrack[istbd] + headingx * mtodeglon * bathalongtrack[istbd];
              navstbdlat[npoint] =
                  lat - headingx * mtodeglat * bathacrosstrack[istbd] + headingy * mtodeglat * bathalongtrack[istbd];
            }
            else {
              navportlon[npoint] = lon;
              navportlat[npoint] = lat;
              navstbdlon[npoint] = lon;
              navstbdlat[npoint] = lat;
            }
          }

          /* store the navigation values */
          navtime_d[npoint] = time_d;
          navlon[npoint] = lon;
          navlat[npoint] = lat;
          navz[npoint] = -sensordepth;
          navheading[npoint] = heading;
          navspeed[npoint] = speed;

          mb_segynumber(verbose, mbio_ptr, &(navline[npoint]), &(navshot[npoint]), &(navcdp[npoint]), error);

          /* increment npoint */
          npoint++;
          npointread++;
        }
      }
    }

    /* close the swath file */
    status = mb_close(verbose, &mbio_ptr, error);

    /* insert nav data to mbview */
    if (npoint > 0) {
      decimation = npointread / npoint;
      /* InteractiveGMT port: swath bounds only when Open Swath Data asked for them. The data alone
         (beams_bath > 1, or .fnv port/stbd columns) sets swathbounds, so Open Navigation drew them too. */
      status = mbview_addnav(verbose, instance, npoint, navtime_d, navlon, navlat, navz, navheading, navspeed, navportlon,
                             navportlat, navstbdlon, navstbdlat, navline, navshot, navcdp, color, size, name, pathstatus,
                             pathraw, pathprocessed, formatorg, wantbounds && swathbounds, line, shot, cdp, decimation, error);
    }
    else
      fprintf(stderr, "    Skipping %s because of 0 nav points read\n", name);

    /* deallocate memory used for data arrays (the beam/pixel arrays went with mb_close: registered) */
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navtime_d, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navlon, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navlat, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navz, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navheading, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navspeed, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navportlon, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navportlat, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navstbdlon, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navstbdlat, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navline, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navshot, error);
    mb_freed(verbose, __FILE__, __LINE__, (void **)&navcdp, error);
  }

  /* all done */
  return (status);
}
/*---------------------------------------------------------------------------------------*/

void do_mbgrdviz_generate_survey(size_t instance) {
  int status = MB_SUCCESS;

  /* mbview instance */
  struct mbview_struct *data;

  /* survey construction parameters */
  int color;
  double line_spacing;
  double line_spacing_use;
  double crossline_spacing;
  double sonar_depth;
  double sonar_altitude;
  double maxtopo;
  struct mbview_linesegment_struct segment;
  int nlines;
  int nlinegroups, npoints;
  double xgrid, ygrid;
  double xlon, ylat, zdata;
  double xdisplay, ydisplay, zdisplay;
  double dsign;
  int waypoint;
  bool first;
  double dsigna[4] = {1.0, -1.0, 1.0, -1.0};
  int jendpointa[4] = {0, 0, 1, 1};

  char *error_message;
  double *xx = NULL;
  double dx, dy, r, dxuse, dyuse, dxd, dyd, dxextra, dyextra;
  double rrr[4], xxx, yyy;
  int iline, jendpoint;
  bool ok;
  int startcorner, endcorner, jstart, kend;
  int nlines_alloc = 0;

  /* get source mbview instance (InteractiveGMT port: passed in; it was survey_instance) */
  survey_instance = (int)instance;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       instance:    %zu\n", instance);
  }

  /* check data source for area to bounding desired survey */
  status = mbview_getdataptr(verbose, instance, &data, &error);

  /* check if area is currently defined */
  if (status == MB_SUCCESS) {
    if (data->area_type != MBV_AREA_QUAD)
      status = MB_FAILURE;
  }

  /* generate survey lines from area and add as new route */
  if (status == MB_SUCCESS) {
    /* delete current working route if defined - but first confirm that
       working_route still refers to the survey route we created. Its
       index can go stale if the user deleted some other route via
       3D-view picking while this dialog stayed open, which shifts route
       array indices and would otherwise make working_route refer to an
       unrelated route */
    if (working_route > -1) {
      int wr_nroutewaypoint, wr_nroutpoint, wr_routecolor, wr_routesize;
      double wr_routedistancelateral, wr_routedistancetopo;
      char wr_routename[MB_PATH_MAXLINE];
      int wr_status = mbview_getrouteinfo(verbose, instance, working_route, &wr_nroutewaypoint, &wr_nroutpoint,
                                          wr_routename, &wr_routecolor, &wr_routesize, &wr_routedistancelateral,
                                          &wr_routedistancetopo, &error);
      if (wr_status == MB_SUCCESS && strcmp(wr_routename, survey_name) == 0) {
        mbview_deleteroute(verbose, instance, working_route, &error);
      }
      working_route = -1;
    }

    /* get unit vector for survey area boundaries */
    dx = data->area.cornerpoints[1].xdisplay - data->area.cornerpoints[0].xdisplay;
    dy = data->area.cornerpoints[1].ydisplay - data->area.cornerpoints[0].ydisplay;
    r = sqrt(dx * dx + dy * dy);
    dx = dx / r;
    dy = dy / r;

    /* get parameters */
    int k = 0;
    if (data->area.bearing >= 315.0 || data->area.bearing < 45.0) {
      if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SW)
        k = 0;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SE)
        k = 1;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NW)
        k = 2;
      else /* if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NE) */
        k = 3;
    }
    else if (data->area.bearing >= 45.0 && data->area.bearing < 135.0) {
      if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SW)
        k = 1;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SE)
        k = 3;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NW)
        k = 0;
      else /* if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NE) */
        k = 2;
    }
    else if (data->area.bearing >= 135.0 && data->area.bearing < 225.0) {
      if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SW)
        k = 3;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SE)
        k = 2;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NW)
        k = 1;
      else /* if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NE) */
        k = 0;
    }
    else /* if (data->area.bearing >= 225.0 && data->area.bearing < 315.0) */
    {
      if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SW)
        k = 2;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_SE)
        k = 0;
      else if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NW)
        k = 3;
      else /* if (survey_direction == MBGRDVIZ_SURVEY_DIRECTION_NE) */
        k = 1;
    }
    dsign = dsigna[k];
    jendpoint = jendpointa[k];
    if (survey_color == 0)
      color = MBV_COLOR_BLACK;
    else if (survey_color == 1)
      color = MBV_COLOR_YELLOW;
    else if (survey_color == 2)
      color = MBV_COLOR_GREEN;
    else if (survey_color == 3)
      color = MBV_COLOR_BLUEGREEN;
    else if (survey_color == 4)
      color = MBV_COLOR_BLUE;
    else if (survey_color == 5)
      color = MBV_COLOR_PURPLE;

    /* initialize number of waypoints */
    npoints = 0;
    first = true;

    /* do uniform line spacing */
    if (survey_mode == MBGRDVIZ_SURVEY_MODE_UNIFORM) {
      /* get number of lines */
      line_spacing = (double)survey_linespacing;
      line_spacing_use = line_spacing * r / data->area.width;
      nlines = (data->area.width / line_spacing) + 1;

      /* allocate space for line position array */
      status = mb_mallocd(verbose, __FILE__, __LINE__, nlines * sizeof(double), (void **)&xx, &error);
      if (status != MB_SUCCESS) {
        nlines_alloc = 0;
        mb_error(verbose, error, &error_message);
        fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", error_message);
      }
      else
        nlines_alloc = nlines;

      /* calculate line positions */
      if (status == MB_SUCCESS)
        for (int i = 0; i < nlines; i++) {
          /* get line position in survey area */
          xx[i] = dsign * line_spacing_use * (((double)i) - 0.5 * (nlines - 1.0));
        }
    }

    /* do variable line spacing with constant altitude */
    else if (survey_mode == MBGRDVIZ_SURVEY_MODE_VARIABLE && survey_platform == MBGRDVIZ_SURVEY_PLATFORM_SUBMERGED_ALTITUDE) {
      /* get number of lines */
      line_spacing = (double)survey_altitude * 2.0 * tan(DTR * 0.5 * (double)survey_swathwidth);
      line_spacing_use = line_spacing * r / data->area.width;
      nlines = (data->area.width / line_spacing) + 1;

      /* allocate space for line position array */
      status = mb_mallocd(verbose, __FILE__, __LINE__, nlines * sizeof(double), (void **)&xx, &error);
      if (status != MB_SUCCESS) {
        nlines_alloc = 0;
        mb_error(verbose, error, &error_message);
        fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", error_message);
      }
      else
        nlines_alloc = nlines;

      /* calculate line positions */
      if (status == MB_SUCCESS)
        for (int i = 0; i < nlines; i++) {
          /* get line position in survey area */
          xx[i] = dsign * line_spacing_use * (((double)i) - 0.5 * (nlines - 1.0));
        }
    }

    /* do variable line spacing with variable altitude */
    else if (survey_mode == MBGRDVIZ_SURVEY_MODE_VARIABLE) {
      /* get platform depth */
      if (survey_platform == MBGRDVIZ_SURVEY_PLATFORM_SUBMERGED_DEPTH) {
        sonar_depth = (double)survey_depth;
      }
      else {
        sonar_depth = 0.0;
      }

      /* allocate space for line position array */
      nlines_alloc += 100;
      status = mb_mallocd(verbose, __FILE__, __LINE__, nlines_alloc * sizeof(double), (void **)&xx, &error);
      if (status != MB_SUCCESS) {
        nlines_alloc = 0;
        mb_error(verbose, error, &error_message);
        fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", error_message);
      }

      /* start on the port side of the survey */
      /* find range of altitude along each line and calculate the swath width
          from the smallest altitude */
      nlines = 1;
      segment.nls = 0;
      segment.nls_alloc = 0;
      segment.lspoints = NULL;

      if (status == MB_SUCCESS) {
        xx[0] = -dsign * 0.5 * r;

        while (nlines == 1 || fabs(xx[nlines - 1] + dsign * 0.5 * line_spacing_use) < 0.5 * r) {
          /* allocate more space for xx if needed */
          if (nlines_alloc <= nlines) {
            nlines_alloc += 100;
            status = mb_reallocd(verbose, __FILE__, __LINE__, nlines_alloc * sizeof(double), (void **)&xx, &error);
            if (status != MB_SUCCESS) {
              nlines_alloc = 0;
              mb_error(verbose, error, &error_message);
              fprintf(stderr, "\nMBIO Error allocating data arrays:\n%s\n", error_message);
              break;
            }
          }
          /* get offset from last xx */
          dxuse = dx * xx[nlines - 1];
          dyuse = dy * xx[nlines - 1];

          /* get first point */
          segment.endpoints[0].xdisplay = data->area.endpoints[0].xdisplay + dxuse;
          segment.endpoints[0].ydisplay = data->area.endpoints[0].ydisplay + dyuse;
          segment.endpoints[0].zdisplay = data->area.endpoints[0].zdisplay;
          mbview_projectinverse(instance, true, segment.endpoints[0].xdisplay, segment.endpoints[0].ydisplay,
                                segment.endpoints[0].zdisplay, &segment.endpoints[0].xlon, &segment.endpoints[0].ylat,
                                &segment.endpoints[0].xgrid, &segment.endpoints[0].ygrid);
          mbview_getzdata(instance, segment.endpoints[0].xgrid, segment.endpoints[0].ygrid, &ok,
                          &segment.endpoints[0].zdata);

          /* get second point */
          segment.endpoints[1].xdisplay = data->area.endpoints[1].xdisplay + dxuse;
          segment.endpoints[1].ydisplay = data->area.endpoints[1].ydisplay + dyuse;
          segment.endpoints[1].zdisplay = data->area.endpoints[1].zdisplay;
          mbview_projectinverse(instance, true, segment.endpoints[1].xdisplay, segment.endpoints[1].ydisplay,
                                segment.endpoints[1].zdisplay, &segment.endpoints[1].xlon, &segment.endpoints[1].ylat,
                                &segment.endpoints[1].xgrid, &segment.endpoints[1].ygrid);
          mbview_getzdata(instance, segment.endpoints[1].xgrid, segment.endpoints[1].ygrid, &ok,
                          &segment.endpoints[1].zdata);

          /* drape line and get max topo */
          mbview_drapesegment(instance, &(segment));
          maxtopo = -9999999.9;
          if (segment.endpoints[0].zdata < -sonar_depth) {
            maxtopo = segment.endpoints[0].zdata;
          }
          if (segment.endpoints[1].zdata < -sonar_depth && segment.endpoints[1].zdata > maxtopo) {
            maxtopo = segment.endpoints[1].zdata;
          }
          for (int i = 0; i < segment.nls; i++) {
            if (segment.lspoints[i].zdata < -sonar_depth)
              maxtopo = MAX(maxtopo, segment.lspoints[i].zdata);
          }

          /* figure minimum swath width and location of next line */
          sonar_altitude = -maxtopo - sonar_depth;
          line_spacing = sonar_altitude * 2.0 * tan(DTR * 0.5 * (double)survey_swathwidth);
          line_spacing_use = line_spacing * r / data->area.width;
          xx[nlines] = xx[nlines - 1] + dsign * line_spacing_use;
          nlines++;
        }
      }

      /* deallocate segment points */
      if (segment.lspoints != NULL) {
        mb_freed(verbose, __FILE__, __LINE__, (void **)&(segment.lspoints), &error);
        segment.nls_alloc = 0;
      }
    }

    /* do crosslines if requested */
    if (survey_crosslines > 0 && !survey_crosslines_last && status == MB_SUCCESS) {
      /* figure out which corner the main lines start at */
      dxuse = dx * xx[0];
      dyuse = dy * xx[0];
      dxextra = 0.0;
      dyextra = 0.0;
      xdisplay = data->area.endpoints[jendpoint].xdisplay + dxuse + dxextra;
      ydisplay = data->area.endpoints[jendpoint].ydisplay + dyuse + dyextra;
      for (int i = 0; i < 4; i++) {
        xxx = xdisplay - data->area.cornerpoints[i].xdisplay;
        yyy = ydisplay - data->area.cornerpoints[i].ydisplay;
        rrr[i] = sqrt(xxx * xxx + yyy * yyy);
      }
      startcorner = 0;
      for (int i = 1; i < 4; i++) {
        if (rrr[i] < rrr[startcorner])
          startcorner = i;
      }

      /* figure out which corner the cross lines should start at */
      if (survey_crosslines % 2 == 0) {
        if (startcorner == 0)
          startcorner = 3;
        else if (startcorner == 1)
          startcorner = 2;
        else if (startcorner == 2)
          startcorner = 1;
        else if (startcorner == 3)
          startcorner = 0;
      }
      else {
        if (startcorner == 0)
          startcorner = 2;
        else if (startcorner == 1)
          startcorner = 3;
        else if (startcorner == 2)
          startcorner = 0;
        else if (startcorner == 3)
          startcorner = 1;
      }

      /* get crossline vector */
      if (startcorner == 0 || startcorner == 3) {
        dx = data->area.cornerpoints[1].xdisplay - data->area.cornerpoints[0].xdisplay;
        dy = data->area.cornerpoints[1].ydisplay - data->area.cornerpoints[0].ydisplay;
      }
      else {
        dx = data->area.cornerpoints[0].xdisplay - data->area.cornerpoints[1].xdisplay;
        dy = data->area.cornerpoints[0].ydisplay - data->area.cornerpoints[1].ydisplay;
      }
      r = sqrt(dx * dx + dy * dy);
      dxd = dx / r;
      dyd = dy / r;

      /* get crossline spacing */
      crossline_spacing = (data->area.length / (survey_crosslines + 1)) * (r / data->area.width);

      /* generate cross lines */
      jstart = startcorner;
      if (startcorner == 0 || startcorner == 2)
        kend = jstart + 1;
      else
        kend = jstart - 1;
      dx = (data->area.endpoints[1].xdisplay - data->area.endpoints[0].xdisplay) / (survey_crosslines + 1);
      dy = (data->area.endpoints[1].ydisplay - data->area.endpoints[0].ydisplay) / (survey_crosslines + 1);
      if (startcorner >= 2) {
        dx = -dx;
        dy = -dy;
      }
      int j = jstart;
      for (int i = 0; i < survey_crosslines; i++) {
        /* get offset from corners */
        dxuse = (i + 1) * dx;
        dyuse = (i + 1) * dy;
        if (j == jstart) {
          dxextra = -dxd * line_spacing_use;
          dyextra = -dyd * line_spacing_use;
        }
        else {
          dxextra = dxd * line_spacing_use;
          dyextra = dyd * line_spacing_use;
        }

        /* get first point */
        waypoint = MBV_ROUTE_WAYPOINT_STARTLINE;
        xdisplay = data->area.cornerpoints[j].xdisplay + dxuse + dxextra;
        ydisplay = data->area.cornerpoints[j].ydisplay + dyuse + dyextra;
        zdisplay = data->area.cornerpoints[j].zdisplay;
        mbview_projectinverse(instance, true, xdisplay, ydisplay, zdisplay, &xlon, &ylat, &xgrid, &ygrid);
        mbview_getzdata(instance, xgrid, ygrid, &ok, &zdata);
        if (!ok)
          zdata = data->area.cornerpoints[jendpoint].zdata;
        mbview_projectll2display(instance, xlon, ylat, zdata, &xdisplay, &ydisplay, &zdisplay);
        if (first) {
          mbview_addroute(verbose, instance, 1, &xlon, &ylat, &waypoint, color, 2, true, survey_name, &working_route,
                          &error);
          first = false;
        }
        else {
          mbview_route_add(verbose, instance, working_route, npoints, waypoint, xgrid, ygrid, xlon, ylat, zdata,
                           xdisplay, ydisplay, zdisplay);
        }
        npoints++;

        /* get second point */
        if (j == jstart)
          j = kend;
        else
          j = jstart;
        if (j == jstart) {
          dxextra = -dxd * line_spacing_use;
          dyextra = -dyd * line_spacing_use;
        }
        else {
          dxextra = dxd * line_spacing_use;
          dyextra = dyd * line_spacing_use;
        }

        /* get second point */
        waypoint = MBV_ROUTE_WAYPOINT_STARTLINE;
        xdisplay = data->area.cornerpoints[j].xdisplay + dxuse + dxextra;
        ydisplay = data->area.cornerpoints[j].ydisplay + dyuse + dyextra;
        zdisplay = data->area.cornerpoints[j].zdisplay;
        mbview_projectinverse(instance, true, xdisplay, ydisplay, zdisplay, &xlon, &ylat, &xgrid, &ygrid);
        mbview_getzdata(instance, xgrid, ygrid, &ok, &zdata);
        if (!ok)
          zdata = data->area.cornerpoints[jendpoint].zdata;
        mbview_projectll2display(instance, xlon, ylat, zdata, &xdisplay, &ydisplay, &zdisplay);
        mbview_route_add(verbose, instance, working_route, npoints, waypoint, xgrid, ygrid, xlon, ylat, zdata, xdisplay,
                         ydisplay, zdisplay);
        npoints++;
      }
    }

    /* generate the lines */
    if (nlines > 0 && status == MB_SUCCESS) {
      /* get unit vector for survey area boundaries */
      dx = data->area.cornerpoints[1].xdisplay - data->area.cornerpoints[0].xdisplay;
      dy = data->area.cornerpoints[1].ydisplay - data->area.cornerpoints[0].ydisplay;
      r = sqrt(dx * dx + dy * dy);
      dx = dx / r;
      dy = dy / r;

      /* generate points */
      /* work in display coordinates */
      nlinegroups = nlines / survey_interleaving + 1;
      for (int j = 0; j < survey_interleaving; j++)
        for (int i = 0; i < nlinegroups; i++) {
          /* get line number */
          iline = i * survey_interleaving + j;

          if (iline < nlines) {
            /* get line position in survey area */
            dxuse = dx * xx[iline];
            dyuse = dy * xx[iline];

            /* add a bit of transit before later interleaved lines */
            if (jendpoint == 1) {
              dxextra = -dy * j * 0.25 * line_spacing_use;
              dyextra = dx * j * 0.25 * line_spacing_use;
            }
            else {
              dxextra = dy * j * 0.25 * line_spacing_use;
              dyextra = -dx * j * 0.25 * line_spacing_use;
            }

            /* get first point */
            waypoint = MBV_ROUTE_WAYPOINT_STARTLINE;
            xdisplay = data->area.endpoints[jendpoint].xdisplay + dxuse + dxextra;
            ydisplay = data->area.endpoints[jendpoint].ydisplay + dyuse + dyextra;
            zdisplay = data->area.endpoints[jendpoint].zdisplay;
            mbview_projectinverse(instance, true, xdisplay, ydisplay, zdisplay, &xlon, &ylat, &xgrid, &ygrid);
            mbview_getzdata(instance, xgrid, ygrid, &ok, &zdata);
            if (!ok)
              zdata = data->area.endpoints[jendpoint].zdata;
            mbview_projectll2display(instance, xlon, ylat, zdata, &xdisplay, &ydisplay, &zdisplay);
            fprintf(stderr, "\nSurvey Line:%d Point:%d  Position: %f %f %f  %f %f   %f %f %f\n", iline, jendpoint,
                    xlon, ylat, zdata, xgrid, ygrid, xdisplay, ydisplay, zdisplay);

            /* add new route for first point, just add single point
                after that */
            if (first) {
              mbview_addroute(verbose, instance, 1, &xlon, &ylat, &waypoint, color, 2, true, survey_name,
                              &working_route, &error);
              first = false;
            }
            else {
              mbview_route_add(verbose, instance, working_route, npoints, waypoint, xgrid, ygrid, xlon, ylat, zdata,
                               xdisplay, ydisplay, zdisplay);
            }
            npoints++;

            /* switch endpoint */
            jendpoint = (jendpoint + 1) % 2;

            /* add a bit of transit before interleaved lines */
            if (jendpoint == 1) {
              dxextra = -dy * j * 0.25 * line_spacing_use;
              dyextra = dx * j * 0.25 * line_spacing_use;
            }
            else {
              dxextra = dy * j * 0.25 * line_spacing_use;
              dyextra = -dx * j * 0.25 * line_spacing_use;
            }

            /* get second point */
            waypoint = MBV_ROUTE_WAYPOINT_STARTLINE;
            xdisplay = data->area.endpoints[jendpoint].xdisplay + dxuse + dxextra;
            ydisplay = data->area.endpoints[jendpoint].ydisplay + dyuse + dyextra;
            zdisplay = data->area.endpoints[jendpoint].zdisplay;
            mbview_projectinverse(instance, true, xdisplay, ydisplay, zdisplay, &xlon, &ylat, &xgrid, &ygrid);
            mbview_getzdata(instance, xgrid, ygrid, &ok, &zdata);
            if (!ok)
              zdata = data->area.endpoints[jendpoint].zdata;
            mbview_projectll2display(instance, xlon, ylat, zdata, &xdisplay, &ydisplay, &zdisplay);
            fprintf(stderr, "Survey Line:%d Point:%d  Position: %f %f %f  %f %f   %f %f %f\n", iline, jendpoint, xlon,
                    ylat, zdata, xgrid, ygrid, xdisplay, ydisplay, zdisplay);

            /* add single point */
            mbview_route_add(verbose, instance, working_route, npoints, waypoint, xgrid, ygrid, xlon, ylat, zdata,
                             xdisplay, ydisplay, zdisplay);
            npoints++;
          }
        }

      /* deallocate line position array */
      mb_freed(verbose, __FILE__, __LINE__, (void **)&xx, &error);
    }

    /* do crosslines if requested */
    if (survey_crosslines > 0 && survey_crosslines_last && status == MB_SUCCESS) {
      /* figure out which corner the mail lines ended at */
      for (int i = 0; i < 4; i++) {
        xxx = xdisplay - data->area.cornerpoints[i].xdisplay;
        yyy = ydisplay - data->area.cornerpoints[i].ydisplay;
        rrr[i] = sqrt(xxx * xxx + yyy * yyy);
      }
      endcorner = 0;
      for (int i = 1; i < 4; i++) {
        if (rrr[i] < rrr[endcorner])
          endcorner = i;
      }

      /* get crossline vector */
      if (endcorner == 0 || endcorner == 3) {
        dx = data->area.cornerpoints[1].xdisplay - data->area.cornerpoints[0].xdisplay;
        dy = data->area.cornerpoints[1].ydisplay - data->area.cornerpoints[0].ydisplay;
      }
      else {
        dx = data->area.cornerpoints[0].xdisplay - data->area.cornerpoints[1].xdisplay;
        dy = data->area.cornerpoints[0].ydisplay - data->area.cornerpoints[1].ydisplay;
      }
      r = sqrt(dx * dx + dy * dy);
      dxd = dx / r;
      dyd = dy / r;

      /* get crossline spacing */
      crossline_spacing = (data->area.length / (crossline_spacing + 1)) * (r / data->area.width);

      /* generate cross lines */
      jstart = endcorner;
      if (endcorner == 0 || endcorner == 2)
        kend = jstart + 1;
      else
        kend = jstart - 1;
      dx = (data->area.endpoints[1].xdisplay - data->area.endpoints[0].xdisplay) / (survey_crosslines + 1);
      dy = (data->area.endpoints[1].ydisplay - data->area.endpoints[0].ydisplay) / (survey_crosslines + 1);
      if (endcorner >= 2) {
        dx = -dx;
        dy = -dy;
      }
      int j = jstart;
      for (int i = 0; i < survey_crosslines; i++) {
        /* get offset from corners */
        dxuse = (i + 1) * dx;
        dyuse = (i + 1) * dy;
        if (j == jstart) {
          dxextra = -dxd * line_spacing_use;
          dyextra = -dyd * line_spacing_use;
        }
        else {
          dxextra = dxd * line_spacing_use;
          dyextra = dyd * line_spacing_use;
        }

        /* get first point */
        waypoint = MBV_ROUTE_WAYPOINT_STARTLINE;
        xdisplay = data->area.cornerpoints[j].xdisplay + dxuse + dxextra;
        ydisplay = data->area.cornerpoints[j].ydisplay + dyuse + dyextra;
        zdisplay = data->area.cornerpoints[j].zdisplay;
        mbview_projectinverse(instance, true, xdisplay, ydisplay, zdisplay, &xlon, &ylat, &xgrid, &ygrid);
        mbview_getzdata(instance, xgrid, ygrid, &ok, &zdata);
        if (!ok)
          zdata = data->area.cornerpoints[jendpoint].zdata;
        mbview_projectll2display(instance, xlon, ylat, zdata, &xdisplay, &ydisplay, &zdisplay);
        mbview_route_add(verbose, instance, working_route, npoints, waypoint, xgrid, ygrid, xlon, ylat, zdata, xdisplay,
                         ydisplay, zdisplay);
        npoints++;

        /* get second point */
        if (j == jstart)
          j = kend;
        else
          j = jstart;
        if (j == jstart) {
          dxextra = -dxd * line_spacing_use;
          dyextra = -dyd * line_spacing_use;
        }
        else {
          dxextra = dxd * line_spacing_use;
          dyextra = dyd * line_spacing_use;
        }

        /* get second point */
        waypoint = MBV_ROUTE_WAYPOINT_STARTLINE;
        xdisplay = data->area.cornerpoints[j].xdisplay + dxuse + dxextra;
        ydisplay = data->area.cornerpoints[j].ydisplay + dyuse + dyextra;
        zdisplay = data->area.cornerpoints[j].zdisplay;
        mbview_projectinverse(instance, true, xdisplay, ydisplay, zdisplay, &xlon, &ylat, &xgrid, &ygrid);
        mbview_getzdata(instance, xgrid, ygrid, &ok, &zdata);
        if (!ok)
          zdata = data->area.cornerpoints[jendpoint].zdata;
        mbview_projectll2display(instance, xlon, ylat, zdata, &xdisplay, &ydisplay, &zdisplay);
        mbview_route_add(verbose, instance, working_route, npoints, waypoint, xgrid, ygrid, xlon, ylat, zdata, xdisplay,
                         ydisplay, zdisplay);
        npoints++;
      }
    }

    /* free the memory for xx */
    status = mb_freed(verbose, __FILE__, __LINE__, (void **)&xx, &error);

    /* update widgets */
    mbview_updateroutelist();
    do_mbgrdviz_arearoute_info(instance);
    mbview_enableviewnavs(verbose, instance, &error);
    status = mbview_update(verbose, instance, &error);
  }

  /* update widgets of remaining mbview windows */
  for (unsigned int i = 0; i < MBV_MAX_WINDOWS; i++) {
    if (i != instance && mbview_id[i])
      status = mbview_update(verbose, i, &error);
  }
}

/*======================================================================================
 * END OF VERBATIM
 *======================================================================================*/

/*======================================================================================
 * The port's own (see the head of this file): what mbgrdviz's Motif callbacks did with widgets,
 * as the plain functions of mbgrdviz.h.
 *======================================================================================*/

int mbgrdviz_proj_init(const char *projection, void **pj) {
	int err = MB_ERROR_NO_ERROR;
	if (!mbgv_mbio_ok || projection == NULL || pj == NULL)
		return 0;
	mb_path id;
	snprintf(id, sizeof(id), "%s", projection);
	return mb_proj_init(0, id, pj, &err) == MB_SUCCESS && *pj != NULL ? 1 : 0;
}

void mbgrdviz_proj_free(void **pj) {
	int err = MB_ERROR_NO_ERROR;
	if (mbgv_mbio_ok && pj != NULL && *pj != NULL)
		mb_proj_free(0, pj, &err);
}

int mbgrdviz_proj_forward(void *pj, double lon, double lat, double *easting, double *northing) {
	int err = MB_ERROR_NO_ERROR;
	if (!mbgv_mbio_ok || pj == NULL)
		return 0;
	return mb_proj_forward(0, pj, lon, lat, easting, northing, &err) == MB_SUCCESS ? 1 : 0;
}

void mbgrdviz_set_message_hook(void (*hook)(const char *message)) {
	mbgv_message_hook = hook;
}

static bool mbgrdviz_view_ok(int view) {
	return view >= 0 && view < MBV_MAX_WINDOWS && mbviews[view].init != MBV_WINDOW_NULL;
}

/* libmbview's shared sites, routes, navigation and vectors carry grid and display coordinates per
   view (struct mbview_pointw_struct); a view made after they were added has none yet. mbview fills
   them when it projects a new instance; this is mbview_updatepointw's body for that one view, over
   every shared point. */
static void mbgrdviz_pointw_view(size_t v, struct mbview_pointw_struct *p) {
	mbview_projectll2xygrid(v, p->xlon, p->ylat, &(p->xgrid[v]), &(p->ygrid[v]));
	mbview_projectll2display(v, p->xlon, p->ylat, p->zdata, &(p->xdisplay[v]), &(p->ydisplay[v]), &(p->zdisplay[v]));
}
static void mbgrdviz_segmentw_view(size_t v, struct mbview_linesegmentw_struct *s) {
	mbgrdviz_pointw_view(v, &(s->endpoints[0]));
	mbgrdviz_pointw_view(v, &(s->endpoints[1]));
	for (int k = 0; k < s->nls; k++)
		mbgrdviz_pointw_view(v, &(s->lspoints[k]));
}
static void mbgrdviz_view_project_shared(size_t v) {
	struct mbview_shareddata_struct *sd = &(shared.shareddata);
	for (int i = 0; i < sd->nsite; i++)
		mbgrdviz_pointw_view(v, &(sd->sites[i].point));
	for (int i = 0; i < sd->nroute; i++) {
		struct mbview_route_struct *r = &(sd->routes[i]);
		for (int j = 0; j < r->npoints; j++)
			mbgrdviz_pointw_view(v, &(r->points[j]));
		for (int j = 0; r->segments && j < r->npoints - 1; j++)
			mbgrdviz_segmentw_view(v, &(r->segments[j]));
	}
	for (int i = 0; i < sd->nnav; i++) {
		struct mbview_nav_struct *n = &(sd->navs[i]);
		for (int j = 0; j < n->npoints; j++) {
			mbgrdviz_pointw_view(v, &(n->navpts[j].point));
			mbgrdviz_pointw_view(v, &(n->navpts[j].pointport));
			mbgrdviz_pointw_view(v, &(n->navpts[j].pointcntr));
			mbgrdviz_pointw_view(v, &(n->navpts[j].pointstbd));
		}
		for (int j = 0; n->segments && j < n->npoints - 1; j++)
			mbgrdviz_segmentw_view(v, &(n->segments[j]));
	}
	for (int i = 0; i < sd->nvector; i++) {
		struct mbview_vector_struct *w = &(sd->vectors[i]);
		for (int j = 0; j < w->npoints; j++)
			mbgrdviz_pointw_view(v, &(w->vectorpts[j].point));
		for (int j = 0; w->segments && j < w->npoints - 1; j++)
			mbgrdviz_segmentw_view(v, &(w->segments[j]));
	}
}

/* do_mbgrdviz_openprimary's choice of the display projection, verbatim, on the bound grid */
int mbgrdviz_view_setup(int view, const char *title, int geographic, const char *crs, const float *z, int nx, int ny,
                        double x0, double x1, double y0, double y1, double dx, double dy) {
	if (view < 0 || view >= MBV_MAX_WINDOWS)
		return 0;
	int mbv_primary_grid_projection_mode;
	char mbv_primary_grid_projection_id[MB_PATH_MAXLINE];
	if (geographic) {
		mbv_primary_grid_projection_mode = MBV_PROJECTION_GEOGRAPHIC;
		sprintf(mbv_primary_grid_projection_id, "EPSG:%d", GCS_WGS_84);
	}
	else {
		if (crs == NULL || crs[0] == '\0')
			return 0;   /* a projected grid without its projection: no lon/lat, nothing to plan on */
		mbv_primary_grid_projection_mode = MBV_PROJECTION_PROJECTED;
		snprintf(mbv_primary_grid_projection_id, sizeof(mbv_primary_grid_projection_id), "%s", crs);
	}
	const double mbv_primary_xmin = x0;
	const double mbv_primary_xmax = x1;
	const double mbv_primary_ymin = y0;
	const double mbv_primary_ymax = y1;
	int mbv_display_projection_mode;
	char mbv_display_projection_id[MB_PATH_MAXLINE];
	double lon_origin, lat_origin;
	int projectionid;

	/* if grid projected then use the same projected coordinate system by default */
	if (mbv_primary_grid_projection_mode == MBV_PROJECTION_PROJECTED) {
		mbv_display_projection_mode = mbv_primary_grid_projection_mode;
		strcpy(mbv_display_projection_id, mbv_primary_grid_projection_id);
	}

	/* else if grid geographic and covers much of the world use spheroid */
	else if (mbv_primary_xmax - mbv_primary_xmin > 20.0 || mbv_primary_ymax - mbv_primary_ymin > 20.0) {
		mbv_display_projection_mode = MBV_PROJECTION_SPHEROID;
		sprintf(mbv_display_projection_id, "SPHEROID");
	}

	/* else if grid geographic then use LTM projection with origin at the center of the
	    grid for non-polar grids */
	else if (mbv_primary_ymax > -80.0 && mbv_primary_ymin < 84.0) {
		mbv_display_projection_mode = MBV_PROJECTION_PROJECTED;
		lon_origin = 0.5 * (mbv_primary_xmin + mbv_primary_xmax);
		lat_origin = 0.5 * (mbv_primary_ymin + mbv_primary_ymax);
		if (lon_origin > 180.0)
			lon_origin -= 360.0;
		sprintf(mbv_display_projection_id, "LTM%.5f/%.5f", lon_origin, lat_origin);
	}

	/* else if grid geographic and more northerly than 84 deg N then use
	        North Universal Polar Stereographic Projection */
	else if (mbv_primary_ymin > 84.0) {
		mbv_display_projection_mode = MBV_PROJECTION_PROJECTED;
		projectionid = 32661;
		sprintf(mbv_display_projection_id, "EPSG:%d", projectionid);
	}

	/* else if grid geographic and more southerly than 80 deg S then use
	        South Universal Polar Stereographic Projection */
	else if (mbv_primary_ymax < 80.0) {
		mbv_display_projection_mode = MBV_PROJECTION_PROJECTED;
		projectionid = 32761;
		sprintf(mbv_display_projection_id, "EPSG:%d", projectionid);
	}

	/* else just use geographic */
	else {
		mbv_display_projection_mode = MBV_PROJECTION_GEOGRAPHIC;
		sprintf(mbv_display_projection_id, "EPSG:%d", GCS_WGS_84);
	}

	const int status = mbgv_setup((size_t)view, title, mbv_primary_grid_projection_mode, mbv_primary_grid_projection_id,
	                              mbv_display_projection_mode, mbv_display_projection_id, z, nx, ny, x0, x1, y0, y1, dx, dy);
	mbview_id[view] = (status == MB_SUCCESS);
	if (status == MB_SUCCESS)
		mbgrdviz_view_project_shared((size_t)view);
	return status == MB_SUCCESS ? 1 : 0;
}

void mbgrdviz_view_release(int view) {
	if (view < 0 || view >= MBV_MAX_WINDOWS)
		return;
	mbgv_release((size_t)view);
	mbview_id[view] = false;
}

int mbgrdviz_view_ready(int view) {
	return mbgrdviz_view_ok(view) ? 1 : 0;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_open(int view, int what, const char *path) {
	if (!mbgrdviz_view_ok(view) || path == NULL || path[0] == '\0')
		return 0;
	mb_path file;
	snprintf(file, sizeof(file), "%s", path);
	int status = MB_FAILURE;
	switch (what) {
	case MBGRDVIZ_OPEN_SITE:
		status = do_mbgrdviz_opensite((size_t)view, file);
		break;
	case MBGRDVIZ_OPEN_ROUTE:
		status = do_mbgrdviz_openroute((size_t)view, file);
		break;
	case MBGRDVIZ_OPEN_VECTOR:
		status = do_mbgrdviz_openvector((size_t)view, file);
		break;
	case MBGRDVIZ_OPEN_NAV:
		status = do_mbgrdviz_opennav((size_t)view, false, file);
		break;
	case MBGRDVIZ_OPEN_SWATH:
		status = do_mbgrdviz_opennav((size_t)view, true, file);
		break;
	default:
		break;
	}
	return status == MB_SUCCESS ? 1 : 0;
}

int mbgrdviz_save(int view, int what, const char *path) {
	if (!mbgrdviz_view_ok(view) || path == NULL || path[0] == '\0')
		return 0;
	mb_path file;
	snprintf(file, sizeof(file), "%s", path);
	const size_t instance = (size_t)view;
	int status = MB_FAILURE;
	switch (what) {
	case MBGRDVIZ_SAVE_SITE:
		status = do_mbgrdviz_savesite(instance, file);
		break;
	case MBGRDVIZ_SAVE_ROUTE:
		status = do_mbgrdviz_saveroute(instance, file);
		break;
	case MBGRDVIZ_SAVE_ROUTEREVERSED:
		status = do_mbgrdviz_saveroutereversed(instance, file);
		break;
	case MBGRDVIZ_SAVE_RISISCRIPTHEADING:
		status = do_mbgrdviz_saverisiscriptheading(instance, file);
		break;
	case MBGRDVIZ_SAVE_RISISCRIPTNOHEADING:
		status = do_mbgrdviz_saverisiscriptnoheading(instance, file);
		break;
	case MBGRDVIZ_SAVE_RISI2SCRIPTHEADING:
		status = do_mbgrdviz_saverisi2scriptheading(instance, file);
		break;
	case MBGRDVIZ_SAVE_RISI2SCRIPTNOHEADING:
		status = do_mbgrdviz_saverisi2scriptnoheading(instance, file);
		break;
	case MBGRDVIZ_SAVE_DEGDECMIN:
		status = do_mbgrdviz_savedegdecmin(instance, file);
		break;
	case MBGRDVIZ_SAVE_LNW:
		status = do_mbgrdviz_savelnw(instance, file);
		break;
	case MBGRDVIZ_SAVE_GREENSEAYML:
		status = do_mbgrdviz_savegreenseayml(instance, file);
		break;
	case MBGRDVIZ_SAVE_TECDISLST:
		status = do_mbgrdviz_savetecdislst(instance, file);
		break;
	case MBGRDVIZ_SAVE_KONGSBERGDP:
		status = do_mbgrdviz_savekongsbergdp(instance, file);
		break;
	case MBGRDVIZ_SAVE_SISASCIIPLAN1:
		status = do_mbgrdviz_savesisasciiplan1(instance, file);
		break;
	case MBGRDVIZ_SAVE_SISASCIIPLAN2:
		status = do_mbgrdviz_savesisasciiplan2(instance, file);
		break;
	case MBGRDVIZ_SAVE_PROFILE:
		status = do_mbgrdviz_saveprofile(instance, file);
		break;
	default:
		break;
	}
	return status == MB_SUCCESS ? 1 : 0;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_route_count(void) {
	return shared.shareddata.nroute;
}

int mbgrdviz_route_info(int route, char *name, int namelen, int *color, int *size, int *nwaypoint) {
	if (route < 0 || route >= shared.shareddata.nroute)
		return 0;
	const struct mbview_route_struct *r = &(shared.shareddata.routes[route]);
	if (name && namelen > 0)
		snprintf(name, (size_t)namelen, "%s", r->name);
	if (color)
		*color = r->color;
	if (size)
		*size = r->size;
	if (nwaypoint)
		*nwaypoint = r->npoints;
	return 1;
}

int mbgrdviz_route_waypoints(int view, int route, double *xgrid, double *ygrid, double *z, int *waypoint, int n) {
	if (!mbgrdviz_view_ok(view) || route < 0 || route >= shared.shareddata.nroute)
		return 0;
	const struct mbview_route_struct *r = &(shared.shareddata.routes[route]);
	const int m = MIN(n, r->npoints);
	for (int j = 0; j < m; j++) {
		if (xgrid)
			xgrid[j] = r->points[j].xgrid[view];
		if (ygrid)
			ygrid[j] = r->points[j].ygrid[view];
		if (z)
			z[j] = r->points[j].zdata;
		if (waypoint)
			waypoint[j] = r->waypoint[j];
	}
	return m;
}

int mbgrdviz_route_add(int view, const char *name, int color, int size, const double *xgrid, const double *ygrid,
                       const int *waypoint, int n) {
	if (!mbgrdviz_view_ok(view) || n <= 0)
		return -1;
	double *lon = (double *)malloc(sizeof(double) * (size_t)n);
	double *lat = (double *)malloc(sizeof(double) * (size_t)n);
	int *wp = (int *)malloc(sizeof(int) * (size_t)n);
	int iroute = -1;
	if (lon && lat && wp) {
		for (int j = 0; j < n; j++) {
			mbview_projectgrid2ll((size_t)view, xgrid[j], ygrid[j], &lon[j], &lat[j]);
			wp[j] = waypoint ? waypoint[j] : MBV_ROUTE_WAYPOINT_SIMPLE;
		}
		mb_path rname;
		snprintf(rname, sizeof(rname), "%s", name ? name : "Route");
		if (mbview_addroute(verbose, (size_t)view, n, lon, lat, wp, color, size, true, rname, &iroute, &error) != MB_SUCCESS)
			iroute = -1;
	}
	free(lon);
	free(lat);
	free(wp);
	return iroute;
}

void mbgrdviz_routes_clear(int view) {
	if (mbgrdviz_view_ok(view))
		mbview_deleteallroutes(verbose, (size_t)view, &error);
	working_route = -1;
}

int mbgrdviz_route_rename(int route, const char *name) {
	if (route < 0 || route >= shared.shareddata.nroute || name == NULL)
		return 0;
	snprintf(shared.shareddata.routes[route].name, sizeof(mb_path), "%s", name);
	return 1;
}

void mbgrdviz_route_select(int route) {
	if (route < 0 || route >= shared.shareddata.nroute) {
		shared.shareddata.route_selected = MBV_SELECT_NONE;
		shared.shareddata.route_point_selected = MBV_SELECT_NONE;
	}
	else {
		shared.shareddata.route_selected = route;
		shared.shareddata.route_point_selected = MBV_SELECT_ALL;
	}
}

int mbgrdviz_route_distances(int view, int route, double *lateral, double *overtopo) {
	(void)view;
	if (route < 0 || route >= shared.shareddata.nroute)
		return 0;
	if (lateral)
		*lateral = shared.shareddata.routes[route].distancelateral;
	if (overtopo)
		*overtopo = shared.shareddata.routes[route].distancetopo;
	return 1;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_site_count(void) {
	return shared.shareddata.nsite;
}

int mbgrdviz_site_get(int view, int site, char *name, int namelen, double *xgrid, double *ygrid, double *z, int *color,
                      int *size) {
	if (!mbgrdviz_view_ok(view) || site < 0 || site >= shared.shareddata.nsite)
		return 0;
	const struct mbview_site_struct *s = &(shared.shareddata.sites[site]);
	if (name && namelen > 0)
		snprintf(name, (size_t)namelen, "%s", s->name);
	if (xgrid)
		*xgrid = s->point.xgrid[view];
	if (ygrid)
		*ygrid = s->point.ygrid[view];
	if (z)
		*z = s->point.zdata;
	if (color)
		*color = s->color;
	if (size)
		*size = s->size;
	return 1;
}

int mbgrdviz_sites_set(int view, int n, const double *xgrid, const double *ygrid, const int *color, const int *size,
                       const char *const *names) {
	if (!mbgrdviz_view_ok(view))
		return 0;
	while (shared.shareddata.nsite > 0)
		mbview_site_delete((size_t)view, shared.shareddata.nsite - 1);
	if (n <= 0)
		return 1;
	double *lon = (double *)malloc(sizeof(double) * (size_t)n);
	double *lat = (double *)malloc(sizeof(double) * (size_t)n);
	double *topo = (double *)malloc(sizeof(double) * (size_t)n);
	int *col = (int *)malloc(sizeof(int) * (size_t)n);
	int *siz = (int *)malloc(sizeof(int) * (size_t)n);
	mb_path *nam = (mb_path *)malloc(sizeof(mb_path) * (size_t)n);
	int status = MB_FAILURE;
	if (lon && lat && topo && col && siz && nam) {
		for (int i = 0; i < n; i++) {
			mbview_projectgrid2ll((size_t)view, xgrid[i], ygrid[i], &lon[i], &lat[i]);
			topo[i] = MBV_DEFAULT_NODATA;   /* addsites takes it from the grid */
			col[i] = color ? color[i] : MBV_COLOR_GREEN;
			siz[i] = size ? size[i] : 1;
			if (names && names[i] && names[i][0])
				snprintf(nam[i], sizeof(mb_path), "%s", names[i]);
			else
				snprintf(nam[i], sizeof(mb_path), "Site %d", i);
		}
		status = mbview_addsites(verbose, (size_t)view, n, lon, lat, topo, col, siz, nam, &error);
	}
	free(lon);
	free(lat);
	free(topo);
	free(col);
	free(siz);
	free(nam);
	return status == MB_SUCCESS ? 1 : 0;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_nav_count(void) {
	return shared.shareddata.nnav;
}

int mbgrdviz_nav_info(int nav, char *name, int namelen, char *pathraw, int pathlen, int *format, int *npoints,
                      int *swathbounds, int *nselected) {
	if (nav < 0 || nav >= shared.shareddata.nnav)
		return 0;
	const struct mbview_nav_struct *v = &(shared.shareddata.navs[nav]);
	if (name && namelen > 0)
		snprintf(name, (size_t)namelen, "%s", v->name);
	if (pathraw && pathlen > 0)
		snprintf(pathraw, (size_t)pathlen, "%s", v->pathraw);
	if (format)
		*format = v->format;
	if (npoints)
		*npoints = v->npoints;
	if (swathbounds)
		*swathbounds = v->swathbounds;
	if (nselected)
		*nselected = v->nselected;
	return 1;
}

int mbgrdviz_nav_points(int view, int nav, double *xgrid, double *ygrid, double *z, double *portx, double *porty,
                        double *stbdx, double *stbdy, int n) {
	if (!mbgrdviz_view_ok(view) || nav < 0 || nav >= shared.shareddata.nnav)
		return 0;
	const struct mbview_nav_struct *v = &(shared.shareddata.navs[nav]);
	const int m = MIN(n, v->npoints);
	for (int j = 0; j < m; j++) {
		const struct mbview_navpointw_struct *p = &(v->navpts[j]);
		if (xgrid)
			xgrid[j] = p->point.xgrid[view];
		if (ygrid)
			ygrid[j] = p->point.ygrid[view];
		if (z)
			z[j] = p->point.zdata;
		if (portx)
			portx[j] = p->pointport.xgrid[view];
		if (porty)
			porty[j] = p->pointport.ygrid[view];
		if (stbdx)
			stbdx[j] = p->pointstbd.xgrid[view];
		if (stbdy)
			stbdy[j] = p->pointstbd.ygrid[view];
	}
	return m;
}

void mbgrdviz_nav_select(int nav, int selected) {
	if (nav < 0 || nav >= shared.shareddata.nnav)
		return;
	struct mbview_nav_struct *v = &(shared.shareddata.navs[nav]);
	for (int j = 0; j < v->npoints; j++)
		v->navpts[j].selected = selected ? true : false;
	v->nselected = selected ? v->npoints : 0;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_vector_count(void) {
	return shared.shareddata.nvector;
}

int mbgrdviz_vector_info(int vec, char *name, int namelen, int *npoints, double *datamin, double *datamax) {
	if (vec < 0 || vec >= shared.shareddata.nvector)
		return 0;
	const struct mbview_vector_struct *v = &(shared.shareddata.vectors[vec]);
	if (name && namelen > 0)
		snprintf(name, (size_t)namelen, "%s", v->name);
	if (npoints)
		*npoints = v->npoints;
	if (datamin)
		*datamin = v->datamin;
	if (datamax)
		*datamax = v->datamax;
	return 1;
}

int mbgrdviz_vector_points(int view, int vec, double *xgrid, double *ygrid, double *z, double *data, int n) {
	if (!mbgrdviz_view_ok(view) || vec < 0 || vec >= shared.shareddata.nvector)
		return 0;
	const struct mbview_vector_struct *v = &(shared.shareddata.vectors[vec]);
	const int m = MIN(n, v->npoints);
	for (int j = 0; j < m; j++) {
		if (xgrid)
			xgrid[j] = v->vectorpts[j].point.xgrid[view];
		if (ygrid)
			ygrid[j] = v->vectorpts[j].point.ygrid[view];
		if (z)
			z[j] = v->vectorpts[j].point.zdata;
		if (data)
			data[j] = v->vectorpts[j].data;
	}
	return m;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_set_area(int view, double x0, double y0, double x1, double y1, double width) {
	if (!mbgrdviz_view_ok(view))
		return 0;
	return mbgv_set_area((size_t)view, x0, y0, x1, y1, width) == MB_SUCCESS ? 1 : 0;
}

int mbgrdviz_set_region(int view, double x0, double y0, double x1, double y1) {
	if (!mbgrdviz_view_ok(view))
		return 0;
	return mbgv_set_region((size_t)view, x0, y0, x1, y1) == MB_SUCCESS ? 1 : 0;
}

int mbgrdviz_area_get(int view, double *x, double *y, double *length, double *width, double *bearing) {
	if (!mbgrdviz_view_ok(view))
		return 0;
	const struct mbview_struct *data = &(mbviews[view].data);
	if (data->area_type != MBV_AREA_QUAD)
		return 0;
	/* the corners as mbview_area projected them back: the start of each side segment */
	for (int i = 0; i < 4; i++) {
		if (x)
			x[i] = data->area.segments[i].endpoints[0].xgrid;
		if (y)
			y[i] = data->area.segments[i].endpoints[0].ygrid;
	}
	if (length)
		*length = data->area.length;
	if (width)
		*width = data->area.width;
	if (bearing)
		*bearing = data->area.bearing;
	return 1;
}

/*--------------------------------------------------------------------*/
void mbgrdviz_survey_get(struct mbgrdviz_survey *p) {
	if (p == NULL)
		return;
	p->mode = survey_mode;
	p->platform = survey_platform;
	p->interleaving = survey_interleaving;
	p->direction = survey_direction;
	p->crosslines_last = survey_crosslines_last ? 1 : 0;
	p->crosslines = survey_crosslines;
	p->linespacing = survey_linespacing;
	p->swathwidth = survey_swathwidth;
	p->depth = survey_depth;
	p->altitude = survey_altitude;
	p->color = survey_color;
	snprintf(p->name, sizeof(p->name), "%s", survey_name);
}

/* do_mbgrdviz_arearoute_parameterchange, the reading of the dialog */
void mbgrdviz_survey_set(const struct mbgrdviz_survey *p) {
	if (p == NULL)
		return;
	survey_mode = p->mode;
	survey_platform = p->platform;
	survey_interleaving = MAX(p->interleaving, 1);
	survey_direction = p->direction;
	survey_crosslines_last = p->crosslines_last != 0;
	survey_crosslines = MAX(p->crosslines, 0);
	survey_linespacing = MAX(p->linespacing, 1);
	survey_swathwidth = p->swathwidth;
	survey_depth = p->depth;
	survey_altitude = p->altitude;
	survey_color = p->color;
	snprintf(survey_name, sizeof(survey_name), "%s", p->name);
	if (strlen(survey_name) <= 0)
		sprintf(survey_name, "Survey");
}

int mbgrdviz_generate_survey(int view) {
	if (!mbgrdviz_view_ok(view))
		return -1;
	do_mbgrdviz_generate_survey((size_t)view);
	return working_route;
}

/* do_mbgrdviz_arearoute_dismiss */
void mbgrdviz_survey_dismiss(void) {
	/* reset current working route so the last one generated is saved */
	working_route = -1;
}

void mbgrdviz_survey_set_working(int route) {
	working_route = (route >= 0 && route < shared.shareddata.nroute) ? route : -1;
}

/* do_mbgrdviz_arearoute_info's text, as plain lines */
static char mbgrdviz_info_text[2 * MB_PATH_MAXLINE];
void do_mbgrdviz_arearoute_info(size_t instance) {
	struct mbview_struct *data;
	int status = mbview_getdataptr(verbose, instance, &data, &error);

	/* check if area is currently defined */
	if (status == MB_SUCCESS) {
		if (data->area_type != MBV_AREA_QUAD)
			status = MB_FAILURE;
		sprintf(mbgrdviz_info_text, "No Current Area:");
	}

	/* set widgets */
	if (status == MB_SUCCESS) {
		if (working_route >= 0) {
			int nroutewaypoint;
			int nroutpoint;
			char routename[MB_PATH_MAXLINE];
			int routecolor;
			int routesize;
			double routedistancelateral;
			double routedistancetopo;

			/* get info for working route */
			status = mbview_getrouteinfo(verbose, instance, working_route, &nroutewaypoint, &nroutpoint, routename,
			                             &routecolor, &routesize, &routedistancelateral, &routedistancetopo, &error);

			snprintf(mbgrdviz_info_text, sizeof(mbgrdviz_info_text),
			         "Current Area:\n Length: %.1f m  Width: %.1f m  Bearing: %.1f deg\nNew Route: %d  Name: %s\n"
			         " Waypoints: %d  Total Points:%d\n Distance: %.1f m (lateral) %.1f m (over bottom)",
			         data->area.length, data->area.width, data->area.bearing, working_route, routename, nroutewaypoint,
			         nroutpoint, routedistancelateral, routedistancetopo);
		}
		else {
			snprintf(mbgrdviz_info_text, sizeof(mbgrdviz_info_text),
			         "Current Area:\n Length: %.3f m\n Width: %.3f m\n Bearing: %.1f deg", data->area.length,
			         data->area.width, data->area.bearing);
		}
	}
}

int mbgrdviz_survey_info(int view, char *text, int len) {
	if (text == NULL || len <= 0)
		return 0;
	if (!mbgrdviz_view_ok(view)) {
		snprintf(text, (size_t)len, "No Current Area:");
		return 0;
	}
	do_mbgrdviz_arearoute_info((size_t)view);
	snprintf(text, (size_t)len, "%s", mbgrdviz_info_text);
	return 1;
}

/*--------------------------------------------------------------------*/
int mbgrdviz_route_profile(int view, int route) {
	if (!mbgrdviz_view_ok(view) || route < 0 || route >= shared.shareddata.nroute)
		return 0;
	mbgrdviz_route_select(route);
	mbview_extract_route_profile((size_t)view);
	return mbviews[view].data.profile.npoints;
}

int mbgrdviz_profile_get(int view, double *distance, double *z, int n, double *length, double *zmin, double *zmax) {
	if (!mbgrdviz_view_ok(view))
		return 0;
	const struct mbview_profile_struct *p = &(mbviews[view].data.profile);
	const int m = MIN(n, p->npoints);
	for (int k = 0; k < m; k++) {
		if (distance)
			distance[k] = p->points[k].distance;
		if (z)
			z[k] = p->points[k].zdata;
	}
	if (length)
		*length = p->length;
	if (zmin)
		*zmin = p->zmin;
	if (zmax)
		*zmax = p->zmax;
	return m;
}

/*--------------------------------------------------------------------*/
/* do_mbgrdviz_open_region's extraction of the primary grid, verbatim; the new view it opened with
   it is the host's to make */
int mbgrdviz_region_grid(int view, float **z, int *nx, int *ny, double *x0, double *x1, double *y0, double *y1) {
	int ixmin, ixmax, jymin, jymax;
	int i, j, k, ksource;
	if (!mbgrdviz_view_ok(view) || z == NULL)
		return 0;
	*z = NULL;
	struct mbview_struct *data_source = &(mbviews[view].data);
	if (data_source->region_type != MBV_REGION_QUAD)
		return 0;

	double mbv_primary_dx = data_source->primary_dx;
	double mbv_primary_dy = data_source->primary_dy;
	double mbv_primary_xmin = MIN(data_source->region.cornerpoints[0].xgrid, data_source->region.cornerpoints[3].xgrid);
	double mbv_primary_xmax = MAX(data_source->region.cornerpoints[0].xgrid, data_source->region.cornerpoints[3].xgrid);
	double mbv_primary_ymin = MIN(data_source->region.cornerpoints[0].ygrid, data_source->region.cornerpoints[3].ygrid);
	double mbv_primary_ymax = MAX(data_source->region.cornerpoints[0].ygrid, data_source->region.cornerpoints[3].ygrid);
	ixmin = (mbv_primary_xmin - data_source->primary_xmin) / mbv_primary_dx;
	ixmax = ((mbv_primary_xmax - data_source->primary_xmin) / mbv_primary_dx) + 1;
	jymin = (mbv_primary_ymin - data_source->primary_ymin) / mbv_primary_dy;
	jymax = ((mbv_primary_ymax - data_source->primary_ymin) / mbv_primary_dy) + 1;
	ixmin = MAX(ixmin, 0);
	ixmax = MIN(ixmax, data_source->primary_n_columns - 1);
	jymin = MAX(jymin, 0);
	jymax = MIN(jymax, data_source->primary_n_rows - 1);
	mbv_primary_xmin = data_source->primary_xmin + mbv_primary_dx * ixmin;
	mbv_primary_xmax = data_source->primary_xmin + mbv_primary_dx * ixmax;
	mbv_primary_ymin = data_source->primary_ymin + mbv_primary_dy * jymin;
	mbv_primary_ymax = data_source->primary_ymin + mbv_primary_dy * jymax;
	const int mbv_primary_n_columns = ixmax - ixmin + 1;
	const int mbv_primary_n_rows = jymax - jymin + 1;
	if (mbv_primary_n_columns < 2 || mbv_primary_n_rows < 2)
		return 0;
	const int mbv_primary_nxy = mbv_primary_n_columns * mbv_primary_n_rows;
	float *mbv_primary_data = (float *)malloc(sizeof(float) * (size_t)mbv_primary_nxy);
	if (mbv_primary_data == NULL)
		return 0;
	for (i = 0; i < mbv_primary_n_columns; i++) {
		for (j = 0; j < mbv_primary_n_rows; j++) {
			k = i * mbv_primary_n_rows + j;
			ksource = (i + ixmin) * data_source->primary_n_rows + (j + jymin);
			mbv_primary_data[k] = data_source->primary_data[ksource];
			/* the host's no-data is NaN */
			if (mbv_primary_data[k] == data_source->primary_nodatavalue)
				mbv_primary_data[k] = NAN;
		}
	}
	*z = mbv_primary_data;
	*nx = mbv_primary_n_columns;
	*ny = mbv_primary_n_rows;
	*x0 = mbv_primary_xmin;
	*x1 = mbv_primary_xmax;
	*y0 = mbv_primary_ymin;
	*y1 = mbv_primary_ymax;
	return 1;
}

void mbgrdviz_free(void *p) {
	free(p);
}
