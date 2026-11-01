#include "camera_scan.h"
#include <tlhelp32.h>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <atomic>

namespace {

constexpr float RadiansToDegrees = 57.2957795f;
constexpr float MatchTolerance = 0.001f;        // relative
constexpr float FovChangeThreshold = 0.01f;     // relative change in tan(half H fov) that counts as a new FOV
constexpr int   MinimumDistinctMatches = 3;     // FOV changes an address must have followed before we write to it
constexpr int   MaximumMisses = 2;              // consecutive refinements an address may miss before it's dropped
constexpr size_t MaximumCandidates = 4u << 20;
constexpr size_t MaximumWidenTargets = 64;
constexpr float WideHalfAngleDegrees = 80.0f;   // horizontal; vertical keeps the game's aspect

enum Encoding : uint8_t {
    HorizontalDegrees, VerticalDegrees, HorizontalRadians, VerticalRadians,
    HorizontalHalfDegrees, VerticalHalfDegrees, HorizontalHalfRadians, VerticalHalfRadians,
    HorizontalTangent, VerticalTangent, ProjectionScaleX, ProjectionScaleY,
    Horizontal4x3Degrees, Horizontal4x3Radians,
    EncodingCount
};

const char* EncodingName(int encoding) {
    static const char* names[EncodingCount] = {
        "horizontal fov (deg)", "vertical fov (deg)", "horizontal fov (rad)", "vertical fov (rad)",
        "horizontal half-fov (deg)", "vertical half-fov (deg)", "horizontal half-fov (rad)", "vertical half-fov (rad)",
        "tan(horizontal half-fov)", "tan(vertical half-fov)", "projection scale x", "projection scale y",
        "4:3 horizontal fov (deg)", "4:3 horizontal fov (rad)",
    };
    return encoding >= 0 && encoding < EncodingCount ? names[encoding] : "?";
}

// Every encoding's value for a given pair of half-angle tangents.
void EncodeFov(float tangentHorizontal, float tangentVertical, float values[EncodingCount]) {
    const float halfHorizontal = atanf(tangentHorizontal);
    const float halfVertical = atanf(tangentVertical);
    const float half4x3 = atanf(tangentVertical * 4.0f / 3.0f);
    values[HorizontalDegrees]     = 2.0f * halfHorizontal * RadiansToDegrees;
    values[VerticalDegrees]       = 2.0f * halfVertical * RadiansToDegrees;
    values[HorizontalRadians]     = 2.0f * halfHorizontal;
    values[VerticalRadians]       = 2.0f * halfVertical;
    values[HorizontalHalfDegrees] = halfHorizontal * RadiansToDegrees;
    values[VerticalHalfDegrees]   = halfVertical * RadiansToDegrees;
    values[HorizontalHalfRadians] = halfHorizontal;
    values[VerticalHalfRadians]   = halfVertical;
    values[HorizontalTangent]     = tangentHorizontal;
    values[VerticalTangent]       = tangentVertical;
    values[ProjectionScaleX]      = 1.0f / tangentHorizontal;
    values[ProjectionScaleY]      = 1.0f / tangentVertical;
    values[Horizontal4x3Degrees]  = 2.0f * half4x3 * RadiansToDegrees;
    values[Horizontal4x3Radians]  = 2.0f * half4x3;
}

bool Matches(float value, float expected) {
    return fabsf(value - expected) <= MatchTolerance * fabsf(expected);
}

struct Candidate {
    uintptr_t address;
    uint8_t   encoding;
    uint8_t   distinctMatches;
    uint8_t   misses;
};

struct WidenTarget {
    uintptr_t address;
    float     value;
};

ScanLogFunction g_log = nullptr;
std::vector<Candidate> g_candidates;
std::atomic<bool> g_scanRunning(false);
std::atomic<bool> g_scanComplete(false);
float g_scanTangentHorizontal = 0.0f;
float g_scanTangentVertical = 0.0f;
float g_lastRefineTangentHorizontal = 0.0f;
float g_previousFrameValues[EncodingCount] = {};
bool  g_havePreviousFrame = false;

std::vector<WidenTarget> g_widenTargets;
std::atomic<bool> g_widening(false);
HANDLE g_widenThread = nullptr;
char g_status[160] = "camera scan: idle (F5 in a song to start)";

void SetStatus(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_status, sizeof(g_status), format, args);
    va_end(args);
}

bool IsScannableRegion(const MEMORY_BASIC_INFORMATION& region) {
    if (region.State != MEM_COMMIT || region.Type == MEM_MAPPED) return false;
    // Write-combined / uncached pages are usually GPU-mapped buffers: reading them from the CPU is
    // orders of magnitude slower than normal memory, and game state never lives there.
    if (region.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (region.Protect & writable) != 0;
}

DWORD WINAPI ScanThread(LPVOID) {
    float expected[EncodingCount];
    EncodeFov(g_scanTangentHorizontal, g_scanTangentVertical, expected);
    float lowest = expected[0], highest = expected[0];
    for (float value : expected) { lowest = std::min(lowest, value); highest = std::max(highest, value); }
    lowest *= 1.0f - MatchTolerance;
    highest *= 1.0f + MatchTolerance;

    constexpr SIZE_T ChunkSize = 1 << 20;
    float* buffer = static_cast<float*>(VirtualAlloc(nullptr, ChunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const uintptr_t bufferAddress = reinterpret_cast<uintptr_t>(buffer);

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    // 64-bit arithmetic: with the 4 GB patch the top region ends at 0x100000000, which wraps to 0
    // in 32 bits and would restart the scan forever.
    const uint64_t lastAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    const HANDLE self = GetCurrentProcess();
    const DWORD startTick = GetTickCount();
    uint64_t bytesScanned = 0;
    uint64_t nextProgressBytes = 256u << 20;

    std::vector<Candidate> found;
    MEMORY_BASIC_INFORMATION region;
    for (uint64_t address = 0x10000; address < lastAddress && found.size() < MaximumCandidates; ) {
        if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(address)), &region, sizeof(region))) break;
        const uint64_t regionStart = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uint64_t regionEnd = regionStart + uint64_t(region.RegionSize);
        if (regionEnd <= address) break;  // no forward progress: stop rather than loop
        if (IsScannableRegion(region) && regionStart != bufferAddress) {
            for (uint64_t chunkStart = regionStart; chunkStart < regionEnd && found.size() < MaximumCandidates; chunkStart += ChunkSize) {
                const SIZE_T chunkBytes = SIZE_T(std::min<uint64_t>(ChunkSize, regionEnd - chunkStart));
                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(self, reinterpret_cast<void*>(uintptr_t(chunkStart)), buffer, chunkBytes, &bytesRead)) continue;
                bytesScanned += bytesRead;
                if (bytesScanned >= nextProgressBytes) {
                    SetStatus("camera scan: scanning... %u MB", unsigned(bytesScanned >> 20));
                    g_log("[camera scan] ...%u MB scanned, %u candidates so far (%lu ms)",
                          unsigned(bytesScanned >> 20), unsigned(found.size()), GetTickCount() - startTick);
                    nextProgressBytes += 256u << 20;
                }
                const size_t floatCount = bytesRead / sizeof(float);
                for (size_t index = 0; index < floatCount; ++index) {
                    const float value = buffer[index];
                    if (!(value >= lowest && value <= highest)) continue;
                    for (int encoding = 0; encoding < EncodingCount; ++encoding) {
                        if (Matches(value, expected[encoding]))
                            found.push_back({ uintptr_t(chunkStart) + index * sizeof(float), uint8_t(encoding), 1, 0 });
                    }
                }
            }
        }
        address = regionEnd;
    }
    VirtualFree(buffer, 0, MEM_RELEASE);

    std::sort(found.begin(), found.end(), [](const Candidate& left, const Candidate& right) { return left.address < right.address; });
    g_candidates.swap(found);
    g_lastRefineTangentHorizontal = g_scanTangentHorizontal;
    if (found.size() >= MaximumCandidates)
        g_log("[camera scan] hit the %u candidate limit - results may be incomplete", unsigned(MaximumCandidates));
    g_log("[camera scan] scanned %u MB in %lu ms: %u candidates (fov %.1f deg). Play on: each camera cut that changes zoom narrows them down.",
          unsigned(bytesScanned >> 20), GetTickCount() - startTick, unsigned(g_candidates.size()),
          2.0f * atanf(g_scanTangentHorizontal) * RadiansToDegrees);
    SetStatus("camera scan: %u candidates", unsigned(g_candidates.size()));
    g_scanComplete = true;
    g_scanRunning = false;
    return 0;
}

// Reads the current value at each candidate, batching reads per 4 KB page.
void ReadCandidateValues(std::vector<float>& values, std::vector<bool>& readable) {
    values.assign(g_candidates.size(), 0.0f);
    readable.assign(g_candidates.size(), false);
    const HANDLE self = GetCurrentProcess();
    uint8_t page[4096];
    uintptr_t loadedPage = ~uintptr_t(0);
    bool pageValid = false;
    for (size_t index = 0; index < g_candidates.size(); ++index) {
        const uintptr_t address = g_candidates[index].address;
        const uintptr_t pageStart = address & ~uintptr_t(4095);
        if (pageStart != loadedPage) {
            SIZE_T bytesRead = 0;
            pageValid = ReadProcessMemory(self, reinterpret_cast<void*>(pageStart), page, sizeof(page), &bytesRead) && bytesRead == sizeof(page);
            loadedPage = pageStart;
        }
        if (pageValid && (address - pageStart) <= sizeof(page) - sizeof(float)) {
            memcpy(&values[index], page + (address - pageStart), sizeof(float));
            readable[index] = true;
        }
    }
}

size_t CountTrusted() {
    size_t trusted = 0;
    for (const Candidate& candidate : g_candidates) if (candidate.distinctMatches >= MinimumDistinctMatches) ++trusted;
    return trusted;
}

DWORD WINAPI WidenThread(LPVOID) {
    const HANDLE self = GetCurrentProcess();
    while (g_widening) {
        for (const WidenTarget& target : g_widenTargets)
            WriteProcessMemory(self, reinterpret_cast<void*>(target.address), &target.value, sizeof(float), nullptr);
        Sleep(0);
    }
    return 0;
}

void DescribeAddress(uintptr_t address, char* text, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module) {
        char modulePath[MAX_PATH] = "";
        GetModuleFileNameA(module, modulePath, MAX_PATH);
        const char* moduleName = strrchr(modulePath, '\\');
        snprintf(text, size, "%s+0x%X", moduleName ? moduleName + 1 : modulePath, unsigned(address - reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(text, size, "heap 0x%08X", unsigned(address));
    }
}

// ---------------------------------------------------------------------------
// Hardware write hooks
//
// x86 debug registers DR0-DR3 can trap right after any instruction writes to an address.
// The handler immediately replaces what the game wrote with the wide value, so whatever
// the game computes next (projection, frustum, culling) always sees the wide FOV - no race.
// It also records which game instruction did the write, for a permanent patch later.
// ---------------------------------------------------------------------------
constexpr int HardwareSlotCount = 4;
constexpr DWORD ReapplyIntervalMs = 2000;  // threads the game creates later need the registers too

enum class HookKind : uint8_t {
    Write,        // trap after an instruction writes the address; overwrite it with value
    Execute,      // trap before the instruction at address runs; overwrite the float at [register + offset]
    FollowWrite,  // (unused) trap after the game writes a camera's matrix
    ViewSetup,    // trap in the renderer's view setup (edi = view, esi = its camera): head follow
    AccessLog,    // trap after any read or write of the address; just record which code did it
    CrowdCamera,  // trap where the crowd update reads the main camera (esi): give it the head pose
};

enum class BaseRegister : uint8_t { Esp, Esi };

struct WriteHook {
    uintptr_t    address;
    float        value;
    HookKind     kind;
    int32_t      offset;
    BaseRegister base;
    bool         rewrite = true;   // false: only observe (execute hooks still record the camera object)
};

// ---------------------------------------------------------------------------
// Game camera objects (seen at the FOV code, where esi is the camera) and head follow
// ---------------------------------------------------------------------------
constexpr uintptr_t EffectiveFovInstructionAddress = 0x5C16CD;   // inside the camera's FOV getter
// Camera object layout (confirmed from a live dump against the rendered camera):
//   +0x74  position x, y, z (world)
//   +0x90  rotation rows: right, up, back (4 floats each, w = 0)
// The mod's packed "pose" is 16 floats: rows 0-2 = those rotation rows, row 3 = position, 1.
constexpr int CameraRotationOffset = 0x90;
constexpr int CameraPositionOffset = 0x74;

void ReadCameraPose(uintptr_t object, float pose[16]) {
    memcpy(pose, reinterpret_cast<const void*>(object + CameraRotationOffset), 12 * sizeof(float));
    memcpy(pose + 12, reinterpret_cast<const void*>(object + CameraPositionOffset), 3 * sizeof(float));
    pose[15] = 1.0f;
}

void WriteCameraPose(uintptr_t object, const float pose[16]) {
    memcpy(reinterpret_cast<void*>(object + CameraRotationOffset), pose, 12 * sizeof(float));
    memcpy(reinterpret_cast<void*>(object + CameraPositionOffset), pose + 12, 3 * sizeof(float));
}

volatile uintptr_t g_followObject = 0;   // the followed view
float g_followMatrix[16] = {};        // what we put into the camera
float g_lastWrittenMatrix[16] = {};   // what we last wrote (to tell the game's own updates apart)
float g_scriptedMatrix[16] = {};      // the game's own camera before we overwrote it
volatile LONG g_scriptedValid = 0;
volatile LONG g_followWrites = 0;
volatile LONG g_scriptedUpdates = 0;


uintptr_t g_gameImageStart = 0, g_gameImageEnd = 0;

bool IsGameCode(uintptr_t address) {
    return address >= g_gameImageStart && address < g_gameImageEnd;
}

// Camera fields next to the matrix: base / effective FOV in degrees, then near plane.
bool StillLooksLikeCamera(uintptr_t object) {
    if (!object) return false;
    const float* fields = reinterpret_cast<const float*>(object + 0xD0);
    const float baseFov = fields[0], effectiveFov = fields[1], nearPlane = fields[3];
    return baseFov > 1.0f && baseFov < 179.0f && effectiveFov > 1.0f && effectiveFov < 179.0f &&
           nearPlane > 0.0001f && nearPlane < 10.0f;
}

// Rows the game rewrote (anything not equal to what we last wrote) become the scripted camera;
// then the whole matrix gets the head pose again.
void ApplyFollowTo(uintptr_t object) {
    if (!object || object != g_followObject) return;
    float current[16];
    ReadCameraPose(object, current);
    bool gameWrote = false;
    for (int row = 0; row < 4; ++row) {
        const int length = row < 3 ? 4 : 3;
        if (memcmp(current + row * 4, g_lastWrittenMatrix + row * 4, length * sizeof(float)) == 0) continue;
        memcpy(g_scriptedMatrix + row * 4, current + row * 4, length * sizeof(float));
        g_scriptedMatrix[15] = 1.0f;
        gameWrote = true;
    }
    if (gameWrote) {
        g_scriptedValid = 1;
        InterlockedIncrement(&g_scriptedUpdates);
    }
    WriteCameraPose(object, g_followMatrix);
    memcpy(g_lastWrittenMatrix, g_followMatrix, sizeof(g_lastWrittenMatrix));
    InterlockedIncrement(&g_followWrites);
}

volatile LONG g_hookHits[4] = {};

struct WriterRecord {
    uintptr_t instruction;   // address of the instruction after the write
    uintptr_t target;
    volatile LONG hits;
    bool      logged;
};

WriteHook g_hooks[HardwareSlotCount] = {};

struct ImageRange {
    uintptr_t start;
    uintptr_t end;
};
volatile LONG g_hookCount = 0;
volatile LONG g_hooksActive = 0;
PVOID g_exceptionHandler = nullptr;
// Per-thread "inside the handler" flag: the handler's own write to a hooked address trips the
// same breakpoint, and must be ignored or the handler recurses until the stack overflows.
DWORD g_insideHandlerTls = TLS_OUT_OF_INDEXES;
DWORD g_lastApplyTick = 0;

constexpr int MaximumWriterRecords = 160;
WriterRecord g_writers[MaximumWriterRecords] = {};
volatile LONG g_writerCount = 0;

// Runs inside the exception dispatcher: no allocation, no locks, no logging.
void RecordWriter(uintptr_t instruction, uintptr_t target) {
    const LONG count = g_writerCount;
    for (LONG index = 0; index < count; ++index) {
        if (g_writers[index].instruction == instruction && g_writers[index].target == target) {
            InterlockedIncrement(&g_writers[index].hits);
            return;
        }
    }
    const LONG slot = InterlockedIncrement(&g_writerCount) - 1;
    if (slot >= MaximumWriterRecords) { InterlockedDecrement(&g_writerCount); return; }
    g_writers[slot].instruction = instruction;
    g_writers[slot].target = target;
    g_writers[slot].hits = 1;
    g_writers[slot].logged = false;
}

// ---------------------------------------------------------------------------
// The renderer's view setup (0x70D910) builds the view matrix, culling planes and camera globals
// from a view's camera object. Hooked just after it fetches the FOV (0x70D930), before it reads
// the camera's pose: for the followed view, the camera's pose is swapped for the head pose there,
// so rendering and culling both see what you see. Views persist across shots (the camera objects
// they use change at each cut), so the view is what gets followed.
// ---------------------------------------------------------------------------
constexpr uintptr_t ViewSetupAfterFovInstruction = 0x70D930;   // mov ecx, edi (esi = camera, edi = view)
const uint8_t ExpectedViewSetupBytes[] = { 0x8B, 0xCF, 0xE8 };
constexpr int MaximumViews = 16;

struct ViewRecord {
    volatile uintptr_t view;
    volatile uintptr_t camera;
    volatile LONG      hits;
    float              originalPose[16];   // the camera as the game set it, before any head follow
    uintptr_t          callers[12];        // diagnostics: game code addresses found on the stack (first sighting)
    bool               callersCaptured;
};

// Diagnostics: the order of view setups within one frame, and how many draws came before each.
constexpr int MaximumTracedSetups = 32;
struct ViewSetupTrace {
    uintptr_t view;
    unsigned  drawIndex;
};
volatile LONG g_traceActive = 0;
ViewSetupTrace g_trace[MaximumTracedSetups] = {};
volatile LONG g_traceCount = 0;
volatile unsigned* g_drawCounter = nullptr;

void CaptureCallers(ViewRecord* record, uintptr_t stackPointer) {
    if (record->callersCaptured) return;
    record->callersCaptured = true;
    const NT_TIB* threadBlock = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t stackTop = reinterpret_cast<uintptr_t>(threadBlock->StackBase);
    int found = 0;
    for (uintptr_t slot = stackPointer; slot + 4 <= stackTop && slot < stackPointer + 0x600 && found < 12; slot += 4) {
        const uintptr_t value = *reinterpret_cast<const uintptr_t*>(slot);
        if (value > 0x401000 && value < 0xA00000) record->callers[found++] = value;   // GHWT.exe code
    }
}
ViewRecord g_views[MaximumViews] = {};
volatile LONG g_viewCount = 0;
volatile LONG g_followMatrixValid = 0;
volatile uintptr_t g_followedViewCamera = 0;   // other views drawing with the same camera follow too
volatile float g_followFovDegrees = 0.0f;      // culling FOV for the followed view (0 = the game's)
constexpr int ViewSetupFovStackOffset = 0x18;  // 0x70D92C stored the camera's FOV (degrees) here

// Runs inside the exception dispatcher: no allocation, no locks, no logging.
void OnViewSetup(uintptr_t view, uintptr_t camera, uintptr_t stackPointer) {
    if (!view || !camera) return;
    ViewRecord* record = nullptr;
    const LONG count = g_viewCount;
    for (LONG index = 0; index < count && index < MaximumViews; ++index)
        if (g_views[index].view == view) { record = &g_views[index]; break; }
    if (!record) {
        const LONG slot = InterlockedIncrement(&g_viewCount) - 1;
        if (slot >= MaximumViews) { InterlockedDecrement(&g_viewCount); return; }
        record = &g_views[slot];
        record->hits = 0;
        record->view = view;
    }
    record->camera = camera;
    InterlockedIncrement(&record->hits);
    CaptureCallers(record, stackPointer);
    if (g_traceActive) {
        const LONG slot = InterlockedIncrement(&g_traceCount) - 1;
        if (slot < MaximumTracedSetups) g_trace[slot] = { view, g_drawCounter ? *g_drawCounter : 0u };
    }

    float current[16];
    ReadCameraPose(camera, current);
    if (view == g_followObject) g_followedViewCamera = camera;
    const bool followed = g_followMatrixValid && (view == g_followObject || camera == g_followedViewCamera);
    if (!followed) {
        memcpy(record->originalPose, current, sizeof(current));
        return;
    }
    // Followed view. If the camera still holds what we wrote last time the game didn't move it,
    // so the scripted pose is unchanged.
    if (memcmp(current, g_lastWrittenMatrix, 15 * sizeof(float)) != 0) {
        memcpy(g_scriptedMatrix, current, sizeof(current));
        g_scriptedValid = 1;
        InterlockedIncrement(&g_scriptedUpdates);
    }
    if (g_scriptedValid) memcpy(record->originalPose, g_scriptedMatrix, sizeof(g_scriptedMatrix));
    WriteCameraPose(camera, g_followMatrix);
    // Your eyes see wider than the game's shot: widen what this view keeps (culling margin).
    float* viewFov = reinterpret_cast<float*>(stackPointer + ViewSetupFovStackOffset);
    const float wantedFov = g_followFovDegrees;
    if (wantedFov > *viewFov) *viewFov = wantedFov;
    memcpy(g_lastWrittenMatrix, g_followMatrix, sizeof(g_lastWrittenMatrix));
    InterlockedIncrement(&g_followWrites);
}

// ---------------------------------------------------------------------------
// The crowd update (0x4373B0) aims the crowd cameras - which pick each crowd card's sprite angle -
// from the main view's camera, read at 0x4374B4 (esi = camera). That runs before the renderer's
// view setup, so it still saw the scripted shot and the crowd turned at every cut. Writing the
// head pose there too makes the crowd face the way it would from where you really are.
// ---------------------------------------------------------------------------
constexpr uintptr_t CrowdCameraReadInstruction = 0x4374B4;   // movss xmm1, [esi+0xB4]
const uint8_t ExpectedCrowdCameraBytes[] = { 0xF3, 0x0F, 0x10, 0x8E, 0xB4, 0x00, 0x00, 0x00 };
volatile LONG g_crowdFollowEnabled = 1;
volatile LONG g_crowdFollowWrites = 0;
bool g_crowdHookAvailable = false;

void OnCrowdCameraRead(uintptr_t camera) {
    if (!g_crowdFollowEnabled || !g_followMatrixValid || !camera || camera != g_followedViewCamera) return;
    float current[16];
    ReadCameraPose(camera, current);
    if (memcmp(current, g_lastWrittenMatrix, 15 * sizeof(float)) != 0) {
        memcpy(g_scriptedMatrix, current, sizeof(current));
        g_scriptedValid = 1;
    }
    WriteCameraPose(camera, g_followMatrix);
    memcpy(g_lastWrittenMatrix, g_followMatrix, sizeof(g_lastWrittenMatrix));
    InterlockedIncrement(&g_crowdFollowWrites);
}

LONG CALLBACK DataBreakpointHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const LONG hookCount = g_hookCount;
    if (hookCount == 0) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* context = exceptionInfo->ContextRecord;

    // Under WOW64 (32-bit game on 64-bit Windows) DR6 isn't always filled in for the handler.
    // Nothing else in the game single-steps, so with hooks armed any single-step is ours:
    // if we can't tell which slot fired, re-apply all of them (harmless - same values).
    DWORD hitSlots = context->Dr6 & 0xF;
    if (!hitSlots) hitSlots = (1u << hookCount) - 1;

    if (TlsGetValue(g_insideHandlerTls)) {   // trap caused by our own write below
        context->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    TlsSetValue(g_insideHandlerTls, reinterpret_cast<LPVOID>(1));

    for (int slot = 0; slot < HardwareSlotCount && slot < hookCount; ++slot) {
        if (!(hitSlots & (1u << slot))) continue;
        const WriteHook& hook = g_hooks[slot];
        if (hook.kind == HookKind::Execute || hook.kind == HookKind::ViewSetup || hook.kind == HookKind::CrowdCamera) {
            if (context->Eip != hook.address) continue;   // EIP identifies execute traps reliably
            if (hook.kind == HookKind::ViewSetup) {
                if (g_hooksActive) OnViewSetup(context->Edi, context->Esi, context->Esp);
                context->EFlags |= 0x10000;
                InterlockedIncrement(&g_hookHits[slot]);
                continue;
            }
            if (hook.kind == HookKind::CrowdCamera) {
                if (g_hooksActive) OnCrowdCameraRead(context->Esi);
                context->EFlags |= 0x10000;
                InterlockedIncrement(&g_hookHits[slot]);
                continue;
            }
            const uintptr_t baseValue = hook.base == BaseRegister::Esp ? context->Esp : context->Esi;
            if (g_hooksActive && hook.rewrite) *reinterpret_cast<volatile float*>(baseValue + hook.offset) = hook.value;
            context->EFlags |= 0x10000;                     // resume flag: run the instruction without re-trapping
            InterlockedIncrement(&g_hookHits[slot]);
        } else if (hook.kind == HookKind::AccessLog) {
            RecordWriter(context->Eip, hook.address);
            InterlockedIncrement(&g_hookHits[slot]);
        } else if (hook.kind == HookKind::FollowWrite) {
            // Only when the game's own code updated a still-live camera. A write from elsewhere
            // (the heap freeing / reusing the object during loading) must be left alone.
            if (g_hooksActive && IsGameCode(context->Eip) && StillLooksLikeCamera(g_followObject)) ApplyFollowTo(g_followObject);
            RecordWriter(context->Eip, hook.address);
            InterlockedIncrement(&g_hookHits[slot]);
        } else {
            if (g_hooksActive && hook.rewrite) *reinterpret_cast<volatile float*>(hook.address) = hook.value;
            RecordWriter(context->Eip, hook.address);
            InterlockedIncrement(&g_hookHits[slot]);
        }
    }
    TlsSetValue(g_insideHandlerTls, nullptr);
    context->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI ApplyDebugRegistersThread(LPVOID enableParameter) {
    const bool enable = enableParameter != nullptr;
    const DWORD processId = GetCurrentProcessId();
    const DWORD selfThreadId = GetCurrentThreadId();

    // Collect thread ids first; nothing below may allocate while a thread is suspended
    // (it could be holding the heap lock).
    DWORD threadIds[512];
    int threadCount = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry = { sizeof(entry) };
    for (BOOL more = Thread32First(snapshot, &entry); more && threadCount < 512; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == processId && entry.th32ThreadID != selfThreadId)
            threadIds[threadCount++] = entry.th32ThreadID;
    CloseHandle(snapshot);

    DWORD controlBits = 0;
    const LONG hookCount = g_hookCount;
    for (int slot = 0; slot < HardwareSlotCount && slot < hookCount && enable; ++slot) {
        controlBits |= 1u << (slot * 2);              // local enable
        if (g_hooks[slot].kind == HookKind::AccessLog) {
            controlBits |= 0x3u << (16 + slot * 4);   // break on read or write
            controlBits |= 0x3u << (18 + slot * 4);   // 4 bytes
        } else if (g_hooks[slot].kind == HookKind::Write || g_hooks[slot].kind == HookKind::FollowWrite) {
            controlBits |= 0x1u << (16 + slot * 4);   // break on write
            controlBits |= 0x3u << (18 + slot * 4);   // 4 bytes
        }                                             // execute: type 00, length 00
    }

    for (int index = 0; index < threadCount; ++index) {
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadIds[index]);
        if (!thread) continue;
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = hookCount > 0 ? g_hooks[0].address : 0;
                context.Dr1 = hookCount > 1 ? g_hooks[1].address : 0;
                context.Dr2 = hookCount > 2 ? g_hooks[2].address : 0;
                context.Dr3 = hookCount > 3 ? g_hooks[3].address : 0;
                context.Dr7 = (context.Dr7 & ~0xFFFF00FFu) | controlBits;
                context.Dr6 = 0;
                SetThreadContext(thread, &context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    return 0;
}

// Debug registers are per thread and a thread can't safely rewrite its own, so a helper
// thread applies them to every thread (including the caller, which waits meanwhile).
void ApplyDebugRegisters(bool enable) {
    HANDLE helper = CreateThread(nullptr, 0, ApplyDebugRegistersThread, enable ? reinterpret_cast<LPVOID>(1) : nullptr, 0, nullptr);
    if (!helper) return;
    WaitForSingleObject(helper, 5000);
    CloseHandle(helper);
    g_lastApplyTick = GetTickCount();
}

void LogNewWriters() {
    const LONG count = g_writerCount;
    for (LONG index = 0; index < count && index < MaximumWriterRecords; ++index) {
        WriterRecord& writer = g_writers[index];
        if (writer.logged) continue;
        writer.logged = true;
        char instructionText[MAX_PATH + 32], targetText[MAX_PATH + 32];
        DescribeAddress(writer.instruction, instructionText, sizeof(instructionText));
        DescribeAddress(writer.target, targetText, sizeof(targetText));
        uint8_t codeBytes[24] = {};
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(writer.instruction - 16), codeBytes, sizeof(codeBytes), nullptr);
        char hexText[24 * 3 + 8] = "";
        for (int byteIndex = 0; byteIndex < 24; ++byteIndex) {
            char part[4];
            snprintf(part, sizeof(part), byteIndex == 16 ? "|%02X" : " %02X", codeBytes[byteIndex]);
            strcat(hexText, part);
        }
        g_log("[camera scan] %s is written by code just before %s  bytes:%s", targetText, instructionText, hexText);
    }
}

} // namespace

namespace CameraScan {

void Start(float projectionScaleX, float projectionScaleY, ScanLogFunction log) {
    g_log = log;
    if (g_scanRunning) { g_log("[camera scan] already scanning"); return; }
    if (g_widening) ToggleWiden();
    if (projectionScaleX <= 0.0f || projectionScaleY <= 0.0f) { g_log("[camera scan] no game camera yet - start a song first"); return; }
    g_scanTangentHorizontal = 1.0f / projectionScaleX;
    g_scanTangentVertical = 1.0f / projectionScaleY;
    g_scanComplete = false;
    g_scanRunning = true;
    g_havePreviousFrame = false;
    SetStatus("camera scan: scanning...");
    g_log("[camera scan] scanning memory for fov %.2f deg (this runs in the background)",
          2.0f * atanf(g_scanTangentHorizontal) * RadiansToDegrees);
    if (HANDLE thread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr)) CloseHandle(thread);
    else g_scanRunning = false;
}

void OnFrame(float projectionScaleX, float projectionScaleY) {
    if (projectionScaleX <= 0.0f || projectionScaleY <= 0.0f) return;
    float currentValues[EncodingCount];
    EncodeFov(1.0f / projectionScaleX, 1.0f / projectionScaleY, currentValues);

    const bool canRefine = g_scanComplete && !g_widening && !g_candidates.empty() && g_havePreviousFrame;
    const float tangentHorizontal = 1.0f / projectionScaleX;
    const bool fovChanged = fabsf(tangentHorizontal - g_lastRefineTangentHorizontal) > FovChangeThreshold * g_lastRefineTangentHorizontal;

    if (canRefine && fovChanged) {
        std::vector<float> values;
        std::vector<bool> readable;
        ReadCandidateValues(values, readable);

        // Memory may hold this frame's or the next frame's camera, so accept either.
        size_t kept = 0;
        for (size_t index = 0; index < g_candidates.size(); ++index) {
            Candidate candidate = g_candidates[index];
            const bool matched = readable[index] &&
                (Matches(values[index], currentValues[candidate.encoding]) ||
                 Matches(values[index], g_previousFrameValues[candidate.encoding]));
            if (matched) {
                if (candidate.distinctMatches < 255) ++candidate.distinctMatches;
                candidate.misses = 0;
            } else if (++candidate.misses > MaximumMisses || !readable[index]) {
                continue;
            }
            g_candidates[kept++] = candidate;
        }
        g_candidates.resize(kept);
        g_lastRefineTangentHorizontal = tangentHorizontal;

        const size_t trusted = CountTrusted();
        g_log("[camera scan] fov now %.1f deg: %u candidates left, %u followed %d+ zoom changes",
              currentValues[HorizontalDegrees], unsigned(g_candidates.size()), unsigned(trusted), MinimumDistinctMatches);
        SetStatus("camera scan: %u candidates, %u trusted", unsigned(g_candidates.size()), unsigned(trusted));
        if (g_candidates.size() <= 64) LogCandidates();
    }

    memcpy(g_previousFrameValues, currentValues, sizeof(currentValues));
    g_havePreviousFrame = true;
}

void ToggleWiden() {
    if (g_widening) {
        g_widening = false;
        g_hooksActive = 0;
        ApplyDebugRegisters(false);
        g_hookCount = 0;
        if (g_widenThread) { WaitForSingleObject(g_widenThread, 1000); CloseHandle(g_widenThread); g_widenThread = nullptr; }
        LogNewWriters();
        g_log("[camera scan] widening OFF");
        SetStatus("camera scan: widening off");
        return;
    }
    if (!g_scanComplete) { g_log("[camera scan] nothing to widen yet - press F5 in a song first"); return; }

    const size_t trusted = CountTrusted();
    if (trusted == 0) { g_log("[camera scan] no address has followed %d zoom changes yet - keep playing", MinimumDistinctMatches); return; }
    if (trusted > MaximumWidenTargets) {
        g_log("[camera scan] %u trusted addresses is too many to write safely (max %u) - keep playing to narrow down",
              unsigned(trusted), unsigned(MaximumWidenTargets));
        return;
    }

    // Wide target: 80 degree horizontal half-angle, vertical scaled to keep the game's aspect.
    const float aspectTangentRatio = g_scanTangentVertical / g_scanTangentHorizontal;
    const float wideTangentHorizontal = tanf(WideHalfAngleDegrees / RadiansToDegrees);
    float wideValues[EncodingCount];
    EncodeFov(wideTangentHorizontal, wideTangentHorizontal * aspectTangentRatio, wideValues);

    // Degree values are what the camera script writes; everything else is derived from them,
    // so those get the hardware hooks. Anything left over is held by the polling thread.
    std::vector<Candidate> trustedCandidates;
    for (const Candidate& candidate : g_candidates)
        if (candidate.distinctMatches >= MinimumDistinctMatches) trustedCandidates.push_back(candidate);
    // Order: degree values first, then the ones that followed the most changes (the live camera,
    // not a stale copy), then addresses inside the game's own image before heap ones.
    HMODULE gameModule = GetModuleHandleA(nullptr);
    ImageRange gameImage = {};
    if (gameModule) {
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameModule);
        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(gameModule) + dosHeader->e_lfanew);
        gameImage.start = reinterpret_cast<uintptr_t>(gameModule);
        gameImage.end = gameImage.start + ntHeaders->OptionalHeader.SizeOfImage;
    }
    auto inGameImage = [&](uintptr_t address) { return address >= gameImage.start && address < gameImage.end; };
    std::stable_sort(trustedCandidates.begin(), trustedCandidates.end(), [&](const Candidate& left, const Candidate& right) {
        const bool leftDegrees = left.encoding == HorizontalDegrees, rightDegrees = right.encoding == HorizontalDegrees;
        if (leftDegrees != rightDegrees) return leftDegrees;
        if (left.distinctMatches != right.distinctMatches) return left.distinctMatches > right.distinctMatches;
        return inGameImage(left.address) && !inGameImage(right.address);
    });

    g_widenTargets.clear();
    LONG hookCount = 0;
    for (const Candidate& candidate : trustedCandidates) {
        if (hookCount < HardwareSlotCount && candidate.encoding == HorizontalDegrees) {
            g_hooks[hookCount++] = { candidate.address, wideValues[candidate.encoding], HookKind::Write, 0, BaseRegister::Esp };
        } else {
            g_widenTargets.push_back({ candidate.address, wideValues[candidate.encoding] });
        }
    }
    g_hookCount = hookCount;

    if (g_insideHandlerTls == TLS_OUT_OF_INDEXES) g_insideHandlerTls = TlsAlloc();
    if (g_insideHandlerTls == TLS_OUT_OF_INDEXES) { g_log("[camera scan] TlsAlloc failed - not widening"); return; }
    if (!g_exceptionHandler) g_exceptionHandler = AddVectoredExceptionHandler(1, DataBreakpointHandler);
    g_hooksActive = 1;
    // Write the wide value once now so the next frame starts wide, then arm the hooks.
    for (LONG slot = 0; slot < hookCount; ++slot)
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(g_hooks[slot].address), &g_hooks[slot].value, sizeof(float), nullptr);
    ApplyDebugRegisters(true);

    g_widening = true;
    g_widenThread = CreateThread(nullptr, 0, WidenThread, nullptr, 0, nullptr);
    if (g_widenThread) SetThreadPriority(g_widenThread, THREAD_PRIORITY_ABOVE_NORMAL);
    g_log("[camera scan] widening ON at %.0f deg: %ld addresses hooked on write (hardware), %u held by polling",
          2.0f * WideHalfAngleDegrees, hookCount, unsigned(g_widenTargets.size()));
    SetStatus("camera scan: widening ON (%ld hooked, %u polled)", hookCount, unsigned(g_widenTargets.size()));
}

void LogCandidates() {
    if (!g_scanComplete) { g_log("[camera scan] no scan yet"); return; }
    std::vector<Candidate> sorted = g_candidates;
    std::sort(sorted.begin(), sorted.end(), [](const Candidate& left, const Candidate& right) {
        return left.distinctMatches > right.distinctMatches;
    });
    std::vector<float> values;
    std::vector<bool> readable;
    ReadCandidateValues(values, readable);
    g_log("[camera scan] top candidates (of %u):", unsigned(g_candidates.size()));
    const size_t shown = std::min<size_t>(sorted.size(), 40);
    for (size_t rank = 0; rank < shown; ++rank) {
        const Candidate& candidate = sorted[rank];
        auto position = std::lower_bound(g_candidates.begin(), g_candidates.end(), candidate.address,
            [](const Candidate& entry, uintptr_t address) { return entry.address < address; });
        const size_t index = size_t(position - g_candidates.begin());
        char where[MAX_PATH + 32];
        DescribeAddress(candidate.address, where, sizeof(where));
        g_log("   %-32s %-26s value %10.4f  followed %u changes", where, EncodingName(candidate.encoding),
              index < values.size() && readable[index] ? values[index] : -1.0f, unsigned(candidate.distinctMatches));
    }
}

// GHWT.exe 1.0.1.25215 (image base 0x400000, no relocations):
//   0x5C1620 computes every camera's effective FOV (camera+0xD4) from its base FOV (camera+0xD0).
//   At 0x5C16CD the result sits in [esp+4] just before being compared/stored, so replacing it
//   there widens every camera, including the one the renderer and culling use.
//   At 0x5C1626 the same function reads the camera's base FOV (camera+0xD0, camera in esi);
//   writing the wide value there first makes the camera object itself wide, for any code that
//   reads +0xD0 directly.
//   0xD58234 is the renderer's global copy of the FOV; hooking its write is a backup.
constexpr uintptr_t BaseFovReadInstruction = 0x5C1626;
constexpr int32_t   CameraBaseFovOffset = 0xD0;
constexpr uintptr_t EffectiveFovResultInstruction = 0x5C16CD;
constexpr int32_t   EffectiveFovResultStackOffset = 4;
constexpr uintptr_t RendererGlobalFov = 0xD58234;
const uint8_t ExpectedInstructionBytes[] = { 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04 };  // movss xmm0, [esp+4]
const uint8_t ExpectedBaseReadBytes[] = { 0xD9, 0x86, 0xD0, 0x00, 0x00, 0x00 };     // fld dword [esi+0xD0]

bool g_cameraHooksArmed = false;
bool g_cameraHooksWiden = false;
float g_wideFovDegrees = 160.0f;

// Slot 0: the renderer's view setup (head follow; once or twice a frame).
// When widening: slot 1 the FOV code (runs ~60 times a frame), slot 2 the renderer's global FOV copy.
uintptr_t g_accessAddresses[3] = {};
bool g_accessCaptureActive = false;

void LayOutHooks() {
    g_hooks[0] = { ViewSetupAfterFovInstruction, 0.0f, HookKind::ViewSetup, 0, BaseRegister::Esi, false };
    int count = 1;
    if (g_crowdHookAvailable)
        g_hooks[count++] = { CrowdCameraReadInstruction, 0.0f, HookKind::CrowdCamera, 0, BaseRegister::Esi, false };
    if (g_accessCaptureActive) {
        for (int index = 0; index < 2 && count < HardwareSlotCount; ++index)
            g_hooks[count++] = { g_accessAddresses[index], 0.0f, HookKind::AccessLog, 0, BaseRegister::Esp, false };
    } else if (g_cameraHooksWiden && count + 2 <= HardwareSlotCount) {
        g_hooks[count++] = { EffectiveFovResultInstruction, g_wideFovDegrees, HookKind::Execute, EffectiveFovResultStackOffset, BaseRegister::Esp, true };
        g_hooks[count++] = { RendererGlobalFov, g_wideFovDegrees, HookKind::Write, 0, BaseRegister::Esp, true };
    }
    g_hookCount = count;
}

bool CodeMatches(uintptr_t address, const uint8_t* expected, size_t length) {
    uint8_t actual[16] = {};
    if (length > sizeof(actual)) return false;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), actual, length, nullptr)) return false;
    return memcmp(actual, expected, length) == 0;
}

bool IsKnownGameBuild() {
    return CodeMatches(EffectiveFovResultInstruction, ExpectedInstructionBytes, sizeof(ExpectedInstructionBytes)) &&
           CodeMatches(BaseFovReadInstruction, ExpectedBaseReadBytes, sizeof(ExpectedBaseReadBytes)) &&
           CodeMatches(ViewSetupAfterFovInstruction, ExpectedViewSetupBytes, sizeof(ExpectedViewSetupBytes));
}

bool ArmCameraHooks(bool widenFov, float wideFovDegrees, ScanLogFunction log) {
    g_log = log;
    if (g_cameraHooksArmed && g_cameraHooksWiden == widenFov) return true;
    if (g_cameraHooksArmed) DisarmCameraHooks();
    if (g_widening) ToggleWiden();
    if (!IsKnownGameBuild()) {
        g_log("[camera hooks] this GHWT.exe doesn't match the known build - head follow / wide culling unavailable");
        return false;
    }
    if (g_insideHandlerTls == TLS_OUT_OF_INDEXES) g_insideHandlerTls = TlsAlloc();
    if (g_insideHandlerTls == TLS_OUT_OF_INDEXES) return false;
    if (!g_exceptionHandler) g_exceptionHandler = AddVectoredExceptionHandler(1, DataBreakpointHandler);

    // (A hook on the camera's base FOV read at 0x5C1626 was tried and removed: the crowd's
    //  level-of-detail reads the base FOV, and at 160 degrees the crowd stops being drawn.)
    // Slot 0 always runs: it is where the camera objects are seen (and where head follow writes).
    if (!g_gameImageStart) {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dosHeader->e_lfanew);
        g_gameImageStart = base;
        g_gameImageEnd = base + ntHeaders->OptionalHeader.SizeOfImage;
    }
    g_crowdHookAvailable = CodeMatches(CrowdCameraReadInstruction, ExpectedCrowdCameraBytes, sizeof(ExpectedCrowdCameraBytes));
    g_wideFovDegrees = wideFovDegrees;
    g_cameraHooksWiden = widenFov;
    for (volatile LONG& hits : g_hookHits) hits = 0;
    LayOutHooks();
    g_hooksActive = 1;
    ApplyDebugRegisters(true);
    g_cameraHooksArmed = true;
    if (widenFov) g_log("[camera hooks] on, wide culling %.0f deg", wideFovDegrees);
    else          g_log("[camera hooks] on (no FOV change)");
    return true;
}

void DisarmCameraHooks() {
    if (!g_cameraHooksArmed) return;
    g_accessCaptureActive = false;
    g_followObject = 0;
    g_hooksActive = 0;
    ApplyDebugRegisters(false);
    g_hookCount = 0;
    g_cameraHooksArmed = false;
    g_log("[camera hooks] off");
}

void Tick() {
    if (g_cameraHooksArmed) {
        if (GetTickCount() - g_lastApplyTick > ReapplyIntervalMs) ApplyDebugRegisters(true);
        return;
    }
    if (!g_widening) return;
    LogNewWriters();
    if (GetTickCount() - g_lastApplyTick > ReapplyIntervalMs) ApplyDebugRegisters(true);
}

void LogCullingHookHits() {
    if (!g_cameraHooksArmed) return;
    for (volatile LONG& hits : g_hookHits) hits = 0;
    g_log("[camera hooks] head follow writes %ld, game camera updates %ld, crowd facing writes %ld",
          InterlockedExchange(&g_followWrites, 0), InterlockedExchange(&g_scriptedUpdates, 0), InterlockedExchange(&g_crowdFollowWrites, 0));
}

static bool IsWritableMemory(uintptr_t address, size_t length) {
    MEMORY_BASIC_INFORMATION information;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &information, sizeof(information))) return false;
    if (information.State != MEM_COMMIT) return false;
    if (!(information.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) return false;
    if (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(information.BaseAddress) + information.RegionSize;
    return uint64_t(address) + length <= regionEnd;
}

// Views the renderer set up (object = the view), each with its camera's pose as the game set it.
int SeenCameras(CameraObjectSnapshot* out, int maximum) {
    const LONG count = std::min<LONG>(LONG(g_viewCount), LONG(MaximumViews));
    int written = 0;
    for (LONG index = 0; index < count && written < maximum; ++index) {
        const ViewRecord& record = g_views[index];
        if (!record.view) continue;
        CameraObjectSnapshot& snapshot = out[written++];
        snapshot.object = record.view;
        snapshot.hits = record.hits;
        memcpy(snapshot.matrix, record.originalPose, sizeof(snapshot.matrix));
        memset(snapshot.raw, 0, sizeof(snapshot.raw));
        const uintptr_t camera = record.camera;
        if (camera && IsWritableMemory(camera, CameraDumpBytes))
            memcpy(snapshot.raw, reinterpret_cast<const void*>(camera), sizeof(snapshot.raw));
    }
    return written;
}

void ForgetCameras() {
    for (ViewRecord& record : g_views) { record.view = 0; record.camera = 0; record.hits = 0; }
    g_viewCount = 0;
}

// Follow this view (0 = stop), putting matrix (the packed head pose) into its camera.
void SetFollowCamera(uintptr_t view, const float matrix[16]) {
    if (!view || !matrix) {
        g_followMatrixValid = 0;
        g_followObject = 0;
        g_followedViewCamera = 0;
        g_scriptedValid = 0;
        return;
    }
    if (view != g_followObject) {
        g_followMatrixValid = 0;
        g_scriptedValid = 0;
        g_followedViewCamera = 0;
        memset(g_lastWrittenMatrix, 0, sizeof(g_lastWrittenMatrix));
        g_followObject = view;
    }
    memcpy(g_followMatrix, matrix, sizeof(g_followMatrix));
    g_followMatrixValid = 1;
}

uintptr_t FollowedCamera() { return g_followObject; }

void SetFollowFov(float degrees) { g_followFovDegrees = degrees; }

void SetCrowdFollow(bool enabled) { g_crowdFollowEnabled = enabled ? 1 : 0; }

bool ScriptedCameraMatrix(float matrix[16]) {
    if (!g_followObject || !g_scriptedValid) return false;
    memcpy(matrix, g_scriptedMatrix, sizeof(g_scriptedMatrix));
    return true;
}

uintptr_t FollowedViewCamera() { return g_followedViewCamera; }

// Diagnostics: record every piece of game code that reads or writes these three addresses.
void StartAccessCapture(uintptr_t first, uintptr_t second, uintptr_t third) {
    if (!g_cameraHooksArmed || g_accessCaptureActive) return;
    g_writerCount = 0;
    g_accessAddresses[0] = first;
    g_accessAddresses[1] = second;
    g_accessAddresses[2] = third;
    g_accessCaptureActive = true;
    LayOutHooks();
    ApplyDebugRegisters(true);
    g_log("[access capture] watching %08X %08X %08X", unsigned(first), unsigned(second), unsigned(third));
}

void StopAccessCapture() {
    if (!g_accessCaptureActive) return;
    g_accessCaptureActive = false;
    LayOutHooks();
    ApplyDebugRegisters(true);
    const LONG count = std::min<LONG>(LONG(g_writerCount), LONG(MaximumWriterRecords));
    g_log("[access capture] %ld different code locations touched them:", count);
    for (LONG index = 0; index < count; ++index) {
        const WriterRecord& record = g_writers[index];
        char instructionText[MAX_PATH + 32];
        DescribeAddress(record.instruction, instructionText, sizeof(instructionText));
        uint8_t codeBytes[16] = {};
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(record.instruction - 12), codeBytes, sizeof(codeBytes), nullptr);
        char hexText[16 * 3 + 8] = "";
        for (int byteIndex = 0; byteIndex < 16; ++byteIndex) {
            char part[4];
            snprintf(part, sizeof(part), byteIndex == 12 ? "|%02X" : " %02X", codeBytes[byteIndex]);
            strcat(hexText, part);
        }
        const int watched = record.target == g_accessAddresses[0] ? 0 : record.target == g_accessAddresses[1] ? 1 : 2;
        g_log("[access capture]   #%d %8ld hits  code before %-28s bytes:%s", watched, long(record.hits), instructionText, hexText);
    }
}

bool AccessCaptureActive() { return g_accessCaptureActive; }

void SetDrawCounter(volatile unsigned* counter) { g_drawCounter = counter; }

// One frame's view setups: call at the start of the frame, then LogViewTrace at its end.
void StartViewTrace() {
    g_traceCount = 0;
    g_traceActive = 1;
}

void LogViewTrace(unsigned totalDraws) {
    if (!g_traceActive) return;
    g_traceActive = 0;
    const LONG count = std::min<LONG>(LONG(g_traceCount), LONG(MaximumTracedSetups));
    g_log("[render loop] one frame: %ld view setups, %u draws in total", count, totalDraws);
    for (LONG index = 0; index < count; ++index)
        g_log("[render loop]   view %08X set up before draw %u%s", unsigned(g_trace[index].view), g_trace[index].drawIndex,
              g_trace[index].view == g_followObject ? "  (main view)" : "");
    const LONG viewCount = std::min<LONG>(LONG(g_viewCount), LONG(MaximumViews));
    for (LONG index = 0; index < viewCount; ++index) {
        const ViewRecord& record = g_views[index];
        if (!record.view) continue;
        char text[256];
        int length = snprintf(text, sizeof(text), "[render loop] view %08X (%ld setups) stack:", unsigned(record.view), long(record.hits));
        for (int caller = 0; caller < 12 && record.callers[caller]; ++caller)
            length += snprintf(text + length, sizeof(text) - length, " %06X", unsigned(record.callers[caller]));
        g_log("%s", text);
    }
}

bool CameraHooksArmed() { return g_cameraHooksArmed; }

bool IsWidening() { return g_widening; }
const char* StatusText() { return g_status; }

} // namespace CameraScan
