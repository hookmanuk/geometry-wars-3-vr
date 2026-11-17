// Hooks for unattended testing: a controller driven from a file, screenshots taken in the game,
// and a small window that does not get in the way. All are inert unless asked for.
#pragma once
#include "common.h"

namespace test
{
    // Replaces the game's XInput read. While "gw3vr_pad.txt" exists beside the game, the
    // controller state comes from it: "<buttons hex> <lx> <ly> <rx> <ry> <lt> <rt>",
    // sticks -1..1 and triggers 0..1.
    void Install();

    // If "gw3vr_shot.req" exists, writes the surface (or the backbuffer when null) to the BMP
    // path it names. A second word "full" keeps the full resolution; otherwise the image is
    // at most 2560 wide.
    void Screenshot(IDirect3DDevice9* dev, IDirect3DSurface9* surface);

    // True once after the player asked to recentre: the Home key, both sticks clicked together,
    // or a "gw3vr_recentre.req" file.
    bool RecentreRequested();

    // Stops the game pausing itself when its window is not the active one (keep_running=0 turns
    // this off). Call before anything that can take activation from the window.
    void KeepActive(HWND wnd);

    // True when gw3vr.ini has been saved since the last call, so tuning needs no restart.
    bool SettingsChanged();

    // With test_window=1 in gw3vr.ini, shrinks the game's window and keeps it from covering the desktop.
    void PlaceWindow(IDirect3DDevice9* dev);
}
