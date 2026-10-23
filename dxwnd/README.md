# DxWnd Configs

[DxWnd](https://sourceforge.net/projects/dxwnd/) configs for Guitar Hero III and Guitar Hero World Tour.

## What they do

- **Borderless window at native resolution**: the game thinks it's fullscreen, but your monitor never changes mode, so no rescaling or alt-tab flicker. Adapts to whatever resolution your desktop is running.
- **Uncapped FPS and much lower input lag**: windowing removes the exclusive-fullscreen vsync and frame queue that both games normally use.
- **Clean exit**: closing the window fully ends the game.

## Requirements

- [DxWnd](https://sourceforge.net/projects/dxwnd/files/latest/download) **v2.06.10** or newer (what these were made with, other versions may read some settings differently)

## Install

1. Open DxWnd and import `GH3.dxw` and/or `GHWT.dxw`.
2. If DxWnd asks where the game is, pick your `GH3.exe` / `GHWT.exe`. Otherwise, open the entry and set **Path** on the Main tab.
3. Launch the game from DxWnd.

## Launching from Steam

Add `dxwnd.exe` as a non-Steam game and set:

- **Launch options:** `/R:N`, where N is the entry's number in DxWnd (Target #N)
- **Name:** the game's name

Repeat for each game with a different name and N. Steam treats them as separate shortcuts.

## Screen tearing

VSync is left at default for the lowest latency. If you see tearing, set **DirectX tab → VSync → ON**. It costs a little latency.

