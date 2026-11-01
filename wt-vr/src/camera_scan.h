// Finds the game's camera field-of-view in memory, so the game's own culling can be widened.
//
// The game skips drawing anything outside its scripted camera's view. The mod knows that
// camera's projection every frame (from shader constants), so it can:
//   1. scan memory for floats that match the current FOV in any common encoding
//      (degrees / radians, full / half angle, horizontal / vertical, tangent, projection scale)
//   2. on every camera cut that changes the FOV, keep only addresses that changed with it
//   3. hold the survivors at a very wide FOV so the game culls far less
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

using ScanLogFunction = void (*)(const char* format, ...);

namespace CameraScan {
    // projectionScaleX/Y are the game projection's [0][0] and [1][1] (1 / tan of the half angles).
    void Start(float projectionScaleX, float projectionScaleY, ScanLogFunction log);
    // Call once per frame with the game's real (unwidened) projection scales.
    void OnFrame(float projectionScaleX, float projectionScaleY);
    void ToggleWiden();
    // Call once per frame: keeps the hooks on new threads and logs which code writes the FOV.
    void Tick();

    // GHWT.exe 1.0.1.25215 only. Hooks the one place every camera computes its FOV (the camera
    // object is in esi there). Always records which camera objects exist; optionally widens the
    // FOV so culling keeps more; and can overwrite one camera's matrix (head follow).
    bool ArmCameraHooks(bool widenFov, float wideFovDegrees, ScanLogFunction log);
    void DisarmCameraHooks();
    bool CameraHooksArmed();
    void LogCullingHookHits();

    constexpr int CameraDumpBytes = 0x100;
    struct CameraObjectSnapshot {
        uintptr_t object;
        long      hits;
        float     matrix[16];                 // packed pose: rows right, up, back (camera + 0x90), position (camera + 0x74)
        float     raw[CameraDumpBytes / 4];   // camera + 0x00.. for diagnostics
    };
    int  SeenCameras(CameraObjectSnapshot* out, int maximum);
    void ForgetCameras();
    // Overwrite this camera's matrix every time the game uses it (object 0 = stop).
    void SetFollowCamera(uintptr_t object, const float matrix[16]);
    uintptr_t FollowedCamera();
    uintptr_t FollowedViewCamera();
    // Diagnostics: log which game code reads/writes these addresses (needs the camera hooks armed).
    void StartAccessCapture(uintptr_t first, uintptr_t second, uintptr_t third);
    void StopAccessCapture();
    bool AccessCaptureActive();
    // Diagnostics: order of the renderer's view setups in one frame, and the code that called them.
    void SetDrawCounter(volatile unsigned* counter);
    void StartViewTrace();
    void LogViewTrace(unsigned totalDraws);
    // Culling FOV (degrees) for the followed view: wider than the shot so your peripheral vision is kept.
    void SetFollowFov(float degrees);
    // Crowd sprites pick their angle from the head pose too (instead of the scripted shot).
    void SetCrowdFollow(bool enabled);
    // The game's own matrix for the followed camera, from before we overwrote it.
    bool ScriptedCameraMatrix(float matrix[16]);
    void LogCandidates();
    bool IsWidening();
    const char* StatusText();
}
