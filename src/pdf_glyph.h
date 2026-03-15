/*
 * AmundsPDF - pdf_glyph.h
 * Common glyph outline representation and rendering API.
 *
 * Both TrueType and CFF font parsers output GlyphOutline structs.
 * The renderer transforms outlines through the text matrix / CTM
 * and draws them as filled GDI paths (BeginPath/EndPath/FillPath).
 *
 * This replaces GDI TextOutW for pixel-perfect font rendering
 * using the actual embedded font outlines from the PDF.
 */
#ifndef PDF_GLYPH_H
#define PDF_GLYPH_H

#include "pdf_types.h"

/* ─── Glyph Path Commands ─── */
typedef enum {
    GLYPH_MOVETO,       /* Start new subpath: move to (x, y) */
    GLYPH_LINETO,       /* Line to (x, y) */
    GLYPH_CURVETO,      /* Cubic bezier: ctrl1=(cx1,cy1), ctrl2=(cx2,cy2), end=(x,y) */
    GLYPH_CLOSE,        /* Close current subpath */
} GlyphCmdType;

typedef struct {
    GlyphCmdType type;
    double x, y;        /* endpoint (or moveto target) */
    double cx1, cy1;    /* control point 1 (curveto only) */
    double cx2, cy2;    /* control point 2 (curveto only) */
} GlyphCmd;

/* ─── Glyph Outline ─── */
typedef struct {
    GlyphCmd   *cmds;
    int         count;
    int         capacity;
    double      advance_width;   /* horizontal advance in font units */
} GlyphOutline;

/* ─── Parsed Font (opaque, format-specific internals) ─── */
typedef struct ParsedFont ParsedFont;

struct ParsedFont {
    /* Common metrics (in font design units) */
    int         units_per_em;    /* typically 1000 (CFF) or 2048 (TT) */
    int         ascent;          /* typographic ascent */
    int         descent;         /* typographic descent (usually negative) */
    int         num_glyphs;

    /* Font identification */
    char        family_name[256];
    bool        is_bold;
    bool        is_italic;
    bool        is_cff;          /* true = CFF, false = TrueType */
    bool        is_type1;        /* true = Type1 (PFB/PFA) */

    /* Raw font data (kept alive for glyph extraction) */
    uint8_t    *data;
    size_t      data_len;
    bool        owns_data;       /* true if we should free data on cleanup */

    /* ── CFF-specific fields ── */
    /* Offsets and sizes into the CFF data */
    const uint8_t *cff_start;    /* start of CFF data within font file */
    size_t      cff_len;

    /* CharStrings INDEX */
    const uint8_t *charstrings_index;
    int         charstrings_count;

    /* Subr INDEX (local) */
    const uint8_t *local_subr_index;
    int         local_subr_count;
    int         local_subr_bias;

    /* Global Subr INDEX */
    const uint8_t *global_subr_index;
    int         global_subr_count;
    int         global_subr_bias;

    /* Default width and nominal width from Private DICT */
    double      default_width;
    double      nominal_width;

    /* Encoding: maps character code -> glyph index */
    /* For standard encoding, code == glyph index (approximately) */
    int         encoding[256];   /* char code -> GID mapping */

    /* Per-glyph advance widths (if available from hmtx or CFF) */
    double     *glyph_widths;    /* array of num_glyphs widths, or NULL */

    /* ── CFF charset: GID → name mapping ── */
    /* charset_sids[gid] = SID (String ID) for that glyph.
     * SIDs 0-390 are standard CFF strings; SIDs >= 391 index
     * into the font's String INDEX (at sid - 391). */
    uint16_t   *charset_sids;    /* array of num_glyphs SIDs, or NULL */

    /* String INDEX: needed to resolve SIDs >= 391 to glyph names */
    const uint8_t *string_index_ptr; /* pointer to String INDEX in CFF data */
    int         string_index_count;  /* number of entries in String INDEX */

    /* ── Type1-specific fields ── */
    uint8_t    *t1_subrs_data;       /* concatenated decrypted subroutine charstrings */
    size_t     *t1_subrs_offs;       /* offset of each subroutine within t1_subrs_data */
    size_t     *t1_subrs_lens;       /* lengths of each subroutine */
    int         t1_subrs_count;      /* number of subroutines */

    uint8_t    *t1_charstrings_data; /* concatenated decrypted charstring data */
    size_t     *t1_charstrings_offs; /* offset of each charstring within t1_charstrings_data */
    size_t     *t1_charstrings_lens; /* length of each charstring */
    char      **t1_glyph_names;      /* glyph names (for GID->name mapping) */
    int         t1_num_charstrings;  /* number of charstrings */

    int         t1_lenIV;            /* charstring encryption skip bytes (default 4) */

    /* ── TrueType-specific fields ── */
    const uint8_t *glyf_table;
    size_t      glyf_len;
    const uint8_t *loca_table;
    size_t      loca_len;
    bool        loca_is_long;    /* true = long format (32-bit offsets) */
    const uint8_t *hmtx_table;
    int         num_h_metrics;
    const uint8_t *cmap_table;
    size_t      cmap_len;
};

/* ─── Font Parsing API ─── */

/*
 * Parse a CFF font from raw Type1C data (from /FontFile3 /Subtype /Type1C).
 * Returns a ParsedFont on success, NULL on failure.
 * Caller must call parsed_font_free() when done.
 */
ParsedFont *parsed_font_from_cff(const uint8_t *data, size_t len);

/*
 * Parse a TrueType font from raw data (from /FontFile2).
 * Returns a ParsedFont on success, NULL on failure.
 * Caller must call parsed_font_free() when done.
 */
ParsedFont *parsed_font_from_truetype(const uint8_t *data, size_t len);

/*
 * Parse a Type1 font from raw PFB/PFA data (from /FontFile).
 * Returns a ParsedFont on success, NULL on failure.
 * Caller must call parsed_font_free() when done.
 */
ParsedFont *parsed_font_from_type1(const uint8_t *data, size_t len);

/* ─── Type1 glyph extraction (called from dispatch in pdf_glyph_cff.c) ─── */
bool t1_get_glyph_by_gid(ParsedFont *font, int gid, GlyphOutline *outline);
int  t1_find_gid_by_name(ParsedFont *font, const char *glyph_name);

/* ─── TrueType glyph extraction (called from dispatch in pdf_glyph_cff.c) ─── */
bool tt_get_glyph_by_gid(ParsedFont *font, int gid, GlyphOutline *outline);

/*
 * Remap encoding[256] for a TrueType system font using Windows-1252 → Unicode
 * → cmap lookup. Call this after parsed_font_from_truetype() on system fonts
 * so that WinAnsiEncoding character codes (0x80-0x9F smart quotes, dashes, etc.)
 * resolve to the correct glyphs.
 */
void tt_remap_encoding_win1252(ParsedFont *font);

/*
 * Free a parsed font and all associated resources.
 */
void parsed_font_free(ParsedFont *font);

/* ─── Glyph Extraction API ─── */

/*
 * Get the glyph outline for a character code.
 * char_code: 0-255 character code (WinAnsiEncoding or as per font encoding)
 * outline: filled on success with the glyph path commands
 *
 * Returns true on success. Caller must call glyph_outline_free() on the outline.
 * Coordinates are in font design units (divide by units_per_em to normalize).
 */
bool parsed_font_get_glyph(ParsedFont *font, int char_code, GlyphOutline *outline);

/*
 * Get the advance width for a character code (in font design units).
 */
double parsed_font_get_advance(ParsedFont *font, int char_code);

/*
 * Get the glyph outline for a specific GID (bypassing encoding).
 * Used when the caller has already resolved the GID via charset lookup.
 */
bool parsed_font_get_glyph_by_gid(ParsedFont *font, int gid, GlyphOutline *outline);

/*
 * Find the GID for a glyph by name.
 * Searches the CFF charset (GID→SID→name mapping) for a match.
 * Returns the GID, or -1 if not found.
 */
int parsed_font_find_gid_by_name(ParsedFont *font, const char *glyph_name);

/*
 * Free the commands array in a glyph outline.
 */
void glyph_outline_free(GlyphOutline *outline);

/* ─── Glyph Outline Helpers ─── */

/* Initialize an empty outline */
static inline void glyph_outline_init(GlyphOutline *o) {
    o->cmds = NULL;
    o->count = 0;
    o->capacity = 0;
    o->advance_width = 0;
}

/* Add a command to an outline (auto-grows) */
static inline bool glyph_outline_add(GlyphOutline *o, GlyphCmd cmd) {
    if (o->count >= o->capacity) {
        int new_cap = o->capacity ? o->capacity * 2 : 64;
        GlyphCmd *new_cmds = (GlyphCmd *)realloc(o->cmds, new_cap * sizeof(GlyphCmd));
        if (!new_cmds) return false;
        o->cmds = new_cmds;
        o->capacity = new_cap;
    }
    o->cmds[o->count++] = cmd;
    return true;
}

#endif /* PDF_GLYPH_H */
