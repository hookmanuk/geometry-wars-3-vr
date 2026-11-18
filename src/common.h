// Shared declarations for the Geometry Wars 3 VR d3d9 proxy.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <string>

#define GW3VR_VERSION "0.1"

// Folder holding GW3.exe (with a trailing backslash).
extern std::string g_dir;

void Log(const char* fmt, ...);

// Replaces vt[idx] with fn and stores the previous entry in *orig.
void HookVt(void** vt, int idx, void* fn, void** orig);

// With VR on: records the size the game asked for and makes the window a plain mirror.
void VrPresentParameters(D3DPRESENT_PARAMETERS* pp);

// Called once the game's device exists.
void InstallDeviceHooks(IDirect3DDevice9* dev);

// Reads "key=value" from gw3vr.ini beside the game, or returns def.
std::string IniString(const char* key);
int IniInt(const char* key, int def);
float IniFloat(const char* key, float def);
