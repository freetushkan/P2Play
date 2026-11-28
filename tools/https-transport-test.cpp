#include "https.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {
    struct Response {
        int status = 0;
        std::string body;
        std::string type;
        std::string location;
        std::string error;
    };

    Response get(const std::string &url, const std::string &resolve, size_t capacity = 2u * 1024u * 1024u) {
        Response result;
        result.body.resize(capacity);
        size_t size = 0;
        char type[256]{}, location[2048]{}, error[512]{};
        result.status = p2p::httpsGet(url.c_str(), resolve.c_str(), result.body.data(), result.body.size(),
                                      &size, type, sizeof(type), location, sizeof(location),
                                      error, sizeof(error), nullptr);
        if (size <= result.body.size()) result.body.resize(size);
        result.type = type;
        result.location = location;
        result.error = error;
        return result;
    }

    bool require(bool condition, const char *message) {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: p2play_https_test PORT CA_FILE\n";
        return 2;
    }
    const int port = std::atoi(argv[1]);
    if (port < 1 || port > 65535) return 2;
    const std::string caFile = argv[2];
    const std::string origin = "https://localhost:" + std::to_string(port);
    const std::string pin = "localhost:" + std::to_string(port) + ":127.0.0.1";

    unsetenv("P2PLAY_HTTPS_TEST_CA");
    if (!p2p::httpsInit("", 0, "", nullptr)) {
        std::cerr << "FAIL: HTTPS transport initialisation\n";
        return 1;
    }

    bool ok = true;
    const Response untrusted = get(origin + "/ok", pin);
    ok &= require(untrusted.status == 0 && !untrusted.error.empty(),
                  "untrusted CA certificate must fail verification");

    if (setenv("P2PLAY_HTTPS_TEST_CA", caFile.c_str(), 1) != 0) {
        std::cerr << "FAIL: cannot configure test CA\n";
        p2p::httpsShutdown();
        return 1;
    }

    const Response success = get(origin + "/ok", pin);
    ok &= require(success.status == 200, "trusted HTTPS request returns HTTP 200");
    ok &= require(success.type.find("image/png") != std::string::npos, "Content-Type is captured");
    ok &= require(success.body == "p2play-test-poster", "response body is copied without modification");

    const Response redirect = get(origin + "/redirect", pin);
    ok &= require(redirect.status == 302, "redirect is returned to the core without following");
    ok &= require(redirect.location == "/ok", "Location header is captured");

    const std::string ipOrigin = "https://127.0.0.1:" + std::to_string(port);
    const std::string ipPin = "127.0.0.1:" + std::to_string(port) + ":127.0.0.1";
    const Response wrongHost = get(ipOrigin + "/ok", ipPin);
    ok &= require(wrongHost.status == 0 && !wrongHost.error.empty(),
                  "hostname mismatch must fail even when the CA is trusted");

    const Response oversized = get(origin + "/big", pin, 8);
    ok &= require(oversized.status == 0 &&
                  oversized.error.find("size limit") != std::string::npos,
                  "response body is bounded by the caller's capacity");

    const Response torrent = get(origin + "/torrent", pin, 16u * 1024u * 1024u);
    ok &= require(torrent.status == 200 && torrent.body.rfind("d4:info", 0) == 0 &&
                  torrent.body.size() < 1024,
                  "HTTPS transport accepts a bounded .torrent response");

    const Response largeTooSmall = get(origin + "/large", pin, 2u * 1024u * 1024u);
    ok &= require(largeTooSmall.status == 0 &&
                  largeTooSmall.error.find("size limit") != std::string::npos,
                  "a torrent-sized response cannot exceed the caller's 2 MiB limit");

    const Response largeAllowed = get(origin + "/large", pin, 16u * 1024u * 1024u);
    ok &= require(largeAllowed.status == 200 && largeAllowed.body.size() == 4u * 1024u * 1024u,
                  "the transport accepts bodies up to the 16 MiB torrent limit");

    p2p::httpsShutdown();
    if (!ok) return 1;
    std::cout << "HTTPS transport: CA validation, hostname validation, DNS pin, redirect and body limit OK\n";
    return 0;
}
