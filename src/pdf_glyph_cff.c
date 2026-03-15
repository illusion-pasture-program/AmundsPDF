/*
 * AmundsPDF - pdf_glyph_cff.c
 * CFF (Compact Font Format) parser and Type 2 charstring interpreter.
 *
 * Parses Type1C font data (from /FontFile3 /Subtype /Type1C) and extracts
 * glyph outlines as cubic bezier paths for rendering via GDI filled paths.
 *
 * References:
 *   - Adobe Technical Note #5176: "The Compact Font Format Specification"
 *   - Adobe Technical Note #5177: "The Type 2 Charstring Format"
 */

#include "pdf_glyph.h"

/* ─── Constants ─── */
#define CFF_MAX_STACK       48    /* Type 2 operand stack limit */
#define CFF_MAX_CALL_DEPTH  10    /* Maximum subroutine nesting depth */
#define CFF_MAX_STEMS       96    /* Maximum number of stem hints */

/* ─── Standard Encodings ─── */

/*
 * Adobe Standard Encoding: maps glyph index -> character code.
 * This is the default encoding for CFF fonts that don't specify one.
 * Only the non-zero entries are listed; all others map to .notdef (0).
 */
static const int standard_encoding[] = {
    /* GID -> char code mapping (only populated entries) */
    /* This maps SID to character code for the Standard Encoding */
      0,   0,   0,   0,   0,   0,   0,   0,   /*  0-7   */
      0,   0,   0,   0,   0,   0,   0,   0,   /*  8-15  */
      0,   0,   0,   0,   0,   0,   0,   0,   /* 16-23  */
      0,   0,   0,   0,   0,   0,   0,   0,   /* 24-31  */
     32,  33,  34,  35,  36,  37,  38,  39,   /* 32-39: space ! " # $ % & ' */
     40,  41,  42,  43,  44,  45,  46,  47,   /* 40-47: ( ) * + , - . / */
     48,  49,  50,  51,  52,  53,  54,  55,   /* 48-55: 0 1 2 3 4 5 6 7 */
     56,  57,  58,  59,  60,  61,  62,  63,   /* 56-63: 8 9 : ; < = > ? */
     64,  65,  66,  67,  68,  69,  70,  71,   /* 64-71: @ A B C D E F G */
     72,  73,  74,  75,  76,  77,  78,  79,   /* 72-79: H I J K L M N O */
     80,  81,  82,  83,  84,  85,  86,  87,   /* 80-87: P Q R S T U V W */
     88,  89,  90,  91,  92,  93,  94,  95,   /* 88-95: X Y Z [ \ ] ^ _ */
     96,  97,  98,  99, 100, 101, 102, 103,   /* 96-103: ` a b c d e f g */
    104, 105, 106, 107, 108, 109, 110, 111,   /* 104-111: h i j k l m n o */
    112, 113, 114, 115, 116, 117, 118, 119,   /* 112-119: p q r s t u v w */
    120, 121, 122, 123, 124, 125, 126,   0,   /* 120-127: x y z { | } ~ */
      0,   0,   0,   0,   0,   0,   0,   0,   /* 128-135 */
      0,   0,   0,   0,   0,   0,   0,   0,   /* 136-143 */
      0,   0,   0,   0,   0,   0,   0,   0,   /* 144-151 */
      0,   0,   0,   0,   0,   0,   0,   0,   /* 152-159 */
      0, 161, 162, 163, 164, 165, 166, 167,   /* 160-167 */
    168, 169, 170, 171, 172, 173, 174, 175,   /* 168-175 */
    176, 177, 178, 179, 180, 181, 182, 183,   /* 176-183 */
    184, 185, 186, 187, 188, 189, 190, 191,   /* 184-191 */
    192, 193, 194, 195, 196, 197, 198, 199,   /* 192-199 */
    200, 201, 202, 203, 204, 205, 206, 207,   /* 200-207 */
    208, 209, 210, 211, 212, 213, 214, 215,   /* 208-215 */
    216, 217, 218, 219, 220, 221, 222, 223,   /* 216-223 */
    224, 225, 226, 227, 228, 229, 230, 231,   /* 224-231 */
    232, 233, 234, 235, 236, 237, 238, 239,   /* 232-239 */
    240, 241, 242, 243, 244, 245, 246, 247,   /* 240-247 */
    248, 249, 250, 251, 252, 253, 254, 255,   /* 248-255 */
};

/* Number of entries in the standard encoding table */
#define STANDARD_ENCODING_SIZE 256

/*
 * Standard Encoding reverse map: char_code -> GID.
 * For Standard Encoding, GID == char_code for the printable range.
 * We build this identity-ish mapping in cff_apply_standard_encoding().
 */

/* ─── Safe Read Helpers ─── */

/* Read 1-4 bytes big-endian unsigned from data, with bounds checking. */
static inline uint32_t cff_read_uint(const uint8_t *data, size_t offset,
                                     int nbytes, size_t data_len)
{
    uint32_t val = 0;
    for (int i = 0; i < nbytes; i++) {
        if (offset + (size_t)i >= data_len) return 0;
        val = (val << 8) | data[offset + i];
    }
    return val;
}

static inline uint8_t cff_u8(const uint8_t *data, size_t off, size_t len) {
    return (off < len) ? data[off] : 0;
}

static inline uint16_t cff_u16(const uint8_t *data, size_t off, size_t len) {
    if (off + 1 >= len) return 0;
    return (uint16_t)((data[off] << 8) | data[off + 1]);
}

static inline int16_t cff_s16(const uint8_t *data, size_t off, size_t len) {
    return (int16_t)cff_u16(data, off, len);
}

static inline uint32_t cff_u32(const uint8_t *data, size_t off, size_t len) {
    if (off + 3 >= len) return 0;
    return ((uint32_t)data[off] << 24) | ((uint32_t)data[off+1] << 16) |
           ((uint32_t)data[off+2] << 8) | data[off+3];
}

/* ─── INDEX Structure ─── */

/*
 * CFF INDEX structure:
 *   count   (2 bytes)    - number of objects
 *   offSize (1 byte)     - offset byte size (1-4)
 *   offset  [(count+1) * offSize bytes] - offsets (1-based)
 *   data    [variable]   - the actual data
 *
 * offset[0] is always 1. The data for element i spans
 * from (data_start + offset[i] - 1) to (data_start + offset[i+1] - 1).
 */
typedef struct {
    int          count;       /* number of elements */
    int          off_size;    /* offset byte size (1-4) */
    const uint8_t *offsets;   /* pointer to offset array */
    const uint8_t *data_start; /* pointer to start of data region */
    size_t       total_size;  /* total bytes consumed by this INDEX */
} CffIndex;

/*
 * Parse a CFF INDEX structure starting at the given offset.
 * Returns true on success, fills out the CffIndex struct.
 */
static bool cff_parse_index(const uint8_t *cff, size_t offset, size_t cff_len,
                            CffIndex *idx)
{
    memset(idx, 0, sizeof(*idx));

    /* Need at least 2 bytes for count */
    if (offset + 2 > cff_len) return false;

    idx->count = cff_u16(cff, offset, cff_len);
    if (idx->count == 0) {
        /* Empty INDEX: just the 2-byte count */
        idx->total_size = 2;
        return true;
    }

    /* Read offSize */
    if (offset + 3 > cff_len) return false;
    idx->off_size = cff_u8(cff, offset + 2, cff_len);
    if (idx->off_size < 1 || idx->off_size > 4) return false;

    /* Offsets array starts at offset+3, has (count+1) entries */
    size_t offsets_start = offset + 3;
    size_t offsets_bytes = (size_t)(idx->count + 1) * idx->off_size;
    if (offsets_start + offsets_bytes > cff_len) return false;

    idx->offsets = cff + offsets_start;

    /* Data starts right after the offsets */
    size_t data_offset = offsets_start + offsets_bytes;
    idx->data_start = cff + data_offset;

    /* Last offset tells us the total data size */
    uint32_t last_off = cff_read_uint(cff, offsets_start + (size_t)idx->count * idx->off_size,
                                      idx->off_size, cff_len);
    /* Offsets are 1-based, so data size = last_off - 1 */
    size_t data_size = (last_off > 0) ? (last_off - 1) : 0;

    idx->total_size = 3 + offsets_bytes + data_size;

    /* Bounds check */
    if (offset + idx->total_size > cff_len) return false;

    return true;
}

/*
 * Get a pointer to element `i` within a parsed INDEX, and its length.
 * Returns NULL if index is out of range.
 */
static const uint8_t *cff_index_get(const CffIndex *idx, int i,
                                    size_t *out_len)
{
    if (i < 0 || i >= idx->count || !idx->offsets || !idx->data_start) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Read offset[i] and offset[i+1] */
    size_t off_a_pos = (size_t)i * idx->off_size;
    size_t off_b_pos = (size_t)(i + 1) * idx->off_size;

    uint32_t off_a = 0, off_b = 0;
    for (int b = 0; b < idx->off_size; b++) {
        off_a = (off_a << 8) | idx->offsets[off_a_pos + b];
        off_b = (off_b << 8) | idx->offsets[off_b_pos + b];
    }

    /* Offsets are 1-based */
    if (off_a < 1 || off_b < off_a) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t start = off_a - 1;
    size_t end = off_b - 1;
    if (out_len) *out_len = end - start;
    return idx->data_start + start;
}

/* ─── DICT Parsing ─── */

/*
 * CFF DICT is a sequence of operands followed by an operator.
 * Operands are encoded as numbers (integers or reals).
 * Operators are 1 or 2 bytes (2-byte operators start with 12).
 *
 * We parse the DICT looking for specific operators and extract their operands.
 */

/* DICT operator IDs (single byte or 12 xx for two-byte) */
#define DICT_OP_CHARSET          15
#define DICT_OP_ENCODING         16
#define DICT_OP_CHARSTRINGS      17
#define DICT_OP_PRIVATE          18  /* size offset (2 operands) */
#define DICT_OP_SUBRS            19  /* local subr offset (within Private DICT) */
#define DICT_OP_DEFAULTWIDTHX    20  /* in Private DICT */
#define DICT_OP_NOMINALWIDTHX    21  /* in Private DICT */

/* Two-byte operator encoding: 12 xx -> 1200 + xx */
#define DICT_OP_2BYTE(x)  (1200 + (x))

/* 12 0 = version, 12 1 = Notice, etc. Not all are needed. */
#define DICT_OP_IS_REAL_FONT     DICT_OP_2BYTE(36)  /* isCIDFont */

/* Structure to hold DICT parse results */
typedef struct {
    int     charset_offset;     /* default 0 = ISOAdobe */
    int     encoding_offset;    /* default 0 = Standard */
    int     charstrings_offset;
    int     private_size;
    int     private_offset;
    int     local_subr_offset;  /* relative to start of Private DICT */
    double  default_width_x;
    double  nominal_width_x;
} CffDictInfo;

/*
 * Parse a single number from a CFF DICT byte stream.
 * Returns the number of bytes consumed, or 0 on error.
 */
static int cff_dict_read_number(const uint8_t *data, size_t offset,
                                size_t len, double *value)
{
    if (offset >= len) return 0;
    uint8_t b0 = data[offset];

    if (b0 == 28) {
        /* 2-byte signed integer */
        if (offset + 2 >= len) return 0;
        int16_t v = (int16_t)((data[offset + 1] << 8) | data[offset + 2]);
        *value = (double)v;
        return 3;
    }
    else if (b0 == 29) {
        /* 4-byte signed integer */
        if (offset + 4 >= len) return 0;
        int32_t v = (int32_t)((data[offset+1] << 24) | (data[offset+2] << 16) |
                              (data[offset+3] << 8) | data[offset+4]);
        *value = (double)v;
        return 5;
    }
    else if (b0 == 30) {
        /* Real number (BCD encoded nibbles) */
        /* Format: pairs of nibbles, terminated by 0xf nibble */
        char buf[64];
        int buf_pos = 0;
        int pos = (int)offset + 1;
        bool done = false;

        while (!done && pos < (int)len && buf_pos < 60) {
            uint8_t byte = data[pos++];
            for (int nib_idx = 0; nib_idx < 2 && !done; nib_idx++) {
                int nib = (nib_idx == 0) ? (byte >> 4) : (byte & 0x0f);
                switch (nib) {
                    case 0: case 1: case 2: case 3: case 4:
                    case 5: case 6: case 7: case 8: case 9:
                        buf[buf_pos++] = '0' + nib;
                        break;
                    case 0xa: /* decimal point */
                        buf[buf_pos++] = '.';
                        break;
                    case 0xb: /* positive exponent */
                        buf[buf_pos++] = 'e';
                        break;
                    case 0xc: /* negative exponent */
                        buf[buf_pos++] = 'e';
                        buf[buf_pos++] = '-';
                        break;
                    case 0xd: /* reserved */
                        break;
                    case 0xe: /* minus sign */
                        buf[buf_pos++] = '-';
                        break;
                    case 0xf: /* end of number */
                        done = true;
                        break;
                }
            }
        }
        buf[buf_pos] = '\0';
        *value = atof(buf);
        return pos - (int)offset;
    }
    else if (b0 >= 32 && b0 <= 246) {
        *value = (double)(b0 - 139);
        return 1;
    }
    else if (b0 >= 247 && b0 <= 250) {
        if (offset + 1 >= len) return 0;
        *value = (double)((b0 - 247) * 256 + data[offset + 1] + 108);
        return 2;
    }
    else if (b0 >= 251 && b0 <= 254) {
        if (offset + 1 >= len) return 0;
        *value = (double)(-(b0 - 251) * 256 - data[offset + 1] - 108);
        return 2;
    }

    return 0; /* not a number */
}

/*
 * Parse a CFF DICT and extract the fields we care about.
 * `is_private` controls whether we look for Private DICT operators.
 */
static void cff_parse_dict(const uint8_t *data, size_t offset, size_t len,
                           CffDictInfo *info, bool is_private)
{
    double operands[48];
    int num_operands = 0;
    size_t pos = offset;
    size_t end = offset + len;

    while (pos < end) {
        uint8_t b0 = data[pos];

        /* Check if this is an operator or an operand */
        if (b0 <= 21) {
            /* Operator */
            int op;
            if (b0 == 12) {
                /* Two-byte operator */
                pos++;
                if (pos >= end) break;
                op = 1200 + data[pos];
            } else {
                op = b0;
            }
            pos++;

            /* Process operator with accumulated operands */
            if (!is_private) {
                switch (op) {
                    case DICT_OP_CHARSET:
                        if (num_operands >= 1)
                            info->charset_offset = (int)operands[0];
                        break;
                    case DICT_OP_ENCODING:
                        if (num_operands >= 1)
                            info->encoding_offset = (int)operands[0];
                        break;
                    case DICT_OP_CHARSTRINGS:
                        if (num_operands >= 1)
                            info->charstrings_offset = (int)operands[0];
                        break;
                    case DICT_OP_PRIVATE:
                        if (num_operands >= 2) {
                            info->private_size = (int)operands[0];
                            info->private_offset = (int)operands[1];
                        }
                        break;
                    default:
                        break;
                }
            } else {
                /* Private DICT operators */
                switch (op) {
                    case DICT_OP_SUBRS:
                        if (num_operands >= 1)
                            info->local_subr_offset = (int)operands[0];
                        break;
                    case DICT_OP_DEFAULTWIDTHX:
                        if (num_operands >= 1)
                            info->default_width_x = operands[0];
                        break;
                    case DICT_OP_NOMINALWIDTHX:
                        if (num_operands >= 1)
                            info->nominal_width_x = operands[0];
                        break;
                    default:
                        break;
                }
            }

            num_operands = 0;
        }
        else if (b0 == 28 || b0 == 29 || b0 == 30 ||
                 (b0 >= 32 && b0 <= 254)) {
            /* Operand */
            double val = 0;
            int consumed = cff_dict_read_number(data, pos, end, &val);
            if (consumed <= 0) {
                pos++;
                continue;
            }
            if (num_operands < 48) {
                operands[num_operands++] = val;
            }
            pos += consumed;
        }
        else {
            /* Unknown byte, skip */
            pos++;
        }
    }
}

/* ─── Subroutine Bias Calculation ─── */

/*
 * Calculate the subroutine index bias per the Type 2 spec.
 * This bias is added to the subroutine number from the stack to get
 * the actual index into the subr INDEX.
 */
static int cff_subr_bias(int count) {
    if (count < 1240) return 107;
    if (count < 33900) return 1131;
    return 32768;
}

/* ─── Encoding Parsing ─── */

/*
 * Apply the CFF Standard Encoding.
 * In Standard Encoding, the char code maps almost 1:1 to GID
 * for the first 256 glyphs.
 */
static void cff_apply_standard_encoding(ParsedFont *font) {
    /*
     * Standard Encoding: for most fonts, GID 1 corresponds to char code
     * of the glyph name at SID 1 in the Standard Encoding.
     * For simplicity, we use an identity mapping for codes 0-255,
     * capped at the number of glyphs. This works for most Type1C fonts.
     */
    for (int i = 0; i < 256; i++) {
        if (i < font->num_glyphs)
            font->encoding[i] = i;
        else
            font->encoding[i] = 0;
    }
}

/*
 * Apply the CFF Expert Encoding.
 * Maps char codes to GIDs for the Expert character set.
 * We use identity as a fallback since Expert encoding is rare.
 */
static void cff_apply_expert_encoding(ParsedFont *font) {
    /* Expert encoding: use identity as practical approximation */
    for (int i = 0; i < 256; i++) {
        if (i < font->num_glyphs)
            font->encoding[i] = i;
        else
            font->encoding[i] = 0;
    }
}

/*
 * Parse a custom CFF encoding from the font data.
 * Supports Format 0 and Format 1 encodings.
 */
static void cff_parse_custom_encoding(ParsedFont *font, const uint8_t *cff,
                                      size_t enc_offset, size_t cff_len)
{
    /* Initialize all codes to .notdef */
    for (int i = 0; i < 256; i++)
        font->encoding[i] = 0;

    if (enc_offset >= cff_len) return;

    uint8_t format = cff_u8(cff, enc_offset, cff_len);
    /* Low 7 bits are the format; bit 7 indicates supplemental encoding */
    uint8_t fmt = format & 0x7f;

    if (fmt == 0) {
        /* Format 0: array of codes */
        size_t pos = enc_offset + 1;
        if (pos >= cff_len) return;
        int n_codes = cff_u8(cff, pos, cff_len);
        pos++;

        for (int i = 0; i < n_codes && pos < cff_len; i++) {
            uint8_t code = cff_u8(cff, pos, cff_len);
            pos++;
            /* GID i+1 gets char code `code` */
            if (code < 256 && (i + 1) < font->num_glyphs) {
                font->encoding[code] = i + 1;
            }
        }
    }
    else if (fmt == 1) {
        /* Format 1: ranges */
        size_t pos = enc_offset + 1;
        if (pos >= cff_len) return;
        int n_ranges = cff_u8(cff, pos, cff_len);
        pos++;

        int gid = 1; /* GID 0 is always .notdef */
        for (int i = 0; i < n_ranges && pos + 1 < cff_len; i++) {
            uint8_t first = cff_u8(cff, pos, cff_len);
            uint8_t n_left = cff_u8(cff, pos + 1, cff_len);
            pos += 2;

            for (int j = 0; j <= n_left; j++) {
                int code = first + j;
                if (code < 256 && gid < font->num_glyphs) {
                    font->encoding[code] = gid;
                }
                gid++;
            }
        }
    }

    /* Handle supplemental encoding (bit 7 set) */
    if (format & 0x80) {
        /* Find position after the main encoding */
        size_t pos = enc_offset + 1;
        if (fmt == 0) {
            int n_codes = cff_u8(cff, pos, cff_len);
            pos += 1 + n_codes;
        } else if (fmt == 1) {
            int n_ranges = cff_u8(cff, pos, cff_len);
            pos += 1 + n_ranges * 2;
        }

        if (pos < cff_len) {
            int n_sups = cff_u8(cff, pos, cff_len);
            pos++;
            for (int i = 0; i < n_sups && pos + 2 < cff_len; i++) {
                uint8_t code = cff_u8(cff, pos, cff_len);
                uint16_t sid = cff_u16(cff, pos + 1, cff_len);
                pos += 3;
                /* Map code to GID - for supplemental, SID is the glyph SID.
                 * We approximate: if SID < num_glyphs, use SID as GID */
                if (code < 256 && sid < (uint16_t)font->num_glyphs) {
                    font->encoding[code] = sid;
                }
            }
        }
    }
}

/* ─── CFF Standard Strings (SID 0..390) ─── */

/*
 * The CFF specification defines 391 standard strings.
 * SID 0 = ".notdef", SID 1 = "space", etc.
 * SIDs >= 391 index into the font's String INDEX at (sid - 391).
 */
static const char *cff_standard_strings[] = {
    ".notdef",           /* 0 */
    "space",             /* 1 */
    "exclam",            /* 2 */
    "quotedbl",          /* 3 */
    "numbersign",        /* 4 */
    "dollar",            /* 5 */
    "percent",           /* 6 */
    "ampersand",         /* 7 */
    "quoteright",        /* 8 */
    "parenleft",         /* 9 */
    "parenright",        /* 10 */
    "asterisk",          /* 11 */
    "plus",              /* 12 */
    "comma",             /* 13 */
    "hyphen",            /* 14 */
    "period",            /* 15 */
    "slash",             /* 16 */
    "zero",              /* 17 */
    "one",               /* 18 */
    "two",               /* 19 */
    "three",             /* 20 */
    "four",              /* 21 */
    "five",              /* 22 */
    "six",               /* 23 */
    "seven",             /* 24 */
    "eight",             /* 25 */
    "nine",              /* 26 */
    "colon",             /* 27 */
    "semicolon",         /* 28 */
    "less",              /* 29 */
    "equal",             /* 30 */
    "greater",           /* 31 */
    "question",          /* 32 */
    "at",                /* 33 */
    "A",                 /* 34 */
    "B",                 /* 35 */
    "C",                 /* 36 */
    "D",                 /* 37 */
    "E",                 /* 38 */
    "F",                 /* 39 */
    "G",                 /* 40 */
    "H",                 /* 41 */
    "I",                 /* 42 */
    "J",                 /* 43 */
    "K",                 /* 44 */
    "L",                 /* 45 */
    "M",                 /* 46 */
    "N",                 /* 47 */
    "O",                 /* 48 */
    "P",                 /* 49 */
    "Q",                 /* 50 */
    "R",                 /* 51 */
    "S",                 /* 52 */
    "T",                 /* 53 */
    "U",                 /* 54 */
    "V",                 /* 55 */
    "W",                 /* 56 */
    "X",                 /* 57 */
    "Y",                 /* 58 */
    "Z",                 /* 59 */
    "bracketleft",       /* 60 */
    "backslash",         /* 61 */
    "bracketright",      /* 62 */
    "asciicircum",       /* 63 */
    "underscore",        /* 64 */
    "quoteleft",         /* 65 */
    "a",                 /* 66 */
    "b",                 /* 67 */
    "c",                 /* 68 */
    "d",                 /* 69 */
    "e",                 /* 70 */
    "f",                 /* 71 */
    "g",                 /* 72 */
    "h",                 /* 73 */
    "i",                 /* 74 */
    "j",                 /* 75 */
    "k",                 /* 76 */
    "l",                 /* 77 */
    "m",                 /* 78 */
    "n",                 /* 79 */
    "o",                 /* 80 */
    "p",                 /* 81 */
    "q",                 /* 82 */
    "r",                 /* 83 */
    "s",                 /* 84 */
    "t",                 /* 85 */
    "u",                 /* 86 */
    "v",                 /* 87 */
    "w",                 /* 88 */
    "x",                 /* 89 */
    "y",                 /* 90 */
    "z",                 /* 91 */
    "braceleft",         /* 92 */
    "bar",               /* 93 */
    "braceright",        /* 94 */
    "asciitilde",        /* 95 */
    "exclamdown",        /* 96 */
    "cent",              /* 97 */
    "sterling",          /* 98 */
    "fraction",          /* 99 */
    "yen",               /* 100 */
    "florin",            /* 101 */
    "section",           /* 102 */
    "currency",          /* 103 */
    "quotesingle",       /* 104 */
    "quotedblleft",      /* 105 */
    "guillemotleft",     /* 106 */
    "guilsinglleft",     /* 107 */
    "guilsinglright",    /* 108 */
    "fi",                /* 109 */
    "fl",                /* 110 */
    "endash",            /* 111 */
    "dagger",            /* 112 */
    "daggerdbl",         /* 113 */
    "periodcentered",    /* 114 */
    "paragraph",         /* 115 */
    "bullet",            /* 116 */
    "quotesinglbase",    /* 117 */
    "quotedblbase",      /* 118 */
    "quotedblright",     /* 119 */
    "guillemotright",    /* 120 */
    "ellipsis",          /* 121 */
    "perthousand",       /* 122 */
    "questiondown",      /* 123 */
    "grave",             /* 124 */
    "acute",             /* 125 */
    "circumflex",        /* 126 */
    "tilde",             /* 127 */
    "macron",            /* 128 */
    "breve",             /* 129 */
    "dotaccent",         /* 130 */
    "dieresis",          /* 131 */
    "ring",              /* 132 */
    "cedilla",           /* 133 */
    "hungarumlaut",      /* 134 */
    "ogonek",            /* 135 */
    "caron",             /* 136 */
    "emdash",            /* 137 */
    "AE",                /* 138 */
    "ordfeminine",       /* 139 */
    "Lslash",            /* 140 */
    "Oslash",            /* 141 */
    "OE",                /* 142 */
    "ordmasculine",      /* 143 */
    "ae",                /* 144 */
    "dotlessi",          /* 145 */
    "lslash",            /* 146 */
    "oslash",            /* 147 */
    "oe",                /* 148 */
    "germandbls",        /* 149 */
    "onesuperior",       /* 150 */
    "logicalnot",        /* 151 */
    "mu",                /* 152 */
    "trademark",         /* 153 */
    "Eth",               /* 154 */
    "onehalf",           /* 155 */
    "plusminus",         /* 156 */
    "Thorn",             /* 157 */
    "onequarter",        /* 158 */
    "divide",            /* 159 */
    "brokenbar",         /* 160 */
    "degree",            /* 161 */
    "thorn",             /* 162 */
    "threequarters",     /* 163 */
    "twosuperior",       /* 164 */
    "registered",        /* 165 */
    "minus",             /* 166 */
    "eth",               /* 167 */
    "multiply",          /* 168 */
    "threesuperior",     /* 169 */
    "copyright",         /* 170 */
    "Aacute",            /* 171 */
    "Acircumflex",       /* 172 */
    "Adieresis",         /* 173 */
    "Agrave",            /* 174 */
    "Aring",             /* 175 */
    "Atilde",            /* 176 */
    "Ccedilla",          /* 177 */
    "Eacute",            /* 178 */
    "Ecircumflex",       /* 179 */
    "Edieresis",         /* 180 */
    "Egrave",            /* 181 */
    "Iacute",            /* 182 */
    "Icircumflex",       /* 183 */
    "Idieresis",         /* 184 */
    "Igrave",            /* 185 */
    "Ntilde",            /* 186 */
    "Oacute",            /* 187 */
    "Ocircumflex",       /* 188 */
    "Odieresis",         /* 189 */
    "Ograve",            /* 190 */
    "Otilde",            /* 191 */
    "Scaron",            /* 192 */
    "Uacute",            /* 193 */
    "Ucircumflex",       /* 194 */
    "Udieresis",         /* 195 */
    "Ugrave",            /* 196 */
    "Yacute",            /* 197 */
    "Ydieresis",         /* 198 */
    "Zcaron",            /* 199 */
    "aacute",            /* 200 */
    "acircumflex",       /* 201 */
    "adieresis",         /* 202 */
    "agrave",            /* 203 */
    "aring",             /* 204 */
    "atilde",            /* 205 */
    "ccedilla",          /* 206 */
    "eacute",            /* 207 */
    "ecircumflex",       /* 208 */
    "edieresis",         /* 209 */
    "egrave",            /* 210 */
    "iacute",            /* 211 */
    "icircumflex",       /* 212 */
    "idieresis",         /* 213 */
    "igrave",            /* 214 */
    "ntilde",            /* 215 */
    "oacute",            /* 216 */
    "ocircumflex",       /* 217 */
    "odieresis",         /* 218 */
    "ograve",            /* 219 */
    "otilde",            /* 220 */
    "scaron",            /* 221 */
    "uacute",            /* 222 */
    "ucircumflex",       /* 223 */
    "udieresis",         /* 224 */
    "ugrave",            /* 225 */
    "yacute",            /* 226 */
    "ydieresis",         /* 227 */
    "zcaron",            /* 228 */
    "exclamsmall",       /* 229 */
    "Hungarumlautsmall", /* 230 */
    "dollaroldstyle",    /* 231 */
    "dollarsuperior",    /* 232 */
    "ampersandsmall",    /* 233 */
    "Acutesmall",        /* 234 */
    "parenleftsuperior", /* 235 */
    "parenrightsuperior",/* 236 */
    "twodotenleader",    /* 237 */
    "onedotenleader",    /* 238 */
    "zerooldstyle",      /* 239 */
    "oneoldstyle",       /* 240 */
    "twooldstyle",       /* 241 */
    "threeoldstyle",     /* 242 */
    "fouroldstyle",      /* 243 */
    "fiveoldstyle",      /* 244 */
    "sixoldstyle",       /* 245 */
    "sevenoldstyle",     /* 246 */
    "eightoldstyle",     /* 247 */
    "nineoldstyle",      /* 248 */
    "commasuperior",     /* 249 */
    "threequartersemdash", /* 250 */
    "periodsuperior",    /* 251 */
    "questionsmall",     /* 252 */
    "asuperior",         /* 253 */
    "bsuperior",         /* 254 */
    "centsuperior",      /* 255 */
    "dsuperior",         /* 256 */
    "esuperior",         /* 257 */
    "isuperior",         /* 258 */
    "lsuperior",         /* 259 */
    "msuperior",         /* 260 */
    "nsuperior",         /* 261 */
    "osuperior",         /* 262 */
    "rsuperior",         /* 263 */
    "ssuperior",         /* 264 */
    "tsuperior",         /* 265 */
    "ff",                /* 266 */
    "ffi",               /* 267 */
    "ffl",               /* 268 */
    "parenleftinferior", /* 269 */
    "parenrightinferior",/* 270 */
    "Circumflexsmall",   /* 271 */
    "hyphensuperior",    /* 272 */
    "Gravesmall",        /* 273 */
    "Asmall",            /* 274 */
    "Bsmall",            /* 275 */
    "Csmall",            /* 276 */
    "Dsmall",            /* 277 */
    "Esmall",            /* 278 */
    "Fsmall",            /* 279 */
    "Gsmall",            /* 280 */
    "Hsmall",            /* 281 */
    "Ismall",            /* 282 */
    "Jsmall",            /* 283 */
    "Ksmall",            /* 284 */
    "Lsmall",            /* 285 */
    "Msmall",            /* 286 */
    "Nsmall",            /* 287 */
    "Osmall",            /* 288 */
    "Psmall",            /* 289 */
    "Qsmall",            /* 290 */
    "Rsmall",            /* 291 */
    "Ssmall",            /* 292 */
    "Tsmall",            /* 293 */
    "Usmall",            /* 294 */
    "Vsmall",            /* 295 */
    "Wsmall",            /* 296 */
    "Xsmall",            /* 297 */
    "Ysmall",            /* 298 */
    "Zsmall",            /* 299 */
    "colonmonetary",     /* 300 */
    "onefitted",         /* 301 */
    "rupiah",            /* 302 */
    "Tildesmall",        /* 303 */
    "exclamdownsmall",   /* 304 */
    "centoldstyle",      /* 305 */
    "Lslashsmall",       /* 306 */
    "Scaronsmall",       /* 307 */
    "Zcaronsmall",       /* 308 */
    "Dieresissmall",     /* 309 */
    "Brevesmall",        /* 310 */
    "Caronsmall",        /* 311 */
    "Dotaccentsmall",    /* 312 */
    "Macronsmall",       /* 313 */
    "figuredash",        /* 314 */
    "hypheninferior",    /* 315 */
    "Ogoneksmall",       /* 316 */
    "Ringsmall",         /* 317 */
    "Cedillasmall",      /* 318 */
    "questiondownsmall", /* 319 */
    "oneeighth",         /* 320 */
    "threeeighths",      /* 321 */
    "fiveeighths",       /* 322 */
    "seveneighths",      /* 323 */
    "onethird",          /* 324 */
    "twothirds",         /* 325 */
    "zerosuperior",      /* 326 */
    "foursuperior",      /* 327 */
    "fivesuperior",      /* 328 */
    "sixsuperior",       /* 329 */
    "sevensuperior",     /* 330 */
    "eightsuperior",     /* 331 */
    "ninesuperior",      /* 332 */
    "zeroinferior",      /* 333 */
    "oneinferior",       /* 334 */
    "twoinferior",       /* 335 */
    "threeinferior",     /* 336 */
    "fourinferior",      /* 337 */
    "fiveinferior",      /* 338 */
    "sixinferior",       /* 339 */
    "seveninferior",     /* 340 */
    "eightinferior",     /* 341 */
    "nineinferior",      /* 342 */
    "centinferior",      /* 343 */
    "dollarinferior",    /* 344 */
    "periodinferior",    /* 345 */
    "commainferior",     /* 346 */
    "Agravesmall",       /* 347 */
    "Aacutesmall",       /* 348 */
    "Acircumflexsmall",  /* 349 */
    "Atildesmall",       /* 350 */
    "Adieresissmall",    /* 351 */
    "Aringsmall",        /* 352 */
    "AEsmall",           /* 353 */
    "Ccedillasmall",     /* 354 */
    "Egravesmall",       /* 355 */
    "Eacutesmall",       /* 356 */
    "Ecircumflexsmall",  /* 357 */
    "Edieresissmall",    /* 358 */
    "Igravesmall",       /* 359 */
    "Iacutesmall",       /* 360 */
    "Icircumflexsmall",  /* 361 */
    "Idieresissmall",    /* 362 */
    "Ethsmall",          /* 363 */
    "Ntildesmall",       /* 364 */
    "Ogravesmall",       /* 365 */
    "Oacutesmall",       /* 366 */
    "Ocircumflexsmall",  /* 367 */
    "Otildesmall",       /* 368 */
    "Odieresissmall",    /* 369 */
    "OEsmall",           /* 370 */
    "Oslashsmall",       /* 371 */
    "Ugravesmall",       /* 372 */
    "Uacutesmall",       /* 373 */
    "Ucircumflexsmall",  /* 374 */
    "Udieresissmall",    /* 375 */
    "Yacutesmall",       /* 376 */
    "Thornsmall",        /* 377 */
    "Ydieresissmall",    /* 378 */
    "001.000",           /* 379 */
    "001.001",           /* 380 */
    "001.002",           /* 381 */
    "001.003",           /* 382 */
    "Black",             /* 383 */
    "Bold",              /* 384 */
    "Book",              /* 385 */
    "Light",             /* 386 */
    "Medium",            /* 387 */
    "Regular",           /* 388 */
    "Roman",             /* 389 */
    "Semibold",          /* 390 */
};

#define CFF_NUM_STANDARD_STRINGS 391

/*
 * Get a CFF string by SID.
 * SIDs 0-390 are standard strings.
 * SIDs >= 391 index into the font's String INDEX.
 * Returns a pointer to the string (NOT null-terminated for font strings;
 * for standard strings, it is null-terminated).
 * Sets *out_len to the string length.
 * Returns NULL if the SID is invalid.
 */
static const char *cff_get_string(const uint8_t *cff_data, size_t cff_len,
                                   const uint8_t *string_index_ptr,
                                   int string_index_count,
                                   uint16_t sid, size_t *out_len)
{
    if (sid < CFF_NUM_STANDARD_STRINGS) {
        const char *s = cff_standard_strings[sid];
        if (out_len) *out_len = strlen(s);
        return s;
    }

    /* Font-specific string: index into String INDEX */
    int idx = sid - CFF_NUM_STANDARD_STRINGS;
    if (idx >= string_index_count || !string_index_ptr) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t idx_offset = (size_t)(string_index_ptr - cff_data);
    CffIndex str_idx;
    if (!cff_parse_index(cff_data, idx_offset, cff_len, &str_idx)) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t slen = 0;
    const uint8_t *sdata = cff_index_get(&str_idx, idx, &slen);
    if (!sdata) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    if (out_len) *out_len = slen;
    return (const char *)sdata;
}

/* ─── Charset Parsing ─── */

/*
 * Parse the CFF charset table and store GID→SID mapping.
 * GID 0 always maps to SID 0 (.notdef).
 * Supports Format 0, 1, and 2.
 */
static void cff_parse_charset(ParsedFont *font, const uint8_t *cff,
                               int charset_offset, size_t cff_len)
{
    int num_glyphs = font->num_glyphs;
    if (num_glyphs <= 0) return;

    /* Allocate the SID array */
    font->charset_sids = (uint16_t *)calloc(num_glyphs, sizeof(uint16_t));
    if (!font->charset_sids) return;

    /* GID 0 is always .notdef (SID 0) */
    font->charset_sids[0] = 0;

    if (charset_offset == 0) {
        /* ISOAdobe predefined charset: GID i maps to SID i */
        for (int i = 1; i < num_glyphs && i < 229; i++) {
            font->charset_sids[i] = (uint16_t)i;
        }
        return;
    }
    if (charset_offset == 1) {
        /* Expert predefined charset - use identity-ish for now */
        for (int i = 1; i < num_glyphs; i++) {
            font->charset_sids[i] = (uint16_t)i;
        }
        return;
    }
    if (charset_offset == 2) {
        /* ExpertSubset predefined charset - use identity-ish for now */
        for (int i = 1; i < num_glyphs; i++) {
            font->charset_sids[i] = (uint16_t)i;
        }
        return;
    }

    /* Custom charset at the given offset */
    size_t pos = (size_t)charset_offset;
    if (pos >= cff_len) return;

    uint8_t format = cff_u8(cff, pos, cff_len);
    pos++;

    if (format == 0) {
        /* Format 0: array of SIDs, one per glyph (excluding .notdef) */
        for (int gid = 1; gid < num_glyphs; gid++) {
            if (pos + 1 >= cff_len) break;
            font->charset_sids[gid] = cff_u16(cff, pos, cff_len);
            pos += 2;
        }
    }
    else if (format == 1) {
        /* Format 1: ranges of SIDs */
        int gid = 1;
        while (gid < num_glyphs && pos + 2 < cff_len) {
            uint16_t first_sid = cff_u16(cff, pos, cff_len);
            uint8_t n_left = cff_u8(cff, pos + 2, cff_len);
            pos += 3;

            for (int j = 0; j <= n_left && gid < num_glyphs; j++) {
                font->charset_sids[gid] = first_sid + j;
                gid++;
            }
        }
    }
    else if (format == 2) {
        /* Format 2: ranges with 16-bit count (for CIDFonts) */
        int gid = 1;
        while (gid < num_glyphs && pos + 3 < cff_len) {
            uint16_t first_sid = cff_u16(cff, pos, cff_len);
            uint16_t n_left = cff_u16(cff, pos + 2, cff_len);
            pos += 4;

            for (int j = 0; j <= (int)n_left && gid < num_glyphs; j++) {
                font->charset_sids[gid] = first_sid + j;
                gid++;
            }
        }
    }
}

/* ─── Type 2 Charstring Interpreter ─── */

/*
 * State for the Type 2 charstring interpreter.
 * This is a stack-based virtual machine that interprets the binary
 * charstring bytecode and emits path commands.
 */
typedef struct {
    /* Operand stack */
    double   stack[CFF_MAX_STACK];
    int      sp;   /* stack pointer (number of items on stack) */

    /* Current point */
    double   x, y;

    /* Has the first moveto been seen? */
    bool     have_moveto;

    /* Has the path been started (are we inside a subpath)? */
    bool     path_open;

    /* Width detection: true if width has been determined */
    bool     width_parsed;
    double   width;

    /* Hint count (needed for hintmask/cntrmask byte calculation) */
    int      num_hints;

    /* Subroutine call depth (to prevent infinite recursion) */
    int      call_depth;

    /* Font reference (for subr access) */
    ParsedFont *font;

    /* Output outline */
    GlyphOutline *outline;
} T2Interp;

/* Forward declaration */
static bool t2_execute(T2Interp *interp, const uint8_t *data, size_t len);

/*
 * Close the current subpath if one is open.
 */
static void t2_close_path(T2Interp *interp) {
    if (interp->path_open) {
        GlyphCmd cmd = {0};
        cmd.type = GLYPH_CLOSE;
        glyph_outline_add(interp->outline, cmd);
        interp->path_open = false;
    }
}

/*
 * Check for and consume the optional width operand.
 * The first stack-clearing operator in a charstring may have an extra
 * argument at the bottom of the stack which is the advance width.
 *
 * expected_args: the number of arguments the operator normally expects.
 * If the stack has one more than expected, the bottom element is the width.
 *
 * Returns the number of operands consumed (0 or 1).
 */
static int t2_check_width(T2Interp *interp, int expected_args) {
    if (!interp->width_parsed) {
        interp->width_parsed = true;
        if (interp->sp > expected_args) {
            /* Bottom of stack is the width operand */
            interp->width = interp->font->nominal_width + interp->stack[0];
            /* Shift remaining operands down by 1 */
            for (int i = 0; i < interp->sp - 1; i++) {
                interp->stack[i] = interp->stack[i + 1];
            }
            interp->sp--;
            return 1;
        } else {
            interp->width = interp->font->default_width;
        }
    }
    return 0;
}

/*
 * Check width for stem operators, which take an even number of arguments.
 * If the stack has an odd number of elements, the first is the width.
 */
static void t2_check_width_stem(T2Interp *interp) {
    if (!interp->width_parsed) {
        interp->width_parsed = true;
        if (interp->sp % 2 != 0) {
            /* Odd number of args: first is width */
            interp->width = interp->font->nominal_width + interp->stack[0];
            for (int i = 0; i < interp->sp - 1; i++) {
                interp->stack[i] = interp->stack[i + 1];
            }
            interp->sp--;
        } else {
            interp->width = interp->font->default_width;
        }
    }
}

/*
 * Emit a moveto command and handle path closing.
 */
static void t2_moveto(T2Interp *interp, double dx, double dy) {
    /* Close previous subpath if open */
    t2_close_path(interp);

    interp->x += dx;
    interp->y += dy;

    GlyphCmd cmd = {0};
    cmd.type = GLYPH_MOVETO;
    cmd.x = interp->x;
    cmd.y = interp->y;
    glyph_outline_add(interp->outline, cmd);

    interp->path_open = true;
    interp->have_moveto = true;
}

/*
 * Emit a lineto command.
 */
static void t2_lineto(T2Interp *interp, double dx, double dy) {
    interp->x += dx;
    interp->y += dy;

    GlyphCmd cmd = {0};
    cmd.type = GLYPH_LINETO;
    cmd.x = interp->x;
    cmd.y = interp->y;
    glyph_outline_add(interp->outline, cmd);
}

/*
 * Emit a cubic bezier curve command.
 * All deltas are relative to current point.
 */
static void t2_curveto(T2Interp *interp,
                       double dx1, double dy1,
                       double dx2, double dy2,
                       double dx3, double dy3)
{
    double cx1 = interp->x + dx1;
    double cy1 = interp->y + dy1;
    double cx2 = cx1 + dx2;
    double cy2 = cy1 + dy2;
    double ex  = cx2 + dx3;
    double ey  = cy2 + dy3;

    GlyphCmd cmd = {0};
    cmd.type = GLYPH_CURVETO;
    cmd.cx1 = cx1;
    cmd.cy1 = cy1;
    cmd.cx2 = cx2;
    cmd.cy2 = cy2;
    cmd.x = ex;
    cmd.y = ey;
    glyph_outline_add(interp->outline, cmd);

    interp->x = ex;
    interp->y = ey;
}

/*
 * Get a subroutine's data from a subr INDEX.
 * Returns pointer to charstring data and sets *out_len.
 */
static const uint8_t *cff_get_subr(const uint8_t *subr_index_ptr,
                                   int subr_count, int bias,
                                   int raw_index,
                                   const uint8_t *cff_data,
                                   size_t cff_len,
                                   size_t *out_len)
{
    if (!subr_index_ptr || subr_count <= 0) return NULL;

    int actual_index = raw_index + bias;
    if (actual_index < 0 || actual_index >= subr_count) return NULL;

    /*
     * We need to parse the INDEX at subr_index_ptr to get element actual_index.
     * subr_index_ptr points to the start of the INDEX structure.
     */
    size_t idx_offset = (size_t)(subr_index_ptr - cff_data);
    CffIndex idx;
    if (!cff_parse_index(cff_data, idx_offset, cff_len, &idx)) return NULL;

    return cff_index_get(&idx, actual_index, out_len);
}

/*
 * Execute a Type 2 charstring.
 * This is the main interpreter loop. It processes bytes from the charstring,
 * pushing operands onto the stack and executing operators.
 */
static bool t2_execute(T2Interp *interp, const uint8_t *data, size_t len)
{
    size_t pos = 0;
    int i; /* loop variable used in several operators */

    while (pos < len) {
        uint8_t b0 = data[pos++];

        /* ── Number encoding ── */
        if (b0 >= 32 && b0 <= 246) {
            /* 1-byte integer */
            if (interp->sp < CFF_MAX_STACK)
                interp->stack[interp->sp++] = (double)(b0 - 139);
            continue;
        }
        if (b0 >= 247 && b0 <= 250) {
            /* 2-byte positive integer */
            if (pos >= len) return false;
            uint8_t b1 = data[pos++];
            if (interp->sp < CFF_MAX_STACK)
                interp->stack[interp->sp++] = (double)((b0 - 247) * 256 + b1 + 108);
            continue;
        }
        if (b0 >= 251 && b0 <= 254) {
            /* 2-byte negative integer */
            if (pos >= len) return false;
            uint8_t b1 = data[pos++];
            if (interp->sp < CFF_MAX_STACK)
                interp->stack[interp->sp++] = (double)(-(b0 - 251) * 256 - b1 - 108);
            continue;
        }
        if (b0 == 28) {
            /* 16-bit signed integer */
            if (pos + 1 >= len) return false;
            int16_t v = (int16_t)((data[pos] << 8) | data[pos + 1]);
            pos += 2;
            if (interp->sp < CFF_MAX_STACK)
                interp->stack[interp->sp++] = (double)v;
            continue;
        }
        if (b0 == 255) {
            /* 32-bit fixed-point 16.16 */
            if (pos + 3 >= len) return false;
            int32_t v = (int32_t)((data[pos] << 24) | (data[pos+1] << 16) |
                                  (data[pos+2] << 8) | data[pos+3]);
            pos += 4;
            if (interp->sp < CFF_MAX_STACK)
                interp->stack[interp->sp++] = (double)v / 65536.0;
            continue;
        }

        /* ── Operators ── */

        if (b0 == 12) {
            /* Two-byte operator */
            if (pos >= len) return false;
            uint8_t b1 = data[pos++];

            switch (b1) {
                case 34: {
                    /* hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6 */
                    if (interp->sp < 7) { interp->sp = 0; break; }
                    double dx1 = interp->stack[0];
                    double dx2 = interp->stack[1];
                    double dy2 = interp->stack[2];
                    double dx3 = interp->stack[3];
                    double dx4 = interp->stack[4];
                    double dx5 = interp->stack[5];
                    double dx6 = interp->stack[6];
                    t2_curveto(interp, dx1, 0, dx2, dy2, dx3, 0);
                    t2_curveto(interp, dx4, 0, dx5, -dy2, dx6, 0);
                    interp->sp = 0;
                    break;
                }
                case 35: {
                    /* flex: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 dx6 dy6 fd */
                    if (interp->sp < 13) { interp->sp = 0; break; }
                    t2_curveto(interp,
                        interp->stack[0], interp->stack[1],
                        interp->stack[2], interp->stack[3],
                        interp->stack[4], interp->stack[5]);
                    t2_curveto(interp,
                        interp->stack[6], interp->stack[7],
                        interp->stack[8], interp->stack[9],
                        interp->stack[10], interp->stack[11]);
                    /* stack[12] is the flex depth, ignored for rendering */
                    interp->sp = 0;
                    break;
                }
                case 36: {
                    /* hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6 */
                    if (interp->sp < 9) { interp->sp = 0; break; }
                    double dx1 = interp->stack[0];
                    double dy1 = interp->stack[1];
                    double dx2 = interp->stack[2];
                    double dy2 = interp->stack[3];
                    double dx3 = interp->stack[4];
                    double dx4 = interp->stack[5];
                    double dx5 = interp->stack[6];
                    double dy5 = interp->stack[7];
                    double dx6 = interp->stack[8];
                    /* dy6 = -(dy1+dy2+dy5) to return to baseline */
                    double dy6 = -(dy1 + dy2 + dy5);
                    t2_curveto(interp, dx1, dy1, dx2, dy2, dx3, 0);
                    t2_curveto(interp, dx4, 0, dx5, dy5, dx6, dy6);
                    interp->sp = 0;
                    break;
                }
                case 37: {
                    /* flex1: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6 */
                    if (interp->sp < 11) { interp->sp = 0; break; }
                    double dx1 = interp->stack[0], dy1 = interp->stack[1];
                    double dx2 = interp->stack[2], dy2 = interp->stack[3];
                    double dx3 = interp->stack[4], dy3 = interp->stack[5];
                    double dx4 = interp->stack[6], dy4 = interp->stack[7];
                    double dx5 = interp->stack[8], dy5 = interp->stack[9];
                    double d6  = interp->stack[10];

                    /* Determine whether final delta is horizontal or vertical */
                    double abs_dx = fabs(dx1+dx2+dx3+dx4+dx5);
                    double abs_dy = fabs(dy1+dy2+dy3+dy4+dy5);
                    double dx6, dy6;
                    if (abs_dx > abs_dy) {
                        dx6 = d6;
                        dy6 = -(dy1+dy2+dy3+dy4+dy5);
                    } else {
                        dx6 = -(dx1+dx2+dx3+dx4+dx5);
                        dy6 = d6;
                    }
                    t2_curveto(interp, dx1, dy1, dx2, dy2, dx3, dy3);
                    t2_curveto(interp, dx4, dy4, dx5, dy5, dx6, dy6);
                    interp->sp = 0;
                    break;
                }

                default:
                    /* Unknown 2-byte operator: ignore and clear stack */
                    interp->sp = 0;
                    break;
            }
            continue;
        }

        /* Single-byte operators */
        switch (b0) {
            case 1:   /* hstem */
            case 3:   /* vstem */
            case 18:  /* hstemhm */
            case 23:  /* vstemhm */
                /*
                 * Stem hint operators consume pairs from the stack.
                 * We just count them (needed for hintmask byte count)
                 * and clear the stack.
                 */
                t2_check_width_stem(interp);
                interp->num_hints += interp->sp / 2;
                interp->sp = 0;
                break;

            case 19:  /* hintmask */
            case 20:  /* cntrmask */
                /*
                 * These operators may have stem hints on the stack (implicit vstem).
                 * Then they consume (num_hints+7)/8 mask bytes after the operator.
                 */
                t2_check_width_stem(interp);
                /* Any remaining stack values are implicit vstem hints */
                interp->num_hints += interp->sp / 2;
                interp->sp = 0;
                {
                    /* Skip mask bytes */
                    int mask_bytes = (interp->num_hints + 7) / 8;
                    if (mask_bytes < 1) mask_bytes = 1;
                    pos += mask_bytes;
                    if (pos > len) pos = len;
                }
                break;

            case 21:  /* rmoveto: dx dy */
                t2_check_width(interp, 2);
                if (interp->sp >= 2) {
                    t2_moveto(interp, interp->stack[0], interp->stack[1]);
                }
                interp->sp = 0;
                break;

            case 22:  /* hmoveto: dx */
                t2_check_width(interp, 1);
                if (interp->sp >= 1) {
                    t2_moveto(interp, interp->stack[0], 0);
                }
                interp->sp = 0;
                break;

            case 4:   /* vmoveto: dy */
                t2_check_width(interp, 1);
                if (interp->sp >= 1) {
                    t2_moveto(interp, 0, interp->stack[0]);
                }
                interp->sp = 0;
                break;

            case 5:   /* rlineto: {dx dy}+ */
                for (i = 0; i + 1 < interp->sp; i += 2) {
                    t2_lineto(interp, interp->stack[i], interp->stack[i+1]);
                }
                interp->sp = 0;
                break;

            case 6:   /* hlineto: alternating dx, dy, dx, dy, ... */
                for (i = 0; i < interp->sp; i++) {
                    if (i % 2 == 0)
                        t2_lineto(interp, interp->stack[i], 0); /* horizontal */
                    else
                        t2_lineto(interp, 0, interp->stack[i]); /* vertical */
                }
                interp->sp = 0;
                break;

            case 7:   /* vlineto: alternating dy, dx, dy, dx, ... */
                for (i = 0; i < interp->sp; i++) {
                    if (i % 2 == 0)
                        t2_lineto(interp, 0, interp->stack[i]); /* vertical */
                    else
                        t2_lineto(interp, interp->stack[i], 0); /* horizontal */
                }
                interp->sp = 0;
                break;

            case 8:   /* rrcurveto: {dx1 dy1 dx2 dy2 dx3 dy3}+ */
                for (i = 0; i + 5 < interp->sp; i += 6) {
                    t2_curveto(interp,
                        interp->stack[i],   interp->stack[i+1],
                        interp->stack[i+2], interp->stack[i+3],
                        interp->stack[i+4], interp->stack[i+5]);
                }
                interp->sp = 0;
                break;

            case 24:  /* rcurveline: {dx1 dy1 dx2 dy2 dx3 dy3}+ dxf dyf */
                if (interp->sp >= 8) {
                    int n_curves = (interp->sp - 2) / 6;
                    for (i = 0; i < n_curves; i++) {
                        int base = i * 6;
                        t2_curveto(interp,
                            interp->stack[base],   interp->stack[base+1],
                            interp->stack[base+2], interp->stack[base+3],
                            interp->stack[base+4], interp->stack[base+5]);
                    }
                    /* Final line */
                    int line_base = n_curves * 6;
                    t2_lineto(interp, interp->stack[line_base], interp->stack[line_base+1]);
                }
                interp->sp = 0;
                break;

            case 25:  /* rlinecurve: {dx dy}+ dx1 dy1 dx2 dy2 dx3 dy3 */
                if (interp->sp >= 8) {
                    int n_lines = (interp->sp - 6) / 2;
                    for (i = 0; i < n_lines; i++) {
                        t2_lineto(interp, interp->stack[i*2], interp->stack[i*2+1]);
                    }
                    int curve_base = n_lines * 2;
                    t2_curveto(interp,
                        interp->stack[curve_base],   interp->stack[curve_base+1],
                        interp->stack[curve_base+2], interp->stack[curve_base+3],
                        interp->stack[curve_base+4], interp->stack[curve_base+5]);
                }
                interp->sp = 0;
                break;

            case 26: {
                /*
                 * vvcurveto: dx1? {dy1 dx2 dy2 dy3}+
                 * If stack count is not a multiple of 4, the first element is dx1
                 * for the first curve's dx1 (all other curves have dx1=0).
                 */
                int si = 0;
                double extra_dx = 0;
                if (interp->sp % 4 != 0) {
                    extra_dx = interp->stack[si++];
                }
                bool first = true;
                while (si + 3 < interp->sp) {
                    double dy1 = interp->stack[si++];
                    double dx2 = interp->stack[si++];
                    double dy2 = interp->stack[si++];
                    double dy3 = interp->stack[si++];
                    double dx1 = (first && extra_dx != 0) ? extra_dx : 0;
                    first = false;
                    t2_curveto(interp, dx1, dy1, dx2, dy2, 0, dy3);
                }
                interp->sp = 0;
                break;
            }

            case 27: {
                /*
                 * hhcurveto: dy1? {dx1 dx2 dy2 dx3}+
                 * If stack count is not a multiple of 4, the first element is dy1
                 * for the first curve's dy1 (all other curves have dy1=0).
                 */
                int si = 0;
                double extra_dy = 0;
                if (interp->sp % 4 != 0) {
                    extra_dy = interp->stack[si++];
                }
                bool first = true;
                while (si + 3 < interp->sp) {
                    double dx1 = interp->stack[si++];
                    double dx2 = interp->stack[si++];
                    double dy2 = interp->stack[si++];
                    double dx3 = interp->stack[si++];
                    double dy1 = (first && extra_dy != 0) ? extra_dy : 0;
                    first = false;
                    t2_curveto(interp, dx1, dy1, dx2, dy2, dx3, 0);
                }
                interp->sp = 0;
                break;
            }

            case 30: {
                /*
                 * vhcurveto: alternating v-start and h-start curves
                 * First curve: dy1 dx2 dy2 dx3 [+dy3 if last and odd arg count]
                 * Second curve: dx1 dx2 dy2 dy3 [+dx3 if last and odd arg count]
                 * Pattern repeats.
                 *
                 * If the total number of args is 4n+1, the last arg is an
                 * extra tangent component for the last curve.
                 */
                int si = 0;
                int remaining = interp->sp;
                bool v_start = true; /* first curve starts vertical */

                while (si + 3 < interp->sp) {
                    int left = interp->sp - si;
                    if (v_start) {
                        /* v-start: dy1 dx2 dy2 dx3 [+dy_last] */
                        double dy1 = interp->stack[si++];
                        double dx2 = interp->stack[si++];
                        double dy2 = interp->stack[si++];
                        double dx3 = interp->stack[si++];
                        /* If this is the last curve and there's one extra arg */
                        double dy3 = 0;
                        if (interp->sp - si == 1) {
                            dy3 = interp->stack[si++];
                        }
                        t2_curveto(interp, 0, dy1, dx2, dy2, dx3, dy3);
                    } else {
                        /* h-start: dx1 dx2 dy2 dy3 [+dx_last] */
                        double dx1 = interp->stack[si++];
                        double dx2 = interp->stack[si++];
                        double dy2 = interp->stack[si++];
                        double dy3 = interp->stack[si++];
                        /* If this is the last curve and there's one extra arg */
                        double dx3 = 0;
                        if (interp->sp - si == 1) {
                            dx3 = interp->stack[si++];
                        }
                        t2_curveto(interp, dx1, 0, dx2, dy2, dx3, dy3);
                    }
                    v_start = !v_start;
                }
                interp->sp = 0;
                (void)remaining;
                break;
            }

            case 31: {
                /*
                 * hvcurveto: alternating h-start and v-start curves
                 * Same as vhcurveto but starts with horizontal.
                 */
                int si = 0;
                bool h_start = true;

                while (si + 3 < interp->sp) {
                    if (h_start) {
                        /* h-start: dx1 dx2 dy2 dy3 [+dx_last] */
                        double dx1 = interp->stack[si++];
                        double dx2 = interp->stack[si++];
                        double dy2 = interp->stack[si++];
                        double dy3 = interp->stack[si++];
                        double dx3 = 0;
                        if (interp->sp - si == 1) {
                            dx3 = interp->stack[si++];
                        }
                        t2_curveto(interp, dx1, 0, dx2, dy2, dx3, dy3);
                    } else {
                        /* v-start: dy1 dx2 dy2 dx3 [+dy_last] */
                        double dy1 = interp->stack[si++];
                        double dx2 = interp->stack[si++];
                        double dy2 = interp->stack[si++];
                        double dx3 = interp->stack[si++];
                        double dy3 = 0;
                        if (interp->sp - si == 1) {
                            dy3 = interp->stack[si++];
                        }
                        t2_curveto(interp, 0, dy1, dx2, dy2, dx3, dy3);
                    }
                    h_start = !h_start;
                }
                interp->sp = 0;
                break;
            }

            case 14:  /* endchar */
                t2_check_width(interp, 0);
                t2_close_path(interp);
                interp->sp = 0;
                /* endchar terminates the charstring */
                return true;

            case 10: {
                /* callsubr: pop index, call local subroutine */
                if (interp->sp < 1) break;
                int raw_idx = (int)interp->stack[--interp->sp];

                if (interp->call_depth >= CFF_MAX_CALL_DEPTH) break;

                size_t subr_len = 0;
                const uint8_t *subr_data = cff_get_subr(
                    interp->font->local_subr_index,
                    interp->font->local_subr_count,
                    interp->font->local_subr_bias,
                    raw_idx,
                    interp->font->cff_start,
                    interp->font->cff_len,
                    &subr_len);

                if (!subr_data || subr_len == 0) break;

                interp->call_depth++;
                bool ok = t2_execute(interp, subr_data, subr_len);
                interp->call_depth--;
                if (!ok) return false;
                break;
            }

            case 29: {
                /* callgsubr: pop index, call global subroutine */
                if (interp->sp < 1) break;
                int raw_idx = (int)interp->stack[--interp->sp];

                if (interp->call_depth >= CFF_MAX_CALL_DEPTH) break;

                size_t subr_len = 0;
                const uint8_t *subr_data = cff_get_subr(
                    interp->font->global_subr_index,
                    interp->font->global_subr_count,
                    interp->font->global_subr_bias,
                    raw_idx,
                    interp->font->cff_start,
                    interp->font->cff_len,
                    &subr_len);

                if (!subr_data || subr_len == 0) break;

                interp->call_depth++;
                bool ok = t2_execute(interp, subr_data, subr_len);
                interp->call_depth--;
                if (!ok) return false;
                break;
            }

            case 11:  /* return: return from subroutine */
                return true;

            default:
                /* Unknown operator: clear stack and continue */
                interp->sp = 0;
                break;
        }
    }

    return true;
}

/* ─── Public API Implementation ─── */

/*
 * Parse a CFF font from raw Type1C data.
 *
 * CFF structure:
 *   Header (4 bytes)
 *   Name INDEX
 *   Top DICT INDEX
 *   String INDEX
 *   Global Subr INDEX
 *   [CharStrings INDEX, Private DICT, Local Subr INDEX at offsets from DICTs]
 */
ParsedFont *parsed_font_from_cff(const uint8_t *data, size_t len)
{
    if (!data || len < 4) return NULL;

    /* ── Parse CFF Header ── */
    uint8_t major = data[0];
    uint8_t minor = data[1];
    uint8_t hdr_size = data[2];
    /* uint8_t off_size = data[3]; -- absolute offset size, not typically used */

    /* Sanity check */
    if (major != 1 || hdr_size < 4 || hdr_size > len) return NULL;

    /* Allocate the ParsedFont */
    ParsedFont *font = (ParsedFont *)calloc(1, sizeof(ParsedFont));
    if (!font) return NULL;

    /* Store raw data reference */
    font->cff_start = data;
    font->cff_len = len;
    font->data = NULL;     /* we don't own the data */
    font->data_len = len;
    font->owns_data = false;
    font->is_cff = true;
    font->units_per_em = 1000;  /* CFF default */
    font->ascent = 800;         /* reasonable defaults */
    font->descent = -200;

    /* ── Parse Name INDEX ── */
    size_t pos = hdr_size;
    CffIndex name_idx;
    if (!cff_parse_index(data, pos, len, &name_idx)) {
        free(font);
        return NULL;
    }

    /* Extract font name from Name INDEX */
    if (name_idx.count > 0) {
        size_t name_len = 0;
        const uint8_t *name_data = cff_index_get(&name_idx, 0, &name_len);
        if (name_data && name_len > 0) {
            size_t copy_len = (name_len < sizeof(font->family_name) - 1) ?
                              name_len : sizeof(font->family_name) - 1;
            memcpy(font->family_name, name_data, copy_len);
            font->family_name[copy_len] = '\0';
        }
    }
    pos += name_idx.total_size;

    /* ── Parse Top DICT INDEX ── */
    CffIndex top_dict_idx;
    if (!cff_parse_index(data, pos, len, &top_dict_idx)) {
        free(font);
        return NULL;
    }

    /* Get the first (and usually only) Top DICT */
    CffDictInfo dict_info;
    memset(&dict_info, 0, sizeof(dict_info));

    if (top_dict_idx.count > 0) {
        size_t dict_len = 0;
        const uint8_t *dict_data = cff_index_get(&top_dict_idx, 0, &dict_len);
        if (dict_data && dict_len > 0) {
            size_t dict_offset = (size_t)(dict_data - data);
            cff_parse_dict(data, dict_offset, dict_len, &dict_info, false);
        }
    }
    pos += top_dict_idx.total_size;

    /* ── Parse String INDEX ── */
    CffIndex string_idx;
    if (!cff_parse_index(data, pos, len, &string_idx)) {
        free(font);
        return NULL;
    }
    /* Store String INDEX pointer for SID→name resolution */
    font->string_index_ptr = data + pos;
    font->string_index_count = string_idx.count;
    pos += string_idx.total_size;

    /* ── Parse Global Subr INDEX ── */
    CffIndex global_subr_idx;
    if (!cff_parse_index(data, pos, len, &global_subr_idx)) {
        /* Global subrs are optional; if parsing fails, just leave them empty */
        memset(&global_subr_idx, 0, sizeof(global_subr_idx));
    }

    font->global_subr_index = data + pos;
    font->global_subr_count = global_subr_idx.count;
    font->global_subr_bias = cff_subr_bias(global_subr_idx.count);

    /* ── Parse CharStrings INDEX ── */
    if (dict_info.charstrings_offset <= 0 ||
        (size_t)dict_info.charstrings_offset >= len) {
        free(font);
        return NULL;
    }

    CffIndex charstrings_idx;
    if (!cff_parse_index(data, dict_info.charstrings_offset, len, &charstrings_idx)) {
        free(font);
        return NULL;
    }

    font->charstrings_index = data + dict_info.charstrings_offset;
    font->charstrings_count = charstrings_idx.count;
    font->num_glyphs = charstrings_idx.count;

    /* ── Parse Private DICT ── */
    if (dict_info.private_size > 0 && dict_info.private_offset > 0 &&
        (size_t)(dict_info.private_offset + dict_info.private_size) <= len) {

        cff_parse_dict(data, dict_info.private_offset, dict_info.private_size,
                       &dict_info, true);

        font->default_width = dict_info.default_width_x;
        font->nominal_width = dict_info.nominal_width_x;

        /* ── Parse Local Subr INDEX ── */
        if (dict_info.local_subr_offset > 0) {
            size_t local_subr_pos = dict_info.private_offset +
                                    dict_info.local_subr_offset;
            if (local_subr_pos < len) {
                CffIndex local_subr_idx;
                if (cff_parse_index(data, local_subr_pos, len, &local_subr_idx)) {
                    font->local_subr_index = data + local_subr_pos;
                    font->local_subr_count = local_subr_idx.count;
                    font->local_subr_bias = cff_subr_bias(local_subr_idx.count);
                }
            }
        }
    }

    /* ── Set up encoding ── */
    if (dict_info.encoding_offset == 0) {
        /* Standard Encoding */
        cff_apply_standard_encoding(font);
    }
    else if (dict_info.encoding_offset == 1) {
        /* Expert Encoding */
        cff_apply_expert_encoding(font);
    }
    else {
        /* Custom encoding at the specified offset */
        cff_parse_custom_encoding(font, data, dict_info.encoding_offset, len);
    }

    /* ── Parse Charset ── */
    cff_parse_charset(font, data, dict_info.charset_offset, len);

    /* ── Pre-compute glyph widths (optional, for fast advance lookups) ── */
    font->glyph_widths = (double *)calloc(font->num_glyphs, sizeof(double));
    if (font->glyph_widths) {
        for (int gi = 0; gi < font->num_glyphs; gi++) {
            font->glyph_widths[gi] = font->default_width;
        }
    }

    return font;
}

/*
 * Get the glyph outline for a character code.
 * Interprets the Type 2 charstring for the glyph and fills the outline
 * with path commands.
 */
bool parsed_font_get_glyph(ParsedFont *font, int char_code, GlyphOutline *outline)
{
    if (!font || !outline) return false;

    /* Dispatch to Type1 handler */
    if (font->is_type1) {
        glyph_outline_init(outline);
        int gid = 0;
        if (char_code >= 0 && char_code < 256)
            gid = font->encoding[char_code];
        return t1_get_glyph_by_gid(font, gid, outline);
    }

    /* Dispatch to TrueType handler */
    if (!font->is_cff && font->glyf_table) {
        int gid = 0;
        if (char_code >= 0 && char_code < 256)
            gid = font->encoding[char_code];
        return tt_get_glyph_by_gid(font, gid, outline);
    }

    if (!font->is_cff) return false;  /* This file only handles CFF */

    glyph_outline_init(outline);

    /* Map char code to GID */
    int gid = 0;
    if (char_code >= 0 && char_code < 256) {
        gid = font->encoding[char_code];
    }

    /* Get the charstring data */
    if (!font->charstrings_index || gid < 0 || gid >= font->charstrings_count) {
        return false;
    }

    /* Parse the CharStrings INDEX to get the charstring for this GID */
    CffIndex cs_idx;
    size_t cs_idx_offset = (size_t)(font->charstrings_index - font->cff_start);
    if (!cff_parse_index(font->cff_start, cs_idx_offset, font->cff_len, &cs_idx)) {
        return false;
    }

    size_t cs_len = 0;
    const uint8_t *cs_data = cff_index_get(&cs_idx, gid, &cs_len);
    if (!cs_data || cs_len == 0) {
        return false;
    }

    /* Set up the Type 2 interpreter */
    T2Interp interp;
    memset(&interp, 0, sizeof(interp));
    interp.font = font;
    interp.outline = outline;
    interp.width = font->default_width;

    /* Execute the charstring */
    bool ok = t2_execute(&interp, cs_data, cs_len);

    /* Close any open path */
    if (interp.path_open) {
        t2_close_path(&interp);
    }

    /* Set the advance width */
    outline->advance_width = interp.width;

    /* Cache the width for this glyph */
    if (font->glyph_widths && gid < font->num_glyphs) {
        font->glyph_widths[gid] = interp.width;
    }

    return ok;
}

/*
 * Get the glyph outline for a specific GID (bypassing encoding).
 * Used when the caller has already resolved the GID via charset lookup.
 */
bool parsed_font_get_glyph_by_gid(ParsedFont *font, int gid, GlyphOutline *outline)
{
    if (!font || !outline) return false;

    /* Dispatch to Type1 handler */
    if (font->is_type1)
        return t1_get_glyph_by_gid(font, gid, outline);

    /* Dispatch to TrueType handler */
    if (!font->is_cff && font->glyf_table)
        return tt_get_glyph_by_gid(font, gid, outline);

    if (!font->is_cff) return false;

    glyph_outline_init(outline);

    if (!font->charstrings_index || gid < 0 || gid >= font->charstrings_count)
        return false;

    /* Parse the CharStrings INDEX to get the charstring for this GID */
    CffIndex cs_idx;
    size_t cs_idx_offset = (size_t)(font->charstrings_index - font->cff_start);
    if (!cff_parse_index(font->cff_start, cs_idx_offset, font->cff_len, &cs_idx))
        return false;

    size_t cs_len = 0;
    const uint8_t *cs_data = cff_index_get(&cs_idx, gid, &cs_len);
    if (!cs_data || cs_len == 0)
        return false;

    /* Set up the Type 2 interpreter */
    T2Interp interp;
    memset(&interp, 0, sizeof(interp));
    interp.font = font;
    interp.outline = outline;
    interp.width = font->default_width;

    /* Execute the charstring */
    bool ok = t2_execute(&interp, cs_data, cs_len);

    /* Close any open path */
    if (interp.path_open)
        t2_close_path(&interp);

    /* Set the advance width */
    outline->advance_width = interp.width;

    /* Cache the width for this glyph */
    if (font->glyph_widths && gid < font->num_glyphs)
        font->glyph_widths[gid] = interp.width;

    return ok;
}

/*
 * Find the GID for a glyph by name.
 * Searches the CFF charset (GID→SID→name mapping) for a match.
 * Returns the GID, or -1 if not found.
 */
int parsed_font_find_gid_by_name(ParsedFont *font, const char *glyph_name)
{
    if (!font || !glyph_name) return -1;

    /* Dispatch to Type1 handler */
    if (font->is_type1)
        return t1_find_gid_by_name(font, glyph_name);

    if (!font->is_cff) return -1;
    if (!font->charset_sids) return -1;

    size_t target_len = strlen(glyph_name);

    for (int gid = 0; gid < font->num_glyphs; gid++) {
        uint16_t sid = font->charset_sids[gid];

        /* Get the name for this SID */
        size_t name_len = 0;
        const char *name = cff_get_string(
            font->cff_start, font->cff_len,
            font->string_index_ptr, font->string_index_count,
            sid, &name_len);

        if (!name) continue;

        /* Compare: standard strings are null-terminated,
         * but font strings from the String INDEX may not be */
        if (name_len == target_len && memcmp(name, glyph_name, target_len) == 0)
            return gid;
    }

    return -1;
}

/*
 * Get the advance width for a character code.
 * If we haven't parsed this glyph yet, we do a quick parse just to get the width.
 */
double parsed_font_get_advance(ParsedFont *font, int char_code)
{
    if (!font) return 0;

    /* Dispatch to Type1 handler */
    if (font->is_type1) {
        int gid = 0;
        if (char_code >= 0 && char_code < 256)
            gid = font->encoding[char_code];
        if (font->glyph_widths && gid >= 0 && gid < font->num_glyphs &&
            font->glyph_widths[gid] != 0.0)
            return font->glyph_widths[gid];
        /* Parse glyph to get width */
        GlyphOutline outline;
        if (t1_get_glyph_by_gid(font, gid, &outline)) {
            double w = outline.advance_width;
            glyph_outline_free(&outline);
            return w;
        }
        return 0;
    }

    /* Dispatch to TrueType handler */
    if (!font->is_cff && font->glyf_table) {
        int gid = 0;
        if (char_code >= 0 && char_code < 256)
            gid = font->encoding[char_code];
        if (font->glyph_widths && gid >= 0 && gid < font->num_glyphs &&
            font->glyph_widths[gid] != 0.0)
            return font->glyph_widths[gid];
        /* Parse glyph to get width from hmtx */
        if (font->hmtx_table && gid < font->num_h_metrics) {
            double w = (double)((font->hmtx_table[gid * 4] << 8) | font->hmtx_table[gid * 4 + 1]);
            if (font->glyph_widths && gid < font->num_glyphs)
                font->glyph_widths[gid] = w;
            return w;
        }
        return 0;
    }

    if (!font->is_cff) return 0;

    /* Map char code to GID */
    int gid = 0;
    if (char_code >= 0 && char_code < 256) {
        gid = font->encoding[char_code];
    }

    /* Check cached width */
    if (font->glyph_widths && gid >= 0 && gid < font->num_glyphs &&
        font->glyph_widths[gid] != font->default_width) {
        /* Already computed, return cached value */
        return font->glyph_widths[gid];
    }

    /* Parse the glyph to extract the width */
    GlyphOutline outline;
    if (parsed_font_get_glyph(font, char_code, &outline)) {
        double w = outline.advance_width;
        glyph_outline_free(&outline);
        return w;
    }

    return font->default_width;
}

/*
 * Free a parsed font and all associated resources.
 */
void parsed_font_free(ParsedFont *font)
{
    if (!font) return;

    if (font->glyph_widths) {
        free(font->glyph_widths);
        font->glyph_widths = NULL;
    }

    if (font->charset_sids) {
        free(font->charset_sids);
        font->charset_sids = NULL;
    }

    if (font->owns_data && font->data) {
        free(font->data);
        font->data = NULL;
    }

    /* Type1-specific cleanup */
    if (font->is_type1) {
        free(font->t1_subrs_data);
        free(font->t1_subrs_offs);
        free(font->t1_subrs_lens);
        free(font->t1_charstrings_data);
        free(font->t1_charstrings_offs);
        free(font->t1_charstrings_lens);
        if (font->t1_glyph_names) {
            for (int i = 0; i < font->t1_num_charstrings; i++)
                free(font->t1_glyph_names[i]);
            free(font->t1_glyph_names);
        }
    }

    free(font);
}

/*
 * Free the commands array in a glyph outline.
 */
void glyph_outline_free(GlyphOutline *outline)
{
    if (!outline) return;
    if (outline->cmds) {
        free(outline->cmds);
        outline->cmds = NULL;
    }
    outline->count = 0;
    outline->capacity = 0;
}
