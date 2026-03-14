/*
 * AmundsPDF - pdf_crypt.c
 * PDF Standard Security Handler decryption (V1/V2, R2/R3, RC4).
 *
 * Implements:
 *   - MD5 hash computation (RFC 1321)
 *   - RC4 stream cipher
 *   - PDF file encryption key computation (Algorithm 2, PDF Reference 1.7)
 *   - Per-object key derivation (Algorithm 1)
 *   - Password validation (Algorithm 4/5)
 *
 * Zero external dependencies: self-contained MD5 and RC4 implementations.
 */

#include "pdf_crypt.h"
#include "pdf_parser.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * MD5 Implementation (RFC 1321)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} MD5_CTX;

/* Constants for MD5Transform */
#define MD5_S11 7
#define MD5_S12 12
#define MD5_S13 17
#define MD5_S14 22
#define MD5_S21 5
#define MD5_S22 9
#define MD5_S23 14
#define MD5_S24 20
#define MD5_S31 4
#define MD5_S32 11
#define MD5_S33 16
#define MD5_S34 23
#define MD5_S41 6
#define MD5_S42 10
#define MD5_S43 15
#define MD5_S44 21

/* MD5 basic functions */
#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac) { \
    (a) += MD5_F((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL32((a), (s)); \
    (a) += (b); \
}
#define MD5_GG(a, b, c, d, x, s, ac) { \
    (a) += MD5_G((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL32((a), (s)); \
    (a) += (b); \
}
#define MD5_HH(a, b, c, d, x, s, ac) { \
    (a) += MD5_H((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL32((a), (s)); \
    (a) += (b); \
}
#define MD5_II(a, b, c, d, x, s, ac) { \
    (a) += MD5_I((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL32((a), (s)); \
    (a) += (b); \
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];

    /* Decode block (little-endian) */
    for (int i = 0; i < 16; i++) {
        x[i] = (uint32_t)block[i*4]
             | ((uint32_t)block[i*4+1] << 8)
             | ((uint32_t)block[i*4+2] << 16)
             | ((uint32_t)block[i*4+3] << 24);
    }

    /* Round 1 */
    MD5_FF(a, b, c, d, x[ 0], MD5_S11, 0xd76aa478);
    MD5_FF(d, a, b, c, x[ 1], MD5_S12, 0xe8c7b756);
    MD5_FF(c, d, a, b, x[ 2], MD5_S13, 0x242070db);
    MD5_FF(b, c, d, a, x[ 3], MD5_S14, 0xc1bdceee);
    MD5_FF(a, b, c, d, x[ 4], MD5_S11, 0xf57c0faf);
    MD5_FF(d, a, b, c, x[ 5], MD5_S12, 0x4787c62a);
    MD5_FF(c, d, a, b, x[ 6], MD5_S13, 0xa8304613);
    MD5_FF(b, c, d, a, x[ 7], MD5_S14, 0xfd469501);
    MD5_FF(a, b, c, d, x[ 8], MD5_S11, 0x698098d8);
    MD5_FF(d, a, b, c, x[ 9], MD5_S12, 0x8b44f7af);
    MD5_FF(c, d, a, b, x[10], MD5_S13, 0xffff5bb1);
    MD5_FF(b, c, d, a, x[11], MD5_S14, 0x895cd7be);
    MD5_FF(a, b, c, d, x[12], MD5_S11, 0x6b901122);
    MD5_FF(d, a, b, c, x[13], MD5_S12, 0xfd987193);
    MD5_FF(c, d, a, b, x[14], MD5_S13, 0xa679438e);
    MD5_FF(b, c, d, a, x[15], MD5_S14, 0x49b40821);

    /* Round 2 */
    MD5_GG(a, b, c, d, x[ 1], MD5_S21, 0xf61e2562);
    MD5_GG(d, a, b, c, x[ 6], MD5_S22, 0xc040b340);
    MD5_GG(c, d, a, b, x[11], MD5_S23, 0x265e5a51);
    MD5_GG(b, c, d, a, x[ 0], MD5_S24, 0xe9b6c7aa);
    MD5_GG(a, b, c, d, x[ 5], MD5_S21, 0xd62f105d);
    MD5_GG(d, a, b, c, x[10], MD5_S22, 0x02441453);
    MD5_GG(c, d, a, b, x[15], MD5_S23, 0xd8a1e681);
    MD5_GG(b, c, d, a, x[ 4], MD5_S24, 0xe7d3fbc8);
    MD5_GG(a, b, c, d, x[ 9], MD5_S21, 0x21e1cde6);
    MD5_GG(d, a, b, c, x[14], MD5_S22, 0xc33707d6);
    MD5_GG(c, d, a, b, x[ 3], MD5_S23, 0xf4d50d87);
    MD5_GG(b, c, d, a, x[ 8], MD5_S24, 0x455a14ed);
    MD5_GG(a, b, c, d, x[13], MD5_S21, 0xa9e3e905);
    MD5_GG(d, a, b, c, x[ 2], MD5_S22, 0xfcefa3f8);
    MD5_GG(c, d, a, b, x[ 7], MD5_S23, 0x676f02d9);
    MD5_GG(b, c, d, a, x[12], MD5_S24, 0x8d2a4c8a);

    /* Round 3 */
    MD5_HH(a, b, c, d, x[ 5], MD5_S31, 0xfffa3942);
    MD5_HH(d, a, b, c, x[ 8], MD5_S32, 0x8771f681);
    MD5_HH(c, d, a, b, x[11], MD5_S33, 0x6d9d6122);
    MD5_HH(b, c, d, a, x[14], MD5_S34, 0xfde5380c);
    MD5_HH(a, b, c, d, x[ 1], MD5_S31, 0xa4beea44);
    MD5_HH(d, a, b, c, x[ 4], MD5_S32, 0x4bdecfa9);
    MD5_HH(c, d, a, b, x[ 7], MD5_S33, 0xf6bb4b60);
    MD5_HH(b, c, d, a, x[10], MD5_S34, 0xbebfbc70);
    MD5_HH(a, b, c, d, x[13], MD5_S31, 0x289b7ec6);
    MD5_HH(d, a, b, c, x[ 0], MD5_S32, 0xeaa127fa);
    MD5_HH(c, d, a, b, x[ 3], MD5_S33, 0xd4ef3085);
    MD5_HH(b, c, d, a, x[ 6], MD5_S34, 0x04881d05);
    MD5_HH(a, b, c, d, x[ 9], MD5_S31, 0xd9d4d039);
    MD5_HH(d, a, b, c, x[12], MD5_S32, 0xe6db99e5);
    MD5_HH(c, d, a, b, x[15], MD5_S33, 0x1fa27cf8);
    MD5_HH(b, c, d, a, x[ 2], MD5_S34, 0xc4ac5665);

    /* Round 4 */
    MD5_II(a, b, c, d, x[ 0], MD5_S41, 0xf4292244);
    MD5_II(d, a, b, c, x[ 7], MD5_S42, 0x432aff97);
    MD5_II(c, d, a, b, x[14], MD5_S43, 0xab9423a7);
    MD5_II(b, c, d, a, x[ 5], MD5_S44, 0xfc93a039);
    MD5_II(a, b, c, d, x[12], MD5_S41, 0x655b59c3);
    MD5_II(d, a, b, c, x[ 3], MD5_S42, 0x8f0ccc92);
    MD5_II(c, d, a, b, x[10], MD5_S43, 0xffeff47d);
    MD5_II(b, c, d, a, x[ 1], MD5_S44, 0x85845dd1);
    MD5_II(a, b, c, d, x[ 8], MD5_S41, 0x6fa87e4f);
    MD5_II(d, a, b, c, x[15], MD5_S42, 0xfe2ce6e0);
    MD5_II(c, d, a, b, x[ 6], MD5_S43, 0xa3014314);
    MD5_II(b, c, d, a, x[13], MD5_S44, 0x4e0811a1);
    MD5_II(a, b, c, d, x[ 4], MD5_S41, 0xf7537e82);
    MD5_II(d, a, b, c, x[11], MD5_S42, 0xbd3af235);
    MD5_II(c, d, a, b, x[ 2], MD5_S43, 0x2ad7d2bb);
    MD5_II(b, c, d, a, x[ 9], MD5_S44, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(MD5_CTX *ctx)
{
    ctx->count = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(MD5_CTX *ctx, const uint8_t *data, size_t len)
{
    size_t index = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    size_t i = 0;
    if (index) {
        size_t part_len = 64 - index;
        if (len >= part_len) {
            memcpy(ctx->buffer + index, data, part_len);
            md5_transform(ctx->state, ctx->buffer);
            i = part_len;
        } else {
            memcpy(ctx->buffer + index, data, len);
            return;
        }
    }

    for (; i + 64 <= len; i += 64)
        md5_transform(ctx->state, data + i);

    if (i < len)
        memcpy(ctx->buffer, data + i, len - i);
}

static void md5_final(MD5_CTX *ctx, uint8_t digest[16])
{
    static const uint8_t padding[64] = { 0x80, 0 };

    /* Encode length (before padding) as 64-bit little-endian */
    uint8_t bits[8];
    uint64_t bit_count = ctx->count * 8;
    for (int i = 0; i < 8; i++)
        bits[i] = (uint8_t)(bit_count >> (i * 8));

    /* Pad to 56 mod 64 */
    size_t index = (size_t)(ctx->count & 0x3F);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    md5_update(ctx, padding, pad_len);

    /* Append length */
    md5_update(ctx, bits, 8);

    /* Encode state as digest (little-endian) */
    for (int i = 0; i < 4; i++) {
        digest[i*4+0] = (uint8_t)(ctx->state[i]);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

/* Convenience: hash a block and return the digest */
static void md5_hash(const uint8_t *data, size_t len, uint8_t digest[16])
{
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, digest);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RC4 Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t S[256];
    int     i, j;
} RC4_CTX;

static void rc4_init(RC4_CTX *ctx, const uint8_t *key, int key_len)
{
    for (int i = 0; i < 256; i++)
        ctx->S[i] = (uint8_t)i;

    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + ctx->S[i] + key[i % key_len]) & 0xFF;
        uint8_t tmp = ctx->S[i];
        ctx->S[i] = ctx->S[j];
        ctx->S[j] = tmp;
    }
    ctx->i = 0;
    ctx->j = 0;
}

static void rc4_crypt(RC4_CTX *ctx, uint8_t *data, size_t len)
{
    int i = ctx->i;
    int j = ctx->j;
    for (size_t k = 0; k < len; k++) {
        i = (i + 1) & 0xFF;
        j = (j + ctx->S[i]) & 0xFF;
        uint8_t tmp = ctx->S[i];
        ctx->S[i] = ctx->S[j];
        ctx->S[j] = tmp;
        data[k] ^= ctx->S[(ctx->S[i] + ctx->S[j]) & 0xFF];
    }
    ctx->i = i;
    ctx->j = j;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PDF Standard Security Handler - Key Computation
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PDF Reference 1.7, Algorithm 2: Computing an encryption key.
 *
 * 1. Pad or truncate the password to exactly 32 bytes using the padding string.
 * 2. Initialize the MD5 hash with the padded password.
 * 3. Pass the value of the /O entry to the MD5 hash.
 * 4. Treat the /P value as a 4-byte little-endian integer and pass those bytes.
 * 5. Pass the first element of the /ID array.
 * 6. (If R>=4 and metadata not encrypted, pass 4 bytes 0xFFFFFFFF)
 * 7. Finish the hash. The first n bytes are the encryption key.
 * 8. (If R>=3, do 50 additional MD5 iterations on the first n bytes.)
 */

/* The 32-byte padding string defined in the PDF specification */
static const uint8_t pdf_password_padding[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
    0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
    0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A
};

/*
 * Compute the file encryption key.
 * password: user password (NULL or "" for empty password)
 * password_len: length of password (0 for empty)
 */
static bool compute_file_key(PdfCryptState *state,
                              const uint8_t *password, int password_len)
{
    MD5_CTX md5;
    md5_init(&md5);

    /* Step 1-2: Pad password to 32 bytes and hash */
    uint8_t padded[32];
    if (password_len > 0 && password) {
        int copy_len = password_len < 32 ? password_len : 32;
        memcpy(padded, password, copy_len);
        if (copy_len < 32)
            memcpy(padded + copy_len, pdf_password_padding, 32 - copy_len);
    } else {
        memcpy(padded, pdf_password_padding, 32);
    }
    md5_update(&md5, padded, 32);

    /* Step 3: Hash the /O value */
    md5_update(&md5, state->o_value, 32);

    /* Step 4: Hash the /P value as 4-byte little-endian */
    uint8_t p_bytes[4];
    p_bytes[0] = (uint8_t)(state->permissions);
    p_bytes[1] = (uint8_t)(state->permissions >> 8);
    p_bytes[2] = (uint8_t)(state->permissions >> 16);
    p_bytes[3] = (uint8_t)(state->permissions >> 24);
    md5_update(&md5, p_bytes, 4);

    /* Step 5: Hash the first ID element */
    md5_update(&md5, state->doc_id, state->doc_id_len);

    /* Step 6: For R>=4 and metadata not encrypted, hash 0xFFFFFFFF.
     * We don't support R4 metadata encryption flag, so skip. */

    /* Step 7: Finish hash */
    uint8_t digest[16];
    md5_final(&md5, digest);

    /* Step 8: For R>=3, iterate MD5 50 times on first key_length bytes */
    if (state->revision >= 3) {
        for (int i = 0; i < 50; i++) {
            md5_hash(digest, state->key_length, digest);
        }
    }

    /* The file encryption key is the first key_length bytes of the digest */
    memcpy(state->file_key, digest, state->key_length);
    return true;
}

/*
 * Validate the user password (Algorithm 4/5 from PDF Reference).
 * For R=2: encrypt the padding string with RC4 using the file key,
 *          compare with /U value.
 * For R=3: MD5(padding + ID[0]), encrypt with RC4, then 19 more passes
 *          with key XOR'd with iteration number. Compare first 16 bytes of /U.
 */
static bool validate_user_password(const PdfCryptState *state)
{
    if (state->revision == 2) {
        /* Algorithm 4: RC4-encrypt the padding string with the file key */
        uint8_t result[32];
        memcpy(result, pdf_password_padding, 32);
        RC4_CTX rc4;
        rc4_init(&rc4, state->file_key, state->key_length);
        rc4_crypt(&rc4, result, 32);
        return memcmp(result, state->u_value, 32) == 0;
    }
    else if (state->revision == 3) {
        /* Algorithm 5: MD5(padding + ID[0]) */
        uint8_t hash[16];
        MD5_CTX md5;
        md5_init(&md5);
        md5_update(&md5, pdf_password_padding, 32);
        md5_update(&md5, state->doc_id, state->doc_id_len);
        md5_final(&md5, hash);

        /* RC4-encrypt with the file key */
        RC4_CTX rc4;
        rc4_init(&rc4, state->file_key, state->key_length);
        rc4_crypt(&rc4, hash, 16);

        /* 19 additional RC4 passes with XOR'd key */
        for (int i = 1; i <= 19; i++) {
            uint8_t xor_key[16];
            for (int k = 0; k < state->key_length; k++)
                xor_key[k] = state->file_key[k] ^ (uint8_t)i;
            rc4_init(&rc4, xor_key, state->key_length);
            rc4_crypt(&rc4, hash, 16);
        }

        /* Compare first 16 bytes of result with /U value */
        return memcmp(hash, state->u_value, 16) == 0;
    }

    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Parse hex string from a PdfObj string value into binary.
 */
static void parse_hex_obj(PdfObj *obj, uint8_t *out, int max_len)
{
    if (!obj) return;
    obj = pdf_resolve(NULL, obj);  /* might be a ref, but we pass NULL doc for now */
    if (!obj) return;
    if (obj->type == PDF_OBJ_STRING && obj->string.data) {
        int copy_len = (int)obj->string.length;
        if (copy_len > max_len) copy_len = max_len;
        memcpy(out, obj->string.data, copy_len);
    }
}

bool pdf_crypt_init(PdfCryptState *state, PdfObj *trailer, PdfDocument *doc)
{
    memset(state, 0, sizeof(*state));
    state->enabled = false;

    if (!trailer || !doc) return true;

    /* Look for /Encrypt in trailer */
    PdfDict *trailer_dict = NULL;
    if (trailer->type == PDF_OBJ_DICT)
        trailer_dict = trailer->dict;
    else
        return true;

    PdfObj *encrypt_ref = pdf_dict_get(trailer_dict, "Encrypt");
    if (!encrypt_ref) return true;  /* Not encrypted */

    /* Remember the encrypt object number so we don't decrypt it */
    if (encrypt_ref->type == PDF_OBJ_REF)
        state->encrypt_obj_num = encrypt_ref->ref.obj_num;

    PdfObj *encrypt_obj = pdf_resolve(doc, encrypt_ref);
    if (!encrypt_obj || encrypt_obj->type != PDF_OBJ_DICT)
        return false;

    PdfDict *enc = encrypt_obj->dict;

    /* Check /Filter - must be /Standard */
    const char *filter = pdf_dict_get_name(enc, "Filter");
    if (!filter || strcmp(filter, "Standard") != 0)
        return false;  /* Unsupported encryption handler */

    /* Parse encryption parameters */
    state->version = pdf_dict_get_int(enc, "V", 0);
    state->revision = pdf_dict_get_int(enc, "R", 2);
    int length_bits = pdf_dict_get_int(enc, "Length", 40);
    state->key_length = length_bits / 8;
    if (state->key_length < 5) state->key_length = 5;
    if (state->key_length > 16) state->key_length = 16;
    state->permissions = pdf_dict_get_int(enc, "P", 0);

    /* Get /O value (32 bytes) */
    PdfObj *o_obj = pdf_dict_get(enc, "O");
    if (o_obj) {
        o_obj = pdf_resolve(doc, o_obj);
        if (o_obj && o_obj->type == PDF_OBJ_STRING && o_obj->string.data) {
            int copy_len = (int)o_obj->string.length;
            if (copy_len > 32) copy_len = 32;
            memcpy(state->o_value, o_obj->string.data, copy_len);
        }
    }

    /* Get /U value (32 bytes) */
    PdfObj *u_obj = pdf_dict_get(enc, "U");
    if (u_obj) {
        u_obj = pdf_resolve(doc, u_obj);
        if (u_obj && u_obj->type == PDF_OBJ_STRING && u_obj->string.data) {
            int copy_len = (int)u_obj->string.length;
            if (copy_len > 32) copy_len = 32;
            memcpy(state->u_value, u_obj->string.data, copy_len);
        }
    }

    /* Get /ID from trailer (first element) */
    PdfObj *id_obj = pdf_dict_get(trailer_dict, "ID");
    if (id_obj) {
        id_obj = pdf_resolve(doc, id_obj);
        if (id_obj && id_obj->type == PDF_OBJ_ARRAY && pdf_array_len(id_obj->array) >= 1) {
            PdfObj *id0 = pdf_array_get(id_obj->array, 0);
            if (id0) {
                id0 = pdf_resolve(doc, id0);
                if (id0 && id0->type == PDF_OBJ_STRING && id0->string.data) {
                    int copy_len = (int)id0->string.length;
                    if (copy_len > 16) copy_len = 16;
                    memcpy(state->doc_id, id0->string.data, copy_len);
                    state->doc_id_len = copy_len;
                }
            }
        }
    }

    /* Compute the file encryption key using empty password */
    compute_file_key(state, NULL, 0);

    /* Validate the user password */
    if (!validate_user_password(state)) {
        /* Empty password didn't work. We don't support password prompts,
         * so mark as encrypted but we can't decrypt. */
        return false;
    }

    state->enabled = true;
    return true;
}

bool pdf_crypt_is_encrypted(const PdfCryptState *state)
{
    return state && state->enabled;
}

/*
 * Compute per-object encryption key (Algorithm 1 from PDF Reference).
 *
 * For each object:
 * 1. Take the file encryption key.
 * 2. Append the low-order 3 bytes of the object number (little-endian).
 * 3. Append the low-order 2 bytes of the generation number (little-endian).
 * 4. MD5 hash this value.
 * 5. Use the first min(n+5, 16) bytes as the RC4 key.
 */
static int compute_object_key(const PdfCryptState *state,
                               int obj_num, int gen_num,
                               uint8_t obj_key[16])
{
    uint8_t buf[21]; /* max 16 + 3 + 2 = 21 bytes */
    int n = state->key_length;

    memcpy(buf, state->file_key, n);
    buf[n+0] = (uint8_t)(obj_num);
    buf[n+1] = (uint8_t)(obj_num >> 8);
    buf[n+2] = (uint8_t)(obj_num >> 16);
    buf[n+3] = (uint8_t)(gen_num);
    buf[n+4] = (uint8_t)(gen_num >> 8);

    uint8_t digest[16];
    md5_hash(buf, n + 5, digest);

    int obj_key_len = n + 5;
    if (obj_key_len > 16) obj_key_len = 16;
    memcpy(obj_key, digest, obj_key_len);
    return obj_key_len;
}

void pdf_crypt_decrypt_stream(const PdfCryptState *state,
                               int obj_num, int gen_num,
                               uint8_t *data, size_t length)
{
    if (!state || !state->enabled || !data || length == 0) return;

    /* Don't decrypt the /Encrypt dict itself */
    if (obj_num == state->encrypt_obj_num) return;

    uint8_t obj_key[16];
    int obj_key_len = compute_object_key(state, obj_num, gen_num, obj_key);

    RC4_CTX rc4;
    rc4_init(&rc4, obj_key, obj_key_len);
    rc4_crypt(&rc4, data, length);
}

void pdf_crypt_decrypt_string(const PdfCryptState *state,
                               int obj_num, int gen_num,
                               uint8_t *data, size_t length)
{
    if (!state || !state->enabled || !data || length == 0) return;

    /* Don't decrypt strings in the /Encrypt dict */
    if (obj_num == state->encrypt_obj_num) return;

    uint8_t obj_key[16];
    int obj_key_len = compute_object_key(state, obj_num, gen_num, obj_key);

    RC4_CTX rc4;
    rc4_init(&rc4, obj_key, obj_key_len);
    rc4_crypt(&rc4, data, length);
}
