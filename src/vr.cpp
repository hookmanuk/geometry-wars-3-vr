// OpenXR session, D3D11 device and eye submission.
//
// The game renders both eyes side by side into a D3D9Ex shared texture. Here that texture is
// opened in D3D11 and each half is drawn into that eye's OpenXR swapchain image.
#include "vr.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <vector>
#include <cmath>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace
{
    bool g_active;
    XrInstance g_instance;
    XrSystemId g_system;
    XrSession g_session;
    XrSpace g_space;
    bool g_running;
    bool g_frameBegun;
    bool g_shouldRender;
    XrTime g_displayTime;
    XrView g_views[2];
    unsigned g_eyeW, g_eyeH;
    vr::Timing g_timing;
    float g_refreshRate; // from XR_FB_display_refresh_rate, 0 when the runtime does not say

    double NowMs()
    {
        static LARGE_INTEGER f;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart * 1000.0 / f.QuadPart;
    }
    DXGI_FORMAT g_format;
    bool g_srgb;

    ID3D11Device* g_d3d;
    ID3D11DeviceContext* g_ctx;
    ID3D11Texture2D* g_shared;
    ID3D11ShaderResourceView* g_sharedView;
    HANDLE g_sharedHandle;
    ID3D11VertexShader* g_vs;
    ID3D11PixelShader* g_ps;
    ID3D11Buffer* g_cb;
    ID3D11SamplerState* g_sampler;

    struct EyeChain
    {
        XrSwapchain chain;
        std::vector<ID3D11RenderTargetView*> views;
    };
    EyeChain g_chains[2];


    bool Check(XrResult r, const char* what)
    {
        if (XR_SUCCEEDED(r)) return true;
        Log("OpenXR: %s failed (%d)", what, r);
        return false;
    }

    const char* kShader = R"(
cbuffer C : register(b0) { float4 p; };   // x = u offset, y = u scale, z = convert sRGB to linear
Texture2D t : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o;
    float2 c = float2((id << 1) & 2, id & 2);
    o.uv = float2(p.x + c.x * p.y, c.y);
    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target
{
    float3 c = t.Sample(s, i.uv).rgb;
    if (p.z > 0.5) c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    return float4(c, 1);
}
)";

    bool CreateD3D11(LUID luid)
    {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
        {
            DXGI_ADAPTER_DESC1 d;
            adapter->GetDesc1(&d);
            if (!memcmp(&d.AdapterLuid, &luid, sizeof luid)) break;
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
        if (!adapter) { Log("OpenXR: the runtime's adapter was not found"); return false; }
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g_d3d, nullptr, &g_ctx);
        adapter->Release();
        if (FAILED(hr)) { Log("D3D11CreateDevice failed %#lx", hr); return false; }

        ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &err)) ||
            FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &err)))
        {
            Log("blit shader: %s", err ? (const char*)err->GetBufferPointer() : "compile failed");
            return false;
        }
        g_d3d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
        g_d3d->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
        vs->Release(); ps->Release();
        D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
        g_d3d->CreateBuffer(&bd, nullptr, &g_cb);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_d3d->CreateSamplerState(&sd, &g_sampler);
        return true;
    }

    bool CreateSwapchains()
    {
        uint32_t n = 0;
        xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
        std::vector<int64_t> formats(n);
        xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
        std::string list;
        for (int64_t f : formats) list += std::to_string(f) + " ";
        Log("OpenXR: swapchain formats %s", list.c_str());
        // sRGB first: the game's output is gamma-encoded and the runtime must know that.
        const DXGI_FORMAT wanted[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
        g_format = DXGI_FORMAT_UNKNOWN;
        for (DXGI_FORMAT w : wanted)
        {
            for (int64_t f : formats) if (f == w) { g_format = w; break; }
            if (g_format != DXGI_FORMAT_UNKNOWN) break;
        }
        if (g_format == DXGI_FORMAT_UNKNOWN) { Log("OpenXR: no usable swapchain format"); return false; }
        g_srgb = g_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || g_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

        for (int e = 0; e < 2; e++)
        {
            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = g_format;
            ci.sampleCount = 1;
            ci.width = g_eyeW;
            ci.height = g_eyeH;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            if (!Check(xrCreateSwapchain(g_session, &ci, &g_chains[e].chain), "xrCreateSwapchain")) return false;
            uint32_t count = 0;
            xrEnumerateSwapchainImages(g_chains[e].chain, 0, &count, nullptr);
            std::vector<XrSwapchainImageD3D11KHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            xrEnumerateSwapchainImages(g_chains[e].chain, count, &count, (XrSwapchainImageBaseHeader*)images.data());
            for (auto& img : images)
            {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = g_format;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* view = nullptr;
                HRESULT hr = g_d3d->CreateRenderTargetView(img.texture, &rd, &view);
                if (FAILED(hr)) { Log("swapchain view failed %#lx", hr); return false; }
                g_chains[e].views.push_back(view);
            }
        }
        Log("OpenXR: eyes %ux%u format %d", g_eyeW, g_eyeH, g_format);
        return true;
    }

    // Sets the "disable" variable of every implicit API layer registered for this process's
    // bitness. Used only after instance creation failed: one broken layer makes the loader
    // refuse to start at all.
    void DisableImplicitLayers()
    {
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        {
            HKEY key;
            if (RegOpenKeyExA(root, "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
            char name[1024];
            for (DWORD i = 0;; i++)
            {
                DWORD len = sizeof name;
                if (RegEnumValueA(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                FILE* f = fopen(name, "rb");
                if (!f) continue;
                std::string text(4096, 0);
                text.resize(fread(&text[0], 1, text.size(), f));
                fclose(f);
                size_t at = text.find("\"disable_environment\"");
                if (at == std::string::npos) continue;
                size_t open = text.find('"', text.find(':', at));
                size_t close = text.find('"', open + 1);
                if (open == std::string::npos || close == std::string::npos) continue;
                std::string var = text.substr(open + 1, close - open - 1);
                SetEnvironmentVariableA(var.c_str(), "1");
                Log("OpenXR: disabled API layer %s", name);
            }
            RegCloseKey(key);
        }
    }

    // Wine and Proton export wine_get_version from ntdll; Windows does not.
    const char* WineVersion()
    {
        typedef const char* (__cdecl* Fn)();
        Fn fn = (Fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version");
        return fn ? fn() : nullptr;
    }

    void PollEvents()
    {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                auto* s = (XrEventDataSessionStateChanged*)&ev;
                Log("OpenXR: session state %d", s->state);
                if (s->state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    g_running = Check(xrBeginSession(g_session, &bi), "xrBeginSession");
                }
                else if (s->state == XR_SESSION_STATE_STOPPING)
                {
                    xrEndSession(g_session);
                    g_running = false;
                }
                else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    g_running = false;
                }
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    void DefaultEyes(Eye eyes[2])
    {
        for (int e = 0; e < 2; e++)
        {
            eyes[e] = {};
            eyes[e].tanL = -1.0f; eyes[e].tanR = 1.0f; eyes[e].tanU = 1.0f; eyes[e].tanD = -1.0f;
            eyes[e].pos[0] = e ? 0.032f : -0.032f;
            eyes[e].rot[3] = 1.0f;
        }
    }
}

bool vr::Active() { return g_active; }
float vr::RefreshRate() { return g_refreshRate; }
const vr::Timing& vr::LastTiming() { return g_timing; }
unsigned vr::EyeWidth() { return g_eyeW; }
unsigned vr::EyeHeight() { return g_eyeH; }

bool vr::Init()
{
    if (!IniInt("vr", 1)) { Log("VR is switched off in gw3vr.ini"); return false; }
    std::string runtime = IniString("runtime_json");
    if (!runtime.empty()) SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime.c_str());

    const char* wine = WineVersion();
    if (wine) Log("running under Wine/Proton (wine %s)", wine);

    // The refresh-rate extension, when the runtime has it, gives the headset's actual rate.
    bool haveRate = false, haveD3D11 = false;
    {
        uint32_t count = 0;
        XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        std::vector<XrExtensionProperties> props(count, { XR_TYPE_EXTENSION_PROPERTIES });
        std::string names;
        if (count && XR_SUCCEEDED(r = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data())))
            for (auto& p : props)
            {
                if (!strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) haveRate = true;
                if (!strcmp(p.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) haveD3D11 = true;
                names += " ";
                names += p.extensionName;
            }
        Log("OpenXR: %u extensions (%d):%s", count, r, names.c_str());
        Log("OpenXR: %s %s, %s %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME, haveD3D11 ? "available" : "NOT available",
            XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, haveRate ? "available" : "not available");
        if (!haveD3D11)
        {
            // No 32-bit runtime with D3D11 support: the loader then lists only its own extensions.
            if (wine)
                Log("OpenXR: no 32-bit OpenXR runtime in this Proton. It needs Proton Experimental (October 2026 or "
                    "newer) with PROTON_USE_WOW64=1 in the game's launch options, and SteamVR or Monado/WiVRn running");
            else
                Log("OpenXR: the active 32-bit OpenXR runtime is missing or has no D3D11 support; SteamVR has both");
        }
    }
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME };
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "Geometry Wars 3 VR");
    strcpy(ici.applicationInfo.engineName, "gw3vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = haveRate ? 2 : 1;
    ici.enabledExtensionNames = exts;
    XrResult created = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(created))
    {
        Log("OpenXR: xrCreateInstance failed (%d), trying again without API layers", created);
        DisableImplicitLayers();
        if (!Check(xrCreateInstance(&ici, &g_instance), "xrCreateInstance")) return false;
    }

    XrInstanceProperties props{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g_instance, &props);
    Log("OpenXR runtime: %s", props.runtimeName);

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(g_instance, &sgi, &g_system), "xrGetSystem")) return false;

    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t n = 0;
    if (!Check(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv), "view configuration")) return false;
    float scale = IniFloat("render_scale", 1.0f);
    g_eyeW = (unsigned)(vcv[0].recommendedImageRectWidth * scale) & ~1u;
    g_eyeH = (unsigned)(vcv[0].recommendedImageRectHeight * scale) & ~1u;
    Log("OpenXR: recommended eye %ux%u, rendering %ux%u", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, g_eyeW, g_eyeH);

    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getRequirements);
    if (!getRequirements || !Check(getRequirements(g_instance, g_system, &req), "graphics requirements")) return false;
    if (!CreateD3D11(req.adapterLuid)) return false;
    Log("OpenXR: D3D11 device ready, creating the session");

    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = g_d3d;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = g_system;
    if (!Check(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) return false;

    if (haveRate)
    {
        PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
        xrGetInstanceProcAddr(g_instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&getRate);
        float rate = 0;
        if (getRate && XR_SUCCEEDED(getRate(g_session, &rate))) g_refreshRate = rate;
        Log("OpenXR: display refresh rate %.2f Hz", g_refreshRate);
    }

    XrReferenceSpaceCreateInfo rsi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(g_session, &rsi, &g_space), "xrCreateReferenceSpace")) return false;

    if (!CreateSwapchains()) return false;
    g_active = true;
    return true;
}

bool vr::BeginFrame(Eye eyes[2])
{
    DefaultEyes(eyes);
    PollEvents();
    g_frameBegun = false;
    g_shouldRender = false;
    if (!g_running) return false;

    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    double t0 = NowMs();
    if (!Check(xrWaitFrame(g_session, nullptr, &fs), "xrWaitFrame")) return false;
    double t1 = NowMs();
    if (!Check(xrBeginFrame(g_session, nullptr), "xrBeginFrame")) return false;
    double t2 = NowMs();
    g_timing.waitMs = t1 - t0;
    g_timing.beginMs = t2 - t1;
    g_timing.displayTime = fs.predictedDisplayTime;
    g_timing.displayPeriod = fs.predictedDisplayPeriod;
    g_timing.shouldRender = fs.shouldRender == XR_TRUE;
    g_frameBegun = true;
    g_displayTime = fs.predictedDisplayTime;
    g_shouldRender = fs.shouldRender == XR_TRUE;

    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_displayTime;
    li.space = g_space;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    g_views[0] = { XR_TYPE_VIEW };
    g_views[1] = { XR_TYPE_VIEW };
    if (XR_FAILED(xrLocateViews(g_session, &li, &vs, 2, &n, g_views)) || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        g_shouldRender = false;
        return false;
    }
    g_timing.locateMs = NowMs() - t2;
    for (int e = 0; e < 2; e++)
    {
        const XrView& v = g_views[e];
        eyes[e].tanL = tanf(v.fov.angleLeft);
        eyes[e].tanR = tanf(v.fov.angleRight);
        eyes[e].tanU = tanf(v.fov.angleUp);
        eyes[e].tanD = tanf(v.fov.angleDown);
        eyes[e].pos[0] = v.pose.position.x; eyes[e].pos[1] = v.pose.position.y; eyes[e].pos[2] = v.pose.position.z;
        eyes[e].rot[0] = v.pose.orientation.x; eyes[e].rot[1] = v.pose.orientation.y;
        eyes[e].rot[2] = v.pose.orientation.z; eyes[e].rot[3] = v.pose.orientation.w;
    }
    return true;
}

void vr::EndFrame(HANDLE sharedSbs)
{
    if (!g_frameBegun) return;
    g_frameBegun = false;

    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = g_displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    double t0 = NowMs();
    if (g_shouldRender && sharedSbs)
    {
        if (sharedSbs != g_sharedHandle)
        {
            if (g_sharedView) g_sharedView->Release();
            if (g_shared) g_shared->Release();
            g_shared = nullptr; g_sharedView = nullptr;
            HRESULT hr = g_d3d->OpenSharedResource(sharedSbs, __uuidof(ID3D11Texture2D), (void**)&g_shared);
            if (SUCCEEDED(hr)) hr = g_d3d->CreateShaderResourceView(g_shared, nullptr, &g_sharedView);
            Log("opened the shared eye texture in D3D11: %#lx", hr);
            g_sharedHandle = sharedSbs;
        }
        bool ok = g_sharedView != nullptr;
        for (int e = 0; e < 2 && ok; e++)
        {
            uint32_t index = 0;
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(xrAcquireSwapchainImage(g_chains[e].chain, nullptr, &index)) ||
                XR_FAILED(xrWaitSwapchainImage(g_chains[e].chain, &wi)))
            {
                ok = false;
                break;
            }
            float c[4] = { e * 0.5f, 0.5f, g_srgb ? 1.0f : 0.0f, 0.0f };
            g_ctx->UpdateSubresource(g_cb, 0, nullptr, c, 0, 0);
            D3D11_VIEWPORT vp{ 0, 0, (float)g_eyeW, (float)g_eyeH, 0, 1 };
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->OMSetRenderTargets(1, &g_chains[e].views[index], nullptr);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->IASetInputLayout(nullptr);
            g_ctx->VSSetShader(g_vs, nullptr, 0);
            g_ctx->PSSetShader(g_ps, nullptr, 0);
            g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
            g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
            g_ctx->PSSetShaderResources(0, 1, &g_sharedView);
            g_ctx->PSSetSamplers(0, 1, &g_sampler);
            g_ctx->Draw(3, 0);
            ID3D11RenderTargetView* none = nullptr;
            g_ctx->OMSetRenderTargets(1, &none, nullptr);
            xrReleaseSwapchainImage(g_chains[e].chain, nullptr);

            pv[e].pose = g_views[e].pose;
            pv[e].fov = g_views[e].fov;
            pv[e].subImage.swapchain = g_chains[e].chain;
            pv[e].subImage.imageRect.extent = { (int32_t)g_eyeW, (int32_t)g_eyeH };
        }
        if (ok)
        {
            g_ctx->Flush();
            layer.space = g_space;
            layer.viewCount = 2;
            layer.views = pv;
            ei.layerCount = 1;
            ei.layers = layers;
        }
    }
    double t1 = NowMs();
    Check(xrEndFrame(g_session, &ei), "xrEndFrame");
    g_timing.copyMs = t1 - t0;
    g_timing.endMs = NowMs() - t1;

}
