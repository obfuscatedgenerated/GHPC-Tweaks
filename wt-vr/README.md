# WT:VR

Guitar Hero World Tour (PC) in VR, through any OpenXR runtime. You stand on stage in the venue, in stereo with head tracking. The highway floats in front of you, with the notes coming out of it in 3D.


## Requirements

- **The [4GB patch](../4gb/) is mandatory.** Stereo rendering keeps a second copy of the game's screen-sized render targets, and GHWT runs out of address space without it.
- Guitar Hero World Tour for PC.
- A VR headset with an OpenXR runtime, running before you start the game.
  - **Tested:** Meta Quest 2 through Virtual Desktop (VDXR). Also tested to work with the [DxWnd](../dxwnd/) config.
  - **Untested:** SteamVR, Meta Quest Link and others. They should work, since only standard OpenXR is used.
- A decent GPU: every 3D draw is made twice, once for each eye. A 1660 performed okay, especially with SSW. You can tweak the in-game res and the eye scalar in the vr.ini file.

## Install

1. Apply the [4GB patch](../4gb/).
2. Copy everything from the release's `wt-vr` folder into the folder with `GHWT.exe`, except `README.md` and `tools`:
   - `d3d9.dll`
   - `openxr_loader.dll` and `openxr_loader_LICENSE.txt`
   - the `ghwt_vr` folder, which holds the height maps that make the notes 3D
3. Start your VR runtime (e.g. Virtual Desktop), then start the game.

The first run creates `ghwt_vr\vr.ini` (settings) and `ghwt_vr\vr_log.txt` (log).

To play without VR, add `-novr` to the game's launch options (via a Windows shortcut, or Steam: right-click the game → Properties → Launch Options; with DxWnd, the profile's command line). The mod then does nothing: no VR runtime starts, and the game runs exactly as it would without it. Forgetting it is mostly harmless: without a headset the mod gives up starting VR and the game draws as normal. But it may start your VR runtime (e.g. SteamVR) and its hotkeys stay active.

To uninstall, delete `d3d9.dll`, `openxr_loader.dll`, `openxr_loader_LICENSE.txt` and the `ghwt_vr` folder.

## Features

**Stereo and tracking**
- Head-tracked stereo 3D. Every 3D draw is made for both eyes each frame (`stereo_mode=1`), or the eyes take turns for speed (`stereo_mode=0`).
- The game's own camera follows your head, so its culling matches what you're looking at and scenery doesn't pop in or out at camera cuts.
- Depth of field is removed in VR.
- Crowd sprites turn to face you, not the scripted camera.
- Move, turn (snap or smooth) and change the world's scale from the keyboard.

**The highway and HUD**
- The highway and HUD float on a panel in front of you. You can change its distance, size and height.
- **3D notes:** gems, star notes and open notes get depth from height maps. Each eye sees the texture shifted sideways by the map, which gives a modelled look.
- Height maps for any other HUD texture (strikeline, flames…) are loaded the same way. See [Height maps](#height-maps).
- The notes go flat while a menu covers the highway, such as pause. They also go flat in multiplayer, since the 3D area is laid out for one highway.

**Menus**
- 3D menus are shown in VR at the game's camera. When the camera moves to another spot, you fade there or glide with it.
- Flat menus are shown on the panel.

**Where you stand**
- **Auto spot:** when a song starts you're put in your band member's place on stage, facing the crowd.
  - The part you play (guitar, bass, drums or vocals) is read from the game itself, so it follows what you picked.
  - Where each band member stands is found from what's drawn.
- **Saved spots:** F8 remembers where you stand for that venue's start camera (or for a 3D menu's camera). Shift+F8 forgets it.
- **Setlists and restarts** keep you where you stood.
- **The intro:** you watch it from its first camera, fading along with it every few seconds, then fade to your spot.

**Your band member**
- Hidden by default, including their guitar strings, so you aren't inside them.
- H picks a different band member. Shift+H switches between left alone / hidden / become them. Note that the mod will try to automatically figure out who you are by default.

**Settings and logs**
- `vr.ini` is reloaded while the game runs.
- Every hotkey can be rebound.
- Every setting in use is listed at the top of the log.

## Untested

- OpenXR runtimes other than Virtual Desktop (see [Requirements](#requirements)).
- Multiplayer. 3D notes should switch off with more than one highway on screen, and the headset player is assumed to be player 1.
- Lefty flip. If it mirrors the stage, the auto spot may put you on the wrong side.
- Drums and vocals:
  - Auto spot for the drummer. Their place is found as the centre band member behind the singer, which hasn't been checked.
  - 3D notes on the drum highway.
- Venues with no real stage, only a backdrop. A song is now also recognised by notes coming down the highway, but it's unconfirmed. Check the difficulty select screen isn't mistaken for a song, since it shows a highway too.
- Height maps for HUD pieces other than notes: the loader is in place, but no maps exist yet.
- Other GHWT builds and executables (e.g. Definitive Edition), and other screen resolutions. It was developed at 3840×2160, windowed. Its HUD measurements scale to the screen, but that's unchecked.
- Running alongside other overlays.

## Known issues

- **"Become them" (`band_member=2`) is experimental:**
  - Their animation moves them around under you.
  - Cancelling their turning (`avatar_lock_turn`) is unreliable.
  - Their guitar strings may still be offset.
- **Open notes don't look very 3D.** The trick shifts each eye's image sideways, and a long horizontal bar has almost nothing for that to show. Only the rounded ends really pop. Raise `[depth]` for those textures if you want more.
- **The notes going flat under menus is a guess:** "something big drawn over the highway". A large effect over the highway could flatten them briefly.
- **Performance:** everything is drawn twice. If it's slow, lower `render_scale` or try `stereo_mode=0`.
- Vocals camera controller doesn't start, so camera angle changes often without culling prevention
- `save_textures=1` only works in VR (because it's implemented in the heightmap pipeline)
- Band members occasionally change to chrome shading from camera LOD
- Recording studio venue is aggressively culled, ignoring the camera controller
- Star note depth map looks bad (needs to be redone as a model)

## Planned

- Make HUD follow look direction slowly (configurable)
- Highway transparency option for more immersion
- Reading more of the game's state directly instead of guessing from what's drawn:
  - song playing / intro / paused / ended
  - which players are active in multiplayer
  - lefty flip
- 3D notes on player 1's highway in multiplayer.
- Height maps for the more of the HUD (e.g. strikeline, flames, rock meter)
- Saved spots keyed by venue, not by start camera.
- Performance: a VR resolution setting, uploading frames on a background thread, drawing shadow maps once, other optimisations.
- A settings editor.
- Better method for band member mode 2 (editing meshes and animations directly, syncing guitar to tilt sensor for fake tracking)

## Using it

Recentre with **F12**. It also happens at every song start.

### Hotkeys

These are the defaults. Change them in `vr.ini` under `[keys]`, written like `save_spot=F8` or `forget_spot=Shift+F8`, or `none` to turn one off. A press only counts with exactly its modifiers held.

| Key | Does |
|---|---|
| F12 | Recentre |
| W A S D / R F | Move / up, down (hold Shift: faster) |
| Q / E | Turn (snap or smooth: `snap_turn_degrees`) |
| F2 / F3 | World scale smaller / bigger |
| Home / End | HUD panel closer / further |
| Insert / Delete | HUD panel wider / narrower |
| PageUp / PageDown | HUD panel up / down |
| F1 | HUD panel on / off |
| Shift+F1 | 3D menus in VR on / off |
| F4 | Depth of field on / off |
| F7 | HUD on the monitor: flat on top / floating / hidden |
| F8 / Shift+F8 | Save / forget your spot for this start camera (or 3D menu) |
| H | Your band member: pick the next one / none |
| Shift+H | Your band member: left alone / hidden / become them |
| Shift+[ / Shift+] | Become them: move them down / up under you |
| Ctrl+[ / Ctrl+] | Become them: move them back / forward |
| F10 | Retry starting VR if it gave up |
| F11 | Save settings to `vr.ini` |

### Settings (`ghwt_vr\vr.ini`)

Every setting is written into the file on first run. Edits apply while the game runs, apart from `render_scale`, which needs a restart.

| `[vr]` | Default | |
|---|---|---|
| `world_scale` | 1 | Game units per real metre (bigger: you feel smaller) |
| `render_scale` | 0.75 | Eye resolution, relative to what the headset asks for |
| `stereo_mode` | 1 | 1: both eyes every frame. 0: eyes take turns (faster, animation ghosts) |
| `hud_distance` / `hud_width` / `hud_height` | 1 / 1.4 / -0.1 | HUD panel placement, in metres |
| `hud_visible` | 1 | HUD panel on / off |
| `hud_depth` | 1 | 3D notes, UI panels, etc |
| `hud_depth_area` | 0.74 | How much of the screen's width (centred) HUD pieces can go 3D in; wider costs a little more performance. Restart to apply |
| `hud_gem_bulge` | 0.02 | How far the notes come out (negative turns them inside out) |
| `mirror_hud` | 1 | HUD on the monitor. 0: hidden. 1: flat on top. 2: floating like in the headset |
| `mirror_mode` | 1 | Eye view on the monitor. 0: stretch. 1: crop to fill. 2: fit |
| `vr_menus` | 1 | 3D menus in VR |
| `menu_camera` | 1 | When a 3D menu's camera moves. 0: stay. 1: fade there. 2: glide with it |
| `snap_turn_degrees` | 30 | 0: smooth turning |
| `depth_of_field_off` | 1 | No depth of field in VR |
| `crowd_follow` | 1 | Crowd sprites face you |
| `role` | auto | Which band member's place is yours. `auto` follows the part you picked in the game, or force `guitar` / `bass` / `drums` / `vocals` |
| `saved_spots` | 1 | Use your F8 spots (and keep your place in setlists and restarts) |
| `auto_spot` | 1 | Otherwise stand in your band member's place. With both off, you stay at the game's camera |
| `song_intro` | 1 | 1: watch the intro, then fade to your spot. 0: go straight there |
| `intro_hop_seconds` | 4 | While watching the intro, fade along with its camera this often (0: stay at its first camera) |
| `band_member` | 1 | 0: left alone. 1: hidden. 2: become them (experimental) |
| `walk_to_pick` | 0 | Walking into a band member makes them yours |
| `avatar_*` | | Become them: how they're placed under you |
| `near_plane` / `far_plane` | 0.05 / 600 | Clipping distances, in game units |

| `[debug]` | Default | |
|---|---|---|
| `log_level` | 1 | 0: start-up, settings and problems. 1: also songs, spots and 3D notes. 2: also the every-few-seconds diagnostics |
| `debug_tools` | 0 | Diagnostic hotkeys and automatic dumps (see below) |
| `save_textures` | 0 | Save HUD textures as `.dds`, to paint height maps from |
| `follow_head` | 1 | The game's camera follows your head |
| `wide_culling` / `culling_fov` | 0 / 130 | Widen the game's camera instead (experimental) |
| `avatar_lock_turn` | 0 | Become them: cancel their turning (experimental) |
| `eye_force_opaque` | 0 | Only for runtimes that blend the eye images' alpha |

`vr.ini` also holds your saved spots (`[spots]`) and each height map's depth (`[depth]`).

## Height maps

A height map is a greyscale `.bmp` the same size as the texture it belongs to: black is flat, white comes out the most. They live in `ghwt_vr\height_maps`, named after the texture they belong to. The name is made from the texture's contents, so it's the same in every session, e.g. `128x64_676342AC.bmp` for the regular gem.

To make one:
1. Set `save_textures=1` under `[debug]` and play. Each HUD texture that could be 3D is saved as `ghwt_vr\textures\<name>.dds`. A note's other layers are saved alongside as `<name>_layer1.dds` / `_layer2.dds`, which help to see its whole shape.
2. Paint the height map over it in an image editor.
3. Save it as `ghwt_vr\height_maps\<name>.bmp`: uncompressed 8, 24 or 32-bit.
4. It's loaded the first time that texture is drawn, so restart the game to see changes.
5. `[depth]` in `vr.ini` gets a `<name>=1.00` line for it. Raise or lower it to change that texture's depth alone.

Pieces other than notes only become 3D during songs, and only when they sit inside the middle area of the screen where the 3D panels are.

Included: the regular gem, the star-power note sheet, and the open notes (normal and beginner).

## Logs and debugging

`ghwt_vr\vr_log.txt` is the log. The previous launch's log is kept as `vr_log_previous.txt`. Send both with any bug report.

With `debug_tools=1` these extra hotkeys work:

| Key | Does |
|---|---|
| Ctrl+F7 | Log the next frame's HUD draws, and save their textures |
| Ctrl+F9 / Ctrl+F10 | Draw finder: hide the draws near you one at a time, logging each. Good for finding what draws something |
| Ctrl+F6 | Role probe: log each band member and what's drawn near them |
| F5 / Shift+F7 | Scan memory for the game's camera / log what was found |
| F6 / Shift+F6 / [ / ] | Wide culling / head follow / culling FOV |
| Shift+F9 | Test pattern: red left eye, green right eye, blue panel |

`tools/probe` is a separate, older `d3d9.dll` that shows what the game draws without VR. See its [README](tools/probe/README.md).

## Building

`d3d9.dll` must be built 32-bit, like GHWT. Both scripts fetch the OpenXR headers into `openxr-sdk` on the first build (needs git), or use `OPENXR_INCLUDE` if you point it at an OpenXR-SDK `include` folder. Output goes to `dist/`.

- **MinGW (Linux/WSL/MSYS2):** `./build.sh`. Needs `i686-w64-mingw32-g++`.
- **MSVC:** run `build.bat` from the *x86 Native Tools Command Prompt for VS*. This hasn't been tried yet, so it may need small fixes.

Neither script builds `openxr_loader.dll`: take it from a release, or from Khronos' [OpenXR-SDK-Source releases](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases) (the 32-bit `Win32` one).

The CI builds with MinGW on every push. Version tags publish a release containing the mod, Khronos' 32-bit loader and the height maps.

### Code layout (`src/`)

| File | |
|---|---|
| `ghwt_vr.cpp` | The `d3d9.dll` proxy: device hooks, per-eye cameras, the HUD panels, where you stand, your band member, sending frames to the headset |
| `openxr_session.*` | OpenXR: the session, eye images, the HUD panel layers |
| `settings.*` | `vr.ini`: one table of every setting drives loading, saving, logging and reloading |
| `input.*` | Hotkeys and their `[keys]` bindings |
| `hud_relief.*` | Height maps: texture names, loading `.bmp`s, the 3D copies of pixel shaders |
| `stereo_targets.*` | Right-eye copies of the game's screen-sized render targets |
| `camera_scan.*` | Finding the game's own camera in memory (for head follow) |
| `mesh_watch.*` | Reading posed vertices (band members are posed on the CPU) |
| `part_scan.*` | Which part player 1 plays, from the game's script data |
| `log.*` | The log and `log_level` |
| `vr_math.h` | Matrices and vectors |

- **New setting:** a field in `VrSettings` (`settings.h`) and a row in the table (`settings.cpp`).
- **New hotkey:** an `Action` (`input.h`) with its default key (`input.cpp`), then check `ActionPressed(...)` in `HandleHotkeys`.

## About the code

Most of this was written by an AI (Claude), not by me. Direct3D hooking and OpenXR are well outside what I know, and this wouldn't exist otherwise. I directed it, tested every build and did the reverse engineering legwork in game. It isn't how I usually write code, and it isn't something I'd do for anything I sold.

That also means I know the code less well than I would otherwise. Bug reports are welcome, especially with `vr_log.txt` attached, but I may not be able to fix everything quickly myself. Pull requests are very welcome, and feel free to tidy up the code if need be, so long as everything still works.
