/*
 * AmundsPDF - pdf_types.h
 * Shared type definitions and macros for the PDF viewer.
 * All modules include this header.
 */
#ifndef PDF_TYPES_H
#define PDF_TYPES_H

#define _WIN32_WINNT  0x0601
#define WINVER        0x0601
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ─── Limits ─── */
#define PDF_MAX_NAME_LEN    256
#define PDF_MAX_STRING_LEN  65536
#define PDF_MAX_ARRAY_SIZE  4096
#define PDF_MAX_DICT_SIZE   256
#define PDF_MAX_OPERANDS    64
#define PDF_MAX_GSTATE_STACK 64
#define PDF_MAX_PAGES       100000

/* ─── PDF Object Types ─── */
typedef enum {
    PDF_OBJ_NULL = 0,
    PDF_OBJ_BOOL,
    PDF_OBJ_INT,
    PDF_OBJ_REAL,
    PDF_OBJ_NAME,
    PDF_OBJ_STRING,
    PDF_OBJ_ARRAY,
    PDF_OBJ_DICT,
    PDF_OBJ_STREAM,
    PDF_OBJ_REF,       /* indirect reference (obj_num gen R) */
} PdfObjType;

/* Forward declaration */
typedef struct PdfObj PdfObj;
typedef struct PdfDict PdfDict;
typedef struct PdfArray PdfArray;

/* ─── PDF Dictionary Entry ─── */
typedef struct {
    char        key[PDF_MAX_NAME_LEN];
    PdfObj     *value;
} PdfDictEntry;

/* ─── PDF Dictionary ─── */
struct PdfDict {
    PdfDictEntry   *entries;
    int             count;
    int             capacity;
};

/* ─── PDF Array ─── */
struct PdfArray {
    PdfObj    **items;
    int         count;
    int         capacity;
};

/* ─── PDF Stream ─── */
typedef struct {
    PdfDict    *dict;           /* stream dictionary */
    uint8_t    *raw_data;       /* raw (possibly compressed) data */
    size_t      raw_length;     /* length of raw data */
    uint8_t    *decoded_data;   /* decoded data (after filters) */
    size_t      decoded_length; /* length of decoded data */
    int         obj_num;        /* owning object number (for decryption) */
    int         gen_num;        /* owning generation number (for decryption) */
    bool        decrypted;      /* true if raw_data has been decrypted already */
} PdfStream;

/* ─── PDF Indirect Reference ─── */
typedef struct {
    int obj_num;
    int gen_num;
} PdfRef;

/* ─── PDF Object ─── */
struct PdfObj {
    PdfObjType type;
    union {
        bool        boolean;
        int64_t     integer;
        double      real;
        char       *name;       /* heap-allocated */
        struct {
            uint8_t *data;      /* heap-allocated */
            size_t   length;
        } string;
        PdfArray   *array;
        PdfDict    *dict;
        PdfStream  *stream;
        PdfRef      ref;
    };
};

/* ─── Cross-Reference Entry ─── */
typedef struct {
    int64_t     offset;     /* byte offset in file (for 'n' entries) */
    int         gen_num;
    bool        in_use;     /* true = 'n', false = 'f' */
    /* For xref streams: object stream support */
    int         obj_stream_num;  /* -1 if not in object stream */
    int         obj_stream_idx;  /* index within object stream */
} XRefEntry;

/* ─── PDF Document ─── */
typedef struct PdfCryptState PdfCryptState;  /* forward declaration (defined in pdf_crypt.h) */

typedef struct {
    /* Memory-mapped file */
    HANDLE      hFile;
    HANDLE      hMapping;
    const uint8_t *data;
    size_t      data_len;

    /* Cross-reference table */
    XRefEntry  *xref;
    int         xref_count;

    /* Trailer dictionary */
    PdfObj     *trailer;

    /* Page cache */
    PdfObj    **pages;      /* array of page dict refs */
    int         page_count;

    /* Version */
    int         version_major;
    int         version_minor;

    /* Encryption state (NULL if not encrypted) */
    PdfCryptState *crypt;
} PdfDocument;

/* ─── PDF Matrix (3x3 affine, stored as 6 values: a b c d e f) ─── */
typedef struct {
    double a, b, c, d, e, f;
} PdfMatrix;

/* ─── Color ─── */
typedef struct {
    double r, g, b;     /* normalized 0..1 */
} PdfColor;

/* ─── Color Space Types ─── */
typedef enum {
    PDF_CS_UNKNOWN = 0,
    PDF_CS_DEVICE_GRAY,
    PDF_CS_DEVICE_RGB,
    PDF_CS_DEVICE_CMYK,
    PDF_CS_ICCBASED,
    PDF_CS_INDEXED,
    PDF_CS_SEPARATION,
    PDF_CS_DEVICEN,
    PDF_CS_PATTERN,
} PdfColorSpaceType;

#define PDF_MAX_INDEXED_PALETTE  256  /* max entries in an Indexed palette */

typedef struct {
    PdfColorSpaceType type;
    int               components;            /* number of input components */
    char              name[PDF_MAX_NAME_LEN]; /* resource name (e.g. "CS0") */

    /* For Indexed color spaces */
    int               indexed_hival;          /* max palette index */
    uint8_t           indexed_palette[PDF_MAX_INDEXED_PALETTE * 4]; /* palette RGB(A) data */
    int               indexed_base_components; /* components per entry in base space (3=RGB, 4=CMYK, 1=Gray) */
    PdfColorSpaceType indexed_base_type;       /* base color space type */
} PdfColorSpaceInfo;

/* ─── Graphics State ─── */
typedef struct {
    PdfMatrix   ctm;            /* current transformation matrix */
    PdfColor    fill_color;
    PdfColor    stroke_color;
    double      line_width;
    int         line_cap;       /* 0=butt, 1=round, 2=square */
    int         line_join;      /* 0=miter, 1=round, 2=bevel */
    double      miter_limit;
    double      font_size;
    char        font_name[PDF_MAX_NAME_LEN];
    /* Text state */
    double      char_spacing;
    double      word_spacing;
    double      horiz_scaling;  /* percentage, default 100 */
    double      leading;
    int         text_render_mode;
    double      text_rise;
    /* Color space state */
    PdfColorSpaceInfo fill_cs;
    PdfColorSpaceInfo stroke_cs;
} PdfGraphicsState;

/* ─── Render Context (passed to renderer) ─── */
typedef struct {
    HDC         hdc;            /* target DC */
    int         page_width_px;  /* rendered page width in pixels */
    int         page_height_px; /* rendered page height in pixels */
    double      scale;          /* user units to pixels */
    double      page_width;     /* page width in user units (from MediaBox) */
    double      page_height;    /* page height in user units */

    /* Graphics state stack */
    PdfGraphicsState  gstate[PDF_MAX_GSTATE_STACK];
    int               gstate_depth;

    /* Text state */
    PdfMatrix   text_matrix;
    PdfMatrix   text_line_matrix;

    /* Reference to document for resolving indirect refs */
    PdfDocument *doc;
} PdfRenderCtx;

/* ─── Utility Macros ─── */
#define PDF_IDENTITY_MATRIX  (PdfMatrix){1.0, 0.0, 0.0, 1.0, 0.0, 0.0}

static inline PdfMatrix pdf_matrix_multiply(PdfMatrix m1, PdfMatrix m2) {
    PdfMatrix r;
    r.a = m1.a * m2.a + m1.b * m2.c;
    r.b = m1.a * m2.b + m1.b * m2.d;
    r.c = m1.c * m2.a + m1.d * m2.c;
    r.d = m1.c * m2.b + m1.d * m2.d;
    r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return r;
}

static inline void pdf_transform_point(PdfMatrix m, double x, double y, double *ox, double *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

#endif /* PDF_TYPES_H */
