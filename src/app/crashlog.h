// Fatal crash diagnostics, pPlay's (src/*/crashlog.*) for a headless application: when the
// core is a pPlay module, pPlay's own crash handlers are the ones installed.
//   Linux:   fatal signals, a backtrace with symbols (linux/crashlog.cpp)
//   Android: fatal signals with an unwound stack, a stuck main loop, the previous logcat
//            (android/crashlog.cpp)
//   Switch:  the libnx exception handler (switch/crashlog.cpp)
//   PS4:     ps4/crashlog.c (installed by itself, /data/p2play/crash.log)
//   PS5:     fatal signals with a backtrace (ps5/crashlog.cpp)
// Offsets: llvm-addr2line -f -C -e <the unstripped binary (CI: P2Play-Symbols-*)> <offset>
#pragma once

#include <string>

namespace p2p::CrashLog {

    void install(const std::string &logPath);

    // Linux: the alternate signal stack of the calling thread
    void registerCurrentThread();

    // Android: the main loop alive (a stuck one: its place written)
    void heartbeat();

    void setIdle(bool idle);
}
