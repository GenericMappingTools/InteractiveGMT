/*======================================================================================
 * mbgrdviz_mbio.h -- the slice of MB-System's MBIO API that the mbgrdviz port (mbgrdviz.c,
 * mbgrdviz_mbview.c) uses on top of the swath editor's (deps/src/mbedit/mbedit_mbio.h).
 *
 * The viewer is built WITHOUT MB-System headers or import libraries: the MBIO library is
 * loaded at RUN time by the swath editor (mbedit_mbio_open), and mbgrdviz_mbio_open() picks
 * the extra entry points this tool needs out of that same library. No structure is shared
 * with the library, so a 5.7.x library serves as well as a 5.8.x one; the two entry points
 * 5.7.x may lack (mb_user_host_date, mb_datalist_read3) have stand-ins in mbgrdviz.c.
 *
 * The MBIO functions are reached through the table `mbgv_mbio`; the #defines at the end map
 * each MBIO name onto its table entry, so the ported code calls them with the original text.
 *====================================================================================*/

#ifndef MBGRDVIZ_MBIO_H_
#define MBGRDVIZ_MBIO_H_

#include "../mbedit/mbedit_mbio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DTR 0.01745329251994329500

/* ---- mb_status.h ------------------------------------------------------------------- */
#define MB_PROCESSED_NONE 0
#define MB_PROCESSED_USE 2
#define MB_ALTNAV_NONE 0
#define MB_ALTNAV_USE 1
#define MB_ERROR_BAD_PARAMETER 12
#define MB_ERROR_DATA_NOT_INSERTED -17

/* ---- mb_format.h ------------------------------------------------------------------- */
#define MBF_SEGYSEGY 160
#define MBF_ASCIIXYZ 162
#define MBF_ASCIIYXZ 163
#define MBF_MBPRONAV 166
#define MBF_ASCIIXYT 168
#define MBF_ASCIIYXT 169

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the run-time function table (the entries the swath editor's table does not have) -- */
struct mbgv_mbio_table {
	int (*proj_init)(int verbose, char *projection, void **pjptr, int *error);
	int (*proj_free)(int verbose, void **pjptr, int *error);
	int (*proj_forward)(int verbose, void *pjptr, double lon, double lat, double *easting, double *northing, int *error);
	int (*proj_inverse)(int verbose, void *pjptr, double easting, double northing, double *lon, double *lat, int *error);
	int (*coor_scale)(int verbose, double latitude, double *mtodeglon, double *mtodeglat);
	int (*get_date)(int verbose, double time_d, int time_i[7]);
	char *(*day_name)(int verbose, int day);
	char *(*month_name)(int verbose, int month);
	int (*get_fbt)(int verbose, char *file, int *format, int *error);
	int (*get_fnv)(int verbose, char *file, int *format, int *error);
	int (*mallocd)(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
	int (*reallocd)(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
	int (*freed)(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error);
	int (*memory_clear)(int verbose, int *error);
	int (*segynumber)(int verbose, void *mbio_ptr, unsigned int *line, unsigned int *shot, unsigned int *cdp, int *error);
	int (*singlebeam_swathbounds)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, double *portlon, double *portlat,
	                              double *stbdlon, double *stbdlat, int *error);
	/* optional: NULL on a 5.7.x library, which has neither (mbgrdviz.c then uses its stand-ins) */
	int (*user_host_date)(int verbose, char user[256], char host[256], char date[32], int *error);
	int (*datalist_read3)(int verbose, void *datalist_ptr, int *pstatus, char *path, char *ppath, int *astatus, char *apath,
	                      char *dpath, int *format, double *weight, int *error);
};
extern struct mbgv_mbio_table mbgv_mbio;

/* Fill mbgv_mbio from the library the swath editor loaded (mbedit_mbio_open must have succeeded).
   1 = every required entry point found; 0 = not, with the reason in msg. */
int mbgrdviz_mbio_open(char *msg, int msglen);

/* the stand-ins for the optional entries (mbgrdviz.c) */
int mbgv_user_host_date(int verbose, char user[256], char host[256], char date[32], int *error);
int mbgv_datalist_read3(int verbose, void *datalist_ptr, int *pstatus, char *path, char *ppath, int *astatus, char *apath,
                        char *dpath, int *format, double *weight, int *error);
/* MBIO's projection calls, through a guard against its uninitialized PJ_COORD (see mbgrdviz.c) */
int mbgv_proj_forward(int verbose, void *pjptr, double lon, double lat, double *easting, double *northing, int *error);
int mbgv_proj_inverse(int verbose, void *pjptr, double easting, double northing, double *lon, double *lat, int *error);

#ifdef __cplusplus
}
#endif

#define mb_proj_init          mbgv_mbio.proj_init
#define mb_proj_free          mbgv_mbio.proj_free
#define mb_proj_forward       mbgv_proj_forward
#define mb_proj_inverse       mbgv_proj_inverse
#define mb_coor_scale         mbgv_mbio.coor_scale
#define mb_get_date           mbgv_mbio.get_date
#define mb_day_name           mbgv_mbio.day_name
#define mb_month_name         mbgv_mbio.month_name
#define mb_get_fbt            mbgv_mbio.get_fbt
#define mb_get_fnv            mbgv_mbio.get_fnv
#define mb_mallocd            mbgv_mbio.mallocd
#define mb_reallocd           mbgv_mbio.reallocd
#define mb_freed              mbgv_mbio.freed
#define mb_memory_clear       mbgv_mbio.memory_clear
#define mb_segynumber         mbgv_mbio.segynumber
#define mbsys_singlebeam_swathbounds mbgv_mbio.singlebeam_swathbounds
#define mb_user_host_date     mbgv_user_host_date
#define mb_datalist_read3     mbgv_datalist_read3

#endif /* MBGRDVIZ_MBIO_H_ */
