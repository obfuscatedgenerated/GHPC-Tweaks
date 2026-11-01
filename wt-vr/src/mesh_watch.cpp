#include "mesh_watch.h"
#include <cmath>

namespace {

using LockFunction = HRESULT (STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
using UnlockFunction = HRESULT (STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*);

MeshWatchLogFunction g_log = nullptr;
LockFunction g_originalLock = nullptr;
UnlockFunction g_originalUnlock = nullptr;
volatile unsigned g_frame = 0;

constexpr int SlotCount = 32;
constexpr int RecordsPerSlot = 48;       // GHWT writes ~23 meshes a frame into one shared buffer
constexpr int SamplesPerRecord = 128;

// A sparse sample of one write (Lock .. Unlock) into a watched buffer.
struct LockRecord {
    UINT     start = 0, end = 0;          // bytes of the buffer written
    UINT     stride = 0;                  // the vertex size it was sampled with
    unsigned frame = 0;
    UINT     sampleCount = 0;
    UINT     sampleByte[SamplesPerRecord];          // where each sampled vertex starts
    float    position[SamplesPerRecord][4];   // x y z, and w when the vertex has room for it (else 1)
};

// One watched vertex buffer. Assigned on the render thread; the game may write buffers from
// other threads, so the Lock/Unlock side only touches its own slot's fields.
struct Slot {
    IDirect3DVertexBuffer9* volatile buffer = nullptr;
    UINT bufferSize = 0;
    UINT stride = 0;
    UINT positionOffset = 0;
    unsigned lastWantedFrame = 0;
    // Set between Lock and Unlock.
    BYTE* lockPointer = nullptr;
    UINT lockOffset = 0;
    UINT lockSize = 0;
    DWORD lockFlags = 0;
    // Recent writes.
    LockRecord records[RecordsPerSlot];
    volatile unsigned nextRecord = 0;
    unsigned locks = 0;
    unsigned draws = 0, drawsFound = 0;   // diagnostics: draws asked about / answered
};
Slot g_slots[SlotCount];

Slot* FindSlot(IDirect3DVertexBuffer9* buffer) {
    for (Slot& slot : g_slots)
        if (slot.buffer == buffer) return &slot;
    return nullptr;
}

bool Plausible(float value) {
    return std::isfinite(value) && std::fabs(value) < 10000.0f;
}

// Samples what was just written: vertices are taken to start at the lock offset (each mesh is
// written as its own block), every stride bytes.
void RecordWrite(Slot& slot) {
    if (!slot.lockPointer || !slot.stride || slot.lockSize < slot.stride) return;
    LockRecord& record = slot.records[slot.nextRecord % RecordsPerSlot];
    const UINT vertexCount = slot.lockSize / slot.stride;
    const UINT step = vertexCount > SamplesPerRecord ? (vertexCount + SamplesPerRecord - 1) / SamplesPerRecord : 1;
    UINT used = 0;
    for (UINT vertex = 0; vertex < vertexCount && used < SamplesPerRecord; vertex += step) {
        const UINT byte = vertex * slot.stride + slot.positionOffset;
        if (byte + 12 > slot.lockSize) break;
        const float* position = reinterpret_cast<const float*>(slot.lockPointer + byte);
        if (!Plausible(position[0]) || !Plausible(position[1]) || !Plausible(position[2])) continue;
        record.sampleByte[used] = slot.lockOffset + vertex * slot.stride;
        record.position[used][0] = position[0];
        record.position[used][1] = position[1];
        record.position[used][2] = position[2];
        record.position[used][3] = byte + 16 <= slot.lockSize && slot.stride >= slot.positionOffset + 16 && Plausible(position[3]) ? position[3] : 1.0f;
        ++used;
    }
    record.start = slot.lockOffset;
    record.end = slot.lockOffset + slot.lockSize;
    record.stride = slot.stride;
    record.frame = g_frame;
    record.sampleCount = used;
    slot.nextRecord = slot.nextRecord + 1;
}

HRESULT STDMETHODCALLTYPE HookedLock(IDirect3DVertexBuffer9* buffer, UINT offset, UINT size, void** data, DWORD flags) {
    const HRESULT result = g_originalLock(buffer, offset, size, data, flags);
    if (SUCCEEDED(result) && data && *data && !(flags & D3DLOCK_READONLY)) {
        if (Slot* slot = FindSlot(buffer)) {
            slot->lockPointer = static_cast<BYTE*>(*data);
            slot->lockOffset = offset;
            slot->lockSize = size ? size : (slot->bufferSize > offset ? slot->bufferSize - offset : 0);
            slot->lockFlags = flags;
            ++slot->locks;
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookedUnlock(IDirect3DVertexBuffer9* buffer) {
    if (Slot* slot = FindSlot(buffer)) {
        RecordWrite(*slot);
        slot->lockPointer = nullptr;
    }
    return g_originalUnlock(buffer);
}

void PatchSlot(void* object, int index, void* replacement, void** original) {
    void** table = *reinterpret_cast<void***>(object);
    if (table[index] == replacement) return;
    DWORD oldProtection;
    VirtualProtect(&table[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection);
    *original = table[index];
    table[index] = replacement;
    VirtualProtect(&table[index], sizeof(void*), oldProtection, &oldProtection);
}

// Where position sits in each vertex of stream 0 (from the vertex declaration; 0 if there isn't one).
UINT PositionOffset(IDirect3DDevice9* device) {
    IDirect3DVertexDeclaration9* declaration = nullptr;
    if (FAILED(device->GetVertexDeclaration(&declaration)) || !declaration) return 0;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = 0;
    UINT offset = 0;
    if (SUCCEEDED(declaration->GetDeclaration(elements, &count))) {
        for (UINT index = 0; index < count && elements[index].Stream != 0xFF; ++index) {
            const D3DVERTEXELEMENT9& element = elements[index];
            if (element.Stream == 0 && element.Usage == D3DDECLUSAGE_POSITION && element.UsageIndex == 0) { offset = element.Offset; break; }
        }
    }
    declaration->Release();
    return offset;
}

} // namespace

namespace MeshWatch {

void Initialise(MeshWatchLogFunction log) { g_log = log; }

void HookVertexBuffers(IDirect3DVertexBuffer9* vertexBuffer) {
    if (g_originalLock || !vertexBuffer) return;
    // IDirect3DVertexBuffer9: ... 11 Lock, 12 Unlock
    PatchSlot(vertexBuffer, 11, reinterpret_cast<void*>(HookedLock), reinterpret_cast<void**>(&g_originalLock));
    PatchSlot(vertexBuffer, 12, reinterpret_cast<void*>(HookedUnlock), reinterpret_cast<void**>(&g_originalUnlock));
}

void SetFrame(unsigned frame) { g_frame = frame; }

int DrawPositions(IDirect3DDevice9* device, UINT firstVertex, UINT vertexCount, Vector3* positions, int maximum, float* wValues) {
    if (!g_originalLock || !vertexCount || maximum <= 0) return 0;
    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT streamOffset = 0, stride = 0;
    if (FAILED(device->GetStreamSource(0, &buffer, &streamOffset, &stride)) || !buffer) return 0;
    buffer->Release();   // the device keeps it bound; only the pointer is kept

    Slot* slot = FindSlot(buffer);
    if (!slot) {
        // Only buffers the game rewrites (dynamic) have anything to read; static ones would just
        // push the useful ones out.
        D3DVERTEXBUFFER_DESC dynamicCheck = {};
        if (FAILED(buffer->GetDesc(&dynamicCheck)) || !(dynamicCheck.Usage & D3DUSAGE_DYNAMIC)) return 0;
        // Take a free slot, or the one wanted longest ago.
        Slot* oldest = &g_slots[0];
        for (Slot& candidate : g_slots) {
            if (!candidate.buffer) { oldest = &candidate; break; }
            if (candidate.lastWantedFrame < oldest->lastWantedFrame) oldest = &candidate;
        }
        slot = oldest;
        slot->buffer = nullptr;
        D3DVERTEXBUFFER_DESC description = {};
        buffer->GetDesc(&description);
        slot->bufferSize = description.Size;
        slot->positionOffset = PositionOffset(device);
        for (LockRecord& record : slot->records) record.frame = 0, record.sampleCount = 0;
        slot->locks = slot->draws = slot->drawsFound = 0;
        slot->lockPointer = nullptr;
        slot->buffer = buffer;
    }
    slot->stride = stride;
    slot->lastWantedFrame = g_frame;
    ++slot->draws;

    // The newest write(s) covering this draw's vertices, from this frame or the last.
    const UINT drawStart = streamOffset + firstVertex * stride;
    const UINT drawEnd = streamOffset + (firstVertex + vertexCount) * stride;
    unsigned newestFrame = 0;
    for (const LockRecord& record : slot->records)
        if (record.sampleCount && record.stride == stride && record.start < drawEnd && record.end > drawStart && record.frame > newestFrame)
            newestFrame = record.frame;
    if (!newestFrame || g_frame - newestFrame > 1) return 0;
    int used = 0;
    for (const LockRecord& record : slot->records) {
        if (record.frame != newestFrame || record.stride != stride || record.start >= drawEnd || record.end <= drawStart) continue;
        for (UINT sample = 0; sample < record.sampleCount; ++sample) {
            const UINT byte = record.sampleByte[sample];
            // Only samples lined up with this draw's vertices (else they straddle two vertices).
            if (byte < drawStart || byte >= drawEnd || (byte - streamOffset) % stride != 0) continue;
            if (used >= maximum) break;
            if (wValues) wValues[used] = record.position[sample][3];
            positions[used++] = { record.position[sample][0], record.position[sample][1], record.position[sample][2] };
        }
    }
    if (used) ++slot->drawsFound;
    return used;
}

bool DrawCentroid(IDirect3DDevice9* device, UINT firstVertex, UINT vertexCount, Vector3& centroid) {
    Vector3 positions[256];
    const int count = DrawPositions(device, firstVertex, vertexCount, positions, 256);
    if (!count) return false;
    double sum[3] = {};
    for (int index = 0; index < count; ++index) {
        sum[0] += positions[index].x;
        sum[1] += positions[index].y;
        sum[2] += positions[index].z;
    }
    centroid = { float(sum[0] / count), float(sum[1] / count), float(sum[2] / count) };
    return true;
}

int UserPointerPositions(IDirect3DDevice9* device, const void* vertexData, UINT stride, UINT firstVertex, UINT vertexCount,
                         Vector3* positions, int maximum, float* wValues) {
    if (!vertexData || !stride || !vertexCount || maximum <= 0) return 0;
    const UINT offset = PositionOffset(device);
    const UINT step = vertexCount > UINT(maximum) ? (vertexCount + maximum - 1) / maximum : 1;
    int used = 0;
    for (UINT vertex = firstVertex; vertex < firstVertex + vertexCount && used < maximum; vertex += step) {
        const float* position = reinterpret_cast<const float*>(static_cast<const BYTE*>(vertexData) + vertex * stride + offset);
        if (!Plausible(position[0]) || !Plausible(position[1]) || !Plausible(position[2])) continue;
        if (wValues) wValues[used] = stride >= offset + 16 && Plausible(position[3]) ? position[3] : 1.0f;
        positions[used++] = { position[0], position[1], position[2] };
    }
    return used;
}

void LogWatched() {
    if (!g_log) return;
    for (Slot& slot : g_slots) {
        if (!slot.buffer || g_frame - slot.lastWantedFrame > 60) continue;
        g_log("[mesh] buffer %p size %u stride %u position at +%u: %u writes (last flags %08lX), %u of %u draws found in them",
              (void*)slot.buffer, slot.bufferSize, slot.stride, slot.positionOffset, slot.locks, slot.lockFlags, slot.drawsFound, slot.draws);
        slot.locks = slot.draws = slot.drawsFound = 0;
    }
}

} // namespace MeshWatch
