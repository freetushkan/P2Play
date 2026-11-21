// PS4: ps4/crashlog.c installs its handlers by itself (a constructor, /data/p2play/crash.log);
// the CrashLog calls of the application do nothing here.
#include "../crashlog.h"

void p2p::CrashLog::install(const std::string &) {}

void p2p::CrashLog::registerCurrentThread() {}

void p2p::CrashLog::heartbeat() {}

void p2p::CrashLog::setIdle(bool) {}
