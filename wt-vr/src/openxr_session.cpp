#include "openxr_session.h"
#include <dxgi.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace {

constexpr int64_t FormatBgraSrgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr int64_t FormatRgbaSrgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr int64_t FormatBgra     = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr int64_t FormatRgba     = DXGI_FORMAT_R8G8B8A8_UNORM;

constexpr DWORD RetryIntervalMs = 3000;
constexpr DWORD NoHeadsetRetryIntervalMs = 15000;  // checking for a headset can be slow on some runtimes

XrPosef IdentityPose() {
    XrPosef pose = {};
    pose.orientation.w = 1.0f;
    return pose;
}

} // namespace

bool OpenXrSession::Check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return true;
    char text[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (xrResultToString && m_instance != XR_NULL_HANDLE) xrResultToString(m_instance, result, text);
    if (m_log) m_log("[xr] %s failed: %s (%d)", what, text, int(result));
    return false;
}

bool OpenXrSession::LoadLoader(const std::string& loaderPath) {
    if (m_loader) return true;
    m_loader = LoadLibraryA(loaderPath.c_str());
    if (!m_loader) {
        m_log("[xr] could not load %s (error %lu)", loaderPath.c_str(), GetLastError());
        return false;
    }
    xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(m_loader, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) {
        m_log("[xr] loader has no xrGetInstanceProcAddr");
        return false;
    }
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties));
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance));
    return xrEnumerateInstanceExtensionProperties && xrCreateInstance;
}

bool OpenXrSession::CreateInstanceAndSystem() {
    if (m_instance == XR_NULL_HANDLE) {
        uint32_t extensionCount = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
        std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
        xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
        bool hasD3D11 = false;
        for (const auto& extension : extensions)
            if (strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
        if (!hasD3D11) {
            m_log("[xr] runtime has no %s (is an OpenXR runtime active? %u extensions)", XR_KHR_D3D11_ENABLE_EXTENSION_NAME, extensionCount);
            return false;
        }

        const char* enabledExtensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy(createInfo.applicationInfo.applicationName, "WT:VR");
        createInfo.applicationInfo.applicationVersion = 1;
        strcpy(createInfo.applicationInfo.engineName, "ghwt_vr");
        createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        createInfo.enabledExtensionCount = 1;
        createInfo.enabledExtensionNames = enabledExtensions;
        if (!Check(xrCreateInstance(&createInfo, &m_instance), "xrCreateInstance")) return false;

#define LOAD_OPENXR_FUNCTION(name) \
        xrGetInstanceProcAddr(m_instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name));
        OPENXR_FUNCTIONS(LOAD_OPENXR_FUNCTION)
#undef LOAD_OPENXR_FUNCTION
        XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
        if (xrGetInstanceProperties && XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &properties)))
            m_log("[xr] instance created, runtime: %s %u.%u.%u", properties.runtimeName,
                  unsigned(XR_VERSION_MAJOR(properties.runtimeVersion)), unsigned(XR_VERSION_MINOR(properties.runtimeVersion)),
                  unsigned(XR_VERSION_PATCH(properties.runtimeVersion)));
        else
            m_log("[xr] instance created");
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const DWORD checkStart = GetTickCount();
    XrResult result = xrGetSystem(m_instance, &systemInfo, &m_systemId);
    const DWORD checkDuration = GetTickCount() - checkStart;
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        m_log("[xr] headset not available (check took %lu ms), next check in %lu s",
              checkDuration, NoHeadsetRetryIntervalMs / 1000);
        m_nextAttemptTick = GetTickCount() + NoHeadsetRetryIntervalMs;
        return false;
    }
    return Check(result, "xrGetSystem");
}

bool OpenXrSession::CreateD3D11Device() {
    XrGraphicsRequirementsD3D11KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (!Check(xrGetD3D11GraphicsRequirementsKHR(m_instance, m_systemId, &requirements), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;
    m_adapterLuid = requirements.adapterLuid;

    using CreateDXGIFactory1Fn = HRESULT (WINAPI*)(REFIID, void**);
    using D3D11CreateDeviceFn = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                  UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE dxgiModule = LoadLibraryA("dxgi.dll");
    HMODULE d3d11Module = LoadLibraryA("d3d11.dll");
    auto createFactory = dxgiModule ? reinterpret_cast<CreateDXGIFactory1Fn>(GetProcAddress(dxgiModule, "CreateDXGIFactory1")) : nullptr;
    auto createDevice = d3d11Module ? reinterpret_cast<D3D11CreateDeviceFn>(GetProcAddress(d3d11Module, "D3D11CreateDevice")) : nullptr;
    if (!createFactory || !createDevice) {
        m_log("[xr] could not load dxgi/d3d11");
        return false;
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    IDXGIAdapter1* matchingAdapter = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        DXGI_ADAPTER_DESC1 description;
        adapter->GetDesc1(&description);
        if (memcmp(&description.AdapterLuid, &m_adapterLuid, sizeof(LUID)) == 0) {
            matchingAdapter = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();
    if (!matchingAdapter) {
        m_log("[xr] no DXGI adapter matches the headset's GPU");
        return false;
    }

    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT result = createDevice(matchingAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, featureLevels, 2,
                                  D3D11_SDK_VERSION, &m_d3d11Device, nullptr, &m_d3d11Context);
    if (FAILED(result)) // 11_1 is refused on some older runtimes; retry with 11_0 only
        result = createDevice(matchingAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, featureLevels + 1, 1,
                              D3D11_SDK_VERSION, &m_d3d11Device, nullptr, &m_d3d11Context);
    matchingAdapter->Release();
    if (FAILED(result)) {
        m_log("[xr] D3D11CreateDevice failed %08lX", result);
        return false;
    }
    m_log("[xr] D3D11 device created");
    return true;
}

bool OpenXrSession::CreateSessionAndSpaces() {
    XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = m_d3d11Device;
    XrSessionCreateInfo createInfo = { XR_TYPE_SESSION_CREATE_INFO };
    createInfo.next = &binding;
    createInfo.systemId = m_systemId;
    if (!Check(xrCreateSession(m_instance, &createInfo, &m_session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.poseInReferenceSpace = IdentityPose();
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_localSpace), "create LOCAL space")) return false;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_anchorSpace), "create anchor space")) return false;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_viewSpace), "create VIEW space")) return false;
    m_log("[xr] session created");
    return true;
}

bool OpenXrSession::CreateSwapchain(UINT width, UINT height, XrSwapchain& swapchain, std::vector<ID3D11Texture2D*>& images) {
    XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    createInfo.usageFlags = m_swapchainUsage;
    createInfo.format = m_swapchainFormat;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrResult createResult = xrCreateSwapchain(m_session, &createInfo, &swapchain);
    if (XR_FAILED(createResult)) {
        char what[160];
        snprintf(what, sizeof(what), "xrCreateSwapchain %ux%u format %d usage 0x%llx", width, height,
                 int(m_swapchainFormat), static_cast<unsigned long long>(m_swapchainUsage));
        Check(createResult, what);
        swapchain = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    if (!Check(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data())),
               "xrEnumerateSwapchainImages"))
        return false;
    images.clear();
    for (const auto& image : swapchainImages) images.push_back(image.texture);
    return true;
}

bool OpenXrSession::CreateSwapchains(float renderScale) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    if (!Check(xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                 viewCount, &viewCount, views.data()), "xrEnumerateViewConfigurationViews")
        || viewCount < EyeCount)
        return false;
    m_eyeWidth = std::max<UINT>(64, UINT(views[0].recommendedImageRectWidth * renderScale));
    m_eyeHeight = std::max<UINT>(64, UINT(views[0].recommendedImageRectHeight * renderScale));

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());
    std::string formatList;
    for (int64_t format : formats) formatList += std::to_string(format) + " ";
    m_log("[xr] recommended eye size %ux%u, runtime formats: %s", views[0].recommendedImageRectWidth,
          views[0].recommendedImageRectHeight, formatList.c_str());

    if (!FindWorkingSwapchainSetup(formats)) return false;
    // FindWorkingSwapchainSetup created the left eye already.
    if (!CreateSwapchain(m_eyeWidth, m_eyeHeight, m_eyeSwapchains[1], m_eyeImages[1])) return false;
    if (!CreateSwapchain(m_overlayWidth, m_overlayHeight, m_overlaySwapchain, m_overlayImages)) return false;
    for (int layer = 0; layer < HudLayerCount; ++layer) {
        m_hudLayerWidth = std::max<UINT>(16, UINT(float(m_overlayWidth) * (m_hudLayerArea[2] - m_hudLayerArea[0]) + 0.5f));
        m_hudLayerHeight = std::max<UINT>(16, UINT(float(m_overlayHeight) * (m_hudLayerArea[3] - m_hudLayerArea[1]) + 0.5f));
        if (!CreateSwapchain(m_hudLayerWidth, m_hudLayerHeight, m_hudLayerSwapchains[layer], m_hudLayerImages[layer])) {
            m_log("[xr] couldn't make the 3D HUD's extra panels: the HUD stays flat");
            m_hudLayerSwapchains[layer] = XR_NULL_HANDLE;
        }
    }

    m_log("[xr] swapchains: eyes %ux%u, overlay %ux%u, format %d%s", m_eyeWidth, m_eyeHeight,
          m_overlayWidth, m_overlayHeight, int(m_swapchainFormat), m_swizzleToRgba ? " (swizzled)" : "");
    return true;
}

// Some runtimes (VDXR's 32-bit D3D11 path among them) reject certain format/usage
// combinations. Try each acceptable one, simplest usage first, keeping the first that works.
bool OpenXrSession::FindWorkingSwapchainSetup(const std::vector<int64_t>& offeredFormats) {
    const int64_t formatPreferences[] = { FormatBgraSrgb, FormatRgbaSrgb, FormatBgra, FormatRgba };
    const XrSwapchainUsageFlags usagePreferences[] = {
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
    };
    for (int64_t format : formatPreferences) {
        if (std::find(offeredFormats.begin(), offeredFormats.end(), format) == offeredFormats.end()) continue;
        for (XrSwapchainUsageFlags usage : usagePreferences) {
            m_swapchainFormat = format;
            m_swapchainUsage = usage;
            if (CreateSwapchain(m_eyeWidth, m_eyeHeight, m_eyeSwapchains[0], m_eyeImages[0])) {
                m_swizzleToRgba = format == FormatRgbaSrgb || format == FormatRgba;
                return true;
            }
        }
    }
    m_log("[xr] no swapchain format/usage combination worked");
    return false;
}

void OpenXrSession::HandleFailedAttempt() {
    Shutdown();
    if (++m_failedAttempts >= 2) {
        m_gaveUp = true;
        m_log("[xr] giving up after %d failed attempts (press F10 in game to try again)", m_failedAttempts);
    }
}

bool OpenXrSession::TryInitialize(const std::string& loaderPath, UINT overlayWidth, UINT overlayHeight,
                                  float renderScale, LogFunction log) {
    if (m_session != XR_NULL_HANDLE) return true;
    if (m_gaveUp || GetTickCount() < m_nextAttemptTick) return false;
    m_nextAttemptTick = GetTickCount() + RetryIntervalMs;
    m_log = log;
    m_overlayWidth = overlayWidth;
    m_overlayHeight = overlayHeight;

    if (!LoadLoader(loaderPath)) return false;
    if (!CreateInstanceAndSystem()) return false;
    if (!m_d3d11Device && !CreateD3D11Device()) return false;
    if (!CreateSessionAndSpaces()) { HandleFailedAttempt(); return false; }
    if (!CreateSwapchains(renderScale)) { HandleFailedAttempt(); return false; }
    m_failedAttempts = 0;
    return true;
}

void OpenXrSession::Shutdown() {
    for (int eye = 0; eye < EyeCount; ++eye) {
        if (m_eyeSwapchains[eye] != XR_NULL_HANDLE) xrDestroySwapchain(m_eyeSwapchains[eye]);
        m_eyeSwapchains[eye] = XR_NULL_HANDLE;
        m_eyeImages[eye].clear();
        m_eyeImageSubmitted[eye] = false;
    }
    if (m_overlaySwapchain != XR_NULL_HANDLE) xrDestroySwapchain(m_overlaySwapchain);
    m_overlaySwapchain = XR_NULL_HANDLE;
    m_overlayImages.clear();
    for (int layer = 0; layer < HudLayerCount; ++layer) {
        if (m_hudLayerSwapchains[layer] != XR_NULL_HANDLE) xrDestroySwapchain(m_hudLayerSwapchains[layer]);
        m_hudLayerSwapchains[layer] = XR_NULL_HANDLE;
        m_hudLayerImages[layer].clear();
        m_hudLayerThisFrame[layer] = false;
    }
    m_overlaySubmitted = false;
    if (m_anchorSpace != XR_NULL_HANDLE) xrDestroySpace(m_anchorSpace);
    if (m_viewSpace != XR_NULL_HANDLE) xrDestroySpace(m_viewSpace);
    if (m_localSpace != XR_NULL_HANDLE) xrDestroySpace(m_localSpace);
    m_anchorSpace = m_viewSpace = m_localSpace = XR_NULL_HANDLE;
    if (m_session != XR_NULL_HANDLE) xrDestroySession(m_session);
    m_session = XR_NULL_HANDLE;
    m_sessionRunning = false;
    m_frameBegun = false;
    m_viewsValid = false;
    m_recenterRequested = true;
    m_sessionState = XR_SESSION_STATE_UNKNOWN;
}

void OpenXrSession::PollEvents() {
    if (m_instance == XR_NULL_HANDLE) return;
    XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(m_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& stateEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (stateEvent.session != m_session || m_session == XR_NULL_HANDLE) {
                event = { XR_TYPE_EVENT_DATA_BUFFER };
                continue;  // stale event from a session we already destroyed
            }
            m_sessionState = stateEvent.state;
            m_log("[xr] session state %d", int(m_sessionState));
            if (m_sessionState == XR_SESSION_STATE_FOCUSED) {   // headset on and tracking: recentre once it settles
                m_recenterRequested = true;
                m_trackedFrames = 0;
            }

            if (m_sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(m_session, &beginInfo), "xrBeginSession")) m_sessionRunning = true;
            } else if (m_sessionState == XR_SESSION_STATE_STOPPING) {
                xrEndSession(m_session);
                m_sessionRunning = false;
                m_frameBegun = false;
            } else if (m_sessionState == XR_SESSION_STATE_EXITING || m_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                Shutdown();
                m_nextAttemptTick = GetTickCount() + RetryIntervalMs;
                return;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            m_log("[xr] instance loss pending");
            Shutdown();
            return;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Re-base the anchor space on the head's current position and yaw (no pitch/roll).
// Waits until the headset has been properly tracked for a moment: right after the session starts
// some runtimes report a "valid" pose at the floor / origin before real tracking arrives, and
// recentring on that leaves you a head-height too high (or low) until the next recentre.
constexpr int TrackedFramesBeforeRecenter = 45;

void OpenXrSession::ApplyRecenter(XrTime time) {
    XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(m_viewSpace, m_localSpace, time, &location))) { m_trackedFrames = 0; return; }
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if ((location.locationFlags & needed) != needed) { m_trackedFrames = 0; return; }
    if (++m_trackedFrames < TrackedFramesBeforeRecenter) return;

    const XrQuaternionf& orientation = location.pose.orientation;
    // Forward (-Z) of the head projected onto the floor plane gives the yaw.
    const float forwardX = -2.0f * (orientation.x * orientation.z + orientation.y * orientation.w);
    const float forwardZ = -(1.0f - 2.0f * (orientation.x * orientation.x + orientation.y * orientation.y));
    const float yaw = atan2f(-forwardX, -forwardZ);

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    spaceInfo.poseInReferenceSpace.position = location.pose.position;

    XrSpace newAnchor = XR_NULL_HANDLE;
    if (XR_SUCCEEDED(xrCreateReferenceSpace(m_session, &spaceInfo, &newAnchor))) {
        if (m_anchorSpace != XR_NULL_HANDLE) xrDestroySpace(m_anchorSpace);
        m_anchorSpace = newAnchor;
        m_recenterRequested = false;
        m_log("[xr] recentred (head %.2f m above the LOCAL origin)", location.pose.position.y);
    }
}

bool OpenXrSession::BeginFrame() {
    m_viewsValid = false;
    if (!m_sessionRunning || m_frameBegun) return m_frameBegun;

    m_frameState = { XR_TYPE_FRAME_STATE };
    XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    if (!Check(xrWaitFrame(m_session, &waitInfo, &m_frameState), "xrWaitFrame")) return false;
    XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult beginResult = xrBeginFrame(m_session, &beginInfo);
    if (XR_FAILED(beginResult)) { Check(beginResult, "xrBeginFrame"); return false; }
    m_frameBegun = true;

    if (m_recenterRequested) ApplyRecenter(m_frameState.predictedDisplayTime);
    else m_trackedFrames = TrackedFramesBeforeRecenter;   // already tracking: later recentres apply at once

    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = m_frameState.predictedDisplayTime;
    locateInfo.space = m_anchorSpace;
    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    XrView views[EyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t viewCount = 0;
    if (XR_SUCCEEDED(xrLocateViews(m_session, &locateInfo, &viewState, EyeCount, &viewCount, views)) &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        for (int eye = 0; eye < EyeCount; ++eye) {
            m_currentViews[eye].pose = views[eye].pose;
            m_currentViews[eye].fov = views[eye].fov;
        }
        m_viewsValid = true;
    }
    return true;
}

void OpenXrSession::UploadToSwapchain(XrSwapchain swapchain, const std::vector<ID3D11Texture2D*>& images,
                                      const void* pixels, int pitch, UINT width, UINT height, bool forceOpaque) {
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage")) return;
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (Check(xrWaitSwapchainImage(swapchain, &waitInfo), "xrWaitSwapchainImage") && imageIndex < images.size()) {
        const void* uploadPixels = pixels;
        int uploadPitch = pitch;
        if (m_swizzleToRgba || forceOpaque) {
            const int redIndex = m_swizzleToRgba ? 2 : 0;
            const int blueIndex = m_swizzleToRgba ? 0 : 2;
            m_conversionBuffer.resize(size_t(width) * height * 4);
            for (UINT row = 0; row < height; ++row) {
                const uint8_t* source = static_cast<const uint8_t*>(pixels) + size_t(row) * pitch;
                uint8_t* destination = m_conversionBuffer.data() + size_t(row) * width * 4;
                for (UINT column = 0; column < width; ++column) {
                    destination[column * 4 + 0] = source[column * 4 + redIndex];
                    destination[column * 4 + 1] = source[column * 4 + 1];
                    destination[column * 4 + 2] = source[column * 4 + blueIndex];
                    destination[column * 4 + 3] = forceOpaque ? 255 : source[column * 4 + 3];
                }
            }
            uploadPixels = m_conversionBuffer.data();
            uploadPitch = int(width * 4);
        }
        m_d3d11Context->UpdateSubresource(images[imageIndex], 0, nullptr, uploadPixels, UINT(uploadPitch), 0);
        // D3D11 only queues the copy; make sure it reaches the GPU before the runtime reads the image.
        m_d3d11Context->Flush();
    }
    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(swapchain, &releaseInfo);
}

void OpenXrSession::SubmitEyeImage(int eye, const void* pixels, int pitch, const EyeView& renderedView, bool forceOpaque) {
    if (!m_frameBegun || eye < 0 || eye >= EyeCount) return;
    UploadToSwapchain(m_eyeSwapchains[eye], m_eyeImages[eye], pixels, pitch, m_eyeWidth, m_eyeHeight, forceOpaque);
    m_submittedViews[eye] = renderedView;
    m_eyeImageSubmitted[eye] = true;
}

void OpenXrSession::SubmitOverlayImage(const void* pixels, int pitch, bool forceOpaque, int layer) {
    if (!m_frameBegun) return;
    if (layer >= 1 && layer <= HudLayerCount) {
        const int index = layer - 1;
        if (m_hudLayerSwapchains[index] == XR_NULL_HANDLE) return;
        UploadToSwapchain(m_hudLayerSwapchains[index], m_hudLayerImages[index], pixels, pitch, m_hudLayerWidth, m_hudLayerHeight, forceOpaque);
        m_hudLayerThisFrame[index] = true;
        return;
    }
    UploadToSwapchain(m_overlaySwapchain, m_overlayImages, pixels, pitch, m_overlayWidth, m_overlayHeight, forceOpaque);
    m_overlaySubmitted = true;
}

void OpenXrSession::ClearSwapchain(XrSwapchain swapchain, const std::vector<ID3D11Texture2D*>& images, const float color[4]) {
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage (test)")) return;
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (Check(xrWaitSwapchainImage(swapchain, &waitInfo), "xrWaitSwapchainImage (test)") && imageIndex < images.size()) {
        D3D11_RENDER_TARGET_VIEW_DESC viewDescription = {};
        viewDescription.Format = DXGI_FORMAT(m_swapchainFormat);
        viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* renderTargetView = nullptr;
        HRESULT result = m_d3d11Device->CreateRenderTargetView(images[imageIndex], &viewDescription, &renderTargetView);
        if (SUCCEEDED(result)) {
            m_d3d11Context->ClearRenderTargetView(renderTargetView, color);
            renderTargetView->Release();
            m_d3d11Context->Flush();
        } else {
            static bool logged = false;
            if (!logged) { m_log("[xr] test pattern: CreateRenderTargetView failed %08lX", result); logged = true; }
        }
    }
    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(swapchain, &releaseInfo);
}

void OpenXrSession::SubmitTestPattern() {
    if (!m_frameBegun) return;
    const float red[4]   = { 1.0f, 0.0f, 0.0f, 1.0f };
    const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const float blue[4]  = { 0.0f, 0.3f, 1.0f, 1.0f };
    ClearSwapchain(m_eyeSwapchains[0], m_eyeImages[0], red);
    ClearSwapchain(m_eyeSwapchains[1], m_eyeImages[1], green);
    ClearSwapchain(m_overlaySwapchain, m_overlayImages, blue);
    for (int eye = 0; eye < EyeCount; ++eye) {
        m_submittedViews[eye] = m_currentViews[eye];
        m_eyeImageSubmitted[eye] = true;
    }
    m_overlaySubmitted = true;
}

void OpenXrSession::EndFrame(const OverlayPlacement& overlay, bool showProjection, bool showOverlay, bool overlayOpaque) {
    if (!m_frameBegun) return;
    m_frameBegun = false;

    XrCompositionLayerProjectionView projectionViews[EyeCount] = {};
    XrCompositionLayerProjection projectionLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad quadLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad hudLayerQuads[HudLayerCount];
    const XrCompositionLayerBaseHeader* layers[2 + HudLayerCount];
    uint32_t layerCount = 0;

    if (m_frameState.shouldRender && showProjection && m_eyeImageSubmitted[0] && m_eyeImageSubmitted[1]) {
        for (int eye = 0; eye < EyeCount; ++eye) {
            XrCompositionLayerProjectionView& view = projectionViews[eye];
            view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            view.pose = m_submittedViews[eye].pose;
            view.fov = m_submittedViews[eye].fov;
            view.subImage.swapchain = m_eyeSwapchains[eye];
            view.subImage.imageRect.offset = { 0, 0 };
            view.subImage.imageRect.extent = { int32_t(m_eyeWidth), int32_t(m_eyeHeight) };
        }
        projectionLayer.space = m_anchorSpace;
        projectionLayer.viewCount = EyeCount;
        projectionLayer.views = projectionViews;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
    }

    if (m_frameState.shouldRender && showOverlay && m_overlaySubmitted) {
        quadLayer.layerFlags = overlayOpaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied
        quadLayer.space = m_anchorSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = m_overlaySwapchain;
        quadLayer.subImage.imageRect.offset = { 0, 0 };
        quadLayer.subImage.imageRect.extent = { int32_t(m_overlayWidth), int32_t(m_overlayHeight) };
        quadLayer.pose = IdentityPose();
        quadLayer.pose.position = { 0.0f, overlay.height, -overlay.distance };
        quadLayer.size = { overlay.width, overlay.width * float(m_overlayHeight) / float(m_overlayWidth) };
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer);
        // 3D HUD: the extra panels, lift metres nearer. Scaled down by the same ratio, so from your
        // (recentred) head everything lines up with the HUD panel - but at a real, nearer distance.
        for (int layer = 0; layer < HudLayerCount; ++layer) {
            if (!m_hudLayerThisFrame[layer]) continue;
            const float distance = std::max(0.1f, overlay.distance - m_hudLayerLifts[layer]);
            const float scale = distance / std::max(0.1f, overlay.distance);
            XrCompositionLayerQuad& hudQuad = hudLayerQuads[layer];
            hudQuad = quadLayer;
            hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hudQuad.eyeVisibility = m_hudLayerEyes[layer] == 0 ? XR_EYE_VISIBILITY_LEFT
                                  : m_hudLayerEyes[layer] == 1 ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = m_hudLayerSwapchains[layer];
            // Just its part of the HUD panel (m_hudLayerArea), in the same place on it.
            const float areaWidth = m_hudLayerArea[2] - m_hudLayerArea[0], areaHeight = m_hudLayerArea[3] - m_hudLayerArea[1];
            const float offsetX = ((m_hudLayerArea[0] + m_hudLayerArea[2]) / 2 - 0.5f) * quadLayer.size.width;
            const float offsetY = (0.5f - (m_hudLayerArea[1] + m_hudLayerArea[3]) / 2) * quadLayer.size.height;
            hudQuad.subImage.imageRect.extent = { int32_t(m_hudLayerWidth), int32_t(m_hudLayerHeight) };
            hudQuad.pose.position = { offsetX * scale, (overlay.height + offsetY) * scale, -distance };
            hudQuad.size = { quadLayer.size.width * areaWidth * scale, quadLayer.size.height * areaHeight * scale };
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
        }
    }
    for (bool& submitted : m_hudLayerThisFrame) submitted = false;

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = m_frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    m_lastLayerCount = layerCount;
    endInfo.layers = layers;
    Check(xrEndFrame(m_session, &endInfo), "xrEndFrame");
}
