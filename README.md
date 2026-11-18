# Geometry Wars 3: Dimensions Evolved VR

A VR mod for the Steam version of *Geometry Wars 3: Dimensions Evolved* (app 310790). The game
is drawn in true stereo with head tracking and sent to your headset through OpenXR. It is a
`d3d9.dll` that sits beside `GW3.exe`: no game files are changed, and removing the DLL gives
you the normal game back.

Played on SteamVR at 90 Hz through the menus, level select and the first Adventure levels,
with the game running at the headset's full frame rate. See [Known issues](#known-issues).

## What you get

- **Stereo 3D with head tracking.** The level floats in front of you at table-top scale; you can
  lean in and look around it. Menus are 3D scenes you stand in.
- **The HUD and menu panels** on a flat panel about a metre in front of you.
- **The game running at your headset's refresh rate.** The game normally steps its world 60 times
  a second, which judders at 90 Hz and above. The mod sets the step to the rate your headset
  reports, so everything moves on every frame. The game clock still runs in real time.
- **A steady backdrop.** The game's camera swings round the level as you move. In VR the level
  turns in front of you instead, while the scenery behind it holds still.
- **Sharper image options:** anisotropic filtering, a higher-resolution floor grid, and a control
  for the game's bloom glow.

## Install

You need the Steam version of the game, an OpenXR runtime with 32-bit support (SteamVR has it),
and Windows 10 or 11.

1. Download a release and copy `d3d9.dll`, `openxr_loader.dll` and `gw3vr.ini` into the game folder,
   usually `C:\Program Files (x86)\Steam\steamapps\common\Geometry Wars 3 - Dimensions`.
2. Start SteamVR, then start the game from Steam.
3. Use a controller. The game's window on the desktop shows the left eye.

To play flat again, set `vr=0` in `gw3vr.ini` or delete `d3d9.dll`.

If the game starts flat in the headset's place, `gw3vr.log` in the game folder says why. The usual
cause is an OpenXR runtime with no 32-bit support: set SteamVR as the active OpenXR runtime
(SteamVR settings, OpenXR).

### Linux (Steam Proton) — untested

The game is 32-bit, and Proton gained 32-bit OpenXR only in Proton Experimental (October 2026).
Older and stable Proton versions have no 32-bit OpenXR, so the game runs flat. To try it:

1. Install the mod files in the game folder as above.
2. In the game's Steam properties, set Compatibility to **Proton Experimental** and the launch
   options to `PROTON_USE_WOW64=1 %command%`.
3. Start your Linux VR runtime (SteamVR, or Monado/WiVRn), then the game.

`gw3vr.log` shows `running under Wine/Proton` and then either `OpenXR runtime: ...` (VR started)
or what was missing. Reports of either result are welcome as GitHub issues, with `gw3vr.log` attached.

**Controls added by the mod**

| Key | Does |
|---|---|
| Home, or click both sticks together | Recentre the view where you are looking |
| F9 | Save a full-resolution screenshot of both eyes and a frame dump to the game folder (for bug reports) |

## Settings

All settings are in `gw3vr.ini` beside the game. Most apply as soon as you save the file.

| Setting | Default | What it does |
|---|---|---|
| `vr` | 1 | 0 = the normal flat game |
| `render_scale` | 1 | Eye resolution as a multiple of what the headset asks for. 1.5 or 2 is sharper if your GPU allows (restart needed) |
| `sim_rate` | 0 | Game update rate. 0 = the headset's refresh rate, N = N Hz, -1 = the game's own 60 (restart needed) |
| `world_scale` | 100 | Game units per metre. Larger makes the level smaller and nearer |
| `scene_zoom` | 1.6 | How much closer you sit than the flat game's camera |
| `steady_background` | 1 | 0 = the backdrop swings round with the camera, as on a flat screen |
| `menu_scale` | 2.5 | Size of the menu world |
| `hud_distance` | 1.1 | Distance to the HUD panel, in metres |
| `hud_size` | 1 | HUD panel size (1 = the angle the game drew it at) |
| `hud_offset_y` | 0.1 | HUD panel height, in metres |
| `score_depth` | hud | Score numbers from kills: `hud` = on the HUD panel; `level` = experimental, aimed at the kill (restart needed) |
| `anisotropy` | 16 | Texture filtering for surfaces at an angle. 0 = the game's own |
| `bloom` | 1 | Strength of the game's glow. 0 = none |
| `grid_upscale` | 8 | The floor grid's line texture redrawn this many times larger. 0 = the game's own |

## Known issues

- **Score numbers** sit a little towards the middle of the view rather than exactly over the kill.
  `score_depth=level` aims them at the kill but sometimes flashes lines from the corner of the view.
- **Tested only part of the game:** menus, level select and the first Adventure levels. Bosses,
  other modes and multiplayer may show effects the mod doesn't handle yet; `gw3vr.log` names
  any it notices.
- **The game reads the keyboard even when it isn't the focused window,** so pressing keys in other
  programs can pause it. It also pauses when the controller disconnects.
- **Higher update rates slightly change some enemy behaviour,** according to PCGamingWiki (noticed
  at 300 Hz; expected to be small at 90 Hz). Set `sim_rate=-1` for the original 60 Hz.

## How it works

The game draws everything with one view-projection matrix per draw, so stereo is done by
drawing each 3D draw twice, once per eye, side by side in one enlarged render target.

- **Per-eye cameras** ([src/stereo.cpp](src/stereo.cpp)). The game's matrix is split into view and
  projection. The view is kept, your head pose is applied on top, and the headset's projection
  replaces the game's. The level camera, the menu camera, camera-fixed backdrop quads and the HUD
  canvas are told apart and each placed in its own way.
- **Render targets** ([src/hooks.cpp](src/hooks.cpp)). Every render target the game creates is
  replaced by one sized for both eyes; full-screen passes (bloom) cover both at once. Screen-space
  effects that assume one screen are adjusted per eye: the pause blur, the corner vignette (off),
  and sprites the game projects itself.
- **Frame rate** ([src/timestep.cpp](src/timestep.cpp)). The game's fixed 1/60 s step is rewritten
  in memory to 1/refresh-rate, the same edit as PCGamingWiki's "high frame rate" fix, with the rate
  read from OpenXR (`XR_FB_display_refresh_rate`).
- **Submission** ([src/vr.cpp](src/vr.cpp)). The game's Direct3D 9Ex device shares the eye texture
  with a Direct3D 11 device, which copies each half into that eye's OpenXR swapchain.

## Build

Visual Studio 2022 or newer with the C++ x86 tools, and CMake.

1. Get and build the OpenXR loader (32-bit) into `deps\`:

   ```
   git clone --depth 1 --branch release-1.1.49 https://github.com/KhronosGroup/OpenXR-SDK.git deps/openxr-sdk
   cmake -S deps/openxr-sdk -B deps/openxr-build -A Win32 -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
   cmake --build deps/openxr-build --config Release --target openxr_loader
   ```

2. Build: `build.cmd` writes `out\d3d9.dll` and `out\openxr_loader.dll`. `build.cmd deploy` also
   copies them into the game folder. Set `VS` to your Visual Studio folder and `GW3_DIR` to the
   game folder if they differ from the defaults at the top of `build.cmd`.

## Development tools

For finding out what the game draws. All are inert unless used.

| What | How |
|---|---|
| Frame dump: every device call of one frame | create `gw3vr_dump.req` in the game folder (or press F9); summarise with `python tools/frame_summary.py <dump>` |
| Frame timing trace | write a frame count to `gw3vr_trace.req`; report with `python tools/trace_report.py <csv>` |
| Shader bytecode | `dump_shaders=1`, then `python tools/ctab.py <game>\gw3vr_shaders` |
| Hide shaders while the game runs | `skip_vs=<hash>,<hash>` |
| New draw types as they first appear | `draw_census=1` |
| Drive the controller from a file | `tools/pad.sh` (`tap A`, `set ...`, `shot <name>`, `off`) |
| Small desktop window | `test_window=1` |
| Test without a headset | a 32-bit OpenXR runtime via `runtime_json=<path to its json>`; `tools/build_simulator32.sh` builds one from a local copy of the OpenXR Simulator |

The shell scripts in `tools\` contain paths from the author's machine; adjust them for yours.
[notes/status.md](notes/status.md) has the measurements and findings behind the mod.

## Credits and licences

- This mod is released under the [MIT licence](LICENSE).
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) by the Khronos Group, Apache 2.0.
  Release zips include `openxr_loader.dll` built from it, with its licence as
  `openxr_loader-LICENSE.txt`.
- The frame-rate fix follows the "High frame rate" section of the
  [PCGamingWiki page](https://www.pcgamingwiki.com/wiki/Geometry_Wars_3:_Dimensions_Evolved).
- *Geometry Wars* is a trademark of Activision. This is an unofficial fan mod, not affiliated with
  or endorsed by Activision or Lucid Games, and it contains no game code or assets.
