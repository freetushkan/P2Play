#pragma once

// Signed MediaMesh v1 manifests, append-only record shards and optional lazy media packs.
// The canonical bencoded manifest (everything except sig) is signed with ed25519.
// Record segments retain the portable built-in zlib format; media packs contain raw bytes.
// See docs/media-shards.md for the wire format, bounds and network integration contract.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../../third_party/nlohmann/json.hpp"

namespace p2p {
    struct MeshMediaRef {
        std::string pack, sha256, type;
        int64_t offset = 0, length = 0;
        bool empty() const { return pack.empty(); }
        nlohmann::json json() const;
    };

    struct MeshRecord {
        std::string ih, title;
        int64_t size = 0;
        int files = 0;
        int64_t date = 0;
        std::vector<std::string> tags, trackers;
        std::string poster; // legacy URL; never fetched by the native catalog
        std::string shard = "default";
        MeshMediaRef descriptionRef, posterRef;
        std::set<std::string> lists;
        nlohmann::json json() const;
    };

    struct MeshMetaItem {
        std::string pubkey, annotation;
        bool enabled = true;
    };

    struct MeshList {
        std::string pubkey, name, desc;
        int64_t seq = 0;
        int records = 0;
        int64_t updated = 0;
        bool own = false;
        bool meta = false;
        std::vector<MeshMetaItem> items;
    };

    // Local state, not part of a signed version or its torrent. Empty shards means none;
    // {"*"} means all. Budgets apply per list, including already-present selected files.
    struct MeshPolicy {
        std::set<std::string> shards{"*"};
        int64_t recordBytes = 256ll << 20;
        int64_t mediaBytes = 256ll << 20;
        int64_t cacheBytes = 32ll << 20;
        bool mirrorMedia = false; // eagerly retain packs, admitted within mediaBytes
        nlohmann::json json() const;
    };

    struct MeshFile {
        std::string path, sha256, shard, kind;
        int64_t bytes = 0;
        int64_t records = 0;
        bool signedSize = true; // legacy record descriptors may omit b
        bool selected = true;
        bool present = false;
    };

    class Mesh {
    public:
        static constexpr int kMaxSegments = 4096;
        static constexpr size_t kMaxSegmentBytes = 64u << 20;
        static constexpr size_t kMaxLine = 64u << 10;
        static constexpr size_t kMaxPackBytes = 16u << 20;
        static constexpr size_t kMaxDescriptionBytes = 64u << 10;
        static constexpr size_t kMaxPosterBytes = 2u << 20;
        static constexpr size_t kMaxVersionTextBytes = 256u << 20;
        static constexpr size_t kMaxMetaItems = 256;
        static constexpr size_t kMaxMetaAnnotationBytes = 2048;

        void load(const std::string &dataDir, std::function<void(const std::string &)> log,
                  bool cacheRecordsInMemory = true);
        // Keep only list cards and signed file descriptors in memory. Records remain on disk
        // and are read when searched or requested; changing this at runtime is supported.
        void setRecordCache(bool enabled);
        std::vector<MeshList> lists() const;
        bool list(const std::string &pubkey, MeshList &out) const;
        std::vector<MeshRecord> search(const std::string &query, const std::string &tag, size_t limit,
                                        const std::set<std::string> &lists = {}) const;
        bool record(const std::string &ih, MeshRecord &out) const;
        std::string create(const std::string &name, const std::string &desc, std::string &pubkey, bool meta = false);
        int addPending(const std::string &pubkey, const std::string &ndjson, std::string &error);
        std::string replace(const std::string &pubkey, const std::string &ndjson,
                            int64_t &seq, int &inputRecords);
        int pending(const std::string &pubkey) const;
        std::string setMetaItems(const std::string &pubkey, const std::vector<MeshMetaItem> &items);
        bool hasPendingMeta(const std::string &pubkey) const;
        std::string release(const std::string &pubkey, int64_t &seq);
        std::string accept(const std::string &dir, const std::string &expectedPk = "", int64_t expectedSeq = -1,
                           bool rejectMetaChild = false);
        std::string remove(const std::string &pubkey);
        std::string listDir(const std::string &pubkey) const;

        // Append to an unpublished pack outside lists/. Return a ref to use in a record.
        std::string addMedia(const std::string &pubkey, const std::string &kind,
                             const std::string &type, const std::string &bytes, MeshMediaRef &ref);
        // No URL fetching and no eager media reads during load/search. Both pack and slice
        // hashes are checked before serving/caching. "media unavailable" means fetch plan().
        std::string media(const std::string &pubkey, const std::string &ih, const std::string &kind,
                          std::string &bytes, std::string &type);
        std::string mediaPath(const std::string &pubkey, const std::string &ih, const std::string &kind) const;
        MeshPolicy policy(const std::string &pubkey) const;
        std::string setPolicy(const std::string &pubkey, const MeshPolicy &policy);
        std::vector<MeshFile> plan(const std::string &pubkey) const;
        // Verify a downloaded selected record segment / requested media pack before install.
        std::string installFile(const std::string &pubkey, const std::string &path, const std::string &source);
        // Owner-only: replace history by live records and repack/deduplicate live media slices.
        std::string compact(const std::string &pubkey, int64_t &seq);

    private:
        struct Loaded {
            MeshList card;
            std::vector<MeshRecord> records;
            std::vector<MeshFile> files;
        };
        std::string read(const std::string &dir, Loaded &out, const std::string *manifest = nullptr,
                         const MeshPolicy *policyOverride = nullptr) const;
        std::string publish(const std::string &pubkey, bool snapshot, int64_t &seq,
                            const std::string *replacement = nullptr, int *inputRecords = nullptr);
        void rebuild();
        void prune(const std::string &pubkey, const Loaded &version, const MeshPolicy &policy);
        std::string m_dir;
        std::function<void(const std::string &)> m_log;
        mutable std::mutex m_mutex;
        mutable std::mutex m_writeMutex; // serialize disk transactions, never held by network code
        std::map<std::string, Loaded> m_lists;
        std::map<std::string, MeshRecord> m_index;
        bool m_cacheRecordsInMemory = true;
    };
    std::string meshFold(const std::string &s);
}
