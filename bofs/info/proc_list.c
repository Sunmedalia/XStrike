/*
 * proc_list.c - Process listing BOF plugin.
 *
 * Enumerates running processes using CreateToolhelp32Snapshot.
 * No arguments required.
 *
 * Output is converted from system codepage (ACP/GBK) to UTF-8.
 *
 * Output format (tab-separated, one entry per line):
 *   NAME\tPID\tPPID\tTHREADS\n
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c proc_list.c -o proc_list.o
 */

#include <windows.h>
#include <tlhelp32.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateToolhelp32Snapshot(DWORD, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$Process32First(HANDLE, LPPROCESSENTRY32);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$Process32Next(HANDLE, LPPROCESSENTRY32);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT int    WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);
DECLSPEC_IMPORT int    WINAPI KERNEL32$WideCharToMultiByte(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);

#define CP_ACP_VAL   0
#define CP_UTF8_VAL  65001

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

static int bof_uitoa(unsigned int val, char* buf) {
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int len = i;
    for (int j = 0; j < len; j++) buf[j] = tmp[len - 1 - j];
    return len;
}

static int acp_to_utf8(const char* acp, int acp_len, char* out_buf, int out_cap) {
    WCHAR wbuf[512];
    int wlen = KERNEL32$MultiByteToWideChar(CP_ACP_VAL, 0, acp, acp_len, wbuf, 512);
    if (wlen <= 0) return 0;
    int u8len = KERNEL32$WideCharToMultiByte(CP_UTF8_VAL, 0, wbuf, wlen, out_buf, out_cap, NULL, NULL);
    return u8len > 0 ? u8len : 0;
}

void go(char* args, int alen)
{
    char line[512];
    char u8name[512];
    int pos;

    HANDLE hSnap = KERNEL32$CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        BeaconOutput(CALLBACK_ERROR, "CreateToolhelp32Snapshot failed\n", 31);
        return;
    }

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (!KERNEL32$Process32First(hSnap, &pe)) {
        KERNEL32$CloseHandle(hSnap);
        BeaconOutput(CALLBACK_ERROR, "Process32First failed\n", 22);
        return;
    }

    do {
        pos = 0;

        /* Name (ACP -> UTF-8) */
        int nlen = bof_strlen(pe.szExeFile);
        int u8len = acp_to_utf8(pe.szExeFile, nlen, u8name, sizeof(u8name));
        if (u8len > 0) {
            bof_memcpy(line + pos, u8name, u8len);
            pos += u8len;
        } else {
            bof_memcpy(line + pos, pe.szExeFile, nlen);
            pos += nlen;
        }
        line[pos++] = '\t';

        /* PID */
        pos += bof_uitoa(pe.th32ProcessID, line + pos);
        line[pos++] = '\t';

        /* PPID */
        pos += bof_uitoa(pe.th32ParentProcessID, line + pos);
        line[pos++] = '\t';

        /* Threads */
        pos += bof_uitoa(pe.cntThreads, line + pos);
        line[pos++] = '\n';

        BeaconOutput(CALLBACK_OUTPUT, line, pos);
    } while (KERNEL32$Process32Next(hSnap, &pe));

    KERNEL32$CloseHandle(hSnap);
}
