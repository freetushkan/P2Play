#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <sys/stat.h>

#if defined(__SWITCH__)

#include <switch.h>

namespace {
    PadState g_pad;
    std::mutex g_consoleMutex;
}

bool p2p::platformInit() {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.tcp_tx_buf_size = 0x20000;
    cfg.tcp_rx_buf_size = 0x20000;
    cfg.tcp_tx_buf_max_size = 0x40000;
    cfg.tcp_rx_buf_max_size = 0x80000;
    cfg.udp_tx_buf_size = 0x4000;
    cfg.udp_rx_buf_size = 0x20000;
    cfg.sb_efficiency = 8;
    if (R_FAILED(socketInitialize(&cfg))) {
        printf("socketInitialize failed\n");
        consoleUpdate(nullptr);
        return false;
    }
    mkdir("sdmc:/switch", 0777);
    mkdir(dataDir().c_str(), 0777);
    return true;
}

void p2p::platformExit() {
    socketExit();
    consoleExit(nullptr);
}

bool p2p::platformRunning() {
    if (!appletMainLoop()) {
        return false;
    }
    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        consoleUpdate(nullptr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
}

std::string p2p::platformName() { return "switch"; }

std::string p2p::dataDir() { return "sdmc:/switch/p2play"; }

void p2p::platformPrint(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    printf("%s\n", line.c_str());
}

void p2p::platformNotify(const std::string &) {}

bool p2p::platformIsConsole() { return true; }

#elif defined(__PS4__)

#include <csignal>
#include <unistd.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include "ps4_compat.h"

bool p2p::platformInit() {
    // the order of pPlay: the timezone in ps4_compat_init needs SystemService loaded
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    ps4_compat_init();
    mkdir(dataDir().c_str(), 0777);
    return true;
}

void p2p::platformExit() {}

bool p2p::platformRunning() {
    struct stat st{};
    if (stat((dataDir() + "/stop").c_str(), &st) == 0) {
        remove((dataDir() + "/stop").c_str());
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return true;
}

std::string p2p::platformName() { return "ps4"; }

std::string p2p::dataDir() { return "/data/p2play"; }

void p2p::platformPrint(const std::string &line) {
    sceKernelDebugOutText(0, ("[p2spike] " + line + "\n").c_str());
}

void p2p::platformNotify(const std::string &) {}

bool p2p::platformIsConsole() { return true; }

#elif defined(__PS5__)

#include <csignal>
#include <unistd.h>
#include <ps5/klog.h>

extern "C" int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);

bool p2p::platformInit() {
    signal(SIGPIPE, SIG_IGN);
    mkdir(dataDir().c_str(), 0777);
    return true;
}

void p2p::platformExit() {}

bool p2p::platformRunning() {
    struct stat st{};
    if (stat((dataDir() + "/stop").c_str(), &st) == 0) {
        remove((dataDir() + "/stop").c_str());
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return true;
}

std::string p2p::platformName() { return "ps5"; }

std::string p2p::dataDir() { return "/data/p2play"; }

void p2p::platformPrint(const std::string &line) {
    klog_printf("[p2spike] %s\n", line.c_str());
}

void p2p::platformNotify(const std::string &text) {
    struct {
        char unused[45];
        char message[3075];
    } request{};
    snprintf(request.message, sizeof(request.message), "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

bool p2p::platformIsConsole() { return true; }

#else

#include <csignal>

namespace {
    std::atomic<bool> g_quit{false};

    void onSignal(int) { g_quit = true; }
}

bool p2p::platformInit() {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);
    mkdir(dataDir().c_str(), 0777);
    return true;
}

void p2p::platformExit() {}

bool p2p::platformRunning() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !g_quit;
}

std::string p2p::platformName() { return "linux"; }

std::string p2p::dataDir() {
    const char *env = getenv("P2SPIKE_DATA");
    return env ? env : "p2play-data";
}

void p2p::platformPrint(const std::string &line) {
    printf("%s\n", line.c_str());
    fflush(stdout);
}

void p2p::platformNotify(const std::string &) {}

bool p2p::platformIsConsole() { return false; }

#endif
