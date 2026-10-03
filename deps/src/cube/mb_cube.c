/*--------------------------------------------------------------------
 *    The MB-system:  mb_cube.c  10/3/2026
 *
 *    See README.md file for copying and redistribution conditions.
 *--------------------------------------------------------------------*/
/*
 * mb_cube.c -- C port of bathycube's cube.py: Python implementation of the CUBE module,
 * Combined Uncertainty and Bathymetry Estimator.  Python implementation done by Eric Younkin,
 * Feb 2022.  bathycube is MIT licensed; the notice is in mb_cube.h.
 *
 * CUBE was developed as a research project within the Center of for Coastal and Ocean Mapping
 * and NOAA/UNH Joint Hydrographic Center (CCOM/JHC) at the University of New Hampshire, starting
 * in the fall of 2000.
 *
 * The port follows cube.py function for function; bathycube's docstrings and comments are kept
 * as the comments here.  Where cube.py is wrong, the port is fixed rather than copied.  Every
 * such place is marked "PORT FIX" in the code and listed here:
 *
 *  1. Queues are flushed before extraction.  cube.py's run_cube_gridding() never calls
 *     flush_node_queues(), so the last median_length points offered to each node never reach a
 *     hypothesis, and a node that received fewer than median_length points returns no data at
 *     all -- although its own add_to_queue() docstring says the queue "must be flushed before
 *     extracting any depth estimates".  mb_cube_run_gridding() flushes; mbgrid flushes.
 *  2. Context search range.  CubeParameters.initialize() computes min_context/max_context with
 *     min(1, ...), which pins both to at most one node and makes the 'local'/'posterior' search
 *     a single ring.  The intent (and CUBE's) is a floor of one node: max(1, ...).
 *  3. IHO limits.  initialize() squares iho_fixed/iho_percent ("it is in the C code"), but
 *     CubeGrid then re-reads them unsquared from get_iho_limits() and uses those in the
 *     max_variance_allowed equation, mixing metres with metres^2.  The squared values are used.
 *  4. Derived parameters.  run_cube_gridding() applies the keyword overrides AFTER initialize(),
 *     so overriding dist_exponent, min_context, iho_order... left var_scale, inv_dist_exponent,
 *     the context ranges and the IHO terms computed from the old values.  Here initialize() is
 *     always the last step and derives everything from the struct as it stands.
 *  5. Grid edge.  insert_points() rejects a sounding when min_x >= num_columns - 1 (and the same
 *     for rows), so soundings whose footprint touches only the last column or row never reach
 *     those nodes.  The test is min_x >= num_columns.
 *  6. nominate_hypothesis() tested "if min_depth_distance:" -- a found distance of exactly 0.0
 *     is falsy, so an exact match was replaced by the next hypothesis within tolerance.
 *     best_hypothesis_index() had the same 0.0-is-false shape.  Both use a found flag.
 *  7. choose_hypothesis() returns None when no hypothesis holds points (only null hypotheses),
 *     which then crashes on attribute access.  It reports no data.
 *  8. extract_closest_node_value() / extract_posterior_weighted_node_value() divide by
 *     (total_points - nearest.number_of_points), which is zero when every other hypothesis is a
 *     null hypothesis (ZeroDivisionError).  The ratio is 0 there, as for a single hypothesis.
 *  9. get_grid_values() passes the context node's 'uncertainty' (stddev_to_conf_scale * sigma, a
 *     confidence interval in metres) where the extractors expect a variance.  The context
 *     variance (uncertainty / stddev_to_conf_scale)^2 is passed.
 * 10. 'predicted' extraction with a zero predicted variance divides by sqrt(0) for every
 *     hypothesis.  With variance <= 0 the normalised error degenerates to its limit, the plain
 *     depth difference, which is what is minimised.
 * 11. The node's nominated hypothesis is held by index; remove_hypothesis() re-indexes it when an
 *     earlier hypothesis is removed (a Python object reference needed no such care).
 */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mb_cube.h"

/* 95% and 99% confidence scales used by insert_points / add_point_to_node */
#define CONF_95_PERCENT 1.96
#define CONF_99_PERCENT 2.95

typedef struct mb_cube_hypothesis {
  double current_depth;     /* current depth mean estimate */
  double current_variance;  /* current depth variance estimate */
  double predict_depth;     /* current depth next-state mean prediction */
  double predict_variance;  /* current depth next-state variance prediction */
  double cum_bayes_fac;     /* cumulative bayes factor for node monitoring */
  double variance_estimate; /* running estimate of variance of inputs */
  int seq_length;           /* worst case sequence length for monitoring */
  int hypothesis_number;    /* index term for debugging */
  int number_of_points;     /* number of points incorporated into this node */
} mb_cube_hypothesis;

/*
 * CubeNode - The primary estimation structural element.  This maintains the median pre-filter
 * queue, the linked list of depth hypotheses, and the sample statistics for a single node.
 *
 * The per-node settings of bathycube's CubeNode are the grid's mb_cube_params; a node carries
 * only its state, so a large grid stays small.  The queue and the hypothesis list are allocated
 * on first use, and the queue is released when flushed.
 */
struct mb_cube_node {
  double *queue;           /* median_length (depth, variance) pairs, depth-sorted, shallowest first */
  mb_cube_hypothesis *hypotheses;
  int n_queued;
  int n_hypotheses;
  int cap_hypotheses;
  int nominated;           /* index into hypotheses, -1 = None */
  float pred_depth;
  float pred_var;
};

struct mb_cube_grid {
  mb_cube_params param;
  double minimum_easting;
  double maximum_northing;
  int num_columns;
  int num_rows;
  double resolution_x;
  double resolution_y;
  bool use_queue;
  bool debug;
  FILE *logfp;
  mb_cube_node *grid;      /* grid[row * num_columns + col], row 0 = north */
};

/*--------------------------------------------------------------------*/
/* logging: DEBUG and INFO go to stdout, WARNING and ERROR to stderr, everything to the log file */
static void cube_log(const mb_cube_grid *g, mb_cube_loglevel_t level, const char *fmt, ...) {
  if (level == MB_CUBE_LOG_DEBUG && (g == NULL || !g->debug))
    return;
  static const char *names[] = {"ERROR", "WARNING", "INFO", "DEBUG"};
  va_list ap;
  FILE *fp = (level <= MB_CUBE_LOG_WARNING) ? stderr : stdout;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fputc('\n', fp);
  if (g != NULL && g->logfp != NULL) {
    fprintf(g->logfp, "cube - %s - ", names[level]);
    va_start(ap, fmt);
    vfprintf(g->logfp, fmt, ap);
    va_end(ap);
    fputc('\n', g->logfp);
  }
}

/*--------------------------------------------------------------------*/
/*
 * Get fixed and variable Total Vertical Uncertainty components for the different IHO Order
 * categories, see S-44 Table 1 - Minimum Bathymetry Standards for Safety of Navigation
 * Hydrographic Surveys
 *
 * Returns 'a' component, the fixed component of the TVU equation, and 'b' component, the
 * variable component of the TVU equation
 */
int mb_cube_iho_limits(mb_cube_iho_t iho_order, double *a, double *b) {
  double fa, fb;
  switch (iho_order) {
  case MB_CUBE_IHO_EXCLUSIVE: fa = 0.15; fb = 0.0075; break;
  case MB_CUBE_IHO_SPECIAL:   fa = 0.25; fb = 0.0075; break;
  case MB_CUBE_IHO_ORDER1A:   fa = 0.5;  fb = 0.013;  break;
  case MB_CUBE_IHO_ORDER1B:   fa = 0.5;  fb = 0.013;  break;
  case MB_CUBE_IHO_ORDER2:    fa = 1.0;  fb = 0.023;  break;
  default: return MB_CUBE_ERR_ARG;
  }
  if (a != NULL) *a = fa;
  if (b != NULL) *b = fb;
  return MB_CUBE_OK;
}

/* S-44 (6th ed.) Table 1, Total Horizontal Uncertainty, THU = a + b * depth at 95% confidence */
int mb_cube_iho_thu_limits(mb_cube_iho_t iho_order, double *a, double *b) {
  double fa, fb;
  switch (iho_order) {
  case MB_CUBE_IHO_EXCLUSIVE: fa = 1.0;  fb = 0.0;  break;
  case MB_CUBE_IHO_SPECIAL:   fa = 2.0;  fb = 0.0;  break;
  case MB_CUBE_IHO_ORDER1A:   fa = 5.0;  fb = 0.05; break;
  case MB_CUBE_IHO_ORDER1B:   fa = 5.0;  fb = 0.05; break;
  case MB_CUBE_IHO_ORDER2:    fa = 20.0; fb = 0.10; break;
  default: return MB_CUBE_ERR_ARG;
  }
  if (a != NULL) *a = fa;
  if (b != NULL) *b = fb;
  return MB_CUBE_OK;
}

/*--------------------------------------------------------------------*/
/* name tables */

static const char *iho_names[] = {"exclusive", "special", "order1a", "order1b", "order2"};
static const char *method_names[] = {"local", "posterior", "prior", "predicted"};
static const char *variance_names[] = {"cube", "input", "max"};
static const char *extractor_names[] = {"lhood", "prior", "posterior", "predsurf", "union"};

static int name_lookup(const char *name, const char **table, int n) {
  if (name == NULL)
    return -1;
  for (int i = 0; i < n; i++) {
    const char *a = name, *b = table[i];
    while (*a && *b && tolower((unsigned char)*a) == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0')
      return i;
  }
  return -1;
}

int mb_cube_iho_from_name(const char *name, mb_cube_iho_t *iho_order) {
  const int i = name_lookup(name, iho_names, 5);
  if (i < 0) return MB_CUBE_ERR_ARG;
  *iho_order = (mb_cube_iho_t)i;
  return MB_CUBE_OK;
}

const char *mb_cube_iho_name(mb_cube_iho_t iho_order) {
  return ((int)iho_order >= 0 && (int)iho_order < 5) ? iho_names[iho_order] : "unknown";
}

int mb_cube_method_from_name(const char *name, mb_cube_method_t *method) {
  const int i = name_lookup(name, method_names, 4);
  if (i < 0) return MB_CUBE_ERR_ARG;
  *method = (mb_cube_method_t)i;
  return MB_CUBE_OK;
}

const char *mb_cube_method_name(mb_cube_method_t method) {
  return ((int)method >= 0 && (int)method < 4) ? method_names[method] : "unknown";
}

int mb_cube_variance_from_name(const char *name, mb_cube_variance_t *variance_selection) {
  const int i = name_lookup(name, variance_names, 3);
  if (i < 0) return MB_CUBE_ERR_ARG;
  *variance_selection = (mb_cube_variance_t)i;
  return MB_CUBE_OK;
}

const char *mb_cube_variance_name(mb_cube_variance_t variance_selection) {
  return ((int)variance_selection >= 0 && (int)variance_selection < 3) ? variance_names[variance_selection] : "unknown";
}

int mb_cube_extractor_from_name(const char *name, mb_cube_extractor_t *extractor) {
  const int i = name_lookup(name, extractor_names, 5);
  if (i < 0) return MB_CUBE_ERR_ARG;
  *extractor = (mb_cube_extractor_t)i;
  return MB_CUBE_OK;
}

const char *mb_cube_extractor_name(mb_cube_extractor_t extractor) {
  return ((int)extractor >= 0 && (int)extractor < 5) ? extractor_names[extractor] : "unknown";
}

const char *mb_cube_strerror(int code) {
  switch (code) {
  case MB_CUBE_OK: return "no error";
  case MB_CUBE_ERR_ARG: return "invalid argument";
  case MB_CUBE_ERR_MEMORY: return "out of memory";
  case MB_CUBE_ERR_NOT_FOUND: return "no hypothesis found within the depth tolerance";
  case MB_CUBE_ERR_AMBIGUOUS: return "multiple hypotheses found within the depth tolerance";
  case MB_CUBE_ERR_FILE: return "unable to read or write the parameter file";
  default: return "unknown error";
  }
}

/*--------------------------------------------------------------------*/
/* CubeParameters */

void mb_cube_params_default(mb_cube_params *p) {
  memset(p, 0, sizeof(*p));
  p->iho_order = MB_CUBE_IHO_ORDER1A;
  p->grid_resolution_x = 0.0;
  p->grid_resolution_y = 0.0;

  p->no_data_value = (double)NAN;           /* Value used to indicate no data */
  p->extractor = MB_CUBE_EXTRACTOR_LHOOD;   /* method used to extract information from sheet */
  p->nodata_depth = 0.0;                    /* depth to initialize estimates */
  p->nodata_variance = 1000000;             /* variance value for initialization */
  p->dist_exponent = 2.0;                   /* exponent on distance for variance scale */
  p->inv_dist_exponent = 1 / p->dist_exponent; /* inverse of dist exponent for efficiency */
  p->dist_scale = 0.0;                      /* normalization coefficient for distance */
  p->var_scale = 0.0;                       /* variance scale dilution factor, placeholder, will be computed on initialization */
  p->iho_fixed = 0.0;                       /* fixed portion of IHO error budget, placeholder, will be computed on initialization */
  p->iho_percent = 0.0;                     /* variable portion of IHO error budget, placeholder, will be computed on initialization */
  p->median_length = 11;                    /* Length of median pre-filter sort queue (must be odd number for algorithm) */
  p->quotient_limit = 30.0;                 /* Outlier quotient upper allowable limit, Approx. 0.1% F(1,6) */
  p->discount = 1.0;                        /* Discount factor for evolution noise variance */
  p->est_offset = 4.0;                      /* Threshold for significant offset from current estimate to warrant an intervention */
  p->bayes_factor_threshold = 0.135;        /* Bayes factor threshold ... to warrant an intervention */
  p->runlength_threshold = 5;               /* Run length threshold ... to warrant an intervention */
  p->min_context = 5;                       /* Minimum context search range for hypothesis disambiguation algorithm */
  p->max_context = 10;                      /* Maximum context search range */
  p->min_context_nodes = 1;
  p->max_context_nodes = 1;
  p->stddev_to_conf_scale = 1.96;           /* Scale from Std.Dev.to CI, 95 percent CI */
  p->blunder_min = 10.0;                    /* Minimum depth difference from pred.depth to consider a blunder */
  p->blunder_percent = 0.25;                /* Percentage of predicted depth to be considered a blunder */
  p->blunder_scalar = 3.0;                  /* Scale on initialisation surface std. dev. at a node ... */
  p->capture_dist_scale = 0.05;             /* Scale on predicted or estimated depth for how far out to accept data. */

  /* Controls the reported variance, one of 'cube' to use CUBE's posterior variance estimate, 'input' to track and
     use input sample variance, and 'max' to report the greater of the two */
  p->variance_selection = MB_CUBE_VARIANCE_CUBE;

  p->depth_tolerance = 0.01;
  p->max_hypothesis_ratio = 5.0;
}

/*
 * Build the situational parameters now, those related to IHO order or grid resolution.
 *
 * iho_order: one of the IHO order identifiers, i.e. 'order1a'
 * grid_resolution_x: size of the grid cell in the x/easting direction
 * grid_resolution_y: size of the grid cell in the y/northing direction
 */
int mb_cube_params_initialize(mb_cube_params *p, mb_cube_iho_t iho_order, double grid_resolution_x,
                              double grid_resolution_y) {
  if (p == NULL || !(grid_resolution_x > 0.0) || !(grid_resolution_y > 0.0) || !(p->dist_exponent > 0.0))
    return MB_CUBE_ERR_ARG;
  double fixed, percent;
  if (mb_cube_iho_limits(iho_order, &fixed, &percent) != MB_CUBE_OK)
    return MB_CUBE_ERR_ARG;
  p->iho_order = iho_order;
  p->grid_resolution_x = grid_resolution_x;
  p->grid_resolution_y = grid_resolution_y;
  /* PORT FIX 4: re-derived here, so an override of dist_exponent is honoured */
  p->inv_dist_exponent = 1.0 / p->dist_exponent;
  /* Compute distance scale based on node spacing */
  p->dist_scale = (grid_resolution_x < grid_resolution_y) ? grid_resolution_x : grid_resolution_y; /* normalization coefficient for distance */
  /* PORT FIX 2: cube.py had min(1, ...), capping the search at one node; the floor is one node */
  p->min_context_nodes = (int)(p->min_context / p->dist_scale);
  if (p->min_context_nodes < 1)
    p->min_context_nodes = 1;
  p->max_context_nodes = (int)(p->max_context / p->dist_scale);
  if (p->max_context_nodes < 1)
    p->max_context_nodes = 1;
  /* Compute variance scaling factor for dilution function */
  p->var_scale = pow(p->dist_scale, -p->dist_exponent);
  /* IHO Survey Order limits for determining maximum allowable error */
  /* we square these?  Not sure about this but it is in the C code */
  p->iho_fixed = fixed * fixed;
  p->iho_percent = percent * percent;
  if (p->median_length < 1)
    p->median_length = 1;
  return MB_CUBE_OK;
}

/*--------------------------------------------------------------------*/
/* parameter file (bathycube writes json.dump(self.__dict__)) */

static void json_number(FILE *fp, const char *key, double v, bool last) {
  if (isnan(v))
    fprintf(fp, "\"%s\": NaN%s", key, last ? "" : ", ");
  else if (isinf(v))
    fprintf(fp, "\"%s\": %sInfinity%s", key, v < 0 ? "-" : "", last ? "" : ", ");
  else
    fprintf(fp, "\"%s\": %.17g%s", key, v, last ? "" : ", ");
}

int mb_cube_params_write(const mb_cube_params *p, const char *param_file) {
  if (p == NULL || param_file == NULL)
    return MB_CUBE_ERR_ARG;
  FILE *fp = fopen(param_file, "w");
  if (fp == NULL) {
    cube_log(NULL, MB_CUBE_LOG_ERROR, "CubeParameters: Unable to write new parameter file to %s", param_file);
    return MB_CUBE_ERR_FILE;
  }
  fprintf(fp, "{\"iho_order\": \"%s\", ", mb_cube_iho_name(p->iho_order));
  json_number(fp, "grid_resolution_x", p->grid_resolution_x, false);
  json_number(fp, "grid_resolution_y", p->grid_resolution_y, false);
  fprintf(fp, "\"initialization_interlock\": null, ");
  json_number(fp, "no_data_value", p->no_data_value, false);
  fprintf(fp, "\"extractor\": \"%s\", ", mb_cube_extractor_name(p->extractor));
  json_number(fp, "nodata_depth", p->nodata_depth, false);
  json_number(fp, "nodata_variance", p->nodata_variance, false);
  json_number(fp, "dist_exponent", p->dist_exponent, false);
  json_number(fp, "inv_dist_exponent", p->inv_dist_exponent, false);
  json_number(fp, "dist_scale", p->dist_scale, false);
  json_number(fp, "var_scale", p->var_scale, false);
  json_number(fp, "iho_fixed", p->iho_fixed, false);
  json_number(fp, "iho_percent", p->iho_percent, false);
  fprintf(fp, "\"median_length\": %d, ", p->median_length);
  json_number(fp, "quotient_limit", p->quotient_limit, false);
  json_number(fp, "discount", p->discount, false);
  json_number(fp, "est_offset", p->est_offset, false);
  json_number(fp, "bayes_factor_threshold", p->bayes_factor_threshold, false);
  fprintf(fp, "\"runlength_threshold\": %d, ", p->runlength_threshold);
  json_number(fp, "min_context", p->min_context, false);
  json_number(fp, "max_context", p->max_context, false);
  json_number(fp, "stddev_to_conf_scale", p->stddev_to_conf_scale, false);
  json_number(fp, "blunder_min", p->blunder_min, false);
  json_number(fp, "blunder_percent", p->blunder_percent, false);
  json_number(fp, "blunder_scalar", p->blunder_scalar, false);
  json_number(fp, "capture_dist_scale", p->capture_dist_scale, false);
  fprintf(fp, "\"variance_selection\": \"%s\", ", mb_cube_variance_name(p->variance_selection));
  json_number(fp, "depth_tolerance", p->depth_tolerance, false);
  json_number(fp, "max_hypothesis_ratio", p->max_hypothesis_ratio, true);
  fprintf(fp, "}\n");
  const bool ok = (ferror(fp) == 0);
  if (fclose(fp) != 0 || !ok) {
    cube_log(NULL, MB_CUBE_LOG_ERROR, "CubeParameters: Unable to write new parameter file to %s", param_file);
    return MB_CUBE_ERR_FILE;
  }
  cube_log(NULL, MB_CUBE_LOG_INFO, "New CubeParameters file written to %s", param_file);
  return MB_CUBE_OK;
}

/* minimal JSON scanner for one flat object of string / number / null / boolean values */
static const char *json_skip_ws(const char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  return s;
}

static const char *json_string(const char *s, char *out, size_t cap) {
  size_t k = 0;
  if (*s != '"')
    return NULL;
  s++;
  while (*s && *s != '"') {
    char c = *s++;
    if (c == '\\' && *s)
      c = *s++;
    if (k + 1 < cap)
      out[k++] = c;
  }
  if (*s != '"')
    return NULL;
  out[k] = '\0';
  return s + 1;
}

/* value: kind 0 = number, 1 = string, 2 = null/boolean */
static const char *json_value(const char *s, int *kind, double *num, char *str, size_t cap) {
  if (*s == '"') {
    *kind = 1;
    return json_string(s, str, cap);
  }
  if (strncmp(s, "null", 4) == 0 || strncmp(s, "true", 4) == 0) {
    *kind = 2;
    return s + 4;
  }
  if (strncmp(s, "false", 5) == 0) {
    *kind = 2;
    return s + 5;
  }
  *kind = 0;
  if (strncmp(s, "NaN", 3) == 0) {
    *num = (double)NAN;
    return s + 3;
  }
  if (strncmp(s, "Infinity", 8) == 0) {
    *num = (double)INFINITY;
    return s + 8;
  }
  if (strncmp(s, "-Infinity", 9) == 0) {
    *num = -(double)INFINITY;
    return s + 9;
  }
  char *end = NULL;
  *num = strtod(s, &end);
  return (end == s) ? NULL : end;
}

static bool apply_param(mb_cube_params *p, const char *key, int kind, double v, const char *str) {
  struct { const char *key; double *dst; } dbl[] = {
    {"grid_resolution_x", &p->grid_resolution_x}, {"grid_resolution_y", &p->grid_resolution_y},
    {"no_data_value", &p->no_data_value}, {"nodata_depth", &p->nodata_depth},
    {"nodata_variance", &p->nodata_variance}, {"dist_exponent", &p->dist_exponent},
    {"inv_dist_exponent", &p->inv_dist_exponent}, {"dist_scale", &p->dist_scale},
    {"var_scale", &p->var_scale}, {"iho_fixed", &p->iho_fixed}, {"iho_percent", &p->iho_percent},
    {"quotient_limit", &p->quotient_limit}, {"discount", &p->discount}, {"est_offset", &p->est_offset},
    {"bayes_factor_threshold", &p->bayes_factor_threshold}, {"min_context", &p->min_context},
    {"max_context", &p->max_context}, {"stddev_to_conf_scale", &p->stddev_to_conf_scale},
    {"blunder_min", &p->blunder_min}, {"blunder_percent", &p->blunder_percent},
    {"blunder_scalar", &p->blunder_scalar}, {"capture_dist_scale", &p->capture_dist_scale},
    {"depth_tolerance", &p->depth_tolerance}, {"max_hypothesis_ratio", &p->max_hypothesis_ratio},
  };
  for (size_t i = 0; i < sizeof(dbl) / sizeof(dbl[0]); i++)
    if (strcmp(key, dbl[i].key) == 0) {
      if (kind == 0)
        *dbl[i].dst = v;
      else if (kind == 2 && strcmp(key, "no_data_value") == 0)
        *dbl[i].dst = (double)NAN;   /* None */
      else
        return false;
      return true;
    }
  if (strcmp(key, "median_length") == 0 && kind == 0) {
    p->median_length = (int)v;
    return true;
  }
  if (strcmp(key, "runlength_threshold") == 0 && kind == 0) {
    p->runlength_threshold = (int)v;
    return true;
  }
  if (strcmp(key, "iho_order") == 0 && kind == 1)
    return mb_cube_iho_from_name(str, &p->iho_order) == MB_CUBE_OK;
  if (strcmp(key, "extractor") == 0 && kind == 1)
    return mb_cube_extractor_from_name(str, &p->extractor) == MB_CUBE_OK;
  if (strcmp(key, "variance_selection") == 0 && kind == 1)
    return mb_cube_variance_from_name(str, &p->variance_selection) == MB_CUBE_OK;
  if (strcmp(key, "initialization_interlock") == 0)
    return true;   /* None placeholder, accepted and ignored */
  return false;
}

int mb_cube_params_read(mb_cube_params *p, const char *param_file, bool *valid_data_out) {
  if (p == NULL || param_file == NULL)
    return MB_CUBE_ERR_ARG;
  bool valid_data = false;
  if (valid_data_out != NULL)
    *valid_data_out = false;
  FILE *fp = fopen(param_file, "rb");
  if (fp == NULL) {
    cube_log(NULL, MB_CUBE_LOG_ERROR, "CubeParameters: Unable to read data from %s as json", param_file);
    return MB_CUBE_ERR_FILE;
  }
  fseek(fp, 0, SEEK_END);
  const long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (size <= 0) {
    fclose(fp);
    cube_log(NULL, MB_CUBE_LOG_ERROR, "CubeParameters: Unable to read data from %s as json", param_file);
    return MB_CUBE_ERR_FILE;
  }
  char *buf = (char *)malloc((size_t)size + 1);
  if (buf == NULL) {
    fclose(fp);
    return MB_CUBE_ERR_MEMORY;
  }
  const size_t nread = fread(buf, 1, (size_t)size, fp);
  fclose(fp);
  buf[nread] = '\0';

  int status = MB_CUBE_OK;
  const char *s = json_skip_ws(buf);
  if (*s != '{')
    status = MB_CUBE_ERR_FILE;
  else
    s++;
  while (status == MB_CUBE_OK) {
    char key[64], str[64];
    int kind = 0;
    double v = 0.0;
    s = json_skip_ws(s);
    if (*s == '}')
      break;
    if ((s = json_string(s, key, sizeof(key))) == NULL) { status = MB_CUBE_ERR_FILE; break; }
    s = json_skip_ws(s);
    if (*s != ':') { status = MB_CUBE_ERR_FILE; break; }
    s = json_skip_ws(s + 1);
    if ((s = json_value(s, &kind, &v, str, sizeof(str))) == NULL) { status = MB_CUBE_ERR_FILE; break; }
    if (apply_param(p, key, kind, v, str))
      valid_data = true;
    s = json_skip_ws(s);
    if (*s == ',')
      s++;
    else if (*s != '}') { status = MB_CUBE_ERR_FILE; break; }
  }
  free(buf);
  if (status != MB_CUBE_OK) {
    cube_log(NULL, MB_CUBE_LOG_ERROR, "CubeParameters: Unable to read data from %s as json", param_file);
    return status;
  }
  if (valid_data)
    cube_log(NULL, MB_CUBE_LOG_INFO, "CubeParameters read successfully from %s", param_file);
  else
    cube_log(NULL, MB_CUBE_LOG_INFO, "CubeParameters: Unable to find any valid data in %s", param_file);
  if (valid_data_out != NULL)
    *valid_data_out = valid_data;
  return MB_CUBE_OK;
}

/*--------------------------------------------------------------------*/
/* CubeNode */

double mb_cube_node_predicted_depth(const mb_cube_node *node) {
  return node->pred_depth;
}

double mb_cube_node_predicted_variance(const mb_cube_node *node) {
  return node->pred_var;
}

void mb_cube_node_set_predicted_depth(mb_cube_node *node, double new_depth) {
  node->pred_depth = (float)new_depth;
}

void mb_cube_node_set_predicted_variance(mb_cube_node *node, double new_variance) {
  node->pred_var = (float)new_variance;
}

int mb_cube_node_number_of_hypotheses(const mb_cube_node *node) {
  return node->n_hypotheses;
}

int mb_cube_node_number_queued(const mb_cube_node *node) {
  return node->n_queued;
}

/*
 * Add a specific depth hypothesis to the current list
 *
 * depth: depth to set for the hypothesis
 * variance: variance to set for the hypothesis
 * null_hypothesis: if True, this is a null hypothesis, which is a specific hypothesis that has
 *     the number of points set to zero
 */
int mb_cube_node_add_hypothesis(mb_cube_grid *g, mb_cube_node *node, double depth, double variance,
                                bool null_hypothesis) {
  if (node->n_hypotheses == node->cap_hypotheses) {
    const int cap = node->cap_hypotheses ? 2 * node->cap_hypotheses : 2;
    mb_cube_hypothesis *h = (mb_cube_hypothesis *)realloc(node->hypotheses, (size_t)cap * sizeof(mb_cube_hypothesis));
    if (h == NULL)
      return MB_CUBE_ERR_MEMORY;
    node->hypotheses = h;
    node->cap_hypotheses = cap;
  }
  mb_cube_hypothesis *new_hypo = &node->hypotheses[node->n_hypotheses];
  new_hypo->current_depth = depth;        /* current depth mean estimate */
  new_hypo->current_variance = variance;  /* current depth variance estimate */
  new_hypo->predict_depth = depth;        /* current depth next-state mean prediction */
  new_hypo->predict_variance = variance;  /* current depth next-state variance prediction */
  new_hypo->cum_bayes_fac = 1.0;          /* cumulative bayes factor for node monitoring */
  new_hypo->seq_length = 0;               /* worst case sequence length for monitoring */
  new_hypo->number_of_points = 1;         /* number of points incorporated into this node */
  new_hypo->variance_estimate = 0.0;      /* running estimate of variance of inputs */
  if (null_hypothesis)
    new_hypo->number_of_points = 0;
  new_hypo->hypothesis_number = node->n_hypotheses + 1;
  cube_log(g, MB_CUBE_LOG_DEBUG, "add_hypothesis: new hypothesis number %d for depth %g variance %g",
           new_hypo->hypothesis_number, depth, variance);
  node->n_hypotheses++;
  return MB_CUBE_OK;
}

/*
 * This removes a hypothesis from a CubeNode permanently.  The hypothesis to remove is determined
 * by the depth provided.  The algorithm allows up to depth_tolerance difference between this
 * depth and the depth in the hypothesis, but will only remove the hypothesis if there is a unique
 * match to the depth.  Tolerance is nominally a metric whisker (slightly smaller than the
 * imperial), or 0.01m.
 */
int mb_cube_node_remove_hypothesis(mb_cube_grid *g, mb_cube_node *node, double depth) {
  const double tol = g->param.depth_tolerance;
  int found = 0, hypo_idx = -1;
  for (int i = 0; i < node->n_hypotheses; i++)
    if (fabs(depth - node->hypotheses[i].current_depth) < tol) {
      if (found == 0)
        hypo_idx = i;
      found++;
    }
  if (found == 0) {
    cube_log(g, MB_CUBE_LOG_WARNING,
             "remove_hypothesis: unable to remove hypothesis at depth %g, no hypothesis found within %g meters", depth,
             tol);
    return MB_CUBE_ERR_NOT_FOUND;
  }
  if (found > 1) {
    cube_log(g, MB_CUBE_LOG_ERROR,
             "remove_hypothesis: Found multiple hypothesis at depth %g +- %g, unable to remove a single hypothesis",
             depth, tol);
    return MB_CUBE_ERR_AMBIGUOUS;
  }
  /* PORT FIX 11: the nomination is an index -- drop it if it is this one, shift it if it is later */
  if (node->nominated == hypo_idx)
    node->nominated = -1;
  else if (node->nominated > hypo_idx)
    node->nominated--;
  memmove(&node->hypotheses[hypo_idx], &node->hypotheses[hypo_idx + 1],
          (size_t)(node->n_hypotheses - hypo_idx - 1) * sizeof(mb_cube_hypothesis));
  node->n_hypotheses--;
  cube_log(g, MB_CUBE_LOG_DEBUG, "remove_hypothesis: hypothesis number %d removed", hypo_idx);
  return MB_CUBE_OK;
}

/*
 * This searches the list of hypotheses for one with depth within a whisker of the specified
 * value --- in this case, a metric whisker, which is the same as 0.01m.  The hypothesis that
 * matches, or the one that minimises the distance if there is more than one, is marked as
 * 'nominated', and is reconstructed every time without running the disam. engine until the user
 * explicitly resets the over-ride (with cube_node_reset_nomination) or more data is added to the
 * node.
 */
int mb_cube_node_nominate_hypothesis(mb_cube_grid *g, mb_cube_node *node, double depth) {
  bool found = false;
  double min_depth_distance = 0.0;
  int curr_hypo = -1;
  for (int i = 0; i < node->n_hypotheses; i++) {
    const double depth_difference = fabs(depth - node->hypotheses[i].current_depth);
    if (depth_difference < g->param.depth_tolerance) {
      /* PORT FIX 6: "if min_depth_distance:" took a found 0.0 for "nothing found yet" */
      if (found) {  /* this is not the first hypothesis that we have found within the tolerance */
        if (depth_difference < min_depth_distance) {
          min_depth_distance = depth_difference;
          curr_hypo = i;
          cube_log(g, MB_CUBE_LOG_DEBUG,
                   "nominate_hypothesis: clearing previously selected hypothesis for hypothesis, selecting hypothesis at depth %g",
                   depth);
        }
      }
      else {  /* this is the first hypo found within the tolerance */
        found = true;
        min_depth_distance = depth_difference;
        curr_hypo = i;
        cube_log(g, MB_CUBE_LOG_DEBUG, "nominate_hypothesis: selecting hypothesis at depth %g", depth);
      }
    }
  }
  node->nominated = curr_hypo;
  if (node->nominated < 0) {
    cube_log(g, MB_CUBE_LOG_WARNING, "nominate_hypothesis: Warning, no hypothesis found to nominate at depth %g +- %g",
             depth, g->param.depth_tolerance);
    return MB_CUBE_ERR_NOT_FOUND;
  }
  return MB_CUBE_OK;
}

/* Remove the reference to the nominated hypothesis */
void mb_cube_node_clear_nomination(mb_cube_grid *g, mb_cube_node *node) {
  node->nominated = -1;
  cube_log(g, MB_CUBE_LOG_DEBUG, "clear_nomination: remove nominated hypothesis");
}

/* Return True if there is a nominated hypothesis */
bool mb_cube_node_has_nomination(const mb_cube_node *node) {
  return node->nominated >= 0;
}

/*
 * Compute West % Harrison's monitoring statistics for the node hypothesis.  Depends on
 * est_offset (the offset we consider to be significant), bayes_factor_threshold (the Bayes
 * factor threshold before intervention) and runlength_threshold (Number of bad factors to
 * indicate sequence failure).
 *
 * hypo_index: The index of the hypothesis we want to monitor
 * new_depth: new input sample which is about to be incorporated
 * new_variance: observation noise variance
 *
 * Returns False if an intervention is required
 */
static bool monitor_hypothesis(mb_cube_grid *g, mb_cube_node *node, int hypo_index, double new_depth,
                               double new_variance) {
  const mb_cube_params *p = &g->param;
  if (hypo_index < 0 || hypo_index >= node->n_hypotheses) {
    cube_log(g, MB_CUBE_LOG_ERROR, "monitor_hypothesis: Unable to pull hypothesis at index %d", hypo_index);
    return false;
  }
  mb_cube_hypothesis *hypo = &node->hypotheses[hypo_index];

  const double forecast_variance = hypo->predict_variance + new_variance;
  const double error = (new_depth - hypo->predict_depth) / sqrt(forecast_variance);

  /* the est_offset is W&H's `h' parameter (i.e., expected normalised difference between the current forecast and
     the observation which just indicates an outlier) */
  double bayes_factor;
  if (error >= 0)
    bayes_factor = exp(0.5 * (p->est_offset * p->est_offset - (2.0 * p->est_offset * error)));
  else
    bayes_factor = exp(0.5 * (p->est_offset * p->est_offset + (2.0 * p->est_offset * error)));
  cube_log(g, MB_CUBE_LOG_DEBUG, "monitor_hypothesis: calculated bayes factor %g, error %g, forecast variance %g",
           bayes_factor, error, forecast_variance);

  /* check for single component failure */
  /* The bayes_factor_threshold is W&H's `tau' (i.e., the minimum Bayes factor which is acceptable as evidence for the
     current model) */
  if (bayes_factor < p->bayes_factor_threshold) {
    cube_log(g, MB_CUBE_LOG_DEBUG,
             "monitor_hypothesis: bayes factor less than minimum threshold %g, potential outlier",
             p->bayes_factor_threshold);
    return false;
  }
  /* update monitors */
  if (hypo->cum_bayes_fac < 1.0)
    hypo->seq_length += 1;
  else
    hypo->seq_length = 1;
  hypo->cum_bayes_fac = bayes_factor * ((hypo->cum_bayes_fac < 1.0) ? hypo->cum_bayes_fac : 1.0);
  /* check for consecutive failure errors */
  /* The runlength_t is W&H's limit on l_t (i.e., the number of consequtively bad Bayes factors which indicate that
     there has been a gradual shift away from the predictor) */
  if ((hypo->cum_bayes_fac < p->bayes_factor_threshold) || (hypo->seq_length > p->runlength_threshold)) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "monitor_hypothesis: cum bayes fac %g < %g or seq length %d > %d, potential outlier",
             hypo->cum_bayes_fac, p->bayes_factor_threshold, hypo->seq_length, p->runlength_threshold);
    return false;
  }
  cube_log(g, MB_CUBE_LOG_DEBUG, "monitor_hypothesis: no intervention required");
  return true;
}

/* Clear the monitoring data from the provided hypothesis */
static void reset_monitor(mb_cube_grid *g, mb_cube_node *node, int hypo_index) {
  if (hypo_index < 0 || hypo_index >= node->n_hypotheses) {
    cube_log(g, MB_CUBE_LOG_ERROR, "monitor_hypothesis: Unable to pull hypothesis at index %d", hypo_index);
    return;
  }
  node->hypotheses[hypo_index].cum_bayes_fac = 1.0;
  node->hypotheses[hypo_index].seq_length = 0;
  cube_log(g, MB_CUBE_LOG_DEBUG, "reset_monitor: clear the monitoring data from the provided hypothesis");
}

/*
 * Update the given hypothesis (index is provided) being tracked at this node.  This implements
 * the standard univariate dynamic linear model update equations (West & Harrison, 'Bayesian
 * Forecasting and Dynamic Models', Springer, 2ed, 1997, Ch. 2), along with the Bayes factor
 * monitoring code (W&H, Ch. 11).  The only failure mode possible with this code is if the input
 * data would cause an intervention to be requested on the current track.  In this case, it is
 * the caller's responsibility to utilise the data point, since it will not be incorporated into
 * the hypothesis --- typically this would mean adding a new hypothesis and pushing it onto the
 * stack.
 *
 * Returns False if the estimate does not really match the track that the hypothesis represents
 * (i.e., an intervention is required).
 */
static bool update_hypothesis(mb_cube_grid *g, mb_cube_node *node, int hypo_index, double depth, double variance) {
  const mb_cube_params *p = &g->param;
  /* check current estimate with node monitoring */
  const bool monitoring_answer = monitor_hypothesis(g, node, hypo_index, depth, variance);
  if (!monitoring_answer) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "update_hypothesis: monitoring determined an intervention is required");
    return false;
  }
  mb_cube_hypothesis *hypo = &node->hypotheses[hypo_index];

  if (p->variance_selection != MB_CUBE_VARIANCE_CUBE) {
    const double n = (double)hypo->number_of_points;
    hypo->variance_estimate = (n - 1) * hypo->variance_estimate / n +
                              (depth - hypo->current_depth) * (depth - hypo->current_depth) / n;
  }
  /* add capability to 'age' the sounding with a discount factor. */
  const double sys_variance = hypo->current_variance * (1.0 - p->discount) / p->discount;

  const double gain = hypo->predict_variance / (variance + hypo->predict_variance);
  const double innovation = depth - hypo->predict_depth;
  hypo->predict_depth += gain * innovation;
  hypo->current_depth = hypo->predict_depth;
  hypo->current_variance = variance * hypo->predict_variance / (variance + hypo->predict_variance);
  hypo->predict_variance = hypo->current_variance + sys_variance;
  hypo->number_of_points += 1;
  cube_log(g, MB_CUBE_LOG_DEBUG, "update_hypothesis: hypothesis number %d updated with depth %g and variance %g",
           hypo_index, depth, variance);
  return true;
}

/*
 * Find the closest matching hypothesis in the current hypothesis list.  This computes the
 * normalized absolute error between one-step forecast for each hypothesis currently being
 * tracked and the input sample, and returns the index of the hypothesis with the smallest error
 * value.  If there is more than one node with the same error (unlikely in practice, but
 * possible), then the first one in the list is chosen.
 *
 * Returns index to the best hypothesis, -1 if there is none
 */
static int best_hypothesis_index(mb_cube_grid *g, const mb_cube_node *node, double depth, double variance) {
  int best_hypo_index = -1;
  double min_error = 0.0;
  for (int idx = 0; idx < node->n_hypotheses; idx++) {
    const mb_cube_hypothesis *hyp = &node->hypotheses[idx];
    const double forecast_variance = hyp->predict_variance + variance;
    const double error = fabs((depth - hyp->predict_depth) / sqrt(forecast_variance));
    /* PORT FIX 6: "(min_error and error < min_error) or (min_error is None)" -- a 0.0 minimum is falsy */
    if (best_hypo_index < 0 || error < min_error) {
      min_error = error;
      best_hypo_index = idx;
      cube_log(g, MB_CUBE_LOG_DEBUG, "best_hypothesis_index: hypothesis number %d picked with minimum error %g",
               best_hypo_index, min_error);
    }
  }
  return best_hypo_index;
}

/*
 * Choose the best hypothesis for this node.  In this context, `best' means `hypothesis with most
 * points', rather than through any other metric.  This may not be the `best' until all of the
 * data is in, but it should give an idea of what's going on in the data structure at any point
 * (particularly if it changes dramatically from sample to sample)
 *
 * Returns the index of the hypothesis with the most points (-1 if none holds any) and the
 * hypothesis strength ratio for this hypothesis (how convinced CUBE is that the hypo is good)
 */
static int choose_hypothesis(mb_cube_grid *g, const mb_cube_node *node, double *hypo_ratio) {
  int best_hypo = -1;
  int second_highest_count = 0;
  int current_max_pointcount = 0;
  *hypo_ratio = 0.0;
  for (int i = 0; i < node->n_hypotheses; i++) {
    const mb_cube_hypothesis *hyp = &node->hypotheses[i];
    if (hyp->number_of_points > 0) {
      if (hyp->number_of_points > current_max_pointcount) {
        best_hypo = i;
        if (current_max_pointcount > second_highest_count)
          second_highest_count = current_max_pointcount;
        current_max_pointcount = hyp->number_of_points;
      }
      else if (hyp->number_of_points > second_highest_count)
        second_highest_count = hyp->number_of_points;
    }
  }
  if (second_highest_count && current_max_pointcount) {
    const double r = g->param.max_hypothesis_ratio - ((double)current_max_pointcount / second_highest_count);
    *hypo_ratio = (r > 0.0) ? r : 0.0;
  }
  /* PORT FIX 7: no hypothesis with points (only null hypotheses) -> -1, the caller reports no data */
  if (best_hypo >= 0)
    cube_log(g, MB_CUBE_LOG_DEBUG,
             "choose_hypothesis: hypothesis number %d picked as it had the most points (%d), hypothesis strength %g",
             node->hypotheses[best_hypo].hypothesis_number, current_max_pointcount, *hypo_ratio);
  return best_hypo;
}

/*
 * Update the CUBE equations for this node and input.  This runs the basic filter equations,
 * using the KF formulation, and its innovations formulation.  This algorithm now includes a
 * discounted system noise variance model to set the evolution noise dynamically depending on the
 * variance that was estimated at the previous stage (West & Harrison, 'Bayesian Forecasting and
 * Dynamic Models', Springer, 2ed., 1997, ch.2) and a monitoring scheme and feed-back
 * interventions to allow the code to check that the estimates are staying in touch with the
 * input data.  The monitoring scheme is also based on West & Harrison as above, Ch.11, Sec.
 * 11.5.1, using cumulative Bayes factors and the unidirectional level shift alternate model.
 */
static int update_node(mb_cube_grid *g, mb_cube_node *node, double depth, double variance) {
  /* find the best matching hypothesis index for the current input sample given those currently being tracked */
  const int best_idx = best_hypothesis_index(g, node, depth, variance);
  if (best_idx < 0) {
    /* didn't match one, should only happen where there are no hypothesis, so we add a new one */
    return mb_cube_node_add_hypothesis(g, node, depth, variance, false);
  }
  /* update the best hypothesis with the current data */
  const bool updated = update_hypothesis(g, node, best_idx, depth, variance);
  if (!updated) {
    /* failed update - indicates an intervention, so that we need to start a new hypothesis to capture the
       outlier/datum shift */
    reset_monitor(g, node, best_idx);
    const int status = mb_cube_node_add_hypothesis(g, node, depth, variance, false);
    cube_log(g, MB_CUBE_LOG_DEBUG,
             "update_node: no hypothesis updated, depth %g variance %g successfully incorporated as new hypothesis",
             depth, variance);
    return status;
  }
  return MB_CUBE_OK;
}

/*
 * Identify all points that are outliers and remove them from the queue.  The definition of
 * 'outlier' depends on the quotient_limit attribute.  In general, the higher the value, the more
 * extreme the difference between mean depth and point depth must be to be considered an outlier.
 *
 * In theory, the distribution of the quotient values computed should be approximately a Fisher
 * F(1,N-2) where there are N points in the input sequence.  The values of the quotients are
 * always positive, and monotonically increasing for worse outliers; therefore, one-sided critical
 * values should be considered.
 */
static void truncate_queue(mb_cube_grid *g, mb_cube_node *node) {
  if (node->n_queued < 3) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "truncate: with %d queued points, truncate unnecessary", node->n_queued);
    return;
  }
  double mean = 0.0;
  double sum_square_diff = 0.0;
  const int num_points = node->n_queued - 1;  /* number of points + 1 outlier */
  for (int i = 0; i < node->n_queued; i++) {
    const double depth = node->queue[2 * i];
    mean += depth;
    sum_square_diff += depth * depth;
  }
  sum_square_diff -= (mean * mean) / (num_points + 1);
  mean /= (num_points + 1);
  const double sum_square_diff_k = num_points * sum_square_diff / ((double)num_points * num_points - 1);

  /* run the list computing quotients, removing the outliers in place (the order is kept) */
  int kept = 0, removed = 0;
  for (int idx = 0; idx < node->n_queued; idx++) {
    const double depth = node->queue[2 * idx];
    const double diff_sq = (depth - mean) * (depth - mean);
    const double quot = diff_sq / (sum_square_diff_k - (diff_sq / (num_points - 1)));
    if (quot >= g->param.quotient_limit) {
      cube_log(g, MB_CUBE_LOG_DEBUG, "truncate: point %d flagged for removal with quotient value greater than limit %g",
               idx, g->param.quotient_limit);
      removed++;
      continue;
    }
    node->queue[2 * kept] = node->queue[2 * idx];
    node->queue[2 * kept + 1] = node->queue[2 * idx + 1];
    kept++;
  }
  /* now remove the marked entities from the queue */
  node->n_queued = kept;
  cube_log(g, MB_CUBE_LOG_DEBUG, "truncate: removed %d points from the queue", removed);
}

/*
 * This flushes the queue into the input sequence in order (i.e., take current median, resort,
 * repeat).  Since the queue is always sorted, we can just walk the list in order, rather than
 * having to re-sort or shift data, etc.  When we have an even number of points, we take the
 * shallowest of the points first; this means that we walk the list alternately to the left and
 * right, starting to the right if the initial number of points is even, and to the left if the
 * number of points is odd.  To avoid shifting the data, we just increase the step after every
 * extraction, until we step off the LHS of the array.
 *
 * For a list of 10 points, the order ends up looking something like this:
 * [4, 5, 3, 6, 2, 7, 1, 8, 0, 9]
 */
int mb_cube_node_flush_queue(mb_cube_grid *g, mb_cube_node *node) {
  int status = MB_CUBE_OK;
  if (node->n_queued == 0) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "flush_queue: no queued points to flush");
    free(node->queue);
    node->queue = NULL;
    return status;
  }
  int scale = 1;
  truncate_queue(g, node);
  cube_log(g, MB_CUBE_LOG_DEBUG, "flush_queue: flushing %d points", node->n_queued);
  int ex_pt, direction;
  if (node->n_queued % 2 == 0) {  /* even */
    ex_pt = node->n_queued / 2 - 1;
    direction = 1;
  }
  else {  /* odd */
    ex_pt = node->n_queued / 2;
    direction = -1;
  }
  while (ex_pt >= 0 && status == MB_CUBE_OK) {
    status = update_node(g, node, node->queue[2 * ex_pt], node->queue[2 * ex_pt + 1]);
    ex_pt += direction * scale;
    direction = -direction;
    scale += 1;
  }
  free(node->queue);
  node->queue = NULL;
  node->n_queued = 0;
  return status;
}

static int queue_alloc(mb_cube_grid *g, mb_cube_node *node) {
  if (node->queue == NULL) {
    node->queue = (double *)malloc(2 * (size_t)g->param.median_length * sizeof(double));
    if (node->queue == NULL)
      return MB_CUBE_ERR_MEMORY;
    node->n_queued = 0;
  }
  return MB_CUBE_OK;
}

/* Insert a new point into the queue, maintain depth sorted order, with greater depths last */
static int queue_fill(mb_cube_grid *g, mb_cube_node *node, double depth, double variance) {
  if (!g->use_queue) {
    cube_log(g, MB_CUBE_LOG_WARNING, "queue_fill: skipping as use_queue is False");
    return MB_CUBE_OK;
  }
  if (queue_alloc(g, node) != MB_CUBE_OK)
    return MB_CUBE_ERR_MEMORY;
  int insertion_index = 0;
  if (node->n_queued == 0)
    cube_log(g, MB_CUBE_LOG_DEBUG, "queue_fill: queue is empty, adding depth %g, variance %g to queue", depth, variance);
  else {
    for (int i = 0; i < node->n_queued; i++) {
      if (depth > node->queue[2 * i])
        insertion_index = i + 1;
      else
        break;
    }
    cube_log(g, MB_CUBE_LOG_DEBUG, "queue_fill: inserted at index %d, adding depth %g, variance %g to queue",
             insertion_index, depth, variance);
  }
  memmove(&node->queue[2 * (insertion_index + 1)], &node->queue[2 * insertion_index],
          2 * (size_t)(node->n_queued - insertion_index) * sizeof(double));
  node->queue[2 * insertion_index] = depth;
  node->queue[2 * insertion_index + 1] = variance;
  node->n_queued += 1;
  return MB_CUBE_OK;
}

/*
 * Insert a point in the already filled queue.  We then return the median point and insert this
 * new point, ensuring that the queue remains sorted, with greater depths last.
 */
static void queue_insert(mb_cube_grid *g, mb_cube_node *node, double depth, double variance, double *mdepth_out,
                         double *mvariance_out) {
  if (!g->use_queue) {
    cube_log(g, MB_CUBE_LOG_WARNING, "queue_insert: skipping as use_queue is False");
    *mdepth_out = depth;
    *mvariance_out = variance;
    return;
  }
  double *q = node->queue;
  /* 11 / 2 = 5.5, floor(5.5) = 5, 5 being the median index of an array of 11 points */
  const int median_index = g->param.median_length / 2;
  const double mdepth = q[2 * median_index];
  const double mvariance = q[2 * median_index + 1];
  memmove(&q[2 * median_index], &q[2 * (median_index + 1)],
          2 * (size_t)(node->n_queued - median_index - 1) * sizeof(double));
  const int len = node->n_queued - 1;
  int insertion_index = len;  /* append */
  const int start = (depth > mdepth) ? median_index : 0;

  for (int i = start; i < len; i++) {
    if (depth < q[2 * i]) {
      insertion_index = i;
      break;
    }
  }

  if (insertion_index < len)
    cube_log(g, MB_CUBE_LOG_DEBUG, "queue_insert: queue full, inserted at index %d, adding depth %g, variance %g to queue",
             insertion_index, depth, variance);
  else
    cube_log(g, MB_CUBE_LOG_DEBUG, "queue_insert: queue full, adding to end of queue, adding depth %g, variance %g to queue",
             depth, variance);
  memmove(&q[2 * (insertion_index + 1)], &q[2 * insertion_index], 2 * (size_t)(len - insertion_index) * sizeof(double));
  q[2 * insertion_index] = depth;
  q[2 * insertion_index + 1] = variance;

  /* compute the likely 99% confidence bound below the shallowest point and above the deepest point in the
     buffer, and check that they do actually overlap somewhere in the middle.  Otherwise, with less than 1% chance
     of error, we are suspicious that there are outliers in the buffer somewhere and we should attempt a round of
     outlier rejection.  Assuming that the errors are approximately normal, 0.5% in either tail is achieved at
     2.5758 std dev from the mean. */

  /* 2.56 being the 99 percent confidence interval for normal distribution */
  const double low_water = q[2 * (node->n_queued - 1)] - 2.56 * sqrt(q[2 * (node->n_queued - 1) + 1]);
  const double high_water = q[0] + 2.56 * sqrt(q[1]);
  if (low_water >= high_water)  /* confidence limits do not overlap */
    truncate_queue(g, node);    /* remove any outliers */
  cube_log(g, MB_CUBE_LOG_DEBUG, "queue_insert: queue full, returning median point depth %g, variance %g", mdepth,
           mvariance);
  *mdepth_out = mdepth;
  *mvariance_out = mvariance;
}

/*
 * Insert points into the queue of estimates and insert point into the filter sequence if the
 * queue is filled.
 *
 * This inserts the depth given into the queue associated with the specified node, creating the
 * queue if required.  After the queue has been primed (i.e., filled with estimates), on each call
 * this routine extracts the median value from the queue and then inserts it into the CUBE input
 * sequence. Note that this algorithm means that the queue will always be full, and hence must be
 * flushed before extracting any depth estimates (this can also be done to save memory).
 *
 * if use_queue is set to False, skips the queue entirely and runs update_node with the provided
 * depth/variance
 */
static int add_to_queue(mb_cube_grid *g, mb_cube_node *node, double depth, double variance) {
  if (g->use_queue) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "add_to_queue: adding depth %g variance %g to the queue", depth, variance);
    if (node->n_queued < g->param.median_length)
      return queue_fill(g, node, depth, variance);
    double median_depth, median_variance;
    queue_insert(g, node, depth, variance, &median_depth, &median_variance);
    return update_node(g, node, median_depth, median_variance);
  }
  return update_node(g, node, depth, variance);
}

/*
 * Insert a point into the node.  This will compute the variance scale factor for the new data,
 * and send the data into the estimation queue.
 *
 * depth: new depth value to add to the queue
 * vertical_uncertainty: new vertical uncertainty value associated with the point (a variance)
 * horizontal_uncertainty: new horizontal uncertainty value associated with the point (a variance)
 * distance_to_node: distance from point to node (squared)
 */
int mb_cube_node_add_point(mb_cube_grid *g, mb_cube_node *node, double depth, double vertical_uncertainty,
                           double horizontal_uncertainty, double distance_to_node) {
  const mb_cube_params *p = &g->param;
  const double conf_95_percent = CONF_95_PERCENT;
  const double predicted_depth = node->pred_depth;
  if (isnan(predicted_depth)) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "add_point_to_node: Sounding rejected with predicted depth of NaN, sounding depth = %g",
             depth);
    return MB_CUBE_OK;
  }
  /* euclidean distance in projected space, i.e. distance sounding is being propagated from touchdown boresight
     to node estimation point */
  double dist = sqrt(distance_to_node);
  double target_depth;
  if (predicted_depth != 0.0) {
    target_depth = predicted_depth;
    /* do the test for blunders here, since it makes no sense to test when there is no predicted depth */
    /* blunders = beam solutions generated by the multibeam that do not correctly represent the seafloor */
    double blunder_limit = fmin(target_depth - p->blunder_min, target_depth - p->blunder_percent * fabs(target_depth));
    blunder_limit = fmin(blunder_limit, target_depth - p->blunder_scalar * sqrt((double)node->pred_var));
    if (depth < blunder_limit) {
      cube_log(g, MB_CUBE_LOG_DEBUG, "add_point_to_node: Sounding rejected, %g less than blunder limit %g", depth,
               blunder_limit);
      return MB_CUBE_OK;
    }
  }
  else {
    cube_log(g, MB_CUBE_LOG_DEBUG, "add_point_to_node: Blunder limit test pass, no predicted depth for this node");
    target_depth = depth;
  }
  const double calculated_captdist = p->capture_dist_scale * fabs(target_depth);
  const double captdist = fmax(calculated_captdist, 0.5);
  if (dist > captdist) {
    cube_log(g, MB_CUBE_LOG_DEBUG,
             "add_point_to_node: sounding rejected, %g greater than max(0.5 or calculated capture distance %g)", dist,
             calculated_captdist);
    return MB_CUBE_OK;
  }
  cube_log(g, MB_CUBE_LOG_DEBUG, "add_point_to_node: sounding accepted at node, distance %gm, target depth %gm", dist,
           captdist);
  /* add horizontal positioning uncertainty, assumes 2sigma */
  dist += conf_95_percent * sqrt(horizontal_uncertainty);
  /* TODO this asked for range (range != 0) in the original source, don't have range */
  const double sounding_range = 0.0;
  double offset;
  if (sounding_range != 0.0 && (!isnan(predicted_depth) && predicted_depth != 0.0)) {
    offset = predicted_depth - depth;
    cube_log(g, MB_CUBE_LOG_DEBUG, "add_point_to_node: adding offset to depth");
  }
  else
    offset = 0.0;
  const double variance = vertical_uncertainty * (1.0 + p->var_scale * pow(dist, p->dist_exponent));
  const int status = add_to_queue(g, node, depth + offset, variance);
  node->nominated = -1;
  return status;
}

/*--------------------------------------------------------------------*/
/* answers */

static void answer_no_data(const mb_cube_grid *g, mb_cube_answer *a) {
  a->depth = a->uncertainty = a->ratio = a->n_hypotheses = a->n_points = g->param.no_data_value;
}

static double hypothesis_uncertainty(const mb_cube_grid *g, const mb_cube_hypothesis *hyp) {
  const mb_cube_params *p = &g->param;
  if (p->variance_selection == MB_CUBE_VARIANCE_MAX)
    return p->stddev_to_conf_scale * sqrt(fmax(hyp->current_variance, hyp->variance_estimate));
  if (p->variance_selection == MB_CUBE_VARIANCE_INPUT)
    return p->stddev_to_conf_scale * sqrt(hyp->variance_estimate);
  return p->stddev_to_conf_scale * sqrt(hyp->current_variance);
}

/* Return the data for the nominated hypothesis */
static void return_nominated_answer(mb_cube_grid *g, const mb_cube_node *node, mb_cube_answer *a) {
  const mb_cube_hypothesis *nom = &node->hypotheses[node->nominated];
  a->depth = nom->current_depth;
  a->uncertainty = g->param.stddev_to_conf_scale * sqrt(nom->current_variance);
  a->ratio = 0.0;  /* having a nominated hypothesis means there is no ratio, only one hypothesis */
  a->n_hypotheses = node->n_hypotheses;
  a->n_points = nom->number_of_points;
  cube_log(g, MB_CUBE_LOG_DEBUG, "_return_nominated_answer: good hypothesis, returning depth %g uncertainty %g",
           a->depth, a->uncertainty);
}

/* Provide the answer for the given hypothesis. */
static void return_answer_from_hypothesis(mb_cube_grid *g, const mb_cube_node *node, const mb_cube_hypothesis *hyp,
                                          double ratio, mb_cube_answer *a) {
  if (hyp != NULL && hyp->number_of_points > 0) {
    /* Only reconstruct if some data was involved in the construction of the hypothesis.  This excludes
       initial hypotheses from an initialisation surface, which are set up with n_j = 0. */
    a->depth = hyp->current_depth;
    a->uncertainty = hypothesis_uncertainty(g, hyp);
    a->ratio = ratio;
    a->n_hypotheses = node->n_hypotheses;
    a->n_points = hyp->number_of_points;
    cube_log(g, MB_CUBE_LOG_DEBUG, "_return_answer_from_hypothesis: good hypothesis, returning depth %g uncertainty %g",
             a->depth, a->uncertainty);
  }
  else {
    cube_log(g, MB_CUBE_LOG_DEBUG, "_return_answer_from_hypothesis: hypothesis empty, returning nodatavalues");
    answer_no_data(g, a);
  }
}

/*
 * Extract a node value.  These values come from either the nominated hypothesis or a selected
 * hypothesis that has the most points of all hypotheses.  If there is no hypothesis, you will get
 * a no_data_value for each value.
 */
void mb_cube_node_extract_value(mb_cube_grid *g, const mb_cube_node *node, mb_cube_answer *a) {
  cube_log(g, MB_CUBE_LOG_DEBUG, "extract_node_value: getting hypothesis answer");
  if (node->nominated >= 0) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_node_value: using nominated hypothesis");
    return_nominated_answer(g, node, a);
    return;
  }
  if (node->n_hypotheses == 0) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_node_value: no hypothesis found");
    answer_no_data(g, a);
  }
  else if (node->n_hypotheses == 1) {  /* Special case: only one depth hypothesis (the usual case, we hope ...) */
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_node_value: only one hypothesis!");
    return_answer_from_hypothesis(g, node, &node->hypotheses[0], 0.0, a);  /* ratio of 0 for only having one hypothesis */
  }
  else {
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_node_value: multiple hypotheses found");
    double ratio;
    const int hyp = choose_hypothesis(g, node, &ratio);
    return_answer_from_hypothesis(g, node, (hyp >= 0) ? &node->hypotheses[hyp] : NULL, ratio, a);
  }
}

/* PORT FIX 8: the strength ratio with the other hypotheses' point total; 0 when they hold none */
static double strength_ratio(const mb_cube_grid *g, int n, int total_points) {
  const int others = total_points - n;
  if (others <= 0)
    return 0.0;
  const double r = g->param.max_hypothesis_ratio - ((double)n / others);
  return (r > 0.0) ? r : 0.0;
}

/*
 * Extract the node values for the hypothesis which is closest in depth to the supplied
 * depth/variance point values, in a minimum error sense.  If there are no depth hypotheses in
 * this node, no_data_value is returned.
 */
void mb_cube_node_extract_closest_value(mb_cube_grid *g, const mb_cube_node *node, double depth, double variance,
                                        mb_cube_answer *a) {
  if (node->nominated >= 0) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_closest_node_value: using nominated hypothesis");
    return_nominated_answer(g, node, a);
    return;
  }
  if (node->n_hypotheses <= 1) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_closest_node_value: one hypothesis or less, falling back on basic extraction");
    /* with no hypotheses or just one hypothesis, this is just doing the basic extraction */
    mb_cube_node_extract_value(g, node, a);
    return;
  }
  /* PORT FIX 10: with no variance the normalised error degenerates to the plain depth difference */
  const double norm = (variance > 0.0) ? sqrt(variance) : 1.0;
  double min_error = 0.0;
  int nearest_hypo = -1;
  int total_points = 0;
  for (int i = 0; i < node->n_hypotheses; i++) {
    const mb_cube_hypothesis *hyp = &node->hypotheses[i];
    if (hyp->number_of_points > 0) {  /* check that some data were used in making the hypothesis before accepting it as valid */
      const double error = fabs((hyp->current_depth - depth) / norm);
      if (nearest_hypo < 0 || error < min_error) {
        min_error = error;
        nearest_hypo = i;
      }
      total_points += hyp->number_of_points;
    }
  }
  if (nearest_hypo < 0) {  /* should never get to this point */
    cube_log(g, MB_CUBE_LOG_WARNING, "extract_closest_node_value: no hypothesis found!");
    answer_no_data(g, a);
  }
  else {
    const mb_cube_hypothesis *hyp = &node->hypotheses[nearest_hypo];
    return_answer_from_hypothesis(g, node, hyp, strength_ratio(g, hyp->number_of_points, total_points), a);
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_closest_node_value: found depth %g", a->depth);
  }
}

/*
 * Extract a posterior weighted best depth hypothesis using the provided depth/variance values as
 * a guide.
 */
void mb_cube_node_extract_posterior_weighted_value(mb_cube_grid *g, const mb_cube_node *node, double depth,
                                                   double variance, mb_cube_answer *a) {
  if (node->nominated >= 0) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_posterior_weighted_node_value: using nominated hypothesis");
    return_nominated_answer(g, node, a);
    return;
  }
  if (node->n_hypotheses <= 1) {
    cube_log(g, MB_CUBE_LOG_DEBUG,
             "extract_posterior_weighted_node_value: one hypothesis or less, falling back on basic extraction");
    /* with no hypotheses or just one hypothesis, this is just doing the basic extraction */
    mb_cube_node_extract_value(g, node, a);
    return;
  }
  double max_posterior = 0.0;
  int nearest_hypo = -1;
  int total_points = 0;
  for (int i = 0; i < node->n_hypotheses; i++) {
    const mb_cube_hypothesis *hyp = &node->hypotheses[i];
    if (hyp->number_of_points > 0) {  /* check that some data were used in making the hypothesis before accepting it as valid */
      const double mean = hyp->current_depth;
      const double posterior = -(depth - mean) * (depth - mean) / (2.0 * variance) + log((double)hyp->number_of_points);
      if (nearest_hypo < 0 || posterior > max_posterior) {
        max_posterior = posterior;
        nearest_hypo = i;
      }
      total_points += hyp->number_of_points;
    }
  }
  if (nearest_hypo < 0) {  /* should never get to this point */
    cube_log(g, MB_CUBE_LOG_WARNING, "extract_posterior_weighted_node_value: no hypothesis found!");
    answer_no_data(g, a);
  }
  else {
    const mb_cube_hypothesis *hyp = &node->hypotheses[nearest_hypo];
    return_answer_from_hypothesis(g, node, hyp, strength_ratio(g, hyp->number_of_points, total_points), a);
    cube_log(g, MB_CUBE_LOG_DEBUG, "extract_posterior_weighted_node_value: found depth %g", a->depth);
  }
}

/*
 * Return the depth for the 'best' hypothesis in this node.  In this case, the 'best' hypothesis
 * is the hypothesis with the most points.  If there is no hypothesis, this returns no_data_value
 */
double mb_cube_node_return_depth(mb_cube_grid *g, const mb_cube_node *node) {
  mb_cube_answer a;
  mb_cube_node_extract_value(g, node, &a);
  return a.depth;
}

/*
 * Return the uncertainty for the 'best' hypothesis in this node.  In this case, the 'best'
 * hypothesis is the hypothesis with the most points.  If there is no hypothesis, this returns
 * no_data_value
 */
double mb_cube_node_return_uncertainty(mb_cube_grid *g, const mb_cube_node *node) {
  mb_cube_answer a;
  mb_cube_node_extract_value(g, node, &a);
  return a.uncertainty;
}

int mb_cube_node_hypothesis(const mb_cube_node *node, int i, double *current_depth, double *current_variance,
                            int *number_of_points, int *hypothesis_number) {
  if (node == NULL || i < 0 || i >= node->n_hypotheses)
    return MB_CUBE_ERR_ARG;
  const mb_cube_hypothesis *h = &node->hypotheses[i];
  if (current_depth != NULL) *current_depth = h->current_depth;
  if (current_variance != NULL) *current_variance = h->current_variance;
  if (number_of_points != NULL) *number_of_points = h->number_of_points;
  if (hypothesis_number != NULL) *hypothesis_number = h->hypothesis_number;
  return MB_CUBE_OK;
}

/* Print the status of each hypothesis */
void mb_cube_node_dump_hypotheses(const mb_cube_node *node, FILE *fp) {
  if (fp == NULL)
    fp = stdout;
  for (int i = 0; i < node->n_hypotheses; i++) {
    const mb_cube_hypothesis *hyp = &node->hypotheses[i];
    fprintf(fp, "Hypothesis %d - depth=%g, variance=%g, number_of_points=%d\n", hyp->hypothesis_number,
            hyp->current_depth, hyp->current_variance, hyp->number_of_points);
  }
}

/*--------------------------------------------------------------------*/
/* CubeGrid */

/*
 * Main structure for Cube, holds CubeNodes in a grid with metadata.
 *
 * minimum_easting: minimum easting extent of the grid
 * maximum_northing: maximum northing extent of the grid
 * num_columns: number of columns in the grid
 * num_rows: number of rows in the grid
 * resolution_x: the resolution in the x direction of the grid (width of columns)
 * resolution_y: the resolution in the y direction of the grid (height of rows)
 * param: CubeParameters object with the default settings for the grid
 * use_queue: Executes the 'Reordering' step, see CUBE User Manual 3.1.  With this set to False,
 *     this step is skipped.  User Manual states that with multiple hypothesis implementation of
 *     CUBE, Reordering is no longer necessary.
 * logfile: optional path to a logfile
 * debug: if True, will print debug messages to the logger
 */
mb_cube_grid *mb_cube_grid_new(double minimum_easting, double maximum_northing, int num_columns, int num_rows,
                               double resolution_x, double resolution_y, const mb_cube_params *param, bool use_queue,
                               const char *logfile, bool debug) {
  if (param == NULL || num_columns <= 0 || num_rows <= 0 || !(resolution_x > 0.0) || !(resolution_y > 0.0) ||
      !(param->dist_scale > 0.0) || param->median_length < 1)
    return NULL;
  mb_cube_grid *g = (mb_cube_grid *)calloc(1, sizeof(mb_cube_grid));
  if (g == NULL)
    return NULL;
  g->param = *param;
  g->use_queue = use_queue;
  g->minimum_easting = minimum_easting;
  g->maximum_northing = maximum_northing;
  g->num_columns = num_columns;
  g->num_rows = num_rows;
  g->resolution_x = resolution_x;
  g->resolution_y = resolution_y;
  g->debug = debug;
  if (logfile != NULL && logfile[0] != '\0')
    g->logfp = fopen(logfile, "a");
  g->grid = (mb_cube_node *)calloc((size_t)num_columns * (size_t)num_rows, sizeof(mb_cube_node));
  if (g->grid == NULL) {
    if (g->logfp != NULL)
      fclose(g->logfp);
    free(g);
    return NULL;
  }
  const size_t total = (size_t)num_columns * (size_t)num_rows;
  for (size_t k = 0; k < total; k++)
    g->grid[k].nominated = -1;
  return g;
}

void mb_cube_grid_free(mb_cube_grid **grid) {
  if (grid == NULL || *grid == NULL)
    return;
  mb_cube_grid *g = *grid;
  const size_t total = (size_t)g->num_columns * (size_t)g->num_rows;
  for (size_t k = 0; k < total; k++) {
    free(g->grid[k].queue);
    free(g->grid[k].hypotheses);
  }
  free(g->grid);
  if (g->logfp != NULL)
    fclose(g->logfp);
  free(g);
  *grid = NULL;
}

const mb_cube_params *mb_cube_grid_params(const mb_cube_grid *grid) {
  return &grid->param;
}

mb_cube_node *mb_cube_grid_node(mb_cube_grid *g, int row, int col) {
  if (g == NULL || row < 0 || row >= g->num_rows || col < 0 || col >= g->num_columns)
    return NULL;
  return &g->grid[(size_t)row * g->num_columns + col];
}

size_t mb_cube_grid_populated_nodes_count(const mb_cube_grid *g) {
  size_t cnt = 0;
  const size_t total = (size_t)g->num_columns * (size_t)g->num_rows;
  for (size_t k = 0; k < total; k++)
    if (g->grid[k].n_hypotheses || g->grid[k].n_queued)
      cnt++;
  return cnt;
}

size_t mb_cube_grid_empty_nodes_count(const mb_cube_grid *g) {
  return mb_cube_grid_total_nodes_count(g) - mb_cube_grid_populated_nodes_count(g);
}

size_t mb_cube_grid_total_nodes_count(const mb_cube_grid *g) {
  return (size_t)g->num_rows * (size_t)g->num_columns;
}

/*
 * Add an array of point values to the grid
 *
 * depth: new depth values to add to the queue
 * horizontal_uncertainty: new horizontal uncertainty values associated with the points (variances)
 * vertical_uncertainty: new vertical uncertainty values associated with the points (variances)
 * easting: new easting values associated with the points
 * northing: new northing values associated with the points
 */
int mb_cube_grid_insert(mb_cube_grid *g, size_t n, const double *depth, const double *horizontal_uncertainty,
                        const double *vertical_uncertainty, const double *easting, const double *northing) {
  if (g == NULL || (n > 0 && (depth == NULL || horizontal_uncertainty == NULL || vertical_uncertainty == NULL ||
                              easting == NULL || northing == NULL)))
    return MB_CUBE_ERR_ARG;
  const mb_cube_params *p = &g->param;
  const double conf_95_percent = CONF_95_PERCENT;
  const double conf_99_percent = CONF_99_PERCENT;
  cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: Adding %zu points...", n);
  for (size_t i = 0; i < n; i++) {
    cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: x:%g, y:%g, z:%g, thu:%g, tvu:%g", easting[i], northing[i],
             depth[i], horizontal_uncertainty[i], vertical_uncertainty[i]);
    if (!isfinite(depth[i]) || !isfinite(easting[i]) || !isfinite(northing[i]) ||
        !(vertical_uncertainty[i] > 0.0) || !(horizontal_uncertainty[i] >= 0.0))
      continue;
    /* Determine IHO S-44 derived limits on maximum variance */
    /* PORT FIX 3: the squared IHO terms of the initialized parameters, not get_iho_limits()'s raw ones */
    const double max_variance_allowed =
        (p->iho_fixed + p->iho_percent * depth[i] * depth[i]) / (conf_95_percent * conf_95_percent);
    double ratio = max_variance_allowed / vertical_uncertainty[i];
    if (ratio <= 2.0)
      ratio = 2.0;
    const double max_radius = conf_99_percent * sqrt(horizontal_uncertainty[i]);
    double radius = p->dist_scale * pow(ratio - 1.0, p->inv_dist_exponent) - max_radius;
    if (radius < 0.0)
      radius = p->dist_scale;
    else if (radius > max_radius)
      radius = max_radius;
    if (radius < p->dist_scale)
      radius = p->dist_scale;
    cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: dist_scale:%g, ratio:%g, max radius:%g, max_variance:%g",
             p->dist_scale, ratio, max_radius, max_variance_allowed);
    /* determine the coordinates of the effect square.  This is designed to compute the largest region the sounding
       can effect, and hence to make the insertion more efficient by only offering the sounding where it is likely
       to be used */
    const double fmin_x = ((easting[i] - radius) - g->minimum_easting) / g->resolution_x;
    const double fmax_x = ((easting[i] + radius) - g->minimum_easting) / g->resolution_x;
    const double fmin_y = (g->maximum_northing - (northing[i] + radius)) / g->resolution_y;
    const double fmax_y = (g->maximum_northing - (northing[i] - radius)) / g->resolution_y;
    /* reject far outside before the int conversion can overflow */
    if (fmax_x < -1.0 || fmin_x > (double)g->num_columns || fmax_y < -1.0 || fmin_y > (double)g->num_rows)
      continue;
    int min_x = (int)fmin_x;
    int max_x = (int)fmax_x;
    int min_y = (int)fmin_y;
    int max_y = (int)fmax_y;
    /* check that the sounding hits somewhere in the grid */
    /* PORT FIX 5: was min_x >= num_columns - 1, which never offered a sounding to the last column/row alone */
    if (max_x < 0 || min_x >= g->num_columns || max_y < 0 || min_y >= g->num_rows) {
      cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: Sounding out of bounds, (%d,%d) (%d,%d)", min_x, min_y, max_x,
               max_y);
      continue;  /* out of bounds */
    }
    /* clip to the interior of the current grid */
    if (min_x < 0) min_x = 0;
    if (max_x > g->num_columns - 1) max_x = g->num_columns - 1;
    if (min_y < 0) min_y = 0;
    if (max_y > g->num_rows - 1) max_y = g->num_rows - 1;
    cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: clipped row,column limits to use in search, (%d,%d) (%d,%d)", min_x,
             min_y, max_x, max_y);
    for (int y = min_y; y <= max_y; y++) {
      for (int x = min_x; x <= max_x; x++) {
        const double node_x = g->minimum_easting + (x * g->resolution_x) + (g->resolution_x / 2);
        const double node_y = g->maximum_northing - (y * g->resolution_y) - (g->resolution_y / 2);
        const double distance_sq = (node_x - easting[i]) * (node_x - easting[i]) +
                                   (node_y - northing[i]) * (node_y - northing[i]);
        if (distance_sq >= radius * radius) {
          cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: rejecting point as out of distance to node at row/col, (%d, %d)",
                   y, x);
          continue;  /* distance to great, not including this point in this node */
        }
        cube_log(g, MB_CUBE_LOG_DEBUG, "insert_points: adding point to node at row/col, (%d, %d)", y, x);
        const int status = mb_cube_node_add_point(g, &g->grid[(size_t)y * g->num_columns + x], depth[i],
                                                  vertical_uncertainty[i], horizontal_uncertainty[i], distance_sq);
        if (status != MB_CUBE_OK)
          return status;
      }
    }
  }
  return MB_CUBE_OK;
}

/* Flush the queues for each node */
void mb_cube_grid_flush(mb_cube_grid *g) {
  const size_t total = (size_t)g->num_columns * (size_t)g->num_rows;
  for (size_t k = 0; k < total; k++)
    mb_cube_node_flush_queue(g, &g->grid[k]);
}

/* the closest node with exactly one hypothesis in the rings min_context..max_context around
   (row, col), searched in bathycube's order; NULL if there is none */
static const mb_cube_node *closest_context_node(mb_cube_grid *g, int row, int col) {
  const mb_cube_params *p = &g->param;
  for (int offset = p->min_context_nodes; offset <= p->max_context_nodes; offset++) {
    const int target_rows[2] = {row - offset, row + offset};
    for (int r = 0; r < 2; r++) {
      const int target_row = target_rows[r];
      if (0 <= target_row && target_row < g->num_rows) {
        for (int col_offset = -offset; col_offset <= offset; col_offset++) {
          const int target_col = col + col_offset;
          if (target_col < 0 || target_col >= g->num_columns)
            continue;
          const mb_cube_node *chk = &g->grid[(size_t)target_row * g->num_columns + target_col];
          if (chk->n_hypotheses == 1) {
            cube_log(g, MB_CUBE_LOG_DEBUG, "get_grid_values: found closest node during row search at (%d,%d)",
                     target_row, target_col);
            return chk;
          }
        }
      }
    }
    const int target_cols[2] = {col - offset, col + offset};
    for (int c = 0; c < 2; c++) {
      const int target_col = target_cols[c];
      if (0 <= target_col && target_col < g->num_columns) {
        for (int row_offset = -offset + 1; row_offset < offset; row_offset++) {
          const int target_row = row + row_offset;
          if (target_row < 0 || target_row >= g->num_rows)
            continue;
          const mb_cube_node *chk = &g->grid[(size_t)target_row * g->num_columns + target_col];
          if (chk->n_hypotheses == 1) {
            cube_log(g, MB_CUBE_LOG_DEBUG, "get_grid_values: found closest node during column search at (%d,%d)",
                     target_row, target_col);
            return chk;
          }
        }
      }
    }
  }
  return NULL;
}

/*
 * Get the values for each node in the grid.
 *
 * method: method to use in determining the appropriate hypothesis value.  'local' to use the
 *     local spatial context to find the closest node with a single hypothesis and use that
 *     hypothesis depth to find the nearest hypothesis in terms of depth in the current node.
 *     'prior' to use the hypothesis with the most points associated with it.  'posterior' to
 *     combine both prior and local methods to form an approximate Bayesian posterior
 *     distribution.  'predicted' to get the hypothesis closest to the predicted depth associated
 *     with each node.
 */
int mb_cube_grid_get_values(mb_cube_grid *g, mb_cube_method_t method, float *depth, float *uncertainty, float *ratio,
                            float *n_hypotheses, float *n_points, mb_cube_layout_t layout) {
  if (g == NULL || (int)method < 0 || (int)method > MB_CUBE_METHOD_PREDICTED ||
      (layout != MB_CUBE_LAYOUT_ROWS_NORTH && layout != MB_CUBE_LAYOUT_COLS_SOUTH))
    return MB_CUBE_ERR_ARG;
  cube_log(g, MB_CUBE_LOG_DEBUG, "get_grid_values: getting grid values using method %s", mb_cube_method_name(method));
  /* you could use the full extraction to get the hypotheses count, but this would do the extra logic for
     determining the best hypothesis that is unnecessary here; let's shortcut this process to make it faster */
  const bool count_only = (depth == NULL && uncertainty == NULL && ratio == NULL && n_points == NULL);
  for (int row = 0; row < g->num_rows; row++) {
    for (int col = 0; col < g->num_columns; col++) {
      const mb_cube_node *node = &g->grid[(size_t)row * g->num_columns + col];
      mb_cube_answer a = {0};
      if (count_only) {
        a.n_hypotheses = node->n_hypotheses;
      }
      else if (method == MB_CUBE_METHOD_LOCAL || method == MB_CUBE_METHOD_POSTERIOR) {
        if (node->n_hypotheses <= 1)
          mb_cube_node_extract_value(g, node, &a);
        else {
          const mb_cube_node *closest_node = closest_context_node(g, row, col);
          if (closest_node == NULL) {  /* default to the basic node hypothesis selection, couldn't find a good hypothesis in the region */
            cube_log(g, MB_CUBE_LOG_DEBUG,
                     "get_grid_values: default to the basic node hypothesis selection, couldn't find a good hypothesis in the region");
            mb_cube_node_extract_value(g, node, &a);
          }
          else {
            cube_log(g, MB_CUBE_LOG_DEBUG, "get_grid_values: extract value from closest node found");
            mb_cube_answer closest_data;
            mb_cube_node_extract_value(g, closest_node, &closest_data);
            /* PORT FIX 9: the context VARIANCE, not its confidence interval */
            const double cvar = (closest_data.uncertainty / g->param.stddev_to_conf_scale) *
                                (closest_data.uncertainty / g->param.stddev_to_conf_scale);
            if (method == MB_CUBE_METHOD_LOCAL)
              mb_cube_node_extract_closest_value(g, node, closest_data.depth, cvar, &a);
            else
              mb_cube_node_extract_posterior_weighted_value(g, node, closest_data.depth, cvar, &a);
          }
        }
      }
      else if (method == MB_CUBE_METHOD_PRIOR)
        mb_cube_node_extract_value(g, node, &a);
      else  /* MB_CUBE_METHOD_PREDICTED */
        mb_cube_node_extract_closest_value(g, node, node->pred_depth, node->pred_var, &a);

      const size_t k = (layout == MB_CUBE_LAYOUT_ROWS_NORTH)
                           ? (size_t)row * g->num_columns + col
                           : (size_t)col * g->num_rows + (size_t)(g->num_rows - 1 - row);
      if (depth != NULL) depth[k] = (float)a.depth;
      if (uncertainty != NULL) uncertainty[k] = (float)a.uncertainty;
      if (ratio != NULL) ratio[k] = (float)a.ratio;
      if (n_hypotheses != NULL) n_hypotheses[k] = (float)a.n_hypotheses;
      if (n_points != NULL) n_points[k] = (float)a.n_points;
    }
  }
  return MB_CUBE_OK;
}

/*--------------------------------------------------------------------*/
/*
 * Entrance point, run this to run Cube.
 *
 * depth: 1d array of depth values
 * horizontal_uncertainty: 1d array of horiz uncertainty values (variances)
 * vertical_uncertainty: 1d array of vert uncertainty values (variances)
 * easting: 1d array of UTM easting values for the soundings
 * northing: 1d array of UTM northing values for the soundings
 * num_columns, num_rows: grid size
 * minimum_easting: minimum easting value for the grid to determine origin
 * maximum_northing: maximum northing value for the grid to determine origin
 * method: one of 'local', 'posterior', 'prior', 'predicted' (see mb_cube_grid_get_values)
 * iho_order: one of the IHO order categories, i.e. 'special' or 'order1a'
 * grid_resolution_x: grid resolution in easting (column) direction in meters
 * grid_resolution_y: grid resolution in northing (row) direction in meters
 * params: values used to modify cube parameters (NULL = defaults)
 *
 * Returns gridded depth, uncertainty, ratio and hypothesis count values of shape
 * (rows, columns) for the grid
 */
int mb_cube_run_gridding(size_t n, const double *depth, const double *horizontal_uncertainty,
                         const double *vertical_uncertainty, const double *easting, const double *northing,
                         int num_columns, int num_rows, double minimum_easting, double maximum_northing,
                         mb_cube_method_t method, mb_cube_iho_t iho_order, double grid_resolution_x,
                         double grid_resolution_y, const mb_cube_params *params, float *depth_grid,
                         float *uncertainty_grid, float *ratio_grid, float *numhyp_grid) {
  mb_cube_params cp;
  if (params != NULL)
    cp = *params;
  else
    mb_cube_params_default(&cp);
  /* PORT FIX 4: overrides first, initialize last */
  int status = mb_cube_params_initialize(&cp, iho_order, grid_resolution_x, grid_resolution_y);
  if (status != MB_CUBE_OK)
    return status;
  if ((int)method < 0 || (int)method > MB_CUBE_METHOD_PREDICTED) {
    cube_log(NULL, MB_CUBE_LOG_ERROR,
             "run_cube_gridding: method not supported, expected one of 'local', 'posterior', 'prior', 'predicted'");
    return MB_CUBE_ERR_ARG;
  }
  mb_cube_grid *cg = mb_cube_grid_new(minimum_easting, maximum_northing, num_columns, num_rows, grid_resolution_x,
                                      grid_resolution_y, &cp, true, NULL, false);
  if (cg == NULL)
    return MB_CUBE_ERR_MEMORY;
  status = mb_cube_grid_insert(cg, n, depth, horizontal_uncertainty, vertical_uncertainty, easting, northing);
  if (status == MB_CUBE_OK) {
    /* PORT FIX 1: the queues must be flushed before extracting any depth estimates */
    mb_cube_grid_flush(cg);
    status = mb_cube_grid_get_values(cg, method, depth_grid, uncertainty_grid, ratio_grid, numhyp_grid, NULL,
                                     MB_CUBE_LAYOUT_ROWS_NORTH);
  }
  mb_cube_grid_free(&cg);
  return status;
}
/*--------------------------------------------------------------------*/
