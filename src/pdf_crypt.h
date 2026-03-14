/*
 * AmundsPDF - pdf_crypt.h
 * PDF Standard Security Handler decryption (V1/V2, R2/R3, RC4 40/128-bit).
 * Supports empty-password PDFs and password-protected PDFs.
 * Zero external dependencies beyond standard C + Win32.
 */
#ifndef PDF_CRYPT_H
#define PDF_CRYPT_H

#include "pdf_types.h"

/* ─── Encryption State ─── */

struct PdfCryptState {
    bool        enabled;            /* true if document is encrypted */
    int         version;            /* /V value (1 or 2) */
    int         revision;           /* /R value (2 or 3) */
    int         key_length;         /* encryption key length in bytes (5 or 16) */
    uint8_t     file_key[16];       /* computed file encryption key */
    uint8_t     o_value[32];        /* /O value */
    uint8_t     u_value[32];        /* /U value */
    int32_t     permissions;        /* /P value */
    uint8_t     doc_id[16];         /* first element of /ID array */
    int         doc_id_len;         /* length of doc_id (usually 16) */
    int         encrypt_obj_num;    /* object number of /Encrypt dict (exempt from decryption) */
};

/*
 * pdf_crypt_init - Initialize decryption from the trailer's /Encrypt dict and /ID.
 *
 * Parses the encryption parameters, computes the file encryption key
 * using the empty password, and verifies against the /U value.
 *
 * Returns true if encryption is detected and the key was computed successfully
 * (or if the document is not encrypted). Returns false on error (e.g., password
 * required but not empty).
 *
 * After calling this, use pdf_crypt_is_encrypted() to check if decryption is needed.
 */
bool pdf_crypt_init(PdfCryptState *state, PdfObj *trailer, PdfDocument *doc);

/*
 * pdf_crypt_is_encrypted - Check if the document uses encryption.
 */
bool pdf_crypt_is_encrypted(const PdfCryptState *state);

/*
 * pdf_crypt_decrypt_stream - Decrypt a stream's raw data in place.
 *
 * obj_num, gen_num: the object/generation number of the stream object.
 * data: the raw stream data to decrypt (modified in place).
 * length: the length of the data.
 *
 * The /Encrypt dictionary object itself is never encrypted.
 * Streams with /Type /XRef are never encrypted.
 */
void pdf_crypt_decrypt_stream(const PdfCryptState *state,
                               int obj_num, int gen_num,
                               uint8_t *data, size_t length);

/*
 * pdf_crypt_decrypt_string - Decrypt a string value in place.
 *
 * obj_num, gen_num: the object/generation number containing this string.
 * data: the string data to decrypt (modified in place).
 * length: the length of the data.
 */
void pdf_crypt_decrypt_string(const PdfCryptState *state,
                               int obj_num, int gen_num,
                               uint8_t *data, size_t length);

#endif /* PDF_CRYPT_H */
