// Per-eye view-projection matrices.
//
// The game writes one matrix to c0-c3: projection * view, rows in registers, so
// clip = M * position. Two cameras use it:
//   scene  - a moving camera with a perspective projection (levels, menu backdrop)
//   screen - the same kind of projection with an identity view (menus, HUD), drawn as
//            geometry standing in front of that camera
// Both are taken apart into view and projection here. The view is kept, so the player sits
// where the game's camera is; the head pose is applied on top and the headset's own
// projection replaces the game's.
//
// Matrices are 4x4, row-major, for column vectors.
#include "stereo.h"
#include <cmath>

namespace
{
    struct Mat { double m[16]; };

    Mat Mul(const Mat& a, const Mat& b)
    {
        Mat r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                double s = 0;
                for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }

    double Len3(const double* v) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

    // Settings (gw3vr.ini).
    double g_worldScale;   // game units per metre for the scene camera
    double g_screenScale;  // game units per metre for the menu/HUD camera
    double g_hudSize;      // 1 = HUD spans the game's own field of view
    double g_sceneOffset[3];
    double g_screenOffset[3];

    Eye g_eyes[2];

    // The seat: where the head was, and which way it faced, at the last recentre. Eye poses
    // are taken relative to it, so the game's camera sits at the player's resting head.
    bool g_haveSeat;
    bool g_recentre;
    double g_seat[3];
    double g_seatYaw;
    float g_game[16];
    bool g_have;
    bool g_dirty;
    float g_out[2][16];

    // Projection scales of the last scene camera, used to lay the HUD over the same field of view.
    double g_sceneSx = 1.0, g_sceneSy = 1.8;
    bool g_loggedOrtho;

    // World-to-eye for one eye, from its pose in the seated space.
    Mat EyeView(const Eye& e)
    {
        double x = e.rot[0], y = e.rot[1], z = e.rot[2], w = e.rot[3];
        double r[9] = {
            1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
        Mat v{};
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++) v.m[i * 4 + j] = r[j * 3 + i]; // transpose
            v.m[i * 4 + 3] = -(r[0 * 3 + i] * e.pos[0] + r[1 * 3 + i] * e.pos[1] + r[2 * 3 + i] * e.pos[2]);
        }
        v.m[15] = 1;
        return v;
    }

    void Compute()
    {
        g_dirty = false;
        double r[4][4];
        for (int i = 0; i < 16; i++) r[i / 4][i % 4] = g_game[i];

        double wl = Len3(r[3]);
        double lx = Len3(r[0]), ly = Len3(r[1]);
        if (wl < 1e-6 || lx < 1e-9 || ly < 1e-9)
        {
            // Orthographic: no depth to work with. Draw it unchanged in both eyes.
            if (!g_loggedOrtho) { Log("stereo: an orthographic matrix was left alone"); g_loggedOrtho = true; }
            for (int e = 0; e < 2; e++) memcpy(g_out[e], g_game, sizeof g_game);
            return;
        }

        // View: unit rows for right, up and forward, each with its translation.
        Mat view{};
        for (int j = 0; j < 4; j++)
        {
            view.m[0 * 4 + j] = r[0][j] / lx;
            view.m[1 * 4 + j] = r[1][j] / ly;
            view.m[2 * 4 + j] = r[3][j] / wl;
        }
        view.m[15] = 1;
        double sx = lx / wl, sy = ly / wl;

        // Depth: clip z = a * w + b in the game's projection.
        double a = (r[2][0] * r[3][0] + r[2][1] * r[3][1] + r[2][2] * r[3][2]) / (wl * wl);
        double b = r[2][3] - a * r[3][3];

        bool screen = fabs(view.m[0] - 1) < 1e-4 && fabs(view.m[5] - 1) < 1e-4 && fabs(view.m[10] - 1) < 1e-4 &&
                      fabs(view.m[3]) < 1e-3 && fabs(view.m[7]) < 1e-3 && fabs(view.m[11]) < 1e-3;
        double scale, kx = 1, ky = 1;
        const double* offset;
        if (screen)
        {
            scale = g_screenScale;
            kx = sx / g_sceneSx * g_hudSize;
            ky = sy / g_sceneSy * g_hudSize;
            offset = g_screenOffset;
        }
        else
        {
            scale = g_worldScale;
            g_sceneSx = sx;
            g_sceneSy = sy;
            offset = g_sceneOffset;
        }

        // Game camera space (left-handed, z forward, game units) to the seated space
        // (right-handed, -z forward, metres).
        Mat conv{};
        conv.m[0] = kx / scale;  conv.m[3] = offset[0];
        conv.m[5] = ky / scale;  conv.m[7] = offset[1];
        conv.m[10] = -1 / scale; conv.m[11] = -offset[2];
        conv.m[15] = 1;
        Mat seated = Mul(conv, view);

        double s = scale * wl; // keeps clip w in the game's units, for shaders that fade by depth
        for (int e = 0; e < 2; e++)
        {
            const Eye& eye = g_eyes[e];
            Mat proj{};
            proj.m[0] = s * 2 / (eye.tanR - eye.tanL);
            proj.m[2] = s * (eye.tanR + eye.tanL) / (eye.tanR - eye.tanL);
            proj.m[5] = s * 2 / (eye.tanU - eye.tanD);
            proj.m[6] = s * (eye.tanU + eye.tanD) / (eye.tanU - eye.tanD);
            proj.m[10] = -a * s;
            proj.m[11] = b;
            proj.m[14] = -s;
            Mat out = Mul(proj, Mul(EyeView(eye), seated));
            for (int i = 0; i < 16; i++) g_out[e][i] = (float)out.m[i];
        }
    }
}

void stereo::LoadSettings()
{
    g_worldScale = IniFloat("world_scale", 250.0f);
    g_screenScale = IniFloat("screen_scale", 500.0f);
    g_hudSize = IniFloat("hud_size", 1.0f);
    g_sceneOffset[0] = IniFloat("scene_offset_x", 0);
    g_sceneOffset[1] = IniFloat("scene_offset_y", 0);
    g_sceneOffset[2] = IniFloat("scene_offset_z", 0);
    g_screenOffset[0] = IniFloat("screen_offset_x", 0);
    g_screenOffset[1] = IniFloat("screen_offset_y", 0);
    g_screenOffset[2] = IniFloat("screen_offset_z", 0);
    Log("stereo: world_scale %g screen_scale %g hud_size %g", g_worldScale, g_screenScale, g_hudSize);
}

void stereo::Recentre() { g_recentre = true; }

void stereo::SetEyes(const Eye eyes[2], bool tracked)
{
    g_dirty = true;
    if (!tracked)
    {
        g_eyes[0] = eyes[0];
        g_eyes[1] = eyes[1];
        return;
    }
    if (!g_haveSeat || g_recentre)
    {
        const float* q = eyes[0].rot;
        // Forward is the rotated -z axis; only its heading is kept, so the horizon stays level.
        double fx = -2.0 * (q[0] * q[2] + q[1] * q[3]);
        double fz = -(1.0 - 2.0 * (q[0] * q[0] + q[1] * q[1]));
        g_seatYaw = atan2(-fx, -fz);
        for (int i = 0; i < 3; i++) g_seat[i] = (eyes[0].pos[i] + eyes[1].pos[i]) * 0.5;
        g_haveSeat = true;
        g_recentre = false;
        Log("stereo: recentred at %.2f %.2f %.2f, heading %.0f degrees", g_seat[0], g_seat[1], g_seat[2], g_seatYaw * 57.2958);
    }
    double c = cos(-g_seatYaw), sn = sin(-g_seatYaw);
    double hy = sin(-g_seatYaw * 0.5), hw = cos(-g_seatYaw * 0.5); // quaternion (0, hy, 0, hw)
    for (int e = 0; e < 2; e++)
    {
        g_eyes[e] = eyes[e];
        double x = eyes[e].pos[0] - g_seat[0], y = eyes[e].pos[1] - g_seat[1], z = eyes[e].pos[2] - g_seat[2];
        g_eyes[e].pos[0] = (float)(x * c + z * sn);
        g_eyes[e].pos[1] = (float)y;
        g_eyes[e].pos[2] = (float)(-x * sn + z * c);
        const float* q = eyes[e].rot;
        g_eyes[e].rot[0] = (float)(hw * q[0] + hy * q[2]);
        g_eyes[e].rot[1] = (float)(hw * q[1] + hy * q[3]);
        g_eyes[e].rot[2] = (float)(hw * q[2] - hy * q[0]);
        g_eyes[e].rot[3] = (float)(hw * q[3] - hy * q[1]);
    }
}

void stereo::SetGameMatrix(const float* rows)
{
    memcpy(g_game, rows, sizeof g_game);
    g_have = true;
    g_dirty = true;
}

bool stereo::HaveMatrix() { return g_have; }

const float* stereo::EyeMatrix(int eye)
{
    if (g_dirty) Compute();
    return g_out[eye];
}
