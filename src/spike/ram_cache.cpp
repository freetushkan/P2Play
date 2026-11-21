#include "ram_cache.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <thread>

#include <boost/asio/post.hpp>
#include <libtorrent/disk_buffer_holder.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/io_context.hpp>
#include <libtorrent/piece_eviction.hpp>
#include <libtorrent/storage_defs.hpp>

namespace p2p {

    struct RamCache::State {
        struct Entry {
            std::shared_ptr<std::vector<char>> data;
            bool verified = false;
            uint64_t used = 0;
        };

        struct Store {
            lt::sha1_hash infoHash;
            const lt::file_storage *files = nullptr;
            std::weak_ptr<void> torrent;
            std::map<int, Entry> pieces;
        };

        struct Reader {
            lt::sha1_hash infoHash;
            int first = 0;
            int last = 0;
        };

        struct Lost {
            std::weak_ptr<void> torrent;
            int piece;
        };

        mutable std::mutex mutex;
        int64_t capacity = 0;
        int64_t bytes = 0;
        int64_t evicted = 0;
        uint64_t tick = 0;
        std::map<int, std::shared_ptr<Store>> stores;
        std::map<int, Reader> readers;

        std::shared_ptr<Store> find(const lt::sha1_hash &infoHash) const {
            for (const auto &s : stores) {
                if (s.second->infoHash == infoHash) {
                    return s.second;
                }
            }
            return nullptr;
        }

        // the reader distance of a piece: -1 if no reader wants it, else how far ahead
        // of the closest reader's first piece it is
        int64_t readerDistance(const Store &store, int piece) const {
            int64_t best = -1;
            for (const auto &r : readers) {
                if (r.second.infoHash == store.infoHash && piece >= r.second.first && piece <= r.second.last) {
                    int64_t d = piece - r.second.first;
                    if (best < 0 || d < best) {
                        best = d;
                    }
                }
            }
            return best;
        }

        // drops verified pieces past the budget: first the least recently used ones no
        // reader wants, then the ones farthest ahead of a reader. Pieces being downloaded
        // stay (the budget may be exceeded by them)
        void evict(std::vector<Lost> &lost) {
            while (bytes > capacity) {
                Store *victimStore = nullptr;
                int victim = -1;
                bool victimWanted = true;
                uint64_t victimUsed = 0;
                int64_t victimDistance = -1;
                for (auto &s : stores) {
                    for (auto &p : s.second->pieces) {
                        if (!p.second.verified) {
                            continue;
                        }
                        int64_t distance = readerDistance(*s.second, p.first);
                        bool wanted = distance >= 0;
                        bool better;
                        if (victim < 0) {
                            better = true;
                        } else if (wanted != victimWanted) {
                            better = !wanted;
                        } else if (!wanted) {
                            better = p.second.used < victimUsed;
                        } else {
                            better = distance > victimDistance;
                        }
                        if (better) {
                            victimStore = s.second.get();
                            victim = p.first;
                            victimWanted = wanted;
                            victimUsed = p.second.used;
                            victimDistance = distance;
                        }
                    }
                }
                if (!victimStore) {
                    return;
                }
                auto it = victimStore->pieces.find(victim);
                bytes -= static_cast<int64_t>(it->second.data->size());
                victimStore->pieces.erase(it);
                evicted++;
                lost.push_back({victimStore->torrent, victim});
            }
        }
    };

    namespace {

        int idx(lt::storage_index_t s) {
            return static_cast<int>(static_cast<std::uint32_t>(s));
        }

        lt::storage_error evictedError() {
            lt::storage_error se;
            se.ec = lt::error_code(lt::errors::piece_evicted, lt::libtorrent_category());
            se.operation = lt::operation_t::file_read;
            return se;
        }

        // SHA-1 of a piece, and its SHA-256 block hashes for v2 torrents
        lt::sha1_hash hashPiece(const std::vector<char> &data, const lt::file_storage &files, lt::piece_index_t piece,
                                lt::span<lt::sha256_hash> blockHashes) {
            if (!blockHashes.empty()) {
                const int size2 = files.piece_size2(piece);
                const int blocks = files.blocks_in_piece2(piece);
                int64_t offset = 0;
                for (int k = 0; k < blocks && k < static_cast<int>(blockHashes.size()); k++) {
                    const int len = static_cast<int>(std::min<int64_t>(lt::default_block_size, size2 - offset));
                    blockHashes[k] = lt::hasher256(data.data() + offset, len).final();
                    offset += len;
                }
            }
            return lt::hasher(data.data(), static_cast<int>(data.size())).final();
        }

        class RamDiskIo final : public lt::disk_interface, public lt::buffer_allocator_interface {
        public:
            RamDiskIo(lt::io_context &ioc, std::shared_ptr<RamCache::State> state)
                    : m_ioc(ioc), m_state(std::move(state)), m_worker([this] { work(); }) {}

            ~RamDiskIo() override {
                stopWorker();
            }

            lt::storage_holder new_torrent(lt::storage_params const &params, std::shared_ptr<void> const &torrent) override {
                auto store = std::make_shared<RamCache::State::Store>();
                store->infoHash = params.info_hash;
                store->files = &params.files;
                store->torrent = torrent;
                int index = m_nextIndex++;
                std::lock_guard<std::mutex> lock(m_state->mutex);
                m_state->stores[index] = store;
                return lt::storage_holder(lt::storage_index_t{static_cast<std::uint32_t>(index)}, *this);
            }

            void remove_torrent(lt::storage_index_t index) override {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                dropAll(idx(index));
                m_state->stores.erase(idx(index));
            }

            void async_read(lt::storage_index_t storage, lt::peer_request const &r,
                            std::function<void(lt::disk_buffer_holder, lt::storage_error const &)> handler,
                            lt::disk_job_flags_t) override {
                char *buf = nullptr;
                lt::storage_error error;
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    RamCache::State::Entry *e = entry(storage, static_cast<int>(r.piece));
                    if (!e || !e->verified || r.start < 0 ||
                        r.start + r.length > static_cast<int>(e->data->size())) {
                        error = evictedError();
                    } else {
                        e->used = ++m_state->tick;
                        buf = new char[static_cast<size_t>(r.length)];
                        std::memcpy(buf, e->data->data() + r.start, static_cast<size_t>(r.length));
                    }
                }
                const int length = buf ? r.length : 0;
                boost::asio::post(m_ioc, [this, handler = std::move(handler), buf, length, error] {
                    handler(lt::disk_buffer_holder(*this, buf, length), error);
                });
            }

            bool async_write(lt::storage_index_t storage, lt::peer_request const &r, char const *buf,
                             std::shared_ptr<lt::disk_observer>, std::function<void(lt::storage_error const &)> handler,
                             lt::disk_job_flags_t) override {
                std::vector<RamCache::State::Lost> lost;
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    auto it = m_state->stores.find(idx(storage));
                    if (it != m_state->stores.end()) {
                        RamCache::State::Store &store = *it->second;
                        RamCache::State::Entry &e = store.pieces[static_cast<int>(r.piece)];
                        if (!e.data) {
                            const int size = store.files->piece_size(r.piece);
                            e.data = std::make_shared<std::vector<char>>(static_cast<size_t>(size));
                            m_state->bytes += size;
                        }
                        if (r.start >= 0 && r.start + r.length <= static_cast<int>(e.data->size())) {
                            std::memcpy(e.data->data() + r.start, buf, static_cast<size_t>(r.length));
                        }
                        e.used = ++m_state->tick;
                        m_state->evict(lost);
                    }
                }
                boost::asio::post(m_ioc, [handler = std::move(handler)] { handler(lt::storage_error()); });
                reportLost(lost);
                return false;
            }

            // on the worker thread: SHA-1 of a 4 MB piece takes tens of ms on a console
            void async_hash(lt::storage_index_t storage, lt::piece_index_t piece, lt::span<lt::sha256_hash> blockHashes,
                            lt::disk_job_flags_t,
                            std::function<void(lt::piece_index_t, lt::sha1_hash const &, lt::storage_error const &)> handler)
            override {
                std::shared_ptr<std::vector<char>> data;
                const lt::file_storage *files = nullptr;
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    RamCache::State::Entry *e = entry(storage, static_cast<int>(piece));
                    if (e) {
                        data = e->data;
                        files = m_state->stores[idx(storage)]->files;
                    }
                }
                if (!data) {
                    // gone: a wrong hash, so the piece is downloaded again
                    boost::asio::post(m_ioc, [handler = std::move(handler), piece] {
                        handler(piece, lt::sha1_hash(), lt::storage_error());
                    });
                    return;
                }
                const int index = idx(storage);
                queue([this, data, files, piece, blockHashes, index, handler = std::move(handler)] {
                    lt::sha1_hash hash = hashPiece(*data, *files, piece, blockHashes);
                    // verified and the handler posted under the lock: an eviction seen after
                    // it reports the loss after the torrent took the piece
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    RamCache::State::Entry *e = entry(lt::storage_index_t{static_cast<std::uint32_t>(index)}, static_cast<int>(piece));
                    if (e && e->data == data) {
                        e->verified = true;
                    }
                    boost::asio::post(m_ioc, [handler, piece, hash] { handler(piece, hash, lt::storage_error()); });
                });
            }

            void async_hash2(lt::storage_index_t storage, lt::piece_index_t piece, int offset, lt::disk_job_flags_t,
                             std::function<void(lt::piece_index_t, lt::sha256_hash const &, lt::storage_error const &)> handler)
            override {
                lt::sha256_hash hash;
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    RamCache::State::Entry *e = entry(storage, static_cast<int>(piece));
                    if (e) {
                        const int size2 = m_state->stores[idx(storage)]->files->piece_size2(piece);
                        const int len = std::min(lt::default_block_size, size2 - offset);
                        if (offset >= 0 && len > 0 && offset + len <= static_cast<int>(e->data->size())) {
                            hash = lt::hasher256(e->data->data() + offset, len).final();
                        }
                    }
                }
                boost::asio::post(m_ioc, [handler = std::move(handler), piece, hash] {
                    handler(piece, hash, lt::storage_error());
                });
            }

            void async_move_storage(lt::storage_index_t, std::string p, lt::move_flags_t,
                                    std::function<void(lt::status_t, std::string const &, lt::storage_error const &)> handler)
            override {
                boost::asio::post(m_ioc, [handler = std::move(handler), p] {
                    handler(lt::status_t::no_error, p, lt::storage_error());
                });
            }

            void async_release_files(lt::storage_index_t, std::function<void()> handler) override {
                if (handler) {
                    boost::asio::post(m_ioc, std::move(handler));
                }
            }

            void async_check_files(lt::storage_index_t, lt::add_torrent_params const *,
                                   lt::aux::vector<std::string, lt::file_index_t>,
                                   std::function<void(lt::status_t, lt::storage_error const &)> handler) override {
                boost::asio::post(m_ioc, [handler = std::move(handler)] {
                    handler(lt::status_t::no_error, lt::storage_error());
                });
            }

            void async_stop_torrent(lt::storage_index_t, std::function<void()> handler) override {
                if (handler) {
                    boost::asio::post(m_ioc, std::move(handler));
                }
            }

            void async_rename_file(lt::storage_index_t, lt::file_index_t index, std::string name,
                                   std::function<void(std::string const &, lt::file_index_t, lt::storage_error const &)> handler)
            override {
                boost::asio::post(m_ioc, [handler = std::move(handler), name, index] {
                    handler(name, index, lt::storage_error());
                });
            }

            void async_delete_files(lt::storage_index_t storage, lt::remove_flags_t,
                                    std::function<void(lt::storage_error const &)> handler) override {
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    dropAll(idx(storage));
                }
                boost::asio::post(m_ioc, [handler = std::move(handler)] { handler(lt::storage_error()); });
            }

            void async_set_file_priority(lt::storage_index_t, lt::aux::vector<lt::download_priority_t, lt::file_index_t> prio,
                                         std::function<void(lt::storage_error const &,
                                                            lt::aux::vector<lt::download_priority_t, lt::file_index_t>)> handler)
            override {
                boost::asio::post(m_ioc, [handler = std::move(handler), prio = std::move(prio)]() mutable {
                    handler(lt::storage_error(), std::move(prio));
                });
            }

            void async_clear_piece(lt::storage_index_t storage, lt::piece_index_t piece,
                                   std::function<void(lt::piece_index_t)> handler) override {
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    auto it = m_state->stores.find(idx(storage));
                    if (it != m_state->stores.end()) {
                        auto p = it->second->pieces.find(static_cast<int>(piece));
                        if (p != it->second->pieces.end()) {
                            m_state->bytes -= static_cast<int64_t>(p->second.data->size());
                            it->second->pieces.erase(p);
                        }
                    }
                }
                boost::asio::post(m_ioc, [handler = std::move(handler), piece] { handler(piece); });
            }

            void update_stats_counters(lt::counters &) const override {}

            std::vector<lt::open_file_state> get_status(lt::storage_index_t) const override {
                return {};
            }

            void abort(bool wait) override {
                if (wait) {
                    stopWorker();
                } else {
                    std::lock_guard<std::mutex> lock(m_jobsMutex);
                    m_stop = true;
                    m_jobsCv.notify_all();
                }
            }

            void submit_jobs() override {}

            void settings_updated() override {}

            void free_disk_buffer(char *buf) override {
                delete[] buf;
            }

        private:
            // with m_state->mutex held
            RamCache::State::Entry *entry(lt::storage_index_t storage, int piece) {
                auto it = m_state->stores.find(idx(storage));
                if (it == m_state->stores.end()) {
                    return nullptr;
                }
                auto p = it->second->pieces.find(piece);
                return p == it->second->pieces.end() ? nullptr : &p->second;
            }

            // with m_state->mutex held
            void dropAll(int index) {
                auto it = m_state->stores.find(index);
                if (it == m_state->stores.end()) {
                    return;
                }
                for (const auto &p : it->second->pieces) {
                    m_state->bytes -= static_cast<int64_t>(p.second.data->size());
                }
                it->second->pieces.clear();
            }

            // on the network thread, after the current job
            void reportLost(const std::vector<RamCache::State::Lost> &lost) {
                for (const auto &l : lost) {
                    boost::asio::post(m_ioc, [torrent = l.torrent, piece = l.piece] {
                        if (auto t = torrent.lock()) {
                            lt::torrent_lost_piece(t, lt::piece_index_t{piece});
                        }
                    });
                }
            }

            void queue(std::function<void()> job) {
                std::lock_guard<std::mutex> lock(m_jobsMutex);
                m_jobs.push_back(std::move(job));
                m_jobsCv.notify_one();
            }

            void work() {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock(m_jobsMutex);
                        m_jobsCv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                        if (m_jobs.empty()) {
                            return;
                        }
                        job = std::move(m_jobs.front());
                        m_jobs.pop_front();
                    }
                    job();
                }
            }

            void stopWorker() {
                {
                    std::lock_guard<std::mutex> lock(m_jobsMutex);
                    m_stop = true;
                    m_jobsCv.notify_all();
                }
                if (m_worker.joinable()) {
                    m_worker.join();
                }
            }

            lt::io_context &m_ioc;
            std::shared_ptr<RamCache::State> m_state;
            int m_nextIndex = 0;

            std::mutex m_jobsMutex;
            std::condition_variable m_jobsCv;
            std::deque<std::function<void()>> m_jobs;
            bool m_stop = false;
            std::thread m_worker;
        };

    }

    RamCache::RamCache(int64_t capacity) : m_state(std::make_shared<State>()) {
        m_state->capacity = capacity;
    }

    RamCache::~RamCache() = default;

    lt::disk_io_constructor_type RamCache::constructor() {
        std::shared_ptr<State> state = m_state;
        return [state](lt::io_context &ioc, lt::settings_interface const &, lt::counters &) {
            return std::unique_ptr<lt::disk_interface>(new RamDiskIo(ioc, state));
        };
    }

    RamCache::Piece RamCache::piece(const lt::sha1_hash &infoHash, int piece) const {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        auto store = m_state->find(infoHash);
        if (!store) {
            return nullptr;
        }
        auto it = store->pieces.find(piece);
        if (it == store->pieces.end() || !it->second.verified) {
            return nullptr;
        }
        it->second.used = ++m_state->tick;
        return it->second.data;
    }

    void RamCache::setReader(const lt::sha1_hash &infoHash, int id, int first, int last) {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (first < 0) {
            m_state->readers.erase(id);
        } else {
            m_state->readers[id] = {infoHash, first, last};
        }
    }

    RamCache::Stats RamCache::stats() const {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        Stats s;
        s.bytes = m_state->bytes;
        s.capacity = m_state->capacity;
        s.evicted = m_state->evicted;
        for (const auto &store : m_state->stores) {
            s.pieces += static_cast<int>(store.second->pieces.size());
        }
        return s;
    }

}
