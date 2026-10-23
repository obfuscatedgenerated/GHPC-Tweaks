# GH3 Rock Band Guitar Fix

Makes Xbox 360 Rock Band guitars (e.g. the RB1 Stratocaster) play correctly in Guitar Hero III PC.

## Why it's needed

GH3 only uses its guitar layout for controllers that report the XInput subtype for Guitar Hero guitars (`0x07`). Rock Band guitars report plain "guitar" (`0x06`), so GH3 incorrectly treats them as a gamepad (the pickup selector holds green, frets mapped wrongly or to strum, etc)

## How it works

A proxy `xinput1_3.dll` placed next to `GH3.exe`. Windows loads it before the system XInput, and it forwards every call to the real DLL in System32, except:

- Rock Band guitars report subtype `0x07`, so GH3 uses the guitar layout.
- The pickup selector (left trigger) and solo-fret flag (left stick click) are cleared, since GH guitars never send them.

Other controllers, including GH guitars, pass through untouched. Exports keep the real `xinput1_3` ordinals because GH3 imports XInput by ordinal.

## Install

Copy `xinput1_3.dll`, `xinput1_2.dll` and `xinput1_1.dll` into the folder containing `GH3.exe`. Delete them to uninstall.

Works alongside DxWnd.

## Steam

Works when launched through Steam, including non-Steam shortcuts, with the overlay and Remote Play left on.

Steam's overlay finds XInput by module name, so it patches a jump into this proxy's own functions and answers GH3 itself, skipping the remap. While a Rock Band guitar is connected, the fix puts its own original function bytes back every 250 ms. Only the proxy's `XInputGetState`, `XInputGetStateEx` and `XInputGetCapabilities` are touched; the rest of the overlay (Shift+Tab, rendering, Remote Play streaming) is untouched. With no Rock Band guitar connected, the overlay's hooks are left alone.

Side effect: while a Rock Band guitar is connected, Remote Play Together guests' controllers won't reach GH3, since Steam delivers those through the hook being removed.

## Debug log

Create an empty file named `xinput_proxy.debug` next to `GH3.exe`. The fix then writes `xinput_proxy.log` there with each controller's type, which modules call XInput, every raw button/axis change, and whenever an overlay hook is removed.

## Building

- **MinGW (Linux/WSL/MSYS2):** `./build.sh`
- **MSVC:** run `build.bat` from the *x86 Native Tools Command Prompt for VS*

Outputs to `dist/`. Must be built 32-bit.
