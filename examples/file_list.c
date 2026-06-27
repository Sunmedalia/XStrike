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
 * Build (mingw):
 *   gcc -c examples/file_list.c -o examples/file_list.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$FindNextFileA(HANDLE, LPWIN32_FIND_DATAA);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$FindClose(HANDLE);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetCurrentDirectoryA(DWORD, LPSTR);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define BUF_SIZE 131072

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

/* Convert a FILETIME (UTC, 100ns ticks since 1601-01-01) to unix seconds. */
static unsigned long long filetime_to_epoch(const FILETIME *ft) {
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    /* 116444736000000000 = 100ns ticks from 1601-01-01 to 1970-01-01 */
    if (t < 116444736000000000ULL) return 0;
    return (t - 116444736000000000ULL) / 10000000ULL;
}

void go(char *args, int alen) {
    char path[MAX_PATH];
    int plen = 0;

    if (alen > 0) {
        int off = 0;
        int n = read_bstr(args, alen, &off, path, (int)sizeof(path));
        while (n > 0 && (path[n-1] == '\r' || path[n-1] == '\n')) path[--n] = 0;
        plen = n;
    }
    if (plen <= 0) {
        /* default to the implant's current directory */
        DWORD got = KERNEL32$GetCurrentDirectoryA((DWORD)sizeof(path) - 4, path);
        if (got == 0 || got >= sizeof(path) - 4) { path[0] = '.'; path[1] = 0; }
        plen = 0; while (path[plen]) plen++;
    }

    /* FindFirstFileA needs a wildcard; append "\*" if none present. */
    int hasWild = 0;
    for (int i = 0; i < plen; i++) if (path[i] == '*' || path[i] == '?') { hasWild = 1; break; }
    char search[MAX_PATH];
    if (!hasWild) {
        /* build "<path>\<*>" — guard the copy */
        if (plen >= (int)sizeof(search) - 3) plen = (int)sizeof(search) - 3;
        for (int i = 0; i < plen; i++) search[i] = path[i];
        if (plen > 0 && search[plen-1] != '\\' && search[plen-1] != '/') {
            search[plen++] = '\\';
        }
        search[plen++] = '*';
        search[plen] = 0;
    } else {
        if (plen >= (int)sizeof(search) - 1) plen = (int)sizeof(search) - 1;
        for (int i = 0; i < plen; i++) search[i] = path[i];
        search[plen] = 0;
    }

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "file_list: out of memory"); return; }
    int total = 0;

    /* Emit the CWD header so the UI path bar syncs to the resolved path
     * (strip the trailing "\*" we appended for the search). */
    char cwd[MAX_PATH];
    int cwdlen = 0;
    {
        int s = 0;
        while (search[s] && s < (int)sizeof(cwd)-2) { cwd[s] = search[s]; s++; }
        cwd[s] = 0; cwdlen = s;
    }
    if (cwdlen > 0 && cwd[cwdlen-1] == '*') cwd[--cwdlen] = 0;
    if (cwdlen > 0 && cwd[cwdlen-1] == '\\') cwd[--cwdlen] = 0;
    total = MSVCRT$_snprintf(buf, BUF_SIZE - 1, "CWD: %s\r\n", cwd);

    WIN32_FIND_DATAA fd;
    HANDLE h = KERNEL32$FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "file_list: FindFirstFileA failed (%lu) for %s",
                     KERNEL32$GetLastError(), search);
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }
    do {
        /* skip "." and ".." — the frontend ignores them, but keep it clean */
        if ((fd.cFileName[0] == '.' && fd.cFileName[1] == 0) ||
            (fd.cFileName[0] == '.' && fd.cFileName[1] == '.' && fd.cFileName[2] == 0)) {
            continue;
        }
        const char *type = (fd.dwFileAttributes & 0x10 /*FILE_ATTRIBUTE_DIRECTORY*/) ? "D" : "F";
        unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        unsigned long long epoch = filetime_to_epoch(&fd.ftLastWriteTime);
        int n = MSVCRT$_snprintf(buf + total, BUF_SIZE - 1 - total,
            "%s\t%s\t%llu\t%llu\r\n", type, fd.cFileName, size, epoch);
        if (n < 0) n = 0;
        total += n;
        if (total >= BUF_SIZE - 256) {
            BeaconOutput(CALLBACK_OUTPUT, buf, total);
            KERNEL32$FindClose(h);
            KERNEL32$HeapFree(heap, 0, buf);
            BeaconPrintf(CALLBACK_ERROR, "file_list: output truncated at %d bytes", BUF_SIZE);
            return;
        }
    } while (KERNEL32$FindNextFileA(h, &fd));
    KERNEL32$FindClose(h);

    BeaconOutput(CALLBACK_OUTPUT, buf, total);
    KERNEL32$HeapFree(heap, 0, buf);
}
