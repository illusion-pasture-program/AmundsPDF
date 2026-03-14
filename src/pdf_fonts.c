/*
 * AmundsPDF - pdf_fonts.c
 * Standard 14 PDF font metrics, Windows system font mapping,
 * and embedded font loading via AddFontMemResourceEx.
 *
 * Provides:
 *   - Mapping from PDF /BaseFont names to Win32 HFONT objects
 *   - Character width tables for Helvetica, Times-Roman, and Courier
 *     (WinAnsiEncoding, 256 entries, units of 1/1000 text space)
 *   - Heuristic fallback for non-standard font names
 *   - Loading of embedded TrueType/OpenType fonts from PDF streams
 */

#include "pdf_fonts.h"
#include "pdf_parser.h"

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

HFONT pdf_font_create_px(const char *base_font_name, int height_px, int rotation_deg)
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

    /* GDI escapement is in tenths of a degree */
    int escapement = rotation_deg * 10;

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight         = height_px;
    lf.lfEscapement     = escapement;
    lf.lfOrientation    = escapement;
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Embedded Font Cache and Loading
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_EMBEDDED_FONTS 128

typedef struct {
    int    obj_num;              /* PDF object number of the font stream */
    HANDLE hMemFont;             /* handle from AddFontMemResourceEx */
    char   family_name[256];     /* resolved family name for CreateFont */
    bool   loaded;               /* true if successfully loaded */
    bool   tried;                /* true if we already attempted loading */
} EmbeddedFontEntry;

static EmbeddedFontEntry g_embedded_fonts[MAX_EMBEDDED_FONTS];
static int g_embedded_font_count = 0;

/*
 * Look up an already-cached embedded font by object number.
 * Returns the cache entry index, or -1 if not found.
 */
static int embedded_font_find(int obj_num)
{
    for (int i = 0; i < g_embedded_font_count; i++) {
        if (g_embedded_fonts[i].obj_num == obj_num)
            return i;
    }
    return -1;
}

/*
 * Read a big-endian uint16 from a byte buffer.
 */
static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/*
 * Read a big-endian uint32 from a byte buffer.
 */
static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Parse the TrueType/OpenType 'name' table to extract the font family name.
 *
 * Looks for nameID=4 (Full Font Name) or nameID=1 (Font Family Name),
 * preferring platformID=3 (Windows), encodingID=1 (Unicode BMP),
 * languageID=0x0409 (English US).
 *
 * Returns true if a name was found, false otherwise.
 */
static bool ttf_get_family_name(const uint8_t *font_data, size_t font_data_len,
                                 char *out_name, int name_buf_len)
{
    if (font_data_len < 12) return false;

    /* Read the offset table */
    uint16_t num_tables = read_u16_be(font_data + 4);

    /* Each table record is 16 bytes, starting at offset 12 */
    if (font_data_len < 12 + (size_t)num_tables * 16) return false;

    /* Find the 'name' table */
    uint32_t name_table_offset = 0;
    uint32_t name_table_length = 0;
    for (int i = 0; i < num_tables; i++) {
        const uint8_t *rec = font_data + 12 + i * 16;
        uint32_t tag = read_u32_be(rec);
        if (tag == 0x6E616D65) { /* 'name' */
            name_table_offset = read_u32_be(rec + 8);
            name_table_length = read_u32_be(rec + 12);
            break;
        }
    }

    if (name_table_offset == 0 || name_table_offset + 6 > font_data_len)
        return false;

    const uint8_t *name_tbl = font_data + name_table_offset;
    if (name_table_offset + name_table_length > font_data_len)
        name_table_length = (uint32_t)(font_data_len - name_table_offset);

    /* Name table format:
     * uint16 format
     * uint16 count
     * uint16 stringOffset  (offset from start of name table to string storage)
     * NameRecord[count]    (each 12 bytes) */
    if (name_table_length < 6) return false;
    uint16_t name_count = read_u16_be(name_tbl + 2);
    uint16_t string_offset = read_u16_be(name_tbl + 4);

    if (name_table_length < 6 + (size_t)name_count * 12) return false;

    /* Strategy: look for nameID 4 (Full Name) or 1 (Family Name),
     * preferring Windows platform (3) with English (0x0409).
     * Fall back to any platform if Windows not found. */
    const uint8_t *best_rec = NULL;
    int best_name_id = 0;      /* 4 is better than 1 */
    int best_platform = -1;    /* 3 (Windows) is preferred */

    for (int i = 0; i < name_count; i++) {
        const uint8_t *rec = name_tbl + 6 + i * 12;
        uint16_t platform_id = read_u16_be(rec + 0);
        uint16_t encoding_id = read_u16_be(rec + 2);
        uint16_t language_id = read_u16_be(rec + 4);
        uint16_t name_id     = read_u16_be(rec + 6);

        if (name_id != 1 && name_id != 4)
            continue;

        /* Score this record */
        int score = 0;
        if (name_id == 4) score += 100;        /* prefer Full Name */
        if (platform_id == 3) score += 50;     /* prefer Windows */
        if (platform_id == 3 && encoding_id == 1) score += 20;
        if (language_id == 0x0409) score += 10; /* prefer English */

        int cur_best_score = 0;
        if (best_rec) {
            if (best_name_id == 4) cur_best_score += 100;
            if (best_platform == 3) cur_best_score += 50;
            /* Simplified: just compare scores */
        }

        if (!best_rec || score > cur_best_score ||
            (score == cur_best_score && name_id >= best_name_id)) {
            best_rec = rec;
            best_name_id = name_id;
            best_platform = platform_id;
        }
    }

    if (!best_rec) return false;

    uint16_t str_length = read_u16_be(best_rec + 8);
    uint16_t str_off    = read_u16_be(best_rec + 10);
    uint16_t platform   = read_u16_be(best_rec + 0);

    /* Bounds check */
    if ((uint32_t)string_offset + str_off + str_length > name_table_length)
        return false;

    const uint8_t *str_data = name_tbl + string_offset + str_off;

    if (platform == 3 || platform == 0) {
        /* UTF-16BE: convert to ASCII (sufficient for font family names) */
        int out_len = 0;
        for (int i = 0; i + 1 < str_length && out_len < name_buf_len - 1; i += 2) {
            uint16_t ch = read_u16_be(str_data + i);
            if (ch > 0 && ch < 128)
                out_name[out_len++] = (char)ch;
            else if (ch >= 128)
                out_name[out_len++] = '?';
        }
        out_name[out_len] = '\0';
        return out_len > 0;
    } else {
        /* Platform 1 (Macintosh) or other: assume single-byte encoding */
        int out_len = 0;
        for (int i = 0; i < str_length && out_len < name_buf_len - 1; i++) {
            out_name[out_len++] = (char)str_data[i];
        }
        out_name[out_len] = '\0';
        return out_len > 0;
    }
}

/*
 * Strip the subset prefix from a PDF font name.
 * PDF subset fonts often have names like "ABCDEF+ArialMT".
 * If the name starts with 6 uppercase letters followed by '+', strip that prefix.
 */
static const char *strip_subset_prefix(const char *name)
{
    if (!name) return name;
    /* Check for exactly 6 uppercase letters followed by '+' */
    for (int i = 0; i < 6; i++) {
        if (name[i] < 'A' || name[i] > 'Z')
            return name;
    }
    if (name[6] == '+')
        return name + 7;
    return name;
}

/*
 * Search a font descriptor dictionary for an embedded font stream.
 * Returns the object number of the stream, or -1 if not found.
 * Sets *out_stream_obj to the resolved stream object and
 * *out_font_file_key to the key name that was found.
 * *out_is_type1c is set to true if the stream is bare CFF (Type1C).
 */
static int search_descriptor_for_font_stream(PdfDocument *doc, PdfDict *descriptor,
                                               PdfObj **out_stream_obj,
                                               const char **out_font_file_key,
                                               bool *out_is_type1c)
{
    *out_is_type1c = false;

    /* Check for font streams in order of preference:
     * FontFile2 = TrueType (can be used directly)
     * FontFile3 = CFF or OpenType
     * FontFile  = Type1 (skip - PFB format, not usable with AddFontMemResourceEx) */
    static const char *font_file_keys[] = { "FontFile2", "FontFile3", NULL };
    for (int k = 0; font_file_keys[k]; k++) {
        PdfObj *ff = pdf_dict_get(descriptor, font_file_keys[k]);
        if (!ff) continue;

        /* Get the object number before resolving */
        int obj_num = -1;
        if (ff->type == PDF_OBJ_REF) {
            obj_num = ff->ref.obj_num;
        }

        PdfObj *resolved = pdf_resolve(doc, ff);
        if (!resolved || resolved->type != PDF_OBJ_STREAM) continue;

        /* For FontFile3, check Subtype */
        if (strcmp(font_file_keys[k], "FontFile3") == 0) {
            const char *subtype = pdf_dict_get_name(resolved->stream->dict, "Subtype");
            if (subtype && strcmp(subtype, "Type1C") == 0) {
                /* Type1C (bare CFF) - needs OpenType wrapping */
                *out_is_type1c = true;
            }
            /* OpenType, CIDFontType0C, Type1C are all accepted */
        }

        *out_stream_obj = resolved;
        *out_font_file_key = font_file_keys[k];
        return obj_num >= 0 ? obj_num : -1;
    }

    return -1;
}

/*
 * Get the object number of the font stream (FontFile2/FontFile3) for a given
 * font resource. Returns -1 if no embedded font stream found.
 *
 * Handles both simple fonts (with direct FontDescriptor) and composite/Type0
 * fonts (where the font descriptor is in the DescendantFonts CIDFont entry).
 */
static int get_font_stream_obj_num(PdfDocument *doc, PdfDict *resources,
                                     const char *font_res_name,
                                     PdfObj **out_stream_obj,
                                     const char **out_font_file_key,
                                     bool *out_is_type1c)
{
    *out_stream_obj = NULL;
    *out_font_file_key = NULL;
    *out_is_type1c = false;

    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return -1;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return -1;

    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
    if (!font_obj) return -1;

    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return -1;

    /* Try direct FontDescriptor first (simple fonts) */
    PdfObj *descriptor = pdf_dict_get(font_obj->dict, "FontDescriptor");
    if (descriptor) {
        descriptor = pdf_resolve(doc, descriptor);
        if (descriptor && descriptor->type == PDF_OBJ_DICT) {
            int result = search_descriptor_for_font_stream(
                doc, descriptor->dict, out_stream_obj, out_font_file_key, out_is_type1c);
            if (result >= 0) return result;
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

                int result = search_descriptor_for_font_stream(
                    doc, cid_desc->dict, out_stream_obj, out_font_file_key, out_is_type1c);
                if (result >= 0) return result;
            }
        }
    }

    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CFF-to-OpenType Wrapping
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Bare CFF data (Type1C from PDF /FontFile3) cannot be used directly with
 * AddFontMemResourceEx. We wrap it in a minimal OpenType font container
 * with the required tables: head, hhea, maxp, OS/2, name, cmap, post, CFF.
 */

/* Write a big-endian uint16 to a buffer */
static void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* Write a big-endian uint32 to a buffer */
static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* Calculate OpenType table checksum */
static uint32_t otf_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t nLongs = (len + 3) / 4;
    for (size_t i = 0; i < nLongs; i++) {
        uint32_t val = 0;
        size_t off = i * 4;
        for (int j = 0; j < 4; j++) {
            val <<= 8;
            if (off + j < len)
                val |= data[off + j];
        }
        sum += val;
    }
    return sum;
}

/*
 * Wrap bare CFF data in a minimal OpenType container.
 * Returns a malloc'd buffer containing the OTF file, and sets *out_len.
 * Caller must free() the returned buffer.
 * Returns NULL on failure.
 */
static uint8_t *wrap_cff_in_otf(const uint8_t *cff_data, size_t cff_len,
                                  const char *font_name, size_t *out_len)
{
    if (!cff_data || cff_len == 0 || !font_name || !out_len)
        return NULL;

    *out_len = 0;
    size_t name_len_bytes = strlen(font_name);
    if (name_len_bytes > 127) name_len_bytes = 127;

    /* We need 8 tables: CFF, OS/2, cmap, head, hhea, maxp, name, post */
    const int num_tables = 8;

    /* 1. head table (54 bytes) */
    uint8_t head_tbl[54];
    memset(head_tbl, 0, sizeof(head_tbl));
    write_u32_be(head_tbl + 0, 0x00010000);
    write_u32_be(head_tbl + 4, 0x00005000);
    write_u32_be(head_tbl + 12, 0x5F0F3CF5);  /* magicNumber */
    write_u16_be(head_tbl + 16, 0x000B);
    write_u16_be(head_tbl + 18, 1000);         /* unitsPerEm */
    write_u16_be(head_tbl + 40, 1000);         /* xMax */
    write_u16_be(head_tbl + 42, 1000);         /* yMax */
    write_u16_be(head_tbl + 46, 8);
    write_u16_be(head_tbl + 48, 2);
    write_u16_be(head_tbl + 50, 1);

    /* 2. hhea table (36 bytes) */
    uint8_t hhea_tbl[36];
    memset(hhea_tbl, 0, sizeof(hhea_tbl));
    write_u32_be(hhea_tbl + 0, 0x00010000);
    write_u16_be(hhea_tbl + 4, 800);
    write_u16_be(hhea_tbl + 6, (uint16_t)(int16_t)-200);
    write_u16_be(hhea_tbl + 10, 1000);
    write_u16_be(hhea_tbl + 34, 1);

    /* 3. maxp table (6 bytes for CFF) */
    uint8_t maxp_tbl[6];
    write_u32_be(maxp_tbl + 0, 0x00005000);
    write_u16_be(maxp_tbl + 4, 1);

    /* 4. OS/2 table (78 bytes) */
    uint8_t os2_tbl[78];
    memset(os2_tbl, 0, sizeof(os2_tbl));
    write_u16_be(os2_tbl + 0, 1);
    write_u16_be(os2_tbl + 2, 500);
    write_u16_be(os2_tbl + 4, 400);
    write_u16_be(os2_tbl + 6, 5);
    write_u32_be(os2_tbl + 42, 0x00000001);
    os2_tbl[58] = 'A'; os2_tbl[59] = 'M'; os2_tbl[60] = 'P'; os2_tbl[61] = 'D';
    write_u16_be(os2_tbl + 62, 0x0040);
    write_u16_be(os2_tbl + 64, 0x0020);
    write_u16_be(os2_tbl + 66, 0x00FF);
    write_u16_be(os2_tbl + 68, 800);
    write_u16_be(os2_tbl + 70, (uint16_t)(int16_t)-200);
    write_u16_be(os2_tbl + 74, 800);
    write_u16_be(os2_tbl + 76, 200);

    /* 5. name table: nameID 1 (family), 2 (subfamily), 4 (full name) */
    int name_rec_count = 3;
    size_t name_str_u16_len = name_len_bytes * 2;
    size_t sub_str_u16_len = 14;  /* "Regular" = 7 * 2 */
    size_t name_storage = name_str_u16_len * 2 + sub_str_u16_len;
    size_t name_tbl_size = 6 + name_rec_count * 12 + name_storage;
    size_t name_tbl_padded = (name_tbl_size + 3) & ~3;
    uint8_t *name_tbl = (uint8_t *)calloc(1, name_tbl_padded);
    if (!name_tbl) return NULL;

    write_u16_be(name_tbl + 0, 0);
    write_u16_be(name_tbl + 2, name_rec_count);
    uint16_t str_off = (uint16_t)(6 + name_rec_count * 12);
    write_u16_be(name_tbl + 4, str_off);

    /* Record 0: Family (nameID 1) */
    uint8_t *nr = name_tbl + 6;
    write_u16_be(nr, 3); write_u16_be(nr+2, 1); write_u16_be(nr+4, 0x0409);
    write_u16_be(nr+6, 1); write_u16_be(nr+8, (uint16_t)name_str_u16_len); write_u16_be(nr+10, 0);

    /* Record 1: Subfamily (nameID 2) */
    nr += 12;
    write_u16_be(nr, 3); write_u16_be(nr+2, 1); write_u16_be(nr+4, 0x0409);
    write_u16_be(nr+6, 2); write_u16_be(nr+8, (uint16_t)sub_str_u16_len);
    write_u16_be(nr+10, (uint16_t)name_str_u16_len);

    /* Record 2: Full Name (nameID 4) */
    nr += 12;
    write_u16_be(nr, 3); write_u16_be(nr+2, 1); write_u16_be(nr+4, 0x0409);
    write_u16_be(nr+6, 4); write_u16_be(nr+8, (uint16_t)name_str_u16_len);
    write_u16_be(nr+10, (uint16_t)(name_str_u16_len + sub_str_u16_len));

    /* String storage (UTF-16BE) */
    uint8_t *ss = name_tbl + str_off;
    for (size_t i = 0; i < name_len_bytes; i++) {
        ss[i*2] = 0; ss[i*2+1] = (uint8_t)font_name[i];
    }
    ss += name_str_u16_len;
    const char *reg = "Regular";
    for (int i = 0; i < 7; i++) { ss[i*2] = 0; ss[i*2+1] = (uint8_t)reg[i]; }
    ss += sub_str_u16_len;
    for (size_t i = 0; i < name_len_bytes; i++) {
        ss[i*2] = 0; ss[i*2+1] = (uint8_t)font_name[i];
    }

    /* 6. cmap table (minimal format 4) */
    uint8_t cmap_tbl[30];
    memset(cmap_tbl, 0, sizeof(cmap_tbl));
    write_u16_be(cmap_tbl + 2, 1);
    write_u16_be(cmap_tbl + 4, 3); write_u16_be(cmap_tbl + 6, 1);
    write_u32_be(cmap_tbl + 8, 12);
    write_u16_be(cmap_tbl + 12, 4); write_u16_be(cmap_tbl + 14, 18);
    write_u16_be(cmap_tbl + 18, 4); write_u16_be(cmap_tbl + 20, 4);
    write_u16_be(cmap_tbl + 22, 1);
    write_u16_be(cmap_tbl + 26, 0xFFFF);

    /* 7. post table (32 bytes, format 3.0) */
    uint8_t post_tbl[32];
    memset(post_tbl, 0, sizeof(post_tbl));
    write_u32_be(post_tbl + 0, 0x00030000);
    write_u16_be(post_tbl + 8, (uint16_t)(int16_t)-100);
    write_u16_be(post_tbl + 10, 50);

    /* 8. CFF table = raw CFF data */
    size_t cff_padded = (cff_len + 3) & ~3;

    /* Assemble the OTF */
    size_t hdr_size = 12 + num_tables * 16;
    size_t head_pad = (sizeof(head_tbl) + 3) & ~3;
    size_t hhea_pad = (sizeof(hhea_tbl) + 3) & ~3;
    size_t maxp_pad = (sizeof(maxp_tbl) + 3) & ~3;
    size_t os2_pad  = (sizeof(os2_tbl)  + 3) & ~3;
    size_t cmap_pad = (sizeof(cmap_tbl) + 3) & ~3;
    size_t post_pad = (sizeof(post_tbl) + 3) & ~3;

    size_t total = hdr_size + cff_padded + os2_pad + cmap_pad + head_pad +
                   hhea_pad + maxp_pad + name_tbl_padded + post_pad;

    uint8_t *otf = (uint8_t *)calloc(1, total);
    if (!otf) { free(name_tbl); return NULL; }

    /* OTF header */
    write_u32_be(otf, 0x4F54544F);  /* 'OTTO' */
    write_u16_be(otf + 4, num_tables);
    int sr = 1, es = 0;
    while (sr * 2 <= num_tables) { sr *= 2; es++; }
    write_u16_be(otf + 6, sr * 16);
    write_u16_be(otf + 8, es);
    write_u16_be(otf + 10, num_tables * 16 - sr * 16);

    /* Table directory (sorted by tag for spec compliance) */
    struct { uint32_t tag; const uint8_t *data; size_t len; size_t pad; } tbls[8] = {
        { 0x43464620, cff_data,   cff_len,         cff_padded },        /* CFF  */
        { 0x4F532F32, os2_tbl,    sizeof(os2_tbl),  os2_pad },          /* OS/2 */
        { 0x636D6170, cmap_tbl,   sizeof(cmap_tbl), cmap_pad },         /* cmap */
        { 0x68656164, head_tbl,   sizeof(head_tbl), head_pad },         /* head */
        { 0x68686561, hhea_tbl,   sizeof(hhea_tbl), hhea_pad },         /* hhea */
        { 0x6D617870, maxp_tbl,   sizeof(maxp_tbl), maxp_pad },         /* maxp */
        { 0x6E616D65, name_tbl,   name_tbl_size,    name_tbl_padded },  /* name */
        { 0x706F7374, post_tbl,   sizeof(post_tbl), post_pad },         /* post */
    };

    size_t offset = hdr_size;
    for (int i = 0; i < num_tables; i++) {
        uint8_t *rp = otf + 12 + i * 16;
        write_u32_be(rp, tbls[i].tag);
        write_u32_be(rp + 4, otf_checksum(tbls[i].data, tbls[i].len));
        write_u32_be(rp + 8, (uint32_t)offset);
        write_u32_be(rp + 12, (uint32_t)tbls[i].len);
        memcpy(otf + offset, tbls[i].data, tbls[i].len);
        offset += tbls[i].pad;
    }

    free(name_tbl);
    *out_len = total;
    return otf;
}

/*
 * Get the BaseFont name from the font resource dictionary.
 * Looks through both simple fonts and Type0 DescendantFonts.
 */
static const char *get_base_font_name(PdfDocument *doc, PdfDict *resources,
                                        const char *font_res_name)
{
    PdfObj *fonts_obj = pdf_dict_get(resources, "Font");
    if (!fonts_obj) return NULL;
    fonts_obj = pdf_resolve(doc, fonts_obj);
    if (!fonts_obj || fonts_obj->type != PDF_OBJ_DICT) return NULL;

    PdfObj *font_obj = pdf_dict_get(fonts_obj->dict, font_res_name);
    if (!font_obj) return NULL;
    font_obj = pdf_resolve(doc, font_obj);
    if (!font_obj || font_obj->type != PDF_OBJ_DICT) return NULL;

    return pdf_dict_get_name(font_obj->dict, "BaseFont");
}

bool pdf_font_try_load_embedded(PdfDocument *doc, PdfDict *resources,
                                 const char *font_res_name,
                                 char *out_family_name, int name_len)
{
    if (!doc || !resources || !font_res_name || !out_family_name || name_len < 2)
        return false;

    out_family_name[0] = '\0';

    /* Find the font stream */
    PdfObj *stream_obj = NULL;
    const char *font_file_key = NULL;
    bool is_type1c = false;
    int obj_num = get_font_stream_obj_num(doc, resources, font_res_name,
                                           &stream_obj, &font_file_key, &is_type1c);
    if (obj_num < 0 || !stream_obj)
        return false;

    /* Check cache */
    int cache_idx = embedded_font_find(obj_num);
    if (cache_idx >= 0) {
        EmbeddedFontEntry *entry = &g_embedded_fonts[cache_idx];
        if (entry->tried) {
            if (entry->loaded) {
                strncpy(out_family_name, entry->family_name, name_len - 1);
                out_family_name[name_len - 1] = '\0';
                return true;
            }
            return false;
        }
    }

    /* Allocate a cache slot */
    if (cache_idx < 0) {
        if (g_embedded_font_count >= MAX_EMBEDDED_FONTS)
            return false;
        cache_idx = g_embedded_font_count++;
        g_embedded_fonts[cache_idx].obj_num = obj_num;
        g_embedded_fonts[cache_idx].hMemFont = NULL;
        g_embedded_fonts[cache_idx].family_name[0] = '\0';
        g_embedded_fonts[cache_idx].loaded = false;
        g_embedded_fonts[cache_idx].tried = false;
    }

    EmbeddedFontEntry *entry = &g_embedded_fonts[cache_idx];
    entry->tried = true;

    /* Decode the stream */
    PdfStream *stream = stream_obj->stream;
    if (!pdf_decode_stream(doc, stream))
        return false;

    const uint8_t *font_data = stream->decoded_data;
    size_t font_data_len = stream->decoded_length;
    if (!font_data || font_data_len < 4)
        return false;

    /* Determine font name.
     * TrueType/OpenType: parse the name table.
     * Type1C (bare CFF): use BaseFont from PDF. */
    char family_name[256];
    family_name[0] = '\0';

    if (!is_type1c) {
        ttf_get_family_name(font_data, font_data_len, family_name, sizeof(family_name));
    }

    /* Fallback (or Type1C): use BaseFont from PDF, strip subset prefix */
    if (family_name[0] == '\0') {
        const char *base_font = get_base_font_name(doc, resources, font_res_name);
        if (base_font) {
            const char *stripped = strip_subset_prefix(base_font);
            strncpy(family_name, stripped, sizeof(family_name) - 1);
            family_name[sizeof(family_name) - 1] = '\0';
        }
    }

    if (family_name[0] == '\0')
        return false;

    /* For Type1C, wrap CFF data in an OpenType container */
    uint8_t *load_data = (uint8_t *)font_data;
    size_t load_data_len = font_data_len;
    uint8_t *otf_wrapper = NULL;

    if (is_type1c) {
        size_t otf_len = 0;
        otf_wrapper = wrap_cff_in_otf(font_data, font_data_len, family_name, &otf_len);
        if (!otf_wrapper)
            return false;
        load_data = otf_wrapper;
        load_data_len = otf_len;
    }

    /* Load the font into GDI memory */
    DWORD num_fonts = 0;
    HANDLE hFont = AddFontMemResourceEx((void *)load_data, (DWORD)load_data_len,
                                         NULL, &num_fonts);
    if (otf_wrapper) free(otf_wrapper);

    if (!hFont || num_fonts == 0)
        return false;

    /* Cache the result */
    entry->hMemFont = hFont;
    strncpy(entry->family_name, family_name, sizeof(entry->family_name) - 1);
    entry->family_name[sizeof(entry->family_name) - 1] = '\0';
    entry->loaded = true;

    /* Output the family name */
    strncpy(out_family_name, family_name, name_len - 1);
    out_family_name[name_len - 1] = '\0';

    return true;
}

void pdf_fonts_cleanup(void)
{
    /* Remove all loaded embedded fonts from GDI */
    for (int i = 0; i < g_embedded_font_count; i++) {
        if (g_embedded_fonts[i].loaded && g_embedded_fonts[i].hMemFont) {
            RemoveFontMemResourceEx(g_embedded_fonts[i].hMemFont);
            g_embedded_fonts[i].hMemFont = NULL;
            g_embedded_fonts[i].loaded = false;
        }
    }
    g_embedded_font_count = 0;
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
