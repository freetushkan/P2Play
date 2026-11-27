// P2Play's native, versioned library API. This file is deliberately independent of
// the optional TorrServer MatriX compatibility layer (P2P_COMPAT_MATRIX).
#include "core_torrent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <limits>
#include <libtorrent/peer_info.hpp>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <chrono>
#include <libtorrent/version.hpp>

namespace p2p {
    using nlohmann::json;

    namespace {
        constexpr auto kNativeInfoTimeout = std::chrono::seconds(60);

        bool apiError(HttpResponse &res, int status, const std::string &message) {
            return res.json(status, json{{"error", message}}.dump());
        }

        bool apiOk(HttpResponse &res, int status, const json &value) {
            return res.json(status, value.dump());
        }

        json apiBody(const HttpRequest &req) {
            json value = json::parse(req.body, nullptr, false);
            return value.is_object() ? value : json();
        }

        std::string apiString(const json &value, const char *key) {
            auto it = value.find(key);
            return it != value.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        std::string apiTrim(std::string value) {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return "";
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        bool apiUnsigned(const std::string &value, size_t &result) {
            if (value.empty() || value.size() > 18 ||
                !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                return false;
            }
            try {
                const unsigned long long parsed = std::stoull(value);
                if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) return false;
                result = static_cast<size_t>(parsed);
                return true;
            } catch (...) {
                return false;
            }
        }

        bool apiSafeText(const std::string &value, size_t limit) {
            return value.size() <= limit &&
                   std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
        }

        std::vector<std::string> apiWords(const std::string &folded) {
            std::vector<std::string> result;
            std::string word;
            for (unsigned char c : folded) {
                if (c < 0x80 && !std::isalnum(c)) {
                    if (!word.empty()) {
                        result.push_back(word);
                        word.clear();
                    }
                } else {
                    word += static_cast<char>(c);
                }
            }
            if (!word.empty()) result.push_back(word);
            return result;
        }
    }

    bool Core::routeP2PlayV1(const HttpRequest &req, HttpResponse &res, const User &user,
                             const std::vector<std::string> &parts) {
        const std::string &method = req.method;
        const bool get = method == "GET" || method == "HEAD";
        const size_t count = parts.size();
        auto at = [&](size_t index) { return index < count ? parts[index] : std::string(); };

        // This is a product-owned response schema, not a MatriX/Silo status payload.
        auto item = [&](const TorrentPtr &torrent) {
            TorrentRecord record;
            int64_t addedAt = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                record = torrent->rec;
                auto it = record.users.find(user.id);
                if (it != record.users.end()) addedAt = it->second;
            }
            const json state = status(torrent);
            return json{
                {"hash", record.hash},
                {"title", record.title},
                {"poster", record.poster},
                {"category", record.category},
                {"size", record.size},
                {"timestamp", record.timestamp},
                {"added_at", addedAt},
                {"state", state.value("stat", 0)},
                {"state_text", state.value("stat_string", std::string())},
                {"file_count", record.files.size()},
                {"download_speed", state.value("download_speed", 0.0)},
                {"upload_speed", state.value("upload_speed", 0.0)}
            };
        };

        // GET /api/v1/library/categories
        if (get && count == 4 && at(2) == "library" && at(3) == "categories") {
            std::map<std::string, std::string> categories;
            for (const TorrentPtr &torrent : listOf(user.id, false)) {
                std::string category;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    category = torrent->rec.category;
                }
                if (!category.empty()) categories.emplace(meshFold(category), category);
            }
            json values = json::array();
            for (const auto &category : categories) values.push_back(category.second);
            return apiOk(res, 200, {{"categories", values}});
        }

        // GET /api/v1/library/torrents
        // Search, category filtering and sorting happen over the complete library before paging.
        if (get && count == 4 && at(2) == "library" && at(3) == "torrents") {
            struct Entry {
                TorrentPtr torrent;
                TorrentRecord record;
                int64_t addedAt = 0;
            };
            std::vector<Entry> all;
            std::map<std::string, std::string> categoryMap;
            for (const TorrentPtr &torrent : listOf(user.id, false)) {
                Entry entry;
                entry.torrent = torrent;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    entry.record = torrent->rec;
                    auto added = entry.record.users.find(user.id);
                    if (added != entry.record.users.end()) entry.addedAt = added->second;
                }
                if (!entry.record.category.empty()) {
                    categoryMap.emplace(meshFold(entry.record.category), entry.record.category);
                }
                all.push_back(std::move(entry));
            }

            json categories = json::array();
            for (const auto &category : categoryMap) categories.push_back(category.second);

            const std::vector<std::string> terms = apiWords(meshFold(req.param("q")));
            const std::string categoryFilter = meshFold(apiTrim(req.param("category")));
            std::vector<Entry> filtered;
            filtered.reserve(all.size());
            for (Entry &entry : all) {
                const std::string title = meshFold(entry.record.title);
                const std::string hash = meshFold(entry.record.hash);
                const std::string category = meshFold(entry.record.category);
                if (!categoryFilter.empty()) {
                    if (categoryFilter == "none") {
                        if (!entry.record.category.empty()) continue;
                    } else if (category != categoryFilter) {
                        continue;
                    }
                }
                const std::string haystack = title + " " + hash + " " + category;
                bool matches = true;
                for (const std::string &term : terms) {
                    if (haystack.find(term) == std::string::npos) {
                        matches = false;
                        break;
                    }
                }
                if (matches) filtered.push_back(std::move(entry));
            }

            const std::string sort = req.param("sort").empty() ? "date" : req.param("sort");
            if (sort != "date" && sort != "title" && sort != "size") {
                return apiError(res, 400, "sort must be date, title or size");
            }
            const std::string order = req.param("order").empty() ? (sort == "title" ? "asc" : "desc") : req.param("order");
            if (order != "asc" && order != "desc") {
                return apiError(res, 400, "order must be asc or desc");
            }
            const bool descending = order == "desc";
            std::stable_sort(filtered.begin(), filtered.end(), [&](const Entry &left, const Entry &right) {
                int comparison = 0;
                if (sort == "date") {
                    comparison = left.addedAt < right.addedAt ? -1 : left.addedAt > right.addedAt ? 1 : 0;
                } else if (sort == "size") {
                    comparison = left.record.size < right.record.size ? -1 : left.record.size > right.record.size ? 1 : 0;
                } else {
                    const std::string a = meshFold(left.record.title);
                    const std::string b = meshFold(right.record.title);
                    comparison = a < b ? -1 : a > b ? 1 : 0;
                }
                if (comparison == 0) return left.record.hash < right.record.hash;
                return descending ? comparison > 0 : comparison < 0;
            });

            size_t offset = 0, limit = filtered.size();
            if (req.has("offset") && !apiUnsigned(req.param("offset"), offset)) {
                return apiError(res, 400, "offset must be a non-negative integer");
            }
            if (req.has("limit")) {
                if (!apiUnsigned(req.param("limit"), limit) || limit == 0 || limit > 1000) {
                    return apiError(res, 400, "limit must be between 1 and 1000");
                }
            }

            json values = json::array();
            const size_t end = std::min(filtered.size(), offset > filtered.size() ? filtered.size() : offset + std::min(limit, filtered.size() - offset));
            for (size_t i = std::min(offset, filtered.size()); i < end; ++i) {
                values.push_back(item(filtered[i].torrent));
            }
            return apiOk(res, 200, {
                {"items", values},
                {"total", filtered.size()},
                {"offset", offset},
                {"limit", limit},
                {"categories", categories}
            });
        }

        // POST /api/v1/torrents — JSON {link,title,category} or multipart {file,title,category}.
        // A successful POST always adds the torrent to this user's library.
        if (count == 3 && at(2) == "torrents" && method == "POST") {
            std::string link, title, category, torrentFile;
            bool hasFile = false;
            const std::string contentType = req.header("content-type");
            if (contentType.rfind("multipart/form-data", 0) == 0) {
                const auto fields = parseMultipart(req);
                auto field = [&](const char *name) {
                    auto it = fields.find(name);
                    return it == fields.end() ? std::string() : it->second.second;
                };
                link = apiTrim(field("link"));
                title = apiTrim(field("title"));
                category = apiTrim(field("category"));
                auto file = fields.find("file");
                if (file != fields.end()) {
                    if (file->second.first.empty()) return apiError(res, 400, "file part must include a filename");
                    hasFile = true;
                    torrentFile = file->second.second;
                }
            } else {
                const json body = apiBody(req);
                if (body.is_null()) return apiError(res, 400, "a JSON object is expected");
                link = apiTrim(apiString(body, "link"));
                title = apiTrim(apiString(body, "title"));
                category = apiTrim(apiString(body, "category"));
            }

            if (hasFile && !link.empty()) return apiError(res, 400, "provide either a torrent file or a link, not both");
            if (hasFile && torrentFile.empty()) return apiError(res, 400, "torrent file is empty");
            if (!hasFile && link.empty()) return apiError(res, 400, "a magnet/link or a .torrent file is required");
            if (hasFile && torrentFile.size() > (16u * 1024u * 1024u)) {
                return apiError(res, 413, "torrent file exceeds the 16 MiB upload limit");
            }
            if (!hasFile && (!apiSafeText(link, 16384) || link.find_first_of("\r\n") != std::string::npos)) {
                return apiError(res, 400, "link is invalid or too long");
            }
            if (!apiSafeText(title, 512) || !apiSafeText(category, 128)) {
                return apiError(res, 400, "title or category is too long or contains control characters");
            }

            std::string error;
            TorrentPtr torrent = hasFile
                ? addMetadata(torrentFile, title, "", category, "", user.id, error)
                : add(link, title, "", category, "", user.id, error);
            if (!torrent) return apiError(res, 400, error.empty() ? "torrent could not be added" : error);

            log("[Web] native library add by %s: %s (%s)", user.name.c_str(), torrent->rec.hash.c_str(),
                hasFile ? "torrent file" : "link");
            return apiOk(res, 201, {{"torrent", item(torrent)}});
        }

        // GET /api/v1/progress — per-user Continue Watching.
        if (get && count == 3 && at(2) == "progress") {
            size_t since = 0;
            if (req.has("since") && !apiUnsigned(req.param("since"), since)) {
                return apiError(res, 400, "since must be a non-negative Unix timestamp");
            }
            return apiOk(res, 200, {{"progress", siloProgress(user.id, static_cast<int64_t>(since))}});
        }

        // Import/export portable .torrs library lists.
        if (count == 4 && at(2) == "library" && at(3) == "export" && get) {
            std::string contents;
            for (const TorrentPtr &torrent : listOf(user.id, false)) {
                const std::string line = torrsOf(torrent);
                if (!line.empty()) contents += line + "\n";
            }
            return res.send(200, "text/plain; charset=utf-8", contents,
                            {{"Content-Disposition", "attachment; filename=\"p2play-library.torrs\""}});
        }
        if (count == 4 && at(2) == "library" && at(3) == "import" && method == "POST") {
            std::string contents = req.body;
            if (req.header("content-type").rfind("multipart/form-data", 0) == 0) {
                const auto fields = parseMultipart(req);
                auto it = fields.find("file");
                contents = it == fields.end() ? std::string() : it->second.second;
            }
            if (contents.empty() || contents.size() > 16u * 1024u * 1024u) {
                return apiError(res, contents.empty() ? 400 : 413,
                                contents.empty() ? "a non-empty torrs library is required" : "library import exceeds 16 MiB");
            }
            int added = 0, skipped = 0, rejected = 0;
            size_t pos = 0;
            while (pos < contents.size()) {
                const size_t end = contents.find('\n', pos);
                const std::string line = apiTrim(contents.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
                pos = end == std::string::npos ? contents.size() : end + 1;
                if (line.empty() || line[0] == '#') continue;
                const std::string link = line.rfind("torrs://", 0) == 0 ? line : "torrs://" + line;
                const std::string hash = hashOfLink(link);
                TorrentPtr known = hash.empty() ? nullptr : find(hash);
                if (known && owns(known, user.id)) { ++skipped; continue; }
                if (addCard(line, user.id)) ++added;
                else ++rejected;
            }
            log("[Web] native library import by %s: added=%d skipped=%d rejected=%d",
                user.name.c_str(), added, skipped, rejected);
            return apiOk(res, 200, {{"status", "imported"}, {"added", added},
                                    {"skipped", skipped}, {"rejected", rejected}});
        }

        // User management shares the core account store and permission checks with Silo.
        if (count >= 3 && at(2) == "users") {
            std::vector<std::string> siloParts = {"api", "users"};
            siloParts.insert(siloParts.end(), parts.begin() + 3, parts.end());
            siloUsers(req, res, user, siloParts);
            return true;
        }

        // Owner-only engine settings. Values are whitelisted and persisted through Store.
        if (count == 3 && at(2) == "settings") {
            if (user.rank < 100) return apiError(res, 403, "owner access required");
            if (get) return apiOk(res, 200, {{"settings", settingsJson()}, {"restart_required", false}});
            if (method != "PUT" && method != "PATCH") return apiError(res, 405, "method not allowed");
            const json body = apiBody(req);
            const json values = body.value("settings", body);
            if (!values.is_object() || values.empty()) return apiError(res, 400, "a non-empty settings object is required");

            json next = settingsJson();
            bool restartRequired = false;
            static const std::set<std::string> booleanKeys = {
                "UseDisk", "DisableUpload", "DisableTCP", "DisableUTP",
                "DisableDHT", "DisableMeshDirectory", "DisableUPNP"
            };
            static const std::set<std::string> integerKeys = {
                "CacheSize", "ReaderReadAHead", "PreloadCache", "TorrentDisconnectTimeout",
                "ConnectionsLimit", "DownloadRateLimit", "UploadRateLimit", "PeersListenPort"
            };
            static const std::set<std::string> stringKeys = {"TorrentsSavePath", "P2PDhtBootstrap"};
            for (auto it = values.begin(); it != values.end(); ++it) {
                const std::string &key = it.key();
                if (booleanKeys.count(key)) {
                    if (!it.value().is_boolean()) return apiError(res, 400, key + " must be boolean");
                    next[key] = it.value();
                    if (key == "UseDisk") restartRequired = true;
                } else if (integerKeys.count(key)) {
                    if (!it.value().is_number_integer()) return apiError(res, 400, key + " must be an integer");
                    const int64_t value = it.value().get<int64_t>();
                    int64_t minimum = 0, maximum = 1000000000;
                    if (key == "CacheSize") { minimum = 16LL * 1024 * 1024; maximum = 8LL * 1024 * 1024 * 1024; }
                    else if (key == "ReaderReadAHead") { minimum = 5; maximum = 100; }
                    else if (key == "PreloadCache") { minimum = 0; maximum = 100; }
                    else if (key == "TorrentDisconnectTimeout") { minimum = 5; maximum = 86400; }
                    else if (key == "ConnectionsLimit") { minimum = 5; maximum = 1000; }
                    else if (key == "PeersListenPort") { minimum = 0; maximum = 65535; restartRequired = true; }
                    if (value < minimum || value > maximum) return apiError(res, 400, key + " is outside the supported range");
                    next[key] = value;
                } else if (stringKeys.count(key)) {
                    if (!it.value().is_string()) return apiError(res, 400, key + " must be a string");
                    const std::string value = it.value().get<std::string>();
                    const size_t maximum = key == "TorrentsSavePath" ? 1024 : 4096;
                    if (!apiSafeText(value, maximum)) return apiError(res, 400, key + " is too long or contains control characters");
                    next[key] = value;
                    if (key == "TorrentsSavePath" || key == "P2PDhtBootstrap") restartRequired = true;
                } else {
                    return apiError(res, 400, "setting is read-only or unknown: " + key);
                }
            }
            try {
                m_store.setSettings(next);
                loadSettings();
                applySettings();
            } catch (const std::exception &error) {
                return apiError(res, 400, error.what());
            }
            log("[Web] native engine settings updated by %s", user.name.c_str());
            return apiOk(res, 200, {{"settings", settingsJson()}, {"restart_required", restartRequired}});
        }

        // GET /api/v1/system/stats
        if (get && count == 4 && at(2) == "system" && at(3) == "stats") {
            int active = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto &entry : m_torrents) active += entry.second->active ? 1 : 0;
            }
            const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - m_started).count();
            const auto cache = m_cache ? m_cache->stats() : RamCache::Stats{};
            return apiOk(res, 200, {
                {"stats", {{"active_sessions", active}, {"active_readers", activeStreams()},
                           {"cache_bytes", cache.bytes}, {"cache_capacity", cache.capacity},
                           {"cache_pieces", cache.pieces}, {"cache_evicted", cache.evicted}}},
                {"uptime_sec", uptime}, {"version", productVersion()},
                {"torrent", std::string("libtorrent ") + LIBTORRENT_VERSION}
            });
        }

        // GET/PUT/DELETE /api/v1/torrents/{infohash}
        if (count == 4 && at(2) == "torrents") {
            const std::string hash = meshFold(at(3));
            TorrentPtr torrent = find(hash);
            if (!torrent || !owns(torrent, user.id)) {
                return apiError(res, 404, "torrent not found in your library");
            }
            if (get) return apiOk(res, 200, {{"torrent", item(torrent)}, {"details", siloStatus(torrent, user.id)}});

            if (method == "PUT") {
                const json body = apiBody(req);
                if (body.is_null()) return apiError(res, 400, "a JSON object is expected");
                const bool hasTitle = body.contains("title");
                const bool hasCategory = body.contains("category");
                if (!hasTitle && !hasCategory) return apiError(res, 400, "title or category is required");
                if ((hasTitle && !body["title"].is_string()) || (hasCategory && !body["category"].is_string())) {
                    return apiError(res, 400, "title and category must be strings");
                }
                const std::string title = hasTitle ? apiTrim(body["title"].get<std::string>()) : "";
                const std::string category = hasCategory ? apiTrim(body["category"].get<std::string>()) : "";
                if ((hasTitle && !apiSafeText(title, 512)) || (hasCategory && !apiSafeText(category, 128))) {
                    return apiError(res, 400, "title or category is too long or contains control characters");
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (hasTitle) torrent->rec.title = title;
                    if (hasCategory) torrent->rec.category = category;
                }
                save(torrent);
                return apiOk(res, 200, {{"torrent", item(torrent)}});
            }

            if (method == "DELETE") {
                unlink(torrent, user.id);
                return apiOk(res, 200, {{"status", "removed"}, {"hash", hash}});
            }
            return apiError(res, 405, "method not allowed");
        }


        // Detailed torrent controls and file actions. File indexes are zero-based, matching
        // Silo and the native v1 API. Store records retain the core's one-based file IDs.
        if (count >= 5 && at(2) == "torrents") {
            const std::string hash = meshFold(at(3));
            TorrentPtr torrent = find(hash);
            if (!torrent || !owns(torrent, user.id)) return apiError(res, 404, "torrent not found in your library");

            auto ensureMetadata = [&]() {
                std::string addError;
                if (!add(hash, "", "", "", "", "", addError)) {
                    apiError(res, 503, addError.empty() ? "torrent could not be activated" : addError);
                    return false;
                }
                if (!waitInfo(torrent, kNativeInfoTimeout)) {
                    apiError(res, 504, "torrent metadata is not ready yet");
                    return false;
                }
                return true;
            };
            auto fileAt = [&](int index, FileInfo &selected) {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const FileInfo &file : torrent->rec.files) {
                    const int externalIndex = file.index >= 0 ? file.index : file.id - 1;
                    if (externalIndex == index) { selected = file; return true; }
                }
                return false;
            };
            auto parseFileIndex = [&](const std::string &text, int &index) {
                size_t value = 0;
                if (!apiUnsigned(text, value) || value > 1000000) return false;
                index = static_cast<int>(value);
                return true;
            };
            auto validFolder = [](std::string value, std::string &folder) {
                value = apiTrim(value);
                while (!value.empty() && value.front() == '/') value.erase(value.begin());
                while (!value.empty() && value.back() == '/') value.pop_back();
                if (value.empty() || value.size() > 1024 || value.find('\\') != std::string::npos ||
                    value.find_first_of("\r\n") != std::string::npos) return false;
                size_t pos = 0;
                while (pos <= value.size()) {
                    size_t end = value.find('/', pos);
                    if (end == std::string::npos) end = value.size();
                    const std::string part = value.substr(pos, end - pos);
                    if (part.empty() || part == "." || part == "..") return false;
                    if (end == value.size()) break;
                    pos = end + 1;
                }
                folder = std::move(value);
                return true;
            };

            if (count == 5 && at(4) == "files" && get) {
                if (!ensureMetadata()) return true; // ensureMetadata wrote the error response
                return apiOk(res, 200, {{"hash", hash}, {"files", siloStatus(torrent, user.id).value("file_stats", json::array())},
                                        {"file_progress", siloStatus(torrent, user.id).value("file_progress", json::array())},
                                        {"viewed_files", siloStatus(torrent, user.id).value("viewed_files", json::array())}});
            }

            if (count == 5 && at(4) == "wake" && method == "POST") {
                if (!ensureMetadata()) return true;
                return apiOk(res, 200, {{"status", "active"}, {"torrent", item(torrent)}});
            }

            if (count == 5 && at(4) == "viewed" && method == "PUT") {
                const json body = apiBody(req);
                if (!body.contains("viewed") || !body["viewed"].is_boolean()) {
                    return apiError(res, 400, "viewed must be a boolean");
                }
                std::vector<FileInfo> files;
                { std::lock_guard<std::mutex> lock(m_mutex); files = torrent->rec.files; }
                if (files.empty() && !ensureMetadata()) return true;
                { std::lock_guard<std::mutex> lock(m_mutex); files = torrent->rec.files; }
                const bool viewed = body["viewed"].get<bool>();
                const int64_t now = static_cast<int64_t>(time(nullptr));
                for (const FileInfo &file : files) {
                    ViewedRecord record = m_store.viewedFile(user.id, hash, file.id);
                    record.viewed = viewed;
                    record.updated = now;
                    m_store.setViewed(record);
                }
                return apiOk(res, 200, {{"status", "updated"}, {"viewed", viewed}, {"files", files.size()}});
            }

            if (count == 5 && at(4) == "progress" && method == "DELETE") {
                m_store.removeViewed(user.id, hash, 0);
                return apiOk(res, 200, {{"status", "reset"}});
            }

            if (count == 6 && at(4) == "folders" && at(5) == "viewed" && method == "PUT") {
                if (!ensureMetadata()) return true;
                const json body = apiBody(req);
                std::string folder;
                if (!body.contains("path") || !body["path"].is_string() ||
                    !body.contains("viewed") || !body["viewed"].is_boolean() ||
                    !validFolder(apiString(body, "path"), folder)) {
                    return apiError(res, 400, "a valid folder path and viewed boolean are required");
                }
                std::vector<FileInfo> matches;
                { std::lock_guard<std::mutex> lock(m_mutex);
                  for (const FileInfo &file : torrent->rec.files) {
                      if (file.path.size() > folder.size() && file.path.compare(0, folder.size(), folder) == 0 &&
                          file.path[folder.size()] == '/') matches.push_back(file);
                  }
                }
                if (matches.empty()) return apiError(res, 404, "folder contains no torrent files");
                const bool viewed = body["viewed"].get<bool>();
                const int64_t now = static_cast<int64_t>(time(nullptr));
                for (const FileInfo &file : matches) {
                    ViewedRecord record = m_store.viewedFile(user.id, hash, file.id);
                    record.viewed = viewed;
                    record.updated = now;
                    m_store.setViewed(record);
                }
                return apiOk(res, 200, {{"status", "updated"}, {"viewed", viewed}, {"files", matches.size()}});
            }

            if (count == 6 && at(4) == "folders" && at(5) == "progress" && method == "DELETE") {
                std::string folder;
                if (!validFolder(req.param("path"), folder)) return apiError(res, 400, "a valid folder path is required");
                std::vector<int> matches;
                { std::lock_guard<std::mutex> lock(m_mutex);
                  for (const FileInfo &file : torrent->rec.files) {
                      if (file.path.size() > folder.size() && file.path.compare(0, folder.size(), folder) == 0 &&
                          file.path[folder.size()] == '/') matches.push_back(file.id);
                  }
                }
                if (matches.empty()) return apiError(res, 404, "folder contains no torrent files");
                for (int id : matches) m_store.removeViewed(user.id, hash, id);
                return apiOk(res, 200, {{"status", "reset"}, {"files", matches.size()}});
            }

            if (count == 5 && at(4) == "cache" && get) {
                bool active = false;
                int pieceLength = 0, pieceCount = 0;
                lt::sha1_hash key;
                json readers = json::array();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    active = torrent->active;
                    pieceLength = torrent->pieceLength;
                    pieceCount = torrent->numPieces;
                    key = torrent->cacheKey;
                    for (const auto &window : torrent->windows) {
                        readers.push_back({{"start", window.second.first}, {"end", window.second.second},
                                           {"reader", window.first}});
                    }
                }
                const auto cache = m_cache ? m_cache->stats() : RamCache::Stats{};
                return apiOk(res, 200, {{"hash", hash}, {"active", active},
                                        {"capacity", m_cache ? m_settings.cacheSize : 0},
                                        {"filled", active && m_cache ? m_cache->bytesOf(key) : 0},
                                        {"piece_length", pieceLength}, {"piece_count", pieceCount},
                                        {"global_cache", {{"bytes", cache.bytes}, {"capacity", cache.capacity},
                                                           {"pieces", cache.pieces}, {"evicted", cache.evicted}}},
                                        {"readers", readers}});
            }

            if (count == 5 && at(4) == "peers" && get) {
                lt::torrent_handle handle;
                { std::lock_guard<std::mutex> lock(m_mutex); handle = torrent->active ? torrent->handle : lt::torrent_handle(); }
                json peers = json::array();
                if (handle.is_valid()) {
                    std::vector<lt::peer_info> values;
                    handle.get_peer_info(values);
                    for (const lt::peer_info &peer : values) {
                        peers.push_back({{"address", peer.ip.address().to_string() + ":" + std::to_string(peer.ip.port())},
                                         {"client", peer.client}, {"download_speed", peer.payload_down_speed},
                                         {"upload_speed", peer.payload_up_speed}, {"progress", peer.progress},
                                         {"seed", static_cast<bool>(peer.flags & lt::peer_info::seed)}});
                    }
                }
                return apiOk(res, 200, {{"hash", hash}, {"peers", peers}});
            }

            if (count == 7 && at(4) == "files" && at(6) == "stream" && get) {
                int index = -1;
                if (!parseFileIndex(at(5), index)) return apiError(res, 400, "file index is invalid");
                if (!ensureMetadata()) return true;
                FileInfo selected;
                if (!fileAt(index, selected)) return apiError(res, 404, "file not found");
                stream(torrent, selected.id, req, res);
                return true;
            }

            if (count == 7 && at(4) == "files") {
                int index = -1;
                if (!parseFileIndex(at(5), index)) return apiError(res, 400, "file index is invalid");
                FileInfo selected;
                if (!fileAt(index, selected)) return apiError(res, 404, "file not found");
                if (at(6) == "viewed" && method == "PUT") {
                    const json body = apiBody(req);
                    if (!body.contains("viewed") || !body["viewed"].is_boolean()) {
                        return apiError(res, 400, "viewed must be a boolean");
                    }
                    const bool viewed = body["viewed"].get<bool>();
                    ViewedRecord record = m_store.viewedFile(user.id, hash, selected.id);
                    record.viewed = viewed;
                    record.updated = static_cast<int64_t>(time(nullptr));
                    m_store.setViewed(record);
                    return apiOk(res, 200, {{"status", "updated"}, {"viewed", viewed}});
                }
                if (at(6) == "progress" && method == "PUT") {
                    const json body = apiBody(req);
                    if (!body.contains("position") || !body["position"].is_number() ||
                        (body.contains("duration") && !body["duration"].is_number())) {
                        return apiError(res, 400, "position and optional duration must be numeric");
                    }
                    const double position = body["position"].get<double>();
                    const double duration = body.value("duration", 0.0);
                    if (!std::isfinite(position) || !std::isfinite(duration) || position < 0 || duration < 0) {
                        return apiError(res, 400, "position and duration must be finite non-negative numbers");
                    }
                    setPosition(user.id, hash, selected.id, position, duration, true);
                    const ViewedRecord record = m_store.viewedFile(user.id, hash, selected.id);
                    return apiOk(res, 200, {{"id", index}, {"position", record.timecode},
                                            {"duration", record.duration}, {"viewed", record.viewed}});
                }
                if (at(6) == "progress" && method == "DELETE") {
                    m_store.removeViewed(user.id, hash, selected.id);
                    return apiOk(res, 200, {{"status", "reset"}});
                }
            }
        }

        if (count >= 2 && at(1) == "v1") {
            return apiError(res, 404, "not found");
        }
        return apiError(res, 404, "not found");
    }
}
