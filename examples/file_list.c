/*
 * file_list BOF for RustStrike — list a directory (component format).
 *
 *   args: encodeBeaconString(path) optional — [2-byte LE length][UTF-8 path][null]
 *         as produced by FileBrowser.vue. Defaults to the current directory.
 *
 * Drives FileBrowser.vue. Output:
 *   CWD: <resolved path>\r\n            (optional; sets the path bar in the UI)
 *   D\t<name>\t<size>\t<epoch>\r\n      per entry (D = dir, F = file)
 *   F\t<name>\t<size>\t<epoch>\r\n
 * The frontend splits each non-CWD line on \t into [type, name, size, epoch]
 * and renders rows; epoch is unix seconds (from ftLastWriteTime). "." and ".."
 * are skipped.
 *
 * UNICODE SAFE: uses FindFirstFileW + MultiByteToWideChar/WideCharToMultiByte
 * so non-ASCII (Chinese, etc.) paths and filenames round-trip correctly. The
 * ANSI FindFirstFileA variant mis-decodes UTF-8 path bytes as the system ACP
 * (GBK on a Chinese Windows) and fails on any non-ASCII segment — which is why
 * deep navigation into localized folders broke. The path is also accepted with
 * a \\?\ prefix so paths longer than MAX_PATH work.
 *
 * Build (mingw):
 *   gcc -c examples/file_list.c -o examples/file_list.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$FindFirstFileW(LPCWSTR, LPWIN32_FIND_DATAW);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$FindNextFileW(HANDLE, LPWIN32_FIND_DATAW);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$FindClose(HANDLE);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetCurrentDirectoryW(DWORD, LPWSTR);
DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID  WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
DECLSPEC_IMPORT int     WINAPI KERNEL32$WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, LPBOOL);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define CP_UTF8_ 65001
#define BUF_SIZE 131072
#define WPATH_MAX 264   /* wide chars — fits MAX_PATH + "\*" with room */

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

/* Convert a UTF-8 NUL-terminated string to a UTF-16 wide string. Returns the
 * number of wide chars written (excluding the NUL), or 0 on failure. */
static int utf8_to_wide(const char *src, wchar_t *dst, int dstcap) {
    int n = KERNEL32$MultiByteToWideChar(CP_UTF8_, 0, src, -1, dst, dstcap);
    if (n <= 0) { dst[0] = 0; return 0; }
    return n - 1;   /* exclude the NUL terminator from the count */
}

/* Convert a UTF-16 wide string to UTF-8. Returns bytes written (excl. NUL). */
static int wide_to_utf8(const wchar_t *src, char *dst, int dstcap) {
    int n = KERNEL32$WideCharToMultiByte(CP_UTF8_, 0, src, -1, dst, dstcap, NULL, NULL);
    if (n <= 0) { dst[0] = 0; return 0; }
    return n - 1;
}

/* Convert a FILETIME (UTC, 100ns ticks since 1601-01-01) to unix seconds. */
static unsigned long long filetime_to_epoch(const FILETIME *ft) {
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    /* 116444736000000000 = 100ns ticks from 1601-01-01 to 1970-01-01 */
    if (t < 116444736000000000ULL) return 0;
    return (t - 116444736000000000ULL) / 10000000ULL;
}

static int wlen(const wchar_t *s) { int n = 0; while (s[n]) n++; return n; }

void go(char *args, int alen) {
    char path[260];
    int plen = 0;

    if (alen > 0) {
        int off = 0;
        int n = read_bstr(args, alen, &off, path, (int)sizeof(path));
        while (n > 0 && (path[n-1] == '\r' || path[n-1] == '\n')) path[--n] = 0;
        plen = n;
    }
    if (plen <= 0) {
        /* default to the implant's current directory (wide, then back to UTF-8) */
        wchar_t wcur[WPATH_MAX];
        DWORD got = KERNEL32$GetCurrentDirectoryW((DWORD)WPATH_MAX, wcur);
        if (got == 0 || got >= (DWORD)WPATH_MAX) { path[0] = '.'; path[1] = 0; plen = 1; }
        else { plen = wide_to_utf8(wcur, path, (int)sizeof(path)); }
    }

    /* Convert the UTF-8 path to UTF-16 for the W APIs. */
    wchar_t wpath[WPATH_MAX];
    utf8_to_wide(path, wpath, WPATH_MAX);

    /* FindFirstFileW needs a wildcard; append "\*" if none present. */
    int wpl = wlen(wpath);
    int hasWild = 0;
    for (int i = 0; i < wpl; i++) if (wpath[i] == L'*' || wpath[i] == L'?') { hasWild = 1; break; }
    wchar_t search[WPATH_MAX];
    int sl = 0;
    if (wpl >= WPATH_MAX - 3) wpl = WPATH_MAX - 3;
    for (int i = 0; i < wpl; i++) search[sl++] = wpath[i];
    if (!hasWild) {
        if (sl > 0 && search[sl-1] != L'\\' && search[sl-1] != L'/') search[sl++] = L'\\';
        search[sl++] = L'*';
    }
    search[sl] = 0;

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "file_list: out of memory"); return; }
    int total = 0;

    /* Emit the CWD header (UTF-8) so the UI path bar syncs to the resolved path
     * (strip the trailing "\*" we appended for the search). */
    char cwd[260];
    int cwdlen = plen;
    for (int i = 0; i < cwdlen && i < (int)sizeof(cwd) - 1; i++) cwd[i] = path[i];
    cwd[cwdlen] = 0;
    if (cwdlen > 0 && cwd[cwdlen-1] == '\\') cwd[--cwdlen] = 0;
    total = MSVCRT$_snprintf(buf, BUF_SIZE - 1, "CWD: %s\r\n", cwd);

    WIN32_FIND_DATAW fd;
    HANDLE h = KERNEL32$FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "file_list: FindFirstFileW failed (%lu) for %s",
                     KERNEL32$GetLastError(), cwd);
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }
    do {
        /* skip "." and ".." — the frontend ignores them, but keep it clean */
        if ((fd.cFileName[0] == L'.' && fd.cFileName[1] == 0) ||
            (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)) {
            continue;
        }
        const char *type = (fd.dwFileAttributes & 0x10 /*FILE_ATTRIBUTE_DIRECTORY*/) ? "D" : "F";
        unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        unsigned long long epoch = filetime_to_epoch(&fd.ftLastWriteTime);
        /* Convert the UTF-16 filename to UTF-8 so the frontend renders non-ASCII
         * (Chinese, etc.) names correctly instead of mojibake. A MAX_PATH (260)
         * wchar name can expand to ~1040 UTF-8 bytes (4 bytes/char worst case). */
        char uname[1048];
        wide_to_utf8(fd.cFileName, uname, (int)sizeof(uname));
        int n = MSVCRT$_snprintf(buf + total, BUF_SIZE - 1 - total,
            "%s\t%s\t%llu\t%llu\r\n", type, uname, size, epoch);
        if (n < 0) n = 0;
        total += n;
        if (total >= BUF_SIZE - 256) {
            BeaconOutput(CALLBACK_OUTPUT, buf, total);
            KERNEL32$FindClose(h);
            KERNEL32$HeapFree(heap, 0, buf);
            BeaconPrintf(CALLBACK_ERROR, "file_list: output truncated at %d bytes", BUF_SIZE);
            return;
        }
    } while (KERNEL32$FindNextFileW(h, &fd));
    KERNEL32$FindClose(h);

    BeaconOutput(CALLBACK_OUTPUT, buf, total);
    KERNEL32$HeapFree(heap, 0, buf);
}
