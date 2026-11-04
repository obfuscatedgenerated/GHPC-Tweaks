// 3D HUD: room for a raised piece to reach past its flat outline. A ray-marched relief (hud_relief.h)
// can draw a raised part where the flat sprite isn't, but only inside the quad the game drew - so each
// quad of a user-pointer HUD draw is copied, made bigger around its own centre (its texture coordinates
// with it, so the picture stays put), and drawn instead. Only plain sprite quads are padded: indexed
// quads (4 vertices each), triangle lists (6 each) and single 4-vertex strips / fans.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <vector>

namespace HudPadding {
    struct Draw {
        D3DPRIMITIVETYPE type;
        const void* vertices;     // the game's user-pointer vertices
        UINT stride;
        UINT firstVertex, vertexCount;
        UINT primitiveCount;
        const void* indices;      // null: not indexed
        D3DFORMAT indexFormat;
    };

    // The texture area the draw's vertices cover (u min, v min, u max, v max); false if unreadable.
    bool FrameOf(IDirect3DDevice9* device, const Draw& draw, float frame[4]);
    // A copy of the draw's vertices with every quad grown so its texture area gains marginU / marginV on
    // each side (capped at three times its size). False (nothing to draw differently) if the draw isn't
    // plain quads or its vertices can't be read.
    bool PadQuads(IDirect3DDevice9* device, const Draw& draw, float marginU, float marginV, std::vector<BYTE>& padded);
}
