// The native API: the REST scheme of P2Play's silo branch under /api, with the viewing
// progress added (PUT .../progress, file_progress, GET /api/progress). File indexes as external host:
// the file's index in the torrent (pad files counted); listed in P2Play's order. Errors: {"error": "..."}. Every route needs a
// token (api_auth.cpp); without users set every request is the owner's.
// torrs:// links are added as any other and given in the status ("torrs"); the library
// goes in and out as .torrs (a torrs:// link a line). /api/events as external host. Not here: external host's
// transcoding, plugins and engine settings (the settings are legacy compatibility's /settings).

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

        // external host's file_hash: sha256 of "<file name>:<length>"
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

        // external host's index of a file (the torrent's order) from legacy compatibility's id, and back; -1: none
        int torrentFileIndex(const std::vector<FileInfo> &files, int id) {
            for (const FileInfo &f : files) {
                if (f.id == id) {
                    return f.index >= 0 ? f.index : f.id - 1;
                }
            }
            return -1;
        }

        int torrentFileId(const std::vector<FileInfo> &files, int index) {
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

    json Core::torrentStatus(const TorrentPtr &t, const std::string &user) {
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
            const int idx = torrentFileIndex(files, r.fileIndex);
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
    json Core::libraryProgress(const std::string &user, int64_t since) {
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
                           {"file_id", torrentFileIndex(rec.files, r.fileIndex)},
                           {"file_name", baseName(path)},
                           {"position", r.timecode},
                           {"duration", r.duration},
                           {"viewed", r.viewed},
                           {"updated_at", isoTime(r.updated)}});
        }
        return out;
    }

    void Core::apiUsers(const HttpRequest &req, HttpResponse &res, const User &user, const std::vector<std::string> &parts) {
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
                    if (r.viewed && torrentFileIndex(rec.files, r.fileIndex) >= 0) {
                        viewed.push_back(torrentFileIndex(rec.files, r.fileIndex));
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
