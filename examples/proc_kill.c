/*
 * proc_kill BOF for RustStrike — terminate a process by PID (component format).
 *
 *   args: encodeBeaconString(pid)  — [2-byte LE length][UTF-8 digits][null]
 *         as produced by ProcessList.vue's encodeBeaconString(String(pid)).
 *
 * Opens the target with PROCESS_TERMINATE and calls TerminateProcess. Prints
 *   killed <pid>      on success
 *   proc_kill: ...    on failure (CALLBACK_ERROR)
 *
 * The frontend only checks result.success and toasts, so the exact success
 * text is free — but echoing the PID is useful in the log/terminal too.
 *
 * Build (mingw):
 *   gcc -c examples/proc_kill.c -o examples/proc_kill.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$OpenProcess(DWORD, WINBOOL, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$TerminateProcess(HANDLE, UINT);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$atoi(const char *);

/* Parse the RustStrike frontend's encodeBeaconString framing:
 *   [2-byte LE length][bytes][optional 0x00]
 * Copy up to outcap-1 bytes into out, NUL-terminate, advance *off. Returns
 * the byte count (excluding NUL) or -1 if no framing is present. Falls back
 * to "raw text from the start" when alen < 2 or the length looks bogus, so a
 * raw "1234" arg (e.g. from the console) also works. */
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
        /* bogus framing — treat the whole buffer as raw text */
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
    /* drop a trailing NUL the frontend appends */
    if (n > 0 && out[n - 1] == 0) out[--n] = 0;
    *off += len;
    return n;
}

void go(char *args, int alen) {
    if (alen <= 0) { BeaconPrintf(CALLBACK_ERROR, "proc_kill: no pid specified"); return; }

    char pidbuf[32];
    int off = 0;
    int n = read_bstr(args, alen, &off, pidbuf, (int)sizeof(pidbuf));
    if (n <= 0) { BeaconPrintf(CALLBACK_ERROR, "proc_kill: no pid specified"); return; }
    /* strip trailing CR/LF whitespace */
    while (n > 0 && (pidbuf[n-1] == '\r' || pidbuf[n-1] == '\n' || pidbuf[n-1] == ' ')) {
        pidbuf[--n] = 0;
    }
    if (n == 0) { BeaconPrintf(CALLBACK_ERROR, "proc_kill: no pid specified"); return; }

    DWORD pid = (DWORD) MSVCRT$atoi(pidbuf);
    if (pid == 0) { BeaconPrintf(CALLBACK_ERROR, "proc_kill: bad pid '%s'", pidbuf); return; }

    HANDLE h = KERNEL32$OpenProcess(0x0001 /*PROCESS_TERMINATE*/, FALSE, pid);
    if (!h || h == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "proc_kill: OpenProcess(%lu) failed (%lu)",
                     pid, KERNEL32$GetLastError());
        return;
    }
    if (!KERNEL32$TerminateProcess(h, 1)) {
        BeaconPrintf(CALLBACK_ERROR, "proc_kill: TerminateProcess(%lu) failed (%lu)",
                     pid, KERNEL32$GetLastError());
        KERNEL32$CloseHandle(h);
        return;
    }
    KERNEL32$CloseHandle(h);

    /* The frontend only checks result.success and toasts; echo the PID so the
     * operator also sees it in the task log/terminal. */
    BeaconPrintf(CALLBACK_OUTPUT, "killed %lu", pid);
}
