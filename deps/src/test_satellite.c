/*====================================================================================
 * test_satellite.c -- numerics regression for deps/src/satellite.cpp.
 *
 * Same shape and same purpose as test_mbgrid.c: pure C, libm only, no Qt, no VTK,
 * built by the default `all` target, runs in well under a second. Run it as
 * `deps/build/test_satellite`; it prints a line per check and exits non-zero on the
 * first real disagreement.
 *
 * THE POINT: part 1 checks this port against Bill Gray's OWN published state vectors
 * (src/sat_code/test2_reference.txt, his test2.txt verbatim), not against itself. It
 * walks every block in that file -- all five models, near-earth and deep-space, ~80
 * TLEs and several hundred epochs -- so a mis-vendored source file, a wrong model
 * dispatch or a botched unit conversion fails HERE, loudly, instead of showing up much
 * later as a ground track that is quietly in the wrong place.
 *
 * Parts 2-4 cover what upstream has no reference for because it does not provide it:
 * the frame maths this project added (GMST, TEME->ECEF, ECEF->geodetic) and the
 * calendar round-trip.
 *==================================================================================*/

#define _USE_MATH_DEFINES   /* MSVC hides M_PI behind this; must precede <math.h> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#  define M_PI 3.141592653589793238462643383279502884197
#endif

#include "satellite.h"

#ifndef SAT_TEST_DATA_DIR
#  define SAT_TEST_DATA_DIR "src/sat_code"
#endif

#define SAT_MIN_PER_DAY 1440.

static int failures = 0;
static int checks   = 0;

static void fail(const char *what, double got, double want, double tol)
{
	failures++;
	printf("  FAIL %-28s got %.8f  want %.8f  (diff %.3e, tol %.3e)\n",
	       what, got, want, fabs(got - want), tol);
}

static void check_close(const char *what, double got, double want, double tol)
{
	checks++;
	if (!(fabs(got - want) <= tol))
		fail(what, got, want, tol);
}

/*------------------------------------------------------------------------------------
 * Part 1 -- against upstream's published vectors
 *
 * test2_reference.txt looks like this, repeating:
 *
 *   1 11801U          80230.29629788  .01431103  00000-0  14311-1       2
 *   2 11801U 46.7916 230.4354 7318036  47.4722  10.4117  2.28537848     2
 *   Deep-Space type Ephemeris (SDP4) selected:
 *   Ephem:SDP4   Tsince         X/Xdot           Y/Ydot           Z/Zdot
 *   11801        0.0000      7473.37102491     428.94748312    5828.74846783
 *                               5.10715539       6.44468030      -0.18613330
 *
 * The model is read from the "Ephem:" header rather than chosen, because upstream
 * generated several of these blocks with an explicit override (its test.tle carries
 * "Ephem 4" directives) and auto-selection would never pick SGP8/SDP8 for a published
 * type-0 TLE. That is exactly what sat_create_ex()'s `model` argument is for.
 *----------------------------------------------------------------------------------*/

/* Upstream prints 8 decimals, so these compare to the last printed digit: this port
   must reproduce the published numbers EXACTLY, not approximately.
   That is only possible through sat_propagate_tsince(). The JD entry point cannot do
   it, and the reason is worth stating because it looked at first like a port bug: a
   Julian Day near 2.44e6 has an ulp of ~40 microseconds, so epoch+tsince/1440 does not
   round-trip, and for a RESONANT deep-space orbit SDP4's stepped resonance integrator
   turns that into ~12 m (measured, on 11801 below). test_jd_vs_tsince() pins that
   difference deliberately rather than hiding it behind a loose tolerance here. */
#define POS_TOL_KM   1.0e-8
#define VEL_TOL_KMS  1.0e-8

static int model_from_name(const char *s)
{
	if (!strncmp(s, "SGP4", 4)) return SAT_MODEL_SGP4;
	if (!strncmp(s, "SDP4", 4)) return SAT_MODEL_SDP4;
	if (!strncmp(s, "SGP8", 4)) return SAT_MODEL_SGP8;
	if (!strncmp(s, "SDP8", 4)) return SAT_MODEL_SDP8;
	if (!strncmp(s, "SGP",  3)) return SAT_MODEL_SGP;   /* "SGP " -- test last */
	return 0;
}

static void test_against_reference(void)
{
	char path[1024], line[512], l1[128], l2[128];
	FILE *fp;
	void *sat = NULL;
	int  have_l1 = 0, model = 0, epochs = 0, blocks = 0;

	snprintf(path, sizeof(path), "%s/test2_reference.txt", SAT_TEST_DATA_DIR);
	fp = fopen(path, "r");
	if (!fp) {
		printf("  FAIL cannot open %s\n", path);
		failures++;
		return;
	}
	l1[0] = l2[0] = '\0';

	while (fgets(line, sizeof(line), fp)) {
		const char *eph = strstr(line, "Ephem:");
		int    norad;
		double tsince, x, y, z;

		/* A TLE line pair. Only lines long enough to BE a TLE qualify: the file also
		   carries prose, and "1 " is a common enough prefix to want the length check. */
		if (line[0] == '1' && line[1] == ' ' && strlen(line) >= 69) {
			strncpy(l1, line, sizeof(l1) - 1);  l1[sizeof(l1) - 1] = '\0';
			have_l1 = 1;
			continue;
		}
		if (line[0] == '2' && line[1] == ' ' && strlen(line) >= 69 && have_l1) {
			strncpy(l2, line, sizeof(l2) - 1);  l2[sizeof(l2) - 1] = '\0';
			have_l1 = 0;
			continue;
		}
		/* The block header names the model, and is the point at which the handle for
		   the TLE pair just read can be built. */
		if (eph) {
			model = model_from_name(eph + 6);
			if (sat) { sat_destroy(sat);  sat = NULL; }
			if (model && l1[0] && l2[0]) {
				int err = SAT_OK;
				sat = sat_create_ex(l1, l2, model, &err);
				if (!sat) {
					printf("  FAIL sat_create_ex: %s\n", sat_strerror(err));
					failures++;
				}
				else
					blocks++;
			}
			continue;
		}
		/* A state-vector row: "<norad> <tsince> <x> <y> <z>", with the velocity on the
		   line that follows it. */
		if (sat && sscanf(line, "%d %lf %lf %lf %lf",
		                  &norad, &tsince, &x, &y, &z) == 5) {
			double pos[3], vel[3], vx, vy, vz;
			char   what[128];
			int    rc = SAT_OK;

			if (!fgets(line, sizeof(line), fp)) break;
			if (sscanf(line, "%lf %lf %lf", &vx, &vy, &vz) != 3)
				continue;

			if (sat_propagate_tsince(sat, &tsince, 1, pos, vel, &rc) != SAT_OK) {
				printf("  FAIL sat_propagate returned an error\n");
				failures++;
				continue;
			}
			snprintf(what, sizeof(what), "%d %s t=%.0f x", norad,
			         sat_model_name(model), tsince);
			check_close(what, pos[0], x, POS_TOL_KM);
			what[strlen(what) - 1] = 'y';  check_close(what, pos[1], y, POS_TOL_KM);
			what[strlen(what) - 1] = 'z';  check_close(what, pos[2], z, POS_TOL_KM);
			/* Velocity is the unit conversion under test: sat_code returns km/MINUTE,
			   the file prints km/SECOND, and satellite.cpp is what divides by 60. */
			what[strlen(what) - 1] = 'u';  check_close(what, vel[0], vx, VEL_TOL_KMS);
			what[strlen(what) - 1] = 'v';  check_close(what, vel[1], vy, VEL_TOL_KMS);
			what[strlen(what) - 1] = 'w';  check_close(what, vel[2], vz, VEL_TOL_KMS);
			epochs++;
		}
	}
	if (sat) sat_destroy(sat);
	fclose(fp);

	printf("  %d blocks, %d epochs, %d value checks\n", blocks, epochs, checks);
	/* A parser that silently matched nothing would "pass" every check above. These
	   floors are what make the absence of failures mean something. */
	if (blocks < 50 || epochs < 200) {
		printf("  FAIL reference file parsed too little (expected >=50 blocks, "
		       ">=200 epochs) -- the fixture or this parser is wrong\n");
		failures++;
	}
}

/* The two time entry points, compared head to head. This exists so the JD path's
   precision loss is a KNOWN, BOUNDED, tested quantity instead of a surprise found
   later in a ground track -- and so that if a future change makes the JD path worse,
   something fails immediately.

   A near-earth orbit (the ISS) must agree to well under a metre: there the only effect
   is the ~40 us of time resolution times orbital speed.
   A resonant deep-space orbit (11801, 2.29 rev/day, e=0.73) is allowed to differ by up
   to 50 m, because SDP4's resonance integration is genuinely discontinuous at that
   time resolution. It must still agree to KILOMETRE level -- a real frame or unit
   error would show up here as hundreds of km, not tens of metres. */
static void test_jd_vs_tsince(void)
{
	const char *n1 = "1 25544U 98067A   24015.50000000  .00016717  00000-0  30777-3 0  9005";
	const char *n2 = "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49514637 10110";
	const char *d1 = "1 11801U          80230.29629788  .01431103  00000-0  14311-1       2";
	const char *d2 = "2 11801U 46.7916 230.4354 7318036  47.4722  10.4117  2.28537848     2";
	struct { const char *l1, *l2, *tag; double tol_km; } cases[2];
	int c;

	cases[0].l1 = n1;  cases[0].l2 = n2;  cases[0].tag = "LEO  jd-vs-tsince";
	cases[0].tol_km = 1.0e-3;
	cases[1].l1 = d1;  cases[1].l2 = d2;  cases[1].tag = "deep jd-vs-tsince";
	cases[1].tol_km = 5.0e-2;

	for (c = 0; c < 2; c++) {
		double ts[5], jd[5], pa[15], pb[15];
		void  *a, *b;
		int    err = SAT_OK, i;

		for (i = 0; i < 5; i++) ts[i] = i * 30.;

		a = sat_create(cases[c].l1, cases[c].l2, &err);
		b = sat_create(cases[c].l1, cases[c].l2, &err);
		checks++;
		if (!a || !b) {
			printf("  FAIL %s: sat_create: %s\n", cases[c].tag, sat_strerror(err));
			failures++;
			if (a) sat_destroy(a);
			if (b) sat_destroy(b);
			continue;
		}
		for (i = 0; i < 5; i++) jd[i] = sat_epoch_jd(a) + ts[i] / SAT_MIN_PER_DAY;

		sat_propagate_tsince(a, ts, 5, pa, NULL, NULL);
		sat_propagate(b, jd, 5, pb, NULL, NULL);

		for (i = 0; i < 5; i++) {
			const double dx = pa[3*i]   - pb[3*i];
			const double dy = pa[3*i+1] - pb[3*i+1];
			const double dz = pa[3*i+2] - pb[3*i+2];
			check_close(cases[c].tag, sqrt(dx*dx + dy*dy + dz*dz), 0., cases[c].tol_km);
		}
		sat_destroy(a);
		sat_destroy(b);
	}
}

/*------------------------------------------------------------------------------------
 * Part 2 -- GMST
 *----------------------------------------------------------------------------------*/
static void test_gmst(void)
{
	double g, prev, jd;
	int    i;

	/* J2000.0 exactly: GMST is 18h 41m 50.548s = 280.46061837 deg (Vallado, ex. 3-5).
	   Tolerance is 1e-6 deg -- this is a polynomial, it either matches or it is wrong. */
	g = sat_gmst(2451545.0) * 180. / M_PI;
	check_close("gmst at J2000 (deg)", g, 280.46061837, 1e-6);

	/* One sidereal day later it must come back to the same angle. A sidereal day is
	   86164.0905 s; this is the check that the rate term is right, not just the
	   constant. Loosened to 1e-4 deg, which is the precision of that figure. */
	g = sat_gmst(2451545.0 + 86164.0905 / 86400.) * 180. / M_PI;
	check_close("gmst after 1 sidereal day", g, 280.46061837, 1e-4);

	/* Range, over a century, at a step that is not a neat fraction of a day. */
	checks++;
	for (i = 0; i < 40000; i++) {
		jd = 2440000. + i * 0.917;
		g  = sat_gmst(jd);
		if (!(g >= 0. && g < 2. * M_PI)) {
			printf("  FAIL gmst out of [0,2pi) at jd %.3f: %.17g\n", jd, g);
			failures++;
			break;
		}
	}

	/* Monotone increasing within a day (mod the wrap), i.e. the earth turns one way. */
	checks++;
	prev = sat_gmst(2451545.0);
	for (i = 1; i <= 20; i++) {
		g = sat_gmst(2451545.0 + i * 0.01);
		if (g < prev && (prev - g) < 6.0) {   /* a real wrap jumps by nearly 2pi */
			printf("  FAIL gmst not increasing at step %d\n", i);
			failures++;
			break;
		}
		prev = g;
	}
}

/*------------------------------------------------------------------------------------
 * Part 3 -- ECEF <-> geodetic
 *----------------------------------------------------------------------------------*/
static void test_geodetic(void)
{
	double e[3], lon, lat, alt;
	const double a = 6378.137, f = 1. / 298.257223563;
	const double b = a * (1. - f);
	int i;

	/* On the equator, on the prime meridian, on the ellipsoid. */
	e[0] = a;  e[1] = 0.;  e[2] = 0.;
	sat_ecef_to_geodetic(e, &lon, &lat, &alt);
	check_close("equator lon", lon, 0.,   1e-9);
	check_close("equator lat", lat, 0.,   1e-9);
	check_close("equator alt", alt, 0.,   1e-9);

	/* The north pole -- the degenerate case for the r/cos(lat) branch. */
	e[0] = 0.;  e[1] = 0.;  e[2] = b;
	sat_ecef_to_geodetic(e, &lon, &lat, &alt);
	check_close("pole lat", lat, 90., 1e-9);
	check_close("pole alt", alt,  0., 1e-9);

	/* 90 deg east, and the sign convention for the southern hemisphere. */
	e[0] = 0.;  e[1] = a;  e[2] = 0.;
	sat_ecef_to_geodetic(e, &lon, &lat, &alt);
	check_close("east lon", lon, 90., 1e-9);

	e[0] = 0.;  e[1] = 0.;  e[2] = -b;
	sat_ecef_to_geodetic(e, &lon, &lat, &alt);
	check_close("south pole lat", lat, -90., 1e-9);

	/* A point at altitude on the equator: geodetic height is measured along the
	   normal, which at the equator is radial, so this one is exact by construction. */
	e[0] = 0.;  e[1] = -(a + 800.);  e[2] = 0.;
	sat_ecef_to_geodetic(e, &lon, &lat, &alt);
	check_close("800 km alt", alt, 800., 1e-9);
	check_close("west lon",   lon, -90., 1e-9);

	/* Round-trip: build ECEF from a known geodetic triple, convert back, compare.
	   This is the real test of the iteration -- it sweeps latitude and altitude,
	   including the 0.5 crossover between the two altitude branches. */
	for (i = -89; i <= 89; i += 7) {
		const double latr = i * M_PI / 180.;
		double h;
		for (h = 0.; h <= 36000.; h += 6000.) {
			const double e2 = f * (2. - f);
			const double sn = sin(latr);
			const double cc = a / sqrt(1. - e2 * sn * sn);
			double got_lat, got_alt, got_lon;
			double v[3];

			v[0] = (cc + h) * cos(latr) * cos(0.7);
			v[1] = (cc + h) * cos(latr) * sin(0.7);
			v[2] = (cc * (1. - e2) + h) * sn;
			sat_ecef_to_geodetic(v, &got_lon, &got_lat, &got_alt);
			check_close("roundtrip lat", got_lat, (double)i, 1e-9);
			check_close("roundtrip alt", got_alt, h,         1e-7);
			check_close("roundtrip lon", got_lon, 0.7 * 180. / M_PI, 1e-9);
		}
	}
}

/*------------------------------------------------------------------------------------
 * Part 4 -- TEME->ECEF, the calendar, and one end-to-end sanity check
 *----------------------------------------------------------------------------------*/
static void test_frames_and_calendar(void)
{
	double teme[3], ecef[3], r0, r1, sec;
	int y, mo, d, h, mi, i;

	/* The rotation is about the pole, so it must preserve length and leave z alone. */
	teme[0] = 4321.;  teme[1] = -1234.;  teme[2] = 5678.;
	sat_teme_to_ecef(teme, 1.2345, ecef);
	r0 = sqrt(teme[0]*teme[0] + teme[1]*teme[1] + teme[2]*teme[2]);
	r1 = sqrt(ecef[0]*ecef[0] + ecef[1]*ecef[1] + ecef[2]*ecef[2]);
	check_close("teme->ecef preserves |r|", r1, r0, 1e-9);
	check_close("teme->ecef preserves z",   ecef[2], teme[2], 1e-12);

	/* In-place must give the same answer as out-of-place -- the header promises it. */
	teme[0] = 4321.;  teme[1] = -1234.;  teme[2] = 5678.;
	sat_teme_to_ecef(teme, 1.2345, teme);
	check_close("teme->ecef in place x", teme[0], ecef[0], 1e-12);
	check_close("teme->ecef in place y", teme[1], ecef[1], 1e-12);

	/* Calendar: known Julian Days. */
	check_close("jd J2000",        sat_cal_to_jd(2000,  1,  1, 12, 0, 0.), 2451545.0, 1e-9);
	check_close("jd 1970 epoch",   sat_cal_to_jd(1970,  1,  1,  0, 0, 0.), 2440587.5, 1e-9);
	check_close("jd 1999-12-31",   sat_cal_to_jd(1999, 12, 31,  0, 0, 0.), 2451543.5, 1e-9);
	/* A fractional day and an explicit clock must agree. */
	check_close("jd day fraction",  sat_cal_to_jd(2024, 3, 1.5, 0, 0, 0.),
	                                sat_cal_to_jd(2024, 3, 1., 12, 0, 0.), 1e-12);

	/* Round-trip across a leap year, a century boundary and a midnight. */
	for (i = 0; i < 4000; i++) {
		const double jd = 2440587.5 + i * 13.37;
		double back;
		sat_jd_to_cal(jd, &y, &mo, &d, &h, &mi, &sec);
		back = sat_cal_to_jd(y, mo, (double)d, h, mi, sec);
		check_close("calendar roundtrip", back, jd, 1e-8);
		checks -= 1;               /* counted once below, not 4000 times */
	}
	checks++;

	/* Exact midnight is the case the millisecond rounding in sat_jd_to_cal exists for:
	   without it this comes back as the 31st at 23:59:59.9999996. */
	sat_jd_to_cal(sat_cal_to_jd(2024, 1, 1., 0, 0, 0.), &y, &mo, &d, &h, &mi, &sec);
	check_close("midnight year",  (double)y,  2024., 0.);
	check_close("midnight month", (double)mo,    1., 0.);
	check_close("midnight day",   (double)d,     1., 0.);
	check_close("midnight hour",  (double)h,     0., 0.);
	check_close("midnight sec",   sec,           0., 1e-6);
}

/* End to end: the ISS TLE below is a real one. This does not check an exact position
   -- that is part 1's job -- but that the whole chain produces a track with the right
   gross properties, which is what catches a frame or a unit mistake that part 1 cannot
   see because upstream has no reference for the frame maths. */
static void test_groundtrack_sanity(void)
{
	const char *l1 = "1 25544U 98067A   24015.50000000  .00016717  00000-0  30777-3 0  9005";
	const char *l2 = "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49514637 10110";
	double jd[181], lon[181], lat[181], alt[181];
	double maxlat = -90., minlat = 90.;
	int    err = SAT_OK, i, rc[181];
	void  *sat;

	sat = sat_create(l1, l2, &err);
	checks++;
	if (!sat) {
		printf("  FAIL ISS sat_create: %s\n", sat_strerror(err));
		failures++;
		return;
	}
	/* A 51.64-deg LEO TLE must select the near-earth model. */
	check_close("ISS model is SGP4", (double)sat_model(sat), (double)SAT_MODEL_SGP4, 0.);
	check_close("ISS norad number",  (double)sat_norad_number(sat), 25544., 0.);
	check_close("ISS mean motion",   sat_mean_motion(sat), 15.49514637, 1e-6);
	check_close("ISS inclination",   sat_inclination(sat), 51.6416,     1e-3);

	for (i = 0; i <= 180; i++)              /* 3 hours, ~2 revolutions, 1-min steps */
		jd[i] = sat_epoch_jd(sat) + i / SAT_MIN_PER_DAY;

	check_close("groundtrack returns OK",
	            (double)sat_groundtrack(sat, jd, 181, lon, lat, alt, rc), 0., 0.);

	checks++;
	for (i = 0; i <= 180; i++) {
		if (rc[i] != 0) { printf("  FAIL ISS rc[%d]=%d\n", i, rc[i]); failures++; break; }
		if (!(lon[i] >= -180. && lon[i] <= 180.)) {
			printf("  FAIL ISS lon out of range: %.6f\n", lon[i]); failures++; break;
		}
		/* The ISS is a ~420 km circular orbit; anything outside this band means the
		   ellipsoid maths or the km/earth-radii scaling is wrong. */
		if (!(alt[i] > 300. && alt[i] < 500.)) {
			printf("  FAIL ISS altitude %.3f km at step %d\n", alt[i], i); failures++; break;
		}
		if (lat[i] > maxlat) maxlat = lat[i];
		if (lat[i] < minlat) minlat = lat[i];
	}
	/* Latitude must reach very nearly the inclination, north and south -- two full
	   revolutions is enough to hit both turning points. This is what actually proves
	   the TEME->ECEF rotation is about the right axis: get it wrong and the track
	   still looks like a sinusoid, but its amplitude is not the inclination. */
	check_close("ISS max lat ~ inclination",  maxlat,  51.6416, 0.5);
	check_close("ISS min lat ~ -inclination", minlat, -51.6416, 0.5);

	sat_destroy(sat);

	/* A deep-space TLE must be promoted to SDP4 without anyone asking. */
	{
		const char *g1 = "1 23581U 95025A   01311.43599209 -.00000094  00000-0  00000+0 0  8214";
		const char *g2 = "2 23581   1.1236  93.7945 0005741 214.4722 151.5103  1.00270260 23672";
		void *geo = sat_create(g1, g2, &err);
		checks++;
		if (!geo) {
			printf("  FAIL GOES-9 sat_create: %s\n", sat_strerror(err));
			failures++;
			return;
		}
		check_close("GOES-9 promoted to SDP4",
		            (double)sat_model(geo), (double)SAT_MODEL_SDP4, 0.);
		{	/* geostationary: ~35786 km up, and barely moving in longitude */
			double j = sat_epoch_jd(geo), lo, la, al;
			sat_groundtrack(geo, &j, 1, &lo, &la, &al, NULL);
			check_close("GOES-9 altitude", al, 35786., 200.);
		}
		sat_destroy(geo);
	}
}

/*----------------------------------------------------------------------------------*/
static void test_bad_input(void)
{
	int err = SAT_OK;

	checks++;
	if (sat_create("garbage", "also garbage", &err) != NULL) {
		printf("  FAIL sat_create accepted garbage\n");  failures++;
	}
	else if (err != SAT_ERR_TLE_PARSE) {
		printf("  FAIL wrong error for garbage: %d\n", err);  failures++;
	}

	checks++;
	if (sat_create(NULL, NULL, &err) != NULL || err != SAT_ERR_ARG) {
		printf("  FAIL sat_create(NULL) not rejected\n");  failures++;
	}

	/* Every accessor must tolerate a NULL handle rather than dereference it. */
	checks++;
	if (sat_epoch_jd(NULL) != 0. || sat_norad_number(NULL) != -1 ||
	    sat_model(NULL) != 0 || sat_intl_desig(NULL, NULL) != SAT_ERR_ARG) {
		printf("  FAIL NULL-handle accessors\n");  failures++;
	}

	checks++;
	if (sat_propagate(NULL, NULL, 0, NULL, NULL, NULL) != SAT_ERR_ARG) {
		printf("  FAIL sat_propagate(NULL) not rejected\n");  failures++;
	}

	sat_destroy(NULL);            /* must not crash */
}

int main(void)
{
	printf("test_satellite -- SGP4/SDP4 regression\n");

	printf("[1] against upstream's published vectors\n");   test_against_reference();
	printf("[1b] JD vs tsince entry points\n");             test_jd_vs_tsince();
	printf("[2] GMST\n");                                   test_gmst();
	printf("[3] ECEF <-> geodetic\n");                      test_geodetic();
	printf("[4] frames + calendar\n");                      test_frames_and_calendar();
	printf("[5] end-to-end ground track\n");                test_groundtrack_sanity();
	printf("[6] bad input\n");                              test_bad_input();

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures) {
		printf("FAILED\n");
		return 1;
	}
	printf("OK\n");
	return 0;
}
