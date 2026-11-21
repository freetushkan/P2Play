#pragma once

// The "mediamesh" BEP 10 extension of the directory swarm (mesh_ext.cpp).

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libtorrent/extensions.hpp>
#include <libtorrent/info_hash.hpp>

namespace p2p {

    struct MeshExtension {
        struct Hooks {
            virtual ~Hooks() = default;
            // the directory swarm's infohash
            virtual lt::info_hash_t directory() = 0;
            // the lists this node holds: pubkey (32 bytes), seq
            virtual std::vector<std::pair<std::string, int64_t>> held() = 0;
            // a node's lists (peer: its peer id)
            virtual void received(const std::string &peer, const std::vector<std::pair<std::string, int64_t>> &lists) = 0;
        };

        static std::shared_ptr<lt::plugin> make(std::shared_ptr<Hooks> hooks);
    };

}
