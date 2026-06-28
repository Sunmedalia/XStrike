/*
 * file_list.c - Directory listing BOF plugin.
 *
 * Lists files and directories with name, size, and timestamp.
 * If no path argument provided, lists the current working directory.
 *
 * Output is converted from system codepage (ACP/GBK) to UTF-8.
 *
 * Output format (tab-separated, one entry per line):
 *   First line:  CWD:<path>
 *   Data lines:  TYPE\tNAME\tSIZE\tEPOCH_SECONDS
 *     TYPE = D (directory) or F (file)
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c file_list.c -o file_list.o
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$FindNextFileA(HANDLE, LPWIN32_FIND_DATAA);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$FindClose(HANDLE);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetCurrentDirectoryA(DWORD, LPSTR);
DECLSPEC_IMPORT int    WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);
DECLSPEC_IMPORT int    WINAPI KERNEL32$WideCharToMultiByte(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);

#define CP_ACP_VAL   0
#define CP_UTF8_VAL  65001

static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}

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

static int bof_uitoa(unsigned int val, char* buf) {
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int len = i;
    for (int j = 0; j < len; j++) buf[j] = tmp[len - 1 - j];
    return len;
}

static int bof_lltoa(long long val, char* buf) {
    if (val <= 0) { buf[0] = '0'; return 1; }
    char tmp[20];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
    int len = i;
    for (int j = 0; j < len; j++) buf[j] = tmp[len - 1 - j];
    return len;
}

static long long ft_to_epoch(FILETIME ft) {
    long long val = ((long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
    return (val / 10000000LL) - 11644473600LL;
}

/*
 * Convert ACP string to UTF-8 in-place into out_buf.
 * Returns the number of UTF-8 bytes written, or 0 on failure.
 */
static int acp_to_utf8(const char* acp, int acp_len, char* out_buf, int out_cap) {
    WCHAR wbuf[512];
    int wlen = KERNEL32$MultiByteToWideChar(CP_ACP_VAL, 0, acp, acp_len, wbuf, 512);
    if (wlen <= 0) return 0;
    int u8len = KERNEL32$WideCharToMultiByte(CP_UTF8_VAL, 0, wbuf, wlen, out_buf, out_cap, NULL, NULL);
    return u8len > 0 ? u8len : 0;
}

void go(char* args, int alen)
{
    char path[512];
    char search[520];
    char line[1024];
    char u8name[1024];
    int pathlen = 0;

    bof_memset(path, 0, sizeof(path));

    /* Parse optional path from args */
    if (args != NULL && alen >= 2) {
        datap parser;
        int ulen = 0;
        BeaconDataParse(&parser, args, alen);
        char* upath = BeaconDataExtract(&parser, &ulen);
        if (upath != NULL && ulen > 0) {
            if (ulen >= (int)sizeof(path)) ulen = sizeof(path) - 1;
            bof_memcpy(path, upath, ulen);
            path[ulen] = '\0';
            pathlen = ulen;
            while (pathlen > 0 && path[pathlen - 1] == '\0') pathlen--;
        }
    }

    /* If no path, use current directory */
    if (pathlen == 0 || path[0] == '\0') {
        pathlen = KERNEL32$GetCurrentDirectoryA(sizeof(path), path);
        if (pathlen == 0) {
            BeaconOutput(CALLBACK_ERROR, "GetCurrentDirectory failed\n", 27);
            return;
        }
    }

    /* Ensure trailing backslash */
    if (pathlen > 0 && path[pathlen - 1] != '\\') {
        path[pathlen++] = '\\';
        path[pathlen] = '\0';
    }

    /* Output CWD line (convert path to UTF-8) */
    int pos = 0;
    bof_memcpy(line, "CWD:", 4); pos = 4;
    int u8pathlen = acp_to_utf8(path, pathlen, line + pos, sizeof(line) - pos - 2);
    if (u8pathlen > 0) {
        pos += u8pathlen;
    } else {
        bof_memcpy(line + pos, path, pathlen); pos += pathlen;
    }
    line[pos++] = '\n';
    BeaconOutput(CALLBACK_OUTPUT, line, pos);

    /* Build search pattern: path\* */
    bof_memcpy(search, path, pathlen);
    search[pathlen] = '*';
    search[pathlen + 1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE hFind = KERNEL32$FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        BeaconOutput(CALLBACK_ERROR, "FindFirstFile failed\n", 21);
        return;
    }

    do {
        pos = 0;

        /* Type */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            line[pos++] = 'D';
        } else {
            line[pos++] = 'F';
        }
        line[pos++] = '\t';

        /* Name (ACP -> UTF-8) */
        int nlen = bof_strlen(fd.cFileName);
        int u8len = acp_to_utf8(fd.cFileName, nlen, u8name, sizeof(u8name));
        if (u8len > 0) {
            bof_memcpy(line + pos, u8name, u8len);
            pos += u8len;
        } else {
            bof_memcpy(line + pos, fd.cFileName, nlen);
            pos += nlen;
        }
        line[pos++] = '\t';

        /* Size */
        unsigned long long fsize = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        pos += bof_lltoa((long long)fsize, line + pos);
        line[pos++] = '\t';

        /* Timestamp (epoch seconds from last write time) */
        long long epoch = ft_to_epoch(fd.ftLastWriteTime);
        pos += bof_lltoa(epoch, line + pos);
        line[pos++] = '\n';

        BeaconOutput(CALLBACK_OUTPUT, line, pos);
    } while (KERNEL32$FindNextFileA(hFind, &fd));

    KERNEL32$FindClose(hFind);
}
