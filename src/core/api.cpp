// The TorrServer MatriX API, as Lampa and pPlay use it: /echo, /torrents, /torrent/upload,
// /stream, /play, /viewed, /settings, /cache, /playlist, /playlistall. File ids start
// from 1. Errors: {"error": "..."}.

#include <algorithm>
#include <cctype>

#include "core_torrent.hpp"

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

        // multipart/form-data: name -> (filename, content)
        std::map<std::string, std::pair<std::string, std::string>> parseMultipart(const HttpRequest &req) {
            std::map<std::string, std::pair<std::string, std::string>> parts;
            std::string type = req.header("content-type");
            size_t b = type.find("boundary=");
            if (b == std::string::npos) {
                return parts;
            }
            std::string boundary = type.substr(b + 9);
            if (!boundary.empty() && boundary[0] == '"') {
                boundary = boundary.substr(1, boundary.find('"', 1) - 1);
            }
            boundary = "--" + boundary.substr(0, boundary.find(';'));
            const std::string &body = req.body;
            size_t pos = body.find(boundary);
            while (pos != std::string::npos) {
                pos += boundary.size();
                if (body.compare(pos, 2, "--") == 0) {
                    break;
                }
                size_t headEnd = body.find("\r\n\r\n", pos);
                if (headEnd == std::string::npos) {
                    break;
                }
                std::string head = body.substr(pos, headEnd - pos);
                size_t next = body.find("\r\n" + boundary, headEnd + 4);
                if (next == std::string::npos) {
                    break;
                }
                std::string content = body.substr(headEnd + 4, next - headEnd - 4);
                auto attr = [&](const std::string &key) {
                    size_t k = head.find(key + "=\"");
                    if (k == std::string::npos) {
                        return std::string();
                    }
                    k += key.size() + 2;
                    return head.substr(k, head.find('"', k) - k);
                };
                std::string name = attr("name");
                if (!name.empty() && !parts.count(name)) {
                    parts[name] = {attr("filename"), content};
                }
                pos = next + 2;
            }
            return parts;
        }
    }

    void Core::handle(const HttpRequest &req, HttpResponse &res) {
        const std::string &p = req.path;
        const bool get = req.method == "GET" || req.method == "HEAD";
        const bool post = req.method == "POST";
        if (get && p == "/echo") {
            res.text(200, version());
        } else if (get && p == "/") {
            res.json(200, json{{"name", "P2Play"}, {"version", version()}}.dump());
        } else if (post && p == "/torrents") {
            apiTorrents(req, res);
        } else if (post && p == "/torrent/upload") {
            apiUpload(req, res);
        } else if (get && (p == "/stream" || p.rfind("/stream/", 0) == 0)) {
            apiStream(req, res, "", -1);
        } else if (get && p.rfind("/play/", 0) == 0) {
            // /play/<hash>/<id>
            std::string rest = p.substr(6);
            size_t slash = rest.find('/');
            int id = slash == std::string::npos ? 1 : std::atoi(rest.c_str() + slash + 1);
            apiStream(req, res, rest.substr(0, slash), id);
        } else if (post && p == "/viewed") {
            apiViewed(req, res);
        } else if (post && p == "/settings") {
            apiSettings(req, res);
        } else if (post && p == "/cache") {
            apiCache(req, res);
        } else if (get && (p == "/playlist" || p.rfind("/playlist/", 0) == 0)) {
            TorrentPtr t = find(req.param("hash"));
            if (!t) {
                error(res, 404, "torrent not found");
                return;
            }
            res.send(200, "audio/x-mpegurl; charset=utf-8", playlist({t}, req.header("host"), req.has("fromlast") ? 1 : 0));
        } else if (get && p == "/playlistall/all.m3u") {
            res.send(200, "audio/x-mpegurl; charset=utf-8", playlist(list(), req.header("host"), 0));
        } else {
            error(res, 404, "not found");
        }
    }

    void Core::apiTorrents(const HttpRequest &req, HttpResponse &res) {
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
                               flag(body, "save_to_db"), err);
            if (!t) {
                error(res, 400, err);
                return;
            }
            log("[Web] torrent added: %s", t->rec.hash.c_str());
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
                remove(t);
            }
            res.json(200, "{}");
        } else if (action == "drop") {
            if (TorrentPtr t = find(hash)) {
                close(t);
            }
            res.json(200, "{}");
        } else if (action == "list") {
            json out = json::array();
            for (const TorrentPtr &t : list()) {
                out.push_back(status(t));
            }
            res.json(200, out.dump());
        } else if (action == "wipe") {
            for (const TorrentPtr &t : list()) {
                remove(t);
            }
            res.json(200, "{}");
        } else {
            error(res, 400, "unknown action: \"" + action + "\"");
        }
    }

    // .torrent files from a form: "file" (or any file part), "title", "poster", "data", "save"
    void Core::apiUpload(const HttpRequest &req, HttpResponse &res) {
        auto parts = parseMultipart(req);
        auto field = [&](const char *name) {
            auto it = parts.find(name);
            return it == parts.end() ? std::string() : it->second.second;
        };
        const bool save = field("save") == "true" || field("save") == "on" || field("save") == "1";
        json out = json::array();
        std::string lastError = "no file";
        for (const auto &part : parts) {
            if (part.second.first.empty()) {
                continue;  // not a file
            }
            std::string err;
            TorrentPtr t = addMetadata(part.second.second, field("title"), field("poster"), "", field("data"), save, err);
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
    void Core::apiStream(const HttpRequest &req, HttpResponse &res, const std::string &playHash, int playIndex) {
        const bool fromPlay = !playHash.empty();
        std::string link = fromPlay ? playHash : req.param("link");
        if (link.empty()) {
            error(res, 400, "link should not be empty");
            return;
        }
        TorrentPtr t = find(link);
        std::string err;
        if (!t || !t->active) {
            t = add(link, req.param("title"), req.param("poster"), req.param("category"), "", false, err);
            if (!t) {
                error(res, 400, err);
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->lastUse = Clock::now();
        }
        if (!waitInfo(t, kInfoTimeout)) {
            error(res, 500, "torrent connection timeout");
            return;
        }
        if (req.has("save")) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                t->saved = true;
            }
            save(t);
        }
        int files;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            files = static_cast<int>(t->fileIndex.size());
        }
        int index = fromPlay ? playIndex : -1;
        if (!fromPlay) {
            if (files == 1) {
                index = 1;
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
            res.send(200, "audio/x-mpegurl; charset=utf-8", playlist({t}, req.header("host"), req.has("fromlast") ? 1 : 0));
        } else if (play) {
            stream(t, index, req, res);
        } else {
            res.json(200, status(t).dump());
        }
    }

    void Core::apiViewed(const HttpRequest &req, HttpResponse &res) {
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
            ViewedRecord r;
            r.hash = hash;
            r.fileIndex = integer(body, "file_index", 0);
            r.timecode = body.contains("timecode") && body["timecode"].is_number() ? body["timecode"].get<double>() : 0;
            m_store.setViewed(r);
            res.json(200, "{}");
        } else if (action == "rem") {
            if (hash.empty()) {
                error(res, 400, "hash is required");
                return;
            }
            m_store.removeViewed(hash, integer(body, "file_index", 0));
            res.json(200, "{}");
        } else if (action == "list") {
            json out = json::array();
            for (const ViewedRecord &r : m_store.viewed(hash)) {
                out.push_back({{"hash", r.hash}, {"file_index", r.fileIndex}, {"timecode", r.timecode}});
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
    std::string Core::playlist(const std::vector<TorrentPtr> &torrents, const std::string &host, int fromIndex) {
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
                for (const ViewedRecord &v : m_store.viewed(rec.hash)) {
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
                       std::to_string(f.id) + "&play\n";
            }
        }
        return out;
    }

}
