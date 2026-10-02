/*--------------------------------------------------------------------
 *    The MB-system:	mbeditviz.h		4/27/2007
 *
 *    Copyright (c) 2007-2025 by
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
 * InteractiveGMT port (deps/src/mbeditviz/mbeditviz.h): MB-System 5.8.3's src/mbeditviz/mbeditviz.h
 * -- the engine's structures, globals and prototypes -- plus the pieces of libmbview's mbview.h the
 * engine reads (the 3-D soundings structures, colours), with the Motif prototypes removed.
 *
 * mbview itself is NOT ported: the grid and the navigation are shown in an ordinary InteractiveGMT
 * window, and the selections the engine reads off mbview's data (a region, an area, picked
 * navigation) are filled by the window (mbeditviz_window.cpp) into the small stand-in structures
 * below, under mbview's own names, so the selection code of the engine is unchanged. The few mbview
 * calls the engine makes (refresh the grid, colour a depth) are host hooks.
 *--------------------------------------------------------------------*/

#ifndef MB_EDITVIZ_DEF
#define MB_EDITVIZ_DEF 1

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the MBIO tables (the swath editor's and this tool's) are C objects: C linkage from C++ too */
#include "mbeditviz_mbio.h"

/* the swath editor's MBIO loader (deps/src/mbedit/mbedit.c) */
int mbedit_mbio_loaded(void);
const char *mbedit_mbio_version(void);

/* MBeditviz defines */
#define MBEV_GRID_NONE 0
#define MBEV_GRID_NOTVIEWED 1
#define MBEV_GRID_VIEWED 2
#define MBEV_GRID_ALGORITH_SIMPLE 0
#define MBEV_GRID_ALGORITH_FOOTPRINT 1
#define MBEV_GRID_WEIGHT_TINY 0.0000001

/* sanity limit on the number of cells in a grid (four float arrays are
   allocated at this size); guards against bogus navigation or bounds
   producing a grid so large that the column*row cell count computation
   overflows a 32-bit int, which previously caused undersized allocations
   and out-of-bounds writes (crashes) rather than a clean allocation failure */
#define MBEV_GRID_MAX_CELLS 100000000
#define MBEV_ALLOC_NUM 24
#define MBEV_ALLOCK_NUM 1024
#define MBEV_NODATA -10000000.0
#define MBEV_NUM_ESF_OPEN_MAX 25

typedef enum {
     MBEV_GRID_ALGORITHM_SIMPLEMEAN = 0,
     MBEV_GRID_ALGORITHM_FOOTPRINT = 1,
     MBEV_GRID_ALGORITHM_SHOALBIAS = 2,
 } gridalgorithm_t;

typedef enum {
     MBEV_OUTPUT_MODE_EDIT = 0,
     MBEV_OUTPUT_MODE_BROWSE = 1,
 } output_mode_t;


/* usage of footprint based weight */
#define MBEV_USE_NO 0
#define MBEV_USE_YES 1
#define MBEV_USE_CONDITIONAL 2

/* ---- from libmbview's mbview.h ------------------------------------------------------ */
#define MBV_COLOR_BLACK 0
#define MBV_COLOR_WHITE 1
#define MBV_COLOR_RED 2
#define MBV_COLOR_YELLOW 3
#define MBV_COLOR_GREEN 4
#define MBV_COLOR_BLUEGREEN 5
#define MBV_COLOR_BLUE 6
#define MBV_COLOR_PURPLE 7

#define MB3DSDG_MOUSE_TOGGLE 0
#define MB3DSDG_MOUSE_PICK 1
#define MB3DSDG_MOUSE_ERASE 2
#define MB3DSDG_MOUSE_RESTORE 3
#define MB3DSDG_MOUSE_GRAB 4
#define MB3DSDG_MOUSE_INFO 5
#define MB3DSDG_EDIT_NOFLUSH 0
#define MB3DSDG_EDIT_FLUSH 1
#define MB3DSDG_EDIT_FLUSHPREVIOUS 2

#define MB3DSDG_OPTIMIZEBIASVALUES_NONE 0x00
#define MB3DSDG_OPTIMIZEBIASVALUES_R 0x01
#define MB3DSDG_OPTIMIZEBIASVALUES_P 0x02
#define MB3DSDG_OPTIMIZEBIASVALUES_H 0x04
#define MB3DSDG_OPTIMIZEBIASVALUES_T 0x08
#define MB3DSDG_OPTIMIZEBIASVALUES_S 0x10
#define MB3DSDG_OPTIMIZEBIASVALUES_RP 0x03
#define MB3DSDG_OPTIMIZEBIASVALUES_RPH 0x07
#define MB3DSDG_OPTIMIZEBIASVALUES_RPHT 0x0F

struct mb3dsoundings_sounding_struct {
	int ifile;
	int iping;
	int iprofile;
	int ibeam;
	int beamcolor;
	char beamflag;
	char beamflagorg;
	double x;
	double y;
	double z;
	double a;
	float glx;
	float gly;
	float glz;
	float r;
	float g;
	float b;
	int winx;
	int winy;
};

struct mb3dsoundings_struct {
	/* display flag */
	bool displayed;

	/* location and scale parameters */
	double xorigin;
	double yorigin;
	double zorigin;
	double xmin;
	double ymin;
	double zmin;
	double xmax;
	double ymax;
	double zmax;
	double bearing;
	double sinbearing;
	double cosbearing;
	double scale;
	double zscale;

	/* sounding data */
	int num_soundings;
	int num_soundings_unflagged;
	int num_soundings_flagged;
	int num_soundings_alloc;
	struct mb3dsoundings_sounding_struct *soundings;
};

/* ---- the mbview selections the engine reads, filled by the InteractiveGMT window ---- */
#define MBV_REGION_NONE 0
#define MBV_REGION_QUAD 2
#define MBV_AREA_NONE 0
#define MBV_AREA_QUAD 2

struct mbview_pointw_struct {             /* the fields of mbview's point the engine reads */
	double xgrid;
	double ygrid;
	double zdata;
};
struct mbview_region_struct {
	struct mbview_pointw_struct cornerpoints[4];
};
struct mbview_area_struct {
	struct mbview_pointw_struct endpoints[2];
	struct mbview_pointw_struct cornerpoints[4];
	double length;
	double width;
	double bearing;
};
struct mbview_struct {
	int region_type;
	int area_type;
	int state21;                             /* "Enable Secondary Picks" (MBV_STATEMASK_21) */
	struct mbview_region_struct region;
	struct mbview_area_struct area;
};
struct mbview_navpointw_struct {
	bool selected;
};
struct mbview_nav_struct {
	struct mbview_navpointw_struct *navpts;  /* one per ping of the loaded file */
};
struct mbview_shareddata_struct {
	struct mbview_nav_struct *navs;          /* one per LOADED file, in file order */
};

/* mbeditviz structures */
struct mbev_ping_struct {
	int time_i[7];
	double time_d;
	int multiplicity;
	bool dualprofile;
	int dualprofilebeam;
	double navlon;
	double navlat;
	double navlonx;
	double navlaty;
	double portlon;
	double portlat;
	double stbdlon;
	double stbdlat;
	double speed;
	double heading;
	double distance;
	double altitude;
	double sensordepth;
	double draft;
	double roll;
	double pitch;
	double heave;
	double ssv;
	int beams_bath;
	char *beamflag;
	char *beamflagorg;
	int *beamcolor;
	double *bath;
	double *amp;
	double *bathacrosstrack;
	double *bathalongtrack;
	double *bathcorr;
	double *bathlon;
	double *bathlat;
	double *bathx;
	double *bathy;
	double *angles;
	double *angles_forward;
	double *angles_null;
	double *ttimes;
	double *bheave;
	double *alongtrack_offset;
};
struct mbev_file_struct {
	int load_status;
	int load_status_shown;
	bool locked;
	bool esf_exists;
	char name[MB_PATH_MAXLINE];
	char path[MB_PATH_MAXLINE];
	int format;
	int raw_info_loaded;
	int processed_info_loaded;
	struct mb_info_struct raw_info;
	struct mb_info_struct processed_info;
	struct mb_process_struct process;
	bool esf_open;
	bool esf_changed;
	char esffile[MB_PATH_MAXLINE];
	struct mb_esf_struct esf;
	int num_pings;
	int num_pings_alloc;
	struct mbev_ping_struct *pings;
	double beamwidth_xtrack;
	double beamwidth_ltrack;
	int topo_type;

	int n_async_heading;
	int n_async_heading_alloc;
	double *async_heading_time_d;
	double *async_heading_heading;
	int n_async_sensordepth;
	int n_async_sensordepth_alloc;
	double *async_sensordepth_time_d;
	double *async_sensordepth_sensordepth;
	int n_async_attitude;
	int n_async_attitude_alloc;
	double *async_attitude_time_d;
	double *async_attitude_roll;
	double *async_attitude_pitch;
	int n_sync_attitude;
	int n_sync_attitude_alloc;
	double *sync_attitude_time_d;
	double *sync_attitude_roll;
	double *sync_attitude_pitch;
};
struct mbev_grid_struct {
	int status;
	char projection_id[MB_PATH_MAXLINE];
	void *pjptr;

	/// minimum lat, maximum lat, minimum lon, maximum lon
	double bounds[4];

	/// minimum northing, maximum northing, minimum easting, maximum easting
	double boundsutm[4];

	/// Grid easting increment (meters)
	double dx;

	/// Grid northing increment (meters)
	double dy;

	int n_columns;
	int n_rows;

	/// minimum depth
	double min;

	/// maximum depth
	double max;

	double smin;

	double smax;

	/// Value denoting 'no data'
	float nodatavalue;

	float *sum;
	float *wgt;

	/// Depth values
	float *val;

	float *sgm;
};

/*--------------------------------------------------------------------*/

/* mbeditviz global control parameters (defined in mbeditviz.c) */

/* status parameters */
extern int mbev_status;
extern int mbev_error;
extern int mbev_verbose;

/* gui parameters */
extern int mbev_message_on;

/* mode parameters */
extern int mbev_mode_output;

/* data parameters */
extern int mbev_num_files;
extern int mbev_num_files_alloc;
extern int mbev_num_files_loaded;
extern int mbev_num_pings_loaded;
extern int mbev_num_esf_open;
extern int mbev_num_soundings_loaded;
extern int mbev_num_soundings_secondary;
extern double mbev_bounds[4];
extern struct mbev_file_struct *mbev_files;
extern struct mbev_grid_struct mbev_grid;
extern size_t mbev_instance;

/* gridding parameters */
extern double mbev_grid_bounds[4];
extern double mbev_grid_boundsutm[4];
extern double mbev_grid_cellsize;
extern gridalgorithm_t mbev_grid_algorithm;
extern int mbev_grid_interpolation;
extern int mbev_grid_n_columns;
extern int mbev_grid_n_rows;

/* global patch test parameters */
extern double mbev_rollbias;
extern double mbev_pitchbias;
extern double mbev_headingbias;
extern double mbev_timelag;
extern double mbev_snell;

/* sparse voxel filter parameters */
extern int mbev_sizemultiplier;
extern int mbev_nsoundingthreshold;

/* selected sounding parameters */
extern struct mb3dsoundings_struct mbev_selected;

/* ---- port additions ----------------------------------------------------------------- */
/* Pick the entry points this tool needs out of the MBIO library the swath editor has ALREADY
 * loaded (mbedit_mbio_open). Returns 1 when that library is MB-System 5.8.x and exports every one
 * of them; otherwise 0 with the reason in `msg`. */
int mbeditviz_mbio_open(char *msg, int msglen);

/* ---- the engine (mbeditviz_prog.c) -------------------------------------------------- */
/* mbeditviz_init: as MB-System's, without argc/argv (the command line, -I -F -G, is the window's) */
int mbeditviz_init(char *programName, char *helpMsg, char *usageMsg, int (*showMessage)(char *), int (*hideMessage)(void),
                   void (*updateGui)(void), int (*showErrorDialog)(char *, char *, char *));

int mbeditviz_get_format(char *file, int *form);
int mbeditviz_open_data(char *path, int format);

/** Read list of relevant files into global mbev_files array */
int mbeditviz_import_file(char *path, int format);

/** Read swath data from specified file into global mbev_file array element  */
int mbeditviz_load_file(int ifile, bool assertLock);

int mbeditviz_apply_biasesandtimelag(struct mbev_file_struct *file, struct mbev_ping_struct *ping, double rollbias, double pitchbias,
                            double headingbias, double timelag, double *headingdelta, double *sensordepth, double *rolldelta,
                            double *pitchdelta);
int mbeditviz_snell_correction(double snell, double roll, double *beam_xtrack,
							   double *beam_ltrack, double *beam_z);
int mbeditviz_beam_position(double navlon, double navlat, double mtodeglon, double mtodeglat, double rawbath, double acrosstrack,
                            double alongtrack, double sensordepth, double rolldelta, double pitchdelta, double heading,
                            double *bathcorr, double *lon, double *lat);
int mbeditviz_unload_file(int ifile, bool assertUnlock);
int mbeditviz_delete_file(int ifile);
double mbeditviz_erf(double x);
int mbeditviz_bin_weight(double foot_a, double foot_b, double scale, double pcx, double pcy, double dx, double dy, double *px,
                         double *py, double *weight, int *use);

/** Read grid bounds of loaded files into global mbev_grid_bounds array */
int mbeditviz_get_grid_bounds(void);

/** Setup the grid to contain loaded files */
int mbeditviz_setup_grid(void);

/** Allocate and load individual swath soundings */
int mbeditviz_project_soundings(void);

/** Create the grid to containing loaded files */
int mbeditviz_make_grid(void);

int mbeditviz_grid_beam(struct mbev_file_struct *file, struct mbev_ping_struct *ping, int ibeam,
                        bool beam_ok, bool apply_now);

int mbeditviz_make_grid_simple(void);
int mbeditviz_destroy_grid(void);
int mbeditviz_selectregion(size_t instance);
int mbeditviz_selectarea(size_t instance);
int mbeditviz_selectnav(size_t instance);
void mbeditviz_mb3dsoundings_dismiss(void);
void mbeditviz_mb3dsoundings_edit(int ifile, int iping, int ibeam, char beamflag, int flush);
void mbeditviz_mb3dsoundings_info(int ifile, int iping, int ibeam, char *infostring);
void mbeditviz_mb3dsoundings_bias(double rollbias, double pitchbias, double headingbias, double timelag, double snell);
void mbeditviz_mb3dsoundings_biasapply(double rollbias, double pitchbias, double headingbias, double timelag, double snell);
void mbeditviz_mb3dsoundings_flagsparsevoxels(int sizemultiplier, int nsoundingthreshold);
void mbeditviz_mb3dsoundings_colorsoundings(int color);
void mbeditviz_mb3dsoundings_optimizebiasvalues(int mode, double *rollbias, double *pitchbias, double *headingbias,
                                                double *timelag, double *snell);
void mbeditviz_mb3dsoundings_getbiasvariance(double local_grid_xmin, double local_grid_xmax, double local_grid_ymin,
                                             double local_grid_ymax, int local_grid_nx, int local_grid_ny, double local_grid_dx,
                                             double local_grid_dy, double *local_grid_first, double *local_grid_sum,
                                             double *local_grid_sum2, double *local_grid_variance, int *local_grid_num,
                                             double rollbias, double pitchbias, double headingbias, double timelag, double snell,
                                             int *variance_total_num, double *variance_total);

/* ---- host hooks: the mbview calls of the engine, implemented by the window ---------------- */
/* Mapped onto mbev_-prefixed symbols so they can never bind to a real libmbview in the process. */
#define mbview_getdataptr            mbev_view_getdataptr
#define mbview_getsharedptr          mbev_view_getsharedptr
#define mbview_colorvalue_instance   mbev_view_colorvalue_instance
#define mbview_updateprimarygridcell mbev_view_updateprimarygridcell
#define mbview_updateprimarygrid     mbev_view_updateprimarygrid
#define mbview_updatesecondarygrid   mbev_view_updatesecondarygrid
#define mbview_plothigh              mbev_view_plothigh

int mbview_getdataptr(int verbose, size_t instance, struct mbview_struct **datahandle, int *error);
int mbview_getsharedptr(int verbose, struct mbview_shareddata_struct **sharedhandle, int *error);
int mbview_colorvalue_instance(size_t instance, double value, float *r, float *g, float *b);
int mbview_updateprimarygridcell(int verbose, size_t instance, int primary_ix, int primary_jy, float value, int *error);
int mbview_updateprimarygrid(int verbose, size_t instance, int primary_n_columns, int primary_n_rows, float *primary_data,
                             int *error);
int mbview_updatesecondarygrid(int verbose, size_t instance, int secondary_n_columns, int secondary_n_rows,
                               float *secondary_data, int *error);
int mbview_plothigh(size_t instance);

#ifdef __cplusplus
}
#endif

#endif /* MB_EDITVIZ_DEF */
