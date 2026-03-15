/*
 * AmundsPDF - pdf_render.c
 * Content stream interpreter and GDI-based page renderer.
 *
 * Parses PDF content streams and executes operators to render pages
 * to Win32 HBITMAPs via GDI. Handles graphics state, paths, text,
 * color, images, and form XObjects.
 */

#include "pdf_render.h"
#include "pdf_parser.h"
#include "pdf_fonts.h"
#include "pdf_glyph_render.h"
#include "pdf_raster.h"
#include "pdf_profile.h"
#include <objbase.h>  /* CoInitializeEx, CoCreateInstance for WIC image decoding */

/* Global profiling accumulator (declared extern in pdf_profile.h) */
RenderProfile g_prof;

/* ═══════════════════════════════════════════════════════════════════════════
 * Content Stream Tokenizer
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tokens in a content stream are: numbers, names, strings, operators.
 * We parse them into an operand stack and dispatch on operator keywords.
 */

typedef enum {
    TOK_EOF = 0,
    TOK_NUMBER,
    TOK_NAME,       /* /SomeName */
    TOK_STRING,     /* (text) or <hex> */
    TOK_ARRAY_BEGIN,/* [ */
    TOK_ARRAY_END,  /* ] */
    TOK_OPERATOR,   /* keyword like q, Q, Tf, Tj, etc. */
    TOK_BOOL,
} CSTokType;

typedef struct {
    CSTokType type;
    double    number;
    char      text[PDF_MAX_NAME_LEN]; /* name or operator text */
    uint8_t  *str_data;               /* string data (heap-allocated) */
    size_t    str_len;                 /* string length */
    bool      bool_val;
} CSToken;

typedef struct {
    const uint8_t *data;
    size_t         length;
    size_t         pos;
} CSParser;

/* ─── Tokenizer helpers ─── */

static inline bool cs_at_end(CSParser *p)
{
    return p->pos >= p->length;
}

static inline int cs_peek(CSParser *p)
{
    if (p->pos >= p->length) return -1;
    return p->data[p->pos];
}

static inline int cs_next(CSParser *p)
{
    if (p->pos >= p->length) return -1;
    return p->data[p->pos++];
}

static inline void cs_skip(CSParser *p)
{
    if (p->pos < p->length) p->pos++;
}

static bool cs_is_whitespace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\0';
}

static bool cs_is_delimiter(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static void cs_skip_whitespace_and_comments(CSParser *p)
{
    while (!cs_at_end(p)) {
        int c = cs_peek(p);
        if (cs_is_whitespace(c)) {
            cs_skip(p);
        } else if (c == '%') {
            /* Comment: skip to end of line */
            while (!cs_at_end(p) && cs_peek(p) != '\n' && cs_peek(p) != '\r')
                cs_skip(p);
        } else {
            break;
        }
    }
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a literal string (xxx) with escape handling and nested parens */
static bool cs_parse_literal_string(CSParser *p, CSToken *tok)
{
    cs_skip(p); /* consume '(' */
    int depth = 1;
    uint8_t *buf = (uint8_t *)malloc(PDF_MAX_STRING_LEN);
    if (!buf) return false;
    size_t len = 0;

    while (!cs_at_end(p) && depth > 0 && len < PDF_MAX_STRING_LEN - 1) {
        int c = cs_next(p);
        if (c == '(') {
            depth++;
            buf[len++] = (uint8_t)c;
        } else if (c == ')') {
            depth--;
            if (depth > 0) buf[len++] = (uint8_t)c;
        } else if (c == '\\') {
            int esc = cs_next(p);
            switch (esc) {
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                case '(':  buf[len++] = '(';  break;
                case ')':  buf[len++] = ')';  break;
                case '\\': buf[len++] = '\\'; break;
                case '\r':
                    /* line continuation: \<CR> or \<CR><LF> */
                    if (cs_peek(p) == '\n') cs_skip(p);
                    break;
                case '\n':
                    /* line continuation */
                    break;
                default:
                    /* Octal escape: up to 3 digits */
                    if (esc >= '0' && esc <= '7') {
                        int val = esc - '0';
                        if (cs_peek(p) >= '0' && cs_peek(p) <= '7') {
                            val = val * 8 + (cs_next(p) - '0');
                            if (cs_peek(p) >= '0' && cs_peek(p) <= '7')
                                val = val * 8 + (cs_next(p) - '0');
                        }
                        buf[len++] = (uint8_t)(val & 0xFF);
                    } else {
                        /* Unknown escape: treat as literal */
                        buf[len++] = (uint8_t)esc;
                    }
                    break;
            }
        } else {
            buf[len++] = (uint8_t)c;
        }
    }

    tok->type = TOK_STRING;
    tok->str_data = buf;
    tok->str_len = len;
    return true;
}

/* Parse a hex string <xx xx> */
static bool cs_parse_hex_string(CSParser *p, CSToken *tok)
{
    cs_skip(p); /* consume '<' */
    uint8_t *buf = (uint8_t *)malloc(PDF_MAX_STRING_LEN);
    if (!buf) return false;
    size_t len = 0;
    bool high_nibble = true;
    uint8_t byte_val = 0;

    while (!cs_at_end(p) && len < PDF_MAX_STRING_LEN) {
        int c = cs_next(p);
        if (c == '>') break;
        if (cs_is_whitespace(c)) continue;
        int hv = hex_val(c);
        if (hv < 0) continue;  /* skip invalid hex chars */
        if (high_nibble) {
            byte_val = (uint8_t)(hv << 4);
            high_nibble = false;
        } else {
            byte_val |= (uint8_t)hv;
            buf[len++] = byte_val;
            high_nibble = true;
        }
    }
    /* If we ended on a high nibble that was set, the low nibble is implicitly 0 */
    if (!high_nibble) {
        buf[len++] = byte_val;
    }

    tok->type = TOK_STRING;
    tok->str_data = buf;
    tok->str_len = len;
    return true;
}

/*
 * Get the next token from the content stream.
 * Returns false on EOF or error.
 */
static bool cs_next_token(CSParser *p, CSToken *tok)
{
    memset(tok, 0, sizeof(*tok));
    cs_skip_whitespace_and_comments(p);
    if (cs_at_end(p)) {
        tok->type = TOK_EOF;
        return false;
    }

    int c = cs_peek(p);

    /* Literal string */
    if (c == '(') {
        return cs_parse_literal_string(p, tok);
    }

    /* Hex string or dict markers */
    if (c == '<') {
        cs_skip(p);
        if (cs_peek(p) == '<') {
            /* << is a dict begin -- shouldn't appear in content stream normally,
               but handle inline images. Treat as operator. */
            cs_skip(p);
            tok->type = TOK_OPERATOR;
            strcpy(tok->text, "<<");
            return true;
        }
        /* Back up and re-parse as hex string */
        p->pos--;
        return cs_parse_hex_string(p, tok);
    }

    if (c == '>') {
        cs_skip(p);
        if (cs_peek(p) == '>') {
            cs_skip(p);
            tok->type = TOK_OPERATOR;
            strcpy(tok->text, ">>");
            return true;
        }
        /* Stray '>' -- ignore */
        tok->type = TOK_OPERATOR;
        strcpy(tok->text, ">");
        return true;
    }

    /* Array markers */
    if (c == '[') {
        cs_skip(p);
        tok->type = TOK_ARRAY_BEGIN;
        strcpy(tok->text, "[");
        return true;
    }
    if (c == ']') {
        cs_skip(p);
        tok->type = TOK_ARRAY_END;
        strcpy(tok->text, "]");
        return true;
    }

    /* Name object */
    if (c == '/') {
        cs_skip(p); /* consume '/' */
        int i = 0;
        while (!cs_at_end(p) && i < PDF_MAX_NAME_LEN - 1) {
            int nc = cs_peek(p);
            if (cs_is_whitespace(nc) || cs_is_delimiter(nc)) break;
            /* Handle #xx hex escapes in names */
            if (nc == '#' && p->pos + 2 < p->length) {
                cs_skip(p);
                int h1 = hex_val(cs_next(p));
                int h2 = hex_val(cs_next(p));
                if (h1 >= 0 && h2 >= 0) {
                    tok->text[i++] = (char)((h1 << 4) | h2);
                }
            } else {
                tok->text[i++] = (char)cs_next(p);
            }
        }
        tok->text[i] = '\0';
        tok->type = TOK_NAME;
        return true;
    }

    /* Number (integer or real) or operator starting with sign */
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        char numbuf[128];
        int i = 0;
        bool has_dot = false;
        bool is_number = true;

        /* Accumulate characters that could form a number */
        while (!cs_at_end(p) && i < 126) {
            int nc = cs_peek(p);
            if (nc == '.') {
                if (has_dot) break;  /* second dot: stop */
                has_dot = true;
                numbuf[i++] = (char)cs_next(p);
            } else if (nc >= '0' && nc <= '9') {
                numbuf[i++] = (char)cs_next(p);
            } else if (i == 0 && (nc == '+' || nc == '-')) {
                numbuf[i++] = (char)cs_next(p);
            } else {
                break;
            }
        }
        numbuf[i] = '\0';

        /* Validate: a lone sign or dot is not a number */
        if (i == 1 && (numbuf[0] == '+' || numbuf[0] == '-' || numbuf[0] == '.')) {
            is_number = false;
        }
        /* A sign followed by a dot and nothing else */
        if (i == 2 && (numbuf[0] == '+' || numbuf[0] == '-') && numbuf[1] == '.') {
            is_number = false;
        }

        if (is_number) {
            tok->type = TOK_NUMBER;
            tok->number = atof(numbuf);
            return true;
        }

        /* Fell through: treat as beginning of an operator keyword */
        /* (This shouldn't happen in valid PDF, but handle gracefully) */
        while (!cs_at_end(p) && i < 126 &&
               !cs_is_whitespace(cs_peek(p)) && !cs_is_delimiter(cs_peek(p))) {
            numbuf[i++] = (char)cs_next(p);
        }
        numbuf[i] = '\0';
        tok->type = TOK_OPERATOR;
        strncpy(tok->text, numbuf, PDF_MAX_NAME_LEN - 1);
        tok->text[PDF_MAX_NAME_LEN - 1] = '\0';
        return true;
    }

    /* Keyword (operator or boolean) */
    {
        char kw[PDF_MAX_NAME_LEN];
        int i = 0;
        while (!cs_at_end(p) && i < PDF_MAX_NAME_LEN - 1 &&
               !cs_is_whitespace(cs_peek(p)) && !cs_is_delimiter(cs_peek(p))) {
            kw[i++] = (char)cs_next(p);
        }
        kw[i] = '\0';

        if (strcmp(kw, "true") == 0) {
            tok->type = TOK_BOOL;
            tok->bool_val = true;
            return true;
        }
        if (strcmp(kw, "false") == 0) {
            tok->type = TOK_BOOL;
            tok->bool_val = false;
            return true;
        }

        tok->type = TOK_OPERATOR;
        strncpy(tok->text, kw, PDF_MAX_NAME_LEN - 1);
        tok->text[PDF_MAX_NAME_LEN - 1] = '\0';
        return true;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Operand Stack
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    CSToken items[PDF_MAX_OPERANDS];
    int     count;
} OperandStack;

static void opstack_clear(OperandStack *s)
{
    /* Free any heap-allocated string data */
    for (int i = 0; i < s->count; i++) {
        if (s->items[i].str_data) {
            free(s->items[i].str_data);
            s->items[i].str_data = NULL;
        }
    }
    s->count = 0;
}

static void opstack_push(OperandStack *s, CSToken *tok)
{
    if (s->count < PDF_MAX_OPERANDS) {
        s->items[s->count] = *tok;
        /* Transfer ownership of str_data */
        tok->str_data = NULL;
        s->count++;
    } else {
        /* Stack overflow: discard token */
        if (tok->str_data) free(tok->str_data);
    }
}

static double opstack_number(OperandStack *s, int idx)
{
    if (idx < 0 || idx >= s->count) return 0.0;
    return s->items[idx].number;
}

static const char *opstack_name(OperandStack *s, int idx)
{
    if (idx < 0 || idx >= s->count) return "";
    return s->items[idx].text;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Graphics State Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static PdfGraphicsState *current_gs(PdfRenderCtx *ctx)
{
    return &ctx->gstate[ctx->gstate_depth];
}

static void gs_init(PdfGraphicsState *gs)
{
    gs->ctm = PDF_IDENTITY_MATRIX;
    gs->fill_color   = (PdfColor){0.0, 0.0, 0.0};
    gs->stroke_color = (PdfColor){0.0, 0.0, 0.0};
    gs->line_width   = 1.0;
    gs->line_cap     = 0;
    gs->line_join    = 0;
    gs->miter_limit  = 10.0;
    gs->dash_count   = 0;      /* solid line (no dash pattern) */
    gs->dash_phase   = 0.0;
    gs->font_size    = 12.0;
    gs->font_name[0] = '\0';
    gs->char_spacing   = 0.0;
    gs->word_spacing   = 0.0;
    gs->horiz_scaling  = 100.0;
    gs->leading        = 0.0;
    gs->text_render_mode = 0;
    gs->text_rise      = 0.0;
    /* Color space state: default to unknown (operand-count guessing) */
    memset(&gs->fill_cs, 0, sizeof(gs->fill_cs));
    memset(&gs->stroke_cs, 0, sizeof(gs->stroke_cs));
}

/* Get the current clip mask (at the current graphics state depth).
 * Returns NULL if no clip is active. */
static uint8_t *current_clip_mask(PdfRenderCtx *ctx)
{
    return ctx->clip_mask_stack[ctx->gstate_depth];
}

static void gs_save(PdfRenderCtx *ctx)
{
    if (ctx->gstate_depth + 1 < PDF_MAX_GSTATE_STACK) {
        int old_depth = ctx->gstate_depth;
        ctx->gstate[old_depth + 1] = ctx->gstate[old_depth];
        ctx->gstate_depth++;

        /* Duplicate the clip mask for the new level.
         * Each level owns its own copy so W/W* can modify it independently. */
        uint8_t *old_mask = ctx->clip_mask_stack[old_depth];
        if (old_mask) {
            size_t sz = (size_t)ctx->clip_mask_w * ctx->clip_mask_h;
            uint8_t *new_mask = (uint8_t *)malloc(sz);
            if (new_mask) {
                memcpy(new_mask, old_mask, sz);
            }
            ctx->clip_mask_stack[ctx->gstate_depth] = new_mask;
        } else {
            ctx->clip_mask_stack[ctx->gstate_depth] = NULL;
        }

        SaveDC(ctx->hdc);
    }
}

static void gs_restore(PdfRenderCtx *ctx)
{
    if (ctx->gstate_depth > 0) {
        /* Free the clip mask at the current level */
        if (ctx->clip_mask_stack[ctx->gstate_depth]) {
            free(ctx->clip_mask_stack[ctx->gstate_depth]);
            ctx->clip_mask_stack[ctx->gstate_depth] = NULL;
        }
        ctx->gstate_depth--;
        RestoreDC(ctx->hdc, -1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Color Conversion Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static COLORREF pdf_color_to_gdi(PdfColor c)
{
    int r = (int)(c.r * 255.0 + 0.5);
    int g = (int)(c.g * 255.0 + 0.5);
    int b = (int)(c.b * 255.0 + 0.5);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return RGB(r, g, b);
}

static PdfColor cmyk_to_rgb(double c, double m, double y, double k)
{
    PdfColor rgb;
    /* Ink cross-contamination model approximating US Web Coated SWOP ICC.
     * In real printing, each CMYK ink absorbs light across multiple RGB
     * channels, not just its theoretical complement. For example, cyan ink
     * primarily absorbs Red but also absorbs ~20% of Blue. The naive formula
     * R=(1-C)(1-K) ignores this, producing oversaturated colors (especially
     * blues). These coefficients approximate the SWOP ICC profile behavior
     * and match Adobe/MuPDF output within ~6 RGB units. */
    double r_abs = 0.91 * c + 0.03 * m + 0.00 * y + k;
    double g_abs = 0.04 * c + 0.85 * m + 0.05 * y + k;
    double b_abs = 0.21 * c + 0.10 * m + 0.84 * y + k;
    rgb.r = 1.0 - (r_abs < 1.0 ? r_abs : 1.0);
    rgb.g = 1.0 - (g_abs < 1.0 ? g_abs : 1.0);
    rgb.b = 1.0 - (b_abs < 1.0 ? b_abs : 1.0);
    if (rgb.r < 0.0) rgb.r = 0.0;
    if (rgb.g < 0.0) rgb.g = 0.0;
    if (rgb.b < 0.0) rgb.b = 0.0;
    return rgb;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Color Space Resolution
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Resolve a color space name from the page's /Resources /ColorSpace dictionary
 * and populate a PdfColorSpaceInfo with the type, component count, and
 * palette data (for Indexed spaces).
 */

static void resolve_colorspace(PdfDocument *doc, PdfDict *resources,
                                const char *cs_name, PdfColorSpaceInfo *info)
{
    memset(info, 0, sizeof(*info));
    strncpy(info->name, cs_name, PDF_MAX_NAME_LEN - 1);
    info->name[PDF_MAX_NAME_LEN - 1] = '\0';

    /* Handle built-in device color space names */
    if (strcmp(cs_name, "DeviceGray") == 0 || strcmp(cs_name, "G") == 0) {
        info->type = PDF_CS_DEVICE_GRAY;
        info->components = 1;
        return;
    }
    if (strcmp(cs_name, "DeviceRGB") == 0 || strcmp(cs_name, "RGB") == 0) {
        info->type = PDF_CS_DEVICE_RGB;
        info->components = 3;
        return;
    }
    if (strcmp(cs_name, "DeviceCMYK") == 0 || strcmp(cs_name, "CMYK") == 0) {
        info->type = PDF_CS_DEVICE_CMYK;
        info->components = 4;
        return;
    }
    if (strcmp(cs_name, "Pattern") == 0) {
        info->type = PDF_CS_PATTERN;
        info->components = 0;
        return;
    }

    /* Look up in page Resources /ColorSpace dictionary */
    if (!resources) return;
    PdfObj *cs_dict_obj = pdf_dict_get(resources, "ColorSpace");
    if (!cs_dict_obj) return;
    cs_dict_obj = pdf_resolve(doc, cs_dict_obj);
    if (!cs_dict_obj || cs_dict_obj->type != PDF_OBJ_DICT) return;

    PdfObj *cs_obj = pdf_dict_get(cs_dict_obj->dict, cs_name);
    if (!cs_obj) return;
    cs_obj = pdf_resolve(doc, cs_obj);
    if (!cs_obj) return;

    /* The color space definition can be a name or an array */
    if (cs_obj->type == PDF_OBJ_NAME) {
        /* Recurse with the resolved name */
        resolve_colorspace(doc, resources, cs_obj->name, info);
        strncpy(info->name, cs_name, PDF_MAX_NAME_LEN - 1);
        return;
    }

    if (cs_obj->type != PDF_OBJ_ARRAY) return;
    PdfArray *arr = cs_obj->array;
    if (pdf_array_len(arr) < 1) return;

    /* First element is the color space family name */
    PdfObj *family_obj = pdf_array_get(arr, 0);
    if (!family_obj) return;
    family_obj = pdf_resolve(doc, family_obj);
    if (!family_obj || family_obj->type != PDF_OBJ_NAME) return;
    const char *family = family_obj->name;

    /* ── ICCBased ── */
    if (strcmp(family, "ICCBased") == 0 && pdf_array_len(arr) >= 2) {
        info->type = PDF_CS_ICCBASED;
        /* The second element is a stream with an /N entry */
        PdfObj *icc_stream_obj = pdf_array_get(arr, 1);
        if (icc_stream_obj) {
            icc_stream_obj = pdf_resolve(doc, icc_stream_obj);
            if (icc_stream_obj && icc_stream_obj->type == PDF_OBJ_STREAM) {
                int n = pdf_dict_get_int(icc_stream_obj->stream->dict, "N", 3);
                info->components = n;
            } else {
                info->components = 3; /* default to RGB */
            }
        } else {
            info->components = 3;
        }
        return;
    }

    /* ── Indexed ── [/Indexed base hival lookup] */
    if (strcmp(family, "Indexed") == 0 && pdf_array_len(arr) >= 4) {
        info->type = PDF_CS_INDEXED;
        info->components = 1; /* Indexed always takes 1 operand (the index) */

        /* Resolve the base color space */
        PdfObj *base_obj = pdf_array_get(arr, 1);
        if (base_obj) {
            base_obj = pdf_resolve(doc, base_obj);
            if (base_obj) {
                if (base_obj->type == PDF_OBJ_NAME) {
                    if (strcmp(base_obj->name, "DeviceRGB") == 0) {
                        info->indexed_base_type = PDF_CS_DEVICE_RGB;
                        info->indexed_base_components = 3;
                    } else if (strcmp(base_obj->name, "DeviceCMYK") == 0) {
                        info->indexed_base_type = PDF_CS_DEVICE_CMYK;
                        info->indexed_base_components = 4;
                    } else if (strcmp(base_obj->name, "DeviceGray") == 0) {
                        info->indexed_base_type = PDF_CS_DEVICE_GRAY;
                        info->indexed_base_components = 1;
                    } else {
                        info->indexed_base_type = PDF_CS_DEVICE_RGB;
                        info->indexed_base_components = 3;
                    }
                } else if (base_obj->type == PDF_OBJ_ARRAY && pdf_array_len(base_obj->array) >= 2) {
                    /* Base is itself an array, e.g. [/ICCBased ...] */
                    PdfObj *base_family = pdf_array_get(base_obj->array, 0);
                    if (base_family) base_family = pdf_resolve(doc, base_family);
                    if (base_family && base_family->type == PDF_OBJ_NAME &&
                        strcmp(base_family->name, "ICCBased") == 0) {
                        PdfObj *icc_obj = pdf_array_get(base_obj->array, 1);
                        if (icc_obj) icc_obj = pdf_resolve(doc, icc_obj);
                        if (icc_obj && icc_obj->type == PDF_OBJ_STREAM) {
                            int n = pdf_dict_get_int(icc_obj->stream->dict, "N", 3);
                            info->indexed_base_components = n;
                            if (n == 1) info->indexed_base_type = PDF_CS_DEVICE_GRAY;
                            else if (n == 4) info->indexed_base_type = PDF_CS_DEVICE_CMYK;
                            else info->indexed_base_type = PDF_CS_DEVICE_RGB;
                        } else {
                            info->indexed_base_type = PDF_CS_DEVICE_RGB;
                            info->indexed_base_components = 3;
                        }
                    } else {
                        info->indexed_base_type = PDF_CS_DEVICE_RGB;
                        info->indexed_base_components = 3;
                    }
                } else {
                    info->indexed_base_type = PDF_CS_DEVICE_RGB;
                    info->indexed_base_components = 3;
                }
            }
        }
        if (info->indexed_base_components == 0) {
            info->indexed_base_type = PDF_CS_DEVICE_RGB;
            info->indexed_base_components = 3;
        }

        /* hival */
        PdfObj *hival_obj = pdf_array_get(arr, 2);
        if (hival_obj) {
            hival_obj = pdf_resolve(doc, hival_obj);
            if (hival_obj) {
                if (hival_obj->type == PDF_OBJ_INT)
                    info->indexed_hival = (int)hival_obj->integer;
                else if (hival_obj->type == PDF_OBJ_REAL)
                    info->indexed_hival = (int)hival_obj->real;
            }
        }
        if (info->indexed_hival > PDF_MAX_INDEXED_PALETTE - 1)
            info->indexed_hival = PDF_MAX_INDEXED_PALETTE - 1;

        /* lookup table: can be a string or a stream */
        PdfObj *lookup_obj = pdf_array_get(arr, 3);
        if (lookup_obj) lookup_obj = pdf_resolve(doc, lookup_obj);
        if (lookup_obj) {
            int bpc = info->indexed_base_components;
            int palette_size = (info->indexed_hival + 1) * bpc;
            if (palette_size > (int)sizeof(info->indexed_palette))
                palette_size = (int)sizeof(info->indexed_palette);

            if (lookup_obj->type == PDF_OBJ_STRING) {
                int copy_len = (int)lookup_obj->string.length;
                if (copy_len > palette_size) copy_len = palette_size;
                memcpy(info->indexed_palette, lookup_obj->string.data, copy_len);
            } else if (lookup_obj->type == PDF_OBJ_STREAM) {
                if (pdf_decode_stream(doc, lookup_obj->stream)) {
                    int copy_len = (int)lookup_obj->stream->decoded_length;
                    if (copy_len > palette_size) copy_len = palette_size;
                    memcpy(info->indexed_palette, lookup_obj->stream->decoded_data, copy_len);
                }
            }
        }
        return;
    }

    /* ── Separation ── [/Separation name alternateSpace tintTransform] */
    if (strcmp(family, "Separation") == 0 && pdf_array_len(arr) >= 3) {
        info->type = PDF_CS_SEPARATION;
        info->components = 1;  /* Separation always takes 1 tint value */
        /* We approximate by checking the alternate space component count.
         * A full implementation would evaluate the tint transform function. */
        if (pdf_array_len(arr) >= 3) {
            PdfObj *alt_obj = pdf_array_get(arr, 2);
            if (alt_obj) alt_obj = pdf_resolve(doc, alt_obj);
            if (alt_obj && alt_obj->type == PDF_OBJ_NAME) {
                if (strcmp(alt_obj->name, "DeviceCMYK") == 0) {
                    info->indexed_base_type = PDF_CS_DEVICE_CMYK;
                    info->indexed_base_components = 4;
                } else if (strcmp(alt_obj->name, "DeviceRGB") == 0) {
                    info->indexed_base_type = PDF_CS_DEVICE_RGB;
                    info->indexed_base_components = 3;
                } else {
                    info->indexed_base_type = PDF_CS_DEVICE_GRAY;
                    info->indexed_base_components = 1;
                }
            }
        }
        return;
    }

    /* ── DeviceN ── [/DeviceN names alternateSpace tintTransform] */
    if (strcmp(family, "DeviceN") == 0 && pdf_array_len(arr) >= 3) {
        info->type = PDF_CS_DEVICEN;
        /* Component count = length of the names array */
        PdfObj *names_obj = pdf_array_get(arr, 1);
        if (names_obj) names_obj = pdf_resolve(doc, names_obj);
        if (names_obj && names_obj->type == PDF_OBJ_ARRAY) {
            info->components = pdf_array_len(names_obj->array);
        } else {
            info->components = 1;
        }
        return;
    }

    /* ── CalGray / CalRGB / Lab ── */
    if (strcmp(family, "CalGray") == 0) {
        info->type = PDF_CS_DEVICE_GRAY;
        info->components = 1;
        return;
    }
    if (strcmp(family, "CalRGB") == 0) {
        info->type = PDF_CS_DEVICE_RGB;
        info->components = 3;
        return;
    }
    if (strcmp(family, "Lab") == 0) {
        info->type = PDF_CS_DEVICE_RGB;
        info->components = 3;
        return;
    }
}

/*
 * Apply color from scn/SCN operands using the current color space info.
 * Returns the resolved PdfColor (RGB).
 */
static PdfColor resolve_color_from_cs(PdfColorSpaceInfo *cs, OperandStack *ops)
{
    PdfColor color = {0.0, 0.0, 0.0};

    switch (cs->type) {
    case PDF_CS_DEVICE_GRAY: {
        double gray = opstack_number(ops, ops->count - 1);
        color.r = color.g = color.b = gray;
        break;
    }
    case PDF_CS_DEVICE_RGB: {
        if (ops->count >= 3) {
            color.r = opstack_number(ops, ops->count - 3);
            color.g = opstack_number(ops, ops->count - 2);
            color.b = opstack_number(ops, ops->count - 1);
        }
        break;
    }
    case PDF_CS_DEVICE_CMYK: {
        if (ops->count >= 4) {
            double c_ = opstack_number(ops, ops->count - 4);
            double m_ = opstack_number(ops, ops->count - 3);
            double y_ = opstack_number(ops, ops->count - 2);
            double k_ = opstack_number(ops, ops->count - 1);
            color = cmyk_to_rgb(c_, m_, y_, k_);
        }
        break;
    }
    case PDF_CS_ICCBASED: {
        /* Treat like the device space with the same component count */
        if (cs->components == 1) {
            double gray = opstack_number(ops, ops->count - 1);
            color.r = color.g = color.b = gray;
        } else if (cs->components == 3 && ops->count >= 3) {
            color.r = opstack_number(ops, ops->count - 3);
            color.g = opstack_number(ops, ops->count - 2);
            color.b = opstack_number(ops, ops->count - 1);
        } else if (cs->components == 4 && ops->count >= 4) {
            double c_ = opstack_number(ops, ops->count - 4);
            double m_ = opstack_number(ops, ops->count - 3);
            double y_ = opstack_number(ops, ops->count - 2);
            double k_ = opstack_number(ops, ops->count - 1);
            color = cmyk_to_rgb(c_, m_, y_, k_);
        }
        break;
    }
    case PDF_CS_INDEXED: {
        /* Operand is a palette index */
        int idx = (int)opstack_number(ops, ops->count - 1);
        if (idx < 0) idx = 0;
        if (idx > cs->indexed_hival) idx = cs->indexed_hival;
        int bpc = cs->indexed_base_components;
        if (bpc < 1) bpc = 3; /* fallback */
        int offset = idx * bpc;

        if (cs->indexed_base_type == PDF_CS_DEVICE_CMYK && bpc == 4) {
            double c_ = cs->indexed_palette[offset + 0] / 255.0;
            double m_ = cs->indexed_palette[offset + 1] / 255.0;
            double y_ = cs->indexed_palette[offset + 2] / 255.0;
            double k_ = cs->indexed_palette[offset + 3] / 255.0;
            color = cmyk_to_rgb(c_, m_, y_, k_);
        } else if (cs->indexed_base_type == PDF_CS_DEVICE_GRAY && bpc == 1) {
            double gray = cs->indexed_palette[offset] / 255.0;
            color.r = color.g = color.b = gray;
        } else {
            /* RGB or ICCBased with 3 components */
            color.r = cs->indexed_palette[offset + 0] / 255.0;
            color.g = cs->indexed_palette[offset + 1] / 255.0;
            color.b = cs->indexed_palette[offset + 2] / 255.0;
        }
        break;
    }
    case PDF_CS_SEPARATION: {
        /* Approximation: use tint value as gray, or as intensity */
        double tint = opstack_number(ops, ops->count - 1);
        /* Without evaluating the tint transform, approximate:
         * For Separation spaces with CMYK alternate, use tint as key (darkness) */
        if (cs->indexed_base_type == PDF_CS_DEVICE_CMYK) {
            /* Approximate: treat as all-zero CMYK with the tint applied uniformly
             * This is a rough heuristic; a real solution evaluates the function */
            double gray = 1.0 - tint;
            color.r = color.g = color.b = gray;
        } else {
            /* Generic fallback: tint 0 = white, tint 1 = black */
            double gray = 1.0 - tint;
            color.r = color.g = color.b = gray;
        }
        break;
    }
    case PDF_CS_DEVICEN: {
        /* Rough approximation: treat multiple components as CMYK or ignore.
         * For now, use the operand-count-based fallback. */
        if (ops->count >= 4) {
            double c_ = opstack_number(ops, ops->count - 4);
            double m_ = opstack_number(ops, ops->count - 3);
            double y_ = opstack_number(ops, ops->count - 2);
            double k_ = opstack_number(ops, ops->count - 1);
            color = cmyk_to_rgb(c_, m_, y_, k_);
        } else if (ops->count >= 3) {
            color.r = opstack_number(ops, ops->count - 3);
            color.g = opstack_number(ops, ops->count - 2);
            color.b = opstack_number(ops, ops->count - 1);
        } else if (ops->count >= 1) {
            double gray = opstack_number(ops, ops->count - 1);
            color.r = color.g = color.b = gray;
        }
        break;
    }
    default:
        /* Fall through to operand-count guessing below */
        if (ops->count >= 4) {
            double c_ = opstack_number(ops, ops->count - 4);
            double m_ = opstack_number(ops, ops->count - 3);
            double y_ = opstack_number(ops, ops->count - 2);
            double k_ = opstack_number(ops, ops->count - 1);
            color = cmyk_to_rgb(c_, m_, y_, k_);
        } else if (ops->count >= 3) {
            color.r = opstack_number(ops, ops->count - 3);
            color.g = opstack_number(ops, ops->count - 2);
            color.b = opstack_number(ops, ops->count - 1);
        } else if (ops->count >= 1) {
            double gray = opstack_number(ops, ops->count - 1);
            color.r = color.g = color.b = gray;
        }
        break;
    }
    return color;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Coordinate Transform: PDF user space -> GDI device pixels
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Transform a point through the current CTM and the initial page transform.
 * PDF: origin bottom-left, Y up.
 * GDI: origin top-left, Y down.
 *
 * The initial CTM already includes the Y-flip and scaling, so we just
 * need to apply the full CTM to get device coordinates.
 */
static void transform_point(PdfRenderCtx *ctx, double x, double y,
                             int *dx, int *dy)
{
    PdfMatrix m = current_gs(ctx)->ctm;
    double ox, oy;
    pdf_transform_point(m, x, y, &ox, &oy);
    *dx = (int)(ox + 0.5);
    *dy = (int)(oy + 0.5);
}

/* Same as transform_point but returns double-precision device coordinates.
 * Used by the software rasterizer path to avoid integer truncation. */
static void transform_point_d(PdfRenderCtx *ctx, double x, double y,
                               double *dx, double *dy)
{
    PdfMatrix m = current_gs(ctx)->ctm;
    pdf_transform_point(m, x, y, dx, dy);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Path Building
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * We accumulate path segments and then paint them using GDI's path API.
 * GDI's BeginPath/EndPath/StrokePath/FillPath maps well to PDF's model.
 */

/* Maximum points in a path */
#define MAX_PATH_POINTS 16384

typedef struct {
    double x, y;    /* double-precision device coordinates */
} DPoint;

typedef struct {
    POINT   pts[MAX_PATH_POINTS];
    DPoint  dpts[MAX_PATH_POINTS];  /* double-precision device coordinates (for software rasterizer) */
    BYTE    types[MAX_PATH_POINTS]; /* PT_MOVETO, PT_LINETO, PT_BEZIERTO */
    int     count;
    double  cur_x, cur_y;    /* current point in user space */
    double  start_x, start_y; /* start of current subpath */
    bool    has_current;
} PathBuilder;

static void path_reset(PathBuilder *pb)
{
    pb->count = 0;
    pb->cur_x = pb->cur_y = 0.0;
    pb->start_x = pb->start_y = 0.0;
    pb->has_current = false;
}

static void path_add_point(PathBuilder *pb, PdfRenderCtx *ctx,
                            double x, double y, BYTE type)
{
    if (pb->count >= MAX_PATH_POINTS) return;
    int dx, dy;
    double ddx, ddy;
    transform_point(ctx, x, y, &dx, &dy);
    transform_point_d(ctx, x, y, &ddx, &ddy);
    pb->pts[pb->count].x = dx;
    pb->pts[pb->count].y = dy;
    pb->dpts[pb->count].x = ddx;
    pb->dpts[pb->count].y = ddy;
    pb->types[pb->count] = type;
    pb->count++;
}

static void path_moveto(PathBuilder *pb, PdfRenderCtx *ctx, double x, double y)
{
    path_add_point(pb, ctx, x, y, PT_MOVETO);
    pb->cur_x = x;
    pb->cur_y = y;
    pb->start_x = x;
    pb->start_y = y;
    pb->has_current = true;
}

static void path_lineto(PathBuilder *pb, PdfRenderCtx *ctx, double x, double y)
{
    if (!pb->has_current) {
        path_moveto(pb, ctx, x, y);
        return;
    }
    path_add_point(pb, ctx, x, y, PT_LINETO);
    pb->cur_x = x;
    pb->cur_y = y;
}

static void path_curveto(PathBuilder *pb, PdfRenderCtx *ctx,
                          double x1, double y1, double x2, double y2,
                          double x3, double y3)
{
    if (!pb->has_current) {
        path_moveto(pb, ctx, x1, y1);
    }
    path_add_point(pb, ctx, x1, y1, PT_BEZIERTO);
    path_add_point(pb, ctx, x2, y2, PT_BEZIERTO);
    path_add_point(pb, ctx, x3, y3, PT_BEZIERTO);
    pb->cur_x = x3;
    pb->cur_y = y3;
}

static void path_closepath(PathBuilder *pb, PdfRenderCtx *ctx)
{
    if (!pb->has_current) return;
    /* Add a line back to the start and mark with PT_CLOSEFIGURE */
    if (pb->count > 0 && pb->count < MAX_PATH_POINTS) {
        path_add_point(pb, ctx, pb->start_x, pb->start_y, PT_LINETO | PT_CLOSEFIGURE);
    }
    pb->cur_x = pb->start_x;
    pb->cur_y = pb->start_y;
}

static void path_rect(PathBuilder *pb, PdfRenderCtx *ctx,
                       double x, double y, double w, double h)
{
    path_moveto(pb, ctx, x, y);
    path_lineto(pb, ctx, x + w, y);
    path_lineto(pb, ctx, x + w, y + h);
    path_lineto(pb, ctx, x, y + h);
    path_closepath(pb, ctx);
}

/*
 * Helper: compute bounding box of the double-precision path points.
 * Returns false if the path is empty.
 */
static bool path_bbox(PathBuilder *pb, double *min_x, double *min_y,
                       double *max_x, double *max_y)
{
    if (pb->count == 0) return false;

    *min_x = 1e30;  *min_y = 1e30;
    *max_x = -1e30; *max_y = -1e30;

    for (int i = 0; i < pb->count; i++) {
        double px = pb->dpts[i].x;
        double py = pb->dpts[i].y;
        if (px < *min_x) *min_x = px;
        if (py < *min_y) *min_y = py;
        if (px > *max_x) *max_x = px;
        if (py > *max_y) *max_y = py;
    }
    return (*max_x >= *min_x) && (*max_y >= *min_y);
}

/*
 * Helper: get the page bitmap's raw pixel data for software blending.
 * Returns the pixel pointer and fills in page dimensions + stride.
 * Falls back to a temporary DIB if the HDC bitmap is not a DIB section.
 */
static uint8_t *get_page_bits(HDC hdc, int *page_w, int *page_h, int *page_stride)
{
    HBITMAP hbm = (HBITMAP)GetCurrentObject(hdc, OBJ_BITMAP);
    if (!hbm) return NULL;

    DIBSECTION ds;
    if (GetObject(hbm, sizeof(DIBSECTION), &ds) == 0 || !ds.dsBm.bmBits)
        return NULL;

    *page_w = ds.dsBm.bmWidth;
    *page_h = abs(ds.dsBm.bmHeight);
    *page_stride = ds.dsBm.bmWidthBytes;
    return (uint8_t *)ds.dsBm.bmBits;
}

/*
 * Software AA fill: replay path into rasterizer and blend onto page bitmap.
 * Returns true on success, false if fallback to GDI is needed.
 */
static bool path_aa_fill(PathBuilder *pb, PdfRenderCtx *ctx, int ops)
{
    PROF_START(fill);

    /* Compute bounding box of the path in device pixels */
    double bmin_x, bmin_y, bmax_x, bmax_y;
    if (!path_bbox(pb, &bmin_x, &bmin_y, &bmax_x, &bmax_y))
        { PROF_END(fill, fill); return false; }

    /* Add 3px padding for anti-aliasing edges + 1px dilation bleed */
    bmin_x = floor(bmin_x) - 3.0;
    bmin_y = floor(bmin_y) - 3.0;
    bmax_x = ceil(bmax_x) + 3.0;
    bmax_y = ceil(bmax_y) + 3.0;

    int rw = (int)(bmax_x - bmin_x);
    int rh = (int)(bmax_y - bmin_y);

    /* Sanity check: rasterizer has size limits */
    if (rw <= 0 || rh <= 0) { PROF_END(fill, fill); return false; }
    if (rw > 16384 || rh > 16384) { PROF_END(fill, fill); return false; }

    RasterCtx *rctx = raster_create(rw, rh);
    if (!rctx) { PROF_END(fill, fill); return false; }

    /* Replay path through rasterizer using double coordinates */
    for (int i = 0; i < pb->count; i++) {
        BYTE t = pb->types[i] & ~PT_CLOSEFIGURE;
        bool close = (pb->types[i] & PT_CLOSEFIGURE) != 0;

        double px = pb->dpts[i].x - bmin_x;
        double py = pb->dpts[i].y - bmin_y;

        if (t == PT_MOVETO) {
            raster_move_to(rctx, px, py);
        } else if (t == PT_LINETO) {
            raster_line_to(rctx, px, py);
        } else if (t == PT_BEZIERTO) {
            if (i + 2 < pb->count) {
                double cx1 = pb->dpts[i].x - bmin_x;
                double cy1 = pb->dpts[i].y - bmin_y;
                double cx2 = pb->dpts[i+1].x - bmin_x;
                double cy2 = pb->dpts[i+1].y - bmin_y;
                double ex  = pb->dpts[i+2].x - bmin_x;
                double ey  = pb->dpts[i+2].y - bmin_y;
                raster_curve_to(rctx, cx1, cy1, cx2, cy2, ex, ey);
                i += 2;
            }
        }

        if (close) {
            raster_close(rctx);
        }
    }

    /* Compute coverage with the appropriate fill rule */
    if (ops & 4) {
        raster_finish_evenodd(rctx);
    } else {
        raster_finish(rctx);
    }

    /* Get page bitmap pixels */
    HDC hdc = ctx->hdc;
    int page_w, page_h, page_stride;
    uint8_t *page_bits = get_page_bits(hdc, &page_w, &page_h, &page_stride);

    PdfGraphicsState *gs = current_gs(ctx);
    COLORREF fill_col = pdf_color_to_gdi(gs->fill_color);
    int fr = GetRValue(fill_col);
    int fg = GetGValue(fill_col);
    int fb = GetBValue(fill_col);

    int dest_x = (int)bmin_x;
    int dest_y = (int)bmin_y;

    /* Get the software clip mask (if any) for rasterizer clipping */
    const uint8_t *cmask = current_clip_mask(ctx);
    int cmask_w = ctx->clip_mask_w;
    int cmask_h = ctx->clip_mask_h;

    if (page_bits) {
        /* Direct pixel access to the page bitmap */
        GdiFlush();
        raster_blend_clipped(rctx, page_bits, page_stride, page_w, page_h,
                             dest_x, dest_y, fr, fg, fb,
                             cmask, cmask_w, cmask_h, dest_x, dest_y);
    } else {
        /* Fallback: read pixels via BitBlt, blend, write back */
        BITMAPINFO out_bmi;
        memset(&out_bmi, 0, sizeof(out_bmi));
        out_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        out_bmi.bmiHeader.biWidth       = rw;
        out_bmi.bmiHeader.biHeight      = -rh;
        out_bmi.bmiHeader.biPlanes      = 1;
        out_bmi.bmiHeader.biBitCount    = 32;
        out_bmi.bmiHeader.biCompression = BI_RGB;

        uint8_t *out_bits = NULL;
        HDC out_dc = CreateCompatibleDC(hdc);
        HBITMAP out_bmp = CreateDIBSection(out_dc, &out_bmi, DIB_RGB_COLORS,
                                            (void **)&out_bits, NULL, 0);
        if (out_bmp && out_bits) {
            HBITMAP old_bmp = (HBITMAP)SelectObject(out_dc, out_bmp);
            BitBlt(out_dc, 0, 0, rw, rh, hdc, dest_x, dest_y, SRCCOPY);
            GdiFlush();

            int stride = rw * 4;
            raster_blend_clipped(rctx, out_bits, stride, rw, rh, 0, 0, fr, fg, fb,
                                 cmask, cmask_w, cmask_h, dest_x, dest_y);

            BitBlt(hdc, dest_x, dest_y, rw, rh, out_dc, 0, 0, SRCCOPY);

            SelectObject(out_dc, old_bmp);
            DeleteObject(out_bmp);
        }
        DeleteDC(out_dc);
    }

    raster_free(rctx);
    PROF_END(fill, fill);
    return true;
}

/*
 * Software AA stroke: expand the path outline by line_width and render as a fill.
 * Converts each line segment to a filled rectangle, handling line joins.
 * Returns true on success, false if fallback to GDI is needed.
 */
static bool path_aa_stroke(PathBuilder *pb, PdfRenderCtx *ctx)
{
    PROF_START(stroke);

    PdfGraphicsState *gs = current_gs(ctx);

    /* Compute line width in device pixels */
    double lw = gs->line_width;
    PdfMatrix m = gs->ctm;
    double scale_factor = sqrt(fabs(m.a * m.d - m.b * m.c));
    double dev_width = lw * scale_factor;
    if (dev_width < 0.5) dev_width = 0.5; /* minimum visible stroke */

    double half_w = dev_width * 0.5;

    /* Compute bounding box, expanded by half the stroke width + padding */
    double bmin_x, bmin_y, bmax_x, bmax_y;
    if (!path_bbox(pb, &bmin_x, &bmin_y, &bmax_x, &bmax_y))
        { PROF_END(stroke, stroke); return false; }

    bmin_x = floor(bmin_x - half_w) - 2.0;
    bmin_y = floor(bmin_y - half_w) - 2.0;
    bmax_x = ceil(bmax_x + half_w) + 2.0;
    bmax_y = ceil(bmax_y + half_w) + 2.0;

    int rw = (int)(bmax_x - bmin_x);
    int rh = (int)(bmax_y - bmin_y);

    if (rw <= 0 || rh <= 0) { PROF_END(stroke, stroke); return false; }
    if (rw > 16384 || rh > 16384) { PROF_END(stroke, stroke); return false; }

    RasterCtx *rctx = raster_create(rw, rh);
    if (!rctx) { PROF_END(stroke, stroke); return false; }

    /* Walk the path and expand each line/curve segment into a stroked outline.
     * For each segment we create a filled rectangle (for lines) or
     * use a simplified approach for curves (flatten + stroke each sub-segment).
     *
     * Strategy: we iterate through segments. For each line (x0,y0)->(x1,y1):
     *   1. Compute the perpendicular offset vector (nx, ny) * half_w
     *   2. Create a quad from the 4 corners
     *   3. Feed the quad into the rasterizer as a closed subpath
     *
     * For Bezier curves: we tessellate them into short line segments first,
     * then stroke each sub-segment. This reuses the rasterizer's bezier
     * flattening by converting curves to a polyline first.
     */

    /* First, flatten the path into a list of line segments with subpath info */
    #define MAX_FLAT_POINTS 32768
    double *flat_x = (double *)malloc(MAX_FLAT_POINTS * sizeof(double));
    double *flat_y = (double *)malloc(MAX_FLAT_POINTS * sizeof(double));
    int *flat_cmd = (int *)malloc(MAX_FLAT_POINTS * sizeof(int)); /* 0=move, 1=line, 2=close */
    if (!flat_x || !flat_y || !flat_cmd) {
        free(flat_x); free(flat_y); free(flat_cmd);
        raster_free(rctx);
        PROF_END(stroke, stroke); return false;
    }

    int flat_count = 0;

    /* Flatten curves into line segments */
    for (int i = 0; i < pb->count; i++) {
        if (flat_count >= MAX_FLAT_POINTS - 1) break;

        BYTE t = pb->types[i] & ~PT_CLOSEFIGURE;
        bool close = (pb->types[i] & PT_CLOSEFIGURE) != 0;

        if (t == PT_MOVETO) {
            flat_x[flat_count] = pb->dpts[i].x;
            flat_y[flat_count] = pb->dpts[i].y;
            flat_cmd[flat_count] = 0;
            flat_count++;
        } else if (t == PT_LINETO) {
            flat_x[flat_count] = pb->dpts[i].x;
            flat_y[flat_count] = pb->dpts[i].y;
            flat_cmd[flat_count] = close ? 2 : 1;
            flat_count++;
        } else if (t == PT_BEZIERTO && i + 2 < pb->count) {
            /* Flatten cubic bezier into line segments.
             * Use De Casteljau subdivision until segments are short enough. */
            double cx1 = pb->dpts[i].x,   cy1 = pb->dpts[i].y;
            double cx2 = pb->dpts[i+1].x, cy2 = pb->dpts[i+1].y;
            double ex  = pb->dpts[i+2].x, ey  = pb->dpts[i+2].y;

            /* Find the start point (last point before this bezier) */
            double sx = 0, sy = 0;
            if (flat_count > 0) {
                sx = flat_x[flat_count - 1];
                sy = flat_y[flat_count - 1];
            }

            /* Simple recursive flattening with fixed step count.
             * Subdivide into ~16 line segments. */
            int n_steps = 16;
            for (int step = 1; step <= n_steps && flat_count < MAX_FLAT_POINTS; step++) {
                double t_param = (double)step / (double)n_steps;
                double inv = 1.0 - t_param;
                double inv2 = inv * inv;
                double inv3 = inv2 * inv;
                double t2 = t_param * t_param;
                double t3 = t2 * t_param;

                double bx = inv3 * sx + 3.0 * inv2 * t_param * cx1 +
                            3.0 * inv * t2 * cx2 + t3 * ex;
                double by = inv3 * sy + 3.0 * inv2 * t_param * cy1 +
                            3.0 * inv * t2 * cy2 + t3 * ey;

                flat_x[flat_count] = bx;
                flat_y[flat_count] = by;
                flat_cmd[flat_count] = (step == n_steps && close) ? 2 : 1;
                flat_count++;
            }
            i += 2; /* skip the extra 2 bezier control points */
        }
    }

    /* ── Apply dash pattern ──
     * When a dash pattern is active, replace the flattened path with
     * dashed sub-segments. Dash lengths are in user space, scaled by CTM.
     * Per PDF spec, odd-length dash arrays are doubled (e.g. [8] -> [8 8]). */
    if (gs->dash_count > 0) {
        /* Normalize: if odd count, double the array so dash/gap alternate */
        double local_dash[20];
        int local_count = gs->dash_count;
        for (int di = 0; di < local_count; di++)
            local_dash[di] = gs->dash_array[di];
        if (local_count % 2 != 0) {
            for (int di = 0; di < gs->dash_count; di++)
                local_dash[local_count + di] = gs->dash_array[di];
            local_count *= 2;
        }

        double dash_total = 0;
        for (int di = 0; di < local_count; di++)
            dash_total += local_dash[di] * scale_factor;
        if (dash_total < 0.01) dash_total = 1.0;

        double *dash_x = (double *)malloc(MAX_FLAT_POINTS * 2 * sizeof(double));
        double *dash_y = (double *)malloc(MAX_FLAT_POINTS * 2 * sizeof(double));
        int *dash_cmd = (int *)malloc(MAX_FLAT_POINTS * 2 * sizeof(int));
        if (!dash_x || !dash_y || !dash_cmd) {
            free(dash_x); free(dash_y); free(dash_cmd);
            free(flat_x); free(flat_y); free(flat_cmd);
            raster_free(rctx);
            PROF_END(stroke, stroke); return false;
        }
        int dash_out = 0;
        int max_dash_pts = MAX_FLAT_POINTS * 2;

        int dash_idx = 0;
        double dash_remaining = local_dash[0] * scale_factor;
        bool dash_on = true;

        /* Apply phase offset */
        double phase = gs->dash_phase * scale_factor;
        while (phase > 0 && dash_total > 0) {
            if (phase >= dash_remaining) {
                phase -= dash_remaining;
                dash_idx = (dash_idx + 1) % local_count;
                dash_remaining = local_dash[dash_idx] * scale_factor;
                dash_on = (dash_idx % 2 == 0);
            } else {
                dash_remaining -= phase;
                phase = 0;
            }
        }

        int init_dash_idx = dash_idx;
        double init_dash_remaining = dash_remaining;
        bool init_dash_on = dash_on;

        double seg_prev_x = 0, seg_prev_y = 0;
        bool need_move = true;

        for (int i = 0; i < flat_count; i++) {
            if (flat_cmd[i] == 0) {
                seg_prev_x = flat_x[i];
                seg_prev_y = flat_y[i];
                need_move = true;
                dash_idx = init_dash_idx;
                dash_remaining = init_dash_remaining;
                dash_on = init_dash_on;
                continue;
            }

            double x0 = seg_prev_x, y0 = seg_prev_y;
            double x1 = flat_x[i], y1 = flat_y[i];
            double sdx = x1 - x0, sdy = y1 - y0;
            double seg_len = sqrt(sdx * sdx + sdy * sdy);

            if (seg_len < 1e-10) {
                seg_prev_x = x1; seg_prev_y = y1;
                continue;
            }

            double ux = sdx / seg_len, uy = sdy / seg_len;
            double consumed = 0;

            while (consumed < seg_len - 1e-10 && dash_out < max_dash_pts - 2) {
                double avail = seg_len - consumed;
                double dstep = (dash_remaining < avail) ? dash_remaining : avail;

                double start_x = x0 + ux * consumed;
                double start_y = y0 + uy * consumed;
                double end_x = x0 + ux * (consumed + dstep);
                double end_y = y0 + uy * (consumed + dstep);

                if (dash_on) {
                    if (need_move) {
                        dash_x[dash_out] = start_x;
                        dash_y[dash_out] = start_y;
                        dash_cmd[dash_out] = 0;
                        dash_out++;
                        need_move = false;
                    }
                    if (dash_out < max_dash_pts) {
                        dash_x[dash_out] = end_x;
                        dash_y[dash_out] = end_y;
                        dash_cmd[dash_out] = 1;
                        dash_out++;
                    }
                }

                consumed += dstep;
                dash_remaining -= dstep;

                if (dash_remaining < 1e-10) {
                    if (dash_on) need_move = true;
                    dash_idx = (dash_idx + 1) % local_count;
                    dash_remaining = local_dash[dash_idx] * scale_factor;
                    dash_on = (dash_idx % 2 == 0);
                    if (!dash_on) need_move = true;
                }
            }

            seg_prev_x = x1; seg_prev_y = y1;
        }

        free(flat_x); free(flat_y); free(flat_cmd);
        flat_x = dash_x;
        flat_y = dash_y;
        flat_cmd = dash_cmd;
        flat_count = dash_out;
    }

    /* Build stroke outline as a SINGLE continuous polygon per subpath.
     * Instead of rendering each segment as a separate quad (which creates
     * visible seam cracks at junction points due to AA), trace the left
     * edge forward and right edge backward as one closed polygon.
     *
     * For each subpath: collect all segment endpoints with their
     * perpendicular offsets, then emit:
     *   start_cap + left_edge[0..n] + end_cap + right_edge[n..0] + close
     */
    double prev_x = 0, prev_y = 0;
    double subpath_start_x = 0, subpath_start_y = 0;

    /* Temporary arrays for left and right edge points.
     * Allocate 2x to accommodate implicit closing segments for closed subpaths. */
    double *left_x  = (double *)malloc(flat_count * 2 * sizeof(double));
    double *left_y  = (double *)malloc(flat_count * 2 * sizeof(double));
    double *right_x = (double *)malloc(flat_count * 2 * sizeof(double));
    double *right_y = (double *)malloc(flat_count * 2 * sizeof(double));
    if (!left_x || !left_y || !right_x || !right_y) {
        free(left_x); free(left_y); free(right_x); free(right_y);
        free(flat_x); free(flat_y); free(flat_cmd);
        raster_free(rctx);
        PROF_END(stroke, stroke); return false;
    }

    int edge_count = 0;
    int subpath_edge_start = 0;
    bool in_subpath = false;
    /* Direction of first and last segments (for caps) */
    double first_dir_x = 0, first_dir_y = 0;
    double last_dir_x = 0, last_dir_y = 0;
    /* Previous segment direction for miter join computation */
    double prev_seg_ux = 0, prev_seg_uy = 0;
    bool have_prev_seg = false;
    /* First segment direction (for closed path miter at start) */
    double first_seg_ux = 0, first_seg_uy = 0;

    for (int i = 0; i <= flat_count; i++) {
        bool is_end = (i >= flat_count);
        bool is_move = (!is_end && flat_cmd[i] == 0);
        bool flush = (is_end || is_move) && in_subpath && edge_count > subpath_edge_start;

        if (flush) {
            /* Emit the stroke outline for this subpath as a single polygon */
            int n = edge_count - subpath_edge_start;
            int s = subpath_edge_start;
            bool is_closed = (!is_end && !is_move) ? (flat_cmd[i] == 2) : false;
            /* Check if last point before flush was a close */
            if (i > 0 && i <= flat_count && flat_cmd[i-1] == 2) is_closed = true;
            bool is_open = !is_closed;

            /* For closed subpaths, apply miter join between last and first segments.
             * The last edge point (edge[s+n-1]) is at the subpath start position,
             * and edge[s] is also at the subpath start. Both need mitering with
             * the last segment's direction and the first segment's direction. */
            if (is_closed && n >= 3) {
                double mlimit = gs->miter_limit;
                double cos_theta = last_dir_x * first_seg_ux + last_dir_y * first_seg_uy;
                double denom = 1.0 + cos_theta;
                if (denom > 1e-6) {
                    double miter_ratio_sq = 2.0 / denom;
                    if (miter_ratio_sq <= mlimit * mlimit) {
                        /* Compute miter offset at subpath start/end junction */
                        double n_last_x = -last_dir_y * half_w;
                        double n_last_y =  last_dir_x * half_w;
                        double n_first_x = -first_seg_uy * half_w;
                        double n_first_y =  first_seg_ux * half_w;
                        double mx = (n_last_x + n_first_x) / denom;
                        double my = (n_last_y + n_first_y) / denom;
                        /* Junction position (subpath start in raster-local coords) */
                        double jx = subpath_start_x - bmin_x;
                        double jy = subpath_start_y - bmin_y;
                        /* Update first edge point */
                        left_x[s]  = jx + mx; left_y[s]  = jy + my;
                        right_x[s] = jx - mx; right_y[s] = jy - my;
                        /* Update last edge point (same position) */
                        left_x[s+n-1]  = jx + mx; left_y[s+n-1]  = jy + my;
                        right_x[s+n-1] = jx - mx; right_y[s+n-1] = jy - my;
                    }
                }
            }

            /* Start the polygon: begin at left edge of first point */
            if (is_open && gs->line_cap == 1 && n >= 1) {
                /* Round start cap: semicircle from right→backward→left */
                int n_half = (int)(half_w * 3.0);
                if (n_half < 6) n_half = 6;
                if (n_half > 32) n_half = 32;
                raster_move_to(rctx, right_x[s], right_y[s]);
                for (int c = 1; c <= n_half; c++) {
                    double t = 3.14159265358979323846 * c / n_half;
                    double px = left_x[s] - right_x[s];
                    double py = left_y[s] - right_y[s];
                    double mx = (left_x[s] + right_x[s]) * 0.5;
                    double my = (left_y[s] + right_y[s]) * 0.5;
                    /* Semicircle center is at the path point (midpoint of left/right) */
                    double cos_t = cos(t);
                    double sin_t = sin(t);
                    /* Rotate from right edge through backward to left edge */
                    double rx = (right_x[s] - mx);
                    double ry = (right_y[s] - my);
                    double bx = -first_dir_x * half_w;
                    double by = -first_dir_y * half_w;
                    /* Parametric: interpolate angle from right-edge to left-edge via backward */
                    double ax = rx * cos_t + bx * sin_t;
                    double ay = ry * cos_t + by * sin_t;
                    raster_line_to(rctx, mx + ax, my + ay);
                }
                raster_line_to(rctx, left_x[s], left_y[s]);
            } else if (is_open && gs->line_cap == 2 && n >= 1) {
                /* Square start cap */
                raster_move_to(rctx, right_x[s] - first_dir_x * half_w,
                               right_y[s] - first_dir_y * half_w);
                raster_line_to(rctx, left_x[s] - first_dir_x * half_w,
                               left_y[s] - first_dir_y * half_w);
                raster_line_to(rctx, left_x[s], left_y[s]);
            } else {
                raster_move_to(rctx, left_x[s], left_y[s]);
            }

            /* Trace left edge forward */
            for (int j = s + 1; j < s + n; j++) {
                raster_line_to(rctx, left_x[j], left_y[j]);
            }

            /* End cap or corner */
            int last = s + n - 1;
            if (is_open && gs->line_cap == 1 && n >= 1) {
                /* Round end cap: semicircle from left→forward→right */
                int n_half = (int)(half_w * 3.0);
                if (n_half < 6) n_half = 6;
                if (n_half > 32) n_half = 32;
                for (int c = 1; c <= n_half; c++) {
                    double t = 3.14159265358979323846 * c / n_half;
                    double mx = (left_x[last] + right_x[last]) * 0.5;
                    double my = (left_y[last] + right_y[last]) * 0.5;
                    double lx = (left_x[last] - mx);
                    double ly = (left_y[last] - my);
                    double fx = last_dir_x * half_w;
                    double fy = last_dir_y * half_w;
                    double ax = lx * cos(t) + fx * sin(t);
                    double ay = ly * cos(t) + fy * sin(t);
                    raster_line_to(rctx, mx + ax, my + ay);
                }
            } else if (is_open && gs->line_cap == 2 && n >= 1) {
                /* Square end cap */
                raster_line_to(rctx, left_x[last] + last_dir_x * half_w,
                               left_y[last] + last_dir_y * half_w);
                raster_line_to(rctx, right_x[last] + last_dir_x * half_w,
                               right_y[last] + last_dir_y * half_w);
            }

            /* Trace right edge backward */
            for (int j = s + n - 1; j >= s; j--) {
                raster_line_to(rctx, right_x[j], right_y[j]);
            }

            raster_close(rctx);

            edge_count = subpath_edge_start;
            have_prev_seg = false;
        }

        if (is_end) break;

        if (is_move) {
            prev_x = flat_x[i];
            prev_y = flat_y[i];
            subpath_start_x = prev_x;
            subpath_start_y = prev_y;
            subpath_edge_start = edge_count;
            in_subpath = true;
            have_prev_seg = false;
            continue;
        }

        /* Line or close segment: compute perpendicular offsets */
        double x0 = prev_x - bmin_x;
        double y0 = prev_y - bmin_y;
        double x1 = flat_x[i] - bmin_x;
        double y1 = flat_y[i] - bmin_y;

        double dx = x1 - x0;
        double dy = y1 - y0;
        double seg_len = sqrt(dx * dx + dy * dy);

        if (seg_len > 1e-10) {
            double ux = dx / seg_len;
            double uy = dy / seg_len;
            double nx = -uy * half_w;
            double ny = ux * half_w;

            if (edge_count == subpath_edge_start) {
                /* First segment: store both start and end points */
                left_x[edge_count]  = x0 + nx; left_y[edge_count]  = y0 + ny;
                right_x[edge_count] = x0 - nx; right_y[edge_count] = y0 - ny;
                first_dir_x = ux;
                first_dir_y = uy;
                first_seg_ux = ux;
                first_seg_uy = uy;
                edge_count++;
            } else if (have_prev_seg) {
                /* Junction between previous and current segment: apply miter join.
                 * Update the previous endpoint (edge[edge_count-1]) which sits at
                 * this junction point (x0, y0). */
                double cos_theta = prev_seg_ux * ux + prev_seg_uy * uy;
                double denom = 1.0 + cos_theta;
                if (denom > 1e-6) {
                    double miter_ratio_sq = 2.0 / denom;
                    if (miter_ratio_sq <= gs->miter_limit * gs->miter_limit) {
                        /* Compute miter offset */
                        double pnx = -prev_seg_uy * half_w;
                        double pny =  prev_seg_ux * half_w;
                        double mx = (pnx + nx) / denom;
                        double my = (pny + ny) / denom;
                        left_x[edge_count-1]  = x0 + mx;
                        left_y[edge_count-1]  = y0 + my;
                        right_x[edge_count-1] = x0 - mx;
                        right_y[edge_count-1] = y0 - my;
                    }
                }
            }
            /* Store end point of this segment */
            left_x[edge_count]  = x1 + nx; left_y[edge_count]  = y1 + ny;
            right_x[edge_count] = x1 - nx; right_y[edge_count] = y1 - ny;
            last_dir_x = ux;
            last_dir_y = uy;
            prev_seg_ux = ux;
            prev_seg_uy = uy;
            have_prev_seg = true;
            edge_count++;
        }

        prev_x = flat_x[i];
        prev_y = flat_y[i];
        if (flat_cmd[i] == 2) {
            prev_x = subpath_start_x;
            prev_y = subpath_start_y;
        }
    }

    free(left_x); free(left_y);
    free(right_x); free(right_y);

    free(flat_x);
    free(flat_y);
    free(flat_cmd);

    /* Compute coverage (strokes always use nonzero winding) */
    raster_finish(rctx);

    /* Blend onto page bitmap */
    HDC hdc = ctx->hdc;
    int page_w, page_h, page_stride;
    uint8_t *page_bits = get_page_bits(hdc, &page_w, &page_h, &page_stride);

    COLORREF stroke_col = pdf_color_to_gdi(gs->stroke_color);
    int sr = GetRValue(stroke_col);
    int sg = GetGValue(stroke_col);
    int sb = GetBValue(stroke_col);

    int dest_x = (int)bmin_x;
    int dest_y = (int)bmin_y;

    /* Get the software clip mask (if any) for rasterizer clipping */
    const uint8_t *cmask = current_clip_mask(ctx);
    int cmask_w = ctx->clip_mask_w;
    int cmask_h = ctx->clip_mask_h;

    if (page_bits) {
        GdiFlush();
        raster_blend_clipped(rctx, page_bits, page_stride, page_w, page_h,
                             dest_x, dest_y, sr, sg, sb,
                             cmask, cmask_w, cmask_h, dest_x, dest_y);
    } else {
        BITMAPINFO out_bmi;
        memset(&out_bmi, 0, sizeof(out_bmi));
        out_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        out_bmi.bmiHeader.biWidth       = rw;
        out_bmi.bmiHeader.biHeight      = -rh;
        out_bmi.bmiHeader.biPlanes      = 1;
        out_bmi.bmiHeader.biBitCount    = 32;
        out_bmi.bmiHeader.biCompression = BI_RGB;

        uint8_t *out_bits = NULL;
        HDC out_dc = CreateCompatibleDC(hdc);
        HBITMAP out_bmp = CreateDIBSection(out_dc, &out_bmi, DIB_RGB_COLORS,
                                            (void **)&out_bits, NULL, 0);
        if (out_bmp && out_bits) {
            HBITMAP old_bmp = (HBITMAP)SelectObject(out_dc, out_bmp);
            BitBlt(out_dc, 0, 0, rw, rh, hdc, dest_x, dest_y, SRCCOPY);
            GdiFlush();

            int stride = rw * 4;
            raster_blend_clipped(rctx, out_bits, stride, rw, rh, 0, 0, sr, sg, sb,
                                 cmask, cmask_w, cmask_h, dest_x, dest_y);

            BitBlt(hdc, dest_x, dest_y, rw, rh, out_dc, 0, 0, SRCCOPY);

            SelectObject(out_dc, old_bmp);
            DeleteObject(out_bmp);
        }
        DeleteDC(out_dc);
    }

    raster_free(rctx);
    PROF_END(stroke, stroke);
    return true;
}

/*
 * Render the accumulated path with anti-aliased software rasterization.
 * Falls back to GDI path rendering if the software path fails.
 * Operations: 1=stroke, 2=fill, 4=even-odd fill rule
 */
static void path_paint(PathBuilder *pb, PdfRenderCtx *ctx, int ops)
{
    if (pb->count == 0) {
        path_reset(pb);
        return;
    }

    /* Try software AA rendering first.
     * For fill+stroke, we do fill first then stroke on top. */
    bool fill_ok = true;
    bool stroke_ok = true;

    if (ops & 2) {
        fill_ok = path_aa_fill(pb, ctx, ops);
    }

    if (ops & 1) {
        stroke_ok = path_aa_stroke(pb, ctx);
    }

    /* If software rasterization succeeded, we're done */
    if (fill_ok && stroke_ok) {
        path_reset(pb);
        return;
    }

    /* ── GDI Fallback ── */
    PdfGraphicsState *gs = current_gs(ctx);
    HDC hdc = ctx->hdc;

    /* Create stroke pen */
    HPEN pen = NULL;
    HPEN old_pen = NULL;
    if (ops & 1) {
        double lw = gs->line_width;
        PdfMatrix m = gs->ctm;
        double scale_factor = sqrt(fabs(m.a * m.d - m.b * m.c));
        int pen_width = (int)(lw * scale_factor + 0.5);
        if (pen_width < 1) pen_width = 1;

        COLORREF stroke_col = pdf_color_to_gdi(gs->stroke_color);
        int pen_style = PS_SOLID;
        int end_cap = PS_ENDCAP_FLAT;
        if (gs->line_cap == 1) end_cap = PS_ENDCAP_ROUND;
        else if (gs->line_cap == 2) end_cap = PS_ENDCAP_SQUARE;
        int join = PS_JOIN_MITER;
        if (gs->line_join == 1) join = PS_JOIN_ROUND;
        else if (gs->line_join == 2) join = PS_JOIN_BEVEL;

        LOGBRUSH lb;
        lb.lbStyle = BS_SOLID;
        lb.lbColor = stroke_col;
        lb.lbHatch = 0;

        /* Dash pattern support for GDI fallback.
         * Odd-length arrays are doubled per PDF spec. */
        DWORD gdi_dash[20];
        int gdi_dash_count = 0;
        if (gs->dash_count > 0) {
            pen_style = PS_USERSTYLE;
            int src_count = gs->dash_count;
            for (int di = 0; di < src_count && di < 10; di++) {
                gdi_dash[di] = (DWORD)(gs->dash_array[di] * scale_factor + 0.5);
                if (gdi_dash[di] < 1) gdi_dash[di] = 1;
                gdi_dash_count++;
            }
            /* Double odd-length arrays */
            if (gdi_dash_count % 2 != 0 && gdi_dash_count <= 10) {
                for (int di = 0; di < gdi_dash_count; di++)
                    gdi_dash[gdi_dash_count + di] = gdi_dash[di];
                gdi_dash_count *= 2;
            }
        }

        pen = ExtCreatePen(PS_GEOMETRIC | pen_style | end_cap | join,
                           pen_width, &lb,
                           gdi_dash_count, gdi_dash_count > 0 ? gdi_dash : NULL);
        if (pen) old_pen = (HPEN)SelectObject(hdc, pen);
    }

    HBRUSH brush = NULL;
    HBRUSH old_brush = NULL;
    if (ops & 2) {
        COLORREF fill_col = pdf_color_to_gdi(gs->fill_color);
        brush = CreateSolidBrush(fill_col);
        if (brush) old_brush = (HBRUSH)SelectObject(hdc, brush);
    } else {
        old_brush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    }

    if (!(ops & 1)) {
        old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    }

    if (ops & 4) {
        SetPolyFillMode(hdc, ALTERNATE);
    } else {
        SetPolyFillMode(hdc, WINDING);
    }

    BeginPath(hdc);
    for (int i = 0; i < pb->count; i++) {
        BYTE t = pb->types[i] & ~PT_CLOSEFIGURE;
        bool close = (pb->types[i] & PT_CLOSEFIGURE) != 0;

        if (t == PT_MOVETO) {
            MoveToEx(hdc, pb->pts[i].x, pb->pts[i].y, NULL);
        } else if (t == PT_LINETO) {
            LineTo(hdc, pb->pts[i].x, pb->pts[i].y);
        } else if (t == PT_BEZIERTO) {
            if (i + 2 < pb->count) {
                POINT bpts[3] = { pb->pts[i], pb->pts[i+1], pb->pts[i+2] };
                PolyBezierTo(hdc, bpts, 3);
                i += 2;
            }
        }

        if (close) {
            CloseFigure(hdc);
        }
    }
    EndPath(hdc);

    if ((ops & 3) == 3) {
        StrokeAndFillPath(hdc);
    } else if (ops & 2) {
        FillPath(hdc);
    } else if (ops & 1) {
        StrokePath(hdc);
    }

    if (old_pen)  SelectObject(hdc, old_pen);
    if (old_brush && (ops & 2)) SelectObject(hdc, old_brush);
    else if (!(ops & 2) && old_brush) SelectObject(hdc, old_brush);
    if (pen)   DeleteObject(pen);
    if (brush) DeleteObject(brush);

    path_reset(pb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Text Rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Resolve the /BaseFont name for a font referenced in the page resources.
 * font_name: the name from the Tf operator (e.g., "F1")
 * resources: the page's /Resources dictionary
 */
static const char *resolve_base_font(PdfDocument *doc, PdfDict *resources,
                                      const char *font_name)
{
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return NULL;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return NULL;

    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_name);
    if (!font_obj) return NULL;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return NULL;

    return pdf_dict_get_name(font_obj->dict, "BaseFont");
}

/*
 * Check if a font has a /Widths array in the PDF.
 * Returns true if PDF-specific widths are available.
 */
static bool font_has_pdf_widths(PdfDocument *doc, PdfDict *resources,
                                 const char *font_res_name)
{
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return false;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return false;

    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
    if (!font_obj) return false;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return false;

    PdfObj *widths_obj = pdf_dict_get(font_obj->dict, "Widths");
    if (!widths_obj) return false;
    widths_obj = pdf_resolve(doc, widths_obj);
    return (widths_obj && widths_obj->type == PDF_OBJ_ARRAY &&
            pdf_array_len(widths_obj->array) > 0);
}

/*
 * Look up font widths from the font dictionary.
 * Returns the width of a character in 1/1000 text space units.
 * If no /Widths array, falls back to standard font metrics.
 */
static int get_font_char_width(PdfDocument *doc, PdfDict *resources,
                                const char *font_res_name, int char_code,
                                const char *base_font_name)
{
    /* Try to get widths from the font dictionary */
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (fonts_obj) {
        fonts_obj = pdf_resolve(doc, fonts_obj);
        if (fonts_obj && fonts_obj->type == PDF_OBJ_DICT) {
            PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
            if (font_obj) {
                font_obj = pdf_resolve(doc, font_obj);
                if (font_obj && font_obj->type == PDF_OBJ_DICT) {
                    int first_char = pdf_dict_get_int(font_obj->dict, "FirstChar", 0);
                    int last_char  = pdf_dict_get_int(font_obj->dict, "LastChar", 255);
                    PdfObj *widths_obj = pdf_dict_get(font_obj->dict, "Widths");
                    if (widths_obj) {
                        widths_obj = pdf_resolve(doc, widths_obj);
                        if (widths_obj && widths_obj->type == PDF_OBJ_ARRAY) {
                            int idx = char_code - first_char;
                            if (idx >= 0 && idx < pdf_array_len(widths_obj->array) &&
                                idx <= (last_char - first_char)) {
                                PdfObj *w = pdf_array_get(widths_obj->array, idx);
                                if (w) {
                                    w = pdf_resolve(doc, w);
                                    if (w) {
                                        if (w->type == PDF_OBJ_INT) return (int)w->integer;
                                        if (w->type == PDF_OBJ_REAL) return (int)(w->real + 0.5);
                                    }
                                }
                            }
                        }
                    }
                    /* Check /MissingWidth in font descriptor */
                    PdfObj *desc = pdf_dict_get(font_obj->dict, "FontDescriptor");
                    if (desc) {
                        desc = pdf_resolve(doc, desc);
                        if (desc && desc->type == PDF_OBJ_DICT) {
                            int mw = pdf_dict_get_int(desc->dict, "MissingWidth", -1);
                            if (mw >= 0) return mw;
                        }
                    }
                }
            }
        }
    }

    /* Fallback to standard font metrics */
    if (base_font_name) {
        int w = pdf_font_char_width(base_font_name, char_code);
        if (w > 0) return w;
        return pdf_font_default_width(base_font_name);
    }
    return 600; /* last resort */
}

/*
 * Get character advance from the GDI font actually being used for rendering.
 * Returns the advance in 1/1000 text space units (to match PDF metrics scale).
 * This gives more accurate positioning when using font substitution.
 */
static int get_gdi_char_width(HDC hdc, int char_code, double font_size_px)
{
    wchar_t wc = (wchar_t)char_code;
    SIZE sz;
    if (GetTextExtentPoint32W(hdc, &wc, 1, &sz)) {
        /* Convert pixel width to 1/1000 text space units.
         * font_size_px is the absolute font height in pixels.
         * width_in_thousandths = (pixel_width / font_size_px) * 1000 */
        if (font_size_px > 0.001) {
            return (int)(sz.cx / font_size_px * 1000.0 + 0.5);
        }
    }
    return 0;
}

/*
 * Resolve a font dictionary from the page resources given a font resource name.
 * Returns the resolved PdfDict* for the font, or NULL if not found.
 */
static PdfDict *resolve_font_dict(PdfDocument *doc, PdfDict *resources,
                                   const char *font_name)
{
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return NULL;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return NULL;

    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_name);
    if (!font_obj) return NULL;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return NULL;

    return font_obj->dict;
}

/* Forward declaration for interpret_stream (needed by render_type3_string) */
static void interpret_stream(PdfRenderCtx *ctx, PdfDict *resources,
                              const uint8_t *data, size_t length);

/*
 * Build a char_code -> glyph_name mapping for a Type3 font from its /Encoding.
 * Type3 fonts typically use a custom encoding with /Differences.
 * The glyph names correspond to keys in /CharProcs.
 */
static void build_type3_encoding(PdfDocument *doc, PdfDict *font_dict,
                                  char glyph_names[256][64])
{
    memset(glyph_names, 0, 256 * 64);

    PdfObj *enc_obj = pdf_dict_get(font_dict, "Encoding");
    if (!enc_obj) return;
    enc_obj = pdf_resolve(doc, enc_obj);
    if (!enc_obj) return;

    if (enc_obj->type == PDF_OBJ_DICT) {
        /* Encoding dictionary with /Differences */
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
                        current_code = (int)item->integer;
                    } else if (item->type == PDF_OBJ_NAME && item->name) {
                        if (current_code >= 0 && current_code < 256) {
                            strncpy(glyph_names[current_code], item->name, 63);
                            glyph_names[current_code][63] = '\0';
                        }
                        current_code++;
                    }
                }
            }
        }
    }
    /* Named encodings are rare for Type3 but handle for safety */
    /* Type3 fonts almost always use /Differences */
}

/*
 * Render a Type3 font text string.
 * Type3 fonts define each glyph as a mini PDF content stream in /CharProcs.
 * We look up each character's glyph stream and interpret it using the
 * existing content stream interpreter, with appropriate coordinate transforms.
 */
static void render_type3_string(PdfRenderCtx *ctx, PdfDict *resources,
                                 PdfDict *font_dict, const uint8_t *str, size_t len)
{
    PdfGraphicsState *gs = current_gs(ctx);

    /* Get FontMatrix (glyph units -> text space), default [0.001 0 0 0.001 0 0] */
    double fm[6] = {0.001, 0.0, 0.0, 0.001, 0.0, 0.0};
    PdfArray *font_matrix = pdf_dict_get_array(font_dict, "FontMatrix");
    if (font_matrix && pdf_array_len(font_matrix) >= 6) {
        for (int i = 0; i < 6; i++) {
            PdfObj *v = pdf_resolve(ctx->doc, pdf_array_get(font_matrix, i));
            if (v) {
                if (v->type == PDF_OBJ_REAL) fm[i] = v->real;
                else if (v->type == PDF_OBJ_INT) fm[i] = (double)v->integer;
            }
        }
    }

    /* Get Widths array and FirstChar */
    PdfObj *widths_obj = pdf_dict_get(font_dict, "Widths");
    if (widths_obj) widths_obj = pdf_resolve(ctx->doc, widths_obj);
    int first_char = pdf_dict_get_int(font_dict, "FirstChar", 0);

    /* Get CharProcs dictionary */
    PdfObj *char_procs = pdf_dict_get(font_dict, "CharProcs");
    if (!char_procs) return;
    char_procs = pdf_resolve(ctx->doc, char_procs);
    if (!char_procs || char_procs->type != PDF_OBJ_DICT) return;

    /* Get font's own Resources (or inherit from page) */
    PdfObj *font_resources_obj = pdf_dict_get(font_dict, "Resources");
    PdfDict *font_resources = resources;
    if (font_resources_obj) {
        font_resources_obj = pdf_resolve(ctx->doc, font_resources_obj);
        if (font_resources_obj && font_resources_obj->type == PDF_OBJ_DICT)
            font_resources = font_resources_obj->dict;
    }

    /* Build char_code -> glyph_name mapping from Encoding */
    char (*glyph_names)[64] = (char (*)[64])calloc(256, 64);
    if (!glyph_names) return;
    build_type3_encoding(ctx->doc, font_dict, glyph_names);

    /* Horizontal scaling */
    double h_scale = gs->horiz_scaling / 100.0;

    for (size_t i = 0; i < len; i++) {
        int code = (int)str[i];

        /* Get glyph name from encoding */
        const char *glyph_name = glyph_names[code];
        if (!glyph_name || glyph_name[0] == '\0') {
            /* No glyph for this code; just advance using width */
            goto advance_type3;
        }

        /* Get glyph stream from CharProcs */
        PdfObj *glyph_obj = pdf_dict_get(char_procs->dict, glyph_name);
        if (glyph_obj) glyph_obj = pdf_resolve(ctx->doc, glyph_obj);

        if (glyph_obj && glyph_obj->type == PDF_OBJ_STREAM) {
            /* Decode the stream if needed */
            if (!glyph_obj->stream->decoded_data)
                pdf_decode_stream(ctx->doc, glyph_obj->stream);

            if (glyph_obj->stream->decoded_data) {
                /* Build transformation: FontMatrix * font_size -> text space,
                 * then text_matrix * CTM -> device space.
                 *
                 * The full transform for a Type3 glyph is:
                 *   FontMatrix x [fontSize 0 0 fontSize 0 0] x text_matrix x CTM
                 *
                 * This maps glyph space -> text space -> user space -> device space.
                 */
                PdfMatrix fm_matrix = {fm[0], fm[1], fm[2], fm[3], fm[4], fm[5]};
                PdfMatrix size_matrix = {gs->font_size, 0.0, 0.0, gs->font_size, 0.0, 0.0};
                PdfMatrix glyph_to_text = pdf_matrix_multiply(fm_matrix, size_matrix);
                PdfMatrix glyph_to_device = pdf_matrix_multiply(
                    pdf_matrix_multiply(glyph_to_text, ctx->text_matrix), gs->ctm);

                /* Compute glyph bounding box in device space.
                 * Use FontBBox if available, otherwise use a generous estimate.
                 * FontBBox is in glyph space coordinates. */
                double bbox[4] = {0.0, 0.0, 1000.0, 1000.0}; /* default estimate */
                PdfArray *font_bbox = pdf_dict_get_array(font_dict, "FontBBox");
                if (font_bbox && pdf_array_len(font_bbox) >= 4) {
                    for (int bi = 0; bi < 4; bi++) {
                        PdfObj *bv = pdf_resolve(ctx->doc, pdf_array_get(font_bbox, bi));
                        if (bv) {
                            if (bv->type == PDF_OBJ_REAL) bbox[bi] = bv->real;
                            else if (bv->type == PDF_OBJ_INT) bbox[bi] = (double)bv->integer;
                        }
                    }
                }

                /* Transform FontBBox corners through glyph_to_device */
                double bx0, by0, bx1, by1, bx2, by2, bx3, by3;
                pdf_transform_point(glyph_to_device, bbox[0], bbox[1], &bx0, &by0);
                pdf_transform_point(glyph_to_device, bbox[2], bbox[1], &bx1, &by1);
                pdf_transform_point(glyph_to_device, bbox[2], bbox[3], &bx2, &by2);
                pdf_transform_point(glyph_to_device, bbox[0], bbox[3], &bx3, &by3);

                double gmin_x = bx0, gmax_x = bx0, gmin_y = by0, gmax_y = by0;
                if (bx1 < gmin_x) gmin_x = bx1; if (bx1 > gmax_x) gmax_x = bx1;
                if (bx2 < gmin_x) gmin_x = bx2; if (bx2 > gmax_x) gmax_x = bx2;
                if (bx3 < gmin_x) gmin_x = bx3; if (bx3 > gmax_x) gmax_x = bx3;
                if (by1 < gmin_y) gmin_y = by1; if (by1 > gmax_y) gmax_y = by1;
                if (by2 < gmin_y) gmin_y = by2; if (by2 > gmax_y) gmax_y = by2;
                if (by3 < gmin_y) gmin_y = by3; if (by3 > gmax_y) gmax_y = by3;

                int dest_x = (int)floor(gmin_x);
                int dest_y = (int)floor(gmin_y);
                int glyph_dev_w = (int)ceil(gmax_x) - dest_x;
                int glyph_dev_h = (int)ceil(gmax_y) - dest_y;
                if (glyph_dev_w < 1) glyph_dev_w = 1;
                if (glyph_dev_h < 1) glyph_dev_h = 1;

                /* Decide whether to supersample: only for small glyphs where
                 * AA matters. Too large = waste memory, too small = not visible. */
                int ss = 1; /* supersample factor */
                if (glyph_dev_h >= 5 && glyph_dev_h <= 200 &&
                    glyph_dev_w >= 3 && glyph_dev_w <= 200) {
                    ss = 4;
                }

                if (ss > 1) {
                    /* ── 4× Supersampled offscreen rendering ──
                     * Render glyph at 4× device size into an offscreen buffer,
                     * then downsample to the page with HALFTONE for smooth AA. */
                    int off_w = glyph_dev_w * ss;
                    int off_h = glyph_dev_h * ss;

                    /* Create offscreen DIB section */
                    BITMAPINFO off_bmi;
                    memset(&off_bmi, 0, sizeof(off_bmi));
                    off_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                    off_bmi.bmiHeader.biWidth       = off_w;
                    off_bmi.bmiHeader.biHeight      = -off_h; /* top-down */
                    off_bmi.bmiHeader.biPlanes      = 1;
                    off_bmi.bmiHeader.biBitCount    = 32;
                    off_bmi.bmiHeader.biCompression = BI_RGB;

                    uint8_t *off_bits = NULL;
                    HBITMAP off_bmp = CreateDIBSection(ctx->hdc, &off_bmi,
                                                       DIB_RGB_COLORS,
                                                       (void **)&off_bits, NULL, 0);
                    if (off_bmp && off_bits) {
                        HDC off_dc = CreateCompatibleDC(ctx->hdc);
                        HBITMAP off_old = (HBITMAP)SelectObject(off_dc, off_bmp);

                        /* Copy background from page DC into offscreen at 4× size.
                         * This ensures transparency composites correctly against
                         * whatever is already on the page. */
                        SetStretchBltMode(off_dc, COLORONCOLOR);
                        StretchBlt(off_dc, 0, 0, off_w, off_h,
                                   ctx->hdc, dest_x, dest_y,
                                   glyph_dev_w, glyph_dev_h, SRCCOPY);

                        /* Save original HDC and swap in offscreen DC */
                        HDC orig_hdc = ctx->hdc;
                        ctx->hdc = off_dc;

                        /* Save graphics state (will SaveDC on off_dc) */
                        gs_save(ctx);

                        /* Build adjusted CTM for offscreen rendering.
                         * The glyph_to_device matrix maps glyph coords to page
                         * device coords. We need to:
                         * 1. Subtract the dest origin (translate to offscreen 0,0)
                         * 2. Scale by ss (render at 4× size)
                         *
                         * new_ctm = glyph_to_device * translate(-dest_x, -dest_y) * scale(ss)
                         */
                        PdfMatrix to_offscreen = {
                            (double)ss, 0.0,
                            0.0, (double)ss,
                            -(double)dest_x * ss, -(double)dest_y * ss
                        };
                        current_gs(ctx)->ctm = pdf_matrix_multiply(
                            glyph_to_device, to_offscreen);

                        /* Interpret the glyph content stream into offscreen */
                        interpret_stream(ctx, font_resources,
                                         glyph_obj->stream->decoded_data,
                                         glyph_obj->stream->decoded_length);

                        /* Restore graphics state (RestoreDC on off_dc) */
                        gs_restore(ctx);

                        /* Restore original page HDC */
                        ctx->hdc = orig_hdc;

                        /* Downsample offscreen → page with HALFTONE for AA */
                        SetStretchBltMode(ctx->hdc, HALFTONE);
                        SetBrushOrgEx(ctx->hdc, 0, 0, NULL);
                        StretchBlt(ctx->hdc, dest_x, dest_y,
                                   glyph_dev_w, glyph_dev_h,
                                   off_dc, 0, 0, off_w, off_h, SRCCOPY);

                        SelectObject(off_dc, off_old);
                        DeleteDC(off_dc);
                    }
                    if (off_bmp) DeleteObject(off_bmp);
                } else {
                    /* No supersampling — render directly (too large or too small) */
                    gs_save(ctx);
                    current_gs(ctx)->ctm = glyph_to_device;
                    interpret_stream(ctx, font_resources,
                                     glyph_obj->stream->decoded_data,
                                     glyph_obj->stream->decoded_length);
                    gs_restore(ctx);
                }

                gs = current_gs(ctx); /* re-acquire after restore */
            }
        }

advance_type3:
        /* Advance text position using Widths */
        {
            double advance = 0;
            if (widths_obj && widths_obj->type == PDF_OBJ_ARRAY) {
                int idx = code - first_char;
                if (idx >= 0 && idx < pdf_array_len(widths_obj->array)) {
                    PdfObj *w = pdf_array_get(widths_obj->array, idx);
                    if (w) w = pdf_resolve(ctx->doc, w);
                    if (w) {
                        if (w->type == PDF_OBJ_REAL) advance = w->real;
                        else if (w->type == PDF_OBJ_INT) advance = (double)w->integer;
                    }
                }
            }

            /* The width is in glyph space. Apply FontMatrix to convert to text space,
             * then multiply by font_size. For most Type3 fonts, FontMatrix[0] handles
             * the x-axis scaling. */
            advance *= fm[0] * gs->font_size;

            /* Add character spacing */
            advance += gs->char_spacing;

            /* Add word spacing for space characters (code 32) */
            if (code == 32) advance += gs->word_spacing;

            /* Apply horizontal scaling */
            advance *= h_scale;

            /* Update text matrix */
            PdfMatrix adv = PDF_IDENTITY_MATRIX;
            adv.e = advance;
            ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
        }
    }

    free(glyph_names);
}

/*
 * Render a text string at the current text position.
 * Updates the text matrix to advance past the rendered text.
 */
static void render_text_string(PdfRenderCtx *ctx, PdfDict *resources,
                                const uint8_t *str, size_t len)
{
    if (len == 0) return;

    /* Check if this is a Type3 font. Type3 fonts define glyphs as mini PDF
     * content streams rather than outline data, so they need special handling. */
    {
        PdfGraphicsState *gs = current_gs(ctx);
        PdfDict *font_dict = resolve_font_dict(ctx->doc, resources, gs->font_name);
        if (font_dict) {
            const char *subtype = pdf_dict_get_name(font_dict, "Subtype");
            if (subtype && strcmp(subtype, "Type3") == 0) {
                render_type3_string(ctx, resources, font_dict, str, len);
                return;
            }
        }
    }

    /* Try the software rasterizer path first for anti-aliased rendering.
     * glyph_render_text_string will extract embedded font outlines, render
     * them with 8x AA, and blend onto the page bitmap. If no embedded font
     * is available, it returns false and we fall through to TextOutW. */
    if (glyph_render_text_string(ctx, resources, str, len))
        return;

    PdfGraphicsState *gs = current_gs(ctx);
    HDC hdc = ctx->hdc;

    /* Resolve base font name */
    const char *base_font = resolve_base_font(ctx->doc, resources, gs->font_name);
    if (!base_font) base_font = "Helvetica";

    /* Try to load embedded font from the PDF.
     * If the font is embedded as TrueType or OpenType, this loads it into GDI
     * memory and gives us the actual family name to use for HFONT creation. */
    char embedded_family[256];
    bool has_embedded_font = pdf_font_try_load_embedded(
        ctx->doc, resources, gs->font_name, embedded_family, sizeof(embedded_family));

    /* Compute effective font size in device pixels.
     * The text matrix and CTM together transform text space to device space.
     * The effective vertical scale is sqrt(b^2 + d^2) of the combined matrix,
     * which gives us the font size in device pixels. */
    double font_size = gs->font_size;
    PdfMatrix font_mtx = pdf_matrix_multiply(ctx->text_matrix, gs->ctm);
    double eff_scale = sqrt(font_mtx.b * font_mtx.b + font_mtx.d * font_mtx.d);
    int font_height_px = -(int)(font_size * eff_scale + 0.5);
    if (font_height_px == 0) font_height_px = -12;

    /* Extract rotation angle from the combined matrix.
     * In PDF, the text matrix maps text space -> user space, then CTM maps to device.
     * The combined matrix's a,b components give the direction of the x-axis in device space.
     * atan2(-b, a) gives the GDI rotation angle (GDI Y-axis is flipped vs math convention).
     * Note: GDI escapement is counter-clockwise in tenths of degrees. */
    double rotation_rad = atan2(-font_mtx.b, font_mtx.a);
    int rotation_deg = (int)(rotation_rad * 180.0 / 3.14159265358979323846 + 0.5);
    /* Normalize to [0, 360) */
    rotation_deg = ((rotation_deg % 360) + 360) % 360;

    /* Create the GDI font. If we loaded an embedded font, use its family name
     * directly; otherwise fall back to the system font mapping heuristics. */
    HFONT hfont;
    if (has_embedded_font) {
        hfont = pdf_font_create_px(embedded_family, font_height_px, rotation_deg);
    } else {
        hfont = pdf_font_create_px(base_font, font_height_px, rotation_deg);
    }
    if (!hfont) return;

    HFONT old_font = (HFONT)SelectObject(hdc, hfont);
    SetBkMode(hdc, TRANSPARENT);

    /* PDF positions text at the baseline, but GDI's TextOutW defaults to
     * rendering from the top-left of the character cell. Set TA_BASELINE
     * so TextOutW interprets the y-coordinate as the baseline position. */
    SetTextAlign(hdc, TA_BASELINE | TA_LEFT);

    /* Text rendering mode determines color and style:
     * 0 = fill, 1 = stroke, 2 = fill+stroke, 3 = invisible,
     * 4-7 = same but add to clipping path */
    COLORREF text_color;
    if (gs->text_render_mode == 1) {
        text_color = pdf_color_to_gdi(gs->stroke_color);
    } else {
        text_color = pdf_color_to_gdi(gs->fill_color);
    }
    SetTextColor(hdc, text_color);

    /* Horizontal scaling factor */
    double h_scale = gs->horiz_scaling / 100.0;

    /* Check if this font has PDF-embedded widths. If not, we'll use GDI
     * measured widths for more accurate positioning with substituted fonts. */
    bool has_pdf_widths = font_has_pdf_widths(ctx->doc, resources, gs->font_name);
    double abs_font_height_px = fabs((double)font_height_px);

    /* ─── Check for Type0/CID composite font (2-byte character codes) ─── */
    {
        PdfDict *font_dict = resolve_font_dict(ctx->doc, resources, gs->font_name);
        if (font_dict) {
            const char *subtype = pdf_dict_get_name(font_dict, "Subtype");
            if (subtype && strcmp(subtype, "Type0") == 0) {
                /* Type0 font: read 2-byte CIDs and map to Unicode via ToUnicode */
                const char *encoding = pdf_dict_get_name(font_dict, "Encoding");
                bool is_identity = encoding && (strcmp(encoding, "Identity-H") == 0 ||
                                                 strcmp(encoding, "Identity-V") == 0);
                if (is_identity && len >= 2) {
                    /* Parse ToUnicode CMap for CID → Unicode mapping */
                    uint16_t tounicode_map[65536];
                    memset(tounicode_map, 0, sizeof(tounicode_map));
                    int has_tounicode = 0;

                    PdfObj *tounicode_obj = pdf_dict_get(font_dict, "ToUnicode");
                    if (tounicode_obj) {
                        tounicode_obj = pdf_resolve(ctx->doc, tounicode_obj);
                        if (tounicode_obj && tounicode_obj->type == PDF_OBJ_STREAM) {
                            PdfStream *tu_stream = tounicode_obj->stream;
                            if (!tu_stream->decoded_data)
                                pdf_decode_stream(ctx->doc, tu_stream);
                            if (tu_stream->decoded_data) {
                                /* Simple CMap parser: look for beginbfchar/endbfchar
                                 * and beginbfrange/endbfrange sections */
                                const char *s = (const char *)tu_stream->decoded_data;
                                size_t slen = tu_stream->decoded_length;
                                const char *end = s + slen;
                                has_tounicode = 1;

                                while (s < end) {
                                    /* Skip to next '<' */
                                    const char *lt = memchr(s, '<', end - s);
                                    if (!lt) break;

                                    /* Check if this is in a bfchar or bfrange section */
                                    /* Parse: <XXXX> <YYYY> for bfchar mapping */
                                    if (lt + 5 < end && lt[5] == '>') {
                                        /* 4-digit hex: <XXXX> */
                                        unsigned int cid = 0;
                                        if (sscanf(lt + 1, "%4x", &cid) == 1 && cid < 65536) {
                                            /* Look for next <YYYY> */
                                            const char *gt = lt + 6;
                                            while (gt < end && *gt == ' ') gt++;
                                            if (gt < end && *gt == '<' && gt + 5 < end && gt[5] == '>') {
                                                unsigned int unicode = 0;
                                                if (sscanf(gt + 1, "%4x", &unicode) == 1) {
                                                    tounicode_map[cid] = (uint16_t)unicode;
                                                }
                                            }
                                        }
                                    }
                                    s = lt + 1;
                                }
                            }
                        }
                    }

                    /* Get CID width info from DescendantFonts */
                    PdfObj *descendants = pdf_dict_get(font_dict, "DescendantFonts");
                    PdfDict *cid_font_dict = NULL;
                    int default_w = 1000;
                    PdfArray *w_array = NULL;

                    if (descendants) {
                        descendants = pdf_resolve(ctx->doc, descendants);
                        if (descendants && descendants->type == PDF_OBJ_ARRAY) {
                            PdfObj *first = pdf_array_get(descendants->array, 0);
                            if (first) first = pdf_resolve(ctx->doc, first);
                            if (first && first->type == PDF_OBJ_DICT) {
                                cid_font_dict = first->dict;
                                default_w = pdf_dict_get_int(cid_font_dict, "DW", 1000);
                                PdfObj *w_obj = pdf_dict_get(cid_font_dict, "W");
                                if (w_obj) {
                                    w_obj = pdf_resolve(ctx->doc, w_obj);
                                    if (w_obj && w_obj->type == PDF_OBJ_ARRAY)
                                        w_array = w_obj->array;
                                }
                            }
                        }
                    }

                    /* Render 2-byte CID characters */
                    for (size_t i = 0; i + 1 < len; i += 2) {
                        int cid = ((int)str[i] << 8) | (int)str[i + 1];

                        /* Map CID to Unicode via ToUnicode */
                        wchar_t wc = 0;
                        if (has_tounicode && cid < 65536) {
                            wc = (wchar_t)tounicode_map[cid];
                        }
                        if (wc == 0) wc = (wchar_t)cid;

                        /* Render if not invisible and not control char */
                        if (gs->text_render_mode != 3 && wc >= 0x20) {
                            PdfMatrix combined = pdf_matrix_multiply(ctx->text_matrix, gs->ctm);
                            double ox, oy;
                            pdf_transform_point(combined, 0, gs->text_rise, &ox, &oy);
                            int dx = (int)floor(ox + 0.5);
                            int dy = (int)floor(oy + 0.5);
                            TextOutW(hdc, dx, dy, &wc, 1);
                        }

                        /* Get CID width from /W array */
                        double cid_width = (double)default_w;
                        if (w_array) {
                            int n = pdf_array_len(w_array);
                            int wi = 0;
                            while (wi < n) {
                                PdfObj *first_obj = pdf_array_get(w_array, wi);
                                if (!first_obj) break;
                                first_obj = pdf_resolve(ctx->doc, first_obj);
                                if (!first_obj || first_obj->type != PDF_OBJ_INT) break;
                                int first_cid = (int)first_obj->integer;
                                wi++;
                                if (wi >= n) break;
                                PdfObj *second = pdf_array_get(w_array, wi);
                                if (!second) break;
                                second = pdf_resolve(ctx->doc, second);
                                if (!second) break;
                                if (second->type == PDF_OBJ_ARRAY) {
                                    int offset = cid - first_cid;
                                    int wcount = pdf_array_len(second->array);
                                    if (offset >= 0 && offset < wcount) {
                                        PdfObj *wv = pdf_array_get(second->array, offset);
                                        if (wv) wv = pdf_resolve(ctx->doc, wv);
                                        if (wv) {
                                            if (wv->type == PDF_OBJ_INT) cid_width = (double)wv->integer;
                                            else if (wv->type == PDF_OBJ_REAL) cid_width = wv->real;
                                        }
                                    }
                                    wi++;
                                } else if (second->type == PDF_OBJ_INT) {
                                    int last_cid = (int)second->integer;
                                    wi++;
                                    if (wi >= n) break;
                                    PdfObj *wv = pdf_array_get(w_array, wi);
                                    if (wv) wv = pdf_resolve(ctx->doc, wv);
                                    wi++;
                                    if (cid >= first_cid && cid <= last_cid && wv) {
                                        if (wv->type == PDF_OBJ_INT) cid_width = (double)wv->integer;
                                        else if (wv->type == PDF_OBJ_REAL) cid_width = wv->real;
                                    }
                                } else break;
                            }
                        }

                        /* Advance text position */
                        double advance = cid_width / 1000.0 * font_size;
                        advance += gs->char_spacing;
                        if (wc == 0x20 || cid == 0x0003)
                            advance += gs->word_spacing;
                        advance *= h_scale;

                        PdfMatrix adv = PDF_IDENTITY_MATRIX;
                        adv.e = advance;
                        ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
                    }

                    /* Cleanup and return */
                    SetTextAlign(hdc, TA_TOP | TA_LEFT);
                    SelectObject(hdc, old_font);
                    DeleteObject(hfont);
                    return;
                }
            }
        }
    }

    /* ─── Simple Font Path (1-byte character codes) ─── */

    /* Render each character individually for accurate positioning */
    for (size_t i = 0; i < len; i++) {
        int char_code = str[i];

        /* Skip invisible text rendering mode */
        if (gs->text_render_mode == 3) {
            /* Still advance position */
        } else if (char_code != 0) {
            /* Render all non-null characters. DO NOT skip codes below 0x20
             * because many fonts (especially LaTeX math) use them for valid glyphs. */
            PdfMatrix combined = pdf_matrix_multiply(ctx->text_matrix, gs->ctm);
            double ox, oy;
            pdf_transform_point(combined, 0, gs->text_rise, &ox, &oy);
            int dx = (int)floor(ox + 0.5);
            int dy = (int)floor(oy + 0.5);

            /* Convert char to wchar for TextOutW */
            wchar_t wc = (wchar_t)char_code;
            /* For standard encoding, direct cast works for Latin-1 range */

            TextOutW(hdc, dx, dy, &wc, 1);
        }
        /* else: control characters (0x01-0x1F) are skipped - they are not
         * renderable glyphs. Some PDFs embed CR/LF in text strings as
         * line-break hints that should not produce visible output. */

        /* Advance text position using PDF-specified widths.
         * For fonts without PDF widths, use GDI-measured widths for better
         * alignment with the substituted font being rendered. */
        int w;
        if (has_pdf_widths) {
            w = get_font_char_width(ctx->doc, resources, gs->font_name,
                                     char_code, base_font);
        } else {
            w = get_gdi_char_width(hdc, char_code, abs_font_height_px);
            if (w <= 0) {
                w = get_font_char_width(ctx->doc, resources, gs->font_name,
                                         char_code, base_font);
            }
        }
        double advance = (double)w / 1000.0 * font_size;

        /* Add character spacing */
        advance += gs->char_spacing;

        /* Add word spacing for space characters (code 32) */
        if (char_code == 32) {
            advance += gs->word_spacing;
        }

        /* Apply horizontal scaling */
        advance *= h_scale;

        /* Update text matrix: translate by advance in text space */
        PdfMatrix adv = PDF_IDENTITY_MATRIX;
        adv.e = advance;
        ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
    }

    /* Restore default text alignment so non-text GDI calls aren't affected */
    SetTextAlign(hdc, TA_TOP | TA_LEFT);

    SelectObject(hdc, old_font);
    DeleteObject(hfont);
}

/*
 * Render a TJ array: mix of strings and numeric adjustments.
 * Numeric values adjust text position (in thousandths of a unit of text space).
 */
static void render_TJ_array(PdfRenderCtx *ctx, PdfDict *resources,
                             OperandStack *ops)
{
    /* The TJ array is represented as a sequence of operands between [ and ] markers.
     * By the time we see the TJ operator, the array has been parsed into operands.
     * We need to iterate the operands looking for strings and numbers. */

    /* Actually, in our implementation, TJ receives its operands differently.
     * The array tokens were collected on the operand stack.
     * We need to process them inline. This is handled by the caller. */
    (void)ctx; (void)resources; (void)ops;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Image Rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Resolve the base color space name from a /ColorSpace entry.
 * Handles both simple names (/DeviceRGB) and arrays ([/Indexed ...]).
 * For Indexed color spaces, also extracts the palette.
 *
 * Returns: components for the base color space (1, 3, or 4).
 * Sets *is_indexed = true and populates palette_rgb/palette_count for Indexed.
 */
static int resolve_image_colorspace(PdfDocument *doc, PdfDict *dict,
                                     bool *is_indexed,
                                     uint8_t *palette_rgb, int *palette_count)
{
    *is_indexed = false;
    *palette_count = 0;

    PdfObj *cs_obj = pdf_dict_get(dict, "ColorSpace");
    if (!cs_obj) return 3; /* default RGB */

    cs_obj = pdf_resolve(doc, cs_obj);
    if (!cs_obj) return 3;

    /* Simple name: /DeviceRGB, /DeviceGray, etc. */
    if (cs_obj->type == PDF_OBJ_NAME) {
        const char *name = cs_obj->name;
        if (strcmp(name, "DeviceGray") == 0 || strcmp(name, "CalGray") == 0) return 1;
        if (strcmp(name, "DeviceRGB") == 0 || strcmp(name, "CalRGB") == 0)   return 3;
        if (strcmp(name, "DeviceCMYK") == 0)  return 4;
        return 3;
    }

    /* Array color space: [/name ...] */
    if (cs_obj->type != PDF_OBJ_ARRAY) return 3;

    PdfArray *arr = cs_obj->array;
    if (pdf_array_len(arr) < 1) return 3;

    PdfObj *cs_type = pdf_resolve(doc, pdf_array_get(arr, 0));
    if (!cs_type || cs_type->type != PDF_OBJ_NAME) return 3;

    const char *cs_name = cs_type->name;

    /* [/CalGray <<...>>] or [/CalRGB <<...>>] */
    if (strcmp(cs_name, "CalGray") == 0)  return 1;
    if (strcmp(cs_name, "CalRGB") == 0)   return 3;
    if (strcmp(cs_name, "ICCBased") == 0) {
        /* [/ICCBased stream_ref] -- get /N from the ICC profile stream */
        if (pdf_array_len(arr) >= 2) {
            PdfObj *icc = pdf_resolve(doc, pdf_array_get(arr, 1));
            if (icc && icc->type == PDF_OBJ_STREAM) {
                int n = pdf_dict_get_int(icc->stream->dict, "N", 3);
                return n;
            }
        }
        return 3;
    }

    /* [/Indexed base hival lookup] */
    if (strcmp(cs_name, "Indexed") == 0 && pdf_array_len(arr) >= 4) {
        *is_indexed = true;

        /* Determine base color space components */
        PdfObj *base_cs = pdf_resolve(doc, pdf_array_get(arr, 1));
        int base_components = 3;
        if (base_cs && base_cs->type == PDF_OBJ_NAME) {
            if (strcmp(base_cs->name, "DeviceGray") == 0 ||
                strcmp(base_cs->name, "CalGray") == 0)     base_components = 1;
            else if (strcmp(base_cs->name, "DeviceCMYK") == 0) base_components = 4;
        }

        /* hival: maximum valid index */
        PdfObj *hival_obj = pdf_resolve(doc, pdf_array_get(arr, 2));
        int hival = 255;
        if (hival_obj) {
            if (hival_obj->type == PDF_OBJ_INT) hival = (int)hival_obj->integer;
            else if (hival_obj->type == PDF_OBJ_REAL) hival = (int)hival_obj->real;
        }
        if (hival > 255) hival = 255;
        *palette_count = hival + 1;

        /* lookup: either a string or a stream containing the palette data */
        PdfObj *lookup = pdf_resolve(doc, pdf_array_get(arr, 3));
        const uint8_t *pal_data = NULL;
        size_t pal_len = 0;

        if (lookup && lookup->type == PDF_OBJ_STRING) {
            pal_data = lookup->string.data;
            pal_len = lookup->string.length;
        } else if (lookup && lookup->type == PDF_OBJ_STREAM) {
            if (!lookup->stream->decoded_data)
                pdf_decode_stream(doc, lookup->stream);
            if (lookup->stream->decoded_data) {
                pal_data = lookup->stream->decoded_data;
                pal_len = lookup->stream->decoded_length;
            }
        }

        /* Convert palette to RGB triplets */
        if (pal_data && pal_len > 0) {
            for (int i = 0; i <= hival && i < 256; i++) {
                size_t pal_idx = (size_t)i * base_components;
                uint8_t r = 0, g = 0, b = 0;

                if (base_components == 1 && pal_idx < pal_len) {
                    r = g = b = pal_data[pal_idx];
                } else if (base_components == 3 && pal_idx + 2 < pal_len) {
                    r = pal_data[pal_idx];
                    g = pal_data[pal_idx + 1];
                    b = pal_data[pal_idx + 2];
                } else if (base_components == 4 && pal_idx + 3 < pal_len) {
                    double c_ = pal_data[pal_idx]     / 255.0;
                    double m_ = pal_data[pal_idx + 1] / 255.0;
                    double y_ = pal_data[pal_idx + 2] / 255.0;
                    double k_ = pal_data[pal_idx + 3] / 255.0;
                    PdfColor pal_rgb = cmyk_to_rgb(c_, m_, y_, k_);
                    double rr = pal_rgb.r;
                    double gg = pal_rgb.g;
                    double bb = pal_rgb.b;
                    if (rr < 0.0) rr = 0.0; if (rr > 1.0) rr = 1.0;
                    if (gg < 0.0) gg = 0.0; if (gg > 1.0) gg = 1.0;
                    if (bb < 0.0) bb = 0.0; if (bb > 1.0) bb = 1.0;
                    r = (uint8_t)(rr * 255.0 + 0.5);
                    g = (uint8_t)(gg * 255.0 + 0.5);
                    b = (uint8_t)(bb * 255.0 + 0.5);
                }

                palette_rgb[i * 3 + 0] = r;
                palette_rgb[i * 3 + 1] = g;
                palette_rgb[i * 3 + 2] = b;
            }
        }

        return 1; /* Indexed images have 1 component (the index byte) */
    }

    /* [/Separation name altCS tintTransform] -- treat as grayscale fallback */
    if (strcmp(cs_name, "Separation") == 0) return 1;

    return 3; /* fallback */
}

/*
 * Check if the image stream's filter is DCTDecode (JPEG) or JPXDecode (JPEG2000).
 * Returns true if the raw stream data is JPEG-encoded and should be decoded
 * with WIC rather than treated as raw pixel data.
 */
static bool image_has_jpeg_filter(PdfDict *dict)
{
    PdfObj *filter_obj = pdf_dict_get(dict, "Filter");
    if (!filter_obj) return false;

    /* Check direct name */
    if (filter_obj->type == PDF_OBJ_NAME) {
        return (strcmp(filter_obj->name, "DCTDecode") == 0 ||
                strcmp(filter_obj->name, "DCT") == 0 ||
                strcmp(filter_obj->name, "JPXDecode") == 0);
    }

    /* Check array of filters -- last filter determines the final format */
    if (filter_obj->type == PDF_OBJ_ARRAY && pdf_array_len(filter_obj->array) > 0) {
        PdfObj *last = pdf_array_get(filter_obj->array,
                                      pdf_array_len(filter_obj->array) - 1);
        if (last && last->type == PDF_OBJ_NAME) {
            return (strcmp(last->name, "DCTDecode") == 0 ||
                    strcmp(last->name, "DCT") == 0 ||
                    strcmp(last->name, "JPXDecode") == 0);
        }
    }

    return false;
}

/*
 * Blit a completed HBITMAP image to the render context DC at the CTM position.
 * Used by both raw-pixel and WIC-decoded image paths.
 */
/*
 * Compute the destination rectangle in device pixels from the current CTM.
 * Images in PDF are defined in a 1x1 unit square; the CTM scales and positions them.
 */
static void compute_image_dest_rect(PdfRenderCtx *ctx,
                                      int *out_x, int *out_y, int *out_w, int *out_h)
{
    /* Transform the image unit square corners through the CTM.
     * PDF images: (0,0)=bottom-left, (0,1)=top-left of image data.
     * Image data is top-to-bottom, so in a top-down DIB, source row 0
     * corresponds to unit-square point (x,1) and source row H corresponds
     * to (x,0).  We need StretchBlt to map:
     *   source (0,0) → device position of unit-square (0,1)  [first row]
     *   source (W,H) → device position of unit-square (1,0)  [last row]
     * If the CTM flips the image (common: image cm has d<0), we use
     * negative dest_w/dest_h so StretchBlt mirrors correctly. */
    PdfMatrix m = current_gs(ctx)->ctm;
    double x_tl, y_tl; /* top-left of image = unit (0,1) */
    double x_tr, y_tr; /* top-right = unit (1,1) */
    double x_bl, y_bl; /* bottom-left = unit (0,0) */

    pdf_transform_point(m, 0.0, 1.0, &x_tl, &y_tl);
    pdf_transform_point(m, 1.0, 1.0, &x_tr, &y_tr);
    pdf_transform_point(m, 0.0, 0.0, &x_bl, &y_bl);

    /* dest_x/y = where source (0,0) = image top-left goes.
     * dest_w/h = signed size; negative values cause StretchBlt to mirror. */
    *out_x = (int)(x_tl + 0.5);
    *out_y = (int)(y_tl + 0.5);
    *out_w = (int)(x_tr - x_tl + 0.5);
    *out_h = (int)(y_bl - y_tl + 0.5);

    /* Ensure non-zero dimensions */
    if (*out_w == 0) *out_w = 1;
    if (*out_h == 0) *out_h = 1;
}

static void blit_image_to_dc(PdfRenderCtx *ctx, HBITMAP hbm, int width, int height)
{
    int dest_x, dest_y, dest_w, dest_h;
    compute_image_dest_rect(ctx, &dest_x, &dest_y, &dest_w, &dest_h);

    HDC mem_dc = CreateCompatibleDC(ctx->hdc);
    HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);
    SetStretchBltMode(ctx->hdc, HALFTONE);
    SetBrushOrgEx(ctx->hdc, 0, 0, NULL);
    StretchBlt(ctx->hdc, dest_x, dest_y, dest_w, dest_h,
               mem_dc, 0, 0, width, height, SRCCOPY);
    SelectObject(mem_dc, old_bm);
    DeleteDC(mem_dc);
}

/*
 * Blit an image with per-pixel alpha (premultiplied) using AlphaBlend.
 * Used for images with SMask (soft mask / transparency).
 * The hbm must have premultiplied alpha in its BGRA data.
 */
static void blit_image_to_dc_alpha(PdfRenderCtx *ctx, HBITMAP hbm,
                                     int width, int height)
{
    int dest_x, dest_y, dest_w, dest_h;
    compute_image_dest_rect(ctx, &dest_x, &dest_y, &dest_w, &dest_h);

    HDC mem_dc = CreateCompatibleDC(ctx->hdc);
    HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);
    SetStretchBltMode(ctx->hdc, HALFTONE);

    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;    /* use per-pixel alpha */
    bf.AlphaFormat = AC_SRC_ALPHA;   /* source has premultiplied alpha */

    AlphaBlend(ctx->hdc, dest_x, dest_y, dest_w, dest_h,
               mem_dc, 0, 0, width, height, bf);

    SelectObject(mem_dc, old_bm);
    DeleteDC(mem_dc);
}

/*
 * Decode a JPEG/JPX image using WIC (Windows Imaging Component).
 * Returns an HBITMAP with 32bpp BGRA data, or NULL on failure.
 * Sets *out_w and *out_h to the decoded image dimensions.
 */
static HBITMAP decode_image_wic(HDC hdc, const uint8_t *data, size_t data_len,
                                 int *out_w, int *out_h)
{
    /* WIC COM interfaces -- we use C-style COM (no C++ needed) */
    /* IIDs and CLSIDs for WIC */
    static const GUID CLSID_WICImagingFactory_local =
        {0xcacaf262,0x9370,0x4615,{0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
    static const GUID IID_IWICImagingFactory_local =
        {0xec5ec8a9,0xc395,0x4314,{0x9c,0x77,0x54,0xd7,0xa9,0x35,0xff,0x70}};
    static const GUID GUID_WICPixelFormat32bppBGRA_local =
        {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x10}};
    static const GUID IID_IWICBitmapDecoder_local =
        {0x9edde9e7,0x8dee,0x47ea,{0x99,0xdf,0xe6,0xfa,0xf2,0xed,0x44,0xbf}};
    static const GUID IID_IWICStream_local =
        {0x135ff860,0x22b7,0x4ddf,{0xb0,0xf6,0x21,0x8f,0x4f,0x29,0x9a,0x43}};
    static const GUID IID_IWICFormatConverter_local =
        {0x00000301,0xa8f2,0x4877,{0xba,0x0a,0xfd,0x2b,0x66,0x45,0xfb,0x94}};
    static const GUID IID_IWICBitmapFrameDecode_local =
        {0x3b16811b,0x6a43,0x4ec9,{0xa8,0x13,0x3d,0x93,0x0c,0x13,0xb9,0x40}};

    *out_w = 0;
    *out_h = 0;

    /* Ensure COM is initialized (safe to call multiple times) */
    static bool com_initialized = false;
    if (!com_initialized) {
        HRESULT hr_com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr_com) || hr_com == S_FALSE /* already initialized */)
            com_initialized = true;
        else
            return NULL;
    }

    HRESULT hr;
    IUnknown *factory_unk = NULL;

    hr = CoCreateInstance(&CLSID_WICImagingFactory_local, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory_local, (void **)&factory_unk);
    if (FAILED(hr) || !factory_unk) return NULL;

    /* We'll use the raw COM vtable approach since we're in C.
     * Cast to function pointers via the vtable. */
    /* IWICImagingFactory vtable layout (IUnknown + factory methods):
     *   0: QueryInterface, 1: AddRef, 2: Release,
     *   3: CreateDecoderFromFilename, 4: CreateDecoderFromStream,
     *   5: CreateDecoderFromFileHandle, 6: CreateComponentInfo,
     *   7: CreateDecoder, 8: CreateEncoder,
     *   9: CreatePalette, 10: CreateFormatConverter,
     *  11: CreateBitmapScaler, 12: CreateBitmapClipper,
     *  13: CreateBitmapFlipRotator, 14: CreateStream, ... */

    void **factory_vtbl = *(void ***)factory_unk;
    typedef HRESULT (STDMETHODCALLTYPE *FnRelease)(IUnknown *);
    typedef HRESULT (STDMETHODCALLTYPE *FnCreateStream)(IUnknown *, IUnknown **);
    typedef HRESULT (STDMETHODCALLTYPE *FnCreateFormatConverter)(IUnknown *, IUnknown **);
    typedef HRESULT (STDMETHODCALLTYPE *FnCreateDecoderFromStream)(
        IUnknown *, IUnknown *, const GUID *, DWORD, IUnknown **);

    HBITMAP result = NULL;
    IUnknown *wic_stream = NULL;
    IUnknown *decoder = NULL;
    IUnknown *frame = NULL;
    IUnknown *converter = NULL;

    /* Create WIC stream */
    FnCreateStream fnCreateStream = (FnCreateStream)factory_vtbl[14];
    hr = fnCreateStream(factory_unk, &wic_stream);
    if (FAILED(hr) || !wic_stream) goto wic_cleanup;

    /* IWICStream vtable: IUnknown(3) + IStream(9) + InitializeFromMemory(at index 15) */
    {
        void **stream_vtbl = *(void ***)wic_stream;
        /* InitializeFromMemory is at vtable index 15:
         * IUnknown: QI(0), AddRef(1), Release(2)
         * ISequentialStream: Read(3), Write(4)
         * IStream: Seek(5), SetSize(6), CopyTo(7), Commit(8), Revert(9),
         *          LockRegion(10), UnlockRegion(11), Stat(12), Clone(13)
         * IWICStream: InitializeFromIStream(14), InitializeFromFilename(15),
         *             InitializeFromMemory(16), InitializeFromIStreamRegion(17) */
        typedef HRESULT (STDMETHODCALLTYPE *FnInitFromMem)(
            IUnknown *, uint8_t *, DWORD);
        FnInitFromMem fnInitFromMem = (FnInitFromMem)stream_vtbl[16];
        hr = fnInitFromMem(wic_stream, (uint8_t *)data, (DWORD)data_len);
        if (FAILED(hr)) goto wic_cleanup;
    }

    /* Create decoder from stream */
    {
        FnCreateDecoderFromStream fnDecode =
            (FnCreateDecoderFromStream)factory_vtbl[4];
        static const GUID GUID_NULL_local =
            {0x00000000,0x0000,0x0000,{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}};
        hr = fnDecode(factory_unk, wic_stream, &GUID_NULL_local,
                      0 /*WICDecodeMetadataCacheOnDemand*/, &decoder);
        if (FAILED(hr) || !decoder) goto wic_cleanup;
    }

    /* Get first frame */
    {
        void **dec_vtbl = *(void ***)decoder;
        /* IWICBitmapDecoder vtable:
         * IUnknown(3) + QueryCapability(3), Initialize(4), GetContainerFormat(5),
         * GetDecoderInfo(6), CopyPalette(7), GetMetadataQueryReader(8),
         * GetPreview(9), GetColorContexts(10), GetThumbnail(11),
         * GetFrameCount(12), GetFrame(13) */
        typedef HRESULT (STDMETHODCALLTYPE *FnGetFrame)(
            IUnknown *, UINT, IUnknown **);
        FnGetFrame fnGetFrame = (FnGetFrame)dec_vtbl[13];
        hr = fnGetFrame(decoder, 0, &frame);
        if (FAILED(hr) || !frame) goto wic_cleanup;
    }

    /* Get frame dimensions */
    UINT img_w = 0, img_h = 0;
    {
        void **frame_vtbl = *(void ***)frame;
        /* IWICBitmapFrameDecode inherits IWICBitmapSource:
         * IUnknown(3) + GetSize(3), GetPixelFormat(4), GetResolution(5),
         * CopyPalette(6), CopyPixels(7) */
        typedef HRESULT (STDMETHODCALLTYPE *FnGetSize)(
            IUnknown *, UINT *, UINT *);
        FnGetSize fnGetSize = (FnGetSize)frame_vtbl[3];
        hr = fnGetSize(frame, &img_w, &img_h);
        if (FAILED(hr) || img_w == 0 || img_h == 0) goto wic_cleanup;
    }

    /* Create format converter to BGRA32 */
    {
        FnCreateFormatConverter fnCreateConv =
            (FnCreateFormatConverter)factory_vtbl[10];
        hr = fnCreateConv(factory_unk, &converter);
        if (FAILED(hr) || !converter) goto wic_cleanup;
    }

    /* Initialize converter */
    {
        void **conv_vtbl = *(void ***)converter;
        /* IWICFormatConverter: IUnknown(3) + IWICBitmapSource(5) + Initialize(8) */
        typedef HRESULT (STDMETHODCALLTYPE *FnInitialize)(
            IUnknown *, IUnknown *, const GUID *, DWORD, IUnknown *, double, DWORD);
        FnInitialize fnInit = (FnInitialize)conv_vtbl[8];
        hr = fnInit(converter, frame, &GUID_WICPixelFormat32bppBGRA_local,
                    0 /*WICBitmapDitherTypeNone*/, NULL, 0.0,
                    0 /*WICBitmapPaletteTypeCustom*/);
        if (FAILED(hr)) goto wic_cleanup;
    }

    /* Create DIB and copy pixels */
    {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)img_w;
        bmi.bmiHeader.biHeight      = -(LONG)img_h; /* top-down */
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        uint8_t *dib_bits = NULL;
        result = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS,
                                   (void **)&dib_bits, NULL, 0);
        if (!result || !dib_bits) {
            if (result) { DeleteObject(result); result = NULL; }
            goto wic_cleanup;
        }

        /* CopyPixels from the converter */
        void **conv_vtbl = *(void ***)converter;
        typedef HRESULT (STDMETHODCALLTYPE *FnCopyPixels)(
            IUnknown *, const void * /*WICRect*/, UINT, UINT, uint8_t *);
        FnCopyPixels fnCopy = (FnCopyPixels)conv_vtbl[7];
        UINT stride = img_w * 4;
        hr = fnCopy(converter, NULL, stride, stride * img_h, dib_bits);
        if (FAILED(hr)) {
            DeleteObject(result);
            result = NULL;
            goto wic_cleanup;
        }

        *out_w = (int)img_w;
        *out_h = (int)img_h;
    }

wic_cleanup:
    if (converter) { ((FnRelease)((*(void ***)converter)[2]))(converter); }
    if (frame)     { ((FnRelease)((*(void ***)frame)[2]))(frame); }
    if (decoder)   { ((FnRelease)((*(void ***)decoder)[2]))(decoder); }
    if (wic_stream){ ((FnRelease)((*(void ***)wic_stream)[2]))(wic_stream); }
    if (factory_unk){ ((FnRelease)((*(void ***)factory_unk)[2]))(factory_unk); }

    return result;
}

/*
 * Render an image XObject to the DC.
 * The image is placed at the current CTM position and scaled by the CTM.
 *
 * Handles:
 *  - Simple color spaces: DeviceGray, DeviceRGB, DeviceCMYK, CalGray, CalRGB
 *  - Indexed color spaces: [/Indexed base hival lookup]
 *  - ICCBased color spaces (by component count)
 *  - DCTDecode (JPEG) images via WIC
 *  - SMask (soft mask / transparency) via AlphaBlend
 */

/*
 * Decode an SMask (soft mask) stream into an alpha buffer.
 * Returns a malloc'd array of width*height bytes, or NULL on failure.
 * The SMask is a grayscale image defining per-pixel opacity (0=transparent, 255=opaque).
 */
static uint8_t *decode_smask(PdfRenderCtx *ctx, PdfDict *image_dict,
                               int img_width, int img_height)
{
    PdfObj *smask_obj = pdf_dict_get(image_dict, "SMask");
    if (!smask_obj) return NULL;
    smask_obj = pdf_resolve(ctx->doc, smask_obj);
    if (!smask_obj || smask_obj->type != PDF_OBJ_STREAM) return NULL;

    PdfStream *smask_stream = smask_obj->stream;
    if (!smask_stream || !smask_stream->dict) return NULL;

    int sw = pdf_dict_get_int(smask_stream->dict, "Width", 0);
    int sh = pdf_dict_get_int(smask_stream->dict, "Height", 0);
    int sbpc = pdf_dict_get_int(smask_stream->dict, "BitsPerComponent", 8);

    /* SMask must match image dimensions and be 8-bit grayscale */
    if (sw != img_width || sh != img_height || sbpc != 8) return NULL;

    /* Decode the SMask stream */
    if (!smask_stream->decoded_data) {
        if (!pdf_decode_stream(ctx->doc, smask_stream))
            return NULL;
    }
    if (!smask_stream->decoded_data) return NULL;

    size_t expected = (size_t)sw * sh;
    if (smask_stream->decoded_length < expected) return NULL;

    /* Copy the alpha data */
    uint8_t *alpha = (uint8_t *)malloc(expected);
    if (!alpha) return NULL;
    memcpy(alpha, smask_stream->decoded_data, expected);
    return alpha;
}

static void render_image(PdfRenderCtx *ctx, PdfObj *xobj)
{
    if (!xobj || xobj->type != PDF_OBJ_STREAM) return;

    PdfStream *stream = xobj->stream;
    PdfDict *dict = stream->dict;
    if (!dict) return;

    int width  = pdf_dict_get_int(dict, "Width", 0);
    int height = pdf_dict_get_int(dict, "Height", 0);
    int bpc    = pdf_dict_get_int(dict, "BitsPerComponent", 8);
    if (width <= 0 || height <= 0) return;

    /* Check if the image is JPEG-encoded (DCTDecode).
     * For JPEG images, we use the raw stream data and WIC to decode. */
    if (image_has_jpeg_filter(dict)) {
        const uint8_t *jpeg_data = stream->raw_data;
        size_t jpeg_len = stream->raw_length;
        if (!jpeg_data || jpeg_len == 0) return;

        int img_w = 0, img_h = 0;
        HBITMAP hbm = decode_image_wic(ctx->hdc, jpeg_data, jpeg_len,
                                        &img_w, &img_h);
        if (hbm) {
            blit_image_to_dc(ctx, hbm, img_w, img_h);
            DeleteObject(hbm);
        }
        return;
    }

    /* Check for ImageMask */
    PdfObj *imask_obj = pdf_dict_get(dict, "ImageMask");
    if (imask_obj && imask_obj->type == PDF_OBJ_REF)
        imask_obj = pdf_resolve(ctx->doc, imask_obj);
    bool is_image_mask = (imask_obj && imask_obj->type == PDF_OBJ_BOOL && imask_obj->boolean);

    /* For ImageMask, BitsPerComponent is implicitly 1 */
    if (is_image_mask) bpc = 1;

    /* Check Decode array for inversion (relevant for 1-bit images) */
    bool decode_inverted = false;
    if (bpc == 1) {
        PdfObj *decode_obj = pdf_dict_get(dict, "Decode");
        if (decode_obj && decode_obj->type == PDF_OBJ_REF)
            decode_obj = pdf_resolve(ctx->doc, decode_obj);
        if (decode_obj && decode_obj->type == PDF_OBJ_ARRAY) {
            PdfArray *darr = decode_obj->array;
            if (darr->count >= 2) {
                PdfObj *d0 = pdf_resolve(ctx->doc, darr->items[0]);
                PdfObj *d1 = pdf_resolve(ctx->doc, darr->items[1]);
                double v0 = 0.0, v1 = 1.0;
                if (d0) {
                    if (d0->type == PDF_OBJ_REAL) v0 = d0->real;
                    else if (d0->type == PDF_OBJ_INT) v0 = (double)d0->integer;
                }
                if (d1) {
                    if (d1->type == PDF_OBJ_REAL) v1 = d1->real;
                    else if (d1->type == PDF_OBJ_INT) v1 = (double)d1->integer;
                }
                if (v0 == 1.0 && v1 == 0.0)
                    decode_inverted = true;
            }
        }
    }

    /* Resolve color space (handles Indexed, ICCBased, etc.) */
    bool is_indexed = false;
    uint8_t palette_rgb[256 * 3]; /* up to 256 RGB entries */
    int palette_count = 0;
    int components = resolve_image_colorspace(ctx->doc, dict,
                                              &is_indexed,
                                              palette_rgb, &palette_count);

    /* For ImageMask, component count is 1 (it's a stencil, not color data) */
    if (is_image_mask) components = 1;

    /* Decode stream data (FlateDecode, CCITTFaxDecode, etc.) */
    if (!stream->decoded_data) {
        if (!pdf_decode_stream(ctx->doc, stream))
            return;
    }
    if (!stream->decoded_data) return;

    const uint8_t *src = stream->decoded_data;
    size_t src_len = stream->decoded_length;

    /* --- Handle 1-bit images (ImageMask or regular 1-bpc grayscale) --- */
    if (bpc == 1) {
        size_t row_bytes = ((size_t)width + 7) / 8;
        size_t expected_1bit = row_bytes * height;
        if (src_len < expected_1bit) return;

        /* Create a DIB section for the image */
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = width;
        bmi.bmiHeader.biHeight      = -height; /* top-down */
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        uint8_t *dib_bits = NULL;
        HBITMAP hbm = CreateDIBSection(ctx->hdc, &bmi, DIB_RGB_COLORS,
                                        (void **)&dib_bits, NULL, 0);
        if (!hbm || !dib_bits) {
            if (hbm) DeleteObject(hbm);
            return;
        }

        if (is_image_mask) {
            /* ImageMask: render as stencil with current fill color.
             * Use 4x4 sub-pixel sampling for anti-aliased edges.
             * For each destination pixel, sample the source bitmap at 16 sub-pixel
             * positions and use the coverage count as alpha for blending. */
            DeleteObject(hbm); /* don't need the pre-allocated DIB */

            PdfColor fill = current_gs(ctx)->fill_color;
            int fr = (int)(fill.r * 255.0 + 0.5);
            int fg = (int)(fill.g * 255.0 + 0.5);
            int fb = (int)(fill.b * 255.0 + 0.5);

            int dest_x, dest_y, dest_w, dest_h;
            compute_image_dest_rect(ctx, &dest_x, &dest_y, &dest_w, &dest_h);
            if (dest_w <= 0 || dest_h <= 0) return;

            /* Get page bitmap for direct pixel access */
            int page_w, page_h, page_stride;
            uint8_t *page_bits = get_page_bits(ctx->hdc, &page_w, &page_h, &page_stride);
            if (!page_bits) {
                /* Fallback: use the old TransparentBlt approach without AA */
                hbm = CreateDIBSection(ctx->hdc, &bmi, DIB_RGB_COLORS,
                                       (void **)&dib_bits, NULL, 0);
                if (!hbm || !dib_bits) { if (hbm) DeleteObject(hbm); return; }
                uint8_t tr_r = 255, tr_g = 0, tr_b = 255;
                if (fr == 255 && fg == 0 && fb == 255) { tr_r = 0; tr_g = 255; tr_b = 0; }
                for (int py = 0; py < height; py++) {
                    const uint8_t *row = src + (size_t)py * row_bytes;
                    for (int px = 0; px < width; px++) {
                        int byte_idx = px / 8;
                        int bit_idx = 7 - (px % 8);
                        int bit = (row[byte_idx] >> bit_idx) & 1;
                        bool painted = decode_inverted ? (bit == 1) : (bit == 0);
                        size_t dst_idx = ((size_t)py * width + px) * 4;
                        if (painted) {
                            dib_bits[dst_idx+0]=(uint8_t)fb; dib_bits[dst_idx+1]=(uint8_t)fg;
                            dib_bits[dst_idx+2]=(uint8_t)fr; dib_bits[dst_idx+3]=255;
                        } else {
                            dib_bits[dst_idx+0]=tr_b; dib_bits[dst_idx+1]=tr_g;
                            dib_bits[dst_idx+2]=tr_r; dib_bits[dst_idx+3]=255;
                        }
                    }
                }
                HDC mem_dc = CreateCompatibleDC(ctx->hdc);
                HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);
                SetStretchBltMode(ctx->hdc, COLORONCOLOR);
                TransparentBlt(ctx->hdc, dest_x, dest_y, dest_w, dest_h,
                               mem_dc, 0, 0, width, height, RGB(tr_r, tr_g, tr_b));
                SelectObject(mem_dc, old_bm);
                DeleteDC(mem_dc);
                DeleteObject(hbm);
                return;
            }

            GdiFlush();

            /* Box-filter anti-aliasing: for each destination pixel, count all
             * source pixels that map to it and use the painted fraction as alpha.
             * This properly handles any scale ratio (both down and up scaling).
             *
             * For downscaling (e.g., 3753→864): each dest pixel covers ~4.3 source
             * pixels per axis, and we count all ~18 source pixels for accurate AA.
             * For upscaling: falls back to point sampling (1 source per dest). */
            for (int dy = 0; dy < dest_h; dy++) {
                int py = dest_y + dy;
                if (py < 0 || py >= page_h) continue;
                uint8_t *page_row = page_bits + py * page_stride;

                /* Source Y range covered by this dest pixel row */
                int src_y0 = (int)((double)dy / dest_h * height);
                int src_y1 = (int)((double)(dy + 1) / dest_h * height);
                if (src_y1 <= src_y0) src_y1 = src_y0 + 1;
                if (src_y0 < 0) src_y0 = 0;
                if (src_y1 > height) src_y1 = height;

                for (int dx = 0; dx < dest_w; dx++) {
                    int ppx = dest_x + dx;
                    if (ppx < 0 || ppx >= page_w) continue;

                    /* Source X range covered by this dest pixel column */
                    int src_x0 = (int)((double)dx / dest_w * width);
                    int src_x1 = (int)((double)(dx + 1) / dest_w * width);
                    if (src_x1 <= src_x0) src_x1 = src_x0 + 1;
                    if (src_x0 < 0) src_x0 = 0;
                    if (src_x1 > width) src_x1 = width;

                    /* Count painted source pixels in this area */
                    int painted = 0;
                    int total = 0;
                    for (int iy = src_y0; iy < src_y1; iy++) {
                        const uint8_t *srow = src + (size_t)iy * row_bytes;
                        for (int ix = src_x0; ix < src_x1; ix++) {
                            int byte_idx = ix / 8;
                            int bit_idx = 7 - (ix % 8);
                            int bit = (srow[byte_idx] >> bit_idx) & 1;
                            bool is_painted = decode_inverted ? (bit == 1) : (bit == 0);
                            if (is_painted) painted++;
                            total++;
                        }
                    }

                    if (painted == 0) continue;

                    /* Convert coverage to alpha (0-255) */
                    int alpha = (total > 0) ? (painted * 255 / total) : 0;
                    if (alpha > 255) alpha = 255;

                    /* Alpha blend fill color onto page pixel */
                    uint8_t *pixel = page_row + ppx * 4;
                    if (alpha >= 255) {
                        pixel[0] = (uint8_t)fb;
                        pixel[1] = (uint8_t)fg;
                        pixel[2] = (uint8_t)fr;
                        pixel[3] = 255;
                    } else {
                        int inv = 255 - alpha;
                        pixel[0] = (uint8_t)((fb * alpha + pixel[0] * inv + 127) / 255);
                        pixel[1] = (uint8_t)((fg * alpha + pixel[1] * inv + 127) / 255);
                        pixel[2] = (uint8_t)((fr * alpha + pixel[2] * inv + 127) / 255);
                        pixel[3] = 255;
                    }
                }
            }
        } else {
            /* Regular 1-bit grayscale image (not a mask).
             * Default Decode [0 1]: bit 0 -> black (0), bit 1 -> white (255)
             * Inverted Decode [1 0]: bit 0 -> white (255), bit 1 -> black (0) */
            for (int py = 0; py < height; py++) {
                const uint8_t *row = src + (size_t)py * row_bytes;
                for (int px = 0; px < width; px++) {
                    int byte_idx = px / 8;
                    int bit_idx = 7 - (px % 8);
                    int bit = (row[byte_idx] >> bit_idx) & 1;

                    uint8_t gray;
                    if (decode_inverted) {
                        gray = (bit == 0) ? 255 : 0;
                    } else {
                        gray = (bit == 1) ? 255 : 0;
                    }

                    size_t dst_idx = ((size_t)py * width + px) * 4;
                    dib_bits[dst_idx + 0] = gray; /* Blue */
                    dib_bits[dst_idx + 1] = gray; /* Green */
                    dib_bits[dst_idx + 2] = gray; /* Red */
                    dib_bits[dst_idx + 3] = 255;
                }
            }

            blit_image_to_dc(ctx, hbm, width, height);
            DeleteObject(hbm);
        }
        return;
    }

    /* --- Handle 8-bit (and higher) images --- */
    if (bpc != 8) return;

    size_t expected = (size_t)width * height * components;
    if (src_len < expected) return;

    /* Check for SMask (soft mask / transparency) */
    uint8_t *smask_alpha = decode_smask(ctx, dict, width, height);
    bool has_alpha = (smask_alpha != NULL);

    /* Create a DIB section for the image */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint8_t *dib_bits = NULL;
    HBITMAP hbm = CreateDIBSection(ctx->hdc, &bmi, DIB_RGB_COLORS,
                                    (void **)&dib_bits, NULL, 0);
    if (!hbm || !dib_bits) {
        if (hbm) DeleteObject(hbm);
        free(smask_alpha);
        return;
    }

    /* Convert source pixels to BGRA */
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            size_t src_idx = ((size_t)py * width + px) * components;
            size_t dst_idx = ((size_t)py * width + px) * 4;
            uint8_t r, g, b;

            if (is_indexed) {
                uint8_t idx = src[src_idx];
                if (idx < palette_count) {
                    r = palette_rgb[idx * 3 + 0];
                    g = palette_rgb[idx * 3 + 1];
                    b = palette_rgb[idx * 3 + 2];
                } else {
                    r = g = b = 0;
                }
            } else if (components == 1) {
                r = g = b = src[src_idx];
            } else if (components == 3) {
                r = src[src_idx];
                g = src[src_idx + 1];
                b = src[src_idx + 2];
            } else if (components == 4) {
                /* CMYK to RGB */
                double c_ = src[src_idx]     / 255.0;
                double m_ = src[src_idx + 1] / 255.0;
                double y_ = src[src_idx + 2] / 255.0;
                double k_ = src[src_idx + 3] / 255.0;
                /* Use ink cross-contamination model (same as cmyk_to_rgb) */
                PdfColor img_rgb = cmyk_to_rgb(c_, m_, y_, k_);
                r = (uint8_t)(img_rgb.r * 255.0 + 0.5);
                g = (uint8_t)(img_rgb.g * 255.0 + 0.5);
                b = (uint8_t)(img_rgb.b * 255.0 + 0.5);
            } else {
                r = g = b = 128;
            }

            if (has_alpha) {
                /* Apply SMask alpha and premultiply for AlphaBlend.
                 * AlphaBlend requires premultiplied alpha: color = color * alpha / 255 */
                uint8_t a = smask_alpha[(size_t)py * width + px];
                dib_bits[dst_idx + 0] = (uint8_t)((b * a + 127) / 255); /* Blue * alpha */
                dib_bits[dst_idx + 1] = (uint8_t)((g * a + 127) / 255); /* Green * alpha */
                dib_bits[dst_idx + 2] = (uint8_t)((r * a + 127) / 255); /* Red * alpha */
                dib_bits[dst_idx + 3] = a;
            } else {
                dib_bits[dst_idx + 0] = b;
                dib_bits[dst_idx + 1] = g;
                dib_bits[dst_idx + 2] = r;
                dib_bits[dst_idx + 3] = 255;
            }
        }
    }

    if (has_alpha) {
        blit_image_to_dc_alpha(ctx, hbm, width, height);
    } else {
        blit_image_to_dc(ctx, hbm, width, height);
    }
    DeleteObject(hbm);
    free(smask_alpha);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Content Stream Interpreter
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * The main interpreter loop: tokenize the content stream, build an operand
 * stack, and dispatch operators.
 */

/* Forward declaration for recursive form XObject interpretation */
static void interpret_stream(PdfRenderCtx *ctx, PdfDict *resources,
                              const uint8_t *data, size_t length);

/*
 * Handle the `gs` operator: apply ExtGState parameters from the graphics
 * state dictionary in the page resources.
 */
static void apply_extgstate(PdfRenderCtx *ctx, PdfDict *resources,
                             const char *gs_name)
{
    PdfObj *extgs = pdf_dict_get(resources, "ExtGState");
    if (!extgs) return;
    extgs = pdf_resolve(ctx->doc, extgs);
    if (!extgs || extgs->type != PDF_OBJ_DICT) return;

    PdfObj *state = pdf_dict_get(extgs->dict, gs_name);
    if (!state) return;
    state = pdf_resolve(ctx->doc, state);
    if (!state || state->type != PDF_OBJ_DICT) return;

    PdfGraphicsState *gs = current_gs(ctx);

    /* Line width */
    PdfObj *lw = pdf_dict_get(state->dict, "LW");
    if (lw) {
        lw = pdf_resolve(ctx->doc, lw);
        if (lw) {
            if (lw->type == PDF_OBJ_REAL) gs->line_width = lw->real;
            else if (lw->type == PDF_OBJ_INT) gs->line_width = (double)lw->integer;
        }
    }

    /* Line cap */
    PdfObj *lc = pdf_dict_get(state->dict, "LC");
    if (lc) {
        lc = pdf_resolve(ctx->doc, lc);
        if (lc && lc->type == PDF_OBJ_INT) gs->line_cap = (int)lc->integer;
    }

    /* Line join */
    PdfObj *lj = pdf_dict_get(state->dict, "LJ");
    if (lj) {
        lj = pdf_resolve(ctx->doc, lj);
        if (lj && lj->type == PDF_OBJ_INT) gs->line_join = (int)lj->integer;
    }

    /* Miter limit */
    PdfObj *ml = pdf_dict_get(state->dict, "ML");
    if (ml) {
        ml = pdf_resolve(ctx->doc, ml);
        if (ml) {
            if (ml->type == PDF_OBJ_REAL) gs->miter_limit = ml->real;
            else if (ml->type == PDF_OBJ_INT) gs->miter_limit = (double)ml->integer;
        }
    }

    /* Dash pattern: D is [dash_array dash_phase] */
    PdfObj *dp = pdf_dict_get(state->dict, "D");
    if (dp) {
        dp = pdf_resolve(ctx->doc, dp);
        if (dp && dp->type == PDF_OBJ_ARRAY && pdf_array_len(dp->array) >= 2) {
            PdfObj *da = pdf_array_get(dp->array, 0);
            PdfObj *dph = pdf_array_get(dp->array, 1);
            da = pdf_resolve(ctx->doc, da);
            dph = pdf_resolve(ctx->doc, dph);
            gs->dash_count = 0;
            gs->dash_phase = 0;
            if (da && da->type == PDF_OBJ_ARRAY) {
                int n = pdf_array_len(da->array);
                if (n > 10) n = 10;
                for (int di = 0; di < n; di++) {
                    PdfObj *v = pdf_array_get(da->array, di);
                    v = pdf_resolve(ctx->doc, v);
                    if (v) {
                        if (v->type == PDF_OBJ_REAL) gs->dash_array[gs->dash_count++] = v->real;
                        else if (v->type == PDF_OBJ_INT) gs->dash_array[gs->dash_count++] = (double)v->integer;
                    }
                }
            }
            if (dph) {
                if (dph->type == PDF_OBJ_REAL) gs->dash_phase = dph->real;
                else if (dph->type == PDF_OBJ_INT) gs->dash_phase = (double)dph->integer;
            }
        }
    }

    /* Font */
    PdfObj *font = pdf_dict_get(state->dict, "Font");
    if (font) {
        font = pdf_resolve(ctx->doc, font);
        if (font && font->type == PDF_OBJ_ARRAY && pdf_array_len(font->array) >= 2) {
            PdfObj *f_ref = pdf_array_get(font->array, 0);
            PdfObj *f_size = pdf_array_get(font->array, 1);
            if (f_ref) {
                f_ref = pdf_resolve(ctx->doc, f_ref);
                if (f_ref && f_ref->type == PDF_OBJ_DICT) {
                    const char *bf = pdf_dict_get_name(f_ref->dict, "BaseFont");
                    if (bf) {
                        strncpy(gs->font_name, bf, PDF_MAX_NAME_LEN - 1);
                        gs->font_name[PDF_MAX_NAME_LEN - 1] = '\0';
                    }
                }
            }
            if (f_size) {
                f_size = pdf_resolve(ctx->doc, f_size);
                if (f_size) {
                    if (f_size->type == PDF_OBJ_REAL) gs->font_size = f_size->real;
                    else if (f_size->type == PDF_OBJ_INT) gs->font_size = (double)f_size->integer;
                }
            }
        }
    }
}

/*
 * Handle the `Do` operator: invoke an XObject from page resources.
 */
static void do_xobject(PdfRenderCtx *ctx, PdfDict *resources, const char *name)
{
    PdfObj *xobjects = pdf_dict_get(resources, "XObject");
    if (!xobjects) return;
    xobjects = pdf_resolve(ctx->doc, xobjects);
    if (!xobjects || xobjects->type != PDF_OBJ_DICT) return;

    PdfObj *xobj = pdf_dict_get(xobjects->dict, name);
    if (!xobj) return;
    xobj = pdf_resolve(ctx->doc, xobj);
    if (!xobj) return;

    if (xobj->type == PDF_OBJ_STREAM) {
        const char *subtype = pdf_dict_get_name(xobj->stream->dict, "Subtype");
        if (!subtype) return;

        if (strcmp(subtype, "Image") == 0) {
            PROF_START(img);
            render_image(ctx, xobj);
            PROF_END(image, img);
        } else if (strcmp(subtype, "Form") == 0) {
            /* Form XObject: recursively interpret its content stream */
            gs_save(ctx);

            /* Apply the form's /Matrix if present */
            PdfArray *matrix_arr = pdf_dict_get_array(xobj->stream->dict, "Matrix");
            if (matrix_arr && pdf_array_len(matrix_arr) >= 6) {
                PdfMatrix form_m;
                PdfObj *v;
                v = pdf_resolve(ctx->doc, pdf_array_get(matrix_arr, 0));
                form_m.a = v ? (v->type == PDF_OBJ_REAL ? v->real : (double)v->integer) : 1.0;
                v = pdf_resolve(ctx->doc, pdf_array_get(matrix_arr, 1));
                form_m.b = v ? (v->type == PDF_OBJ_REAL ? v->real : (double)v->integer) : 0.0;
                v = pdf_resolve(ctx->doc, pdf_array_get(matrix_arr, 2));
                form_m.c = v ? (v->type == PDF_OBJ_REAL ? v->real : (double)v->integer) : 0.0;
                v = pdf_resolve(ctx->doc, pdf_array_get(matrix_arr, 3));
                form_m.d = v ? (v->type == PDF_OBJ_REAL ? v->real : (double)v->integer) : 1.0;
                v = pdf_resolve(ctx->doc, pdf_array_get(matrix_arr, 4));
                form_m.e = v ? (v->type == PDF_OBJ_REAL ? v->real : (double)v->integer) : 0.0;
                v = pdf_resolve(ctx->doc, pdf_array_get(matrix_arr, 5));
                form_m.f = v ? (v->type == PDF_OBJ_REAL ? v->real : (double)v->integer) : 0.0;

                current_gs(ctx)->ctm = pdf_matrix_multiply(form_m, current_gs(ctx)->ctm);
            }

            /* Decode the form's stream */
            if (!xobj->stream->decoded_data)
                pdf_decode_stream(ctx->doc, xobj->stream);

            if (xobj->stream->decoded_data) {
                /* Use the form's own resources, or fall back to the page's */
                PdfObj *form_res = pdf_dict_get(xobj->stream->dict, "Resources");
                PdfDict *res_dict = resources;
                if (form_res) {
                    form_res = pdf_resolve(ctx->doc, form_res);
                    if (form_res && form_res->type == PDF_OBJ_DICT)
                        res_dict = form_res->dict;
                }

                interpret_stream(ctx, res_dict,
                                 xobj->stream->decoded_data,
                                 xobj->stream->decoded_length);
            }

            gs_restore(ctx);
        }
    }
}

/*
 * Render an inline image (BI...ID...EI).
 * Parses the inline image dictionary, extracts pixel data, and renders
 * it to the DC using the current CTM.
 *
 * This handles both regular inline images and 1-bit image masks used
 * by Type3 font glyph streams.
 */
static void render_inline_image(PdfRenderCtx *ctx, CSParser *p)
{
    /* Parse inline image dictionary key/value pairs until "ID" */
    int img_width = 0, img_height = 0, bpc = 8;
    bool is_image_mask = false;
    bool decode_inverted = false; /* true if Decode is [1 0] */
    int components = 1;
    /* Abbreviated inline image keys:
     * /W = Width, /H = Height, /BPC = BitsPerComponent
     * /IM = ImageMask, /D = Decode, /CS = ColorSpace */

    while (!cs_at_end(p)) {
        cs_skip_whitespace_and_comments(p);
        if (cs_at_end(p)) break;

        /* Check for "ID" keyword */
        if (p->pos + 1 < p->length &&
            p->data[p->pos] == 'I' && p->data[p->pos + 1] == 'D') {
            /* Check that ID is followed by a single whitespace */
            if (p->pos + 2 < p->length && cs_is_whitespace(p->data[p->pos + 2])) {
                p->pos += 3; /* skip "ID" + whitespace */
                break;
            }
        }

        /* Parse a token */
        CSToken key_tok;
        if (!cs_next_token(p, &key_tok)) break;

        if (key_tok.type == TOK_NAME) {
            /* Read the value token */
            CSToken val_tok;
            if (!cs_next_token(p, &val_tok)) {
                if (key_tok.str_data) free(key_tok.str_data);
                break;
            }

            /* Match abbreviated or full key names */
            if (strcmp(key_tok.text, "W") == 0 || strcmp(key_tok.text, "Width") == 0) {
                img_width = (int)val_tok.number;
            } else if (strcmp(key_tok.text, "H") == 0 || strcmp(key_tok.text, "Height") == 0) {
                img_height = (int)val_tok.number;
            } else if (strcmp(key_tok.text, "BPC") == 0 || strcmp(key_tok.text, "BitsPerComponent") == 0) {
                bpc = (int)val_tok.number;
            } else if (strcmp(key_tok.text, "IM") == 0 || strcmp(key_tok.text, "ImageMask") == 0) {
                is_image_mask = val_tok.bool_val;
            } else if (strcmp(key_tok.text, "CS") == 0 || strcmp(key_tok.text, "ColorSpace") == 0) {
                /* Parse color space name */
                if (strcmp(val_tok.text, "RGB") == 0 || strcmp(val_tok.text, "DeviceRGB") == 0) {
                    components = 3;
                } else if (strcmp(val_tok.text, "G") == 0 || strcmp(val_tok.text, "DeviceGray") == 0) {
                    components = 1;
                } else if (strcmp(val_tok.text, "CMYK") == 0 || strcmp(val_tok.text, "DeviceCMYK") == 0) {
                    components = 4;
                }
            } else if (strcmp(key_tok.text, "D") == 0 || strcmp(key_tok.text, "Decode") == 0) {
                /* Decode array: for image masks, [1 0] means inverted.
                 * The value token might be '[', so we need to handle the array.
                 * For simplicity, if we see '[', read tokens until ']'. */
                if (val_tok.type == TOK_ARRAY_BEGIN) {
                    CSToken d0_tok, d1_tok, end_tok;
                    if (cs_next_token(p, &d0_tok) && cs_next_token(p, &d1_tok)) {
                        if (d0_tok.number == 1.0 && d1_tok.number == 0.0) {
                            decode_inverted = true;
                        }
                        if (d0_tok.str_data) free(d0_tok.str_data);
                        if (d1_tok.str_data) free(d1_tok.str_data);
                    }
                    /* consume ']' */
                    if (cs_next_token(p, &end_tok)) {
                        if (end_tok.str_data) free(end_tok.str_data);
                    }
                }
            }
            /* /F or /Filter -- we ignore filters for inline images in Type3 glyphs
             * since they're typically uncompressed raw bitmap data */

            if (val_tok.str_data) free(val_tok.str_data);
        }

        if (key_tok.str_data) free(key_tok.str_data);
    }

    /* Now at the start of image data. Find "EI" to determine data extent. */
    size_t data_start = p->pos;
    size_t data_end = data_start;

    /* Calculate expected data size for uncompressed image data */
    size_t row_bits = 0;
    if (is_image_mask) {
        bpc = 1;
        components = 1;
    }
    row_bits = (size_t)img_width * bpc * components;
    size_t row_bytes = (row_bits + 7) / 8;
    size_t expected_data = row_bytes * img_height;

    /* Scan for "EI" to find end of data */
    while (!cs_at_end(p)) {
        if (cs_is_whitespace(p->data[p->pos]) &&
            p->pos + 2 < p->length &&
            p->data[p->pos + 1] == 'E' &&
            p->data[p->pos + 2] == 'I') {
            if (p->pos + 3 >= p->length ||
                cs_is_whitespace(p->data[p->pos + 3]) ||
                cs_is_delimiter(p->data[p->pos + 3])) {
                data_end = p->pos;
                p->pos += 3; /* skip whitespace + "EI" */
                break;
            }
        }
        p->pos++;
    }

    /* Validate dimensions */
    if (img_width <= 0 || img_height <= 0) return;
    size_t data_len = data_end - data_start;
    if (data_len < expected_data) return;

    const uint8_t *img_data = p->data + data_start;

    /* Create DIB section for the image */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = img_width;
    bmi.bmiHeader.biHeight      = -img_height; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint8_t *dib_bits = NULL;
    HBITMAP hbm = CreateDIBSection(ctx->hdc, &bmi, DIB_RGB_COLORS,
                                    (void **)&dib_bits, NULL, 0);
    if (!hbm || !dib_bits) {
        if (hbm) DeleteObject(hbm);
        return;
    }

    /* Get the fill color for image mask rendering */
    PdfColor fill = current_gs(ctx)->fill_color;
    uint8_t fr = (uint8_t)(fill.r * 255.0 + 0.5);
    uint8_t fg = (uint8_t)(fill.g * 255.0 + 0.5);
    uint8_t fb = (uint8_t)(fill.b * 255.0 + 0.5);

    if (is_image_mask && bpc == 1) {
        /* 1-bit image mask: render as stencil with current fill color.
         * Bits are packed MSB-first. Each row is padded to byte boundary.
         *
         * Anti-aliasing for Type3 glyph bitmaps is now handled at the glyph
         * level in render_type3_string() via 4x supersampled offscreen rendering.
         * Here we just do a simple TransparentBlt — painted pixels get the fill
         * color, transparent pixels use a sentinel color key for TransparentBlt. */

        /* Use magenta (0xFF00FF) as the transparent color key */
        COLORREF trans_color = RGB(255, 0, 255);

        for (int py = 0; py < img_height; py++) {
            const uint8_t *row = img_data + (size_t)py * row_bytes;
            for (int px = 0; px < img_width; px++) {
                int byte_idx = px / 8;
                int bit_idx = 7 - (px % 8); /* MSB first */
                int bit = (row[byte_idx] >> bit_idx) & 1;

                /* Apply Decode array to determine paint/transparent:
                 * Default [0 1]: bit=0 -> PAINTED, bit=1 -> TRANSPARENT
                 * Inverted [1 0]: bit=0 -> TRANSPARENT, bit=1 -> PAINTED */
                bool painted;
                if (decode_inverted) {
                    painted = (bit == 1);
                } else {
                    painted = (bit == 0);
                }

                size_t dst_idx = ((size_t)py * img_width + px) * 4;
                if (painted) {
                    dib_bits[dst_idx + 0] = fb;   /* Blue */
                    dib_bits[dst_idx + 1] = fg;   /* Green */
                    dib_bits[dst_idx + 2] = fr;   /* Red */
                    dib_bits[dst_idx + 3] = 255;
                } else {
                    /* Transparent: use magenta color key */
                    dib_bits[dst_idx + 0] = 255;  /* Blue */
                    dib_bits[dst_idx + 1] = 0;    /* Green */
                    dib_bits[dst_idx + 2] = 255;  /* Red */
                    dib_bits[dst_idx + 3] = 255;
                }
            }
        }

        /* Blit with TransparentBlt — simple nearest-neighbor with transparency.
         * The glyph-level supersampling in render_type3_string() handles AA. */
        int dest_x, dest_y, dest_w, dest_h;
        compute_image_dest_rect(ctx, &dest_x, &dest_y, &dest_w, &dest_h);

        HDC mem_dc = CreateCompatibleDC(ctx->hdc);
        HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);
        TransparentBlt(ctx->hdc, dest_x, dest_y, dest_w, dest_h,
                       mem_dc, 0, 0, img_width, img_height, trans_color);
        SelectObject(mem_dc, old_bm);
        DeleteDC(mem_dc);

        DeleteObject(hbm);
        return;
    } else if (bpc == 8) {
        /* Regular 8-bit inline image */
        for (int py = 0; py < img_height; py++) {
            for (int px = 0; px < img_width; px++) {
                size_t src_idx = ((size_t)py * img_width + px) * components;
                size_t dst_idx = ((size_t)py * img_width + px) * 4;
                uint8_t r, g, b;

                if (components == 1) {
                    r = g = b = img_data[src_idx];
                } else if (components == 3) {
                    r = img_data[src_idx];
                    g = img_data[src_idx + 1];
                    b = img_data[src_idx + 2];
                } else {
                    r = g = b = 128;
                }

                dib_bits[dst_idx + 0] = b;
                dib_bits[dst_idx + 1] = g;
                dib_bits[dst_idx + 2] = r;
                dib_bits[dst_idx + 3] = 255;
            }
        }
    } else {
        /* Unsupported BPC; just clean up */
        DeleteObject(hbm);
        return;
    }

    /* Blit to the DC using the current CTM */
    blit_image_to_dc(ctx, hbm, img_width, img_height);
    DeleteObject(hbm);
}

/*
 * Main content stream interpreter.
 */
static void interpret_stream(PdfRenderCtx *ctx, PdfDict *resources,
                              const uint8_t *data, size_t length)
{
    CSParser parser;
    parser.data = data;
    parser.length = length;
    parser.pos = 0;

    OperandStack ops;
    ops.count = 0;

    PathBuilder path;
    path_reset(&path);

    /* State for TJ array parsing */
    bool in_tj_array = false;

    /* Temporary storage for TJ array items */
    typedef struct {
        bool   is_string;
        uint8_t *str_data;
        size_t   str_len;
        double   number;
    } TJItem;
    TJItem tj_items[1024];
    int tj_count = 0;

    CSToken tok;
    while (cs_next_token(&parser, &tok)) {

        /* Handle TJ array collection mode */
        if (in_tj_array) {
            if (tok.type == TOK_ARRAY_END) {
                in_tj_array = false;
                /* The next token should be "TJ" operator */
                /* Push a marker so the TJ handler knows we have collected items */
                opstack_push(&ops, &tok);
                continue;
            }
            /* Collect array items for TJ */
            if (tok.type == TOK_STRING && tj_count < 1024) {
                tj_items[tj_count].is_string = true;
                tj_items[tj_count].str_data = tok.str_data;
                tj_items[tj_count].str_len = tok.str_len;
                tj_items[tj_count].number = 0;
                tok.str_data = NULL; /* transfer ownership */
                tj_count++;
            } else if (tok.type == TOK_NUMBER && tj_count < 1024) {
                tj_items[tj_count].is_string = false;
                tj_items[tj_count].str_data = NULL;
                tj_items[tj_count].str_len = 0;
                tj_items[tj_count].number = tok.number;
                tj_count++;
            }
            if (tok.str_data) free(tok.str_data);
            continue;
        }

        /* Non-operator tokens go on the operand stack */
        if (tok.type == TOK_NUMBER || tok.type == TOK_NAME ||
            tok.type == TOK_STRING || tok.type == TOK_BOOL) {
            opstack_push(&ops, &tok);
            continue;
        }

        /* Array begin: might be a TJ array */
        if (tok.type == TOK_ARRAY_BEGIN) {
            in_tj_array = true;
            tj_count = 0;
            continue;
        }

        if (tok.type == TOK_ARRAY_END) {
            /* Stray ']' outside array context; ignore */
            continue;
        }

        /* ─── Operator Dispatch ─── */
        if (tok.type != TOK_OPERATOR) {
            opstack_clear(&ops);
            continue;
        }

        const char *op = tok.text;

        /* ── Graphics State ── */

        if (strcmp(op, "q") == 0) {
            gs_save(ctx);
        }
        else if (strcmp(op, "Q") == 0) {
            gs_restore(ctx);
        }
        else if (strcmp(op, "cm") == 0 && ops.count >= 6) {
            PdfMatrix m;
            m.a = opstack_number(&ops, ops.count - 6);
            m.b = opstack_number(&ops, ops.count - 5);
            m.c = opstack_number(&ops, ops.count - 4);
            m.d = opstack_number(&ops, ops.count - 3);
            m.e = opstack_number(&ops, ops.count - 2);
            m.f = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->ctm = pdf_matrix_multiply(m, current_gs(ctx)->ctm);
        }
        else if (strcmp(op, "w") == 0 && ops.count >= 1) {
            current_gs(ctx)->line_width = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "J") == 0 && ops.count >= 1) {
            current_gs(ctx)->line_cap = (int)opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "j") == 0 && ops.count >= 1) {
            current_gs(ctx)->line_join = (int)opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "M") == 0 && ops.count >= 1) {
            current_gs(ctx)->miter_limit = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "d") == 0 && ops.count >= 1) {
            /* Dash pattern: [array] phase d
             * The array was collected via the TJ array mechanism into tj_items.
             * The phase is the last operand on the stack. */
            PdfGraphicsState *dgs = current_gs(ctx);
            dgs->dash_phase = opstack_number(&ops, ops.count - 1);
            dgs->dash_count = 0;
            for (int di = 0; di < tj_count && di < 10; di++) {
                if (!tj_items[di].is_string) {
                    dgs->dash_array[dgs->dash_count++] = tj_items[di].number;
                }
            }
            /* Free any string data and reset tj state */
            for (int di = 0; di < tj_count; di++) {
                if (tj_items[di].str_data) {
                    free(tj_items[di].str_data);
                    tj_items[di].str_data = NULL;
                }
            }
            tj_count = 0;
        }
        else if (strcmp(op, "gs") == 0 && ops.count >= 1) {
            apply_extgstate(ctx, resources, opstack_name(&ops, ops.count - 1));
        }
        else if (strcmp(op, "i") == 0 && ops.count >= 1) {
            /* Flatness tolerance: ignore */
        }
        else if (strcmp(op, "ri") == 0 && ops.count >= 1) {
            /* Rendering intent: ignore */
        }

        /* ── Path Construction ── */

        else if (strcmp(op, "m") == 0 && ops.count >= 2) {
            double x = opstack_number(&ops, ops.count - 2);
            double y = opstack_number(&ops, ops.count - 1);
            path_moveto(&path, ctx, x, y);
        }
        else if (strcmp(op, "l") == 0 && ops.count >= 2) {
            double x = opstack_number(&ops, ops.count - 2);
            double y = opstack_number(&ops, ops.count - 1);
            path_lineto(&path, ctx, x, y);
        }
        else if (strcmp(op, "c") == 0 && ops.count >= 6) {
            double x1 = opstack_number(&ops, ops.count - 6);
            double y1 = opstack_number(&ops, ops.count - 5);
            double x2 = opstack_number(&ops, ops.count - 4);
            double y2 = opstack_number(&ops, ops.count - 3);
            double x3 = opstack_number(&ops, ops.count - 2);
            double y3 = opstack_number(&ops, ops.count - 1);
            path_curveto(&path, ctx, x1, y1, x2, y2, x3, y3);
        }
        else if (strcmp(op, "v") == 0 && ops.count >= 4) {
            /* curveto with initial point = current point */
            double x2 = opstack_number(&ops, ops.count - 4);
            double y2 = opstack_number(&ops, ops.count - 3);
            double x3 = opstack_number(&ops, ops.count - 2);
            double y3 = opstack_number(&ops, ops.count - 1);
            path_curveto(&path, ctx, path.cur_x, path.cur_y, x2, y2, x3, y3);
        }
        else if (strcmp(op, "y") == 0 && ops.count >= 4) {
            /* curveto with final point = last control point */
            double x1 = opstack_number(&ops, ops.count - 4);
            double y1 = opstack_number(&ops, ops.count - 3);
            double x3 = opstack_number(&ops, ops.count - 2);
            double y3 = opstack_number(&ops, ops.count - 1);
            path_curveto(&path, ctx, x1, y1, x3, y3, x3, y3);
        }
        else if (strcmp(op, "h") == 0) {
            path_closepath(&path, ctx);
        }
        else if (strcmp(op, "re") == 0 && ops.count >= 4) {
            double x = opstack_number(&ops, ops.count - 4);
            double y = opstack_number(&ops, ops.count - 3);
            double w = opstack_number(&ops, ops.count - 2);
            double h = opstack_number(&ops, ops.count - 1);
            path_rect(&path, ctx, x, y, w, h);
        }

        /* ── Path Painting ── */

        else if (strcmp(op, "S") == 0) {
            path_paint(&path, ctx, 1); /* stroke */
        }
        else if (strcmp(op, "s") == 0) {
            path_closepath(&path, ctx);
            path_paint(&path, ctx, 1); /* close + stroke */
        }
        else if (strcmp(op, "f") == 0 || strcmp(op, "F") == 0) {
            path_paint(&path, ctx, 2); /* fill (nonzero) */
        }
        else if (strcmp(op, "f*") == 0) {
            path_paint(&path, ctx, 2 | 4); /* fill (even-odd) */
        }
        else if (strcmp(op, "B") == 0) {
            path_paint(&path, ctx, 1 | 2); /* fill + stroke */
        }
        else if (strcmp(op, "B*") == 0) {
            path_paint(&path, ctx, 1 | 2 | 4); /* fill (even-odd) + stroke */
        }
        else if (strcmp(op, "b") == 0) {
            path_closepath(&path, ctx);
            path_paint(&path, ctx, 1 | 2); /* close + fill + stroke */
        }
        else if (strcmp(op, "b*") == 0) {
            path_closepath(&path, ctx);
            path_paint(&path, ctx, 1 | 2 | 4); /* close + fill (even-odd) + stroke */
        }
        else if (strcmp(op, "n") == 0) {
            path_reset(&path); /* end path without painting */
        }

        /* ── Clipping ── */

        else if (strcmp(op, "W") == 0 || strcmp(op, "W*") == 0) {
            /*
             * Clipping: set GDI clip region AND build a software clip mask.
             * The software clip mask is used by the rasterizer (path_aa_fill,
             * path_aa_stroke) which bypasses GDI clipping entirely.
             */
            if (path.count > 0) {
                /* 1. GDI clip path (for GDI fallback rendering) */
                BeginPath(ctx->hdc);
                for (int i = 0; i < path.count; i++) {
                    BYTE t = path.types[i] & ~PT_CLOSEFIGURE;
                    bool close = (path.types[i] & PT_CLOSEFIGURE) != 0;
                    if (t == PT_MOVETO) {
                        MoveToEx(ctx->hdc, path.pts[i].x, path.pts[i].y, NULL);
                    } else if (t == PT_LINETO) {
                        LineTo(ctx->hdc, path.pts[i].x, path.pts[i].y);
                    } else if (t == PT_BEZIERTO && i + 2 < path.count) {
                        POINT bpts[3] = { path.pts[i], path.pts[i+1], path.pts[i+2] };
                        PolyBezierTo(ctx->hdc, bpts, 3);
                        i += 2;
                    }
                    if (close) CloseFigure(ctx->hdc);
                }
                EndPath(ctx->hdc);
                if (strcmp(op, "W*") == 0)
                    SetPolyFillMode(ctx->hdc, ALTERNATE);
                else
                    SetPolyFillMode(ctx->hdc, WINDING);
                SelectClipPath(ctx->hdc, RGN_AND);

                /* 2. Software clip mask for the rasterizer.
                 * Rasterize the clip path into a page-sized coverage buffer
                 * using the existing double-precision device coordinates. */
                int cw = ctx->clip_mask_w;
                int ch = ctx->clip_mask_h;
                if (cw > 0 && ch > 0) {
                    RasterCtx *clip_rctx = raster_create(cw, ch);
                    if (clip_rctx) {
                        /* Replay the path using double-precision coordinates */
                        for (int i = 0; i < path.count; i++) {
                            BYTE t = path.types[i] & ~PT_CLOSEFIGURE;
                            bool close = (path.types[i] & PT_CLOSEFIGURE) != 0;

                            double px = path.dpts[i].x;
                            double py = path.dpts[i].y;

                            if (t == PT_MOVETO) {
                                raster_move_to(clip_rctx, px, py);
                            } else if (t == PT_LINETO) {
                                raster_line_to(clip_rctx, px, py);
                            } else if (t == PT_BEZIERTO && i + 2 < path.count) {
                                double cx1 = path.dpts[i].x;
                                double cy1 = path.dpts[i].y;
                                double cx2 = path.dpts[i+1].x;
                                double cy2 = path.dpts[i+1].y;
                                double ex  = path.dpts[i+2].x;
                                double ey  = path.dpts[i+2].y;
                                raster_curve_to(clip_rctx, cx1, cy1, cx2, cy2, ex, ey);
                                i += 2;
                            }
                            if (close) {
                                raster_close(clip_rctx);
                            }
                        }

                        /* Compute coverage with the appropriate fill rule */
                        if (strcmp(op, "W*") == 0) {
                            raster_finish_evenodd(clip_rctx);
                        } else {
                            raster_finish(clip_rctx);
                        }

                        /* Get the new clip coverage */
                        const uint8_t *new_cov = raster_get_coverage(clip_rctx);
                        size_t mask_sz = (size_t)cw * ch;

                        /* AND with existing clip mask (for nested clips) */
                        uint8_t *existing = ctx->clip_mask_stack[ctx->gstate_depth];
                        if (existing) {
                            /* Intersect: AND the new clip with the existing one */
                            for (size_t p = 0; p < mask_sz; p++) {
                                int combined = ((int)existing[p] * (int)new_cov[p] + 127) / 255;
                                existing[p] = (uint8_t)combined;
                            }
                        } else {
                            /* First clip at this level: allocate and copy */
                            uint8_t *mask = (uint8_t *)malloc(mask_sz);
                            if (mask) {
                                memcpy(mask, new_cov, mask_sz);
                                ctx->clip_mask_stack[ctx->gstate_depth] = mask;
                            }
                        }

                        raster_free(clip_rctx);
                    }
                }
            }
            /* Note: the path is NOT consumed by W/W*; the next painting operator
             * (or 'n') will consume it. */
        }

        /* ── Color Operators ── */

        else if (strcmp(op, "g") == 0 && ops.count >= 1) {
            double gray = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->fill_color = (PdfColor){gray, gray, gray};
            current_gs(ctx)->fill_cs.type = PDF_CS_DEVICE_GRAY;
            current_gs(ctx)->fill_cs.components = 1;
        }
        else if (strcmp(op, "G") == 0 && ops.count >= 1) {
            double gray = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->stroke_color = (PdfColor){gray, gray, gray};
            current_gs(ctx)->stroke_cs.type = PDF_CS_DEVICE_GRAY;
            current_gs(ctx)->stroke_cs.components = 1;
        }
        else if (strcmp(op, "rg") == 0 && ops.count >= 3) {
            current_gs(ctx)->fill_color.r = opstack_number(&ops, ops.count - 3);
            current_gs(ctx)->fill_color.g = opstack_number(&ops, ops.count - 2);
            current_gs(ctx)->fill_color.b = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->fill_cs.type = PDF_CS_DEVICE_RGB;
            current_gs(ctx)->fill_cs.components = 3;
        }
        else if (strcmp(op, "RG") == 0 && ops.count >= 3) {
            current_gs(ctx)->stroke_color.r = opstack_number(&ops, ops.count - 3);
            current_gs(ctx)->stroke_color.g = opstack_number(&ops, ops.count - 2);
            current_gs(ctx)->stroke_color.b = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->stroke_cs.type = PDF_CS_DEVICE_RGB;
            current_gs(ctx)->stroke_cs.components = 3;
        }
        else if (strcmp(op, "k") == 0 && ops.count >= 4) {
            double c_ = opstack_number(&ops, ops.count - 4);
            double m_ = opstack_number(&ops, ops.count - 3);
            double y_ = opstack_number(&ops, ops.count - 2);
            double k_ = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->fill_color = cmyk_to_rgb(c_, m_, y_, k_);
            current_gs(ctx)->fill_cs.type = PDF_CS_DEVICE_CMYK;
            current_gs(ctx)->fill_cs.components = 4;
        }
        else if (strcmp(op, "K") == 0 && ops.count >= 4) {
            double c_ = opstack_number(&ops, ops.count - 4);
            double m_ = opstack_number(&ops, ops.count - 3);
            double y_ = opstack_number(&ops, ops.count - 2);
            double k_ = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->stroke_color = cmyk_to_rgb(c_, m_, y_, k_);
            current_gs(ctx)->stroke_cs.type = PDF_CS_DEVICE_CMYK;
            current_gs(ctx)->stroke_cs.components = 4;
        }
        else if (strcmp(op, "cs") == 0 && ops.count >= 1) {
            /* Set fill color space: resolve from resources */
            const char *cs_name = opstack_name(&ops, ops.count - 1);
            resolve_colorspace(ctx->doc, resources, cs_name, &current_gs(ctx)->fill_cs);
        }
        else if (strcmp(op, "CS") == 0 && ops.count >= 1) {
            /* Set stroke color space: resolve from resources */
            const char *cs_name = opstack_name(&ops, ops.count - 1);
            resolve_colorspace(ctx->doc, resources, cs_name, &current_gs(ctx)->stroke_cs);
        }
        else if (strcmp(op, "sc") == 0 || strcmp(op, "scn") == 0) {
            /* Set fill color using active fill color space */
            if (current_gs(ctx)->fill_cs.type != PDF_CS_UNKNOWN) {
                current_gs(ctx)->fill_color = resolve_color_from_cs(
                    &current_gs(ctx)->fill_cs, &ops);
            } else {
                /* Fallback: operand-count guessing (no cs was called) */
                if (ops.count >= 4) {
                    double c_ = opstack_number(&ops, ops.count - 4);
                    double m_ = opstack_number(&ops, ops.count - 3);
                    double y_ = opstack_number(&ops, ops.count - 2);
                    double k_ = opstack_number(&ops, ops.count - 1);
                    current_gs(ctx)->fill_color = cmyk_to_rgb(c_, m_, y_, k_);
                } else if (ops.count >= 3) {
                    current_gs(ctx)->fill_color.r = opstack_number(&ops, ops.count - 3);
                    current_gs(ctx)->fill_color.g = opstack_number(&ops, ops.count - 2);
                    current_gs(ctx)->fill_color.b = opstack_number(&ops, ops.count - 1);
                } else if (ops.count >= 1) {
                    double gray = opstack_number(&ops, ops.count - 1);
                    current_gs(ctx)->fill_color = (PdfColor){gray, gray, gray};
                }
            }
        }
        else if (strcmp(op, "SC") == 0 || strcmp(op, "SCN") == 0) {
            /* Set stroke color using active stroke color space */
            if (current_gs(ctx)->stroke_cs.type != PDF_CS_UNKNOWN) {
                current_gs(ctx)->stroke_color = resolve_color_from_cs(
                    &current_gs(ctx)->stroke_cs, &ops);
            } else {
                /* Fallback: operand-count guessing (no CS was called) */
                if (ops.count >= 4) {
                    double c_ = opstack_number(&ops, ops.count - 4);
                    double m_ = opstack_number(&ops, ops.count - 3);
                    double y_ = opstack_number(&ops, ops.count - 2);
                    double k_ = opstack_number(&ops, ops.count - 1);
                    current_gs(ctx)->stroke_color = cmyk_to_rgb(c_, m_, y_, k_);
                } else if (ops.count >= 3) {
                    current_gs(ctx)->stroke_color.r = opstack_number(&ops, ops.count - 3);
                    current_gs(ctx)->stroke_color.g = opstack_number(&ops, ops.count - 2);
                    current_gs(ctx)->stroke_color.b = opstack_number(&ops, ops.count - 1);
                } else if (ops.count >= 1) {
                    double gray = opstack_number(&ops, ops.count - 1);
                    current_gs(ctx)->stroke_color = (PdfColor){gray, gray, gray};
                }
            }
        }

        /* ── Text State Operators ── */

        else if (strcmp(op, "BT") == 0) {
            /* Begin text object: reset text matrix and text line matrix to identity */
            ctx->text_matrix = PDF_IDENTITY_MATRIX;
            ctx->text_line_matrix = PDF_IDENTITY_MATRIX;
        }
        else if (strcmp(op, "ET") == 0) {
            /* End text object */
        }
        else if (strcmp(op, "Tc") == 0 && ops.count >= 1) {
            current_gs(ctx)->char_spacing = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "Tw") == 0 && ops.count >= 1) {
            current_gs(ctx)->word_spacing = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "Tz") == 0 && ops.count >= 1) {
            current_gs(ctx)->horiz_scaling = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "TL") == 0 && ops.count >= 1) {
            current_gs(ctx)->leading = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "Tf") == 0 && ops.count >= 2) {
            /* Set font: /FontName size */
            const char *fname = opstack_name(&ops, ops.count - 2);
            double fsize = opstack_number(&ops, ops.count - 1);
            strncpy(current_gs(ctx)->font_name, fname, PDF_MAX_NAME_LEN - 1);
            current_gs(ctx)->font_name[PDF_MAX_NAME_LEN - 1] = '\0';
            current_gs(ctx)->font_size = fsize;
        }
        else if (strcmp(op, "Tr") == 0 && ops.count >= 1) {
            current_gs(ctx)->text_render_mode = (int)opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "Ts") == 0 && ops.count >= 1) {
            current_gs(ctx)->text_rise = opstack_number(&ops, ops.count - 1);
        }

        /* ── Text Positioning Operators ── */

        else if (strcmp(op, "Td") == 0 && ops.count >= 2) {
            double tx = opstack_number(&ops, ops.count - 2);
            double ty = opstack_number(&ops, ops.count - 1);
            PdfMatrix t = PDF_IDENTITY_MATRIX;
            t.e = tx;
            t.f = ty;
            ctx->text_line_matrix = pdf_matrix_multiply(t, ctx->text_line_matrix);
            ctx->text_matrix = ctx->text_line_matrix;
        }
        else if (strcmp(op, "TD") == 0 && ops.count >= 2) {
            double tx = opstack_number(&ops, ops.count - 2);
            double ty = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->leading = -ty;
            PdfMatrix t = PDF_IDENTITY_MATRIX;
            t.e = tx;
            t.f = ty;
            ctx->text_line_matrix = pdf_matrix_multiply(t, ctx->text_line_matrix);
            ctx->text_matrix = ctx->text_line_matrix;
        }
        else if (strcmp(op, "Tm") == 0 && ops.count >= 6) {
            PdfMatrix m;
            m.a = opstack_number(&ops, ops.count - 6);
            m.b = opstack_number(&ops, ops.count - 5);
            m.c = opstack_number(&ops, ops.count - 4);
            m.d = opstack_number(&ops, ops.count - 3);
            m.e = opstack_number(&ops, ops.count - 2);
            m.f = opstack_number(&ops, ops.count - 1);
            ctx->text_matrix = m;
            ctx->text_line_matrix = m;
        }
        else if (strcmp(op, "T*") == 0) {
            PdfMatrix t = PDF_IDENTITY_MATRIX;
            t.e = 0;
            t.f = -current_gs(ctx)->leading;
            ctx->text_line_matrix = pdf_matrix_multiply(t, ctx->text_line_matrix);
            ctx->text_matrix = ctx->text_line_matrix;
        }

        /* ── Text Showing Operators ── */

        else if (strcmp(op, "Tj") == 0 && ops.count >= 1) {
            CSToken *str_tok = &ops.items[ops.count - 1];
            if (str_tok->type == TOK_STRING && str_tok->str_data) {
                render_text_string(ctx, resources, str_tok->str_data, str_tok->str_len);
            }
        }
        else if (strcmp(op, "TJ") == 0) {
            /* Process collected TJ array items */
            PdfGraphicsState *gs = current_gs(ctx);
            double font_size = gs->font_size;
            double h_scale = gs->horiz_scaling / 100.0;

            for (int ti = 0; ti < tj_count; ti++) {
                if (tj_items[ti].is_string && tj_items[ti].str_data) {
                    render_text_string(ctx, resources,
                                       tj_items[ti].str_data, tj_items[ti].str_len);
                } else if (!tj_items[ti].is_string) {
                    /* Numeric adjustment: move text position.
                     * Positive values move LEFT (subtract from tx).
                     * Value is in thousandths of a unit of text space. */
                    double adjust = -tj_items[ti].number / 1000.0 * font_size * h_scale;
                    PdfMatrix adv = PDF_IDENTITY_MATRIX;
                    adv.e = adjust;
                    ctx->text_matrix = pdf_matrix_multiply(adv, ctx->text_matrix);
                }
            }
            /* Free TJ string data */
            for (int ti = 0; ti < tj_count; ti++) {
                if (tj_items[ti].str_data) {
                    free(tj_items[ti].str_data);
                    tj_items[ti].str_data = NULL;
                }
            }
            tj_count = 0;
        }
        else if (strcmp(op, "'") == 0 && ops.count >= 1) {
            /* T* then Tj */
            PdfMatrix t = PDF_IDENTITY_MATRIX;
            t.f = -current_gs(ctx)->leading;
            ctx->text_line_matrix = pdf_matrix_multiply(t, ctx->text_line_matrix);
            ctx->text_matrix = ctx->text_line_matrix;

            CSToken *str_tok = &ops.items[ops.count - 1];
            if (str_tok->type == TOK_STRING && str_tok->str_data) {
                render_text_string(ctx, resources, str_tok->str_data, str_tok->str_len);
            }
        }
        else if (strcmp(op, "\"") == 0 && ops.count >= 3) {
            /* aw ac string " -- set word spacing, char spacing, T*, Tj */
            current_gs(ctx)->word_spacing = opstack_number(&ops, ops.count - 3);
            current_gs(ctx)->char_spacing = opstack_number(&ops, ops.count - 2);

            PdfMatrix t = PDF_IDENTITY_MATRIX;
            t.f = -current_gs(ctx)->leading;
            ctx->text_line_matrix = pdf_matrix_multiply(t, ctx->text_line_matrix);
            ctx->text_matrix = ctx->text_line_matrix;

            CSToken *str_tok = &ops.items[ops.count - 1];
            if (str_tok->type == TOK_STRING && str_tok->str_data) {
                render_text_string(ctx, resources, str_tok->str_data, str_tok->str_len);
            }
        }

        /* ── XObject ── */

        else if (strcmp(op, "Do") == 0 && ops.count >= 1) {
            do_xobject(ctx, resources, opstack_name(&ops, ops.count - 1));
        }

        /* ── Inline Image (BI/ID/EI) ── */

        else if (strcmp(op, "BI") == 0) {
            PROF_START(inl_img);
            render_inline_image(ctx, &parser);
            PROF_END(image, inl_img);
        }

        /* ── Marked Content (consume operands, no-op) ── */

        else if (strcmp(op, "BMC") == 0 ||
                 strcmp(op, "BDC") == 0 ||
                 strcmp(op, "EMC") == 0 ||
                 strcmp(op, "MP") == 0 ||
                 strcmp(op, "DP") == 0) {
            /* Ignore -- operands are consumed by clearing the stack below */
        }

        /* ── Type3 Glyph Operators ── */

        else if (strcmp(op, "d0") == 0 && ops.count >= 2) {
            /* Type3 glyph width: wx wy d0
             * Sets the glyph width. We consume and ignore since we get
             * widths from the /Widths array in the font dictionary. */
        }
        else if (strcmp(op, "d1") == 0 && ops.count >= 6) {
            /* Type3 glyph width + bounding box: wx wy llx lly urx ury d1
             * Sets the glyph width and bounding box. We consume and ignore
             * since we get widths from /Widths and don't cache glyph bitmaps. */
        }

        /* ── Dictionary markers in content stream (inline image dict) ── */
        else if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
            /* Handled as part of inline image parsing; ignore if stray */
        }

        /* Unknown operator: ignore but don't crash */

        /* Clear operand stack after processing */
        opstack_clear(&ops);
    }

    /* Cleanup any remaining operands */
    opstack_clear(&ops);

    /* Free any remaining TJ items */
    for (int ti = 0; ti < tj_count; ti++) {
        if (tj_items[ti].str_data) free(tj_items[ti].str_data);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page Rendering Entry Point
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Get page dimensions from the MediaBox (falling back to CropBox or defaults).
 * Returns false if the page has no valid box.
 */
static bool get_page_box(PdfDocument *doc, PdfObj *page,
                          double *x0, double *y0, double *x1, double *y1)
{
    PdfDict *dict;
    if (page->type == PDF_OBJ_DICT)
        dict = page->dict;
    else
        return false;

    /* Try MediaBox first, then CropBox */
    const char *box_names[] = { "MediaBox", "CropBox", NULL };
    for (int b = 0; box_names[b]; b++) {
        PdfObj *box = pdf_dict_get(dict, box_names[b]);
        if (!box) continue;
        box = pdf_resolve(doc, box);
        if (!box || box->type != PDF_OBJ_ARRAY) continue;
        PdfArray *arr = box->array;
        if (pdf_array_len(arr) < 4) continue;

        PdfObj *vals[4];
        double coords[4];
        bool valid = true;
        for (int i = 0; i < 4; i++) {
            vals[i] = pdf_resolve(doc, pdf_array_get(arr, i));
            if (!vals[i]) { valid = false; break; }
            if (vals[i]->type == PDF_OBJ_INT)
                coords[i] = (double)vals[i]->integer;
            else if (vals[i]->type == PDF_OBJ_REAL)
                coords[i] = vals[i]->real;
            else
                { valid = false; break; }
        }
        if (!valid) continue;

        *x0 = coords[0]; *y0 = coords[1];
        *x1 = coords[2]; *y1 = coords[3];
        return true;
    }

    return false;
}

/*
 * Get the page's /Resources dictionary, checking parent nodes if needed.
 */
static PdfDict *get_page_resources(PdfDocument *doc, PdfObj *page)
{
    if (!page || page->type != PDF_OBJ_DICT) return NULL;

    PdfObj *res = pdf_dict_get(page->dict, "Resources");
    if (res) {
        res = pdf_resolve(doc, res);
        if (res && res->type == PDF_OBJ_DICT) return res->dict;
    }

    /* Check parent for inherited resources */
    PdfObj *parent = pdf_dict_get(page->dict, "Parent");
    if (parent) {
        parent = pdf_resolve(doc, parent);
        if (parent && parent->type == PDF_OBJ_DICT) {
            return get_page_resources(doc, parent);
        }
    }

    return NULL;
}

/*
 * Get content stream data for a page.
 * The /Contents entry can be a single stream or an array of streams.
 * If it's an array, concatenate all streams into one buffer.
 */
static bool get_page_content(PdfDocument *doc, PdfObj *page,
                              uint8_t **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len = 0;

    if (!page || page->type != PDF_OBJ_DICT) return false;

    PdfObj *contents = pdf_dict_get(page->dict, "Contents");
    if (!contents) return true;  /* No content = blank page, not an error */
    contents = pdf_resolve(doc, contents);
    if (!contents) return false;

    if (contents->type == PDF_OBJ_STREAM) {
        /* Single stream */
        if (!contents->stream->decoded_data) {
            if (!pdf_decode_stream(doc, contents->stream))
                return false;
        }
        if (!contents->stream->decoded_data) return false;

        /* Make a copy so the caller can free it */
        *out_len = contents->stream->decoded_length;
        *out_data = (uint8_t *)malloc(*out_len);
        if (!*out_data) return false;
        memcpy(*out_data, contents->stream->decoded_data, *out_len);
        return true;
    }

    if (contents->type == PDF_OBJ_ARRAY) {
        /* Array of streams: concatenate with spaces between */
        PdfArray *arr = contents->array;
        int n = pdf_array_len(arr);

        /* First pass: decode all and compute total size */
        size_t total = 0;
        for (int i = 0; i < n; i++) {
            PdfObj *item = pdf_resolve(doc, pdf_array_get(arr, i));
            if (!item || item->type != PDF_OBJ_STREAM) continue;
            if (!item->stream->decoded_data) {
                if (!pdf_decode_stream(doc, item->stream))
                    continue;
            }
            if (item->stream->decoded_data)
                total += item->stream->decoded_length + 1; /* +1 for separator */
        }

        if (total == 0) return true;

        *out_data = (uint8_t *)malloc(total);
        if (!*out_data) return false;

        /* Second pass: copy data */
        size_t offset = 0;
        for (int i = 0; i < n; i++) {
            PdfObj *item = pdf_resolve(doc, pdf_array_get(arr, i));
            if (!item || item->type != PDF_OBJ_STREAM) continue;
            if (!item->stream->decoded_data) continue;
            if (offset > 0) {
                (*out_data)[offset++] = ' '; /* separator */
            }
            memcpy(*out_data + offset, item->stream->decoded_data,
                   item->stream->decoded_length);
            offset += item->stream->decoded_length;
        }
        *out_len = offset;
        return true;
    }

    return false;
}

HBITMAP pdf_render_page(PdfDocument *doc, int page_idx, double scale,
                        int *out_width, int *out_height)
{
    if (!doc || page_idx < 0 || page_idx >= doc->page_count)
        return NULL;
    if (scale <= 0.0) scale = 1.0;

    /* Initialize profiling for this page render */
    prof_reset(page_idx, scale);

    /* Get the page dictionary */
    PdfObj *page = pdf_get_page(doc, page_idx);
    if (!page) return NULL;

    /* Get page dimensions from MediaBox */
    double box_x0 = 0, box_y0 = 0, box_x1 = 612, box_y1 = 792; /* Letter default */
    get_page_box(doc, page, &box_x0, &box_y0, &box_x1, &box_y1);

    double page_w = fabs(box_x1 - box_x0);
    double page_h = fabs(box_y1 - box_y0);
    if (page_w < 1.0) page_w = 612.0;
    if (page_h < 1.0) page_h = 792.0;

    /* Compute pixel dimensions */
    int px_w = (int)(page_w * scale + 0.5);
    int px_h = (int)(page_h * scale + 0.5);
    if (px_w < 1) px_w = 1;
    if (px_h < 1) px_h = 1;

    if (out_width)  *out_width  = px_w;
    if (out_height) *out_height = px_h;

    /* Create a memory DC and DIB bitmap */
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = px_w;
    bmi.bmiHeader.biHeight      = -px_h; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm) {
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return NULL;
    }

    HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);

    /* Fill background with white */
    RECT rc = { 0, 0, px_w, px_h };
    HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(mem_dc, &rc, white_brush);
    DeleteObject(white_brush);

    /* Set up GDI defaults */
    SetBkMode(mem_dc, TRANSPARENT);
    SetGraphicsMode(mem_dc, GM_ADVANCED);
    SetStretchBltMode(mem_dc, HALFTONE);

    /* Initialize the render context */
    PdfRenderCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hdc = mem_dc;
    ctx.page_width_px = px_w;
    ctx.page_height_px = px_h;
    ctx.scale = scale;
    ctx.page_width = page_w;
    ctx.page_height = page_h;
    ctx.doc = doc;
    ctx.gstate_depth = 0;

    /* Initialize software clip mask dimensions (all masks NULL = no clipping) */
    ctx.clip_mask_w = px_w;
    ctx.clip_mask_h = px_h;

    /* Initialize graphics state */
    gs_init(&ctx.gstate[0]);

    /*
     * Set up the initial CTM:
     * PDF coordinate system: origin at bottom-left corner of MediaBox, Y up.
     * GDI coordinate system: origin at top-left, Y down.
     *
     * Transform: scale, then flip Y, then translate so MediaBox origin maps correctly.
     *
     * The matrix is:
     *   [scale    0      ]
     *   [0       -scale   ]
     *   [-box_x0*scale  (box_y1)*scale]
     *
     * This maps:
     *   (box_x0, box_y0) -> (0, px_h)  [bottom-left -> bottom of bitmap]
     *   (box_x1, box_y1) -> (px_w, 0)  [top-right -> top of bitmap]
     */
    PdfMatrix initial_ctm;
    initial_ctm.a = scale;
    initial_ctm.b = 0.0;
    initial_ctm.c = 0.0;
    initial_ctm.d = -scale;
    initial_ctm.e = -box_x0 * scale;
    initial_ctm.f = box_y1 * scale;

    ctx.gstate[0].ctm = initial_ctm;
    ctx.text_matrix = PDF_IDENTITY_MATRIX;
    ctx.text_line_matrix = PDF_IDENTITY_MATRIX;

    /* Get page resources */
    PdfDict *resources = get_page_resources(doc, page);

    /* Get and interpret the content stream */
    uint8_t *content_data = NULL;
    size_t content_len = 0;

    if (get_page_content(doc, page, &content_data, &content_len)) {
        if (content_data && content_len > 0 && resources) {
            PROF_START(interp);
            interpret_stream(&ctx, resources, content_data, content_len);
            LARGE_INTEGER interp_end;
            QueryPerformanceCounter(&interp_end);
            g_prof.interpret_time += interp_end.QuadPart - interp_prof_start.QuadPart;
        }
        free(content_data);
    }

    /* Free any remaining clip masks on the stack */
    for (int i = 0; i <= ctx.gstate_depth; i++) {
        if (ctx.clip_mask_stack[i]) {
            free(ctx.clip_mask_stack[i]);
            ctx.clip_mask_stack[i] = NULL;
        }
    }

    /* Emit profiling results */
    prof_emit();

    /* Cleanup */
    SelectObject(mem_dc, old_bm);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    return hbm;
}

HBITMAP pdf_render_page_region(PdfDocument *doc, int page_idx, double scale,
                                double region_x, double region_y,
                                double region_w, double region_h,
                                int *out_width, int *out_height)
{
    if (!doc || page_idx < 0 || page_idx >= doc->page_count)
        return NULL;
    if (scale <= 0.0) scale = 1.0;

    /* Initialize profiling for this page render */
    prof_reset(page_idx, scale);
    g_prof.is_region = true;

    /* Get the page dictionary */
    PdfObj *page = pdf_get_page(doc, page_idx);
    if (!page) return NULL;

    /* Get page dimensions from MediaBox */
    double box_x0 = 0, box_y0 = 0, box_x1 = 612, box_y1 = 792; /* Letter default */
    get_page_box(doc, page, &box_x0, &box_y0, &box_x1, &box_y1);

    double page_w = fabs(box_x1 - box_x0);
    double page_h = fabs(box_y1 - box_y0);
    if (page_w < 1.0) page_w = 612.0;
    if (page_h < 1.0) page_h = 792.0;

    /* Compute pixel dimensions for the REGION (not the full page) */
    int px_w = (int)(region_w * scale + 0.5);
    int px_h = (int)(region_h * scale + 0.5);
    if (px_w < 1) px_w = 1;
    if (px_h < 1) px_h = 1;

    if (out_width)  *out_width  = px_w;
    if (out_height) *out_height = px_h;

    /* Record region dimensions for profiling */
    g_prof.region_px_w = px_w;
    g_prof.region_px_h = px_h;

    /* Create a memory DC and DIB bitmap */
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = px_w;
    bmi.bmiHeader.biHeight      = -px_h; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm) {
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return NULL;
    }

    HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);

    /* Fill background with white */
    RECT rc = { 0, 0, px_w, px_h };
    HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(mem_dc, &rc, white_brush);
    DeleteObject(white_brush);

    /* Set up GDI defaults */
    SetBkMode(mem_dc, TRANSPARENT);
    SetGraphicsMode(mem_dc, GM_ADVANCED);
    SetStretchBltMode(mem_dc, HALFTONE);

    /* Initialize the render context */
    PdfRenderCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hdc = mem_dc;
    ctx.page_width_px = px_w;
    ctx.page_height_px = px_h;
    ctx.scale = scale;
    ctx.page_width = page_w;
    ctx.page_height = page_h;
    ctx.doc = doc;
    ctx.gstate_depth = 0;

    /* Initialize software clip mask dimensions */
    ctx.clip_mask_w = px_w;
    ctx.clip_mask_h = px_h;

    /* Initialize graphics state */
    gs_init(&ctx.gstate[0]);

    /*
     * Set up the initial CTM for region rendering:
     *
     * Like the full-page CTM, we scale and flip Y, but we also translate
     * so that region_x/region_y maps to the bitmap origin.
     *
     * The matrix is:
     *   [scale    0        -region_x * scale              ]
     *   [0       -scale     (box_y1 - region_y) * scale   ]
     *
     * This maps:
     *   (region_x, region_y + region_h) -> (0, 0)    [top-left of region -> top of bitmap]
     *   (region_x + region_w, region_y) -> (px_w, px_h)  [bottom-right of region -> bottom of bitmap]
     */
    PdfMatrix initial_ctm;
    initial_ctm.a = scale;
    initial_ctm.b = 0.0;
    initial_ctm.c = 0.0;
    initial_ctm.d = -scale;
    initial_ctm.e = -(box_x0 + region_x) * scale;
    initial_ctm.f = (box_y1 - region_y) * scale;

    ctx.gstate[0].ctm = initial_ctm;
    ctx.text_matrix = PDF_IDENTITY_MATRIX;
    ctx.text_line_matrix = PDF_IDENTITY_MATRIX;

    /* Get page resources */
    PdfDict *resources = get_page_resources(doc, page);

    /* Get and interpret the content stream */
    uint8_t *content_data = NULL;
    size_t content_len = 0;

    if (get_page_content(doc, page, &content_data, &content_len)) {
        if (content_data && content_len > 0 && resources) {
            PROF_START(interp);
            interpret_stream(&ctx, resources, content_data, content_len);
            LARGE_INTEGER interp_end;
            QueryPerformanceCounter(&interp_end);
            g_prof.interpret_time += interp_end.QuadPart - interp_prof_start.QuadPart;
        }
        free(content_data);
    }

    /* Free any remaining clip masks on the stack */
    for (int i = 0; i <= ctx.gstate_depth; i++) {
        if (ctx.clip_mask_stack[i]) {
            free(ctx.clip_mask_stack[i]);
            ctx.clip_mask_stack[i] = NULL;
        }
    }

    /* Emit profiling results */
    prof_emit();

    /* Cleanup */
    SelectObject(mem_dc, old_bm);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    return hbm;
}
