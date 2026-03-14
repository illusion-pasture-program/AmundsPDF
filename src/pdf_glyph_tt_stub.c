/*
 * AmundsPDF - pdf_glyph_tt_stub.c
 * Stub implementation of parsed_font_from_truetype().
 *
 * Replace this file with a real TrueType glyph parser when available.
 * For now, returns NULL so the renderer falls back to TextOutW for
 * TrueType-embedded fonts.
 */

#include "pdf_glyph.h"

ParsedFont *parsed_font_from_truetype(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return NULL;
}
