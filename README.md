# Geometry Wars 3: Dimensions Evolved in VR

A VR mod for the Steam version (app 310790, 32-bit, Direct3D 9). It is a `d3d9.dll` that sits
beside `GW3.exe`, renders the game once per eye with head tracking, and sends the picture to
the headset through OpenXR. No game files are changed.

Status: runs on the OpenXR Simulator at 90 FPS with menus, level select, the first Adventure
level, pause and tutorial screens checked. **Not yet tried in a real headset.** See
[notes/status.md](notes/status.md) for what was measured and what is still open.

## Install

Copy three files into the game folder (`...\steamapps\common\Geometry Wars 3 - Dimensions`):

- `out\d3d9.dll`
- `out\openxr_loader.dll`
- `dist\gw3vr.ini`

Start SteamVR (or another OpenXR runtime with a 32-bit build), then start the game from Steam.
The game's window shows the left eye. To go back to the flat game, set `vr=0` in `gw3vr.ini` or
delete `d3d9.dll`.

- Recentre: **Home**, or click both sticks together.
- Settings are in `gw3vr.ini` and apply as soon as the file is saved.
- `gw3vr.log` in the game folder records start-up, the frame rate and frame cost every 600
  frames, and anything that stalls.

## How it works

The game is regular: every vertex shader takes one view-projection matrix in constants
`c0`-`c3`, and the frame is scene, a bloom chain of full-screen passes, then HUD.

- **Both eyes side by side.** Every render target the game creates is replaced by one twice
  the eye width, and the backbuffer by a shared texture. Draws that use the matrix run twice,
  once in each half with that eye's matrix. Full-screen passes run once and cover both eyes.
- **Per-eye matrices** ([src/stereo.cpp](src/stereo.cpp)). The game's matrix is split into
  view and projection. The view is kept, the head pose is applied on top, and the headset's
  projection replaces the game's. Four kinds of camera are told apart and placed differently:
  the level, the menu world, backdrop quads fixed to the camera, and the 640x360 canvas that
  holds the HUD and menu panels.
- **Steady backdrop.** The game's camera circles the level as the ship moves. In VR the level
  turns in front of you, and the backdrop shaders are given a camera that keeps its original
  direction, so the surroundings hold still.
- **Screen-space effects.** The corner vignette is switched off, the pause blur is drawn per
  eye around the straight-ahead point, and sprites the game projects on the CPU are
  un-projected and re-projected for each eye.
- **Submission** ([src/vr.cpp](src/vr.cpp)). The device is D3D9Ex, so the eye texture is
  shared with a D3D11 device, which draws each half into that eye's OpenXR swapchain.

## Build

Visual Studio 2026 Insiders with the x86 tools. `build.cmd` builds `out\d3d9.dll`;
`build.cmd deploy` also copies it and the loader into the game folder.

Dependencies go in `deps\` (not in git):

```
git clone --depth 1 --branch release-1.1.49 https://github.com/KhronosGroup/OpenXR-SDK.git deps/openxr-sdk
cmake -S deps/openxr-sdk -B deps/openxr-build -G "Visual Studio 18 2026" -A Win32 -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build deps/openxr-build --config Release --target openxr_loader
```

## Testing without a headset

The game is 32-bit, so it needs a 32-bit OpenXR runtime. [tools/build_simulator32.sh](tools/build_simulator32.sh)
builds a 32-bit copy of the OpenXR Simulator into `deps\simulator32`. Point the mod at it with
`runtime_json=H:/gw3-vr/deps/simulator32/bin/openxr_simulator.json` in `gw3vr.ini`.

Test-only settings and files (all inert unless used):

| What | How |
|---|---|
| Small window that stays out of the way | `test_window=1` |
| Drive the controller | `tools/pad.sh tap A`, `tools/pad.sh set <buttons> lx ly rx ry lt rt`, `tools/pad.sh off` |
| Screenshot of both eyes | `tools/pad.sh shot <name>` (PNG in `scratch\`) |
| Launch and drive into the first level | `tools/to_level.sh <prefix> [title\|select\|tutorial\|level]` |
| Dump one frame's draw calls | create `gw3vr_dump.req` in the game folder, then `python tools/frame_summary.py <dump>` |
| Dump shader bytecode | `dump_shaders=1`, then `python tools/ctab.py <game>\gw3vr_shaders` |
| Head pose | `python F:\rpsc3\source\plans\tools\re\simpose.py YAW PITCH ROLL`, then create `gw3vr_recentre.req` once |
