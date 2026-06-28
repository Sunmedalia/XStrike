/*
 * proc_kill.c - Process kill BOF plugin.
 *
 * Terminates a process by PID.
 *
 * Args: PID as string (e.g. "1234")
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c proc_kill.c -o proc_kill.o
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$OpenProcess(DWORD, BOOL, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$TerminateProcess(HANDLE, UINT);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);

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

static unsigned int bof_atoi(const char* s) {
    unsigned int result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result;
}

void go(char* args, int alen)
{
    char* pid_str = NULL;
    int pid_str_len = 0;

    if (args != NULL && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        pid_str = BeaconDataExtract(&parser, &pid_str_len);
    }

    if (pid_str == NULL || pid_str_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No PID provided\n", 16);
        return;
    }

    /* Null-terminate */
    char buf[32];
    if (pid_str_len >= (int)sizeof(buf)) pid_str_len = sizeof(buf) - 1;
    bof_memcpy(buf, pid_str, pid_str_len);
    buf[pid_str_len] = '\0';

    DWORD pid = (DWORD)bof_atoi(buf);
    if (pid == 0) {
        BeaconOutput(CALLBACK_ERROR, "Invalid PID\n", 12);
        return;
    }

    HANDLE hProc = KERNEL32$OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProc == NULL) {
        BeaconOutput(CALLBACK_ERROR, "OpenProcess failed (access denied?)\n", 36);
        return;
    }

    if (!KERNEL32$TerminateProcess(hProc, 1)) {
        KERNEL32$CloseHandle(hProc);
        BeaconOutput(CALLBACK_ERROR, "TerminateProcess failed\n", 23);
        return;
    }

    KERNEL32$CloseHandle(hProc);

    /* Build success message */
    char msg[64];
    int pos = 0;
    const char* p1 = "Killed PID ";
    int p1len = bof_strlen(p1);
    bof_memcpy(msg + pos, p1, p1len); pos += p1len;
    bof_memcpy(msg + pos, buf, pid_str_len); pos += pid_str_len;
    msg[pos++] = '\n';
    BeaconOutput(CALLBACK_OUTPUT, msg, pos);
}
