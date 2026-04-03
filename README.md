# Amund's PDF Viewer

A zero-dependency PDF viewer for Windows, built from scratch in C.

No libraries. No frameworks. No zlib, no freetype, no libpng. Just the PDF spec, the Win32 API, and ~21,000 lines of C.

The final binary is **110 KB**.

## What it does

Opens and renders PDF documents with full anti-aliased output. Handles the real-world PDFs you actually encounter: tax forms, invoices, scanned documents, technical papers.

**Rendering pipeline:**
- Software scanline rasterizer with 16x anti-aliasing
- CFF, Type1, and TrueType font parsing and glyph rendering
- Type3 font support via recursive content stream interpretation
- CIDFont/ToUnicode mapping for composite (CJK) fonts
- JPEG and multi-format image decoding via WIC
- Soft mask (SMask) transparency
- CMYK color space handling
- Clipping paths (W/W*) via software coverage masks
- Dash patterns, line joins, line caps

**PDF structure:**
- Cross-reference tables and xref streams
- FlateDecode via a from-scratch RFC 1951 inflate implementation
- RC4/MD5 decryption for encrypted PDFs
- Full page tree navigation

**Viewer:**
- Smooth zooming with fit-to-width
- Viewport-aware region rendering (only renders what's visible)
- Page navigation with keyboard shortcuts
- Drag-to-scroll
- Dark theme UI
- DPI-aware

## Building

Requires MinGW GCC on Windows.

```
python build.py
```

## Architecture

```
src/
  main.c                 Win32 UI, window management, input handling
  pdf_parser.h/.c        PDF tokenizer, object model, xref, page tree
  pdf_render.h/.c        Content stream interpreter, path/image rendering
  pdf_raster.h/.c        Software scanline rasterizer (16x AA)
  pdf_fonts.h/.c         Standard 14 font metrics, system font mapping
  pdf_glyph.h            Glyph outline types and ParsedFont interface
  pdf_glyph_cff.c        CFF (Type1C) font parser
  pdf_glyph_type1.c      Type1 (PFB) font parser
  pdf_glyph_tt_stub.c    TrueType glyph parser (head/loca/glyf)
  pdf_glyph_render.c     Font cache, encoding, CID, glyph rasterization
  pdf_inflate.h/.c       RFC 1951 inflate (zero-dep deflate)
  pdf_crypt.h/.c         RC4/MD5 decryption
  pdf_types.h            Shared types and macros
  pdf_profile.h          QPC-based render profiling
```

Every component is written from first principles. The inflate implementation follows RFC 1951 directly. Font parsers decode CFF charstrings, Type1 PostScript, and TrueType glyf tables by hand. The rasterizer does scanline fill with sub-pixel coverage, not GDI.

## Why

Most PDF viewers pull in hundreds of megabytes of dependencies. This one proves you don't need any of them. The entire thing compiles in seconds and produces a binary smaller than most favicons.
