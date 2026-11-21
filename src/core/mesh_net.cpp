// MediaMesh over the network (stage 3): a list version is a v2 torrent of the list's directory
// (<data>/lists/<pubkey>/: manifest.ben, records/), kept on disk and seeded (hybrid_disk.cpp);
// the pointer to the current version is a BEP 44 mutable item under the list's key, salt
// "mediamesh.v1", value {"ih": the version's v2 infohash (32 bytes), "n": the name}, seq = the
// version. Subscribing: the pointer from the DHT, the version downloaded beside the lists,
// checked (Mesh::accept) and seeded. Every node puts the pointers of its lists again now and
// then (signed by the key's owner, any node can store it again): a list lives on without its
// releaser.

#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/kademlia/ed25519.hpp>
#include <libtorrent/kademlia/item.hpp>
#include <libtorrent/magnet_uri.hpp>

#include "core_torrent.hpp"

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

        std::string readAll(const std::string &path) {
            std::ifstream in(path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
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
        lt::add_files(fs, root + "/" + pk, [](const std::string &p) {
            const std::string name = p.substr(p.rfind('/') + 1);
            return name != "pending.ndjson" && name != "pointer.ben" &&
                   !(name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0);
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

    // a list seeded: its current version's torrent (the previous one goes)
    void Core::meshSeed(const std::string &pk, std::shared_ptr<lt::torrent_info> ti) {
        std::string error;
        if (!ti) {
            ti = meshTorrent(pk, error);
        }
        if (!ti) {
            log("[Mesh] list %s not seeded: %s", pk.substr(0, 16).c_str(), error.c_str());
            return;
        }
        lt::add_torrent_params atp;
        atp.ti = ti;
        atp.save_path = m_store.dir() + "/lists";
        atp.flags |= lt::torrent_flags::seed_mode;
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
        lt::torrent_handle old;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            MeshNet &n = m_meshNet[pk];
            if (n.seeding.is_valid() && n.seeding.info_hashes() == ti->info_hashes()) {
                return;
            }
            old = n.seeding;
            n.versionIh = ti->info_hashes().v2;
            n.seeding = m_session->add_torrent(std::move(atp), m_meshError);
        }
        if (old.is_valid()) {
            m_session->remove_torrent(old);
        }
        log("[Mesh] list %s seeded: %s", pk.substr(0, 16).c_str(), hexOf(ti->info_hashes().v2.data(), 32).c_str());
    }

    // the pointer put into the DHT: ours signed with the list's key, a received one as received
    void Core::meshPut(const std::string &pk) {
        if (m_settings.disableDht) {
            return;
        }
        std::array<char, 32> key;
        if (!unhexTo(pk, key.data(), 32)) {
            return;
        }
        MeshList card;
        if (!m_mesh.list(pk, card)) {
            return;
        }
        const std::string dir = m_mesh.listDir(pk);
        std::string seedHex = readAll(m_store.dir() + "/keys/" + pk + ".seed");
        lt::sha256_hash ih;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            ih = m_meshNet[pk].versionIh;
        }
        if (card.own && seedHex.size() >= 64 && !ih.is_all_zeros()) {
            std::array<char, 32> seed;
            if (!unhexTo(seedHex.substr(0, 64), seed.data(), 32)) {
                return;
            }
            const int64_t seq = card.seq;
            const std::string name = card.name.substr(0, 64);
            const std::string ihBytes(ih.data(), ih.size());
            m_session->dht_put_item(key, [seed, seq, name, ihBytes, this, dir](lt::entry &e, std::array<char, 64> &sig,
                                                                             std::int64_t &s, std::string const &salt) {
                e = lt::entry(lt::entry::dictionary_t);
                e["ih"] = ihBytes;
                e["n"] = name;
                s = seq;
                std::vector<char> v;
                lt::bencode(std::back_inserter(v), e);
                lt::dht::public_key pk;
                lt::dht::secret_key sk;
                std::tie(pk, sk) = lt::dht::ed25519_create_keypair(seed);
                lt::dht::signature signature = lt::dht::sign_mutable_item(v, salt, lt::dht::sequence_number(seq), pk, sk);
                sig = signature.bytes;
                // kept as received ones are: put again by every node
                lt::entry kept(lt::entry::dictionary_t);
                kept["v"] = e;
                kept["sig"] = std::string(sig.data(), sig.size());
                kept["seq"] = seq;
                std::string out;
                lt::bencode(std::back_inserter(out), kept);
                writeAll(dir + "/pointer.ben", out);
            }, kSalt);
            return;
        }
        // a received pointer, signed by the list's key: put as it is
        const std::string raw = readAll(dir + "/pointer.ben");
        lt::error_code ec;
        lt::bdecode_node p = lt::bdecode(raw, ec);
        if (ec || p.type() != lt::bdecode_node::dict_t || p.dict_find_string_value("sig").size() != 64) {
            return;
        }
        lt::entry value;
        value = p.dict_find_dict("v");
        const std::string sigStr(p.dict_find_string_value("sig"));
        const int64_t seq = p.dict_find_int_value("seq", -1);
        m_session->dht_put_item(key, [value, sigStr, seq](lt::entry &e, std::array<char, 64> &sig, std::int64_t &s,
                                                          std::string const &) {
            e = value;
            std::copy(sigStr.begin(), sigStr.end(), sig.begin());
            s = seq;
        }, kSalt);
    }

    // the current pointer asked from the DHT (the answer: meshAlert)
    void Core::meshGet(const std::string &pk) {
        std::array<char, 32> key;
        if (!m_settings.disableDht && unhexTo(pk, key.data(), 32)) {
            m_session->dht_get_item(key, kSalt);
        }
    }

    std::string Core::meshSubscribe(const std::string &pk, const std::vector<std::string> &peers) {
        std::array<char, 32> key;
        if (!unhexTo(pk, key.data(), 32)) {
            return "a list key is 64 hex digits";
        }
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            MeshNet &n = m_meshNet[pk];
            n.wanted = true;
            n.peers = peers;
        }
        log("[Mesh] list %s: asking the DHT for its version", pk.substr(0, 16).c_str());
        meshGet(pk);
        return "";
    }

    // a version to fetch: its torrent downloaded into lists/.in, then checked and taken
    void Core::meshFetch(const std::string &pk, const lt::sha256_hash &ih, int64_t seq, const std::string &pointer) {
        const std::string staging = m_store.dir() + "/lists/.in/" + pk + "-" + std::to_string(seq);
        mkdir((m_store.dir() + "/lists/.in").c_str(), 0777);
        mkdir(staging.c_str(), 0777);
        lt::add_torrent_params atp;
        atp.info_hashes = lt::info_hash_t(ih);
        atp.name = pk;
        atp.save_path = staging;
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
        std::vector<std::string> peers;
        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            MeshNet &n = m_meshNet[pk];
            if (n.fetching.is_valid()) {
                if (n.fetchSeq >= seq) {
                    return;
                }
                m_session->remove_torrent(n.fetching, lt::session::delete_files);
            }
            n.fetchSeq = seq;
            n.fetchPointer = pointer;
            n.fetchDir = staging;
            peers = n.peers;
            n.fetching = m_session->add_torrent(std::move(atp), m_meshError);
            for (const std::string &p : peers) {
                const size_t colon = p.rfind(':');
                lt::error_code ec;
                auto addr = lt::make_address(p.substr(0, colon), ec);
                if (!ec && colon != std::string::npos) {
                    n.fetching.connect_peer(lt::tcp::endpoint(addr, static_cast<std::uint16_t>(std::atoi(p.c_str() + colon + 1))));
                }
            }
        }
        log("[Mesh] list %s: fetching version %lld", pk.substr(0, 16).c_str(), static_cast<long long>(seq));
    }

    // the DHT's answers and the fetched versions; true: a MediaMesh alert
    bool Core::meshAlert(lt::alert *a) {
        if (auto *mi = lt::alert_cast<lt::dht_mutable_item_alert>(a)) {
            if (mi->salt != kSalt || mi->item.type() != lt::entry::dictionary_t) {
                return true;
            }
            const std::string pk = hexOf(mi->key.data(), mi->key.size());
            MeshList held;
            const bool have = m_mesh.list(pk, held);
            const lt::entry *ih = mi->item.find_key("ih");
            if (!ih || ih->type() != lt::entry::string_t || ih->string().size() != 32) {
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(m_meshMutex);
                auto it = m_meshNet.find(pk);
                if (!have && (it == m_meshNet.end() || !it->second.wanted)) {
                    return true;  // not a list of ours
                }
            }
            if (have && held.seq >= mi->seq) {
                return true;
            }
            lt::entry kept(lt::entry::dictionary_t);
            kept["v"] = mi->item;
            kept["sig"] = std::string(mi->signature.data(), mi->signature.size());
            kept["seq"] = mi->seq;
            std::string pointer;
            lt::bencode(std::back_inserter(pointer), kept);
            meshFetch(pk, lt::sha256_hash(ih->string().data()), mi->seq, pointer);
            return true;
        }
        if (auto *tf = lt::alert_cast<lt::torrent_finished_alert>(a)) {
            std::string pk, dir, pointer;
            lt::torrent_handle h;
            {
                std::lock_guard<std::mutex> lock(m_meshMutex);
                for (auto &e : m_meshNet) {
                    if (e.second.fetching.is_valid() && e.second.fetching == tf->handle) {
                        pk = e.first;
                        dir = e.second.fetchDir;
                        pointer = e.second.fetchPointer;
                        h = e.second.fetching;
                        e.second.fetching = lt::torrent_handle();
                    }
                }
            }
            if (pk.empty()) {
                return false;
            }
            // the version checked and taken by the worker: not on the alert thread
            std::thread([this, pk, dir, pointer, h] {
                std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();
                m_session->remove_torrent(h);
                const std::string err = m_mesh.accept(dir + "/" + pk);
                if (!err.empty()) {
                    log("[Mesh] list %s: version refused: %s", pk.substr(0, 16).c_str(), err.c_str());
                } else {
                    writeAll(m_mesh.listDir(pk) + "/pointer.ben", pointer);
                    meshSeed(pk, ti ? std::make_shared<lt::torrent_info>(*ti) : nullptr);
                }
                removeTree(dir);
            }).detach();
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
        const auto now = std::chrono::steady_clock::now();
        if (!start && now - m_meshLast < std::chrono::minutes(30)) {
            // a list subscribed to, no version yet: asked again every 15 s
            if (now - m_meshWantedLast > std::chrono::seconds(15)) {
                m_meshWantedLast = now;
                std::vector<std::string> wanted;
                {
                    std::lock_guard<std::mutex> lock(m_meshMutex);
                    for (const auto &e : m_meshNet) {
                        MeshList l;
                        if (e.second.wanted && !e.second.fetching.is_valid() && !m_mesh.list(e.first, l)) {
                            wanted.push_back(e.first);
                        }
                    }
                }
                for (const std::string &pk : wanted) {
                    meshGet(pk);
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
        meshSeed(pk, nullptr);
        meshPut(pk);
    }

    json Core::meshState(const std::string &pk) {
        std::lock_guard<std::mutex> lock(m_meshMutex);
        auto it = m_meshNet.find(pk);
        if (it == m_meshNet.end()) {
            return json{{"seeding", false}};
        }
        json j = {{"seeding", it->second.seeding.is_valid()}, {"fetching", it->second.fetching.is_valid()}};
        if (!it->second.versionIh.is_all_zeros()) {
            j["version_ih"] = hexOf(it->second.versionIh.data(), 32);
        }
        return j;
    }

}
