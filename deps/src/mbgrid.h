/*======================================================================================
 * mbgrid.h -- Gaussian-weighted binning + spline gap filling, with NO GMT dependency.
 *
 * Distilled from gmtmbgrid.c (J. Luis, 2006-2018), itself distilled from MB-System's
 * MBGRID (D. W. Caress, 1993).  What survives here is the part GMT cannot do for you:
 *
 *   1. bin scattered x,y,z onto grid nodes with a Gaussian weight  (mbgrid_bin)
 *   2. fill the gaps with the IGPP/SIO thin-plate spline           (mbgrid_zgrid)
 *   3. decide WHICH gaps may be filled (the -C clip modes)         (mbgrid_fill)
 *   4. crop the working grid back to the requested region          (mbgrid_extract)
 *
 * What was DROPPED, and why:
 *   - mb_surface() and its ~1300 lines of helpers (iterate, fill_in_forecast,
 *     find_nearest_point, remove_planar_trend, get_prime_factors, ...): a fork of
 *     GMT 4-era surface.c.  Call GMT.surface on the binned nodes instead --
 *     mbgrid_nodes() hands you exactly the x,y,z triplets it wants, and
 *     mbgrid_fill() takes the solved grid back via sgrid_layout = MBGRID_SG_COLMAJOR.
 *   - option parsing, usage(), GMT record I/O, the -F background grid: caller's job.
 *
 * DELIBERATE DEVIATIONS from gmtmbgrid.c (all of them fixes; see mbgrid.c for detail):
 *   - mb_zgrid's "shift data points back" block is gone.  It indexed the z array with
 *     WORLD coordinates (gmtmbgrid.c:1564) where every other site in the same function
 *     divides by dx first -- an out-of-bounds write on any grid not starting at (0,0)
 *     with unit spacing.  For node-binned input the block is also provably a no-op.
 *   - breaklines are re-implemented; the original interp_breakline() dereferenced a
 *     NULL grid header on its first statement and can never have executed.
 *   - the unset-node marker is NaN, not the magic value 99999, which real bathymetry
 *     in mm (or any z > 99999) would collide with.
 *   - node indices come from floor(), not truncation, so the half-cell strip west/south
 *     of the working region is not folded onto column/row 0.
 *   - the working-grid origin is the -E extended corner everywhere; the original mixed
 *     the extended corner (binning) with the un-extended one (node coordinates), so
 *     -E shifted the data against the grid it was binned into.
 *   - the zmin/zmax scan starts at node 0, not node 1.
 *   - pixel registration is supported (the original was gridline-only), because the
 *     Interpolate dialog's registration checkbox is shared by every gridding method.
 *
 * Layouts.  Nothing here transposes anything silently; every buffer says what it is.
 *   work[]  : ALWAYS column-major over the working grid, work[ix*gydim + iy], y ascending.
 *   sgrid[] : MBGRID_SG_SOUTHUP  row-major, x fastest, row 0 = south (mbgrid_zgrid writes
 *                                this one);
 *             MBGRID_SG_NORTHUP  row-major, x fastest, row 0 = north;
 *             MBGRID_SG_COLMAJOR column-major, y ascending -- a GMT/GMT.jl "BCB" grid
 *                                straight off GMT.surface, handed over UNTRANSPOSED.
 *   out[]   : MBGRID_LAY_BCB, out[ix*ny + iy], y ascending -- GMT's own column-major
 *             order, the same "BCB" that gmtvtk_view_grid documents -- or
 *             MBGRID_LAY_TRB, out[(ny-1-iy)*nx + ix], row 0 = north.
 *
 * All buffers are caller-allocated: nothing crosses the DLL boundary that the caller
 * did not allocate and does not free.  Every entry point is stateless.
 *
 * ABI.  These structs are mirrored field-for-field in src/mbgrid.jl.  There is exactly
 * ONE version guard for everything this DLL exports -- gmtvtk_abi_version (90_c_api.cpp)
 * against _ABI_REQUIRED (src/libgmtvtk.jl).  Reorder or retype a field below and you
 * must bump that pair, exactly as for any other host-facing signature change; do not
 * grow a second, mbgrid-private version counter beside it.
 *====================================================================================*/

#ifndef MBGRID_H
#define MBGRID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define MBGRID_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define MBGRID_API __attribute__((visibility("default")))
#else
#  define MBGRID_API
#endif

/* ---- clipmode: which gaps the spline is allowed to fill (gmtmbgrid's -C) ---------- */
#define MBGRID_INTERP_NONE 0  /* -Co : bin only, no spline at all                     */
#define MBGRID_INTERP_GAP  1  /* -C<n>g : gaps up to 2*clip cells ACROSS (data on
                                 opposite sides within clip rings)                    */
#define MBGRID_INTERP_NEAR 2  /* -C<n>n : any node within clip rings of data          */
#define MBGRID_INTERP_ALL  3  /* default : every node the spline reached              */

/* ---- sgrid_layout ----------------------------------------------------------------- */
#define MBGRID_SG_SOUTHUP  0  /* sgrid[ix + iy*gxdim], row 0 = south (mbgrid_zgrid)   */
#define MBGRID_SG_NORTHUP  1  /* sgrid[ix + (gydim-1-iy)*gxdim], row 0 = north        */
#define MBGRID_SG_COLMAJOR 2  /* sgrid[ix*gydim + iy], y ascending -- a "BCB" grid    */

/* ---- output layout ----------------------------------------------------------------- */
#define MBGRID_LAY_BCB 0      /* out[ix*ny + iy], y ascending  (GMT / GMT.jl native)  */
#define MBGRID_LAY_TRB 1      /* out[(ny-1-iy)*nx + ix]        (row 0 = north)        */

/* ---- registration ------------------------------------------------------------------ */
#define MBGRID_REG_GRIDLINE 0 /* west/south are the CENTRES of the first row/column    */
#define MBGRID_REG_PIXEL    1 /* west/south are the EDGES; first node sits half a cell in */

/* ---- return codes ------------------------------------------------------------------ */
#define MBGRID_OK             0
#define MBGRID_ERR_ARG       -1
#define MBGRID_ERR_REGION    -2
#define MBGRID_ERR_TOO_SMALL -3   /* fewer than 4 nodes in a direction                */
#define MBGRID_ERR_NO_DATA   -4   /* no point landed inside the working region        */
#define MBGRID_ERR_MEMORY    -5
#define MBGRID_ERR_LAYOUT    -6

/* Leave nx/ny at 0 to have them derived from the region, the increments and the
 * registration.  Field order is part of the ABI -- doubles first, then ints; do not
 * reorder (see the ABI note at the top of this file). */
typedef struct mbgrid_params {
	double west, east, south, north;
	double xinc, yinc;
	double scale;    /* -W  Gaussian half-width in grid cells                    [1.0] */
	double extend;   /* -E  widen the working grid by extend*nx / extend*ny      [0.0] */
	double tension;  /* -T  zgrid tension; 0 = minimum curvature                 [0.0] */
	int32_t nx, ny;  /* 0 -> derived from the region, the increments and reg           */
	int32_t clipmode;/* MBGRID_INTERP_*                                                */
	int32_t clip;    /* -C  search radius in grid cells (GAP / NEAR)                   */
	int32_t verbose; /* !=0 -> iteration progress on stderr                            */
	int32_t registration; /* MBGRID_REG_GRIDLINE | MBGRID_REG_PIXEL                    */
} mbgrid_params;

/* Multi-segment breakline, densified to ~one point per grid cell before binning.
 * A node touched by a breakline is pinned to the breakline value (the original's
 * "in this bin we don't want anyone else" rule). */
typedef struct mbgrid_breakline {
	const double *x, *y, *z;
	const int32_t *seg_len;   /* rows in each segment */
	int32_t nseg;
	int32_t reserved;
} mbgrid_breakline;

typedef struct mbgrid_stats {
	double zmin, zmax;    /* over the OUTPUT region; 0,0 if it is entirely empty */
	int64_t n_data;       /* input points that landed inside the working region  */
	int64_t n_set;        /* nodes set from data                                 */
	int64_t n_spline;     /* nodes set by the spline                             */
	int64_t n_unset;      /* nodes still NaN in the output                       */
} mbgrid_stats;

/* Node counts of the OUTPUT grid (honours nx/ny if given, else derives them). */
MBGRID_API int mbgrid_dims(const mbgrid_params *p, int32_t *nx, int32_t *ny);

/* Node counts and corner offsets of the WORKING grid (output grid + -E margins).
 * work[] must hold gxdim*gydim floats; sgrid[] the same. Any out pointer may be NULL. */
MBGRID_API int mbgrid_work_dims(const mbgrid_params *p, int32_t *gxdim, int32_t *gydim,
                                int32_t *offx, int32_t *offy);

/* SW-most NODE CENTRE of the WORKING grid -- the origin every other entry point here
 * bins and indexes against. The caller needs it to build the region it hands a external
 * solver (GMT.surface), which must be the working region, not the output one. */
MBGRID_API int mbgrid_work_origin(const mbgrid_params *p, double *x0, double *y0);

/* Step 1. Gaussian-weighted binning. work[] is overwritten: the weighted mean where a
 * point fell in the node's own cell, NaN everywhere else. */
MBGRID_API int mbgrid_bin(const mbgrid_params *p,
                          const double *x, const double *y, const double *z, int64_t n,
                          const mbgrid_breakline *bl,
                          float *work, int64_t *n_data, int64_t *n_set);

/* Step 2a. Fill every node by thin-plate spline (IGPP/SIO zgrid). sgrid[] is overwritten.
 * `nrng` bounds the initial nearest-value sweep; pass the effective clip (or max(gxdim,gydim)).
 * Nodes the sweep never reached come back >= 1e35 and are ignored downstream. */
MBGRID_API int mbgrid_zgrid(const mbgrid_params *p, const float *work, float *sgrid, int32_t nrng);

/* Step 2b (alternative). The set nodes as x,y,z triplets, ready for GMT.surface.
 * Returns the count written, or a negative error. Pass xo=yo=zo=NULL to just count. */
MBGRID_API int64_t mbgrid_nodes(const mbgrid_params *p, const float *work,
                                double *xo, double *yo, double *zo, int64_t cap);

/* Step 3. Merge the solved grid into work[] wherever clipmode allows. */
MBGRID_API int mbgrid_fill(const mbgrid_params *p, float *work,
                           const float *sgrid, int32_t sgrid_layout, int64_t *n_spline);

/* Step 4. Crop the -E margins off and write the output grid. out[] holds nx*ny floats. */
MBGRID_API int mbgrid_extract(const mbgrid_params *p, const float *work,
                              float *out, int32_t layout, mbgrid_stats *st);

/* Steps 1-4 in one call, zgrid solver. `out` holds nx*ny floats; `st` may be NULL. */
MBGRID_API int mbgrid_run(const mbgrid_params *p,
                          const double *x, const double *y, const double *z, int64_t n,
                          const mbgrid_breakline *bl,
                          float *out, int32_t layout, mbgrid_stats *st);

MBGRID_API const char *mbgrid_strerror(int code);

#ifdef __cplusplus
}
#endif
#endif /* MBGRID_H */
