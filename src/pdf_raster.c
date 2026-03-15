/*
 * AmundsPDF - pdf_raster.c
 * Software scanline rasterizer with anti-aliasing.
 *
 * Implements a scanline-based polygon rasterizer using 8x vertical
 * sub-sampling for anti-aliasing. The pipeline:
 *
 *   1. Path construction: cubic beziers are flattened to line segments
 *      via recursive De Casteljau subdivision.
 *
 *   2. Edge table: line segments that cross at least one sub-scanline
 *      are stored as edges with pre-computed x-intercept and dx/dy.
 *
 *   3. Scanline sweep: for each pixel row, 8 sub-scanlines are processed.
 *      Edge crossings are found, sorted by x, and the nonzero winding
 *      rule determines which pixel spans are "inside" the path.
 *
 *   4. Coverage: for each pixel, the fraction of sub-scanlines that are
 *      inside is the anti-aliased coverage (0..255).
 *
 *   5. Blending: coverage is alpha-blended onto a target BGRA bitmap.
 *
 * This produces smooth, anti-aliased glyph edges comparable to FreeType.
 */

#include "pdf_raster.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Number of vertical sub-scanlines per pixel row for anti-aliasing.
 * 8 gives good quality for text rendering (8 levels of vertical AA). */
#define SUBSAMPLE_Y     8

/* Flatness threshold for bezier subdivision (in pixels squared).
 * Smaller = more accurate curves but more edges. 0.25 px is quarter-pixel
 * accuracy, which is imperceptible. We compare distance-squared to avoid sqrt. */
#define FLATNESS_SQ     (0.25 * 0.25)

/* Maximum recursion depth for bezier flattening.
 * 8 levels = 256 segments per curve, more than enough. */
#define MAX_FLATTEN_DEPTH  8

/* Initial edge array capacity. Grows as needed. */
#define INITIAL_EDGE_CAP   512

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* An edge in the edge table.
 * Represents a line segment that crosses one or more sub-scanlines.
 * Edges are sorted by y_top for efficient sweep-line processing. */
typedef struct {
    double x_at_y_top;  /* x coordinate at y_top (sub-scanline units) */
    double dx_per_sub;  /* change in x per sub-scanline step (inverse slope) */
    int    y_top;       /* topmost sub-scanline (inclusive) */
    int    y_bottom;    /* bottommost sub-scanline (exclusive) */
    int    direction;   /* +1 for downward edge, -1 for upward (winding rule) */
} Edge;

/* Active edge list entry: an edge currently being processed during scanline sweep. */
typedef struct {
    double x_current;   /* current x at the current sub-scanline */
    double dx_per_sub;  /* x increment per sub-scanline */
    int    y_bottom;    /* sub-scanline where this edge ends (exclusive) */
    int    direction;   /* +1 or -1 for winding rule */
} ActiveEdge;

/* The rasterizer context. */
struct RasterCtx {
    int     width;          /* output bitmap width in pixels */
    int     height;         /* output bitmap height in pixels */

    /* Coverage buffer: one byte per pixel, 0=transparent, 255=opaque */
    uint8_t *coverage;

    /* Edge table: all edges from the path */
    Edge    *edges;
    int      edge_count;
    int      edge_capacity;

    /* Current path state */
    double   cur_x, cur_y;          /* current point */
    double   subpath_x, subpath_y;  /* start of current subpath (for close) */
    int      has_point;             /* true if cur_x/cur_y are valid */

    /* Active edge list (reused per scanline sweep) */
    ActiveEdge *ael;
    int          ael_count;
    int          ael_capacity;

    /* Sorted crossing buffer for sub-scanline processing */
    double  *crossings;
    int     *crossing_dirs;     /* winding direction at each crossing */
    int      crossing_count;
    int      crossing_capacity;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge Management
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Add an edge to the edge table. Grows the array if needed. */
static void add_edge(RasterCtx *ctx, double x0, double y0, double x1, double y1)
{
    /* Skip horizontal edges: they don't contribute to scanline crossings */
    if (y0 == y1)
        return;

    /* Determine direction and ensure edge goes top-to-bottom */
    int direction;
    double top_x, top_y, bot_x, bot_y;

    if (y0 < y1) {
        /* Downward edge */
        direction = 1;
        top_x = x0; top_y = y0;
        bot_x = x1; bot_y = y1;
    } else {
        /* Upward edge: swap so top is first */
        direction = -1;
        top_x = x1; top_y = y1;
        bot_x = x0; bot_y = y0;
    }

    /* Convert to sub-scanline coordinates.
     * Sub-scanline index = pixel_row * SUBSAMPLE_Y + sub_offset.
     * We use top-inclusive, bottom-exclusive convention. */
    int sub_top = (int)ceil(top_y * SUBSAMPLE_Y);
    int sub_bot = (int)ceil(bot_y * SUBSAMPLE_Y);

    /* Clamp to the coverage buffer bounds */
    if (sub_top < 0) sub_top = 0;
    if (sub_bot > ctx->height * SUBSAMPLE_Y) sub_bot = ctx->height * SUBSAMPLE_Y;

    /* Skip edges that don't cross any sub-scanline */
    if (sub_top >= sub_bot)
        return;

    /* Compute inverse slope: dx per sub-scanline */
    double dy = bot_y - top_y;
    double dx_total = bot_x - top_x;
    double dx_per_sub = dx_total / (dy * SUBSAMPLE_Y);

    /* Compute x at the first sub-scanline crossing.
     * The first sub-scanline center is at y = sub_top / SUBSAMPLE_Y. */
    double y_first = (double)sub_top / (double)SUBSAMPLE_Y;
    double x_at_top = top_x + (y_first - top_y) * (dx_total / dy);

    /* Grow the edge array if needed */
    if (ctx->edge_count >= ctx->edge_capacity) {
        int new_cap = ctx->edge_capacity * 2;
        if (new_cap < INITIAL_EDGE_CAP) new_cap = INITIAL_EDGE_CAP;
        Edge *new_edges = (Edge *)realloc(ctx->edges, new_cap * sizeof(Edge));
        if (!new_edges) return; /* allocation failure: silently drop edge */
        ctx->edges = new_edges;
        ctx->edge_capacity = new_cap;
    }

    Edge *e = &ctx->edges[ctx->edge_count++];
    e->x_at_y_top  = x_at_top;
    e->dx_per_sub  = dx_per_sub;
    e->y_top       = sub_top;
    e->y_bottom    = sub_bot;
    e->direction   = direction;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Bezier Flattening
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Converts cubic bezier curves to a series of line segments.
 * Uses recursive De Casteljau subdivision with a flatness test:
 * if the control points are close enough to the straight line from
 * start to end, we emit the line; otherwise we split at t=0.5.
 */

/* Compute the perpendicular distance squared from point (px,py) to the
 * line segment from (ax,ay) to (bx,by). */
static double point_line_dist_sq(double px, double py,
                                   double ax, double ay,
                                   double bx, double by)
{
    double dx = bx - ax;
    double dy = by - ay;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        /* Degenerate line segment: just return distance to point a */
        double ex = px - ax;
        double ey = py - ay;
        return ex * ex + ey * ey;
    }

    /* Cross product gives area of parallelogram; divide by base length
     * to get height (perpendicular distance). */
    double cross = (px - ax) * dy - (py - ay) * dx;
    return (cross * cross) / len_sq;
}

static void flatten_cubic(RasterCtx *ctx,
                            double x0, double y0,
                            double cx1, double cy1,
                            double cx2, double cy2,
                            double x3, double y3,
                            int depth)
{
    /* Check flatness: if both control points are close enough to the
     * line from (x0,y0) to (x3,y3), emit a straight line. */
    if (depth >= MAX_FLATTEN_DEPTH) {
        /* Maximum depth reached: emit line regardless of flatness */
        add_edge(ctx, x0, y0, x3, y3);
        return;
    }

    double d1 = point_line_dist_sq(cx1, cy1, x0, y0, x3, y3);
    double d2 = point_line_dist_sq(cx2, cy2, x0, y0, x3, y3);

    if (d1 <= FLATNESS_SQ && d2 <= FLATNESS_SQ) {
        /* Flat enough: emit line segment */
        add_edge(ctx, x0, y0, x3, y3);
        return;
    }

    /* Subdivide at t=0.5 using De Casteljau's algorithm */
    double m01x  = (x0  + cx1) * 0.5;
    double m01y  = (y0  + cy1) * 0.5;
    double m12x  = (cx1 + cx2) * 0.5;
    double m12y  = (cy1 + cy2) * 0.5;
    double m23x  = (cx2 + x3)  * 0.5;
    double m23y  = (cy2 + y3)  * 0.5;

    double m012x = (m01x + m12x) * 0.5;
    double m012y = (m01y + m12y) * 0.5;
    double m123x = (m12x + m23x) * 0.5;
    double m123y = (m12y + m23y) * 0.5;

    double mx    = (m012x + m123x) * 0.5;
    double my    = (m012y + m123y) * 0.5;

    /* Recurse on the two half-curves */
    flatten_cubic(ctx, x0, y0, m01x, m01y, m012x, m012y, mx, my, depth + 1);
    flatten_cubic(ctx, mx, my, m123x, m123y, m23x, m23y, x3, y3, depth + 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API: Context Management
 * ═══════════════════════════════════════════════════════════════════════════ */

RasterCtx *raster_create(int width, int height)
{
    if (width <= 0 || height <= 0)
        return NULL;

    /* Guard against unreasonable sizes.
     * Raised to 16384 to support page-wide path rendering, not just glyphs. */
    if (width > 16384 || height > 16384)
        return NULL;

    RasterCtx *ctx = (RasterCtx *)calloc(1, sizeof(RasterCtx));
    if (!ctx) return NULL;

    ctx->width  = width;
    ctx->height = height;

    /* Allocate coverage buffer (zero-initialized = transparent) */
    ctx->coverage = (uint8_t *)calloc((size_t)width * height, 1);
    if (!ctx->coverage) {
        free(ctx);
        return NULL;
    }

    /* Allocate initial edge array */
    ctx->edge_capacity = INITIAL_EDGE_CAP;
    ctx->edges = (Edge *)malloc(ctx->edge_capacity * sizeof(Edge));
    if (!ctx->edges) {
        free(ctx->coverage);
        free(ctx);
        return NULL;
    }
    ctx->edge_count = 0;

    /* Allocate initial active edge list */
    ctx->ael_capacity = 64;
    ctx->ael = (ActiveEdge *)malloc(ctx->ael_capacity * sizeof(ActiveEdge));
    if (!ctx->ael) {
        free(ctx->edges);
        free(ctx->coverage);
        free(ctx);
        return NULL;
    }
    ctx->ael_count = 0;

    /* Allocate initial crossing buffer */
    ctx->crossing_capacity = 64;
    ctx->crossings = (double *)malloc(ctx->crossing_capacity * sizeof(double));
    ctx->crossing_dirs = (int *)malloc(ctx->crossing_capacity * sizeof(int));
    if (!ctx->crossings || !ctx->crossing_dirs) {
        free(ctx->crossing_dirs);
        free(ctx->crossings);
        free(ctx->ael);
        free(ctx->edges);
        free(ctx->coverage);
        free(ctx);
        return NULL;
    }
    ctx->crossing_count = 0;

    ctx->has_point = 0;

    return ctx;
}

void raster_reset(RasterCtx *ctx)
{
    if (!ctx) return;
    ctx->edge_count = 0;
    ctx->ael_count = 0;
    ctx->crossing_count = 0;
    ctx->has_point = 0;
    ctx->cur_x = 0; ctx->cur_y = 0;
    ctx->subpath_x = 0; ctx->subpath_y = 0;
    memset(ctx->coverage, 0, (size_t)ctx->width * ctx->height);
}

void raster_free(RasterCtx *ctx)
{
    if (!ctx) return;
    free(ctx->coverage);
    free(ctx->edges);
    free(ctx->ael);
    free(ctx->crossings);
    free(ctx->crossing_dirs);
    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API: Path Construction
 * ═══════════════════════════════════════════════════════════════════════════ */

void raster_move_to(RasterCtx *ctx, double x, double y)
{
    if (!ctx) return;
    ctx->cur_x = x;
    ctx->cur_y = y;
    ctx->subpath_x = x;
    ctx->subpath_y = y;
    ctx->has_point = 1;
}

void raster_line_to(RasterCtx *ctx, double x, double y)
{
    if (!ctx || !ctx->has_point) return;
    add_edge(ctx, ctx->cur_x, ctx->cur_y, x, y);
    ctx->cur_x = x;
    ctx->cur_y = y;
}

void raster_curve_to(RasterCtx *ctx,
                      double cx1, double cy1,
                      double cx2, double cy2,
                      double x, double y)
{
    if (!ctx || !ctx->has_point) return;
    flatten_cubic(ctx, ctx->cur_x, ctx->cur_y,
                  cx1, cy1, cx2, cy2, x, y, 0);
    ctx->cur_x = x;
    ctx->cur_y = y;
}

void raster_close(RasterCtx *ctx)
{
    if (!ctx || !ctx->has_point) return;

    /* Close the subpath by drawing a line back to the start */
    if (ctx->cur_x != ctx->subpath_x || ctx->cur_y != ctx->subpath_y) {
        add_edge(ctx, ctx->cur_x, ctx->cur_y, ctx->subpath_x, ctx->subpath_y);
    }
    ctx->cur_x = ctx->subpath_x;
    ctx->cur_y = ctx->subpath_y;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge Sorting
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Comparison function for sorting edges by y_top, then by x_at_y_top. */
static int edge_compare(const void *a, const void *b)
{
    const Edge *ea = (const Edge *)a;
    const Edge *eb = (const Edge *)b;
    if (ea->y_top != eb->y_top)
        return ea->y_top - eb->y_top;
    if (ea->x_at_y_top < eb->x_at_y_top) return -1;
    if (ea->x_at_y_top > eb->x_at_y_top) return  1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Active Edge List Management
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Ensure the AEL has room for one more entry. */
static int ael_ensure_capacity(RasterCtx *ctx)
{
    if (ctx->ael_count < ctx->ael_capacity)
        return 1;
    int new_cap = ctx->ael_capacity * 2;
    ActiveEdge *new_ael = (ActiveEdge *)realloc(ctx->ael, new_cap * sizeof(ActiveEdge));
    if (!new_ael) return 0;
    ctx->ael = new_ael;
    ctx->ael_capacity = new_cap;
    return 1;
}

/* Remove expired edges from the AEL (those where y_bottom <= current sub-scanline). */
static void ael_remove_expired(RasterCtx *ctx, int sub_y)
{
    int dst = 0;
    for (int i = 0; i < ctx->ael_count; i++) {
        if (ctx->ael[i].y_bottom > sub_y) {
            if (dst != i)
                ctx->ael[dst] = ctx->ael[i];
            dst++;
        }
    }
    ctx->ael_count = dst;
}

/* Insert a new edge into the AEL. */
static void ael_insert(RasterCtx *ctx, const Edge *e, int sub_y)
{
    if (!ael_ensure_capacity(ctx)) return;

    ActiveEdge *ae = &ctx->ael[ctx->ael_count++];
    ae->x_current  = e->x_at_y_top + (double)(sub_y - e->y_top) * e->dx_per_sub;
    ae->dx_per_sub = e->dx_per_sub;
    ae->y_bottom   = e->y_bottom;
    ae->direction  = e->direction;
}

/* Advance all active edges by one sub-scanline step. */
static void ael_advance(RasterCtx *ctx)
{
    for (int i = 0; i < ctx->ael_count; i++) {
        ctx->ael[i].x_current += ctx->ael[i].dx_per_sub;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scanline Rasterization
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * For each pixel row, we process SUBSAMPLE_Y sub-scanlines.
 * For each sub-scanline:
 *   1. Collect x-crossings from all active edges
 *   2. Sort crossings by x
 *   3. Walk left-to-right applying nonzero winding rule
 *   4. For each pixel column, count how many sub-scanlines are "inside"
 *
 * The coverage for a pixel = (count_inside * 255 + SUBSAMPLE_Y/2) / SUBSAMPLE_Y.
 */

/* Ensure the crossing buffer has room for one more entry. */
static int crossing_ensure_capacity(RasterCtx *ctx)
{
    if (ctx->crossing_count < ctx->crossing_capacity)
        return 1;
    int new_cap = ctx->crossing_capacity * 2;
    double *new_cx = (double *)realloc(ctx->crossings, new_cap * sizeof(double));
    int    *new_cd = (int *)realloc(ctx->crossing_dirs, new_cap * sizeof(int));
    if (!new_cx || !new_cd) {
        /* If one succeeded but not the other, keep the old pointers valid */
        if (new_cx) ctx->crossings = new_cx;
        if (new_cd) ctx->crossing_dirs = new_cd;
        ctx->crossing_capacity = new_cap; /* at least the realloc'd one is bigger */
        return 0;
    }
    ctx->crossings = new_cx;
    ctx->crossing_dirs = new_cd;
    ctx->crossing_capacity = new_cap;
    return 1;
}

/* Sort crossings by x (simple insertion sort; typical glyph has few crossings). */
static void sort_crossings(double *xs, int *dirs, int n)
{
    for (int i = 1; i < n; i++) {
        double kx = xs[i];
        int kd = dirs[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > kx) {
            xs[j + 1] = xs[j];
            dirs[j + 1] = dirs[j];
            j--;
        }
        xs[j + 1] = kx;
        dirs[j + 1] = kd;
    }
}

/* Fill rule constants for raster_finish_internal */
#define FILL_RULE_NONZERO  0
#define FILL_RULE_EVENODD  1

/* Internal finish function that supports both fill rules.
 * fill_rule: FILL_RULE_NONZERO or FILL_RULE_EVENODD */
static void raster_finish_internal(RasterCtx *ctx, int fill_rule)
{
    if (!ctx) return;
    if (ctx->edge_count == 0) return;

    int width  = ctx->width;
    int height = ctx->height;
    int total_sub = height * SUBSAMPLE_Y;

    /* Sort all edges by y_top for efficient sweep-line processing */
    qsort(ctx->edges, ctx->edge_count, sizeof(Edge), edge_compare);

    /* Clear coverage buffer */
    memset(ctx->coverage, 0, (size_t)width * height);

    /* We accumulate sub-scanline hit counts per pixel row.
     * For each pixel row, we process SUBSAMPLE_Y sub-scanlines and count
     * how many times each pixel column is "inside" the path. */
    int *row_counts = (int *)calloc(width, sizeof(int));
    if (!row_counts) return;

    ctx->ael_count = 0;
    int edge_idx = 0; /* index into sorted edge array: next edge to activate */

    for (int sub_y = 0; sub_y < total_sub; sub_y++) {
        int pixel_row = sub_y / SUBSAMPLE_Y;
        int sub_offset = sub_y % SUBSAMPLE_Y;

        /* At the start of each pixel row, clear the per-pixel counters */
        if (sub_offset == 0) {
            memset(row_counts, 0, width * sizeof(int));
        }

        /* Remove edges that have ended */
        ael_remove_expired(ctx, sub_y);

        /* Add new edges that start at this sub-scanline */
        while (edge_idx < ctx->edge_count && ctx->edges[edge_idx].y_top <= sub_y) {
            if (ctx->edges[edge_idx].y_bottom > sub_y) {
                ael_insert(ctx, &ctx->edges[edge_idx], sub_y);
            }
            edge_idx++;
        }

        /* Collect crossings from active edges */
        ctx->crossing_count = 0;
        for (int i = 0; i < ctx->ael_count; i++) {
            if (!crossing_ensure_capacity(ctx)) break;
            ctx->crossings[ctx->crossing_count] = ctx->ael[i].x_current;
            ctx->crossing_dirs[ctx->crossing_count] = ctx->ael[i].direction;
            ctx->crossing_count++;
        }

        /* Sort crossings by x */
        if (ctx->crossing_count > 1) {
            sort_crossings(ctx->crossings, ctx->crossing_dirs, ctx->crossing_count);
        }

        /* Walk crossings left-to-right applying the chosen fill rule.
         * Nonzero winding: inside when winding != 0
         * Even-odd: inside when (crossing_count_so_far % 2) != 0 */
        int winding = 0;
        for (int c = 0; c < ctx->crossing_count; c++) {
            winding += ctx->crossing_dirs[c];

            /* Determine if we're "inside" after this crossing */
            int inside;
            if (fill_rule == FILL_RULE_EVENODD) {
                /* Even-odd: count total crossings seen so far.
                 * Inside when an odd number of crossings have been passed. */
                inside = ((c + 1) % 2 != 0);
            } else {
                /* Nonzero winding: inside when winding count is non-zero */
                inside = (winding != 0);
            }

            /* Between crossing c and c+1, if inside, those pixels are filled. */
            if (inside && c + 1 < ctx->crossing_count) {
                double x_start = ctx->crossings[c];
                double x_end   = ctx->crossings[c + 1];

                /* Clamp to pixel buffer bounds */
                if (x_start < 0.0) x_start = 0.0;
                if (x_end > (double)width) x_end = (double)width;
                if (x_start >= x_end) continue;

                /* Convert to pixel columns.
                 * Pixel column px covers x in [px, px+1).
                 * Partial coverage at the left and right edges of the span
                 * could be handled for horizontal AA, but for now we use
                 * a simple threshold: a pixel is "hit" if any part of it
                 * is inside the span. */
                int px_start = (int)floor(x_start);
                int px_end   = (int)ceil(x_end);
                if (px_start < 0) px_start = 0;
                if (px_end > width) px_end = width;

                for (int px = px_start; px < px_end; px++) {
                    /* Compute fractional coverage for this pixel on this sub-scanline.
                     * The pixel covers x in [px, px+1).
                     * The span covers x in [x_start, x_end).
                     * The overlap is the intersection. We scale to 256ths. */
                    double left  = (px > x_start) ? (double)px : x_start;
                    double right = ((px + 1) < x_end) ? (double)(px + 1) : x_end;
                    double frac  = right - left; /* 0.0 to 1.0 */

                    /* Accumulate fractional coverage scaled to 256.
                     * Each sub-scanline contributes up to 256/SUBSAMPLE_Y = 32. */
                    row_counts[px] += (int)(frac * 256.0 + 0.5);
                }
            }
        }

        /* Advance all active edges for the next sub-scanline */
        ael_advance(ctx);

        /* At the end of each pixel row (last sub-scanline), write coverage */
        if (sub_offset == SUBSAMPLE_Y - 1) {
            uint8_t *row = ctx->coverage + (size_t)pixel_row * width;
            for (int px = 0; px < width; px++) {
                /* row_counts[px] is the sum of fractional coverage * 256
                 * across SUBSAMPLE_Y sub-scanlines. Divide by SUBSAMPLE_Y
                 * to get the final coverage in 0..255.
                 *
                 * Max possible = SUBSAMPLE_Y * 256 = 2048.
                 * Divided by SUBSAMPLE_Y = 256, clamped to 255. */
                int val = (row_counts[px] + SUBSAMPLE_Y / 2) / SUBSAMPLE_Y;
                if (val > 255) val = 255;
                if (val < 0) val = 0;
                row[px] = (uint8_t)val;
            }
        }
    }

    free(row_counts);
}

void raster_finish(RasterCtx *ctx)
{
    raster_finish_internal(ctx, FILL_RULE_NONZERO);
}

void raster_finish_evenodd(RasterCtx *ctx)
{
    raster_finish_internal(ctx, FILL_RULE_EVENODD);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API: Coverage Access
 * ═══════════════════════════════════════════════════════════════════════════ */

const uint8_t *raster_get_coverage(RasterCtx *ctx)
{
    if (!ctx) return NULL;
    return ctx->coverage;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API: Alpha Blending
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Blends the rasterized coverage onto a target BGRA bitmap.
 * Uses standard "source over" compositing:
 *   dst = src * alpha + dst * (1 - alpha)
 *
 * The +127 in the division rounds to nearest instead of truncating,
 * which avoids the slight darkening bias of simple integer division.
 */

void raster_blend(RasterCtx *ctx,
                   uint8_t *target, int target_stride,
                   int target_w, int target_h,
                   int dst_x, int dst_y,
                   int r, int g, int b)
{
    if (!ctx || !target) return;

    const uint8_t *coverage = ctx->coverage;
    int cw = ctx->width;
    int ch = ctx->height;

    for (int cy = 0; cy < ch; cy++) {
        int ty = dst_y + cy;
        if (ty < 0 || ty >= target_h) continue;

        const uint8_t *cov_row = coverage + cy * cw;
        uint8_t *target_row = target + ty * target_stride;

        for (int cx = 0; cx < cw; cx++) {
            int tx = dst_x + cx;
            if (tx < 0 || tx >= target_w) continue;

            int alpha = cov_row[cx];
            if (alpha == 0) continue;

            uint8_t *pixel = target_row + tx * 4;

            if (alpha == 255) {
                /* Fully opaque: direct write */
                pixel[0] = (uint8_t)b;   /* B */
                pixel[1] = (uint8_t)g;   /* G */
                pixel[2] = (uint8_t)r;   /* R */
                pixel[3] = 255;          /* A */
            } else {
                /* Alpha blend: dst = src * alpha + dst * (1 - alpha)
                 * Using the integer approximation:
                 *   result = (src * alpha + dst * (255 - alpha) + 127) / 255
                 * The +127 provides proper rounding. */
                int inv = 255 - alpha;
                pixel[0] = (uint8_t)((b * alpha + pixel[0] * inv + 127) / 255);
                pixel[1] = (uint8_t)((g * alpha + pixel[1] * inv + 127) / 255);
                pixel[2] = (uint8_t)((r * alpha + pixel[2] * inv + 127) / 255);
                pixel[3] = 255;
            }
        }
    }
}
