/*
 * AmundsPDF - pdf_render.h
 * Content stream interpreter and GDI-based page renderer.
 * Zero external dependencies beyond standard C + Win32.
 */
#ifndef PDF_RENDER_H
#define PDF_RENDER_H

#include "pdf_types.h"

/*
 * pdf_render_page - Render a PDF page to a device-independent bitmap.
 *
 * Interprets the page's content stream(s), executing all PDF operators
 * (graphics state, path construction/painting, text, color, XObjects)
 * and rasterizes the result using Windows GDI.
 *
 * page_idx:    0-based page index.
 * scale:       zoom factor (1.0 = 72 DPI, 2.0 = 144 DPI, etc.).
 * out_width:   receives the rendered bitmap width in pixels (may be NULL).
 * out_height:  receives the rendered bitmap height in pixels (may be NULL).
 *
 * Returns an HBITMAP on success (caller must DeleteObject()), or NULL on failure.
 */
HBITMAP pdf_render_page(PdfDocument *doc, int page_idx, double scale,
                        int *out_width, int *out_height);

#endif /* PDF_RENDER_H */
