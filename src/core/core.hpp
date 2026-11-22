#pragma once

// The P2Play core: a libtorrent session, the torrents (active or only in the library),
// streaming any file of a torrent over HTTP; the native API of Silo's scheme (api_silo.cpp) and
// the TorrServer MatriX API (api_matrix.cpp), one set of users (auth.hpp).
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
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/alert.hpp>

#include "../../third_party/nlohmann/json.hpp"
#include "auth.hpp"
#include "http.hpp"
#include "mesh_incoming_queue.hpp"
#include "mesh.hpp"
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
        std::string dhtBootstrap;    // P2PDhtBootstrap: "host:port,...", a DHT of our own
        bool catalogOnly = false;    // P2PCatalogOnly: lists and search, no streamer (restart)
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

        // streams being served (the host keeps the device awake meanwhile)
        int activeStreams();

        // a client asked the server to stop (MatriX /shutdown); the host decides
        bool shutdownRequested() const { return m_shutdownRequested; }

        // a catalog node: the lists and the search only, no streamer
        bool catalogOnly() const { return m_settings.catalogOnly; }

        static const char *version();

        // /echo: the version and the platform
        std::string echo() const;

        void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    private:
        struct Torrent;
        using TorrentPtr = std::shared_ptr<Torrent>;

        // api_auth.cpp: every request, who sends it
        void handle(const HttpRequest &req, HttpResponse &res);
        void route(const HttpRequest &req, HttpResponse &res);

        // api_silo.cpp: the native API, Silo's scheme under /api (file indexes from 0)
        void routeSilo(const HttpRequest &req, HttpResponse &res, const User &user);
        // mesh_net.cpp: list versions as torrents, their pointers in the DHT (stage 3)
        struct MeshNet {
            lt::torrent_handle seeding;      // the current version's torrent
            lt::sha256_hash versionIh;
            bool pendingGet = false;         // deferred DHT lookup from discovery callback
            bool requested = false;          // ask directory peers as well as the DHT
            int64_t pointerSeq = -1;          // signed high-water mark, including in-flight versions
            std::string pointer;             // verified BEP 44 envelope, ready to relay
            bool wanted = false;             // subscribed, no version yet
            std::vector<std::string> peers;  // hints: ip:port of nodes holding it
            bool importing = false;          // finished version queued on the joined worker
            lt::torrent_handle fetching;
            lt::sha256_hash fetchIh;
            int64_t fetchSeq = -1;
            std::string fetchPointer, fetchDir;
            bool accepted = false;
            bool prioritiesSet = false;
            std::set<std::string> installed, mediaWanted, reservedMedia;
            bool recordsReady = false;
            std::chrono::steady_clock::time_point transferStarted, nextRetry;
            std::map<std::string, std::chrono::steady_clock::time_point> mediaDeadlines;
            std::string netError;
        };
        std::shared_ptr<lt::torrent_info> meshTorrent(const std::string &pk, std::string &error);
        void meshSeed(const std::string &pk, std::shared_ptr<lt::torrent_info> ti);
        void meshPut(const std::string &pk);
        void meshGet(const std::string &pk);
        void meshReceivePointer(const std::string &raw, const std::string &peer);
        void meshWork();
        void meshDetach(const std::string &pk);
        void meshRefresh(const std::string &pk);
        bool meshMediaRequest(const std::string &pk, const std::string &ih, const std::string &kind);
        void meshForget(const std::string &pk);
        std::string meshSubscribe(const std::string &pk, const std::vector<std::string> &peers);
        void meshFetch(const std::string &pk, const lt::sha256_hash &ih, int64_t seq, const std::string &pointer);
        bool meshAlert(lt::alert *a);
        void meshTick(bool start);
        void meshReleased(const std::string &pk);
        nlohmann::json meshState(const std::string &pk);
        void meshDirectory();
        nlohmann::json meshDirectoryJson();
    public:
        // mesh_ext.cpp's hooks
        lt::info_hash_t meshDirectoryHash();
        std::vector<std::pair<std::string, int64_t>> meshHeld();
        void meshDiscovered(const std::string &peer, const std::vector<std::pair<std::string, int64_t>> &lists);
        std::vector<std::pair<std::string, int64_t>> meshRequests();
        bool meshPeerItem(const std::string &key, std::string &raw);
        void meshPeerReceived(const std::string &raw, const std::string &peer = "", const std::string &source = "");
    private:
        struct Discovered {
            int64_t seq = 0;
            std::set<std::string> holders;
        };

        // api_catalog.cpp: the lists, the catalog search, publishing (stage 3)
        void routeMesh(const HttpRequest &req, HttpResponse &res, const User &user, const std::vector<std::string> &parts);
        void siloEvents(HttpResponse &res, const User &user);
        void siloAdd(const HttpRequest &req, HttpResponse &res, const User &user);
        void siloUsers(const HttpRequest &req, HttpResponse &res, const User &user, const std::vector<std::string> &parts);
        nlohmann::json siloStatus(const TorrentPtr &t, const std::string &user);
        nlohmann::json siloProgress(const std::string &user, int64_t since);
        // a position: viewed set past the threshold
        void setPosition(const std::string &user, const std::string &hash, int fileIndex, double position,
                         double duration, bool keepViewed);

        // api_matrix.cpp: the MatriX compatibility layer (file ids from 1)
        bool routeMatrix(const HttpRequest &req, HttpResponse &res, const User &user);
        bool matrixPublic(const HttpRequest &req);
        void downloadTest(const HttpRequest &req, HttpResponse &res, int64_t megabytes);
        void apiTorrents(const HttpRequest &req, HttpResponse &res, const User &user);
        void apiStream(const HttpRequest &req, HttpResponse &res, const std::string &hash, int index, const User &user);
        void apiViewed(const HttpRequest &req, HttpResponse &res, const User &user);
        void apiSettings(const HttpRequest &req, HttpResponse &res);
        void apiCache(const HttpRequest &req, HttpResponse &res);
        void apiUpload(const HttpRequest &req, HttpResponse &res, const User &user);
        std::string playlist(const std::vector<TorrentPtr> &torrents, const std::string &host, int fromIndex,
                             const std::string &user, const std::string &query);

        // the hex infohash of a magnet, an infohash (hex or base32); empty otherwise
        static std::string hashOfLink(const std::string &link);

        // torrents
        // saveFor: the user whose library takes it (empty: none, the torrent is forgotten
        // once closed unless a library holds it)
        TorrentPtr add(const std::string &link, const std::string &title, const std::string &poster,
                       const std::string &category, const std::string &data, const std::string &saveFor,
                       std::string &error);
        TorrentPtr addMetadata(const std::string &torrentFile, const std::string &title, const std::string &poster,
                               const std::string &category, const std::string &data, const std::string &saveFor,
                               std::string &error);
        // a torrs:// card into a user's library, not opened (Silo's import); false: a bad link
        bool addCard(const std::string &link, const std::string &user);
        // the torrs:// link of a torrent (its card, size and trackers)
        std::string torrsOf(const TorrentPtr &t);
        TorrentPtr find(const std::string &hash);
        std::vector<TorrentPtr> list();
        // the torrents of a user's library; withOpen: and the open ones no library holds
        std::vector<TorrentPtr> listOf(const std::string &user, bool withOpen);
        bool owns(const TorrentPtr &t, const std::string &user);
        void keep(const TorrentPtr &t, const std::string &user);
        // out of a user's library; gone once no library holds it
        void unlink(const TorrentPtr &t, const std::string &user);
        void removeUser(const std::string &user);
        bool activate(const TorrentPtr &t, std::string &error);
        void close(const TorrentPtr &t);
        void remove(const TorrentPtr &t);
        void save(const TorrentPtr &t);
        void renumber(TorrentRecord &r);
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
        Mesh m_mesh;
        std::mutex m_meshMutex;
        // Worker/API disk lifecycle only; never acquired from libtorrent callbacks.
        std::recursive_mutex m_meshLifecycle;
        uint64_t m_meshGeneration = 0;
        struct MeshMediaRequest { std::string pk, path; int64_t seq; };
        std::vector<MeshMediaRequest> m_meshMediaQueue;
        std::map<lt::torrent_handle, std::string> m_meshRetired;
        std::vector<std::string> m_meshGarbage;
        std::map<std::string, MeshNet> m_meshNet;
        MeshIncomingQueue m_meshIncoming; // bounded network -> idle worker queue
        lt::error_code m_meshError;
        lt::torrent_handle m_directory;
        std::mutex m_directoryMutex;
        lt::info_hash_t m_directoryHash;
        std::map<std::string, Discovered> m_discovered;
        std::chrono::steady_clock::time_point m_meshLast, m_meshWantedLast;
        Auth m_auth;
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
        std::atomic<bool> m_shutdownRequested{false};
        std::chrono::steady_clock::time_point m_started;
        std::atomic<int> m_nextReader{1};
        std::thread m_alertThread;
        std::thread m_idleThread;
        std::mutex m_logMutex;
    };

}
