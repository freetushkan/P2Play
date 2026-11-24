#include "p2play.h"

#include <memory>
#include <mutex>

#include "core.hpp"

namespace {
    std::mutex g_mutex;
    std::unique_ptr<p2p::Core> g_core;
}

extern "C" int p2play_start(const p2play_config *config) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_core || !config || !config->data_dir) {
        return -1;
    }
    p2p::CoreConfig c;
    c.dataDir = config->data_dir;
    c.httpPort = config->http_port == 0 ? 8090 : config->http_port < 0 ? 0 : static_cast<uint16_t>(config->http_port);
    c.console = config->console != 0;
    const bool hasOwnerUsername = config->owner_username != nullptr;
    const bool hasOwnerPassword = config->owner_password != nullptr;
    if (hasOwnerUsername != hasOwnerPassword ||
        (hasOwnerUsername && (!*config->owner_username || !*config->owner_password))) {
        return -1;
    }
    if (hasOwnerUsername) {
        c.ownerUsername = config->owner_username;
        c.ownerPassword = config->owner_password;
    }
    if (config->platform) {
        c.platform = config->platform;
    }
    if (config->log) {
        p2play_log_fn fn = config->log;
        void *user = config->log_user;
        c.log = [fn, user](const std::string &line) { fn(line.c_str(), user); };
    }
    auto core = std::make_unique<p2p::Core>(c);
    if (!core->start()) {
        return -1;
    }
    g_core = std::move(core);
    return 0;
}

extern "C" int p2play_http_port(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_core ? g_core->httpPort() : 0;
}

extern "C" int p2play_active_streams(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_core ? g_core->activeStreams() : 0;
}

extern "C" int p2play_shutdown_requested(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_core && g_core->shutdownRequested() ? 1 : 0;
}

extern "C" void p2play_stop(void) {
    std::unique_ptr<p2p::Core> core;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        core = std::move(g_core);
    }
    if (core) {
        core->stop();
    }
}

extern "C" const char *p2play_version(void) {
    return p2p::Core::version();
}
