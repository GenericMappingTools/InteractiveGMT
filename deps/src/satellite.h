/*====================================================================================
 * satellite.h -- SGP4/SDP4 satellite orbit propagation, as a C API of gmtvtk.dll.
 *
 * The maths is Bill Gray's `sat_code` (https://github.com/Bill-Gray/sat_code), MIT,
 * vendored VERBATIM under deps/src/sat_code/ -- see that directory's PROVENANCE.md.
 * Only the ten translation units of its `libsatell` core are taken; they include
 * nothing but libc, so the `lunar` library the upstream README names as a dependency
 * is NOT needed (it is used only by upstream's own command-line tools). NOTHING in
 * deps/src/sat_code/ is edited: a local fix would be invisible at the next update.
 * Everything this project adds lives HERE.
 *
 * This is a SECOND TRANSLATION UNIT inside gmtvtk.dll, exactly like deps/src/mbgrid.c
 * -- it is NOT one of the NN_*.cpp fragments that gmtvtk.cpp #includes, so it is
 * listed in GMTVTK_SRC in deps/CMakeLists.txt and compiled on its own. It pulls in no
 * Qt and no VTK: it is pure numerics, and `test_satellite` links it against nothing
 * but libm to check it against upstream's own published reference values.
 *
 * WHAT THIS ADDS OVER RAW sat_code
 * --------------------------------
 *  - a handle that carries the parsed TLE *and* its initialised model parameters, so
 *    the per-epoch cost is one SxPx call and the SGPx/SDPx choice is made once;
 *  - BATCHED entry points (an array of epochs in, arrays out) -- per the standing rule
 *    that host<->DLL calls are batched, never one ccall per point;
 *  - epochs as Julian Days rather than upstream's "minutes since the TLE epoch";
 *  - TEME -> ECEF -> geodetic, which sat_code does NOT provide (its observe.cpp does
 *    topocentric RA/dec instead). That is the part a ground track actually needs.
 *
 * UNITS, stated once because upstream's differ:
 *    position  km, TEME (true equator, mean equinox of date) -- what SxPx returns
 *    velocity  km/SECOND. sat_code returns km/MINUTE; sat_propagate divides by 60,
 *              the same conversion upstream's own test2.cpp applies before printing.
 *    epochs    Julian Day, UTC. tle_t::epoch is already a JD UTC.
 *    lon/lat   degrees, geodetic WGS84, lon in [-180, 180]
 *    altitude  km above the WGS84 ellipsoid
 *
 * ABI: these symbols are resolved by src/libgmtvtk.jl like every other export of this
 * library. Changing a signature here is a host-facing ABI change and must bump
 * gmtvtk_abi_version with the rest; do not grow a satellite-private version counter.
 *==================================================================================*/

#ifndef SATELLITE_H
#define SATELLITE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define SAT_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define SAT_API __attribute__((visibility("default")))
#else
#  define SAT_API
#endif

/* ---- return codes ----------------------------------------------------------------
 * These start at -101 ON PURPOSE. Per-epoch failures are reported with sat_code's OWN
 * SXPX_ERR_* / SXPX_WARN_* codes, which occupy -1 to -6 (deps/src/sat_code/norad.h),
 * and sat_strerror() translates both families -- so the two ranges MUST NOT overlap.
 * They did in the first draft (SAT_ERR_ARG was -1, colliding with
 * SXPX_ERR_NEARLY_PARABOLIC) and the compiler caught it as a duplicate switch case.
 * Keep anything added here at -101 and below.                                       */
#define SAT_OK                 0
#define SAT_ERR_ARG         -101   /* NULL pointer / non-positive count                */
#define SAT_ERR_TLE_PARSE   -102   /* parse_elements() rejected the two lines          */
#define SAT_ERR_MEMORY      -103
/* Per-epoch codes are reported through sat_propagate's `rc` array rather than folded
   into the call's return value, because a single bad epoch in a long run is not a
   failed run -- the caller decides. */

/* ---- model ids: sat_code's TLE_EPHEMERIS_TYPE_*, repeated so a host need not
        parse norad.h ------------------------------------------------------------- */
#define SAT_MODEL_SGP          1
#define SAT_MODEL_SGP4         2
#define SAT_MODEL_SDP4         3
#define SAT_MODEL_SGP8         4
#define SAT_MODEL_SDP8         5

/*------------------------------------------------------------------------------------
 * Handle lifetime
 *----------------------------------------------------------------------------------*/

/* Parse a TLE pair and initialise its propagator. `line1`/`line2` are the two 69-char
   TLE lines (a trailing newline is fine; the name line, if any, is NOT passed here).
   The model is chosen the way upstream's test2.cpp chooses it: the ephemeris type in
   column 63 of line 1, then promoted SGP4->SDP4 / SGP8->SDP8 (or demoted) to match
   select_ephemeris()'s near-earth/deep-space verdict, so a deep-space object always
   gets an SDPx model whatever its TLE claims.
   Returns NULL on failure; *err (when non-NULL) gets a SAT_ERR_* code. */
SAT_API void *sat_create(const char *line1, const char *line2, int *err);

/* As sat_create(), but forcing `model` (a SAT_MODEL_*) instead of choosing one.
   `model <= 0` means "choose", making this a strict superset -- sat_create() is this
   with 0, never a second implementation. Two callers want the override: a host that
   lets the user pick SGP8/SDP8, and test_satellite, which has to reproduce upstream's
   published vectors for all five models including the ones auto-selection would never
   pick. A model that disagrees with select_ephemeris()'s verdict is honoured as asked
   for -- that is the point of an override -- so a deep-space object forced to SGP4
   will be wrong, exactly as it would be in any other SGP4 implementation. */
SAT_API void *sat_create_ex(const char *line1, const char *line2, int model, int *err);

/* Release a handle from sat_create(). NULL is a no-op. */
SAT_API void sat_destroy(void *h);

/*------------------------------------------------------------------------------------
 * Handle metadata
 *----------------------------------------------------------------------------------*/
SAT_API double sat_epoch_jd(const void *h);      /* TLE epoch, JD UTC; 0 if h is NULL */
SAT_API int    sat_norad_number(const void *h);  /* catalogue number; -1 if h is NULL */
SAT_API int    sat_model(const void *h);         /* the SAT_MODEL_* actually selected */
SAT_API double sat_mean_motion(const void *h);   /* revolutions per day              */
SAT_API double sat_inclination(const void *h);   /* degrees                          */
SAT_API double sat_eccentricity(const void *h);
/* International designator ("98067A"), NUL-terminated, copied into `out` (>= 9 bytes).
   Returns SAT_OK, or SAT_ERR_ARG. */
SAT_API int sat_intl_desig(const void *h, char *out);

/* Static, never-freed description strings. Always non-NULL, even for unknown input. */
SAT_API const char *sat_model_name(int model);
SAT_API const char *sat_strerror(int code);

/*------------------------------------------------------------------------------------
 * Propagation -- all batched
 *----------------------------------------------------------------------------------*/

/* Propagate to `n` epochs. `jd` is JD UTC.
   `pos` (3*n, km, TEME) and `vel` (3*n, km/s, TEME) are each optional -- pass NULL for
   one you do not want. `rc` (n) is optional and receives each epoch's sat_code status.
   Returns SAT_OK, or SAT_ERR_ARG. A per-epoch propagation failure is NOT a call
   failure: it is reported in `rc`, and that epoch's outputs are left as NaN so a bad
   point can never masquerade as a position at the origin. */
SAT_API int sat_propagate(void *h, const double *jd, int n,
                          double *pos, double *vel, int *rc);

/* The same, but with epochs as MINUTES SINCE THE TLE EPOCH -- SGP4's own native time
   argument. Identical in every other respect; sat_propagate() is literally this
   function with jd converted, not a second implementation.
 *
 * WHY IT EXISTS, measured rather than assumed. A Julian Day near 2.44e6 has an ulp of
 * about 4.5e-10 d = 40 microseconds, so `jd = epoch + tsince/1440` does not round-trip:
 * asking for 60.0 minutes gives back 59.999999776. For almost every orbit that is
 * worth ~0.1 m and nobody cares. For a RESONANT DEEP-SPACE orbit it is not, because
 * SDP4's 12-hour/24-hour resonance term is integrated by stepping a persistent
 * (atime, xli, xni) state toward the requested time -- and that integration is
 * genuinely discontinuous at the sub-microsecond level. Verified on upstream's own
 * 11801 test case (2.29 rev/day, e=0.73): a 2.2e-7 minute difference in tsince moves
 * the answer by 12 metres, reproducibly, with everything else held fixed.
 *
 * So: use this entry point when you have exact minutes-from-epoch and want to
 * reproduce a published SGP4 vector bit-for-bit (test_satellite does). Use the JD one
 * for anything on a calendar -- 12 m is far below the error of the TLE itself. */
SAT_API int sat_propagate_tsince(void *h, const double *tsince, int n,
                                 double *pos, double *vel, int *rc);

/* Ground track: the sub-satellite point at each epoch.
   `lon` (deg, [-180,180]), `lat` (deg, geodetic WGS84) and `alt` (km above the WGS84
   ellipsoid) are each optional. `rc` (n) as above; a failed epoch yields NaN.
   Returns SAT_OK, or SAT_ERR_ARG. */
SAT_API int sat_groundtrack(void *h, const double *jd, int n,
                            double *lon, double *lat, double *alt, int *rc);

/* Earth-fixed (ECEF/ITRF) position, km, 3*n. Same optional `rc`. The rotation is by
   GMST only -- see sat_gmst() on what that neglects. Returns SAT_OK / SAT_ERR_ARG. */
SAT_API int sat_propagate_ecef(void *h, const double *jd, int n,
                               double *ecef, int *rc);

/*------------------------------------------------------------------------------------
 * The frame maths, exposed on its own because it is independently useful and, being
 * separately callable, independently TESTABLE.
 *----------------------------------------------------------------------------------*/

/* Greenwich Mean Sidereal Time, radians in [0, 2pi), from the IAU-1982 series
   (Vallado, "Fundamentals of Astrodynamics and Applications", eq. 3-47).
   The argument is nominally UT1; a UTC Julian Day is what callers have, and the
   |UT1-UTC| < 0.9 s difference is ~0.4 km of along-track error at the equator. Polar
   motion and the equation of the equinoxes are likewise neglected (metres). That is
   the normal accuracy floor for TLE work, where the elements themselves are good to
   about a kilometre at epoch and degrade by the day. */
SAT_API double sat_gmst(double jd_ut1);

/* Rotate a TEME vector to ECEF about the pole by `gmst` (radians). In-place safe. */
SAT_API void sat_teme_to_ecef(const double *teme, double gmst, double *ecef);

/* ECEF (km) -> geodetic WGS84. `alt_km` is height above the ellipsoid, not the geoid.
   Iterates on latitude, so it stays correct at any altitude -- unlike the closed-form
   approximations that assume a point near the surface. In-place safe. */
SAT_API void sat_ecef_to_geodetic(const double *ecef, double *lon_deg,
                                  double *lat_deg, double *alt_km);

/*------------------------------------------------------------------------------------
 * Calendar helpers. Here rather than in the host because the epoch in a TLE is already
 * a JD and round-tripping it through a host date type is a needless place to lose
 * precision.
 *----------------------------------------------------------------------------------*/

/* Gregorian UTC -> Julian Day. `day` may carry a fraction; hour/min/sec are added on
   top of it, so both (d=1.5, h=0) and (d=1, h=12) mean the same instant. */
SAT_API double sat_cal_to_jd(int year, int month, double day,
                             int hour, int minute, double second);

/* Julian Day -> Gregorian UTC. Any output pointer may be NULL. */
SAT_API void sat_jd_to_cal(double jd, int *year, int *month, int *day,
                           int *hour, int *minute, double *second);

#ifdef __cplusplus
}
#endif
#endif /* SATELLITE_H */
