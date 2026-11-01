// Full stereo (stereo_mode 1): every 3D draw is issued twice per frame, once per eye, like
// NVIDIA's old 3D Vision. Each screen-shaped render target the game uses (scene, depth, bloom
// chain, backbuffer) gets a right-eye twin; for the second draw the twins are bound in place of
// the originals - as render targets, depth buffers and textures - so post-processing also runs
// per eye. Shadow maps and other non-screen-shaped targets stay shared.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

using StereoLogFunction = void (*)(const char* format, ...);

namespace StereoTargets {
    void Initialise(StereoLogFunction log);
    // Targets of this aspect ratio (any size down to a 32nd) are screen-shaped, so per eye.
    void SetScreenSize(UINT width, UINT height);

    // Record what the game creates (hooks call these; ignored while CreatingTwin()).
    void NoteTexture(IDirect3DTexture9* texture);
    void NoteSurface(IDirect3DSurface9* surface);   // render targets, depth-stencil surfaces, the backbuffer

    // The right-eye twin to draw into, created on first use; nullptr when the resource isn't per eye.
    IDirect3DSurface9* TwinSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface);
    // The right-eye twin to sample; nullptr when not per eye or when it missed the game's last write
    // (then the right eye uses the original rather than something stale).
    IDirect3DBaseTexture9* TwinTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* texture);
    bool IsPerEye(IDirect3DSurface9* surface);

    // Every game write into a per-eye target: was it repeated into the twin?
    void NoteWrite(IDirect3DSurface9* surface, bool rightEyeToo);
    bool RightEyeCurrent(IDirect3DSurface9* surface);

    bool CreatingTwin();
    void ReleaseIdle(unsigned frame);   // every frame: frees twins unused for a few seconds
    void ReleaseAll();      // device reset
    void LogSummary();
}
