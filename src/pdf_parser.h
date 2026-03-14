/*
 * AmundsPDF - pdf_parser.h
 * PDF file parser: tokenizer, object model, xref table, page tree.
 * Zero external dependencies beyond standard C + Win32.
 */
#ifndef PDF_PARSER_H
#define PDF_PARSER_H

#include "pdf_types.h"

/* ─── Document Lifecycle ─── */

/*
 * pdf_open - Open a PDF file, parse xref table, trailer, and page tree.
 *
 * Uses memory-mapped I/O for fast access. The document must be closed
 * with pdf_close() when no longer needed.
 *
 * Returns true on success, false on any error (file not found, malformed PDF).
 */
bool pdf_open(PdfDocument *doc, const wchar_t *path);

/*
 * pdf_close - Release all resources associated with a parsed PDF.
 *
 * Frees the page array, xref table, trailer, object cache, and unmaps the file.
 * Safe to call on a zero-initialized PdfDocument (no-op).
 */
void pdf_close(PdfDocument *doc);

/* ─── Object Access ─── */

/*
 * pdf_resolve - If obj is an indirect reference, resolve it to the actual object.
 *
 * Returns the resolved object, or obj itself if it is not a reference.
 * Returns NULL if the reference cannot be resolved.
 */
PdfObj *pdf_resolve(PdfDocument *doc, PdfObj *obj);

/*
 * pdf_get_object - Parse and return the object with the given object/generation number.
 *
 * The result is cached; subsequent calls return the cached object.
 * Returns NULL if the object does not exist.
 */
PdfObj *pdf_get_object(PdfDocument *doc, int obj_num, int gen_num);

/* ─── Dictionary Helpers ─── */

PdfObj      *pdf_dict_get(PdfDict *dict, const char *key);
int          pdf_dict_get_int(PdfDict *dict, const char *key, int default_val);
double       pdf_dict_get_real(PdfDict *dict, const char *key, double default_val);
const char  *pdf_dict_get_name(PdfDict *dict, const char *key);
PdfArray    *pdf_dict_get_array(PdfDict *dict, const char *key);

/* ─── Array Helpers ─── */

PdfObj *pdf_array_get(PdfArray *arr, int index);
int     pdf_array_len(PdfArray *arr);

/* ─── Page Access ─── */

/*
 * pdf_page_count - Return the number of pages in the document.
 */
int pdf_page_count(PdfDocument *doc);

/*
 * pdf_get_page - Return the page dictionary for a 0-based page index.
 *
 * Returns NULL if the index is out of range.
 */
PdfObj *pdf_get_page(PdfDocument *doc, int page_idx);

/* ─── Stream Decoding ─── */

/*
 * pdf_decode_stream - Decode stream data in-place.
 *
 * Applies /Filter (FlateDecode) and /DecodeParms (predictors).
 * On success, stream->decoded_data and stream->decoded_length are set.
 * Returns true on success.
 */
bool pdf_decode_stream(PdfDocument *doc, PdfStream *stream);

/* ─── Object Memory Management ─── */

/*
 * pdf_free_obj - Recursively free a PdfObj and all its children.
 *
 * Safe to call with NULL.
 */
void pdf_free_obj(PdfObj *obj);

#endif /* PDF_PARSER_H */
