#include "stereo_targets.h"
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace {

// IID_IDirect3DTexture9 (not in mingw's import libraries).
const GUID TextureInterfaceId = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };

StereoLogFunction g_log = nullptr;
UINT g_screenWidth = 0, g_screenHeight = 0;
bool g_creatingTwin = false;
unsigned g_twinCount = 0;
double g_twinMegabytes = 0.0;
unsigned g_frame = 0;
// The game frees targets without telling us (menus, songs loading): twins nobody has drawn into
// or sampled for this long are released, or they'd pile up and run the graphics card out of memory.
constexpr unsigned IdleFramesBeforeRelease = 300;

struct TextureEntry {
    D3DSURFACE_DESC description;
    UINT levels;
    IDirect3DTexture9* twin;
    bool rightCurrent;            // the twin got the game's last write too (else it's stale: use the original)
    unsigned lastUsedFrame;
};

struct SurfaceEntry {
    D3DSURFACE_DESC description;
    IDirect3DTexture9* owner;     // level 0 of this render target texture, or null (stand-alone surface)
    IDirect3DSurface9* twin;
    bool rightCurrent;            // stand-alone surfaces (textures keep theirs in TextureEntry)
    unsigned lastUsedFrame;
};

std::unordered_map<IDirect3DTexture9*, TextureEntry> g_textures;
std::unordered_map<IDirect3DSurface9*, SurfaceEntry> g_surfaces;

bool IsScreenShaped(UINT width, UINT height) {
    if (!g_screenWidth || !g_screenHeight || !width || !height) return false;
    if (width < 16 || width * 32 < g_screenWidth || width > g_screenWidth * 2) return false;
    const double screenAspect = double(g_screenWidth) / double(g_screenHeight);
    const double aspect = double(width) / double(height);
    return std::fabs(aspect - screenAspect) < screenAspect * 0.02;
}

bool IsTargetUsage(DWORD usage) {
    return (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
}

bool SameDescription(const D3DSURFACE_DESC& first, const D3DSURFACE_DESC& second) {
    return first.Width == second.Width && first.Height == second.Height && first.Format == second.Format &&
           first.Usage == second.Usage && first.MultiSampleType == second.MultiSampleType;
}

double Megabytes(const D3DSURFACE_DESC& description) {
    return double(description.Width) * double(description.Height) * 4.0 / (1024.0 * 1024.0);
}

// Twins owned by a surface entry of a texture's level are references into the texture's twin:
// they don't count separately (description null).
template <typename Resource>
void ReleaseTwin(Resource*& twin, const D3DSURFACE_DESC* description) {
    if (!twin) return;
    twin->Release();
    twin = nullptr;
    if (description) {
        g_twinCount = g_twinCount ? g_twinCount - 1 : 0;
        g_twinMegabytes = std::max(0.0, g_twinMegabytes - Megabytes(*description));
    }
}

template <typename Resource>
void ReleaseTwin(Resource*& twin, const D3DSURFACE_DESC& description) { ReleaseTwin(twin, &description); }

// Surface entries of a texture's level hold a reference into the texture's twin: drop those
// whenever that twin goes, or it's never actually freed.
void ReleaseLevelReferences(IDirect3DTexture9* texture) {
    for (auto& surface : g_surfaces)
        if (surface.second.owner == texture && surface.second.twin) ReleaseTwin(surface.second.twin, nullptr);
}

struct CreatingTwinScope {
    CreatingTwinScope() { g_creatingTwin = true; }
    ~CreatingTwinScope() { g_creatingTwin = false; }
};

} // namespace

namespace StereoTargets {

void Initialise(StereoLogFunction log) { g_log = log; }

void SetScreenSize(UINT width, UINT height) {
    g_screenWidth = width;
    g_screenHeight = height;
}

bool CreatingTwin() { return g_creatingTwin; }

void NoteTexture(IDirect3DTexture9* texture) {
    if (g_creatingTwin || !texture) return;
    D3DSURFACE_DESC description;
    if (FAILED(texture->GetLevelDesc(0, &description))) return;
    if (!IsTargetUsage(description.Usage) || description.Pool != D3DPOOL_DEFAULT || !IsScreenShaped(description.Width, description.Height)) return;
    // A new texture at the address of one the game freed (happens when a song is left and another
    // loaded): start over, so its level surface below is registered too and nothing stale is kept.
    auto existing = g_textures.find(texture);
    if (existing != g_textures.end() && existing->second.twin) {
        ReleaseLevelReferences(texture);
        ReleaseTwin(existing->second.twin, existing->second.description);
    }
    g_textures[texture] = { description, texture->GetLevelCount(), nullptr, false, g_frame };
    IDirect3DSurface9* level = nullptr;
    if (SUCCEEDED(texture->GetSurfaceLevel(0, &level))) {
        auto surface = g_surfaces.find(level);
        if (surface != g_surfaces.end() && surface->second.twin) surface->second.twin->Release();
        g_surfaces[level] = { description, texture, nullptr, false, g_frame };   // twin taken from the texture's twin when needed
        level->Release();
    }
}

void NoteSurface(IDirect3DSurface9* surface) {
    if (g_creatingTwin || !surface) return;
    D3DSURFACE_DESC description;
    if (FAILED(surface->GetDesc(&description))) return;
    if (!IsTargetUsage(description.Usage) || !IsScreenShaped(description.Width, description.Height)) return;
    // Only called for stand-alone surfaces: any entry at this address belonged to something freed,
    // except the device's own backbuffer / depth buffer being noted again after a reset.
    auto existing = g_surfaces.find(surface);
    if (existing != g_surfaces.end()) {
        if (!existing->second.owner && SameDescription(existing->second.description, description)) return;
        if (existing->second.twin) ReleaseTwin(existing->second.twin, existing->second.owner ? nullptr : &existing->second.description);
    }
    g_surfaces[surface] = { description, nullptr, nullptr, false, g_frame };
}

static IDirect3DTexture9* EnsureTwinTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* baseTexture);

IDirect3DBaseTexture9* TwinTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* baseTexture) {
    if (!baseTexture || baseTexture->GetType() != D3DRTYPE_TEXTURE) return nullptr;
    auto found = g_textures.find(static_cast<IDirect3DTexture9*>(baseTexture));
    if (found == g_textures.end() || !found->second.rightCurrent) return nullptr;
    found->second.lastUsedFrame = g_frame;
    return EnsureTwinTexture(device, baseTexture);
}

static IDirect3DTexture9* EnsureTwinTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* baseTexture) {
    if (!baseTexture || baseTexture->GetType() != D3DRTYPE_TEXTURE) return nullptr;
    IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
    auto found = g_textures.find(texture);
    if (found == g_textures.end()) return nullptr;
    TextureEntry& entry = found->second;
    entry.lastUsedFrame = g_frame;
    D3DSURFACE_DESC current;
    if (FAILED(texture->GetLevelDesc(0, &current)) || !SameDescription(current, entry.description)) {
        // A different texture now lives at this address.
        ReleaseLevelReferences(texture);
        if (entry.twin) ReleaseTwin(entry.twin, entry.description);
        g_textures.erase(found);
        return nullptr;
    }
    if (!entry.twin) {
        CreatingTwinScope creating;
        const HRESULT result = device->CreateTexture(entry.description.Width, entry.description.Height, entry.levels,
                                                     entry.description.Usage, entry.description.Format, D3DPOOL_DEFAULT, &entry.twin, nullptr);
        if (FAILED(result)) {
            g_log("[full stereo] couldn't make a right-eye copy of a %ux%u target (%08lX)", entry.description.Width, entry.description.Height, result);
            entry.twin = nullptr;
            return nullptr;
        }
        ++g_twinCount;
        g_twinMegabytes += Megabytes(entry.description);
    }
    return entry.twin;
}

IDirect3DSurface9* TwinSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    if (!surface) return nullptr;
    auto found = g_surfaces.find(surface);
    if (found == g_surfaces.end()) return nullptr;
    SurfaceEntry& entry = found->second;
    entry.lastUsedFrame = g_frame;
    if (entry.owner && !entry.twin) {
        // Only trust the recorded texture if this surface really is its level (the address may be reused).
        IDirect3DTexture9* container = nullptr;
        if (FAILED(surface->GetContainer(TextureInterfaceId, reinterpret_cast<void**>(&container))) || !container) {
            g_surfaces.erase(found);
            return nullptr;
        }
        container->Release();
        if (container != entry.owner) { g_surfaces.erase(found); return nullptr; }
    }
    if (entry.twin) return entry.twin;
    if (entry.owner) {
        IDirect3DTexture9* twinTexture = EnsureTwinTexture(device, entry.owner);
        if (!twinTexture) return nullptr;
        twinTexture->GetSurfaceLevel(0, &entry.twin);   // reference kept with the entry
        return entry.twin;
    }
    D3DSURFACE_DESC current;
    if (FAILED(surface->GetDesc(&current)) || !SameDescription(current, entry.description)) {
        g_surfaces.erase(found);
        return nullptr;
    }
    CreatingTwinScope creating;
    const D3DSURFACE_DESC& description = entry.description;
    HRESULT result;
    if (description.Usage & D3DUSAGE_DEPTHSTENCIL)
        result = device->CreateDepthStencilSurface(description.Width, description.Height, description.Format, description.MultiSampleType,
                                                   description.MultiSampleQuality, FALSE, &entry.twin, nullptr);
    else
        result = device->CreateRenderTarget(description.Width, description.Height, description.Format, description.MultiSampleType,
                                            description.MultiSampleQuality, FALSE, &entry.twin, nullptr);
    if (FAILED(result)) {
        g_log("[full stereo] couldn't make a right-eye copy of a %ux%u surface (%08lX)", description.Width, description.Height, result);
        entry.twin = nullptr;
        return nullptr;
    }
    ++g_twinCount;
    g_twinMegabytes += Megabytes(description);
    return entry.twin;
}

bool IsPerEye(IDirect3DSurface9* surface) {
    return surface && g_surfaces.find(surface) != g_surfaces.end();
}

static bool* RightCurrentFlag(IDirect3DSurface9* surface) {
    if (!surface) return nullptr;
    auto found = g_surfaces.find(surface);
    if (found == g_surfaces.end()) return nullptr;
    if (!found->second.owner) return &found->second.rightCurrent;
    auto texture = g_textures.find(found->second.owner);
    return texture == g_textures.end() ? nullptr : &texture->second.rightCurrent;
}

void NoteWrite(IDirect3DSurface9* surface, bool rightEyeToo) {
    if (bool* flag = RightCurrentFlag(surface)) *flag = rightEyeToo;
}

bool RightEyeCurrent(IDirect3DSurface9* surface) {
    const bool* flag = RightCurrentFlag(surface);
    return flag && *flag;
}

void ReleaseIdle(unsigned frame) {
    g_frame = frame;
    if (frame % 60 != 0) return;
    unsigned released = 0;
    double releasedMegabytes = 0.0;
    const double megabytesBefore = g_twinMegabytes;
    for (auto& entry : g_textures) {
        TextureEntry& texture = entry.second;
        if (!texture.twin || frame - texture.lastUsedFrame < IdleFramesBeforeRelease) continue;
        ReleaseLevelReferences(entry.first);
        ReleaseTwin(texture.twin, texture.description);
        texture.rightCurrent = false;
        ++released;
    }
    for (auto& entry : g_surfaces) {
        SurfaceEntry& surface = entry.second;
        if (surface.owner || !surface.twin || frame - surface.lastUsedFrame < IdleFramesBeforeRelease) continue;
        ReleaseTwin(surface.twin, surface.description);
        surface.rightCurrent = false;
        ++released;
    }
    releasedMegabytes = megabytesBefore - g_twinMegabytes;
    if (released && g_log)
        g_log("[full stereo] released %u unused right-eye copies (~%.0f MB); %u left (~%.0f MB)", released, releasedMegabytes,
              g_twinCount, g_twinMegabytes);
}

void ReleaseAll() {
    for (auto& entry : g_surfaces) if (entry.second.twin) entry.second.twin->Release();
    for (auto& entry : g_textures) if (entry.second.twin) entry.second.twin->Release();
    g_surfaces.clear();
    g_textures.clear();
    g_twinCount = 0;
    g_twinMegabytes = 0.0;
}

void LogSummary() {
    if (g_log) g_log("[full stereo] %u right-eye copies of screen-shaped targets (~%.0f MB of video memory)", g_twinCount, g_twinMegabytes);
}

} // namespace StereoTargets
