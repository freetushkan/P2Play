// One libtorrent session, two storages: the RAM cache for streamed torrents and files on disk
// for the MediaMesh list versions (kept and seeded whole). A torrent whose save path is under
// the given directory goes to disk; every call is passed to the storage that holds it.

#include "hybrid_disk.hpp"

#include <map>
#include <mutex>

#include <libtorrent/disk_interface.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/storage_defs.hpp>

namespace p2p {

    namespace {
        class HybridDiskIo final : public lt::disk_interface {
        public:
            HybridDiskIo(std::unique_ptr<lt::disk_interface> ram, std::unique_ptr<lt::disk_interface> disk,
                         std::string diskPrefix)
                    : m_ram(std::move(ram)), m_disk(std::move(disk)), m_prefix(std::move(diskPrefix)) {}

            lt::storage_holder new_torrent(lt::storage_params const &p, std::shared_ptr<void> const &torrent) override {
                const bool disk = !m_prefix.empty() && p.path.compare(0, m_prefix.size(), m_prefix) == 0;
                lt::disk_interface &target = disk ? *m_disk : *m_ram;
                lt::storage_holder inner = target.new_torrent(p, torrent);
                std::lock_guard<std::mutex> lock(m_mutex);
                const std::uint32_t index = m_next++;
                m_slots.emplace(index, Slot{&target, std::move(inner)});
                return lt::storage_holder(lt::storage_index_t{index}, *this);
            }

            void remove_torrent(lt::storage_index_t index) override {
                lt::storage_holder inner;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_slots.find(static_cast<std::uint32_t>(index));
                    if (it == m_slots.end()) {
                        return;
                    }
                    inner = std::move(it->second.holder);
                    m_slots.erase(it);
                }
                // inner's destructor removes it from its storage
            }

            void async_read(lt::storage_index_t s, lt::peer_request const &r,
                            std::function<void(lt::disk_buffer_holder, lt::storage_error const &)> h,
                            lt::disk_job_flags_t f) override {
                auto t = at(s);
                t.first->async_read(t.second, r, std::move(h), f);
            }

            bool async_write(lt::storage_index_t s, lt::peer_request const &r, char const *buf,
                             std::shared_ptr<lt::disk_observer> o, std::function<void(lt::storage_error const &)> h,
                             lt::disk_job_flags_t f) override {
                auto t = at(s);
                return t.first->async_write(t.second, r, buf, std::move(o), std::move(h), f);
            }

            void async_hash(lt::storage_index_t s, lt::piece_index_t piece, lt::span<lt::sha256_hash> v2,
                            lt::disk_job_flags_t f,
                            std::function<void(lt::piece_index_t, lt::sha1_hash const &, lt::storage_error const &)> h) override {
                auto t = at(s);
                t.first->async_hash(t.second, piece, v2, f, std::move(h));
            }

            void async_hash2(lt::storage_index_t s, lt::piece_index_t piece, int offset, lt::disk_job_flags_t f,
                             std::function<void(lt::piece_index_t, lt::sha256_hash const &, lt::storage_error const &)> h) override {
                auto t = at(s);
                t.first->async_hash2(t.second, piece, offset, f, std::move(h));
            }

            void async_move_storage(lt::storage_index_t s, std::string p, lt::move_flags_t f,
                                    std::function<void(lt::status_t, std::string const &, lt::storage_error const &)> h) override {
                auto t = at(s);
                t.first->async_move_storage(t.second, std::move(p), f, std::move(h));
            }

            void async_release_files(lt::storage_index_t s, std::function<void()> h) override {
                auto t = at(s);
                t.first->async_release_files(t.second, std::move(h));
            }

            void async_check_files(lt::storage_index_t s, lt::add_torrent_params const *resume,
                                   lt::aux::vector<std::string, lt::file_index_t> links,
                                   std::function<void(lt::status_t, lt::storage_error const &)> h) override {
                auto t = at(s);
                t.first->async_check_files(t.second, resume, std::move(links), std::move(h));
            }

            void async_stop_torrent(lt::storage_index_t s, std::function<void()> h) override {
                auto t = at(s);
                t.first->async_stop_torrent(t.second, std::move(h));
            }

            void async_rename_file(lt::storage_index_t s, lt::file_index_t index, std::string name,
                                   std::function<void(std::string const &, lt::file_index_t, lt::storage_error const &)> h) override {
                auto t = at(s);
                t.first->async_rename_file(t.second, index, std::move(name), std::move(h));
            }

            void async_delete_files(lt::storage_index_t s, lt::remove_flags_t o,
                                    std::function<void(lt::storage_error const &)> h) override {
                auto t = at(s);
                t.first->async_delete_files(t.second, o, std::move(h));
            }

            void async_set_file_priority(lt::storage_index_t s, lt::aux::vector<lt::download_priority_t, lt::file_index_t> prio,
                                         std::function<void(lt::storage_error const &,
                                                            lt::aux::vector<lt::download_priority_t, lt::file_index_t>)> h) override {
                auto t = at(s);
                t.first->async_set_file_priority(t.second, std::move(prio), std::move(h));
            }

            void async_clear_piece(lt::storage_index_t s, lt::piece_index_t index,
                                   std::function<void(lt::piece_index_t)> h) override {
                auto t = at(s);
                t.first->async_clear_piece(t.second, index, std::move(h));
            }

            void update_stats_counters(lt::counters &c) const override {
                m_ram->update_stats_counters(c);
                m_disk->update_stats_counters(c);
            }

            std::vector<lt::open_file_state> get_status(lt::storage_index_t s) const override {
                auto t = const_cast<HybridDiskIo *>(this)->at(s);
                return t.first->get_status(t.second);
            }

            void abort(bool wait) override {
                m_ram->abort(wait);
                m_disk->abort(wait);
            }

            void submit_jobs() override {
                m_ram->submit_jobs();
                m_disk->submit_jobs();
            }

            void settings_updated() override {
                m_ram->settings_updated();
                m_disk->settings_updated();
            }

        private:
            struct Slot {
                lt::disk_interface *target;
                lt::storage_holder holder;
            };

            std::pair<lt::disk_interface *, lt::storage_index_t> at(lt::storage_index_t s) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const Slot &slot = m_slots.at(static_cast<std::uint32_t>(s));
                return {slot.target, static_cast<lt::storage_index_t>(slot.holder)};
            }

            std::unique_ptr<lt::disk_interface> m_ram, m_disk;
            std::string m_prefix;
            std::mutex m_mutex;
            std::map<std::uint32_t, Slot> m_slots;
            std::uint32_t m_next = 0;
        };
    }

    lt::disk_io_constructor_type hybridDisk(lt::disk_io_constructor_type ram, std::string diskPrefix) {
        return [ram, diskPrefix](lt::io_context &ioc, lt::settings_interface const &sett, lt::counters &cnt) {
            return std::unique_ptr<lt::disk_interface>(
                    new HybridDiskIo(ram(ioc, sett, cnt), lt::posix_disk_io_constructor(ioc, sett, cnt), diskPrefix));
        };
    }

}
