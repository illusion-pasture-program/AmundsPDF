/*
 * AmundsPDF - pdf_fonts.h
 * Font system: standard 14 font metrics, PDF-to-Windows font mapping.
 * Zero external dependencies beyond standard C + Win32.
 */
#ifndef PDF_FONTS_H
#define PDF_FONTS_H

#include "pdf_types.h"

/*
 * pdf_font_create - Map a PDF /BaseFont name to a Windows HFONT.
 *
 * Handles the standard 14 PDF fonts plus heuristic matching for non-standard
 * fonts (looks for bold/italic/serif/mono keywords).
 *
 * size_pt: font size in points (PDF text space units).
 * hdc:     device context used for font metrics resolution.
 *
 * Returns NULL on failure. Caller must DeleteObject() when done.
 */
HFONT pdf_font_create(const char *base_font_name, double size_pt, HDC hdc);

/*
 * pdf_font_create_px - Create a font with an explicit pixel height.
 *
 * Use this when the caller has already computed the device-space font size
 * (e.g., by scaling through the CTM). Bypasses DPI conversion.
 *
 * height_px: font height in device pixels (positive = cell height, negative = character height).
 * Returns NULL on failure. Caller must DeleteObject() when done.
 */
HFONT pdf_font_create_px(const char *base_font_name, int height_px);

/*
 * pdf_fonts_cleanup - Free any internally cached font resources.
 *
 * Call once at program shutdown.
 */
void pdf_fonts_cleanup(void);

/*
 * pdf_font_char_width - Get the width of a character in a standard font.
 *
 * Returns the width in 1/1000 of a text space unit (the standard PDF metric).
 * For non-standard fonts, returns approximate widths based on the closest
 * matching standard font.
 *
 * char_code: character code 0..255 in WinAnsiEncoding.
 */
int pdf_font_char_width(const char *base_font_name, int char_code);

/*
 * pdf_font_default_width - Get the default (missing glyph) width for a font.
 *
 * Returns the default character width in 1/1000 of a text space unit.
 */
int pdf_font_default_width(const char *base_font_name);

#endif /* PDF_FONTS_H */
