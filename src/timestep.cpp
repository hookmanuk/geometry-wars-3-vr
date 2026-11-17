// Raises the game's fixed 60 Hz simulation step to the headset's refresh rate.
//
// The game advances its world by a float step of 1/60 s (0x3C888889), stored in a few places:
// an immediate in the code that sets the step up, a constant, and variables in its data. At
// 90 Hz output it then runs two updates for every three frames, so every third frame repeats
// the last one and motion stutters at 30 Hz. Writing 1/rate to all of them makes the game run
// one update per frame. Matches the "High frame rate" fix on PCGamingWiki.
#include "timestep.h"

namespace
{
    const uint32_t kStep60 = 0x3C888889; // 1/60 as a float
    volatile uint32_t g_target;          // the float bits to write, 0 until a rate is set
    double g_rate;
    HANDLE g_thread;

    // Replaces every 4-byte occurrence of kStep60 in the game's image with g_target.
    int Patch()
    {
        uint32_t target = g_target;
        if (!target) return 0;
        BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        BYTE* end = base + nt->OptionalHeader.SizeOfImage;
        int count = 0;
        for (BYTE* p = base; p < end;)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            BYTE* regionEnd = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
            if (regionEnd > end) regionEnd = end;
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD))
            {
                for (BYTE* q = p; q + 4 <= regionEnd; q++)
                {
                    if (*(const uint32_t*)q != kStep60) continue;
                    DWORD old;
                    if (!VirtualProtect(q, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
                    *(volatile uint32_t*)q = target;
                    VirtualProtect(q, 4, old, &old);
                    if (old & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
                        FlushInstructionCache(GetCurrentProcess(), q, 4);
                    Log("timestep: 1/60 at GW3.exe+%#x set to 1/%g", (unsigned)(q - base), g_rate);
                    count++;
                }
            }
            p = regionEnd;
        }
        return count;
    }

    DWORD WINAPI Rescan(void*)
    {
        for (;;)
        {
            Patch();
            Sleep(2000);
        }
    }
}

void timestep::Apply(double rate)
{
    if (rate < 30 || rate > 1000 || rate == g_rate) return;
    if (g_rate) Log("timestep: rate changed from %g to %g; values already patched keep 1/%g", g_rate, rate, g_rate);
    g_rate = rate;
    float step = (float)(1.0 / rate);
    uint32_t bits;
    memcpy(&bits, &step, 4);
    g_target = bits;
    int n = Patch();
    Log("timestep: game simulation set to %g Hz (%d values changed now)", rate, n);
    if (!g_thread) g_thread = CreateThread(nullptr, 0, Rescan, nullptr, 0, nullptr);
}
