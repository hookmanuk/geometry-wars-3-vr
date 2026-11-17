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
    const float* EyeMatrix(int eye, bool overlay);
}
