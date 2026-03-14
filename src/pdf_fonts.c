/*
 * AmundsPDF - pdf_fonts.c
 * Standard 14 PDF font metrics and Windows system font mapping.
 *
 * Provides:
 *   - Mapping from PDF /BaseFont names to Win32 HFONT objects
 *   - Character width tables for Helvetica, Times-Roman, and Courier
 *     (WinAnsiEncoding, 256 entries, units of 1/1000 text space)
 *   - Heuristic fallback for non-standard font names
 */

#include "pdf_fonts.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Standard 14 Font Mapping Table
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *pdf_name;       /* PDF /BaseFont name */
    const char *win_family;     /* Windows font family name */
    int         weight;         /* FW_NORMAL=400, FW_BOLD=700 */
    BOOL        italic;         /* TRUE for italic/oblique */
} FontMapping;

static const FontMapping g_std14_fonts[] = {
    { "Helvetica",              "Arial",            FW_NORMAL, FALSE },
    { "Helvetica-Bold",         "Arial",            FW_BOLD,   FALSE },
    { "Helvetica-Oblique",      "Arial",            FW_NORMAL, TRUE  },
    { "Helvetica-BoldOblique",  "Arial",            FW_BOLD,   TRUE  },
    { "Times-Roman",            "Times New Roman",  FW_NORMAL, FALSE },
    { "Times-Bold",             "Times New Roman",  FW_BOLD,   FALSE },
    { "Times-Italic",           "Times New Roman",  FW_NORMAL, TRUE  },
    { "Times-BoldItalic",       "Times New Roman",  FW_BOLD,   TRUE  },
    { "Courier",                "Courier New",      FW_NORMAL, FALSE },
    { "Courier-Bold",           "Courier New",      FW_BOLD,   FALSE },
    { "Courier-Oblique",        "Courier New",      FW_NORMAL, TRUE  },
    { "Courier-BoldOblique",    "Courier New",      FW_BOLD,   TRUE  },
    { "Symbol",                 "Symbol",           FW_NORMAL, FALSE },
    { "ZapfDingbats",           "Wingdings",        FW_NORMAL, FALSE },
    { NULL, NULL, 0, 0 }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Character Width Tables (WinAnsiEncoding, 256 entries)
 * Units: 1/1000 of a text space unit (standard PDF metrics)
 * Source: PDF Reference Appendix D / AFM files
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Helvetica (Arial) character widths.
 * Codes 0-31 and some gaps use 0 (undefined/notdef glyph).
 */
static const int g_helvetica_widths[256] = {
    /* 0x00 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x08 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x10 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x18 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x20 space ! " # $ % & ' */
               278, 278, 355, 556, 556, 889, 667, 191,
    /* 0x28 ( ) * + , - . / */
               333, 333, 389, 584, 278, 333, 278, 278,
    /* 0x30 0 1 2 3 4 5 6 7 */
               556, 556, 556, 556, 556, 556, 556, 556,
    /* 0x38 8 9 : ; < = > ? */
               556, 556, 278, 278, 584, 584, 584, 556,
    /* 0x40 @ A B C D E F G */
              1015, 667, 667, 722, 722, 667, 611, 778,
    /* 0x48 H I J K L M N O */
               722, 278, 500, 667, 556, 833, 722, 778,
    /* 0x50 P Q R S T U V W */
               667, 778, 722, 667, 611, 722, 667, 944,
    /* 0x58 X Y Z [ \ ] ^ _ */
               667, 667, 611, 278, 278, 278, 469, 556,
    /* 0x60 ` a b c d e f g */
               333, 556, 556, 500, 556, 556, 278, 556,
    /* 0x68 h i j k l m n o */
               556, 222, 222, 500, 222, 833, 556, 556,
    /* 0x70 p q r s t u v w */
               556, 556, 333, 500, 278, 556, 500, 722,
    /* 0x78 x y z { | } ~ DEL */
               500, 500, 500, 334, 260, 334, 584, 0,
    /* 0x80-0x8F (extended Latin: Euro, quotes, dagger, etc.) */
               556, 0,   222, 556, 333, 1000,556, 556,
               333, 1000,667, 333, 1000,0,   611, 0,
    /* 0x90-0x9F */
               0,   222, 222, 333, 333, 350, 556, 1000,
               333, 1000,500, 333, 944, 0,   500, 667,
    /* 0xA0-0xAF (NBSP, inverted!, cent, pound, currency, yen, ...) */
               278, 333, 556, 556, 556, 556, 260, 556,
               333, 737, 370, 556, 584, 333, 737, 333,
    /* 0xB0-0xBF (degree, plusminus, super2, super3, ...) */
               400, 584, 333, 333, 333, 556, 537, 278,
               333, 333, 365, 556, 834, 834, 834, 611,
    /* 0xC0-0xCF (Agrave..Iuml) */
               667, 667, 667, 667, 667, 667, 1000,722,
               667, 667, 667, 667, 278, 278, 278, 278,
    /* 0xD0-0xDF (Eth..germandbls) */
               722, 722, 778, 778, 778, 778, 778, 584,
               778, 722, 722, 722, 722, 667, 667, 611,
    /* 0xE0-0xEF (agrave..iuml) */
               556, 556, 556, 556, 556, 556, 889, 500,
               556, 556, 556, 556, 278, 278, 278, 278,
    /* 0xF0-0xFF (eth..ydieresis) */
               556, 556, 556, 556, 556, 584, 611, 556,
               556, 556, 556, 500, 556, 500, 556, 500
};

/*
 * Helvetica-Bold character widths.
 */
static const int g_helvetica_bold_widths[256] = {
    /* 0x00-0x1F */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 0x20 space ! " # $ % & ' */
    278, 333, 474, 556, 556, 889, 722, 238,
    /* 0x28 ( ) * + , - . / */
    333, 333, 389, 584, 278, 333, 278, 278,
    /* 0x30 0-7 */
    556, 556, 556, 556, 556, 556, 556, 556,
    /* 0x38 8 9 : ; < = > ? */
    556, 556, 333, 333, 584, 584, 584, 611,
    /* 0x40 @ A B C D E F G */
    975, 722, 722, 722, 722, 667, 611, 778,
    /* 0x48 H I J K L M N O */
    722, 278, 556, 722, 611, 833, 722, 778,
    /* 0x50 P Q R S T U V W */
    667, 778, 722, 667, 611, 722, 667, 944,
    /* 0x58 X Y Z [ \ ] ^ _ */
    667, 667, 611, 333, 278, 333, 584, 556,
    /* 0x60 ` a b c d e f g */
    333, 556, 611, 556, 611, 556, 333, 611,
    /* 0x68 h i j k l m n o */
    611, 278, 278, 556, 278, 889, 611, 611,
    /* 0x70 p q r s t u v w */
    611, 611, 389, 556, 333, 611, 556, 778,
    /* 0x78 x y z { | } ~ DEL */
    556, 556, 500, 389, 280, 389, 584, 0,
    /* 0x80-0xFF: use same as regular for simplicity */
    556, 0,   278, 556, 500, 1000,556, 556,
    333, 1000,667, 333, 1000,0,   611, 0,
    0,   278, 278, 500, 500, 350, 556, 1000,
    333, 1000,556, 333, 944, 0,   500, 667,
    278, 333, 556, 556, 556, 556, 280, 556,
    333, 737, 370, 556, 584, 333, 737, 333,
    400, 584, 333, 333, 333, 611, 556, 278,
    333, 333, 365, 556, 834, 834, 834, 611,
    722, 722, 722, 722, 722, 722, 1000,722,
    667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584,
    778, 722, 722, 722, 722, 667, 667, 611,
    556, 556, 556, 556, 556, 556, 889, 556,
    556, 556, 556, 556, 278, 278, 278, 278,
    611, 611, 611, 611, 611, 584, 611, 611,
    611, 611, 611, 556, 611, 556, 611, 556
};

/*
 * Times-Roman character widths.
 */
static const int g_times_widths[256] = {
    /* 0x00-0x1F */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 0x20 space ! " # $ % & ' */
    250, 333, 408, 500, 500, 833, 778, 180,
    /* 0x28 ( ) * + , - . / */
    333, 333, 500, 564, 250, 333, 250, 278,
    /* 0x30 0-7 */
    500, 500, 500, 500, 500, 500, 500, 500,
    /* 0x38 8 9 : ; < = > ? */
    500, 500, 278, 278, 564, 564, 564, 444,
    /* 0x40 @ A B C D E F G */
    921, 722, 667, 667, 722, 611, 556, 722,
    /* 0x48 H I J K L M N O */
    722, 333, 389, 722, 611, 889, 722, 722,
    /* 0x50 P Q R S T U V W */
    556, 722, 667, 556, 611, 722, 722, 944,
    /* 0x58 X Y Z [ \ ] ^ _ */
    722, 722, 611, 333, 278, 333, 469, 500,
    /* 0x60 ` a b c d e f g */
    333, 444, 500, 444, 500, 444, 333, 500,
    /* 0x68 h i j k l m n o */
    500, 278, 278, 500, 278, 778, 500, 500,
    /* 0x70 p q r s t u v w */
    500, 500, 333, 389, 278, 500, 500, 722,
    /* 0x78 x y z { | } ~ DEL */
    500, 500, 444, 480, 200, 480, 541, 0,
    /* 0x80-0xFF */
    500, 0,   333, 500, 444, 1000,500, 500,
    333, 1000,556, 333, 889, 0,   611, 0,
    0,   333, 333, 444, 444, 350, 500, 1000,
    333, 1000,389, 333, 722, 0,   444, 722,
    250, 333, 500, 500, 500, 500, 200, 500,
    333, 760, 276, 500, 564, 333, 760, 333,
    400, 564, 300, 300, 333, 500, 453, 250,
    333, 300, 310, 500, 750, 750, 750, 444,
    722, 722, 722, 722, 722, 722, 889, 667,
    611, 611, 611, 611, 333, 333, 333, 333,
    722, 722, 722, 722, 722, 722, 722, 564,
    722, 722, 722, 722, 722, 722, 556, 500,
    444, 444, 444, 444, 444, 444, 667, 444,
    444, 444, 444, 444, 278, 278, 278, 278,
    500, 500, 500, 500, 500, 564, 500, 500,
    500, 500, 500, 500, 500, 500, 500, 500
};

/*
 * Times-Bold character widths.
 */
static const int g_times_bold_widths[256] = {
    /* 0x00-0x1F */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 0x20 space ! " # $ % & ' */
    250, 333, 555, 500, 500, 1000,833, 278,
    /* 0x28 ( ) * + , - . / */
    333, 333, 500, 570, 250, 333, 250, 278,
    /* 0x30 0-7 */
    500, 500, 500, 500, 500, 500, 500, 500,
    /* 0x38 8 9 : ; < = > ? */
    500, 500, 333, 333, 570, 570, 570, 500,
    /* 0x40 @ A B C D E F G */
    930, 722, 667, 722, 722, 667, 611, 778,
    /* 0x48 H I J K L M N O */
    778, 389, 500, 778, 667, 944, 722, 778,
    /* 0x50 P Q R S T U V W */
    611, 778, 722, 556, 667, 722, 722, 1000,
    /* 0x58 X Y Z [ \ ] ^ _ */
    722, 722, 667, 333, 278, 333, 581, 500,
    /* 0x60 ` a b c d e f g */
    333, 500, 556, 444, 556, 444, 333, 500,
    /* 0x68 h i j k l m n o */
    556, 278, 333, 556, 278, 833, 556, 500,
    /* 0x70 p q r s t u v w */
    556, 556, 444, 389, 333, 556, 500, 722,
    /* 0x78 x y z { | } ~ DEL */
    500, 500, 444, 394, 220, 394, 520, 0,
    /* 0x80-0xFF (approximate) */
    500, 0,   333, 500, 500, 1000,500, 500,
    333, 1000,556, 333, 1000,0,   667, 0,
    0,   333, 333, 500, 500, 350, 500, 1000,
    333, 1000,389, 333, 722, 0,   444, 722,
    250, 333, 500, 500, 500, 500, 220, 500,
    333, 747, 300, 500, 570, 333, 747, 333,
    400, 570, 300, 300, 333, 556, 540, 250,
    333, 300, 330, 500, 750, 750, 750, 500,
    722, 722, 722, 722, 722, 722, 1000,722,
    667, 667, 667, 667, 389, 389, 389, 389,
    722, 722, 778, 778, 778, 778, 778, 570,
    778, 722, 722, 722, 722, 722, 611, 556,
    500, 500, 500, 500, 500, 500, 722, 444,
    444, 444, 444, 444, 278, 278, 278, 278,
    500, 556, 500, 500, 500, 570, 500, 556,
    556, 556, 556, 500, 556, 500, 556, 500
};

/*
 * Courier is monospaced: every glyph is 600 units wide.
 * We don't need a table, but we provide a small helper for consistency.
 */
#define COURIER_WIDTH 600

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Case-insensitive substring search */
static bool str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/* Case-insensitive exact string compare */
static bool str_eq_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

/*
 * Find the standard 14 mapping for a given PDF base font name.
 * Returns NULL if not one of the standard 14.
 */
static const FontMapping *find_std14(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; g_std14_fonts[i].pdf_name; i++) {
        if (str_eq_ci(name, g_std14_fonts[i].pdf_name))
            return &g_std14_fonts[i];
    }
    return NULL;
}

/*
 * Determine which width table to use for a given font name.
 * Returns: 0 = Helvetica, 1 = Helvetica-Bold, 2 = Times, 3 = Times-Bold, 4 = Courier
 */
static int classify_font(const char *name)
{
    if (!name) return 0;

    /* Check for Courier / monospace first */
    if (str_contains_ci(name, "Courier") || str_contains_ci(name, "Mono"))
        return 4;

    /* Check for Times / serif */
    if (str_contains_ci(name, "Times") || str_contains_ci(name, "Serif")) {
        if (str_contains_ci(name, "Bold"))
            return 3;
        return 2;
    }

    /* Default: Helvetica family */
    if (str_contains_ci(name, "Bold"))
        return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

HFONT pdf_font_create(const char *base_font_name, double size_pt, HDC hdc)
{
    const char *win_family = "Arial";
    int weight = FW_NORMAL;
    BOOL italic = FALSE;
    BYTE charset = DEFAULT_CHARSET;

    if (!base_font_name || !base_font_name[0])
        base_font_name = "Helvetica";

    /* Try exact match against standard 14 */
    const FontMapping *mapping = find_std14(base_font_name);
    if (mapping) {
        win_family = mapping->win_family;
        weight     = mapping->weight;
        italic     = mapping->italic;
        /* Symbol and ZapfDingbats use special charsets */
        if (str_eq_ci(base_font_name, "Symbol"))
            charset = SYMBOL_CHARSET;
        else if (str_eq_ci(base_font_name, "ZapfDingbats"))
            charset = SYMBOL_CHARSET;
    } else {
        /*
         * Heuristic matching for non-standard fonts.
         * Look for keywords in the font name to guess weight/style/family.
         */
        bool is_bold   = str_contains_ci(base_font_name, "Bold");
        bool is_italic = str_contains_ci(base_font_name, "Italic")
                      || str_contains_ci(base_font_name, "Oblique");
        bool is_mono   = str_contains_ci(base_font_name, "Mono")
                      || str_contains_ci(base_font_name, "Courier")
                      || str_contains_ci(base_font_name, "Fixed");
        bool is_serif  = str_contains_ci(base_font_name, "Serif")
                      || str_contains_ci(base_font_name, "Times")
                      || str_contains_ci(base_font_name, "Roman")
                      || str_contains_ci(base_font_name, "Georgia")
                      || str_contains_ci(base_font_name, "Garamond")
                      || str_contains_ci(base_font_name, "Palatino")
                      || str_contains_ci(base_font_name, "Bookman")
                      || str_contains_ci(base_font_name, "Century");

        if (is_mono)
            win_family = "Courier New";
        else if (is_serif)
            win_family = "Times New Roman";
        else
            win_family = "Arial";   /* sans-serif fallback */

        weight = is_bold ? FW_BOLD : FW_NORMAL;
        italic = is_italic ? TRUE : FALSE;
    }

    /*
     * Convert point size to logical units.
     * LOGFONT height = -(size_pt * dpi / 72)
     * We use the DC's DPI if available, else 96.
     */
    int dpi = 96;
    if (hdc) {
        int dev_dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        if (dev_dpi > 0)
            dpi = dev_dpi;
    }
    int height = -(int)(size_pt * (double)dpi / 72.0 + 0.5);
    if (height == 0) height = -12;  /* minimum size */

    /* Convert family name to wide string */
    wchar_t wfamily[64];
    int i;
    for (i = 0; i < 63 && win_family[i]; i++)
        wfamily[i] = (wchar_t)(unsigned char)win_family[i];
    wfamily[i] = L'\0';

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight         = height;
    lf.lfWidth          = 0;
    lf.lfEscapement     = 0;
    lf.lfOrientation    = 0;
    lf.lfWeight         = weight;
    lf.lfItalic         = italic;
    lf.lfUnderline      = FALSE;
    lf.lfStrikeOut      = FALSE;
    lf.lfCharSet        = charset;
    lf.lfOutPrecision   = OUT_TT_PRECIS;
    lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy(lf.lfFaceName, wfamily, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = L'\0';

    return CreateFontIndirectW(&lf);
}

HFONT pdf_font_create_px(const char *base_font_name, int height_px)
{
    const char *win_family = "Arial";
    int weight = FW_NORMAL;
    BOOL italic = FALSE;
    BYTE charset = DEFAULT_CHARSET;

    if (!base_font_name || !base_font_name[0])
        base_font_name = "Helvetica";

    const FontMapping *mapping = find_std14(base_font_name);
    if (mapping) {
        win_family = mapping->win_family;
        weight     = mapping->weight;
        italic     = mapping->italic;
        if (str_eq_ci(base_font_name, "Symbol"))
            charset = SYMBOL_CHARSET;
        else if (str_eq_ci(base_font_name, "ZapfDingbats"))
            charset = SYMBOL_CHARSET;
    } else {
        bool is_bold   = str_contains_ci(base_font_name, "Bold");
        bool is_italic = str_contains_ci(base_font_name, "Italic")
                      || str_contains_ci(base_font_name, "Oblique");
        bool is_mono   = str_contains_ci(base_font_name, "Mono")
                      || str_contains_ci(base_font_name, "Courier")
                      || str_contains_ci(base_font_name, "Fixed");
        bool is_serif  = str_contains_ci(base_font_name, "Serif")
                      || str_contains_ci(base_font_name, "Times")
                      || str_contains_ci(base_font_name, "Roman")
                      || str_contains_ci(base_font_name, "Georgia")
                      || str_contains_ci(base_font_name, "Garamond")
                      || str_contains_ci(base_font_name, "Palatino")
                      || str_contains_ci(base_font_name, "Bookman")
                      || str_contains_ci(base_font_name, "Century");
        if (is_mono)       win_family = "Courier New";
        else if (is_serif) win_family = "Times New Roman";
        else               win_family = "Arial";
        weight = is_bold ? FW_BOLD : FW_NORMAL;
        italic = is_italic ? TRUE : FALSE;
    }

    if (height_px == 0) height_px = -12;

    wchar_t wfamily[64];
    int i;
    for (i = 0; i < 63 && win_family[i]; i++)
        wfamily[i] = (wchar_t)(unsigned char)win_family[i];
    wfamily[i] = L'\0';

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight         = height_px;
    lf.lfWeight         = weight;
    lf.lfItalic         = italic;
    lf.lfCharSet        = charset;
    lf.lfOutPrecision   = OUT_TT_PRECIS;
    lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy(lf.lfFaceName, wfamily, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = L'\0';

    return CreateFontIndirectW(&lf);
}

void pdf_fonts_cleanup(void)
{
    /* Currently no global caches to free.
     * Reserved for future use (e.g., font descriptor caching). */
}

int pdf_font_char_width(const char *base_font_name, int char_code)
{
    if (char_code < 0 || char_code > 255)
        return 0;

    int cls = classify_font(base_font_name);
    switch (cls) {
        case 0:  return g_helvetica_widths[char_code];
        case 1:  return g_helvetica_bold_widths[char_code];
        case 2:  return g_times_widths[char_code];
        case 3:  return g_times_bold_widths[char_code];
        case 4:  return COURIER_WIDTH;
        default: return g_helvetica_widths[char_code];
    }
}

int pdf_font_default_width(const char *base_font_name)
{
    int cls = classify_font(base_font_name);
    switch (cls) {
        case 0:  return 556;    /* Helvetica average */
        case 1:  return 556;    /* Helvetica-Bold average */
        case 2:  return 500;    /* Times-Roman average */
        case 3:  return 500;    /* Times-Bold average */
        case 4:  return COURIER_WIDTH;
        default: return 556;
    }
}
