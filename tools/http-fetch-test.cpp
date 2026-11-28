#include "core.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <libtorrent/torrent_info.hpp>

namespace p2p {
    bool p2playFetchTest(Core &core, const std::string &url, size_t maxBytes,
                         std::string &bytes, std::string &type, std::string &error) {
        return core.fetchUrl(url, maxBytes, bytes, type, error);
    }

    bool p2playAddTest(Core &core, const std::string &url, std::string &error) {
        return static_cast<bool>(core.add(url, "", "", "", "", "", error));
    }
}

namespace {
    struct Fixture {
        std::string mode;
        std::string body;
        std::vector<std::string> urls;
        std::vector<std::string> pins;
    };

    bool check(bool condition, const char *message) {
        std::cout << (condition ? "OK   " : "FAIL ") << message << '\n';
        return condition;
    }

    std::string sampleTorrent() {
        return "d4:infod6:lengthi1e4:name8:test.bin12:piece lengthi16384e6:pieces20:" +
               std::string(20, 'x') + "ee";
    }
}

int main() {
    Fixture fixture;
    fixture.body = sampleTorrent();

    p2p::CoreConfig config;
    config.dataDir = "/tmp/p2play-http-fetch-test";
    config.httpGet = [&fixture](const std::string &url, const std::string &pin, size_t maxBytes,
                                std::string &body, std::string &type, std::string &location,
                                std::string &error) {
        fixture.urls.push_back(url);
        fixture.pins.push_back(pin);
        type = "application/x-bittorrent";
        if (fixture.mode == "redirect-upgrade" && url.find("/redirect") != std::string::npos) {
            location = "https://1.1.1.1:8443/torrent";
            return 302;
        }
        if (fixture.mode == "redirect-downgrade" && url.find("/downgrade") != std::string::npos) {
            location = "http://1.1.1.1:8080/torrent";
            return 302;
        }
        if (fixture.mode == "untrusted") {
            error = "certificate verify failed";
            return 0;
        }
        if (fixture.mode == "oversized") {
            body.assign(maxBytes + 1, 'x');
            return 200;
        }
        body = fixture.body;
        return 200;
    };

    p2p::Core core(config);
    bool ok = true;
    std::string body, type, error;

    fixture.mode.clear();
    fixture.urls.clear(); fixture.pins.clear();
    const bool https = p2p::p2playFetchTest(core, "https://8.8.8.8:8443/torrent",
                                             16u * 1024u * 1024u, body, type, error);
    ok &= check(https && body == fixture.body && type == "application/x-bittorrent" &&
                fixture.pins.size() == 1 && fixture.pins[0] == "8.8.8.8:8443:8.8.8.8",
                "HTTPS torrent fetch passes its DNS pin and 16 MiB limit to the host transport");

    fixture.urls.clear(); fixture.pins.clear();
    const bool http = p2p::p2playFetchTest(core, "http://8.8.8.8:8080/torrent",
                                            16u * 1024u * 1024u, body, type, error);
    ok &= check(http && body == fixture.body && fixture.pins.size() == 1 &&
                fixture.pins[0] == "8.8.8.8:8080:8.8.8.8",
                "HTTP torrent fetch uses the same bounded host transport and DNS pin");

    fixture.mode = "redirect-upgrade";
    fixture.urls.clear(); fixture.pins.clear();
    const bool upgraded = p2p::p2playFetchTest(core, "http://8.8.8.8:8080/redirect",
                                                16u * 1024u * 1024u, body, type, error);
    ok &= check(upgraded && fixture.urls.size() == 2 &&
                fixture.urls[1] == "https://1.1.1.1:8443/torrent" &&
                fixture.pins[1] == "1.1.1.1:8443:1.1.1.1",
                "redirect targets are re-resolved, pinned and followed one hop at a time");

    fixture.mode = "redirect-downgrade";
    fixture.urls.clear(); fixture.pins.clear();
    const bool downgraded = p2p::p2playFetchTest(core, "https://8.8.8.8:8443/downgrade",
                                                  16u * 1024u * 1024u, body, type, error);
    ok &= check(!downgraded && fixture.urls.size() == 1 &&
                error.find("downgrade") != std::string::npos,
                "HTTPS redirects cannot downgrade to HTTP");

    fixture.mode = "untrusted";
    fixture.urls.clear(); fixture.pins.clear();
    const bool untrusted = p2p::p2playFetchTest(core, "https://8.8.8.8:8443/torrent",
                                                 16u * 1024u * 1024u, body, type, error);
    ok &= check(!untrusted && error.find("certificate verify failed") != std::string::npos,
                "TLS verification failures are returned without disabling certificate checks");

    fixture.mode = "oversized";
    const bool oversized = p2p::p2playFetchTest(core, "https://8.8.8.8:8443/torrent", 8,
                                                 body, type, error);
    ok &= check(!oversized && error.find("size limit") != std::string::npos,
                "the common fetcher rejects bodies beyond the caller's size limit");

    fixture.mode.clear();
    fixture.urls.clear(); fixture.pins.clear();
    const bool privateTarget = p2p::p2playFetchTest(core, "https://127.0.0.1:8443/private",
                                                     16u * 1024u * 1024u, body, type, error);
    ok &= check(!privateTarget && fixture.urls.empty() &&
                error.find("publicly reachable") != std::string::npos,
                "loopback and other non-public DNS targets are rejected before transport");

    fixture.body = "not a torrent";
    error.clear();
    const bool invalidTorrent = p2p::p2playAddTest(core, "https://8.8.8.8:8443/invalid.torrent", error);
    ok &= check(!invalidTorrent && error.find("bad .torrent") != std::string::npos,
                "a fetched response still has to parse as valid .torrent metadata");

    lt::error_code torrentError;
    const std::string sample = sampleTorrent();
    lt::torrent_info parsed(sample.data(), static_cast<int>(sample.size()), torrentError);
    ok &= check(!torrentError && parsed.name() == "test.bin",
                "the HTTPS fixture contains a parseable .torrent document");

    return ok ? 0 : 1;
}
