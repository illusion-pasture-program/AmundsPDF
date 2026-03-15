/*
 * AmundsPDF - pdf_raster.h
 * Software scanline rasterizer with anti-aliasing.
 *
 * Converts filled bezier paths to anti-aliased pixel coverage.
 * Used for rendering glyph outlines onto a page bitmap without
 * depending on GDI path rendering.
 *
 * Usage:
 *   1. raster_create(width, height)            - allocate context
 *   2. raster_move_to(ctx, x, y)               - start subpath
 *   3. raster_line_to(ctx, x, y)               - line segment
 *   4. raster_curve_to(ctx, cx1,cy1,cx2,cy2,x,y) - cubic bezier
 *   5. raster_close(ctx)                       - close subpath
 *   6. Repeat 2-5 for each subpath
 *   7. raster_finish(ctx)                      - compute coverage
 *   8. raster_blend(ctx, target, ...)          - alpha-blend onto bitmap
 *   9. raster_free(ctx)                        - cleanup
 *
 * All coordinates are in PIXEL SPACE (the caller transforms from font units
 * to device pixels before calling these functions).
 * Sub-pixel coordinates are supported (double precision).
 */
#ifndef PDF_RASTER_H
#define PDF_RASTER_H

#include <stdint.h>

typedef struct RasterCtx RasterCtx;

/* Create a new rasterizer context.
 * width, height: dimensions of the output coverage buffer in pixels.
 * Returns NULL on failure. */
RasterCtx *raster_create(int width, int height);

/* Path construction (coordinates in pixel space, sub-pixel precision) */
void raster_move_to(RasterCtx *ctx, double x, double y);
void raster_line_to(RasterCtx *ctx, double x, double y);
void raster_curve_to(RasterCtx *ctx,
                      double cx1, double cy1,
                      double cx2, double cy2,
                      double x, double y);
void raster_close(RasterCtx *ctx);

/* Finish path and compute coverage buffer.
 * Uses nonzero winding fill rule (standard for fonts).
 * After this call, the coverage buffer is ready for blending. */
void raster_finish(RasterCtx *ctx);

/* Finish path using even-odd fill rule.
 * Same as raster_finish() but uses even-odd instead of nonzero winding. */
void raster_finish_evenodd(RasterCtx *ctx);

/* Get the coverage buffer.
 * Returns a width x height array of uint8_t values (0=transparent, 255=opaque).
 * The buffer is owned by the RasterCtx (don't free it). */
const uint8_t *raster_get_coverage(RasterCtx *ctx);

/* Blend the rasterized coverage onto a target 32-bit BGRA bitmap.
 * target: pointer to pixel data (BGRA format, 4 bytes per pixel)
 * target_stride: bytes per row of the target bitmap
 * target_w, target_h: dimensions of the target bitmap
 * dst_x, dst_y: position in target to place the glyph
 * r, g, b: fill color (0-255)
 *
 * This performs proper alpha-blending using the coverage values. */
void raster_blend(RasterCtx *ctx,
                   uint8_t *target, int target_stride,
                   int target_w, int target_h,
                   int dst_x, int dst_y,
                   int r, int g, int b);

/* Reset the rasterizer for a new path without reallocating.
 * Clears the edge list and coverage buffer. */
void raster_reset(RasterCtx *ctx);

/* Free the rasterizer context and all buffers. */
void raster_free(RasterCtx *ctx);

#endif /* PDF_RASTER_H */
