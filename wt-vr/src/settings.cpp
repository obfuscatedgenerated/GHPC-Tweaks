#include "settings.h"
#include "input.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

VrSettings  g_settings;
std::string g_iniPath;

namespace {

// Every setting: where it lives in vr.ini, its type and range. Loading, saving, the list at the top of
// the log and live reloading all go through this table. [vr]: everyday settings; [debug]: diagnostics
// and experiments (read from [vr] too, where older versions kept them).
enum class SettingType { Bool, Int, Float, Role };
struct SettingInfo {
    const char* section;
    const char* key;
    SettingType type;
    void* value;
    float minimum, maximum;
    bool restartToApply;
    std::string defaultText;   // filled in at start-up from VrSettings' own defaults
};
const char* const RoleNames[] = { "auto", "guitar", "bass", "drums", "vocals" };   // role + 1

std::vector<SettingInfo>& SettingTable() {
    static std::vector<SettingInfo> table = {
        { "vr", "world_scale",        SettingType::Float, &g_settings.worldScale,        0.05f, 20.0f, false, {} },
        { "vr", "render_scale",       SettingType::Float, &g_settings.renderScale,       0.25f, 2.0f,  true,  {} },
        { "vr", "near_plane",         SettingType::Float, &g_settings.nearPlane,         0.001f, 10.0f, false, {} },
        { "vr", "far_plane",          SettingType::Float, &g_settings.farPlane,          10.0f, 100000.0f, false, {} },
        { "vr", "stereo_mode",        SettingType::Bool,  &g_settings.fullStereo,        0, 1, false, {} },
        { "vr", "hud_visible",        SettingType::Bool,  &g_settings.showOverlay,       0, 1, false, {} },
        { "vr", "hud_distance",       SettingType::Float, &g_settings.overlay.distance,  0.2f, 10.0f, false, {} },
        { "vr", "hud_width",          SettingType::Float, &g_settings.overlay.width,     0.2f, 10.0f, false, {} },
        { "vr", "hud_height",         SettingType::Float, &g_settings.overlay.height,    -5.0f, 5.0f, false, {} },
        { "vr", "hud_depth",          SettingType::Bool,  &g_settings.hudDepth,          0, 1, false, {} },
        { "vr", "hud_depth_area",     SettingType::Float, &g_settings.hudDepthArea,      0.2f, 1.0f,  true,  {} },
        { "vr", "hud_gem_bulge",      SettingType::Float, &g_settings.hudGemBulge,       -0.2f, 0.2f, false, {} },
        { "vr", "mirror_hud",         SettingType::Int,   &g_settings.mirrorHud,         0, 2, false, {} },
        { "vr", "mirror_mode",        SettingType::Int,   &g_settings.mirrorMode,        0, 2, false, {} },
        { "vr", "vr_menus",           SettingType::Bool,  &g_settings.vrMenus,           0, 1, false, {} },
        { "vr", "menu_camera",        SettingType::Int,   &g_settings.menuCamera,        0, 2, false, {} },
        { "vr", "snap_turn_degrees",  SettingType::Float, &g_settings.snapTurnDegrees,   0.0f, 180.0f, false, {} },
        { "vr", "depth_of_field_off", SettingType::Bool,  &g_settings.depthOfFieldOff,   0, 1, false, {} },
        { "vr", "crowd_follow",       SettingType::Bool,  &g_settings.crowdFollow,       0, 1, false, {} },
        { "vr", "role",               SettingType::Role,  &g_settings.role,              -1, 3, false, {} },
        { "vr", "saved_spots",        SettingType::Bool,  &g_settings.savedSpots,        0, 1, false, {} },
        { "vr", "auto_spot",          SettingType::Bool,  &g_settings.autoSpot,          0, 1, false, {} },
        { "vr", "song_intro",         SettingType::Int,   &g_settings.songIntro,         0, 1, false, {} },
        { "vr", "intro_hop_seconds",  SettingType::Float, &g_settings.introHopSeconds,   0.0f, 60.0f, false, {} },
        { "vr", "band_member",        SettingType::Int,   &g_settings.bandMemberMode,    0, 2, false, {} },
        { "vr", "walk_to_pick",       SettingType::Bool,  &g_settings.walkToPick,        0, 1, false, {} },
        { "vr", "avatar_anchor",      SettingType::Int,   &g_settings.avatarAnchor,      0, 1, false, {} },
        { "vr", "avatar_offset",      SettingType::Float, &g_settings.avatarOffset,      -3.0f, 3.0f, false, {} },
        { "vr", "avatar_eye_height",  SettingType::Float, &g_settings.avatarEyeHeight,   0.5f, 3.0f, false, {} },
        { "vr", "avatar_head_drop",   SettingType::Float, &g_settings.avatarHeadDrop,    -1.0f, 1.0f, false, {} },
        { "vr", "avatar_pin_drop",    SettingType::Float, &g_settings.avatarPinDrop,     0.1f, 1.2f, false, {} },
        { "debug", "log_level",       SettingType::Int,   &g_settings.logLevel,          0, 2, false, {} },
        { "debug", "debug_tools",     SettingType::Bool,  &g_settings.debugTools,        0, 1, false, {} },
        { "debug", "save_textures",   SettingType::Bool,  &g_settings.saveTextures,      0, 1, false, {} },
        { "debug", "follow_head",     SettingType::Bool,  &g_settings.followHead,        0, 1, false, {} },
        { "debug", "wide_culling",    SettingType::Bool,  &g_settings.wideCulling,       0, 1, false, {} },
        { "debug", "culling_fov",     SettingType::Float, &g_settings.cullingFov,        60.0f, 170.0f, false, {} },
        { "debug", "avatar_lock_turn", SettingType::Bool, &g_settings.avatarLockTurn,    0, 1, false, {} },
        { "debug", "eye_force_opaque", SettingType::Bool, &g_settings.eyeForceOpaque,    0, 1, false, {} },
    };
    return table;
}

std::string SettingText(const SettingInfo& setting) {
    char text[32];
    switch (setting.type) {
        case SettingType::Bool:  return *static_cast<bool*>(setting.value) ? "1" : "0";
        case SettingType::Int:   return std::to_string(*static_cast<int*>(setting.value));
        case SettingType::Role:  return RoleNames[*static_cast<int*>(setting.value) + 1];
        case SettingType::Float: snprintf(text, sizeof(text), "%g", *static_cast<float*>(setting.value)); return text;
    }
    return {};
}

void SetSettingFromText(SettingInfo& setting, const char* text) {
    switch (setting.type) {
        case SettingType::Bool:  *static_cast<bool*>(setting.value) = atoi(text) != 0; break;
        case SettingType::Int:   *static_cast<int*>(setting.value) = std::min(int(setting.maximum), std::max(int(setting.minimum), atoi(text))); break;
        case SettingType::Float: *static_cast<float*>(setting.value) = std::min(setting.maximum, std::max(setting.minimum, float(atof(text)))); break;
        case SettingType::Role: {
            int role = -1;
            for (int index = 0; index < 5; ++index)
                if (_stricmp(text, RoleNames[index]) == 0) role = index - 1;
            *static_cast<int*>(setting.value) = role;
            break;
        }
    }
}

// The value as written in vr.ini ([debug] settings also from [vr], where older versions kept them); empty if missing.
std::string IniText(const SettingInfo& setting) {
    char text[64];
    GetPrivateProfileStringA(setting.section, setting.key, "", text, sizeof(text), g_iniPath.c_str());
    if (!text[0] && strcmp(setting.section, "vr") != 0) GetPrivateProfileStringA("vr", setting.key, "", text, sizeof(text), g_iniPath.c_str());
    return text;
}

FILETIME g_iniWriteTime = {};

FILETIME IniWriteTime() {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &attributes)) return {};
    return attributes.ftLastWriteTime;
}


} // namespace

// Reads vr.ini; a missing setting keeps its default and is written in (so the file always lists every setting).
void LoadSettings() {
    static bool defaultsKnown = false;
    if (!defaultsKnown) {
        for (SettingInfo& setting : SettingTable()) setting.defaultText = SettingText(setting);
        defaultsKnown = true;
    }
    for (SettingInfo& setting : SettingTable()) {
        const std::string text = IniText(setting);
        if (!text.empty()) SetSettingFromText(setting, text.c_str());
        else WritePrivateProfileStringA(setting.section, setting.key, SettingText(setting).c_str(), g_iniPath.c_str());
    }
    LoadKeys(true);
    g_iniWriteTime = IniWriteTime();
}

void SaveSettings() {
    for (SettingInfo& setting : SettingTable()) {
        WritePrivateProfileStringA(setting.section, setting.key, SettingText(setting).c_str(), g_iniPath.c_str());
        if (strcmp(setting.section, "vr") != 0) WritePrivateProfileStringA("vr", setting.key, nullptr, g_iniPath.c_str());   // moved out of [vr]
    }
    g_iniWriteTime = IniWriteTime();
}

// The settings in use, at the top of the log (so a log shows exactly what someone was running).
void LogSettings() {
    LogLine("[settings] %s:", g_iniPath.c_str());
    const char* section = "";
    for (const SettingInfo& setting : SettingTable()) {
        if (strcmp(section, setting.section) != 0) { section = setting.section; LogLine("[settings] [%s]", section); }
        const std::string value = SettingText(setting);
        LogLine("[settings]   %s=%s%s%s%s", setting.key, value.c_str(), value != setting.defaultText ? "   (default " : "",
                value != setting.defaultText ? setting.defaultText.c_str() : "", value != setting.defaultText ? ")" : "");
    }
}

bool ReloadSettingsIfChanged() {
    const FILETIME writeTime = IniWriteTime();
    if (CompareFileTime(&writeTime, &g_iniWriteTime) == 0) return false;
    std::vector<std::string> before;
    for (const SettingInfo& setting : SettingTable()) before.push_back(SettingText(setting));
    LoadSettings();
    for (size_t index = 0; index < SettingTable().size(); ++index) {
        const SettingInfo& setting = SettingTable()[index];
        const std::string after = SettingText(setting);
        if (after != before[index])
            LogLine("[settings] %s: %s -> %s%s", setting.key, before[index].c_str(), after.c_str(), setting.restartToApply ? " (restart the game to apply)" : "");
    }
    LogLine("[settings] vr.ini reloaded");
    return true;
}

