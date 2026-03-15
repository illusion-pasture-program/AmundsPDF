/*
 * AmundsPDF - main.c
 * Zero-dependency PDF viewer: pure C + Win32 API.
 * Entry point, window management, user interaction, wiring to parser/renderer.
 */
#include "pdf_types.h"
#include "pdf_parser.h"
#include "pdf_render.h"
#include "pdf_fonts.h"
#include "pdf_glyph_render.h"

#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <strsafe.h>

/* ─── Constants ─── */
#define APP_CLASS_NAME      L"AmundsPDF"
#define APP_TITLE           L"Amund's PDF Viewer"

#define BG_COLOR            RGB(30, 30, 30)
#define PAGE_BORDER_COLOR   RGB(60, 60, 60)
#define PAGE_SHADOW_COLOR   RGB(15, 15, 15)
#define TEXT_HINT_COLOR     RGB(140, 140, 140)

#define PAGE_MARGIN         16      /* pixels around page */
#define SHADOW_OFFSET       4       /* drop shadow offset */
#define SCROLL_LINE         48      /* arrow key scroll increment */
#define SCROLL_WHEEL_LINES  3       /* lines per wheel notch */

#define ZOOM_MIN            0.1
#define ZOOM_MAX            10.0
#define ZOOM_IN_FACTOR      1.25
#define ZOOM_OUT_FACTOR     0.8

#define MIN_RENDER_SCALE    1.5     /* oversample threshold for sharp text */

#define TIMER_RESIZE        1
#define TIMER_RESIZE_MS     100

/* ─── Menu IDs ─── */
#define IDM_FILE_OPEN       1001
#define IDM_FILE_EXIT       1002
#define IDM_VIEW_ZOOMIN     2001
#define IDM_VIEW_ZOOMOUT    2002
#define IDM_VIEW_FITWIDTH   2003
#define IDM_VIEW_ACTUALSIZE 2004
#define IDM_VIEW_FULLSCREEN 2005
#define IDM_GO_FIRST        3001
#define IDM_GO_PREV         3002
#define IDM_GO_NEXT         3003
#define IDM_GO_LAST         3004
#define IDM_GO_GOTO         3005

/* ─── Application State ─── */
typedef struct {
    HINSTANCE   hInst;
    HWND        hWnd;

    /* Document */
    PdfDocument doc;
    bool        doc_loaded;
    wchar_t     file_path[MAX_PATH];

    /* Current view */
    int         current_page;       /* 0-based */
    double      zoom;               /* explicit zoom level */
    bool        fit_width;          /* auto-fit to window width */

    /* Rendered page cache */
    HBITMAP     hPageBitmap;
    int         page_bmp_w;         /* raw bitmap width (render_scale) */
    int         page_bmp_h;         /* raw bitmap height (render_scale) */
    int         display_w;          /* display width (at user zoom) */
    int         display_h;          /* display height (at user zoom) */
    double      render_scale;       /* actual scale bitmap was rendered at */
    int         rendered_page;      /* which page is cached */
    double      rendered_zoom;      /* at what zoom level */

    /* Viewport-aware region rendering */
    bool        region_mode;        /* true if bitmap is a sub-region, not full page */
    double      region_x;           /* page-space origin X of rendered bitmap */
    double      region_y;           /* page-space origin Y of rendered bitmap */
    double      region_w;           /* page-space width of rendered bitmap */
    double      region_h;           /* page-space height of rendered bitmap */

    /* Scrolling */
    int         scroll_x;
    int         scroll_y;
    int         max_scroll_x;
    int         max_scroll_y;

    /* UI */
    HBRUSH      hBgBrush;
    HBRUSH      hShadowBrush;
    bool        fullscreen;
    WINDOWPLACEMENT wpPrev;
    DWORD       stylePrev;
    DWORD       exStylePrev;

    /* Dragging */
    bool        dragging;
    POINT       drag_start;
    int         drag_scroll_x;
    int         drag_scroll_y;

    /* Resize debounce */
    bool        resize_pending;
} AppState;

static AppState g_app = {0};

/* ─── Forward Declarations ─── */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static HMENU    CreateAppMenu(void);
static void     OpenFileDialog(void);
static void     OpenDocument(const wchar_t *path);
static void     CloseDocument(void);
static void     RenderCurrentPage(void);
static void     NavigatePage(int page);
static void     NavigatePageDelta(int delta);
static void     GoToPageDialog(void);
static void     UpdateScrollLimits(void);
static void     ClampScroll(void);
static void     ApplyZoom(double new_zoom, bool is_fit_width);
static void     ZoomIn(void);
static void     ZoomOut(void);
static void     ZoomFitWidth(void);
static void     ZoomActualSize(void);
static void     ToggleFullscreen(void);
static void     UpdateTitle(void);
static double   GetPageWidthPts(void);
static double   GetPageHeightPts(void);

/* ═══════════════════════════════════════════════════════════════════════
 *  wWinMain
 * ═══════════════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════════════
 *  Batch export mode: --export output.bmp [--page N] [--scale S] input.pdf
 *  Renders a page directly to a BMP file without opening a window.
 * ═══════════════════════════════════════════════════════════════════════ */
static int BatchExport(LPWSTR lpCmd)
{
    /* Parse arguments: --export <out.bmp> [--page N] [--scale S] <input.pdf> */
    wchar_t export_path[MAX_PATH] = {0};
    wchar_t input_path[MAX_PATH] = {0};
    int page_idx = 0;
    double scale = 2.0;

    /* Simple argument parsing */
    wchar_t *p = lpCmd;
    bool got_export = false;
    bool expect_export_path = false;
    bool expect_page = false;
    bool expect_scale = false;

    while (p && *p) {
        /* Skip whitespace */
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;

        /* Extract token */
        wchar_t token[MAX_PATH] = {0};
        int ti = 0;
        if (*p == L'"') {
            p++;
            while (*p && *p != L'"' && ti < MAX_PATH - 1)
                token[ti++] = *p++;
            if (*p == L'"') p++;
        } else {
            while (*p && *p != L' ' && *p != L'\t' && ti < MAX_PATH - 1)
                token[ti++] = *p++;
        }
        token[ti] = L'\0';

        if (expect_export_path) {
            StringCchCopyW(export_path, MAX_PATH, token);
            expect_export_path = false;
            continue;
        }
        if (expect_page) {
            page_idx = _wtoi(token) - 1;  /* 1-based to 0-based */
            if (page_idx < 0) page_idx = 0;
            expect_page = false;
            continue;
        }
        if (expect_scale) {
            scale = _wtof(token);
            if (scale <= 0.0) scale = 2.0;
            expect_scale = false;
            continue;
        }

        if (wcsicmp(token, L"--export") == 0) {
            got_export = true;
            expect_export_path = true;
        } else if (wcsicmp(token, L"--page") == 0) {
            expect_page = true;
        } else if (wcsicmp(token, L"--scale") == 0) {
            expect_scale = true;
        } else {
            /* Assume it's the input PDF path */
            StringCchCopyW(input_path, MAX_PATH, token);
        }
    }

    if (!got_export || !export_path[0] || !input_path[0])
        return -1;  /* not export mode or missing args */

    /* Open the PDF */
    PdfDocument doc;
    memset(&doc, 0, sizeof(doc));
    if (!pdf_open(&doc, input_path)) {
        return 1;
    }

    if (page_idx >= pdf_page_count(&doc))
        page_idx = pdf_page_count(&doc) - 1;

    /* Render the page */
    int bmp_w = 0, bmp_h = 0;
    HBITMAP hbm = pdf_render_page(&doc, page_idx, scale, &bmp_w, &bmp_h);
    if (!hbm) {
        pdf_close(&doc);
        return 1;
    }

    /* Save as BMP file */
    HDC hdc = CreateCompatibleDC(NULL);
    HBITMAP old = (HBITMAP)SelectObject(hdc, hbm);

    BITMAPINFOHEADER bih;
    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = bmp_w;
    bih.biHeight = bmp_h;  /* bottom-up */
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    int row_stride = ((bmp_w * 3 + 3) & ~3);
    DWORD img_size = (DWORD)row_stride * bmp_h;

    uint8_t *bits = (uint8_t *)malloc(img_size);
    if (bits) {
        GetDIBits(hdc, hbm, 0, bmp_h, bits, (BITMAPINFO *)&bih, DIB_RGB_COLORS);

        HANDLE hFile = CreateFileW(export_path, GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER bfh;
            memset(&bfh, 0, sizeof(bfh));
            bfh.bfType = 0x4D42;  /* 'BM' */
            bfh.bfSize = sizeof(bfh) + sizeof(bih) + img_size;
            bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

            DWORD written;
            WriteFile(hFile, &bfh, sizeof(bfh), &written, NULL);
            WriteFile(hFile, &bih, sizeof(bih), &written, NULL);
            WriteFile(hFile, bits, img_size, &written, NULL);
            CloseHandle(hFile);
        }
        free(bits);
    }

    SelectObject(hdc, old);
    DeleteDC(hdc);
    DeleteObject(hbm);
    pdf_close(&doc);
    glyph_cache_cleanup();
    pdf_fonts_cleanup();
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    (void)hPrev;
    (void)nShow;

    /* Check for batch export mode */
    if (lpCmd && wcsstr(lpCmd, L"--export")) {
        int result = BatchExport(lpCmd);
        if (result >= 0)
            return result;
        /* result == -1 means not valid export args, fall through to GUI */
    }

    g_app.hInst       = hInstance;
    g_app.hBgBrush    = CreateSolidBrush(BG_COLOR);
    g_app.hShadowBrush = CreateSolidBrush(PAGE_SHADOW_COLOR);
    g_app.zoom        = 1.0;
    g_app.fit_width   = true;
    g_app.rendered_page = -1;
    g_app.rendered_zoom = -1.0;

    /* Register window class */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hIcon          = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm        = wc.hIcon;
    wc.hCursor        = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground  = g_app.hBgBrush;
    wc.lpszClassName  = APP_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class.", L"Error", MB_ICONERROR);
        return 1;
    }

    /* Create menu */
    HMENU hMenu = CreateAppMenu();

    /* Create window */
    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        APP_CLASS_NAME, APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        NULL, hMenu, hInstance, NULL);

    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create window.", L"Error", MB_ICONERROR);
        return 1;
    }

    g_app.hWnd = hwnd;

    /* Parse command line: expect a file path */
    if (lpCmd && lpCmd[0]) {
        wchar_t path[MAX_PATH];
        /* Strip quotes if present */
        if (lpCmd[0] == L'"') {
            StringCchCopyW(path, MAX_PATH, lpCmd + 1);
            size_t len = 0;
            StringCchLengthW(path, MAX_PATH, &len);
            if (len > 0 && path[len - 1] == L'"')
                path[len - 1] = L'\0';
        } else {
            StringCchCopyW(path, MAX_PATH, lpCmd);
        }

        /* Trim trailing whitespace */
        size_t len = 0;
        StringCchLengthW(path, MAX_PATH, &len);
        while (len > 0 && (path[len - 1] == L' ' || path[len - 1] == L'\t'))
            path[--len] = L'\0';

        if (len > 0 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            OpenDocument(path);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Cleanup */
    CloseDocument();
    glyph_cache_cleanup();
    pdf_fonts_cleanup();
    if (g_app.hPageBitmap) DeleteObject(g_app.hPageBitmap);
    DeleteObject(g_app.hBgBrush);
    DeleteObject(g_app.hShadowBrush);

    return (int)msg.wParam;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Menu creation
 * ═══════════════════════════════════════════════════════════════════════ */
static HMENU CreateAppMenu(void)
{
    HMENU hMenuBar = CreateMenu();

    /* File menu */
    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN, L"&Open\tCtrl+O");
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit\tAlt+F4");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile, L"&File");

    /* View menu */
    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOMIN,     L"Zoom &In\tCtrl+=");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOMOUT,    L"Zoom &Out\tCtrl+-");
    AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_FITWIDTH,   L"&Fit Width\tCtrl+0");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ACTUALSIZE, L"&Actual Size\tCtrl+1");
    AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_FULLSCREEN, L"F&ullscreen\tF11");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hView, L"&View");

    /* Go menu */
    HMENU hGo = CreatePopupMenu();
    AppendMenuW(hGo, MF_STRING, IDM_GO_FIRST, L"&First Page\tCtrl+Home");
    AppendMenuW(hGo, MF_STRING, IDM_GO_PREV,  L"&Previous Page\tCtrl+Left");
    AppendMenuW(hGo, MF_STRING, IDM_GO_NEXT,  L"&Next Page\tCtrl+Right");
    AppendMenuW(hGo, MF_STRING, IDM_GO_LAST,  L"&Last Page\tCtrl+End");
    AppendMenuW(hGo, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hGo, MF_STRING, IDM_GO_GOTO,  L"&Go To Page...\tCtrl+G");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hGo, L"&Go");

    return hMenuBar;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Document lifecycle
 * ═══════════════════════════════════════════════════════════════════════ */
static void OpenFileDialog(void)
{
    wchar_t path[MAX_PATH] = {0};

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = g_app.hWnd;
    ofn.lpstrFilter  = L"PDF Files (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt  = L"pdf";

    if (GetOpenFileNameW(&ofn))
        OpenDocument(path);
}

static void OpenDocument(const wchar_t *path)
{
    /* Close any existing document */
    CloseDocument();

    /* Try to open the new PDF */
    memset(&g_app.doc, 0, sizeof(g_app.doc));
    if (!pdf_open(&g_app.doc, path)) {
        wchar_t msg[MAX_PATH + 64];
        StringCchPrintfW(msg, MAX_PATH + 64, L"Failed to open:\n%s", path);
        MessageBoxW(g_app.hWnd, msg, L"Error", MB_ICONERROR);
        return;
    }

    g_app.doc_loaded = true;
    StringCchCopyW(g_app.file_path, MAX_PATH, path);
    g_app.current_page = 0;
    g_app.scroll_x = 0;
    g_app.scroll_y = 0;

    /* Start in fit-width mode */
    g_app.fit_width = true;

    RenderCurrentPage();
    UpdateTitle();
    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void CloseDocument(void)
{
    if (g_app.doc_loaded) {
        pdf_close(&g_app.doc);
        g_app.doc_loaded = false;
        g_app.file_path[0] = L'\0';
    }

    if (g_app.hPageBitmap) {
        DeleteObject(g_app.hPageBitmap);
        g_app.hPageBitmap = NULL;
    }

    g_app.page_bmp_w = 0;
    g_app.page_bmp_h = 0;
    g_app.display_w = 0;
    g_app.display_h = 0;
    g_app.render_scale = 0.0;
    g_app.rendered_page = -1;
    g_app.rendered_zoom = -1.0;
    g_app.region_mode = false;
    g_app.region_x = 0.0;
    g_app.region_y = 0.0;
    g_app.region_w = 0.0;
    g_app.region_h = 0.0;
    g_app.current_page = 0;
    g_app.scroll_x = 0;
    g_app.scroll_y = 0;
    g_app.max_scroll_x = 0;
    g_app.max_scroll_y = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Page geometry helpers
 * ═══════════════════════════════════════════════════════════════════════ */
static double GetPageWidthPts(void)
{
    if (!g_app.doc_loaded) return 612.0; /* default letter width */

    PdfObj *page = pdf_get_page(&g_app.doc, g_app.current_page);
    if (!page) return 612.0;

    PdfObj *resolved = pdf_resolve(&g_app.doc, page);
    if (!resolved || resolved->type != PDF_OBJ_DICT) return 612.0;

    PdfArray *mbox = pdf_dict_get_array(resolved->dict, "MediaBox");
    if (!mbox || pdf_array_len(mbox) < 4) return 612.0;

    PdfObj *x0 = pdf_resolve(&g_app.doc, pdf_array_get(mbox, 0));
    PdfObj *x1 = pdf_resolve(&g_app.doc, pdf_array_get(mbox, 2));
    if (!x0 || !x1) return 612.0;

    double left = (x0->type == PDF_OBJ_INT) ? (double)x0->integer :
                  (x0->type == PDF_OBJ_REAL) ? x0->real : 0.0;
    double right = (x1->type == PDF_OBJ_INT) ? (double)x1->integer :
                   (x1->type == PDF_OBJ_REAL) ? x1->real : 612.0;

    return fabs(right - left);
}

static double GetPageHeightPts(void)
{
    if (!g_app.doc_loaded) return 792.0; /* default letter height */

    PdfObj *page = pdf_get_page(&g_app.doc, g_app.current_page);
    if (!page) return 792.0;

    PdfObj *resolved = pdf_resolve(&g_app.doc, page);
    if (!resolved || resolved->type != PDF_OBJ_DICT) return 792.0;

    PdfArray *mbox = pdf_dict_get_array(resolved->dict, "MediaBox");
    if (!mbox || pdf_array_len(mbox) < 4) return 792.0;

    PdfObj *y0 = pdf_resolve(&g_app.doc, pdf_array_get(mbox, 1));
    PdfObj *y1 = pdf_resolve(&g_app.doc, pdf_array_get(mbox, 3));
    if (!y0 || !y1) return 792.0;

    double bottom = (y0->type == PDF_OBJ_INT) ? (double)y0->integer :
                    (y0->type == PDF_OBJ_REAL) ? y0->real : 0.0;
    double top    = (y1->type == PDF_OBJ_INT) ? (double)y1->integer :
                    (y1->type == PDF_OBJ_REAL) ? y1->real : 792.0;

    return fabs(top - bottom);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Zoom and rendering
 * ═══════════════════════════════════════════════════════════════════════ */
static double CalculateFitWidthZoom(void)
{
    RECT rc;
    GetClientRect(g_app.hWnd, &rc);
    int client_w = rc.right - rc.left;
    double page_w = GetPageWidthPts();

    if (page_w <= 0.0) page_w = 612.0;

    double z = (double)(client_w - 2 * PAGE_MARGIN) / page_w;
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    return z;
}

/*
 * Check if the viewport has scrolled close enough to the edge of the
 * rendered region that we should re-render.  Returns true if the
 * visible viewport extends within 25% of the rendered region's edge.
 */
static bool NeedsRegionRerender(void)
{
    if (!g_app.region_mode || !g_app.hPageBitmap) return false;

    RECT rc;
    GetClientRect(g_app.hWnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    double render_scale = g_app.render_scale;
    if (render_scale <= 0.0) return false;

    /* Viewport rectangle in page coordinates */
    double vp_x = (double)(g_app.scroll_x - PAGE_MARGIN) / render_scale;
    double vp_y = (double)(g_app.scroll_y - PAGE_MARGIN) / render_scale;
    double vp_w = (double)cw / render_scale;
    double vp_h = (double)ch / render_scale;

    /* How much padding exists between the viewport edge and the rendered region edge */
    double pad_left   = vp_x - g_app.region_x;
    double pad_right  = (g_app.region_x + g_app.region_w) - (vp_x + vp_w);
    double pad_top    = vp_y - g_app.region_y;
    double pad_bottom = (g_app.region_y + g_app.region_h) - (vp_y + vp_h);

    /* Trigger re-render if padding on any side is less than 25% of viewport */
    double thresh_x = vp_w * 0.25;
    double thresh_y = vp_h * 0.25;

    if (pad_left < thresh_x || pad_right < thresh_x ||
        pad_top < thresh_y || pad_bottom < thresh_y)
        return true;

    return false;
}

static void RenderCurrentPage(void)
{
    if (!g_app.doc_loaded) return;

    /* Calculate effective zoom */
    double effective_zoom = g_app.zoom;
    if (g_app.fit_width)
        effective_zoom = CalculateFitWidthZoom();

    g_app.zoom = effective_zoom;

    /* Check if we already have this page rendered at this zoom */
    if (g_app.hPageBitmap &&
        g_app.rendered_page == g_app.current_page &&
        fabs(g_app.rendered_zoom - effective_zoom) < 0.001 &&
        !NeedsRegionRerender())
        return;

    /* Free old bitmap */
    if (g_app.hPageBitmap) {
        DeleteObject(g_app.hPageBitmap);
        g_app.hPageBitmap = NULL;
    }

    /* Render at a minimum quality level for sharp text when zoomed out */
    double render_scale = effective_zoom;
    if (render_scale < MIN_RENDER_SCALE)
        render_scale = MIN_RENDER_SCALE;

    /* Get page dimensions */
    double page_w_pts = GetPageWidthPts();
    double page_h_pts = GetPageHeightPts();

    /* Decide whether to use region rendering or full-page rendering.
     * Use region rendering when the full page at this scale would be
     * significantly larger than 3x the viewport (i.e., the page is large
     * relative to the window). */
    RECT rc;
    GetClientRect(g_app.hWnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    bool use_region = (page_w_pts * render_scale > 3.0 * cw) ||
                      (page_h_pts * render_scale > 3.0 * ch);

    int w = 0, h = 0;

    if (use_region) {
        /* Compute viewport in page coordinates */
        double vp_x = (double)(g_app.scroll_x - PAGE_MARGIN) / render_scale;
        double vp_y = (double)(g_app.scroll_y - PAGE_MARGIN) / render_scale;
        double vp_w = (double)cw / render_scale;
        double vp_h = (double)ch / render_scale;

        /* Add 100% padding on each side (total rendered = 3x viewport) */
        double reg_x = vp_x - vp_w;
        double reg_y = vp_y - vp_h;
        double reg_w = vp_w * 3.0;
        double reg_h = vp_h * 3.0;

        /* Clamp to page bounds */
        if (reg_x < 0.0) reg_x = 0.0;
        if (reg_y < 0.0) reg_y = 0.0;
        if (reg_x + reg_w > page_w_pts) reg_w = page_w_pts - reg_x;
        if (reg_y + reg_h > page_h_pts) reg_h = page_h_pts - reg_y;
        if (reg_w < 1.0) reg_w = 1.0;
        if (reg_h < 1.0) reg_h = 1.0;

        g_app.hPageBitmap = pdf_render_page_region(&g_app.doc, g_app.current_page,
                                                    render_scale,
                                                    reg_x, reg_y, reg_w, reg_h,
                                                    &w, &h);

        g_app.region_mode = true;
        g_app.region_x = reg_x;
        g_app.region_y = reg_y;
        g_app.region_w = reg_w;
        g_app.region_h = reg_h;
    } else {
        g_app.hPageBitmap = pdf_render_page(&g_app.doc, g_app.current_page,
                                             render_scale, &w, &h);

        g_app.region_mode = false;
        g_app.region_x = 0.0;
        g_app.region_y = 0.0;
        g_app.region_w = page_w_pts;
        g_app.region_h = page_h_pts;
    }

    if (g_app.hPageBitmap) {
        g_app.page_bmp_w = w;
        g_app.page_bmp_h = h;
        g_app.render_scale = render_scale;

        /* Display dimensions are always the FULL page at user zoom.
         * The bitmap only covers a region, but the display_w/h represents
         * the conceptual full-page size for scroll limits and centering. */
        double display_ratio = effective_zoom / render_scale;
        if (!g_app.region_mode) {
            g_app.display_w = (int)(w * display_ratio + 0.5);
            g_app.display_h = (int)(h * display_ratio + 0.5);
        } else {
            g_app.display_w = (int)(page_w_pts * effective_zoom + 0.5);
            g_app.display_h = (int)(page_h_pts * effective_zoom + 0.5);
        }

        g_app.rendered_page = g_app.current_page;
        g_app.rendered_zoom = effective_zoom;
    } else {
        g_app.page_bmp_w = 0;
        g_app.page_bmp_h = 0;
        g_app.display_w = 0;
        g_app.display_h = 0;
        g_app.render_scale = 0.0;
        g_app.rendered_page = -1;
        g_app.rendered_zoom = -1.0;
        g_app.region_mode = false;
    }

    UpdateScrollLimits();
}

static void UpdateScrollLimits(void)
{
    RECT rc;
    GetClientRect(g_app.hWnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    int content_w = g_app.display_w + 2 * PAGE_MARGIN;
    int content_h = g_app.display_h + 2 * PAGE_MARGIN;

    g_app.max_scroll_x = content_w > cw ? content_w - cw : 0;
    g_app.max_scroll_y = content_h > ch ? content_h - ch : 0;

    ClampScroll();
}

static void ClampScroll(void)
{
    if (g_app.scroll_x < 0) g_app.scroll_x = 0;
    if (g_app.scroll_y < 0) g_app.scroll_y = 0;
    if (g_app.scroll_x > g_app.max_scroll_x) g_app.scroll_x = g_app.max_scroll_x;
    if (g_app.scroll_y > g_app.max_scroll_y) g_app.scroll_y = g_app.max_scroll_y;
}

static void ApplyZoom(double new_zoom, bool is_fit_width)
{
    if (new_zoom < ZOOM_MIN) new_zoom = ZOOM_MIN;
    if (new_zoom > ZOOM_MAX) new_zoom = ZOOM_MAX;

    /* Remember view center to stabilize zoom */
    RECT rc;
    GetClientRect(g_app.hWnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    double center_rx = 0.5;
    double center_ry = 0.5;
    if (g_app.display_w > 0 && g_app.display_h > 0) {
        int content_w = g_app.display_w + 2 * PAGE_MARGIN;
        int content_h = g_app.display_h + 2 * PAGE_MARGIN;
        center_rx = (content_w > 0) ? (double)(g_app.scroll_x + cw / 2) / content_w : 0.5;
        center_ry = (content_h > 0) ? (double)(g_app.scroll_y + ch / 2) / content_h : 0.5;
    }

    g_app.zoom = new_zoom;
    g_app.fit_width = is_fit_width;

    /* Force re-render */
    g_app.rendered_page = -1;
    g_app.rendered_zoom = -1.0;
    RenderCurrentPage();

    /* Restore view center */
    int new_content_w = g_app.display_w + 2 * PAGE_MARGIN;
    int new_content_h = g_app.display_h + 2 * PAGE_MARGIN;
    g_app.scroll_x = (int)(center_rx * new_content_w - cw / 2);
    g_app.scroll_y = (int)(center_ry * new_content_h - ch / 2);
    ClampScroll();

    UpdateTitle();
    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void ZoomIn(void)
{
    double z = g_app.zoom * ZOOM_IN_FACTOR;
    ApplyZoom(z, false);
}

static void ZoomOut(void)
{
    double z = g_app.zoom * ZOOM_OUT_FACTOR;
    ApplyZoom(z, false);
}

static void ZoomFitWidth(void)
{
    double z = CalculateFitWidthZoom();
    ApplyZoom(z, true);
}

static void ZoomActualSize(void)
{
    ApplyZoom(1.0, false);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Page navigation
 * ═══════════════════════════════════════════════════════════════════════ */
static void NavigatePage(int page)
{
    if (!g_app.doc_loaded) return;

    int count = pdf_page_count(&g_app.doc);
    if (count <= 0) return;

    if (page < 0) page = 0;
    if (page >= count) page = count - 1;
    if (page == g_app.current_page) return;

    g_app.current_page = page;
    g_app.scroll_x = 0;
    g_app.scroll_y = 0;

    RenderCurrentPage();
    UpdateTitle();
    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void NavigatePageDelta(int delta)
{
    if (!g_app.doc_loaded) return;
    NavigatePage(g_app.current_page + delta);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Go To Page dialog
 * ═══════════════════════════════════════════════════════════════════════ */

/* Dialog procedure for the Go To Page popup */
static INT_PTR CALLBACK GoToPageDlgProc(HWND hDlg, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
    {
        /* Center on parent */
        RECT rcParent, rcDlg;
        GetWindowRect(g_app.hWnd, &rcParent);
        GetWindowRect(hDlg, &rcDlg);
        int dx = ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
        int dy = ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
        SetWindowPos(hDlg, NULL,
            rcParent.left + dx, rcParent.top + dy,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);

        /* Set default text to current page */
        wchar_t buf[32];
        StringCchPrintfW(buf, 32, L"%d", g_app.current_page + 1);
        SetDlgItemTextW(hDlg, 101, buf);

        /* Select all text in the edit control */
        HWND hEdit = GetDlgItem(hDlg, 101);
        SendMessageW(hEdit, EM_SETSEL, 0, -1);
        SetFocus(hEdit);
        return FALSE;  /* we set focus ourselves */
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
        {
            wchar_t buf[32];
            GetDlgItemTextW(hDlg, 101, buf, 32);
            int page = _wtoi(buf);
            EndDialog(hDlg, page);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, 0);
        return TRUE;
    }
    return FALSE;
}

static void GoToPageDialog(void)
{
    if (!g_app.doc_loaded) return;

    int count = pdf_page_count(&g_app.doc);
    if (count <= 0) return;

    /*
     * Build a dialog template in memory.
     * Layout: a static label, an edit control, OK/Cancel buttons.
     */
    /* We use a simpler approach: a small modeless trick with DialogBoxIndirectParam */
    #pragma pack(push, 4)
    struct {
        DLGTEMPLATE tmpl;
        WORD menu;
        WORD cls;
        wchar_t title[16];
        /* Items follow, each DWORD-aligned */
        /* Item 0: Static label */
        DLGITEMTEMPLATE item0;
        WORD cls0_arr; WORD cls0_id;
        wchar_t text0[16];
        WORD creation0;
        /* Item 1: Edit control */
        DLGITEMTEMPLATE item1;
        WORD cls1_arr; WORD cls1_id;
        wchar_t text1[1];
        WORD creation1;
        /* Item 2: OK button */
        DLGITEMTEMPLATE item2;
        WORD cls2_arr; WORD cls2_id;
        wchar_t text2[3];
        WORD creation2;
        /* Item 3: Cancel button */
        DLGITEMTEMPLATE item3;
        WORD cls3_arr; WORD cls3_id;
        wchar_t text3[7];
        WORD creation3;
    } dlg;
    #pragma pack(pop)

    memset(&dlg, 0, sizeof(dlg));

    /* Template header */
    dlg.tmpl.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg.tmpl.cdit  = 4;
    dlg.tmpl.cx    = 160;
    dlg.tmpl.cy    = 60;
    wcscpy(dlg.title, L"Go To Page");

    /* Static label: "Page (1-N):" */
    dlg.item0.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    dlg.item0.x  = 8;  dlg.item0.y  = 8;
    dlg.item0.cx = 60; dlg.item0.cy = 10;
    dlg.item0.id = 100;
    dlg.cls0_arr = 0xFFFF; dlg.cls0_id = 0x0082; /* Static */
    wcscpy(dlg.text0, L"Page:");

    /* Edit control */
    dlg.item1.style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER;
    dlg.item1.x  = 70; dlg.item1.y  = 6;
    dlg.item1.cx = 40; dlg.item1.cy = 12;
    dlg.item1.id = 101;
    dlg.cls1_arr = 0xFFFF; dlg.cls1_id = 0x0081; /* Edit */
    dlg.text1[0] = 0;

    /* OK button */
    dlg.item2.style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP;
    dlg.item2.x  = 30; dlg.item2.y  = 34;
    dlg.item2.cx = 40; dlg.item2.cy = 14;
    dlg.item2.id = IDOK;
    dlg.cls2_arr = 0xFFFF; dlg.cls2_id = 0x0080; /* Button */
    wcscpy(dlg.text2, L"OK");

    /* Cancel button */
    dlg.item3.style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
    dlg.item3.x  = 85; dlg.item3.y  = 34;
    dlg.item3.cx = 40; dlg.item3.cy = 14;
    dlg.item3.id = IDCANCEL;
    dlg.cls3_arr = 0xFFFF; dlg.cls3_id = 0x0080; /* Button */
    wcscpy(dlg.text3, L"Cancel");

    INT_PTR result = DialogBoxIndirectW(
        g_app.hInst, &dlg.tmpl, g_app.hWnd, GoToPageDlgProc);

    if (result > 0 && result <= count)
        NavigatePage((int)result - 1);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Fullscreen toggle (F11)
 * ═══════════════════════════════════════════════════════════════════════ */
static void ToggleFullscreen(void)
{
    if (!g_app.fullscreen) {
        /* Enter fullscreen */
        g_app.stylePrev   = (DWORD)GetWindowLongW(g_app.hWnd, GWL_STYLE);
        g_app.exStylePrev = (DWORD)GetWindowLongW(g_app.hWnd, GWL_EXSTYLE);
        g_app.wpPrev.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(g_app.hWnd, &g_app.wpPrev);

        SetWindowLongW(g_app.hWnd, GWL_STYLE,
            g_app.stylePrev & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowLongW(g_app.hWnd, GWL_EXSTYLE,
            g_app.exStylePrev & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                   WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

        MONITORINFO mi = { .cbSize = sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(g_app.hWnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowPos(g_app.hWnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        g_app.fullscreen = true;
    } else {
        /* Exit fullscreen */
        SetWindowLongW(g_app.hWnd, GWL_STYLE, g_app.stylePrev);
        SetWindowLongW(g_app.hWnd, GWL_EXSTYLE, g_app.exStylePrev);
        SetWindowPlacement(g_app.hWnd, &g_app.wpPrev);
        SetWindowPos(g_app.hWnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        g_app.fullscreen = false;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Title bar update
 * ═══════════════════════════════════════════════════════════════════════ */
static void UpdateTitle(void)
{
    if (!g_app.doc_loaded) {
        SetWindowTextW(g_app.hWnd, APP_TITLE);
        return;
    }

    /* Extract filename from path */
    const wchar_t *filename = g_app.file_path;
    const wchar_t *sep = wcsrchr(g_app.file_path, L'\\');
    const wchar_t *sep2 = wcsrchr(g_app.file_path, L'/');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
    if (sep) filename = sep + 1;

    int count = pdf_page_count(&g_app.doc);
    int zoom_pct = (int)(g_app.zoom * 100.0 + 0.5);

    wchar_t title[MAX_PATH + 128];
    StringCchPrintfW(title, MAX_PATH + 128,
        L"%s - " APP_TITLE L" - Page %d of %d (%d%%)",
        filename,
        g_app.current_page + 1,
        count,
        zoom_pct);
    SetWindowTextW(g_app.hWnd, title);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Painting
 * ═══════════════════════════════════════════════════════════════════════ */
static void PaintWindow(HDC hdc, const RECT *rcClient)
{
    int cw = rcClient->right - rcClient->left;
    int ch = rcClient->bottom - rcClient->top;

    /* Check if we've scrolled past the region boundary and need a re-render */
    if (g_app.region_mode && NeedsRegionRerender()) {
        /* Force re-render centered on current viewport */
        g_app.rendered_page = -1;
        RenderCurrentPage();
    }

    /* Fill entire background */
    FillRect(hdc, rcClient, g_app.hBgBrush);

    if (!g_app.doc_loaded || !g_app.hPageBitmap ||
        g_app.display_w == 0 || g_app.display_h == 0) {
        /* No document -- show hint text */
        SetTextColor(hdc, TEXT_HINT_COLOR);
        SetBkMode(hdc, TRANSPARENT);

        HFONT hFont = CreateFontW(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        RECT rcText = *rcClient;
        DrawTextW(hdc,
            L"Open a PDF file (Ctrl+O) or drag and drop one here",
            -1, &rcText,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        return;
    }

    /* Use display dimensions for layout (may differ from bitmap size when oversampled) */
    int dw = g_app.display_w;
    int dh = g_app.display_h;

    /* Calculate page position (centered if smaller, or scrolled) */
    int page_x, page_y;

    if (dw + 2 * PAGE_MARGIN <= cw) {
        /* Page fits horizontally: center it */
        page_x = (cw - dw) / 2;
    } else {
        /* Page wider than window: apply horizontal scroll */
        page_x = PAGE_MARGIN - g_app.scroll_x;
    }

    if (dh + 2 * PAGE_MARGIN <= ch) {
        /* Page fits vertically: center it */
        page_y = (ch - dh) / 2;
    } else {
        /* Page taller than window: apply vertical scroll */
        page_y = PAGE_MARGIN - g_app.scroll_y;
    }

    /* Draw drop shadow */
    RECT rcShadow = {
        page_x + SHADOW_OFFSET,
        page_y + SHADOW_OFFSET,
        page_x + dw + SHADOW_OFFSET,
        page_y + dh + SHADOW_OFFSET
    };
    FillRect(hdc, &rcShadow, g_app.hShadowBrush);

    /* Draw page border (1px around the page) */
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, PAGE_BORDER_COLOR);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc,
        page_x - 1, page_y - 1,
        page_x + dw + 1,
        page_y + dh + 1);

    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBorderPen);

    /* Blit the rendered page to screen */
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, g_app.hPageBitmap);

    if (g_app.region_mode) {
        /*
         * Region mode: the bitmap covers only a sub-region of the page.
         * We need to compute where the bitmap should be placed relative
         * to the page origin, accounting for the region offset.
         *
         * render_scale is the scale at which the bitmap was rendered.
         * effective_zoom is the user-visible zoom level.
         * display_ratio maps from render pixels to display pixels.
         */
        double render_scale = g_app.render_scale;
        double effective_zoom = g_app.rendered_zoom;
        double display_ratio = (render_scale > 0.0) ? effective_zoom / render_scale : 1.0;

        /* Where the region starts in display coordinates (relative to page origin) */
        int region_disp_x = (int)(g_app.region_x * effective_zoom + 0.5);
        int region_disp_y = (int)(g_app.region_y * effective_zoom + 0.5);

        /* Display size of the rendered region */
        int region_disp_w = (int)(g_app.page_bmp_w * display_ratio + 0.5);
        int region_disp_h = (int)(g_app.page_bmp_h * display_ratio + 0.5);

        /* Destination on screen */
        int dst_x = page_x + region_disp_x;
        int dst_y = page_y + region_disp_y;

        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);

        if (region_disp_w != g_app.page_bmp_w || region_disp_h != g_app.page_bmp_h) {
            StretchBlt(hdc, dst_x, dst_y, region_disp_w, region_disp_h,
                       hdcMem, 0, 0, g_app.page_bmp_w, g_app.page_bmp_h, SRCCOPY);
        } else {
            BitBlt(hdc, dst_x, dst_y, region_disp_w, region_disp_h,
                   hdcMem, 0, 0, SRCCOPY);
        }
    } else if (dw != g_app.page_bmp_w || dh != g_app.page_bmp_h) {
        /* Oversampled: downscale with high-quality interpolation */
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, page_x, page_y, dw, dh,
                   hdcMem, 0, 0, g_app.page_bmp_w, g_app.page_bmp_h, SRCCOPY);
    } else {
        BitBlt(hdc, page_x, page_y, dw, dh,
               hdcMem, 0, 0, SRCCOPY);
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Drag-and-drop support
 * ═══════════════════════════════════════════════════════════════════════ */
static void HandleDropFiles(HDROP hDrop)
{
    wchar_t path[MAX_PATH];
    if (DragQueryFileW(hDrop, 0, path, MAX_PATH) > 0) {
        /* Check if it has a .pdf extension */
        const wchar_t *ext = wcsrchr(path, L'.');
        if (ext && _wcsicmp(ext, L".pdf") == 0) {
            OpenDocument(path);
        }
    }
    DragFinish(hDrop);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Window procedure
 * ═══════════════════════════════════════════════════════════════════════ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    /* ─── Painting ─── */
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        if (w > 0 && h > 0) {
            /* Double-buffered painting */
            HDC hdcBuf = CreateCompatibleDC(hdc);
            HBITMAP hBmpBuf = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcBuf, hBmpBuf);

            PaintWindow(hdcBuf, &rc);

            BitBlt(hdc, 0, 0, w, h, hdcBuf, 0, 0, SRCCOPY);

            SelectObject(hdcBuf, hOld);
            DeleteObject(hBmpBuf);
            DeleteDC(hdcBuf);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; /* handled in WM_PAINT (double-buffered) */

    /* ─── Resize ─── */
    case WM_SIZE:
    {
        if (g_app.doc_loaded && g_app.fit_width) {
            /* Debounce re-render during live resize */
            g_app.resize_pending = true;
            KillTimer(hwnd, TIMER_RESIZE);
            SetTimer(hwnd, TIMER_RESIZE, TIMER_RESIZE_MS, NULL);
        }
        UpdateScrollLimits();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == TIMER_RESIZE) {
            KillTimer(hwnd, TIMER_RESIZE);
            g_app.resize_pending = false;
            if (g_app.doc_loaded && g_app.fit_width) {
                g_app.rendered_page = -1; /* force re-render */
                RenderCurrentPage();
                UpdateTitle();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    /* ─── Keyboard ─── */
    case WM_KEYDOWN:
    {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        (void)shift;

        switch (wParam) {

        /* Zoom */
        case VK_OEM_PLUS:  /* = / + key */
        case VK_ADD:       /* numpad + */
            if (ctrl) { ZoomIn(); return 0; }
            break;

        case VK_OEM_MINUS: /* - key */
        case VK_SUBTRACT:  /* numpad - */
            if (ctrl) { ZoomOut(); return 0; }
            break;

        case '0':
            if (ctrl) { ZoomFitWidth(); return 0; }
            break;

        case '1':
            if (ctrl) { ZoomActualSize(); return 0; }
            break;

        /* Page navigation */
        case VK_LEFT:
            if (ctrl) {
                NavigatePageDelta(-1);
                return 0;
            }
            /* Scroll left, or prev page if at leftmost */
            if (g_app.scroll_x > 0) {
                g_app.scroll_x -= SCROLL_LINE;
                ClampScroll();
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                NavigatePageDelta(-1);
            }
            return 0;

        case VK_RIGHT:
            if (ctrl) {
                NavigatePageDelta(1);
                return 0;
            }
            /* Scroll right, or next page if at rightmost */
            if (g_app.scroll_x < g_app.max_scroll_x) {
                g_app.scroll_x += SCROLL_LINE;
                ClampScroll();
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                NavigatePageDelta(1);
            }
            return 0;

        case VK_UP:
            if (ctrl) break; /* let Ctrl+Up fall through */
            if (g_app.scroll_y > 0) {
                g_app.scroll_y -= SCROLL_LINE;
                ClampScroll();
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                NavigatePageDelta(-1);
            }
            return 0;

        case VK_DOWN:
            if (ctrl) break;
            if (g_app.scroll_y < g_app.max_scroll_y) {
                g_app.scroll_y += SCROLL_LINE;
                ClampScroll();
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                NavigatePageDelta(1);
            }
            return 0;

        case VK_PRIOR: /* Page Up */
            if (ctrl) {
                NavigatePageDelta(-1);
            } else {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int page_scroll = rc.bottom - rc.top - SCROLL_LINE;
                if (page_scroll < SCROLL_LINE) page_scroll = SCROLL_LINE;
                if (g_app.scroll_y > 0) {
                    g_app.scroll_y -= page_scroll;
                    ClampScroll();
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    NavigatePageDelta(-1);
                }
            }
            return 0;

        case VK_NEXT: /* Page Down */
            if (ctrl) {
                NavigatePageDelta(1);
            } else {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int page_scroll = rc.bottom - rc.top - SCROLL_LINE;
                if (page_scroll < SCROLL_LINE) page_scroll = SCROLL_LINE;
                if (g_app.scroll_y < g_app.max_scroll_y) {
                    g_app.scroll_y += page_scroll;
                    ClampScroll();
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    NavigatePageDelta(1);
                }
            }
            return 0;

        case VK_HOME:
            if (ctrl) {
                NavigatePage(0);
            } else {
                g_app.scroll_y = 0;
                g_app.scroll_x = 0;
                ClampScroll();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case VK_END:
            if (ctrl) {
                if (g_app.doc_loaded)
                    NavigatePage(pdf_page_count(&g_app.doc) - 1);
            } else {
                g_app.scroll_y = g_app.max_scroll_y;
                ClampScroll();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        /* File open */
        case 'O':
            if (ctrl) { OpenFileDialog(); return 0; }
            break;

        /* Go to page */
        case 'G':
            if (ctrl) { GoToPageDialog(); return 0; }
            break;

        /* Fullscreen */
        case VK_F11:
            ToggleFullscreen();
            return 0;

        case VK_ESCAPE:
            if (g_app.fullscreen) ToggleFullscreen();
            return 0;
        }
        break;
    }

    /* ─── Mouse wheel ─── */
    case WM_MOUSEWHEEL:
    {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        WORD keys  = GET_KEYSTATE_WPARAM(wParam);
        bool ctrl  = (keys & MK_CONTROL) != 0;
        bool shift = (keys & MK_SHIFT)   != 0;

        if (ctrl) {
            /* Zoom */
            if (delta > 0)
                ZoomIn();
            else
                ZoomOut();
        } else if (shift) {
            /* Horizontal scroll */
            int amount = (SCROLL_LINE * SCROLL_WHEEL_LINES * delta) / WHEEL_DELTA;
            g_app.scroll_x -= amount;
            ClampScroll();
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            /* Vertical scroll */
            int amount = (SCROLL_LINE * SCROLL_WHEEL_LINES * delta) / WHEEL_DELTA;
            g_app.scroll_y -= amount;
            ClampScroll();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    /* ─── Mouse drag for panning ─── */
    case WM_LBUTTONDOWN:
    {
        g_app.dragging = true;
        g_app.drag_start.x = GET_X_LPARAM(lParam);
        g_app.drag_start.y = GET_Y_LPARAM(lParam);
        g_app.drag_scroll_x = g_app.scroll_x;
        g_app.drag_scroll_y = g_app.scroll_y;
        SetCapture(hwnd);
        SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
        return 0;
    }

    case WM_MBUTTONDOWN:
    {
        g_app.dragging = true;
        g_app.drag_start.x = GET_X_LPARAM(lParam);
        g_app.drag_start.y = GET_Y_LPARAM(lParam);
        g_app.drag_scroll_x = g_app.scroll_x;
        g_app.drag_scroll_y = g_app.scroll_y;
        SetCapture(hwnd);
        SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (g_app.dragging) {
            int dx = GET_X_LPARAM(lParam) - g_app.drag_start.x;
            int dy = GET_Y_LPARAM(lParam) - g_app.drag_start.y;
            g_app.scroll_x = g_app.drag_scroll_x - dx;
            g_app.scroll_y = g_app.drag_scroll_y - dy;
            ClampScroll();
            InvalidateRect(hwnd, NULL, FALSE);
            SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
        }
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    {
        if (g_app.dragging) {
            g_app.dragging = false;
            ReleaseCapture();
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
        }
        return 0;
    }

    /* ─── Drag and drop ─── */
    case WM_DROPFILES:
        HandleDropFiles((HDROP)wParam);
        return 0;

    /* ─── Menu commands ─── */
    case WM_COMMAND:
    {
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN:     OpenFileDialog();       return 0;
        case IDM_FILE_EXIT:     DestroyWindow(hwnd);    return 0;
        case IDM_VIEW_ZOOMIN:   ZoomIn();               return 0;
        case IDM_VIEW_ZOOMOUT:  ZoomOut();              return 0;
        case IDM_VIEW_FITWIDTH: ZoomFitWidth();         return 0;
        case IDM_VIEW_ACTUALSIZE: ZoomActualSize();     return 0;
        case IDM_VIEW_FULLSCREEN: ToggleFullscreen();   return 0;
        case IDM_GO_FIRST:      NavigatePage(0);        return 0;
        case IDM_GO_PREV:       NavigatePageDelta(-1);  return 0;
        case IDM_GO_NEXT:       NavigatePageDelta(1);   return 0;
        case IDM_GO_LAST:
            if (g_app.doc_loaded)
                NavigatePage(pdf_page_count(&g_app.doc) - 1);
            return 0;
        case IDM_GO_GOTO:       GoToPageDialog();       return 0;
        }
        break;
    }

    /* ─── Cleanup ─── */
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_RESIZE);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
