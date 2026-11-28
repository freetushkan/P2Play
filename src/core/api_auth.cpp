// P2Play HTTP entry point: public identity/login routes, durable-token authentication,
// the embedded web UI and the native, versioned REST API.
#include "core_torrent.hpp"
#include "ui_files.hpp"

#include <cctype>
#include <vector>

namespace p2p {
    using nlohmann::json;

    namespace {
        bool error(HttpResponse &res, int status, const std::string &message,
                   const std::map<std::string, std::string> &headers = {}) {
            return res.send(status, "application/json; charset=utf-8", json{{"error", message}}.dump(), headers);
        }

        std::string tokenCookie(const std::string &token, int maxAge, const std::string &prefix) {
            return "p2play_token=" + token + "; Path=" + (prefix.empty() ? "/" : prefix) +
                   "; Max-Age=" + std::to_string(maxAge) + "; HttpOnly; SameSite=Lax";
        }

        bool internalStreamPath(const std::string &path) {
            if (path.rfind("/api/v1/torrents/", 0) != 0) return false;
            const size_t files = path.find("/files/");
            if (files == std::string::npos || files <= 17) return false;
            const size_t index = files + 7;
            const size_t stream = path.find("/stream", index);
            if (stream == std::string::npos || stream != path.size() - 7 || stream == index) return false;
            for (size_t i = index; i < stream; ++i) if (!std::isdigit(static_cast<unsigned char>(path[i]))) return false;
            for (size_t i = 17; i < files; ++i) if (!std::isxdigit(static_cast<unsigned char>(path[i]))) return false;
            return true;
        }

        std::vector<std::string> pathParts(const std::string &path) {
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos < path.size()) {
                const size_t end = path.find('/', pos);
                if (end != pos) parts.push_back(path.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
                if (end == std::string::npos) break;
                pos = end + 1;
            }
            return parts;
        }
    }

    void Core::handle(const HttpRequest &req, HttpResponse &res) {
        try {
            route(req, res);
        } catch (const std::exception &e) {
            log("[Web] %s %s failed: %s", req.method.c_str(), req.path.c_str(), e.what());
            if (!res.started()) error(res, 500, e.what());
        }
    }

    void Core::route(const HttpRequest &req, HttpResponse &res) {
        std::string path = req.path;
        if (!m_config.httpPrefix.empty()) {
            if (path == m_config.httpPrefix) {
                path = "/";
            } else if (path.rfind(m_config.httpPrefix + "/", 0) == 0) {
                path.erase(0, m_config.httpPrefix.size());
            } else {
                error(res, 404, "not found");
                return;
            }
        }
        const bool get = req.method == "GET" || req.method == "HEAD";
        const bool post = req.method == "POST";

        if (get && path == "/") {
            if (req.header("accept").find("text/html") != std::string::npos)
                res.send(302, "text/plain; charset=utf-8", m_config.httpPrefix + "/ui/\n", {{"Location", m_config.httpPrefix + "/ui/"}});
            else
                res.json(200, json{{"name", "P2Play"}, {"version", productVersion()}, {"auth", true}}.dump());
            return;
        }

        if (get && (path == "/ui" || path.rfind("/ui/", 0) == 0)) {
            if (path == "/ui") {
                res.send(301, "text/plain; charset=utf-8", m_config.httpPrefix + "/ui/\n", {{"Location", m_config.httpPrefix + "/ui/"}});
                return;
            }
            const UiFile *file = uiFile(path == "/ui/" ? "/index.html" : path.substr(3));
            if (!file) { error(res, 404, "not found"); return; }
            res.send(200, file->type, std::string(reinterpret_cast<const char *>(file->data), file->size),
                     {{"Cache-Control", "no-cache"}, {"X-Content-Type-Options", "nosniff"},
                      {"Referrer-Policy", "no-referrer"},
                      {"Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; media-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'self'; form-action 'self'"}});
            return;
        }

        if (get && path == "/api/v1/system/ping") {
            res.json(200, json{{"status", "ok"}}.dump());
            return;
        }
        if (get && path == "/api/v1/system/version") {
            res.json(200, json{{"name", "P2Play"}, {"version", productVersion()}}.dump());
            return;
        }

        if (post && path == "/api/v1/auth/login") {
            const json body = json::parse(req.body, nullptr, false);
            const std::string name = body.is_object() ? body.value("username", std::string()) : std::string();
            const std::string password = body.is_object() && body.value("password", json()).is_string()
                                             ? body["password"].get<std::string>() : std::string();
            User user = m_auth.login(name, password);
            if (!user.valid() && name == Auth::kOwner && !m_auth.passwordSet())
                user = password.empty() ? m_auth.find(Auth::kOwner) : m_auth.claimOwner(password);
            if (!user.valid()) {
                log("[Web] failed login for \"%s\" from %s", name.c_str(), req.peer.c_str());
                error(res, name.empty() ? 400 : 401, name.empty() ? "username and password are required"
                                                                  : "invalid username or password");
                return;
            }
            res.send(200, "application/json; charset=utf-8",
                     json{{"user", user.json(false)}, {"token", user.token}}.dump(),
                     {{"Set-Cookie", tokenCookie(user.token, 3600 * 24 * 365, m_config.httpPrefix)}});
            return;
        }
        if (post && path == "/api/v1/auth/logout") {
            res.send(200, "application/json; charset=utf-8", json{{"status", "logged out"}}.dump(),
                     {{"Set-Cookie", tokenCookie("", -1, m_config.httpPrefix)}});
            return;
        }

        if (path.rfind("/api/v1/", 0) == 0) {
            const User user = m_auth.byToken(req);
            if (user.valid()) {
                if (m_settings.catalogOnly && path.find("/catalog/") == std::string::npos &&
                    path.find("/lists") == std::string::npos && path.find("/publish") == std::string::npos &&
                    path != "/api/v1/auth/me" && path != "/api/v1/users") {
                    error(res, 404, "this is a catalog-only node");
                    return;
                }
                routeP2PlayV1(req, res, user, pathParts(path));
                return;
            }

            // Cover probing uses one short-lived internal credential, loopback-only, and a
            // single exact stream route. It does not grant access to any other API method.
            if (req.method == "GET" && req.peer == "127.0.0.1" && m_internalToken.size() == 64 &&
                req.header("x-p2play-internal") == m_internalToken && internalStreamPath(path)) {
                const User owner = m_auth.find(Auth::kOwner);
                if (owner.valid()) {
                    routeP2PlayV1(req, res, owner, pathParts(path));
                    return;
                }
            }
            error(res, 401, "unauthorized");
            return;
        }
        error(res, 404, "not found");
    }
}
