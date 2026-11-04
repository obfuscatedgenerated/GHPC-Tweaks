// WT:VR - Guitar Hero World Tour in VR (OpenXR).
//
// A d3d9.dll proxy: the game loads it as Direct3D 9, it hooks the device and hands the real one
// (System32\d3d9.dll) the work. This file holds the device hooks and everything that rides on them:
//   - every 3D draw gets its camera rebuilt from your headset's eye pose, and is drawn for both eyes
//   - the highway / notes / HUD (drawn in screen space) go onto a panel floating in front of you,
//     with notes (and anything with a height map) on per-eye panels that give them depth
//   - where you stand: songs' start cameras, saved spots, your band member's place, 3D menus
//   - your band member: found by their shaders and hidden, or placed under you
//   - copying the eye images and panels to the headset (openxr_session.cpp)
// Other parts: settings.cpp (vr.ini), input.cpp (hotkeys), log.cpp, hud_relief.cpp (3D HUD pieces),
// stereo_targets.cpp (right-eye copies of render targets), camera_scan.cpp (the game's own camera in
// memory), mesh_watch.cpp (reading posed vertices), part_scan.cpp (which part you play).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <iterator>

#include "vr_math.h"
#include "openxr_session.h"
#include "camera_scan.h"
#include "stereo_targets.h"
#include "mesh_watch.h"
#include "part_scan.h"
#include "log.h"
#include "settings.h"
#include "input.h"
#include "hud_relief.h"
#include "hud_padding.h"

#ifndef WTVR_VERSION
#define WTVR_VERSION "dev"   // the build sets it (the release tag)
#endif

// ---------------------------------------------------------------------------
// Vtable slot indices (verified against mingw d3d9.h)
// ---------------------------------------------------------------------------
namespace Slot {
    constexpr int CreateDevice             = 16;  // IDirect3D9
    constexpr int Reset                    = 16;  // IDirect3DDevice9
    constexpr int Present                  = 17;
    constexpr int CreateTexture            = 23;
    constexpr int CreateVertexBuffer       = 26;
    constexpr int CreateRenderTarget       = 28;
    constexpr int CreateDepthStencilSurface = 29;
    constexpr int StretchRect              = 34;
    constexpr int ColorFill                = 35;
    constexpr int SetRenderTarget          = 37;
    constexpr int SetDepthStencilSurface   = 39;
    constexpr int Clear                    = 43;
    constexpr int SetTexture               = 65;
    constexpr int DrawPrimitive            = 81;
    constexpr int DrawIndexedPrimitive     = 82;
    constexpr int DrawPrimitiveUP          = 83;
    constexpr int DrawIndexedPrimitiveUP   = 84;
    constexpr int CreateVertexShader       = 91;
    constexpr int SetVertexShader          = 92;
    constexpr int SetVertexShaderConstantF = 94;
    constexpr int CreatePixelShader        = 106;
    constexpr int SetPixelShader           = 107;
    constexpr int SetPixelShaderConstantF  = 109;
}

constexpr int MaxFloatConstants = 256;
constexpr int NoRegister        = -1;

// ---------------------------------------------------------------------------
// Shader constant table (CTAB) parsing
// ---------------------------------------------------------------------------
struct ConstantTableHeader {
    DWORD size;
    DWORD creatorOffset;
    DWORD version;
    DWORD constantCount;
    DWORD constantInfoOffset;
    DWORD flags;
    DWORD targetOffset;
};

struct ConstantTableEntry {
    DWORD nameOffset;
    WORD  registerSet;
    WORD  registerIndex;
    WORD  registerCount;
    WORD  reserved;
    DWORD typeInfoOffset;
    DWORD defaultValueOffset;
};

constexpr WORD  RegisterSetFloat4 = 2;
constexpr DWORD CtabFourCC        = 0x42415443; // 'CTAB'
constexpr DWORD EndToken          = 0x0000FFFF;

struct ShaderInfo {
    int viewProjRegister      = NoRegister;  // g_ViewProj      (4 rows)
    int worldViewProjRegister = NoRegister;  // g_WorldViewProj (4 rows)
    int worldRegister         = NoRegister;  // g_World         (4 rows)
    int worldViewRegister     = NoRegister;  // g_WorldView     (4 rows)
    int inverseViewRegister   = NoRegister;  // g_InvView       (3 rows: camera right, up, back)
    int viewportRegister      = NoRegister;  // g_viewport      (screen-space highway / HUD shaders)
    bool isHighway            = false;       // has m_startFade: highway, notes, sustains (only drawn in songs)
    bool isCharacterSkin      = false;       // has g_TextureProjection0 + g_World: band member body / face / hair
    uint32_t hash             = 0;           // FNV-1a of the bytecode (same as the probe's, to match the notes)
    struct Constant { std::string name; int index; int count; };
    std::vector<Constant> constants;         // its float constants (diagnostics)

    bool UsesCamera() const {
        return viewProjRegister != NoRegister || worldViewProjRegister != NoRegister ||
               worldViewRegister != NoRegister || inverseViewRegister != NoRegister;
    }
    bool IsScreenSpaceOverlay() const { return viewportRegister != NoRegister && !UsesCamera(); }
};

static ShaderInfo ParseShader(const DWORD* tokens) {
    ShaderInfo info;
    const int majorVersion = (tokens[0] >> 8) & 0xFF;
    const size_t tokenLimit = 1 << 16;

    size_t position = 1;
    while (position < tokenLimit && tokens[position] != EndToken) {
        const DWORD token = tokens[position];
        if ((token & 0xFFFF) == 0xFFFE) {
            const DWORD commentLength = (token >> 16) & 0x7FFF;
            const DWORD* commentData = &tokens[position + 1];
            if (commentLength >= 2 && commentData[0] == CtabFourCC) {
                const char* tableBase = reinterpret_cast<const char*>(&commentData[1]);
                const auto* header = reinterpret_cast<const ConstantTableHeader*>(tableBase);
                const auto* entries = reinterpret_cast<const ConstantTableEntry*>(tableBase + header->constantInfoOffset);
                for (DWORD entryIndex = 0; entryIndex < header->constantCount; ++entryIndex) {
                    const ConstantTableEntry& entry = entries[entryIndex];
                    if (entry.registerSet != RegisterSetFloat4) continue;
                    if (entry.registerIndex + entry.registerCount > MaxFloatConstants) continue;
                    const char* name = tableBase + entry.nameOffset;
                    const int index = entry.registerIndex;
                    const int count = entry.registerCount;
                    if (info.constants.size() < 48) info.constants.push_back({ name, index, count });
                    if (count == 4 && strcmp(name, "g_ViewProj") == 0)      info.viewProjRegister = index;
                    if (count == 4 && strcmp(name, "g_WorldViewProj") == 0) info.worldViewProjRegister = index;
                    if (count == 4 && strcmp(name, "g_World") == 0)         info.worldRegister = index;
                    if (count == 4 && strcmp(name, "g_WorldView") == 0)     info.worldViewRegister = index;
                    if (count == 3 && strcmp(name, "g_InvView") == 0)       info.inverseViewRegister = index;
                    if (strcmp(name, "g_viewport") == 0)                    info.viewportRegister = index;
                    if (strcmp(name, "m_startFade") == 0)                   info.isHighway = true;
                    if (strcmp(name, "g_TextureProjection0") == 0)          info.isCharacterSkin = true;
                }
            }
            position += 1 + commentLength;
            continue;
        }
        position += majorVersion >= 2 ? 1 + ((token >> 24) & 0x0F) : 1;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(tokens);
    const size_t byteSize = (position + 1) * sizeof(DWORD);
    uint32_t hash = 2166136261u;
    for (size_t byteIndex = 0; byteIndex < byteSize; ++byteIndex) {
        hash ^= bytes[byteIndex];
        hash *= 16777619u;
    }
    info.hash = hash;
    return info;
}

// Pixel shaders: only used to find the depth-of-field passes (by their constant names).
struct PixelShaderInfo {
    bool isDepthOfField = false;
    std::string constantNames;
    std::string registerMap;           // "name=c4x1 sampler=s0 ..." for diagnostics
    std::vector<DWORD> bytecode;       // kept for depth-of-field shaders only (dumped for analysis)
    int depthOfFieldRegister = -1;     // g_DOF: x near focus, y far start, z far range, w blur strength
    uint32_t hash = 0;                 // FNV-1a of the bytecode
};

static bool IsDepthOfFieldConstant(const char* name) {
    return strstr(name, "DOF") || strstr(name, "DepthOfField") || strstr(name, "NearBlur");
}

static PixelShaderInfo ParsePixelShader(const DWORD* tokens) {
    PixelShaderInfo info;
    const int majorVersion = (tokens[0] >> 8) & 0xFF;
    const size_t tokenLimit = 1 << 16;
    size_t position = 1;
    while (position < tokenLimit && tokens[position] != EndToken) {
        const DWORD token = tokens[position];
        if ((token & 0xFFFF) == 0xFFFE) {
            const DWORD commentLength = (token >> 16) & 0x7FFF;
            const DWORD* commentData = &tokens[position + 1];
            if (commentLength >= 2 && commentData[0] == CtabFourCC) {
                const char* tableBase = reinterpret_cast<const char*>(&commentData[1]);
                const auto* header = reinterpret_cast<const ConstantTableHeader*>(tableBase);
                const auto* entries = reinterpret_cast<const ConstantTableEntry*>(tableBase + header->constantInfoOffset);
                for (DWORD entryIndex = 0; entryIndex < header->constantCount; ++entryIndex) {
                    const char* name = tableBase + entries[entryIndex].nameOffset;
                    if (IsDepthOfFieldConstant(name)) info.isDepthOfField = true;
                    const ConstantTableEntry& entry = entries[entryIndex];
                    if (strcmp(name, "g_DOF") == 0 && entry.registerSet == RegisterSetFloat4) info.depthOfFieldRegister = entry.registerIndex;
                    char mapping[96];
                    snprintf(mapping, sizeof(mapping), "%s=%c%ux%u ", name, entry.registerSet == 3 ? 's' : (entry.registerSet == 2 ? 'c' : 'i'),
                             unsigned(entry.registerIndex), unsigned(entry.registerCount));
                    if (info.registerMap.size() < 600) info.registerMap += mapping;
                    if (info.constantNames.size() < 400) {
                        if (!info.constantNames.empty()) info.constantNames += ' ';
                        info.constantNames += name;
                    }
                }
            }
            position += 1 + commentLength;
            continue;
        }
        position += majorVersion >= 2 ? 1 + ((token >> 24) & 0x0F) : 1;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(tokens);
    uint32_t hash = 2166136261u;
    for (size_t byteIndex = 0; byteIndex < (position + 1) * sizeof(DWORD); ++byteIndex) {
        hash ^= bytes[byteIndex];
        hash *= 16777619u;
    }
    info.hash = hash;
    return info;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static std::string g_outputDir;
static std::string g_loaderPath;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
using CreateDeviceFn             = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using ResetFn                    = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn                  = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetRenderTargetFn          = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using CreateTextureFn            = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateVertexBufferFn       = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using CreateRenderTargetFn       = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using CreateDepthStencilSurfaceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using StretchRectFn              = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using ColorFillFn                = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
using SetDepthStencilSurfaceFn   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using ClearFn                    = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetTextureFn               = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawPrimitiveFn            = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn     = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn          = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using CreateVertexShaderFn       = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVertexShaderFn          = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using CreatePixelShaderFn        = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPixelShaderFn           = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFFn  = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

static HMODULE g_realD3D9 = nullptr;

static CreateDeviceFn             g_originalCreateDevice = nullptr;
static ResetFn                    g_originalReset = nullptr;
static PresentFn                  g_originalPresent = nullptr;
static SetRenderTargetFn          g_originalSetRenderTarget = nullptr;
static CreateTextureFn            g_originalCreateTexture = nullptr;
static CreateVertexBufferFn       g_originalCreateVertexBuffer = nullptr;
static CreateRenderTargetFn       g_originalCreateRenderTarget = nullptr;
static CreateDepthStencilSurfaceFn g_originalCreateDepthStencilSurface = nullptr;
static StretchRectFn              g_originalStretchRect = nullptr;
static ColorFillFn                g_originalColorFill = nullptr;
static SetDepthStencilSurfaceFn   g_originalSetDepthStencilSurface = nullptr;
static ClearFn                    g_originalClear = nullptr;
static SetTextureFn               g_originalSetTexture = nullptr;
static DrawPrimitiveFn            g_originalDrawPrimitive = nullptr;
static DrawIndexedPrimitiveFn     g_originalDrawIndexedPrimitive = nullptr;
static DrawPrimitiveUPFn          g_originalDrawPrimitiveUP = nullptr;
static DrawIndexedPrimitiveUPFn   g_originalDrawIndexedPrimitiveUP = nullptr;
static CreateVertexShaderFn       g_originalCreateVertexShader = nullptr;
static SetVertexShaderFn          g_originalSetVertexShader = nullptr;
static SetVertexShaderConstantFFn g_originalSetVertexShaderConstantF = nullptr;
static CreatePixelShaderFn        g_originalCreatePixelShader = nullptr;
static SetPixelShaderFn           g_originalSetPixelShader = nullptr;
static SetPixelShaderConstantFFn  g_originalSetPixelShaderConstantF = nullptr;


static std::unordered_map<IDirect3DVertexShader9*, ShaderInfo> g_shaders;
static IDirect3DVertexShader9* g_currentShader = nullptr;
static std::unordered_map<IDirect3DPixelShader9*, PixelShaderInfo> g_pixelShaders;
static IDirect3DPixelShader9* g_currentPixelShader = nullptr;
static float g_pixelConstants[224][4] = {};   // what the game last set (ps_3_0 has 224)

// What the game asked for (never modified by us).
static float g_gameConstants[MaxFloatConstants][4] = {};

// Register ranges currently holding our VR values on the device instead of the game's.
struct ModifiedRange {
    int baseRegister;
    int registerCount;
};
static std::vector<ModifiedRange> g_modifiedOnDevice;

static UINT g_backBufferWidth = 0;
static UINT g_backBufferHeight = 0;
static UINT g_renderTargetWidth = 0;
static UINT g_renderTargetHeight = 0;
static unsigned g_frameIndex = 0;

static OpenXrSession g_xr;

// Diagnostics: counted per status interval, logged every StatusIntervalFrames frames.
constexpr unsigned StatusIntervalFrames = 120;
struct FrameCounters {
    unsigned framesWithBegunXrFrame = 0;
    unsigned framesWithEyeView = 0;
    unsigned mainTargetCameraDraws = 0;   // 3D draws into the main view
    unsigned vrCameraDraws = 0;           // ...of which got the VR camera
    unsigned overlayDraws = 0;
    unsigned depthOfFieldDraws = 0;
    unsigned depthOfFieldSkipped = 0;
    unsigned followMatchedFrames = 0;      // game camera == our head matrix (head follow working)
    unsigned followFrames = 0;
    unsigned rightEyeDraws = 0;           // full stereo: draws repeated for the right eye
    unsigned eyeImagesSubmitted = 0;
    unsigned overlayImagesSubmitted = 0;
    unsigned framesWithLayers = 0;
    unsigned framesShouldRender = 0;
    double totalFrameMs = 0, worstFrameMs = 0;     // Present to Present
    double totalSubmitMs = 0, worstSubmitMs = 0;   // copying images to the headset
    double totalXrWaitMs = 0, worstXrWaitMs = 0;   // headset frame pacing + polling + init attempts
    double totalStageMs = 0;                       // ...of the headset copy: queueing GPU -> CPU (should be ~0)
    double totalReadbackMs = 0;                    // ...waiting for GPU -> CPU next frame
    double totalAlphaMs = 0;                       // ...making eye images opaque on the CPU
    double totalUploadMs = 0;                      // ...CPU -> headset textures
    UINT lastRenderTargetWidth = 0, lastRenderTargetHeight = 0;
};
static FrameCounters g_counters;
static unsigned g_mainDrawsThisFrame = 0;
static volatile unsigned g_allDrawsThisFrame = 0;   // every draw call (diagnostics)

// Diagnostics: list one frame's 3D draws with where each object is, to find your band member.
static bool g_drawListActive = false;
static unsigned g_drawListLines = 0;

// Hook each slot exactly once (overlays like Steam/Discord re-hook the same slots).
static void PatchVtableSlot(void* comObject, int slot, void* replacement, void** originalOut) {
    if (*originalOut) return;
    void** vtable = *reinterpret_cast<void***>(comObject);
    if (vtable[slot] == replacement) return;
    DWORD oldProtection;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection);
    *originalOut = vtable[slot];
    vtable[slot] = replacement;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtection, &oldProtection);
}

static const ShaderInfo* CurrentShaderInfo() {
    auto found = g_shaders.find(g_currentShader);
    return found == g_shaders.end() ? nullptr : &found->second;
}

// Several screen-sized render targets can be in use. The 3D scene goes to one (or two, if the game
// alternates); others hold pieces like the main menu's text, drawn flat into its own image and then
// shown on an angled card. Used to keep the HUD redirect and the menu-text handling off those.
struct RenderTargetStats {
    unsigned cameraDrawsThisFrame = 0;
    unsigned recentPeak = 0;        // decays slowly, so alternating targets keep their status
    unsigned lastUsedFrame = 0;
    bool     loggedAsOffscreen = false;
};
static IDirect3DSurface9* g_currentRenderTarget = nullptr;
static std::unordered_map<IDirect3DSurface9*, RenderTargetStats> g_renderTargetStats;
static unsigned g_busiestTargetPeak = 0;

static bool IsMainRenderTarget();

static bool IsSceneRenderTarget() {
    if (!IsMainRenderTarget()) return false;
    auto found = g_renderTargetStats.find(g_currentRenderTarget);
    if (g_busiestTargetPeak == 0) return true;               // nothing known yet
    if (found == g_renderTargetStats.end()) return false;    // never gets 3D draws (e.g. the menu text image)
    return found->second.recentPeak * 2 >= g_busiestTargetPeak;
}

static void UpdateRenderTargetStats(unsigned frameIndex, void (*log)(const char*, ...)) {
    g_busiestTargetPeak = 0;
    for (auto entry = g_renderTargetStats.begin(); entry != g_renderTargetStats.end();) {
        RenderTargetStats& stats = entry->second;
        stats.recentPeak = std::max(stats.cameraDrawsThisFrame, stats.recentPeak - stats.recentPeak / 32);
        if (stats.cameraDrawsThisFrame) stats.lastUsedFrame = frameIndex;
        stats.cameraDrawsThisFrame = 0;
        if (frameIndex - stats.lastUsedFrame > 600) { entry = g_renderTargetStats.erase(entry); continue; }
        g_busiestTargetPeak = std::max(g_busiestTargetPeak, stats.recentPeak);
        ++entry;
    }
    for (auto& entry : g_renderTargetStats) {
        RenderTargetStats& stats = entry.second;
        if (stats.loggedAsOffscreen || stats.recentPeak == 0 || stats.recentPeak * 2 >= g_busiestTargetPeak) continue;
        stats.loggedAsOffscreen = true;
        log("[targets] screen-sized target %p gets ~%u 3D draws a frame (scene ~%u): left as the game draws it",
            (void*)entry.first, stats.recentPeak, g_busiestTargetPeak);
    }
}

static bool IsMainRenderTarget() {
    return g_renderTargetWidth == g_backBufferWidth && g_renderTargetHeight == g_backBufferHeight;
}

static int EyeForThisFrame() {
    return int(g_frameIndex & 1);
}

// ---------------------------------------------------------------------------
// Game camera observation and the VR anchor
// ---------------------------------------------------------------------------
static Matrix4 g_gameViewProj;
static Matrix4 g_gameViewProjInverse;
static bool    g_gameViewProjKnownThisFrame = false;
static bool    g_gameViewProjEverKnown = false;

// Where the player stands in the game world, and which way the anchor faces.
static Vector3 g_anchorPosition = { 0.0f, 0.0f, 0.0f };
static float   g_anchorYaw = 0.0f;
static bool    g_anchorValid = false;
static bool    g_anchorFromCameraRequested = true;

static void SetGameViewProj(const Matrix4& viewProj) {
    g_gameViewProj = viewProj;
    if (!Invert(viewProj, g_gameViewProjInverse)) return;
    g_gameViewProjKnownThisFrame = true;
    g_gameViewProjEverKnown = true;

    if (g_anchorFromCameraRequested) {
        Vector3 cameraPosition;
        if (CameraPositionFromViewProj(viewProj, cameraPosition)) {
            g_anchorPosition = cameraPosition;
            g_anchorYaw = CameraYawFromViewProj(viewProj);
            g_anchorValid = true;
            g_anchorFromCameraRequested = false;
            LogLine("[anchor] from game camera: position %.2f %.2f %.2f yaw %.1f deg",
                    cameraPosition.x, cameraPosition.y, cameraPosition.z, g_anchorYaw * 57.2958f);
        }
    }
}

// Projection scale of the game camera (1 / tan of the half angle) for a ViewProj column.
// Column 0 is right * scaleX and column 1 is up * scaleY, so their lengths are the scales.
static float GameProjectionScale(int column) {
    const float x = g_gameViewProj.values[0][column];
    const float y = g_gameViewProj.values[1][column];
    const float z = g_gameViewProj.values[2][column];
    return sqrtf(x * x + y * y + z * z);
}

// The game camera's axes (rows right, up, back) as the game set them, first seen this frame.
static Vector3 g_gameCameraAxes[3];
static bool    g_gameCameraAxesKnownThisFrame = false;

static void ObserveGameCamera(const ShaderInfo& shader) {
    if (shader.inverseViewRegister != NoRegister && !g_gameCameraAxesKnownThisFrame) {
        for (int row = 0; row < 3; ++row) {
            const float* values = g_gameConstants[shader.inverseViewRegister + row];
            g_gameCameraAxes[row] = { values[0], values[1], values[2] };
        }
        g_gameCameraAxesKnownThisFrame = true;
    }
    if (g_gameViewProjKnownThisFrame) return;
    if (shader.viewProjRegister != NoRegister) {
        SetGameViewProj(Matrix4::FromRegisters(&g_gameConstants[shader.viewProjRegister]));
        return;
    }
    if (shader.worldViewProjRegister != NoRegister && shader.worldRegister != NoRegister) {
        Matrix4 inverseWorld;
        if (Invert(Matrix4::FromRegisters(&g_gameConstants[shader.worldRegister]), inverseWorld))
            SetGameViewProj(Multiply(inverseWorld, Matrix4::FromRegisters(&g_gameConstants[shader.worldViewProjRegister])));
    }
}

// ---------------------------------------------------------------------------
// VR camera for this frame's eye
// ---------------------------------------------------------------------------
static EyeView g_renderingEyeView;          // pose/fov this frame is rendered with
static bool    g_renderingEyeViewValid = false;

static Matrix4 g_vrCameraToWorld;            // the eye being drawn (see SelectEye)
static Matrix4 g_vrView;
static Matrix4 g_vrViewProj;
static bool    g_vrMatricesValid = false;    // computed for this frame
static bool    g_vrCameraUsedThisFrame = false;

// Full stereo (stereo_mode 1): this frame is drawn for both eyes, every draw twice.
struct FullStereoFrame {
    bool    active = false;
    EyeView eyes[2];
};
static FullStereoFrame g_fullStereoFrame;

struct EyeMatrices {
    Matrix4 cameraToWorld, view, viewProj;
};
static EyeMatrices g_eyeMatrices[2];

static bool ComputeEyeMatrices(const EyeView& eyeView, EyeMatrices& matrices) {
    const XrPosef& pose = eyeView.pose;
    const XrFovf& fov = eyeView.fov;

    // Eye in anchor space (metres) -> game units -> game world at the anchor.
    Vector3 scaledPosition = { pose.position.x * g_settings.worldScale,
                               pose.position.y * g_settings.worldScale,
                               pose.position.z * g_settings.worldScale };
    Quaternion orientation = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w };
    Matrix4 eyeToAnchor = PoseToMatrix(orientation, scaledPosition);
    Matrix4 anchorToWorld = YawTranslation(g_anchorYaw, g_anchorPosition);
    matrices.cameraToWorld = Multiply(eyeToAnchor, anchorToWorld);

    if (!Invert(matrices.cameraToWorld, matrices.view)) return false;
    Matrix4 projection = OffCentreProjection(tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleDown), tanf(fov.angleUp),
                                             g_settings.nearPlane, g_settings.farPlane);
    matrices.viewProj = Multiply(matrices.view, projection);
    return true;
}

static void SelectEye(int eye) {
    g_vrCameraToWorld = g_eyeMatrices[eye].cameraToWorld;
    g_vrView = g_eyeMatrices[eye].view;
    g_vrViewProj = g_eyeMatrices[eye].viewProj;
}

static void ComputeVrMatrices() {
    if (g_fullStereoFrame.active) {
        if (!ComputeEyeMatrices(g_fullStereoFrame.eyes[0], g_eyeMatrices[0]) ||
            !ComputeEyeMatrices(g_fullStereoFrame.eyes[1], g_eyeMatrices[1])) return;
    } else if (!ComputeEyeMatrices(g_renderingEyeView, g_eyeMatrices[0])) {
        return;
    }
    SelectEye(0);
    g_vrMatricesValid = true;
}

// Full VR only during songs (detected by the highway being drawn). Menus and loading screens
// are shown on the flat panel instead: some menus use 3D cameras with roll that look wrong in VR.
constexpr unsigned SongTimeoutFrames = 45;
constexpr unsigned SongMinimumSceneDraws = 80;   // venue draws per frame (songs ~200+, difficulty screen ~30)
static unsigned g_framesSinceHighway = ~0u;
static bool g_highwayDrawnThisFrame = false;
// Any highway at all lately (practice mode has one but next to no venue, so it doesn't count as a song).
static unsigned g_framesSinceAnyHighway = ~0u;
static bool g_wasInSong = false;
static unsigned g_framesInSong = 0;

static bool InSong() {
    return g_framesSinceHighway < SongTimeoutFrames;
}

// Menus with a real 3D scene are shown in VR too (the 2D menu goes on the panel in front of you).
// Menus drawn with a "UI camera" stay on the flat panel: one sitting exactly at the world origin
// (the quickplay / setlist screens) or one that's rolled. Every camera draw is checked, since a
// menu can mix a level background camera with a rotated one. Switching has some hysteresis so a
// menu doesn't flicker between the two.
static bool g_menuVrAllowed = false;
static int  g_menuGoodFrames = 0;
static int  g_menuBadFrames = 0;
static bool g_uiCameraThisFrame = false;

static bool IsUiCamera(const Matrix4& viewProj) {
    // ViewProj column 0 is the camera's right axis (scaled), column 1 its up axis.
    const Vector3 right = { viewProj.values[0][0], viewProj.values[1][0], viewProj.values[2][0] };
    const Vector3 up    = { viewProj.values[0][1], viewProj.values[1][1], viewProj.values[2][1] };
    const float rightLength = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    const float upLength    = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
    if (rightLength <= 0 || upLength <= 0) return true;
    if (fabsf(right.y / rightLength) > 0.35f || up.y / upLength < 0.5f) return true;   // rolled / upside down
    Vector3 position;
    if (!CameraPositionFromViewProj(viewProj, position)) return true;
    return fabsf(position.x) < 0.01f && fabsf(position.y) < 0.01f && fabsf(position.z) < 0.01f;
}

// Diagnostics: once every 120 frames in menus, log each distinct camera drawn that frame.
static void LogMenuCamera(const char* kind, const Matrix4& viewProj, bool isUi) {
    static unsigned loggedFrame = ~0u;
    static Vector3 loggedPositions[8];
    static int loggedCount = 0;
    if (g_frameIndex % 120 != 0) return;
    if (loggedFrame != g_frameIndex) { loggedFrame = g_frameIndex; loggedCount = 0; }
    Vector3 position = {};
    CameraPositionFromViewProj(viewProj, position);
    for (int index = 0; index < loggedCount; ++index) {
        const Vector3& seen = loggedPositions[index];
        if (fabsf(seen.x - position.x) < 0.01f && fabsf(seen.y - position.y) < 0.01f && fabsf(seen.z - position.z) < 0.01f) return;
    }
    if (loggedCount >= 8) return;
    loggedPositions[loggedCount++] = position;
    LogLine("[menu camera] frame %u %s: pos %.2f %.2f %.2f right %.2f %.2f %.2f up %.2f %.2f %.2f -> %s", g_frameIndex, kind,
            position.x, position.y, position.z, viewProj.values[0][0], viewProj.values[1][0], viewProj.values[2][0],
            viewProj.values[0][1], viewProj.values[1][1], viewProj.values[2][1], isUi ? "UI camera (flat panel)" : "3D");
}

// Setlists: the next song keeps where you stood in the last one (its later songs all start from
// an intro shot of their own, which has no remembered spot). Forgotten once a flat menu (the
// song list) is shown, since a different venue may come next.
static bool    g_songCarryValid = false;
static double  g_songCarryFlatMs = 0.0;   // time spent in flat menus since the last song ended
static Vector3 g_songCarryPosition = {};
static float   g_songCarryYaw = 0.0f;

static double NowMs();

// A while in flat menus (the song list) means a different venue may come next. A short one is the
// pause menu's restart; the fail / results screens are 3D (in the venue) and don't count at all.
constexpr double SongCarryFlatMenuMs = 30000.0;
static void ForgetSongCarryIfOld() {
    static double previousMs = 0.0;
    const double nowMs = NowMs();
    const double sinceLast = nowMs - previousMs;
    previousMs = nowMs;
    if (!g_songCarryValid || sinceLast <= 0.0) return;
    g_songCarryFlatMs += std::min(sinceLast, 100.0);   // called for every flat draw: counts frames, not draws
    if (g_songCarryFlatMs > SongCarryFlatMenuMs) {
        g_songCarryValid = false;
        LogLine("[spots] %.0f s in flat menus since the last song: the next one starts fresh", g_songCarryFlatMs / 1000.0);
    }
}

static unsigned g_menuDrawsWithoutCamera = 0;   // WVP-only draws: their camera can't be checked directly
static unsigned g_menuTiltedDraws = 0;          // ...of which come out lying on their side in the world

// WVP-only draws (the main menu's text) may use their own rolled camera, which can't be read back.
// Their world matrix as seen from the main camera then has the roll baked in: the object's up axis
// ends up horizontal, and re-rendered from your head it comes out sideways. Upright scenery
// doesn't do that. These draws go onto the floating panel exactly as the game drew them, while
// the rest of the menu stays 3D.
static bool g_currentDrawIsRolledUi = false;

static void CheckCameraLessDraw(const ShaderInfo& shader) {
    ++g_menuDrawsWithoutCamera;
    if (shader.worldViewProjRegister == NoRegister || !g_gameViewProjKnownThisFrame) return;
    const Matrix4 world = Multiply(Matrix4::FromRegisters(&g_gameConstants[shader.worldViewProjRegister]), g_gameViewProjInverse);
    const Vector3 up = { world.values[1][0], world.values[1][1], world.values[1][2] };
    const float upLength = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
    if (upLength <= 0 || fabsf(up.y / upLength) >= 0.5f) return;
    ++g_menuTiltedDraws;
    g_currentDrawIsRolledUi = true;
    if (g_frameIndex % 120 == 0 && g_menuTiltedDraws <= 3)
        LogLine("[menu camera] frame %u tilted draw: up axis %.2f %.2f %.2f, position %.2f %.2f %.2f", g_frameIndex,
                up.x / upLength, up.y / upLength, up.z / upLength, world.values[3][0], world.values[3][1], world.values[3][2]);
}

// Called for every 3D draw into the main view while not in a song.
static void CheckMenuCamera(const ShaderInfo& shader) {
    if (shader.viewProjRegister != NoRegister) {
        const Matrix4 viewProj = Matrix4::FromRegisters(&g_gameConstants[shader.viewProjRegister]);
        const bool isUi = IsUiCamera(viewProj);
        LogMenuCamera("ViewProj", viewProj, isUi);
        if (isUi) { g_uiCameraThisFrame = true; ForgetSongCarryIfOld(); }
        return;
    }
    if (shader.worldViewProjRegister != NoRegister && shader.worldRegister != NoRegister) {
        Matrix4 inverseWorld;
        if (Invert(Matrix4::FromRegisters(&g_gameConstants[shader.worldRegister]), inverseWorld)) {
            const Matrix4 viewProj = Multiply(inverseWorld, Matrix4::FromRegisters(&g_gameConstants[shader.worldViewProjRegister]));
            const bool isUi = IsUiCamera(viewProj);
            LogMenuCamera("World+WVP", viewProj, isUi);
            if (isUi) { g_uiCameraThisFrame = true; ForgetSongCarryIfOld(); }
        }
        return;
    }
    CheckCameraLessDraw(shader);
}

static void UpdateMenuVrAllowed() {
    if (g_frameIndex % 120 == 0 && !InSong())
        LogLine("[menu camera] frame %u: %u draws without their own camera (%u tilted), UI camera %s, VR menus %s", g_frameIndex,
                g_menuDrawsWithoutCamera, g_menuTiltedDraws, g_uiCameraThisFrame ? "seen" : "not seen", g_menuVrAllowed ? "on" : "off");
    g_menuDrawsWithoutCamera = 0;
    g_menuTiltedDraws = 0;
    const bool good = g_settings.vrMenus && g_gameViewProjKnownThisFrame && !g_uiCameraThisFrame;
    g_uiCameraThisFrame = false;
    if (good) {
        g_menuBadFrames = 0;
        if (++g_menuGoodFrames >= 20) g_menuVrAllowed = true;
    } else {
        g_menuGoodFrames = 0;
        if (++g_menuBadFrames >= 10 || !g_settings.vrMenus) g_menuVrAllowed = false;
    }
}

static bool VrCameraActive() {
    return g_xr.IsRunning() && g_xr.FrameBegun() && g_renderingEyeViewValid && g_anchorValid && (InSong() || g_menuVrAllowed);
}

// ---------------------------------------------------------------------------
// Per-draw constant replacement
// ---------------------------------------------------------------------------
static void UploadOverride(IDirect3DDevice9* device, int baseRegister, const float* values, int registerCount) {
    g_originalSetVertexShaderConstantF(device, baseRegister, values, registerCount);
    for (const ModifiedRange& range : g_modifiedOnDevice)
        if (range.baseRegister == baseRegister && range.registerCount == registerCount) return;
    g_modifiedOnDevice.push_back({ baseRegister, registerCount });
}

static void RestoreAllOverrides(IDirect3DDevice9* device) {
    for (const ModifiedRange& range : g_modifiedOnDevice)
        g_originalSetVertexShaderConstantF(device, range.baseRegister, g_gameConstants[range.baseRegister], range.registerCount);
    g_modifiedOnDevice.clear();
}

static void NoteCharacterDraw(const ShaderInfo& shader);
static bool    g_currentDrawIsAvatar = false;   // this draw is part of your band member, placed under you
static const void* g_upVertexData = nullptr;    // DrawPrimitiveUP / DIPUP: the vertices
static UINT    g_upStride = 0;
static UINT    g_upPrimitiveCount = 0;          // ...its primitive count
static const void* g_upIndexData = nullptr;     // DIPUP: its indices (null: DrawPrimitiveUP)
static D3DFORMAT g_upIndexFormat = D3DFMT_INDEX16;
static UINT    g_drawFirstVertex = 0;           // vertex buffer draws: which vertices this draw uses
static UINT    g_drawVertexCount = 0;
static D3DPRIMITIVETYPE g_drawPrimitiveType = D3DPT_TRIANGLELIST;
static int     g_nearbyDiagnosticFrames = 0;    // log draws near your band member that aren't theirs
static Matrix4 g_avatarTransform;
static bool    g_currentDrawUsedVrCamera = false;   // full stereo: the right-eye repeat needs its camera too

static void UploadVrCamera(IDirect3DDevice9* device, const ShaderInfo* shader);

static void PrepareCameraForDraw(IDirect3DDevice9* device, const ShaderInfo* shader) {
    // Anything other than a 3D draw into the main view gets the game's own values back
    // (shadow maps, bloom, highway, HUD).
    RestoreAllOverrides(device);
    if (shader && shader->UsesCamera()) {
        g_counters.lastRenderTargetWidth = g_renderTargetWidth;
        g_counters.lastRenderTargetHeight = g_renderTargetHeight;
    }
    if (!shader || !shader->UsesCamera() || !IsMainRenderTarget()) return;
    ++g_renderTargetStats[g_currentRenderTarget].cameraDrawsThisFrame;
    const bool sceneTarget = IsSceneRenderTarget();
    if (sceneTarget) {
        ++g_counters.mainTargetCameraDraws;
        ++g_mainDrawsThisFrame;
        ObserveGameCamera(*shader);
        if (InSong()) NoteCharacterDraw(*shader);
    }
    if (!InSong()) CheckMenuCamera(*shader);
    if (g_currentDrawIsRolledUi) {
        // Menu text drawn with its own rolled camera. Into the scene: it goes onto the panel as
        // drawn. Into the menu's own text image: left alone, the game shows that image on its card.
        if (!sceneTarget) g_currentDrawIsRolledUi = false;
        return;
    }
    if (!VrCameraActive()) return;
    if (!g_vrMatricesValid) ComputeVrMatrices();
    if (!g_vrMatricesValid) return;
    g_vrCameraUsedThisFrame = true;
    ++g_counters.vrCameraDraws;

    if (g_drawListActive && g_drawListLines < 500) {
        ++g_drawListLines;
        Matrix4 listedWorld;
        bool haveListedWorld = false;
        if (shader->worldRegister != NoRegister) {
            listedWorld = Matrix4::FromRegisters(&g_gameConstants[shader->worldRegister]);
            haveListedWorld = true;
        } else if (shader->worldViewProjRegister != NoRegister && g_gameViewProjEverKnown) {
            listedWorld = Multiply(Matrix4::FromRegisters(&g_gameConstants[shader->worldViewProjRegister]), g_gameViewProjInverse);
            haveListedWorld = true;
        }
        const Vector3 head = { g_vrCameraToWorld.values[3][0], g_vrCameraToWorld.values[3][1], g_vrCameraToWorld.values[3][2] };
        if (haveListedWorld) {
            const float deltaX = listedWorld.values[3][0] - head.x, deltaZ = listedWorld.values[3][2] - head.z;
            LogLine("[draw list] %3u VS %08X %s pos %7.2f %7.2f %7.2f  scale %.2f  %.2f m from your head (flat)", g_drawListLines, shader->hash,
                    shader->worldRegister != NoRegister ? "world" : "wvp  ", listedWorld.values[3][0], listedWorld.values[3][1], listedWorld.values[3][2],
                    sqrtf(listedWorld.values[0][0] * listedWorld.values[0][0] + listedWorld.values[0][1] * listedWorld.values[0][1] +
                          listedWorld.values[0][2] * listedWorld.values[0][2]), sqrtf(deltaX * deltaX + deltaZ * deltaZ));
        } else {
            LogLine("[draw list] %3u VS %08X (no world matrix)", g_drawListLines, shader->hash);
        }
    }
    g_currentDrawUsedVrCamera = true;
    UploadVrCamera(device, shader);
}

// Uploads the selected eye's camera for this draw (World recovered from the game's matrices).
static void UploadVrCamera(IDirect3DDevice9* device, const ShaderInfo* shader) {
    // Recover this object's World matrix.
    Matrix4 world;
    bool haveWorld = false;
    if (shader->worldRegister != NoRegister) {
        world = Matrix4::FromRegisters(&g_gameConstants[shader->worldRegister]);
        haveWorld = true;
    } else if (shader->worldViewProjRegister != NoRegister && g_gameViewProjEverKnown) {
        world = Multiply(Matrix4::FromRegisters(&g_gameConstants[shader->worldViewProjRegister]), g_gameViewProjInverse);
        haveWorld = true;
    }

    if (g_currentDrawIsAvatar && haveWorld) {
        world = Multiply(world, g_avatarTransform);
        if (shader->worldRegister != NoRegister) UploadOverride(device, shader->worldRegister, &world.values[0][0], 4);
    }

    if (shader->viewProjRegister != NoRegister) {
        // Parts drawn in world space (no World matrix) move with the avatar through ViewProj instead.
        const bool worldSpaceAvatarPart = g_currentDrawIsAvatar && shader->worldRegister == NoRegister && shader->worldViewProjRegister == NoRegister;
        const Matrix4 viewProj = worldSpaceAvatarPart ? Multiply(g_avatarTransform, g_vrViewProj) : g_vrViewProj;
        UploadOverride(device, shader->viewProjRegister, &viewProj.values[0][0], 4);
    }

    if (shader->worldViewProjRegister != NoRegister && haveWorld) {
        Matrix4 worldViewProj = Multiply(world, g_vrViewProj);
        UploadOverride(device, shader->worldViewProjRegister, &worldViewProj.values[0][0], 4);
    }

    if (shader->worldViewRegister != NoRegister && haveWorld) {
        Matrix4 worldView = Multiply(world, g_vrView);
        UploadOverride(device, shader->worldViewRegister, &worldView.values[0][0], 4);
    }

    if (shader->inverseViewRegister != NoRegister) {
        float cameraAxes[3][4];
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                cameraAxes[row][column] = column < 3 ? g_vrCameraToWorld.values[row][column] : 0.0f;
        UploadOverride(device, shader->inverseViewRegister, &cameraAxes[0][0], 3);
    }
}

// ---------------------------------------------------------------------------
// Highway / HUD redirection into the overlay image
//
// The game draws these in screen space on top of the finished frame. While VR is
// running they go into a transparent render target instead, which becomes a floating
// panel. Alpha is accumulated as premultiplied "over" so the panel composites the
// same way the game blends onto the scene; additive effects keep the alpha untouched.
// ---------------------------------------------------------------------------
// What the game has bound (tracked by the hooks; used by full stereo and the 3D HUD).
constexpr DWORD TrackedTextureStages = 16;
static IDirect3DSurface9*     g_boundTargets[4] = {};
static IDirect3DSurface9*     g_boundDepth = nullptr;
static IDirect3DBaseTexture9* g_boundTextures[TrackedTextureStages] = {};

// Backbuffer-sized plus OverlayMargin pixels either side, A8R8G8B8: the HUD panel's image.
constexpr UINT OverlayMargin = 64;
static IDirect3DTexture9* g_overlayTexture = nullptr;
static IDirect3DSurface9* g_overlaySurface = nullptr;
static bool g_overlayClearedThisFrame = false;
static bool g_overlayDrawnThisFrame = false;
// 3D gems (songs): two extra panels on top of the HUD panel, at the same distance - the left and the
// right eye's gems, each gem drawn per eye with its own depth (see the gem relief below). They only
// cover the middle of the screen (GemArea: left, top, right, bottom fractions; hud_depth_area sets the
// width, wide enough by default for the rock meter left of the highway), so copying them to the
// headset costs less than a full panel. Only a HUD piece fully inside it can go 3D.
constexpr int HudLayerCount = 2;
static float GemArea[4] = { 0.13f, 0.17f, 0.87f, 0.98f };
static IDirect3DTexture9* g_hudLayerTextures[HudLayerCount] = {};
static IDirect3DSurface9* g_hudLayerSurfaces[HudLayerCount] = {};
static std::vector<BYTE> g_paddedHudVertices;   // a 3D HUD piece's quads, padded (kept to reuse the memory)
static bool g_hudLayerCleared[HudLayerCount] = {};
static bool g_hudLayerDrawn[HudLayerCount] = {};
// Something drawn over the highway after the gems (the pause menu): the gem panels are in front of the
// HUD panel, so they'd show through it. While that's up, gems are drawn flat with the rest instead.
static unsigned g_highwaysThisFrame = 0, g_highwaysLastFrame = 0;   // highway surfaces drawn (more than one: multiplayer)
static bool     g_gemDrawnThisFrame = false;   // a note on the highway: a song is being played (even with next to no venue)
static float    g_gemAreaCoveredThisFrame = 0.0f;   // fraction of the gem area drawn over after the gems
static unsigned g_gemsCoveredFrames = 0;           // frames left treating the gems as covered
static bool g_logHudDrawsThisFrame = false;
static unsigned g_hudDrawLines = 0;

// The game's screen inside an overlay image (pixels / texture coordinates).
static RECT OverlayScreenRect() {
    return { LONG(OverlayMargin), 0, LONG(OverlayMargin + g_backBufferWidth), LONG(g_backBufferHeight) };
}

// Menu mode: frames with no 3D scene (menus, loading) show the whole game screen as the panel.
// Decided from the previous frame, since a frame's draws happen before we know how it ends.
static bool g_previousFrameUsedVrCamera = false;

struct SavedBlendState {
    DWORD separateAlpha, sourceAlpha, destinationAlpha, operationAlpha;
};

// The mod's own resources and drawing must not be treated as the game's (full stereo would
// otherwise give them right-eye copies and repeat them).
static int g_modWorkDepth = 0;
struct ModWorkScope {
    ModWorkScope() { ++g_modWorkDepth; }
    ~ModWorkScope() { --g_modWorkDepth; }
};

static bool CreateOverlayTarget(IDirect3DDevice9* device, IDirect3DTexture9*& texture, IDirect3DSurface9*& surface) {
    if (surface) return true;
    ModWorkScope modWork;
    if (FAILED(device->CreateTexture(g_backBufferWidth + 2 * OverlayMargin, g_backBufferHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, &texture, nullptr))) {
        LogLine("[overlay] could not create %ux%u target", g_backBufferWidth + 2 * OverlayMargin, g_backBufferHeight);
        texture = nullptr;
        return false;
    }
    texture->GetSurfaceLevel(0, &surface);
    return true;
}

static bool EnsureOverlayTarget(IDirect3DDevice9* device) {
    return CreateOverlayTarget(device, g_overlayTexture, g_overlaySurface);
}

// The HUD goes onto the panel only when it's drawn onto the screen itself (or the scene image).
// 2D drawn into other screen-sized images (the menu text, before the game puts it on its angled
// card) must stay there.
static IDirect3DSurface9* g_backBufferSurface = nullptr;

static bool ShouldRedirectToOverlay(const ShaderInfo* shader) {
    if (!shader || !shader->IsScreenSpaceOverlay() || !IsMainRenderTarget() || !g_xr.IsRunning() || !g_previousFrameUsedVrCamera)
        return false;
    return g_currentRenderTarget == g_backBufferSurface || IsSceneRenderTarget();
}

// This draw's sampled vertices in mesh space (vertex buffer or user pointer); wValues optional.
static int CurrentDrawPositions(IDirect3DDevice9* device, Vector3* positions, int maximum, float* wValues = nullptr) {
    if (g_upVertexData)
        return MeshWatch::UserPointerPositions(device, g_upVertexData, g_upStride, g_drawFirstVertex, g_drawVertexCount, positions, maximum, wValues);
    return MeshWatch::DrawPositions(device, g_drawFirstVertex, g_drawVertexCount, positions, maximum, wValues);
}

// 3D HUD: which panel each HUD draw goes to.
constexpr uint32_t HudHighwayItemShader = 0xEC6EE373;  // gems, sustains, beat lines
constexpr uint32_t HudHighwaySurfaceShader = 0x29036C27;
constexpr uint32_t HudStarNoteShader = 0x5D4E10DA;     // star power notes
constexpr uint32_t HudSustainShader = 0x81A9E595;      // sustain trails
constexpr uint32_t HudOpenNoteShader = 0xEE7AEEA4;     // open notes (the bar across the highway)

enum class HudPiece { Highway, Gem };

struct HudDrawPlace {
    bool  known = false;   // vertices readable
    float centreX = 0.0f, centreY = 0.0f;   // screen pixels
};

// Screen rectangle a HUD draw covers (pixels); false if its vertices can't be read.
static bool HudBounds(IDirect3DDevice9* device, float& left, float& top, float& right, float& bottom) {
    Vector3 positions[256];
    float wValues[256];
    const int count = CurrentDrawPositions(device, positions, 256, wValues);
    if (!count) return false;
    left = top = 1e9f;
    right = bottom = -1e9f;
    for (int index = 0; index < count; ++index) {
        const float w = std::fabs(wValues[index]) > 1e-4f ? wValues[index] : 1.0f;
        left = std::min(left, positions[index].x / w);
        right = std::max(right, positions[index].x / w);
        top = std::min(top, positions[index].y / w);
        bottom = std::max(bottom, positions[index].y / w);
    }
    return true;
}

static HudDrawPlace HudPlace(IDirect3DDevice9* device) {
    HudDrawPlace place;
    Vector3 positions[256];
    float wValues[256];
    const int count = CurrentDrawPositions(device, positions, 256, wValues);
    if (!count) return place;
    double sumX = 0, sumY = 0;
    for (int index = 0; index < count; ++index) {
        const float w = std::fabs(wValues[index]) > 1e-4f ? wValues[index] : 1.0f;
        sumX += positions[index].x / w;
        sumY += positions[index].y / w;
    }
    place.known = true;
    place.centreX = float(sumX / count);
    place.centreY = float(sumY / count);
    return place;
}


// GHWT's highway on a 3840x2160 screen (from the HUD log): ~680 px either side of the middle at the
// strikeline (y 1888), ~330 at y 935. Scaled to the actual screen size.
static bool IsOverHighway(const HudDrawPlace& place) {
    if (!place.known) return true;   // unknown: keep it with the highway
    const float scaleX = float(g_backBufferWidth) / 3840.0f, scaleY = float(g_backBufferHeight) / 2160.0f;
    const float row = place.centreY / std::max(0.01f, scaleY);
    const float halfWidth = std::max(180.0f, 380.0f + (row - 935.0f) * 0.42f) * scaleX;
    return std::fabs(place.centreX - float(g_backBufferWidth) / 2) < halfWidth;
}


template <typename DrawCall>
static HRESULT DrawIntoOverlayTarget(IDirect3DDevice9* device, IDirect3DSurface9* target, bool& cleared, const D3DVIEWPORT9& gameViewport,
                                     DrawCall draw) {
    const int shift = 0;
    g_originalSetRenderTarget(device, 0, target);
    if (!cleared) {
        g_originalClear(device, 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        cleared = true;
    }
    // The game's screen sits OverlayMargin pixels in.
    D3DVIEWPORT9 viewport = gameViewport;
    viewport.X = DWORD(std::max(0, int(gameViewport.X) + int(OverlayMargin) + shift));
    device->SetViewport(&viewport);
    RECT scissor;
    device->GetScissorRect(&scissor);
    RECT shiftedScissor = { scissor.left + LONG(OverlayMargin) + shift, scissor.top, scissor.right + LONG(OverlayMargin) + shift, scissor.bottom };
    device->SetScissorRect(&shiftedScissor);
    const HRESULT result = draw();
    device->SetScissorRect(&scissor);
    return result;
}

// 3D HUD relief, hud_depth_tracking: which way (and how far) one eye sees a raised piece's top shifted,
// from where that eye really is. A point raised above the panel, seen from the eye, lands on the panel
// moved away from the eye by (piece - eye) * height / distance, so the texture is read the other way.
// Scaled so that an eye straight ahead at half a typical IPD, at hud_distance, gives the fixed offset
// (direction ±1, 0): tilting your head turns it, leaning in grows it, moving changes it. Mode 1 measures
// the piece as if it were straight ahead of your recentred head (both eyes' shared shift for pieces off
// to the side left out - it's the same in both eyes, so not depth, just the calmer look); mode 2 (the
// default) keeps it, as real raised pieces would look - the ray march and padded quads give it room.
static void ReliefViewDirection(int eye, float centreX, float centreY, float direction[2]) {
    if (g_settings.hudDepthTracking == 0 || !g_xr.IsRunning() || !g_backBufferWidth || !g_backBufferHeight) return;
    const XrVector3f eyePosition = g_xr.CurrentView(eye).pose.position;   // anchor space, like the HUD panel
    const OverlayPlacement& panel = g_settings.overlay;
    const float panelHeight = panel.width * float(g_backBufferHeight) / float(g_backBufferWidth);
    float pieceX = 0.0f, pieceY = 0.0f;   // mode 1: straight ahead of the recentred head
    if (g_settings.hudDepthTracking == 2) {
        pieceX = (centreX / float(g_backBufferWidth) - 0.5f) * panel.width;
        pieceY = panel.height + (0.5f - centreY / float(g_backBufferHeight)) * panelHeight;
    }
    const float eyeDistance = std::max(0.05f, eyePosition.z + panel.distance);
    constexpr float HalfIpd = 0.0315f;
    const float scale = panel.distance / HalfIpd / eyeDistance;
    direction[0] = -(pieceX - eyePosition.x) * scale;
    direction[1] = (pieceY - eyePosition.y) * scale;   // texture v runs down, the panel's y up
}

// A HUD piece split between the flat HUD and the 3D panels: its part outside the panels' area onto the
// flat HUD, in up to four scissored strips around the area (left, right, above, below).
template <typename DrawCall>
static void DrawOutsidePanelArea(IDirect3DDevice9* device, const D3DVIEWPORT9& gameViewport, DrawCall draw) {
    DWORD scissorEnabled = FALSE;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorEnabled);
    RECT gameScissor;
    device->GetScissorRect(&gameScissor);
    const LONG screenWidth = LONG(g_backBufferWidth), screenHeight = LONG(g_backBufferHeight);
    const LONG areaLeft = LONG(GemArea[0] * g_backBufferWidth), areaTop = LONG(GemArea[1] * g_backBufferHeight);
    const LONG areaRight = LONG(GemArea[2] * g_backBufferWidth), areaBottom = LONG(GemArea[3] * g_backBufferHeight);
    const RECT strips[4] = { { 0, 0, areaLeft, screenHeight }, { areaRight, 0, screenWidth, screenHeight },
                             { areaLeft, 0, areaRight, areaTop }, { areaLeft, areaBottom, areaRight, screenHeight } };
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    for (const RECT& strip : strips) {
        RECT clip = strip;   // screen pixels, like the game's scissor (DrawIntoOverlayTarget moves it in by the margin)
        if (scissorEnabled) {
            clip.left = std::max(clip.left, gameScissor.left);
            clip.top = std::max(clip.top, gameScissor.top);
            clip.right = std::min(clip.right, gameScissor.right);
            clip.bottom = std::min(clip.bottom, gameScissor.bottom);
        }
        if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
        device->SetScissorRect(&clip);
        DrawIntoOverlayTarget(device, g_overlaySurface, g_overlayClearedThisFrame, gameViewport, draw);
    }
    device->SetScissorRect(&gameScissor);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorEnabled);
}

template <typename DrawCall>
static HRESULT DrawIntoOverlay(IDirect3DDevice9* device, DrawCall draw, const ShaderInfo* shader = nullptr) {
    if (!EnsureOverlayTarget(device)) return draw();
    ModWorkScope modWork;

    IDirect3DSurface9* previousTarget = nullptr;
    device->GetRenderTarget(0, &previousTarget);
    D3DVIEWPORT9 gameViewport;
    device->GetViewport(&gameViewport);
    // The panel images are wider than the game's depth buffer, which D3D9 won't draw with: the HUD
    // is drawn without depth (it's flat on top of everything anyway).
    IDirect3DSurface9* previousDepth = nullptr;
    device->GetDepthStencilSurface(&previousDepth);
    g_originalSetDepthStencilSurface(device, nullptr);
    DWORD depthEnabled = D3DZB_FALSE;
    device->GetRenderState(D3DRS_ZENABLE, &depthEnabled);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

    SavedBlendState saved;
    device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &saved.separateAlpha);
    device->GetRenderState(D3DRS_SRCBLENDALPHA, &saved.sourceAlpha);
    device->GetRenderState(D3DRS_DESTBLENDALPHA, &saved.destinationAlpha);
    device->GetRenderState(D3DRS_BLENDOPALPHA, &saved.operationAlpha);
    DWORD colorDestinationBlend = D3DBLEND_ZERO;
    device->GetRenderState(D3DRS_DESTBLEND, &colorDestinationBlend);

    const bool isAdditive = colorDestinationBlend == D3DBLEND_ONE;
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, isAdditive ? D3DBLEND_ZERO : D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, isAdditive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);

    // 3D gems (songs): gems go onto the per-eye gem panels.
    // One highway only: with several (multiplayer) the 3D gem area / sizes don't fit, so gems stay flat.
    if (shader && shader->hash == HudHighwaySurfaceShader) ++g_highwaysThisFrame;
    const bool layeredHud = g_settings.hudDepth && (InSong() || g_framesSinceAnyHighway < SongTimeoutFrames) && shader && g_highwaysLastFrame <= 1;
    const HudDrawPlace place = g_logHudDrawsThisFrame ? HudPlace(device) : HudDrawPlace();
    // Gems: the highway-item shader's sprites, except the 32x32 ones (beat lines across the highway).
    D3DSURFACE_DESC boundTexture = {};
    if (shader && shader->hash == HudHighwayItemShader && g_boundTextures[0] && g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE)
        static_cast<IDirect3DTexture9*>(g_boundTextures[0])->GetLevelDesc(0, &boundTexture);
    const bool gemShader = shader && ((shader->hash == HudHighwayItemShader && !(boundTexture.Width == 32 && boundTexture.Height == 32)) ||
                                shader->hash == HudStarNoteShader || shader->hash == HudSustainShader || shader->hash == HudOpenNoteShader);
    if (gemShader) g_gemDrawnThisFrame = true;
    // ...and only over the highway, above the strikeline (the combo light and its number share the shader).
    const HudDrawPlace gemPlace = gemShader && layeredHud ? HudPlace(device) : HudDrawPlace();
    const bool gem = gemShader && gemPlace.known && IsOverHighway(gemPlace) && g_gemsCoveredFrames == 0 &&
                     gemPlace.centreY < (1888.0f + 40.0f) * float(g_backBufferHeight) / 2160.0f;
    // Its texture's height map (gems, and any other HUD piece someone has made one for).
    const Relief::Texture* reliefTexture = nullptr;
    if (layeredHud && g_gemsCoveredFrames == 0 && g_boundTextures[0] && shader->hash != HudHighwaySurfaceShader) {
        ModWorkScope modWork;   // loading a height map makes a texture: the mod's own
        IDirect3DBaseTexture9* const layers[2] = { g_boundTextures[1], g_boundTextures[2] };
        reliefTexture = &Relief::TextureFor(device, g_boundTextures[0], gem ? layers : nullptr);
    }
    // Other HUD pieces (strikeline, flames...) go onto the 3D panels once they have a height map - if they're
    // wholly inside the panels' area.
    float drawLeft, drawTop, drawRight, drawBottom;
    const bool haveBounds = layeredHud && HudBounds(device, drawLeft, drawTop, drawRight, drawBottom);
    const bool insidePanels = haveBounds && drawLeft >= GemArea[0] * g_backBufferWidth && drawRight <= GemArea[2] * g_backBufferWidth &&
                              drawTop >= GemArea[1] * g_backBufferHeight && drawBottom <= GemArea[3] * g_backBufferHeight;
    const bool raisedPiece = !gem && reliefTexture && reliefTexture->height && insidePanels;
    // The panels are always shown on top of the flat HUD, so a flat piece drawn after something on the
    // panels (the rock meter's face over its hidden star tubes, the score box over the combo meter) goes
    // onto the panels too, flat, to keep covering it as the game drew it. One only partly inside (often a
    // batch of HUD pieces in one draw, or one whose bounds can't be read) is split: on the panels whole,
    // and on the flat HUD only outside the panels' area.
    const bool outsidePanels = haveBounds && (drawRight <= GemArea[0] * g_backBufferWidth || drawLeft >= GemArea[2] * g_backBufferWidth ||
                                              drawBottom <= GemArea[1] * g_backBufferHeight || drawTop >= GemArea[3] * g_backBufferHeight);
    const bool overPanelPiece = !gem && !raisedPiece && layeredHud && !outsidePanels && g_gemsCoveredFrames == 0 && g_hudLayerDrawn[0];
    const bool coversPanelPiece = overPanelPiece && insidePanels;
    const bool splitPiece = overPanelPiece && !insidePanels;
    const bool onPanels = gem || raisedPiece || overPanelPiece;
    // Flat draws after this frame's gems: how much of the gem area do they cover?
    if (!gem && !raisedPiece && !coversPanelPiece && haveBounds && (g_hudLayerDrawn[0] || g_gemsCoveredFrames > 0)) {
        const float areaLeft = GemArea[0] * g_backBufferWidth, areaTop = GemArea[1] * g_backBufferHeight;
        const float areaRight = GemArea[2] * g_backBufferWidth, areaBottom = GemArea[3] * g_backBufferHeight;
        const float overlapWidth = std::max(0.0f, std::min(drawRight, areaRight) - std::max(drawLeft, areaLeft));
        const float overlapHeight = std::max(0.0f, std::min(drawBottom, areaBottom) - std::max(drawTop, areaTop));
        g_gemAreaCoveredThisFrame = std::max(g_gemAreaCoveredThisFrame,
                                             overlapWidth * overlapHeight / std::max(1.0f, (areaRight - areaLeft) * (areaBottom - areaTop)));
    }
    const HudPiece piece = onPanels ? HudPiece::Gem : HudPiece::Highway;
    char reliefNote[160] = "";   // HUD dump: how a 3D piece's relief was drawn
    HRESULT result;
    const bool gemPanels = piece == HudPiece::Gem && layeredHud &&
                           CreateOverlayTarget(device, g_hudLayerTextures[0], g_hudLayerSurfaces[0]) &&
                           CreateOverlayTarget(device, g_hudLayerTextures[1], g_hudLayerSurfaces[1]);
    if (gemPanels) {
        // Each eye's copy through the 3D copy of its pixel shader - if its texture has a height map
        // (gems without one go on the panels flat).
        IDirect3DPixelShader9* const reliefShader =
            (gem || raisedPiece) && reliefTexture && reliefTexture->height ? Relief::ReliefShader(device, g_currentPixelShader) : nullptr;
        // The shift is in texture coordinates, so it's scaled by how much of the texture one piece uses:
        // the regular gem is a whole 128-wide texture, the star notes are frames of a 512-wide sheet
        // (4 across), the open notes' bar is one 512-wide texture stretched across all five lanes.
        D3DSURFACE_DESC textureDescription = {};
        if (g_boundTextures[0] && g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE)
            static_cast<IDirect3DTexture9*>(g_boundTextures[0])->GetLevelDesc(0, &textureDescription);
        const float frameScale = (textureDescription.Width ? 128.0f / float(textureDescription.Width) : 1.0f) *
                                 (reliefTexture ? reliefTexture->strength : 1.0f);
        if (reliefShader) {
            g_originalSetTexture(device, Relief::HeightSampler, reliefTexture->height);
            device->SetSamplerState(Relief::HeightSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(Relief::HeightSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(Relief::HeightSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(Relief::HeightSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(Relief::HeightSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            const float brightness[4] = { 0.299f, 0.587f, 0.114f, 0.0f };
            g_originalSetPixelShaderConstantF(device, Relief::ZeroRegister, zero, 1);
            g_originalSetPixelShaderConstantF(device, Relief::BrightnessRegister, brightness, 1);
            g_originalSetPixelShader(device, reliefShader);
        }
        // The same in texels down as across (the height maps' depth is in texels of the texture).
        const float frameScaleY = (textureDescription.Height ? 128.0f / float(textureDescription.Height) : 1.0f) *
                                  (reliefTexture ? reliefTexture->strength : 1.0f);
        // Each eye's shift: nearer = the left eye sees it further right, so it reads the texture further
        // left (u - shift).
        float directions[2][2] = { { -1.0f, 0.0f }, { 1.0f, 0.0f } };
        if (reliefShader && haveBounds)
            for (int eye = 0; eye < 2; ++eye) ReliefViewDirection(eye, (drawLeft + drawRight) / 2, (drawTop + drawBottom) / 2, directions[eye]);
        // Only the shift both eyes share is limited (it's what reads far outside the piece); the difference
        // between them is the depth, so it's kept.
        constexpr float SharedLimit = 8.0f;
        for (int axis = 0; axis < 2; ++axis) {
            const float shared = (directions[0][axis] + directions[1][axis]) / 2;
            const float limited = std::clamp(shared, -SharedLimit, SharedLimit);
            directions[0][axis] += limited - shared;
            directions[1][axis] += limited - shared;
        }
        float shifts[2][4] = {};
        for (int eye = 0; eye < 2; ++eye) {
            shifts[eye][0] = directions[eye][0] * g_settings.hudGemBulge * frameScale;
            shifts[eye][1] = directions[eye][1] * g_settings.hudGemBulge * frameScaleY;
        }
        // Ray-marched: the piece's own frame of its texture (nothing read past it shows), and its quads
        // padded by the biggest shift so a raised part has room past the flat outline.
        std::vector<BYTE>& paddedVertices = g_paddedHudVertices;
        bool padded = false;
        if (reliefShader && Relief::RayMarches(g_currentPixelShader)) {
            float frame[4] = { -1e4f, -1e4f, 1e4f, 1e4f };   // unknown: no limit
            if (g_upVertexData) {
                const HudPadding::Draw upDraw = { g_drawPrimitiveType, g_upVertexData, g_upStride, g_drawFirstVertex, g_drawVertexCount,
                                                  g_upPrimitiveCount, g_upIndexData, g_upIndexFormat };
                if (!HudPadding::FrameOf(device, upDraw, frame)) frame[0] = frame[1] = -1e4f, frame[2] = frame[3] = 1e4f;
                const float marginU = std::max(std::fabs(shifts[0][0]), std::fabs(shifts[1][0]));
                const float marginV = std::max(std::fabs(shifts[0][1]), std::fabs(shifts[1][1]));
                padded = HudPadding::PadQuads(device, upDraw, marginU, marginV, paddedVertices);
            }
            g_originalSetPixelShaderConstantF(device, Relief::FrameRegister, frame, 1);
        }
        if (g_logHudDrawsThisFrame && reliefShader)
            snprintf(reliefNote, sizeof(reliefNote), ", relief %s, %s, %s %u, shift L %.4f %.4f R %.4f %.4f",
                     Relief::RayMarches(g_currentPixelShader) ? "ray-marched" : "simple", padded ? "padded" : "not padded",
                     g_upIndexData ? "indexed" : "type", g_upIndexData ? 0u : unsigned(g_drawPrimitiveType),
                     shifts[0][0], shifts[0][1], shifts[1][0], shifts[1][1]);
        else if (g_logHudDrawsThisFrame && (gem || raisedPiece))
            snprintf(reliefNote, sizeof(reliefNote), ", relief none (%s)", reliefTexture && reliefTexture->height ? "no 3D shader copy" : "no height map");
        const auto paddedDraw = [&]() {
            return g_upIndexData ? g_originalDrawIndexedPrimitiveUP(device, g_drawPrimitiveType, g_drawFirstVertex, g_drawVertexCount, g_upPrimitiveCount,
                                                                   g_upIndexData, g_upIndexFormat, paddedVertices.data(), g_upStride)
                                 : g_originalDrawPrimitiveUP(device, g_drawPrimitiveType, g_upPrimitiveCount, paddedVertices.data(), g_upStride);
        };
        result = S_OK;
        for (int eye = 0; eye < 2; ++eye) {
            if (reliefShader) g_originalSetPixelShaderConstantF(device, Relief::ShiftRegister, shifts[eye], 1);
            const HRESULT eyeResult = padded ? DrawIntoOverlayTarget(device, g_hudLayerSurfaces[eye], g_hudLayerCleared[eye], gameViewport, paddedDraw)
                                             : DrawIntoOverlayTarget(device, g_hudLayerSurfaces[eye], g_hudLayerCleared[eye], gameViewport, draw);
            if (eye == 0) result = eyeResult;
            g_hudLayerDrawn[eye] = true;
        }
        if (reliefShader) {
            g_originalSetPixelShader(device, g_currentPixelShader);
            g_originalSetPixelShaderConstantF(device, Relief::FrameRegister, g_pixelConstants[Relief::FrameRegister], 4);
            g_originalSetTexture(device, Relief::HeightSampler, g_boundTextures[Relief::HeightSampler]);
        }
        if (splitPiece) DrawOutsidePanelArea(device, gameViewport, draw);
    } else {
        result = DrawIntoOverlayTarget(device, g_overlaySurface, g_overlayClearedThisFrame, gameViewport, draw);
    }
    if (g_logHudDrawsThisFrame && shader && g_hudDrawLines < 120) {
        ++g_hudDrawLines;
        auto pixelShader = g_pixelShaders.find(g_currentPixelShader);
        D3DSURFACE_DESC description = {};
        IDirect3DBaseTexture9* texture = g_boundTextures[0];
        if (texture && texture->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &description);
        char bounds[64] = "bounds unknown";
        if (haveBounds) snprintf(bounds, sizeof(bounds), "x %.0f-%.0f y %.0f-%.0f", drawLeft, drawRight, drawTop, drawBottom);
        DWORD sourceBlend = 0, destinationBlend = 0;
        device->GetRenderState(D3DRS_SRCBLEND, &sourceBlend);
        device->GetRenderState(D3DRS_DESTBLEND, &destinationBlend);
        LogLine("[hud] VS %08X PS %08X %s texture %ux%u #%08X, %u vertices %s at %.0f %.0f (%s, blend %lu/%lu) -> %s%s%s", shader->hash,
                pixelShader != g_pixelShaders.end() ? pixelShader->second.hash : 0u, shader->isHighway ? "highway" : "other  ",
                description.Width, description.Height, (g_settings.debugTools ? Relief::SaveTextureFile(texture) : 0u), g_drawVertexCount, g_upVertexData ? "(user pointer)" : "", place.centreX, place.centreY,
                bounds, sourceBlend, destinationBlend,
                gem ? "gem (3D)" : raisedPiece ? "raised (3D)" : coversPanelPiece ? "on the panels (covers 3D)" : splitPiece ? "split (covers 3D)" : "flat",
                reliefNote, layeredHud ? "" : " (3D gems off)");
    }
    ++g_counters.overlayDraws;

    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, saved.separateAlpha);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, saved.sourceAlpha);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, saved.destinationAlpha);
    device->SetRenderState(D3DRS_BLENDOPALPHA, saved.operationAlpha);
    if (previousTarget) {
        g_originalSetRenderTarget(device, 0, previousTarget);
        previousTarget->Release();
    }
    g_originalSetDepthStencilSurface(device, previousDepth);
    if (previousDepth) previousDepth->Release();
    device->SetRenderState(D3DRS_ZENABLE, depthEnabled);
    device->SetViewport(&gameViewport);
    g_overlayDrawnThisFrame = true;
    return result;
}


// Depth of field: the game blurs by distance from its scripted focus, which in VR (where you
// look wherever you like, at lower resolution) just looks short-sighted. Its passes are found by
// their shader constants (m_DOF_*, m_vDepthOfFieldParams, m_NearBlurParams) and skipped.
enum class DepthOfFieldAction { Draw, DrawWithoutBlur, Skip };

static DepthOfFieldAction DepthOfFieldActionForDraw(IDirect3DDevice9* device) {
    auto found = g_pixelShaders.find(g_currentPixelShader);
    if (found == g_pixelShaders.end() || !found->second.isDepthOfField) return DepthOfFieldAction::Draw;
    ++g_counters.depthOfFieldDraws;

    static std::vector<IDirect3DPixelShader9*> described;
    if (std::find(described.begin(), described.end(), g_currentPixelShader) == described.end() && described.size() < 32) {
        described.push_back(g_currentPixelShader);
        DWORD blendEnabled = 0, sourceBlend = 0, destinationBlend = 0;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendEnabled);
        device->GetRenderState(D3DRS_SRCBLEND, &sourceBlend);
        device->GetRenderState(D3DRS_DESTBLEND, &destinationBlend);
        LogLine("[dof] pass %p drawn into %ux%u, blend %lu (%lu/%lu): %s", (void*)g_currentPixelShader,
                g_renderTargetWidth, g_renderTargetHeight, blendEnabled, sourceBlend, destinationBlend,
                found->second.registerMap.c_str());
        for (int index = 0; index < 32; ++index)
            LogLine("[dof]   c%d = %.4f %.4f %.4f %.4f", index, g_pixelConstants[index][0], g_pixelConstants[index][1],
                    g_pixelConstants[index][2], g_pixelConstants[index][3]);
    }
    if (!g_settings.depthOfFieldOff || !(g_vrCameraUsedThisFrame || g_previousFrameUsedVrCamera)) return DepthOfFieldAction::Draw;
    ++g_counters.depthOfFieldSkipped;
    // The final composite (g_DOF) also adds bloom: keep the pass, just zero the blur strength.
    return found->second.depthOfFieldRegister >= 0 ? DepthOfFieldAction::DrawWithoutBlur : DepthOfFieldAction::Skip;
}


// ---------------------------------------------------------------------------
// Your band member: left alone, hidden, or moved to where you stand so you see their body and
// instrument when you look down.
//
// Every part of a band member (body, face, hair, instrument, sticks) is drawn with the same World
// translation: the character's root, at their feet. Roots with skin-shader draws are the band
// members. Walking into one picks them (H cycles / none); they're followed from frame to frame as
// they move about.
// ---------------------------------------------------------------------------
enum BandMemberMode { LeaveBandMember = 0, HideBandMember = 1, BecomeBandMember = 2 };

struct CharacterRoot {
    Vector3 position;
    unsigned draws;
    float yaw;          // facing, from a skin draw's World rotation
};
static std::vector<CharacterRoot> g_charactersThisFrame;   // collected while drawing
static std::vector<CharacterRoot> g_characters;            // last finished frame, sorted left to right
static bool    g_chosenCharacterLocked = false;
static Vector3 g_chosenCharacterRoot = {};
static float   g_chosenCharacterYaw = 0.0f;
static bool    g_choiceIsManual = false;     // H was used: don't auto-pick again this song

// "Become" placement, recomputed each frame (g_avatarTransform is declared above PrepareCameraForDraw).

constexpr float CharacterPartTolerance = 0.1f;   // all parts share the root; allows a frame of movement
constexpr float CharacterFollowStep = 0.6f;      // furthest a band member moves between frames
constexpr float CharacterPickRadius = 0.6f;      // walk this close (flat) to a band member to pick them
constexpr float InstrumentPartRadius = 1.5f;     // guitar strings are drawn from the guitar, not the root
constexpr uint32_t GuitarStringsShader = 0x415420CC;
constexpr uint32_t FaceShader = 0x8FA892F2;
constexpr uint32_t HairShader = 0x9F85BB97;

static float FlatDistance(const Vector3& first, const Vector3& second) {
    const float deltaX = first.x - second.x, deltaZ = first.z - second.z;
    return sqrtf(deltaX * deltaX + deltaZ * deltaZ);
}

static bool WorldTranslation(const ShaderInfo& shader, Vector3& position) {
    if (shader.worldRegister == NoRegister) return false;
    const float* row = g_gameConstants[shader.worldRegister + 3];
    position = { row[0], row[1], row[2] };
    return true;
}

static void NoteCharacterDraw(const ShaderInfo& shader) {
    if (!shader.isCharacterSkin) return;
    Vector3 position;
    if (!WorldTranslation(shader, position)) return;
    for (CharacterRoot& root : g_charactersThisFrame) {
        if (FlatDistance(root.position, position) < 0.01f && fabsf(root.position.y - position.y) < 0.01f) { ++root.draws; return; }
    }
    const float* backRow = g_gameConstants[shader.worldRegister + 2];
    if (g_charactersThisFrame.size() < 64) g_charactersThisFrame.push_back({ position, 1, atan2f(backRow[0], backRow[2]) });
}

static bool IsChosenCharacterDraw(const ShaderInfo& shader) {
    if (g_settings.bandMemberMode == LeaveBandMember || !g_chosenCharacterLocked) return false;
    Vector3 position;
    if (!WorldTranslation(shader, position)) return false;
    const float distance = FlatDistance(position, g_chosenCharacterRoot);
    if (shader.hash == GuitarStringsShader) {
        // Your own character's strings come with their World at the world origin (the vertices are already
        // placed on stage) - the other band members' have a World at their spot. Found with the draw finder.
        const bool atOrigin = fabsf(position.x) < 0.01f && fabsf(position.y) < 0.01f && fabsf(position.z) < 0.01f;
        return atOrigin || distance < InstrumentPartRadius;
    }
    return distance < CharacterPartTolerance && fabsf(position.y - g_chosenCharacterRoot.y) < 0.5f;
}

static Vector3 HeadPosition() {
    return { g_vrCameraToWorld.values[3][0], g_vrCameraToWorld.values[3][1], g_vrCameraToWorld.values[3][2] };
}

// Where their head really is, and which way they face: read from their posed vertices (the game
// poses them on the CPU, so the World matrix alone only says where their spot on stage is).
// - head: the middle of their face mesh. Their arms use the face's shader too, so of those parts
//   the highest is taken.
// - facing: their shoulder line (the widest direction of the body at shoulder height), turned to
//   the side their face is on. With avatar_lock_turn their turning (spins) is cancelled.
constexpr int MaxBodyPoints = 2048;
static Vector3  g_faceThisFrame = {};       // the highest face-shader part this frame (world)
static Vector3  g_faceLocalThisFrame = {};  // ...in their own space
static unsigned g_faceParts = 0;
static Vector3  g_bodyPoints[MaxBodyPoints];   // this frame's body vertices, their own space
static int      g_bodyPointCount = 0;
static Vector3  g_characterHead = {};
static bool     g_characterHeadValid = false;
static float    g_characterWorld[4][4] = {};     // their World (their spot on stage), from a skin draw
static Vector3  g_chestThisFrame = {};           // middle of their chest (world), when measured
static bool     g_chestMeasuredThisFrame = false;
static Vector3  g_characterChest = {};           // smoothed
static bool     g_chestValid = false;
static unsigned g_framesWithoutFace = 0;
static float    g_characterFacing = 0.0f;   // their own space, smoothed
static float    g_restFacing = 0.0f;        // the way they face when not turning (slow average)
static bool     g_facingValid = false;
static unsigned g_facingReadsThisInterval = 0;
static unsigned g_worldSpacePartsThisInterval = 0;   // diagnostics: strings etc. found by their vertices
static unsigned g_faceReadsThisInterval = 0;
static bool     g_logNearbyThisFrame = false;   // diagnostics: list what's near them but not theirs
static unsigned g_nearbyLines = 0;

static void LockCharacter(const CharacterRoot& character) {
    g_nearbyDiagnosticFrames = g_settings.debugTools ? 2 : 0;   // log what's near them but not counted as theirs
    g_chosenCharacterRoot = character.position;
    g_chosenCharacterYaw = character.yaw;
    g_chosenCharacterLocked = true;
    g_characterHeadValid = false;
    g_facingValid = false;
    g_chestValid = false;
}

static Vector3 TransformPoint(const Vector3& point, const float (*rows)[4]) {
    return { point.x * rows[0][0] + point.y * rows[1][0] + point.z * rows[2][0] + rows[3][0],
             point.x * rows[0][1] + point.y * rows[1][1] + point.z * rows[2][1] + rows[3][1],
             point.x * rows[0][2] + point.y * rows[1][2] + point.z * rows[2][2] + rows[3][2] };
}


static Vector3 Average(const Vector3* positions, int count) {
    Vector3 sum = {};
    for (int index = 0; index < count; ++index) sum = { sum.x + positions[index].x, sum.y + positions[index].y, sum.z + positions[index].z };
    return { sum.x / count, sum.y / count, sum.z / count };
}

// One of your band member's own draws (World at their spot): note their face and body vertices.
static void NoteChosenCharacterDraw(IDirect3DDevice9* device, const ShaderInfo& shader) {
    if (shader.worldRegister == NoRegister || !shader.isCharacterSkin) return;
    memcpy(g_characterWorld, g_gameConstants[shader.worldRegister], sizeof(g_characterWorld));
    Vector3 positions[256];
    const int count = CurrentDrawPositions(device, positions, 256);
    if (!count) return;
    if (shader.hash == FaceShader) {
        const Vector3 local = Average(positions, count);
        const Vector3 world = TransformPoint(local, &g_gameConstants[shader.worldRegister]);
        // Sanity: a head is within a few metres of their spot (else it's not the face's own vertices).
        if (FlatDistance(world, g_chosenCharacterRoot) > 3.0f || fabsf(world.y - g_chosenCharacterRoot.y) > 3.0f) return;
        if (!g_faceParts || world.y > g_faceThisFrame.y) { g_faceThisFrame = world; g_faceLocalThisFrame = local; }
        ++g_faceParts;
    }
    for (int index = 0; index < count && g_bodyPointCount < MaxBodyPoints; ++index) g_bodyPoints[g_bodyPointCount++] = positions[index];
}

// Draws of theirs that aren't drawn from their spot (guitar strings and the like: drawn straight in
// world space, or with a World of their own): theirs when their vertices are close to your band member.
constexpr float NearbyPartRadius = 1.0f;

static bool IsChosenCharacterNearbyDraw(IDirect3DDevice9* device, const ShaderInfo& shader) {
    if (g_settings.bandMemberMode == LeaveBandMember || !g_chosenCharacterLocked || !shader.UsesCamera()) return false;
    if (shader.isCharacterSkin) return false;   // other band members' bodies (theirs are matched by their spot)
    // Only draws that could be theirs: world-space / WVP-only / user-pointer draws, or a World near them.
    const bool worldSpace = shader.worldRegister == NoRegister && shader.worldViewProjRegister == NoRegister;
    const bool wvpOnly = shader.worldRegister == NoRegister && shader.worldViewProjRegister != NoRegister;
    Vector3 translation;
    const bool haveTranslation = WorldTranslation(shader, translation);
    // A World near them, or one at the world origin (vertices already placed in the world).
    const bool nearWorld = haveTranslation && (FlatDistance(translation, g_chosenCharacterRoot) < 3.0f ||
                                               (fabsf(translation.x) < 0.01f && fabsf(translation.y) < 0.01f && fabsf(translation.z) < 0.01f));
    if (!worldSpace && !wvpOnly && !nearWorld && !g_upVertexData) return false;

    Vector3 positions[256];
    const int count = CurrentDrawPositions(device, positions, 256);
    if (!count) {
        if (g_logNearbyThisFrame && g_nearbyLines < 40 && (nearWorld || g_upVertexData)) {
            ++g_nearbyLines;
            LogLine("[band near] VS %08X %s%s: its vertices can't be read (not rewritten each frame)%s", shader.hash,
                    worldSpace ? "world-space" : wvpOnly ? "WVP-only" : "own World", g_upVertexData ? " (user pointer)" : "",
                    nearWorld ? " - World is near them" : "");
        }
        return false;
    }
    Vector3 centre = Average(positions, count);
    if (shader.worldRegister != NoRegister) {
        centre = TransformPoint(centre, &g_gameConstants[shader.worldRegister]);
    } else if (wvpOnly) {
        if (!g_gameViewProjEverKnown) return false;
        const Matrix4 world = Multiply(Matrix4::FromRegisters(&g_gameConstants[shader.worldViewProjRegister]), g_gameViewProjInverse);
        centre = TransformPoint(centre, world.values);
    }
    const Vector3 body = g_characterHeadValid ? g_characterHead : g_chosenCharacterRoot;
    const float distance = FlatDistance(centre, body);
    const bool heightOk = centre.y > g_chosenCharacterRoot.y - 0.3f && centre.y < body.y + 0.5f;
    if (g_logNearbyThisFrame && distance < 2.5f && g_nearbyLines < 40) {
        ++g_nearbyLines;
        LogLine("[band near] VS %08X %s%s centre %.2f %.2f %.2f, %.2f m from them (flat)%s", shader.hash,
                worldSpace ? "world-space" : wvpOnly ? "WVP-only" : "own World", g_upVertexData ? " (user pointer)" : "",
                centre.x, centre.y, centre.z, distance, distance <= NearbyPartRadius && heightOk ? " -> counted as theirs" : "");
    }
    if (distance > NearbyPartRadius || !heightOk) return false;
    ++g_worldSpacePartsThisInterval;
    return true;
}

static float WrapAngle(float angle) {
    while (angle > 3.14159265f) angle -= 6.2831853f;
    while (angle < -3.14159265f) angle += 6.2831853f;
    return angle;
}

// Their facing (own space) from this frame's body vertices: shoulder line, turned to the face side.
static bool MeasureFacing(float& facing) {
    if (!g_faceParts || g_bodyPointCount < 40) return false;
    // Chest height only, and only near the middle of the body (arms and guitar neck stick out).
    const float headHeight = g_faceLocalThisFrame.y;
    auto inChestBand = [&](const Vector3& point) { return point.y > headHeight - 0.5f && point.y < headHeight - 0.25f; };
    double sumX = 0, sumZ = 0;
    int used = 0;
    for (int index = 0; index < g_bodyPointCount; ++index) {
        const Vector3& point = g_bodyPoints[index];
        if (!inChestBand(point) || FlatDistance(point, g_faceLocalThisFrame) > 0.45f) continue;
        sumX += point.x;
        sumZ += point.z;
        ++used;
    }
    if (used < 20) return false;
    const double centreX = sumX / used, centreZ = sumZ / used;
    sumX = sumZ = 0;
    used = 0;
    for (int index = 0; index < g_bodyPointCount; ++index) {
        const Vector3& point = g_bodyPoints[index];
        const float deltaX = point.x - float(centreX), deltaZ = point.z - float(centreZ);
        if (!inChestBand(point) || deltaX * deltaX + deltaZ * deltaZ > 0.3f * 0.3f) continue;
        sumX += point.x;
        sumZ += point.z;
        ++used;
    }
    if (used < 20) return false;
    const double meanX = sumX / used, meanZ = sumZ / used;
    double xx = 0, zz = 0, xz = 0;
    for (int index = 0; index < g_bodyPointCount; ++index) {
        const Vector3& point = g_bodyPoints[index];
        const float fromCentreX = point.x - float(centreX), fromCentreZ = point.z - float(centreZ);
        if (!inChestBand(point) || fromCentreX * fromCentreX + fromCentreZ * fromCentreZ > 0.3f * 0.3f) continue;
        const double deltaX = point.x - meanX, deltaZ = point.z - meanZ;
        xx += deltaX * deltaX;
        zz += deltaZ * deltaZ;
        xz += deltaX * deltaZ;
    }
    // Shoulder line = the main axis; needs to be clearly wider one way than the other.
    const double trace = xx + zz, determinant = xx * zz - xz * xz;
    const double spread = sqrt(std::max(0.0, trace * trace / 4 - determinant));
    const double major = trace / 2 + spread, minor = trace / 2 - spread;
    // The pin point: the middle of their body avatar_pin_drop below their face (waist by default).
    {
        const float pinHeight = headHeight - g_settings.avatarPinDrop;
        double pinX = 0, pinZ = 0;
        int pinCount = 0;
        for (int index = 0; index < g_bodyPointCount; ++index) {
            const Vector3& point = g_bodyPoints[index];
            const float fromCentreX = point.x - float(centreX), fromCentreZ = point.z - float(centreZ);
            if (fabsf(point.y - pinHeight) > 0.12f || fromCentreX * fromCentreX + fromCentreZ * fromCentreZ > 0.35f * 0.35f) continue;
            pinX += point.x;
            pinZ += point.z;
            ++pinCount;
        }
        const float useX = pinCount >= 8 ? float(pinX / pinCount) : float(meanX);
        const float useZ = pinCount >= 8 ? float(pinZ / pinCount) : float(meanZ);
        g_chestThisFrame = TransformPoint({ useX, pinHeight, useZ }, g_characterWorld);
        g_chestMeasuredThisFrame = true;
    }
    if (major < 1e-6 || major < minor * 1.3) return false;
    const double axisAngle = 0.5 * atan2(2 * xz, xx - zz);   // in the x/z plane
    double facingX = -sin(axisAngle), facingZ = cos(axisAngle);   // perpendicular to the shoulders
    if (g_facingValid) {
        // Front or back: whichever is closer to last frame's (a turn can't flip it in one frame).
        if (fabsf(WrapAngle(float(atan2(facingX, facingZ)) - g_characterFacing)) > 1.5707963f) { facingX = -facingX; facingZ = -facingZ; }
    } else if (facingX * (g_faceLocalThisFrame.x - meanX) + facingZ * (g_faceLocalThisFrame.z - meanZ) < 0) {
        facingX = -facingX;   // first reading: the face is in front of the chest
        facingZ = -facingZ;
    }
    facing = float(atan2(facingX, facingZ));
    return true;
}

// Once per frame: roll this frame's reads into their head position and facing.
static void UpdateCharacterHead() {
    if (g_faceParts) {
        const Vector3 measured = g_faceThisFrame;
        if (!g_characterHeadValid) {
            g_characterHead = measured;
        } else {
            // Light smoothing: animation jitter out, real moves (leaning) in within a few frames.
            const float follow = 0.5f;
            g_characterHead = { g_characterHead.x + (measured.x - g_characterHead.x) * follow,
                                g_characterHead.y + (measured.y - g_characterHead.y) * follow,
                                g_characterHead.z + (measured.z - g_characterHead.z) * follow };
        }
        g_characterHeadValid = true;
        g_framesWithoutFace = 0;
        ++g_faceReadsThisInterval;
    } else if (++g_framesWithoutFace > 30) {
        g_characterHeadValid = false;
        g_facingValid = false;
        g_chestValid = false;
    }
    float facing;
    g_chestMeasuredThisFrame = false;
    const bool facingMeasured = MeasureFacing(facing);
    if (g_chestMeasuredThisFrame) {
        g_characterChest = !g_chestValid ? g_chestThisFrame
            : Vector3{ g_characterChest.x + (g_chestThisFrame.x - g_characterChest.x) * 0.85f, g_chestThisFrame.y,
                       g_characterChest.z + (g_chestThisFrame.z - g_characterChest.z) * 0.85f };
        g_chestValid = true;
    }
    if (facingMeasured) {
        if (!g_facingValid) {
            g_characterFacing = facing;
            // Normal facing: their model's own forward (along its z axis, whichever way is nearer),
            // not whatever pose they were in when picked.
            g_restFacing = fabsf(facing) < 1.5707963f ? 0.0f : 3.14159265f;
            g_facingValid = true;
        } else {
            g_characterFacing = WrapAngle(g_characterFacing + WrapAngle(facing - g_characterFacing) * 0.2f);
        }
        ++g_facingReadsThisInterval;
    }
    g_faceParts = 0;
    g_bodyPointCount = 0;
}

// Places the chosen band member under you, facing the way the game has them (towards the crowd).
// Their head is clipped away (see DrawAvatarPart).
// - avatar_anchor 1: their head at your head (slid avatar_offset forward, avatar_head_drop down),
//   however their animation moves them; avatar_lock_turn also turns them back when they turn.
// - avatar_anchor 0: their feet avatar_eye_height below your eyes.
static void UpdateAvatarTransform() {
    const Vector3 head = HeadPosition();
    const float offset = g_settings.avatarOffset * g_settings.worldScale;
    g_avatarTransform = Matrix4::Identity();
    if (g_settings.avatarAnchor == 1 && g_characterHeadValid) {
        const Vector3 target = { head.x + sinf(g_chosenCharacterYaw) * offset, head.y - g_settings.avatarHeadDrop * g_settings.worldScale,
                                 head.z + cosf(g_chosenCharacterYaw) * offset };
        // Steady them by their chest across (swaying hips move less under you) and their head up/down:
        // move that point to the origin, turn back by however far they've turned, then put it at yours.
        const float turnBack = g_settings.avatarLockTurn && g_facingValid ? WrapAngle(g_restFacing - g_characterFacing) : 0.0f;
        const bool useChest = g_chestValid;
        Matrix4 toOrigin = Matrix4::Identity();
        toOrigin.values[3][0] = -(useChest ? g_characterChest.x : g_characterHead.x);
        toOrigin.values[3][1] = -g_characterHead.y;
        toOrigin.values[3][2] = -(useChest ? g_characterChest.z : g_characterHead.z);
        g_avatarTransform = Multiply(toOrigin, YawTranslation(turnBack, target));
        return;
    }
    const Vector3 feet = { head.x + sinf(g_chosenCharacterYaw) * offset, head.y - g_settings.avatarEyeHeight * g_settings.worldScale,
                           head.z + cosf(g_chosenCharacterYaw) * offset };
    g_avatarTransform.values[3][0] = feet.x - g_chosenCharacterRoot.x;
    g_avatarTransform.values[3][1] = feet.y - g_chosenCharacterRoot.y;
    g_avatarTransform.values[3][2] = feet.z - g_chosenCharacterRoot.z;
}

// Once per frame (Present): roll the collected roots over, pick / follow the chosen band member.
static bool    g_pickNearPending = false;   // pick the band member standing here once they're seen
static Vector3 g_pickNearPosition = {};

static void UpdateChosenCharacter() {
    g_characters.clear();
    for (const CharacterRoot& root : g_charactersThisFrame)
        if (root.draws >= 2) g_characters.push_back(root);   // a body is several draws
    g_charactersThisFrame.clear();
    UpdateCharacterHead();
    std::sort(g_characters.begin(), g_characters.end(),
              [](const CharacterRoot& left, const CharacterRoot& right) { return left.position.x < right.position.x; });
    if (g_characters.empty() || g_settings.bandMemberMode == LeaveBandMember) return;

    if (g_chosenCharacterLocked) {
        float best = CharacterFollowStep;
        const CharacterRoot* nearest = nullptr;
        for (const CharacterRoot& character : g_characters) {
            const float distance = FlatDistance(character.position, g_chosenCharacterRoot);
            if (distance < best) { best = distance; nearest = &character; }
        }
        if (nearest) { g_chosenCharacterRoot = nearest->position; g_chosenCharacterYaw = nearest->yaw; }
    } else if (!g_choiceIsManual && g_pickNearPending) {
        // Put somewhere at a song's start: the band member standing there is yours.
        for (const CharacterRoot& character : g_characters) {
            if (FlatDistance(character.position, g_pickNearPosition) < 1.0f) {
                LockCharacter(character);
                g_pickNearPending = false;
                LogLine("[band] your band member: the one where you were put (at %.2f %.2f %.2f) - H to change",
                        character.position.x, character.position.y, character.position.z);
                break;
            }
        }
    } else if (!g_choiceIsManual && g_settings.walkToPick) {
        // Walk into a band member (from then on they're followed / placed under you).
        const Vector3 head = HeadPosition();
        for (const CharacterRoot& character : g_characters) {
            if (FlatDistance(character.position, head) < CharacterPickRadius) {
                LockCharacter(character);
                LogLine("[band] picked the band member you walked into (at %.2f %.2f %.2f) - H to change",
                        character.position.x, character.position.y, character.position.z);
                break;
            }
        }
    }
    if (g_chosenCharacterLocked && g_settings.bandMemberMode == BecomeBandMember) UpdateAvatarTransform();
}

// H: none -> each band member left to right -> none.
static void CycleChosenCharacter() {
    if (g_characters.empty()) { LogLine("[band] no band members in view to pick from"); return; }
    g_choiceIsManual = true;
    int current = -1;
    if (g_chosenCharacterLocked) {
        float best = 0.5f;
        for (size_t index = 0; index < g_characters.size(); ++index) {
            const float distance = FlatDistance(g_characters[index].position, g_chosenCharacterRoot);
            if (distance < best) { best = distance; current = int(index); }
        }
    }
    const int next = current + 1;
    if (next >= int(g_characters.size())) {
        g_chosenCharacterLocked = false;
        LogLine("[band] no band member chosen");
        return;
    }
    LockCharacter(g_characters[next]);
    LogLine("[band] band member %d of %u chosen (at %.2f %.2f %.2f)", next + 1, unsigned(g_characters.size()),
            g_chosenCharacterRoot.x, g_chosenCharacterRoot.y, g_chosenCharacterRoot.z);
}

static void CycleBandMemberMode() {
    g_settings.bandMemberMode = (g_settings.bandMemberMode + 1) % 3;
    static const char* const modeNames[] = { "left alone", "hidden", "you become them" };
    LogLine("[band] your band member: %s (H picks which; F11 saves)", modeNames[g_settings.bandMemberMode]);
}

// Diagnostics: something near your band member that isn't counted as theirs (e.g. guitar strings).
static void LogDrawNearChosenCharacter(const ShaderInfo& shader) {
    static unsigned lines = 0;
    if (lines > 200) return;
    Vector3 position = {};
    const char* source = nullptr;
    if (WorldTranslation(shader, position)) {
        source = "world";
    } else if (shader.worldViewProjRegister != NoRegister && g_gameViewProjEverKnown) {
        const Matrix4 world = Multiply(Matrix4::FromRegisters(&g_gameConstants[shader.worldViewProjRegister]), g_gameViewProjInverse);
        position = { world.values[3][0], world.values[3][1], world.values[3][2] };
        source = "wvp";
    }
    Vector3 firstVertex = {};
    const bool haveVertex = g_upVertexData != nullptr;
    if (haveVertex) memcpy(&firstVertex, g_upVertexData, sizeof(firstVertex));
    const bool isNearby = (source && FlatDistance(position, g_chosenCharacterRoot) < 2.0f) ||
                      (haveVertex && FlatDistance(firstVertex, g_chosenCharacterRoot) < 2.0f);
    if (!isNearby) return;
    ++lines;
    LogLine("[band near] VS %08X %s pos %.2f %.2f %.2f%s first vertex %.2f %.2f %.2f", shader.hash, source ? source : "-",
            position.x, position.y, position.z, haveVertex ? " | UP draw," : " |", firstVertex.x, firstVertex.y, firstVertex.z);
}

// Draws one part of your band member with everything above your chin clipped away (their head,
// hair, eyes, hats - whatever shaders they use), so it doesn't fill your view.
template <typename DrawCall>
static HRESULT DrawAvatarPart(IDirect3DDevice9* device, DrawCall draw) {
    // Plane kept side: y <= chin. With vertex shaders D3D9 clip planes are in clip space:
    // plane_clip = inverse(ViewProj) * plane_world.
    const float chinHeight = g_vrCameraToWorld.values[3][1] - 0.12f * g_settings.worldScale;
    const float worldPlane[4] = { 0.0f, -1.0f, 0.0f, chinHeight };
    Matrix4 inverseViewProj;
    if (!Invert(g_vrViewProj, inverseViewProj)) return draw();
    float clipPlane[4];
    for (int row = 0; row < 4; ++row) {
        clipPlane[row] = 0.0f;
        for (int column = 0; column < 4; ++column) clipPlane[row] += inverseViewProj.values[row][column] * worldPlane[column];
    }
    DWORD previousClipping = 0;
    device->GetRenderState(D3DRS_CLIPPLANEENABLE, &previousClipping);
    device->SetClipPlane(0, clipPlane);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
    const HRESULT result = draw();
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, previousClipping);
    return result;
}

// ---------------------------------------------------------------------------
// Full stereo (stereo_mode 1): every draw into a screen-shaped target is repeated for the right
// eye, with the right-eye copies of the targets, depth buffer and screen-shaped textures bound
// and the right eye's camera uploaded. What the game has bound is tracked by the hooks.
// ---------------------------------------------------------------------------

struct RightEyeBindings {
    bool          targetSwapped[4];
    bool          depthSwapped;
    bool          textureSwapped[TrackedTextureStages];
    D3DVIEWPORT9  viewport;
    RECT          scissor;
};

static bool FullStereoWanted() {
    return g_fullStereoFrame.active && g_modWorkDepth == 0 && VrCameraActive() && StereoTargets::IsPerEye(g_boundTargets[0]);
}

static bool BindRightEyeTargets(IDirect3DDevice9* device, RightEyeBindings& saved, bool swapTextures) {
    IDirect3DSurface9* rightTarget = StereoTargets::TwinSurface(device, g_boundTargets[0]);
    if (!rightTarget) return false;
    // Changing render target 0 resets the viewport (and scissor): put the game's back afterwards.
    device->GetViewport(&saved.viewport);
    device->GetScissorRect(&saved.scissor);
    g_originalSetRenderTarget(device, 0, rightTarget);
    saved.targetSwapped[0] = true;
    for (DWORD index = 1; index < 4; ++index) {
        IDirect3DSurface9* twin = g_boundTargets[index] ? StereoTargets::TwinSurface(device, g_boundTargets[index]) : nullptr;
        saved.targetSwapped[index] = twin != nullptr;
        if (twin) g_originalSetRenderTarget(device, index, twin);
    }
    IDirect3DSurface9* depthTwin = g_boundDepth ? StereoTargets::TwinSurface(device, g_boundDepth) : nullptr;
    saved.depthSwapped = depthTwin != nullptr;
    if (depthTwin) {
        g_originalSetDepthStencilSurface(device, depthTwin);
    } else if (g_boundDepth) {
        static bool logged = false;
        if (!logged) {
            D3DSURFACE_DESC description = {};
            g_boundDepth->GetDesc(&description);
            LogLine("[full stereo] a %ux%u depth buffer isn't per eye: the right eye shares the left's depth there", description.Width, description.Height);
            logged = true;
        }
    }
    for (DWORD stage = 0; stage < TrackedTextureStages; ++stage) {
        IDirect3DBaseTexture9* twin = swapTextures && g_boundTextures[stage] ? StereoTargets::TwinTexture(device, g_boundTextures[stage]) : nullptr;
        saved.textureSwapped[stage] = twin != nullptr;
        if (twin) g_originalSetTexture(device, stage, twin);
    }
    device->SetViewport(&saved.viewport);
    device->SetScissorRect(&saved.scissor);
    return true;
}

static void RestoreLeftEyeTargets(IDirect3DDevice9* device, const RightEyeBindings& saved) {
    for (DWORD index = 0; index < 4; ++index)
        if (saved.targetSwapped[index]) g_originalSetRenderTarget(device, index, g_boundTargets[index]);
    if (saved.depthSwapped) g_originalSetDepthStencilSurface(device, g_boundDepth);
    for (DWORD stage = 0; stage < TrackedTextureStages; ++stage)
        if (saved.textureSwapped[stage]) g_originalSetTexture(device, stage, g_boundTextures[stage]);
    device->SetViewport(&saved.viewport);
    device->SetScissorRect(&saved.scissor);
}

// Records whether the game's write into its bound targets reached the right-eye copies.
static void NoteTargetsWritten(bool rightEyeToo) {
    if (g_modWorkDepth != 0) return;
    for (IDirect3DSurface9* target : g_boundTargets)
        if (target) StereoTargets::NoteWrite(target, rightEyeToo);
}

// Draws for the left eye (as set up), then again for the right eye.
template <typename DrawCall>
static HRESULT DrawBothEyes(IDirect3DDevice9* device, const ShaderInfo* shader, DrawCall draw) {
    const bool avatarPart = g_currentDrawIsAvatar && g_vrMatricesValid;
    const HRESULT result = avatarPart ? DrawAvatarPart(device, draw) : draw();
    RightEyeBindings saved;
    if (!FullStereoWanted() || !BindRightEyeTargets(device, saved, true)) {
        NoteTargetsWritten(false);
        return result;
    }
    const bool switchCamera = g_currentDrawUsedVrCamera && shader && g_vrMatricesValid;
    if (switchCamera) {
        SelectEye(1);
        UploadVrCamera(device, shader);
    }
    if (avatarPart) DrawAvatarPart(device, draw);
    else draw();
    ++g_counters.rightEyeDraws;
    if (switchCamera) SelectEye(0);   // the next draw uploads its own camera again
    RestoreLeftEyeTargets(device, saved);
    NoteTargetsWritten(true);
    return result;
}

// Single entry point for all four draw calls.

// ---------------------------------------------------------------------------
// Draw finder (diagnostics): Ctrl+F9 / Ctrl+F10 step forward / back through the 3D draws within a few
// metres of you, nearest first, hiding one at a time - when something you're after (guitar strings)
// disappears, the log line says what it is. Stepping past either end shows everything again.
// ---------------------------------------------------------------------------
struct DrawKey {
    uint32_t vertexShader = 0, pixelShader = 0;
    const void* texture = nullptr;
    UINT vertexCount = 0;
    D3DPRIMITIVETYPE primitiveType = D3DPT_TRIANGLELIST;
    bool operator==(const DrawKey& other) const {
        return vertexShader == other.vertexShader && pixelShader == other.pixelShader && texture == other.texture &&
               vertexCount == other.vertexCount && primitiveType == other.primitiveType;
    }
};
struct DrawCandidate {
    DrawKey key;
    float distance = 0.0f;   // metres from your head
    Vector3 centre = {};
    const char* placement = "";
    bool characterSkin = false, userPointer = false, positionKnown = false;
    UINT stride = 0, textureWidth = 0, textureHeight = 0;
    D3DFORMAT textureFormat = D3DFMT_UNKNOWN;
};
static std::vector<DrawCandidate> g_finderThisFrame, g_finderList;
static int  g_finderIndex = -1;   // -1: nothing hidden
static bool g_finderActive = false;
static bool g_finderLogConstants = false;
static DrawKey g_finderHidden;
constexpr float FinderRadius = 3.0f;

// Where a draw is in the world (centre of its vertices, or its World's position).
static bool DrawWorldCentre(IDirect3DDevice9* device, const ShaderInfo& shader, Vector3& centre, const char*& placement) {
    const bool worldSpace = shader.worldRegister == NoRegister && shader.worldViewProjRegister == NoRegister;
    const bool wvpOnly = shader.worldRegister == NoRegister && shader.worldViewProjRegister != NoRegister;
    placement = worldSpace ? "world-space" : wvpOnly ? "WVP-only" : "own World";
    Vector3 positions[256];
    const int count = CurrentDrawPositions(device, positions, 256);
    if (count) {
        centre = Average(positions, count);
        if (shader.worldRegister != NoRegister) centre = TransformPoint(centre, &g_gameConstants[shader.worldRegister]);
        else if (wvpOnly) {
            if (!g_gameViewProjEverKnown) return false;
            const Matrix4 world = Multiply(Matrix4::FromRegisters(&g_gameConstants[shader.worldViewProjRegister]), g_gameViewProjInverse);
            centre = TransformPoint(centre, world.values);
        }
        return true;
    }
    // Vertices not readable: its World's position - unless that's the world origin (placed some other way, e.g. by bones).
    if (WorldTranslation(shader, centre) && (fabsf(centre.x) > 0.01f || fabsf(centre.y) > 0.01f || fabsf(centre.z) > 0.01f)) {
        placement = "own World (vertices not readable: its origin)";
        return true;
    }
    if (shader.worldRegister != NoRegister) placement = "World at the world origin, vertices not readable";
    return false;
}

// Called for 3D draws into the main view in songs; true = this is the draw being hidden.
static bool FinderConsiderDraw(IDirect3DDevice9* device, const ShaderInfo& shader) {
    if (!g_finderActive) return false;
    DrawCandidate candidate;
    auto pixelShader = g_pixelShaders.find(g_currentPixelShader);
    candidate.key = { shader.hash, pixelShader != g_pixelShaders.end() ? pixelShader->second.hash : 0u, g_boundTextures[0],
                      g_drawVertexCount, g_drawPrimitiveType };
    const bool hidden = g_finderIndex >= 0 && candidate.key == g_finderHidden;
    if (hidden && g_finderLogConstants) {
        // What its vertex shader is given: matrices (count 3-4) with where they put things (their translation).
        g_finderLogConstants = false;
        for (const ShaderInfo::Constant& constant : shader.constants) {
            const float* rows = g_gameConstants[constant.index];
            if (constant.count == 4)
                LogLine("[finder]   %s = c%d x4, translation %.2f %.2f %.2f", constant.name.c_str(), constant.index,
                        g_gameConstants[constant.index + 3][0], g_gameConstants[constant.index + 3][1], g_gameConstants[constant.index + 3][2]);
            else if (constant.count == 3)
                LogLine("[finder]   %s = c%d x3, translation (w) %.2f %.2f %.2f", constant.name.c_str(), constant.index,
                        rows[3], g_gameConstants[constant.index + 1][3], g_gameConstants[constant.index + 2][3]);
            else if (constant.count > 4)
                LogLine("[finder]   %s = c%d x%d (a matrix palette?), first rows' w: %.2f %.2f %.2f", constant.name.c_str(), constant.index,
                        constant.count, rows[3], g_gameConstants[constant.index + 1][3], g_gameConstants[constant.index + 2][3]);
            else
                LogLine("[finder]   %s = c%d x%d: %.3f %.3f %.3f %.3f", constant.name.c_str(), constant.index, constant.count,
                        rows[0], rows[1], rows[2], rows[3]);
        }
    }
    for (const DrawCandidate& seen : g_finderThisFrame)
        if (seen.key == candidate.key) return hidden;
    const Vector3 head = { g_vrCameraToWorld.values[3][0], g_vrCameraToWorld.values[3][1], g_vrCameraToWorld.values[3][2] };
    candidate.positionKnown = DrawWorldCentre(device, shader, candidate.centre, candidate.placement);
    if (candidate.positionKnown) {
        const float deltaY = candidate.centre.y - head.y;
        const float flat = FlatDistance(candidate.centre, head);
        candidate.distance = sqrtf(flat * flat + deltaY * deltaY) / std::max(0.01f, g_settings.worldScale);
        if (candidate.distance > FinderRadius) return hidden;
    } else {
        candidate.distance = 1e6f;   // unknown: listed last
    }
    candidate.characterSkin = shader.isCharacterSkin;
    candidate.userPointer = g_upVertexData != nullptr;
    candidate.stride = g_upVertexData ? g_upStride : 0;
    if (!g_upVertexData) {
        IDirect3DVertexBuffer9* buffer = nullptr;
        UINT offset = 0;
        if (SUCCEEDED(device->GetStreamSource(0, &buffer, &offset, &candidate.stride)) && buffer) buffer->Release();
    }
    if (g_boundTextures[0] && g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC description = {};
        static_cast<IDirect3DTexture9*>(g_boundTextures[0])->GetLevelDesc(0, &description);
        candidate.textureWidth = description.Width;
        candidate.textureHeight = description.Height;
        candidate.textureFormat = description.Format;
    }
    if (g_finderThisFrame.size() < 400) g_finderThisFrame.push_back(candidate);
    return hidden;
}

static void LogFinderCandidate(int index) {
    const DrawCandidate& candidate = g_finderList[index];
    static const char* const typeNames[] = { "?", "points", "lines", "line strip", "triangles", "triangle strip", "triangle fan" };
    const int type = int(candidate.key.primitiveType);
    char distance[32];
    if (candidate.positionKnown) snprintf(distance, sizeof(distance), "%.2f m", candidate.distance);
    else snprintf(distance, sizeof(distance), "unknown");
    LogLine("[finder] %d of %d: VS %08X PS %08X, %u vertices as %s, stride %u%s, texture %ux%u fmt %d, %s%s, centre %.2f %.2f %.2f, %s from you",
            index + 1, int(g_finderList.size()), candidate.key.vertexShader, candidate.key.pixelShader, candidate.key.vertexCount,
            type >= 1 && type <= 6 ? typeNames[type] : typeNames[0], candidate.stride, candidate.userPointer ? " (user pointer)" : "",
            candidate.textureWidth, candidate.textureHeight, int(candidate.textureFormat), candidate.placement,
            candidate.characterSkin ? ", band member skin" : "", candidate.centre.x, candidate.centre.y, candidate.centre.z, distance);
}

// Once a frame (Present).
static void UpdateDrawFinder() {
    if (g_finderActive) {
        std::sort(g_finderThisFrame.begin(), g_finderThisFrame.end(),
                  [](const DrawCandidate& first, const DrawCandidate& second) { return first.distance < second.distance; });
        if (!g_finderThisFrame.empty()) g_finderList.swap(g_finderThisFrame);
        g_finderThisFrame.clear();
    }
}

// Ctrl+F9 (next) / Ctrl+F10 (previous), from the hotkeys.
static void StepDrawFinder(bool next) {
    if (!InSong()) { LogLine("[finder] only in a song"); return; }
    if (!g_finderActive) {
        g_finderActive = true;
        g_finderIndex = -1;
        LogLine("[finder] on: collecting the draws within %.0f m of you - press Ctrl+F9 again to start hiding them one by one", FinderRadius);
        return;
    }
    if (g_finderList.empty()) { LogLine("[finder] nothing found near you yet"); return; }
    if (g_finderIndex == -1 && next) {
        LogLine("[finder] %d draws near you, nearest first:", int(g_finderList.size()));
        for (int index = 0; index < int(g_finderList.size()) && index < 80; ++index) LogFinderCandidate(index);
    }
    g_finderIndex += next ? 1 : -1;
    if (g_finderIndex < -1) g_finderIndex = int(g_finderList.size()) - 1;
    if (g_finderIndex >= int(g_finderList.size())) g_finderIndex = -1;
    if (g_finderIndex == -1) { LogLine("[finder] showing everything (Ctrl+F9 / Ctrl+F10 to step again)"); return; }
    g_finderHidden = g_finderList[g_finderIndex].key;
    LogLine("[finder] hiding:");
    LogFinderCandidate(g_finderIndex);
    g_finderLogConstants = true;   // its vertex shader's constants, at its next draw
}

// ---------------------------------------------------------------------------
// Role probe (diagnostics): for one frame, everything drawn on stage is put with the band member it's
// nearest to, and each member's list logged - their instrument (guitar / bass strings, drum kit, mic)
// should give away who plays what. Ctrl+F6, and automatically 20 s into each song.
// ---------------------------------------------------------------------------
struct RoleProbeDraw {
    uint32_t vertexShader = 0, pixelShader = 0;
    UINT vertexCount = 0, textureWidth = 0, textureHeight = 0;
    D3DPRIMITIVETYPE primitiveType = D3DPT_TRIANGLELIST;
    Vector3 position = {};
    bool skin = false, worldAtOrigin = false, fromVertices = false;
    float bones[4][3] = {};   // strings at the world origin: their first matrices' translations
    int boneCount = 0;
};
static bool g_roleProbeThisFrame = false, g_roleProbeRequested = false;
static std::vector<RoleProbeDraw> g_roleProbeDraws;

static void RoleProbeNote(IDirect3DDevice9* device, const ShaderInfo& shader) {
    if (g_roleProbeDraws.size() >= 3000) return;
    RoleProbeDraw entry;
    auto pixelShader = g_pixelShaders.find(g_currentPixelShader);
    entry.vertexShader = shader.hash;
    entry.pixelShader = pixelShader != g_pixelShaders.end() ? pixelShader->second.hash : 0u;
    entry.vertexCount = g_drawVertexCount;
    entry.primitiveType = g_drawPrimitiveType;
    entry.skin = shader.isCharacterSkin;
    if (g_boundTextures[0] && g_boundTextures[0]->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC description = {};
        static_cast<IDirect3DTexture9*>(g_boundTextures[0])->GetLevelDesc(0, &description);
        entry.textureWidth = description.Width;
        entry.textureHeight = description.Height;
    }
    Vector3 translation;
    const bool haveWorld = WorldTranslation(shader, translation);
    entry.worldAtOrigin = haveWorld && fabsf(translation.x) < 0.01f && fabsf(translation.y) < 0.01f && fabsf(translation.z) < 0.01f;
    const char* placement = nullptr;
    Vector3 centre;
    Vector3 positions[64];
    entry.fromVertices = CurrentDrawPositions(device, positions, 64) > 0;
    if (!DrawWorldCentre(device, shader, centre, placement)) {
        if (!entry.worldAtOrigin) return;
        centre = {};
    }
    entry.position = centre;
    if (entry.worldAtOrigin && !entry.fromVertices) {
        // Placed on the graphics card by other matrices (bones?): note where those put things.
        for (const ShaderInfo::Constant& constant : shader.constants) {
            if (constant.count < 6 || constant.index == shader.worldRegister) continue;
            for (int bone = 0; bone < 4 && (bone + 1) * 3 <= constant.count; ++bone) {
                entry.bones[bone][0] = g_gameConstants[constant.index + bone * 3][3];
                entry.bones[bone][1] = g_gameConstants[constant.index + bone * 3 + 1][3];
                entry.bones[bone][2] = g_gameConstants[constant.index + bone * 3 + 2][3];
                entry.boneCount = bone + 1;
            }
            break;
        }
        // The first bone's translation is a better guess at where it is than the origin.
        if (entry.boneCount) entry.position = { entry.bones[0][0], entry.bones[0][1], entry.bones[0][2] };
    }
    g_roleProbeDraws.push_back(entry);
}

// Once per frame (Present): the frame just drawn was probed - log it member by member.
static void UpdateRoleProbe() {
    if (g_roleProbeThisFrame) {
        g_roleProbeThisFrame = false;
        LogLine("[role] %d band members, %d draws on stage this frame:", int(g_characters.size()), int(g_roleProbeDraws.size()));
        static const char* const typeNames[] = { "?", "points", "lines", "line strip", "triangles", "tri strip", "tri fan" };
        for (size_t member = 0; member < g_characters.size(); ++member) {
            const CharacterRoot& root = g_characters[member];
            const bool chosen = g_chosenCharacterLocked && FlatDistance(root.position, g_chosenCharacterRoot) < 0.3f;
            float headHeight = -1.0f;
            int skinDraws = 0;
            for (const RoleProbeDraw& entry : g_roleProbeDraws) {
                if (!entry.skin || FlatDistance(entry.position, root.position) > 1.0f) continue;
                ++skinDraws;
                if (entry.vertexShader == FaceShader && entry.fromVertices) headHeight = std::max(headHeight, entry.position.y - root.position.y);
            }
            LogLine("[role] member %d%s at %.2f %.2f %.2f facing %.0f deg: face %.2f m above their feet, %d body draws", int(member) + 1,
                    chosen ? " (yours)" : "", root.position.x, root.position.y, root.position.z, root.yaw * 57.2958f, headHeight, skinDraws);
            int listed = 0;
            for (const RoleProbeDraw& entry : g_roleProbeDraws) {
                if (entry.skin) continue;
                // Nearest member (flat), within 1.5 m.
                size_t nearest = 0;
                float nearestDistance = 1e9f;
                for (size_t other = 0; other < g_characters.size(); ++other) {
                    const float distance = FlatDistance(entry.position, g_characters[other].position);
                    if (distance < nearestDistance) { nearestDistance = distance; nearest = other; }
                }
                if (nearest != member || nearestDistance > 1.5f || listed >= 30) continue;
                ++listed;
                const int type = int(entry.primitiveType);
                LogLine("[role]   VS %08X PS %08X %u vertices %s, texture %ux%u, %.2f m away, %.2f m up%s%s", entry.vertexShader, entry.pixelShader,
                        entry.vertexCount, type >= 1 && type <= 6 ? typeNames[type] : typeNames[0], entry.textureWidth, entry.textureHeight,
                        nearestDistance, entry.position.y - root.position.y, entry.vertexShader == GuitarStringsShader ? "  <- strings shader" : "",
                        entry.worldAtOrigin ? (entry.boneCount ? " (placed by bones)" : " (World at origin)") : "");
            }
        }
        // Strings that couldn't be put with anyone.
        for (const RoleProbeDraw& entry : g_roleProbeDraws) {
            if (entry.vertexShader != GuitarStringsShader) continue;
            float nearestDistance = 1e9f;
            for (const CharacterRoot& root : g_characters) nearestDistance = std::min(nearestDistance, FlatDistance(entry.position, root.position));
            if (nearestDistance <= 1.5f) continue;
            LogLine("[role] strings draw (%u vertices) not near anyone: at %.2f %.2f %.2f%s, first matrices at %.2f %.2f %.2f / %.2f %.2f %.2f",
                    entry.vertexCount, entry.position.x, entry.position.y, entry.position.z, entry.worldAtOrigin ? " (World at origin)" : "",
                    entry.bones[0][0], entry.bones[0][1], entry.bones[0][2], entry.bones[1][0], entry.bones[1][1], entry.bones[1][2]);
        }
        g_roleProbeDraws.clear();
    }
    if (g_roleProbeRequested || (g_settings.debugTools && InSong() && g_framesInSong == 600)) {
        g_roleProbeRequested = false;
        g_roleProbeThisFrame = true;
        g_roleProbeDraws.clear();
    }
}

template <typename DrawCall>
static HRESULT HandleDraw(IDirect3DDevice9* device, DrawCall draw) {
    const ShaderInfo* shader = CurrentShaderInfo();
    ++g_allDrawsThisFrame;
    g_currentDrawUsedVrCamera = false;
    g_currentDrawIsAvatar = false;
    if (shader && shader->isHighway && IsMainRenderTarget()) g_highwayDrawnThisFrame = true;
    // Diagnostics: which highway shaders/textures are drawn (to tell instruments apart).
    if (g_drawListActive && shader && shader->isHighway && g_drawListLines < 600) {
        ++g_drawListLines;
        auto pixelShader = g_pixelShaders.find(g_currentPixelShader);
        IDirect3DBaseTexture9* texture = nullptr;
        device->GetTexture(0, &texture);
        D3DSURFACE_DESC description = {};
        if (texture && texture->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &description);
        LogLine("[draw list] highway VS %08X PS %08X texture %ux%u format %d", shader->hash,
                pixelShader != g_pixelShaders.end() ? pixelShader->second.hash : 0u, description.Width, description.Height, int(description.Format));
        if (texture) texture->Release();
    }
    if (g_currentPixelShader) {
        const DepthOfFieldAction action = DepthOfFieldActionForDraw(device);
        if (action == DepthOfFieldAction::Skip) return D3D_OK;
        if (action == DepthOfFieldAction::DrawWithoutBlur) {
            // Composite = blurred scene * strength + bloom over the scene * (1 - strength):
            // strength 0 keeps the sharp scene and the bloom.
            const int dofRegister = g_pixelShaders[g_currentPixelShader].depthOfFieldRegister;
            float noBlur[4] = { g_pixelConstants[dofRegister][0], g_pixelConstants[dofRegister][1],
                                g_pixelConstants[dofRegister][2], 0.0f };
            RestoreAllOverrides(device);   // full-screen pass: the game's own vertex constants
            g_originalSetPixelShaderConstantF(device, dofRegister, noBlur, 1);
            const HRESULT result = DrawBothEyes(device, shader, draw);
            g_originalSetPixelShaderConstantF(device, dofRegister, g_pixelConstants[dofRegister], 1);
            return result;
        }
    }
    g_currentDrawIsAvatar = false;
    if (g_roleProbeThisFrame && shader && shader->UsesCamera() && VrCameraActive() && IsMainRenderTarget() && InSong())
        RoleProbeNote(device, *shader);
    if (g_finderActive && shader && shader->UsesCamera() && VrCameraActive() && IsMainRenderTarget() && InSong() &&
        FinderConsiderDraw(device, *shader)) return D3D_OK;
    const bool chosenCandidate = shader && VrCameraActive() && IsMainRenderTarget() && InSong();
    if (chosenCandidate && IsChosenCharacterDraw(*shader)) {
        if (shader->isCharacterSkin) NoteCharacterDraw(*shader);   // keep tracking them
        NoteChosenCharacterDraw(device, *shader);
        if (g_settings.bandMemberMode == HideBandMember) return D3D_OK;
        g_currentDrawIsAvatar = true;
    } else if (chosenCandidate && IsChosenCharacterNearbyDraw(device, *shader)) {
        if (g_settings.bandMemberMode == HideBandMember) return D3D_OK;
        g_currentDrawIsAvatar = true;
    } else if (shader && g_nearbyDiagnosticFrames > 0 && g_chosenCharacterLocked && VrCameraActive() && IsMainRenderTarget() && InSong()) {
        LogDrawNearChosenCharacter(*shader);
    }
    g_currentDrawIsRolledUi = false;
    PrepareCameraForDraw(device, shader);
    if (g_currentDrawIsAvatar && g_vrMatricesValid) return DrawBothEyes(device, shader, draw);
    if (g_currentDrawIsRolledUi && g_xr.IsRunning() && g_previousFrameUsedVrCamera) {
        // The scene's depth is from your head's view, not the game's: draw the text on top.
        DWORD depthEnabled = D3DZB_TRUE;
        device->GetRenderState(D3DRS_ZENABLE, &depthEnabled);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        const HRESULT result = DrawIntoOverlay(device, draw);
        device->SetRenderState(D3DRS_ZENABLE, depthEnabled);
        return result;
    }
    if (ShouldRedirectToOverlay(shader)) return DrawIntoOverlay(device, draw, shader);
    return DrawBothEyes(device, shader, draw);
}

// ---------------------------------------------------------------------------
// Present: hand the finished eye (and overlay) to OpenXR
// ---------------------------------------------------------------------------
static double NowMs();

static void DarkenSurface(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT width, UINT height, float amount);

struct ReadbackTarget {
    IDirect3DSurface9* gpuSurface = nullptr;     // render target at the destination size
    IDirect3DSurface9* systemSurface = nullptr;  // CPU-readable copy
    UINT width = 0, height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;

    void Release() {
        if (gpuSurface)    { gpuSurface->Release();    gpuSurface = nullptr; }
        if (systemSurface) { systemSurface->Release(); systemSurface = nullptr; }
    }

    bool Ensure(IDirect3DDevice9* device, UINT targetWidth, UINT targetHeight, D3DFORMAT targetFormat) {
        if (gpuSurface && width == targetWidth && height == targetHeight && format == targetFormat) return true;
        Release();
        width = targetWidth;
        height = targetHeight;
        format = targetFormat;
        HRESULT targetResult = device->CreateRenderTarget(width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE, &gpuSurface, nullptr);
        HRESULT systemResult = SUCCEEDED(targetResult)
            ? device->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &systemSurface, nullptr)
            : E_ABORT;
        if (FAILED(targetResult) || FAILED(systemResult)) {
            static UINT lastFailedWidth = 0, lastFailedHeight = 0;
            if (width != lastFailedWidth || height != lastFailedHeight) {
                LogLine("[readback] could not create %ux%u surfaces: render target %08lX, system copy %08lX "
                        "(8876017C = out of video memory, 8007000E = out of memory) - will keep retrying quietly",
                        width, height, targetResult, systemResult);
                lastFailedWidth = width;
                lastFailedHeight = height;
            }
            Release();
            return false;
        }
        return true;
    }

    // Scales source into the target on the GPU, then copies it to system memory.
    // Calls consume(bits, pitch) while the copy is locked.
    // X8R8G8B8 sources have undefined alpha; set it to 255 in place (no extra buffer: this is a
    // 32-bit process and address space is tight).
    void ForceOpaque(void* bits, int pitch) const {
        for (UINT row = 0; row < height; ++row) {
            uint8_t* pixel = static_cast<uint8_t*>(bits) + size_t(row) * pitch + 3;
            for (UINT column = 0; column < width; ++column, pixel += 4) *pixel = 255;
        }
    }

    // Step 1 (this frame): scale the source into the target and queue the copy to system memory,
    // right behind this frame's drawing. Queued here (not next frame), the copy only has to wait
    // for this frame - asking for it next frame put it behind next frame's drawing too, so the CPU
    // sat waiting for the GPU to finish a whole extra frame.
    bool Stage(IDirect3DDevice9* device, IDirect3DSurface9* source, float darken = 0.0f, const RECT* sourceRect = nullptr) {
        if (FAILED(device->StretchRect(source, sourceRect, gpuSurface, nullptr, D3DTEXF_LINEAR))) return false;
        DarkenSurface(device, gpuSurface, width, height, darken);
        const double startMs = NowMs();
        const bool queued = SUCCEEDED(device->GetRenderTargetData(gpuSurface, systemSurface));
        g_counters.totalStageMs += NowMs() - startMs;
        return queued;
    }

    // Step 2 (next frame): by now the GPU has normally finished the copy, so locking doesn't wait.
    template <typename Consumer>
    void Collect(bool writable, Consumer consume) {
        const double startMs = NowMs();
        D3DLOCKED_RECT locked;
        if (FAILED(systemSurface->LockRect(&locked, nullptr, writable ? 0 : D3DLOCK_READONLY))) return;
        g_counters.totalReadbackMs += NowMs() - startMs;
        consume(locked.pBits, locked.Pitch);
        systemSurface->UnlockRect();
    }
};

// Two slots each: one staged this frame, one collected from last frame.
static float g_fadeAmount = 0.0f;   // 0 clear .. 1 black: menu camera moves fade the eye images through black
struct PendingEyeImage {
    bool    valid;
    int     eye;
    EyeView view;
};
struct PendingOverlayImage {
    bool valid;
    bool opaque;   // whole game screen (menus): no usable alpha
    bool layers[HudLayerCount];   // 3D HUD: those panels were staged too
};
static ReadbackTarget      g_eyeReadbacks[2][2];   // [slot][eye]
static ReadbackTarget      g_overlayReadbacks[2];
static ReadbackTarget      g_hudLayerReadbacks[2][HudLayerCount];   // 3D HUD: gems / meters panels
static PendingEyeImage     g_pendingEyes[2][2] = {};
static PendingOverlayImage g_pendingOverlays[2] = {};
static int  g_readbackSlot = 0;
static bool g_submittedOverlayOpaque = false;

// Desktop mirror: eyes alternate per frame, so the monitor would flicker between them.
// Keep a copy of the last left-eye frame and show that on right-eye frames.
static IDirect3DTexture9* g_mirrorTexture = nullptr;
static IDirect3DSurface9* g_mirrorSurface = nullptr;
static bool g_mirrorFailed = false;
static IDirect3DStateBlock9* g_desktopDrawState = nullptr;

struct ScreenVertex {
    float x, y, z, rhw;
    float u, v;
};

struct ScreenRect {
    float left, top, right, bottom;
};

// Draws part of a texture (uv, 0..1) into a rectangle of the backbuffer (pixels).
// premultipliedBlend: blend over what's there (HUD layer); otherwise replace (eye image).
static void DrawTexturedQuad(IDirect3DDevice9* device, IDirect3DTexture9* texture, const ScreenVertex quad[4],
                             bool premultipliedBlend, bool linearFilter) {
    if (!g_desktopDrawState && FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_desktopDrawState))) return;
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) return;
    IDirect3DSurface9* previousTarget = nullptr;
    device->GetRenderTarget(0, &previousTarget);
    g_desktopDrawState->Capture();

    g_originalSetRenderTarget(device, 0, backBuffer);
    D3DVIEWPORT9 fullViewport = { 0, 0, g_backBufferWidth, g_backBufferHeight, 0.0f, 1.0f };
    device->SetViewport(&fullViewport);
    g_originalSetVertexShader(device, nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, texture);
    device->SetTexture(1, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, premultipliedBlend ? TRUE : FALSE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    const D3DTEXTUREFILTERTYPE filter = linearFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    if (SUCCEEDED(device->BeginScene())) {
        g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
        device->EndScene();
    }

    g_desktopDrawState->Apply();
    if (previousTarget) {
        g_originalSetRenderTarget(device, 0, previousTarget);
        previousTarget->Release();
    }
    backBuffer->Release();
}

// Draws part of a texture (uv, 0..1) into a rectangle of the backbuffer (pixels).
static void DrawTextureToBackBuffer(IDirect3DDevice9* device, IDirect3DTexture9* texture, const ScreenRect& destination,
                                    const ScreenRect& uv, bool premultipliedBlend, bool linearFilter) {
    // -0.5: D3D9 maps texel centres to pixel centres this way.
    const float left = destination.left - 0.5f, top = destination.top - 0.5f;
    const float right = destination.right - 0.5f, bottom = destination.bottom - 0.5f;
    const ScreenVertex quad[4] = {
        { left,  top,    0.0f, 1.0f, uv.left,  uv.top },
        { right, top,    0.0f, 1.0f, uv.right, uv.top },
        { left,  bottom, 0.0f, 1.0f, uv.left,  uv.bottom },
        { right, bottom, 0.0f, 1.0f, uv.right, uv.bottom },
    };
    DrawTexturedQuad(device, texture, quad, premultipliedBlend, linearFilter);
}

// Blends black over a render target (amount 0..1): the menu camera fade.
struct ColourVertex {
    float x, y, z, rhw;
    D3DCOLOR colour;
};

static void DarkenSurface(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT width, UINT height, float amount) {
    if (amount <= 0.0f || !target) return;
    if (amount >= 1.0f) { g_originalColorFill(device, target, nullptr, D3DCOLOR_XRGB(0, 0, 0)); return; }
    if (!g_desktopDrawState && FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_desktopDrawState))) return;
    IDirect3DSurface9* previousTarget = nullptr;
    device->GetRenderTarget(0, &previousTarget);
    g_desktopDrawState->Capture();

    g_originalSetRenderTarget(device, 0, target);
    D3DVIEWPORT9 fullViewport = { 0, 0, width, height, 0.0f, 1.0f };
    device->SetViewport(&fullViewport);
    g_originalSetVertexShader(device, nullptr);
    g_originalSetPixelShader(device, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_originalSetTexture(device, 0, nullptr);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    const D3DCOLOR colour = D3DCOLOR_ARGB(int(amount * 255.0f + 0.5f), 0, 0, 0);
    const float right = float(width), bottom = float(height);
    const ColourVertex quad[4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f, colour }, { right - 0.5f, -0.5f, 0.0f, 1.0f, colour },
        { -0.5f, bottom - 0.5f, 0.0f, 1.0f, colour }, { right - 0.5f, bottom - 0.5f, 0.0f, 1.0f, colour },
    };
    if (SUCCEEDED(device->BeginScene())) {
        g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ColourVertex));
        device->EndScene();
    }

    g_desktopDrawState->Apply();
    if (previousTarget) {
        g_originalSetRenderTarget(device, 0, previousTarget);
        previousTarget->Release();
    }
}

// Draws the HUD panel into the current eye image where it floats in the headset (perspective
// correct), so the monitor shows what you see. Uses the eye pose this frame was rendered with.
static void DrawFloatingPanelIntoEyeImage(IDirect3DDevice9* device) {
    if (!g_overlayDrawnThisFrame || !g_overlayTexture || !g_renderingEyeViewValid || !g_settings.showOverlay) return;
    const XrPosef& pose = g_renderingEyeView.pose;
    const XrFovf& fov = g_renderingEyeView.fov;
    Matrix4 eyeToAnchor = PoseToMatrix({ pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w },
                                       { pose.position.x, pose.position.y, pose.position.z });
    Matrix4 anchorToEye;
    if (!Invert(eyeToAnchor, anchorToEye)) return;
    const Matrix4 projection = OffCentreProjection(tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleDown), tanf(fov.angleUp), 0.01f, 100.0f);
    const Matrix4 anchorToClip = Multiply(anchorToEye, projection);

    // Same placement as the headset's quad layer: centred (0, height, -distance) in anchor space.
    const OverlayPlacement& panel = g_settings.overlay;
    const float halfWidth = panel.width / 2;
    const float halfHeight = panel.width * float(g_xr.OverlayHeight()) / float(std::max<UINT>(1, g_xr.OverlayWidth())) / 2;
    const float corners[4][2] = { { -halfWidth, halfHeight }, { halfWidth, halfHeight }, { -halfWidth, -halfHeight }, { halfWidth, -halfHeight } };
    const float screenLeft = float(OverlayMargin) / float(g_backBufferWidth + 2 * OverlayMargin);
    const float screenRight = float(OverlayMargin + g_backBufferWidth) / float(g_backBufferWidth + 2 * OverlayMargin);
    const float textureCorners[4][2] = { { screenLeft, 0 }, { screenRight, 0 }, { screenLeft, 1 }, { screenRight, 1 } };
    ScreenVertex quad[4];
    for (int corner = 0; corner < 4; ++corner) {
        const float point[4] = { corners[corner][0], panel.height + corners[corner][1], -panel.distance, 1.0f };
        float clip[4] = {};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) clip[column] += point[row] * anchorToClip.values[row][column];
        if (clip[3] <= 0.01f) return;   // behind you: nothing to draw
        const float reciprocal = 1.0f / clip[3];
        quad[corner] = { (clip[0] * reciprocal + 1.0f) * 0.5f * float(g_backBufferWidth) - 0.5f,
                         (1.0f - clip[1] * reciprocal) * 0.5f * float(g_backBufferHeight) - 0.5f,
                         0.0f, reciprocal, textureCorners[corner][0], textureCorners[corner][1] };
    }
    DrawTexturedQuad(device, g_overlayTexture, quad, true, true);
    for (int layer = 0; layer < HudLayerCount; ++layer)   // (layer 1 is the right eye's gems)
        if (layer != 1 && g_hudLayerDrawn[layer] && g_hudLayerTextures[layer]) DrawTexturedQuad(device, g_hudLayerTextures[layer], quad, true, true);
}

// The eye image is roughly square but the monitor is wide, so shown as-is it looks stretched.
// mirror_mode 0: stretch (as rendered), 1: crop the middle to fill the screen, 2: fit with black bars.
static float g_mirrorEyeAspect = 1.0f;   // width / height of the mirrored eye's field of view

static void ShowLeftEyeOnDesktop(IDirect3DDevice9* device) {
    if (!g_vrCameraUsedThisFrame || g_mirrorFailed) return;
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) return;

    if (!g_mirrorTexture) {
        D3DSURFACE_DESC description;
        backBuffer->GetDesc(&description);
        if (FAILED(device->CreateTexture(description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format,
                                         D3DPOOL_DEFAULT, &g_mirrorTexture, nullptr))) {
            LogLine("[mirror] could not create desktop mirror texture; monitor will show both eyes");
            g_mirrorFailed = true;
            backBuffer->Release();
            return;
        }
        g_mirrorTexture->GetSurfaceLevel(0, &g_mirrorSurface);
    }

    // Left-eye frames: keep a same-size copy (no scaling) for the right-eye frames.
    // (The headset already has its copy of this frame, so the panel drawn here is monitor-only.)
    const bool freshImage = EyeForThisFrame() == 0 || g_fullStereoFrame.active;   // full stereo draws the left eye every frame
    if (freshImage) {
        if (g_settings.mirrorHud == 2) DrawFloatingPanelIntoEyeImage(device);
        device->StretchRect(backBuffer, nullptr, g_mirrorSurface, nullptr, D3DTEXF_NONE);
        const XrFovf& fov = g_renderingEyeView.fov;
        const float width = tanf(fov.angleRight) - tanf(fov.angleLeft);
        const float height = tanf(fov.angleUp) - tanf(fov.angleDown);
        if (width > 0 && height > 0) g_mirrorEyeAspect = width / height;
    }

    if (g_settings.mirrorMode == 0) {
        if (!freshImage && FAILED(device->StretchRect(g_mirrorSurface, nullptr, backBuffer, nullptr, D3DTEXF_NONE))) {
            LogLine("[mirror] copying to the backbuffer failed; monitor will show both eyes");
            g_mirrorFailed = true;
        }
        backBuffer->Release();
        return;
    }

    // Crop / fit: drawn as a textured quad from the copy (StretchRect scaling is unreliable on some drivers).
    const float screenWidth = float(g_backBufferWidth), screenHeight = float(g_backBufferHeight);
    const float screenAspect = screenWidth / screenHeight;
    ScreenRect uv = { 0.0f, 0.0f, 1.0f, 1.0f };
    ScreenRect destination = { 0.0f, 0.0f, screenWidth, screenHeight };
    if (g_settings.mirrorMode == 1) {
        if (g_mirrorEyeAspect < screenAspect) {
            const float keptFraction = g_mirrorEyeAspect / screenAspect;
            uv.top = 0.5f - keptFraction / 2;
            uv.bottom = 0.5f + keptFraction / 2;
        } else {
            const float keptFraction = screenAspect / g_mirrorEyeAspect;
            uv.left = 0.5f - keptFraction / 2;
            uv.right = 0.5f + keptFraction / 2;
        }
    } else {
        if (g_mirrorEyeAspect < screenAspect) {
            const float shownWidth = screenHeight * g_mirrorEyeAspect;
            destination.left = (screenWidth - shownWidth) / 2;
            destination.right = destination.left + shownWidth;
        } else {
            const float shownHeight = screenWidth / g_mirrorEyeAspect;
            destination.top = (screenHeight - shownHeight) / 2;
            destination.bottom = destination.top + shownHeight;
        }
        device->ColorFill(backBuffer, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    }
    backBuffer->Release();
    DrawTextureToBackBuffer(device, g_mirrorTexture, destination, uv, false, true);
}

// Draws the redirected highway/HUD layer over the desktop image (premultiplied alpha).
static void DrawOverlayOnDesktop(IDirect3DDevice9* device) {
    if (!g_overlayDrawnThisFrame || !g_overlayTexture) return;
    const ScreenRect fullScreen = { 0.0f, 0.0f, float(g_backBufferWidth), float(g_backBufferHeight) };
    const float overlayWidth = float(g_backBufferWidth + 2 * OverlayMargin);
    const ScreenRect screenPart = { float(OverlayMargin) / overlayWidth, 0.0f, float(OverlayMargin + g_backBufferWidth) / overlayWidth, 1.0f };
    DrawTextureToBackBuffer(device, g_overlayTexture, fullScreen, screenPart, true, false);
    for (int layer = 0; layer < HudLayerCount; ++layer)   // (layer 1 is the right eye's gems)
        if (layer != 1 && g_hudLayerDrawn[layer] && g_hudLayerTextures[layer])
            DrawTextureToBackBuffer(device, g_hudLayerTextures[layer], fullScreen, screenPart, true, false);
}

static void ReleaseDeviceResources() {
    if (g_desktopDrawState) { g_desktopDrawState->Release(); g_desktopDrawState = nullptr; }
    if (g_mirrorSurface) { g_mirrorSurface->Release(); g_mirrorSurface = nullptr; }
    if (g_mirrorTexture) { g_mirrorTexture->Release(); g_mirrorTexture = nullptr; }
    g_mirrorFailed = false;
    for (int slot = 0; slot < 2; ++slot) {
        for (int eye = 0; eye < 2; ++eye) {
            g_eyeReadbacks[slot][eye].Release();
            g_pendingEyes[slot][eye].valid = false;
        }
        g_overlayReadbacks[slot].Release();
        for (ReadbackTarget& readback : g_hudLayerReadbacks[slot]) readback.Release();
        g_pendingOverlays[slot].valid = false;
    }
    if (g_overlaySurface) { g_overlaySurface->Release(); g_overlaySurface = nullptr; }
    if (g_overlayTexture) { g_overlayTexture->Release(); g_overlayTexture = nullptr; }
    Relief::ForgetAllTextures();
    for (int layer = 0; layer < HudLayerCount; ++layer) {
        if (g_hudLayerSurfaces[layer]) { g_hudLayerSurfaces[layer]->Release(); g_hudLayerSurfaces[layer] = nullptr; }
        if (g_hudLayerTextures[layer]) { g_hudLayerTextures[layer]->Release(); g_hudLayerTextures[layer] = nullptr; }
    }
}

static bool g_testPatternEnabled = false;

static void CollectPreviousFrameImages(IDirect3DDevice9* device, int slot) {
    for (int eye = 0; eye < 2; ++eye) {
    PendingEyeImage& pendingEye = g_pendingEyes[slot][eye];
    ReadbackTarget& readback = g_eyeReadbacks[slot][eye];
    if (pendingEye.valid) {
        readback.Collect(g_settings.eyeForceOpaque, [&](void* bits, int pitch) {
            const double alphaStartMs = NowMs();
            // The projection layer is submitted without alpha blending, so the runtime should ignore
            // alpha anyway; eye_force_opaque=1 restores the old CPU pass if eyes ever look see-through.
            if (g_settings.eyeForceOpaque) readback.ForceOpaque(bits, pitch);
            const double uploadStartMs = NowMs();
            g_counters.totalAlphaMs += uploadStartMs - alphaStartMs;
            g_xr.SubmitEyeImage(pendingEye.eye, bits, pitch, pendingEye.view, false);
            g_counters.totalUploadMs += NowMs() - uploadStartMs;
            ++g_counters.eyeImagesSubmitted;
        });
        pendingEye.valid = false;
    }
    }
    PendingOverlayImage& pendingOverlay = g_pendingOverlays[slot];
    if (pendingOverlay.valid) {
        g_overlayReadbacks[slot].Collect(pendingOverlay.opaque, [&](void* bits, int pitch) {
            if (pendingOverlay.opaque) g_overlayReadbacks[slot].ForceOpaque(bits, pitch);
            const double uploadStartMs = NowMs();
            g_xr.SubmitOverlayImage(bits, pitch, false);
            g_counters.totalUploadMs += NowMs() - uploadStartMs;
            g_submittedOverlayOpaque = pendingOverlay.opaque;
            ++g_counters.overlayImagesSubmitted;
        });
        for (int layer = 0; layer < HudLayerCount; ++layer) {
            if (!pendingOverlay.layers[layer]) continue;
            g_hudLayerReadbacks[slot][layer].Collect(false, [&](void* bits, int pitch) {
                const double uploadStartMs = NowMs();
                g_xr.SubmitOverlayImage(bits, pitch, false, 1 + layer);
                g_counters.totalUploadMs += NowMs() - uploadStartMs;
            });
        }
        pendingOverlay.valid = false;
    }
}


static void StageThisFrameImages(IDirect3DDevice9* device, int slot, bool menuMode) {
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) return;

    if (menuMode) {
        // Menus and loading screens: the whole game screen becomes the panel.
        if (g_overlayReadbacks[slot].Ensure(device, g_xr.OverlayWidth(), g_xr.OverlayHeight(), D3DFMT_A8R8G8B8) &&
            g_overlayReadbacks[slot].Stage(device, backBuffer))
            g_pendingOverlays[slot] = { true, true, {} };
    } else {
        if (g_fullStereoFrame.active) {
            // Left eye from the backbuffer, right eye from its right-eye copy (if anything was drawn there).
            IDirect3DSurface9* rightBackBuffer = StereoTargets::RightEyeCurrent(backBuffer) ? StereoTargets::TwinSurface(device, backBuffer) : nullptr;
            for (int eye = 0; eye < 2; ++eye) {
                IDirect3DSurface9* source = eye == 1 && rightBackBuffer ? rightBackBuffer : backBuffer;
                if (g_eyeReadbacks[slot][eye].Ensure(device, g_xr.EyeWidth(), g_xr.EyeHeight(), D3DFMT_X8R8G8B8) &&
                    g_eyeReadbacks[slot][eye].Stage(device, source, g_fadeAmount))
                    g_pendingEyes[slot][eye] = { true, eye, g_fullStereoFrame.eyes[eye] };
            }
        } else {
            const int eye = EyeForThisFrame();
            if (g_eyeReadbacks[slot][eye].Ensure(device, g_xr.EyeWidth(), g_xr.EyeHeight(), D3DFMT_X8R8G8B8) &&
                g_eyeReadbacks[slot][eye].Stage(device, backBuffer, g_fadeAmount))
                g_pendingEyes[slot][eye] = { true, eye, g_renderingEyeView };
        }
        const RECT screen = OverlayScreenRect();
        if (g_overlayDrawnThisFrame && g_overlaySurface &&
            g_overlayReadbacks[slot].Ensure(device, g_xr.OverlayWidth(), g_xr.OverlayHeight(), D3DFMT_A8R8G8B8) &&
            g_overlayReadbacks[slot].Stage(device, g_overlaySurface, 0.0f, &screen)) {
            // The gem panels: just the highway's part of the screen.
            const RECT gemArea = { LONG(OverlayMargin + GemArea[0] * g_backBufferWidth), LONG(GemArea[1] * g_backBufferHeight),
                                   LONG(OverlayMargin + GemArea[2] * g_backBufferWidth), LONG(GemArea[3] * g_backBufferHeight) };
            PendingOverlayImage pending = { true, false, {} };
            for (int layer = 0; layer < HudLayerCount; ++layer)
                pending.layers[layer] = g_hudLayerDrawn[layer] && g_hudLayerSurfaces[layer] &&
                    g_hudLayerReadbacks[slot][layer].Ensure(device, g_xr.HudLayerWidth(), g_xr.HudLayerHeight(), D3DFMT_A8R8G8B8) &&
                    g_hudLayerReadbacks[slot][layer].Stage(device, g_hudLayerSurfaces[layer], 0.0f, &gemArea);
            g_pendingOverlays[slot] = pending;
        }
    }
    backBuffer->Release();
}

static void SubmitFrameToHeadset(IDirect3DDevice9* device) {
    if (!g_xr.FrameBegun()) return;

    if (g_testPatternEnabled) {
        g_xr.SubmitTestPattern();
        g_xr.EndFrame(g_settings.overlay, true, true, true);
        return;
    }

    // Images are handed over one frame late (each with the pose it was rendered with), so the
    // copy never has to wait for the GPU to finish the frame that's still being drawn.
    const int currentSlot = g_readbackSlot;
    const int previousSlot = 1 - currentSlot;
    const bool menuMode = !g_vrCameraUsedThisFrame;
    CollectPreviousFrameImages(device, previousSlot);
    StageThisFrameImages(device, currentSlot, menuMode);
    g_readbackSlot = previousSlot;

    ++g_counters.framesWithBegunXrFrame;
    if (g_renderingEyeViewValid) ++g_counters.framesWithEyeView;
    const bool showOverlay = menuMode || g_settings.showOverlay;
    g_xr.SetHudLayer(0, 0.0f, 0);
    g_xr.SetHudLayer(1, 0.0f, 1);
    g_xr.EndFrame(g_settings.overlay, !menuMode, showOverlay, g_submittedOverlayOpaque);
    if (g_xr.LastLayerCount() > 0) ++g_counters.framesWithLayers;
    if (g_xr.LastShouldRender()) ++g_counters.framesShouldRender;
}

static double NowMs() {
    static LARGE_INTEGER frequency = {};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * 1000.0 / double(frequency.QuadPart);
}

static void AddTiming(double durationMs, double& total, double& worst) {
    total += durationMs;
    if (durationMs > worst) worst = durationMs;
}

static void LogStatusIfDue() {
    if (g_frameIndex == 1) LogLine("[status] first frame presented");
    if (g_frameIndex % StatusIntervalFrames != 0) return;
    const FrameCounters& counters = g_counters;
    LogLine("[status] last %u frames: xr frames %u, eye views %u, shouldRender %u, 3D draws main %u / VR camera %u, "
            "overlay draws %u, submitted eye %u overlay %u, frames with layers %u | anchor %s, eyes ever submitted L%d R%d, "
            "session state %d, last 3D target %ux%u (backbuffer %ux%u), mode %s",
            StatusIntervalFrames, counters.framesWithBegunXrFrame, counters.framesWithEyeView, counters.framesShouldRender,
            counters.mainTargetCameraDraws, counters.vrCameraDraws, counters.overlayDraws,
            counters.eyeImagesSubmitted, counters.overlayImagesSubmitted, counters.framesWithLayers,
            g_anchorValid ? "set" : "NOT SET", int(g_xr.EyeImageSubmitted(0)), int(g_xr.EyeImageSubmitted(1)),
            g_xr.SessionState(), counters.lastRenderTargetWidth, counters.lastRenderTargetHeight,
            g_backBufferWidth, g_backBufferHeight, g_previousFrameUsedVrCamera ? "VR" : "menu screen");
    const double frames = double(StatusIntervalFrames);
    LogLine("[timing] headset copy breakdown avg: queue %.1f ms, GPU->CPU wait %.1f ms, alpha %.1f ms, upload %.1f ms",
            counters.totalStageMs / double(StatusIntervalFrames), counters.totalReadbackMs / double(StatusIntervalFrames), counters.totalAlphaMs / double(StatusIntervalFrames),
            counters.totalUploadMs / double(StatusIntervalFrames));
    LogLine("[timing] frame avg %.1f ms (%.0f fps) worst %.1f | headset copy avg %.1f worst %.1f | "
            "xr wait/poll avg %.1f worst %.1f | %s",
            counters.totalFrameMs / frames, counters.totalFrameMs > 0 ? 1000.0 * frames / counters.totalFrameMs : 0.0,
            counters.worstFrameMs, counters.totalSubmitMs / frames, counters.worstSubmitMs,
            counters.totalXrWaitMs / frames, counters.worstXrWaitMs, CameraScan::StatusText());
    if (g_chosenCharacterLocked && g_settings.bandMemberMode != LeaveBandMember && InSong()) {
        if (g_characterHeadValid)
            LogLine("[band] their head read from their face on %u of %u frames: %.2f %.2f %.2f (%.2f m from their spot, %.2f m up); "
                    "facing read on %u frames, turned %.0f deg from normal; %u other parts (strings etc.) counted as theirs",
                    g_faceReadsThisInterval, StatusIntervalFrames, g_characterHead.x, g_characterHead.y, g_characterHead.z,
                    FlatDistance(g_characterHead, g_chosenCharacterRoot), g_characterHead.y - g_chosenCharacterRoot.y,
                    g_facingReadsThisInterval, g_facingValid ? WrapAngle(g_characterFacing - g_restFacing) * 57.2958f : 0.0f,
                    g_worldSpacePartsThisInterval);
        else
            LogLine("[band] couldn't read their face (%u reads): placing them by their feet instead; %u world-space parts counted",
                    g_faceReadsThisInterval, g_worldSpacePartsThisInterval);
        if (g_frameIndex % (StatusIntervalFrames * 5) == 0) MeshWatch::LogWatched();
    }
    g_faceReadsThisInterval = 0;
    g_facingReadsThisInterval = 0;
    g_worldSpacePartsThisInterval = 0;
    if (g_settings.fullStereo) {
        LogLine("[full stereo] %u draws repeated for the right eye", counters.rightEyeDraws);
        StereoTargets::LogSummary();
    }
    if (counters.depthOfFieldDraws)
        LogLine("[dof] depth-of-field draws %u, blur removed %u (F4 toggles)", counters.depthOfFieldDraws, counters.depthOfFieldSkipped);
    if (counters.followFrames)
        LogLine("[follow] game camera matched your head on %u of %u frames", counters.followMatchedFrames, counters.followFrames);
    CameraScan::LogCullingHookHits();
    g_counters = FrameCounters();
}

static void StartNextHeadsetFrame() {
    UINT overlayWidth = std::min<UINT>(g_backBufferWidth, 1920);
    UINT overlayHeight = g_backBufferWidth ? overlayWidth * g_backBufferHeight / g_backBufferWidth : 1080;
    if (!g_xr.IsRunning()) {
        // The panels' size is fixed once the session has its swapchains, so the area only changes here.
        GemArea[0] = 0.5f - 0.5f * g_settings.hudDepthArea;
        GemArea[2] = 0.5f + 0.5f * g_settings.hudDepthArea;
        g_xr.SetHudLayerArea(GemArea[0], GemArea[1], GemArea[2], GemArea[3]);
    }
    if (!g_xr.IsRunning() && g_xr.TryInitialize(g_loaderPath, overlayWidth, overlayHeight, g_settings.renderScale, LogLine))
        LogLine("[xr] ready, waiting for the session to start");

    g_xr.PollEvents();
    g_renderingEyeViewValid = false;
    g_fullStereoFrame.active = false;
    if (g_xr.IsRunning() && g_xr.BeginFrame() && g_xr.ViewsValid()) {
        if (g_settings.fullStereo) {
            g_fullStereoFrame.eyes[0] = g_xr.CurrentView(0);
            g_fullStereoFrame.eyes[1] = g_xr.CurrentView(1);
            g_renderingEyeView = g_fullStereoFrame.eyes[0];
            g_fullStereoFrame.active = true;
        } else {
            g_renderingEyeView = g_xr.CurrentView(EyeForThisFrame());
        }
        g_renderingEyeViewValid = true;
    }
}

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------
// Yaw of the head within the anchor space, so movement follows where you're looking.
static float HeadYawInAnchorSpace() {
    if (!g_renderingEyeViewValid) return 0.0f;
    const XrQuaternionf& orientation = g_renderingEyeView.pose.orientation;
    const float forwardX = -2.0f * (orientation.x * orientation.z + orientation.y * orientation.w);
    const float forwardZ = -(1.0f - 2.0f * (orientation.x * orientation.x + orientation.y * orientation.y));
    return atan2f(-forwardX, -forwardZ);
}

static void MoveAnchor(float forward, float right, float up) {
    // Heading = anchor yaw + head yaw. Yaw maps +Z to (sin, 0, cos) ("back"); +X to (cos, 0, -sin).
    const float heading = g_anchorYaw + HeadYawInAnchorSpace();
    const float cosine = cosf(heading), sine = sinf(heading);
    const Vector3 rightAxis = { cosine, 0.0f, -sine };
    const Vector3 backAxis  = { sine, 0.0f, cosine };
    g_anchorPosition.x += right * rightAxis.x - forward * backAxis.x;
    g_anchorPosition.y += up;
    g_anchorPosition.z += right * rightAxis.z - forward * backAxis.z;
}

static double NowMs();

static void HandleMovement() {
    static double previousMs = 0;
    const double nowMs = NowMs();
    const float seconds = previousMs > 0 ? float(std::min(0.1, (nowMs - previousMs) / 1000.0)) : 0.0f;
    previousMs = nowMs;
    if (seconds <= 0.0f) return;

    const float speedMetresPerSecond = ActionHeld(ActionMoveFast) ? 4.5f : 1.5f;
    const float turnRadiansPerSecond = 1.2f;   // ~70 degrees per second
    const float distance = speedMetresPerSecond * g_settings.worldScale * seconds;

    // Not while Ctrl / Alt is held (those are other hotkeys).
    if (KeyDown(VK_CONTROL) || KeyDown(VK_MENU)) return;
    const float forward = float(ActionHeld(ActionMoveForward)) - float(ActionHeld(ActionMoveBack));
    const float right   = float(ActionHeld(ActionMoveRight)) - float(ActionHeld(ActionMoveLeft));
    const float up      = float(ActionHeld(ActionMoveUp)) - float(ActionHeld(ActionMoveDown));
    if (forward || right || up) MoveAnchor(forward * distance, right * distance, up * distance);

    if (g_settings.snapTurnDegrees > 0.0f) {
        const float snap = g_settings.snapTurnDegrees * 3.14159265f / 180.0f;
        if (ActionTapped(ActionTurnLeft)) g_anchorYaw += snap;
        if (ActionTapped(ActionTurnRight)) g_anchorYaw -= snap;
        return;
    }
    const float turn = float(ActionHeld(ActionTurnLeft)) - float(ActionHeld(ActionTurnRight));
    g_anchorYaw += turn * turnRadiansPerSecond * seconds;
}

static bool ScriptedCameraPose(Vector3& position, float& yaw);
static void RememberSpot(bool forget);
static void UpdateCameraHooks();

static void RequestFullRecenter() {
    g_xr.RequestRecenter();
    // While the game camera follows your head, its rendered camera is you: use the scripted one.
    float scripted[16];
    Vector3 position;
    float yaw;
    if (CameraScan::FollowedCamera() && CameraScan::ScriptedCameraMatrix(scripted) && ScriptedCameraPose(position, yaw)) {
        g_anchorPosition = position;
        g_anchorYaw = yaw;
        LogLine("[anchor] from scripted camera: position %.2f %.2f %.2f yaw %.1f deg", position.x, position.y, position.z, yaw * 57.2958f);
        return;
    }
    g_anchorFromCameraRequested = true;
}

static void HandleHotkeys() {
    UpdateKeyStates();
    if (!GameHasFocus()) return;
    HandleMovement();
    if (ActionPressed(ActionRecenter)) RequestFullRecenter();

    if (ActionPressed(ActionWorldScaleDown)) g_settings.worldScale /= 1.05f;
    if (ActionPressed(ActionWorldScaleUp)) g_settings.worldScale *= 1.05f;

    if (ActionPressed(ActionHudCloser))   g_settings.overlay.distance = std::max(0.2f, g_settings.overlay.distance - 0.05f);
    if (ActionPressed(ActionHudFurther))  g_settings.overlay.distance += 0.05f;
    if (ActionPressed(ActionHudWider))    g_settings.overlay.width += 0.05f;
    if (ActionPressed(ActionHudNarrower)) g_settings.overlay.width = std::max(0.2f, g_settings.overlay.width - 0.05f);
    if (ActionPressed(ActionHudHigher))   g_settings.overlay.height += 0.05f;
    if (ActionPressed(ActionHudLower))    g_settings.overlay.height -= 0.05f;
    if (ActionPressed(ActionToggleHud)) g_settings.showOverlay = !g_settings.showOverlay;
    if (ActionPressed(ActionToggleVrMenus)) {
        g_settings.vrMenus = !g_settings.vrMenus;
        LogLine("[menus] 3D menus in VR %s (F11 saves this)", g_settings.vrMenus ? "on" : "off");
    }
    if (ActionPressed(ActionToggleDepthOfField)) {
        g_settings.depthOfFieldOff = !g_settings.depthOfFieldOff;
        LogLine("[dof] depth of field %s in VR (F11 saves this)", g_settings.depthOfFieldOff ? "off" : "on");
    }
    if (ActionPressed(ActionMirrorHud)) {
        g_settings.mirrorHud = (g_settings.mirrorHud + 1) % 3;
        static const char* const modeNames[] = { "hidden", "flat on top", "floating like the headset" };
        LogLine("[mirror] HUD on the monitor: %s (F11 saves this)", modeNames[g_settings.mirrorHud]);
    }
    if (ActionPressed(ActionSaveSpot)) RememberSpot(false);
    if (ActionPressed(ActionForgetSpot)) RememberSpot(true);
    if (ActionPressed(ActionBandMemberMode)) CycleBandMemberMode();
    if (ActionPressed(ActionPickBandMember) && InSong()) CycleChosenCharacter();
    const bool bandMemberUp = ActionPressed(ActionBandMemberUp), bandMemberDown = ActionPressed(ActionBandMemberDown);
    if (bandMemberUp || bandMemberDown) {
        if (g_settings.avatarAnchor == 1) {
            g_settings.avatarHeadDrop += bandMemberUp ? -0.02f : 0.02f;
            LogLine("[band] your band member's face %.2f m below your eyes (F11 saves this)", g_settings.avatarHeadDrop);
        } else {
            g_settings.avatarEyeHeight += bandMemberUp ? -0.05f : 0.05f;
            LogLine("[band] your band member's feet %.2f m below your eyes (F11 saves this)", g_settings.avatarEyeHeight);
        }
    }
    const bool bandMemberForward = ActionPressed(ActionBandMemberForward), bandMemberBack = ActionPressed(ActionBandMemberBack);
    if (bandMemberForward || bandMemberBack) {
        g_settings.avatarOffset += bandMemberForward ? 0.05f : -0.05f;
        LogLine("[band] your band member %.2f m forward of you (F11 saves this)", g_settings.avatarOffset);
    }
    if (ActionPressed(ActionXrRetry) && g_xr.HasGivenUp()) {
        LogLine("[xr] retrying");
        g_xr.RetryInitialization();
    }
    if (ActionPressed(ActionSaveSettings)) {
        SaveSettings();
        LogLine("[settings] saved: world scale %.3f, hud distance %.2f width %.2f height %.2f",
                g_settings.worldScale, g_settings.overlay.distance, g_settings.overlay.width, g_settings.overlay.height);
        MessageBeep(MB_OK);
    }

    // debug_tools
    if (ActionPressed(ActionCameraScan)) {
        if (g_gameViewProjEverKnown) CameraScan::Start(GameProjectionScale(0), GameProjectionScale(1), LogLine);
        else LogLine("[camera scan] no game camera seen yet - start a song first");
    }
    if (ActionPressed(ActionCameraScanList)) CameraScan::LogCandidates();
    if (ActionPressed(ActionRoleProbe)) {
        g_roleProbeRequested = true;
        LogLine("[role] probing the next frame");
    }
    if (ActionPressed(ActionToggleFollowHead)) {
        g_settings.followHead = !g_settings.followHead;
        LogLine("[follow] head follow %s (F11 saves this)", g_settings.followHead ? "enabled" : "disabled");
    }
    if (ActionPressed(ActionToggleWideCulling)) {
        g_settings.wideCulling = !g_settings.wideCulling;
        LogLine("[culling] wide culling %s (F11 saves this)", g_settings.wideCulling ? "enabled" : "disabled");
    }
    const bool fovDown = ActionPressed(ActionCullingFovDown), fovUp = ActionPressed(ActionCullingFovUp);
    if (fovDown || fovUp) {
        g_settings.cullingFov = std::min(170.0f, std::max(60.0f, g_settings.cullingFov + (fovUp ? 5.0f : -5.0f)));
        LogLine("[culling] culling FOV %.0f deg (F11 saves this)", g_settings.cullingFov);
    }
    if (ActionPressed(ActionHudDump)) {
        g_logHudDrawsThisFrame = true;   // next frame
        g_hudDrawLines = 0;
        LogLine("[hud] next frame's HUD draws:");
    }
    if (ActionPressed(ActionFinderNext)) StepDrawFinder(true);
    if (ActionPressed(ActionFinderPrevious)) StepDrawFinder(false);
    if (ActionPressed(ActionTestPattern)) {
        g_testPatternEnabled = !g_testPatternEnabled;
        LogLine("[xr] test pattern %s", g_testPatternEnabled ? "ON" : "off");
    }
}

// ---------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DVertexShader9** shaderOut) {
    HRESULT result = g_originalCreateVertexShader(device, function, shaderOut);
    if (SUCCEEDED(result) && function && shaderOut && *shaderOut) {
        g_shaders[*shaderOut] = ParseShader(function);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    g_currentShader = shader;
    return g_originalSetVertexShader(device, shader);
}

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DPixelShader9** shaderOut) {
    HRESULT result = g_originalCreatePixelShader(device, function, shaderOut);
    if (SUCCEEDED(result) && function && shaderOut && *shaderOut) {
        PixelShaderInfo info = ParsePixelShader(function);
        if (info.isDepthOfField) {
            size_t tokenCount = 1;
            while (tokenCount < (1u << 16) && function[tokenCount] != EndToken) {
                const DWORD token = function[tokenCount];
                tokenCount += (token & 0xFFFF) == 0xFFFE ? 1 + ((token >> 16) & 0x7FFF) : 1;
            }
            info.bytecode.assign(function, function + tokenCount + 1);
            LogLine("[dof] found depth-of-field shader %p: %s", (void*)*shaderOut, info.registerMap.c_str());
            static int dumped = 0;
            char fileName[64];
            snprintf(fileName, sizeof(fileName), "\\dof_shader_%d.bin", dumped++);
            if (FILE* file = g_settings.debugTools ? fopen((g_outputDir + fileName).c_str(), "wb") : nullptr) {
                fwrite(info.bytecode.data(), sizeof(DWORD), info.bytecode.size(), file);
                fclose(file);
                LogLine("[dof] saved its bytecode to ghwt_vr%s", fileName);
            }
        }
        Relief::NotePixelShader(*shaderOut, function, info.hash);   // kept for a 3D copy if it ever draws a piece with a height map
        // Saved for study (gem relief), with debug_tools.
        const uint32_t gemPixelShaders[] = { 0x12827EBF, 0x5DBF7AEB, 0x0B5AD3EA, 0xFE74FFEA, 0x2393BA2D, 0x6E589F67 };
        if (g_settings.debugTools && std::find(std::begin(gemPixelShaders), std::end(gemPixelShaders), info.hash) != std::end(gemPixelShaders)) {
            size_t tokenCount = 1;
            while (tokenCount < (1u << 16) && function[tokenCount] != EndToken) {
                const DWORD token = function[tokenCount];
                tokenCount += (token & 0xFFFF) == 0xFFFE ? 1 + ((token >> 16) & 0x7FFF) : 1;
            }
            char fileName[64];
            snprintf(fileName, sizeof(fileName), "\\hud_ps_%08X.bin", info.hash);
            if (FILE* file = fopen((g_outputDir + fileName).c_str(), "wb")) {
                fwrite(function, sizeof(DWORD), tokenCount + 1, file);
                fclose(file);
                LogLine("[hud] saved HUD pixel shader %08X to ghwt_vr%s", info.hash, fileName);
            }
        }
        g_pixelShaders[*shaderOut] = info;
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedSetPixelShaderConstantF(IDirect3DDevice9* device, UINT startRegister, const float* data, UINT registerCount) {
    if (data && startRegister < 224) {
        const UINT copyCount = std::min<UINT>(registerCount, 224 - startRegister);
        memcpy(g_pixelConstants[startRegister], data, copyCount * 4 * sizeof(float));
    }
    return g_originalSetPixelShaderConstantF(device, startRegister, data, registerCount);
}

static HRESULT STDMETHODCALLTYPE HookedSetPixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    g_currentPixelShader = shader;
    return g_originalSetPixelShader(device, shader);
}

static HRESULT STDMETHODCALLTYPE HookedSetVertexShaderConstantF(IDirect3DDevice9* device, UINT startRegister, const float* data, UINT registerCount) {
    if (data && startRegister < MaxFloatConstants) {
        UINT copyCount = std::min<UINT>(registerCount, MaxFloatConstants - startRegister);
        memcpy(g_gameConstants[startRegister], data, copyCount * 4 * sizeof(float));

        // The game just overwrote these registers with its own values.
        const int endRegister = int(startRegister + copyCount);
        g_modifiedOnDevice.erase(std::remove_if(g_modifiedOnDevice.begin(), g_modifiedOnDevice.end(),
            [&](const ModifiedRange& range) {
                return range.baseRegister < endRegister && range.baseRegister + range.registerCount > int(startRegister);
            }), g_modifiedOnDevice.end());
    }
    return g_originalSetVertexShaderConstantF(device, startRegister, data, registerCount);
}

static HRESULT STDMETHODCALLTYPE HookedSetRenderTarget(IDirect3DDevice9* device, DWORD targetIndex, IDirect3DSurface9* surface) {
    if (targetIndex < 4 && g_modWorkDepth == 0) g_boundTargets[targetIndex] = surface;
    if (targetIndex == 0 && surface) {
        g_currentRenderTarget = surface;
        D3DSURFACE_DESC description;
        if (SUCCEEDED(surface->GetDesc(&description))) {
            g_renderTargetWidth = description.Width;
            g_renderTargetHeight = description.Height;
        }
    }
    return g_originalSetRenderTarget(device, targetIndex, surface);
}

static HRESULT STDMETHODCALLTYPE HookedSetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    if (g_modWorkDepth == 0) g_boundDepth = surface;
    return g_originalSetDepthStencilSurface(device, surface);
}

static HRESULT STDMETHODCALLTYPE HookedSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    if (stage < TrackedTextureStages && g_modWorkDepth == 0) g_boundTextures[stage] = texture;
    return g_originalSetTexture(device, stage, texture);
}

static HRESULT STDMETHODCALLTYPE HookedCreateVertexBuffer(IDirect3DDevice9* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                          IDirect3DVertexBuffer9** bufferOut, HANDLE* sharedHandle) {
    const HRESULT result = g_originalCreateVertexBuffer(device, length, usage, fvf, pool, bufferOut, sharedHandle);
    if (SUCCEEDED(result) && bufferOut && *bufferOut) MeshWatch::HookVertexBuffers(*bufferOut);
    return result;
}

static bool TrackingGameResources() {
    return g_modWorkDepth == 0 && !StereoTargets::CreatingTwin();
}

static HRESULT STDMETHODCALLTYPE HookedCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                                     D3DPOOL pool, IDirect3DTexture9** textureOut, HANDLE* sharedHandle) {
    const HRESULT result = g_originalCreateTexture(device, width, height, levels, usage, format, pool, textureOut, sharedHandle);
    if (SUCCEEDED(result) && textureOut && *textureOut) {
        Relief::ForgetTexture(*textureOut);   // a new texture at an old one's address
        if (TrackingGameResources()) StereoTargets::NoteTexture(*textureOut);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedCreateRenderTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
                                                          DWORD multiSampleQuality, BOOL lockable, IDirect3DSurface9** surfaceOut, HANDLE* sharedHandle) {
    const HRESULT result = g_originalCreateRenderTarget(device, width, height, format, multiSample, multiSampleQuality, lockable, surfaceOut, sharedHandle);
    if (SUCCEEDED(result) && surfaceOut && *surfaceOut && TrackingGameResources()) StereoTargets::NoteSurface(*surfaceOut);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDepthStencilSurface(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
                                                                 DWORD multiSampleQuality, BOOL discard, IDirect3DSurface9** surfaceOut, HANDLE* sharedHandle) {
    const HRESULT result = g_originalCreateDepthStencilSurface(device, width, height, format, multiSample, multiSampleQuality, discard, surfaceOut, sharedHandle);
    if (SUCCEEDED(result) && surfaceOut && *surfaceOut && TrackingGameResources()) StereoTargets::NoteSurface(*surfaceOut);
    return result;
}

// Copies and fills into a per-eye target are repeated into its right-eye copy.
static bool FullStereoCopyWanted(IDirect3DSurface9* destination) {
    return g_fullStereoFrame.active && g_modWorkDepth == 0 && VrCameraActive() && StereoTargets::IsPerEye(destination);
}

static HRESULT STDMETHODCALLTYPE HookedStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* sourceRect,
                                                   IDirect3DSurface9* destination, const RECT* destinationRect, D3DTEXTUREFILTERTYPE filter) {
    const HRESULT result = g_originalStretchRect(device, source, sourceRect, destination, destinationRect, filter);
    if (FAILED(result) || g_modWorkDepth != 0) return result;
    bool rightEyeToo = false;
    if (FullStereoCopyWanted(destination)) {
        IDirect3DSurface9* rightDestination = StereoTargets::TwinSurface(device, destination);
        // The source's twin only if it's up to date (else both eyes get the original).
        IDirect3DSurface9* rightSource = StereoTargets::RightEyeCurrent(source) ? StereoTargets::TwinSurface(device, source) : nullptr;
        rightEyeToo = rightDestination &&
            SUCCEEDED(g_originalStretchRect(device, rightSource ? rightSource : source, sourceRect, rightDestination, destinationRect, filter));
    }
    StereoTargets::NoteWrite(destination, rightEyeToo);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedColorFill(IDirect3DDevice9* device, IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR colour) {
    const HRESULT result = g_originalColorFill(device, surface, rect, colour);
    if (FAILED(result) || g_modWorkDepth != 0) return result;
    bool rightEyeToo = false;
    if (FullStereoCopyWanted(surface))
        if (IDirect3DSurface9* rightSurface = StereoTargets::TwinSurface(device, surface))
            rightEyeToo = SUCCEEDED(g_originalColorFill(device, rightSurface, rect, colour));
    StereoTargets::NoteWrite(surface, rightEyeToo);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedClear(IDirect3DDevice9* device, DWORD rectCount, const D3DRECT* rects, DWORD flags, D3DCOLOR colour, float depth, DWORD stencil) {
    const HRESULT result = g_originalClear(device, rectCount, rects, flags, colour, depth, stencil);
    if (FAILED(result) || g_modWorkDepth != 0) return result;
    bool rightEyeToo = false;
    RightEyeBindings saved;
    if (FullStereoWanted() && BindRightEyeTargets(device, saved, false)) {
        rightEyeToo = SUCCEEDED(g_originalClear(device, rectCount, rects, flags, colour, depth, stencil));
        RestoreLeftEyeTargets(device, saved);
    }
    if (flags & D3DCLEAR_TARGET) NoteTargetsWritten(rightEyeToo);
    return result;
}

static UINT VerticesFor(D3DPRIMITIVETYPE type, UINT primitiveCount) {
    switch (type) {
        case D3DPT_POINTLIST:     return primitiveCount;
        case D3DPT_LINELIST:      return primitiveCount * 2;
        case D3DPT_LINESTRIP:     return primitiveCount + 1;
        case D3DPT_TRIANGLELIST:  return primitiveCount * 3;
        default:                  return primitiveCount + 2;   // strips / fans
    }
}

static HRESULT STDMETHODCALLTYPE HookedDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) {
    g_drawPrimitiveType = type;
    g_drawFirstVertex = startVertex;
    g_drawVertexCount = VerticesFor(type, primitiveCount);
    return HandleDraw(device, [&]() { return g_originalDrawPrimitive(device, type, startVertex, primitiveCount); });
}

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT vertexCount, UINT startIndex, UINT primitiveCount) {
    g_drawPrimitiveType = type;
    g_drawFirstVertex = UINT(std::max(0, baseVertex + int(minVertex)));
    g_drawVertexCount = vertexCount;
    return HandleDraw(device, [&]() {
        return g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    });
}

static HRESULT STDMETHODCALLTYPE HookedDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitiveCount, const void* vertexData, UINT stride) {
    g_drawPrimitiveType = type;
    g_upVertexData = vertexData;
    g_upStride = stride;
    g_upPrimitiveCount = primitiveCount;
    g_upIndexData = nullptr;
    g_drawFirstVertex = 0;
    g_drawVertexCount = VerticesFor(type, primitiveCount);
    const HRESULT result = HandleDraw(device, [&]() { return g_originalDrawPrimitiveUP(device, type, primitiveCount, vertexData, stride); });
    g_upVertexData = nullptr;
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minVertex, UINT vertexCount, UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat, const void* vertexData, UINT stride) {
    g_drawPrimitiveType = type;
    g_upVertexData = vertexData;
    g_upStride = stride;
    g_upPrimitiveCount = primitiveCount;
    g_upIndexData = indexData;
    g_upIndexFormat = indexFormat;
    g_drawFirstVertex = minVertex;
    g_drawVertexCount = vertexCount;
    const HRESULT result = HandleDraw(device, [&]() {
        return g_originalDrawIndexedPrimitiveUP(device, type, minVertex, vertexCount, primitiveCount, indexData, indexFormat, vertexData, stride);
    });
    g_upVertexData = nullptr;
    return result;
}

// ---------------------------------------------------------------------------
// Camera hooks: head follow + optional wide culling (songs in VR only)
//
// GHWT culls (and picks what to draw) from its own scripted camera. Your VR view stays where you
// stand while the scripted camera keeps cutting between shots, so at every cut a different part
// of the venue gets culled around you. Head follow writes your head pose into the game camera
// object, so the game culls for what you're actually looking at.
// ---------------------------------------------------------------------------
static bool g_cameraHooksArmed = false;
static bool g_cameraHooksUnavailable = false;

// Which camera object is followed, and how its matrix rows map to the game's camera axes.
static float g_followBackSign = 1.0f;      // +1: row 2 is the back axis, -1: it's the forward axis
static int   g_followMismatchFrames = 0;
static int   g_cameraDumpsThisSong = 0;
static bool  g_followMatrixWritten = false;
static float g_lastFollowMatrix[16];

static float Dot(const Vector3& left, const Vector3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

static Vector3 RowOf(const float matrix[16], int row) {
    return { matrix[row * 4 + 0], matrix[row * 4 + 1], matrix[row * 4 + 2] };
}

// Does this camera matrix describe the game camera seen this frame? Sets the back-axis sign.
static bool MatrixMatchesGameCamera(const float matrix[16], float& backSign) {
    if (!g_gameCameraAxesKnownThisFrame || !g_gameViewProjKnownThisFrame) return false;
    Vector3 gamePosition;
    if (!CameraPositionFromViewProj(g_gameViewProj, gamePosition)) return false;
    const float rightDot = Dot(RowOf(matrix, 0), g_gameCameraAxes[0]);
    const float upDot    = Dot(RowOf(matrix, 1), g_gameCameraAxes[1]);
    const float backDot  = Dot(RowOf(matrix, 2), g_gameCameraAxes[2]);
    const Vector3 position = RowOf(matrix, 3);
    const float distanceX = position.x - gamePosition.x, distanceY = position.y - gamePosition.y, distanceZ = position.z - gamePosition.z;
    const float distance = sqrtf(distanceX * distanceX + distanceY * distanceY + distanceZ * distanceZ);
    if (rightDot < 0.98f || upDot < 0.98f || fabsf(backDot) < 0.98f || distance > 0.5f) return false;
    backSign = backDot > 0 ? 1.0f : -1.0f;
    return true;
}

static void LogCameraDump(const CameraScan::CameraObjectSnapshot& snapshot) {
    Vector3 gamePosition = {};
    CameraPositionFromViewProj(g_gameViewProj, gamePosition);
    LogLine("[follow] camera object %08X (hits %ld) does not match the game camera. Game: right %.3f %.3f %.3f up %.3f %.3f %.3f "
            "back %.3f %.3f %.3f pos %.2f %.2f %.2f. Object floats from +0x00:", unsigned(snapshot.object), snapshot.hits,
            g_gameCameraAxes[0].x, g_gameCameraAxes[0].y, g_gameCameraAxes[0].z, g_gameCameraAxes[1].x, g_gameCameraAxes[1].y,
            g_gameCameraAxes[1].z, g_gameCameraAxes[2].x, g_gameCameraAxes[2].y, g_gameCameraAxes[2].z,
            gamePosition.x, gamePosition.y, gamePosition.z);
    char line[512];
    for (int start = 0; start < CameraScan::CameraDumpBytes / 4; start += 8) {
        int length = snprintf(line, sizeof(line), "   +%02X:", start * 4);
        for (int index = start; index < start + 8; ++index)
            length += snprintf(line + length, sizeof(line) - length, " %10.4f", snapshot.raw[index]);
        LogLine("%s", line);
    }
}

// Picks the camera object to follow: the one whose own matrix is the camera the game rendered
// with this frame. (Once followed, the game renders with our matrix instead, so a different
// object matching means the game switched cameras at a cut.)
static void UpdateFollowTarget() {
    if (!g_gameCameraAxesKnownThisFrame || !g_gameViewProjKnownThisFrame) return;
    static CameraScan::CameraObjectSnapshot snapshots[32];   // shadow cameras alone can fill 8
    const int count = CameraScan::SeenCameras(snapshots, 32);
    const uintptr_t followed = CameraScan::FollowedCamera();

    if (followed && g_followMatrixWritten) {
        ++g_counters.followFrames;
        float sign;
        if (MatrixMatchesGameCamera(g_lastFollowMatrix, sign)) {
            ++g_counters.followMatchedFrames;
            g_followMismatchFrames = 0;
        } else {
            ++g_followMismatchFrames;
        }
    }

    // Cameras come and go while a song loads; only lock on once the song has been running a few
    // seconds and the same camera has matched for half a second.
    static uintptr_t candidate = 0;
    static int candidateFrames = 0;
    if (g_framesInSong < 300) { candidate = 0; candidateFrames = 0; return; }
    bool candidateSeen = false;
    for (int index = 0; index < count; ++index) {
        const CameraScan::CameraObjectSnapshot& snapshot = snapshots[index];
        if (snapshot.object == followed) continue;
        if (snapshot.hits < 100) continue;   // per-shot cameras come and go; the render camera persists
        float sign;
        if (!MatrixMatchesGameCamera(snapshot.matrix, sign)) continue;
        candidateSeen = true;
        if (snapshot.object != candidate) { candidate = snapshot.object; candidateFrames = 0; }
        if (++candidateFrames < 30) break;
        candidate = 0;
        candidateFrames = 0;
        LogLine("[follow] %s view %08X (row 2 is the %s axis)", followed ? "game switched to" : "following",
                unsigned(snapshot.object), sign > 0 ? "back" : "forward");
        g_followBackSign = sign;
        g_followMatrixWritten = false;
        g_followMismatchFrames = 0;
        CameraScan::SetFollowCamera(snapshot.object, snapshot.matrix);   // starts unchanged; head pose next frame
        return;
    }
    if (!candidateSeen) { candidate = 0; candidateFrames = 0; }

    if (!followed && count > 0 && g_cameraDumpsThisSong < 2 && g_frameIndex % 240 == 0) {
        ++g_cameraDumpsThisSong;
        int dumped = 0;
        for (int index = 0; index < count && dumped < 6; ++index) {
            const float baseFov = snapshots[index].raw[0xD0 / 4];
            if (baseFov <= 1.0f || baseFov >= 179.0f || baseFov == 21.0f) continue;   // stale, or a shadow camera (21 deg)
            LogCameraDump(snapshots[index]);
            ++dumped;
        }
        if (!dumped) LogLine("[follow] %d camera objects seen, none usable (stale or shadow cameras)", count);
    }
}

// Head pose for the coming frame -> the followed camera object.
static void WriteHeadIntoGameCamera() {
    if (!CameraScan::FollowedCamera() || !g_renderingEyeViewValid || !g_anchorValid) return;
    if (!g_vrMatricesValid) ComputeVrMatrices();
    if (!g_vrMatricesValid) return;
    float matrix[16];
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            matrix[row * 4 + column] = g_vrCameraToWorld.values[row][column] * (row == 2 && column < 3 ? g_followBackSign : 1.0f);
    for (int row = 0; row < 3; ++row) matrix[row * 4 + 3] = 0.0f;
    matrix[15] = 1.0f;
    memcpy(g_lastFollowMatrix, matrix, sizeof(matrix));
    g_followMatrixWritten = true;
    CameraScan::SetFollowCamera(CameraScan::FollowedCamera(), matrix);
}

// The scripted camera (where the game wants the shot), even while it's following the head.
static bool ScriptedCameraPose(Vector3& position, float& yaw) {
    float matrix[16];
    if (CameraScan::FollowedCamera() && CameraScan::ScriptedCameraMatrix(matrix)) {
        position = RowOf(matrix, 3);
        const Vector3 back = { matrix[8] * g_followBackSign, matrix[9] * g_followBackSign, matrix[10] * g_followBackSign };
        yaw = atan2f(back.x, back.z);
        return true;
    }
    if (!g_gameViewProjKnownThisFrame || !CameraPositionFromViewProj(g_gameViewProj, position)) return false;
    yaw = CameraYawFromViewProj(g_gameViewProj);
    return true;
}

static void UpdateCameraHooks() {
    const bool inVrSong = g_xr.IsRunning() && InSong();
    const bool wanted = inVrSong && (g_settings.followHead || g_settings.wideCulling) && !g_cameraHooksUnavailable;
    if (wanted) {
        if (!CameraScan::ArmCameraHooks(g_settings.wideCulling, g_settings.cullingFov, LogLine)) {
            g_cameraHooksUnavailable = true;
            return;
        }
        g_cameraHooksArmed = true;
    } else if (g_cameraHooksArmed) {
        CameraScan::SetFollowCamera(0, nullptr);
        CameraScan::DisarmCameraHooks();
        CameraScan::ForgetCameras();
        g_cameraHooksArmed = false;
        g_followMatrixWritten = false;
    }
    if (!g_cameraHooksArmed) return;

    if (!g_settings.followHead) {
        if (CameraScan::FollowedCamera()) { CameraScan::SetFollowCamera(0, nullptr); g_followMatrixWritten = false; }
        return;
    }
    UpdateFollowTarget();

    // Diagnostics (crowd facing): 20 s into each song, log for ~5 s which game code reads the
    // followed camera's position (x, z) and the renderer's camera position global.
    static unsigned captureStartFrame = 0;
    if (!CameraScan::AccessCaptureActive() && g_framesInSong == 1200 && CameraScan::FollowedViewCamera()) {
        const uintptr_t camera = CameraScan::FollowedViewCamera();
        CameraScan::StartAccessCapture(camera + 0x74, camera + 0x7C, 0xD57A90);
        captureStartFrame = g_frameIndex;
    }
    if (CameraScan::AccessCaptureActive() && g_frameIndex - captureStartFrame > 300) CameraScan::StopAccessCapture();
    // A followed camera the game stopped using for ~4 s (destroyed or replaced): let it go.
    if (CameraScan::FollowedCamera() && g_followMismatchFrames > 360) {
        LogLine("[follow] view %08X no longer matches - looking again", unsigned(CameraScan::FollowedCamera()));
        CameraScan::SetFollowCamera(0, nullptr);
        CameraScan::ForgetCameras();
        g_followMatrixWritten = false;
        g_followMismatchFrames = 0;
    }
}

// Logs how many 3D draws the game sends either side of each camera cut, to show what the cut culls.

static bool g_cameraCutThisFrame = false;
static void LogCameraCuts() {
    g_cameraCutThisFrame = false;
    static Vector3 previousPosition = {};
    static float previousYaw = 0.0f;
    static bool havePrevious = false;
    static unsigned previousDraws = 0;
    static unsigned lastCutFrame = 0;
    Vector3 position;
    float yaw;
    if (!InSong() || !ScriptedCameraPose(position, yaw)) { havePrevious = false; return; }
    if (havePrevious) {
        const float moveX = position.x - previousPosition.x, moveY = position.y - previousPosition.y, moveZ = position.z - previousPosition.z;
        const float moved = sqrtf(moveX * moveX + moveY * moveY + moveZ * moveZ);
        float turned = fabsf(yaw - previousYaw);
        if (turned > 3.14159265f) turned = 6.2831853f - turned;
        if ((moved > 1.5f || turned > 0.5f) && g_frameIndex - lastCutFrame > 10) {
            LogLine("[cut] scripted camera cut (moved %.1f, turned %.0f deg): 3D draws %u -> %u%s", moved, turned * 57.2958f,
                    previousDraws, g_mainDrawsThisFrame, CameraScan::FollowedCamera() ? " (head follow on)" : "");
            lastCutFrame = g_frameIndex;
            g_cameraCutThisFrame = true;
        }
    }
    previousPosition = position;
    previousYaw = yaw;
    previousDraws = g_mainDrawsThisFrame;
    havePrevious = true;
}

// ---------------------------------------------------------------------------
// Remembered standing spots, keyed by the game camera they replace: a song's start camera (one
// venue's songs all start from the same few camera positions) or a 3D menu screen's camera.
// Stored in vr.ini [spots] (a 9th value of 1 marks a menu spot).
// ---------------------------------------------------------------------------
enum SpotKind { SongSpot = 0, MenuSpot = 1 };

struct StandingSpot {
    Vector3 startCamera;
    float   startYaw;
    Vector3 position;
    float   yaw;
    int     kind;
};
static std::vector<StandingSpot> g_spots;
static Vector3 g_spotKeyCamera = {};   // the camera F8 remembers a spot for
static float   g_spotKeyYaw = 0.0f;
static int     g_spotKeyKind = SongSpot;
static bool    g_spotKeyKnown = false;
static bool    g_songStartPending = false;

constexpr float SpotMatchDistance = 3.0f;      // game units (song start cameras)
constexpr float SpotMatchYaw = 0.35f;          // radians (~20 degrees)
constexpr float MenuSpotMatchDistance = 0.5f;  // menu screens' cameras can be close together
constexpr float MenuSpotMatchYaw = 0.17f;      // ~10 degrees

static void LoadSpots() {
    g_spots.clear();
    const int count = GetPrivateProfileIntA("spots", "count", 0, g_iniPath.c_str());
    for (int index = 0; index < count && index < 256; ++index) {
        char key[32], text[256];
        snprintf(key, sizeof(key), "spot%d", index);
        GetPrivateProfileStringA("spots", key, "", text, sizeof(text), g_iniPath.c_str());
        StandingSpot spot;
        spot.kind = SongSpot;
        if (sscanf(text, "%f %f %f %f %f %f %f %f %d", &spot.startCamera.x, &spot.startCamera.y, &spot.startCamera.z, &spot.startYaw,
                   &spot.position.x, &spot.position.y, &spot.position.z, &spot.yaw, &spot.kind) >= 8)
            g_spots.push_back(spot);
    }
}

static void SaveSpots() {
    WritePrivateProfileStringA("spots", nullptr, nullptr, g_iniPath.c_str());   // clear the section
    WritePrivateProfileStringA("spots", "count", std::to_string(g_spots.size()).c_str(), g_iniPath.c_str());
    for (size_t index = 0; index < g_spots.size(); ++index) {
        const StandingSpot& spot = g_spots[index];
        char key[32], text[256];
        snprintf(key, sizeof(key), "spot%u", unsigned(index));
        snprintf(text, sizeof(text), "%.3f %.3f %.3f %.4f %.3f %.3f %.3f %.4f %d", spot.startCamera.x, spot.startCamera.y, spot.startCamera.z,
                 spot.startYaw, spot.position.x, spot.position.y, spot.position.z, spot.yaw, spot.kind);
        WritePrivateProfileStringA("spots", key, text, g_iniPath.c_str());
    }
}

static int FindSpot(const Vector3& startCamera, float startYaw, int kind) {
    int best = -1;
    float bestDistance = kind == MenuSpot ? MenuSpotMatchDistance : SpotMatchDistance;
    const float matchYaw = kind == MenuSpot ? MenuSpotMatchYaw : SpotMatchYaw;
    for (size_t index = 0; index < g_spots.size(); ++index) {
        const StandingSpot& spot = g_spots[index];
        if (spot.kind != kind) continue;
        const float distanceX = spot.startCamera.x - startCamera.x, distanceY = spot.startCamera.y - startCamera.y,
                    distanceZ = spot.startCamera.z - startCamera.z;
        const float distance = sqrtf(distanceX * distanceX + distanceY * distanceY + distanceZ * distanceZ);
        float turned = fabsf(spot.startYaw - startYaw);
        if (turned > 3.14159265f) turned = 6.2831853f - turned;
        if (distance < bestDistance && turned < matchYaw) { best = int(index); bestDistance = distance; }
    }
    return best;
}

// Where you go at a song's start: a remembered spot for its start camera, else where you stood in the
// last song (setlists / restarts), else your band member's place (role).
enum class SongTarget { None, Spot, Carry, Role };
static SongTarget g_songTargetKind = SongTarget::None;
static Vector3    g_songTargetPosition = {};
static float      g_songTargetYaw = 0.0f;
static bool       g_songMovePending = false;   // waiting for the intro to end
static double     g_songMoveSinceMs = 0.0;
static void BeginSongMove();

// Called once the anchor has been taken from the song's first camera.
static void ApplyRememberedSpot() {
    g_spotKeyCamera = g_anchorPosition;
    g_spotKeyYaw = g_anchorYaw;
    g_spotKeyKind = SongSpot;
    g_spotKeyKnown = true;
    const int found = g_settings.savedSpots ? FindSpot(g_spotKeyCamera, g_spotKeyYaw, SongSpot) : -1;
    if (found >= 0) {
        g_songTargetKind = SongTarget::Spot;
        g_songTargetPosition = g_spots[found].position;
        g_songTargetYaw = g_spots[found].yaw;
        LogLine("[spots] remembered spot %d for this start camera (%.1f %.1f %.1f)", found, g_spotKeyCamera.x, g_spotKeyCamera.y, g_spotKeyCamera.z);
    } else if (g_settings.savedSpots && g_songCarryValid) {
        g_songTargetKind = SongTarget::Carry;
        g_songTargetPosition = g_songCarryPosition;
        g_songTargetYaw = g_songCarryYaw;
        LogLine("[spots] next / restarted song: back where you stood in the last song (F8 saves a spot for this start camera %.1f %.1f %.1f)",
                g_spotKeyCamera.x, g_spotKeyCamera.y, g_spotKeyCamera.z);
    } else if (g_settings.autoSpot) {
        g_songTargetKind = SongTarget::Role;   // worked out when you're moved (the band may not be in view yet)
        LogLine("[spots] %s for this start camera (%.1f %.1f %.1f): your band member's place", g_settings.savedSpots ? "no remembered spot" :
                "saved spots off", g_spotKeyCamera.x, g_spotKeyCamera.y, g_spotKeyCamera.z);
    } else {
        g_songTargetKind = SongTarget::None;
        LogLine("[spots] saved spots and auto spot off: staying at the game's camera");
        return;
    }
    g_songMovePending = true;
    g_songMoveSinceMs = NowMs();
    if (g_settings.songIntro == 0) BeginSongMove();
}

static void RememberSpot(bool forget) {
    if (!g_spotKeyKnown) { LogLine("[spots] nothing to remember a spot for yet (start a song or open a 3D menu)"); return; }
    const char* const kindName = g_spotKeyKind == MenuSpot ? "menu" : "start";
    const int found = FindSpot(g_spotKeyCamera, g_spotKeyYaw, g_spotKeyKind);
    if (forget) {
        if (found >= 0) g_spots.erase(g_spots.begin() + found);
        if (found >= 0) LogLine("[spots] forgot the spot for this %s camera", kindName);
        else LogLine("[spots] nothing remembered here");
    } else {
        StandingSpot spot = { g_spotKeyCamera, g_spotKeyYaw, g_anchorPosition, g_anchorYaw, g_spotKeyKind };
        if (found >= 0) g_spots[found] = spot;
        else g_spots.push_back(spot);
        LogLine("[spots] remembered %.1f %.1f %.1f for %s camera %.1f %.1f %.1f", spot.position.x, spot.position.y,
                spot.position.z, kindName, spot.startCamera.x, spot.startCamera.y, spot.startCamera.z);
    }
    SaveSpots();
    MessageBeep(MB_OK);
}

// ---------------------------------------------------------------------------
// 3D menus: when the game moves its menu camera (a submenu in another room), take you there too -
// fading through black once its camera has settled (menu_camera 1), or gliding along with it (2).
// A spot remembered with F8 for that menu camera is used instead of the camera itself.
// ---------------------------------------------------------------------------
constexpr float MenuCameraMovedDistance = 0.5f;   // game units: further than this from where you were put = new spot
constexpr float MenuCameraMovedYaw = 0.26f;       // ~15 degrees
constexpr float MenuCameraStillDistance = 0.01f;  // per frame: below this the camera is "still"
constexpr float MenuCameraStillYaw = 0.004f;
constexpr int   MenuCameraSettleFrames = 12;
constexpr double MenuCameraSettleTimeoutMs = 1500.0;   // a camera that keeps swaying still gets followed
constexpr float FadeSeconds = 0.25f;

struct MenuCameraTracker {
    bool    known = false;            // which game camera you were last put at
    Vector3 position = {};
    float   yaw = 0.0f;
    bool    havePrevious = false;     // the camera last frame
    Vector3 previousPosition = {};
    float   previousYaw = 0.0f;
    int     stillFrames = 0;
    double  movedSinceMs = 0.0;       // when it first left `position` (0: hasn't)
};
static MenuCameraTracker g_menuCamera;

enum class FadePhase { None, Out, In };
static FadePhase g_fadePhase = FadePhase::None;
static Vector3   g_fadeTargetCamera = {};   // the menu camera you're being taken to
static float     g_fadeTargetCameraYaw = 0.0f;

static float AngleBetween(float first, float second) {
    float difference = fmodf(fabsf(first - second), 6.2831853f);
    return difference > 3.14159265f ? 6.2831853f - difference : difference;
}

// Stand at a menu camera (or the spot remembered for it); it becomes what F8 remembers for.
static void MoveToMenuCamera(const Vector3& camera, float yaw) {
    g_menuCamera.known = true;
    g_menuCamera.position = camera;
    g_menuCamera.yaw = yaw;
    g_menuCamera.movedSinceMs = 0.0;
    g_spotKeyCamera = camera;
    g_spotKeyYaw = yaw;
    g_spotKeyKind = MenuSpot;
    g_spotKeyKnown = true;
    const int found = FindSpot(camera, yaw, MenuSpot);
    g_anchorPosition = found >= 0 ? g_spots[found].position : camera;
    g_anchorYaw = found >= 0 ? g_spots[found].yaw : yaw;
    LogLine("[menu camera] you're at %s for menu camera %.2f %.2f %.2f", found >= 0 ? "your remembered spot" : "the game's camera",
            camera.x, camera.y, camera.z);
}

// Band members seen since the song started (they're only drawn while in view).
static std::vector<Vector3> g_stageMembers;

static void NoteStageMembers() {
    for (const CharacterRoot& root : g_characters) {
        bool known = false;
        for (Vector3& member : g_stageMembers)
            if (FlatDistance(member, root.position) < 0.5f) { member = root.position; known = true; break; }
        if (!known && g_stageMembers.size() < 8) g_stageMembers.push_back(root.position);
    }
}

// Your band member's place. GHWT's stage, from the crowd: bass far left, guitar right, vocals front
// centre, drums at the back (the crowd looks along -Z with +X on their right).
static bool RoleSlot(Vector3& position) {
    static const char* const roleNames[] = { "guitarist", "bassist", "drummer", "singer" };
    int role = g_settings.role;
    if (role < 0) {
        role = int(PartScan::CurrentPart());
        if (role < 0) { role = 0; LogLine("[spots] which part you play isn't known yet: guitar"); }
        else LogLine("[spots] the game says you play %s", role == 0 ? "guitar" : role == 1 ? "bass" : role == 2 ? "drums" : "vocals");
    }
    const Vector3* best = nullptr;
    for (const Vector3& member : g_stageMembers) {
        const bool centre = fabsf(member.x) < 1.0f;
        switch (role) {
            case 0: if (!centre && member.x > 0 && (!best || member.x > best->x)) best = &member; break;
            case 1: if (!centre && member.x < 0 && (!best || member.x < best->x)) best = &member; break;
            case 2: if (centre && (!best || member.z < best->z)) best = &member; break;
            default: if (centre && (!best || member.z > best->z)) best = &member; break;
        }
    }
    // The drummer is the centre member furthest back - only if there's a singer in front of them.
    if (role == 2 && best) {
        bool singerInFront = false;
        for (const Vector3& member : g_stageMembers)
            if (fabsf(member.x) < 1.0f && member.z > best->z + 0.5f) singerInFront = true;
        if (!singerInFront) best = nullptr;
    }
    if (best) {
        position = *best;
        LogLine("[spots] the %s stands at %.2f %.2f %.2f", roleNames[role], position.x, position.y, position.z);
        return true;
    }
    // Not seen: GHWT's usual places (the same in every venue seen so far). The drummer's isn't known.
    static const Vector3 usualPlaces[] = { { 2.5f, 0.0f, 2.5f }, { -2.5f, 0.0f, 2.5f }, {}, { 0.0f, 0.0f, 3.0f } };
    if (role == 2) {
        LogLine("[spots] the drummer hasn't been seen: staying at the game's camera (F8 saves a spot)");
        return false;
    }
    position = usualPlaces[role];
    LogLine("[spots] the %s hasn't been seen: their usual place %.2f %.2f %.2f", roleNames[role], position.x, position.y, position.z);
    return true;
}

static bool g_fadeIsSongMove = false;   // the fade under way takes you to your song spot (not a menu camera)
static bool g_fadeIsIntroHop = false;   // ...or to where the intro's camera has got to
static double g_lastIntroHopMs = 0.0;

// Fade (through black) to your song spot.
static void BeginSongMove() {
    if (!g_songMovePending) return;
    g_songMovePending = false;
    g_fadeIsSongMove = true;
    g_fadeIsIntroHop = false;
    g_fadePhase = FadePhase::Out;
}

// At the fade's black: where to stand.
static void ArriveAtSongSpot() {
    if (g_songTargetKind == SongTarget::Role) {
        Vector3 slot;
        if (!RoleSlot(slot)) return;
        // At their place, at your eye height above their feet, facing the crowd (+Z).
        g_songTargetPosition = { slot.x, slot.y + g_settings.avatarEyeHeight * g_settings.worldScale, slot.z };
        g_songTargetYaw = 3.14159265f;
    }
    if (g_songTargetKind == SongTarget::None) return;
    g_anchorPosition = g_songTargetPosition;
    g_anchorYaw = g_songTargetYaw;
    if (!g_choiceIsManual) {
        g_chosenCharacterLocked = false;
        g_pickNearPending = true;
        g_pickNearPosition = g_anchorPosition;
    }
    LogLine("[spots] moved to %s", g_songTargetKind == SongTarget::Spot ? "your remembered spot" :
                                   g_songTargetKind == SongTarget::Carry ? "where you stood in the last song" : "your band member's place");
}

// Once a frame in songs: the intro is over at its first camera cut (or after a while).
constexpr double SongIntroLongestMs = 15000.0;
static void UpdateSongMove(bool cameraCut) {
    if (!g_songMovePending) return;
    if (!InSong()) { g_songMovePending = false; return; }
    if (cameraCut) LogLine("[spots] the intro's camera cut: moving you");
    else if (NowMs() - g_songMoveSinceMs > SongIntroLongestMs) LogLine("[spots] no camera cut in %.0f s: moving you", SongIntroLongestMs / 1000.0);
    else {
        // Long fly-ins: every few seconds, fade to where the intro's camera has got to.
        Vector3 camera;
        float cameraYaw;
        const double sinceHop = NowMs() - std::max(g_lastIntroHopMs, g_songMoveSinceMs);
        if (g_settings.introHopSeconds > 0.0f && sinceHop > g_settings.introHopSeconds * 1000.0 && g_fadePhase == FadePhase::None &&
            ScriptedCameraPose(camera, cameraYaw) &&
            sqrtf((camera.x - g_anchorPosition.x) * (camera.x - g_anchorPosition.x) + (camera.y - g_anchorPosition.y) * (camera.y - g_anchorPosition.y) +
                  (camera.z - g_anchorPosition.z) * (camera.z - g_anchorPosition.z)) > 1.0f) {
            g_lastIntroHopMs = NowMs();
            g_fadeIsIntroHop = true;
            g_fadePhase = FadePhase::Out;
        }
        return;
    }
    BeginSongMove();
}

static void AdvanceFade() {
    static double previousMs = 0.0;
    const double nowMs = NowMs();
    const float seconds = previousMs > 0.0 ? float(std::min(0.1, (nowMs - previousMs) / 1000.0)) : 0.0f;
    previousMs = nowMs;
    const float step = seconds / FadeSeconds;
    if (g_fadePhase == FadePhase::Out) {
        g_fadeAmount = std::min(1.0f, g_fadeAmount + step);
        // Fully black (and shown black for a frame): move, then fade back in.
        if (g_fadeAmount >= 1.0f) {
            Vector3 camera;
            float cameraYaw;
            if (g_fadeIsSongMove) ArriveAtSongSpot();
            else if (g_fadeIsIntroHop) {
                if (ScriptedCameraPose(camera, cameraYaw)) { g_anchorPosition = camera; g_anchorYaw = cameraYaw; }
            } else MoveToMenuCamera(g_fadeTargetCamera, g_fadeTargetCameraYaw);
            g_fadeIsSongMove = false;
            g_fadeIsIntroHop = false;
            g_fadePhase = FadePhase::In;
        }
    } else if (g_fadePhase == FadePhase::In) {
        g_fadeAmount = std::max(0.0f, g_fadeAmount - step);
        if (g_fadeAmount <= 0.0f) g_fadePhase = FadePhase::None;
    } else {
        g_fadeAmount = 0.0f;
    }
}

// Once per frame (Present), while this frame's game camera is still known.
static void UpdateMenuCamera() {
    AdvanceFade();
    const bool inVrMenu = !InSong() && g_vrCameraUsedThisFrame && g_gameViewProjKnownThisFrame;
    if (!inVrMenu || g_anchorFromCameraRequested) {
        // Not in a 3D menu, or about to stand at its camera anyway: start over when back.
        if (!inVrMenu) { g_menuCamera.known = false; g_menuCamera.havePrevious = false; }
        if (g_fadePhase == FadePhase::Out && !g_fadeIsSongMove && !g_fadeIsIntroHop) g_fadePhase = FadePhase::In;
        return;
    }
    Vector3 camera;
    if (!CameraPositionFromViewProj(g_gameViewProj, camera)) return;
    const float yaw = CameraYawFromViewProj(g_gameViewProj);

    // First frame in this menu (you were just put at its camera): use a remembered spot if there is one.
    if (!g_menuCamera.known) {
        MoveToMenuCamera(camera, yaw);
        g_menuCamera.havePrevious = false;
    }

    const bool still = g_menuCamera.havePrevious && FlatDistance(camera, g_menuCamera.previousPosition) < MenuCameraStillDistance &&
                       fabsf(camera.y - g_menuCamera.previousPosition.y) < MenuCameraStillDistance &&
                       AngleBetween(yaw, g_menuCamera.previousYaw) < MenuCameraStillYaw;
    g_menuCamera.stillFrames = still ? g_menuCamera.stillFrames + 1 : 0;

    if (g_settings.menuCamera == 2 && g_menuCamera.havePrevious) {
        // Glide: carry you along with the camera's movement (keeping where you are relative to it).
        const float turn = yaw - g_menuCamera.previousYaw;
        const float offsetX = g_anchorPosition.x - g_menuCamera.previousPosition.x, offsetZ = g_anchorPosition.z - g_menuCamera.previousPosition.z;
        const float cosine = cosf(turn), sine = sinf(turn);
        g_anchorPosition.x = camera.x + offsetX * cosine + offsetZ * sine;
        g_anchorPosition.z = camera.z - offsetX * sine + offsetZ * cosine;
        g_anchorPosition.y += camera.y - g_menuCamera.previousPosition.y;
        g_anchorYaw += turn;
    } else if (g_settings.menuCamera == 1 && g_fadePhase != FadePhase::Out) {
        const bool moved = FlatDistance(camera, g_menuCamera.position) > MenuCameraMovedDistance ||
                           fabsf(camera.y - g_menuCamera.position.y) > MenuCameraMovedDistance ||
                           AngleBetween(yaw, g_menuCamera.yaw) > MenuCameraMovedYaw;
        if (!moved) {
            g_menuCamera.movedSinceMs = 0.0;
        } else {
            if (g_menuCamera.movedSinceMs == 0.0) g_menuCamera.movedSinceMs = NowMs();
            const bool settled = g_menuCamera.stillFrames >= MenuCameraSettleFrames ||
                                 NowMs() - g_menuCamera.movedSinceMs > MenuCameraSettleTimeoutMs;
            if (settled) {
                g_fadeTargetCamera = camera;
                g_fadeTargetCameraYaw = yaw;
                g_fadePhase = FadePhase::Out;
                LogLine("[menu camera] game camera moved to %.2f %.2f %.2f (%.1f from you): fading there", camera.x, camera.y, camera.z,
                        FlatDistance(camera, g_menuCamera.position));
            }
        }
    }
    g_menuCamera.previousPosition = camera;
    g_menuCamera.previousYaw = yaw;
    g_menuCamera.havePrevious = true;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destRect, HWND overrideWindow, const RGNDATA* dirtyRegion) {
    RestoreAllOverrides(device);
    static double previousPresentMs = 0;
    const double presentStartMs = NowMs();
    if (previousPresentMs > 0) AddTiming(presentStartMs - previousPresentMs, g_counters.totalFrameMs, g_counters.worstFrameMs);
    previousPresentMs = presentStartMs;

    {
        ModWorkScope modWork;
        SubmitFrameToHeadset(device);
        AddTiming(NowMs() - presentStartMs, g_counters.totalSubmitMs, g_counters.worstSubmitMs);
        ShowLeftEyeOnDesktop(device);
        if (g_settings.mirrorHud == 1) DrawOverlayOnDesktop(device);
    }

    HRESULT result = g_originalPresent(device, sourceRect, destRect, overrideWindow, dirtyRegion);

    if (g_gameViewProjKnownThisFrame && !CameraScan::IsWidening() && !g_cameraHooksArmed)
        CameraScan::OnFrame(GameProjectionScale(0), GameProjectionScale(1));
    UpdateMenuVrAllowed();
    UpdateCameraHooks();
    LogCameraCuts();
    CameraScan::Tick();

    // Song tracking: recentre automatically when a song's 3D scene starts.
    // The quickplay difficulty screen also draws a highway, over a tiny 3D scene: a real song has
    // a venue behind it (hundreds of 3D draws), so that's required too.
    // A highway over a venue, or notes coming down it (some venues are just a backdrop). Only needed to enter: close-ups draw less.
    const bool songFrame = g_highwayDrawnThisFrame && (InSong() || g_mainDrawsThisFrame >= SongMinimumSceneDraws || g_gemDrawnThisFrame);
    g_gemDrawnThisFrame = false;
    g_framesSinceHighway = songFrame ? 0 : (g_framesSinceHighway == ~0u ? ~0u : g_framesSinceHighway + 1);
    g_framesSinceAnyHighway = g_highwayDrawnThisFrame ? 0 : (g_framesSinceAnyHighway == ~0u ? ~0u : g_framesSinceAnyHighway + 1);
    if (!InSong() && g_wasInSong) {
        g_anchorFromCameraRequested = true;   // back to the menu's camera
        g_songCarryPosition = g_anchorPosition;
        g_songCarryYaw = g_anchorYaw;
        g_songCarryValid = true;
        g_songCarryFlatMs = 0.0;
    }
    // Restarts (pause menu or after failing) can load fast enough that the song never looks over:
    // the highway goes for a few frames while a loading screen (next to no 3D) is up, then comes back.
    static unsigned highwayGapFrames = 0, loadingFrames = 0;
    if (InSong() && g_wasInSong) {
        if (!g_highwayDrawnThisFrame) {
            ++highwayGapFrames;
            if (g_mainDrawsThisFrame < SongMinimumSceneDraws / 4) ++loadingFrames;
        } else {
            if (highwayGapFrames >= 8 && loadingFrames >= 3) {
                LogLine("[song] restarted (highway gone %u frames, %u of them loading) - back to your spot", highwayGapFrames, loadingFrames);
                g_stageMembers.clear();
                g_songCarryPosition = g_anchorPosition;
                g_songCarryYaw = g_anchorYaw;
                g_songCarryValid = true;
                g_songCarryFlatMs = 0.0;
                RequestFullRecenter();
                g_cameraDumpsThisSong = 0;
                g_songStartPending = true;
            }
            highwayGapFrames = loadingFrames = 0;
        }
    } else {
        highwayGapFrames = loadingFrames = 0;
    }
    if (InSong() && !g_wasInSong) {
        LogLine("[song] started - recentring");
        PartScan::Find();   // which part you play (found once per launch, in the background)
        g_stageMembers.clear();
        RequestFullRecenter();
        g_cameraDumpsThisSong = 0;
        g_songStartPending = true;
    }
    g_highwayDrawnThisFrame = false;
    g_wasInSong = InSong();
    g_framesInSong = InSong() ? g_framesInSong + 1 : 0;
    if (g_songStartPending && g_anchorValid && !g_anchorFromCameraRequested) {
        g_songStartPending = false;
        ApplyRememberedSpot();
    }
    UpdateChosenCharacter();
    if (InSong()) NoteStageMembers();
    UpdateSongMove(g_cameraCutThisFrame);
    if (g_nearbyDiagnosticFrames > 0) --g_nearbyDiagnosticFrames;
    g_logNearbyThisFrame = g_settings.debugTools && g_chosenCharacterLocked && InSong() && g_settings.bandMemberMode != LeaveBandMember && (g_frameIndex + 1) % 600 == 0;
    if (g_logNearbyThisFrame) { g_nearbyLines = 0; LogLine("[band near] next frame, draws near your band member that aren't drawn from their spot:"); }
    if (!InSong()) { g_chosenCharacterLocked = false; g_choiceIsManual = false; g_pickNearPending = false; }
    // A 3D menu just came into VR: stand where its camera is.
    if (g_vrCameraUsedThisFrame && !g_previousFrameUsedVrCamera && !InSong()) g_anchorFromCameraRequested = true;
    UpdateMenuCamera();
    if (g_frameIndex % 60 == 0 && ReloadSettingsIfChanged()) LoadSpots();
    UpdateDrawFinder();
    UpdateRoleProbe();
    PartScan::Tick();

    // Diagnostics, once per song at ~30 s: one frame's draw list (band member) and view setup
    // order + callers (render loop, for drawing both eyes every frame).
    if (g_drawListActive) {
        g_drawListActive = false;
        CameraScan::LogViewTrace(g_allDrawsThisFrame);
        LogLine("[draw list] end (head at %.2f %.2f %.2f)", g_vrCameraToWorld.values[3][0], g_vrCameraToWorld.values[3][1], g_vrCameraToWorld.values[3][2]);
    }
    if (g_settings.debugTools && g_framesInSong == 1800 && g_vrCameraUsedThisFrame) {
        g_drawListActive = true;
        g_drawListLines = 0;
        CameraScan::SetDrawCounter(&g_allDrawsThisFrame);
        CameraScan::StartViewTrace();
        LogLine("[draw list] next frame's draws (stand inside your band member for this); say which instrument you played:");
    }
    g_allDrawsThisFrame = 0;

    // Next frame
    UpdateRenderTargetStats(g_frameIndex, LogLine);
    StereoTargets::ReleaseIdle(g_frameIndex);
    MeshWatch::SetFrame(g_frameIndex + 1);
    IDirect3DSurface9* backBuffer = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
        g_backBufferSurface = backBuffer;   // pointer kept only for comparison
        backBuffer->Release();
    }
    g_previousFrameUsedVrCamera = g_vrCameraUsedThisFrame;
    ++g_frameIndex;
    g_gameViewProjKnownThisFrame = false;
    g_gameCameraAxesKnownThisFrame = false;
    g_mainDrawsThisFrame = 0;
    g_vrMatricesValid = false;
    g_vrCameraUsedThisFrame = false;
    g_overlayClearedThisFrame = false;
    g_overlayDrawnThisFrame = false;
    for (int layer = 0; layer < HudLayerCount; ++layer) g_hudLayerCleared[layer] = g_hudLayerDrawn[layer] = false;
    {
        // A third of the highway drawn over after the gems: a menu on top (pause). Gems go flat
        // until it's been gone for a few frames.
        constexpr float CoveredFraction = 0.33f;
        constexpr unsigned CoveredHoldFrames = 5;
        const bool covered = g_gemAreaCoveredThisFrame > CoveredFraction;
        if (covered && g_gemsCoveredFrames == 0)
            LogLine("[gems] %.0f%% of the highway drawn over (a menu?): gems flat while it's up", g_gemAreaCoveredThisFrame * 100.0f);
        if (!covered && g_gemsCoveredFrames == 1) LogLine("[gems] highway uncovered: gems 3D again");
        g_gemsCoveredFrames = covered ? CoveredHoldFrames : (g_gemsCoveredFrames ? g_gemsCoveredFrames - 1 : 0);
        g_gemAreaCoveredThisFrame = 0.0f;
        if (g_highwaysThisFrame > 1 && g_highwaysLastFrame <= 1) LogLine("[gems] %u highways (multiplayer): gems flat", g_highwaysThisFrame);
        g_highwaysLastFrame = g_highwaysThisFrame;
        g_highwaysThisFrame = 0;
    }
    g_logHudDrawsThisFrame = g_settings.debugTools && InSong() && g_framesInSong == 900;
    if (g_logHudDrawsThisFrame) { g_hudDrawLines = 0; LogLine("[hud] next frame's HUD draws (3D HUD sorting):"); }
    HandleHotkeys();
    LogStatusIfDue();
    const double xrStartMs = NowMs();
    StartNextHeadsetFrame();
    AddTiming(NowMs() - xrStartMs, g_counters.totalXrWaitMs, g_counters.worstXrWaitMs);
    CameraScan::SetFollowFov(g_settings.cullingFov);
    CameraScan::SetCrowdFollow(g_settings.crowdFollow);
    if (g_cameraHooksArmed && g_settings.followHead) WriteHeadIntoGameCamera();
    return result;
}

// Full stereo: the backbuffer and automatic depth buffer are the game's too (made by the device).
static void NoteDeviceTargets(IDirect3DDevice9* device) {
    StereoTargets::SetScreenSize(g_backBufferWidth, g_backBufferHeight);
    for (IDirect3DSurface9*& target : g_boundTargets) target = nullptr;
    for (IDirect3DBaseTexture9*& texture : g_boundTextures) texture = nullptr;
    g_boundDepth = nullptr;
    IDirect3DSurface9* surface = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface) {
        StereoTargets::NoteSurface(surface);
        g_boundTargets[0] = surface;   // pointer kept for comparison; the device holds it
        surface->Release();
    }
    surface = nullptr;
    if (SUCCEEDED(device->GetDepthStencilSurface(&surface)) && surface) {
        StereoTargets::NoteSurface(surface);
        g_boundDepth = surface;
        surface->Release();
    }
}

static void ReadBackBufferInfo(D3DPRESENT_PARAMETERS* presentParameters) {
    g_backBufferWidth = presentParameters->BackBufferWidth;
    g_backBufferHeight = presentParameters->BackBufferHeight;
    g_renderTargetWidth = g_backBufferWidth;
    g_renderTargetHeight = g_backBufferHeight;
    LogLine("[device] backbuffer %ux%u windowed %d", g_backBufferWidth, g_backBufferHeight, presentParameters->Windowed);
}

// The headset sets the pace (xrWaitFrame); desktop vsync on top of that would halve the frame rate.
static void DisableDesktopVsync(D3DPRESENT_PARAMETERS* presentParameters) {
    if (presentParameters) presentParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

static HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentParameters) {
    ReleaseDeviceResources();
    StereoTargets::ReleaseAll();
    g_modifiedOnDevice.clear();
    DisableDesktopVsync(presentParameters);
    HRESULT result = g_originalReset(device, presentParameters);
    LogLine("[device] Reset -> %08lX", result);
    if (SUCCEEDED(result) && presentParameters) ReadBackBufferInfo(presentParameters);
    if (SUCCEEDED(result)) NoteDeviceTargets(device);
    return result;
}

static void HookDevice(IDirect3DDevice9* device) {
    PatchVtableSlot(device, Slot::Reset,                    (void*)HookedReset,                    (void**)&g_originalReset);
    PatchVtableSlot(device, Slot::Present,                  (void*)HookedPresent,                  (void**)&g_originalPresent);
    PatchVtableSlot(device, Slot::SetRenderTarget,          (void*)HookedSetRenderTarget,          (void**)&g_originalSetRenderTarget);
    PatchVtableSlot(device, Slot::SetDepthStencilSurface,   (void*)HookedSetDepthStencilSurface,   (void**)&g_originalSetDepthStencilSurface);
    PatchVtableSlot(device, Slot::SetTexture,               (void*)HookedSetTexture,               (void**)&g_originalSetTexture);
    PatchVtableSlot(device, Slot::CreateTexture,            (void*)HookedCreateTexture,            (void**)&g_originalCreateTexture);
    PatchVtableSlot(device, Slot::CreateVertexBuffer,       (void*)HookedCreateVertexBuffer,       (void**)&g_originalCreateVertexBuffer);
    PatchVtableSlot(device, Slot::CreateRenderTarget,       (void*)HookedCreateRenderTarget,       (void**)&g_originalCreateRenderTarget);
    PatchVtableSlot(device, Slot::CreateDepthStencilSurface, (void*)HookedCreateDepthStencilSurface, (void**)&g_originalCreateDepthStencilSurface);
    PatchVtableSlot(device, Slot::StretchRect,              (void*)HookedStretchRect,              (void**)&g_originalStretchRect);
    PatchVtableSlot(device, Slot::ColorFill,                (void*)HookedColorFill,                (void**)&g_originalColorFill);
    PatchVtableSlot(device, Slot::Clear,                    (void*)HookedClear,                    (void**)&g_originalClear);
    PatchVtableSlot(device, Slot::DrawPrimitive,            (void*)HookedDrawPrimitive,            (void**)&g_originalDrawPrimitive);
    PatchVtableSlot(device, Slot::DrawIndexedPrimitive,     (void*)HookedDrawIndexedPrimitive,     (void**)&g_originalDrawIndexedPrimitive);
    PatchVtableSlot(device, Slot::DrawPrimitiveUP,          (void*)HookedDrawPrimitiveUP,          (void**)&g_originalDrawPrimitiveUP);
    PatchVtableSlot(device, Slot::DrawIndexedPrimitiveUP,   (void*)HookedDrawIndexedPrimitiveUP,   (void**)&g_originalDrawIndexedPrimitiveUP);
    PatchVtableSlot(device, Slot::CreateVertexShader,       (void*)HookedCreateVertexShader,       (void**)&g_originalCreateVertexShader);
    PatchVtableSlot(device, Slot::SetVertexShader,          (void*)HookedSetVertexShader,          (void**)&g_originalSetVertexShader);
    PatchVtableSlot(device, Slot::SetVertexShaderConstantF, (void*)HookedSetVertexShaderConstantF, (void**)&g_originalSetVertexShaderConstantF);
    PatchVtableSlot(device, Slot::CreatePixelShader,        (void*)HookedCreatePixelShader,        (void**)&g_originalCreatePixelShader);
    Relief::Initialise(g_outputDir, g_originalCreatePixelShader);
    PatchVtableSlot(device, Slot::SetPixelShader,           (void*)HookedSetPixelShader,           (void**)&g_originalSetPixelShader);
    PatchVtableSlot(device, Slot::SetPixelShaderConstantF,  (void*)HookedSetPixelShaderConstantF,  (void**)&g_originalSetPixelShaderConstantF);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* presentParameters, IDirect3DDevice9** deviceOut) {
    DisableDesktopVsync(presentParameters);
    HRESULT result = g_originalCreateDevice(direct3d, adapter, deviceType, focusWindow, behaviorFlags, presentParameters, deviceOut);
    LogLine("[device] CreateDevice -> %08lX flags %08lX", result, behaviorFlags);
    if (SUCCEEDED(result) && deviceOut && *deviceOut) {
        if (presentParameters) ReadBackBufferInfo(presentParameters);
        HookDevice(*deviceOut);
        NoteDeviceTargets(*deviceOut);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Loader / exports
// ---------------------------------------------------------------------------
// If the game crashes, say where (module + offset) in the log before Windows takes over.
static LPTOP_LEVEL_EXCEPTION_FILTER g_previousCrashFilter = nullptr;

static LONG WINAPI LogCrash(EXCEPTION_POINTERS* exceptionInfo) {
    const EXCEPTION_RECORD* record = exceptionInfo->ExceptionRecord;
    const uintptr_t address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    HMODULE module = nullptr;
    char moduleName[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module) {
        GetModuleFileNameA(module, moduleName, MAX_PATH);
    }
    const char* shortName = strrchr(moduleName, '\\');
    LogLine("[crash] exception %08lX at %08X (%s+0x%X)%s", record->ExceptionCode, unsigned(address), shortName ? shortName + 1 : moduleName,
            unsigned(address - reinterpret_cast<uintptr_t>(module)),
            record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? (record->ExceptionInformation[0] ? " writing" : " reading") : "");
    return g_previousCrashFilter ? g_previousCrashFilter(exceptionInfo) : EXCEPTION_CONTINUE_SEARCH;
}

// -novr on the game's command line (e.g. Steam launch options): the mod hooks nothing and the game runs
// exactly as it would without it - no VR runtime started, no hotkeys.
static bool g_vrDisabled = false;

static bool CommandLineHas(const char* argument) {
    std::string commandLine = GetCommandLineA();
    for (char& character : commandLine) character = char(tolower(static_cast<unsigned char>(character)));
    size_t start = 0;
    while ((start = commandLine.find(argument, start)) != std::string::npos) {
        const size_t end = start + strlen(argument);
        const bool startsWord = start == 0 || commandLine[start - 1] == ' ' || commandLine[start - 1] == '"';
        const bool endsWord = end == commandLine.size() || commandLine[end] == ' ' || commandLine[end] == '"';
        if (startsWord && endsWord) return true;
        start = end;
    }
    return false;
}

static void EnsureInitialized() {
    if (g_realD3D9) return;

    char modulePath[MAX_PATH];
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    std::string gameDir(modulePath);
    gameDir = gameDir.substr(0, gameDir.find_last_of("\\/"));

    g_outputDir = gameDir + "\\ghwt_vr";
    g_iniPath = g_outputDir + "\\vr.ini";
    g_loaderPath = gameDir + "\\openxr_loader.dll";
    CreateDirectoryA(g_outputDir.c_str(), nullptr);
    OpenLog(g_outputDir);
    g_vrDisabled = CommandLineHas("-novr");
    if (g_vrDisabled) {
        LogLine("WT:VR %s loaded - VR off (-novr on the command line): the game runs as normal", WTVR_VERSION);
        char systemDirectory[MAX_PATH];
        GetSystemDirectoryA(systemDirectory, MAX_PATH);
        g_realD3D9 = LoadLibraryA((std::string(systemDirectory) + "\\d3d9.dll").c_str());
        if (!g_realD3D9) LogLine("FAILED to load real d3d9.dll");
        return;
    }
    LoadSettings();
    LoadSpots();
    g_previousCrashFilter = SetUnhandledExceptionFilter(LogCrash);
    StereoTargets::Initialise(LogLine);
    MeshWatch::Initialise(LogLine);
    PartScan::Initialise(LogLine);
    LogLine("WT:VR %s loaded", WTVR_VERSION);
    LogSettings();

    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    g_realD3D9 = LoadLibraryA((std::string(systemDir) + "\\d3d9.dll").c_str());
    if (!g_realD3D9) LogLine("FAILED to load real d3d9.dll");
}

template <typename FunctionType>
static FunctionType RealExport(const char* name) {
    EnsureInitialized();
    return g_realD3D9 ? reinterpret_cast<FunctionType>(GetProcAddress(g_realD3D9, name)) : nullptr;
}

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    static auto realCreate = RealExport<Direct3DCreate9Fn>("Direct3DCreate9");
    IDirect3D9* direct3d = realCreate ? realCreate(sdkVersion) : nullptr;
    if (direct3d && !g_vrDisabled) PatchVtableSlot(direct3d, Slot::CreateDevice, (void*)HookedCreateDevice, (void**)&g_originalCreateDevice);
    return direct3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** direct3dOut) {
    using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
    static auto realCreateEx = RealExport<Direct3DCreate9ExFn>("Direct3DCreate9Ex");
    return realCreateEx ? realCreateEx(sdkVersion, direct3dOut) : E_FAIL;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    using Fn = int (WINAPI*)(D3DCOLOR, LPCWSTR);
    static auto real = RealExport<Fn>("D3DPERF_BeginEvent");
    return real ? real(color, name) : 0;
}

int WINAPI D3DPERF_EndEvent() {
    using Fn = int (WINAPI*)();
    static auto real = RealExport<Fn>("D3DPERF_EndEvent");
    return real ? real() : 0;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    using Fn = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    static auto real = RealExport<Fn>("D3DPERF_SetMarker");
    if (real) real(color, name);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    using Fn = void (WINAPI*)(D3DCOLOR, LPCWSTR);
    static auto real = RealExport<Fn>("D3DPERF_SetRegion");
    if (real) real(color, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    using Fn = BOOL (WINAPI*)();
    static auto real = RealExport<Fn>("D3DPERF_QueryRepeatFrame");
    return real ? real() : FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD options) {
    using Fn = void (WINAPI*)(DWORD);
    static auto real = RealExport<Fn>("D3DPERF_SetOptions");
    if (real) real(options);
}

DWORD WINAPI D3DPERF_GetStatus() {
    using Fn = DWORD (WINAPI*)();
    static auto real = RealExport<Fn>("D3DPERF_GetStatus");
    return real ? real() : 0;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        CloseLog();
    }
    return TRUE;
}
