/*
 * AmundsPDF - pdf_glyph_tt.c
 * TrueType font parser: extracts glyph outlines from TrueType (FontFile2) data.
 *
 * Parses the TrueType sfnt table directory and extracts glyph outlines from
 * the 'glyf' table using 'loca' offsets. Supports both simple and compound
 * (composite) glyphs. Advance widths come from the 'hmtx' table.
 *
 * The cmap table is also parsed for simple font encoding lookups, but for
 * Type0/CID fonts the caller provides GIDs directly via parsed_font_get_glyph_by_gid().
 */

#include "pdf_glyph.h"
#include <string.h>
#include <stdlib.h>

/* ─── Big-endian read helpers ─── */

static inline uint16_t tt_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline int16_t tt_s16(const uint8_t *p) {
    return (int16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t tt_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ─── Table lookup ─── */

typedef struct {
    const uint8_t *data;
    uint32_t length;
} TtTable;

static bool tt_find_table(const uint8_t *data, size_t len, const char *tag, TtTable *out)
{
    if (len < 12) return false;

    uint16_t num_tables = tt_u16(data + 4);
    if (len < (size_t)(12 + num_tables * 16)) return false;

    for (int i = 0; i < num_tables; i++) {
        const uint8_t *entry = data + 12 + i * 16;
        if (memcmp(entry, tag, 4) == 0) {
            uint32_t offset = tt_u32(entry + 8);
            uint32_t length = tt_u32(entry + 12);
            if ((size_t)(offset + length) > len) return false;
            out->data = data + offset;
            out->length = length;
            return true;
        }
    }
    return false;
}

/* ─── cmap table parsing ─── */

/* Find a Unicode cmap subtable (format 4 preferred) and build encoding[256].
 * This maps character codes 0-255 to GIDs for simple font usage. */
/* Windows-1252 → Unicode mapping for the 0x80-0x9F range.
 * All other codes (0x00-0x7F, 0xA0-0xFF) map directly to the same Unicode value. */
static const uint16_t g_win1252_to_unicode[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  /* 80-87 */
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,  /* 88-8F */
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  /* 90-97 */
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,  /* 98-9F */
};

static uint16_t win1252_to_unicode(int code)
{
    if (code >= 0x80 && code <= 0x9F)
        return g_win1252_to_unicode[code - 0x80];
    return (uint16_t)code;
}

/* Look up a Unicode code point in a cmap format 4 subtable.
 * Returns the glyph index, or 0 (notdef) if not found. */
static int tt_cmap4_lookup(const uint8_t *subtable, const uint8_t *cmap_end,
                            uint16_t unicode_cp)
{
    if (subtable + 14 > cmap_end) return 0;

    uint16_t seg_count_x2 = tt_u16(subtable + 6);
    uint16_t seg_count = seg_count_x2 / 2;

    const uint8_t *end_codes = subtable + 14;
    const uint8_t *start_codes = end_codes + seg_count_x2 + 2;
    const uint8_t *id_deltas = start_codes + seg_count_x2;
    const uint8_t *id_range_offsets = id_deltas + seg_count_x2;

    for (int seg = 0; seg < seg_count; seg++) {
        uint16_t end_code = tt_u16(end_codes + seg * 2);
        uint16_t start_code = tt_u16(start_codes + seg * 2);

        if (unicode_cp < start_code || unicode_cp > end_code) continue;

        uint16_t range_offset = tt_u16(id_range_offsets + seg * 2);
        int16_t delta = tt_s16(id_deltas + seg * 2);

        int gid;
        if (range_offset == 0) {
            gid = (unicode_cp + delta) & 0xFFFF;
        } else {
            const uint8_t *glyph_addr = id_range_offsets + seg * 2 + range_offset +
                                         (unicode_cp - start_code) * 2;
            if (glyph_addr + 1 >= cmap_end) return 0;
            gid = tt_u16(glyph_addr);
            if (gid != 0)
                gid = (gid + delta) & 0xFFFF;
        }
        return gid;
    }
    return 0;
}

static void tt_parse_cmap(const uint8_t *font_data, size_t font_len, ParsedFont *font)
{
    TtTable cmap_tbl;
    if (!tt_find_table(font_data, font_len, "cmap", &cmap_tbl))
        return;

    font->cmap_table = cmap_tbl.data;
    font->cmap_len = cmap_tbl.length;

    if (cmap_tbl.length < 4) return;

    uint16_t num_subtables = tt_u16(cmap_tbl.data + 2);
    const uint8_t *best_subtable = NULL;
    int best_priority = -1;

    for (int i = 0; i < num_subtables; i++) {
        const uint8_t *rec = cmap_tbl.data + 4 + i * 8;
        if ((size_t)(4 + (i + 1) * 8) > cmap_tbl.length) break;

        uint16_t platform = tt_u16(rec);
        uint16_t encoding = tt_u16(rec + 2);
        uint32_t offset = tt_u32(rec + 4);

        int priority = -1;
        /* Prefer: (3,1) Windows Unicode BMP, then (0,*) Unicode */
        if (platform == 3 && encoding == 1) priority = 10;
        else if (platform == 0) priority = 5;
        else if (platform == 1 && encoding == 0) priority = 1; /* Mac Roman */

        if (priority > best_priority && offset < cmap_tbl.length) {
            best_subtable = cmap_tbl.data + offset;
            best_priority = priority;
        }
    }

    if (!best_subtable) return;

    uint16_t format = tt_u16(best_subtable);

    if (format == 4) {
        /* Format 4: Segment mapping to delta values */
        if ((size_t)(best_subtable - cmap_tbl.data + 14) > cmap_tbl.length) return;

        uint16_t seg_count_x2 = tt_u16(best_subtable + 6);
        uint16_t seg_count = seg_count_x2 / 2;

        const uint8_t *end_codes = best_subtable + 14;
        const uint8_t *start_codes = end_codes + seg_count_x2 + 2; /* +2 for reservedPad */
        const uint8_t *id_deltas = start_codes + seg_count_x2;
        const uint8_t *id_range_offsets = id_deltas + seg_count_x2;

        /* Map char codes 0-255 */
        for (int code = 0; code < 256; code++) {
            for (int seg = 0; seg < seg_count; seg++) {
                uint16_t end_code = tt_u16(end_codes + seg * 2);
                uint16_t start_code = tt_u16(start_codes + seg * 2);

                if (code < start_code || code > end_code) continue;

                uint16_t range_offset = tt_u16(id_range_offsets + seg * 2);
                int16_t delta = tt_s16(id_deltas + seg * 2);

                int gid;
                if (range_offset == 0) {
                    gid = (code + delta) & 0xFFFF;
                } else {
                    const uint8_t *glyph_addr = id_range_offsets + seg * 2 + range_offset +
                                                 (code - start_code) * 2;
                    if (glyph_addr + 1 >= cmap_tbl.data + cmap_tbl.length) break;
                    gid = tt_u16(glyph_addr);
                    if (gid != 0)
                        gid = (gid + delta) & 0xFFFF;
                }

                if (gid >= 0 && gid < font->num_glyphs)
                    font->encoding[code] = gid;
                break;
            }
        }
    } else if (format == 0) {
        /* Format 0: Byte encoding table */
        if ((size_t)(best_subtable - cmap_tbl.data + 6 + 256) > cmap_tbl.length) return;
        for (int i = 0; i < 256; i++) {
            font->encoding[i] = best_subtable[6 + i];
        }
    }
}

/* ─── Simple glyph parsing ─── */

/* Maximum recursion depth for compound glyphs */
#define TT_MAX_COMPOUND_DEPTH 10

/* Parse a simple TrueType glyph (numberOfContours >= 0) */
static bool tt_parse_simple_glyph(const uint8_t *glyph_data, size_t glyph_len,
                                    int num_contours, GlyphOutline *outline,
                                    double offset_x, double offset_y,
                                    double scale_xx, double scale_xy,
                                    double scale_yx, double scale_yy)
{
    if (num_contours <= 0) return true; /* empty glyph */
    if (glyph_len < (size_t)(10 + num_contours * 2)) return false;

    const uint8_t *p = glyph_data + 10; /* skip header (numContours, xMin, yMin, xMax, yMax) */

    /* Read endPtsOfContours */
    uint16_t *end_pts = (uint16_t *)calloc(num_contours, sizeof(uint16_t));
    if (!end_pts) return false;

    for (int i = 0; i < num_contours; i++) {
        end_pts[i] = tt_u16(p);
        p += 2;
    }

    int num_points = end_pts[num_contours - 1] + 1;
    if (num_points <= 0 || num_points > 16384) {
        free(end_pts);
        return false;
    }

    /* Skip instructions */
    if ((size_t)(p - glyph_data + 2) > glyph_len) { free(end_pts); return false; }
    uint16_t instr_len = tt_u16(p);
    p += 2 + instr_len;

    if ((size_t)(p - glyph_data) > glyph_len) { free(end_pts); return false; }

    /* Parse flags */
    uint8_t *flags = (uint8_t *)calloc(num_points, sizeof(uint8_t));
    if (!flags) { free(end_pts); return false; }

    int pts_read = 0;
    while (pts_read < num_points) {
        if ((size_t)(p - glyph_data) >= glyph_len) { free(flags); free(end_pts); return false; }
        uint8_t flag = *p++;
        flags[pts_read++] = flag;

        if (flag & 0x08) { /* repeat flag */
            if ((size_t)(p - glyph_data) >= glyph_len) { free(flags); free(end_pts); return false; }
            uint8_t repeat_count = *p++;
            for (int r = 0; r < repeat_count && pts_read < num_points; r++) {
                flags[pts_read++] = flag;
            }
        }
    }

    /* Parse x coordinates */
    int16_t *x_coords = (int16_t *)calloc(num_points, sizeof(int16_t));
    int16_t *y_coords = (int16_t *)calloc(num_points, sizeof(int16_t));
    if (!x_coords || !y_coords) {
        free(x_coords); free(y_coords); free(flags); free(end_pts);
        return false;
    }

    int16_t x_val = 0;
    for (int i = 0; i < num_points; i++) {
        if (flags[i] & 0x02) { /* x is 1 byte */
            if ((size_t)(p - glyph_data) >= glyph_len) { goto fail; }
            int16_t dx = *p++;
            if (!(flags[i] & 0x10)) dx = -dx; /* sign */
            x_val += dx;
        } else {
            if (flags[i] & 0x10) {
                /* x is same (delta = 0) */
            } else {
                if ((size_t)(p - glyph_data + 2) > glyph_len) { goto fail; }
                x_val += tt_s16(p);
                p += 2;
            }
        }
        x_coords[i] = x_val;
    }

    /* Parse y coordinates */
    int16_t y_val = 0;
    for (int i = 0; i < num_points; i++) {
        if (flags[i] & 0x04) { /* y is 1 byte */
            if ((size_t)(p - glyph_data) >= glyph_len) { goto fail; }
            int16_t dy = *p++;
            if (!(flags[i] & 0x20)) dy = -dy;
            y_val += dy;
        } else {
            if (flags[i] & 0x20) {
                /* y is same */
            } else {
                if ((size_t)(p - glyph_data + 2) > glyph_len) { goto fail; }
                y_val += tt_s16(p);
                p += 2;
            }
        }
        y_coords[i] = y_val;
    }

    /* Convert TrueType contours to outline commands.
     * TrueType uses quadratic bezier curves with on-curve and off-curve points.
     * We need to convert quadratic beziers to cubic beziers for our outline format.
     *
     * Rules:
     * - On-curve points are endpoints of lines/curves
     * - Off-curve points are quadratic bezier control points
     * - Two consecutive off-curve points have an implied on-curve point between them
     */
    int contour_start = 0;
    for (int c = 0; c < num_contours; c++) {
        int contour_end = end_pts[c];
        int contour_len = contour_end - contour_start + 1;
        if (contour_len < 2) {
            contour_start = contour_end + 1;
            continue;
        }

        /* Find the first on-curve point to start the contour */
        int first_on = -1;
        for (int j = 0; j < contour_len; j++) {
            int idx = contour_start + j;
            if (flags[idx] & 0x01) { /* on-curve */
                first_on = j;
                break;
            }
        }

        double start_x, start_y;
        int start_offset;

        if (first_on >= 0) {
            int idx = contour_start + first_on;
            start_x = (double)x_coords[idx] * scale_xx + (double)y_coords[idx] * scale_yx + offset_x;
            start_y = (double)x_coords[idx] * scale_xy + (double)y_coords[idx] * scale_yy + offset_y;
            start_offset = first_on;
        } else {
            /* All off-curve: implied on-curve between first and last */
            int idx0 = contour_start;
            int idx1 = contour_start + contour_len - 1;
            double px0 = (double)x_coords[idx0] * scale_xx + (double)y_coords[idx0] * scale_yx + offset_x;
            double py0 = (double)x_coords[idx0] * scale_xy + (double)y_coords[idx0] * scale_yy + offset_y;
            double px1 = (double)x_coords[idx1] * scale_xx + (double)y_coords[idx1] * scale_yx + offset_x;
            double py1 = (double)x_coords[idx1] * scale_xy + (double)y_coords[idx1] * scale_yy + offset_y;
            start_x = (px0 + px1) * 0.5;
            start_y = (py0 + py1) * 0.5;
            start_offset = 0;
        }

        /* MoveTo the start */
        GlyphCmd cmd;
        cmd.type = GLYPH_MOVETO;
        cmd.x = start_x;
        cmd.y = start_y;
        cmd.cx1 = cmd.cy1 = cmd.cx2 = cmd.cy2 = 0;
        glyph_outline_add(outline, cmd);

        /* Walk through points */
        int i = 0;
        int total = contour_len;
        for (int step = 0; step < total; step++) {
            int curr_local = (start_offset + 1 + step) % contour_len;
            int curr_idx = contour_start + curr_local;

            double cx = (double)x_coords[curr_idx] * scale_xx + (double)y_coords[curr_idx] * scale_yx + offset_x;
            double cy = (double)x_coords[curr_idx] * scale_xy + (double)y_coords[curr_idx] * scale_yy + offset_y;

            if (flags[curr_idx] & 0x01) {
                /* On-curve: line to */
                cmd.type = GLYPH_LINETO;
                cmd.x = cx;
                cmd.y = cy;
                glyph_outline_add(outline, cmd);
            } else {
                /* Off-curve: quadratic bezier control point */
                /* Look at next point */
                int next_local = (curr_local + 1) % contour_len;
                int next_idx = contour_start + next_local;

                double nx = (double)x_coords[next_idx] * scale_xx + (double)y_coords[next_idx] * scale_yx + offset_x;
                double ny = (double)x_coords[next_idx] * scale_xy + (double)y_coords[next_idx] * scale_yy + offset_y;

                double end_x, end_y;

                if (flags[next_idx] & 0x01) {
                    /* Next is on-curve: end of quadratic bezier */
                    end_x = nx;
                    end_y = ny;
                    step++; /* consume next point */
                } else {
                    /* Next is also off-curve: implied on-curve midpoint */
                    end_x = (cx + nx) * 0.5;
                    end_y = (cy + ny) * 0.5;
                    /* Don't consume next — it will be processed next iteration */
                }

                /* Convert quadratic bezier to cubic bezier.
                 * For quadratic P0, P1(control), P2(end):
                 * Cubic CP1 = P0 + 2/3 * (P1 - P0)
                 * Cubic CP2 = P2 + 2/3 * (P1 - P2)
                 *
                 * We need the previous point as P0 (the current pen position).
                 * Since we're building the outline sequentially, the last
                 * command's endpoint is our P0.
                 */
                double p0x = start_x, p0y = start_y;
                if (outline->count > 0) {
                    GlyphCmd *last = &outline->cmds[outline->count - 1];
                    p0x = last->x;
                    p0y = last->y;
                }

                cmd.type = GLYPH_CURVETO;
                cmd.cx1 = p0x + (2.0 / 3.0) * (cx - p0x);
                cmd.cy1 = p0y + (2.0 / 3.0) * (cy - p0y);
                cmd.cx2 = end_x + (2.0 / 3.0) * (cx - end_x);
                cmd.cy2 = end_y + (2.0 / 3.0) * (cy - end_y);
                cmd.x = end_x;
                cmd.y = end_y;
                glyph_outline_add(outline, cmd);
            }
        }

        /* Close the contour */
        cmd.type = GLYPH_CLOSE;
        cmd.x = cmd.y = 0;
        glyph_outline_add(outline, cmd);

        contour_start = contour_end + 1;
    }

    free(x_coords);
    free(y_coords);
    free(flags);
    free(end_pts);
    return true;

fail:
    free(x_coords);
    free(y_coords);
    free(flags);
    free(end_pts);
    return false;
}

/* ─── Compound glyph parsing ─── */

/* Forward declaration for recursive compound glyph parsing */
static bool tt_parse_glyph_recursive(ParsedFont *font, int gid, GlyphOutline *outline,
                                      double offset_x, double offset_y,
                                      double scale_xx, double scale_xy,
                                      double scale_yx, double scale_yy,
                                      int depth);

static bool tt_parse_compound_glyph(ParsedFont *font, const uint8_t *glyph_data, size_t glyph_len,
                                      GlyphOutline *outline,
                                      double parent_offset_x, double parent_offset_y,
                                      double parent_scale_xx, double parent_scale_xy,
                                      double parent_scale_yx, double parent_scale_yy,
                                      int depth)
{
    if (depth >= TT_MAX_COMPOUND_DEPTH) return false;

    const uint8_t *p = glyph_data + 10; /* skip glyph header */
    size_t remaining = glyph_len - 10;

    uint16_t flags;
    do {
        if (remaining < 4) return false;

        flags = tt_u16(p);
        uint16_t glyph_index = tt_u16(p + 2);
        p += 4;
        remaining -= 4;

        double arg1 = 0, arg2 = 0;

        if (flags & 0x0001) { /* ARG_1_AND_2_ARE_WORDS */
            if (remaining < 4) return false;
            if (flags & 0x0002) { /* ARGS_ARE_XY_VALUES */
                arg1 = (double)tt_s16(p);
                arg2 = (double)tt_s16(p + 2);
            } else {
                arg1 = (double)tt_u16(p);
                arg2 = (double)tt_u16(p + 2);
            }
            p += 4;
            remaining -= 4;
        } else {
            if (remaining < 2) return false;
            if (flags & 0x0002) {
                arg1 = (double)(int8_t)p[0];
                arg2 = (double)(int8_t)p[1];
            } else {
                arg1 = (double)p[0];
                arg2 = (double)p[1];
            }
            p += 2;
            remaining -= 2;
        }

        /* Component transform */
        double c_xx = 1.0, c_xy = 0.0, c_yx = 0.0, c_yy = 1.0;
        double c_dx = 0.0, c_dy = 0.0;

        if (flags & 0x0002) { /* ARGS_ARE_XY_VALUES */
            c_dx = arg1;
            c_dy = arg2;
        }

        if (flags & 0x0008) { /* WE_HAVE_A_SCALE */
            if (remaining < 2) return false;
            c_xx = c_yy = (double)tt_s16(p) / 16384.0;
            p += 2;
            remaining -= 2;
        } else if (flags & 0x0040) { /* WE_HAVE_AN_X_AND_Y_SCALE */
            if (remaining < 4) return false;
            c_xx = (double)tt_s16(p) / 16384.0;
            c_yy = (double)tt_s16(p + 2) / 16384.0;
            p += 4;
            remaining -= 4;
        } else if (flags & 0x0080) { /* WE_HAVE_A_TWO_BY_TWO */
            if (remaining < 8) return false;
            c_xx = (double)tt_s16(p) / 16384.0;
            c_xy = (double)tt_s16(p + 2) / 16384.0;
            c_yx = (double)tt_s16(p + 4) / 16384.0;
            c_yy = (double)tt_s16(p + 6) / 16384.0;
            p += 8;
            remaining -= 8;
        }

        /* Compose transforms: component = parent * component */
        double final_xx = parent_scale_xx * c_xx + parent_scale_yx * c_xy;
        double final_xy = parent_scale_xy * c_xx + parent_scale_yy * c_xy;
        double final_yx = parent_scale_xx * c_yx + parent_scale_yx * c_yy;
        double final_yy = parent_scale_xy * c_yx + parent_scale_yy * c_yy;
        double final_dx = parent_scale_xx * c_dx + parent_scale_yx * c_dy + parent_offset_x;
        double final_dy = parent_scale_xy * c_dx + parent_scale_yy * c_dy + parent_offset_y;

        /* Recursively parse the component glyph */
        tt_parse_glyph_recursive(font, glyph_index, outline,
                                  final_dx, final_dy,
                                  final_xx, final_xy, final_yx, final_yy,
                                  depth + 1);

    } while (flags & 0x0020); /* MORE_COMPONENTS */

    return true;
}

/* ─── Get glyph data by GID ─── */

static const uint8_t *tt_get_glyph_data(ParsedFont *font, int gid, size_t *out_len)
{
    if (!font->glyf_table || !font->loca_table)
        return NULL;

    uint32_t offset, next_offset;

    if (font->loca_is_long) {
        size_t loca_entry = (size_t)gid * 4;
        if (loca_entry + 8 > font->loca_len) return NULL;
        offset = tt_u32(font->loca_table + loca_entry);
        next_offset = tt_u32(font->loca_table + loca_entry + 4);
    } else {
        size_t loca_entry = (size_t)gid * 2;
        if (loca_entry + 4 > font->loca_len) return NULL;
        offset = (uint32_t)tt_u16(font->loca_table + loca_entry) * 2;
        next_offset = (uint32_t)tt_u16(font->loca_table + loca_entry + 2) * 2;
    }

    if (offset == next_offset) {
        /* Empty glyph (e.g., space) */
        *out_len = 0;
        return NULL;
    }

    if (offset >= font->glyf_len || next_offset > font->glyf_len || next_offset <= offset)
        return NULL;

    *out_len = next_offset - offset;
    return font->glyf_table + offset;
}

/* Recursive glyph parser: handles both simple and compound glyphs */
static bool tt_parse_glyph_recursive(ParsedFont *font, int gid, GlyphOutline *outline,
                                      double offset_x, double offset_y,
                                      double scale_xx, double scale_xy,
                                      double scale_yx, double scale_yy,
                                      int depth)
{
    if (depth >= TT_MAX_COMPOUND_DEPTH) return false;
    if (gid < 0 || gid >= font->num_glyphs) return false;

    size_t glyph_len;
    const uint8_t *glyph_data = tt_get_glyph_data(font, gid, &glyph_len);

    if (!glyph_data || glyph_len < 10)
        return true; /* empty glyph — not an error */

    int16_t num_contours = tt_s16(glyph_data);

    if (num_contours >= 0) {
        /* Simple glyph */
        return tt_parse_simple_glyph(glyph_data, glyph_len, num_contours, outline,
                                      offset_x, offset_y,
                                      scale_xx, scale_xy, scale_yx, scale_yy);
    } else {
        /* Compound glyph (num_contours == -1) */
        return tt_parse_compound_glyph(font, glyph_data, glyph_len, outline,
                                        offset_x, offset_y,
                                        scale_xx, scale_xy, scale_yx, scale_yy,
                                        depth);
    }
}

/* ─── Public: get glyph by GID for TrueType ─── */

bool tt_get_glyph_by_gid(ParsedFont *font, int gid, GlyphOutline *outline)
{
    if (!font || !outline) return false;
    if (!font->glyf_table || !font->loca_table) return false;
    if (gid < 0 || gid >= font->num_glyphs) return false;

    glyph_outline_init(outline);

    /* Get advance width from hmtx */
    double advance = 0;
    if (font->hmtx_table) {
        if (gid < font->num_h_metrics) {
            advance = (double)tt_u16(font->hmtx_table + gid * 4);
        } else if (font->num_h_metrics > 0) {
            /* Monospaced trailing glyphs: use last hMetric */
            advance = (double)tt_u16(font->hmtx_table + (font->num_h_metrics - 1) * 4);
        }
    }
    outline->advance_width = advance;

    /* Parse the glyph outline */
    tt_parse_glyph_recursive(font, gid, outline,
                              0.0, 0.0,  /* no offset */
                              1.0, 0.0, 0.0, 1.0, /* identity transform */
                              0);

    /* Cache width */
    if (font->glyph_widths && gid < font->num_glyphs)
        font->glyph_widths[gid] = advance;

    return true;
}

/* ─── Remap encoding for system fonts using Windows-1252 ─── */

void tt_remap_encoding_win1252(ParsedFont *font)
{
    if (!font || !font->cmap_table || font->cmap_len < 4) return;

    /* Find the best cmap subtable (same logic as tt_parse_cmap) */
    uint16_t num_subtables = tt_u16(font->cmap_table + 2);
    const uint8_t *best_subtable = NULL;
    int best_priority = -1;

    for (int i = 0; i < num_subtables; i++) {
        const uint8_t *rec = font->cmap_table + 4 + i * 8;
        if ((size_t)(4 + (i + 1) * 8) > font->cmap_len) break;

        uint16_t platform = tt_u16(rec);
        uint16_t encoding = tt_u16(rec + 2);
        uint32_t offset = tt_u32(rec + 4);

        int priority = -1;
        if (platform == 3 && encoding == 1) priority = 10;
        else if (platform == 0) priority = 5;

        if (priority > best_priority && offset < font->cmap_len) {
            best_subtable = font->cmap_table + offset;
            best_priority = priority;
        }
    }

    if (!best_subtable) return;
    uint16_t format = tt_u16(best_subtable);
    if (format != 4) return; /* only handle format 4 for now */

    const uint8_t *cmap_end = font->cmap_table + font->cmap_len;

    /* Rebuild encoding[256] using Windows-1252 → Unicode → cmap lookup */
    for (int code = 0; code < 256; code++) {
        uint16_t unicode_cp = win1252_to_unicode(code);
        int gid = tt_cmap4_lookup(best_subtable, cmap_end, unicode_cp);
        if (gid > 0 && gid < font->num_glyphs)
            font->encoding[code] = gid;
    }
}

/* ─── Public: parse TrueType font ─── */

ParsedFont *parsed_font_from_truetype(const uint8_t *data, size_t len)
{
    if (!data || len < 12) return NULL;

    /* Verify sfnt signature */
    uint32_t sig = tt_u32(data);
    if (sig != 0x00010000 && sig != 0x74727565) /* 0x00010000 or 'true' */
        return NULL;

    /* Find required tables */
    TtTable head_tbl, maxp_tbl, hhea_tbl, hmtx_tbl, glyf_tbl, loca_tbl;

    if (!tt_find_table(data, len, "head", &head_tbl) || head_tbl.length < 54)
        return NULL;
    if (!tt_find_table(data, len, "maxp", &maxp_tbl) || maxp_tbl.length < 6)
        return NULL;
    if (!tt_find_table(data, len, "glyf", &glyf_tbl))
        return NULL;
    if (!tt_find_table(data, len, "loca", &loca_tbl))
        return NULL;

    bool has_hhea = tt_find_table(data, len, "hhea", &hhea_tbl) && hhea_tbl.length >= 36;
    bool has_hmtx = tt_find_table(data, len, "hmtx", &hmtx_tbl);

    /* Parse head */
    int units_per_em = tt_u16(head_tbl.data + 18);
    int16_t index_to_loc_format = tt_s16(head_tbl.data + 50);

    /* Parse maxp */
    int num_glyphs = tt_u16(maxp_tbl.data + 4);

    /* Parse hhea */
    int num_h_metrics = 0;
    int ascent = 0, descent = 0;
    if (has_hhea) {
        ascent = tt_s16(hhea_tbl.data + 4);
        descent = tt_s16(hhea_tbl.data + 6);
        num_h_metrics = tt_u16(hhea_tbl.data + 34);
    }

    /* Validate */
    if (units_per_em == 0 || num_glyphs == 0)
        return NULL;

    /* Allocate ParsedFont */
    ParsedFont *font = (ParsedFont *)calloc(1, sizeof(ParsedFont));
    if (!font) return NULL;

    font->units_per_em = units_per_em;
    font->ascent = ascent;
    font->descent = descent;
    font->num_glyphs = num_glyphs;
    font->is_cff = false;
    font->is_type1 = false;
    font->is_bold = false;
    font->is_italic = false;

    /* Store table pointers */
    font->glyf_table = glyf_tbl.data;
    font->glyf_len = glyf_tbl.length;
    font->loca_table = loca_tbl.data;
    font->loca_len = loca_tbl.length;
    font->loca_is_long = (index_to_loc_format != 0);

    if (has_hmtx) {
        font->hmtx_table = hmtx_tbl.data;
        font->num_h_metrics = num_h_metrics;
    }

    /* Keep a reference to the raw data (we don't own it — the PDF stream does) */
    font->data = (uint8_t *)data;
    font->data_len = len;
    font->owns_data = false;

    /* Allocate glyph width cache */
    font->glyph_widths = (double *)calloc(num_glyphs, sizeof(double));

    /* Parse cmap for simple font encoding */
    memset(font->encoding, 0, sizeof(font->encoding));
    tt_parse_cmap(data, len, font);

    /* Try to get font name from name table */
    TtTable name_tbl;
    if (tt_find_table(data, len, "name", &name_tbl) && name_tbl.length >= 6) {
        uint16_t name_count = tt_u16(name_tbl.data + 2);
        uint16_t string_offset = tt_u16(name_tbl.data + 4);

        for (int i = 0; i < name_count; i++) {
            const uint8_t *rec = name_tbl.data + 6 + i * 12;
            if ((size_t)(6 + (i + 1) * 12) > name_tbl.length) break;

            uint16_t name_id = tt_u16(rec + 6);
            if (name_id != 1) continue; /* Font Family name */

            uint16_t str_length = tt_u16(rec + 8);
            uint16_t str_offset = tt_u16(rec + 10);
            uint16_t platform = tt_u16(rec);

            const uint8_t *str_data = name_tbl.data + string_offset + str_offset;
            if ((size_t)(string_offset + str_offset + str_length) > name_tbl.length) continue;

            if (platform == 1) {
                /* Mac Roman: direct ASCII-ish copy */
                int copy_len = str_length < 255 ? str_length : 255;
                memcpy(font->family_name, str_data, copy_len);
                font->family_name[copy_len] = '\0';
                break;
            } else if (platform == 3 || platform == 0) {
                /* Windows/Unicode: big-endian UTF-16, extract ASCII portion */
                int copy_len = 0;
                for (int j = 0; j + 1 < str_length && copy_len < 255; j += 2) {
                    uint16_t ch = tt_u16(str_data + j);
                    if (ch < 128) font->family_name[copy_len++] = (char)ch;
                }
                font->family_name[copy_len] = '\0';
                break;
            }
        }
    }

    return font;
}
