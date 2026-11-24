#include "core.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/write_resume_data.hpp>

#include "core_torrent.hpp"
#include "torrs.hpp"
#include "hybrid_disk.hpp"

namespace p2p {

    using nlohmann::json;
    using Clock = std::chrono::steady_clock;

    namespace {

        std::string hexOf(const lt::info_hash_t &ih) {
            std::ostringstream o;
            o << (ih.has_v1() ? ih.v1 : ih.get_best());
            return o.str();
        }

        bool isHex40(const std::string &s) {
            return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
        }

        bool isBase32(const std::string &s) {
            return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
            });
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        // the digits at the start of s; false if none or too many (Go's strconv.Atoi)
        bool numberPrefix(const std::string &s, int64_t &out) {
            size_t n = 0;
            while (n < s.size() && std::isdigit(static_cast<unsigned char>(s[n]))) {
                n++;
            }
            if (n == 0 || n > 18) {
                return false;
            }
            out = std::stoll(s.substr(0, n));
            return true;
        }

        // TorrServer's file order (server/utils CompareStrings): after the common prefix (back
        // to the start of a number it ends in), numbers compare as numbers: "e2" before "e10"
        bool torrServerLess(const std::string &first, const std::string &second) {
            size_t prefix = 0;
            while (prefix < first.size() && prefix < second.size() && first[prefix] == second[prefix]) {
                prefix++;
            }
            while (prefix > 0 && std::isdigit(static_cast<unsigned char>(first[prefix - 1]))) {
                prefix--;
            }
            const std::string a = first.substr(prefix), b = second.substr(prefix);
            int64_t na = 0, nb = 0;
            const bool ha = numberPrefix(a, na), hb = numberPrefix(b, nb);
            if (ha && hb) {
                return na == nb ? a < b : na < nb;
            }
            if (ha) {
                return true;
            }
            if (hb) {
                return false;
            }
            return a < b;
        }

    }

    // The files as TorrServer numbers them. MatriX (anacrolix): every file of the torrent,
    // pad files (BEP 47) too, sorted by path (CompareStrings), ids from 1 in that order. Silo:
    // the index in the torrent's own order. Pad files keep their ids but are not listed.
    static std::vector<FileInfo> torrServerFiles(const lt::torrent_info &ti, int64_t &size,
                                          std::map<int, lt::file_index_t> &index) {
        const lt::file_storage &fs = ti.files();
        std::vector<std::pair<std::string, lt::file_index_t>> sorted;
        size = 0;
        for (lt::file_index_t i : fs.file_range()) {
            std::string path = fs.file_path(i);
            std::replace(path.begin(), path.end(), '\\', '/');
            sorted.emplace_back(path, i);
            if (!fs.pad_file_at(i)) {
                size += fs.file_size(i);
            }
        }
        // stable_sort: the comparison is not a strict order in every case (std::sort may then
        // run past the range)
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto &a, const auto &b) { return torrServerLess(a.first, b.first); });
        std::vector<FileInfo> files;
        index.clear();
        int id = 0;
        for (const auto &f : sorted) {
            id++;
            if (fs.pad_file_at(f.second)) {
                continue;
            }
            files.push_back({id, f.first, fs.file_size(f.second), static_cast<int>(f.second)});
            index[id] = f.second;
        }
        return files;
    }

    namespace {
        bool readFile(const std::string &path, std::string &out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return false;
            }
            std::stringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

        bool fileExists(const std::string &path) {
            struct stat st{};
            return stat(path.c_str(), &st) == 0;
        }

        // Applied before session construction, not after its network thread starts.
        void proxySettings(lt::settings_pack &sp, const PublisherProxy &proxy) {
            if (!proxy.enabled) return;
            sp.set_int(lt::settings_pack::proxy_type, proxy.username.empty()
                       ? lt::settings_pack::socks5 : lt::settings_pack::socks5_pw);
            sp.set_str(lt::settings_pack::proxy_hostname, proxy.host);
            sp.set_int(lt::settings_pack::proxy_port, proxy.port);
            sp.set_str(lt::settings_pack::proxy_username, proxy.username);
            sp.set_str(lt::settings_pack::proxy_password, proxy.password);
            sp.set_bool(lt::settings_pack::proxy_hostnames, true);
            sp.set_bool(lt::settings_pack::proxy_peer_connections, true);
            sp.set_bool(lt::settings_pack::proxy_tracker_connections, true);
            sp.set_bool(lt::settings_pack::enable_lsd, false);
            sp.set_bool(lt::settings_pack::enable_upnp, false);
            sp.set_bool(lt::settings_pack::enable_natpmp, false);
            sp.set_bool(lt::settings_pack::anonymous_mode, true);
            sp.set_bool(lt::settings_pack::socks5_udp_send_local_ep, false);
        }

        bool numericDhtNodes(const std::vector<std::pair<std::string, int>> &nodes) {
            for (const auto &node : nodes) {
                lt::error_code ec;
                lt::make_address(node.first, ec);
                if (ec || node.second < 1 || node.second > 65535) return false;
            }
            return true;
        }

        // libtorrent resolves non-compact HTTP tracker peer hostnames locally even
        // with proxy_hostnames=true. In strict mode only UDP trackers are admitted.
        // Metadata DHT bootstrap names are another local-resolver bypass.
        bool proxyTorrent(lt::add_torrent_params &atp, std::string &error) {
            if (!numericDhtNodes(atp.dht_nodes) || (atp.ti && !numericDhtNodes(atp.ti->nodes()))) {
                error = "PublisherProxy requires numeric torrent DHT nodes (no local DNS)";
                return false;
            }
            std::vector<std::string> trackers;
            for (const auto &tr : atp.trackers) {
                if (tr.rfind("udp://", 0) == 0) trackers.push_back(tr);
            }
            if (atp.ti) {
                for (const auto &tr : atp.ti->trackers()) {
                    if (tr.url.rfind("udp://", 0) == 0 &&
                        std::find(trackers.begin(), trackers.end(), tr.url) == trackers.end()) {
                        trackers.push_back(tr.url);
                    }
                }
            }
            atp.trackers = std::move(trackers);
            atp.tracker_tiers.assign(atp.trackers.size(), 0);
            atp.flags |= lt::torrent_flags::override_trackers;
            return true;
        }

        // http:// only (no TLS in the core); a few redirects
        bool httpGet(std::string url, std::string &body, std::string &error) {
            for (int hop = 0; hop < 4; hop++) {
                if (url.rfind("http://", 0) != 0) {
                    error = "only http:// links can be fetched";
                    return false;
                }
                std::string rest = url.substr(7);
                size_t slash = rest.find('/');
                std::string hostPort = rest.substr(0, slash);
                std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
                std::string host = hostPort, port = "80";
                size_t colon = hostPort.rfind(':');
                if (colon != std::string::npos) {
                    host = hostPort.substr(0, colon);
                    port = hostPort.substr(colon + 1);
                }
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *res = nullptr;
                if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
                    error = "cannot resolve " + host;
                    return false;
                }
                int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                bool connected = fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0;
                freeaddrinfo(res);
                if (!connected) {
                    if (fd >= 0) {
                        ::close(fd);
                    }
                    error = "cannot connect to " + hostPort;
                    return false;
                }
                std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostPort +
                                  "\r\nUser-Agent: P2Play\r\nConnection: close\r\n\r\n";
                ::send(fd, req.data(), req.size(), 0);
                std::string data;
                char buf[8192];
                ssize_t n;
                while ((n = recv(fd, buf, sizeof(buf), 0)) > 0 && data.size() < 32 * 1024 * 1024) {
                    data.append(buf, static_cast<size_t>(n));
                }
                ::close(fd);
                size_t headEnd = data.find("\r\n\r\n");
                if (headEnd == std::string::npos) {
                    error = "bad answer from " + hostPort;
                    return false;
                }
                std::string head = data.substr(0, headEnd);
                int status = std::atoi(head.substr(head.find(' ') + 1, 3).c_str());
                if (status >= 300 && status < 400) {
                    std::string lh = lower(head);
                    size_t loc = lh.find("\r\nlocation:");
                    if (loc == std::string::npos) {
                        error = "redirect without a location";
                        return false;
                    }
                    size_t end = head.find("\r\n", loc + 2);
                    std::string target = head.substr(loc + 11, end == std::string::npos ? std::string::npos : end - loc - 11);
                    target.erase(0, target.find_first_not_of(' '));
                    url = target.rfind("http", 0) == 0 ? target : "http://" + hostPort + target;
                    continue;
                }
                if (status != 200) {
                    error = "http status " + std::to_string(status);
                    return false;
                }
                body = data.substr(headEnd + 4);
                return true;
            }
            error = "too many redirects";
            return false;
        }
    }

    const char *Core::version() {
        return "MatriX.141 P2Play/0.1";
    }

    const char *Core::productVersion() {
        return P2P_NATIVE_VERSION;
    }

    std::string Core::hashOfLink(const std::string &link) {
        Torrs th;
        if (link.rfind("torrs://", 0) == 0) {
            return torrsUnpack(link, th) ? th.hash : std::string();
        }
        if (isHex40(link)) {
            return lower(link);
        }
        std::string magnet = isBase32(link) ? "magnet:?xt=urn:btih:" + link : link;
        if (magnet.rfind("magnet:", 0) != 0) {
            return "";
        }
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, ec);
        return ec ? std::string() : hexOf(atp.info_hashes);
    }

    int Core::activeStreams() {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = 0;
        for (const auto &t : m_torrents) {
            n += t.second->readers + (t.second->preloading ? 1 : 0);
        }
        return n;
    }

    int Core::activeRequests() const {
        return m_http.activeConnections();
    }

    void Core::stopAccepting() {
        m_http.stopAccepting();
    }

    std::string Core::echo() const {
        return m_config.platform.empty() ? std::string(version()) : std::string(version()) + " " + m_config.platform;
    }

    Core::Core(CoreConfig config) : m_config(std::move(config)), m_store(m_config.dataDir) {}

    Core::~Core() {
        stop();
    }

    void Core::log(const char *fmt, ...) {
        char line[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        std::string safe(line);
        const PublisherProxy proxy = m_store.publisherProxy();
        for (const std::string &secret : {proxy.username, proxy.password}) {
            if (secret.empty()) continue;
            for (const std::string &value : {secret, HttpServer::urlEncode(secret)}) {
                size_t pos = 0;
                while ((pos = safe.find(value, pos)) != std::string::npos) {
                    safe.replace(pos, value.size(), "[redacted]");
                    pos += std::strlen("[redacted]");
                }
            }
        }
        std::lock_guard<std::mutex> lock(m_logMutex);
        if (m_config.log) {
            m_config.log(safe);
        }
    }

    bool Core::start() {
        m_started = Clock::now();
        try {
            m_store.load();
            loadSettings();
        } catch (const std::exception &) {
            // Parsing exceptions may contain input text; do not log configuration
            // values. In particular never fall back to a direct session on failure.
            log("[Core] invalid or unreadable settings; refusing network startup");
            return false;
        }
        m_mesh.load(m_store.dir(), [this](const std::string &line) { log("%s", line.c_str()); });
        m_auth.load(m_store.dir());
        if (!m_config.ownerPassword.empty()) {
            const std::string authError = m_auth.setOwnerPassword(m_config.ownerPassword);
            if (!authError.empty()) {
                log("[Auth] cannot apply embedded owner password: %s", authError.c_str());
                return false;
            }
        }
        // one BitTorrent port for every socket, chosen once and kept (0: a random port per
        // socket, which the DHT cannot announce)
        if (m_settings.listenPort == 0) {
            std::random_device rd;
            m_settings.listenPort = 20000 + static_cast<int>(rd() % 40000);
            json s = m_store.settings();
            s["PeersListenPort"] = m_settings.listenPort;
            m_store.setSettings(s);
        }

        lt::settings_pack sp;
        const std::string port = std::to_string(m_settings.listenPort);
        sp.set_str(lt::settings_pack::listen_interfaces,
                   m_config.console ? "0.0.0.0:" + port : "0.0.0.0:" + port + ",[::]:" + port);
        sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::status | lt::alert_category::error |
                                                  lt::alert_category::storage | lt::alert_category::dht);
        sp.set_bool(lt::settings_pack::enable_lsd, !m_config.console);
        sp.set_bool(lt::settings_pack::enable_upnp, !m_config.console && !m_settings.disableUpnp);
        sp.set_bool(lt::settings_pack::enable_natpmp, !m_config.console && !m_settings.disableUpnp);
        // only the readers' windows are wanted: a torrent is "finished" between them and its
        // seeds must stay connected
        sp.set_bool(lt::settings_pack::close_redundant_connections, false);
        // torrents are opened on demand, none waits in a queue
        sp.set_int(lt::settings_pack::active_downloads, -1);
        sp.set_int(lt::settings_pack::active_seeds, -1);
        sp.set_int(lt::settings_pack::active_limit, -1);
        sp.set_str(lt::settings_pack::user_agent, std::string("P2Play/0.1 libtorrent/") + LIBTORRENT_VERSION);
        if (m_config.console) {
            sp.set_int(lt::settings_pack::max_peerlist_size, 500);
        }

        // DHT defaults include a hostname, which bypasses SOCKS5 resolution in
        // this libtorrent version. Clear it before the session starts. Explicit
        // proxied bootstrap entries have already been validated as numeric.
        const PublisherProxy proxy = m_store.publisherProxy();
        sp.set_bool(lt::settings_pack::enable_dht, !m_settings.disableDht);
        if (proxy.enabled || !m_settings.dhtBootstrap.empty()) {
            sp.set_str(lt::settings_pack::dht_bootstrap_nodes, m_settings.dhtBootstrap);
        }
        proxySettings(sp, proxy);
        lt::session_params params(std::move(sp));
        if (m_settings.useDisk) {
            params.disk_io_constructor = lt::posix_disk_io_constructor;
        } else {
            m_cache = std::make_unique<RamCache>(m_settings.cacheSize);
            // the MediaMesh list versions on disk, the streams in RAM
            params.disk_io_constructor = hybridDisk(m_cache->constructor(), m_store.dir() + "/lists");
        }
        try {
            m_session = std::make_unique<lt::session>(std::move(params));
        } catch (const std::exception &e) {
            log("[Core] session failed: %s", e.what());
            return false;
        }
        applySettings();
        if (proxy.enabled) {
            log("[Core] publisher SOCKS5 enabled (UDP ASSOCIATE required); no direct fallback; transport changes require restart");
        }

        // the library: closed torrents, opened when used
        for (TorrentRecord r : m_store.torrents()) {
            renumber(r);
            auto t = std::make_shared<Torrent>();
            t->rec = r;
            t->saved = !r.users.empty();
            m_torrents[r.hash] = t;
        }

        m_alertThread = std::thread(&Core::alertLoop, this);
        meshDirectory();
        meshTick(true);
        m_idleThread = std::thread(&Core::idleLoop, this);

        if (!m_http.start(m_config.httpPort, [this](const HttpRequest &req, HttpResponse &res) { handle(req, res); })) {
            log("[Web] cannot listen on port %d", m_config.httpPort);
            stop();
            return false;
        }
        log("[Core] %s, libtorrent %s, storage %s, http port %d, %d torrents in the library, the owner's password %s",
            version(), LIBTORRENT_VERSION,
            m_cache ? ("ram " + std::to_string(m_settings.cacheSize >> 20) + " MB").c_str() : "disk",
            m_http.port(), static_cast<int>(m_torrents.size()), m_auth.passwordSet() ? "set" : "not set: the first login sets it");
        return true;
    }

    void Core::stop() {
        if (m_stopping.exchange(true)) {
            return;
        }
        m_changed.notify_all();
        m_http.stop();
        if (m_alertThread.joinable()) {
            m_alertThread.join();
        }
        if (m_idleThread.joinable()) {
            m_idleThread.join();
        }
        m_session.reset();
        log("[Core] stopped");
    }

    // --- settings ---

    void Core::loadSettings() {
        json s = m_store.settings();
        auto i = [&](const char *k, int &v) { if (s.contains(k) && s[k].is_number()) v = s[k].get<int>(); };
        auto b = [&](const char *k, bool &v) { if (s.contains(k) && s[k].is_boolean()) v = s[k].get<bool>(); };
        if (s.contains("CacheSize") && s["CacheSize"].is_number()) {
            m_settings.cacheSize = std::max<int64_t>(16LL << 20, s["CacheSize"].get<int64_t>());
        }
        i("ReaderReadAHead", m_settings.readAhead);
        i("PreloadCache", m_settings.preloadCache);
        b("UseDisk", m_settings.useDisk);
        if (s.contains("TorrentsSavePath") && s["TorrentsSavePath"].is_string()) {
            m_settings.savePath = s["TorrentsSavePath"].get<std::string>();
        }
        i("TorrentDisconnectTimeout", m_settings.disconnectTimeout);
        i("ConnectionsLimit", m_settings.connectionsLimit);
        i("DownloadRateLimit", m_settings.downloadRateLimit);
        i("UploadRateLimit", m_settings.uploadRateLimit);
        b("DisableUpload", m_settings.disableUpload);
        b("DisableDHT", m_settings.disableDht);
        b("DisableUPNP", m_settings.disableUpnp);
        i("PeersListenPort", m_settings.listenPort);
        b("P2PCatalogOnly", m_settings.catalogOnly);
        if (s.contains("P2PDhtBootstrap") && s["P2PDhtBootstrap"].is_string()) {
            m_settings.dhtBootstrap = s["P2PDhtBootstrap"].get<std::string>();
        }
        m_settings.readAhead = std::min(100, std::max(5, m_settings.readAhead));
        m_settings.preloadCache = std::min(100, std::max(0, m_settings.preloadCache));
        m_settings.disconnectTimeout = std::max(5, m_settings.disconnectTimeout);
        m_settings.connectionsLimit = std::max(5, m_settings.connectionsLimit);
    }

    void Core::applySettings() {
        if (m_cache) {
            m_cache->setCapacity(m_settings.cacheSize);
        }
        if (!m_session) {
            return;
        }
        lt::settings_pack sp;
        sp.set_int(lt::settings_pack::connections_limit, m_settings.connectionsLimit * (m_config.console ? 2 : 4));
        sp.set_int(lt::settings_pack::download_rate_limit, m_settings.downloadRateLimit * 1024);
        sp.set_int(lt::settings_pack::upload_rate_limit,
                   m_settings.disableUpload ? 1 : m_settings.uploadRateLimit * 1024);
        sp.set_bool(lt::settings_pack::enable_dht, !m_settings.disableDht);
        // a DHT of our own (tests, a LAN): its nodes on private and loopback addresses allowed
        if (!m_settings.dhtBootstrap.empty()) {
            sp.set_str(lt::settings_pack::dht_bootstrap_nodes, m_settings.dhtBootstrap);
            sp.set_bool(lt::settings_pack::dht_restrict_routing_ips, false);
            sp.set_bool(lt::settings_pack::dht_restrict_search_ips, false);
            sp.set_bool(lt::settings_pack::dht_ignore_dark_internet, false);
            sp.set_bool(lt::settings_pack::dht_prefer_verified_node_ids, false);
            sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
            // the nodes may share an address: no rate limit by address
            sp.set_int(lt::settings_pack::dht_block_ratelimit, 100000);
            sp.set_int(lt::settings_pack::dht_block_timeout, 0);
        }
        const PublisherProxy proxy = m_store.publisherProxy();
        proxySettings(sp, proxy);
        if (proxy.enabled) {
            sp.set_str(lt::settings_pack::dht_bootstrap_nodes, m_settings.dhtBootstrap);
        }
        m_session->apply_settings(sp);
        // the nodes given known at once (the bootstrap list is read when the DHT starts)
        size_t pos = 0;
        while (pos < m_settings.dhtBootstrap.size()) {
            size_t end = m_settings.dhtBootstrap.find(',', pos);
            const std::string node = m_settings.dhtBootstrap.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            const size_t colon = node.rfind(':');
            if (colon != std::string::npos) {
                std::string host = node.substr(0, colon);
                if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
                    host = host.substr(1, host.size() - 2);
                }
                m_session->add_dht_node({host, std::atoi(node.c_str() + colon + 1)});
            }
            pos = end == std::string::npos ? m_settings.dhtBootstrap.size() : end + 1;
        }
    }

    json Core::settingsJson() {
        // TorrServer's BTSets; the stored ones on top of the defaults
        json s = {
                {"CacheSize", m_settings.cacheSize},
                {"ReaderReadAHead", m_settings.readAhead},
                {"PreloadCache", m_settings.preloadCache},
                {"UseDisk", m_settings.useDisk},
                {"TorrentsSavePath", m_settings.savePath},
                {"RemoveCacheOnDrop", false},
                {"ForceEncrypt", false},
                {"RetrackersMode", 1},
                {"TorrentDisconnectTimeout", m_settings.disconnectTimeout},
                {"EnableDebug", false},
                {"EnableDLNA", false},
                {"FriendlyName", ""},
                {"EnableRutorSearch", false},
                {"EnableIPv6", !m_config.console},
                {"DisableTCP", false},
                {"DisableUTP", false},
                {"DisableUPNP", m_settings.disableUpnp},
                {"DisableDHT", m_settings.disableDht},
                {"DisablePEX", false},
                {"DisableUpload", m_settings.disableUpload},
                {"DownloadRateLimit", m_settings.downloadRateLimit},
                {"UploadRateLimit", m_settings.uploadRateLimit},
                {"ConnectionsLimit", m_settings.connectionsLimit},
                {"PeersListenPort", m_settings.listenPort},
                {"ResponsiveMode", false},
                {"StoreSettingsInJson", true},
                {"StoreViewedInJson", true},
        };
        // the ones the core uses come from m_settings (checked values), the others as stored
        static const std::set<std::string> used = {
                "CacheSize", "ReaderReadAHead", "PreloadCache", "UseDisk", "TorrentsSavePath",
                "TorrentDisconnectTimeout", "ConnectionsLimit", "DownloadRateLimit", "UploadRateLimit",
                "DisableUpload", "DisableDHT", "DisableUPNP", "PeersListenPort"};
        json stored = m_store.publicSettings();
        for (auto it = stored.begin(); it != stored.end(); ++it) {
            if (!used.count(it.key())) {
                s[it.key()] = it.value();
            }
        }
        return s;
    }

    // --- torrents ---

    std::vector<Core::TorrentPtr> Core::list() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<TorrentPtr> out;
        for (const auto &t : m_torrents) {
            out.push_back(t.second);
        }
        std::sort(out.begin(), out.end(), [](const TorrentPtr &a, const TorrentPtr &b) {
            return a->rec.timestamp > b->rec.timestamp;
        });
        return out;
    }

    bool Core::addCard(const std::string &link, const std::string &user) {
        Torrs th;
        if (!torrsUnpack(link, th)) {
            return false;
        }
        TorrentPtr t;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_torrents.find(th.hash);
            if (it != m_torrents.end()) {
                t = it->second;
                if (t->rec.title.empty()) t->rec.title = th.title;
                if (t->rec.poster.empty()) t->rec.poster = th.poster;
                if (t->rec.category.empty()) t->rec.category = th.category;
            } else {
                t = std::make_shared<Torrent>();
                t->rec.hash = th.hash;
                t->rec.link = "magnet:?xt=urn:btih:" + th.hash;
                if (!th.title.empty()) {
                    t->rec.link += "&dn=" + HttpServer::urlEncode(th.title);
                }
                for (const std::string &tr : th.trackers) {
                    t->rec.link += "&tr=" + HttpServer::urlEncode(tr);
                }
                t->rec.title = th.title;
                t->rec.poster = th.poster;
                t->rec.category = th.category;
                t->rec.size = th.size;
                t->rec.timestamp = static_cast<int64_t>(time(nullptr));
                m_torrents[th.hash] = t;
            }
        }
        keep(t, user);
        save(t);
        return true;
    }

    std::string Core::torrsOf(const TorrentPtr &t) {
        TorrentRecord rec;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            rec = t->rec;
        }
        Torrs th;
        th.hash = rec.hash.substr(0, 40);
        th.title = rec.title == rec.name ? "" : rec.title;  // the name is in the metadata
        th.poster = rec.poster;
        th.category = rec.category;
        th.size = rec.size;
        // the magnet's trackers
        size_t pos = 0;
        while ((pos = rec.link.find("tr=", pos)) != std::string::npos) {
            if (pos > 0 && rec.link[pos - 1] != '&' && rec.link[pos - 1] != '?') {
                pos += 3;
                continue;
            }
            size_t end = rec.link.find('&', pos);
            th.trackers.push_back(HttpServer::urlDecode(rec.link.substr(pos + 3, end == std::string::npos ? std::string::npos : end - pos - 3), false));
            pos += 3;
        }
        return rec.hash.size() == 40 ? torrsPack(th) : std::string();
    }

    std::vector<Core::TorrentPtr> Core::listOf(const std::string &user, bool withOpen) {
        std::vector<TorrentPtr> out;
        for (const TorrentPtr &t : list()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (t->rec.users.count(user) || (withOpen && t->rec.users.empty())) {
                out.push_back(t);
            }
        }
        return out;
    }

    bool Core::owns(const TorrentPtr &t, const std::string &user) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return t->rec.users.count(user) > 0;
    }

    void Core::keep(const TorrentPtr &t, const std::string &user) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->rec.users.emplace(user, static_cast<int64_t>(time(nullptr))).second && t->saved) {
                return;
            }
            t->saved = true;
        }
        save(t);
    }

    void Core::unlink(const TorrentPtr &t, const std::string &user) {
        bool last;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->rec.users.erase(user);
            last = t->rec.users.empty();
        }
        m_store.removeViewed(user, t->rec.hash, 0);
        if (last) {
            remove(t);
        } else {
            save(t);
        }
    }

    void Core::removeUser(const std::string &user) {
        for (const TorrentPtr &t : listOf(user, false)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->rec.users.erase(user);
        }
        for (const std::string &hash : m_store.removeUser(user)) {
            if (TorrentPtr t = find(hash)) {
                remove(t);
            }
        }
    }

    Core::TorrentPtr Core::find(const std::string &hash) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_torrents.find(lower(hash));
        return it == m_torrents.end() ? nullptr : it->second;
    }

    Core::TorrentPtr Core::add(const std::string &linkIn, const std::string &title, const std::string &poster,
                               const std::string &category, const std::string &data, const std::string &saveFor,
                               std::string &error) {
        const bool save = !saveFor.empty();
        std::string link = linkIn;
        size_t amp;
        while ((amp = link.find("&amp;")) != std::string::npos) {
            link.replace(amp, 5, "&");
        }
        if (link.rfind("http://", 0) == 0 || link.rfind("https://", 0) == 0) {
            if (m_store.publisherProxy().enabled) {
                error = "HTTP torrent fetch is disabled with PublisherProxy; upload metadata or use a magnet";
                return nullptr;
            }
            std::string body;
            if (!httpGet(link, body, error)) {
                return nullptr;
            }
            return addMetadata(body, title, poster, category, data, saveFor, error);
        }
        if (link.rfind("file://", 0) == 0 || (!link.empty() && link[0] == '/')) {
            std::string body;
            if (!readFile(link.rfind("file://", 0) == 0 ? link.substr(7) : link, body)) {
                error = "cannot read " + link;
                return nullptr;
            }
            return addMetadata(body, title, poster, category, data, saveFor, error);
        }
        // torrs://: the infohash, the trackers and the card's fields (those given win)
        std::string title2 = title, poster2 = poster, category2 = category;
        if (link.rfind("torrs://", 0) == 0) {
            Torrs th;
            if (!torrsUnpack(link, th)) {
                error = "bad torrs:// link";
                return nullptr;
            }
            link = "magnet:?xt=urn:btih:" + th.hash;
            if (!th.title.empty()) {
                link += "&dn=" + HttpServer::urlEncode(th.title);
            }
            for (const std::string &tr : th.trackers) {
                link += "&tr=" + HttpServer::urlEncode(tr);
            }
            if (title2.empty()) title2 = th.title;
            if (poster2.empty()) poster2 = th.poster;
            if (category2.empty()) category2 = th.category;
        }
        std::string magnet = link;
        if (isHex40(link) || isBase32(link)) {
            magnet = "magnet:?xt=urn:btih:" + link;
        }
        if (magnet.rfind("magnet:", 0) != 0) {
            error = "unknown link: a magnet, an infohash or a .torrent link is expected";
            return nullptr;
        }
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, ec);
        if (ec) {
            error = "bad magnet: " + ec.message();
            return nullptr;
        }
        const std::string hash = hexOf(atp.info_hashes);

        TorrentPtr t;
        bool filled = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_torrents.find(hash);
            if (it != m_torrents.end()) {
                t = it->second;
            } else {
                t = std::make_shared<Torrent>();
                t->rec.hash = hash;
                t->rec.link = magnet;
                t->rec.timestamp = static_cast<int64_t>(time(nullptr));
                if (!atp.name.empty()) {
                    t->rec.title = atp.name;
                }
                m_torrents[hash] = t;
            }
            // MatriX's AddTorrent: a field given fills an empty one (set changes them)
            const TorrentRecord before = t->rec;
            if (!title2.empty() && (t->rec.title.empty() || t->rec.title == atp.name)) t->rec.title = title2;
            if (!poster2.empty() && t->rec.poster.empty()) t->rec.poster = poster2;
            if (!category2.empty() && t->rec.category.empty()) t->rec.category = category2;
            if (!data.empty() && t->rec.data.empty()) t->rec.data = data;
            filled = t->saved && (before.title != t->rec.title || before.poster != t->rec.poster ||
                                  before.category != t->rec.category || before.data != t->rec.data);
            t->lastUse = Clock::now();
        }
        if (!activate(t, error)) {
            return nullptr;
        }
        if (save) {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->rec.users.emplace(saveFor, static_cast<int64_t>(time(nullptr)));
            t->saved = true;
        }
        if (save || filled) {
            this->save(t);
        }
        return t;
    }

    // a record of an earlier version: its files numbered again from the metadata kept (as
    // TorrServer, see torrServerFiles), the progress moved along by path
    void Core::renumber(TorrentRecord &r) {
        std::string raw;
        std::shared_ptr<const lt::torrent_info> ti;
        lt::error_code ec;
        if (readFile(m_store.resumePath(r.hash), raw)) {
            lt::add_torrent_params atp = lt::read_resume_data({raw.data(), static_cast<std::ptrdiff_t>(raw.size())}, ec);
            if (!ec && atp.ti) {
                ti = atp.ti;
            }
        }
        if (!ti && readFile(m_store.metadataPath(r.hash), raw)) {
            auto info = std::make_shared<lt::torrent_info>(raw.data(), static_cast<int>(raw.size()), ec);
            if (!ec) {
                ti = info;
            }
        }
        if (!ti) {
            return;
        }
        int64_t size = 0;
        std::map<int, lt::file_index_t> index;
        std::vector<FileInfo> files = torrServerFiles(*ti, size, index);
        bool same = files.size() == r.files.size();
        for (size_t i = 0; same && i < files.size(); i++) {
            same = files[i].id == r.files[i].id && files[i].path == r.files[i].path && files[i].index == r.files[i].index;
        }
        if (same) {
            return;
        }
        std::map<int, int> moved;
        for (const FileInfo &old : r.files) {
            for (const FileInfo &f : files) {
                if (f.path == old.path && f.id != old.id) {
                    moved[old.id] = f.id;
                }
            }
        }
        log("[Torrent:%s] files numbered again as TorrServer (%d moved)", r.hash.c_str(), static_cast<int>(moved.size()));
        r.files = files;
        r.size = size;
        m_store.putTorrent(r);
        m_store.renumberViewed(r.hash, moved);
    }

    Core::TorrentPtr Core::addMetadata(const std::string &torrentFile, const std::string &title, const std::string &poster,
                                       const std::string &category, const std::string &data,
                                       const std::string &saveFor, std::string &error) {
        lt::error_code ec;
        auto ti = std::make_shared<lt::torrent_info>(torrentFile.data(), static_cast<int>(torrentFile.size()), ec);
        if (ec) {
            error = "bad .torrent: " + ec.message();
            return nullptr;
        }
        const std::string hash = hexOf(ti->info_hashes());
        // the metadata kept: the torrent opens again without peers for it
        {
            std::string path = m_store.metadataPath(hash);
            if (!fileExists(path)) {
                FILE *f = std::fopen(path.c_str(), "wb");
                if (f) {
                    std::fwrite(torrentFile.data(), 1, torrentFile.size(), f);
                    std::fclose(f);
                }
            }
        }
        std::string magnet = lt::make_magnet_uri(*ti);
        return add(magnet, title.empty() ? ti->name() : title, poster, category, data, saveFor, error);
    }

    bool Core::activate(const TorrentPtr &t, std::string &error) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (t->active) {
                return true;
            }
        }
        lt::add_torrent_params atp;
        lt::error_code ec;
        std::string resume;
        std::string meta = m_store.metadataPath(t->rec.hash);
        if (readFile(m_store.resumePath(t->rec.hash), resume)) {
            atp = lt::read_resume_data({resume.data(), static_cast<std::ptrdiff_t>(resume.size())}, ec);
            if (ec || !atp.ti) {
                atp = lt::add_torrent_params();
            } else {
                // nothing kept in memory between runs; the windows decide what is downloaded
                if (m_cache) {
                    atp.have_pieces.clear();
                    atp.verified_pieces.clear();
                    atp.unfinished_pieces.clear();
                }
                atp.piece_priorities.clear();
                atp.file_priorities.assign(static_cast<size_t>(atp.ti->num_files()), lt::dont_download);
            }
            ec.clear();
        }
        if (!atp.ti && fileExists(meta)) {
            auto ti = std::make_shared<lt::torrent_info>(meta, ec);
            if (!ec) {
                atp.ti = ti;
                atp.file_priorities.assign(static_cast<size_t>(ti->num_files()), lt::dont_download);
            }
            ec.clear();
        }
        if (atp.ti) {
            // the trackers and peers of the link it was added with
            lt::error_code mec;
            lt::add_torrent_params m = lt::parse_magnet_uri(t->rec.link, mec);
            if (!mec) {
                atp.tracker_tiers.resize(atp.trackers.size(), 0);
                for (const auto &tr : m.trackers) {
                    if (std::find(atp.trackers.begin(), atp.trackers.end(), tr) == atp.trackers.end()) {
                        atp.trackers.push_back(tr);
                        atp.tracker_tiers.push_back(0);
                    }
                }
                atp.peers.insert(atp.peers.end(), m.peers.begin(), m.peers.end());
            }
        } else {
            atp = lt::parse_magnet_uri(t->rec.link.empty() ? "magnet:?xt=urn:btih:" + t->rec.hash : t->rec.link, ec);
            if (ec) {
                error = "bad magnet: " + ec.message();
                return false;
            }
        }
        if (m_store.publisherProxy().enabled && !proxyTorrent(atp, error)) {
            return false;
        }
        atp.save_path = m_settings.savePath.empty() ? m_store.dir() + "/downloads" : m_settings.savePath;
        // opened at once, never queued or paused by the session
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
        lt::torrent_handle h = m_session->add_torrent(std::move(atp), ec);
        if (ec) {
            error = "cannot add the torrent: " + ec.message();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->handle = h;
            t->active = true;
            t->lastUse = Clock::now();
        }
        log("[Torrent:%s] opened", t->rec.hash.c_str());
        if (h.torrent_file()) {
            onMetadata(t);
        }
        return true;
    }

    void Core::onMetadata(const TorrentPtr &t) {
        lt::torrent_handle h;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->active || t->gotInfo) {
                return;
            }
            h = t->handle;
        }
        std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();
        if (!ti) {
            return;
        }
        const lt::file_storage &fs = ti->files();
        int64_t size = 0;
        std::map<int, lt::file_index_t> index;
        std::vector<FileInfo> files = torrServerFiles(*ti, size, index);
        // nothing is downloaded but the readers' windows
        h.prioritize_files(std::vector<lt::download_priority_t>(static_cast<size_t>(fs.num_files()), lt::dont_download));

        // the metadata kept for the next opening: resume data (with the v2 piece layers once
        // known; written again when the torrent is closed)
        h.save_resume_data(lt::torrent_handle::save_info_dict);
        bool saved;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->rec.files = files;
            t->rec.name = ti->name();
            t->rec.size = size;
            if (t->rec.title.empty()) {
                t->rec.title = ti->name();
            }
            t->fileIndex = index;
            t->pieceLength = ti->piece_length();
            t->numPieces = ti->num_pieces();
            t->cacheKey = h.info_hashes().get_best();
            t->gotInfo = true;
            saved = t->saved;
        }
        if (saved) {
            save(t);
        }
        log("[Torrent:%s] metadata: %s, %d files, %.1f MB", t->rec.hash.c_str(), ti->name().c_str(),
            static_cast<int>(files.size()), static_cast<double>(size) / 1048576.0);
        m_changed.notify_all();
    }

    bool Core::waitInfo(const TorrentPtr &t, std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&] { return m_stopping || t->gotInfo || !t->active; }) &&
               t->gotInfo && !m_stopping;
    }

    void Core::save(const TorrentPtr &t) {
        TorrentRecord rec;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->saved) {
                return;
            }
            rec = t->rec;
        }
        m_store.putTorrent(rec);
    }

    void Core::close(const TorrentPtr &t) {
        lt::torrent_handle h;
        bool saved;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->active) {
                return;
            }
            h = t->handle;
            t->active = false;
            t->gotInfo = false;
            t->handle = lt::torrent_handle();
            t->windows.clear();
            t->wanted.clear();
            t->pinned.clear();
            saved = t->saved;
            if (!saved) {
                m_torrents.erase(t->rec.hash);
            }
        }
        if (saved) {
            // the resume data (peers, piece layers) before the torrent goes
            std::unique_lock<std::mutex> lock(m_mutex);
            const uint64_t before = m_resumeWrites;
            lock.unlock();
            h.save_resume_data(lt::torrent_handle::save_info_dict);
            lock.lock();
            m_changed.wait_for(lock, std::chrono::seconds(3), [&] { return m_resumeWrites != before || m_stopping; });
        }
        m_session->remove_torrent(h);
        if (!saved) {
            std::remove(m_store.metadataPath(t->rec.hash).c_str());
            std::remove(m_store.resumePath(t->rec.hash).c_str());
        }
        log("[Torrent:%s] closed%s", t->rec.hash.c_str(), saved ? "" : " and forgotten (not in the library)");
        m_changed.notify_all();
    }

    void Core::remove(const TorrentPtr &t) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->saved = false;
        }
        close(t);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_torrents.erase(t->rec.hash);
        }
        m_store.removeTorrent(t->rec.hash);
        std::remove(m_store.metadataPath(t->rec.hash).c_str());
        std::remove(m_store.resumePath(t->rec.hash).c_str());
    }

    json Core::filesData(const TorrentPtr &t) {
        json files = json::array();
        for (const FileInfo &f : t->rec.files) {
            files.push_back({{"id", f.id}, {"path", f.path}, {"length", f.length}});
        }
        return files;
    }

    json Core::status(const TorrentPtr &t) {
        lt::torrent_handle h;
        bool active, gotInfo, preloading, saved;
        TorrentRecord rec;
        int64_t preloadSize, preloadedBytes;
        lt::sha1_hash key;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            h = t->handle;
            active = t->active;
            gotInfo = t->gotInfo;
            preloading = t->preloading;
            saved = t->saved;
            rec = t->rec;
            preloadSize = t->preloadSize;
            preloadedBytes = t->preloadedBytes;
            key = t->cacheKey;
        }
        int stat = !active ? (saved ? 5 : 4) : !gotInfo ? 1 : preloading ? 2 : 3;
        static const char *statNames[] = {"Torrent added", "Torrent getting info", "Torrent preload",
                                          "Torrent working", "Torrent closed", "Torrent in db"};
        // data: the client's JSON with the files added, as TorrServer keeps it
        std::string data = rec.data;
        if (!rec.files.empty()) {
            json d = rec.data.empty() ? json::object() : json::parse(rec.data, nullptr, false);
            if (d.is_object()) {
                d["TorrServer"]["Files"] = filesData(t);
                data = d.dump();
            }
        }
        json j = {
                {"title", rec.title},
                {"category", rec.category},
                {"poster", rec.poster},
                {"data", data},
                {"timestamp", rec.timestamp},
                {"name", rec.name},
                {"hash", rec.hash},
                {"stat", stat},
                {"stat_string", statNames[stat]},
                {"torrent_size", rec.size},
                {"preloaded_bytes", preloadedBytes},
                {"preload_size", preloadSize},
        };
        lt::torrent_status st;
        if (active) {
            try {
                st = h.status();
            } catch (const std::exception &) {
                active = false;  // closed meanwhile
            }
        }
        if (active) {
            j["download_speed"] = static_cast<double>(st.download_payload_rate);
            j["upload_speed"] = static_cast<double>(st.upload_payload_rate);
            j["total_peers"] = st.list_peers;
            j["pending_peers"] = std::max(0, st.list_peers - st.num_peers);
            j["active_peers"] = st.num_peers;
            j["connected_seeders"] = st.num_seeds;
            j["half_open_peers"] = st.num_connections - st.num_peers > 0 ? st.num_connections - st.num_peers : 0;
            j["bytes_read"] = st.total_payload_download;
            j["bytes_read_data"] = st.total_payload_download;
            j["bytes_written"] = st.total_payload_upload;
            j["bytes_written_data"] = st.total_payload_upload;
            j["loaded_size"] = m_cache ? m_cache->bytesOf(key) : st.total_wanted_done;
        }
        if (!rec.files.empty()) {
            j["file_stats"] = filesData(t);
        }
        return j;
    }

    // --- loops ---

    void Core::alertLoop() {
        while (!m_stopping) {
            try {
                alertStep();
            } catch (const std::exception &e) {
                log("[Torrent] alert handling failed: %s", e.what());
            }
        }
    }

    void Core::alertStep() {
        {
            if (!m_session->wait_for_alert(lt::milliseconds(200))) {
                return;
            }
            std::vector<lt::alert *> alerts;
            m_session->pop_alerts(&alerts);
            for (lt::alert *a : alerts) {
                try {
                    handleAlert(a);
                } catch (const std::exception &e) {
                    log("[Torrent] %s: %s", a->what(), e.what());
                }
            }
        }
    }

    // a torrent may be closed while its alerts wait: its handle throws
    void Core::handleAlert(lt::alert *a) {
        if (meshAlert(a)) {
            return;
        }
        {
            {
                if (auto *rp = lt::alert_cast<lt::read_piece_alert>(a)) {
                    TorrentPtr t;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        for (const auto &e : m_torrents) {
                            if (e.second->active && e.second->handle == rp->handle) {
                                t = e.second;
                            }
                        }
                    }
                    if (t) {
                        std::lock_guard<std::mutex> lock(m_readsMutex);
                        auto key = std::make_pair(t->rec.hash, static_cast<int>(rp->piece));
                        if (rp->error) {
                            m_readsFailed.insert(key);
                        } else {
                            m_reads[key] = std::make_shared<const std::vector<char>>(rp->buffer.get(),
                                                                                     rp->buffer.get() + rp->size);
                        }
                    }
                    m_changed.notify_all();
                } else if (auto *sr = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                    std::string hash = hexOf(sr->params.info_hashes);
                    std::vector<char> buf = lt::write_resume_data_buf(sr->params);
                    std::string path = m_store.resumePath(hash), tmp = path + ".tmp";
                    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
                        bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
                        ok = std::fclose(f) == 0 && ok;
                        if (ok && std::rename(tmp.c_str(), path.c_str()) != 0) {
                            std::remove(path.c_str());
                            std::rename(tmp.c_str(), path.c_str());
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_resumeWrites++;
                    }
                    m_changed.notify_all();
                } else if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_resumeWrites++;
                    }
                    m_changed.notify_all();
                } else if (auto *mr = lt::alert_cast<lt::metadata_received_alert>(a)) {
                    TorrentPtr t;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        for (const auto &e : m_torrents) {
                            if (e.second->active && e.second->handle == mr->handle) {
                                t = e.second;
                            }
                        }
                    }
                    if (t) {
                        onMetadata(t);
                    }
                } else if (lt::alert_cast<lt::torrent_error_alert>(a) || lt::alert_cast<lt::file_error_alert>(a) ||
                           lt::alert_cast<lt::listen_failed_alert>(a) || lt::alert_cast<lt::listen_succeeded_alert>(a) ||
                           lt::alert_cast<lt::metadata_failed_alert>(a)) {
                    log("[Torrent] %s", a->message().c_str());
                }
            }
        }
    }

    // torrents without readers closed after the timeout; read_piece results dropped
    void Core::idleLoop() {
        while (!m_stopping) {
            try {
                idleStep();
            } catch (const std::exception &e) {
                log("[Torrent] closing idle torrents failed: %s", e.what());
            }
        }
    }

    void Core::idleStep() {
        {
            for (int i = 0; i < 10 && !m_stopping; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::vector<TorrentPtr> idle;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto &e : m_torrents) {
                    const Torrent &t = *e.second;
                    if (t.active && t.readers == 0 && !t.preloading &&
                        Clock::now() - t.lastUse > std::chrono::seconds(m_settings.disconnectTimeout)) {
                        idle.push_back(e.second);
                    }
                }
            }
            for (const auto &t : idle) {
                close(t);
            }
            meshTick(false);
            std::lock_guard<std::mutex> lock(m_readsMutex);
            if (m_reads.size() > 16) {
                m_reads.clear();
            }
            m_readsFailed.clear();
        }
    }

}
