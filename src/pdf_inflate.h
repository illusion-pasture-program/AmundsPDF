/*
 * AmundsPDF - pdf_inflate.h
 * RFC 1951 DEFLATE inflate decoder and PNG de-predictor for FlateDecode.
 * Zero external dependencies beyond standard C + Win32.
 */
#ifndef PDF_INFLATE_H
#define PDF_INFLATE_H

#include "pdf_types.h"

/*
 * pdf_inflate - Decompress zlib-wrapped deflate data (RFC 1950 + RFC 1951).
 *
 * Expects the standard 2-byte zlib header and 4-byte Adler-32 trailer.
 * Allocates the output buffer; caller must free() it.
 *
 * Returns true on success, false on any error (bad data, OOM, checksum mismatch).
 */
bool pdf_inflate(const uint8_t *src, size_t src_len,
                 uint8_t **out, size_t *out_len);

/*
 * pdf_inflate_raw - Decompress raw deflate data (RFC 1951, no wrapper).
 *
 * Same semantics as pdf_inflate but expects no zlib header or trailer.
 */
bool pdf_inflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t **out, size_t *out_len);

/*
 * pdf_depredict - Undo PDF predictor post-processing.
 *
 * Supports:
 *   predictor = 1            : No prediction (copies input to output)
 *   predictor = 2            : TIFF Predictor 2 (horizontal differencing)
 *   predictor = 10..15       : PNG predictors (None/Sub/Up/Average/Paeth)
 *     The per-row filter byte selects the actual algorithm.
 *
 * Parameters:
 *   data      - Decompressed (post-inflate) data
 *   data_len  - Length of data
 *   predictor - PDF Predictor value from DecodeParms
 *   columns   - Number of samples per row (default 1)
 *   colors    - Number of color components per sample (default 1)
 *   bpc       - Bits per component (default 8)
 *   out       - Receives heap-allocated output; caller frees with free()
 *   out_len   - Receives output length
 *
 * Returns true on success.
 */
bool pdf_depredict(const uint8_t *data, size_t data_len,
                   int predictor, int columns, int colors, int bpc,
                   uint8_t **out, size_t *out_len);

#endif /* PDF_INFLATE_H */
