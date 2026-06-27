/*
 * proc_list BOF for RustStrike — list running processes (component format).
 *
 *   args: none
 *
 * Drives ProcessList.vue. Prints one line per process, TAB-separated:
 *   NAME    PID    PPID    THREADS
 * The frontend parses exactly 4 fields (name, pid, ppid, threads) and leaves
 * arch/user/path as '-'. (v1: no per-process user/arch lookup.)
 *
 * Built with the same Toolhelp32 walk as ps.c, but emits the component's
 * TAB-separated schema instead of ps.c's column-formatted text.
 *
 * Build (mingw):
 *   gcc -c examples/proc_list.c -o examples/proc_list.x64.o
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

#define BUF_SIZE 131072

void go(char *args, int alen) {
    (void)args; (void)alen;   /* proc_list takes no args */

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "proc_list: out of memory"); return; }
    int total = 0;

    HANDLE snap = KERNEL32$CreateToolhelp32Snapshot(0x00000002 /*TH32CS_SNAPPROCESS*/, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "proc_list: CreateToolhelp32Snapshot failed (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (KERNEL32$Process32First(snap, &pe)) {
        do {
            /* NAME \t PID \t PPID \t THREADS \r\n  — the component splits on \t */
            int n = MSVCRT$_snprintf(buf + total, BUF_SIZE - 1 - total,
                "%s\t%lu\t%lu\t%lu\r\n",
                pe.szExeFile, pe.th32ProcessID, pe.th32ParentProcessID,
                pe.cntThreads);
            if (n < 0) n = 0;
            total += n;
            if (total >= BUF_SIZE - 256) {
                BeaconOutput(CALLBACK_OUTPUT, buf, total);
                KERNEL32$CloseHandle(snap);
                KERNEL32$HeapFree(heap, 0, buf);
                BeaconPrintf(CALLBACK_ERROR, "proc_list: output truncated at %d bytes", BUF_SIZE);
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
