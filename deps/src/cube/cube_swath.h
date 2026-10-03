/*======================================================================================
 * cube_swath.h -- the soundings of a swath file or datalist, for CUBE (Geophysics >
 * MB-System > CUBE gridding). iGMT's own file, NOT part of the verbatim MB-System copy beside it.
 *
 * Read through MB-System's MBIO, the library the swath editor loaded (mbedit_mbio_open, the ONE
 * MBIO loader). Positions are what mb_read hands back -- each beam's longitude/latitude, MBIO's own
 * navigation maths -- and depth is bath[] as MBIO gives it: positive down.
 *====================================================================================*/
#ifndef CUBE_SWATH_H_
#define CUBE_SWATH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read every good beam of `path` (a swath file, or a datalist when format = -1; format 0 asks MBIO
   to guess it from the name). bounds = {west, east, south, north} limits the read like mbgrid's -R
   (NULL = everything). Returns the number of soundings held for cube_swath_take(), or -1 with the
   reason in msg. The MBIO library must already be loaded. */
int64_t cube_swath_read(const char *path, int format, const double *bounds, char *msg, int msglen);

/* Copy the held soundings into caller arrays of n doubles and release them. Returns the count copied. */
int64_t cube_swath_take(double *lon, double *lat, double *depth, int64_t n);

#ifdef __cplusplus
}
#endif
#endif /* CUBE_SWATH_H_ */
