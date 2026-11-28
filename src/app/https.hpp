#pragma once

#include <cstddef>
#include <string>

#include "p2play.h"

namespace p2p {

    // Initialise the standalone host's outbound HTTP(S) poster transport. Linux uses libcurl's system
    // trust store; consoles use their platform TLS backend/CA bundle; Android uses
    // the foreground service's loopback-only bridge to the system SSLSocket provider.
    bool httpsInit(const std::string &dataDir, int androidBridgePort, const std::string &androidBridgeToken, p2play_log_fn log);
    void httpsShutdown();

    int httpsGet(const char *url, const char *pinnedResolve,
                 char *body, size_t bodyCapacity, size_t *bodySize,
                 char *contentType, size_t contentTypeCapacity,
                 char *location, size_t locationCapacity,
                 char *error, size_t errorCapacity, void *user);

} // namespace p2p
