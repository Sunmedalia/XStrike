/*
 * test_messagebox.c - Simple MessageBox shellcode for testing
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -c test_messagebox.c -o test_messagebox.o
 *   x86_64-w64-mingw32-objcopy -O binary -j .text test_messagebox.o test_messagebox.bin
 *   xxd -p test_messagebox.bin | tr -d '\n' > test_messagebox.hex
 */

#include <windows.h>

typedef int (WINAPI *pMessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);

void shellcode_entry() {
    HMODULE hUser32 = LoadLibraryA("user32.dll");
    if (!hUser32) return;

    pMessageBoxA MessageBoxA_ptr = (pMessageBoxA)GetProcAddress(hUser32, "MessageBoxA");
    if (!MessageBoxA_ptr) return;

    MessageBoxA_ptr(NULL, "Shellcode executed successfully!", "Test", MB_OK);
}
