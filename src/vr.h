// OpenXR session and frame submission.
#pragma once
#include "common.h"

// One eye for the frame being rendered: head pose in the seated (LOCAL) space and the
// tangents of the field-of-view half angles.
struct Eye
{
    float tanL, tanR, tanU, tanD; // left and down are negative
    float pos[3];                 // metres
    float rot[4];                 // quaternion x y z w
};

namespace vr
{
    // Timing of the last frame's OpenXR calls, for the frame trace.
    struct Timing
    {
        double waitMs, beginMs, locateMs;   // xrWaitFrame, xrBeginFrame, xrLocateViews
        double copyMs, endMs;               // eye copies with swapchain waits, xrEndFrame
        long long displayTime, displayPeriod; // predicted, nanoseconds
        bool shouldRender;
    };
    const Timing& LastTiming();

    // The headset's refresh rate from XR_FB_display_refresh_rate, or 0 when the runtime does not
    // offer that extension.
    float RefreshRate();

    // Creates the instance, the D3D11 device and the session. False when no headset or runtime
    // is available; the game then runs flat and untouched.
    bool Init();
    bool Active();
    unsigned EyeWidth();
    unsigned EyeHeight();

    // Waits for the runtime's frame slot and returns this frame's eyes. False when the headset
    // gave no pose and the eyes are stand-ins.
    bool BeginFrame(Eye eyes[2]);

    // Submits the side-by-side image (a D3D9Ex shared texture, left eye in the left half).
    // The caller has already flushed D3D9.
    void EndFrame(HANDLE sharedSbs);
}
