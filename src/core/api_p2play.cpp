// P2Play's native, versioned library API. This file is deliberately independent of
// the optional TorrServer MatriX compatibility layer (P2P_COMPAT_MATRIX).
#include "core_torrent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace p2p {
    using nlohmann::json;

    namespace {
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
            if (hasFile && torrentFile.size() > (128u * 1024u * 1024u)) {
                return apiError(res, 413, "torrent file exceeds the 128 MiB upload limit");
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

        // GET/PUT/DELETE /api/v1/torrents/{infohash}
        if (count == 4 && at(2) == "torrents") {
            const std::string hash = meshFold(at(3));
            TorrentPtr torrent = find(hash);
            if (!torrent || !owns(torrent, user.id)) {
                return apiError(res, 404, "torrent not found in your library");
            }
            if (get) return apiOk(res, 200, {{"torrent", item(torrent)}});

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

        if (count >= 2 && at(1) == "v1") {
            return apiError(res, 404, "not found");
        }
        return apiError(res, 404, "not found");
    }
}
