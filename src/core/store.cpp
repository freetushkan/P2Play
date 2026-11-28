#include "store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

        bool numericAddress(const std::string &host) {
            in_addr v4{};
            in6_addr v6{};
            return inet_pton(AF_INET, host.c_str(), &v4) == 1 ||
                   inet_pton(AF_INET6, host.c_str(), &v6) == 1;
        }

        // Never include supplied values (especially credentials) in validation errors.
        PublisherProxy parseProxy(const json &settings) {
            PublisherProxy p;
            auto it = settings.find("PublisherProxy");
            if (it == settings.end()) return p;
            if (!it->is_object() || !it->contains("Enabled") || !(*it)["Enabled"].is_boolean()) {
                throw std::invalid_argument("PublisherProxy requires a boolean Enabled field");
            }
            const json &j = *it;
            for (auto field = j.begin(); field != j.end(); ++field) {
                if (field.key() != "Enabled" && field.key() != "Host" && field.key() != "Port" &&
                    field.key() != "Username" && field.key() != "Password" && field.key() != "CredentialsSet") {
                    throw std::invalid_argument("PublisherProxy has an unsupported field");
                }
            }
            p.enabled = j["Enabled"].get<bool>();
            auto text = [&](const char *key, std::string &out) {
                if (!j.contains(key)) return;
                if (!j[key].is_string()) throw std::invalid_argument("PublisherProxy text field has invalid type");
                out = j[key].get<std::string>();
                if (out.find('\0') != std::string::npos) {
                    throw std::invalid_argument("PublisherProxy text fields must not contain NUL");
                }
            };
            text("Host", p.host);
            text("Username", p.username);
            text("Password", p.password);
            if (j.contains("Port")) {
                if (!j["Port"].is_number_integer() || j["Port"] < 1 || j["Port"] > 65535) {
                    throw std::invalid_argument("PublisherProxy Port must be an integer in 1..65535");
                }
                p.port = j["Port"].get<int>();
            }
            // The pinned libtorrent SOCKS5 UDP implementation uses an IPv4 proxy
            // interface and cannot consume an IPv6 UDP ASSOCIATE relay reply.
            in_addr v4{};
            if ((!p.host.empty() || p.enabled) && inet_pton(AF_INET, p.host.c_str(), &v4) != 1) {
                throw std::invalid_argument("PublisherProxy Host must be a numeric IPv4 address (no local DNS)");
            }
            if (p.enabled && (p.host.empty() || p.port == 0 || p.host == "0.0.0.0")) {
                throw std::invalid_argument("PublisherProxy requires a concrete Host and Port");
            }
            if (p.username.size() > 255 || p.password.size() > 255 ||
                p.username.empty() != p.password.empty()) {
                throw std::invalid_argument("PublisherProxy requires paired credentials of 1..255 bytes, or neither");
            }
            if (p.enabled && settings.contains("P2PDhtBootstrap")) {
                if (!settings["P2PDhtBootstrap"].is_string()) {
                    throw std::invalid_argument("proxied P2PDhtBootstrap must be a string of numeric IP:port nodes");
                }
                const std::string nodes = settings["P2PDhtBootstrap"].get<std::string>();
                size_t pos = 0;
                while (pos < nodes.size()) {
                    const size_t end = nodes.find(',', pos);
                    const std::string node = nodes.substr(pos, end == std::string::npos ? end : end - pos);
                    const size_t colon = node.rfind(':');
                    std::string host = colon == std::string::npos ? "" : node.substr(0, colon);
                    if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
                        host = host.substr(1, host.size() - 2);
                    }
                    const std::string port = colon == std::string::npos ? "" : node.substr(colon + 1);
                    if (!numericAddress(host) || port.empty() || port.size() > 5 ||
                        !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                        std::atoi(port.c_str()) < 1 || std::atoi(port.c_str()) > 65535 || end == nodes.size() - 1) {
                        throw std::invalid_argument("proxied P2PDhtBootstrap requires numeric IP:port nodes (no local DNS)");
                    }
                    pos = end == std::string::npos ? nodes.size() : end + 1;
                }
            }
            return p;
        }

        bool sameProxy(const PublisherProxy &a, const PublisherProxy &b) {
            return std::tie(a.enabled, a.host, a.port, a.username, a.password) ==
                   std::tie(b.enabled, b.host, b.port, b.username, b.password);
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
                        r.files.push_back({static_cast<int>(num(f, "id")), str(f, "path"), num(f, "length"),
                                           f.contains("index") ? static_cast<int>(num(f, "index")) : -1});
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
                // before per-user state: every file in the saved list was viewed
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
        // A damaged/unreadable settings file must not silently disable the proxy.
        const std::string settingsPath = m_dir + "/settings.json";
        struct stat st{};
        const bool exists = stat(settingsPath.c_str(), &st) == 0;
        if (!exists && errno != ENOENT) {
            throw std::runtime_error("cannot inspect settings.json; refusing network startup");
        }
        json s = exists ? readJson(settingsPath) : json::object();
        if (!s.is_object()) {
            throw std::runtime_error("settings.json must be a readable JSON object; refusing network startup");
        }
        PublisherProxy proxy = parseProxy(s);
        m_settings = std::move(s);
        m_publisherProxy = std::move(proxy);
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

    void Store::renumberViewed(const std::string &hash, const std::map<int, int> &moved) {
        if (moved.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &r : m_viewed) {
            auto it = moved.find(r.fileIndex);
            if (r.hash == hash && it != moved.end()) {
                r.fileIndex = it->second;
            }
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

    json Store::publicSettings() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        json s = m_settings;
        if (s.contains("PublisherProxy")) {
            auto &p = s["PublisherProxy"];
            p.erase("Username");
            p.erase("Password");
            p["CredentialsSet"] = !m_publisherProxy.username.empty();
        }
        return s;
    }

    PublisherProxy Store::publisherProxy() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_publisherProxy;
    }

    void Store::setSettings(const json &settings) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!settings.is_object()) throw std::invalid_argument("settings must be an object");
        json next = settings;
        if (next.contains("PublisherProxy") && next["PublisherProxy"].is_object()) {
            auto &p = next["PublisherProxy"];
            // API reads redact credentials. An unrelated update must not erase them.
            if (!p.contains("Username")) p["Username"] = m_publisherProxy.username;
            if (!p.contains("Password")) p["Password"] = m_publisherProxy.password;
            p.erase("CredentialsSet");
        }
        const PublisherProxy proxy = parseProxy(next);
        if (!sameProxy(proxy, m_publisherProxy)) {
            throw std::invalid_argument("PublisherProxy is startup-only: stop, edit settings.json, then restart");
        }
        if (!writeFile("settings.json", next.dump(2))) {
            throw std::runtime_error("cannot persist settings.json");
        }
        m_settings = std::move(next);
    }

    void Store::saveTorrents() const {
        json t = json::array();
        for (const auto &r : m_torrents) {
            json files = json::array();
            for (const auto &f : r.files) {
                files.push_back({{"id", f.id}, {"path", f.path}, {"length", f.length}, {"index", f.index}});
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
        if (name == "settings.json") {
            // The proxy password is plaintext at rest; do not make replacement
            // settings world-readable on POSIX systems (console FS may ignore it).
            chmod(tmp.c_str(), 0600);
        }
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = std::fclose(f) == 0 && ok;
        if (!ok) {
            return false;
        }
        // some console file systems refuse to rename onto an existing file
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            // Never remove the last valid transport policy to update settings.
            // A filesystem without replace-by-rename must be edited while stopped.
            if (name == "settings.json") return false;
            std::remove(path.c_str());
            return std::rename(tmp.c_str(), path.c_str()) == 0;
        }
        return true;
    }

}
