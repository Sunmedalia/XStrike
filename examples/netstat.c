/*
 * netstat BOF for RustStrike — list TCP/UDP network connections (component format).
 *
 *   args: none
 *
 * Drives NetConnections.vue. Enumerates the IPv4 TCP and UDP owner tables via
 * IPHLPAPI's GetExtendedTcpTable / GetExtendedUdpTable (no shell-out to
 * netstat.exe — pure API, quieter on the target). Prints one line per
 * endpoint, TAB-separated:
 *
 *   PROTO   LOCAL           REMOTE          PID     STATE
 *   TCP     0.0.0.0:135     0.0.0.0:0       912     LISTEN
 *   TCP     10.0.0.5:49200  34.160.111.145:443  4212  ESTABLISHED
 *   UDP     0.0.0.0:500     *:*             912
 *
 * The frontend splits on \t into (proto, local, remote, pid, state). UDP rows
 * have an empty state and remote "*:*" (a UDP socket has no peer).
 *
 * Port fields in the MIB tables are DWORDs holding the 16-bit port in network
 * (big-endian) byte order in the lower 16 bits; ntohs_() flips it. Addresses
 * are already a.b.c.d when read byte-wise (network order).
 *
 * Stack frame stays small (< 4 KiB) — the table and the output buffer are
 * heap-allocated via GetProcessHeap/HeapAlloc, so __chkstk never fires.
 *
 * Build (mingw):
 *   gcc -c examples/netstat.c -o examples/netstat.x64.o
 */
#include <windows.h>
#include "beacon.h"

/* ---- BOF Imports ---- */
DECLSPEC_IMPORT DWORD   WINAPI IPHLPAPI$GetExtendedTcpTable(void *, DWORD *, BOOL, ULONG, int, ULONG);
DECLSPEC_IMPORT DWORD   WINAPI IPHLPAPI$GetExtendedUdpTable(void *, DWORD *, BOOL, ULONG, int, ULONG);
DECLSPEC_IMPORT HANDLE  WINAPI KERNEL32$GetProcessHeap(void);
DECLSPEC_IMPORT LPVOID  WINAPI KERNEL32$HeapAlloc(HANDLE, DWORD, SIZE_T);
DECLSPEC_IMPORT WINBOOL WINAPI KERNEL32$HeapFree(HANDLE, DWORD, LPVOID);
DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetLastError(VOID);
DECLSPEC_IMPORT int __cdecl MSVCRT$_snprintf(char *, size_t, const char *, ...);

#define AF_INET_                 2
#define TCP_TABLE_OWNER_PID_ALL_ 5
#define UDP_TABLE_OWNER_PID_     2
#define ERROR_INSUFFICIENT_BUFFER_ 122
#define NO_ERROR_                0

/* MIB_TCPROW_OWNER_PID — one TCP endpoint with owning PID (no module info). */
typedef struct {
    DWORD dwState;
    DWORD dwLocalAddr;
    DWORD dwLocalPort;
    DWORD dwRemoteAddr;
    DWORD dwRemotePort;
    DWORD dwOwningPid;
} MIB_TCPROW_OWNER_PID_;

typedef struct {
    DWORD dwNumEntries;
    MIB_TCPROW_OWNER_PID_ table[1];
} MIB_TCPTABLE_OWNER_PID_;

/* MIB_UDPROW_OWNER_PID — one UDP endpoint with owning PID. */
typedef struct {
    DWORD dwLocalAddr;
    DWORD dwLocalPort;
    DWORD dwOwningPid;
} MIB_UDPROW_OWNER_PID_;

typedef struct {
    DWORD dwNumEntries;
    MIB_UDPROW_OWNER_PID_ table[1];
} MIB_UDPTABLE_OWNER_PID_;

/* The 16-bit port lives in the lower 16 bits of the DWORD, in network
 * (big-endian) byte order — flip it to host order. */
static unsigned short ntohs_(unsigned long v) {
    unsigned short lo = (unsigned short)(v & 0xFFFFu);
    return (unsigned short)(((lo & 0x00FFu) << 8) | ((lo >> 8) & 0x00FFu));
}

static void ip_str(char *buf, DWORD addr) {
    unsigned char *a = (unsigned char *)&addr;
    MSVCRT$_snprintf(buf, 24, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

static const char *tcp_state(DWORD s) {
    switch (s) {
        case 2:  return "LISTEN";
        case 3:  return "SYN_SENT";
        case 4:  return "SYN_RCVD";
        case 5:  return "ESTABLISHED";
        case 6:  return "FIN_WAIT1";
        case 7:  return "FIN_WAIT2";
        case 8:  return "CLOSE_WAIT";
        case 9:  return "CLOSING";
        case 10: return "LAST_ACK";
        case 11: return "TIME_WAIT";
        case 12: return "DELETE_TCB";
        default: return "UNKNOWN";
    }
}

#define OUT_SIZE 262144   /* 256 KiB — plenty for a few thousand endpoints */

void go(char *args, int alen) {
    (void)args; (void)alen;   /* netstat takes no args */

    HANDLE heap = KERNEL32$GetProcessHeap();
    char *out = (char *)KERNEL32$HeapAlloc(heap, 0, OUT_SIZE);
    if (!out) { BeaconPrintf(CALLBACK_ERROR, "netstat: out of memory"); return; }
    int total = 0;

    char lip[24], rip[24], line[128];

    /* ---- TCP endpoints ---- */
    DWORD tcpSize = 0;
    DWORD rc = IPHLPAPI$GetExtendedTcpTable(NULL, &tcpSize, FALSE, AF_INET_,
                                            TCP_TABLE_OWNER_PID_ALL_, 0);
    if ((rc == ERROR_INSUFFICIENT_BUFFER_ || rc == NO_ERROR_) && tcpSize > 0) {
        void *tbl = KERNEL32$HeapAlloc(heap, 0, tcpSize);
        if (tbl) {
            if (IPHLPAPI$GetExtendedTcpTable(tbl, &tcpSize, FALSE, AF_INET_,
                                             TCP_TABLE_OWNER_PID_ALL_, 0) == NO_ERROR_) {
                MIB_TCPTABLE_OWNER_PID_ *t = (MIB_TCPTABLE_OWNER_PID_ *)tbl;
                for (DWORD i = 0; i < t->dwNumEntries; i++) {
                    MIB_TCPROW_OWNER_PID_ *r = &t->table[i];
                    ip_str(lip, r->dwLocalAddr);
                    ip_str(rip, r->dwRemoteAddr);
                    unsigned short lp = ntohs_(r->dwLocalPort);
                    unsigned short rp = ntohs_(r->dwRemotePort);
                    int n = MSVCRT$_snprintf(line, sizeof(line),
                        "TCP\t%s:%u\t%s:%u\t%lu\t%s\r\n",
                        lip, lp, rip, rp, r->dwOwningPid, tcp_state(r->dwState));
                    if (n < 0) n = 0;
                    if (total + n >= OUT_SIZE - 1) break;   /* stop at cap */
                    for (int k = 0; k < n; k++) out[total + k] = line[k];
                    total += n;
                }
            }
            KERNEL32$HeapFree(heap, 0, tbl);
        }
    }

    /* ---- UDP endpoints ---- */
    DWORD udpSize = 0;
    rc = IPHLPAPI$GetExtendedUdpTable(NULL, &udpSize, FALSE, AF_INET_,
                                      UDP_TABLE_OWNER_PID_, 0);
    if ((rc == ERROR_INSUFFICIENT_BUFFER_ || rc == NO_ERROR_) && udpSize > 0) {
        void *tbl = KERNEL32$HeapAlloc(heap, 0, udpSize);
        if (tbl) {
            if (IPHLPAPI$GetExtendedUdpTable(tbl, &udpSize, FALSE, AF_INET_,
                                             UDP_TABLE_OWNER_PID_, 0) == NO_ERROR_) {
                MIB_UDPTABLE_OWNER_PID_ *t = (MIB_UDPTABLE_OWNER_PID_ *)tbl;
                for (DWORD i = 0; i < t->dwNumEntries; i++) {
                    MIB_UDPROW_OWNER_PID_ *r = &t->table[i];
                    ip_str(lip, r->dwLocalAddr);
                    unsigned short lp = ntohs_(r->dwLocalPort);
                    int n = MSVCRT$_snprintf(line, sizeof(line),
                        "UDP\t%s:%u\t*:*\t%lu\t\r\n",
                        lip, lp, r->dwOwningPid);
                    if (n < 0) n = 0;
                    if (total + n >= OUT_SIZE - 1) break;
                    for (int k = 0; k < n; k++) out[total + k] = line[k];
                    total += n;
                }
            }
            KERNEL32$HeapFree(heap, 0, tbl);
        }
    }

    if (total > 0) {
        BeaconOutput(CALLBACK_OUTPUT, out, total);
    } else {
        BeaconPrintf(CALLBACK_OUTPUT, "(no network endpoints)");
    }
    KERNEL32$HeapFree(heap, 0, out);
}
