// OpenXR session for WT:VR.
//
// OpenXR can't take Direct3D 9 images, so this owns a small Direct3D 11 device that the
// runtime uses. Eye and overlay images are handed over as CPU pixel buffers (BGRA) and
// uploaded into the runtime's swapchains.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

using LogFunction = void (*)(const char* format, ...);

struct EyeView {
    XrPosef pose;
    XrFovf  fov;
};

struct OverlayPlacement {
    float distance;  // metres in front of the recentred head
    float width;     // metres
    float height;    // metres above (+) / below (-) eye level
};

class OpenXrSession {
public:
    static constexpr int EyeCount = 2;

    // Creates instance, system, D3D11 device, session and swapchains.
    // Safe to call repeatedly (e.g. headset not connected yet); returns true once ready.
    bool TryInitialize(const std::string& loaderPath, UINT overlayWidth, UINT overlayHeight, float renderScale, LogFunction log);
    void Shutdown();

    // After repeated failures initialisation stops (so a broken runtime isn't hammered).
    bool HasGivenUp() const { return m_gaveUp; }
    void RetryInitialization() { m_gaveUp = false; m_failedAttempts = 0; m_nextAttemptTick = 0; }

    void PollEvents();
    bool IsRunning() const { return m_sessionRunning; }

    // xrWaitFrame + xrBeginFrame + xrLocateViews. Blocks to the headset's frame rate.
    bool BeginFrame();
    bool FrameBegun() const { return m_frameBegun; }
    bool ViewsValid() const { return m_viewsValid; }
    const EyeView& CurrentView(int eye) const { return m_currentViews[eye]; }

    // pixels are BGRA rows with the given pitch, sized EyeWidth() x EyeHeight().
    // forceOpaque: overwrite alpha with 255 (sources without a real alpha channel).
    void SubmitEyeImage(int eye, const void* pixels, int pitch, const EyeView& renderedView, bool forceOpaque);
    // pixels are premultiplied BGRA, sized OverlayWidth() x OverlayHeight().
    // layer 0: the HUD panel. 1..HudLayerCount: the 3D HUD's extra panels, each shown lift metres
    // in front of the HUD panel to one eye or both (SetHudLayer).
    void SubmitOverlayImage(const void* pixels, int pitch, bool forceOpaque, int layer = 0);
    // eye: 0 left only, 1 right only, -1 both.
    void SetHudLayer(int index, float lift, int eye) { m_hudLayerLifts[index] = lift; m_hudLayerEyes[index] = eye; }
    // The part of the HUD panel the extra panels cover (fractions of its width / height: left, top,
    // right, bottom). Set before initialising; their images are that part's size.
    void SetHudLayerArea(float left, float top, float right, float bottom) { m_hudLayerArea[0] = left; m_hudLayerArea[1] = top; m_hudLayerArea[2] = right; m_hudLayerArea[3] = bottom; }
    UINT HudLayerWidth() const { return m_hudLayerWidth; }
    UINT HudLayerHeight() const { return m_hudLayerHeight; }
    static constexpr int HudLayerCount = 2;

    // Diagnostic: fill the eyes and panel with solid colours drawn by the GPU (no copying),
    // to check whether the runtime displays anything we send.
    void SubmitTestPattern();
    // showProjection: the 3D eye images. overlayOpaque: the panel image has no usable alpha
    // (whole game screen in menus) and is shown as a solid screen.
    void EndFrame(const OverlayPlacement& overlay, bool showProjection, bool showOverlay, bool overlayOpaque);

    // Make the current head position and facing the origin of the play space.
    void RequestRecenter() { m_recenterRequested = true; }
    bool RecenterPending() const { return m_recenterRequested; }

    // Diagnostics
    uint32_t LastLayerCount() const { return m_lastLayerCount; }
    bool EyeImageSubmitted(int eye) const { return m_eyeImageSubmitted[eye]; }
    bool LastShouldRender() const { return m_frameState.shouldRender == XR_TRUE; }
    int  SessionState() const { return int(m_sessionState); }

    UINT EyeWidth() const { return m_eyeWidth; }
    UINT EyeHeight() const { return m_eyeHeight; }
    UINT OverlayWidth() const { return m_overlayWidth; }
    UINT OverlayHeight() const { return m_overlayHeight; }

private:
    bool LoadLoader(const std::string& loaderPath);
    bool CreateInstanceAndSystem();
    bool CreateD3D11Device();
    bool CreateSessionAndSpaces();
    bool CreateSwapchains(float renderScale);
    bool CreateSwapchain(UINT width, UINT height, XrSwapchain& swapchain, std::vector<ID3D11Texture2D*>& images);
    bool FindWorkingSwapchainSetup(const std::vector<int64_t>& offeredFormats);
    void HandleFailedAttempt();
    void UploadToSwapchain(XrSwapchain swapchain, const std::vector<ID3D11Texture2D*>& images,
                           const void* pixels, int pitch, UINT width, UINT height, bool forceOpaque);
    void ClearSwapchain(XrSwapchain swapchain, const std::vector<ID3D11Texture2D*>& images, const float color[4]);
    void ApplyRecenter(XrTime time);
    bool Check(XrResult result, const char* what);

    LogFunction m_log = nullptr;
    HMODULE m_loader = nullptr;
    DWORD m_nextAttemptTick = 0;
    int  m_failedAttempts = 0;
    bool m_gaveUp = false;
    XrSwapchainUsageFlags m_swapchainUsage = 0;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession  m_session = XR_NULL_HANDLE;
    XrSpace    m_localSpace = XR_NULL_HANDLE;
    XrSpace    m_viewSpace = XR_NULL_HANDLE;
    XrSpace    m_anchorSpace = XR_NULL_HANDLE;  // LOCAL, re-based by recentre

    ID3D11Device*        m_d3d11Device = nullptr;
    ID3D11DeviceContext* m_d3d11Context = nullptr;
    LUID m_adapterLuid = {};

    int64_t m_swapchainFormat = 0;
    bool    m_swizzleToRgba = false;
    UINT m_eyeWidth = 0, m_eyeHeight = 0;
    UINT m_overlayWidth = 0, m_overlayHeight = 0;
    XrSwapchain m_eyeSwapchains[EyeCount] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::vector<ID3D11Texture2D*> m_eyeImages[EyeCount];
    XrSwapchain m_overlaySwapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> m_overlayImages;
    XrSwapchain m_hudLayerSwapchains[HudLayerCount] = {};   // 3D gems: left-eye gems, right-eye gems
    std::vector<ID3D11Texture2D*> m_hudLayerImages[HudLayerCount];
    bool m_hudLayerThisFrame[HudLayerCount] = {};
    float m_hudLayerLifts[HudLayerCount] = {};
    int m_hudLayerEyes[HudLayerCount] = { -1, -1 };
    float m_hudLayerArea[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    UINT m_hudLayerWidth = 0, m_hudLayerHeight = 0;
    std::vector<uint8_t> m_conversionBuffer;

    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    bool m_sessionRunning = false;
    bool m_frameBegun = false;
    bool m_viewsValid = false;
    bool m_recenterRequested = true;
    int  m_trackedFrames = 0;
    XrFrameState m_frameState = { XR_TYPE_FRAME_STATE };
    EyeView m_currentViews[EyeCount] = {};
    EyeView m_submittedViews[EyeCount] = {};
    bool m_eyeImageSubmitted[EyeCount] = { false, false };
    bool m_overlaySubmitted = false;
    uint32_t m_lastLayerCount = 0;

#define OPENXR_FUNCTIONS(X)                       \
    X(xrEnumerateInstanceExtensionProperties)     \
    X(xrCreateInstance)                           \
    X(xrDestroyInstance)                          \
    X(xrResultToString)                           \
    X(xrGetInstanceProperties)                    \
    X(xrGetSystem)                                \
    X(xrGetD3D11GraphicsRequirementsKHR)          \
    X(xrCreateSession)                            \
    X(xrDestroySession)                           \
    X(xrBeginSession)                             \
    X(xrEndSession)                               \
    X(xrPollEvent)                                \
    X(xrCreateReferenceSpace)                     \
    X(xrDestroySpace)                             \
    X(xrLocateSpace)                              \
    X(xrLocateViews)                              \
    X(xrEnumerateViewConfigurationViews)          \
    X(xrEnumerateSwapchainFormats)                \
    X(xrCreateSwapchain)                          \
    X(xrDestroySwapchain)                         \
    X(xrEnumerateSwapchainImages)                 \
    X(xrAcquireSwapchainImage)                    \
    X(xrWaitSwapchainImage)                       \
    X(xrReleaseSwapchainImage)                    \
    X(xrWaitFrame)                                \
    X(xrBeginFrame)                               \
    X(xrEndFrame)

#define DECLARE_OPENXR_FUNCTION(name) PFN_##name name = nullptr;
    OPENXR_FUNCTIONS(DECLARE_OPENXR_FUNCTION)
#undef DECLARE_OPENXR_FUNCTION
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
};
