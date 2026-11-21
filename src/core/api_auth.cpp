// The entry of every request: errors caught, the public routes, who sends it (auth.hpp):
// a token on /api (the native API, Silo's scheme, api_silo.cpp), Basic on the MatriX
// routes (api_matrix.cpp, a compatibility layer). One set of users for both.

#include "core_torrent.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        bool error(HttpResponse &res, int status, const std::string &message,
                   const std::map<std::string, std::string> &headers = {}) {
            return res.send(status, "application/json; charset=utf-8", json{{"error", message}}.dump(), headers);
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

        // public
        if (get && p == "/echo") {
            res.text(200, echo());
            return;
        }
        if (get && p == "/") {
            res.json(200, json{{"name", "P2Play"}, {"version", version()}, {"auth", m_auth.required()}}.dump());
            return;
        }
        if (get && p == "/api/system/ping") {
            res.json(200, json{{"status", "ok"}}.dump());
            return;
        }
        if (post && p == "/api/auth/login") {
            json body = json::parse(req.body, nullptr, false);
            const std::string name = body.is_object() ? body.value("username", "") : "";
            User u = body.is_object() && body.value("password", json()).is_string()
                             ? m_auth.login(name, body.value("password", ""))
                             : User();
            if (!m_auth.required() && name == Auth::kOwner) {
                u = m_auth.find(Auth::kOwner);
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
            if (!user.valid()) {
                error(res, 401, "unauthorized");
                return;
            }
            routeSilo(req, res, user);
            return;
        }

#if P2P_COMPAT_MATRIX
        // MatriX: Basic; a known torrent is played without it
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
