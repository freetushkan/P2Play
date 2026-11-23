// The MediaMesh routes of the native API (stage 3; mesh.hpp): the catalog search over the
// lists of this node, the lists, publishing a list (admins: the releaser's side). The search
// gives a torrs:// link a result, for the APK and the Silo plugin to open.
//   GET    /api/catalog/search?q=&tag=&limit=   {"results": [record]}
//   GET    /api/catalog/:infohash               a record and the lists holding it
//   GET    /api/lists                           {"lists": [card]}
//   GET    /api/lists/discover                  the lists of the network: holders, seq
//   DELETE /api/lists/:pubkey                   a received list dropped
//   POST   /api/lists/import {"dir"}            a version directory taken (checked) - admins
//   POST   /api/publish {"name","description"}  a new list, its key made here - admins
//   GET    /api/publish/:pubkey/status
//   POST   /api/publish/:pubkey/records         NDJSON records for the next version
//   POST   /api/publish/:pubkey/release         the next version
//   POST   /api/publish/:pubkey/media/:kind     bounded raw media -> signed slice reference
//   POST   /api/publish/:pubkey/snapshot        compact live records and media
//   GET    /api/catalog/:infohash/:kind        lazy verified media (optional ?list=)
//   GET/POST /api/lists/:pubkey/policy          persisted shard selection / budgets
//   GET/POST /api/lists/:pubkey/files           signed fetch plan / verified local install

#include "core_torrent.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        bool error(HttpResponse &res, int status, const std::string &message) {
            return res.json(status, json{{"error", message}}.dump());
        }

        json card(const MeshList &l) {
            json j = {{"pubkey", l.pubkey},
                      {"name", l.name},
                      {"description", l.desc},
                      {"seq", l.seq},
                      {"records", l.records},
                      {"updated", l.updated},
                      {"own", l.own},
                      {"type", l.meta ? "meta" : "catalog"}};
            if (l.meta) {
                j["items"] = json::array();
                for (const auto &item : l.items)
                    j["items"].push_back({{"pubkey", item.pubkey}, {"annotation", item.annotation}, {"enabled", item.enabled}});
            }
            return j;
        }
    }

    void Core::routeMesh(const HttpRequest &req, HttpResponse &res, const User &user, const std::vector<std::string> &parts) {
        const std::string &m = req.method;
        const bool get = m == "GET" || m == "HEAD";
        const size_t n = parts.size();
        auto at = [&](size_t i) { return i < n ? parts[i] : std::string(); };
        const bool admin = user.rank >= 50;
        // Serialize policy/version/remove mutations with the joined network installer.
        std::unique_lock<std::recursive_mutex> lifecycle(m_meshLifecycle, std::defer_lock);
        if (!get) lifecycle.lock();

        if (at(1) == "catalog") {
            if (get && n == 4 && (at(3) == "description" || at(3) == "poster")) {
                MeshRecord record;
                if (!m_mesh.record(at(2), record)) { error(res, 404, "not in the catalog"); return; }
                const auto selectedList = req.param("list");
                if (!selectedList.empty() && !record.lists.count(selectedList)) { error(res, 404, "record not in list"); return; }
                std::string bytes, type, err = "media not found";
                for (const auto &pk : record.lists) {
                    if (!selectedList.empty() && pk != selectedList) continue;
                    err = m_mesh.media(pk, at(2), at(3), bytes, type);
                    if (err.empty()) break;
                    if (err == "media unavailable" || err == "media pack hash mismatch")
                        meshMediaRequest(pk, at(2), at(3));
                }
                if (!err.empty()) {
                    const bool missing = err == "media not found" || err == "record not found";
                    if (!missing) {
                        res.send(503, "application/json", json{{"error", err}, {"retry", true}}.dump(), {{"Retry-After", "2"}, {"X-Content-Type-Options", "nosniff"}});
                    } else error(res, 404, err);
                    return;
                }
                const std::map<std::string, std::string> headers = {{"X-Content-Type-Options", "nosniff"}, {"Cache-Control", "private, max-age=300"}};
                if (m == "HEAD") res.begin(200, type, bytes.size(), headers);
                else res.send(200, type, bytes, headers);
                return;
            }
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
            if (get && n == 4 && at(3) == "status") {
                MeshList l;
                json j = m_mesh.list(at(2), l) ? card(l) : json{{"pubkey", at(2)}};
                j["net"] = meshState(at(2));
                res.json(200, j.dump());
                return;
            }
            if (n == 4 && (at(3) == "files" || at(3) == "policy")) {
                MeshList l;
                if (!m_mesh.list(at(2), l)) {
                    if (m != "POST" || at(3) != "policy") { error(res, 404, "list not found"); return; }
                    l.pubkey = at(2);
                }
                if (get) {
                    json files = json::array();
                    for (const auto &f : m_mesh.plan(l.pubkey)) files.push_back({{"path", f.path}, {"sha256", f.sha256}, {"bytes", f.bytes}, {"records", f.records}, {"shard", f.shard}, {"kind", f.kind}, {"selected", f.selected}, {"present", f.present}});
                    res.json(200, json{{"policy", m_mesh.policy(l.pubkey).json()}, {"files", files}}.dump()); return;
                }
                if (!admin) { error(res, 403, "insufficient permissions"); return; }
                auto b = json::parse(req.body, nullptr, false);
                if (m != "POST" || !b.is_object()) { error(res, 400, "invalid request format"); return; }
                std::string err;
                if (at(3) == "files") {
                    if (!b.value("path", json()).is_string() || !b.value("source", json()).is_string()) { error(res, 400, "path and source are required"); return; }
                    err = m_mesh.installFile(l.pubkey, b["path"].get<std::string>(), b["source"].get<std::string>());
                } else {
                    auto p = m_mesh.policy(l.pubkey);
                    if (b.contains("shards")) {
                        if (!b["shards"].is_array() || b["shards"].size() > Mesh::kMaxSegments) { error(res, 400, "invalid shards"); return; }
                        p.shards.clear();
                        for (const auto &v : b["shards"]) { if (!v.is_string()) { error(res, 400, "invalid shard"); return; } p.shards.insert(v.get<std::string>()); }
                    }
                    for (auto item : {std::make_pair("record_bytes", &p.recordBytes), std::make_pair("media_bytes", &p.mediaBytes), std::make_pair("cache_bytes", &p.cacheBytes)}) {
                        if (!b.contains(item.first)) continue;
                        const auto &v = b[item.first];
                        if (!v.is_number_integer() || (v.is_number_unsigned() && v.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX))) { error(res, 400, "invalid budget"); return; }
                        *item.second = v.get<int64_t>();
                    }
                    if (b.contains("mirror_media")) {
                        if (!b["mirror_media"].is_boolean()) { error(res, 400, "invalid mirror_media"); return; }
                        p.mirrorMedia = b["mirror_media"].get<bool>();
                    }
                    err = m_mesh.setPolicy(l.pubkey, p);
                }
                if (!err.empty()) { error(res, 400, err); return; }
                if (at(3) == "policy") meshRefresh(l.pubkey);
                res.json(200, json{{"status", "updated"}}.dump()); return;
            }
            if (get && n == 2) {
                json lists = json::array();
                for (const MeshList &l : m_mesh.lists()) {
                    json j = card(l);
                    j["net"] = meshState(l.pubkey);
                    lists.push_back(j);
                }
                res.json(200, json{{"lists", lists}}.dump());
            } else if (get && n == 3 && at(2) == "discover") {
                // the lists the directory swarm told of, the most held first
                res.json(200, json{{"lists", meshDirectoryJson()}}.dump());
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
                meshSyncMetaSubscriptions();
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
                meshForget(at(2));
                res.json(200, json{{"status", "removed"}}.dump());
            } else if (m == "POST" && n == 3) {
                if (!admin) { error(res, 403, "insufficient permissions"); return; }
                // subscribe: the pointer from the DHT; "peers" (ip:port) help find the version
                json b = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
                std::vector<std::string> peers;
                if (b.is_object() && b.value("peers", json()).is_array()) {
                    for (const auto &p : b["peers"]) {
                        if (p.is_string()) peers.push_back(p.get<std::string>());
                    }
                }
                const std::string err = meshSubscribe(at(2), peers);
                if (!err.empty()) {
                    error(res, 400, err);
                    return;
                }
                res.json(202, json{{"status", "subscribing"}}.dump());
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
            const auto type = b.value("type", json("catalog"));
            if (!type.is_string() || (type != "catalog" && type != "meta")) {
                error(res, 400, "type must be catalog or meta");
                return;
            }
            const std::string err = m_mesh.create(b["name"].get<std::string>(), desc, pubkey, type == "meta");
            if (!err.empty()) {
                error(res, 400, err);
                return;
            }
            res.json(201, json{{"pubkey", pubkey}}.dump());
            return;
        }
        MeshList l;
        if (m == "POST" && n == 4 && at(3) == "meta") {
            if (!m_mesh.list(at(2), l) || !l.own || !l.meta) {
                error(res, 404, "not an owned meta-list");
                return;
            }
            auto b = json::parse(req.body, nullptr, false);
            if (!b.is_object() || !b.value("items", json()).is_array()) {
                error(res, 400, "items array is required");
                return;
            }
            std::vector<MeshMetaItem> items;
            for (const auto &item : b["items"]) {
                if (!item.is_object() || !item.value("pubkey", json()).is_string() ||
                    !item.value("annotation", json()).is_string() || !item.value("enabled", json()).is_boolean()) {
                    error(res, 400, "each item requires pubkey, annotation and enabled");
                    return;
                }
                items.push_back({item["pubkey"].get<std::string>(), item["annotation"].get<std::string>(),
                                 item["enabled"].get<bool>()});
            }
            const auto err = m_mesh.setMetaItems(l.pubkey, items);
            if (!err.empty()) {
                error(res, 400, err);
                return;
            }
            res.json(200, json{{"status", "pending"}, {"items", items.size()}}.dump());
            return;
        }
        if (m == "POST" && n == 5 && at(3) == "media") {
            if (!m_mesh.list(at(2), l) || !l.own) { error(res, 404, "not a list of this node"); return; }
            auto type = req.header("content-type");
            if (type.empty() && at(4) == "description") type = "text/plain; charset=utf-8";
            MeshMediaRef ref;
            auto err = m_mesh.addMedia(l.pubkey, at(4), type, req.body, ref);
            if (!err.empty()) { error(res, 400, err); return; }
            res.json(201, ref.json().dump()); return;
        }
        if (n != 4 || !m_mesh.list(at(2), l) || !l.own) {
            error(res, 404, "not a list of this node");
            return;
        }
        if (get && at(3) == "status") {
            json j = card(l);
            j["pending"] = m_mesh.pending(l.pubkey);
            j["pending_meta"] = m_mesh.hasPendingMeta(l.pubkey);
            res.json(200, j.dump());
        } else if (m == "POST" && at(3) == "records") {
            std::string err;
            int count = m_mesh.addPending(l.pubkey, req.body, err);
            if (count < 0) {
                error(res, 400, err);
                return;
            }
            res.json(200, json{{"count", count}, {"pending", m_mesh.pending(l.pubkey)}}.dump());
        } else if (m == "POST" && (at(3) == "release" || at(3) == "snapshot")) {
            int64_t seq = 0;
            const std::string err = at(3) == "snapshot" ? m_mesh.compact(l.pubkey, seq) : m_mesh.release(l.pubkey, seq);
            if (!err.empty()) {
                error(res, 400, err);
                return;
            }
            meshReleased(l.pubkey);
            m_mesh.list(l.pubkey, l);
            json j = card(l);
            j["net"] = meshState(l.pubkey);
            res.json(200, j.dump());
        } else {
            error(res, 404, "not found");
        }
    }

}
