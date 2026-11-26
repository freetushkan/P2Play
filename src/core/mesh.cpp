#include "mesh.hpp"
#include "torrs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/kademlia/ed25519.hpp>

namespace p2p {
    using nlohmann::json;
    namespace {
        constexpr int64_t kMaxBudget = 16ll << 30;
        constexpr size_t kMaxManifest = 4u << 20;
        constexpr size_t kMaxRecords = 1000000;

        bool readFile(const std::string &path, std::string &out, size_t limit) {
            // Check before allocation, then read at most limit+1 even if the file grows.
            out.clear();
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            in.seekg(0, std::ios::end);
            auto n = in.tellg();
            if (n < 0 || static_cast<uint64_t>(n) > limit) return false;
            in.seekg(0);
            out.resize(static_cast<size_t>(n));
            if (n && !in.read(&out[0], n)) { out.clear(); return false; }
            return in.peek() == std::char_traits<char>::eof();
        }
        bool writeFile(const std::string &path, const std::string &data) {
            const std::string tmp = path + ".tmp";
            FILE *f = std::fopen(tmp.c_str(), "wb");
            if (!f) return false;
            bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            ok = std::fclose(f) == 0 && ok;
            if (!ok) { std::remove(tmp.c_str()); return false; }
            if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
            return true;
        }
        // New immutable files are rolled back until the manifest is atomically committed.
        // Failed releases/imports must not accumulate unbounded orphan payloads.
        struct FileTransaction {
            std::vector<std::string> created;
            bool committed = false;
            ~FileTransaction() { if (!committed) for (const auto &p : created) std::remove(p.c_str()); }
        };
        bool immutableFile(const std::string &path, const std::string &bytes, FileTransaction &tx) {
            std::ifstream existing(path, std::ios::binary);
            if (existing.good()) { existing.close(); std::string old; return readFile(path, old, bytes.size()) && old == bytes; }
            if (!writeFile(path, bytes)) return false;
            tx.created.push_back(path); return true;
        }
        std::string hex(const char *data, size_t n) {
            static const char *d = "0123456789abcdef";
            std::string s;
            for (size_t i = 0; i < n; ++i) {
                s += d[(static_cast<unsigned char>(data[i]) >> 4) & 15];
                s += d[static_cast<unsigned char>(data[i]) & 15];
            }
            return s;
        }
        bool hexString(const std::string &s, size_t n) {
            return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }
        bool unhex(const std::string &s, std::string &out) {
            if (!hexString(s, s.size()) || s.size() % 2) return false;
            out.clear();
            auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            for (size_t i = 0; i < s.size(); i += 2) out += static_cast<char>(v(s[i])*16+v(s[i+1]));
            return true;
        }
        std::string sha256(const std::string &s) {
            auto h = lt::hasher256(s).final();
            return std::string(h.data(), h.size());
        }
        std::string hashHex(const std::string &s) { auto h = sha256(s); return hex(h.data(), h.size()); }
        std::string lowerAscii(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }
        bool isHash(const std::string &s) { return hexString(s, 40) || hexString(s, 64); }
        bool shardName(const std::string &s) {
            return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            });
        }
        bool packName(const std::string &s) {
            return s.size() == 70 && s.substr(0, 2) == "p-" && s.substr(66) == ".bin" && hexString(s.substr(2, 64), 64);
        }
        bool segmentName(const std::string &s, int i) {
            char legacy[32]; std::snprintf(legacy, sizeof(legacy), "seg-%06d.z", i + 1);
            return s == legacy || (s.size() == 68 && s.substr(0, 2) == "s-" && s.substr(66) == ".z" && hexString(s.substr(2, 64), 64));
        }
        bool mediaType(const std::string &kind, const std::string &type) {
            return kind == "description" ? type == "text/plain; charset=utf-8" :
                   kind == "poster" && (type == "image/jpeg" || type == "image/png" || type == "image/webp");
        }
        bool validMedia(const std::string &kind, const std::string &type, const std::string &s) {
            if (!mediaType(kind, type) || s.empty()) return false;
            if (kind == "description") {
                if (s.size() > Mesh::kMaxDescriptionBytes || s.find(static_cast<char>(0)) != std::string::npos) return false;
                // Reject invalid UTF-8, overlong encodings, surrogates and out-of-range codepoints.
                for (size_t i = 0; i < s.size();) {
                    unsigned char c = s[i++]; if (c < 128) continue;
                    int n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
                    if (n < 0 || i + n > s.size()) return false;
                    uint32_t cp = c & ((1u << (6-n))-1);
                    for (int j = 0; j < n; ++j) { unsigned char b = s[i++]; if ((b & 0xc0) != 0x80) return false; cp = (cp << 6) | (b & 63); }
                    if ((n == 1 && cp < 128) || (n == 2 && cp < 2048) || (n == 3 && cp < 65536) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
                }
                return true;
            }
            if (s.size() > Mesh::kMaxPosterBytes) return false;
            return type == "image/jpeg" ? s.size() >= 3 && s.compare(0, 3, "\xff\xd8\xff", 3) == 0 :
                   type == "image/png" ? s.size() >= 8 && s.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 :
                   s.size() >= 12 && s.substr(0, 4) == "RIFF" && s.substr(8, 4) == "WEBP";
        }
        int64_t integer(const json &j, const char *key, int64_t fallback = 0) {
            auto it = j.find(key);
            if (it == j.end() || !it->is_number_integer()) return fallback;
            if (it->is_number_unsigned() && it->get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)) return -1;
            return it->get<int64_t>();
        }
        std::string stringValue(const json &j, const char *key, const std::string &fallback = "") {
            auto it = j.find(key); return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
        }
        bool validUtf8Text(const std::string &s, size_t limit) {
            if (s.size() > limit || s.find('\0') != std::string::npos) return false;
            for (size_t i = 0; i < s.size();) {
                unsigned char c = static_cast<unsigned char>(s[i++]);
                if (c < 0x80) continue;
                int n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 :
                        c >= 0xf0 && c <= 0xf4 ? 3 : -1;
                if (n < 0 || i + static_cast<size_t>(n) > s.size()) return false;
                uint32_t cp = c & ((1u << (6-n))-1);
                for (int j = 0; j < n; ++j) {
                    unsigned char b = static_cast<unsigned char>(s[i++]);
                    if ((b & 0xc0) != 0x80) return false;
                    cp = (cp << 6) | (b & 63);
                }
                if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
                    (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
                    (cp >= 0xd800 && cp <= 0xdfff)) return false;
            }
            return true;
        }
        bool parseRef(const json &j, const std::string &kind, MeshMediaRef &r) {
            if (!j.is_object()) return false;
            r.pack = stringValue(j, "pack"); r.sha256 = stringValue(j, "sha256"); r.type = stringValue(j, "type");
            r.offset = integer(j, "offset", -1); r.length = integer(j, "length", -1);
            return packName(r.pack) && hexString(r.sha256, 64) && mediaType(kind, r.type) && r.offset >= 0 &&
                   r.length > 0 && r.length <= static_cast<int64_t>(kind == "description" ? Mesh::kMaxDescriptionBytes : Mesh::kMaxPosterBytes) &&
                   r.offset <= static_cast<int64_t>(Mesh::kMaxPackBytes) - r.length;
        }
        std::string parseRecord(const json &j, MeshRecord &r, std::string &removed) {
            removed.clear(); r = MeshRecord();
            if (!j.is_object()) return "not an object";
            r.shard = stringValue(j, "shard", "default");
            if (!shardName(r.shard)) return "bad shard";
            if (j.contains("del")) {
                removed = lowerAscii(stringValue(j, "del")); return isHash(removed) ? "" : "bad del";
            }
            r.ih = lowerAscii(stringValue(j, "ih")); r.title = stringValue(j, "title");
            if (!isHash(r.ih)) return "bad ih";
            if (r.title.empty() || r.title.size() > 512) return "a title of 1 to 512 bytes is required";
            r.size = integer(j, "size"); r.date = integer(j, "date"); auto files = integer(j, "files");
            if (r.size < 0 || r.date < 0 || files < 0 || files > INT32_MAX) return "bad numeric field";
            r.files = static_cast<int>(files); r.poster = stringValue(j, "poster");
            if (r.poster.size() > 2048) return "poster URL too long";
            for (const char *key : {"tags", "trackers"}) {
                if (!j.contains(key)) continue;
                if (!j[key].is_array() || j[key].size() > 64) return "bad tags or trackers";
                for (const auto &v : j[key]) {
                    if (!v.is_string() || v.get<std::string>().size() > 512) return "bad tags or trackers";
                    (key[1] == 'a' ? r.tags : r.trackers).push_back(v.get<std::string>());
                }
            }
            if (j.contains("description_ref") && !parseRef(j["description_ref"], "description", r.descriptionRef)) return "bad description_ref";
            if (j.contains("poster_ref") && !parseRef(j["poster_ref"], "poster", r.posterRef)) return "bad poster_ref";
            return "";
        }
        json recordJson(const MeshRecord &r) {
            json j = {{"ih", r.ih}, {"title", r.title}, {"shard", r.shard}, {"date", r.date}};
            if (r.size) j["size"] = r.size;
            if (r.files) j["files"] = r.files;
            if (!r.tags.empty()) j["tags"] = r.tags;
            if (!r.trackers.empty()) j["trackers"] = r.trackers;
            if (!r.poster.empty()) j["poster"] = r.poster;
            if (!r.descriptionRef.empty()) j["description_ref"] = r.descriptionRef.json();
            if (!r.posterRef.empty()) j["poster_ref"] = r.posterRef.json();
            return j;
        }
        std::string signedPart(lt::entry e) {
            e.dict().erase("sig"); std::string out; lt::bencode(std::back_inserter(out), e); return out;
        }
        bool keys(const std::string &dir, const std::string &pkHex, lt::dht::public_key &pk, lt::dht::secret_key &sk) {
            if (!hexString(pkHex, 64)) return false;
            std::string h, seed;
            if (!readFile(dir + "/keys/" + pkHex + ".seed", h, 64) || h.size() != 64 || !unhex(h, seed)) return false;
            std::array<char, 32> a; std::copy(seed.begin(), seed.end(), a.begin());
            std::tie(pk, sk) = lt::dht::ed25519_create_keypair(a);
            return hex(pk.bytes.data(), 32) == pkHex;
        }
        std::string encodeSigned(lt::entry &e, const lt::dht::public_key &pk, const lt::dht::secret_key &sk) {
            auto sig = lt::dht::ed25519_sign(signedPart(e), pk, sk);
            e["sig"] = std::string(sig.bytes.data(), 64);
            std::string out; lt::bencode(std::back_inserter(out), e); return out;
        }
        std::string localDir(const std::string &dir, const std::string &pk) { return dir + "/mesh-local/" + pk; }
        void makeDirs(const std::string &dir, const std::string &pk) {
            mkdir((dir + "/mesh-local").c_str(), 0700); auto p = localDir(dir, pk);
            mkdir(p.c_str(), 0700); mkdir((p + "/staging").c_str(), 0700); mkdir((p + "/cache").c_str(), 0700);
        }
        int64_t fileSize(const std::string &path) {
            struct stat s{}; return stat(path.c_str(), &s) == 0 && S_ISREG(s.st_mode) ? s.st_size : -1;
        }
        void cleanDirectory(const std::string &dir, const std::set<std::string> &keep) {
            if (DIR *d = opendir(dir.c_str())) {
                while (auto *e = readdir(d)) { std::string f = e->d_name; if (f != "." && f != ".." && !keep.count(f)) std::remove((dir + "/" + f).c_str()); }
                closedir(d);
            }
        }
        MeshPolicy readPolicy(const std::string &dir, const std::string &pk) {
            MeshPolicy p; std::string raw;
            if (!readFile(localDir(dir, pk) + "/policy.json", raw, 65536)) return p;
            auto j = json::parse(raw, nullptr, false);
            if (!j.is_object() || !j.contains("shards") || !j["shards"].is_array()) return p;
            MeshPolicy x; x.shards.clear();
            for (const auto &v : j["shards"]) { if (!v.is_string()) return p; auto s = v.get<std::string>(); if (s != "*" && !shardName(s)) return p; x.shards.insert(s); }
            if (j.contains("mirror_media") && !j["mirror_media"].is_boolean()) return p;
            x.mirrorMedia = j.value("mirror_media", false);
#if defined(__SWITCH__) || defined(__PS4__) || defined(__PS5__)
            x.mirrorMedia = false; // a desktop/server option, including imported policies
#endif
            x.recordBytes = integer(j, "record_bytes", -1); x.mediaBytes = integer(j, "media_bytes", -1); x.cacheBytes = integer(j, "cache_bytes", -1);
            for (auto b : {x.recordBytes, x.mediaBytes, x.cacheBytes}) if (b < 0 || b > kMaxBudget) return p;
            return x;
        }
        // UTF-8 code point at s[i]; advances i
        uint32_t next(const std::string &s, size_t &i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80 || i + 1 >= s.size()) {
                i++;
                return c;
            }
            if ((c & 0xE0) == 0xC0) {
                uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
                i += 2;
                return cp;
            }
            i++;
            return 0xFFFFFFFF;  // longer sequences: kept as bytes by the caller
        }

        void put(std::string &out, uint32_t cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        std::vector<std::string> words(const std::string &folded) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : folded) {
                unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x80 && !std::isalnum(u)) {
                    if (!cur.empty()) {
                        out.push_back(cur);
                    }
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) {
                out.push_back(cur);
            }
            return out;
        }
    }

    std::string meshFold(const std::string &s) {
        std::string out;
        for (size_t i = 0; i < s.size();) {
            size_t start = i;
            uint32_t cp = next(s, i);
            if (cp == 0xFFFFFFFF) {
                out += s[start];
                continue;
            }
            if (cp >= 'A' && cp <= 'Z') cp += 32;
            else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;        // А-Я
            else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;        // Ѐ-Џ
            if (cp == 0x451) cp = 0x435;                            // ё -> е
            put(out, cp);
        }
        return out;
    }

    json MeshMediaRef::json() const {
        return {{"pack", pack}, {"sha256", sha256}, {"offset", offset}, {"length", length}, {"type", type}};
    }
    json MeshPolicy::json() const {
        return {{"shards", std::vector<std::string>(shards.begin(), shards.end())}, {"record_bytes", recordBytes}, {"media_bytes", mediaBytes}, {"cache_bytes", cacheBytes}, {"mirror_media", mirrorMedia}};
    }
    json MeshRecord::json() const {
        auto j = recordJson(*this); j.erase("ih"); j["hash"] = ih;
        j["lists"] = std::vector<std::string>(lists.begin(), lists.end());
        if (ih.size() == 40) {
            Torrs t; t.hash = ih; t.title = title; t.poster = poster; t.category = tags.empty() ? "" : tags.front(); t.size = size; t.trackers = trackers;
            j["torrs"] = torrsPack(t);
        }
        return j;
    }
    std::string Mesh::listDir(const std::string &pk) const { return hexString(pk, 64) ? m_dir + "/lists/" + pk : m_dir + "/lists/.invalid"; }
    MeshPolicy Mesh::policy(const std::string &pk) const { return hexString(pk, 64) ? readPolicy(m_dir, pk) : MeshPolicy(); }

    void Mesh::load(const std::string &dir, std::function<void(const std::string &)> log) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        m_dir = dir; m_log = log ? std::move(log) : [](const std::string &) {};
        mkdir((dir + "/lists").c_str(), 0777); mkdir((dir + "/keys").c_str(), 0700); mkdir((dir + "/mesh-local").c_str(), 0700);
        std::lock_guard<std::mutex> lock(m_mutex); m_lists.clear();
        if (DIR *d = opendir((dir + "/lists").c_str())) {
            while (auto *e = readdir(d)) {
                std::string pk = e->d_name; if (!hexString(pk, 64)) continue;
                Loaded l; auto err = read(listDir(pk), l);
                if (!err.empty() || l.card.pubkey != pk) { m_log("[Mesh] refused " + pk + ": " + err); continue; }
                makeDirs(dir, pk); prune(pk, l, policy(pk)); m_lists[pk] = std::move(l);
            }
            closedir(d);
        }
        rebuild();
        m_log("[Mesh] " + std::to_string(m_lists.size()) + " lists, " + std::to_string(m_index.size()) + " records");
    }

    std::string Mesh::read(const std::string &dir, Loaded &out, const std::string *manifest, const MeshPolicy *overridePolicy) const {
        std::string raw;
        if (manifest) raw = *manifest;
        else if (!readFile(dir + "/manifest.ben", raw, kMaxManifest)) return "no manifest";
        if (raw.size() > kMaxManifest) return "manifest too large";
        lt::error_code ec; auto m = lt::bdecode(raw, ec);
        if (ec || m.type() != lt::bdecode_node::dict_t || m.dict_find_int_value("v", 0) != 1) return "bad manifest";
        std::string pk(m.dict_find_string_value("pk")), sig(m.dict_find_string_value("sig"));
        if (pk.size() != 32 || sig.size() != 64) return "no key or signature";
        lt::entry e; e = m;
        if (!lt::dht::ed25519_verify(lt::dht::signature(sig.data()), signedPart(e), lt::dht::public_key(pk.data()))) return "bad signature";
        out = Loaded(); out.card.pubkey = hex(pk.data(), 32);
        out.card.name = std::string(m.dict_find_string_value("name")); out.card.desc = std::string(m.dict_find_string_value("desc"));
        out.card.seq = m.dict_find_int_value("seq", -1);
        if (out.card.seq < 0 || out.card.name.empty() || out.card.name.size() > 256 || out.card.desc.size() > 4096) return "bad list metadata";
        auto metaNode = m.dict_find("meta");
        auto metaItems = m.dict_find_list("meta");
        if (metaNode) {
            if (!metaItems || metaItems.list_size() > static_cast<int>(kMaxMetaItems)) return "bad meta-list items";
            out.card.meta = true;
            std::set<std::string> seen;
            for (int i = 0; i < metaItems.list_size(); ++i) {
                auto item = metaItems.list_at(i);
                if (item.type() != lt::bdecode_node::dict_t) return "bad meta-list item";
                auto childNode = item.dict_find("pk"), noteNode = item.dict_find("note");
                if (childNode.type() != lt::bdecode_node::string_t || noteNode.type() != lt::bdecode_node::string_t)
                    return "bad meta-list item";
                std::string child(childNode.string_value());
                std::string annotation(noteNode.string_value());
                auto enabled = item.dict_find_int_value("enabled", -1);
                if (child.size() != 32 || !validUtf8Text(annotation, kMaxMetaAnnotationBytes) ||
                    (enabled != 0 && enabled != 1)) return "bad meta-list item";
                const std::string childPk = hex(child.data(), child.size());
                if (childPk == out.card.pubkey || !seen.insert(childPk).second) return "self-reference or duplicate meta-list item";
                out.card.items.push_back({childPk, annotation, enabled == 1});
            }
        }
        lt::dht::public_key ownerPk; lt::dht::secret_key ownerSk;
        out.card.own = keys(m_dir, out.card.pubkey, ownerPk, ownerSk);
        struct stat st{}; if (stat((dir + "/manifest.ben").c_str(), &st) == 0) out.card.updated = st.st_mtime;
        auto p = overridePolicy ? *overridePolicy : policy(out.card.pubkey);
        auto segs = m.dict_find_list("segs"), packs = m.dict_find_list("packs");
        if (!segs || segs.list_size() > kMaxSegments || (packs && packs.list_size() > kMaxSegments)) return "too many or missing descriptors";
        if (m.dict_find("packs") && !packs) return "bad packs";
        if (out.card.meta && (segs.list_size() != 0 || (packs && packs.list_size() != 0)))
            return "meta-list cannot contain record segments or media packs";
        std::map<std::string, MeshFile> media;
        std::set<std::string> paths;
        for (int i = 0; i < segs.list_size(); ++i) {
            auto s = segs.list_at(i); if (s.type() != lt::bdecode_node::dict_t) return "bad segment entry";
            std::string f(s.dict_find_string_value("f")), h(s.dict_find_string_value("h"));
            std::string shard(s.dict_find_string_value("s", "default"));
            auto b = s.dict_find_int_value("b", -1), n = s.dict_find_int_value("n", -1);
            bool legacy = f.substr(0, 4) == "seg-";
            if (!segmentName(f, i) || h.size() != 32 || !shardName(shard) || n < 0 || n > static_cast<int64_t>(kMaxRecords) ||
                (!legacy && b < 0) || b > static_cast<int64_t>(kMaxSegmentBytes) || !paths.insert("records/" + f).second) return "bad segment entry";
            MeshFile d; d.path = "records/" + f; d.sha256 = hex(h.data(), 32); d.shard = shard; d.kind = "records"; d.records = n;
            auto actual = fileSize(dir + "/" + d.path); d.present = actual >= 0;
            d.signedSize = b >= 0;
            d.bytes = b < 0 ? (actual < 0 ? static_cast<int64_t>(kMaxSegmentBytes) : actual) : b;
            d.selected = p.shards.count("*") || p.shards.count(shard);
            out.files.push_back(d);
        }
        // Admission and availability are atomic per shard: never index an earlier
        // segment while a later update/tombstone in the same shard is absent.
        std::map<std::string, int64_t> shardBytes;
        for (const auto &f : out.files) if (f.selected) shardBytes[f.shard] += f.bytes;
        std::set<std::string> admitted, unavailable;
        int64_t usedRecords = 0;
        for (const auto &s : shardBytes) if (s.second <= p.recordBytes - usedRecords) { admitted.insert(s.first); usedRecords += s.second; }
        for (auto &f : out.files) {
            f.selected = admitted.count(f.shard) != 0;
            if (f.selected && !f.present) unavailable.insert(f.shard);
        }
        for (int i = 0; packs && i < packs.list_size(); ++i) {
            auto s = packs.list_at(i); if (s.type() != lt::bdecode_node::dict_t) return "bad pack entry";
            std::string f(s.dict_find_string_value("f")), h(s.dict_find_string_value("h")), k(s.dict_find_string_value("k"));
            auto b = s.dict_find_int_value("b", -1);
            if (!packName(f) || h.size() != 32 || (k != "description" && k != "poster") || b <= 0 || b > static_cast<int64_t>(kMaxPackBytes) || !paths.insert("media/" + f).second) return "bad pack entry";
            MeshFile d; d.path = "media/" + f; d.sha256 = hex(h.data(), 32); d.kind = k; d.bytes = b; d.selected = false; d.present = fileSize(dir + "/" + d.path) == b;
            media[f] = d; out.files.push_back(d);
        }
        std::map<std::string, MeshRecord> records;
        size_t inflated = 0, totalRecords = 0;
        for (const auto &d : out.files) {
            if (d.kind != "records" || !d.selected || !d.present || unavailable.count(d.shard)) continue;
            std::string z, text;
            if (!readFile(dir + "/" + d.path, z, kMaxSegmentBytes) || static_cast<int64_t>(z.size()) != d.bytes || hashHex(z) != d.sha256) return d.path + ": hash mismatch";
            if (!zlibInflate(z, text, std::min(kMaxSegmentBytes * 4, kMaxVersionTextBytes - inflated))) return d.path + ": bad zlib or version too large";
            inflated += text.size();
            size_t pos = 0; int64_t count = 0;
            while (pos < text.size()) {
                auto end = text.find('\n', pos); if (end == std::string::npos) end = text.size();
                if (end - pos > kMaxLine) return "record too long";
                auto line = text.substr(pos, end-pos); pos = end + 1;
                if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                if (++totalRecords > kMaxRecords) return "too many records";
                ++count; MeshRecord r; std::string removed;
                auto err = parseRecord(json::parse(line, nullptr, false), r, removed); if (!err.empty()) return d.path + ": " + err;
                if (r.shard != d.shard) return "record shard disagrees with descriptor";
                if (!removed.empty()) { records.erase(removed); continue; }
                for (auto item : {std::make_pair("description", &r.descriptionRef), std::make_pair("poster", &r.posterRef)}) {
                    const auto &ref = *item.second; if (ref.empty()) continue;
                    auto f = media.find(ref.pack);
                    if (f == media.end() || f->second.kind != item.first || ref.offset > f->second.bytes - ref.length) return "media reference outside signed pack";
                }
                r.lists.insert(out.card.pubkey); records[r.ih] = std::move(r);
            }
            if (count != d.records) return "segment record count mismatch";
        }
        for (auto &r : records) out.records.push_back(std::move(r.second));
        out.card.records = static_cast<int>(out.records.size());
        return "";
    }

    void Mesh::rebuild() {
        m_index.clear();
        for (const auto &l : m_lists) {
            for (const MeshRecord &r : l.second.records) {
                auto it = m_index.find(r.ih);
                if (it == m_index.end()) {
                    m_index[r.ih] = r;
                    continue;
                }
                // merged: the fields one list lacks from another, the tags of all
                MeshRecord &m = it->second;
                m.lists.insert(l.first);
                if (!m.size) m.size = r.size;
                if (!m.files) m.files = r.files;
                if (!m.date || (r.date && r.date < m.date)) m.date = r.date ? r.date : m.date;
                if (m.poster.empty()) m.poster = r.poster;
                if (m.descriptionRef.empty()) m.descriptionRef = r.descriptionRef;
                if (m.posterRef.empty()) m.posterRef = r.posterRef;
                for (const std::string &t : r.tags) {
                    if (std::find(m.tags.begin(), m.tags.end(), t) == m.tags.end()) m.tags.push_back(t);
                }
                for (const std::string &t : r.trackers) {
                    if (std::find(m.trackers.begin(), m.trackers.end(), t) == m.trackers.end()) m.trackers.push_back(t);
                }
            }
        }
    }

    std::vector<MeshList> Mesh::lists() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<MeshList> out;
        for (const auto &l : m_lists) {
            out.push_back(l.second.card);
        }
        return out;
    }

    bool Mesh::list(const std::string &pubkey, MeshList &out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_lists.find(pubkey);
        if (it == m_lists.end()) {
            return false;
        }
        out = it->second.card;
        return true;
    }

    std::vector<MeshRecord> Mesh::search(const std::string &query, const std::string &tag, size_t limit,
                                         const std::set<std::string> &lists) const {
        const std::vector<std::string> want = words(meshFold(query));
        const std::string tagFolded = meshFold(tag);
        std::vector<std::pair<int, const MeshRecord *>> hits;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &item : m_index) {
            const MeshRecord &r = item.second;
            if (!lists.empty() && std::none_of(lists.begin(), lists.end(), [&](const std::string &pk) {
                    return r.lists.count(pk) != 0;
                })) {
                continue;
            }
            if (!tagFolded.empty() && std::none_of(r.tags.begin(), r.tags.end(),
                                                   [&](const std::string &t) { return meshFold(t) == tagFolded; })) {
                continue;
            }
            const std::string title = meshFold(r.title);
            std::string tags;
            for (const std::string &t : r.tags) {
                tags += " " + meshFold(t);
            }
            int score = 0;
            bool all = true;
            for (const std::string &w : want) {
                if (title.find(w) != std::string::npos) {
                    score += 2;
                } else if (tags.find(w) != std::string::npos) {
                    score += 1;
                } else {
                    all = false;
                    break;
                }
            }
            if (all) {
                hits.push_back({score, &r});
            }
        }
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first) return a.first > b.first;
            if (a.second->lists.size() != b.second->lists.size()) return a.second->lists.size() > b.second->lists.size();
            return a.second->date > b.second->date;
        });
        std::vector<MeshRecord> out;
        for (size_t i = 0; i < hits.size() && i < limit; i++) {
            out.push_back(*hits[i].second);
        }
        return out;
    }

    bool Mesh::record(const std::string &ih, MeshRecord &out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(lowerAscii(ih));
        if (it == m_index.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    std::string Mesh::create(const std::string &name, const std::string &desc, std::string &pubkey, bool meta) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        if (name.empty() || name.size() > 256 || desc.size() > 4096) return "a name of 1 to 256 bytes is required";
        auto seed = lt::dht::ed25519_create_seed(); lt::dht::public_key pk; lt::dht::secret_key sk;
        std::tie(pk, sk) = lt::dht::ed25519_create_keypair(seed); pubkey = hex(pk.bytes.data(), 32);
        if (!writeFile(m_dir + "/keys/" + pubkey + ".seed", hex(seed.data(), seed.size()))) return "cannot write the key";
        const auto dir = listDir(pubkey); mkdir(dir.c_str(), 0777); mkdir((dir + "/records").c_str(), 0777); mkdir((dir + "/media").c_str(), 0777); makeDirs(m_dir, pubkey);
        lt::entry e(lt::entry::dictionary_t); e["v"] = 1; e["name"] = name; e["desc"] = desc;
        e["pk"] = std::string(pk.bytes.data(), 32); e["seq"] = 0; e["segs"] = lt::entry::list_type(); e["packs"] = lt::entry::list_type();
        if (meta) e["meta"] = lt::entry::list_type();
        if (!writeFile(dir + "/manifest.ben", encodeSigned(e, pk, sk))) return "cannot write the manifest";
        Loaded l; auto err = read(dir, l); if (!err.empty()) return err;
        std::lock_guard<std::mutex> lock(m_mutex); m_lists[pubkey] = std::move(l); return "";
    }

    int Mesh::addPending(const std::string &pubkey, const std::string &ndjson, std::string &error) {
        std::lock_guard<std::mutex> writer(m_writeMutex); error.clear();
        MeshList l; if (!list(pubkey, l) || !l.own) { error = "not a list of this node"; return -1; }
        if (l.meta) { error = "meta-lists cannot publish catalog records"; return -1; }
        if (ndjson.size() > kMaxSegmentBytes * 4) { error = "request too big"; return -1; }
        std::string checked; int count = 0; size_t pos = 0, lineNo = 0;
        while (pos < ndjson.size()) {
            auto end = ndjson.find('\n', pos); if (end == std::string::npos) end = ndjson.size();
            ++lineNo; if (end-pos > kMaxLine) { error = "line " + std::to_string(lineNo) + ": too long"; return -1; }
            auto line = ndjson.substr(pos, end-pos); pos = end + 1;
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            MeshRecord r; std::string removed; auto j = json::parse(line, nullptr, false);
            auto err = parseRecord(j, r, removed);
            if (!err.empty()) { error = "line " + std::to_string(lineNo) + ": " + err; return -1; }
            if (!r.date) r.date = time(nullptr);
            auto clean = removed.empty() ? recordJson(r) : json{{"del", removed}, {"shard", r.shard}, {"date", r.date}};
            auto normalized = clean.dump();
            if (normalized.size() > kMaxLine) { error = "normalized record too long"; return -1; }
            checked += normalized + "\n"; ++count;
        }
        const auto path = listDir(pubkey) + "/pending.ndjson";
        std::string previous;
        if (fileSize(path) >= 0 && !readFile(path, previous, kMaxVersionTextBytes)) { error = "pending too large"; return -1; }
        if (checked.size() > kMaxVersionTextBytes - previous.size()) { error = "pending too large"; return -1; }
        if (!writeFile(path, previous + checked)) { error = "cannot write pending records"; return -1; }
        return count;
    }
    int Mesh::pending(const std::string &pubkey) const {
        std::string text;
        if (!hexString(pubkey, 64) || !readFile(listDir(pubkey) + "/pending.ndjson", text, kMaxVersionTextBytes)) return 0;
        return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    }

    bool Mesh::hasPendingMeta(const std::string &pubkey) const {
        return hexString(pubkey, 64) && fileSize(localDir(m_dir, pubkey) + "/pending-meta.json") >= 0;
    }

    std::string Mesh::setMetaItems(const std::string &pubkey, const std::vector<MeshMetaItem> &items) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        MeshList parent;
        if (!list(pubkey, parent) || !parent.own) return "not a list of this node";
        if (!parent.meta) return "list is not a meta-list";
        if (items.size() > kMaxMetaItems) return "too many meta-list items";
        std::set<std::string> seen;
        json pending = json::array();
        for (const auto &item : items) {
            const auto childPk = lowerAscii(item.pubkey);
            if (!hexString(childPk, 64) || childPk == pubkey || !seen.insert(childPk).second)
                return "bad, self-referencing or duplicate child key";
            if (!validUtf8Text(item.annotation, kMaxMetaAnnotationBytes)) return "invalid meta-list annotation";
            MeshList child;
            if (!list(childPk, child)) return "child list must already be held by the publisher";
            if (child.meta) return "nested meta-lists are not allowed";
            pending.push_back({{"pubkey", childPk}, {"annotation", item.annotation}, {"enabled", item.enabled}});
        }
        makeDirs(m_dir, pubkey);
        const auto path = localDir(m_dir, pubkey) + "/pending-meta.json";
        return writeFile(path, pending.dump()) ? "" : "cannot write pending meta-list";
    }

    std::string Mesh::addMedia(const std::string &pubkey, const std::string &kind, const std::string &type,
                               const std::string &bytes, MeshMediaRef &ref) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        MeshList l; if (!list(pubkey, l) || !l.own) return "not a list of this node";
        if (l.meta) return "meta-lists cannot publish media packs";
        if (!validMedia(kind, type, bytes)) return "invalid or oversized media";
        makeDirs(m_dir, pubkey); auto staging = localDir(m_dir, pubkey) + "/staging";
        auto p = policy(pubkey); int64_t total = 0;
        std::string active;
        if (DIR *d = opendir(staging.c_str())) {
            while (auto *e = readdir(d)) {
                std::string name = e->d_name; if (!packName(name)) continue;
                auto n = fileSize(staging + "/" + name); if (n < 0 || n > static_cast<int64_t>(kMaxPackBytes)) { closedir(d); return "invalid staging pack"; }
                total += n;
                std::string k; if (readFile(staging + "/" + name + ".kind", k, 32) && k == kind && n + bytes.size() <= kMaxPackBytes && fileSize(listDir(pubkey) + "/media/" + name) < 0) active = name;
            }
            closedir(d);
        }
        if (total + static_cast<int64_t>(bytes.size()) > p.mediaBytes) return "staging media budget exceeded";
        if (active.empty()) {
            auto random = lt::dht::ed25519_create_seed(); active = "p-" + hex(random.data(), random.size()) + ".bin";
            if (!writeFile(staging + "/" + active + ".kind", kind)) return "cannot write media kind";
        }
        const auto path = staging + "/" + active; auto offset = std::max<int64_t>(0, fileSize(path));
        FILE *f = std::fopen(path.c_str(), "ab"); if (!f) return "cannot append media";
        bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size(); ok = std::fclose(f) == 0 && ok;
        if (!ok) return "cannot append media";
        ref.pack = active; ref.offset = offset; ref.length = bytes.size(); ref.type = type; ref.sha256 = hashHex(bytes);
        return "";
    }

    std::string Mesh::release(const std::string &pubkey, int64_t &seq) { return publish(pubkey, false, seq); }
    std::string Mesh::compact(const std::string &pubkey, int64_t &seq) { return publish(pubkey, true, seq); }

    std::string Mesh::replace(const std::string &pubkey, const std::string &ndjson,
                              int64_t &seq, int &inputRecords) {
        inputRecords = 0;
        return publish(pubkey, true, seq, &ndjson, &inputRecords);
    }

    std::string Mesh::publish(const std::string &pubkey, bool snapshot, int64_t &seq,
                              const std::string *replacement, int *inputRecords) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        lt::dht::public_key pk; lt::dht::secret_key sk; if (!keys(m_dir, pubkey, pk, sk)) return "not a list of this node";
        const auto dir = listDir(pubkey), staging = localDir(m_dir, pubkey) + "/staging";
        FileTransaction tx;
        Loaded old; auto err = read(dir, old); if (!err.empty()) return err;
        if (replacement && old.card.meta) return "meta-lists cannot publish catalog records";
        for (const auto &f : old.files) if (f.kind == "records" && (!f.selected || !f.present)) return "publishing needs all record shards";
        std::string raw; if (!readFile(dir + "/manifest.ben", raw, kMaxManifest)) return "no manifest";
        lt::error_code ec; auto m = lt::bdecode(raw, ec); lt::entry e; e = m;
        if (old.card.seq == INT64_MAX) return "sequence exhausted";
        std::string pending;
        auto pendingPath = dir + "/pending.ndjson";
        if (replacement) {
            if (replacement->size() > kMaxVersionTextBytes) return "replacement catalog is too large";
            pending = *replacement;
        } else if (fileSize(pendingPath) >= 0 && !readFile(pendingPath, pending, kMaxVersionTextBytes)) {
            return "pending too large";
        }
        const auto pendingMetaPath = localDir(m_dir, pubkey) + "/pending-meta.json";
        const bool hasPendingMeta = fileSize(pendingMetaPath) >= 0;
        std::string pendingMetaRaw;
        json pendingMeta = json::array();
        if (hasPendingMeta) {
            if (!readFile(pendingMetaPath, pendingMetaRaw, 1u << 20)) return "pending meta-list too large";
            pendingMeta = json::parse(pendingMetaRaw, nullptr, false);
            if (!pendingMeta.is_array() || pendingMeta.size() > kMaxMetaItems) return "bad pending meta-list";
            if (!old.card.meta) return "list is not a meta-list";
        }
        if (!snapshot && pending.empty() && !hasPendingMeta) return "no records or meta changes to release";
        if (snapshot && !pending.empty() && !replacement) return "release pending records before compaction";
        if (snapshot && hasPendingMeta) return "release pending meta changes before compaction";
        if (hasPendingMeta) {
            lt::entry::list_type entries;
            std::set<std::string> seen;
            for (const auto &item : pendingMeta) {
                if (!item.is_object() || !item.value("pubkey", json()).is_string() ||
                    !item.value("annotation", json()).is_string() || !item.value("enabled", json()).is_boolean())
                    return "bad pending meta-list item";
                const auto childPk = lowerAscii(item["pubkey"].get<std::string>());
                const auto annotation = item["annotation"].get<std::string>();
                if (!hexString(childPk, 64) || childPk == pubkey || !seen.insert(childPk).second ||
                    !validUtf8Text(annotation, kMaxMetaAnnotationBytes)) return "bad pending meta-list item";
                MeshList child;
                if (!list(childPk, child)) return "child list is no longer held by the publisher";
                if (child.meta) return "nested meta-lists are not allowed";
                std::string childRaw;
                if (!unhex(childPk, childRaw) || childRaw.size() != 32) return "bad child key";
                lt::entry entry(lt::entry::dictionary_t);
                entry["pk"] = childRaw; entry["note"] = annotation;
                entry["enabled"] = item["enabled"].get<bool>() ? 1 : 0;
                entries.push_back(entry);
            }
            e["meta"] = entries;
        }
        std::map<std::string, std::string> texts;
        int replacementCount = 0;
        if (snapshot && replacement) {
            std::set<std::string> seen;
            size_t pos = 0, lineNo = 0;
            while (pos < pending.size()) {
                auto end = pending.find('\n', pos);
                if (end == std::string::npos) end = pending.size();
                ++lineNo;
                if (end - pos > kMaxLine) return "replacement line " + std::to_string(lineNo) + ": too long";
                auto line = pending.substr(pos, end - pos);
                pos = end + 1;
                if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                MeshRecord r;
                std::string removed;
                err = parseRecord(json::parse(line, nullptr, false), r, removed);
                if (!err.empty()) return "replacement line " + std::to_string(lineNo) + ": " + err;
                if (!removed.empty()) return "replacement accepts live records only; omit deleted infohashes";
                if (!seen.insert(r.ih).second) return "replacement contains duplicate infohash " + r.ih;
                if (!r.date) r.date = time(nullptr);
                auto normalized = recordJson(r).dump();
                if (normalized.size() > kMaxLine) return "replacement record too long";
                texts[r.shard] += normalized + "\n";
                ++replacementCount;
            }
            e["segs"] = lt::entry::list_type();
        } else if (snapshot) {
            // Repack only live slices into fresh append-only packs. Never mutate an old
            // pack in place: record refs and pack descriptors switch in one signed commit.
            std::map<std::string, std::string> active;
            std::map<std::string, MeshMediaRef> dedup;
            std::string lastName, lastPack;
            for (auto r : old.records) {
                for (auto item : {std::make_pair("description", &r.descriptionRef), std::make_pair("poster", &r.posterRef)}) {
                    auto &ref = *item.second; if (ref.empty()) continue;
                    const std::string key = std::string(item.first) + "\n" + ref.sha256 + "\n" + ref.type;
                    auto reused = dedup.find(key);
                    if (reused != dedup.end()) { ref = reused->second; continue; }
                    if (lastName != ref.pack) {
                        auto f = std::find_if(old.files.begin(), old.files.end(), [&](const MeshFile &f) { return f.path == "media/" + ref.pack; });
                        if (f == old.files.end() || !readFile(dir + "/media/" + ref.pack, lastPack, kMaxPackBytes) || static_cast<int64_t>(lastPack.size()) != f->bytes || hashHex(lastPack) != f->sha256) return "snapshot needs verified media packs";
                        lastName = ref.pack;
                    }
                    auto slice = lastPack.substr(static_cast<size_t>(ref.offset), static_cast<size_t>(ref.length));
                    if (hashHex(slice) != ref.sha256 || !validMedia(item.first, ref.type, slice)) return "snapshot media hash mismatch";
                    auto &name = active[item.first]; auto n = name.empty() ? -1 : fileSize(staging + "/" + name);
                    if (n < 0 || n + ref.length > static_cast<int64_t>(kMaxPackBytes)) {
                        auto random = lt::dht::ed25519_create_seed(); name = "p-" + hex(random.data(), random.size()) + ".bin"; n = 0;
                        if (!immutableFile(staging + "/" + name + ".kind", item.first, tx) || !immutableFile(staging + "/" + name, "", tx)) return "cannot stage snapshot pack";
                    }
                    FILE *f = std::fopen((staging + "/" + name).c_str(), "ab"); if (!f) return "cannot append snapshot pack";
                    bool ok = std::fwrite(slice.data(), 1, slice.size(), f) == slice.size(); ok = std::fclose(f) == 0 && ok;
                    if (!ok) return "cannot append snapshot pack";
                    ref.pack = name; ref.offset = n; dedup[key] = ref;
                }
                texts[r.shard] += recordJson(r).dump() + "\n";
            }
            e["segs"] = lt::entry::list_type();
        } else {
            size_t pos = 0;
            while (pos < pending.size()) {
                auto end = pending.find('\n', pos); if (end == std::string::npos) end = pending.size();
                if (end-pos > kMaxLine) return "pending record too long";
                auto line = pending.substr(pos, end-pos); pos = end+1;
                MeshRecord r; std::string del; err = parseRecord(json::parse(line, nullptr, false), r, del); if (!err.empty()) return err;
                texts[r.shard] += line + "\n";
            }
        }
        // Retain only packs referenced by live snapshot records, or every historical pack
        // for an ordinary append. Existing published packs are never appended or rewritten.
        std::set<std::string> referenced;
        std::map<std::string, std::string> packKinds;
        for (const auto &t : texts) {
            std::istringstream lines(t.second); std::string line;
            while (std::getline(lines, line)) {
                MeshRecord r; std::string del; err = parseRecord(json::parse(line, nullptr, false), r, del); if (!err.empty()) return err;
                for (auto item : {std::make_pair("description", &r.descriptionRef), std::make_pair("poster", &r.posterRef)}) {
                    const auto &ref = *item.second; if (ref.empty()) continue;
                    if (packKinds.count(ref.pack) && packKinds[ref.pack] != item.first) return "mixed media pack kinds";
                    referenced.insert(ref.pack); packKinds[ref.pack] = item.first;
                    std::string pack;
                    auto source = fileSize(dir + "/media/" + ref.pack) >= 0 ? dir + "/media/" + ref.pack : staging + "/" + ref.pack;
                    if (!readFile(source, pack, kMaxPackBytes) || ref.offset > static_cast<int64_t>(pack.size())-ref.length) return "missing media referenced by record";
                    auto slice = pack.substr(static_cast<size_t>(ref.offset), static_cast<size_t>(ref.length));
                    if (hashHex(slice) != ref.sha256 || !validMedia(item.first, ref.type, slice)) return "invalid media referenced by record";
                }
            }
        }
        lt::entry::list_type packEntries;
        if (auto oldPacks = e.find_key("packs")) {
            for (const auto &s : oldPacks->list()) {
                const auto &f = s.dict().at("f").string();
                if (!snapshot || referenced.count(f)) packEntries.push_back(s);
            }
        }
        std::set<std::string> known;
        for (const auto &s : packEntries) known.insert(s.dict().at("f").string());
        mkdir((dir + "/media").c_str(), 0777);
        for (const auto &f : referenced) {
            if (known.count(f)) continue;
            std::string bytes, kind;
            if (!readFile(staging + "/" + f, bytes, kMaxPackBytes) || !readFile(staging + "/" + f + ".kind", kind, 32) || kind != packKinds[f]) return "missing staging pack";
            if (!immutableFile(dir + "/media/" + f, bytes, tx)) return "cannot write immutable media pack";
            lt::entry s(lt::entry::dictionary_t); s["f"] = f; s["b"] = static_cast<int64_t>(bytes.size()); s["h"] = sha256(bytes); s["k"] = kind;
            packEntries.push_back(s);
        }
        if (packEntries.size() > kMaxSegments) return "too many media packs: compact first";
        e["packs"] = packEntries;
        auto &segs = e["segs"].list();
        for (const auto &t : texts) {
            // Split on line boundaries; each independently hashed shard remains bounded.
            size_t begin = 0;
            while (begin < t.second.size()) {
                size_t end = std::min(begin + (8u << 20), t.second.size());
                if (end < t.second.size()) { end = t.second.rfind('\n', end); if (end == std::string::npos || end < begin) return "record too large"; ++end; }
                auto text = t.second.substr(begin, end-begin); begin = end;
                auto z = zlibDeflate(text); if (z.empty() || z.size() > kMaxSegmentBytes) return "segment too big";
                // Version-scoped immutable identity allows identical payloads to be
                // appended again without overwriting any previous torrent's files.
                auto f = "s-" + hashHex(pubkey + "\n" + std::to_string(old.card.seq+1) + "\n" + t.first + "\n" + std::to_string(segs.size()) + "\n" + z) + ".z";
                if (!snapshot && !e.find_key("snapshot")) { char name[32]; std::snprintf(name, sizeof(name), "seg-%06d.z", static_cast<int>(segs.size())+1); f = name; }
                if (segs.size() >= kMaxSegments) return "too many segments: a snapshot is needed";
                if (!immutableFile(dir + "/records/" + f, z, tx)) return "cannot write immutable segment";
                lt::entry s(lt::entry::dictionary_t); s["f"] = f; s["h"] = sha256(z); s["b"] = static_cast<int64_t>(z.size()); s["s"] = t.first;
                s["n"] = static_cast<int64_t>(std::count(text.begin(), text.end(), '\n')); segs.push_back(s);
            }
        }
        e["seq"] = old.card.seq + 1;
        if (snapshot) e["snapshot"] = old.card.seq;
        auto candidate = encodeSigned(e, pk, sk);
        Loaded next; err = read(dir, next, &candidate); if (!err.empty()) return err;
        for (const auto &f : next.files) if (f.kind == "records" && !f.selected) return "record disk budget exceeded";
        auto p = policy(pubkey); int64_t mediaBytes = 0;
        for (const auto &f : next.files) if (f.kind != "records") mediaBytes += f.bytes;
        if (mediaBytes > p.mediaBytes) return "media disk budget exceeded";
        if (!writeFile(dir + "/manifest.ben", candidate)) return "cannot write manifest";
        tx.committed = true;
        seq = next.card.seq; next.card.updated = time(nullptr);
        if (inputRecords) *inputRecords = replacementCount;
        std::remove(pendingPath.c_str());
        if (hasPendingMeta) std::remove(pendingMetaPath.c_str());
        cleanDirectory(staging, {});
        if (snapshot) prune(pubkey, next, p);
        { std::lock_guard<std::mutex> lock(m_mutex); m_lists[pubkey] = std::move(next); rebuild(); }
        m_log("[Mesh] " + pubkey + " version " + std::to_string(seq) + (snapshot ? " snapshot" : " released"));
        return "";
    }

    std::vector<MeshFile> Mesh::plan(const std::string &pubkey) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_lists.find(pubkey); if (it == m_lists.end()) return {};
        auto files = it->second.files;
        for (auto &f : files) f.present = fileSize(listDir(pubkey) + "/" + f.path) == f.bytes;
        return files;
    }

    std::string Mesh::accept(const std::string &dir, const std::string &expectedPk, int64_t expectedSeq, bool rejectMetaChild) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        Loaded l; auto err = read(dir, l); if (!err.empty()) return err;
        if ((!expectedPk.empty() && l.card.pubkey != expectedPk) || (expectedSeq >= 0 && l.card.seq != expectedSeq)) return "pointer/manifest key or version mismatch";
        if (rejectMetaChild && l.card.meta) return "nested meta-lists are not allowed";
        for (const auto &item : l.card.items) {
            MeshList child;
            if (list(item.pubkey, child) && child.meta) return "meta-list cannot include another meta-list";
        }
        MeshList old; if (list(l.card.pubkey, old) && old.seq >= l.card.seq) return "not newer than the version held (" + std::to_string(old.seq) + ")";
        const auto pk = l.card.pubkey, to = listDir(pk); auto p = policy(pk);
        FileTransaction tx;
        std::string manifest; if (!readFile(dir + "/manifest.ben", manifest, kMaxManifest)) return "no manifest";
        makeDirs(m_dir, pk); mkdir(to.c_str(), 0777); mkdir((to + "/records").c_str(), 0777); mkdir((to + "/media").c_str(), 0777);
        int64_t mediaBytes = 0;
        for (const auto &f : l.files) {
            if (f.kind == "records" && !f.selected) continue;
            if (f.kind != "records") {
                if (!f.present || f.bytes > p.mediaBytes-mediaBytes) continue;
                mediaBytes += f.bytes;
            }
            if (dir == to || fileSize(dir + "/" + f.path) < 0) continue; // descriptors-first imports allowed
            std::string bytes;
            if (!readFile(dir + "/" + f.path, bytes, f.kind == "records" ? kMaxSegmentBytes : kMaxPackBytes) ||
                static_cast<int64_t>(bytes.size()) != f.bytes || hashHex(bytes) != f.sha256) return f.path + ": hash mismatch";
            // A different file at a legacy path must not destroy the last committed version.
            if (fileSize(to + "/" + f.path) >= 0) {
                std::string existing;
                if (!readFile(to + "/" + f.path, existing, f.kind == "records" ? kMaxSegmentBytes : kMaxPackBytes) || existing != bytes) return "immutable filename collision";
            } else if (!immutableFile(to + "/" + f.path, bytes, tx)) return "cannot copy version file";
        }
        Loaded next; err = read(to, next, &manifest); if (!err.empty()) return err;
        if (!writeFile(to + "/manifest.ben", manifest)) return "cannot copy manifest";
        tx.committed = true;
        prune(pk, next, p);
        { std::lock_guard<std::mutex> lock(m_mutex); m_lists[pk] = std::move(next); rebuild(); }
        return "";
    }

    void Mesh::prune(const std::string &pk, const Loaded &version, const MeshPolicy &p) {
        std::set<std::string> records, media;
        int64_t used = 0;
        for (const auto &f : version.files) {
            if (f.kind == "records") { if (f.selected) records.insert(f.path.substr(8)); }
            else if (fileSize(listDir(pk) + "/" + f.path) == f.bytes && f.bytes <= p.mediaBytes-used) { used += f.bytes; media.insert(f.path.substr(6)); }
        }
        cleanDirectory(listDir(pk) + "/records", records); cleanDirectory(listDir(pk) + "/media", media);
        // Cache uses content-addressed files and is always verified before a cache hit.
        const auto cache = localDir(m_dir, pk) + "/cache";
        struct Cached { std::string path; int64_t size, time; }; std::vector<Cached> cached; int64_t total = 0;
        if (DIR *d = opendir(cache.c_str())) {
            while (auto *e = readdir(d)) {
                auto path = cache + "/" + e->d_name; struct stat s{};
                if (stat(path.c_str(), &s) != 0 || !S_ISREG(s.st_mode)) continue;
                if (!hexString(e->d_name, 64) || s.st_size > static_cast<int64_t>(kMaxPosterBytes)) { std::remove(path.c_str()); continue; }
                cached.push_back({path, s.st_size, s.st_mtime}); total += s.st_size;
            }
            closedir(d);
        }
        std::sort(cached.begin(), cached.end(), [](const Cached &a, const Cached &b) { return a.time < b.time; });
        for (const auto &f : cached) { if (total <= p.cacheBytes) break; if (std::remove(f.path.c_str()) == 0) total -= f.size; }
    }

    std::string Mesh::setPolicy(const std::string &pk, const MeshPolicy &p) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        if (!hexString(pk, 64)) return "bad list key";
#if defined(__SWITCH__) || defined(__PS4__) || defined(__PS5__)
        if (p.mirrorMedia) return "media mirroring is not available on consoles";
#endif
        MeshList l; const bool have = list(pk, l);
        if (p.shards.size() > kMaxSegments) return "too many shards";
        for (const auto &s : p.shards) if (s != "*" && !shardName(s)) return "bad shard";
        for (auto b : {p.recordBytes, p.mediaBytes, p.cacheBytes}) if (b < 0 || b > kMaxBudget) return "bad disk budget";
        Loaded next; auto err = have ? read(listDir(pk), next, nullptr, &p) : std::string(); if (!err.empty()) return err;
        if (l.own) {
            int64_t mediaBytes = 0;
            for (const auto &f : next.files) { if (f.kind == "records" && !f.selected) return "owner must retain all shards"; if (f.kind != "records") mediaBytes += f.bytes; }
            if (mediaBytes > p.mediaBytes) return "owner must retain all media packs";
        }
        makeDirs(m_dir, pk);
        if (!writeFile(localDir(m_dir, pk) + "/policy.json", p.json().dump())) return "cannot write policy";
        if (have) {
            prune(pk, next, p);
            std::lock_guard<std::mutex> lock(m_mutex); m_lists[pk] = std::move(next); rebuild();
        }
        return "";
    }

    std::string Mesh::installFile(const std::string &pk, const std::string &path, const std::string &source) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        auto files = plan(pk); auto f = std::find_if(files.begin(), files.end(), [&](const MeshFile &f) { return f.path == path; });
        if (f == files.end()) return "not a signed version file";
        if (f->kind == "records" && !f->selected) return "shard not selected or record budget exceeded";
        auto p = policy(pk);
        if (f->kind != "records") {
            int64_t used = 0; for (const auto &d : files) if (d.kind != "records" && d.path != path && d.present) used += d.bytes;
            if (f->bytes > p.mediaBytes-used) return "media disk budget exceeded";
        }
        std::string bytes;
        if (!readFile(source, bytes, f->kind == "records" ? kMaxSegmentBytes : kMaxPackBytes) ||
            (f->signedSize && static_cast<int64_t>(bytes.size()) != f->bytes) || hashHex(bytes) != f->sha256) return "file hash mismatch";
        const auto dest = listDir(pk) + "/" + path;
        // Keep the prior bytes for rollback if a signed segment is semantically invalid.
        std::string previous; bool had = readFile(dest, previous, f->kind == "records" ? kMaxSegmentBytes : kMaxPackBytes);
        if (!writeFile(dest, bytes)) return "cannot install file";
        Loaded next; auto err = read(listDir(pk), next);
        if (!err.empty()) { if (had) writeFile(dest, previous); else std::remove(dest.c_str()); return err; }
        { std::lock_guard<std::mutex> lock(m_mutex); m_lists[pk] = std::move(next); rebuild(); }
        return "";
    }

    std::string Mesh::mediaPath(const std::string &pk, const std::string &ih, const std::string &kind) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (kind != "description" && kind != "poster") return {};
        auto l = m_lists.find(pk); if (l == m_lists.end()) return {};
        for (const auto &r : l->second.records) if (r.ih == lowerAscii(ih)) {
            const auto &ref = kind == "description" ? r.descriptionRef : r.posterRef;
            return ref.empty() ? std::string() : "media/" + ref.pack;
        }
        return {};
    }

    std::string Mesh::media(const std::string &pk, const std::string &ih, const std::string &kind, std::string &bytes, std::string &type) {
        std::lock_guard<std::mutex> writer(m_writeMutex); bytes.clear(); type.clear();
        MeshMediaRef ref; Loaded version;
        {
            std::lock_guard<std::mutex> lock(m_mutex); auto l = m_lists.find(pk); if (l == m_lists.end()) return "list not found";
            auto r = std::find_if(l->second.records.begin(), l->second.records.end(), [&](const MeshRecord &r) { return r.ih == lowerAscii(ih); });
            if (r == l->second.records.end()) return "record not found";
            if (kind == "description") ref = r->descriptionRef; else if (kind == "poster") ref = r->posterRef; else return "bad media kind";
            if (ref.empty()) return "media not found";
            version.card = l->second.card; version.files = l->second.files;
        }
        auto p = policy(pk); makeDirs(m_dir, pk); const auto cache = localDir(m_dir, pk) + "/cache/" + ref.sha256;
        if (readFile(cache, bytes, static_cast<size_t>(ref.length)) && static_cast<int64_t>(bytes.size()) == ref.length && hashHex(bytes) == ref.sha256 && validMedia(kind, ref.type, bytes)) {
            type = ref.type; return "";
        }
        std::remove(cache.c_str()); bytes.clear();
        auto f = std::find_if(version.files.begin(), version.files.end(), [&](const MeshFile &f) { return f.path == "media/" + ref.pack; });
        if (f == version.files.end()) return "unsigned media descriptor";
        std::string pack;
        if (!readFile(listDir(pk) + "/" + f->path, pack, kMaxPackBytes)) return "media unavailable";
        if (static_cast<int64_t>(pack.size()) != f->bytes || hashHex(pack) != f->sha256) return "media pack hash mismatch";
        if (ref.offset > static_cast<int64_t>(pack.size()) - ref.length) return "media bounds mismatch";
        bytes = pack.substr(static_cast<size_t>(ref.offset), static_cast<size_t>(ref.length));
        if (hashHex(bytes) != ref.sha256 || !validMedia(kind, ref.type, bytes)) { bytes.clear(); return "media hash or type mismatch"; }
        type = ref.type;
        if (ref.length <= p.cacheBytes) { writeFile(cache, bytes); prune(pk, version, p); }
        return "";
    }

    std::string Mesh::remove(const std::string &pk) {
        std::lock_guard<std::mutex> writer(m_writeMutex);
        MeshList l; if (!list(pk, l)) return "list not found"; if (l.own) return "a list of this node: its key would be lost";
        cleanDirectory(listDir(pk) + "/records", {}); cleanDirectory(listDir(pk) + "/media", {});
        std::remove((listDir(pk) + "/manifest.ben").c_str()); std::remove((listDir(pk) + "/pointer.ben").c_str());
        rmdir((listDir(pk) + "/records").c_str()); rmdir((listDir(pk) + "/media").c_str()); rmdir(listDir(pk).c_str());
        auto local = localDir(m_dir, pk); cleanDirectory(local + "/staging", {}); cleanDirectory(local + "/cache", {});
        rmdir((local + "/staging").c_str()); rmdir((local + "/cache").c_str());
        std::remove((local + "/policy.json").c_str()); std::remove((local + "/pending-meta.json").c_str());
        rmdir(local.c_str());
        std::lock_guard<std::mutex> lock(m_mutex); m_lists.erase(pk); rebuild(); return "";
    }
}
