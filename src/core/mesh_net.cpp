// MediaMesh over the network (stage 3): a list version is a v2 torrent of the list's directory
// (<data>/lists/<pubkey>/: manifest.ben, records/), kept on disk and seeded (hybrid_disk.cpp);
// the pointer to the current version is a BEP 44 mutable item under the list's key, salt
// "mediamesh.v1", value {"ih": the version's v2 infohash (32 bytes), "n": the name}, seq = the
// version. Subscribing: the pointer from the DHT, the version downloaded beside the lists,
// checked (Mesh::accept) and seeded. Every node puts the pointers of its lists again now and
// then (signed by the key's owner, any node can store it again): a list lives on without its
// releaser.

#include <algorithm>
#include <deque>
#include <fstream>
#include <sys/stat.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/kademlia/ed25519.hpp>
#include <libtorrent/kademlia/item.hpp>
#include <libtorrent/hasher.hpp>

#include "core_torrent.hpp"
#include "mesh_ext.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        constexpr const char *kSalt = "mediamesh.v1";
        constexpr int kPieceSize = 64 * 1024;

        std::string hexOf(const char *data, size_t size) {
            static const char *digits = "0123456789abcdef";
            std::string out;
            for (size_t i = 0; i < size; i++) {
                out += digits[(static_cast<unsigned char>(data[i]) >> 4) & 15];
                out += digits[static_cast<unsigned char>(data[i]) & 15];
            }
            return out;
        }

        bool unhexTo(const std::string &h, char *out, size_t size) {
            if (h.size() != size * 2) {
                return false;
            }
            for (size_t i = 0; i < size; i++) {
                auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
                int a = v(h[2 * i]), b = v(h[2 * i + 1]);
                if (a < 0 || b < 0) {
                    return false;
                }
                out[i] = static_cast<char>(a * 16 + b);
            }
            return true;
        }

        bool validDiscoveryName(const std::string &name) {
            if (name.empty() || name.size() > 64) return false;
            for (size_t i = 0; i < name.size();) {
                const unsigned char c = static_cast<unsigned char>(name[i]);
                if (c < 0x20 || c == 0x7f) return false;
                if (c < 0x80) {
                    ++i;
                    continue;
                }
                size_t tail = 0;
                if (c >= 0xc2 && c <= 0xdf) tail = 1;
                else if (c >= 0xe0 && c <= 0xef) tail = 2;
                else if (c >= 0xf0 && c <= 0xf4) tail = 3;
                else return false;
                if (i + tail >= name.size()) return false;
                const unsigned char second = static_cast<unsigned char>(name[i + 1]);
                if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
                    (c == 0xf0 && second < 0x90) || (c == 0xf4 && second > 0x8f)) return false;
                for (size_t j = 1; j <= tail; ++j) {
                    const unsigned char next = static_cast<unsigned char>(name[i + j]);
                    if (next < 0x80 || next > 0xbf) return false;
                }
                i += tail + 1;
            }
            return true;
        }

        std::string readAll(const std::string &path, size_t limit = 2 * 1024 * 1024) {
            std::ifstream in(path, std::ios::binary);
            std::string out(limit + 1, '\0');
            in.read(&out[0], static_cast<std::streamsize>(out.size()));
            const size_t count = static_cast<size_t>(in.gcount());
            if (count > limit) {
                return {};
            }
            out.resize(count);
            return out;
        }

        bool peerEndpoint(const std::string &hint, lt::tcp::endpoint &endpoint) {
            const size_t colon = hint.rfind(':');
            if (colon == std::string::npos || colon + 1 == hint.size()) {
                return false;
            }
            std::string address = hint.substr(0, colon);
            if (address.size() > 2 && address.front() == '[' && address.back() == ']') {
                address = address.substr(1, address.size() - 2);
            }
            unsigned port = 0;
            for (size_t i = colon + 1; i < hint.size(); ++i) {
                if (hint[i] < '0' || hint[i] > '9' || port > 6553) {
                    return false;
                }
                port = port * 10 + static_cast<unsigned>(hint[i] - '0');
            }
            lt::error_code ec;
            const auto ip = lt::make_address(address, ec);
            if (ec || port == 0 || port > 65535) {
                return false;
            }
            endpoint = lt::tcp::endpoint(ip, static_cast<uint16_t>(port));
            return true;
        }

        bool writeAll(const std::string &path, const std::string &data) {
            FILE *f = std::fopen((path + ".tmp").c_str(), "wb");
            if (!f) {
                return false;
            }
            bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok && std::rename((path + ".tmp").c_str(), path.c_str()) != 0) {
                std::remove(path.c_str());
                ok = std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
            }
            return ok;
        }

        void removeTree(const std::string &dir) {
            if (DIR *d = opendir(dir.c_str())) {
                while (dirent *e = readdir(d)) {
                    const std::string name = e->d_name;
                    if (name == "." || name == "..") {
                        continue;
                    }
                    const std::string p = dir + "/" + name;
                    struct stat st{};
                    if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                        removeTree(p);
                    } else {
                        std::remove(p.c_str());
                    }
                }
                closedir(d);
            }
            rmdir(dir.c_str());
        }
    }

    // the torrent of a list's version, made from its directory (the same files, piece size
    // and name give the same infohash on every node)
    std::shared_ptr<lt::torrent_info> Core::meshTorrent(const std::string &pk, std::string &error) {
        const std::string root = m_store.dir() + "/lists";
        MeshList card;
        if (!m_mesh.list(pk, card)) {
            error = "list not found";
            return nullptr;
        }
        lt::file_storage fs;
        std::set<std::string> allowed{"manifest.ben"};
        for (const auto &f : m_mesh.plan(pk)) allowed.insert(f.path);
        lt::add_files(fs, root + "/" + pk, [&](const std::string &path) {
            if (path == root + "/" + pk || path == root + "/" + pk + "/records" || path == root + "/" + pk + "/media") return true;
            return path.size() > root.size()+pk.size()+2 && allowed.count(path.substr(root.size()+pk.size()+2)) != 0;
        });
        if (fs.num_files() == 0) {
            error = "no files";
            return nullptr;
        }
        lt::create_torrent ct(fs, kPieceSize, lt::create_torrent::v2_only);
        lt::error_code ec;
        lt::set_piece_hashes(ct, root, ec);
        if (ec) {
            error = ec.message();
            return nullptr;
        }
        std::vector<char> buf;
        lt::bencode(std::back_inserter(buf), ct.generate());
        auto ti = std::make_shared<lt::torrent_info>(buf, ec, lt::from_span);
        if (ec) {
            error = ec.message();
            return nullptr;
        }
        return ti;
    }

    // Keep the ORIGINAL info dictionary, not a torrent rebuilt from partial files.
    // V2 piece layers are optional: peers supply authenticated hash trees on demand.
    namespace {
        std::string originalTorrent(const lt::torrent_info &ti, const std::string &pointer = "") {
            const auto info = ti.info_section();
            std::string out = "d4:info" + std::string(info.data(), info.size());
            if (!pointer.empty()) {
                out += "14:p2play-pointer";
                lt::bencode(std::back_inserter(out), lt::entry(pointer));
            }
            return out + "e";
        }
        void copyPayload(const std::string &from, const std::string &to, size_t limit) {
            std::ifstream source(from, std::ios::binary);
            if (source.good()) writeAll(to, readAll(from, limit));
        }

        bool copyVerifiedPayload(const std::string &from, const std::string &to, size_t limit,
                                 int64_t expectedBytes, const std::string &expectedSha256) {
            if (expectedBytes <= 0 || static_cast<uint64_t>(expectedBytes) > limit) return false;
            const auto bytes = readAll(from, limit);
            if (static_cast<int64_t>(bytes.size()) != expectedBytes) return false;
            const auto digest = lt::hasher256(bytes).final();
            if (hexOf(digest.data(), digest.size()) != expectedSha256) return false;
            return writeAll(to, bytes);
        }
    }

    void Core::meshDetach(const std::string &pk) {
        lt::torrent_handle handle;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto it = m_meshNet.find(pk); if (it == m_meshNet.end()) return;
            auto &n = it->second;
            handle = n.fetching.is_valid() ? n.fetching : n.seeding;
            if (handle.is_valid() && !n.fetchDir.empty()) m_meshRetired[handle] = n.fetchDir;
            n.fetching = {}; n.seeding = {}; n.fetchIh = {}; n.versionIh = {};
            n.fetchDir.clear(); n.accepted = false; n.prioritiesSet = false;
            n.installed.clear(); n.mediaWanted.clear(); n.reservedMedia.clear(); n.recordsReady = false; n.mediaDeadlines.clear(); n.importing = false;
        }
        // Isolated storage is cleaned only after the delete job completes (success or
        // failure). A partial torrent can leave empty directories/part files behind.
        if (handle.is_valid()) m_session->remove_torrent(handle, lt::session::delete_files);
    }

    void Core::meshSeed(const std::string &pk, std::shared_ptr<lt::torrent_info> ti) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        MeshList card; if (!m_mesh.list(pk, card)) return;
        std::string error;
        const auto local = m_store.dir() + "/mesh-local/" + pk;
        if (!ti && !card.own) {
            MeshExtension::Item item; std::string pointer;
            // A crash after manifest commit but before metadata promotion is recoverable.
            for (const auto &name : {"original.pending.torrent", "original.torrent"}) {
                const auto raw = readAll(local + "/" + name, 8 << 20);
                if (raw.empty()) continue;
                lt::error_code ec;
                auto candidate = std::make_shared<lt::torrent_info>(lt::span<char const>(raw.data(), raw.size()), ec, lt::from_span);
                if (ec) continue;
                auto envelope = lt::bdecode(raw, ec);
                pointer = ec ? "" : std::string(envelope.dict_find_string_value("p2play-pointer"));
                if (pointer.empty()) pointer = readAll(m_mesh.listDir(pk) + "/pointer.ben", MeshExtension::kMaxItem);
                if (!MeshExtension::decodeItem(pointer, item) || hexOf(item.key.data(), 32) != pk ||
                    item.seq != card.seq || lt::sha256_hash(item.value["ih"].string().data()) != candidate->info_hashes().v2) continue;
                ti = candidate;
                break;
            }
            // No reconstruction fallback: absent metadata means offline imported holdings.
            if (!ti) return;
            std::lock_guard<std::mutex> lock(m_meshMutex);
            m_meshNet[pk].pointer = pointer; m_meshNet[pk].pointerSeq = item.seq;
        }
        if (!ti) ti = meshTorrent(pk, error); // owner only
        if (!ti) { log("[Mesh] not seeded: %s", error.c_str()); return; }
        mkdir(local.c_str(), 0700);
        std::string pointer;
        { std::lock_guard<std::mutex> lock(m_meshMutex); pointer = m_meshNet[pk].pointer; }
        if (!writeAll(local + "/original.torrent", originalTorrent(*ti, pointer))) return;
        std::remove((local + "/original.pending.torrent").c_str());
        meshDetach(pk);
        // Signing does not depend on the old storage's asynchronous retirement.
        // A release must not leave a one-shot BEP10 get observing the old sequence.
        if (card.own) {
            { std::lock_guard<std::mutex> lock(m_meshMutex); m_meshNet[pk].versionIh = ti->info_hashes().v2; }
            meshPut(pk);
        }
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            for (const auto &retired : m_meshRetired)
                if (retired.second.find("/" + pk + "-") != std::string::npos) return;
        }
        const auto root = m_store.dir() + "/lists/.network";
        mkdir(root.c_str(), 0700);
        const auto dir = root + "/" + pk + "-" + std::to_string(++m_meshGeneration);
        mkdir(dir.c_str(), 0700); mkdir((dir + "/" + pk).c_str(), 0700);
        mkdir((dir + "/" + pk + "/records").c_str(), 0700); mkdir((dir + "/" + pk + "/media").c_str(), 0700);
        copyPayload(m_mesh.listDir(pk) + "/manifest.ben", dir + "/" + pk + "/manifest.ben", 4 << 20);
        for (const auto &f : m_mesh.plan(pk)) if (f.present && (f.kind != "records" || f.selected))
            copyPayload(m_mesh.listDir(pk) + "/" + f.path, dir + "/" + pk + "/" + f.path,
                        f.kind == "records" ? Mesh::kMaxSegmentBytes : Mesh::kMaxPackBytes);
        lt::add_torrent_params atp;
        atp.ti = ti; atp.save_path = dir;
        atp.file_priorities.assign(ti->num_files(), lt::dont_download);
        // NEVER seed_mode: even complete local files are checked, partial ones are legal.
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused | lt::torrent_flags::seed_mode);
        lt::error_code ec; auto added = m_session->add_torrent(std::move(atp), ec);
        if (ec || !added.is_valid()) { removeTree(dir); return; }
        std::vector<std::string> peers;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto &n = m_meshNet[pk]; n.seeding = added; n.versionIh = ti->info_hashes().v2;
            n.fetchIh = n.versionIh; n.fetchSeq = card.seq; n.fetchDir = dir;
            n.accepted = true; n.prioritiesSet = false; n.netError.clear(); peers = n.peers;
        }
        for (const auto &peer : peers) { lt::tcp::endpoint endpoint; if (peerEndpoint(peer, endpoint)) added.connect_peer(endpoint); }
        meshPut(pk);
    }

    void Core::meshRefresh(const std::string &pk) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        meshSeed(pk, nullptr);
    }

    void Core::meshForget(const std::string &pk) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        meshDetach(pk);
        std::lock_guard<std::mutex> lock(m_meshMutex);
        m_meshNet.erase(pk);
        m_meshMediaQueue.erase(std::remove_if(m_meshMediaQueue.begin(), m_meshMediaQueue.end(),
            [&](const auto &r) { return r.pk == pk; }), m_meshMediaQueue.end());
        const auto local = m_store.dir() + "/mesh-local/" + pk;
        std::remove((local + "/original.torrent").c_str());
        std::remove((local + "/original.pending.torrent").c_str());
        rmdir(local.c_str());
    }

    bool Core::meshMediaRequest(const std::string &pk, const std::string &ih, const std::string &kind) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        MeshList card; if (!m_mesh.list(pk, card)) return false;
        const auto path = m_mesh.mediaPath(pk, ih, kind); if (path.empty()) return false;
        std::lock_guard<std::mutex> lock(m_meshMutex);
        auto it = m_meshNet.find(pk);
        if (it == m_meshNet.end() || !it->second.accepted || !it->second.seeding.is_valid()) return false;
        if (it->second.mediaWanted.count(path)) return true;
        for (const auto &r : m_meshMediaQueue) if (r.pk == pk && r.path == path && r.seq == card.seq) return true;
        size_t active = m_meshMediaQueue.size();
        for (const auto &n : m_meshNet) active += n.second.mediaDeadlines.size();
        if (active >= 64) return false;
        it->second.installed.erase(path); // repair a corrupted installed pack from checked torrent storage
        m_meshMediaQueue.push_back({pk, path, card.seq});
        return true;
    }

    // Create/cache a pointer independently of DHT: isolated peers must work too.
    void Core::meshPut(const std::string &pk) {
        MeshList card;
        MeshExtension::Item item;
        item.key.resize(32);
        if (!unhexTo(pk, &item.key[0], 32) || !m_mesh.list(pk, card)) {
            return;
        }
        const std::string dir = m_mesh.listDir(pk);
        lt::sha256_hash ih;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            ih = m_meshNet[pk].versionIh;
        }
        if (card.own && card.seq >= 0 && !ih.is_all_zeros()) {
            const std::string seedHex = readAll(m_store.dir() + "/keys/" + pk + ".seed", 128);
            std::array<char, 32> seed;
            if (seedHex.size() < 64 || !unhexTo(seedHex.substr(0, 64), seed.data(), 32)) {
                return;
            }
            lt::dht::public_key publicKey;
            lt::dht::secret_key secretKey;
            std::tie(publicKey, secretKey) = lt::dht::ed25519_create_keypair(seed);
            if (!std::equal(publicKey.bytes.begin(), publicKey.bytes.end(), item.key.begin())) {
                return;
            }
            item.salt = kSalt;
            item.seq = card.seq;
            item.value = lt::entry(lt::entry::dictionary_t);
            item.value["ih"] = std::string(ih.data(), ih.size());
            item.value["n"] = card.name.substr(0, 64);
            std::vector<char> value;
            lt::bencode(std::back_inserter(value), item.value);
            const auto sig = lt::dht::sign_mutable_item(value, item.salt, lt::dht::sequence_number(item.seq),
                                                       publicKey, secretKey);
            item.signature.assign(sig.bytes.data(), sig.bytes.size());
        } else {
            std::string raw;
            {
                std::lock_guard<std::mutex> lock(m_meshMutex);
                raw = m_meshNet[pk].pointer;
            }
            if (raw.empty()) {
                // Upgrade pre-peer-exchange pointer files, after verifying the original signature.
                raw = readAll(dir + "/pointer.ben", MeshExtension::kMaxItem);
                if (raw.size() > MeshExtension::kMaxItem) {
                    return;
                }
                lt::error_code ec;
                auto old = lt::bdecode(raw, ec, nullptr, 8, 64);
                if (ec || old.type() != lt::bdecode_node::dict_t) {
                    return;
                }
                lt::entry envelope(old);
                envelope["k"] = item.key;
                envelope["salt"] = kSalt;
                raw.clear();
                lt::bencode(std::back_inserter(raw), envelope);
            }
            if (!MeshExtension::decodeItem(raw, item) || hexOf(item.key.data(), 32) != pk || item.seq < card.seq) {
                return;
            }
        }
        const std::string raw = MeshExtension::encodeItem(item);
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto &n = m_meshNet[pk];
            if (item.seq < n.pointerSeq || (item.seq == n.pointerSeq && !n.pointer.empty() && n.pointer != raw)) {
                return;
            }
            n.pointerSeq = item.seq;
            n.pointer = raw;
        }
        writeAll(dir + "/pointer.ben", raw);
        if (m_settings.disableDht) {
            return;
        }
        std::array<char, 32> key;
        std::copy(item.key.begin(), item.key.end(), key.begin());
        m_session->dht_put_item(key, [item](lt::entry &e, std::array<char, 64> &sig, std::int64_t &seq,
                                          std::string const &) {
            // Do not overwrite a newer DHT item with an older locally cached version.
            if (seq > item.seq) {
                return;
            }
            e = item.value;
            std::copy(item.signature.begin(), item.signature.end(), sig.begin());
            seq = item.seq;
        }, kSalt);
    }

    // Mark a peer request and retain the existing independent DHT lookup.
    void Core::meshGet(const std::string &pk) {
        std::array<char, 32> key;
        if (!unhexTo(pk, key.data(), 32)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            m_meshNet[pk].requested = true;
        }
        if (!m_settings.disableDht) {
            m_session->dht_get_item(key, kSalt);
        }
    }

    void Core::meshPeerReceived(const std::string &raw, const std::string &peer, const std::string &source) {
        if (raw.size() > MeshExtension::kMaxItem || m_stopping) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_meshMutex);
        m_meshIncoming.push({raw, peer, source});
    }

    std::vector<std::pair<std::string, int64_t>> Core::meshRequests() {
        std::vector<std::pair<std::string, int64_t>> out;
        std::lock_guard<std::mutex> lock(m_meshMutex);
        for (const auto &e : m_meshNet) {
            if (!e.second.requested && !e.second.wanted &&
                !(e.second.discoveryOnly && e.second.pointerSeq < 0)) {
                continue;
            }
            std::string key(32, '\0');
            if (unhexTo(e.first, &key[0], 32)) {
                out.emplace_back(key, e.second.pointerSeq);
            }
            if (out.size() >= 4096) {
                break;
            }
        }
        return out;
    }

    bool Core::meshPeerItem(const std::string &key, std::string &raw) {
        if (key.size() != 32) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_meshMutex);
        const auto it = m_meshNet.find(hexOf(key.data(), key.size()));
        if (it == m_meshNet.end() || it->second.pointer.empty()) {
            return false;
        }
        raw = it->second.pointer;
        return true;
    }

    // Only the idle worker verifies/imports items. In particular, no synchronous
    // add_torrent, filesystem access or DHT API calls occur in BEP 10 callbacks.
    void Core::meshReceivePointer(const std::string &raw, const std::string &peer) {
        MeshExtension::Item item;
        if (!MeshExtension::decodeItem(raw, item)) {
            return;
        }
        const std::string pk = hexOf(item.key.data(), item.key.size());
        MeshList held;
        const bool have = m_mesh.list(pk, held);
        bool fetchVersion = false;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto it = m_meshNet.find(pk);
            if (it == m_meshNet.end() || (!have && !it->second.wanted && !it->second.discoveryOnly) ||
                (have && held.seq >= item.seq)) {
                return;
            }
            auto &n = it->second;
            if (item.seq < n.pointerSeq || (item.seq == n.pointerSeq && !n.pointer.empty() && n.pointer != raw)) {
                return; // rollback or same-sequence equivocation
            }
            n.pointerSeq = item.seq;
            n.pointer = raw;
            n.requested = false;
            n.pendingGet = false;
            if (!peer.empty() && n.peers.size() < 128 && std::find(n.peers.begin(), n.peers.end(), peer) == n.peers.end()) {
                n.peers.push_back(peer);
            }
            if (m_discovered.size() < 4096 || m_discovered.count(pk) != 0) {
                Discovered &d = m_discovered[pk];
                d.seq = std::max(d.seq, item.seq);
                const std::string name = item.value["n"].string();
                if (validDiscoveryName(name) && item.seq >= d.nameSeq) {
                    d.name = name;
                    d.nameSeq = item.seq;
                }
            }
            fetchVersion = n.wanted || have;
        }
        // A verified pointer is enough for a discovery card. Fetch content only after
        // an explicit subscription or when this node already holds an older list version.
        if (fetchVersion) {
            meshFetch(pk, lt::sha256_hash(item.value["ih"].string().data()), item.seq, raw);
        }
    }

    void Core::meshWork() {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        std::vector<MeshIncomingQueue::Item> incoming;
        std::vector<std::string> lookups, keys, garbage;
        std::vector<MeshMediaRequest> media;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            incoming = m_meshIncoming.take(); garbage.swap(m_meshGarbage); media.swap(m_meshMediaQueue);
            for (auto &e : m_meshNet) {
                keys.push_back(e.first);
                if (e.second.pendingGet) { e.second.pendingGet = false; lookups.push_back(e.first); }
            }
        }
        // Fairness is applied to the detached batch, outside m_meshMutex.
        auto fairIncoming = MeshIncomingQueue::roundRobin(std::move(incoming));
        for (const auto &dir : garbage) removeTree(dir);
        for (const auto &pk : lookups) meshGet(pk);
        for (const auto &item : fairIncoming) { if (m_stopping) return; meshReceivePointer(item.raw, item.peer); }
        const auto now = std::chrono::steady_clock::now();
        for (const auto &pk : keys) {
            if (m_stopping) return;
            MeshNet n;
            { std::lock_guard<std::mutex> lock(m_meshMutex); n = m_meshNet[pk]; }
            auto h = n.fetching.is_valid() ? n.fetching : n.seeding;
            if (!h.is_valid()) {
                if (now < n.nextRetry) continue;
                MeshList card; const bool have = m_mesh.list(pk, card);
                if (!n.pointer.empty() && (!have || card.seq < n.pointerSeq)) {
                    MeshExtension::Item item;
                    if (MeshExtension::decodeItem(n.pointer, item)) meshFetch(pk, lt::sha256_hash(item.value["ih"].string().data()), item.seq, n.pointer);
                } else if (have) meshSeed(pk, nullptr);
                continue;
            }
            auto ti = h.torrent_file();
            std::string failure; bool newlyAccepted = false;
            if (!ti) {
                if (!n.accepted && now - n.transferStarted > std::chrono::minutes(2)) failure = "metadata timeout";
            } else {
                const auto &fs = ti->files();
                const auto manifestPath = pk + "/manifest.ben";
                lt::file_index_t manifest(-1);
                if (fs.num_files() > 16385 || !ti->info_hashes().has_v2() || ti->info_hashes().v2 != n.fetchIh) failure = "invalid version metadata";
                for (auto i : fs.file_range()) {
                    const auto path = fs.file_path(i);
                    if (fs.pad_file_at(i)) continue;
                    if (fs.file_flags(i) & lt::file_storage::flag_symlink) { failure = "torrent symlink refused"; break; }
                    if (path == manifestPath) { manifest = i; if (fs.file_size(i) <= 0 || fs.file_size(i) > (4 << 20)) failure = "manifest too large"; }
                    else if (path.compare(0, pk.size()+1, pk+"/") != 0) failure = "wrong list torrent root";
                }
                if (manifest == lt::file_index_t(-1)) failure = "missing version manifest";
                if (failure.empty()) {
                    if (!n.prioritiesSet) {
                        std::vector<lt::download_priority_t> priorities(fs.num_files(), lt::dont_download);
                        priorities[static_cast<int>(manifest)] = lt::top_priority;
                        h.prioritize_files(priorities); n.prioritiesSet = true;
                    }
                    auto progress = h.file_progress(lt::torrent_handle::piece_granularity);
                    if (!n.accepted && progress.size() == size_t(fs.num_files()) && progress[static_cast<int>(manifest)] == fs.file_size(manifest)) {
                        // Persist authenticated torrent identity BEFORE manifest commit; recover
                        // the pending sidecar on restart if the process exits between renames.
                        const auto local = m_store.dir() + "/mesh-local/" + pk;
                        mkdir(local.c_str(), 0700);
                        if (!writeAll(local + "/original.pending.torrent", originalTorrent(*ti, n.fetchPointer))) failure = "cannot persist original version metadata";
                        else failure = m_mesh.accept(n.fetchDir + "/" + pk, pk, n.fetchSeq, !n.metaParents.empty());
                        if (failure.empty()) {
                            n.accepted = true; n.seeding = h; n.fetching = {}; n.versionIh = n.fetchIh;
                            n.netError.clear(); newlyAccepted = true;
                            writeAll(m_mesh.listDir(pk) + "/pointer.ben", n.fetchPointer);
                            // Failure here is recoverable using the pending file and its signed pointer.
                            std::rename((local + "/original.pending.torrent").c_str(), (local + "/original.torrent").c_str());
                        }
                    }
                    if (n.accepted && failure.empty()) {
                        MeshList card;
                        if (!m_mesh.list(pk, card) || card.seq != n.fetchSeq) failure = "version changed";
                        else {
                            const auto plan = m_mesh.plan(pk); const auto policy = m_mesh.policy(pk);
                            std::map<std::string, MeshFile> files;
                            for (const auto &f : plan) files[f.path] = f;
                            // Strict metadata allowlist. No unsigned file may ever be downloaded.
                            std::set<std::string> torrentPaths;
                            for (auto i : fs.file_range()) {
                                if (fs.pad_file_at(i) || i == manifest) continue;
                                torrentPaths.insert(fs.file_path(i).substr(pk.size()+1));
                                auto f = files.find(fs.file_path(i).substr(pk.size()+1));
                                if (f == files.end() || (f->second.signedSize ? f->second.bytes != fs.file_size(i) : fs.file_size(i) > int64_t(Mesh::kMaxSegmentBytes))) { failure = "torrent disagrees with signed file descriptors"; break; }
                            }
                            for (const auto &f : files) {
                                if (!torrentPaths.count(f.first)) { failure = "signed file absent from version torrent"; break; }
                            }
                            if (failure.empty()) {
                                for (const auto &r : media) if (r.pk == pk && r.seq == n.fetchSeq) {
                                    n.mediaWanted.insert(r.path); n.mediaDeadlines[r.path] = now + std::chrono::seconds(60);
                                }
                                for (auto it = n.mediaDeadlines.begin(); it != n.mediaDeadlines.end();) {
                                    if (it->second <= now) { n.mediaWanted.erase(it->first); it = n.mediaDeadlines.erase(it); }
                                    else ++it;
                                }
                                std::set<std::string> admittedMedia;
                                int64_t usedMedia = 0;
                                // Existing holdings reserve their budget first; requests cannot cause unbounded staging.
                                for (const auto &f : plan) if (f.kind != "records" && (f.present || n.reservedMedia.count(f.path))) {
                                    n.reservedMedia.insert(f.path); usedMedia += f.bytes;
                                    if (f.present || card.own || policy.mirrorMedia || n.mediaWanted.count(f.path)) admittedMedia.insert(f.path);
                                }
                                for (const auto &f : plan) if (f.kind != "records" && !f.present && !n.reservedMedia.count(f.path) &&
                                    (card.own || policy.mirrorMedia || n.mediaWanted.count(f.path)) && f.bytes <= policy.mediaBytes-usedMedia) {
                                    admittedMedia.insert(f.path); n.reservedMedia.insert(f.path); usedMedia += f.bytes;
                                }
                                std::vector<lt::download_priority_t> priorities(fs.num_files(), lt::dont_download);
                                priorities[static_cast<int>(manifest)] = lt::top_priority;
                                for (auto i : fs.file_range()) {
                                    if (fs.pad_file_at(i) || i == manifest) continue;
                                    const auto path = fs.file_path(i).substr(pk.size()+1);
                                    const auto &f = files.at(path);
                                    const bool wanted = f.kind == "records" ? f.selected : admittedMedia.count(path) != 0;
                                    if (!wanted) continue;
                                    priorities[static_cast<int>(i)] = f.kind == "records" ? lt::top_priority : lt::default_priority;
                                    if (progress.size() == size_t(fs.num_files()) && progress[static_cast<int>(i)] == fs.file_size(i) && !n.installed.count(path)) {
                                        const auto err = m_mesh.installFile(pk, path, n.fetchDir+"/"+pk+"/"+path);
                                        if (err.empty()) { n.installed.insert(path); n.mediaWanted.erase(path); n.mediaDeadlines.erase(path); }
                                        else { failure = path+": "+err; break; }
                                    }
                                }
                                if (failure.empty()) {
                                    h.prioritize_files(priorities);
                                    n.recordsReady = true;
                                    for (const auto &f : m_mesh.plan(pk)) if (f.kind == "records" && f.selected && !f.present) n.recordsReady = false;
                                }
                            }
                        }
                    }
                }
                if (!n.accepted && failure.empty() && now - n.transferStarted > std::chrono::minutes(2)) failure = "manifest timeout";
            }
            if (!failure.empty()) {
                log("[Mesh] %s: %s", pk.substr(0,16).c_str(), failure.c_str());
                meshDetach(pk);
                std::lock_guard<std::mutex> lock(m_meshMutex);
                m_meshNet[pk].netError = failure; m_meshNet[pk].nextRetry = now + std::chrono::seconds(60);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_meshMutex);
                n.pendingGet = m_meshNet[pk].pendingGet; n.requested = n.requested || m_meshNet[pk].requested;
                m_meshNet[pk] = std::move(n);
            }
            if (newlyAccepted) {
                meshSyncMetaSubscriptions();
                meshPut(pk);
            }
        }
    }

    std::string Core::meshSubscribe(const std::string &pk, const std::vector<std::string> &peers,
                                    const std::string &metaParent) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        std::array<char, 32> key;
        if (!unhexTo(pk, key.data(), 32)) {
            return "a list key is 64 hex digits";
        }
        if (!metaParent.empty()) {
            MeshList parent;
            if (!m_mesh.list(metaParent, parent) || !parent.meta) return "parent is not a held meta-list";
            auto item = std::find_if(parent.items.begin(), parent.items.end(),
                                     [&](const MeshMetaItem &entry) { return entry.pubkey == pk; });
            if (item == parent.items.end() || !item->enabled) return "child is not enabled by the parent meta-list";
            MeshList child;
            if (m_mesh.list(pk, child) && child.meta) return "nested meta-lists are not allowed";
        }
        std::string cachedPointer;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            MeshNet &n = m_meshNet[pk];
            if (metaParent.empty()) n.directWanted = true;
            else n.metaParents.insert(metaParent);
            n.wanted = n.directWanted || !n.metaParents.empty();
            cachedPointer = n.pointer;
            if (!peers.empty()) n.peers.assign(peers.begin(), peers.begin() + std::min<size_t>(peers.size(), 128));
        }
        // Hints also bootstrap the directory connection when DHT/LSD are unavailable.
        for (size_t i = 0; i < peers.size() && i < 128; ++i) {
            lt::tcp::endpoint endpoint;
            if (m_directory.is_valid() && peerEndpoint(peers[i], endpoint)) {
                m_directory.connect_peer(endpoint);
            }
        }
        log("[Mesh] list %s: asking peers and the DHT for its version", pk.substr(0, 16).c_str());
        meshGet(pk);
        if (!cachedPointer.empty()) {
            meshReceivePointer(cachedPointer, "");
        }
        return "";
    }

    // Reconstruct the enabled one-level relationships from signed manifests. A child
    // already known to be a meta-list is never followed. Unknown children are tagged before
    // their first pointer fetch, so the worker can reject a meta manifest before accepting it.
    void Core::meshSyncMetaSubscriptions() {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        std::map<std::string, std::set<std::string>> parents;
        for (const auto &parent : m_mesh.lists()) {
            if (!parent.meta) continue;
            for (const auto &item : parent.items) {
                if (!item.enabled) continue;
                MeshList child;
                if (m_mesh.list(item.pubkey, child) && child.meta) {
                    log("[Mesh] refusing nested meta-list %s from %s",
                        item.pubkey.substr(0, 16).c_str(), parent.pubkey.substr(0, 16).c_str());
                    continue;
                }
                parents[item.pubkey].insert(parent.pubkey);
            }
        }
        std::vector<std::string> cancel;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            for (auto &entry : m_meshNet) {
                const bool wasMetaChild = !entry.second.metaParents.empty();
                auto current = parents.find(entry.first);
                if (current == parents.end()) {
                    if (wasMetaChild && !entry.second.directWanted) {
                        if (entry.second.fetching.is_valid()) cancel.push_back(entry.first);
                        entry.second.pointer.clear();
                        entry.second.pointerSeq = -1;
                    }
                    entry.second.metaParents.clear();
                    entry.second.wanted = entry.second.directWanted;
                } else {
                    entry.second.metaParents = current->second;
                    entry.second.wanted = entry.second.directWanted || !entry.second.metaParents.empty();
                }
            }
            for (const auto &entry : parents) {
                auto &net = m_meshNet[entry.first];
                net.metaParents = entry.second;
                net.wanted = net.directWanted || !net.metaParents.empty();
            }
        }
        for (const auto &pk : cancel) {
            meshDetach(pk);
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto &net = m_meshNet[pk];
            if (!net.directWanted && net.metaParents.empty()) {
                net.pointer.clear();
                net.pointerSeq = -1;
                net.wanted = false;
            }
        }
        for (const auto &entry : parents) {
            const auto parent = *entry.second.begin();
            const auto err = meshSubscribe(entry.first, {}, parent);
            if (!err.empty()) log("[Mesh] meta child %s not subscribed: %s",
                                  entry.first.substr(0, 16).c_str(), err.c_str());
        }
    }

    // Metadata only initially. default_dont_download closes the metadata-alert race:
    // no record/media payload can start before the worker authenticates the manifest.
    void Core::meshFetch(const std::string &pk, const lt::sha256_hash &ih, int64_t seq, const std::string &pointer) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            const auto &n = m_meshNet[pk];
            if (n.fetchSeq > seq || (n.fetchSeq == seq && (n.fetching.is_valid() || n.seeding.is_valid()))) return;
        }
        meshDetach(pk);
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            for (const auto &retired : m_meshRetired)
                if (retired.second.find("/" + pk + "-") != std::string::npos) return;
        }
        const auto root = m_store.dir() + "/lists/.network"; mkdir(root.c_str(), 0700);
        const auto staging = root + "/" + pk + "-" + std::to_string(++m_meshGeneration);
        mkdir(staging.c_str(), 0700);
        const auto listRoot = staging + "/" + pk;
        mkdir(listRoot.c_str(), 0700);
        mkdir((listRoot + "/records").c_str(), 0700);

        // Append releases retain old immutable record descriptors. Pre-populate the new
        // torrent's working tree from files already held for this list, but only after
        // checking them against the previous signed descriptor. Libtorrent still verifies
        // every copied byte against the new v2 torrent before treating a file as complete.
        size_t reusedRecordSegments = 0;
        for (const auto &f : m_mesh.plan(pk)) {
            if (f.kind != "records" || !f.selected || !f.present ||
                f.path.compare(0, 8, "records/") != 0) continue;
            const auto source = m_mesh.listDir(pk) + "/" + f.path;
            const auto target = listRoot + "/" + f.path;
            if (copyVerifiedPayload(source, target, Mesh::kMaxSegmentBytes, f.bytes, f.sha256))
                ++reusedRecordSegments;
        }
        if (reusedRecordSegments) {
            log("[Mesh] list %s: preloaded_cached_record_segments=%zu for version %lld",
                pk.substr(0, 16).c_str(), reusedRecordSegments, static_cast<long long>(seq));
        }

        lt::add_torrent_params atp;
        atp.info_hashes = lt::info_hash_t(ih); atp.name = pk; atp.save_path = staging;
        atp.flags |= lt::torrent_flags::default_dont_download;
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused | lt::torrent_flags::seed_mode);
        lt::error_code ec; auto h = m_session->add_torrent(std::move(atp), ec);
        if (ec || !h.is_valid()) { removeTree(staging); return; }
        std::vector<std::string> peers;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto &n = m_meshNet[pk]; n.fetching = h; n.fetchIh = ih; n.fetchSeq = seq;
            n.fetchPointer = pointer; n.fetchDir = staging; n.transferStarted = std::chrono::steady_clock::now();
            peers = n.peers;
        }
        for (const auto &p : peers) { lt::tcp::endpoint endpoint; if (peerEndpoint(p, endpoint)) h.connect_peer(endpoint); }
        log("[Mesh] list %s: fetching manifest for version %lld", pk.substr(0,16).c_str(), static_cast<long long>(seq));
    }

    // the DHT's answers and the fetched versions; true: a MediaMesh alert
    bool Core::meshAlert(lt::alert *a) {
        if (auto *mi = lt::alert_cast<lt::dht_mutable_item_alert>(a)) {
            if (mi->salt == kSalt && mi->item.type() == lt::entry::dictionary_t) {
                MeshExtension::Item item;
                item.key.assign(mi->key.data(), mi->key.size());
                item.salt = mi->salt;
                item.seq = mi->seq;
                item.signature.assign(mi->signature.data(), mi->signature.size());
                item.value = mi->item;
                meshPeerReceived(MeshExtension::encodeItem(item));
            }
            return true;
        }
        if (auto *deleted = lt::alert_cast<lt::torrent_deleted_alert>(a)) {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto it = m_meshRetired.find(deleted->handle);
            if (it == m_meshRetired.end()) return false;
            m_meshGarbage.push_back(it->second); m_meshRetired.erase(it);
            return true;
        }
        if (auto *failed = lt::alert_cast<lt::torrent_delete_failed_alert>(a)) {
            // This also marks completion of the disk delete job. Missing selective
            // files or extra empty directories must not permanently block reseeding.
            std::lock_guard<std::mutex> lock(m_meshMutex);
            auto it = m_meshRetired.find(failed->handle);
            if (it == m_meshRetired.end()) return false;
            m_meshGarbage.push_back(it->second); m_meshRetired.erase(it);
            return true;
        }
        if (auto *pa = lt::alert_cast<lt::dht_put_alert>(a)) {
            if (pa->salt == kSalt) {
                log("[Mesh] list %s: pointer %lld put on %d nodes", hexOf(pa->public_key.data(), 32).substr(0, 16).c_str(),
                    static_cast<long long>(pa->seq), pa->num_success);
            }
            return true;
        }
        return false;
    }

    // at the start and every 30 minutes: our lists seeded, their pointers put again, newer
    // versions asked for
    void Core::meshTick(bool start) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        if (start) removeTree(m_store.dir() + "/lists/.network"); // no mesh handles exist yet
        meshWork();
        const auto now = std::chrono::steady_clock::now();
        if (!start && now - m_meshLast < std::chrono::minutes(30)) {
            // a list subscribed to, no version yet: asked again every 15 s
            if (now - m_meshWantedLast > std::chrono::seconds(15)) {
                m_meshWantedLast = now;
                std::vector<std::string> wanted, retry;
                {
                    std::lock_guard<std::mutex> lock(m_meshMutex);
                    for (const auto &e : m_meshNet) {
                        const auto &n = e.second;
                        if (n.fetching.is_valid() || n.importing || !n.fetchIh.is_all_zeros()) {
                            continue;
                        }
                        MeshList l;
                        const bool have = m_mesh.list(e.first, l);
                        if (n.wanted && !have) {
                            wanted.push_back(e.first);
                        }
                        const auto known = m_discovered.find(e.first);
                        const int64_t advertisedSeq = known == m_discovered.end() ? -1 : known->second.seq;
                        if (n.discoveryOnly && advertisedSeq > n.pointerSeq &&
                            std::find(wanted.begin(), wanted.end(), e.first) == wanted.end()) {
                            wanted.push_back(e.first);
                        }
                        if ((n.wanted || n.requested) && !n.pointer.empty() && (!have || l.seq < n.pointerSeq)) {
                            retry.push_back(n.pointer);
                        }
                    }
                }
                for (const std::string &pk : wanted) {
                    meshGet(pk);
                }
                for (const std::string &raw : retry) {
                    meshReceivePointer(raw, "");
                }
            }
            return;
        }
        m_meshLast = now;
        for (const MeshList &l : m_mesh.lists()) {
            if (start) {
                meshSeed(l.pubkey, nullptr);
            }
            meshPut(l.pubkey);
            if (!l.own) {
                meshGet(l.pubkey);
            }
        }
        meshSyncMetaSubscriptions();
        std::vector<std::string> wanted;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            for (const auto &e : m_meshNet) {
                if (e.second.wanted && !e.second.fetching.is_valid()) {
                    wanted.push_back(e.first);
                }
            }
        }
        for (const std::string &pk : wanted) {
            meshGet(pk);
        }
    }

    // a version released here: seeded and announced
    void Core::meshReleased(const std::string &pk) {
        std::lock_guard<std::recursive_mutex> lifecycle(m_meshLifecycle);
        meshSeed(pk, nullptr);
        meshPut(pk);
        meshSyncMetaSubscriptions();
    }

    json Core::meshState(const std::string &pk) {
        std::lock_guard<std::mutex> lock(m_meshMutex);
        auto it = m_meshNet.find(pk);
        if (it == m_meshNet.end()) {
            return json{{"seeding", false}, {"fetching", false}, {"wanted", false}, {"meta_child", false}};
        }
        json j = {{"seeding", it->second.seeding.is_valid()}, {"fetching", it->second.fetching.is_valid()},
                  {"wanted", it->second.wanted}, {"meta_child", !it->second.metaParents.empty()},
                  {"selective", true}, {"records_ready", it->second.recordsReady},
                  {"media_pending", it->second.mediaDeadlines.size()}, {"error", it->second.netError}};
        if (!it->second.versionIh.is_all_zeros()) {
            j["version_ih"] = hexOf(it->second.versionIh.data(), 32);
        }
        return j;
    }

}

namespace p2p {

    namespace {
        class CoreHooks final : public MeshExtension::Hooks {
        public:
            explicit CoreHooks(Core *core) : m_core(core) {}

            lt::info_hash_t directory() override { return m_core->meshDirectoryHash(); }

            std::vector<std::pair<std::string, int64_t>> held() override { return m_core->meshHeld(); }

            std::vector<std::pair<std::string, int64_t>> requests() override { return m_core->meshRequests(); }
            std::vector<std::pair<std::string, int64_t>> known() override { return m_core->meshKnown(); }
            void knownReceived(const std::string &peer,
                               const std::vector<std::pair<std::string, int64_t>> &lists) override {
                m_core->meshKnownDiscovered(peer, lists);
            }
            bool item(const std::string &key, std::string &raw) override { return m_core->meshPeerItem(key, raw); }
            void itemReceived(const std::string &raw, const std::string &peer, const std::string &source) override {
                m_core->meshPeerReceived(raw, peer, source);
            }

            void received(const std::string &peer, const std::vector<std::pair<std::string, int64_t>> &lists) override {
                m_core->meshDiscovered(peer, lists);
            }

        private:
            Core *m_core;
        };
    }

    // the directory swarm: a fixed tiny torrent (one file "mediamesh.v1", v1, 16 KiB pieces:
    // the same infohash on every node) all the nodes seed; its peers tell each other their lists
    void Core::meshDirectory() {
        m_session->add_extension(MeshExtension::make(std::make_shared<CoreHooks>(this)));
        const std::string dir = m_store.dir() + "/lists/.directory";
        mkdir((m_store.dir() + "/lists").c_str(), 0777);
        mkdir(dir.c_str(), 0777);
        const std::string content = "MediaMesh v1 directory: the nodes holding lists meet here.\n";
        if (readAll(dir + "/mediamesh.v1") != content) {
            writeAll(dir + "/mediamesh.v1", content);
        }
        lt::file_storage fs;
        lt::add_files(fs, dir + "/mediamesh.v1");
        lt::create_torrent ct(fs, 16 * 1024, lt::create_torrent::v1_only);
        lt::error_code ec;
        lt::set_piece_hashes(ct, dir, ec);
        if (ec) {
            log("[Mesh] no directory swarm: %s", ec.message().c_str());
            return;
        }
        std::vector<char> buf;
        lt::bencode(std::back_inserter(buf), ct.generate());
        auto ti = std::make_shared<lt::torrent_info>(buf, ec, lt::from_span);
        if (ec) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_directoryMutex);
            m_directoryHash = ti->info_hashes();
        }
        // nobody holds its data (an empty directory, the file not wanted): a seed asks the DHT
        // for leechers only and would meet no other node; a finished torrent that is no seed
        // meets them all
        lt::add_torrent_params atp;
        atp.ti = ti;
        atp.save_path = dir + "/none";
        atp.file_priorities = {lt::dont_download};
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
        m_directory = m_session->add_torrent(std::move(atp), m_meshError);
        log("[Mesh] directory swarm %s", hexOf(ti->info_hashes().v1.data(), 20).c_str());
    }

    lt::info_hash_t Core::meshDirectoryHash() {
        std::lock_guard<std::mutex> lock(m_directoryMutex);
        return m_directoryHash;
    }

    std::vector<std::pair<std::string, int64_t>> Core::meshHeld() {
        std::vector<std::pair<std::string, int64_t>> out;
        for (const MeshList &l : m_mesh.lists()) {
            std::string pk(32, '\0');
            if (unhexTo(l.pubkey, &pk[0], 32)) {
                out.emplace_back(pk, l.seq);
            }
        }
        return out;
    }

    // HAVE lists are actual copies held by a peer. These alone contribute to holders.
    void Core::meshDiscovered(const std::string &peer, const std::vector<std::pair<std::string, int64_t>> &lists) {
        for (const auto &l : lists) {
            if (l.first.size() != 32 || l.second < 0) continue;
            const std::string pk = hexOf(l.first.data(), l.first.size());
            MeshList held;
            const bool have = m_mesh.list(pk, held);
            std::lock_guard<std::mutex> lock(m_meshMutex);
            if (m_discovered.size() >= 4096 && m_discovered.count(pk) == 0) continue;
            Discovered &d = m_discovered[pk];
            d.seq = std::max(d.seq, l.second);
            if (!peer.empty() && d.holders.size() < 256) d.holders.insert(peer);
            if (!peer.empty() && d.knowers.size() < 256) d.knowers.insert(peer);
            MeshNet &n = m_meshNet[pk];
            if (!have) n.discoveryOnly = true;
            if ((!have || held.seq < l.second) && n.pointerSeq < l.second &&
                !n.requested && !n.fetching.is_valid()) {
                n.requested = true;
                n.pendingGet = true;
            }
        }
    }

    // KNOWN carries transitive knowledge, not a claim that the sender holds content.
    void Core::meshKnownDiscovered(const std::string &peer, const std::vector<std::pair<std::string, int64_t>> &lists) {
        for (const auto &l : lists) {
            if (l.first.size() != 32 || l.second < 0) continue;
            const std::string pk = hexOf(l.first.data(), l.first.size());
            MeshList held;
            const bool have = m_mesh.list(pk, held);
            std::lock_guard<std::mutex> lock(m_meshMutex);
            if (m_discovered.size() >= 4096 && m_discovered.count(pk) == 0) continue;
            Discovered &d = m_discovered[pk];
            d.seq = std::max(d.seq, l.second);
            if (!peer.empty() && d.knowers.size() < 256) d.knowers.insert(peer);
            MeshNet &n = m_meshNet[pk];
            if (!have) n.discoveryOnly = true;
            // Compare the untrusted hint with the signed sequence, but never adopt it as a floor.
            if ((!have || held.seq < l.second) && n.pointerSeq < l.second &&
                !n.requested && !n.fetching.is_valid()) {
                n.requested = true;
                n.pendingGet = true;
            }
        }
    }

    std::vector<std::pair<std::string, int64_t>> Core::meshKnown() {
        std::map<std::string, int64_t> snapshot;
        for (const MeshList &list : m_mesh.lists()) {
            std::string key(32, '\0');
            if (unhexTo(list.pubkey, &key[0], 32)) snapshot[key] = list.seq;
        }
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            for (const auto &entry : m_discovered) {
                std::string key(32, '\0');
                if (!unhexTo(entry.first, &key[0], 32)) continue;
                auto it = snapshot.find(key);
                if (it == snapshot.end()) snapshot.emplace(std::move(key), entry.second.seq);
                else it->second = std::max(it->second, entry.second.seq);
            }
        }
        std::vector<std::pair<std::string, int64_t>> out;
        out.reserve(std::min<size_t>(snapshot.size(), 4096));
        for (const auto &entry : snapshot) {
            if (out.size() >= 4096) break;
            out.push_back(entry);
        }
        return out;
    }

    json Core::meshDirectoryJson() {
        json out = json::array();
        const auto localLists = m_mesh.lists();
        std::lock_guard<std::mutex> lock(m_meshMutex);
        for (const MeshList &list : localLists) {
            if (m_discovered.size() >= 4096 && m_discovered.count(list.pubkey) == 0) continue;
            Discovered &d = m_discovered[list.pubkey];
            d.seq = std::max(d.seq, list.seq);
            if (list.seq >= d.nameSeq) {
                d.name = list.name;
                d.nameSeq = list.seq;
            }
        }
        for (const auto &d : m_discovered) {
            MeshList held;
            const bool have = m_mesh.list(d.first, held);
            out.push_back({{"pubkey", d.first},
                           {"seq", d.second.seq},
                           {"holders", d.second.holders.size()},
                           {"known_by", d.second.knowers.size()},
                           {"subscribed", have},
                           {"name", have ? held.name : d.second.name}});
        }
        // Popularity remains based on HAVE (actual held copies), never KNOWN.
        std::sort(out.begin(), out.end(), [](const json &a, const json &b) { return a["holders"] > b["holders"]; });
        return out;
    }

}
