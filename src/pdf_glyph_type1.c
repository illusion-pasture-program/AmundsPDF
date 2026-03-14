/*
 * AmundsPDF - pdf_glyph_type1.c
 * Type1 (PFB/PFA) font parser and Type 1 charstring interpreter.
 *
 * Parses Type1 font data (from /FontFile in PFB or PFA format) and extracts
 * glyph outlines as cubic bezier paths for rendering.
 *
 * References:
 *   - Adobe Type 1 Font Format (the "Black Book")
 *   - Adobe Technical Note #5040: "Supporting Downloadable PostScript Language Fonts"
 */

#include "pdf_glyph.h"
#include <math.h>

/* ─── Constants ─── */
#define T1_MAX_STACK       48
#define T1_MAX_CALL_DEPTH  10
#define T1_MAX_CHARSTRINGS 4096
#define T1_MAX_SUBRS       1024
#define T1_MAX_OTHER_ARGS  32

/* ─── Standard Encoding (Adobe Standard Encoding) ─── */
/* Maps character code -> glyph name for StandardEncoding */
static const char *standard_encoding_names[256] = {
    /* 0x00-0x1F */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0x20-0x2F */
    "space", "exclam", "quotedbl", "numbersign",
    "dollar", "percent", "ampersand", "quoteright",
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
    "quoteleft", "a", "b", "c", "d", "e", "f", "g",
    "h", "i", "j", "k", "l", "m", "n", "o",
    /* 0x70-0x7F */
    "p", "q", "r", "s", "t", "u", "v", "w",
    "x", "y", "z", "braceleft",
    "bar", "braceright", "asciitilde", NULL,
    /* 0x80-0xFF */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, "exclamdown", "cent", "sterling",
    "fraction", "yen", "florin", "section",
    "currency", "quotesingle", "quotedblleft", "guillemotleft",
    "guilsinglleft", "guilsinglright", "fi", "fl",
    NULL, "endash", "dagger", "daggerdbl",
    "periodcentered", NULL, "paragraph", "bullet",
    "quotesinglbase", "quotedblbase", "quotedblright", "guillemotright",
    "ellipsis", "perthousand", NULL, "questiondown",
    NULL, "grave", "acute", "circumflex",
    "tilde", "macron", "breve", "dotaccent",
    "dieresis", NULL, "ring", "cedilla",
    NULL, "hungarumlaut", "ogonek", "caron",
    "emdash", NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, "AE", NULL, "ordfeminine",
    NULL, NULL, NULL, NULL,
    "Lslash", "Oslash", "OE", "ordmasculine",
    NULL, NULL, NULL, NULL,
    NULL, "ae", NULL, NULL,
    NULL, "dotlessi", NULL, NULL,
    "lslash", "oslash", "oe", "germandbls",
    NULL, NULL, NULL, NULL,
};

/* ─── eexec Decryption ─── */

/*
 * Decrypt eexec-encrypted data in-place.
 * The first `skip` bytes of output are random padding and should be skipped.
 */
static void eexec_decrypt(uint8_t *data, size_t len)
{
    uint16_t key = 55665;
    for (size_t i = 0; i < len; i++) {
        uint8_t cipher = data[i];
        uint8_t plain = cipher ^ (key >> 8);
        key = ((uint16_t)cipher + key) * 52845 + 22719;
        data[i] = plain;
    }
}

/*
 * Decrypt a charstring in-place.
 * The first `lenIV` bytes of output are random padding and should be skipped.
 */
static void charstring_decrypt(uint8_t *data, size_t len)
{
    uint16_t key = 4330;
    for (size_t i = 0; i < len; i++) {
        uint8_t cipher = data[i];
        uint8_t plain = cipher ^ (key >> 8);
        key = ((uint16_t)cipher + key) * 52845 + 22719;
        data[i] = plain;
    }
}

/* ─── Hex decoding for PFA ─── */
static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ─── PFB Segment Extraction ─── */

typedef struct {
    uint8_t *ascii_data;
    size_t   ascii_len;
    uint8_t *binary_data;
    size_t   binary_len;
} PfbSegments;

static bool extract_pfb_segments(const uint8_t *data, size_t len, PfbSegments *seg)
{
    memset(seg, 0, sizeof(*seg));

    /* Accumulate ASCII and binary segments */
    size_t ascii_cap = 4096, binary_cap = 32768;
    seg->ascii_data = (uint8_t *)malloc(ascii_cap);
    seg->binary_data = (uint8_t *)malloc(binary_cap);
    if (!seg->ascii_data || !seg->binary_data) return false;

    size_t pos = 0;
    while (pos + 1 < len) {
        if (data[pos] != 0x80) break; /* not a PFB segment marker */
        uint8_t seg_type = data[pos + 1];
        pos += 2;

        if (seg_type == 3) break; /* EOF marker */

        if (pos + 4 > len) break;
        uint32_t seg_len = (uint32_t)data[pos] |
                           ((uint32_t)data[pos+1] << 8) |
                           ((uint32_t)data[pos+2] << 16) |
                           ((uint32_t)data[pos+3] << 24);
        pos += 4;

        if (pos + seg_len > len) seg_len = (uint32_t)(len - pos);

        if (seg_type == 1) {
            /* ASCII segment */
            while (seg->ascii_len + seg_len > ascii_cap) {
                ascii_cap *= 2;
                uint8_t *tmp = (uint8_t *)realloc(seg->ascii_data, ascii_cap);
                if (!tmp) return false;
                seg->ascii_data = tmp;
            }
            memcpy(seg->ascii_data + seg->ascii_len, data + pos, seg_len);
            seg->ascii_len += seg_len;
        } else if (seg_type == 2) {
            /* Binary segment */
            while (seg->binary_len + seg_len > binary_cap) {
                binary_cap *= 2;
                uint8_t *tmp = (uint8_t *)realloc(seg->binary_data, binary_cap);
                if (!tmp) return false;
                seg->binary_data = tmp;
            }
            memcpy(seg->binary_data + seg->binary_len, data + pos, seg_len);
            seg->binary_len += seg_len;
        }

        pos += seg_len;
    }

    return (seg->ascii_len > 0 && seg->binary_len > 0);
}

/* ─── Simple PostScript tokenizer helpers ─── */

/* Skip whitespace and comments */
static size_t skip_ws(const char *s, size_t pos, size_t len)
{
    while (pos < len) {
        if (s[pos] == '%') {
            /* Comment: skip to end of line */
            while (pos < len && s[pos] != '\n' && s[pos] != '\r') pos++;
        } else if (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' ||
                   s[pos] == '\r' || s[pos] == '\f') {
            pos++;
        } else {
            break;
        }
    }
    return pos;
}

/* Read the next token from PostScript text (name, number, or delimiter).
 * Returns the length of the token, or 0 if end of input. */
static size_t next_token(const char *s, size_t pos, size_t len,
                          const char **out_start)
{
    pos = skip_ws(s, pos, len);
    if (pos >= len) {
        *out_start = s + pos;
        return 0;
    }

    *out_start = s + pos;

    /* Special single-char delimiters */
    if (s[pos] == '{' || s[pos] == '}' || s[pos] == '[' || s[pos] == ']') {
        return 1;
    }

    /* String literal */
    if (s[pos] == '(') {
        int depth = 1;
        size_t start = pos++;
        while (pos < len && depth > 0) {
            if (s[pos] == '\\') { pos += 2; continue; }
            if (s[pos] == '(') depth++;
            if (s[pos] == ')') depth--;
            pos++;
        }
        return pos - start;
    }

    /* Hex string */
    if (s[pos] == '<') {
        size_t start = pos++;
        while (pos < len && s[pos] != '>') pos++;
        if (pos < len) pos++; /* skip '>' */
        return pos - start;
    }

    /* Regular token: delimited by whitespace or special chars */
    size_t start = pos;
    while (pos < len && s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' &&
           s[pos] != '\r' && s[pos] != '\f' && s[pos] != '{' && s[pos] != '}' &&
           s[pos] != '[' && s[pos] != ']' && s[pos] != '(' && s[pos] != ')' &&
           s[pos] != '<' && s[pos] != '>' && s[pos] != '%') {
        pos++;
    }
    return pos - start;
}

/* Check if a token matches a string */
static bool tok_eq(const char *tok, size_t tok_len, const char *target)
{
    size_t tgt_len = strlen(target);
    return (tok_len == tgt_len && memcmp(tok, target, tgt_len) == 0);
}

/* Parse a number from a token */
static double tok_to_double(const char *tok, size_t tok_len)
{
    char buf[64];
    size_t n = (tok_len < 63) ? tok_len : 63;
    memcpy(buf, tok, n);
    buf[n] = '\0';
    return atof(buf);
}

static int tok_to_int(const char *tok, size_t tok_len)
{
    return (int)tok_to_double(tok, tok_len);
}

/* ─── Parse ASCII segment ─── */

typedef struct {
    char font_name[256];
    double font_matrix[6];
    double font_bbox[4];
    /* Encoding: glyph name per code */
    char encoding_names[256][64];
    bool has_custom_encoding;
    bool uses_standard_encoding;
} T1AsciiInfo;

static void parse_ascii_segment(const char *ascii, size_t ascii_len, T1AsciiInfo *info)
{
    memset(info, 0, sizeof(*info));
    /* Default FontMatrix is [0.001 0 0 0.001 0 0] */
    info->font_matrix[0] = 0.001;
    info->font_matrix[3] = 0.001;

    size_t pos = 0;
    while (pos < ascii_len) {
        const char *tok;
        size_t tok_len = next_token(ascii, pos, ascii_len, &tok);
        if (tok_len == 0) break;
        pos = (size_t)(tok - ascii) + tok_len;

        /* /FontName */
        if (tok_len >= 9 && memcmp(tok, "/FontName", 9) == 0) {
            const char *next;
            size_t nlen = next_token(ascii, pos, ascii_len, &next);
            if (nlen > 0) {
                pos = (size_t)(next - ascii) + nlen;
                /* Name is usually /SomeName */
                const char *name = next;
                size_t name_len = nlen;
                if (name[0] == '/') { name++; name_len--; }
                if (name_len > 255) name_len = 255;
                memcpy(info->font_name, name, name_len);
                info->font_name[name_len] = '\0';
            }
        }

        /* /FontMatrix */
        if (tok_len >= 11 && memcmp(tok, "/FontMatrix", 11) == 0) {
            /* Expect [ or { followed by 6 numbers */
            const char *next;
            size_t nlen = next_token(ascii, pos, ascii_len, &next);
            if (nlen > 0) {
                pos = (size_t)(next - ascii) + nlen;
                /* Skip '[' or '{' */
                for (int i = 0; i < 6; i++) {
                    nlen = next_token(ascii, pos, ascii_len, &next);
                    if (nlen == 0) break;
                    pos = (size_t)(next - ascii) + nlen;
                    info->font_matrix[i] = tok_to_double(next, nlen);
                }
            }
        }

        /* /FontBBox */
        if (tok_len >= 9 && memcmp(tok, "/FontBBox", 9) == 0) {
            const char *next;
            size_t nlen = next_token(ascii, pos, ascii_len, &next);
            if (nlen > 0) {
                pos = (size_t)(next - ascii) + nlen;
                /* Skip '{' or '[' */
                for (int i = 0; i < 4; i++) {
                    nlen = next_token(ascii, pos, ascii_len, &next);
                    if (nlen == 0) break;
                    pos = (size_t)(next - ascii) + nlen;
                    info->font_bbox[i] = tok_to_double(next, nlen);
                }
            }
        }

        /* /Encoding */
        if (tok_len >= 9 && memcmp(tok, "/Encoding", 9) == 0) {
            /* Check what follows: StandardEncoding or a custom encoding */
            const char *next;
            size_t nlen = next_token(ascii, pos, ascii_len, &next);
            if (nlen == 0) continue;

            if (tok_eq(next, nlen, "StandardEncoding")) {
                info->uses_standard_encoding = true;
                pos = (size_t)(next - ascii) + nlen;
            } else {
                /* Custom encoding - expect: 256 array ... */
                /* Parse the encoding array.
                 * Format is typically:
                 *   /Encoding 256 array
                 *   0 1 255 {1 index exch /.notdef put} for
                 *   dup <code> /<name> put
                 *   ...
                 *   readonly def
                 */
                info->has_custom_encoding = true;
                /* We already read the first token (might be "256") */
                pos = (size_t)(next - ascii) + nlen;

                /* Scan for "dup <code> /<name> put" sequences */
                while (pos < ascii_len) {
                    nlen = next_token(ascii, pos, ascii_len, &next);
                    if (nlen == 0) break;
                    pos = (size_t)(next - ascii) + nlen;

                    /* Stop at "def" or "readonly" (end of encoding) */
                    if (tok_eq(next, nlen, "def") || tok_eq(next, nlen, "readonly")) {
                        break;
                    }

                    /* Look for "dup" */
                    if (tok_eq(next, nlen, "dup")) {
                        /* Read code number */
                        const char *code_tok;
                        size_t code_len = next_token(ascii, pos, ascii_len, &code_tok);
                        if (code_len == 0) break;
                        pos = (size_t)(code_tok - ascii) + code_len;
                        int code = tok_to_int(code_tok, code_len);

                        /* Read glyph name (starts with /) */
                        const char *name_tok;
                        size_t name_len = next_token(ascii, pos, ascii_len, &name_tok);
                        if (name_len == 0) break;
                        pos = (size_t)(name_tok - ascii) + name_len;

                        if (name_tok[0] == '/' && code >= 0 && code < 256) {
                            size_t copy_len = name_len - 1;
                            if (copy_len > 63) copy_len = 63;
                            memcpy(info->encoding_names[code], name_tok + 1, copy_len);
                            info->encoding_names[code][copy_len] = '\0';
                        }

                        /* Skip "put" */
                        const char *put_tok;
                        size_t put_len = next_token(ascii, pos, ascii_len, &put_tok);
                        if (put_len > 0) pos = (size_t)(put_tok - ascii) + put_len;
                    }
                }
            }
        }
    }
}

/* ─── Parse binary (eexec-decrypted) segment ─── */

typedef struct {
    int lenIV;  /* charstring encryption padding, default 4 */

    /* Subroutines */
    uint8_t *subr_data[T1_MAX_SUBRS];
    size_t   subr_lens[T1_MAX_SUBRS];
    int      subr_count;

    /* CharStrings */
    char     cs_names[T1_MAX_CHARSTRINGS][64];
    uint8_t *cs_data[T1_MAX_CHARSTRINGS];
    size_t   cs_lens[T1_MAX_CHARSTRINGS];
    int      cs_count;
} T1BinaryInfo;

/*
 * Find a binary pattern in data.
 */
static const uint8_t *memmem_simple(const uint8_t *hay, size_t hay_len,
                                      const char *needle, size_t needle_len)
{
    if (needle_len > hay_len) return NULL;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }
    return NULL;
}

/*
 * Read a binary integer (big-endian) from charstring data.
 * Used when parsing RD/- data sections.
 */

/* Skip whitespace in the binary data (after eexec decryption it's like PostScript text) */
static size_t skip_ws_bin(const uint8_t *d, size_t pos, size_t len)
{
    while (pos < len) {
        uint8_t c = d[pos];
        if (c == '%') {
            while (pos < len && d[pos] != '\n' && d[pos] != '\r') pos++;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            pos++;
        } else {
            break;
        }
    }
    return pos;
}

static size_t next_token_bin(const uint8_t *d, size_t pos, size_t len,
                              const uint8_t **out_start)
{
    pos = skip_ws_bin(d, pos, len);
    if (pos >= len) {
        *out_start = d + pos;
        return 0;
    }

    *out_start = d + pos;
    size_t start = pos;

    /* Regular token */
    while (pos < len && d[pos] != ' ' && d[pos] != '\t' && d[pos] != '\n' &&
           d[pos] != '\r' && d[pos] != '\f' && d[pos] != '{' && d[pos] != '}' &&
           d[pos] != '[' && d[pos] != ']' && d[pos] != '(' && d[pos] != ')' &&
           d[pos] != '%') {
        pos++;
    }

    if (pos == start) {
        /* Single char delimiter */
        return 1;
    }

    return pos - start;
}

static bool bin_tok_eq(const uint8_t *tok, size_t tok_len, const char *target)
{
    size_t tgt_len = strlen(target);
    return (tok_len == tgt_len && memcmp(tok, target, tgt_len) == 0);
}

static int bin_tok_to_int(const uint8_t *tok, size_t tok_len)
{
    char buf[32];
    size_t n = (tok_len < 31) ? tok_len : 31;
    memcpy(buf, tok, n);
    buf[n] = '\0';
    return atoi(buf);
}

static void parse_binary_segment(uint8_t *binary, size_t binary_len, T1BinaryInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->lenIV = 4; /* default */

    /* The binary segment is now decrypted eexec data, essentially PostScript text.
     * We need to find:
     *   /lenIV <n> def
     *   /Subrs <n> array
     *     dup <index> <length> RD <binary_data> NP
     *     ...
     *   /CharStrings <n> dict ... begin
     *     /<name> <length> RD <binary_data> ND
     *     ...
     *   end
     */

    /* First pass: find /lenIV */
    {
        const uint8_t *p = memmem_simple(binary, binary_len, "/lenIV", 6);
        if (p) {
            size_t lpos = (size_t)(p - binary) + 6;
            const uint8_t *tok;
            size_t tlen = next_token_bin(binary, lpos, binary_len, &tok);
            if (tlen > 0) {
                info->lenIV = bin_tok_to_int(tok, tlen);
            }
        }
    }

    /* Find /Subrs */
    {
        const uint8_t *p = memmem_simple(binary, binary_len, "/Subrs", 6);
        if (p) {
            size_t spos = (size_t)(p - binary) + 6;
            const uint8_t *tok;
            size_t tlen = next_token_bin(binary, spos, binary_len, &tok);
            if (tlen > 0) {
                int num_subrs = bin_tok_to_int(tok, tlen);
                if (num_subrs > T1_MAX_SUBRS) num_subrs = T1_MAX_SUBRS;
                spos = (size_t)(tok - binary) + tlen;

                /* Skip "array" token */
                tlen = next_token_bin(binary, spos, binary_len, &tok);
                if (tlen > 0) spos = (size_t)(tok - binary) + tlen;

                /* Parse subroutines: dup <index> <length> RD <data> NP */
                while (spos < binary_len) {
                    tlen = next_token_bin(binary, spos, binary_len, &tok);
                    if (tlen == 0) break;
                    spos = (size_t)(tok - binary) + tlen;

                    /* Stop at "end", "def", or next "/" that's not "dup" */
                    if (bin_tok_eq(tok, tlen, "end") ||
                        bin_tok_eq(tok, tlen, "def") ||
                        bin_tok_eq(tok, tlen, "readonly")) {
                        break;
                    }

                    /* Check for /CharStrings to stop subroutine parsing */
                    if (tlen > 1 && tok[0] == '/' &&
                        !bin_tok_eq(tok, tlen, "/Subrs")) {
                        /* Rewind so the main loop finds /CharStrings */
                        spos = (size_t)(tok - binary);
                        break;
                    }

                    if (bin_tok_eq(tok, tlen, "dup")) {
                        /* Read index */
                        tlen = next_token_bin(binary, spos, binary_len, &tok);
                        if (tlen == 0) break;
                        spos = (size_t)(tok - binary) + tlen;
                        int subr_idx = bin_tok_to_int(tok, tlen);

                        /* Read length */
                        tlen = next_token_bin(binary, spos, binary_len, &tok);
                        if (tlen == 0) break;
                        spos = (size_t)(tok - binary) + tlen;
                        int subr_len = bin_tok_to_int(tok, tlen);

                        /* Read RD or -| (the data command) */
                        tlen = next_token_bin(binary, spos, binary_len, &tok);
                        if (tlen == 0) break;
                        spos = (size_t)(tok - binary) + tlen;

                        /* After RD/-, skip exactly one space/newline then read binary data */
                        if (spos < binary_len &&
                            (binary[spos] == ' ' || binary[spos] == '\n' || binary[spos] == '\r'))
                            spos++;

                        if (subr_len > 0 && spos + (size_t)subr_len <= binary_len &&
                            subr_idx >= 0 && subr_idx < T1_MAX_SUBRS) {
                            /* Copy and decrypt the charstring */
                            uint8_t *cs = (uint8_t *)malloc(subr_len);
                            if (cs) {
                                memcpy(cs, binary + spos, subr_len);
                                charstring_decrypt(cs, subr_len);
                                info->subr_data[subr_idx] = cs;
                                info->subr_lens[subr_idx] = subr_len;
                                if (subr_idx >= info->subr_count)
                                    info->subr_count = subr_idx + 1;
                            }
                        }
                        spos += subr_len;

                        /* Skip NP, noaccess put, |, etc. */
                        tlen = next_token_bin(binary, spos, binary_len, &tok);
                        if (tlen > 0) spos = (size_t)(tok - binary) + tlen;
                    }
                }
            }
        }
    }

    /* Find /CharStrings */
    {
        const uint8_t *p = memmem_simple(binary, binary_len, "/CharStrings", 12);
        if (!p) return;

        size_t cpos = (size_t)(p - binary) + 12;

        /* Read count */
        const uint8_t *tok;
        size_t tlen = next_token_bin(binary, cpos, binary_len, &tok);
        if (tlen == 0) return;
        /* int num_cs = bin_tok_to_int(tok, tlen); -- not needed */
        cpos = (size_t)(tok - binary) + tlen;

        /* Skip "dict dup begin" or "dict begin" */
        for (int skip = 0; skip < 5; skip++) {
            tlen = next_token_bin(binary, cpos, binary_len, &tok);
            if (tlen == 0) break;
            cpos = (size_t)(tok - binary) + tlen;
            if (bin_tok_eq(tok, tlen, "begin")) break;
        }

        /* Parse charstrings: /<name> <length> RD <data> ND */
        while (cpos < binary_len && info->cs_count < T1_MAX_CHARSTRINGS) {
            tlen = next_token_bin(binary, cpos, binary_len, &tok);
            if (tlen == 0) break;
            cpos = (size_t)(tok - binary) + tlen;

            /* Stop at "end" */
            if (bin_tok_eq(tok, tlen, "end")) break;

            /* Glyph name starts with / */
            if (tlen > 1 && tok[0] == '/') {
                int idx = info->cs_count;
                size_t name_len = tlen - 1;
                if (name_len > 63) name_len = 63;
                memcpy(info->cs_names[idx], tok + 1, name_len);
                info->cs_names[idx][name_len] = '\0';

                /* Read length */
                tlen = next_token_bin(binary, cpos, binary_len, &tok);
                if (tlen == 0) break;
                cpos = (size_t)(tok - binary) + tlen;
                int cs_len = bin_tok_to_int(tok, tlen);

                /* Read RD or -| */
                tlen = next_token_bin(binary, cpos, binary_len, &tok);
                if (tlen == 0) break;
                cpos = (size_t)(tok - binary) + tlen;

                /* Skip one whitespace byte after RD */
                if (cpos < binary_len &&
                    (binary[cpos] == ' ' || binary[cpos] == '\n' || binary[cpos] == '\r'))
                    cpos++;

                if (cs_len > 0 && cpos + (size_t)cs_len <= binary_len) {
                    uint8_t *cs = (uint8_t *)malloc(cs_len);
                    if (cs) {
                        memcpy(cs, binary + cpos, cs_len);
                        charstring_decrypt(cs, cs_len);
                        info->cs_data[idx] = cs;
                        info->cs_lens[idx] = cs_len;
                        info->cs_count++;
                    }
                }
                cpos += cs_len;

                /* Skip ND, noaccess def, |-, etc. */
                tlen = next_token_bin(binary, cpos, binary_len, &tok);
                if (tlen > 0) cpos = (size_t)(tok - binary) + tlen;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Type 1 Charstring Interpreter
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* Operand stack */
    double   stack[T1_MAX_STACK];
    int      sp;

    /* Current point */
    double   x, y;

    /* Path state */
    bool     path_open;
    bool     have_width;
    double   width;

    /* Sidebearing */
    double   sbx, sby;

    /* Flex hint protocol */
    bool     in_flex;
    int      flex_count;
    double   flex_pts[14]; /* 7 points * 2 coords */

    /* PostScript operand stack for callothersubr/pop */
    double   ps_stack[T1_MAX_OTHER_ARGS];
    int      ps_sp;

    /* Subroutine call depth */
    int      call_depth;

    /* Font reference */
    ParsedFont *font;

    /* Output */
    GlyphOutline *outline;
} T1Interp;

/* Forward declaration */
static bool t1_execute(T1Interp *interp, const uint8_t *data, size_t len);

static void t1_close_path(T1Interp *interp)
{
    if (interp->path_open) {
        GlyphCmd cmd = {0};
        cmd.type = GLYPH_CLOSE;
        glyph_outline_add(interp->outline, cmd);
        interp->path_open = false;
    }
}

static void t1_moveto(T1Interp *interp, double x, double y)
{
    /* Close previous subpath if open */
    t1_close_path(interp);

    interp->x = x;
    interp->y = y;

    GlyphCmd cmd = {0};
    cmd.type = GLYPH_MOVETO;
    cmd.x = interp->x;
    cmd.y = interp->y;
    glyph_outline_add(interp->outline, cmd);

    interp->path_open = true;
}

static void t1_rmoveto(T1Interp *interp, double dx, double dy)
{
    t1_moveto(interp, interp->x + dx, interp->y + dy);
}

static void t1_lineto(T1Interp *interp, double dx, double dy)
{
    interp->x += dx;
    interp->y += dy;

    GlyphCmd cmd = {0};
    cmd.type = GLYPH_LINETO;
    cmd.x = interp->x;
    cmd.y = interp->y;
    glyph_outline_add(interp->outline, cmd);
}

static void t1_curveto(T1Interp *interp,
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
 * Execute a Type 1 charstring.
 */
static bool t1_execute(T1Interp *interp, const uint8_t *data, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        uint8_t b0 = data[pos++];

        /* ── Number encoding ── */
        if (b0 >= 32 && b0 <= 246) {
            if (interp->sp < T1_MAX_STACK)
                interp->stack[interp->sp++] = (double)(b0 - 139);
            continue;
        }
        if (b0 >= 247 && b0 <= 250) {
            if (pos >= len) return false;
            uint8_t b1 = data[pos++];
            if (interp->sp < T1_MAX_STACK)
                interp->stack[interp->sp++] = (double)((b0 - 247) * 256 + b1 + 108);
            continue;
        }
        if (b0 >= 251 && b0 <= 254) {
            if (pos >= len) return false;
            uint8_t b1 = data[pos++];
            if (interp->sp < T1_MAX_STACK)
                interp->stack[interp->sp++] = (double)(-(b0 - 251) * 256 - b1 - 108);
            continue;
        }
        if (b0 == 255) {
            /* 4-byte signed integer (NOT fixed-point like Type2!) */
            if (pos + 3 >= len) return false;
            int32_t v = (int32_t)(((uint32_t)data[pos] << 24) |
                                  ((uint32_t)data[pos+1] << 16) |
                                  ((uint32_t)data[pos+2] << 8) |
                                  (uint32_t)data[pos+3]);
            pos += 4;
            if (interp->sp < T1_MAX_STACK)
                interp->stack[interp->sp++] = (double)v;
            continue;
        }

        /* ── Operators ── */

        if (b0 == 12) {
            /* Two-byte operator */
            if (pos >= len) return false;
            uint8_t b1 = data[pos++];

            switch (b1) {
                case 0:  /* dotsection - ignored */
                    interp->sp = 0;
                    break;

                case 1:  /* vstem3: x0 dx0 x1 dx1 x2 dx2 */
                    interp->sp = 0;
                    break;

                case 2:  /* hstem3: y0 dy0 y1 dy1 y2 dy2 */
                    interp->sp = 0;
                    break;

                case 6: {
                    /* seac: asb adx ady bchar achar
                     * Composite character: render base char at (0,0) + sidebearing,
                     * then accent char at (adx, ady).
                     * bchar and achar are character codes in StandardEncoding. */
                    if (interp->sp >= 5) {
                        double asb  = interp->stack[0];
                        double adx  = interp->stack[1];
                        double ady  = interp->stack[2];
                        int    bchar = (int)interp->stack[3];
                        int    achar = (int)interp->stack[4];

                        /* Find bchar glyph name from StandardEncoding */
                        const char *bname = (bchar >= 0 && bchar < 256) ?
                                            standard_encoding_names[bchar] : NULL;
                        const char *aname = (achar >= 0 && achar < 256) ?
                                            standard_encoding_names[achar] : NULL;

                        /* Render base character */
                        if (bname) {
                            int bgid = t1_find_gid_by_name(interp->font, bname);
                            if (bgid >= 0 && interp->call_depth < T1_MAX_CALL_DEPTH) {
                                interp->x = 0;
                                interp->y = 0;

                                /* Execute base char */
                                int lenIV_skip = interp->font->t1_lenIV;
                                const uint8_t *bdata = interp->font->t1_charstrings_data +
                                                        interp->font->t1_charstrings_offs[bgid];
                                size_t blen = interp->font->t1_charstrings_lens[bgid];
                                if (blen > (size_t)lenIV_skip) {
                                    interp->call_depth++;
                                    t1_execute(interp, bdata + lenIV_skip, blen - lenIV_skip);
                                    interp->call_depth--;
                                }
                            }
                        }

                        /* Render accent character */
                        if (aname) {
                            int agid = t1_find_gid_by_name(interp->font, aname);
                            if (agid >= 0 && interp->call_depth < T1_MAX_CALL_DEPTH) {
                                /* Position accent relative to base */
                                interp->x = adx + interp->sbx - asb;
                                interp->y = ady;

                                int lenIV_skip = interp->font->t1_lenIV;
                                const uint8_t *adata = interp->font->t1_charstrings_data +
                                                        interp->font->t1_charstrings_offs[agid];
                                size_t alen = interp->font->t1_charstrings_lens[agid];
                                if (alen > (size_t)lenIV_skip) {
                                    interp->call_depth++;
                                    t1_execute(interp, adata + lenIV_skip, alen - lenIV_skip);
                                    interp->call_depth--;
                                }
                            }
                        }
                    }
                    interp->sp = 0;
                    return true; /* seac is like endchar */
                }

                case 7: {
                    /* sbw: sbx sby wx wy */
                    if (interp->sp >= 4) {
                        interp->sbx = interp->stack[0];
                        interp->sby = interp->stack[1];
                        interp->width = interp->stack[2];
                        /* wy = interp->stack[3] - vertical width, ignored */
                        interp->have_width = true;
                        interp->x = interp->sbx;
                        interp->y = interp->sby;
                    }
                    interp->sp = 0;
                    break;
                }

                case 12: {
                    /* div: num1 num2 div -> result */
                    if (interp->sp >= 2) {
                        double num2 = interp->stack[--interp->sp];
                        double num1 = interp->stack[--interp->sp];
                        if (num2 != 0.0) {
                            if (interp->sp < T1_MAX_STACK)
                                interp->stack[interp->sp++] = num1 / num2;
                        }
                    }
                    break;
                }

                case 16: {
                    /* callothersubr: arg1..argn n othersubr# callothersubr
                     *
                     * Standard othersubrs:
                     *   0 = end flex
                     *   1 = start flex
                     *   2 = add flex point
                     *   3 = hint replacement (push current point)
                     */
                    if (interp->sp >= 2) {
                        int othersubr = (int)interp->stack[--interp->sp];
                        int n_args = (int)interp->stack[--interp->sp];

                        switch (othersubr) {
                            case 0: {
                                /* End flex: args are fd (flex depth) on the stack,
                                 * plus the 7 points collected during flex.
                                 * We emit two cubic curves. */
                                interp->in_flex = false;

                                /* Pop fd argument from stack */
                                if (interp->sp >= 1) interp->sp--;

                                /* The flex points are:
                                 *  0: reference point (start)
                                 *  1-3: first curve control points
                                 *  4-6: second curve control points
                                 * Each stored as (x, y) absolute coordinates. */
                                if (interp->flex_count >= 7) {
                                    /* flex_pts[0,1] = reference point (not used in curves) */
                                    double x1 = interp->flex_pts[2];
                                    double y1 = interp->flex_pts[3];
                                    double x2 = interp->flex_pts[4];
                                    double y2 = interp->flex_pts[5];
                                    double x3 = interp->flex_pts[6];
                                    double y3 = interp->flex_pts[7];
                                    double x4 = interp->flex_pts[8];
                                    double y4 = interp->flex_pts[9];
                                    double x5 = interp->flex_pts[10];
                                    double y5 = interp->flex_pts[11];
                                    double x6 = interp->flex_pts[12];
                                    double y6 = interp->flex_pts[13];

                                    /* First curve: cp1=(x1,y1) cp2=(x2,y2) end=(x3,y3) */
                                    {
                                        GlyphCmd cmd = {0};
                                        cmd.type = GLYPH_CURVETO;
                                        cmd.cx1 = x1; cmd.cy1 = y1;
                                        cmd.cx2 = x2; cmd.cy2 = y2;
                                        cmd.x = x3; cmd.y = y3;
                                        glyph_outline_add(interp->outline, cmd);
                                    }

                                    /* Second curve: cp1=(x4,y4) cp2=(x5,y5) end=(x6,y6) */
                                    {
                                        GlyphCmd cmd = {0};
                                        cmd.type = GLYPH_CURVETO;
                                        cmd.cx1 = x4; cmd.cy1 = y4;
                                        cmd.cx2 = x5; cmd.cy2 = y5;
                                        cmd.x = x6; cmd.y = y6;
                                        glyph_outline_add(interp->outline, cmd);
                                    }

                                    interp->x = x6;
                                    interp->y = y6;
                                }

                                /* Push results to PS stack (epx, epy for setcurrentpoint) */
                                if (interp->ps_sp < T1_MAX_OTHER_ARGS)
                                    interp->ps_stack[interp->ps_sp++] = interp->x;
                                if (interp->ps_sp < T1_MAX_OTHER_ARGS)
                                    interp->ps_stack[interp->ps_sp++] = interp->y;
                                break;
                            }

                            case 1:
                                /* Start flex: initialize flex point collection */
                                interp->in_flex = true;
                                interp->flex_count = 0;
                                break;

                            case 2:
                                /* Add flex point: record current position */
                                if (interp->flex_count < 7) {
                                    interp->flex_pts[interp->flex_count * 2] = interp->x;
                                    interp->flex_pts[interp->flex_count * 2 + 1] = interp->y;
                                    interp->flex_count++;
                                }
                                break;

                            case 3:
                                /* Hint replacement: push current point to PS stack */
                                if (interp->ps_sp < T1_MAX_OTHER_ARGS)
                                    interp->ps_stack[interp->ps_sp++] = interp->x;
                                break;

                            default:
                                /* Unknown othersubr: pop n_args from stack to PS stack */
                                for (int i = 0; i < n_args && interp->sp > 0; i++) {
                                    double val = interp->stack[--interp->sp];
                                    if (interp->ps_sp < T1_MAX_OTHER_ARGS)
                                        interp->ps_stack[interp->ps_sp++] = val;
                                }
                                break;
                        }
                    }
                    break;
                }

                case 17: {
                    /* pop: push a value from the PS stack onto the charstring stack */
                    if (interp->ps_sp > 0 && interp->sp < T1_MAX_STACK) {
                        interp->stack[interp->sp++] = interp->ps_stack[--interp->ps_sp];
                    }
                    break;
                }

                case 33: {
                    /* setcurrentpoint: x y setcurrentpoint */
                    if (interp->sp >= 2) {
                        interp->x = interp->stack[0];
                        interp->y = interp->stack[1];
                    }
                    interp->sp = 0;
                    break;
                }

                default:
                    /* Unknown 2-byte operator: clear stack */
                    interp->sp = 0;
                    break;
            }
            continue;
        }

        /* Single-byte operators */
        switch (b0) {
            case 1:   /* hstem: y dy */
                interp->sp = 0;
                break;

            case 3:   /* vstem: x dx */
                interp->sp = 0;
                break;

            case 4:   /* vmoveto: dy */
                if (interp->sp >= 1) {
                    if (interp->in_flex) {
                        /* During flex, movetos just update position for flex points */
                        interp->x += 0;
                        interp->y += interp->stack[0];
                    } else {
                        t1_rmoveto(interp, 0, interp->stack[0]);
                    }
                }
                interp->sp = 0;
                break;

            case 5:   /* rlineto: dx dy */
                if (interp->sp >= 2) {
                    t1_lineto(interp, interp->stack[0], interp->stack[1]);
                }
                interp->sp = 0;
                break;

            case 6:   /* hlineto: dx */
                if (interp->sp >= 1) {
                    t1_lineto(interp, interp->stack[0], 0);
                }
                interp->sp = 0;
                break;

            case 7:   /* vlineto: dy */
                if (interp->sp >= 1) {
                    t1_lineto(interp, 0, interp->stack[0]);
                }
                interp->sp = 0;
                break;

            case 8:   /* rrcurveto: dx1 dy1 dx2 dy2 dx3 dy3 */
                if (interp->sp >= 6) {
                    t1_curveto(interp,
                        interp->stack[0], interp->stack[1],
                        interp->stack[2], interp->stack[3],
                        interp->stack[4], interp->stack[5]);
                }
                interp->sp = 0;
                break;

            case 9:   /* closepath */
                t1_close_path(interp);
                interp->sp = 0;
                break;

            case 10: {
                /* callsubr: subr# */
                if (interp->sp < 1) break;
                int subr_idx = (int)interp->stack[--interp->sp];

                if (interp->call_depth >= T1_MAX_CALL_DEPTH) break;
                if (subr_idx < 0 || subr_idx >= interp->font->t1_subrs_count) break;

                size_t slen = interp->font->t1_subrs_lens[subr_idx];
                int skip = interp->font->t1_lenIV;
                if (slen <= (size_t)skip) break;

                const uint8_t *sdata = interp->font->t1_subrs_data +
                                        interp->font->t1_subrs_offs[subr_idx];

                interp->call_depth++;
                t1_execute(interp, sdata + skip, slen - skip);
                interp->call_depth--;
                break;
            }

            case 11:  /* return */
                return true;

            case 13: {
                /* hsbw: sbx wx */
                if (interp->sp >= 2) {
                    interp->sbx = interp->stack[0];
                    interp->sby = 0;
                    interp->width = interp->stack[1];
                    interp->have_width = true;
                    interp->x = interp->sbx;
                    interp->y = 0;
                }
                interp->sp = 0;
                break;
            }

            case 14:  /* endchar */
                t1_close_path(interp);
                interp->sp = 0;
                return true;

            case 21:  /* rmoveto: dx dy */
                if (interp->sp >= 2) {
                    if (interp->in_flex) {
                        interp->x += interp->stack[0];
                        interp->y += interp->stack[1];
                    } else {
                        t1_rmoveto(interp, interp->stack[0], interp->stack[1]);
                    }
                }
                interp->sp = 0;
                break;

            case 22:  /* hmoveto: dx */
                if (interp->sp >= 1) {
                    if (interp->in_flex) {
                        interp->x += interp->stack[0];
                        interp->y += 0;
                    } else {
                        t1_rmoveto(interp, interp->stack[0], 0);
                    }
                }
                interp->sp = 0;
                break;

            case 30: {
                /* vhcurveto: dy1 dx2 dy2 dx3 */
                if (interp->sp >= 4) {
                    t1_curveto(interp,
                        0, interp->stack[0],
                        interp->stack[1], interp->stack[2],
                        interp->stack[3], 0);
                }
                interp->sp = 0;
                break;
            }

            case 31: {
                /* hvcurveto: dx1 dx2 dy2 dy3 */
                if (interp->sp >= 4) {
                    t1_curveto(interp,
                        interp->stack[0], 0,
                        interp->stack[1], interp->stack[2],
                        0, interp->stack[3]);
                }
                interp->sp = 0;
                break;
            }

            default:
                /* Unknown operator: clear stack */
                interp->sp = 0;
                break;
        }
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Get a glyph outline by GID for a Type1 font.
 */
bool t1_get_glyph_by_gid(ParsedFont *font, int gid, GlyphOutline *outline)
{
    if (!font || !outline || !font->is_type1) return false;
    if (gid < 0 || gid >= font->t1_num_charstrings) return false;

    glyph_outline_init(outline);

    size_t cs_off = font->t1_charstrings_offs[gid];
    size_t cs_len = font->t1_charstrings_lens[gid];
    if (cs_len == 0) return false;

    int skip = font->t1_lenIV;
    if (cs_len <= (size_t)skip) return false;

    const uint8_t *cs_data = font->t1_charstrings_data + cs_off;

    T1Interp interp;
    memset(&interp, 0, sizeof(interp));
    interp.font = font;
    interp.outline = outline;

    bool ok = t1_execute(&interp, cs_data + skip, cs_len - skip);

    if (interp.path_open)
        t1_close_path(&interp);

    outline->advance_width = interp.width;

    /* Cache width */
    if (font->glyph_widths && gid < font->num_glyphs)
        font->glyph_widths[gid] = interp.width;

    return ok;
}

/*
 * Find GID by glyph name for a Type1 font.
 */
int t1_find_gid_by_name(ParsedFont *font, const char *glyph_name)
{
    if (!font || !glyph_name || !font->is_type1) return -1;
    if (!font->t1_glyph_names) return -1;

    for (int i = 0; i < font->t1_num_charstrings; i++) {
        if (font->t1_glyph_names[i] &&
            strcmp(font->t1_glyph_names[i], glyph_name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Parse a Type1 font from PFB or PFA data.
 */
ParsedFont *parsed_font_from_type1(const uint8_t *data, size_t len)
{
    if (!data || len < 4) return NULL;

    PfbSegments seg;
    memset(&seg, 0, sizeof(seg));

    bool is_pfb = (data[0] == 0x80 && (data[1] == 0x01 || data[1] == 0x02));
    bool is_pfa = (len > 2 && data[0] == '%' && data[1] == '!');

    if (is_pfb) {
        if (!extract_pfb_segments(data, len, &seg)) {
            free(seg.ascii_data);
            free(seg.binary_data);
            return NULL;
        }
    } else if (is_pfa) {
        /* PFA: find "eexec" to split ASCII from binary part.
         * In PDF /FontFile streams, the binary part is typically raw binary
         * (not hex-encoded). In standalone PFA files, it's hex-encoded.
         * We detect which by checking if the bytes after "eexec" are hex chars. */
        const uint8_t *eexec_ptr = memmem_simple(data, len, "eexec", 5);
        if (!eexec_ptr) {
            return NULL;
        }

        /* ASCII part is everything before eexec */
        size_t ascii_end = (size_t)(eexec_ptr - data);
        seg.ascii_data = (uint8_t *)malloc(ascii_end + 1);
        if (!seg.ascii_data) return NULL;
        memcpy(seg.ascii_data, data, ascii_end);
        seg.ascii_len = ascii_end;

        /* Binary part: skip "eexec" and whitespace */
        size_t bin_start = (size_t)(eexec_ptr - data) + 5;
        while (bin_start < len && (data[bin_start] == ' ' || data[bin_start] == '\n' ||
               data[bin_start] == '\r' || data[bin_start] == '\t'))
            bin_start++;

        /* Detect hex vs raw binary: check if first few non-whitespace bytes are hex chars */
        bool is_hex_encoded = true;
        for (int probe = 0; probe < 8 && bin_start + probe < len; probe++) {
            uint8_t c = data[bin_start + probe];
            if (hex_val(c) < 0 && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                is_hex_encoded = false;
                break;
            }
        }

        if (is_hex_encoded) {
            /* Hex-encoded binary (standalone PFA file) */
            seg.binary_data = (uint8_t *)malloc((len - bin_start) / 2 + 1);
            if (!seg.binary_data) {
                free(seg.ascii_data);
                return NULL;
            }
            seg.binary_len = 0;

            for (size_t i = bin_start; i + 1 < len; ) {
                while (i < len && (data[i] == ' ' || data[i] == '\n' || data[i] == '\r' ||
                       data[i] == '\t'))
                    i++;
                if (i + 1 >= len) break;

                int hi = hex_val(data[i]);
                int lo = hex_val(data[i+1]);
                if (hi < 0 || lo < 0) break;
                seg.binary_data[seg.binary_len++] = (uint8_t)((hi << 4) | lo);
                i += 2;
            }
        } else {
            /* Raw binary (PDF /FontFile stream) */
            size_t raw_len = len - bin_start;
            /* Strip trailing cleartext section (zeros followed by "cleartomark") */
            /* Look for the pattern of 512 zeros (0x00) which marks the start of
             * the cleartext trailer */
            size_t end_pos = raw_len;
            for (size_t i = 0; i + 1 < raw_len; i++) {
                if (data[bin_start + i] == '0' && data[bin_start + i + 1] == '0') {
                    /* Check if this is the start of the "0000..." cleartext trailer */
                    bool all_zeros = true;
                    for (size_t j = i; j < i + 16 && j < raw_len; j++) {
                        if (data[bin_start + j] != '0' &&
                            data[bin_start + j] != '\n' &&
                            data[bin_start + j] != '\r') {
                            all_zeros = false;
                            break;
                        }
                    }
                    if (all_zeros && (raw_len - i) > 32) {
                        end_pos = i;
                        break;
                    }
                }
            }

            seg.binary_data = (uint8_t *)malloc(end_pos + 1);
            if (!seg.binary_data) {
                free(seg.ascii_data);
                return NULL;
            }
            memcpy(seg.binary_data, data + bin_start, end_pos);
            seg.binary_len = end_pos;
        }
    } else {
        return NULL;
    }

    /* Parse ASCII segment */
    T1AsciiInfo ascii_info;
    parse_ascii_segment((const char *)seg.ascii_data, seg.ascii_len, &ascii_info);

    /* Decrypt binary segment (eexec decryption) */
    eexec_decrypt(seg.binary_data, seg.binary_len);

    /* Parse binary segment (skip first 4 bytes of random padding) */
    T1BinaryInfo bin_info;
    uint8_t *bin_start = seg.binary_data + 4;
    size_t bin_len = (seg.binary_len > 4) ? seg.binary_len - 4 : 0;
    parse_binary_segment(bin_start, bin_len, &bin_info);

    /* Check if we got any charstrings */
    if (bin_info.cs_count == 0) {
        /* Cleanup */
        for (int i = 0; i < bin_info.subr_count; i++)
            free(bin_info.subr_data[i]);
        for (int i = 0; i < bin_info.cs_count; i++)
            free(bin_info.cs_data[i]);
        free(seg.ascii_data);
        free(seg.binary_data);
        return NULL;
    }

    /* Build ParsedFont */
    ParsedFont *font = (ParsedFont *)calloc(1, sizeof(ParsedFont));
    if (!font) {
        for (int i = 0; i < bin_info.subr_count; i++)
            free(bin_info.subr_data[i]);
        for (int i = 0; i < bin_info.cs_count; i++)
            free(bin_info.cs_data[i]);
        free(seg.ascii_data);
        free(seg.binary_data);
        return NULL;
    }

    font->is_type1 = true;
    font->is_cff = false;
    font->owns_data = false;
    font->t1_lenIV = bin_info.lenIV;

    /* Font name */
    if (ascii_info.font_name[0]) {
        strncpy(font->family_name, ascii_info.font_name, 255);
        font->family_name[255] = '\0';
    }

    /* Units per em from FontMatrix.
     * FontMatrix is typically [0.001 0 0 0.001 0 0] meaning 1000 units/em.
     * units_per_em = round(1 / FontMatrix[0]) */
    if (ascii_info.font_matrix[0] > 0) {
        font->units_per_em = (int)(1.0 / ascii_info.font_matrix[0] + 0.5);
    } else {
        font->units_per_em = 1000;
    }

    /* Metrics from FontBBox */
    font->ascent = (int)ascii_info.font_bbox[3];
    font->descent = (int)ascii_info.font_bbox[1];
    if (font->ascent == 0) font->ascent = 800;
    if (font->descent == 0) font->descent = -200;

    /* Store subroutines */
    font->t1_subrs_count = bin_info.subr_count;
    if (bin_info.subr_count > 0) {
        /* Compute total size for flat buffer */
        size_t total_subr_size = 0;
        for (int i = 0; i < bin_info.subr_count; i++) {
            total_subr_size += bin_info.subr_lens[i];
        }

        font->t1_subrs_data = (uint8_t *)malloc(total_subr_size > 0 ? total_subr_size : 1);
        font->t1_subrs_offs = (size_t *)calloc(bin_info.subr_count, sizeof(size_t));
        font->t1_subrs_lens = (size_t *)calloc(bin_info.subr_count, sizeof(size_t));

        if (font->t1_subrs_data && font->t1_subrs_offs && font->t1_subrs_lens) {
            size_t off = 0;
            for (int i = 0; i < bin_info.subr_count; i++) {
                font->t1_subrs_offs[i] = off;
                if (bin_info.subr_data[i] && bin_info.subr_lens[i] > 0) {
                    memcpy(font->t1_subrs_data + off, bin_info.subr_data[i],
                           bin_info.subr_lens[i]);
                    font->t1_subrs_lens[i] = bin_info.subr_lens[i];
                } else {
                    font->t1_subrs_lens[i] = 0;
                }
                off += bin_info.subr_lens[i];
            }
        }
    }

    /* Store charstrings */
    font->t1_num_charstrings = bin_info.cs_count;
    font->num_glyphs = bin_info.cs_count;

    {
        /* Compute total charstring data size */
        size_t total_cs_size = 0;
        for (int i = 0; i < bin_info.cs_count; i++) {
            total_cs_size += bin_info.cs_lens[i];
        }

        font->t1_charstrings_data = (uint8_t *)malloc(total_cs_size > 0 ? total_cs_size : 1);
        font->t1_charstrings_offs = (size_t *)calloc(bin_info.cs_count, sizeof(size_t));
        font->t1_charstrings_lens = (size_t *)calloc(bin_info.cs_count, sizeof(size_t));
        font->t1_glyph_names = (char **)calloc(bin_info.cs_count, sizeof(char *));

        if (font->t1_charstrings_data && font->t1_charstrings_offs &&
            font->t1_charstrings_lens && font->t1_glyph_names) {
            size_t off = 0;
            for (int i = 0; i < bin_info.cs_count; i++) {
                font->t1_charstrings_offs[i] = off;
                font->t1_charstrings_lens[i] = bin_info.cs_lens[i];
                if (bin_info.cs_data[i] && bin_info.cs_lens[i] > 0) {
                    memcpy(font->t1_charstrings_data + off, bin_info.cs_data[i],
                           bin_info.cs_lens[i]);
                }
                off += bin_info.cs_lens[i];

                /* Store glyph name */
                font->t1_glyph_names[i] = _strdup(bin_info.cs_names[i]);
            }
        }
    }

    /* Build encoding mapping (char_code -> GID) */
    for (int i = 0; i < 256; i++)
        font->encoding[i] = 0;

    if (ascii_info.has_custom_encoding) {
        /* Custom encoding from the font */
        for (int code = 0; code < 256; code++) {
            if (ascii_info.encoding_names[code][0] != '\0') {
                /* Find the GID for this glyph name */
                int gid = t1_find_gid_by_name(font, ascii_info.encoding_names[code]);
                if (gid >= 0) {
                    font->encoding[code] = gid;
                }
            }
        }
    } else if (ascii_info.uses_standard_encoding) {
        /* Standard Encoding */
        for (int code = 0; code < 256; code++) {
            if (standard_encoding_names[code]) {
                int gid = t1_find_gid_by_name(font, standard_encoding_names[code]);
                if (gid >= 0)
                    font->encoding[code] = gid;
            }
        }
    } else {
        /* Try identity mapping capped at num_glyphs */
        for (int i = 0; i < 256 && i < font->num_glyphs; i++) {
            font->encoding[i] = i;
        }
    }

    /* Pre-compute glyph widths */
    font->glyph_widths = (double *)calloc(font->num_glyphs, sizeof(double));

    /* Free temporary parse data */
    for (int i = 0; i < bin_info.subr_count; i++)
        free(bin_info.subr_data[i]);
    for (int i = 0; i < bin_info.cs_count; i++)
        free(bin_info.cs_data[i]);
    free(seg.ascii_data);
    free(seg.binary_data);

    fprintf(stderr, "[TYPE1] Parsed '%s': %d glyphs, %d subrs, upem=%d, lenIV=%d\n",
            font->family_name, font->num_glyphs, font->t1_subrs_count,
            font->units_per_em, font->t1_lenIV);

    return font;
}
