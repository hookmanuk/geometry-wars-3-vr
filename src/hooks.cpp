// Device hooks. Stage 1: record what the game draws, so the VR path can be designed from facts.
//
// A frame dump is requested by creating "gw3vr_dump.req" beside the game. The next frame is written
// to gw3vr_dump_<n>.log, one line per device call. Shader bytecode goes to gw3vr_shaders\.
#include "common.h"
#include "stereo.h"
#include "test.h"
#include "timestep.h"
#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <d3dcompiler.h>
#include <vector>

static IDirect3DDevice9* g_dev;
static FILE* g_dump;
static int g_dumpIndex;
static unsigned g_frame;
static unsigned g_draws;

struct ShaderInfo { uint32_t hash; int index; bool usesViewProj; bool backdrop; bool sprites; };

// The vertex shaders that draw the scenery behind levels and menus: glow, bokeh discs, line
// webs and streamers.
static const uint32_t kBackdropShaders[] = { 0xad044890, 0x590e2dee, 0x1631024d, 0x3d07851d, 0x3571a687, 0xc2dcfe36 };
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
    ShaderInfo info{ h, g_shaderCount++, vp, false, false };
    // Backdrop shaders that build camera-facing sprites take the camera's right and up axes.
    for (size_t i = 0; i + 21 <= n * 4; i++)
        if (!memcmp((const char*)code + i, "backgroundParameters1", 21)) { info.sprites = true; break; }
    if (kind[0] == 'v')
        for (uint32_t b : kBackdropShaders) if (b == h) info.backdrop = true;
    g_shaders[shader] = info;
    // Shader bytecode is written out only when asked for (dump_shaders=1), for tools/ctab.py and fxc.
    static int dumpShaders = -1;
    if (dumpShaders < 0) dumpShaders = IniInt("dump_shaders", 0);
    if (!dumpShaders) return;
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
static uint32_t g_vsHash;
static const uint32_t kFullScreenQuad = 0x39041d6b;
static const uint32_t kCpuProjected = 0xb8717a0e;  // sprites the game projects itself: position is x/w, y/w, z/w
static IDirect3DVertexShader9* g_vsCurrent;
static IDirect3DVertexShader9* g_vsReproject;

// Replacement for kCpuProjected. The game's version outputs its input position as it is. This
// one recovers the clip-space position (w from depth), and applies c0-c3, which take it to an
// eye. A quad on the near plane is a full-screen one and is left where it is.
static const char* kReprojectShader = R"(
float4 r0 : register(c0); float4 r1 : register(c1); float4 r2 : register(c2); float4 r3 : register(c3);
float4 depth : register(c4);
struct I { float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; };
struct O { float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float t2 : TEXCOORD2; };
O main(I i)
{
    O o;
    float w = depth.y / (i.pos.z - depth.x);
    float4 clip = float4(i.pos.xyz * w, w);
    float4 moved = float4(dot(r0, clip), dot(r1, clip), dot(r2, clip), dot(r3, clip));
    o.pos = i.pos.z > 0.01 ? moved : float4(i.pos.xyz, 1);
    o.t0 = i.t0;
    o.t1 = i.t1;
    o.t2 = i.pos.w;
    return o;
}
)";
static bool g_vsBackdrop;
static bool g_vsSprites;
static float g_gameConst[16][4];           // what the game last wrote to c0-c15
static uint32_t g_psHash;
static const uint32_t kFinalPass = 0xa356001c; // colour offset and vignette, drawn to the backbuffer
static const uint32_t kFocusBlur = 0x9545d86c; // behind pop-ups: sharp in the middle of the screen, blurred outside
static float g_gamePsConst[8][4];              // what the game last wrote to pixel constants c0-c7

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

// Frame cost: the time from the headset handing over a frame slot to the GPU finishing the
// game's drawing. It is what must fit in a refresh interval, whatever rate the runtime paces at.
static LARGE_INTEGER g_workStart;
static float g_work[600];
static unsigned g_workCount;
static LARGE_INTEGER g_statStart;

static double Seconds(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) / f.QuadPart;
}

static void RecordFrameCost()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_workStart.QuadPart) { g_statStart = now; return; }
    g_work[g_workCount++] = (float)(Seconds(g_workStart, now) * 1000.0);
    if (g_workCount < 600) return;
    g_workCount = 0;
    float sorted[600];
    memcpy(sorted, g_work, sizeof sorted);
    std::sort(sorted, sorted + 600);
    double sum = 0;
    for (float v : sorted) sum += v;
    Log("VR: %.1f FPS; frame cost avg %.2f ms, worst 1%% %.2f ms, max %.2f ms (600 frames)",
        600.0 / Seconds(g_statStart, now), sum / 600, sorted[593], sorted[599]);
    g_statStart = now;
}

// ---- frame trace ----
// "gw3vr_trace.req" (optionally holding a frame count) records one row per frame to
// gw3vr_trace_N.csv: where the time went, what the runtime predicted, and whether the game's
// world moved since the previous frame (a hash of every object transform it set).
struct TraceRow
{
    unsigned frame;
    double start;                 // ms, when the frame slot was handed over
    double game, flush, copy, end, present, wait, begin, locate;
    long long displayTime, period;
    uint32_t worldHash;
    float camX, camY, camZ;
    unsigned draws;
};
static std::vector<TraceRow> g_trace;
static unsigned g_traceWanted;
static int g_traceIndex;
static TraceRow g_row;
static uint32_t g_worldHash = 2166136261u;
static bool g_camSeen;

static double QpcMs()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

static void TraceCheckRequest()
{
    if (g_traceWanted || (g_frame % 30)) return;
    std::string req = g_dir + "gw3vr_trace.req";
    FILE* f = fopen(req.c_str(), "r");
    if (!f) return;
    unsigned n = 0;
    if (fscanf(f, "%u", &n) != 1 || !n) n = 1000;
    fclose(f);
    DeleteFileA(req.c_str());
    g_traceWanted = n;
    g_trace.clear();
    g_trace.reserve(n);
    Log("frame trace: recording %u frames", n);
}

static void TraceFinish()
{
    char name[64];
    sprintf(name, "gw3vr_trace_%d.csv", g_traceIndex++);
    FILE* f = fopen((g_dir + name).c_str(), "w");
    if (f)
    {
        fprintf(f, "frame,start_ms,game_ms,flush_ms,copy_ms,endframe_ms,present_ms,wait_ms,begin_ms,locate_ms,display_ms,period_ms,world_hash,cam_x,cam_y,cam_z,draws\n");
        for (auto& r : g_trace)
            fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%08x,%g,%g,%g,%u\n", r.frame, r.start, r.game, r.flush, r.copy, r.end,
                r.present, r.wait, r.begin, r.locate, r.displayTime / 1e6, r.period / 1e6, r.worldHash, r.camX, r.camY, r.camZ, r.draws);
        fclose(f);
    }
    Log("frame trace: %u frames written to %s", (unsigned)g_trace.size(), name);
    g_traceWanted = 0;
    g_trace.clear();
}

// ---- draw census ----
// Logs each new combination of shaders, camera kind and target once, with the frame it first
// appeared in. Something that appears only at certain moments (score pop-ups, explosions)
// shows up as a new line at that moment.
static void Census(const char* kind)
{
    static std::unordered_map<uint64_t, int> seen;
    if (!g_vr || !IniInt("draw_census", 0)) return;
    uint64_t key = ((uint64_t)g_vsHash << 32) ^ g_psHash ^ ((uint64_t)(uintptr_t)kind << 7) ^ (g_rtFinal ? 1 : 0) ^ (g_rtStereo ? 2 : 0);
    if (seen[key]++) return;
    Log("census: frame %u vs_%08x ps_%08x kind %s target %s", g_frame, g_vsHash, g_psHash, kind,
        g_rtFinal ? "frame" : g_rtStereo ? "eye-target" : "other");
}

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

std::unordered_map<uint32_t, int> g_unhandledSeen;
static char g_drawNote[256];
static INT g_noteBase;
static UINT g_noteMinV, g_noteNumV;

// Appends the first vertices of the indexed draw being made to g_drawNote (diagnostics only).
static void NoteVertices()
{
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    void* data = nullptr;
    if (FAILED(g_dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb) return;
    if (stride >= 16 && g_noteNumV && SUCCEEDED(vb->Lock(offset + (g_noteBase + g_noteMinV) * stride, g_noteNumV * stride, &data, D3DLOCK_READONLY)) && data)
    {
        size_t at = strlen(g_drawNote);
        at += sprintf(g_drawNote + at, " stride %u", stride);
        for (UINT i = 0; i < g_noteNumV && i < 4 && at < sizeof g_drawNote - 60; i++)
        {
            const float* f = (const float*)((const char*)data + i * stride);
            at += sprintf(g_drawNote + at, " [%.3g %.3g %.3g %.3g]", f[0], f[1], f[2], f[3]);
        }
        vb->Unlock();
    }
    vb->Release();
}

// True when the next draw goes to both eyes with its own matrix for each.
static bool StereoDraw()
{
    return g_vr && g_rtStereo && g_vsUsesViewProj && stereo::HaveMatrix();
}

// Runs a draw once per eye, or once unchanged.
template <typename F> static HRESULT Draw(F draw)
{
    if (g_vr && g_rtStereo && !g_rtFinal && g_vsHash == kCpuProjected && g_vsReproject)
    {
        float m[2][16], depth[4];
        if (stereo::Reproject(0, m[0], depth) && stereo::Reproject(1, m[1], depth))
        {
            o_SetVertexShader(g_dev, g_vsReproject);
            o_SetVSConstF(g_dev, 4, depth, 1);
            HRESULT hr = S_OK;
            for (int eye = 0; eye < 2; eye++)
            {
                SetEyeViewport(eye);
                o_SetVSConstF(g_dev, 0, m[eye], 4);
                hr = draw();
            }
            o_SetVertexShader(g_dev, g_vsCurrent);
            o_SetVSConstF(g_dev, 4, g_gameConst[4], 1);
            return hr;
        }
    }
    if (!StereoDraw())
    {
        // A shader that places its vertices itself, drawn into the eyes, is either a full-screen
        // pass (fine: it covers both eyes) or something that needs handling. Name each once.
        if (g_vr && g_rtStereo && !g_vsUsesViewProj && g_vsHash && g_vsHash != kFullScreenQuad)
        {
            extern std::unordered_map<uint32_t, int> g_unhandledSeen;
            auto& seen = g_unhandledSeen;
            if (seen[g_vsHash] < 8) NoteVertices();
            if (seen[g_vsHash]++ < 8) Log("unhandled: vs_%08x ps_%08x draws into the eyes without the game's matrix, frame %u: %s", g_vsHash, g_psHash, g_frame, g_drawNote);
        }
        EnsureViewport();
        Census(g_vsUsesViewProj ? "unplaced" : "own-position");
        if (g_vr && g_rtStereo && g_psHash == kFocusBlur)
        {
            // The pass measures distance from a point given in c5 (scaled by c6.zw) across the
            // whole image. Drawn once per eye, clipped to that eye's half, with the point and
            // the horizontal scale restated for the half.
            DWORD scissor = FALSE;
            g_dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
            o_SetRenderState(g_dev, D3DRS_SCISSORTESTENABLE, TRUE);
            HRESULT hr = S_OK;
            for (int eye = 0; eye < 2; eye++)
            {
                RECT half{ (LONG)(eye * g_rtWidth / 2), 0, (LONG)((eye + 1) * g_rtWidth / 2), (LONG)g_viewport.Height };
                g_dev->SetScissorRect(&half);
                float c5[4], c6[4];
                memcpy(c5, g_gamePsConst[5], sizeof c5);
                memcpy(c6, g_gamePsConst[6], sizeof c6);
                // The game's point is relative to the middle of its screen; here that middle
                // is straight ahead of the seat, and the eye's image spans several screens.
                float u, v, su, sv;
                stereo::ScreenCentre(eye, &u, &v, &su, &sv);
                c5[0] = (u + (c5[0] - 0.5f) / su + eye) * 0.5f;
                c5[1] = v + (c5[1] - 0.5f) / sv;
                c6[2] *= 2.0f * su;
                c6[3] *= sv;
                o_SetPSConstF(g_dev, 5, c5, 1);
                o_SetPSConstF(g_dev, 6, c6, 1);
                hr = draw();
            }
            o_SetPSConstF(g_dev, 5, g_gamePsConst[5], 2);
            o_SetRenderState(g_dev, D3DRS_SCISSORTESTENABLE, scissor);
            return hr;
        }
        return draw();
    }
    HRESULT hr = S_OK;
    bool backdrop = g_vsBackdrop && !g_rtFinal;
    bool steady = false;
    for (int eye = 0; eye < 2; eye++)
    {
        SetEyeViewport(eye);
        o_SetVSConstF(g_dev, 0, stereo::EyeMatrix(eye, g_rtFinal, backdrop), 4);
        if (backdrop && eye == 0)
        {
            stereo::SteadyValues v;
            steady = stereo::SteadyCamera(&v);
            if (steady)
            {
                o_SetVSConstF(g_dev, 4, v.position, 1);
                o_SetVSConstF(g_dev, 5, v.direction, 1);
                if (g_vsSprites) { o_SetVSConstF(g_dev, 7, v.right, 1); o_SetVSConstF(g_dev, 8, v.up, 1); }
            }
        }
        hr = draw();
        if (eye == 0) Census(stereo::LastKind());
    }
    if (steady) o_SetVSConstF(g_dev, 4, g_gameConst[4], 5); // c4-c8 back to the game's values
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
    double tGameEnd = QpcMs();
    EnsureViewport();
    g_stage = "waiting for the GPU to finish D3D9 work";
    FlushD3D9();
    double tFlushed = QpcMs();
    RecordFrameCost();
    g_stage = "screenshot";
    test::Screenshot(g_dev, g_finalSurf);
    g_stage = "submitting the eyes (xrEndFrame)";
    vr::EndFrame(g_finalShare);
    if (g_traceWanted)
    {
        const vr::Timing& t = vr::LastTiming();
        g_row.frame = g_frame;
        g_row.game = tGameEnd - g_row.start;
        g_row.flush = tFlushed - tGameEnd;
        g_row.copy = t.copyMs;
        g_row.end = t.endMs;
        g_row.worldHash = g_worldHash;
        g_row.draws = g_draws;
    }
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
    QueryPerformanceCounter(&g_workStart);
    {
        // sim_rate: 0 = the headset's refresh rate (default), N = N Hz, -1 = the game's own 60 Hz.
        // The runtime's predicted period varies from frame to frame (SteamVR reports half and
        // third rates now and then), so the rate is the most common one over the first 90 frames,
        // chosen once.
        static int simRate = IniInt("sim_rate", 0);
        static int samples, chosen;
        static std::unordered_map<int, int> votes;
        long long period = vr::LastTiming().displayPeriod;
        if (simRate > 0) timestep::Apply(simRate);
        else if (simRate == 0 && vr::RefreshRate() > 0) timestep::Apply(floor(vr::RefreshRate() + 0.5));
        else if (simRate == 0 && !chosen && period > 0)
        {
            votes[(int)floor(1e9 / period + 0.5)]++;
            if (++samples == 90)
            {
                for (auto& v : votes) if (!chosen || v.second > votes[chosen]) chosen = v.first;
                timestep::Apply(chosen);
            }
        }
    }
    if (g_traceWanted)
    {
        if (g_row.frame)
        {
            g_trace.push_back(g_row);
            if (g_trace.size() >= g_traceWanted) TraceFinish();
        }
        const vr::Timing& t = vr::LastTiming();
        g_row = {};
        g_row.wait = t.waitMs;
        g_row.begin = t.beginMs;
        g_row.locate = t.locateMs;
        g_row.displayTime = t.displayTime;
        g_row.period = t.displayPeriod;
    }
    g_worldHash = 2166136261u;
    g_camSeen = false;
    g_row.start = QpcMs();
    TraceCheckRequest();
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
    double tp = QpcMs();
    HRESULT hr = o_Present(d, a, b, w, r);
    g_row.present = QpcMs() - tp;
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
    double tp = QpcMs();
    HRESULT hr = o_PresentEx(d, a, b, w, r, f);
    g_row.present = QpcMs() - tp;
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
    if (!g_vsUsesViewProj) { sprintf(g_drawNote, "type %d start %u prims %u", t, start, count); g_noteNumV = 0; }
    return Draw([&] { return o_DrawPrimitive(d, t, start, count); });
}

static HRESULT __stdcall H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minV, UINT numV, UINT start, UINT count)
{
    g_draws++;
    D("DrawIndexedPrimitive type %d base %d verts %u start %u prims %u", t, base, numV, start, count);
    if (!g_vsUsesViewProj)
    {
        sprintf(g_drawNote, "indexed type %d verts %u prims %u", t, numV, count);
        g_noteBase = base; g_noteMinV = minV; g_noteNumV = numV;
    }
    if (g_dump && !g_vsUsesViewProj)
    {
        // Frame dumps only: the vertices of draws that place themselves, read back from the buffer.
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT offset = 0, stride = 0;
        void* data = nullptr;
        if (SUCCEEDED(d->GetStreamSource(0, &vb, &offset, &stride)) && vb)
        {
            if (stride >= 16 && SUCCEEDED(vb->Lock(offset + (base + minV) * stride, numV * stride, &data, D3DLOCK_READONLY)) && data)
            {
                for (UINT i = 0; i < numV && i < 8; i++)
                {
                    const float* f = (const float*)((const char*)data + i * stride);
                    fprintf(g_dump, "  vertex %u: [%g %g %g %g] [%g %g %g %g]\n", i, f[0], f[1], f[2], f[3],
                        stride >= 32 ? f[4] : 0.f, stride >= 32 ? f[5] : 0.f, stride >= 32 ? f[6] : 0.f, stride >= 32 ? f[7] : 0.f);
                }
                vb->Unlock();
            }
            vb->Release();
        }
    }
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
    if (!g_vsUsesViewProj && stride >= 16)
    {
        const float* f = (const float*)data;
        const float* g = (const float*)((const char*)data + stride);
        const float* h = (const float*)((const char*)data + 2 * stride);
        sprintf(g_drawNote, "UP type %d prims %u stride %u [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]", t, count, stride,
            f[0], f[1], f[2], f[3], g[0], g[1], g[2], g[3], h[0], h[1], h[2], h[3]);
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
    g_vsCurrent = s;
    auto it = g_shaders.find(s);
    g_vsUsesViewProj = it != g_shaders.end() && it->second.usesViewProj;
    g_vsBackdrop = it != g_shaders.end() && it->second.backdrop;
    g_vsSprites = it != g_shaders.end() && it->second.sprites;
    g_vsHash = it != g_shaders.end() ? it->second.hash : 0;
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
    if (g_traceWanted)
    {
        if (reg == 13)
            for (UINT i = 0; i < count * 4; i++) { uint32_t u; memcpy(&u, &f[i], 4); g_worldHash = (g_worldHash ^ u) * 16777619u; }
        if (reg == 4 && !g_camSeen && fabsf(f[0]) + fabsf(f[1]) + fabsf(f[2]) > 1) { g_row.camX = f[0]; g_row.camY = f[1]; g_row.camZ = f[2]; g_camSeen = true; }
    }
    if (reg < 16) memcpy(g_gameConst[reg], f, (reg + count > 16 ? 16 - reg : count) * 16);
    return o_SetVSConstF(d, reg, f, count);
}

static HRESULT __stdcall H_SetPSConstF(IDirect3DDevice9* d, UINT reg, const float* f, UINT count)
{
    DumpConsts("PSConst", reg, f, count);
    if (reg < 8) memcpy(g_gamePsConst[reg], f, (reg + count > 8 ? 8 - reg : count) * 16);
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
    else if (IniInt("sim_rate", 0) > 0) timestep::Apply(IniInt("sim_rate", 0)); // flat play at a high refresh rate
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
    if (g_vr && !g_vsReproject)
    {
        ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
        if (SUCCEEDED(D3DCompile(kReprojectShader, strlen(kReprojectShader), nullptr, nullptr, nullptr, "main", "vs_3_0", 0, 0, &code, &err)))
        {
            o_CreateVertexShader(dev, (const DWORD*)code->GetBufferPointer(), &g_vsReproject);
            code->Release();
        }
        else Log("reprojection shader: %s", err ? (const char*)err->GetBufferPointer() : "compile failed");
    }
    SetupDevice();
    test::PlaceWindow(dev);
    static bool watchdog;
    if (!watchdog) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_renderThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!watchdog) { watchdog = true; CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)); }
}
