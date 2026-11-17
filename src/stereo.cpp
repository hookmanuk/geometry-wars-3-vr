// Per-eye view-projection matrices.
//
// The game writes one matrix to c0-c3: projection * view, rows in registers, so
// clip = M * position. It is taken apart into view and projection here. The view is kept, so
// the player sits where the game's camera is; the head pose is applied on top and the
// headset's own projection replaces the game's.
//
// The game uses four kinds of camera, and each is placed differently:
//   level    - looks at the level from a few hundred units away (near plane 100). Shown at
//              world_scale, and moved towards the level by scene_zoom so it fills the view.
//   menu     - the menu backdrop: a camera a few units above a grid floor (near plane 1).
//              Shown at menu_scale, which makes the floor a floor.
//   backdrop - an identity view inside the scene pass: a quad fixed to the game's camera.
//              Kept as far-off scenery covering the game's own frame.
//              With steady_background on, the backdrop's shaders get a camera that keeps
//              the direction it had when the level began: the game's camera circles the
//              level as the ship moves, and without this the whole backdrop would swing
//              round the player. The level turns in front of a still backdrop instead.
//   canvas   - everything drawn after post-processing: HUD and menu panels on a 640x360
//              canvas. Shown as a panel hud_distance metres away.
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

    // General 4x4 inverse by cofactors. False when the matrix is singular.
    bool Invert(const Mat& in, Mat& out)
    {
        const double* m = in.m;
        double inv[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (fabs(det) < 1e-30) return false;
        for (int i = 0; i < 16; i++) out.m[i] = inv[i] / det;
        return true;
    }

    // Settings (gw3vr.ini).
    double g_worldScale;    // level camera: game units per metre
    double g_sceneZoom;     // level camera: how many times closer the player sits than the game's camera
    double g_menuScale;     // menu camera: game units per metre
    double g_backdropScale; // backdrop quads: game units per metre
    double g_hudDistance;   // canvas: metres from the seat to the panel
    double g_hudSize;       // canvas: 1 = the angle the game drew it at
    double g_hudOffsetY;    // canvas: metres up
    double g_nearMetres;    // closest distance drawn
    bool g_steadyBackground;

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
    bool g_overlay;         // the matrix in g_out was made for the canvas pass
    const char* g_kind = "none";
    bool g_steady;          // the matrix in g_out was made for a backdrop draw

    // The level camera's direction when the level began (rows: right, up, forward), and the
    // camera position and direction that go with the steady view.
    unsigned g_frame, g_lastLevelFrame;
    double g_ref[9];
    bool g_steadyValid;
    float g_steadyPos[4], g_steadyDir[4], g_steadyRight[4], g_steadyUp[4];
    float g_out[2][16];

    // Projection scales of the last scene camera, used to lay the HUD over the same field of view.
    double g_sceneSx = 1.0, g_sceneSy = 1.8;
    // Half-width and half-height of the game's own frame as the player sees it (tangents).
    double g_frameTanX = 0.58, g_frameTanY = 0.33;
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

        // Depth: clip z = a * w + b in the game's projection, with w = wl * depth.
        double a = (r[2][0] * r[3][0] + r[2][1] * r[3][1] + r[2][2] * r[3][2]) / (wl * wl);
        double b = r[2][3] - a * r[3][3];
        double nearUnits = a != 0 ? -b / a / wl : 1;
        double farUnits = fabs(a - 1) > 1e-9 ? nearUnits * a / (a - 1) : 1e9;

        bool identity = fabs(view.m[0] - 1) < 1e-4 && fabs(view.m[5] - 1) < 1e-4 && fabs(view.m[10] - 1) < 1e-4 &&
                        fabs(view.m[3]) < 1e-3 && fabs(view.m[7]) < 1e-3 && fabs(view.m[11]) < 1e-3;

        // Game camera space (left-handed, z forward, game units) to the seated space
        // (right-handed, -z forward, metres): x' = cx x, y' = cy y + oy, z' = -cz (z - shift).
        double cx, cy, cz, oy = 0, shift = 0;
        if (g_overlay)
        {
            g_kind = "canvas";
            // The canvas lies in the world's z = 0 plane; its depth is the origin's depth.
            double plane = view.m[11] > 1 ? view.m[11] : 434.6;
            cz = g_hudDistance / plane;
            cx = cy = cz * g_hudSize;
            oy = g_hudOffsetY;
        }
        else if (identity)
        {
            g_kind = "backdrop";
            cz = 1 / g_backdropScale;
            cx = cz * sx / g_sceneSx;
            cy = cz * sy / g_sceneSy;
        }
        else if (nearUnits < 50)
        {
            g_kind = "menu";
            cx = cy = cz = 1 / g_menuScale;
            g_sceneSx = sx;
            g_sceneSy = sy;
            g_frameTanX = 1 / sx;
            g_frameTanY = 1 / sy;
        }
        else
        {
            g_kind = "level";
            cx = cy = cz = 1 / g_worldScale;
            g_sceneSx = sx;
            g_sceneSy = sy;
            g_frameTanX = g_sceneZoom / sx;
            g_frameTanY = g_sceneZoom / sy;
            if (g_frame - g_lastLevelFrame > 1)
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) g_ref[i * 3 + j] = view.m[i * 4 + j];
            g_lastLevelFrame = g_frame;
            g_steadyValid = g_steadyBackground;
            if (g_steady && g_steadyBackground)
            {
                // Same distance from the world's centre, original direction, centre on the axis.
                double dist = sqrt(view.m[3] * view.m[3] + view.m[7] * view.m[7] + view.m[11] * view.m[11]);
                for (int i = 0; i < 3; i++)
                {
                    for (int j = 0; j < 3; j++) view.m[i * 4 + j] = g_ref[i * 3 + j];
                    view.m[i * 4 + 3] = i == 2 ? dist : 0;
                    g_steadyPos[i] = (float)(-dist * g_ref[6 + i]);
                    g_steadyDir[i] = (float)g_ref[6 + i];
                    g_steadyRight[i] = (float)g_ref[i];
                    g_steadyUp[i] = (float)g_ref[3 + i];
                }
                g_steadyRight[3] = g_steadyUp[3] = 0;
                g_steadyPos[3] = 1;
                g_steadyDir[3] = 0;
            }
            // Sit closer to the point the camera looks at: the level's centre, when the
            // world origin is in front of the camera and near its axis.
            double depth = view.m[11];
            if (depth > 0 && fabs(view.m[3]) < 0.5 * depth && fabs(view.m[7]) < 0.5 * depth)
                shift = depth * (1 - 1 / g_sceneZoom);
        }
        Mat conv{};
        conv.m[0] = cx;
        conv.m[5] = cy;   conv.m[7] = oy;
        conv.m[10] = -cz; conv.m[11] = cz * shift;
        conv.m[15] = 1;
        Mat seated = Mul(conv, view);

        // Depth range: the game's far plane, and a near plane no further than g_nearMetres.
        double nearClip = nearUnits < g_nearMetres / cz ? nearUnits : g_nearMetres / cz;
        double A = farUnits / (farUnits - nearClip);
        double B = -A * nearClip * wl;

        double sw = wl / cz; // clip w = wl * depth in game units, as the game's shaders expect
        for (int e = 0; e < 2; e++)
        {
            const Eye& eye = g_eyes[e];
            Mat proj{};
            proj.m[0] = sw * 2 / (eye.tanR - eye.tanL);
            proj.m[2] = sw * (eye.tanR + eye.tanL) / (eye.tanR - eye.tanL);
            proj.m[5] = sw * 2 / (eye.tanU - eye.tanD);
            proj.m[6] = sw * (eye.tanU + eye.tanD) / (eye.tanU - eye.tanD);
            proj.m[10] = -A * sw;
            proj.m[11] = B;
            proj.m[14] = -sw;
            Mat out = Mul(proj, Mul(EyeView(eye), seated));
            for (int i = 0; i < 16; i++) g_out[e][i] = (float)out.m[i];
        }
    }
}

void stereo::LoadSettings()
{
    g_worldScale = IniFloat("world_scale", 200.0f);
    g_sceneZoom = IniFloat("scene_zoom", 1.6f);
    g_menuScale = IniFloat("menu_scale", 2.5f);
    g_backdropScale = IniFloat("backdrop_scale", 10.0f);
    g_hudDistance = IniFloat("hud_distance", 1.1f);
    g_hudSize = IniFloat("hud_size", 1.0f);
    g_hudOffsetY = IniFloat("hud_offset_y", 0.0f);
    g_nearMetres = IniFloat("near_metres", 0.1f);
    g_steadyBackground = IniInt("steady_background", 1) != 0;
    g_dirty = true;
    Log("stereo: world_scale %g scene_zoom %g menu_scale %g hud_distance %g hud_size %g",
        g_worldScale, g_sceneZoom, g_menuScale, g_hudDistance, g_hudSize);
}

void stereo::Recentre() { g_recentre = true; }

void stereo::SetEyes(const Eye eyes[2], bool tracked)
{
    g_dirty = true;
    g_frame++;
    if (g_frame - g_lastLevelFrame > 1) g_steadyValid = false;
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

bool stereo::Reproject(int eye, float matrix[16], float depth[4])
{
    if (!g_have) return false;
    const float* out = EyeMatrix(eye, false, false);
    Mat game, inverse, eyeMatrix;
    for (int i = 0; i < 16; i++) { game.m[i] = g_game[i]; eyeMatrix.m[i] = out[i]; }
    if (!Invert(game, inverse)) return false;
    Mat m = Mul(eyeMatrix, inverse);
    for (int i = 0; i < 16; i++) matrix[i] = (float)m.m[i];
    // clip z = a * w + b in the game's projection (see Compute).
    double wl2 = g_game[12] * g_game[12] + g_game[13] * g_game[13] + g_game[14] * g_game[14];
    if (wl2 < 1e-12) return false;
    double a = (g_game[8] * g_game[12] + g_game[9] * g_game[13] + g_game[10] * g_game[14]) / wl2;
    double b = g_game[11] - a * g_game[15];
    depth[0] = (float)a;
    depth[1] = (float)b;
    depth[2] = depth[3] = 0;
    return true;
}

void stereo::ScreenCentre(int eye, float* u, float* v, float* scaleU, float* scaleV)
{
    const Eye& e = g_eyes[eye];
    // The seat's forward direction, in this eye's axes.
    Mat view = EyeView(e);
    double dx = -view.m[2], dy = -view.m[6], dz = -view.m[10];
    double depth = -dz > 0.05 ? -dz : 0.05;
    double ndcX = (2 * dx / depth - (e.tanR + e.tanL)) / (e.tanR - e.tanL);
    double ndcY = (2 * dy / depth - (e.tanU + e.tanD)) / (e.tanU - e.tanD);
    *u = (float)(0.5 + 0.5 * ndcX);
    *v = (float)(0.5 - 0.5 * ndcY);
    *scaleU = (float)((e.tanR - e.tanL) / (2 * g_frameTanX));
    *scaleV = (float)((e.tanU - e.tanD) / (2 * g_frameTanY));
}

bool stereo::SteadyCamera(SteadyValues* out)
{
    if (!g_steadyValid) return false;
    out->position = g_steadyPos;
    out->direction = g_steadyDir;
    out->right = g_steadyRight;
    out->up = g_steadyUp;
    return true;
}

const char* stereo::LastKind() { return g_kind; }

const float* stereo::EyeMatrix(int eye, bool overlay, bool backdrop)
{
    if (overlay != g_overlay || backdrop != g_steady) { g_overlay = overlay; g_steady = backdrop; g_dirty = true; }
    if (g_dirty) Compute();
    return g_out[eye];
}
