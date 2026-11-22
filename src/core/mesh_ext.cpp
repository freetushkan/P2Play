// MediaMesh BEP 10 directory protocol: have, get and signed BEP 44 item.
#include "mesh_ext.hpp"

#include <algorithm>
#include <map>
#include <set>

#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/bt_peer_connection.hpp>
#include <libtorrent/kademlia/item.hpp>
#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/torrent_handle.hpp>

namespace p2p {

    std::string MeshExtension::encodeItem(const Item &item) {
        lt::entry e(lt::entry::dictionary_t);
        e["k"] = item.key;
        e["salt"] = item.salt;
        e["seq"] = item.seq;
        e["sig"] = item.signature;
        e["v"] = item.value;
        std::string out;
        lt::bencode(std::back_inserter(out), e);
        return out;
    }

    bool MeshExtension::decodeItem(const std::string &raw, Item &item) {
        if (raw.empty() || raw.size() > kMaxItem) {
            return false;
        }
        lt::error_code ec;
        auto e = lt::bdecode(raw, ec, nullptr, 8, 64);
        if (ec || e.type() != lt::bdecode_node::dict_t || e.dict_size() != 5 ||
            e.data_section().size() != static_cast<int>(raw.size())) {
            return false;
        }
        auto seq = e.dict_find_int("seq");
        auto v = e.dict_find_dict("v");
        Item parsed;
        parsed.key = std::string(e.dict_find_string_value("k"));
        parsed.salt = std::string(e.dict_find_string_value("salt"));
        parsed.signature = std::string(e.dict_find_string_value("sig"));
        if (parsed.key.size() != 32 || parsed.salt != "mediamesh.v1" ||
            parsed.signature.size() != 64 || !seq || seq.int_value() < 0 || !v || v.dict_size() != 2 ||
            v.dict_find_string_value("ih").size() != 32 || !v.dict_find_string("n") ||
            v.dict_find_string_value("n").size() > 64 || v.data_section().size() > 1000) {
            return false;
        }
        const auto ih = v.dict_find_string_value("ih");
        if (std::all_of(ih.begin(), ih.end(), [](char c) { return c == 0; })) {
            return false;
        }
        parsed.seq = seq.int_value();
        parsed.value = v;
        // Reject duplicate keys, noncanonical encodings and trailing bytes. Signatures
        // must cover the exact canonical value that will be republished to the DHT.
        if (encodeItem(parsed) != raw || !lt::dht::verify_mutable_item(v.data_section(), parsed.salt,
                lt::dht::sequence_number(parsed.seq), lt::dht::public_key(parsed.key.data()),
                lt::dht::signature(parsed.signature.data()))) {
            return false;
        }
        item = std::move(parsed);
        return true;
    }

    namespace {
        constexpr const char *kName = "mediamesh";
        constexpr int kIndex = 23;
        constexpr int kMaxMessage = 512 * 1024;
        constexpr int kMaxHave = 4096;
        constexpr int kMaxRequests = 32;

        class MeshPeer final : public lt::peer_plugin {
        public:
            MeshPeer(lt::peer_connection_handle pc, std::shared_ptr<MeshExtension::Hooks> hooks)
                    : m_pc(std::move(pc)), m_hooks(std::move(hooks)) {}

            lt::string_view type() const override { return kName; }

            void add_handshake(lt::entry &h) override { h["m"][kName] = kIndex; }

            bool on_extension_handshake(lt::bdecode_node const &h) override {
                auto m = h.type() == lt::bdecode_node::dict_t ? h.dict_find_dict("m") : lt::bdecode_node();
                const int64_t id = m ? m.dict_find_int_value(kName, 0) : 0;
                m_their = id > 0 && id <= 255 ? static_cast<int>(id) : 0;
                if (m_their) {
                    const auto remote = m_pc.remote();
                    m_source = remote.address().to_string();
                    const int64_t port = h.dict_find_int_value("p", remote.port());
                    if (port > 0 && port <= 65535) {
                        const std::string address = remote.address().to_string();
                        m_endpoint = (remote.address().is_v6() ? "[" + address + "]" : address) + ":" + std::to_string(port);
                    }
                    sendHave(true);
                    requestItems();
                }
                return true;
            }

            bool on_extended(int length, int msg, lt::span<char const> body) override {
                if (msg != kIndex) {
                    return false;
                }
                if (length < 0 || length > kMaxMessage) {
                    m_pc.disconnect(lt::errors::invalid_message, lt::operation_t::bittorrent);
                    return true;
                }
                if (body.size() < length || !m_their) {
                    return true;
                }
                lt::error_code ec;
                auto d = lt::bdecode(body.first(length), ec, nullptr, 8, 17000);
                if (ec || d.type() != lt::bdecode_node::dict_t || d.data_section().size() != length) {
                    return true;
                }
                auto have = d.dict_find_list("have");
                if (have && d.dict_size() == 1 && have.list_size() <= kMaxHave && m_haveBudget > 0) {
                    --m_haveBudget;
                    std::vector<std::pair<std::string, int64_t>> lists;
                    std::map<std::string, int64_t> remote;
                    for (int i = 0; i < have.list_size(); ++i) {
                        auto e = have.list_at(i);
                        if (e.type() != lt::bdecode_node::list_t || e.list_size() != 2 ||
                            e.list_at(0).type() != lt::bdecode_node::string_t || e.list_at(0).string_length() != 32 ||
                            e.list_at(1).type() != lt::bdecode_node::int_t || e.list_at(1).int_value() < 0) {
                            return true;
                        }
                        std::string key(e.list_at(0).string_value());
                        const int64_t seq = e.list_at(1).int_value();
                        if (!remote.emplace(key, seq).second) {
                            return true;
                        }
                        lists.emplace_back(std::move(key), seq);
                    }
                    m_remoteHave = std::move(remote);
                    const auto &id = m_pc.pid();
                    m_hooks->received(std::string(id.data(), id.size()), lists);
                    requestItems();
                    return true;
                }
                auto get = d.dict_find_string("get");
                auto seq = d.dict_find_int("seq");
                if (get && get.string_length() == 32 && seq && seq.int_value() >= -1 &&
                    d.dict_size() == 2 && length <= 128 && m_getBudget > 0) {
                    --m_getBudget;
                    std::string raw;
                    if (m_hooks->item(std::string(get.string_value()), raw) && raw.size() <= MeshExtension::kMaxItem) {
                        // Cached envelopes were verified/created by the worker, not this thread.
                        auto p = lt::bdecode(raw, ec);
                        if (!ec && p.dict_find_int_value("seq", -1) > seq.int_value()) {
                            lt::entry reply(lt::entry::dictionary_t);
                            reply["item"] = p;
                            send(reply);
                        }
                    }
                    return true;
                }
                auto item = d.dict_find_dict("item");
                if (item && d.dict_size() == 1 && length <= MeshExtension::kMaxItem + 16 && m_itemBudget > 0) {
                    --m_itemBudget;
                    const std::string key(item.dict_find_string_value("k"));
                    auto pending = m_pending.find(key);
                    const auto seq = item.dict_find_int("seq");
                    const auto advertised = m_remoteHave.find(key);
                    const bool pushed = seq && advertised != m_remoteHave.end() &&
                                        seq.int_value() == advertised->second;
                    if (pending != m_pending.end() || pushed) {
                        const auto bytes = item.data_section();
                        m_hooks->itemReceived(std::string(bytes.data(), bytes.size()), m_endpoint, m_source);
                        if (pending != m_pending.end()) {
                            m_pending.erase(pending);
                        }
                    }
                }
                return true;
            }

            void tick() override {
                ++m_ticks;
                m_getBudget = m_itemBudget = kMaxRequests;
                m_sendItemBudget = kMaxRequests;
                m_haveBudget = 2;
                if (m_their) {
                    if (m_ticks % 10 == 0) {
                        sendHave();
                    }
                    sendQueuedItems();
                    if (m_ticks % 2 == 0) {
                        requestItems();
                    }
                }
            }

        private:
            void requestItems() {
                if (!m_their) {
                    return;
                }
                for (auto it = m_pending.begin(); it != m_pending.end();) {
                    if (m_ticks - it->second >= 15) {
                        it = m_pending.erase(it);
                    } else {
                        ++it;
                    }
                }
                const auto requests = m_hooks->requests();
                for (size_t i = 0; i < requests.size() && m_pending.size() < kMaxRequests; ++i) {
                    const auto &r = requests[m_requestCursor++ % requests.size()];
                    if (r.first.size() != 32 || r.second < -1 || m_pending.count(r.first)) {
                        continue;
                    }
                    lt::entry msg(lt::entry::dictionary_t);
                    msg["get"] = r.first;
                    msg["seq"] = r.second;
                    send(msg);
                    m_pending.emplace(r.first, m_ticks);
                }
            }

            bool send(const lt::entry &msg) {
                std::vector<char> body;
                lt::bencode(std::back_inserter(body), msg);
                if (!m_their || body.size() > kMaxMessage) {
                    return false;
                }
                const std::uint32_t len = static_cast<std::uint32_t>(2 + body.size());
                char head[6] = {static_cast<char>(len >> 24), static_cast<char>(len >> 16), static_cast<char>(len >> 8),
                                static_cast<char>(len), static_cast<char>(lt::bt_peer_connection::msg_extended),
                                static_cast<char>(m_their)};
                m_pc.send_buffer(head, sizeof(head));
                m_pc.send_buffer(body.data(), static_cast<int>(body.size()));
                return true;
            }

            void sendQueuedItems() {
                for (auto it = m_pushPending.begin(); it != m_pushPending.end() && m_sendItemBudget > 0;) {
                    const std::string key = it->first;
                    const int64_t seq = it->second;
                    std::string raw;
                    if (m_hooks->item(key, raw) && raw.size() <= MeshExtension::kMaxItem) {
                        lt::error_code ec;
                        auto encoded = lt::bdecode(raw, ec, nullptr, 8, 64);
                        if (!ec && encoded.type() == lt::bdecode_node::dict_t) {
                            const std::string actualKey(encoded.dict_find_string_value("k"));
                            const auto actualSeq = encoded.dict_find_int("seq");
                            if (actualKey == key && actualSeq && actualSeq.int_value() == seq) {
                                lt::entry msg(lt::entry::dictionary_t);
                                msg["item"] = encoded;
                                if (send(msg)) {
                                    m_pushed[key] = seq;
                                }
                            }
                        }
                    }
                    it = m_pushPending.erase(it);
                    --m_sendItemBudget;
                }
            }

            // The initial inventory is pull-based. Later sequence changes are pushed,
            // but only after the new have snapshot has been sent on this connection.
            void sendHave(bool initial = false) {
                lt::entry msg(lt::entry::dictionary_t);
                auto &have = msg["have"].list();
                std::set<std::string> announced;
                for (const auto &l : m_hooks->held()) {
                    if (l.first.size() != 32 || l.second < 0) {
                        continue;
                    }
                    if (have.size() >= kMaxHave) {
                        break;
                    }
                    announced.insert(l.first);
                    const auto pushed = m_pushed.find(l.first);
                    const int64_t last = pushed == m_pushed.end() ? -1 : pushed->second;
                    if (initial) {
                        m_pushed[l.first] = std::max(last, l.second);
                    } else if (l.second > last) {
                        m_pushPending[l.first] = l.second;
                    }
                    lt::entry::list_type item;
                    item.emplace_back(l.first);
                    item.emplace_back(l.second);
                    have.emplace_back(std::move(item));
                }
                for (auto it = m_pushed.begin(); it != m_pushed.end();) {
                    if (!announced.count(it->first)) it = m_pushed.erase(it);
                    else ++it;
                }
                for (auto it = m_pushPending.begin(); it != m_pushPending.end();) {
                    if (!announced.count(it->first)) it = m_pushPending.erase(it);
                    else ++it;
                }
                std::vector<char> body;
                lt::bencode(std::back_inserter(body), msg);
                if (body != m_sent) {
                    m_sent = std::move(body);
                    send(msg);
                }
            }

            lt::peer_connection_handle m_pc;
            std::shared_ptr<MeshExtension::Hooks> m_hooks;
            int m_their = 0;
            int64_t m_ticks = 0;
            int m_getBudget = kMaxRequests, m_itemBudget = kMaxRequests, m_haveBudget = 2;
            int m_sendItemBudget = kMaxRequests;
            std::map<std::string, int64_t> m_pending;
            std::map<std::string, int64_t> m_remoteHave, m_pushed, m_pushPending;
            std::vector<char> m_sent;
            std::string m_source; // actual TCP source address; unlike BEP 10 p, it is not peer-advertised
            std::string m_endpoint;
            size_t m_requestCursor = 0;
        };

        class MeshTorrentPlugin final : public lt::torrent_plugin {
        public:
            explicit MeshTorrentPlugin(std::shared_ptr<MeshExtension::Hooks> hooks) : m_hooks(std::move(hooks)) {}
            std::shared_ptr<lt::peer_plugin> new_connection(lt::peer_connection_handle const &pc) override {
                return pc.type() == lt::connection_type::bittorrent ? std::make_shared<MeshPeer>(pc, m_hooks) : nullptr;
            }
        private:
            std::shared_ptr<MeshExtension::Hooks> m_hooks;
        };

        class MeshPlugin final : public lt::plugin {
        public:
            explicit MeshPlugin(std::shared_ptr<MeshExtension::Hooks> hooks) : m_hooks(std::move(hooks)) {}
            std::shared_ptr<lt::torrent_plugin> new_torrent(lt::torrent_handle const &h, lt::client_data_t) override {
                if (h.info_hashes() != m_hooks->directory()) {
                    return nullptr;
                }
                return std::make_shared<MeshTorrentPlugin>(m_hooks);
            }
        private:
            std::shared_ptr<MeshExtension::Hooks> m_hooks;
        };
    }

    std::shared_ptr<lt::plugin> MeshExtension::make(std::shared_ptr<Hooks> hooks) {
        return std::make_shared<MeshPlugin>(std::move(hooks));
    }
}
