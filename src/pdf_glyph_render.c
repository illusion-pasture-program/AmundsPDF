/*
 * AmundsPDF - pdf_glyph_render.c
 * Glyph outline rendering pipeline.
 *
 * Extracts embedded font data from PDF font descriptors (/FontFile3 for CFF,
 * /FontFile2 for TrueType), parses glyph outlines, and renders them as
 * anti-aliased filled GDI paths using 4x supersampled rendering.
 *
 * This replaces the TextOutW-based rendering for fonts that have embedded
 * outline data, providing pixel-perfect glyph shapes matching the original
 * PDF typography.
 */

#include "pdf_glyph_render.h"
#include "pdf_glyph.h"
#include "pdf_parser.h"
#include "pdf_raster.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Font Cache
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Maps PDF font resource names + object numbers to ParsedFont objects.
 * Deduplicates by font stream object number so the same embedded font
 * referenced from multiple pages is only parsed once.
 */

#define MAX_CACHED_FONTS 64

typedef struct {
    char resource_name[PDF_MAX_NAME_LEN]; /* e.g., "F1", "F2" */
    int font_obj_num;                      /* PDF object number for dedup */
    ParsedFont *parsed;                    /* parsed font data (may be NULL if parse failed) */
    bool tried;                            /* true if we already attempted extraction */
} CachedFont;

static CachedFont g_font_cache[MAX_CACHED_FONTS];
static int g_font_cache_count = 0;

/* ─── Cache Lookup ─── */

/* Find a cached font by stream object number. Returns index or -1. */
static int cache_find_by_obj(int obj_num)
{
    if (obj_num < 0) return -1;
    for (int i = 0; i < g_font_cache_count; i++) {
        if (g_font_cache[i].font_obj_num == obj_num)
            return i;
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Font Stream Extraction
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Navigates the PDF font dictionary tree to find and decode embedded font
 * data. Handles both simple fonts (/FontDescriptor directly on the font
 * dict) and composite/Type0 fonts (through /DescendantFonts).
 */

/* Search a font descriptor dict for /FontFile, /FontFile2, or /FontFile3 streams.
 * Returns the stream PdfObj and the object number, or NULL/-1 on failure.
 * Sets *out_is_cff if the stream is Type1C (bare CFF data).
 * Sets *out_is_type1 if the stream is Type1 (PFB/PFA from /FontFile). */
static PdfObj *find_font_stream_in_descriptor(PdfDocument *doc, PdfDict *descriptor,
                                               int *out_obj_num, bool *out_is_cff,
                                               bool *out_is_type1)
{
    *out_obj_num = -1;
    *out_is_cff = false;
    *out_is_type1 = false;

    /* Check FontFile (Type1), FontFile2 (TrueType), FontFile3 (CFF/OpenType) */
    static const char *keys[] = { "FontFile2", "FontFile3", "FontFile", NULL };
    for (int k = 0; keys[k]; k++) {
        PdfObj *ff = pdf_dict_get(descriptor, keys[k]);
        if (!ff) continue;

        /* Capture object number before resolving */
        int obj_num = -1;
        if (ff->type == PDF_OBJ_REF)
            obj_num = ff->ref.obj_num;

        PdfObj *resolved = pdf_resolve(doc, ff);
        if (!resolved || resolved->type != PDF_OBJ_STREAM) continue;

        /* For FontFile3, check Subtype to distinguish CFF vs OpenType */
        if (strcmp(keys[k], "FontFile3") == 0) {
            const char *subtype = pdf_dict_get_name(resolved->stream->dict, "Subtype");
            if (subtype && (strcmp(subtype, "Type1C") == 0 ||
                            strcmp(subtype, "CIDFontType0C") == 0)) {
                *out_is_cff = true;
            }
            /* OpenType subtype: could contain CFF or TrueType outlines inside
             * an OTF wrapper. For now treat it the same as CFF. */
            if (subtype && strcmp(subtype, "OpenType") == 0) {
                /* OpenType font - could be CFF-based OTF. We'll try CFF first,
                 * and if that fails the caller falls back. For now mark as CFF. */
                *out_is_cff = true;
            }
        } else if (strcmp(keys[k], "FontFile") == 0) {
            /* FontFile = Type1 (PFB/PFA format) */
            *out_is_type1 = true;
        } else {
            /* FontFile2 = TrueType */
            *out_is_cff = false;
        }

        *out_obj_num = obj_num;
        return resolved;
    }

    return NULL;
}

/* Navigate from a font resource name to the embedded font stream.
 * Handles both simple fonts and Type0 composite fonts.
 * Returns the stream object, sets *out_obj_num, *out_is_cff, and *out_is_type1. */
static PdfObj *find_font_stream(PdfDocument *doc, PdfDict *resources,
                                 const char *font_res_name,
                                 int *out_obj_num, bool *out_is_cff,
                                 bool *out_is_type1)
{
    *out_obj_num = -1;
    *out_is_cff = false;
    *out_is_type1 = false;

    /* Get /Font dictionary from resources */
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return NULL;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return NULL;

    /* Get the specific font dict (e.g., /F1) */
    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
    if (!font_obj) return NULL;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return NULL;

    /* Try direct FontDescriptor (simple fonts) */
    PdfObj *descriptor = pdf_dict_get(font_obj->dict, "FontDescriptor");
    if (descriptor) {
        descriptor = pdf_resolve(doc, descriptor);
        if (descriptor && descriptor->type == PDF_OBJ_DICT) {
            PdfObj *stream = find_font_stream_in_descriptor(
                doc, descriptor->dict, out_obj_num, out_is_cff, out_is_type1);
            if (stream) return stream;
        }
    }

    /* For Type0/composite fonts, look through DescendantFonts */
    PdfObj *descendants = pdf_dict_get(font_obj->dict, "DescendantFonts");
    if (descendants) {
        descendants = pdf_resolve(doc, descendants);
        if (descendants && descendants->type == PDF_OBJ_ARRAY) {
            int n = pdf_array_len(descendants->array);
            for (int i = 0; i < n; i++) {
                PdfObj *cid_font = pdf_array_get(descendants->array, i);
                cid_font = pdf_resolve(doc, cid_font);
                if (!cid_font || cid_font->type != PDF_OBJ_DICT) continue;

                PdfObj *cid_desc = pdf_dict_get(cid_font->dict, "FontDescriptor");
                if (!cid_desc) continue;
                cid_desc = pdf_resolve(doc, cid_desc);
                if (!cid_desc || cid_desc->type != PDF_OBJ_DICT) continue;

                PdfObj *stream = find_font_stream_in_descriptor(
                    doc, cid_desc->dict, out_obj_num, out_is_cff, out_is_type1);
                if (stream) return stream;
            }
        }
    }

    return NULL;
}

/* Extract and parse an embedded font from a PDF font dictionary.
 * Returns a ParsedFont* on success, NULL if no embedded font or parse failure.
 * The result is cached for subsequent calls. */
static ParsedFont *extract_embedded_font(PdfDocument *doc, PdfDict *resources,
                                          const char *font_res_name)
{
    /* Find the font stream */
    int obj_num = -1;
    bool is_cff = false;
    bool is_type1 = false;
    PdfObj *stream_obj = find_font_stream(doc, resources, font_res_name,
                                           &obj_num, &is_cff, &is_type1);
    if (!stream_obj || obj_num < 0)
        return NULL;

    /* Check cache by object number */
    int cache_idx = cache_find_by_obj(obj_num);
    if (cache_idx >= 0) {
        if (g_font_cache[cache_idx].tried)
            return g_font_cache[cache_idx].parsed; /* may be NULL if parse failed */
    }

    /* Allocate cache slot */
    if (cache_idx < 0) {
        if (g_font_cache_count >= MAX_CACHED_FONTS)
            return NULL;
        cache_idx = g_font_cache_count++;
        memset(&g_font_cache[cache_idx], 0, sizeof(CachedFont));
        g_font_cache[cache_idx].font_obj_num = obj_num;
        strncpy(g_font_cache[cache_idx].resource_name, font_res_name,
                PDF_MAX_NAME_LEN - 1);
    }

    CachedFont *entry = &g_font_cache[cache_idx];
    entry->tried = true;

    /* Decode the stream (applies FlateDecode etc.) */
    PdfStream *stream = stream_obj->stream;
    if (!pdf_decode_stream(doc, stream))
        return NULL;

    const uint8_t *font_data = stream->decoded_data;
    size_t font_data_len = stream->decoded_length;
    if (!font_data || font_data_len < 4)
        return NULL;

    /* Parse the font data */
    ParsedFont *parsed = NULL;
    if (is_type1) {
        parsed = parsed_font_from_type1(font_data, font_data_len);
    } else if (is_cff) {
        parsed = parsed_font_from_cff(font_data, font_data_len);
    } else {
        parsed = parsed_font_from_truetype(font_data, font_data_len);
    }

    entry->parsed = parsed;

    if (parsed) {
        fprintf(stderr, "[GLYPH] Parsed embedded font '%s' obj=%d cff=%d glyphs=%d upem=%d\n",
                font_res_name, obj_num, is_cff, parsed->num_glyphs, parsed->units_per_em);
    }

    return parsed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Glyph Path Rendering
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Renders a single glyph outline as a filled GDI path. The glyph coordinates
 * (in font design units) are transformed through the combined text + CTM
 * matrix to produce device coordinates.
 *
 * Anti-aliasing: We use 4x supersampled rendering for smooth edges.
 * 1. Compute the glyph bounding box in device pixels
 * 2. Create a 4x oversampled DIB section
 * 3. Render the glyph path at 4x resolution
 * 4. Box-filter downsample to 1x with alpha from coverage
 * 5. Alpha-blend onto the target DC
 */

/* Transform a point from font design units to device coordinates.
 * Applies: font_units -> text_space (via font_size/units_per_em) -> device (via combined matrix) */
static void glyph_to_device(double gx, double gy,
                              double font_size, int units_per_em,
                              PdfMatrix combined,
                              double *out_dx, double *out_dy)
{
    double scale = font_size / (double)units_per_em;
    double tx = gx * scale;
    double ty = gy * scale;
    pdf_transform_point(combined, tx, ty, out_dx, out_dy);
}

/* Compute the bounding box of a glyph outline in device pixels.
 * Returns false if the outline is empty. */
static bool glyph_device_bounds(GlyphOutline *outline,
                                 double font_size, int units_per_em,
                                 PdfMatrix combined,
                                 double *min_x, double *min_y,
                                 double *max_x, double *max_y)
{
    if (!outline || outline->count == 0)
        return false;

    *min_x = 1e30;  *min_y = 1e30;
    *max_x = -1e30; *max_y = -1e30;

    for (int i = 0; i < outline->count; i++) {
        GlyphCmd *cmd = &outline->cmds[i];
        if (cmd->type == GLYPH_CLOSE) continue;

        double dx, dy;
        glyph_to_device(cmd->x, cmd->y, font_size, units_per_em, combined, &dx, &dy);
        if (dx < *min_x) *min_x = dx;
        if (dy < *min_y) *min_y = dy;
        if (dx > *max_x) *max_x = dx;
        if (dy > *max_y) *max_y = dy;

        /* For curves, also include control points in the bounds */
        if (cmd->type == GLYPH_CURVETO) {
            glyph_to_device(cmd->cx1, cmd->cy1, font_size, units_per_em, combined, &dx, &dy);
            if (dx < *min_x) *min_x = dx;
            if (dy < *min_y) *min_y = dy;
            if (dx > *max_x) *max_x = dx;
            if (dy > *max_y) *max_y = dy;

            glyph_to_device(cmd->cx2, cmd->cy2, font_size, units_per_em, combined, &dx, &dy);
            if (dx < *min_x) *min_x = dx;
            if (dy < *min_y) *min_y = dy;
            if (dx > *max_x) *max_x = dx;
            if (dy > *max_y) *max_y = dy;
        }
    }

    return (*max_x > *min_x) && (*max_y > *min_y);
}

/* ─── Software Rasterizer Rendering ─── */

/* Render a single glyph with the software scanline rasterizer.
 *
 * Strategy:
 * 1. Compute glyph bounding box in device pixels
 * 2. Create a RasterCtx for that bbox size
 * 3. Feed transformed outline points to the rasterizer
 * 4. Compute coverage via raster_finish()
 * 5. Get the page bitmap pixel pointer and alpha-blend directly
 */
static void render_glyph_outline(HDC hdc, GlyphOutline *outline, ParsedFont *font,
                                  double font_size, PdfMatrix combined, COLORREF color)
{
    if (!outline || outline->count == 0) return;
    if (font->units_per_em <= 0) return;

    /* Compute device-space bounding box */
    double bmin_x, bmin_y, bmax_x, bmax_y;
    if (!glyph_device_bounds(outline, font_size, font->units_per_em, combined,
                              &bmin_x, &bmin_y, &bmax_x, &bmax_y))
        return;

    /* Add 2px padding for anti-aliasing edges */
    bmin_x = floor(bmin_x) - 2.0;
    bmin_y = floor(bmin_y) - 2.0;
    bmax_x = ceil(bmax_x)  + 2.0;
    bmax_y = ceil(bmax_y)  + 2.0;

    int glyph_w = (int)(bmax_x - bmin_x);
    int glyph_h = (int)(bmax_y - bmin_y);

    /* Sanity check: skip glyphs that are too large or degenerate */
    if (glyph_w <= 0 || glyph_h <= 0) return;
    if (glyph_w > 2000 || glyph_h > 2000) return;

    /* Create rasterizer context for the glyph bbox */
    RasterCtx *rctx = raster_create(glyph_w, glyph_h);
    if (!rctx) return;

    /* Feed the glyph outline to the rasterizer.
     * Transform coordinates from font units to device pixels,
     * then offset so the bbox origin is at (0,0). */
    for (int i = 0; i < outline->count; i++) {
        GlyphCmd *cmd = &outline->cmds[i];
        double dx, dy;

        switch (cmd->type) {
        case GLYPH_MOVETO:
            glyph_to_device(cmd->x, cmd->y, font_size, font->units_per_em,
                            combined, &dx, &dy);
            raster_move_to(rctx, dx - bmin_x, dy - bmin_y);
            break;

        case GLYPH_LINETO:
            glyph_to_device(cmd->x, cmd->y, font_size, font->units_per_em,
                            combined, &dx, &dy);
            raster_line_to(rctx, dx - bmin_x, dy - bmin_y);
            break;

        case GLYPH_CURVETO: {
            double cx1, cy1, cx2, cy2, ex, ey;
            glyph_to_device(cmd->cx1, cmd->cy1, font_size, font->units_per_em,
                            combined, &cx1, &cy1);
            glyph_to_device(cmd->cx2, cmd->cy2, font_size, font->units_per_em,
                            combined, &cx2, &cy2);
            glyph_to_device(cmd->x, cmd->y, font_size, font->units_per_em,
                            combined, &ex, &ey);
            raster_curve_to(rctx,
                            cx1 - bmin_x, cy1 - bmin_y,
                            cx2 - bmin_x, cy2 - bmin_y,
                            ex  - bmin_x, ey  - bmin_y);
            break;
        }

        case GLYPH_CLOSE:
            raster_close(rctx);
            break;
        }
    }

    /* Compute coverage */
    raster_finish(rctx);

    /* Get the page bitmap pixels.
     * The target HDC has a DIB section selected into it. We use GetCurrentObject
     * to get the bitmap handle, then GetObject to get the DIBSECTION with the
     * pixel pointer. */
    HBITMAP hbm = (HBITMAP)GetCurrentObject(hdc, OBJ_BITMAP);
    if (!hbm) {
        raster_free(rctx);
        return;
    }

    DIBSECTION ds;
    if (GetObject(hbm, sizeof(DIBSECTION), &ds) == 0 || !ds.dsBm.bmBits) {
        /* Couldn't get direct pixel access (maybe not a DIB section).
         * Fall back to the BitBlt approach: read pixels, blend, write back. */
        BITMAPINFO out_bmi;
        memset(&out_bmi, 0, sizeof(out_bmi));
        out_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        out_bmi.bmiHeader.biWidth       = glyph_w;
        out_bmi.bmiHeader.biHeight      = -glyph_h;
        out_bmi.bmiHeader.biPlanes      = 1;
        out_bmi.bmiHeader.biBitCount    = 32;
        out_bmi.bmiHeader.biCompression = BI_RGB;

        uint8_t *out_bits = NULL;
        HDC out_dc = CreateCompatibleDC(hdc);
        HBITMAP out_bmp = CreateDIBSection(out_dc, &out_bmi, DIB_RGB_COLORS,
                                            (void **)&out_bits, NULL, 0);
        if (out_bmp && out_bits) {
            HBITMAP old_bmp = (HBITMAP)SelectObject(out_dc, out_bmp);
            int dest_x = (int)bmin_x;
            int dest_y = (int)bmin_y;
            BitBlt(out_dc, 0, 0, glyph_w, glyph_h, hdc, dest_x, dest_y, SRCCOPY);
            GdiFlush();

            int stride = glyph_w * 4;
            raster_blend(rctx, out_bits, stride, glyph_w, glyph_h, 0, 0,
                         GetRValue(color), GetGValue(color), GetBValue(color));

            BitBlt(hdc, dest_x, dest_y, glyph_w, glyph_h, out_dc, 0, 0, SRCCOPY);

            SelectObject(out_dc, old_bmp);
            DeleteObject(out_bmp);
        }
        DeleteDC(out_dc);
        raster_free(rctx);
        return;
    }

    /* Direct pixel access to the page bitmap.
     * The DIB section is top-down (negative biHeight), so row 0 is at the top. */
    uint8_t *page_bits = (uint8_t *)ds.dsBm.bmBits;
    int page_w = ds.dsBm.bmWidth;
    int page_h = abs(ds.dsBm.bmHeight);
    int page_stride = ds.dsBm.bmWidthBytes;

    /* Ensure GDI has finished any pending operations before we write directly */
    GdiFlush();

    int dest_x = (int)bmin_x;
    int dest_y = (int)bmin_y;

    /* Blend the rasterized coverage directly onto the page bitmap */
    raster_blend(rctx, page_bits, page_stride, page_w, page_h,
                 dest_x, dest_y,
                 GetRValue(color), GetGValue(color), GetBValue(color));

    raster_free(rctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WinAnsiEncoding Table
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Maps character codes 0-255 to Adobe glyph names for the WinAnsiEncoding,
 * which is the most common PDF encoding.
 */

static const char *winansi_glyph_names[256] = {
    /* 0x00-0x0F */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0x10-0x1F */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0x20-0x2F */
    "space", "exclam", "quotedbl", "numbersign",
    "dollar", "percent", "ampersand", "quotesingle",
    "parenleft", "parenright", "asterisk", "plus",
    "comma", "hyphen", "period", "slash",
    /* 0x30-0x3F */
    "zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "colon", "semicolon",
    "less", "equal", "greater", "question",
    /* 0x40-0x4F */
    "at", "A", "B", "C", "D", "E", "F", "G",
    "H", "I", "J", "K", "L", "M", "N", "O",
    /* 0x50-0x5F */
    "P", "Q", "R", "S", "T", "U", "V", "W",
    "X", "Y", "Z", "bracketleft",
    "backslash", "bracketright", "asciicircum", "underscore",
    /* 0x60-0x6F */
    "grave", "a", "b", "c", "d", "e", "f", "g",
    "h", "i", "j", "k", "l", "m", "n", "o",
    /* 0x70-0x7F */
    "p", "q", "r", "s", "t", "u", "v", "w",
    "x", "y", "z", "braceleft",
    "bar", "braceright", "asciitilde", NULL,
    /* 0x80-0x8F */
    "Euro", "bullet"/*0x81=undef,use bullet*/, "quotesinglbase", "florin",
    "quotedblbase", "ellipsis", "dagger", "daggerdbl",
    "circumflex", "perthousand", "Scaron", "guilsinglleft",
    "OE", NULL, "Zcaron", NULL,
    /* 0x90-0x9F */
    NULL, "quoteleft", "quoteright", "quotedblleft",
    "quotedblright", "bullet", "endash", "emdash",
    "tilde", "trademark", "scaron", "guilsinglright",
    "oe", NULL, "zcaron", "Ydieresis",
    /* 0xA0-0xAF */
    NULL/*NBSP*/, "exclamdown", "cent", "sterling",
    "currency", "yen", "brokenbar", "section",
    "dieresis", "copyright", "ordfeminine", "guillemotleft",
    "logicalnot", NULL/*SHY*/, "registered", "macron",
    /* 0xB0-0xBF */
    "degree", "plusminus", "twosuperior", "threesuperior",
    "acute", "mu", "paragraph", "periodcentered",
    "cedilla", "onesuperior", "ordmasculine", "guillemotright",
    "onequarter", "onehalf", "threequarters", "questiondown",
    /* 0xC0-0xCF */
    "Agrave", "Aacute", "Acircumflex", "Atilde",
    "Adieresis", "Aring", "AE", "Ccedilla",
    "Egrave", "Eacute", "Ecircumflex", "Edieresis",
    "Igrave", "Iacute", "Icircumflex", "Idieresis",
    /* 0xD0-0xDF */
    "Eth", "Ntilde", "Ograve", "Oacute",
    "Ocircumflex", "Otilde", "Odieresis", "multiply",
    "Oslash", "Ugrave", "Uacute", "Ucircumflex",
    "Udieresis", "Yacute", "Thorn", "germandbls",
    /* 0xE0-0xEF */
    "agrave", "aacute", "acircumflex", "atilde",
    "adieresis", "aring", "ae", "ccedilla",
    "egrave", "eacute", "ecircumflex", "edieresis",
    "igrave", "iacute", "icircumflex", "idieresis",
    /* 0xF0-0xFF */
    "eth", "ntilde", "ograve", "oacute",
    "ocircumflex", "otilde", "odieresis", "divide",
    "oslash", "ugrave", "uacute", "ucircumflex",
    "udieresis", "yacute", "thorn", "ydieresis",
};

/* MacRomanEncoding glyph names (for completeness) */
static const char *macroman_glyph_names[256] = {
    /* 0x00-0x7F: same as standard ASCII for printable range */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "space", "exclam", "quotedbl", "numbersign",
    "dollar", "percent", "ampersand", "quotesingle",
    "parenleft", "parenright", "asterisk", "plus",
    "comma", "hyphen", "period", "slash",
    "zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "colon", "semicolon",
    "less", "equal", "greater", "question",
    "at", "A", "B", "C", "D", "E", "F", "G",
    "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W",
    "X", "Y", "Z", "bracketleft",
    "backslash", "bracketright", "asciicircum", "underscore",
    "grave", "a", "b", "c", "d", "e", "f", "g",
    "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w",
    "x", "y", "z", "braceleft",
    "bar", "braceright", "asciitilde", NULL,
    /* 0x80-0xFF: Mac specific */
    "Adieresis", "Aring", "Ccedilla", "Eacute",
    "Ntilde", "Odieresis", "Udieresis", "aacute",
    "agrave", "acircumflex", "adieresis", "atilde",
    "aring", "ccedilla", "eacute", "egrave",
    "ecircumflex", "edieresis", "iacute", "igrave",
    "icircumflex", "idieresis", "ntilde", "oacute",
    "ograve", "ocircumflex", "odieresis", "otilde",
    "uacute", "ugrave", "ucircumflex", "udieresis",
    "dagger", "degree", "cent", "sterling",
    "section", "bullet", "paragraph", "germandbls",
    "registered", "copyright", "trademark", "acute",
    "dieresis", NULL, "AE", "Oslash",
    NULL, "plusminus", NULL, NULL,
    "yen", "mu", NULL, NULL,
    NULL, NULL, NULL, "ordfeminine",
    "ordmasculine", NULL, "ae", "oslash",
    "questiondown", "exclamdown", "logicalnot", NULL,
    "florin", NULL, NULL, "guillemotleft",
    "guillemotright", "ellipsis", "space"/*NBSP*/, "Agrave",
    "Atilde", "Otilde", "OE", "oe",
    "endash", "emdash", "quotedblleft", "quotedblright",
    "quoteleft", "quoteright", "divide", NULL,
    "ydieresis", "Ydieresis", "fraction", "currency",
    "guilsinglleft", "guilsinglright", "fi", "fl",
    "daggerdbl", "periodcentered", "quotesinglbase", "quotedblbase",
    "perthousand", "Acircumflex", "Ecircumflex", "Aacute",
    "Edieresis", "Egrave", "Iacute", "Icircumflex",
    "Idieresis", "Igrave", "Oacute", "Ocircumflex",
    NULL, "Ograve", "Uacute", "Ucircumflex",
    "Ugrave", "dotlessi", "circumflex", "tilde",
    "macron", "breve", "dotaccent", "ring",
    "cedilla", "hungarumlaut", "ogonek", "caron",
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PDF Encoding Resolution
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Resolves the PDF font's /Encoding dictionary to build a mapping from
 * character code to glyph name. This is critical for subset fonts where
 * the CFF font's built-in encoding doesn't match the PDF's intended mapping.
 *
 * The resolution chain is:
 *   char_code (from PDF string)
 *     -> glyph_name (via PDF /Encoding)
 *     -> GID (via CFF charset: search for name)
 *     -> glyph outline (via CFF CharStrings[GID])
 */

/* Maximum number of encoding entries we cache per font */
#define MAX_ENCODING_ENTRIES 256

typedef struct {
    char names[MAX_ENCODING_ENTRIES][64]; /* glyph names indexed by char code */
    bool has_encoding;                     /* true if PDF encoding was found */
} PdfEncodingMap;

/*
 * Build a glyph-name encoding map from a PDF font dictionary's /Encoding.
 * Returns true if encoding was found and parsed.
 */
static bool build_pdf_encoding(PdfDocument *doc, PdfDict *resources,
                                const char *font_res_name,
                                PdfEncodingMap *enc_map)
{
    memset(enc_map, 0, sizeof(*enc_map));

    /* Navigate to the font dictionary */
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return false;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return false;

    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
    if (!font_obj) return false;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return false;

    /* Check font Subtype - only simple fonts (Type1, TrueType) use this encoding.
     * Type0 (composite) fonts use CIDToGIDMap instead. */
    const char *subtype = pdf_dict_get_name(font_obj->dict, "Subtype");
    if (subtype && strcmp(subtype, "Type0") == 0)
        return false; /* Type0 fonts don't use /Encoding this way */

    /* Get the /Encoding entry */
    PdfObj *enc_obj = pdf_dict_get(font_obj->dict, "Encoding");
    if (!enc_obj) return false;
    enc_obj = pdf_resolve(doc, enc_obj);
    if (!enc_obj) return false;

    /* Start with a base encoding */
    const char **base_names = winansi_glyph_names; /* default base */

    if (enc_obj->type == PDF_OBJ_NAME) {
        /* Named encoding: /WinAnsiEncoding, /MacRomanEncoding, etc. */
        const char *enc_name = enc_obj->name;
        if (strcmp(enc_name, "WinAnsiEncoding") == 0) {
            base_names = winansi_glyph_names;
        } else if (strcmp(enc_name, "MacRomanEncoding") == 0) {
            base_names = macroman_glyph_names;
        } else {
            /* Unknown named encoding - use WinAnsi as fallback */
            base_names = winansi_glyph_names;
        }

        /* Copy base encoding names */
        for (int i = 0; i < 256; i++) {
            if (base_names[i]) {
                strncpy(enc_map->names[i], base_names[i], 63);
                enc_map->names[i][63] = '\0';
            }
        }
        enc_map->has_encoding = true;
        return true;
    }

    if (enc_obj->type == PDF_OBJ_DICT) {
        /* Encoding dictionary with optional /BaseEncoding and /Differences */

        /* Get base encoding */
        const char *base_enc_name = pdf_dict_get_name(enc_obj->dict, "BaseEncoding");
        if (base_enc_name) {
            if (strcmp(base_enc_name, "WinAnsiEncoding") == 0) {
                base_names = winansi_glyph_names;
            } else if (strcmp(base_enc_name, "MacRomanEncoding") == 0) {
                base_names = macroman_glyph_names;
            }
        }

        /* Initialize with base encoding */
        for (int i = 0; i < 256; i++) {
            if (base_names[i]) {
                strncpy(enc_map->names[i], base_names[i], 63);
                enc_map->names[i][63] = '\0';
            }
        }

        /* Apply /Differences array */
        PdfObj *diff_obj = pdf_dict_get(enc_obj->dict, "Differences");
        if (diff_obj) {
            diff_obj = pdf_resolve(doc, diff_obj);
            if (diff_obj && diff_obj->type == PDF_OBJ_ARRAY) {
                int current_code = 0;
                int n = pdf_array_len(diff_obj->array);

                for (int i = 0; i < n; i++) {
                    PdfObj *item = pdf_array_get(diff_obj->array, i);
                    if (!item) continue;
                    item = pdf_resolve(doc, item);
                    if (!item) continue;

                    if (item->type == PDF_OBJ_INT) {
                        /* Integer: sets the current character code */
                        current_code = (int)item->integer;
                    } else if (item->type == PDF_OBJ_NAME && item->name) {
                        /* Name: assigns this glyph name to current_code */
                        if (current_code >= 0 && current_code < 256) {
                            strncpy(enc_map->names[current_code], item->name, 63);
                            enc_map->names[current_code][63] = '\0';
                        }
                        current_code++;
                    }
                }
            }
        }

        enc_map->has_encoding = true;
        return true;
    }

    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Text String Rendering
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * High-level function that renders a string of characters using embedded
 * font glyph outlines. For each character:
 * 1. Look up the glyph outline from the parsed font
 * 2. Transform and render the glyph with anti-aliasing
 * 3. Advance the text position
 */

/* Access the current graphics state from the render context */
static PdfGraphicsState *glyph_current_gs(PdfRenderCtx *ctx)
{
    return &ctx->gstate[ctx->gstate_depth];
}

/* Convert a PdfColor to a GDI COLORREF */
static COLORREF glyph_pdf_color_to_gdi(PdfColor c)
{
    int r = (int)(c.r * 255.0 + 0.5);
    int g = (int)(c.g * 255.0 + 0.5);
    int b = (int)(c.b * 255.0 + 0.5);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* Cached PDF encoding maps, keyed by font resource name */
#define MAX_CACHED_ENCODINGS 64

typedef struct {
    char resource_name[PDF_MAX_NAME_LEN];
    PdfEncodingMap enc_map;
    bool resolved;  /* true if we already tried to build encoding */
} CachedEncoding;

static CachedEncoding g_encoding_cache[MAX_CACHED_ENCODINGS];
static int g_encoding_cache_count = 0;

static PdfEncodingMap *get_cached_encoding(PdfDocument *doc, PdfDict *resources,
                                            const char *font_res_name)
{
    /* Check cache first */
    for (int i = 0; i < g_encoding_cache_count; i++) {
        if (strcmp(g_encoding_cache[i].resource_name, font_res_name) == 0) {
            if (g_encoding_cache[i].resolved)
                return g_encoding_cache[i].enc_map.has_encoding ?
                       &g_encoding_cache[i].enc_map : NULL;
        }
    }

    /* Build and cache */
    if (g_encoding_cache_count >= MAX_CACHED_ENCODINGS)
        return NULL;

    CachedEncoding *entry = &g_encoding_cache[g_encoding_cache_count++];
    strncpy(entry->resource_name, font_res_name, PDF_MAX_NAME_LEN - 1);
    entry->resource_name[PDF_MAX_NAME_LEN - 1] = '\0';
    entry->resolved = true;

    if (build_pdf_encoding(doc, resources, font_res_name, &entry->enc_map)) {
        return &entry->enc_map;
    }

    return NULL;
}

/* ─── Type0/CID Font Helpers ─── */

/* Resolve CID width from /W array and /DW default.
 *
 * /W array format (from CIDFont dict):
 *   c [w1 w2 w3 ...]   — CIDs c, c+1, c+2, ... get widths w1, w2, w3, ...
 *   c_first c_last w    — CIDs c_first through c_last all get width w
 *
 * Returns width in font units (typically 1/1000 of text space).
 */
static double cid_get_width(PdfDocument *doc, PdfArray *w_array,
                             int default_w, int cid)
{
    if (!w_array) return (double)default_w;

    int n = pdf_array_len(w_array);
    int i = 0;

    while (i < n) {
        PdfObj *first_obj = pdf_array_get(w_array, i);
        if (!first_obj) break;
        first_obj = pdf_resolve(doc, first_obj);
        if (!first_obj || first_obj->type != PDF_OBJ_INT) break;

        int first_cid = (int)first_obj->integer;
        i++;
        if (i >= n) break;

        PdfObj *second_obj = pdf_array_get(w_array, i);
        if (!second_obj) break;
        second_obj = pdf_resolve(doc, second_obj);
        if (!second_obj) break;

        if (second_obj->type == PDF_OBJ_ARRAY) {
            /* Format: c [w1 w2 w3 ...] */
            PdfArray *widths = second_obj->array;
            int wcount = pdf_array_len(widths);
            int offset = cid - first_cid;
            if (offset >= 0 && offset < wcount) {
                PdfObj *w_obj = pdf_array_get(widths, offset);
                if (w_obj) {
                    w_obj = pdf_resolve(doc, w_obj);
                    if (w_obj) {
                        if (w_obj->type == PDF_OBJ_INT)
                            return (double)w_obj->integer;
                        if (w_obj->type == PDF_OBJ_REAL)
                            return w_obj->real;
                    }
                }
            }
            i++;
        } else if (second_obj->type == PDF_OBJ_INT) {
            /* Format: c_first c_last w */
            int last_cid = (int)second_obj->integer;
            i++;
            if (i >= n) break;

            PdfObj *w_obj = pdf_array_get(w_array, i);
            if (!w_obj) break;
            w_obj = pdf_resolve(doc, w_obj);
            i++;

            if (cid >= first_cid && cid <= last_cid && w_obj) {
                if (w_obj->type == PDF_OBJ_INT)
                    return (double)w_obj->integer;
                if (w_obj->type == PDF_OBJ_REAL)
                    return w_obj->real;
            }
        } else {
            break; /* unexpected format */
        }
    }

    return (double)default_w;
}

/* Detect whether a font resource is a Type0 (composite) font and extract
 * the CID font parameters needed for rendering.
 *
 * On success, sets:
 *   *out_is_type0 = true
 *   *out_default_w = /DW value (default 1000)
 *   *out_w_array = /W array (may be NULL)
 *   *out_cidtogidmap_data = decoded CIDToGIDMap stream data (NULL if Identity)
 *   *out_cidtogidmap_len = length of CIDToGIDMap data
 *   *out_is_identity_h = true if /Encoding is Identity-H or Identity-V (2-byte)
 */
typedef struct {
    bool is_type0;
    bool is_identity_h;
    int  default_w;
    PdfArray *w_array;
    const uint8_t *cidtogidmap_data;
    size_t cidtogidmap_len;
} CidFontInfo;

static void detect_type0_font(PdfDocument *doc, PdfDict *resources,
                               const char *font_res_name, CidFontInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->default_w = 1000;

    /* Get /Font dictionary from resources */
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return;

    /* Get the specific font dict (e.g., /F1) */
    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
    if (!font_obj) return;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return;

    /* Check /Subtype == /Type0 */
    const char *subtype = pdf_dict_get_name(font_obj->dict, "Subtype");
    if (!subtype || strcmp(subtype, "Type0") != 0)
        return;

    info->is_type0 = true;

    /* Check /Encoding (Identity-H or Identity-V mean 2-byte codes) */
    const char *encoding = pdf_dict_get_name(font_obj->dict, "Encoding");
    if (encoding && (strcmp(encoding, "Identity-H") == 0 ||
                     strcmp(encoding, "Identity-V") == 0)) {
        info->is_identity_h = true;
    }

    /* Navigate to DescendantFonts -> first CIDFont dict */
    PdfObj *descendants = pdf_dict_get(font_obj->dict, "DescendantFonts");
    if (!descendants) return;
    descendants = pdf_resolve(doc, descendants);
    if (!descendants || descendants->type != PDF_OBJ_ARRAY) return;
    if (pdf_array_len(descendants->array) < 1) return;

    PdfObj *cid_font = pdf_array_get(descendants->array, 0);
    cid_font = pdf_resolve(doc, cid_font);
    if (!cid_font || cid_font->type != PDF_OBJ_DICT) return;

    /* Get /DW (default width, default 1000) */
    info->default_w = pdf_dict_get_int(cid_font->dict, "DW", 1000);

    /* Get /W (per-CID width overrides) */
    PdfObj *w_obj = pdf_dict_get(cid_font->dict, "W");
    if (w_obj) {
        w_obj = pdf_resolve(doc, w_obj);
        if (w_obj && w_obj->type == PDF_OBJ_ARRAY)
            info->w_array = w_obj->array;
    }

    /* Get /CIDToGIDMap */
    PdfObj *cidtogid = pdf_dict_get(cid_font->dict, "CIDToGIDMap");
    if (cidtogid) {
        cidtogid = pdf_resolve(doc, cidtogid);
        if (cidtogid) {
            if (cidtogid->type == PDF_OBJ_NAME) {
                /* /Identity — CID == GID, nothing to do */
            } else if (cidtogid->type == PDF_OBJ_STREAM) {
                /* Stream: big-endian uint16 table, gid = data[cid*2]<<8 | data[cid*2+1] */
                if (pdf_decode_stream(doc, cidtogid->stream)) {
                    info->cidtogidmap_data = cidtogid->stream->decoded_data;
                    info->cidtogidmap_len = cidtogid->stream->decoded_length;
                }
            }
        }
    }
}

/* Map CID to GID using a CIDToGIDMap stream (big-endian uint16 table).
 * If no map is provided (Identity), returns cid unchanged. */
static int cid_to_gid(const CidFontInfo *info, int cid)
{
    if (info->cidtogidmap_data && info->cidtogidmap_len > 0) {
        size_t offset = (size_t)cid * 2;
        if (offset + 1 < info->cidtogidmap_len) {
            return (info->cidtogidmap_data[offset] << 8) |
                    info->cidtogidmap_data[offset + 1];
        }
        return 0; /* CID out of range of map */
    }
    /* Identity: CID == GID */
    return cid;
}

/* ═══════════════════════════════════════════════════════════════════════════ */

bool glyph_render_text_string(PdfRenderCtx *ctx, PdfDict *resources,
                               const uint8_t *str, size_t len)
{
    if (!ctx || !resources || !str || len == 0)
        return false;

    PdfGraphicsState *gs = glyph_current_gs(ctx);

    /* Skip invisible text */
    if (gs->text_render_mode == 3)
        return false;

    /* Must have a font name set */
    if (gs->font_name[0] == '\0')
        return false;

    /* Try to extract/parse the embedded font */
    ParsedFont *font = extract_embedded_font(ctx->doc, resources, gs->font_name);
    if (!font)
        return false; /* no embedded font, caller should fall back to TextOutW */

    double font_size = gs->font_size;
    if (font_size == 0.0)
        return false;

    /* Determine text color */
    COLORREF text_color;
    if (gs->text_render_mode == 1) {
        text_color = glyph_pdf_color_to_gdi(gs->stroke_color);
    } else {
        text_color = glyph_pdf_color_to_gdi(gs->fill_color);
    }

    double h_scale = gs->horiz_scaling / 100.0;

    /* ─── Type0/CID Composite Font Path ─── */
    CidFontInfo cid_info;
    detect_type0_font(ctx->doc, resources, gs->font_name, &cid_info);

    if (cid_info.is_type0 && cid_info.is_identity_h) {
        /* Type0 font with Identity-H encoding: 2-byte character codes.
         * Each pair of bytes forms a CID: (str[i] << 8) | str[i+1]
         * CID -> GID via CIDToGIDMap (Identity or stream)
         * Width from /W array and /DW default */

        for (size_t i = 0; i + 1 < len; i += 2) {
            int cid = ((int)str[i] << 8) | (int)str[i + 1];
            int gid = cid_to_gid(&cid_info, cid);

            /* Get width for this CID from /W array (in 1/1000 units) */
            double cid_width = cid_get_width(ctx->doc, cid_info.w_array,
                                              cid_info.default_w, cid);

            /* Convert CID width (1/1000 units) to font design units.
             * CID widths are always in 1/1000 of text space, but the font's
             * units_per_em may differ. Scale accordingly. */
            double advance_width = cid_width * ((double)font->units_per_em / 1000.0);

            /* Get glyph outline by GID */
            GlyphOutline outline;
            glyph_outline_init(&outline);
            bool got_glyph = parsed_font_get_glyph_by_gid(font, gid, &outline);

            if (got_glyph && outline.count > 0 && gs->text_render_mode != 3) {
                PdfMatrix combined = pdf_matrix_multiply(ctx->text_matrix, gs->ctm);

                if (gs->text_rise != 0.0) {
                    PdfMatrix rise = PDF_IDENTITY_MATRIX;
                    rise.f = gs->text_rise;
                    combined = pdf_matrix_multiply(rise, combined);
                }

                render_glyph_outline(ctx->hdc, &outline, font, font_size,
                                      combined, text_color);
            }

            if (got_glyph) {
                glyph_outline_free(&outline);
            }

            /* Advance text position using CID width */
            double advance = (advance_width / (double)font->units_per_em) * font_size;

            advance += gs->char_spacing;

            /* Word spacing for CID 0 (space) — PDF spec says word spacing
             * applies to single-byte code 32, but for CID fonts it applies
             * to CID that maps to space. In practice, check for CID == space
             * character codes commonly used. */
            if (cid == 0x0003 || cid == 0x0020 || cid == 32)
                advance += gs->word_spacing;

            advance *= h_scale;

            PdfMatrix adv = PDF_IDENTITY_MATRIX;
            adv.e = advance;
            ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
        }

        return true;
    }

    /* ─── Simple Font Path (original 1-byte character codes) ─── */

    /* Get PDF encoding map for this font (if available) */
    PdfEncodingMap *enc_map = NULL;
    if ((font->is_cff && font->charset_sids) || font->is_type1) {
        enc_map = get_cached_encoding(ctx->doc, resources, gs->font_name);
    }

    /* Render each character */
    for (size_t i = 0; i < len; i++) {
        int char_code = str[i];

        /* Skip only true null characters. DO NOT skip 0x01-0x1F because
         * many PDF fonts (especially LaTeX math fonts like CMSY10) use codes
         * below 0x20 for valid glyphs (∼, ×, ±, ∞, etc.). */
        if (char_code == 0) {
            /* Still advance text position using the font's width for this code,
             * but do not render a glyph. */
            double advance_width = parsed_font_get_advance(font, char_code);
            double advance = (advance_width / (double)font->units_per_em) * font_size;
            advance += gs->char_spacing;
            if (char_code == 32)
                advance += gs->word_spacing;
            advance *= h_scale;
            PdfMatrix adv = PDF_IDENTITY_MATRIX;
            adv.e = advance;
            ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
            continue;
        }

        /* Get glyph outline */
        GlyphOutline outline;
        glyph_outline_init(&outline);

        bool got_glyph = false;
        int resolved_gid = -1;

        if (enc_map && (font->is_cff || font->is_type1)) {
            /* PDF encoding resolution path:
             * char_code -> glyph_name (via PDF /Encoding)
             * -> GID (via CFF charset name search)
             * -> glyph outline (via CFF CharStrings[GID]) */
            const char *glyph_name = enc_map->names[char_code];
            if (glyph_name && glyph_name[0] != '\0') {
                resolved_gid = parsed_font_find_gid_by_name(font, glyph_name);
                if (resolved_gid >= 0) {
                    got_glyph = parsed_font_get_glyph_by_gid(font, resolved_gid, &outline);
                }
            }

        }

        if (!got_glyph) {
            /* Fallback: use the font's built-in encoding */
            got_glyph = parsed_font_get_glyph(font, char_code, &outline);
        }

        if (got_glyph && outline.count > 0 && gs->text_render_mode != 3) {
            /* Compute combined matrix: text_matrix * CTM */
            PdfMatrix combined = pdf_matrix_multiply(ctx->text_matrix, gs->ctm);

            /* Apply text rise: shift origin vertically in text space */
            if (gs->text_rise != 0.0) {
                PdfMatrix rise = PDF_IDENTITY_MATRIX;
                rise.f = gs->text_rise;
                combined = pdf_matrix_multiply(rise, combined);
            }

            /* Render the glyph with anti-aliasing */
            render_glyph_outline(ctx->hdc, &outline, font, font_size,
                                  combined, text_color);
        }

        if (got_glyph) {
            glyph_outline_free(&outline);
        }

        /* Advance text position.
         * Use the advance width from the glyph we actually rendered. */
        double advance_width;
        if (resolved_gid >= 0 && font->glyph_widths &&
            resolved_gid < font->num_glyphs) {
            advance_width = font->glyph_widths[resolved_gid];
        } else {
            advance_width = parsed_font_get_advance(font, char_code);
        }
        double advance = (advance_width / (double)font->units_per_em) * font_size;

        /* Add character spacing */
        advance += gs->char_spacing;

        /* Add word spacing for space characters (code 32) */
        if (char_code == 32)
            advance += gs->word_spacing;

        /* Apply horizontal scaling */
        advance *= h_scale;

        /* Update text matrix: translate by advance in text space */
        PdfMatrix adv = PDF_IDENTITY_MATRIX;
        adv.e = advance;
        ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

void glyph_cache_cleanup(void)
{
    for (int i = 0; i < g_font_cache_count; i++) {
        if (g_font_cache[i].parsed) {
            parsed_font_free(g_font_cache[i].parsed);
            g_font_cache[i].parsed = NULL;
        }
    }
    g_font_cache_count = 0;

    /* Also clear encoding cache */
    g_encoding_cache_count = 0;
}
