// The "mediamesh" extension (BEP 10) in the directory swarm: right after the extension
// handshake each side sends "have", the lists it holds and their versions (again when they
// change), bencoded:
// {"have": [[pubkey (32 bytes), seq], ...]}. A node learns the lists of the network so (the
// directory, with how many peers hold each: the rating) and that a list it holds has a newer
// version. The directory swarm: a fixed tiny torrent every node seeds (mesh_net.cpp).

#include "mesh_ext.hpp"

#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/bt_peer_connection.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/torrent_handle.hpp>

namespace p2p {

    namespace {
        constexpr const char *kName = "mediamesh";
        constexpr int kIndex = 23;              // our id for the message
        constexpr int kMaxMessage = 512 * 1024;

        class MeshPeer final : public lt::peer_plugin {
        public:
            MeshPeer(lt::peer_connection_handle pc, std::shared_ptr<MeshExtension::Hooks> hooks)
                    : m_pc(std::move(pc)), m_hooks(std::move(hooks)) {}

            lt::string_view type() const override { return kName; }

            void add_handshake(lt::entry &h) override {
                h["m"][kName] = kIndex;
            }

            bool on_extension_handshake(lt::bdecode_node const &h) override {
                lt::bdecode_node m = h.type() == lt::bdecode_node::dict_t ? h.dict_find_dict("m") : lt::bdecode_node();
                m_their = m ? static_cast<int>(m.dict_find_int_value(kName, 0)) : 0;
                if (m_their > 0) {
                    sendHave();
                }
                return true;
            }

            bool on_extended(int length, int msg, lt::span<char const> body) override {
                if (msg != kIndex) {
                    return false;
                }
                if (length > kMaxMessage) {
                    m_pc.disconnect(lt::errors::invalid_message, lt::operation_t::bittorrent);
                    return true;
                }
                if (static_cast<int>(body.size()) < length) {
                    return true;  // the rest still coming
                }
                lt::error_code ec;
                lt::bdecode_node d = lt::bdecode(body, ec);
                lt::bdecode_node have = !ec && d.type() == lt::bdecode_node::dict_t ? d.dict_find_list("have") : lt::bdecode_node();
                if (!have) {
                    return true;
                }
                std::vector<std::pair<std::string, int64_t>> lists;
                for (int i = 0; i < have.list_size() && i < 4096; i++) {
                    lt::bdecode_node e = have.list_at(i);
                    if (e.type() == lt::bdecode_node::list_t && e.list_size() >= 2 && e.list_at(0).string_length() == 32) {
                        lists.emplace_back(std::string(e.list_at(0).string_value()), e.list_at(1).int_value());
                    }
                }
                // a node by its peer id (one node may come through several addresses)
                const lt::peer_id &id = m_pc.pid();
                m_hooks->received(std::string(id.data(), id.size()), lists);
                return true;
            }

            // a list taken or a version released since: told again (every 10 s at most)
            void tick() override {
                if (m_their > 0 && ++m_ticks % 10 == 0) {
                    sendHave();
                }
            }

        private:
            void sendHave() {
                lt::entry msg(lt::entry::dictionary_t);
                lt::entry::list_type &have = msg["have"].list();
                for (const auto &l : m_hooks->held()) {
                    lt::entry::list_type item;
                    item.emplace_back(l.first);
                    item.emplace_back(l.second);
                    have.emplace_back(std::move(item));
                }
                std::vector<char> body;
                lt::bencode(std::back_inserter(body), msg);
                if (body == m_sent) {
                    return;
                }
                m_sent = body;
                const std::uint32_t len = static_cast<std::uint32_t>(2 + body.size());
                char head[6] = {static_cast<char>(len >> 24), static_cast<char>(len >> 16), static_cast<char>(len >> 8),
                                static_cast<char>(len), static_cast<char>(lt::bt_peer_connection::msg_extended),
                                static_cast<char>(m_their)};
                m_pc.send_buffer(head, sizeof(head));
                m_pc.send_buffer(body.data(), static_cast<int>(body.size()));
            }

            lt::peer_connection_handle m_pc;
            std::shared_ptr<MeshExtension::Hooks> m_hooks;
            int m_their = 0;
            int m_ticks = 0;
            std::vector<char> m_sent;
        };

        class MeshTorrentPlugin final : public lt::torrent_plugin {
        public:
            explicit MeshTorrentPlugin(std::shared_ptr<MeshExtension::Hooks> hooks) : m_hooks(std::move(hooks)) {}

            std::shared_ptr<lt::peer_plugin> new_connection(lt::peer_connection_handle const &pc) override {
                if (pc.type() != lt::connection_type::bittorrent) {
                    return nullptr;
                }
                return std::make_shared<MeshPeer>(pc, m_hooks);
            }

        private:
            std::shared_ptr<MeshExtension::Hooks> m_hooks;
        };

        class MeshPlugin final : public lt::plugin {
        public:
            explicit MeshPlugin(std::shared_ptr<MeshExtension::Hooks> hooks) : m_hooks(std::move(hooks)) {}

            std::shared_ptr<lt::torrent_plugin> new_torrent(lt::torrent_handle const &h, lt::client_data_t) override {
                // the directory swarm only (its infohash known once it is made)
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
