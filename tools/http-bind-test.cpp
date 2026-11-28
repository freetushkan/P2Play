#include "http.hpp"
#include "p2play.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>

namespace {
    bool check(bool condition, const char *message) {
        std::cout << (condition ? "OK   " : "FAIL ") << message << '\n';
        return condition;
    }

    bool connectTo(const std::string &address, uint16_t port) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
            close(fd);
            return false;
        }
        const bool connected = connect(fd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) == 0;
        close(fd);
        return connected;
    }

    std::string externalAddress() {
        ifaddrs *interfaces = nullptr;
        if (getifaddrs(&interfaces) != 0) return {};
        std::string result;
        for (ifaddrs *it = interfaces; it != nullptr; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET ||
                (it->ifa_flags & IFF_LOOPBACK) != 0) continue;
            char text[INET_ADDRSTRLEN]{};
            const auto *addr = reinterpret_cast<const sockaddr_in *>(it->ifa_addr);
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) && std::string(text) != "0.0.0.0") {
                result = text;
                break;
            }
        }
        freeifaddrs(interfaces);
        return result;
    }
}

int main() {
    const std::string external = externalAddress();
    if (external.empty()) {
        std::cerr << "FAIL no non-loopback IPv4 interface is available for the bind regression test\n";
        return 1;
    }

    p2p::HttpServer server;
    const auto handler = [](const p2p::HttpRequest &, p2p::HttpResponse &response) {
        response.text(200, "ok");
    };
    bool ok = true;

    ok &= check(server.start(0, handler), "HTTP server starts on any free port");
    if (server.port() == 0) return 1;
    ok &= check(connectTo("127.0.0.1", server.port()), "default listener accepts loopback connections");
    ok &= check(connectTo(external, server.port()), "default listener remains reachable on the network interface");
    server.stop();

    ok &= check(server.start(0, handler, true), "loopback-only HTTP server starts on any free port");
    if (server.port() == 0) return 1;
    ok &= check(connectTo("127.0.0.1", server.port()), "loopback-only listener accepts localhost connections");
    ok &= check(!connectTo(external, server.port()), "loopback-only listener rejects non-loopback interface connections");
    server.stop();

    p2play_config config{};
    config.data_dir = "/tmp/p2play-invalid-port-test";
    config.http_port = -2;
    ok &= check(p2play_start(&config) == -1, "C API rejects HTTP ports below the -1 auto-port sentinel");
    config.http_port = 65536;
    ok &= check(p2play_start(&config) == -1, "C API rejects HTTP ports above 65535");

    return ok ? 0 : 1;
}
