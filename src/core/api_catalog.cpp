// The MediaMesh routes of the native API (stage 3; mesh.hpp): the catalog search over the
// lists of this node, the lists, publishing a list (admins: the releaser's side). The search
// gives a torrs:// link a result, for the APK and the Silo plugin to open.
//   GET    /api/catalog/search?q=&tag=&limit=   {"results": [record]}
//   GET    /api/catalog/:infohash               a record and the lists holding it
//   GET    /api/lists                           {"lists": [card]}
//   DELETE /api/lists/:pubkey                   a received list dropped
//   POST   /api/lists/import {"dir"}            a version directory taken (checked) - admins
//   POST   /api/publish {"name","description"}  a new list, its key made here - admins
//   GET    /api/publish/:pubkey/status
//   POST   /api/publish/:pubkey/records         NDJSON records for the next version
//   POST   /api/publish/:pubkey/release         the next version

#include "core_torrent.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        bool error(HttpResponse &res, int status, const std::string &message) {
            return res.json(status, json{{"error", message}}.dump());
        }

        json card(const MeshList &l) {
            return {{"pubkey", l.pubkey},
                    {"name", l.name},
                    {"description", l.desc},
                    {"seq", l.seq},
                    {"records", l.records},
                    {"updated", l.updated},
                    {"own", l.own}};
        }
    }

    void Core::routeMesh(const HttpRequest &req, HttpResponse &res, const User &user, const std::vector<std::string> &parts) {
        const std::string &m = req.method;
        const bool get = m == "GET" || m == "HEAD";
        const size_t n = parts.size();
        auto at = [&](size_t i) { return i < n ? parts[i] : std::string(); };
        const bool admin = user.rank >= 50;

        if (at(1) == "catalog") {
            if (get && n == 3 && at(2) == "search") {
                int limit = std::atoi(req.param("limit").c_str());
                limit = limit <= 0 ? 50 : std::min(limit, 500);
                json results = json::array();
                for (const MeshRecord &r : m_mesh.search(req.param("q"), req.param("tag"), static_cast<size_t>(limit))) {
                    results.push_back(r.json());
                }
                res.json(200, json{{"results", results}}.dump());
            } else if (get && n == 3) {
                MeshRecord r;
                if (!m_mesh.record(at(2), r)) {
                    error(res, 404, "not in the catalog");
                    return;
                }
                json lists = json::array();
                for (const std::string &pk : r.lists) {
                    MeshList l;
                    if (m_mesh.list(pk, l)) {
                        lists.push_back(card(l));
                    }
                }
                json j = r.json();
                j["sources"] = lists;
                res.json(200, j.dump());
            } else {
                error(res, 404, "not found");
            }
            return;
        }

        if (at(1) == "lists") {
            if (get && n == 2) {
                json lists = json::array();
                for (const MeshList &l : m_mesh.lists()) {
                    lists.push_back(card(l));
                }
                res.json(200, json{{"lists", lists}}.dump());
            } else if (m == "POST" && n == 3 && at(2) == "import") {
                if (!admin) {
                    error(res, 403, "insufficient permissions");
                    return;
                }
                json b = json::parse(req.body, nullptr, false);
                const std::string dir = b.is_object() && b.value("dir", json()).is_string() ? b["dir"].get<std::string>() : "";
                const std::string err = dir.empty() ? "dir is required" : m_mesh.accept(dir);
                if (!err.empty()) {
                    error(res, 400, err);
                    return;
                }
                res.json(200, json{{"status", "imported"}}.dump());
            } else if (m == "DELETE" && n == 3) {
                if (!admin) {
                    error(res, 403, "insufficient permissions");
                    return;
                }
                const std::string err = m_mesh.remove(at(2));
                if (!err.empty()) {
                    error(res, err == "list not found" ? 404 : 400, err);
                    return;
                }
                res.json(200, json{{"status", "removed"}}.dump());
            } else if (m == "POST" && n == 3) {
                error(res, 501, "subscribing through the network comes with the DHT pointers (not yet)");
            } else {
                error(res, 404, "not found");
            }
            return;
        }

        // publishing
        if (!admin) {
            error(res, 403, "insufficient permissions");
            return;
        }
        if (m == "POST" && n == 2) {
            json b = json::parse(req.body, nullptr, false);
            if (!b.is_object() || !b.value("name", json()).is_string()) {
                error(res, 400, "invalid request format");
                return;
            }
            std::string pubkey;
            const std::string desc = b.value("description", json()).is_string() ? b["description"].get<std::string>() : "";
            const std::string err = m_mesh.create(b["name"].get<std::string>(), desc, pubkey);
            if (!err.empty()) {
                error(res, 400, err);
                return;
            }
            res.json(201, json{{"pubkey", pubkey}}.dump());
            return;
        }
        MeshList l;
        if (n != 4 || !m_mesh.list(at(2), l) || !l.own) {
            error(res, 404, "not a list of this node");
            return;
        }
        if (get && at(3) == "status") {
            json j = card(l);
            j["pending"] = m_mesh.pending(l.pubkey);
            res.json(200, j.dump());
        } else if (m == "POST" && at(3) == "records") {
            std::string err;
            int count = m_mesh.addPending(l.pubkey, req.body, err);
            if (count < 0) {
                error(res, 400, err);
                return;
            }
            res.json(200, json{{"count", count}, {"pending", m_mesh.pending(l.pubkey)}}.dump());
        } else if (m == "POST" && at(3) == "release") {
            int64_t seq = 0;
            const std::string err = m_mesh.release(l.pubkey, seq);
            if (!err.empty()) {
                error(res, 400, err);
                return;
            }
            m_mesh.list(l.pubkey, l);
            res.json(200, card(l).dump());
        } else {
            error(res, 404, "not found");
        }
    }

}
