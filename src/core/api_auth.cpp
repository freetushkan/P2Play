// The entry of every request: errors caught, the public routes, who sends it (auth.hpp):
// a token on /api (the native API, Silo's scheme, api_silo.cpp), Basic on the MatriX
// routes (api_matrix.cpp, a compatibility layer). One set of users for both.

#include "core_torrent.hpp"
#include "ui_files.hpp"

#include <cctype>

namespace p2p {

    using nlohmann::json;

    namespace {
        bool error(HttpResponse &res, int status, const std::string &message,
                   const std::map<std::string, std::string> &headers = {}) {
            return res.send(status, "application/json; charset=utf-8", json{{"error", message}}.dump(), headers);
        }

        // /api/v1/torrents/<hash>/files/<index>/stream and nothing else
        bool internalStreamPath(const std::string &path) {
            if (path.rfind("/api/v1/torrents/", 0) != 0) return false;
            const size_t files = path.find("/files/");
            if (files == std::string::npos || files <= 17) return false;
            const size_t index = files + 7;
            const size_t stream = path.find("/stream", index);
            if (stream == std::string::npos || stream != path.size() - 7 || stream == index) return false;
            for (size_t i = index; i < stream; i++) {
                if (path[i] < '0' || path[i] > '9') return false;
            }
            for (size_t i = 17; i < files; i++) {
                if (!isxdigit(static_cast<unsigned char>(path[i]))) return false;
            }
            return true;
        }

        std::string tokenCookie(const std::string &token, int maxAge) {
            return "silo_token=" + token + "; Path=/; Max-Age=" + std::to_string(maxAge) + "; HttpOnly; SameSite=Lax";
        }
    }

    // an exception (a torrent closed meanwhile: its handle throws) ends the request, not the
    // process
    void Core::handle(const HttpRequest &req, HttpResponse &res) {
        try {
            route(req, res);
        } catch (const std::exception &e) {
            log("[Web] %s %s failed: %s", req.method.c_str(), req.path.c_str(), e.what());
            if (!res.started()) {
                error(res, 500, e.what());
            }
        }
    }

    void Core::route(const HttpRequest &req, HttpResponse &res) {
        const std::string &p = req.path;
        const bool get = req.method == "GET" || req.method == "HEAD";
        const bool post = req.method == "POST";

        // MatriX compatibility discovery is absent from standard builds.
#if P2P_COMPAT_MATRIX
        if (get && p == "/echo") {
            res.text(200, echo());
            return;
        }
#endif
        // the web interface: the Silo plugin's pages (plugins/mediamesh), at /ui/; a browser at /
        // goes there, anything else gets the node's card. It logs in through /api/auth/login.
        if (get && p == "/") {
            if (req.header("accept").find("text/html") != std::string::npos) {
                res.send(302, "text/plain; charset=utf-8", "/ui/\n", {{"Location", "/ui/"}});
            } else {
                res.json(200, json{{"name", "P2Play"}, {"version", version()}, {"auth", true}}.dump());
            }
            return;
        }
        if (get && (p == "/ui" || p.rfind("/ui/", 0) == 0)) {
            if (p == "/ui") {
                res.send(301, "text/plain; charset=utf-8", "/ui/\n", {{"Location", "/ui/"}});
                return;
            }
            const UiFile *f = uiFile(p == "/ui/" ? "/index.html" : p.substr(3));
            if (!f) {
                error(res, 404, "not found");
                return;
            }
            // the plugin's policy in Silo, the same here
            res.send(200, f->type, std::string(reinterpret_cast<const char *>(f->data), f->size),
                     {{"Cache-Control", "no-cache"}, {"X-Content-Type-Options", "nosniff"}, {"Referrer-Policy", "no-referrer"},
                      {"Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; media-src 'self'; "
                                                  "connect-src 'self'; base-uri 'none'; frame-ancestors 'self'; form-action 'self'"}});
            return;
        }
        if (get && p == "/api/system/ping") {
            res.json(200, json{{"status", "ok"}}.dump());
            return;
        }
        // Native product identity is intentionally separate from the TorrServer/Silo
        // compatibility endpoints below. Keep this probe public for client discovery.
        if (get && p == "/api/p2play/version") {
            res.json(200, json{{"name", "P2Play"}, {"version", productVersion()}}.dump());
            return;
        }
        if (post && p == "/api/auth/login") {
            json body = json::parse(req.body, nullptr, false);
            const std::string name = body.is_object() ? body.value("username", "") : "";
            User u = body.is_object() && body.value("password", json()).is_string()
                             ? m_auth.login(name, body.value("password", ""))
                             : User();
            if (!u.valid() && name == Auth::kOwner && !m_auth.passwordSet()) {
                // no password yet: the first one given becomes the owner's (Silo's clients ask a
                // login anyway; a console has no other place to set it)
                const std::string password = body.value("password", json()).is_string() ? body["password"].get<std::string>() : "";
                u = password.empty() ? m_auth.find(Auth::kOwner) : m_auth.claimOwner(password);
                if (u.valid() && !password.empty()) {
                    log("[Auth] the owner's password set at the first login, from %s", req.peer.c_str());
                }
            }
            if (!u.valid()) {
                log("[Web] failed login for \"%s\" from %s", name.c_str(), req.peer.c_str());
                error(res, name.empty() ? 400 : 401, name.empty() ? "invalid request format" : "invalid username or password");
                return;
            }
            res.send(200, "application/json; charset=utf-8", json{{"user", u.json(true)}, {"token", u.token}}.dump(),
                     {{"Set-Cookie", tokenCookie(u.token, 3600 * 24 * 30)}});
            return;
        }
        if (post && p == "/api/auth/logout") {
            res.send(200, "application/json; charset=utf-8", json{{"status", "logged out"}}.dump(),
                     {{"Set-Cookie", tokenCookie("", -1)}});
            return;
        }

        // the native API: Silo's tokens
        if (p.rfind("/api/", 0) == 0) {
            const User user = m_auth.byToken(req);
            if (user.valid()) {
                routeSilo(req, res, user);
                return;
            }
            // The core's own ffmpeg (covers.cpp) reads one stream over loopback for the cover
            // probe. It presents a random per-run token, is accepted from 127.0.0.1 only, and
            // reaches nothing but the stream route: no other /api route knows this header.
            if (req.method == "GET" && req.peer == "127.0.0.1" && m_internalToken.size() == 64 &&
                req.header("x-p2play-internal") == m_internalToken && internalStreamPath(p)) {
                const User owner = m_auth.find(Auth::kOwner);
                if (owner.valid()) {
                    routeSilo(req, res, owner);
                    return;
                }
            }
            error(res, 401, "unauthorized");
            return;
        }

        if (m_settings.catalogOnly && p != "/echo") {
            error(res, 404, "a catalog node: no streamer");
            return;
        }
#if P2P_COMPAT_MATRIX
        // MatriX: Basic; a known torrent is played without it (TorrServer leaves these open)
        const User user = m_auth.byBasic(req);
        if (!user.valid() && !matrixPublic(req)) {
            error(res, 401, "unauthorized", {{"WWW-Authenticate", "Basic realm=\"Authorization Required\""}});
            return;
        }
        if (routeMatrix(req, res, user)) {
            return;
        }
#endif
        error(res, 404, "not found");
    }

}
