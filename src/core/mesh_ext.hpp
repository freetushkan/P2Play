#pragma once

// The "mediamesh" BEP 10 extension of the directory swarm (mesh_ext.cpp).

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libtorrent/entry.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/info_hash.hpp>

namespace p2p {

    struct MeshExtension {
        // A BEP 44 mutable pointer, transported unchanged over BEP 10 or DHT.
        struct Item {
            std::string key, salt, signature; // 32, "mediamesh.v1", 64 bytes
            int64_t seq = -1;
            lt::entry value;
        };
        static constexpr int kMaxItem = 2048;
        // Strict shape/size checks, canonical value and BEP 44 signature verification.
        static bool decodeItem(const std::string &raw, Item &item);
        static std::string encodeItem(const Item &item);

        struct Hooks {
            virtual ~Hooks() = default;
            // the directory swarm's infohash
            virtual lt::info_hash_t directory() = 0;
            // the lists this node holds: pubkey (32 bytes), seq
            virtual std::vector<std::pair<std::string, int64_t>> held() = 0;
            // a node's lists (peer: its peer id)
            virtual void received(const std::string &peer, const std::vector<std::pair<std::string, int64_t>> &lists) = 0;
            // All hooks run on libtorrent's thread: snapshots/queues only, never session
            // calls, disk I/O or downloading. Requests are (key bytes, minimum known seq).
            virtual std::vector<std::pair<std::string, int64_t>> requests() = 0;
            virtual bool item(const std::string &key, std::string &raw) = 0;
            virtual void itemReceived(const std::string &raw, const std::string &peer) = 0;
        };

        static std::shared_ptr<lt::plugin> make(std::shared_ptr<Hooks> hooks);
    };

}
