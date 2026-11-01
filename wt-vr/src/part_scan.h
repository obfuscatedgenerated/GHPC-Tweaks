// Which part (guitar / bass / drums / vocals) player 1 is playing, read from the game's script data.
// GHWT keeps each player's settings as a script structure; its field "part" holds the part's name as
// a hashed key (a CRC32 of the name, like all its script names). The structures move every launch,
// so they're found once by scanning the game's memory (a background thread, a second or two) for
// the field's pattern - "part" = <a part>, then two fields on, "band_member" - and the address kept.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using PartScanLogFunction = void (*)(const char* format, ...);

namespace PartScan {
    enum Part { Unknown = -1, Guitar = 0, Bass = 1, Drums = 2, Vocals = 3 };

    void Initialise(PartScanLogFunction log);
    // Find player 1's settings if they aren't known yet (or have moved): call at a song's start.
    void Find();
    // Player 1's part now; Unknown until found.
    Part CurrentPart();
    // Once a frame on the render thread: logs what the background search found.
    void Tick();
}
