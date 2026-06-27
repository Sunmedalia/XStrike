/*
 * ps BOF for RustStrike — list running processes.
 *
 *   args: none
 *
 * Snapshots the process list via Toolhelp32 and prints one line per process:
 *   PID    PPID   NAME
 * Output is built in a heap buffer (avoids a large stack frame / __chkstk)
 * and flushed once via BeaconOutput.
 *
 * Build (mingw):
 *   gcc -c examples/ps.c -o examples/ps.x64.o
 */
#include <windows.h>
#include <tlhelp32.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateToolhelp32Snapshot(DWORD, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$Process32First(HANDLE, LPPROCESSENTRY32);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$Process32Next(HANDLE, LPPROCESSENTRY32);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define BUF_SIZE 65536

void go(char *args, int alen) {
    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "ps: out of memory"); return; }
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = 0;
    int total = 0;

    total += MSVCRT$_snprintf(buf + total, BUF_SIZE - 1 - total,
        "%-8s %-8s %s\r\n", "PID", "PPID", "NAME");

    HANDLE snap = KERNEL32$CreateToolhelp32Snapshot(0x00000002 /*TH32CS_SNAPPROCESS*/, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "ps: CreateToolhelp32Snapshot failed (%lu)", KERNEL32$GetLastError());
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (KERNEL32$Process32First(snap, &pe)) {
        do {
            total += MSVCRT$_snprintf(buf + total, BUF_SIZE - 1 - total,
                "%-8lu %-8lu %s\r\n", pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile);
            if (total >= BUF_SIZE - 256) {
                buf[BUF_SIZE - 1] = 0;
                BeaconOutput(CALLBACK_OUTPUT, buf, total);
                KERNEL32$CloseHandle(snap);
                KERNEL32$HeapFree(heap, 0, buf);
                BeaconPrintf(CALLBACK_ERROR, "ps: output truncated at %d bytes", BUF_SIZE);
                return;
            }
        } while (KERNEL32$Process32Next(snap, &pe));
    }
    KERNEL32$CloseHandle(snap);

    if (total > 0) {
        BeaconOutput(CALLBACK_OUTPUT, buf, total);
    } else {
        BeaconPrintf(CALLBACK_OUTPUT, "(no processes)");
    }
    KERNEL32$HeapFree(heap, 0, buf);
}
