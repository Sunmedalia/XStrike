/*
 * Example BOF for ruststrike v1.
 *
 * Calls BeaconPrintf with a literal string (no format specifiers) so it works
 * with the v1 loader's variadic capture. Entry point is `go(char*, int)`.
 *
 * Build (cross-compile on Linux):
 *   x86_64-w64-mingw32-gcc -c examples/hello.c -o examples/hello.x64.o
 */
#include <windows.h>
#include "beacon.h"

void go(char *args, int alen) {
    (void)args;
    (void)alen;
    BeaconPrintf(CALLBACK_OUTPUT, "hello from bof");
}
