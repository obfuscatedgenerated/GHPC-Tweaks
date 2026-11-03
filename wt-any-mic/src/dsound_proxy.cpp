// DirectSound proxy for Guitar Hero World Tour PC.
// Lets any microphone be used for vocals.
//
// Why GHWT rejects most mics: it lists capture devices with
// DirectSoundCaptureEnumerateA and only keeps ones whose name contains
// "Logitech USB Mic", "Headset (XBOX 360 For Windows)", "PDP USB Microphone",
// "AK5370" or "Mad Catz USB microphone". That's why renaming a mic in Windows
// works.
//
// Fix: when GHWT.exe itself enumerates capture devices, it is shown a single
// device: the Windows default recording device (DSDEVID_DefaultCapture), named
// "Logitech USB Mic". DirectSoundCaptureCreate8 accepts that GUID, so the game
// opens whatever is set as default in Windows Sound settings. If there is no
// recording device at all, nothing is reported, same as before.
//
// Calls from anything other than GHWT.exe (FMOD, Bink, overlays) and every
// other DirectSound export pass straight through to the real dsound.dll.
//
// Toggle: with a mic present GHWT won't let the keyboard play the other
// instruments, so a key (Ctrl+M by default) switches the mic off and on at
// runtime. While off, the game is shown no capture devices at all. After each
// toggle the game window is sent WM_DEVICECHANGE / DBT_DEVNODES_CHANGED, which
// is what makes GHWT re-scan its mics (its window procedure sets the rescan
// flag on that message), so the change applies immediately.
//
// Settings: optional wt-any-mic.ini next to the DLL, see README.
//
// GHWT imports DirectSound by ordinal, so the .def must keep the real
// dsound.dll ordinal numbers.

#include <windows.h>
#include <wchar.h>

#ifdef _MSC_VER
#include <intrin.h>
#define CALLER_ADDRESS() _ReturnAddress()
#else
#define CALLER_ADDRESS() __builtin_return_address(0)
#endif

namespace {

// {DEF00001-9C6D-47ED-AAF1-4DDA8F2B5C03}
const GUID DefaultCaptureDevice = {0xdef00001, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
const char ReportedMicName[] = "Logitech USB Mic";

using EnumCallbackA            = BOOL(CALLBACK*)(GUID*, LPCSTR, LPCSTR, void*);
using CreateFn                 = HRESULT(WINAPI*)(const GUID*, void**, IUnknown*);
using EnumerateAFn             = HRESULT(WINAPI*)(EnumCallbackA, void*);
using EnumerateWFn             = HRESULT(WINAPI*)(void*, void*);
using CanUnloadNowFn           = HRESULT(WINAPI*)();
using GetClassObjectFn         = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
using GetDeviceIdFn            = HRESULT(WINAPI*)(const GUID*, GUID*);
using FullDuplexCreateFn       = HRESULT(WINAPI*)(const GUID*, const GUID*, const void*, const void*, HWND, DWORD,
                                                  void**, void**, void**, IUnknown*);

struct RealDirectSound {
    CreateFn           create                = nullptr; // ordinal 1
    EnumerateAFn       enumerateA            = nullptr; // ordinal 2
    EnumerateWFn       enumerateW            = nullptr; // ordinal 3
    CanUnloadNowFn     canUnloadNow          = nullptr; // ordinal 4
    GetClassObjectFn   getClassObject        = nullptr; // ordinal 5
    CreateFn           captureCreate         = nullptr; // ordinal 6
    EnumerateAFn       captureEnumerateA     = nullptr; // ordinal 7
    EnumerateWFn       captureEnumerateW     = nullptr; // ordinal 8
    GetDeviceIdFn      getDeviceId           = nullptr; // ordinal 9
    FullDuplexCreateFn fullDuplexCreate      = nullptr; // ordinal 10
    CreateFn           create8               = nullptr; // ordinal 11
    CreateFn           captureCreate8        = nullptr; // ordinal 12
};

// Same format as WT:VR's vr.ini [keys]: "Ctrl+Shift+Alt+Key", and a press only
// counts with exactly those modifiers held. Ctrl+M is free in WT:VR.
const wchar_t   DefaultToggleKey[]  = L"Ctrl+M";
constexpr DWORD KeyPollIntervalMs   = 30;
constexpr WPARAM DeviceNodesChanged = 0x0007; // DBT_DEVNODES_CHANGED

RealDirectSound realDirectSound;
INIT_ONCE       loadOnce = INIT_ONCE_STATIC_INIT;
HMODULE         proxyModule = nullptr;
volatile LONG   micEnabled = 1;

struct KeyBinding {
    UINT key     = 0; // virtual key, 0 = none
    bool control = false;
    bool shift   = false;
    bool alt     = false;
};

struct Settings {
    KeyBinding toggleKey;
    bool       startOn     = true;
    bool       toggleSound = true;
};

template <typename FunctionType>
FunctionType lookup(HMODULE module, LPCSTR name) {
    return reinterpret_cast<FunctionType>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

BOOL CALLBACK loadRealDirectSound(INIT_ONCE*, void*, void**) {
    wchar_t path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + 12 >= MAX_PATH) {
        return TRUE;
    }
    lstrcatW(path, L"\\dsound.dll");
    HMODULE module = LoadLibraryW(path);
    if (!module) {
        return TRUE;
    }
    realDirectSound.create            = lookup<CreateFn>(module, "DirectSoundCreate");
    realDirectSound.enumerateA        = lookup<EnumerateAFn>(module, "DirectSoundEnumerateA");
    realDirectSound.enumerateW        = lookup<EnumerateWFn>(module, "DirectSoundEnumerateW");
    realDirectSound.canUnloadNow      = lookup<CanUnloadNowFn>(module, "DllCanUnloadNow");
    realDirectSound.getClassObject    = lookup<GetClassObjectFn>(module, "DllGetClassObject");
    realDirectSound.captureCreate     = lookup<CreateFn>(module, "DirectSoundCaptureCreate");
    realDirectSound.captureEnumerateA = lookup<EnumerateAFn>(module, "DirectSoundCaptureEnumerateA");
    realDirectSound.captureEnumerateW = lookup<EnumerateWFn>(module, "DirectSoundCaptureEnumerateW");
    realDirectSound.getDeviceId       = lookup<GetDeviceIdFn>(module, "GetDeviceID");
    realDirectSound.fullDuplexCreate  = lookup<FullDuplexCreateFn>(module, "DirectSoundFullDuplexCreate");
    realDirectSound.create8           = lookup<CreateFn>(module, "DirectSoundCreate8");
    realDirectSound.captureCreate8    = lookup<CreateFn>(module, "DirectSoundCaptureCreate8");
    return TRUE;
}

const RealDirectSound& real() {
    InitOnceExecuteOnce(&loadOnce, loadRealDirectSound, nullptr, nullptr);
    return realDirectSound;
}

bool calledFromGameExe(void* returnAddress) {
    HMODULE callerModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(returnAddress), &callerModule);
    return callerModule != nullptr && callerModule == GetModuleHandleW(nullptr);
}

struct GameEnumeration {
    EnumCallbackA gameCallback;
    void*         gameContext;
};

// DirectSound reports the default device first, as "Primary Sound Capture
// Driver" with a NULL GUID. Only that one is passed on, renamed and with a GUID
// the game can store and later hand to DirectSoundCaptureCreate8.
BOOL CALLBACK onlyDefaultDevice(GUID* deviceGuid, LPCSTR, LPCSTR moduleName, void* context) {
    if (deviceGuid != nullptr) {
        return TRUE;
    }
    auto* enumeration = static_cast<GameEnumeration*>(context);
    GUID defaultDevice = DefaultCaptureDevice;
    return enumeration->gameCallback(&defaultDevice, ReportedMicName, moduleName, enumeration->gameContext);
}

constexpr HRESULT ErrorNoRealDll = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

// A-Z, 0-9, F1-F24, a few named keys, or a virtual-key code ("0x77").
UINT virtualKeyFromName(const wchar_t* name) {
    if (name[0] && !name[1]) {
        wchar_t character = static_cast<wchar_t>(towupper(name[0]));
        if ((character >= L'A' && character <= L'Z') || (character >= L'0' && character <= L'9')) {
            return character;
        }
        return 0;
    }
    if ((name[0] == L'F' || name[0] == L'f') && name[1] >= L'1' && name[1] <= L'9') {
        wchar_t* end = nullptr;
        unsigned long functionNumber = wcstoul(name + 1, &end, 10);
        return (*end == 0 && functionNumber >= 1 && functionNumber <= 24) ? VK_F1 + static_cast<UINT>(functionNumber - 1) : 0;
    }
    static const struct { const wchar_t* name; UINT key; } namedKeys[] = {
        {L"Home", VK_HOME}, {L"End", VK_END}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
        {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE}, {L"ScrollLock", VK_SCROLL},
    };
    for (const auto& namedKey : namedKeys) {
        if (lstrcmpiW(name, namedKey.name) == 0) {
            return namedKey.key;
        }
    }
    wchar_t* end = nullptr;
    unsigned long keyCode = wcstoul(name, &end, 0);
    return (end != name && *end == 0 && keyCode > 0 && keyCode < 0xFF) ? static_cast<UINT>(keyCode) : 0;
}

// "Ctrl+Shift+F7" -> key + modifiers. "none" gives no key. False if unreadable.
bool parseBinding(const wchar_t* text, KeyBinding& binding) {
    binding = KeyBinding();
    if (text[0] == 0 || lstrcmpiW(text, L"none") == 0) {
        return true;
    }
    wchar_t part[32];
    const wchar_t* partStart = text;
    for (const wchar_t* cursor = text;; ++cursor) {
        if (*cursor != L'+' && *cursor != 0) {
            continue;
        }
        while (partStart < cursor && *partStart == L' ') {
            ++partStart;
        }
        const wchar_t* partEnd = cursor;
        while (partEnd > partStart && partEnd[-1] == L' ') {
            --partEnd;
        }
        size_t partLength = static_cast<size_t>(partEnd - partStart);
        if (partLength == 0 || partLength >= ARRAYSIZE(part)) {
            return false;
        }
        lstrcpynW(part, partStart, static_cast<int>(partLength) + 1);
        if (*cursor == 0) {
            binding.key = virtualKeyFromName(part);
            return binding.key != 0;
        }
        if (lstrcmpiW(part, L"Ctrl") == 0) {
            binding.control = true;
        } else if (lstrcmpiW(part, L"Shift") == 0) {
            binding.shift = true;
        } else if (lstrcmpiW(part, L"Alt") == 0) {
            binding.alt = true;
        } else {
            return false;
        }
        partStart = cursor + 1;
    }
}

bool keyDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool modifiersMatch(const KeyBinding& binding) {
    return keyDown(VK_CONTROL) == binding.control && keyDown(VK_SHIFT) == binding.shift &&
           keyDown(VK_MENU) == binding.alt;
}

Settings loadSettings() {
    Settings settings;
    wchar_t iniPath[MAX_PATH];
    DWORD length = GetModuleFileNameW(proxyModule, iniPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return settings;
    }
    wchar_t* fileName = iniPath;
    for (wchar_t* cursor = iniPath; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            fileName = cursor + 1;
        }
    }
    const wchar_t iniName[] = L"wt-any-mic.ini";
    if ((fileName - iniPath) + ARRAYSIZE(iniName) > MAX_PATH) {
        return settings;
    }
    lstrcpyW(fileName, iniName);

    wchar_t keyText[64];
    GetPrivateProfileStringW(L"mic", L"toggle_key", DefaultToggleKey, keyText, ARRAYSIZE(keyText), iniPath);
    if (!parseBinding(keyText, settings.toggleKey)) {
        parseBinding(DefaultToggleKey, settings.toggleKey);
    }
    settings.startOn     = GetPrivateProfileIntW(L"mic", L"start_on", 1, iniPath) != 0;
    settings.toggleSound = GetPrivateProfileIntW(L"mic", L"toggle_sound", 1, iniPath) != 0;
    return settings;
}

HWND foregroundGameWindow() {
    HWND window = GetForegroundWindow();
    DWORD windowProcess = 0;
    if (window) {
        GetWindowThreadProcessId(window, &windowProcess);
    }
    return windowProcess == GetCurrentProcessId() ? window : nullptr;
}

DWORD WINAPI toggleKeyThread(void* settingsPointer) {
    const Settings settings = *static_cast<Settings*>(settingsPointer);
    delete static_cast<Settings*>(settingsPointer);

    bool wasDown = false;
    for (;;) {
        Sleep(KeyPollIntervalMs);
        bool isDown = keyDown(static_cast<int>(settings.toggleKey.key));
        bool pressed = isDown && !wasDown;
        wasDown = isDown;
        if (!pressed || !modifiersMatch(settings.toggleKey)) {
            continue;
        }
        HWND gameWindow = foregroundGameWindow();
        if (!gameWindow) {
            continue; // key pressed in another app
        }
        LONG nowEnabled = InterlockedXor(&micEnabled, 1) ^ 1;
        PostMessageW(gameWindow, WM_DEVICECHANGE, DeviceNodesChanged, 0);
        if (settings.toggleSound) {
            MessageBeep(nowEnabled ? MB_OK : MB_ICONHAND);
        }
    }
}

void startToggle() {
    Settings settings = loadSettings();
    InterlockedExchange(&micEnabled, settings.startOn ? 1 : 0);
    if (settings.toggleKey.key == 0) {
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, toggleKeyThread, new Settings(settings), 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

INIT_ONCE toggleOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK startToggleOnce(INIT_ONCE*, void*, void**) {
    startToggle();
    return TRUE;
}

} // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        proxyModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateA(EnumCallbackA callback, void* context) {
    void* returnAddress = CALLER_ADDRESS();
    if (!real().captureEnumerateA) {
        return ErrorNoRealDll;
    }
    if (!callback || !calledFromGameExe(returnAddress)) {
        return real().captureEnumerateA(callback, context);
    }
    InitOnceExecuteOnce(&toggleOnce, startToggleOnce, nullptr, nullptr);
    if (!micEnabled) {
        return S_OK; // enumerated fine, just nothing to report
    }
    GameEnumeration enumeration = {callback, context};
    return real().captureEnumerateA(onlyDefaultDevice, &enumeration);
}

HRESULT WINAPI Proxy_DirectSoundCreate(const GUID* device, void** directSound, IUnknown* outer) {
    return real().create ? real().create(device, directSound, outer) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DirectSoundEnumerateA(EnumCallbackA callback, void* context) {
    return real().enumerateA ? real().enumerateA(callback, context) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DirectSoundEnumerateW(void* callback, void* context) {
    return real().enumerateW ? real().enumerateW(callback, context) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    return real().canUnloadNow ? real().canUnloadNow() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID classId, REFIID interfaceId, void** object) {
    return real().getClassObject ? real().getClassObject(classId, interfaceId, object) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DirectSoundCaptureCreate(const GUID* device, void** capture, IUnknown* outer) {
    return real().captureCreate ? real().captureCreate(device, capture, outer) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateW(void* callback, void* context) {
    return real().captureEnumerateW ? real().captureEnumerateW(callback, context) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_GetDeviceID(const GUID* source, GUID* destination) {
    return real().getDeviceId ? real().getDeviceId(source, destination) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DirectSoundFullDuplexCreate(const GUID* captureDevice, const GUID* renderDevice,
                                                 const void* captureBufferDesc, const void* renderBufferDesc,
                                                 HWND window, DWORD level, void** fullDuplex,
                                                 void** captureBuffer, void** renderBuffer, IUnknown* outer) {
    if (!real().fullDuplexCreate) {
        return ErrorNoRealDll;
    }
    return real().fullDuplexCreate(captureDevice, renderDevice, captureBufferDesc, renderBufferDesc, window, level,
                                   fullDuplex, captureBuffer, renderBuffer, outer);
}

HRESULT WINAPI Proxy_DirectSoundCreate8(const GUID* device, void** directSound, IUnknown* outer) {
    return real().create8 ? real().create8(device, directSound, outer) : ErrorNoRealDll;
}

HRESULT WINAPI Proxy_DirectSoundCaptureCreate8(const GUID* device, void** capture, IUnknown* outer) {
    return real().captureCreate8 ? real().captureCreate8(device, capture, outer) : ErrorNoRealDll;
}

} // extern "C"
