// P2Play: the core as an application. Linux and Android: the service p2playd (systemd, the
// Android app's foreground service); Switch, PS4, PS5: a homebrew application. It serves
// the TorrServer MatriX API and the streams on port 8090; players (pPlay, Lampa, mpv) use it
// there, on this device or over the network.
//
//   Linux, Android: p2playd [--data DIR] [--port N]
//     DIR: $P2PLAY_DATA, else $XDG_DATA_HOME/p2play or ~/.local/share/p2play
//   Switch: sdmc:/switch/p2play, + quits. PS4, PS5: /data/p2play, quits on a "stop" file there.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <exception>

#include "crashlog.h"
#include "p2play.h"
#include "platform.hpp"

#if defined(__SWITCH__) || defined(__PS4__) || defined(__PS5__)
#define P2P_CONSOLE 1
#else
#define P2P_CONSOLE 0
#endif

namespace {
    std::FILE *g_log = nullptr;
    std::mutex g_logMutex;

    void logLine(const char *line, void *) {
        char stamp[32];
        std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::lock_guard<std::mutex> lock(g_logMutex);
        p2p::platformPrint(std::string(stamp) + " " + line);
        if (g_log) {
            std::fprintf(g_log, "%s %s\n", stamp, line);
            std::fflush(g_log);
        }
    }

    // mkdir -p
    void makeDirs(const std::string &path) {
        for (size_t i = 1; i <= path.size(); i++) {
            if (i == path.size() || path[i] == '/') {
                mkdir(path.substr(0, i).c_str(), 0755);
            }
        }
    }

    // an uncaught exception: its reason in the log, then abort() (the crash handler)
    void onTerminate() {
        std::string what = "unknown";
        if (std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception &ex) {
                what = ex.what();
            } catch (...) {
            }
        }
        logLine(("[App] uncaught exception: " + what).c_str(), nullptr);
        std::abort();
    }

    // the address other devices reach this one at
    std::string localAddress() {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            return "127.0.0.1";
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53);
        inet_pton(AF_INET, "1.1.1.1", &addr.sin_addr);
        std::string out = "127.0.0.1";
        if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            sockaddr_in self{};
            socklen_t len = sizeof(self);
            char buf[INET_ADDRSTRLEN];
            if (getsockname(s, reinterpret_cast<sockaddr *>(&self), &len) == 0 &&
                inet_ntop(AF_INET, &self.sin_addr, buf, sizeof(buf))) {
                out = buf;
            }
        }
        close(s);
        return out;
    }
}

int main(int argc, char **argv) {
    std::string dataDir = p2p::dataDir();
    int port = 0;
#if !P2P_CONSOLE
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--version") {
            std::printf("%s\n", p2play_version());
            return 0;
        } else {
            std::fprintf(stderr, "usage: p2playd [--data DIR] [--port N]\n");
            return 2;
        }
    }
#else
    (void) argc;
    (void) argv;
#endif
    if (!p2p::platformInit()) {
        p2p::platformExit();
        return 1;
    }
    makeDirs(dataDir);
    g_log = std::fopen((dataDir + "/p2play.log").c_str(), P2P_CONSOLE ? "w" : "a");
    std::set_terminate(onTerminate);
#ifndef __PS4__
    // PS4: ps4/crashlog.c installs itself (/data/p2play/crash.log)
    p2p::CrashLog::install(dataDir + "/crash.log");
#endif

    p2play_config config{};
    config.data_dir = dataDir.c_str();
    config.http_port = port;
    config.console = p2p::platformIsConsole() ? 1 : 0;
    const std::string platform = p2p::platformName();
    config.platform = platform.c_str();
    config.log = logLine;
    if (p2play_start(&config) != 0) {
        logLine("[App] cannot start (already running?)", nullptr);
        p2p::platformNotify("P2Play: cannot start (already running?)");
#ifdef __SWITCH__
        // the message stays on the screen until +
        while (p2p::platformRunning()) {
        }
#endif
        p2p::platformExit();
        return 1;
    }
    std::string url = "http://" + localAddress() + ":" + std::to_string(p2play_http_port());
    logLine(("[App] P2Play " APP_VERSION_FULL " on " + platform + ", data in " + dataDir + ", " + url).c_str(), nullptr);
    p2p::platformStarted();
    p2p::platformNotify("P2Play: " + url);

    while (p2p::platformRunning() && !p2play_shutdown_requested()) {
        p2p::CrashLog::heartbeat();
        p2p::platformKeepAwake(p2play_active_streams() > 0);
    }
    logLine("[App] quitting", nullptr);
    p2play_stop();
    if (g_log) {
        std::fclose(g_log);
    }
    p2p::platformExit();
    return 0;
}
