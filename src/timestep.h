// The game's fixed simulation step, raised to the headset's refresh rate.
#pragma once
#include "common.h"

namespace timestep
{
    // Sets the rate the game simulates at (ticks per second). Every 1/60 constant in the game's
    // loaded image is replaced by 1/rate, the same edit PCGamingWiki describes for GW3.exe, done
    // in memory so the file on disk is untouched. A background thread repeats the scan so values
    // the game sets up later are caught too. Calling it again with the same rate does nothing.
    void Apply(double rate);
}
