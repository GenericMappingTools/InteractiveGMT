/*--------------------------------------------------------------------
 *    The MB-system:	mbview.h	10/9/2002
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
 * InteractiveGMT port (deps/src/mbgrdviz/mbgrdviz_mbview.h): the DATA half of libmbview that
 * mbgrdviz drives -- sites, routes, navigation, vectors, profiles, the area and region picks, and
 * the projections between grid, geographic and display coordinates -- for the two C files of this
 * port (mbgrdviz.c, mbgrdviz_mbview.c). C only: no C++ file includes it.
 *
 * The defines and structures below are MB-System 5.8.3's src/mbview/mbview.h lines 39-692,
 * verbatim. The world structure is mbviewprivate.h's, cut down to the members the ported data
 * code reads (no widgets, no OpenGL). The pictures are not ported: what libmbview draws with
 * OpenGL, InteractiveGMT draws as its own elements (mbgrdviz_window.cpp).
 *
 * Every libmbview function name is mapped onto mbgv_* below, because the mbeditviz port already
 * carries a few functions under libmbview's names and both live in the same library.
 */

#ifndef MBGRDVIZ_MBVIEW_H_
#define MBGRDVIZ_MBVIEW_H_

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mbgrdviz_mbio.h"

#define MB_VERSION (mbedit_mbio_version())
int mbedit_mbio_loaded(void);
const char *mbedit_mbio_version(void);

/* ---- libmbview names -> this port's ------------------------------------------------- */
#define mbview_addnav                      mbgv_addnav
#define mbview_addroute                    mbgv_addroute
#define mbview_addsites                    mbgv_addsites
#define mbview_addvector                   mbgv_addvector
#define mbview_allocnavarrays              mbgv_allocnavarrays
#define mbview_allocprofilearrays          mbgv_allocprofilearrays
#define mbview_allocprofilepoints          mbgv_allocprofilepoints
#define mbview_allocroutearrays            mbgv_allocroutearrays
#define mbview_allocsitearrays             mbgv_allocsitearrays
#define mbview_allocvectorarrays           mbgv_allocvectorarrays
#define mbview_deleteallroutes             mbgv_deleteallroutes
#define mbview_deleteroute                 mbgv_deleteroute
#define mbview_drapesegment                mbgv_drapesegment
#define mbview_drapesegment_gc             mbgv_drapesegment_gc
#define mbview_drapesegment_grid           mbgv_drapesegment_grid
#define mbview_drapesegmentw               mbgv_drapesegmentw
#define mbview_drapesegmentw_gc            mbgv_drapesegmentw_gc
#define mbview_drapesegmentw_grid          mbgv_drapesegmentw_grid
#define mbview_enableeditroutes            mbgv_enableeditroutes
#define mbview_enableeditsites             mbgv_enableeditsites
#define mbview_enableviewnavs              mbgv_enableviewnavs
#define mbview_enableviewroutes            mbgv_enableviewroutes
#define mbview_enableviewsites             mbgv_enableviewsites
#define mbview_enableviewvectors           mbgv_enableviewvectors
#define mbview_extract_pick_profile        mbgv_extract_pick_profile
#define mbview_extract_route_profile       mbgv_extract_route_profile
#define mbview_freenavarrays               mbgv_freenavarrays
#define mbview_freeprofilearrays           mbgv_freeprofilearrays
#define mbview_freeprofilepoints           mbgv_freeprofilepoints
#define mbview_freeroutearrays             mbgv_freeroutearrays
#define mbview_freesitearrays              mbgv_freesitearrays
#define mbview_freevectorarrays            mbgv_freevectorarrays
#define mbview_getdataptr                  mbgv_getdataptr
#define mbview_getnavcount                 mbgv_getnavcount
#define mbview_getnavpointcount            mbgv_getnavpointcount
#define mbview_getprofile                  mbgv_getprofile
#define mbview_getprofilecount             mbgv_getprofilecount
#define mbview_getroute                    mbgv_getroute
#define mbview_getroutecount               mbgv_getroutecount
#define mbview_getrouteinfo                mbgv_getrouteinfo
#define mbview_getroutepointcount          mbgv_getroutepointcount
#define mbview_getrouteselected            mbgv_getrouteselected
#define mbview_getsharedptr                mbgv_getsharedptr
#define mbview_getsitecount                mbgv_getsitecount
#define mbview_getsites                    mbgv_getsites
#define mbview_getvectorcount              mbgv_getvectorcount
#define mbview_getvectorpointcount         mbgv_getvectorpointcount
#define mbview_getzdata                    mbgv_getzdata
#define mbview_greatcircle_dist            mbgv_greatcircle_dist
#define mbview_greatcircle_distbearing     mbgv_greatcircle_distbearing
#define mbview_greatcircle_endposition     mbgv_greatcircle_endposition
#define mbview_nav_delete                  mbgv_nav_delete
#define mbview_pick_text                   mbgv_pick_text
#define mbview_projectdisplay2ll           mbgv_projectdisplay2ll
#define mbview_projectdistance             mbgv_projectdistance
#define mbview_projectforward              mbgv_projectforward
#define mbview_projectfromlonlat           mbgv_projectfromlonlat
#define mbview_projectgrid2ll              mbgv_projectgrid2ll
#define mbview_projectinverse              mbgv_projectinverse
#define mbview_projectll2display           mbgv_projectll2display
#define mbview_projectll2xygrid            mbgv_projectll2xygrid
#define mbview_projectll2xyzgrid           mbgv_projectll2xyzgrid
#define mbview_route_add                   mbgv_route_add
#define mbview_route_delete                mbgv_route_delete
#define mbview_route_setdistance           mbgv_route_setdistance
#define mbview_site_delete                 mbgv_site_delete
#define mbview_sphere_forward              mbgv_sphere_forward
#define mbview_sphere_inverse              mbgv_sphere_inverse
#define mbview_sphere_matrix               mbgv_sphere_matrix
#define mbview_sphere_rotate               mbgv_sphere_rotate
#define mbview_sphere_setup                mbgv_sphere_setup
#define mbview_update                      mbgv_update
#define mbview_updatenavlist               mbgv_updatenavlist
#define mbview_updatepointw                mbgv_updatepointw
#define mbview_updateroutelist             mbgv_updateroutelist
#define mbview_updatesegmentw              mbgv_updatesegmentw
#define mbview_updatesitelist              mbgv_updatesitelist
#define mbview_vector_delete               mbgv_vector_delete
#define mbview_zscalepoint                 mbgv_zscalepoint
#define mbview_zscalepointw                mbgv_zscalepointw

/* ---- mbview.h 5.8.3, lines 39-692, verbatim ----------------------------------------- */

/* maximum number of mbview windows */
#define MBV_MAX_WINDOWS 10

/* no window / invalid instance flag */
#define MBV_NO_WINDOW 999

/* typical number of array elements to allocate at a time */
#define MBV_ALLOC_NUM 128

/* mouse mode defines */
#define MBV_MOUSE_MOVE 0
#define MBV_MOUSE_ROTATE 1
#define MBV_MOUSE_SHADE 2
#define MBV_MOUSE_VIEWPOINT 3
#define MBV_MOUSE_AREA 4
#define MBV_MOUSE_SITE 5
#define MBV_MOUSE_ROUTE 6
#define MBV_MOUSE_NAV 7
#define MBV_MOUSE_NAVFILE 8
#define MBV_MOUSE_VECTOR 9

/* projection mode */
#define MBV_PROJECTION_GEOGRAPHIC 0
#define MBV_PROJECTION_PROJECTED 1
#define MBV_PROJECTION_ALREADYPROJECTED 2
#define MBV_PROJECTION_SPHEROID 3
#define MBV_PROJECTION_ELLIPSOID 4

/* display mode defines */
#define MBV_DISPLAY_2D 0
#define MBV_DISPLAY_3D 1

/* data type defines */
#define MBV_DATA_PRIMARY 0
#define MBV_DATA_PRIMARYSLOPE 1
#define MBV_DATA_SECONDARY 2

/* grid view mode defines */
#define MBV_GRID_VIEW_PRIMARY 0
#define MBV_GRID_VIEW_PRIMARYSLOPE 1
#define MBV_GRID_VIEW_SECONDARY 2

/* shade view mode defines */
#define MBV_SHADE_VIEW_NONE 0
#define MBV_SHADE_VIEW_ILLUMINATION 1
#define MBV_SHADE_VIEW_SLOPE 2
#define MBV_SHADE_VIEW_OVERLAY 3

/* simple view mode defines for contours, sites, routes, etc */
#define MBV_VIEW_OFF 0
#define MBV_VIEW_ON 1

/* lon lat style mode */
#define MBV_LONLAT_DEGREESDECIMAL 0
#define MBV_LONLAT_DEGREESMINUTES 1

/* colortable view mode defines */
#define MBV_COLORTABLE_NORMAL 0
#define MBV_COLORTABLE_REVERSED 1

/* colortable view mode defines */
#define MBV_COLORTABLE_HAXBY 0
#define MBV_COLORTABLE_BRIGHT 1
#define MBV_COLORTABLE_MUTED 2
#define MBV_COLORTABLE_GRAY 3
#define MBV_COLORTABLE_FLAT 4
#define MBV_COLORTABLE_SEALEVEL1 5
#define MBV_COLORTABLE_SEALEVEL2 6

/* individual color defines */
#define MBV_COLOR_BLACK 0
#define MBV_COLOR_WHITE 1
#define MBV_COLOR_RED 2
#define MBV_COLOR_YELLOW 3
#define MBV_COLOR_GREEN 4
#define MBV_COLOR_BLUEGREEN 5
#define MBV_COLOR_BLUE 6
#define MBV_COLOR_PURPLE 7

/* default no data value define */
#define MBV_DEFAULT_NODATA -9999999.9

/* selection defines */
#define MBV_SELECT_NONE -1
#define MBV_SELECT_ALL -2

/* pick defines */
#define MBV_PICK_NONE 0
#define MBV_PICK_ONEPOINT 1
#define MBV_PICK_TWOPOINT 2
#define MBV_PICK_AREA 3
#define MBV_PICK_REGION 4
#define MBV_PICK_SITE 5
#define MBV_PICK_ROUTE 6
#define MBV_PICK_NAV 7
#define MBV_PICK_VECTOR 8

/* region defines */
#define MBV_REGION_REPICKWIDTH 2
#define MBV_REGION_NONE 0
#define MBV_REGION_ONEPOINT 1
#define MBV_REGION_QUAD 2
#define MBV_REGION_PICKCORNER0 0
#define MBV_REGION_PICKCORNER1 1
#define MBV_REGION_PICKCORNER2 2
#define MBV_REGION_PICKCORNER3 3

/* area defines */
#define MBV_AREA_REPICKWIDTH 2
#define MBV_AREA_NONE 0
#define MBV_AREA_ONEPOINT 1
#define MBV_AREA_QUAD 2
#define MBV_AREA_PICKENDPOINT0 0
#define MBV_AREA_PICKENDPOINT1 1

/* site defines */
#define MBV_SITE_OFF 0
#define MBV_SITE_VIEW 1
#define MBV_SITE_EDIT 2

/* route defines */
#define MBV_ROUTE_OFF 0
#define MBV_ROUTE_VIEW 1
#define MBV_ROUTE_EDIT 2
#define MBV_ROUTE_NAVADJUST 3
#define MBV_ROUTE_WAYPOINT_DELETEFLAG -1
#define MBV_ROUTE_WAYPOINT_NONE 0
#define MBV_ROUTE_WAYPOINT_SIMPLE 1
#define MBV_ROUTE_WAYPOINT_TRANSIT 2
#define MBV_ROUTE_WAYPOINT_STARTLINE 3
#define MBV_ROUTE_WAYPOINT_ENDLINE 4
#define MBV_ROUTE_WAYPOINT_STARTLINE2 5
#define MBV_ROUTE_WAYPOINT_ENDLINE2 6
#define MBV_ROUTE_WAYPOINT_STARTLINE3 7
#define MBV_ROUTE_WAYPOINT_ENDLINE3 8
#define MBV_ROUTE_WAYPOINT_STARTLINE4 9
#define MBV_ROUTE_WAYPOINT_ENDLINE4 10
#define MBV_ROUTE_WAYPOINT_STARTLINE5 11
#define MBV_ROUTE_WAYPOINT_ENDLINE5 12

/* nav defines */
#define MBV_NAV_OFF 0
#define MBV_NAV_VIEW 1
#define MBV_NAV_MBNAVADJUST 2

/* vector defines */
#define MBV_VECTOR_OFF 0
#define MBV_VECTOR_VIEW 1

/* stat masks */
#define MBV_STATMASK0 0x01
#define MBV_STATMASK1 0x02
#define MBV_STATMASK2 0x04
#define MBV_STATMASK3 0x08
#define MBV_STATMASK4 0x10
#define MBV_STATMASK5 0x20
#define MBV_STATMASK6 0x40
#define MBV_STATMASK7 0x80

/* pick sensitivity masks */
#define MBV_PICKMASK_NONE 0x0
#define MBV_PICKMASK_ONEPOINT 0x1
#define MBV_PICKMASK_TWOPOINT 0x2
#define MBV_PICKMASK_AREA 0x4
#define MBV_PICKMASK_REGION 0x8
#define MBV_PICKMASK_SITE 0x10
#define MBV_PICKMASK_ROUTE 0x20
#define MBV_PICKMASK_NAVONEPOINT 0x40
#define MBV_PICKMASK_NAVTWOPOINT 0x80
#define MBV_PICKMASK_NAVANY 0x100
#define MBV_PICKMASK_NEWINSTANCE 0x200
#define MBV_EXISTMASK_SITE 0x400
#define MBV_EXISTMASK_ROUTE 0x800
#define MBV_EXISTMASK_NAV 0x1000
#define MBV_STATEMASK_13 0x2000
#define MBV_STATEMASK_14 0x4000
#define MBV_STATEMASK_15 0x8000
#define MBV_STATEMASK_16 0x10000
#define MBV_STATEMASK_17 0x20000
#define MBV_STATEMASK_18 0x40000
#define MBV_STATEMASK_19 0x80000
#define MBV_STATEMASK_20 0x100000
#define MBV_STATEMASK_21 0x200000
#define MBV_STATEMASK_22 0x400000
#define MBV_STATEMASK_23 0x800000
#define MBV_STATEMASK_24 0x1000000
#define MBV_STATEMASK_25 0x2000000
#define MBV_STATEMASK_26 0x4000000
#define MBV_STATEMASK_27 0x8000000
#define MBV_STATEMASK_28 0x10000000
#define MBV_STATEMASK_29 0x20000000
#define MBV_STATEMASK_30 0x40000000
#define MBV_STATEMASK_31 0x80000000

/* profile defines */
#define MBV_PROFILE_NONE 0
#define MBV_PROFILE_TWOPOINT 1
#define MBV_PROFILE_ROUTE 2
#define MBV_PROFILE_NAV 3
#define MBV_PROFILE_FACTOR_MAX 4.0

/* mb3dsounding mouse mode value defines */
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

/*--------------------------------------------------------------------*/

/* structure declarations */
struct mbview_contoursegment_struct {
	float x[2];
	float y[2];
	float z;
	float level;
};

struct mbview_point_struct {
	double xgrid;
	double ygrid;
	double xlon;
	double ylat;
	double zdata;
	double xdisplay;
	double ydisplay;
	double zdisplay;
};

struct mbview_pointw_struct {
	double xgrid[MBV_MAX_WINDOWS];
	double ygrid[MBV_MAX_WINDOWS];
	double xlon;
	double ylat;
	double zdata;
	double xdisplay[MBV_MAX_WINDOWS];
	double ydisplay[MBV_MAX_WINDOWS];
	double zdisplay[MBV_MAX_WINDOWS];
};

struct mbview_navpoint_struct {
	int draped;
	int selected;
	double time_d;
	double heading;
	double speed;
	struct mbview_point_struct point;
	struct mbview_point_struct pointport;
	struct mbview_point_struct pointcntr;
	struct mbview_point_struct pointstbd;
	int line;
	int shot;
	int cdp;
};

struct mbview_navpointw_struct {
	int draped;
	int selected;
	double time_d;
	double heading;
	double speed;
	struct mbview_pointw_struct point;
	struct mbview_pointw_struct pointport;
	struct mbview_pointw_struct pointcntr;
	struct mbview_pointw_struct pointstbd;
	int line;
	int shot;
	int cdp;
};

struct mbview_vectorpointw_struct {
	int draped;
	int selected;
	struct mbview_pointw_struct point;
	double data;
};

struct mbview_profilepoint_struct {
	int boundary;
	double xgrid;
	double ygrid;
	double xlon;
	double ylat;
	double zdata;
	double distance;
	double distovertopo;
	double xdisplay;
	double ydisplay;
	double navzdata;
	double navtime_d;
	double slope;
	double bearing;
};

struct mbview_linesegment_struct {
	struct mbview_point_struct endpoints[2];
	int nls;
	int nls_alloc;
	struct mbview_point_struct *lspoints;
};

struct mbview_linesegmentw_struct {
	struct mbview_pointw_struct endpoints[2];
	int nls;
	int nls_alloc;
	struct mbview_pointw_struct *lspoints;
};

struct mbview_pick_struct {
	double range;
	double bearing;
	struct mbview_point_struct endpoints[2];
	struct mbview_point_struct xpoints[8];
	struct mbview_linesegment_struct segment;
	struct mbview_linesegment_struct xsegments[4];
};

struct mbview_pickw_struct {
	struct mbview_pointw_struct endpoints[2];
	struct mbview_pointw_struct xpoints[8];
	struct mbview_linesegmentw_struct segment;
	struct mbview_linesegmentw_struct xsegments[4];
};

struct mbview_region_struct {
	double width;
	double height;
	struct mbview_point_struct cornerpoints[4];
	struct mbview_linesegment_struct segments[4];
};

struct mbview_area_struct {
	double width;
	double length;
	double bearing;
	struct mbview_point_struct endpoints[2];
	struct mbview_linesegment_struct segment;
	struct mbview_point_struct cornerpoints[4];
	struct mbview_linesegment_struct segments[4];
};

struct mbview_site_struct {
  bool active;
	struct mbview_pointw_struct point;
	int color;
	int size;
	mb_path name;
};

struct mbview_route_struct {
  bool active;
	int color;
	int size;
	int editmode;
	mb_path name;
	double distancelateral;
	double distancetopo;
	int npoints;
	int npoints_alloc;
	int nroutepoint;
	int *waypoint;
	double *distlateral;
	double *disttopo;
	struct mbview_pointw_struct *points;
	struct mbview_linesegmentw_struct *segments;
};

struct mbview_nav_struct {
  bool active;
	int color;
	int size;
	mb_path name;
	int pathstatus;
	mb_path pathraw;
	mb_path pathprocessed;
	int format;
	int swathbounds;
	int line;
	int shot;
	int cdp;
	int decimation;
	int npoints;
	int npoints_alloc;
	int nselected;
	struct mbview_navpointw_struct *navpts;
	struct mbview_linesegmentw_struct *segments;
};

struct mbview_vector_struct {
  bool active;
	int color;
	int size;
	mb_path name;
	int format;
	int npoints;
	int npoints_alloc;
	int nselected;
	double datamin;
	double datamax;
	struct mbview_vectorpointw_struct *vectorpts;
	struct mbview_linesegmentw_struct *segments;
};

struct mbview_profile_struct {
	int source;
	mb_path source_name;
	double length;
	double zmin;
	double zmax;
	int npoints;
	int npoints_alloc;
	struct mbview_profilepoint_struct *points;
};

struct mbview_shareddata_struct {
	/* nav pick */
	int navpick_type;
	struct mbview_pickw_struct navpick;

	/* site data */
	int site_mode;
	int nsite;
	int nsite_alloc;
	int site_selected;
	struct mbview_site_struct *sites;

	/* route data */
	int route_mode;
	int nroute;
	int nroute_alloc;
	int route_selected;
	int route_point_selected;
	struct mbview_route_struct *routes;

	/* nav data */
	int nav_mode;
	int nnav;
	int nnav_alloc;
	int nav_selected[2];
	int nav_point_selected[2];
	int nav_selected_mbnavadjust[2];
	struct mbview_nav_struct *navs;

	/* vector data */
	int vector_mode;
	int nvector;
	int nvector_alloc;
	int vector_selected;
	int vector_point_selected;
	struct mbview_vector_struct *vectors;
};

struct mbview_struct {

	/* function pointers */
	int (*mbview_dismiss_notify)(size_t id);
	void (*mbview_pickonepoint_notify)(size_t id);
	void (*mbview_picktwopoint_notify)(size_t id);
	void (*mbview_pickarea_notify)(size_t id);
	void (*mbview_pickregion_notify)(size_t id);
	void (*mbview_picksite_notify)(size_t id);
	void (*mbview_pickroute_notify)(size_t id);
	void (*mbview_picknav_notify)(size_t id);
	void (*mbview_pickvector_notify)(size_t id);
	void (*mbview_sensitivity_notify)(void);
	void (*mbview_colorchange_notify)(size_t id);

	/* active flag */
	int active;

	/* main plot widget controls */
	mb_path title;
	int xo;
	int yo;
	int width;
	int height;
	int lorez_dimension;
	int hirez_dimension;
	int lorez_navdecimate;
	int hirez_navdecimate;

	/* profile plot widget controls */
	mb_path prtitle;
	int prwidth;
	int prheight;

	/* mode controls */
	int display_mode;
	int mouse_mode;
	int grid_mode;
	int grid_shade_mode;
	int grid_contour_mode;

	/* histogram equalization controls */
	int primary_histogram;
	int primaryslope_histogram;
	int secondary_histogram;

	/* colortable controls */
	int primary_colortable;
	int primary_colortable_mode;
	double primary_colortable_min;
	double primary_colortable_max;
	int primary_shade_mode;
	int slope_colortable;
	int slope_colortable_mode;
	double slope_colortable_min;
	double slope_colortable_max;
	int slope_shade_mode;
	int secondary_colortable;
	int secondary_colortable_mode;
	double secondary_colortable_min;
	double secondary_colortable_max;
	int secondary_shade_mode;

	/* view controls */
	double exaggeration;
	double modelelevation3d;
	double modelazimuth3d;
	double viewelevation3d;
	double viewazimuth3d;
	int viewbounds[4];

	/* shading controls */
	double illuminate_magnitude;
	double illuminate_elevation;
	double illuminate_azimuth;
	double slope_magnitude;
	double overlay_shade_magnitude;
	double overlay_shade_center;
	int overlay_shade_mode;

	/* contour controls */
	double contour_interval;

	/* profile controls */
	double profile_exaggeration;
	int profile_widthfactor;
	double profile_slopethreshold;

	/* projection controls */
	int primary_grid_projection_mode;
	mb_path primary_grid_projection_id;
	int secondary_grid_projection_mode;
	mb_path secondary_grid_projection_id;
	int display_projection_mode;
	mb_path display_projection_id;

	/* grid data */
	float primary_nodatavalue;
	int primary_nxy;
	int primary_n_columns;
	int primary_n_rows;
	double primary_min;
	double primary_max;
	double primary_xmin;
	double primary_xmax;
	double primary_ymin;
	double primary_ymax;
	double primary_dx;
	double primary_dy;
	float *primary_data;
	float *primary_x;
	float *primary_y;
	float *primary_z;
	float *primary_dzdx;
	float *primary_dzdy;
	float *primary_r;
	float *primary_g;
	float *primary_b;
	char *primary_stat_color;
	char *primary_stat_z;
	bool secondary_sameas_primary;
	float secondary_nodatavalue;
	int secondary_nxy;
	int secondary_n_columns;
	int secondary_n_rows;
	double secondary_min;
	double secondary_max;
	double secondary_xmin;
	double secondary_xmax;
	double secondary_ymin;
	double secondary_ymax;
	double secondary_dx;
	double secondary_dy;
	float *secondary_data;

	/* pick info flag */
	int pickinfo_mode;

	/* point and line pick */
	int pick_type;
	struct mbview_pick_struct pick;

	/* area data */
	int area_type;
	int area_pickendpoint;
	struct mbview_area_struct area;

	/* region data */
	int region_type;
	int region_pickcorner;
	struct mbview_region_struct region;

	/* profile data */
	struct mbview_profile_struct profile;

	/* global data view modes */
	int site_view_mode;
	int route_view_mode;
	int nav_view_mode;
	int navswathbounds_view_mode;
	int navdrape_view_mode;
	int vector_view_mode;
	int profile_view_mode;

  /* general use state variables to turn action button sensitivity on and off */
  int state13;
  int state14;
  int state15;
  int state16;
  int state17;
  int state18;
  int state19;
  int state20;
  int state21;
  int state22;
  int state23;
  int state24;
  int state25;
  int state26;
  int state27;
  int state28;
  int state29;
  int state30;
  int state31;
};

/*--------------------------------------------------------------------*/

/* ---- mbviewprivate.h 5.8.3, the members the data code reads -------------------------- */
#define MBV_OPENGL_WIDTH 3.0
#define MBV_OPENGL_3D_CONTOUR_OFFSET 0.001
#define MBV_WINDOW_NULL 0
#define MBV_WINDOW_HIDDEN 1
#define MBV_WINDOW_VISIBLE 2
#define MBV_SPHEROID_RADIUS 6371000.0

struct mbview_shared_struct {
	/* global lon lat print style */
	int lonlatstyle;

	/* pointer to structure holding global data */
	struct mbview_shareddata_struct shareddata;
};

struct mbview_world_struct {
	/* flag if this instance is initialized */
	int init;

	/* pointer to structure holding data to be rendered */
	struct mbview_struct data;

	/* drawing state the data code tests: only "is this view live" survives the port */
	void *dpy;
	bool glx_init;

	/* projection parameters */
	bool primary_pj_init;
	void *primary_pjptr;
	bool secondary_pj_init;
	void *secondary_pjptr;
	bool display_pj_init;
	void *display_pjptr;
	double sphere_reflon;
	double sphere_reflat;
	double sphere_refx;
	double sphere_refy;
	double sphere_refz;
	double sphere_eulerforward[9];
	double sphere_eulerreverse[9];
	double mtodeglon;
	double mtodeglat;

	/* view parameters */
	double xmin;
	double xmax;
	double ymin;
	double ymax;
	double xorigin;
	double yorigin;
	double zorigin;
	double scale;
	double aspect_ratio;
	float areaaspect;
};

#ifdef __cplusplus
extern "C" {
#endif

/* libmbview's globals, under this port's names (include system headers BEFORE this file) */
#define mbv_verbose mbgv_verbose
#define shared      mbgv_shared
#define mbviews     mbgv_views
extern int mbv_verbose;
extern struct mbview_shared_struct shared;
extern struct mbview_world_struct mbviews[MBV_MAX_WINDOWS];

/* ---- libmbview's functions, as the ported files define them ------------------------- */
int mbview_addnav(int verbose, size_t instance, int npoint, double *time_d, double *navlon, double *navlat, double *navz,
                  double *heading, double *speed, double *navportlon, double *navportlat, double *navstbdlon, double *navstbdlat,
                  unsigned int *line, unsigned int *shot, unsigned int *cdp, int navcolor, int navsize, mb_path navname, int navpathstatus,
                  mb_path navpathraw, mb_path navpathprocessed, int navformat, bool navswathbounds, bool navline, bool navshot,
                  bool navcdp, int decimation, int *error);
int mbview_addroute(int verbose, size_t instance, int npoint, double *routelon, double *routelat, int *waypoint, int routecolor,
                    int routesize, int routeeditmode, mb_path routename, int *iroute, int *error);
int mbview_addsites(int verbose, size_t instance, int nsite, double *sitelon, double *sitelat, double *sitetopo, int *sitecolor,
                    int *sitesize, mb_path *sitename, int *error);
int mbview_addvector(int verbose, size_t instance, int npoint, double *veclon, double *veclat, double *vecz, double *vecdata,
                     int veccolor, int vecsize, mb_path vecname, double vecdatamin, double vecdatamax, int *error);
int mbview_allocnavarrays(int verbose, int npointtotal, double **time_d, double **navlon, double **navlat, double **navz,
                          double **heading, double **speed, double **navportlon, double **navportlat, double **navstbdlon,
                          double **navstbdlat, int **line, int **shot, int **cdp, int *error);
int mbview_allocprofilearrays(int verbose, int npoints, double **distance, double **zdata, int **boundary, double **xlon,
                              double **ylat, double **distovertopo, double **bearing, double **slope, int *error);
int mbview_allocprofilepoints(int verbose, int npoints, struct mbview_profilepoint_struct **points, int *error);
int mbview_allocroutearrays(int verbose, int npointtotal, double **routelon, double **routelat, int **waypoint,
                            double **routetopo, double **routebearing, double **distlateral, double **distovertopo,
                            double **slope, int *error);
int mbview_allocsitearrays(int verbose, int nsite, double **sitelon, double **sitelat, double **sitetopo, int **sitecolor,
                           int **sitesize, mb_path **sitename, int *error);
int mbview_allocvectorarrays(int verbose, int npointtotal, double **veclon, double **veclat, double **vecz, double **vecdata,
                             int *error);
int mbview_deleteallroutes(int verbose, size_t instance, int *error);
int mbview_deleteroute(int verbose, size_t instance, int iroute, int *error);
int mbview_drapesegment(size_t instance, struct mbview_linesegment_struct *seg);
int mbview_drapesegment_gc(size_t instance, struct mbview_linesegment_struct *seg);
int mbview_drapesegment_grid(size_t instance, struct mbview_linesegment_struct *seg);
int mbview_drapesegmentw(size_t instance, struct mbview_linesegmentw_struct *seg);
int mbview_drapesegmentw_gc(size_t instance, struct mbview_linesegmentw_struct *seg);
int mbview_drapesegmentw_grid(size_t instance, struct mbview_linesegmentw_struct *seg);
int mbview_enableeditroutes(int verbose, size_t instance, int *error);
int mbview_enableeditsites(int verbose, size_t instance, int *error);
int mbview_enableviewnavs(int verbose, size_t instance, int *error);
int mbview_enableviewroutes(int verbose, size_t instance, int *error);
int mbview_enableviewsites(int verbose, size_t instance, int *error);
int mbview_enableviewvectors(int verbose, size_t instance, int *error);
int mbview_extract_pick_profile(size_t instance);
int mbview_extract_route_profile(size_t instance);
int mbview_freenavarrays(int verbose, double **time_d, double **navlon, double **navlat, double **navz, double **heading,
                         double **speed, double **navportlon, double **navportlat, double **navstbdlon, double **navstbdlat,
                         int **line, int **shot, int **cdp, int *error);
int mbview_freeprofilearrays(int verbose, double **distance, double **zdata, int **boundary, double **xlon, double **ylat,
                             double **distovertopo, double **bearing, double **slope, int *error);
int mbview_freeprofilepoints(int verbose, double **points, int *error);
int mbview_freeroutearrays(int verbose, double **routelon, double **routelat, int **waypoint, double **routetopo,
                           double **routebearing, double **distlateral, double **distovertopo, double **slope, int *error);
int mbview_freesitearrays(int verbose, double **sitelon, double **sitelat, double **sitetopo, int **sitecolor, int **sitesize,
                          mb_path **sitename, int *error);
int mbview_freevectorarrays(int verbose, double **veclon, double **veclat, double **vecz, double **vecdata, int *error);
int mbview_getdataptr(int verbose, size_t instance, struct mbview_struct **datahandle, int *error);
int mbview_getnavcount(int verbose, size_t instance, int *nnav, int *error);
int mbview_getnavpointcount(int verbose, size_t instance, int nav, int *npoint, int *nintpoint, int *error);
int mbview_getprofile(int verbose, size_t instance, mb_path source_name, double *length, double *zmin, double *zmax, int *npoints,
                      double *distance, double *zdata, int *boundary, double *xlon, double *ylat, double *distovertopo,
                      double *bearing, double *slope, int *error);
int mbview_getprofilecount(int verbose, size_t instance, int *npoints, int *error);
int mbview_getroute(int verbose, size_t instance, int route, int *npointtotal, double *routelon, double *routelat, int *waypoint,
                    double *routetopo, double *routebearing, double *distlateral, double *distovertopo, double *slope,
                    int *routecolor, int *routesize, int *routeeditmode, mb_path routename, int *error);
int mbview_getroutecount(int verbose, size_t instance, int *nroute, int *error);
int mbview_getrouteinfo(int verbose, size_t instance, int working_route, int *nroutewaypoint, int *nroutpoint, char *routename,
                        int *routecolor, int *routesize, double *routedistancelateral, double *routedistancetopo, int *error);
int mbview_getroutepointcount(int verbose, size_t instance, int route, int *npoint, int *nintpoint, int *error);
int mbview_getrouteselected(int verbose, size_t instance, int route, bool *selected, int *error);
int mbview_getsharedptr(int verbose, struct mbview_shareddata_struct **sharedhandle, int *error);
int mbview_getsitecount(int verbose, size_t instance, int *nsite, int *error);
int mbview_getsites(int verbose, size_t instance, int *nsite, double *sitelon, double *sitelat, double *sitetopo, int *sitecolor,
                    int *sitesize, mb_path *sitename, int *error);
int mbview_getvectorcount(int verbose, size_t instance, int *nvec, int *error);
int mbview_getvectorpointcount(int verbose, size_t instance, int vec, int *npoint, int *nintpoint, int *error);
int mbview_getzdata(size_t instance, double xgrid, double ygrid, bool *found, double *zdata);
int mbview_greatcircle_dist(size_t instance, double lon1, double lat1, double lon2, double lat2, double *distance);
int mbview_greatcircle_distbearing(size_t instance, double lon1, double lat1, double lon2, double lat2, double *bearing,
                                   double *distance);
int mbview_greatcircle_endposition(size_t instance, double lon1, double lat1, double bearing, double distance, double *lon2,
                                   double *lat2);
int mbview_nav_delete(size_t instance, int inav);
int mbview_pick_text(size_t instance);
int mbview_projectdisplay2ll(size_t instance, double xdisplay, double ydisplay, double zdisplay, double *xlon, double *ylat);
int mbview_projectdistance(size_t instance, double xlon1, double ylat1, double zdata1, double xlon2, double ylat2, double zdata2,
                           double *distancelateral, double *distanceoverground, double *slope);
int mbview_projectforward(size_t instance, bool needlonlat, double xgrid, double ygrid, double zdata, double *xlon, double *ylat,
                          double *xdisplay, double *ydisplay, double *zdisplay);
int mbview_projectfromlonlat(size_t instance, double xlon, double ylat, double zdata, double *xgrid, double *ygrid,
                             double *xdisplay, double *ydisplay, double *zdisplay);
int mbview_projectgrid2ll(size_t instance, double xgrid, double ygrid, double *xlon, double *ylat);
int mbview_projectinverse(size_t instance, bool needlonlat, double xdisplay, double ydisplay, double zdisplay, double *xlon,
                          double *ylat, double *xgrid, double *ygrid);
int mbview_projectll2display(size_t instance, double xlon, double ylat, double zdata, double *xdisplay, double *ydisplay,
                             double *zdisplay);
int mbview_projectll2xygrid(size_t instance, double xlon, double ylat, double *xgrid, double *ygrid);
int mbview_projectll2xyzgrid(size_t instance, double xlon, double ylat, double *xgrid, double *ygrid, double *zdata);
int mbview_route_add(int verbose, size_t instance, int inew, int jnew, int waypoint, double xgrid, double ygrid, double xlon,
                     double ylat, double zdata, double xdisplay, double ydisplay, double zdisplay);
int mbview_route_delete(size_t instance, int iroute, int ipoint);
int mbview_route_setdistance(size_t instance, int working_route);
int mbview_site_delete(size_t instance, int isite);
int mbview_sphere_forward(size_t instance, double xlon, double ylat, double *xx, double *yy, double *zz);
int mbview_sphere_inverse(size_t instance, double xx, double yy, double zz, double *xlon, double *ylat);
int mbview_sphere_matrix(double phi, double theta, double psi, double *eulermatrix);
int mbview_sphere_rotate(double *eulermatrix, double *v, double *vr);
int mbview_sphere_setup(size_t instance, bool earthcentered, double xlon, double ylat);
int mbview_update(int verbose, size_t instance, int *error);
int mbview_updatenavlist(void);
int mbview_updatepointw(size_t instance, struct mbview_pointw_struct *pointw);
int mbview_updateroutelist(void);
int mbview_updatesegmentw(size_t instance, struct mbview_linesegmentw_struct *segmentw);
int mbview_updatesitelist(void);
int mbview_vector_delete(size_t instance, int ivec);
int mbview_zscalepoint(size_t instance, int globalview, double offset_factor, struct mbview_point_struct *point);
int mbview_zscalepointw(size_t instance, int globalview, double offset_factor, struct mbview_pointw_struct *pointw);

/* ---- the port's own: what libmbview did through its widgets ------------------------- */
/* Set up view `instance` on a primary grid (mbview_setprimarygrid + mbview_projectdata, data part):
   z is copied, column-major z[i*n_rows+j] from the south-west node; NaN nodes become nodatavalue. */
int mbgv_setup(size_t instance, const char *title, int grid_projection_mode, const char *grid_projection_id,
               int display_projection_mode, const char *display_projection_id, const float *z, int n_columns,
               int n_rows, double xmin, double xmax, double ymin, double ymax, double dx, double dy);
/* Forget view `instance` (its grid and projections; the shared sites, routes and navigation stay). */
void mbgv_release(size_t instance);
/* The area pick from its two centre-line ends in grid coordinates and its width in metres (mbview_area's
   quad recalculation, verbatim), or the region pick from two opposite corners (grid coordinates). */
int mbgv_set_area(size_t instance, double xgrid0, double ygrid0, double xgrid1, double ygrid1, double width);
int mbgv_set_region(size_t instance, double xgrid0, double ygrid0, double xgrid1, double ygrid1);
/* The status line mbview shows in its window (do_mbview_message_on), given to the host. */
void do_mbview_message_on(char *message, size_t instance);
extern void (*mbgv_message_hook)(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* MBGRDVIZ_MBVIEW_H_ */
