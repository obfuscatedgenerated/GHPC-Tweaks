#include "log.h"
#include "settings.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

FILE* g_log = nullptr;

bool StartsWith(const char* text, const char* prefix) { return strncmp(text, prefix, strlen(prefix)) == 0; }

// log_level 2: everything. 1 (default): not the every-few-seconds diagnostics. 0: only start-up,
// settings, problems and the headset connection.
bool LogLevelAllows(const char* format) {
    if (g_settings.logLevel >= 2 || format[0] != '[') return true;
    static const char* const periodic[] = { "[status]", "[timing]", "[full stereo]", "[dof] depth-of-field draws", "[follow]",
                                            "[camera hooks]", "[mesh]", "[cut]", "[band] their head", "[menu camera] frame",
                                            "[targets]", "[render loop]", "[access capture]", "[readback]" };
    for (const char* prefix : periodic)
        if (StartsWith(format, prefix)) return false;
    if (g_settings.logLevel >= 1) return true;
    static const char* const essential[] = { "[settings]", "[xr]", "[device]", "[crash]" };
    for (const char* prefix : essential)
        if (StartsWith(format, prefix)) return true;
    return false;
}

} // namespace

void OpenLog(const std::string& folder) {
    const std::string path = folder + "\\vr_log.txt", previousPath = folder + "\\vr_log_previous.txt";
    MoveFileExA(path.c_str(), previousPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = fopen(path.c_str(), "w");
}

void CloseLog() {
    if (!g_log) return;
    fclose(g_log);
    g_log = nullptr;
}

void LogLine(const char* format, ...) {
    if (!g_log || !LogLevelAllows(format)) return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(g_log, format, arguments);
    va_end(arguments);
    fputc('\n', g_log);
    fflush(g_log);
}
