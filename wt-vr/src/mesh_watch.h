// Reads where a mesh actually is from its vertex data. GHWT animates band members on the CPU: each
// frame it writes the posed vertices into a vertex buffer, and every part is drawn with the same
// World matrix (the character's fixed spot on stage). So the only way to know where their head
// really is - or where world-space parts like guitar strings are - is the vertices themselves.
// The vertex buffers' Lock/Unlock are hooked; a few vertices of the buffers being watched are
// sampled as the game finishes writing them.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include "vr_math.h"

using MeshWatchLogFunction = void (*)(const char* format, ...);

namespace MeshWatch {
    void Initialise(MeshWatchLogFunction log);
    // Hooks Lock / Unlock (every vertex buffer of the device shares them). Call with the first one made.
    void HookVertexBuffers(IDirect3DVertexBuffer9* vertexBuffer);
    void SetFrame(unsigned frame);

    // At a draw: watch the buffer on stream 0 (from now on) and give the average position of the
    // vertices the game last wrote into the part this draw uses, in the mesh's own space.
    // False until there's a sample from this or the previous frame.
    bool DrawCentroid(IDirect3DDevice9* device, UINT firstVertex, UINT vertexCount, Vector3& centroid);
    // The sampled vertices themselves (up to maximum), mesh space; returns how many.
    // wValues (optional): each vertex's 4th position value (screen-space HUD vertices carry depth there).
    int DrawPositions(IDirect3DDevice9* device, UINT firstVertex, UINT vertexCount, Vector3* positions, int maximum, float* wValues = nullptr);
    // Same for DrawPrimitiveUP / DrawIndexedPrimitiveUP vertices (the game hands them over directly).
    int UserPointerPositions(IDirect3DDevice9* device, const void* vertexData, UINT stride, UINT firstVertex, UINT vertexCount,
                             Vector3* positions, int maximum, float* wValues = nullptr);

    void LogWatched();   // diagnostics
}
