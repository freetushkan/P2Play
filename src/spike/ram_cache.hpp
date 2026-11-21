#pragma once

// RAM storage for libtorrent (a disk_interface): pieces live in memory up to a budget,
// the oldest verified ones are dropped past it and the torrent is told so
// (lt::torrent_lost_piece, patches/libtorrent/0006): it downloads them again when they
// are wanted. Pieces near a reader (a stream position) are dropped last. The stream
// reads the verified pieces from here directly.

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <libtorrent/session_params.hpp>
#include <libtorrent/sha1_hash.hpp>

namespace p2p {

    class RamCache {
    public:
        struct Stats {
            int64_t bytes = 0;
            int64_t capacity = 0;
            int pieces = 0;
            int64_t evicted = 0;
        };

        // a verified piece, kept alive while used even if dropped from the cache meanwhile
        using Piece = std::shared_ptr<const std::vector<char>>;

        explicit RamCache(int64_t capacity);
        ~RamCache();

        RamCache(const RamCache &) = delete;
        RamCache &operator=(const RamCache &) = delete;

        // for lt::session_params::disk_io_constructor; the cache outlives the session
        lt::disk_io_constructor_type constructor();

        // the piece if it is verified and in memory
        Piece piece(const lt::sha1_hash &infoHash, int piece) const;

        // pieces [first, last] of a torrent are read now: dropped last. id: the reader's
        // own number (a stream connection); a negative first removes the reader
        void setReader(const lt::sha1_hash &infoHash, int id, int first, int last);

        Stats stats() const;

        struct State;

    private:
        std::shared_ptr<State> m_state;
    };

}
