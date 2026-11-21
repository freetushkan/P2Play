#pragma once

// MediaMesh lists, the local half (stage 3): a list is an ed25519 key; a version is a signed
// manifest plus append-only segments of records. Here: the lists of this node (made here or
// received), checked, merged into one index and searched; publishing (the releaser: records
// in, a version out). The network half (the version as a v2 torrent, the BEP 44 pointer, the
// mediamesh extension) builds on these files. Independent of the streamer.
//
// On disk, <data>/lists/<pubkey hex>/:
//   manifest.ben  bencoded dict: "v" 1, "name", "desc", "pk" (32 bytes), "seq",
//                 "segs" [{"f" file, "n" records, "h" sha256}], "sig" (64 bytes): ed25519 of
//                 the bencoded dict without "sig" (bencode is canonical: any client can check)
//   records/seg-NNNNNN.z  zlib of NDJSON records (zlib, not zstd: our own inflate runs on
//                 every target): {"ih", "title", "size"?, "files"?, "date"?, "tags"?,
//                 "trackers"?, "poster"?}; a removal: {"del": ih, "date"}
//   pending.ndjson  the releaser's records waiting for the next version
// <data>/keys/<pubkey hex>.seed: the 32 byte seed of a list made here (the releaser's).

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../../third_party/nlohmann/json.hpp"

namespace p2p {

    struct MeshRecord {
        std::string ih;       // hex, lower case (v1 40, or v2 64)
        std::string title;
        int64_t size = 0;
        int files = 0;
        int64_t date = 0;     // unix time
        std::vector<std::string> tags;
        std::vector<std::string> trackers;
        std::string poster;   // a URL until the media packs
        std::set<std::string> lists;   // the lists holding it

        nlohmann::json json() const;
    };

    struct MeshList {
        std::string pubkey;   // hex
        std::string name;
        std::string desc;
        int64_t seq = 0;
        int records = 0;
        int64_t updated = 0;  // when this version was taken
        bool own = false;     // its key is here
    };

    class Mesh {
    public:
        // the client's hard limits: a version past them is refused
        static constexpr int kMaxSegments = 4096;
        static constexpr size_t kMaxSegmentBytes = 64u << 20;
        static constexpr size_t kMaxLine = 64u << 10;

        void load(const std::string &dataDir, std::function<void(const std::string &)> log);

        std::vector<MeshList> lists() const;

        bool list(const std::string &pubkey, MeshList &out) const;

        // search: every word in the title or the tags (case folded, Latin and Cyrillic); a tag
        // filters; the newest first among equal matches
        std::vector<MeshRecord> search(const std::string &query, const std::string &tag, size_t limit) const;

        bool record(const std::string &ih, MeshRecord &out) const;

        // the releaser: a new list (its key made here); "" on success, else the error
        std::string create(const std::string &name, const std::string &desc, std::string &pubkey);

        // records (NDJSON) waiting for the next version; the count taken, -1: not our list
        int addPending(const std::string &pubkey, const std::string &ndjson, std::string &error);

        int pending(const std::string &pubkey) const;

        // the pending records as a new segment, seq + 1, the manifest signed
        std::string release(const std::string &pubkey, int64_t &seq);

        // a list received (a version directory): checked (signature, seq, hashes, limits),
        // then taken in place of an older version
        std::string accept(const std::string &dir);

        std::string remove(const std::string &pubkey);

        std::string listDir(const std::string &pubkey) const;

    private:
        struct Loaded {
            MeshList card;
            std::vector<MeshRecord> records;   // after the removals
        };

        // a version directory read and checked
        std::string read(const std::string &dir, Loaded &out) const;

        void rebuild();

        std::string m_dir;
        std::function<void(const std::string &)> m_log;
        mutable std::mutex m_mutex;
        std::map<std::string, Loaded> m_lists;
        std::map<std::string, MeshRecord> m_index;   // ih -> merged
    };

    // case folding for the search: ASCII and Cyrillic (UTF-8) to lower case, ё as е
    std::string meshFold(const std::string &s);

}
