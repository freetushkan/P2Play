#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

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
#include "screen/console.hpp"

namespace {
    // the log on the screen (the application's own video out)
    p2p::TextConsole g_console;
}

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
    g_console.setTitle("P2Play: starting");
    p2p::screenStart(&g_console);
    return true;
}

// back to the system, as pPlay (a plain return leaves the application hanging)
void p2p::platformExit() {
    p2p::screenStop();
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
    g_console.add(line);
}

void p2p::platformNotify(const std::string &text) {
    g_console.setTitle(text);
}

bool p2p::platformIsConsole() { return true; }

#elif defined(__PS5__)

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <ps5/klog.h>

extern "C" int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);

// A payload has no system "already running": the icon opened again starts a second
// process next to the first (and an updated package runs beside the old server). As pPlay
// (its port 9095): the running server holds a loopback control port (UDP), which the system
// releases when the process ends, whatever way (a process name would outlive it: an ended
// payload stays listed until its parent reaps it). Opening the icon again finds the port
// taken: it asks the server to quit, waits for the port (kills the server by its thread name
// after 10 s) and exits; the next opening starts the new one.
namespace {
    constexpr const char *kThreadName = "p2play";
    constexpr uint16_t kControlPort = 9097;   // pPlay holds 9095
    int g_control = -1;

    sockaddr_in loopback(uint16_t port) {
        sockaddr_in addr{};
        addr.sin_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return addr;
    }

    // the control port bound, -1 if taken. UDP: nothing of it outlives the process (a TCP
    // port can stay in TIME_WAIT), no SO_REUSEADDR: a second one must fail.
    int listenControl() {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_in addr = loopback(kControlPort);
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        return fd;
    }

    // the pid of another process whose (main) thread is named so, -1 if none (pPlay's hbldr)
    pid_t findOther(const char *name) {
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
        size_t size = 0;
        if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) {
            return -1;
        }
        size += size / 4;
        std::vector<uint8_t> buf(size);
        if (sysctl(mib, 4, buf.data(), &size, nullptr, 0) != 0) {
            return -1;
        }
        const pid_t self = getpid();
        pid_t found = -1;
        for (size_t off = 0; off + 460 <= size;) {
            const uint8_t *p = buf.data() + off;
            int structSize;
            pid_t pid;
            std::memcpy(&structSize, p, sizeof(structSize));
            std::memcpy(&pid, p + 72, sizeof(pid));
            if (structSize <= 0) {
                break;
            }
            if (pid != self && std::strncmp(reinterpret_cast<const char *>(p + 447), name, 20) == 0) {
                found = pid;
            }
            off += static_cast<size_t>(structSize);
        }
        return found;
    }

    // a GET on 127.0.0.1; the answer's start
    std::string localGet(uint16_t port, const char *path) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return "";
        }
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in addr = loopback(port);
        std::string out;
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            std::string req = std::string("GET ") + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
            if (send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size())) {
                char b[512];
                ssize_t n;
                while (out.size() < 4096 && (n = recv(fd, b, sizeof(b), 0)) > 0) {
                    out.append(b, static_cast<size_t>(n));
                }
            }
        }
        close(fd);
        return out;
    }

    // the running server asked to quit; true once its port is free (killed after 10 s)
    bool stopRunning() {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            sockaddr_in addr = loopback(kControlPort);
            sendto(fd, "quit", 4, 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            close(fd);
        }
        for (int i = 0; i < 80; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (i == 40) {
                pid_t other = findOther(kThreadName);
                klog_printf("[p2play] the running server did not quit in 10 s: killing pid %d\n", static_cast<int>(other));
                if (other > 0) {
                    kill(other, SIGKILL);
                }
            }
            int probe = listenControl();
            if (probe >= 0) {
                close(probe);
                return true;
            }
        }
        return false;
    }
}

bool p2p::platformInit() {
    signal(SIGPIPE, SIG_IGN);
    syscall(SYS_thr_set_name, -1, kThreadName);
    mkdir(dataDir().c_str(), 0777);

    g_control = listenControl();
    if (g_control < 0) {
        // opened again: the running server stops, this process too (before the log is
        // opened, it is the other's)
        klog_printf("[p2play] already running: stopping it\n");
        platformNotify(stopRunning() ? "P2Play: stopped. Open it again to start"
                                     : "P2Play: the running server does not stop");
        return false;
    }
    // a server from before the control port: its MatriX /shutdown
    if (localGet(8090, "/echo").find("P2Play/") != std::string::npos) {
        klog_printf("[p2play] an older server answers on 8090: /shutdown\n");
        localGet(8090, "/shutdown");
        bool gone = false;
        for (int i = 0; i < 40 && !gone; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            gone = localGet(8090, "/echo").find("P2Play/") == std::string::npos;
        }
        close(g_control);
        g_control = -1;
        platformNotify(gone ? "P2Play: stopped. Open it again to start"
                            : "P2Play: an older server holds port 8090 and does not stop: restart the console");
        return false;
    }
    return true;
}

void p2p::platformExit() {
    if (g_control >= 0) {
        close(g_control);
        g_control = -1;
    }
}

bool p2p::platformRunning() {
    struct stat st{};
    if (stat((dataDir() + "/stop").c_str(), &st) == 0) {
        remove((dataDir() + "/stop").c_str());
        return false;
    }
    // another opening of the icon
    if (g_control >= 0) {
        char b[16];
        if (recv(g_control, b, sizeof(b), 0) >= 4 && std::memcmp(b, "quit", 4) == 0) {
            platformPrint("[App] opened again: quitting");
            return false;
        }
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
