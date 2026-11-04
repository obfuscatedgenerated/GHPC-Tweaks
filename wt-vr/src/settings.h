// Settings: ghwt_vr\vr.ini. [vr] everyday settings, [debug] diagnostics and experiments, [keys] hotkeys
// (input.cpp). Every setting is one row of the table in settings.cpp, which drives loading, saving, the
// list at the top of the log and reloading while the game runs: a new setting is a field here plus a row there.
#pragma once

#include <string>
#include "openxr_session.h"   // OverlayPlacement

struct VrSettings {
    float worldScale  = 1.0f;   // game units per real metre (bigger = you feel smaller)
    float renderScale = 0.75f;  // eye resolution relative to the headset's recommendation (restart to apply)
    float nearPlane   = 0.05f;  // game units
    float farPlane    = 600.0f; // game units
    OverlayPlacement overlay = { 1.0f, 1.4f, -0.1f };
    bool  showOverlay = true;
    bool  hudDepth    = true;   // 3D gems in songs: each gem bulges by its height map (hud_depth)
    float hudDepthArea = 0.74f; // hud_depth_area: middle fraction of the screen's width where HUD pieces can go 3D (restart to apply; wider costs more copying)
    float hudGemBulge = 0.02f;
    int   hudDepthTracking = 2; // hud_depth_tracking: 0 fixed per-eye offset (3D-glasses style), 1 follows your eyes (tilt, lean, move) as if every piece were straight ahead, 2 also as seen from off to the side (pieces away from the middle shift as real raised ones would)  // 3D HUD: how far each gem's top bulges, as a fraction of a gem's width per eye (negative flips it)
    int   mirrorHud   = 1;      // monitor HUD: 0 hidden, 1 flat on top (good for playing), 2 floating in place like the headset
    bool  crowdFollow = true;   // crowd sprites face the way they would from where you stand (not the scripted shot)
    int   bandMemberMode = 1;   // the band member you stand in: 0 left alone, 1 hidden, 2 you become them (Shift+H)
    float avatarOffset = 0.0f;  // metres: slides your band member forward (+) / back (-) under you (Ctrl+[ / ])
    float avatarEyeHeight = 1.6f; // avatar_anchor 0: metres from their feet to your eyes: lower = they stand taller (Shift+[ / ])
    int   avatarAnchor = 1;     // you become them: 1 their head is kept at yours (follows their animation), 0 their feet a fixed height below you
    float avatarHeadDrop = 0.05f; // avatar_anchor 1: metres their face sits below your eyes (Shift+[ / ])
    float avatarPinDrop = 0.65f;  // avatar_anchor 1: metres below their eyes where their body is pinned under you (chest ~0.35, waist ~0.65)
    bool  avatarLockTurn = false; // [debug] avatar_lock_turn: cancel their turning under you (experimental: the facing reading isn't reliable)
    bool  eyeForceOpaque = false; // [debug] CPU pass forcing eye alpha to opaque (only needed if a runtime blends eye alpha)
    bool  fullStereo  = true;   // stereo_mode 1: every draw made for both eyes each frame; 0: eyes take turns (animation ghosts)
    bool  wideCulling = false;  // [debug] widen the game's own camera FOV (experimental: GHWT's popping turned out not to be FOV culling)
    float cullingFov  = 130.0f; // [debug] degrees: how wide the game keeps things around your view ([ / ] adjust)
    bool  followHead  = true;   // [debug] the game's camera follows your head (its culling then matches your view)
    bool  depthOfFieldOff = true;  // skip the game's depth-of-field passes in VR
    bool  vrMenus     = true;   // 3D menus in VR when the menu camera is level (rolled ones stay on the flat panel)
    float snapTurnDegrees = 30.0f; // Q/E; 0 = smooth turning
    int   mirrorMode  = 1;      // monitor view of the eye: 0 stretch, 1 crop to fill, 2 fit with black bars
    int   menuCamera  = 1;      // 3D menus, when the game's camera moves to another spot: 0 stay put, 1 fade there, 2 glide with it
    int   role        = -1;     // role=auto (what the game says player 1 plays) / guitar / bass / drums / vocals: auto_spot puts you in that band member's place
    bool  savedSpots  = true;   // saved_spots: songs use your F8 spots (and setlists / restarts keep where you stood)
    bool  walkToPick  = false;  // walk_to_pick: walking into a band member makes them yours (else: the one where you're put at a song's start, or H)
    bool  autoSpot    = true;   // auto_spot: otherwise you're put in your band member's place (role); both off: the game's camera
    float introHopSeconds = 4.0f;   // intro_hop_seconds: while watching the intro, fade to where its camera has got to every this many seconds (0: stay at its first camera)
    int   songIntro   = 1;      // song start: 1 watch the intro from its first camera, then fade to your spot; 0 fade there straight away
    int   logLevel    = 1;      // [debug] log_level: 0 start-up and problems, 1 + what happens (songs, spots, gems), 2 + every-few-seconds diagnostics
    bool  debugTools  = false;  // [debug] debug_tools: diagnostic hotkeys and automatic dumps (draw finder, role probe, HUD dump, camera scan...)
    bool  saveTextures = false; // [debug] save_textures: save each 3D-able HUD texture seen as textures\<name>.dds (to paint a height map from)
};

extern VrSettings  g_settings;
extern std::string g_iniPath;   // ghwt_vr\vr.ini (also holds [spots] and [depth], kept by their own code)

// Reads vr.ini (and [keys]); a missing setting keeps its default and is written in, so the file always lists them all.
void LoadSettings();
void SaveSettings();
// Every setting in use, at the top of the log (marking those changed from the default).
void LogSettings();
// vr.ini edited while the game runs? Reads it again, logs what changed and returns true.
bool ReloadSettingsIfChanged();
