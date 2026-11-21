#include "mesh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <tuple>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/kademlia/ed25519.hpp>

#include "torrs.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        bool readFile(const std::string &path, std::string &out, size_t limit) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return false;
            }
            std::stringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return out.size() <= limit;
        }

        bool writeFile(const std::string &path, const std::string &data) {
            const std::string tmp = path + ".tmp";
            FILE *f = std::fopen(tmp.c_str(), "wb");
            if (!f) {
                return false;
            }
            bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            ok = std::fclose(f) == 0 && ok;
            if (!ok) {
                return false;
            }
            if (std::rename(tmp.c_str(), path.c_str()) != 0) {
                std::remove(path.c_str());
                return std::rename(tmp.c_str(), path.c_str()) == 0;
            }
            return true;
        }

        std::string hex(const char *data, size_t size) {
            static const char *digits = "0123456789abcdef";
            std::string out;
            for (size_t i = 0; i < size; i++) {
                out += digits[(static_cast<unsigned char>(data[i]) >> 4) & 15];
                out += digits[static_cast<unsigned char>(data[i]) & 15];
            }
            return out;
        }

        bool unhex(const std::string &h, std::string &out) {
            if (h.size() % 2) {
                return false;
            }
            out.clear();
            for (size_t i = 0; i < h.size(); i += 2) {
                auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
                int a = v(h[i]), b = v(h[i + 1]);
                if (a < 0 || b < 0) {
                    return false;
                }
                out += static_cast<char>(a * 16 + b);
            }
            return true;
        }

        bool isHash(const std::string &s) {
            return (s.size() == 40 || s.size() == 64) &&
                   std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        std::string sha256(const std::string &data) {
            lt::sha256_hash h = lt::hasher256(data).final();
            return std::string(h.data(), h.size());
        }

        std::string lowerAscii(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        // a record line checked; "" if fine
        std::string parseRecord(const json &j, MeshRecord &r, std::string &removed) {
            removed.clear();
            if (!j.is_object()) {
                return "not an object";
            }
            if (j.contains("del")) {
                removed = j["del"].is_string() ? lowerAscii(j["del"].get<std::string>()) : "";
                return isHash(removed) ? "" : "bad del";
            }
            r = MeshRecord();
            r.ih = j.value("ih", json()).is_string() ? lowerAscii(j["ih"].get<std::string>()) : "";
            r.title = j.value("title", json()).is_string() ? j["title"].get<std::string>() : "";
            if (!isHash(r.ih)) {
                return "bad ih";
            }
            if (r.title.empty() || r.title.size() > 512) {
                return "a title of 1 to 512 bytes is required";
            }
            r.size = j.value("size", json()).is_number_integer() ? j["size"].get<int64_t>() : 0;
            r.files = j.value("files", json()).is_number_integer() ? j["files"].get<int>() : 0;
            r.date = j.value("date", json()).is_number_integer() ? j["date"].get<int64_t>() : 0;
            r.poster = j.value("poster", json()).is_string() ? j["poster"].get<std::string>().substr(0, 2048) : "";
            for (const char *key : {"tags", "trackers"}) {
                if (j.contains(key) && j[key].is_array()) {
                    for (const auto &v : j[key]) {
                        if (v.is_string() && !v.get<std::string>().empty()) {
                            (key[0] == 't' && key[1] == 'a' ? r.tags : r.trackers).push_back(v.get<std::string>().substr(0, 512));
                        }
                    }
                }
            }
            return "";
        }

        // the bytes a manifest's signature covers: the dict bencoded without "sig"
        std::string signedPart(lt::entry e) {
            e.dict().erase("sig");
            std::string out;
            lt::bencode(std::back_inserter(out), e);
            return out;
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

    json MeshRecord::json() const {
        nlohmann::json j = {{"hash", ih}, {"title", title}, {"lists", std::vector<std::string>(lists.begin(), lists.end())}};
        if (size) j["size"] = size;
        if (files) j["files"] = files;
        if (date) j["date"] = date;
        if (!tags.empty()) j["tags"] = tags;
        if (!trackers.empty()) j["trackers"] = trackers;
        if (!poster.empty()) j["poster"] = poster;
        if (ih.size() == 40) {
            Torrs t;
            t.hash = ih;
            t.title = title;
            t.poster = poster;
            t.category = tags.empty() ? "" : tags.front();
            t.size = size;
            t.trackers = trackers;
            j["torrs"] = torrsPack(t);
        }
        return j;
    }

    void Mesh::load(const std::string &dataDir, std::function<void(const std::string &)> log) {
        m_log = std::move(log);
        m_dir = dataDir;
        mkdir((m_dir + "/lists").c_str(), 0777);
        mkdir((m_dir + "/keys").c_str(), 0700);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lists.clear();
        if (DIR *d = opendir((m_dir + "/lists").c_str())) {
            while (dirent *e = readdir(d)) {
                std::string name = e->d_name;
                if (name.size() != 64) {
                    continue;
                }
                Loaded l;
                std::string err = read(listDir(name), l);
                if (!err.empty() || l.card.pubkey != name) {
                    m_log("[Mesh] list " + name.substr(0, 16) + " refused: " + (err.empty() ? "another key" : err));
                    continue;
                }
                m_lists[name] = std::move(l);
            }
            closedir(d);
        }
        rebuild();
        m_log("[Mesh] " + std::to_string(m_lists.size()) + " lists, " + std::to_string(m_index.size()) + " records");
    }

    std::string Mesh::listDir(const std::string &pubkey) const {
        return m_dir + "/lists/" + pubkey;
    }

    std::string Mesh::read(const std::string &dir, Loaded &out) const {
        std::string raw;
        if (!readFile(dir + "/manifest.ben", raw, 1 << 20)) {
            return "no manifest";
        }
        lt::error_code ec;
        lt::bdecode_node m = lt::bdecode(raw, ec);
        if (ec || m.type() != lt::bdecode_node::dict_t || m.dict_find_int_value("v", 0) != 1) {
            return "bad manifest";
        }
        const std::string pk(m.dict_find_string_value("pk"));
        const std::string sig(m.dict_find_string_value("sig"));
        if (pk.size() != 32 || sig.size() != 64) {
            return "no key or signature";
        }
        lt::entry e;
        e = m;
        const std::string part = signedPart(e);
        if (!lt::dht::ed25519_verify(lt::dht::signature(sig.data()), part, lt::dht::public_key(pk.data()))) {
            return "bad signature";
        }
        out = Loaded();
        out.card.pubkey = hex(pk.data(), pk.size());
        out.card.name = std::string(m.dict_find_string_value("name")).substr(0, 256);
        out.card.desc = std::string(m.dict_find_string_value("desc")).substr(0, 4096);
        out.card.seq = m.dict_find_int_value("seq", -1);
        out.card.own = std::ifstream(m_dir + "/keys/" + out.card.pubkey + ".seed").good();
        struct stat st{};
        out.card.updated = stat((dir + "/manifest.ben").c_str(), &st) == 0 ? static_cast<int64_t>(st.st_mtime) : 0;
        if (out.card.seq < 0) {
            return "no seq";
        }
        lt::bdecode_node segs = m.dict_find_list("segs");
        if (segs && segs.list_size() > kMaxSegments) {
            return "too many segments";
        }
        std::map<std::string, MeshRecord> records;
        std::vector<std::string> order;
        for (int i = 0; segs && i < segs.list_size(); i++) {
            lt::bdecode_node s = segs.list_at(i);
            const std::string file(s.dict_find_string_value("f"));
            const std::string hash(s.dict_find_string_value("h"));
            char expect[32];
            std::snprintf(expect, sizeof(expect), "seg-%06d.z", i + 1);
            if (file != expect || hash.size() != 32) {
                return "bad segment entry";
            }
            std::string z, text;
            if (!readFile(dir + "/records/" + file, z, kMaxSegmentBytes)) {
                return "missing " + file;
            }
            if (sha256(z) != hash) {
                return file + ": hash mismatch";
            }
            if (!zlibInflate(z, text, kMaxSegmentBytes * 4)) {
                return file + ": bad zlib";
            }
            size_t pos = 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) {
                    end = text.size();
                }
                if (end - pos > kMaxLine) {
                    return file + ": a record too long";
                }
                const std::string line = text.substr(pos, end - pos);
                pos = end + 1;
                if (line.find_first_not_of(" \t\r") == std::string::npos) {
                    continue;
                }
                MeshRecord r;
                std::string removed;
                const std::string err = parseRecord(json::parse(line, nullptr, false), r, removed);
                if (!err.empty()) {
                    return file + ": " + err;
                }
                if (!removed.empty()) {
                    records.erase(removed);
                    continue;
                }
                if (!records.count(r.ih)) {
                    order.push_back(r.ih);
                }
                records[r.ih] = std::move(r);
            }
        }
        for (const std::string &ih : order) {
            auto it = records.find(ih);
            if (it != records.end()) {
                it->second.lists.insert(out.card.pubkey);
                out.records.push_back(std::move(it->second));
                records.erase(it);
            }
        }
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

    std::vector<MeshRecord> Mesh::search(const std::string &query, const std::string &tag, size_t limit) const {
        const std::vector<std::string> want = words(meshFold(query));
        const std::string tagFolded = meshFold(tag);
        std::vector<std::pair<int, const MeshRecord *>> hits;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &item : m_index) {
            const MeshRecord &r = item.second;
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

    std::string Mesh::create(const std::string &name, const std::string &desc, std::string &pubkey) {
        if (name.empty() || name.size() > 256 || desc.size() > 4096) {
            return "a name of 1 to 256 bytes is required";
        }
        std::array<char, 32> seed = lt::dht::ed25519_create_seed();
        lt::dht::public_key pk;
        lt::dht::secret_key sk;
        std::tie(pk, sk) = lt::dht::ed25519_create_keypair(seed);
        pubkey = hex(pk.bytes.data(), pk.bytes.size());
        if (!writeFile(m_dir + "/keys/" + pubkey + ".seed", hex(seed.data(), seed.size()))) {
            return "cannot write the key";
        }
        mkdir(listDir(pubkey).c_str(), 0777);
        mkdir((listDir(pubkey) + "/records").c_str(), 0777);
        lt::entry e(lt::entry::dictionary_t);
        e["v"] = 1;
        e["name"] = name;
        e["desc"] = desc;
        e["pk"] = std::string(pk.bytes.data(), pk.bytes.size());
        e["seq"] = 0;
        e["segs"] = lt::entry::list_type();
        lt::dht::signature sig = lt::dht::ed25519_sign(signedPart(e), pk, sk);
        e["sig"] = std::string(sig.bytes.data(), sig.bytes.size());
        std::string out;
        lt::bencode(std::back_inserter(out), e);
        if (!writeFile(listDir(pubkey) + "/manifest.ben", out)) {
            return "cannot write the manifest";
        }
        Loaded l;
        std::string err = read(listDir(pubkey), l);
        if (!err.empty()) {
            return err;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lists[pubkey] = std::move(l);
        m_log("[Mesh] list created: " + pubkey + " (" + name + ")");
        return "";
    }

    int Mesh::addPending(const std::string &pubkey, const std::string &ndjson, std::string &error) {
        MeshList card;
        if (!list(pubkey, card) || !card.own) {
            error = "not a list of this node";
            return -1;
        }
        std::string checked;
        int count = 0;
        size_t pos = 0, lineNo = 0;
        while (pos < ndjson.size()) {
            size_t end = ndjson.find('\n', pos);
            if (end == std::string::npos) {
                end = ndjson.size();
            }
            std::string line = ndjson.substr(pos, end - pos);
            pos = end + 1;
            lineNo++;
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                continue;
            }
            json j = json::parse(line, nullptr, false);
            MeshRecord r;
            std::string removed;
            std::string err = line.size() > kMaxLine ? "too long" : parseRecord(j, r, removed);
            if (!err.empty()) {
                error = "line " + std::to_string(lineNo) + ": " + err;
                return -1;
            }
            // normalized: lower case hashes, nothing unknown
            json clean = removed.empty() ? json::object() : json{{"del", removed}};
            if (removed.empty()) {
                clean = {{"ih", r.ih}, {"title", r.title}};
                if (r.size) clean["size"] = r.size;
                if (r.files) clean["files"] = r.files;
                clean["date"] = r.date ? r.date : static_cast<int64_t>(time(nullptr));
                if (!r.tags.empty()) clean["tags"] = r.tags;
                if (!r.trackers.empty()) clean["trackers"] = r.trackers;
                if (!r.poster.empty()) clean["poster"] = r.poster;
            } else {
                clean["date"] = j.value("date", json()).is_number_integer() ? j["date"].get<int64_t>()
                                                                             : static_cast<int64_t>(time(nullptr));
            }
            checked += clean.dump() + "\n";
            count++;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        FILE *f = std::fopen((listDir(pubkey) + "/pending.ndjson").c_str(), "ab");
        if (!f) {
            error = "cannot write";
            return -1;
        }
        std::fwrite(checked.data(), 1, checked.size(), f);
        std::fclose(f);
        return count;
    }

    int Mesh::pending(const std::string &pubkey) const {
        std::string text;
        if (!readFile(listDir(pubkey) + "/pending.ndjson", text, kMaxSegmentBytes * 4)) {
            return 0;
        }
        return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    }

    std::string Mesh::release(const std::string &pubkey, int64_t &seq) {
        std::string seedHex, seed, pending, raw;
        if (!readFile(m_dir + "/keys/" + pubkey + ".seed", seedHex, 256) || !unhex(seedHex.substr(0, 64), seed) ||
            seed.size() != 32) {
            return "not a list of this node";
        }
        const std::string dir = listDir(pubkey);
        if (!readFile(dir + "/pending.ndjson", pending, kMaxSegmentBytes * 4) || pending.empty()) {
            return "no records to release";
        }
        if (!readFile(dir + "/manifest.ben", raw, 1 << 20)) {
            return "no manifest";
        }
        lt::error_code ec;
        lt::bdecode_node m = lt::bdecode(raw, ec);
        if (ec) {
            return "bad manifest";
        }
        lt::entry e;
        e = m;
        auto &segs = e["segs"].list();
        if (static_cast<int>(segs.size()) >= kMaxSegments) {
            return "too many segments: a snapshot is needed";
        }
        char file[32];
        std::snprintf(file, sizeof(file), "seg-%06d.z", static_cast<int>(segs.size()) + 1);
        const std::string z = zlibDeflate(pending);
        if (z.size() > kMaxSegmentBytes) {
            return "the segment is too big";
        }
        if (!writeFile(dir + "/records/" + file, z)) {
            return "cannot write the segment";
        }
        lt::entry s(lt::entry::dictionary_t);
        s["f"] = std::string(file);
        s["n"] = static_cast<std::int64_t>(std::count(pending.begin(), pending.end(), '\n'));
        s["h"] = sha256(z);
        segs.push_back(s);
        seq = e["seq"].integer() + 1;
        e["seq"] = seq;
        std::array<char, 32> seedArr;
        std::copy(seed.begin(), seed.end(), seedArr.begin());
        lt::dht::public_key pk;
        lt::dht::secret_key sk;
        std::tie(pk, sk) = lt::dht::ed25519_create_keypair(seedArr);
        lt::dht::signature sig = lt::dht::ed25519_sign(signedPart(e), pk, sk);
        e["sig"] = std::string(sig.bytes.data(), sig.bytes.size());
        std::string out;
        lt::bencode(std::back_inserter(out), e);
        if (!writeFile(dir + "/manifest.ben", out)) {
            return "cannot write the manifest";
        }
        std::remove((dir + "/pending.ndjson").c_str());
        Loaded l;
        std::string err = read(dir, l);
        if (!err.empty()) {
            return err;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lists[pubkey] = std::move(l);
        rebuild();
        m_log("[Mesh] list " + pubkey.substr(0, 16) + " version " + std::to_string(seq) + ": " + file);
        return "";
    }

    std::string Mesh::accept(const std::string &dir) {
        Loaded l;
        std::string err = read(dir, l);
        if (!err.empty()) {
            return err;
        }
        MeshList old;
        if (list(l.card.pubkey, old) && old.seq >= l.card.seq) {
            return "not newer than the version held (" + std::to_string(old.seq) + ")";
        }
        const std::string to = listDir(l.card.pubkey);
        if (dir != to) {
            mkdir(to.c_str(), 0777);
            mkdir((to + "/records").c_str(), 0777);
            std::string data;
            for (int i = 1; i <= kMaxSegments; i++) {
                char file[32];
                std::snprintf(file, sizeof(file), "seg-%06d.z", i);
                if (!readFile(dir + "/records/" + file, data, kMaxSegmentBytes)) {
                    break;
                }
                writeFile(to + "/records/" + file, data);
            }
            if (!readFile(dir + "/manifest.ben", data, 1 << 20) || !writeFile(to + "/manifest.ben", data)) {
                return "cannot copy the version";
            }
            err = read(to, l);
            if (!err.empty()) {
                return err;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string pk = l.card.pubkey;
        m_lists[pk] = std::move(l);
        rebuild();
        m_log("[Mesh] list " + pk.substr(0, 16) + " version " + std::to_string(m_lists[pk].card.seq) + " taken");
        return "";
    }

    std::string Mesh::remove(const std::string &pubkey) {
        MeshList card;
        if (!list(pubkey, card)) {
            return "list not found";
        }
        if (card.own) {
            return "a list of this node: its key would be lost";
        }
        const std::string dir = listDir(pubkey);
        for (int i = 1; i <= kMaxSegments; i++) {
            char file[32];
            std::snprintf(file, sizeof(file), "/records/seg-%06d.z", i);
            if (std::remove((dir + file).c_str()) != 0) {
                break;
            }
        }
        std::remove((dir + "/manifest.ben").c_str());
        rmdir((dir + "/records").c_str());
        rmdir(dir.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lists.erase(pubkey);
        rebuild();
        return "";
    }

}
