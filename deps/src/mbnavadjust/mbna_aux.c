/*======================================================================================
 * mbna_aux.c -- the pieces of MB-System's libmbaux the mbnavadjust port uses, carried in
 * because the MBIO library the viewer loads at run time does not hold them:
 *   - mb_truecont.c: the swath contourer (mb_contour_init / mb_contour / mb_contour_deall,
 *     mb_triangulate) that draws the naverr window's two sections;
 *   - mb_delaun.c: the Delaunay triangulator mb_triangulate calls;
 *   - mb_cheb.c, BCLS part: the CBLAS subset and LSQR (mblsqr_lsqr) the navigation inversion
 *     solves with.
 * The code is MB-System 5.8.3's, unchanged, with these differences only:
 *   - the exported names are mbna_-prefixed (mbnavadjust_mbio.h #defines them), so they can
 *     never bind to a real libmbaux in the same process, nor to another ported tool's copy;
 *   - every helper upstream exports but mbnavadjust never calls (get_*_tri, mb_tcontour,
 *     mb_ocontour, mbcblas_*, ...) is static, for the same reason.
 *====================================================================================*/

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbnavadjust_mbio.h"

/* mb_aux.h, for the BCLS part */
#define ZERO 0.0
#define ONE 1.0
#define MBCBLAS_OFFSET(N, incX) ((incX) > 0 ? 0 : ((N)-1) * (-(incX)))

/*--------------------------------------------------------------------
 *    The MB-system:  mb_truecont.c  4/21/94
 *
 *    Copyright (c) 1994-2025 by
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
 * mb_truecontour.c contours a block of multibeam bathymetry data,
 * dealing correctly with beams in arbitrary locations by forming
 * a delauney triangle network and then contouring that network.
 *
 * Author:  D. W. Caress
 * Date:  April, 1994
 */


#define IMOVE 3
#define IDRAW 2
#define ISTROKE -2
#define IOR -3
const double EPS = 0.0001;
#define NUM_BEAMS_ALLOC_MIN 16

/*--------------------------------------------------------------------------*/
/*   function mb_contour_init initializes the memory required to
    contour multibeam bathymetry data.
    if mbio_ptr is null, the arrays are allocated using mb_mallocd. If
    mbio_ptr is a valid mbio structure, then the arrays tied to
    beams_bath will be registered using mb_register_array */
int mb_contour_init(int verbose, struct swath **data, int npings_max, int beams_bath, int contour_algorithm, int plot_contours,
                    int plot_triangles, int plot_track, int plot_name, int plot_pingnumber, double contour_int, double color_int,
                    double tick_int, double label_int, double tick_len, double label_hgt, double label_spacing, int ncolor,
                    int nlevel, double *level_list, int *label_list, int *tick_list, double time_tick_int, double time_annot_int,
                    double date_annot_int, double time_tick_len, double name_hgt, int pingnumber_tick_int,
                    int pingnumber_annot_int, double pingnumber_tick_len, void (*contour_plot)(double, double, int),
                    void (*contour_newpen)(int), void (*contour_setline)(int),
                    void (*contour_justify_string)(double, char *, double *),
                    void (*contour_plot_string)(double, double, double, double, char *), int *error) {
  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       verbose:              %d\n", verbose);
    fprintf(stderr, "dbg2       data:                 %p\n", data);
    fprintf(stderr, "dbg2       npings_max:           %d\n", npings_max);
    fprintf(stderr, "dbg2       beams_bath:           %d\n", beams_bath);
    fprintf(stderr, "dbg2       contour algorithm:    %d\n", contour_algorithm);
    fprintf(stderr, "dbg2       plot contours:        %d\n", plot_contours);
    fprintf(stderr, "dbg2       plot triangles:       %d\n", plot_triangles);
    fprintf(stderr, "dbg2       plot track:           %d\n", plot_track);
    fprintf(stderr, "dbg2       plot name:            %d\n", plot_name);
    fprintf(stderr, "dbg2       plot pingnumber:      %d\n", plot_pingnumber);
    fprintf(stderr, "dbg2       contour interval:     %f\n", contour_int);
    fprintf(stderr, "dbg2       color interval:       %f\n", color_int);
    fprintf(stderr, "dbg2       tick interval:        %f\n", tick_int);
    fprintf(stderr, "dbg2       label interval:       %f\n", label_int);
    fprintf(stderr, "dbg2       tick length:          %f\n", tick_len);
    fprintf(stderr, "dbg2       label height:         %f\n", label_hgt);
    fprintf(stderr, "dbg2       label spacing:        %f\n", label_spacing);
    fprintf(stderr, "dbg2       number of colors:     %d\n", ncolor);
    fprintf(stderr, "dbg2       number of levels:     %d\n", nlevel);
    for (int i = 0; i < nlevel; i++)
      fprintf(stderr, "dbg2       level %d: %f %d %d\n", i, level_list[i], label_list[i], tick_list[i]);
    fprintf(stderr, "dbg2       time tick int:        %f\n", time_tick_int);
    fprintf(stderr, "dbg2       time interval:        %f\n", time_annot_int);
    fprintf(stderr, "dbg2       date interval:        %f\n", date_annot_int);
    fprintf(stderr, "dbg2       time tick length:     %f\n", time_tick_len);
    fprintf(stderr, "dbg2       name height:          %f\n", name_hgt);
    fprintf(stderr, "dbg2       pingnumber tick int:  %d\n", pingnumber_tick_int);
    fprintf(stderr, "dbg2       pingnumber annot int: %d\n", pingnumber_annot_int);
    fprintf(stderr, "dbg2       pingnumber tick len:  %f\n", pingnumber_tick_len);
    fprintf(stderr, "dbg2       contour_plot():       %p\n", contour_plot);
    fprintf(stderr, "dbg2       contour_newpen():     %p\n", contour_newpen);
    fprintf(stderr, "dbg2       contour_setline():    %p\n", contour_setline);
    fprintf(stderr, "dbg2       contour_justify_string():     %p\n", contour_justify_string);
    fprintf(stderr, "dbg2       contour_plot_string():     %p\n", contour_plot_string);
  }

  /* allocate memory for swath structure */
  int status = mb_mallocd(verbose, __FILE__, __LINE__, sizeof(struct swath), (void **)data, error);
  memset(*data, 0, sizeof(struct swath));

  /* set variables and allocate memory for bathymetry data */
  struct swath *dataptr = *data;
  dataptr->npings = 0;
  dataptr->npings_max = npings_max;
  dataptr->beams_bath = beams_bath;
  status &= mb_mallocd(verbose, __FILE__, __LINE__, npings_max * sizeof(struct ping), (void **)&(dataptr->pings), error);
  memset(dataptr->pings, 0, npings_max * sizeof(struct ping));
  for (int i = 0; i < npings_max; i++) {
    struct ping *ping = &dataptr->pings[i];
    ping->beams_bath = 0;
    ping->beams_bath_alloc = MAX(beams_bath, NUM_BEAMS_ALLOC_MIN);
    ping->beamflag = NULL;
    ping->bath = NULL;
    ping->bathlon = NULL;
    ping->bathlat = NULL;
    status &= mb_mallocd(verbose, __FILE__, __LINE__, ping->beams_bath_alloc * sizeof(char), (void **)&(ping->beamflag), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, ping->beams_bath_alloc * sizeof(double), (void **)&(ping->bath), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, ping->beams_bath_alloc * sizeof(double), (void **)&(ping->bathlon), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, ping->beams_bath_alloc * sizeof(double), (void **)&(ping->bathlat), error);
    if (contour_algorithm == MB_CONTOUR_TRIANGLES) {
      ping->bflag[0] = NULL;
      ping->bflag[1] = NULL;
    }
    else {
      ping->bflag[0] = NULL;
      ping->bflag[1] = NULL;
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ping->beams_bath_alloc * sizeof(int), (void **)&(ping->bflag[0]), error);
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ping->beams_bath_alloc * sizeof(int), (void **)&(ping->bflag[1]), error);
    }
  }

  /* set controls on what gets plotted */
  dataptr->contour_algorithm = contour_algorithm;
  dataptr->plot_contours = plot_contours;
  dataptr->plot_triangles = plot_triangles;
  dataptr->plot_track = plot_track;
  dataptr->plot_name = plot_name;
  dataptr->plot_pingnumber = plot_pingnumber;

  /* set variables and allocate memory for contour controls */
  dataptr->contour_int = contour_int;
  dataptr->color_int = color_int;
  dataptr->tick_int = tick_int;
  dataptr->label_int = label_int;
  dataptr->tick_len = tick_len;
  dataptr->label_hgt = label_hgt;
  if (label_spacing > 0.0)
    dataptr->label_spacing = label_spacing;
  else
    dataptr->label_spacing = label_hgt;
  dataptr->ncolor = ncolor;
  dataptr->nlevel = nlevel;
  dataptr->nlevelset = false;
  dataptr->level_list = NULL;
  dataptr->label_list = NULL;
  dataptr->tick_list = NULL;
  dataptr->color_list = NULL;
  if (nlevel > 0) {
    dataptr->nlevelset = true;
    status &= mb_mallocd(verbose, __FILE__, __LINE__, nlevel * sizeof(double), (void **)&(dataptr->level_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, nlevel * sizeof(int), (void **)&(dataptr->label_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, nlevel * sizeof(int), (void **)&(dataptr->tick_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, nlevel * sizeof(int), (void **)&(dataptr->color_list), error);
    for (int i = 0; i < nlevel; i++) {
      dataptr->level_list[i] = level_list[i];
      dataptr->label_list[i] = label_list[i];
      dataptr->tick_list[i] = tick_list[i];
      dataptr->color_list[i] = i;
    }
  }

  /* set variables and allocate memory for track controls */
  dataptr->time_tick_int = time_tick_int;
  dataptr->time_annot_int = time_annot_int;
  dataptr->date_annot_int = date_annot_int;
  dataptr->time_tick_len = time_tick_len;
  dataptr->name_hgt = name_hgt;

  /* set variables and allocate memory for pingnumber control parameters */
  dataptr->pingnumber_tick_int = pingnumber_tick_int;
  dataptr->pingnumber_annot_int = pingnumber_annot_int;
  dataptr->pingnumber_tick_len = pingnumber_tick_len;

  /* set variables and allocate memory for triangle network */
  dataptr->npts = 0;
  dataptr->edge = NULL;
  dataptr->pingid = NULL;
  dataptr->beamid = NULL;
  dataptr->ntri = 0;
  dataptr->ntri_alloc = 0;
  for (int i = 0; i < 3; i++) {
    dataptr->iv[i] = NULL;
    dataptr->ct[i] = NULL;
    dataptr->cs[i] = NULL;
    dataptr->ed[i] = NULL;
    dataptr->flag[i] = NULL;
  }
  dataptr->bath_min = 0.0;
  dataptr->bath_max = 0.0;
  dataptr->triangle_scale = 0.0;
  dataptr->x = NULL;
  dataptr->y = NULL;
  dataptr->z = NULL;
  dataptr->ndelaun_alloc = 0;
  dataptr->v1 = NULL;
  dataptr->v2 = NULL;
  dataptr->v3 = NULL;
  dataptr->istack = NULL;
  dataptr->kv1 = NULL;
  dataptr->kv2 = NULL;
  int ntri_max = 0;
  if (contour_algorithm == MB_CONTOUR_TRIANGLES) {
    dataptr->npts = 0;
    dataptr->npts_alloc = npings_max * beams_bath + 3;
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(int), (void **)&(dataptr->edge), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(int), (void **)&(dataptr->pingid), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(int), (void **)&(dataptr->beamid), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(double), (void **)&(dataptr->x), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(double), (void **)&(dataptr->y), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(double), (void **)&(dataptr->z), error);
    ntri_max = 3 * npings_max * beams_bath + 1;
    dataptr->ntri = 0;
    dataptr->ntri_alloc = ntri_max;
    for (int i = 0; i < 3; i++) {
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ntri_max * sizeof(int), (void **)&(dataptr->iv[i]), error);
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ntri_max * sizeof(int), (void **)&(dataptr->ct[i]), error);
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ntri_max * sizeof(int), (void **)&(dataptr->cs[i]), error);
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ntri_max * sizeof(int), (void **)&(dataptr->ed[i]), error);
      status &= mb_mallocd(verbose, __FILE__, __LINE__, ntri_max * sizeof(int), (void **)&(dataptr->flag[i]), error);
    }
  }

  /* allocate memory for contour positions */
  dataptr->nsave = 0;
  dataptr->xsave = NULL;
  dataptr->ysave = NULL;
  dataptr->isave = NULL;
  dataptr->jsave = NULL;
  if (contour_algorithm == MB_CONTOUR_TRIANGLES) {
  	dataptr->nsave_alloc = (4 * ntri_max + 1);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->nsave_alloc * sizeof(double), (void **)&(dataptr->xsave), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->nsave_alloc * sizeof(double), (void **)&(dataptr->ysave), error);
  }
  else {
    dataptr->npts = 0;
    dataptr->npts_alloc = npings_max * beams_bath;
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(double), (void **)&(dataptr->xsave), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(double), (void **)&(dataptr->ysave), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(int), (void **)&(dataptr->isave), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, dataptr->npts_alloc * sizeof(int), (void **)&(dataptr->jsave), error);
  }

  /* allocate memory for contour labels */
  dataptr->nlabel = 0;
  dataptr->xlabel = NULL;
  dataptr->ylabel = NULL;
  dataptr->angle = NULL;
  dataptr->justify = NULL;
  status &= mb_mallocd(verbose, __FILE__, __LINE__, (5 * npings_max) * sizeof(double), (void **)&(dataptr->xlabel), error);
  status &= mb_mallocd(verbose, __FILE__, __LINE__, (5 * npings_max) * sizeof(double), (void **)&(dataptr->ylabel), error);
  status &= mb_mallocd(verbose, __FILE__, __LINE__, (5 * npings_max) * sizeof(double), (void **)&(dataptr->angle), error);
  status &= mb_mallocd(verbose, __FILE__, __LINE__, (5 * npings_max) * sizeof(int), (void **)&(dataptr->justify), error);

  /* set plotting function pointers */
  dataptr->contour_plot = contour_plot;
  dataptr->contour_newpen = contour_newpen;
  dataptr->contour_setline = contour_setline;
  dataptr->contour_justify_string = contour_justify_string;
  dataptr->contour_plot_string = contour_plot_string;

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
    fprintf(stderr, "dbg2  Return values:\n");
    fprintf(stderr, "dbg2       error:      %d\n", *error);
    fprintf(stderr, "dbg2  Return status:\n");
    fprintf(stderr, "dbg2       status:     %d\n", status);
  }

  return (MB_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/*   function mb_contour_deall deallocates the memory required to
    contour multibeam bathymetry data. */
int mb_contour_deall(int verbose, struct swath *data, int *error) {
  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       verbose:                 %d\n", verbose);
    fprintf(stderr, "dbg2       data:                    %p\n", data);
  }

  int status = MB_SUCCESS;

  /* deallocate memory for bathymetry data */
  if (data->npings_max > 0) {
    for (int i = 0; i < data->npings_max; i++) {
      struct ping *ping = &data->pings[i];
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&ping->beamflag, error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&ping->bath, error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&ping->bathlon, error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&ping->bathlat, error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&ping->bflag[0], error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&ping->bflag[1], error);
    }
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->pings, error);
    data->npings_max = 0;
  }
  data->npings = 0;
  data->beams_bath = 0;

  /* deallocate memory for contour controls */
  data->contour_algorithm = 0;
  data->plot_contours = 0;
  data->plot_triangles = 0;
  data->plot_track = 0;
  data->plot_name = 0;
  data->plot_pingnumber = 0;
  data->contour_int = 0.0;
  data->color_int = 0.0;
  data->tick_int = 0.0;
  data->label_int = 0.0;
  data->tick_len = 0.0;
  data->label_hgt = 0.0;
  data->label_spacing = 0.0;
  if (data->nlevel > 0) {
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->level_list, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->label_list, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->tick_list, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->color_list, error);
    data->nlevel = 0;
  }
  data->ncolor = 0;
  data->nlevelset = 0;

  data->time_tick_int = 0.0;
  data->time_annot_int = 0.0;
  data->date_annot_int = 0.0;
  data->time_tick_len = 0.0;
  data->name_hgt = 0.0;
  data->pingnumber_tick_int = 0;
  data->pingnumber_annot_int = 0;
  data->pingnumber_tick_len = 0.0;

  /* deallocate memory for triangle network */
  if (data->ntri_alloc > 0) {
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->edge, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->pingid, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->beamid, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->x, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->y, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->z, error);
    data->npts_alloc = 0;
  }
  data->npts = 0;

  if (data->ntri_alloc > 0) {
    for (int i = 0; i < 3; i++) {
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->iv[i], error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->ct[i], error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->cs[i], error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->ed[i], error);
      status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->flag[i], error);
    }
    data->ntri_alloc = 0;
  }
  data->ntri = 0;
  data->bath_min = 0.0;
  data->bath_max = 0.0;

  if (data->ndelaun_alloc > 0) {
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->v1, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->v2, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->v3, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->istack, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->kv1, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->kv2, error);
    data->ndelaun_alloc = 0;
  }

  /* deallocate memory for contour positions */
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->xsave, error);
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->ysave, error);
  if (data->contour_algorithm != MB_CONTOUR_TRIANGLES) {
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->isave, error);
    status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->jsave, error);
  }

  /* deallocate memory for contour labels */
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->xlabel, error);
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->ylabel, error);
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->angle, error);
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data->justify, error);

  /* deallocate memory for swath structure */
  status &= mb_freed(verbose, __FILE__, __LINE__, (void **)&data, error);

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
    fprintf(stderr, "dbg2  Return values:\n");
    fprintf(stderr, "dbg2       error:      %d\n", *error);
    fprintf(stderr, "dbg2  Return status:\n");
    fprintf(stderr, "dbg2       status:     %d\n", status);
  }

  return (MB_SUCCESS);
}
/*--------------------------------------------------------------------------*/
/*   function get_start_tri finds next contour starting point. */
static int get_start_tri(struct swath *data, int *itri, int *iside1, int *iside2, int *closed) {
  /* search triangles */
  *closed = false;
  for (int i = 0; i < data->ntri; i++)
    for (int j = 0; j < 3; j++) {
      if (data->flag[j][i] > 0) {
        /* find two flagged sides */
        *itri = i;
        *iside1 = j;
        *iside2 = -1;
        for (int jj = 0; jj < 3; jj++)
          if (jj != j && data->flag[jj][i] > 0)
            *iside2 = jj;
        if (*iside2 == -1) {
          fprintf(stderr, "no flagged side in get_start_tri???\n");
          fprintf(stderr, "noflag: itri:%d flags: %d %d %d\n", *itri, data->flag[0][*itri], data->flag[1][*itri],
                  data->flag[2][*itri]);
        }

        /* check if contour continues on both sides */
        if (data->ct[*iside1][i] > -1 && data->ct[*iside2][i] > -1)
          *closed = true;

        /* else make sure contour starts at dead end */
        else if (data->ct[*iside1][i] > -1) {
          const int isave = *iside1;
          *iside1 = *iside2;
          *iside2 = isave;
          *closed = false;
        }
        else
          *closed = false;
        return (true);
      }
    }

  /* nothing found */
  return (false);
}
/*--------------------------------------------------------------------------*/
/*   function get_next_tri finds next contour component if it exists */
static int get_next_tri(struct swath *data, int *itri, int *iside1, int *iside2, int *closed, int *itristart, int *isidestart) {
  /* check if contour ends where it began */
  if (*closed && data->ct[*iside2][*itri] == *itristart && data->cs[*iside2][*itri] == *isidestart)
    return (false);

  /* check if current triangle side connects to another */
  else if (data->ct[*iside2][*itri] > -1) {
    *iside1 = data->cs[*iside2][*itri];
    *itri = data->ct[*iside2][*itri];
    *iside2 = -1;
    for (int j = 0; j < 3; j++)
      if (j != *iside1 && data->flag[j][*itri] != 0)
        *iside2 = j;
    if (*iside2 == -1) {
      fprintf(stderr, "no flagged side in get_next_tri???\n");
      fprintf(stderr, "noflag: itri:%d flags: %d %d %d\n", *itri, data->flag[0][*itri], data->flag[1][*itri],
              data->flag[2][*itri]);
      return (false);
    }
    return (true);
  }

  /* else if contour ends but closed set true then
      turn contour around and continue in other direction */
  else if (*closed) {
    for (int i = 0; i < data->nsave / 2; i++) {
      const double xs = data->xsave[i];
      const double ys = data->ysave[i];
      data->xsave[i] = data->xsave[data->nsave - i - 1];
      data->ysave[i] = data->ysave[data->nsave - i - 1];
      data->xsave[data->nsave - i - 1] = xs;
      data->ysave[data->nsave - i - 1] = ys;
    }
    *closed = false;
    data->nsave--;
    const int itrisave = *itristart;
    const int isidesave = *isidestart;
    *itristart = *itri;
    *isidestart = *iside2;
    *itri = itrisave;
    *iside2 = isidesave;
    *iside1 = -1;
    for (int j = 0; j < 3; j++)
      if (j != *iside2 && data->flag[j][*itri] != 0)
        *iside1 = j;

    /* if next side not found end contour */
    if (*iside1 == -1)
      return (false);

    /* else keep going */
    return (true);
  }

  /* else contour ends and is not closed */
  else
    return (false);
}
/*--------------------------------------------------------------------------*/
/*   function get_pos_tri finds position of contour crossing point */
static int get_pos_tri(const struct swath *data, double eps, int itri, int iside, double value, double *x, double *y) {
  const int v1 = iside;
  int v2 = iside + 1;
  if (v2 == 3)
    v2 = 0;
  const int ipt1 = data->iv[v1][itri];
  const int ipt2 = data->iv[v2][itri];
  double factor;
  if (fabs(data->z[ipt2] - data->z[ipt1]) > eps)
    factor = (value - data->z[ipt1]) / (data->z[ipt2] - data->z[ipt1]);
  else
    factor = 0.5;
  *x = data->x[ipt1] + factor * (data->x[ipt2] - data->x[ipt1]);
  *y = data->y[ipt1] + factor * (data->y[ipt2] - data->y[ipt1]);

  return (true);
}
/*--------------------------------------------------------------------------*/
/*   function get_azimuth_tri gets azimuth across track for a label */
static int get_azimuth_tri(const struct swath *data, int itri, int iside, double *angle) {
  *angle = -data->pings[data->pingid[data->iv[iside][itri]]].heading;
  if (*angle > 180.0)
    *angle = *angle - 360.0;
  if (*angle < -180.0)
    *angle = *angle + 360.0;

  return (true);
}
/*--------------------------------------------------------------------------*/
/*   function check_label checks if new label will overwrite any recent
 *  labels. */
static int check_label(struct swath *data, int nlab) {
#define MAXHIS 30
  static double xlabel_his[MAXHIS];
  static double ylabel_his[MAXHIS];
  static int nlabel_his = 0;

  int good = 1;
  int ilab = 0;
  double rad_label_his = data->label_spacing;
  while (good && ilab < nlabel_his) {
    const double dx = xlabel_his[ilab] - data->xlabel[nlab];
    const double dy = ylabel_his[ilab] - data->ylabel[nlab];
    const double rr = sqrt(dx * dx + dy * dy);
    if (rr < rad_label_his)
      good = 0;
    ilab++;
  }
  ilab--;
  if (good) {
    nlabel_his++;
    if (nlabel_his >= MAXHIS)
      nlabel_his = MAXHIS - 1;
    for (int i = nlabel_his; i > 0; i--) {
      xlabel_his[i] = xlabel_his[i - 1];
      ylabel_his[i] = ylabel_his[i - 1];
    }
    xlabel_his[0] = data->xlabel[nlab];
    ylabel_his[0] = data->ylabel[nlab];
  }
  return (good);
}
/*--------------------------------------------------------------------------*/
/*   function dump_contour dumps the contour stored in xsave and ysave
 *  to the plotting routines */
static int dump_contour(struct swath *data, double value) {
  /* plot the contours */
  if (data->nsave < 2)
    return (false);
  data->contour_plot(data->xsave[0], data->ysave[0], IMOVE);
  for (int i = 1; i < data->nsave - 1; i++)
    data->contour_plot(data->xsave[i], data->ysave[i], IDRAW);
  data->contour_plot(data->xsave[data->nsave - 1], data->ysave[data->nsave - 1], ISTROKE);
  data->nsave = 0;

  /* plot the labels */
  char label[25];
  sprintf(label, "  %d", (int)value);
  for (int i = 0; i < data->nlabel; i++) {
    if (data->justify[i] == 1) {
      double mtodeglon;
      double mtodeglat;
      mb_coor_scale(0, data->ylabel[i], &mtodeglon, &mtodeglat);
      double s[4];
      data->contour_justify_string(data->label_hgt, label, s);
      const double dx = 1.5 * s[2] * cos(DTR * data->angle[i]);
      const double dy = 1.5 * mtodeglat / mtodeglon * s[2] * sin(DTR * data->angle[i]);
      const double x = data->xlabel[i] - dx;
      const double y = data->ylabel[i] - dy;
      data->contour_plot_string(x, y, data->label_hgt, data->angle[i], label);
    }
    else {
      data->contour_plot_string(data->xlabel[i], data->ylabel[i], data->label_hgt, data->angle[i], label);
    }
  }
  data->nlabel = 0;

  return (true);
}
/*--------------------------------------------------------------------------*/
/*  function mb_triangulate calculates a delauney triangulization of the
    swath bathymetry in data */
int mb_triangulate(int verbose, struct swath *data, int *error) {

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       verbose:                 %d\n", verbose);
    fprintf(stderr, "dbg2       data:                    %p\n", data);
    fprintf(stderr, "dbg2       data->contour_algorithm: %d\n", data->contour_algorithm);
    fprintf(stderr, "dbg2       data->plot_contours:     %d\n", data->plot_contours);
    fprintf(stderr, "dbg2       data->plot_triangles:    %d\n", data->plot_triangles);
    fprintf(stderr, "dbg2       data->plot_track:        %d\n", data->plot_track);
    fprintf(stderr, "dbg2       data->plot_name:         %d\n", data->plot_name);
    fprintf(stderr, "dbg2       data->contour_int:       %f\n", data->contour_int);
    fprintf(stderr, "dbg2       data->color_int:         %f\n", data->color_int);
    fprintf(stderr, "dbg2       data->tick_int:          %f\n", data->tick_int);
    fprintf(stderr, "dbg2       data->label_int:         %f\n", data->label_int);
    fprintf(stderr, "dbg2       data->tick_len:          %f\n", data->tick_len);
    fprintf(stderr, "dbg2       data->label_hgt:         %f\n", data->label_hgt);
    fprintf(stderr, "dbg2       data->label_spacing:     %f\n", data->label_spacing);
    fprintf(stderr, "dbg2       data->ncolor:            %d\n", data->ncolor);
    fprintf(stderr, "dbg2       data->nlevel:            %d\n", data->nlevel);
    fprintf(stderr, "dbg2       data->nlevelset:         %d\n", data->nlevelset);
    if (data->nlevelset)
      for (int i = 0; i < data->nlevel; i++) {
        fprintf(stderr, "dbg2          level[%3d]:  %f %d %d %d\n", i, data->level_list[i], data->label_list[i],
                data->tick_list[i], data->color_list[i]);
      }
    fprintf(stderr, "dbg2       data->npings:     %d\n", data->npings);
    fprintf(stderr, "dbg2       data->npings_max: %d\n", data->npings_max);
    fprintf(stderr, "dbg2       data->beams_bath: %d\n", data->beams_bath);
    for (int i = 0; i < data->npings; i++) {
      fprintf(stderr, "dbg2          ping[%4d]: %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d:%6.6d %f %f %f %f %d\n", i,
              data->pings[i].time_i[0], data->pings[i].time_i[1], data->pings[i].time_i[2], data->pings[i].time_i[3],
              data->pings[i].time_i[4], data->pings[i].time_i[5], data->pings[i].time_i[6], data->pings[i].time_d,
              data->pings[i].navlon, data->pings[i].navlat, data->pings[i].heading, data->pings[i].beams_bath);
      for (int j = 0; j < data->pings[i].beams_bath; j++) {
        if (mb_beam_ok(data->pings[i].beamflag[j]))
          fprintf(stderr, "dbg2          beam[%4d:%3d]:  %2d %f %f %f\n", i, j, data->pings[i].beamflag[j],
                  data->pings[i].bath[j], data->pings[i].bathlon[j], data->pings[i].bathlat[j]);
      }
    }
  }

  /* count number of points and verify that enough memory is allocated */
  int npt_cnt = 0;
  for (int i = 0; i < data->npings; i++) {
    struct ping *ping = &data->pings[i];
    for (int j = 0; j < ping->beams_bath; j++) {
      if (mb_beam_ok(ping->beamflag[j]))
        npt_cnt++;
    }
  }

  // allocate memory as needed
  int status = MB_SUCCESS;
  const int ntri_cnt = 3 * npt_cnt + 1;
  if (npt_cnt > data->npts_alloc) {
    data->npts_alloc = npt_cnt;
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->npts_alloc * sizeof(int), (void **)&data->edge, error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->npts_alloc * sizeof(int), (void **)&data->pingid, error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->npts_alloc * sizeof(int), (void **)&data->beamid, error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->npts_alloc * sizeof(double), (void **)&data->x, error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->npts_alloc * sizeof(double), (void **)&data->y, error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->npts_alloc * sizeof(double), (void **)&data->z, error);
    memset(data->edge, 0, data->npts_alloc * sizeof(int));
    memset(data->pingid, 0, data->npts_alloc * sizeof(int));
    memset(data->beamid, 0, data->npts_alloc * sizeof(int));
    memset(data->x, 0, data->npts_alloc * sizeof(double));
    memset(data->y, 0, data->npts_alloc * sizeof(double));
    memset(data->z, 0, data->npts_alloc * sizeof(double));
  }
  if (ntri_cnt > data->ntri_alloc) {
    data->ntri_alloc = ntri_cnt;
    for (int i = 0; i < 3; i++) {
      status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(int), (void **)&(data->iv[i]), error);
      status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(int), (void **)&(data->ct[i]), error);
      status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(int), (void **)&(data->cs[i]), error);
      status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(int), (void **)&(data->ed[i]), error);
      status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(int), (void **)&(data->flag[i]), error);
      memset(data->iv[i], 0, ntri_cnt * sizeof(int));
      memset(data->ct[i], 0, ntri_cnt * sizeof(int));
      memset(data->cs[i], 0, ntri_cnt * sizeof(int));
      memset(data->ed[i], 0, ntri_cnt * sizeof(int));
      memset(data->flag[i], 0, ntri_cnt * sizeof(int));
    }
    status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(double), (void **)&(data->v1), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(double), (void **)&(data->v2), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(double), (void **)&(data->v3), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, ntri_cnt * sizeof(int), (void **)&(data->istack), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, 3 * ntri_cnt * sizeof(int), (void **)&(data->kv1), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, 3 * ntri_cnt * sizeof(int), (void **)&(data->kv2), error);
    data->nsave_alloc = (4 * ntri_cnt + 1);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->xsave), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->ysave), error);
    memset(data->v1, 0, ntri_cnt * sizeof(double));
    memset(data->v2, 0, ntri_cnt * sizeof(double));
    memset(data->v3, 0, ntri_cnt * sizeof(double));
    memset(data->istack, 0, ntri_cnt * sizeof(int));
    memset(data->kv1, 0, 3 * ntri_cnt * sizeof(int));
    memset(data->kv2, 0, 3 * ntri_cnt * sizeof(int));
    memset(data->xsave, 0, data->nsave_alloc * sizeof(double));
    memset(data->ysave, 0, data->nsave_alloc * sizeof(double));
  }

  /* construct list of good soundings */
  data->npts = 0;
  for (int i = 0; i < data->npings; i++) {
    struct ping *ping = &data->pings[i];

    /* find edges of ping */
    int left = ping->beams_bath / 2;
    int right = left;
    for (int j = 0; j < ping->beams_bath; j++) {
      if (j < left && mb_beam_ok(ping->beamflag[j]))
        left = j;
      if (j > right && mb_beam_ok(ping->beamflag[j]))
        right = j;
    }

    /* add valid points to list */
    for (int j = 0; j < ping->beams_bath; j++) {
      if (mb_beam_ok(ping->beamflag[j])) {
        data->pingid[data->npts] = i;
        data->beamid[data->npts] = j;
        if (j == right)
          data->edge[data->npts] = 1;
        else if (j == left)
          data->edge[data->npts] = -1;
        else
          data->edge[data->npts] = 0;
        data->x[data->npts] = ping->bathlon[j];
        data->y[data->npts] = ping->bathlat[j];
        data->z[data->npts] = ping->bath[j];
        data->npts++;
      }
    }
  }

  // get extrema of preliminary soundings and the minimum  distance allowed
  // between points submitted for triangulization
  double mtodeglon = 0.0;
  double mtodeglat = 0.0;
  double rr_threshold = 0.0;
  double xmin = 0.0;
  double xmax = 0.0;
  double ymin = 0.0;
  double ymax = 0.0;
  double zmin = 0.0;
  double zmax = 0.0;
  double dlon = 0.0;
  double dlat = 0.0;
  if (data->npts > 0) {
    xmin = data->x[0];
    xmax = data->x[0];
    ymin = data->y[0];
    ymax = data->y[0];
    zmin = data->z[0];
    zmax = data->z[0];
    for (int ipt = 1; ipt < data->npts; ipt++) {
      xmin = MIN(xmin, data->x[ipt]);
      xmax = MAX(xmax, data->x[ipt]);
      ymin = MIN(ymin, data->y[ipt]);
      ymax = MAX(ymax, data->y[ipt]);
      zmin = MIN(zmin, data->z[ipt]);
      zmax = MAX(zmax, data->z[ipt]);
    }
    mb_coor_scale(verbose, 0.5 * (ymin + ymax), &mtodeglon, &mtodeglat);
    if (data->triangle_scale > 0.001) {
      rr_threshold = data->triangle_scale;
    } else {
      rr_threshold = MAX(0.01 * (xmax - xmin) / mtodeglon, 0.01 * (ymax - ymin) / mtodeglat);
// fprintf(stderr, "%s:%d:%s: rr_threshold: %f m\n", __FILE__, __LINE__, __FUNCTION__, rr_threshold);
    }
    dlon = rr_threshold * mtodeglon;
    dlat = rr_threshold * mtodeglat;
  }

  // delete all but one of points with close x-y positions where close is 1/100
  // of the long dimension of the area covered by the section
  for (int ipt0 = 0; ipt0 < data->npts; ipt0++) {
    if (data->pingid[ipt0] >= 0) {
      int ii0 = (int)floor((data->x[ipt0] - xmin) / dlon);
      int jj0 = (int)floor((data->y[ipt0] - ymin) / dlat);
      for (int ipt1 = data->npts - 1; ipt1 > ipt0; ipt1--) {
        if (data->pingid[ipt1] >= 0) {
          int ii1 = (int)floor((data->x[ipt1] - xmin) / dlon);
          int jj1 = (int)floor((data->y[ipt1] - ymin) / dlat);
          if (ii0 == ii1 && jj0 == jj1) {
            if (data->z[ipt0] > data->z[ipt1]) {
              data->pingid[ipt0] = -1;
            } else {
              data->pingid[ipt1] = -1;
            }
          }
        }
      }
    }
  }
  for (int ipt = data->npts - 1; ipt >= 0; ipt--) {
    if (data->pingid[ipt] < 0) {
      for (int ipt0=ipt; ipt0<data->npts-1; ipt0++) {
        data->pingid[ipt0] = data->pingid[ipt0+1];
        data->beamid[ipt0] = data->beamid[ipt0+1];
        data->edge[ipt0] = data->edge[ipt0+1];
        data->x[ipt0] = data->x[ipt0+1];
        data->y[ipt0] = data->y[ipt0+1];
        data->z[ipt0] = data->z[ipt0+1];
      }
      data->npts--;
    }
  }

  /* get extrema of remaining soundings */
  if (data->npts > 0) {
    data->bath_min = data->z[0];
    data->bath_max = data->z[0];
    for (int ipt = 1; ipt < data->npts; ipt++) {
      data->bath_min = MIN(data->bath_min, data->z[ipt]);
      data->bath_max = MAX(data->bath_max, data->z[ipt]);
    }
  }

  if (verbose >= 4) {
    fprintf(stderr, "\ndbg4  Data points to be used for triangulization:\n");
    fprintf(stderr, "dbg4       npts:             %d\n", data->npts);
    fprintf(stderr, "dbg4       bath_min:         %f\n", data->bath_min);
    fprintf(stderr, "dbg4       bath_max:         %f\n", data->bath_max);
    for (int ipt = 0; ipt < data->npts; ipt++)
      fprintf(stderr, "dbg4       %4d %4d %4d %d  %f %f %f\n",
              ipt, data->pingid[ipt], data->beamid[ipt], data->edge[ipt],
              data->x[ipt], data->y[ipt], data->z[ipt]);
  }

  /* get triangle network */
  if (data->npts > 2) {
    status = mb_delaun(verbose, data->npts, data->x, data->y, data->edge,
                      &data->ntri, data->iv[0], data->iv[1], data->iv[2],
                      data->ct[0], data->ct[1], data->ct[2], data->cs[0], data->cs[1], data->cs[2],
                      data->v1, data->v2, data->v3, data->istack, data->kv1, data->kv2, error);
  }
  if (verbose > 1)
    fprintf(stderr, "\n");
  if (verbose > 0)
    fprintf(stderr, "-->Obtained %d triangles of scale %f meters from %d points in %d pings...\n",
                    data->ntri, rr_threshold, data->npts, data->npings);

  /* figure out which triangle sides are on the swath edge */
  for (int itri = 0; itri < data->ntri; itri++) {
    for (int j = 0; j < 3; j++) {
      int jj = j + 1;
      if (jj > 2)
        jj = 0;
      if (data->edge[data->iv[j][itri]] == -1 && data->edge[data->iv[jj][itri]] == -1)
        data->ed[j][itri] = -1;
      else if (data->edge[data->iv[j][itri]] == 1 && data->edge[data->iv[jj][itri]] == 1)
        data->ed[j][itri] = 1;
      else
        data->ed[j][itri] = 0;
    }
  }

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
    fprintf(stderr, "dbg2  Return values:\n");
    fprintf(stderr, "dbg2       data:             %p\n", data);
    fprintf(stderr, "dbg2       data->npts:       %d\n", data->npts);
    fprintf(stderr, "dbg2       data->npts_alloc: %d\n", data->npts_alloc);
    for (int ipt = 0; ipt < data->npts; ipt++) {
      fprintf(stderr, "dbg2          pt[%4d]: %4d %4d  %f %f %f\n",
                      ipt, data->pingid[ipt], data->beamid[ipt],
                      data->x[ipt], data->y[ipt], data->z[ipt]);
    }
    fprintf(stderr, "dbg2       data->ntri:       %d\n", data->ntri);
    fprintf(stderr, "dbg2       data->ntri_alloc: %d\n", data->ntri_alloc);
    fprintf(stderr, "dbg2       triangle[i of %d]: <vertices> <connecting triangles> <connecting sides> <on edge?>\n",
                      data->ntri);
    for (int i = 0; i < data->ntri; i++) {
      fprintf(stderr, "dbg2          tri[%4d]: v: %d %d %d  t: %d %d %d  s: %d %d %d  e: %d %d %d\n",
                      i, data->iv[0][i], data->iv[1][i], data->iv[2][i],
                      data->ct[0][i], data->ct[1][i], data->ct[2][i],
                      data->cs[0][i], data->cs[1][i], data->cs[2][i],
                      data->ed[0][i], data->ed[0][i], data->ed[0][i]);
    }
    fprintf(stderr, "dbg2       error:      %d\n", *error);
    fprintf(stderr, "dbg2  Return status:\n");
    fprintf(stderr, "dbg2       status:     %d\n", status);
  }

  return (status);
}
/*--------------------------------------------------------------------------*/
/*   function mb_tcontour contours multibeam data that has been triangulated */
static int mb_tcontour(int verbose, struct swath *data, int *error) {

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       verbose:                 %d\n", verbose);
    fprintf(stderr, "dbg2       data:                    %p\n", data);
    fprintf(stderr, "dbg2       data->contour_algorithm: %d\n", data->contour_algorithm);
    fprintf(stderr, "dbg2       data->plot_contours:     %d\n", data->plot_contours);
    fprintf(stderr, "dbg2       data->plot_triangles:    %d\n", data->plot_triangles);
    fprintf(stderr, "dbg2       data->plot_track:        %d\n", data->plot_track);
    fprintf(stderr, "dbg2       data->plot_name:         %d\n", data->plot_name);
    fprintf(stderr, "dbg2       data->contour_int:       %f\n", data->contour_int);
    fprintf(stderr, "dbg2       data->color_int:         %f\n", data->color_int);
    fprintf(stderr, "dbg2       data->tick_int:          %f\n", data->tick_int);
    fprintf(stderr, "dbg2       data->label_int:         %f\n", data->label_int);
    fprintf(stderr, "dbg2       data->tick_len:          %f\n", data->tick_len);
    fprintf(stderr, "dbg2       data->label_hgt:         %f\n", data->label_hgt);
    fprintf(stderr, "dbg2       data->label_spacing:     %f\n", data->label_spacing);
    fprintf(stderr, "dbg2       data->ncolor:            %d\n", data->ncolor);
    fprintf(stderr, "dbg2       data->nlevel:            %d\n", data->nlevel);
    fprintf(stderr, "dbg2       data->nlevelset:         %d\n", data->nlevelset);
    if (data->nlevelset)
      for (int i = 0; i < data->nlevel; i++) {
        fprintf(stderr, "dbg2          level[%3d]:  %f %d %d %d\n", i, data->level_list[i], data->label_list[i],
                data->tick_list[i], data->color_list[i]);
      }
    fprintf(stderr, "dbg2       data->npings:     %d\n", data->npings);
    fprintf(stderr, "dbg2       data->npings_max: %d\n", data->npings_max);
    fprintf(stderr, "dbg2       data->beams_bath: %d\n", data->beams_bath);
    for (int i = 0; i < data->npings; i++) {
      fprintf(stderr, "dbg2          ping[%4d]: %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d:%6.6d %f %f %f %f %d\n", i,
              data->pings[i].time_i[0], data->pings[i].time_i[1], data->pings[i].time_i[2], data->pings[i].time_i[3],
              data->pings[i].time_i[4], data->pings[i].time_i[5], data->pings[i].time_i[6], data->pings[i].time_d,
              data->pings[i].navlon, data->pings[i].navlat, data->pings[i].heading, data->pings[i].beams_bath);
      for (int j = 0; j < data->pings[i].beams_bath; j++) {
        if (mb_beam_ok(data->pings[i].beamflag[j]))
          fprintf(stderr, "dbg2          beam[%4d:%3d]:  %2d %f %f %f\n", i, j, data->pings[i].beamflag[j],
                  data->pings[i].bath[j], data->pings[i].bathlon[j], data->pings[i].bathlat[j]);
      }
    }
    fprintf(stderr, "dbg2       data->npts:       %d\n", data->npts);
    fprintf(stderr, "dbg2       data->npts_alloc: %d\n", data->npts_alloc);
    fprintf(stderr, "dbg2       data->bath_min:   %f\n", data->bath_min);
    fprintf(stderr, "dbg2       data->bath_max:   %f\n", data->bath_max);
    for (int ipt = 0; ipt < data->npts; ipt++) {
      fprintf(stderr, "dbg2          pt[%4d]: %4d %4d  %f %f %f\n",
                      ipt, data->pingid[ipt], data->beamid[ipt],
                      data->x[ipt], data->y[ipt], data->z[ipt]);
    }
    fprintf(stderr, "dbg2       data->ntri:       %d\n", data->ntri);
    fprintf(stderr, "dbg2       data->ntri_alloc: %d\n", data->ntri_alloc);
    fprintf(stderr, "dbg2       triangle[i of %d]: <vertices> <connecting triangles> <connecting sides> <on edge?>\n",
                      data->ntri);
    for (int i = 0; i < data->ntri; i++) {
      fprintf(stderr, "dbg2          tri[%4d]: v: %d %d %d  t: %d %d %d  s: %d %d %d  e: %d %d %d\n",
                      i, data->iv[0][i], data->iv[1][i], data->iv[2][i],
                      data->ct[0][i], data->ct[1][i], data->ct[2][i],
                      data->cs[0][i], data->cs[1][i], data->cs[2][i],
                      data->ed[0][i], data->ed[0][i], data->ed[0][i]);
    }
  }

  /* if no depth variation don't bother */
  int status = MB_SUCCESS;
  if ((data->bath_max - data->bath_min) < EPS)
    return (status);

  /* get number of contour intervals */
  if (!data->nlevelset) {
    if (data->nlevel > 0) {
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->level_list, error);
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->color_list, error);
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->label_list, error);
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->tick_list, error);
    }
    const int nci = data->bath_min / data->contour_int + 1;
    const int ncf = data->bath_max / data->contour_int + 1;
    data->nlevel = ncf - nci;
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(double), (void **)&(data->level_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(int), (void **)&(data->color_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(int), (void **)&(data->label_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(int), (void **)&(data->tick_list), error);
    if (*error != MB_ERROR_NO_ERROR)
      return (status);
    for (int i = 0; i < data->nlevel; i++) {
      const int k = nci + i;
      data->level_list[i] = k * data->contour_int;
      data->color_list[i] = (int)(data->level_list[i] / data->color_int) % data->ncolor;
      if (data->tick_int <= 0.0)
        data->tick_list[i] = 0;

      else {
        const double ratio = data->level_list[i] / data->tick_int;
        if (fabs(round(ratio) - ratio) < 0.005 * data->contour_int)
          data->tick_list[i] = 1;
        else
          data->tick_list[i] = 0;
      }
      if (data->label_int <= 0.0)
        data->label_list[i] = 0;
      else {
        const double ratio = data->level_list[i] / data->label_int;
        if (fabs(round(ratio) - ratio) < 0.005 * data->contour_int)
          data->label_list[i] = 1;
        else
          data->label_list[i] = 0;
      }
    }
  }

  if (verbose >= 4) {
    fprintf(stderr, "\ndbg4  Contour levels:\n");
    fprintf(stderr, "dbg4       nlevel:           %d\n", data->nlevel);
    fprintf(stderr, "dbg4       i level color tick label:\n");
    for (int i = 0; i < data->nlevel; i++)
      fprintf(stderr, "dbg4       %d %f %d %d %d\n", i, data->level_list[i], data->color_list[i], data->tick_list[i],
              data->label_list[i]);
  }

  /* make sure that no depths are exact contour values */
  const double eps = EPS * (data->bath_max - data->bath_min);
  for (int klevel = 0; klevel < data->nlevel; klevel++) {
    for (int ipt = 0; ipt < data->npts; ipt++) {
      if (fabs(data->z[ipt] - data->level_list[klevel]) < eps)
        data->z[ipt] = data->level_list[klevel] + eps;
    }
  }

  /* plot the triangles if desired */
  if (data->plot_triangles) {
    data->contour_newpen(0);
    for (int itri = 0; itri < data->ntri; itri++) {
      const int ipt0 = data->iv[0][itri];
      const int ipt1 = data->iv[1][itri];
      const int ipt2 = data->iv[2][itri];
      data->contour_plot(data->x[ipt0], data->y[ipt0], IMOVE);
      data->contour_plot(data->x[ipt1], data->y[ipt1], IDRAW);
      data->contour_plot(data->x[ipt2], data->y[ipt2], IDRAW);
      data->contour_plot(data->x[ipt0], data->y[ipt0], ISTROKE);
    }
  }

  /* loop over all of the contour values */
  data->nsave = 0;
  data->nlabel = 0;
  if (status == MB_SUCCESS && data->plot_contours)
    for (int ival = 0; ival < data->nlevel; ival++) {
      const double value = data->level_list[ival];
      data->contour_newpen(data->color_list[ival]);
      const int tick = data->tick_list[ival];
      const int label = data->label_list[ival];

      if (verbose >= 4) {
        fprintf(stderr, "\ndbg4  About to contour level in function <%s>\n", __func__);
        fprintf(stderr, "dbg4       value:         %f\n", value);
        fprintf(stderr, "dbg4       tick:          %d\n", tick);
        fprintf(stderr, "dbg4       label:         %d\n", label);
      }

      /* flag all triangle sides crossed by the current contour */
      for (int itri = 0; itri < data->ntri; itri++) {
        for (int j = 0; j < 3; j++) {
          int jj = j + 1;
          if (jj == 3)
            jj = 0;
          if ((data->z[data->iv[j][itri]] > value && data->z[data->iv[jj][itri]] < value) ||
              (data->z[data->iv[jj][itri]] > value && data->z[data->iv[j][itri]] < value))
            data->flag[j][itri] = 1;
          else
            data->flag[j][itri] = 0;
        }
      }

      /* do the contouring */
      data->nsave = 0;
      int itri;
      int iside1;
      int iside2;
      int closed;
      while (get_start_tri(data, &itri, &iside1, &iside2, &closed)) {
        
        /* check that large enough xsave and ysave arrays have been allocated */
        if (data->nsave >= data->nsave_alloc - 4) {
			data->nsave_alloc += 8192;
			status &= mb_reallocd(2, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->xsave), error);
			status &= mb_reallocd(2, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->ysave), error);
        }

        /* if not closed remove flags */
        data->flag[iside1][itri] = -1;
        data->flag[iside2][itri] = -1;

        /* get position of start of contour */
        get_pos_tri(data, eps, itri, iside1, value, &data->xsave[data->nsave], &data->ysave[data->nsave]);
        data->nsave++;
        get_pos_tri(data, eps, itri, iside2, value, &data->xsave[data->nsave], &data->ysave[data->nsave]);
        data->nsave++;
        int itristart = itri;
        int isidestart = iside1;
        int itriend = itri;
        int isideend = iside2;

        /* set tick flag */
        bool tick_last = false;

        /* look for next segment */
        while (get_next_tri(data, &itri, &iside1, &iside2, &closed, &itristart, &isidestart)) {
		  
		  /* check that large enough xsave and ysave arrays have been allocated */
		  if (data->nsave >= data->nsave_alloc - 4) {
			  data->nsave_alloc += 8192;
			  status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->xsave), error);
			  status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->ysave), error);
		  }

          /* get position */
          double x;
          double y;
          get_pos_tri(data, eps, itri, iside2, value, &x, &y);

          /* deal with tick as needed */
          if (tick && !tick_last) {
            int hand = 0;
            if (data->z[data->iv[iside1][itri]] > data->z[data->iv[iside2][itri]])
              hand = -1;
            else
              hand = 1;
            data->xsave[data->nsave] = 0.5 * (x + data->xsave[data->nsave - 1]);
            data->ysave[data->nsave] = 0.5 * (y + data->ysave[data->nsave - 1]);
            const double magdis =
                sqrt(pow((x - data->xsave[data->nsave - 1]), 2.0) + pow((y - data->ysave[data->nsave - 1]), 2.0));
            data->xsave[data->nsave + 1] =
                data->xsave[data->nsave] - hand * data->tick_len * (y - data->ysave[data->nsave - 1]) / magdis;
            data->ysave[data->nsave + 1] =
                data->ysave[data->nsave] + hand * data->tick_len * (x - data->xsave[data->nsave - 1]) / magdis;
            data->xsave[data->nsave + 2] = data->xsave[data->nsave];
            data->ysave[data->nsave + 2] = data->ysave[data->nsave];
            data->xsave[data->nsave + 3] = x;
            data->ysave[data->nsave + 3] = y;
            data->flag[iside1][itri] = -1;
            data->flag[iside2][itri] = -1;
            data->nsave = data->nsave + 4;
            tick_last = true;
          }
          else {
            data->xsave[data->nsave] = x;
            data->ysave[data->nsave] = y;
            data->flag[iside1][itri] = -1;
            data->flag[iside2][itri] = -1;
            data->nsave++;
            tick_last = false;
          }

          /* set latest point */
          itriend = itri;
          isideend = iside2;
        }

        /* set label if needed */
        if (label && !closed && data->ed[isidestart][itristart] != 0) {
          data->xlabel[data->nlabel] = data->xsave[0];
          data->ylabel[data->nlabel] = data->ysave[0];
          get_azimuth_tri(data, itristart, isidestart, &data->angle[data->nlabel]);
          if (data->ed[isidestart][itristart] == -1)
            data->justify[data->nlabel] = 1;
          else
            data->justify[data->nlabel] = 0;
          if (check_label(data, data->nlabel))
            data->nlabel++;
        }
        if (label && !closed && data->ed[isideend][itriend] != 0) {
          data->xlabel[data->nlabel] = data->xsave[data->nsave - 1];
          data->ylabel[data->nlabel] = data->ysave[data->nsave - 1];
          get_azimuth_tri(data, itriend, isideend, &data->angle[data->nlabel]);
          if (data->ed[isideend][itriend] == -1)
            data->justify[data->nlabel] = 1;
          else
            data->justify[data->nlabel] = 0;
          if (check_label(data, data->nlabel))
            data->nlabel++;
        }

        /* dump the contour */
        dump_contour(data, value);
      }

      /* done with contouring this level */
    }

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
    fprintf(stderr, "dbg2  Return values:\n");
    fprintf(stderr, "dbg2       error:      %d\n", *error);
    fprintf(stderr, "dbg2  Return status:\n");
    fprintf(stderr, "dbg2       status:     %d\n", status);
  }

  return (status);
}
/*--------------------------------------------------------------------------*/
/*   function get_start_old finds next contour starting point.
 *  the borders are searched first and then the interior */
static int get_start_old(const struct swath *data, int *k, int *i, int *j, int *d, int *closed) {
  /* search edges */
  *closed = 0;

  /* search bottom (i = 0) */
  for (int jj = 0; jj < data->pings[0].beams_bath - 1; jj++)
    if (data->pings[0].bflag[0][jj]) {
      *k = 0;
      *i = 0;
      *j = jj;
      *d = 0;
      return (1);
    }

  /* search top (i = npings-1) */
  for (int jj = 0; jj < data->pings[data->npings - 1].beams_bath - 1; jj++)
    if (data->pings[data->npings - 1].bflag[0][jj]) {
      *k = 0;
      *i = data->npings - 1;
      *j = jj;
      *d = 1;
      return (1);
    }

  /* search left (j = 0) */
  for (int ii = 0; ii < data->npings - 1; ii++)
    if (data->pings[ii].beams_bath > 0 && data->pings[ii].bflag[1][0]) {
      *k = 1;
      *i = ii;
      *j = 0;
      *d = 0;
      return (1);
    }

  /* search right (j = beams_bath-1) */
  for (int ii = 0; ii < data->npings - 1; ii++)
    if (data->pings[ii].beams_bath > 0 && data->pings[ii].bflag[1][data->pings[ii].beams_bath - 1]) {
      *k = 1;
      *i = ii;
      *j = data->pings[ii].beams_bath - 1;
      *d = 1;
      return (1);
    }

  /* search interior */
  *closed = 1;
  for (int ii = 0; ii < data->npings - 1; ii++)
    for (int jj = 0; jj < data->pings[ii].beams_bath - 1; jj++) {
      if (data->pings[ii].bflag[0][jj]) {
        *k = 0;
        *i = ii;
        *j = jj;
        *d = 0;
        return (1);
      }
      if (data->pings[ii].bflag[1][jj]) {
        *k = 1;
        *i = ii;
        *j = jj;
        *d = 0;
        return (1);
      }
    }

  /* nothing found */
  return (0);
}
/*--------------------------------------------------------------------------*/
/*   function get_next_old finds next contour component if it exists */
static int get_next_old(struct swath *data, int *nk, int *ni, int *nj, int *nd, int k, int i, int j, int d, int kbeg, int ibeg, int jbeg,
                 int dbeg, int *closed) {
  static const int ioff[3][2][2] = {{{0, -1}, {1, 0}}, {{1, -1}, {0, 0}}, {{0, -1}, {0, 1}}};
  static const int joff[3][2][2] = {{{0, 1}, {0, -1}}, {{0, 0}, {1, -1}}, {{1, 0}, {0, -1}}};
  static const int koff[3][2][2] = {{{1, 1}, {0, 0}}, {{0, 0}, {1, 1}}, {{1, 1}, {0, 0}}};
  static const int doff[3][2][2] = {{{1, 0}, {0, 1}}, {{0, 1}, {0, 1}}, {{0, 1}, {1, 0}}};
  int kt[3];
  int it[3];
  int jt[3];
  int dt[3];
  int ifedge[3];

  /* there are three possible edges for the contour to go to */
  /* (left = 0, across = 1, right = 2) */
  /* find out which edges have unflagged crossing points */
  for (int edge = 0; edge < 3; edge++) {
    kt[edge] = koff[edge][k][d];
    it[edge] = i + ioff[edge][k][d];
    jt[edge] = j + joff[edge][k][d];
    dt[edge] = doff[edge][k][d];
    if (it[edge] < 0 || it[edge] >= data->npings || jt[edge] < 0
            || jt[edge] >= data->pings[i].beams_bath || data->pings[it[edge]].beams_bath <= 0)
      ifedge[edge] = 0;
    else
      ifedge[edge] = data->pings[it[edge]].bflag[kt[edge]][jt[edge]];
  }

  /* if the across edge exists, use it */
  if (ifedge[1]) {
    *nk = kt[1];
    *ni = it[1];
    *nj = jt[1];
    *nd = dt[1];
    return (1);
  }

  /* else if edge 0 exists, use it */
  else if (ifedge[0]) {
    *nk = kt[0];
    *ni = it[0];
    *nj = jt[0];
    *nd = dt[0];
    return (1);
  }

  /* else if edge 2 exists, use it */
  else if (ifedge[2]) {
    *nk = kt[2];
    *ni = it[2];
    *nj = jt[2];
    *nd = dt[2];
    return (1);
  }

  /* if no edge is found and contour is closed and closes then */
  /* contour ends */
  else if (*closed && kbeg == k && ibeg == i && jbeg == j)
    return (0);

  /* if no edge is found and contour is closed but doesn't close then */
  /* reverse order of points and start over */
  else if (*closed) {
    for (int ii = 0; ii < data->nsave / 2; ii++) {
      const double xs = data->xsave[ii];
      const double ys = data->ysave[ii];
      data->xsave[ii] = data->xsave[data->nsave - ii - 1];
      data->ysave[ii] = data->ysave[data->nsave - ii - 1];
      data->xsave[data->nsave - ii - 1] = xs;
      data->ysave[data->nsave - ii - 1] = ys;
    }
    *closed = 0;
    *nk = kbeg;
    *ni = ibeg;
    *nj = jbeg;
    if (dbeg)
      *nd = 0;
    else
      *nd = 1;
    data->nsave--;
    return (1);
  }

  /* else if no edge is found and contour is not closed */
  /* then contour ends */
  else
    return (0);
}
/*--------------------------------------------------------------------------*/
/*   function get_pos_old finds position of contour crossing point */
static int get_pos_old(const struct swath *data, double eps, double *x, double *y, int k, int i, int j, double value) {

  /* get grid positions and values */
  const double x1 = data->pings[i].bathlon[j];
  const double y1 = data->pings[i].bathlat[j];
  const double v1 = data->pings[i].bath[j];
  double x2;
  double y2;
  double v2;
  if (k == 0) {
    x2 = data->pings[i].bathlon[j + 1];
    y2 = data->pings[i].bathlat[j + 1];
    v2 = data->pings[i].bath[j + 1];
  }
  else {
    x2 = data->pings[i + 1].bathlon[j];
    y2 = data->pings[i + 1].bathlat[j];
    v2 = data->pings[i + 1].bath[j];
  }

  /* interpolate the position */
  double factor;
  if (fabs(v2 - v1) > eps)
    factor = (value - v1) / (v2 - v1);
  else
    factor = 0.5;
  if (factor < 0.0)
    factor = 0.0;
  if (factor > 1.0)
    factor = 1.0;
  *x = factor * (x2 - x1) + x1;
  *y = factor * (y2 - y1) + y1;

  return (true);
}
/*--------------------------------------------------------------------------*/
/*   function get_hand_old finds handedness of contour */
static int get_hand_old(const struct swath *data, int *hand, int k, int i, int j, int d) {
  if (k == 0 && d == 0) {
    if (data->pings[i].bath[j] > data->pings[i].bath[j + 1])
      *hand = 1;
    else
      *hand = -1;
  }
  else if (k == 0 && d == 1) {
    if (data->pings[i].bath[j] > data->pings[i].bath[j + 1])
      *hand = -1;
    else
      *hand = 1;
  }
  else if (k == 1 && d == 0) {
    if (data->pings[i].bath[j] > data->pings[i + 1].bath[j])
      *hand = -1;
    else
      *hand = 1;
  }
  else if (k == 1 && d == 1) {
    if (data->pings[i].bath[j] > data->pings[i + 1].bath[j])
      *hand = 1;
    else
      *hand = -1;
  }
  return (true);
}
/*--------------------------------------------------------------------------*/
/*   function get_azimuth_old gets azimuth across shiptrack at ping iping */
static int get_azimuth_old(const struct swath *data, int iping, double *angle) {

  *angle = -data->pings[iping].heading;
  if (*angle > 180.0)
    *angle = *angle - 360.0;
  if (*angle < -180.0)
    *angle = *angle + 360.0;

  return (true);
}
/*--------------------------------------------------------------------------*/
/*  function mb_ocontour contours multibeam data connecting soundings
    from one ping to the next, without delauney triangulization */
static int mb_ocontour(int verbose, struct swath *data, int *error) {
  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       verbose:                 %d\n", verbose);
    fprintf(stderr, "dbg2       data:                    %p\n", data);
    fprintf(stderr, "dbg2       data->contour_algorithm: %d\n", data->contour_algorithm);
    fprintf(stderr, "dbg2       data->plot_contours:     %d\n", data->plot_contours);
    fprintf(stderr, "dbg2       data->plot_triangles:    %d\n", data->plot_triangles);
    fprintf(stderr, "dbg2       data->plot_track:        %d\n", data->plot_track);
    fprintf(stderr, "dbg2       data->plot_name:         %d\n", data->plot_name);
    fprintf(stderr, "dbg2       data->contour_int:       %f\n", data->contour_int);
    fprintf(stderr, "dbg2       data->color_int:         %f\n", data->color_int);
    fprintf(stderr, "dbg2       data->tick_int:          %f\n", data->tick_int);
    fprintf(stderr, "dbg2       data->label_int:         %f\n", data->label_int);
    fprintf(stderr, "dbg2       data->tick_len:          %f\n", data->tick_len);
    fprintf(stderr, "dbg2       data->label_hgt:         %f\n", data->label_hgt);
    fprintf(stderr, "dbg2       data->label_spacing:     %f\n", data->label_spacing);
    fprintf(stderr, "dbg2       data->ncolor:            %d\n", data->ncolor);
    fprintf(stderr, "dbg2       data->nlevel:            %d\n", data->nlevel);
    fprintf(stderr, "dbg2       data->nlevelset:         %d\n", data->nlevelset);
    if (data->nlevelset)
      for (int i = 0; i < data->nlevel; i++) {
        fprintf(stderr, "dbg2          level[%3d]:  %f %d %d %d\n", i, data->level_list[i], data->label_list[i],
                data->tick_list[i], data->color_list[i]);
      }
    fprintf(stderr, "dbg2       data->npings:     %d\n", data->npings);
    fprintf(stderr, "dbg2       data->npings_max: %d\n", data->npings_max);
    fprintf(stderr, "dbg2       data->beams_bath: %d\n", data->beams_bath);
    for (int i = 0; i < data->npings; i++) {
      fprintf(stderr, "dbg2          ping[%4d]: %4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d:%6.6d %f %f %f %f %d\n", i,
              data->pings[i].time_i[0], data->pings[i].time_i[1], data->pings[i].time_i[2], data->pings[i].time_i[3],
              data->pings[i].time_i[4], data->pings[i].time_i[5], data->pings[i].time_i[6], data->pings[i].time_d,
              data->pings[i].navlon, data->pings[i].navlat, data->pings[i].heading, data->pings[i].beams_bath);
      for (int j = 0; j < data->pings[i].beams_bath; j++) {
        if (mb_beam_ok(data->pings[i].beamflag[j]))
          fprintf(stderr, "dbg2          beam[%4d:%3d]:  %2d %f %f %f\n", i, j, data->pings[i].beamflag[j],
                  data->pings[i].bath[j], data->pings[i].bathlon[j], data->pings[i].bathlat[j]);
      }
    }
  }


  /* count number of points and verify that enough memory is allocated */
  int nsave_cnt = 0;
  for (int i = 0; i < data->npings; i++) {
    struct ping *ping = &data->pings[i];
    for (int j = 0; j < ping->beams_bath; j++) {
      if (mb_beam_ok(ping->beamflag[j]))
        nsave_cnt++;
    }
  }

  int status = MB_SUCCESS;
  if (nsave_cnt > data->nsave_alloc) {
    data->nsave_alloc = nsave_cnt;
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->xsave), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->ysave), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(int), (void **)&(data->isave), error);
    status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(int), (void **)&(data->jsave), error);
  }

  /* zero flags */
  for (int i = 0; i < data->npings; i++) {
    struct ping *ping = &data->pings[i];
    for (int j = 0; j < ping->beams_bath; j++) {
      ping->bflag[0][j] = 0;
      ping->bflag[1][j] = 0;
    }
  }

  /* get min max of bathymetry */
  double bath_min = DBL_MAX;
  double bath_max = -DBL_MAX;
  bool extreme_start = false;
  for (int i = 0; i < data->npings; i++) {
    struct ping *ping = &data->pings[i];
    for (int j = 0; j < ping->beams_bath; j++) {
      if (!extreme_start && mb_beam_ok(ping->beamflag[j])) {
        bath_min = ping->bath[j];
        bath_max = ping->bath[j];
        extreme_start = true;
      }
      if (mb_beam_ok(ping->beamflag[j])) {
        bath_min = MIN(bath_min, ping->bath[j]);
        bath_max = MAX(bath_max, ping->bath[j]);
      }
    }
  }

  /* if no depth variation don't bother */
  if ((bath_max - bath_min) < EPS)
    return (status);

  /* get number of contour intervals */
  if (!data->nlevelset) {
    if (data->nlevel > 0) {
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->level_list, error);
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->color_list, error);
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->label_list, error);
      mb_freed(verbose, __FILE__, __LINE__, (void **)&data->tick_list, error);
    }
    const int nci = bath_min / data->contour_int + 1;
    const int ncf = bath_max / data->contour_int + 1;
    data->nlevel = ncf - nci;
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(double), (void **)&(data->level_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(int), (void **)&(data->color_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(int), (void **)&(data->label_list), error);
    status &= mb_mallocd(verbose, __FILE__, __LINE__, data->nlevel * sizeof(int), (void **)&(data->tick_list), error);
    if (*error != MB_ERROR_NO_ERROR)
      return (status);
    for (int i = 0; i < data->nlevel; i++) {
      const int k = nci + i;
      data->level_list[i] = k * data->contour_int;
      data->color_list[i] = (int)(data->level_list[i] / data->color_int) % data->ncolor;
      if (data->tick_int <= 0.0)
        data->tick_list[i] = 0;
      else {
        const double ratio = data->level_list[i] / data->tick_int;
        if (fabs(round(ratio) - ratio) < 0.005 * data->contour_int)
          data->tick_list[i] = 1;
        else
          data->tick_list[i] = 0;
      }
      if (data->label_int <= 0.0)
        data->label_list[i] = 0;
      else {
        const double ratio = data->level_list[i] / data->label_int;
        if (fabs(round(ratio) - ratio) < 0.005 * data->contour_int)
          data->label_list[i] = 1;
        else
          data->label_list[i] = 0;
      }
    }
  }

  if (verbose >= 4) {
    fprintf(stderr, "\ndbg4  Data points:\n");
    fprintf(stderr, "dbg4       nlevel:           %d\n", data->nlevel);
    fprintf(stderr, "dbg4       i level color tick label:\n");
    for (int i = 0; i < data->nlevel; i++)
      fprintf(stderr, "dbg4       %d %f %d %d %d\n", i, data->level_list[i], data->color_list[i], data->tick_list[i],
              data->label_list[i]);
  }

  /* make sure that no depths are exact contour values */
  const double eps = EPS * (bath_max - bath_min);
  for (int k = 0; k < data->nlevel; k++) {
    for (int i = 0; i < data->npts; i++) {
      if (fabs(data->z[i] - data->level_list[k]) < eps)
        data->z[i] = data->level_list[k] + eps;
    }
  }

  /* loop over all of the contour values */
  data->nsave = 0;
  data->nlabel = 0;
  if (status == MB_SUCCESS && data->plot_contours)
    for (int ival = 0; ival < data->nlevel; ival++) {
      const double value = data->level_list[ival];
      data->contour_newpen(data->color_list[ival]);
      const int tick = data->tick_list[ival];
      const int label = data->label_list[ival];

      if (verbose >= 4) {
        fprintf(stderr, "\ndbg4  About to contour level in function <%s>\n", __func__);
        fprintf(stderr, "dbg4       value:         %f\n", value);
        fprintf(stderr, "dbg4       tick:          %d\n", tick);
        fprintf(stderr, "dbg4       label:         %d\n", label);
      }

      /* flag all grid sides crossed by the current contour */
      char *beamflag2 = NULL;
      double *bath2 = NULL;
      for (int i = 0; i < data->npings; i++) {
        char *beamflag1 = data->pings[i].beamflag;
        beamflag2 = NULL;
        double *bath1 = data->pings[i].bath;
        bath2 = NULL;
        int beams_bath_use = data->pings[i].beams_bath;
        if (i < data->npings - 1) {
          beamflag2 = data->pings[i + 1].beamflag;
          bath2 = data->pings[i + 1].bath;
          beams_bath_use = MIN(beams_bath_use, data->pings[i + 1].beams_bath);
        }
        for (int j = 0; j < beams_bath_use; j++) {
          /* check for across track intersection */
          if (j < beams_bath_use - 1)
            if ((mb_beam_ok(beamflag1[j]) && mb_beam_ok(beamflag1[j + 1])) &&
                ((bath1[j] < value && bath1[j + 1] > value) || (bath1[j] > value && bath1[j + 1] < value)))
              data->pings[i].bflag[0][j] = 1;

          /* check for along track intersection */
          if (i < data->npings - 1)
            if ((mb_beam_ok(beamflag1[j]) && mb_beam_ok(beamflag2[j])) &&
                ((bath1[j] < value && bath2[j] > value) || (bath1[j] > value && bath2[j] < value)))
              data->pings[i].bflag[1][j] = 1;
        }
      }

      /* loop until all flagged points have been unflagged */
      int i = 0;
      int k = 0;
      int j = 0;
      int d = 0;
      int closed = 0;
      while (get_start_old(data, &k, &i, &j, &d, &closed)) {
        /* if not closed remove from flag list */
        if (closed == 0)
          data->pings[i].bflag[k][j] = 0;

        /* get position and handedness */
        double x;
        double y;
        get_pos_old(data, eps, &x, &y, k, i, j, value);
        data->xsave[0] = x;
        data->ysave[0] = y;
        data->isave[0] = i;
        data->jsave[0] = j;
        data->nsave = 1;
        data->nlabel = 0;
        const int ibeg = i;
        const int jbeg = j;
        const int kbeg = k;
        const int dbeg = d;

        /* set tick flag */
        bool tick_last = false;

        /* look for next component */
        int hand;
        int nk;
        int ni;
        int nj;
        int nd;
        while (get_next_old(data, &nk, &ni, &nj, &nd, k, i, j, d, kbeg, ibeg, jbeg, dbeg, &closed)) {
		  
		  /* check that large enough xsave and ysave arrays have been allocated */
		  if (data->nsave >= data->nsave_alloc - 4) {
			  data->nsave_alloc += 8192;
			  status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->xsave), error);
			  status &= mb_reallocd(verbose, __FILE__, __LINE__, data->nsave_alloc * sizeof(double), (void **)&(data->ysave), error);
		  }

          /* get position */
          get_pos_old(data, eps, &x, &y, nk, ni, nj, value);
          get_hand_old(data, &hand, k, i, j, d);
          if (tick && !tick_last) {
            data->xsave[data->nsave] = 0.5 * (x + data->xsave[data->nsave - 1]);
            data->ysave[data->nsave] = 0.5 * (y + data->ysave[data->nsave - 1]);
            const double magdis =
                sqrt(pow((x - data->xsave[data->nsave - 1]), 2.0) + pow((y - data->ysave[data->nsave - 1]), 2.0));
            if (magdis > 0.0) {
              data->xsave[data->nsave + 1] =
                  data->xsave[data->nsave] - hand * data->tick_len * (y - data->ysave[data->nsave - 1]) / magdis;
              data->ysave[data->nsave + 1] =
                  data->ysave[data->nsave] + hand * data->tick_len * (x - data->xsave[data->nsave - 1]) / magdis;
            }
            else {
              data->xsave[data->nsave + 1] = data->xsave[data->nsave];
              data->ysave[data->nsave + 1] = data->ysave[data->nsave];
            }
            data->xsave[data->nsave + 2] = data->xsave[data->nsave];
            data->ysave[data->nsave + 2] = data->ysave[data->nsave];
            data->xsave[data->nsave + 3] = x;
            data->ysave[data->nsave + 3] = y;
            data->isave[data->nsave] = ni;
            data->jsave[data->nsave] = nj;
            data->isave[data->nsave + 1] = ni;
            data->jsave[data->nsave + 1] = nj;
            data->isave[data->nsave + 2] = ni;
            data->jsave[data->nsave + 2] = nj;
            data->isave[data->nsave + 3] = ni;
            data->jsave[data->nsave + 3] = nj;
            data->nsave = data->nsave + 4;
            tick_last = true;
          }
          else {
            data->xsave[data->nsave] = x;
            data->ysave[data->nsave] = y;
            data->isave[data->nsave] = ni;
            data->jsave[data->nsave] = nj;
            data->nsave++;
            tick_last = false;
          }
          i = ni;
          j = nj;
          k = nk;
          d = nd;
          data->pings[i].bflag[k][j] = 0;
        }

        /* clean up if not a full contour */
        if (data->nsave < 2) {
          data->nsave = 0;
          data->pings[i].bflag[k][j] = 0;
        }

        /* set labels if needed */
        if (data->nsave > 0 && label && !closed) {
          /* check beginning of contour */
          int left = data->pings[data->isave[0]].beams_bath / 2;
          int right = data->pings[data->isave[0]].beams_bath / 2;
          for (int jj = 0; jj < data->beams_bath; jj++) {
            if (mb_beam_ok(data->pings[data->isave[0]].beamflag[jj])) {
              if (jj < left)
                left = jj;
              if (jj > right)
                right = jj;
            }
          }
          if (data->jsave[0] == left || data->jsave[0] == left + 1) {
            data->xlabel[data->nlabel] = data->xsave[0];
            data->ylabel[data->nlabel] = data->ysave[0];
            get_azimuth_old(data, data->isave[0], &data->angle[data->nlabel]);
            data->justify[data->nlabel] = 1;
            if (check_label(data, data->nlabel))
              data->nlabel++;
          }
          else if (data->jsave[0] == right || data->jsave[0] == right - 1) {
            data->xlabel[data->nlabel] = data->xsave[0];
            data->ylabel[data->nlabel] = data->ysave[0];
            get_azimuth_old(data, data->isave[0], &data->angle[data->nlabel]);
            data->justify[data->nlabel] = 0;
            if (check_label(data, data->nlabel))
              data->nlabel++;
          }

          /* check end of contour */
          left = data->pings[data->isave[data->nsave - 1]].beams_bath / 2;
          right = data->pings[data->isave[data->nsave - 1]].beams_bath / 2;
          for (int jj = 0; jj < data->pings[data->isave[data->nsave - 1]].beams_bath; jj++) {
            if (mb_beam_ok(data->pings[data->isave[data->nsave - 1]].beamflag[jj])) {
              if (jj < left)
                left = jj;
              if (jj > right)
                right = jj;
            }
          }
          if ((data->nlabel == 0 || data->nsave > 10) &&
              (data->jsave[data->nsave - 1] == left || data->jsave[data->nsave - 1] == left + 1)) {
            data->xlabel[data->nlabel] = data->xsave[data->nsave - 1];
            data->ylabel[data->nlabel] = data->ysave[data->nsave - 1];
            get_azimuth_old(data, data->isave[data->nsave - 1], &data->angle[data->nlabel]);
            data->justify[data->nlabel] = 1;
            if (check_label(data, data->nlabel))
              data->nlabel++;
          }
          else if ((data->nlabel == 0 || data->nsave > 10) &&
                   (data->jsave[data->nsave - 1] == right || data->jsave[data->nsave - 1] == right - 1)) {
            data->xlabel[data->nlabel] = data->xsave[data->nsave - 1];
            data->ylabel[data->nlabel] = data->ysave[data->nsave - 1];
            get_azimuth_old(data, data->isave[data->nsave - 1], &data->angle[data->nlabel]);
            data->justify[data->nlabel] = 0;
            if (check_label(data, data->nlabel))
              data->nlabel++;
          }
        }

        /* dump the contour */
        dump_contour(data, value);
      }

      /* done with contouring this level */
    }

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
    fprintf(stderr, "dbg2  Return values:\n");
    fprintf(stderr, "dbg2       error:      %d\n", *error);
    fprintf(stderr, "dbg2  Return status:\n");
    fprintf(stderr, "dbg2       status:     %d\n", status);
  }

  return (MB_SUCCESS);
}
/*--------------------------------------------------------------------------*/
/*   function mb_contour calls the appropriate contouring routine. */
int mb_contour(int verbose, struct swath *data, int *error) {
  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> called\n", __func__);
    fprintf(stderr, "dbg2  Input arguments:\n");
    fprintf(stderr, "dbg2       verbose:                 %d\n", verbose);
    fprintf(stderr, "dbg2       data:                    %p\n", data);
    fprintf(stderr, "dbg2       data->contour_alg:       %d\n", data->contour_algorithm);
  }

  /* call the appropriate contouring routine */
  int status = MB_SUCCESS;
  if (data->contour_algorithm == MB_CONTOUR_TRIANGLES) {
    if (data->ntri <= 0) {
      status &= mb_triangulate(verbose, data, error);
    }
    status &= mb_tcontour(verbose, data, error);
    }
  else
    status &= mb_ocontour(verbose, data, error);

  if (verbose >= 2) {
    fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
    fprintf(stderr, "dbg2  Return values:\n");
    fprintf(stderr, "dbg2       error:      %d\n", *error);
    fprintf(stderr, "dbg2  Return status:\n");
    fprintf(stderr, "dbg2       status:     %d\n", status);
  }

  return (status);
}

/*--------------------------------------------------------------------------*/

/*==================================== mb_delaun.c ====================================*/
/*--------------------------------------------------------------------
 *    The MB-system:	mb_delaun.c	4/19/94
 *
 *    Copyright (c) 1994-2025 by
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
 * The function mb_delaun.c assigns triangles to a set of x,y points
 * based on the method of:
 *	Watson, Computers and Geosciences, V8, 97-101, 1982.
 * The final network is a set of Delauney triangles with the property
 * that no vertex lies inside the circumcircle of any triangle.  This
 * system is as close to equiangular as possible.
 * This code was translated from a Fortran 77 subroutine obtained from
 * Robert Parker at the Scripps Institution of Oceanography.  It is
 * unclear whether Bob wrote the subroutine based on the Watson article
 * or obtained the code from someone else.
 *
 * The input values are:
 *   verbose:		verbosity of debug output (MBIO convention)
 *   npts:		number of input x,y points
 *   p1[npts+3]:	array of npts input x values
 *   p2[npts+3]:	array of npts input y values
 *   ed[npts+3]:	array of edge flags - points on the edge
 *			of the region are flagged by nonzero values
 *			- interior points are flagged by zero values
 *			- triangles constructed using three edge points
 *			are removed
 *
 * The output values are:
 *   ntri:		number of output triangles
 *   iv1[2*npts+1]:	array of indices of points forming the first vertex
 *			of the triangles
 *   iv2[2*npts+1]:	array of indices of points forming the second vertex
 *			of the triangles
 *   iv3[2*npts+1]:	array of indices of points forming the third vertex
 *			of the triangles
 *   ct1[2*npts+1]:	triangle connection array, value ct1[i] indicates which
 *			triangle connects to side 1 of triangle i, value
 *			of -1 indicates no connecting triangle
 *   ct2[2*npts+1]:	triangle connection array, value ct2[i] indicates which
 *			triangle connects to side 2 of triangle i
 *			of -1 indicates no connecting triangle
 *   ct3[2*npts+1]:	triangle connection array, value ct3[i] indicates which
 *			triangle connects to side 3 of triangle i
 *			of -1 indicates no connecting triangle
 *   cs1[2*npts+1]:	triangle connection array, value cs1[i] indicates which
 *			side of triangle ct1[i] connects to side 1 of triangle i
 *   cs2[2*npts+1]:	triangle connection array, value cs2[i] indicates which
 *			side of triangle ct2[i] connects to side 2 of triangle i
 *   cs3[2*npts+1]:	triangle connection array, value cs3[i] indicates which
 *			side of triangle ct3[i] connects to side 3 of triangle i
 *   error:		error value, MBIO convention
 *
 * The work arrays are passed into mb_delaun rather than allocated and
 * deallocated within mb_delaun to increase the efficiency of programs
 * which use mb_delaun repeatedly. These work arrays are:
 *   v1[2*npts+1]:	the value v1[i] stores the x value of the
 *			circumcenter of triangle i
 *   v2[2*npts+1]:	the value v2[i] stores the y value of the
 *			circumcenter of triangle i
 *   v3[2*npts+1]:	the value v3[i] stores the square of the radius
 *			of the circumcircle of triangle i
 *   istack[2*npts+1]:	This is a stack onto which is pushed the the
 *			indexes of all the triangles that need to be
 *			replaced as a result of the addition of a new
 *			point. During the construction of the new 3-tuples
 *			the index of the 3-tuple to be replaced is
 *			popped from the stack. Since the addition of each
 *			new point adds 2 more triangles to the total,
 *			istack is popped 2 more times than it is pushed
 *			for each additional point. Hence istack is
 *			initialized prior to the main part of the routine
 *			so that it correctly returns the indexes of the
 *			new triangles.
 *    kv1[6*npts+1]:	For each new point, each existing triangle is
 *    kv2[6*npts+1]:	tested in turn and replaced if the new point
 *			lies within its circumcircle. Each side of the
 *			triangle is pushed onto the arrays kv1 and kv2 to
 *			potentially form a triangle with the new point.
 *			HOWEVER, if the side is already on the kv stack it
 *			is removed from the stack.  This means that if the
 *			side is common to two triangles that are to be
 *			replaced, i.e an interior side, then it is not
 *			used as the basis for forming the new triangles.
 *
 * Author:	D. W. Caress
 * Date:	April, 1994
 */


/*--------------------------------------------------------------------------*/
/* 	function mb_delaun creates a network of triangles connecting an
    input set of points, where the triangles are as close to equiangular
    as possible. */
int mb_delaun(int verbose, int npts, double *p1, double *p2, int *ed, int *ntri, int *iv1, int *iv2, int *iv3, int *ct1, int *ct2,
              int *ct3, int *cs1, int *cs2, int *cs3, double *v1, double *v2, double *v3, int *istack, int *kv1, int *kv2,
              int *error) {
	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBBA function <%s> called\n", __func__);
		fprintf(stderr, "dbg2  Input arguments:\n");
		fprintf(stderr, "dbg2       verbose:          %d\n", verbose);
		fprintf(stderr, "dbg2       npts:             %d\n", npts);
		fprintf(stderr, "dbg2       p1:               %p\n", p1);
		fprintf(stderr, "dbg2       p2:               %p\n", p2);
		fprintf(stderr, "dbg2       ed:               %p\n", ed);
		fprintf(stderr, "dbg2       ntri:             %d\n", *ntri);
		fprintf(stderr, "dbg2       iv1:              %p\n", iv1);
		fprintf(stderr, "dbg2       iv2:              %p\n", iv2);
		fprintf(stderr, "dbg2       iv3:              %p\n", iv3);
		fprintf(stderr, "dbg2       ct1:              %p\n", ct1);
		fprintf(stderr, "dbg2       ct2:              %p\n", ct2);
		fprintf(stderr, "dbg2       ct3:              %p\n", ct3);
		fprintf(stderr, "dbg2       cs1:              %p\n", cs1);
		fprintf(stderr, "dbg2       cs2:              %p\n", cs2);
		fprintf(stderr, "dbg2       cs3:              %p\n", cs3);
		fprintf(stderr, "dbg2       v1:               %p\n", v1);
		fprintf(stderr, "dbg2       v2:               %p\n", v2);
		fprintf(stderr, "dbg2       v3:               %p\n", v3);
		fprintf(stderr, "dbg2       istack:           %p\n", istack);
		fprintf(stderr, "dbg2       kv1:              %p\n", kv1);
		fprintf(stderr, "dbg2       kv2:              %p\n", kv2);
		fprintf(stderr, "dbg2       error:            %d\n", *error);
		if (verbose >= 5) {
			fprintf(stderr, "dbg5       Input data:\n");
			for (int i = 0; i < npts; i++)
				fprintf(stderr, "dbg5       %d %f %f\n", i, p1[i], p2[i]);
		}
	}

	int itemp[2][3];
	itemp[0][0] = 1;
	itemp[1][0] = 2;
	itemp[0][1] = 1;
	itemp[1][1] = 3;
	itemp[0][2] = 2;
	itemp[1][2] = 3;

	/* initialize the triangle stack */
	int n1 = 2 * npts + 3;
	for (int i = 0; i < n1; i++)
		istack[i] = i;

	/* determine the extremes of the data */
	double xmin = p1[0];
	double xmax = p1[0];
	double ymin = p2[0];
	double ymax = p2[0];
	for (int i = 0; i < npts; i++) {
		xmin = MIN(xmin, p1[i]);
		xmax = MAX(xmax, p1[i]);
		ymin = MIN(ymin, p2[i]);
		ymax = MAX(ymax, p2[i]);
	}

	/* enclose the data region in an equilateral triangle
	    - put circumcenter and radius-squared into the list */
	double cx = xmax - xmin;
	double cy = ymax - ymin;
	const double crsq = 1.2 * (cx * cx + cy * cy);
	cx = 0.5 * (xmin + xmax);
	cy = 0.5 * (ymin + ymax);
	v1[0] = cx;
	v2[0] = cy;
	v3[0] = crsq;

	/* put vertex coordinates in the end of the p array */
	const double rad = sqrt(v3[0]);
	/* const int maxn = npts + 3; */
	for (int i = 0; i < 3; i++) {
		p1[npts + 2 - i] = v1[0] + rad * cos(2.0944 * (i + 1));
		p2[npts + 2 - i] = v2[0] + rad * sin(2.0944 * (i + 1));
	}
	iv1[0] = npts + 2;
	iv2[0] = npts + 1;
	iv3[0] = npts;

	/* scan through the data backwards */
	int isp = 1;
	int id = 1;

	int status = MB_SUCCESS;

	for (int nuc = npts - 1; nuc > -1; nuc--) {
		int km = 0;

		/* loop through the established 3-tuples */
		for (int jt = 0; jt < isp; jt++) {
			const int i1 = iv3[jt];
			/* calculate the distance of the
			point from the jt circumcenter */
			const double rsq = (p1[nuc] - p1[i1]) * (p1[nuc] + p1[i1] - 2 * v1[jt]) + (p2[nuc] - p2[i1]) * (p2[nuc] + p2[i1] - 2 * v2[jt]);

			/* If the point lies within circumcircle of triangle
			(3-tuple) jt then delete this triangle. Save the
			edges provided that they are not part of another
			triangle for which the point lies within the circumcircle
			Thus "interior" edges are eliminated prior to
			construction of the new triangles (3 tuples). */
			if (rsq <= 0.0) {
				/* triangle needs replacing => push the index on the stack */
				id = id - 1;
				istack[id] = jt;

				/* add edges to kv but delete if already present */
				for (int i = 0; i < 3; i++) {
					/* cycle through the edges of the triangle */
					const int l1 = itemp[0][i];
					const int l2 = itemp[1][i];
					int *ivs1 = NULL;
					int *ivs2 = NULL;
					if (l1 == 1)
						ivs1 = iv1;
					if (l1 == 2)
						ivs1 = iv2;
					if (l1 == 3)
						ivs1 = iv3;
					if (l2 == 1)
						ivs2 = iv1;
					if (l2 == 2)
						ivs2 = iv2;
					if (l2 == 3)
						ivs2 = iv3;
					bool addside = true;

					/* Check if the side is already stored in kv. If it
					    is then Side common to more than one of the
					    current triangles. So remove it from list. */
					int j = 0;
					while ((j < km) && addside) {
						if (ivs1[jt] == kv1[j] && ivs2[jt] == kv2[j]) {
							addside = false;
							km--;
							for (int k = j; k < km; k++) {
								kv1[k] = kv1[k + 1];
								kv2[k] = kv2[k + 1];
							}
						}
						j++;
					}

					/* side was not found in list so add it now */
					if (addside) {
						km++;
						if (km > 6 * npts + 1) {
							fprintf(stderr, "Error in mb_delaun - kv array too small at dimension %d\n", 6 * npts + 1);
							*error = 99;
							status = MB_FAILURE;
							return (status);
						}
						kv1[km - 1] = ivs1[jt];
						kv2[km - 1] = ivs2[jt];
					}
				} /* end: for (i=0;i<3;i++) */
			}     /* end: if (rsq < 0.0) */

		} /* end: for (jt=0;jt<isp;jt++) */

		/* form new 3-tuples */
		for (int i = 0; i < km; i++) {
			/* pop the triangle index from the stack */
			int kt = istack[id];
			id++;

			/* calculate the circumcircle and radius squared */
			const int i1 = kv1[i];
			const int i2 = kv2[i];
			const double denom = ((p1[i1] - p1[nuc]) * (p2[i2] - p2[nuc]) - (p2[i1] - p2[nuc]) * (p1[i2] - p1[nuc]));

			/* check if the three points form a true triangle */
			if (denom != 0.0) {
				const double s =
				    ((p1[i1] - p1[nuc]) * (p1[i1] - p1[i2]) + (p2[i1] - p2[nuc]) * (p2[i1] - p2[i2])) / denom;
				v1[kt] = 0.5 * (p1[i2] + p1[nuc] + s * (p2[i2] - p2[nuc]));
				v2[kt] = 0.5 * (p2[i2] + p2[nuc] - s * (p1[i2] - p1[nuc]));
			}

			/* The three points are degenerate and lie on a line.
			The "true" circumcenter is at infinity and the radius
			is also infinite. All points on the plane lie within
			the circumcircle. Thus simulate this by using
			circumcenter and circumcircle of the enclosing
			equilateral triangle */
			else {
				fprintf(stderr, "\nmb_delaun Warning: three linear points cannot be a triangle\n");
				fprintf(stderr, "%d %f %f\n", i1, p1[i1], p2[i1]);
				fprintf(stderr, "%d %f %f\n", i2, p1[i2], p2[i2]);
				fprintf(stderr, "%d %f %f\n", nuc, p1[nuc], p2[nuc]);
				v1[kt] = cx;
				v2[kt] = cy;
				v3[kt] = nuc;
			}
			iv1[kt] = kv1[i];
			iv2[kt] = kv2[i];
			iv3[kt] = nuc;
		} /* end: for (i=0;i<km;i++) */

		/* adding a point adds two more triangles to the total
		  number of triangles */
		isp = isp + 2;

	} /* end: for (nuc=npts-1;nuc>-1;nuc--) */

	/* set number of triangles */
	*ntri = isp;

	/* remove triangles using added points, triangles made
	    up of three flagged edge points, and degenerate triangles */
	for (int i = *ntri - 1; i > -1; i--) {
		if (iv1[i] >= npts || iv2[i] >= npts || iv3[i] >= npts) {
			for (int j = i; j < isp - 1; j++) {
				iv1[j] = iv1[j + 1];
				iv2[j] = iv2[j + 1];
				iv3[j] = iv3[j + 1];
			}
			isp--;
		}
		else if (ed[iv1[i]] != 0 && ed[iv2[i]] != 0 && ed[iv3[i]] != 0) {
			for (int j = i; j < isp - 1; j++) {
				iv1[j] = iv1[j + 1];
				iv2[j] = iv2[j + 1];
				iv3[j] = iv3[j + 1];
			}
			isp--;
		}
		if (iv1[i] == iv2[i] || iv2[i] == iv3[i] || iv3[i] == iv1[i]) {
fprintf(stderr, "%s:%d:%s: Removing degenerate triangle: %d %d %d\n", 
__FILE__, __LINE__, __FUNCTION__, iv1[i], iv2[i], iv3[i]);
			for (int j = i; j < isp - 1; j++) {
				iv1[j] = iv1[j + 1];
				iv2[j] = iv2[j + 1];
				iv3[j] = iv3[j + 1];
			}
			isp--;
		}
	}
	*ntri = isp;

	/* make sure all triangles are defined clockwise */
	for (int i = 0; i < *ntri; i++) {
		const double xproduct =
		    -(p1[iv2[i]] - p1[iv1[i]]) * (p2[iv3[i]] - p2[iv2[i]]) + (p1[iv3[i]] - p1[iv2[i]]) * (p2[iv2[i]] - p2[iv1[i]]);
		if (xproduct < 0.0) {
			int j = iv2[i];
			iv2[i] = iv3[i];
			iv3[i] = j;
		}
	}

	/* now get connectivity */
	for (int i = 0; i < *ntri; i++) {
		ct1[i] = -1;
		ct2[i] = -1;
		ct3[i] = -1;
		cs1[i] = -1;
		cs2[i] = -1;
		cs3[i] = -1;
	}
	for (int i = 0; i < *ntri; i++) {
		/* check side 1 of triangle i */
		if (ct1[i] == -1) {
			bool notfound = true;
			for (int j = 0; notfound && j < *ntri; j++) {
				if (notfound && iv1[i] == iv2[j] && iv2[i] == iv1[j]) {
					ct1[i] = j;
					cs1[i] = 0;
					ct1[j] = i;
					cs1[j] = 0;
					notfound = false;
				}
				if (notfound && iv1[i] == iv3[j] && iv2[i] == iv2[j]) {
					ct1[i] = j;
					cs1[i] = 1;
					ct2[j] = i;
					cs2[j] = 0;
					notfound = false;
				}
				if (notfound && iv1[i] == iv1[j] && iv2[i] == iv3[j]) {
					ct1[i] = j;
					cs1[i] = 2;
					ct3[j] = i;
					cs3[j] = 0;
					notfound = false;
				}
			}
		}
		/* check side 2 of triangle i */
		if (ct2[i] == -1) {
			bool notfound = true;
			for (int j = 0; notfound && j < *ntri; j++) {
				if (notfound && iv2[i] == iv2[j] && iv3[i] == iv1[j]) {
					ct2[i] = j;
					cs2[i] = 0;
					ct1[j] = i;
					cs1[j] = 1;
					notfound = false;
				}
				if (notfound && iv2[i] == iv3[j] && iv3[i] == iv2[j]) {
					ct2[i] = j;
					cs2[i] = 1;
					ct2[j] = i;
					cs2[j] = 1;
					notfound = false;
				}
				if (notfound && iv2[i] == iv1[j] && iv3[i] == iv3[j]) {
					ct2[i] = j;
					cs2[i] = 2;
					ct3[j] = i;
					cs3[j] = 1;
					notfound = false;
				}
			}
		}
		/* check side 3 of triangle i */
		if (ct3[i] == -1) {
			bool notfound = true;
			for (int j = 0; notfound && j < *ntri; j++) {
				if (notfound && iv3[i] == iv2[j] && iv1[i] == iv1[j]) {
					ct3[i] = j;
					cs3[i] = 0;
					ct1[j] = i;
					cs1[j] = 2;
					notfound = false;
				}
				if (notfound && iv3[i] == iv3[j] && iv1[i] == iv2[j]) {
					ct3[i] = j;
					cs3[i] = 1;
					ct2[j] = i;
					cs2[j] = 2;
					notfound = false;
				}
				if (notfound && iv3[i] == iv1[j] && iv1[i] == iv3[j]) {
					ct3[i] = j;
					cs3[i] = 2;
					ct3[j] = i;
					cs3[j] = 2;
					notfound = false;
				}
			}
		}
	}

	if (verbose >= 2) {
		fprintf(stderr, "\ndbg2  MBIO function <%s> completed\n", __func__);
		fprintf(stderr, "dbg2  Return values:\n");
		fprintf(stderr, "dbg2       ntri:             %d\n", *ntri);
		if (verbose >= 5) {
			fprintf(stderr, "dbg5       Output vertices:\n");
			for (int i = 0; i < *ntri; i++)
				fprintf(stderr, "dbg5       %3d  %3d %3d %3d\n", i, iv1[i], iv2[i], iv3[i]);
			fprintf(stderr, "dbg5       Output connectivity:\n");
			for (int i = 0; i < *ntri; i++)
				fprintf(stderr, "dbg5       %3d   %3d %3d   %3d %3d   %3d %3d\n", i, ct1[i], cs1[i], ct2[i], cs2[i], ct3[i],
				        cs3[i]);
		}
		fprintf(stderr, "dbg2       error:      %d\n", *error);
		fprintf(stderr, "dbg2  Return status:\n");
		fprintf(stderr, "dbg2       status:     %d\n", status);
	}

	return (status);
}
/*--------------------------------------------------------------------------*/

/*==================================== mb_cheb.c (BCLS) ===============================*/
/*
 * The mb_cheb library is a least squares matrix solver using
 * Richardson's algorithm with chebyshev acceleration. The step size is
 * varied to obtain uniform convergence over a prescribed
 * range of eigenvalues. This code is a C translation of Fortran 77
 * code distributed by Dr. Allen H. Olson in 1987 following the
 * publication of
 *   Olson, A. H., A Chebyshev condition for accelerating
 *   convergance of iterative tomographic methods - Solving
 *   large least squares problems, Phys. Earth Planet. Inter.,
 *   47, 333-345, 1987.
 * This algorithm is both time and memory efficient for large,
 * sparse least squares problems. The instructions for use are
 * given in comments below.
 *
 *
 *
 *   This C version of LSQR was first created by
 *      Michael P Friedlander <mpf@cs.ubc.ca>
 *   as part of his BCLS package:
 *      http://www.cs.ubc.ca/~mpf/bcls/index.html.
 *  The present file is maintained by
 *      Michael Saunders <saunders@stanford.edu>
 *
 *   31 Aug 2007: First version of this file lsqr.c obtained from
 *                Michael Friedlander's BCLS package, svn version number
 *                $Revision: 273 $ $Date: 2006-09-04 15:59:04 -0700 (Mon, 04 Sep 2006) $
 *
 *                The stopping rules were slightly altered in that version.
 *                They have been restored to the original rules used in the f77 LSQR.
 *
 *   13 September 2016
 *                The lsqr library was added to MB-System by David Caress
 *                after accessing the source code at
 *                    http://web.stanford.edu/group/SOL/software/lsqr/
 *
 * Author:	D. W. Caress
 * Date:	March 23, 2000
 *
 */

/* bccblas.c
   $Revision: 231 $ $Date: 2006-04-15 18:47:05 -0700 (Sat, 15 Apr 2006) $

   ----------------------------------------------------------------------
   This file is part of BCLS (Bound-Constrained Least Squares).

   Copyright (C) 2006 Michael P. Friedlander, Department of Computer
   Science, University of British Columbia, Canada. All rights
   reserved. E-mail: <mpf@cs.ubc.ca>.

   BCLS is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   BCLS is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General
   Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with BCLS; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301
   USA
   ----------------------------------------------------------------------
*/
/*!
   \file

   This file contains C-wrappers to the BLAS (Basic Linear Algebra
   Subprograms) routines that are used by BCLS.  Whenever possible,
   they should be replaced by corresponding BLAS routines that have
   been optimized to the machine being used.

   Included BLAS routines:

   - mbcblas_daxpy
   - mbcblas_dcopy
   - mbcblas_ddot
   - mbcblas_dnrm2
   - mbcblas_dscal
*/

/*!
  \param[in]     N
  \param[in]     alpha
  \param[in]     X
  \param[in]     incX
  \param[in,out] Y
  \param[in]     incY
*/
static void mbcblas_daxpy(const int N, const double alpha, const double *X, const int incX, double *Y, const int incY) {
	if (N <= 0)
		return;
	if (alpha == 0.0)
		return;

	if (incX == 1 && incY == 1) {
		const int m = N % 4;

		for (int i = 0; i < m; i++)
			Y[i] += alpha * X[i];

		for (int i = m; i + 3 < N; i += 4) {
			Y[i] += alpha * X[i];
			Y[i + 1] += alpha * X[i + 1];
			Y[i + 2] += alpha * X[i + 2];
			Y[i + 3] += alpha * X[i + 3];
		}
	}
	else {
		int ix = MBCBLAS_OFFSET(N, incX);
		int iy = MBCBLAS_OFFSET(N, incY);

		for (int i = 0; i < N; i++) {
			Y[iy] += alpha * X[ix];
			ix += incX;
			iy += incY;
		}
	}
}

/*!
  \param[in]     N
  \param[in]     X
  \param[in]     incX
  \param[out]    Y
  \param[in]     incY
*/
static void mbcblas_dcopy(const int N, const double *X, const int incX, double *Y, const int incY) {
	int ix = MBCBLAS_OFFSET(N, incX);
	int iy = MBCBLAS_OFFSET(N, incY);

	for (int i = 0; i < N; i++) {
		Y[iy] = X[ix];
		ix += incX;
		iy += incY;
	}
}

/*!
  \param[in]     N
  \param[in]     X
  \param[in]     incX
  \param[in]     Y
  \param[in]     incY

  \return  Dot product of X and Y.

*/
static double mbcblas_ddot(const int N, const double *X, const int incX, const double *Y, const int incY) {
	double r = 0.0;
	int ix = MBCBLAS_OFFSET(N, incX);
	int iy = MBCBLAS_OFFSET(N, incY);

	for (int i = 0; i < N; i++) {
		r += X[ix] * Y[iy];
		ix += incX;
		iy += incY;
	}

	return r;
}

/*!
  \param[in]     N
  \param[in]     X
  \param[in]     incX

  \return Two-norm of X.
*/
static double mbcblas_dnrm2(const int N, const double *X, const int incX) {
	if (N <= 0 || incX <= 0)
		return 0;
	else if (N == 1)
		return fabs(X[0]);

	double scale = 0.0;
	double ssq = 1.0;
	int ix = 0;
	for (int i = 0; i < N; i++) {
		const double x = X[ix];

		if (x != 0.0) {
			const double ax = fabs(x);

			if (scale < ax) {
				ssq = 1.0 + ssq * (scale / ax) * (scale / ax);
				scale = ax;
			} else {
				ssq += (ax / scale) * (ax / scale);
			}
		}

		ix += incX;
	}

	return scale * sqrt(ssq);
}

/*!
  \param[in]     N
  \param[in]     alpha
  \param[in,out] X
  \param[in]     incX
*/
static void mbcblas_dscal(const int N, const double alpha, double *X, const int incX) {
	if (incX <= 0)
		return;

	int ix = MBCBLAS_OFFSET(N, incX);

	for (int i = 0; i < N; i++) {
		X[ix] *= alpha;
		ix += incX;
	}
}

/*----------------------------------------------------------------------*/
// START LSQR SOURCE CODE
//
// ---------------------------------------------------------------------
// d2norm  returns  sqrt( a**2 + b**2 )  with precautions
// to avoid overflow.
//
// 21 Mar 1990: First version.
// ---------------------------------------------------------------------
static double mblsqr_d2norm(const double a, const double b) {
	const double zero = 0.0;

	double scale = fabs(a) + fabs(b);
	if (scale == zero)
		return zero;
	else
		return scale * sqrt((a / scale) * (a / scale) + (b / scale) * (b / scale));
}

static void mblsqr_dload(const int n, const double alpha, double x[]) {
	for (int i = 0; i < n; i++)
		x[i] = alpha;
	return;
}

// LSQR  finds a solution x to the following problems:
//
// 1. Unsymmetric equations --    solve  A*x = b
//
// 2. Linear least squares  --    solve  A*x = b
//                                in the least-squares sense
//
// 3. Damped least squares  --    solve  (   A    )*x = ( b )
//                                       ( damp*I )     ( 0 )
//                                in the least-squares sense
//
// where A is a matrix with m rows and n columns, b is an
// m-vector, and damp is a scalar.  (All quantities are real.)
// The matrix A is intended to be large and sparse.  It is accessed
// by means of subroutine calls of the form
//
//            aprod ( mode, m, n, x, y, UsrWrk )
//
// which must perform the following functions:
//
//            If mode = 1, compute  y = y + A*x.
//            If mode = 2, compute  x = x + A(transpose)*y.
//
// The vectors x and y are input parameters in both cases.
// If  mode = 1,  y should be altered without changing x.
// If  mode = 2,  x should be altered without changing y.
// The parameter UsrWrk may be used for workspace as described
// below.
//
// The rhs vector b is input via u, and subsequently overwritten.
//
//
// Note:  LSQR uses an iterative method to approximate the solution.
// The number of iterations required to reach a certain accuracy
// depends strongly on the scaling of the problem.  Poor scaling of
// the rows or columns of A should therefore be avoided where
// possible.
//
// For example, in problem 1 the solution is unaltered by
// row-scaling.  If a row of A is very small or large compared to
// the other rows of A, the corresponding row of ( A  b ) should be
// scaled up or down.
//
// In problems 1 and 2, the solution x is easily recovered
// following column-scaling.  Unless better information is known,
// the nonzero columns of A should be scaled so that they all have
// the same Euclidean norm (e.g., 1.0).
//
// In problem 3, there is no freedom to re-scale if damp is
// nonzero.  However, the value of damp should be assigned only
// after attention has been paid to the scaling of A.
//
// The parameter damp is intended to help regularize
// ill-conditioned systems, by preventing the true solution from
// being very large.  Another aid to regularization is provided by
// the parameter acond, which may be used to terminate iterations
// before the computed solution becomes very large.
//
// Note that x is not an input parameter.
// If some initial estimate x0 is known and if damp = 0,
// one could proceed as follows:
//
//   1. Compute a residual vector     r0 = b - A*x0.
//   2. Use LSQR to solve the system  A*dx = r0.
//   3. Add the correction dx to obtain a final solution x = x0 + dx.
//
// This requires that x0 be available before and after the call
// to LSQR.  To judge the benefits, suppose LSQR takes k1 iterations
// to solve A*x = b and k2 iterations to solve A*dx = r0.
// If x0 is "good", norm(r0) will be smaller than norm(b).
// If the same stopping tolerances atol and btol are used for each
// system, k1 and k2 will be similar, but the final solution x0 + dx
// should be more accurate.  The only way to reduce the total work
// is to use a larger stopping tolerance for the second system.
// If some value btol is suitable for A*x = b, the larger value
// btol*norm(b)/norm(r0)  should be suitable for A*dx = r0.
//
// Preconditioning is another way to reduce the number of iterations.
// If it is possible to solve a related system M*x = b efficiently,
// where M approximates A in some helpful way
// (e.g. M - A has low rank or its elements are small relative to
// those of A), LSQR may converge more rapidly on the system
//       A*M(inverse)*z = b,
// after which x can be recovered by solving M*x = z.
//
// NOTE: If A is symmetric, LSQR should not be used!
// Alternatives are the symmetric conjugate-gradient method (cg)
// and/or SYMMLQ.
// SYMMLQ is an implementation of symmetric cg that applies to
// any symmetric A and will converge more rapidly than LSQR.
// If A is positive definite, there are other implementations of
// symmetric cg that require slightly less work per iteration
// than SYMMLQ (but will take the same number of iterations).
//
//
// Notation
// --------
//
// The following quantities are used in discussing the subroutine
// parameters:
//
// Abar   =  (   A    ),          bbar  =  ( b )
//           ( damp*I )                    ( 0 )
//
// r      =  b  -  A*x,           rbar  =  bbar  -  Abar*x
//
// rnorm  =  sqrt( norm(r)**2  +  damp**2 * norm(x)**2 )
//        =  norm( rbar )
//
// relpr  =  the relative precision of floating-point arithmetic
//           on the machine being used.  On most machines,
//           relpr is about 1.0e-7 and 1.0d-16 in single and double
//           precision respectively.
//
// LSQR  minimizes the function rnorm with respect to x.
//
//
// Parameters
// ----------
//
// m       input      m, the number of rows in A.
//
// n       input      n, the number of columns in A.
//
// aprod   external   See above.
//
// damp    input      The damping parameter for problem 3 above.
//                    (damp should be 0.0 for problems 1 and 2.)
//                    If the system A*x = b is incompatible, values
//                    of damp in the range 0 to sqrt(relpr)*norm(A)
//                    will probably have a negligible effect.
//                    Larger values of damp will tend to decrease
//                    the norm of x and reduce the number of
//                    iterations required by LSQR.
//
//                    The work per iteration and the storage needed
//                    by LSQR are the same for all values of damp.
//
// rw      workspace  Transit pointer to user's workspace.
//                    Note:  LSQR  does not explicitly use this
//                    parameter, but passes it to subroutine aprod for
//                    possible use as workspace.
//
// u(m)    input      The rhs vector b.  Beware that u is
//                    over-written by LSQR.
//
// v(n)    workspace
//
// w(n)    workspace
//
// x(n)    output     Returns the computed solution x.
//
// se(*)   output     If m .gt. n  or  damp .gt. 0,  the system is
//         (maybe)    overdetermined and the standard errors may be
//                    useful.  (See the first LSQR reference.)
//                    Otherwise (m .le. n  and  damp = 0) they do not
//                    mean much.  Some time and storage can be saved
//                    by setting  se = NULL.  In that case, se will
//                    not be touched.
//
//                    If se is not NULL, then the dimension of se must
//                    be n or more.  se(1:n) then returns standard error
//                    estimates for the components of x.
//                    For each i, se(i) is set to the value
//                       rnorm * sqrt( sigma(i,i) / t ),
//                    where sigma(i,i) is an estimate of the i-th
//                    diagonal of the inverse of Abar(transpose)*Abar
//                    and  t = 1      if  m .le. n,
//                         t = m - n  if  m .gt. n  and  damp = 0,
//                         t = m      if  damp .ne. 0.
//
// atol    input      An estimate of the relative error in the data
//                    defining the matrix A.  For example,
//                    if A is accurate to about 6 digits, set
//                    atol = 1.0e-6 .
//
// btol    input      An estimate of the relative error in the data
//                    defining the rhs vector b.  For example,
//                    if b is accurate to about 6 digits, set
//                    btol = 1.0e-6 .
//
// conlim  input      An upper limit on cond(Abar), the apparent
//                    condition number of the matrix Abar.
//                    Iterations will be terminated if a computed
//                    estimate of cond(Abar) exceeds conlim.
//                    This is intended to prevent certain small or
//                    zero singular values of A or Abar from
//                    coming into effect and causing unwanted growth
//                    in the computed solution.
//
//                    conlim and damp may be used separately or
//                    together to regularize ill-conditioned systems.
//
//                    Normally, conlim should be in the range
//                    1000 to 1/relpr.
//                    Suggested value:
//                    conlim = 1/(100*relpr)  for compatible systems,
//                    conlim = 1/(10*sqrt(relpr)) for least squares.
//
//         Note:  If the user is not concerned about the parameters
//         atol, btol and conlim, any or all of them may be set
//         to zero.  The effect will be the same as the values
//         relpr, relpr and 1/relpr respectively.
//
// itnlim  input      An upper limit on the number of iterations.
//                    Suggested value:
//                    itnlim = n/2   for well-conditioned systems
//                                   with clustered singular values,
//                    itnlim = 4*n   otherwise.
//
// nout    input      File number for printed output.  If positive,
//                    a summary will be printed on file nout.
//
// istop   output     An integer giving the reason for termination:
//
//            0       x = 0  is the exact solution.
//                    No iterations were performed.
//
//            1       The equations A*x = b are probably
//                    compatible.  Norm(A*x - b) is sufficiently
//                    small, given the values of atol and btol.
//
//            2       damp is zero.  The system A*x = b is probably
//                    not compatible.  A least-squares solution has
//                    been obtained that is sufficiently accurate,
//                    given the value of atol.
//
//            3       damp is nonzero.  A damped least-squares
//                    solution has been obtained that is sufficiently
//                    accurate, given the value of atol.
//
//            4       An estimate of cond(Abar) has exceeded
//                    conlim.  The system A*x = b appears to be
//                    ill-conditioned.  Otherwise, there could be an
//                    error in subroutine aprod.
//
//            5       The iteration limit itnlim was reached.
//
// itn     output     The number of iterations performed.
//
// anorm   output     An estimate of the Frobenius norm of  Abar.
//                    This is the square-root of the sum of squares
//                    of the elements of Abar.
//                    If damp is small and if the columns of A
//                    have all been scaled to have length 1.0,
//                    anorm should increase to roughly sqrt(n).
//                    A radically different value for anorm may
//                    indicate an error in subroutine aprod (there
//                    may be an inconsistency between modes 1 and 2).
//
// acond   output     An estimate of cond(Abar), the condition
//                    number of Abar.  A very high value of acond
//                    may again indicate an error in aprod.
//
// rnorm   output     An estimate of the final value of norm(rbar),
//                    the function being minimized (see notation
//                    above).  This will be small if A*x = b has
//                    a solution.
//
// arnorm  output     An estimate of the final value of
//                    norm( Abar(transpose)*rbar ), the norm of
//                    the residual for the usual normal equations.
//                    This should be small in all cases.  (arnorm
//                    will often be smaller than the true value
//                    computed from the output vector x.)
//
// xnorm   output     An estimate of the norm of the final
//                    solution vector x.
//
// References:
//
// C.C. Paige and M.A. Saunders,  LSQR: An algorithm for sparse
//      linear equations and sparse least squares,
//      ACM Transactions on Mathematical Software 8, 1 (March 1982),
//      pp. 43-71.
//
// C.C. Paige and M.A. Saunders,  Algorithm 583, LSQR: Sparse
//      linear equations and least-squares problems,
//      ACM Transactions on Mathematical Software 8, 2 (June 1982),
//      pp. 195-209.
//
// C.L. Lawson, R.J. Hanson, D.R. Kincaid and F.T. Krogh,
//      Basic linear algebra subprograms for Fortran usage,
//      ACM Transactions on Mathematical Software 5, 3 (Sept 1979),
//      pp. 308-323 and 324-325.
//
// Michael A. Saunders                  mike@sol-michael.stanford.edu
// Dept of Operations Research          na.Msaunders@na-net.ornl.gov
// Stanford University
// Stanford, CA 94305-4022              (415) 723-1875
void mblsqr_lsqr(int m, int n, void (*aprod)(int mode, int m, int n, double x[], double y[], void *UsrWrk), double damp,
                 void *UsrWrk,
                 double u[],  // len = m
                 double v[],  // len = n
                 double w[],  // len = n
                 double x[],  // len = n
                 double se[], // len at least n.  May be NULL.
                 double atol, double btol, double conlim, int itnlim, FILE *nout,
                 // The remaining variables are output only.
                 int *istop_out, int *itn_out, double *anorm_out, double *acond_out, double *rnorm_out, double *arnorm_out,
                 double *xnorm_out) {
	const bool extra = false; // true for extra printing below.
	const bool damped = damp > ZERO;
	const bool wantse = se != NULL;

	const char *enter = "Enter LSQR.  ";
	const char *exit = "Exit  LSQR.  ";
	const char msg[6][100] = {{"The exact solution is  x = 0"},
	                    {"A solution to Ax = b was found, given atol, btol"},
	                    {"A least-squares solution was found, given atol"},
	                    {"A damped least-squares solution was found, given atol"},
	                    {"Cond(Abar) seems to be too large, given conlim"},
	                    {"The iteration limit was reached"}};
	//-----------------------------------------------------------------------

	//  Initialize.

	if (nout != NULL)
		fprintf(nout,
		        " %s        Least-squares solution of  Ax = b\n"
		        " The matrix  A  has %7d rows  and %7d columns\n"
		        " damp   = %-22.2e    wantse = %10i\n"
		        " atol   = %-22.2e    conlim = %10.2e\n"
		        " btol   = %-22.2e    itnlim = %10d\n\n",
		        enter, m, n, damp, wantse, atol, conlim, btol, itnlim);

	int itn = 0;
	int istop = 0;
	int nstop = 0;
	int maxdx = 0;
	const double ctol = conlim > ZERO ? ONE / conlim : ZERO;
	double anorm = ZERO;
	double acond = ZERO;
	double xnorm = ZERO;

	double dnorm = ZERO;
	double dxmax = ZERO;
	double res2 = ZERO;
	double psi = ZERO;
	xnorm = ZERO;
	double xnorm1 = ZERO;
	double cs2 = -ONE;
	double sn2 = ZERO;
	double z = ZERO;

	double test1 = 0.0;
	double test2 = 0.0;
	double test3 = 0.0;

	//  ------------------------------------------------------------------
	//  Set up the first vectors u and v for the bidiagonalization.
	//  These satisfy  beta*u = b,  alpha*v = A(transpose)*u.
	//  ------------------------------------------------------------------
	mblsqr_dload(n, 0.0, v);
	mblsqr_dload(n, 0.0, x);

	if (wantse)
		mblsqr_dload(n, 0.0, se);

	double alpha = ZERO;
	double beta = mbcblas_dnrm2(m, u, 1);

	if (beta > ZERO) {
		mbcblas_dscal(m, (ONE / beta), u, 1);
		aprod(2, m, n, v, u, UsrWrk);
		alpha = mbcblas_dnrm2(n, v, 1);
	}

	if (alpha > ZERO) {
		mbcblas_dscal(n, (ONE / alpha), v, 1);
		mbcblas_dcopy(n, v, 1, w, 1);
	}

	int nconv;
	double bnorm = 0.0;
	double rnorm = 0.0;
	double alfopt, cs, cs1, delta, dknorm, dxk, gamma, gambar, phi;
	double rho, rhbar1, rhs, rtol, sn, sn1, t, tau, temp, theta, t1, t2, t3;
	double zbar;

	double arnorm0 = alpha * beta;
	double arnorm = arnorm0;
	// if (arnorm == ZERO)
	//	goto goto_800;
	if (arnorm != ZERO) {
		double rhobar = alpha;
		double phibar = beta;
		bnorm = beta;
		rnorm = beta;

	if (nout != NULL) {
		if (damped)
			fprintf(nout, "    Itn       x(1)           Function"
			              "     Compatible    LS      Norm Abar   Cond Abar\n");
		else
			fprintf(nout, "    Itn       x(1)           Function"
			              "     Compatible    LS      Norm A   Cond A\n");

		test1 = ONE;
		test2 = alpha / beta;

		if (extra)
			fprintf(nout, "     phi    dknorm  dxk  alfa_opt\n");

		fprintf(nout, " %6d %16.9e %16.9e %9.2e %9.2e\n", itn, x[0], rnorm, test1, test2);
		fprintf(nout, "\n");
	}

	//  ==================================================================
	//  Main iteration loop.
	//  ==================================================================
	while (1) {
		itn = itn + 1;

		//      ------------------------------------------------------------------
		//      Perform the next step of the bidiagonalization to obtain the
		//      next  beta, u, alpha, v.  These satisfy the relations
		//                 beta*u  =  A*v  -  alpha*u,
		//                alpha*v  =  A(transpose)*u  -  beta*v.
		//      ------------------------------------------------------------------
		mbcblas_dscal(m, (-alpha), u, 1);
		aprod(1, m, n, v, u, UsrWrk);
		beta = mbcblas_dnrm2(m, u, 1);

		//      Accumulate  anorm = || Bk ||
		//                        =  sqrt( sum of  alpha**2 + beta**2 + damp**2 ).

		temp = mblsqr_d2norm(alpha, beta);
		temp = mblsqr_d2norm(temp, damp);
		anorm = mblsqr_d2norm(anorm, temp);

		if (beta > ZERO) {
			mbcblas_dscal(m, (ONE / beta), u, 1);
			mbcblas_dscal(n, (-beta), v, 1);
			aprod(2, m, n, v, u, UsrWrk);
			alpha = mbcblas_dnrm2(n, v, 1);
			if (alpha > ZERO) {
				mbcblas_dscal(n, (ONE / alpha), v, 1);
			}
		}

		//      ------------------------------------------------------------------
		//      Use a plane rotation to eliminate the damping parameter.
		//      This alters the diagonal (rhobar) of the lower-bidiagonal matrix.
		//      ------------------------------------------------------------------
		rhbar1 = rhobar;
		if (damped) {
			rhbar1 = mblsqr_d2norm(rhobar, damp);
			cs1 = rhobar / rhbar1;
			sn1 = damp / rhbar1;
			psi = sn1 * phibar;
			phibar = cs1 * phibar;
		}

		//      ------------------------------------------------------------------
		//      Use a plane rotation to eliminate the subdiagonal element (beta)
		//      of the lower-bidiagonal matrix, giving an upper-bidiagonal matrix.
		//      ------------------------------------------------------------------
		rho = mblsqr_d2norm(rhbar1, beta);
		cs = rhbar1 / rho;
		sn = beta / rho;
		theta = sn * alpha;
		rhobar = -cs * alpha;
		phi = cs * phibar;
		phibar = sn * phibar;
		tau = sn * phi;

		//      ------------------------------------------------------------------
		//      Update  x, w  and (perhaps) the standard error estimates.
		//      ------------------------------------------------------------------
		t1 = phi / rho;
		t2 = -theta / rho;
		t3 = ONE / rho;
		dknorm = ZERO;

		if (wantse) {
			for (int i = 0; i < n; i++) {
				t = w[i];
				x[i] = t1 * t + x[i];
				w[i] = t2 * t + v[i];
				t = (t3 * t) * (t3 * t);
				se[i] = t + se[i];
				dknorm = t + dknorm;
			}
		}
		else {
			for (int i = 0; i < n; i++) {
				t = w[i];
				x[i] = t1 * t + x[i];
				w[i] = t2 * t + v[i];
				dknorm = (t3 * t) * (t3 * t) + dknorm;
			}
		}

		//      ------------------------------------------------------------------
		//      Monitor the norm of d_k, the update to x.
		//      dknorm = norm( d_k )
		//      dnorm  = norm( D_k ),        where   D_k = (d_1, d_2, ..., d_k )
		//      dxk    = norm( phi_k d_k ),  where new x = x_k + phi_k d_k.
		//      ------------------------------------------------------------------
		dknorm = sqrt(dknorm);
		dnorm = mblsqr_d2norm(dnorm, dknorm);
		dxk = fabs(phi * dknorm);
		if (dxmax < dxk) {
			dxmax = dxk;
			maxdx = itn;
		}

		//      ------------------------------------------------------------------
		//      Use a plane rotation on the right to eliminate the
		//      super-diagonal element (theta) of the upper-bidiagonal matrix.
		//      Then use the result to estimate  norm(x).
		//      ------------------------------------------------------------------
		delta = sn2 * rho;
		gambar = -cs2 * rho;
		rhs = phi - delta * z;
		zbar = rhs / gambar;
		xnorm = mblsqr_d2norm(xnorm1, zbar);
		gamma = mblsqr_d2norm(gambar, theta);
		cs2 = gambar / gamma;
		sn2 = theta / gamma;
		z = rhs / gamma;
		xnorm1 = mblsqr_d2norm(xnorm1, z);

		//      ------------------------------------------------------------------
		//      Test for convergence.
		//      First, estimate the norm and condition of the matrix  Abar,
		//      and the norms of  rbar  and  Abar(transpose)*rbar.
		//      ------------------------------------------------------------------
		acond = anorm * dnorm;
		res2 = mblsqr_d2norm(res2, psi);
		rnorm = mblsqr_d2norm(res2, phibar);
		arnorm = alpha * fabs(tau);

		//      Now use these norms to estimate certain other quantities,
		//      some of which will be small near a solution.

		alfopt = sqrt(rnorm / (dnorm * xnorm));
		test1 = rnorm / bnorm;
		test2 = ZERO;
		if (rnorm > ZERO)
			test2 = arnorm / (anorm * rnorm);
		//      if (arnorm0 > ZERO) test2 = arnorm / arnorm0;  //(Michael Friedlander's modification)
		test3 = ONE / acond;
		t1 = test1 / (ONE + anorm * xnorm / bnorm);
		rtol = btol + atol * anorm * xnorm / bnorm;

		//      The following tests guard against extremely small values of
		//      atol, btol  or  ctol.  (The user may have set any or all of
		//      the parameters  atol, btol, conlim  to zero.)
		//      The effect is equivalent to the normal tests using
		//      atol = relpr,  btol = relpr,  conlim = 1/relpr.

		t3 = ONE + test3;
		t2 = ONE + test2;
		t1 = ONE + t1;
		if (itn >= itnlim)
			istop = 5;
		if (t3 <= ONE)
			istop = 4;
		if (t2 <= ONE)
			istop = 2;
		if (t1 <= ONE)
			istop = 1;

		//      Allow for tolerances set by the user.

		if (test3 <= ctol)
			istop = 4;
		if (test2 <= atol)
			istop = 2;
		if (test1 <= rtol)
			istop = 1; //(Michael Friedlander had this commented out)

		//      ------------------------------------------------------------------
		//      See if it is time to print something.
		//      ------------------------------------------------------------------
		if (nout == NULL)
			goto goto_600;
		if (n <= 40)
			goto goto_400;
		if (itn <= 10)
			goto goto_400;
		if (itn >= itnlim - 10)
			goto goto_400;
		if (itn % 100 == 0)
			goto goto_400;
		// if (test3 <=  2.0*ctol) goto goto_400;
		// if (test2 <= 2.0*atol) goto goto_400;
		// if (test1 <= 2.0*rtol) goto goto_400;
		if (istop != 0)
			goto goto_400;
		goto goto_600;

	//      Print a line for this iteration.
	//      "extra" is for experimental purposes.

	goto_400:
		if (extra) {
			fprintf(nout, " %6d %16.9e %16.9e %9.2e %9.2e %8.1e %8.1e %8.1e %7.1e %7.1e %7.1e\n", itn, x[0], rnorm, test1, test2,
			        anorm, acond, phi, dknorm, dxk, alfopt);
		}
		else {
			fprintf(nout, " %6d %16.9e %16.9e %9.2e %9.2e %8.1e %8.1e  Convergence: %9.2e %9.2e\n", itn, x[0], rnorm, test1,
			        test2, anorm, acond, test1 / rtol, test2 / atol);
		}
	// if (itn % 10 == 0) fprintf(nout, "\n");

	//      ------------------------------------------------------------------
	//      Stop if appropriate.
	//      The convergence criteria are required to be met on  nconv
	//      consecutive iterations, where  nconv  is set below.
	//      Suggested value:  nconv = 1, 2  or  3.
	//      ------------------------------------------------------------------
	goto_600:
		if (istop == 0) {
			nstop = 0;
		}
		else {
			nconv = 1;
			nstop = nstop + 1;
			if (nstop < nconv && itn < itnlim)
				istop = 0;
		}

		if (istop != 0)
			break;
	}
	//  ==================================================================
	//  End of iteration loop.
	//  ==================================================================

	//  Finish off the standard error estimates.

	if (wantse) {
		t = ONE;
		if (m > n)
			t = m - n;
		if (damped)
			t = m;
		t = rnorm / sqrt(t);

		for (int i = 0; i < n; i++)
			se[i] = t * sqrt(se[i]);
	}

//  Decide if istop = 2 or 3.
//  Print the stopping condition.
	} // goto_800:

	if (damped && istop == 2)
		istop = 3;
	if (nout != NULL) {
		fprintf(nout,
		        "\n"
		        " %s       istop  = %-10d      itn    = %-10d\n"
		        " %s       anorm  = %11.5e     acond  = %11.5e\n"
		        " %s       vnorm  = %11.5e     xnorm  = %11.5e\n"
		        " %s       rnorm  = %11.5e     arnorm = %11.5e\n",
		        exit, istop, itn, exit, anorm, acond, exit, bnorm, xnorm, exit, rnorm, arnorm);
		fprintf(nout,
		        " %s       max dx = %7.1e occurred at itn %-9d\n"
		        " %s              = %7.1e*xnorm\n",
		        exit, dxmax, maxdx, exit, dxmax / (xnorm + 1.0e-20));
		fprintf(nout, " %s       %s\n", exit, msg[istop]);
	}

	//  Assign output variables from local copies.
	*istop_out = istop;
	*itn_out = itn;
	*anorm_out = anorm;
	*acond_out = acond;
	*rnorm_out = rnorm;
	*arnorm_out = test2;
	*xnorm_out = xnorm;

	return;
}
// ---------------------------------------------------------------------
