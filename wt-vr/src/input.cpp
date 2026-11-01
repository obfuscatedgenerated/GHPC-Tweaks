#include "input.h"
#include "log.h"
#include "settings.h"

#include <windows.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {


struct KeyBinding {
    const char* name;
    const char* defaultText;
    bool debugOnly;
    int key = 0;   // virtual key (0: none)
    bool control = false, shift = false, alt = false;
};
KeyBinding g_keys[ActionCount] = {
    { "recenter", "F12", false }, { "save_settings", "F11", false }, { "toggle_hud", "F1", false },
    { "toggle_vr_menus", "Shift+F1", false }, { "mirror_hud", "F7", false },
    { "world_scale_down", "F2", false }, { "world_scale_up", "F3", false },
    { "hud_closer", "Home", false }, { "hud_further", "End", false }, { "hud_wider", "Insert", false }, { "hud_narrower", "Delete", false },
    { "hud_higher", "PageUp", false }, { "hud_lower", "PageDown", false }, { "toggle_depth_of_field", "F4", false },
    { "save_spot", "F8", false }, { "forget_spot", "Shift+F8", false },
    { "pick_band_member", "H", false }, { "band_member_mode", "Shift+H", false },
    { "band_member_up", "Shift+]", false }, { "band_member_down", "Shift+[", false },
    { "band_member_forward", "Ctrl+]", false }, { "band_member_back", "Ctrl+[", false }, { "xr_retry", "F10", false },
    { "move_forward", "W", false }, { "move_back", "S", false }, { "move_left", "A", false }, { "move_right", "D", false },
    { "move_up", "R", false }, { "move_down", "F", false }, { "turn_left", "Q", false }, { "turn_right", "E", false },
    { "move_fast", "Shift", false },
    { "toggle_follow_head", "Shift+F6", true }, { "toggle_wide_culling", "F6", true },
    { "culling_fov_down", "[", true }, { "culling_fov_up", "]", true }, { "camera_scan", "F5", true },
    { "camera_scan_list", "Shift+F7", true }, { "hud_dump", "Ctrl+F7", true }, { "role_probe", "Ctrl+F6", true },
    { "finder_next", "Ctrl+F9", true }, { "finder_previous", "Ctrl+F10", true }, { "test_pattern", "Shift+F9", true },
};

int VirtualKeyFromName(const std::string& name) {
    if (name.size() == 1) {
        const char character = char(toupper(static_cast<unsigned char>(name[0])));
        if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) return character;
        if (character == '[') return VK_OEM_4;
        if (character == ']') return VK_OEM_6;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3 && isdigit(static_cast<unsigned char>(name[1]))) {
        const int number = atoi(name.c_str() + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    if (_strnicmp(name.c_str(), "Num", 3) == 0 && name.size() == 4 && isdigit(static_cast<unsigned char>(name[3]))) return VK_NUMPAD0 + (name[3] - '0');
    static const struct { const char* name; int key; } named[] = {
        { "Home", VK_HOME }, { "End", VK_END }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "PageUp", VK_PRIOR },
        { "PageDown", VK_NEXT }, { "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL },
        { "Alt", VK_MENU }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    };
    for (const auto& entry : named)
        if (_stricmp(name.c_str(), entry.name) == 0) return entry.key;
    return 0;
}

// "Ctrl+Shift+F7" -> key + modifiers; false if it can't be read.
bool ParseBinding(const std::string& text, KeyBinding& binding) {
    binding.key = 0;
    binding.control = binding.shift = binding.alt = false;
    if (_stricmp(text.c_str(), "none") == 0 || text.empty()) return true;
    std::vector<std::string> parts;
    size_t begin = 0;
    // "+" splits, except a key that is "+" itself isn't supported; "[" / "]" are fine.
    for (size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == '+') {
            std::string part = text.substr(begin, index - begin);
            while (!part.empty() && part.front() == ' ') part.erase(part.begin());
            while (!part.empty() && part.back() == ' ') part.pop_back();
            if (!part.empty()) parts.push_back(part);
            begin = index + 1;
        }
    }
    if (parts.empty()) return false;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        if (_stricmp(parts[index].c_str(), "Ctrl") == 0) binding.control = true;
        else if (_stricmp(parts[index].c_str(), "Shift") == 0) binding.shift = true;
        else if (_stricmp(parts[index].c_str(), "Alt") == 0) binding.alt = true;
        else return false;
    }
    binding.key = VirtualKeyFromName(parts.back());
    return binding.key != 0;
}

// Key states, worked out once a frame (so several bindings can share a key).
bool g_keyDown[256] = {}, g_keyPressedNow[256] = {};

bool ActionAllowed(Action action) { return g_keys[action].key && (!g_keys[action].debugOnly || g_settings.debugTools); }

} // namespace

void LoadKeys(bool writeMissing) {
    for (KeyBinding& binding : g_keys) {
        char text[64];
        GetPrivateProfileStringA("keys", binding.name, "", text, sizeof(text), g_iniPath.c_str());
        if (!text[0]) {
            if (writeMissing) WritePrivateProfileStringA("keys", binding.name, binding.defaultText, g_iniPath.c_str());
            ParseBinding(binding.defaultText, binding);
        } else if (!ParseBinding(text, binding)) {
            LogLine("[settings] [keys] %s=%s isn't a key I know: using %s", binding.name, text, binding.defaultText);
            ParseBinding(binding.defaultText, binding);
        }
    }
}

void UpdateKeyStates() {
    for (int key = 1; key < 256; ++key) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        g_keyPressedNow[key] = down && !g_keyDown[key];
        g_keyDown[key] = down;
    }
}

bool KeyDown(int virtualKey) { return g_keyDown[virtualKey & 0xFF]; }

bool ActionPressed(Action action) {
    if (!ActionAllowed(action)) return false;
    const KeyBinding& binding = g_keys[action];
    return g_keyPressedNow[binding.key] && KeyDown(VK_CONTROL) == binding.control && KeyDown(VK_SHIFT) == binding.shift &&
           KeyDown(VK_MENU) == binding.alt;
}

bool ActionHeld(Action action) { return ActionAllowed(action) && g_keyDown[g_keys[action].key]; }
bool ActionTapped(Action action) { return ActionAllowed(action) && g_keyPressedNow[g_keys[action].key]; }

bool GameHasFocus() {
    HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

