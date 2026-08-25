/*======================================================================================
 * test_mbgrid.c -- standalone checks for mbgrid.c.  No framework, no GMT, no Julia:
 *     cc -std=c99 -O2 -Wall -Wextra mbgrid.c test_mbgrid.c -lm -o test_mbgrid && ./test_mbgrid
 * Exits non-zero on the first failure batch, and prints one line per check.
 *====================================================================================*/

#include "mbgrid.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;

static void ok(int cond, const char *what) {
	checks++;
	if (!cond) { failures++; printf("  FAIL  %s\n", what); }
	else        printf("  ok    %s\n", what);
}

static void ok_near(double got, double want, double tol, const char *what) {
	checks++;
	if (!(fabs(got - want) <= tol)) {
		failures++;
		printf("  FAIL  %s  (got %.10g, want %.10g, tol %g)\n", what, got, want, tol);
	}
	else printf("  ok    %s  (%.6g)\n", what, got);
}

static mbgrid_params base_params(void) {
	mbgrid_params p;
	memset(&p, 0, sizeof(p));
	p.west = 0.0; p.east = 10.0; p.south = 0.0; p.north = 10.0;
	p.xinc = 0.5; p.yinc = 0.5;
	p.scale = 1.0;
	p.clipmode = MBGRID_INTERP_ALL;
	return p;
}

/* z = a + b*x + c*y */
static double plane(double x, double y) { return 3.0 + 2.0 * x - 1.5 * y; }

/* ---------------------------------------------------------------------------------- */

static void test_geometry(void) {
	mbgrid_params p = base_params();
	int32_t nx = 0, ny = 0, gx = 0, gy = 0, ox = -1, oy = -1;
	double x0 = 0.0, y0 = 0.0;

	printf("geometry\n");
	ok(mbgrid_dims(&p, &nx, &ny) == MBGRID_OK && nx == 21 && ny == 21, "node counts from -R/-I");
	ok(mbgrid_work_dims(&p, &gx, &gy, &ox, &oy) == MBGRID_OK && gx == 21 && gy == 21 &&
	   ox == 0 && oy == 0, "no -E: working grid == output grid");
	ok(mbgrid_work_origin(&p, &x0, &y0) == MBGRID_OK && x0 == 0.0 && y0 == 0.0,
	   "no -E: working origin is the region corner");

	p.extend = 0.25;
	ok(mbgrid_work_dims(&p, &gx, &gy, &ox, &oy) == MBGRID_OK && ox == 5 && oy == 5 &&
	   gx == 31 && gy == 31, "-E0.25 adds 5 nodes per side");
	ok(mbgrid_work_origin(&p, &x0, &y0) == MBGRID_OK && x0 == -2.5 && y0 == -2.5,
	   "-E0.25 moves the working origin out by 5 cells");

	/* Pixel registration counts CELLS, not nodes, and puts the first node half a cell in.
	 * The Interpolate dialog's registration checkbox is shared by every method, so mbgrid
	 * has to answer it like the GMT modules beside it do. */
	p = base_params();
	p.registration = MBGRID_REG_PIXEL;
	ok(mbgrid_dims(&p, &nx, &ny) == MBGRID_OK && nx == 20 && ny == 20, "pixel reg: 20 cells, not 21 nodes");
	ok(mbgrid_work_origin(&p, &x0, &y0) == MBGRID_OK && x0 == 0.25 && y0 == 0.25,
	   "pixel reg: first node sits half a cell inside the region");

	p = base_params();
	p.xinc = 0.0;
	ok(mbgrid_dims(&p, &nx, &ny) == MBGRID_ERR_REGION, "zero increment rejected");
	p = base_params();
	p.east = 1.0; p.xinc = 0.5;
	ok(mbgrid_dims(&p, &nx, &ny) == MBGRID_ERR_TOO_SMALL, "grid narrower than 4 nodes rejected");
	p = base_params();
	p.registration = 7;
	ok(mbgrid_dims(&p, &nx, &ny) == MBGRID_ERR_ARG, "unknown registration rejected");
}

static void test_binning(void) {
	mbgrid_params p = base_params();
	int32_t gx, gy;
	float *work;
	int64_t ndat = 0, nset = 0;
	double x[1], y[1], z[1];

	printf("binning\n");
	mbgrid_work_dims(&p, &gx, &gy, NULL, NULL);
	work = malloc((size_t)gx * gy * sizeof(float));

	x[0] = 5.0; y[0] = 5.0; z[0] = 42.0;
	ok(mbgrid_bin(&p, x, y, z, 1, NULL, work, &ndat, &nset) == MBGRID_OK, "single point bins");
	ok(ndat == 1 && nset == 1, "one point sets exactly one node");
	ok_near(work[10 * (size_t)gy + 10], 42.0, 1e-5, "the node it landed on holds its value");
	ok(isnan(work[11 * (size_t)gy + 10]), "the neighbour it only leaked weight into stays NaN");

	x[0] = 100.0; y[0] = 100.0;
	ok(mbgrid_bin(&p, x, y, z, 1, NULL, work, &ndat, &nset) == MBGRID_ERR_NO_DATA,
	   "a point outside the region is no data at all");

	/* A plane sampled at every node must come back as the plane: the Gaussian
	 * neighbourhood is symmetric, so its weighted mean of a linear field is the centre. */
	{
		int i, j, k = 0, bad = 0;
		int n = 21 * 21;
		double *px = malloc(n * sizeof(double));
		double *py = malloc(n * sizeof(double));
		double *pz = malloc(n * sizeof(double));
		for (i = 0; i < 21; i++)
			for (j = 0; j < 21; j++, k++) {
				px[k] = i * 0.5; py[k] = j * 0.5; pz[k] = plane(px[k], py[k]);
			}
		ok(mbgrid_bin(&p, px, py, pz, n, NULL, work, &ndat, &nset) == MBGRID_OK, "plane bins");
		ok(nset == 441, "every node is set");
		for (i = 3; i < 18; i++)
			for (j = 3; j < 18; j++) {
				double want = plane(i * 0.5, j * 0.5);
				if (fabs(work[(size_t)i * gy + j] - want) > 1e-3) bad++;
			}
		ok(bad == 0, "interior nodes reproduce the plane exactly");
		free(px); free(py); free(pz);
	}
	free(work);
}

static void test_nodes_roundtrip(void) {
	mbgrid_params p = base_params();
	int32_t gx, gy;
	float *work;
	double x[3], y[3], z[3], xo[8], yo[8], zo[8];
	int64_t cnt;

	printf("node extraction (the GMT.surface path)\n");
	mbgrid_work_dims(&p, &gx, &gy, NULL, NULL);
	work = malloc((size_t)gx * gy * sizeof(float));

	x[0] = 1.0; y[0] = 1.0; z[0] = 10.0;
	x[1] = 5.0; y[1] = 5.0; z[1] = 20.0;
	x[2] = 9.0; y[2] = 9.0; z[2] = 30.0;
	mbgrid_bin(&p, x, y, z, 3, NULL, work, NULL, NULL);

	cnt = mbgrid_nodes(&p, work, NULL, NULL, NULL, 0);
	ok(cnt == 3, "counting pass returns the set-node count");
	cnt = mbgrid_nodes(&p, work, xo, yo, zo, 8);
	ok(cnt == 3, "extraction pass returns the same count");
	ok_near(xo[0], 1.0, 1e-9, "first node x");
	ok_near(yo[0], 1.0, 1e-9, "first node y");
	ok_near(zo[2], 30.0, 1e-4, "last node z");
	ok(mbgrid_nodes(&p, work, xo, yo, zo, 2) == MBGRID_ERR_ARG, "too-small capacity is an error");
	free(work);
}

static void test_zgrid_plane(void) {
	/* Minimum curvature through samples of a plane is that plane, so a hole punched in
	 * one must be filled with the plane's own values. */
	mbgrid_params p = base_params();
	int32_t gx, gy;
	float *work, *sgrid;
	int i, j, worst_i = 0, worst_j = 0;
	double worst = 0.0;
	int n = 21 * 21, k = 0;
	double *px = malloc(n * sizeof(double));
	double *py = malloc(n * sizeof(double));
	double *pz = malloc(n * sizeof(double));

	printf("zgrid\n");
	mbgrid_work_dims(&p, &gx, &gy, NULL, NULL);
	work  = malloc((size_t)gx * gy * sizeof(float));
	sgrid = calloc((size_t)gx * gy, sizeof(float));

	for (i = 0; i < 21; i++)
		for (j = 0; j < 21; j++) {
			/* leave a 5x5 hole in the middle */
			if (i >= 8 && i <= 12 && j >= 8 && j <= 12) continue;
			px[k] = i * 0.5; py[k] = j * 0.5; pz[k] = plane(px[k], py[k]); k++;
		}
	mbgrid_bin(&p, px, py, pz, k, NULL, work, NULL, NULL);
	ok(isnan(work[10 * (size_t)gy + 10]), "the hole is a hole after binning");

	ok(mbgrid_zgrid(&p, work, sgrid, 32) == MBGRID_OK, "zgrid runs");
	for (i = 8; i <= 12; i++)
		for (j = 8; j <= 12; j++) {
			double got = sgrid[i + (size_t)j * gx];
			double want = plane(i * 0.5, j * 0.5);
			if (fabs(got - want) > worst) { worst = fabs(got - want); worst_i = i; worst_j = j; }
		}
	printf("        worst hole node (%d,%d), plane range %g\n", worst_i, worst_j,
	       plane(10.0, 0.0) - plane(0.0, 10.0));
	/* 100 relaxation sweeps (the original's itmax) leave a residual of a few 1e-4 of the
	 * field range at the corners of the hole. That is the algorithm, not the port. */
	ok_near(worst, 0.0, 2e-2, "the hole is filled with the plane");

	/* Data nodes must not have moved. */
	ok_near(sgrid[2 + (size_t)2 * gx], plane(1.0, 1.0), 1e-3, "a data node is left pinned");

	free(work); free(sgrid); free(px); free(py); free(pz);
}

static void test_clip_modes(void) {
	mbgrid_params p = base_params();
	int32_t gx, gy;
	float *work, *sgrid, *out;
	double x[2], y[2], z[2];
	int64_t nfill_all = 0, nfill_near = 0, nfill_none = 0;
	mbgrid_stats st;

	printf("clip modes\n");
	mbgrid_work_dims(&p, &gx, &gy, NULL, NULL);
	out = malloc((size_t)21 * 21 * sizeof(float));

	/* two isolated points, far apart: NEAR with a small radius must fill only their
	 * neighbourhoods, ALL must fill the whole grid, NONE nothing. */
	x[0] = 2.0; y[0] = 2.0; z[0] = 0.0;
	x[1] = 8.0; y[1] = 8.0; z[1] = 10.0;

	{
		work  = malloc((size_t)gx * gy * sizeof(float));
		sgrid = calloc((size_t)gx * gy, sizeof(float));
		mbgrid_bin(&p, x, y, z, 2, NULL, work, NULL, NULL);
		mbgrid_zgrid(&p, work, sgrid, 32);

		p.clipmode = MBGRID_INTERP_NONE;
		mbgrid_fill(&p, work, sgrid, MBGRID_SG_SOUTHUP, &nfill_none);
		ok(nfill_none == 0, "-Co fills nothing");

		p.clipmode = MBGRID_INTERP_NEAR; p.clip = 2;
		mbgrid_fill(&p, work, sgrid, MBGRID_SG_SOUTHUP, &nfill_near);
		ok(nfill_near > 0 && nfill_near < 100, "-C2n fills only around the data");

		p.clipmode = MBGRID_INTERP_ALL;
		mbgrid_fill(&p, work, sgrid, MBGRID_SG_SOUTHUP, &nfill_all);
		mbgrid_extract(&p, work, out, MBGRID_LAY_BCB, &st);
		ok(st.n_unset == 0, "the default fills every node");
		free(work); free(sgrid);
	}

	/* -Cg must NOT fill outside the convex-ish span of the data: a node with data on one
	 * side only stays empty. */
	{
		int64_t nfill_gap = 0;
		work  = malloc((size_t)gx * gy * sizeof(float));
		sgrid = calloc((size_t)gx * gy, sizeof(float));
		p.clipmode = MBGRID_INTERP_GAP; p.clip = 6;
		mbgrid_bin(&p, x, y, z, 2, NULL, work, NULL, NULL);
		mbgrid_zgrid(&p, work, sgrid, 32);
		mbgrid_fill(&p, work, sgrid, MBGRID_SG_SOUTHUP, &nfill_gap);
		mbgrid_extract(&p, work, out, MBGRID_LAY_BCB, &st);
		ok(nfill_gap > 0, "-C6g fills the gap between the two points");
		ok(st.n_unset > 0, "-C6g leaves the outside alone");
		free(work); free(sgrid);
	}
	free(out);
}

static void test_sgrid_layouts(void) {
	/* The three sgrid layouts describe the SAME solution. zgrid writes SOUTHUP; a
	 * GMT.surface answer arrives as COLMAJOR ("BCB") and must merge identically without
	 * anyone transposing it on the way in. */
	mbgrid_params p = base_params();
	int32_t gx, gy;
	float *work, *sg_south, *sg_col, *sg_north, *w_south, *w_col, *w_north;
	int i, j, bad_col = 0, bad_north = 0;
	int64_t f_south = 0, f_col = 0, f_north = 0;
	size_t nw;
	double x[2], y[2], z[2];

	printf("sgrid layouts\n");
	mbgrid_work_dims(&p, &gx, &gy, NULL, NULL);
	nw = (size_t)gx * (size_t)gy;
	work     = malloc(nw * sizeof(float));
	sg_south = calloc(nw, sizeof(float));
	sg_col   = malloc(nw * sizeof(float));
	sg_north = malloc(nw * sizeof(float));
	w_south  = malloc(nw * sizeof(float));
	w_col    = malloc(nw * sizeof(float));
	w_north  = malloc(nw * sizeof(float));

	x[0] = 2.0; y[0] = 2.0; z[0] = 0.0;
	x[1] = 8.0; y[1] = 8.0; z[1] = 10.0;
	mbgrid_bin(&p, x, y, z, 2, NULL, work, NULL, NULL);
	mbgrid_zgrid(&p, work, sg_south, 32);

	for (i = 0; i < gx; i++)
		for (j = 0; j < gy; j++) {
			float v = sg_south[(size_t)i + (size_t)j * gx];
			sg_col[(size_t)i * gy + j] = v;
			sg_north[(size_t)i + (size_t)(gy - 1 - j) * gx] = v;
		}

	memcpy(w_south, work, nw * sizeof(float));
	memcpy(w_col,   work, nw * sizeof(float));
	memcpy(w_north, work, nw * sizeof(float));
	ok(mbgrid_fill(&p, w_south, sg_south, MBGRID_SG_SOUTHUP,  &f_south) == MBGRID_OK, "fill SOUTHUP");
	ok(mbgrid_fill(&p, w_col,   sg_col,   MBGRID_SG_COLMAJOR, &f_col)   == MBGRID_OK, "fill COLMAJOR");
	ok(mbgrid_fill(&p, w_north, sg_north, MBGRID_SG_NORTHUP,  &f_north) == MBGRID_OK, "fill NORTHUP");
	ok(f_south == f_col && f_south == f_north, "all three fill the same node count");
	for (i = 0; i < (int)nw; i++) {
		if (isnan(w_south[i]) != isnan(w_col[i]) ||
		    (!isnan(w_south[i]) && w_south[i] != w_col[i])) bad_col++;
		if (isnan(w_south[i]) != isnan(w_north[i]) ||
		    (!isnan(w_south[i]) && w_south[i] != w_north[i])) bad_north++;
	}
	ok(bad_col == 0, "COLMAJOR merges the same values as SOUTHUP");
	ok(bad_north == 0, "NORTHUP merges the same values as SOUTHUP");
	ok(mbgrid_fill(&p, w_south, sg_south, 99, NULL) == MBGRID_ERR_LAYOUT,
	   "an unknown sgrid layout is rejected");

	free(work); free(sg_south); free(sg_col); free(sg_north);
	free(w_south); free(w_col); free(w_north);
}

static void test_layouts(void) {
	mbgrid_params p = base_params();
	float *out_bcb, *out_trb;
	int i, j, bad = 0;
	int n = 21 * 21, k = 0;
	double *px = malloc(n * sizeof(double));
	double *py = malloc(n * sizeof(double));
	double *pz = malloc(n * sizeof(double));

	printf("output layouts\n");
	for (i = 0; i < 21; i++)
		for (j = 0; j < 21; j++, k++) {
			px[k] = i * 0.5; py[k] = j * 0.5; pz[k] = plane(px[k], py[k]);
		}
	out_bcb = malloc((size_t)n * sizeof(float));
	out_trb = malloc((size_t)n * sizeof(float));
	ok(mbgrid_run(&p, px, py, pz, n, NULL, out_bcb, MBGRID_LAY_BCB, NULL) == MBGRID_OK, "run BCB");
	ok(mbgrid_run(&p, px, py, pz, n, NULL, out_trb, MBGRID_LAY_TRB, NULL) == MBGRID_OK, "run TRB");
	for (i = 0; i < 21; i++)
		for (j = 0; j < 21; j++)
			if (out_bcb[i * 21 + j] != out_trb[(20 - j) * 21 + i]) bad++;
	ok(bad == 0, "BCB and TRB are the same grid, transposed and flipped");
	/* CORNER nodes carry the Gaussian binning's edge bias: half the weighting kernel
	 * falls outside the grid, so the weighted mean of a linear field is pulled inward by
	 * ~0.1% of the range. Interior exactness is asserted in test_binning(). */
	ok_near(out_bcb[0], plane(0.0, 0.0), 5e-2, "BCB node 0 is the SW corner");
	ok_near(out_trb[0], plane(0.0, 10.0), 5e-2, "TRB node 0 is the NW corner");
	free(out_bcb); free(out_trb); free(px); free(py); free(pz);
}

static void test_pixel_registration(void) {
	/* A pixel-registered run over the same plane: 20x20 cells, node centres offset half a
	 * cell from the region corner. Both facts are visible in the output values. */
	mbgrid_params p = base_params();
	float *out;
	int i, j, k = 0, bad = 0;
	int32_t nx = 0, ny = 0;
	int n = 20 * 20;
	double *px = malloc(n * sizeof(double));
	double *py = malloc(n * sizeof(double));
	double *pz = malloc(n * sizeof(double));

	printf("pixel registration\n");
	p.registration = MBGRID_REG_PIXEL;
	mbgrid_dims(&p, &nx, &ny);
	for (i = 0; i < 20; i++)
		for (j = 0; j < 20; j++, k++) {
			px[k] = 0.25 + i * 0.5; py[k] = 0.25 + j * 0.5; pz[k] = plane(px[k], py[k]);
		}
	out = malloc((size_t)nx * ny * sizeof(float));
	ok(mbgrid_run(&p, px, py, pz, n, NULL, out, MBGRID_LAY_BCB, NULL) == MBGRID_OK, "pixel-reg run");
	for (i = 3; i < 17; i++)
		for (j = 3; j < 17; j++)
			if (fabs(out[i * (size_t)ny + j] - plane(0.25 + i * 0.5, 0.25 + j * 0.5)) > 1e-3) bad++;
	ok(bad == 0, "cell centres reproduce the plane");
	free(out); free(px); free(py); free(pz);
}

static void test_breakline(void) {
	mbgrid_params p = base_params();
	int32_t gx, gy;
	float *work;
	double x[4], y[4], z[4];
	double bx[2], by[2], bz[2];
	int32_t seglen[1];
	mbgrid_breakline bl;
	int64_t nset = 0;

	printf("breakline\n");
	mbgrid_work_dims(&p, &gx, &gy, NULL, NULL);
	work = malloc((size_t)gx * gy * sizeof(float));

	/* four corners of a square at height 0, and a ridge at height 100 across the middle */
	x[0] = 4.0; y[0] = 4.0; z[0] = 0.0;
	x[1] = 6.0; y[1] = 4.0; z[1] = 0.0;
	x[2] = 4.0; y[2] = 6.0; z[2] = 0.0;
	x[3] = 6.0; y[3] = 6.0; z[3] = 0.0;
	bx[0] = 3.0; by[0] = 5.0; bz[0] = 100.0;
	bx[1] = 7.0; by[1] = 5.0; bz[1] = 100.0;
	seglen[0] = 2;
	memset(&bl, 0, sizeof(bl));
	bl.x = bx; bl.y = by; bl.z = bz; bl.seg_len = seglen; bl.nseg = 1;

	ok(mbgrid_bin(&p, x, y, z, 4, &bl, work, NULL, &nset) == MBGRID_OK, "breakline bins");
	ok_near(work[10 * (size_t)gy + 10], 100.0, 1e-3,
	        "a node on the breakline takes the breakline value, undiluted");
	ok(nset > 4, "the breakline sets nodes of its own");
	free(work);
}

static void test_translation_invariance(void) {
	/* The regression for gmtmbgrid.c:1564, where mb_zgrid indexed its array with world
	 * coordinates: shifting the whole problem in x,y must not change the result. */
	mbgrid_params p1 = base_params(), p2 = base_params();
	float *o1, *o2;
	int i, j, k = 0, bad = 0;
	int n = 21 * 21;
	double *px1 = malloc(n * sizeof(double)), *py1 = malloc(n * sizeof(double));
	double *px2 = malloc(n * sizeof(double)), *py2 = malloc(n * sizeof(double));
	double *pz  = malloc(n * sizeof(double));
	const double SHIFT_X = -25.0, SHIFT_Y = 37.0;

	printf("translation invariance (regression: world coords used as array indices)\n");
	for (i = 0; i < 21; i++)
		for (j = 0; j < 21; j++) {
			if (i >= 8 && i <= 12 && j >= 8 && j <= 12) continue;   /* same hole */
			px1[k] = i * 0.5;            py1[k] = j * 0.5;
			px2[k] = i * 0.5 + SHIFT_X;  py2[k] = j * 0.5 + SHIFT_Y;
			pz[k]  = plane(i * 0.5, j * 0.5);
			k++;
		}
	p2.west += SHIFT_X; p2.east += SHIFT_X; p2.south += SHIFT_Y; p2.north += SHIFT_Y;

	o1 = malloc((size_t)n * sizeof(float));
	o2 = malloc((size_t)n * sizeof(float));
	ok(mbgrid_run(&p1, px1, py1, pz, k, NULL, o1, MBGRID_LAY_BCB, NULL) == MBGRID_OK, "origin at 0,0");
	ok(mbgrid_run(&p2, px2, py2, pz, k, NULL, o2, MBGRID_LAY_BCB, NULL) == MBGRID_OK, "origin at -25,37");
	for (i = 0; i < n; i++) {
		if (isnan(o1[i]) != isnan(o2[i])) { bad++; continue; }
		if (!isnan(o1[i]) && fabs(o1[i] - o2[i]) > 1e-3) bad++;
	}
	ok(bad == 0, "both grids are identical");
	free(o1); free(o2); free(px1); free(py1); free(px2); free(py2); free(pz);
}

static void test_extend(void) {
	/* -E only widens the working grid; the cropped output must still be registered on
	 * the requested region.  (gmtmbgrid.c mixed the extended and un-extended origins,
	 * which shifted the data by offx cells.) */
	mbgrid_params p = base_params();
	float *o0, *oE;
	int i, j, k = 0, bad = 0;
	int n = 21 * 21;
	double *px = malloc(n * sizeof(double));
	double *py = malloc(n * sizeof(double));
	double *pz = malloc(n * sizeof(double));

	printf("-E extension\n");
	for (i = 0; i < 21; i++)
		for (j = 0; j < 21; j++, k++) {
			px[k] = i * 0.5; py[k] = j * 0.5; pz[k] = plane(px[k], py[k]);
		}
	o0 = malloc((size_t)n * sizeof(float));
	oE = malloc((size_t)n * sizeof(float));
	mbgrid_run(&p, px, py, pz, n, NULL, o0, MBGRID_LAY_BCB, NULL);
	p.extend = 0.25;
	ok(mbgrid_run(&p, px, py, pz, n, NULL, oE, MBGRID_LAY_BCB, NULL) == MBGRID_OK, "-E0.25 runs");
	for (i = 0; i < n; i++)
		if (fabs(o0[i] - oE[i]) > 1e-3) bad++;
	ok(bad == 0, "the data still land on the same nodes with -E");
	free(o0); free(oE); free(px); free(py); free(pz);
}

static void test_stats_and_errors(void) {
	mbgrid_params p = base_params();
	mbgrid_stats st;
	float *out;
	int i, j, k = 0;
	int n = 21 * 21;
	double *px = malloc(n * sizeof(double));
	double *py = malloc(n * sizeof(double));
	double *pz = malloc(n * sizeof(double));

	printf("stats and error reporting\n");
	for (i = 0; i < 21; i++)
		for (j = 0; j < 21; j++, k++) {
			px[k] = i * 0.5; py[k] = j * 0.5; pz[k] = plane(px[k], py[k]);
		}
	out = malloc((size_t)n * sizeof(float));
	memset(&st, 0, sizeof(st));
	mbgrid_run(&p, px, py, pz, n, NULL, out, MBGRID_LAY_BCB, &st);
	ok(st.n_data == 441, "n_data counts the points that landed inside");
	ok(st.n_set == 441, "n_set counts the nodes data reached");
	ok(st.n_unset == 0, "n_unset is zero for a fully covered grid");
	ok_near(st.zmin, plane(0.0, 10.0), 5e-2, "zmin is the NW-most plane value (edge-biased)");
	ok_near(st.zmax, plane(10.0, 0.0), 5e-2, "zmax is the SE-most plane value (edge-biased)");
	ok(strcmp(mbgrid_strerror(MBGRID_ERR_NO_DATA), "unknown error") != 0, "errors have messages");
	ok(mbgrid_run(&p, px, py, pz, n, NULL, out, 99, NULL) == MBGRID_ERR_LAYOUT,
	   "an unknown layout code is rejected");
	free(out); free(px); free(py); free(pz);
}

static void test_stress(void) {
	/* Awkward on purpose: non-square grid, mutually prime node counts, a real-world
	 * geographic region far from the origin, scattered data with duplicates and NaNs,
	 * a multi-segment breakline, -E on, and the ring-search clip mode. */
	mbgrid_params p;
	mbgrid_stats st;
	float *out;
	int32_t nx, ny, seglen[3] = {4, 2, 5};
	int i, bad = 0;
	const int N = 20000;
	double *px = malloc(N * sizeof(double));
	double *py = malloc(N * sizeof(double));
	double *pz = malloc(N * sizeof(double));
	double bx[11], by[11], bz[11];
	mbgrid_breakline bl;
	unsigned int seed = 12345;
	int64_t n_dropped = 0, n_plain = 0;

	printf("stress\n");
	memset(&p, 0, sizeof(p));
	p.west = -25.0; p.east = -7.0; p.south = 30.0; p.north = 45.0;
	p.xinc = 18.0 / 136.0; p.yinc = 15.0 / 88.0;
	p.scale = 1.5; p.extend = 0.1; p.tension = 0.35;
	p.clipmode = MBGRID_INTERP_GAP; p.clip = 8;
	mbgrid_dims(&p, &nx, &ny);
	ok(nx == 137 && ny == 89, "non-square grid dimensions");

	/* Three separate passes so the drop count below is exact: generate, then make some
	 * points coincident, then spoil a fixed set of them. Chaining these in one pass makes
	 * a duplicate silently inherit a spoiled neighbour. */
	for (i = 0; i < N; i++) {
		double u = (double)(seed = seed * 1103515245u + 12345u) / 4294967296.0;
		double v = (double)(seed = seed * 1103515245u + 12345u) / 4294967296.0;
		px[i] = -26.0 + 20.0 * u;
		py[i] =  29.0 + 17.0 * v;
		pz[i] = -3000.0 * sin(px[i] * 0.7) * cos(py[i] * 0.5);
	}
	for (i = 1; i < N; i++)
		if (i % 501 == 0) { px[i] = px[i-1]; py[i] = py[i-1]; }   /* coincident points */
	for (i = 0; i < N; i++) {
		if (i % 250 == 0) { px[i] = 500.0; py[i] = 500.0; n_dropped++; }  /* far outside */
		else if (i % 997 == 0) { pz[i] = NAN; n_dropped++; }              /* NaN z       */
	}
	for (i = 0; i < 11; i++) { bx[i] = -20.0 + i * 1.0; by[i] = 37.0; bz[i] = -1234.0; }
	memset(&bl, 0, sizeof(bl));
	bl.x = bx; bl.y = by; bl.z = bz; bl.seg_len = seglen; bl.nseg = 3;

	out = malloc((size_t)nx * ny * sizeof(float));
	memset(&st, 0, sizeof(st));
	ok(mbgrid_run(&p, px, py, pz, N, NULL, out, MBGRID_LAY_BCB, &st) == MBGRID_OK, "stress run");
	n_plain = st.n_data;
	ok(n_plain == N - n_dropped, "far-outside points and NaN z are dropped, nothing else");

	memset(&st, 0, sizeof(st));
	ok(mbgrid_run(&p, px, py, pz, N, &bl, out, MBGRID_LAY_BCB, &st) == MBGRID_OK, "stress run + breakline");
	ok(st.n_data > n_plain, "the breakline adds densified points of its own");
	for (i = 0; i < nx * ny; i++)
		if (!isnan(out[i]) && !isfinite(out[i])) bad++;
	ok(bad == 0, "no infinities anywhere in the output");
	ok(st.zmin > -1.0e5 && st.zmax < 1.0e5, "no zbase or 1e35 sentinel leaked into the output");
	printf("        %lld data, %lld binned, %lld splined, %lld empty, z %.1f..%.1f\n",
	       (long long)st.n_data, (long long)st.n_set, (long long)st.n_spline,
	       (long long)st.n_unset, st.zmin, st.zmax);
	free(out); free(px); free(py); free(pz);
}

int main(void) {
	test_geometry();
	test_binning();
	test_nodes_roundtrip();
	test_zgrid_plane();
	test_clip_modes();
	test_sgrid_layouts();
	test_layouts();
	test_pixel_registration();
	test_breakline();
	test_translation_invariance();
	test_extend();
	test_stats_and_errors();
	test_stress();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
