// The entry of every request: errors caught, who sends it (auth.hpp), the native routes
// (/echo, the login of Silo's /api/auth), then the compatibility layers.

#include "core_torrent.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        bool error(HttpResponse &res, int status, const std::string &message,
                   const std::map<std::string, std::string> &headers = {}) {
            return res.send(status, "application/json; charset=utf-8", json{{"error", message}}.dump(), headers);
        }

        std::string tokenCookie(const std::string &token, int maxAge) {
            return "p2play_token=" + token + "; Path=/; Max-Age=" + std::to_string(maxAge) + "; HttpOnly; SameSite=Lax";
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
        if (post && p == "/api/auth/login") {
            json body = json::parse(req.body, nullptr, false);
            std::string name = body.is_object() ? body.value("username", "") : "";
            std::string token = body.is_object() ? m_auth.login(name, body.value("password", "")) : "";
            if (token.empty()) {
                log("[Web] failed login for \"%s\" from %s", name.c_str(), req.peer.c_str());
                error(res, 401, "invalid username or password");
                return;
            }
            res.send(200, "application/json; charset=utf-8",
                     json{{"user", {{"name", name}}}, {"token", token}}.dump(),
                     {{"Set-Cookie", tokenCookie(token, 3600 * 24 * 30)}});
            return;
        }
        if (post && p == "/api/auth/logout") {
            res.send(200, "application/json; charset=utf-8", json{{"status", "logged out"}}.dump(),
                     {{"Set-Cookie", tokenCookie("", 0)}});
            return;
        }

        // the rest needs a user once users are set
        const std::string user = m_auth.identify(req);
        bool allowed = !m_auth.required() || !user.empty();
#if P2P_COMPAT_MATRIX
        allowed = allowed || matrixPublic(req);
#endif
        if (!allowed) {
            std::map<std::string, std::string> headers;
            if (m_auth.basicEnabled()) {
                headers["WWW-Authenticate"] = "Basic realm=\"Authorization Required\"";
            }
            error(res, 401, "unauthorized", headers);
            return;
        }

        if (get && p == "/api/auth/me") {
            json me = user.empty() ? json{{"name", "owner"}} : json{{"name", user}, {"token", m_auth.tokenOf(user)}};
            res.json(200, json{{"user", me}, {"auth", m_auth.required()}}.dump());
            return;
        }
#if P2P_COMPAT_MATRIX
        if (routeMatrix(req, res)) {
            return;
        }
#endif
        error(res, 404, "not found");
    }

}
