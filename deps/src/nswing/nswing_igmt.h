/*
 * nswing_igmt.h -- what the GMT library gave nswing.c, provided here instead.
 *
 * deps/src/nswing/nswing.c is the GMT supplement's nswing.c (GMT master, src/nswing/nswing.c) carrying
 * a fix GMT_jll has not shipped yet: GMT_get_bcr_z back to Mirone's unmasked bicubic. The masked one
 * divided by a weight sum of ~1e-10 on coastal nodes of a nested grid and wrote ~1e9 into its initial
 * state. Built INTO gmtvtk.dll, so it must not link GMT or netCDF: on Windows the user's GMT is
 * GMT_jll's MinGW build and gmtvtk is MSVC. So nothing GMT crosses this boundary:
 *
 *   - grids and point sets come from the host as plain buffers (igmt_nswing_grid / _points), named
 *     "@g<n>" / "@d<n>" on the command line, and are read through the GMT_Read_Data shim below;
 *   - messages go to the host's callback (or stderr);
 *   - netCDF is the library GMT already loaded into the process, its nc_* entry points resolved at run
 *     time from the path the host hands over. Only ints, sizes and our own buffers cross that boundary
 *     (no FILE*, no memory freed across), so the two C runtimes never meet.
 *
 * The simulation code itself is untouched. Delete this copy (and go back to `gmt("nswing ...")`) once
 * GMT_jll carries the fix.
 */
#ifndef NSWING_IGMT_H
#define NSWING_IGMT_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#  define NSWING_IGMT_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define NSWING_IGMT_API __attribute__((visibility("default")))
#else
#  define NSWING_IGMT_API
#endif

/* ----------------------------- host-facing types ----------------------------- */
typedef struct {
	const float *z;          /* the grid as it lies in the host's memory */
	int     nx, ny;          /* columns, rows */
	int     zlayout;         /* bit0 row-major, bit1 north-first ("BCB" = 0, "TRB" = 3), as gmtvtk's gridLay */
	int     registration;    /* 0 gridline, 1 pixel */
	int     is_geog;         /* the grid says it is geographic */
	double  wesn[4];
	double  inc[2];
	double  zmin, zmax;
} igmt_nswing_grid;

typedef struct {
	const double *x, *y;     /* n positions */
	const char  **text;      /* n names, or NULL */
	int     n;
} igmt_nswing_points;

/* level: 1 error, 2 warning, 4 information (GMT's GMT_MSG_* numbers); msg is one formatted message */
typedef void (*igmt_nswing_msg_fn)(int level, const char *msg);

/* workdir: where a RELATIVE file name on the command line lives (outputs, -O's boundary file), as if
   the run had been started in that directory; NULL or "" = the process's current directory. */
NSWING_IGMT_API int gmtvtk_nswing_run(int argc, const char **argv,
                                      const igmt_nswing_grid *grids, int ngrids,
                                      const igmt_nswing_points *points, int npoints,
                                      const char *netcdf_lib, const char *workdir, igmt_nswing_msg_fn msg);

/* ----------------------------- GMT names nswing.c uses ----------------------------- */
#define GMT_LOCAL static
#define EXTERN_MSC static

#define GMT_X 0
#define GMT_Y 1
#define XLO 0
#define XHI 1
#define YLO 2
#define YHI 3

#define GMT_LEN64  64
#define GMT_LEN128 128
#define GMT_LEN256 256
#define GMT_LEN512 512

#define GMT_MSG_QUIET       0
#define GMT_MSG_ERROR       1
#define GMT_MSG_WARNING     2
#define GMT_MSG_TICTOC      3
#define GMT_MSG_INFORMATION 4
#define GMT_MSG_DEBUG       6
#define GMT_TIME_NONE       0

#define GMT_NOERROR       0
#define GMT_PARSE_ERROR   72
#define GMT_OPT_INFILE    '<'
#define GMT_GRID_NODE_REG 0

/* Arguments of the GMT_Read_Data / GMT_Destroy_Data shims -- only `family` is looked at */
#define GMT_IS_DATASET         0
#define GMT_IS_GRID            1
#define GMT_IS_FILE            1
#define GMT_IS_POINT           1
#define GMT_IS_SURFACE         4
#define GMT_CONTAINER_AND_DATA 3
#define GMT_READ_NORMAL        0

#ifndef MIN
#  define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif
#ifndef MAX
#  define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#  define M_PI_2 1.57079632679489661923
#endif
#ifndef D2R
#  define D2R (M_PI / 180.0)
#endif
#ifndef R2D
#  define R2D (180.0 / M_PI)
#endif
#define irint(x) ((int)lrint(x))

struct GMT_GRID_HEADER {
	uint32_t n_columns, n_rows;
	uint32_t registration;
	double   wesn[4];
	double   inc[2];
	double   z_min, z_max;
	size_t   nm;
	void    *hidden;
};

struct GMT_GRID {                /* a VIEW of a host grid: nothing here is owned */
	struct GMT_GRID_HEADER *header;
	struct GMT_GRID_HEADER  hdr;
	const igmt_nswing_grid *src;
};

struct GMT_DATASEGMENT {
	uint64_t n_rows;
	double  *data[2];
	char   **text;
};
struct GMT_DATATABLE {
	uint64_t n_segments;
	struct GMT_DATASEGMENT **segment;
};
struct GMT_DATASET {             /* a VIEW of a host point set: one table, one segment */
	uint64_t n_tables, n_records;
	struct GMT_DATATABLE  **table;
	struct GMT_DATATABLE    tbl, *tblp;
	struct GMT_DATASEGMENT  seg, *segp;
};

/* ----------------------------- netCDF, resolved at run time ----------------------------- */
#define NC_NOERR     0
#define NC_GLOBAL    (-1)
#define NC_UNLIMITED 0L
#define NC_NETCDF4   0x1000
#define NC_CLOBBER   0x0000
#define NC_CHAR      2
#define NC_INT       4
#define NC_FLOAT     5
#define NC_DOUBLE    6
#define NC_UBYTE     7
#define NC_STRING    12
typedef int nc_type;

struct nswing_nc_api {
	int (*create)(const char *, int, int *);
	int (*close)(int);
	int (*enddef)(int);
	int (*def_dim)(int, const char *, size_t, int *);
	int (*def_var)(int, const char *, nc_type, int, const int *, int *);
	int (*def_var_deflate)(int, int, int, int, int);
	int (*put_att_text)(int, int, const char *, size_t, const char *);
	int (*put_att_double)(int, int, const char *, nc_type, size_t, const double *);
	int (*put_att_float)(int, int, const char *, nc_type, size_t, const float *);
	int (*put_att_int)(int, int, const char *, nc_type, size_t, const int *);
	int (*put_var_float)(int, int, const float *);
	int (*put_var_double)(int, int, const double *);
	int (*put_var_int)(int, int, const int *);
	int (*put_var_string)(int, int, const char **);
	int (*put_vara_float)(int, int, const size_t *, const size_t *, const float *);
	int (*put_vara_double)(int, int, const size_t *, const size_t *, const double *);
	int (*put_vara_int)(int, int, const size_t *, const size_t *, const int *);
	int (*put_vara_ubyte)(int, int, const size_t *, const size_t *, const unsigned char *);
	const char *(*strerror)(int);
};
extern struct nswing_nc_api nswing_nc;

#define nc_create          nswing_nc_create     /* resolves a relative name against the run's workdir */
#define nc_close           nswing_nc.close
#define nc_enddef          nswing_nc.enddef
#define nc_def_dim         nswing_nc.def_dim
#define nc_def_var         nswing_nc.def_var
#define nc_def_var_deflate nswing_nc.def_var_deflate
#define nc_put_att_text    nswing_nc.put_att_text
#define nc_put_att_double  nswing_nc.put_att_double
#define nc_put_att_float   nswing_nc.put_att_float
#define nc_put_att_int     nswing_nc.put_att_int
#define nc_put_var_float   nswing_nc.put_var_float
#define nc_put_var_double  nswing_nc.put_var_double
#define nc_put_var_int     nswing_nc.put_var_int
#define nc_put_var_string  nswing_nc.put_var_string
#define nc_put_vara_float  nswing_nc.put_vara_float
#define nc_put_vara_double nswing_nc.put_vara_double
#define nc_put_vara_int    nswing_nc.put_vara_int
#define nc_put_vara_ubyte  nswing_nc.put_vara_ubyte
#define nc_strerror        nswing_nc.strerror

#endif
