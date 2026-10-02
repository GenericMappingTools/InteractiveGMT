/*--------------------------------------------------------------------
 *    The MB-system:	mbview_*.c	10/9/2002
 *
 *    Copyright (c) 2002-2025 by
 *    David W. Caress (caress@mbari.org)
 *      Monterey Bay Aquarium Research Institute
 *      Moss Landing, California, USA
 *    Dale N. Chayes
 *      Center for Coastal and Ocean Mapping
 *      University of New Hampshire
 *      Durham, New Hampshire, USA
 *
 *    MB-System was created by Caress and Chayes in 1992 at the
 *      Lamont-Doherty Earth Observatory
 *      Columbia University
 *      Palisades, NY 10964
 *
 *    See README.md file for copying and redistribution conditions.
 *--------------------------------------------------------------------*/
/*
 * InteractiveGMT port (deps/src/mbgrdviz/mbgrdviz_mbview.c): the data half of libmbview that
 * mbgrdviz calls. Below the line marked VERBATIM are MB-System 5.8.3's own functions, copied from
 * src/mbview/mbview_process.c (projections, distances, the spheroid, getzdata), mbview_plot.c
 * (draping), mbview_route.c, mbview_site.c, mbview_nav.c, mbview_vector.c, mbview_profile.c and
 * mbview_pick.c (the pick profile) -- unchanged but for the names (mbgrdviz_mbview.h maps every
 * mbview_* onto mbgv_*).
 *
 * Above it, the port's own:
 *   - the globals libmbview keeps (mbviews, shared, mbv_verbose);
 *   - mbgv_setup: mbview_setprimarygrid plus the projection half of mbview_projectdata (the
 *     OpenGL arrays and the slope derivatives are not built: nothing here draws);
 *   - mbgv_set_area / mbgv_set_region: libmbview sets these from mouse drags; here they come from
 *     shapes drawn in the InteractiveGMT window, and the area's quad is recalculated by
 *     mbview_area's own code, verbatim;
 *   - the widget calls the data code makes (route/site/nav list refresh, the pick text, the view
 *     mode toggles, the bell) are empty: the InteractiveGMT window shows those as its own elements.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbgrdviz_mbview.h"

/* libmbview's globals */
int mbv_verbose = 0;
struct mbview_shared_struct shared;
struct mbview_world_struct mbviews[MBV_MAX_WINDOWS];

void (*mbgv_message_hook)(const char *message) = NULL;

/* the widget side of libmbview, not ported (see the head of this file) */
#define XBell(display, percent) ((void)0)
static void set_mbview_route_view_mode(size_t instance, int value) {
	(void)instance;
	(void)value;
}
int mbview_pick_text(size_t instance) {
	(void)instance;
	return (MB_SUCCESS);
}
int mbview_updatesitelist(void) {
	return (MB_SUCCESS);
}
int mbview_updateroutelist(void) {
	return (MB_SUCCESS);
}
int mbview_updatenavlist(void) {
	return (MB_SUCCESS);
}
int mbview_update(int verbose, size_t instance, int *error) {
	(void)verbose;
	(void)instance;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_enableviewsites(int verbose, size_t instance, int *error) {
	(void)verbose;
	mbviews[instance].data.site_view_mode = MBV_VIEW_ON;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_enableeditsites(int verbose, size_t instance, int *error) {
	(void)verbose;
	mbviews[instance].data.site_view_mode = MBV_VIEW_ON;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_enableviewroutes(int verbose, size_t instance, int *error) {
	(void)verbose;
	mbviews[instance].data.route_view_mode = MBV_VIEW_ON;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_enableeditroutes(int verbose, size_t instance, int *error) {
	(void)verbose;
	mbviews[instance].data.route_view_mode = MBV_VIEW_ON;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_enableviewnavs(int verbose, size_t instance, int *error) {
	(void)verbose;
	mbviews[instance].data.nav_view_mode = MBV_VIEW_ON;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_enableviewvectors(int verbose, size_t instance, int *error) {
	(void)verbose;
	mbviews[instance].data.vector_view_mode = MBV_VIEW_ON;
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
void do_mbview_message_on(char *message, size_t instance) {
	(void)instance;
	if (mbgv_message_hook != NULL)
		mbgv_message_hook(message);
}

/* mbview_callbacks.c */
int mbview_getdataptr(int verbose, size_t instance, struct mbview_struct **datahandle, int *error) {
	(void)verbose;
	*datahandle = &(mbviews[instance].data);
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}
int mbview_getsharedptr(int verbose, struct mbview_shareddata_struct **sharedhandle, int *error) {
	(void)verbose;
	*sharedhandle = &(shared.shareddata);
	*error = MB_ERROR_NO_ERROR;
	return (MB_SUCCESS);
}

/*------------------------------------------------------------------------------*/
int mbgv_setup(size_t instance, const char *title, int grid_projection_mode, const char *grid_projection_id,
               int display_projection_mode, const char *display_projection_id, const float *z, int n_columns,
               int n_rows, double xmin, double xmax, double ymin, double ymax, double dx, double dy) {
	int error = MB_ERROR_NO_ERROR;
	if (instance >= MBV_MAX_WINDOWS || z == NULL || n_columns < 2 || n_rows < 2)
		return (MB_FAILURE);
	mbgv_release(instance);

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* mbview_init's defaults for what the data code reads */
	data->display_mode = MBV_DISPLAY_2D;
	data->exaggeration = 1.0;
	data->area_type = MBV_AREA_NONE;
	data->region_type = MBV_REGION_NONE;
	data->pick_type = MBV_PICK_NONE;
	data->profile.source = MBV_PROFILE_NONE;
	snprintf(data->title, sizeof(data->title), "MBgrdviz: %s", title ? title : "");
	view->areaaspect = 0.5;
	view->aspect_ratio = 1.0;

	/* mbview_setprimarygrid: the values, and the grid copied (NaN = no data -> the nodata value the
	   data code compares against) */
	data->primary_grid_projection_mode = grid_projection_mode;
	snprintf(data->primary_grid_projection_id, sizeof(mb_path), "%s", grid_projection_id ? grid_projection_id : "");
	data->primary_nodatavalue = MBV_DEFAULT_NODATA;
	data->primary_nxy = n_columns * n_rows;
	data->primary_n_columns = n_columns;
	data->primary_n_rows = n_rows;
	data->primary_xmin = xmin;
	data->primary_xmax = xmax;
	data->primary_ymin = ymin;
	data->primary_ymax = ymax;
	data->primary_dx = dx;
	data->primary_dy = dy;
	data->viewbounds[0] = 0;
	data->viewbounds[1] = data->primary_n_columns;
	data->viewbounds[2] = 0;
	data->viewbounds[3] = data->primary_n_rows;
	data->primary_data = (float *)malloc(sizeof(float) * (size_t)data->primary_nxy);
	if (data->primary_data == NULL)
		return (MB_FAILURE);
	bool first = true;
	for (int k = 0; k < data->primary_nxy; k++) {
		const float v = z[k];
		if (isnan(v)) {
			data->primary_data[k] = data->primary_nodatavalue;
			continue;
		}
		data->primary_data[k] = v;
		if (first || v < data->primary_min)
			data->primary_min = v;
		if (first || v > data->primary_max)
			data->primary_max = v;
		first = false;
	}
	data->display_projection_mode = display_projection_mode;
	snprintf(data->display_projection_id, sizeof(mb_path), "%s", display_projection_id ? display_projection_id : "");

	/* mbview_projectdata, the projection part */
	int proj_status = MB_SUCCESS;
	double xlonmin, xlonmax, ylatmin, ylatmax, zdisplay;
	if (data->primary_grid_projection_mode == MBV_PROJECTION_PROJECTED &&
	    data->display_projection_mode == MBV_PROJECTION_PROJECTED &&
	    strcmp(data->primary_grid_projection_id, data->display_projection_id) == 0) {
		/* reset modes */
		data->primary_grid_projection_mode = MBV_PROJECTION_ALREADYPROJECTED;
		data->display_projection_mode = MBV_PROJECTION_ALREADYPROJECTED;

		/* get bounds */
		view->xmin = data->primary_xmin;
		view->xmax = data->primary_xmax;
		view->ymin = data->primary_ymin;
		view->ymax = data->primary_ymax;

		/* get origin */
		view->xorigin = 0.5 * (view->xmin + view->xmax);
		view->yorigin = 0.5 * (view->ymin + view->ymax);
		view->zorigin = data->exaggeration * 0.5 * (data->primary_min + data->primary_max);

		/* set projection for getting lon lat */
		proj_status = mb_proj_init(mbv_verbose, data->primary_grid_projection_id, &(view->primary_pjptr), &error);
		if (proj_status == MB_SUCCESS) {
			view->primary_pj_init = true;
			proj_status = mb_proj_init(mbv_verbose, data->display_projection_id, &(view->display_pjptr), &error);
			if (proj_status == MB_SUCCESS)
				view->display_pj_init = true;
		}
		if (proj_status != MB_SUCCESS)
			return (MB_FAILURE);
	}
	else {
		/* first go from grid coordinates to lon lat */
		if (data->primary_grid_projection_mode == MBV_PROJECTION_PROJECTED) {
			proj_status = mb_proj_init(mbv_verbose, data->primary_grid_projection_id, &(view->primary_pjptr), &error);
			if (proj_status != MB_SUCCESS)
				return (MB_FAILURE);
			view->primary_pj_init = true;
			mb_proj_inverse(mbv_verbose, view->primary_pjptr, data->primary_xmin, data->primary_ymin, &xlonmin, &ylatmin,
			                &error);
			mb_proj_inverse(mbv_verbose, view->primary_pjptr, data->primary_xmax, data->primary_ymax, &xlonmax, &ylatmax,
			                &error);
		}
		else {
			xlonmin = data->primary_xmin;
			xlonmax = data->primary_xmax;
			ylatmin = data->primary_ymin;
			ylatmax = data->primary_ymax;
		}

		/* now go from lon lat to display coordinates */
		if (data->display_projection_mode == MBV_PROJECTION_PROJECTED) {
			proj_status = mb_proj_init(mbv_verbose, data->display_projection_id, &(view->display_pjptr), &error);
			if (proj_status != MB_SUCCESS)
				return (MB_FAILURE);
			view->display_pj_init = true;
			mb_proj_forward(mbv_verbose, view->display_pjptr, xlonmin, ylatmin, &view->xmin, &view->ymin, &error);
			mb_proj_forward(mbv_verbose, view->display_pjptr, xlonmax, ylatmax, &view->xmax, &view->ymax, &error);
			view->xorigin = 0.5 * (view->xmin + view->xmax);
			view->yorigin = 0.5 * (view->ymin + view->ymax);
			view->zorigin = data->exaggeration * 0.5 * (data->primary_min + data->primary_max);
		}
		else if (data->display_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
			mb_coor_scale(mbv_verbose, 0.5 * (ylatmin + ylatmax), &(view->mtodeglon), &(view->mtodeglat));
			view->xmin = xlonmin / view->mtodeglon;
			view->xmax = xlonmax / view->mtodeglon;
			view->ymin = ylatmin / view->mtodeglat;
			view->ymax = ylatmax / view->mtodeglat;
			view->xorigin = 0.5 * (view->xmin + view->xmax);
			view->yorigin = 0.5 * (view->ymin + view->ymax);
			view->zorigin = data->exaggeration * 0.5 * (data->primary_min + data->primary_max);
		}
		else if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) {
			if (xlonmax - xlonmin >= 180.0 || ylatmax - ylatmin >= 90.0) {
				mbview_sphere_setup(instance, true, 0.5 * (xlonmin + xlonmax), 0.5 * (ylatmin + ylatmax));
				view->xmin = -MBV_SPHEROID_RADIUS;
				view->xmax = MBV_SPHEROID_RADIUS;
				view->ymin = -MBV_SPHEROID_RADIUS;
				view->ymax = MBV_SPHEROID_RADIUS;
				view->sphere_refx = 0.0;
				view->sphere_refy = 0.0;
				view->sphere_refz = 0.0;
				view->xorigin = 0.0;
				view->yorigin = 0.0;
				view->zorigin = 0.0;
			}
			else {
				mbview_sphere_setup(instance, false, 0.5 * (xlonmin + xlonmax), 0.5 * (ylatmin + ylatmax));
				mbview_sphere_forward(instance, 0.5 * (xlonmin + xlonmax), 0.5 * (ylatmin + ylatmax), &view->sphere_refx,
				                      &view->sphere_refy, &view->sphere_refz);
				mbview_sphere_forward(instance, xlonmin, ylatmin, &view->xmin, &view->ymin, &zdisplay);
				mbview_sphere_forward(instance, xlonmax, ylatmax, &view->xmax, &view->ymax, &zdisplay);
				view->xmin -= view->sphere_refx;
				view->xmax -= view->sphere_refx;
				view->ymin -= view->sphere_refy;
				view->ymax -= view->sphere_refy;
				mbview_sphere_forward(instance, 0.5 * (xlonmin + xlonmax), 0.5 * (ylatmin + ylatmax), &view->xorigin,
				                      &view->yorigin, &view->zorigin);
				view->xorigin -= view->sphere_refx;
				view->yorigin -= view->sphere_refy;
				view->zorigin += 0.5 * (data->primary_min + data->primary_max) - view->sphere_refz;
			}
		}
	}

	/* get origin and scaling */
	view->scale = MIN((1.75 * MBV_OPENGL_WIDTH / (view->xmax - view->xmin)),
	                  (1.75 * MBV_OPENGL_WIDTH / view->aspect_ratio / (view->ymax - view->ymin)));

	view->init = MBV_WINDOW_VISIBLE;
	view->glx_init = true;
	return (MB_SUCCESS);
}
/*------------------------------------------------------------------------------*/
void mbgv_release(size_t instance) {
	int error = MB_ERROR_NO_ERROR;
	if (instance >= MBV_MAX_WINDOWS)
		return;
	struct mbview_world_struct *view = &(mbviews[instance]);
	if (view->primary_pj_init && view->primary_pjptr != NULL)
		mb_proj_free(mbv_verbose, &(view->primary_pjptr), &error);
	if (view->display_pj_init && view->display_pjptr != NULL)
		mb_proj_free(mbv_verbose, &(view->display_pjptr), &error);
	free(view->data.primary_data);   /* mbgv_setup's own */
	/* the data code's, from MBIO's allocator */
	if (view->data.profile.points != NULL)
		mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(view->data.profile.points), &error);
	if (view->data.area.segment.lspoints != NULL)
		mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(view->data.area.segment.lspoints), &error);
	for (int i = 0; i < 4; i++) {
		if (view->data.area.segments[i].lspoints != NULL)
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(view->data.area.segments[i].lspoints), &error);
		if (view->data.region.segments[i].lspoints != NULL)
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(view->data.region.segments[i].lspoints), &error);
	}
	memset(view, 0, sizeof(struct mbview_world_struct));
}
/*------------------------------------------------------------------------------*/
/* mbview_area (mbview_pick.c): the quad recalculation and the draping, verbatim; `which` stands for
   the mouse event that drove it there (a move recalculates, the release drapes) */
#define MBV_AREALENGTH_DOWN 1
#define MBV_AREALENGTH_MOVE 2
#define MBV_AREALENGTH_UP 3
#define MBV_AREAASPECT_CHANGE 4
#define MBV_AREAASPECT_UP 5
static int mbgv_area_quad(size_t instance, int which) {
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* recalculate any good quad area whether defined this time or previously
	    this catches which == MBV_AREAASPECT_CHANGE calls */
	if (data->area_type == MBV_AREA_QUAD && which != MBV_AREALENGTH_UP && which != MBV_AREAASPECT_UP) {
		/* deal with non-spheroid case */
		if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
			/* now define the quad corners in display coordinates */
			const double dx = data->area.endpoints[1].xdisplay - data->area.endpoints[0].xdisplay;
			const double dy = data->area.endpoints[1].ydisplay - data->area.endpoints[0].ydisplay;
			const double dxuse = 0.5 * view->areaaspect * dy;
			const double dyuse = 0.5 * view->areaaspect * dx;

			data->area.cornerpoints[0].xdisplay = data->area.endpoints[0].xdisplay - dxuse;
			data->area.cornerpoints[0].ydisplay = data->area.endpoints[0].ydisplay + dyuse;
			data->area.cornerpoints[1].xdisplay = data->area.endpoints[0].xdisplay + dxuse;
			data->area.cornerpoints[1].ydisplay = data->area.endpoints[0].ydisplay - dyuse;
			data->area.cornerpoints[2].xdisplay = data->area.endpoints[1].xdisplay + dxuse;
			data->area.cornerpoints[2].ydisplay = data->area.endpoints[1].ydisplay - dyuse;
			data->area.cornerpoints[3].xdisplay = data->area.endpoints[1].xdisplay - dxuse;
			data->area.cornerpoints[3].ydisplay = data->area.endpoints[1].ydisplay + dyuse;

			/* calculate width and length */
			data->area.length = sqrt(dx * dx + dy * dy) / view->scale;
			data->area.width = view->areaaspect * data->area.length;
			data->area.bearing = RTD * atan2(dx, dy);
			if (data->area.bearing < 0.0)
				data->area.bearing += 360.0;
			if (data->area.bearing > 360.0)
				data->area.bearing -= 360.0;

			/* set pick info */
			data->pickinfo_mode = MBV_PICK_AREA;

			/* reset segment endpoints */
			for (int i = 0; i < 2; i++) {
				data->area.segment.endpoints[i] = data->area.endpoints[i];
			}
			for (int i = 0; i < 4; i++) {
				int k = i + 1;
				if (k > 3)
					k = 0;
				data->area.segments[i].endpoints[0] = data->area.cornerpoints[i];
				data->area.segments[i].endpoints[1] = data->area.cornerpoints[k];
			}

			/* now project the segment endpoints */
			for (int i = 0; i < 4; i++) {
				for (int j = 0; j < 2; j++) {
					mbview_projectinverse(
					    instance, true, data->area.segments[i].endpoints[j].xdisplay,
					    data->area.segments[i].endpoints[j].ydisplay, data->area.segments[i].endpoints[j].zdisplay,
					    &(data->area.segments[i].endpoints[j].xlon), &(data->area.segments[i].endpoints[j].ylat),
					    &(data->area.segments[i].endpoints[j].xgrid), &(data->area.segments[i].endpoints[j].ygrid));
					bool ok;
					mbview_getzdata(instance, data->area.segments[i].endpoints[j].xgrid,
					                data->area.segments[i].endpoints[j].ygrid, &ok, &(data->area.segments[i].endpoints[j].zdata));
					if (!ok && ((i == 0) || (i == 1 && j == 0) || (i == 3 && j == 1)))
						data->area.segments[i].endpoints[j].zdata = data->area.endpoints[0].zdata;
					else if (!ok)
						data->area.segments[i].endpoints[j].zdata = data->area.endpoints[1].zdata;
					mbview_projectll2display(
					    instance, data->area.segments[i].endpoints[j].xlon, data->area.segments[i].endpoints[j].ylat,
					    data->area.segments[i].endpoints[j].zdata, &data->area.segments[i].endpoints[j].xdisplay,
					    &data->area.segments[i].endpoints[j].ydisplay, &data->area.segments[i].endpoints[j].zdisplay);
				}
			}
		}

		/* else deal with spheroid case */
		else {
			/* now get length and bearing of center line */
			mbview_greatcircle_distbearing(instance, data->area.endpoints[0].xlon, data->area.endpoints[0].ylat,
			                               data->area.endpoints[1].xlon, data->area.endpoints[1].ylat, &data->area.bearing,
			                               &data->area.length);
			data->area.width = view->areaaspect * data->area.length;

			/* the corners of the area are defined by great
			    circle arcs perpendicular to the center line */

			double bearing = data->area.bearing - 90.0;
			if (bearing < 0.0)
				bearing += 360.0;
			if (bearing > 360.0)
				bearing -= 360.0;
			mbview_greatcircle_endposition(instance, data->area.endpoints[0].xlon, data->area.endpoints[0].ylat, bearing,
			                               (0.5 * data->area.width), &(data->area.cornerpoints[0].xlon),
			                               &(data->area.cornerpoints[0].ylat)),
			    status = mbview_projectll2xyzgrid(instance, data->area.cornerpoints[0].xlon, data->area.cornerpoints[0].ylat,
			                                      &(data->area.cornerpoints[0].xgrid), &(data->area.cornerpoints[0].ygrid),
			                                      &(data->area.cornerpoints[0].zdata));
			status = mbview_projectll2display(instance, data->area.cornerpoints[0].xlon, data->area.cornerpoints[0].ylat,
			                                  data->area.cornerpoints[0].zdata, &data->area.cornerpoints[0].xdisplay,
			                                  &data->area.cornerpoints[0].ydisplay, &data->area.cornerpoints[0].zdisplay);

			bearing = data->area.bearing + 90.0;
			if (bearing < 0.0)
				bearing += 360.0;
			if (bearing > 360.0)
				bearing -= 360.0;
			mbview_greatcircle_endposition(instance, data->area.endpoints[0].xlon, data->area.endpoints[0].ylat, bearing,
			                               (0.5 * data->area.width), &(data->area.cornerpoints[1].xlon),
			                               &(data->area.cornerpoints[1].ylat)),
			    status = mbview_projectll2xyzgrid(instance, data->area.cornerpoints[1].xlon, data->area.cornerpoints[1].ylat,
			                                      &(data->area.cornerpoints[1].xgrid), &(data->area.cornerpoints[1].ygrid),
			                                      &(data->area.cornerpoints[1].zdata));
			status = mbview_projectll2display(instance, data->area.cornerpoints[1].xlon, data->area.cornerpoints[1].ylat,
			                                  data->area.cornerpoints[1].zdata, &data->area.cornerpoints[1].xdisplay,
			                                  &data->area.cornerpoints[1].ydisplay, &data->area.cornerpoints[1].zdisplay);

			bearing = data->area.bearing + 90.0;
			if (bearing < 0.0)
				bearing += 360.0;
			if (bearing > 360.0)
				bearing -= 360.0;
			mbview_greatcircle_endposition(instance, data->area.endpoints[1].xlon, data->area.endpoints[1].ylat, bearing,
			                               (0.5 * data->area.width), &(data->area.cornerpoints[2].xlon),
			                               &(data->area.cornerpoints[2].ylat)),
			    status = mbview_projectll2xyzgrid(instance, data->area.cornerpoints[2].xlon, data->area.cornerpoints[2].ylat,
			                                      &(data->area.cornerpoints[2].xgrid), &(data->area.cornerpoints[2].ygrid),
			                                      &(data->area.cornerpoints[2].zdata));
			status = mbview_projectll2display(instance, data->area.cornerpoints[2].xlon, data->area.cornerpoints[2].ylat,
			                                  data->area.cornerpoints[2].zdata, &data->area.cornerpoints[2].xdisplay,
			                                  &data->area.cornerpoints[2].ydisplay, &data->area.cornerpoints[2].zdisplay);

			bearing = data->area.bearing - 90.0;
			if (bearing < 0.0)
				bearing += 360.0;
			if (bearing > 360.0)
				bearing -= 360.0;
			mbview_greatcircle_endposition(instance, data->area.endpoints[1].xlon, data->area.endpoints[1].ylat, bearing,
			                               (0.5 * data->area.width), &(data->area.cornerpoints[3].xlon),
			                               &(data->area.cornerpoints[3].ylat)),
			    status = mbview_projectll2xyzgrid(instance, data->area.cornerpoints[3].xlon, data->area.cornerpoints[3].ylat,
			                                      &(data->area.cornerpoints[3].xgrid), &(data->area.cornerpoints[3].ygrid),
			                                      &(data->area.cornerpoints[3].zdata));
			status = mbview_projectll2display(instance, data->area.cornerpoints[3].xlon, data->area.cornerpoints[3].ylat,
			                                  data->area.cornerpoints[3].zdata, &data->area.cornerpoints[3].xdisplay,
			                                  &data->area.cornerpoints[3].ydisplay, &data->area.cornerpoints[3].zdisplay);

			/* set pick info */
			data->pickinfo_mode = MBV_PICK_AREA;

			/* reset segment endpoints */
			for (int i = 0; i < 2; i++) {
				data->area.segment.endpoints[i] = data->area.endpoints[i];
			}
			for (int i = 0; i < 4; i++) {
				int k = i + 1;
				if (k > 3)
					k = 0;
				data->area.segments[i].endpoints[0] = data->area.cornerpoints[i];
				data->area.segments[i].endpoints[1] = data->area.cornerpoints[k];
			}

			/* now project the segment endpoints */
			for (int i = 0; i < 4; i++) {
				for (int j = 0; j < 2; j++) {
					bool ok;
					mbview_getzdata(instance, data->area.segments[i].endpoints[j].xgrid,
					                data->area.segments[i].endpoints[j].ygrid, &ok, &(data->area.segments[i].endpoints[j].zdata));
					if (!ok && ((i == 0) || (i == 1 && j == 0) || (i == 3 && j == 1)))
						data->area.segments[i].endpoints[j].zdata = data->area.endpoints[0].zdata;
					else if (!ok)
						data->area.segments[i].endpoints[j].zdata = data->area.endpoints[1].zdata;
					mbview_projectll2display(
					    instance, data->area.segments[i].endpoints[j].xlon, data->area.segments[i].endpoints[j].ylat,
					    data->area.segments[i].endpoints[j].zdata, &data->area.segments[i].endpoints[j].xdisplay,
					    &data->area.segments[i].endpoints[j].ydisplay, &data->area.segments[i].endpoints[j].zdisplay);
				}
			}
		}

		/* set pick annotation */
		mbview_pick_text(instance);
	}

	/* now set and drape the segments
	    if either 3D display
	    or the pick move is final  */
	if (data->area_type == MBV_AREA_QUAD &&
	    (data->display_mode == MBV_DISPLAY_3D || which == MBV_AREALENGTH_UP || which == MBV_AREAASPECT_UP)) {
		mbview_drapesegment(instance, &(data->area.segment));
		for (int i = 0; i < 4; i++) {
			/* drape the segment */
			mbview_drapesegment(instance, &(data->area.segments[i]));
		}
	}


	return (status);
}
static void mbgv_set_point(size_t instance, double xgrid, double ygrid, struct mbview_point_struct *p) {
	bool found;
	p->xgrid = xgrid;
	p->ygrid = ygrid;
	mbview_projectgrid2ll(instance, xgrid, ygrid, &p->xlon, &p->ylat);
	mbview_getzdata(instance, xgrid, ygrid, &found, &p->zdata);
	if (!found)
		p->zdata = 0.5 * (mbviews[instance].data.primary_min + mbviews[instance].data.primary_max);
	mbview_projectll2display(instance, p->xlon, p->ylat, p->zdata, &p->xdisplay, &p->ydisplay, &p->zdisplay);
}
int mbgv_set_area(size_t instance, double xgrid0, double ygrid0, double xgrid1, double ygrid1, double width) {
	if (instance >= MBV_MAX_WINDOWS || mbviews[instance].init == MBV_WINDOW_NULL)
		return (MB_FAILURE);
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);
	data->area_type = MBV_AREA_NONE;
	if (xgrid0 == xgrid1 && ygrid0 == ygrid1)
		return (MB_FAILURE);
	mbgv_set_point(instance, xgrid0, ygrid0, &data->area.endpoints[0]);
	mbgv_set_point(instance, xgrid1, ygrid1, &data->area.endpoints[1]);
	data->area_type = MBV_AREA_QUAD;
	data->area_pickendpoint = MBV_AREA_PICKENDPOINT1;

	/* the area aspect (width / length) from the width asked for; the length is the centre line's,
	   measured as mbview_area measures it (display distance / scale) */
	if (width > 0.0) {
		const double dx = data->area.endpoints[1].xdisplay - data->area.endpoints[0].xdisplay;
		const double dy = data->area.endpoints[1].ydisplay - data->area.endpoints[0].ydisplay;
		double length = sqrt(dx * dx + dy * dy) / view->scale;
		if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) {
			double bearing;
			mbview_greatcircle_distbearing(instance, data->area.endpoints[0].xlon, data->area.endpoints[0].ylat,
			                               data->area.endpoints[1].xlon, data->area.endpoints[1].ylat, &bearing, &length);
		}
		if (length > 0.0)
			view->areaaspect = (float)(width / length);
	}
	mbgv_area_quad(instance, MBV_AREALENGTH_MOVE);
	return mbgv_area_quad(instance, MBV_AREALENGTH_UP);
}
/*------------------------------------------------------------------------------*/
int mbgv_set_region(size_t instance, double xgrid0, double ygrid0, double xgrid1, double ygrid1) {
	if (instance >= MBV_MAX_WINDOWS || mbviews[instance].init == MBV_WINDOW_NULL)
		return (MB_FAILURE);
	struct mbview_struct *data = &(mbviews[instance].data);
	data->region_type = MBV_REGION_NONE;
	if (xgrid0 == xgrid1 || ygrid0 == ygrid1)
		return (MB_FAILURE);

	/* the corners in mbview_region's order: 0 the first pick, 3 the opposite one */
	mbgv_set_point(instance, xgrid0, ygrid0, &data->region.cornerpoints[0]);
	mbgv_set_point(instance, xgrid1, ygrid0, &data->region.cornerpoints[1]);
	mbgv_set_point(instance, xgrid0, ygrid1, &data->region.cornerpoints[2]);
	mbgv_set_point(instance, xgrid1, ygrid1, &data->region.cornerpoints[3]);
	data->region_type = MBV_REGION_QUAD;
	return (MB_SUCCESS);
}

/*==============================================================================
 * VERBATIM from here: MB-System 5.8.3 src/mbview/ (see the head of this file)
 *==============================================================================*/
int mbview_projectforward(size_t instance, bool needlonlat, double xgrid, double ygrid, double zdata, double *xlon, double *ylat,
                          double *xdisplay, double *ydisplay, double *zdisplay) {
	double xx, yy, zz;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       needlonlat:       %d\n", needlonlat);
		fprintf(stderr, "dbg2       xgrid:            %f\n", xgrid);
		fprintf(stderr, "dbg2       ygrid:            %f\n", ygrid);
		fprintf(stderr, "dbg2       zdata:            %f\n", zdata);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* get positions into geographic coordinates if necessary */
	if (needlonlat || data->primary_grid_projection_mode != MBV_PROJECTION_ALREADYPROJECTED) {
		status = mbview_projectgrid2ll(instance, xgrid, ygrid, xlon, ylat);
	}

	/* get positions in the display projection */
	if (data->primary_grid_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
		xx = xgrid;
		yy = ygrid;
		zz = data->exaggeration * zdata;
		*xdisplay = view->scale * (xx - view->xorigin);
		*ydisplay = view->scale * (yy - view->yorigin);
		*zdisplay = view->scale * (zz - view->zorigin);
	}
	else {
		status = mbview_projectll2display(instance, *xlon, *ylat, zdata, xdisplay, ydisplay, zdisplay);
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xlon:        %f\n", *xlon);
		fprintf(stderr, "dbg2       ylat:        %f\n", *ylat);
		fprintf(stderr, "dbg2       xdisplay:    %f\n", *xdisplay);
		fprintf(stderr, "dbg2       ydisplay:    %f\n", *ydisplay);
		fprintf(stderr, "dbg2       zdisplay:    %f\n", *zdisplay);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectinverse(size_t instance, bool needlonlat, double xdisplay, double ydisplay, double zdisplay, double *xlon,
                          double *ylat, double *xgrid, double *ygrid) {
	double xx, yy;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       needlonlat:       %d\n", needlonlat);
		fprintf(stderr, "dbg2       xdisplay:         %f\n", xdisplay);
		fprintf(stderr, "dbg2       ydisplay:         %f\n", ydisplay);
		fprintf(stderr, "dbg2       zdisplay:         %f\n", zdisplay);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* get positions in geographic coordinates */
	if (needlonlat || data->primary_grid_projection_mode != MBV_PROJECTION_ALREADYPROJECTED) {
		status = mbview_projectdisplay2ll(instance, xdisplay, ydisplay, zdisplay, xlon, ylat);
	}

	/* get positions into grid coordinates */
	if (data->primary_grid_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
		xx = xdisplay / view->scale + view->xorigin;
		yy = ydisplay / view->scale + view->yorigin;
		*xgrid = xx;
		*ygrid = yy;
	}
	else {
		status = mbview_projectll2xygrid(instance, *xlon, *ylat, xgrid, ygrid);
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xlon:         %f\n", *xlon);
		fprintf(stderr, "dbg2       ylat:         %f\n", *ylat);
		fprintf(stderr, "dbg2       xgrid:        %f\n", *xgrid);
		fprintf(stderr, "dbg2       ygrid:        %f\n", *ygrid);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:       %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectfromlonlat(size_t instance, double xlon, double ylat, double zdata, double *xgrid, double *ygrid,
                             double *xdisplay, double *ydisplay, double *zdisplay) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
		fprintf(stderr, "dbg2       zdata:            %f\n", zdata);
	}

	/* get positions into grid coordinates */
	int status = mbview_projectll2xygrid(instance, xlon, ylat, xgrid, ygrid);

	/* get positions in the display projection */
	status = mbview_projectll2display(instance, xlon, ylat, zdata, xdisplay, ydisplay, zdisplay);

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xgrid:       %f\n", *xgrid);
		fprintf(stderr, "dbg2       ygrid:       %f\n", *ygrid);
		fprintf(stderr, "dbg2       xdisplay:    %f\n", *xdisplay);
		fprintf(stderr, "dbg2       ydisplay:    %f\n", *ydisplay);
		fprintf(stderr, "dbg2       zdisplay:    %f\n", *zdisplay);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectgrid2ll(size_t instance, double xgrid, double ygrid, double *xlon, double *ylat) {
	int error = MB_ERROR_NO_ERROR;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xgrid:            %f\n", xgrid);
		fprintf(stderr, "dbg2       ygrid:            %f\n", ygrid);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get positions into geographic coordinates */
	if (data->primary_grid_projection_mode == MBV_PROJECTION_PROJECTED ||
	    data->primary_grid_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
		mb_proj_inverse(mbv_verbose, view->primary_pjptr, xgrid, ygrid, xlon, ylat, &error);
	}
	else if (data->primary_grid_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
		*xlon = xgrid;
		*ylat = ygrid;
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xlon:             %f\n", *xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", *ylat);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectll2xygrid(size_t instance, double xlon, double ylat, double *xgrid, double *ygrid) {
	int error = MB_ERROR_NO_ERROR;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get positions into grid coordinates */
	if (data->primary_grid_projection_mode == MBV_PROJECTION_PROJECTED ||
	    data->primary_grid_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
if (ylat > 90.0) {
fprintf(stderr, "%s:%d:%s: Warning: calling mb_proj_forward with invalid latitude: lon: %f lat: %f\n",
__FILE__, __LINE__, __FUNCTION__, xlon, ylat);
}
		mb_proj_forward(mbv_verbose, view->primary_pjptr, xlon, ylat, xgrid, ygrid, &error);
	}
	else {
		if (data->primary_grid_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
			if (data->primary_xmin < -180.0 && xlon > 0.0)
				xlon -= 360.0;
			if (data->primary_xmax > 180.0 && xlon < 0.0)
				xlon += 360.0;
		}
		*xgrid = xlon;
		*ygrid = ylat;
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xgrid:       %f\n", *xgrid);
		fprintf(stderr, "dbg2       ygrid:       %f\n", *ygrid);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectll2xyzgrid(size_t instance, double xlon, double ylat, double *xgrid, double *ygrid, double *zdata) {
	int error = MB_ERROR_NO_ERROR;
	int nfound;
	int i, j, k, ii, jj;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get positions into grid coordinates */
	if (data->primary_grid_projection_mode == MBV_PROJECTION_PROJECTED ||
	    data->primary_grid_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
if (ylat > 90.0) {
fprintf(stderr, "%s:%d:%s: Warning: calling mb_proj_forward with invalid latitude: lon: %f lat: %f\n",
__FILE__, __LINE__, __FUNCTION__, xlon, ylat);
}
		mb_proj_forward(mbv_verbose, view->primary_pjptr, xlon, ylat, xgrid, ygrid, &error);
	}
	else {
		if (data->primary_grid_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
			if (data->primary_xmin < -180.0 && xlon > 0.0)
				xlon -= 360.0;
			if (data->primary_xmax > 180.0 && xlon < 0.0)
				xlon += 360.0;
		}
		*xgrid = xlon;
		*ygrid = ylat;
	}

	/* now get zdata  from primary grid */
	nfound = 0;
	*zdata = 0.0;
	i = (int)((*xgrid - data->primary_xmin) / data->primary_dx);
	j = (int)((*ygrid - data->primary_ymin) / data->primary_dy);
	if (i >= 0 && i < data->primary_n_columns - 1 && j >= 0 && j < data->primary_n_rows - 1) {
		for (ii = i; ii <= i + 1; ii++)
			for (jj = j; jj <= j + 1; jj++) {
				k = ii * data->primary_n_rows + jj;
				if (data->primary_data[k] != data->primary_nodatavalue) {
					nfound++;
					*zdata += data->primary_data[k];
				}
			}
	}

	int status = MB_SUCCESS;
	if (nfound > 0) {
		*zdata /= (double)nfound;
		status = MB_SUCCESS;
	}
	else {
		*zdata = 0.0;
		status = MB_FAILURE;
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xgrid:       %f\n", *xgrid);
		fprintf(stderr, "dbg2       ygrid:       %f\n", *ygrid);
		fprintf(stderr, "dbg2       zdata:       %f\n", *zdata);
		fprintf(stderr, "dbg2       data->primary_nodatavalue:       %f\n", data->primary_nodatavalue);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectll2display(size_t instance, double xlon, double ylat, double zdata, double *xdisplay, double *ydisplay,
                             double *zdisplay) {
	int error = MB_ERROR_NO_ERROR;
	double xx, yy, zz;
	double effective_topography;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
		fprintf(stderr, "dbg2       zdata:            %f\n", zdata);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get positions in the display projection */
	if (data->display_projection_mode == MBV_PROJECTION_PROJECTED ||
	    data->display_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
if (ylat > 90.0) {
fprintf(stderr, "%s:%d:%s: Warning: calling mb_proj_forward with invalid latitude: lon: %f lat: %f\n",
__FILE__, __LINE__, __FUNCTION__, xlon, ylat);
}
		mb_proj_forward(mbv_verbose, view->display_pjptr, xlon, ylat, &xx, &yy, &error);
		zz = data->exaggeration * zdata;
		/* fprintf(stderr,"pos: %f %f %f   raw: %f %f %f ",
		xlon, ylat, zdata, xx, yy, zz); */
	}
	else if (data->display_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
		xx = xlon / view->mtodeglon;
		yy = ylat / view->mtodeglat;
		zz = data->exaggeration * zdata;
	}
	else /*if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) */
	{
		mbview_sphere_forward(instance, xlon, ylat, &xx, &yy, &zz);
		effective_topography = data->exaggeration * (zdata - 0.5 * (data->primary_min + data->primary_max)) +
		                       0.5 * (data->primary_min + data->primary_max);
		/* fprintf(stderr,"pos: %f %f %f   raw: %f %f %f  topo:%f ",
		xlon, ylat, zdata, xx, yy, zz, effective_topography); */

		xx += (effective_topography * xx / MBV_SPHEROID_RADIUS) - view->sphere_refx;
		yy += (effective_topography * yy / MBV_SPHEROID_RADIUS) - view->sphere_refy;
		zz += (effective_topography * zz / MBV_SPHEROID_RADIUS) - view->sphere_refz;
		/* fprintf(stderr,"unscaled: %f %f %f",
		xx, yy, zz); */
	}

	/* get final positions in display coordinates */
	*xdisplay = view->scale * (xx - view->xorigin);
	*ydisplay = view->scale * (yy - view->yorigin);
	*zdisplay = view->scale * (zz - view->zorigin);
	if (isnan(*xdisplay)) {
		fprintf(stderr, "NaN alert!!\n");
		//fprintf(stderr,"pos: %f %f %f   raw: %f %f %f  topo:%f   scale:%f   scaled: %f %f %f\n",
		//xlon, ylat, zdata, xx, yy, zz, effective_topography, view->scale, *xdisplay, *ydisplay, *zdisplay);
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xdisplay:    %f\n", *xdisplay);
		fprintf(stderr, "dbg2       ydisplay:    %f\n", *ydisplay);
		fprintf(stderr, "dbg2       zdisplay:    %f\n", *zdisplay);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectdisplay2ll(size_t instance, double xdisplay, double ydisplay, double zdisplay, double *xlon, double *ylat) {
	int error = MB_ERROR_NO_ERROR;
	double xx, yy, zz;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xdisplay:         %f\n", xdisplay);
		fprintf(stderr, "dbg2       ydisplay:         %f\n", ydisplay);
		fprintf(stderr, "dbg2       zdisplay:         %f\n", zdisplay);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get positions in display projection */
	xx = xdisplay / view->scale + view->xorigin;
	yy = ydisplay / view->scale + view->yorigin;
	zz = zdisplay / view->scale + view->zorigin;

	/* get positions in geographic coordinates */
	if (data->display_projection_mode == MBV_PROJECTION_PROJECTED ||
	    data->display_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
		mb_proj_inverse(mbv_verbose, view->display_pjptr, xx, yy, xlon, ylat, &error);
	}
	else if (data->display_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
		*xlon = xx * view->mtodeglon;
		*ylat = yy * view->mtodeglat;
	}
	else if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) {
		xx += view->sphere_refx;
		yy += view->sphere_refy;
		zz += view->sphere_refz;
		mbview_sphere_inverse(instance, xx, yy, zz, xlon, ylat);
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       xlon:             %f\n", *xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", *ylat);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_projectdistance(size_t instance, double xlon1, double ylat1, double zdata1, double xlon2, double ylat2, double zdata2,
                           double *distancelateral, double *distanceoverground, double *slope) {
	int error = MB_ERROR_NO_ERROR;
	double xx1, yy1, zz1;
	double xx2, yy2, zz2;
	double dx, dy, dz;
	double bearing;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xlon1:            %f\n", xlon1);
		fprintf(stderr, "dbg2       ylat1:            %f\n", ylat1);
		fprintf(stderr, "dbg2       zdata1:           %f\n", zdata1);
		fprintf(stderr, "dbg2       xlon2:            %f\n", xlon2);
		fprintf(stderr, "dbg2       ylat2:            %f\n", ylat2);
		fprintf(stderr, "dbg2       zdata2:           %f\n", zdata2);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get positions in display projection without scaling or exaggeration */
	if (data->display_projection_mode == MBV_PROJECTION_PROJECTED ||
	    data->display_projection_mode == MBV_PROJECTION_ALREADYPROJECTED) {
		/* point 1 */
if (ylat1 > 90.0) {
fprintf(stderr, "%s:%d:%s: Warning: calling mb_proj_forward with invalid latitude: lon: %f lat: %f\n",
__FILE__, __LINE__, __FUNCTION__, xlon1, ylat1);
}
		mb_proj_forward(mbv_verbose, view->display_pjptr, xlon1, ylat1, &xx1, &yy1, &error);
		zz1 = zdata1;

		/* point 2 */
if (ylat2 > 90.0) {
fprintf(stderr, "%s:%d:%s: Warning: calling mb_proj_forward with invalid latitude: lon: %f lat: %f\n",
__FILE__, __LINE__, __FUNCTION__, xlon2, ylat2);
}
		mb_proj_forward(mbv_verbose, view->display_pjptr, xlon2, ylat2, &xx2, &yy2, &error);
		zz2 = zdata2;

		/* distance and slope */
		dx = xx2 - xx1;
		dy = yy2 - yy1;
		dz = zz2 - zz1;
		*distancelateral = sqrt(dx * dx + dy * dy);
		*distanceoverground = sqrt(dx * dx + dy * dy + dz * dz);
		if (*distancelateral > 0.0)
			*slope = dz / (*distancelateral);
		else
			*slope = 0.0;
	}
	else if (data->display_projection_mode == MBV_PROJECTION_GEOGRAPHIC) {
		/* point 1 */
		xx1 = xlon1 / view->mtodeglon;
		yy1 = ylat1 / view->mtodeglat;
		zz1 = zdata1;

		/* point 2 */
		xx2 = xlon2 / view->mtodeglon;
		yy2 = ylat2 / view->mtodeglat;
		zz2 = zdata2;

		/* distance and slope */
		dx = xx2 - xx1;
		dy = yy2 - yy1;
		dz = zz2 - zz1;
		*distancelateral = sqrt(dx * dx + dy * dy);
		*distanceoverground = sqrt(dx * dx + dy * dy + dz * dz);
		if (*distancelateral > 0.0)
			*slope = dz / (*distancelateral);
		else
			*slope = 0.0;
	}
	else if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) {
		/* point 1 */
		mbview_sphere_forward(instance, xlon1, ylat1, &xx1, &yy1, &zz1);

		/* point 2 */
		mbview_sphere_forward(instance, xlon2, ylat2, &xx2, &yy2, &zz2);

		/* lateral distance */
		mbview_greatcircle_distbearing(instance, xlon1, ylat1, xlon2, ylat2, &bearing, distancelateral);

		/* distance over ground */
		xx1 += zdata1 * xx1 / MBV_SPHEROID_RADIUS;
		yy1 += zdata1 * yy1 / MBV_SPHEROID_RADIUS;
		zz1 += zdata1 * zz1 / MBV_SPHEROID_RADIUS;
		xx2 += zdata2 * xx2 / MBV_SPHEROID_RADIUS;
		yy2 += zdata2 * yy2 / MBV_SPHEROID_RADIUS;
		zz2 += zdata2 * zz2 / MBV_SPHEROID_RADIUS;
		dx = xx2 - xx1;
		dy = yy2 - yy1;
		dz = zz2 - zz1;
		*distanceoverground = sqrt(dx * dx + dy * dy + dz * dz);

		/* slope */
		if (*distancelateral > 0.0)
			*slope = (zdata2 - zdata1) / (*distancelateral);
		else
			*slope = 0.0;
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       distancelateral:     %f\n", *distancelateral);
		fprintf(stderr, "dbg2       distanceoverground:  %f\n", *distanceoverground);
		fprintf(stderr, "dbg2       slope:               %f\n", *slope);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_sphere_setup(size_t instance, bool earthcentered, double xlon, double ylat) {
	double phi, theta, psi;
	int j;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       earthcentered:    %d\n", earthcentered);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* The initial spherical coordinate system is defined as:
	        x = r * cos(longitude) * cos(latitude)
	        y = r * sin(longitude) * cos(latitude)
	        z = r * sin(latitude)
	   which is equivalent to:
	        x = r * cos(longitude) * sin(colatitude)
	        y = r * sin(longitude) * sin(colatitude)
	        z = r * cos(colatitude)
	   where:
	        colatitude = PI/2 - latitude

	   Euler's rotation theorem proves than any general rotation may be
	   described by three successive rotations about the axes. One convention
	   is to use first a rotation about the z-axis (angle phi), then a
	   rotation about the x'-axis (angle theta), and finally a rotation
	   about the z''-axis (angle psi).

	   The euler rotation matrix becomes:
	        |	a11	a12	a13	|
	        |	a21	a22	a23	|
	        |	a31	a32	a33	|
	   where:
	        a11 = cos(phi) * cos(psi) - sin(phi) * cos(theta) * sin(psi)
	        a12 = sin(phi) * cos(psi) + cos(phi) * cos(theta) * sin(psi)
	        a13 = sin(theta) * sin (psi)
	        a21 = -cos(phi) * sin(psi) - sin(phi) * cos(theta) * cos(psi)
	        a22 = -sin(phi) * sin(psi) + cos(phi) * cos(theta) * cos(psi)
	        a23 = sin(theta) * cos(psi)
	        a31 = sin(phi) * sin(theta)
	        a32 = -cos(phi) * sin(theta)
	        a33 = cos(theta)

	   We wish to rotate the coordinate system so that the reference position
	   defined by xlon and ylat are located on the positive z-axis. The forward
	   rotation is accomplished using:
	        phi = -PI/2 + xlon
	        theta = -ycolat = ylat - PI/2
	        psi = PI
	   The reverse rotation is accomplished using:
	        phi = -PI
	        theta = ycolat = PI/2 - ylat
	        psi = xlon - PI/2

	  The relevant equations derived in part from:
	    http://mathworld.wolfram.com/EulerAngles.html
	  which were viewed on January 19, 2004
	    */

	/* create forward rotation matrix */
	phi = DTR * xlon - 0.5 * M_PI;
	theta = DTR * ylat - 0.5 * M_PI;
	psi = M_PI;
	mbview_sphere_matrix(phi, theta, psi, view->sphere_eulerforward);

	/* create reverse rotation matrix */
	phi = -M_PI;
	theta = 0.5 * M_PI - DTR * ylat;
	psi = 0.5 * M_PI - DTR * xlon;
	mbview_sphere_matrix(phi, theta, psi, view->sphere_eulerreverse);

	/* now get reference location in rotated coordinates */
	view->sphere_reflon = xlon;
	view->sphere_reflat = ylat;
	view->sphere_refx = 0.0;
	view->sphere_refy = 0.0;
	view->sphere_refz = 0.0;
	if (!earthcentered) {
		mbview_sphere_forward(instance, xlon, ylat, &view->sphere_refx, &view->sphere_refy, &view->sphere_refz);
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Internal results:\n");
		fprintf(stderr, "dbg2       view->sphere_reflon:      %f\n", view->sphere_reflon);
		fprintf(stderr, "dbg2       view->sphere_reflat:      %f\n", view->sphere_reflat);
		fprintf(stderr, "dbg2       view->sphere_refx:        %f\n", view->sphere_refx);
		fprintf(stderr, "dbg2       view->sphere_refy:        %f\n", view->sphere_refy);
		fprintf(stderr, "dbg2       view->sphere_refz:        %f\n", view->sphere_refz);
		fprintf(stderr, "dbg2       view->sphere_eulerforward:\n");
		for (j = 0; j < 3; j++) {
			fprintf(stderr, "dbg2                         %f %f %f\n", view->sphere_eulerforward[0 + 3 * j],
			        view->sphere_eulerforward[1 + 3 * j], view->sphere_eulerforward[2 + 3 * j]);
		}
		fprintf(stderr, "dbg2       view->sphere_eulerreverse:\n");
		for (j = 0; j < 3; j++) {
			fprintf(stderr, "dbg2                         %f %f %f\n", view->sphere_eulerreverse[0 + 3 * j],
			        view->sphere_eulerreverse[1 + 3 * j], view->sphere_eulerreverse[2 + 3 * j]);
		}
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_sphere_forward(size_t instance, double xlon, double ylat, double *xx, double *yy, double *zz) {
	double sinlon, coslon, sinlat, coslat;
	double posu[3], posr[3];

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get position in initial cartesian coordinates */
	sinlon = sin(DTR * xlon);
	coslon = cos(DTR * xlon);
	sinlat = sin(DTR * ylat);
	coslat = cos(DTR * ylat);
	posu[0] = MBV_SPHEROID_RADIUS * coslon * coslat;
	posu[1] = MBV_SPHEROID_RADIUS * sinlon * coslat;
	posu[2] = MBV_SPHEROID_RADIUS * sinlat;

	/* apply rotation to coordinates with the reference location
	    at the center of the view, on the positive z-axis. */
	mbview_sphere_rotate(view->sphere_eulerforward, posu, posr);

	/* make relative to reference location */
	*xx = posr[0];
	*yy = posr[1];
	*zz = posr[2];

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       posu[0]:     %f\n", posu[0]);
		fprintf(stderr, "dbg2       posu[1]:     %f\n", posu[1]);
		fprintf(stderr, "dbg2       posu[2]:     %f\n", posu[2]);
		fprintf(stderr, "dbg2       posr[0]:     %f\n", posr[0]);
		fprintf(stderr, "dbg2       posr[1]:     %f\n", posr[1]);
		fprintf(stderr, "dbg2       posr[2]:     %f\n", posr[2]);
		fprintf(stderr, "dbg2       xx:          %f\n", *xx);
		fprintf(stderr, "dbg2       yy:          %f\n", *yy);
		fprintf(stderr, "dbg2       zz:          %f\n", *zz);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_sphere_inverse(size_t instance, double xx, double yy, double zz, double *xlon, double *ylat) {
	double posu[3], posr[3];

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xx:               %f\n", xx);
		fprintf(stderr, "dbg2       yy:               %f\n", yy);
		fprintf(stderr, "dbg2       zz:               %f\n", zz);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get position in cartesian spheroid coordinates */
	posr[0] = xx;
	posr[1] = yy;
	posr[2] = zz;

	/* unrotate position */
	mbview_sphere_rotate(view->sphere_eulerreverse, posr, posu);

	/* get longitude and latitude */
	*xlon = RTD * atan2(posu[1], posu[0]);
	*ylat = 90.0 - RTD * (atan2(sqrt(posu[0] * posu[0] + posu[1] * posu[1]), posu[2]));

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       posr[0]:     %f\n", posr[0]);
		fprintf(stderr, "dbg2       posr[1]:     %f\n", posr[1]);
		fprintf(stderr, "dbg2       posr[2]:     %f\n", posr[2]);
		fprintf(stderr, "dbg2       posu[0]:     %f\n", posu[0]);
		fprintf(stderr, "dbg2       posu[1]:     %f\n", posu[1]);
		fprintf(stderr, "dbg2       posu[2]:     %f\n", posu[2]);
		fprintf(stderr, "dbg2       xlon:        %f\n", *xlon);
		fprintf(stderr, "dbg2       ylat:        %f\n", *ylat);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_sphere_matrix(double phi, double theta, double psi, double *eulermatrix) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       phi:              %f\n", phi);
		fprintf(stderr, "dbg2       theta:            %f\n", theta);
		fprintf(stderr, "dbg2       psi:              %f\n", psi);
	}

	/* The initial spherical coordinate system is defined as:
	        x = r * cos(longitude) * cos(latitude)
	        y = r * sin(longitude) * cos(latitude)
	        z = r * sin(latitude)
	   which is equivalent to:
	        x = r * cos(longitude) * sin(colatitude)
	        y = r * sin(longitude) * sin(colatitude)
	        z = r * cos(colatitude)
	   where:
	        colatitude = PI/2 - latitude

	   Euler's rotation theorem proves than any general rotation may be
	   described by three successive rotations about the axes. One convention
	   is to use first a rotation about the z-axis (angle phi), then a
	   rotation about the x'-axis (angle theta), and finally a rotation
	   about the z''-axis (angle psi).

	   The euler rotation matrix becomes:
	        |	a11	a12	a13	|
	        |	a21	a22	a23	|
	        |	a31	a32	a33	|
	   where:
	        a11 = cos(phi) * cos(psi) - sin(phi) * cos(theta) * sin(psi)
	        a12 = sin(phi) * cos(psi) + cos(phi) * cos(theta) * sin(psi)
	        a13 = sin(theta) * sin (psi)
	        a21 = -cos(phi) * sin(psi) - sin(phi) * cos(theta) * cos(psi)
	        a22 = -sin(phi) * sin(psi) + cos(phi) * cos(theta) * cos(psi)
	        a23 = sin(theta) * cos(psi)
	        a31 = sin(phi) * sin(theta)
	        a32 = -cos(phi) * sin(theta)
	        a33 = cos(theta)

	   We wish to rotate the coordinate system so that the reference position
	   defined by xlon and ylat are located on the positive z-axis. The forward
	   rotation is accomplished using:
	        phi = -PI/2 + xlon
	        theta = -ycolat = ylat - PI/2
	        psi = PI
	   The reverse rotation is accomplished using:
	        phi = -PI
	        theta = ycolat = PI/2 - ylat
	        psi = xlon - PI/2

	  The relevant equations derived in part from:
	    http://mathworld.wolfram.com/EulerAngles.html
	  which were viewed on January 19, 2004
	    */

	/* create forward rotation matrix */
	eulermatrix[0] = cos(phi) * cos(psi) - sin(phi) * cos(theta) * sin(psi);
	eulermatrix[1] = sin(phi) * cos(psi) + cos(phi) * cos(theta) * sin(psi);
	eulermatrix[2] = sin(theta) * sin(psi);
	eulermatrix[3] = -cos(phi) * sin(psi) - sin(phi) * cos(theta) * cos(psi);
	eulermatrix[4] = -sin(phi) * sin(psi) + cos(phi) * cos(theta) * cos(psi);
	eulermatrix[5] = sin(theta) * cos(psi);
	eulermatrix[6] = sin(phi) * sin(theta);
	eulermatrix[7] = -cos(phi) * sin(theta);
	eulermatrix[8] = cos(theta);

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       eulermatrix       %f %f %f\n", eulermatrix[0], eulermatrix[1], eulermatrix[2]);
		fprintf(stderr, "dbg2       eulermatrix       %f %f %f\n", eulermatrix[3], eulermatrix[4], eulermatrix[5]);
		fprintf(stderr, "dbg2       eulermatrix       %f %f %f\n", eulermatrix[6], eulermatrix[7], eulermatrix[8]);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_sphere_rotate(double *eulermatrix, double *v, double *vr) {
	int i, j;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       eulermatrix       %f %f %f\n", eulermatrix[0], eulermatrix[1], eulermatrix[2]);
		fprintf(stderr, "dbg2       eulermatrix       %f %f %f\n", eulermatrix[3], eulermatrix[4], eulermatrix[5]);
		fprintf(stderr, "dbg2       eulermatrix       %f %f %f\n", eulermatrix[6], eulermatrix[7], eulermatrix[8]);
		fprintf(stderr, "dbg2       -----------\n");
		fprintf(stderr, "dbg2       v:                %f %f %f\n", v[0], v[1], v[3]);
	}

	/* get original view direction in cartesian coordinates */
	for (i = 0; i < 3; i++)
		vr[i] = 0.0;
	for (j = 0; j < 3; j++) {
		for (i = 0; i < 3; i++) {
			vr[j] += v[i] * eulermatrix[i + 3 * j];
		}
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       vr:               %f %f %f\n", vr[0], vr[1], vr[3]);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_greatcircle_distbearing(size_t instance, double lon1, double lat1, double lon2, double lat2, double *bearing,
                                   double *distance) {
	double rlon1, rlat1, rlon2, rlat2, rbearing;
	double t1, t2, t3, dd;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       lon1:             %f\n", lon1);
		fprintf(stderr, "dbg2       lat1:             %f\n", lat1);
		fprintf(stderr, "dbg2       lon2:             %f\n", lon2);
		fprintf(stderr, "dbg2       lat2:             %f\n", lat2);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* note: these equations derive in part from source code read at:
	    http://simgear.org/doxygen/polar3d_8hxx-source.html
	    on 17 February 2004 by D.W. Caress
	    The source code found at this location is licensed under the LGPL */

	/* get great circle distance */
	rlon1 = DTR * lon1;
	rlat1 = DTR * lat1;
	rlon2 = DTR * lon2;
	rlat2 = DTR * lat2;
	t1 = sin(0.5 * (rlon1 - rlon2));
	t2 = sin(0.5 * (rlat1 - rlat2));
	dd = 2.0 * asin(sqrt(t2 * t2 + cos(rlat1) * cos(rlat2) * t1 * t1));
	*distance = MBV_SPHEROID_RADIUS * dd;

	/* get great circle bearing */

	/* first check if at poles */
	if (fabs(1.0 - sin(rlat1)) < 0.000001) {
		/* at north pole therefore heading south */
		if (lat1 > 0.0) {
			*bearing = 180.0;
		}

		/* at south pole therefore heading north */
		else {
			*bearing = 0.0;
		}
	}

	/* handle position away from poles */
	else {
		t3 = (sin(rlat2) - sin(rlat1) * cos(dd)) / (sin(dd) * cos(rlat1));
		rbearing = acos(MAX(MIN(t3, 1.0), -1.0));
		if (t1 <= 0.0) {
			*bearing = RTD * rbearing;
		}
		else {
			*bearing = 360.0 - RTD * rbearing;
		}
		if (*bearing < 0.0)
			*bearing += 360.0;
	}

	int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       t3:          %f\n", t3);
		fprintf(stderr, "dbg2       bearing:     %f\n", *bearing);
		fprintf(stderr, "dbg2       distance:    %f\n", *distance);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_greatcircle_dist(size_t instance, double lon1, double lat1, double lon2, double lat2, double *distance) {
	double rlon1, rlat1, rlon2, rlat2;
	double t1, t2, dd;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       lon1:             %f\n", lon1);
		fprintf(stderr, "dbg2       lat1:             %f\n", lat1);
		fprintf(stderr, "dbg2       lon2:             %f\n", lon2);
		fprintf(stderr, "dbg2       lat2:             %f\n", lat2);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* note: these equations derive in part from source code read at:
	    http://simgear.org/doxygen/polar3d_8hxx-source.html
	    on 17 February 2004 by D.W. Caress
	    The source code found at this location is licensed under the LGPL */

	/* get great circle distance */
	rlon1 = DTR * lon1;
	rlat1 = DTR * lat1;
	rlon2 = DTR * lon2;
	rlat2 = DTR * lat2;
	t1 = sin(0.5 * (rlon1 - rlon2));
	t2 = sin(0.5 * (rlat1 - rlat2));
	dd = 2.0 * asin(sqrt(t2 * t2 + cos(rlat1) * cos(rlat2) * t1 * t1));
	*distance = MBV_SPHEROID_RADIUS * dd;

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       distance:    %f\n", *distance);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_greatcircle_endposition(size_t instance, double lon1, double lat1, double bearing, double distance, double *lon2,
                                   double *lat2) {
	double rd, rbearing, rlon1, rlat1, rlat2;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       lon1:             %f\n", lon1);
		fprintf(stderr, "dbg2       lat1:             %f\n", lat1);
		fprintf(stderr, "dbg2       bearing:          %f\n", bearing);
		fprintf(stderr, "dbg2       distance:         %f\n", distance);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* note: these equations derive in part from source code read at:
	    http://simgear.org/doxygen/polar3d_8hxx-source.html
	    on 17 February 2004 by D.W. Caress
	    The source code found at this location is licensed under the LGPL */

	/* scale angles to radians */
	rd = distance / MBV_SPHEROID_RADIUS;
	rbearing = DTR * (360.0 - bearing);
	rlon1 = DTR * lon1;
	rlat1 = DTR * lat1;

	/* calculate latitude */
	rlat2 = asin(sin(rlat1) * cos(rd) + cos(rlat1) * sin(rd) * cos(rbearing));
	*lat2 = RTD * rlat2;

	/* calculate longitude */
	if (cos(rlat2) < 0.000001) {
		*lon2 = lon1;
	}
	else {
		*lon2 = RTD * (fmod(rlon1 - asin(sin(rbearing) * sin(rd) / cos(rlat2)) + M_PI, 2.0 * M_PI) - M_PI);
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return value:\n");
		fprintf(stderr, "dbg2       lon2:             %f\n", *lon2);
		fprintf(stderr, "dbg2       lat2:             %f\n", *lat2);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getzdata(size_t instance, double xgrid, double ygrid, bool *found, double *zdata) {
	int nsum;
	double zdatasum;
	int i, j, k, l, m, n;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       xgrid:            %f\n", xgrid);
		fprintf(stderr, "dbg2       ygrid:            %f\n", ygrid);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* get location in grid */
	i = (int)((xgrid - data->primary_xmin) / data->primary_dx);
	j = (int)((ygrid - data->primary_ymin) / data->primary_dy);

	/* fail if outside grid */
	if (i < 0 || i >= data->primary_n_columns - 1 || j < 0 || j >= data->primary_n_rows - 1) {
		*found = false;
		*zdata = 0.0;
	}

	/* check all four points and average the good ones */
	else {
		k = i * data->primary_n_rows + j;
		l = (i + 1) * data->primary_n_rows + j;
		m = i * data->primary_n_rows + j + 1;
		n = (i + 1) * data->primary_n_rows + j + 1;
		nsum = 0;
		zdatasum = 0.0;
		if (data->primary_data[k] != data->primary_nodatavalue) {
			zdatasum += data->primary_data[k];
			nsum++;
		}
		if (data->primary_data[l] != data->primary_nodatavalue) {
			zdatasum += data->primary_data[l];
			nsum++;
		}
		if (data->primary_data[m] != data->primary_nodatavalue) {
			zdatasum += data->primary_data[m];
			nsum++;
		}
		if (data->primary_data[n] != data->primary_nodatavalue) {
			zdatasum += data->primary_data[n];
			nsum++;
		}
		if (nsum > 0) {
			*zdata = zdatasum / nsum;
			*found = true;
		}
		else {
			*zdata = 0.0;
			*found = false;
		}
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       found:           %d\n", *found);
		fprintf(stderr, "dbg2       zdata:           %f\n", *zdata);
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_drapesegment(size_t instance, struct mbview_linesegment_struct *seg) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       seg:              %p\n", seg);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* only plot if this view is still active */
	if (view->glx_init) {

		/* if spheroid dipslay project on great circle arc */
		if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) {
			status = mbview_drapesegment_gc(instance, seg);
		}

		/* else project on straight lines in grid projection */
		else {
			status = mbview_drapesegment_grid(instance, seg);
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
		fprintf(stderr, "dbg2       seg->nls:        %d\n", seg->nls);
		fprintf(stderr, "dbg2       seg->nls_alloc:  %d\n", seg->nls_alloc);
		fprintf(stderr, "dbg2       seg->lspoints:\n");
		for (int i = 0; i < seg->nls; i++) {
			fprintf(stderr, "dbg2         point[%4d]:    %f %f %f  %f %f  %f %f %f\n", i, seg->lspoints[i].xgrid,
			        seg->lspoints[i].ygrid, seg->lspoints[i].zdata, seg->lspoints[i].xlon, seg->lspoints[i].ylat,
			        seg->lspoints[i].xdisplay, seg->lspoints[i].ydisplay, seg->lspoints[i].zdisplay);
		}
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_drapesegment_gc(size_t instance, struct mbview_linesegment_struct *seg) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       seg:              %p\n", seg);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	bool done = false;

	/* check if the contour offset needs to be applied in a global spherical direction or just up */
	const bool global =
		data->display_projection_mode == MBV_PROJECTION_SPHEROID &&
		view->sphere_refx == 0.0 && view->sphere_refy == 0.0 &&
		view->sphere_refz == 0.0;
	const double offset_factor =
		10.0 *
		(global
		 ? MBV_OPENGL_3D_CONTOUR_OFFSET / (view->scale * MBV_SPHEROID_RADIUS)
		 : MBV_OPENGL_3D_CONTOUR_OFFSET);

	double xlon1, ylat1;
	double xlon2, ylat2;

	/* get half characteristic distance between grid points
	    from center of primary grid */
	{
		const int i = data->primary_n_columns / 2;
		const int j = data->primary_n_rows / 2;
		mbview_projectgrid2ll(instance, (double)(data->primary_xmin + i * data->primary_dx),
	                      (double)(data->primary_ymin + j * data->primary_dy), &xlon1, &ylat1);
		mbview_projectgrid2ll(instance, (double)(data->primary_xmin + (i + 1) * data->primary_dx),
	                      (double)(data->primary_ymin + (j + 1) * data->primary_dy), &xlon2, &ylat2);
	}
	double dsegbearing;
	double dsegdist;
	mbview_greatcircle_distbearing(instance, xlon1, ylat1, xlon2, ylat2, &dsegbearing, &dsegdist);

	/* get number of preliminary points along the segment */
	double segbearing;
	double segdist;
	mbview_greatcircle_distbearing(instance, seg->endpoints[0].xlon, seg->endpoints[0].ylat, seg->endpoints[1].xlon,
	                               seg->endpoints[1].ylat, &segbearing, &segdist);
	const int nsegpoint = MAX(((int)((segdist / dsegdist) + 1)), 2);

	int status = MB_SUCCESS;

	/* no need to fill in if the segment doesn't cross grid boundaries */
	if (nsegpoint <= 2) {
		done = true;
		seg->nls = 0;
		seg->nls_alloc = 0;
	} else {
		/* get the points along the great circle arc */
		/* get effective distance between points along great circle */
		dsegdist = segdist / (nsegpoint - 1);

		/* allocate segment points */
		seg->nls_alloc = nsegpoint;
		int error = MB_ERROR_NO_ERROR;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, seg->nls_alloc * sizeof(struct mbview_point_struct),
		                     (void **)&(seg->lspoints), &error);
		if (status == MB_FAILURE) {
			done = true;
			seg->nls_alloc = 0;
			seg->nls = 0;
		}
	}

	/* now calculate points along great circle arc */
	if (seg->nls_alloc > 1 && !done) {
		/* put begin point in list */
		seg->nls = 0;
		seg->lspoints[seg->nls].xgrid = seg->endpoints[0].xgrid;
		seg->lspoints[seg->nls].ygrid = seg->endpoints[0].ygrid;
		seg->lspoints[seg->nls].zdata = seg->endpoints[0].zdata;
		seg->lspoints[seg->nls].xlon = seg->endpoints[0].xlon;
		seg->lspoints[seg->nls].ylat = seg->endpoints[0].ylat;
		seg->nls++;

		for (int i = 1; i < nsegpoint - 1; i++) {
			mbview_greatcircle_endposition(instance, seg->lspoints[0].xlon, seg->lspoints[0].ylat, segbearing,
			                               (double)(i * dsegdist), &(seg->lspoints[seg->nls].xlon),
			                               &(seg->lspoints[seg->nls].ylat)),
			    status = mbview_projectll2xyzgrid(instance, seg->lspoints[seg->nls].xlon, seg->lspoints[seg->nls].ylat,
			                                      &(seg->lspoints[seg->nls].xgrid), &(seg->lspoints[seg->nls].ygrid),
			                                      &(seg->lspoints[seg->nls].zdata));
			if (status == MB_SUCCESS) {
				seg->nls++;
			}
		}

		/* put end point in list */
		seg->lspoints[seg->nls].xgrid = seg->endpoints[1].xgrid;
		seg->lspoints[seg->nls].ygrid = seg->endpoints[1].ygrid;
		seg->lspoints[seg->nls].zdata = seg->endpoints[1].zdata;
		seg->lspoints[seg->nls].xlon = seg->endpoints[1].xlon;
		seg->lspoints[seg->nls].ylat = seg->endpoints[1].ylat;
		seg->nls++;

		/* now calculate rest of point values */
		for (int icnt = 0; icnt < seg->nls; icnt++) {
			mbview_projectll2display(instance, seg->lspoints[icnt].xlon, seg->lspoints[icnt].ylat, seg->lspoints[icnt].zdata,
			                         &(seg->lspoints[icnt].xdisplay), &(seg->lspoints[icnt].ydisplay),
			                         &(seg->lspoints[icnt].zdisplay));
			if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
				seg->lspoints[icnt].zdisplay += offset_factor;
			}
			else if (global) {
				seg->lspoints[icnt].xdisplay += seg->lspoints[icnt].xdisplay * offset_factor;
				seg->lspoints[icnt].ydisplay += seg->lspoints[icnt].ydisplay * offset_factor;
				seg->lspoints[icnt].zdisplay += seg->lspoints[icnt].zdisplay * offset_factor;
			}
			else {
				seg->lspoints[icnt].zdisplay += offset_factor;
			}
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
		fprintf(stderr, "dbg2       seg->nls:        %d\n", seg->nls);
		fprintf(stderr, "dbg2       seg->nls_alloc:  %d\n", seg->nls_alloc);
		fprintf(stderr, "dbg2       seg->lspoints:\n");
		for (int i = 0; i < seg->nls; i++) {
			fprintf(stderr, "dbg2         point[%4d]:    %f %f %f  %f %f  %f %f %f\n", i, seg->lspoints[i].xgrid,
			        seg->lspoints[i].ygrid, seg->lspoints[i].zdata, seg->lspoints[i].xlon, seg->lspoints[i].ylat,
			        seg->lspoints[i].xdisplay, seg->lspoints[i].ydisplay, seg->lspoints[i].zdisplay);
		}
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_drapesegment_grid(size_t instance, struct mbview_linesegment_struct *seg) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       seg:              %p\n", seg);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* check if the contour offset needs to be applied in a global spherical direction or just up */
	const bool global =
		data->display_projection_mode == MBV_PROJECTION_SPHEROID && view->sphere_refx == 0.0 &&
		view->sphere_refy == 0.0 && view->sphere_refz == 0.0;
	const double offset_factor =
		10.0 *
		(global
		 ? MBV_OPENGL_3D_CONTOUR_OFFSET / (view->scale * MBV_SPHEROID_RADIUS)
		 : MBV_OPENGL_3D_CONTOUR_OFFSET);

	/* figure out how many points to calculate along the segment */
	int istart = (int)((seg->endpoints[0].xgrid - data->primary_xmin) / data->primary_dx);
	int iend = (int)((seg->endpoints[1].xgrid - data->primary_xmin) / data->primary_dx);
	int jstart = (int)((seg->endpoints[0].ygrid - data->primary_ymin) / data->primary_dy);
	int jend = (int)((seg->endpoints[1].ygrid - data->primary_ymin) / data->primary_dy);

	int status = MB_SUCCESS;
	int error = MB_ERROR_NO_ERROR;
	int ni, nj;
	bool done = false;
	int iadd, jadd;

	/* no need to fill in if the segment doesn't cross grid boundaries */
	if (istart == iend && jstart == jend) {
		done = true;
		seg->nls = 0;
	} else {
		// allocate space for the array of points
		if (iend > istart) {
			ni = iend - istart;
			iadd = 1;
			istart++;
			// iend++;
		} else {
			ni = istart - iend;
			iadd = -1;
		}

		if (jend > jstart) {
			nj = jend - jstart;
			jadd = 1;
			jstart++;
			// jend++;
		} else {
			nj = jstart - jend;
			jadd = -1;
		}
		if ((ni + nj + 2) > seg->nls_alloc) {
			seg->nls_alloc = (ni + nj + 2);
			status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, seg->nls_alloc * sizeof(struct mbview_point_struct),
			                     (void **)&(seg->lspoints), &error);
			if (status == MB_FAILURE) {
				done = true;
				seg->nls_alloc = 0;
			}
		}
	}

	/* if points needed and space allocated do it */
	if (!done && ni + nj > 0) {
		/* put begin point in list */
		seg->nls = 0;
		seg->lspoints[seg->nls].xgrid = seg->endpoints[0].xgrid;
		seg->lspoints[seg->nls].ygrid = seg->endpoints[0].ygrid;
		seg->lspoints[seg->nls].zdata = seg->endpoints[0].zdata;
		seg->nls++;

		/* get line equation */
		double mm;
		double bb;
		if (ni > 0 && seg->endpoints[1].xgrid != seg->endpoints[0].xgrid) {
			mm = (seg->endpoints[1].ygrid - seg->endpoints[0].ygrid) / (seg->endpoints[1].xgrid - seg->endpoints[0].xgrid);
			bb = seg->endpoints[0].ygrid - mm * seg->endpoints[0].xgrid;
		}

		double xgrid, ygrid, zdata;

		/* loop over xgrid */
		int insert = 1;
		for (int icnt = 0; icnt < ni; icnt++) {
			const int i = istart + icnt * iadd;
			xgrid = data->primary_xmin + i * data->primary_dx;
			ygrid = mm * xgrid + bb;
			const int j = (int)((ygrid - data->primary_ymin) / data->primary_dy);
			const int k = i * data->primary_n_rows + j;
			const int l = i * data->primary_n_rows + j + 1;
			if (i >= 0 && i < data->primary_n_columns - 1 && j >= 0 && j < data->primary_n_rows - 1 &&
			    data->primary_data[k] != data->primary_nodatavalue && data->primary_data[l] != data->primary_nodatavalue) {
				/* interpolate zdata */
				zdata = data->primary_data[k] + (ygrid - data->primary_ymin - j * data->primary_dy) / data->primary_dy *
				                                    (data->primary_data[l] - data->primary_data[k]);

				/* add point to list */
				seg->lspoints[seg->nls].xgrid = xgrid;
				seg->lspoints[seg->nls].ygrid = ygrid;
				seg->lspoints[seg->nls].zdata = zdata;
				seg->nls++;
			}
		}

		/* put end point in list */
		seg->lspoints[seg->nls].xgrid = seg->endpoints[1].xgrid;
		seg->lspoints[seg->nls].ygrid = seg->endpoints[1].ygrid;
		seg->lspoints[seg->nls].zdata = seg->endpoints[1].zdata;
		seg->nls++;

		/* get line equation */
		if (nj > 0 && seg->endpoints[1].ygrid != seg->endpoints[0].ygrid) {
			mm = (seg->endpoints[1].xgrid - seg->endpoints[0].xgrid) / (seg->endpoints[1].ygrid - seg->endpoints[0].ygrid);
			bb = seg->endpoints[0].xgrid - mm * seg->endpoints[0].ygrid;
		}

		/* loop over ygrid */
		insert = 1;
		for (int jcnt = 0; jcnt < nj; jcnt++) {
			const int j = jstart + jcnt * jadd;
			ygrid = data->primary_ymin + j * data->primary_dy;
			xgrid = mm * ygrid + bb;
			const int i = (int)((xgrid - data->primary_xmin) / data->primary_dx);
			const int k = i * data->primary_n_rows + j;
			const int l = (i + 1) * data->primary_n_rows + j;
			if (i >= 0 && i < data->primary_n_columns - 1 && j >= 0 && j < data->primary_n_rows - 1 &&
			    data->primary_data[k] != data->primary_nodatavalue && data->primary_data[l] != data->primary_nodatavalue) {
				/* interpolate zdata */
				zdata = data->primary_data[k] + (xgrid - data->primary_xmin - i * data->primary_dx) / data->primary_dx *
				                                    (data->primary_data[l] - data->primary_data[k]);

				/* insert point into list */
				double found = false;
				done = false;
				if (jadd > 0)
					while (!done) {
						if (ygrid > seg->lspoints[insert - 1].ygrid && ygrid < seg->lspoints[insert].ygrid) {
							found = true;
							done = true;
						}
						else if (ygrid == seg->lspoints[insert - 1].ygrid || ygrid == seg->lspoints[insert].ygrid) {
							done = true;
						}
						else if (ygrid < seg->lspoints[insert - 1].ygrid) {
							insert--;
						}
						else if (ygrid > seg->lspoints[insert].ygrid) {
							insert++;
						}
						if (insert <= 0 || insert >= seg->nls) {
							done = true;
						}
					}
				else if (jadd < 0)
					while (!done) {
						if (ygrid > seg->lspoints[insert].ygrid && ygrid < seg->lspoints[insert - 1].ygrid) {
							found = true;
							done = true;
						}
						else if (ygrid == seg->lspoints[insert].ygrid || ygrid == seg->lspoints[insert - 1].ygrid) {
							done = true;
						}
						else if (ygrid > seg->lspoints[insert - 1].ygrid) {
							insert--;
						}
						else if (ygrid < seg->lspoints[insert].ygrid) {
							insert++;
						}
						if (insert <= 0 || insert >= seg->nls) {
							done = true;
						}
					}
				if (found) {
					for (int ii = seg->nls; ii > insert; ii--) {
						seg->lspoints[ii].xgrid = seg->lspoints[ii - 1].xgrid;
						seg->lspoints[ii].ygrid = seg->lspoints[ii - 1].ygrid;
						seg->lspoints[ii].zdata = seg->lspoints[ii - 1].zdata;
					}
					seg->lspoints[insert].xgrid = xgrid;
					seg->lspoints[insert].ygrid = ygrid;
					seg->lspoints[insert].zdata = zdata;
					seg->nls++;
				}
			}
		}

		// calculate rest of point values
		for (int icnt = 0; icnt < seg->nls; icnt++) {
			mbview_projectforward(instance, true, seg->lspoints[icnt].xgrid, seg->lspoints[icnt].ygrid,
			                      seg->lspoints[icnt].zdata, &(seg->lspoints[icnt].xlon), &(seg->lspoints[icnt].ylat),
			                      &(seg->lspoints[icnt].xdisplay), &(seg->lspoints[icnt].ydisplay),
			                      &(seg->lspoints[icnt].zdisplay));
			if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
				seg->lspoints[icnt].zdisplay += offset_factor;
			}
			else if (global) {
				seg->lspoints[icnt].xdisplay += seg->lspoints[icnt].xdisplay * offset_factor;
				seg->lspoints[icnt].ydisplay += seg->lspoints[icnt].ydisplay * offset_factor;
				seg->lspoints[icnt].zdisplay += seg->lspoints[icnt].zdisplay * offset_factor;
			}
			else {
				seg->lspoints[icnt].zdisplay += offset_factor;
			}
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
		fprintf(stderr, "dbg2       seg->nls:        %d\n", seg->nls);
		fprintf(stderr, "dbg2       seg->nls_alloc:  %d\n", seg->nls_alloc);
		fprintf(stderr, "dbg2       seg->lspoints:\n");
		for (int i = 0; i < seg->nls; i++) {
			fprintf(stderr, "dbg2         point[%4d]:    %f %f %f  %f %f  %f %f %f\n", i, seg->lspoints[i].xgrid,
			        seg->lspoints[i].ygrid, seg->lspoints[i].zdata, seg->lspoints[i].xlon, seg->lspoints[i].ylat,
			        seg->lspoints[i].xdisplay, seg->lspoints[i].ydisplay, seg->lspoints[i].zdisplay);
		}
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_drapesegmentw(size_t instance, struct mbview_linesegmentw_struct *seg) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       seg:              %p\n", seg);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* if spheroid dipslay project on great circle arc */
	if (data->display_projection_mode == MBV_PROJECTION_SPHEROID) {
		status = mbview_drapesegmentw_gc(instance, seg);
	}

	/* else project on straight lines in grid projection */
	else {
		status = mbview_drapesegmentw_grid(instance, seg);
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
		fprintf(stderr, "dbg2       seg->nls:        %d\n", seg->nls);
		fprintf(stderr, "dbg2       seg->nls_alloc:  %d\n", seg->nls_alloc);
		fprintf(stderr, "dbg2       seg->lspoints:\n");
		for (int i = 0; i < seg->nls; i++) {
			fprintf(stderr, "dbg2         point[%4d]:    %f %f %f  %f %f  %f %f %f\n", i, seg->lspoints[i].xgrid[instance],
			        seg->lspoints[i].ygrid[instance], seg->lspoints[i].zdata, seg->lspoints[i].xlon, seg->lspoints[i].ylat,
			        seg->lspoints[i].xdisplay[instance], seg->lspoints[i].ydisplay[instance],
			        seg->lspoints[i].zdisplay[instance]);
		}
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_drapesegmentw_gc(size_t instance, struct mbview_linesegmentw_struct *seg) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       seg:              %p\n", seg);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* check if the contour offset needs to be applied in a global spherical direction or just up */
	const bool global =
		data->display_projection_mode == MBV_PROJECTION_SPHEROID &&
		view->sphere_refx == 0.0 && view->sphere_refy == 0.0 && view->sphere_refz == 0.0;
	const double offset_factor =
		10.0 *
		(global
		 ? MBV_OPENGL_3D_CONTOUR_OFFSET / (view->scale * MBV_SPHEROID_RADIUS)
		 : MBV_OPENGL_3D_CONTOUR_OFFSET);

	/* get half characteristic distance between grid points
	    from center of primary grid */
	double xlon1, ylat1, xlon2, ylat2;
	{
		const int i = data->primary_n_columns / 2;
		const int j = data->primary_n_rows / 2;
		mbview_projectgrid2ll(instance, (double)(data->primary_xmin + i * data->primary_dx),
	                      (double)(data->primary_ymin + j * data->primary_dy), &xlon1, &ylat1);
		mbview_projectgrid2ll(instance, (double)(data->primary_xmin + (i + 1) * data->primary_dx),
	                      (double)(data->primary_ymin + (j + 1) * data->primary_dy), &xlon2, &ylat2);
        }
	double dsegbearing;
	double dsegdist;
	mbview_greatcircle_distbearing(instance, xlon1, ylat1, xlon2, ylat2, &dsegbearing, &dsegdist);

	/* get number of preliminary points along the segment */
	double segbearing, segdist;
	mbview_greatcircle_distbearing(instance, seg->endpoints[0].xlon, seg->endpoints[0].ylat, seg->endpoints[1].xlon,
	                               seg->endpoints[1].ylat, &segbearing, &segdist);
	const int nsegpoint = MAX(((int)((segdist / dsegdist) + 1)), 2);

	int status = MB_SUCCESS;

	bool done = false;
	/* no need to fill in if the segment doesn't cross grid boundaries */
	if (nsegpoint <= 2) {
		done = true;
		seg->nls = 0;
		seg->nls_alloc = 0;
	} else {
		/* get the points along the great circle arc */

		/* get effective distance between points along great circle */
		dsegdist = segdist / (nsegpoint - 1);

		/* allocate segment points */
		seg->nls_alloc = nsegpoint;
	int error = MB_ERROR_NO_ERROR;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, seg->nls_alloc * sizeof(struct mbview_pointw_struct),
		                     (void **)&(seg->lspoints), &error);
		if (status == MB_FAILURE) {
			done = true;
			seg->nls_alloc = 0;
			seg->nls = 0;
		}
	}

	/* now calculate points along great circle arc */
	if (seg->nls_alloc > 1 && !done) {
		/* put begin point in list */
		seg->nls = 0;
		seg->lspoints[seg->nls].xgrid[instance] = seg->endpoints[0].xgrid[instance];
		seg->lspoints[seg->nls].ygrid[instance] = seg->endpoints[0].ygrid[instance];
		seg->lspoints[seg->nls].zdata = seg->endpoints[0].zdata;
		seg->lspoints[seg->nls].xlon = seg->endpoints[0].xlon;
		seg->lspoints[seg->nls].ylat = seg->endpoints[0].ylat;
		seg->nls++;

		for (int i = 1; i < nsegpoint - 1; i++) {
			mbview_greatcircle_endposition(instance, seg->lspoints[0].xlon, seg->lspoints[0].ylat, segbearing,
			                               (double)(i * dsegdist), &(seg->lspoints[seg->nls].xlon),
			                               &(seg->lspoints[seg->nls].ylat)),
			    status = mbview_projectll2xyzgrid(instance, seg->lspoints[seg->nls].xlon, seg->lspoints[seg->nls].ylat,
			                                      &(seg->lspoints[seg->nls].xgrid[instance]),
			                                      &(seg->lspoints[seg->nls].ygrid[instance]), &(seg->lspoints[seg->nls].zdata));
			if (status == MB_SUCCESS) {
				seg->nls++;
			}
		}

		/* put end point in list */
		seg->lspoints[seg->nls].xgrid[instance] = seg->endpoints[1].xgrid[instance];
		seg->lspoints[seg->nls].ygrid[instance] = seg->endpoints[1].ygrid[instance];
		seg->lspoints[seg->nls].zdata = seg->endpoints[1].zdata;
		seg->lspoints[seg->nls].xlon = seg->endpoints[1].xlon;
		seg->lspoints[seg->nls].ylat = seg->endpoints[1].ylat;
		seg->nls++;

		/* now calculate rest of point values */
		for (int icnt = 0; icnt < seg->nls; icnt++) {
			mbview_projectll2display(instance, seg->lspoints[icnt].xlon, seg->lspoints[icnt].ylat, seg->lspoints[icnt].zdata,
			                         &(seg->lspoints[icnt].xdisplay[instance]), &(seg->lspoints[icnt].ydisplay[instance]),
			                         &(seg->lspoints[icnt].zdisplay[instance]));
			if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
				seg->lspoints[icnt].zdisplay[instance] += offset_factor;
			}
			else if (global) {
				seg->lspoints[icnt].xdisplay[instance] += seg->lspoints[icnt].xdisplay[instance] * offset_factor;
				seg->lspoints[icnt].ydisplay[instance] += seg->lspoints[icnt].ydisplay[instance] * offset_factor;
				seg->lspoints[icnt].zdisplay[instance] += seg->lspoints[icnt].zdisplay[instance] * offset_factor;
			}
			else {
				seg->lspoints[icnt].zdisplay[instance] += offset_factor;
			}
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[0]:     %f\n", seg->endpoints[0].xlon);
		fprintf(stderr, "dbg2            ylat[0]:     %f\n", seg->endpoints[0].ylat);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid[instance]);
		fprintf(stderr, "dbg2            xlon[1]:     %f\n", seg->endpoints[1].xlon);
		fprintf(stderr, "dbg2            ylat[1]:     %f\n", seg->endpoints[1].ylat);
		fprintf(stderr, "dbg2       seg->nls:        %d\n", seg->nls);
		fprintf(stderr, "dbg2       seg->nls_alloc:  %d\n", seg->nls_alloc);
		fprintf(stderr, "dbg2       seg->lspoints:\n");
		for (int i = 0; i < seg->nls; i++) {
			fprintf(stderr, "dbg2         point[%4d]:    %f %f %f  %f %f  %f %f %f\n", i, seg->lspoints[i].xgrid[instance],
			        seg->lspoints[i].ygrid[instance], seg->lspoints[i].zdata, seg->lspoints[i].xlon, seg->lspoints[i].ylat,
			        seg->lspoints[i].xdisplay[instance], seg->lspoints[i].ydisplay[instance],
			        seg->lspoints[i].zdisplay[instance]);
		}
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_drapesegmentw_grid(size_t instance, struct mbview_linesegmentw_struct *seg) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       seg:              %p\n", seg);
		fprintf(stderr, "dbg2       seg->endpoints:\n");
		fprintf(stderr, "dbg2            xgrid[0]:    %f\n", seg->endpoints[0].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[0]:    %f\n", seg->endpoints[0].ygrid[instance]);
		fprintf(stderr, "dbg2            xgrid[1]:    %f\n", seg->endpoints[1].xgrid[instance]);
		fprintf(stderr, "dbg2            ygrid[1]:    %f\n", seg->endpoints[1].ygrid[instance]);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* check if the contour offset needs to be applied in a global spherical direction or just up */
	int global;
	double offset_factor;
	if (data->display_projection_mode == MBV_PROJECTION_SPHEROID && view->sphere_refx == 0.0 && view->sphere_refy == 0.0 &&
	    view->sphere_refz == 0.0) {
		global = true;
		offset_factor = 10.0 * MBV_OPENGL_3D_CONTOUR_OFFSET / (view->scale * MBV_SPHEROID_RADIUS);
	}
	else {
		global = false;
		offset_factor = 10.0 * MBV_OPENGL_3D_CONTOUR_OFFSET;
	}

	/* figure out how many points to calculate along the segment */
	const double xgridstart = seg->endpoints[0].xgrid[instance];
	const double xgridend = seg->endpoints[1].xgrid[instance];
	const double ygridstart = seg->endpoints[0].ygrid[instance];
	const double ygridend = seg->endpoints[1].ygrid[instance];
	int istart = (int)((xgridstart - data->primary_xmin) / data->primary_dx);
	int iend = (int)((xgridend - data->primary_xmin) / data->primary_dx);
	int jstart = (int)((ygridstart - data->primary_ymin) / data->primary_dy);
	int jend = (int)((ygridend - data->primary_ymin) / data->primary_dy);
	if (istart < 0)
		istart = 0;
	if (istart >= data->primary_n_columns)
		istart = data->primary_n_columns - 1;
	if (iend < 0)
		iend = 0;
	if (iend >= data->primary_n_columns)
		iend = data->primary_n_columns - 1;
	if (jstart < 0)
		jstart = 0;
	if (jstart >= data->primary_n_rows)
		jstart = data->primary_n_rows - 1;
	if (jend < 0)
		jend = 0;
	if (jend >= data->primary_n_rows)
		jend = data->primary_n_rows - 1;

	int iadd;
	int jadd;
	int ni;
	int nj;
	int status = MB_SUCCESS;

	/* no need to fill in if the segment doesn't cross grid boundaries */
	bool done = false;
	if (istart == iend && jstart == jend) {
		done = true;
		seg->nls = 0;
	} else {
		/* else allocate space for the array of points */

		/* allocate space for the array of points */
		if (iend > istart) {
			ni = iend - istart;
			iadd = 1;
			istart++;
			// iend++;
		} else {
			ni = istart - iend;
			iadd = -1;
		} if (jend > jstart) {
			nj = jend - jstart;
			jadd = 1;
			jstart++;
			// jend++;
		} else {
			nj = jstart - jend;
			jadd = -1;
		}
		if ((ni + nj + 2) > seg->nls_alloc) {
			seg->nls_alloc = (ni + nj + 2);
			int error = MB_ERROR_NO_ERROR;
			status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, seg->nls_alloc * sizeof(struct mbview_pointw_struct),
			                     (void **)&(seg->lspoints), &error);
			if (status == MB_FAILURE) {
				done = true;
				seg->nls_alloc = 0;
			}
		}
	}

	/* if points needed and space allocated do it */
	if (!done && ni + nj > 0) {
		/* put begin point in list */
		seg->nls = 0;
		seg->lspoints[seg->nls].xgrid[instance] = seg->endpoints[0].xgrid[instance];
		seg->lspoints[seg->nls].ygrid[instance] = seg->endpoints[0].ygrid[instance];
		seg->lspoints[seg->nls].zdata = seg->endpoints[0].zdata;
		seg->nls++;

		/* get line equation */
		double mm, bb;
		if (ni > 0 && seg->endpoints[1].xgrid[instance] != seg->endpoints[0].xgrid[instance]) {
			mm = (seg->endpoints[1].ygrid[instance] - seg->endpoints[0].ygrid[instance]) /
			     (seg->endpoints[1].xgrid[instance] - seg->endpoints[0].xgrid[instance]);
			bb = seg->endpoints[0].ygrid[instance] - mm * seg->endpoints[0].xgrid[instance];
		}

		/* loop over xgrid */
		for (int icnt = 0; icnt < ni; icnt++) {
			const int i = istart + icnt * iadd;
			const double xgrid = data->primary_xmin + i * data->primary_dx;
			const double ygrid = mm * xgrid + bb;
			const int j = (int)((ygrid - data->primary_ymin) / data->primary_dy);
			const int k = i * data->primary_n_rows + j;
			const int l = i * data->primary_n_rows + j + 1;
			if (i >= 0 && i < data->primary_n_columns - 1 && j >= 0 && j < data->primary_n_rows - 1 &&
			    data->primary_data[k] != data->primary_nodatavalue && data->primary_data[l] != data->primary_nodatavalue) {
				/* interpolate zdata */
				const double zdata = data->primary_data[k] + (ygrid - data->primary_ymin - j * data->primary_dy) / data->primary_dy *
				                                    (data->primary_data[l] - data->primary_data[k]);

				/* add point to list */
				seg->lspoints[seg->nls].xgrid[instance] = xgrid;
				seg->lspoints[seg->nls].ygrid[instance] = ygrid;
				seg->lspoints[seg->nls].zdata = zdata;
				seg->nls++;
				/*fprintf(stderr,"new ni point: nls:%d icnt:%d i:%d j:%d k:%d l:%d xgrid:%f ygrid:%f zdata:%f\n",
				seg->nls,icnt,i,j,k,l,xgrid,ygrid,zdata);*/
			}
		}

		/* put end point in list */
		seg->lspoints[seg->nls].xgrid[instance] = seg->endpoints[1].xgrid[instance];
		seg->lspoints[seg->nls].ygrid[instance] = seg->endpoints[1].ygrid[instance];
		seg->lspoints[seg->nls].zdata = seg->endpoints[1].zdata;
		seg->nls++;

		/* get line equation */
		if (nj > 0 && seg->endpoints[1].ygrid[instance] != seg->endpoints[0].ygrid[instance]) {
			mm = (seg->endpoints[1].xgrid[instance] - seg->endpoints[0].xgrid[instance]) /
			     (seg->endpoints[1].ygrid[instance] - seg->endpoints[0].ygrid[instance]);
			bb = seg->endpoints[0].xgrid[instance] - mm * seg->endpoints[0].ygrid[instance];
		}

		/* loop over ygrid */
		int insert = 1;
		for (int jcnt = 0; jcnt < nj; jcnt++) {
			const int j = jstart + jcnt * jadd;
			const double ygrid = data->primary_ymin + j * data->primary_dy;
			const double xgrid = mm * ygrid + bb;
			const int i = (int)((xgrid - data->primary_xmin) / data->primary_dx);
			const int k = i * data->primary_n_rows + j;
			const int l = (i + 1) * data->primary_n_rows + j;
			if (i >= 0 && i < data->primary_n_columns - 1 && j >= 0 && j < data->primary_n_rows - 1 &&
			    data->primary_data[k] != data->primary_nodatavalue && data->primary_data[l] != data->primary_nodatavalue) {
				/* interpolate zdata */
				const double zdata = data->primary_data[k] + (xgrid - data->primary_xmin - i * data->primary_dx) / data->primary_dx *
				                                    (data->primary_data[l] - data->primary_data[k]);

				/* insert point into list */
				bool found = false;
				done = false;
				if (jadd > 0)
					while (!done) {
						if (ygrid > seg->lspoints[insert - 1].ygrid[instance] && ygrid < seg->lspoints[insert].ygrid[instance]) {
							found = true;
							done = true;
						}
						else if (ygrid == seg->lspoints[insert - 1].ygrid[instance] ||
						         ygrid == seg->lspoints[insert].ygrid[instance]) {
							done = true;
						}
						else if (ygrid < seg->lspoints[insert - 1].ygrid[instance]) {
							insert--;
						}
						else if (ygrid > seg->lspoints[insert].ygrid[instance]) {
							insert++;
						}
						if (insert <= 0 || insert >= seg->nls) {
							done = true;
						}
						/*fprintf(stderr,"jadd>0: insert:%d found:%d done:%d\n",insert,found,done);*/
					}
				else if (jadd < 0)
					while (!done) {
						if (ygrid > seg->lspoints[insert].ygrid[instance] && ygrid < seg->lspoints[insert - 1].ygrid[instance]) {
							found = true;
							done = true;
						}
						else if (ygrid == seg->lspoints[insert].ygrid[instance] ||
						         ygrid == seg->lspoints[insert - 1].ygrid[instance]) {
							done = true;
						}
						else if (ygrid > seg->lspoints[insert - 1].ygrid[instance]) {
							insert--;
						}
						else if (ygrid < seg->lspoints[insert].ygrid[instance]) {
							insert++;
						}
						if (insert <= 0 || insert >= seg->nls) {
							done = true;
						}
						/*fprintf(stderr,"jadd<0: insert:%d found:%d done:%d\n",insert,found,done);*/
					}
				if (insert < 0)
					insert = 0;
				else if (insert > seg->nls)
					insert = seg->nls;
				if (found) {
					for (int ii = seg->nls; ii > insert; ii--) {
						seg->lspoints[ii].xgrid[instance] = seg->lspoints[ii - 1].xgrid[instance];
						seg->lspoints[ii].ygrid[instance] = seg->lspoints[ii - 1].ygrid[instance];
						seg->lspoints[ii].zdata = seg->lspoints[ii - 1].zdata;
					}
					seg->lspoints[insert].xgrid[instance] = xgrid;
					seg->lspoints[insert].ygrid[instance] = ygrid;
					seg->lspoints[insert].zdata = zdata;
					seg->nls++;
					/*fprintf(stderr,"new nj point: nls:%d jcnt:%d insert:%d jadd:%d i:%d j:%d k:%d l:%d xgrid:%f ygrid:%f
					zdata:%f\n", seg->nls,jcnt,insert,jadd,i,j,k,l,xgrid,ygrid,zdata);*/
				}
				if (insert <= 0)
					insert = 1;
				else if (insert >= seg->nls)
					insert = seg->nls - 1;
			}
		}

		/* now calculate rest of point values */
		for (int icnt = 0; icnt < seg->nls; icnt++) {
			mbview_projectforward(instance, true, seg->lspoints[icnt].xgrid[instance], seg->lspoints[icnt].ygrid[instance],
			                      seg->lspoints[icnt].zdata, &(seg->lspoints[icnt].xlon), &(seg->lspoints[icnt].ylat),
			                      &(seg->lspoints[icnt].xdisplay[instance]), &(seg->lspoints[icnt].ydisplay[instance]),
			                      &(seg->lspoints[icnt].zdisplay[instance]));
			if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
				seg->lspoints[icnt].zdisplay[instance] += offset_factor;
			}
			else if (global) {
				seg->lspoints[icnt].xdisplay[instance] += seg->lspoints[icnt].xdisplay[instance] * offset_factor;
				seg->lspoints[icnt].ydisplay[instance] += seg->lspoints[icnt].ydisplay[instance] * offset_factor;
				seg->lspoints[icnt].zdisplay[instance] += seg->lspoints[icnt].zdisplay[instance] * offset_factor;
			}
			else {
				seg->lspoints[icnt].zdisplay[instance] += offset_factor;
			}
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", (char *) __FUNCTION__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
		fprintf(stderr, "dbg2       seg->nls:        %d\n", seg->nls);
		fprintf(stderr, "dbg2       seg->nls_alloc:  %d\n", seg->nls_alloc);
		fprintf(stderr, "dbg2       seg->lspoints:\n");
		for (int i = 0; i < seg->nls; i++) {
			fprintf(stderr, "dbg2         point[%4d]:    %f %f %f  %f %f  %f %f %f\n", i, seg->lspoints[i].xgrid[instance],
			        seg->lspoints[i].ygrid[instance], seg->lspoints[i].zdata, seg->lspoints[i].xlon, seg->lspoints[i].ylat,
			        seg->lspoints[i].xdisplay[instance], seg->lspoints[i].ydisplay[instance],
			        seg->lspoints[i].zdisplay[instance]);
		}
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getroutecount(int verbose, size_t instance, int *nroute, int *error) {
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get number of routes */
	*nroute = shared.shareddata.nroute;

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nroute:                    %d\n", *nroute);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getroutepointcount(int verbose, size_t instance, int route, int *npoint, int *nintpoint, int *error) {
	/* local variables */
	int status = MB_SUCCESS;
	int i;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       route:                     %d\n", route);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get number of points in specified route */
	*npoint = 0;
	*nintpoint = 0;
	if (route >= 0 && route < shared.shareddata.nroute) {
		*npoint = shared.shareddata.routes[route].npoints;
		for (i = 0; i < *npoint - 1; i++) {
			if (shared.shareddata.routes[route].segments[i].nls > 2)
				*nintpoint += shared.shareddata.routes[route].segments[i].nls - 2;
		}
	}

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       npoint:                    %d\n", *npoint);
		fprintf(stderr, "dbg2       nintpoint:                 %d\n", *nintpoint);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getrouteselected(int verbose, size_t instance, int route, bool *selected, int *error) {
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       route:                     %d\n", route);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* check if the specified route is currently selected in totality */
	if (route == shared.shareddata.route_selected && shared.shareddata.route_point_selected == MBV_SELECT_ALL)
		*selected = true;
	else
		*selected = false;

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       selected:                  %d\n", *selected);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getrouteinfo(int verbose, size_t instance, int working_route, int *nroutewaypoint, int *nroutpoint, char *routename,
                        int *routecolor, int *routesize, double *routedistancelateral, double *routedistancetopo, int *error) {
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_route_struct *route;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       working_route:             %d\n", working_route);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* check that the route is valid */
	if (working_route < 0 || working_route >= shared.shareddata.nroute) {
		*nroutewaypoint = 0;
		*nroutpoint = 0;
		routename[0] = '\0';
		*routecolor = 0;
		*routesize = 0;
		*routedistancelateral = 0.0;
		*routedistancetopo = 0.0;
		status = MB_FAILURE;
		*error = MB_ERROR_DATA_NOT_INSERTED;
	}

	/* otherwise go get the route data */
	else {
		/* get basic info */
		route = &(shared.shareddata.routes[working_route]);
		*nroutewaypoint = route->npoints;
		*nroutpoint = route->nroutepoint;
		strcpy(routename, route->name);
		*routecolor = route->color;
		*routesize = route->size;
		*routedistancelateral = route->distancelateral;
		*routedistancetopo = route->distancetopo;
	}

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nroutewaypoint:            %d\n", *nroutewaypoint);
		fprintf(stderr, "dbg2       nroutpoint:                %d\n", *nroutpoint);
		fprintf(stderr, "dbg2       routename:                 %d\n", *routename);
		fprintf(stderr, "dbg2       routecolor:                %d\n", *routecolor);
		fprintf(stderr, "dbg2       routesize:                 %d\n", *routesize);
		fprintf(stderr, "dbg2       routedistancelateral:      %f\n", *routedistancelateral);
		fprintf(stderr, "dbg2       routedistancetopo:         %f\n", *routedistancetopo);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_allocroutearrays(int verbose, int npointtotal, double **routelon, double **routelat, int **waypoint,
                            double **routetopo, double **routebearing, double **distlateral, double **distovertopo,
                            double **slope, int *error) {
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       npointtotal:               %d\n", npointtotal);
		fprintf(stderr, "dbg2       routelon:                  %p\n", *routelon);
		fprintf(stderr, "dbg2       routelat:                  %p\n", *routelat);
		if (waypoint != NULL)
			fprintf(stderr, "dbg2       waypoint:                  %p\n", *waypoint);
		if (routetopo != NULL)
			fprintf(stderr, "dbg2       routetopo:                 %p\n", *routetopo);
		if (routebearing != NULL)
			fprintf(stderr, "dbg2       routebearing:              %p\n", *routebearing);
		if (distlateral != NULL)
			fprintf(stderr, "dbg2       distlateral:               %p\n", *distlateral);
		if (distovertopo != NULL)
			fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		if (slope != NULL)
			fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
	}

	/* allocate the arrays using mb_reallocd */
	status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)routelon, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)routelat, error);
	if (status == MB_SUCCESS && waypoint != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(int), (void **)waypoint, error);
	if (status == MB_SUCCESS && routetopo != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)routetopo, error);
	if (status == MB_SUCCESS && routebearing != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)routebearing, error);
	if (status == MB_SUCCESS && distlateral != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)distlateral, error);
	if (status == MB_SUCCESS && distovertopo != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)distovertopo, error);
	if (status == MB_SUCCESS && slope != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)slope, error);

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       routelon:                  %p\n", *routelon);
		fprintf(stderr, "dbg2       routelat:                  %p\n", *routelat);
		if (waypoint != NULL)
			fprintf(stderr, "dbg2       waypoint:                  %p\n", *waypoint);
		if (routetopo != NULL)
			fprintf(stderr, "dbg2       routetopo:                 %p\n", *routetopo);
		if (routebearing != NULL)
			fprintf(stderr, "dbg2       routebearing:              %p\n", *routebearing);
		if (distlateral != NULL)
			fprintf(stderr, "dbg2       distlateral:               %p\n", *distlateral);
		if (distovertopo != NULL)
			fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		if (slope != NULL)
			fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_freeroutearrays(int verbose, double **routelon, double **routelat, int **waypoint, double **routetopo,
                           double **routebearing, double **distlateral, double **distovertopo, double **slope, int *error) {
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       routelon:                  %p\n", *routelon);
		fprintf(stderr, "dbg2       routelat:                  %p\n", *routelat);
		if (waypoint != NULL)
			fprintf(stderr, "dbg2       waypoint:                  %p\n", *waypoint);
		if (routetopo != NULL)
			fprintf(stderr, "dbg2       routetopo:                 %p\n", *routetopo);
		if (routebearing != NULL)
			fprintf(stderr, "dbg2       routebearing:              %p\n", *routebearing);
		if (distlateral != NULL)
			fprintf(stderr, "dbg2       distlateral:               %p\n", *distlateral);
		if (distovertopo != NULL)
			fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		if (slope != NULL)
			fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
	}

	/* free the arrays using mb_freed */
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)routelon, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)routelat, error);
	if (waypoint != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)waypoint, error);
	if (routetopo != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)routetopo, error);
	if (routebearing != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)routebearing, error);
	if (distlateral != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)distlateral, error);
	if (distovertopo != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)distovertopo, error);
	if (slope != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)slope, error);

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       routelon:                  %p\n", *routelon);
		fprintf(stderr, "dbg2       routelat:                  %p\n", *routelat);
		if (waypoint != NULL)
			fprintf(stderr, "dbg2       waypoint:                  %p\n", *waypoint);
		if (routetopo != NULL)
			fprintf(stderr, "dbg2       routetopo:                 %p\n", *routetopo);
		if (routebearing != NULL)
			fprintf(stderr, "dbg2       routebearing:              %p\n", *routebearing);
		if (distlateral != NULL)
			fprintf(stderr, "dbg2       distlateral:               %p\n", *distlateral);
		if (distovertopo != NULL)
			fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		if (slope != NULL)
			fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_addroute(int verbose, size_t instance, int npoint, double *routelon, double *routelat, int *waypoint, int routecolor,
                    int routesize, int routeeditmode, mb_path routename, int *iroute, int *error) {
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	double xgrid, ygrid, zdata;
	double xdisplay, ydisplay, zdisplay;
	int i;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       npoint:                    %d\n", npoint);
		fprintf(stderr, "dbg2       routelon:                  %p\n", routelon);
		fprintf(stderr, "dbg2       routelat:                  %p\n", routelat);
		fprintf(stderr, "dbg2       waypoint:                  %p\n", waypoint);
		for (i = 0; i < npoint; i++) {
			fprintf(stderr, "dbg2       point:%d lon:%f lat:%f waypoint:%d\n", i, routelon[i], routelat[i], waypoint[i]);
		}
		fprintf(stderr, "dbg2       routecolor:                %d\n", routecolor);
		fprintf(stderr, "dbg2       routesize:                 %d\n", routesize);
		fprintf(stderr, "dbg2       routeeditmode:             %d\n", routeeditmode);
		fprintf(stderr, "dbg2       routename:                 %s\n", routename);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	/* make sure no route is selected */
	shared.shareddata.route_selected = MBV_SELECT_NONE;
	shared.shareddata.route_point_selected = MBV_SELECT_NONE;

	/* set route id so that new route is created */
	*iroute = shared.shareddata.nroute;

	/* loop over the points in the new route */
	for (i = 0; i < npoint; i++) {
		/* check waypoint flag correct */
		if (waypoint[i] <= MBV_ROUTE_WAYPOINT_NONE || waypoint[i] > MBV_ROUTE_WAYPOINT_ENDLINE5)
			waypoint[i] = MBV_ROUTE_WAYPOINT_SIMPLE;

		/* get route positions in grid coordinates */
		status = mbview_projectll2xyzgrid(instance, routelon[i], routelat[i], &xgrid, &ygrid, &zdata);

		/* get route positions in display coordinates */
		status = mbview_projectll2display(instance, routelon[i], routelat[i], zdata, &xdisplay, &ydisplay, &zdisplay);
		
		if (isnan(xdisplay)) {
			mbv_verbose = 5;
			status = mbview_projectll2display(instance, routelon[i], routelat[i], zdata, &xdisplay, &ydisplay, &zdisplay);
			mbv_verbose = 0;
		}

		/* check for reasonable coordinates */
		if (fabs(xdisplay) < 1000.0 && fabs(ydisplay) < 1000.0 && fabs(zdisplay) < 1000.0) {

			/* add the route point */
			mbview_route_add(mbv_verbose, instance, *iroute, i, waypoint[i], xgrid, ygrid, routelon[i], routelat[i], zdata,
			                 xdisplay, ydisplay, zdisplay);
		}

		/* report failure due to unreasonable coordinates */
		else {
			fprintf(stderr,
			        "Failed to add route point at position lon:%f lat:%f due to display coordinate projection (%f %f %f) far "
			        "outside view...\n",
			        routelon[i], routelat[i], xdisplay, ydisplay, zdisplay);
			XBell(view->dpy, 100);
		}
	}

	/* set color size and name for new route - only if a route was actually
	    created above by mbview_route_add(); with npoint <= 0 that loop never
	    ran, so routes[*iroute] was never allocated and must not be touched
	    (InteractiveGMT port: nor when every point was refused above -- the same hole, the route
	    count then never reached *iroute) */
	if (npoint > 0 && *iroute < shared.shareddata.nroute) {
		shared.shareddata.routes[*iroute].color = routecolor;
		shared.shareddata.routes[*iroute].size = routesize;
		shared.shareddata.routes[*iroute].editmode = routeeditmode;
		strcpy(shared.shareddata.routes[*iroute].name, routename);

		/* set distance values */
		mbview_route_setdistance(instance, *iroute);
	}

	/* make routes viewable */
	if (data->route_view_mode != MBV_VIEW_ON) {
		data->route_view_mode = MBV_VIEW_ON;
		set_mbview_route_view_mode(instance, MBV_VIEW_ON);
	}

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       iroute:                    %d\n", *iroute);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_deleteroute(int verbose, size_t instance, int iroute, int *error) {
	/* local variables */
	int status = MB_SUCCESS;
	int jpoint;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       iroute:                    %d\n", iroute);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* delete the points in the route backwards */
	for (jpoint = shared.shareddata.routes[iroute].npoints - 1; jpoint >= 0; jpoint--) {
		/* delete the route point */
		mbview_route_delete(instance, iroute, jpoint);
	}

	/* set pick annotation */
	mbview_pick_text(instance);

	/* update route list */
	mbview_updateroutelist();

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_deleteallroutes(int verbose, size_t instance, int *error) {
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	struct mbview_route_struct *route;
	struct mbview_linesegmentw_struct *segment;
	int i, j;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	for (i = 0; i < shared.shareddata.nroute_alloc; i++) {
		route = &shared.shareddata.routes[i];
		if (route->npoints_alloc > 0) {
			for (j = 0; j < route->npoints_alloc; j++) {
				segment = &route->segments[j];
				if (segment->nls_alloc > 0 && segment->lspoints != NULL) {
					status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(segment->lspoints), error);
					segment->nls_alloc = 0;
					segment->nls = 0;
				}
			}
			status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&route->waypoint, error);
			status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&route->distlateral, error);
			status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&route->disttopo, error);
			status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&route->points, error);
			status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&route->segments, error);
		}
		route->npoints = 0;
		route->npoints_alloc = 0;
		route->nroutepoint = 0;
		route->waypoint = NULL;
		route->distlateral = NULL;
		route->disttopo = NULL;
		route->points = NULL;
		route->segments = NULL;
	}
	if (shared.shareddata.nroute_alloc > 0 && shared.shareddata.routes != NULL) {
		status = mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&shared.shareddata.routes, error);
	}
	shared.shareddata.nroute = 0;
	shared.shareddata.nroute_alloc = 0;
	shared.shareddata.route_selected = MBV_SELECT_NONE;
	shared.shareddata.route_point_selected = MBV_SELECT_NONE;
	shared.shareddata.routes = NULL;

	/* set pick annotation */
	mbview_pick_text(instance);

	/* update route list */
	mbview_updateroutelist();

	/* print route debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  Route data altered in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  Route values:\n");
		fprintf(stderr, "dbg2       route_view_mode:      %d\n", data->route_view_mode);
		fprintf(stderr, "dbg2       route_mode:           %d\n", shared.shareddata.route_mode);
		fprintf(stderr, "dbg2       nroute:               %d\n", shared.shareddata.nroute);
		fprintf(stderr, "dbg2       nroute_alloc:         %d\n", shared.shareddata.nroute_alloc);
		fprintf(stderr, "dbg2       route_selected:       %d\n", shared.shareddata.route_selected);
		fprintf(stderr, "dbg2       route_point_selected: %d\n", shared.shareddata.route_point_selected);
		for (i = 0; i < shared.shareddata.nroute; i++) {
			fprintf(stderr, "dbg2       route %d active:        %d\n", i, shared.shareddata.routes[i].active);
			fprintf(stderr, "dbg2       route %d color:         %d\n", i, shared.shareddata.routes[i].color);
			fprintf(stderr, "dbg2       route %d size:          %d\n", i, shared.shareddata.routes[i].size);
			fprintf(stderr, "dbg2       route %d name:          %s\n", i, shared.shareddata.routes[i].name);
			fprintf(stderr, "dbg2       route %d npoints:       %d\n", i, shared.shareddata.routes[i].npoints);
			fprintf(stderr, "dbg2       route %d npoints_alloc: %d\n", i, shared.shareddata.routes[i].npoints_alloc);
			for (j = 0; j < shared.shareddata.routes[i].npoints; j++) {
				fprintf(stderr, "dbg2       route %d %d xgrid:    %f\n", i, j,
				        shared.shareddata.routes[i].points[j].xgrid[instance]);
				fprintf(stderr, "dbg2       route %d %d ygrid:    %f\n", i, j,
				        shared.shareddata.routes[i].points[j].ygrid[instance]);
				fprintf(stderr, "dbg2       route %d %d xlon:     %f\n", i, j, shared.shareddata.routes[i].points[j].xlon);
				fprintf(stderr, "dbg2       route %d %d ylat:     %f\n", i, j, shared.shareddata.routes[i].points[j].ylat);
				fprintf(stderr, "dbg2       route %d %d zdata:    %f\n", i, j, shared.shareddata.routes[i].points[j].zdata);
				fprintf(stderr, "dbg2       route %d %d xdisplay: %f\n", i, j,
				        shared.shareddata.routes[i].points[j].xdisplay[instance]);
				fprintf(stderr, "dbg2       route %d %d ydisplay: %f\n", i, j,
				        shared.shareddata.routes[i].points[j].ydisplay[instance]);
				fprintf(stderr, "dbg2       route %d %d zdisplay: %f\n", i, j,
				        shared.shareddata.routes[i].points[j].zdisplay[instance]);
			}
			for (j = 0; j < shared.shareddata.routes[i].npoints - 1; j++) {
				fprintf(stderr, "dbg2       route %d %d nls:          %d\n", i, j, shared.shareddata.routes[i].segments[j].nls);
				fprintf(stderr, "dbg2       route %d %d nls_alloc:    %d\n", i, j,
				        shared.shareddata.routes[i].segments[j].nls_alloc);
				fprintf(stderr, "dbg2       route %d %d endpoints[0]: %p\n", i, j,
				        &shared.shareddata.routes[i].segments[j].endpoints[0]);
				fprintf(stderr, "dbg2       route %d %d endpoints[1]: %p\n", i, j,
				        &shared.shareddata.routes[i].segments[j].endpoints[1]);
			}
		}
	}

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getroute(int verbose, size_t instance, int route, int *npointtotal, double *routelon, double *routelat, int *waypoint,
                    double *routetopo, double *routebearing, double *distlateral, double *distovertopo, double *slope,
                    int *routecolor, int *routesize, int *routeeditmode, mb_path routename, int *error) {
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	double dx, dy, range, bearing;
	double xx1, yy1, xx2, yy2;
	int i, j;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       route:                     %d\n", route);
		fprintf(stderr, "dbg2       npointtotal:               %p\n", npointtotal);
		fprintf(stderr, "dbg2       routelon:                  %p\n", routelon);
		fprintf(stderr, "dbg2       routelat:                  %p\n", routelat);
		fprintf(stderr, "dbg2       waypoint:                  %p\n", waypoint);
		fprintf(stderr, "dbg2       routetopo:                 %p\n", routetopo);
		fprintf(stderr, "dbg2       routebearing:              %p\n", routebearing);
		fprintf(stderr, "dbg2       distlateral:               %p\n", distlateral);
		fprintf(stderr, "dbg2       distovertopo:              %p\n", distovertopo);
		fprintf(stderr, "dbg2       slope:                     %p\n", slope);
		fprintf(stderr, "dbg2       routecolor:                %p\n", routecolor);
		fprintf(stderr, "dbg2       routesize:                 %p\n", routesize);
		fprintf(stderr, "dbg2       routeeditmode:             %p\n", routeeditmode);
		fprintf(stderr, "dbg2       routename:                 %p\n", routename);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	/* zero the points returned */
	*npointtotal = 0;

	/* check that the array pointers are not NULL */
	if (routelon == NULL || routelat == NULL || waypoint == NULL || routetopo == NULL || distlateral == NULL ||
	    distovertopo == NULL || slope == NULL) {
		status = MB_FAILURE;
		*error = MB_ERROR_DATA_NOT_INSERTED;
	}

	/* otherwise go get the route data */
	else {
		/* loop over the route segments */
		for (i = 0; i < shared.shareddata.routes[route].npoints - 1; i++) {
			/* get bearing of segment */
			if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
				xx1 = shared.shareddata.routes[route].points[i].xdisplay[instance];
				yy1 = shared.shareddata.routes[route].points[i].ydisplay[instance];
				xx2 = shared.shareddata.routes[route].points[i + 1].xdisplay[instance];
				yy2 = shared.shareddata.routes[route].points[i + 1].ydisplay[instance];
				dx = (double)(shared.shareddata.routes[route].points[i + 1].xdisplay[instance] -
				              shared.shareddata.routes[route].points[i].xdisplay[instance]);
				dy = (double)(shared.shareddata.routes[route].points[i + 1].ydisplay[instance] -
				              shared.shareddata.routes[route].points[i].ydisplay[instance]);
				dx = xx2 - xx1;
				dy = yy2 - yy1;
				range = sqrt(dx * dx + dy * dy) / view->scale;
				bearing = RTD * atan2(dx, dy);
			}
			else {
				mbview_greatcircle_distbearing(instance, shared.shareddata.routes[route].points[i].xlon,
				                               shared.shareddata.routes[route].points[i].ylat,
				                               shared.shareddata.routes[route].points[i + 1].xlon,
				                               shared.shareddata.routes[route].points[i + 1].ylat, &bearing, &range);
			}
			if (bearing < 0.0)
				bearing += 360.0;

			/* add first point */
			routelon[*npointtotal] = shared.shareddata.routes[route].points[i].xlon;
			if (routelon[*npointtotal] < -180.0)
				routelon[*npointtotal] += 360.0;
			else if (routelon[*npointtotal] > 180.0)
				routelon[*npointtotal] -= 360.0;
			routelat[*npointtotal] = shared.shareddata.routes[route].points[i].ylat;
			waypoint[*npointtotal] = shared.shareddata.routes[route].waypoint[i];
			routetopo[*npointtotal] = shared.shareddata.routes[route].points[i].zdata;
			routebearing[*npointtotal] = bearing;
			if (*npointtotal == 0) {
				distlateral[*npointtotal] = 0.0;
				distovertopo[*npointtotal] = 0.0;
				slope[*npointtotal] = 0.0;
			}
			else {
				mbview_projectdistance(instance, routelon[*npointtotal - 1], routelat[*npointtotal - 1],
				                       routetopo[*npointtotal - 1], routelon[*npointtotal], routelat[*npointtotal],
				                       routetopo[*npointtotal], &distlateral[*npointtotal], &distovertopo[*npointtotal],
				                       &slope[*npointtotal]);
				distlateral[*npointtotal] += distlateral[*npointtotal - 1];
				distovertopo[*npointtotal] += distovertopo[*npointtotal - 1];
			}
			(*npointtotal)++;

			/* loop over interior of segment */
			for (j = 1; j < shared.shareddata.routes[route].segments[i].nls - 1; j++) {
				routelon[*npointtotal] = shared.shareddata.routes[route].segments[i].lspoints[j].xlon;
				if (routelon[*npointtotal] < -180.0)
					routelon[*npointtotal] += 360.0;
				else if (routelon[*npointtotal] > 180.0)
					routelon[*npointtotal] -= 360.0;
				routelat[*npointtotal] = shared.shareddata.routes[route].segments[i].lspoints[j].ylat;
				waypoint[*npointtotal] = MBV_ROUTE_WAYPOINT_NONE;
				routetopo[*npointtotal] = shared.shareddata.routes[route].segments[i].lspoints[j].zdata;
				routebearing[*npointtotal] = bearing;
				mbview_projectdistance(instance, routelon[*npointtotal - 1], routelat[*npointtotal - 1],
				                       routetopo[*npointtotal - 1], routelon[*npointtotal], routelat[*npointtotal],
				                       routetopo[*npointtotal], &distlateral[*npointtotal], &distovertopo[*npointtotal],
				                       &slope[*npointtotal]);
				distlateral[*npointtotal] += distlateral[*npointtotal - 1];
				distovertopo[*npointtotal] += distovertopo[*npointtotal - 1];
				(*npointtotal)++;
			}
		}

		/* add last point */
		j = shared.shareddata.routes[route].npoints - 1;
		routelon[*npointtotal] = shared.shareddata.routes[route].points[j].xlon;
		if (routelon[*npointtotal] < -180.0)
			routelon[*npointtotal] += 360.0;
		else if (routelon[*npointtotal] > 180.0)
			routelon[*npointtotal] -= 360.0;
		routelat[*npointtotal] = shared.shareddata.routes[route].points[j].ylat;
		waypoint[*npointtotal] = shared.shareddata.routes[route].waypoint[j];
		;
		routetopo[*npointtotal] = shared.shareddata.routes[route].points[j].zdata;
		routebearing[*npointtotal] = bearing;
		mbview_projectdistance(instance, routelon[*npointtotal - 1], routelat[*npointtotal - 1], routetopo[*npointtotal - 1],
		                       routelon[*npointtotal], routelat[*npointtotal], routetopo[*npointtotal],
		                       &distlateral[*npointtotal], &distovertopo[*npointtotal], &slope[*npointtotal]);
		distlateral[*npointtotal] += distlateral[*npointtotal - 1];
		distovertopo[*npointtotal] += distovertopo[*npointtotal - 1];
		(*npointtotal)++;

		/* get color size and name */
		*routecolor = shared.shareddata.routes[route].color;
		*routesize = shared.shareddata.routes[route].size;
		*routeeditmode = shared.shareddata.routes[route].editmode;
		strcpy(routename, shared.shareddata.routes[route].name);

		/* recalculate slope */
		for (j = 0; j < *npointtotal; j++) {
			if (j == 0 && *npointtotal == 1) {
				slope[j] = 0.0;
			}
			else if (j == 0) {
				if (distlateral[j + 1] > 0.0)
					slope[j] = (routetopo[j + 1] - routetopo[j]) / distlateral[j + 1];
				else
					slope[j] = 0.0;
			}
			else if (j == *npointtotal - 1) {
				if ((distlateral[j] - distlateral[j - 1]) > 0.0)
					slope[j] = (routetopo[j] - routetopo[j - 1]) / (distlateral[j] - distlateral[j - 1]);
				else
					slope[j] = 0.0;
			}
			else {
				if ((distlateral[j + 1] - distlateral[j - 1]) > 0.0)
					slope[j] = (routetopo[j + 1] - routetopo[j - 1]) / (distlateral[j + 1] - distlateral[j - 1]);
				else
					slope[j] = 0.0;
			}
		}
	}

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       npointtotal:               %d\n", *npointtotal);
		fprintf(stderr, "dbg2       routecolor:                %d\n", *routecolor);
		fprintf(stderr, "dbg2       routesize:                 %d\n", *routesize);
		fprintf(stderr, "dbg2       routeeditmode:             %d\n", *routeeditmode);
		fprintf(stderr, "dbg2       routename:                 %s\n", routename);
		for (i = 0; i < *npointtotal; i++) {
			fprintf(
			    stderr,
			    "dbg2       route:%d lon:%f lat:%f waypoint:%d topo:%f bearing:%f dist:%f distbot:%f color:%d size:%d name:%s\n",
			    i, routelon[i], routelat[i], waypoint[i], routetopo[i], routebearing[i], distlateral[i], distovertopo[i],
			    *routecolor, *routesize, routename);
		}
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_extract_route_profile(size_t instance) {

	/* local variables */
	int status = MB_SUCCESS;
	int error = MB_ERROR_NO_ERROR;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	int iroute, jstart;
	int nprpoints;
	double dx, dy;
	int i, j;

	/* print starting debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	/* if a route is selected, extract the profile */
	if (shared.shareddata.route_selected != MBV_SELECT_NONE &&
	    shared.shareddata.routes[shared.shareddata.route_selected].npoints > 1) {
		data->profile.source = MBV_PROFILE_ROUTE;
		strcpy(data->profile.source_name, "Route");
		data->profile.length = 0.0;
		iroute = shared.shareddata.route_selected;

		/* make sure enough memory is allocated for the profile */
		nprpoints = 0;
		for (i = 0; i < shared.shareddata.routes[iroute].npoints - 1; i++) {
			nprpoints += shared.shareddata.routes[iroute].segments[i].nls;
		}
		if (data->profile.npoints_alloc < nprpoints) {
			status = mbview_allocprofilepoints(mbv_verbose, nprpoints, &(data->profile.points), &error);
			if (status == MB_SUCCESS) {
				data->profile.npoints_alloc = nprpoints;
			}
			else {
				data->profile.npoints_alloc = 0;
			}
		}

		/* extract the profile */
		if (nprpoints > 2 && data->profile.npoints_alloc >= nprpoints) {
			data->profile.npoints = 0;
			for (i = 0; i < shared.shareddata.routes[iroute].npoints - 1; i++) {
				if (i == 0)
					jstart = 0;
				else
					jstart = 1;
				for (j = jstart; j < shared.shareddata.routes[iroute].segments[i].nls; j++) {
					if (j == 0 || j == shared.shareddata.routes[iroute].segments[i].nls - 1)
						data->profile.points[data->profile.npoints].boundary = true;
					else
						data->profile.points[data->profile.npoints].boundary = false;
					data->profile.points[data->profile.npoints].xgrid =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].xgrid[instance];
					data->profile.points[data->profile.npoints].ygrid =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].ygrid[instance];
					data->profile.points[data->profile.npoints].xlon =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].xlon;
					data->profile.points[data->profile.npoints].ylat =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].ylat;
					data->profile.points[data->profile.npoints].zdata =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].zdata;
					data->profile.points[data->profile.npoints].xdisplay =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].xdisplay[instance];
					data->profile.points[data->profile.npoints].ydisplay =
					    shared.shareddata.routes[iroute].segments[i].lspoints[j].ydisplay[instance];
					if (data->profile.npoints == 0) {
						data->profile.zmin = data->profile.points[data->profile.npoints].zdata;
						data->profile.zmax = data->profile.points[data->profile.npoints].zdata;
						data->profile.points[data->profile.npoints].distance = 0.0;
						data->profile.points[data->profile.npoints].distovertopo = 0.0;
						data->profile.points[data->profile.npoints].bearing = 0.0;
					}
					else {
						data->profile.zmin = MIN(data->profile.zmin, data->profile.points[data->profile.npoints].zdata);
						data->profile.zmax = MAX(data->profile.zmax, data->profile.points[data->profile.npoints].zdata);
						if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
							dx = data->profile.points[data->profile.npoints].xdisplay -
							     data->profile.points[data->profile.npoints - 1].xdisplay;
							dy = data->profile.points[data->profile.npoints].ydisplay -
							     data->profile.points[data->profile.npoints - 1].ydisplay;
							data->profile.points[data->profile.npoints].distance =
							    sqrt(dx * dx + dy * dy) / view->scale + data->profile.points[data->profile.npoints - 1].distance;
							data->profile.points[data->profile.npoints].bearing = RTD * atan2(dx, dy);
						}
						else {
							mbview_greatcircle_distbearing(instance, data->profile.points[data->profile.npoints - 1].xlon,
							                               data->profile.points[data->profile.npoints - 1].ylat,
							                               data->profile.points[data->profile.npoints].xlon,
							                               data->profile.points[data->profile.npoints].ylat,
							                               &(data->profile.points[data->profile.npoints].bearing),
							                               &(data->profile.points[data->profile.npoints].distance));
							mbview_greatcircle_dist(instance, data->profile.points[0].xlon, data->profile.points[0].ylat,
							                        data->profile.points[data->profile.npoints].xlon,
							                        data->profile.points[data->profile.npoints].ylat,
							                        &(data->profile.points[data->profile.npoints].distance));
						}
						dy = (data->profile.points[data->profile.npoints].zdata -
						      data->profile.points[data->profile.npoints - 1].zdata);
						dx = (data->profile.points[data->profile.npoints].distance -
						      data->profile.points[data->profile.npoints - 1].distance);
						data->profile.points[data->profile.npoints].distovertopo =
						    data->profile.points[data->profile.npoints - 1].distovertopo + sqrt(dy * dy + dx * dx);
						if (dx > 0.0)
							data->profile.points[data->profile.npoints].slope = fabs(dy / dx);
						else
							data->profile.points[data->profile.npoints].slope = 0.0;
					}
					if (data->profile.points[data->profile.npoints].bearing < 0.0)
						data->profile.points[data->profile.npoints].bearing += 360.0;
					if (data->profile.npoints == 1)
						data->profile.points[0].bearing = data->profile.points[data->profile.npoints].bearing;
					if (data->profile.npoints > 1) {
						dy = (data->profile.points[data->profile.npoints].zdata -
						      data->profile.points[data->profile.npoints - 2].zdata);
						dx = (data->profile.points[data->profile.npoints].distance -
						      data->profile.points[data->profile.npoints - 2].distance);
						if (dx > 0.0)
							data->profile.points[data->profile.npoints - 1].slope = fabs(dy / dx);
						else
							data->profile.points[data->profile.npoints - 1].slope = 0.0;
					}
					data->profile.points[data->profile.npoints].navzdata = 0.0;
					data->profile.points[data->profile.npoints].navtime_d = 0.0;
					data->profile.npoints++;
				}
			}
			data->profile.length = data->profile.points[data->profile.npoints - 1].distance;
		}
	}

	/* print output debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_route_add(int verbose, size_t instance, int inew, int jnew, int waypoint, double xgrid, double ygrid, double xlon,
                     double ylat, double zdata, double xdisplay, double ydisplay, double zdisplay) {

	/* local variables */
	int status = MB_SUCCESS;
	int error = MB_ERROR_NO_ERROR;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	struct mbview_linesegmentw_struct *seg;
	int npoints, npoints_alloc, npoints_diff;
	int i, j, k;

	/* print starting debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:          %d\n", verbose);
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       inew:             %d\n", inew);
		fprintf(stderr, "dbg2       jnew:             %d\n", jnew);
		fprintf(stderr, "dbg2       waypoint:         %d\n", waypoint);
		fprintf(stderr, "dbg2       xgrid:            %f\n", xgrid);
		fprintf(stderr, "dbg2       ygrid:            %f\n", ygrid);
		fprintf(stderr, "dbg2       xlon:             %f\n", xlon);
		fprintf(stderr, "dbg2       ylat:             %f\n", ylat);
		fprintf(stderr, "dbg2       zdata:            %f\n", zdata);
		fprintf(stderr, "dbg2       xdisplay:         %f\n", xdisplay);
		fprintf(stderr, "dbg2       ydisplay:         %f\n", ydisplay);
		fprintf(stderr, "dbg2       zdisplay:         %f\n", zdisplay);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	/* add route if required */
	if (inew == shared.shareddata.nroute) {
		/* allocate memory for a new route if required */
		if (shared.shareddata.nroute_alloc < shared.shareddata.nroute + 1) {
			shared.shareddata.nroute_alloc += MBV_ALLOC_NUM;
			status =
			    mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.nroute_alloc * sizeof(struct mbview_route_struct),
			                (void **)&(shared.shareddata.routes), &error);
			if (status == MB_FAILURE) {
				shared.shareddata.nroute_alloc = 0;
			}
			else {
				memset((void *)&shared.shareddata.routes[shared.shareddata.nroute], 0,
				       MBV_ALLOC_NUM * sizeof(struct mbview_route_struct));
				for (i = shared.shareddata.nroute; i < shared.shareddata.nroute_alloc; i++) {
					shared.shareddata.routes[i].color = false;
					shared.shareddata.routes[i].color = MBV_COLOR_RED;
					shared.shareddata.routes[i].size = 1;
					shared.shareddata.routes[i].editmode = true;
					shared.shareddata.routes[i].name[0] = '\0';
					shared.shareddata.routes[i].npoints = 0;
					shared.shareddata.routes[i].npoints_alloc = MBV_ALLOC_NUM;
					shared.shareddata.routes[i].waypoint = NULL;
					shared.shareddata.routes[i].distlateral = NULL;
					shared.shareddata.routes[i].disttopo = NULL;
					shared.shareddata.routes[i].points = NULL;
					shared.shareddata.routes[i].segments = NULL;
					status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.routes[i].npoints_alloc * sizeof(int),
					                     (void **)&(shared.shareddata.routes[i].waypoint), &error);
					status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.routes[i].npoints_alloc * sizeof(double),
					                (void **)&(shared.shareddata.routes[i].distlateral), &error);
					status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.routes[i].npoints_alloc * sizeof(double),
					                (void **)&(shared.shareddata.routes[i].disttopo), &error);
					status = mb_reallocd(mbv_verbose, __FILE__, __LINE__,
					                     shared.shareddata.routes[i].npoints_alloc * sizeof(struct mbview_pointw_struct),
					                     (void **)&(shared.shareddata.routes[i].points), &error);
					status = mb_reallocd(mbv_verbose, __FILE__, __LINE__,
					                     shared.shareddata.routes[i].npoints_alloc * sizeof(struct mbview_linesegmentw_struct),
					                     (void **)&(shared.shareddata.routes[i].segments), &error);
					memset((void *)shared.shareddata.routes[i].waypoint, 0,
					       shared.shareddata.routes[i].npoints_alloc * sizeof(int));
					memset((void *)shared.shareddata.routes[i].distlateral, 0,
					       shared.shareddata.routes[i].npoints_alloc * sizeof(double));
					memset((void *)shared.shareddata.routes[i].disttopo, 0,
					       shared.shareddata.routes[i].npoints_alloc * sizeof(double));
					memset((void *)shared.shareddata.routes[i].points, 0,
					       shared.shareddata.routes[i].npoints_alloc * sizeof(struct mbview_pointw_struct));
					memset((void *)shared.shareddata.routes[i].segments, 0,
					       shared.shareddata.routes[i].npoints_alloc * sizeof(struct mbview_linesegmentw_struct));
				}
			}
		}

		/* set nroute */
		shared.shareddata.nroute++;

		/* add the new route */
		shared.shareddata.routes[inew].active = true;
		shared.shareddata.routes[inew].color = MBV_COLOR_BLACK;
		shared.shareddata.routes[inew].size = 1;
		shared.shareddata.routes[inew].editmode = true;
		sprintf(shared.shareddata.routes[inew].name, "Route:%d", shared.shareddata.nroute);
	}

	/* allocate memory for point if required */
	if (status == MB_SUCCESS && shared.shareddata.routes[inew].npoints_alloc < shared.shareddata.routes[inew].npoints + 1) {
		npoints = shared.shareddata.routes[inew].npoints;
		if (shared.shareddata.routes[inew].npoints_alloc == 0)
			npoints_alloc = 2;
		else if (shared.shareddata.routes[inew].npoints_alloc < MBV_ALLOC_NUM)
			npoints_alloc = MBV_ALLOC_NUM;
		else
			npoints_alloc = shared.shareddata.routes[inew].npoints_alloc + MBV_ALLOC_NUM;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, npoints_alloc * sizeof(int),
		                     (void **)&(shared.shareddata.routes[inew].waypoint), &error);
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, npoints_alloc * sizeof(double),
		                     (void **)&(shared.shareddata.routes[inew].distlateral), &error);
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, npoints_alloc * sizeof(double),
		                     (void **)&(shared.shareddata.routes[inew].disttopo), &error);
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, npoints_alloc * sizeof(struct mbview_pointw_struct),
		                     (void **)&(shared.shareddata.routes[inew].points), &error);
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, npoints_alloc * sizeof(struct mbview_linesegmentw_struct),
		                     (void **)&(shared.shareddata.routes[inew].segments), &error);
		npoints_diff = npoints_alloc - npoints;
		memset((void *)&shared.shareddata.routes[inew].waypoint[npoints], 0, npoints_diff * sizeof(int));
		memset((void *)&shared.shareddata.routes[inew].distlateral[npoints], 0, npoints_diff * sizeof(double));
		memset((void *)&shared.shareddata.routes[inew].disttopo[npoints], 0, npoints_diff * sizeof(double));
		memset((void *)&shared.shareddata.routes[inew].points[npoints], 0, npoints_diff * sizeof(struct mbview_pointw_struct));
		memset((void *)&shared.shareddata.routes[inew].segments[npoints], 0,
		       npoints_diff * sizeof(struct mbview_linesegmentw_struct));
		if (status == MB_SUCCESS) {
			shared.shareddata.routes[inew].npoints_alloc = npoints_alloc;
		}
		else {
			shared.shareddata.routes[inew].npoints = 0;
			shared.shareddata.routes[inew].npoints_alloc = 0;
		}
	}

	/* add the new route point */
	// fprintf(stderr,"mbview_route_add: inew:%d jnew:%d waypoint:%d
	// editmode:%d\n",inew,jnew,waypoint,shared.shareddata.routes[inew].editmode);
	if (status == MB_SUCCESS) {
		/* move points after jnew if necessary */
		for (j = shared.shareddata.routes[inew].npoints; j > jnew; j--) {
			shared.shareddata.routes[inew].waypoint[j] = shared.shareddata.routes[inew].waypoint[j - 1];
			shared.shareddata.routes[inew].points[j] = shared.shareddata.routes[inew].points[j - 1];
		}

		/* move segments after jnew if necessary */
		for (j = shared.shareddata.routes[inew].npoints - 1; j > jnew; j--) {
			shared.shareddata.routes[inew].segments[j] = shared.shareddata.routes[inew].segments[j - 1];
			shared.shareddata.routes[inew].segments[j].endpoints[0] = shared.shareddata.routes[inew].points[j];
			shared.shareddata.routes[inew].segments[j].endpoints[1] = shared.shareddata.routes[inew].points[j + 1];
		}

		/* add the new point */
		shared.shareddata.routes[inew].waypoint[jnew] = waypoint;
		shared.shareddata.routes[inew].points[jnew].xgrid[instance] = xgrid;
		shared.shareddata.routes[inew].points[jnew].ygrid[instance] = ygrid;
		shared.shareddata.routes[inew].points[jnew].xlon = xlon;
		shared.shareddata.routes[inew].points[jnew].ylat = ylat;
		shared.shareddata.routes[inew].points[jnew].zdata = zdata;
		shared.shareddata.routes[inew].points[jnew].xdisplay[instance] = xdisplay;
		shared.shareddata.routes[inew].points[jnew].ydisplay[instance] = ydisplay;
		shared.shareddata.routes[inew].points[jnew].zdisplay[instance] = zdisplay;
		mbview_updatepointw(instance, &(shared.shareddata.routes[inew].points[jnew]));

		/* initialize the new segment */
		shared.shareddata.routes[inew].segments[jnew].nls = 0;
		shared.shareddata.routes[inew].segments[jnew].nls_alloc = 0;
		shared.shareddata.routes[inew].segments[jnew].lspoints = NULL;
		shared.shareddata.routes[inew].segments[jnew].endpoints[0] = shared.shareddata.routes[inew].points[jnew];
		shared.shareddata.routes[inew].segments[jnew].endpoints[1] = shared.shareddata.routes[inew].points[jnew + 1];
		if (jnew > 0) {
			shared.shareddata.routes[inew].segments[jnew - 1].endpoints[0] = shared.shareddata.routes[inew].points[jnew - 1];
			shared.shareddata.routes[inew].segments[jnew - 1].endpoints[1] = shared.shareddata.routes[inew].points[jnew];
		}

		/* set npoints */
		shared.shareddata.routes[inew].npoints++;

		/* reset affected segment endpoints */
		if (shared.shareddata.routes[inew].npoints > 0) {
			for (j = MAX(0, jnew - 1); j < MIN(shared.shareddata.routes[inew].npoints - 1, jnew + 1); j++) {
				shared.shareddata.routes[inew].segments[j].endpoints[0] = shared.shareddata.routes[inew].points[j];
				shared.shareddata.routes[inew].segments[j].endpoints[1] = shared.shareddata.routes[inew].points[j + 1];

				/* drape the segment */
				mbview_drapesegmentw(instance, &(shared.shareddata.routes[inew].segments[j]));

				/* update the segment for all active instances */
				mbview_updatesegmentw(instance, &(shared.shareddata.routes[inew].segments[j]));
			}
		}

		/* set or reset distance values */
		mbview_route_setdistance(instance, inew);

		/* make routes viewable */
		if (data->route_view_mode != MBV_VIEW_ON) {
			data->route_view_mode = MBV_VIEW_ON;
			set_mbview_route_view_mode(instance, MBV_VIEW_ON);
		}
	}

	/* else beep */
	else {
		XBell(view->dpy, 100);
	}

	/* print route debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  Route data altered in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  Route values:\n");
		fprintf(stderr, "dbg2       route_view_mode:      %d\n", data->route_view_mode);
		fprintf(stderr, "dbg2       route_mode:           %d\n", shared.shareddata.route_mode);
		fprintf(stderr, "dbg2       nroute:               %d\n", shared.shareddata.nroute);
		fprintf(stderr, "dbg2       nroute_alloc:         %d\n", shared.shareddata.nroute_alloc);
		fprintf(stderr, "dbg2       route_selected:       %d\n", shared.shareddata.route_selected);
		fprintf(stderr, "dbg2       route_point_selected: %d\n", shared.shareddata.route_point_selected);
		for (i = 0; i < shared.shareddata.nroute; i++) {
			fprintf(stderr, "dbg2       route %d active:        %d\n", i, shared.shareddata.routes[i].active);
			fprintf(stderr, "dbg2       route %d color:         %d\n", i, shared.shareddata.routes[i].color);
			fprintf(stderr, "dbg2       route %d size:          %d\n", i, shared.shareddata.routes[i].size);
			fprintf(stderr, "dbg2       route %d name:          %s\n", i, shared.shareddata.routes[i].name);
			fprintf(stderr, "dbg2       route %d npoints:       %d\n", i, shared.shareddata.routes[i].npoints);
			fprintf(stderr, "dbg2       route %d npoints_alloc: %d\n", i, shared.shareddata.routes[i].npoints_alloc);
			fprintf(stderr, "dbg2       route points: iroute jpoint xgrid[instance] ygrid[instance] xlon ylat zdata "
			                "xdisplay[instance] ydisplay[instance] zdisplay[instance]\n");
			for (j = 0; j < shared.shareddata.routes[i].npoints; j++) {
				fprintf(stderr, "dbg2       %d %d %f %f %f %f %f %f %f %f\n", i, j,
				        shared.shareddata.routes[i].points[j].xgrid[instance],
				        shared.shareddata.routes[i].points[j].ygrid[instance], shared.shareddata.routes[i].points[j].xlon,
				        shared.shareddata.routes[i].points[j].ylat, shared.shareddata.routes[i].points[j].zdata,
				        shared.shareddata.routes[i].points[j].xdisplay[instance],
				        shared.shareddata.routes[i].points[j].ydisplay[instance],
				        shared.shareddata.routes[i].points[j].zdisplay[instance]);
			}
			for (j = 0; j < shared.shareddata.routes[i].npoints - 1; j++) {
				fprintf(stderr, "dbg2       route %d %d nls:          %d\n", i, j, shared.shareddata.routes[i].segments[j].nls);
				fprintf(stderr, "dbg2       route %d %d nls_alloc:    %d\n", i, j,
				        shared.shareddata.routes[i].segments[j].nls_alloc);
				fprintf(stderr, "dbg2       route %d %d endpoints[0]: %f %f %f %f %f %f %f %f\n", i, j,
				        shared.shareddata.routes[i].segments[j].endpoints[0].xgrid[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[0].ygrid[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[0].xlon,
				        shared.shareddata.routes[i].segments[j].endpoints[0].ylat,
				        shared.shareddata.routes[i].segments[j].endpoints[0].zdata,
				        shared.shareddata.routes[i].segments[j].endpoints[0].xdisplay[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[0].ydisplay[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[0].zdisplay[instance]);
				fprintf(stderr, "dbg2       route %d %d endpoints[1]: %f %f %f %f %f %f %f %f\n", i, j,
				        shared.shareddata.routes[i].segments[j].endpoints[1].xgrid[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[1].ygrid[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[1].xlon,
				        shared.shareddata.routes[i].segments[j].endpoints[1].ylat,
				        shared.shareddata.routes[i].segments[j].endpoints[1].zdata,
				        shared.shareddata.routes[i].segments[j].endpoints[1].xdisplay[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[1].ydisplay[instance],
				        shared.shareddata.routes[i].segments[j].endpoints[1].zdisplay[instance]);
				fprintf(stderr, "dbg2       segment points: kpoint xgrid[instance] ygrid[instance] xlon ylat zdata "
				                "xdisplay[instance] ydisplay[instance] zdisplay[instance]\n");
				seg = (struct mbview_linesegmentw_struct *)&(shared.shareddata.routes[inew].segments[j]);
				for (k = 0; k < seg->nls; k++) {
					fprintf(stderr, "dbg2         %d %f %f %f  %f %f  %f %f %f\n", k, seg->lspoints[k].xgrid[instance],
					        seg->lspoints[k].ygrid[instance], seg->lspoints[k].zdata, seg->lspoints[k].xlon,
					        seg->lspoints[k].ylat, seg->lspoints[k].xdisplay[instance], seg->lspoints[k].ydisplay[instance],
					        seg->lspoints[k].zdisplay[instance]);
				}
			}
		}
	}

	/* print output debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_route_delete(size_t instance, int iroute, int ipoint) {

	/* local variables */
	int status = MB_SUCCESS;
	int error = MB_ERROR_NO_ERROR;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	int idelete;
	int i, j;

	/* print starting debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       iroute:          %d\n", iroute);
		fprintf(stderr, "dbg2       ipoint:          %d\n", ipoint);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	fprintf(stderr, "mbview_route_delete: iroute:%d ipoint:%d editmode:%d\n", iroute, ipoint,
	        shared.shareddata.routes[iroute].editmode);
	/* delete route point if its valid */
	if (iroute >= 0 && iroute < shared.shareddata.nroute && ipoint >= 0 && ipoint < shared.shareddata.routes[iroute].npoints &&
	    shared.shareddata.routes[iroute].editmode) {
		/* free segment immediately after deleted point if in the middle of the
		    route or before if it is at the end */
		if (shared.shareddata.routes[iroute].npoints > 1) {
			if (ipoint < shared.shareddata.routes[iroute].npoints - 1) {
				idelete = ipoint;
			}
			else {
				idelete = ipoint - 1;
			}
			if (shared.shareddata.routes[iroute].segments[idelete].nls_alloc > 0 &&
			    shared.shareddata.routes[iroute].segments[idelete].lspoints != NULL) {
				mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.routes[iroute].segments[idelete].lspoints),
				         &error);
				shared.shareddata.routes[iroute].segments[idelete].nls = 0;
				shared.shareddata.routes[iroute].segments[idelete].nls_alloc = 0;
			}
		}

		/* move route point data if necessary */
		for (j = ipoint; j < shared.shareddata.routes[iroute].npoints - 1; j++) {
			shared.shareddata.routes[iroute].waypoint[j] = shared.shareddata.routes[iroute].waypoint[j + 1];
			shared.shareddata.routes[iroute].points[j] = shared.shareddata.routes[iroute].points[j + 1];
		}

		/* move route segment data if necessary */
		for (j = ipoint; j < shared.shareddata.routes[iroute].npoints - 2; j++) {
			shared.shareddata.routes[iroute].segments[j] = shared.shareddata.routes[iroute].segments[j + 1];
		}
		j = shared.shareddata.routes[iroute].npoints - 2;
		if (j >= 0) {
			shared.shareddata.routes[iroute].segments[j].nls = 0;
			shared.shareddata.routes[iroute].segments[j].nls_alloc = 0;
			shared.shareddata.routes[iroute].segments[j].lspoints = NULL;
		}

		/* decrement npoints */
		shared.shareddata.routes[iroute].npoints--;

		/* if route still has points then reset affected segment endpoints */
		if (shared.shareddata.routes[iroute].npoints > 0) {
			for (j = MAX(0, ipoint - 1); j < shared.shareddata.routes[iroute].npoints - 1; j++) {
				shared.shareddata.routes[iroute].segments[j].endpoints[0] = shared.shareddata.routes[iroute].points[j];
				shared.shareddata.routes[iroute].segments[j].endpoints[1] = shared.shareddata.routes[iroute].points[j + 1];

				/* drape the segment */
				mbview_drapesegmentw(instance, &(shared.shareddata.routes[iroute].segments[j]));

				/* update the segment for all active instances */
				mbview_updatesegmentw(instance, &(shared.shareddata.routes[iroute].segments[j]));
			}
		}

		/* if route still has points then reset distance values */
		if (shared.shareddata.routes[iroute].npoints > 0) {
			mbview_route_setdistance(instance, iroute);
		}

		/* if last point deleted then move remaining routes if necessary */
		if (shared.shareddata.routes[iroute].npoints <= 0) {
			/* free memory owned by the now-empty route before its slot is overwritten */
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.routes[iroute].waypoint), &error);
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.routes[iroute].distlateral), &error);
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.routes[iroute].disttopo), &error);
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.routes[iroute].points), &error);
			mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.routes[iroute].segments), &error);
			shared.shareddata.routes[iroute].npoints_alloc = 0;

			/* move route data if necessary */
			for (i = iroute; i < shared.shareddata.nroute - 1; i++) {
				shared.shareddata.routes[i] = shared.shareddata.routes[i + 1];
			}

			/* decrement nroute */
			shared.shareddata.nroute--;

			/* reset last route so its stale/aliased pointers cannot be reused by mbview_route_add */
			shared.shareddata.routes[shared.shareddata.nroute].active = false;
			shared.shareddata.routes[shared.shareddata.nroute].color = MBV_COLOR_BLACK;
			shared.shareddata.routes[shared.shareddata.nroute].size = 1;
			shared.shareddata.routes[shared.shareddata.nroute].editmode = true;
			shared.shareddata.routes[shared.shareddata.nroute].name[0] = '\0';
			shared.shareddata.routes[shared.shareddata.nroute].npoints = 0;
			shared.shareddata.routes[shared.shareddata.nroute].npoints_alloc = 0;
			shared.shareddata.routes[shared.shareddata.nroute].nroutepoint = 0;
			shared.shareddata.routes[shared.shareddata.nroute].waypoint = NULL;
			shared.shareddata.routes[shared.shareddata.nroute].distlateral = NULL;
			shared.shareddata.routes[shared.shareddata.nroute].disttopo = NULL;
			shared.shareddata.routes[shared.shareddata.nroute].points = NULL;
			shared.shareddata.routes[shared.shareddata.nroute].segments = NULL;
		}

		/* no route selection now */
		if (shared.shareddata.route_selected != MBV_SELECT_NONE) {
			shared.shareddata.route_selected = MBV_SELECT_NONE;
			shared.shareddata.route_point_selected = MBV_SELECT_NONE;
			data->pickinfo_mode = data->pick_type;
		}
	}

	/* else beep */
	else {
		XBell(view->dpy, 100);
	}

	/* print route debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  Route data altered in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  Route values:\n");
		fprintf(stderr, "dbg2       route_view_mode:      %d\n", data->route_view_mode);
		fprintf(stderr, "dbg2       route_mode:           %d\n", shared.shareddata.route_mode);
		fprintf(stderr, "dbg2       nroute:               %d\n", shared.shareddata.nroute);
		fprintf(stderr, "dbg2       nroute_alloc:         %d\n", shared.shareddata.nroute_alloc);
		fprintf(stderr, "dbg2       route_selected:       %d\n", shared.shareddata.route_selected);
		fprintf(stderr, "dbg2       route_point_selected: %d\n", shared.shareddata.route_point_selected);
		for (i = 0; i < shared.shareddata.nroute; i++) {
			fprintf(stderr, "dbg2       route %d active:        %d\n", i, shared.shareddata.routes[i].active);
			fprintf(stderr, "dbg2       route %d color:         %d\n", i, shared.shareddata.routes[i].color);
			fprintf(stderr, "dbg2       route %d size:          %d\n", i, shared.shareddata.routes[i].size);
			fprintf(stderr, "dbg2       route %d name:          %s\n", i, shared.shareddata.routes[i].name);
			fprintf(stderr, "dbg2       route %d npoints:       %d\n", i, shared.shareddata.routes[i].npoints);
			fprintf(stderr, "dbg2       route %d npoints_alloc: %d\n", i, shared.shareddata.routes[i].npoints_alloc);
			for (j = 0; j < shared.shareddata.routes[i].npoints; j++) {
				fprintf(stderr, "dbg2       route %d %d xgrid:    %f\n", i, j,
				        shared.shareddata.routes[i].points[j].xgrid[instance]);
				fprintf(stderr, "dbg2       route %d %d ygrid:    %f\n", i, j,
				        shared.shareddata.routes[i].points[j].ygrid[instance]);
				fprintf(stderr, "dbg2       route %d %d xlon:     %f\n", i, j, shared.shareddata.routes[i].points[j].xlon);
				fprintf(stderr, "dbg2       route %d %d ylat:     %f\n", i, j, shared.shareddata.routes[i].points[j].ylat);
				fprintf(stderr, "dbg2       route %d %d zdata:    %f\n", i, j, shared.shareddata.routes[i].points[j].zdata);
				fprintf(stderr, "dbg2       route %d %d xdisplay: %f\n", i, j,
				        shared.shareddata.routes[i].points[j].xdisplay[instance]);
				fprintf(stderr, "dbg2       route %d %d ydisplay: %f\n", i, j,
				        shared.shareddata.routes[i].points[j].ydisplay[instance]);
				fprintf(stderr, "dbg2       route %d %d zdisplay: %f\n", i, j,
				        shared.shareddata.routes[i].points[j].zdisplay[instance]);
			}
			for (j = 0; j < shared.shareddata.routes[i].npoints - 1; j++) {
				fprintf(stderr, "dbg2       route %d %d nls:          %d\n", i, j, shared.shareddata.routes[i].segments[j].nls);
				fprintf(stderr, "dbg2       route %d %d nls_alloc:    %d\n", i, j,
				        shared.shareddata.routes[i].segments[j].nls_alloc);
				fprintf(stderr, "dbg2       route %d %d endpoints[0]: %p\n", i, j,
				        &shared.shareddata.routes[i].segments[j].endpoints[0]);
				fprintf(stderr, "dbg2       route %d %d endpoints[1]: %p\n", i, j,
				        &shared.shareddata.routes[i].segments[j].endpoints[1]);
			}
		}
	}

	/* print output debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_route_setdistance(size_t instance, int working_route) {
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_route_struct *route = NULL;
	bool valid_route = false;
	double distlateral, distovertopo;
	double routelon0, routelon1;
	double routelat0, routelat1;
	double routetopo0, routetopo1;
	double routeslope;
	int i, j;

	/* print starting debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", mbv_verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       working_route:             %d\n", working_route);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* check that the route is valid */
	if (working_route >= 0 && working_route < shared.shareddata.nroute
      && shared.shareddata.routes[working_route].npoints > 0 && shared.shareddata.routes[working_route].active) {
		/* get route pointer */
		route = &(shared.shareddata.routes[working_route]);
		valid_route = true;

		/* loop over the route segments */
		route->distancelateral = 0.0;
		route->distancetopo = 0.0;
		route->nroutepoint = 0;
		for (i = 0; i < route->npoints - 1; i++) {
			/* do first point */
			routelon1 = route->points[i].xlon;
			if (routelon1 < -180.0)
				routelon1 += 360.0;
			else if (routelon1 > 180.0)
				routelon1 -= 360.0;
			routelat1 = route->points[i].ylat;
			routetopo1 = route->points[i].zdata;
			if (route->nroutepoint == 0) {
				distlateral = 0.0;
				distovertopo = 0.0;
			}
			else {
				mbview_projectdistance(instance, routelon0, routelat0, routetopo0, routelon1, routelat1, routetopo1, &distlateral,
				                       &distovertopo, &routeslope);
			}
			route->distancelateral += distlateral;
			route->distancetopo += distovertopo;
			routelon0 = routelon1;
			routelat0 = routelat1;
			routetopo0 = routetopo1;
			route->nroutepoint++;

			/* set distances for route waypoint */
			route->distlateral[i] = route->distancelateral;
			route->disttopo[i] = route->distancetopo;

			/* loop over interior of segment */
			for (j = 1; j < route->segments[i].nls - 1; j++) {
				routelon1 = route->segments[i].lspoints[j].xlon;
				if (routelon1 < -180.0)
					routelon1 += 360.0;
				else if (routelon1 > 180.0)
					routelon1 -= 360.0;
				routelat1 = route->segments[i].lspoints[j].ylat;
				routetopo1 = route->segments[i].lspoints[j].zdata;
				mbview_projectdistance(instance, routelon0, routelat0, routetopo0, routelon1, routelat1, routetopo1, &distlateral,
				                       &distovertopo, &routeslope);
				route->distancelateral += distlateral;
				route->distancetopo += distovertopo;
				routelon0 = routelon1;
				routelat0 = routelat1;
				routetopo0 = routetopo1;
				route->nroutepoint++;
			}
		}

		/* do last point */
		j = route->npoints - 1;
		routelon1 = route->points[j].xlon;
		if (routelon1 < -180.0)
			routelon1 += 360.0;
		else if (routelon1 > 180.0)
			routelon1 -= 360.0;
		routelat1 = route->points[j].ylat;
		routetopo1 = route->points[j].zdata;
		if (j > 0) {
		  mbview_projectdistance(instance, routelon0, routelat0, routetopo0, routelon1, routelat1, routetopo1, &distlateral,
		                       &distovertopo, &routeslope);
		  route->distancelateral += distlateral;
		  route->distancetopo += distovertopo;
		}
		route->nroutepoint++;

		/* set distances for route waypoint */
		route->distlateral[j] = route->distancelateral;
		route->disttopo[j] = route->distancetopo;
	}

	/* print output debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		if (valid_route) {
			route = &(shared.shareddata.routes[working_route]);
			fprintf(stderr, "dbg2       routedistancelateral:      %f\n", route->distancelateral);
			fprintf(stderr, "dbg2       routedistancetopo:         %f\n", route->distancetopo);
		}
		else {
			route = &(shared.shareddata.routes[working_route]);
			fprintf(stderr, "dbg2       invalid working route:     %d\n", working_route);
		}
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getsitecount(int verbose, size_t instance, int *nsite, int *error)
{
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get number of sites */
	*nsite = shared.shareddata.nsite;

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nsite:                     %d\n", *nsite);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_allocsitearrays(int verbose, int nsite, double **sitelon, double **sitelat, double **sitetopo, int **sitecolor,
                           int **sitesize, mb_path **sitename, int *error)
{
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       nsite:                     %d\n", nsite);
		fprintf(stderr, "dbg2       sitelon:                   %p\n", *sitelon);
		fprintf(stderr, "dbg2       sitelat:                   %p\n", *sitelat);
		fprintf(stderr, "dbg2       sitetopo:                  %p\n", *sitetopo);
		fprintf(stderr, "dbg2       sitecolor:                 %p\n", *sitecolor);
		fprintf(stderr, "dbg2       sitesize:                  %p\n", *sitesize);
		fprintf(stderr, "dbg2       sitename:                  %p\n", *sitename);
	}

	/* allocate the arrays using mb_reallocd */
	int status = mb_reallocd(verbose, __FILE__, __LINE__, nsite * sizeof(double), (void **)sitelon, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, nsite * sizeof(double), (void **)sitelat, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, nsite * sizeof(double), (void **)sitetopo, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, nsite * sizeof(int), (void **)sitecolor, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, nsite * sizeof(int), (void **)sitesize, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, nsite * sizeof(mb_path), (void **)sitename, error);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       sitelon:                   %p\n", *sitelon);
		fprintf(stderr, "dbg2       sitelat:                   %p\n", *sitelat);
		fprintf(stderr, "dbg2       sitetopo:                  %p\n", *sitetopo);
		fprintf(stderr, "dbg2       sitecolor:                 %p\n", *sitecolor);
		fprintf(stderr, "dbg2       sitesize:                  %p\n", *sitesize);
		fprintf(stderr, "dbg2       sitename:                  %p\n", *sitename);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_freesitearrays(int verbose, double **sitelon, double **sitelat, double **sitetopo, int **sitecolor, int **sitesize,
                          mb_path **sitename, int *error)
{
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       sitelon:                   %p\n", *sitelon);
		fprintf(stderr, "dbg2       sitelat:                   %p\n", *sitelat);
		fprintf(stderr, "dbg2       sitetopo:                  %p\n", *sitetopo);
		fprintf(stderr, "dbg2       sitecolor:                 %p\n", *sitecolor);
		fprintf(stderr, "dbg2       sitesize:                  %p\n", *sitesize);
		fprintf(stderr, "dbg2       sitename:                  %p\n", *sitename);
	}

	/* free the arrays using mb_freed */
	int status = mb_freed(verbose, __FILE__, __LINE__, (void **)sitelon, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)sitelat, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)sitetopo, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)sitecolor, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)sitesize, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)sitename, error);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       sitelon:                   %p\n", *sitelon);
		fprintf(stderr, "dbg2       sitelat:                   %p\n", *sitelat);
		fprintf(stderr, "dbg2       sitetopo:                  %p\n", *sitetopo);
		fprintf(stderr, "dbg2       sitecolor:                 %p\n", *sitecolor);
		fprintf(stderr, "dbg2       sitesize:                  %p\n", *sitesize);
		fprintf(stderr, "dbg2       sitename:                  %p\n", *sitename);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_addsites(int verbose, size_t instance, int nsite, double *sitelon, double *sitelat, double *sitetopo, int *sitecolor,
                    int *sitesize, mb_path *sitename, int *error)
{
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       nsite:                     %d\n", nsite);
		fprintf(stderr, "dbg2       sitelon:                   %p\n", sitelon);
		fprintf(stderr, "dbg2       sitelat:                   %p\n", sitelat);
		fprintf(stderr, "dbg2       sitetopo:                  %p\n", sitetopo);
		fprintf(stderr, "dbg2       sitecolor:                 %p\n", sitecolor);
		fprintf(stderr, "dbg2       sitesize:                  %p\n", sitesize);
		fprintf(stderr, "dbg2       sitename:                  %p\n", sitename);
		for (int i = 0; i < nsite; i++) {
			fprintf(stderr, "dbg2       site:%d lon:%f lat:%f topo:%f color:%d size:%d name:%s\n", i, sitelon[i], sitelat[i],
			        sitetopo[i], sitecolor[i], sitesize[i], sitename[i]);
		}
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* make sure no site is selected */
	shared.shareddata.site_selected = MBV_SELECT_NONE;

	int status = MB_SUCCESS;

	/* allocate memory if required */
	if (shared.shareddata.nsite_alloc < shared.shareddata.nsite + nsite) {
		fprintf(stderr, "Have %d sites allocated but need %d + %d = %d\n", shared.shareddata.nsite_alloc, shared.shareddata.nsite,
		        nsite, shared.shareddata.nsite + nsite);
		shared.shareddata.nsite_alloc = shared.shareddata.nsite + nsite;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.nsite_alloc * sizeof(struct mbview_site_struct),
		                     (void **)&(shared.shareddata.sites), error);
		if (status == MB_FAILURE) {
			shared.shareddata.nsite_alloc = 0;
		}
		else {
			for (int i = shared.shareddata.nsite; i < shared.shareddata.nsite_alloc; i++) {
				shared.shareddata.sites[i].active = false;
				shared.shareddata.sites[i].color = MBV_COLOR_GREEN;
				shared.shareddata.sites[i].size = 1;
				shared.shareddata.sites[i].name[0] = '\0';
			}
		}
	}

	/* loop over the sites */
	int nadded = 0;
	for (int i = 0; i < nsite; i++) {
		/* get site positions in grid coordinates */
		double xgrid, ygrid, zdata;
		status = mbview_projectll2xyzgrid(instance, sitelon[i], sitelat[i], &xgrid, &ygrid, &zdata);

		/* use provided topo */
		if (sitetopo[i] != MBV_DEFAULT_NODATA) {
			zdata = sitetopo[i];
		}

		/* get site positions in display coordinates */
		double xdisplay, ydisplay, zdisplay;
		status = mbview_projectll2display(instance, sitelon[i], sitelat[i], zdata, &xdisplay, &ydisplay, &zdisplay);

		/* check for reasonable coordinates */
		if (fabs(xdisplay) < 1000.0 && fabs(ydisplay) < 1000.0 && fabs(zdisplay) < 1000.0) {

			/* add the new site */
			shared.shareddata.sites[shared.shareddata.nsite].active = true;
			shared.shareddata.sites[shared.shareddata.nsite].color = sitecolor[i];
			shared.shareddata.sites[shared.shareddata.nsite].size = sitesize[i];
			strcpy(shared.shareddata.sites[shared.shareddata.nsite].name, sitename[i]);
			shared.shareddata.sites[shared.shareddata.nsite].point.xgrid[instance] = xgrid;
			shared.shareddata.sites[shared.shareddata.nsite].point.ygrid[instance] = ygrid;
			shared.shareddata.sites[shared.shareddata.nsite].point.xlon = sitelon[i];
			shared.shareddata.sites[shared.shareddata.nsite].point.ylat = sitelat[i];
			shared.shareddata.sites[shared.shareddata.nsite].point.zdata = zdata;
			shared.shareddata.sites[shared.shareddata.nsite].point.xdisplay[instance] = xdisplay;
			shared.shareddata.sites[shared.shareddata.nsite].point.ydisplay[instance] = ydisplay;
			shared.shareddata.sites[shared.shareddata.nsite].point.zdisplay[instance] = zdisplay;

			/* set grid and display coordinates for all instances */
			mbview_updatepointw(instance, &(shared.shareddata.sites[shared.shareddata.nsite].point));

			/* set nsite */
			shared.shareddata.nsite++;
			nadded++;
			fprintf(stderr,"Added site %d added so far:%d total:%d\n",
			shared.shareddata.nsite-1, nadded, shared.shareddata.nsite);
		}

		/* report failure due to unreasonable coordinates */
		else {
			fprintf(stderr,
			        "Failed to add site at position lon:%f lat:%f due to display coordinate projection (%f %f %f) far outside "
			        "view...\n",
			        sitelon[i], sitelat[i], xdisplay, ydisplay, zdisplay);
			XBell(view->dpy, 100);
		}
	}

	/* make sites viewable */
	if (nadded > 0) {
		data->site_view_mode = MBV_VIEW_ON;
	}

	/* update site list */
	mbview_updatesitelist();

	/* print site debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  Site data in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  Site values:\n");
		fprintf(stderr, "dbg2       site_view_mode:      %d\n", data->site_view_mode);
		fprintf(stderr, "dbg2       site_mode:           %d\n", shared.shareddata.site_mode);
		fprintf(stderr, "dbg2       nsite:               %d\n", shared.shareddata.nsite);
		fprintf(stderr, "dbg2       nsite_alloc:         %d\n", shared.shareddata.nsite_alloc);
		fprintf(stderr, "dbg2       site_selected:       %d\n", shared.shareddata.site_selected);
		for (int i = 0; i < shared.shareddata.nsite; i++) {
			fprintf(stderr, "dbg2       site %d active:      %d\n", i, shared.shareddata.sites[i].active);
			fprintf(stderr, "dbg2       site %d color:       %d\n", i, shared.shareddata.sites[i].color);
			fprintf(stderr, "dbg2       site %d size:        %d\n", i, shared.shareddata.sites[i].size);
			fprintf(stderr, "dbg2       site %d name:        %s\n", i, shared.shareddata.sites[i].name);
			fprintf(stderr, "dbg2       site %d xgrid:       %f\n", i, shared.shareddata.sites[i].point.xgrid[instance]);
			fprintf(stderr, "dbg2       site %d ygrid:       %f\n", i, shared.shareddata.sites[i].point.ygrid[instance]);
			fprintf(stderr, "dbg2       site %d xlon:        %f\n", i, shared.shareddata.sites[i].point.xlon);
			fprintf(stderr, "dbg2       site %d ylat:        %f\n", i, shared.shareddata.sites[i].point.ylat);
			fprintf(stderr, "dbg2       site %d zdata:       %f\n", i, shared.shareddata.sites[i].point.zdata);
			fprintf(stderr, "dbg2       site %d xdisplay:    %f\n", i, shared.shareddata.sites[i].point.xdisplay[instance]);
			fprintf(stderr, "dbg2       site %d ydisplay:    %f\n", i, shared.shareddata.sites[i].point.ydisplay[instance]);
			fprintf(stderr, "dbg2       site %d zdisplay:    %f\n", i, shared.shareddata.sites[i].point.zdisplay[instance]);
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getsites(int verbose, size_t instance, int *nsite, double *sitelon, double *sitelat, double *sitetopo, int *sitecolor,
                    int *sitesize, mb_path *sitename, int *error)
{
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       nsite:                     %p\n", nsite);
		fprintf(stderr, "dbg2       sitelon:                   %p\n", sitelon);
		fprintf(stderr, "dbg2       sitelat:                   %p\n", sitelat);
		fprintf(stderr, "dbg2       sitetopo:                  %p\n", sitetopo);
		fprintf(stderr, "dbg2       sitecolor:                 %p\n", sitecolor);
		fprintf(stderr, "dbg2       sitesize:                  %p\n", sitesize);
		fprintf(stderr, "dbg2       sitename:                  %p\n", sitename);
	}

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* print site debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  Site data in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  Site values:\n");
		fprintf(stderr, "dbg2       site_view_mode:      %d\n", data->site_view_mode);
		fprintf(stderr, "dbg2       site_mode:           %d\n", shared.shareddata.site_mode);
		fprintf(stderr, "dbg2       nsite:               %d\n", shared.shareddata.nsite);
		fprintf(stderr, "dbg2       nsite_alloc:         %d\n", shared.shareddata.nsite_alloc);
		fprintf(stderr, "dbg2       site_selected:       %d\n", shared.shareddata.site_selected);
		for (int i = 0; i < shared.shareddata.nsite; i++) {
			fprintf(stderr, "dbg2       site %d active:      %d\n", i, shared.shareddata.sites[i].active);
			fprintf(stderr, "dbg2       site %d color:       %d\n", i, shared.shareddata.sites[i].color);
			fprintf(stderr, "dbg2       site %d size:        %d\n", i, shared.shareddata.sites[i].size);
			fprintf(stderr, "dbg2       site %d name:        %s\n", i, shared.shareddata.sites[i].name);
			fprintf(stderr, "dbg2       site %d xgrid:       %f\n", i, shared.shareddata.sites[i].point.xgrid[instance]);
			fprintf(stderr, "dbg2       site %d ygrid:       %f\n", i, shared.shareddata.sites[i].point.ygrid[instance]);
			fprintf(stderr, "dbg2       site %d xlon:        %f\n", i, shared.shareddata.sites[i].point.xlon);
			fprintf(stderr, "dbg2       site %d ylat:        %f\n", i, shared.shareddata.sites[i].point.ylat);
			fprintf(stderr, "dbg2       site %d zdata:       %f\n", i, shared.shareddata.sites[i].point.zdata);
			fprintf(stderr, "dbg2       site %d xdisplay:    %f\n", i, shared.shareddata.sites[i].point.xdisplay[instance]);
			fprintf(stderr, "dbg2       site %d ydisplay:    %f\n", i, shared.shareddata.sites[i].point.ydisplay[instance]);
			fprintf(stderr, "dbg2       site %d zdisplay:    %f\n", i, shared.shareddata.sites[i].point.zdisplay[instance]);
		}
	}

	/* check that the array pointers are not NULL */
	int status = MB_SUCCESS;
	if (sitelon == NULL || sitelat == NULL || sitetopo == NULL || sitecolor == NULL || sitesize == NULL || sitename == NULL) {
		status = MB_FAILURE;
		*error = MB_ERROR_DATA_NOT_INSERTED;
	}

	/* otherwise go get the site data */
	else {
		/* loop over the sites */
    int j = 0;
		for (int i = 0; i < shared.shareddata.nsite; i++) {
      if (shared.shareddata.sites[i].active) {
  			sitelon[j] = shared.shareddata.sites[i].point.xlon;
  			sitelat[j] = shared.shareddata.sites[i].point.ylat;
  			sitetopo[j] = shared.shareddata.sites[i].point.zdata;
  			sitecolor[j] = shared.shareddata.sites[i].color;
  			sitesize[j] = shared.shareddata.sites[i].size;
  			strcpy(sitename[j], shared.shareddata.sites[i].name);
		    j += 1;
      }
      *nsite = j;
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nsite:                     %d\n", *nsite);
		for (int i = 0; i < *nsite; i++) {
			fprintf(stderr, "dbg2       site:%d lon:%f lat:%f topo:%f color:%d size:%d name:%s\n", i, sitelon[i], sitelat[i],
			        sitetopo[i], sitecolor[i], sitesize[i], sitename[i]);
		}
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_site_delete(size_t instance, int isite) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       isite:            %d\n", isite);
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}

	/* get view */
	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* delete site if its the same as previously selected */
	int status = MB_SUCCESS;
	if (isite >= 0 && isite < shared.shareddata.nsite) {
		/* move site data if necessary */
		for (int i = isite; i < shared.shareddata.nsite - 1; i++) {
			shared.shareddata.sites[i] = shared.shareddata.sites[i + 1];
		}

		/* set nsite */
		shared.shareddata.nsite--;

		/* no selection */
		shared.shareddata.site_selected = MBV_SELECT_NONE;
	}
	else {
		status = MB_FAILURE;
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getnavcount(int verbose, size_t instance, int *nnav, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	/* get number of navs */
	*nnav = shared.shareddata.nnav;

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nnav:                      %d\n", *nnav);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getnavpointcount(int verbose, size_t instance, int nav, int *npoint, int *nintpoint, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       nav:                     %d\n", nav);
	}

	/* get number of points in specified nav */
	*npoint = 0;
	*nintpoint = 0;
	if (nav >= 0 && nav < shared.shareddata.nnav) {
		*npoint = shared.shareddata.navs[nav].npoints;
		for (int i = 0; i < *npoint - 1; i++) {
			if (shared.shareddata.navs[nav].segments[i].nls > 2)
				*nintpoint += shared.shareddata.navs[nav].segments[i].nls - 2;
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       npoint:                    %d\n", *npoint);
		fprintf(stderr, "dbg2       nintpoint:                 %d\n", *nintpoint);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_allocnavarrays(int verbose, int npointtotal, double **time_d, double **navlon, double **navlat, double **navz,
                          double **heading, double **speed, double **navportlon, double **navportlat, double **navstbdlon,
                          double **navstbdlat, int **line, int **shot, int **cdp, int *error) {
	fprintf(stderr, "mbview_allocnavarrays: %d points\n", npointtotal);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       npointtotal:               %d\n", npointtotal);
		fprintf(stderr, "dbg2       time_d:                    %p\n", *time_d);
		fprintf(stderr, "dbg2       navlon:                    %p\n", *navlon);
		fprintf(stderr, "dbg2       navlat:                    %p\n", *navlat);
		fprintf(stderr, "dbg2       navz:                      %p\n", *navz);
		fprintf(stderr, "dbg2       heading:                   %p\n", *heading);
		fprintf(stderr, "dbg2       speed:                     %p\n", *speed);
		if (navportlon != NULL)
			fprintf(stderr, "dbg2       navportlon:                %p\n", *navportlon);
		if (navportlat != NULL)
			fprintf(stderr, "dbg2       navportlat:                %p\n", *navportlat);
		if (navstbdlon != NULL)
			fprintf(stderr, "dbg2       navstbdlon:                %p\n", *navstbdlon);
		if (navstbdlat != NULL)
			fprintf(stderr, "dbg2       navstbdlat:                %p\n", *navstbdlat);
		if (line != NULL)
			fprintf(stderr, "dbg2       line:                      %p\n", *line);
		if (shot != NULL)
			fprintf(stderr, "dbg2       shot:                      %p\n", *shot);
		if (cdp != NULL)
			fprintf(stderr, "dbg2       cdp:                       %p\n", *cdp);
	}

	/* allocate the arrays using mb_reallocd */
	int status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)time_d, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navlon, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navlat, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navz, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)heading, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)speed, error);
	if (status == MB_SUCCESS && navportlon != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navportlon, error);
	if (status == MB_SUCCESS && navportlat != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navportlat, error);
	if (status == MB_SUCCESS && navstbdlon != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navstbdlon, error);
	if (status == MB_SUCCESS && navstbdlat != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)navstbdlat, error);
	if (status == MB_SUCCESS && line != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(int), (void **)line, error);
	if (status == MB_SUCCESS && shot != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(int), (void **)shot, error);
	if (status == MB_SUCCESS && cdp != NULL)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(int), (void **)cdp, error);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       time_d:                    %p\n", *time_d);
		fprintf(stderr, "dbg2       navlon:                    %p\n", *navlon);
		fprintf(stderr, "dbg2       navlat:                    %p\n", *navlat);
		fprintf(stderr, "dbg2       navz:                      %p\n", *navz);
		fprintf(stderr, "dbg2       heading:                   %p\n", *heading);
		fprintf(stderr, "dbg2       speed:                     %p\n", *speed);
		if (navportlon != NULL)
			fprintf(stderr, "dbg2       navportlon:                %p\n", *navportlon);
		if (navportlat != NULL)
			fprintf(stderr, "dbg2       navportlat:                %p\n", *navportlat);
		if (navstbdlon != NULL)
			fprintf(stderr, "dbg2       navstbdlon:                %p\n", *navstbdlon);
		if (navstbdlat != NULL)
			fprintf(stderr, "dbg2       navstbdlat:                %p\n", *navstbdlat);
		if (line != NULL)
			fprintf(stderr, "dbg2       line:                      %p\n", *line);
		if (shot != NULL)
			fprintf(stderr, "dbg2       shot:                      %p\n", *shot);
		if (cdp != NULL)
			fprintf(stderr, "dbg2       cdp:                       %p\n", *cdp);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_freenavarrays(int verbose, double **time_d, double **navlon, double **navlat, double **navz, double **heading,
                         double **speed, double **navportlon, double **navportlat, double **navstbdlon, double **navstbdlat,
                         int **line, int **shot, int **cdp, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       time_d:                    %p\n", *time_d);
		fprintf(stderr, "dbg2       navlon:                    %p\n", *navlon);
		fprintf(stderr, "dbg2       navlat:                    %p\n", *navlat);
		fprintf(stderr, "dbg2       navz:                      %p\n", *navz);
		fprintf(stderr, "dbg2       heading:                   %p\n", *heading);
		fprintf(stderr, "dbg2       speed:                     %p\n", *speed);
		if (navportlon != NULL)
			fprintf(stderr, "dbg2       navportlon:                %p\n", *navportlon);
		if (navportlat != NULL)
			fprintf(stderr, "dbg2       navportlat:                %p\n", *navportlat);
		if (navstbdlon != NULL)
			fprintf(stderr, "dbg2       navstbdlon:                %p\n", *navstbdlon);
		if (navstbdlat != NULL)
			fprintf(stderr, "dbg2       navstbdlat:                %p\n", *navstbdlat);
		if (line != NULL)
			fprintf(stderr, "dbg2       line:                      %p\n", *line);
		if (shot != NULL)
			fprintf(stderr, "dbg2       shot:                      %p\n", *shot);
		if (cdp != NULL)
			fprintf(stderr, "dbg2       cdp:                       %p\n", *cdp);
	}

	/* free the arrays using mb_freed */
	int status = mb_freed(verbose, __FILE__, __LINE__, (void **)time_d, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)navlon, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)navlat, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)navz, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)heading, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)speed, error);
	if (navportlon != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)navportlon, error);
	if (navportlat != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)navportlat, error);
	if (navstbdlon != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)navstbdlon, error);
	if (navstbdlat != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)navstbdlat, error);
	if (line != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)line, error);
	if (shot != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)shot, error);
	if (cdp != NULL)
		status = mb_freed(verbose, __FILE__, __LINE__, (void **)cdp, error);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       time_d:                    %p\n", *time_d);
		fprintf(stderr, "dbg2       navlon:                    %p\n", *navlon);
		fprintf(stderr, "dbg2       navlat:                    %p\n", *navlat);
		fprintf(stderr, "dbg2       navz:                      %p\n", *navz);
		fprintf(stderr, "dbg2       heading:                   %p\n", *heading);
		fprintf(stderr, "dbg2       speed:                     %p\n", *speed);
		if (navportlon != NULL)
			fprintf(stderr, "dbg2       navportlon:                %p\n", *navportlon);
		if (navportlat != NULL)
			fprintf(stderr, "dbg2       navportlat:                %p\n", *navportlat);
		if (navstbdlon != NULL)
			fprintf(stderr, "dbg2       navstbdlon:                %p\n", *navstbdlon);
		if (navstbdlat != NULL)
			fprintf(stderr, "dbg2       navstbdlat:                %p\n", *navstbdlat);
		if (line != NULL)
			fprintf(stderr, "dbg2       line:                      %p\n", *line);
		if (shot != NULL)
			fprintf(stderr, "dbg2       shot:                      %p\n", *shot);
		if (cdp != NULL)
			fprintf(stderr, "dbg2       cdp:                       %p\n", *cdp);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_addnav(int verbose, size_t instance, int npoint, double *time_d, double *navlon, double *navlat, double *navz,
                  double *heading, double *speed, double *navportlon, double *navportlat, double *navstbdlon, double *navstbdlat,
                  unsigned int *line, unsigned int *shot, unsigned int *cdp, int navcolor, int navsize, mb_path navname, int navpathstatus,
                  mb_path navpathraw, mb_path navpathprocessed, int navformat, bool navswathbounds, bool navline, bool navshot,
                  bool navcdp, int decimation, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       npoint:                    %d\n", npoint);
		for (int i = 0; i < npoint; i++) {
			fprintf(stderr, "dbg2       point:%d time_d:%f lon:%f lat:%f z:%f heading:%f zpeed:%f\n", i, time_d[i], navlon[i],
			        navlat[i], navz[i], heading[i], speed[i]);
		}
		if (navswathbounds)
			for (int i = 0; i < npoint; i++) {
				fprintf(stderr, "dbg2       point:%d port: lon:%f lat:%f  stbd: lon:%f lat:%f\n", i, navportlon[i], navportlat[i],
				        navstbdlon[i], navstbdlat[i]);
			}
		if (navline)
			for (int i = 0; i < npoint; i++) {
				fprintf(stderr, "dbg2       point:%d line:%d\n", i, line[i]);
			}
		if (navshot)
			for (int i = 0; i < npoint; i++) {
				fprintf(stderr, "dbg2       point:%d shot:%d\n", i, shot[i]);
			}
		if (navcdp)
			for (int i = 0; i < npoint; i++) {
				fprintf(stderr, "dbg2       point:%d cdp: %d\n", i, cdp[i]);
			}
		fprintf(stderr, "dbg2       navcolor:                  %d\n", navcolor);
		fprintf(stderr, "dbg2       navsize:                   %d\n", navsize);
		fprintf(stderr, "dbg2       navname:                   %s\n", navname);
		fprintf(stderr, "dbg2       navpathstatus:             %d\n", navpathstatus);
		fprintf(stderr, "dbg2       navpathraw:                %s\n", navpathraw);
		fprintf(stderr, "dbg2       navpathprocessed:          %s\n", navpathprocessed);
		fprintf(stderr, "dbg2       navformat:                 %d\n", navformat);
		fprintf(stderr, "dbg2       navswathbounds:            %d\n", navswathbounds);
		fprintf(stderr, "dbg2       navline:                   %d\n", navline);
		fprintf(stderr, "dbg2       navshot:                   %d\n", navshot);
		fprintf(stderr, "dbg2       navcdp:                    %d\n", navcdp);
		fprintf(stderr, "dbg2       decimation:                %d\n", decimation);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* make sure no nav is selected */
	shared.shareddata.nav_selected[0] = MBV_SELECT_NONE;
	shared.shareddata.nav_selected[1] = MBV_SELECT_NONE;
	shared.shareddata.nav_point_selected[0] = MBV_SELECT_NONE;
	shared.shareddata.nav_point_selected[1] = MBV_SELECT_NONE;
	shared.shareddata.nav_selected_mbnavadjust[0] = MBV_SELECT_NONE;
	shared.shareddata.nav_selected_mbnavadjust[1] = MBV_SELECT_NONE;

	/* set nav id so that new nav is created */
	int inav = shared.shareddata.nnav;

	int status = MB_SUCCESS;

	/* allocate memory for a new nav if required */
	if (shared.shareddata.nnav_alloc < shared.shareddata.nnav + 1) {
		shared.shareddata.nnav_alloc = shared.shareddata.nnav + 1;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.nnav_alloc * sizeof(struct mbview_nav_struct),
		                     (void **)&(shared.shareddata.navs), error);
		if (status == MB_FAILURE) {
			shared.shareddata.nnav_alloc = 0;
		}
		else {
			for (int i = shared.shareddata.nnav; i < shared.shareddata.nnav_alloc; i++) {
				shared.shareddata.navs[i].active = false;
				shared.shareddata.navs[i].color = MBV_COLOR_RED;
				shared.shareddata.navs[i].size = 4;
				shared.shareddata.navs[i].name[0] = '\0';
				shared.shareddata.navs[i].pathstatus = MB_PROCESSED_NONE;
				shared.shareddata.navs[i].pathraw[0] = '\0';
				shared.shareddata.navs[i].pathprocessed[0] = '\0';
				shared.shareddata.navs[i].format = 0;
				shared.shareddata.navs[i].swathbounds = false;
				shared.shareddata.navs[i].line = false;
				shared.shareddata.navs[i].shot = false;
				shared.shareddata.navs[i].cdp = false;
				shared.shareddata.navs[i].decimation = false;
				shared.shareddata.navs[i].npoints = 0;
				shared.shareddata.navs[i].npoints_alloc = 0;
				shared.shareddata.navs[i].nselected = 0;
				shared.shareddata.navs[i].navpts = NULL;
				shared.shareddata.navs[i].segments = NULL;
			}
		}
	}

	/* allocate memory to for nav arrays */
	if (shared.shareddata.navs[inav].npoints_alloc < npoint) {
		shared.shareddata.navs[inav].npoints_alloc = npoint;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__,
		                     shared.shareddata.navs[inav].npoints_alloc * sizeof(struct mbview_navpointw_struct),
		                     (void **)&(shared.shareddata.navs[inav].navpts), error);
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__,
		                     shared.shareddata.navs[inav].npoints_alloc * sizeof(struct mbview_linesegmentw_struct),
		                     (void **)&(shared.shareddata.navs[inav].segments), error);
    memset((void *)shared.shareddata.navs[inav].segments, 0,
            (size_t)shared.shareddata.navs[inav].npoints_alloc * sizeof(struct mbview_linesegmentw_struct));
	}

	/* add the new nav */
	if (status == MB_SUCCESS) {
		/* set nnav */
		shared.shareddata.nnav++;

		/* set color size and name for new nav */
		shared.shareddata.navs[inav].active = true;
		shared.shareddata.navs[inav].color = navcolor;
		shared.shareddata.navs[inav].size = navsize;
		strcpy(shared.shareddata.navs[inav].name, navname);
		shared.shareddata.navs[inav].pathstatus = navpathstatus;
		strcpy(shared.shareddata.navs[inav].pathraw, navpathraw);
		strcpy(shared.shareddata.navs[inav].pathprocessed, navpathprocessed);
		shared.shareddata.navs[inav].format = navformat;
		shared.shareddata.navs[inav].swathbounds = navswathbounds;
		shared.shareddata.navs[inav].line = navline;
		shared.shareddata.navs[inav].shot = navshot;
		shared.shareddata.navs[inav].cdp = navcdp;
		shared.shareddata.navs[inav].decimation = decimation;

		/* loop over the points in the new nav */
		shared.shareddata.navs[inav].npoints = npoint;
		for (int i = 0; i < npoint; i++) {
			/* set status values */
			shared.shareddata.navs[inav].navpts[i].draped = false;
			shared.shareddata.navs[inav].navpts[i].selected = false;

			/* set time and shot info */
			shared.shareddata.navs[inav].navpts[i].time_d = time_d[i];
			shared.shareddata.navs[inav].navpts[i].heading = heading[i];
			shared.shareddata.navs[inav].navpts[i].speed = speed[i];
			if (shared.shareddata.navs[inav].line)
				shared.shareddata.navs[inav].navpts[i].line = line[i];
			if (shared.shareddata.navs[inav].shot)
				shared.shareddata.navs[inav].navpts[i].shot = shot[i];
			if (shared.shareddata.navs[inav].cdp)
				shared.shareddata.navs[inav].navpts[i].cdp = cdp[i];

			/* ************************************************* */
			/* get nav positions in grid and display coordinates */
			shared.shareddata.navs[inav].navpts[i].point.xlon = navlon[i];
			shared.shareddata.navs[inav].navpts[i].point.ylat = navlat[i];
			shared.shareddata.navs[inav].navpts[i].point.zdata = navz[i];
			status = mbview_projectfromlonlat(instance, shared.shareddata.navs[inav].navpts[i].point.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].point.ylat,
			                                  shared.shareddata.navs[inav].navpts[i].point.zdata,
			                                  &(shared.shareddata.navs[inav].navpts[i].point.xgrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].point.ygrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].point.xdisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].point.ydisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].point.zdisplay[instance]));
			mbview_updatepointw(instance, &(shared.shareddata.navs[inav].navpts[i].point));

			/* fprintf(stderr,"Depth: llz:%.10f %.10f %.10f   grid:%.10f %.10f   dpy:%.10f %.10f %.10f\n",
			shared.shareddata.navs[inav].navpts[i].point.xlon,
			shared.shareddata.navs[inav].navpts[i].point.ylat,
			shared.shareddata.navs[inav].navpts[i].point.zdata,
			shared.shareddata.navs[inav].navpts[i].point.xgrid[instance],
			shared.shareddata.navs[inav].navpts[i].point.ygrid[instance],
			shared.shareddata.navs[inav].navpts[i].point.xdisplay[instance],
			shared.shareddata.navs[inav].navpts[i].point.ydisplay[instance],
			shared.shareddata.navs[inav].navpts[i].point.zdisplay[instance]); */

			/* ************************************************* */
			/* get center on-bottom nav positions in grid coordinates */
			shared.shareddata.navs[inav].navpts[i].pointcntr.xlon = navlon[i];
			shared.shareddata.navs[inav].navpts[i].pointcntr.ylat = navlat[i];
			status = mbview_projectll2xyzgrid(instance, shared.shareddata.navs[inav].navpts[i].pointcntr.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].pointcntr.ylat,
			                                  &(shared.shareddata.navs[inav].navpts[i].pointcntr.xgrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointcntr.ygrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointcntr.zdata));

			/* get center on-bottom nav positions in display coordinates */
			status = mbview_projectll2display(instance, shared.shareddata.navs[inav].navpts[i].pointcntr.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].pointcntr.ylat,
			                                  shared.shareddata.navs[inav].navpts[i].pointcntr.zdata,
			                                  &(shared.shareddata.navs[inav].navpts[i].pointcntr.xdisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointcntr.ydisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointcntr.zdisplay[instance]));

			/* get center on-bottom nav positions for all active instances */
			mbview_updatepointw(instance, &(shared.shareddata.navs[inav].navpts[i].pointcntr));

			/* ************************************************* */
			/* get port swathbound nav positions in grid and display coordinates */
			shared.shareddata.navs[inav].navpts[i].pointport.xlon = navportlon[i];
			shared.shareddata.navs[inav].navpts[i].pointport.ylat = navportlat[i];
			status = mbview_projectll2xyzgrid(instance, shared.shareddata.navs[inav].navpts[i].pointport.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].pointport.ylat,
			                                  &(shared.shareddata.navs[inav].navpts[i].pointport.xgrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointport.ygrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointport.zdata));

			/* get port on-bottom nav positions in display coordinates */
			status = mbview_projectll2display(instance, shared.shareddata.navs[inav].navpts[i].pointport.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].pointport.ylat,
			                                  shared.shareddata.navs[inav].navpts[i].pointport.zdata,
			                                  &(shared.shareddata.navs[inav].navpts[i].pointport.xdisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointport.ydisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointport.zdisplay[instance]));

			/* get port on-bottom nav positions for all active instances */
			mbview_updatepointw(instance, &(shared.shareddata.navs[inav].navpts[i].pointport));

			/* ************************************************* */
			/* get starboard swathbound nav positions in grid coordinates */
			shared.shareddata.navs[inav].navpts[i].pointstbd.xlon = navstbdlon[i];
			shared.shareddata.navs[inav].navpts[i].pointstbd.ylat = navstbdlat[i];
			status = mbview_projectll2xyzgrid(instance, shared.shareddata.navs[inav].navpts[i].pointstbd.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].pointstbd.ylat,
			                                  &(shared.shareddata.navs[inav].navpts[i].pointstbd.xgrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointstbd.ygrid[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointstbd.zdata));

			/* get starboard on-bottom nav positions in display coordinates */
			status = mbview_projectll2display(instance, shared.shareddata.navs[inav].navpts[i].pointstbd.xlon,
			                                  shared.shareddata.navs[inav].navpts[i].pointstbd.ylat,
			                                  shared.shareddata.navs[inav].navpts[i].pointstbd.zdata,
			                                  &(shared.shareddata.navs[inav].navpts[i].pointstbd.xdisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointstbd.ydisplay[instance]),
			                                  &(shared.shareddata.navs[inav].navpts[i].pointstbd.zdisplay[instance]));

			/* get starboard on-bottom nav positions for all active instances */
			mbview_updatepointw(instance, &(shared.shareddata.navs[inav].navpts[i].pointstbd));

			/* ************************************************* */
		}

		/* drape the segments */
		for (int i = 0; i < shared.shareddata.navs[inav].npoints - 1; i++) {
			shared.shareddata.navs[inav].segments[i].endpoints[0] = shared.shareddata.navs[inav].navpts[i].pointcntr;
			shared.shareddata.navs[inav].segments[i].endpoints[1] = shared.shareddata.navs[inav].navpts[i + 1].pointcntr;

			/* drape the segment */
			mbview_drapesegmentw(instance, &(shared.shareddata.navs[inav].segments[i]));

			/* update the segment for all active instances */
			mbview_updatesegmentw(instance, &(shared.shareddata.navs[inav].segments[i]));
		}

		/* make navs viewable */
		data->nav_view_mode = MBV_VIEW_ON;

		/* update nav data list */
		mbview_updatenavlist();
	}

	/* print nav debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  Nav data altered in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  Nav values:\n");
		fprintf(stderr, "dbg2       nav_mode:                  %d\n", shared.shareddata.nav_mode);
		fprintf(stderr, "dbg2       nav_view_mode:             %d\n", data->nav_view_mode);
		fprintf(stderr, "dbg2       navswathbounds_view_mode:  %d\n", data->navswathbounds_view_mode);
		fprintf(stderr, "dbg2       navdrape_view_mode:        %d\n", data->navdrape_view_mode);
		fprintf(stderr, "dbg2       nnav:                      %d\n", shared.shareddata.nnav);
		fprintf(stderr, "dbg2       nnav_alloc:                %d\n", shared.shareddata.nnav_alloc);
		fprintf(stderr, "dbg2       nav_selected[0]:           %d\n", shared.shareddata.nav_selected[0]);
		fprintf(stderr, "dbg2       nav_selected[1]:           %d\n", shared.shareddata.nav_selected[1]);
		fprintf(stderr, "dbg2       nav_point_selected:        %p\n", shared.shareddata.nav_point_selected);
		for (int i = 0; i < shared.shareddata.nnav; i++) {
			fprintf(stderr, "dbg2       nav %d active:        %d\n", i, shared.shareddata.navs[i].active);
			fprintf(stderr, "dbg2       nav %d color:         %d\n", i, shared.shareddata.navs[i].color);
			fprintf(stderr, "dbg2       nav %d size:          %d\n", i, shared.shareddata.navs[i].size);
			fprintf(stderr, "dbg2       nav %d name:          %s\n", i, shared.shareddata.navs[i].name);
			fprintf(stderr, "dbg2       nav %d pathstatus:    %d\n", i, shared.shareddata.navs[i].pathstatus);
			fprintf(stderr, "dbg2       nav %d pathraw:       %s\n", i, shared.shareddata.navs[i].pathraw);
			fprintf(stderr, "dbg2       nav %d pathprocessed: %s\n", i, shared.shareddata.navs[i].pathprocessed);
			fprintf(stderr, "dbg2       nav %d swathbounds:   %d\n", i, shared.shareddata.navs[i].swathbounds);
			fprintf(stderr, "dbg2       nav %d line:          %d\n", i, shared.shareddata.navs[i].line);
			fprintf(stderr, "dbg2       nav %d shot:          %d\n", i, shared.shareddata.navs[i].shot);
			fprintf(stderr, "dbg2       nav %d cdp:           %d\n", i, shared.shareddata.navs[i].cdp);
			fprintf(stderr, "dbg2       nav %d decimation:    %d\n", i, shared.shareddata.navs[i].decimation);
			fprintf(stderr, "dbg2       nav %d npoints:       %d\n", i, shared.shareddata.navs[i].npoints);
			fprintf(stderr, "dbg2       nav %d npoints_alloc: %d\n", i, shared.shareddata.navs[i].npoints_alloc);
			fprintf(stderr, "dbg2       nav %d nselected:     %d\n", i, shared.shareddata.navs[i].nselected);
			for (int j = 0; j < shared.shareddata.navs[i].npoints; j++) {
				fprintf(stderr, "dbg2       nav %d %d draped:   %d\n", i, j, shared.shareddata.navs[i].navpts[j].draped);
				fprintf(stderr, "dbg2       nav %d %d selected: %d\n", i, j, shared.shareddata.navs[i].navpts[j].selected);
				fprintf(stderr, "dbg2       nav %d %d time_d:   %f\n", i, j, shared.shareddata.navs[i].navpts[j].time_d);
				fprintf(stderr, "dbg2       nav %d %d heading:  %f\n", i, j, shared.shareddata.navs[i].navpts[j].heading);
				fprintf(stderr, "dbg2       nav %d %d speed:    %f\n", i, j, shared.shareddata.navs[i].navpts[j].speed);
				fprintf(stderr, "dbg2       nav %d %d line:     %d\n", i, j, shared.shareddata.navs[i].navpts[j].line);
				fprintf(stderr, "dbg2       nav %d %d shot:     %d\n", i, j, shared.shareddata.navs[i].navpts[j].shot);
				fprintf(stderr, "dbg2       nav %d %d cdp:      %d\n", i, j, shared.shareddata.navs[i].navpts[j].cdp);

				fprintf(stderr, "dbg2       nav %d %d xgrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].point.xgrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d ygrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].point.ygrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d xlon:     %f\n", i, j, shared.shareddata.navs[i].navpts[j].point.xlon);
				fprintf(stderr, "dbg2       nav %d %d ylat:     %f\n", i, j, shared.shareddata.navs[i].navpts[j].point.ylat);
				fprintf(stderr, "dbg2       nav %d %d zdata:    %f\n", i, j, shared.shareddata.navs[i].navpts[j].point.zdata);
				fprintf(stderr, "dbg2       nav %d %d xdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].point.xdisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d ydisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].point.ydisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d zdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].point.zdisplay[instance]);

				fprintf(stderr, "dbg2       nav %d %d stbd xgrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.xgrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d stbd ygrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.ygrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d stbd xlon:     %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.xlon);
				fprintf(stderr, "dbg2       nav %d %d stbd ylat:     %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.ylat);
				fprintf(stderr, "dbg2       nav %d %d stbd zdata:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.zdata);
				fprintf(stderr, "dbg2       nav %d %d stbd xdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.xdisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d stbd ydisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.ydisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d stbd zdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointport.zdisplay[instance]);

				fprintf(stderr, "dbg2       nav %d %d cntr xgrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.xgrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d cntr ygrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.ygrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d cntr xlon:     %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.xlon);
				fprintf(stderr, "dbg2       nav %d %d cntr ylat:     %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.ylat);
				fprintf(stderr, "dbg2       nav %d %d cntr zdata:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.zdata);
				fprintf(stderr, "dbg2       nav %d %d cntr xdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.xdisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d cntr ydisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.ydisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d cntr zdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointcntr.zdisplay[instance]);

				fprintf(stderr, "dbg2       nav %d %d port xgrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.xgrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d port ygrid:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.ygrid[instance]);
				fprintf(stderr, "dbg2       nav %d %d port xlon:     %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.xlon);
				fprintf(stderr, "dbg2       nav %d %d port ylat:     %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.ylat);
				fprintf(stderr, "dbg2       nav %d %d port zdata:    %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.zdata);
				fprintf(stderr, "dbg2       nav %d %d port xdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.xdisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d port ydisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.ydisplay[instance]);
				fprintf(stderr, "dbg2       nav %d %d port zdisplay: %f\n", i, j,
				        shared.shareddata.navs[i].navpts[j].pointstbd.zdisplay[instance]);
			}
			for (int j = 0; j < shared.shareddata.navs[i].npoints - 1; j++) {
				fprintf(stderr, "dbg2       nav %d %d nls:          %d\n", i, j, shared.shareddata.navs[i].segments[j].nls);
				fprintf(stderr, "dbg2       nav %d %d nls_alloc:    %d\n", i, j, shared.shareddata.navs[i].segments[j].nls_alloc);
				fprintf(stderr, "dbg2       nav %d %d endpoints[0]: %p\n", i, j,
				        &shared.shareddata.navs[i].segments[j].endpoints[0]);
				fprintf(stderr, "dbg2       nav %d %d endpoints[1]: %p\n", i, j,
				        &shared.shareddata.navs[i].segments[j].endpoints[1]);
			}
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_nav_delete(size_t instance, int inav) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       inav:            %d\n", inav);
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}

	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* delete nav if its the same as previously selected */
	if (inav >= 0 && inav < shared.shareddata.nnav) {
		/* free memory for deleted nav */
		int error = MB_ERROR_NO_ERROR;
		for (int j = 0; j < shared.shareddata.navs[inav].npoints_alloc; j++) {
			struct mbview_linesegmentw_struct *segment = &shared.shareddata.navs[inav].segments[j];
			if (segment->nls_alloc > 0 && segment->lspoints != NULL) {
				mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(segment->lspoints), &error);
				segment->nls = 0;
				segment->nls_alloc = 0;
			}
		}
		mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.navs[inav].navpts), &error);
		mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.navs[inav].segments), &error);

		/* move nav data if necessary */
		for (int i = inav; i < shared.shareddata.nnav - 1; i++) {
			shared.shareddata.navs[i] = shared.shareddata.navs[i + 1];
		}

		/* reset last nav */
		shared.shareddata.navs[shared.shareddata.nnav - 1].active = false;
		shared.shareddata.navs[shared.shareddata.nnav - 1].color = MBV_COLOR_RED;
		shared.shareddata.navs[shared.shareddata.nnav - 1].size = 4;
		shared.shareddata.navs[shared.shareddata.nnav - 1].name[0] = '\0';
		shared.shareddata.navs[shared.shareddata.nnav - 1].pathstatus = MB_PROCESSED_NONE;
		shared.shareddata.navs[shared.shareddata.nnav - 1].pathraw[0] = '\0';
		shared.shareddata.navs[shared.shareddata.nnav - 1].pathprocessed[0] = '\0';
		shared.shareddata.navs[shared.shareddata.nnav - 1].format = 0;
		shared.shareddata.navs[shared.shareddata.nnav - 1].swathbounds = false;
		shared.shareddata.navs[shared.shareddata.nnav - 1].line = false;
		shared.shareddata.navs[shared.shareddata.nnav - 1].shot = false;
		shared.shareddata.navs[shared.shareddata.nnav - 1].cdp = false;
		shared.shareddata.navs[shared.shareddata.nnav - 1].decimation = 1;
		shared.shareddata.navs[shared.shareddata.nnav - 1].npoints = 0;
		shared.shareddata.navs[shared.shareddata.nnav - 1].npoints_alloc = 0;
		shared.shareddata.navs[shared.shareddata.nnav - 1].navpts = NULL;
		shared.shareddata.navs[shared.shareddata.nnav - 1].segments = NULL;

		/* set nnav */
		shared.shareddata.nnav--;

		/* no selection */
		shared.shareddata.navpick_type = MBV_PICK_NONE;
		shared.shareddata.nav_selected[0] = MBV_SELECT_NONE;
		shared.shareddata.nav_selected[1] = MBV_SELECT_NONE;
		shared.shareddata.nav_point_selected[0] = MBV_SELECT_NONE;
		shared.shareddata.nav_point_selected[1] = MBV_SELECT_NONE;
		shared.shareddata.nav_selected_mbnavadjust[0] = MBV_SELECT_NONE;
		shared.shareddata.nav_selected_mbnavadjust[1] = MBV_SELECT_NONE;
	}
	else {
		status = MB_FAILURE;
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getvectorcount(int verbose, size_t instance, int *nvector, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get number of vecs */
	*nvector = shared.shareddata.nvector;

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       nvector:                      %d\n", *nvector);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getvectorpointcount(int verbose, size_t instance, int vec, int *npoint, int *nintpoint, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       vec:                     %d\n", vec);
	}

	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	/* get number of points in specified vec */
	*npoint = 0;
	*nintpoint = 0;
	if (vec >= 0 && vec < shared.shareddata.nvector) {
		*npoint = shared.shareddata.vectors[vec].npoints;
		for (int i = 0; i < *npoint - 1; i++) {
			if (shared.shareddata.vectors[vec].segments[i].nls > 2)
				*nintpoint += shared.shareddata.vectors[vec].segments[i].nls - 2;
		}
	}

	const int status = MB_SUCCESS;

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       npoint:                    %d\n", *npoint);
		fprintf(stderr, "dbg2       nintpoint:                 %d\n", *nintpoint);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_allocvectorarrays(int verbose, int npointtotal, double **veclon, double **veclat, double **vecz, double **vecdata,
                             int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       npointtotal:               %d\n", npointtotal);
		fprintf(stderr, "dbg2       veclon:                    %p\n", *veclon);
		fprintf(stderr, "dbg2       veclat:                    %p\n", *veclat);
		fprintf(stderr, "dbg2       vecz:                      %p\n", *vecz);
		fprintf(stderr, "dbg2       vecdata:                   %p\n", *vecdata);
	}

	/* allocate the arrays using mb_reallocd */
	int status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)veclon, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)veclat, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)vecz, error);
	if (status == MB_SUCCESS)
		status = mb_reallocd(verbose, __FILE__, __LINE__, npointtotal * sizeof(double), (void **)vecdata, error);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       veclon:                    %p\n", *veclon);
		fprintf(stderr, "dbg2       veclat:                    %p\n", *veclat);
		fprintf(stderr, "dbg2       vecz:                      %p\n", *vecz);
		fprintf(stderr, "dbg2       vecdata:                   %p\n", *vecdata);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_freevectorarrays(int verbose, double **veclon, double **veclat, double **vecz, double **vecdata, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       veclon:                    %p\n", *veclon);
		fprintf(stderr, "dbg2       veclat:                    %p\n", *veclat);
		fprintf(stderr, "dbg2       vecz:                      %p\n", *vecz);
		fprintf(stderr, "dbg2       vecdata:                   %p\n", *vecdata);
	}

	/* free the arrays using mb_freed */
	int status = mb_freed(verbose, __FILE__, __LINE__, (void **)veclon, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)veclat, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)vecz, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)vecdata, error);

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       veclon:                    %p\n", *veclon);
		fprintf(stderr, "dbg2       veclat:                    %p\n", *veclat);
		fprintf(stderr, "dbg2       vecz:                      %p\n", *vecz);
		fprintf(stderr, "dbg2       vecdata:                   %p\n", *vecdata);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_addvector(int verbose, size_t instance, int npoint, double *veclon, double *veclat, double *vecz, double *vecdata,
                     int veccolor, int vecsize, mb_path vecname, double vecdatamin, double vecdatamax, int *error) {
	if (verbose >= 0) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
		fprintf(stderr, "dbg2       npoint:                    %d\n", npoint);
		for (int i = 0; i < npoint; i++) {
			fprintf(stderr, "dbg2       point:%d lon:%f lat:%f z:%f data:%f\n", i, veclon[i], veclat[i], vecz[i], vecdata[i]);
		}
		fprintf(stderr, "dbg2       veccolor:                  %d\n", veccolor);
		fprintf(stderr, "dbg2       vecsize:                   %d\n", vecsize);
		fprintf(stderr, "dbg2       vecname:                   %s\n", vecname);
		fprintf(stderr, "dbg2       vecdatamin:                %f\n", vecdatamin);
		fprintf(stderr, "dbg2       vecdatamax:                %f\n", vecdatamax);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* make sure no vec is selected */
	shared.shareddata.vector_selected = MBV_SELECT_NONE;
	shared.shareddata.vector_point_selected = MBV_SELECT_NONE;

	/* set vec id so that new vec is created */
	const int ivec = shared.shareddata.nvector;

	int status = MB_SUCCESS;

	/* allocate memory for a new vec if required */
	if (shared.shareddata.nvector_alloc < shared.shareddata.nvector + 1) {
		shared.shareddata.nvector_alloc = shared.shareddata.nvector + 1;
		status =
		    mb_reallocd(mbv_verbose, __FILE__, __LINE__, shared.shareddata.nvector_alloc * sizeof(struct mbview_vector_struct),
		                (void **)&(shared.shareddata.vectors), error);
		if (status == MB_FAILURE) {
			shared.shareddata.nvector_alloc = 0;
		} else {
			for (int i = shared.shareddata.nvector; i < shared.shareddata.nvector_alloc; i++) {
				shared.shareddata.vectors[i].active = false;
				shared.shareddata.vectors[i].color = MBV_COLOR_RED;
				shared.shareddata.vectors[i].size = 4;
				shared.shareddata.vectors[i].name[0] = '\0';
				shared.shareddata.vectors[i].npoints = 0;
				shared.shareddata.vectors[i].npoints_alloc = 0;
				shared.shareddata.vectors[i].nselected = 0;
				shared.shareddata.vectors[i].vectorpts = NULL;
				shared.shareddata.vectors[i].segments = NULL;
			}
		}
	}

	/* allocate memory to for vec arrays */
	if (shared.shareddata.vectors[ivec].npoints_alloc < npoint) {
		shared.shareddata.vectors[ivec].npoints_alloc = npoint;
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__,
		                     shared.shareddata.vectors[ivec].npoints_alloc * sizeof(struct mbview_vectorpointw_struct),
		                     (void **)&(shared.shareddata.vectors[ivec].vectorpts), error);
		status = mb_reallocd(mbv_verbose, __FILE__, __LINE__,
		                     shared.shareddata.vectors[ivec].npoints_alloc * sizeof(struct mbview_linesegmentw_struct),
		                     (void **)&(shared.shareddata.vectors[ivec].segments), error);
		for (int j = 0; j < shared.shareddata.vectors[ivec].npoints_alloc - 1; j++) {
			shared.shareddata.vectors[ivec].segments[j].nls = 0;
			shared.shareddata.vectors[ivec].segments[j].nls_alloc = 0;
			shared.shareddata.vectors[ivec].segments[j].lspoints = NULL;
		}
	}

	/* add the new vec */
	if (status == MB_SUCCESS) {
		/* set nvector */
		shared.shareddata.nvector++;

		/* set color size and name for new vec */
		shared.shareddata.vectors[ivec].active = true;
		shared.shareddata.vectors[ivec].color = veccolor;
		shared.shareddata.vectors[ivec].size = vecsize;
		strcpy(shared.shareddata.vectors[ivec].name, vecname);
		shared.shareddata.vectors[ivec].datamin = vecdatamin;
		shared.shareddata.vectors[ivec].datamax = vecdatamax;
		const bool recalculate_minmax = vecdatamin == vecdatamax;

		/* loop over the points in the new vec */
		shared.shareddata.vectors[ivec].npoints = npoint;
		for (int i = 0; i < npoint; i++) {
			/* set status values */
			shared.shareddata.vectors[ivec].vectorpts[i].selected = false;

			/* set data */
			shared.shareddata.vectors[ivec].vectorpts[i].data = vecdata[i];

			/* get min max of data if necessary */
			if (recalculate_minmax) {
				if (i == 0) {
					shared.shareddata.vectors[ivec].datamin = vecdata[i];
					shared.shareddata.vectors[ivec].datamax = vecdata[i];
				}
				else {
					shared.shareddata.vectors[ivec].datamin = MIN(vecdata[i], shared.shareddata.vectors[ivec].datamin);
					shared.shareddata.vectors[ivec].datamax = MAX(vecdata[i], shared.shareddata.vectors[ivec].datamax);
				}
			}

			/* ************************************************* */
			/* get vec positions in grid and display coordinates */
			shared.shareddata.vectors[ivec].vectorpts[i].point.xlon = veclon[i];
			shared.shareddata.vectors[ivec].vectorpts[i].point.ylat = veclat[i];
			shared.shareddata.vectors[ivec].vectorpts[i].point.zdata = vecz[i];
			status = mbview_projectfromlonlat(instance, shared.shareddata.vectors[ivec].vectorpts[i].point.xlon,
			                                  shared.shareddata.vectors[ivec].vectorpts[i].point.ylat,
			                                  shared.shareddata.vectors[ivec].vectorpts[i].point.zdata,
			                                  &(shared.shareddata.vectors[ivec].vectorpts[i].point.xgrid[instance]),
			                                  &(shared.shareddata.vectors[ivec].vectorpts[i].point.ygrid[instance]),
			                                  &(shared.shareddata.vectors[ivec].vectorpts[i].point.xdisplay[instance]),
			                                  &(shared.shareddata.vectors[ivec].vectorpts[i].point.ydisplay[instance]),
			                                  &(shared.shareddata.vectors[ivec].vectorpts[i].point.zdisplay[instance]));
			mbview_updatepointw(instance, &(shared.shareddata.vectors[ivec].vectorpts[i].point));

			/*fprintf(stderr,"Depth: llz:%f %f %f   grid:%f %f   dpy:%f %f %f\n",
			shared.shareddata.vectors[ivec].vectorpts[i].point.xlon,
			shared.shareddata.vectors[ivec].vectorpts[i].point.ylat,
			shared.shareddata.vectors[ivec].vectorpts[i].point.zdata,
			shared.shareddata.vectors[ivec].vectorpts[i].point.xgrid[instance],
			shared.shareddata.vectors[ivec].vectorpts[i].point.ygrid[instance],
			shared.shareddata.vectors[ivec].vectorpts[i].point.xdisplay[instance],
			shared.shareddata.vectors[ivec].vectorpts[i].point.ydisplay[instance],
			shared.shareddata.vectors[ivec].vectorpts[i].point.zdisplay[instance]);*/

			/* ************************************************* */
		}

		/* set segment endpoints now that all vector points are populated */
		for (int j = 0; j < shared.shareddata.vectors[ivec].npoints - 1; j++) {
			shared.shareddata.vectors[ivec].segments[j].endpoints[0] = shared.shareddata.vectors[ivec].vectorpts[j].point;
			shared.shareddata.vectors[ivec].segments[j].endpoints[1] = shared.shareddata.vectors[ivec].vectorpts[j + 1].point;
		}

		/* make vecs viewable */
		data->vector_view_mode = MBV_VIEW_ON;

		/* some info to terminal */
		fprintf(stderr, "Added %d point vector with data bounds: min:%f max:%f\n", shared.shareddata.vectors[ivec].npoints,
		        shared.shareddata.vectors[ivec].datamin, shared.shareddata.vectors[ivec].datamax);
	}

	/* print vec debug statements */
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  vec data altered in function <%s>\n", __func__);
		fprintf(stderr, "dbg2  vec values:\n");
		fprintf(stderr, "dbg2       vector_mode:        %d\n", shared.shareddata.vector_mode);
		fprintf(stderr, "dbg2       vector_view_mode:      %d\n", data->vector_view_mode);
		fprintf(stderr, "dbg2       nvector:               %d\n", shared.shareddata.nvector);
		fprintf(stderr, "dbg2       nvector_alloc:         %d\n", shared.shareddata.nvector_alloc);
		fprintf(stderr, "dbg2       vector_selected:       %d\n", shared.shareddata.vector_selected);
		fprintf(stderr, "dbg2       vector_point_selected: %d\n", shared.shareddata.vector_point_selected);
		for (int i = 0; i < shared.shareddata.nvector; i++) {
			fprintf(stderr, "dbg2       vec %d active:        %d\n", i, shared.shareddata.vectors[i].active);
			fprintf(stderr, "dbg2       vec %d color:         %d\n", i, shared.shareddata.vectors[i].color);
			fprintf(stderr, "dbg2       vec %d size:          %d\n", i, shared.shareddata.vectors[i].size);
			fprintf(stderr, "dbg2       vec %d name:          %s\n", i, shared.shareddata.vectors[i].name);
			fprintf(stderr, "dbg2       vec %d npoints:       %d\n", i, shared.shareddata.vectors[i].npoints);
			fprintf(stderr, "dbg2       vec %d npoints_alloc: %d\n", i, shared.shareddata.vectors[i].npoints_alloc);
			fprintf(stderr, "dbg2       vec %d nselected:     %d\n", i, shared.shareddata.vectors[i].nselected);
			for (int j = 0; j < shared.shareddata.vectors[i].npoints; j++) {
				fprintf(stderr, "dbg2       vec %d %d selected: %d\n", i, j, shared.shareddata.vectors[i].vectorpts[j].selected);
				fprintf(stderr, "dbg2       vec %d %d data:     %f\n", i, j, shared.shareddata.vectors[i].vectorpts[j].data);

				fprintf(stderr, "dbg2       vec %d %d xgrid:    %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.xgrid[instance]);
				fprintf(stderr, "dbg2       vec %d %d ygrid:    %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.ygrid[instance]);
				fprintf(stderr, "dbg2       vec %d %d xlon:     %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.xlon);
				fprintf(stderr, "dbg2       vec %d %d ylat:     %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.ylat);
				fprintf(stderr, "dbg2       vec %d %d zdata:    %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.zdata);
				fprintf(stderr, "dbg2       vec %d %d xdisplay: %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.xdisplay[instance]);
				fprintf(stderr, "dbg2       vec %d %d ydisplay: %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.ydisplay[instance]);
				fprintf(stderr, "dbg2       vec %d %d zdisplay: %f\n", i, j,
				        shared.shareddata.vectors[i].vectorpts[j].point.zdisplay[instance]);
			}
			for (int j = 0; j < shared.shareddata.vectors[i].npoints - 1; j++) {
				fprintf(stderr, "dbg2       vec %d %d nls:          %d\n", i, j, shared.shareddata.vectors[i].segments[j].nls);
				fprintf(stderr, "dbg2       vec %d %d nls_alloc:    %d\n", i, j,
				        shared.shareddata.vectors[i].segments[j].nls_alloc);
				fprintf(stderr, "dbg2       vec %d %d endpoints[0]: %p\n", i, j,
				        &shared.shareddata.vectors[i].segments[j].endpoints[0]);
				fprintf(stderr, "dbg2       vec %d %d endpoints[1]: %p\n", i, j,
				        &shared.shareddata.vectors[i].segments[j].endpoints[1]);
			}
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_vector_delete(size_t instance, int ivec) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       ivec:            %d\n", ivec);
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}

	// struct mbview_world_struct *view = &(mbviews[instance]);
	// struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* delete vec if its the same as previously selected */
	if (ivec >= 0 && ivec < shared.shareddata.nvector) {
		/* free memory for deleted vec, including each segment's own drape
		    buffer - not currently populated for vectors, but freed here
		    defensively to match mbview_nav_delete()/mbview_deleteallroutes()
		    in case vector draping is ever wired up */
		int error = MB_ERROR_NO_ERROR;
		for (int j = 0; j < shared.shareddata.vectors[ivec].npoints_alloc; j++) {
			struct mbview_linesegmentw_struct *segment = &shared.shareddata.vectors[ivec].segments[j];
			if (segment->nls_alloc > 0 && segment->lspoints != NULL) {
				mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(segment->lspoints), &error);
				segment->nls = 0;
				segment->nls_alloc = 0;
			}
		}
		mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.vectors[ivec].vectorpts), &error);
		mb_freed(mbv_verbose, __FILE__, __LINE__, (void **)&(shared.shareddata.vectors[ivec].segments), &error);

		/* move vec data if necessary */
		for (int i = ivec; i < shared.shareddata.nvector - 1; i++) {
			shared.shareddata.vectors[i] = shared.shareddata.vectors[i + 1];
		}

		/* rest last vec */
		shared.shareddata.vectors[shared.shareddata.nvector - 1].active = false;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].color = MBV_COLOR_RED;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].size = 4;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].name[0] = '\0';
		shared.shareddata.vectors[shared.shareddata.nvector - 1].format = 0;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].npoints = 0;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].npoints_alloc = 0;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].nselected = 0;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].datamin = 0.0;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].datamax = 0.0;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].vectorpts = NULL;
		shared.shareddata.vectors[shared.shareddata.nvector - 1].segments = NULL;

		/* set nvector */
		shared.shareddata.nvector--;

		/* no selection */
		shared.shareddata.vector_selected = MBV_SELECT_NONE;
	} else {
		status = MB_FAILURE;
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getprofilecount(int verbose, size_t instance, int *npoints, int *error)

{
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_world_struct *view;
	struct mbview_struct *data;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	/* get number of profiles */
	*npoints = data->profile.npoints;

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       npoints:                   %d\n", *npoints);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_allocprofilepoints(int verbose, int npoints, struct mbview_profilepoint_struct **points, int *error)

{
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       npoints:                   %d\n", npoints);
		fprintf(stderr, "dbg2       points:                    %p\n", *points);
	}

	/* allocate the arrays using mb_reallocd */
	status =
	    mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(struct mbview_profilepoint_struct), (void **)points, error);

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       points:                    %p\n", *points);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_freeprofilepoints(int verbose, double **points, int *error)

{
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       points:                    %p\n", *points);
	}

	/* free the arrays using mb_freed */
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)points, error);

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       points:                    %p\n", *points);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_allocprofilearrays(int verbose, int npoints, double **distance, double **zdata, int **boundary, double **xlon,
                              double **ylat, double **distovertopo, double **bearing, double **slope, int *error)

{
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       npoints:                   %d\n", npoints);
		fprintf(stderr, "dbg2       distance:                  %p\n", *distance);
		fprintf(stderr, "dbg2       zdata:                     %p\n", *zdata);
		fprintf(stderr, "dbg2       boundary:                  %p\n", *boundary);
		fprintf(stderr, "dbg2       xlon:                      %p\n", *xlon);
		fprintf(stderr, "dbg2       ylat:                      %p\n", *ylat);
		fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		fprintf(stderr, "dbg2       bearing:                   %p\n", *bearing);
		fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
	}

	/* allocate the arrays using mb_reallocd */
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)distance, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)zdata, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)boundary, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)xlon, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)ylat, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)distovertopo, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)bearing, error);
	status = mb_reallocd(verbose, __FILE__, __LINE__, npoints * sizeof(double), (void **)slope, error);

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       distance:                  %p\n", *distance);
		fprintf(stderr, "dbg2       zdata:                     %p\n", *zdata);
		fprintf(stderr, "dbg2       boundary:                  %p\n", *boundary);
		fprintf(stderr, "dbg2       xlon:                      %p\n", *xlon);
		fprintf(stderr, "dbg2       ylat:                      %p\n", *ylat);
		fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		fprintf(stderr, "dbg2       bearing:                   %p\n", *bearing);
		fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_freeprofilearrays(int verbose, double **distance, double **zdata, int **boundary, double **xlon, double **ylat,
                             double **distovertopo, double **bearing, double **slope, int *error)

{
	/* local variables */
	int status = MB_SUCCESS;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       distance:                  %p\n", *distance);
		fprintf(stderr, "dbg2       zdata:                     %p\n", *zdata);
		fprintf(stderr, "dbg2       boundary:                  %p\n", *boundary);
		fprintf(stderr, "dbg2       xlon:                      %p\n", *xlon);
		fprintf(stderr, "dbg2       ylat:                      %p\n", *ylat);
		fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		fprintf(stderr, "dbg2       bearing:                   %p\n", *bearing);
		fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
	}

	/* free the arrays using mb_freed */
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)distance, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)zdata, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)boundary, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)xlon, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)ylat, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)distovertopo, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)bearing, error);
	status = mb_freed(verbose, __FILE__, __LINE__, (void **)slope, error);

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       distance:                  %p\n", *distance);
		fprintf(stderr, "dbg2       zdata:                     %p\n", *zdata);
		fprintf(stderr, "dbg2       boundary:                  %p\n", *boundary);
		fprintf(stderr, "dbg2       xlon:                      %p\n", *xlon);
		fprintf(stderr, "dbg2       ylat:                      %p\n", *ylat);
		fprintf(stderr, "dbg2       distovertopo:              %p\n", *distovertopo);
		fprintf(stderr, "dbg2       bearing:                   %p\n", *bearing);
		fprintf(stderr, "dbg2       slope:                     %p\n", *slope);
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_getprofile(int verbose, size_t instance, mb_path source_name, double *length, double *zmin, double *zmax, int *npoints,
                      double *distance, double *zdata, int *boundary, double *xlon, double *ylat, double *distovertopo,
                      double *bearing, double *slope, int *error)

{
	/* local variables */
	int status = MB_SUCCESS;
	struct mbview_world_struct *view;
	struct mbview_struct *data;
	int i;

	/* print starting debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:                   %d\n", verbose);
		fprintf(stderr, "dbg2       instance:                  %zu\n", instance);
	}

	/* get view */
	view = &(mbviews[instance]);
	data = &(view->data);

	/* check that the array pointers are not NULL */
	if (distance == NULL || zdata == NULL || boundary == NULL || xlon == NULL || ylat == NULL || distovertopo == NULL ||
	    slope == NULL || bearing == NULL) {
		status = MB_FAILURE;
		*error = MB_ERROR_DATA_NOT_INSERTED;
	}

	/* otherwise go get the profile data */
	else {
		/* loop over the profiles */
		strcpy(source_name, data->profile.source_name);
		*length = data->profile.length;
		*zmin = data->profile.zmin;
		*zmax = data->profile.npoints;
		*npoints = data->profile.npoints;
		for (i = 0; i < data->profile.npoints; i++) {
			distance[i] = data->profile.points[i].distance;
			zdata[i] = data->profile.points[i].zdata;
			boundary[i] = data->profile.points[i].boundary;
			xlon[i] = data->profile.points[i].xlon;
			ylat[i] = data->profile.points[i].ylat;
			distovertopo[i] = data->profile.points[i].distovertopo;
			bearing[i] = data->profile.points[i].bearing;
			slope[i] = data->profile.points[i].slope;
		}
	}

	/* print output debug statements */
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       source_name:                %s\n", source_name);
		fprintf(stderr, "dbg2       length:                     %f\n", *length);
		fprintf(stderr, "dbg2       zmin:                       %f\n", *zmin);
		fprintf(stderr, "dbg2       zmax:                       %f\n", *zmax);
		fprintf(stderr, "dbg2       npoints:                    %d\n", *npoints);
		for (i = 0; i < *npoints; i++) {
			fprintf(stderr,
			        "dbg2       %d distance:%f zdata:%f boundary:%d xlon:%f ylat:%f distovertopo:%f bearing:%f slope:%f\n", i,
			        distance[i], zdata[i], boundary[i], xlon[i], ylat[i], distovertopo[i], bearing[i], slope[i]);
		}
		fprintf(stderr, "dbg2       error:                     %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:                    %d\n", status);
	}

	/* return */
	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_extract_pick_profile(size_t instance) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}

	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	int status = MB_SUCCESS;

	/* if a two point pick has been made and the profile display
	    is on or the pick is final, insert the draped
	    segment into the profile data */
	if (data->pick_type == MBV_PICK_TWOPOINT) {
		data->profile.source = MBV_PROFILE_TWOPOINT;
		strcpy(data->profile.source_name, "Two point pick");
		data->profile.length = data->pick.range;
		const int npoints = MAX(2, data->pick.segment.nls);
		if (data->profile.npoints_alloc < npoints) {
			int error = MB_ERROR_NO_ERROR;
			status = mbview_allocprofilepoints(mbv_verbose, npoints, &(data->profile.points), &error);
			if (status == MB_SUCCESS) {
				data->profile.npoints_alloc = npoints;
			}
			else {
				data->profile.npoints_alloc = 0;
			}
		}
		if (npoints > 2 && data->profile.npoints_alloc >= npoints) {
			/* get the profile data */
			for (int i = 0; i < npoints; i++) {
				data->profile.points[i].boundary = false;
				data->profile.points[i].xgrid = data->pick.segment.lspoints[i].xgrid;
				data->profile.points[i].ygrid = data->pick.segment.lspoints[i].ygrid;
				data->profile.points[i].xlon = data->pick.segment.lspoints[i].xlon;
				data->profile.points[i].ylat = data->pick.segment.lspoints[i].ylat;
				data->profile.points[i].zdata = data->pick.segment.lspoints[i].zdata;
				data->profile.points[i].xdisplay = data->pick.segment.lspoints[i].xdisplay;
				data->profile.points[i].ydisplay = data->pick.segment.lspoints[i].ydisplay;
				if (i == 0) {
					data->profile.zmin = data->profile.points[i].zdata;
					data->profile.zmax = data->profile.points[i].zdata;
					data->profile.points[i].distance = 0.0;
					data->profile.points[i].distovertopo = 0.0;
				}
				else {
					data->profile.zmin = MIN(data->profile.zmin, data->profile.points[i].zdata);
					data->profile.zmax = MAX(data->profile.zmax, data->profile.points[i].zdata);
					if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
						const double dx = data->profile.points[i].xdisplay - data->profile.points[i - 1].xdisplay;
						const double dy = data->profile.points[i].ydisplay - data->profile.points[i - 1].ydisplay;
						data->profile.points[i].distance =
						    sqrt(dx * dx + dy * dy) / view->scale + data->profile.points[i - 1].distance;
					}
					else {
						mbview_greatcircle_dist(instance, data->profile.points[0].xlon, data->profile.points[0].ylat,
						                        data->profile.points[i].xlon, data->profile.points[i].ylat,
						                        &(data->profile.points[i].distance));
					}
					const double dy = (data->profile.points[i].zdata - data->profile.points[i - 1].zdata);
					const double dx = (data->profile.points[i].distance - data->profile.points[i - 1].distance);
					data->profile.points[i].distovertopo = data->profile.points[i - 1].distovertopo + sqrt(dy * dy + dx * dx);
					if (dx > 0.0)
						data->profile.points[i].slope = fabs(dy / dx);
					else
						data->profile.points[i].slope = 0.0;
				}
				data->profile.points[i].bearing = data->pick.bearing;
				if (i > 1) {
					const double dy = (data->profile.points[i].zdata - data->profile.points[i - 2].zdata);
					const double dx = (data->profile.points[i].distance - data->profile.points[i - 2].distance);
					if (dx > 0.0)
						data->profile.points[i - 1].slope = fabs(dy / dx);
					else
						data->profile.points[i - 1].slope = 0.0;
				}
				data->profile.points[i].navzdata = 0.0;
				data->profile.points[i].navtime_d = 0.0;
			}
			data->profile.points[0].boundary = true;
			data->profile.points[npoints - 1].boundary = true;
			data->profile.npoints = npoints;
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:          %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_zscalepoint(size_t instance, int globalview, double offset_factor, struct mbview_point_struct *point) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       globalview:       %d\n", globalview);
		fprintf(stderr, "dbg2       offset_factor:    %f\n", offset_factor);
	}
	if (mbv_verbose >= 2)
		fprintf(stderr, "mbview_zscalepoint: %zu\n", instance);

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* scale z value */
	if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
		/* scale z value alone */
		point->zdisplay = view->scale * (data->exaggeration * point->zdata - view->zorigin) + offset_factor;
	}
	else {
		/* reproject positions into display coordinates */
		mbview_projectforward(instance, false, point->xgrid, point->ygrid, point->zdata, &point->xlon, &point->ylat,
		                      &point->xdisplay, &point->ydisplay, &point->zdisplay);

		if (!globalview) {
			point->zdisplay += offset_factor;
		}
		else {
			point->xdisplay += point->xdisplay * offset_factor;
			point->ydisplay += point->ydisplay * offset_factor;
			point->zdisplay += point->zdisplay * offset_factor;
		}
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_zscalepointw(size_t instance, int globalview, double offset_factor, struct mbview_pointw_struct *pointw) {
	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
		fprintf(stderr, "dbg2       globalview:       %d\n", globalview);
		fprintf(stderr, "dbg2       offset_factor:    %f\n", offset_factor);
	}
	if (mbv_verbose >= 2)
		fprintf(stderr, "mbview_zscalepointw: %zu\n", instance);

	/* get view */
	struct mbview_world_struct *view = &(mbviews[instance]);
	struct mbview_struct *data = &(view->data);

	/* scale z value */
	if (data->display_projection_mode != MBV_PROJECTION_SPHEROID) {
		/* scale z value alone */
		pointw->zdisplay[instance] = view->scale * (data->exaggeration * pointw->zdata - view->zorigin) + offset_factor;
	}
	else {
		/* reproject positions into display coordinates */
		mbview_projectforward(instance, false, pointw->xgrid[instance], pointw->ygrid[instance], pointw->zdata, &(pointw->xlon),
		                      &(pointw->ylat), &(pointw->xdisplay[instance]), &(pointw->ydisplay[instance]),
		                      &(pointw->zdisplay[instance]));

		if (!globalview) {
			pointw->zdisplay[instance] += offset_factor;
		}
		else {
			pointw->xdisplay[instance] += pointw->xdisplay[instance] * offset_factor;
			pointw->ydisplay[instance] += pointw->ydisplay[instance] * offset_factor;
			pointw->zdisplay[instance] += pointw->zdisplay[instance] * offset_factor;
		}
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_updatepointw(size_t instance, struct mbview_pointw_struct *pointw) {
	size_t i;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}
	if (mbv_verbose >= 2)
		fprintf(stderr, "mbview_updatepointw: %zu\n", instance);

	int status = MB_SUCCESS;

	/* update grid and display coordinates for pointw for all
	    active instances other than instance, which has
	    already been set */
	for (i = 0; i < MBV_MAX_WINDOWS; i++) {
		struct mbview_world_struct *view = &(mbviews[i]);
		if (i != instance && view->init != MBV_WINDOW_NULL) {
                  // struct mbview_struct *data = &(view->data);

			/* get positions in grid coordinates */
			status = mbview_projectll2xygrid(i, pointw->xlon, pointw->ylat, &(pointw->xgrid[i]), &(pointw->ygrid[i]));

			/* get positions in display coordinates */
			status = mbview_projectll2display(i, pointw->xlon, pointw->ylat, pointw->zdata, &(pointw->xdisplay[i]),
			                                  &(pointw->ydisplay[i]), &(pointw->zdisplay[i]));
		}
	}

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
int mbview_updatesegmentw(size_t instance, struct mbview_linesegmentw_struct *segmentw) {
	int i;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  MB-system Version %s\n", MB_VERSION);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       instance:         %zu\n", instance);
	}
	if (mbv_verbose >= 2)
		fprintf(stderr, "mbview_updatesegmentw: %zu\n", instance);

	/* update grid and display coordinates for segmentw for all
	    active instances other than instance, which has
	    already been set */
	for (i = 0; i < segmentw->nls; i++) {
		mbview_updatepointw(instance, &(segmentw->lspoints[i]));
	}

	const int status = MB_SUCCESS;

	if (mbv_verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:  %d\n", status);
	}

	return (status);
}
/*------------------------------------------------------------------------------*/
