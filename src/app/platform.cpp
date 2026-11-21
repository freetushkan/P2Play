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
    AppletHookCookie g_hook;
    std::atomic<bool> g_exitRequested{false};

    // the system asks to close (HOME, then close): as pPlay, with the exit locked until then
    void onAppletHook(AppletHookType hook, void *) {
        if (hook == AppletHookType_OnExitRequest) {
            g_exitRequested = true;
        }
    }
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
    appletUnhook(&g_hook);
    appletUnlockExit();
    socketExit();
    consoleExit(nullptr);
}

void p2p::platformStarted() {
    appletLockExit();
    appletHook(&g_hook, onAppletHook, nullptr);
    // HOME: the server keeps serving in the background
    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
}

void p2p::platformKeepAwake(bool on) {
    static int last = -1;
    if (static_cast<int>(on) != last) {
        appletSetAutoSleepDisabled(on);
        last = on;
    }
}

bool p2p::platformRunning() {
    if (!appletMainLoop() || g_exitRequested) {
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

std::string p2p::platformName() {
    std::string out = "Switch";
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysInitialize())) {
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            out += std::string(" ") + fw.display_version;
        }
        setsysExit();
    }
    out += appletGetAppletType() == AppletType_Application ? " (title)" : " (applet)";
    return out;
}

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

extern "C" {
int sceSystemServiceLoadExec(const char *path, const char *args[]);
int sceSystemServicePowerTick();
int sceSystemServiceTickVideoPlayback();
int sceSystemServiceTurnOffScreenSaver();
}

bool p2p::platformInit() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // the order of pPlay: the timezone in ps4_compat_init needs SystemService loaded
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    ps4_compat_init();
    mkdir(dataDir().c_str(), 0777);
    return true;
}

// back to the system, as pPlay (a plain return leaves the application hanging)
void p2p::platformExit() {
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool p2p::platformRunning() {
    struct stat st{};
    if (stat((dataDir() + "/stop").c_str(), &st) == 0) {
        remove((dataDir() + "/stop").c_str());
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return true;
}

void p2p::platformStarted() {
    char report[256];
    ps4_clock_report(report, sizeof(report));
    platformPrint(std::string("PS4 clocks: ") + report);
}

// the video playback tick: what video apps report to keep the console awake (the power
// tick alone is not enough), as pPlay
void p2p::platformKeepAwake(bool on) {
    static auto last = std::chrono::steady_clock::time_point();
    if (!on || std::chrono::steady_clock::now() - last < std::chrono::seconds(5)) {
        return;
    }
    last = std::chrono::steady_clock::now();
    sceSystemServiceTickVideoPlayback();
    sceSystemServicePowerTick();
    sceSystemServiceTurnOffScreenSaver();
}

std::string p2p::platformName() { return "PS4"; }

std::string p2p::dataDir() { return "/data/p2play"; }

void p2p::platformPrint(const std::string &line) {
    sceKernelDebugOutText(0, ("[p2play] " + line + "\n").c_str());
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

void p2p::platformStarted() {}

// a payload: no system ticks for it (pPlay keeps none on the PS5 either)
void p2p::platformKeepAwake(bool) {}

std::string p2p::platformName() { return "PS5"; }

std::string p2p::dataDir() { return "/data/p2play"; }

void p2p::platformPrint(const std::string &line) {
    klog_printf("[p2play] %s\n", line.c_str());
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
#include <cstdlib>

namespace {
    std::atomic<bool> g_quit{false};

    void onSignal(int) { g_quit = true; }
}

bool p2p::platformInit() {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);
    return true;
}

void p2p::platformExit() {}

bool p2p::platformRunning() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !g_quit;
}

void p2p::platformStarted() {}

// Android: the app's service holds a wake lock; Linux: a server
void p2p::platformKeepAwake(bool) {}

std::string p2p::platformName() {
#ifdef __ANDROID__
    return "Android";
#else
    return "Linux";
#endif
}

std::string p2p::dataDir() {
    if (const char *d = getenv("P2PLAY_DATA")) {
        return d;
    }
    if (const char *x = getenv("XDG_DATA_HOME")) {
        return std::string(x) + "/p2play";
    }
    if (const char *h = getenv("HOME")) {
        return std::string(h) + "/.local/share/p2play";
    }
    return "p2play-data";
}

void p2p::platformPrint(const std::string &line) {
    printf("%s\n", line.c_str());
    fflush(stdout);
}

void p2p::platformNotify(const std::string &) {}

bool p2p::platformIsConsole() { return false; }

#endif
