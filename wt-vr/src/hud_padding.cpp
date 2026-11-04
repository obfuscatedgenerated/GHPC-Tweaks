#include "hud_padding.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct Layout {
    UINT positionOffset = 0;
    bool positionHasW = false;
    UINT uvOffset = 0;
    bool valid = false;
};

// Where stream 0's position and first texture coordinate sit (both must be floats).
Layout ReadLayout(IDirect3DDevice9* device, UINT stride) {
    Layout layout;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    if (FAILED(device->GetVertexDeclaration(&declaration)) || !declaration) return layout;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = 0;
    bool havePosition = false, haveUv = false;
    if (SUCCEEDED(declaration->GetDeclaration(elements, &count))) {
        for (UINT index = 0; index < count && elements[index].Stream != 0xFF; ++index) {
            const D3DVERTEXELEMENT9& element = elements[index];
            if (element.Stream != 0 || element.UsageIndex != 0) continue;
            if (element.Usage == D3DDECLUSAGE_POSITION || element.Usage == D3DDECLUSAGE_POSITIONT) {
                if (element.Type != D3DDECLTYPE_FLOAT2 && element.Type != D3DDECLTYPE_FLOAT3 && element.Type != D3DDECLTYPE_FLOAT4) continue;
                layout.positionOffset = element.Offset;
                layout.positionHasW = element.Type == D3DDECLTYPE_FLOAT4;
                havePosition = true;
            } else if (element.Usage == D3DDECLUSAGE_TEXCOORD) {
                if (element.Type != D3DDECLTYPE_FLOAT2 && element.Type != D3DDECLTYPE_FLOAT3 && element.Type != D3DDECLTYPE_FLOAT4) continue;
                layout.uvOffset = element.Offset;
                haveUv = true;
            }
        }
    }
    declaration->Release();
    layout.valid = havePosition && haveUv && layout.positionOffset + 8 <= stride && layout.uvOffset + 8 <= stride &&
                   (!layout.positionHasW || layout.positionOffset + 16 <= stride);
    return layout;
}

float* FloatsAt(BYTE* vertices, UINT stride, UINT vertex, UINT offset) {
    return reinterpret_cast<float*>(vertices + size_t(vertex) * stride + offset);
}
const float* FloatsAt(const BYTE* vertices, UINT stride, UINT vertex, UINT offset) {
    return reinterpret_cast<const float*>(vertices + size_t(vertex) * stride + offset);
}

UINT IndexAt(const HudPadding::Draw& draw, UINT index) {
    return draw.indexFormat == D3DFMT_INDEX32 ? static_cast<const DWORD*>(draw.indices)[index]
                                              : static_cast<const WORD*>(draw.indices)[index];
}

// How many vertices make one quad (0: not plain quads we can pad).
UINT QuadSize(const HudPadding::Draw& draw) {
    if (draw.indices) {
        if (draw.type != D3DPT_TRIANGLELIST || draw.vertexCount % 4 || draw.primitiveCount != draw.vertexCount / 2) return 0;   // 2 triangles a quad
        // Every triangle's corners within one group of 4.
        for (UINT triangle = 0; triangle < draw.primitiveCount; ++triangle) {
            const UINT first = IndexAt(draw, triangle * 3);
            if (first < draw.firstVertex) return 0;
            const UINT group = (first - draw.firstVertex) / 4;
            for (UINT corner = 1; corner < 3; ++corner) {
                const UINT vertex = IndexAt(draw, triangle * 3 + corner);
                if (vertex < draw.firstVertex || (vertex - draw.firstVertex) / 4 != group) return 0;
            }
        }
        return 4;
    }
    if (draw.type == D3DPT_TRIANGLELIST && draw.vertexCount % 6 == 0) return 6;
    if ((draw.type == D3DPT_TRIANGLESTRIP || draw.type == D3DPT_TRIANGLEFAN) && draw.vertexCount == 4) return 4;
    return 0;
}

bool Plausible(float value) { return std::isfinite(value) && std::fabs(value) < 1e7f; }

} // namespace

namespace HudPadding {

bool FrameOf(IDirect3DDevice9* device, const Draw& draw, float frame[4]) {
    if (!draw.vertices || !draw.vertexCount) return false;
    const Layout layout = ReadLayout(device, draw.stride);
    if (!layout.valid) return false;
    const BYTE* vertices = static_cast<const BYTE*>(draw.vertices);
    frame[0] = frame[1] = 1e9f;
    frame[2] = frame[3] = -1e9f;
    for (UINT vertex = draw.firstVertex; vertex < draw.firstVertex + draw.vertexCount; ++vertex) {
        const float* uv = FloatsAt(vertices, draw.stride, vertex, layout.uvOffset);
        if (!Plausible(uv[0]) || !Plausible(uv[1])) return false;
        frame[0] = std::min(frame[0], uv[0]);
        frame[1] = std::min(frame[1], uv[1]);
        frame[2] = std::max(frame[2], uv[0]);
        frame[3] = std::max(frame[3], uv[1]);
    }
    return frame[2] > frame[0] && frame[3] > frame[1];
}

bool PadQuads(IDirect3DDevice9* device, const Draw& draw, float marginU, float marginV, std::vector<BYTE>& padded) {
    if (!draw.vertices || !draw.vertexCount || (marginU <= 0.0f && marginV <= 0.0f)) return false;
    const UINT quadSize = QuadSize(draw);
    if (!quadSize) return false;
    const Layout layout = ReadLayout(device, draw.stride);
    if (!layout.valid) return false;
    const size_t bytes = size_t(draw.firstVertex + draw.vertexCount) * draw.stride;
    padded.assign(static_cast<const BYTE*>(draw.vertices), static_cast<const BYTE*>(draw.vertices) + bytes);
    BYTE* vertices = padded.data();
    constexpr float MostGrowth = 3.0f;
    for (UINT quad = draw.firstVertex; quad < draw.firstVertex + draw.vertexCount; quad += quadSize) {
        // Its centre on screen (positions divided by w) and in the texture, and its texture size.
        float centreX = 0, centreY = 0, centreU = 0, centreV = 0;
        float minimumU = 1e9f, minimumV = 1e9f, maximumU = -1e9f, maximumV = -1e9f;
        for (UINT vertex = quad; vertex < quad + quadSize; ++vertex) {
            const float* position = FloatsAt(vertices, draw.stride, vertex, layout.positionOffset);
            const float* uv = FloatsAt(vertices, draw.stride, vertex, layout.uvOffset);
            const float w = layout.positionHasW && std::fabs(position[3]) > 1e-6f ? position[3] : 1.0f;
            if (!Plausible(position[0]) || !Plausible(position[1]) || !Plausible(w) || !Plausible(uv[0]) || !Plausible(uv[1])) return false;
            centreX += position[0] / w;
            centreY += position[1] / w;
            centreU += uv[0];
            centreV += uv[1];
            minimumU = std::min(minimumU, uv[0]);
            maximumU = std::max(maximumU, uv[0]);
            minimumV = std::min(minimumV, uv[1]);
            maximumV = std::max(maximumV, uv[1]);
        }
        centreX /= quadSize, centreY /= quadSize, centreU /= quadSize, centreV /= quadSize;
        const float spanU = maximumU - minimumU, spanV = maximumV - minimumV;
        const float growthU = spanU > 1e-6f ? 1.0f + 2.0f * marginU / spanU : 1.0f;
        const float growthV = spanV > 1e-6f ? 1.0f + 2.0f * marginV / spanV : 1.0f;
        // One growth for both directions: the quad may be turned relative to its texture.
        const float growth = std::min(MostGrowth, std::max(growthU, growthV));
        for (UINT vertex = quad; vertex < quad + quadSize; ++vertex) {
            float* position = FloatsAt(vertices, draw.stride, vertex, layout.positionOffset);
            float* uv = FloatsAt(vertices, draw.stride, vertex, layout.uvOffset);
            const float w = layout.positionHasW && std::fabs(position[3]) > 1e-6f ? position[3] : 1.0f;
            position[0] = (centreX + (position[0] / w - centreX) * growth) * w;
            position[1] = (centreY + (position[1] / w - centreY) * growth) * w;
            uv[0] = centreU + (uv[0] - centreU) * growth;
            uv[1] = centreV + (uv[1] - centreV) * growth;
        }
    }
    return true;
}

} // namespace HudPadding
