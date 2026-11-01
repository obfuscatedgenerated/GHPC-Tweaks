// The mod's log, ghwt_vr\vr_log.txt (the previous launch's is kept as vr_log_previous.txt).
// Lines start with a [tag]; log_level in vr.ini decides which are written (see log.cpp).
#pragma once

#include <string>

void OpenLog(const std::string& folder);
void CloseLog();
void LogLine(const char* format, ...);
