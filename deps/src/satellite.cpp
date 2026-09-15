/*====================================================================================
 * satellite.cpp -- implementation of the SGP4/SDP4 C API declared in satellite.h.
 *
 * See satellite.h for the contract, the units, and why the vendored sat_code tree is
 * never edited. This file is the ONLY place this project's own satellite code lives.
 *
 * A second translation unit of gmtvtk.dll (GMTVTK_SRC, deps/CMakeLists.txt) -- no Qt,
 * no VTK, nothing but libc and the vendored sat_code core.
 *==================================================================================*/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "satellite.h"
#include "sat_code/norad.h"

/* sat_code hands back km/MINUTE; the host contract is km/second. */
#define SAT_VEL_KM_MIN_TO_KM_S  (1. / 60.)

/* Defined here rather than taken from sat_code's norad_in.h: that header is the
   propagator's PRIVATE one (it carries the WGS72 gravity constants and the model's
   working macros), and including it here would tie this file to upstream internals
   that are free to change between updates. 1440 is not going to. */
#define SAT_MIN_PER_DAY  1440.

/* WGS84. NOTE the deliberate mismatch with sat_code's own earth_radius_in_km
   (6378.135, WGS72): the propagator's internal constant must stay WGS72 or its maths
   stops being SGP4, while a ground track is wanted on the ellipsoid maps actually use.
   The two radii differ by 2 m -- far below this pipeline's error floor (see sat_gmst).
   Changing sat_code's constant to "match" would silently alter every orbit. */
#define WGS84_A_KM      6378.137
#define WGS84_F         (1. / 298.257223563)
#define WGS84_E2        (WGS84_F * (2. - WGS84_F))

#ifndef M_PI
#  define M_PI 3.141592653589793238462643383279502884197
#endif
#define SAT_TWOPI   (2. * M_PI)
#define SAT_DEG2RAD (M_PI / 180.)
#define SAT_RAD2DEG (180. / M_PI)

/* One handle: the parsed elements plus the model parameters initialised from them.
   Keeping the two together is the point -- SxPx_init() is the expensive half, and a
   caller propagating a thousand epochs must pay it once, not a thousand times. */
typedef struct
{
	tle_t  tle;
	double params[N_SAT_PARAMS];
	int    model;              /* SAT_MODEL_*, after the deep-space promotion */
} sat_obj;

/* A NaN to write into an epoch that failed, so a bad point is never mistaken for a
   real position at the centre of the earth. math.h's NAN is the right spelling and is
   what every supported compiler provides; the fallback divides two VOLATILE doubles
   because MSVC constant-folds a plain 0./0. at compile time and rejects it (C2124). */
static double sat_nan(void)
{
#ifdef NAN
	return (double)NAN;
#else
	volatile double zero = 0.;
	return zero / zero;
#endif
}

/*------------------------------------------------------------------------------------
 * Model dispatch. sat_code exposes five model pairs and no dispatcher of its own (its
 * dynamic.cpp SXPX() is Win32 run-time-linking scaffolding, not this). Upstream's
 * test2.cpp open-codes the same two switches; they are here ONCE so the init and the
 * step can never disagree about which model a handle is using.
 *----------------------------------------------------------------------------------*/
static void sat_model_init(int model, double *params, const tle_t *tle)
{
	switch (model) {
		case SAT_MODEL_SGP:  SGP_init( params, tle); break;
		case SAT_MODEL_SGP4: SGP4_init(params, tle); break;
		case SAT_MODEL_SGP8: SGP8_init(params, tle); break;
		case SAT_MODEL_SDP4: SDP4_init(params, tle); break;
		case SAT_MODEL_SDP8: SDP8_init(params, tle); break;
		default:             SGP4_init(params, tle); break;
	}
}

static int sat_model_step(int model, double tsince, const tle_t *tle,
                          const double *params, double *pos, double *vel)
{
	switch (model) {
		case SAT_MODEL_SGP:  return SGP( tsince, tle, params, pos, vel);
		case SAT_MODEL_SGP4: return SGP4(tsince, tle, params, pos, vel);
		case SAT_MODEL_SGP8: return SGP8(tsince, tle, params, pos, vel);
		case SAT_MODEL_SDP4: return SDP4(tsince, tle, params, pos, vel);
		case SAT_MODEL_SDP8: return SDP8(tsince, tle, params, pos, vel);
		default:             return SGP4(tsince, tle, params, pos, vel);
	}
}

/* Which model a TLE should really use. The ephemeris type in column 63 of line 1 is a
   REQUEST; select_ephemeris() gives the verdict that matters (upstream: "deep space"
   means a period of 225 minutes or more). A published TLE almost always carries type
   0, so without this promotion every Molniya and every geostationary bird would be
   propagated with the near-earth model and be wrong by hundreds of km. Same rule, and
   the same +1/-1 within sat_code's id ordering, as upstream's test2.cpp. */
static int sat_pick_model(const tle_t *tle)
{
	int model = (int)tle->ephemeris_type;
	const int is_deep = select_ephemeris(tle);

	if (model < SAT_MODEL_SGP || model > SAT_MODEL_SDP8)
		model = SAT_MODEL_SGP4;              /* the usual "type 0" -> the default */

	if (is_deep && (model == SAT_MODEL_SGP4 || model == SAT_MODEL_SGP8))
		model++;                             /* SGP4 -> SDP4,  SGP8 -> SDP8 */
	else if (!is_deep && (model == SAT_MODEL_SDP4 || model == SAT_MODEL_SDP8))
		model--;                             /* SDP4 -> SGP4,  SDP8 -> SGP8 */
	return model;
}

/*------------------------------------------------------------------------------------
 * Handle lifetime
 *----------------------------------------------------------------------------------*/

/* parse_elements() reads fixed columns and will walk off the end of a line that was
   truncated (a hand-edited file, a copy-paste that lost trailing blanks). Both lines
   are copied into a 80-byte blank-padded buffer first, so a short line is padded
   rather than read past. */
static void sat_pad_line(const char *src, char *dst)
{
	size_t i, n;
	memset(dst, ' ', 80);
	dst[80] = '\0';
	n = strlen(src);
	for (i = 0; i < n && i < 80; i++) {
		if (src[i] == '\r' || src[i] == '\n')
			break;
		dst[i] = src[i];
	}
}

SAT_API void *sat_create_ex(const char *line1, const char *line2, int model, int *err)
{
	char buf1[81], buf2[81];
	sat_obj *o;

	if (err) *err = SAT_OK;
	if (!line1 || !line2) {
		if (err) *err = SAT_ERR_ARG;
		return NULL;
	}
	sat_pad_line(line1, buf1);
	sat_pad_line(line2, buf2);

	o = (sat_obj *)calloc(1, sizeof(sat_obj));
	if (!o) {
		if (err) *err = SAT_ERR_MEMORY;
		return NULL;
	}
	if (parse_elements(buf1, buf2, &o->tle) < 0) {
		free(o);
		if (err) *err = SAT_ERR_TLE_PARSE;
		return NULL;
	}
	o->model = (model >= SAT_MODEL_SGP && model <= SAT_MODEL_SDP8)
	         ? model : sat_pick_model(&o->tle);
	sat_model_init(o->model, o->params, &o->tle);
	return o;
}

SAT_API void *sat_create(const char *line1, const char *line2, int *err)
{
	return sat_create_ex(line1, line2, 0, err);   /* 0 = choose; never a second impl */
}

SAT_API void sat_destroy(void *h)
{
	if (h) free(h);
}

/*------------------------------------------------------------------------------------
 * Handle metadata
 *----------------------------------------------------------------------------------*/
SAT_API double sat_epoch_jd(const void *h)
{
	return h ? ((const sat_obj *)h)->tle.epoch : 0.;
}

SAT_API int sat_norad_number(const void *h)
{
	return h ? ((const sat_obj *)h)->tle.norad_number : -1;
}

SAT_API int sat_model(const void *h)
{
	return h ? ((const sat_obj *)h)->model : 0;
}

/* tle_t::xno is radians/minute; revolutions per day is the form TLEs are read in. */
SAT_API double sat_mean_motion(const void *h)
{
	return h ? ((const sat_obj *)h)->tle.xno * SAT_MIN_PER_DAY / SAT_TWOPI : 0.;
}

SAT_API double sat_inclination(const void *h)
{
	return h ? ((const sat_obj *)h)->tle.xincl * SAT_RAD2DEG : 0.;
}

SAT_API double sat_eccentricity(const void *h)
{
	return h ? ((const sat_obj *)h)->tle.eo : 0.;
}

SAT_API int sat_intl_desig(const void *h, char *out)
{
	if (!h || !out) return SAT_ERR_ARG;
	memcpy(out, ((const sat_obj *)h)->tle.intl_desig, 9);
	out[8] = '\0';
	return SAT_OK;
}

SAT_API const char *sat_model_name(int model)
{
	switch (model) {
		case SAT_MODEL_SGP:  return "SGP";
		case SAT_MODEL_SGP4: return "SGP4";
		case SAT_MODEL_SDP4: return "SDP4";
		case SAT_MODEL_SGP8: return "SGP8";
		case SAT_MODEL_SDP8: return "SDP8";
		default:             return "unknown";
	}
}

SAT_API const char *sat_strerror(int code)
{
	switch (code) {
		case SAT_OK:                        return "no error";
		case SAT_ERR_ARG:                   return "invalid argument";
		case SAT_ERR_TLE_PARSE:             return "the two lines are not a valid TLE";
		case SAT_ERR_MEMORY:                return "out of memory";
		/* sat_code's own per-epoch codes, so one host-side lookup covers both. */
		case SXPX_ERR_NEARLY_PARABOLIC:     return "orbit is nearly parabolic";
		case SXPX_ERR_NEGATIVE_MAJOR_AXIS:  return "negative semi-major axis";
		case SXPX_WARN_ORBIT_WITHIN_EARTH:  return "orbit lies within the earth";
		case SXPX_WARN_PERIGEE_WITHIN_EARTH:return "perigee lies within the earth";
		case SXPX_ERR_NEGATIVE_XN:          return "negative mean motion";
		case SXPX_ERR_CONVERGENCE_FAIL:     return "Kepler iteration did not converge";
		default:                            return "unknown error";
	}
}

/*------------------------------------------------------------------------------------
 * Frame maths
 *----------------------------------------------------------------------------------*/

SAT_API double sat_gmst(double jd_ut1)
{
	/* IAU-1982, as a polynomial in Julian centuries from J2000. The 876600*3600 term
	   is kept separate from 8640184.812866 exactly as published: folding them loses
	   bits, because the first is ~10^9 and is a whole number of seconds per century. */
	const double tu  = (jd_ut1 - 2451545.) / 36525.;
	double gmst_sec  = 67310.54841
	                 + (876600. * 3600. + 8640184.812866) * tu
	                 + 0.093104 * tu * tu
	                 - 6.2e-6 * tu * tu * tu;
	double rad;

	gmst_sec = fmod(gmst_sec, 86400.);           /* seconds of sidereal time */
	if (gmst_sec < 0.) gmst_sec += 86400.;
	rad = gmst_sec * (SAT_TWOPI / 86400.);
	/* fmod on a ~10^11 argument can land a hair outside the range after scaling. */
	rad = fmod(rad, SAT_TWOPI);
	if (rad < 0.) rad += SAT_TWOPI;
	return rad;
}

SAT_API void sat_teme_to_ecef(const double *teme, double gmst, double *ecef)
{
	const double c = cos(gmst), s = sin(gmst);
	const double x = teme[0], y = teme[1], z = teme[2];   /* read first: in-place safe */

	ecef[0] =  c * x + s * y;
	ecef[1] = -s * x + c * y;
	ecef[2] =  z;
}

SAT_API void sat_ecef_to_geodetic(const double *ecef, double *lon_deg,
                                  double *lat_deg, double *alt_km)
{
	const double x = ecef[0], y = ecef[1], z = ecef[2];
	const double r = sqrt(x * x + y * y);
	double lat, c = WGS84_A_KM, sinlat;
	int i;

	if (lon_deg) {
		double lon = atan2(y, x) * SAT_RAD2DEG;
		/* atan2 already gives (-180,180]; normalise anyway so a caller can rely on the
		   documented range without knowing that. */
		while (lon >  180.) lon -= 360.;
		while (lon <= -180.) lon += 360.;
		*lon_deg = lon;
	}

	/* Iterate on latitude. Starting from the geocentric latitude this converges to
	   double precision in a handful of passes at any altitude; 20 is a ceiling, not an
	   expectation, and the early exit is what actually ends it. The degenerate case is
	   a point on the polar axis (r == 0), where atan2(z, 0) is already exactly +-pi/2
	   and the loop is a no-op. */
	lat = atan2(z, r);
	for (i = 0; i < 20; i++) {
		const double prev = lat;
		sinlat = sin(lat);
		c      = WGS84_A_KM / sqrt(1. - WGS84_E2 * sinlat * sinlat);
		lat    = atan2(z + c * WGS84_E2 * sinlat, r);
		if (fabs(lat - prev) < 1e-14)
			break;
	}
	sinlat = sin(lat);
	c      = WGS84_A_KM / sqrt(1. - WGS84_E2 * sinlat * sinlat);

	if (lat_deg) *lat_deg = lat * SAT_RAD2DEG;
	if (alt_km) {
		/* r/cos(lat) is unusable near the poles and z/sin(lat) near the equator; pick
		   whichever denominator is the larger, so neither is ever the small one. */
		if (fabs(sinlat) > 0.5)
			*alt_km = z / sinlat - c * (1. - WGS84_E2);
		else
			*alt_km = r / cos(lat) - c;
	}
}

/*------------------------------------------------------------------------------------
 * Propagation
 *----------------------------------------------------------------------------------*/

/* The one place an epoch is turned into a state vector. Every batched entry point
   below goes through it, so they cannot drift apart in unit handling or in what they
   do with a failed step. Writes NaN and returns the sat_code code on failure.
   `vel` may be NULL. */
static int sat_step(sat_obj *o, double tsince, double *pos, double *vel)
{
	double p[3], v[3];
	int rc;

	rc = sat_model_step(o->model, tsince, &o->tle, o->params, p, v);

	/* The SXPX_WARN_* codes come with a mathematically meaningful position, so they
	   are passed through as-is; only a hard error voids the point. */
	if (rc == SXPX_ERR_NEARLY_PARABOLIC || rc == SXPX_ERR_NEGATIVE_MAJOR_AXIS ||
	    rc == SXPX_ERR_NEGATIVE_XN      || rc == SXPX_ERR_CONVERGENCE_FAIL) {
		const double nan_val = sat_nan();
		pos[0] = pos[1] = pos[2] = nan_val;
		if (vel) vel[0] = vel[1] = vel[2] = nan_val;
		return rc;
	}
	pos[0] = p[0];  pos[1] = p[1];  pos[2] = p[2];
	if (vel) {
		vel[0] = v[0] * SAT_VEL_KM_MIN_TO_KM_S;
		vel[1] = v[1] * SAT_VEL_KM_MIN_TO_KM_S;
		vel[2] = v[2] * SAT_VEL_KM_MIN_TO_KM_S;
	}
	return rc;
}

SAT_API int sat_propagate_tsince(void *h, const double *tsince, int n,
                                 double *pos, double *vel, int *rc)
{
	sat_obj *o = (sat_obj *)h;
	double scratch_pos[3];
	int i;

	if (!o || !tsince || n <= 0) return SAT_ERR_ARG;

	for (i = 0; i < n; i++) {
		double *p = pos ? pos + 3 * i : scratch_pos;   /* vel alone is a legal ask */
		const int code = sat_step(o, tsince[i], p, vel ? vel + 3 * i : NULL);
		if (rc) rc[i] = code;
	}
	return SAT_OK;
}

SAT_API int sat_propagate(void *h, const double *jd, int n,
                          double *pos, double *vel, int *rc)
{
	sat_obj *o = (sat_obj *)h;
	double scratch_pos[3];
	int i;

	if (!o || !jd || n <= 0) return SAT_ERR_ARG;

	/* Converted one epoch at a time and pushed through the SAME sat_step() the tsince
	   entry point uses -- not a parallel loop, and no temporary array the caller would
	   pay for. See sat_propagate_tsince()'s note in satellite.h on what this
	   conversion costs and why it is the right default anyway. */
	for (i = 0; i < n; i++) {
		double *p = pos ? pos + 3 * i : scratch_pos;
		const double ts = (jd[i] - o->tle.epoch) * SAT_MIN_PER_DAY;
		const int code = sat_step(o, ts, p, vel ? vel + 3 * i : NULL);
		if (rc) rc[i] = code;
	}
	return SAT_OK;
}

SAT_API int sat_propagate_ecef(void *h, const double *jd, int n, double *ecef, int *rc)
{
	sat_obj *o = (sat_obj *)h;
	int i;

	if (!o || !jd || !ecef || n <= 0) return SAT_ERR_ARG;

	for (i = 0; i < n; i++) {
		double p[3];
		const int code = sat_step(o, (jd[i] - o->tle.epoch) * SAT_MIN_PER_DAY, p, NULL);
		if (rc) rc[i] = code;
		sat_teme_to_ecef(p, sat_gmst(jd[i]), ecef + 3 * i);
	}
	return SAT_OK;
}

SAT_API int sat_groundtrack(void *h, const double *jd, int n,
                            double *lon, double *lat, double *alt, int *rc)
{
	sat_obj *o = (sat_obj *)h;
	int i;

	if (!o || !jd || n <= 0) return SAT_ERR_ARG;

	for (i = 0; i < n; i++) {
		double p[3], e[3];
		const int code = sat_step(o, (jd[i] - o->tle.epoch) * SAT_MIN_PER_DAY, p, NULL);
		if (rc) rc[i] = code;
		/* A NaN position propagates through the rotation and the geodetic solve on its
		   own -- atan2(NaN,NaN) is NaN -- so a failed epoch needs no special case here
		   and cannot turn into a plausible-looking point on the equator. */
		sat_teme_to_ecef(p, sat_gmst(jd[i]), e);
		sat_ecef_to_geodetic(e, lon ? lon + i : NULL,
		                        lat ? lat + i : NULL,
		                        alt ? alt + i : NULL);
	}
	return SAT_OK;
}

/*------------------------------------------------------------------------------------
 * Calendar. Meeus, "Astronomical Algorithms", ch. 7 -- the Gregorian branch only:
 * every TLE postdates 1582 by a wide margin.
 *----------------------------------------------------------------------------------*/

SAT_API double sat_cal_to_jd(int year, int month, double day,
                             int hour, int minute, double second)
{
	long a, b;
	double jd;

	if (month <= 2) { year -= 1;  month += 12; }
	a = year / 100;
	b = 2 - a + a / 4;

	jd = floor(365.25 * (year + 4716)) + floor(30.6001 * (month + 1))
	   + day + (double)b - 1524.5;
	return jd + (hour + minute / 60. + second / 3600.) / 24.;
}

SAT_API void sat_jd_to_cal(double jd, int *year, int *month, int *day,
                           int *hour, int *minute, double *second)
{
	double z_frac, f, aa, bb, dd, ee, day_frac, secs;
	long z, alpha, a_corr;
	int h, mi, yr, mo, dy;

	z_frac = jd + .5;
	z      = (long)floor(z_frac);
	f      = z_frac - (double)z;

	alpha  = (long)floor(((double)z - 1867216.25) / 36524.25);
	a_corr = z + 1 + alpha - alpha / 4;
	bb     = (double)a_corr + 1524.;
	aa     = floor((bb - 122.1) / 365.25);
	dd     = floor(365.25 * aa);
	ee     = floor((bb - dd) / 30.6001);

	day_frac = bb - dd - floor(30.6001 * ee) + f;
	dy       = (int)floor(day_frac);
	mo       = (int)((ee < 14.) ? ee - 1. : ee - 13.);
	yr       = (int)((mo > 2) ? aa - 4716. : aa - 4715.);

	/* Round to the nearest millisecond before splitting. Without it a JD that IS an
	   exact midnight comes back as 23:59:59.9999996 of the day before, because the
	   fraction cannot be represented -- and then the DATE is wrong too, not just the
	   clock, which is the part that actually bites a caller. */
	secs = (day_frac - (double)dy) * 86400.;
	secs = floor(secs * 1000. + .5) / 1000.;
	if (secs >= 86400.) { secs -= 86400.;  dy += 1; }

	h  = (int)(secs / 3600.);
	mi = (int)((secs - h * 3600.) / 60.);

	if (year)   *year   = yr;
	if (month)  *month  = mo;
	if (day)    *day    = dy;
	if (hour)   *hour   = h;
	if (minute) *minute = mi;
	if (second) *second = secs - h * 3600. - mi * 60.;
}
