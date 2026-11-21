// Fatal crash diagnostics (from pPlay): Switch (libnx exception handler) and PS5 (fatal
// signals with a backtrace) write to the given file; PS4 has ps4/crashlog.c.
#pragma once

#include <string>

namespace p2p::CrashLog {
    void install(const std::string &logPath);
}
