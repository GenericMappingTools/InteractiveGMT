/*======================================================================================
 * mbnavadjust_mbio.h -- the slice of MB-System's MBIO and MBAUX APIs that the mbnavadjust port
 * (deps/src/mbnavadjust/) uses on top of the swath editor's (deps/src/mbedit/mbedit_mbio.h).
 *
 * The viewer is built WITHOUT MB-System headers or import libraries: the MBIO library is loaded
 * at RUN time by the swath editor (mbedit_mbio_open), and mbna_mbio_open() picks the extra entry
 * points this tool needs out of that same library. Everything below is copied from MB-System
 * 5.8.3 (mb_define.h, mb_status.h, mb_format.h, mb_process.h, mb_aux.h, mbsys_ldeoih.h), values
 * and layouts unchanged.
 *
 * Two structures are SHARED MEMORY with the library:
 *   - struct mbsys_ldeoih_struct: mbnavadjust_import_file writes its section files (format 71)
 *     by filling the output store directly. Upstream reaches the store through
 *     mb_io_struct->store_data; the port asks MBIO for it with mb_get_store, so mb_io_struct (a
 *     big structure that changes between releases) is never needed.
 *   - the MBIO allocation book-keeping: memory the library frees (the store's arrays, on mb_close)
 *     is allocated with the library's own mb_mallocd, as upstream does.
 *
 * libmbaux is NOT an MBIO dependency, so the pieces of it mbnavadjust uses (the swath contourer
 * mb_truecont.c, the Delaunay triangulator mb_delaun.c, LSQR from mb_cheb.c) are carried in
 * mbna_aux.c, unchanged, under mbna_-prefixed names (the #defines below) so they can never bind to
 * a real libmbaux loaded in the same process, nor to another ported tool's copy.
 *
 * MB-System's grid reader (mb_check_gmt_grd / mb_read_gmt_grd, GMT C API) is a host hook: the
 * Qt window reads the grid through the viewer and hands it over (mbnavadjust_window.cpp).
 *====================================================================================*/

#ifndef MBNAVADJUST_MBIO_H_
#define MBNAVADJUST_MBIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#	include <direct.h>
#	include <io.h>
#	include <process.h>
#	define getcwd _getcwd
#	define mkdir(...) _mkdir(__VA_ARGS__)
#	ifndef R_OK
#		define R_OK 4
#	endif
#	ifndef W_OK
#		define W_OK 2
#	endif
#	ifndef F_OK
#		define F_OK 0
#	endif
#	define access _access
#	ifndef S_IFMT
#		define S_IFMT _S_IFMT
#	endif
#	ifndef S_IFDIR
#		define S_IFDIR _S_IFDIR
#	endif
#	ifndef S_ISDIR
#		define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#	endif
#	ifndef S_ISREG
#		define S_ISREG(m) (((m) & S_IFMT) == _S_IFREG)
#	endif
#else
#	include <unistd.h>
#endif

#include "../mbedit/mbedit_mbio.h"

#define MBNAVADJUST_API

/* ---- mb_define.h ------------------------------------------------------------------- */
#define MB_DEFINE_DEF
#ifndef MB_VERSION
/* the version of the LOADED library, as the swath editor's loader reports it (deps/src/mbedit/mbedit.c) */
const char *mbedit_mbio_version(void);
int mbedit_mbio_loaded(void);
#	define MB_VERSION (mbedit_mbio_version())
#endif
#define MB_VERSION_DATE ""
#define MB_PATHPLUS_MAXLINE 1152
#define MB_PATHPLUSPLUS_MAXLINE 2304
#define MB_NAME_LENGTH 32
#define MB_LONGNAME_LENGTH 128
#define MB_COMMAND_LENGTH 8192
typedef char mb_pathplus[MB_PATHPLUS_MAXLINE];
typedef char mb_pathplusplus[MB_PATHPLUSPLUS_MAXLINE];
typedef char mb_name[MB_NAME_LENGTH];
typedef char mb_longname[MB_LONGNAME_LENGTH];
typedef char mb_command[MB_COMMAND_LENGTH];
typedef unsigned char mb_u_char;
typedef signed char mb_s_char;
#ifndef M_PI
#	define M_PI 3.14159265358979323846
#endif
#ifndef DTR
#	define DTR 0.01745329251994329500
#endif

/* ---- mb_status.h ------------------------------------------------------------------- */
#define MB_YES 1
#define MB_NO 0
#define MB_DATA_NONE 0
#define MB_DATA_COMMENT 2
#define MB_DATA_NAV 12
#define MB_ERROR_WRITE_FAIL 5
#define MB_ERROR_BAD_DATA 15
#define MB_ERROR_IGNORE -10
#define MB_ERROR_INIT_FAIL -24
#ifndef MB_PROCESSED_USE
#	define MB_PROCESSED_NONE 0
#	define MB_PROCESSED_USE 2
#endif
#define MB_TOPOGRAPHY_TYPE_UNKNOWN 0
#define MB_TOPOGRAPHY_TYPE_INTERFEROMETRIC 4

/* ---- mb_format.h ------------------------------------------------------------------- */
#define MBF_MBLDEOIH 71

/* ---- mb_process.h ------------------------------------------------------------------ */
#define MBP_FILENAMESIZE MB_PATH_MAXLINE
#define MBP_HEADING_OFF 0
#define MBP_HEADING_CALC 1
#define MBP_HEADING_OFFSET 2
#define MBP_HEADING_CALCOFFSET 3
#define MBP_ROLLBIAS_OFF 0
#define MBP_ROLLBIAS_SINGLE 1
#define MBP_ROLLBIAS_DOUBLE 2
#define MBP_NAV_LINEAR 0
#define MBP_NAVADJ_LLZ 2

/* ---- mbsys_ldeoih.h ---------------------------------------------------------------- */
#define MBSYS_LDEOIH_MAXLINE 200
struct mbsys_ldeoih_struct {
	int kind;
	double time_d;
	double longitude;
	double latitude;
	double sensordepth;
	double altitude;
	float heading;
	float speed;
	float roll;
	float pitch;
	float heave;
	float beam_xwidth;
	float beam_lwidth;
	int beams_bath;
	int beams_amp;
	int pixels_ss;
	int sensorhead;
	int beams_bath_alloc;
	int beams_amp_alloc;
	int pixels_ss_alloc;
	float depth_scale;
	float distance_scale;
	mb_s_char ss_scalepower;
	mb_u_char ss_type;
	mb_u_char imagery_type;
	mb_u_char topo_type;
	unsigned char *beamflag;
	short *bath;
	short *amp;
	short *bath_acrosstrack;
	short *bath_alongtrack;
	short *ss;
	short *ss_acrosstrack;
	short *ss_alongtrack;
	char comment[MBSYS_LDEOIH_MAXLINE];
};

/* ---- mb_aux.h ---------------------------------------------------------------------- */
#define MB_CONTOUR_OLD 0
#define MB_CONTOUR_TRIANGLES 1

struct ping {
	int time_i[7];
	double time_d;
	double navlon;
	double navlat;
	double heading;
	double sensordepth;
	unsigned int pingnumber;
	int beams_bath;
	int beams_bath_alloc;
	char *beamflag;
	double *bath;
	double *bathlon;
	double *bathlat;
	int *bflag[2];
};

struct swath {
	/* raw swath data */
	int npings;
	int npings_max;
	int beams_bath;
	struct ping *pings;

	/* what is plotted */
	int contour_algorithm;
	int plot_contours;
	int plot_triangles;
	int plot_track;
	int plot_name;
	int plot_pingnumber;

	/* contour control parameters */
	double contour_int;
	double color_int;
	double tick_int;
	double label_int;
	double tick_len;
	double label_hgt;
	double label_spacing;
	int ncolor;
	int nlevel;
	int nlevelset;
	double *level_list;
	int *label_list;
	int *tick_list;
	int *color_list;

	/* track control parameters */
	double time_tick_int;
	double time_annot_int;
	double date_annot_int;
	double time_tick_len;
	double name_hgt;

	/* pingnumber control parameters */
	int pingnumber_tick_int;
	int pingnumber_annot_int;
	double pingnumber_tick_len;

	/* triangle network */
	int npts;
	int npts_alloc;
	int *edge;
	int *pingid;
	int *beamid;
	double *x;
	double *y;
	double *z;
	int ntri;
	int ntri_alloc;
	int *iv[3];
	int *ct[3];
	int *cs[3];
	int *ed[3];
	double bath_min;
	double bath_max;
	double triangle_scale;

	/* triangle side flags */
	int *flag[3];

	/* mb_delaun work arrays */
	int ndelaun_alloc;
	double *v1;
	double *v2;
	double *v3;
	int *istack;
	int *kv1;
	int *kv2;

	/* contour arrays */
	int nsave;
	int nsave_alloc;
	double *xsave;
	double *ysave;
	int *isave;
	int *jsave;

	/* contour label arrays */
	int nlabel;
	double *xlabel;
	double *ylabel;
	double *angle;
	int *justify;

	/* function pointers for plot functions */
	void (*contour_plot)(double x, double y, int ipen);
	void (*contour_newpen)(int ipen);
	void (*contour_setline)(int linewidth);
	void (*contour_justify_string)(double height, char *string, double *s);
	void (*contour_plot_string)(double x, double y, double hgt, double angle, char *label);
};

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the run-time function table (the entries the swath editor's table does not have) -- */
struct mbna_mbio_table {
	int (*user_host_date)(int verbose, char user[256], char host[256], char date[32], int *error);
	int (*coor_scale)(int verbose, double latitude, double *mtodeglon, double *mtodeglat);
	int (*get_date)(int verbose, double time_d, int time_i[7]);
	int (*make_info)(int verbose, bool force, char *file, int format, int *error);
	int (*write_init)(int verbose, char *file, int format, void **mbio_ptr, int *beams_bath, int *beams_amp,
	                  int *pixels_ss, int *error);
	int (*put_all)(int verbose, void *mbio_ptr, void *store_ptr, int usevalues, int kind, int time_i[7], double time_d,
	               double navlon, double navlat, double speed, double heading, int nbath, int namp, int nss,
	               char *beamflag, double *bath, double *amp, double *bathacrosstrack, double *bathalongtrack,
	               double *ss, double *ssacrosstrack, double *ssalongtrack, char *comment, int *error);
	int (*get_store)(int verbose, void *mbio_ptr, void **store_ptr, int *error);
	int (*beamwidths)(int verbose, void *mbio_ptr, double *beamwidth_xtrack, double *beamwidth_ltrack, int *error);
	int (*sonartype)(int verbose, void *mbio_ptr, void *store_ptr, int *sonartype, int *error);
	int (*pr_update_navadj)(int verbose, char *file, int mbp_navadj_mode, char *mbp_navadjfile, int mbp_navadj_algorithm,
	                        int *error);
	int (*pr_get_rollbias)(int verbose, char *file, int *mbp_rollbias_mode, double *mbp_rollbias,
	                       double *mbp_rollbias_port, double *mbp_rollbias_stbd, int *error);
	int (*pr_get_heading)(int verbose, char *file, int *mbp_heading_mode, double *mbp_headingbias, int *error);
	int (*pr_update_rollbias)(int verbose, char *file, int mbp_rollbias_mode, double mbp_rollbias,
	                          double mbp_rollbias_port, double mbp_rollbias_stbd, int *error);
	int (*pr_update_heading)(int verbose, char *file, int mbp_heading_mode, double mbp_headingbias, int *error);
	int (*pr_get_output)(int verbose, int *format, char *ifile, char *ofile, int *error);
	int (*pr_get_ofile)(int verbose, char *file, int *mbp_ofile_specified, char *mbp_ofile, int *error);
	int (*mergesort)(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *));
	int (*get_relative_path)(int verbose, char *path, char *pwd, int *error);
	int (*copyfile)(int verbose, const char *src, const char *dst, int *error);
	int (*catfiles)(int verbose, const char *src1, const char *src2, const char *dst, int *error);
	int (*datalist_read)(int verbose, void *datalist_ptr, char *path, char *dpath, int *format, double *weight,
	                     int *error);
	int (*mallocd)(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
	int (*reallocd)(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
	int (*freed)(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error);
};
extern struct mbna_mbio_table mbna_mbio;

/* Pick the entry points above out of the MBIO library the swath editor has ALREADY loaded
 * (mbedit_mbio_open). Returns 1 when every one was found; otherwise 0 with the reason in `msg`. */
int mbna_mbio_open(char *msg, int msglen);

#define mb_user_host_date     mbna_mbio.user_host_date
#define mb_coor_scale         mbna_mbio.coor_scale
#define mb_get_date           mbna_mbio.get_date
#define mb_make_info          mbna_mbio.make_info
#define mb_write_init         mbna_mbio.write_init
#define mb_put_all            mbna_mbio.put_all
#define mb_get_store          mbna_mbio.get_store
#define mb_beamwidths         mbna_mbio.beamwidths
#define mb_sonartype          mbna_mbio.sonartype
#define mb_pr_update_navadj   mbna_mbio.pr_update_navadj
#define mb_pr_get_rollbias    mbna_mbio.pr_get_rollbias
#define mb_pr_get_heading     mbna_mbio.pr_get_heading
#define mb_pr_update_rollbias mbna_mbio.pr_update_rollbias
#define mb_pr_update_heading  mbna_mbio.pr_update_heading
#define mb_pr_get_output      mbna_mbio.pr_get_output
#define mb_pr_get_ofile       mbna_mbio.pr_get_ofile
#define mb_mergesort          mbna_mbio.mergesort
#define mb_get_relative_path  mbna_mbio.get_relative_path
#define mb_copyfile           mbna_mbio.copyfile
#define mb_catfiles           mbna_mbio.catfiles
#define mb_datalist_read      mbna_mbio.datalist_read
#define mb_mallocd            mbna_mbio.mallocd
#define mb_reallocd           mbna_mbio.reallocd
#define mb_freed              mbna_mbio.freed

/* ---- libmbaux, carried in mbna_aux.c ------------------------------------------------- */
#define mb_triangulate   mbna_mb_triangulate
#define mb_contour_init  mbna_mb_contour_init
#define mb_contour_deall mbna_mb_contour_deall
#define mb_contour       mbna_mb_contour
#define mb_delaun        mbna_mb_delaun
#define mblsqr_lsqr      mbna_mblsqr_lsqr
#define mb_aprod         mbna_mb_aprod

int mb_triangulate(int verbose, struct swath *data, int *error);
int mb_contour_init(int verbose, struct swath **data, int npings_max, int beams_bath, int contour_algorithm,
                    int plot_contours, int plot_triangles, int plot_track, int plot_name, int plot_pingnumber,
                    double contour_int, double color_int, double tick_int, double label_int, double tick_len,
                    double label_hgt, double label_spacing, int ncolor, int nlevel, double *level_list, int *label_list,
                    int *tick_list, double time_tick_int, double time_annot_int, double date_annot_int,
                    double time_tick_len, double name_hgt, int pingnumber_tick_int, int pingnumber_annot_int,
                    double pingnumber_tick_len, void (*contour_plot)(double, double, int),
                    void (*contour_newpen)(int), void (*contour_setline)(int),
                    void (*contour_justify_string)(double, char *, double *),
                    void (*contour_plot_string)(double, double, double, double, char *), int *error);
int mb_contour_deall(int verbose, struct swath *data, int *error);
int mb_contour(int verbose, struct swath *data, int *error);
int mb_delaun(int verbose, int npts, double *p1, double *p2, int *ed, int *ntri, int *iv1, int *iv2, int *iv3, int *ct1,
              int *ct2, int *ct3, int *cs1, int *cs2, int *cs3, double *v1, double *v2, double *v3, int *istack, int *kv1,
              int *kv2, int *error);
void mb_aprod(int mode, int m, int n, double x[], double y[], void *UsrWrk);
void mblsqr_lsqr(int m, int n, void (*aprod)(int mode, int m, int n, double x[], double y[], void *UsrWrk), double damp,
                 void *UsrWrk, double u[], double v[], double w[], double x[], double se[], double atol, double btol,
                 double conlim, int itnlim, FILE *nout, int *istop_out, int *itn_out, double *anorm_out,
                 double *acond_out, double *rnorm_out, double *arnorm_out, double *xnorm_out);

/* ---- the MB-System programs upstream runs through system() (mbdatalist, mbprocess, mbgrid, chmod):
 * a host hook, so the window decides how each runs (mbnavadjust_window.cpp). Returns the exit status. */
int mbna_system(const char *command);

/* ---- the grid reader: a host hook (mbnavadjust_window.cpp) ----------------------------- */
#define mb_check_gmt_grd mbna_check_gmt_grd
#define mb_read_gmt_grd  mbna_read_gmt_grd
int mb_check_gmt_grd(int verbose, char *grdfile, int *grid_projection_mode, char *grid_projection_id,
                     float *nodatavalue, int *nxy, int *n_columns, int *n_rows, double *min, double *max, double *xmin,
                     double *xmax, double *ymin, double *ymax, double *dx, double *dy, int *error);
int mb_read_gmt_grd(int verbose, char *grdfile, int *grid_projection_mode, char *grid_projection_id,
                    float *nodatavalue, int *nxy, int *n_columns, int *n_rows, double *min, double *max, double *xmin,
                    double *xmax, double *ymin, double *ymax, double *dx, double *dy, float **data, float **data_dzdx,
                    float **data_dzdy, int *error);

#ifdef __cplusplus
}
#endif

#endif /* MBNAVADJUST_MBIO_H_ */
