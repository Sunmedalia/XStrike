/*
 * upload BOF for RustStrike — write a file to the target from operator-supplied
 * bytes.
 *
 *   args: Cobalt Strike packed buffer with TWO BeaconDataExtract blobs:
 *         [4-byte BE len][remote path][4-byte BE len][file contents]
 *       Build it with tools/upload_args.py <remote_path> <local_file>.
 *
 * Writes the contents to `remote path` (CREATE_ALWAYS) and prints
 *   uploaded <path> <n> bytes
 *
 * Build (mingw):
 *   gcc -c examples/upload.c -o examples/upload.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$WriteFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

void go(char *args, int alen) {
    if (alen <= 0) { BeaconPrintf(CALLBACK_ERROR, "upload: no args (need path + content)"); return; }

    datap parser;
    BeaconDataParse(&parser, args, alen);

    int pathLen = 0;
    char *path = BeaconDataExtract(&parser, &pathLen);
    int contentLen = 0;
    char *content = BeaconDataExtract(&parser, &contentLen);
    if (pathLen <= 0 || !path) {
        BeaconPrintf(CALLBACK_ERROR, "upload: missing path in args");
        return;
    }
    /* NUL-terminate the path safely (pathLen may not include one). */
    char pathBuf[MAX_PATH];
    int pl = pathLen < (int)sizeof(pathBuf) - 1 ? pathLen : (int)sizeof(pathBuf) - 1;
    for (int i = 0; i < pl; i++) pathBuf[i] = path[i];
    pathBuf[pl] = 0;

    HANDLE h = KERNEL32$CreateFileA(pathBuf, 0x40000000 /*GENERIC_WRITE*/, 0, NULL,
        2 /*CREATE_ALWAYS*/, 0x80 /*FILE_ATTRIBUTE_NORMAL*/, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "upload: cannot create %s (%lu)", pathBuf, KERNEL32$GetLastError());
        return;
    }

    DWORD written = 0;
    if (contentLen > 0 && content) {
        if (!KERNEL32$WriteFile(h, content, contentLen, &written, NULL)) {
            BeaconPrintf(CALLBACK_ERROR, "upload: WriteFile failed (%lu)", KERNEL32$GetLastError());
            KERNEL32$CloseHandle(h);
            return;
        }
    }
    KERNEL32$CloseHandle(h);

    char msg[512];
    MSVCRT$_snprintf(msg, sizeof(msg) - 1, "uploaded %s %lu bytes", pathBuf, written);
    msg[sizeof(msg) - 1] = 0;
    BeaconPrintf(CALLBACK_OUTPUT, "%s", msg);
}
