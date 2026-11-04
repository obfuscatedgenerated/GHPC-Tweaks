// 3D HUD pieces from height maps ("relief"). A HUD texture with a height map - ghwt_vr\height_maps\<name>.bmp,
// black = flat, white = nearest - is drawn once per eye onto that eye's 3D HUD panel, through a copy of its
// pixel shader that reads the texture shifted sideways by the height there: a parallax bulge.
//
// Names come from a texture's contents ("<width>x<height>_<hash>", e.g. 128x64_676342AC), so they're the
// same in every session. With save_textures=1 each texture is saved as ghwt_vr\textures\<name>.dds (and the
// shader's other textures as <name>_layer1/2.dds) to paint a height map from. vr.ini [depth] <name>=<x>
// scales one texture's depth. Older names (gem_textures\gem_<name>.bmp, [gem_depth]) still work.
//
// The pixel shader copy gets, before each texture read at the main coordinate:
//   texld r31, uv, s15            (the height map on sampler 15)
//   dp3   r31.x, r31, c223        (its brightness)
//   mad   r30.xy, r31.x, c222, uv (uv + height * this eye's shift)
//   mov   r30.zw, uv.zw / c221    (kept, or 0 if the shader only has uv.xy)
// and those reads use r30 instead. s15, c221-c223, r30 and r31 are unused by GHWT's HUD shaders.
//
// For ps_3_0 shaders that leave r27-r29, c219 and c220 free, the copy ray-marches instead: it steps along
// the eye's view through the height map (uv + t * shift, t from 0 to 1, RayMarchSteps steps) and reads at
// the highest point where the surface meets the ray, so a raised part can be drawn past its flat outline
// (the caller pads the quad to make room). Whatever ends up read outside the piece's own frame of the
// texture (c220: u min, v min, u max, v max) comes out transparent, so sprite sheets don't bleed.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <string>

namespace Relief {
    constexpr DWORD HeightSampler = 15;
    constexpr UINT  FrameRegister = 220, ZeroRegister = 221, ShiftRegister = 222, BrightnessRegister = 223;

    using CreatePixelShaderFunction = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

    // outputFolder: ghwt_vr. createPixelShader: the device's own (unhooked) CreatePixelShader.
    void Initialise(const std::string& outputFolder, CreatePixelShaderFunction createPixelShader);

    // Every pixel shader the game makes (its bytecode is kept, to make a 3D copy if it's ever needed).
    void NotePixelShader(IDirect3DPixelShader9* shader, const DWORD* tokens, uint32_t hash);
    // The 3D copy of a pixel shader (made the first time it's asked for); null if it can't be made.
    IDirect3DPixelShader9* ReliefShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
    // Its 3D copy ray-marches (and so wants c220 set, and gains from a padded quad).
    bool RayMarches(IDirect3DPixelShader9* shader);

    struct Texture {
        std::string name;                       // empty: its contents couldn't be read
        IDirect3DTexture9* height = nullptr;    // its height map (null: none, stays flat)
        float strength = 1.0f;                  // [depth] <name>
    };
    // A HUD texture's name, height map and depth (looked up once per texture). layers: the shader's other
    // textures (s1, s2), only used to save them alongside with save_textures=1.
    const Texture& TextureFor(IDirect3DDevice9* device, IDirect3DBaseTexture9* texture, IDirect3DBaseTexture9* const layers[2]);
    // A texture was made at this address: forget what was known about whatever was there before.
    void ForgetTexture(IDirect3DBaseTexture9* texture);
    // The device is being reset: let go of every height map (they're loaded again when next needed).
    void ForgetAllTextures();

    // Diagnostics (HUD dump): saves a texture as ghwt_vr\textures\<name>.dds; returns its hash (0: unreadable).
    uint32_t SaveTextureFile(IDirect3DBaseTexture9* texture);
}
