/*--------------------------------------------------------------------
 *    The MB-system:  mb_cube.h  10/3/2026
 *
 *    See README.md file for copying and redistribution conditions.
 *--------------------------------------------------------------------*/
/*
 * mb_cube.h declares a C port of bathycube's cube.py, the Python implementation of the CUBE
 * module, Combined Uncertainty and Bathymetry Estimator.  Python implementation done by Eric
 * Younkin, Feb 2022 (https://github.com/noaa-ocs-hydrography/bathycube).
 *
 * CUBE was developed as a research project within the Center of for Coastal and Ocean Mapping
 * and NOAA/UNH Joint Hydrographic Center (CCOM/JHC) at the University of New Hampshire, starting
 * in the fall of 2000.  Reference: Calder, B. R. and Mayer, L. A., "Automatic processing of
 * high-rate, high-density multibeam echosounder data", Geochem. Geophys. Geosyst. 4(6), 2003.
 *
 * bathycube is distributed under the MIT License:
 *
 *   Copyright (c) 2022 NOAA OCS Hydrography
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this
 *   software and associated documentation files (the "Software"), to deal in the Software
 *   without restriction, including without limitation the rights to use, copy, modify, merge,
 *   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
 *   to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 *   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 *   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 *   FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 *
 * The engine is pure C with no MBIO or GMT dependency.  All distances are in projected
 * (metric) units: a caller holding geographic coordinates converts them to local metres first
 * (mbgrid does so with mb_coor_scale).
 *
 * Grid geometry is bathycube's: the grid origin is (minimum_easting, maximum_northing), the
 * outer corner of the north-west cell; node (row, col) sits at the CENTRE of its cell,
 *     x = minimum_easting  + col * resolution_x + resolution_x / 2
 *     y = maximum_northing - row * resolution_y - resolution_y / 2
 * and row 0 is the northernmost row.  mb_cube_grid_get_values() can write its result in that
 * order (MB_CUBE_LAYOUT_ROWS_NORTH, bathycube's (rows, columns) numpy shape) or column-major
 * with y ascending (MB_CUBE_LAYOUT_COLS_SOUTH, mbgrid's grid[ix * gydim + iy]).
 *
 * Deviations from cube.py, all of them fixes, are listed at the top of mb_cube.c.
 */

#ifndef MB_CUBE_H_
#define MB_CUBE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* MB-System exports every symbol of its libraries (CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS), so the
   macro is empty there.  A host that compiles mb_cube.c into its own shared library and exports
   only what it marks (InteractiveGMT's gmtvtk.dll) defines MB_CUBE_BUILD_DLL for that file. */
#ifndef MB_CUBE_API
#  if defined(MB_CUBE_BUILD_DLL) && defined(_WIN32)
#    define MB_CUBE_API __declspec(dllexport)
#  elif defined(MB_CUBE_BUILD_DLL) && defined(__GNUC__)
#    define MB_CUBE_API __attribute__((visibility("default")))
#  else
#    define MB_CUBE_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* return codes */
#define MB_CUBE_OK 0
#define MB_CUBE_ERR_ARG -1
#define MB_CUBE_ERR_MEMORY -2
#define MB_CUBE_ERR_NOT_FOUND -3 /* no hypothesis within depth_tolerance */
#define MB_CUBE_ERR_AMBIGUOUS -4 /* more than one hypothesis within depth_tolerance */
#define MB_CUBE_ERR_FILE -5

/* IHO S-44 survey orders */
typedef enum {
  MB_CUBE_IHO_EXCLUSIVE = 0,
  MB_CUBE_IHO_SPECIAL = 1,
  MB_CUBE_IHO_ORDER1A = 2,
  MB_CUBE_IHO_ORDER1B = 3,
  MB_CUBE_IHO_ORDER2 = 4,
} mb_cube_iho_t;

/* method used to extract information from sheet (CubeParameters.extractor) */
typedef enum {
  MB_CUBE_EXTRACTOR_LHOOD = 0,
  MB_CUBE_EXTRACTOR_PRIOR = 1,
  MB_CUBE_EXTRACTOR_POSTERIOR = 2,
  MB_CUBE_EXTRACTOR_PREDSURF = 3,
  MB_CUBE_EXTRACTOR_UNION = 4,
} mb_cube_extractor_t;

/* method used to determine the appropriate hypothesis value (CubeGrid.get_grid_values) */
typedef enum {
  MB_CUBE_METHOD_LOCAL = 0,     /* local spatial context: closest single-hypothesis node guides the choice */
  MB_CUBE_METHOD_POSTERIOR = 1, /* prior and local combined into an approximate Bayesian posterior */
  MB_CUBE_METHOD_PRIOR = 2,     /* the hypothesis with the most points */
  MB_CUBE_METHOD_PREDICTED = 3, /* the hypothesis closest to the node's predicted depth */
} mb_cube_method_t;

/* Controls the reported variance, one of 'cube' to use CUBE's posterior variance estimate,
   'input' to track and use input sample variance, and 'max' to report the greater of the two */
typedef enum {
  MB_CUBE_VARIANCE_CUBE = 0,
  MB_CUBE_VARIANCE_INPUT = 1,
  MB_CUBE_VARIANCE_MAX = 2,
} mb_cube_variance_t;

/* output layouts of mb_cube_grid_get_values() */
typedef enum {
  MB_CUBE_LAYOUT_ROWS_NORTH = 0, /* out[row * num_columns + col], row 0 = north (bathycube) */
  MB_CUBE_LAYOUT_COLS_SOUTH = 1, /* out[col * num_rows + (num_rows - 1 - row)], y ascending (mbgrid) */
} mb_cube_layout_t;

/* logging levels (bathycube's logging.DEBUG / INFO / WARNING / ERROR) */
typedef enum {
  MB_CUBE_LOG_ERROR = 0,
  MB_CUBE_LOG_WARNING = 1,
  MB_CUBE_LOG_INFO = 2,
  MB_CUBE_LOG_DEBUG = 3,
} mb_cube_loglevel_t;

/* CubeParameters.  Fill with mb_cube_params_default(), override what you want, then call
   mb_cube_params_initialize(), which derives every situational value (those related to IHO order
   or grid resolution) from the values in the struct at that moment. */
typedef struct mb_cube_params {
  mb_cube_iho_t iho_order;
  double grid_resolution_x;
  double grid_resolution_y;

  /* initialization_interlock (system mapsheet initialization marker) is a None placeholder in
     bathycube and is not carried */
  double no_data_value;              /* Value used to indicate no data */
  mb_cube_extractor_t extractor;     /* method used to extract information from sheet, one of 'lhood', 'prior',
                                        'posterior', 'predsurf', 'union' */
  double nodata_depth;               /* depth to initialize estimates */
  double nodata_variance;            /* variance value for initialization */
  double dist_exponent;              /* exponent on distance for variance scale */
  double inv_dist_exponent;          /* inverse of dist exponent for efficiency */
  double dist_scale;                 /* normalization coefficient for distance */
  double var_scale;                  /* variance scale dilution factor, placeholder, will be computed on initialization */
  double iho_fixed;                  /* fixed portion of IHO error budget, placeholder, will be computed on initialization */
  double iho_percent;                /* variable portion of IHO error budget, placeholder, will be computed on initialization */
  int median_length;                 /* Length of median pre-filter sort queue (must be odd number for algorithm) */
  double quotient_limit;             /* Outlier quotient upper allowable limit, Approx. 0.1% F(1,6) */
  double discount;                   /* Discount factor for evolution noise variance */
  double est_offset;                 /* Threshold for significant offset from current estimate to warrant an intervention,
                                        Set by West & Harrison's method of significant percentage points. */
  double bayes_factor_threshold;     /* Bayes factor threshold for either a single estimate, or the worst case recent
                                        sequence to warrant an intervention, Set by West & Harrison's method of
                                        significant evidence for M_1 */
  int runlength_threshold;           /* Run length threshold for worst case recent sequence to indicate a drift failure
                                        and hence to warrant an intervention, Ball-park figure following West &
                                        Harrison's method */
  double min_context;                /* Minimum context search range for hypothesis disambiguation algorithm (in
                                        distance units; converted to nodes by initialize) */
  double max_context;                /* Maximum context search range (in distance units) */
  int min_context_nodes;             /* min_context in nodes, computed on initialization */
  int max_context_nodes;             /* max_context in nodes, computed on initialization */
  double stddev_to_conf_scale;       /* Scale from Std.Dev.to CI, 95 percent CI */
  /* blunders = beam solutions generated by the multibeam that do not correctly represent the seafloor */
  double blunder_min;                /* Minimum depth difference from pred.depth to consider a blunder */
  double blunder_percent;            /* Percentage of predicted depth to be considered a blunder, if more than the
                                        minimum (0 < p < 1, typ.0.25). */
  double blunder_scalar;             /* Scale on initialisation surface std. dev. at a node to allow before
                                        considering deep spikes to be blunders. */
  double capture_dist_scale;         /* Scale on predicted or estimated depth for how far out to accept data.
                                        (unitless; typically 0.05 for hydrography but can be greater for geological
                                        mapping in flat areas with sparse data) */
  mb_cube_variance_t variance_selection;

  /* CubeNode settings that bathycube's CubeGrid leaves at their CubeNode defaults */
  double depth_tolerance;            /* the maximum difference allowed when searching hypotheses by depth */
  double max_hypothesis_ratio;       /* ceiling to place on hypothesis strength ratios */
} mb_cube_params;

/* opaque types */
typedef struct mb_cube_node mb_cube_node;
typedef struct mb_cube_grid mb_cube_grid;

/* one node's answer: what bathycube's value identifiers 'depth', 'uncertainty', 'ratio' and
   'n_hypotheses' return, plus n_points (the number of soundings in the reported hypothesis) */
typedef struct mb_cube_answer {
  double depth;
  double uncertainty;
  double ratio;
  double n_hypotheses;
  double n_points;
} mb_cube_answer;

/* ---- IHO limits and names ------------------------------------------------------------------ */

/* Get fixed and variable Total Vertical Uncertainty components for the different IHO Order
   categories, see S-44 Table 1 - Minimum Bathymetry Standards for Safety of Navigation
   Hydrographic Surveys.  'a' is the fixed component of the TVU equation, 'b' the variable one. */
MB_CUBE_API int mb_cube_iho_limits(mb_cube_iho_t iho_order, double *a, double *b);

/* Same table, Total Horizontal Uncertainty: THU = a + b * depth, both at 95% confidence.
   (Not in bathycube; used by callers that have no per-sounding horizontal uncertainty.) */
MB_CUBE_API int mb_cube_iho_thu_limits(mb_cube_iho_t iho_order, double *a, double *b);

MB_CUBE_API int mb_cube_iho_from_name(const char *name, mb_cube_iho_t *iho_order);
MB_CUBE_API const char *mb_cube_iho_name(mb_cube_iho_t iho_order);
MB_CUBE_API int mb_cube_method_from_name(const char *name, mb_cube_method_t *method);
MB_CUBE_API const char *mb_cube_method_name(mb_cube_method_t method);
MB_CUBE_API int mb_cube_variance_from_name(const char *name, mb_cube_variance_t *variance_selection);
MB_CUBE_API const char *mb_cube_variance_name(mb_cube_variance_t variance_selection);
MB_CUBE_API int mb_cube_extractor_from_name(const char *name, mb_cube_extractor_t *extractor);
MB_CUBE_API const char *mb_cube_extractor_name(mb_cube_extractor_t extractor);
MB_CUBE_API const char *mb_cube_strerror(int code);

/* ---- CubeParameters ------------------------------------------------------------------------ */

MB_CUBE_API void mb_cube_params_default(mb_cube_params *p);

/* Build the situational parameters now, those related to IHO order or grid resolution. */
MB_CUBE_API int mb_cube_params_initialize(mb_cube_params *p, mb_cube_iho_t iho_order, double grid_resolution_x,
                              double grid_resolution_y);

/* The parameter file is the JSON object bathycube's write_parameter_file() writes (json.dump of
   the CubeParameters __dict__), and open_parameter_file() accepts any such object: every key
   that names a parameter is applied, unknown keys are ignored.  Reading does not re-initialize;
   call mb_cube_params_initialize() afterwards so derived values follow what was read.
   *valid_data (may be NULL) tells whether the file held any parameter at all. */
MB_CUBE_API int mb_cube_params_write(const mb_cube_params *p, const char *param_file);
MB_CUBE_API int mb_cube_params_read(mb_cube_params *p, const char *param_file, bool *valid_data);

/* ---- CubeGrid ------------------------------------------------------------------------------ */

/* Main structure for Cube, holds CubeNodes in a grid with metadata.  param must already be
   initialized.  use_queue executes the 'Reordering' step, see CUBE User Manual 3.1; with it set
   to false this step is skipped (the User Manual states that with multiple hypothesis
   implementation of CUBE, Reordering is no longer necessary).  logfile may be NULL; debug enables
   the DEBUG-level messages.  Returns NULL on bad arguments or out of memory. */
MB_CUBE_API mb_cube_grid *mb_cube_grid_new(double minimum_easting, double maximum_northing, int num_columns, int num_rows,
                               double resolution_x, double resolution_y, const mb_cube_params *param, bool use_queue,
                               const char *logfile, bool debug);
MB_CUBE_API void mb_cube_grid_free(mb_cube_grid **grid);

MB_CUBE_API const mb_cube_params *mb_cube_grid_params(const mb_cube_grid *grid);

/* Add an array of point values to the grid.  depth: new depth values; horizontal_uncertainty and
   vertical_uncertainty: the variances (units^2) associated with the points; easting, northing:
   the positions, in the grid's projected units. */
MB_CUBE_API int mb_cube_grid_insert(mb_cube_grid *grid, size_t n, const double *depth, const double *horizontal_uncertainty,
                        const double *vertical_uncertainty, const double *easting, const double *northing);

/* Flush the queues for each node.  Must be done before extracting depth estimates. */
MB_CUBE_API void mb_cube_grid_flush(mb_cube_grid *grid);

MB_CUBE_API size_t mb_cube_grid_populated_nodes_count(const mb_cube_grid *grid);
MB_CUBE_API size_t mb_cube_grid_empty_nodes_count(const mb_cube_grid *grid);
MB_CUBE_API size_t mb_cube_grid_total_nodes_count(const mb_cube_grid *grid);

/* Get the values for each node in the grid.  Any output pointer may be NULL; each non-NULL one
   holds num_rows * num_columns floats in the given layout.  n_hypotheses comes back without the
   expensive hypothesis selection when it is the only output requested. */
MB_CUBE_API int mb_cube_grid_get_values(mb_cube_grid *grid, mb_cube_method_t method, float *depth, float *uncertainty,
                            float *ratio, float *n_hypotheses, float *n_points, mb_cube_layout_t layout);

/* ---- CubeNode (addressed through the grid) ------------------------------------------------- */

MB_CUBE_API mb_cube_node *mb_cube_grid_node(mb_cube_grid *grid, int row, int col);

MB_CUBE_API double mb_cube_node_predicted_depth(const mb_cube_node *node);
MB_CUBE_API double mb_cube_node_predicted_variance(const mb_cube_node *node);
MB_CUBE_API void mb_cube_node_set_predicted_depth(mb_cube_node *node, double new_depth);
MB_CUBE_API void mb_cube_node_set_predicted_variance(mb_cube_node *node, double new_variance);

MB_CUBE_API int mb_cube_node_add_hypothesis(mb_cube_grid *grid, mb_cube_node *node, double depth, double variance,
                                bool null_hypothesis);
MB_CUBE_API int mb_cube_node_remove_hypothesis(mb_cube_grid *grid, mb_cube_node *node, double depth);
MB_CUBE_API int mb_cube_node_nominate_hypothesis(mb_cube_grid *grid, mb_cube_node *node, double depth);
MB_CUBE_API void mb_cube_node_clear_nomination(mb_cube_grid *grid, mb_cube_node *node);
MB_CUBE_API bool mb_cube_node_has_nomination(const mb_cube_node *node);
MB_CUBE_API int mb_cube_node_number_of_hypotheses(const mb_cube_node *node);
MB_CUBE_API int mb_cube_node_number_queued(const mb_cube_node *node);

/* Insert a point into the node (squared distance from point to node, as bathycube passes it). */
MB_CUBE_API int mb_cube_node_add_point(mb_cube_grid *grid, mb_cube_node *node, double depth, double vertical_uncertainty,
                           double horizontal_uncertainty, double distance_to_node);
MB_CUBE_API int mb_cube_node_flush_queue(mb_cube_grid *grid, mb_cube_node *node);

MB_CUBE_API void mb_cube_node_extract_value(mb_cube_grid *grid, const mb_cube_node *node, mb_cube_answer *answer);
MB_CUBE_API void mb_cube_node_extract_closest_value(mb_cube_grid *grid, const mb_cube_node *node, double depth, double variance,
                                        mb_cube_answer *answer);
MB_CUBE_API void mb_cube_node_extract_posterior_weighted_value(mb_cube_grid *grid, const mb_cube_node *node, double depth,
                                                   double variance, mb_cube_answer *answer);
MB_CUBE_API double mb_cube_node_return_depth(mb_cube_grid *grid, const mb_cube_node *node);
MB_CUBE_API double mb_cube_node_return_uncertainty(mb_cube_grid *grid, const mb_cube_node *node);

/* Hypothesis i (0-based) of the node; any out pointer may be NULL. */
MB_CUBE_API int mb_cube_node_hypothesis(const mb_cube_node *node, int i, double *current_depth, double *current_variance,
                            int *number_of_points, int *hypothesis_number);

/* Print the status of each hypothesis */
MB_CUBE_API void mb_cube_node_dump_hypotheses(const mb_cube_node *node, FILE *fp);

/* ---- run_cube_gridding --------------------------------------------------------------------- */

/* Entrance point, run this to run Cube.  params may be NULL (defaults); when given, its values
   are used and then initialized with iho_order and the resolutions.  Each output grid (any may
   be NULL) holds num_rows * num_columns floats, MB_CUBE_LAYOUT_ROWS_NORTH: bathycube's
   (rows, columns). */
MB_CUBE_API int mb_cube_run_gridding(size_t n, const double *depth, const double *horizontal_uncertainty,
                         const double *vertical_uncertainty, const double *easting, const double *northing,
                         int num_columns, int num_rows, double minimum_easting, double maximum_northing,
                         mb_cube_method_t method, mb_cube_iho_t iho_order, double grid_resolution_x,
                         double grid_resolution_y, const mb_cube_params *params, float *depth_grid,
                         float *uncertainty_grid, float *ratio_grid, float *numhyp_grid);

#ifdef __cplusplus
}
#endif

#endif /* MB_CUBE_H_ */
