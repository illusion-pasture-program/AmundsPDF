/*
 * AmundsPDF - pdf_profile.h
 * Lightweight render-pipeline profiling instrumentation.
 *
 * Uses QueryPerformanceCounter for high-resolution timing.
 * Global accumulators are reset at the start of each pdf_render_page() call,
 * and results are emitted via OutputDebugStringA at the end.
 *
 * The profiling adds only QPC calls (sub-microsecond overhead per call),
 * so it is safe to leave compiled in for release builds.
 */
#ifndef PDF_PROFILE_H
#define PDF_PROFILE_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Profiling Accumulator
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LARGE_INTEGER freq;         /* QPC frequency (ticks per second) */
    LARGE_INTEGER page_start;   /* timestamp at start of pdf_render_page() */

    /* Accumulated ticks and call counts for each profiled subsystem */
    int64_t interpret_time;     /* interpret_stream (top-level only) */

    int64_t fill_time;          int fill_count;
    int64_t stroke_time;        int stroke_count;
    int64_t glyph_time;         int glyph_count;
    int64_t image_time;         int image_count;
    int64_t raster_finish_time; int raster_finish_count;
    int64_t blend_time;         int blend_count;

    /* Tracking page number and scale for the output line */
    int     page_idx;
    double  scale;
} RenderProfile;

/* Single global instance, defined in pdf_render.c */
extern RenderProfile g_prof;

/* ═══════════════════════════════════════════════════════════════════════════
 * Macros
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PROF_START(tag)  -- declare and capture a start timestamp named tag_start
 * PROF_END(field, tag) -- capture end timestamp, accumulate into g_prof.field_time/count
 *
 * Usage:
 *   void some_func(...) {
 *       PROF_START(my);
 *       ... work ...
 *       PROF_END(fill, my);   // adds elapsed to g_prof.fill_time, increments fill_count
 *   }
 */

#define PROF_START(tag) \
    LARGE_INTEGER tag##_prof_start; \
    QueryPerformanceCounter(&tag##_prof_start)

#define PROF_END(field, tag) \
    do { \
        LARGE_INTEGER tag##_prof_end; \
        QueryPerformanceCounter(&tag##_prof_end); \
        g_prof.field##_time += tag##_prof_end.QuadPart - tag##_prof_start.QuadPart; \
        g_prof.field##_count++; \
    } while (0)

/* Reset all accumulators and record start-of-page timestamp */
static inline void prof_reset(int page_idx, double scale)
{
    QueryPerformanceFrequency(&g_prof.freq);
    QueryPerformanceCounter(&g_prof.page_start);

    g_prof.interpret_time     = 0;

    g_prof.fill_time          = 0;  g_prof.fill_count          = 0;
    g_prof.stroke_time        = 0;  g_prof.stroke_count        = 0;
    g_prof.glyph_time         = 0;  g_prof.glyph_count         = 0;
    g_prof.image_time         = 0;  g_prof.image_count         = 0;
    g_prof.raster_finish_time = 0;  g_prof.raster_finish_count = 0;
    g_prof.blend_time         = 0;  g_prof.blend_count         = 0;

    g_prof.page_idx = page_idx;
    g_prof.scale    = scale;
}

/* Convert accumulated ticks to milliseconds */
static inline double prof_ticks_to_ms(int64_t ticks)
{
    if (g_prof.freq.QuadPart == 0) return 0.0;
    return (double)ticks * 1000.0 / (double)g_prof.freq.QuadPart;
}

/* Emit profiling results via OutputDebugStringA */
static inline void prof_emit(void)
{
    LARGE_INTEGER page_end;
    QueryPerformanceCounter(&page_end);

    double total_ms     = prof_ticks_to_ms(page_end.QuadPart - g_prof.page_start.QuadPart);
    double interp_ms    = prof_ticks_to_ms(g_prof.interpret_time);
    double fill_ms      = prof_ticks_to_ms(g_prof.fill_time);
    double stroke_ms    = prof_ticks_to_ms(g_prof.stroke_time);
    double glyph_ms     = prof_ticks_to_ms(g_prof.glyph_time);
    double image_ms     = prof_ticks_to_ms(g_prof.image_time);
    double rfinish_ms   = prof_ticks_to_ms(g_prof.raster_finish_time);
    double blend_ms     = prof_ticks_to_ms(g_prof.blend_time);

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "[AmundsPDF PERF] Render page %d at %.1fx: %.1fms total\n"
        "  interpret_stream:  %8.1fms\n"
        "  path_aa_fill:      %8.1fms  (%d calls)\n"
        "  path_aa_stroke:    %8.1fms  (%d calls)\n"
        "  glyph_render:      %8.1fms  (%d calls)\n"
        "  image_render:      %8.1fms  (%d calls)\n"
        "  raster_finish:     %8.1fms  (%d calls)\n"
        "  raster_blend:      %8.1fms  (%d calls)\n",
        g_prof.page_idx + 1,  /* 1-based for display */
        g_prof.scale,
        total_ms,
        interp_ms,
        fill_ms,   g_prof.fill_count,
        stroke_ms, g_prof.stroke_count,
        glyph_ms,  g_prof.glyph_count,
        image_ms,  g_prof.image_count,
        rfinish_ms,g_prof.raster_finish_count,
        blend_ms,  g_prof.blend_count);

    OutputDebugStringA(buf);

    /* Also print to stderr so --export mode can see it in the console */
    fprintf(stderr, "%s", buf);
}

#endif /* PDF_PROFILE_H */
