#pragma once

// The P2Play core: a libtorrent session, the torrents (active or only in the library),
// streaming any file of a torrent over HTTP and the TorrServer MatriX API (api.cpp).
// Torrents are downloaded only around what is read (the readers' windows), as in
// TorrServer; with the RAM storage nothing else is kept.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>

#include "../../third_party/nlohmann/json.hpp"
#include "http.hpp"
#include "ram_cache.hpp"
#include "store.hpp"

namespace p2p {

    struct CoreConfig {
        std::string dataDir;
        uint16_t httpPort = 8090;   // TorrServer's
        bool console = false;       // fewer connections, no LSD/UPnP/NAT-PMP, IPv4 only
        std::string platform;       // shown by /echo
        std::function<void(const std::string &)> log;
    };

    // TorrServer's settings (BTSets) the core uses; the others are kept and given back
    struct Settings {
        int64_t cacheSize = 128LL * 1024 * 1024;
        int readAhead = 95;          // % of the cache for the readers' windows (at most half of it)
        int preloadCache = 50;       // % of the cache preloaded
        bool useDisk = false;        // whole pieces on disk (restart)
        std::string savePath;        // with useDisk; empty: <data>/downloads
        int disconnectTimeout = 30;  // s without a reader before a torrent is closed
        int connectionsLimit = 25;
        int downloadRateLimit = 0;   // KB/s, 0: none
        int uploadRateLimit = 0;
        bool disableUpload = false;
        bool disableDht = false;
        bool disableUpnp = false;
        int listenPort = 0;          // BitTorrent, 0: any (restart)
    };

    class Core {
    public:
        explicit Core(CoreConfig config);
        ~Core();

        Core(const Core &) = delete;
        Core &operator=(const Core &) = delete;

        bool start();

        void stop();

        uint16_t httpPort() const { return m_http.port(); }

        static const char *version();

        // /echo: the version and the platform
        std::string echo() const;

        void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    private:
        struct Torrent;
        using TorrentPtr = std::shared_ptr<Torrent>;

        // api.cpp
        void handle(const HttpRequest &req, HttpResponse &res);
        void route(const HttpRequest &req, HttpResponse &res);
        void apiTorrents(const HttpRequest &req, HttpResponse &res);
        void apiStream(const HttpRequest &req, HttpResponse &res, const std::string &hash, int index);
        void apiViewed(const HttpRequest &req, HttpResponse &res);
        void apiSettings(const HttpRequest &req, HttpResponse &res);
        void apiCache(const HttpRequest &req, HttpResponse &res);
        void apiUpload(const HttpRequest &req, HttpResponse &res);
        std::string playlist(const std::vector<TorrentPtr> &torrents, const std::string &host, int fromIndex);

        // torrents
        TorrentPtr add(const std::string &link, const std::string &title, const std::string &poster,
                       const std::string &category, const std::string &data, bool save, std::string &error);
        TorrentPtr addMetadata(const std::string &torrentFile, const std::string &title, const std::string &poster,
                               const std::string &category, const std::string &data, bool save, std::string &error);
        TorrentPtr find(const std::string &hash);
        std::vector<TorrentPtr> list();
        bool activate(const TorrentPtr &t, std::string &error);
        void close(const TorrentPtr &t);
        void remove(const TorrentPtr &t);
        void save(const TorrentPtr &t);
        bool waitInfo(const TorrentPtr &t, std::chrono::seconds timeout);
        void onMetadata(const TorrentPtr &t);
        nlohmann::json status(const TorrentPtr &t);
        nlohmann::json filesData(const TorrentPtr &t);

        // streaming
        void stream(const TorrentPtr &t, int index, const HttpRequest &req, HttpResponse &res);
        void preload(const TorrentPtr &t, int index);
        void setWindow(const TorrentPtr &t, int reader, int first, int last);
        std::shared_ptr<const std::vector<char>> readPiece(const TorrentPtr &t, int piece, std::chrono::seconds timeout);
        bool waitPiece(const TorrentPtr &t, int piece, std::chrono::seconds timeout);
        int64_t windowBytes();

        // settings
        void loadSettings();
        void applySettings();
        nlohmann::json settingsJson();

        void alertLoop();
        void alertStep();
        void handleAlert(lt::alert *a);
        void idleLoop();
        void idleStep();

        CoreConfig m_config;
        Store m_store;
        Settings m_settings;
        std::unique_ptr<RamCache> m_cache;   // null with useDisk
        std::unique_ptr<lt::session> m_session;
        HttpServer m_http;

        std::mutex m_mutex;                  // m_torrents and the Torrent fields
        std::condition_variable m_changed;   // metadata, pieces read
        std::map<std::string, TorrentPtr> m_torrents;
        uint64_t m_resumeWrites = 0;         // save_resume_data answers

        std::mutex m_readsMutex;             // disk storage: read_piece results
        std::map<std::pair<std::string, int>, std::shared_ptr<const std::vector<char>>> m_reads;
        std::set<std::pair<std::string, int>> m_readsFailed;

        std::atomic<bool> m_stopping{false};
        std::atomic<int> m_nextReader{1};
        std::thread m_alertThread;
        std::thread m_idleThread;
        std::mutex m_logMutex;
    };

}
