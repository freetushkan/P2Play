// p2spike: stage 0 of the roadmap. A magnet (or a .torrent) in, the largest file out
// over HTTP with Range, so a player on this or another device can stream it.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <set>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>

#include "http_server.hpp"
#include "platform.hpp"

using namespace p2p;
using Clock = std::chrono::steady_clock;

namespace {

    struct Config {
        std::string link;
        std::vector<std::string> peers;
        uint16_t httpPort = 8095;
        uint16_t btPort = 6881;
        int64_t readahead = 16 * 1024 * 1024;
    };

    struct StreamFile {
        std::mutex mutex;
        bool ready = false;
        lt::torrent_handle handle;
        std::string name;
        int64_t size = 0;
        int64_t offset = 0;
        int pieceLength = 0;
        int firstPiece = 0;
        int lastPiece = 0;
    };

    StreamFile g_stream;
    std::atomic<bool> g_stopping{false};
    std::atomic<int> g_clients{0};
    std::mutex g_logMutex;
    std::FILE *g_logFile = nullptr;

    void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    void log(const char *fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_logMutex);
        platformPrint(buf);
        if (g_logFile) {
            time_t now = time(nullptr);
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&now));
            fprintf(g_logFile, "%s %s\n", stamp, buf);
            fflush(g_logFile);
        }
    }

    std::string trim(const std::string &s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? "" : s.substr(b, e - b + 1);
    }

    void applyOption(Config &cfg, const std::string &key, const std::string &value) {
        if (key == "peer") {
            cfg.peers.push_back(value);
        } else if (key == "port") {
            cfg.btPort = static_cast<uint16_t>(std::stoi(value));
        } else if (key == "http") {
            cfg.httpPort = static_cast<uint16_t>(std::stoi(value));
        } else if (key == "readahead") {
            cfg.readahead = std::stoll(value) * 1024 * 1024;
        }
    }

    // args: <link> [--peer ip:port] [--port n] [--http n] [--readahead mb]
    // or <data dir>/magnet.txt: the link on the first line, then "peer ip:port", "port n"...
    Config loadConfig(int argc, char **argv) {
        Config cfg;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
                applyOption(cfg, arg.substr(2), argv[++i]);
            } else if (cfg.link.empty()) {
                cfg.link = arg;
            }
        }
        if (!cfg.link.empty()) {
            return cfg;
        }

        std::ifstream in(dataDir() + "/magnet.txt");
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            size_t space = line.find(' ');
            if (cfg.link.empty() && space == std::string::npos) {
                cfg.link = line;
            } else if (space != std::string::npos) {
                applyOption(cfg, line.substr(0, space), trim(line.substr(space + 1)));
            }
        }
        return cfg;
    }

    std::string localAddress() {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            return "127.0.0.1";
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53);
        inet_pton(AF_INET, "1.1.1.1", &addr.sin_addr);
        std::string out = "127.0.0.1";
        if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            sockaddr_in self{};
            socklen_t len = sizeof(self);
            char buf[INET_ADDRSTRLEN];
            if (getsockname(s, reinterpret_cast<sockaddr *>(&self), &len) == 0 &&
                inet_ntop(AF_INET, &self.sin_addr, buf, sizeof(buf))) {
                out = buf;
            }
        }
        close(s);
        return out;
    }

    bool isHex40(const std::string &s) {
        return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return isxdigit(c); });
    }

    const char *stateName(lt::torrent_status::state_t s) {
        switch (s) {
            case lt::torrent_status::checking_files: return "checking";
            case lt::torrent_status::downloading_metadata: return "metadata";
            case lt::torrent_status::downloading: return "downloading";
            case lt::torrent_status::finished: return "finished";
            case lt::torrent_status::seeding: return "seeding";
            case lt::torrent_status::checking_resume_data: return "resume";
            default: return "unknown";
        }
    }

    std::string jsonEscape(const std::string &s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
        return out;
    }

    void onMetadata(const lt::torrent_handle &h) {
        auto ti = h.torrent_file();
        if (!ti) {
            return;
        }
        const lt::file_storage &fs = ti->files();
        lt::file_index_t best{0};
        for (lt::file_index_t i : fs.file_range()) {
            if (fs.file_size(i) > fs.file_size(best)) {
                best = i;
            }
        }

        std::vector<lt::download_priority_t> prio(static_cast<size_t>(fs.num_files()), lt::dont_download);
        prio[static_cast<size_t>(static_cast<int>(best))] = lt::default_priority;
        h.prioritize_files(prio);

        std::lock_guard<std::mutex> lock(g_stream.mutex);
        g_stream.handle = h;
        g_stream.name = std::string(fs.file_name(best));
        g_stream.size = fs.file_size(best);
        g_stream.offset = fs.file_offset(best);
        g_stream.pieceLength = ti->piece_length();
        g_stream.firstPiece = static_cast<int>(g_stream.offset / g_stream.pieceLength);
        g_stream.lastPiece = static_cast<int>((g_stream.offset + std::max<int64_t>(g_stream.size, 1) - 1) / g_stream.pieceLength);
        g_stream.ready = true;

        // container headers and indexes live at both ends
        h.set_piece_deadline(lt::piece_index_t{g_stream.firstPiece}, 0);
        h.set_piece_deadline(lt::piece_index_t{g_stream.lastPiece}, 0);

        log("metadata: %s, %d files, piece %d KB; streaming \"%s\" (%.1f MB)",
            ti->name().c_str(), fs.num_files(), g_stream.pieceLength / 1024,
            g_stream.name.c_str(), static_cast<double>(g_stream.size) / 1048576.0);
    }

    // Pieces through the disk subsystem: in libtorrent 2.0 a piece passes the hash check
    // before its write reaches the file, so the file itself is not read directly.
    class PieceReader {
    public:
        using Buffer = std::shared_ptr<const std::vector<char>>;

        Buffer read(const lt::torrent_handle &h, int piece, std::chrono::seconds timeout) {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto it = m_done.find(piece);
            if (it != m_done.end()) {
                return it->second;
            }
            if (m_pending.insert(piece).second) {
                h.read_piece(lt::piece_index_t{piece});
            }
            bool ok = m_cv.wait_for(lock, timeout, [&] {
                return g_stopping || m_done.count(piece) || m_failed.count(piece);
            });
            if (!ok || g_stopping || m_failed.erase(piece)) {
                return nullptr;
            }
            return m_done[piece];
        }

        void onAlert(const lt::read_piece_alert *a) {
            int piece = static_cast<int>(a->piece);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.erase(piece);
            if (a->error) {
                m_failed.insert(piece);
            } else {
                m_done[piece] = std::make_shared<const std::vector<char>>(a->buffer.get(), a->buffer.get() + a->size);
                m_order.push_back(piece);
                m_bytes += a->size;
                while (m_bytes > kMaxBytes && m_order.size() > 2) {
                    int old = m_order.front();
                    m_order.pop_front();
                    auto it = m_done.find(old);
                    if (it != m_done.end()) {
                        m_bytes -= static_cast<int64_t>(it->second->size());
                        m_done.erase(it);
                    }
                }
            }
            m_cv.notify_all();
        }

        void wake() {
            m_cv.notify_all();
        }

    private:
        static constexpr int64_t kMaxBytes = 24 * 1024 * 1024;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::map<int, Buffer> m_done;
        std::set<int> m_pending;
        std::set<int> m_failed;
        std::deque<int> m_order;
        int64_t m_bytes = 0;
    };

    PieceReader g_reader;

    bool waitPiece(const lt::torrent_handle &h, int piece, int lastPiece, int ahead) {
        for (int i = 0; i < ahead && piece + i <= lastPiece; i++) {
            lt::piece_index_t p{piece + i};
            if (!h.have_piece(p)) {
                h.set_piece_deadline(p, i * 100);
            }
        }
        auto deadline = Clock::now() + std::chrono::minutes(3);
        while (!h.have_piece(lt::piece_index_t{piece})) {
            if (g_stopping || Clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }

    std::string mimeType(const std::string &name) {
        auto dot = name.rfind('.');
        std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "mkv") return "video/x-matroska";
        if (ext == "mp4" || ext == "m4v") return "video/mp4";
        if (ext == "avi") return "video/x-msvideo";
        if (ext == "ts") return "video/mp2t";
        if (ext == "webm") return "video/webm";
        return "application/octet-stream";
    }

    void serveStream(int fd, const HttpRequest &req, int64_t readahead) {
        lt::torrent_handle h;
        std::string name;
        int64_t size, offset;
        int pieceLength, lastPiece;
        {
            std::lock_guard<std::mutex> lock(g_stream.mutex);
            if (!g_stream.ready) {
                HttpServer::sendText(fd, 503, "text/plain", "metadata not received yet\n");
                return;
            }
            h = g_stream.handle;
            name = g_stream.name;
            size = g_stream.size;
            offset = g_stream.offset;
            pieceLength = g_stream.pieceLength;
            lastPiece = g_stream.lastPiece;
        }

        int64_t start = req.hasRange ? req.rangeStart : 0;
        int64_t end = req.hasRange && req.rangeEnd >= 0 ? std::min(req.rangeEnd, size - 1) : size - 1;
        if (start >= size || start > end) {
            std::string out = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                              std::to_string(size) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            HttpServer::sendAll(fd, out.data(), out.size());
            return;
        }

        std::string head = req.hasRange ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        head += "Content-Type: " + mimeType(name) + "\r\n";
        head += "Accept-Ranges: bytes\r\n";
        head += "Content-Length: " + std::to_string(end - start + 1) + "\r\n";
        if (req.hasRange) {
            head += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                    std::to_string(size) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";
        if (!HttpServer::sendAll(fd, head.data(), head.size()) || req.method == "HEAD") {
            return;
        }

        g_clients++;
        log("stream: %s bytes %lld-%lld", req.method.c_str(), (long long) start, (long long) end);
        const int ahead = std::max<int>(2, static_cast<int>(readahead / pieceLength));
        int64_t pos = start;
        int64_t sent = 0;
        auto began = Clock::now();

        while (pos <= end && !g_stopping) {
            int piece = static_cast<int>((offset + pos) / pieceLength);
            if (!waitPiece(h, piece, lastPiece, ahead)) {
                log("stream: piece %d timed out", piece);
                break;
            }
            PieceReader::Buffer data = g_reader.read(h, piece, std::chrono::seconds(30));
            if (!data) {
                log("stream: piece %d read failed", piece);
                break;
            }
            int64_t pieceStart = static_cast<int64_t>(piece) * pieceLength - offset;
            int64_t from = pos - pieceStart;
            int64_t to = std::min<int64_t>(static_cast<int64_t>(data->size()), end + 1 - pieceStart);
            if (from >= to || !HttpServer::sendAll(fd, data->data() + from, static_cast<size_t>(to - from))) {
                break;
            }
            pos += to - from;
            sent += to - from;
        }
        double secs = std::chrono::duration<double>(Clock::now() - began).count();
        log("stream: closed after %.1f MB in %.0f s", static_cast<double>(sent) / 1048576.0, secs);
        g_clients--;
    }

    std::string statusJson(const lt::session &ses, const lt::torrent_handle &h, int dhtNodes) {
        lt::torrent_status st = h.status();
        std::ostringstream o;
        o << "{\"platform\":\"" << platformName() << "\""
          << ",\"libtorrent\":\"" << LIBTORRENT_VERSION << "\""
          << ",\"name\":\"" << jsonEscape(st.name) << "\""
          << ",\"state\":\"" << stateName(st.state) << "\""
          << ",\"progress\":" << st.progress
          << ",\"peers\":" << st.num_peers
          << ",\"seeds\":" << st.num_seeds
          << ",\"download_rate\":" << st.download_rate
          << ",\"upload_rate\":" << st.upload_rate
          << ",\"dht_nodes\":" << dhtNodes
          << ",\"clients\":" << g_clients.load();
        {
            std::lock_guard<std::mutex> lock(g_stream.mutex);
            if (g_stream.ready) {
                o << ",\"file\":\"" << jsonEscape(g_stream.name) << "\""
                  << ",\"file_size\":" << g_stream.size;
            }
        }
        o << "}\n";
        (void) ses;
        return o.str();
    }

}

namespace {
    // the reason of an uncaught exception, before abort()
    void onTerminate() {
        const char *what = "unknown";
        try {
            std::exception_ptr e = std::current_exception();
            if (e) {
                std::rethrow_exception(e);
            }
            what = "terminate without an exception";
        } catch (const std::exception &ex) {
            what = ex.what();
        } catch (...) {
        }
        log("fatal: uncaught exception: %s", what);
        std::abort();
    }
}

int main(int argc, char **argv) {
    std::set_terminate(onTerminate);
    if (!platformInit()) {
        return 1;
    }
    const std::string dir = dataDir();
    const std::string savePath = dir + "/downloads";
    mkdir(savePath.c_str(), 0777);
    g_logFile = std::fopen((dir + "/spike.log").c_str(), "w");

    log("p2spike %s, libtorrent %s, platform %s", "0.1", LIBTORRENT_VERSION, platformName().c_str());

    Config cfg = loadConfig(argc, argv);
    if (cfg.link.empty()) {
        log("no link: put a magnet into %s/magnet.txt", dir.c_str());
        while (platformRunning()) {
        }
        platformExit();
        return 0;
    }

    lt::settings_pack sp;
    const bool console = platformIsConsole();
    sp.set_str(lt::settings_pack::listen_interfaces,
               console ? "0.0.0.0:" + std::to_string(cfg.btPort)
                       : "0.0.0.0:" + std::to_string(cfg.btPort) + ",[::]:" + std::to_string(cfg.btPort));
    sp.set_int(lt::settings_pack::alert_mask,
               lt::alert_category::status | lt::alert_category::error | lt::alert_category::storage |
               lt::alert_category::dht);
    sp.set_bool(lt::settings_pack::enable_dht, true);
    sp.set_bool(lt::settings_pack::enable_lsd, !console);
    sp.set_bool(lt::settings_pack::enable_upnp, !console);
    sp.set_bool(lt::settings_pack::enable_natpmp, !console);
    sp.set_str(lt::settings_pack::user_agent, std::string("P2Play-spike/0.1 libtorrent/") + LIBTORRENT_VERSION);
    if (console) {
        sp.set_int(lt::settings_pack::connections_limit, 60);
        sp.set_int(lt::settings_pack::max_peerlist_size, 500);
    }

    lt::session_params params(std::move(sp));
    params.disk_io_constructor = lt::posix_disk_io_constructor;

    std::unique_ptr<lt::session> ses;
    try {
        ses = std::make_unique<lt::session>(std::move(params));
    } catch (const std::exception &e) {
        log("session failed: %s", e.what());
        while (platformRunning()) {
        }
        platformExit();
        return 1;
    }

    lt::add_torrent_params atp;
    lt::error_code ec;
    if (cfg.link.rfind("magnet:", 0) == 0) {
        atp = lt::parse_magnet_uri(cfg.link, ec);
    } else if (isHex40(cfg.link)) {
        atp = lt::parse_magnet_uri("magnet:?xt=urn:btih:" + cfg.link, ec);
    } else {
        atp.ti = std::make_shared<lt::torrent_info>(cfg.link, ec);
    }
    if (ec) {
        log("bad link: %s", ec.message().c_str());
        while (platformRunning()) {
        }
        platformExit();
        return 1;
    }
    atp.save_path = savePath;
    for (const std::string &peer : cfg.peers) {
        size_t colon = peer.rfind(':');
        if (colon == std::string::npos) {
            continue;
        }
        lt::error_code aec;
        lt::address a = lt::make_address(peer.substr(0, colon), aec);
        if (!aec) {
            atp.peers.emplace_back(a, static_cast<uint16_t>(std::stoi(peer.substr(colon + 1))));
        }
    }

    lt::torrent_handle h = ses->add_torrent(std::move(atp), ec);
    if (ec) {
        log("add_torrent failed: %s", ec.message().c_str());
        while (platformRunning()) {
        }
        platformExit();
        return 1;
    }
    if (h.torrent_file()) {
        onMetadata(h);
    }

    std::atomic<int> dhtNodes{0};
    HttpServer server;
    const int64_t readahead = cfg.readahead;
    bool httpOk = server.start(cfg.httpPort, [&](int fd, const HttpRequest &req) {
        if (req.path == "/stream" || req.path.rfind("/stream/", 0) == 0) {
            serveStream(fd, req, readahead);
        } else if (req.path == "/" || req.path == "/status") {
            HttpServer::sendText(fd, 200, "application/json", statusJson(*ses, h, dhtNodes));
        } else {
            HttpServer::sendText(fd, 404, "text/plain", "not found\n");
        }
    });
    std::string url = "http://" + localAddress() + ":" + std::to_string(cfg.httpPort) + "/stream";
    if (httpOk) {
        log("http: %s (status: /status)", url.c_str());
        platformNotify("p2spike: " + url);
    } else {
        log("http: cannot listen on port %d", cfg.httpPort);
    }

    std::thread alertThread([&] {
        while (!g_stopping) {
            if (!ses->wait_for_alert(lt::milliseconds(100))) {
                continue;
            }
            std::vector<lt::alert *> alerts;
            ses->pop_alerts(&alerts);
            for (lt::alert *a : alerts) {
                if (auto *rp = lt::alert_cast<lt::read_piece_alert>(a)) {
                    g_reader.onAlert(rp);
                } else if (lt::alert_cast<lt::metadata_received_alert>(a)) {
                    onMetadata(h);
                } else if (auto *ds = lt::alert_cast<lt::dht_stats_alert>(a)) {
                    int nodes = 0;
                    for (const auto &bucket : ds->routing_table) {
                        nodes += bucket.num_nodes;
                    }
                    dhtNodes = nodes;
                } else if (lt::alert_cast<lt::listen_succeeded_alert>(a) || lt::alert_cast<lt::listen_failed_alert>(a) ||
                           lt::alert_cast<lt::dht_bootstrap_alert>(a) || lt::alert_cast<lt::torrent_error_alert>(a) ||
                           lt::alert_cast<lt::file_error_alert>(a) || lt::alert_cast<lt::metadata_failed_alert>(a) ||
                           lt::alert_cast<lt::dht_error_alert>(a) || lt::alert_cast<lt::udp_error_alert>(a)) {
                    log("%s", a->message().c_str());
                }
            }
        }
    });

    auto lastStatus = Clock::now();
    while (platformRunning()) {
        if (Clock::now() - lastStatus > std::chrono::seconds(2)) {
            lastStatus = Clock::now();
            ses->post_dht_stats();
            lt::torrent_status st = h.status();
            log("%s %.1f%% peers %d (seeds %d) down %d KB/s up %d KB/s dht %d clients %d",
                stateName(st.state), st.progress * 100.0, st.num_peers, st.num_seeds,
                st.download_rate / 1024, st.upload_rate / 1024, dhtNodes.load(), g_clients.load());
        }
    }

    log("exiting");
    g_stopping = true;
    g_reader.wake();
    server.stop();
    alertThread.join();
    ses.reset();
    if (g_logFile) {
        std::fclose(g_logFile);
    }
    platformExit();
    return 0;
}
