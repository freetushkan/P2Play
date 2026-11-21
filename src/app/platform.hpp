#pragma once

#include <string>

namespace p2p {

    bool platformInit();

    void platformExit();

    // false once the user asked to quit
    bool platformRunning();

    // as pPlay reports it: "Switch 19.0.1 (title)", "PS4", "PS5", "Android", "Linux"
    std::string platformName();

    // writable directory for downloads, config and the log
    std::string dataDir();

    // console, system log or toast where the platform has one
    void platformPrint(const std::string &line);

    void platformNotify(const std::string &text);

    // consoles: fewer sockets, no UPnP/NAT-PMP
    bool platformIsConsole();

    // after the core started (hooks, the log lines of pPlay)
    void platformStarted();

    // streams are served: the console kept awake (as pPlay while playing)
    void platformKeepAwake(bool on);

}
