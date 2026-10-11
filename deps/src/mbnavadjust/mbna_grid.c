/*======================================================================================
 * mbna_grid.c -- MB-System's mb_check_gmt_grd and mb_read_gmt_grd (mbaux/mb_readwritegrd.c, 5.8.3)
 * for the mbnavadjust port: how a grid's header becomes MB-System's projection, no-data value and
 * internal column-major, south-first array, and its derivatives, kept as upstream has them.
 *
 * The one thing that is not here is GMT itself: upstream opens a GMT session and GMT_Read_Data's the
 * file; the port asks the host for the same header and values through mbna_set_grid_reader
 * (mbnavadjust_window.cpp hands it the viewer's reader). What upstream wrote twice -- the header to
 * projection lines, in each of the two functions -- is one function here.
 *====================================================================================*/

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mbnavadjust_mbio.h"
#include "mbna_grid.h"

/* mb_define.h */
#define MB_DEFAULT_GRID_NODATA -9999999.9
#define MB_PROJECTION_GEOGRAPHIC 0
#define MB_PROJECTION_PROJECTED 1
#define MB_IS_FNAN isnan

enum ModelType { ModelTypeProjected = 1, ModelTypeGeographic = 2 };
static const int GCS_WGS_84 = 4326;

static mbna_grid_reader_fn mbna_grid_reader = NULL;

void mbna_set_grid_reader(mbna_grid_reader_fn fn) {
	mbna_grid_reader = fn;
}

/* the file exists, is not a folder, is not empty */
static int mbna_grid_exists(const char *grdfile, int *error) {
	struct stat file_status;
	if (stat(grdfile, &file_status) == 0 && (file_status.st_mode & S_IFMT) != S_IFDIR && file_status.st_size > 0) {
		*error = MB_ERROR_NO_ERROR;
		return MB_SUCCESS;
	}
	*error = MB_ERROR_OPEN_FAIL;
	return MB_FAILURE;
}

/* the header lines of both functions: the projection from the grd file remark, the no-data value
   and the dimensions */
static void mbna_grid_header(const struct mbna_gmtgrid *header, int *grid_projection_mode, char *grid_projection_id,
                             float *nodatavalue, int *nxy, int *n_columns, int *n_rows, double *min, double *max,
                             double *xmin, double *xmax, double *ymin, double *ymax, double *dx, double *dy) {
	mb_path projectionname = "";
	int epsgid;
	enum ModelType modeltype;

	/* try to get projection from the grd file remark */
	if (strncmp(&(header->remark[2]), "Projection: ", 12) == 0) {
		int nscan;
		int utmzone;
		char NorS;
		double lon_origin;
		double lat_origin;
		if ((nscan = sscanf(&(header->remark[2]), "Projection: UTM%d%c", &utmzone, &NorS)) == 2) {
			if (NorS == 'N') {
				epsgid = 32600 + utmzone;
			}
			else if (NorS == 'S') {
				epsgid = 32700 + utmzone;
			}
			else {
				epsgid = 32600 + utmzone;
			}
			modeltype = ModelTypeProjected;
			snprintf(projectionname, sizeof(projectionname), "UTM%2.2d%c", utmzone, NorS);
			*grid_projection_mode = MB_PROJECTION_PROJECTED;
			sprintf(grid_projection_id, "EPSG:%d", epsgid);
		}
		else if ((nscan = sscanf(&(header->remark[2]), "Projection: LTM%lf/%lf", &lon_origin, &lat_origin)) == 2) {
			modeltype = ModelTypeProjected;
			snprintf(projectionname, sizeof(projectionname), "LTM%.9f/%.9f", lon_origin, lat_origin);
			*grid_projection_mode = MB_PROJECTION_PROJECTED;
			sprintf(grid_projection_id, "+proj=tmerc +lon_0=%.9f +lat_0=%.9f +ellps=WGS84", lon_origin, lat_origin);
		}
		else if ((nscan = sscanf(&(header->remark[2]), "Projection: EPSG:%d", &epsgid)) == 1) {
			snprintf(projectionname, sizeof(projectionname), "EPSG:%d", epsgid);
			modeltype = ModelTypeProjected;
			*grid_projection_mode = MB_PROJECTION_PROJECTED;
			sprintf(grid_projection_id, "EPSG:%d", epsgid);
		}
		else {
			strncpy(projectionname, "Geographic WGS84", sizeof(projectionname));
			modeltype = ModelTypeGeographic;
			epsgid = GCS_WGS_84;
			*grid_projection_mode = MB_PROJECTION_GEOGRAPHIC;
			sprintf(grid_projection_id, "EPSG:%d", epsgid);
		}
	}
	else {
		strncpy(projectionname, "Geographic WGS84", sizeof(projectionname));
		modeltype = ModelTypeGeographic;
		epsgid = GCS_WGS_84;
		*grid_projection_mode = MB_PROJECTION_GEOGRAPHIC;
		sprintf(grid_projection_id, "EPSG:%d", epsgid);
	}
	(void)modeltype;

	/* set up internal arrays */
	*nodatavalue = MIN(MB_DEFAULT_GRID_NODATA, header->z_min - 10 * (header->z_max - header->z_min));
	*nxy = header->n_columns * header->n_rows;
	*n_columns = header->n_columns;
	*n_rows = header->n_rows;
	*xmin = header->wesn[0];
	*xmax = header->wesn[1];
	*ymin = header->wesn[2];
	*ymax = header->wesn[3];
	*dx = header->inc[0];
	*dy = header->inc[1];
	*min = header->z_min;
	*max = header->z_max;
}

int mbna_check_gmt_grd(int verbose, char *grdfile, int *grid_projection_mode, char *grid_projection_id,
                       float *nodatavalue, int *nxy, int *n_columns, int *n_rows, double *min, double *max, double *xmin,
                       double *xmax, double *ymin, double *ymax, double *dx, double *dy, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBBA function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:         %d\n", verbose);
		fprintf(stderr, "dbg2       grdfile:         %s\n", grdfile);
	}

	/* check if the file exists and is readable */
	int status = mbna_grid_exists(grdfile, error);

	/* if file exists proceed: the header only (GMT_CONTAINER_ONLY upstream) */
	if (status == MB_SUCCESS) {
		struct mbna_gmtgrid header;
		memset(&header, 0, sizeof(header));
		if (mbna_grid_reader == NULL || !mbna_grid_reader(grdfile, &header, NULL)) {
			fprintf(stderr, "\nUnable to read GMT grid file %s in function %s\n", grdfile, __func__);
			*error = MB_ERROR_OPEN_FAIL;
			status = MB_FAILURE;   /* port: upstream exits the program */
		}
		else {
			mbna_grid_header(&header, grid_projection_mode, grid_projection_id, nodatavalue, nxy, n_columns, n_rows, min,
			                 max, xmin, xmax, ymin, ymax, dx, dy);
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBBA function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}

int mbna_read_gmt_grd(int verbose, char *grdfile, int *grid_projection_mode, char *grid_projection_id,
                      float *nodatavalue, int *nxy, int *n_columns, int *n_rows, double *min, double *max, double *xmin,
                      double *xmax, double *ymin, double *ymax, double *dx, double *dy, float **data, float **data_dzdx,
                      float **data_dzdy, int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBBA function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:         %d\n", verbose);
		fprintf(stderr, "dbg2       grdfile:         %s\n", grdfile);
	}

	/* check if the file exists and is readable */
	int status = mbna_grid_exists(grdfile, error);

	if (status == MB_SUCCESS) {
		struct mbna_gmtgrid header;
		memset(&header, 0, sizeof(header));
		if (mbna_grid_reader == NULL || !mbna_grid_reader(grdfile, &header, NULL)) {
			fprintf(stderr, "\nUnable to read GMT grid file %s in function %s\n", grdfile, __func__);
			*error = MB_ERROR_OPEN_FAIL;
			return MB_FAILURE;   /* port: upstream exits the program */
		}
		mbna_grid_header(&header, grid_projection_mode, grid_projection_id, nodatavalue, nxy, n_columns, n_rows, min, max,
		                 xmin, xmax, ymin, ymax, dx, dy);

		/* the values, as GMT_Read_Data with no pad hands them: row-major, north row first */
		float *gdata = (float *)malloc(sizeof(float) * (size_t)(*nxy));
		if (gdata == NULL || !mbna_grid_reader(grdfile, &header, gdata)) {
			free(gdata);
			fprintf(stderr, "\nUnable to read GMT grid file %s in function %s\n", grdfile, __func__);
			*error = MB_ERROR_OPEN_FAIL;
			return MB_FAILURE;
		}

		float *usedata;
		status = mb_mallocd(verbose, __FILE__, __LINE__, sizeof(float) * (*nxy), (void **)&usedata, error);
		if (status == MB_SUCCESS) {
			*data = usedata;
		}
		if (status == MB_SUCCESS && data_dzdx != NULL) {
			status = mb_mallocd(verbose, __FILE__, __LINE__, sizeof(float) * (*nxy), (void **)&usedata, error);
			*data_dzdx = usedata;
		}
		if (status == MB_SUCCESS && data_dzdy != NULL) {
			status = mb_mallocd(verbose, __FILE__, __LINE__, sizeof(float) * (*nxy), (void **)&usedata, error);
			*data_dzdy = usedata;
		}

		/* copy grid data, reordering to internal convention (no pad: header->pad[] are all zero) */
		if (status == MB_SUCCESS) {
			for (int i = 0; i < *n_columns; i++) {
				for (int j = 0; j < *n_rows; j++) {
					const int k = i * *n_rows + j;
					const int kk = (*n_rows - 1 - j) * (*n_columns) + i;
					if (MB_IS_FNAN(gdata[kk]))
						(*data)[k] = *nodatavalue;
					else
						(*data)[k] = gdata[kk];
				}
			}
		}
		free(gdata);

		/* calculate derivatives */
		if (status == MB_SUCCESS && data_dzdx != NULL && data_dzdy != NULL) {
			double ddx = *dx;
			double ddy = *dy;
			if (*grid_projection_mode == MB_PROJECTION_GEOGRAPHIC) {
				double mtodeglon;
				double mtodeglat;
				mb_coor_scale(verbose, 0.5 * (*ymin + *ymax), &mtodeglon, &mtodeglat);
				ddx /= mtodeglon;
				ddy /= mtodeglon;
			}
			for (int i = 0; i < *n_columns; i++)
				for (int j = 0; j < *n_rows; j++) {
					const int k = i * (*n_rows) + j;
					int ii = 0;
					int kx0;
					if (i > 0) {
						kx0 = (i - 1) * (*n_rows) + j;
						ii++;
					}
					else {
						kx0 = k;
					}
					int kx2 = 0;
					if (i < *n_columns - 1) {
						kx2 = (i + 1) * (*n_rows) + j;
						ii++;
					}
					else {
						kx2 = k;
					}
					int jj = 0;
					int ky0;
					if (j > 0) {
						ky0 = i * (*n_rows) + j + 1;
						jj++;
					}
					else {
						ky0 = k;
					}
					int ky2;
					if (j < *n_rows - 1) {
						ky2 = i * (*n_rows) + j - 1;
						jj++;
					}
					else {
						ky2 = k;
					}
					if (ii > 0)
						(*data_dzdx)[k] = ((*data)[kx2] - (*data)[kx0]) / (((double)ii) * ddx);
					if (jj > 0)
						(*data_dzdy)[k] = ((*data)[ky2] - (*data)[ky0]) / (((double)jj) * ddy);
				}
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBBA function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:      %d\n", status);
	}

	return (status);
}
