/*
 * AmundsPDF - pdf_parser.c
 * PDF file parser: tokenizer, object model, xref table, page tree navigation.
 *
 * Memory-mapped I/O, full xref (table + stream), indirect object caching,
 * page tree flattening with resource inheritance.
 */

#include "pdf_parser.h"
#include "pdf_inflate.h"
#include "pdf_crypt.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OBJECT_CACHE_BUCKETS  1024
#define INITIAL_XREF_SIZE     256

/* ═══════════════════════════════════════════════════════════════════════════
 * Object cache (hash map: obj_num -> PdfObj*)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct CacheEntry {
    int                 obj_num;
    int                 gen_num;
    PdfObj             *obj;
    struct CacheEntry  *next;
} CacheEntry;

/* Stored as file-level state embedded in a wrapper we keep beside PdfDocument.
 * Since PdfDocument doesn't have a slot for this, we use a simple parallel
 * mapping keyed by the PdfDocument pointer. For simplicity, we embed the cache
 * directly in a static table keyed by document address. Because AmundsPDF is
 * single-document, a single global cache suffices. */

static CacheEntry *g_cache[OBJECT_CACHE_BUCKETS];
static PdfDocument *g_cache_owner = NULL;

static void cache_init(PdfDocument *doc) {
    (void)doc;
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_owner = doc;
}

static void cache_destroy(void) {
    for (int i = 0; i < OBJECT_CACHE_BUCKETS; i++) {
        CacheEntry *e = g_cache[i];
        while (e) {
            CacheEntry *next = e->next;
            /* Do NOT free e->obj here: it was returned to the caller and may
             * still be referenced (e.g., page dicts). We free these objects
             * via pdf_close -> cache cleanup. Actually we DO free them here
             * since cache_destroy is called from pdf_close. */
            pdf_free_obj(e->obj);
            free(e);
            e = next;
        }
        g_cache[i] = NULL;
    }
    g_cache_owner = NULL;
}

static unsigned cache_hash(int obj_num) {
    return (unsigned)obj_num % OBJECT_CACHE_BUCKETS;
}

static PdfObj *cache_get(int obj_num, int gen_num) {
    unsigned h = cache_hash(obj_num);
    for (CacheEntry *e = g_cache[h]; e; e = e->next) {
        if (e->obj_num == obj_num && e->gen_num == gen_num)
            return e->obj;
    }
    return NULL;
}

static void cache_put(int obj_num, int gen_num, PdfObj *obj) {
    if (!obj) return;
    /* Don't double-insert */
    if (cache_get(obj_num, gen_num)) return;

    CacheEntry *e = (CacheEntry *)calloc(1, sizeof(CacheEntry));
    if (!e) return;
    e->obj_num = obj_num;
    e->gen_num = gen_num;
    e->obj = obj;

    unsigned h = cache_hash(obj_num);
    e->next = g_cache[h];
    g_cache[h] = e;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Parsing cursor
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t  *base;   /* start of file data */
    size_t          len;    /* total file length */
    size_t          pos;    /* current read position */
} ParseCtx;

static inline bool at_eof(const ParseCtx *ctx) {
    return ctx->pos >= ctx->len;
}

static inline uint8_t peek_byte(const ParseCtx *ctx) {
    if (ctx->pos >= ctx->len) return 0;
    return ctx->base[ctx->pos];
}

static inline uint8_t read_byte(ParseCtx *ctx) {
    if (ctx->pos >= ctx->len) return 0;
    return ctx->base[ctx->pos++];
}

static inline bool is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\0';
}

static inline bool is_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static inline bool is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static inline bool is_octal(uint8_t c) {
    return c >= '0' && c <= '7';
}

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* ─── Whitespace / comment skipping ─── */

static void skip_whitespace_and_comments(ParseCtx *ctx) {
    while (ctx->pos < ctx->len) {
        uint8_t c = ctx->base[ctx->pos];
        if (is_ws(c)) {
            ctx->pos++;
        } else if (c == '%') {
            /* skip to end of line */
            while (ctx->pos < ctx->len && ctx->base[ctx->pos] != '\n' &&
                   ctx->base[ctx->pos] != '\r')
                ctx->pos++;
            /* skip the EOL itself */
            if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '\r')
                ctx->pos++;
            if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '\n')
                ctx->pos++;
        } else {
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PdfObj allocation helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static PdfObj *obj_alloc(PdfObjType type) {
    PdfObj *o = (PdfObj *)calloc(1, sizeof(PdfObj));
    if (o) o->type = type;
    return o;
}

static PdfObj *obj_null(void) {
    return obj_alloc(PDF_OBJ_NULL);
}

static PdfObj *obj_bool(bool v) {
    PdfObj *o = obj_alloc(PDF_OBJ_BOOL);
    if (o) o->boolean = v;
    return o;
}

static PdfObj *obj_int(int64_t v) {
    PdfObj *o = obj_alloc(PDF_OBJ_INT);
    if (o) o->integer = v;
    return o;
}

static PdfObj *obj_real(double v) {
    PdfObj *o = obj_alloc(PDF_OBJ_REAL);
    if (o) o->real = v;
    return o;
}

static PdfObj *obj_name(const char *s) {
    PdfObj *o = obj_alloc(PDF_OBJ_NAME);
    if (!o) return NULL;
    size_t slen = strlen(s);
    o->name = (char *)malloc(slen + 1);
    if (!o->name) { free(o); return NULL; }
    memcpy(o->name, s, slen + 1);
    return o;
}

static PdfObj *obj_string(const uint8_t *data, size_t len) {
    PdfObj *o = obj_alloc(PDF_OBJ_STRING);
    if (!o) return NULL;
    o->string.data = (uint8_t *)malloc(len + 1);
    if (!o->string.data) { free(o); return NULL; }
    if (len > 0) memcpy(o->string.data, data, len);
    o->string.data[len] = 0;
    o->string.length = len;
    return o;
}

static PdfObj *obj_ref(int obj_num, int gen_num) {
    PdfObj *o = obj_alloc(PDF_OBJ_REF);
    if (o) { o->ref.obj_num = obj_num; o->ref.gen_num = gen_num; }
    return o;
}

/* ─── Dict helpers ─── */

static PdfDict *dict_alloc(void) {
    PdfDict *d = (PdfDict *)calloc(1, sizeof(PdfDict));
    if (!d) return NULL;
    d->capacity = 16;
    d->entries = (PdfDictEntry *)calloc(d->capacity, sizeof(PdfDictEntry));
    if (!d->entries) { free(d); return NULL; }
    return d;
}

static bool dict_put(PdfDict *dict, const char *key, PdfObj *value) {
    if (!dict || !key || !value) return false;

    /* Overwrite existing? */
    for (int i = 0; i < dict->count; i++) {
        if (strcmp(dict->entries[i].key, key) == 0) {
            pdf_free_obj(dict->entries[i].value);
            dict->entries[i].value = value;
            return true;
        }
    }

    /* Grow if needed */
    if (dict->count >= dict->capacity) {
        int new_cap = dict->capacity * 2;
        if (new_cap > PDF_MAX_DICT_SIZE * 4)
            new_cap = PDF_MAX_DICT_SIZE * 4; /* hard safety limit */
        PdfDictEntry *ne = (PdfDictEntry *)realloc(dict->entries,
                            new_cap * sizeof(PdfDictEntry));
        if (!ne) return false;
        memset(ne + dict->capacity, 0,
               (new_cap - dict->capacity) * sizeof(PdfDictEntry));
        dict->entries = ne;
        dict->capacity = new_cap;
    }

    size_t klen = strlen(key);
    if (klen >= PDF_MAX_NAME_LEN) klen = PDF_MAX_NAME_LEN - 1;
    memcpy(dict->entries[dict->count].key, key, klen);
    dict->entries[dict->count].key[klen] = 0;
    dict->entries[dict->count].value = value;
    dict->count++;
    return true;
}

static PdfObj *obj_dict(PdfDict *d) {
    PdfObj *o = obj_alloc(PDF_OBJ_DICT);
    if (o) o->dict = d;
    return o;
}

/* ─── Array helpers ─── */

static PdfArray *array_alloc(void) {
    PdfArray *a = (PdfArray *)calloc(1, sizeof(PdfArray));
    if (!a) return NULL;
    a->capacity = 8;
    a->items = (PdfObj **)calloc(a->capacity, sizeof(PdfObj *));
    if (!a->items) { free(a); return NULL; }
    return a;
}

static bool array_push(PdfArray *arr, PdfObj *item) {
    if (!arr || !item) return false;

    if (arr->count >= arr->capacity) {
        int new_cap = arr->capacity * 2;
        PdfObj **ni = (PdfObj **)realloc(arr->items, new_cap * sizeof(PdfObj *));
        if (!ni) return false;
        arr->items = ni;
        arr->capacity = new_cap;
    }
    arr->items[arr->count++] = item;
    return true;
}

static PdfObj *obj_array(PdfArray *a) {
    PdfObj *o = obj_alloc(PDF_OBJ_ARRAY);
    if (o) o->array = a;
    return o;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * pdf_free_obj - recursively free a PdfObj
 * ═══════════════════════════════════════════════════════════════════════════ */

void pdf_free_obj(PdfObj *obj) {
    if (!obj) return;

    switch (obj->type) {
    case PDF_OBJ_NAME:
        free(obj->name);
        break;

    case PDF_OBJ_STRING:
        free(obj->string.data);
        break;

    case PDF_OBJ_ARRAY:
        if (obj->array) {
            for (int i = 0; i < obj->array->count; i++)
                pdf_free_obj(obj->array->items[i]);
            free(obj->array->items);
            free(obj->array);
        }
        break;

    case PDF_OBJ_DICT:
        if (obj->dict) {
            for (int i = 0; i < obj->dict->count; i++)
                pdf_free_obj(obj->dict->entries[i].value);
            free(obj->dict->entries);
            free(obj->dict);
        }
        break;

    case PDF_OBJ_STREAM:
        if (obj->stream) {
            if (obj->stream->dict) {
                for (int i = 0; i < obj->stream->dict->count; i++)
                    pdf_free_obj(obj->stream->dict->entries[i].value);
                free(obj->stream->dict->entries);
                free(obj->stream->dict);
            }
            free(obj->stream->raw_data);
            free(obj->stream->decoded_data);
            free(obj->stream);
        }
        break;

    default:
        break;
    }

    free(obj);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tokenizer / object parser
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declarations */
static PdfObj *parse_object(ParseCtx *ctx);
static PdfObj *parse_dict_or_hex_string(ParseCtx *ctx);
static PdfObj *parse_array(ParseCtx *ctx);
static PdfObj *parse_literal_string(ParseCtx *ctx);
static PdfObj *parse_hex_string(ParseCtx *ctx);
static PdfObj *parse_name(ParseCtx *ctx);
static PdfObj *parse_number_or_ref(ParseCtx *ctx);
static PdfObj *parse_keyword(ParseCtx *ctx);
static PdfDict *parse_dict_body(ParseCtx *ctx);

/* ─── Name parsing ─── */

static PdfObj *parse_name(ParseCtx *ctx) {
    /* Skip the leading '/' */
    ctx->pos++;

    char buf[PDF_MAX_NAME_LEN];
    int len = 0;

    while (ctx->pos < ctx->len && len < PDF_MAX_NAME_LEN - 1) {
        uint8_t c = ctx->base[ctx->pos];
        if (is_ws(c) || is_delim(c))
            break;

        if (c == '#' && ctx->pos + 2 < ctx->len) {
            int h1 = hex_val(ctx->base[ctx->pos + 1]);
            int h2 = hex_val(ctx->base[ctx->pos + 2]);
            if (h1 >= 0 && h2 >= 0) {
                buf[len++] = (char)((h1 << 4) | h2);
                ctx->pos += 3;
                continue;
            }
        }

        buf[len++] = (char)c;
        ctx->pos++;
    }

    buf[len] = 0;
    return obj_name(buf);
}

/* ─── Literal string () ─── */

static PdfObj *parse_literal_string(ParseCtx *ctx) {
    ctx->pos++; /* skip '(' */

    uint8_t buf[PDF_MAX_STRING_LEN];
    size_t len = 0;
    int depth = 1;

    while (ctx->pos < ctx->len && depth > 0 && len < PDF_MAX_STRING_LEN - 1) {
        uint8_t c = read_byte(ctx);

        if (c == '(') {
            depth++;
            buf[len++] = c;
        } else if (c == ')') {
            depth--;
            if (depth > 0)
                buf[len++] = c;
        } else if (c == '\\') {
            if (at_eof(ctx)) break;
            uint8_t esc = read_byte(ctx);
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
                if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '\n')
                    ctx->pos++;
                break;
            case '\n':
                /* line continuation: \<LF> */
                break;
            default:
                if (is_octal(esc)) {
                    int val = esc - '0';
                    if (ctx->pos < ctx->len && is_octal(ctx->base[ctx->pos])) {
                        val = val * 8 + (read_byte(ctx) - '0');
                        if (ctx->pos < ctx->len && is_octal(ctx->base[ctx->pos]))
                            val = val * 8 + (read_byte(ctx) - '0');
                    }
                    buf[len++] = (uint8_t)(val & 0xFF);
                } else {
                    /* Unknown escape: include the character as-is */
                    buf[len++] = esc;
                }
                break;
            }
        } else {
            buf[len++] = c;
        }
    }

    return obj_string(buf, len);
}

/* ─── Hex string <> ─── */

static PdfObj *parse_hex_string(ParseCtx *ctx) {
    ctx->pos++; /* skip '<' */

    uint8_t buf[PDF_MAX_STRING_LEN];
    size_t len = 0;
    int nibble = -1;

    while (ctx->pos < ctx->len && len < PDF_MAX_STRING_LEN) {
        uint8_t c = ctx->base[ctx->pos++];
        if (c == '>') break;
        if (is_ws(c)) continue;

        int hv = hex_val(c);
        if (hv < 0) continue; /* skip invalid chars */

        if (nibble < 0) {
            nibble = hv;
        } else {
            buf[len++] = (uint8_t)((nibble << 4) | hv);
            nibble = -1;
        }
    }

    /* If odd number of hex digits, the final nibble is padded with 0 */
    if (nibble >= 0 && len < PDF_MAX_STRING_LEN)
        buf[len++] = (uint8_t)(nibble << 4);

    return obj_string(buf, len);
}

/* ─── Dictionary << >> ─── */

static PdfDict *parse_dict_body(ParseCtx *ctx) {
    PdfDict *dict = dict_alloc();
    if (!dict) return NULL;

    while (ctx->pos < ctx->len) {
        skip_whitespace_and_comments(ctx);
        if (ctx->pos >= ctx->len) break;

        /* Check for >> */
        if (ctx->base[ctx->pos] == '>' && ctx->pos + 1 < ctx->len &&
            ctx->base[ctx->pos + 1] == '>') {
            ctx->pos += 2;
            return dict;
        }

        /* Key must be a name */
        if (ctx->base[ctx->pos] != '/') {
            /* Malformed: skip this byte and try to continue */
            ctx->pos++;
            continue;
        }

        PdfObj *key_obj = parse_name(ctx);
        if (!key_obj) break;
        const char *key = key_obj->name;

        /* Value */
        skip_whitespace_and_comments(ctx);
        PdfObj *val = parse_object(ctx);
        if (!val) val = obj_null();

        dict_put(dict, key, val);
        pdf_free_obj(key_obj);
    }

    return dict;
}

static PdfObj *parse_dict_or_hex_string(ParseCtx *ctx) {
    /* We're at '<'. Peek at the next character. */
    if (ctx->pos + 1 < ctx->len && ctx->base[ctx->pos + 1] == '<') {
        /* Dictionary << ... >> */
        ctx->pos += 2; /* skip '<<' */
        PdfDict *dict = parse_dict_body(ctx);
        if (!dict) return obj_null();

        /* Check for 'stream' keyword following the dictionary */
        size_t saved = ctx->pos;
        skip_whitespace_and_comments(ctx);

        /* Check if "stream" follows */
        if (ctx->pos + 6 <= ctx->len &&
            memcmp(ctx->base + ctx->pos, "stream", 6) == 0) {
            /* Verify the character after "stream" is EOL or whitespace */
            size_t after = ctx->pos + 6;
            if (after < ctx->len &&
                (ctx->base[after] == '\r' || ctx->base[after] == '\n' ||
                 ctx->base[after] == ' ')) {
                /* This is a stream object */
                ctx->pos = after;

                /* Skip the single EOL after "stream" (CR, LF, or CRLF) */
                if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '\r')
                    ctx->pos++;
                if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '\n')
                    ctx->pos++;

                /* Get /Length from dictionary */
                PdfObj *len_obj = pdf_dict_get(dict, "Length");
                int64_t stream_len = 0;
                if (len_obj) {
                    if (len_obj->type == PDF_OBJ_INT) {
                        stream_len = len_obj->integer;
                    } else if (len_obj->type == PDF_OBJ_REF) {
                        /* Need to resolve indirect reference for Length.
                         * Parse the referenced object directly. */
                        int64_t ref_off = -1;
                        if (g_cache_owner &&
                            len_obj->ref.obj_num >= 0 &&
                            len_obj->ref.obj_num < g_cache_owner->xref_count) {
                            XRefEntry *xe = &g_cache_owner->xref[len_obj->ref.obj_num];
                            if (xe->in_use && xe->obj_stream_num < 0)
                                ref_off = xe->offset;
                        }
                        if (ref_off >= 0 && (size_t)ref_off < ctx->len) {
                            ParseCtx tmp = { ctx->base, ctx->len, (size_t)ref_off };
                            /* Skip "N G obj" prefix */
                            skip_whitespace_and_comments(&tmp);
                            /* Skip object number */
                            while (tmp.pos < tmp.len && is_digit(tmp.base[tmp.pos])) tmp.pos++;
                            skip_whitespace_and_comments(&tmp);
                            /* Skip gen number */
                            while (tmp.pos < tmp.len && is_digit(tmp.base[tmp.pos])) tmp.pos++;
                            skip_whitespace_and_comments(&tmp);
                            /* Skip "obj" keyword */
                            if (tmp.pos + 3 <= tmp.len &&
                                memcmp(tmp.base + tmp.pos, "obj", 3) == 0)
                                tmp.pos += 3;
                            skip_whitespace_and_comments(&tmp);
                            PdfObj *len_resolved = parse_object(&tmp);
                            if (len_resolved && len_resolved->type == PDF_OBJ_INT) {
                                stream_len = len_resolved->integer;
                            }
                            pdf_free_obj(len_resolved);
                        }
                    }
                }

                /* Clamp stream length */
                if (stream_len < 0) stream_len = 0;
                if (ctx->pos + (size_t)stream_len > ctx->len)
                    stream_len = (int64_t)(ctx->len - ctx->pos);

                /* Build stream object */
                PdfStream *stm = (PdfStream *)calloc(1, sizeof(PdfStream));
                if (!stm) {
                    /* Cleanup dict on failure */
                    for (int i = 0; i < dict->count; i++)
                        pdf_free_obj(dict->entries[i].value);
                    free(dict->entries);
                    free(dict);
                    return obj_null();
                }
                stm->dict = dict;
                stm->raw_length = (size_t)stream_len;
                stm->raw_data = (uint8_t *)malloc(stm->raw_length);
                if (stm->raw_data && stm->raw_length > 0)
                    memcpy(stm->raw_data, ctx->base + ctx->pos, stm->raw_length);

                ctx->pos += (size_t)stream_len;

                /* Skip to endstream */
                skip_whitespace_and_comments(ctx);
                if (ctx->pos + 9 <= ctx->len &&
                    memcmp(ctx->base + ctx->pos, "endstream", 9) == 0)
                    ctx->pos += 9;

                PdfObj *o = obj_alloc(PDF_OBJ_STREAM);
                if (o) {
                    o->stream = stm;
                } else {
                    free(stm->raw_data);
                    free(stm);
                }
                return o;
            }
        }

        /* Not a stream, restore position */
        ctx->pos = saved;
        return obj_dict(dict);
    } else {
        /* Hex string */
        return parse_hex_string(ctx);
    }
}

/* ─── Array [ ... ] ─── */

static PdfObj *parse_array(ParseCtx *ctx) {
    ctx->pos++; /* skip '[' */

    PdfArray *arr = array_alloc();
    if (!arr) return obj_null();

    while (ctx->pos < ctx->len) {
        skip_whitespace_and_comments(ctx);
        if (ctx->pos >= ctx->len) break;

        if (ctx->base[ctx->pos] == ']') {
            ctx->pos++;
            break;
        }

        PdfObj *item = parse_object(ctx);
        if (!item) {
            /* Try to skip bad data and continue */
            ctx->pos++;
            continue;
        }
        array_push(arr, item);
    }

    return obj_array(arr);
}

/* ─── Number or indirect reference (N G R) ─── */

static PdfObj *parse_number_or_ref(ParseCtx *ctx) {
    size_t start = ctx->pos;
    bool negative = false;
    bool has_dot = false;

    if (ctx->base[ctx->pos] == '+' || ctx->base[ctx->pos] == '-') {
        if (ctx->base[ctx->pos] == '-') negative = true;
        ctx->pos++;
    }

    while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos]))
        ctx->pos++;

    if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '.') {
        has_dot = true;
        ctx->pos++;
        while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos]))
            ctx->pos++;
    }

    size_t num_end = ctx->pos;

    /* Extract the number text */
    size_t tlen = num_end - start;
    if (tlen == 0 || tlen > 63) {
        ctx->pos = start + 1; /* advance at least 1 byte */
        return obj_null();
    }

    char numbuf[64];
    memcpy(numbuf, ctx->base + start, tlen);
    numbuf[tlen] = 0;

    if (has_dot) {
        return obj_real(atof(numbuf));
    }

    int64_t ival = _atoi64(numbuf);

    /* Check for indirect reference: <int> <int> R */
    if (!negative && !has_dot && ival >= 0) {
        size_t saved = ctx->pos;
        skip_whitespace_and_comments(ctx);

        if (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) {
            size_t gen_start = ctx->pos;
            while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos]))
                ctx->pos++;
            size_t glen = ctx->pos - gen_start;

            skip_whitespace_and_comments(ctx);

            if (ctx->pos < ctx->len && ctx->base[ctx->pos] == 'R' &&
                (ctx->pos + 1 >= ctx->len ||
                 is_ws(ctx->base[ctx->pos + 1]) ||
                 is_delim(ctx->base[ctx->pos + 1]))) {
                /* It's an indirect reference */
                char genbuf[32];
                if (glen > 31) glen = 31;
                memcpy(genbuf, ctx->base + gen_start, glen);
                genbuf[glen] = 0;
                int gen = atoi(genbuf);
                ctx->pos++; /* skip 'R' */
                return obj_ref((int)ival, gen);
            }
        }

        /* Not a reference, restore position */
        ctx->pos = saved;
    }

    return obj_int(ival);
}

/* ─── Keyword (true, false, null) ─── */

static PdfObj *parse_keyword(ParseCtx *ctx) {
    size_t start = ctx->pos;

    while (ctx->pos < ctx->len && !is_ws(ctx->base[ctx->pos]) &&
           !is_delim(ctx->base[ctx->pos]))
        ctx->pos++;

    size_t klen = ctx->pos - start;

    if (klen == 4 && memcmp(ctx->base + start, "true", 4) == 0)
        return obj_bool(true);
    if (klen == 5 && memcmp(ctx->base + start, "false", 5) == 0)
        return obj_bool(false);
    if (klen == 4 && memcmp(ctx->base + start, "null", 4) == 0)
        return obj_null();

    /* Unknown keyword - skip */
    return NULL;
}

/* ─── Main object parser ─── */

static PdfObj *parse_object(ParseCtx *ctx) {
    skip_whitespace_and_comments(ctx);
    if (at_eof(ctx)) return NULL;

    uint8_t c = peek_byte(ctx);

    switch (c) {
    case '/':
        return parse_name(ctx);

    case '(':
        return parse_literal_string(ctx);

    case '<':
        return parse_dict_or_hex_string(ctx);

    case '[':
        return parse_array(ctx);

    case '+': case '-': case '.':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return parse_number_or_ref(ctx);

    default:
        return parse_keyword(ctx);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Parse an indirect object definition: N G obj <value> endobj
 * Returns the inner value (not wrapped in an "indirect" type).
 * ═══════════════════════════════════════════════════════════════════════════ */

static PdfObj *parse_indirect_object(ParseCtx *ctx, int *out_obj_num, int *out_gen_num) {
    skip_whitespace_and_comments(ctx);

    /* Parse object number */
    size_t start = ctx->pos;
    while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) ctx->pos++;
    if (ctx->pos == start) return NULL;
    char numbuf[32];
    size_t nlen = ctx->pos - start;
    if (nlen > 31) nlen = 31;
    memcpy(numbuf, ctx->base + start, nlen);
    numbuf[nlen] = 0;
    int obj_num = atoi(numbuf);

    skip_whitespace_and_comments(ctx);

    /* Parse generation number */
    start = ctx->pos;
    while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) ctx->pos++;
    if (ctx->pos == start) return NULL;
    nlen = ctx->pos - start;
    if (nlen > 31) nlen = 31;
    memcpy(numbuf, ctx->base + start, nlen);
    numbuf[nlen] = 0;
    int gen_num = atoi(numbuf);

    skip_whitespace_and_comments(ctx);

    /* Expect "obj" keyword */
    if (ctx->pos + 3 > ctx->len || memcmp(ctx->base + ctx->pos, "obj", 3) != 0)
        return NULL;
    ctx->pos += 3;

    /* Parse the object value */
    PdfObj *val = parse_object(ctx);

    /* Check for "stream" keyword if the object is a dict (parse_dict_or_hex_string
     * already handles this, but in case parse_object returned the dict before
     * seeing stream, we handle it here too). This is already handled inside
     * parse_dict_or_hex_string, so we just skip to endobj. */

    /* Skip to endobj */
    skip_whitespace_and_comments(ctx);
    if (ctx->pos + 6 <= ctx->len &&
        memcmp(ctx->base + ctx->pos, "endobj", 6) == 0)
        ctx->pos += 6;

    if (out_obj_num) *out_obj_num = obj_num;
    if (out_gen_num) *out_gen_num = gen_num;

    /* Record the object/generation number on stream objects for decryption */
    if (val && val->type == PDF_OBJ_STREAM && val->stream) {
        val->stream->obj_num = obj_num;
        val->stream->gen_num = gen_num;
    }

    return val;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cross-reference table parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Grow xref table if needed ─── */
static bool xref_ensure(PdfDocument *doc, int max_obj) {
    if (max_obj <= doc->xref_count) return true;

    int new_count = max_obj;
    XRefEntry *ne = (XRefEntry *)realloc(doc->xref, new_count * sizeof(XRefEntry));
    if (!ne) return false;

    /* Initialize new entries */
    for (int i = doc->xref_count; i < new_count; i++) {
        ne[i].offset = 0;
        ne[i].gen_num = 65535;
        ne[i].in_use = false;
        ne[i].obj_stream_num = -1;
        ne[i].obj_stream_idx = 0;
    }

    doc->xref = ne;
    doc->xref_count = new_count;
    return true;
}

/* ─── Parse a single classic xref section ─── */
static bool parse_xref_section(ParseCtx *ctx, PdfDocument *doc) {
    /* We should be right after the "xref" keyword + whitespace */
    while (ctx->pos < ctx->len) {
        skip_whitespace_and_comments(ctx);
        if (at_eof(ctx)) return false;

        /* Check if we've hit "trailer" */
        if (ctx->pos + 7 <= ctx->len &&
            memcmp(ctx->base + ctx->pos, "trailer", 7) == 0)
            break;

        /* Parse subsection header: first_obj count */
        size_t start = ctx->pos;
        while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) ctx->pos++;
        if (ctx->pos == start) break; /* Not a digit - probably "trailer" or error */

        char buf[32];
        size_t blen = ctx->pos - start;
        if (blen > 31) blen = 31;
        memcpy(buf, ctx->base + start, blen);
        buf[blen] = 0;
        int first_obj = atoi(buf);

        skip_whitespace_and_comments(ctx);

        start = ctx->pos;
        while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) ctx->pos++;
        blen = ctx->pos - start;
        if (blen > 31) blen = 31;
        memcpy(buf, ctx->base + start, blen);
        buf[blen] = 0;
        int entry_count = atoi(buf);

        if (entry_count <= 0 || entry_count > PDF_MAX_PAGES * 100)
            break; /* sanity check */

        if (!xref_ensure(doc, first_obj + entry_count))
            return false;

        /* Parse entries */
        for (int i = 0; i < entry_count; i++) {
            skip_whitespace_and_comments(ctx);

            /* Each entry: 10-digit offset, space, 5-digit gen, space, f|n, EOL */
            /* But be flexible with formatting */
            start = ctx->pos;
            while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) ctx->pos++;
            blen = ctx->pos - start;
            if (blen > 31) blen = 31;
            memcpy(buf, ctx->base + start, blen);
            buf[blen] = 0;
            int64_t offset = _atoi64(buf);

            skip_whitespace_and_comments(ctx);

            start = ctx->pos;
            while (ctx->pos < ctx->len && is_digit(ctx->base[ctx->pos])) ctx->pos++;
            blen = ctx->pos - start;
            if (blen > 31) blen = 31;
            memcpy(buf, ctx->base + start, blen);
            buf[blen] = 0;
            int gen = atoi(buf);

            skip_whitespace_and_comments(ctx);

            bool in_use = false;
            if (ctx->pos < ctx->len) {
                uint8_t flag = ctx->base[ctx->pos];
                if (flag == 'n') in_use = true;
                ctx->pos++;
            }

            /* Skip trailing whitespace/EOL for this entry */
            while (ctx->pos < ctx->len &&
                   (ctx->base[ctx->pos] == ' ' || ctx->base[ctx->pos] == '\r'))
                ctx->pos++;
            if (ctx->pos < ctx->len && ctx->base[ctx->pos] == '\n')
                ctx->pos++;

            int obj_idx = first_obj + i;
            if (obj_idx < doc->xref_count) {
                /* Only update if not already set (first xref takes priority
                 * in incremental updates) */
                if (!doc->xref[obj_idx].in_use || doc->xref[obj_idx].offset == 0) {
                    doc->xref[obj_idx].offset = offset;
                    doc->xref[obj_idx].gen_num = gen;
                    doc->xref[obj_idx].in_use = in_use;
                    doc->xref[obj_idx].obj_stream_num = -1;
                    doc->xref[obj_idx].obj_stream_idx = 0;
                }
            }
        }
    }

    return true;
}

/* ─── Parse xref stream ─── */
static bool parse_xref_stream(ParseCtx *ctx, PdfDocument *doc, PdfObj **out_trailer) {
    /* We're at the offset which is an indirect object containing a stream.
     * Parse the indirect object. */
    int obj_num = 0, gen_num = 0;
    PdfObj *stm_obj = parse_indirect_object(ctx, &obj_num, &gen_num);
    if (!stm_obj || stm_obj->type != PDF_OBJ_STREAM) {
        pdf_free_obj(stm_obj);
        return false;
    }

    PdfStream *stm = stm_obj->stream;
    PdfDict *dict = stm->dict;

    /* Verify /Type /XRef */
    const char *type_name = pdf_dict_get_name(dict, "Type");
    if (!type_name || strcmp(type_name, "XRef") != 0) {
        /* Some PDFs omit /Type, try anyway */
    }

    /* Get /Size */
    int size = pdf_dict_get_int(dict, "Size", 0);
    if (size > 0)
        xref_ensure(doc, size);

    /* Get /W array (field widths) */
    PdfArray *w_arr = pdf_dict_get_array(dict, "W");
    if (!w_arr || w_arr->count < 3) {
        pdf_free_obj(stm_obj);
        return false;
    }

    int w[3];
    for (int i = 0; i < 3; i++) {
        PdfObj *wi = pdf_array_get(w_arr, i);
        w[i] = (wi && wi->type == PDF_OBJ_INT) ? (int)wi->integer : 0;
    }

    int entry_size = w[0] + w[1] + w[2];
    if (entry_size <= 0) {
        pdf_free_obj(stm_obj);
        return false;
    }

    /* Decode the stream */
    if (!pdf_decode_stream(doc, stm)) {
        /* Try with raw data if decode fails (maybe uncompressed) */
        if (!stm->decoded_data) {
            stm->decoded_data = stm->raw_data;
            stm->decoded_length = stm->raw_length;
            stm->raw_data = NULL; /* prevent double-free */
        }
    }

    const uint8_t *sdata = stm->decoded_data ? stm->decoded_data : stm->raw_data;
    size_t slen = stm->decoded_data ? stm->decoded_length : stm->raw_length;

    if (!sdata || slen == 0) {
        pdf_free_obj(stm_obj);
        return false;
    }

    /* Get /Index array (default: [0 Size]) */
    PdfArray *index_arr = pdf_dict_get_array(dict, "Index");
    int *ranges = NULL;
    int range_count = 0;

    if (index_arr && index_arr->count >= 2) {
        range_count = index_arr->count / 2;
        ranges = (int *)calloc(range_count * 2, sizeof(int));
        if (ranges) {
            for (int i = 0; i < range_count * 2; i++) {
                PdfObj *v = pdf_array_get(index_arr, i);
                ranges[i] = (v && v->type == PDF_OBJ_INT) ? (int)v->integer : 0;
            }
        }
    } else {
        range_count = 1;
        ranges = (int *)calloc(2, sizeof(int));
        if (ranges) {
            ranges[0] = 0;
            ranges[1] = size;
        }
    }

    if (!ranges) {
        pdf_free_obj(stm_obj);
        return false;
    }

    /* Parse entries */
    size_t data_pos = 0;
    for (int r = 0; r < range_count; r++) {
        int start = ranges[r * 2];
        int count = ranges[r * 2 + 1];

        xref_ensure(doc, start + count);

        for (int i = 0; i < count && data_pos + entry_size <= slen; i++) {
            /* Read fields */
            int64_t fields[3] = {0, 0, 0};
            for (int f = 0; f < 3; f++) {
                for (int b = 0; b < w[f]; b++) {
                    fields[f] = (fields[f] << 8) | sdata[data_pos++];
                }
            }

            /* Default type is 1 if w[0] == 0 */
            int type = (w[0] == 0) ? 1 : (int)fields[0];

            int obj_idx = start + i;
            if (obj_idx >= doc->xref_count) continue;

            /* Only update if not already set */
            if (doc->xref[obj_idx].in_use && doc->xref[obj_idx].offset != 0)
                continue;

            switch (type) {
            case 0: /* free object */
                doc->xref[obj_idx].in_use = false;
                doc->xref[obj_idx].offset = fields[1];
                doc->xref[obj_idx].gen_num = (int)fields[2];
                doc->xref[obj_idx].obj_stream_num = -1;
                break;

            case 1: /* uncompressed object */
                doc->xref[obj_idx].in_use = true;
                doc->xref[obj_idx].offset = fields[1];
                doc->xref[obj_idx].gen_num = (int)fields[2];
                doc->xref[obj_idx].obj_stream_num = -1;
                break;

            case 2: /* compressed in object stream */
                doc->xref[obj_idx].in_use = true;
                doc->xref[obj_idx].offset = 0;
                doc->xref[obj_idx].gen_num = 0;
                doc->xref[obj_idx].obj_stream_num = (int)fields[1];
                doc->xref[obj_idx].obj_stream_idx = (int)fields[2];
                break;

            default:
                break;
            }
        }
    }

    free(ranges);

    /* Build a trailer-like dict from the xref stream dictionary.
     * Clone the relevant keys into a new dict to serve as the trailer. */
    if (out_trailer && !*out_trailer) {
        PdfDict *trailer_dict = dict_alloc();
        if (trailer_dict) {
            const char *trailer_keys[] = {
                "Size", "Root", "Info", "ID", "Prev", "Encrypt", NULL
            };
            for (int k = 0; trailer_keys[k]; k++) {
                PdfObj *v = pdf_dict_get(dict, trailer_keys[k]);
                if (v) {
                    /* Create a copy of the reference/value */
                    PdfObj *copy = NULL;
                    if (v->type == PDF_OBJ_REF)
                        copy = obj_ref(v->ref.obj_num, v->ref.gen_num);
                    else if (v->type == PDF_OBJ_INT)
                        copy = obj_int(v->integer);
                    else if (v->type == PDF_OBJ_ARRAY) {
                        /* Simple shallow clone for /ID */
                        PdfArray *ca = array_alloc();
                        if (ca) {
                            for (int i = 0; i < v->array->count; i++) {
                                PdfObj *item = v->array->items[i];
                                if (item && item->type == PDF_OBJ_STRING)
                                    array_push(ca, obj_string(item->string.data,
                                                              item->string.length));
                                else if (item && item->type == PDF_OBJ_REF)
                                    array_push(ca, obj_ref(item->ref.obj_num,
                                                           item->ref.gen_num));
                                else if (item)
                                    array_push(ca, obj_null());
                            }
                            copy = obj_array(ca);
                        }
                    }
                    if (copy) dict_put(trailer_dict, trailer_keys[k], copy);
                }
            }
            *out_trailer = obj_dict(trailer_dict);
        }
    }

    /* Check for /Prev to chain xref sections */
    int prev = pdf_dict_get_int(dict, "Prev", -1);
    if (prev > 0 && (size_t)prev < ctx->len) {
        ParseCtx prev_ctx = { ctx->base, ctx->len, (size_t)prev };
        skip_whitespace_and_comments(&prev_ctx);

        /* Could be another xref table or xref stream */
        if (prev_ctx.pos + 4 <= prev_ctx.len &&
            memcmp(prev_ctx.base + prev_ctx.pos, "xref", 4) == 0) {
            prev_ctx.pos += 4;
            parse_xref_section(&prev_ctx, doc);
            /* Parse the trailer of the previous section */
            skip_whitespace_and_comments(&prev_ctx);
            if (prev_ctx.pos + 7 <= prev_ctx.len &&
                memcmp(prev_ctx.base + prev_ctx.pos, "trailer", 7) == 0) {
                prev_ctx.pos += 7;
                skip_whitespace_and_comments(&prev_ctx);
                PdfObj *prev_trailer = parse_object(&prev_ctx);
                if (prev_trailer && prev_trailer->type == PDF_OBJ_DICT) {
                    int pp = pdf_dict_get_int(prev_trailer->dict, "Prev", -1);
                    if (pp > 0 && (size_t)pp < ctx->len) {
                        ParseCtx pp_ctx = { ctx->base, ctx->len, (size_t)pp };
                        /* Recurse for additional /Prev chains */
                        skip_whitespace_and_comments(&pp_ctx);
                        if (pp_ctx.pos + 4 <= pp_ctx.len &&
                            memcmp(pp_ctx.base + pp_ctx.pos, "xref", 4) == 0) {
                            pp_ctx.pos += 4;
                            parse_xref_section(&pp_ctx, doc);
                        } else {
                            parse_xref_stream(&pp_ctx, doc, NULL);
                        }
                    }
                }
                pdf_free_obj(prev_trailer);
            }
        } else {
            parse_xref_stream(&prev_ctx, doc, NULL);
        }
    }

    pdf_free_obj(stm_obj);
    return true;
}

/* ─── Find startxref from end of file ─── */
static int64_t find_startxref(const uint8_t *data, size_t len) {
    /* Search backward from end of file for "startxref" */
    size_t search_start = (len > 1024) ? len - 1024 : 0;

    for (size_t i = len; i > search_start; ) {
        i--;
        if (i + 9 <= len && memcmp(data + i, "startxref", 9) == 0) {
            /* Found it. Skip whitespace and read the number. */
            size_t pos = i + 9;
            while (pos < len && is_ws(data[pos])) pos++;

            char buf[32];
            int blen = 0;
            while (pos < len && is_digit(data[pos]) && blen < 31) {
                buf[blen++] = (char)data[pos++];
            }
            buf[blen] = 0;
            if (blen > 0)
                return _atoi64(buf);
            break;
        }
    }

    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dictionary & Array helpers (public API)
 * ═══════════════════════════════════════════════════════════════════════════ */

PdfObj *pdf_dict_get(PdfDict *dict, const char *key) {
    if (!dict || !key) return NULL;
    for (int i = 0; i < dict->count; i++) {
        if (strcmp(dict->entries[i].key, key) == 0)
            return dict->entries[i].value;
    }
    return NULL;
}

int pdf_dict_get_int(PdfDict *dict, const char *key, int default_val) {
    PdfObj *o = pdf_dict_get(dict, key);
    if (!o) return default_val;
    if (o->type == PDF_OBJ_INT) return (int)o->integer;
    if (o->type == PDF_OBJ_REAL) return (int)o->real;
    return default_val;
}

double pdf_dict_get_real(PdfDict *dict, const char *key, double default_val) {
    PdfObj *o = pdf_dict_get(dict, key);
    if (!o) return default_val;
    if (o->type == PDF_OBJ_REAL) return o->real;
    if (o->type == PDF_OBJ_INT) return (double)o->integer;
    return default_val;
}

const char *pdf_dict_get_name(PdfDict *dict, const char *key) {
    PdfObj *o = pdf_dict_get(dict, key);
    if (!o || o->type != PDF_OBJ_NAME) return NULL;
    return o->name;
}

PdfArray *pdf_dict_get_array(PdfDict *dict, const char *key) {
    PdfObj *o = pdf_dict_get(dict, key);
    if (!o || o->type != PDF_OBJ_ARRAY) return NULL;
    return o->array;
}

PdfObj *pdf_array_get(PdfArray *arr, int index) {
    if (!arr || index < 0 || index >= arr->count) return NULL;
    return arr->items[index];
}

int pdf_array_len(PdfArray *arr) {
    return arr ? arr->count : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Object access (public API)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declaration for object stream parsing */
static PdfObj *parse_object_from_stream(PdfDocument *doc, int stm_obj_num, int obj_idx);

PdfObj *pdf_get_object(PdfDocument *doc, int obj_num, int gen_num) {
    if (!doc || obj_num < 0 || obj_num >= doc->xref_count)
        return NULL;

    /* Check cache */
    PdfObj *cached = cache_get(obj_num, gen_num);
    if (cached) return cached;

    XRefEntry *xe = &doc->xref[obj_num];
    if (!xe->in_use) return NULL;

    PdfObj *result = NULL;

    if (xe->obj_stream_num >= 0) {
        /* Object is in an object stream */
        result = parse_object_from_stream(doc, xe->obj_stream_num, xe->obj_stream_idx);
    } else {
        /* Normal uncompressed object */
        if (xe->offset <= 0 || (size_t)xe->offset >= doc->data_len)
            return NULL;

        ParseCtx ctx = { doc->data, doc->data_len, (size_t)xe->offset };
        int parsed_obj_num = 0, parsed_gen_num = 0;
        result = parse_indirect_object(&ctx, &parsed_obj_num, &parsed_gen_num);
    }

    if (result) {
        cache_put(obj_num, gen_num, result);
    }

    return result;
}

PdfObj *pdf_resolve(PdfDocument *doc, PdfObj *obj) {
    if (!doc || !obj) return NULL;

    /* Follow chains of references (with depth limit to prevent loops) */
    int depth = 0;
    while (obj && obj->type == PDF_OBJ_REF && depth < 32) {
        obj = pdf_get_object(doc, obj->ref.obj_num, obj->ref.gen_num);
        depth++;
    }

    return obj;
}

/* ─── Object stream parsing ─── */

static PdfObj *parse_object_from_stream(PdfDocument *doc, int stm_obj_num, int obj_idx) {
    /* Get the object stream itself */
    PdfObj *stm_obj = pdf_get_object(doc, stm_obj_num, 0);
    if (!stm_obj || stm_obj->type != PDF_OBJ_STREAM) return NULL;

    PdfStream *stm = stm_obj->stream;

    /* Decode if needed */
    if (!stm->decoded_data) {
        if (!pdf_decode_stream(doc, stm))
            return NULL;
    }

    const uint8_t *sdata = stm->decoded_data;
    size_t slen = stm->decoded_length;
    if (!sdata || slen == 0) return NULL;

    /* Get /N (number of objects) and /First (byte offset of first object) */
    int n = pdf_dict_get_int(stm->dict, "N", 0);
    int first = pdf_dict_get_int(stm->dict, "First", 0);

    if (obj_idx < 0 || obj_idx >= n || first < 0 || (size_t)first > slen)
        return NULL;

    /* Parse the N pairs of (obj_num, offset) from the beginning of stream */
    ParseCtx hdr_ctx = { sdata, slen, 0 };
    int *offsets = (int *)calloc(n, sizeof(int));
    if (!offsets) return NULL;

    for (int i = 0; i < n; i++) {
        skip_whitespace_and_comments(&hdr_ctx);
        /* obj_num (we don't need it, just skip) */
        while (hdr_ctx.pos < hdr_ctx.len && is_digit(hdr_ctx.base[hdr_ctx.pos]))
            hdr_ctx.pos++;
        skip_whitespace_and_comments(&hdr_ctx);

        /* offset (relative to /First) */
        size_t ostart = hdr_ctx.pos;
        while (hdr_ctx.pos < hdr_ctx.len && is_digit(hdr_ctx.base[hdr_ctx.pos]))
            hdr_ctx.pos++;
        char obuf[32];
        size_t olen = hdr_ctx.pos - ostart;
        if (olen > 31) olen = 31;
        memcpy(obuf, hdr_ctx.base + ostart, olen);
        obuf[olen] = 0;
        offsets[i] = atoi(obuf);
    }

    /* Now parse the object at the given index */
    int obj_offset = first + offsets[obj_idx];
    free(offsets);

    if ((size_t)obj_offset >= slen)
        return NULL;

    ParseCtx obj_ctx = { sdata, slen, (size_t)obj_offset };
    PdfObj *result = parse_object(&obj_ctx);

    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stream decoding (public API)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool pdf_decode_stream(PdfDocument *doc, PdfStream *stream) {
    if (!stream) return false;

    /* Already decoded? */
    if (stream->decoded_data) return true;

    if (!stream->raw_data || stream->raw_length == 0) {
        /* No data to decode - treat as empty */
        stream->decoded_data = (uint8_t *)calloc(1, 1);
        stream->decoded_length = 0;
        return true;
    }

    /* Decrypt stream data if the document is encrypted.
     * Decryption must happen before any filter decompression.
     * Note: XRef streams (/Type /XRef) are not encrypted per PDF spec.
     * The decrypted flag prevents double-decryption on retry. */
    if (doc && doc->crypt && pdf_crypt_is_encrypted(doc->crypt) && !stream->decrypted) {
        stream->decrypted = true;
        bool is_xref_stream = false;
        if (stream->dict) {
            const char *stype = pdf_dict_get_name(stream->dict, "Type");
            if (stype && strcmp(stype, "XRef") == 0)
                is_xref_stream = true;
        }
        if (!is_xref_stream) {
            pdf_crypt_decrypt_stream(doc->crypt,
                                     stream->obj_num, stream->gen_num,
                                     stream->raw_data, stream->raw_length);
        }
    }

    PdfDict *dict = stream->dict;
    PdfObj *filter_obj = dict ? pdf_dict_get(dict, "Filter") : NULL;

    /* Resolve indirect reference on filter */
    if (filter_obj && filter_obj->type == PDF_OBJ_REF)
        filter_obj = pdf_resolve(doc, filter_obj);

    /* Collect filter names */
    const char *filters[16];
    int filter_count = 0;

    if (!filter_obj) {
        /* No filter: data is uncompressed */
        stream->decoded_data = (uint8_t *)malloc(stream->raw_length);
        if (!stream->decoded_data) return false;
        memcpy(stream->decoded_data, stream->raw_data, stream->raw_length);
        stream->decoded_length = stream->raw_length;
        return true;
    }

    if (filter_obj->type == PDF_OBJ_NAME) {
        filters[0] = filter_obj->name;
        filter_count = 1;
    } else if (filter_obj->type == PDF_OBJ_ARRAY) {
        PdfArray *fa = filter_obj->array;
        for (int i = 0; i < fa->count && i < 16; i++) {
            PdfObj *f = fa->items[i];
            if (f && f->type == PDF_OBJ_REF)
                f = pdf_resolve(doc, f);
            if (f && f->type == PDF_OBJ_NAME)
                filters[filter_count++] = f->name;
        }
    }

    /* Collect DecodeParms */
    PdfObj *parms_obj = dict ? pdf_dict_get(dict, "DecodeParms") : NULL;
    if (parms_obj && parms_obj->type == PDF_OBJ_REF)
        parms_obj = pdf_resolve(doc, parms_obj);

    PdfDict *parms_array[16] = {NULL};
    if (parms_obj && parms_obj->type == PDF_OBJ_DICT) {
        parms_array[0] = parms_obj->dict;
    } else if (parms_obj && parms_obj->type == PDF_OBJ_ARRAY) {
        for (int i = 0; i < parms_obj->array->count && i < 16; i++) {
            PdfObj *p = parms_obj->array->items[i];
            if (p && p->type == PDF_OBJ_REF)
                p = pdf_resolve(doc, p);
            if (p && p->type == PDF_OBJ_DICT)
                parms_array[i] = p->dict;
        }
    }

    /* Apply filters in order */
    uint8_t *current_data = stream->raw_data;
    size_t current_len = stream->raw_length;
    bool own_data = false; /* whether current_data needs to be freed */

    for (int i = 0; i < filter_count; i++) {
        const char *fname = filters[i];
        uint8_t *out = NULL;
        size_t out_len = 0;

        if (strcmp(fname, "FlateDecode") == 0 || strcmp(fname, "Fl") == 0) {
            if (!pdf_inflate(current_data, current_len, &out, &out_len)) {
                /* Try raw inflate as fallback */
                if (!pdf_inflate_raw(current_data, current_len, &out, &out_len)) {
                    if (own_data) free(current_data);
                    return false;
                }
            }
        } else if (strcmp(fname, "ASCIIHexDecode") == 0 || strcmp(fname, "AHx") == 0) {
            /* Decode ASCII hex */
            out = (uint8_t *)malloc(current_len / 2 + 1);
            if (!out) { if (own_data) free(current_data); return false; }
            out_len = 0;
            int nibble = -1;
            for (size_t j = 0; j < current_len; j++) {
                uint8_t c = current_data[j];
                if (c == '>') break;
                if (is_ws(c)) continue;
                int hv = hex_val(c);
                if (hv < 0) continue;
                if (nibble < 0) {
                    nibble = hv;
                } else {
                    out[out_len++] = (uint8_t)((nibble << 4) | hv);
                    nibble = -1;
                }
            }
            if (nibble >= 0)
                out[out_len++] = (uint8_t)(nibble << 4);
        } else if (strcmp(fname, "ASCII85Decode") == 0 || strcmp(fname, "A85") == 0) {
            /* ASCII85 decode */
            out = (uint8_t *)malloc(current_len);
            if (!out) { if (own_data) free(current_data); return false; }
            out_len = 0;
            uint32_t tuple = 0;
            int count = 0;
            for (size_t j = 0; j < current_len; j++) {
                uint8_t c = current_data[j];
                if (c == '~' && j + 1 < current_len && current_data[j + 1] == '>')
                    break;
                if (is_ws(c)) continue;
                if (c == 'z' && count == 0) {
                    /* z = 4 zero bytes */
                    if (out_len + 4 <= current_len) {
                        out[out_len++] = 0; out[out_len++] = 0;
                        out[out_len++] = 0; out[out_len++] = 0;
                    }
                    continue;
                }
                if (c < '!' || c > 'u') continue;
                tuple = tuple * 85 + (c - '!');
                count++;
                if (count == 5) {
                    out[out_len++] = (uint8_t)(tuple >> 24);
                    out[out_len++] = (uint8_t)(tuple >> 16);
                    out[out_len++] = (uint8_t)(tuple >> 8);
                    out[out_len++] = (uint8_t)(tuple);
                    tuple = 0;
                    count = 0;
                }
            }
            /* Handle remaining bytes */
            if (count > 1) {
                for (int k = count; k < 5; k++)
                    tuple = tuple * 85 + 84;
                for (int k = 0; k < count - 1; k++)
                    out[out_len++] = (uint8_t)(tuple >> (24 - k * 8));
            }
        } else {
            /* Unsupported filter - pass through */
            out = (uint8_t *)malloc(current_len);
            if (!out) { if (own_data) free(current_data); return false; }
            memcpy(out, current_data, current_len);
            out_len = current_len;
        }

        if (own_data) free(current_data);
        current_data = out;
        current_len = out_len;
        own_data = true;

        /* Apply predictor if present */
        PdfDict *parms = parms_array[i];
        if (parms) {
            int predictor = pdf_dict_get_int(parms, "Predictor", 1);
            if (predictor > 1) {
                int columns = pdf_dict_get_int(parms, "Columns", 1);
                int colors = pdf_dict_get_int(parms, "Colors", 1);
                int bpc = pdf_dict_get_int(parms, "BitsPerComponent", 8);

                uint8_t *depred_out = NULL;
                size_t depred_len = 0;
                if (pdf_depredict(current_data, current_len, predictor,
                                  columns, colors, bpc, &depred_out, &depred_len)) {
                    free(current_data);
                    current_data = depred_out;
                    current_len = depred_len;
                } /* else: leave data as-is */
            }
        }
    }

    if (!own_data) {
        /* Data was never transformed; make a copy */
        uint8_t *copy = (uint8_t *)malloc(current_len);
        if (!copy) return false;
        memcpy(copy, current_data, current_len);
        current_data = copy;
    }

    stream->decoded_data = current_data;
    stream->decoded_length = current_len;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page tree navigation
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Flatten the page tree. Walk /Kids recursively.
 * Inherit /Resources, /MediaBox, /CropBox, /Rotate from parent nodes.
 */
static bool flatten_page_tree(PdfDocument *doc, PdfObj *node,
                              PdfObj **inherited_resources,
                              PdfObj **inherited_mediabox,
                              PdfObj **inherited_cropbox,
                              PdfObj **inherited_rotate) {
    if (!node) return false;

    /* Resolve if indirect */
    node = pdf_resolve(doc, node);
    if (!node || node->type != PDF_OBJ_DICT) return false;

    PdfDict *dict = node->dict;

    /* Determine inherited values: use this node's value if present, else parent's */
    PdfObj *res = pdf_dict_get(dict, "Resources");
    if (!res) res = inherited_resources ? *inherited_resources : NULL;

    PdfObj *mbox = pdf_dict_get(dict, "MediaBox");
    if (!mbox) mbox = inherited_mediabox ? *inherited_mediabox : NULL;

    PdfObj *cbox = pdf_dict_get(dict, "CropBox");
    if (!cbox) cbox = inherited_cropbox ? *inherited_cropbox : NULL;

    PdfObj *rot = pdf_dict_get(dict, "Rotate");
    if (!rot) rot = inherited_rotate ? *inherited_rotate : NULL;

    /* Check /Type */
    const char *type = pdf_dict_get_name(dict, "Type");

    if (type && strcmp(type, "Pages") == 0) {
        /* Intermediate node: recurse into /Kids */
        PdfArray *kids = pdf_dict_get_array(dict, "Kids");
        if (!kids) return false;

        for (int i = 0; i < kids->count; i++) {
            if (!flatten_page_tree(doc, kids->items[i], &res, &mbox, &cbox, &rot))
                return false;
        }
        return true;
    }

    if ((type && strcmp(type, "Page") == 0) || !type) {
        /* Leaf page node */
        if (doc->page_count >= PDF_MAX_PAGES) return false;

        /* Inject inherited values into the page dict if not already present */
        if (res && !pdf_dict_get(dict, "Resources")) {
            /* We don't modify the original dict; instead, we store the
             * reference so it can be found via the parent. But for simplicity
             * in page access, we inject a reference. */
            if (res->type == PDF_OBJ_REF)
                dict_put(dict, "Resources", obj_ref(res->ref.obj_num, res->ref.gen_num));
            else if (res->type == PDF_OBJ_DICT) {
                /* Store a sentinel - we cannot clone easily, so we store the
                 * raw pointer. This is safe because the parent dict outlives
                 * our pages array. We use a REF-to-null trick: actually, let's
                 * just not inject and let callers walk up. For now, we don't
                 * inject complex objects. */
            }
        }
        if (mbox && !pdf_dict_get(dict, "MediaBox")) {
            if (mbox->type == PDF_OBJ_REF)
                dict_put(dict, "MediaBox", obj_ref(mbox->ref.obj_num, mbox->ref.gen_num));
            else if (mbox->type == PDF_OBJ_ARRAY) {
                /* Clone the MediaBox array */
                PdfArray *mb_copy = array_alloc();
                if (mb_copy) {
                    for (int j = 0; j < mbox->array->count; j++) {
                        PdfObj *v = mbox->array->items[j];
                        if (v) {
                            if (v->type == PDF_OBJ_INT)
                                array_push(mb_copy, obj_int(v->integer));
                            else if (v->type == PDF_OBJ_REAL)
                                array_push(mb_copy, obj_real(v->real));
                            else
                                array_push(mb_copy, obj_null());
                        }
                    }
                    dict_put(dict, "MediaBox", obj_array(mb_copy));
                }
            }
        }
        if (cbox && !pdf_dict_get(dict, "CropBox")) {
            if (cbox->type == PDF_OBJ_REF)
                dict_put(dict, "CropBox", obj_ref(cbox->ref.obj_num, cbox->ref.gen_num));
            else if (cbox->type == PDF_OBJ_ARRAY) {
                PdfArray *cb_copy = array_alloc();
                if (cb_copy) {
                    for (int j = 0; j < cbox->array->count; j++) {
                        PdfObj *v = cbox->array->items[j];
                        if (v) {
                            if (v->type == PDF_OBJ_INT)
                                array_push(cb_copy, obj_int(v->integer));
                            else if (v->type == PDF_OBJ_REAL)
                                array_push(cb_copy, obj_real(v->real));
                            else
                                array_push(cb_copy, obj_null());
                        }
                    }
                    dict_put(dict, "CropBox", obj_array(cb_copy));
                }
            }
        }
        if (rot && !pdf_dict_get(dict, "Rotate")) {
            if (rot->type == PDF_OBJ_INT)
                dict_put(dict, "Rotate", obj_int(rot->integer));
        }

        /* Add page to the flat list */
        doc->pages[doc->page_count++] = node;
        return true;
    }

    /* Unknown type: try treating as Pages if it has /Kids, else as Page */
    PdfArray *kids = pdf_dict_get_array(dict, "Kids");
    if (kids) {
        for (int i = 0; i < kids->count; i++) {
            if (!flatten_page_tree(doc, kids->items[i], &res, &mbox, &cbox, &rot))
                return false;
        }
        return true;
    }

    /* Treat as a leaf page */
    if (doc->page_count < PDF_MAX_PAGES) {
        doc->pages[doc->page_count++] = node;
    }
    return true;
}

static bool build_page_tree(PdfDocument *doc) {
    if (!doc->trailer || doc->trailer->type != PDF_OBJ_DICT)
        return false;

    PdfObj *root_ref = pdf_dict_get(doc->trailer->dict, "Root");
    PdfObj *root = pdf_resolve(doc, root_ref);
    if (!root || root->type != PDF_OBJ_DICT) return false;

    PdfObj *pages_ref = pdf_dict_get(root->dict, "Pages");
    PdfObj *pages = pdf_resolve(doc, pages_ref);
    if (!pages || pages->type != PDF_OBJ_DICT) return false;

    /* Allocate page array */
    int expected_count = pdf_dict_get_int(pages->dict, "Count", 64);
    if (expected_count <= 0) expected_count = 64;
    if (expected_count > PDF_MAX_PAGES) expected_count = PDF_MAX_PAGES;

    doc->pages = (PdfObj **)calloc(expected_count + 64, sizeof(PdfObj *));
    if (!doc->pages) return false;
    doc->page_count = 0;

    return flatten_page_tree(doc, pages, NULL, NULL, NULL, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page access (public API)
 * ═══════════════════════════════════════════════════════════════════════════ */

int pdf_page_count(PdfDocument *doc) {
    return doc ? doc->page_count : 0;
}

PdfObj *pdf_get_page(PdfDocument *doc, int page_idx) {
    if (!doc || page_idx < 0 || page_idx >= doc->page_count)
        return NULL;
    return doc->pages[page_idx];
}

/* ═══════════════════════════════════════════════════════════════════════════
 * pdf_open / pdf_close (public API)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool pdf_open(PdfDocument *doc, const wchar_t *path) {
    if (!doc || !path) return false;

    memset(doc, 0, sizeof(PdfDocument));
    doc->hFile = INVALID_HANDLE_VALUE;
    doc->hMapping = NULL;

    /* ─── Open and memory-map the file ─── */
    doc->hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (doc->hFile == INVALID_HANDLE_VALUE)
        goto fail;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(doc->hFile, &file_size) || file_size.QuadPart == 0)
        goto fail;

    doc->data_len = (size_t)file_size.QuadPart;

    doc->hMapping = CreateFileMappingW(doc->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!doc->hMapping)
        goto fail;

    doc->data = (const uint8_t *)MapViewOfFile(doc->hMapping, FILE_MAP_READ,
                                               0, 0, 0);
    if (!doc->data)
        goto fail;

    /* ─── Initialize the object cache ─── */
    cache_init(doc);

    /* ─── Parse PDF header ─── */
    if (doc->data_len < 8)
        goto fail;

    /* Find %PDF-x.y (may not be at offset 0 for linearized PDFs) */
    size_t hdr_offset = 0;
    bool found_header = false;
    size_t hdr_search_limit = (doc->data_len > 1024) ? 1024 : doc->data_len;
    for (size_t i = 0; i + 5 <= hdr_search_limit; i++) {
        if (doc->data[i] == '%' && doc->data[i + 1] == 'P' &&
            doc->data[i + 2] == 'D' && doc->data[i + 3] == 'F' &&
            doc->data[i + 4] == '-') {
            hdr_offset = i;
            found_header = true;
            break;
        }
    }

    if (!found_header)
        goto fail;

    /* Parse version */
    doc->version_major = 1;
    doc->version_minor = 0;
    if (hdr_offset + 7 <= doc->data_len) {
        if (is_digit(doc->data[hdr_offset + 5]) && doc->data[hdr_offset + 6] == '.' &&
            is_digit(doc->data[hdr_offset + 7])) {
            doc->version_major = doc->data[hdr_offset + 5] - '0';
            doc->version_minor = doc->data[hdr_offset + 7] - '0';
        }
    }

    /* ─── Find startxref ─── */
    int64_t startxref = find_startxref(doc->data, doc->data_len);
    if (startxref < 0 || (size_t)startxref >= doc->data_len)
        goto fail;

    /* ─── Initialize xref table ─── */
    doc->xref = (XRefEntry *)calloc(INITIAL_XREF_SIZE, sizeof(XRefEntry));
    if (!doc->xref) goto fail;
    doc->xref_count = INITIAL_XREF_SIZE;
    for (int i = 0; i < doc->xref_count; i++) {
        doc->xref[i].obj_stream_num = -1;
        doc->xref[i].gen_num = 65535;
    }

    /* ─── Parse xref ─── */
    ParseCtx ctx = { doc->data, doc->data_len, (size_t)startxref };
    skip_whitespace_and_comments(&ctx);

    if (ctx.pos + 4 <= ctx.len &&
        memcmp(ctx.base + ctx.pos, "xref", 4) == 0) {
        /* Classic xref table */
        ctx.pos += 4;

        if (!parse_xref_section(&ctx, doc))
            goto fail;

        /* Parse trailer */
        skip_whitespace_and_comments(&ctx);
        if (ctx.pos + 7 <= ctx.len &&
            memcmp(ctx.base + ctx.pos, "trailer", 7) == 0) {
            ctx.pos += 7;
            skip_whitespace_and_comments(&ctx);
            doc->trailer = parse_object(&ctx);
        }

        if (!doc->trailer || doc->trailer->type != PDF_OBJ_DICT)
            goto fail;

        /* Handle /Prev for incremental updates */
        int prev = pdf_dict_get_int(doc->trailer->dict, "Prev", -1);
        while (prev > 0 && (size_t)prev < doc->data_len) {
            ParseCtx prev_ctx = { doc->data, doc->data_len, (size_t)prev };
            skip_whitespace_and_comments(&prev_ctx);

            if (prev_ctx.pos + 4 <= prev_ctx.len &&
                memcmp(prev_ctx.base + prev_ctx.pos, "xref", 4) == 0) {
                prev_ctx.pos += 4;
                parse_xref_section(&prev_ctx, doc);

                /* Read the previous section's trailer to find its /Prev */
                skip_whitespace_and_comments(&prev_ctx);
                if (prev_ctx.pos + 7 <= prev_ctx.len &&
                    memcmp(prev_ctx.base + prev_ctx.pos, "trailer", 7) == 0) {
                    prev_ctx.pos += 7;
                    skip_whitespace_and_comments(&prev_ctx);
                    PdfObj *prev_trailer = parse_object(&prev_ctx);
                    if (prev_trailer && prev_trailer->type == PDF_OBJ_DICT)
                        prev = pdf_dict_get_int(prev_trailer->dict, "Prev", -1);
                    else
                        prev = -1;
                    pdf_free_obj(prev_trailer);
                } else {
                    prev = -1;
                }
            } else {
                /* Might be an xref stream at this offset */
                parse_xref_stream(&prev_ctx, doc, NULL);
                prev = -1; /* xref_stream handles its own /Prev */
            }
        }

        /* Ensure /Size is respected */
        int size = pdf_dict_get_int(doc->trailer->dict, "Size", 0);
        if (size > 0)
            xref_ensure(doc, size);

    } else {
        /* xref stream (PDF 1.5+) */
        if (!parse_xref_stream(&ctx, doc, &doc->trailer))
            goto fail;

        if (!doc->trailer)
            goto fail;
    }

    /* ─── Initialize encryption (if present) ─── */
    {
        PdfCryptState *crypt = (PdfCryptState *)calloc(1, sizeof(PdfCryptState));
        if (crypt) {
            if (!pdf_crypt_init(crypt, doc->trailer, doc)) {
                /* Encryption requires a password we can't provide */
                free(crypt);
                /* Continue anyway -- some objects may be accessible */
            } else {
                if (pdf_crypt_is_encrypted(crypt)) {
                    doc->crypt = crypt;
                } else {
                    free(crypt);
                }
            }
        }
    }

    /* ─── Build page tree ─── */
    if (!build_page_tree(doc))
        goto fail;

    return true;

fail:
    pdf_close(doc);
    return false;
}

void pdf_close(PdfDocument *doc) {
    if (!doc) return;

    /* Free encryption state */
    free(doc->crypt);
    doc->crypt = NULL;

    /* Free pages array (page objects are owned by the cache, not freed here) */
    free(doc->pages);
    doc->pages = NULL;
    doc->page_count = 0;

    /* Destroy object cache (frees all cached objects) */
    cache_destroy();

    /* Free trailer (only if it wasn't cached - it's parsed separately) */
    pdf_free_obj(doc->trailer);
    doc->trailer = NULL;

    /* Free xref table */
    free(doc->xref);
    doc->xref = NULL;
    doc->xref_count = 0;

    /* Unmap and close file */
    if (doc->data) {
        UnmapViewOfFile(doc->data);
        doc->data = NULL;
    }
    if (doc->hMapping) {
        CloseHandle(doc->hMapping);
        doc->hMapping = NULL;
    }
    if (doc->hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(doc->hFile);
        doc->hFile = INVALID_HANDLE_VALUE;
    }

    doc->data_len = 0;
}
