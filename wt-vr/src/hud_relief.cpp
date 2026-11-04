#include "hud_relief.h"
#include "log.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace {

constexpr DWORD EndToken = 0x0000FFFF;

std::string g_outputFolder;
Relief::CreatePixelShaderFunction g_createPixelShader = nullptr;

struct PixelShaderRecord {
    std::vector<DWORD> bytecode;
    uint32_t hash = 0;
    IDirect3DPixelShader9* relief = nullptr;
    bool reliefTried = false;
    bool rayMarches = false;   // the 3D copy is the ray-marched kind (else one height read per pixel)
};
std::unordered_map<IDirect3DPixelShader9*, PixelShaderRecord> g_pixelShaders;
std::unordered_map<IDirect3DBaseTexture9*, Relief::Texture> g_textures;

constexpr int RayMarchSteps = 16;

DWORD FloatBits(float value) { DWORD bits; memcpy(&bits, &value, 4); return bits; }

// Does the shader already use any of the registers the ray march adds (r27-r29, c219, c220)?
bool UsesRayMarchRegisters(const DWORD* tokens) {
    auto registerType = [](DWORD parameter) { return ((parameter >> 28) & 7) | ((parameter >> 8) & 0x18); };
    for (size_t index = 1; index < (1u << 16) && tokens[index] != EndToken;) {
        const DWORD token = tokens[index];
        if ((token & 0xFFFF) == 0xFFFE) { index += 1 + ((token >> 16) & 0x7FFF); continue; }
        const DWORD opcode = token & 0xFFFF, length = (token >> 24) & 0xF;
        // dcl: its first parameter is a usage token, not a register. def: only its first is a register.
        const size_t firstParameter = opcode == 0x1F ? 2 : 1, lastParameter = opcode == 0x51 ? 1 : length;
        for (size_t parameter = firstParameter; parameter <= lastParameter; ++parameter) {
            const DWORD value = tokens[index + parameter];
            if (!(value & 0x80000000)) continue;
            const DWORD type = registerType(value), number = value & 0x7FF;
            if ((type == 0 && number >= 27 && number <= 29) || (type == 2 && (number == 219 || number == 220))) return true;
        }
        index += 1 + length;
    }
    return false;
}

// ps_3_0 reads at most one constant register per instruction, so each added instruction uses one; and
// GHWT's runtime refuses sub in ps_3_0 (found by the refusal search below), so it's add with a negated input.
//
// rayMarch (ps_3_0 only): instead of one read of the height at the pixel itself, steps along this eye's
// view (uv + t * shift, t from 0 to 1) and takes the highest point where the surface is at or above the
// ray - so a raised part can be drawn past its flat outline (the quad is padded for room), and what's
// read outside the piece's own frame (c220: u, v min / max) comes out transparent.
// Diagnostics: keepInstructions < all keeps only that many of the ray march's added instructions (then
// takes the plain uv, unmasked), to find which one a refused copy trips over.
constexpr size_t AllInstructions = ~size_t(0);

std::vector<DWORD> PatchPixelShader(const DWORD* tokens, bool rayMarch, size_t keepInstructions = AllInstructions) {
    // GHWT's gem shaders layer several textures (s0 cap / ring, s1, s2) read with the same uv: each
    // read at that uv gets the height-shifted uv instead, so all the layers bulge together. Reads at
    // other coordinates (e.g. an animated colour ramp) are left alone. Plain sprite shaders (one
    // texture) work the same way.
    std::vector<DWORD> result;
    auto registerType = [](DWORD parameter) { return ((parameter >> 28) & 7) | ((parameter >> 8) & 0x18); };
    auto isPlainTextureRead = [&](size_t index) {
        const DWORD token = tokens[index];
        return (token & 0xFFFF) == 0x42 && ((token >> 16) & 0xFF) == 0 && ((token >> 24) & 0xF) == 3 && registerType(tokens[index + 3]) == 10;
    };
    constexpr DWORD InputRegister = 1, TempRegister = 0;
    size_t firstRead = 0, end = 0;
    DWORD coordinate = 0;
    bool haveCoordinate = false;
    for (size_t index = 1; index < (1u << 16) && tokens[index] != EndToken;) {
        const DWORD token = tokens[index];
        if ((token & 0xFFFF) == 0xFFFE) { index += 1 + ((token >> 16) & 0x7FFF); continue; }
        if (isPlainTextureRead(index)) {
            if (!firstRead) firstRead = index;
            if (!haveCoordinate && (tokens[index + 3] & 0x7FF) == 0) { coordinate = tokens[index + 2]; haveCoordinate = true; }
        }
        index += 1 + ((token >> 24) & 0xF);
        end = index;
    }
    if (!firstRead || !haveCoordinate) return result;
    const DWORD coordinateType = registerType(coordinate);
    if (coordinateType != InputRegister && coordinateType != TempRegister) return result;
    auto sameRegister = [&](DWORD parameter) {
        return (parameter & 0x7FF) == (coordinate & 0x7FF) && registerType(parameter) == coordinateType;
    };
    // Does the coordinate carry .zw too (some shaders read a second coordinate from them)? Inputs: from
    // their declaration (reading undeclared components is refused). Temps: assume so.
    bool coordinateHasZw = coordinateType == TempRegister;
    for (size_t index = 1; index < firstRead;) {
        const DWORD token = tokens[index];
        if ((token & 0xFFFF) == 0xFFFE) { index += 1 + ((token >> 16) & 0x7FFF); continue; }
        if ((token & 0xFFFF) == 0x1F && sameRegister(tokens[index + 2]))
            coordinateHasZw = ((tokens[index + 2] >> 16) & 0xC) == 0xC;
        index += 1 + ((token >> 24) & 0xF);
    }
    const DWORD coordinateXy = (coordinate & 0xF0001FFF) | (0x54u << 16);   // uv.xyyy
    const DWORD coordinateAll = (coordinate & 0xF0001FFF) | (0xE4u << 16);  // uv.xyzw
    const DWORD keepZw = coordinateHasZw ? coordinateAll : 0xA0E400DDu;      // uv.zw (or c221: 0)

    std::vector<DWORD> shiftedCoordinate;
    if (!rayMarch) {
        shiftedCoordinate = { 0x03000042, 0x800F001F, coordinateAll, 0xA0E4080F,            // texld r31, uv, s15
                              0x03000008, 0x8001001F, 0x80E4001F, 0xA0E400DF,               // dp3 r31.x, r31, c223
                              0x04000004, 0x8003001E, 0x8000001F, 0xA0E400DE, coordinateXy,  // mad r30.xy, r31.x, c222, uv.xy
                              0x02000001, 0x800C001E, keepZw };                              // mov r30.zw, uv.zw
    } else {
        shiftedCoordinate = { 0x02000001, 0x8003001C, coordinateXy,                          // mov r28.xy, uv        (hit so far: the base)
                              0x02000001, 0x8004001C, 0xA0E400DD,                            // mov r28.z, c221       (t = 0)
                              0x02000001, 0x800C001D, 0xA0E400DD };                          // mov r29.zw, c221
        for (int step = 0; step < RayMarchSteps; ++step) {
            const DWORD stepTokens[] = {
                0x03000002, 0x8004001C, 0x80AA001C, 0xA00000DB,                // add r28.z, r28.z, c219.x      (t += 1 / steps)
                0x04000004, 0x8003001B, 0x80AA001C, 0xA0E400DE, coordinateXy,  // mad r27.xy, t, c222, uv       (the ray at height t)
                0x0300000B, 0x8003001D, 0x80E4001B, 0xA0E400DC,                // max r29.xy, r27, c220         (kept in the frame
                0x0300000A, 0x8003001D, 0x80E4001D, 0xA0EE00DC,                // min r29.xy, r29, c220.zw       for the height read)
                0x03000042, 0x800F001F, 0x80E4001D, 0xA0E4080F,                // texld r31, r29, s15
                0x03000008, 0x8001001F, 0x80E4001F, 0xA0E400DF,                // dp3 r31.x, r31, c223          (height there)
                0x03000002, 0x8001001F, 0x8000001F, 0x81AA001C,                // add r31.x, height, -t
                0x04000058, 0x8003001C, 0x8000001F, 0x80E4001B, 0x80E4001C };  // cmp r28.xy: surface at or above the ray -> hit
            shiftedCoordinate.insert(shiftedCoordinate.end(), std::begin(stepTokens), std::end(stepTokens));
        }
        const DWORD finish[] = {
            0x02000001, 0x8003001E, 0x80E4001C,                          // mov r30.xy, r28 (the highest hit)
            0x02000001, 0x800C001E, keepZw,                              // mov r30.zw, uv.zw
            0x03000002, 0x8003001D, 0x80E4001E, 0xA1E400DC,              // add r29.xy, r30, -c220        (>= 0: past the minimum
            0x03000002, 0x800C001D, 0xA0E400DC, 0x8144001E,              // add r29.zw, c220, -r30.xyxy    and short of the maximum)
            0x0300000A, 0x8003001D, 0x80E4001D, 0x80EE001D,              // min r29.xy, r29, r29.zwzw
            0x0300000A, 0x8001001D, 0x8000001D, 0x8055001D,              // min r29.x, r29.x, r29.y
            0x04000058, 0x8001001D, 0x8000001D, 0xA05500DB, 0xA0AA00DB }; // cmp r29.x: inside -> 1 (c219.y), else 0 (c219.z)
        shiftedCoordinate.insert(shiftedCoordinate.end(), std::begin(finish), std::end(finish));
        if (keepInstructions != AllInstructions) {
            size_t offset = 0;
            for (size_t kept = 0; kept < keepInstructions && offset < shiftedCoordinate.size(); ++kept)
                offset += 1 + ((shiftedCoordinate[offset] >> 24) & 0xF);
            shiftedCoordinate.resize(offset);
            const DWORD plain[] = { 0x02000001, 0x8003001E, coordinateXy, 0x02000001, 0x800C001E, keepZw };   // r30 = uv
            shiftedCoordinate.insert(shiftedCoordinate.end(), std::begin(plain), std::end(plain));
        }
    }

    result.push_back(tokens[0]);
    const DWORD declareHeightSampler[] = { 0x0200001F, 0x90000000, 0xA00F080F };   // dcl_2d s15
    result.insert(result.end(), declareHeightSampler, declareHeightSampler + 3);
    if (rayMarch) {
        const DWORD defineSteps[] = { 0x05000051, 0xA00F00DB, FloatBits(1.0f / RayMarchSteps), FloatBits(1.0f), 0, 0 };   // def c219, 1/steps, 1, 0, 0
        result.insert(result.end(), std::begin(defineSteps), std::end(defineSteps));
    }
    result.insert(result.end(), tokens + 1, tokens + firstRead);
    // An input never changes: work it out once. A temp may be rewritten between reads: before each one.
    if (coordinateType == InputRegister) result.insert(result.end(), shiftedCoordinate.begin(), shiftedCoordinate.end());
    for (size_t index = firstRead; index < end;) {
        const DWORD token = tokens[index];
        const size_t length = (token & 0xFFFF) == 0xFFFE ? ((token >> 16) & 0x7FFF) : ((token >> 24) & 0xF);
        const bool redirected = (token & 0xFFFF) != 0xFFFE && isPlainTextureRead(index) && sameRegister(tokens[index + 2]);
        if (redirected && coordinateType == TempRegister) result.insert(result.end(), shiftedCoordinate.begin(), shiftedCoordinate.end());
        const size_t first = result.size();
        result.insert(result.end(), tokens + index, tokens + index + 1 + length);
        if (redirected) {
            result[first + 2] = (tokens[index + 2] & 0x00FF0000) | 0x8000001E;   // this read now uses r30 (same swizzle)
            if (rayMarch && keepInstructions == AllInstructions) {   // mul <read>, <read>, r29.x: nothing from outside the frame
                const DWORD destination = tokens[index + 1] & ~0x00F00000u;   // without _sat etc.
                const DWORD source = (destination & 0xF0001FFF) | (0xE4u << 16);
                const DWORD mask[] = { 0x03000005, destination, source, 0x8000001D };
                result.insert(result.end(), std::begin(mask), std::end(mask));
            }
        }
        index += 1 + length;
    }
    result.push_back(EndToken);
    return result;
}

uint32_t Fnv1a(const BYTE* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < size; ++index) hash = (hash ^ data[index]) * 16777619u;
    return hash;
}

void WriteLittle32(FILE* file, DWORD value) { fwrite(&value, 4, 1, file); }

// A texture's content hash: the same texture gets the same name in every session (0 if it can't be read).
uint32_t ContentHash(IDirect3DBaseTexture9* baseTexture) {
    if (!baseTexture || baseTexture->GetType() != D3DRTYPE_TEXTURE) return 0;
    IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
    D3DSURFACE_DESC description = {};
    texture->GetLevelDesc(0, &description);
    uint32_t hash = 0;
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY))) {
        const bool compressed = description.Format >= D3DFMT_DXT1 && description.Format <= D3DFMT_DXT5;
        const UINT rows = compressed ? std::max(1u, (description.Height + 3) / 4) : description.Height;
        hash = 2166136261u;
        for (UINT row = 0; row < rows; ++row)
            hash = (hash ^ Fnv1a(static_cast<const BYTE*>(locked.pBits) + size_t(row) * locked.Pitch, std::min<UINT>(locked.Pitch, 4096))) * 16777619u;
        texture->UnlockRect(0);
    }
    return hash;
}

// Saves level 0 of a texture as a .dds (DXT1-5 or 32-bit), for painting a height map from.
// Saves level 0 of a texture as a .dds (DXT1-5 or 32-bit).
void SaveTextureAsDds(const D3DSURFACE_DESC& description, const D3DLOCKED_RECT& locked, const std::string& path) {
    const bool compressed = description.Format == D3DFMT_DXT1 || description.Format == D3DFMT_DXT2 || description.Format == D3DFMT_DXT3 ||
                            description.Format == D3DFMT_DXT4 || description.Format == D3DFMT_DXT5;
    const bool argb = description.Format == D3DFMT_A8R8G8B8 || description.Format == D3DFMT_X8R8G8B8;
    if (!compressed && !argb) { LogLine("[3d hud] texture format %d can't be saved as .dds", int(description.Format)); return; }
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return;
    const UINT blockBytes = description.Format == D3DFMT_DXT1 ? 8 : 16;
    const UINT rowBytes = compressed ? std::max(1u, (description.Width + 3) / 4) * blockBytes : description.Width * 4;
    const UINT rows = compressed ? std::max(1u, (description.Height + 3) / 4) : description.Height;
    fwrite("DDS ", 4, 1, file);
    WriteLittle32(file, 124);
    WriteLittle32(file, 0x1 | 0x2 | 0x4 | 0x1000 | (compressed ? 0x80000 : 0x8));   // caps height width pixelformat linearsize/pitch
    WriteLittle32(file, description.Height);
    WriteLittle32(file, description.Width);
    WriteLittle32(file, compressed ? rowBytes * rows : rowBytes);
    WriteLittle32(file, 0);
    WriteLittle32(file, 0);
    for (int reserved = 0; reserved < 11; ++reserved) WriteLittle32(file, 0);
    WriteLittle32(file, 32);   // pixel format
    if (compressed) {
        WriteLittle32(file, 0x4);
        WriteLittle32(file, DWORD(description.Format));   // the DXTn FourCC
        for (int unused = 0; unused < 5; ++unused) WriteLittle32(file, 0);
    } else {
        const bool alpha = description.Format == D3DFMT_A8R8G8B8;
        WriteLittle32(file, 0x40 | (alpha ? 0x1 : 0));
        WriteLittle32(file, 0);
        WriteLittle32(file, 32);
        WriteLittle32(file, 0x00FF0000);
        WriteLittle32(file, 0x0000FF00);
        WriteLittle32(file, 0x000000FF);
        WriteLittle32(file, alpha ? 0xFF000000 : 0);
    }
    WriteLittle32(file, 0x1000);   // caps: texture
    for (int unused = 0; unused < 4; ++unused) WriteLittle32(file, 0);
    for (UINT row = 0; row < rows; ++row) fwrite(static_cast<const BYTE*>(locked.pBits) + size_t(row) * locked.Pitch, rowBytes, 1, file);
    fclose(file);
}

// A .bmp (8-bit with a palette - e.g. greyscale -, 24 or 32-bit) as a greyscale height texture (any size).
IDirect3DTexture9* LoadHeightMap(IDirect3DDevice9* device, const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return nullptr;
    std::vector<BYTE> data;
    BYTE chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) data.insert(data.end(), chunk, chunk + got);
    fclose(file);
    auto read32 = [&](size_t offset) { DWORD value = 0; if (offset + 4 <= data.size()) memcpy(&value, &data[offset], 4); return value; };
    auto read16 = [&](size_t offset) { WORD value = 0; if (offset + 2 <= data.size()) memcpy(&value, &data[offset], 2); return value; };
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') { LogLine("[3d hud] %s isn't a .bmp", path.c_str()); return nullptr; }
    const DWORD pixelOffset = read32(10);
    const int width = int(read32(18));
    const int signedHeight = int(read32(22));
    const int height = std::abs(signedHeight);
    const int bitsPerPixel = read16(28);
    const DWORD compression = read32(30);   // 0 plain, 3 bit fields (32-bit: still B G R A bytes)
    const bool supported = (bitsPerPixel == 8 || bitsPerPixel == 24 || bitsPerPixel == 32) &&
                           (compression == 0 || (compression == 3 && bitsPerPixel == 32));
    if (width <= 0 || height <= 0 || !supported) {
        LogLine("[3d hud] %s: %d-bit, compression %lu - needs an uncompressed 8, 24 or 32-bit .bmp", path.c_str(), bitsPerPixel, compression);
        return nullptr;
    }
    // 8-bit: the palette follows the header (4 bytes each: B G R unused).
    const size_t paletteOffset = 14 + read32(14);
    const DWORD paletteSize = read32(46) ? read32(46) : 256;
    const size_t bytesPerPixel = size_t(bitsPerPixel / 8);
    const size_t rowStride = (size_t(width) * bytesPerPixel + 3) & ~size_t(3);
    if (pixelOffset + rowStride * height > data.size()) { LogLine("[3d hud] %s is cut short", path.c_str()); return nullptr; }
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) return nullptr;
    D3DLOCKED_RECT locked;
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) { texture->Release(); return nullptr; }
    for (int row = 0; row < height; ++row) {
        const int sourceRow = signedHeight > 0 ? height - 1 - row : row;   // positive height: stored bottom-up
        const BYTE* source = &data[pixelOffset + rowStride * sourceRow];
        DWORD* destination = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + size_t(row) * locked.Pitch);
        for (int column = 0; column < width; ++column) {
            const BYTE* pixel = source + column * bytesPerPixel;
            if (bitsPerPixel == 8) {
                const size_t entry = paletteOffset + size_t(std::min<DWORD>(pixel[0], paletteSize - 1)) * 4;
                pixel = entry + 3 <= data.size() ? &data[entry] : pixel;
            }
            const DWORD grey = bitsPerPixel == 8 && pixel == source + column ? pixel[0] : (DWORD(pixel[0]) + pixel[1] + pixel[2]) / 3;
            destination[column] = 0xFF000000 | (grey << 16) | (grey << 8) | grey;
        }
    }
    texture->UnlockRect(0);
    return texture;
}

std::string TextureName(IDirect3DTexture9* texture, uint32_t hash) {
    D3DSURFACE_DESC description = {};
    texture->GetLevelDesc(0, &description);
    char name[64];
    snprintf(name, sizeof(name), "%ux%u_%08X", description.Width, description.Height, hash);
    return name;
}

bool FileExists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Saves a texture (once) as <folder>\<fileName>.dds.
void SaveOnce(IDirect3DBaseTexture9* baseTexture, const std::string& fileName) {
    if (!baseTexture || baseTexture->GetType() != D3DRTYPE_TEXTURE) return;
    const std::string folder = g_outputFolder + "\\textures";
    CreateDirectoryA(folder.c_str(), nullptr);
    const std::string path = folder + "\\" + fileName + ".dds";
    if (FileExists(path)) return;
    IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
    D3DSURFACE_DESC description = {};
    texture->GetLevelDesc(0, &description);
    D3DLOCKED_RECT locked;
    if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY))) return;
    SaveTextureAsDds(description, locked, path);
    texture->UnlockRect(0);
    LogLine("[3d hud] saved ghwt_vr\\textures\\%s.dds (paint a height map: height_maps\\%s.bmp)", fileName.c_str(), fileName.c_str());
}

// [depth] <name>, or the older [gem_depth] gem_<name>; listed in [depth] once seen.
float DepthStrength(const std::string& name) {
    char text[32];
    GetPrivateProfileStringA("depth", name.c_str(), "", text, sizeof(text), g_iniPath.c_str());
    if (!text[0]) GetPrivateProfileStringA("gem_depth", ("gem_" + name).c_str(), "", text, sizeof(text), g_iniPath.c_str());
    if (!text[0]) {
        WritePrivateProfileStringA("depth", name.c_str(), "1.00", g_iniPath.c_str());
        return 1.0f;
    }
    return std::min(10.0f, std::max(0.0f, float(atof(text))));
}

} // namespace

namespace Relief {

void Initialise(const std::string& outputFolder, CreatePixelShaderFunction createPixelShader) {
    g_outputFolder = outputFolder;
    g_createPixelShader = createPixelShader;
}

void NotePixelShader(IDirect3DPixelShader9* shader, const DWORD* tokens, uint32_t hash) {
    size_t count = 1;
    while (count < (1u << 16) && tokens[count] != EndToken) {
        const DWORD token = tokens[count];
        count += (token & 0xFFFF) == 0xFFFE ? 1 + ((token >> 16) & 0x7FFF) : 1 + ((token >> 24) & 0xF);
    }
    PixelShaderRecord& record = g_pixelShaders[shader];   // a new shader at an old one's address replaces it
    if (record.relief) record.relief->Release();
    record = PixelShaderRecord();
    record.bytecode.assign(tokens, tokens + count + 1);
    record.hash = hash;
}

// A refused ray-marched copy: which added instruction is the problem? Tries copies keeping fewer and
// fewer of them (a binary search) and logs the first one that gets it refused, as its raw tokens.
static void DiagnoseRefusal(IDirect3DDevice9* device, const DWORD* tokens, uint32_t hash) {
    auto accepted = [&](size_t keep) {
        const std::vector<DWORD> patched = PatchPixelShader(tokens, true, keep);
        IDirect3DPixelShader9* shader = nullptr;
        if (patched.empty() || FAILED(g_createPixelShader(device, patched.data(), &shader)) || !shader) return false;
        shader->Release();
        return true;
    };
    if (!accepted(0)) { LogLine("[3d hud]   %08X: refused even with none of the ray march (its def / dcl?)", hash); return; }
    // How many instructions the ray march adds (counted from a full build's difference isn't needed:
    // keep growing until the whole list is in).
    size_t good = 0, bad = 1;
    while (accepted(bad)) {
        good = bad;
        bad *= 2;
        if (bad > 4096) { LogLine("[3d hud]   %08X: every part is accepted alone - the frame mask (mul after reads) is refused", hash); return; }
    }
    while (bad - good > 1) {
        const size_t middle = (good + bad) / 2;
        if (accepted(middle)) good = middle; else bad = middle;
    }
    // The instruction at index good (0-based) of the added list: rebuild and print it.
    const std::vector<DWORD> withIt = PatchPixelShader(tokens, true, bad), without = PatchPixelShader(tokens, true, good);
    size_t first = 0;
    while (first < without.size() && first < withIt.size() && without[first] == withIt[first]) ++first;
    char text[160];
    int length = snprintf(text, sizeof(text), "[3d hud]   %08X: refused from added instruction %zu:", hash, good);
    for (size_t index = first; index < withIt.size() && index < first + 6 && length < int(sizeof(text)) - 10; ++index)
        length += snprintf(text + length, sizeof(text) - length, " %08lX", withIt[index]);
    LogLine("%s", text);
}

IDirect3DPixelShader9* ReliefShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    auto found = g_pixelShaders.find(shader);
    if (found == g_pixelShaders.end()) return nullptr;
    PixelShaderRecord& record = found->second;
    if (record.reliefTried) return record.relief;
    record.reliefTried = true;
    const DWORD* tokens = record.bytecode.data();
    // The ray march needs ps_3_0 (instruction count) and its extra registers free; else the simple kind.
    const bool canRayMarch = tokens[0] == 0xFFFF0300 && !UsesRayMarchRegisters(tokens);
    char whySimple[96] = "";
    if (tokens[0] != 0xFFFF0300) snprintf(whySimple, sizeof(whySimple), "shader version %08lX isn't ps_3_0", tokens[0]);
    else if (!canRayMarch) snprintf(whySimple, sizeof(whySimple), "it already uses r27-r29 / c219 / c220");
    std::vector<DWORD> patched = PatchPixelShader(tokens, canRayMarch);
    record.rayMarches = canRayMarch;
    if (canRayMarch && !patched.empty() && g_createPixelShader) {
        const HRESULT created = g_createPixelShader(device, patched.data(), &record.relief);
        if (FAILED(created)) {
            snprintf(whySimple, sizeof(whySimple), "the ray-marched copy was refused (%08lX)", created);
            DiagnoseRefusal(device, tokens, record.hash);
            record.relief = nullptr;
            patched = PatchPixelShader(tokens, false);
            record.rayMarches = false;
        }
    }
    if (!patched.empty() && g_createPixelShader && (record.relief || SUCCEEDED(g_createPixelShader(device, patched.data(), &record.relief))) && record.relief) {
        LogLine("[3d hud] made a 3D copy of pixel shader %08X (%s)", record.hash,
                record.rayMarches ? "ray-marched: raised parts can reach past the outline" : "simple: one height read per pixel");
        if (!record.rayMarches) LogLine("[3d hud]   not ray-marched: %s", whySimple);
    } else {
        record.relief = nullptr;
        LogLine("[3d hud] couldn't make a 3D copy of pixel shader %08X (%s): what it draws stays flat", record.hash,
                patched.empty() ? "no texture read from s0 found" : "the copy was refused");
    }
    return record.relief;
}

bool RayMarches(IDirect3DPixelShader9* shader) {
    auto found = g_pixelShaders.find(shader);
    return found != g_pixelShaders.end() && found->second.relief && found->second.rayMarches;
}

const Texture& TextureFor(IDirect3DDevice9* device, IDirect3DBaseTexture9* baseTexture, IDirect3DBaseTexture9* const layers[2]) {
    auto found = g_textures.find(baseTexture);
    if (found != g_textures.end()) return found->second;
    Texture& entry = g_textures[baseTexture];
    const uint32_t hash = ContentHash(baseTexture);
    if (!hash) return entry;
    IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(baseTexture);
    entry.name = TextureName(texture, hash);
    if (g_settings.saveTextures) {
        SaveOnce(baseTexture, entry.name);
        for (int layer = 0; layer < 2; ++layer)
            if (layers && layers[layer]) SaveOnce(layers[layer], entry.name + "_layer" + std::to_string(layer + 1));
    }
    std::string path = g_outputFolder + "\\height_maps\\" + entry.name + ".bmp";
    if (!FileExists(path)) path = g_outputFolder + "\\gem_textures\\gem_" + entry.name + ".bmp";   // older name
    if (FileExists(path)) {
        entry.height = LoadHeightMap(device, path);
        if (entry.height) {
            entry.strength = DepthStrength(entry.name);
            LogLine("[3d hud] %s: 3D from its height map (depth x%.2f)", entry.name.c_str(), entry.strength);
        }
    }
    return entry;
}

void ForgetTexture(IDirect3DBaseTexture9* texture) {
    auto found = g_textures.find(texture);
    if (found == g_textures.end()) return;
    if (found->second.height) found->second.height->Release();
    g_textures.erase(found);
}

void ForgetAllTextures() {
    for (auto& entry : g_textures)
        if (entry.second.height) entry.second.height->Release();
    g_textures.clear();
}

uint32_t SaveTextureFile(IDirect3DBaseTexture9* baseTexture) {
    const uint32_t hash = ContentHash(baseTexture);
    if (hash) SaveOnce(baseTexture, TextureName(static_cast<IDirect3DTexture9*>(baseTexture), hash));
    return hash;
}

} // namespace Relief
