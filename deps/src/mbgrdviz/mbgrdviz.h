/*======================================================================================
 * mbgrdviz.h -- the plain interface of the mbgrdviz port's engine (mbgrdviz.c, which carries
 * MB-System's mbgrdviz_callbacks.c) and of its libmbview data half (mbgrdviz_mbview.c), for the
 * window that drives them (mbgrdviz_window.cpp). Only plain types cross it: libmbview's
 * structures stay on the C side.
 *
 * A "view" (0 .. MBGRDVIZ_MAX_VIEWS-1) is what libmbview calls an instance: one InteractiveGMT
 * window bound to the tool, with its grid. Sites, routes, navigation and vectors are libmbview's
 * SHARED data, as in mbgrdviz; grid coordinates are the view's grid's own (lon/lat for a
 * geographic grid, its projected x/y otherwise).
 *====================================================================================*/

#ifndef MBGRDVIZ_H_
#define MBGRDVIZ_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBGRDVIZ_MAX_VIEWS 10

/* what the engine opens (mbgrdviz_open) */
#define MBGRDVIZ_OPEN_SITE 2
#define MBGRDVIZ_OPEN_ROUTE 3
#define MBGRDVIZ_OPEN_VECTOR 4
#define MBGRDVIZ_OPEN_NAV 5
#define MBGRDVIZ_OPEN_SWATH 6

/* what the engine writes (mbgrdviz_save): mbgrdviz's own fileSelectionBox mode numbers */
#define MBGRDVIZ_SAVE_ROUTE 7
#define MBGRDVIZ_SAVE_ROUTEREVERSED 8
#define MBGRDVIZ_SAVE_RISISCRIPTHEADING 9
#define MBGRDVIZ_SAVE_RISISCRIPTNOHEADING 10
#define MBGRDVIZ_SAVE_RISI2SCRIPTHEADING 11
#define MBGRDVIZ_SAVE_RISI2SCRIPTNOHEADING 12
#define MBGRDVIZ_SAVE_DEGDECMIN 13
#define MBGRDVIZ_SAVE_LNW 14
#define MBGRDVIZ_SAVE_GREENSEAYML 15
#define MBGRDVIZ_SAVE_TECDISLST 16
#define MBGRDVIZ_SAVE_KONGSBERGDP 17
#define MBGRDVIZ_SAVE_SISASCIIPLAN1 18
#define MBGRDVIZ_SAVE_SISASCIIPLAN2 19
#define MBGRDVIZ_SAVE_SITE 20
#define MBGRDVIZ_SAVE_PROFILE 21

/* libmbview's waypoint kinds (MBV_ROUTE_WAYPOINT_*) and colours (MBV_COLOR_*) */
#define MBGRDVIZ_WAYPOINT_SIMPLE 1
#define MBGRDVIZ_NCOLORS 8

/* mbgrdviz's survey planning parameters (the "Generate Survey Route from Area" dialog) */
struct mbgrdviz_survey {
	int mode;            /* 0 uniform line spacing, 1 variable (from the swath width and the topography) */
	int platform;        /* 0 surface, 1 submerged at constant altitude, 2 submerged at constant depth */
	int interleaving;    /* lines per group */
	int direction;       /* first line from the 0 SW, 1 SE, 2 NW, 3 NE corner */
	int crosslines_last; /* 0 crosslines first, 1 last */
	int crosslines;      /* number of crosslines */
	int linespacing;     /* m */
	int swathwidth;      /* degrees */
	int depth;           /* m */
	int altitude;        /* m */
	int color;           /* 0 black, 1 yellow, 2 green, 3 bluegreen, 4 blue, 5 purple */
	char name[1024];
};

/* MBIO: the extra entry points from the library the swath editor loaded; 1 = ready */
int mbgrdviz_mbio_open(char *msg, int msglen);

/* MBIO's projection, for other ports (pointCloudEditor's UTM display): mb_proj_init / mb_proj_free and
   the forward transform, through the guard against MBIO's uninitialized PJ_COORD (mbgrdviz.c).
   mbgrdviz_mbio_open must have succeeded. 1 = done */
int mbgrdviz_proj_init(const char *projection, void **pj);
void mbgrdviz_proj_free(void **pj);
int mbgrdviz_proj_forward(void *pj, double lon, double lat, double *easting, double *northing);

/* the status line the engine writes while it reads (mbview's message), or NULL */
void mbgrdviz_set_message_hook(void (*hook)(const char *message));

/* bind a view to a grid: z column-major z[i*ny+j] from the south-west node, NaN = no data; `crs`
   = the grid's projection for MBIO (e.g. "EPSG:32629"), ignored when geographic. The display
   projection is chosen as mbgrdviz chooses it. 1 = ready */
int mbgrdviz_view_setup(int view, const char *title, int geographic, const char *crs, const float *z, int nx, int ny,
                        double x0, double x1, double y0, double y1, double dx, double dy);
void mbgrdviz_view_release(int view);
int mbgrdviz_view_ready(int view);

/* the engine's file operations; 1 = done */
int mbgrdviz_open(int view, int what, const char *path);
int mbgrdviz_save(int view, int what, const char *path);

/* routes (libmbview's shared list) */
int mbgrdviz_route_count(void);
int mbgrdviz_route_info(int route, char *name, int namelen, int *color, int *size, int *nwaypoint);
/* the waypoints (not the draped points between them) in the view's grid coordinates, with their topo */
int mbgrdviz_route_waypoints(int view, int route, double *xgrid, double *ygrid, double *z, int *waypoint, int n);
/* a new route from waypoints in grid coordinates (waypoint kinds may be NULL: all simple); its index */
int mbgrdviz_route_add(int view, const char *name, int color, int size, const double *xgrid, const double *ygrid,
                       const int *waypoint, int n);
void mbgrdviz_routes_clear(int view);
int mbgrdviz_route_rename(int route, const char *name);
void mbgrdviz_route_select(int route);   /* -1: none (the writers then take every route) */
int mbgrdviz_route_distances(int view, int route, double *lateral, double *overtopo);

/* sites (libmbview's shared list) */
int mbgrdviz_site_count(void);
int mbgrdviz_site_get(int view, int site, char *name, int namelen, double *xgrid, double *ygrid, double *z, int *color,
                      int *size);
/* replace every site: names[i] may be empty (libmbview then names it "Site i") */
int mbgrdviz_sites_set(int view, int n, const double *xgrid, const double *ygrid, const int *color, const int *size,
                       const char *const *names);

/* navigation (libmbview's shared list) */
int mbgrdviz_nav_count(void);
int mbgrdviz_nav_info(int nav, char *name, int namelen, char *pathraw, int pathlen, int *format, int *npoints,
                      int *swathbounds, int *nselected);
int mbgrdviz_nav_points(int view, int nav, double *xgrid, double *ygrid, double *z, double *portx, double *porty,
                        double *stbdx, double *stbdy, int n);
void mbgrdviz_nav_select(int nav, int selected);   /* every point of the line, or none */

/* vectors (libmbview's shared list) */
int mbgrdviz_vector_count(void);
int mbgrdviz_vector_info(int vec, char *name, int namelen, int *npoints, double *datamin, double *datamax);
int mbgrdviz_vector_points(int view, int vec, double *xgrid, double *ygrid, double *z, double *data, int n);

/* the area (centre line ends + width in m) and the region (two opposite corners), grid coordinates */
int mbgrdviz_set_area(int view, double x0, double y0, double x1, double y1, double width);
int mbgrdviz_set_region(int view, double x0, double y0, double x1, double y1);
/* the area's corners in grid coordinates (x[4], y[4]), its length, width and bearing */
int mbgrdviz_area_get(int view, double *x, double *y, double *length, double *width, double *bearing);

/* survey planning */
void mbgrdviz_survey_get(struct mbgrdviz_survey *p);
void mbgrdviz_survey_set(const struct mbgrdviz_survey *p);
int mbgrdviz_generate_survey(int view);          /* the route it made (or replaced), or -1 */
void mbgrdviz_survey_dismiss(void);              /* the dialog closed: the next survey is a new route */
/* the route the open dialog generated last, found again after the route list was rebuilt from the
   window (-1: none); Generate Route replaces it, as mbgrdviz does */
void mbgrdviz_survey_set_working(int route);
int mbgrdviz_survey_info(int view, char *text, int len);   /* the dialog's info lines */

/* the profile along a route (as picking it in mbview does); its point count */
int mbgrdviz_route_profile(int view, int route);
int mbgrdviz_profile_get(int view, double *distance, double *z, int n, double *length, double *zmin, double *zmax);

/* "Open Region as New View": the region's sub-grid of the view's grid, as do_mbgrdviz_open_region
   extracts it (column-major from the south-west node; no-data nodes NaN). The array is the
   caller's, free it with mbgrdviz_free. 1 = there is a region */
int mbgrdviz_region_grid(int view, float **z, int *nx, int *ny, double *x0, double *x1, double *y0, double *y1);
void mbgrdviz_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* MBGRDVIZ_H_ */
