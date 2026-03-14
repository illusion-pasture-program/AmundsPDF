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
#include <objbase.h>  /* CoInitializeEx, CoCreateInstance for WIC image decoding */

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
    gs->font_size    = 12.0;
    gs->font_name[0] = '\0';
    gs->char_spacing   = 0.0;
    gs->word_spacing   = 0.0;
    gs->horiz_scaling  = 100.0;
    gs->leading        = 0.0;
    gs->text_render_mode = 0;
    gs->text_rise      = 0.0;
}

static void gs_save(PdfRenderCtx *ctx)
{
    if (ctx->gstate_depth + 1 < PDF_MAX_GSTATE_STACK) {
        ctx->gstate[ctx->gstate_depth + 1] = ctx->gstate[ctx->gstate_depth];
        ctx->gstate_depth++;
        SaveDC(ctx->hdc);
    }
}

static void gs_restore(PdfRenderCtx *ctx)
{
    if (ctx->gstate_depth > 0) {
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
    rgb.r = 1.0 - (c + k);
    rgb.g = 1.0 - (m + k);
    rgb.b = 1.0 - (y + k);
    if (rgb.r < 0.0) rgb.r = 0.0;
    if (rgb.r > 1.0) rgb.r = 1.0;
    if (rgb.g < 0.0) rgb.g = 0.0;
    if (rgb.g > 1.0) rgb.g = 1.0;
    if (rgb.b < 0.0) rgb.b = 0.0;
    if (rgb.b > 1.0) rgb.b = 1.0;
    return rgb;
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
    POINT  pts[MAX_PATH_POINTS];
    BYTE   types[MAX_PATH_POINTS];  /* PT_MOVETO, PT_LINETO, PT_BEZIERTO */
    int    count;
    double cur_x, cur_y;    /* current point in user space */
    double start_x, start_y; /* start of current subpath */
    bool   has_current;
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
    transform_point(ctx, x, y, &dx, &dy);
    pb->pts[pb->count].x = dx;
    pb->pts[pb->count].y = dy;
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
 * Render the accumulated path using GDI.
 * Operations: 1=stroke, 2=fill, 4=even-odd fill rule
 */
static void path_paint(PathBuilder *pb, PdfRenderCtx *ctx, int ops)
{
    if (pb->count == 0) {
        path_reset(pb);
        return;
    }

    PdfGraphicsState *gs = current_gs(ctx);
    HDC hdc = ctx->hdc;

    /* Create stroke pen */
    HPEN pen = NULL;
    HPEN old_pen = NULL;
    if (ops & 1) {
        /* Line width in device pixels */
        double lw = gs->line_width;
        PdfMatrix m = gs->ctm;
        /* Approximate device line width from CTM */
        double scale_factor = sqrt(fabs(m.a * m.d - m.b * m.c));
        int pen_width = (int)(lw * scale_factor + 0.5);
        if (pen_width < 1) pen_width = 1;

        COLORREF stroke_col = pdf_color_to_gdi(gs->stroke_color);
        int pen_style = PS_SOLID;
        /* Map line cap */
        int end_cap = PS_ENDCAP_FLAT;
        if (gs->line_cap == 1) end_cap = PS_ENDCAP_ROUND;
        else if (gs->line_cap == 2) end_cap = PS_ENDCAP_SQUARE;
        /* Map line join */
        int join = PS_JOIN_MITER;
        if (gs->line_join == 1) join = PS_JOIN_ROUND;
        else if (gs->line_join == 2) join = PS_JOIN_BEVEL;

        LOGBRUSH lb;
        lb.lbStyle = BS_SOLID;
        lb.lbColor = stroke_col;
        lb.lbHatch = 0;
        pen = ExtCreatePen(PS_GEOMETRIC | pen_style | end_cap | join,
                           pen_width, &lb, 0, NULL);
        if (pen) old_pen = (HPEN)SelectObject(hdc, pen);
    }

    /* Create fill brush */
    HBRUSH brush = NULL;
    HBRUSH old_brush = NULL;
    if (ops & 2) {
        COLORREF fill_col = pdf_color_to_gdi(gs->fill_color);
        brush = CreateSolidBrush(fill_col);
        if (brush) old_brush = (HBRUSH)SelectObject(hdc, brush);
    } else {
        /* No fill: use null brush */
        old_brush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    }

    if (!(ops & 1)) {
        /* No stroke: use null pen */
        old_pen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    }

    /* Set fill mode */
    if (ops & 4) {
        SetPolyFillMode(hdc, ALTERNATE); /* even-odd */
    } else {
        SetPolyFillMode(hdc, WINDING);   /* nonzero winding */
    }

    /* Replay path through GDI */
    BeginPath(hdc);
    for (int i = 0; i < pb->count; i++) {
        BYTE t = pb->types[i] & ~PT_CLOSEFIGURE;
        bool close = (pb->types[i] & PT_CLOSEFIGURE) != 0;

        if (t == PT_MOVETO) {
            MoveToEx(hdc, pb->pts[i].x, pb->pts[i].y, NULL);
        } else if (t == PT_LINETO) {
            LineTo(hdc, pb->pts[i].x, pb->pts[i].y);
        } else if (t == PT_BEZIERTO) {
            /* Need exactly 3 bezier control points */
            if (i + 2 < pb->count) {
                POINT bpts[3] = { pb->pts[i], pb->pts[i+1], pb->pts[i+2] };
                PolyBezierTo(hdc, bpts, 3);
                i += 2; /* skip the next two, loop will do i++ */
            }
        }

        if (close) {
            CloseFigure(hdc);
        }
    }
    EndPath(hdc);

    /* Paint */
    if ((ops & 3) == 3) {
        /* Fill and stroke */
        StrokeAndFillPath(hdc);
    } else if (ops & 2) {
        FillPath(hdc);
    } else if (ops & 1) {
        StrokePath(hdc);
    }

    /* Cleanup */
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
 * Render a text string at the current text position.
 * Updates the text matrix to advance past the rendered text.
 */
static void render_text_string(PdfRenderCtx *ctx, PdfDict *resources,
                                const uint8_t *str, size_t len)
{
    if (len == 0) return;

    PdfGraphicsState *gs = current_gs(ctx);
    HDC hdc = ctx->hdc;

    /* Resolve base font name */
    const char *base_font = resolve_base_font(ctx->doc, resources, gs->font_name);
    if (!base_font) base_font = "Helvetica";

    /* Compute effective font size in device pixels.
     * The text matrix and CTM together transform text space to device space.
     * The effective vertical scale is sqrt(b^2 + d^2) of the combined matrix,
     * which gives us the font size in device pixels. */
    double font_size = gs->font_size;
    PdfMatrix font_mtx = pdf_matrix_multiply(ctx->text_matrix, gs->ctm);
    double eff_scale = sqrt(font_mtx.b * font_mtx.b + font_mtx.d * font_mtx.d);
    int font_height_px = -(int)(font_size * eff_scale + 0.5);
    if (font_height_px == 0) font_height_px = -12;

    HFONT hfont = pdf_font_create_px(base_font, font_height_px);
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

    /* Render each character individually for accurate positioning */
    for (size_t i = 0; i < len; i++) {
        int char_code = str[i];

        /* Skip invisible text rendering mode */
        if (gs->text_render_mode == 3) {
            /* Still advance position */
        } else {
            /* Compute device position from text matrix * CTM.
             * Apply text rise: shift vertically in text space by gs->text_rise */
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
                    double rr = (1.0 - (c_ + k_));
                    double gg = (1.0 - (m_ + k_));
                    double bb = (1.0 - (y_ + k_));
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
static void blit_image_to_dc(PdfRenderCtx *ctx, HBITMAP hbm, int width, int height)
{
    /* Compute destination rectangle from CTM.
     * Images in PDF are defined in a 1x1 unit square at the origin,
     * and the CTM maps them to the target position and size. */
    PdfMatrix m = current_gs(ctx)->ctm;
    double x0, y0, x1, y1, x2, y2, x3, y3;
    pdf_transform_point(m, 0.0, 0.0, &x0, &y0);
    pdf_transform_point(m, 1.0, 0.0, &x1, &y1);
    pdf_transform_point(m, 1.0, 1.0, &x2, &y2);
    pdf_transform_point(m, 0.0, 1.0, &x3, &y3);

    /* Find bounding box */
    double min_x = x0, max_x = x0, min_y = y0, max_y = y0;
    if (x1 < min_x) min_x = x1; if (x1 > max_x) max_x = x1;
    if (x2 < min_x) min_x = x2; if (x2 > max_x) max_x = x2;
    if (x3 < min_x) min_x = x3; if (x3 > max_x) max_x = x3;
    if (y1 < min_y) min_y = y1; if (y1 > max_y) max_y = y1;
    if (y2 < min_y) min_y = y2; if (y2 > max_y) max_y = y2;
    if (y3 < min_y) min_y = y3; if (y3 > max_y) max_y = y3;

    int dest_x = (int)(min_x + 0.5);
    int dest_y = (int)(min_y + 0.5);
    int dest_w = (int)(max_x - min_x + 0.5);
    int dest_h = (int)(max_y - min_y + 0.5);
    if (dest_w < 1) dest_w = 1;
    if (dest_h < 1) dest_h = 1;

    /* StretchBlt the image to the DC */
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
 */
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
        /* For DCTDecode, we need the raw stream data (still JPEG-compressed).
         * The pdf_decode_stream would have passed it through unchanged via
         * the "unsupported filter" path. Use raw_data if available. */
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

    /* Resolve color space (handles Indexed, ICCBased, etc.) */
    bool is_indexed = false;
    uint8_t palette_rgb[256 * 3]; /* up to 256 RGB entries */
    int palette_count = 0;
    int components = resolve_image_colorspace(ctx->doc, dict,
                                              &is_indexed,
                                              palette_rgb, &palette_count);

    /* Decode stream data (FlateDecode, etc.) */
    if (!stream->decoded_data) {
        if (!pdf_decode_stream(ctx->doc, stream))
            return;
    }
    if (!stream->decoded_data) return;

    const uint8_t *src = stream->decoded_data;
    size_t src_len = stream->decoded_length;

    /* Only handle 8-bit components for now */
    if (bpc != 8) return;

    size_t expected = (size_t)width * height * components;
    if (src_len < expected) return;

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

    /* Convert source pixels to BGRA */
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            size_t src_idx = ((size_t)py * width + px) * components;
            size_t dst_idx = ((size_t)py * width + px) * 4;
            uint8_t r, g, b;

            if (is_indexed) {
                /* Index into palette */
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
                r = (uint8_t)((1.0 - (c_ + k_)) * 255.0);
                g = (uint8_t)((1.0 - (m_ + k_)) * 255.0);
                b = (uint8_t)((1.0 - (y_ + k_)) * 255.0);
            } else {
                r = g = b = 128;
            }

            dib_bits[dst_idx + 0] = b;  /* Blue */
            dib_bits[dst_idx + 1] = g;  /* Green */
            dib_bits[dst_idx + 2] = r;  /* Red */
            dib_bits[dst_idx + 3] = 255; /* Alpha */
        }
    }

    blit_image_to_dc(ctx, hbm, width, height);
    DeleteObject(hbm);
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
            render_image(ctx, xobj);
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
 * Skip inline image data (BI...ID...EI).
 * After seeing "BI", we need to skip the image dictionary and data
 * until we find "EI".
 */
static void skip_inline_image(CSParser *p)
{
    /* We're past "BI". First skip the image dictionary key-value pairs
     * until we find "ID". */
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

        /* Skip this token */
        CSToken dummy;
        if (!cs_next_token(p, &dummy)) break;
        if (dummy.str_data) free(dummy.str_data);
    }

    /* Now skip binary data until "EI" preceded by whitespace */
    while (!cs_at_end(p)) {
        if (p->pos + 2 <= p->length) {
            /* Look for whitespace followed by "EI" followed by whitespace/delimiter/EOF */
            if (cs_is_whitespace(p->data[p->pos]) &&
                p->pos + 2 < p->length &&
                p->data[p->pos + 1] == 'E' &&
                p->data[p->pos + 2] == 'I') {
                /* Verify EI is followed by whitespace/delimiter/EOF */
                if (p->pos + 3 >= p->length ||
                    cs_is_whitespace(p->data[p->pos + 3]) ||
                    cs_is_delimiter(p->data[p->pos + 3])) {
                    p->pos += 3; /* skip whitespace + "EI" */
                    return;
                }
            }
        }
        p->pos++;
    }
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
        else if (strcmp(op, "d") == 0 && ops.count >= 2) {
            /* Dash pattern: [array] phase -- store but ignore for v1 rendering */
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
             * Clipping: v1 approximation.
             * We replay the current path into a GDI clipping region.
             * Full correctness would require intersecting with the existing clip.
             */
            if (path.count > 0) {
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
            }
            /* Note: the path is NOT consumed by W/W*; the next painting operator
             * (or 'n') will consume it. */
        }

        /* ── Color Operators ── */

        else if (strcmp(op, "g") == 0 && ops.count >= 1) {
            double gray = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->fill_color = (PdfColor){gray, gray, gray};
        }
        else if (strcmp(op, "G") == 0 && ops.count >= 1) {
            double gray = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->stroke_color = (PdfColor){gray, gray, gray};
        }
        else if (strcmp(op, "rg") == 0 && ops.count >= 3) {
            current_gs(ctx)->fill_color.r = opstack_number(&ops, ops.count - 3);
            current_gs(ctx)->fill_color.g = opstack_number(&ops, ops.count - 2);
            current_gs(ctx)->fill_color.b = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "RG") == 0 && ops.count >= 3) {
            current_gs(ctx)->stroke_color.r = opstack_number(&ops, ops.count - 3);
            current_gs(ctx)->stroke_color.g = opstack_number(&ops, ops.count - 2);
            current_gs(ctx)->stroke_color.b = opstack_number(&ops, ops.count - 1);
        }
        else if (strcmp(op, "k") == 0 && ops.count >= 4) {
            double c_ = opstack_number(&ops, ops.count - 4);
            double m_ = opstack_number(&ops, ops.count - 3);
            double y_ = opstack_number(&ops, ops.count - 2);
            double k_ = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->fill_color = cmyk_to_rgb(c_, m_, y_, k_);
        }
        else if (strcmp(op, "K") == 0 && ops.count >= 4) {
            double c_ = opstack_number(&ops, ops.count - 4);
            double m_ = opstack_number(&ops, ops.count - 3);
            double y_ = opstack_number(&ops, ops.count - 2);
            double k_ = opstack_number(&ops, ops.count - 1);
            current_gs(ctx)->stroke_color = cmyk_to_rgb(c_, m_, y_, k_);
        }
        else if (strcmp(op, "cs") == 0 && ops.count >= 1) {
            /* Set fill color space. For DeviceGray/RGB/CMYK the operand count
             * in subsequent sc/scn calls determines the interpretation.
             * We don't need to store this explicitly for basic color spaces. */
        }
        else if (strcmp(op, "CS") == 0 && ops.count >= 1) {
            /* Set stroke color space -- same approach as cs */
        }
        else if (strcmp(op, "sc") == 0 || strcmp(op, "scn") == 0) {
            /* Set fill color based on operand count */
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
        else if (strcmp(op, "SC") == 0 || strcmp(op, "SCN") == 0) {
            /* Set stroke color based on operand count */
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
            skip_inline_image(&parser);
        }

        /* ── Marked Content (consume operands, no-op) ── */

        else if (strcmp(op, "BMC") == 0 ||
                 strcmp(op, "BDC") == 0 ||
                 strcmp(op, "EMC") == 0 ||
                 strcmp(op, "MP") == 0 ||
                 strcmp(op, "DP") == 0) {
            /* Ignore -- operands are consumed by clearing the stack below */
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
            interpret_stream(&ctx, resources, content_data, content_len);
        }
        free(content_data);
    }

    /* Cleanup */
    SelectObject(mem_dc, old_bm);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    return hbm;
}
