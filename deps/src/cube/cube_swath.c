/*======================================================================================
 * cube_swath.c -- the soundings of a swath file or datalist, for CUBE. See cube_swath.h.
 *
 * MBIO is the library the swath editor loaded (mbedit_mbio_open). Its table (mbedit_mbio.h)
 * carries read_init / close / register_array / the datalist readers, but not mb_read -- the reader
 * that hands back each beam's longitude and latitude, as mbgrid reads. That one entry point is
 * resolved here from the SAME module, the way mbeditviz resolves its extra table, so the shared
 * loader itself is not touched.
 *====================================================================================*/

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "../mbedit/mbedit_mbio.h"
#include "../mbedit/mbedit.h"
#include "cube_swath.h"

/* mb_status.h: the datalist entry asks for the processed file */
#define CS_PROCESSED_USE 1

typedef int (*cs_mb_read_fn)(int verbose, void *mbio_ptr, int *kind, int *pings, int time_i[7], double *time_d,
                             double *navlon, double *navlat, double *speed, double *heading, double *distance,
                             double *altitude, double *sensordepth, int *nbath, int *namp, int *nss, char *beamflag,
                             double *bath, double *amp, double *bathlon, double *bathlat, double *ss, double *sslon,
                             double *sslat, char *comment, int *error);
static cs_mb_read_fn cs_mb_read = NULL;

static double *cs_lon = NULL, *cs_lat = NULL, *cs_z = NULL;
static int64_t cs_n = 0, cs_cap = 0;

static void cs_free(void) {
	free(cs_lon);
	free(cs_lat);
	free(cs_z);
	cs_lon = cs_lat = cs_z = NULL;
	cs_n = cs_cap = 0;
}

static int cs_push(double lon, double lat, double z) {
	if (cs_n == cs_cap) {
		const int64_t cap = cs_cap ? 2 * cs_cap : 65536;
		double *a = (double *)realloc(cs_lon, (size_t)cap * sizeof(double));
		if (a == NULL) return 0;
		cs_lon = a;
		a = (double *)realloc(cs_lat, (size_t)cap * sizeof(double));
		if (a == NULL) return 0;
		cs_lat = a;
		a = (double *)realloc(cs_z, (size_t)cap * sizeof(double));
		if (a == NULL) return 0;
		cs_z = a;
		cs_cap = cap;
	}
	cs_lon[cs_n] = lon;
	cs_lat[cs_n] = lat;
	cs_z[cs_n] = z;
	cs_n++;
	return 1;
}

/* mb_read, from the module that holds the loaded MBIO table */
static int cs_resolve(void) {
	if (cs_mb_read != NULL)
		return 1;
#ifdef _WIN32
	HMODULE h = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)mbedit_mbio.read_init, &h))
		return 0;
	cs_mb_read = (cs_mb_read_fn)(void *)GetProcAddress(h, "mb_read");
#else
	Dl_info info;
	if (dladdr((void *)mbedit_mbio.read_init, &info) == 0 || info.dli_fname == NULL)
		return 0;
	void *h = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);   /* already loaded: only its handle */
	if (h == NULL)
		return 0;
	cs_mb_read = (cs_mb_read_fn)dlsym(h, "mb_read");
#endif
	return cs_mb_read != NULL;
}

/* one swath file: every good beam inside bounds */
static int cs_read_file(char *file, int format, int pings, int lonflip, double bounds[4], int btime_i[7],
                        int etime_i[7], double speedmin, double timegap, char *msg, int msglen) {
	void *mbio_ptr = NULL;
	double btime_d, etime_d;
	int beams_bath = 0, beams_amp = 0, pixels_ss = 0, error = MB_ERROR_NO_ERROR;
	if (mbedit_mbio.read_init(0, file, format, pings, lonflip, bounds, btime_i, etime_i, speedmin, timegap, &mbio_ptr,
	                          &btime_d, &etime_d, &beams_bath, &beams_amp, &pixels_ss, &error) != MB_SUCCESS) {
		char *message = NULL;
		mbedit_mbio.error(0, error, &message);
		snprintf(msg, (size_t)msglen, "%s: %s", file, message ? message : "cannot be opened");
		return 0;
	}
	char *beamflag = NULL, comment[MB_PATH_MAXLINE];
	double *bath = NULL, *amp = NULL, *bathlon = NULL, *bathlat = NULL, *ss = NULL, *sslon = NULL, *sslat = NULL;
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(char), (void **)&beamflag, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bath, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_AMPLITUDE, sizeof(double), (void **)&amp, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathlon, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_BATHYMETRY, sizeof(double), (void **)&bathlat, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&ss, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&sslon, &error);
	mbedit_mbio.register_array(0, mbio_ptr, MB_MEM_TYPE_SIDESCAN, sizeof(double), (void **)&sslat, &error);
	if (error != MB_ERROR_NO_ERROR) {
		snprintf(msg, (size_t)msglen, "%s: MBIO could not allocate its read arrays", file);
		mbedit_mbio.close(0, &mbio_ptr, &error);
		return 0;
	}
	int ok = 1;
	while (error <= MB_ERROR_NO_ERROR) {
		int kind = 0, rpings = 0, time_i[7], nbath = 0, namp = 0, nss = 0;
		double time_d, navlon, navlat, speed, heading, distance, altitude, sensordepth;
		cs_mb_read(0, mbio_ptr, &kind, &rpings, time_i, &time_d, &navlon, &navlat, &speed, &heading, &distance,
		           &altitude, &sensordepth, &nbath, &namp, &nss, beamflag, bath, amp, bathlon, bathlat, ss, sslon, sslat,
		           comment, &error);
		if (error == MB_ERROR_TIME_GAP)   /* time gaps are not a problem here */
			error = MB_ERROR_NO_ERROR;
		if (error != MB_ERROR_NO_ERROR || kind != MB_DATA_DATA)
			continue;
		for (int ib = 0; ib < nbath; ib++)
			if (mb_beam_ok(beamflag[ib]) && !cs_push(bathlon[ib], bathlat[ib], bath[ib])) {
				snprintf(msg, (size_t)msglen, "out of memory after %lld soundings", (long long)cs_n);
				ok = 0;
				error = MB_ERROR_EOF;
				break;
			}
	}
	mbedit_mbio.close(0, &mbio_ptr, &error);
	return ok;
}

int64_t cube_swath_read(const char *path, int format, const double *bounds, char *msg, int msglen) {
	if (msg && msglen > 0)
		msg[0] = '\0';
	cs_free();
	if (path == NULL || path[0] == '\0') {
		snprintf(msg, (size_t)msglen, "no swath file given");
		return -1;
	}
	if (!mbedit_mbio_loaded()) {
		snprintf(msg, (size_t)msglen, "the MB-System MBIO library is not loaded");
		return -1;
	}
	if (!cs_resolve()) {
		snprintf(msg, (size_t)msglen, "the loaded MBIO library (%s) has no mb_read", mbedit_mbio_version());
		return -1;
	}

	int dformat, pings, lonflip, btime_i[7], etime_i[7];
	double dbounds[4], speedmin, timegap;
	mbedit_mbio.defaults(0, &dformat, &pings, &lonflip, dbounds, btime_i, etime_i, &speedmin, &timegap);
	double rbounds[4] = {-360.0, 360.0, -90.0, 90.0};
	if (bounds != NULL)
		memcpy(rbounds, bounds, sizeof(rbounds));
	lonflip = (rbounds[0] < -180.0) ? -1 : (rbounds[1] > 180.0 ? 1 : 0);

	char file[MB_PATH_MAXLINE];
	snprintf(file, sizeof(file), "%s", path);
	int error = MB_ERROR_NO_ERROR;
	if (format == 0) {
		char root[MB_PATH_MAXLINE];
		if (mbedit_mbio.get_format(0, file, root, &format, &error) != MB_SUCCESS || format == 0) {
			snprintf(msg, (size_t)msglen, "%s: MBIO cannot tell its format", path);
			return -1;
		}
	}

	if (format > 0) {
		if (!cs_read_file(file, format, pings, lonflip, rbounds, btime_i, etime_i, speedmin, timegap, msg, msglen)) {
			cs_free();
			return -1;
		}
		return cs_n;
	}

	/* a datalist: every swath file it names (and the datalists it nests, which MBIO follows) */
	void *datalist = NULL;
	if (mbedit_mbio.datalist_open(0, &datalist, file, MB_DATALIST_LOOK_UNSET, &error) != MB_SUCCESS) {
		snprintf(msg, (size_t)msglen, "%s: cannot open the datalist", path);
		return -1;
	}
	int pstatus = 0, fformat = 0, nfiles = 0;
	double weight = 1.0;
	char fpath[MB_PATH_MAXLINE], ppath[MB_PATH_MAXLINE], dpath[MB_PATH_MAXLINE];
	while (mbedit_mbio.datalist_read2(0, datalist, &pstatus, fpath, ppath, dpath, &fformat, &weight, &error) ==
	       MB_SUCCESS) {
		if (fformat <= 0 || fpath[0] == '#')
			continue;
		char *use = (pstatus == CS_PROCESSED_USE) ? ppath : fpath;
		if (!cs_read_file(use, fformat, pings, lonflip, rbounds, btime_i, etime_i, speedmin, timegap, msg, msglen)) {
			mbedit_mbio.datalist_close(0, &datalist, &error);
			cs_free();
			return -1;
		}
		nfiles++;
	}
	mbedit_mbio.datalist_close(0, &datalist, &error);
	if (nfiles == 0) {
		snprintf(msg, (size_t)msglen, "%s names no swath file", path);
		return -1;
	}
	return cs_n;
}

int64_t cube_swath_take(double *lon, double *lat, double *depth, int64_t n) {
	const int64_t k = (n < cs_n) ? n : cs_n;
	if (k > 0 && lon && lat && depth) {
		memcpy(lon, cs_lon, (size_t)k * sizeof(double));
		memcpy(lat, cs_lat, (size_t)k * sizeof(double));
		memcpy(depth, cs_z, (size_t)k * sizeof(double));
	}
	cs_free();
	return k;
}
