#include "store.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

namespace p2p {

    using nlohmann::json;

    namespace {
        json readJson(const std::string &path) {
            std::ifstream in(path);
            if (!in) {
                return json();
            }
            std::stringstream ss;
            ss << in.rdbuf();
            return json::parse(ss.str(), nullptr, false);
        }

        std::string str(const json &j, const char *key) {
            auto it = j.find(key);
            return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        int64_t num(const json &j, const char *key) {
            auto it = j.find(key);
            return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
        }
    }

    Store::Store(std::string dir) : m_dir(std::move(dir)) {
        mkdir(m_dir.c_str(), 0777);
        mkdir((m_dir + "/torrents").c_str(), 0777);
    }

    void Store::load() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_torrents.clear();
        m_viewed.clear();
        json t = readJson(m_dir + "/torrents.json");
        if (t.is_array()) {
            for (const auto &item : t) {
                TorrentRecord r;
                r.hash = str(item, "hash");
                if (r.hash.empty()) {
                    continue;
                }
                r.link = str(item, "link");
                r.title = str(item, "title");
                r.poster = str(item, "poster");
                r.category = str(item, "category");
                r.data = str(item, "data");
                r.name = str(item, "name");
                r.timestamp = num(item, "timestamp");
                r.size = num(item, "size");
                if (item.contains("users") && item["users"].is_object()) {
                    for (auto it = item["users"].begin(); it != item["users"].end(); ++it) {
                        r.users[it.key()] = it.value().is_number() ? it.value().get<int64_t>() : r.timestamp;
                    }
                } else {
                    r.users["owner"] = r.timestamp;  // before the users: the owner's
                }
                if (item.contains("files") && item["files"].is_array()) {
                    for (const auto &f : item["files"]) {
                        r.files.push_back({static_cast<int>(num(f, "id")), str(f, "path"), num(f, "length")});
                    }
                }
                m_torrents.push_back(std::move(r));
            }
        }
        json v = readJson(m_dir + "/viewed.json");
        if (v.is_array()) {
            for (const auto &item : v) {
                ViewedRecord r;
                r.user = str(item, "user");
                r.hash = str(item, "hash");
                r.fileIndex = static_cast<int>(num(item, "file_index"));
                r.timecode = item.value("timecode", 0.0);
                r.duration = item.value("duration", 0.0);
                // before the users: MatriX's list, every file in it viewed
                r.viewed = item.value("viewed", r.user.empty());
                r.updated = num(item, "updated");
                if (r.user.empty()) {
                    r.user = "owner";
                }
                if (!r.hash.empty()) {
                    m_viewed.push_back(r);
                }
            }
        }
        json s = readJson(m_dir + "/settings.json");
        m_settings = s.is_object() ? s : json::object();
    }

    std::string Store::metadataPath(const std::string &hash) const {
        return m_dir + "/torrents/" + hash + ".torrent";
    }

    std::string Store::resumePath(const std::string &hash) const {
        return m_dir + "/torrents/" + hash + ".resume";
    }

    std::vector<TorrentRecord> Store::torrents() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_torrents;
    }

    bool Store::torrent(const std::string &hash, TorrentRecord &out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &r : m_torrents) {
            if (r.hash == hash) {
                out = r;
                return true;
            }
        }
        return false;
    }

    void Store::putTorrent(const TorrentRecord &record) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_torrents.begin(), m_torrents.end(),
                               [&](const TorrentRecord &r) { return r.hash == record.hash; });
        if (it == m_torrents.end()) {
            m_torrents.push_back(record);
        } else {
            *it = record;
        }
        saveTorrents();
    }

    void Store::removeTorrent(const std::string &hash) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_torrents.erase(std::remove_if(m_torrents.begin(), m_torrents.end(),
                                        [&](const TorrentRecord &r) { return r.hash == hash; }),
                         m_torrents.end());
        m_viewed.erase(std::remove_if(m_viewed.begin(), m_viewed.end(),
                                      [&](const ViewedRecord &r) { return r.hash == hash; }),
                       m_viewed.end());
        saveTorrents();
        saveViewed();
    }

    std::vector<ViewedRecord> Store::viewed(const std::string &user, const std::string &hash) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ViewedRecord> out;
        for (const auto &r : m_viewed) {
            if (r.user == user && (hash.empty() || r.hash == hash)) {
                out.push_back(r);
            }
        }
        return out;
    }

    ViewedRecord Store::viewedFile(const std::string &user, const std::string &hash, int fileIndex) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &r : m_viewed) {
            if (r.user == user && r.hash == hash && r.fileIndex == fileIndex) {
                return r;
            }
        }
        ViewedRecord r;
        r.user = user;
        r.hash = hash;
        r.fileIndex = fileIndex;
        return r;
    }

    void Store::setViewed(const ViewedRecord &record) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_viewed.begin(), m_viewed.end(), [&](const ViewedRecord &r) {
            return r.user == record.user && r.hash == record.hash && r.fileIndex == record.fileIndex;
        });
        if (it == m_viewed.end()) {
            m_viewed.push_back(record);
        } else {
            *it = record;
        }
        saveViewed();
    }

    std::vector<std::string> Store::removeUser(const std::string &user) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> orphans;
        for (auto &r : m_torrents) {
            if (r.users.erase(user) && r.users.empty()) {
                orphans.push_back(r.hash);
            }
        }
        m_viewed.erase(std::remove_if(m_viewed.begin(), m_viewed.end(),
                                      [&](const ViewedRecord &r) { return r.user == user; }),
                       m_viewed.end());
        saveTorrents();
        saveViewed();
        return orphans;
    }

    void Store::removeViewed(const std::string &user, const std::string &hash, int fileIndex) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_viewed.erase(std::remove_if(m_viewed.begin(), m_viewed.end(), [&](const ViewedRecord &r) {
            return r.user == user && r.hash == hash && (fileIndex < 1 || r.fileIndex == fileIndex);
        }), m_viewed.end());
        saveViewed();
    }

    json Store::settings() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings;
    }

    void Store::setSettings(const json &settings) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = settings;
        writeFile("settings.json", m_settings.dump(2));
    }

    void Store::saveTorrents() const {
        json t = json::array();
        for (const auto &r : m_torrents) {
            json files = json::array();
            for (const auto &f : r.files) {
                files.push_back({{"id", f.id}, {"path", f.path}, {"length", f.length}});
            }
            json users = json::object();
            for (const auto &u : r.users) {
                users[u.first] = u.second;
            }
            t.push_back({{"hash", r.hash}, {"link", r.link}, {"title", r.title}, {"poster", r.poster},
                         {"category", r.category}, {"data", r.data}, {"name", r.name},
                         {"timestamp", r.timestamp}, {"size", r.size}, {"files", files}, {"users", users}});
        }
        writeFile("torrents.json", t.dump(1));
    }

    void Store::saveViewed() const {
        json v = json::array();
        for (const auto &r : m_viewed) {
            v.push_back({{"user", r.user}, {"hash", r.hash}, {"file_index", r.fileIndex}, {"timecode", r.timecode},
                         {"duration", r.duration}, {"viewed", r.viewed}, {"updated", r.updated}});
        }
        writeFile("viewed.json", v.dump(1));
    }

    bool Store::writeFile(const std::string &name, const std::string &text) const {
        std::string path = m_dir + "/" + name;
        std::string tmp = path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            return false;
        }
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = std::fclose(f) == 0 && ok;
        if (!ok) {
            return false;
        }
        // some console file systems refuse to rename onto an existing file
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(path.c_str());
            return std::rename(tmp.c_str(), path.c_str()) == 0;
        }
        return true;
    }

}
