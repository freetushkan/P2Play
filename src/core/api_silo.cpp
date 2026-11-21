// The native API: the REST scheme of TorrServer's silo branch under /api, with the viewing
// progress added (PUT .../progress, file_progress, GET /api/progress). File indexes as Silo:
// the file's index in the torrent (pad files counted); listed in TorrServer's order. Errors: {"error": "..."}. Every route needs a
// token (api_auth.cpp); without users set every request is the owner's.
// torrs:// links are added as any other and given in the status ("torrs"); the library
// goes in and out as .torrs (a torrs:// link a line). /api/events as Silo. Not here: Silo's
// transcoding, plugins and engine settings (the settings are MatriX's /settings).

#include <algorithm>
#include <cmath>

#include <libtorrent/hasher.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/version.hpp>

#include "core_torrent.hpp"

namespace p2p {

    using nlohmann::json;
    using Clock = std::chrono::steady_clock;

    namespace {
        constexpr auto kInfoTimeout = std::chrono::seconds(60);
        constexpr double kViewedAt = 0.9;   // position / duration that marks a file viewed

        bool error(HttpResponse &res, int status, const std::string &message) {
            return res.json(status, json{{"error", message}}.dump());
        }

        bool ok(HttpResponse &res, int status, const json &j) {
            return res.json(status, j.dump());
        }

        std::vector<std::string> split(const std::string &path) {
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos < path.size()) {
                size_t end = path.find('/', pos);
                if (end == std::string::npos) {
                    end = path.size();
                }
                if (end > pos) {
                    parts.push_back(path.substr(pos, end - pos));
                }
                pos = end + 1;
            }
            return parts;
        }

        bool number(const std::string &s, int &out) {
            if (s.empty() || s.size() > 9 || !std::all_of(s.begin(), s.end(), ::isdigit)) {
                return false;
            }
            out = std::atoi(s.c_str());
            return true;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string baseName(const std::string &path) {
            size_t slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        // Silo's file_hash: sha256 of "<file name>:<length>"
        std::string fileHash(const std::string &path, int64_t length) {
            lt::sha256_hash h = lt::hasher256(baseName(path) + ":" + std::to_string(length)).final();
            static const char *digits = "0123456789abcdef";
            std::string out;
            for (char c : h) {
                out += digits[(static_cast<unsigned char>(c) >> 4) & 15];
                out += digits[static_cast<unsigned char>(c) & 15];
            }
            return out;
        }

        std::string isoTime(int64_t t) {
            char buf[32];
            time_t tt = static_cast<time_t>(t);
            struct tm tm{};
            gmtime_r(&tt, &tm);
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        }

        int statusOf(const std::string &err) {
            if (err == "user not found") return 404;
            if (err == "user already exists") return 409;
            if (err == "permission denied" || err.find("owner") != std::string::npos) return 403;
            return 400;
        }

        // Silo's index of a file (the torrent's order) from MatriX's id, and back; -1: none
        int siloIndex(const std::vector<FileInfo> &files, int id) {
            for (const FileInfo &f : files) {
                if (f.id == id) {
                    return f.index >= 0 ? f.index : f.id - 1;
                }
            }
            return -1;
        }

        int matrixId(const std::vector<FileInfo> &files, int index) {
            for (const FileInfo &f : files) {
                if ((f.index >= 0 ? f.index : f.id - 1) == index) {
                    return f.id;
                }
            }
            return -1;
        }

        json body(const HttpRequest &req) {
            json j = json::parse(req.body, nullptr, false);
            return j.is_object() ? j : json();
        }
    }

    void Core::setPosition(const std::string &user, const std::string &hash, int fileIndex, double position,
                           double duration, bool keepViewed) {
        ViewedRecord r = m_store.viewedFile(user, hash, fileIndex);
        r.timecode = std::max(0.0, position);
        if (duration > 0) {
            r.duration = duration;
        }
        const bool past = r.duration > 0 && r.timecode / r.duration >= kViewedAt;
        r.viewed = past || (keepViewed && r.viewed);
        r.updated = static_cast<int64_t>(time(nullptr));
        m_store.setViewed(r);
    }

    json Core::siloStatus(const TorrentPtr &t, const std::string &user) {
        json j = status(t);
        std::vector<FileInfo> files;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            files = t->rec.files;
        }
        json stats = json::array();
        for (const FileInfo &f : files) {
            stats.push_back({{"id", f.index >= 0 ? f.index : f.id - 1},
                             {"path", f.path},
                             {"name", baseName(f.path)},
                             {"length", f.length},
                             {"file_hash", fileHash(f.path, f.length)}});
        }
        if (!files.empty()) {
            j["file_stats"] = stats;
        }
        json viewed = json::array(), progress = json::array();
        for (const ViewedRecord &r : m_store.viewed(user, t->rec.hash)) {
            const int idx = siloIndex(files, r.fileIndex);
            if (idx < 0) {
                continue;
            }
            if (r.viewed) {
                viewed.push_back(idx);
            }
            progress.push_back({{"id", idx},
                                {"position", r.timecode},
                                {"duration", r.duration},
                                {"viewed", r.viewed},
                                {"updated_at", isoTime(r.updated)}});
        }
        j["viewed_files"] = viewed;
        j["file_progress"] = progress;
        const std::string torrs = torrsOf(t);
        if (!torrs.empty()) {
            j["torrs"] = torrs;
        }
        return j;
    }

    // "continue watching": the files of a user with a position, the latest first
    json Core::siloProgress(const std::string &user, int64_t since) {
        std::vector<ViewedRecord> all = m_store.viewed(user, "");
        std::sort(all.begin(), all.end(), [](const ViewedRecord &a, const ViewedRecord &b) { return a.updated > b.updated; });
        json out = json::array();
        for (const ViewedRecord &r : all) {
            if (r.updated < since || r.updated == 0) {
                continue;
            }
            TorrentPtr t = find(r.hash);
            TorrentRecord rec;
            if (t) {
                std::lock_guard<std::mutex> lock(m_mutex);
                rec = t->rec;
            }
            std::string path;
            for (const FileInfo &f : rec.files) {
                if (f.id == r.fileIndex) {
                    path = f.path;
                }
            }
            out.push_back({{"hash", r.hash},
                           {"title", rec.title},
                           {"poster", rec.poster},
                           {"file_id", siloIndex(rec.files, r.fileIndex)},
                           {"file_name", baseName(path)},
                           {"position", r.timecode},
                           {"duration", r.duration},
                           {"viewed", r.viewed},
                           {"updated_at", isoTime(r.updated)}});
        }
        return out;
    }

    void Core::routeSilo(const HttpRequest &req, HttpResponse &res, const User &user) {
        const std::string &m = req.method;
        const bool get = m == "GET" || m == "HEAD";
        const std::vector<std::string> parts = split(req.path);  // "api", ...
        const size_t n = parts.size();
        auto at = [&](size_t i) { return i < n ? parts[i] : std::string(); };

        if (get && n == 3 && at(1) == "auth" && at(2) == "me") {
            ok(res, 200, user.json(true));
            return;
        }

        // system
        if (at(1) == "system") {
            if (get && at(2) == "version") {
                ok(res, 200, {{"version", version()}, {"torrent", std::string("libtorrent ") + LIBTORRENT_VERSION}});
            } else if (get && at(2) == "stats") {
                int sessions = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto &t : m_torrents) {
                        sessions += t.second->active ? 1 : 0;
                    }
                }
                const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_started).count();
                ok(res, 200, {{"stats", {{"active_sessions", sessions}, {"active_readers", activeStreams()}}},
                              {"uptime_sec", uptime},
                              {"version", version()},
                              {"torrent", std::string("libtorrent ") + LIBTORRENT_VERSION}});
            } else if (get && at(2) == "torrent-config") {
                if (user.rank < 100) {
                    error(res, 403, "owner access required");
                    return;
                }
                ok(res, 200, {{"cache_size", m_settings.cacheSize},
                              {"preload_size", m_settings.cacheSize * m_settings.preloadCache / 100},
                              {"connections_limit", m_settings.connectionsLimit},
                              {"download_rate_kb", m_settings.downloadRateLimit},
                              {"upload_rate_kb", m_settings.uploadRateLimit},
                              {"disable_upload", m_settings.disableUpload},
                              {"disable_dht", m_settings.disableDht},
                              {"use_disk", m_settings.useDisk},
                              {"peers_listen_port", m_settings.listenPort}});
            } else if (m == "POST" && at(2) == "shutdown") {
                if (user.rank < 100) {
                    error(res, 403, "owner access required");
                    return;
                }
                log("[Web] shutdown asked by %s", user.name.c_str());
                m_shutdownRequested = true;
                ok(res, 200, {{"status", "shutting down"}});
            } else {
                error(res, 404, "not found");
            }
            return;
        }

        if (at(1) == "catalog" || at(1) == "lists" || at(1) == "publish") {
            routeMesh(req, res, user, parts);
            return;
        }

        if (at(1) == "users") {
            siloUsers(req, res, user, parts);
            return;
        }

        if (get && n == 3 && at(1) == "ui" && at(2) == "menu") {
            ok(res, 200, {{"menu", json::array()}});  // no plugins
            return;
        }

        if (get && n == 2 && at(1) == "events") {
            siloEvents(res, user);
            return;
        }

        // continue watching
        if (get && n == 2 && at(1) == "progress") {
            ok(res, 200, {{"progress", siloProgress(user.id, std::atoll(req.param("since").c_str()))}});
            return;
        }

        // the library as .torrs, as Silo
        if (at(1) == "library" && n == 3) {
            if (get && at(2) == "export") {
                std::string text;
                for (const TorrentPtr &t : listOf(user.id, false)) {
                    std::string line = torrsOf(t);
                    if (!line.empty()) {
                        text += line + "\n";
                    }
                }
                res.send(200, "text/plain; charset=utf-8", text,
                         {{"Content-Disposition", "attachment; filename=\"p2play-library.torrs\""}});
            } else if (m == "POST" && at(2) == "import") {
                std::string text = req.body;
                if (req.header("content-type").rfind("multipart/form-data", 0) == 0) {
                    auto parts = parseMultipart(req);
                    text = parts.count("file") ? parts["file"].second : "";
                }
                int count = 0;
                size_t pos = 0;
                while (pos < text.size()) {
                    size_t end = text.find('\n', pos);
                    std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    pos = end == std::string::npos ? text.size() : end + 1;
                    line.erase(0, line.find_first_not_of(" \t\r"));
                    line.erase(line.find_last_not_of(" \t\r") + 1);
                    if (line.empty()) {
                        continue;
                    }
                    TorrentPtr known = find(hashOfLink(line.rfind("torrs://", 0) == 0 ? line : "torrs://" + line));
                    if (known && owns(known, user.id)) {
                        continue;
                    }
                    count += addCard(line, user.id) ? 1 : 0;
                }
                log("[Web] %s imported %d torrent(s)", user.name.c_str(), count);
                ok(res, 200, {{"status", "imported"}, {"count", count}});
            } else {
                error(res, 404, "not found");
            }
            return;
        }

        if (at(1) == "torrents" && n == 2) {
            if (get) {
                json list = json::array();
                for (const TorrentPtr &t : listOf(user.id, false)) {
                    list.push_back(siloStatus(t, user.id));
                }
                ok(res, 200, {{"torrents", list}});
            } else if (m == "POST") {
                siloAdd(req, res, user);
            } else {
                error(res, 405, "method not allowed");
            }
            return;
        }

        // /api/torrents/:hash/... and /api/stream/:hash/:idx: a torrent of the user's library
        // (or open, in no library)
        if ((at(1) == "torrents" || at(1) == "stream") && n >= 3) {
            TorrentPtr t = find(lower(at(2)));
            bool mine = false;
            if (t) {
                std::lock_guard<std::mutex> lock(m_mutex);
                mine = t->rec.users.count(user.id) || (t->rec.users.empty() && t->active);
            }
            if (!mine) {
                error(res, 404, "torrent not found in your library");
                return;
            }
            const std::string hash = t->rec.hash;
            int idx = -1;
            const bool file = (at(1) == "stream" && n == 4 && number(at(3), idx)) ||
                              (at(1) == "torrents" && at(3) == "files" && n == 6 && number(at(4), idx));
            std::vector<FileInfo> fileList;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                fileList = t->rec.files;
            }
            // MatriX's id of the file (the core's), -1 unknown yet (no metadata) or none
            int id = file ? matrixId(fileList, idx) : -1;
            const std::string action = at(1) == "stream" ? "stream" : file ? at(5) : at(3);

            if (action == "stream" || action == "preload") {
                if (!file) {
                    error(res, 400, "invalid file index");
                    return;
                }
                std::string err;
                if (!add(hash, "", "", "", "", "", err)) {
                    error(res, 500, err);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    t->lastUse = Clock::now();
                }
                if (!waitInfo(t, kInfoTimeout)) {
                    error(res, 500, "torrent connection timeout");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    fileList = t->rec.files;
                }
                id = matrixId(fileList, idx);
                if (id < 0) {
                    error(res, 404, "torrent or file not found");
                    return;
                }
                if (action == "stream") {
                    if (get) {
                        stream(t, id, req, res);
                    } else {
                        error(res, 405, "method not allowed");
                    }
                } else {
                    preload(t, id);
                    ok(res, 200, {{"status", "preloaded"}});
                }
                return;
            }
            if (file && id < 0) {
                error(res, 404, "file not found");
                return;
            }
            if (get && n == 3) {
                ok(res, 200, siloStatus(t, user.id));
            } else if (m == "DELETE" && n == 3) {
                unlink(t, user.id);
                ok(res, 200, {{"status", "removed"}});
            } else if (m == "POST" && action == "wake" && n == 4) {
                std::string err;
                if (!add(hash, "", "", "", "", "", err)) {
                    error(res, 404, err);
                    return;
                }
                ok(res, 200, {{"status", "wakeup"}});
            } else if (m == "PUT" && action == "meta" && n == 4) {
                json b = body(req);
                if (b.is_null()) {
                    error(res, 400, "invalid request format");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (b.value("title", json()).is_string()) t->rec.title = b["title"].get<std::string>();
                    if (b.value("poster", json()).is_string()) t->rec.poster = b["poster"].get<std::string>();
                    if (b.value("category", json()).is_string()) t->rec.category = b["category"].get<std::string>();
                }
                save(t);
                ok(res, 200, {{"status", "updated"}});
            } else if (m == "POST" && action == "viewed") {
                json b = body(req);
                if (b.is_null() || !b.value("viewed", json()).is_boolean()) {
                    error(res, 400, "invalid request format");
                    return;
                }
                const bool viewed = b["viewed"].get<bool>();
                std::vector<int> ids;
                for (const FileInfo &f : fileList) {
                    if (!file || f.id == id) {
                        ids.push_back(f.id);
                    }
                }
                for (int i : ids) {
                    ViewedRecord r = m_store.viewedFile(user.id, hash, i);
                    r.viewed = viewed;
                    r.updated = static_cast<int64_t>(time(nullptr));
                    m_store.setViewed(r);
                }
                if (file) {
                    ok(res, 200, {{"status", "updated"}});
                } else {
                    ok(res, 200, {{"status", "updated"}, {"viewed", viewed}});
                }
            } else if (m == "PUT" && file && action == "progress") {
                json b = body(req);
                if (b.is_null() || !b.value("position", json()).is_number()) {
                    error(res, 400, "invalid request format");
                    return;
                }
                const double duration = b.value("duration", json()).is_number() ? b["duration"].get<double>() : 0;
                setPosition(user.id, hash, id, b["position"].get<double>(), duration, true);
                ViewedRecord r = m_store.viewedFile(user.id, hash, id);
                ok(res, 200, {{"id", idx}, {"position", r.timecode}, {"duration", r.duration}, {"viewed", r.viewed},
                              {"updated_at", isoTime(r.updated)}});
            } else if (get && action == "cache" && n == 4) {
                int pieceLength, numPieces;
                bool active;
                lt::sha1_hash key;
                json readers = json::array();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    active = t->active;
                    pieceLength = t->pieceLength;
                    numPieces = t->numPieces;
                    key = t->cacheKey;
                    for (const auto &w : t->windows) {
                        readers.push_back({{"start", w.second.first}, {"end", w.second.second}, {"reader", w.first}});
                    }
                }
                if (!active) {
                    res.json(204, "");
                    return;
                }
                ok(res, 200, {{"hash", hash},
                              {"capacity", m_settings.cacheSize},
                              {"filled", m_cache ? m_cache->bytesOf(key) : 0},
                              {"piece_length", pieceLength},
                              {"piece_count", numPieces},
                              {"ids", json::array()},
                              {"sizes", json::array()},
                              {"priorities", json::array()},
                              {"readers", readers}});
            } else if (get && action == "peers" && n == 4) {
                lt::torrent_handle h;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    h = t->active ? t->handle : lt::torrent_handle();
                }
                std::vector<lt::peer_info> peers;
                if (!h.is_valid()) {
                    res.json(204, "");
                    return;
                }
                h.get_peer_info(peers);
                json list = json::array();
                for (const lt::peer_info &p : peers) {
                    list.push_back({{"addr", p.ip.address().to_string() + ":" + std::to_string(p.ip.port())},
                                    {"client", p.client},
                                    {"download_speed", p.payload_down_speed},
                                    {"upload_speed", p.payload_up_speed},
                                    {"progress", p.progress},
                                    {"seed", static_cast<bool>(p.flags & lt::peer_info::seed)}});
                }
                ok(res, 200, {{"hash", hash}, {"peers", list}});
            } else {
                error(res, 404, "not found");
            }
            return;
        }

        error(res, 404, "not found");
    }

    // Silo's /api/events: the user's library as server-sent events, once a second the cards that
    // changed ("sync" all of them first, then "card" and "remove"); a comment every 15 s finds a
    // client gone
    void Core::siloEvents(HttpResponse &res, const User &user) {
        if (!res.begin(200, "text/event-stream", -1, {{"Cache-Control", "no-cache"}, {"X-Accel-Buffering", "no"}})) {
            return;
        }
        std::map<std::string, std::string> prev;
        bool first = true;
        int quiet = 0;
        while (!m_stopping) {
            std::map<std::string, std::string> cur;
            json all = json::array();
            for (const TorrentPtr &t : listOf(user.id, false)) {
                json st = siloStatus(t, user.id);
                json card = {{"hash", st.value("hash", "")},
                             {"title", st.value("title", "")},
                             {"poster", st.value("poster", "")},
                             {"category", st.value("category", "")},
                             {"stat", st.value("stat", 0)},
                             {"size", st.value("torrent_size", static_cast<int64_t>(0))},
                             {"peers", st.value("active_peers", 0)},
                             {"seeders", st.value("connected_seeders", 0)},
                             {"download_speed", st.value("download_speed", 0.0)},
                             {"timestamp", st.value("timestamp", static_cast<int64_t>(0))},
                             {"torrs", st.value("torrs", "")}};
                cur[card["hash"].get<std::string>()] = card.dump();
                all.push_back(card);
            }
            std::string out;
            if (first) {
                out = "event: sync\ndata: " + all.dump() + "\n\n";
                first = false;
            } else {
                for (const auto &c : cur) {
                    auto it = prev.find(c.first);
                    if (it == prev.end() || it->second != c.second) {
                        out += "event: card\ndata: " + c.second + "\n\n";
                    }
                }
                for (const auto &p : prev) {
                    if (!cur.count(p.first)) {
                        out += "event: remove\ndata: " + json{{"hash", p.first}}.dump() + "\n\n";
                    }
                }
            }
            if (out.empty() && ++quiet >= 15) {
                out = ": ping\n\n";
            }
            if (!out.empty()) {
                quiet = 0;
                if (!res.write(out.data(), out.size())) {
                    return;
                }
            }
            prev = std::move(cur);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // POST /api/torrents: JSON {link, title, poster, category, save_to_db} or multipart with a
    // .torrent "file"; saved to the library by default, as Silo
    void Core::siloAdd(const HttpRequest &req, HttpResponse &res, const User &user) {
        std::string link, title, poster, category, file;
        bool save = true;
        if (req.header("content-type").rfind("multipart/form-data", 0) == 0) {
            auto parts = parseMultipart(req);
            auto field = [&](const char *name) {
                auto it = parts.find(name);
                return it == parts.end() ? std::string() : it->second.second;
            };
            link = field("link");
            title = field("title");
            poster = field("poster");
            category = field("category");
            file = field("file");
            if (parts.count("save_to_db")) {
                save = field("save_to_db") == "true";
            }
        } else {
            json b = body(req);
            if (b.is_null()) {
                error(res, 400, "invalid request format");
                return;
            }
            auto s = [&](const char *key) { return b.value(key, json()).is_string() ? b[key].get<std::string>() : ""; };
            link = s("link");
            title = s("title");
            poster = s("poster");
            category = s("category");
            if (b.value("save_to_db", json()).is_boolean()) {
                save = b["save_to_db"].get<bool>();
            }
        }
        link.erase(0, link.find_first_not_of(" \t\r\n"));
        link.erase(link.find_last_not_of(" \t\r\n") + 1);
        if (file.empty() && link.empty()) {
            error(res, 400, "torrent file or link is required");
            return;
        }
        std::string err;
        const std::string saveFor = save ? user.id : "";
        TorrentPtr t = file.empty() ? add(link, title, poster, category, "", saveFor, err)
                                    : addMetadata(file, title, poster, category, "", saveFor, err);
        if (!t) {
            error(res, 400, err);
            return;
        }
        log("[Web] torrent added by %s: %s (%s)", user.name.c_str(), t->rec.hash.c_str(),
            save ? "to the library" : "not saved");
        ok(res, 201, siloStatus(t, user.id));
    }

    void Core::siloUsers(const HttpRequest &req, HttpResponse &res, const User &user, const std::vector<std::string> &parts) {
        const std::string &m = req.method;
        const size_t n = parts.size();
        const std::string id = n > 2 ? parts[2] : "";
        const std::string action = n > 3 ? parts[3] : "";
        json b = body(req);

        if (n == 2 && (m == "GET" || m == "HEAD")) {
            if (user.rank < 50) {
                error(res, 403, "insufficient permissions");
                return;
            }
            json list = json::array();
            for (const User &u : m_auth.list()) {
                list.push_back(u.json(false));
            }
            ok(res, 200, {{"users", list}});
        } else if (n == 2 && m == "POST") {
            if (b.is_null() || !b.value("username", json()).is_string() || !b.value("password", json()).is_string()) {
                error(res, 400, "invalid request format");
                return;
            }
            int rank = b.value("rank", json()).is_number() ? b["rank"].get<int>() : 0;
            User created;
            std::string err = m_auth.create(user, b["username"].get<std::string>(), b["password"].get<std::string>(),
                                            rank > 0 ? rank : 10, created);
            if (!err.empty()) {
                error(res, statusOf(err), err);
                return;
            }
            log("[Web] user %s created by %s", created.name.c_str(), user.name.c_str());
            ok(res, 201, created.json(true));
        } else if (n == 3 && m == "PUT") {
            if (b.is_null()) {
                error(res, 400, "invalid request format");
                return;
            }
            std::string password;
            bool banned = false;
            const bool hasPassword = b.value("password", json()).is_string();
            const bool hasBanned = b.value("banned", json()).is_boolean();
            if (hasPassword) password = b["password"].get<std::string>();
            if (hasBanned) banned = b["banned"].get<bool>();
            if (hasPassword && password.empty()) {
                error(res, 400, "password should not be empty");
                return;
            }
            std::string err = m_auth.update(user, id, hasPassword ? &password : nullptr, hasBanned ? &banned : nullptr);
            if (!err.empty()) {
                error(res, statusOf(err), err);
                return;
            }
            ok(res, 200, {{"status", "updated"}});
        } else if (n == 3 && m == "DELETE") {
            std::string err = m_auth.remove(user, id);
            if (!err.empty()) {
                error(res, statusOf(err), err);
                return;
            }
            removeUser(id);
            log("[Web] user %s deleted by %s", id.c_str(), user.name.c_str());
            ok(res, 200, {{"status", "deleted"}});
        } else if (n == 4 && m == "POST" && action == "regenerate-token") {
            std::string token;
            std::string err = m_auth.regenerateToken(user, id, token);
            if (!err.empty()) {
                error(res, statusOf(err), err);
                return;
            }
            ok(res, 200, {{"token", token}});
        } else if (n == 4 && m == "POST" && action == "rank") {
            if (b.is_null() || !b.value("rank", json()).is_number()) {
                error(res, 400, "invalid request format");
                return;
            }
            std::string err = m_auth.setRank(user, id, b["rank"].get<int>());
            if (!err.empty()) {
                error(res, statusOf(err), err);
                return;
            }
            ok(res, 200, {{"status", "rank updated"}});
        } else if (n == 4 && (m == "GET" || m == "HEAD") && action == "torrents") {
            User target = m_auth.find(id);
            if (!target.valid()) {
                error(res, 404, "user not found");
                return;
            }
            if (target.id != user.id && (user.rank < 50 || user.rank <= target.rank)) {
                error(res, 403, "permission denied");
                return;
            }
            json list = json::array();
            for (const TorrentPtr &t : listOf(id, false)) {
                TorrentRecord rec;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    rec = t->rec;
                }
                json viewed = json::array();
                for (const ViewedRecord &r : m_store.viewed(id, rec.hash)) {
                    if (r.viewed && siloIndex(rec.files, r.fileIndex) >= 0) {
                        viewed.push_back(siloIndex(rec.files, r.fileIndex));
                    }
                }
                list.push_back({{"user_id", id},
                                {"torrent_hash", rec.hash},
                                {"title", rec.title},
                                {"poster", rec.poster},
                                {"category", rec.category},
                                {"added_at", isoTime(rec.users.count(id) ? rec.users.at(id) : rec.timestamp)},
                                {"viewed_files", viewed}});
            }
            ok(res, 200, {{"torrents", list}});
        } else {
            error(res, 404, "not found");
        }
    }

}
