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
    if (config->http_port < -1 || config->http_port > 65535) {
        return -1;
    }
    c.dataDir = config->data_dir;
    c.httpPort = config->http_port == 0 ? 8660 : config->http_port < 0 ? 0 : static_cast<uint16_t>(config->http_port);
    c.httpLoopbackOnly = config->loopback_only != 0;
    if (config->http_prefix) c.httpPrefix = config->http_prefix;
    c.console = config->console != 0;
    if (config->owner_password) {
        if (!*config->owner_password) {
            return -1;
        }
        c.ownerPassword = config->owner_password;
    }
    if (config->platform) {
        c.platform = config->platform;
    }
    if (config->http_get) {
        p2play_http_get_fn fn = config->http_get;
        void *user = config->http_user;
        c.httpGet = [fn, user](const std::string &url, const std::string &resolve, size_t maxBytes,
                               std::string &body, std::string &type, std::string &location, std::string &error) {
            if (maxBytes == 0 || maxBytes > 16u * 1024u * 1024u) {
                error = "HTTP(S) response limit is invalid";
                return 0;
            }
            body.resize(maxBytes);
            size_t size = 0;
            char contentType[256] = {}, redirect[2048] = {}, message[512] = {};
            const int status = fn(url.c_str(), resolve.c_str(), body.data(), body.size(), &size,
                                  contentType, sizeof(contentType), redirect, sizeof(redirect),
                                  message, sizeof(message), user);
            if (size > body.size()) { error = "HTTP callback returned an oversized body"; body.clear(); return 0; }
            body.resize(size); type = contentType; location = redirect; error = message;
            return status;
        };
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

extern "C" int p2play_active_requests(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_core ? g_core->activeRequests() : 0;
}

extern "C" void p2play_stop_accepting(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_core) {
        g_core->stopAccepting();
    }
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
