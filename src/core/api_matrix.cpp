// The TorrServer MatriX API, as Lampa and pPlay use it: /torrents, /torrent/upload,
// /stream, /play, /viewed, /settings, /cache, /playlist, /playlistall. File ids start
// from 1. Errors: {"error": "..."}. A compatibility layer: P2P_COMPAT_MATRIX off leaves it
// out (with the Basic authentication of auth.cpp).

#include <algorithm>
#include <cctype>

#include "core_torrent.hpp"

#if P2P_COMPAT_MATRIX

namespace p2p {

    using nlohmann::json;
    using Clock = std::chrono::steady_clock;

    namespace {
        constexpr auto kInfoTimeout = std::chrono::seconds(60);

        std::string str(const json &j, const char *key) {
            auto it = j.find(key);
            return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        bool flag(const json &j, const char *key) {
            auto it = j.find(key);
            return it != j.end() && it->is_boolean() && it->get<bool>();
        }

        int integer(const json &j, const char *key, int def) {
            auto it = j.find(key);
            return it != j.end() && it->is_number() ? it->get<int>() : def;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        bool error(HttpResponse &res, int status, const std::string &message) {
            return res.json(status, json{{"error", message}}.dump());
        }

        bool isMedia(const std::string &path) {
            auto dot = path.rfind('.');
            std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            static const char *media[] = {"mkv", "mp4", "m4v", "avi", "ts", "m2ts", "webm", "mov", "wmv", "mpg",
                                          "mpeg", "vob", "flv", "3gp", "mp3", "flac", "m4a", "aac", "ogg",
                                          "opus", "wav", "ape", "wv", "dts", "ac3"};
            return std::any_of(std::begin(media), std::end(media), [&](const char *m) { return ext == m; });
        }

        std::string baseName(const std::string &path) {
            size_t slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

    }

    // the MatriX routes; false: not one of them
    bool Core::routeMatrix(const HttpRequest &req, HttpResponse &res, const User &user) {
        const std::string &p = req.path;
        const bool get = req.method == "GET" || req.method == "HEAD";
        const bool post = req.method == "POST";
        if (post && p == "/torrents") {
            apiTorrents(req, res, user);
        } else if (post && p == "/torrent/upload") {
            apiUpload(req, res, user);
        } else if (get && (p == "/stream" || p.rfind("/stream/", 0) == 0)) {
            apiStream(req, res, "", -1, user);
        } else if (get && p.rfind("/play/", 0) == 0) {
            // /play/<hash>/<id>
            std::string rest = p.substr(6);
            size_t slash = rest.find('/');
            int id = slash == std::string::npos ? 1 : std::atoi(rest.c_str() + slash + 1);
            apiStream(req, res, rest.substr(0, slash), id, user);
        } else if (post && p == "/viewed") {
            apiViewed(req, res, user);
        } else if (post && p == "/settings") {
            apiSettings(req, res);
        } else if (post && p == "/cache") {
            apiCache(req, res);
        } else if (get && (p == "/playlist" || p.rfind("/playlist/", 0) == 0)) {
            TorrentPtr t = find(req.param("hash"));
            if (!t) {
                error(res, 404, "torrent not found");
                return true;
            }
            res.send(200, "audio/x-mpegurl; charset=utf-8", playlist({t}, req.header("host"), req.has("fromlast") ? 1 : 0, user.id, ""));
        } else if (get && p == "/playlistall/all.m3u") {
            res.send(200, "audio/x-mpegurl; charset=utf-8", playlist(listOf(user.id, true), req.header("host"), 0, user.id, ""));
        } else if (get && (p == "/shutdown" || p.rfind("/shutdown/", 0) == 0) && user.valid()) {
            // the host decides (a standalone server quits, pPlay may ignore it)
            log("[Web] shutdown asked by %s", req.peer.c_str());
            m_shutdownRequested = true;
            res.json(200, "{}");
        } else if (get && p.rfind("/download/", 0) == 0) {
            downloadTest(req, res, std::atoll(p.c_str() + 10));
        } else if (get && p == "/storage/settings") {
            res.json(200, json{{"settings", "json"}, {"viewed", "json"}}.dump());
        } else if (post && p == "/storage/settings") {
            res.json(200, json{{"status", "ok"}}.dump());
        } else {
            return false;
        }
        return true;
    }

    // TorrServer's speed test: <mb> megabytes of zeros, with Range
    void Core::downloadTest(const HttpRequest &req, HttpResponse &res, int64_t megabytes) {
        const int64_t size = std::max<int64_t>(0, std::min<int64_t>(megabytes, 10240)) * 1024 * 1024;
        int64_t start = 0, end = size - 1;
        if (req.hasRange && size > 0) {
            start = req.rangeStart < 0 ? std::max<int64_t>(0, size + req.rangeStart) : req.rangeStart;
            if (req.rangeStart >= 0 && req.rangeEnd >= 0) {
                end = std::min(req.rangeEnd, size - 1);
            }
        }
        if (start > end || start >= size) {
            res.begin(416, "", 0, {{"Content-Range", "bytes */" + std::to_string(size)}});
            return;
        }
        std::map<std::string, std::string> headers = {
                {"Accept-Ranges", "bytes"},
                {"Content-Disposition", "attachment; filename=\"" + std::to_string(megabytes) + "mb.bin\""}};
        if (req.hasRange) {
            headers["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size);
        }
        if (!res.begin(req.hasRange ? 206 : 200, "application/octet-stream", end - start + 1, headers) ||
            req.method == "HEAD") {
            return;
        }
        static const std::vector<char> zeros(256 * 1024, 0);
        for (int64_t left = end - start + 1; left > 0 && !m_stopping;) {
            size_t n = static_cast<size_t>(std::min<int64_t>(left, static_cast<int64_t>(zeros.size())));
            if (!res.write(zeros.data(), n)) {
                return;
            }
            left -= static_cast<int64_t>(n);
        }
    }

    // MatriX without a user: a known torrent may be played (/stream with play or m3u,
    // /play, /playlist), as players open these links without credentials
    bool Core::matrixPublic(const HttpRequest &req) {
        const std::string &p = req.path;
        std::string link;
        if (p == "/stream" || p.rfind("/stream/", 0) == 0) {
            if (!req.has("play") && !req.has("m3u")) {
                return false;
            }
            link = req.param("link");
        } else if (p.rfind("/play/", 0) == 0) {
            link = p.substr(6, p.find('/', 6) == std::string::npos ? std::string::npos : p.find('/', 6) - 6);
        } else if (p == "/playlist" || p.rfind("/playlist/", 0) == 0) {
            link = req.param("hash");
        } else {
            return false;
        }
        std::string hash = hashOfLink(link);
        return !hash.empty() && find(hash) != nullptr;
    }

    void Core::apiTorrents(const HttpRequest &req, HttpResponse &res, const User &user) {
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            error(res, 400, "a JSON object is expected");
            return;
        }
        const std::string action = str(body, "action");
        const std::string hash = str(body, "hash");
        if (action == "add") {
            std::string link = str(body, "link");
            if (link.empty()) {
                error(res, 400, "link is empty");
                return;
            }
            std::string err;
            TorrentPtr t = add(link, str(body, "title"), str(body, "poster"), str(body, "category"), str(body, "data"),
                               flag(body, "save_to_db") ? user.id : "", err);
            if (!t) {
                error(res, 400, err);
                return;
            }
            log("[Web] torrent added: %s (title %s, poster %s, category %s, data %s, %s)", t->rec.hash.c_str(),
                str(body, "title").empty() ? "-" : "+", str(body, "poster").empty() ? "-" : "+",
                str(body, "category").empty() ? "-" : "+", str(body, "data").empty() ? "-" : "+",
                flag(body, "save_to_db") ? "to the library" : "not saved");
            res.json(200, status(t).dump());
        } else if (action == "get") {
            TorrentPtr t = find(hash);
            if (!t) {
                res.json(404, "{}");
                return;
            }
            res.json(200, status(t).dump());
        } else if (action == "set") {
            TorrentPtr t = find(hash);
            if (!t) {
                error(res, 404, "torrent not found");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (body.contains("title")) t->rec.title = str(body, "title");
                if (body.contains("poster")) t->rec.poster = str(body, "poster");
                if (body.contains("category")) t->rec.category = str(body, "category");
                if (body.contains("data")) t->rec.data = str(body, "data");
            }
            save(t);
            res.json(200, "{}");
        } else if (action == "rem") {
            if (TorrentPtr t = find(hash)) {
                unlink(t, user.id);
            }
            res.json(200, "{}");
        } else if (action == "drop") {
            if (TorrentPtr t = find(hash)) {
                close(t);
            }
            res.json(200, "{}");
        } else if (action == "list") {
            json out = json::array();
            for (const TorrentPtr &t : listOf(user.id, true)) {
                out.push_back(status(t));
            }
            res.json(200, out.dump());
        } else if (action == "wipe") {
            for (const TorrentPtr &t : listOf(user.id, false)) {
                unlink(t, user.id);
            }
            res.json(200, "{}");
        } else {
            error(res, 400, "unknown action: \"" + action + "\"");
        }
    }

    // .torrent files from a form: "file" (or any file part), "title", "poster", "data", "save"
    void Core::apiUpload(const HttpRequest &req, HttpResponse &res, const User &user) {
        auto parts = parseMultipart(req);
        auto field = [&](const char *name) {
            auto it = parts.find(name);
            return it == parts.end() ? std::string() : it->second.second;
        };
        // MatriX: the field's presence saves, whatever its value
        const bool save = parts.count("save") > 0;
        json out = json::array();
        std::string lastError = "no file";
        for (const auto &part : parts) {
            if (part.second.first.empty()) {
                continue;  // not a file
            }
            std::string err;
            TorrentPtr t = addMetadata(part.second.second, field("title"), field("poster"), field("category"),
                                       field("data"), save ? user.id : "", err);
            if (t) {
                out.push_back(status(t));
            } else {
                lastError = err;
            }
        }
        if (out.empty()) {
            error(res, 400, lastError);
            return;
        }
        // TorrServer answers with the status of the (last) torrent
        res.json(200, out.back().dump());
    }

    // /stream?link=...&index=...&play|stat|m3u|preload|save and /play/<hash>/<id>
    void Core::apiStream(const HttpRequest &req, HttpResponse &res, const std::string &playHash, int playIndex,
                         const User &user) {
        const bool fromPlay = !playHash.empty();
        std::string link = fromPlay ? playHash : req.param("link");
        if (link.empty()) {
            error(res, 400, "link should not be empty");
            return;
        }
        // as MatriX: a torrent opened again (or open) takes the title, poster and category it
        // lacks from the parameters
        TorrentPtr t = find(link);
        std::string err;
        t = add(t ? t->rec.hash : link, req.param("title"), req.param("poster"), req.param("category"), "", "", err);
        if (!t) {
            error(res, 400, err);
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
        if (req.has("save") && user.valid()) {
            keep(t, user.id);
        }
        int files, single = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            files = static_cast<int>(t->fileIndex.size());
            single = files == 1 ? t->fileIndex.begin()->first : -1;
        }
        int index = fromPlay ? playIndex : -1;
        if (!fromPlay) {
            if (files == 1) {
                index = single;
            } else if (req.has("index") && !req.param("index").empty()) {
                index = std::atoi(req.param("index").c_str());
            }
        }
        const bool play = fromPlay || req.has("play");
        if (index < 1 && play) {
            error(res, 400, "\"index\" is empty or wrong");
            return;
        }
        if (req.has("preload") && index >= 1) {
            preload(t, index);
        }
        if (req.has("stat")) {
            res.json(200, status(t).dump());
        } else if (req.has("m3u")) {
            res.send(200, "audio/x-mpegurl; charset=utf-8", playlist({t}, req.header("host"), req.has("fromlast") ? 1 : 0, user.id, ""));
        } else if (play) {
            stream(t, index, req, res);
        } else {
            res.json(200, status(t).dump());
        }
    }

    void Core::apiViewed(const HttpRequest &req, HttpResponse &res, const User &user) {
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            error(res, 400, "a JSON object is expected");
            return;
        }
        const std::string action = str(body, "action");
        const std::string hash = str(body, "hash");
        if (action == "set") {
            if (hash.empty()) {
                error(res, 400, "hash is required");
                return;
            }
            // MatriX's list is of the files viewed: a set marks it, the timecode is the position
            ViewedRecord r = m_store.viewedFile(user.id, lower(hash), integer(body, "file_index", 0));
            if (body.contains("timecode") && body["timecode"].is_number()) {
                r.timecode = body["timecode"].get<double>();
            }
            r.viewed = true;
            r.updated = static_cast<int64_t>(time(nullptr));
            m_store.setViewed(r);
            res.json(200, "{}");
        } else if (action == "rem") {
            if (hash.empty()) {
                error(res, 400, "hash is required");
                return;
            }
            m_store.removeViewed(user.id, lower(hash), integer(body, "file_index", 0));
            res.json(200, "{}");
        } else if (action == "list") {
            json out = json::array();
            for (const ViewedRecord &r : m_store.viewed(user.id, lower(hash))) {
                if (r.viewed || r.timecode > 0) {
                    out.push_back({{"hash", r.hash}, {"file_index", r.fileIndex}, {"timecode", r.timecode}});
                }
            }
            res.json(200, out.dump());
        } else {
            error(res, 400, "unknown action: \"" + action + "\"");
        }
    }

    void Core::apiSettings(const HttpRequest &req, HttpResponse &res) {
        json body = json::parse(req.body, nullptr, false);
        const std::string action = body.is_object() ? str(body, "action") : "";
        if (action == "get") {
            res.json(200, settingsJson().dump());
            return;
        }
        if (action == "set" && body.contains("sets") && body["sets"].is_object()) {
            json s = settingsJson();
            for (auto it = body["sets"].begin(); it != body["sets"].end(); ++it) {
                s[it.key()] = it.value();
            }
            m_store.setSettings(s);
        } else if (action == "def") {
            m_store.setSettings(json::object());
            m_settings = Settings();
        } else {
            error(res, 400, "unknown action: \"" + action + "\"");
            return;
        }
        loadSettings();
        applySettings();
        log("[Web] settings changed (storage and the BitTorrent port apply at the next start)");
        res.json(200, "{}");
    }

    void Core::apiCache(const HttpRequest &req, HttpResponse &res) {
        json body = json::parse(req.body, nullptr, false);
        TorrentPtr t = body.is_object() ? find(str(body, "hash")) : nullptr;
        if (!t) {
            error(res, 404, "torrent not found");
            return;
        }
        json readers = json::array();
        int pieceLength, numPieces;
        lt::sha1_hash key;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto &w : t->windows) {
                readers.push_back({{"Start", w.second.first}, {"End", w.second.second}, {"Reader", w.second.first}});
            }
            pieceLength = t->pieceLength;
            numPieces = t->numPieces;
            key = t->cacheKey;
        }
        json out = {
                {"Hash", t->rec.hash},
                {"Capacity", m_settings.cacheSize},
                {"Filled", m_cache ? m_cache->bytesOf(key) : 0},
                {"PiecesLength", pieceLength},
                {"PiecesCount", numPieces},
                {"Torrent", status(t)},
                {"Pieces", json::object()},
                {"Readers", readers},
        };
        res.json(200, out.dump());
    }

    // the media files of the torrents; fromIndex: from the last viewed file on
    std::string Core::playlist(const std::vector<TorrentPtr> &torrents, const std::string &host, int fromIndex,
                               const std::string &user, const std::string &query) {
        std::string out = "#EXTM3U\n";
        const std::string base = "http://" + (host.empty() ? "127.0.0.1:" + std::to_string(httpPort()) : host);
        for (const TorrentPtr &t : torrents) {
            TorrentRecord rec;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                rec = t->rec;
            }
            int from = 0;
            if (fromIndex) {
                for (const ViewedRecord &v : m_store.viewed(user, rec.hash)) {
                    from = std::max(from, v.fileIndex);
                }
            }
            for (const FileInfo &f : rec.files) {
                if (f.id < from || !isMedia(f.path)) {
                    continue;
                }
                std::string name = baseName(f.path);
                out += "#EXTINF:0," + name + "\n";
                out += base + "/stream/" + HttpServer::urlEncode(name) + "?link=" + rec.hash + "&index=" +
                       std::to_string(f.id) + "&play" + query + "\n";
            }
        }
        return out;
    }

}

#endif
