#include "part_scan.h"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// A script structure field is a run of words: [link] [type] <name key> <value> [next] ...
// Player settings, as found in the game's memory:
//   "part" <part key>, then "?" <value> four words on, then "band_member" four more on.
constexpr int SecondFieldOffset = 4;       // words from the "part" key to the next field's name
constexpr int BandMemberFieldOffset = 8;   // ...and to "band_member"'s
constexpr DWORD PlayerStructureStride = 0x9E0;   // the players' settings sit this far apart

PartScanLogFunction g_log = nullptr;
DWORD g_partKey = 0, g_secondFieldKey = 0xEEC9418A, g_bandMemberKey = 0;
DWORD g_partValueKeys[4] = {};   // guitar, bass, drum, vocals
const char* const PartNames[4] = { "guitar", "bass", "drums", "vocals" };

CRITICAL_SECTION g_lock;               // guards g_pendingLines
std::vector<std::string> g_pendingLines;
volatile LONG g_searching = 0;
volatile DWORD g_partAddress = 0;      // where player 1's "part" key is (0: not found)

DWORD QbKey(const char* text) {
    DWORD crc = 0xFFFFFFFF;
    for (const char* character = text; *character; ++character) {
        crc ^= DWORD(std::tolower(static_cast<unsigned char>(*character)));
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

void AddLine(const char* format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    EnterCriticalSection(&g_lock);
    g_pendingLines.push_back(text);
    LeaveCriticalSection(&g_lock);
}

int PartFromKey(DWORD key) {
    for (int part = 0; part < 4; ++part)
        if (g_partValueKeys[part] == key) return part;
    return -1;
}

// The words from the "part" key to "band_member"'s, if they're there and look right.
bool LooksLikePlayerSettings(const DWORD* words) {
    return words[0] == g_partKey && PartFromKey(words[1]) >= 0 && words[SecondFieldOffset] == g_secondFieldKey &&
           words[BandMemberFieldOffset] == g_bandMemberKey;
}

bool ReadWords(DWORD address, DWORD* words, size_t count) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(address)), words, count * sizeof(DWORD), &bytesRead) &&
           bytesRead == count * sizeof(DWORD);
}

bool IsWritable(const MEMORY_BASIC_INFORMATION& region) {
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = region.Protect & 0xFF;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

DWORD WINAPI SearchThread(void*) {
    const DWORD startTick = GetTickCount();
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    const unsigned long long lastAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    constexpr SIZE_T ChunkSize = 1 << 20;
    constexpr size_t Overlap = BandMemberFieldOffset + 1;   // words carried over so a match can span chunks
    std::vector<DWORD> buffer(ChunkSize / sizeof(DWORD) + Overlap);
    std::vector<DWORD> matches;
    MEMORY_BASIC_INFORMATION region;
    for (unsigned long long address = 0x10000; address < lastAddress && matches.size() < 256;) {
        if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(address)), &region, sizeof(region))) break;
        const unsigned long long regionStart = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const unsigned long long regionEnd = regionStart + region.RegionSize;
        if (regionEnd <= address) break;
        if (IsWritable(region)) {
            for (unsigned long long chunk = regionStart; chunk < regionEnd; chunk += ChunkSize) {
                const SIZE_T bytes = SIZE_T(std::min<unsigned long long>(ChunkSize + Overlap * sizeof(DWORD), regionEnd - chunk));
                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(chunk)), buffer.data(), bytes, &bytesRead)) continue;
                const size_t count = bytesRead / sizeof(DWORD);
                const size_t ownWords = std::min<size_t>(count, ChunkSize / sizeof(DWORD));
                for (size_t index = 0; index < ownWords && index + BandMemberFieldOffset < count; ++index)
                    if (buffer[index] == g_partKey && LooksLikePlayerSettings(&buffer[index]))
                        matches.push_back(DWORD(chunk) + DWORD(index * sizeof(DWORD)));
            }
        }
        address = regionEnd;
    }
    // The players' settings are a run of structures PlayerStructureStride apart; player 1's is the first.
    std::sort(matches.begin(), matches.end());
    DWORD playerOne = 0;
    unsigned bestRun = 0;
    for (size_t first = 0; first < matches.size(); ++first) {
        unsigned run = 1;
        for (size_t next = first + 1; next < matches.size(); ++next)
            if (matches[next] == matches[first] + run * PlayerStructureStride) ++run;
        if (run > bestRun) { bestRun = run; playerOne = matches[first]; }
    }
    if (playerOne) {
        DWORD words[BandMemberFieldOffset + 1];
        const int part = ReadWords(playerOne, words, BandMemberFieldOffset + 1) ? PartFromKey(words[1]) : -1;
        AddLine("[part] found the players' settings (%u of them; player 1's at %08lX) in %lu ms: player 1 plays %s", bestRun, playerOne,
                GetTickCount() - startTick, part >= 0 ? PartNames[part] : "?");
    } else {
        AddLine("[part] players' settings not found (%u ms, %u candidates): role= in vr.ini is used", unsigned(GetTickCount() - startTick),
                unsigned(matches.size()));
    }
    g_partAddress = playerOne;
    InterlockedExchange(&g_searching, 0);
    return 0;
}

} // namespace

namespace PartScan {

void Initialise(PartScanLogFunction log) {
    if (!g_log) InitializeCriticalSection(&g_lock);
    g_log = log;
    g_partKey = QbKey("part");
    g_bandMemberKey = QbKey("band_member");
    g_partValueKeys[0] = QbKey("guitar");
    g_partValueKeys[1] = QbKey("bass");
    g_partValueKeys[2] = QbKey("drum");
    g_partValueKeys[3] = QbKey("vocals");
}

void Find() {
    if (g_partAddress) {
        DWORD words[BandMemberFieldOffset + 1];
        if (ReadWords(g_partAddress, words, BandMemberFieldOffset + 1) && LooksLikePlayerSettings(words)) return;   // still there
        g_partAddress = 0;
    }
    if (InterlockedCompareExchange(&g_searching, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, SearchThread, nullptr, 0, nullptr);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_searching, 0);
    }
}

Part CurrentPart() {
    const DWORD address = g_partAddress;
    if (!address) return Unknown;
    DWORD words[BandMemberFieldOffset + 1];
    if (!ReadWords(address, words, BandMemberFieldOffset + 1) || !LooksLikePlayerSettings(words)) return Unknown;
    return Part(PartFromKey(words[1]));
}

void Tick() {
    if (!g_log) return;
    std::vector<std::string> lines;
    EnterCriticalSection(&g_lock);
    lines.swap(g_pendingLines);
    LeaveCriticalSection(&g_lock);
    for (const std::string& line : lines) g_log("%s", line.c_str());
}

} // namespace PartScan
