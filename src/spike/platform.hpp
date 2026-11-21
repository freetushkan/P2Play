#pragma once

#include <string>

namespace p2p {

    bool platformInit();

    void platformExit();

    // false once the user asked to quit
    bool platformRunning();

    std::string platformName();

    // writable directory for downloads, config and the log
    std::string dataDir();

    // console, system log or toast where the platform has one
    void platformPrint(const std::string &line);

    void platformNotify(const std::string &text);

    // consoles: fewer sockets, no UPnP/NAT-PMP
    bool platformIsConsole();

}
