/*
 * AmundsPDF - pdf_glyph_render.h
 * Glyph outline rendering pipeline.
 *
 * Extracts embedded font data from PDF font streams, parses glyph outlines
 * via the CFF/TrueType parsers, and renders them as anti-aliased filled
 * GDI paths. Provides a drop-in replacement for TextOutW-based rendering.
 */
#ifndef PDF_GLYPH_RENDER_H
#define PDF_GLYPH_RENDER_H

#include "pdf_types.h"

/*
 * glyph_render_text_string - Render text using embedded font outlines.
 *
 * Looks up the current font in the graphics state, extracts/parses the
 * embedded font data (CFF or TrueType), and renders each glyph as a
 * filled GDI path with anti-aliasing.
 *
 * If no embedded font is available or glyph extraction fails, returns false
 * so the caller can fall back to the TextOutW-based rendering path.
 *
 * ctx:       render context (provides HDC, text matrix, graphics state, doc)
 * resources: the page's /Resources dictionary
 * str:       raw byte string from the PDF content stream
 * len:       length of the string
 *
 * Returns true if text was rendered using glyph outlines.
 * Returns false if fallback to TextOutW is needed.
 */
bool glyph_render_text_string(PdfRenderCtx *ctx, PdfDict *resources,
                               const uint8_t *str, size_t len);

/*
 * glyph_cache_cleanup - Free all cached parsed fonts.
 *
 * Call once at program shutdown to release memory.
 */
void glyph_cache_cleanup(void);

#endif /* PDF_GLYPH_RENDER_H */
