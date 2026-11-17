# Status and findings

Last updated 2026-11-17.

## Where it stands

Everything below was checked on a 32-bit build of the OpenXR Simulator (Quest 3 profile,
2880x2880 per eye, 90 Hz), driven by the injected controller. **Nothing has been seen in a
real headset or on SteamVR yet**, so comfort, depth, scale and pacing on the real compositor
are unjudged.

| Screen | Result |
|---|---|
| Title and main menus | Stereo, panels and backdrop correct |
| Level select (world map) | Stereo, fuses |
| Level results | Stereo, correct |
| Adventure level 1, "The Beginning" | Playable: ship, shots, gates, particles, HUD in both eyes |
| Tutorial pop-up and its blur | Correct per eye |
| Flat game with `vr=0` | Unchanged |

Head turns, with the simulator's real head pose (`simpose.py`), at centre, yaw -20 and +20,
pitch 10 and roll 15: in the level and on level select, the scene, backdrop, HUD and panels
all move together as one rigid picture. Nothing follows the head and no layer slides against
another.

Not tested: every level after the first (locked on this save), Bonus and Ultimate modes,
bosses, bombs and super states, multiplayer and co-op, the options screens in VR, a
keyboard-and-mouse player.

## Frame rate

RTX 5090, 2880x2880 per eye (5760x2880 targets), simulator pacing at 90 Hz, level 1 with the
ship moving and firing, 600-frame windows:

| Window | FPS | Frame cost avg | Worst 1% | Max |
|---|---|---|---|---|
| typical | 90.0 | 1.5 ms | 1.9 ms | 2.5 ms |
| heaviest seen | 90.0 | 1.6 ms | 2.4 ms | 6.6 ms |

Frame cost is the time from the runtime handing over a frame slot to the GPU finishing the
game's drawing, so it is what has to fit inside a refresh interval (11.1 ms at 90 Hz, 6.9 ms
at 144 Hz). The first window after launch includes loading and one 1 s hitch.

The game's clock is right at 90 FPS: 11 s of level timer passed in 11.3 s (including the two
screenshots). No frame-rate patch is needed.

## What the game does (from frame dumps)

- 32-bit, Direct3D 9Ex, shader model 3. Steam DRM wraps the executable, which a `d3d9.dll`
  beside it does not disturb.
- Every vertex shader but two names `worldViewProjection` at `c0`-`c3`. It is view-projection:
  the world matrix is separate (`transformMatrix`, `c13`). `c4` is the camera position, `c5`
  its direction.
- Frame: scene into a full-size target, bloom (downsample, blur across, blur down, add), a
  final pass with a colour offset and corner vignette to the backbuffer, then the HUD straight
  onto the backbuffer.
- Cameras:

  | Kind | Seen as | Used for |
  |---|---|---|
  | level | 36 degrees, near 100, far 4000, circling the origin 560-660 units away | level, its backdrop, enemies, shots |
  | menu | 55 degrees, near 1, far 10000, about 4 units above the floor | menu backdrop, world map |
  | backdrop | identity view, 36 degrees, near 100 | a glow quad fixed to the camera, menus only |
  | canvas | 45 degrees, near 1, looking at a 640x360 canvas from 434.6 units | all HUD and menu panels |

- The level camera circles the level as the ship moves, and looks slightly towards the ship.
- The two shaders without the matrix: `vs_39041d6b` is the full-screen quad; `vs_b8717a0e`
  takes positions the game projected on the CPU (the ship's spawn flash).
- The game stops its main loop while its window is inactive. With VR on the mod hides
  deactivation from it (`keep_running=0` turns that off).
- A ReShade 32-bit OpenXR layer on this PC fails to load and makes the loader refuse to create
  an instance. The mod then retries with implicit layers disabled.

## Fixed 2026-11-17 (second pass)

- **Judder.** The game steps its world by a fixed 1/60 s. At 90 Hz that is two updates per
  three frames, so every third frame repeated the last (a 30 Hz stutter). Frame delivery was
  never the problem: on SteamVR 798 of 800 frames landed on consecutive vsyncs with ~7 ms spare.
  The mod now writes 1/rate over every 1/60 constant in the loaded image (the PCGamingWiki
  "High frame rate" hex edit, done in memory) with the rate from `XR_FB_display_refresh_rate`
  (SteamVR reports 90.00). Result: the camera moves on every frame, and the level clock still
  runs at real time (12 s in 12.1 s). `sim_rate` overrides. Do not derive the rate from
  `predictedDisplayPeriod`: SteamVR varies it (45, 30 Hz during loading). Trace in
  `notes/traces/`.
- **Score numbers deep in the background.** They are drawn with the HUD text shader
  (`vs_fe5eb358`/`ps_12e27094`) and the HUD canvas camera, but in the scene pass, so they get
  bloom. That camera (near plane 1) was taken for the menu world and placed ~170 m away. An
  unrotated camera with an offset, outside the final pass, is now the `popup` kind: placed at
  `hud_distance`, spread by `scene_zoom` so a number stays over its kill. Checked by eye
  disparity: the number and the HUD now shift identically between the eyes.
- PCGamingWiki notes the higher update rate slightly changes some enemy behaviour (seekers and
  chargers track a little faster; the Turret ability fires more; crashes with Turret on green
  walls at 300 Hz). At 90 Hz the effect should be small; not yet checked here.

## Open

1. **Headset run on SteamVR.** Scale (`world_scale`), distance (`scene_zoom`), HUD distance
   and the steady backdrop are first guesses made from screenshots.
2. **Bloom crosses the seam.** The blur passes sample across the middle of the side-by-side
   image, so strong glow at the inner edge of one eye can leak a little into the other.
   Fix: draw the two blur passes per eye with clamped sampling.
3. **Half-pixel offset.** Full-screen quads carry an offset computed for the game's own window
   size. With a small mirror window the picture and its bloom are a few pixels out at eye
   resolution. Unnoticeable with a 4K window. Fix: a replacement full-screen vertex shader.
4. **Other levels and modes** may use shaders, cameras or effects not seen yet. The log names
   any shader that draws into the eyes without the matrix ("unhandled").
5. The two "unhandled" lines at frame 1 are start-up draws made before any camera exists and
   are harmless.
6. Anti-aliasing: the game has none. `render_scale` above 1 supersamples; the cost is low.
