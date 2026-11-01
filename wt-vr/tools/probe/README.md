# WT:VR probe

The tool WT:VR started from: a standalone `d3d9.dll` proxy for GHWT that renders nothing differently, it just shows you what the game draws. It's useful for finding what draws what before writing code for it.

It's separate from WT:VR, and you install one or the other since both are a `d3d9.dll`. WT:VR's own `debug_tools=1` covers some of the same ground in VR: the draw finder, the band member role probe and HUD dumps. The probe works without a headset, and across every shader rather than just what's near you.

## What it does

- Logs every vertex shader with its named constants.
- Hides one vertex shader at a time, so you can see what disappears.
- Captures one frame of draws and constant values to a text file.

Output goes to a `ghwt_vr` folder next to `GHWT.exe`.

## Hotkeys

| Key | Does |
|---|---|
| F8 | Hunting mode on / off (the selected shader is hidden while on) |
| F5 / F6 | Previous / next shader |
| F7 | Mark the selected shader in the log |
| F9 | Reset the shader list to what's on screen now |
| F10 | Capture the next frame to `ghwt_vr\capture_N.txt` |

## Building

`./build.sh` (MinGW, 32-bit). `PASSTHROUGH=1 ./build.sh` builds a version that hooks nothing, to check that a proxy loads at all.
