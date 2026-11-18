// d3d9.dll proxy: forwards the exports to the system d3d9 and hooks device creation.
#include "common.h"
#include "vr.h"
#include "test.h"

std::string g_dir;
static FILE* g_log;
static CRITICAL_SECTION g_logLock;
static HMODULE g_real;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

void HookVt(void** vt, int idx, void* fn, void** orig)
{
    if (vt[idx] == fn) return;
    if (*orig && *orig != vt[idx]) Log("HookVt: slot %d has a second original %p (first %p)", idx, vt[idx], *orig);
    *orig = vt[idx];
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

std::string IniString(const char* key)
{
    std::string path = g_dir + "gw3vr.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char line[256];
    std::string out;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f))
    {
        if (!strncmp(line, key, klen) && line[klen] == '=')
        {
            out = line + klen + 1;
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
            break;
        }
    }
    fclose(f);
    return out;
}

int IniInt(const char* key, int def)
{
    std::string v = IniString(key);
    return v.empty() ? def : atoi(v.c_str());
}

float IniFloat(const char* key, float def)
{
    std::string v = IniString(key);
    return v.empty() ? def : (float)atof(v.c_str());
}

static FARPROC Real(const char* name)
{
    if (!g_real)
    {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        g_real = LoadLibraryA((std::string(sys) + "\\d3d9.dll").c_str());
        Log("system d3d9 loaded at %p", g_real);
    }
    return GetProcAddress(g_real, name);
}

static void LogPP(const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) return;
    Log("  backbuffer %ux%u fmt %d count %u ms %d swap %d windowed %d autoDS %d dsfmt %d flags %#x hz %u interval %#x hwnd %p",
        pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType,
        pp->SwapEffect, pp->Windowed, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags,
        pp->FullScreen_RefreshRateInHz, pp->PresentationInterval, pp->hDeviceWindow);
}

typedef HRESULT(__stdcall* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(__stdcall* CreateDeviceEx_t)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
static CreateDevice_t o_CreateDevice;
static CreateDeviceEx_t o_CreateDeviceEx;

static HRESULT __stdcall H_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    Log("CreateDevice adapter %u type %d flags %#x", adapter, type, flags);
    LogPP(pp);
    HRESULT hr = o_CreateDevice(self, adapter, type, wnd, flags, pp, out);
    Log("  -> %#lx", hr);
    if (SUCCEEDED(hr) && out && *out) InstallDeviceHooks(*out);
    return hr;
}

static HRESULT __stdcall H_CreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    Log("CreateDeviceEx adapter %u type %d flags %#x", adapter, type, flags);
    LogPP(pp);
    if (IniInt("vr", 1)) test::KeepActive(wnd ? wnd : pp->hDeviceWindow);
    if (vr::Init()) { VrPresentParameters(pp); mode = nullptr; }
    else Log("VR is off: the game runs unchanged");
    HRESULT hr = o_CreateDeviceEx(self, adapter, type, wnd, flags, pp, mode, out);
    Log("  -> %#lx", hr);
    if (SUCCEEDED(hr) && out && *out) InstallDeviceHooks(*out);
    return hr;
}

extern "C" {

IDirect3D9* WINAPI P_Direct3DCreate9(UINT sdk)
{
    auto fn = (IDirect3D9 * (WINAPI*)(UINT))Real("Direct3DCreate9");
    IDirect3D9* d3d = fn(sdk);
    Log("Direct3DCreate9(%u) -> %p", sdk, d3d);
    if (d3d) HookVt(*(void***)d3d, 16, (void*)H_CreateDevice, (void**)&o_CreateDevice);
    return d3d;
}

HRESULT WINAPI P_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    auto fn = (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))Real("Direct3DCreate9Ex");
    HRESULT hr = fn(sdk, out);
    Log("Direct3DCreate9Ex(%u) -> %#lx", sdk, hr);
    if (SUCCEEDED(hr) && out && *out)
    {
        HookVt(*(void***)*out, 16, (void*)H_CreateDevice, (void**)&o_CreateDevice);
        HookVt(*(void***)*out, 20, (void*)H_CreateDeviceEx, (void**)&o_CreateDeviceEx);
    }
    return hr;
}

int WINAPI P_D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return ((int(WINAPI*)(D3DCOLOR, LPCWSTR))Real("D3DPERF_BeginEvent"))(c, n); }
int WINAPI P_D3DPERF_EndEvent() { return ((int(WINAPI*)())Real("D3DPERF_EndEvent"))(); }
void WINAPI P_D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) { ((void(WINAPI*)(D3DCOLOR, LPCWSTR))Real("D3DPERF_SetMarker"))(c, n); }
void WINAPI P_D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) { ((void(WINAPI*)(D3DCOLOR, LPCWSTR))Real("D3DPERF_SetRegion"))(c, n); }
BOOL WINAPI P_D3DPERF_QueryRepeatFrame() { return ((BOOL(WINAPI*)())Real("D3DPERF_QueryRepeatFrame"))(); }
void WINAPI P_D3DPERF_SetOptions(DWORD o) { ((void(WINAPI*)(DWORD))Real("D3DPERF_SetOptions"))(o); }
DWORD WINAPI P_D3DPERF_GetStatus() { return ((DWORD(WINAPI*)())Real("D3DPERF_GetStatus"))(); }

}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, MAX_PATH);
        g_dir = path;
        g_dir.resize(g_dir.find_last_of('\\') + 1);
        g_log = fopen((g_dir + "gw3vr.log").c_str(), "w");
        Log("gw3vr %s loaded from %s", GW3VR_VERSION, path);
    }
    return TRUE;
}
