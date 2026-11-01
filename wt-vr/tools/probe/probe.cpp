// GHWT VR probe - stage 1
// d3d9.dll proxy that:
//   - logs every vertex shader with its named constants (from the shader's CTAB)
//   - lets you hide one vertex shader at a time to find what draws what
//   - captures one frame of draws + constant values to a text file
//
// Hotkeys (in game):
//   F8  toggle hunting mode (selected shader is hidden while on)
//   F5  previous shader      F6  next shader
//   F7  mark selected shader in the log (note what disappeared)
//   F9  reset the shader list to what is on screen right now
//   F10 capture the next frame to ghwt_vr\capture_N.txt
//
// Output goes to a "ghwt_vr" folder next to the game exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

#ifdef PROBE_PASSTHROUGH
#define PROBE_VARIANT " passthrough - no hooks"
#else
#define PROBE_VARIANT ""
#endif

// ---------------------------------------------------------------------------
// Vtable slot indices (verified against mingw d3d9.h)
// ---------------------------------------------------------------------------
namespace Slot {
    constexpr int CreateDevice             = 16;
    constexpr int Present                  = 17;
    constexpr int SetRenderTarget          = 37;
    constexpr int SetTransform             = 44;
    constexpr int SetViewport              = 47;
    constexpr int DrawPrimitive            = 81;
    constexpr int DrawIndexedPrimitive     = 82;
    constexpr int DrawPrimitiveUP          = 83;
    constexpr int DrawIndexedPrimitiveUP   = 84;
    constexpr int CreateVertexShader       = 91;
    constexpr int SetVertexShader          = 92;
    constexpr int SetVertexShaderConstantF = 94;
}

// ---------------------------------------------------------------------------
// Shader constant table (CTAB) layout
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
    WORD  registerSet;     // 0 bool, 1 int4, 2 float4, 3 sampler
    WORD  registerIndex;
    WORD  registerCount;
    WORD  reserved;
    DWORD typeInfoOffset;
    DWORD defaultValueOffset;
};

constexpr WORD  RegisterSetFloat4 = 2;
constexpr DWORD CtabFourCC        = 0x42415443; // 'CTAB'
constexpr DWORD EndToken          = 0x0000FFFF;
constexpr int   MaxFloatConstants = 256;

struct NamedConstant {
    std::string name;
    int registerIndex;
    int registerCount;
};

struct ShaderInfo {
    uint32_t hash = 0;
    size_t   byteSize = 0;
    std::vector<NamedConstant> floatConstants;
};

// Bytecode kept in memory; only written to disk for marked / captured shaders.
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_shaderBytecode;
static std::unordered_set<uint32_t> g_shadersWrittenToDisk;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
using CreateDeviceFn             = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PresentFn                  = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetRenderTargetFn          = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetTransformFn             = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetViewportFn              = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using DrawPrimitiveFn            = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn     = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn          = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using CreateVertexShaderFn       = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVertexShaderFn          = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

static HMODULE g_realD3D9 = nullptr;

static CreateDeviceFn             g_originalCreateDevice = nullptr;
static PresentFn                  g_originalPresent = nullptr;
static SetRenderTargetFn          g_originalSetRenderTarget = nullptr;
static SetTransformFn             g_originalSetTransform = nullptr;
static SetViewportFn              g_originalSetViewport = nullptr;
static DrawPrimitiveFn            g_originalDrawPrimitive = nullptr;
static DrawIndexedPrimitiveFn     g_originalDrawIndexedPrimitive = nullptr;
static DrawPrimitiveUPFn          g_originalDrawPrimitiveUP = nullptr;
static DrawIndexedPrimitiveUPFn   g_originalDrawIndexedPrimitiveUP = nullptr;
static CreateVertexShaderFn       g_originalCreateVertexShader = nullptr;
static SetVertexShaderFn          g_originalSetVertexShader = nullptr;
static SetVertexShaderConstantFFn g_originalSetVertexShaderConstantF = nullptr;

static std::string g_outputDir;
static FILE*       g_mainLog = nullptr;
static CRITICAL_SECTION g_lock;

static std::unordered_map<IDirect3DVertexShader9*, ShaderInfo> g_shaders;
static IDirect3DVertexShader9* g_currentShader = nullptr;
static float g_shadowConstants[MaxFloatConstants][4] = {};

// Hunting
static bool g_huntingEnabled = false;
static uint32_t g_selectedHash = 0;
static std::vector<uint32_t> g_shadersThisFrame;
static std::vector<uint32_t> g_shadersLastFrame;
static HWND g_gameWindow = nullptr;

// Window title is set from a separate thread: SetWindowText sends a message to the
// window's thread, and calling it from the render thread can deadlock the game.
static char   g_pendingTitle[256] = {};
static HANDLE g_titleEvent = nullptr;

static int g_framesSinceFlush = 0;
static int g_shaderCreateCount = 0;

// Capture
static bool  g_capturePending = false;
static FILE* g_captureFile = nullptr;
static int   g_captureNumber = 0;
static int   g_drawIndex = 0;
static UINT  g_renderTargetWidth = 0;
static UINT  g_renderTargetHeight = 0;
static D3DVIEWPORT9 g_viewport = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void LogLine(const char* format, ...) {
    if (!g_mainLog) return;
    va_list args;
    va_start(args, format);
    vfprintf(g_mainLog, format, args);
    va_end(args);
    fputc('\n', g_mainLog);
    // flushed periodically from Present, not per line
}

static void FlushLog() {
    if (g_mainLog) fflush(g_mainLog);
}

// Startup diagnostics: written and flushed immediately so the log shows how far it got.
static void Breadcrumb(const char* message) {
    LogLine("[startup] %s  (thread %lu, tick %lu)", message, GetCurrentThreadId(), GetTickCount());
    FlushLog();
}

#define BREADCRUMB_ONCE(message) do { static bool logged = false; if (!logged) { logged = true; Breadcrumb(message); } } while (0)

static void WriteShaderBinary(uint32_t hash) {
    if (g_shadersWrittenToDisk.count(hash)) return;
    auto found = g_shaderBytecode.find(hash);
    if (found == g_shaderBytecode.end()) return;
    char binaryPath[MAX_PATH];
    snprintf(binaryPath, sizeof(binaryPath), "%s\\shaders\\VS_%08X.bin", g_outputDir.c_str(), hash);
    if (FILE* binaryFile = fopen(binaryPath, "wb")) {
        fwrite(found->second.data(), 1, found->second.size(), binaryFile);
        fclose(binaryFile);
        g_shadersWrittenToDisk.insert(hash);
    }
}

static DWORD WINAPI TitleThread(LPVOID) {
    char title[256];
    while (WaitForSingleObject(g_titleEvent, INFINITE) == WAIT_OBJECT_0) {
        EnterCriticalSection(&g_lock);
        memcpy(title, g_pendingTitle, sizeof(title));
        HWND window = g_gameWindow;
        LeaveCriticalSection(&g_lock);
        if (window) SetWindowTextA(window, title);
    }
    return 0;
}

static void CaptureLine(const char* format, ...) {
    if (!g_captureFile) return;
    va_list args;
    va_start(args, format);
    vfprintf(g_captureFile, format, args);
    va_end(args);
    fputc('\n', g_captureFile);
}

// Each slot is hooked exactly once. Overlays (Steam, Discord, RTSS, GPU overlays) hook the
// same vtable slots after us; re-patching would save their hook as our "original" while
// theirs points back at us -> infinite recursion. Once we're in the chain, leave it alone.
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

static uint32_t HashBytes(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t byteIndex = 0; byteIndex < length; ++byteIndex) {
        hash ^= bytes[byteIndex];
        hash *= 16777619u;
    }
    return hash;
}

// Walks shader bytecode tokens to find total size; parses CTAB along the way.
static ShaderInfo ParseShader(const DWORD* tokens) {
    ShaderInfo info;
    const DWORD versionToken = tokens[0];
    const int majorVersion = (versionToken >> 8) & 0xFF;
    const size_t tokenLimit = 1 << 16;

    size_t position = 1;
    while (position < tokenLimit && tokens[position] != EndToken) {
        const DWORD token = tokens[position];

        if ((token & 0xFFFF) == 0xFFFE) { // comment block
            const DWORD commentLength = (token >> 16) & 0x7FFF;
            const DWORD* commentData = &tokens[position + 1];

            if (commentLength >= 2 && commentData[0] == CtabFourCC) {
                const char* tableBase = reinterpret_cast<const char*>(&commentData[1]);
                const auto* header = reinterpret_cast<const ConstantTableHeader*>(tableBase);
                const auto* entries = reinterpret_cast<const ConstantTableEntry*>(tableBase + header->constantInfoOffset);

                for (DWORD entryIndex = 0; entryIndex < header->constantCount; ++entryIndex) {
                    const ConstantTableEntry& entry = entries[entryIndex];
                    if (entry.registerSet != RegisterSetFloat4) continue;
                    info.floatConstants.push_back({
                        std::string(tableBase + entry.nameOffset),
                        entry.registerIndex,
                        entry.registerCount
                    });
                }
            }
            position += 1 + commentLength;
            continue;
        }

        if (majorVersion >= 2) {
            position += 1 + ((token >> 24) & 0x0F);
        } else {
            position += 1; // vs_1_x has no length field; scan token by token
        }
    }

    info.byteSize = (position + 1) * sizeof(DWORD);
    info.hash = HashBytes(tokens, info.byteSize);
    std::sort(info.floatConstants.begin(), info.floatConstants.end(),
              [](const NamedConstant& left, const NamedConstant& right) { return left.registerIndex < right.registerIndex; });
    return info;
}

static uint32_t CurrentShaderHash() {
    auto found = g_shaders.find(g_currentShader);
    return found == g_shaders.end() ? 0 : found->second.hash;
}

// Hunt list: every shader drawn since hunting was (re)started, in first-seen order.
// It only ever grows, so positions and hashes stay put when the camera cuts.
static std::vector<uint32_t> g_huntList;
static std::unordered_set<uint32_t> g_huntListMembers;
static int g_selectedDrawsLastFrame = 0;
static int g_selectedDrawsThisFrame = 0;

static void AddToHuntList(uint32_t shaderHash) {
    if (g_huntListMembers.insert(shaderHash).second)
        g_huntList.push_back(shaderHash);
}

static void ResetHuntList() {
    g_huntList.clear();
    g_huntListMembers.clear();
    for (uint32_t shaderHash : g_shadersLastFrame) AddToHuntList(shaderHash);
}

// Call with g_lock held.
static void UpdateWindowTitle() {
    if (!g_gameWindow || !g_titleEvent) return;
    if (!g_huntingEnabled) {
        snprintf(g_pendingTitle, sizeof(g_pendingTitle), "GHWT [probe] hunting off - F8 to start");
    } else {
        auto found = std::find(g_huntList.begin(), g_huntList.end(), g_selectedHash);
        int position = found == g_huntList.end() ? -1 : int(found - g_huntList.begin());
        snprintf(g_pendingTitle, sizeof(g_pendingTitle), "GHWT [probe] hiding VS %08X  (%d / %d)  %s",
                 g_selectedHash, position + 1, int(g_huntList.size()),
                 g_selectedDrawsLastFrame > 0 ? "ON SCREEN" : "not in this shot");
    }
    SetEvent(g_titleEvent);
}

static void StepSelection(int direction) {
    if (g_huntList.empty()) return;
    auto found = std::find(g_huntList.begin(), g_huntList.end(), g_selectedHash);
    int count = int(g_huntList.size());
    int position = found == g_huntList.end() ? 0 : int(found - g_huntList.begin()) + direction;
    position = (position % count + count) % count;
    g_selectedHash = g_huntList[position];
    LogLine("[hunt] selected VS %08X (%d / %d)", g_selectedHash, position + 1, count);
    UpdateWindowTitle();
}

static bool KeyPressed(int virtualKey) {
    static bool previousState[256] = {};
    bool isDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    bool pressed = isDown && !previousState[virtualKey];
    previousState[virtualKey] = isDown;
    return pressed;
}

static void HandleHotkeys() {
    if (KeyPressed(VK_F8)) {
        g_huntingEnabled = !g_huntingEnabled;
        if (g_huntingEnabled && g_huntList.empty()) ResetHuntList();
        if (g_huntingEnabled && g_selectedHash == 0 && !g_huntList.empty())
            g_selectedHash = g_huntList[0];
        LogLine("[hunt] %s", g_huntingEnabled ? "on" : "off");
        UpdateWindowTitle();
    }
    if (g_huntingEnabled && KeyPressed(VK_F5)) StepSelection(-1);
    if (g_huntingEnabled && KeyPressed(VK_F6)) StepSelection(+1);
    if (g_huntingEnabled && KeyPressed(VK_F7)) {
        LogLine("[hunt] MARKED VS %08X", g_selectedHash);
        WriteShaderBinary(g_selectedHash);
        FlushLog();
        MessageBeep(MB_OK);
    }
    if (g_huntingEnabled && KeyPressed(VK_F9)) {
        ResetHuntList();
        g_selectedHash = g_huntList.empty() ? 0 : g_huntList[0];
        LogLine("[hunt] list reset (%d shaders)", int(g_huntList.size()));
        UpdateWindowTitle();
    }
    if (KeyPressed(VK_F10) && !g_captureFile) g_capturePending = true;
}

// Returns true if the draw should be skipped.
static bool BeforeDraw(const char* drawKind, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount) {
    BREADCRUMB_ONCE("first draw");
    uint32_t shaderHash = CurrentShaderHash();

    if (shaderHash && std::find(g_shadersThisFrame.begin(), g_shadersThisFrame.end(), shaderHash) == g_shadersThisFrame.end())
        g_shadersThisFrame.push_back(shaderHash);

    if (g_huntingEnabled && shaderHash) AddToHuntList(shaderHash);

    bool hidden = g_huntingEnabled && shaderHash != 0 && shaderHash == g_selectedHash;
    if (hidden) ++g_selectedDrawsThisFrame;

    if (g_captureFile) {
        if (shaderHash) WriteShaderBinary(shaderHash);
        CaptureLine("");
        CaptureLine("=== draw %d  %s  type=%d prims=%u  VS=%08X  RT=%ux%u  viewport=%u,%u %ux%u z=%.2f-%.2f%s",
                    g_drawIndex, drawKind, int(primitiveType), primitiveCount, shaderHash,
                    g_renderTargetWidth, g_renderTargetHeight,
                    g_viewport.X, g_viewport.Y, g_viewport.Width, g_viewport.Height, g_viewport.MinZ, g_viewport.MaxZ,
                    hidden ? "  (HIDDEN)" : "");

        auto found = g_shaders.find(g_currentShader);
        if (found != g_shaders.end() && !found->second.floatConstants.empty()) {
            for (const NamedConstant& constant : found->second.floatConstants) {
                int shownCount = std::min(constant.registerCount, 8);
                CaptureLine("  %s  c%d (x%d)%s", constant.name.c_str(), constant.registerIndex, constant.registerCount,
                            shownCount < constant.registerCount ? "  [first 8 shown]" : "");
                for (int row = 0; row < shownCount; ++row) {
                    int registerIndex = constant.registerIndex + row;
                    if (registerIndex >= MaxFloatConstants) break;
                    const float* value = g_shadowConstants[registerIndex];
                    CaptureLine("    c%-3d % 12.5f % 12.5f % 12.5f % 12.5f", registerIndex, value[0], value[1], value[2], value[3]);
                }
            }
        } else if (g_currentShader) {
            CaptureLine("  (no constant table - raw c0..c15)");
            for (int registerIndex = 0; registerIndex < 16; ++registerIndex) {
                const float* value = g_shadowConstants[registerIndex];
                CaptureLine("    c%-3d % 12.5f % 12.5f % 12.5f % 12.5f", registerIndex, value[0], value[1], value[2], value[3]);
            }
        } else {
            CaptureLine("  (fixed function)");
        }
    }

    ++g_drawIndex;
    return hidden;
}

// ---------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DVertexShader9** shaderOut) {
    BREADCRUMB_ONCE("first CreateVertexShader");
    HRESULT result = g_originalCreateVertexShader(device, function, shaderOut);
    if (FAILED(result) || !function || !shaderOut || !*shaderOut) return result;

    EnterCriticalSection(&g_lock);
    ShaderInfo info = ParseShader(function);
    bool alreadySeen = g_shaderBytecode.count(info.hash) != 0;
    g_shaders[*shaderOut] = info;

    if (!alreadySeen) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(function);
        g_shaderBytecode.emplace(info.hash, std::vector<uint8_t>(bytes, bytes + info.byteSize));

        std::string line;
        for (const NamedConstant& constant : info.floatConstants) {
            char part[160];
            snprintf(part, sizeof(part), "  %s=c%d(x%d)", constant.name.c_str(), constant.registerIndex, constant.registerCount);
            line += part;
        }
        LogLine("[vs] %08X  %u bytes  vs_%d_%d %s", info.hash, unsigned(info.byteSize),
                (function[0] >> 8) & 0xFF, function[0] & 0xFF, line.empty() ? "  (no CTAB)" : line.c_str());
    }

    ++g_shaderCreateCount;
    if (g_shaderCreateCount % 1000 == 0) {
        LogLine("[vs] %d created so far (%u unique)", g_shaderCreateCount, unsigned(g_shaderBytecode.size()));
        FlushLog();
    }
    LeaveCriticalSection(&g_lock);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookedSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    g_currentShader = shader;
    return g_originalSetVertexShader(device, shader);
}

static HRESULT STDMETHODCALLTYPE HookedSetVertexShaderConstantF(IDirect3DDevice9* device, UINT startRegister, const float* data, UINT registerCount) {
    if (data && startRegister < MaxFloatConstants) {
        UINT copyCount = std::min<UINT>(registerCount, MaxFloatConstants - startRegister);
        memcpy(g_shadowConstants[startRegister], data, copyCount * 4 * sizeof(float));
    }
    return g_originalSetVertexShaderConstantF(device, startRegister, data, registerCount);
}

static HRESULT STDMETHODCALLTYPE HookedSetRenderTarget(IDirect3DDevice9* device, DWORD targetIndex, IDirect3DSurface9* surface) {
    if (targetIndex == 0 && surface) {
        D3DSURFACE_DESC description;
        if (SUCCEEDED(surface->GetDesc(&description))) {
            g_renderTargetWidth = description.Width;
            g_renderTargetHeight = description.Height;
        }
        CaptureLine("");
        CaptureLine("--- SetRenderTarget 0 -> %ux%u", g_renderTargetWidth, g_renderTargetHeight);
    }
    return g_originalSetRenderTarget(device, targetIndex, surface);
}

static HRESULT STDMETHODCALLTYPE HookedSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) {
    if (viewport) g_viewport = *viewport;
    return g_originalSetViewport(device, viewport);
}

static HRESULT STDMETHODCALLTYPE HookedSetTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    if (g_captureFile && matrix) {
        CaptureLine("--- SetTransform %d", int(state));
        for (int row = 0; row < 4; ++row)
            CaptureLine("    % 12.5f % 12.5f % 12.5f % 12.5f", matrix->m[row][0], matrix->m[row][1], matrix->m[row][2], matrix->m[row][3]);
    }
    return g_originalSetTransform(device, state, matrix);
}

static HRESULT STDMETHODCALLTYPE HookedDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) {
    if (BeforeDraw("DP", type, primitiveCount)) return D3D_OK;
    return g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
}

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT vertexCount, UINT startIndex, UINT primitiveCount) {
    if (BeforeDraw("DIP", type, primitiveCount)) return D3D_OK;
    return g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
}

static HRESULT STDMETHODCALLTYPE HookedDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitiveCount, const void* vertexData, UINT stride) {
    if (BeforeDraw("DPUP", type, primitiveCount)) return D3D_OK;
    return g_originalDrawPrimitiveUP(device, type, primitiveCount, vertexData, stride);
}

static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minVertex, UINT vertexCount, UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat, const void* vertexData, UINT stride) {
    if (BeforeDraw("DIPUP", type, primitiveCount)) return D3D_OK;
    return g_originalDrawIndexedPrimitiveUP(device, type, minVertex, vertexCount, primitiveCount, indexData, indexFormat, vertexData, stride);
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destRect, HWND overrideWindow, const RGNDATA* dirtyRegion) {
    BREADCRUMB_ONCE("first Present");
    HRESULT result = g_originalPresent(device, sourceRect, destRect, overrideWindow, dirtyRegion);

    EnterCriticalSection(&g_lock);
    if (g_captureFile) {
        fclose(g_captureFile);
        g_captureFile = nullptr;
        LogLine("[capture] wrote capture_%d.txt (%d draws)", g_captureNumber, g_drawIndex);
        MessageBeep(MB_ICONASTERISK);
    }

    g_shadersLastFrame.swap(g_shadersThisFrame);
    g_shadersThisFrame.clear();

    bool wasOnScreen = g_selectedDrawsLastFrame > 0;
    g_selectedDrawsLastFrame = g_selectedDrawsThisFrame;
    g_selectedDrawsThisFrame = 0;
    if (g_huntingEnabled && wasOnScreen != (g_selectedDrawsLastFrame > 0)) UpdateWindowTitle();
    g_drawIndex = 0;

    if (++g_framesSinceFlush >= 120) {
        g_framesSinceFlush = 0;
        FlushLog();
    }

    HandleHotkeys();

    if (g_capturePending) {
        g_capturePending = false;
        ++g_captureNumber;
        char capturePath[MAX_PATH];
        snprintf(capturePath, sizeof(capturePath), "%s\\capture_%d.txt", g_outputDir.c_str(), g_captureNumber);
        g_captureFile = fopen(capturePath, "w");
        CaptureLine("GHWT probe capture %d  hunting=%s selected=%08X", g_captureNumber, g_huntingEnabled ? "on" : "off", g_selectedHash);
    }
    LeaveCriticalSection(&g_lock);
    return result;
}

static void HookDevice(IDirect3DDevice9* device) {
    PatchVtableSlot(device, Slot::Present,                  (void*)HookedPresent,                  (void**)&g_originalPresent);
    PatchVtableSlot(device, Slot::SetRenderTarget,          (void*)HookedSetRenderTarget,          (void**)&g_originalSetRenderTarget);
    PatchVtableSlot(device, Slot::SetTransform,             (void*)HookedSetTransform,             (void**)&g_originalSetTransform);
    PatchVtableSlot(device, Slot::SetViewport,              (void*)HookedSetViewport,              (void**)&g_originalSetViewport);
    PatchVtableSlot(device, Slot::DrawPrimitive,            (void*)HookedDrawPrimitive,            (void**)&g_originalDrawPrimitive);
    PatchVtableSlot(device, Slot::DrawIndexedPrimitive,     (void*)HookedDrawIndexedPrimitive,     (void**)&g_originalDrawIndexedPrimitive);
    PatchVtableSlot(device, Slot::DrawPrimitiveUP,          (void*)HookedDrawPrimitiveUP,          (void**)&g_originalDrawPrimitiveUP);
    PatchVtableSlot(device, Slot::DrawIndexedPrimitiveUP,   (void*)HookedDrawIndexedPrimitiveUP,   (void**)&g_originalDrawIndexedPrimitiveUP);
    PatchVtableSlot(device, Slot::CreateVertexShader,       (void*)HookedCreateVertexShader,       (void**)&g_originalCreateVertexShader);
    PatchVtableSlot(device, Slot::SetVertexShader,          (void*)HookedSetVertexShader,          (void**)&g_originalSetVertexShader);
    PatchVtableSlot(device, Slot::SetVertexShaderConstantF, (void*)HookedSetVertexShaderConstantF, (void**)&g_originalSetVertexShaderConstantF);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* presentParameters, IDirect3DDevice9** deviceOut) {
    Breadcrumb("CreateDevice called");
    HRESULT result = g_originalCreateDevice(direct3d, adapter, deviceType, focusWindow, behaviorFlags, presentParameters, deviceOut);
    LogLine("[startup] CreateDevice returned %08lX", result);
    FlushLog();
    if (SUCCEEDED(result) && deviceOut && *deviceOut) {
        EnterCriticalSection(&g_lock);
        g_gameWindow = presentParameters && presentParameters->hDeviceWindow ? presentParameters->hDeviceWindow : focusWindow;
        if (presentParameters) {
            g_renderTargetWidth = presentParameters->BackBufferWidth;
            g_renderTargetHeight = presentParameters->BackBufferHeight;
            LogLine("[device] %ux%u windowed=%d flags=%08X", presentParameters->BackBufferWidth, presentParameters->BackBufferHeight,
                    presentParameters->Windowed, behaviorFlags);
        }
        HookDevice(*deviceOut);
        LogLine("[device] hooked");
        FlushLog();
        LeaveCriticalSection(&g_lock);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Loader / exports
// ---------------------------------------------------------------------------
static void EnsureInitialized() {
    if (g_realD3D9) return;

    InitializeCriticalSection(&g_lock);

    char modulePath[MAX_PATH];
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    std::string gameDir(modulePath);
    gameDir = gameDir.substr(0, gameDir.find_last_of("\\/"));

    g_outputDir = gameDir + "\\ghwt_vr";
    CreateDirectoryA(g_outputDir.c_str(), nullptr);
    CreateDirectoryA((g_outputDir + "\\shaders").c_str(), nullptr);
    g_mainLog = fopen((g_outputDir + "\\probe_log.txt").c_str(), "w");
    if (g_mainLog) setvbuf(g_mainLog, nullptr, _IOFBF, 1 << 20);
    LogLine("GHWT VR probe loaded (v5%s)", PROBE_VARIANT);
    FlushLog();

    g_titleEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE titleThread = CreateThread(nullptr, 0, TitleThread, nullptr, 0, nullptr))
        CloseHandle(titleThread);

    // Optional chain-load (e.g. rename wiz3D's dll to d3d9_chain.dll)
    std::string chainPath = gameDir + "\\d3d9_chain.dll";
    if (GetFileAttributesA(chainPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_realD3D9 = LoadLibraryA(chainPath.c_str());
        LogLine("chain-loaded %s", chainPath.c_str());
    }
    if (!g_realD3D9) {
        char systemDir[MAX_PATH];
        GetSystemDirectoryA(systemDir, MAX_PATH);
        g_realD3D9 = LoadLibraryA((std::string(systemDir) + "\\d3d9.dll").c_str());
    }
    if (!g_realD3D9) LogLine("FAILED to load real d3d9.dll");
}

template <typename FunctionType>
static FunctionType RealExport(const char* name) {
    EnsureInitialized();
    return reinterpret_cast<FunctionType>(GetProcAddress(g_realD3D9, name));
}

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    static auto realCreate = RealExport<Direct3DCreate9Fn>("Direct3DCreate9");
    Breadcrumb("Direct3DCreate9 called");
    IDirect3D9* direct3d = realCreate ? realCreate(sdkVersion) : nullptr;
    Breadcrumb(direct3d ? "Direct3DCreate9 returned ok" : "Direct3DCreate9 returned NULL");
#ifndef PROBE_PASSTHROUGH
    if (direct3d) PatchVtableSlot(direct3d, Slot::CreateDevice, (void*)HookedCreateDevice, (void**)&g_originalCreateDevice);
#endif
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
    if (reason == DLL_PROCESS_DETACH && g_mainLog) {
        fclose(g_mainLog);
        g_mainLog = nullptr;
    }
    return TRUE;
}
