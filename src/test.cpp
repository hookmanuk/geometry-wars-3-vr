// Unattended-test hooks: file-driven controller, in-game screenshots, scaled window.
#include "test.h"
#include <xinput.h>
#include <vector>

namespace
{
    typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
    XInputGetState_t o_XInputGetState;
    XINPUT_GAMEPAD g_pad;
    bool g_padActive;
    DWORD g_padRead;
    DWORD g_packet;

    void ReadPad()
    {
        DWORD now = GetTickCount();
        if (now - g_padRead < 30) return;
        g_padRead = now;
        FILE* f = fopen((g_dir + "gw3vr_pad.txt").c_str(), "r");
        if (!f) { g_padActive = false; return; }
        unsigned buttons = 0;
        float v[6] = {};
        int n = fscanf(f, "%x %f %f %f %f %f %f", &buttons, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        fclose(f);
        if (n < 1) return; // caught mid-write: keep the last state
        g_padActive = true;
        g_pad.wButtons = (WORD)buttons;
        g_pad.sThumbLX = (SHORT)(v[0] * 32767);
        g_pad.sThumbLY = (SHORT)(v[1] * 32767);
        g_pad.sThumbRX = (SHORT)(v[2] * 32767);
        g_pad.sThumbRY = (SHORT)(v[3] * 32767);
        g_pad.bLeftTrigger = (BYTE)(v[4] * 255);
        g_pad.bRightTrigger = (BYTE)(v[5] * 255);
    }

    bool g_sticksClicked;

    DWORD WINAPI H_XInputGetState(DWORD index, XINPUT_STATE* state)
    {
        ReadPad();
        DWORD result = ERROR_SUCCESS;
        if (!g_padActive) result = o_XInputGetState(index, state);
        else if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
        else
        {
            state->dwPacketNumber = ++g_packet;
            state->Gamepad = g_pad;
        }
        const WORD both = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
        if (result == ERROR_SUCCESS && index == 0 && (state->Gamepad.wButtons & both) == both) g_sticksClicked = true;
        return result;
    }

    // Points the game's import of XInputGetState (ordinal 2 of XINPUT1_3.dll) at ours.
    void PatchXInput()
    {
        HMODULE xinput = GetModuleHandleA("XINPUT1_3.dll");
        if (!xinput) { Log("test: XINPUT1_3.dll is not loaded"); return; }
        auto real = (XInputGetState_t)GetProcAddress(xinput, (LPCSTR)2);
        BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return;
        for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
        {
            if (_stricmp((const char*)base + imp->Name, "XINPUT1_3.dll")) continue;
            for (auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); thunk->u1.Function; thunk++)
            {
                if ((void*)thunk->u1.Function != (void*)real) continue;
                DWORD old;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                thunk->u1.Function = (DWORD_PTR)H_XInputGetState;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                o_XInputGetState = real;
                Log("test: controller input can be driven from gw3vr_pad.txt");
                return;
            }
        }
        Log("test: the game's XInputGetState import was not found");
    }
}

bool test::RecentreRequested()
{
    static bool wasDown;
    static unsigned frame;
    bool now = false;
    bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0 || g_sticksClicked;
    g_sticksClicked = false;
    if (down && !wasDown) now = true;
    wasDown = down;
    if (++frame % 30 == 0)
    {
        std::string req = g_dir + "gw3vr_recentre.req";
        if (GetFileAttributesA(req.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(req.c_str()); now = true; }
    }
    return now;
}

namespace
{
    WNDPROC o_WndProc;

    // The game stops its main loop while its window is not the active one. In VR the window is
    // only a mirror and something else is often in front, so the game is never told.
    LRESULT CALLBACK H_WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) return 0;
        if (msg == WM_ACTIVATEAPP && !wp) return 0;
        if (msg == WM_KILLFOCUS) return 0;
        return CallWindowProcA(o_WndProc, wnd, msg, wp, lp);
    }
}

void test::KeepActive(HWND wnd)
{
    if (o_WndProc || !wnd || !IniInt("keep_running", 1)) return;
    o_WndProc = (WNDPROC)SetWindowLongPtrA(wnd, GWLP_WNDPROC, (LONG_PTR)H_WndProc);
    Log("the game keeps running while its window is inactive");
}

void test::CaptureKey()
{
    static bool wasDown;
    static int count;
    bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !wasDown)
    {
        FILE* f = fopen((g_dir + "gw3vr_dump.req").c_str(), "w");
        if (f) fclose(f);
        char shot[MAX_PATH];
        sprintf(shot, "%sgw3vr_f9_%d.bmp", g_dir.c_str(), count++);
        f = fopen((g_dir + "gw3vr_shot.req").c_str(), "w");
        if (f) { fputs(shot, f); fclose(f); }
        Log("F9: capturing this moment (%s and the next frame dump)", shot);
    }
    wasDown = down;
}

bool test::SettingsChanged()
{
    static unsigned frame;
    static FILETIME last;
    if (++frame % 30) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA((g_dir + "gw3vr.ini").c_str(), GetFileExInfoStandard, &data)) return false;
    if (!CompareFileTime(&data.ftLastWriteTime, &last)) return false;
    bool first = !last.dwLowDateTime && !last.dwHighDateTime;
    last = data.ftLastWriteTime;
    return !first;
}

void test::Install()
{
    if (!o_XInputGetState) PatchXInput();
}

void test::PlaceWindow(IDirect3DDevice9* dev)
{
    if (!IniInt("test_window", 0)) return;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
    SetWindowPos(cp.hFocusWindow, HWND_BOTTOM, 0, 0, 960, 540, SWP_NOACTIVATE);
}

void test::Screenshot(IDirect3DDevice9* dev, IDirect3DSurface9* surface)
{
    std::string req = g_dir + "gw3vr_shot.req";
    if (GetFileAttributesA(req.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    char path[MAX_PATH] = {}, mode[16] = {};
    FILE* f = fopen(req.c_str(), "r");
    if (!f) return;
    int n = fscanf(f, "%259s %15s", path, mode);
    fclose(f);
    if (n < 1) return; // still being written
    DeleteFileA(req.c_str());

    IDirect3DSurface9* own = nullptr;
    if (!surface)
    {
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &own);
        surface = own;
    }
    if (!surface) return;
    D3DSURFACE_DESC d{};
    surface->GetDesc(&d);
    UINT w = d.Width, h = d.Height;
    if (strcmp(mode, "full") && w > 2560) { h = (UINT)((uint64_t)h * 2560 / w); w = 2560; }

    IDirect3DSurface9* scaled = nullptr;
    IDirect3DSurface9* mem = nullptr;
    HRESULT hr = dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &scaled, nullptr);
    if (SUCCEEDED(hr)) hr = dev->StretchRect(surface, nullptr, scaled, nullptr, D3DTEXF_LINEAR);
    if (SUCCEEDED(hr)) hr = dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &mem, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(scaled, mem);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr)) hr = mem->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr))
    {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + w * h * 4;
        ih.biSize = sizeof ih;
        ih.biWidth = w;
        ih.biHeight = -(LONG)h;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        // Written under a temporary name so a reader never sees half a file.
        std::string tmp = std::string(path) + ".tmp";
        FILE* out = fopen(tmp.c_str(), "wb");
        if (out)
        {
            fwrite(&fh, sizeof fh, 1, out);
            fwrite(&ih, sizeof ih, 1, out);
            for (UINT y = 0; y < h; y++) fwrite((BYTE*)lr.pBits + y * lr.Pitch, 4, w, out);
            fclose(out);
            MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
        }
        mem->UnlockRect();
    }
    Log("screenshot %ux%u -> %s (%#lx)", w, h, path, hr);
    if (mem) mem->Release();
    if (scaled) scaled->Release();
    if (own) own->Release();
}
