// XInput proxy for Guitar Hero III PC.
// Makes Xbox 360 Rock Band guitars (e.g. the RB1 Stratocaster) work as Guitar
// Hero guitars.
//
// Why GH3 breaks: GH3 only uses its guitar input layout for the
// "Guitar Alternate" XInput subtype (0x07) that GH guitars report. RB guitars
// report plain "Guitar" (0x06), so GH3 falls back to its gamepad layout.
//
// Fix: report subtype 0x07 for RB guitars, and clear the inputs a GH guitar
// never sends (pickup selector on LT, solo-fret flag on left stick click).
//
// GH3 imports XInput by ordinal, so the .def must keep the real xinput1_3
// ordinal numbers.
//
// Steam overlay: it finds XInput by module name, so it patches a jump into
// this proxy's own exports and answers GH3 itself, skipping the remap. While a
// Rock Band guitar is connected, a guard thread puts the proxy's original entry
// bytes back. Only our own XInput exports are touched; the rest of the overlay
// (rendering, Remote Play streaming) is left alone.
//
// Debug: put an empty file named "xinput_proxy.debug" next to the DLL and it
// writes xinput_proxy.log there: what each controller reports, who calls
// XInput, every raw button/axis change, and where GH3's XInput calls go.

#include <windows.h>
#include <stdarg.h>

#ifdef _MSC_VER
#include <intrin.h>
#define CALLER_ADDRESS() _ReturnAddress()
#else
#define CALLER_ADDRESS() __builtin_return_address(0)
#endif

namespace {

struct XInputGamepad {
    WORD  buttons;
    BYTE  leftTrigger;
    BYTE  rightTrigger;
    SHORT thumbLeftX;
    SHORT thumbLeftY;
    SHORT thumbRightX;
    SHORT thumbRightY;
};

struct XInputState {
    DWORD         packetNumber;
    XInputGamepad gamepad;
};

struct XInputVibration {
    WORD leftMotorSpeed;
    WORD rightMotorSpeed;
};

struct XInputCapabilities {
    BYTE            type;
    BYTE            subType;
    WORD            flags;
    XInputGamepad   gamepad;
    XInputVibration vibration;
};

struct XInputBatteryInformation {
    BYTE batteryType;
    BYTE batteryLevel;
};

struct XInputKeystroke {
    WORD  virtualKey;
    WCHAR unicode;
    WORD  flags;
    BYTE  userIndex;
    BYTE  hidCode;
};

constexpr BYTE  SubtypeGuitar            = 0x06; // Rock Band guitars
constexpr BYTE  SubtypeGuitarAlternate   = 0x07; // Guitar Hero guitars
constexpr WORD  ButtonLeftThumb          = 0x0040; // RB solo-fret flag
constexpr DWORD MaxControllers           = 4;
constexpr DWORD MaxLoggedChanges         = 5000;
constexpr LONG  MaxLoggedCallsPerCounter = 10;

using GetStateFn              = DWORD(WINAPI*)(DWORD, XInputState*);
using SetStateFn              = DWORD(WINAPI*)(DWORD, XInputVibration*);
using GetCapabilitiesFn       = DWORD(WINAPI*)(DWORD, DWORD, XInputCapabilities*);
using EnableFn                = void(WINAPI*)(BOOL);
using GetAudioGuidsFn         = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
using GetBatteryInfoFn        = DWORD(WINAPI*)(DWORD, BYTE, XInputBatteryInformation*);
using GetKeystrokeFn          = DWORD(WINAPI*)(DWORD, DWORD, XInputKeystroke*);
using WaitForGuideButtonFn    = DWORD(WINAPI*)(DWORD, DWORD, void*);
using CancelGuideButtonWaitFn = DWORD(WINAPI*)(DWORD);
using PowerOffControllerFn    = DWORD(WINAPI*)(DWORD);

struct RealXInput {
    GetStateFn              getState              = nullptr;
    GetStateFn              getStateEx            = nullptr; // ordinal 100
    SetStateFn              setState              = nullptr;
    GetCapabilitiesFn       getCapabilities       = nullptr;
    EnableFn                enable                = nullptr;
    GetAudioGuidsFn         getAudioGuids         = nullptr;
    GetBatteryInfoFn        getBatteryInfo        = nullptr;
    GetKeystrokeFn          getKeystroke          = nullptr;
    WaitForGuideButtonFn    waitForGuideButton    = nullptr; // ordinal 101
    CancelGuideButtonWaitFn cancelGuideButtonWait = nullptr; // ordinal 102
    PowerOffControllerFn    powerOffController    = nullptr; // ordinal 103
};

enum class PadKind : BYTE { Unknown, RockBandGuitar, Other };

enum CallCounter { CounterGetState, CounterGetStateEx, CounterGetCapabilities, CounterOther, CounterCount };

HMODULE          proxyModule = nullptr;
RealXInput       realXInput;
INIT_ONCE        loadOnce = INIT_ONCE_STATIC_INIT;
PadKind          padKinds[MaxControllers] = {};
XInputGamepad    lastRawGamepads[MaxControllers] = {};
CRITICAL_SECTION logLock;
HANDLE           logFile = INVALID_HANDLE_VALUE;
DWORD            loggedChanges = 0;
LONG             callCounts[CounterCount] = {};

template <typename FunctionType>
FunctionType lookup(HMODULE module, LPCSTR nameOrOrdinal) {
    return reinterpret_cast<FunctionType>(reinterpret_cast<void*>(GetProcAddress(module, nameOrOrdinal)));
}

template <typename CharType>
const CharType* fileNameOf(const CharType* path) {
    const CharType* fileName = path;
    for (const CharType* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            fileName = cursor + 1;
        }
    }
    return fileName;
}

// Logging is off unless an "xinput_proxy.debug" file sits next to the DLL.
void openLog() {
    wchar_t logPath[MAX_PATH];
    DWORD pathLength = GetModuleFileNameW(proxyModule, logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        return;
    }
    wchar_t* fileName = const_cast<wchar_t*>(fileNameOf(logPath));
    *fileName = L'\0';
    if (lstrlenW(logPath) + lstrlenW(L"xinput_proxy.debug") >= MAX_PATH) {
        return;
    }

    lstrcatW(logPath, L"xinput_proxy.debug");
    bool debugEnabled = GetFileAttributesW(logPath) != INVALID_FILE_ATTRIBUTES;
    *fileName = L'\0';
    if (!debugEnabled) {
        return;
    }
    lstrcatW(logPath, L"xinput_proxy.log");
    logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void logLine(const char* format, ...) {
    if (logFile == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    int length = wvsprintfA(line, format, arguments);
    va_end(arguments);
    if (length <= 0 || length > 1020) {
        return;
    }
    line[length++] = '\r';
    line[length++] = '\n';

    // No flush here: this runs on GH3's input thread, and waiting on the disk
    // stalls the game. The guard thread flushes in the background instead.
    EnterCriticalSection(&logLock);
    DWORD written = 0;
    WriteFile(logFile, line, static_cast<DWORD>(length), &written, nullptr);
    LeaveCriticalSection(&logLock);
}

void flushLog() {
    if (logFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(logFile);
    }
}

BOOL CALLBACK loadRealXInput(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t ownPath[MAX_PATH];
    if (!GetModuleFileNameW(proxyModule, ownPath, MAX_PATH)) {
        logLine("ERROR: GetModuleFileName failed (%lu)", GetLastError());
        return TRUE;
    }
    const wchar_t* ownFileName = fileNameOf(ownPath);

    wchar_t realPath[MAX_PATH];
    UINT systemDirLength = GetSystemDirectoryW(realPath, MAX_PATH);
    if (systemDirLength == 0 || systemDirLength + 2 + lstrlenW(ownFileName) >= MAX_PATH) {
        logLine("ERROR: could not build system XInput path");
        return TRUE;
    }
    lstrcatW(realPath, L"\\");
    lstrcatW(realPath, ownFileName);

    HMODULE realModule = LoadLibraryW(realPath);
    if (!realModule) {
        logLine("ERROR: could not load real %ls (%lu)", realPath, GetLastError());
        return TRUE;
    }
    logLine("Loaded real %ls", realPath);

    realXInput.getState              = lookup<GetStateFn>(realModule, "XInputGetState");
    realXInput.getStateEx            = lookup<GetStateFn>(realModule, MAKEINTRESOURCEA(100));
    realXInput.setState              = lookup<SetStateFn>(realModule, "XInputSetState");
    realXInput.getCapabilities       = lookup<GetCapabilitiesFn>(realModule, "XInputGetCapabilities");
    realXInput.enable                = lookup<EnableFn>(realModule, "XInputEnable");
    realXInput.getAudioGuids         = lookup<GetAudioGuidsFn>(realModule, "XInputGetDSoundAudioDeviceGuids");
    realXInput.getBatteryInfo        = lookup<GetBatteryInfoFn>(realModule, "XInputGetBatteryInformation");
    realXInput.getKeystroke          = lookup<GetKeystrokeFn>(realModule, "XInputGetKeystroke");
    realXInput.waitForGuideButton    = lookup<WaitForGuideButtonFn>(realModule, MAKEINTRESOURCEA(101));
    realXInput.cancelGuideButtonWait = lookup<CancelGuideButtonWaitFn>(realModule, MAKEINTRESOURCEA(102));
    realXInput.powerOffController    = lookup<PowerOffControllerFn>(realModule, MAKEINTRESOURCEA(103));
    return TRUE;
}

const RealXInput& real() {
    InitOnceExecuteOnce(&loadOnce, loadRealXInput, nullptr, nullptr);
    return realXInput;
}

void moduleNameAt(const void* address, char* buffer, DWORD bufferSize) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)
        && GetModuleFileNameA(module, buffer, bufferSize)) {
        return;
    }
    lstrcpynA(buffer, "unknown", static_cast<int>(bufferSize));
}

void logCall(CallCounter counter, const char* functionName, DWORD userIndex, DWORD result, void* returnAddress) {
    if (logFile == INVALID_HANDLE_VALUE || InterlockedIncrement(&callCounts[counter]) > MaxLoggedCallsPerCounter) {
        return;
    }
    char caller[MAX_PATH];
    moduleNameAt(returnAddress, caller, MAX_PATH);
    logLine("Call %s(pad %lu) = %lu from %s", functionName, userIndex, result, caller);
}

void rememberPadKind(DWORD userIndex, const XInputCapabilities& capabilities) {
    PadKind kind = capabilities.subType == SubtypeGuitar ? PadKind::RockBandGuitar : PadKind::Other;
    if (padKinds[userIndex] != kind) {
        logLine("Pad %lu: type=0x%02X subtype=0x%02X flags=0x%04X -> %s",
                userIndex, capabilities.type, capabilities.subType, capabilities.flags,
                kind == PadKind::RockBandGuitar ? "Rock Band guitar, remapping" : "left alone");
    }
    padKinds[userIndex] = kind;
}

PadKind detectPad(DWORD userIndex) {
    if (userIndex >= MaxControllers) {
        return PadKind::Other;
    }
    if (padKinds[userIndex] == PadKind::Unknown && real().getCapabilities) {
        XInputCapabilities capabilities = {};
        if (real().getCapabilities(userIndex, 0, &capabilities) == ERROR_SUCCESS) {
            rememberPadKind(userIndex, capabilities);
        }
    }
    return padKinds[userIndex];
}

bool anyRockBandGuitarConnected() {
    for (DWORD userIndex = 0; userIndex < MaxControllers; ++userIndex) {
        if (detectPad(userIndex) == PadKind::RockBandGuitar) {
            return true;
        }
    }
    return false;
}

void logRawChange(DWORD userIndex, const XInputGamepad& gamepad) {
    XInputGamepad& last = lastRawGamepads[userIndex];
    bool changed = gamepad.buttons != last.buttons
        || gamepad.leftTrigger != last.leftTrigger
        || gamepad.rightTrigger != last.rightTrigger
        || gamepad.thumbLeftX != last.thumbLeftX
        || gamepad.thumbLeftY != last.thumbLeftY
        || gamepad.thumbRightX != last.thumbRightX
        || gamepad.thumbRightY != last.thumbRightY;
    if (!changed || loggedChanges >= MaxLoggedChanges) {
        return;
    }
    ++loggedChanges;
    last = gamepad;
    logLine("Pad %lu raw: buttons=0x%04X LT=%3u RT=%3u LX=%6d LY=%6d RX=%6d RY=%6d",
            userIndex, gamepad.buttons, gamepad.leftTrigger, gamepad.rightTrigger,
            gamepad.thumbLeftX, gamepad.thumbLeftY, gamepad.thumbRightX, gamepad.thumbRightY);
}

void convertRockBandToGuitarHero(XInputGamepad& gamepad) {
    gamepad.leftTrigger = 0;               // pickup selector
    gamepad.buttons &= ~ButtonLeftThumb;   // solo-fret flag
}

DWORD getStateWith(GetStateFn realGetState, DWORD userIndex, XInputState* state) {
    if (!realGetState) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    DWORD result = realGetState(userIndex, state);
    if (result != ERROR_SUCCESS) {
        if (userIndex < MaxControllers) {
            padKinds[userIndex] = PadKind::Unknown; // re-detect on reconnect
        }
        return result;
    }
    if (!state || userIndex >= MaxControllers) {
        return result;
    }

    logRawChange(userIndex, state->gamepad);
    if (detectPad(userIndex) == PadKind::RockBandGuitar) {
        convertRockBandToGuitarHero(state->gamepad);
    }
    return result;
}

} // namespace

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD userIndex, XInputState* state) {
    DWORD result = getStateWith(real().getState, userIndex, state);
    logCall(CounterGetState, "XInputGetState", userIndex, result, CALLER_ADDRESS());
    return result;
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD userIndex, XInputState* state) {
    GetStateFn realGetState = real().getStateEx ? real().getStateEx : real().getState;
    DWORD result = getStateWith(realGetState, userIndex, state);
    logCall(CounterGetStateEx, "XInputGetStateEx", userIndex, result, CALLER_ADDRESS());
    return result;
}

DWORD WINAPI Proxy_XInputSetState(DWORD userIndex, XInputVibration* vibration) {
    return real().setState ? real().setState(userIndex, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD userIndex, DWORD flags, XInputCapabilities* capabilities) {
    if (!real().getCapabilities) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    DWORD result = real().getCapabilities(userIndex, flags, capabilities);
    logCall(CounterGetCapabilities, "XInputGetCapabilities", userIndex, result, CALLER_ADDRESS());
    if (result != ERROR_SUCCESS || !capabilities) {
        return result;
    }

    if (userIndex < MaxControllers) {
        rememberPadKind(userIndex, *capabilities);
    }
    if (capabilities->subType == SubtypeGuitar) {
        capabilities->subType = SubtypeGuitarAlternate;
        capabilities->gamepad.leftTrigger = 0;
    }
    return result;
}

void WINAPI Proxy_XInputEnable(BOOL enable) {
    logCall(CounterOther, "XInputEnable", static_cast<DWORD>(enable), 0, CALLER_ADDRESS());
    if (real().enable) {
        real().enable(enable);
    }
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD userIndex, GUID* renderGuid, GUID* captureGuid) {
    return real().getAudioGuids ? real().getAudioGuids(userIndex, renderGuid, captureGuid) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD userIndex, BYTE deviceType, XInputBatteryInformation* batteryInfo) {
    return real().getBatteryInfo ? real().getBatteryInfo(userIndex, deviceType, batteryInfo) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD userIndex, DWORD reserved, XInputKeystroke* keystroke) {
    return real().getKeystroke ? real().getKeystroke(userIndex, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD userIndex, DWORD flags, void* listenState) {
    return real().waitForGuideButton ? real().waitForGuideButton(userIndex, flags, listenState) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD userIndex) {
    return real().cancelGuideButtonWait ? real().cancelGuideButtonWait(userIndex) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputPowerOffController(DWORD userIndex) {
    return real().powerOffController ? real().powerOffController(userIndex) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"

// Export guard: undoes jumps that overlays (Steam) patch into this proxy's own
// exports, so GH3's calls reach the remap again.
namespace {

constexpr DWORD GuardIntervalMs      = 250;
constexpr DWORD GuardTicksPerFlush   = 4; // flush the debug log once a second
constexpr LONG  MaxLoggedRestores    = 20;
constexpr DWORD GuardedEntryCount    = 3;
constexpr DWORD GuardedByteCount     = sizeof(ULONGLONG);

struct GuardedEntry {
    const char* name;
    BYTE*       function;
    ULONGLONG   originalBytes;
};

GuardedEntry guardedEntries[GuardedEntryCount];
LONG         restoreCount = 0;

void captureEntry(GuardedEntry& entry, const char* name, void* function) {
    entry.name = name;
    entry.function = static_cast<BYTE*>(function);
    CopyMemory(&entry.originalBytes, entry.function, GuardedByteCount);
}

bool restoreEntry(GuardedEntry& entry) {
    ULONGLONG currentBytes;
    CopyMemory(&currentBytes, entry.function, GuardedByteCount);
    if (currentBytes == entry.originalBytes) {
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(entry.function, GuardedByteCount, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }
    // One atomic 8-byte write when aligned, so GH3 never runs half-restored code.
    if ((reinterpret_cast<ULONG_PTR>(entry.function) % GuardedByteCount) == 0) {
        InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(entry.function),
                                     static_cast<LONGLONG>(entry.originalBytes),
                                     static_cast<LONGLONG>(currentBytes));
    } else {
        CopyMemory(entry.function, &entry.originalBytes, GuardedByteCount);
    }
    VirtualProtect(entry.function, GuardedByteCount, oldProtection, &oldProtection);
    FlushInstructionCache(GetCurrentProcess(), entry.function, GuardedByteCount);
    return true;
}

DWORD WINAPI guardExports(LPVOID) {
    for (DWORD tick = 1;; ++tick) {
        Sleep(GuardIntervalMs);
        if (tick % GuardTicksPerFlush == 0) {
            flushLog();
        }
        if (!anyRockBandGuitarConnected()) {
            continue; // GH guitars and pads work fine through the overlay's hooks
        }
        for (GuardedEntry& entry : guardedEntries) {
            if (restoreEntry(entry) && InterlockedIncrement(&restoreCount) <= MaxLoggedRestores) {
                logLine("Removed overlay hook from proxy %s", entry.name);
            }
        }
    }
}

void startExportGuard() {
    captureEntry(guardedEntries[0], "XInputGetState", reinterpret_cast<void*>(&Proxy_XInputGetState));
    captureEntry(guardedEntries[1], "XInputGetStateEx", reinterpret_cast<void*>(&Proxy_XInputGetStateEx));
    captureEntry(guardedEntries[2], "XInputGetCapabilities", reinterpret_cast<void*>(&Proxy_XInputGetCapabilities));
    HANDLE guardThread = CreateThread(nullptr, 0, guardExports, nullptr, 0, nullptr);
    if (guardThread) {
        CloseHandle(guardThread);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        proxyModule = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&logLock);
        openLog();
        logLine("GH3 RB guitar proxy loaded");
        startExportGuard(); // capture entry bytes now, before any overlay patches them
    }
    return TRUE;
}
