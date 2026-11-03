# GHWT Any Mic

Lets any microphone be used for vocals in Guitar Hero World Tour PC, without renaming it in Windows.

## Why it's needed

GHWT only accepts a recording device if its name contains one of:

- `Logitech USB Mic`
- `Headset (XBOX 360 For Windows)`
- `PDP USB Microphone`
- `AK5370`
- `Mad Catz USB microphone`

That's why renaming your mic to "Logitech USB Microphone" works.

## How it works

A proxy `dsound.dll` placed next to `GHWT.exe`. Windows loads it before the system DirectSound, and it forwards every call to the real DLL in System32, except:

- When `GHWT.exe` lists recording devices, it is shown only one: your **Windows default recording device**, named `Logitech USB Mic`. The game opens whatever is set as default in Sound settings → Recording.
- With no recording device plugged in, nothing is reported, same as the unmodded game.

Only the game's own lookups are changed. FMOD, Bink and anything else in the process see the real device list. `GHWT.exe` itself isn't modified.

Exports keep the real `dsound.dll` ordinals because GHWT imports DirectSound by ordinal.

## Install

1. Copy `dsound.dll` into the folder containing `GHWT.exe`.
2. Set your mic as the default device in Windows Sound settings → Recording.

Delete `dsound.dll` to uninstall.

Changing the default device while the game is running may not take effect until the mic is replugged or the game restarted.

## Compatibility

Works alongside the 4GB patch, WT:VR (`d3d9.dll`) and DxWnd. It can't be combined with another `dsound.dll` (e.g. DSOAL), since both need the same file name.

## Building

- **MinGW (Linux/WSL/MSYS2):** `./build.sh`
- **MSVC:** run `build.bat` from the *x86 Native Tools Command Prompt for VS*

Outputs to `dist/`. Must be built 32-bit.
