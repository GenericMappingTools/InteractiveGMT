/*======================================================================================
 * mbgrid.c -- see mbgrid.h for the API and for the list of deliberate deviations from
 * gmtmbgrid.c.  No GMT headers, no Qt, no VTK: plain C99 + libm.
 *====================================================================================*/

#include "mbgrid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define MB_MIN(a,b) ((a) < (b) ? (a) : (b))
#define MB_MAX(a,b) ((a) > (b) ? (a) : (b))

/* zgrid's "unreachable" marker, and the threshold the merge step tests against.
 * Kept at the original's values so a grid solved here and a grid solved by the old
 * module are interchangeable at the merge boundary. */
static const double MB_BIG   = 9.0e29;
static const double MB_ZFLAG = 5.0e34;

/* ------------------------------------------------------------------------------------
 * geometry
 * ---------------------------------------------------------------------------------- */

typedef struct {
	int32_t nx, ny;          /* output grid                                          */
	int32_t gxdim, gydim;    /* working grid (output + 2*off)                        */
	int32_t offx, offy;      /* -E margin, in nodes                                  */
	double  x0, y0;          /* SW corner of the WORKING grid (node centre)          */
	double  dx, dy;
} mb_geom;

static int mb_geometry(const mbgrid_params *p, mb_geom *g) {
	int pix;

	if (p == NULL || g == NULL) return MBGRID_ERR_ARG;
	if (!(p->xinc > 0.0) || !(p->yinc > 0.0)) return MBGRID_ERR_REGION;
	if (!(p->east > p->west) || !(p->north > p->south)) return MBGRID_ERR_REGION;
	if (p->registration != MBGRID_REG_GRIDLINE && p->registration != MBGRID_REG_PIXEL)
		return MBGRID_ERR_ARG;

	/* Pixel registration counts CELLS between the region edges and puts the first node
	 * half a cell in; gridline registration counts NODES and puts it on the edge. Both
	 * come out as the one thing everything downstream uses: a node count and the SW-most
	 * node centre. No other site in this file looks at p->registration again. */
	pix = (p->registration == MBGRID_REG_PIXEL);
	g->dx = p->xinc;
	g->dy = p->yinc;
	g->nx = (p->nx > 0) ? p->nx : (int32_t)floor((p->east  - p->west ) / p->xinc + 0.5) + (pix ? 0 : 1);
	g->ny = (p->ny > 0) ? p->ny : (int32_t)floor((p->north - p->south) / p->yinc + 0.5) + (pix ? 0 : 1);
	if (g->nx < 4 || g->ny < 4) return MBGRID_ERR_TOO_SMALL;

	/* -E widens the working grid so that data just outside -R still constrain the
	 * spline near the edge.  The margin is a FRACTION of the grid, as in the original. */
	g->offx = (p->extend > 0.0) ? (int32_t)(p->extend * g->nx) : 0;
	g->offy = (p->extend > 0.0) ? (int32_t)(p->extend * g->ny) : 0;
	g->gxdim = g->nx + 2 * g->offx;
	g->gydim = g->ny + 2 * g->offy;

	/* ONE origin for everything downstream.  gmtmbgrid.c bins against this extended
	 * corner but then builds its node coordinates from the un-extended one, so with -E
	 * the data sat offx cells away from the nodes they had been binned into. */
	g->x0 = p->west  + (pix ? 0.5 * g->dx : 0.0) - g->offx * g->dx;
	g->y0 = p->south + (pix ? 0.5 * g->dy : 0.0) - g->offy * g->dy;
	return MBGRID_OK;
}

/* -C resolution, lifted from GMT_gmtmbgrid().  ALL is expressed as "a radius nothing can
 * exceed", and a radius that already covers the grid degenerates back to ALL.
 *
 * The radius is measured on the WORKING grid, which is what every sweep that consumes it
 * (mbgrid_zgrid's seeding, mbgrid_fill's rings) actually walks. Sizing it from the OUTPUT
 * grid, as the original did, leaves the -E margin under-reached whenever extend > 0. */
static void mb_resolve_clip(const mbgrid_params *p, const mb_geom *g,
                            int32_t *mode, int32_t *clip) {
	int32_t m = p->clipmode, c = p->clip;
	const int32_t span = MB_MAX(g->gxdim, g->gydim) + 1;
	if (m == MBGRID_INTERP_ALL) c = span;
	if ((m == MBGRID_INTERP_GAP || m == MBGRID_INTERP_NEAR) && c >= span) m = MBGRID_INTERP_ALL;
	if (c < 0) c = 0;
	*mode = m;
	*clip = c;
}

MBGRID_API int mbgrid_dims(const mbgrid_params *p, int32_t *nx, int32_t *ny) {
	mb_geom g;
	int rc = mb_geometry(p, &g);
	if (rc != MBGRID_OK) return rc;
	if (nx) *nx = g.nx;
	if (ny) *ny = g.ny;
	return MBGRID_OK;
}

MBGRID_API int mbgrid_work_dims(const mbgrid_params *p, int32_t *gxdim, int32_t *gydim,
                                int32_t *offx, int32_t *offy) {
	mb_geom g;
	int rc = mb_geometry(p, &g);
	if (rc != MBGRID_OK) return rc;
	if (gxdim) *gxdim = g.gxdim;
	if (gydim) *gydim = g.gydim;
	if (offx)  *offx  = g.offx;
	if (offy)  *offy  = g.offy;
	return MBGRID_OK;
}

MBGRID_API int mbgrid_work_origin(const mbgrid_params *p, double *x0, double *y0) {
	mb_geom g;
	int rc = mb_geometry(p, &g);
	if (rc != MBGRID_OK) return rc;
	if (x0) *x0 = g.x0;
	if (y0) *y0 = g.y0;
	return MBGRID_OK;
}

/* ------------------------------------------------------------------------------------
 * step 1 -- Gaussian-weighted binning
 * ---------------------------------------------------------------------------------- */

/* Accumulators shared by the table pass and the breakline pass. */
typedef struct {
	double *sum;      /* sum of w*z            */
	double *norm;     /* sum of w              */
	unsigned char *own; /* a point fell in THIS node's own cell */
} mb_acc;

/* Spread one point over the (2*xtradim+1)^2 neighbourhood.  `pin` marks a breakline
 * point, which overrides whatever else reached its own cell.  Returns 1 if the point
 * landed inside the working window, 0 if it was too far out to touch any node -- only
 * the former counts towards n_data, as in the original. */
static int mb_spread(const mb_geom *g, mb_acc *a, int xtradim, double factor,
                     double px, double py, double pz, int pin) {
	int ix, iy, ix1, ix2, iy1, iy2, ii, jj;
	double xx, xx2, yy, w;

	/* floor(), not the original's (int) truncation: with truncation every point in the
	 * half-cell strip at negative index folds onto index 0 and falsely marks that node
	 * as data-bearing. */
	ix = (int)floor((px - g->x0) / g->dx + 0.5);
	iy = (int)floor((py - g->y0) / g->dy + 0.5);

	if (ix < -xtradim || ix >= g->gxdim + xtradim) return 0;
	if (iy < -xtradim || iy >= g->gydim + xtradim) return 0;

	ix1 = MB_MAX(ix - xtradim, 0);
	ix2 = MB_MIN(ix + xtradim, g->gxdim - 1);
	iy1 = MB_MAX(iy - xtradim, 0);
	iy2 = MB_MIN(iy + xtradim, g->gydim - 1);

	for (ii = ix1; ii <= ix2; ii++) {
		xx  = g->x0 + ii * g->dx - px;
		xx2 = xx * xx;
		for (jj = iy1; jj <= iy2; jj++) {
			size_t k = (size_t)ii * (size_t)g->gydim + (size_t)jj;
			yy = g->y0 + jj * g->dy - py;
			w  = exp(-(xx2 + yy * yy) * factor);
			a->sum[k]  += w * pz;
			a->norm[k] += w;
			if (ii == ix && jj == iy) {
				if (pin) {          /* breakline owns this cell outright */
					a->sum[k]  = pz;
					a->norm[k] = 1.0;
				}
				a->own[k] = 1;
			}
		}
	}
	return 1;
}

/* Densify a breakline to ~one point per cell and bin it.  Rewritten from scratch: the
 * original interp_breakline() dereferenced a NULL grid header before its first loop. */
static void mb_bin_breakline(const mb_geom *g, mb_acc *a, int xtradim, double factor,
                             const mbgrid_breakline *bl, int64_t *n_added) {
	int32_t s;
	int64_t base = 0, added = 0;
	double r_dx = 1.0 / g->dx, r_dy = 1.0 / g->dy;

	for (s = 0; s < bl->nseg; s++) {
		int32_t nrow = bl->seg_len[s], r;
		for (r = 0; r + 1 < nrow; r++) {
			int64_t i0 = base + r;
			double x0 = bl->x[i0],     y0 = bl->y[i0],     z0 = bl->z[i0];
			double dx = bl->x[i0 + 1] - x0;
			double dy = bl->y[i0 + 1] - y0;
			double dz = bl->z[i0 + 1] - z0;
			long n_int = lrint(MB_MAX(fabs(dx) * r_dx, fabs(dy) * r_dy)) + 1;
			long k;
			if (n_int < 2) n_int = 2;
			for (k = 0; k < n_int; k++) {
				double t = (double)k / (double)(n_int - 1);
				double zz = z0 + t * dz;
				if (isnan(zz)) continue;
				/* the last sample of a segment is the first of the next one; skipping it
				 * keeps a vertex from being pinned twice (harmless, but it doubles work) */
				if (k == n_int - 1 && r + 2 < nrow) continue;
				added += mb_spread(g, a, xtradim, factor, x0 + t * dx, y0 + t * dy, zz, 1);
			}
		}
		base += nrow;
	}
	*n_added = added;
}

MBGRID_API int mbgrid_bin(const mbgrid_params *p,
                          const double *x, const double *y, const double *z, int64_t n,
                          const mbgrid_breakline *bl,
                          float *work, int64_t *n_data, int64_t *n_set) {
	mb_geom g;
	mb_acc a;
	size_t nw;
	int64_t i, ndat = 0, nset = 0, nbl = 0;
	int xtradim;
	double factor, scale;
	int rc = mb_geometry(p, &g);

	if (rc != MBGRID_OK) return rc;
	if (work == NULL) return MBGRID_ERR_ARG;
	if (n > 0 && (x == NULL || y == NULL || z == NULL)) return MBGRID_ERR_ARG;
	if (bl != NULL && bl->nseg > 0 &&
	    (bl->x == NULL || bl->y == NULL || bl->z == NULL || bl->seg_len == NULL))
		return MBGRID_ERR_ARG;

	scale = (p->scale > 0.0) ? p->scale : 1.0;
	/* Gaussian falls to exp(-4) ~ 1.8% at one `scale` cell away, as in MBGRID. */
	factor  = 4.0 / (scale * scale * g.dx * g.dy);
	xtradim = (int)scale + 2;

	nw = (size_t)g.gxdim * (size_t)g.gydim;
	a.sum  = (double *)calloc(nw, sizeof(double));
	a.norm = (double *)calloc(nw, sizeof(double));
	a.own  = (unsigned char *)calloc(nw, 1);
	if (a.sum == NULL || a.norm == NULL || a.own == NULL) {
		free(a.sum); free(a.norm); free(a.own);
		return MBGRID_ERR_MEMORY;
	}

	for (i = 0; i < n; i++) {
		if (isnan(z[i]) || isnan(x[i]) || isnan(y[i])) continue;
		ndat += mb_spread(&g, &a, xtradim, factor, x[i], y[i], z[i], 0);
	}
	if (bl != NULL && bl->nseg > 0)
		mb_bin_breakline(&g, &a, xtradim, factor, bl, &nbl);

	for (i = 0; i < (int64_t)nw; i++) {
		if (a.own[i] && a.norm[i] > 0.0) {
			work[i] = (float)(a.sum[i] / a.norm[i]);
			nset++;
		}
		else
			work[i] = (float)NAN;      /* the ONE marker for "no value here" */
	}

	free(a.sum); free(a.norm); free(a.own);

	if (n_data) *n_data = ndat + nbl;
	if (n_set)  *n_set  = nset;
	if (nset == 0) return MBGRID_ERR_NO_DATA;
	return MBGRID_OK;
}

MBGRID_API int64_t mbgrid_nodes(const mbgrid_params *p, const float *work,
                                double *xo, double *yo, double *zo, int64_t cap) {
	mb_geom g;
	int32_t ix, iy;
	int64_t k = 0;
	int rc = mb_geometry(p, &g);
	if (rc != MBGRID_OK) return rc;
	if (work == NULL) return MBGRID_ERR_ARG;

	for (ix = 0; ix < g.gxdim; ix++) {
		for (iy = 0; iy < g.gydim; iy++) {
			float v = work[(size_t)ix * (size_t)g.gydim + (size_t)iy];
			if (isnan(v)) continue;
			if (xo != NULL) {
				if (k >= cap) return MBGRID_ERR_ARG;
				xo[k] = g.x0 + ix * g.dx;
				yo[k] = g.y0 + iy * g.dy;
				zo[k] = (double)v;
			}
			k++;
		}
	}
	return k;
}

/* ------------------------------------------------------------------------------------
 * step 2a -- IGPP/SIO thin-plate spline (mb_zgrid)
 *
 * Translated from the f2c'd Fortran in gmtmbgrid.c, with the goto lattice unwound and
 * the data-point machinery removed.  That removal is exact, not a simplification: the
 * input here is ALREADY one value per node, so
 *   - the knxt[] chains that averaged several points sharing a node are all length 1;
 *   - the "shift data points back" block computes a sub-cell offset (x,y) that is
 *     identically zero for a node-centred point, so its parabolic correction is zero
 *     and it re-pins each data node to the value it already holds.
 * Data nodes are stored NEGATED (and biased by zbase so the negation is unambiguous);
 * that sign is what the relaxation sweep uses to know it must not move them.
 *
 * ZZ(i,j) is the f2c'd code's 1-based z[i + j*nx] view of sgrid, i in 1..nx, j in 1..ny.
 * It is a MACRO over sgrid rather than the shifted pointer `z = sgrid - (nx + 1)` the
 * translation invites: that pointer is formed BEFORE the start of the object, which is
 * undefined behaviour whether or not it is ever dereferenced.
 * ---------------------------------------------------------------------------------- */

MBGRID_API int mbgrid_zgrid(const mbgrid_params *p, const float *work, float *sgrid,
                            int32_t nrng) {
	mb_geom g;
	int nx, ny, i, j, iter;
	size_t nw;
	double zmin = DBL_MAX, zmax = -DBL_MAX, zrange, zbase;
	double eps = 0.002, relax = 1.0, dzrms8 = 0.0;
	int itmax = 100;
	int *imnew = NULL;
	int rc = mb_geometry(p, &g);

	if (rc != MBGRID_OK) return rc;
	if (work == NULL || sgrid == NULL) return MBGRID_ERR_ARG;

	nx = g.gxdim;
	ny = g.gydim;
	nw = (size_t)nx * (size_t)ny;

#define ZZ(i,j) sgrid[(size_t)((i) - 1) + (size_t)((j) - 1) * (size_t)nx]

	for (i = 0; i < (int)nw; i++) {
		float v = work[i];
		if (isnan(v)) continue;
		if (v < zmin) zmin = v;
		if (v > zmax) zmax = v;        /* the original started this scan at node 1 */
	}
	if (zmin > zmax) return MBGRID_ERR_NO_DATA;
	zrange = zmax - zmin;
	if (zrange <= 0.0) zrange = 1.0;   /* constant field: keep every ratio finite */
	zbase = zrange * 20.0 - zmin;

	imnew = (int *)calloc((size_t)ny, sizeof(int));
	if (imnew == NULL) return MBGRID_ERR_MEMORY;

	/* sgrid is row-major, x fastest, row 0 = south; work is column-major, y ascending. */
	for (i = 1; i <= nx; i++) {
		for (j = 1; j <= ny; j++) {
			float v = work[(size_t)(i - 1) * (size_t)ny + (size_t)(j - 1)];
			ZZ(i, j) = isnan(v) ? -1.0e35f : (float)(-((double)v + zbase));
		}
	}

	/* Seed every unset node from an already-known neighbour, one ring per sweep.  imnew /
	 * jmnew stop a value created earlier in THIS sweep from being smeared further. */
	{
		int jmnew = 0;
		for (iter = 1; iter <= nrng; iter++) {
			int nnew = 0;
			for (i = 1; i <= nx; i++) {
				for (j = 1; j <= ny; j++) {
					double zijn = 0.0;
					int got = 0;
					if (ZZ(i, j) + MB_BIG >= 0.0) { imnew[j-1] = 0; jmnew = 0; continue; }

					if (j > 1 && jmnew <= 0) {
						zijn = fabs(ZZ(i, j - 1));
						if (zijn < MB_BIG) got = 1;
					}
					if (!got && i > 1 && imnew[j - 1] <= 0) {
						zijn = fabs(ZZ(i - 1, j));
						if (zijn < MB_BIG) got = 1;
					}
					if (!got && j < ny) {
						zijn = fabs(ZZ(i, j + 1));
						if (zijn < MB_BIG) got = 1;
					}
					if (!got && i < nx) {
						zijn = fabs(ZZ(i + 1, j));
						if (zijn < MB_BIG) got = 1;
					}
					if (got) {
						imnew[j - 1] = 1; jmnew = 1;
						ZZ(i, j) = (float)zijn;
						nnew++;
					}
					else { imnew[j - 1] = 0; jmnew = 0; }
				}
			}
			if (nnew <= 0) break;
		}
	}

	/* Nodes the sweep never reached are -1e35; flip them positive so they read as
	 * "unknown" (>= MB_ZFLAG) to the caller instead of as a huge negative depth. */
	for (i = 1; i <= nx; i++)
		for (j = 1; j <= ny; j++) {
			double abz = fabs(ZZ(i, j));
			if (abz >= MB_BIG) ZZ(i, j) = (float)abz;
		}

	/* Point over-relaxation on the Laplace/spline equation (Carre's method). */
	for (iter = 1; iter <= itmax; iter++) {
		double cay = p->tension;
		double dzrms = 0.0, dzmax = 0.0, root, dzmaxf;
		int64_t npg = 0;

		for (i = 1; i <= nx; i++) {
			for (j = 1; j <= ny; j++) {
				double z00 = ZZ(i, j);
				double wgt = 0.0, zsum = 0.0, dz;
				double zim = 0.0, zip, zimm, zipp;
				double zjm = 0.0, zjp, zjmm, zjpp;
				int im = 0, jm = 0;

				if (z00 >= MB_BIG) continue;   /* never reached      */
				if (z00 < 0.0) continue;       /* data node: pinned  */

				/* --- x direction --- */
				do {
					if (i <= 1) break;
					zim = fabs(ZZ(i - 1, j));
					if (zim >= MB_BIG) break;
					im = 1; wgt += 1.0; zsum += zim;
					if (i <= 2) break;
					zimm = fabs(ZZ(i - 2, j));
					if (zimm >= MB_BIG) break;
					wgt += cay; zsum -= cay * (zimm - 2.0 * zim);
				} while (0);
				do {
					if (i >= nx) break;
					zip = fabs(ZZ(i + 1, j));
					if (zip >= MB_BIG) break;
					wgt += 1.0; zsum += zip;
					if (im > 0) { wgt += 4.0 * cay; zsum += 2.0 * cay * (zim + zip); }
					if (i >= nx - 1) break;
					zipp = fabs(ZZ(i + 2, j));
					if (zipp >= MB_BIG) break;
					wgt += cay; zsum -= cay * (zipp - 2.0 * zip);
				} while (0);

				/* --- y direction --- */
				do {
					if (j <= 1) break;
					zjm = fabs(ZZ(i, j - 1));
					if (zjm >= MB_BIG) break;
					jm = 1; wgt += 1.0; zsum += zjm;
					if (j <= 2) break;
					zjmm = fabs(ZZ(i, j - 2));
					if (zjmm >= MB_BIG) break;
					wgt += cay; zsum -= cay * (zjmm - 2.0 * zjm);
				} while (0);
				do {
					if (j >= ny) break;
					zjp = fabs(ZZ(i, j + 1));
					if (zjp >= MB_BIG) break;
					wgt += 1.0; zsum += zjp;
					if (jm > 0) { wgt += 4.0 * cay; zsum += 2.0 * cay * (zjm + zjp); }
					if (j >= ny - 1) break;
					zjpp = fabs(ZZ(i, j + 2));
					if (zjpp >= MB_BIG) break;
					wgt += cay; zsum -= cay * (zjpp - 2.0 * zjp);
				} while (0);

				if (wgt <= 0.0) continue;      /* fully isolated node */
				dz = zsum / wgt - z00;
				npg++;
				dzrms += dz * dz;
				dzmax = MB_MAX(fabs(dz), dzmax);
				ZZ(i, j) = (float)(z00 + dz * relax);
			}
		}

		if (npg <= 1) break;                   /* nothing left to relax */

		dzrms  = sqrt(dzrms / (double)npg);
		dzmaxf = dzmax / zrange;
		if (iter % 10 == 2) dzrms8 = dzrms;
		if (iter % 10 != 0) {
			if (p->verbose) fprintf(stderr, "mbgrid: zgrid iteration %d/%d\r", iter, itmax);
			continue;
		}

		root = (dzrms8 > 0.0) ? sqrt(sqrt(sqrt(dzrms / dzrms8))) : 1.0;
		if (root < 0.9999) {
			if (dzmaxf / (1.0 - root) <= eps) break;    /* converged */
			/* Retune the over-relaxation factor, but only at the three iterations the
			 * original picked -- the estimate is only meaningful once the decay is
			 * geometric. */
			if ((iter == 20 || iter == 40 || iter == 60) && relax - 1.0 - root < 0.0) {
				double tpy    = (root + relax - 1.0) / relax;
				double rootgs = tpy * tpy / root;
				double relaxn;
				if (rootgs < 1.0) {
					relaxn = 2.0 / (sqrt(1.0 - rootgs) + 1.0);
					if (iter != 60) relaxn -= (2.0 - relaxn) * 0.25;
					relax = MB_MAX(relax, relaxn);
				}
			}
		}
		if (p->verbose) fprintf(stderr, "mbgrid: zgrid iteration %d/%d\r", iter, itmax);
	}
	if (p->verbose) fprintf(stderr, "\n");

	/* Undo the bias and the sign. */
	for (i = 1; i <= nx; i++)
		for (j = 1; j <= ny; j++)
			if (ZZ(i, j) < MB_BIG)
				ZZ(i, j) = (float)(fabs(ZZ(i, j)) - zbase);

#undef ZZ

	free(imnew);
	return MBGRID_OK;
}

/* ------------------------------------------------------------------------------------
 * step 3 -- merge the solution back, honouring the clip mode
 *
 * All ring arithmetic is done in signed int.  gmtmbgrid.c ran it in uint64_t, where
 * `i - ir` underflows at the west/south edges (so those rings were silently skipped) and
 * `(ii - i)` wraps to ~1.8e19, making `dmask[iii*3 + jjj]` a stack overrun on a 9-element
 * array.  -Cg was corrupting memory.
 * ---------------------------------------------------------------------------------- */

static float mb_sg(const float *sgrid, const mb_geom *g, int i, int j, int layout) {
	size_t k;
	if (layout == MBGRID_SG_COLMAJOR)     k = (size_t)i * (size_t)g->gydim + (size_t)j;
	else if (layout == MBGRID_SG_NORTHUP) k = (size_t)i + (size_t)(g->gydim - 1 - j) * (size_t)g->gxdim;
	else                                  k = (size_t)i + (size_t)j * (size_t)g->gxdim;
	return sgrid[k];
}

static int mb_sg_layout_ok(int32_t layout) {
	return layout == MBGRID_SG_SOUTHUP || layout == MBGRID_SG_NORTHUP ||
	       layout == MBGRID_SG_COLMAJOR;
}

/* One ring node visited by the GAP/NEAR search. Returns 1 when the search may stop:
 * immediately for NEAR (any data within the rings is enough), and for GAP once two
 * OPPOSITE directions have both seen data. Shared by the two ring walks below so the
 * test cannot drift between them. */
static int mb_ring_hit(int dxn, int dyn, int mode, int *dmask) {
	double r, fx = (double)dxn, fy = (double)dyn;
	int iii, jjj;
	if (mode == MBGRID_INTERP_NEAR) return 1;
	r = sqrt(fx * fx + fy * fy);
	if (r == 0.0) return 0;
	iii = (int)lrint(fx / r) + 1;
	jjj = (int)lrint(fy / r) + 1;
	dmask[iii * 3 + jjj] = 1;
	return (dmask[0] && dmask[8]) || (dmask[3] && dmask[5]) ||
	       (dmask[6] && dmask[2]) || (dmask[1] && dmask[7]);
}

MBGRID_API int mbgrid_fill(const mbgrid_params *p, float *work,
                           const float *sgrid, int32_t sgrid_layout, int64_t *n_spline) {
	mb_geom g;
	int32_t mode, clip;
	int i, j;
	int64_t nfill = 0;
	unsigned char *mask = NULL;
	int rc = mb_geometry(p, &g);

	if (rc != MBGRID_OK) return rc;
	if (work == NULL) return MBGRID_ERR_ARG;
	if (!mb_sg_layout_ok(sgrid_layout)) return MBGRID_ERR_LAYOUT;

	mb_resolve_clip(p, &g, &mode, &clip);
	if (mode == MBGRID_INTERP_NONE || sgrid == NULL) {
		if (n_spline) *n_spline = 0;
		return MBGRID_OK;
	}

	if (mode == MBGRID_INTERP_ALL) {
		for (i = 0; i < g.gxdim; i++)
			for (j = 0; j < g.gydim; j++) {
				size_t k = (size_t)i * (size_t)g.gydim + (size_t)j;
				float s = mb_sg(sgrid, &g, i, j, sgrid_layout);
				if (isnan(work[k]) && !isnan(s) && s < MB_ZFLAG) { work[k] = s; nfill++; }
			}
		if (n_spline) *n_spline = nfill;
		return MBGRID_OK;
	}

	/* GAP / NEAR both build a mask first and only then write, so a node filled early can
	 * never act as "data" for a later node. */
	mask = (unsigned char *)calloc((size_t)g.gxdim * (size_t)g.gydim, 1);
	if (mask == NULL) return MBGRID_ERR_MEMORY;

	for (i = 0; i < g.gxdim; i++) {
		for (j = 0; j < g.gydim; j++) {
			size_t k = (size_t)i * (size_t)g.gydim + (size_t)j;
			float s = mb_sg(sgrid, &g, i, j, sgrid_layout);
			int dmask[9], ir, ii, jj, i1, i2, j1, j2, done = 0;

			if (!isnan(work[k])) continue;
			if (isnan(s) || s >= MB_ZFLAG) continue;
			memset(dmask, 0, sizeof(dmask));

			for (ir = 0; ir <= clip && !done; ir++) {
				i1 = MB_MAX(0, i - ir);
				i2 = MB_MIN(g.gxdim - 1, i + ir);
				j1 = MB_MAX(0, j - ir);
				j2 = MB_MIN(g.gydim - 1, j + ir);

				/* the four sides of the ring; the corners are visited twice, which costs
				 * nothing and is what the original did */
				for (ii = i1; ii <= i2 && !done; ii++) {
					int sides[2], t;
					sides[0] = j1; sides[1] = j2;
					for (t = 0; t < 2 && !done; t++) {
						jj = sides[t];
						if (isnan(work[(size_t)ii * (size_t)g.gydim + (size_t)jj])) continue;
						done = mb_ring_hit(ii - i, jj - j, mode, dmask);
					}
				}
				for (jj = j1; jj <= j2 && !done; jj++) {
					int sides[2], t;
					sides[0] = i1; sides[1] = i2;
					for (t = 0; t < 2 && !done; t++) {
						ii = sides[t];
						if (isnan(work[(size_t)ii * (size_t)g.gydim + (size_t)jj])) continue;
						done = mb_ring_hit(ii - i, jj - j, mode, dmask);
					}
				}
			}
			if (done) mask[k] = 1;
		}
	}

	for (i = 0; i < g.gxdim; i++)
		for (j = 0; j < g.gydim; j++) {
			size_t k = (size_t)i * (size_t)g.gydim + (size_t)j;
			if (!mask[k]) continue;
			work[k] = mb_sg(sgrid, &g, i, j, sgrid_layout);
			nfill++;
		}

	free(mask);
	if (n_spline) *n_spline = nfill;
	return MBGRID_OK;
}

/* ------------------------------------------------------------------------------------
 * step 4 -- crop the -E margins and emit
 * ---------------------------------------------------------------------------------- */

MBGRID_API int mbgrid_extract(const mbgrid_params *p, const float *work,
                              float *out, int32_t layout, mbgrid_stats *st) {
	mb_geom g;
	int i, j;
	double zmin = DBL_MAX, zmax = -DBL_MAX;
	int64_t nset = 0, nunset = 0;
	int rc = mb_geometry(p, &g);

	if (rc != MBGRID_OK) return rc;
	if (work == NULL || out == NULL) return MBGRID_ERR_ARG;
	if (layout != MBGRID_LAY_BCB && layout != MBGRID_LAY_TRB) return MBGRID_ERR_LAYOUT;

	for (i = 0; i < g.nx; i++) {
		for (j = 0; j < g.ny; j++) {
			size_t kw = (size_t)(i + g.offx) * (size_t)g.gydim + (size_t)(j + g.offy);
			size_t ko = (layout == MBGRID_LAY_BCB)
			          ? (size_t)i * (size_t)g.ny + (size_t)j
			          : (size_t)(g.ny - 1 - j) * (size_t)g.nx + (size_t)i;
			float v = work[kw];
			/* a spline node the solver never reached is not a value, it is a hole */
			if (!isnan(v) && (double)v >= MB_ZFLAG) v = (float)NAN;
			out[ko] = v;
			if (isnan(v)) { nunset++; continue; }
			nset++;
			if (v < zmin) zmin = v;
			if (v > zmax) zmax = v;
		}
	}

	if (st) {
		st->zmin = (nset > 0) ? zmin : 0.0;
		st->zmax = (nset > 0) ? zmax : 0.0;
		st->n_unset = nunset;
		/* n_data / n_set / n_spline are filled by the caller that ran the earlier steps;
		 * mbgrid_run does it. Leave them alone here if already set. */
	}
	return MBGRID_OK;
}

/* ------------------------------------------------------------------------------------
 * one-shot
 * ---------------------------------------------------------------------------------- */

MBGRID_API int mbgrid_run(const mbgrid_params *p,
                          const double *x, const double *y, const double *z, int64_t n,
                          const mbgrid_breakline *bl,
                          float *out, int32_t layout, mbgrid_stats *st) {
	mb_geom g;
	int32_t mode, clip;
	size_t nw;
	float *work = NULL, *sgrid = NULL;
	int64_t n_data = 0, n_set = 0, n_spline = 0;
	int rc = mb_geometry(p, &g);

	if (rc != MBGRID_OK) return rc;
	if (out == NULL) return MBGRID_ERR_ARG;
	/* Checked BEFORE any work: a bad layout code must not cost a full grid solution
	 * before mbgrid_extract gets round to rejecting it. */
	if (layout != MBGRID_LAY_BCB && layout != MBGRID_LAY_TRB) return MBGRID_ERR_LAYOUT;

	nw    = (size_t)g.gxdim * (size_t)g.gydim;
	work  = (float *)malloc(nw * sizeof(float));
	if (work == NULL) return MBGRID_ERR_MEMORY;

	rc = mbgrid_bin(p, x, y, z, n, bl, work, &n_data, &n_set);
	if (rc != MBGRID_OK) { free(work); return rc; }

	mb_resolve_clip(p, &g, &mode, &clip);
	if (mode != MBGRID_INTERP_NONE && clip > 0) {
		sgrid = (float *)calloc(nw, sizeof(float));
		if (sgrid == NULL) { free(work); return MBGRID_ERR_MEMORY; }
		rc = mbgrid_zgrid(p, work, sgrid, clip);
		if (rc == MBGRID_OK)
			rc = mbgrid_fill(p, work, sgrid, MBGRID_SG_SOUTHUP, &n_spline);
		free(sgrid);
		if (rc != MBGRID_OK) { free(work); return rc; }
	}

	rc = mbgrid_extract(p, work, out, layout, st);
	free(work);
	if (rc != MBGRID_OK) return rc;

	if (st) {
		st->n_data   = n_data;
		st->n_set    = n_set;
		st->n_spline = n_spline;
	}
	return MBGRID_OK;
}

MBGRID_API const char *mbgrid_strerror(int code) {
	switch (code) {
		case MBGRID_OK:             return "ok";
		case MBGRID_ERR_ARG:        return "bad argument (NULL buffer, capacity too small, or unknown registration)";
		case MBGRID_ERR_REGION:     return "bad region: need east>west, north>south, xinc>0, yinc>0";
		case MBGRID_ERR_TOO_SMALL:  return "grid must have at least 4 nodes in each direction";
		case MBGRID_ERR_NO_DATA:    return "no data point fell inside the region";
		case MBGRID_ERR_MEMORY:     return "out of memory";
		case MBGRID_ERR_LAYOUT:     return "unknown layout code";
		default:                    return "unknown error";
	}
}
