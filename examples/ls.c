/*
 * ls BOF for RustStrike — list a directory.
 *
 *   args: optional raw text path (default "."). e.g. "ls C:\\Users" or "ls ."
 *
 * Prints one line per entry: TYPE  SIZE        NAME  (TYPE is DIR/FILE).
 * Output is built in a heap buffer and flushed once via BeaconOutput.
 *
 * Build (mingw):
 *   gcc -c examples/ls.c -o examples/ls.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$FindNextFileA(HANDLE, LPWIN32_FIND_DATAA);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$FindClose(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define BUF_SIZE 65536

void go(char *args, int alen) {
    char path[MAX_PATH];
    int plen = alen < (int)sizeof(path) - 3 ? alen : (int)sizeof(path) - 3;
    if (plen <= 0) {
        path[0] = '.'; path[1] = 0; plen = 1;
    } else {
        for (int i = 0; i < plen; i++) path[i] = args[i];
        path[plen] = 0;
        while (plen > 0 && (path[plen - 1] == '\n' || path[plen - 1] == '\r')) path[--plen] = 0;
        if (plen == 0) { path[0] = '.'; path[1] = 0; plen = 1; }
    }
    /* FindFirstFile needs a wildcard; append "\*" if none present. */
    int hasWild = 0;
    for (int i = 0; i < plen; i++) if (path[i] == '*' || path[i] == '?') { hasWild = 1; break; }
    if (!hasWild) {
        if (plen > 0 && path[plen - 1] != '\\' && path[plen - 1] != '/') {
            path[plen++] = '\\'; path[plen] = 0;
        }
        path[plen++] = '*'; path[plen] = 0;
    }

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "ls: out of memory"); return; }
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = 0;
    int total = MSVCRT$_snprintf(buf, BUF_SIZE - 1, "Listing: %s\r\n%-5s %-12s %s\r\n",
                                 path, "TYPE", "SIZE", "NAME");

    WIN32_FIND_DATAA fd;
    HANDLE h = KERNEL32$FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "ls: FindFirstFileA failed (%lu) for %s",
                     KERNEL32$GetLastError(), path);
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }
    do {
        const char *type = (fd.dwFileAttributes & 0x10 /*FILE_ATTRIBUTE_DIRECTORY*/) ? "DIR" : "FILE";
        unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        total += MSVCRT$_snprintf(buf + total, BUF_SIZE - 1 - total, "%-5s %-12llu %s\r\n",
                                  type, size, fd.cFileName);
        if (total >= BUF_SIZE - 256) {
            BeaconOutput(CALLBACK_OUTPUT, buf, total);
            KERNEL32$FindClose(h);
            KERNEL32$HeapFree(heap, 0, buf);
            BeaconPrintf(CALLBACK_ERROR, "ls: output truncated at %d bytes", BUF_SIZE);
            return;
        }
    } while (KERNEL32$FindNextFileA(h, &fd));
    KERNEL32$FindClose(h);

    BeaconOutput(CALLBACK_OUTPUT, buf, total);
    KERNEL32$HeapFree(heap, 0, buf);
}
