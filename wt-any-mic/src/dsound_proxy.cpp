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
// GHWT imports DirectSound by ordinal, so the .def must keep the real
// dsound.dll ordinal numbers.

#include <windows.h>

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

RealDirectSound realDirectSound;
INIT_ONCE       loadOnce = INIT_ONCE_STATIC_INIT;

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

} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateA(EnumCallbackA callback, void* context) {
    void* returnAddress = CALLER_ADDRESS();
    if (!real().captureEnumerateA) {
        return ErrorNoRealDll;
    }
    if (!callback || !calledFromGameExe(returnAddress)) {
        return real().captureEnumerateA(callback, context);
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
