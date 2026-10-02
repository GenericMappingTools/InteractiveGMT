/*======================================================================================
 * mbeditviz_mbio.h -- the slice of MB-System's MBIO API that the mbeditviz port
 * (mbeditviz.c) uses on top of the swath editor's (deps/src/mbedit/mbedit_mbio.h).
 *
 * The viewer is built WITHOUT MB-System headers or import libraries: the MBIO library is
 * loaded at RUN time by the swath editor (mbedit_mbio_open), and mbeditviz_mbio_open() picks
 * the extra entry points this tool needs out of that same library.
 *
 * Two structures below are SHARED MEMORY with the library (mb_get_info fills
 * struct mb_info_struct, mb_pr_readpar / mb_pr_writepar read and write struct
 * mb_process_struct), so they are copied VERBATIM from MB-System 5.8.3 (mb_define.h,
 * mb_process.h) and mbeditviz_mbio_open() accepts only a 5.8.x library.
 *
 * The MBIO functions are reached through the table `mbev_mbio`; the #defines at the end map
 * each MBIO name onto its table entry, so the ported engine calls them with the original text.
 *====================================================================================*/

#ifndef MBEDITVIZ_MBIO_H_
#define MBEDITVIZ_MBIO_H_

#include "../mbedit/mbedit_mbio.h"

/* ---- mb_define.h ------------------------------------------------------------------- */
#define MB_PATHPLUS_MAXLINE 1152
#define MB_PATHPLUSPLUS_MAXLINE 2304
#define MB_NAME_LENGTH 32
typedef char mb_pathplus[MB_PATHPLUS_MAXLINE];
typedef char mb_pathplusplus[MB_PATHPLUSPLUS_MAXLINE];
typedef char mb_name[MB_NAME_LENGTH];
#define DTR (M_PI / 180.)
#define GOLDEN_MEAN_SMALL 0.38197
#define GOLDEN_MEAN_LARGE 0.61803
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- mb_status.h ------------------------------------------------------------------- */
#define MB_DATA_COMMENT 2
#define MB_ERROR_BAD_FORMAT 3
#define MB_ERROR_BAD_PARAMETER 12
#define MB_PROCESSED_NONE 0
#define MB_TOPOGRAPHY_TYPE_UNKNOWN 0
#define MB_TOPOGRAPHY_TYPE_ECHOSOUNDER 1
#define MB_TOPOGRAPHY_TYPE_MULTIBEAM 2
#define MB_TOPOGRAPHY_TYPE_SIDESCAN 3
#define MB_TOPOGRAPHY_TYPE_INTERFEROMETRIC 4
#define MB_TOPOGRAPHY_TYPE_LIDAR 5
#define MB_TOPOGRAPHY_TYPE_CAMERA 6
#define MB_TOPOGRAPHY_TYPE_GRID 7
#define MB_TOPOGRAPHY_TYPE_POINT 8

/* ---- mb_format.h ------------------------------------------------------------------- */
#define MBF_MBPRONAV 166

/* ---- mb_process.h ------------------------------------------------------------------ */
#define MBP_FILENAMESIZE MB_PATH_MAXLINE
#define MBP_CUT_NUM_MAX 20

/* ---- mb_define.h 5.8.3, verbatim ---------------------------------------------------- */
struct mb_info_struct {
	int loaded;
	char file[MB_PATH_MAXLINE];

	int nrecords;
	int nrecords_ss1;
	int nrecords_ss2;
	int nrecords_sbp;
	int nbeams_bath;
	int nbeams_bath_total;
	int nbeams_bath_good;
	int nbeams_bath_zero;
	int nbeams_bath_flagged;
	int nbeams_amp;
	int nbeams_amp_total;
	int nbeams_amp_good;
	int nbeams_amp_zero;
	int nbeams_amp_flagged;
	int npixels_ss;
	int npixels_ss_total;
	int npixels_ss_good;
	int npixels_ss_zero;
	int npixels_ss_flagged;

	double time_total;
	double dist_total;
	double speed_avg;

	double time_start;
	double lon_start;
	double lat_start;
	double depth_start;
	double heading_start;
	double speed_start;
	double sensordepth_start;
	double sonaraltitude_start;

	double time_end;
	double lon_end;
	double lat_end;
	double depth_end;
	double heading_end;
	double speed_end;
	double sensordepth_end;
	double sonaraltitude_end;

	double lon_min;
	double lon_max;
	double lat_min;
	double lat_max;
	double sensordepth_min;
	double sensordepth_max;
	double altitude_min;
	double altitude_max;
	double depth_min;
	double depth_max;
	double amp_min;
	double amp_max;
	double ss_min;
	double ss_max;

	int problem_nodata;
	int problem_zeronav;
	int problem_toofast;
	int problem_avgtoofast;
	int problem_toodeep;
	int problem_baddatagram;

	// int	mask_nx;
	// int	mask_ny;
	// double	mask_dx;
	// double	mask_dy;
	// int	mask_alloc;
	// int	*mask;
};

/* ---- mb_process.h 5.8.3, verbatim ---------------------------------------------------- */
struct mb_process_struct {
  /* general parameters */
  int mbp_ifile_specified;
  char mbp_ifile[MBP_FILENAMESIZE];
  int mbp_ofile_specified;
  char mbp_ofile[MBP_FILENAMESIZE];
  int mbp_format_specified;
  int mbp_format;

  /* navigation merging */
  int mbp_nav_mode;
  char mbp_navfile[MBP_FILENAMESIZE];
  int mbp_nav_format;
  int mbp_nav_heading;
  int mbp_nav_speed;
  int mbp_nav_draft;
  int mbp_nav_attitude;
  int mbp_nav_algorithm;
  double mbp_nav_timeshift;
  int mbp_nav_shift;
  double mbp_nav_offsetx;
  double mbp_nav_offsety;
  double mbp_nav_offsetz;
  double mbp_nav_shiftlon;
  double mbp_nav_shiftlat;
  double mbp_nav_shiftx;
  double mbp_nav_shifty;

  /* adjusted navigation merging */
  int mbp_navadj_mode;
  char mbp_navadjfile[MBP_FILENAMESIZE];
  int mbp_navadj_algorithm;

  /* attitude merging */
  int mbp_attitude_mode;
  char mbp_attitudefile[MBP_FILENAMESIZE];
  int mbp_attitude_format;

  /* sensordepth merging */
  int mbp_sensordepth_mode;
  char mbp_sensordepthfile[MBP_FILENAMESIZE];
  int mbp_sensordepth_format;

  /* data cutting */
  int mbp_cut_num;
  int mbp_cut_kind[MBP_CUT_NUM_MAX];
  int mbp_cut_mode[MBP_CUT_NUM_MAX];
  double mbp_cut_min[MBP_CUT_NUM_MAX];
  double mbp_cut_max[MBP_CUT_NUM_MAX];

  /* bathymetry editing */
  int mbp_edit_mode;
  char mbp_editfile[MBP_FILENAMESIZE];

  /* bathymetry recalculation */
  int mbp_bathrecalc_mode;
  int mbp_svp_mode;
  char mbp_svpfile[MBP_FILENAMESIZE];
  int mbp_ssv_mode;
  double mbp_ssv;
  int mbp_tt_mode;
  double mbp_tt_mult;
  int mbp_angle_mode;
  int mbp_corrected;
  int mbp_static_mode;
  char mbp_staticfile[MBP_FILENAMESIZE];

  /* draft correction */
  int mbp_draft_mode;
  double mbp_draft;
  double mbp_draft_offset;
  double mbp_draft_mult;

  /* heave correction */
  int mbp_heave_mode;
  double mbp_heave;
  double mbp_heave_mult;

  /* lever correction */
  int mbp_lever_mode;
  double mbp_vru_offsetx;
  double mbp_vru_offsety;
  double mbp_vru_offsetz;
  double mbp_sonar_offsetx;
  double mbp_sonar_offsety;
  double mbp_sonar_offsetz;

  /* roll correction */
  int mbp_rollbias_mode;
  double mbp_rollbias;
  double mbp_rollbias_port;
  double mbp_rollbias_stbd;

  /* pitch correction */
  int mbp_pitchbias_mode;
  double mbp_pitchbias;

  /* heading correction */
  int mbp_heading_mode;
  double mbp_headingbias;

  /* tide correction */
  int mbp_tide_mode;
  char mbp_tidefile[MBP_FILENAMESIZE];
  int mbp_tide_format;

  /* amplitude correction */
  int mbp_ampcorr_mode;
  char mbp_ampcorrfile[MBP_FILENAMESIZE];
  int mbp_ampcorr_type;
  int mbp_ampcorr_symmetry;
  double mbp_ampcorr_angle;
  int mbp_ampcorr_slope;

  /* sidescan correction */
  int mbp_sscorr_mode;
  char mbp_sscorrfile[MBP_FILENAMESIZE];
  int mbp_sscorr_type;
  int mbp_sscorr_symmetry;
  double mbp_sscorr_angle;
  int mbp_sscorr_slope;

  /* amplitude and sidescan correction */
  char mbp_ampsscorr_topofile[MBP_FILENAMESIZE];

  /* sidescan recalculation */
  int mbp_ssrecalc_mode;
  double mbp_ssrecalc_pixelsize;
  double mbp_ssrecalc_swathwidth;
  int mbp_ssrecalc_interpolate;

  /* strip comments */
  int mbp_strip_comments;

  /* metadata strings */
  char mbp_meta_vessel[MBP_FILENAMESIZE];
  char mbp_meta_institution[MBP_FILENAMESIZE];
  char mbp_meta_platform[MBP_FILENAMESIZE];
  char mbp_meta_sonar[MBP_FILENAMESIZE];
  char mbp_meta_sonarversion[MBP_FILENAMESIZE];
  char mbp_meta_cruiseid[MBP_FILENAMESIZE];
  char mbp_meta_cruisename[MBP_FILENAMESIZE];
  char mbp_meta_pi[MBP_FILENAMESIZE];
  char mbp_meta_piinstitution[MBP_FILENAMESIZE];
  char mbp_meta_client[MBP_FILENAMESIZE];
  int mbp_meta_svcorrected;
  int mbp_meta_tidecorrected;
  int mbp_meta_batheditmanual;
  int mbp_meta_batheditauto;
  double mbp_meta_rollbias;
  double mbp_meta_pitchbias;
  double mbp_meta_headingbias;
  double mbp_meta_draft;

  /* processing kluges */
  int mbp_kluge001;
  int mbp_kluge002;
  int mbp_kluge003;
  int mbp_kluge004;
  int mbp_kluge005;
  int mbp_kluge006;
  int mbp_kluge007;
  int mbp_kluge008;
  int mbp_kluge009;
  int mbp_kluge010;
};

/* ---- the run-time function table (the entries the swath editor's table does not have) -- */
struct mbev_mbio_table {
	int (*proj_forward)(int verbose, void *pjptr, double lon, double lat, double *easting, double *northing, int *error);
	int (*proj_init)(int verbose, char *projection, void **pjptr, int *error);
	int (*proj_free)(int verbose, void **pjptr, int *error);
	int (*coor_scale)(int verbose, double latitude, double *mtodeglon, double *mtodeglat);
	int (*get_binary_float)(bool swapped, void *buffer, const void *ptr);
	int (*get_binary_double)(bool swapped, void *buffer, const void *ptr);
	int (*linear_interp)(int verbose, const double *xa, const double *ya, int n, double x, double *y, int *i, int *error);
	int (*linear_interp_heading)(int verbose, const double *xa, const double *ya, int n, double x, double *y, int *i,
	                             int *error);
	int (*swathbounds)(int verbose, int checkgood, int nbath, int nss, char *beamflag, double *bathacrosstrack, double *ss,
	                   double *ssacrosstrack, int *ibeamport, int *ibeamcntr, int *ibeamstbd, int *ipixelport,
	                   int *ipixelcntr, int *ipixelstbd, int *error);
	int (*sonartype)(int verbose, void *mbio_ptr, void *store_ptr, int *sonartype, int *error);
	int (*beamwidths)(int verbose, void *mbio_ptr, double *beamwidth_xtrack, double *beamwidth_ltrack, int *error);
	int (*get_date)(int verbose, double time_d, int time_i[7]);
	int (*get_fbt)(int verbose, char *file, int *format, int *error);
	int (*get_shortest_path)(int verbose, char *path, int *error);
	int (*esf_open)(int verbose, const char *program_name, char *esffile, bool load, int output, struct mb_esf_struct *esf,
	                int *error);
	int (*mallocd)(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
	int (*freed)(int verbose, const char *sourcefile, int sourceline, void **ptr, int *error);
	int (*reallocd)(int verbose, const char *sourcefile, int sourceline, size_t size, void **ptr, int *error);
	int (*lonflip)(int verbose, int *lonflip);
	int (*get_info)(int verbose, char *file, struct mb_info_struct *mb_info, int lonflip, int *error);
	int (*pr_readpar)(int verbose, char *file, int lookforfiles, struct mb_process_struct *process, int *error);
	int (*pr_writepar)(int verbose, char *file, struct mb_process_struct *process, int *error);
	int (*platform_math_attitude_rotate_beam)(int verbose, double beam_acrosstrack, double beam_alongtrack, double beam_bath,
	                                          double attitude_roll, double attitude_pitch, double attitude_heading,
	                                          double *newbeam_easting, double *newbeam_northing, double *newbeam_bath,
	                                          int *error);
	int (*platform_math_attitude_offset_corrected_by_nav)(int verbose, double prev_attitude_roll, double prev_attitude_pitch,
	                                                      double prev_attitude_heading, double target_offset_to_source_roll,
	                                                      double target_offset_to_source_pitch,
	                                                      double target_offset_to_source_heading, double new_attitude_roll,
	                                                      double new_attitude_pitch, double new_attitude_heading,
	                                                      double *corrected_offset_roll, double *corrected_offset_pitch,
	                                                      double *corrected_offset_heading, int *error);
	int (*singlebeam_swathbounds)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, double *portlon, double *portlat,
	                              double *stbdlon, double *stbdlat, int *error);
	int (*ttimes)(int verbose, void *mbio_ptr, void *store_ptr, int *kind, int *nbeams, double *ttimes, double *angles,
	              double *angles_forward, double *angles_null, double *heave, double *alongtrack_offset, double *draft,
	              double *ssv, int *error);
};
extern struct mbev_mbio_table mbev_mbio;

#define mb_proj_forward       mbev_mbio.proj_forward
#define mb_proj_init          mbev_mbio.proj_init
#define mb_proj_free          mbev_mbio.proj_free
#define mb_coor_scale         mbev_mbio.coor_scale
#define mb_get_binary_float   mbev_mbio.get_binary_float
#define mb_get_binary_double  mbev_mbio.get_binary_double
#define mb_linear_interp      mbev_mbio.linear_interp
#define mb_linear_interp_heading mbev_mbio.linear_interp_heading
#define mb_swathbounds        mbev_mbio.swathbounds
#define mb_sonartype          mbev_mbio.sonartype
#define mb_beamwidths         mbev_mbio.beamwidths
#define mb_get_date           mbev_mbio.get_date
#define mb_get_fbt            mbev_mbio.get_fbt
#define mb_get_shortest_path  mbev_mbio.get_shortest_path
#define mb_esf_open           mbev_mbio.esf_open
#define mb_mallocd            mbev_mbio.mallocd
#define mb_freed              mbev_mbio.freed
#define mb_reallocd           mbev_mbio.reallocd
#define mb_lonflip            mbev_mbio.lonflip
#define mb_get_info           mbev_mbio.get_info
#define mb_pr_readpar         mbev_mbio.pr_readpar
#define mb_pr_writepar        mbev_mbio.pr_writepar
#define mb_platform_math_attitude_rotate_beam mbev_mbio.platform_math_attitude_rotate_beam
#define mb_platform_math_attitude_offset_corrected_by_nav mbev_mbio.platform_math_attitude_offset_corrected_by_nav
#define mbsys_singlebeam_swathbounds mbev_mbio.singlebeam_swathbounds
#define mb_ttimes             mbev_mbio.ttimes

#endif /* MBEDITVIZ_MBIO_H_ */
