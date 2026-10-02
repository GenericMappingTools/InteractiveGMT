/*======================================================================================
 * mbedit_mbio.h -- the slice of MB-System's MBIO API that the mbedit port (mbedit.c) uses.
 *
 * The viewer is built WITHOUT MB-System headers or import libraries. The MBIO library itself
 * (mbio.dll / libmbio.so / libmbio.dylib) is loaded at RUN time by mbedit_mbio_open() in
 * mbedit.c, so gmtvtk loads on a machine that has no MB-System at all; only the swath editor
 * needs it.
 *
 * Everything below is copied from MB-System 5.8.3 (mb_define.h, mb_status.h, mb_process.h),
 * values and layouts unchanged. struct mb_esf_struct is SHARED MEMORY with the library (mbio
 * fills it in mb_esf_load), which is why mbedit_mbio_open() accepts only the MBIO versions it was checked against: 5.8.x,
 * and 5.7.x (5.7.7beta02, the one GMT's MB-System supplement ships; see mbedit_mbio_open).
 *
 * The MBIO functions are reached through the table `mbedit_mbio` (filled by dlsym /
 * GetProcAddress). The #defines at the end map each MBIO name onto its table entry, so the
 * ported engine calls mb_get_all(...) etc. with the original text.
 *====================================================================================*/

#ifndef MBEDIT_MBIO_H_
#define MBEDIT_MBIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ---- mb_define.h ------------------------------------------------------------------- */
#define MB_PATH_MAXLINE 1024
#define MB_COMMENT_MAXLINE 1944
typedef char mb_path[MB_PATH_MAXLINE];

#define MB_DATALIST_LOOK_UNSET 0
#define MB_DATALIST_LOOK_NO 1
#define MB_DATALIST_LOOK_YES 2

#define RTD 57.2957795130823230000

typedef enum {
	MB_COLOR_WHITE = 0,
	MB_COLOR_BLACK,
	MB_COLOR_RED,
	MB_COLOR_ORANGE,
	MB_COLOR_YELLOW,
	MB_COLOR_GREEN,
	MB_COLOR_BLUEGREEN,
	MB_COLOR_BLUE,
	MB_COLOR_PURPLE,
	MB_COLOR_CORAL,
	MB_COLOR_LIGHTGREY,
	MB_NDrawingColors
} mb_color_t;

#ifndef MIN
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif
#ifndef MAX
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#endif

#define MB_MEM_TYPE_BATHYMETRY 1
#define MB_MEM_TYPE_AMPLITUDE 2
#define MB_MEM_TYPE_SIDESCAN 3

/* ---- mb_status.h ------------------------------------------------------------------- */
#define MB_SUCCESS 1
#define MB_FAILURE 0

#define MB_ERROR_NO_ERROR 0
#define MB_ERROR_MEMORY_FAIL 1
#define MB_ERROR_OPEN_FAIL 2
#define MB_ERROR_EOF 4
#define MB_ERROR_BAD_USAGE 9
#define MB_ERROR_TIME_GAP -1
#define MB_ERROR_OUT_BOUNDS -2
#define MB_ERROR_OUT_TIME -3
#define MB_ERROR_SPEED_TOO_SMALL -4
#define MB_ERROR_OTHER -8
#define MB_ERROR_FILE_LOCKED -23

#define MB_DATA_DATA 1

#define MB_DETECT_UNKNOWN 0
#define MB_DETECT_AMPLITUDE 1
#define MB_DETECT_PHASE 2

#define MB_PULSE_UNKNOWN 0
#define MB_PULSE_CW 1
#define MB_PULSE_UPCHIRP 2
#define MB_PULSE_DOWNCHIRP 3

#define MB_FLAG_NONE        0x00
#define MB_FLAG_FLAG        0x01
#define MB_FLAG_NULL        0x01
#define MB_FLAG_MANUAL      0x04
#define MB_FLAG_FILTER      0x08
#define MB_FLAG_FILTER2     0x10
#define MB_FLAG_MULTIPICK   0x20
#define MB_FLAG_INTERPOLATE 0x40
#define MB_FLAG_SONAR       0x80

#define mb_beam_ok(F) ((int)(!(F & MB_FLAG_FLAG)))
#define mb_beam_check_flag(F) ((int)(F & MB_FLAG_FLAG))
#define mb_beam_check_flag_null(F) ((int)(F == MB_FLAG_NULL))
#define mb_beam_check_flag_flagged(F) ((int)((F & MB_FLAG_FLAG) && (F & 0xFC)))
#define mb_beam_check_flag_manual(F) ((int)((F & MB_FLAG_MANUAL) && (F & MB_FLAG_FLAG)))
#define mb_beam_check_flag_filter(F) ((int)((F & MB_FLAG_FILTER) && (F & MB_FLAG_FLAG)))
#define mb_beam_check_flag_filter2(F) ((int)((F & MB_FLAG_FILTER2) && (F & MB_FLAG_FLAG)))
#define mb_beam_check_flag_multipick(F) ((int)((F & MB_FLAG_MULTIPICK) && (F & MB_FLAG_FLAG)))
#define mb_beam_check_flag_interpolate(F) ((int)((F & MB_FLAG_INTERPOLATE) && (F & MB_FLAG_FLAG)))
#define mb_beam_check_flag_sonar(F) ((int)((F & MB_FLAG_SONAR) && (F & MB_FLAG_FLAG)))
#define mb_beam_check_flag_usable(F) ((int)((F != MB_FLAG_NULL) && !((F & MB_FLAG_FLAG) && ((F & MB_FLAG_INTERPOLATE)))))
#define mb_beam_check_flag_unusable(F) ((int)((F == MB_FLAG_NULL) || ((F & MB_FLAG_FLAG) && ((F & MB_FLAG_INTERPOLATE)))))
#define mb_beam_check_flag_usable2(F) ((int)((F != MB_FLAG_NULL) && !((F & MB_FLAG_FLAG) && ((F & MB_FLAG_INTERPOLATE) || (F & MB_FLAG_MULTIPICK)))))
#define mb_beam_check_flag_unusable2(F) ((int)((F == MB_FLAG_NULL) || ((F & MB_FLAG_FLAG) && ((F & MB_FLAG_INTERPOLATE) || (F & MB_FLAG_MULTIPICK)))))
#define mb_beam_set_flag_null(F) (0x01)
#define mb_beam_set_flag_none(F) (0x00)
#define mb_beam_set_flag_manual(F) (F | 0x05)
#define mb_beam_set_flag_filter(F) (F | 0x09)
#define mb_beam_set_flag_filter2(F) (F | 0x11)
#define mb_beam_set_flag_multipick(F) (F | 0x21)
#define mb_beam_set_flag_interpolate(F) (F | 0x41)
#define mb_beam_set_flag_sonar(F) (F | 0x81)

/* ---- mb_process.h ------------------------------------------------------------------ */
#define MBP_EDIT_OFF 0
#define MBP_EDIT_ON 1
#define MBP_EDIT_FLAG 1
#define MBP_EDIT_UNFLAG 2
#define MBP_EDIT_ZERO 3
#define MBP_EDIT_FILTER 4
#define MBP_EDIT_SONAR 5
#define MBP_ESF_NOWRITE 0
#define MBP_ESF_WRITE 1
#define MBP_ESF_APPEND 2

#define MBP_LOCK_NONE 0
#define MBP_LOCK_PROCESS 1
#define MBP_LOCK_EDITBATHY 2
#define MBP_LOCK_EDITNAV 3

#define MB_ESF_MODE_EXPLICIT 0
#define MB_ESF_MODE_IMPLICIT_NULL 1
#define MB_ESF_MODE_IMPLICIT_GOOD 2
#define MB_ESF_MAXTIMEDIFF 0.0000011
#define MB_ESF_MAXTIMEDIFF_X10 0.0011
#define MB_ESF_MULTIPLICITY_FACTOR 100000000

struct mb_edit_struct {
	double time_d;
	int beam;
	int action;
	int use;
};
struct mb_esf_struct {
	char esffile[MB_PATH_MAXLINE];
	char esstream[MB_PATH_MAXLINE];
	int byteswapped;
	int version;
	int mode;
	int nedit;
	struct mb_edit_struct *edit;
	FILE *esffp;
	FILE *essfp;
	int startnextsearch;
};

/* ---- the run-time function table ---------------------------------------------------- */
struct mbedit_mbio_table {
	int (*version)(int verbose, char *version_string, int *version_id, int *version_major, int *version_minor,
	               int *version_archive, int *error);
	int (*defaults)(int verbose, int *format, int *pings, int *lonflip, double bounds[4], int *btime_i, int *etime_i,
	                double *speedmin, double *timegap);
	int (*uselockfiles)(int verbose, bool *uselockfiles);
	int (*read_init)(int verbose, char *file, int format, int pings, int lonflip, double bounds[4], int btime_i[7],
	                 int etime_i[7], double speedmin, double timegap, void **mbio_ptr, double *btime_d, double *etime_d,
	                 int *beams_bath, int *beams_amp, int *pixels_ss, int *error);
	int (*close)(int verbose, void **mbio_ptr, int *error);
	int (*get_all)(int verbose, void *mbio_ptr, void **store_ptr, int *kind, int time_i[7], double *time_d, double *navlon,
	               double *navlat, double *speed, double *heading, double *distance, double *altitude, double *sensordepth,
	               int *nbath, int *namp, int *nss, char *beamflag, double *bath, double *amp, double *bathacrosstrack,
	               double *bathalongtrack, double *ss, double *ssacrosstrack, double *ssalongtrack, char *comment, int *error);
	int (*get_format)(int verbose, char *filename, char *fileroot, int *format, int *error);
	int (*register_array)(int verbose, void *mbio_ptr, int type, size_t size, void **handle, int *error);
	int (*memory_list)(int verbose, int *error);
	int (*error)(int verbose, int error, char **message);
	int (*get_time)(int verbose, int time_i[7], double *time_d);
	int (*extract_nav)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, int time_i[7], double *time_d,
	                   double *navlon, double *navlat, double *speed, double *heading, double *draft, double *roll,
	                   double *pitch, double *heave, int *error);
	int (*sensorhead)(int verbose, void *mbio_ptr, void *store_ptr, int *sensorhead, int *error);
	int (*detects)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, int *nbeams, int *detects, int *error);
	int (*pulses)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, int *nbeams, int *pulses, int *error);
	int (*double_compare)(const void *a, const void *b);
	int (*pr_lockswathfile)(int verbose, const char *file, int purpose, const char *program_name, int *error);
	int (*pr_unlockswathfile)(int verbose, const char *file, int purpose, const char *program_name, int *error);
	int (*pr_lockinfo)(int verbose, const char *file, bool *locked, int *purpose, char *program, char *user, char *cpu,
	                   char *date, int *error);
	int (*pr_update_format)(int verbose, char *file, int mbp_format_specified, int mbp_format, int *error);
	int (*pr_update_edit)(int verbose, char *file, int mbp_edit_mode, char *mbp_editfile, int *error);
	int (*esf_load)(int verbose, const char *program_name, char *swathfile, bool load, int output, char *esffile,
	                struct mb_esf_struct *esf, int *error);
	int (*esf_apply)(int verbose, struct mb_esf_struct *esf, double time_d, int pingmultiplicity, int nbath,
	                 char *beamflag, int *error);
	int (*esf_save)(int verbose, struct mb_esf_struct *esf, double time_d, int beam, int action, int *error);
	int (*ess_save)(int verbose, struct mb_esf_struct *esf, double time_d, int beam, int action, int *error);
	int (*esf_close)(int verbose, struct mb_esf_struct *esf, int *error);
	int (*datalist_open)(int verbose, void **datalist_ptr, char *path, int look_processed, int *error);
	int (*datalist_read2)(int verbose, void *datalist_ptr, int *pstatus, char *path, char *ppath, char *dpath,
	                      int *format, double *weight, int *error);
	int (*datalist_close)(int verbose, void **datalist_ptr, int *error);
};
extern struct mbedit_mbio_table mbedit_mbio;

#define mb_version            mbedit_mbio.version
#define mb_defaults           mbedit_mbio.defaults
/* 5.7.x writes an int here, 5.8.x a bool: the adapter (mbedit.c) passes what the loaded library wants */
int mbedit_uselockfiles(int verbose, bool *uselockfiles);
#define mb_uselockfiles       mbedit_uselockfiles
#define mb_read_init          mbedit_mbio.read_init
#define mb_close              mbedit_mbio.close
#define mb_get_all            mbedit_mbio.get_all
#define mb_get_format         mbedit_mbio.get_format
#define mb_register_array     mbedit_mbio.register_array
#define mb_memory_list        mbedit_mbio.memory_list
#define mb_error              mbedit_mbio.error
#define mb_get_time           mbedit_mbio.get_time
#define mb_extract_nav        mbedit_mbio.extract_nav
#define mb_sensorhead         mbedit_mbio.sensorhead
#define mb_detects            mbedit_mbio.detects
#define mb_pulses             mbedit_mbio.pulses
#define mb_double_compare     mbedit_mbio.double_compare
#define mb_pr_lockswathfile   mbedit_mbio.pr_lockswathfile
#define mb_pr_unlockswathfile mbedit_mbio.pr_unlockswathfile
#define mb_pr_lockinfo        mbedit_mbio.pr_lockinfo
#define mb_pr_update_format   mbedit_mbio.pr_update_format
#define mb_pr_update_edit     mbedit_mbio.pr_update_edit
#define mb_esf_load           mbedit_mbio.esf_load
#define mb_esf_apply          mbedit_mbio.esf_apply
#define mb_esf_save           mbedit_mbio.esf_save
#define mb_ess_save           mbedit_mbio.ess_save
#define mb_esf_close          mbedit_mbio.esf_close
#define mb_datalist_open      mbedit_mbio.datalist_open
#define mb_datalist_read2     mbedit_mbio.datalist_read2
#define mb_datalist_close     mbedit_mbio.datalist_close

#endif /* MBEDIT_MBIO_H_ */
