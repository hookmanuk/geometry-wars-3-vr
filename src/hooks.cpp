// Device hooks. Stage 1: record what the game draws, so the VR path can be designed from facts.
//
// A frame dump is requested by creating "gw3vr_dump.req" beside the game. The next frame is written
// to gw3vr_dump_<n>.log, one line per device call. Shader bytecode goes to gw3vr_shaders\.
#include "common.h"
#include "stereo.h"
#include "test.h"
#include <unordered_map>

static IDirect3DDevice9* g_dev;
static FILE* g_dump;
static int g_dumpIndex;
static unsigned g_frame;
static unsigned g_draws;

struct ShaderInfo { uint32_t hash; int index; bool usesViewProj; };
static std::unordered_map<void*, ShaderInfo> g_shaders;
static int g_shaderCount;

#define D(...) do { if (g_dump) { fprintf(g_dump, __VA_ARGS__); fputc('\n', g_dump); } } while (0)

// Length in DWORDs of a shader's bytecode, including the end token.
static size_t ShaderLength(const DWORD* code)
{
    const DWORD* p = code + 1;
    bool sm2 = ((code[0] >> 8) & 0xff) >= 2;
    for (;;)
    {
        DWORD t = *p++;
        if (t == 0x0000ffff) break;
        if ((t & 0xffff) == 0xfffe) p += (t >> 16) & 0x7fff;
        else if (sm2) p += (t >> 24) & 0xf;
    }
    return p - code;
}

static void RegisterShader(void* shader, const DWORD* code, const char* kind)
{
    size_t n = ShaderLength(code);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n * 4; i++) h = (h ^ ((const uint8_t*)code)[i]) * 16777619u;
    // Shaders that take the game's view-projection matrix in c0 name it in their constant table.
    bool vp = false;
    for (size_t i = 0; i + 19 <= n * 4; i++)
        if (!memcmp((const char*)code + i, "worldViewProjection", 19)) { vp = true; break; }
    ShaderInfo info{ h, g_shaderCount++, vp };
    g_shaders[shader] = info;
    CreateDirectoryA((g_dir + "gw3vr_shaders").c_str(), nullptr);
    char name[64];
    sprintf(name, "gw3vr_shaders\\%s_%08x.bin", kind, h);
    FILE* f = fopen((g_dir + name).c_str(), "wb");
    if (f) { fwrite(code, 4, n, f); fclose(f); }
}

static uint32_t ShaderHash(void* shader)
{
    auto it = g_shaders.find(shader);
    return it == g_shaders.end() ? 0 : it->second.hash;
}

static void DescribeSurface(IDirect3DSurface9* s, char* out)
{
    if (!s) { strcpy(out, "null"); return; }
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    IDirect3DTexture9* tex = nullptr;
    s->GetContainer(IID_IDirect3DTexture9, (void**)&tex);
    if (tex) tex->Release();
    sprintf(out, "%p %ux%u fmt %d usage %#lx ms %d tex %p", s, d.Width, d.Height, d.Format, d.Usage, d.MultiSampleType, tex);
}

// ---- stereo state ----
// With VR on, every render target the game makes is replaced by one holding both eyes side by
// side, and the backbuffer is replaced by g_final, which is shared with D3D11 for submission.
static bool g_vr;
static unsigned g_gameW, g_gameH;          // the backbuffer size the game asked for
static IDirect3DTexture9* g_final;
static IDirect3DSurface9* g_finalSurf;
static HANDLE g_finalShare;
static IDirect3DSurface9* g_depth;
static IDirect3DSurface9* g_backbuffer;    // the real one, not owned
static IDirect3DSurface9* g_autoDepth;     // the device's own depth buffer, not owned
static IDirect3DQuery9* g_flushQuery;
static std::unordered_map<void*, unsigned> g_eyeTextures; // texture -> the width the game asked for

static bool g_rtStereo;                    // the bound target holds both eyes
static float g_rtScaleX = 1, g_rtScaleY = 1;
static D3DVIEWPORT9 g_viewport;            // the game's viewport, scaled to the bound target
static int g_viewportState;                // 0 = g_viewport is set, 1 = an eye's half is set
static unsigned g_rtWidth;
static bool g_rtFinal;                     // the bound target is the frame itself (g_final)
static bool g_vsUsesViewProj;
static uint32_t g_psHash;
static const uint32_t kFinalPass = 0xa356001c; // colour offset and vignette, drawn to the backbuffer

// ---- originals ----
typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(__stdcall* PresentEx_t)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(__stdcall* ResetEx_t)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
typedef HRESULT(__stdcall* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(__stdcall* CreateRenderTarget_t)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT(__stdcall* StretchRect_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(__stdcall* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(__stdcall* SetDepthStencil_t)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(__stdcall* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(__stdcall* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT(__stdcall* SetViewport_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT(__stdcall* SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(__stdcall* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(__stdcall* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(__stdcall* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(__stdcall* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(__stdcall* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(__stdcall* CreateVertexShader_t)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
typedef HRESULT(__stdcall* SetVertexShader_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT(__stdcall* SetShaderConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(__stdcall* CreatePixelShader_t)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
typedef HRESULT(__stdcall* SetPixelShader_t)(IDirect3DDevice9*, IDirect3DPixelShader9*);

static Present_t o_Present;
static PresentEx_t o_PresentEx;
static Reset_t o_Reset;
static ResetEx_t o_ResetEx;
static CreateTexture_t o_CreateTexture;
static CreateRenderTarget_t o_CreateRenderTarget;
static CreateRenderTarget_t o_CreateDepthStencil;
static StretchRect_t o_StretchRect;
static SetRenderTarget_t o_SetRenderTarget;
static SetDepthStencil_t o_SetDepthStencil;
static Clear_t o_Clear;
static SetTransform_t o_SetTransform;
static SetViewport_t o_SetViewport;
static SetRenderState_t o_SetRenderState;
static SetTexture_t o_SetTexture;
static DrawPrimitive_t o_DrawPrimitive;
static DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
static DrawPrimitiveUP_t o_DrawPrimitiveUP;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
static CreateVertexShader_t o_CreateVertexShader;
static SetVertexShader_t o_SetVertexShader;
static SetShaderConstF_t o_SetVSConstF;
static CreatePixelShader_t o_CreatePixelShader;
static SetPixelShader_t o_SetPixelShader;
static SetShaderConstF_t o_SetPSConstF;

// Where the render thread is, for the stall report below.
static const char* volatile g_stage = "game";

static HANDLE g_renderThread;

// Names the module an address belongs to, as "module+offset".
static void DescribeAddress(DWORD addr, char* out)
{
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
    {
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        sprintf(out, "%s+%lx", name ? name + 1 : path, addr - (DWORD)mod);
    }
    else out[0] = 0;
}

// Writes where the render thread is executing and the code addresses found on its stack.
static void LogRenderThreadStack()
{
    if (!g_renderThread || SuspendThread(g_renderThread) == (DWORD)-1) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    std::string line;
    if (GetThreadContext(g_renderThread, &ctx))
    {
        char text[300];
        DescribeAddress(ctx.Eip, text);
        line = std::string("at ") + text + "; stack:";
        const DWORD* sp = (const DWORD*)ctx.Esp;
        int found = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (int i = 0; i < 4000 && found < 24; i++)
        {
            if (IsBadReadPtr(sp + i, 4)) break;
            DWORD v = sp[i];
            if (!VirtualQuery((LPCVOID)v, &mbi, sizeof mbi) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) || mbi.Type != MEM_IMAGE) continue;
            DescribeAddress(v, text);
            if (!text[0]) continue;
            line += " ";
            line += text;
            found++;
        }
    }
    ResumeThread(g_renderThread);
    Log("  %s", line.c_str());
}

// Logs once when no frame has finished for three seconds, naming the step that is stuck.
static DWORD WINAPI Watchdog(void*)
{
    unsigned last = g_frame;
    int quiet = 0;
    for (;;)
    {
        Sleep(1000);
        if (g_frame != last) { last = g_frame; quiet = 0; continue; }
        if (++quiet == 3) Log("no frame for 3 s: the render thread is in '%s' (frame %u)", g_stage, g_frame);
        if (quiet >= 3 && quiet <= 8) LogRenderThreadStack();
    }
}

static void EnsureViewport()
{
    if (g_viewportState)
    {
        o_SetViewport(g_dev, &g_viewport);
        g_viewportState = 0;
    }
}

static void SetEyeViewport(int eye)
{
    D3DVIEWPORT9 v = g_viewport;
    v.Width = g_viewport.Width / 2;
    v.X = g_viewport.X / 2 + eye * (g_rtWidth / 2);
    o_SetViewport(g_dev, &v);
    g_viewportState = 1;
}

// True when the next draw goes to both eyes with its own matrix for each.
static bool StereoDraw()
{
    return g_vr && g_rtStereo && g_vsUsesViewProj && stereo::HaveMatrix();
}

// Runs a draw once per eye, or once unchanged.
template <typename F> static HRESULT Draw(F draw)
{
    if (!StereoDraw())
    {
        EnsureViewport();
        return draw();
    }
    HRESULT hr = S_OK;
    for (int eye = 0; eye < 2; eye++)
    {
        SetEyeViewport(eye);
        o_SetVSConstF(g_dev, 0, stereo::EyeMatrix(eye, g_rtFinal), 4);
        hr = draw();
    }
    return hr;
}

// Describes the bound colour target: whether it holds both eyes and how the game's sizes map to it.
static void TargetChanged(IDirect3DSurface9* s)
{
    g_rtStereo = false;
    g_rtFinal = s && s == g_finalSurf;
    g_rtScaleX = g_rtScaleY = 1;
    g_viewportState = 0;
    if (!s) return;
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    g_viewport = { 0, 0, d.Width, d.Height, 0, 1 };
    g_rtWidth = d.Width;
    if (!g_vr) return;
    unsigned gameWidth = 0;
    if (s == g_finalSurf) gameWidth = g_gameW;
    else
    {
        IDirect3DTexture9* tex = nullptr;
        s->GetContainer(IID_IDirect3DTexture9, (void**)&tex);
        if (tex)
        {
            tex->Release();
            auto it = g_eyeTextures.find(tex);
            if (it != g_eyeTextures.end()) gameWidth = it->second;
        }
    }
    if (!gameWidth) return;
    g_rtStereo = true;
    g_rtScaleX = (float)d.Width / gameWidth;
    g_rtScaleY = (float)d.Height / ((float)gameWidth * g_gameH / g_gameW);
}

// Creates what VR needs on the device, and again after a reset.
static void SetupDevice()
{
    if (!g_vr) return;
    IDirect3DSurface9* bb = nullptr;
    g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    g_backbuffer = bb;
    if (bb) bb->Release();
    unsigned w = vr::EyeWidth() * 2, h = vr::EyeHeight();
    if (!g_final)
    {
        HRESULT hr = o_CreateTexture(g_dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_final, &g_finalShare);
        Log("eye target %ux%u: %#lx share %p", w, h, hr, g_finalShare);
        if (g_final) g_final->GetSurfaceLevel(0, &g_finalSurf);
        hr = o_CreateDepthStencil(g_dev, w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_depth, nullptr);
        Log("eye depth: %#lx", hr);
        g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_flushQuery);
    }
    IDirect3DSurface9* ds = nullptr;
    g_dev->GetDepthStencilSurface(&ds);
    if (ds && ds != g_depth) g_autoDepth = ds;
    if (ds) ds->Release();
    o_SetDepthStencil(g_dev, g_depth);
    o_SetRenderTarget(g_dev, 0, g_finalSurf);
    TargetChanged(g_finalSurf);
}

// Waits until the GPU has finished everything D3D9 sent, so D3D11 reads a complete image.
static void FlushD3D9()
{
    if (!g_flushQuery) return;
    g_flushQuery->Issue(D3DISSUE_END);
    while (g_flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) SwitchToThread();
}

// End of the game's frame with VR on: submit the eyes and mirror one to the window.
static void VrFrame()
{
    EnsureViewport();
    g_stage = "waiting for the GPU to finish D3D9 work";
    FlushD3D9();
    g_stage = "screenshot";
    test::Screenshot(g_dev, g_finalSurf);
    g_stage = "submitting the eyes (xrEndFrame)";
    vr::EndFrame(g_finalShare);
    g_stage = "mirror and present";
    if (g_backbuffer && g_finalSurf)
    {
        // A 16:9 cut from the middle of the left eye.
        LONG w = vr::EyeWidth(), h = vr::EyeHeight();
        LONG ch = w * 9 / 16 < h ? w * 9 / 16 : h;
        RECT src{ 0, (h - ch) / 2, w, (h - ch) / 2 + ch };
        o_StretchRect(g_dev, g_finalSurf, &src, g_backbuffer, nullptr, D3DTEXF_LINEAR);
    }
}

static void VrNextFrame()
{
    Eye eyes[2];
    g_stage = "waiting for the headset's next frame (xrWaitFrame)";
    bool tracked = vr::BeginFrame(eyes);
    g_stage = "game";
    if (test::RecentreRequested()) stereo::Recentre();
    if (test::SettingsChanged()) stereo::LoadSettings();
    stereo::SetEyes(eyes, tracked);
}

static void FrameBoundary()
{
    if (g_dump)
    {
        D("PRESENT draws %u", g_draws);
        fclose(g_dump);
        g_dump = nullptr;
        Log("frame %u dumped", g_frame);
    }
    g_frame++;
    g_draws = 0;
    std::string req = g_dir + "gw3vr_dump.req";
    if (GetFileAttributesA(req.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(req.c_str());
        char name[64];
        sprintf(name, "gw3vr_dump_%d.log", g_dumpIndex++);
        g_dump = fopen((g_dir + name).c_str(), "w");
        D("FRAME %u", g_frame);
    }
}

static HRESULT __stdcall H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    FrameBoundary();
    if (!g_vr)
    {
        test::Screenshot(d, nullptr);
        return o_Present(d, a, b, w, r);
    }
    VrFrame();
    HRESULT hr = o_Present(d, a, b, w, r);
    VrNextFrame();
    return hr;
}

static HRESULT __stdcall H_PresentEx(IDirect3DDevice9Ex* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f)
{
    FrameBoundary();
    if (!g_vr)
    {
        test::Screenshot(d, nullptr);
        return o_PresentEx(d, a, b, w, r, f);
    }
    VrFrame();
    HRESULT hr = o_PresentEx(d, a, b, w, r, f);
    VrNextFrame();
    return hr;
}

static HRESULT __stdcall H_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp)
{
    Log("Reset %ux%u windowed %d", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed);
    if (g_vr) VrPresentParameters(pp);
    HRESULT hr = o_Reset(d, pp);
    Log("  -> %#lx", hr);
    SetupDevice();
    return hr;
}

static HRESULT __stdcall H_ResetEx(IDirect3DDevice9Ex* d, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m)
{
    Log("ResetEx %ux%u windowed %d", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed);
    if (g_vr) { VrPresentParameters(pp); m = nullptr; }
    HRESULT hr = o_ResetEx(d, pp, m);
    Log("  -> %#lx", hr);
    SetupDevice();
    return hr;
}

static HRESULT __stdcall H_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* share)
{
    UINT gameWidth = w;
    bool eyes = g_vr && (usage & D3DUSAGE_RENDERTARGET) && g_gameW;
    if (eyes)
    {
        // The same fraction of the frame as the game asked for, for two eyes side by side.
        h = (UINT)((uint64_t)h * vr::EyeHeight() / g_gameH);
        w = (UINT)((uint64_t)w * vr::EyeWidth() * 2 / g_gameW);
    }
    HRESULT hr = o_CreateTexture(d, w, h, levels, usage, fmt, pool, out, share);
    if (eyes && SUCCEEDED(hr) && out && *out) g_eyeTextures[*out] = gameWidth;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        Log("CreateTexture %ux%u levels %u usage %#lx fmt %d pool %d -> %p (%#lx)", w, h, levels, usage, fmt, pool, out ? *out : nullptr, hr);
    return hr;
}

static HRESULT __stdcall H_CreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lock, IDirect3DSurface9** out, HANDLE* share)
{
    HRESULT hr = o_CreateRenderTarget(d, w, h, fmt, ms, q, lock, out, share);
    Log("CreateRenderTarget %ux%u fmt %d ms %d -> %p (%#lx)", w, h, fmt, ms, out ? *out : nullptr, hr);
    return hr;
}

static HRESULT __stdcall H_CreateDepthStencil(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* share)
{
    HRESULT hr = o_CreateDepthStencil(d, w, h, fmt, ms, q, discard, out, share);
    Log("CreateDepthStencilSurface %ux%u fmt %d ms %d -> %p (%#lx)", w, h, fmt, ms, out ? *out : nullptr, hr);
    return hr;
}

static HRESULT __stdcall H_StretchRect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    if (g_dump)
    {
        char a[160], b[160];
        DescribeSurface(src, a);
        DescribeSurface(dst, b);
        D("StretchRect [%s] -> [%s] filter %d", a, b, f);
    }
    if (g_vr && dst == g_backbuffer) dst = g_finalSurf;
    if (g_vr && src == g_backbuffer) src = g_finalSurf;
    return o_StretchRect(d, src, sr, dst, dr, f);
}

static HRESULT __stdcall H_SetRenderTarget(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* s)
{
    if (g_dump)
    {
        char a[160];
        DescribeSurface(s, a);
        D("SetRenderTarget %lu [%s]", idx, a);
    }
    if (g_vr && idx == 0 && s == g_backbuffer) s = g_finalSurf;
    HRESULT hr = o_SetRenderTarget(d, idx, s);
    if (idx == 0) TargetChanged(s);
    return hr;
}

static HRESULT __stdcall H_SetDepthStencil(IDirect3DDevice9* d, IDirect3DSurface9* s)
{
    if (g_dump)
    {
        char a[160];
        DescribeSurface(s, a);
        D("SetDepthStencil [%s]", a);
    }
    if (g_vr && s == g_autoDepth) s = g_depth;
    return o_SetDepthStencil(d, s);
}

static HRESULT __stdcall H_Clear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD st)
{
    D("Clear rects %lu flags %#lx color %08lx z %g stencil %lu", n, flags, c, z, st);
    EnsureViewport();
    return o_Clear(d, n, r, flags, c, z, st);
}

static HRESULT __stdcall H_SetTransform(IDirect3DDevice9* d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m)
{
    if (g_dump && m)
    {
        const float* f = &m->_11;
        D("SetTransform %d [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", t, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    return o_SetTransform(d, t, m);
}

static HRESULT __stdcall H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v)
{
    if (v) D("SetViewport %lu,%lu %lux%lu z %g..%g", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
    if (!v) return o_SetViewport(d, v);
    g_viewport = *v;
    if (g_rtStereo)
    {
        g_viewport.X = (DWORD)(v->X * g_rtScaleX);
        g_viewport.Y = (DWORD)(v->Y * g_rtScaleY);
        g_viewport.Width = (DWORD)(v->Width * g_rtScaleX);
        g_viewport.Height = (DWORD)(v->Height * g_rtScaleY);
    }
    g_viewportState = 0;
    return o_SetViewport(d, &g_viewport);
}

static HRESULT __stdcall H_SetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v)
{
    D("RS %d = %lu", s, v);
    return o_SetRenderState(d, s, v);
}

static HRESULT __stdcall H_SetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t)
{
    D("SetTexture %lu %p", stage, t);
    return o_SetTexture(d, stage, t);
}

static HRESULT __stdcall H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
    g_draws++;
    D("DrawPrimitive type %d start %u prims %u", t, start, count);
    return Draw([&] { return o_DrawPrimitive(d, t, start, count); });
}

static HRESULT __stdcall H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minV, UINT numV, UINT start, UINT count)
{
    g_draws++;
    D("DrawIndexedPrimitive type %d base %d verts %u start %u prims %u", t, base, numV, start, count);
    return Draw([&] { return o_DrawIndexedPrimitive(d, t, base, minV, numV, start, count); });
}

static HRESULT __stdcall H_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride)
{
    g_draws++;
    if (g_dump)
    {
        const float* f = (const float*)data;
        D("DrawPrimitiveUP type %d prims %u stride %u v0 [%g %g %g %g]", t, count, stride, f[0], f[1], f[2], stride >= 16 ? f[3] : 0.f);
    }
    return Draw([&] { return o_DrawPrimitiveUP(d, t, count, data, stride); });
}

static HRESULT __stdcall H_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minV, UINT numV, UINT count, const void* idx, D3DFORMAT fmt, const void* data, UINT stride)
{
    g_draws++;
    D("DrawIndexedPrimitiveUP type %d verts %u prims %u stride %u", t, numV, count, stride);
    return Draw([&] { return o_DrawIndexedPrimitiveUP(d, t, minV, numV, count, idx, fmt, data, stride); });
}

static HRESULT __stdcall H_CreateVertexShader(IDirect3DDevice9* d, const DWORD* code, IDirect3DVertexShader9** out)
{
    HRESULT hr = o_CreateVertexShader(d, code, out);
    if (SUCCEEDED(hr) && out && *out) RegisterShader(*out, code, "vs");
    return hr;
}

static HRESULT __stdcall H_CreatePixelShader(IDirect3DDevice9* d, const DWORD* code, IDirect3DPixelShader9** out)
{
    HRESULT hr = o_CreatePixelShader(d, code, out);
    if (SUCCEEDED(hr) && out && *out) RegisterShader(*out, code, "ps");
    return hr;
}

static HRESULT __stdcall H_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s)
{
    D("SetVertexShader vs_%08x", ShaderHash(s));
    auto it = g_shaders.find(s);
    g_vsUsesViewProj = it != g_shaders.end() && it->second.usesViewProj;
    return o_SetVertexShader(d, s);
}

static HRESULT __stdcall H_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s)
{
    D("SetPixelShader ps_%08x", ShaderHash(s));
    g_psHash = ShaderHash(s);
    return o_SetPixelShader(d, s);
}

static void DumpConsts(const char* what, UINT reg, const float* f, UINT count)
{
    if (!g_dump) return;
    fprintf(g_dump, "%s c%u x%u:", what, reg, count);
    for (UINT i = 0; i < count; i++) fprintf(g_dump, " [%g %g %g %g]", f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]);
    fputc('\n', g_dump);
}

static HRESULT __stdcall H_SetVSConstF(IDirect3DDevice9* d, UINT reg, const float* f, UINT count)
{
    DumpConsts("VSConst", reg, f, count);
    if (reg == 0 && count >= 4) stereo::SetGameMatrix(f);
    return o_SetVSConstF(d, reg, f, count);
}

static HRESULT __stdcall H_SetPSConstF(IDirect3DDevice9* d, UINT reg, const float* f, UINT count)
{
    DumpConsts("PSConst", reg, f, count);
    if (g_vr && g_psHash == kFinalPass && reg == 4 && count == 1)
    {
        // The final pass darkens the corners of the screen around a point that would sit
        // between the two eyes. Its amount is c4; zero switches the vignette off.
        const float zero[4] = { 0, 0, 0, f[3] };
        return o_SetPSConstF(d, reg, zero, count);
    }
    return o_SetPSConstF(d, reg, f, count);
}

void VrPresentParameters(D3DPRESENT_PARAMETERS* pp)
{
    g_gameW = pp->BackBufferWidth;
    g_gameH = pp->BackBufferHeight;
    // The window is only a mirror: windowed, and never waiting for the monitor.
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    int mw = IniInt("mirror_width", 0), mh = IniInt("mirror_height", 0);
    if (mw > 0 && mh > 0) { pp->BackBufferWidth = mw; pp->BackBufferHeight = mh; }
}

void InstallDeviceHooks(IDirect3DDevice9* dev)
{
    g_dev = dev;
    g_vr = vr::Active();
    if (g_vr) stereo::LoadSettings();
    test::Install();
    void** vt = *(void***)dev;
    IDirect3DDevice9Ex* ex = nullptr;
    dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex);
    Log("device %p (Ex %p)", dev, ex);
#define HOOK(idx, name) HookVt(vt, idx, (void*)H_##name, (void**)&o_##name)
    HOOK(16, Reset);
    HOOK(17, Present);
    HOOK(23, CreateTexture);
    HOOK(28, CreateRenderTarget);
    HOOK(29, CreateDepthStencil);
    HOOK(34, StretchRect);
    HOOK(37, SetRenderTarget);
    HOOK(39, SetDepthStencil);
    HOOK(43, Clear);
    HOOK(44, SetTransform);
    HOOK(47, SetViewport);
    HOOK(57, SetRenderState);
    HOOK(65, SetTexture);
    HOOK(81, DrawPrimitive);
    HOOK(82, DrawIndexedPrimitive);
    HOOK(83, DrawPrimitiveUP);
    HOOK(84, DrawIndexedPrimitiveUP);
    HOOK(91, CreateVertexShader);
    HOOK(92, SetVertexShader);
    HOOK(94, SetVSConstF);
    HOOK(106, CreatePixelShader);
    HOOK(107, SetPixelShader);
    HOOK(109, SetPSConstF);
    if (ex)
    {
        void** vx = *(void***)ex;
        HookVt(vx, 121, (void*)H_PresentEx, (void**)&o_PresentEx);
        HookVt(vx, 132, (void*)H_ResetEx, (void**)&o_ResetEx);
        ex->Release();
    }
#undef HOOK
    SetupDevice();
    test::PlaceWindow(dev);
    static bool watchdog;
    if (!watchdog) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_renderThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!watchdog) { watchdog = true; CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)); }
}
