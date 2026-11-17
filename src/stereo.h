// Turns the game's view-projection matrix into one per eye.
#pragma once
#include "vr.h"

namespace stereo
{
    void LoadSettings();

    // The eyes for the frame about to be drawn. "tracked" is false for the stand-in eyes used
    // while no headset pose is available.
    void SetEyes(const Eye eyes[2], bool tracked);

    // Makes the head's current position and heading the seat on the next tracked frame.
    void Recentre();

    // The matrix the game wrote to vertex constants c0-c3 (four rows of four).
    void SetGameMatrix(const float* rows);
    bool HaveMatrix();

    // The replacement for c0-c3 for one eye (0 left, 1 right). "overlay" is true for draws made
    // after post-processing, straight to the frame: the HUD and menu panels.
    // "backdrop" is true for the shaders that draw the scenery behind a level.
    const float* EyeMatrix(int eye, bool overlay, bool backdrop);

    // For geometry the game projected itself, with the camera of the matrix it last set: the
    // matrix taking the game's clip-space position to this eye's, and (a, b) of the game's
    // depth mapping clip z = a * w + b, from which w is recovered. False when unavailable.
    bool Reproject(int eye, float matrix[16], float depth[4]);

    // For full-screen effects measured from the middle of the screen: where straight ahead from
    // the seat lands in one eye's image (u, v in 0..1), and how many of the game's screen
    // widths and heights that image spans.
    void ScreenCentre(int eye, float* u, float* v, float* scaleU, float* scaleV);

    // After EyeMatrix for a backdrop draw: the camera values that match the steady view, for
    // the constants the game feeds its backdrop shaders. False when the steady view is not in use.
    struct SteadyValues
    {
        const float* position;  // c4 viewPosition
        const float* direction; // c5 viewDirection
        const float* right;     // c7 backgroundParameters1: the axis sprites are widened along
        const float* up;        // c8 backgroundParameters2
    };
    bool SteadyCamera(SteadyValues* out);
}
