/*======================================================================================
 * mbna_grid.h -- the grid reader the mbnavadjust port's mb_check_gmt_grd / mb_read_gmt_grd ask for
 * (mbna_grid.c): what GMT_Read_Data hands MB-System, delivered by the host. C and C++.
 *====================================================================================*/

#ifndef MBNA_GRID_H_
#define MBNA_GRID_H_

#ifdef __cplusplus
extern "C" {
#endif

/* a GMT grid header, the members MB-System reads */
struct mbna_gmtgrid {
	int n_columns;
	int n_rows;
	int registration;
	double wesn[4];
	double inc[2];
	double z_min;
	double z_max;
	char remark[160];   /* GMT_GRID_REMARK_LEN160 */
};

/* Fill `hdr` from the grid file `path`; with `data` (n_columns * n_rows floats) also its values,
   row-major with the NORTH row first and no pad, NaN where there is no data -- as GMT_Read_Data hands
   them with a zero pad. Returns nonzero on success. */
typedef int (*mbna_grid_reader_fn)(const char *path, struct mbna_gmtgrid *hdr, float *data);
void mbna_set_grid_reader(mbna_grid_reader_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* MBNA_GRID_H_ */
